#version 450 core
// Three.js r121 MeshBasicMaterial with vertexColors (+ optional map), no fog:
// gl_FragColor = vec4(texel.rgb * vColor, texel.a).
out vec4 frag_color;

in vec2 tex_coord;
in vec3 color;

uniform bool use_texture;
uniform sampler2D tex;

void main() {
    vec4 texel = use_texture ? texture(tex, tex_coord) : vec4(1.0);
    frag_color = vec4(texel.rgb * color, texel.a);
}
