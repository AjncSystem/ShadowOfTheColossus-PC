// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: stack unwinding over a copy of a thread's stack (see unwind.h). Guest frames use the
// eboot's .eh_frame CFI; host frames use the PE exception directory. Both are decoded here with
// bounds-checked readers: no dwarf.cpp (its decoder pulls in the logging and assert layer).
#include "unwind.h"

#include <algorithm>
#include <cstring>
#include <set>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace SotcUnwind {
namespace {

constexpr size_t kMaxModules = 64;
constexpr size_t kMaxThunks = 32;
constexpr int kMaxChain = 4;
constexpr int kMaxCfaStack = 4;
constexpr uint32_t kDwarfRbp = 6;
constexpr uint32_t kDwarfRsp = 7;
constexpr uint32_t kDwarfRa = 16;
constexpr uint8_t kEncOmit = 0xff;

// UNWIND_CODE operations (winnt.h).
constexpr uint8_t kUwopPushNonvol = 0;
constexpr uint8_t kUwopAllocLarge = 1;
constexpr uint8_t kUwopAllocSmall = 2;
constexpr uint8_t kUwopSetFpreg = 3;
constexpr uint8_t kUwopSaveNonvol = 4;
constexpr uint8_t kUwopSaveNonvolFar = 5;
constexpr uint8_t kUwopSaveXmm128 = 8;
constexpr uint8_t kUwopSaveXmm128Far = 9;
constexpr uint8_t kUwopPushMachframe = 10;
// Register numbers in UNWIND_CODE are the x64 encoding (rbp = 5), not DWARF's (rbp = 6).
constexpr uint8_t kWinRbp = 5;
constexpr uint8_t kUnwFlagChainInfo = 0x04; // UNW_FLAG_CHAININFO after the 3-bit version shift

enum RuleKind : uint8_t { kSame, kOffset, kUndef, kOther };

struct Rule {
    RuleKind kind = kSame;
    int64_t off = 0;
};

// The CFI row for the registers that matter: CFA, rbp and the return address.
struct Row {
    uint32_t cfa_reg = kDwarfRsp;
    int64_t cfa_off = 8;
    bool cfa_ok = true;
    Rule rbp;
    Rule ra;
};

struct Cie {
    uint64_t code_align = 1;
    int64_t data_align = 1;
    uint8_t fde_enc = 0;
    bool has_z = false; // augmentation "z...": FDEs carry an augmentation data length
    Row init;
};

struct GuestEntry {
    uint64_t loc; // FDE initial location (pc_begin)
    uint64_t fde; // address of the FDE
};

struct HostFunc {
    uint32_t begin, end, unwind; // RVAs
};

struct HostBlob {
    uint32_t rva;    // UNWIND_INFO RVA
    uint32_t offset; // into HostModule::pool
    uint32_t length; // header + codes (+ the chained RUNTIME_FUNCTION)
};

struct HostModule {
    uint64_t base = 0;
    uint64_t size = 0;
    std::vector<HostFunc> funcs; // sorted by begin
    std::vector<HostBlob> blobs; // sorted by rva
    std::vector<uint8_t> pool;   // copies of the UNWIND_INFO blobs
};

struct ThunkRange {
    uint64_t start = 0;
    uint64_t size = 0;
};

// Guest state (set by InitGuest, read-only afterwards).
uint64_t g_image_base = 0;
uint64_t g_image_size = 0;
std::vector<GuestEntry> g_guest; // sorted by loc
ThunkRange g_thunks[kMaxThunks];
size_t g_thunk_count = 0;
std::vector<HostModule> g_modules;

enum class Step { Ok, Root, Fallback, Fail };

bool InImage(uint64_t addr, uint64_t n) {
    return g_image_size != 0 && addr >= g_image_base && n <= g_image_size &&
           addr - g_image_base <= g_image_size - n;
}

// Bounds-checked reader over [pos, end) inside the guest image.
struct Cursor {
    uint64_t pos = 0;
    uint64_t end = 0;
    bool ok = false;

    const uint8_t* Take(uint64_t n) {
        if (!ok || pos > end || n > end - pos) {
            ok = false;
            return nullptr;
        }
        const uint8_t* p = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(pos));
        pos += n;
        return p;
    }

    uint64_t Unsigned(uint64_t n) {
        const uint8_t* p = Take(n);
        uint64_t v = 0;
        for (uint64_t i = 0; p && i < n; ++i) {
            v |= uint64_t(p[i]) << (8 * i);
        }
        return v;
    }

    uint64_t Uleb() {
        uint64_t v = 0;
        unsigned shift = 0;
        for (;;) {
            const uint8_t* p = Take(1);
            if (!p) {
                return 0;
            }
            if (shift < 64) {
                v |= uint64_t(p[0] & 0x7f) << shift;
            }
            shift += 7;
            if (!(p[0] & 0x80)) {
                break;
            }
            if (shift >= 70) {
                ok = false;
                return 0;
            }
        }
        return v;
    }

    int64_t Sleb() {
        uint64_t v = 0;
        unsigned shift = 0;
        uint8_t b = 0;
        do {
            const uint8_t* p = Take(1);
            if (!p) {
                return 0;
            }
            b = p[0];
            if (shift < 64) {
                v |= uint64_t(b & 0x7f) << shift;
            }
            shift += 7;
            if (shift >= 70 && (b & 0x80)) {
                ok = false;
                return 0;
            }
        } while (b & 0x80);
        if (shift < 64 && (b & 0x40)) {
            v |= ~uint64_t(0) << shift;
        }
        return int64_t(v);
    }
};

Cursor Span(uint64_t addr, uint64_t n) {
    Cursor c;
    c.pos = addr;
    c.end = addr + n;
    c.ok = InImage(addr, n);
    return c;
}

// A pointer in the DWARF pointer encoding. `datarel` is the base for DW_EH_PE_datarel.
bool ReadEncoded(Cursor& c, uint8_t enc, uint64_t datarel, uint64_t& out) {
    const uint64_t field = c.pos;
    uint64_t v = 0;
    switch (enc & 0x0f) {
    case 0x00: // absptr
    case 0x04: // udata8
    case 0x0c: // sdata8
        v = c.Unsigned(8);
        break;
    case 0x01:
        v = c.Uleb();
        break;
    case 0x02:
        v = c.Unsigned(2);
        break;
    case 0x03:
        v = c.Unsigned(4);
        break;
    case 0x09:
        v = uint64_t(c.Sleb());
        break;
    case 0x0a:
        v = uint64_t(int64_t(int16_t(c.Unsigned(2))));
        break;
    case 0x0b:
        v = uint64_t(int64_t(int32_t(c.Unsigned(4))));
        break;
    default:
        return false;
    }
    if (!c.ok || (enc & 0x80)) {
        return false;
    }
    switch (enc & 0x70) {
    case 0x00:
        break;
    case 0x10: // pcrel
        v += field;
        break;
    case 0x30: // datarel
        v += datarel;
        break;
    default:
        return false;
    }
    out = v;
    return true;
}

// Runs a CFI program from c.pos to c.end, applying rows whose location is <= target.
bool Execute(const Cie& cie, Cursor c, uint64_t loc, uint64_t target, Row& row, const Row* init) {
    Row saved[kMaxCfaStack];
    int depth = 0;
    bool done = false;
    while (!done && c.pos < c.end) {
        if (!c.ok) {
            return false;
        }
        const uint8_t op = uint8_t(c.Unsigned(1));
        const uint8_t high = op & 0xc0;
        const uint8_t low = op & 0x3f;
        // Rule setters for the registers that matter (others are parsed and dropped).
        auto set_rule = [&row](uint64_t reg, RuleKind kind, int64_t off) {
            if (reg == kDwarfRbp) {
                row.rbp = Rule{kind, off};
            } else if (reg == kDwarfRa) {
                row.ra = Rule{kind, off};
            }
        };
        auto restore = [&row, init](uint64_t reg) {
            if (reg == kDwarfRbp) {
                row.rbp = init ? init->rbp : Rule{};
            } else if (reg == kDwarfRa) {
                row.ra = init ? init->ra : Rule{};
            }
        };
        auto advance = [&](uint64_t delta) {
            loc += delta;
            if (loc > target) {
                done = true;
            }
        };
        const auto data = [&cie](uint64_t v) { return int64_t(uint64_t(v) * uint64_t(cie.data_align)); };

        if (high == 0x40) {
            advance(uint64_t(low) * cie.code_align);
            continue;
        }
        if (high == 0x80) {
            set_rule(low, kOffset, data(c.Uleb()));
            continue;
        }
        if (high == 0xc0) {
            restore(low);
            continue;
        }
        switch (op) {
        case 0x00: // nop
            break;
        case 0x01: { // set_loc
            uint64_t v = 0;
            if (!ReadEncoded(c, cie.fde_enc, 0, v)) {
                return false;
            }
            loc = v;
            if (loc > target) {
                done = true;
            }
            break;
        }
        case 0x02:
            advance(c.Unsigned(1) * cie.code_align);
            break;
        case 0x03:
            advance(c.Unsigned(2) * cie.code_align);
            break;
        case 0x04:
            advance(c.Unsigned(4) * cie.code_align);
            break;
        case 0x05: { // offset_extended
            const uint64_t reg = c.Uleb();
            set_rule(reg, kOffset, data(c.Uleb()));
            break;
        }
        case 0x06: // restore_extended
            restore(c.Uleb());
            break;
        case 0x07: // undefined
            set_rule(c.Uleb(), kUndef, 0);
            break;
        case 0x08: // same_value
            set_rule(c.Uleb(), kSame, 0);
            break;
        case 0x09: { // register
            const uint64_t reg = c.Uleb();
            c.Uleb();
            set_rule(reg, kOther, 0);
            break;
        }
        case 0x0a: // remember_state
            if (depth == kMaxCfaStack) {
                return false;
            }
            saved[depth++] = row;
            break;
        case 0x0b: // restore_state
            if (depth == 0) {
                return false;
            }
            row = saved[--depth];
            break;
        case 0x0c: { // def_cfa
            const uint64_t reg = c.Uleb();
            row.cfa_off = int64_t(c.Uleb());
            row.cfa_reg = uint32_t(reg);
            row.cfa_ok = reg == kDwarfRbp || reg == kDwarfRsp;
            break;
        }
        case 0x0d: { // def_cfa_register
            const uint64_t reg = c.Uleb();
            row.cfa_reg = uint32_t(reg);
            row.cfa_ok = reg == kDwarfRbp || reg == kDwarfRsp;
            break;
        }
        case 0x0e: // def_cfa_offset
            row.cfa_off = int64_t(c.Uleb());
            break;
        case 0x0f: { // def_cfa_expression: the CFA is not a register + offset
            const uint64_t len = c.Uleb();
            c.Take(len);
            row.cfa_ok = false;
            break;
        }
        case 0x10: { // expression
            const uint64_t reg = c.Uleb();
            const uint64_t len = c.Uleb();
            c.Take(len);
            set_rule(reg, kOther, 0);
            break;
        }
        case 0x11: { // offset_extended_sf
            const uint64_t reg = c.Uleb();
            set_rule(reg, kOffset, data(uint64_t(c.Sleb())));
            break;
        }
        case 0x12: { // def_cfa_sf
            const uint64_t reg = c.Uleb();
            row.cfa_off = data(uint64_t(c.Sleb()));
            row.cfa_reg = uint32_t(reg);
            row.cfa_ok = reg == kDwarfRbp || reg == kDwarfRsp;
            break;
        }
        case 0x13: // def_cfa_offset_sf
            row.cfa_off = data(uint64_t(c.Sleb()));
            break;
        case 0x14: { // val_offset
            const uint64_t reg = c.Uleb();
            c.Uleb();
            set_rule(reg, kOther, 0);
            break;
        }
        case 0x15: { // val_offset_sf
            const uint64_t reg = c.Uleb();
            c.Sleb();
            set_rule(reg, kOther, 0);
            break;
        }
        case 0x16: { // val_expression
            const uint64_t reg = c.Uleb();
            const uint64_t len = c.Uleb();
            c.Take(len);
            set_rule(reg, kOther, 0);
            break;
        }
        case 0x2e: // GNU_args_size
            c.Uleb();
            break;
        case 0x2f: { // GNU_negative_offset_extended
            const uint64_t reg = c.Uleb();
            set_rule(reg, kOffset, -data(c.Uleb()));
            break;
        }
        default:
            return false;
        }
    }
    return c.ok;
}

// CIE at addr: its augmentation (z, R, L, P, S) and its initial instructions.
bool ParseCie(uint64_t addr, Cie& cie) {
    Cursor head = Span(addr, 4);
    const uint64_t len = head.Unsigned(4);
    if (!head.ok || len == 0 || len == 0xffffffff) {
        return false;
    }
    Cursor c = Span(addr + 4, len);
    if (!c.ok || c.Unsigned(4) != 0) { // CIE_id is 0 in .eh_frame
        return false;
    }
    const uint8_t version = uint8_t(c.Unsigned(1));
    if (version != 1 && version != 3) {
        return false;
    }
    char aug[8] = {};
    size_t n = 0;
    for (;;) {
        const uint8_t ch = uint8_t(c.Unsigned(1));
        if (!c.ok) {
            return false;
        }
        if (ch == 0) {
            break;
        }
        if (n + 1 >= sizeof aug) {
            return false;
        }
        aug[n++] = char(ch);
    }
    cie.code_align = c.Uleb();
    cie.data_align = c.Sleb();
    if (version == 1) {
        c.Unsigned(1); // return address register: always rip (16) here
    } else {
        c.Uleb();
    }
    if (n != 0 && aug[0] == 'z') {
        const uint64_t aug_len = c.Uleb();
        const uint64_t data_begin = c.pos;
        cie.has_z = true;
        for (size_t i = 1; i < n; ++i) {
            switch (aug[i]) {
            case 'R':
                cie.fde_enc = uint8_t(c.Unsigned(1));
                break;
            case 'L':
                c.Unsigned(1);
                break;
            case 'P': {
                const uint8_t penc = uint8_t(c.Unsigned(1));
                uint64_t skip = 0;
                if (!ReadEncoded(c, penc, 0, skip)) {
                    return false;
                }
                break;
            }
            case 'S':
            case 'B':
                break;
            default:
                return false;
            }
        }
        if (!c.ok || aug_len > c.end - data_begin) {
            return false;
        }
        c.pos = data_begin + aug_len;
    } else if (n != 0) {
        return false; // unknown augmentation: no safe way to find the instructions
    }
    if (!c.ok) { // a truncated CIE header: its initial instructions are not there
        return false;
    }
    cie.init = Row{};
    if (!Execute(cie, Cursor{c.pos, c.end, true}, 0, UINT64_MAX, cie.init, nullptr)) {
        return false;
    }
    return true;
}

// The CFI row for pc (a return address is looked up at pc - 1 by the caller).
bool CfiRowFor(uint64_t pc, Row& row) {
    if (g_guest.empty()) {
        return false;
    }
    auto it = std::upper_bound(g_guest.begin(), g_guest.end(), pc,
                               [](uint64_t v, const GuestEntry& e) { return v < e.loc; });
    if (it == g_guest.begin()) {
        return false;
    }
    --it;
    Cursor f = Span(it->fde, 4);
    const uint64_t len = f.Unsigned(4);
    if (!f.ok || len == 0 || len == 0xffffffff) {
        return false;
    }
    f = Span(it->fde + 4, len);
    if (!f.ok) {
        return false;
    }
    const uint64_t cie_field = f.pos;
    const uint64_t cie_off = f.Unsigned(4);
    Cie cie;
    if (!f.ok || !ParseCie(cie_field - cie_off, cie)) {
        return false;
    }
    // pc_begin uses the CIE's R encoding, pc_range only its format (no application bits).
    uint64_t begin = 0;
    uint64_t range = 0;
    if (!ReadEncoded(f, cie.fde_enc, 0, begin) || !ReadEncoded(f, cie.fde_enc & 0x0f, 0, range)) {
        return false;
    }
    if (begin != it->loc || pc < begin || pc - begin >= range) {
        return false;
    }
    if (cie.has_z) {
        const uint64_t aug_len = f.Uleb();
        if (!f.Take(aug_len)) {
            return false;
        }
    }
    // The FDE's instructions run from here to its end.
    Row row_out = cie.init;
    if (!Execute(cie, f, begin, pc, row_out, &cie.init)) {
        return false;
    }
    row = row_out;
    return true;
}

// Guest frame: the CFI at pc gives the CFA and where the return address and rbp are saved.
Step CfiStep(uint64_t pc, uint64_t rsp, uint64_t rbp, const StackCopy& st, uint64_t& ra,
             uint64_t& nrsp, uint64_t& nrbp) {
    Row row;
    if (!CfiRowFor(pc, row) || !row.cfa_ok) {
        return Step::Fallback;
    }
    const uint64_t cfa = (row.cfa_reg == kDwarfRsp ? rsp : rbp) + uint64_t(row.cfa_off);
    if (row.ra.kind == kUndef) {
        ra = 0; // the outermost frame
        return Step::Root;
    }
    if (row.ra.kind != kOffset) {
        return Step::Fallback;
    }
    if (!st.Read(cfa + uint64_t(row.ra.off), ra)) {
        return Step::Fail;
    }
    nrbp = rbp;
    if (row.rbp.kind == kOffset) {
        if (!st.Read(cfa + uint64_t(row.rbp.off), nrbp)) {
            return Step::Fail;
        }
    } else if (row.rbp.kind == kOther) {
        return Step::Fallback;
    }
    nrsp = cfa;
    return Step::Ok;
}

// rbp chain: [rbp] is the caller's rbp, [rbp + 8] the return address. rbp == 0 is a fiber root.
Step RbpStep(uint64_t rbp, const StackCopy& st, uint64_t& ra, uint64_t& nrsp, uint64_t& nrbp) {
    if (rbp == 0) {
        return Step::Root;
    }
    if (!st.Read(rbp + 8, ra) || !st.Read(rbp, nrbp)) {
        return Step::Fail;
    }
    nrsp = rbp + 16;
    return Step::Ok;
}

const HostModule* FindModule(uint64_t addr) {
    for (const HostModule& m : g_modules) {
        if (addr - m.base < m.size) {
            return &m;
        }
    }
    return nullptr;
}

bool FindFunc(const HostModule& m, uint32_t rva, HostFunc& out) {
    auto it = std::upper_bound(m.funcs.begin(), m.funcs.end(), rva,
                               [](uint32_t v, const HostFunc& f) { return v < f.begin; });
    if (it == m.funcs.begin()) {
        return false;
    }
    --it;
    if (rva >= it->end) {
        return false;
    }
    out = *it;
    return true;
}

const HostBlob* FindBlob(const HostModule& m, uint32_t rva) {
    auto it = std::lower_bound(m.blobs.begin(), m.blobs.end(), rva,
                               [](const HostBlob& b, uint32_t v) { return b.rva < v; });
    return (it != m.blobs.end() && it->rva == rva) ? &*it : nullptr;
}

uint32_t Le32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

uint16_t Le16(const uint8_t* p) {
    uint16_t v;
    std::memcpy(&v, p, 2);
    return v;
}

// UNWIND_CODE slots used by an operation (0 = not a valid operation for this unwinder).
uint32_t NodeCount(uint8_t op, uint8_t info) {
    switch (op) {
    case kUwopPushNonvol:
    case kUwopAllocSmall:
    case kUwopSetFpreg:
    case kUwopPushMachframe:
        return 1;
    case kUwopAllocLarge:
        return info == 0 ? 2 : info == 1 ? 3 : 0;
    case kUwopSaveNonvol:
    case kUwopSaveXmm128:
        return 2;
    case kUwopSaveNonvolFar:
    case kUwopSaveXmm128Far:
        return 3;
    default:
        return 0; // EPILOG and SPARE_CODE: not in a prolog
    }
}

// Host frame: interprets the UNWIND_INFO of the function containing pc on the module copy. The
// codes whose CodeOffset is past the offset of pc in the function have not run yet and are skipped.
Step HostStep(uint64_t pc, uint64_t rsp, uint64_t rbp, const StackCopy& st, uint64_t& ra,
              uint64_t& nrsp, uint64_t& nrbp, bool& machine) {
    machine = false;
    const HostModule* m = FindModule(pc);
    if (!m) {
        return Step::Fail;
    }
    const uint32_t rva = uint32_t(pc - m->base);
    HostFunc f{};
    if (!FindFunc(*m, rva, f)) {
        // No unwind info: a leaf, the return address is on top of the stack.
        if (!st.Read(rsp, ra)) {
            return Step::Fail;
        }
        nrsp = rsp + 8;
        nrbp = rbp;
        return Step::Ok;
    }
    const uint32_t offset = rva - f.begin;
    uint64_t r = rsp;
    uint64_t b = rbp;
    uint32_t unwind = f.unwind;
    for (int depth = 0;; ++depth) {
        if (depth == kMaxChain) {
            return Step::Fail;
        }
        const HostBlob* blob = FindBlob(*m, unwind);
        if (!blob || blob->length < 4) {
            return Step::Fail;
        }
        const uint8_t* p = m->pool.data() + blob->offset;
        const uint32_t len = blob->length;
        const uint8_t version = p[0] & 7;
        if (version != 1 && version != 2) { // UNWIND_INFO versions this unwinder interprets
            return Step::Fail;
        }
        const uint8_t flags = p[0] >> 3;
        const uint32_t count = p[2];
        const uint8_t frame_reg = p[3] & 0x0f;
        const uint32_t frame_off = p[3] >> 4;
        const uint32_t slots = (count + 1) & ~1u;
        if (4 + 2 * slots > len) {
            return Step::Fail;
        }
        for (uint32_t i = 0; i < count;) {
            const uint8_t* s = p + 4 + 2 * i;
            const uint8_t op = s[1] & 0x0f;
            const uint8_t info = s[1] >> 4;
            const uint32_t nodes = NodeCount(op, info);
            if (nodes == 0 || i + nodes > count) {
                return Step::Fail;
            }
            if (s[0] <= offset) {
                switch (op) {
                case kUwopPushNonvol: {
                    if (info == kWinRbp) {
                        if (!st.Read(r, b)) {
                            return Step::Fail;
                        }
                    }
                    r += 8;
                    break;
                }
                case kUwopAllocLarge:
                    r += nodes == 2 ? uint64_t(Le16(s + 2)) * 8 : uint64_t(Le32(s + 2));
                    break;
                case kUwopAllocSmall:
                    r += uint64_t(info) * 8 + 8;
                    break;
                case kUwopSetFpreg:
                    if (frame_reg != kWinRbp) { // only rbp frames are tracked
                        return Step::Fail;
                    }
                    r = b - uint64_t(frame_off) * 16;
                    break;
                case kUwopSaveNonvol:
                case kUwopSaveNonvolFar: {
                    const uint64_t off = nodes == 2 ? uint64_t(Le16(s + 2)) * 8 : uint64_t(Le32(s + 2));
                    if (info == kWinRbp && !st.Read(r + off, b)) {
                        return Step::Fail;
                    }
                    break;
                }
                case kUwopSaveXmm128:
                case kUwopSaveXmm128Far:
                    break; // xmm registers are not tracked
                case kUwopPushMachframe: {
                    // Interrupt frame: [RIP, CS, EFLAGS, RSP, SS], after an error code if info == 1.
                    const uint64_t base = r + (info ? 8 : 0);
                    uint64_t rip_value = 0;
                    uint64_t rsp_value = 0;
                    if (!st.Read(base, rip_value) || !st.Read(base + 24, rsp_value)) {
                        return Step::Fail;
                    }
                    ra = rip_value;
                    nrsp = rsp_value;
                    nrbp = b;
                    machine = true;
                    return Step::Ok;
                }
                default:
                    break;
                }
            }
            i += nodes;
        }
        if (!(flags & kUnwFlagChainInfo)) {
            break;
        }
        // The chained RUNTIME_FUNCTION follows the codes; its codes come next.
        if (4 + 2 * slots + 12 > len) {
            return Step::Fail;
        }
        unwind = Le32(p + 4 + 2 * slots + 8);
    }
    if (!st.Read(r, ra)) {
        return Step::Fail;
    }
    nrsp = r + 8;
    nrbp = b;
    return Step::Ok;
}

// Copies the UNWIND_INFO blob at `rva` (header, codes and the chained entry) out of the module.
bool CopyBlob(const uint8_t* img, uint32_t image_size, uint32_t rva, std::vector<uint8_t>& pool,
              std::vector<HostBlob>& blobs, uint32_t& chained_unwind, bool& chained) {
    if (uint64_t(rva) + 4 > image_size) {
        return false;
    }
    const uint8_t* p = img + rva;
    const uint32_t slots = (uint32_t(p[2]) + 1) & ~1u;
    uint32_t len = 4 + 2 * slots;
    chained = (p[0] >> 3) & kUnwFlagChainInfo;
    if (chained) {
        len += 12;
    }
    if (uint64_t(rva) + len > image_size) {
        return false;
    }
    if (chained) { // read only after the bounds check above
        chained_unwind = Le32(p + 4 + 2 * slots + 8);
    }
    blobs.push_back(HostBlob{rva, uint32_t(pool.size()), len});
    pool.insert(pool.end(), p, p + len);
    return true;
}

} // namespace

bool StackCopy::Read(uint64_t addr, uint64_t& out) const {
    if (!bytes || size < 8 || addr < base) {
        return false;
    }
    const uint64_t off = addr - base;
    if (off > size - 8) {
        return false;
    }
    std::memcpy(&out, bytes + off, 8);
    return true;
}

bool InitGuest(uint64_t image_base, uint64_t image_size, uint64_t eh_frame_hdr, uint64_t hdr_size) {
    g_image_base = image_base;
    g_image_size = image_size;
    g_guest.clear();
    if (!image_base || !image_size || !hdr_size) {
        return false;
    }
    // .eh_frame_hdr: version, pointer/count/table encodings, eh_frame_ptr, fde_count, then the
    // binary search table of (initial location, FDE address) pairs (datarel to the header).
    Cursor c = Span(eh_frame_hdr, hdr_size);
    if (!c.ok || c.Unsigned(1) != 1) {
        return false;
    }
    const uint8_t ptr_enc = uint8_t(c.Unsigned(1));
    const uint8_t count_enc = uint8_t(c.Unsigned(1));
    const uint8_t table_enc = uint8_t(c.Unsigned(1));
    uint64_t skip = 0;
    if (ptr_enc != kEncOmit && !ReadEncoded(c, ptr_enc, eh_frame_hdr, skip)) {
        return false;
    }
    uint64_t count = 0;
    if (count_enc != kEncOmit && !ReadEncoded(c, count_enc, eh_frame_hdr, count)) {
        return false;
    }
    if (count == 0 || table_enc == kEncOmit || count > (c.end - c.pos) / 2) {
        return false;
    }
    g_guest.reserve(size_t(count));
    for (uint64_t i = 0; i < count; ++i) {
        uint64_t loc = 0;
        uint64_t fde = 0;
        if (!ReadEncoded(c, table_enc, eh_frame_hdr, loc) ||
            !ReadEncoded(c, table_enc, eh_frame_hdr, fde)) {
            g_guest.clear();
            return false;
        }
        if (InImage(fde, 4)) {
            g_guest.push_back(GuestEntry{loc, fde});
        }
    }
    std::sort(g_guest.begin(), g_guest.end(),
              [](const GuestEntry& a, const GuestEntry& b) { return a.loc < b.loc; });
    return !g_guest.empty();
}

// True when [p, p+len) is committed and readable now. A module can be mid-load or mid-unload when
// enumerated at boot, so its header and tables are not safe to read by size alone (an unreadable
// module header faulted the game). Always true off Windows (the parser is only used there).
bool HostReadable(const void* p, size_t len) {
#ifdef _WIN32
    const auto* a = reinterpret_cast<const uint8_t*>(p);
    size_t done = 0;
    while (done < len) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(a + done, &mbi, sizeof(mbi)) != sizeof(mbi) || mbi.State != MEM_COMMIT) {
            return false;
        }
        const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                               PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        if ((mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) || !(mbi.Protect & readable)) {
            return false;
        }
        const auto* end = reinterpret_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
        if (end <= a + done) {
            return false;
        }
        done = static_cast<size_t>(end - a);
    }
    return true;
#else
    (void)p;
    (void)len;
    return true;
#endif
}

bool AddHostModule(uint64_t base) {
    if (base == 0) {
        return false;
    }
    for (const HostModule& m : g_modules) {
        if (m.base == base) {
            return true;
        }
    }
    if (g_modules.size() >= kMaxModules) {
        return false;
    }
    // The module may be mid-load or mid-unload: read nothing without checking it is readable.
    const uint8_t* img = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(base));
    if (!HostReadable(img, 0x40) || img[0] != 'M' || img[1] != 'Z') {
        return false;
    }
    const uint32_t lfanew = Le32(img + 0x3c);
    if (lfanew < 0x40 || lfanew > 0x400) {
        return false;
    }
    const uint8_t* nt = img + lfanew;
    if (!HostReadable(nt, 168) || std::memcmp(nt, "PE\0\0", 4) != 0 ||
        Le16(nt + 4) != 0x8664) { // x86-64 only
        return false;
    }
    const uint8_t* opt = nt + 24;
    if (Le16(opt) != 0x20b) { // PE32+
        return false;
    }
    const uint32_t image_size = Le32(opt + 56);
    const uint32_t exc_rva = Le32(opt + 136); // data directory 3: exception (.pdata)
    const uint32_t exc_size = Le32(opt + 140);
    if (image_size == 0 || image_size > 0x7fffffff || exc_size % 12 != 0 ||
        uint64_t(exc_rva) + exc_size > image_size || !HostReadable(img + exc_rva, exc_size)) {
        return false;
    }

    std::vector<HostFunc> funcs;
    std::set<uint32_t> queued;
    for (uint32_t i = 0; i < exc_size / 12; ++i) {
        const uint8_t* e = img + exc_rva + size_t(i) * 12;
        const HostFunc f{Le32(e), Le32(e + 4), Le32(e + 8)};
        if (f.begin >= f.end || f.end > image_size || f.unwind >= image_size) {
            continue;
        }
        funcs.push_back(f);
        queued.insert(f.unwind);
    }
    std::sort(funcs.begin(), funcs.end(),
              [](const HostFunc& a, const HostFunc& b) { return a.begin < b.begin; });

    // Copy every UNWIND_INFO the walk can reach, including chained ones.
    std::vector<uint32_t> work(queued.begin(), queued.end());
    std::vector<HostBlob> blobs;
    std::vector<uint8_t> pool;
    for (size_t k = 0; k < work.size(); ++k) {
        uint32_t next = 0;
        bool chained = false;
        if (!CopyBlob(img, image_size, work[k], pool, blobs, next, chained)) {
            return false;
        }
        if (chained && next < image_size && queued.insert(next).second) {
            work.push_back(next);
        }
    }
    std::sort(blobs.begin(), blobs.end(),
              [](const HostBlob& a, const HostBlob& b) { return a.rva < b.rva; });

    if (g_modules.capacity() < kMaxModules) {
        g_modules.reserve(kMaxModules);
    }
    HostModule m;
    m.base = base;
    m.size = image_size;
    m.funcs = std::move(funcs);
    m.blobs = std::move(blobs);
    m.pool = std::move(pool);
    g_modules.push_back(std::move(m));
    return true;
}

void AddThunkRange(uint64_t start, uint64_t size) {
    if (size && g_thunk_count < kMaxThunks) {
        g_thunks[g_thunk_count++] = ThunkRange{start, size};
    }
}

FrameKind KindOf(uint64_t addr) {
    for (size_t i = 0; i < g_thunk_count; ++i) {
        if (addr - g_thunks[i].start < g_thunks[i].size) {
            return Thunk;
        }
    }
    if (g_image_size && addr - g_image_base < g_image_size) {
        return Guest;
    }
    if (FindModule(addr)) {
        return Host;
    }
    return Unknown;
}

uint32_t Walk(const Regs& regs, const StackCopy& stack, uint64_t* frames, uint32_t max,
              bool* truncated, ThunkResolver thunk_ra, void* ctx) {
    bool trunc = false;
    uint32_t n = 0;
    if (!frames || max == 0) {
        if (truncated) {
            *truncated = false;
        }
        return 0;
    }
    uint64_t rip = regs.rip;
    uint64_t rsp = regs.rsp;
    uint64_t rbp = regs.rbp;
    bool exact = true; // rip is the exact pc (frame 0, or the frame of an interrupt)
    uint32_t thunk_index = 0;
    frames[n++] = rip;
    while (n < max && rip != 0) {
        const uint64_t pc = exact ? rip : rip - 1; // a return address names the call's last byte
        const FrameKind kind = KindOf(pc);
        uint64_t ra = 0;
        uint64_t nrsp = 0;
        uint64_t nrbp = 0;
        bool machine = false;
        Step step = Step::Fail;
        if (kind == Guest) {
            step = CfiStep(pc, rsp, rbp, stack, ra, nrsp, nrbp);
            if (step == Step::Fallback) {
                step = RbpStep(rbp, stack, ra, nrsp, nrbp);
            }
        } else if (kind == Host) {
            step = HostStep(pc, rsp, rbp, stack, ra, nrsp, nrbp, machine);
        } else if (kind == Thunk) {
            // A thunk has no frame of its own. Its caller is the guest return address: the
            // resolver's if one was passed, else the one the thunk saved at [frame top + 344]
            // (the guest resumes with rsp = frame top + 352).
            const uint64_t real = thunk_ra ? thunk_ra(ctx, thunk_index) : 0;
            ++thunk_index;
            if (real) {
                ra = real;
                nrsp = rsp;
                nrbp = rbp;
                step = Step::Ok;
            } else if (rsp > UINT64_MAX - 352 || !stack.Read(rsp + 344, ra) || KindOf(ra) != Guest) {
                step = Step::Fail;
            } else {
                nrsp = rsp + 352;
                nrbp = rbp;
                step = Step::Ok;
            }
        } else {
            trunc = true; // Unknown: no unwinding data for this address
            break;
        }
        if (step == Step::Fail) {
            trunc = true;
            break;
        }
        if (step == Step::Root || ra == 0) {
            break;
        }
        if (kind != Thunk && nrsp <= rsp) {
            trunc = true; // the stack must grow toward the caller: anything else is corrupt
            break;
        }
        rip = ra;
        rsp = nrsp;
        rbp = nrbp;
        exact = machine;
        frames[n++] = rip;
    }
    if (truncated) {
        *truncated = trunc;
    }
    return n;
}

} // namespace SotcUnwind
