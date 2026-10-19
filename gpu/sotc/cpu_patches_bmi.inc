// sotc: BMI1 (ANDN, BEXTR, BLSI, BLSMSK, BLSR, TZCNT), LZCNT and MOVBE on CPUs without them
// (Intel before Haswell, issue #49). The PS4's Jaguar has them and the game, its libc and its
// modules use them. Included by cpu_patches.cpp. Each trampoline gives the result and the flags
// the instruction defines; scratch registers are saved below the guest red zone.
// SOTC_EMULATE_BMI=1 uses the trampolines on any CPU (testing).

static bool EmulatesCpuFeature(Cpu::Type type) {
    static const bool forced = [] {
        const char* v = std::getenv("SOTC_EMULATE_BMI");
        return v && v[0] == '1';
    }();
    static const Cpu cpu;
    return forced || !cpu.has(type);
}

/// The trampolines handle 64-bit addressing without segment overrides and never rsp as a
/// register operand (compilers do not emit those forms here).
static bool FilterSupportedOperands(const ZydisDecodedOperand* operands) {
    for (u32 i = 0; i < 3; ++i) {
        const auto& op = operands[i];
        if (op.type == ZYDIS_OPERAND_TYPE_REGISTER &&
            ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, op.reg.value) ==
                ZYDIS_REGISTER_RSP) {
            return false;
        }
        if (op.type != ZYDIS_OPERAND_TYPE_MEMORY) {
            continue;
        }
        if (op.mem.segment == ZYDIS_REGISTER_FS || op.mem.segment == ZYDIS_REGISTER_GS) {
            return false;
        }
        for (const ZydisRegister reg : {op.mem.base, op.mem.index}) {
            if (reg != ZYDIS_REGISTER_NONE && reg != ZYDIS_REGISTER_RIP &&
                ZydisRegisterGetWidth(ZYDIS_MACHINE_MODE_LONG_64, reg) != 64) {
                return false;
            }
        }
    }
    return true;
}

static bool FilterNoBMI1(const ZydisDecodedOperand* operands) {
    return EmulatesCpuFeature(Cpu::tBMI1) && FilterSupportedOperands(operands);
}

static bool FilterNoLZCNT(const ZydisDecodedOperand* operands) {
    return EmulatesCpuFeature(Cpu::tLZCNT) && FilterSupportedOperands(operands);
}

static bool FilterNoMOVBE(const ZydisDecodedOperand* operands) {
    return EmulatesCpuFeature(Cpu::tMOVBE) && FilterSupportedOperands(operands);
}

static bool IsBmiMnemonic(ZydisMnemonic mnemonic) {
    switch (mnemonic) {
    case ZYDIS_MNEMONIC_ANDN:
    case ZYDIS_MNEMONIC_BEXTR:
    case ZYDIS_MNEMONIC_BLSI:
    case ZYDIS_MNEMONIC_BLSMSK:
    case ZYDIS_MNEMONIC_BLSR:
    case ZYDIS_MNEMONIC_TZCNT:
    case ZYDIS_MNEMONIC_LZCNT:
    case ZYDIS_MNEMONIC_MOVBE:
        return true;
    default:
        return false;
    }
}

/// A short TZCNT/LZCNT/MOVBE that could not be relocated: its first two bytes became UD2 and
/// the illegal instruction handler runs the original from here (EmulateBmiTrapSite). Filled
/// while the image is patched, before the game runs; read only afterwards.
struct BmiTrapSite {
    ZydisDecodedInstruction instruction;
    std::array<ZydisDecodedOperand, ZYDIS_MAX_OPERAND_COUNT> operands;
};
static std::unordered_map<u64, BmiTrapSite> g_bmi_trap_sites;

namespace {

constexpr s32 GuestRedZone = 128;

/// General purpose register of a register operand at its own size (16, 32 or 64 bits).
Xbyak::Reg GprOperand(const ZydisDecodedOperand& operand) {
    return ZydisToXbyakRegisterOperand(operand);
}

Xbyak::Reg GprOfSize(const Xbyak::Reg64& reg, u32 bits) {
    switch (bits) {
    case 16:
        return reg.cvt16();
    case 32:
        return reg.cvt32();
    default:
        return reg;
    }
}

/// Index (0-15) of the 64-bit register holding a general purpose register, or -1.
int GprIndex(ZydisRegister reg) {
    if (reg == ZYDIS_REGISTER_NONE) {
        return -1;
    }
    const ZydisRegister largest = ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, reg);
    if (largest >= ZYDIS_REGISTER_RAX && largest <= ZYDIS_REGISTER_R15) {
        return largest - ZYDIS_REGISTER_RAX;
    }
    return -1;
}

/// Registers the instruction reads or writes (operands and addresses), as a bit mask of indices.
u32 UsedGprs(const ZydisDecodedOperand* operands, u32 count) {
    u32 mask = 1u << 4; // rsp is never a scratch register
    for (u32 i = 0; i < count; ++i) {
        const auto& op = operands[i];
        if (op.type == ZYDIS_OPERAND_TYPE_REGISTER) {
            if (const int index = GprIndex(op.reg.value); index >= 0) {
                mask |= 1u << index;
            }
        } else if (op.type == ZYDIS_OPERAND_TYPE_MEMORY) {
            if (const int index = GprIndex(op.mem.base); index >= 0) {
                mask |= 1u << index;
            }
            if (const int index = GprIndex(op.mem.index); index >= 0) {
                mask |= 1u << index;
            }
        }
    }
    return mask;
}

/// A free register for scratch use, marked used in mask. rcx can be asked for (shifts).
Xbyak::Reg64 TakeScratch(u32& mask, int wanted = -1) {
    if (wanted >= 0) {
        mask |= 1u << wanted;
        return Xbyak::Reg64(wanted);
    }
    static constexpr int order[] = {0, 2, 3, 6, 7, 8, 9, 10, 11, 1, 5, 12, 13, 14, 15};
    for (const int index : order) {
        if (!(mask & (1u << index))) {
            mask |= 1u << index;
            return Xbyak::Reg64(index);
        }
    }
    UNREACHABLE_MSG("No scratch register left");
}

/// Memory operand of the instruction at address, from a trampoline whose rsp is rsp_shift bytes
/// below the guest's. The image lies below 2 GiB, so a rip-relative address becomes absolute.
Xbyak::Address GuestMemory(const ZydisDecodedOperand& op, const void* address, s32 rsp_shift) {
    ASSERT_MSG(op.type == ZYDIS_OPERAND_TYPE_MEMORY, "Expected a memory operand");
    if (op.mem.base == ZYDIS_REGISTER_RIP) {
        ZydisDecodedInstruction instruction;
        ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
        const auto status = Common::Decoder::Instance()->decodeInstruction(
            instruction, operands, const_cast<void*>(address));
        ASSERT(ZYAN_SUCCESS(status));
        const u64 target = reinterpret_cast<u64>(address) + instruction.length + op.mem.disp.value;
        ASSERT_MSG(target < (u64{1} << 31), "rip-relative target above 2 GiB: {:#x}", target);
        return ptr[reinterpret_cast<void*>(target)];
    }
    Xbyak::RegExp expression{};
    if (op.mem.base != ZYDIS_REGISTER_NONE) {
        expression = expression + ZydisToXbyakRegister(op.mem.base);
    }
    if (op.mem.index != ZYDIS_REGISTER_NONE) {
        expression = expression + ZydisToXbyakRegister(op.mem.index) *
                                      (op.mem.scale != 0 ? op.mem.scale : 1);
    }
    s64 disp = op.mem.disp.has_displacement ? op.mem.disp.value : 0;
    if (GprIndex(op.mem.base) == 4) {
        disp += rsp_shift;
    }
    if (disp != 0) {
        expression = expression + disp;
    }
    return ptr[expression];
}

/// Saves scratch registers below the red zone; Restore puts them back, except the one that
/// holds the destination, whose saved value is replaced by the result first. No flag changes.
struct ScratchFrame {
    Xbyak::CodeGenerator& c;
    std::vector<Xbyak::Reg64> saved;

    ScratchFrame(Xbyak::CodeGenerator& c_, std::initializer_list<Xbyak::Reg64> regs)
        : c{c_}, saved{regs} {
        c.lea(rsp, ptr[rsp - GuestRedZone]);
        for (const auto& reg : saved) {
            c.push(reg);
        }
    }

    s32 Shift() const {
        return GuestRedZone + static_cast<s32>(saved.size()) * 8;
    }

    /// dst = result (sizes as the instruction); restores every saved register.
    void Restore(const Xbyak::Reg& dst, const Xbyak::Reg64& result) {
        const int dst_index = dst.getIdx();
        bool dst_saved = false;
        for (size_t i = 0; i < saved.size(); ++i) {
            if (saved[i].getIdx() == dst_index) {
                // Slot i is at rsp + (saved.size() - 1 - i) * 8. A 32-bit result zero-extends,
                // a 16-bit one keeps the upper bits of the destination.
                const s32 slot = static_cast<s32>(saved.size() - 1 - i) * 8;
                if (dst.getBit() == 32) {
                    c.mov(result.cvt32(), result.cvt32());
                    c.mov(qword[rsp + slot], result);
                } else if (dst.getBit() == 16) {
                    c.mov(word[rsp + slot], result.cvt16());
                } else {
                    c.mov(qword[rsp + slot], result);
                }
                dst_saved = true;
            }
        }
        if (!dst_saved) {
            c.mov(dst, GprOfSize(result, dst.getBit()));
        }
        for (auto it = saved.rbegin(); it != saved.rend(); ++it) {
            c.pop(*it);
        }
        c.lea(rsp, ptr[rsp + GuestRedZone]);
    }
};

void LoadOperand(Xbyak::CodeGenerator& c, const Xbyak::Reg64& into, const ZydisDecodedOperand& op,
                 u32 bits, const void* address, s32 rsp_shift) {
    const auto dst = GprOfSize(into, bits == 64 ? 64 : 32);
    if (op.type == ZYDIS_OPERAND_TYPE_REGISTER) {
        c.mov(dst, GprOfSize(Xbyak::Reg64(GprIndex(op.reg.value)), bits == 64 ? 64 : 32));
    } else {
        c.mov(dst, GuestMemory(op, address, rsp_shift));
    }
}

/// TZCNT / LZCNT: BSF / BSR, the source-zero case, then the flags (CF: source zero, ZF: result
/// zero). Without the trampoline an old CPU runs them as BSF / BSR: wrong results, no fault.
void GenerateBitCount(void* address, const ZydisDecodedOperand* operands, Xbyak::CodeGenerator& c,
                      bool leading) {
    const auto dst = GprOperand(operands[0]);
    const u32 bits = operands[0].size;
    Xbyak::Label zero, done;
    if (operands[1].type == ZYDIS_OPERAND_TYPE_REGISTER) {
        const auto src = GprOperand(operands[1]);
        leading ? c.bsr(dst, src) : c.bsf(dst, src);
    } else {
        const auto src = GuestMemory(operands[1], address, 0);
        leading ? c.bsr(dst, src) : c.bsf(dst, src);
    }
    c.jz(zero);
    if (leading) {
        c.xor_(dst, bits - 1); // bits - 1 - index; ZF = result zero, CF = 0
    } else {
        c.test(dst, dst);
    }
    c.jmp(done);
    c.L(zero);
    c.mov(dst, bits);
    c.test(dst, dst); // ZF = 0
    c.stc();          // CF = 1: the source was zero
    c.L(done);
}

} // namespace

static void GenerateTZCNT(void* address, const ZydisDecodedOperand* operands,
                          Xbyak::CodeGenerator& c) {
    GenerateBitCount(address, operands, c, false);
}

static void GenerateLZCNT(void* address, const ZydisDecodedOperand* operands,
                          Xbyak::CodeGenerator& c) {
    GenerateBitCount(address, operands, c, true);
}

/// ANDN dst, src1, src2: dst = ~src1 & src2; SF, ZF from the result, CF = OF = 0.
static void GenerateANDN(void* address, const ZydisDecodedOperand* operands,
                         Xbyak::CodeGenerator& c) {
    const auto dst = GprOperand(operands[0]);
    const u32 bits = operands[0].size;
    u32 used = UsedGprs(operands, 3);
    const auto t = TakeScratch(used);
    ScratchFrame frame{c, {t}};
    c.mov(GprOfSize(t, bits), GprOfSize(Xbyak::Reg64(GprIndex(operands[1].reg.value)), bits));
    c.not_(GprOfSize(t, bits));
    if (operands[2].type == ZYDIS_OPERAND_TYPE_REGISTER) {
        c.and_(GprOfSize(t, bits), GprOperand(operands[2]));
    } else {
        c.and_(GprOfSize(t, bits), GuestMemory(operands[2], address, frame.Shift()));
    }
    frame.Restore(dst, t);
}

/// BEXTR dst, src, control: bits [start, start + length) of src, start = control[7:0],
/// length = control[15:8]; ZF from the result, CF = OF = 0.
static void GenerateBEXTR(void* address, const ZydisDecodedOperand* operands,
                          Xbyak::CodeGenerator& c) {
    const auto dst = GprOperand(operands[0]);
    const u32 bits = operands[0].size;
    u32 used = UsedGprs(operands, 3) | (1u << 1); // rcx: saved for the shifts
    const auto value = TakeScratch(used);
    const auto mask = TakeScratch(used);
    ScratchFrame frame{c, {rcx, value, mask}};
    LoadOperand(c, value, operands[1], bits, address, frame.Shift());
    // The control register may be rcx itself: it was not changed yet.
    c.mov(ecx, GprOfSize(Xbyak::Reg64(GprIndex(operands[2].reg.value)), 32));
    Xbyak::Label zero, no_mask, done;
    c.cmp(cl, bits);
    c.jae(zero); // start past the operand: 0
    c.shr(value, cl);
    c.movzx(ecx, ch);
    c.cmp(ecx, bits);
    c.jae(no_mask); // length covers the rest
    c.mov(mask.cvt32(), 1);
    c.shl(mask, cl);
    c.dec(mask);
    c.and_(value, mask);
    c.L(no_mask);
    c.jmp(done);
    c.L(zero);
    c.xor_(value.cvt32(), value.cvt32());
    c.L(done);
    c.test(value, value); // ZF; CF = OF = 0
    frame.Restore(dst, value);
}

namespace {

enum class BlsKind { Isolate, Mask, Reset };

/// BLSI: src & -src, CF = src != 0. BLSMSK: src ^ (src - 1), CF = src == 0.
/// BLSR: src & (src - 1), CF = src == 0. SF, ZF from the result, OF = 0.
void GenerateBls(void* address, const ZydisDecodedOperand* operands, Xbyak::CodeGenerator& c,
                 BlsKind kind) {
    const auto dst = GprOperand(operands[0]);
    const u32 bits = operands[0].size;
    u32 used = UsedGprs(operands, 2);
    const auto src = TakeScratch(used);
    const auto result = TakeScratch(used);
    ScratchFrame frame{c, {src, result}};
    LoadOperand(c, src, operands[1], bits, address, frame.Shift());
    const auto s = GprOfSize(src, bits);
    const auto r = GprOfSize(result, bits);
    Xbyak::Label zero, done;
    c.test(s, s);
    c.jz(zero);
    switch (kind) {
    case BlsKind::Isolate:
        c.mov(r, s);
        c.neg(r);
        c.and_(r, s); // nonzero: ZF = 0, SF; CF = 0
        c.stc();      // CF = 1: source nonzero
        break;
    case BlsKind::Mask:
        c.lea(r, ptr[src - 1]);
        c.xor_(r, s);
        break;
    case BlsKind::Reset:
        c.lea(r, ptr[src - 1]);
        c.and_(r, s);
        break;
    }
    c.jmp(done);
    c.L(zero);
    switch (kind) {
    case BlsKind::Isolate:
        c.xor_(r, r); // ZF = 1, CF = 0
        break;
    case BlsKind::Mask:
        c.mov(r, -1);
        c.test(r, r); // ZF = 0, SF = 1
        c.stc();
        break;
    case BlsKind::Reset:
        c.xor_(r, r);
        c.stc();
        break;
    }
    c.L(done);
    frame.Restore(dst, result);
}

} // namespace

static void GenerateBLSI(void* address, const ZydisDecodedOperand* operands,
                         Xbyak::CodeGenerator& c) {
    GenerateBls(address, operands, c, BlsKind::Isolate);
}

static void GenerateBLSMSK(void* address, const ZydisDecodedOperand* operands,
                           Xbyak::CodeGenerator& c) {
    GenerateBls(address, operands, c, BlsKind::Mask);
}

static void GenerateBLSR(void* address, const ZydisDecodedOperand* operands,
                         Xbyak::CodeGenerator& c) {
    GenerateBls(address, operands, c, BlsKind::Reset);
}

/// MOVBE: a move that reverses the byte order; no flag changes.
static void GenerateMOVBE(void* address, const ZydisDecodedOperand* operands,
                          Xbyak::CodeGenerator& c) {
    const u32 bits = operands[0].size;
    if (operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER) {
        // Load: the memory is read before anything moves rsp.
        const auto dst = GprOperand(operands[0]);
        c.mov(dst, GuestMemory(operands[1], address, 0));
        if (bits == 16) {
            c.lea(rsp, ptr[rsp - GuestRedZone]);
            c.pushfq();
            c.rol(dst, 8);
            c.popfq();
            c.lea(rsp, ptr[rsp + GuestRedZone]);
        } else {
            c.bswap(Xbyak::Reg32e(dst.getIdx(), dst.getBit()));
        }
        return;
    }
    // Store: swap a copy of the source.
    u32 used = UsedGprs(operands, 2);
    const auto t = TakeScratch(used);
    const auto value = GprOfSize(t, bits);
    c.lea(rsp, ptr[rsp - GuestRedZone]);
    if (bits == 16) {
        c.pushfq();
    }
    c.push(t);
    const s32 shift = GuestRedZone + (bits == 16 ? 16 : 8);
    c.mov(value, GprOperand(operands[1]));
    if (bits == 16) {
        c.rol(value, 8);
    } else {
        c.bswap(Xbyak::Reg32e(value.getIdx(), value.getBit()));
    }
    c.mov(GuestMemory(operands[0], address, shift), value);
    c.pop(t);
    if (bits == 16) {
        c.popfq();
    }
    c.lea(rsp, ptr[rsp + GuestRedZone]);
}
