#version 450 core
// LineMaterial: three.js LineBasicMaterial / MeshBasicMaterial (no lighting, fog: true).
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
        // THREE.Fog(color, near, far): smoothstep(near, far, depth)
        float f = smoothstep(fog_near, fog_far, view_depth);
        c = mix(c, fog_color, f);
    }
    frag_color = vec4(c, color.a);
}
