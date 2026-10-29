// Krunker v10.0.4 skin shader 17 "Hollowmere Night" (deob-10.0.4.js:92959-93011; its table entry :196147-196150):
// a moonlit Halloween skyline on the item, in the item's local space (scale 0.001). Ported from the TSL node graph:
// kn_hash3 / noise / fbm (:91666-91681), hash2 (iíìîiiï, :93046-93048). client/src/skin_fx.c prepends the GLSL header.
// args (shaderArgs, deob-10.0.4.js:92963-92979): scale, skyline (points) and moon (x, y, radius) arrive as uSkyline /
// uSkylineCount / uMoon / uScale from client/src/skin_fx.c, which falls back to the bundle's defaults (the Hollowmere
// Night skins 8074 / 8075 pass their own skyline and moon).
out vec4 kn_frag_color;
in vec2 vUv;
in vec3 vLocal;
in vec3 vView;

uniform float uTime;
#define KN_SKYLINE_MAX 48
uniform vec2 uSkyline[KN_SKYLINE_MAX];
uniform int uSkylineCount;
uniform vec3 uMoon;   // x, y, radius
uniform float uScale; // positionLocal scale (args.scale || 0.001)
uniform mat4 transform;
uniform mat4 camera_world_inverse;

float kn_hash3(vec3 p) {
    p = fract(p * 0.3183099 + vec3(0.1, 0.2, 0.3)) * 17.0;
    return fract(p.x * p.y * p.z * (p.x + p.y + p.z));
}

float kn_noise3(vec3 x) {
    vec3 i = floor(x);
    vec3 f = fract(x);
    vec3 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(mix(kn_hash3(i + vec3(0.0, 0.0, 0.0)), kn_hash3(i + vec3(1.0, 0.0, 0.0)), u.x),
                   mix(kn_hash3(i + vec3(0.0, 1.0, 0.0)), kn_hash3(i + vec3(1.0, 1.0, 0.0)), u.x), u.y),
               mix(mix(kn_hash3(i + vec3(0.0, 0.0, 1.0)), kn_hash3(i + vec3(1.0, 0.0, 1.0)), u.x),
                   mix(kn_hash3(i + vec3(0.0, 1.0, 1.0)), kn_hash3(i + vec3(1.0, 1.0, 1.0)), u.x), u.y), u.z);
}

float kn_fbm3(vec3 x) {
    return (kn_noise3(x) * 0.5 + kn_noise3(x * 2.03 + 17.1) * 0.25 + kn_noise3(x * 4.1 + 31.7) * 0.125) / 0.875;
}

float hash2(vec2 v) {
    return fract(sin(dot(v, vec2(127.1, 311.7))) * 43758.5453);
}

// the skyline's height at x: piecewise linear over args.skyline (a vertical step where two points share an x), as
// the build's loop (deob-10.0.4.js:92980-92992)
float skyline(float x) {
    float v = uSkyline[0].y;
    for (int i = 0; i + 1 < KN_SKYLINE_MAX; i++) {
        if (i + 1 >= uSkylineCount) break;
        vec2 a = uSkyline[i];
        vec2 b = uSkyline[i + 1];
        float seg = b.x > a.x ? mix(a.y, b.y, clamp((x - a.x) / (b.x - a.x), 0.0, 1.0)) : b.y;
        v = x >= a.x ? seg : v;
    }
    return v;
}

void main() {
    float SCALE = uScale;
    vec3 MOON = uMoon; // x, y, radius
    const float LOT = 0.42;
    float t = uTime;

    // the context (:92964-92979): the face normal from the view-space position (no normal attribute), back to local
    vec3 nView = normalize(cross(dFdx(vView), dFdy(vView)));
    mat3 viewModel = mat3(camera_world_inverse * transform);
    vec3 n = normalize(transpose(viewModel) * nView); // normalLocal
    vec3 viewDir = normalize(-vView);                 // positionViewDirection
    vec3 p = vLocal * SCALE;
    float diffuse = max(dot(vec3(n.x, n.y, abs(n.z)), vec3(0.37, 0.84, 0.4)), 0.0) * 0.55 + 0.45;
    vec3 halfDir = normalize(viewDir + vec3(0.3, 0.85, 0.42));
    float facing = max(dot(nView, viewDir), 0.0);
    float fres = pow(1.0 - facing, 3.0);
    float side = smoothstep(0.45, 0.65, abs(n.z));

    float cloud = kn_fbm3(p * vec3(0.7, 2.2, 2.2) + vec3(t * 0.12, 0.0, t * 0.05));
    vec3 sky = mix(vec3(0.02, 0.015, 0.06), vec3(0.13, 0.08, 0.26), smoothstep(-1.2, 0.6, p.y)) +
               vec3(0.28, 0.22, 0.4) * (smoothstep(0.5, 0.85, cloud) * 0.5);
    float ground = skyline(p.x);

    // houses and trees on lots of LOT
    float lot = floor(p.x / LOT);
    float r7 = hash2(vec2(lot, 7.0));
    float r3 = hash2(vec2(lot, 3.0));
    float lx = p.x / LOT - lot;
    bool hasHouse = r7 > 0.35;
    float houseH = r3 * 0.12 + 0.12;
    float roofH = houseH + (1.0 - abs(lx - 0.5) * 2.0) * 0.13;
    bool house = hasHouse && lx > 0.08 && lx < 0.92 && p.y < ground + roofH;
    bool tree = !hasHouse && p.y < ground + (r3 * 0.2 + 0.18) * (1.0 - abs(lx - 0.5) * 2.4);
    float sideMask = smoothstep(0.55, 0.8, abs(n.z));
    bool silhouette = p.y < ground || house || tree;
    bool dark = n.y < -0.3 || (silhouette && sideMask > 0.5 && n.y < 0.6);
    vec3 darkCol = mix(vec3(0.015, 0.01, 0.03), vec3(0.05, 0.03, 0.08), smoothstep(-1.5, 0.3, p.y));

    // twinkling stars
    vec3 cell = floor(p * 11.0);
    float sr = kn_hash3(cell);
    float sr2 = kn_hash3(cell + 31.0);
    float d = length((fract(p * 11.0) - 0.5).xy);
    float tw = sin(t * (sr2 * 3.0 + 1.5) + sr * 6.283) * 0.5 + 0.5;
    float stars = step(0.88, sr) * smoothstep(sr2 * 0.08 + 0.1, 0.02, d) * (tw * 0.7 + 0.3);

    // the moon and its halo
    float moonD = length(vec2(p.x - MOON.x, p.y - MOON.y));
    float moon = smoothstep(MOON.z + 0.005, MOON.z - 0.005, moonD) * side;
    vec3 moonCol = mix(vec3(1.0, 0.96, 0.78), vec3(0.78, 0.74, 0.6), step(0.62, kn_fbm3(p * 6.0 + 9.0)));
    vec3 halo = vec3(0.3, 0.26, 0.36) * (smoothstep(2.7 * MOON.z, MOON.z, moonD) * side);

    // lit windows (two per house), flickering on and off
    float windows = 0.0;
    for (int k = 0; k < 2; k++) {
        float wx = k == 0 ? 0.3 : 0.7;
        float lit = step(0.3, hash2(vec2(lot + 10.0 * wx, floor(t * 0.2 + r7 * 5.0 + wx))));
        float w = step(abs(lx - wx), 0.07) * step(abs(p.y - (ground + houseH * 0.55)), 0.035);
        windows = max(windows, w * lit * (house ? 1.0 : 0.0) * step(0.85, abs(n.z)));
    }
    vec3 winCol = vec3(1.0, 0.6, 0.15) * (sin(t * 9.0 + lot) * 0.08 + 0.92);

    vec3 lit = sky * diffuse + vec3(0.85, 0.85, 1.0) * (stars * (1.0 - moon)) + halo;
    vec3 col = dark ? darkCol * diffuse : lit;
    col = mix(col, moonCol, moon * (dark ? 0.0 : 1.0));
    col = mix(col, winCol, windows);
    col += vec3(0.3, 0.22, 0.55) * (fres * 0.45) + vec3(0.6, 0.55, 0.9) * (pow(max(dot(nView, halfDir), 0.0), 40.0) * 0.12);
    kn_frag_color = vec4(col, 1.0);
}
