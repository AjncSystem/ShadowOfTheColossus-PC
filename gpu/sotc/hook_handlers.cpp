// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: the one place that registers every engine hook handler (gpu/sotc/hooks.h). Each probe
// file exposes a Register<Name>() function; it is listed here because the GPU library is
// static and unreferenced objects would not be linked.
#include "hooks.h"
#include "probes.h"

namespace SotcHooks {

void RegisterAllHandlers() {
    // Engine probes (gpu/sotc/probe_*.cpp). Each registers the handlers of its own hooks; a
    // hook whose switch is off installs nothing, so a registration here is harmless when off.
    SotcProbes::RegisterXpr();
    SotcProbes::RegisterLoader();
    SotcProbes::RegisterAlloc();
    SotcProbes::RegisterCvars();
    SotcProbes::RegisterArea();
}

} // namespace SotcHooks
