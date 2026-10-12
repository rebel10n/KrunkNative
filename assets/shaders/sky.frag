#version 450 core
// Krunker v7.1.4 SkyDome (sky.c): three.js r150 MeshBasicMaterial (texel * white) or, with an
// emissive colour, StrippedLambertMaterial in the light-less background scene (emissive *
// emissiveMap only), then tonemapping_fragment / encodings_fragment, then the onBeforeCompile
// gradient multiply after dithering. No fog (the background scene has none).
out vec4 frag_color;

in vec2 tex_coord;
in vec2 sphere_uv;

uniform bool use_texture;
uniform sampler2D tex;
uniform bool emissive_mode;
uniform vec3 emissive_color;
uniform bool use_emissive_map;
uniform sampler2D emissive_map;
uniform vec3 first_color;
uniform vec3 middle_color;
uniform vec3 end_color;
uniform int tone_mapping;
uniform float tone_mapping_exposure;
uniform int output_encoding;

vec3 tone_map(vec3 color) {
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
    vec4 texel = use_texture ? texture(tex, tex_coord) : vec4(1.0);
    vec4 color = texel;
    if (emissive_mode) {
        vec3 emissive = emissive_color;
        if (use_emissive_map) emissive *= texture(emissive_map, tex_coord).rgb;
        color = vec4(emissive, texel.a);
    }
    color.rgb = tone_map(color.rgb);
    if (output_encoding == 1) {
        color.rgb = mix(pow(color.rgb, vec3(0.41666)) * 1.055 - vec3(0.055), color.rgb * 12.92, vec3(lessThanEqual(color.rgb, vec3(0.0031308))));
    }
    // SkyDome getGradient(RES * uv): xy = uv.yx, h = 0.5
    float v = sphere_uv.y;
    float h = 0.5;
    vec3 gradient = mix(mix(end_color, middle_color, v / h), mix(middle_color, first_color, (v - h) / (1.0 - h)), step(h, v));
    color.rgb *= gradient;
    frag_color = color;
}
