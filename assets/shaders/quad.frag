#version 420 core
out vec4 frag_color;
in vec2 tex_coord;

uniform vec4 color;

uniform float tex_viewport[4];
uniform sampler2D tex;

void main() {
    frag_color = texture(tex, vec2(
        tex_coord.x * tex_viewport[2] - tex_viewport[0],
        tex_coord.y * tex_viewport[3] - tex_viewport[1]
    )) * color;
}