// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <span>

#include "common/types.h"

// ============================================================================
// Windows static guest red-zone protection
// ============================================================================

enum class WindowsGuestRedZoneProtectionMode : u32 {
    Disabled,
    StaticPatching,
};

namespace Core::WindowsGuestRedZoneProtection {

void SetActiveMode(WindowsGuestRedZoneProtectionMode mode) noexcept;
WindowsGuestRedZoneProtectionMode GetActiveMode() noexcept;
bool IsStaticPatchingEnabled() noexcept;

} // namespace Core::WindowsGuestRedZoneProtection

// ============================================================================
// End Windows static guest red-zone protection
// ============================================================================

namespace Core {

// Windows static guest red-zone protection
struct RedZonePatchResult {
    u64 function_count{};
    u64 instruction_count{};
    u64 red_zone_function_count{};
    u64 memory_instruction_count{};
    u64 short_memory_instruction_count{};
    u64 patched_memory_instruction_count{};
    u64 stack_dependent_memory_instruction_count{};
    u64 control_flow_memory_instruction_count{};
    u64 unrelocatable_memory_instruction_count{};
    u64 indirect_red_zone_function_count{};
    u64 cpu_patch_instruction_count{};
    u64 patched_cpu_patch_instruction_count{};
    u64 unsupported_cpu_patch_instruction_count{};
};

/// Registers a module for patching, providing an area to generate trampoline code.
void RegisterPatchModule(void* module_ptr, u64 module_size, void* trampoline_area_ptr,
                         u64 trampoline_area_size);

/// Applies CPU patches that need to be done before beginning executions.
void PrePatchInstructions(u64 segment_addr, u64 segment_size);

// Windows static guest red-zone protection
/// Keeps Windows exception dispatch outside live guest red zones at faultable memory accesses.
/// When selected_addresses is not empty only functions containing one of them are patched.
RedZonePatchResult PatchRedZoneMemoryInstructions(u64 segment_addr, u64 segment_size,
                                                  std::span<const uintptr_t> function_starts,
                                                  std::span<const uintptr_t> selected_addresses = {});

/// Returns true when the host CPU lacks SSE4a and EXTRQ/INSERTQ must be emulated.
bool NeedsSse4aEmulation();

/// Relocates 4-byte EXTRQ/INSERTQ (too short for a near jump) together with the following
/// instructions into trampolines, so they never reach the illegal-instruction handler. On Windows
/// exception dispatch writes below RSP and clobbers the guest red zone, which corrupts leaf
/// functions that keep temporaries there. Returns the number of instructions patched.
u64 PatchShortSse4aInstructions(u64 segment_addr, u64 segment_size,
                                std::span<const uintptr_t> function_starts);

/// sotc: points every rdtsc at `clock` (a host function returning the TSC replacement), for
/// systems whose cores disagree on the TSC. Returns the number of sites patched; `unpatched`
/// counts the sites that could not be (they keep reading the host TSC). Windows only.
u64 PatchRdtscInstructions(u64 segment_addr, u64 segment_size,
                           std::span<const uintptr_t> function_starts, void* clock,
                           u64* unpatched);

/// sotc: a detour from guest code into the host hook dispatcher (gpu/sotc/hooks.h,
/// docs/HOOK-DESIGN.md). The stub follows the rdtsc clock call (GenerateClockCall): it steps over
/// the 128-byte red zone, saves flags, every GPR (as a GuestRegs) and xmm0-7, aligns the stack,
/// reserves the Win64 shadow space and calls sotc_hook_dispatch(regs, hook_id, phase).
enum class DetourMode : u32 {
    /// At a function entry. The pre-handler may return HookAction::Return: the stub then returns
    /// to the guest caller with regs.rax/rdx/xmm0 as the result, skipping the original.
    Entry,
    /// At a function entry, pre and post. The original runs as a call from the stub (one extra
    /// frame on the guest stack); the post-handler sees and may change the return values. Only
    /// for functions without stack arguments and without varargs.
    Wrap,
    /// At one instruction inside a function. The handler may edit the registers, which are
    /// written back before the displaced instructions run.
    Site,
};

struct DetourRequest {
    u64 address{};                  ///< host address of the hooked instruction (image + offset)
    DetourMode mode{DetourMode::Entry};
    u32 hook_id{};                  ///< passed to sotc_hook_dispatch
    std::span<const u8> pattern;    ///< bytes expected at address after all other patches
    std::span<const u8> mask;       ///< same length: 0xff compare, 0x00 wildcard (empty: all 0xff)
};

struct DetourResult {
    bool installed{};
    const char* reason{""};         ///< why not installed (static string)
    u64 displaced_bytes{};          ///< guest bytes moved into the trampoline
};

/// Installs one detour into the module registered with RegisterPatchModule. Refuses (installed =
/// false) when the pattern does not match, the displaced span (>= 5 bytes) holds a branch target
/// after its first instruction or control flow before its end, overlaps another CPU patch, lies in
/// a red-zone protected function, or the trampoline area is full. Nothing is written on refusal.
bool InstallGuestDetour(const DetourRequest& request, DetourResult* result);

/// Trampoline bytes used so far by every CPU patch of the registered module.
u64 TrampolineBytesUsed();

/// sotc: gives the registered module its function starts (host addresses) for the guest detours,
/// without patching anything. The other passes merge theirs too, but they may not run.
void RegisterFunctionStarts(std::span<const uintptr_t> function_starts);

} // namespace Core
