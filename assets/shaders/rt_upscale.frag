#version 450 core
// Edge-adaptive upscale of the scene target to the window (Box Office-only rtSharpen, render_target.c), the first
// of two passes after the TAA resolve; rt_sharpen.frag sharpens its output. Written from the published description
// of AMD FidelityFX Super Resolution 1's EASU (no FSR source used):
//   - 12 taps around the output pixel's input position: the 2x2 bilinear quad (f g j k) and the 8 texels that
//     ring it (b c / e h / i l / n o);
//   - local edge analysis on luma at the four quad texels, each against its cross neighbours, blended with the
//     bilinear weights: a gradient direction and an "edge length" (0 flat .. 1 a clean edge);
//   - each tap is weighted by a windowed lanczos-2 style kernel evaluated in a frame rotated onto that direction
//     and stretched along the edge, so edges are interpolated along their length and stay sharp across it;
//   - the result is clamped to the min / max of the 2x2 quad (deringing).
// Input colours are clamped to [0, 1] (the scene target is RGBA16F with bloom on; the composite does the same).
out vec4 frag_color;

uniform sampler2D scene;       // the scene target or the TAA resolve, scene size (texture unit 0)
uniform vec2 input_size;       // scene target size in texels
uniform vec2 output_size;      // window framebuffer size

vec3 tap(ivec2 p) {
    ivec2 limit = ivec2(input_size) - 1;
    return min(texelFetch(scene, clamp(p, ivec2(0), limit), 0).rgb, vec3(1.0));
}

float luma(vec3 c) {
    return dot(c, vec3(0.299, 0.587, 0.114));
}

// One quad texel's contribution to the direction / length estimate: `c` the texel, a / e above and below it,
// b / d left and right, `w` its bilinear weight.
void analyse(inout vec2 dir, inout float len, float w, float a, float b, float c, float d, float e) {
    float dirx = d - b;
    float lenx = max(abs(d - c), abs(c - b));
    lenx = lenx > 0.0 ? clamp(abs(dirx) / lenx, 0.0, 1.0) : 0.0;
    lenx *= lenx;
    float diry = e - a;
    float leny = max(abs(e - c), abs(c - a));
    leny = leny > 0.0 ? clamp(abs(diry) / leny, 0.0, 1.0) : 0.0;
    leny *= leny;
    dir += vec2(dirx, diry) * w;
    len += (lenx + leny) * w;
}

// Accumulates one tap: `off` is the tap's position relative to the sample point, in input texels.
void accumulate(inout vec3 acc, inout float acc_w, vec3 color, vec2 off, vec2 dir, vec2 len2, float lob, float clp) {
    vec2 v = vec2(off.x * dir.x + off.y * dir.y, -off.x * dir.y + off.y * dir.x) * len2;
    float d2 = min(dot(v, v), clp);
    // windowed lanczos-2 approximation: (25/16 (2/5 x^2 - 1)^2 - 9/16) * (lob x^2 - 1)^2
    float wb = 0.4 * d2 - 1.0;
    float wa = lob * d2 - 1.0;
    float w = (1.5625 * wb * wb - 0.5625) * (wa * wa);
    acc += color * w;
    acc_w += w;
}

void main() {
    vec2 pos = gl_FragCoord.xy * (input_size / output_size) - 0.5; // input position, texel centres at integers
    vec2 base = floor(pos);
    vec2 f = pos - base;
    ivec2 p = ivec2(base);

    vec3 cb = tap(p + ivec2(0, -1)), cc = tap(p + ivec2(1, -1));
    vec3 ce = tap(p + ivec2(-1, 0)), cf = tap(p), cg = tap(p + ivec2(1, 0)), ch = tap(p + ivec2(2, 0));
    vec3 ci = tap(p + ivec2(-1, 1)), cj = tap(p + ivec2(0, 1)), ck = tap(p + ivec2(1, 1)), cl = tap(p + ivec2(2, 1));
    vec3 cn = tap(p + ivec2(0, 2)), co = tap(p + ivec2(1, 2));

    // flat neighbourhood (sky gradients, untextured walls): every tap within 1/1024 of the others, so any
    // normalised weighting of them, clamped to the quad, lands within a quarter of an 8-bit step of cf; the edge
    // analysis and the 12 kernel weights are skipped there.
    vec3 lo12 = min(min(min(min(cb, cc), min(ce, cf)), min(min(cg, ch), min(ci, cj))), min(min(ck, cl), min(cn, co)));
    vec3 hi12 = max(max(max(max(cb, cc), max(ce, cf)), max(max(cg, ch), max(ci, cj))), max(max(ck, cl), max(cn, co)));
    vec3 spread = hi12 - lo12;
    if (max(spread.r, max(spread.g, spread.b)) < 1.0 / 1024.0) {
        frag_color = vec4(cf, 1.0);
        return;
    }

    float lb = luma(cb), lc = luma(cc), le = luma(ce), lf = luma(cf), lg = luma(cg), lh = luma(ch);
    float li = luma(ci), lj = luma(cj), lk = luma(ck), ll = luma(cl), ln = luma(cn), lo = luma(co);

    vec2 dir = vec2(0.0);
    float len = 0.0;
    analyse(dir, len, (1.0 - f.x) * (1.0 - f.y), lb, le, lf, lg, lj);
    analyse(dir, len, f.x * (1.0 - f.y), lc, lf, lg, lh, lk);
    analyse(dir, len, (1.0 - f.x) * f.y, lf, li, lj, lk, ln);
    analyse(dir, len, f.x * f.y, lg, lj, lk, ll, lo);

    float dir_len2 = dot(dir, dir);
    if (dir_len2 < 1.0 / 32768.0) dir = vec2(1.0, 0.0);
    else dir *= inversesqrt(dir_len2);

    len *= 0.5;   // 0..1
    len *= len;
    // stretch along the edge: 1 on axis-aligned edges, sqrt(2) on diagonals
    float stretch = 1.0 / max(abs(dir.x), abs(dir.y));
    vec2 len2 = vec2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);
    float lob = 0.5 - 0.29 * len;   // the kernel's negative lobe: sharper on edges
    float clp = 1.0 / lob;          // clip the window where the lobe ends

    vec3 acc = vec3(0.0);
    float acc_w = 0.0;
    accumulate(acc, acc_w, cb, vec2(0.0, -1.0) - f, dir, len2, lob, clp);
    accumulate(acc, acc_w, cc, vec2(1.0, -1.0) - f, dir, len2, lob, clp);
    accumulate(acc, acc_w, ce, vec2(-1.0, 0.0) - f, dir, len2, lob, clp);
    accumulate(acc, acc_w, cf, vec2(0.0, 0.0) - f, dir, len2, lob, clp);
    accumulate(acc, acc_w, cg, vec2(1.0, 0.0) - f, dir, len2, lob, clp);
    accumulate(acc, acc_w, ch, vec2(2.0, 0.0) - f, dir, len2, lob, clp);
    accumulate(acc, acc_w, ci, vec2(-1.0, 1.0) - f, dir, len2, lob, clp);
    accumulate(acc, acc_w, cj, vec2(0.0, 1.0) - f, dir, len2, lob, clp);
    accumulate(acc, acc_w, ck, vec2(1.0, 1.0) - f, dir, len2, lob, clp);
    accumulate(acc, acc_w, cl, vec2(2.0, 1.0) - f, dir, len2, lob, clp);
    accumulate(acc, acc_w, cn, vec2(0.0, 2.0) - f, dir, len2, lob, clp);
    accumulate(acc, acc_w, co, vec2(1.0, 2.0) - f, dir, len2, lob, clp);

    vec3 lo_c = min(min(cf, cg), min(cj, ck));
    vec3 hi_c = max(max(cf, cg), max(cj, ck));
    vec3 color = acc_w > 0.0 ? acc / acc_w : cf;
    frag_color = vec4(clamp(color, lo_c, hi_c), 1.0);
}
