// Krunker v10.0.5 skin shader 18 "Ametwista" (deob-10.0.5.js:196197-197428): the first shipped "graph" shader, a
// node graph (kind "surface") that the bundle compiles to TSL at load time (iiiíîiï, :94816; the material, iiïîiîì,
// :95805-95835). This is the graph hand-translated to GLSL, node by node (each block names the node ids and their
// lines). Amethyst crystal: Worley cells on the scaled local position, facet normals perturbed per cell, a deep-to-mid
// purple ramp along the barrel, a glint, blinking cell sparkles, an inner glow, a rim and an outline band that runs
// up the model along the cell edges.
//
// The output node (:197240) is lit, side front, normal blending, depthWrite, opaque: a MeshLambertNodeMaterial with
// colorNode vec4(albedo, 1), emissiveNode emis and normalNode transformNormalByViewMatrix(pn, cameraViewMatrix) (:95830,
// :95571-95581). The graph builds pn from normalView, so the bundle turns a view-space normal by the view matrix a
// second time; the lit normal here does the same (parity). Lambert as basic.frag's krunker_lambert: the ambient light
// and the map's sun (no point lights or shadows on this material, KrunkNative).
//
// Noise: the graph's worley / noise3 nodes are three.js's MaterialX functions (deob-10.0.5.js:70400-72900), ported as
// MaterialX's mx_noise.glsl. The "f2" output is mx_worley_noise_vec2 with metric 1 (squared F1 / F2, :72887-72891);
// style "cell" is the nearest cell's mx_cell_noise_float (:71505-71507), style "distance" sqrt(F1).
out vec4 kn_frag_color;
in vec3 vLocal;
in vec3 vView;
in vec3 vScale;
in vec3 vNormalView; // normalView ((0, 0, 0) without a normal attribute: the face normal below)

uniform float time;
uniform mat4 camera_world_inverse;

// the graph's params (deob-10.0.5.js:196207-196411), defaults and shaderArgs.params from client/src/skin_fx.c
uniform float stockX;
uniform float muzzleX;
uniform vec3 deepColor;
uniform vec3 midColor;
uniform vec3 lightColor;
uniform float crystalScale;
uniform float facetDepth;
uniform float glintAmt;
uniform float sparkleRate;
uniform float glowAmt;
uniform float blinkSpeed;
uniform vec3 sparkleColor;
uniform vec3 outlineColor;
uniform float outlineAmt;
uniform float outlineSpeed;
uniform float outlineCount;
uniform float bottomY;
uniform float topY;

// the scene lighting (basic.frag's names, uploaded by skin_fx.c for this shader)
uniform vec4 ambient_color;
uniform vec4 light_color;
uniform vec3 light_direction;
uniform float ambient_intensity;
uniform float light_intensity;
uniform bool correct_lights;

#define KN_PI 3.141592653589793

// ---- MaterialX noise (three.js mx_noise, deob-10.0.5.js:70400-72160)
float mx_select(bool b, float t, float f) { return b ? t : f; }
float mx_negate_if(float val, bool b) { return b ? -val : val; }
int mx_floor(float x) { return int(floor(x)); }
float mx_floorfrac(float x, out int i) {
    i = mx_floor(x);
    return x - float(i);
}
float mx_fade(float t) { return t * t * t * (t * (t * 6.0 - 15.0) + 10.0); }
uint mx_rotl32(uint x, int k) { return (x << uint(k)) | (x >> uint(32 - k)); }
void mx_bjmix(inout uint a, inout uint b, inout uint c) {
    a -= c; a ^= mx_rotl32(c, 4); c += b;
    b -= a; b ^= mx_rotl32(a, 6); a += c;
    c -= b; c ^= mx_rotl32(b, 8); b += a;
    a -= c; a ^= mx_rotl32(c, 16); c += b;
    b -= a; b ^= mx_rotl32(a, 19); a += c;
    c -= b; c ^= mx_rotl32(b, 4); b += a;
}
uint mx_bjfinal(uint a, uint b, uint c) {
    c ^= b; c -= mx_rotl32(b, 14);
    a ^= c; a -= mx_rotl32(c, 11);
    b ^= a; b -= mx_rotl32(a, 25);
    c ^= b; c -= mx_rotl32(b, 16);
    a ^= c; a -= mx_rotl32(c, 4);
    b ^= a; b -= mx_rotl32(a, 14);
    c ^= b; c -= mx_rotl32(b, 24);
    return c;
}
float mx_bits_to_01(uint bits) { return float(bits) / float(0xffffffffu); }
// mx_hash_int_2 (deob-10.0.5.js:70750-70766): three ints
uint mx_hash_int(int x, int y, int z) {
    uint a = 0xdeadbeefu + (3u << 2u) + 13u, b = a, c = a;
    a += uint(x);
    b += uint(y);
    c += uint(z);
    return mx_bjfinal(a, b, c);
}
// mx_hash_int_3 (:70775-70804): three ints and a channel
uint mx_hash_int(int x, int y, int z, int xx) {
    uint a = 0xdeadbeefu + (4u << 2u) + 13u, b = a, c = a;
    a += uint(x);
    b += uint(y);
    c += uint(z);
    mx_bjmix(a, b, c);
    a += uint(xx);
    return mx_bjfinal(a, b, c);
}
// mx_hash_vec3_1 (:70893-70906): the low three bytes of the hash
uvec3 mx_hash_vec3(int x, int y, int z) {
    uint h = mx_hash_int(x, y, z);
    return uvec3(h & 0xffu, (h >> 8u) & 0xffu, (h >> 16u) & 0xffu);
}
// mx_gradient_float_1 (:70457-70472) and its vec3 / scale (0.982, :70556-70580)
float mx_gradient_float(uint hash, float x, float y, float z) {
    uint h = hash & 15u;
    float u = mx_select(h < 8u, x, y);
    float v = mx_select(h < 4u, y, mx_select(h == 12u || h == 14u, x, z));
    return mx_negate_if(u, (h & 1u) != 0u) + mx_negate_if(v, (h & 2u) != 0u);
}
vec3 mx_gradient_vec3(uvec3 hash, float x, float y, float z) {
    return vec3(mx_gradient_float(hash.x, x, y, z), mx_gradient_float(hash.y, x, y, z), mx_gradient_float(hash.z, x, y, z));
}
vec3 mx_trilerp(vec3 v0, vec3 v1, vec3 v2, vec3 v3, vec3 v4, vec3 v5, vec3 v6, vec3 v7, float s, float t, float r) {
    float s1 = 1.0 - s, t1 = 1.0 - t, r1 = 1.0 - r;
    return r1 * (t1 * (v0 * s1 + v1 * s) + t * (v2 * s1 + v3 * s)) + r * (t1 * (v4 * s1 + v5 * s) + t * (v6 * s1 + v7 * s));
}
// mx_perlin_noise_vec3_1 (:70982-70996); mx_noise_vec3 (:72743-72748) with amplitude 1, pivot 0
vec3 mx_perlin_noise_vec3(vec3 p) {
    int X, Y, Z;
    float fx = mx_floorfrac(p.x, X), fy = mx_floorfrac(p.y, Y), fz = mx_floorfrac(p.z, Z);
    float u = mx_fade(fx), v = mx_fade(fy), w = mx_fade(fz);
    vec3 result = mx_trilerp(
        mx_gradient_vec3(mx_hash_vec3(X, Y, Z), fx, fy, fz),
        mx_gradient_vec3(mx_hash_vec3(X + 1, Y, Z), fx - 1.0, fy, fz),
        mx_gradient_vec3(mx_hash_vec3(X, Y + 1, Z), fx, fy - 1.0, fz),
        mx_gradient_vec3(mx_hash_vec3(X + 1, Y + 1, Z), fx - 1.0, fy - 1.0, fz),
        mx_gradient_vec3(mx_hash_vec3(X, Y, Z + 1), fx, fy, fz - 1.0),
        mx_gradient_vec3(mx_hash_vec3(X + 1, Y, Z + 1), fx - 1.0, fy, fz - 1.0),
        mx_gradient_vec3(mx_hash_vec3(X, Y + 1, Z + 1), fx, fy - 1.0, fz - 1.0),
        mx_gradient_vec3(mx_hash_vec3(X + 1, Y + 1, Z + 1), fx - 1.0, fy - 1.0, fz - 1.0),
        u, v, w);
    return 0.982 * result;
}
// mx_cell_noise_float_2 (vec3, :71026-71039) and the vec3 cell noise (:71147-71162)
float mx_cell_noise_float(vec3 p) { return mx_bits_to_01(mx_hash_int(mx_floor(p.x), mx_floor(p.y), mx_floor(p.z))); }
vec3 mx_cell_noise_vec3(vec3 p) {
    int ix = mx_floor(p.x), iy = mx_floor(p.y), iz = mx_floor(p.z);
    return vec3(mx_bits_to_01(mx_hash_int(ix, iy, iz, 0)), mx_bits_to_01(mx_hash_int(ix, iy, iz, 1)), mx_bits_to_01(mx_hash_int(ix, iy, iz, 2)));
}
// mx_worley_distance_1 (:71386-71420), metric 0 / 1: the squared distance to the jittered point of cell (x, y, z) + off
vec3 mx_worley_offset(vec3 p, int x, int y, int z, int xoff, int yoff, int zoff, float jitter) {
    vec3 off = mx_cell_noise_vec3(vec3(float(x + xoff), float(y + yoff), float(z + zoff)));
    off = (off - 0.5) * jitter + 0.5;
    return vec3(float(x), float(y), float(z)) + off - p; // cellpos - p
}
// The graph's two Worley reads of one point (w.f2 and wc share p, scale and jitter): F1² and F2² (mx_worley_noise_vec2_1,
// :71679-71718, metric 1) and the nearest cell's value (mx_worley_noise_float with style 1 "cell", :71440-71511).
void kn_worley(vec3 p, float jitter, out vec2 f2, out float cell) {
    int X, Y, Z;
    vec3 localpos = vec3(mx_floorfrac(p.x, X), mx_floorfrac(p.y, Y), mx_floorfrac(p.z, Z));
    f2 = vec2(1e6, 1e6);
    float best = 1e6;
    vec3 best_diff = vec3(0.0);
    for (int x = -1; x <= 1; x++) {
        for (int y = -1; y <= 1; y++) {
            for (int z = -1; z <= 1; z++) {
                vec3 diff = mx_worley_offset(localpos, x, y, z, X, Y, Z, jitter);
                float dist = dot(diff, diff);
                if (dist < f2.x) {
                    f2.y = f2.x;
                    f2.x = dist;
                } else if (dist < f2.y) {
                    f2.y = dist;
                }
                if (dist < best) {
                    best = dist;
                    best_diff = diff;
                }
            }
        }
    }
    cell = mx_cell_noise_float(best_diff + p);
}

// TSL remap / remapClamp (x, inLo, inHi, outLo, outHi; the compiler's order, :95168-95171)
float kn_remap(float x, float in_lo, float in_hi, float out_lo, float out_hi) { return (x - in_lo) / (in_hi - in_lo) * (out_hi - out_lo) + out_lo; }
float kn_remap_clamp(float x, float in_lo, float in_hi, float out_lo, float out_hi) {
    return clamp((x - in_lo) / (in_hi - in_lo), 0.0, 1.0) * (out_hi - out_lo) + out_lo;
}

void main() {
    // pos, msc, tm, nv, vd (:196413-196437): positionLocal, modelScale, time, normalView, positionViewDirection
    vec3 nv = length(vNormalView) > 0.0 ? normalize(vNormalView) : normalize(cross(dFdx(vView), dFdy(vView)));
    vec3 vd = normalize(-vView);
    // obj, spl (:196438-196460)
    vec3 obj = vLocal * vScale;
    // t (:196461): remapClamp(obj.x, stockX, muzzleX, 0, 1), stock to muzzle
    float t = kn_remap_clamp(obj.x, stockX, muzzleX, 0.0, 1.0);
    // w, wc, f2s (:196484-196532): worley(obj * crystalScale, jitter 0.9)
    vec2 f2;
    float wc;
    kn_worley(obj * crystalScale, 0.9, f2, wc);
    // edge, es, line (:196533-196573): 1 - smoothstep(0, 0.05, F2² - F1²), the cell edges
    float line = 1.0 - smoothstep(0.0, 0.05, f2.y - f2.x);
    // k3, idv, rn, rnk, pn0, pn (:196574-196643): the facet normal, normalView + noise3(vec3(1, 1.7, 2.9) * wc * 9) * facetDepth
    vec3 rn = mx_perlin_noise_vec3(vec3(1.0, 1.7, 2.9) * wc * 9.0);
    vec3 pn = normalize(nv + rn * facetDepth);
    // sh, sh2, sh3, shc, alb0 (:196644-196708): mix(deepColor, midColor, saturate((wc * 0.8 + t) * 0.6))
    vec3 alb0 = mix(deepColor, midColor, clamp((wc * 0.8 + t) * 0.6, 0.0, 1.0));
    // gk, gk2, albedo (:196709-196744): darker cell edges
    vec3 albedo = alb0 * (1.0 - line * 0.5);
    // ldir, hv0, hv, nh, nhs, gl0, gl, eGl (:196745-196837): a fixed-direction glint
    vec3 hv = normalize(vd + vec3(0.35, 0.75, 0.55));
    vec3 e_gl = lightColor * (pow(clamp(dot(pn, hv), 0.0, 1.0), 45.0) * glintAmt);
    // tq0, tq, tf, wcs, hs, hh, thr, on, fade, fade2, spk, spk2, eSpk (:196838-196991): per-cell blinking sparkles; hash
    // is fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453) of vec2(hs) (:94785-94787; a float splats, :94684)
    float tq0 = time * blinkSpeed;
    float hs = wc * 131.7 + floor(tq0);
    float hh = fract(sin(dot(vec2(hs), vec2(12.9898, 78.233))) * 43758.5453);
    float on = step(1.0 - sparkleRate, hh);
    vec3 e_spk = sparkleColor * (on * pow(1.0 - fract(tq0), 2.0) * 1.5);
    // cen, cen2, brT, br0, br, ig0, ig2, ig3, ig, eIg (:196992-197128): the inner glow near cell centres, breathing with
    // oscSine(time * 0.35) = sin((t + 0.75) * 2 PI) * 0.5 + 0.5 (:72941-72943) remapped to 0.5..1
    float cen2 = 1.0 - smoothstep(0.0, 0.45, f2.x);
    float br = kn_remap(sin((time * 0.35 + 0.75) * 2.0 * KN_PI) * 0.5 + 0.5, 0.0, 1.0, 0.5, 1.0);
    vec3 e_ig = midColor * (cen2 * br * t * glowAmt * 0.5);
    // fd, fa, fr, rim, eRim (:197129-197187): pow(1 - |dot(normalView, viewDir)|, 3.5)
    vec3 e_rim = midColor * pow(1.0 - abs(dot(nv, vd)), 3.5);
    // ty, tyc, os, ov0, ov, ob1, ob2, ob2n, oband, ol0, ol, eOut (:197263-197428): a band rising bottomY..topY along the edges
    float ty = kn_remap_clamp(obj.y, bottomY, topY, 0.0, 1.0);
    float ov = fract(ty * outlineCount - time * outlineSpeed);
    float oband = smoothstep(0.0, 0.25, ov) * (1.0 - smoothstep(0.25, 0.5, ov));
    vec3 e_out = outlineColor * (line * oband * outlineAmt);
    // e1, e2, e3, emis (:197188-197239)
    vec3 emis = e_gl + e_spk + e_ig + e_out + e_rim;

    // out (:197240): Lambert(albedo) with normalNode = transformNormalByViewMatrix(pn) + emissive
    mat3 view = mat3(camera_world_inverse);
    vec3 n = normalize(view * pn);
    vec3 l = normalize(view * light_direction);
    float light_scale = correct_lights ? 1.0 : KN_PI;
    vec3 irradiance = ambient_color.rgb * ambient_intensity * light_scale;
    irradiance += light_color.rgb * light_intensity * light_scale * clamp(dot(n, l), 0.0, 1.0);
    kn_frag_color = vec4(irradiance * albedo / KN_PI + emis, 1.0);
}
