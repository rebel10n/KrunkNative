#version 460
layout (location = 0) in vec3 v_pos;
layout (location = 1) in vec2 v_tex_coord;

out vec2 tex_coord;

void main() {
    tex_coord = v_tex_coord;
    gl_Position = vec4(v_pos, 1.0f);
}