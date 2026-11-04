#version 450 core
// Bloom (Box Office-only rtBloom; client/src/render_target.c, doc/raytracing.md "Bloom"). The scene target is
// half-float while bloom runs and opaque basic.frag fragments write alpha = 1 + their bloom weight (the
// pre-tone-map radiance above 1 plus their emissive glow); additive draws may also leave rgb above 1. One
// program, three steps over a mip chain at half, quarter, ... resolution (the "dual filter" chain of
// Jimenez, "Next generation post processing in Call of Duty", 2014):
//   mode 0  prefilter: the scene -> level 0. Extracts rgb * weight + max(rgb - 1, 0) with a 13-tap
//           downsample whose four 2x2 groups are Karis-averaged (weighted by 1 / (1 + luma)), so one
//           sub-pixel highlight cannot flash the whole bloom as the TAA jitter moves it.
//   mode 1  downsample: level i - 1 -> level i, the same 13-tap filter without the extraction.
//   mode 2  upsample: level i + 1 -> level i with a 3x3 tent, added onto level i by the blend (ONE, ONE).
// The composite (scene_composite.frag) adds level 0 times the strength to the clamped scene colour.
in vec2 uv;
out vec4 frag_color;

uniform sampler2D source; // unit 0
uniform vec2 texel;       // 1 / the source's size
uniform int mode;
uniform float radius;     // the upsample tent's reach in source texels
uniform float knee;       // Cinematic: display luminance from which highlights bloom too (0 = off, the HDR weight only)

vec3 extract(vec4 c) {
    vec3 rgb = max(c.rgb, vec3(0.0));
    vec3 e = min(rgb, vec3(1.0)) * max(c.a - 1.0, 0.0) + max(rgb - vec3(1.0), vec3(0.0));
    if (knee > 0.0) { // the sun disk, sunlit sand and white walls glow softly under Cinematic
        vec3 d = min(rgb, vec3(1.0));
        float l = dot(d, vec3(0.2126, 0.7152, 0.0722));
        e += d * smoothstep(knee, 1.0, l) * 0.35;
    }
    return e;
}

vec3 fetch(vec2 p) {
    vec4 c = texture(source, p);
    return mode == 0 ? extract(c) : c.rgb;
}

float karis(vec3 c) {
    return 1.0 / (1.0 + dot(c, vec3(0.2126, 0.7152, 0.0722)));
}

vec3 downsample() {
    vec3 a = fetch(uv + texel * vec2(-2.0, 2.0));
    vec3 b = fetch(uv + texel * vec2(0.0, 2.0));
    vec3 c = fetch(uv + texel * vec2(2.0, 2.0));
    vec3 d = fetch(uv + texel * vec2(-2.0, 0.0));
    vec3 e = fetch(uv);
    vec3 f = fetch(uv + texel * vec2(2.0, 0.0));
    vec3 g = fetch(uv + texel * vec2(-2.0, -2.0));
    vec3 h = fetch(uv + texel * vec2(0.0, -2.0));
    vec3 i = fetch(uv + texel * vec2(2.0, -2.0));
    vec3 j = fetch(uv + texel * vec2(-1.0, 1.0));
    vec3 k = fetch(uv + texel * vec2(1.0, 1.0));
    vec3 l = fetch(uv + texel * vec2(-1.0, -1.0));
    vec3 m = fetch(uv + texel * vec2(1.0, -1.0));
    if (mode == 0) {
        vec3 g0 = (a + b + d + e) * 0.25, g1 = (b + c + e + f) * 0.25;
        vec3 g2 = (d + e + g + h) * 0.25, g3 = (e + f + h + i) * 0.25;
        vec3 g4 = (j + k + l + m) * 0.25;
        float w0 = karis(g0), w1 = karis(g1), w2 = karis(g2), w3 = karis(g3), w4 = karis(g4);
        vec3 sum = g0 * w0 * 0.125 + g1 * w1 * 0.125 + g2 * w2 * 0.125 + g3 * w3 * 0.125 + g4 * w4 * 0.5;
        float wsum = w0 * 0.125 + w1 * 0.125 + w2 * 0.125 + w3 * 0.125 + w4 * 0.5;
        return sum / max(wsum, 1e-4);
    }
    return e * 0.125 + (a + c + g + i) * 0.03125 + (b + d + f + h) * 0.0625 + (j + k + l + m) * 0.125;
}

vec3 upsample() {
    vec2 o = texel * radius;
    vec3 s = texture(source, uv).rgb * 4.0;
    s += (texture(source, uv + vec2(-o.x, 0.0)).rgb + texture(source, uv + vec2(o.x, 0.0)).rgb +
          texture(source, uv + vec2(0.0, -o.y)).rgb + texture(source, uv + vec2(0.0, o.y)).rgb) * 2.0;
    s += texture(source, uv + vec2(-o.x, -o.y)).rgb + texture(source, uv + vec2(o.x, -o.y)).rgb +
         texture(source, uv + vec2(-o.x, o.y)).rgb + texture(source, uv + vec2(o.x, o.y)).rgb;
    return s / 16.0;
}

void main() {
    vec3 c = mode == 2 ? upsample() : downsample();
    frag_color = vec4(min(c, vec3(64.0)), 1.0); // a stray inf / huge additive sum cannot poison the chain
}
