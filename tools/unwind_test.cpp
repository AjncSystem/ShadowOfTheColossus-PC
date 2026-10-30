// sotc: tests for gpu/sotc/unwind.cpp (trace v2 L3a). Guest part: a synthetic .eh_frame_hdr and
// .eh_frame in a buffer standing in for the eboot, with fake stacks. Host part (Windows): a real
// nested call chain in this executable, walked from RtlCaptureContext.
// Build: ninja -C out/gpu unwind-test (CMake target in the lead's gpu/CMakeLists.txt snippet).
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../gpu/sotc/unwind.h"

#ifdef _WIN32
#include <windows.h>
#endif

using namespace SotcUnwind;

namespace {

int g_failures = 0;

void Check(const char* name, bool ok) {
    std::printf("%-58s %s\n", name, ok ? "ok" : "FAILED");
    if (!ok) {
        ++g_failures;
    }
}

void Put32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); }

// Layout of the fake image (offsets from its base). Functions are 0x40 bytes apart.
alignas(16) uint8_t g_image[0x1000];
constexpr uint64_t kF1 = 0x100;     // push rbp; mov rbp,rsp (CFA rbp+16 after the prologue)
constexpr uint64_t kF2 = 0x140;     // frameless leaf: CFA rsp+8
constexpr uint64_t kF3 = 0x180;     // same shape as F1 (mid-prologue PC tested at +1)
constexpr uint64_t kThunk = 0x1c0;  // thunk range [0x1c0, 0x200)
constexpr uint64_t kCie = 0x200;
constexpr uint64_t kFdeF1 = 0x240;
constexpr uint64_t kFdeF2 = 0x260;
constexpr uint64_t kFdeF3 = 0x280;
constexpr uint64_t kHdr = 0x400;
constexpr uint64_t kHdrSize = 0x24;

// The CFI program shared by F1 and F3: push rbp (cfa rsp+16, rbp at cfa-16), mov rbp,rsp (cfa rbp+16).
constexpr uint8_t kPrologueCfi[] = {
    0x41,       // DW_CFA_advance_loc 1
    0x0e, 0x10, // DW_CFA_def_cfa_offset 16
    0x86, 0x02, // DW_CFA_offset r6, 2 (-16)
    0x43,       // DW_CFA_advance_loc 3
    0x0d, 0x06, // DW_CFA_def_cfa_register r6
};

uint64_t Addr(uint64_t off) { return reinterpret_cast<uint64_t>(g_image) + off; }

void BuildEhFrame() {
    std::memset(g_image, 0, sizeof g_image);
    uint8_t* cie = g_image + kCie;
    // CIE: augmentation "zR", data alignment -8, RA = r16, FDE encoding pcrel|sdata4 (0x1b).
    const uint8_t cie_body[] = {
        0x00, 0x00, 0x00, 0x00,       // CIE_id
        0x01,                         // version
        'z', 'R', 0x00,               // augmentation
        0x01,                         // code alignment
        0x78,                         // data alignment -8
        0x10,                         // return address register 16
        0x01, 0x1b,                   // augmentation data: R = pcrel sdata4
        0x0c, 0x07, 0x08,             // DW_CFA_def_cfa rsp, 8
        0x90, 0x01,                   // DW_CFA_offset ra, 1 (-8)
        0x00, 0x00,                   // nop padding
    };
    Put32(cie, uint32_t(sizeof cie_body));
    std::memcpy(cie + 4, cie_body, sizeof cie_body);

    auto fde = [](uint64_t at, uint64_t begin, uint64_t range, const uint8_t* insn, size_t insn_len,
                  size_t pad) {
        uint8_t* p = g_image + at;
        Put32(p + 4, uint32_t(at + 4 - kCie));                       // CIE pointer
        Put32(p + 8, uint32_t(int64_t(begin) - int64_t(at + 8)));     // pc_begin, pcrel
        Put32(p + 12, uint32_t(range));                              // pc_range
        p[16] = 0;                                                   // augmentation length
        std::memcpy(p + 17, insn, insn_len);
        const size_t total = 17 + insn_len + pad;
        Put32(p, uint32_t(total - 4));
        return total;
    };
    fde(kFdeF1, kF1, 0x40, kPrologueCfi, sizeof kPrologueCfi, 3);
    fde(kFdeF2, kF2, 0x20, nullptr, 0, 3);
    fde(kFdeF3, kF3, 0x40, kPrologueCfi, sizeof kPrologueCfi, 3);

    // .eh_frame_hdr: version 1, eh_frame_ptr pcrel sdata4, fde_count udata4, table datarel sdata4.
    uint8_t* h = g_image + kHdr;
    h[0] = 1;
    h[1] = 0x1b;
    h[2] = 0x03;
    h[3] = 0x3b;
    Put32(h + 4, uint32_t(int64_t(kCie) - int64_t(kHdr + 4)));
    Put32(h + 8, 3);
    const uint64_t table[][2] = {{kF1, kFdeF1}, {kF2, kFdeF2}, {kF3, kFdeF3}}; // sorted by location
    for (int i = 0; i < 3; ++i) {
        Put32(h + 12 + 8 * i, uint32_t(int64_t(table[i][0]) - int64_t(kHdr)));
        Put32(h + 16 + 8 * i, uint32_t(int64_t(table[i][1]) - int64_t(kHdr)));
    }
}

StackCopy Stack(const uint64_t* words, size_t count) {
    StackCopy s;
    s.base = reinterpret_cast<uint64_t>(words);
    s.bytes = reinterpret_cast<const uint8_t*>(words);
    s.size = count * 8;
    return s;
}

// A thunk resolver with one saved return address (the shadow stack entry for thunk 0).
struct OneThunk {
    uint64_t ra;
};
uint64_t ResolveOne(void* ctx, uint32_t index) {
    return index == 0 ? static_cast<OneThunk*>(ctx)->ra : 0;
}

void GuestTests() {
    BuildEhFrame();
    Check("InitGuest accepts the synthetic eh_frame_hdr",
          InitGuest(Addr(0), sizeof g_image, Addr(kHdr), kHdrSize));
    AddThunkRange(Addr(kThunk), 0x40);

    Check("KindOf: guest function", KindOf(Addr(kF1 + 10)) == Guest);
    Check("KindOf: thunk range wins over guest", KindOf(Addr(kThunk + 0x10)) == Thunk);
    Check("KindOf: outside everything", KindOf(0x1234) == SotcUnwind::Unknown);

    // A: leaf F2 at +2 (CFA rsp+8), called from F1 (rbp frame), called from F3 (rbp frame).
    //    st[0] = return into F1, st[8] = F1's saved rbp (= &st[16]), st[9] = RA into F3,
    //    st[16] = F3's saved rbp (= &st[18]) and st[17] = 0 (root).
    {
        uint64_t st[64] = {};
        st[0] = Addr(kF1 + 10);
        st[8] = reinterpret_cast<uint64_t>(&st[16]);
        st[9] = Addr(kF3 + 20);
        st[16] = reinterpret_cast<uint64_t>(&st[18]);
        st[17] = 0;
        Regs r{Addr(kF2 + 2), reinterpret_cast<uint64_t>(&st[0]), reinterpret_cast<uint64_t>(&st[8])};
        uint64_t frames[8] = {};
        bool trunc = true;
        const uint32_t n = Walk(r, Stack(st, 64), frames, 8, &trunc);
        Check("A: leaf, rbp frame, rbp frame: 3 frames", n == 3);
        Check("A: frame 1 is F1+10", n > 1 && frames[1] == Addr(kF1 + 10));
        Check("A: frame 2 is F3+20 (CFI restores rbp)", n > 2 && frames[2] == Addr(kF3 + 20));
        Check("A: not truncated (root reached)", !trunc);
    }

    // B: mid-prologue PC: F3+1 (after push rbp only). CFA = rsp+16, RA at rsp+8, rbp at rsp.
    {
        uint64_t st[64] = {};
        const size_t t = 24; // rsp
        const size_t r_index = 30; // F1's frame, above rsp
        st[t] = reinterpret_cast<uint64_t>(&st[r_index]);
        st[t + 1] = Addr(kF1 + 10);
        st[r_index] = 0;
        st[r_index + 1] = 0; // RA of F1 is 0: root
        Regs r{Addr(kF3 + 1), reinterpret_cast<uint64_t>(&st[t]), 0x1234};
        uint64_t frames[8] = {};
        bool trunc = true;
        const uint32_t n = Walk(r, Stack(st, 64), frames, 8, &trunc);
        Check("B: mid-prologue: 2 frames", n == 2);
        Check("B: mid-prologue: frame 1 is F1+10", n > 1 && frames[1] == Addr(kF1 + 10));
        Check("B: mid-prologue: not truncated", !trunc);
    }

    // C: the leaf's return address is in the thunk range: the resolver supplies F1+10.
    {
        uint64_t st[64] = {};
        st[0] = Addr(kThunk + 0x10);
        st[8] = reinterpret_cast<uint64_t>(&st[16]);
        st[9] = Addr(kF3 + 20);
        st[16] = reinterpret_cast<uint64_t>(&st[18]);
        st[17] = 0;
        OneThunk ctx{Addr(kF1 + 10)};
        Regs r{Addr(kF2 + 2), reinterpret_cast<uint64_t>(&st[0]), reinterpret_cast<uint64_t>(&st[8])};
        uint64_t frames[8] = {};
        bool trunc = true;
        const uint32_t n = Walk(r, Stack(st, 64), frames, 8, &trunc, ResolveOne, &ctx);
        Check("C: thunk: 4 frames (leaf, thunk, F1, F3)", n == 4);
        Check("C: thunk: frame 1 is the thunk", n > 1 && frames[1] == Addr(kThunk + 0x10));
        Check("C: thunk: frame 2 is the resolved F1+10", n > 2 && frames[2] == Addr(kF1 + 10));
        Check("C: thunk: not truncated", !trunc);
    }

    // D: an address nobody knows stops the walk truncated.
    {
        uint64_t st[8] = {};
        st[0] = 0x5555;
        Regs r{Addr(kF2 + 2), reinterpret_cast<uint64_t>(&st[0]), 0};
        uint64_t frames[8] = {};
        bool trunc = false;
        const uint32_t n = Walk(r, Stack(st, 8), frames, 8, &trunc);
        Check("D: unknown return address: truncated", trunc && n == 2);
    }
}

#ifdef _WIN32
// Host chain: each level records its own return address (the frame it returns into).
struct Chain {
    uint64_t ra[3];
    uint64_t frames[32];
    uint32_t count;
    bool truncated;
    uint64_t frame0;
};
volatile int g_sink = 0;

__attribute__((noinline)) void HostLevel3(Chain& c) {
    c.ra[0] = reinterpret_cast<uint64_t>(__builtin_return_address(0));
    // Static: a CONTEXT on this frame would push the return addresses out of the copied window.
    static CONTEXT ctx;
    std::memset(&ctx, 0, sizeof ctx);
    ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    RtlCaptureContext(&ctx);
    // The copy starts at the captured rsp: this function's frame and the callers' frames above it.
    static uint64_t copy[128];
    std::memcpy(copy, reinterpret_cast<const void*>(static_cast<uintptr_t>(ctx.Rsp)), sizeof copy);
    StackCopy stack{};
    stack.base = ctx.Rsp;
    stack.bytes = reinterpret_cast<const uint8_t*>(copy);
    stack.size = sizeof copy;
    Regs regs{ctx.Rip, ctx.Rsp, ctx.Rbp};
    c.count = Walk(regs, stack, c.frames, 32, &c.truncated);
    c.frame0 = ctx.Rip;
    g_sink = g_sink + 1;
}

__attribute__((noinline)) void HostLevel2(Chain& c) {
    c.ra[1] = reinterpret_cast<uint64_t>(__builtin_return_address(0));
    HostLevel3(c);
    g_sink = g_sink + 1; // keeps the call from becoming a tail call
}

__attribute__((noinline)) void HostLevel1(Chain& c) {
    c.ra[2] = reinterpret_cast<uint64_t>(__builtin_return_address(0));
    HostLevel2(c);
    g_sink = g_sink + 1;
}

void HostTests() {
    const uint64_t exe = reinterpret_cast<uint64_t>(GetModuleHandleA(nullptr));
    Check("AddHostModule: executable", AddHostModule(exe));
    if (HMODULE ntdll = GetModuleHandleA("ntdll.dll")) {
        AddHostModule(reinterpret_cast<uint64_t>(ntdll));
    }
    if (HMODULE k32 = GetModuleHandleA("kernel32.dll")) {
        AddHostModule(reinterpret_cast<uint64_t>(k32));
    }
    Check("KindOf: executable code is Host", KindOf(reinterpret_cast<uint64_t>(&HostLevel3)) == Host);

    Chain c{};
    HostLevel1(c);
    std::printf("    host walk: %u frames, truncated=%d\n", c.count, c.truncated ? 1 : 0);
    for (uint32_t i = 0; i < c.count; ++i) {
        std::printf("      #%u %016llx %s\n", i, static_cast<unsigned long long>(c.frames[i]),
                    KindOf(c.frames[i]) == Host ? "host" : "other");
    }
    Check("host: frame 0 is the captured rip", c.count > 0 && c.frames[0] == c.frame0);
    // The three return addresses recorded by the chain must appear in order.
    uint32_t at = 1;
    bool found[3] = {false, false, false};
    for (int level = 0; level < 3; ++level) {
        for (; at < c.count; ++at) {
            if (c.frames[at] == c.ra[level]) {
                found[level] = true;
                ++at;
                break;
            }
        }
    }
    Check("host: returns into HostLevel2 found", found[0]);
    Check("host: returns into HostLevel1 found", found[1]);
    Check("host: returns into main found", found[2]);
}
#endif

} // namespace

int main() {
    GuestTests();
#ifdef _WIN32
    HostTests();
#endif
    std::printf("%s (%d failures)\n", g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures ? 1 : 0;
}
