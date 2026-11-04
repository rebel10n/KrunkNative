#version 450 core
// Ray tracing debug blit (console `rt_debug <what>`, client/src/rt/rt_pass.c): draws one of the
// tracer's intermediates over the frame.
//   mode 1 bvh         traversal steps as a heat map (blue 0 .. red 256+)
//   mode 2 normals     prepass world normal
//   mode 3 visibility  the raw traced sun visibility
//   mode 4 history     the denoised result (R) and its history length (G / 32)
//   mode 5 ao          the denoised ambient occlusion (G) as grey
//   mode 6 local       the local light visibilities: lights 0..3 as grey in four quadrants
//                      (top-left 0, top-right 1, bottom-left 2, bottom-right 3)
//   mode 8 motion      the prepass motion vector (phase 6; 7 is the reflection view)
//   mode 9 gi          the denoised one-bounce GI radiance (rt_gi.frag)
uniform sampler2D rt_trace_tex;
uniform sampler2D rt_normal;
uniform sampler2D rt_result;
uniform sampler2D rt_refl_tex; // mode 7: the denoised reflection radiance (phase 4)
uniform sampler2D rt_local;    // mode 6: the packed local visibilities (rt.h)
uniform sampler2D rt_velocity; // mode 8: the prepass motion vector (uv delta, previous view depth, written)
uniform sampler2D rt_gi_tex;   // mode 9: the denoised GI radiance
uniform int mode;

float local_unpacked(float word, int which) {
    float hi = floor(word / 4096.0);
    return (which == 0 ? hi : word - hi * 4096.0) / 4095.0;
}

in vec2 uv;
out vec4 frag_out;

vec3 heat(float x) {
    x = clamp(x, 0.0, 1.0);
    return clamp(vec3(x * 3.0 - 1.5, 1.5 - abs(x * 3.0 - 1.5), 1.5 - x * 3.0), 0.0, 1.0);
}

void main() {
    if (mode == 1) frag_out = vec4(heat(texture(rt_trace_tex, uv).b), 1.0);
    else if (mode == 2) frag_out = vec4(texture(rt_normal, uv).xyz * 0.5 + 0.5, 1.0);
    else if (mode == 3) frag_out = vec4(vec3(texture(rt_trace_tex, uv).r), 1.0);
    else if (mode == 4) { vec4 r = texture(rt_result, uv); frag_out = vec4(r.r, r.a / 32.0, 0.0, 1.0); }
    else if (mode == 5) frag_out = vec4(vec3(texture(rt_result, uv).g), 1.0);
    else if (mode == 6) {
        vec2 q = fract(uv * 2.0);
        int light = (uv.x >= 0.5 ? 1 : 0) + (uv.y < 0.5 ? 2 : 0); // uv.y is up: the top row holds 0 and 1
        vec4 words = texture(rt_local, q);
        float v = light < 2 ? local_unpacked(words.x, light) : local_unpacked(words.y, light - 2);
        frag_out = vec4(vec3(v), 1.0);
    }
    else if (mode == 7) frag_out = vec4(texture(rt_refl_tex, uv).rgb, 1.0);
    else if (mode == 8) {
        // motion: the uv delta scaled x20 about mid grey (red = moved right on screen, green = up),
        // blue where the previous camera could not see the point, black on the sky
        vec4 v = texture(rt_velocity, uv);
        if (v.w < 0.5) frag_out = vec4(0.0, 0.0, 0.0, 1.0);
        else if (v.z <= 0.0) frag_out = vec4(0.0, 0.0, 1.0, 1.0);
        else frag_out = vec4(clamp(v.xy * 20.0 + 0.5, 0.0, 1.0), 0.0, 1.0);
    }
    else if (mode == 9) frag_out = vec4(texture(rt_gi_tex, uv).rgb, 1.0);
    else frag_out = vec4(0.0);
}
