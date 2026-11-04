#version 450 core
// Scene render target composite (client/src/render_target.c): bilinear upscale of
// the resolution-scaled scene plus Krunker's in-game CSS filter (bundleFilters(0):
// saturate(saturationn * 100%) then hue-rotate(hue * 3.6deg)), each clamped like a
// CSS filter step. Matrices are uploaded row-major with transpose = GL_TRUE.
in vec2 uv;
out vec4 frag_color;

uniform sampler2D scene; // texture unit 0 (RT_SCENE_TEXTURE_UNIT)
uniform bool filtered;
uniform mat3 saturate_matrix;
uniform mat3 hue_matrix;
// Bloom (Box Office-only rtBloom, render_target.c / rt_bloom.frag): the half-float scene may hold rgb above 1
// (additive draws) and the bloom weight in alpha; the display takes the clamped colour plus the blurred glow.
// bloom_strength 0 = no bloom: the colour is used as before.
uniform sampler2D bloom;  // texture unit 1, level 0 of the bloom chain
uniform float bloom_strength;

#include "rt_grade.glsl"

void main() {
    vec3 color = texture(scene, uv).rgb;
    if (bloom_strength > 0.0) color = min(color, vec3(1.0)) + texture(bloom, uv).rgb * bloom_strength;
    color = rt_grade(color, uv); // Cinematic: LUT + vignette, before the player's own filter
    if (filtered) {
        color = clamp(saturate_matrix * color, 0.0, 1.0);
        color = clamp(hue_matrix * color, 0.0, 1.0);
    }
    frag_color = vec4(color, 1.0);
}
