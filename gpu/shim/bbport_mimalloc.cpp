// sotc: BB_MIMALLOC=ON (CMake option, off by default): the C++ operator new/delete of bb-probe go
// to mimalloc instead of the ucrt heap (malloc/free were 7-14% of the draw thread). Only new and
// delete are replaced: C malloc/free (the C runtime, FFmpeg, SDL, the Vulkan driver) keep the ucrt
// heap, so memory never crosses allocators. This needs libc++ linked statically (BB_STATIC_LIBCXX):
// with libc++.dll, its out-of-line code would free with the ucrt what mimalloc allocated.
// This file is compiled into the executable itself, so its operators win over libc++'s.
#ifdef BB_MIMALLOC
#include <mimalloc-new-delete.h>
#endif
