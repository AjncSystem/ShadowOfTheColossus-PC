// sotc: hook-detour-test (gpu/CMakeLists.txt, EXCLUDE_FROM_ALL). Checks the guest detour stubs of
// Core::InstallGuestDetour (gpu/sotc/cpu_patches.cpp, docs/HOOK-DESIGN.md) on hand-assembled SysV
// functions in a fake module: Entry with a RIP-relative prologue, Entry Return, Wrap (pre and
// post, Return ignored), Site with register write-back, a red-zone sentinel and live registers and
// flags, the host call alignment, HookAction::CallGuest (Entry and Wrap: a guest callee with six
// arguments that clobbers the GPRs, flags and xmm0/8/15, while the caller keeps xmm8 live), and the
// refusals (Wrap stack reads, branch to the entry, the trampoline rollback). Windows only. Prints
// PASS or FAIL; the exit code is 0 on PASS.

#include <windows.h>
#include <intrin.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <vector>

#include "cpu_patches.h"
#include "hooks.h"

namespace {

int g_failures = 0;

#define EXPECT(cond)                                            \
    do {                                                        \
        if (!(cond)) {                                          \
            ++g_failures;                                       \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond); \
        }                                                       \
    } while (0)

// Hook ids passed to sotc_hook_dispatch.
constexpr uint32_t kHookEntryRip = 1;
constexpr uint32_t kHookWrap = 2;
constexpr uint32_t kHookSite = 3;
constexpr uint32_t kHookSiteRegs = 6;
constexpr uint32_t kHookCallEntry = 7;

// State written by the test dispatcher and read by the checks.
int g_entry_pre = 0;
int g_wrap_pre = 0;
int g_wrap_post = 0;
int g_site_pre = 0;
int g_site_regs_pre = 0;
int g_host_misaligned = 0;
int g_guest_misaligned = 0;
int g_wrap_rsp_changed = 0;
bool g_entry_return = false;   // Entry handler answers HookAction::Return
bool g_wrap_pre_return = false; // Wrap pre handler answers HookAction::Return (must be ignored)
bool g_wrap_post_adds = false; // Wrap post handler adds 1000 to rax
bool g_site_edit = false;      // Site handler sets rsi = 100
uint64_t g_wrap_entry_rsp = 0;
uint64_t g_wrap_post_rax = 0;
int g_call_entry_pre = 0;
int g_call_entry_misaligned = 0;
bool g_call_entry_enabled = false; // Entry handler answers HookAction::CallGuest
bool g_wrap_call_enabled = false;  // Wrap pre handler answers HookAction::CallGuest

// The guest call set by the test handlers (stands in for SotcHooks::SetGuestCall).
struct TestGuestCall {
    bool set = false;
    uint64_t words[7] = {}; // fn, then six arguments
};
TestGuestCall g_test_call;

// Offsets of the fake functions inside the module, and of the data word they load.
constexpr uint64_t kCodeSize = 0x2000;
constexpr uint64_t kTrampolineSize = 0x2000;
constexpr uint64_t kOffRip = 0x00;        // mov eax,[rip+data]; add eax,edi; ret
constexpr uint64_t kOffWrap = 0x10;       // mov eax,edi; add eax,esi; add eax,1; ret
constexpr uint64_t kOffSite = 0x20;       // leaf: red-zone sentinel, then the Site hook
constexpr uint64_t kOffSiteAt = 0x2A;     // add eax,esi inside kOffSite
constexpr uint64_t kOffBranch = 0x50;     // mov eax,edi; T: add eax,esi; add eax,1; jne T; ret
constexpr uint64_t kOffRegs = 0x100;      // Site leaf with live registers and flags (0xEF bytes)
constexpr uint64_t kOffRegsSite = 0x184;  // mov r10d,0x12345678 inside kOffRegs: the Site hook point
constexpr uint64_t kOffWrapArg = 0x200;   // mov rax,[rsp+8]; ret (Wrap refused)
constexpr uint64_t kOffWrapFrame = 0x240; // push rbp; mov rbp,rsp; mov rax,[rbp+8]; pop rbp; ret
constexpr uint64_t kOffLoopEntry = 0x280; // T: add eax,esi; add eax,-1; jne T; ret (Entry refused)
constexpr uint64_t kOffWrapRet = 0x2C0;   // mov eax,edi; add eax,esi; add eax,1; ret (Wrap)
constexpr uint64_t kOffCallee = 0x300;    // guest callee: stores its args, clobbers, returns 0x999
constexpr uint64_t kOffDriver = 0x400;    // holds xmm8 live, calls kOffCallHook, stores xmm8
constexpr uint64_t kOffCallHook = 0x440;  // mov eax,edi; add eax,esi; add eax,1; ret (Entry CallGuest)
constexpr uint64_t kOffCallData = 0xA00;  // callee results: 6 args, rsp&15, then the xmm8 word
constexpr uint64_t kOffData = 0x800;

typedef int(__attribute__((sysv_abi)) * Fn1)(int);
typedef int(__attribute__((sysv_abi)) * Fn2)(int, int);

void Put(uint8_t* base, uint64_t offset, std::initializer_list<uint8_t> bytes) {
    uint64_t i = 0;
    for (const uint8_t byte : bytes) {
        base[offset + i++] = byte;
    }
}

// Emits the opcode bytes in head, then a disp32 that addresses target (the disp32 ends the
// instruction). at advances past it.
void PutRipRelative(uint8_t* base, uint64_t& at, std::initializer_list<uint8_t> head,
                    uint64_t target) {
    for (const uint8_t byte : head) {
        base[at++] = byte;
    }
    const auto disp = static_cast<int32_t>(static_cast<int64_t>(target) - static_cast<int64_t>(at + 4));
    std::memcpy(base + at, &disp, sizeof(disp));
    at += 4;
}

// Host work the Site handler runs: a big stack array, floating point, and integer flags left
// different from the guest's. The stub must hide all of it from the guest.
__attribute__((noinline)) double ClobberHostState(uint32_t seed) {
    char scratch[4096];
    std::memset(scratch, static_cast<int>(seed & 0xFF), sizeof(scratch));
    double acc = 0.0;
    for (int i = 0; i < 64; ++i) {
        acc += static_cast<double>(scratch[i * 64]) * 0.5;
    }
    // cmp 0,1: CF=1, SF=1, ZF=0, OF=0 (the guest's jl flags are different).
    __asm__ volatile("xor %%r11d, %%r11d\n\tcmp $1, %%r11d" : : : "r11", "cc");
    return acc;
}

} // namespace

// Test side of SotcHooks::SetGuestCall and of the stub's take helper (hooks.h).
uintptr_t g_callee_addr = 0;
void TestSetGuestCall(uint64_t fn, const uint64_t (&args)[6]) {
    g_test_call.words[0] = fn;
    std::memcpy(&g_test_call.words[1], args, sizeof(args));
    g_test_call.set = true;
}

// The stub calls this with the Win64 ABI. The return address sits just above the saved frame
// pointer (frame + 8), and the host call was 16-byte aligned when that slot is 8 mod 16.
extern "C" uint32_t sotc_hook_dispatch(SotcHooks::GuestRegs* regs, uint32_t hook_id, uint32_t phase) {
    if ((reinterpret_cast<uintptr_t>(static_cast<char*>(__builtin_frame_address(0)) + 8) & 15) != 8) {
        ++g_host_misaligned;
    }
    const auto action_continue = static_cast<uint32_t>(SotcHooks::HookAction::Continue);
    const auto action_return = static_cast<uint32_t>(SotcHooks::HookAction::Return);
    const auto action_call = static_cast<uint32_t>(SotcHooks::HookAction::CallGuest);
    g_test_call.set = false;
    switch (hook_id) {
    case kHookEntryRip:
        // The guest entered with rsp = 8 mod 16 (its return address is at rsp).
        if ((regs->rsp & 15) != 8) {
            ++g_guest_misaligned;
        }
        ++g_entry_pre;
        if (g_entry_return) {
            regs->rax = 777;
            return action_return;
        }
        return action_continue;
    case kHookCallEntry:
        if ((regs->rsp & 15) != 8) {
            ++g_call_entry_misaligned;
        }
        ++g_call_entry_pre;
        if (g_call_entry_enabled) {
            const uint64_t args[6] = {11, 22, 33, 44, 55, 66};
            TestSetGuestCall(g_callee_addr, args);
            return action_call;
        }
        return action_continue;
    case kHookWrap:
        if (phase == static_cast<uint32_t>(SotcHooks::Phase::Pre)) {
            ++g_wrap_pre;
            g_wrap_entry_rsp = regs->rsp;
            if (g_wrap_pre_return) {
                regs->rax = 777; // Wrap ignores Return: the original overwrites rax
                return action_return;
            }
            if (g_wrap_call_enabled) {
                const uint64_t args[6] = {1, 2, 3, 4, 5, 6};
                TestSetGuestCall(g_callee_addr, args);
                return action_call;
            }
            return action_continue;
        }
        ++g_wrap_post;
        if (regs->rsp != g_wrap_entry_rsp) {
            ++g_wrap_rsp_changed;
        }
        g_wrap_post_rax = regs->rax;
        if (g_wrap_post_adds) {
            regs->rax += 1000;
        }
        return action_continue;
    case kHookSite:
        ++g_site_pre;
        if (g_site_edit) {
            regs->rsi = 100; // the displaced add eax,esi then adds 100
        }
        return action_continue;
    case kHookSiteRegs:
        // Leaves *regs alone: the host code clobbers the real registers and flags instead.
        ++g_site_regs_pre;
        static_cast<void>(ClobberHostState(0x5A));
        return action_continue;
    default:
        return action_continue;
    }
}

// The detour stub calls this on the guest stack after a CallGuest (hooks.h).
extern "C" uint32_t sotc_hook_take_guest_call(uint64_t out[7]) {
    if (!g_test_call.set) {
        return 0;
    }
    std::memcpy(out, g_test_call.words, sizeof(g_test_call.words));
    g_test_call.set = false;
    return 1;
}

int main() {
    void* memory = VirtualAlloc(nullptr, kCodeSize + kTrampolineSize, MEM_RESERVE | MEM_COMMIT,
                                PAGE_EXECUTE_READWRITE);
    if (memory == nullptr) {
        std::printf("FAIL hook-detour-test: VirtualAlloc\n");
        return 1;
    }
    auto* base = static_cast<uint8_t*>(memory);
    const auto addr = [base](uint64_t offset) {
        return reinterpret_cast<uintptr_t>(base) + offset;
    };

    // mov eax,[rip+disp32]; add eax,edi; ret. The displacement points at the data word.
    Put(base, kOffRip, {0x8B, 0x05, 0x00, 0x00, 0x00, 0x00, 0x01, 0xF8, 0xC3});
    const int32_t rip_disp = static_cast<int32_t>(kOffData - (kOffRip + 6));
    std::memcpy(base + kOffRip + 2, &rip_disp, sizeof(rip_disp));
    const int32_t data_value = 1000;
    std::memcpy(base + kOffData, &data_value, sizeof(data_value));

    // mov eax,edi; add eax,esi; add eax,1; ret
    Put(base, kOffWrap, {0x89, 0xF8, 0x01, 0xF0, 0x83, 0xC0, 0x01, 0xC3});

    // Site leaf: stores a sentinel below rsp (red zone), runs the hook at kOffSiteAt, checks the
    // sentinel, and returns 0xDEAD if the sentinel was clobbered.
    Put(base, kOffSite,
        {0xC7, 0x44, 0x24, 0xF8, 0x5A, 0x5A, 0x5A, 0x5A, // mov dword [rsp-8], 0x5A5A5A5A
         0x89, 0xF8,                                     // mov eax,edi
         0x01, 0xF0,                                     // add eax,esi        <- kOffSiteAt
         0x05, 0x00, 0x10, 0x00, 0x00,                   // add eax,0x1000
         0x81, 0x7C, 0x24, 0xF8, 0x5A, 0x5A, 0x5A, 0x5A, // cmp dword [rsp-8], 0x5A5A5A5A
         0x75, 0x01,                                     // jne bad (+1)
         0xC3,                                           // ret
         0xB8, 0xAD, 0xDE, 0x00, 0x00,                   // bad: mov eax,0xDEAD
         0xC3});                                         // ret

    // A branch back into its own span: the Entry detour must refuse it.
    Put(base, kOffBranch, {0x89, 0xF8, 0x01, 0xF0, 0x83, 0xC0, 0x01, 0x75, 0xF9, 0xC3});

    // Site leaf with live registers and flags. Pushes rbx and rbp, loads known values into rbx,
    // rbp, rcx, rdx, r8, r9, r11 and xmm0-7, compares esi with edi and runs the Site hook on the
    // mov r10d (flags stay live across it). The jl picks 2000 or 1000 and the epilogue folds
    // every register into eax. r10 is set by the displaced mov, so it is folded after the hook.
    Put(base, kOffRegs,
        {0x53, 0x55,                                 // push rbx; push rbp
         0xBB, 0x44, 0x33, 0x22, 0x11,               // mov ebx,0x11223344
         0xBD, 0x88, 0x77, 0x66, 0x55,               // mov ebp,0x55667788
         0xB9, 0x0C, 0x0C, 0x0C, 0x0C,               // mov ecx,0x0C0C0C0C
         0xBA, 0x0D, 0x0D, 0x0D, 0x0D});             // mov edx,0x0D0D0D0D
    // xmm0-7 = 0x01010101 * (n + 1), through r11d (movd xmmN, r11d).
    constexpr std::array<uint8_t, 8> kXmmRegModrm{0xC3, 0xCB, 0xD3, 0xDB, 0xE3, 0xEB, 0xF3, 0xFB};
    uint64_t at = kOffRegs + 0x16;
    for (uint8_t n = 0; n < 8; ++n) {
        const uint8_t v = static_cast<uint8_t>(n + 1);
        Put(base, at, {0x41, 0xBB, v, v, v, v,       // mov r11d,0x0V0V0V0V
                       0x66, 0x41, 0x0F, 0x6E, kXmmRegModrm[n]}); // movd xmmN,r11d
        at += 11;
    }
    Put(base, kOffRegs + 0x6E,
        {0x41, 0xB8, 0x21, 0x21, 0x21, 0x21,         // mov r8d,0x21212121
         0x41, 0xB9, 0x31, 0x31, 0x31, 0x31,         // mov r9d,0x31313131
         0x41, 0xBB, 0x41, 0x41, 0x41, 0x41,         // mov r11d,0x41414141
         0x89, 0xF0,                                 // mov eax,esi
         0x39, 0xF8,                                 // cmp eax,edi (flags live to the jl)
         0x41, 0xBA, 0x78, 0x56, 0x34, 0x12,         // mov r10d,0x12345678 <- kOffRegsSite
         0x7C, 0x07,                                 // jl less (+7)
         0xB8, 0xE8, 0x03, 0x00, 0x00,               // mov eax,1000
         0xEB, 0x05,                                 // jmp fold (+5)
         0xB8, 0xD0, 0x07, 0x00, 0x00});             // less: mov eax,2000
    Put(base, kOffRegs + 0x98,
        {0x01, 0xC8,                                 // fold: add eax,ecx
         0x01, 0xD0,                                 // add eax,edx
         0x44, 0x01, 0xC0,                           // add eax,r8d
         0x44, 0x01, 0xC8,                           // add eax,r9d
         0x44, 0x01, 0xD0,                           // add eax,r10d
         0x44, 0x01, 0xD8,                           // add eax,r11d
         0x01, 0xD8,                                 // add eax,ebx
         0x01, 0xE8});                               // add eax,ebp
    at = kOffRegs + 0xAC;
    for (uint8_t n = 0; n < 8; ++n) {
        Put(base, at, {0x66, 0x41, 0x0F, 0x7E, kXmmRegModrm[n],  // movd r11d,xmmN
                       0x44, 0x01, 0xD8});                       // add eax,r11d
        at += 8;
    }
    Put(base, at, {0x5D, 0x5B, 0xC3}); // pop rbp; pop rbx; ret

    // Wrap refusal bodies (see the offsets above) and the Entry loop.
    Put(base, kOffWrapArg, {0x48, 0x8B, 0x44, 0x24, 0x08, 0xC3});
    Put(base, kOffWrapFrame, {0x55, 0x48, 0x89, 0xE5, 0x48, 0x8B, 0x45, 0x08, 0x5D, 0xC3});
    Put(base, kOffLoopEntry, {0x01, 0xF0, 0x83, 0xC0, 0xFF, 0x75, 0xF9, 0xC3});
    Put(base, kOffWrapRet, {0x89, 0xF8, 0x01, 0xF0, 0x83, 0xC0, 0x01, 0xC3});

    // Guest callee for CallGuest: stores rdi, rsi, rdx, rcx, r8, r9 and rsp & 15 into the data
    // area, then clobbers every argument register, flags, and xmm0/8/15, and returns 0x999.
    const auto emit = [base](uint64_t& at, std::initializer_list<uint8_t> bytes) {
        Put(base, at, bytes);
        at += bytes.size();
    };
    {
        uint64_t at = kOffCallee;
        PutRipRelative(base, at, {0x48, 0x89, 0x3D}, kOffCallData + 0);  // mov [rip],rdi
        PutRipRelative(base, at, {0x48, 0x89, 0x35}, kOffCallData + 8);  // mov [rip],rsi
        PutRipRelative(base, at, {0x48, 0x89, 0x15}, kOffCallData + 16); // mov [rip],rdx
        PutRipRelative(base, at, {0x48, 0x89, 0x0D}, kOffCallData + 24); // mov [rip],rcx
        PutRipRelative(base, at, {0x4C, 0x89, 0x05}, kOffCallData + 32); // mov [rip],r8
        PutRipRelative(base, at, {0x4C, 0x89, 0x0D}, kOffCallData + 40); // mov [rip],r9
        emit(at, {0x48, 0x89, 0xE0, 0x83, 0xE0, 0x0F});                 // mov rax,rsp; and eax,15
        PutRipRelative(base, at, {0x48, 0x89, 0x05}, kOffCallData + 48); // mov [rip],rax
        emit(at, {0xBF, 0xAD, 0x0B, 0x00, 0x00});                       // mov edi,0xBAD
        emit(at, {0xBE, 0xAD, 0x0B, 0x00, 0x00});                       // mov esi,0xBAD
        emit(at, {0xBA, 0xAD, 0x0B, 0x00, 0x00});                       // mov edx,0xBAD
        emit(at, {0xB9, 0xAD, 0x0B, 0x00, 0x00});                       // mov ecx,0xBAD
        emit(at, {0x41, 0xB8, 0xAD, 0x0B, 0x00, 0x00});                 // mov r8d,0xBAD
        emit(at, {0x41, 0xB9, 0xAD, 0x0B, 0x00, 0x00});                 // mov r9d,0xBAD
        emit(at, {0x41, 0xBA, 0xAD, 0x0B, 0x00, 0x00});                 // mov r10d,0xBAD
        emit(at, {0x41, 0xBB, 0xAD, 0x0B, 0x00, 0x00});                 // mov r11d,0xBAD
        emit(at, {0xB8, 0xAD, 0x0B, 0x00, 0x00});                       // mov eax,0xBAD
        emit(at, {0x66, 0x0F, 0x6E, 0xC0});                             // movd xmm0,eax
        emit(at, {0x66, 0x44, 0x0F, 0x6E, 0xC0});                       // movd xmm8,eax
        emit(at, {0x66, 0x44, 0x0F, 0x6E, 0xF8});                       // movd xmm15,eax
        emit(at, {0x83, 0xF8, 0x01});                                   // cmp eax,1 (flags)
        emit(at, {0xB8, 0x99, 0x09, 0x00, 0x00, 0xC3});                 // mov eax,0x999; ret
    }
    // Driver: sub rsp,8; holds 0x5A5A in xmm8 across a call to kOffCallHook; stores xmm8; ret.
    {
        uint64_t at = kOffDriver;
        emit(at, {0x48, 0x83, 0xEC, 0x08});       // sub rsp,8 (call sees rsp = 8 mod 16)
        emit(at, {0xB8, 0x5A, 0x5A, 0x00, 0x00}); // mov eax,0x5A5A
        emit(at, {0x66, 0x44, 0x0F, 0x6E, 0xC0}); // movd xmm8,eax
        emit(at, {0xE8});                         // call kOffCallHook
        const auto disp = static_cast<int32_t>(static_cast<int64_t>(kOffCallHook) -
                                               static_cast<int64_t>(at + 4));
        std::memcpy(base + at, &disp, sizeof(disp));
        at += 4;
        PutRipRelative(base, at, {0x66, 0x44, 0x0F, 0xD6, 0x05}, kOffCallData + 64); // movq [rip],xmm8
        emit(at, {0x48, 0x83, 0xC4, 0x08});       // add rsp,8
        emit(at, {0xC3});                         // ret
    }
    // The Entry hook's body: mov eax,edi; add eax,esi; add eax,1; ret.
    Put(base, kOffCallHook, {0x89, 0xF8, 0x01, 0xF0, 0x83, 0xC0, 0x01, 0xC3});
    g_callee_addr = addr(kOffCallee);

    Core::RegisterPatchModule(base, kCodeSize, base + kCodeSize, kTrampolineSize);

    // Function starts reach the detour code through the red-zone pass. The empty selection
    // (address 0 is in no function) only records the starts; it protects nothing.
    const std::array<uintptr_t, 12> starts{addr(kOffRip),       addr(kOffWrap),      addr(kOffSite),
                                           addr(kOffBranch),    addr(kOffRegs),      addr(kOffWrapArg),
                                           addr(kOffWrapFrame), addr(kOffLoopEntry), addr(kOffWrapRet),
                                           addr(kOffCallee),    addr(kOffDriver),    addr(kOffCallHook)};
    const std::array<uintptr_t, 1> no_selection{0};
    Core::PatchRedZoneMemoryInstructions(reinterpret_cast<uintptr_t>(base), kCodeSize, starts,
                                         no_selection);

    const auto install = [&](uint64_t offset, Core::DetourMode mode, uint32_t hook_id,
                             const std::vector<uint8_t>& pattern, Core::DetourResult* out) {
        Core::DetourRequest request{};
        request.address = addr(offset);
        request.mode = mode;
        request.hook_id = hook_id;
        request.pattern = pattern;
        const bool ok = Core::InstallGuestDetour(request, out);
        if (!ok && out->reason != nullptr) {
            std::printf("  hook %u refused: %s\n", hook_id, out->reason);
        }
        return ok;
    };

    // A refused install writes nothing: the code bytes and the trampoline area stay as they were.
    // Returns the refusal reason.
    const auto expect_refused = [&](uint64_t offset, Core::DetourMode mode, uint32_t hook_id,
                                    const std::vector<uint8_t>& pattern, size_t length) -> const char* {
        const std::vector<uint8_t> before(base + offset, base + offset + length);
        const uint64_t trampoline_before = Core::TrampolineBytesUsed();
        Core::DetourResult refused{};
        EXPECT(!install(offset, mode, hook_id, pattern, &refused));
        EXPECT(!refused.installed && refused.reason[0] != '\0');
        EXPECT(Core::TrampolineBytesUsed() == trampoline_before);
        EXPECT(std::memcmp(before.data(), base + offset, before.size()) == 0);
        return refused.reason;
    };

    // Refusals that must write nothing.
    Core::DetourResult result{};
    expect_refused(kOffRip, Core::DetourMode::Entry, kHookEntryRip, {0x90, 0x90}, 2);

    // Entry on the RIP-relative prologue: the 6-byte load moves into the stub.
    const std::vector<uint8_t> rip_pattern{0x8B, 0x05, 0xFA, 0x07, 0x00, 0x00};
    EXPECT(install(kOffRip, Core::DetourMode::Entry, kHookEntryRip, rip_pattern, &result));
    EXPECT(result.installed && result.displaced_bytes == 6);

    // Wrap on the three-instruction body: the 7 displaced bytes run inside the original.
    const std::vector<uint8_t> wrap_pattern{0x89, 0xF8, 0x01, 0xF0, 0x83, 0xC0, 0x01};
    EXPECT(install(kOffWrap, Core::DetourMode::Wrap, kHookWrap, wrap_pattern, &result));
    EXPECT(result.installed && result.displaced_bytes == 7);

    // Site on add eax,esi inside the leaf: the 7 bytes of the two adds move into the stub.
    const std::vector<uint8_t> site_pattern{0x01, 0xF0, 0x05, 0x00, 0x10, 0x00, 0x00};
    EXPECT(install(kOffSiteAt, Core::DetourMode::Site, kHookSite, site_pattern, &result));
    EXPECT(result.installed && result.displaced_bytes == 7);

    // Refusals after the installs: a branch target inside the span, and a pattern that now sees
    // the jump written by the Wrap detour.
    const std::vector<uint8_t> branch_pattern{0x89, 0xF8, 0x01, 0xF0};
    expect_refused(kOffBranch, Core::DetourMode::Entry, 4, branch_pattern, 10);
    expect_refused(kOffWrap, Core::DetourMode::Entry, 5, wrap_pattern, 7);

    // Wrap refuses a function that reads its return address or its stack arguments, and the
    // detour is not written. The pattern is the whole body, so only the stack analysis refuses.
    const std::vector<uint8_t> wrap_arg_bytes{0x48, 0x8B, 0x44, 0x24, 0x08, 0xC3}; // mov rax,[rsp+8]; ret
    const char* arg_reason = expect_refused(kOffWrapArg, Core::DetourMode::Wrap, kHookWrap,
                                            wrap_arg_bytes, wrap_arg_bytes.size());
    EXPECT(std::strstr(arg_reason, "stack arguments") != nullptr);
    const std::vector<uint8_t> wrap_frame_bytes{0x55, 0x48, 0x89, 0xE5, 0x48, 0x8B,
                                                0x45, 0x08, 0x5D, 0xC3}; // frame-pointer read
    const char* frame_reason = expect_refused(kOffWrapFrame, Core::DetourMode::Wrap, kHookWrap,
                                              wrap_frame_bytes, wrap_frame_bytes.size());
    EXPECT(std::strstr(frame_reason, "stack arguments") != nullptr);

    // Entry refuses a branch back to the entry (a loop head is the first instruction).
    const std::vector<uint8_t> loop_pattern{0x01, 0xF0, 0x83, 0xC0, 0xFF};
    const char* loop_reason = expect_refused(kOffLoopEntry, Core::DetourMode::Entry, 6,
                                             loop_pattern, loop_pattern.size());
    EXPECT(std::strstr(loop_reason, "targets its entry") != nullptr);

    EXPECT(Core::TrampolineBytesUsed() > 0);

    // Entry: the load runs in the stub, the body runs after the detour.
    const auto rip_fn = reinterpret_cast<Fn1>(addr(kOffRip));
    EXPECT(rip_fn(5) == 1005);
    EXPECT(g_entry_pre == 1);

    // Entry with Return: the caller gets regs.rax, the original does not run.
    g_entry_return = true;
    EXPECT(rip_fn(5) == 777);
    g_entry_return = false;
    EXPECT(g_entry_pre == 2);

    // Wrap: pre and post run once, post sees the original result, and post may change it.
    const auto wrap_fn = reinterpret_cast<Fn2>(addr(kOffWrap));
    EXPECT(wrap_fn(3, 4) == 8);
    EXPECT(g_wrap_pre == 1 && g_wrap_post == 1 && g_wrap_post_rax == 8);
    g_wrap_post_adds = true;
    EXPECT(wrap_fn(3, 4) == 1008);
    g_wrap_post_adds = false;

    // Site: the leaf's sentinel survives the stub, and the handler's rsi write-back reaches the
    // displaced add (1 + 100 + 4096 instead of 1 + 2 + 4096).
    const auto site_fn = reinterpret_cast<Fn2>(addr(kOffSite));
    EXPECT(site_fn(1, 2) == 4099);
    EXPECT(g_site_pre == 1);
    g_site_edit = true;
    EXPECT(site_fn(1, 2) == 4197);
    g_site_edit = false;

    // Site with live registers and flags. The plain values come from the leaf before the hook is
    // installed (esi < edi takes the jl: 2000 + folded registers, else 1000 + folded registers).
    // The hand sums check the leaf's own bytes. With the hook, the handler clobbers the flags, the
    // GPRs and xmm registers, and the result must not change.
    const auto regs_fn = reinterpret_cast<Fn2>(addr(kOffRegs));
    const auto regs_taken = static_cast<uint32_t>(regs_fn(5, 2));
    const auto regs_fall = static_cast<uint32_t>(regs_fn(2, 5));
    EXPECT(regs_taken == 0x498DD9E4u);
    EXPECT(regs_fall == 0x498DD5FCu);
    const std::vector<uint8_t> regs_pattern{0x41, 0xBA, 0x78, 0x56, 0x34, 0x12}; // mov r10d,imm32
    EXPECT(install(kOffRegsSite, Core::DetourMode::Site, kHookSiteRegs, regs_pattern, &result));
    EXPECT(result.installed && result.displaced_bytes == 6);
    EXPECT(static_cast<uint32_t>(regs_fn(5, 2)) == regs_taken);
    EXPECT(static_cast<uint32_t>(regs_fn(2, 5)) == regs_fall);
    EXPECT(g_site_regs_pre == 2);

    // Wrap ignores HookAction::Return: the original still runs (8, not 777) and the post handler
    // sees the original result.
    const std::vector<uint8_t> wrap_ret_pattern{0x89, 0xF8, 0x01, 0xF0, 0x83, 0xC0, 0x01};
    EXPECT(install(kOffWrapRet, Core::DetourMode::Wrap, kHookWrap, wrap_ret_pattern, &result));
    EXPECT(result.installed && result.displaced_bytes == 7);
    const auto wrap_ret_fn = reinterpret_cast<Fn2>(addr(kOffWrapRet));
    const int wrap_ret_pre = g_wrap_pre;
    const int wrap_ret_post = g_wrap_post;
    g_wrap_pre_return = true;
    EXPECT(wrap_ret_fn(3, 4) == 8);
    g_wrap_pre_return = false;
    EXPECT(g_wrap_pre == wrap_ret_pre + 1 && g_wrap_post == wrap_ret_post + 1);
    EXPECT(g_wrap_post_rax == 8);

    // CallGuest on Entry. The handler asks for a six-argument call; the callee clobbers every
    // register and the flags, and the hooked function still returns 3 + 4 + 1 with the driver's
    // xmm8 intact. Without the call (handler off) the detour behaves as before.
    const std::vector<uint8_t> call_pattern{0x89, 0xF8, 0x01, 0xF0, 0x83, 0xC0, 0x01};
    EXPECT(install(kOffCallHook, Core::DetourMode::Entry, kHookCallEntry, call_pattern, &result));
    EXPECT(result.installed && result.displaced_bytes == 7);
    const auto call_hook_fn = reinterpret_cast<Fn2>(addr(kOffCallHook));
    const auto driver_fn = reinterpret_cast<Fn2>(addr(kOffDriver));
    const auto* call_data = reinterpret_cast<const uint64_t*>(base + kOffCallData);
    EXPECT(call_hook_fn(3, 4) == 8);
    EXPECT(g_call_entry_pre == 1);
    EXPECT(call_data[0] == 0); // the callee did not run
    g_call_entry_enabled = true;
    std::memset(base + kOffCallData, 0, 0x100);
    EXPECT(driver_fn(3, 4) == 8);
    EXPECT(call_data[0] == 11 && call_data[1] == 22 && call_data[2] == 33);
    EXPECT(call_data[3] == 44 && call_data[4] == 55 && call_data[5] == 66);
    EXPECT(call_data[6] == 8);       // rsp was 16-byte aligned at the guest call (8 after the push of the return address)
    EXPECT(call_data[8] == 0x5A5A);  // xmm8 survived the callee (which wrote 0xBAD)
    EXPECT(g_call_entry_pre == 2);   // the bare call above and the driver's call
    EXPECT(g_call_entry_misaligned == 0);
    g_call_entry_enabled = false;
    EXPECT(call_hook_fn(5, 6) == 12);

    // CallGuest on Wrap (pre phase): the callee runs before the original, the original and the
    // post handler still see the registers as they were.
    g_wrap_call_enabled = true;
    std::memset(base + kOffCallData, 0, 0x100);
    const int wrap_call_post = g_wrap_post;
    EXPECT(wrap_fn(3, 4) == 8);
    EXPECT(g_wrap_post == wrap_call_post + 1 && g_wrap_post_rax == 8);
    EXPECT(call_data[0] == 1 && call_data[5] == 6 && call_data[6] == 8);
    g_wrap_call_enabled = false;

    EXPECT(g_host_misaligned == 0);
    EXPECT(g_guest_misaligned == 0);
    EXPECT(g_wrap_rsp_changed == 0);

    VirtualFree(memory, 0, MEM_RELEASE);
    std::printf(g_failures == 0 ? "PASS hook-detour-test\n"
                                : "FAIL hook-detour-test: %d check(s) failed\n",
                g_failures);
    return g_failures == 0 ? 0 : 1;
}
