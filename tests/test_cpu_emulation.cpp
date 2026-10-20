// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: the BMI1/LZCNT/MOVBE emulation of the CPU patches (gpu/sotc/cpu_patches.cpp) against the
// real instructions, on a CPU that has them: each form runs natively and emulated on the same
// random registers, memory and flags, and the registers, memory and defined flags must match.
//   ninja -C out/gpu cpu-emulation-test && out/gpu/cpu-emulation-test.exe
#include <cstdio>
#include <cstring>
#include <functional>
#include <random>
#include <xbyak/xbyak.h>
#include "cpu_patches.h"

using namespace Xbyak::util;

namespace {

struct State {
    u64 rax, rbx, rcx, rdx, rsi;
    u64 flags;
    u64 memory; // [rsi]
    u64 stack;  // [rsp + 8]
};

constexpr u64 CF = 1, PF = 4, ZF = 0x40, SF = 0x80, OF = 0x800;
constexpr u64 Arithmetic = CF | PF | 0x10 | ZF | SF | OF;

using Emit = std::function<void(Xbyak::CodeGenerator&)>;

// void run(State*): loads the state (rsi points at State::memory), runs `body`, stores it back.
struct Runner : Xbyak::CodeGenerator {
    explicit Runner(const Emit& body) : Xbyak::CodeGenerator(4096) {
        const auto state = r12;
        push(rbx);
        push(rsi);
        push(rdi);
        push(r12);
        mov(state, rcx); // Windows x64: first argument
        sub(rsp, 64);
        mov(rax, ptr[state + offsetof(State, stack)]);
        mov(ptr[rsp + 8], rax);
        mov(rax, ptr[state + offsetof(State, rax)]);
        mov(rbx, ptr[state + offsetof(State, rbx)]);
        mov(rcx, ptr[state + offsetof(State, rcx)]);
        mov(rdx, ptr[state + offsetof(State, rdx)]);
        lea(rsi, ptr[state + offsetof(State, memory)]);
        push(qword[state + offsetof(State, flags)]);
        popfq();
        body(*this);
        pushfq();
        pop(qword[state + offsetof(State, flags)]);
        mov(ptr[state + offsetof(State, rax)], rax);
        mov(ptr[state + offsetof(State, rbx)], rbx);
        mov(ptr[state + offsetof(State, rcx)], rcx);
        mov(ptr[state + offsetof(State, rdx)], rdx);
        mov(ptr[state + offsetof(State, rsi)], rsi);
        mov(rax, ptr[rsp + 8]);
        mov(ptr[state + offsetof(State, stack)], rax);
        add(rsp, 64);
        pop(r12);
        pop(rdi);
        pop(rsi);
        pop(rbx);
        ret();
        ready();
    }
};

struct Case {
    const char* name;
    Emit native;
    u64 flags; // flags the instruction defines (or keeps)
};

u64 Value(std::mt19937_64& random) {
    switch (random() % 6) {
    case 0:
        return 0;
    case 1:
        return u64{1} << (random() % 64);
    case 2:
        return ~u64{0};
    case 3:
        return random() & 0xFFFFFFFF;
    default:
        return random();
    }
}

} // namespace

int main() {
    const std::vector<Case> cases = {
        {"tzcnt eax, ecx", [](auto& c) { c.tzcnt(eax, ecx); }, CF | ZF},
        {"tzcnt rax, rcx", [](auto& c) { c.tzcnt(rax, rcx); }, CF | ZF},
        {"tzcnt rcx, rcx", [](auto& c) { c.tzcnt(rcx, rcx); }, CF | ZF},
        {"tzcnt eax, [rsi]", [](auto& c) { c.tzcnt(eax, dword[rsi]); }, CF | ZF},
        {"tzcnt rax, [rsp+8]", [](auto& c) { c.tzcnt(rax, qword[rsp + 8]); }, CF | ZF},
        {"lzcnt eax, ecx", [](auto& c) { c.lzcnt(eax, ecx); }, CF | ZF},
        {"lzcnt rdx, rdx", [](auto& c) { c.lzcnt(rdx, rdx); }, CF | ZF},
        {"lzcnt rax, [rsp+8]", [](auto& c) { c.lzcnt(rax, qword[rsp + 8]); }, CF | ZF},
        {"andn eax, ebx, ecx", [](auto& c) { c.andn(eax, ebx, ecx); }, CF | ZF | SF | OF},
        {"andn rax, rbx, rcx", [](auto& c) { c.andn(rax, rbx, rcx); }, CF | ZF | SF | OF},
        {"andn rax, rax, rbx", [](auto& c) { c.andn(rax, rax, rbx); }, CF | ZF | SF | OF},
        {"andn rax, rbx, rax", [](auto& c) { c.andn(rax, rbx, rax); }, CF | ZF | SF | OF},
        {"andn rsi, rbx, [rsi]", [](auto& c) { c.andn(rsi, rbx, qword[rsi]); }, CF | ZF | SF | OF},
        {"andn eax, ebx, [rsp+8]", [](auto& c) { c.andn(eax, ebx, dword[rsp + 8]); },
         CF | ZF | SF | OF},
        {"bextr eax, ebx, edx", [](auto& c) { c.bextr(eax, ebx, edx); }, CF | ZF | OF},
        {"bextr rax, rbx, rdx", [](auto& c) { c.bextr(rax, rbx, rdx); }, CF | ZF | OF},
        {"bextr rcx, rbx, rdx", [](auto& c) { c.bextr(rcx, rbx, rdx); }, CF | ZF | OF},
        {"bextr rax, rcx, rdx", [](auto& c) { c.bextr(rax, rcx, rdx); }, CF | ZF | OF},
        {"bextr rax, rbx, rcx", [](auto& c) { c.bextr(rax, rbx, rcx); }, CF | ZF | OF},
        {"bextr eax, [rsi], edx", [](auto& c) { c.bextr(eax, dword[rsi], edx); }, CF | ZF | OF},
        {"bextr rcx, [rsp+8], rcx", [](auto& c) { c.bextr(rcx, qword[rsp + 8], rcx); },
         CF | ZF | OF},
        {"blsi eax, ecx", [](auto& c) { c.blsi(eax, ecx); }, CF | ZF | SF | OF},
        {"blsi rax, rax", [](auto& c) { c.blsi(rax, rax); }, CF | ZF | SF | OF},
        {"blsi rax, [rsp+8]", [](auto& c) { c.blsi(rax, qword[rsp + 8]); }, CF | ZF | SF | OF},
        {"blsmsk eax, ecx", [](auto& c) { c.blsmsk(eax, ecx); }, CF | ZF | SF | OF},
        {"blsmsk rax, rax", [](auto& c) { c.blsmsk(rax, rax); }, CF | ZF | SF | OF},
        {"blsmsk rax, [rsi]", [](auto& c) { c.blsmsk(rax, qword[rsi]); }, CF | ZF | SF | OF},
        {"blsr eax, ecx", [](auto& c) { c.blsr(eax, ecx); }, CF | ZF | SF | OF},
        {"blsr rcx, rcx", [](auto& c) { c.blsr(rcx, rcx); }, CF | ZF | SF | OF},
        {"blsr rax, [rsi]", [](auto& c) { c.blsr(rax, qword[rsi]); }, CF | ZF | SF | OF},
        {"movbe eax, [rsi]", [](auto& c) { c.movbe(eax, dword[rsi]); }, Arithmetic},
        {"movbe rax, [rsi]", [](auto& c) { c.movbe(rax, qword[rsi]); }, Arithmetic},
        {"movbe cx, [rsi]", [](auto& c) { c.movbe(cx, word[rsi]); }, Arithmetic},
        {"movbe rsi, [rsi]", [](auto& c) { c.movbe(rsi, qword[rsi]); }, Arithmetic},
        {"movbe rax, [rsp+8]", [](auto& c) { c.movbe(rax, qword[rsp + 8]); }, Arithmetic},
        {"movbe [rsi], ebx", [](auto& c) { c.movbe(dword[rsi], ebx); }, Arithmetic},
        {"movbe [rsi], rbx", [](auto& c) { c.movbe(qword[rsi], rbx); }, Arithmetic},
        {"movbe [rsi], dx", [](auto& c) { c.movbe(word[rsi], dx); }, Arithmetic},
        {"movbe [rsp+8], rcx", [](auto& c) { c.movbe(qword[rsp + 8], rcx); }, Arithmetic},
    };

    std::mt19937_64 random(12345);
    int failures = 0;
    for (const auto& test : cases) {
        Xbyak::CodeGenerator instruction(64);
        test.native(instruction);
        void* bytes = const_cast<u8*>(instruction.getCode());
        Runner native(test.native);
        bool emitted = true;
        Runner emulated([&](Xbyak::CodeGenerator& c) {
            emitted = Core::EmitCpuEmulationForTest(bytes, c);
        });
        if (!emitted) {
            std::printf("%-24s no emulation\n", test.name);
            ++failures;
            continue;
        }
        int mismatches = 0;
        for (int i = 0; i < 200000 && mismatches < 3; ++i) {
            State in{Value(random), Value(random), Value(random), Value(random), 0,
                     0x202 | (random() & Arithmetic), Value(random), Value(random)};
            if (random() % 2) { // BEXTR controls within or just past the operand
                in.rcx = (in.rcx & ~u64{0xFFFF}) | (random() % 72) | (random() % 72) << 8;
                in.rdx = (in.rdx & ~u64{0xFFFF}) | (random() % 72) | (random() % 72) << 8;
            }
            State a = in, b = in;
            native.getCode<void (*)(State*)>()(&a);
            emulated.getCode<void (*)(State*)>()(&b);
            for (State* s : {&a, &b}) { // rsi still pointing at its own State::memory
                if (s->rsi == reinterpret_cast<u64>(&s->memory)) {
                    s->rsi = 0;
                }
            }
            const bool same = a.rax == b.rax && a.rbx == b.rbx && a.rcx == b.rcx &&
                              a.rdx == b.rdx && a.rsi == b.rsi && a.memory == b.memory &&
                              a.stack == b.stack && (a.flags & test.flags) == (b.flags & test.flags);
            if (!same) {
                ++mismatches;
                std::printf("%-24s MISMATCH in rax=%llx rbx=%llx rcx=%llx rdx=%llx mem=%llx "
                            "stack=%llx flags=%llx\n  native   rax=%llx rbx=%llx rcx=%llx rdx=%llx "
                            "mem=%llx stack=%llx flags=%llx\n  emulated rax=%llx rbx=%llx "
                            "rcx=%llx rdx=%llx mem=%llx stack=%llx flags=%llx\n",
                            test.name, in.rax, in.rbx, in.rcx, in.rdx, in.memory, in.stack,
                            in.flags, a.rax, a.rbx, a.rcx, a.rdx, a.memory, a.stack,
                            a.flags & test.flags, b.rax, b.rbx, b.rcx, b.rdx, b.memory, b.stack,
                            b.flags & test.flags);
            }
        }
        std::printf("%-24s %s (%zu bytes emulated)\n", test.name, mismatches ? "FAIL" : "ok",
                    emulated.getSize());
        failures += mismatches != 0;
    }
    std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
