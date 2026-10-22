// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: where the process's memory is, for the SOTC_FPS report ("SOTC MEM" line, every minute):
// committed bytes in the process heaps (the C/C++ allocations) and the other private memory
// (driver allocations, stacks) by allocation size. Diagnostic only; Windows only.
#pragma once

#include <string>

namespace BbMemReport {
/// One line without the "SOTC MEM " prefix; empty where it is not supported.
std::string Line();

/// SOTC_WS_DIAG=1: buffer upload copies check how many of their source (guest backing view) and
/// destination (staging) pages are outside the working set before copying, i.e. will fault.
bool WsDiagEnabled();
void WsDiagCopy(const void* source, const void* guest, const void* dest, size_t size);
void WsDiagCopyDone(uint64_t ns_before, size_t size);
/// The window's line without the "SOTC WS " prefix (also resets it); empty when disabled.
std::string WsDiagLine();
}
