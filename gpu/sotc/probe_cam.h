// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: camera and Wander probes for the SotC Lab (docs/re-stutters/freecam-re.md). Observe only.
#pragma once

namespace SotcProbes {

/// Registers the handlers of the observe-only camera and Wander hooks (PLAYER_XFORM, switch
/// SOTC_CAM_PROBE; scripts/hooks.d/cam_probe.py). Called from SotcFreecam::Register().
void RegisterCamProbe();

} // namespace SotcProbes
