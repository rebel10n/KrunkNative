#version 450 core
// three.js examples Water.js (Krunker v7.1.4 libs/Water.js) fragment shader: four scrolled
// samples of the water normal map, the sun's diffuse / specular, the distorted mirror
// reflection with Schlick reflectance, getShadowMask (the sun shadow map), then three.js r150
// tonemapping_fragment and fog_fragment (no encodings_fragment: ShaderMaterial).
out vec4 frag_color;

in vec4 mirror_coord;
in vec4 world_position;
in float view_depth;
in vec4 shadow_coord;

uniform sampler2D mirror_sampler; // texture unit 0
uniform sampler2D normal_sampler; // texture unit 2 (water_0)
uniform float alpha;
uniform float time;
uniform float size;
uniform float distortion_scale;
uniform vec3 sun_color;
uniform vec3 sun_direction;
uniform vec3 eye;
uniform vec3 water_color;

uniform bool shadow_enabled;
uniform float shadow_bias;
uniform sampler2D shadow_map; // texture unit 1 (SHADOW_TEXTURE_UNIT)

uniform vec4 fog_color;
uniform float fog_near;
uniform float fog_far;
uniform bool fog_enabled;
uniform int tone_mapping;
uniform float tone_mapping_exposure;

vec4 get_noise(vec2 uv) {
    vec2 uv0 = (uv / 103.0) + vec2(time / 17.0, time / 29.0);
    vec2 uv1 = uv / 107.0 - vec2(time / -19.0, time / 31.0);
    vec2 uv2 = uv / vec2(8907.0, 9803.0) + vec2(time / 101.0, time / 97.0);
    vec2 uv3 = uv / vec2(1091.0, 1027.0) - vec2(time / 109.0, time / -113.0);
    vec4 noise = texture(normal_sampler, uv0) + texture(normal_sampler, uv1) + texture(normal_sampler, uv2) + texture(normal_sampler, uv3);
    return noise * 0.5 - 1.0;
}

void sun_light(vec3 surface_normal, vec3 eye_direction, float shiny, float spec, float diffuse, inout vec3 diffuse_color, inout vec3 specular_color) {
    vec3 reflection = normalize(reflect(-sun_direction, surface_normal));
    float direction = max(0.0, dot(eye_direction, reflection));
    specular_color += pow(direction, shiny) * sun_color * spec;
    diffuse_color += max(dot(sun_direction, surface_normal), 0.0) * sun_color * diffuse;
}

float shadow_mask() {
    if (!shadow_enabled) return 1.0;
    vec3 coord = shadow_coord.xyz / shadow_coord.w;
    coord.z += shadow_bias;
    bool in_frustum = coord.x >= 0.0 && coord.x <= 1.0 && coord.y >= 0.0 && coord.y <= 1.0 && coord.z <= 1.0;
    if (!in_frustum) return 1.0;
    return step(coord.z, texture(shadow_map, coord.xy).r);
}

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
    vec4 noise = get_noise(world_position.xz * size);
    vec3 surface_normal = normalize(noise.xzy * vec3(1.5, 1.0, 1.5));

    vec3 diffuse_light = vec3(0.0);
    vec3 specular_light = vec3(0.0);
    vec3 world_to_eye = eye - world_position.xyz;
    vec3 eye_direction = normalize(world_to_eye);
    sun_light(surface_normal, eye_direction, 100.0, 2.0, 0.5, diffuse_light, specular_light);

    float dist = length(world_to_eye);
    vec2 distortion = surface_normal.xz * (0.001 + 1.0 / dist) * distortion_scale;
    vec3 reflection_sample = vec3(texture(mirror_sampler, mirror_coord.xy / mirror_coord.w + distortion));

    float theta = max(dot(eye_direction, surface_normal), 0.0);
    float rf0 = 0.3;
    float reflectance = rf0 + (1.0 - rf0) * pow((1.0 - theta), 5.0);
    vec3 scatter = max(0.0, dot(surface_normal, eye_direction)) * water_color;
    vec3 albedo = mix((sun_color * diffuse_light * 0.3 + scatter) * shadow_mask(), (vec3(0.1) + reflection_sample * 0.9 + reflection_sample * specular_light), reflectance);
    vec4 color = vec4(albedo, alpha);

    color.rgb = tone_map(color.rgb);
    if (fog_enabled) color.rgb = mix(color.rgb, fog_color.rgb, smoothstep(fog_near, fog_far, view_depth));
    frag_color = color;
}
