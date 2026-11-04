#version 450 core
// Ray tracing GI denoiser (client/src/rt/rt_pass.c, RT_EFFECT_GI), rt_denoise.frag's scheme for the
// rgb radiance of rt_gi.frag (one cosine-weighted sample per pixel and frame is far too noisy alone):
//   pass 0  temporal: reproject this pixel into the previous frame (the prepass motion vector when it
//           was written, else the previous camera), blend the GI history where the main denoiser's
//           history (rt_depth_history.b: the stored view depth) says it is the same surface; 32 frames
//           on the static world, 16 on a moving mesh with a motion vector, 4 without. Writes (rgb,
//           a history length).
//   pass 1  spatial: one 5x5 a-trous step (uniform `step_size`) with depth / normal edge stops only:
//           bounce light is low frequency, so the driver runs three widening steps (1, 2, 4).
uniform sampler2D rt_input;         // pass 0: rt_gi's output; pass 1: the previous level
uniform sampler2D rt_history;       // pass 0: the GI history
uniform sampler2D rt_depth_history; // pass 0: the main denoiser's previous history (b = view depth, a = length)
uniform sampler2D rt_depth;
uniform sampler2D rt_normal;        // xyz normal, w = dynamic flag
uniform sampler2D rt_velocity;      // prepass motion vector (rt_denoise.frag's layout)
uniform int rt_velocity_valid;
uniform mat4 inverse_view_projection;
uniform mat4 previous_view_projection;
uniform vec3 camera_position;
uniform int pass;
uniform int step_size;
uniform int history_valid;
uniform int rt_gi_gradient;         // pass 0: 1 = clip the history to this frame's 3x3 band (KRUNKNATIVE_RT_NO_GRADIENT=1 off)
uniform vec2 rt_texel;

in vec2 uv;
out vec4 frag_out;

vec3 world_at(vec2 at, float depth) {
    vec4 clip = vec4(at * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 world = inverse_view_projection * clip;
    return world.xyz / world.w;
}

// rt_denoise.frag reproject(): where this pixel's surface point was last frame
bool reproject(vec3 pos, out vec2 prev_uv, out float view_depth, out bool moved) {
    moved = false;
    if (rt_velocity_valid != 0) {
        vec4 v = texture(rt_velocity, uv);
        if (v.w > 0.5) {
            if (v.z <= 0.0) return false;
            prev_uv = uv - v.xy;
            view_depth = v.z;
            moved = true;
            return all(greaterThanEqual(prev_uv, vec2(0.0))) && all(lessThanEqual(prev_uv, vec2(1.0)));
        }
    }
    vec4 prev_clip = previous_view_projection * vec4(pos, 1.0);
    if (prev_clip.w <= 0.0) return false;
    view_depth = prev_clip.w;
    prev_uv = prev_clip.xy / prev_clip.w * 0.5 + 0.5;
    return all(greaterThanEqual(prev_uv, vec2(0.0))) && all(lessThanEqual(prev_uv, vec2(1.0)));
}

float max_history(float dynamic_flag, bool moved) {
    if (dynamic_flag <= 0.5) return 32.0;
    return moved ? 16.0 : 4.0;
}

void main() {
    vec4 current = texture(rt_input, uv);
    float depth = texture(rt_depth, uv).r;
    if (depth >= 1.0) {
        frag_out = vec4(0.0);
        return;
    }
    vec3 pos = world_at(uv, depth);
    vec4 nd = texture(rt_normal, uv);
    vec3 n = nd.xyz;

    if (pass == 0) {
        vec2 prev_uv;
        float view_depth;
        bool moved;
        float history_length = 0.0;
        vec3 value = current.rgb;
        if (history_valid != 0 && reproject(pos, prev_uv, view_depth, moved)) {
            vec4 hist = texture(rt_history, prev_uv);
            vec4 main_hist = texture(rt_depth_history, prev_uv);
            float tolerance = 0.01 * view_depth + 0.2;
            if (hist.a > 0.0 && main_hist.a > 0.0 && abs(main_hist.b - view_depth) < tolerance) {
                history_length = min(hist.a, max_history(nd.w, moved));
                vec3 h = hist.rgb;
                if (rt_gi_gradient != 0) {
                    // temporal gradient (rt_denoise.frag's AO clip, per channel): this frame's 3x3 GI mean and spread;
                    // a history outside mean +- 2.5 sigma (+ a floor relative to the mean) belongs to a lighting that
                    // changed (a moving object's bounce or occlusion, a door, an emissive that moved): it is clipped
                    // into the band and kept young. One noisy sample per pixel makes sigma large on a static surface,
                    // so a converged history (~ the mean) is rarely touched.
                    vec3 s = vec3(0.0), sq = vec3(0.0);
                    float n_ok = 0.0;
                    for (int y = -1; y <= 1; y++) {
                        for (int x = -1; x <= 1; x++) {
                            vec2 at = uv + vec2(float(x), float(y)) * rt_texel;
                            if (texture(rt_depth, at).r >= 1.0) continue;
                            vec3 c = texture(rt_input, at).rgb;
                            s += c;
                            sq += c * c;
                            n_ok += 1.0;
                        }
                    }
                    if (n_ok >= 3.0) {
                        vec3 mean = s / n_ok;
                        vec3 sigma = sqrt(max(sq / n_ok - mean * mean, vec3(0.0)));
                        vec3 band = 2.5 * sigma + 0.1 * mean + 0.01;
                        vec3 clipped = clamp(h, mean - band, mean + band);
                        if (any(greaterThan(abs(clipped - h), 0.05 * mean + 0.01))) history_length = min(history_length, 4.0);
                        h = clipped;
                    }
                }
                value = mix(h, current.rgb, 1.0 / (history_length + 1.0));
            }
        }
        frag_out = vec4(value, min(history_length + 1.0, 32.0));
    } else {
        float depth_scale = 1.0 / max(length(pos - camera_position) * 0.03, 1e-3);
        vec3 sum = vec3(0.0);
        float weight_sum = 0.0;
        for (int y = -2; y <= 2; y++) {
            for (int x = -2; x <= 2; x++) {
                vec2 at = uv + vec2(float(x), float(y)) * float(step_size) * rt_texel;
                float d = texture(rt_depth, at).r;
                if (d >= 1.0) continue;
                vec4 sample_nd = texture(rt_normal, at);
                vec3 p = world_at(at, d);
                float plane_distance = length(p - pos);
                float dot_n = max(dot(n, sample_nd.xyz), 0.0);
                float kernel = (abs(x) == 2 ? 0.0625 : abs(x) == 1 ? 0.25 : 0.375) * (abs(y) == 2 ? 0.0625 : abs(y) == 1 ? 0.25 : 0.375);
                float w = kernel * exp(-plane_distance * depth_scale * 0.5) * pow(dot_n, 4.0);
                sum += texture(rt_input, at).rgb * w;
                weight_sum += w;
            }
        }
        frag_out = vec4(weight_sum > 0.0 ? sum / weight_sum : current.rgb, current.a);
    }
}
