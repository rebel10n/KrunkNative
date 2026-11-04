#version 450 core
// Ray tracing: volumetric sun shafts (client/src/rt/rt_pass.c shafts_pass, rtShafts; doc/raytracing.md).
// One fullscreen triangle at RT resolution / RT_SHAFTS_DIVISOR. Each pixel walks its view ray from
// the camera to the prepass surface (the sky: the fog's far distance) and asks, at RT_SHAFTS_STEPS
// points, whether the sun reaches that point of the air (one any-hit shadow ray each, the same
// traversal as the sun shadow; the local player's shadow puppet is seen, so your own shadow cuts
// the shafts too).
//
// Two media share one march. Krunker's fog is THREE.Fog: the fog share of a view ray is
// F(d) = smoothstep(fog_near, fog_far, d) (basic.frag krunker_fog), almost nothing in the first 100
// units, so on its own it would put shafts only in the distance. The light that scatters toward the
// eye is therefore carried by a thin exponential haze of its own, M(d) = 1 - exp(-density * d) with
// density = RT_SHAFTS_DENSITY / fog_far (a map's fog distance sets how hazy it is), which shows shafts
// between the nearby buildings too; the march spreads its samples evenly in M (inverse of M, jittered
// per pixel and frame for the temporal pass), so each carries the weight M(d) / N, and per sample
// slice [b_i, b_i+1] also the share of Krunker's fog F(b_i+1) - F(b_i) it covers. With V the sun
// visibility at the sample (one any-hit shadow ray):
//   R  lit haze        sum V * M(d) / N              (the composite adds sun light * phase * R)
//   G  haze share      M(d)
//   B  lit fog         sum V * (F(b_i+1) - F(b_i))    (the composite removes the shadowed F(d) - B)
//   A  the march end's distance from the camera (the composite's upsample guide)
#include "rt_common.glsl"

#define RT_SHAFTS_STEPS 10

uniform sampler2D rt_depth;    // prepass depth (RT resolution)
uniform mat4 inverse_view_projection;
uniform vec3 camera_position;
uniform vec3 sun_direction;    // normalised, towards the sun
uniform float sun_tan_radius;  // tan(rtSunRadius): the shafts soften like the shadows
uniform float fog_near;        // the fog (or rt_pass.c's default haze on a map without fog)
uniform float fog_far;
uniform float density;       // RT_SHAFTS_DENSITY / fog_far: the haze that carries the in-scattering
uniform int frame_index;

in vec2 uv;
out vec4 frag_out;

vec3 world_at(vec2 at, float depth) {
    vec4 clip = vec4(at * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 world = inverse_view_projection * clip;
    return world.xyz / world.w;
}

float fog_amount(float d) {
    return smoothstep(fog_near, fog_far, d);
}

float haze_amount(float d) {
    return 1.0 - exp(-density * d);
}

// inverse of haze_amount
float haze_distance(float y) {
    return -log(max(1.0 - y, 1e-6)) / density;
}

// interleaved gradient noise (Jimenez 2014), advanced by the frame: the temporal pass averages it
float ign(vec2 p, int frame) {
    p += 5.588238 * float(frame & 63);
    return fract(52.9829189 * fract(0.06711056 * p.x + 0.00583715 * p.y));
}

void main() {
    float depth = texture(rt_depth, uv).r;
    vec3 dir;
    float d;
    if (depth < 1.0) {
        vec3 to = world_at(uv, depth) - camera_position;
        d = length(to);
        dir = to / max(d, 1e-6);
    } else {
        dir = normalize(world_at(uv, 0.5) - camera_position); // the sky: through the whole fog
        d = fog_far;
    }
    float haze = haze_amount(d);
    if (haze < 1e-4 || rt_tlas_count <= 0) {
        frag_out = vec4(0.0, haze, 0.0, d);
        return;
    }

    vec3 sun = sun_direction;
    float jitter = ign(gl_FragCoord.xy, frame_index);
    if (sun_tan_radius > 0.0) {
        uvec3 seed = uvec3(uint(gl_FragCoord.x), uint(gl_FragCoord.y), uint(frame_index) + 911u);
        float r1 = rt_hash(seed), r2 = rt_hash(seed + uvec3(17u, 3u, 61u));
        vec3 t, b;
        rt_basis(sun_direction, t, b);
        float phi = r2 * 6.28318530718;
        sun = normalize(sun_direction + (t * cos(phi) + b * sin(phi)) * sqrt(r1) * sun_tan_radius);
    }

    int steps = 0;
    float lit = 0.0, lit_fog = 0.0;
    float b0 = 0.0, f0 = 0.0;
    for (int i = 0; i < RT_SHAFTS_STEPS; i++) {
        float b1 = min(haze_distance(haze * float(i + 1) / float(RT_SHAFTS_STEPS)), d);
        float f1 = fog_amount(b1);
        float t = mix(b0, b1, jitter);
        if (!rt_occluded(camera_position + dir * t, sun, 20000.0, steps)) {
            lit += 1.0;
            lit_fog += f1 - f0;
        }
        b0 = b1;
        f0 = f1;
    }
    frag_out = vec4(haze * lit / float(RT_SHAFTS_STEPS), haze, lit_fog, d);
}
