#version 450 core
// Ray tracing one-bounce diffuse global illumination (client/src/rt/rt_pass.c, RT_EFFECT_GI): one
// fullscreen triangle; every lit pixel of the prepass shoots one cosine-weighted ray about its
// normal (a fresh direction per frame: the temporal denoiser averages them) and returns the radiance
// it sees, shaded the way rt_reflect.frag shades a reflection hit:
//   hit   albedo (material colour x atlas texel, or the texture's mean) x (ambient + sun N.L with one
//         shadow ray) / PI + emissive; unlit / emissive instances at full strength (that is what makes
//         a neon sign light the wall beside it); fogged by camera distance + ray length
//   miss  Krunker's ambient as a radiance (ambient irradiance / PI) blended rt_gi_sky_mix of the way to
//         the sky (sky colour above the horizon, fog colour at it): the map's constant ambient is what
//         its author tuned the open surfaces to, the sky part tints them by what they actually face
// Energy: a cosine-weighted sample with pdf cos / PI estimates the irradiance E = integral(L cos) as
// PI * L. basic.frag (krunker_gi_irradiance, rt.h RT_EFFECT_GI) does not add that on top of Krunker's
// ambient: it keeps the ambient's luminance as a floor, tints it with the bounce's colour and raises it
// where the bounce is brighter. The hit's own ambient (full strength) stands in for the bounces this pass
// does not trace.
// Output: rgb the linear radiance (before tone mapping), a = the ray length (RT_GI_FAR on a miss, 0 where
// nothing was traced: the sky).
#include "rt_common.glsl"
#include "rt_sky.glsl" // Box Office rtSky: the miss sees the physical sky (psky_on, sky_physical.c)

uniform sampler2D rt_depth;
uniform sampler2D rt_normal;
uniform sampler2D rt_atlas_mean; // RT_ATLAS_SLOTS x 1: each cell's mean colour (rt_atlas itself: rt_common.glsl)
uniform mat4 inverse_view_projection;
uniform vec3 camera_position;
uniform vec3 sun_direction;      // normalised, towards the sun
uniform vec3 ambient_light;      // ambient_color * ambient_intensity * light_scale (basic.frag krunker_lambert)
uniform vec3 sun_light;          // light_color * light_intensity * light_scale
uniform vec3 sky_color;
uniform vec3 fog_color;
uniform float fog_near;
uniform float fog_far;           // <= fog_near: no fog
uniform float rt_gi_emissive;    // emissive / unlit radiance scale at the hit (RT_GI_EMISSIVE, rt_pass.c)
uniform float rt_gi_sky_mix;     // a miss: ambient radiance -> sky radiance (RT_GI_SKY_MIX)
uniform int frame_index;
uniform int rt_gi_cine;          // the Cinematic preset (rt.h): no lift to the ambient floor (basic.frag adds the bounce as it is)
uniform int rt_prepass_normal_only; // 1: the prepass normal as is (the first-person viewmodel; see rt_reflect.frag)
uniform float rt_gi_far;            // the bounce ray's reach (RT_GI_FAR for the world; the viewmodel's is capped, rt_pass.c: its few
                                    // rays are latency-bound, and the bounce onto a weapon comes from the surfaces around the player)
uniform vec2 rt_texel;
// Sun visibility at a bounce hit, reused from this frame's trace output where the hit is on screen
// (rt_pass.c gi_pass, world pass only): the trace pass already shot a sun ray from the surface the
// camera sees there, so the hit's own shadow ray is skipped. The hit must project inside the screen and
// land on the same surface (the prepass position within a depth-scaled tolerance); otherwise it traces.
// The trace's R is one jittered 0 / 1 sample per pixel: GI averages it over frames like its own ray.
// Saves roughly half the hit shadow rays (~0.3 ms at 1080p / rtScale 0.5 on Sandstorm).
uniform int rt_gi_reuse_sun;       // 0: always trace (the viewmodel: its camera is not the trace's)
// First choice, any camera: the engine's static sun shadow map (shadow.c shadow_sun_map), one texture fetch instead
// of a ray. The trace pass is bound by each pixel's chain of sequential rays, so dropping the hit's shadow ray for
// every pixel is what saves time (an on-screen reuse that most but not all of a warp's lanes get saves little).
// The map holds the map's casters only (no players) and is static: right for low-frequency bounce light.
uniform int rt_sun_map_on;         // 0: no map this frame (shadows off): screen reuse, else the ray
uniform sampler2D rt_sun_map;
uniform mat4 rt_sun_matrix;        // world -> [0,1] shadow coordinates (shadow.c shadow_matrix)
uniform float rt_sun_bias;
uniform sampler2D rt_trace_vis;    // the trace output (R = sun visibility), RT resolution
uniform mat4 view_projection;      // the world camera of the trace output

#define RT_GI_FAR 20000.0
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
// rebuilt, else x the texture's mean (as rt_reflect.frag)
vec3 hit_albedo(RtHit hit, vec4 m0, vec4 m2) {
    vec3 albedo = m0.rgb;
    int code = int(m2.x + 0.5);
    if (code <= 0 || rt_atlas_ready == 0) return albedo;
    vec2 coord;
    if (rt_atlas_coord(hit.instance, hit.position, hit.normal, m2, coord)) return albedo * textureLod(rt_atlas, coord, 0.0).rgb;
    int slot = code % RT_ATLAS_UV_MODE_STEP - 1;
    return albedo * texelFetch(rt_atlas_mean, ivec2(slot, 0), 0).rgb;
}

// as basic.frag krunker_shadow_mask: lit outside the map's frustum
float map_sun(vec3 p) {
    vec4 sc = rt_sun_matrix * vec4(p, 1.0);
    vec3 coord = sc.xyz / sc.w;
    coord.z += rt_sun_bias;
    if (coord.x < 0.0 || coord.x > 1.0 || coord.y < 0.0 || coord.y > 1.0 || coord.z > 1.0) return 1.0;
    return step(coord.z, textureLod(rt_sun_map, coord.xy, 0.0).r);
}

// -1 = not on screen / another surface there: trace instead
float screen_sun(vec3 p) {
    vec4 clip = view_projection * vec4(p, 1.0);
    if (clip.w <= 0.0) return -1.0;
    vec2 suv = clip.xy / clip.w * 0.5 + 0.5;
    if (any(lessThan(suv, vec2(0.0))) || any(greaterThan(suv, vec2(1.0)))) return -1.0;
    ivec2 size = textureSize(rt_trace_vis, 0);
    ivec2 texel = clamp(ivec2(suv * vec2(size)), ivec2(0), size - 1);
    float d = texelFetch(rt_depth, texel, 0).r;
    if (d >= 1.0) return -1.0;
    vec3 q = world_at((vec2(texel) + 0.5) / vec2(size), d);
    if (length(q - p) > 0.02 * clip.w + 0.25) return -1.0;
    return texelFetch(rt_trace_vis, texel, 0).r;
}

vec3 sky(vec3 dir) {
    float up = clamp(dir.y, 0.0, 1.0);
    vec3 krunker = mix(fog_color, sky_color, sqrt(up));
    // rtSky: the physical sky's hue at the old miss brightness (the GI energy is tuned against it), no clouds or disk
    if (psky_on != 0) return psky_lum(krunker) * psky_chroma(psky_base(dir));
    return krunker;
}

vec3 fogged(vec3 color, float depth) {
    if (fog_far <= fog_near) return color;
    return mix(color, fog_color, smoothstep(fog_near, fog_far, depth));
}

// ---- emissive lights: next-event estimation (rt.h "emissive lights", rt_scene_emissive_lights)
// One light per pixel, picked in proportion to its power x area / distance^2 at this point (two passes over the
// list: the total, then the pick), one point on a box face that faces the point (faces in proportion to their
// area x cosine toward the point), one shadow ray. The estimate E = Le cos_s cos_l / d^2 x A / (p_face p_light)
// is irradiance; rt_gi's output is radiance (basic.frag reads G = PI x L), so it adds E / PI.
#define RT_EMISSIVE_MAX 64
#define RT_EMISSIVE_TEXELS 5
uniform sampler2D rt_emissive;
uniform int rt_emissive_count; // 0: no list (the bit off, KRUNKNATIVE_RT_NO_EMISSIVE_LIGHTS, no emitters near)

vec4 emissive_texel(int light, int t) {
    return texelFetch(rt_emissive, ivec2(light * RT_EMISSIVE_TEXELS + t, 0), 0);
}

float emissive_weight(int light, vec3 p, vec3 n) {
    vec4 t0 = emissive_texel(light, 0); // centre, power (rt_gpu.c)
    float size2 = emissive_texel(light, 1).w;
    vec3 d = t0.xyz - p;
    float dd = dot(d, d);
    float facing = clamp(dot(n, d) * inversesqrt(max(dd, 1e-6)) + 0.35, 0.0, 1.0); // a light behind the surface may still reach round its edge
    return t0.w * facing / (dd + size2); // the near field capped by the box's own size
}

// is `instance` one of the listed lights (a bounce that hits it leaves the emission to the sampled estimate)
bool emissive_listed(int instance) {
    for (int i = 0; i < RT_EMISSIVE_MAX; i++) {
        if (i >= rt_emissive_count) break;
        if (int(emissive_texel(i, 2).w + 0.5) == instance) return true;
    }
    return false;
}

vec3 emissive_direct(vec3 p, vec3 n, vec3 origin, float dist, uvec3 seed, inout int steps) {
    if (rt_emissive_count <= 0) return vec3(0.0);
    // one pass, weighted reservoir sampling: light i replaces the pick with probability w_i / (running total)
    float total = 0.0, w = 0.0;
    int light = -1;
    for (int i = 0; i < RT_EMISSIVE_MAX; i++) {
        if (i >= rt_emissive_count) break;
        float wi = emissive_weight(i, p, n);
        if (wi <= 0.0) continue;
        total += wi;
        if (rt_hash(seed + uvec3(97u + uint(i) * 131u, 13u, 5u)) * total < wi) { light = i; w = wi; }
    }
    if (light < 0 || total <= 0.0) return vec3(0.0);
    float p_light = w / total;

    vec3 c = emissive_texel(light, 0).xyz;
    vec3 axis[3];
    axis[0] = emissive_texel(light, 1).xyz; axis[1] = emissive_texel(light, 2).xyz; axis[2] = emissive_texel(light, 3).xyz;
    vec3 le = emissive_texel(light, 4).rgb;
    // the faces toward p: face k on side s (the outward normal s x axis[k]) when p lies beyond it
    float face_w[3];
    float face_s[3];
    float face_total = 0.0;
    for (int k = 0; k < 3; k++) {
        float len = length(axis[k]);
        int i = (k + 1) - 3 * ((k + 1) / 3), j = (k + 2) - 3 * ((k + 2) / 3);
        float area = 4.0 * length(axis[i]) * length(axis[j]);
        float coord = len > 1e-6 ? dot(p - c, axis[k]) / (len * len) : 0.0;
        face_s[k] = coord >= 0.0 ? 1.0 : -1.0;
        float beyond = len > 1e-6 ? abs(coord) - 1.0 : 1.0; // a flat light (zero half-axis): its face is visible from either side
        face_w[k] = beyond > 0.0 || len <= 1e-6 ? area * clamp(beyond + 0.05, 0.05, 1.0) : 0.0;
        face_total += face_w[k];
    }
    if (face_total <= 0.0) return vec3(0.0); // inside the box
    float r = rt_hash(seed + uvec3(29u, 71u, 3u)) * face_total;
    int k = 2;
    for (int f = 0; f < 3; f++) {
        if (r < face_w[f]) { k = f; break; }
        r -= face_w[f];
    }
    int i = (k + 1) - 3 * ((k + 1) / 3), j = (k + 2) - 3 * ((k + 2) / 3);
    float u = rt_hash(seed + uvec3(5u, 211u, 17u)) * 2.0 - 1.0, v = rt_hash(seed + uvec3(151u, 3u, 59u)) * 2.0 - 1.0;
    vec3 q = c + face_s[k] * axis[k] + u * axis[i] + v * axis[j];
    vec3 nl = length(axis[k]) > 1e-6 ? face_s[k] * normalize(axis[k]) : normalize(cross(axis[i], axis[j]));
    vec3 to = q - p;
    float d2 = max(dot(to, to), 0.0625), d = sqrt(d2); // at least 0.25 units: a sample at the surface (an emitter on the gun) cannot explode
    vec3 wi = to / d;
    float cos_s = dot(n, wi);
    float cos_l = abs(dot(nl, wi)); // a flat light emits from both faces
    if (cos_s <= 0.0 || cos_l <= 1e-4) return vec3(0.0);
    float face_area = 4.0 * length(axis[i]) * length(axis[j]);
    float p_face = face_w[k] / face_total;
    // The geometric term of one sample, capped at a full hemisphere (2 PI): a light can never give more than its
    // radiance x PI, and right next to a small emitter the 1 / d^2 of a single sample is what turns into blotches
    float g = min(cos_s * cos_l / d2 * face_area / p_face, 2.0 * PI);
    vec3 e = le * g / p_light;
    if (fog_far > fog_near) e *= 1.0 - smoothstep(fog_near, fog_far, d);
    // The pick never comes up empty while any light is listed (the weights have no range), so every pixel would
    // shoot this shadow ray even at a sign across the map. Below 2 % of the ambient the light cannot show: it is
    // added unshadowed, without the ray (the ray is the second link in each pixel's chain; ~0.2-0.4 ms at 1080p).
    const vec3 LUM_E = vec3(0.2126, 0.7152, 0.0722);
    if (dot(e, LUM_E) * rt_gi_emissive / PI < max(0.02 * dot(ambient_light, LUM_E) / PI, 1e-4)) return e;
    vec3 o = origin;
    float reach = d - (0.05 + d * 0.002);
    if (reach > 0.0 && rt_occluded(o, wi, reach, steps)) return vec3(0.0);
    return e;
}

void main() {
    float depth = texture(rt_depth, uv).r;
    if (depth >= 1.0) {
        frag_out = vec4(0.0);
        return;
    }
    vec3 pos = world_at(uv, depth);
    vec3 n = rt_prepass_normal_only != 0 ? normalize(texture(rt_normal, uv).xyz) : surface_normal(pos, texture(rt_normal, uv).xyz);
    float dist = length(pos - camera_position);
    vec3 origin = pos + n * (0.02 + dist * 0.0015);

    // cosine-weighted hemisphere about n (Malley), a fresh pair per pixel and frame
    uvec3 seed = uvec3(uint(gl_FragCoord.x) + 733u, uint(gl_FragCoord.y) + 419u, uint(frame_index));
    float r1 = rt_hash(seed), r2 = rt_hash(seed + uvec3(61u, 37u, 11u));
    float phi = r2 * 6.28318530718;
    float sin_theta = sqrt(r1), cos_theta = sqrt(1.0 - r1);
    vec3 t, b;
    rt_basis(n, t, b);
    vec3 dir = normalize(t * (cos(phi) * sin_theta) + b * (sin(phi) * sin_theta) + n * cos_theta);

    int steps = 0;
    RtHit hit;
    vec3 radiance;
    float ray_length = rt_gi_far;
    rt_skip_shadow_only = rt_hit_shadow_only == 0; // 0 for every pass today: no reflection / GI ray enters the shadow puppet (rt_pass.c)
    rt_skip_transparent = true; // through glass / liquids / the AO strips lying on every face (rt_common.glsl)
    bool found = rt_closest(origin, dir, rt_gi_far, hit, steps);
    rt_skip_transparent = false;
    vec3 ambient_radiance = ambient_light / PI; // the full constant ambient: basic.frag treats G as a tint / boost over it
    if (!found) {
        radiance = mix(ambient_radiance, sky(dir), rt_gi_sky_mix);
    } else {
        ray_length = hit.t;
        int mb = hit.material * 4;
        vec4 m0 = rt_fetch(rt_mats, mb), m1 = rt_fetch(rt_mats, mb + 1), m2 = rt_fetch(rt_mats, mb + 2), m3 = rt_fetch(rt_mats, mb + 3);
        vec3 albedo = hit_albedo(hit, m0, m2);
        if (m3.w > 0.5 || (hit.flags & RT_INST_UNLIT) != 0) {
            radiance = (albedo + m1.rgb) * rt_gi_emissive; // a sign / light cone / glowing skin: its colour is its light
            if (emissive_listed(hit.instance)) radiance = vec3(0.0); // its light comes from the sampled estimate below
        } else {
            vec3 irradiance = ambient_radiance * PI; // the hit's own constant ambient stands in for the next bounce
            float ndl = dot(hit.normal, sun_direction);
            if (ndl > 0.0) {
                float vis = rt_sun_map_on != 0 ? map_sun(hit.position + hit.normal * 0.1)
                          : rt_gi_reuse_sun != 0 ? screen_sun(hit.position) : -1.0;
                if (vis < 0.0) {
                    vec3 o = hit.position + hit.normal * (0.02 + (dist + hit.t) * 0.0015);
                    vis = rt_occluded(o, sun_direction, RT_GI_FAR, steps) ? 0.0 : 1.0;
                }
                irradiance += sun_light * ndl * vis;
            }
            radiance = albedo * irradiance / PI + m1.rgb * rt_gi_emissive;
        }
        radiance = fogged(radiance, dist + hit.t);
    }
    // Emissive light is extra light, not a bounce of the ambient: basic.frag's krunker_gi_irradiance keeps the
    // ambient's luminance as a floor and only raises it where G is brighter, so a dim bounce plus a sign's light
    // that still sums below the ambient would only tint. Where the sampled estimate is a real part of this pixel's
    // light (f: its luminance against 5 % of the ambient), the bounce is first lifted to the ambient's luminance (the
    // floor basic.frag would apply anyway), so the sign's light lands on top: E ~ A + E_emissive. Pixels far from
    // every emitter (f ~ 0) are unchanged.
    vec3 e_rad = emissive_direct(pos, n, origin, dist, seed, steps) * rt_gi_emissive / PI; // occlusion skips transparent instances (rt_occluder)
    const vec3 LUM = vec3(0.2126, 0.7152, 0.0722);
    float la = dot(ambient_radiance, LUM), lb = dot(radiance, LUM);
    float f = la > 1e-5 ? clamp(dot(e_rad, LUM) / (0.05 * la), 0.0, 1.0) : 0.0;
    if (rt_gi_cine == 0 && f > 0.0 && lb < la) radiance += ambient_radiance * (1.0 - lb / la) * f;
    radiance += e_rad;
    // Guard (world and viewmodel targets alike): no NaN / Inf or runaway radiance into the denoiser history or bloom
    radiance = any(isnan(radiance)) || any(isinf(radiance)) ? vec3(0.0) : min(radiance, vec3(RT_RADIANCE_MAX));
    frag_out = vec4(radiance, ray_length);
}
