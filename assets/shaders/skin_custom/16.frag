// Krunker v10.0.4 skin shader 16 "Ghillie" (deob-10.0.4.js:196141-196147; TSL twin :92925-92958): domain-warped camo
// blotches in four greens, on the local position with the part's stretch undone (vPos = position * scale) so they stay
// round on every part.
out vec4 kn_frag_color;
in vec3 vLocal;
in vec3 vScale;

float hash(vec3 p) {
    p = fract(p * 0.3183099 + vec3(0.1, 0.2, 0.3));
    p *= 17.0;
    return fract(p.x * p.y * p.z * (p.x + p.y + p.z));
}

float noise(vec3 x) {
    vec3 i = floor(x);
    vec3 f = fract(x);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(mix(hash(i + vec3(0.0, 0.0, 0.0)), hash(i + vec3(1.0, 0.0, 0.0)), f.x),
                   mix(hash(i + vec3(0.0, 1.0, 0.0)), hash(i + vec3(1.0, 1.0, 0.0)), f.x), f.y),
               mix(mix(hash(i + vec3(0.0, 0.0, 1.0)), hash(i + vec3(1.0, 0.0, 1.0)), f.x),
                   mix(hash(i + vec3(0.0, 1.0, 1.0)), hash(i + vec3(1.0, 1.0, 1.0)), f.x), f.y), f.z);
}

float fbm(vec3 p) {
    float v = 0.0;
    float a = 0.5;
    for (int i = 0; i < 3; i++) {
        v += a * noise(p);
        p *= 2.0;
        a *= 0.5;
    }
    return v;
}

void main() {
    vec3 c0 = vec3(0.15, 0.18, 0.09); // dark green
    vec3 c1 = vec3(0.34, 0.40, 0.19); // shadow olive
    vec3 c2 = vec3(0.44, 0.51, 0.26); // main green
    vec3 c3 = vec3(0.66, 0.70, 0.47); // light sage

    float scale = 1.5;
    vec3 q = (vLocal * vScale) * scale;

    vec3 w = vec3(noise(q * 2.0 + 3.1), noise(q * 2.0 + 7.7), noise(q * 2.0 + 1.3));
    float p = fbm(q + (w - 0.5) * 0.9);

    vec3 col;
    if (p < 0.38)
        col = c0;
    else if (p < 0.47)
        col = c1;
    else if (p < 0.60)
        col = c2;
    else
        col = c3;

    kn_frag_color = vec4(col, 1.0);
}
