#version 420 core
out vec4 frag_color;
in vec2 tex_coord;

uniform vec4 color;
uniform sampler2D tex;

void main() {
    frag_color = texture(tex, tex_coord) * color;
}