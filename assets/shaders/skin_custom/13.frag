// Krunker v10.0.4 skin shader 13 (deob-10.0.4.js:196113-196122; TSL twin :92585-92672): flowing gold-violet veins over a
// black lacquer, triplanar on the part's scaled local position, with rim light and a travelling glint. vObjPos is
// positionLocal times the model's x scale (as its vertex shader); the object / view normals are the face's (dFdx / dFdy).
out vec4 kn_frag_color;
in vec3 vLocal;
in vec3 vView;
in vec3 vScale;

uniform float time;

float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
float noise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = hash(i), b = hash(i + vec2(1.0, 0.0)), c = hash(i + vec2(0.0, 1.0)), d = hash(i + vec2(1.0, 1.0));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}
float fbm(vec2 p) {
    float v = 0.0, a = 0.5;
    for (int i = 0; i < 3; i++) {
        v += a * noise(p);
        p *= 2.0;
        a *= 0.5;
    }
    return v;
}

float triFbm(vec3 p, vec3 n) {
    vec3 w = abs(n);
    w = w / (w.x + w.y + w.z + 0.0001);
    return fbm(p.xy) * w.z + fbm(p.yz) * w.x + fbm(p.xz) * w.y;
}

void main() {
    vec3 vObjPos = vLocal * vScale.x;
    vec3 vObjNormal = normalize(cross(dFdx(vLocal), dFdy(vLocal)));
    vec3 vViewNormal = normalize(cross(dFdx(vView), dFdy(vView)));
    vec3 vViewPos = vView;

    float modelLen = 9.53;
    float flowDir = 1.0;

    float flowSpeed = 0.2;
    float veinCount = 2.2;
    float veinScale = 1.3;
    float goldLevel = 0.47;
    float goldSharp = 0.09;
    float muzzleBias = 0.22;

    float rimPow = 2.6;
    float rimBoost = 1.1;
    float specPow = 26.0;
    float specAmt = 0.85;
    float glintSpeed = 0.45;
    float glintAmt = 0.5;
    float grainAmt = 0.22;

    vec3 GOLD = vec3(0.439, 0.0, 0.8);
    vec3 DEEP = vec3(0.36, 0.2, 0.04);
    vec3 LIGHT = vec3(1.0, 0.95, 0.8);
    vec3 BLACK = vec3(0.047, 0.0, 0.059);

    float t = flowDir * vObjPos.x / max(modelLen, 0.0001) + 0.5;

    float n = triFbm(vObjPos * veinScale + vec3(-time * flowSpeed, 0.0, 0.0), vObjNormal);

    float streak = 0.5 + 0.5 * sin((t * veinCount - time * flowSpeed) * 6.2831 + (n - 0.5) * 5.0);

    float mask = mix(n, streak, 0.70) + (t - 0.5) * muzzleBias;
    float g = smoothstep(goldLevel - goldSharp, goldLevel + goldSharp, mask);

    float lam = clamp(dot(normalize(vViewNormal), normalize(vec3(0.35, 0.70, 0.62))), 0.0, 1.0);

    vec3 gold = mix(DEEP, GOLD, smoothstep(goldLevel - 0.18, goldLevel + 0.30, mask));
    gold *= 0.70 + 0.55 * lam;
    gold += LIGHT * pow(lam, specPow) * specAmt;

    vec3 black = BLACK * (0.80 + grainAmt * n) * (0.75 + 0.60 * lam);
    black += vec3(0.30, 0.26, 0.22) * pow(lam, specPow * 1.6) * 0.35;

    vec3 col = mix(black, gold, g);

    float seam = smoothstep(goldSharp, 0.0, abs(mask - goldLevel));
    col += LIGHT * seam * 0.22;

    float fres = 1.0 - abs(dot(normalize(vViewNormal), normalize(-vViewPos)));
    float rim = pow(clamp(fres, 0.0, 1.0), rimPow);
    col += mix(GOLD, LIGHT, 0.35) * rim * rimBoost;

    float sweep = fract(t * 0.85 - time * glintSpeed);
    float glint = smoothstep(0.0, 0.05, sweep) * smoothstep(0.14, 0.05, sweep);
    col += LIGHT * glint * (0.20 + 0.80 * rim) * glintAmt;

    kn_frag_color = vec4(min(col, vec3(1.4)), 1.0);
}
