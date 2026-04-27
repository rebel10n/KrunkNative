#version 450 core
out vec4 frag_color;
in vec2 tex_coord;

uniform vec4 color;
uniform sampler2D tex;

void main() {
    frag_color = vec4(1.0f, 1.0f, 1.0f, texture(tex, vec2(tex_coord.x, 1.0f - tex_coord.y)).r) * color;
}