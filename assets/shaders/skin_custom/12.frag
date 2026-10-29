// Krunker v10.0.4 skin shader 12 (deob-10.0.4.js:196105-196112; TSL twin :92519-92583): a raymarched tunnel of morphing
// sphere / cube / torus cells over the item's UVs, on a 7.5 s loop ("Void Fractured").
out vec4 kn_frag_color;
in vec2 vUv;

uniform float time;

#define PI 3.14159265359
#define TAU 6.28318530718

const float LOOP = 7.5;

void main() {
    vec2 uv = fract(vUv);
    float loopProgress = fract(time / LOOP);
    float smoothPhase = smoothstep(0.0, 1.0, sin(loopProgress * PI));
    vec4 base = vec4(0.0, 0.0, 0.0, 1.0);
    float depth = 0.0;
    for (int step = 0; step < 72; step++) {
        vec3 rayDir = normalize(vec3(uv, 1.5));
        vec4 samplePos = vec4(depth * rayDir, 0.8);

        float rotationAngle = 0.5 * smoothPhase + 0.35 * samplePos.z;
        mat2 rotXY = mat2(cos(rotationAngle), sin(rotationAngle), -sin(rotationAngle), cos(rotationAngle));
        samplePos.xy *= rotXY;

        vec4 tiled = samplePos;
        tiled -= round(tiled);
        tiled.xz *= rotXY;
        tiled.yw *= rotXY;

        float distSphere = length(tiled.xyz) - 0.6;
        float distCube = length(max(abs(tiled.xyz) - 0.5, 0.0));
        float distTorus = length(vec2(length(tiled.xz) - 0.8, tiled.y)) - 0.25;

        float dist;
        if (loopProgress < 0.33)
            dist = mix(distSphere, distCube, smoothstep(0.0, 0.33, loopProgress));
        else if (loopProgress < 0.66)
            dist = mix(distCube, distTorus, smoothstep(0.33, 0.66, loopProgress));
        else
            dist = mix(distTorus, distSphere, smoothstep(0.66, 1.0, loopProgress));

        dist += 0.02 * sin(20.0 * tiled.x) * cos(15.0 * tiled.y);
        dist = abs(dist) + 0.001;

        vec3 red = pow(abs(sin(tiled.xyz * 3.0 + smoothPhase * TAU)), vec3(1.5)) * vec3(1.0, 0.05, 0.1);
        vec3 blue = pow(abs(cos(tiled.xyz * 2.5 - smoothPhase * TAU)), vec3(1.5)) * vec3(0.1, 0.05, 1.0);
        vec3 purple = pow(abs(sin(tiled.yzx * 2.8 + smoothPhase * TAU)), vec3(1.5)) * vec3(0.6, 0.1, 0.8);
        vec3 pink = pow(abs(cos(tiled.zxy * 3.2 - smoothPhase * TAU)), vec3(1.5)) * vec3(1.0, 0.3, 0.7);

        vec3 blended = mix(red, blue, smoothstep(-0.5, 0.5, tiled.x));
        blended = mix(blended, purple, smoothstep(-0.5, 0.5, tiled.y));
        blended = mix(blended, pink, smoothstep(-0.5, 0.5, tiled.z));

        base.rgb += blended / dist;
        depth += 0.5 * dist;
    }

    base.rgb += pow(1.0 - smoothPhase, 2.0) * pow(depth, 2.0) * vec3(1.5, 0.8, 2.0);
    base.rgb *= 1.7;
    kn_frag_color = vec4(tanh(base.rgb / 9000.0), 1.0);
}
