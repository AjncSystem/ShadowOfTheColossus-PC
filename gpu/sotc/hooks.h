// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: engine hooks (docs/HOOK-DESIGN.md). Declarations come from out/hooks.bin (SOTCHOOK1,
// written by scripts/sotc_hooks.py); each hook is resolved by signature, installed as a guest
// detour (Core::InstallGuestDetour, cpu_patches.h) and dispatched to a host handler registered
// by name. Every hook is off unless its switch SOTC_HOOK_<NAME> is set; SOTC_HOOKS=0 turns all
// hooks off. A hook whose pattern, group or dependency fails is off, never guessed.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace SotcHooks {

/// The guest's registers at the hook, as the detour stub saved them. Handlers may change them:
/// Entry/Site write them back before the guest continues; Wrap's post phase sees the return
/// values (rax, rdx, xmm0) and may change them.
struct alignas(16) GuestRegs {
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rsp;      ///< guest rsp at the hooked instruction (Entry/Wrap: points at the return address)
    uint64_t rflags;
    uint64_t pad;
    uint64_t xmm[8][2]; ///< xmm0-7
};
static_assert(sizeof(GuestRegs) == 18 * 8 + 8 * 16);

enum class Phase : uint32_t { Pre = 0, Post = 1 };
enum class HookAction : uint32_t {
    Continue = 0, ///< run the original (Entry/Wrap) or the displaced instructions (Site)
    Return = 1,   ///< Entry only: return to the guest caller with regs.rax/rdx/xmm0
    /// Pre phase only (Entry, Wrap, Site): the handler must have called SetGuestCall. The stub
    /// restores the registers (with the handler's changes), calls the guest function on the guest
    /// stack with the SysV ABI (args[0..5] in rdi, rsi, rdx, rcx, r8, r9; rsp 16-byte aligned at the
    /// call; the 128-byte red zone below the guest rsp is skipped), discards its return value,
    /// restores every guest register (GPRs, rflags, xmm0-15) and continues as for Continue (Wrap:
    /// the original runs next). Without a pending SetGuestCall, or outside the pre phase, the
    /// dispatcher turns it into Continue. Not preserved: MXCSR and the x87 state.
    CallGuest = 2,
};

/// A handler runs on the host stack (runtime_call_on_host_stack), on the guest thread.
/// It must not block on the GPU threads unless that is its purpose, and must not throw.
using PreHandler = HookAction (*)(GuestRegs& regs, void* user);
using PostHandler = void (*)(GuestRegs& regs, void* user);

struct Handler {
    PreHandler pre{};
    PostHandler post{};  ///< Wrap only
    void* user{};
};

/// Registers the handler for a hook name of scripts/sotc_hooks.py. Called from
/// RegisterAllHandlers() (hook_handlers.cpp), before InstallHooks.
void RegisterHandler(std::string_view name, const Handler& handler);

/// Calls every probe's registration function (gpu/sotc/hook_handlers.cpp). Explicit, because
/// unreferenced objects of the static GPU library are not linked.
void RegisterAllHandlers();

struct InstallStats {
    uint64_t installed{}, off{};
};

/// Parses hooks.bin, resolves each enabled hook (expected address, else a unique scan of the
/// image's code segments), applies dependencies and groups, installs the detours and prints
/// one manifest line per hook: "SOTC HOOK <name> 0x<offset> expected|scanned|off(<reason>)".
/// Called by sotc_patch_eboot after every other CPU patch. image = the mapped eboot.
bool InstallHooks(const char* hooks_path, void* image, uint64_t module_size, InstallStats* stats);

/// The mapped eboot (image offsets are relative to it); 0 before InstallHooks.
uintptr_t ImageBase();

/// Whether the named hook resolved and is installed (handlers may depend on other hooks).
bool IsInstalled(std::string_view name);

/// The current value of SOTC_HOOK_<NAME> for a hook (0 unset), read once at install.
int HookMode(std::string_view hook);

/// A derived field of a resolved hook (see the format below): imm/disp give the value read
/// from the matched instruction bytes, rip_target gives an image offset. False when the hook
/// did not resolve or has no such field.
bool Derived(std::string_view hook, std::string_view field, uint64_t* value);

/// A resolved Range hook (mode 3): [begin, end) image offsets. False when not resolved.
bool ResolvedRange(std::string_view hook, uint64_t* begin, uint64_t* end);

/// For a pre-phase handler returning HookAction::CallGuest: the guest function fn and its six
/// integer arguments, for the calling thread only. The call runs when the handler returns; the
/// pending call is cleared by that dispatch (a call set but not returned is dropped).
void SetGuestCall(uint64_t fn, const uint64_t args[6]);

// out/hooks.bin, format SOTCHOOK1 (little endian), written by scripts/sotc_hooks.py:
//   header: char magic[8] = "SOTCHOOK"; u32 version = 1; u32 count;
//           u8 fingerprint[32]   (SHA-256 of the eboot's PT_LOAD + PT_SCE_RELRO segments, as
//                                 tools/re/eboot_re.py / docs/HOOK-DESIGN.md compute it)
//           u32 fingerprint_known (1: the declarations' expected offsets are for this eboot)
//           u32 reserved
//   count records, each:
//     char name[48]; char switch_name[64]; char group[32];   (NUL-padded ASCII)
//     u32 mode;           0 Entry, 1 Wrap, 2 Site, 3 Range (no detour, resolve only)
//     u32 pattern_len;    bytes of pattern and of mask that follow
//     u64 expected;       image offset for this fingerprint, 0 = unknown (scan)
//     u64 range_len;      Range: bytes from the match covered by the range, else 0
//     u32 anchor_kind;    0 none, 1 inside the function containing hook anchor_index
//     u32 anchor_index;
//     u32 dep_count;      u32 deps[dep_count] (record indices that must resolve too)
//     u32 derived_count;  then derived_count entries of:
//                            char field[32]; u32 offset (into the pattern); u32 size (1/2/4/8);
//                            u32 kind (0 imm, 1 disp, 2 rip_target: offset of the next
//                            instruction is offset+size unless the next u32 says otherwise);
//                            u32 insn_end (pattern offset of the end of that instruction)
//     u8 pattern[pattern_len]; u8 mask[pattern_len] (0xff compare, 0x00 wildcard)
//   A hook is enabled when its switch is set to a non-zero value. Groups are all or nothing.

} // namespace SotcHooks

namespace SotcHookStats {
/// Appends the hook counters of the last report window to out and resets them (driver.cpp 5 s
/// report; printed with SOTC_FPS even under SOTC_FPS_LEAN). seconds = window length.
void Report(std::string& out, double seconds);
} // namespace SotcHookStats

extern "C" {
/// Entered from the detour stubs (Win64 ABI). Switches to the host stack, runs the handler of
/// hook_id for the phase, returns the HookAction as uint32_t.
uint32_t sotc_hook_dispatch(SotcHooks::GuestRegs* regs, uint32_t hook_id, uint32_t phase);
/// Called by the detour stub after sotc_hook_dispatch returned CallGuest, on the guest stack
/// (no host stack switch). Copies the pending guest call into out[0] = fn, out[1..6] = args and
/// clears it. Returns 1 when a call was pending, 0 otherwise (the stub then continues).
uint32_t sotc_hook_take_guest_call(uint64_t out[7]);
/// src/probe.c: runs fn(arg) on the thread's host stack (the fault handler's alternate stack
/// when the guest runs on a fiber stack); calls it directly when already on the host stack.
void runtime_call_on_host_stack(void (*fn)(void*), void* arg);
}
