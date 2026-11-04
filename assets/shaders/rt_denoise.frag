#version 450 core
// Ray tracing denoiser (client/src/rt/rt_pass.c). Two programs from one source, picked by `pass`:
//   pass 0  temporal: reproject this pixel into the previous frame (the prepass motion vector,
//           rt_velocity, which carries mesh and camera motion; previous_view_projection alone
//           without it), blend the history where its stored view depth agrees (disocclusion
//           test), keep less history on dynamic pixels (prepass normal.w; see max_history).
//           Writes (R vis, G ao, B view depth, A history length) for the next frame.
//   pass 1  spatial: one a-trous step (uniform `step_size`) of a 5x5 edge-aware blur on R and G
//           with depth and normal weights; B and A pass through. `filter_mask` picks the channels
//           a step filters (the others pass through): AO (G) is lower frequency than the shadow
//           edges (R), so it gets a wider depth stop, a softer normal stop and a third, wider
//           step that leaves R alone (rt_pass.c denoise()).
//   pass 2  temporal, local lights (phase 5): rt_input / rt_history hold the packed 8 local light
//           visibilities (rt.h: 2 per channel x 12 bits); each is unpacked, blended with the same
//           reprojection test as pass 0 (rt_history_main is pass 0's previous history: its stored
//           view depth and length) and repacked. No spatial step: the lights are small.
uniform sampler2D rt_input;     // pass 0: the trace output; pass 1: the previous a-trous level; pass 2: the packed local trace
uniform sampler2D rt_history;   // pass 0 and 2 only
uniform sampler2D rt_history_main; // pass 2: pass 0's history of the previous frame (depth / length test)
uniform sampler2D rt_depth;
uniform sampler2D rt_normal;
uniform mat4 inverse_view_projection;
uniform mat4 previous_view_projection;
uniform mat4 view_projection;   // current: its clip w is the view depth stored for the next frame
uniform vec3 camera_position;
uniform int pass;
uniform int step_size;
uniform vec2 filter_mask;       // pass 1: 1 = filter that channel (R, G), 0 = pass it through
uniform int history_valid;      // 0: first frame after a reset, nothing to reproject
uniform int rt_ao_gradient;     // pass 0: 1 = clip the AO history to this frame's 3x3 band (rt_pass.c; KRUNKNATIVE_RT_NO_GRADIENT=1 off)
uniform vec2 rt_texel;

in vec2 uv;
out vec4 frag_out;

vec3 world_at(vec2 at, float depth) {
    vec4 clip = vec4(at * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 world = inverse_view_projection * clip;
    return world.xyz / world.w;
}

// rt.h local light packing: two 12-bit visibilities per channel
vec2 local_unpack(float word) {
    float hi = floor(word / 4096.0);
    return vec2(hi, word - hi * 4096.0) / 4095.0;
}

float local_pack(vec2 v) {
    return floor(clamp(v.x, 0.0, 1.0) * 4095.0 + 0.5) * 4096.0 + floor(clamp(v.y, 0.0, 1.0) * 4095.0 + 0.5);
}

uniform int rt_local_mask; // pass 2: bit i = slot i was traced this frame (else its history is held)
uniform int rt_local_fresh; // pass 2: bit i = slot i is a transient light (muzzle flash): no history, the current value

vec2 local_blend(vec2 hist, vec2 cur, float alpha, int bits) {
    return vec2((bits & 1) != 0 ? mix(hist.x, cur.x, alpha) : hist.x, (bits & 2) != 0 ? mix(hist.y, cur.y, alpha) : hist.y);
}
// local_blend with transient (muzzle-flash) slots: a fresh bit takes the current value whole (a flash lasts a few
// frames; history would smear its shadow over the next flash at another place)
vec2 local_blend_fresh(vec2 hist, vec2 cur, float alpha, int bits, int fresh) {
    vec2 blended = local_blend(hist, cur, alpha, bits);
    return vec2((fresh & 1) != 0 ? cur.x : blended.x, (fresh & 2) != 0 ? cur.y : blended.y);
}

// Motion vectors (phase 6, rt_prepass.frag frag_velocity): .xy = uv now minus uv last frame (mesh and
// camera motion), .z = the point's view depth last frame (-1: behind the previous camera), .w = 1 where
// the prepass wrote it. rt_velocity_valid 0 (no target): the camera-only reprojection of the static world.
uniform sampler2D rt_velocity;
uniform int rt_velocity_valid;

// Where this pixel's surface point was last frame: prev_uv and its view depth then. 0 = nowhere usable.
// With a velocity the moving meshes reproject onto themselves, so their history can stay longer; the
// static world gets the same answer either way (velocity = camera motion).
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

// History a pixel may keep: 32 frames on the static world; a dynamic mesh (prepass normal.w) 16 when its
// motion vector carried the history along, 4 when only the camera was reprojected (the old policy).
float max_history(float dynamic_flag, bool moved) {
    if (dynamic_flag <= 0.5) return 32.0;
    return moved ? 16.0 : 4.0;
}

void main() {
    vec4 current = texture(rt_input, uv);
    float depth = texture(rt_depth, uv).r;
    if (depth >= 1.0) {
        frag_out = pass == 0 ? vec4(current.rg, 0.0, 0.0) : current;
        return;
    }
    vec3 pos = world_at(uv, depth);
    vec4 nd = texture(rt_normal, uv);
    vec3 n = nd.xyz;

    if (pass == 2) {
        vec2 prev_uv;
        float view_depth;
        bool moved;
        frag_out = current;
        if (history_valid != 0 && reproject(pos, prev_uv, view_depth, moved)) { // motion vectors (phase 6)
            vec4 main_hist = texture(rt_history_main, prev_uv);
            float tolerance = 0.01 * view_depth + 0.2;
            if (main_hist.a > 0.0 && abs(main_hist.b - view_depth) < tolerance) {
                float history_length = min(main_hist.a, max_history(nd.w, moved));
                float alpha = 1.0 / (history_length + 1.0);
                // the history was written with nearest sampling in mind: fetch the texel the
                // reprojected point falls in, filtering packed values would mix the lights
                vec4 hist = texelFetch(rt_history, ivec2(prev_uv / rt_texel), 0);
                // a slot not traced this frame (rt_local_mask, round robin) keeps its history as is
                vec4 blended;
                blended.x = local_pack(local_blend_fresh(local_unpack(hist.x), local_unpack(current.x), alpha, rt_local_mask, rt_local_fresh));
                blended.y = local_pack(local_blend_fresh(local_unpack(hist.y), local_unpack(current.y), alpha, rt_local_mask >> 2, rt_local_fresh >> 2));
                blended.z = local_pack(local_blend_fresh(local_unpack(hist.z), local_unpack(current.z), alpha, rt_local_mask >> 4, rt_local_fresh >> 4));
                blended.w = local_pack(local_blend_fresh(local_unpack(hist.w), local_unpack(current.w), alpha, rt_local_mask >> 6, rt_local_fresh >> 6));
                frag_out = blended;
            }
        }
        return;
    }

    if (pass == 0) {
        vec2 prev_uv;
        float view_depth;
        bool moved;
        float history_length = 0.0;
        vec2 value = current.rg;
        if (history_valid != 0 && reproject(pos, prev_uv, view_depth, moved)) { // motion vectors (phase 6)
            vec4 hist = texture(rt_history, prev_uv);
            // the same surface: the stored view depth matches what this point had last frame
            float tolerance = 0.01 * view_depth + 0.2;
            if (hist.a > 0.0 && abs(hist.b - view_depth) < tolerance) {
                history_length = hist.a;
                history_length = min(history_length, max_history(nd.w, moved)); // dynamic pixels forget faster
                // A shadow sweeping over a static surface (a walking player's) reprojects perfectly and
                // still trails: the surface's history holds the old visibility. Clamp the history's R to
                // the 3x3 neighbourhood of this frame's sun visibility (0 / 1 per pixel): inside a lit or
                // shadowed area the neighbourhood agrees and the history snaps to it, in a penumbra it
                // spans [0, 1] and clamps nothing (the soft edge keeps accumulating). AO (G) is too noisy
                // at a few samples for the same treatment and keeps its full history.
                float vis_min = current.r, vis_max = current.r;
                // AO (G) temporal gradient: the 3x3 mean / spread of this frame's AO samples (untraced -1 entries
                // skipped). A moving caster changes the occlusion of the static surface under it while the surface
                // reprojects perfectly; with 32 frames of history its contact darkness lingered about half a second
                // behind a walking player or a moving prop. History outside mean +- 2 sigma (plus a floor for the
                // few-sample noise) is clipped to that band and kept young, so a real change shows within a few
                // frames while the static image (history ~ the true mean, inside the band) keeps its full length.
                float ao_sum = 0.0, ao_sq = 0.0, ao_n = 0.0;
                if (current.g >= 0.0) { ao_sum = current.g; ao_sq = current.g * current.g; ao_n = 1.0; }
                for (int y = -1; y <= 1; y++) {
                    for (int x = -1; x <= 1; x++) {
                        if (x == 0 && y == 0) continue;
                        vec2 at = uv + vec2(float(x), float(y)) * rt_texel;
                        if (texture(rt_depth, at).r >= 1.0) continue;
                        vec2 v = texture(rt_input, at).rg;
                        vis_min = min(vis_min, v.r);
                        vis_max = max(vis_max, v.r);
                        if (v.g >= 0.0) { ao_sum += v.g; ao_sq += v.g * v.g; ao_n += 1.0; }
                    }
                }
                float hist_vis = clamp(hist.r, vis_min, vis_max);
                if (hist_vis != hist.r) history_length = min(history_length, 4.0); // a clamped history is a young one
                float hist_ao = hist.g;
                if (ao_n >= 3.0 && rt_ao_gradient != 0) {
                    float mean = ao_sum / ao_n;
                    float sigma = sqrt(max(ao_sq / ao_n - mean * mean, 0.0));
                    float band = 2.0 * sigma + 0.08;
                    float clipped = clamp(hist.g, mean - band, mean + band);
                    if (abs(clipped - hist.g) > 0.02) history_length = min(history_length, 4.0);
                    hist_ao = clipped;
                }
                float alpha = 1.0 / (history_length + 1.0);
                value = mix(vec2(hist_vis, hist_ao), current.rg, alpha);
                // Box Office specular occlusion at rtMode 1 (rt_trace.frag rt_sky_vis 2): G < 0 = not traced this
                // frame (a quarter of the pixels are, rotating): the history's value stands
                if (current.g < 0.0) value.y = hist.g;
            }
        }
        if (value.y < 0.0) {
            // untraced without a history (reset, disocclusion) at rtMode 1's opt-in sky-visibility ray (8 x 8 cells, a
            // quarter traced each frame): the pixels one cell away that were traced stand in, else open until a ray lands
            float fill = 0.0, fill_n = 0.0;
            for (int k = 0; k < 4; k++) {
                vec2 off = k == 0 ? vec2(-8.0, 0.0) : k == 1 ? vec2(8.0, 0.0) : k == 2 ? vec2(0.0, -8.0) : vec2(0.0, 8.0);
                vec2 at = uv + off * rt_texel;
                if (texture(rt_depth, at).r >= 1.0) continue;
                float g = texture(rt_input, at).g;
                if (g < 0.0) continue;
                fill += g;
                fill_n += 1.0;
            }
            value.y = fill_n > 0.0 ? fill / fill_n : 1.0;
        }
        float view_depth_now = (view_projection * vec4(pos, 1.0)).w;
        frag_out = vec4(value, view_depth_now, min(history_length + 1.0, 32.0));
    } else {
        float depth_scale = 1.0 / max(length(pos - camera_position) * 0.02, 1e-3);
        vec2 sum = vec2(0.0);
        vec2 weight_sum = vec2(0.0);
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
                // R (shadow): tight stops keep the penumbra edges; G (AO): twice the depth reach and a
                // softer normal stop (corners of a box still separate at dot 0, which both stops keep)
                vec2 w = kernel * vec2(exp(-plane_distance * depth_scale) * pow(dot_n, 16.0),
                                       exp(-plane_distance * depth_scale * 0.5) * pow(dot_n, 4.0));
                sum += texture(rt_input, at).rg * w;
                weight_sum += w;
            }
        }
        vec2 filtered = vec2(weight_sum.x > 0.0 ? sum.x / weight_sum.x : current.r, weight_sum.y > 0.0 ? sum.y / weight_sum.y : current.g);
        vec2 value = mix(current.rg, filtered, filter_mask);
        frag_out = vec4(value, current.ba);
    }
}
