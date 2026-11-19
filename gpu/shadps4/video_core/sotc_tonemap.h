// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: the game's tonemap (vk_rasterizer.cpp kTonemapHash) and the HDR tone mapper choice.
//
// The tonemap pass, before the HDR composite: scene (+ fog, depth of field, bloom) -> vignette ->
// tint (flatbuf dwords 37..39) -> saturation (35) -> contrast (40) -> curve (mode in dword 20;
// the game uses 5, x (A x + B) / (x (C x + D) + E) with A..E in dwords 28..32, then min 300) ->
// gamma (36) and scale (44) -> per channel gamma (45..47), min 10 -> lift (41..43) to gain
// (48..50) -> / scale.
//
// The tone mapper choice only changes A..E, so it works on every upscaler path: the pass stays
// the game's own, only its curve differs.
#pragma once

#include <atomic>
#include <bit>
#include <vector>

namespace SotcTonemap {

enum Mode : int {
    kVanilla = 0,     ///< the game's curve (flattens towards A / C = 3.3)
    kVanillaPlus = 1, ///< the same toe and mid tones, highlights rising towards kPlusCeiling
    kNone = 2,        ///< a straight line through the game's curve at mid grey
    kModeCount = 3,
};

/// Set by the presenter every frame (vk_presenter.cpp RefreshHdrLevels): the tone mapper wanted
/// (vanilla with game HDR off) and the strength of the game's colour grading, 0..1.
inline std::atomic<int> wanted{kVanilla};
inline std::atomic<float> grading{1.0f};

/// Vanilla+'s ceiling (the curve's limit A / C). With the game's scale 0.556 it stays under the
/// pass's min 10 cap (16 * 0.556 = 8.9), so the final pass's roll-off shapes the highlights.
inline constexpr float kPlusCeiling = 16.0f;

/// The curve of `mode` in place of the game's, in a copy of the tonemap's flatbuf. Only for the
/// game's curve mode 5; returns false (flatbuf unchanged) otherwise.
inline bool ApplyCurve(std::vector<unsigned>& flat, int mode) {
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
    if (mode == kNone) {
        // y = x * f(0.18) / 0.18: A = C = D = 0, B / E the slope.
        put(28, 0.0f);
        put(29, static_cast<float>(E * f(0.18) / 0.18));
        put(30, 0.0f);
        put(31, 0.0f);
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
    put(28, static_cast<float>(H * c));
    put(30, static_cast<float>(c));
    put(31, static_cast<float>(d));
    return true;
}

} // namespace SotcTonemap
