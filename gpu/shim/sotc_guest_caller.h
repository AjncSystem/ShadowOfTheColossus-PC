// sotc: guest return address for the SOTC trace (SOTC_TRACE_IMPORTS, src/trace_imports.h).
// With the switch on, an import slot points at a thunk, so __builtin_return_address(0) inside a
// host function the game calls is the thunk; trace_imports_caller() maps it back to the game's
// call site. Off, or in builds without src/ (tests), the value is exactly __builtin_return_address(0).
#ifndef SOTC_GUEST_CALLER_H
#define SOTC_GUEST_CALLER_H
#include <cstdint>

// Weak: the GPU library is also linked into tests that have no src/ (no trace_imports.c).
extern "C" int trace_imports_mode __attribute__((weak));
extern "C" uint64_t trace_imports_caller(uint64_t builtin_ra) __attribute__((weak));

// Use SOTC_GUEST_CALLER() instead of calling this directly.
inline uint64_t SotcGuestCaller(void* builtin_ra) {
    const uint64_t ra = reinterpret_cast<uint64_t>(builtin_ra);
    if (&trace_imports_mode && trace_imports_mode && &trace_imports_caller) {
        return trace_imports_caller(ra);
    }
    return ra;
}

// Expand only in the body of the host function that the import slot calls directly.
#define SOTC_GUEST_CALLER() ::SotcGuestCaller(__builtin_return_address(0))

#endif
