#version 450 core
out vec4 frag_color;
in vec2 tex_coord;

uniform vec4 color;
uniform sampler2D tex;

void main() {
    frag_color = vec4(1.0, 1.0, 1.0, texture(tex, vec2(tex_coord.x, 1.0 - tex_coord.y)).r) * color;
}