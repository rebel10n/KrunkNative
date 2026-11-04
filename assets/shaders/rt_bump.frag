#version 450 core
// Generated normal maps (client/src/rt/rt_normals.c, rt.h "generated normal maps"): one fullscreen pass over a
// target `scale` times the base texture's size. Height = the base texture's luminance, interpolated bilinearly
// between texel centres (Krunker's pixel-art steps become soft one-texel ramps instead of hard aliasing edges),
// optionally pre-blurred by half a texel. A 3x3 Sobel on that height (one target texel apart) gives the slope
// per source texel; the tangent-space normal (x along +u, y along +v, the texel space the base is sampled in)
// is stored as rgb = n * 0.5 + 0.5 (alpha: a cavity term, see main). Bright = raised: mortar, plank gaps, panel seams (darker) sink.
// The base texture tiles, so lookups wrap.
uniform sampler2D src;
uniform vec2 src_size;   // base texture size in texels
uniform float scale;     // target texels per source texel (1..4)
uniform float slope;     // RT_BUMP_SLOPE
uniform float blur;      // 0: bilinear height; 1: half-texel tent blur on top (pixel noise)
out vec4 frag_out;

float lum_at(ivec2 p) {
    ivec2 q = ivec2(mod(vec2(p), src_size));
    vec3 c = texelFetch(src, q, 0).rgb;
    return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

// bilinear luminance at q (source texel units, texel centres at i + 0.5)
float lum_bilinear(vec2 q) {
    vec2 g = q - 0.5;
    vec2 i = floor(g);
    vec2 f = g - i;
    ivec2 p = ivec2(i);
    float a = lum_at(p);
    float b = lum_at(p + ivec2(1, 0));
    float c = lum_at(p + ivec2(0, 1));
    float d = lum_at(p + ivec2(1, 1));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

float height(vec2 q) {
    float h = lum_bilinear(q);
    if (blur > 0.0) {
        float r = 0.5;
        float n = lum_bilinear(q + vec2(r, 0.0)) + lum_bilinear(q - vec2(r, 0.0)) +
                  lum_bilinear(q + vec2(0.0, r)) + lum_bilinear(q - vec2(0.0, r));
        h = mix(h, (h * 2.0 + n) / 6.0, blur);
    }
    return h;
}

void main() {
    vec2 q = gl_FragCoord.xy / scale; // this target texel's centre in source texel units
    float d = 1.0 / scale;            // one target texel
    float h00 = height(q + vec2(-d, -d));
    float h10 = height(q + vec2(0.0, -d));
    float h20 = height(q + vec2(d, -d));
    float h01 = height(q + vec2(-d, 0.0));
    float h21 = height(q + vec2(d, 0.0));
    float h02 = height(q + vec2(-d, d));
    float h12 = height(q + vec2(0.0, d));
    float h22 = height(q + vec2(d, d));
    // Sobel: sum of weights 4 per side, 2 * d apart -> per source texel
    float du = ((h20 + 2.0 * h21 + h22) - (h00 + 2.0 * h01 + h02)) / (8.0 * d);
    float dv = ((h02 + 2.0 * h12 + h22) - (h00 + 2.0 * h10 + h20)) / (8.0 * d);
    vec3 n = normalize(vec3(-du * slope, -dv * slope, 1.0));
    // alpha: cavity, the height against its 3x3 neighbourhood (0.5 = level, below = a seam / mortar / gap). The
    // Cinematic preset darkens the low parts with it (basic.frag krunker_rt_cavity); 0.5 everywhere is neutral.
    float h11 = height(q);
    float mean = (h00 + h10 + h20 + h01 + h21 + h02 + h12 + h22) * 0.125;
    float cavity = clamp(0.5 + (h11 - mean) * 4.0, 0.0, 1.0);
    frag_out = vec4(n * 0.5 + 0.5, cavity);
}
