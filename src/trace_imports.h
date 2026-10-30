/* sotc: SOTC_TRACE_IMPORTS, timing of every PS4 system call the game makes (trace v2, phase 3).
 *
 * At bind time (src/probe.c), each function import slot points at a small per-import thunk instead
 * of the host function. The thunk times the call with the trace clock and adds it to per-thread
 * per-import counters; with SOTC_TRACE_IMPORTS=2 a call of at least SOTC_TRACE_IMPORT_MIN_US
 * (default 50) also becomes an EvImport span. Unset: no thunk exists and the slots are bound as
 * before. Counters reach the trace as IMPORT_STATS chunks and a 5 s "SOTC IMPORTS" stderr line.
 *
 * The thunk pops the guest return address into a per-fiber shadow stack and calls the host
 * function, so inside the host function __builtin_return_address(0) is the thunk, not the game.
 * GUEST_CALLER() gives the game's return address in both cases. */
#ifndef SOTC_TRACE_IMPORTS_H
#define SOTC_TRACE_IMPORTS_H
#include <stdint.h>

/* 0 off, 1 counters, 2 counters and spans. Set once before binding (trace_imports_init). */
extern int trace_imports_mode;

/* Reads SOTC_TRACE_IMPORTS / SOTC_TRACE_IMPORT_MIN_US; allocates the thunk area when on. Returns
 * the mode. Called once by the loader before the import slots are written. */
int trace_imports_init(uint32_t import_count);

/* The value to store in an import slot: the host function itself when off or when the import is
 * excluded (data imports, noreturn, fiber switches, setjmp-like), else the thunk for `index`.
 * name: the import's "NID#lib#mod" string (for exclusion and labelling). */
uint64_t trace_imports_wrap(uint32_t index, const char *name, uint64_t host_fn);

/* Called once after every import slot is written: makes the thunk area executable (no-op when
 * off). Added to the contract in P3 for src/probe.c. */
void trace_imports_seal(void);

/* The guest caller of the current host function: `builtin_ra` (its __builtin_return_address(0))
 * when it is not inside the thunk area, else the saved guest return address of the calling frame. */
uint64_t trace_imports_caller(uint64_t builtin_ra);

/* The [begin, end) code range of the thunk common code (0, 0 when off). A stack walker that meets
 * a return address inside it is in a thunk frame; the guest return address is at [frame + 344]. */
void trace_imports_thunk_range(uint64_t *begin, uint64_t *end);

/* Called by every thread/fiber that may run guest code before its first import call is fine
 * lazily; exposed for the thread start path to pre-create the per-thread state. */
void trace_imports_thread_init(void);

/* One "SOTC IMPORTS" line (top imports by time in the window), for the 5 s report; resets the
 * window. Empty when off. */
void trace_imports_report(char *out, unsigned long size, double seconds);

#define GUEST_CALLER() \
    (trace_imports_mode ? trace_imports_caller((uint64_t)__builtin_return_address(0)) \
                        : (uint64_t)__builtin_return_address(0))
#endif
