// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#version 450 core

// sotc: the in-game menu (Dear ImGui, bbport_overlay.cpp) on an HDR10 swapchain. ImGui's colours
// are sRGB display values; written as they are into a PQ image, its white was 10000 nits (the
// most PQ can say) and its greys and colours far too bright. Here they are decoded, put in
// Rec.2020 and encoded as PQ with white at 203 nits (ITU-R BT.2408's graphics white). The
// interface matches ImGui's own vertex shader (imgui_impl_vulkan.cpp).

layout(location = 0) out vec4 fColor;
layout(set = 0, binding = 0) uniform sampler2D sTexture;
layout(location = 0) in struct {
    vec4 Color;
    vec2 UV;
} In;

const float kWhiteNits = 203.0;
const mat3 rec709to2020 = mat3(0.6274, 0.0691, 0.0164, 0.3293, 0.9195, 0.0880, 0.0433, 0.0114,
                               0.8956);
const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;

void main() {
    const vec4 c = In.Color * texture(sTexture, In.UV.st);
    const vec3 e = clamp(c.rgb, 0.0, 1.0);
    const vec3 lin = mix(pow((e + 0.055) / 1.055, vec3(2.4)), e / 12.92, lessThan(e, vec3(0.04045)));
    const vec3 y = pow(clamp(rec709to2020 * lin * (kWhiteNits / 10000.0), 0.0, 1.0), vec3(m1));
    fColor = vec4(pow((c1 + c2 * y) / (1.0 + c3 * y), vec3(m2)), c.a);
}
