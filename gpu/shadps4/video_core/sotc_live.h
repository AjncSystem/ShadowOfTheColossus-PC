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

/// Grass multipliers (SOTC_GRASS): grass distance, grass height, weed distance, weed height,
/// grass detail. Below 0: the environment's.
inline std::atomic<float> grass[5]{-1.f, -1.f, -1.f, -1.f, -1.f};

/// Game HDR levels in nits (vk_presenter.cpp): the scene's paper white (SOTC_HDR_WHITE), the
/// display's peak (SOTC_HDR_PEAK, 0 = the value Windows reports) and the UI's white
/// (SOTC_HDR_UI, 0 = the paper white). Below 0: the value read at start.
inline std::atomic<float> hdr_white{-1.f};
inline std::atomic<float> hdr_peak{-1.f};
inline std::atomic<float> hdr_ui{-1.f};

/// Game HDR picture controls (vk_presenter.cpp), in the menu's units: exposure in EV (-3..3,
/// neutral 0; SOTC_HDR_EXPOSURE), then contrast, shadows, highlights, saturation (0..100, neutral
/// 50; SOTC_HDR_CONTRAST / _SHADOWS / _HIGHLIGHTS / _SATURATION) and highlight saturation loss
/// (0..100, neutral 0; SOTC_HDR_BLOWOUT). kGradeUnset: the value read at start.
inline constexpr float kGradeUnset = -1000.f;
enum HdrGrade { kExposure, kContrast, kShadows, kHighlights, kSaturation, kBlowout, kGradeCount };
inline std::atomic<float> hdr_grade[kGradeCount]{kGradeUnset, kGradeUnset, kGradeUnset,
                                                 kGradeUnset, kGradeUnset, kGradeUnset};

} // namespace SotcLive
