// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: guest data layout used by the SotC Lab (docs/sotc/LAB.md). Code addresses never live here:
// hooks are found by signature (scripts/hooks.d/enginecmd.py, cam_probe.py, freecam.py). This file
// holds only the struct offsets that reverse engineering established (docs/re-stutters/
// engine-commands.md, docs/re-stutters/freecam-re.md). A value of kUnknown means "not established":
// the feature that needs it stays off, it is never guessed.
#pragma once

#include <cstdint>

namespace SotcLab::Layout {

inline constexpr int64_t kUnknown = -1;

// --- Player (Wander) ---------------------------------------------------------------------------
/// Offset of the world position (float x, y, z) inside the player object that the FC_WANDER /
/// CAM_PROBE hooks receive.
inline constexpr int64_t kPlayerPos = kUnknown;
/// Offset of the velocity (float x, y, z) inside the player object.
inline constexpr int64_t kPlayerVel = kUnknown;
/// Offset of a visibility / render flag of the player object (hide Wander in free-cam).
inline constexpr int64_t kPlayerHidden = kUnknown;
// Established (docs/re-stutters/freecam-re.md, section 2, observed in the decompilation of sub_0xb0
// and sub_0x660fc0): the player object's transform T = sub_660fc0(obj) (type byte at obj+0xc; types
// 9, 13, 22 give obj+0x120, type 10 gives obj+0x40). T+0x150 is a pointer to the position float[3],
// T+0x160 a pointer to the rotation quaternion float[4]. Not yet a kPlayerPos: the object type of
// Wander and the physics step that writes it are not established.
inline constexpr int64_t kPlayerXformTypeOffset = 0xc;
inline constexpr int64_t kXformPosPtrOffset = 0x150;
inline constexpr int64_t kXformRotPtrOffset = 0x160;

// --- Camera ------------------------------------------------------------------------------------
/// Offset of the camera position (float x, y, z) inside the GameCamera object (FC_CAM hook).
inline constexpr int64_t kCamPos = kUnknown;
/// Offset of the camera orientation inside the GameCamera object. kCamRotIsMatrix: a row-major
/// 3x3 (or 3x4) rotation; otherwise yaw/pitch floats.
inline constexpr int64_t kCamRot = kUnknown;
inline constexpr bool kCamRotIsMatrix = true;
/// Stride in bytes between matrix rows when kCamRotIsMatrix (12 for 3x3 floats, 16 for 4x4).
inline constexpr int64_t kCamRotRowStride = 16;

// --- Engine command executor -------------------------------------------------------------------
/// See docs/re-stutters/engine-commands.md. Offsets used by engine_cmd.cpp when it builds the
/// engine's command/queue objects; kUnknown keeps live engine commands off.
inline constexpr int64_t kCmdQueueText = kUnknown;

} // namespace SotcLab::Layout
