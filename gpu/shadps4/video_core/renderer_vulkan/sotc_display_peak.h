// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: the HDR display's peak brightness as Windows reports it (DXGI), for the game HDR roll-off.
#pragma once

namespace Vulkan {

/// MaxLuminance of the display the game window is on (`native_window`, an HWND), in nits, when
/// that output has HDR on (Rec.2020 PQ); 0 when it reports none (HDR off there, not Windows, or
/// the driver gives no value). Without a window: the highest among the HDR outputs. Cached: DXGI
/// is only asked again after SotcDisplayPeakChanged, so it can be called every frame.
float SotcDisplayPeakNits(void* native_window);

/// The window moved to another display or the display's HDR changed (SDL window events, from the
/// event thread): the next SotcDisplayPeakNits asks Windows again.
void SotcDisplayPeakChanged();

} // namespace Vulkan
