#version 450 core
// Temporal anti-aliasing resolve (client/src/render_target.c, rtTaa): the scene frame, rendered with a
// sub-pixel camera jitter (camera_set_jitter, Halton 2/3 over 8 frames), blended into a history that is
// reprojected with the ray tracer's motion vectors (rt_prepass.frag frag_velocity at RT resolution,
// bilinearly upsampled). Where no world surface wrote a velocity the pixel is reprojected as a far-plane point
// with the camera motion alone (sky_reproject) - except the first-person viewmodel: it is drawn with its own
// camera, locked to the view, so it does not move on screen when the camera turns. Reprojecting it with the
// camera motion pulled its history from wherever the turn came from (the world), which smeared and doubled a
// small detailed item like the wrist watch on every turn; a same-place history then lagged behind the walking bob
// and sway. Viewmodel pixels (scene_depth < 1: main.c clears the depth before the fps scene, so only its pixels
// keep a depth) are drawn unjittered (main.c) and taken from the current frame, no history. The history is
// clamped to the 3x3 neighbourhood of the current frame (RGB min/max) so stale colour cannot ghost, then
// blended 1/10 current : 9/10 history, and lightly sharpened against the local mean to offset the blur
// that a temporal filter adds. Output RGBA8 at scene resolution.
in vec2 uv;
out vec4 frag_color;

uniform sampler2D scene;         // unit 0: this frame's (MSAA-resolved) scene colour
uniform sampler2D history;       // unit 1: the previous resolve
uniform sampler2D velocity;      // unit 2: rt_prepass.frag's motion vectors (.xy uv delta, .w written)
uniform sampler2D scene_depth;   // unit 3: this frame's scene depth (render_target.c copies it); < 1 = viewmodel
uniform bool depth_valid;        // 0: no depth copy (every pixel is treated as the world, as before)
uniform vec2 texel;              // 1 / scene size
uniform mat4 sky_reproject;      // previous_view_projection * inverse(view_projection), unjittered
uniform bool history_valid;
uniform float blend;             // weight of the current frame (0.1)
uniform float sharpen;           // 0 = none
// Bloom (rtBloom, render_target.c): the scene and the history are half-float and the alpha carries the bloom
// weight (basic.frag: 1 + weight); it is reprojected, clamped and blended like the colour, and rgb may exceed 1.
uniform bool keep_alpha;

void main() {
    // the scene target is the resolve's own size: this fragment's texel and its 3x3 neighbourhood are read with
    // texelFetch (no filtering; the fragment centre is a texel centre, so the filtered reads returned the same)
    ivec2 pix = ivec2(gl_FragCoord.xy);
    ivec2 limit = textureSize(scene, 0) - 1;
    vec4 current4 = texelFetch(scene, pix, 0);
    vec3 current = current4.rgb;
    float out_alpha = keep_alpha ? current4.a : 1.0;
    if (!history_valid) {
        frag_color = vec4(current, out_alpha);
        return;
    }
    vec4 v = texture(velocity, uv);
    vec2 prev_uv;
    bool viewmodel = depth_valid && texelFetch(scene_depth, pix, 0).r < 1.0;
    if (viewmodel) {
        // The viewmodel has no motion vectors, and it moves on screen every frame while walking (bob, sway), aiming,
        // recoiling and swapping: any history read for it lags behind and smeared the watch dial and doubled the
        // sights. main.c draws the fps scene without the TAA jitter, so its pixels are stable as they are: take the
        // current frame (MSAA still smooths its edges).
        frag_color = vec4(current, out_alpha);
        return;
    } else if (v.w > 0.5) {
        prev_uv = uv - v.xy;
    } else {
        // no velocity (sky, viewmodel, nothing drawn): a far-plane point under the camera's own motion
        vec4 clip = vec4(uv * 2.0 - 1.0, 1.0, 1.0);
        vec4 prev = sky_reproject * clip;
        prev_uv = prev.w > 0.0 ? prev.xy / prev.w * 0.5 + 0.5 : uv;
    }
    bool inside = prev_uv.x >= 0.0 && prev_uv.x <= 1.0 && prev_uv.y >= 0.0 && prev_uv.y <= 1.0;

    // 3x3 neighbourhood bounds of the current frame; the history is clamped into them
    vec3 lo = current, hi = current, sum = current;
    float alo = current4.a, ahi = current4.a;
    for (int y = -1; y <= 1; y++) {
        for (int x = -1; x <= 1; x++) {
            if (x == 0 && y == 0) continue;
            vec4 c4 = texelFetch(scene, clamp(pix + ivec2(x, y), ivec2(0), limit), 0); // CLAMP_TO_EDGE
            vec3 c = c4.rgb;
            lo = min(lo, c);
            hi = max(hi, c);
            sum += c;
            alo = min(alo, c4.a);
            ahi = max(ahi, c4.a);
        }
    }
    vec3 mean = sum / 9.0;
    vec3 out_color = current;
    if (inside) {
        vec4 prev4 = texture(history, prev_uv);
        vec3 prev = clamp(prev4.rgb, lo, hi);
        out_color = mix(prev, current, blend);
        if (keep_alpha) out_alpha = mix(clamp(prev4.a, alo, ahi), current4.a, blend);
    }
    if (sharpen > 0.0) {
        out_color = out_color + (out_color - mean) * sharpen;
        out_color = keep_alpha ? max(out_color, vec3(0.0)) : clamp(out_color, 0.0, 1.0);
    }
    frag_color = vec4(out_color, out_alpha);
}
