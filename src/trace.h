/* sotc: SOTC_TRACE flight recorder from the loader's C runtime (gpu/shadps4/video_core/sotc_trace.h).
 * The GPU library is bound weakly: builds without it (the Linux C tests) record nothing.
 *
 *     uint64_t t0 = trace_now();             // 0 while the recorder is off: one weak call
 *     ... wait ...
 *     trace_span("evf wait", t0, a, (uint64_t)__builtin_return_address(0));
 *
 * Names must be string literals; only the pointer is kept. */
#ifndef SOTC_TRACE_C_H
#define SOTC_TRACE_C_H
#include <stdint.h>

uint64_t bbgpu_trace_now(void) __attribute__((weak));
void bbgpu_trace_span(const char *name, uint64_t t0, uint64_t a, uint64_t b) __attribute__((weak));
void bbgpu_trace_image(uint64_t base, uint64_t size) __attribute__((weak));
/* Creates the calling thread's ring ahead of time, so the fault handler never allocates one. */
void bbgpu_trace_thread_init(void) __attribute__((weak));

static inline void trace_thread_init(void) { if (bbgpu_trace_thread_init) bbgpu_trace_thread_init(); }

static inline uint64_t trace_now(void) { return bbgpu_trace_now ? bbgpu_trace_now() : 0; }
static inline void trace_span(const char *name, uint64_t t0, uint64_t a, uint64_t b) {
    if (t0 && bbgpu_trace_span) bbgpu_trace_span(name, t0, a, b);
}
#endif
