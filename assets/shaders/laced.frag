#version 450 core
// Nettspend "laced" post-effect (client/src/render_target.c, client/include/nettspend.h): the
// high visuals after a bad pour of Purple Drank. Replaces scene_composite.frag's pass while the
// effect runs (same vertex shader, same `filtered` saturate / hue step at the end, so the in-game
// CSS filter is kept): a slow breathing zoom, a wobble of the picture, chromatic fringes that
// grow toward the edges, a ghost image drifting about, a purple pulse and a soft vignette.
// `intensity` 0..1 fades everything in and out (nettspend_laced_intensity). GLSL ES 3.00 safe.
in vec2 uv;
out vec4 frag_color;

uniform sampler2D scene; // texture unit 0 (RT_SCENE_TEXTURE_UNIT)
uniform float time;      // seconds since the dose
uniform float intensity; // 0..1
uniform bool filtered;
uniform mat3 saturate_matrix;
uniform mat3 hue_matrix;

const vec3 LEAN = vec3(0.48, 0.17, 0.75); // #7b2cbf

vec2 clamp_uv(vec2 p) {
    return clamp(p, vec2(0.001), vec2(0.999));
}

void main() {
    float k = intensity;
    vec2 centred = uv - 0.5;

    // breathing: the picture swells and shrinks about the centre
    float breath = 1.0 - 0.045 * k * (0.5 + 0.5 * sin(time * 1.3));
    vec2 p = 0.5 + centred * breath;

    // the wobble: two slow waves across the frame, stronger away from the centre
    float edge = smoothstep(0.0, 0.7, length(centred));
    p.x += 0.013 * k * sin(p.y * 9.0 + time * 2.1) * (0.4 + 0.6 * edge);
    p.y += 0.011 * k * cos(p.x * 7.0 + time * 1.7) * (0.4 + 0.6 * edge);

    // chromatic fringes along the radius
    vec2 fringe = centred * 0.012 * k * (0.5 + edge);
    vec3 color;
    color.r = texture(scene, clamp_uv(p + fringe)).r;
    color.g = texture(scene, clamp_uv(p)).g;
    color.b = texture(scene, clamp_uv(p - fringe)).b;

    // the ghost: a second, fainter picture drifting in a slow circle
    vec2 drift = vec2(cos(time * 0.9), sin(time * 0.7)) * 0.02 * k;
    vec3 ghost = texture(scene, clamp_uv(p + drift)).rgb;
    color = mix(color, ghost, 0.35 * k);

    // the purple pulse and the vignette
    float pulse = 0.5 + 0.5 * sin(time * 2.4);
    float tint = k * (0.1 + 0.1 * pulse);
    color = mix(color, color * (0.55 + 0.45 * LEAN / max(max(LEAN.r, LEAN.g), LEAN.b)) + LEAN * 0.12, tint);
    float vignette = smoothstep(0.35, 1.0, length(centred) * 1.25);
    color = mix(color, color * (1.0 - 0.6 * vignette) + LEAN * 0.08 * vignette, k);

    if (filtered) {
        color = clamp(saturate_matrix * color, 0.0, 1.0);
        color = clamp(hue_matrix * color, 0.0, 1.0);
    }
    frag_color = vec4(clamp(color, 0.0, 1.0), 1.0);
}
