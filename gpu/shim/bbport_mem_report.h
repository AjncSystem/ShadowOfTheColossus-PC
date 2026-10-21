// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: where the process's memory is, for the SOTC_FPS report ("SOTC MEM" line, every minute):
// committed bytes in the process heaps (the C/C++ allocations) and the other private memory
// (driver allocations, stacks) by allocation size. Diagnostic only; Windows only.
#pragma once

#include <string>

namespace BbMemReport {
/// One line without the "SOTC MEM " prefix; empty where it is not supported.
std::string Line();
}
