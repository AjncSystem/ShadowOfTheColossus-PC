/* sotc: SOTC_TRACE_IMPORTS timing thunks for PS4 import calls (src/trace_imports.h, trace v2 P3).
 *
 * Layout of one thunked call (guest SysV code, Windows host). The guest return address E stays in
 * guest memory; the host function runs on the frame C = E - 344 below it:
 *
 *     C+0     16 copied stack-argument slots (the callee's incoming stack arguments)
 *     C+128   saved argument registers, index, t0, return registers (the callee never writes here)
 *     C+344   E: the guest return address (untouched by the callee)
 *
 * The callee returns into trace_imports_common with rsp == C; it reads E from [C+344] and jumps
 * back with rsp = E+8. Control flow therefore depends only on the stack, so longjmp, fiber
 * switches and any amount of nesting cannot corrupt the guest return. The per-thread shadow of
 * frames (diagnostics only) feeds trace_imports_caller. Limit: a host function reads at most 16
 * stack arguments through the copy; the import table needs fewer. */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "trace.h"
#include "trace_imports.h"

int trace_imports_mode = 0;

#ifdef _WIN32
#include <windows.h>

/* Host functions and the helpers below use the guest (SysV) convention. */
#define IMPORT_ABI __attribute__((sysv_abi))

/* Weak: absent in the standalone test (tools/trace_thunk_test.c). */
const char *runtime_import_name(const char *name) __attribute__((weak));
uintptr_t runtime_fiber_resolve(const char *name) __attribute__((weak));

#define THUNK_BYTES 32
#define FRAME_BYTES 344    /* C to E: must match the constants in the asm below */
#define SHADOW_DEPTH 64
#define EV_IMPORT 4        /* EventKind::EvImport (gpu/shadps4/video_core/sotc_trace.h) */

typedef struct { uint64_t calls, ticks, max, window_max; } ImportStat;
typedef struct ThreadStats { struct ThreadStats *next; ImportStat *items; } ThreadStats;
typedef struct { uint64_t frame; uint32_t index; } ShadowEntry;

/* Read by the common code: targets[index] is the host function. */
uint64_t *trace_imports_targets __attribute__((used));
extern char trace_imports_common_begin[], trace_imports_common_end[];

static uint8_t *area;
static size_t area_size;
static uint32_t import_total;
static const char **labels;
static uint64_t min_ns = 50000;
static uint64_t *prev_calls, *prev_ticks;   /* report window baseline, under lock */
static uint64_t shadow_overflow;
static ThreadStats *thread_list;
static CRITICAL_SECTION lock;
static __thread ThreadStats *tls_stats;
static __thread ShadowEntry shadow[SHADOW_DEPTH];
static __thread uint32_t shadow_depth;

/* The common code, in .text. Entry: rsp = E (guest return address), r11 = import index.
 * r10 is scratch; argument and return registers are preserved around the host call. */
__asm__(
    ".text\n\t"
    ".p2align 4\n\t"
    ".globl trace_imports_common_begin\n"
    "trace_imports_common_begin:\n\t"
    "subq $344, %rsp\n\t"                       /* rsp = C */
#define TI_COPY(k) "movq " #k "+352(%rsp),%r10\n\t" "movq %r10," #k "(%rsp)\n\t"
    TI_COPY(0) TI_COPY(8) TI_COPY(16) TI_COPY(24) TI_COPY(32) TI_COPY(40) TI_COPY(48) TI_COPY(56)
    TI_COPY(64) TI_COPY(72) TI_COPY(80) TI_COPY(88) TI_COPY(96) TI_COPY(104) TI_COPY(112) TI_COPY(120)
#undef TI_COPY
    "movq %rdi,128(%rsp)\n\t"
    "movq %rsi,136(%rsp)\n\t"
    "movq %rdx,144(%rsp)\n\t"
    "movq %rcx,152(%rsp)\n\t"
    "movq %r8,160(%rsp)\n\t"
    "movq %r9,168(%rsp)\n\t"
    "movq %rax,176(%rsp)\n\t"
    "movq %r11,184(%rsp)\n\t"
    "movdqu %xmm0,208(%rsp)\n\t"
    "movdqu %xmm1,224(%rsp)\n\t"
    "movdqu %xmm2,240(%rsp)\n\t"
    "movdqu %xmm3,256(%rsp)\n\t"
    "movdqu %xmm4,272(%rsp)\n\t"
    "movdqu %xmm5,288(%rsp)\n\t"
    "movdqu %xmm6,304(%rsp)\n\t"
    "movdqu %xmm7,320(%rsp)\n\t"
    "movq %r11,%rdi\n\t"                        /* enter(index, frame) -> t0 */
    "movq %rsp,%rsi\n\t"
    "call trace_imports_enter\n\t"
    "movq %rax,192(%rsp)\n\t"
    "movq 128(%rsp),%rdi\n\t"
    "movq 136(%rsp),%rsi\n\t"
    "movq 144(%rsp),%rdx\n\t"
    "movq 152(%rsp),%rcx\n\t"
    "movq 160(%rsp),%r8\n\t"
    "movq 168(%rsp),%r9\n\t"
    "movdqu 208(%rsp),%xmm0\n\t"
    "movdqu 224(%rsp),%xmm1\n\t"
    "movdqu 240(%rsp),%xmm2\n\t"
    "movdqu 256(%rsp),%xmm3\n\t"
    "movdqu 272(%rsp),%xmm4\n\t"
    "movdqu 288(%rsp),%xmm5\n\t"
    "movdqu 304(%rsp),%xmm6\n\t"
    "movdqu 320(%rsp),%xmm7\n\t"
    "movq 184(%rsp),%r11\n\t"
    "movq 176(%rsp),%rax\n\t"                   /* al: vector count of variadic callers */
    "movq trace_imports_targets(%rip),%r10\n\t"
    "movq (%r10,%r11,8),%r10\n\t"
    "call *%r10\n\t"                            /* returns here with rsp == C */
    "movq %rax,176(%rsp)\n\t"
    "movq %rdx,200(%rsp)\n\t"
    "movdqu %xmm0,208(%rsp)\n\t"
    "movdqu %xmm1,224(%rsp)\n\t"
    "movq 192(%rsp),%rdi\n\t"                   /* leave(t0, index, frame) */
    "movq 184(%rsp),%rsi\n\t"
    "movq %rsp,%rdx\n\t"
    "call trace_imports_leave\n\t"
    "movq 176(%rsp),%rax\n\t"
    "movq 200(%rsp),%rdx\n\t"
    "movdqu 208(%rsp),%xmm0\n\t"
    "movdqu 224(%rsp),%xmm1\n\t"
    "movq 344(%rsp),%r10\n\t"                   /* guest return address E */
    "addq $352,%rsp\n\t"                        /* rsp = E + 8, as after a guest ret */
    "jmp *%r10\n\t"
    ".globl trace_imports_common_end\n"
    "trace_imports_common_end:\n\t"
    ".text\n");

/* Stub i (32 bytes): mov r11d, i; movabs r10, common; jmp r10. */
static void write_stub(uint8_t *p, uint32_t index, uint64_t target) {
    p[0] = 0x41; p[1] = 0xBB; memcpy(p + 2, &index, 4);     /* mov r11d, imm32 */
    p[6] = 0x49; p[7] = 0xBA; memcpy(p + 8, &target, 8);    /* movabs r10, imm64 */
    p[16] = 0x41; p[17] = 0xFF; p[18] = 0xE2;               /* jmp r10 */
    memset(p + 19, 0xCC, THUNK_BYTES - 19);
}

static const char *const excluded_nids[] = {
    "FJrT5LuUBAU", /* pthread_exit */
    "3kg7rT0NQIs", /* scePthreadExit */
    "6Z83sYWFlA8", /* _exit */
    "uMei1W9uyNo", /* exit */
    "qdGFBoLVNKI", /* quick_exit */
    "L1SBTkC+Cvw", /* abort */
    "Ou3iL1abvng", /* __stack_chk_fail */
    "OMDRKKAZ8I4", /* sceKernelDebugRaiseException */
    "zE-wXIZjLoM", /* sceKernelDebugRaiseExceptionOnReleaseMode */
    "lKEN2IebgJ0", /* longjmp */
    "gNQ1V2vfXDE", /* setjmp */
    /* C++ exception unwinding and context switching: a throw/resume starting in these imports
     * unwinds the native stack, which has no .eh_frame entry for the thunk's common code, so the
     * unwinder would hit end-of-stack and terminate instead of finding the catch. Keep them direct. */
    "vkuuLfhnSZI", /* __cxa_throw */
    "ZL9FV4mJXxo", /* __cxa_rethrow */
    "f1zwJ3jAI2k", /* _Unwind_Resume */
    "usKbuvy2hQg", /* __cxa_call_unexpected */
    "zr094EQ39Ww", /* __cxa_pure_virtual */
    "qYhnoevd9bI", /* _ZSt9terminatev */
    "OjWstbIRPUo", /* __Ux86_64_setcontext */
};
static const char *const excluded_symbols[] = {
    "exit", "_exit", "quick_exit", "abort", "pthread_exit", "scePthreadExit", "thread_exit",
    "__stack_chk_fail", "setjmp", "_setjmp", "__sigsetjmp", "sigsetjmp", "longjmp", "_longjmp",
    "__longjmp_chk", "siglongjmp", "sceKernelDebugRaiseException",
    "sceKernelDebugRaiseExceptionOnReleaseMode",
};

/* Noreturn, stack-switching and returns-twice imports keep their host binding. */
static int is_excluded(const char *name) {
    size_t length = strcspn(name, "#");
    if (length >= 32) return 1;
    char nid[32];
    memcpy(nid, name, length);
    nid[length] = 0;
    for (size_t i = 0; i < sizeof(excluded_nids) / sizeof(*excluded_nids); ++i)
        if (!strcmp(nid, excluded_nids[i])) return 1;
    if (runtime_fiber_resolve && runtime_fiber_resolve(nid)) return 1;
    const char *symbol = runtime_import_name ? runtime_import_name(name) : NULL;
    if (!symbol) symbol = nid;
    for (size_t i = 0; i < sizeof(excluded_symbols) / sizeof(*excluded_symbols); ++i)
        if (!strcmp(symbol, excluded_symbols[i])) return 1;
    /* C++ EH / unwinding / context switch by symbol prefix, in case the NID is not in the list. */
    if (!strncmp(symbol, "__cxa", 5) || !strncmp(symbol, "_Unwind", 7) ||
        !strncmp(symbol, "_ZSt9terminate", 14) || !strncmp(symbol, "__Ux86_64_setcontext", 20))
        return 1;
    return !strncmp(symbol, "sceFiber", 8) || !strncmp(symbol, "_sceFiber", 9);
}

/* The readable symbol (or the NID when the table has no entry), kept for the process. */
static const char *label_for(const char *name) {
    const char *symbol = runtime_import_name ? runtime_import_name(name) : NULL;
    if (!symbol || strstr(symbol, "not resolved")) {
        size_t length = strcspn(name, "#");
        char *copy = malloc(length + 1);
        if (copy) { memcpy(copy, name, length); copy[length] = 0; }
        return copy;
    }
    size_t length = strlen(symbol);
    char *copy = malloc(length + 1);
    if (copy) memcpy(copy, symbol, length + 1);
    return copy;
}

static void bump(uint64_t *p, uint64_t v) {
    __atomic_store_n(p, __atomic_load_n(p, __ATOMIC_RELAXED) + v, __ATOMIC_RELAXED);
}
static void raise_max(uint64_t *p, uint64_t v) {
    if (v > __atomic_load_n(p, __ATOMIC_RELAXED)) __atomic_store_n(p, v, __ATOMIC_RELAXED);
}

static ThreadStats *thread_stats(void) {
    if (tls_stats) return tls_stats;
    ThreadStats *ts = calloc(1, sizeof(*ts));
    if (!ts) return NULL;
    ts->items = calloc(import_total ? import_total : 1, sizeof(ImportStat));
    if (!ts->items) { free(ts); return NULL; }
    EnterCriticalSection(&lock);
    ts->next = thread_list;
    thread_list = ts;
    LeaveCriticalSection(&lock);
    tls_stats = ts;
    return ts;
}

/* Called by the common code before the host function. Returns t0. */
IMPORT_ABI __attribute__((used)) uint64_t trace_imports_enter(uint64_t index, uint64_t frame) {
    ThreadStats *ts = thread_stats();
    /* Entries at or below this frame were abandoned by a longjmp out of their call: drop them. */
    while (shadow_depth && shadow[shadow_depth - 1].frame <= frame) --shadow_depth;
    if (shadow_depth < SHADOW_DEPTH) {
        shadow[shadow_depth].frame = frame;
        shadow[shadow_depth].index = (uint32_t)index;
        ++shadow_depth;
    } else {
        __atomic_fetch_add(&shadow_overflow, 1, __ATOMIC_RELAXED);
    }
    if (ts && index < import_total) bump(&ts->items[index].calls, 1);
    return trace_now();
}

/* Called by the common code after the host function returns; rsp is the frame. */
IMPORT_ABI __attribute__((used)) void trace_imports_leave(uint64_t t0, uint64_t index, uint64_t frame) {
    uint64_t t1 = trace_now();
    while (shadow_depth && shadow[shadow_depth - 1].frame < frame) --shadow_depth;
    if (shadow_depth && shadow[shadow_depth - 1].frame == frame) --shadow_depth;
    ThreadStats *ts = tls_stats;
    if (!ts || !t0 || index >= import_total) return;
    uint64_t dt = t1 > t0 ? t1 - t0 : 0;
    ImportStat *s = &ts->items[index];
    bump(&s->ticks, dt);
    raise_max(&s->max, dt);
    raise_max(&s->window_max, dt);
    if (trace_imports_mode == 2 && dt >= min_ns && labels[index]) {
        uint64_t guest = *(const uint64_t *)(uintptr_t)(frame + FRAME_BYTES);
        trace_push(EV_IMPORT, labels[index], t0, t1, 0, guest, (uint32_t)index);
    }
}

/* Text builder for the IMPORT_STATS chunk and the report line. */
typedef struct { char *p; size_t n, cap; int failed; } Text;
static void text_add(Text *t, const char *s, size_t n) {
    if (t->failed) return;
    if (t->n + n + 1 > t->cap) {
        size_t cap = (t->n + n + 1) * 2 + 256;
        char *p = realloc(t->p, cap);
        if (!p) { t->failed = 1; return; }
        t->p = p;
        t->cap = cap;
    }
    memcpy(t->p + t->n, s, n);
    t->n += n;
    t->p[t->n] = 0;
}
static void text_fmt(Text *t, const char *fmt, ...) {
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n > 0) text_add(t, tmp, (size_t)n < sizeof(tmp) ? (size_t)n : sizeof(tmp) - 1);
}
static void text_json_string(Text *t, const char *s) {
    text_add(t, "\"", 1);
    for (; *s; ++s) {
        if (*s == '"' || *s == '\\') text_add(t, "\\", 1);
        text_add(t, s, 1);
    }
    text_add(t, "\"", 1);
}

/* Sums every thread's counters per import (under lock). window_max is reset when asked. */
static void merge_locked(uint64_t *calls, uint64_t *ticks, uint64_t *max, uint64_t *window_max, int reset) {
    for (ThreadStats *ts = thread_list; ts; ts = ts->next) {
        for (uint32_t i = 0; i < import_total; ++i) {
            ImportStat *s = &ts->items[i];
            calls[i] += __atomic_load_n(&s->calls, __ATOMIC_RELAXED);
            ticks[i] += __atomic_load_n(&s->ticks, __ATOMIC_RELAXED);
            if (__atomic_load_n(&s->max, __ATOMIC_RELAXED) > max[i]) max[i] = __atomic_load_n(&s->max, __ATOMIC_RELAXED);
            if (__atomic_load_n(&s->window_max, __ATOMIC_RELAXED) > window_max[i]) window_max[i] = __atomic_load_n(&s->window_max, __ATOMIC_RELAXED);
            if (reset) __atomic_store_n(&s->window_max, 0, __ATOMIC_RELAXED);
        }
    }
}

/* IMPORT_STATS (chunk type 6): cumulative JSON array, the last chunk wins. Writer thread only. */
static const char *imports_chunk(size_t *len) {
    static Text out;
    uint64_t *calls = calloc(import_total ? import_total : 1, sizeof(*calls));
    uint64_t *ticks = calloc(import_total ? import_total : 1, sizeof(*ticks));
    uint64_t *max = calloc(import_total ? import_total : 1, sizeof(*max));
    uint64_t *wmax = calloc(import_total ? import_total : 1, sizeof(*wmax));
    out.n = 0;
    if (out.p) out.p[0] = 0;
    out.failed = 0;
    text_add(&out, "[", 1);
    if (calls && ticks && max && wmax) {
        EnterCriticalSection(&lock);
        merge_locked(calls, ticks, max, wmax, 0);
        LeaveCriticalSection(&lock);
        int first = 1;
        for (uint32_t i = 0; i < import_total; ++i) {
            if (!calls[i]) continue;
            text_fmt(&out, first ? "{\"index\": %u, \"name\": " : ", {\"index\": %u, \"name\": ", i);
            text_json_string(&out, labels[i] ? labels[i] : "?");
            text_fmt(&out, ", \"calls\": %llu, \"total_ns\": %llu, \"max_ns\": %llu}",
                     (unsigned long long)calls[i], (unsigned long long)ticks[i], (unsigned long long)max[i]);
            first = 0;
        }
    }
    text_add(&out, "]", 1);
    free(calls); free(ticks); free(max); free(wmax);
    *len = out.failed ? 0 : out.n;
    return out.p ? out.p : "[]";
}

int trace_imports_init(uint32_t import_count) {
    const char *env = getenv("SOTC_TRACE_IMPORTS");
    int mode = env && env[0] == '2' ? 2 : env && env[0] == '1' ? 1 : 0;
    if (!mode || !import_count || !trace_now()) { trace_imports_mode = 0; return 0; }
    const char *min_us = getenv("SOTC_TRACE_IMPORT_MIN_US");
    min_ns = (min_us && min_us[0] ? strtoull(min_us, NULL, 10) : 50) * 1000ull;

    size_t stubs = (size_t)import_count * THUNK_BYTES;
    size_t table = stubs;                       /* RUNTIME_FUNCTION (12 bytes) */
    size_t unwind = table + 12;                 /* UNWIND_INFO (4 bytes) */
    size_t total = (unwind + 4 + 4095) & ~(size_t)4095;
    uint8_t *mem = VirtualAlloc(NULL, total, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    uint64_t *targets = calloc(import_count, sizeof(*targets));
    const char **names = calloc(import_count, sizeof(*names));
    prev_calls = calloc(import_count, sizeof(*prev_calls));
    prev_ticks = calloc(import_count, sizeof(*prev_ticks));
    if (!mem || !targets || !names || !prev_calls || !prev_ticks) {
        if (mem) VirtualFree(mem, 0, MEM_RELEASE);
        free(targets); free(names); free(prev_calls); free(prev_ticks);
        prev_calls = prev_ticks = NULL;
        return 0;
    }
    uint64_t common = (uint64_t)(uintptr_t)trace_imports_common_begin;
    for (uint32_t i = 0; i < import_count; ++i) write_stub(mem + (size_t)i * THUNK_BYTES, i, common);

    /* Unwind data: one entry covering the stubs, no prolog and no codes, so a walker reads the
     * return address at [rsp]. That is exact at the stub's only instructions (the jumps). The
     * common code has no entry. A walker whose return address lies in trace_imports_common_begin
     * .. trace_imports_common_end is inside a thunk: the guest return address is at [sp + 344]
     * (FRAME_BYTES), and the callee's frame ends at sp = C. */
    RUNTIME_FUNCTION *rf = (RUNTIME_FUNCTION *)(void *)(mem + table);
    rf->BeginAddress = 0;
    rf->EndAddress = (DWORD)stubs;
    rf->UnwindData = (DWORD)unwind;
    static const uint8_t unwind_info[4] = {0x01, 0, 0, 0};   /* version 1, no prolog, no codes */
    memcpy(mem + unwind, unwind_info, sizeof(unwind_info));
    if (!RtlAddFunctionTable(rf, 1, (DWORD64)(uintptr_t)mem))
        fputs("trace imports: RtlAddFunctionTable failed (stub frames unwind as leaves anyway)\n", stderr);

    InitializeCriticalSection(&lock);
    area = mem;
    area_size = total;
    import_total = import_count;
    labels = names;
    trace_imports_targets = targets;
    trace_register_chunk(6, imports_chunk);
    trace_imports_mode = mode;
    return mode;
}

uint64_t trace_imports_wrap(uint32_t index, const char *name, uint64_t host_fn) {
    if (!trace_imports_mode || index >= import_total || !host_fn || !name) return host_fn;
    if (trace_imports_targets[index]) return (uint64_t)(uintptr_t)(area + (size_t)index * THUNK_BYTES);
    if (is_excluded(name)) return host_fn;
    labels[index] = label_for(name);
    trace_imports_targets[index] = host_fn;
    return (uint64_t)(uintptr_t)(area + (size_t)index * THUNK_BYTES);
}

void trace_imports_seal(void) {
    if (!trace_imports_mode || !area) return;
    DWORD old;
    if (!VirtualProtect(area, area_size, PAGE_EXECUTE_READ, &old)) {
        fputs("trace imports: VirtualProtect failed; imports are not thunked\n", stderr);
        trace_imports_mode = 0;
        return;
    }
    FlushInstructionCache(GetCurrentProcess(), area, area_size);
}

uint64_t trace_imports_caller(uint64_t builtin_ra) {
    uint64_t begin = (uint64_t)(uintptr_t)trace_imports_common_begin;
    uint64_t end = (uint64_t)(uintptr_t)trace_imports_common_end;
    if (!trace_imports_mode || builtin_ra < begin || builtin_ra >= end) return builtin_ra;
    /* Live frames lie above this function's frame; abandoned ones lie below it. A fiber that
     * switched away can leave an entry whose stack was freed, so only read the saved return
     * address when the frame is inside the calling thread's own stack (fiber stacks fall outside
     * and are skipped: attribution is lost, never a fault). */
    uint64_t here = (uint64_t)(uintptr_t)__builtin_frame_address(0);
    ULONG_PTR stack_low = 0, stack_high = 0;
    GetCurrentThreadStackLimits(&stack_low, &stack_high);
    for (uint32_t d = shadow_depth; d; --d) {
        const uint64_t frame = shadow[d - 1].frame;
        if (frame > here && frame >= stack_low && frame + FRAME_BYTES + 8 <= stack_high)
            return *(const uint64_t *)(uintptr_t)(frame + FRAME_BYTES);
    }
    return builtin_ra;
}

void trace_imports_thread_init(void) {
    if (trace_imports_mode) thread_stats();
}

void trace_imports_thunk_range(uint64_t *begin, uint64_t *end) {
    /* A stack walker whose return address lands in this range is inside a thunk; the guest return
     * address is at [frame top + FRAME_BYTES]. The sampler registers this with AddThunkRange. */
    if (begin) *begin = trace_imports_mode ? (uint64_t)(uintptr_t)trace_imports_common_begin : 0;
    if (end) *end = trace_imports_mode ? (uint64_t)(uintptr_t)trace_imports_common_end : 0;
}

void trace_imports_report(char *out, unsigned long size, double seconds) {
    if (!out || !size) return;
    out[0] = 0;
    if (!trace_imports_mode || !import_total) return;
    if (seconds <= 0) seconds = 1;
    uint64_t *calls = calloc(import_total, sizeof(*calls));
    uint64_t *ticks = calloc(import_total, sizeof(*ticks));
    uint64_t *max = calloc(import_total, sizeof(*max));
    uint64_t *wmax = calloc(import_total, sizeof(*wmax));
    if (!calls || !ticks || !max || !wmax) { free(calls); free(ticks); free(max); free(wmax); return; }
    EnterCriticalSection(&lock);
    merge_locked(calls, ticks, max, wmax, 1);
    /* Window = change since the last report; prev_* hold the last totals. */
    for (uint32_t i = 0; i < import_total; ++i) {
        uint64_t dc = calls[i] - prev_calls[i], dt = ticks[i] - prev_ticks[i];
        prev_calls[i] = calls[i];
        prev_ticks[i] = ticks[i];
        calls[i] = dc;
        ticks[i] = dt;
    }
    LeaveCriticalSection(&lock);
    Text text = {0};
    text_add(&text, "SOTC IMPORTS: ", 14);
    int shown = 0;
    for (int rank = 0; rank < 10; ++rank) {
        uint32_t best = import_total;
        for (uint32_t i = 0; i < import_total; ++i)
            if (ticks[i] && (best == import_total || ticks[i] > ticks[best])) best = i;
        if (best == import_total) break;
        text_fmt(&text, "%s%s %.0f calls/s %.2f ms/s max %.2f ms", shown ? "; " : "",
                 labels[best] ? labels[best] : "?", (double)calls[best] / seconds,
                 (double)ticks[best] / 1e6 / seconds, (double)wmax[best] / 1e6);
        ticks[best] = 0;
        ++shown;
    }
    if (!shown) text_add(&text, "none", 4);
    if (!text.failed && text.p) {
        size_t n = text.n < size - 1 ? text.n : size - 1;
        memcpy(out, text.p, n);
        out[n] = 0;
    }
    free(text.p);
    free(calls); free(ticks); free(max); free(wmax);
}

#else /* !_WIN32: the thunks are Windows-only; everything reports off. */

int trace_imports_init(uint32_t import_count) { (void)import_count; return 0; }
uint64_t trace_imports_wrap(uint32_t index, const char *name, uint64_t host_fn) {
    (void)index; (void)name;
    return host_fn;
}
void trace_imports_seal(void) {}
uint64_t trace_imports_caller(uint64_t builtin_ra) { return builtin_ra; }
void trace_imports_thread_init(void) {}
void trace_imports_thunk_range(uint64_t *begin, uint64_t *end) {
    if (begin) *begin = 0;
    if (end) *end = 0;
}
void trace_imports_report(char *out, unsigned long size, double seconds) {
    (void)seconds;
    if (out && size) out[0] = 0;
}
#endif
