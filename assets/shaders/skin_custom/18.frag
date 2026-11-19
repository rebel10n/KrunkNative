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
// v10.0.7 (deob-10.0.7.js:196569-199154, "Ametwista skins showing black") rebuilt the cells by hand: the worley / split /
// noise3 nodes are gone; 8 cells (offsets {-1, 0}^3, c0..c7, :197706-199154) hash dot(cell, kHash) with the graph's
// "hash" node (fract(sin(dot(vec2(p), vec2(12.9898, 78.233))) * 43758.5453), :94793-94795, a float splats to vec2),
// jitter fract(h * kSpread) * 0.75 and keep F1, F2 (true distances, no longer squared) and the nearest cell's hash; the
// facet vector is fract(id * kFacet) * 2 - 1. msc is the float param objScale (:196778-196785), not the model's scale.
out vec4 kn_frag_color;
in vec3 vLocal;
in vec3 vView;
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
uniform float objScale; // msc (deob-10.0.7.js:196778-196785)
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

// the graph's "hash" node (deob-10.0.7.js:94793-94795, :95249-95250): its float input splats to vec2
float kn_hash(float p) { return fract(sin(dot(vec2(p), vec2(12.9898, 78.233))) * 43758.5453); }

// wps..wrel and the cells c0..c7 (deob-10.0.7.js:197706-199154): kBig 0.5, kOff 0.625, kHash, kSpread, jitter 0.75. Each
// cell after c0 runs mx = max(f1, d), f2 = min(f2, mx), id = mix(id, h, step(d, f1)) (step: 0 where f1 < d), f1 = min(f1, d);
// c1's max(c0, c1) is that rule with f2 starting unbounded.
void kn_cells(vec3 wps, out float f1, out float f2, out float id) {
    const vec3 k_hash = vec3(0.1031, 0.3313, 0.7477), k_spread = vec3(7.31, 17.13, 113.71);
    vec3 wpb = wps + vec3(0.5);
    vec3 wbase = floor(wpb);
    vec3 wrel = wpb - vec3(0.625);
    f1 = 1e6;
    f2 = 1e6;
    id = 0.0;
    for (int i = 0; i < 8; i++) { // c0 [-1, -1, -1] .. c7 [0, 0, 0]: x is bit 2, y bit 1, z bit 0
        vec3 c = wbase + vec3(float(i / 4) - 1.0, float((i / 2) % 2) - 1.0, float(i % 2) - 1.0);
        float h = kn_hash(dot(c, k_hash));
        float d = distance(c + fract(h * k_spread) * 0.75, wrel);
        if (i == 0) {
            f1 = d;
            id = h;
            continue;
        }
        f2 = min(f2, max(f1, d));
        id = mix(id, h, step(d, f1));
        f1 = min(f1, d);
    }
}

// TSL remap / remapClamp (x, inLo, inHi, outLo, outHi; the compiler's order, :95168-95171)
float kn_remap(float x, float in_lo, float in_hi, float out_lo, float out_hi) { return (x - in_lo) / (in_hi - in_lo) * (out_hi - out_lo) + out_lo; }
float kn_remap_clamp(float x, float in_lo, float in_hi, float out_lo, float out_hi) {
    return clamp((x - in_lo) / (in_hi - in_lo), 0.0, 1.0) * (out_hi - out_lo) + out_lo;
}

void main() {
    // pos, msc, tm, nv, vd: positionLocal, objScale (deob-10.0.7.js:196778-196785), time, normalView, positionViewDirection
    vec3 nv = length(vNormalView) > 0.0 ? normalize(vNormalView) : normalize(cross(dFdx(vView), dFdy(vView)));
    vec3 vd = normalize(-vView);
    // obj, spl (:196438-196460)
    vec3 obj = vLocal * objScale;
    // t (:196461): remapClamp(obj.x, stockX, muzzleX, 0, 1), stock to muzzle
    float t = kn_remap_clamp(obj.x, stockX, muzzleX, 0.0, 1.0);
    // the cells of obj * crystalScale (wps, deob-10.0.7.js:197706-199154): F1, F2 and the nearest cell's hash (wc)
    float f1, f2, wc;
    kn_cells(obj * crystalScale, f1, f2, wc);
    // edge, es, line (deob-10.0.7.js:196854-196857): 1 - smoothstep(0, 0.05, F2 - F1), the cell edges
    float line = 1.0 - smoothstep(0.0, 0.05, f2 - f1);
    // kFacet, fct0..fct2, rn, rnk, pn0, pn (deob-10.0.7.js:199154): normalView + (fract(id * kFacet) * 2 - 1) * facetDepth
    vec3 rn = fract(wc * vec3(13.7, 31.3, 57.9)) * 2.0 - 1.0;
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
    float cen2 = 1.0 - smoothstep(0.0, 0.45, f1); // cen reads c7_f1 (deob-10.0.7.js:197277)
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
