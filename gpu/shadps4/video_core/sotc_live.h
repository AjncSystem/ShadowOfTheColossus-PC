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
/// (0..100, neutral 0; SOTC_HDR_BLOWOUT), hue correction of the hue-preserving tone mapper
/// (0..100, default 100; SOTC_HDR_HUE_CORRECTION), flare compensation (0..100, neutral 0;
/// SOTC_HDR_FLARE), gamma correction (0 or 100, neutral 0; SOTC_HDR_GAMMA_CORRECTION) and
/// highlight saturation (0..100, neutral 50; SOTC_HDR_HIGHLIGHT_SATURATION), film grain (0..100,
/// neutral 0; SOTC_HDR_GRAIN; SDR too) and gamut expansion (0..100, neutral 0;
/// SOTC_HDR_GAMUT_EXPANSION). Gamma correction: 0 off, 100 gamma 2.2, 200 gamma 2.4 (BT.1886).
/// Exposure to highlight saturation and grain also work in SDR. kGradeUnset: the value read at
/// start.
inline constexpr float kGradeUnset = -1000.f;
enum HdrGrade {
    kExposure,
    kContrast,
    kShadows,
    kHighlights,
    kSaturation,
    kBlowout,
    kHueCorrection,
    kFlare,
    kGammaCorrection,
    kHighlightSaturation,
    kGrain,
    kGamutExpansion,
    kGradeCount
};
inline std::atomic<float> hdr_grade[kGradeCount]{kGradeUnset, kGradeUnset, kGradeUnset,
                                                 kGradeUnset, kGradeUnset, kGradeUnset,
                                                 kGradeUnset, kGradeUnset, kGradeUnset,
                                                 kGradeUnset, kGradeUnset, kGradeUnset};

/// Game HDR tone mapper (sotc_tonemap.h Mode: 0 vanilla, 1 vanilla+, 2 none, 3 vanilla+ hue
/// preserving, 4 RenoDRT; SOTC_HDR_TONEMAP)
/// and the strength of the game's colour grading in percent (SOTC_HDR_GRADING, 100 = the
/// game's). -1: the value read at start.
inline std::atomic<int> hdr_tonemap{-1};
inline std::atomic<float> hdr_grading{-1.f};

/// Effect strengths in percent, 100 = the game's (SOTC_FX_BLOOM, SOTC_FX_VIGNETTE, SOTC_FX_FOG;
/// launcher fx_bloom_strength, fx_vignette_strength, fx_fog_strength). -1: the value read at
/// start.
inline std::atomic<float> fx_bloom{-1.f};
inline std::atomic<float> fx_vignette{-1.f};
inline std::atomic<float> fx_fog{-1.f};
inline std::atomic<float> fx_motion_blur{-1.f};

/// F3: the game's own picture (tone mapper, grading, picture controls and effect strengths at
/// the game's), brightness and peak kept, to compare with the user's settings.
inline std::atomic<bool> compare_vanilla{false};

/// RCAS after DLSS / FSR 4 (SOTC_DLSS_SHARPEN; launcher dlss_sharpen): attenuation in stops, 0 =
/// sharpest; kSharpenOff: none. kSharpenUnset: the value read at start.
inline constexpr float kSharpenUnset = -2.f;
inline constexpr float kSharpenOff = -1.f;
inline std::atomic<float> dlss_sharpen{kSharpenUnset};

} // namespace SotcLive
