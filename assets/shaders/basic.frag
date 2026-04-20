#version 460
out vec4 frag_color;
in vec2 tex_coord;

uniform vec4 color;
uniform mat3 tex_transform;
uniform sampler2D tex;

void main() {
    vec3 t_tex_coord = tex_transform * vec3(tex_coord, 1.0);
    frag_color = texture(tex, t_tex_coord.xy) * color;
}