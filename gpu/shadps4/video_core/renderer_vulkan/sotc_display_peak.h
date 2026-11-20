// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: the HDR display's peak brightness as Windows reports it (DXGI), for the game HDR roll-off.
#pragma once

namespace Vulkan {

/// The highest MaxLuminance among the outputs with HDR on (Rec.2020 PQ), in nits; 0 when none
/// reports one (HDR off, not Windows, or the driver gives no value). Called once at start.
float SotcDisplayPeakNits();

} // namespace Vulkan
