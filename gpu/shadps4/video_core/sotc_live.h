// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: settings the in-game menu changes while playing (bbport_overlay.cpp, "Quick settings").
// -1: the value from the environment at start (the launcher's choice); else the menu's.
#pragma once

#include <atomic>

namespace SotcLive {

/// High quality textures: how many reports a texture bank stays sampled (SOTC_LOD_LINGER).
inline std::atomic<int> lod_linger{-1};

/// Texture level "Maximum" turns the page pool switch off (SOTC_LOD_POOL_LOW): its threshold in
/// percent, 0 off.
inline std::atomic<int> lod_pool_low{-1};

/// Upscaler: 1 FSR 1, 0 bilinear (BB_FSR1). RCAS sharpening: attenuation in thousandths (0 high,
/// 250 medium, 600 low), -2 off (BB_RCAS=0). -1: the launcher's at start.
inline std::atomic<int> fsr1{-1};
inline std::atomic<int> rcas{-1};

/// Far sun shadow cascades drawn on alternate frames (SOTC_SHADOW_REUSE): 0 off, 1 on.
inline std::atomic<int> shadow_reuse{-1};

} // namespace SotcLive
