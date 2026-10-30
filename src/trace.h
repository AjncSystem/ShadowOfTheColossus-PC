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
#include <stddef.h>
#include <stdint.h>

uint64_t bbgpu_trace_now(void) __attribute__((weak));
void bbgpu_trace_span(const char *name, uint64_t t0, uint64_t a, uint64_t b) __attribute__((weak));
void bbgpu_trace_image(uint64_t base, uint64_t size) __attribute__((weak));
/* Creates the calling thread's ring ahead of time, so the fault handler never allocates one. */
void bbgpu_trace_thread_init(void) __attribute__((weak));
/* Any event kind on the calling thread's ring (the recorder's PushEvent). */
void bbgpu_trace_push(uint8_t kind, const char *name, uint64_t t0, uint64_t t1, uint64_t a,
                      uint64_t b, uint32_t aux) __attribute__((weak));
/* An extra file chunk of `type`: the provider returns a pointer and length, valid until its next
 * call; the writer copies it (called on the writer thread, see sotc_trace.h). */
void bbgpu_trace_register_chunk(uint32_t type, const char *(*provider)(size_t *len))
    __attribute__((weak));

static inline void trace_thread_init(void) { if (bbgpu_trace_thread_init) bbgpu_trace_thread_init(); }

static inline uint64_t trace_now(void) { return bbgpu_trace_now ? bbgpu_trace_now() : 0; }
static inline void trace_span(const char *name, uint64_t t0, uint64_t a, uint64_t b) {
    if (t0 && bbgpu_trace_span) bbgpu_trace_span(name, t0, a, b);
}
static inline void trace_push(uint8_t kind, const char *name, uint64_t t0, uint64_t t1, uint64_t a,
                              uint64_t b, uint32_t aux) {
    if (bbgpu_trace_push) bbgpu_trace_push(kind, name, t0, t1, a, b, aux);
}
static inline void trace_register_chunk(uint32_t type, const char *(*provider)(size_t *len)) {
    if (bbgpu_trace_register_chunk) bbgpu_trace_register_chunk(type, provider);
}
#endif
