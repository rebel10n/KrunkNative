#version 450 core
// Ray tracing: composite of the volumetric sun shafts over the drawn world (client/src/rt/rt_pass.c
// rt_shafts_composite, called by main.c after the world and its effects, before the first-person
// scene, so the viewmodel is not fogged by them and any later bloom / tone mapping of the scene target
// sees them). Drawn twice with different blend equations, `term` picking the half:
//   term 1  added      sun light scattered toward the eye by the lit haze (rt_shafts.frag R):
//                      sun_light * strength * (iso + HG(cos) / HG(1)) * R
//   term 0  subtracted the map's own fog where the sun does not reach it: krunker_fog mixed fog_color in
//                      by F(d) along the whole ray, F(d) - B of it lies in shadow, scaled by
//                      shadow_strength (shadowed fog darkens, so shafts read as volumes when looking
//                      away from the sun too). Off without real fog.
// The shafts target is smaller than the scene: each pixel takes its 4 nearest shafts texels weighted
// bilinearly and by how close their march distance (A) is to this pixel's (the RT prepass depth), so
// a shaft does not bleed across a silhouette; with no tap on this pixel's surface it adds nothing.
uniform sampler2D rt_shafts;   // R lit haze, G haze share, B lit fog share, A march distance (rt_shafts_denoise.frag)
uniform sampler2D rt_depth;    // the RT prepass depth: this pixel's distance
uniform mat4 inverse_view_projection;
uniform vec3 camera_position;
uniform vec3 sun_direction;
uniform vec3 sun_light;        // the sun's colour * intensity as basic.frag lights a white surface
uniform vec3 fog_color;
uniform float fog_near;
uniform float fog_far;         // the fog's far distance, also the march distance of a sky pixel
uniform float strength;        // RT_SHAFTS_STRENGTH (x RT_SHAFTS_HAZE on a map without fog)
uniform float shadow_strength; // RT_SHAFTS_SHADOW, 0 without real fog
uniform float phase_g;         // Henyey-Greenstein asymmetry (RT_SHAFTS_G)
uniform float phase_iso;       // RT_SHAFTS_ISO: the isotropic floor of the phase
uniform vec2 shafts_size;      // the shafts target size in texels
uniform int term;

in vec2 uv;
out vec4 frag_out;

vec3 world_at(vec2 at, float depth) {
    vec4 clip = vec4(at * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 world = inverse_view_projection * clip;
    return world.xyz / world.w;
}

float hg(float c, float g) {
    float k = 1.0 + g * g - 2.0 * g * c;
    return (1.0 - g * g) / (k * sqrt(k));
}

void main() {
    float depth = texture(rt_depth, uv).r;
    vec3 dir = normalize(world_at(uv, 0.5) - camera_position);
    float d = depth < 1.0 ? length(world_at(uv, depth) - camera_position) : fog_far;

    // joint bilateral upsample from the shafts target, guided by the march distance
    vec2 p = uv * shafts_size - 0.5;
    vec2 base = floor(p);
    vec2 f = p - base;
    ivec2 limit = ivec2(shafts_size) - 1;
    vec3 sum = vec3(0.0);
    float weight = 0.0;
    vec3 nearest = vec3(0.0);
    float nearest_dz = 1e30;
    for (int y = 0; y <= 1; y++) {
        for (int x = 0; x <= 1; x++) {
            ivec2 at = clamp(ivec2(base) + ivec2(x, y), ivec2(0), limit);
            vec4 s = texelFetch(rt_shafts, at, 0);
            float bilinear = (x == 1 ? f.x : 1.0 - f.x) * (y == 1 ? f.y : 1.0 - f.y);
            float dz = abs(s.a - d) / max(d, 1.0);
            float w = bilinear * exp(-dz * 20.0);
            sum += s.rgb * w;
            weight += w;
            if (dz < nearest_dz) { nearest_dz = dz; nearest = s.rgb; }
        }
    }
    // every tap across an edge (a feature thinner than a shafts texel): the closest in depth if it is the same
    // surface, else nothing (a sky texel's lit haze on a roof edge reads as a halo)
    vec3 shafts = weight > 1e-4 ? sum / weight : (nearest_dz < 0.15 ? nearest : vec3(0.0));
    float lit = shafts.r, lit_fog = shafts.b;
    float total_fog = smoothstep(fog_near, fog_far, d); // basic.frag krunker_fog at this pixel

    if (term == 1) {
        float phase = phase_iso + hg(dot(dir, sun_direction), phase_g) / hg(1.0, phase_g);
        frag_out = vec4(sun_light * strength * phase * lit, 0.0);
    } else {
        frag_out = vec4(fog_color * shadow_strength * max(total_fog - lit_fog, 0.0), 0.0);
    }
}
