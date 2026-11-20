// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#version 450

layout (location = 0) in vec2 uv;
layout (location = 0) out vec4 color;

layout (binding = 0) uniform sampler2D texSampler;
// sotc: the game's UI layer (premultiplied, sRGB view), put over a DLSS upscaled scene.
layout (binding = 1) uniform sampler2D uiSampler;
// sotc: render size, g = 1 in the sky (DLSS's camera motion fill).
layout (binding = 2) uniform sampler2D skySampler;

// sotc: the scene's key (pp_key.comp): log2 of its log-average luminance / the paper white.
layout (binding = 3) readonly buffer SceneKey {
    float log2_key;
} scene_key;

// sotc: pp_pass.cpp GpuSettings (128 bytes, the push constant size every device takes).
layout (push_constant) uniform settings {
    float gamma;
    // sotc: bits 0-1 hdr (0 = SDR, 1 = the game's own HDR10, 2 = SDR game on HDR10), bit 2
    // srgb_input, bits 3-4 ui (1 = uiSampler over a DLSS upscaled scene; 2 = the same scene
    // without it, the HUD-less frame of frame generation), bit 5 the RenoDRT-style curve
    // (renoDrt()), bit 6 / 7 dithering for an 8 / 10 bit swapchain.
    uint flags;
    float grain; // sotc: film grain strength, 0..1 (0: none)
    uint seed;   // sotc: changes every presented frame (grain and dither move)
    vec4 vignette; // sotc: start, end, radius scale, strength (the game's tonemap, after DLSS)
    float ui_sharpen; // sotc: unsharp amount for the upscaled ui
    float sky_sharpen; // sotc: unsharp amount in the sky
    float hdr_white; // sotc: HDR paper white in nits
    float hdr_curve; // sotc: exponent of the game's HDR composite
    float paper_white; // sotc: hdr 2: nits of SDR white
    float highlight_gain; // sotc: hdr 2: gain of the highlights
    float hdr_peak; // sotc: hdr 1: the display's peak in nits (0: no roll-off)
    float ui_white; // sotc: hdr 1 with the UI layer: nits of the UI's white (0: hdr_white)
    // sotc: hdr 1: picture controls on the scene (grade()); the defaults are neutral.
    float exposure;   // linear gain (1)
    float contrast;   // power on luminance around mid grey (1)
    float shadows;    // extra power below mid grey (1)
    float highlights; // extra power above mid grey (1)
    float saturation; // chroma scale (1)
    float blowout;    // highlights towards white near the peak, 0..1 (0)
    float flare;      // flare compensation, 0..1 (0)
    float gamma_correction;     // SDR range decoded with this gamma instead of sRGB (0: off)
    float highlight_saturation; // chroma scale of the highlights (1)
    // sotc: hdr 1, the hue-preserving tone mapper (sotc_tonemap.h kVanillaPlusHue): Vanilla+'s
    // B..E (hue_e 0: off; A last) and the hue correction towards the per-channel curve, 0..1.
    // With the RenoDRT bit: the game's own A..E (hueCurve1()).
    float hue_b, hue_c, hue_d, hue_e;
    float hue_correction;
    float gamut_expansion; // sotc: hdr 1: 0..1, saturated colours towards P3 (0: none)
    float hue_a;
} pp;

#define PP_HDR (pp.flags & 3u)
#define PP_SRGB_INPUT ((pp.flags & 4u) != 0u)
#define PP_UI ((pp.flags >> 3) & 3u)
#define PP_DRT ((pp.flags & 32u) != 0u)

// sotc: noise for the film grain and the dithering: PCG hash of the pixel, the frame and a salt,
// triangular in -1..1.
uint pcg(uint v) {
    const uint state = v * 747796405u + 2891336453u;
    const uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}
float rand01(uint salt) {
    const uvec2 p = uvec2(gl_FragCoord.xy);
    return float(pcg(p.x + pcg(p.y + pcg(pp.seed ^ salt)))) * (1.0 / 4294967295.0);
}
float triNoise(uint salt) {
    return rand01(salt) + rand01(salt ^ 0x68bc21ebu) - 1.0;
}

// sotc: linear Rec.709 -> HDR10 (Rec.2020 primaries, ST 2084 PQ) for an SDR game. SDR white sits at
// paper_white nits; the highlights get up to highlight_gain times that, smoothly.
vec3 sdrToHdr10(vec3 lin) {
    lin = max(lin, 0.0);
    const float peak = max(lin.r, max(lin.g, lin.b));
    const float t = smoothstep(0.6, 1.0, peak);
    lin *= 1.0 + (pp.highlight_gain - 1.0) * t * t;
    const mat3 to2020 = mat3(0.6274, 0.0691, 0.0164,
                             0.3293, 0.9195, 0.0880,
                             0.0433, 0.0114, 0.8956);
    const vec3 y = clamp(to2020 * lin * pp.paper_white / 10000.0, 0.0, 1.0);
    const vec3 p = pow(y, vec3(0.1593017578125));
    return pow((0.8359375 + 18.8515625 * p) / (1.0 + 18.6875 * p), vec3(78.84375));
}

const float cutoff = 0.0031308, a = 1.055, b = 0.055, d = 12.92;
// The game's brightness (sceVideoOutAdjustColor, 0.1..2.0) as a power on the linear colour, then
// the standard sRGB encode. The upper branch is the same as before (a * rgb^(1/(3.4-gamma)) - b),
// but the old lower branch divided by the gamma (d * rgb / gamma): away from gamma 1 the curve
// jumped at the cutoff (at 0.1, 0.40 just below it and 0.13 just above), so each channel of a
// dark pixel landed on a different side and dark areas turned red, green and blue.
vec3 gamma(vec3 rgb) {
    if (pp.gamma != 1.0) { // the default: exactly the plain sRGB encode, as before
        rgb = pow(max(rgb, vec3(0.0)), vec3(2.4 / (2.4 + 1.0 - pp.gamma)));
    }
    return mix(
        a * pow(rgb, vec3(1.0 / 2.4)) - b,
        d * rgb,
        lessThan(rgb, vec3(cutoff))
    );
}

// Exact inverse of gamma() at unit gamma, for buffers that are sRGB encoded but must be sampled
// through a UNORM view because Vulkan has no sRGB variant of the 10-bit format.
vec3 degamma(vec3 rgb) {
    return mix(
        pow(max(rgb + b, 0.0) / a, vec3(2.4)),
        rgb / d,
        lessThan(rgb, vec3(d * cutoff))
    );
}

// sotc: SMPTE ST 2084 (PQ), in nits.
const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
vec3 pqDecode(vec3 e) {
    const vec3 p = pow(clamp(e, 0.0, 1.0), vec3(1.0 / m2));
    return 10000.0 * pow(max(p - c1, 0.0) / (c2 - c3 * p), vec3(1.0 / m1));
}
vec3 pqEncode(vec3 nits) {
    const vec3 y = pow(clamp(nits / 10000.0, 0.0, 1.0), vec3(m1));
    return pow((c1 + c2 * y) / (1.0 + c3 * y), vec3(m2));
}
// sotc: BT.2390 EETF on the brightest channel: the scene, scaled to the paper white, goes up to
// 10000 nits and used to clip at the display's peak (blown-out sun, sky and water). Below the
// knee (about half the peak in PQ) nothing changes; above it the highlights roll off smoothly into
// hdr_peak. The RGB ratio is kept, so hues do not shift as they brighten.
float pqEncode1(float nits) {
    const float y = pow(clamp(nits / 10000.0, 0.0, 1.0), m1);
    return pow((c1 + c2 * y) / (1.0 + c3 * y), m2);
}
float pqDecode1(float e) {
    const float p = pow(clamp(e, 0.0, 1.0), 1.0 / m2);
    return 10000.0 * pow(max(p - c1, 0.0) / (c2 - c3 * p), 1.0 / m1);
}
float eetf1(float nits) {
    if (pp.hdr_peak <= 0.0 || nits <= 0.0) {
        return max(nits, 0.0);
    }
    const float e = pqEncode1(nits);
    const float max_lum = pqEncode1(pp.hdr_peak);
    const float ks = 1.5 * max_lum - 0.5;
    if (e <= ks) {
        return nits;
    }
    const float t = (e - ks) / (1.0 - ks);
    const float t2 = t * t, t3 = t2 * t;
    const float e2 = (2.0 * t3 - 3.0 * t2 + 1.0) * ks + (t3 - 2.0 * t2 + t) * (1.0 - ks) +
                     (-2.0 * t3 + 3.0 * t2) * max_lum;
    return min(pqDecode1(e2), pp.hdr_peak);
}
vec3 rollOff(vec3 nits) {
    nits = max(nits, vec3(0.0));
    if (pp.hdr_peak <= 0.0) {
        return nits;
    }
    const float m = max(nits.r, max(nits.g, nits.b));
    if (m <= 0.0) {
        return nits;
    }
    return nits * (eetf1(m) / m);
}

// sotc: OkLab (Ottosson) from linear Rec.2020, for the hue correction.
const mat3 rec2020to709 = mat3(1.6605, -0.1246, -0.0182, -0.5876, 1.1329, -0.1006, -0.0728,
                               -0.0083, 1.1187);
const mat3 rec709to2020 = mat3(0.6274, 0.0691, 0.0164, 0.3293, 0.9195, 0.0880, 0.0433, 0.0114, 0.8956);
const mat3 okM1 = mat3(0.4122214708, 0.2119034982, 0.0883024619, 0.5363325363, 0.6806995451,
                       0.2817188376, 0.0514459929, 0.1073969566, 0.6299787005);
const mat3 okM2 = mat3(0.2104542553, 1.9779984951, 0.0259040371, 0.7936177850, -2.4285922050,
                       0.7827717662, -0.0040720468, 0.4505937099, -0.8086757660);
const mat3 okM2i = mat3(1.0, 1.0, 1.0, 0.3963377774, -0.1055613458, -0.0894841775, 0.2158037573,
                        -0.0638541728, -1.2914855480);
const mat3 okM1i = mat3(4.0767416621, -1.2684380046, -0.0041960863, -3.3077115913, 2.6097574011,
                        -0.7034186147, 0.2309699292, -0.3413193965, 1.7076147010);
vec3 toOkLab(vec3 c2020) {
    const vec3 lms = okM1 * (rec2020to709 * c2020);
    return okM2 * (sign(lms) * pow(abs(lms), vec3(1.0 / 3.0)));
}
vec3 fromOkLab(vec3 lab) {
    const vec3 lms = okM2i * lab;
    return rec709to2020 * (okM1i * (lms * lms * lms));
}

// sotc: `r` with its OkLab hue (a/b direction) moved towards `ref`'s by `amount`, its lightness
// and chroma kept.
vec3 hueTowards(vec3 r, vec3 ref, float amount) {
    const vec3 lab = toOkLab(r);
    const vec3 lab_ref = toOkLab(ref);
    const float chroma = length(lab.yz);
    const float chroma_ref = length(lab_ref.yz);
    if (chroma > 1e-5 && chroma_ref > 1e-5) {
        const vec2 dir = mix(lab.yz / chroma, lab_ref.yz / chroma_ref, amount);
        const float len = length(dir);
        if (len > 1e-5) {
            return max(fromOkLab(vec3(lab.x, dir / len * chroma)), vec3(0.0));
        }
    }
    return r;
}

// sotc: the hue-preserving tone mapper (sotc_tonemap.h kVanillaPlusHue). The game's pass drew a
// straight line (slope s, the game's curve at mid grey); here Vanilla+'s curve goes on the
// luminance and the colour is scaled with it, so a bright colour keeps its hue and saturation
// instead of drifting towards yellow and white per channel. hue_correction takes the hue (OkLab
// a/b direction, chroma kept) back towards the per-channel curve's. `u` in the game's linear
// units (Rec.2020).
float gameCurve1(float x) {
    return x * (pp.hue_a * x + pp.hue_b) / (x * (pp.hue_c * x + pp.hue_d) + pp.hue_e);
}
// RenoDRT: the game's curve (its toe: shadows and mid tones as the game shows them) blended over
// two stops around mid grey into the straight line through it (highlights untonemapped, for
// renoDrt()'s roll-off into the peak). Both meet at mid grey and rise, so the blend does too.
float hueCurve1(float x) {
    const float f = gameCurve1(x);
    if (!PP_DRT) {
        return f;
    }
    const float s = gameCurve1(0.18) / 0.18;
    const float t = smoothstep(-1.0, 1.0, log2(max(x, 1e-6) / 0.18));
    return mix(f, s * x, t);
}
vec3 hueCurve(vec3 u) {
    if (pp.hue_e <= 0.0) {
        return u;
    }
    const float s = hueCurve1(0.18) / 0.18;
    u = max(u, vec3(0.0));
    const float y = dot(u, vec3(0.2627, 0.6780, 0.0593));
    if (y <= 0.0 || s <= 0.0) {
        return u;
    }
    vec3 r = u * (hueCurve1(y / s) / y);
    if (pp.hue_correction > 0.0) {
        const vec3 x = u / s;
        const vec3 ref = vec3(hueCurve1(x.r), hueCurve1(x.g), hueCurve1(x.b));
        r = hueTowards(r, ref, pp.hue_correction);
    }
    return r;
}

// sotc: picture controls on the scene in nits (linear Rec.2020), before rollOff(). Contrast,
// shadows and highlights are powers on luminance pivoting at the scene's key (its log-average,
// measured each frame and smoothed: pp_key.comp), the colour scaled by the luminance ratio so
// hues stay. A fixed mid grey does not work here: SotC's frames go from ~3% of the paper white at
// night to ~140% in the desert, so a fixed pivot sits above or below nearly every pixel and the
// three controls only brighten or darken. Saturation scales the distance from grey; blowout fades
// highlights towards white from two stops above the key to the display's peak. All neutral:
// skipped, so the picture is unchanged bit for bit.
const vec3 luma2020 = vec3(0.2627, 0.6780, 0.0593);
// Gamma correction (RenoDX's): the SDR range (up to the paper white) as a 2.2 display shows an
// sRGB-encoded picture, darker near black; on luminance, so hues stay. Flare compensation takes
// the veiling glare out of the near-blacks (y^2 / (y + f)), absolute, a few nits at most.
// Highlight saturation scales the chroma of what is brighter than the key, more the brighter.
vec3 grade(vec3 nits) {
    if (pp.exposure == 1.0 && pp.contrast == 1.0 && pp.shadows == 1.0 && pp.highlights == 1.0 &&
        pp.saturation == 1.0 && pp.blowout == 0.0 && pp.flare == 0.0 &&
        pp.gamma_correction == 0.0 && pp.highlight_saturation == 1.0) {
        return nits;
    }
    nits = max(nits, vec3(0.0));
    float y = dot(nits, luma2020);
    if (y <= 0.0) {
        return nits;
    }
    if (pp.gamma_correction > 0.0) {
        const float w = y / pp.hdr_white;
        if (w < 1.0) {
            const float e = w <= 0.0031308 ? 12.92 * w : 1.055 * pow(w, 1.0 / 2.4) - 0.055;
            const float y2 = pp.hdr_white * pow(e, pp.gamma_correction);
            nits *= y2 / y;
            y = y2;
            if (y <= 0.0) {
                return nits;
            }
        }
    }
    nits *= pp.exposure;
    y *= pp.exposure;
    float k = scene_key.log2_key;
    if (!(k > -12.0 && k < 6.0)) { // not measured yet (or a bad value): 18% grey
        k = -2.474;
    }
    const float mid = pp.hdr_white * exp2(clamp(k, -8.0, 1.0)) * pp.exposure;
    const float yn = y / mid;
    const float y2 = mid * pow(yn, pp.contrast * (yn < 1.0 ? pp.shadows : pp.highlights));
    nits *= y2 / y;
    y = y2;
    if (pp.flare > 0.0) {
        const float y2 = y * y / (y + pp.flare * 0.02 * pp.hdr_white);
        nits *= y2 / y;
        y = y2;
    }
    float sat = pp.saturation;
    if (pp.highlight_saturation != 1.0 && y > mid) {
        const float t = smoothstep(0.0, 1.0, log2(y / mid) / 3.0);
        sat *= mix(1.0, pp.highlight_saturation, t);
    }
    nits = max(vec3(y) + (nits - vec3(y)) * sat, vec3(0.0));
    if (pp.blowout > 0.0) {
        const float peak = pp.hdr_peak > 0.0 ? pp.hdr_peak : 10000.0;
        const float start = 4.0 * mid;
        const float range = log2(max(peak / start, 1.01));
        const float t = smoothstep(0.0, 1.0, log2(max(y / start, 1e-6)) / range);
        nits = mix(nits, vec3(y), pp.blowout * t);
    }
    return nits;
}

// sotc: gamut expansion (RenoDX's): the game's colours are Rec.709; on an HDR display the
// saturated ones may go further, towards the P3 primaries (inside Rec.2020), the more the more
// saturated they already are, so greys and skin barely move. `nits` in linear Rec.2020.
const mat3 p3to2020 = mat3(0.753833, 0.045744, -0.001210, 0.198597, 0.941777, 0.017601, 0.047570,
                           0.012479, 0.983608);
vec3 expandGamut(vec3 nits) {
    if (pp.gamut_expansion <= 0.0) {
        return nits;
    }
    const vec3 c = max(rec2020to709 * nits, vec3(0.0));
    const float mx = max(c.r, max(c.g, c.b));
    if (mx <= 0.0) {
        return nits;
    }
    const float s = (mx - min(c.r, min(c.g, c.b))) / mx;
    return max(mix(nits, p3to2020 * c, pp.gamut_expansion * s * s), vec3(0.0));
}

// sotc: the RenoDRT-style tone mapper (sotc_tonemap.h kRenoDrt). The game's pass drew a straight
// line; here the luminance goes through the BT.2390 curve into the display's peak and the colour
// is scaled with it, so hues and saturation hold as they brighten. The hue correction control
// takes the hue towards the per-channel curve's (bright fire towards yellow, as film does).
vec3 renoDrt(vec3 nits) {
    nits = max(nits, vec3(0.0));
    const float y = dot(nits, luma2020);
    if (y <= 0.0 || pp.hdr_peak <= 0.0) {
        return nits;
    }
    vec3 r = nits * (eetf1(y) / y);
    if (pp.hue_correction > 0.0) {
        r = hueTowards(r, vec3(eetf1(nits.r), eetf1(nits.g), eetf1(nits.b)), pp.hue_correction);
    }
    return r;
}

// sotc: film grain (RenoDX's kind, the game has none): a fine, moving grain on luminance, the
// strongest in the mid tones and fading into black and the brightest highlights, as on film.
// Multiplicative, so black stays black. `white`: the paper white in the units of `c`.
vec3 filmGrain(vec3 c, float white) {
    if (pp.grain <= 0.0) {
        return c;
    }
    const float y = dot(max(c, vec3(0.0)), luma2020);
    if (y <= 0.0) {
        return c;
    }
    const float l = (log2(y / white) + 1.5) / 2.5;
    const float weight = exp2(-0.5 * l * l);
    return c * exp2(pp.grain * 0.3 * weight * triNoise(0x9e3779b9u));
}

// sotc: the scene in nits (linear Rec.2020, the game's HDR): gamut expansion, picture controls,
// tone mapper (RenoDRT or the roll-off into the peak), grain.
vec3 hdrScene(vec3 nits) {
    nits = grade(expandGamut(nits));
    if (PP_DRT) {
        // The luminance is in the peak already; a saturated colour's brightest channel may still
        // be above it: scaled down to it (hue kept), not rolled off a second time from the knee.
        nits = renoDrt(nits);
        const float m = max(nits.r, max(nits.g, nits.b));
        if (pp.hdr_peak > 0.0 && m > pp.hdr_peak) {
            nits *= pp.hdr_peak / m;
        }
    } else {
        nits = rollOff(nits);
    }
    nits = filmGrain(nits, pp.hdr_white);
    return pp.hdr_peak > 0.0 ? min(nits, vec3(pp.hdr_peak)) : nits;
}

// sotc: the scene in SDR (linear Rec.709, white 1): the same picture controls (the presenter
// gives hdr_white = hdr_peak = 100 here and no gamma correction) and grain. All neutral:
// unchanged bit for bit.
vec3 sdrScene(vec3 lin) {
    if (pp.grain <= 0.0 && pp.exposure == 1.0 && pp.contrast == 1.0 && pp.shadows == 1.0 &&
        pp.highlights == 1.0 && pp.saturation == 1.0 && pp.blowout == 0.0 && pp.flare == 0.0 &&
        pp.highlight_saturation == 1.0) {
        return lin;
    }
    const vec3 c = rec709to2020 * max(lin, vec3(0.0));
    const vec3 g = filmGrain(grade(c * pp.hdr_white) / pp.hdr_white, 1.0);
    return max(rec2020to709 * g, vec3(0.0));
}

// sotc: the UI layer is at render size; Catmull-Rom (9 bilinear taps) keeps its text sharper
// than a bilinear stretch. Clamped: the filter overshoots at hard edges.
vec4 sampleCatmullRom(sampler2D tex, vec2 p) {
    const vec2 size = vec2(textureSize(tex, 0));
    const vec2 pos = p * size;
    const vec2 c1 = floor(pos - 0.5) + 0.5;
    const vec2 f = pos - c1;
    const vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    const vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    const vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    const vec2 w3 = f * f * (-0.5 + 0.5 * f);
    const vec2 w12 = w1 + w2;
    const vec2 t0 = (c1 - 1.0) / size;
    const vec2 t3 = (c1 + 2.0) / size;
    const vec2 t12 = (c1 + w2 / w12) / size;
    vec4 r = vec4(0.0);
    r += textureLod(tex, vec2(t0.x, t0.y), 0.0) * w0.x * w0.y;
    r += textureLod(tex, vec2(t12.x, t0.y), 0.0) * w12.x * w0.y;
    r += textureLod(tex, vec2(t3.x, t0.y), 0.0) * w3.x * w0.y;
    r += textureLod(tex, vec2(t0.x, t12.y), 0.0) * w0.x * w12.y;
    r += textureLod(tex, vec2(t12.x, t12.y), 0.0) * w12.x * w12.y;
    r += textureLod(tex, vec2(t3.x, t12.y), 0.0) * w3.x * w12.y;
    r += textureLod(tex, vec2(t0.x, t3.y), 0.0) * w0.x * w3.y;
    r += textureLod(tex, vec2(t12.x, t3.y), 0.0) * w12.x * w3.y;
    r += textureLod(tex, vec2(t3.x, t3.y), 0.0) * w3.x * w3.y;
    return clamp(r, 0.0, 1.0);
}

vec4 shade() {
    vec4 color_linear = texture(texSampler, uv);
    if (pp.sky_sharpen != 0.0) {
        // DLSS softens the drifting clouds more than FSR1: extra unsharp there, limited to the
        // neighbours' range so the horizon does not ring.
        const float sky = texture(skySampler, uv).g;
        if (sky > 0.0) {
            const vec2 px = 1.0 / vec2(textureSize(texSampler, 0));
            const vec3 n = texture(texSampler, uv - vec2(0.0, px.y)).rgb;
            const vec3 s = texture(texSampler, uv + vec2(0.0, px.y)).rgb;
            const vec3 w = texture(texSampler, uv - vec2(px.x, 0.0)).rgb;
            const vec3 e = texture(texSampler, uv + vec2(px.x, 0.0)).rgb;
            const vec3 c = color_linear.rgb;
            const vec3 lo = min(min(min(n, s), min(w, e)), c);
            const vec3 hi = max(max(max(n, s), max(w, e)), c);
            color_linear.rgb =
                clamp(c + sky * pp.sky_sharpen * (c - 0.25 * (n + s + w + e)), lo, hi);
        }
    }
    if (PP_UI != 0u) {
        if (pp.vignette.w != 0.0 && pp.vignette.y != pp.vignette.x) {
            const float r = pp.vignette.z * length(uv - 0.5);
            const float t = clamp((r - pp.vignette.x) / (pp.vignette.y - pp.vignette.x), 0.0, 1.0);
            color_linear.rgb *= 1.0 + pp.vignette.w * (t * t * (3.0 - 2.0 * t) - 1.0);
        }
        vec4 ui = PP_UI == 1u ? sampleCatmullRom(uiSampler, uv) : vec4(0.0);
        if (PP_UI == 1u && pp.ui_sharpen > 0.0) {
            // Like RCAS on the FSR1 path: unsharp against the output-pixel neighbours, limited
            // to their range so edges do not ring. Premultiplied, so rgb and alpha alike.
            const vec2 px = 1.0 / vec2(textureSize(texSampler, 0));
            const vec4 n = sampleCatmullRom(uiSampler, uv - vec2(0.0, px.y));
            const vec4 s = sampleCatmullRom(uiSampler, uv + vec2(0.0, px.y));
            const vec4 w = sampleCatmullRom(uiSampler, uv - vec2(px.x, 0.0));
            const vec4 e = sampleCatmullRom(uiSampler, uv + vec2(px.x, 0.0));
            const vec4 lo = min(min(min(n, s), min(w, e)), ui);
            const vec4 hi = max(max(max(n, s), max(w, e)), ui);
            ui = clamp(ui + pp.ui_sharpen * (ui - 0.25 * (n + s + w + e)), lo, hi);
            ui.rgb = min(ui.rgb, vec3(ui.a));
        }
        if (PP_HDR == 1u) {
            // The game's HDR composite: UI over scene, ^k (its constant), Rec.2020, PQ with
            // 1.0 at 300 nits; scaled to the paper white like below.
            // The scene part gets the paper white and the roll-off to the display's peak; the UI
            // (where its alpha covers the scene) gets its own white, so menus and subtitles can
            // be dimmer or brighter than the world.
            const vec3 scene_part = abs(color_linear.rgb) * (1.0 - clamp(ui.a, 0.0, 1.0));
            const vec3 c = scene_part + abs(ui.rgb);
            const vec3 lin_all = pow(c, vec3(pp.hdr_curve));
            // The composite's light split by each layer's share (exact with the game's exponent
            // at 1): the scene's share gets the paper white, the tone mapper, the picture controls
            // and grain; the UI's share its own white. Mixing the whole composite by the UI's
            // alpha instead showed part of a translucent UI (the pause menu's white veil, alpha
            // ~0.7) at the scene's brightness and through the scene's grading: at UI brightness
            // 80 the menu still sat at the game brightness (190 nits).
            const vec3 ui_share = clamp(abs(ui.rgb) / max(c, vec3(1e-6)), 0.0, 1.0);
            const vec3 lin_scene = rec709to2020 * (lin_all * (1.0 - ui_share));
            vec3 nits = hdrScene(3.0 * pp.hdr_white * hueCurve(lin_scene));
            if (ui.a > 0.0) {
                const float ui_white = pp.ui_white > 0.0 ? pp.ui_white : pp.hdr_white;
                nits += 3.0 * ui_white * (rec709to2020 * (lin_all * ui_share));
                if (pp.hdr_peak > 0.0) nits = min(nits, vec3(pp.hdr_peak));
            }
            return vec4(pqEncode(nits), 1.0);
        }
        if (PP_HDR == 0u) {
            color_linear.rgb = sdrScene(color_linear.rgb);
        }
        color_linear = vec4(clamp(color_linear.rgb, 0.0, 1.0) * (1.0 - ui.a) + ui.rgb, 1.0);
    }
    if (PP_HDR == 1u) {
        // The game's own BT.2020 PQ output has its white at about 100 nits, too dim next to
        // the desktop; scaled to the paper white, highlights rolled off into the display's peak.
        // (The UI is already in this picture here, so it follows the paper white.)
        // The picture controls apply to the whole frame here (the UI is baked in).
        // The game's PQ has 1.0 of its linear units at 300 nits (hueCurve's units).
        const vec3 u = hueCurve(pqDecode(color_linear.rgb) / 300.0);
        return vec4(pqEncode(hdrScene(u * (3.0 * pp.hdr_white))), color_linear.a);
    }
    if (PP_SRGB_INPUT) color_linear.rgb = degamma(color_linear.rgb);
    if (PP_HDR == 2u) {
        return vec4(sdrToHdr10(color_linear.rgb), color_linear.a);
    }
    if (PP_UI == 0u) { // with the UI layer the scene was graded above, apart from the UI
        color_linear.rgb = sdrScene(color_linear.rgb);
    }
    return vec4(gamma(color_linear.rgb), color_linear.a);
}

void main() {
    color = shade();
    // sotc: dithering before the swapchain's 8 or 10 bits: smooth gradients (sky, fog, the dark)
    // instead of bands. A triangular noise of one step, invisible as noise.
    if ((pp.flags & 0xc0u) != 0u) {
        const float steps = (pp.flags & 64u) != 0u ? 255.0 : 1023.0;
        color.rgb += vec3(triNoise(0x1b873593u), triNoise(0xcc9e2d51u), triNoise(0xe6546b64u)) /
                     steps;
    }
}
