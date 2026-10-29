// Krunker v10.0.4 skin shader 9 "GasFresnal" (deob-10.0.4.js:195971-196019; TSL twin :92254-92368): a gas nebula with a
// fresnel edge and star clusters. Uniform presets "main" / "blackhole" (shaderArgs.uniforms) come from client/src/skin_fx.c.
// vPositionW / vNormalW are the view-space direction and the face normal (from dFdx / dFdy). uWobble's displacement along
// normalLocal (:92332-92337) is skin_custom.vert's uDisplace 1.
out vec4 kn_frag_color;
in vec2 vUv;
in vec3 vView;

uniform vec3 uBase;
uniform vec3 uAccent;
uniform float uScale;
uniform float uPower;
uniform float time;
uniform float uStars; // the bundle's bool uStars, 0 / 1

vec4 mod289(vec4 x) { return x - floor(x * (1.0 / 289.0)) * 289.0; }
vec4 permute(vec4 x) { return mod289(((x * 34.0) + 10.0) * x); }
vec4 taylorInvSqrt(vec4 r) { return 1.79284291400159 - 0.85373472095314 * r; }
vec2 fade(vec2 t) { return t * t * t * (t * (t * 6.0 - 15.0) + 10.0); }

float cnoise(vec2 P) {
    vec4 Pi = floor(P.xyxy) + vec4(0.0, 0.0, 1.0, 1.0);
    vec4 Pf = fract(P.xyxy) - vec4(0.0, 0.0, 1.0, 1.0);
    Pi = mod289(Pi);
    vec4 ix = Pi.xzxz;
    vec4 iy = Pi.yyww;
    vec4 fx = Pf.xzxz;
    vec4 fy = Pf.yyww;
    vec4 i = permute(permute(ix) + iy);
    vec4 gx = fract(i * (1.0 / 41.0)) * 2.0 - 1.0;
    vec4 gy = abs(gx) - 0.5;
    vec4 tx = floor(gx + 0.5);
    gx = gx - tx;
    vec2 g00 = vec2(gx.x, gy.x);
    vec2 g10 = vec2(gx.y, gy.y);
    vec2 g01 = vec2(gx.z, gy.z);
    vec2 g11 = vec2(gx.w, gy.w);
    vec4 norm = taylorInvSqrt(vec4(dot(g00, g00), dot(g01, g01), dot(g10, g10), dot(g11, g11)));
    float n00 = norm.x * dot(g00, vec2(fx.x, fy.x));
    float n10 = norm.y * dot(g10, vec2(fx.y, fy.y));
    float n01 = norm.z * dot(g01, vec2(fx.z, fy.z));
    float n11 = norm.w * dot(g11, vec2(fx.w, fy.w));
    vec2 fade_xy = fade(Pf.xy);
    vec2 n_x = mix(vec2(n00, n01), vec2(n10, n11), fade_xy.x);
    float n_xy = mix(n_x.x, n_x.y, fade_xy.y);
    return 2.3 * n_xy;
}

void main() {
    vec3 vPositionW = normalize(vView);
    vec3 vNormalW = normalize(cross(dFdx(vView), dFdy(vView)));

    float turbulence = cnoise(-(vUv * uScale) + time / 2.0);
    float noise = cnoise((vUv * uScale) + time / 2.0);
    float displacement = (noise + turbulence);

    float fresnelTerm = (uPower + min(dot(vPositionW, normalize(vNormalW)), 0.0));

    vec3 base = uBase * 4.0;
    vec3 gas = max(uAccent * displacement, base); // the three per-channel `if (gas.c < base.c) gas.c = base.c`

    if (uStars > 0.5) {
        float starSize = (1.0 / 0.01) * uScale / 10.0;
        float cluster = cnoise(vUv * starSize + time);
        float clampedCluster = clamp(abs(pow(cluster, 1.0 / 0.09)), 0.0, 1.0);
        if (clampedCluster == 1.0) clampedCluster = 0.0;
        clampedCluster *= 3.0;
        vec4 stars = vec4(clampedCluster);
        kn_frag_color = stars + vec4(gas * vec3(fresnelTerm), 1.0);
    } else {
        kn_frag_color = vec4(gas * vec3(fresnelTerm), 1.0);
    }
}
