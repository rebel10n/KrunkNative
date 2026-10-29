// Krunker v10.0.4 skin shader 8 "IdolShader" (deob-10.0.4.js:195962-195970; TSL twin :92213-92252): a scanline hologram.
// client/src/skin_fx.c prepends the GLSL header; skin_custom.vert supplies vClip (the bundle's vPos).
out vec4 kn_frag_color;
in vec2 vUv;
in vec4 vClip;

uniform float time;

const float scanlineSize = 8.0;
const float hologramBrightness = 2.0;
const float signalSpeed = 1.0;
const float hologramOpacity = 1.0;
const vec3 hologramColorRgb = vec3(0.7215686, 0.5568628, 0.96862745); // 0xb88ef7

float random(in float a, in float b) { return fract((cos(dot(vec2(a, b), vec2(12.9898, 78.233))) * 43758.5453)); }

void main() {
    vec2 vCoords = vClip.xy;
    vCoords /= vClip.w;
    vCoords = vCoords * 0.5 + 0.5;
    vec2 myUV = fract(vCoords);

    vec4 hologramColor = vec4(hologramColorRgb, mix(hologramBrightness, vUv.y, 0.5));

    float scanlines = 10.0;
    scanlines += 20.0 * sin(time * signalSpeed * 20.8 - myUV.y * 60.0 * scanlineSize);
    scanlines *= smoothstep(1.3 * cos(time * signalSpeed + myUV.y * scanlineSize), 0.78, 0.9);
    scanlines *= max(0.25, sin(time * signalSpeed) * 1.0);

    float r = random(vUv.x, vUv.y);
    float g = random(vUv.y * 20.2, vUv.y * 0.2);
    float b = random(vUv.y * 0.9, vUv.y * 0.2);

    hologramColor += vec4(r * scanlines, b * scanlines, r, 1.0) / 84.0;
    vec4 scanlineMix = mix(vec4(0.0), hologramColor, hologramColor.a);

    kn_frag_color = vec4(scanlineMix.rgb, hologramOpacity);
}
