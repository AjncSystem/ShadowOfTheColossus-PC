// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: the game's tonemap (vk_rasterizer.cpp kTonemapHash) and the HDR tone mapper choice.
//
// The tonemap pass, before the HDR composite: scene (+ fog, depth of field, bloom) -> vignette ->
// tint (flatbuf dwords 37..39) -> saturation (35) -> contrast (40) -> curve (mode in dword 20;
// the game uses 5, x (A x + B) / (x (C x + D) + E) with A..E in dwords 28..32, then min dword 33,
// 300) -> gamma (36) and scale (44) -> per channel gamma (45..47), min 10 (a constant in the
// shader) -> lift (41..43) to gain (48..50) -> / scale. So the pass outputs at most 10 / scale
// (18 with the game's 0.556).
//
// The tone mapper choice only changes A..E, so it works on every upscaler path: the pass stays
// the game's own, only its curve differs.
#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <limits>
#include <vector>

namespace SotcTonemap {

enum Mode : int {
    kVanilla = 0,     ///< the game's curve (flattens towards A / C = 3.3)
    kVanillaPlus = 1, ///< the same toe and mid tones, highlights rising towards kPlusCeiling
    kNone = 2,        ///< a straight line through the game's curve at mid grey
    /// Vanilla+ on luminance instead of per channel, so hues hold as they brighten: the game's
    /// pass gets kNone's line and the final pass (post_process.frag hueCurve) applies Vanilla+'s
    /// curve to the luminance, the hue pulled back towards the per-channel one by the hue
    /// correction control.
    kVanillaPlusHue = 3,
    /// RenoDRT-style (RenoDX's): kNone's line in the game's pass; the final pass puts the game's
    /// own curve back on luminance below mid grey (the game's shadows and mid tones), the line
    /// above it (highlights untonemapped), then a peak-aware roll-off on luminance (BT.2390 in
    /// PQ), so hues and saturation hold up to the display's peak. The hue correction control
    /// takes the hue towards the per-channel curves'.
    kRenoDrt = 4,
    kModeCount = 5,
};

/// Set by the presenter every frame (vk_presenter.cpp RefreshHdrLevels): the tone mapper wanted
/// (vanilla with game HDR off) and the strength of the game's colour grading, 0..1.
inline std::atomic<int> wanted{kVanilla};
inline std::atomic<float> grading{1.0f};

/// Effect strengths (presenter RefreshEffectLevels, every frame), 1 = the game's: the bloom the
/// tonemap adds (its img6; the first bloom upsample's scale, kBloomUpsampleHash dword 24), the
/// vignette (tonemap dword 54) and the volumetric fog (the distance into the fog volume,
/// kFogApplyHash dword 31).
inline std::atomic<float> bloom{1.0f};
inline std::atomic<float> vignette{1.0f};
inline std::atomic<float> fog{1.0f};
/// Motion blur length (kMotionBlurHash dwords 40 and 41), 1 = the game's.
inline std::atomic<float> motion_blur{1.0f};

/// kVanillaPlusHue: set at the tonemap draw (rasterizer) for the final pass: on when this frame's
/// tonemap got kNone's line for it, and Vanilla+'s A..E (the curve the final pass applies).
inline std::atomic<bool> hue_curve_on{false};
inline std::atomic<float> hue_curve[5]{};
/// kRenoDrt: set at the tonemap draw: on when this frame's tonemap got kNone's line for it
/// (hue_curve then holds the game's own A..E).
inline std::atomic<bool> drt_on{false};

/// Set at the tonemap draw: the most this frame's pass can output (its curve's limit and both
/// caps, through its gamma and gain), in the HDR composite's units before its exponent; 0 unknown.
/// The presenter turns it into the roll-off's source peak in nits.
inline std::atomic<float> source_limit{0.0f};

/// The pass's output cap for the tone mappers that draw kNone's line (kNone, kVanillaPlusHue,
/// kRenoDrt), in place of 10 / scale = 18: the line's highlights were clipped there per channel
/// (the sun, sky and water flat and their hues shifted) before the final pass's peak-aware curve
/// ever saw them. 50 is past PQ's 10000 nits at any paper white from 67 nits.
inline constexpr float kRaisedLimit = 50.0f;

/// Raises the pass's caps to kRaisedLimit in a copy of its flatbuf. The min 10 is a constant in
/// the shader but sits between `* scale` and `/ scale`: a smaller scale lifts it and, with
/// per-channel gamma 1 and lift 0 (the game's values), leaves everything below it unchanged.
inline bool RaiseCaps(std::vector<unsigned>& flat) {
    if (flat.size() <= 50) {
        return false;
    }
    const auto get = [&](unsigned d) { return std::bit_cast<float>(flat[d]); };
    const auto put = [&](unsigned d, float v) { flat[d] = std::bit_cast<unsigned>(v); };
    for (unsigned d = 45; d <= 47; ++d) {
        if (get(d) != 1.0f) {
            return false;
        }
    }
    for (unsigned d = 41; d <= 43; ++d) {
        if (get(d) != 0.0f) {
            return false;
        }
    }
    const float scale = get(44), gain = std::max({get(48), get(49), get(50)});
    if (!(scale > 0.0f) || !(gain > 0.0f)) {
        return false;
    }
    put(44, std::min(scale, 10.0f * gain / kRaisedLimit));
    put(33, std::max(get(33), 1.0e4f)); // the curve's own cap (300)
    return true;
}

/// The most the pass outputs with these constants (see source_limit). Tint, saturation and
/// contrast are left out (the game's tint lowers it).
inline float OutputLimit(const unsigned* flat, size_t size) {
    if (size <= 50 || flat[20] != 5u) {
        return 0.0f;
    }
    const auto get = [&](unsigned d) { return std::bit_cast<float>(flat[d]); };
    const float A = get(28), C = get(30);
    float lim = C > 0.0f ? A / C : std::numeric_limits<float>::infinity();
    lim = std::min(lim, get(33));
    const float scale = get(44), gain = std::max({get(48), get(49), get(50)});
    if (!(scale > 0.0f) || !std::isfinite(lim)) {
        return 0.0f;
    }
    lim = std::min(10.0f, scale * std::pow(lim, get(36))) * gain / scale;
    return std::isfinite(lim) ? lim : 0.0f;
}

/// Vanilla+'s ceiling (the curve's limit A / C). With the game's scale 0.556 it stays under the
/// pass's min 10 cap (16 * 0.556 = 8.9), so the final pass's roll-off shapes the highlights.
inline constexpr float kPlusCeiling = 16.0f;

/// The curve of `mode` in place of the game's, in a copy of the tonemap's flatbuf. Only for the
/// game's curve mode 5; returns false (flatbuf unchanged) otherwise. kVanillaPlusHue writes
/// kNone's line and gives Vanilla+'s A..E in `plus`.
inline bool ApplyCurve(std::vector<unsigned>& flat, int mode, float* plus = nullptr) {
    if (mode == kVanilla || flat.size() <= 32 || flat[20] != 5u) {
        return false;
    }
    const auto get = [&](unsigned d) { return std::bit_cast<float>(flat[d]); };
    const auto put = [&](unsigned d, float v) { flat[d] = std::bit_cast<unsigned>(v); };
    const double A = get(28), B = get(29), C = get(30), D = get(31), E = get(32);
    const auto f = [&](double x) { return x * (A * x + B) / (x * (C * x + D) + E); };
    if (!(E > 0.0) || !(B > 0.0)) {
        return false;
    }
    const auto line = [&] {
        // y = x * f(0.18) / 0.18: A = C = D = 0, B / E the slope.
        put(28, 0.0f);
        put(29, static_cast<float>(E * f(0.18) / 0.18));
        put(30, 0.0f);
        put(31, 0.0f);
    };
    if (mode == kNone) {
        line();
        return true;
    }
    if (mode == kRenoDrt) {
        if (!plus) {
            return false;
        }
        for (unsigned i = 0; i < 5; ++i) {
            plus[i] = get(28 + i);
        }
        line();
        return true;
    }
    // Vanilla+: B and E kept (the same toe), A' = H C', and C', D' so the curve goes through the
    // game's at x = 0.18 and x = 1. Per point: C' x^2 (H - f) - D' f x = f E - x B.
    const double H = kPlusCeiling;
    const double x1 = 0.18, x2 = 1.0, f1 = f(x1), f2 = f(x2);
    const double a11 = x1 * x1 * (H - f1), a12 = -f1 * x1, b1 = f1 * E - x1 * B;
    const double a21 = x2 * x2 * (H - f2), a22 = -f2 * x2, b2 = f2 * E - x2 * B;
    const double det = a11 * a22 - a12 * a21;
    if (det == 0.0) {
        return false;
    }
    const double c = (b1 * a22 - a12 * b2) / det;
    const double d = (a11 * b2 - b1 * a21) / det;
    if (!(c > 0.0) || !(d >= 0.0)) {
        return false; // not a rising curve with these constants: keep the game's
    }
    if (mode == kVanillaPlusHue) {
        if (!plus) {
            return false;
        }
        plus[0] = static_cast<float>(H * c);
        plus[1] = static_cast<float>(B);
        plus[2] = static_cast<float>(c);
        plus[3] = static_cast<float>(d);
        plus[4] = static_cast<float>(E);
        line();
        return true;
    }
    put(28, static_cast<float>(H * c));
    put(30, static_cast<float>(c));
    put(31, static_cast<float>(d));
    return true;
}

} // namespace SotcTonemap
