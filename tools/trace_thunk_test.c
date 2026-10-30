/* sotc: standalone test of the import thunks (src/runtime_trace_imports.c, no game needed).
 *
 *   clang --target=x86_64-w64-windows-gnu -std=c11 -O1 -Wall -Wextra \
 *       -o trace_thunk_test.exe tools/trace_thunk_test.c src/runtime_trace_imports.c
 *   trace_thunk_test.exe        thunks on (SOTC_TRACE_IMPORTS=2, every call is a span)
 *   trace_thunk_test.exe off    thunks off (wrap returns the host function unchanged)
 *
 * The recorder (trace_now, trace_push, trace_register_chunk) is replaced by the strong stubs
 * below. Host functions use the guest convention (sysv_abi) and are called through the slot
 * values that trace_imports_wrap returns, as the loader does. */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "../src/trace.h"
#include "../src/trace_imports.h"

#define SYSV __attribute__((sysv_abi))

/* Recorder stand-ins. */
static uint64_t fake_clock;
uint64_t bbgpu_trace_now(void) { return fake_clock += 7; }
static unsigned span_count_index7;
void bbgpu_trace_push(uint8_t kind, const char *name, uint64_t t0, uint64_t t1, uint64_t a,
                      uint64_t b, uint32_t aux) {
    (void)name; (void)a; (void)b;
    if (kind == 4 && aux == 7 && t1 >= t0) ++span_count_index7;
}
static const char *(*chunk_provider)(size_t *len);
void bbgpu_trace_register_chunk(uint32_t type, const char *(*provider)(size_t *len)) {
    if (type == 6) chunk_provider = provider;
}

typedef struct { uint64_t a, b; } P128;
typedef uint64_t (SYSV *F8)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
typedef int (SYSV *VSumFn)(int, ...);
typedef double (SYSV *DRet)(double);
typedef P128 (SYSV *S128)(uint64_t);
typedef uint64_t (SYSV *Outer)(uint64_t);
typedef void (SYSV *JumpFn)(uint64_t);
typedef void (SYSV *ProbeFn)(void);
typedef uint64_t (SYSV *Plus1)(uint64_t);

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { ++failures; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* Host functions. */
static SYSV uint64_t f8(uint64_t a, uint64_t b, uint64_t c, uint64_t d,
                        uint64_t e, uint64_t f, uint64_t g, uint64_t h) {
    return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * g + 8 * h; /* g and h are stack arguments */
}
/* Variadic callers pass the vector-register count in al; this host returns al unchanged. Clang
 * does not allow va_start in a SysV function on the Windows target, so the test uses asm. */
__asm__(".text\n\t"
        ".globl al_probe\n"
        "al_probe:\n\t"
        "movzbl %al, %eax\n\t"
        "ret\n\t");
extern int SYSV al_probe(int, ...);
static SYSV double dret(double x) { return x * 2.5; }
static SYSV P128 s128(uint64_t x) { P128 r = {x, ~x}; return r; }
static uint64_t f8_thunk_or_host;
static SYSV uint64_t outer(uint64_t x) { /* nested: a host function calling another slot */
    return ((F8)(uintptr_t)f8_thunk_or_host)(x, 1, 2, 3, 4, 5, 6, 7) + 1000;
}
/* Non-unwinding setjmp/longjmp, the same as the game's bb_setjmp/bb_longjmp (src/compat_win.c).
 * The msvcrt longjmp unwinds frames, and the thunk frames carry no unwind data. Win64 ABI. */
typedef unsigned long long JmpBuf[32];
__asm__(".text\n\t"
        ".globl test_setjmp\n"
        "test_setjmp:\n\t"
        "movq %rbx,0(%rcx)\n\t"
        "movq %rbp,8(%rcx)\n\t"
        "movq %rdi,16(%rcx)\n\t"
        "movq %rsi,24(%rcx)\n\t"
        "movq %r12,32(%rcx)\n\t"
        "movq %r13,40(%rcx)\n\t"
        "movq %r14,48(%rcx)\n\t"
        "movq %r15,56(%rcx)\n\t"
        "leaq 8(%rsp),%rdx\n\t"
        "movq %rdx,64(%rcx)\n\t"
        "movq (%rsp),%rdx\n\t"
        "movq %rdx,72(%rcx)\n\t"
        "xorl %eax,%eax\n\t"
        "ret\n\t"
        ".globl test_longjmp\n"
        "test_longjmp:\n\t"
        "movl %edx,%eax\n\t"
        "testl %eax,%eax\n\t"
        "jnz 1f\n\t"
        "movl $1,%eax\n"
        "1:\n\t"
        "movq 0(%rcx),%rbx\n\t"
        "movq 8(%rcx),%rbp\n\t"
        "movq 16(%rcx),%rdi\n\t"
        "movq 24(%rcx),%rsi\n\t"
        "movq 32(%rcx),%r12\n\t"
        "movq 40(%rcx),%r13\n\t"
        "movq 48(%rcx),%r14\n\t"
        "movq 56(%rcx),%r15\n\t"
        "movq 64(%rcx),%rsp\n\t"
        "jmp *72(%rcx)\n\t");
__attribute__((returns_twice)) int test_setjmp(JmpBuf buffer);
__attribute__((noreturn)) void test_longjmp(JmpBuf buffer, int value);
static JmpBuf *jump_target;
static SYSV void jumper(uint64_t v) { test_longjmp(*jump_target, (int)v); }
static uint64_t probe_seen;
static SYSV void probe(void) { probe_seen = GUEST_CALLER(); }
static SYSV uint64_t plus1(uint64_t x) { return x + 1; }

/* Calls fn and returns the address of the instruction after its call (the return address the
 * callee sees). Everything caller-saved is clobbered, as for any call. */
static uint64_t call_with_label(uint64_t fn) {
    uint64_t ra;
    __asm__ volatile(
        "leaq 1f(%%rip), %0\n\t"
        "callq *%1\n"
        "1:\n\t"
        : "=&r"(ra)
        : "r"(fn)
        : "rax", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11",
          "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7",
          "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15", "memory", "cc");
    return ra;
}

static uint64_t t_count;
static DWORD WINAPI worker(LPVOID arg) {
    (void)arg;
    for (uint64_t i = 0; i < 50; ++i) {
        uint64_t r = ((Plus1)(uintptr_t)t_count)(i);
        if (r != i + 1) { CHECK(0, "thread call returned %llu", (unsigned long long)r); break; }
    }
    return 0;
}

static int contains(const char *s, const char *what) { return s && strstr(s, what) != NULL; }

int main(int argc, char **argv) {
    int off = argc > 1 && !strcmp(argv[1], "off");
    if (off) {
        _putenv("SOTC_TRACE_IMPORTS=");
    } else {
        _putenv("SOTC_TRACE_IMPORTS=2");
        _putenv("SOTC_TRACE_IMPORT_MIN_US=0");
    }
    int mode = trace_imports_init(16);
    CHECK(mode == (off ? 0 : 2), "init returned %d", mode);

    uint64_t t_f8 = trace_imports_wrap(0, "t_f8#lib#mod", (uint64_t)(uintptr_t)f8);
    uint64_t t_vsum = trace_imports_wrap(1, "t_vsum#lib#mod", (uint64_t)(uintptr_t)al_probe);
    uint64_t t_dret = trace_imports_wrap(2, "t_dret#lib#mod", (uint64_t)(uintptr_t)dret);
    uint64_t t_s128 = trace_imports_wrap(3, "t_s128#lib#mod", (uint64_t)(uintptr_t)s128);
    uint64_t t_outer = trace_imports_wrap(4, "t_outer#lib#mod", (uint64_t)(uintptr_t)outer);
    uint64_t t_jumper = trace_imports_wrap(5, "t_jumper#lib#mod", (uint64_t)(uintptr_t)jumper);
    uint64_t t_probe = trace_imports_wrap(6, "t_probe#lib#mod", (uint64_t)(uintptr_t)probe);
    t_count = trace_imports_wrap(7, "t_count#lib#mod", (uint64_t)(uintptr_t)plus1);
    f8_thunk_or_host = t_f8;
    /* Excluded (longjmp) and out-of-range indices keep the host value. */
    CHECK(trace_imports_wrap(8, "lKEN2IebgJ0#lib#mod", (uint64_t)(uintptr_t)f8) == (uint64_t)(uintptr_t)f8,
          "longjmp import was wrapped");
    CHECK(trace_imports_wrap(99, "t_f8#lib#mod", (uint64_t)(uintptr_t)f8) == (uint64_t)(uintptr_t)f8,
          "out-of-range index was wrapped");
    trace_imports_seal();

    if (off) {
        CHECK(t_f8 == (uint64_t)(uintptr_t)f8 && t_count == (uint64_t)(uintptr_t)plus1, "off: slots changed");
    } else {
        CHECK(t_f8 != (uint64_t)(uintptr_t)f8, "on: slot not thunked");
    }

    /* 1. Eight integer arguments, two of them on the stack. */
    CHECK(((F8)(uintptr_t)t_f8)(1, 2, 3, 4, 5, 6, 7, 8) == 204, "f8 arguments");
    /* 2. Variadic: the thunk must keep al (vector registers used) for the host. */
    CHECK(((VSumFn)(uintptr_t)t_vsum)(2, 1.0, 2.0) == 2, "variadic al (2 doubles)");
    CHECK(((VSumFn)(uintptr_t)t_vsum)(10, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10.0) == 8,
          "variadic al (10 doubles)");
    /* 3. Double return in xmm0. */
    CHECK(((DRet)(uintptr_t)t_dret)(4.0) == 10.0, "double return");
    /* 4. 16-byte struct return in rax:rdx. */
    P128 p = ((S128)(uintptr_t)t_s128)(5);
    CHECK(p.a == 5 && p.b == ~(uint64_t)5, "128-bit return");
    /* 5. Nested: outer is a thunked host function that calls the f8 slot. outer(3) = 3 + 168 + 1000. */
    CHECK(((Outer)(uintptr_t)t_outer)(3) == 1171, "nested thunk call");
    /* 6. longjmp out of a thunked call, then calls keep working (abandoned frame). */
    JmpBuf jb;
    jump_target = &jb;
    volatile int jumped = 0;
    if (test_setjmp(jb) == 0) {
        ((JumpFn)(uintptr_t)t_jumper)(42);
        jumped = -1;
    } else {
        jumped = 1;
    }
    CHECK(jumped == 1, "longjmp did not return to setjmp");
    for (int i = 0; i < 3; ++i)
        CHECK(((F8)(uintptr_t)t_f8)(1, 2, 3, 4, 5, 6, 7, 8) == 204, "call after longjmp %d", i);
    /* 7. The guest caller of a host function reached through a thunk is the real return address. */
    uint64_t ra = call_with_label(t_probe);
    CHECK(probe_seen == ra, "GUEST_CALLER %llx != return address %llx",
          (unsigned long long)probe_seen, (unsigned long long)ra);
    CHECK(trace_imports_caller(0x1234) == 0x1234, "caller outside the thunk code changed");

    if (!off) {
        /* 8. Counters and spans, then the same from a second thread (merged in the chunk). */
        for (uint64_t i = 0; i < 100; ++i)
            CHECK(((Plus1)(uintptr_t)t_count)(i) == i + 1, "plus1 %llu", (unsigned long long)i);
        HANDLE h = CreateThread(NULL, 0, worker, NULL, 0, NULL);
        CHECK(h != NULL, "CreateThread");
        if (h) { WaitForSingleObject(h, INFINITE); CloseHandle(h); }
        CHECK(span_count_index7 == 150, "spans for index 7: %u", span_count_index7);
        size_t len = 0;
        const char *json = chunk_provider ? chunk_provider(&len) : NULL;
        char copy[16384];
        if (json) { strncpy(copy, json, sizeof(copy) - 1); copy[sizeof(copy) - 1] = 0; }
        CHECK(json && contains(copy, "\"name\": \"t_count\", \"calls\": 150,"), "IMPORT_STATS: %s",
              copy);
        CHECK(json && contains(copy, "\"name\": \"t_f8\""), "IMPORT_STATS lacks t_f8");
        char line[512];
        trace_imports_report(line, sizeof(line), 1.0);
        CHECK(contains(line, "SOTC IMPORTS: ") && contains(line, "t_count"), "report: %s", line);
        printf("IMPORT_STATS: %s\nREPORT: %s\n", copy, line);
    }

    printf(failures ? "FAILED (%d)\n" : "PASSED\n", failures);
    return failures ? 1 : 0;
}
