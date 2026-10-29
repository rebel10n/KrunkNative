// Krunker v10.0.4 skin shader 15 "Sketch" (deob-10.0.4.js:196133-196140; its TSL build, the one 10.0.4 draws: :92856-92923,
// seeded): cross-hatched pencil shading on paper, laid out on the part's dominant local plane. uSeed is the owned copy's
// seed mixed into [0, 1) (client/src/skin_fx.c skin_fx_copy_seed, :24344-24353): it turns, scales, thickens, wobbles and
// inks the hatching and shifts its shadow bands and the paper; 0 (shop previews, no owned copy) is the build's defaults.
// normalLocal is the model's vertex normal (attribute 3), else the face normal from dFdx / dFdy of positionLocal.
out vec4 kn_frag_color;
in vec2 vUv;
in vec3 vLocal;
in vec3 vNormal;

uniform float uSeed;

const vec3 PAPER_COLOR = vec3(0.91, 0.875, 0.81);
const vec3 INK_COLOR = vec3(0.25, 0.25, 0.25);

// the seed's random for one key (iIïïîiî, :92863-92865)
float seeded(float k) { return fract(sin(uSeed * k + 0.618 * k) * 43758.5453); }
// the value with a seed, else the default (iÌìîiiì, :92866-92868)
float pick(float def, float value) { return uSeed > 0.0 ? value : def; }

float hash(vec2 position) {
    return fract(sin(dot(position, vec2(127.1, 311.7))) * 43758.5453123);
}

float noise(vec2 position) {
    vec2 cell = floor(position);
    vec2 local = fract(position);
    vec2 u = local * local * (3.0 - 2.0 * local);
    return mix(mix(hash(cell), hash(cell + vec2(1.0, 0.0)), u.x),
               mix(hash(cell + vec2(0.0, 1.0)), hash(cell + vec2(1.0, 1.0)), u.x), u.y);
}

void main() {
    // the seed's look (:92869-92875)
    float rotation = pick(0.0, (seeded(12.9898) - 0.5) * 0.9);
    float scale = pick(1.0, mix(0.7, 1.4, seeded(78.233)));
    float thickness = pick(0.0, pow(seeded(37.719), 2.0) * 0.09);
    float wobble = pick(1.0, mix(0.35, 2.2, seeded(51.123)));
    float inkScale = pick(1.0, mix(0.45, 2.2, seeded(93.989)));
    float inkMin = pick(0.1, mix(0.0, 0.45, seeded(24.681)));
    float shadowShift = pick(0.0, (seeded(63.7) - 0.5) * 0.24);
    float c = cos(rotation), s = sin(rotation);
    // the two hatch directions, turned (iÏîìïiì, :92878-92883)
    vec2 first = normalize(vec2(0.9, 0.45));
    vec2 second = normalize(vec2(-0.15, 0.95));
    first = vec2(c * first.x - s * first.y, s * first.x + c * first.y);
    second = vec2(c * second.x - s * second.y, s * second.x + c * second.y);

    vec3 normal = dot(vNormal, vNormal) > 0.0 ? normalize(vNormal) : normalize(cross(dFdx(vLocal), dFdy(vLocal)));
    vec3 axis = abs(normal);
    vec3 local = vLocal * 0.25;
    // the dominant plane (:92907): x -> zy, y -> xz, else xy; offset by the seed
    vec2 plane = axis.x > max(axis.y, axis.z) ? local.zy : (axis.y > axis.z ? local.xz : local.xy);
    plane += vec2(uSeed * 1000.0, uSeed * 617.0);
    float diffuse = normal.y * 0.5 + 0.5;

    float paperVariation = mix(0.97, 1.03, mix(noise(plane * 0.008), noise(plane * 0.035), 0.2));
    vec3 paper = PAPER_COLOR * paperVariation * mix(0.88, 1.0, diffuse);

    // two hatch layers (iÏìîiíì, :92890-92902), each fading in below its shadow band
    float hatches[2];
    for (int layer = 0; layer < 2; layer++) {
        vec2 direction = layer == 0 ? first : second;
        float coordinate = dot(plane * (scale * 0.1), direction);
        coordinate += (noise(vec2(coordinate * 1.7, 0.0)) - 0.5) * wobble;
        float line = abs(fract(coordinate) - 0.5);
        float width = max(fwidth(coordinate), 0.001);
        float edge = thickness + 0.001;
        float hatchLine = 1.0 - smoothstep(edge, edge + max(0.025, width), line);
        float variation = mix(noise(plane * (inkScale * 0.018) + direction * 13.7), noise(plane * (inkScale * 0.065) + direction * 31.4), 0.25);
        hatches[layer] = hatchLine * mix(inkMin, 1.0, smoothstep(0.15, 0.85, variation));
    }
    float firstLayer = hatches[0] * (1.0 - smoothstep(shadowShift + 0.32, shadowShift + 0.68, diffuse));
    float secondLayer = hatches[1] * (1.0 - smoothstep(shadowShift, shadowShift + 0.38, diffuse));
    vec3 color = mix(paper, INK_COLOR, max(firstLayer, secondLayer));
    // uv.y outside [0, 1]: the ink (:92915)
    if (vUv.y < 0.0 || vUv.y > 1.0) color = INK_COLOR;
    kn_frag_color = vec4(color, 1.0);
}
