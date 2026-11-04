#version 450 core
// Box Office physical sky pass (rtSky; client/src/sky_physical.c): replaces Krunker's sky dome / clear colour
// with rt_sky.glsl's sky, one fullscreen triangle (scene_composite.vert) drawn first, depth untouched. The output
// goes through Krunker's tone mapping like sky.frag's dome.
in vec2 uv;
out vec4 frag_color;

uniform mat4 inverse_view_projection;
uniform vec3 camera_position;
uniform int tone_mapping;
uniform float tone_mapping_exposure;

#include "rt_sky.glsl"

// Cinematic (rt.h "Cinematic colour pipeline"): three.js's ACES fit at cine_exposure (rt_grade_exposure(): the
// preset's exposure x RT_CINE_EXPOSURE x a Linear map's own), like basic.frag krunker_cine_curve; 0 = off.
uniform float cine_exposure;
vec3 cine_curve(vec3 color) {
    const mat3 aces_input = mat3(vec3(0.59719, 0.07600, 0.02840), vec3(0.35458, 0.90834, 0.13383), vec3(0.04823, 0.01566, 0.83777));
    const mat3 aces_output = mat3(vec3(1.60475, -0.10208, -0.00327), vec3(-0.53108, 1.10813, -0.07276), vec3(-0.07367, -0.00605, 1.07602));
    vec3 x = color * cine_exposure;
    vec3 c = aces_input * (x / 0.6);
    vec3 a = c * (c + 0.0245786) - 0.000090537;
    vec3 b = c * (0.983729 * c + 0.4329510) + 0.238081;
    vec3 per_channel = clamp(aces_output * (a / b), 0.0, 1.0);
    float m = max(max(x.r, x.g), x.b); // 60 % hue-preserving, as basic.frag krunker_cine_curve
    vec3 hue_kept = m > 1e-4 ? x * (max(max(per_channel.r, per_channel.g), per_channel.b) / m) : per_channel;
    return clamp(mix(per_channel, hue_kept, 0.6), 0.0, 1.0);
}

vec3 tone_map(vec3 color) { // sky.frag's
    if (cine_exposure > 0.0 && tone_mapping <= 1) return cine_curve(color);
    if (tone_mapping == 1) return tone_mapping_exposure * color;
    if (tone_mapping == 2) {
        color *= tone_mapping_exposure;
        return clamp(color / (vec3(1.0) + color), 0.0, 1.0);
    }
    if (tone_mapping == 3) {
        color *= tone_mapping_exposure;
        color = max(vec3(0.0), color - 0.004);
        return pow((color * (6.2 * color + 0.5)) / (color * (6.2 * color + 1.7) + 0.06), vec3(2.2));
    }
    if (tone_mapping == 4) {
        const mat3 aces_input = mat3(vec3(0.59719, 0.07600, 0.02840), vec3(0.35458, 0.90834, 0.13383), vec3(0.04823, 0.01566, 0.83777));
        const mat3 aces_output = mat3(vec3(1.60475, -0.10208, -0.00327), vec3(-0.53108, 1.10813, -0.07276), vec3(-0.07367, -0.00605, 1.07602));
        color *= tone_mapping_exposure / 0.6;
        color = aces_input * color;
        vec3 a = color * (color + 0.0245786) - 0.000090537;
        vec3 b = color * (0.983729 * color + 0.4329510) + 0.238081;
        return clamp(aces_output * (a / b), 0.0, 1.0);
    }
    return color;
}

void main() {
    vec4 far_point = inverse_view_projection * vec4(uv * 2.0 - 1.0, 1.0, 1.0);
    vec3 dir = normalize(far_point.xyz / far_point.w - camera_position);
    frag_color = vec4(tone_map(psky_radiance(dir, 1)), 1.0);
}
