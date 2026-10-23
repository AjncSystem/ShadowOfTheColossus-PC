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

layout (push_constant) uniform settings {
    float gamma;
    bool hdr;
    bool srgb_input;
    // sotc: 1 = uiSampler over a DLSS upscaled scene; 2 = the same scene without it (the
    // HUD-less frame of frame generation); 0 = neither.
    uint ui;
    vec4 vignette; // sotc: start, end, radius scale, strength (the game's tonemap, after DLSS)
    float ui_sharpen; // sotc: unsharp amount for the upscaled ui
    float sky_sharpen; // sotc: unsharp amount in the sky
    float hdr_white; // sotc: HDR paper white in nits
    float hdr_curve; // sotc: exponent of the game's HDR composite
} pp;

const float cutoff = 0.0031308, a = 1.055, b = 0.055, d = 12.92;
vec3 gamma(vec3 rgb) {
    return mix(
        a * pow(rgb, vec3(1.0 / (2.4 + 1.0 - pp.gamma))) - b,
        d * rgb / pp.gamma,
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
const mat3 rec709to2020 = mat3(0.6274, 0.0691, 0.0164, 0.3293, 0.9195, 0.0880, 0.0433, 0.0114, 0.8956);

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

void main() {
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
    if (pp.ui != 0u) {
        if (pp.vignette.w != 0.0 && pp.vignette.y != pp.vignette.x) {
            const float r = pp.vignette.z * length(uv - 0.5);
            const float t = clamp((r - pp.vignette.x) / (pp.vignette.y - pp.vignette.x), 0.0, 1.0);
            color_linear.rgb *= 1.0 + pp.vignette.w * (t * t * (3.0 - 2.0 * t) - 1.0);
        }
        vec4 ui = pp.ui == 1u ? sampleCatmullRom(uiSampler, uv) : vec4(0.0);
        if (pp.ui == 1u && pp.ui_sharpen > 0.0) {
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
        if (pp.hdr) {
            // The game's HDR composite: UI over scene, ^k (its constant), Rec.2020, PQ with
            // 1.0 at 300 nits; scaled to the paper white like below.
            const vec3 c = abs(color_linear.rgb * (1.0 - ui.a) + ui.rgb);
            const vec3 nits = 3.0 * pp.hdr_white * (rec709to2020 * pow(c, vec3(pp.hdr_curve)));
            color = vec4(pqEncode(nits), 1.0);
            return;
        }
        color_linear = vec4(clamp(color_linear.rgb, 0.0, 1.0) * (1.0 - ui.a) + ui.rgb, 1.0);
    }
    if (pp.hdr) {
        // The game's own BT.2020 PQ output has its white at about 100 nits, too dim next to
        // the desktop; scaled to the paper white.
        color = vec4(pqEncode(pqDecode(color_linear.rgb) * (pp.hdr_white / 100.0)),
                     color_linear.a);
    } else {
        if (pp.srgb_input) color_linear.rgb = degamma(color_linear.rgb);
        color = vec4(gamma(color_linear.rgb), color_linear.a);
    }
}
