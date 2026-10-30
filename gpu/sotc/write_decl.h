// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: SOTC_WRITE_DECL statistics (write_decl.cpp). The declared spans are counted per guest
// caller (the return address of the memcpy/memset), with the write-protected pages they hold. Under
// SOTC_FPS=1 a write fault on a page a declare settled (mode 1) or would settle (mode 2) a moment
// ago is counted, and the write faults of SOTC_FPS (0b) are counted by the code that faulted. All
// counters are lock-free; Report takes and resets them (driver.cpp 5 s report, under SOTC_FPS=1).
#pragma once

#include <cstdint>
#include <string>

namespace SotcWriteDecl {

/// SOTC_WRITE_DECL, read once: 1 declare, 2 count only, 0 (unset or any other value) off.
uint32_t Mode();

/// SOTC_WRITE_DECL_MIN, read once: spans smaller than this many bytes are not declared (default
/// 256; a value with no digits counts as the default, as in src/runtime.c).
uint64_t MinBytes();

/// SOTC_WRITE_DECL_MAX, read once: spans larger than this many bytes are not declared (default 1
/// MiB; 0 = no limit; a value with no digits counts as the default).
uint64_t MaxBytes();

/// One call of bbgpu_declare_cpu_write: the guest caller (return address), the span size and the
/// write-protected pages found in it (0 when the span was not checked: the pre-check refused it,
/// or it is over SOTC_WRITE_DECL_MAX).
void NoteCall(uint64_t return_address, uint64_t size, uint64_t pages);

/// SOTC_FPS only: the run [addr, addr + size) was settled by a declare now (mode 1), or would be
/// (mode 2). The pages are stamped for the verify count of write faults.
void NoteDeclared(uint64_t addr, uint64_t size);

/// SOTC_FPS only (0b): a guest write fault on addr, taken at the instruction rip. Counted by the
/// class of rip: the eboot code (the BPE heap allocator range apart, when the hook resolves), a
/// host module, or other.
void NoteWriteFault(uint64_t rip, uint64_t addr);

/// Appends the lines of the last window to out and resets it. seconds = the window length.
/// The SOTC WRITE DECL lines print when SOTC_WRITE_DECL is on; the verify count and the SOTC WRITE
/// FAULTS line need SOTC_FPS=1. driver.cpp calls this inside its SOTC_FPS report block, so the
/// whole report needs SOTC_FPS=1.
void Report(std::string& out, double seconds);

} // namespace SotcWriteDecl
