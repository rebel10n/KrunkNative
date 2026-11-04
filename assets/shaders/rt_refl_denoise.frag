#version 450 core
// Ray tracing reflection denoiser (client/src/rt/rt_pass.c, phase 4), rt_denoise.frag's scheme for
// the rgb radiance of rt_reflect.frag:
//   pass 0  temporal: reproject into the previous frame, blend the reflection history where the main
//           denoiser's history (rt_depth_history.b: the stored view depth) says it is the same surface;
//           mirrors keep less history than rough surfaces (their image moves with the camera).
//           Writes (rgb radiance, a history length).
//   pass 1  spatial: one 5x5 a-trous step with depth / normal stops, scaled by the material's
//           roughness: a mirror (roughness < 0.1) is left sharp, a rough surface gets the full blur.
uniform sampler2D rt_input;         // pass 0: rt_reflect's output; pass 1: the previous level
uniform sampler2D rt_history;       // pass 0: the reflection history
uniform sampler2D rt_depth_history; // pass 0: the main denoiser's previous history (b = view depth)
uniform sampler2D rt_depth;
uniform sampler2D rt_normal;
uniform sampler2D rt_material;      // g = roughness
uniform mat4 inverse_view_projection;
uniform mat4 previous_view_projection;
uniform vec3 camera_position;
uniform int pass;
uniform int step_size;
uniform int history_valid;
uniform vec2 rt_texel;
uniform sampler2D rt_velocity;      // pass 0: the prepass motion vector (rt_denoise.frag's layout)
uniform int rt_velocity_valid;      // 0: no velocity target (the viewmodel pass, KRUNKNATIVE_RT_NO_MOTION)
uniform int rt_refl_gradient;       // pass 0: 1 = clip the history to this frame's 3x3 band (KRUNKNATIVE_RT_NO_GRADIENT=1 off)

in vec2 uv;
out vec4 frag_out;

vec3 world_at(vec2 at, float depth) {
    vec4 clip = vec4(at * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 world = inverse_view_projection * clip;
    return world.xyz / world.w;
}

void main() {
    vec4 current = texture(rt_input, uv);
    float depth = texture(rt_depth, uv).r;
    vec4 material = texture(rt_material, uv);
    if (depth >= 1.0 || material.r <= 0.0) {
        frag_out = vec4(0.0);
        return;
    }
    vec3 pos = world_at(uv, depth);
    vec4 nd = texture(rt_normal, uv);
    vec3 n = nd.xyz;
    float roughness = material.g;

    if (pass == 0) {
        // where this surface point was last frame: the prepass motion vector when one was written (a moving
        // reflective mesh reprojects onto itself; before, it took the camera-only answer and pulled history from
        // whatever was behind it last frame), else the previous camera
        vec4 prev_clip = previous_view_projection * vec4(pos, 1.0);
        float view_depth = prev_clip.w;
        vec2 prev_uv = prev_clip.xy / prev_clip.w * 0.5 + 0.5;
        bool usable = prev_clip.w > 0.0;
        if (rt_velocity_valid != 0) {
            vec4 v = texture(rt_velocity, uv);
            if (v.w > 0.5) {
                prev_uv = uv - v.xy;
                view_depth = v.z;
                usable = v.z > 0.0;
            }
        }
        float history_length = 0.0;
        vec3 value = current.rgb;
        if (history_valid != 0 && usable && all(greaterThanEqual(prev_uv, vec2(0.0))) && all(lessThanEqual(prev_uv, vec2(1.0)))) {
            vec4 hist = texture(rt_history, prev_uv);
            float stored_depth = texture(rt_depth_history, prev_uv).b;
            float tolerance = 0.01 * view_depth + 0.2;
            if (hist.a > 0.0 && abs(stored_depth - view_depth) < tolerance) {
                // a mirror's image slides with the camera: 6 frames; a rough surface's average holds: 24
                float max_history = nd.w > 0.5 ? 4.0 : mix(6.0, 24.0, clamp(roughness * 2.0, 0.0, 1.0));
                history_length = min(hist.a, max_history);
                vec3 h = hist.rgb;
                if (rt_refl_gradient != 0) {
                    // what the surface reflects moved (a player passing a window, the camera's parallax on a mirror):
                    // clip the history into this frame's 3x3 band of reflecting pixels and keep it young
                    vec3 s = vec3(0.0), sq = vec3(0.0);
                    float n_ok = 0.0;
                    for (int y = -1; y <= 1; y++) {
                        for (int x = -1; x <= 1; x++) {
                            vec2 at = uv + vec2(float(x), float(y)) * rt_texel;
                            if (texture(rt_depth, at).r >= 1.0 || texture(rt_material, at).r <= 0.0) continue;
                            vec3 c = texture(rt_input, at).rgb;
                            s += c;
                            sq += c * c;
                            n_ok += 1.0;
                        }
                    }
                    if (n_ok >= 3.0) {
                        vec3 mean = s / n_ok;
                        vec3 sigma = sqrt(max(sq / n_ok - mean * mean, vec3(0.0)));
                        vec3 band = 2.0 * sigma + 0.1 * mean + 0.01;
                        vec3 clipped = clamp(h, mean - band, mean + band);
                        if (any(greaterThan(abs(clipped - h), 0.05 * mean + 0.01))) history_length = min(history_length, 3.0);
                        h = clipped;
                    }
                }
                value = mix(h, current.rgb, 1.0 / (history_length + 1.0));
            }
        }
        frag_out = vec4(value, min(history_length + 1.0, 32.0));
    } else {
        if (roughness < 0.1) {
            frag_out = current;
            return;
        }
        float depth_scale = 1.0 / max(length(pos - camera_position) * 0.02, 1e-3);
        vec3 sum = vec3(0.0);
        float weight_sum = 0.0;
        for (int y = -2; y <= 2; y++) {
            for (int x = -2; x <= 2; x++) {
                vec2 at = uv + vec2(float(x), float(y)) * float(step_size) * rt_texel;
                float d = texture(rt_depth, at).r;
                if (d >= 1.0) continue;
                vec4 sample_material = texture(rt_material, at);
                if (sample_material.r <= 0.0) continue; // only reflecting pixels hold radiance
                vec4 sample_nd = texture(rt_normal, at);
                vec3 p = world_at(at, d);
                float plane_distance = length(p - pos);
                float dot_n = max(dot(n, sample_nd.xyz), 0.0);
                float kernel = (abs(x) == 2 ? 0.0625 : abs(x) == 1 ? 0.25 : 0.375) * (abs(y) == 2 ? 0.0625 : abs(y) == 1 ? 0.25 : 0.375);
                float w = kernel * exp(-plane_distance * depth_scale * 0.5) * pow(dot_n, 8.0);
                sum += texture(rt_input, at).rgb * w;
                weight_sum += w;
            }
        }
        vec3 filtered = weight_sum > 0.0 ? sum / weight_sum : current.rgb;
        // the blur fades in from roughness 0.1 (sharp) to 0.4 (full)
        frag_out = vec4(mix(current.rgb, filtered, smoothstep(0.1, 0.4, roughness)), current.a);
    }
}
