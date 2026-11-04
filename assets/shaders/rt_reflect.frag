#version 450 core
// Ray tracing reflection pass (client/src/rt/rt_pass.c, phase 4): one fullscreen triangle; a
// pixel whose prepass material reflects (rt_material.r > 0: a v10 GLB env material, a worn skin's
// env look, or every lit surface under KRUNKNATIVE_RT_FORCE_REFL) traces one reflection ray,
// GGX-jittered by the material's roughness (the temporal denoiser averages the frames), and
// shades the closest hit the way basic.frag's Lambert path would see it:
//   albedo    material colour x its texture (atlas cell; Krunker cube / plane UVs rebuilt from the
//             hit's local position, no texture repeat / offset / rotation / flips; a mesh-UV model
//             reflects its texture's mean colour)
//   lighting  ambient + the sun (N.L, one shadow ray); unlit / emissive materials as they are
//   fog       by the distance to the camera plus the ray length
//   miss      the sky: the map's sky colour above the horizon fading to the fog colour at it
// Output: rgb the linear radiance (before tone mapping: basic.frag applies its env weighting, fog
// and tone map), a = 1 on a traced pixel, 0 where the material does not reflect.
#include "rt_common.glsl"
#include "rt_sky.glsl" // Box Office rtSky: the miss sees the physical sky (psky_on, sky_physical.c)

uniform sampler2D rt_depth;
uniform sampler2D rt_normal;
uniform sampler2D rt_material;   // prepass attachment 1: (reflect intensity, roughness, metalness)
uniform sampler2D rt_atlas_mean; // RT_ATLAS_SLOTS x 1: each cell's mean colour (rt_atlas itself: rt_common.glsl)
uniform mat4 inverse_view_projection;
uniform vec3 camera_position;
uniform vec3 sun_direction;      // normalised, towards the sun
// the sun visibility at the hit from the engine's sun shadow map when one is active (rt_pass.c rt_sun_map_bind, as
// rt_gi.frag): one fetch instead of a second ray in each pixel's chain; else the traced ray
uniform int rt_sun_map_on;
uniform sampler2D rt_sun_map;
uniform mat4 rt_sun_matrix;
uniform float rt_sun_bias;

float map_sun(vec3 p) { // as basic.frag krunker_shadow_mask: lit outside the map's frustum
    vec4 sc = rt_sun_matrix * vec4(p, 1.0);
    vec3 coord = sc.xyz / sc.w;
    coord.z += rt_sun_bias;
    if (coord.x < 0.0 || coord.x > 1.0 || coord.y < 0.0 || coord.y > 1.0 || coord.z > 1.0) return 1.0;
    return step(coord.z, textureLod(rt_sun_map, coord.xy, 0.0).r);
}
uniform vec3 ambient_light;      // ambient_color * ambient_intensity * light_scale (basic.frag krunker_lambert)
uniform vec3 sun_light;          // light_color * light_intensity * light_scale
uniform vec3 sky_color;
uniform vec3 fog_color;
uniform float fog_near;
uniform float fog_far;           // <= fog_near: no fog
uniform int frame_index;
uniform vec2 rt_texel;
uniform int rt_prepass_normal_only; // 1: the prepass normal as is (the first-person viewmodel: thin parts a unit from the camera, where the silhouette rebuild below picks wrong neighbours)
uniform int refl_debug;          // dev (KRUNKNATIVE_RT_REFL_DEBUG): 1 the hit albedo, 2 the hit normal, 3 the ray length / 200, 4 hit = white / miss = blue

#define RT_REFL_FAR 20000.0
#define PI 3.14159265358979

in vec2 uv;
out vec4 frag_out;
const float RT_RADIANCE_MAX = 32.0; // rt.h RT_RADIANCE_MAX: about 30x a sunlit white surface

vec3 world_at(vec2 at, float depth) {
    vec4 clip = vec4(at * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 world = inverse_view_projection * clip;
    return world.xyz / world.w;
}

// as rt_trace.frag: the prepass normal rebuilt from the nearer depth neighbours at silhouettes
vec3 surface_normal(vec3 pos, vec3 prepass_n) {
    vec2 dx = vec2(rt_texel.x, 0.0), dy = vec2(0.0, rt_texel.y);
    float dl = texture(rt_depth, uv - dx).r, dr = texture(rt_depth, uv + dx).r;
    float dd = texture(rt_depth, uv - dy).r, du = texture(rt_depth, uv + dy).r;
    vec3 pl = dl < 1.0 ? world_at(uv - dx, dl) : pos, pr = dr < 1.0 ? world_at(uv + dx, dr) : pos;
    vec3 pd = dd < 1.0 ? world_at(uv - dy, dd) : pos, pu = du < 1.0 ? world_at(uv + dy, du) : pos;
    float ll = dl < 1.0 ? length(pl - pos) : 1e30, lr = dr < 1.0 ? length(pr - pos) : 1e30;
    float ld = dd < 1.0 ? length(pd - pos) : 1e30, lu = du < 1.0 ? length(pu - pos) : 1e30;
    vec3 ex = lr <= ll ? pr - pos : pos - pl;
    vec3 ey = lu <= ld ? pu - pos : pos - pd;
    if (ll >= 1e29 && lr >= 1e29 || ld >= 1e29 && lu >= 1e29) return prepass_n;
    vec3 n = cross(ex, ey);
    float len = length(n);
    if (len < 1e-12) return prepass_n;
    n /= len;
    if (dot(n, prepass_n) < 0.0) n = -n;
    return dot(n, prepass_n) > 0.98 ? prepass_n : n;
}

// The hit material's albedo: colour x texture (atlas, rt_common.glsl rt_atlas_coord) where the UVs can be
// rebuilt, else x the texture's mean
vec3 hit_albedo(RtHit hit, vec4 m0, vec4 m2) {
    vec3 albedo = m0.rgb;
    int code = int(m2.x + 0.5);
    if (code <= 0 || rt_atlas_ready == 0) return albedo;
    vec2 coord;
    if (rt_atlas_coord(hit.instance, hit.position, hit.normal, m2, coord)) return albedo * textureLod(rt_atlas, coord, 0.0).rgb;
    int slot = code % RT_ATLAS_UV_MODE_STEP - 1;
    return albedo * texelFetch(rt_atlas_mean, ivec2(slot, 0), 0).rgb;
}

vec3 sky(vec3 dir) {
    if (psky_on != 0) return psky_radiance(dir, 1); // rtSky: the drawn sky, disk and clouds included
    float up = clamp(dir.y, 0.0, 1.0);
    return mix(fog_color, sky_color, sqrt(up));
}

vec3 fogged(vec3 color, float depth) {
    if (fog_far <= fog_near) return color;
    return mix(color, fog_color, smoothstep(fog_near, fog_far, depth));
}

void main() {
    float depth = texture(rt_depth, uv).r;
    vec4 material = texture(rt_material, uv);
    if (depth >= 1.0 || material.r <= 0.0) {
        frag_out = vec4(0.0);
        return;
    }
    vec3 pos = world_at(uv, depth);
    vec3 n = rt_prepass_normal_only != 0 ? normalize(texture(rt_normal, uv).xyz) : surface_normal(pos, texture(rt_normal, uv).xyz);
    vec3 v = normalize(pos - camera_position);
    float dist = length(pos - camera_position);
    vec3 origin = pos + n * (0.02 + dist * 0.0015);

    // a GGX half vector around n (alpha = roughness^2); the frames' jitter is what the denoiser averages
    float roughness = clamp(material.g, 0.02, 1.0);
    float a2 = roughness * roughness;
    a2 *= a2;
    uvec3 seed = uvec3(uint(gl_FragCoord.x) + 311u, uint(gl_FragCoord.y) + 557u, uint(frame_index));
    float r1 = rt_hash(seed), r2 = rt_hash(seed + uvec3(17u, 23u, 5u));
    float cos_t = sqrt((1.0 - r1) / (1.0 + (a2 - 1.0) * r1));
    float sin_t = sqrt(max(1.0 - cos_t * cos_t, 0.0));
    float phi = r2 * 6.28318530718;
    vec3 t, b;
    rt_basis(n, t, b);
    vec3 h = normalize(t * (cos(phi) * sin_t) + b * (sin(phi) * sin_t) + n * cos_t);
    vec3 dir = reflect(v, h);
    if (dot(dir, n) <= 0.0) dir = reflect(v, n);

    int steps = 0;
    RtHit hit;
    vec3 radiance;
    vec3 albedo = vec3(0.0);
    rt_skip_shadow_only = rt_hit_shadow_only == 0; // 0 for every pass today: no reflection / GI ray enters the shadow puppet (rt_pass.c)
    rt_skip_transparent = true; // through glass / liquids / the AO strips (rt_common.glsl)
    bool found = rt_closest(origin, dir, RT_REFL_FAR, hit, steps);
    rt_skip_transparent = false; // the shadow ray below is an occlusion query anyway
    if (!found) {
        radiance = sky(dir);
    } else {
        int mb = hit.material * 4;
        vec4 m0 = rt_fetch(rt_mats, mb), m1 = rt_fetch(rt_mats, mb + 1), m2 = rt_fetch(rt_mats, mb + 2), m3 = rt_fetch(rt_mats, mb + 3);
        albedo = hit_albedo(hit, m0, m2);
        if (m3.w > 0.5 || (hit.flags & RT_INST_UNLIT) != 0) {
            radiance = albedo + m1.rgb;
        } else {
            vec3 irradiance = ambient_light;
            float ndl = dot(hit.normal, sun_direction);
            if (ndl > 0.0) {
                float vis;
                if (rt_sun_map_on != 0) vis = map_sun(hit.position + hit.normal * 0.1);
                else {
                    vec3 o = hit.position + hit.normal * (0.02 + (dist + hit.t) * 0.0015);
                    vis = rt_occluded(o, sun_direction, RT_REFL_FAR, steps) ? 0.0 : 1.0;
                }
                irradiance += sun_light * ndl * vis;
            }
            radiance = albedo * irradiance / PI + m1.rgb;
        }
        radiance = fogged(radiance, dist + hit.t);
    }
    if (refl_debug == 1) radiance = found ? albedo : vec3(0.0, 0.0, 1.0);
    else if (refl_debug == 2) radiance = found ? hit.normal * 0.5 + 0.5 : vec3(0.0, 0.0, 1.0);
    else if (refl_debug == 3) radiance = vec3(found ? hit.t / 200.0 : 1.0);
    else if (refl_debug == 4) radiance = found ? vec3(1.0) : vec3(0.0, 0.0, 1.0);
    else if (refl_debug == 5) radiance = found ? rt_fetch(rt_mats, hit.material * 4).rgb : vec3(0.0, 0.0, 1.0);
    else if (refl_debug == 6) { vec4 m2 = rt_fetch(rt_mats, hit.material * 4 + 2); int code = int(m2.x + 0.5); radiance = found ? vec3(float(code / RT_ATLAS_UV_MODE_STEP) / 2.0, float(code % RT_ATLAS_UV_MODE_STEP) / 256.0, float(rt_atlas_ready)) : vec3(0.0, 0.0, 1.0); }
    // Guard (world and viewmodel targets alike): no NaN / Inf or runaway radiance into the denoiser history or bloom
    radiance = any(isnan(radiance)) || any(isinf(radiance)) ? vec3(0.0) : min(radiance, vec3(RT_RADIANCE_MAX));
    frag_out = vec4(radiance, 1.0);
}
