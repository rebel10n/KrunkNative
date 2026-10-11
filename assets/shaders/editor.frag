#version 450 core
uniform vec4 color;
uniform vec3 fog_color;
uniform float fog_near;
uniform float fog_far;
uniform int fog_enabled;

in float view_depth;
out vec4 frag_color;

void main() {
    vec3 c = color.rgb;
    if (fog_enabled != 0) {
        // three.js linear fog (LineBasicMaterial / MeshBasicMaterial are fogged by default)
        float f = smoothstep(fog_near, fog_far, view_depth);
        c = mix(c, fog_color, f);
    }
    frag_color = vec4(c, color.a);
}
