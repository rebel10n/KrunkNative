#version 450 core
// Contrast-adaptive sharpening (Box Office-only rtSharpen, render_target.c), the second pass after rt_upscale.frag
// (or straight on the TAA resolve when the scene is not scaled). Written from the published description of AMD
// FidelityFX FSR 1's RCAS (no FSR source used): a 5-tap cross whose negative lobe is the largest one that keeps
// the result inside the neighbourhood's [min, max] (so it cannot ring or clip), capped at 0.1875 and scaled by
// `sharpness`. Bloom (added after, so the glow is not sharpened) and the saturationn / hue CSS filter step of
// scene_composite.frag follow, then the window gets the result.
out vec4 frag_color;

uniform sampler2D source;      // rt_upscale's output (window size) or the scene / TAA resolve (texture unit 0)
uniform float sharpness;       // 0..1: exp2(-stops) of the chosen level
uniform sampler2D bloom;       // texture unit 1, level 0 of the bloom chain
uniform float bloom_strength;  // 0 = no bloom
uniform vec2 output_size;      // window framebuffer size (bloom is sampled by window uv)
uniform bool filtered;
uniform mat3 saturate_matrix;
uniform mat3 hue_matrix;

#include "rt_grade.glsl"

vec3 fetch(ivec2 p) {
    ivec2 limit = textureSize(source, 0) - 1;
    return min(texelFetch(source, clamp(p, ivec2(0), limit), 0).rgb, vec3(1.0));
}

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec3 b = fetch(p + ivec2(0, 1));
    vec3 d = fetch(p + ivec2(-1, 0));
    vec3 e = fetch(p);
    vec3 f = fetch(p + ivec2(1, 0));
    vec3 h = fetch(p + ivec2(0, -1));

    vec3 mn = min(min(min(b, d), min(f, h)), e);
    vec3 mx = max(max(max(b, d), max(f, h)), e);
    // the lobe that would push the result to 0 (hit_min) or to 1 (hit_max), per channel
    vec3 hit_min = mn / max(4.0 * mx, vec3(1.0 / 4096.0));
    vec3 hit_max = (vec3(1.0) - mx) / min(4.0 * mn - 4.0, vec3(-1.0 / 4096.0));
    vec3 lobe3 = max(-hit_min, hit_max);
    float lobe = max(-0.1875, min(max(lobe3.r, max(lobe3.g, lobe3.b)), 0.0)) * sharpness;
    vec3 color = (lobe * (b + d + f + h) + e) / (4.0 * lobe + 1.0);
    color = clamp(color, 0.0, 1.0);

    if (bloom_strength > 0.0) color += texture(bloom, gl_FragCoord.xy / output_size).rgb * bloom_strength;
    color = rt_grade(color, gl_FragCoord.xy / output_size); // Cinematic: LUT + vignette
    if (filtered) {
        color = clamp(saturate_matrix * color, 0.0, 1.0);
        color = clamp(hue_matrix * color, 0.0, 1.0);
    }
    frag_color = vec4(color, 1.0);
}
