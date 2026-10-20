// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: settings the in-game menu changes while playing (bbport_overlay.cpp, "Quick settings").
// -1: the value from the environment at start (the launcher's choice); else the menu's.
#pragma once

#include <atomic>

namespace SotcLive {

/// High quality textures: how many reports a texture bank stays sampled (SOTC_LOD_LINGER).
inline std::atomic<int> lod_linger{-1};

/// Far sun shadow cascades drawn on alternate frames (SOTC_SHADOW_REUSE): 0 off, 1 on.
inline std::atomic<int> shadow_reuse{-1};

} // namespace SotcLive
