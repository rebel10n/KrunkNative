#version 450 core
// Ray tracing: temporal accumulation of the volumetric sun shafts (client/src/rt/rt_pass.c, rtShafts).
// The shafts are a property of the view ray, not of a surface, so the reprojection takes the march's
// end point (rt_shafts.frag A: its distance along this pixel's ray) into the previous camera: the
// surface behind the pixel, or a point deep in the fog for the sky. Moving meshes are ignored (their
// share of the air is small); the 3x3 neighbourhood clamp of R / G / B keeps a reprojected history from
// trailing when the camera or a shadow moves. Writes (R, G, B blended; A current).
uniform sampler2D rt_input;    // rt_shafts.frag's output this frame
uniform sampler2D rt_history;  // the previous accumulated result
uniform mat4 inverse_view_projection;
uniform mat4 previous_view_projection;
uniform vec3 camera_position;
uniform int history_valid;
uniform vec2 rt_texel;         // 1 / the shafts target size

in vec2 uv;
out vec4 frag_out;

vec3 world_at(vec2 at, float depth) {
    vec4 clip = vec4(at * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 world = inverse_view_projection * clip;
    return world.xyz / world.w;
}

void main() {
    vec4 current = texture(rt_input, uv);
    if (history_valid == 0) {
        frag_out = current;
        return;
    }
    vec3 lo = current.rgb, hi = current.rgb;
    for (int y = -1; y <= 1; y++) {
        for (int x = -1; x <= 1; x++) {
            vec3 s = texture(rt_input, uv + vec2(float(x), float(y)) * rt_texel).rgb;
            lo = min(lo, s);
            hi = max(hi, s);
        }
    }
    vec3 dir = normalize(world_at(uv, 0.5) - camera_position);
    vec3 end = camera_position + dir * current.a;
    vec4 prev = previous_view_projection * vec4(end, 1.0);
    if (prev.w <= 1e-4) {
        frag_out = current;
        return;
    }
    vec2 prev_uv = prev.xy / prev.w * 0.5 + 0.5;
    if (any(lessThan(prev_uv, vec2(0.0))) || any(greaterThan(prev_uv, vec2(1.0)))) {
        frag_out = current;
        return;
    }
    vec4 previous = texture(rt_history, prev_uv);
    // a different surface (or the sky) behind the pixel last frame: its shafts do not belong here (the clamp
    // alone lets a sky history slide onto a roof edge, whose 3x3 neighbourhood holds sky too)
    if (abs(previous.a - current.a) > 0.1 * max(current.a, 1.0)) {
        frag_out = current;
        return;
    }
    vec3 history = clamp(previous.rgb, lo, hi);
    // 1/10 of the new frame: 10 jittered steps x 10 frames resolve the shafts, the clamp keeps them crisp
    frag_out = vec4(mix(history, current.rgb, 0.1), current.a);
}
