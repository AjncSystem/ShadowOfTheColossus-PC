// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: a sun shadow cascade moved by whole texels (vk_shadow_scroll.cpp). The engine snaps its
// cascades to texels and only their translation changes between frames: the new cascade is the
// old one shifted by `shift` texels, every depth moved by `dz` (orthographic projection).
// Texels shifted in from outside the old cascade, and empty texels, hold the clear depth.

#version 450 core
#extension GL_EXT_samplerless_texture_functions : require

layout (binding = 0, set = 0) uniform texture2D src;

layout (push_constant) uniform Scroll {
    ivec2 shift;
    float dz;
    float empty;
} pc;

void main() {
    const ivec2 at = ivec2(gl_FragCoord.xy) - pc.shift;
    const ivec2 size = textureSize(src, 0);
    if (any(lessThan(at, ivec2(0))) || any(greaterThanEqual(at, size))) {
        gl_FragDepth = pc.empty;
        return;
    }
    const float depth = texelFetch(src, at, 0).r;
    gl_FragDepth = depth == pc.empty ? depth : clamp(depth + pc.dz, 0.0, 1.0);
}
