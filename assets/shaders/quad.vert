#version 450 core
layout (location = 0) in vec3 v_pos;
layout (location = 1) in vec2 v_tex_coord;

uniform vec2 offset;
uniform vec2 scale;

out vec2 tex_coord;

void main() {
    tex_coord = v_tex_coord;

    vec2 transformed = v_pos.xy * scale + offset;
    gl_Position = vec4(transformed.xy, 0.0f, 1.0f);
}