#version 450 core
// Ray tracing trace pass (client/src/rt/rt_pass.c, doc/raytracing-plan.md): one fullscreen
// triangle; each pixel reconstructs its world position from the prepass depth, and traces
//   R  sun visibility      one shadow ray towards the sun, jittered inside its angular radius
//   G  ambient occlusion   rt_ao_samples cosine-weighted hemisphere rays of length rt_ao_radius,
//                          each occluder weighted (1 - t / radius): contact corners go dark the way
//                          Krunker's ambient_0/1 strips do, open floors stay white (phase 3);
//                          at rtMode 1 (no AO) the sky visibility of specular occlusion instead (rt_sky_vis)
//   B  traversal steps / 256 (rt_debug bvh)
//   A  1.0 where a surface was traced, 0.0 on the sky
#include "rt_common.glsl"

uniform sampler2D rt_depth;    // prepass depth
uniform sampler2D rt_normal;   // prepass world normal (.w dynamic flag)
uniform mat4 inverse_view_projection;
uniform vec3 camera_position;
uniform vec3 sun_direction;    // normalised, towards the sun (g_render_lighting.light_direction)
uniform float sun_tan_radius;  // tan(rt_sun_radius): disk radius at unit distance, 0 = hard
uniform int frame_index;
uniform int rt_ao_samples;     // AO rays per pixel (0 = no AO: mode 1 keeps its cost)
uniform float rt_ao_radius;    // AO ray length, world units (RT_AO_RADIUS)
uniform float rt_ao_min;       // occluders nearer than this do not count (RT_AO_MIN: decals, planks, trims)
uniform int rt_sky_vis;        // rtMode 1: a sky-visibility ray into G instead of AO (RT_EFFECT_SKY_VIS): 1 every pixel, 2 a quarter a frame
uniform float rt_sky_vis_radius; // its length, world units (RT_SKY_VIS_RADIUS)
uniform vec2 rt_texel;         // 1 / RT target size
// AO split over frames (rt_pass.c, with the denoiser on): each frame traces half the AO rays per pixel (rounded
// up), each frame in fresh directions, and the temporal history (up to 32 frames on the static world) adds them up.
// The trace pass is bound by each pixel's chain of sequential rays, not by how many pixels trace: skipping half
// the pixels (checkerboards of pixels or tiles) saved ~20 %, halving the chain saves ~0.85 ms at 1080p,
// rtScale 0.5, rtSamples 2 (Sandstorm: trace 1.97 -> 1.13 ms).
uniform int rt_ao_split;

// Local light shadows (phase 5, rt.h RT_LOCAL_*): up to RT_LOCAL_MAX point / spot lights of the
// forward list. rt_local_pos = (position, range), rt_local_dir = (direction from the target to the
// light, coneCos: spot; w = -2 marks a point light). rt_local_count 0 = off (mode cost unchanged).
#define RT_LOCAL_MAX 8
uniform int rt_local_count;
uniform vec4 rt_local_pos[RT_LOCAL_MAX];
uniform vec4 rt_local_dir[RT_LOCAL_MAX];
uniform float rt_local_radius; // jitter disk radius at the light (RT_LOCAL_LIGHT_RADIUS)
uniform vec4 rt_local_params[RT_LOCAL_MAX]; // (decay, penumbraCos, luminance of lt_color, 0)
uniform int rt_local_mask;     // bit i: slot i is traced this frame (round robin, rt.h RT_LOCAL_PER_FRAME)
uniform float rt_local_min;    // RT_LOCAL_MIN: a light contributing less than this is left untraced
uniform bool rt_correct_lights; // RenderLighting.correct_lights (the attenuation model below)

// basic.frag distance_attenuation: the ray is skipped where the light would not show anyway
float rt_local_attenuation(float d, float cutoff, float decay) {
    if (!rt_correct_lights) {
        return cutoff > 0.0 && decay > 0.0 ? pow(clamp(-d / cutoff + 1.0, 0.0, 1.0), decay) : 1.0;
    }
    float falloff = 1.0 / max(pow(d, decay), 0.01);
    if (cutoff > 0.0) {
        float r = d / cutoff;
        float x = clamp(1.0 - r * r * r * r, 0.0, 1.0);
        falloff *= x * x;
    }
    return falloff;
}

in vec2 uv;
layout (location = 0) out vec4 frag_out;
layout (location = 1) out vec4 frag_local; // 8 local light visibilities, 2 per channel x 12 bits (rt.h)

// Two 12-bit visibilities in one float (exact: < 2^24). A light that was not traced reads 1.
float rt_local_pack(float v0, float v1) {
    return floor(clamp(v0, 0.0, 1.0) * 4095.0 + 0.5) * 4096.0 + floor(clamp(v1, 0.0, 1.0) * 4095.0 + 0.5);
}

vec3 world_at(vec2 at, float depth) {
    vec4 clip = vec4(at * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 world = inverse_view_projection * clip;
    return world.xyz / world.w;
}

// The prepass normal is cross(dFdx, dFdy) of the world position: at a silhouette the derivative
// straddles two surfaces and the normal can point into the one we stand on, so every ray starts
// inside it (a dark seam along every thin plank and trim). Rebuild the normal from the depth
// buffer picking, per axis, the neighbour whose position is nearer (the one on our surface);
// a sky neighbour never wins. Where both agree with the prepass normal, keep that one (exact).
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
    // same surface as the prepass saw: its exact normal; otherwise the rebuilt one
    return dot(n, prepass_n) > 0.98 ? prepass_n : n;
}

void main() {
    float depth = texture(rt_depth, uv).r;
    frag_local = vec4(rt_local_pack(1.0, 1.0)); // every local light fully visible unless traced below
    if (depth >= 1.0) {
        frag_out = vec4(1.0, 1.0, 0.0, 0.0);
        return;
    }
    vec4 normal_dyn = texture(rt_normal, uv);
    vec3 pos = world_at(uv, depth);
    vec3 n = surface_normal(pos, normal_dyn.xyz);

    float dist = length(pos - camera_position);
    // off the surface by a distance-aware epsilon: the depth buffer's quantisation at `dist`
    // and the 16-bit normal's error must not put the origin inside the surface
    vec3 origin = pos + n * (0.02 + dist * 0.0015);

    int steps = 0;
    float visibility = 1.0;
    float facing = dot(n, sun_direction);
    if (facing <= 0.0) {
        // the sun is behind the surface: the direct term is 0 whatever we trace
        visibility = 0.0;
    } else {
        vec3 dir = sun_direction;
        if (sun_tan_radius > 0.0) {
            uvec3 seed = uvec3(uint(gl_FragCoord.x), uint(gl_FragCoord.y), uint(frame_index));
            float r1 = rt_hash(seed), r2 = rt_hash(seed + uvec3(7u, 13u, 29u));
            float radius = sqrt(r1) * sun_tan_radius;
            float phi = r2 * 6.28318530718;
            vec3 t, b;
            rt_basis(sun_direction, t, b);
            dir = normalize(sun_direction + (t * cos(phi) + b * sin(phi)) * radius);
        }
        if (rt_occluded(origin, dir, 20000.0, steps)) visibility = 0.0;
    }

    // from here on no ray may enter the shadow puppet (rt_common.glsl rt_skip_shadow_only): only the sun ray above does
    rt_skip_shadow_only = true;
    float ao = 1.0;
    // G: the AO rays (mode 2+), or at rtMode 1 the Box Office specular-occlusion sky-visibility ray (rt.h
    // RT_EFFECT_SKY_VIS; basic.frag krunker_rt_mat_specular scales its sky reflection by it). Both share this
    // one loop and its single traversal call site.
    // rt_sky_vis 2 (the denoiser runs): one 8x8 tile of each 16x16 block a frame, rotating, the others write -1
    // and rt_denoise.frag keeps their history; 1: every pixel (no denoiser to fill in). Tiles, not a per-pixel
    // checkerboard: a GPU warp with any traced pixel pays the whole traversal, so a 2x2 checkerboard cost
    // two thirds of the full rate; whole-warp tiles cost about a quarter.
    int g_rays = rt_ao_samples;
    if (g_rays == 0 && rt_sky_vis > 0) {
        int cell = ((int(gl_FragCoord.x) >> 3) & 1) + 2 * ((int(gl_FragCoord.y) >> 3) & 1);
        if (rt_sky_vis == 1 || cell == (frame_index & 3)) g_rays = 1;
        else ao = -1.0; // not traced this frame
    }
    // see rt_ao_split; a pixel of a moving mesh (prepass normal.w) keeps the full count: its history is short (the
    // denoiser's max_history), so half the samples summed over two frames would leave it with half the AO
    if (g_rays > 0 && rt_ao_samples > 0 && rt_ao_split != 0 && normal_dyn.w <= 0.5) g_rays = (g_rays + 1) / 2;
    if (g_rays > 0) {
        bool sky = rt_ao_samples == 0;
        // the AO lobe is about the normal; the sky lobe about the halfway of the mirror direction and the normal
        // (the class roughness is not known here; most classed surfaces are mid-rough)
        vec3 axis = sky ? normalize(reflect(normalize(pos - camera_position), n) + n) : n;
        float reach = sky ? rt_sky_vis_radius : rt_ao_radius;
        float start = sky ? 0.0 : rt_ao_min;
        vec3 t, b;
        rt_basis(axis, t, b);
        float occlusion = 0.0;
        for (int i = 0; i < 8; i++) { // bounded for ES; rt_ao_samples <= 8
            if (i >= g_rays) break;
            // a fresh pair per pixel, frame and sample: the temporal pass averages the frames
            uvec3 seed = uvec3(uint(gl_FragCoord.x) + 977u, uint(gl_FragCoord.y) + 131u, uint(frame_index * 8 + i));
            float r1 = rt_hash(seed), r2 = rt_hash(seed + uvec3(43u, 19u, 3u));
            // cosine-weighted lobe about the axis (Malley): the irradiance-weighted occlusion
            float phi = r2 * 6.28318530718;
            float sin_theta = sqrt(r1), cos_theta = sqrt(1.0 - r1);
            vec3 dir = t * (cos(phi) * sin_theta) + b * (sin(phi) * sin_theta) + axis * cos_theta;
            if (dot(dir, n) < 0.0) dir = reflect(dir, n); // the sky lobe may dip below the surface
            float t_hit;
            // an AO ray starts rt_ao_min out: a decal plane or a plank lying on this face is not a crevice
            if (rt_occluded_t(origin + dir * start, dir, reach - start, t_hit, steps))
                occlusion += sky ? 1.0 : 1.0 - (t_hit + start) / reach; // the sky ray is binary
        }
        ao = 1.0 - occlusion / float(g_rays);
    }

    // local light shadows: one any-hit ray per light from the surface to a jittered point on the
    // light's disk, inside the light's range and (spot) cone only; a light behind the surface or out
    // of reach keeps 1 (its direct term is 0 there anyway, and 1 keeps the history smooth)
    if (rt_local_count > 0) {
        float lv[RT_LOCAL_MAX];
        for (int i = 0; i < RT_LOCAL_MAX; i++) {
            lv[i] = 1.0;
            if (i >= rt_local_count || ((rt_local_mask >> i) & 1) == 0) continue; // not this frame's turn: the history holds
            vec4 lp = rt_local_pos[i];
            vec4 ld = rt_local_dir[i];
            vec4 params = rt_local_params[i];
            vec3 to_light = lp.xyz - pos;
            float d = length(to_light);
            if (d <= 1e-3 || d > lp.w) continue;
            vec3 l = to_light / d;
            float ndotl = dot(n, l);
            if (ndotl <= 0.0) continue;
            float cone = 1.0;
            if (ld.w > -1.5) { // spot: outside the cone, and getSpotAttenuation inside it
                float angle_cos = dot(l, ld.xyz);
                if (angle_cos <= ld.w) continue;
                cone = smoothstep(ld.w, params.y, angle_cos);
            }
            // what the light adds here (basic.frag: colour * attenuation * N.L / PI * albedo): below an
            // 8-bit step the shadow would not show, so the ray is not worth shooting
            if (params.z * cone * rt_local_attenuation(d, lp.w, params.x) * ndotl * 0.31830988618 < rt_local_min) continue;
            uvec3 seed = uvec3(uint(gl_FragCoord.x) + 311u, uint(gl_FragCoord.y) + 557u, uint(frame_index * 8 + i));
            float r1 = rt_hash(seed), r2 = rt_hash(seed + uvec3(5u, 71u, 23u));
            vec3 t, b;
            rt_basis(l, t, b);
            float radius = sqrt(r1) * rt_local_radius;
            float phi = r2 * 6.28318530718;
            vec3 target = lp.xyz + (t * cos(phi) + b * sin(phi)) * radius;
            vec3 dir = target - origin;
            float t_max = length(dir);
            if (t_max <= 0.05) continue; // the surface is at the light: nothing fits in between
            dir /= t_max;
            if (rt_occluded(origin, dir, t_max, steps)) lv[i] = 0.0;
        }
        frag_local = vec4(rt_local_pack(lv[0], lv[1]), rt_local_pack(lv[2], lv[3]), rt_local_pack(lv[4], lv[5]), rt_local_pack(lv[6], lv[7]));
    }
    frag_out = vec4(visibility, ao, float(steps) / 256.0, 1.0);
}
