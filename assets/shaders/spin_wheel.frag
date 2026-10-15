#version 450 core
// v7.1.4's prize wheel canvas (index.js updateSpinUi), evaluated per fragment in canvas px:
//   fill #2b2b2b arc(center, R)                                outer disc
//   rotate(sliceRotation); per rarity fill colour arc(0.95 R)  pie slices sized by the odds
//   shadowColor black, shadowBlur 20 (kept for the rest of the frame):
//   per item: rotate(vA630[k]); fill #F44336 triangle         the pointers (half width 0.7 x 0.6 R)
//   fill #2b2b2b arc(center, 0.6 R)                            centre disc (under #spinButton)
// Edges are 3x3 supersampled like the canvas's antialiasing; the shadows are analytic Gaussians
// (shadowBlur 20 = sigma 10). Output is premultiplied (blend ONE, ONE_MINUS_SRC_ALPHA).
// mode 1 draws the confetti triangles with their vertex colour instead.
out vec4 frag_color;
in vec2 canvas_pos;
in vec4 color;

uniform int mode;
uniform vec2 center;
uniform float radius;
uniform float slice_rotation;
uniform int slice_count;
uniform float slice_from[8];
uniform float slice_to[8];
uniform vec3 slice_rgb[8];
uniform float pointer_rotation[16];
uniform int pointer_count;
uniform float pointer_scale;
uniform float shadow_sigma;

const float TAU = 6.28318530718;
const vec3 DISC = vec3(0.168627, 0.168627, 0.168627); // #2b2b2b

vec4 over(vec4 dst, vec3 rgb, float a) {
    return vec4(rgb * a + dst.rgb * (1.0 - a), a + dst.a * (1.0 - a));
}

// Gaussian CDF (logistic approximation): blurred coverage at signed distance x / sigma inside
float gauss_cdf(float x) {
    return 1.0 / (1.0 + exp(-1.702 * x));
}

// signed distance to the pointer triangle (negative inside), local (unrotated) coordinates
float triangle_sdf(vec2 p, vec2 a, vec2 b, vec2 c) {
    vec2 e0 = b - a, e1 = c - b, e2 = a - c;
    vec2 v0 = p - a, v1 = p - b, v2 = p - c;
    vec2 pq0 = v0 - e0 * clamp(dot(v0, e0) / max(dot(e0, e0), 1e-6), 0.0, 1.0);
    vec2 pq1 = v1 - e1 * clamp(dot(v1, e1) / max(dot(e1, e1), 1e-6), 0.0, 1.0);
    vec2 pq2 = v2 - e2 * clamp(dot(v2, e2) / max(dot(e2, e2), 1e-6), 0.0, 1.0);
    float s = sign(e0.x * e2.y - e0.y * e2.x);
    vec2 d = min(min(vec2(dot(pq0, pq0), s * (v0.x * e0.y - v0.y * e0.x)),
                     vec2(dot(pq1, pq1), s * (v1.x * e1.y - v1.y * e1.x))),
                     vec2(dot(pq2, pq2), s * (v2.x * e2.y - v2.y * e2.x)));
    return -sqrt(d.x) * sign(d.y);
}

// angle is the running angle, unreduced: v10 (index.js updateSpinUi) draws each slice from the
// running sum of the shares, and once the end animation takes the losing rarities past zero that
// sum goes negative. A slice covers the angles (angle - from) mod 2 PI < its span (a span of a full
// turn or more covers the whole disc).
vec3 slice_color(float angle, out float covered) {
    covered = 0.0;
    vec3 rgb = vec3(1.0);
    for (int i = 0; i < 8; i++) {
        if (i >= slice_count) break;
        if (mod(angle - slice_from[i], TAU) < slice_to[i] - slice_from[i]) {
            rgb = slice_rgb[i];
            covered = 1.0;
        }
    }
    return rgb;
}

vec4 wheel_sample(vec2 p) {
    vec2 d = p - center;
    float r = length(d);
    vec4 dst = vec4(0.0);
    if (r < radius) dst = over(dst, DISC, 1.0);
    if (r < radius * 0.95) {
        float angle = atan(d.y, d.x) - slice_rotation;
        float covered;
        vec3 rgb = slice_color(angle, covered);
        dst = over(dst, rgb, covered);
    }
    // the pointers (one per item), each in its rotated frame: canvas rotate(t) maps local q to
    // device R(t) q; drawn in order, each with its shadow over the ones before
    float inner = radius * 0.6;
    vec2 tip = vec2(0.0, inner * -1.2 * pointer_scale);
    vec2 right = vec2(inner * 0.7 * pointer_scale, 0.0);
    vec2 left = vec2(-inner * 0.7 * pointer_scale, 0.0);
    if (pointer_scale > 0.0) {
        for (int k = 0; k < 16; k++) {
            if (k >= pointer_count) break;
            float cs = cos(pointer_rotation[k]), sn = sin(pointer_rotation[k]);
            vec2 q = vec2(cs * d.x + sn * d.y, -sn * d.x + cs * d.y);
            float sd = triangle_sdf(q, tip, right, left);
            float shadow = gauss_cdf(-sd / shadow_sigma) * smoothstep(0.0, 0.15, pointer_scale);
            dst = over(dst, vec3(0.0), shadow);
            if (sd < 0.0) dst = over(dst, vec3(0.956863, 0.262745, 0.211765), 1.0); // #F44336
        }
    }
    dst = over(dst, vec3(0.0), gauss_cdf((inner - r) / shadow_sigma));
    if (r < inner) dst = over(dst, DISC, 1.0);
    return dst;
}

void main() {
    if (mode == 1) {
        frag_color = vec4(color.rgb * color.a, color.a);
        return;
    }
    vec2 dx = dFdx(canvas_pos), dy = dFdy(canvas_pos);
    vec4 sum = vec4(0.0);
    for (int j = -1; j <= 1; j++) {
        for (int i = -1; i <= 1; i++) {
            sum += wheel_sample(canvas_pos + dx * (float(i) / 3.0) + dy * (float(j) / 3.0));
        }
    }
    frag_color = sum / 9.0;
}
