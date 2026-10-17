// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/arch.h"
#include "common/signal_context.h"

#ifdef _WIN32
#include <windows.h>
#elif defined(__FreeBSD__)
#include <machine/npx.h>
#include <sys/ucontext.h>
#else
#include <sys/ucontext.h>
#endif

namespace Common {

void* GetRip(void* ctx) {
#if defined(_WIN32)
    return (void*)((EXCEPTION_POINTERS*)ctx)->ContextRecord->Rip;
#elif defined(__APPLE__) && defined(ARCH_X86_64)
    return (void*)((ucontext_t*)ctx)->uc_mcontext->__ss.__rip;
#elif defined(__APPLE__) && defined(ARCH_ARM64)
    return (void*)((ucontext_t*)ctx)->uc_mcontext->__ss.__pc;
#elif defined(__FreeBSD__)
    return (void*)((ucontext_t*)ctx)->uc_mcontext.mc_rip;
#elif defined(ARCH_X86_64)
    return (void*)((ucontext_t*)ctx)->uc_mcontext.gregs[REG_RIP];
#else
#error "Unsupported architecture"
#endif
}

void SkipLoadAsZero(void* ctx, u8 gpr_index, u64 length) {
#if defined(_WIN32)
    // CONTEXT keeps Rax..R15 in x86 encoding order.
    auto* context = ((EXCEPTION_POINTERS*)ctx)->ContextRecord;
    (&context->Rax)[gpr_index] = 0;
    context->Rip += length;
#elif defined(ARCH_X86_64) && !defined(__APPLE__) && !defined(__FreeBSD__)
    static constexpr int regs[16] = {REG_RAX, REG_RCX, REG_RDX, REG_RBX, REG_RSP, REG_RBP,
                                     REG_RSI, REG_RDI, REG_R8,  REG_R9,  REG_R10, REG_R11,
                                     REG_R12, REG_R13, REG_R14, REG_R15};
    auto& gregs = ((ucontext_t*)ctx)->uc_mcontext.gregs;
    gregs[regs[gpr_index & 15]] = 0;
    gregs[REG_RIP] += length;
#else
#error "Unsupported architecture"
#endif
}

bool IsWriteError(void* ctx) {
#if defined(_WIN32)
    return ((EXCEPTION_POINTERS*)ctx)->ExceptionRecord->ExceptionInformation[0] == 1;
#elif defined(__APPLE__) && defined(ARCH_X86_64)
    return ((ucontext_t*)ctx)->uc_mcontext->__es.__err & 0x2;
#elif defined(__APPLE__) && defined(ARCH_ARM64)
    return ((ucontext_t*)ctx)->uc_mcontext->__es.__esr & 0x40;
#elif defined(__FreeBSD__) && defined(ARCH_X86_64)
    return ((ucontext_t*)ctx)->uc_mcontext.mc_err & 0x2;
#elif defined(ARCH_X86_64)
    return ((ucontext_t*)ctx)->uc_mcontext.gregs[REG_ERR] & 0x2;
#else
#error "Unsupported architecture"
#endif
}

} // namespace Common
