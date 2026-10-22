// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: the session event log (src/runtime_event.h) from the GPU library: settings changed in the
// in-game menu, keys, the mouse, loads, texture levels. Weak: executables without the runtime
// (bb-gpu-capabilities) have no event log and the calls do nothing there.
#pragma once

#include <cstdarg>
#include <cstdio>

extern "C" __attribute__((weak)) void sotc_event(const char* category, const char* format, ...);

namespace SotcEvent {

template <typename... Args>
inline void Log(const char* category, const char* format, Args... args) {
    if (sotc_event) {
        sotc_event(category, format, args...);
    }
}

} // namespace SotcEvent
