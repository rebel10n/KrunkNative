// Krunker v10.0.4 skin shader 14 "Shock" (deob-10.0.4.js:196123-196132; TSL twin :92674-92854, shotPulse): lightning
// arcs that play on each shot. client/src/skin_fx.c drives the build's four arcs (skin_fx_shot_pulse, custom_arcs_step):
// each shot takes an arc with its own animation time (uArcTime, a random start) and shock (uArcShock, sin(min(t / 0.75,
// 1) * PI) over 0.75 s, :92689-92717); the colour is the sum of the playing arcs (:92826-92839). The part shows only while
// an arc plays (the build hides the mesh otherwise). vViewNormal is the face normal (dFdx / dFdy: no normal attribute).
out vec4 kn_frag_color;
in vec2 vUv;
in vec3 vView;

uniform float uArcTime[4];
uniform float uArcShock[4];

#define vViewPosition vView

const float ANIMATION_SPEED = 0.35;
const float PAUSE_FREQUENCY = 20.0;
const float PAUSE_AMOUNT = 0.8;

const float ORBIT_SPEED = 1.0;
const float TILT_AMOUNT = 0.45;
const float TILT_SPEED = 1.4;
const float ROTATION_AMOUNT = 0.35;
const float ROTATION_SPEED = 0.8;

const float BEND_SCALE = 5.0;
const float BEND_SPEED_X = 5.0;
const float BEND_SPEED_Y = 3.0;
const float BEND_AMOUNT = 1.0;

const float BREAKUP_SCALE = 1.0;
const float BREAKUP_SPEED_X = 2.0;
const float BREAKUP_SPEED_Y = 5.0;
const float BREAKUP_AMOUNT = 0.1;

const float VORONOI_SCALE = 2.0;
const vec3 COLOR = vec3(0.0, 0.5, 0.0);
const vec3 GLOW_COLOR = vec3(1.0, 1.0, 1.0);

vec2 hash2(vec2 position) {
    return fract(sin(vec2(
        dot(position, vec2(127.1, 311.7)),
        dot(position, vec2(269.5, 183.3))
    )) * 43758.5453);
}

vec4 voronoi(vec2 position) {
    vec2 cell = floor(position);
    vec2 localPosition = fract(position);
    vec2 closestOffset;
    vec2 closestCell;
    float closestDistance = 8.0;

    for (int y = -1; y <= 1; y++) {
        for (int x = -1; x <= 1; x++) {
            vec2 cellOffset = vec2(float(x), float(y));
            vec2 difference = cellOffset + hash2(cell + cellOffset) - localPosition;
            float distanceSquared = dot(difference, difference);

            if (distanceSquared < closestDistance) {
                closestDistance = distanceSquared;
                closestOffset = difference;
                closestCell = cellOffset;
            }
        }
    }

    float originalDistance = closestDistance;
    closestDistance = 8.0;

    for (int y = -2; y <= 2; y++) {
        for (int x = -2; x <= 2; x++) {
            vec2 cellOffset = closestCell + vec2(float(x), float(y));
            vec2 difference = cellOffset + hash2(cell + cellOffset) - localPosition;

            if (dot(closestOffset - difference, closestOffset - difference) > 0.00001) {
                vec2 borderDirection = normalize(difference - closestOffset);
                float borderDistance = dot(0.5 * (closestOffset + difference), borderDirection);
                closestDistance = min(closestDistance, borderDistance);
            }
        }
    }

    return vec4(closestDistance, closestOffset, originalDistance);
}

vec2 rotate(vec2 position, float angle) {
    float sine = sin(angle);
    float cosine = cos(angle);

    return vec2(
        position.x * cosine - position.y * sine,
        position.x * sine + position.y * cosine
    );
}

// one arc (iÎïiììi, deob-10.0.4.js:92778-92824): its own time and shock
vec4 arc(float arcTime, float shock) {
    // Animation
    vec2 uv = vUv;

    float animationTime = arcTime * ANIMATION_SPEED;
    float pulse = shock * 2.0 - 1.0;
    float pulseOffset = -cos(asin(clamp(shock, 0.0, 1.0)));
    float steppedTime = animationTime + PAUSE_AMOUNT * pulse / PAUSE_FREQUENCY;

    // Surface Distortion
    vec2 distortionPosition = vec2(0.5 - steppedTime * 3.0);
    float distortion = voronoi(distortionPosition).x;

    uv.x += 1.5 * (2.0 * distortion - 0.33) * (uv.y - 0.5);

    // Lightning Path
    vec2 lineUv = uv - 0.5;

    lineUv.x += steppedTime * ORBIT_SPEED;

    float tilt = sin(steppedTime * TILT_SPEED) * TILT_AMOUNT;
    lineUv.y += lineUv.x * tilt;

    float rotation = sin(steppedTime * ROTATION_SPEED) * ROTATION_AMOUNT;
    lineUv = rotate(lineUv, rotation) + 0.5;

    // Merging Lines
    vec2 patternPosition = lineUv;
    patternPosition.y = max(patternPosition.y, 0.5);
    patternPosition *= VORONOI_SCALE;
    patternPosition += vec2(steppedTime * 11.0, steppedTime * 24.0);

    vec2 secondaryPosition = lineUv * VORONOI_SCALE;
    secondaryPosition += vec2(steppedTime * 4.0, -steppedTime * 7.0);

    float secondaryBend = voronoi(secondaryPosition).x;
    patternPosition.x += (secondaryBend - 0.25) * 2.0;

    // Camera Facing
    vec3 surfaceNormal = normalize(cross(dFdx(vViewPosition), dFdy(vViewPosition)));
    vec3 cameraDirection = normalize(-vViewPosition);

    float cameraFacing = max(dot(surfaceNormal, cameraDirection), 0.0);
    float edgeFade = pow(cameraFacing, 0.75);

    // Line Scale
    float pulseAmount = pow(0.5 * pulse + 0.5, 2.0);
    float pulseScale = pow(0.5 * pulseOffset + 0.5, 2.0);
    float lineScale = 2.0 * edgeFade * (0.25 + 0.75 * pulseScale);

    // Main Line
    patternPosition.y = 0.5 * VORONOI_SCALE + steppedTime * 3.0;

    float lineDistance = lineUv.y + voronoi(patternPosition).x * lineScale - 0.5;

    // Bending
    vec2 bendPosition = lineUv * BEND_SCALE;
    bendPosition += vec2(steppedTime * BEND_SPEED_X, -steppedTime * BEND_SPEED_Y);

    float bend = voronoi(bendPosition).x - 0.25;
    lineDistance += bend * BEND_AMOUNT;

    // Breakup
    vec2 breakupPosition = lineUv * BREAKUP_SCALE;
    breakupPosition += vec2(-steppedTime * BREAKUP_SPEED_X, steppedTime * BREAKUP_SPEED_Y);

    float breakup = smoothstep(0.05, 0.45, voronoi(breakupPosition).x);
    lineDistance += (breakup - 0.5) * BREAKUP_AMOUNT;

    // Lightning Mask
    float lineIntensity = 1.0 - abs(lineDistance * 0.4);
    float lineWidth = lineIntensity + 0.025 * pulseAmount * edgeFade;
    float lineMask = smoothstep(1.0, 1.005, lineWidth);

    // Color
    vec4 lineColor = mix(
        vec4(COLOR, 1.0),
        vec4(GLOW_COLOR, 1.0),
        pulseAmount
    );

    vec4 color = lineColor * lineMask;

    // Core
    color += edgeFade * pow(
        pulseAmount * smoothstep(0.75, 1.005, lineWidth),
        16.0
    ) * 0.5 * vec4(COLOR, 1.0);

    return color;
}

void main() {
    // the sum of the playing arcs (iÏiíiïí, deob-10.0.4.js:92826-92839); none playing: the build hides the part
    vec4 color = vec4(0.0);
    bool any = false;
    for (int a = 0; a < 4; a++) {
        if (uArcShock[a] > 0.0) {
            color += arc(uArcTime[a], uArcShock[a]);
            any = true;
        }
    }
    if (!any) discard;
    kn_frag_color = color;
}
