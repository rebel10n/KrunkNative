#version 450 core
layout (location = 0) in vec3 v_pos;
layout (location = 1) in vec2 v_tex_coord;
layout (location = 2) in vec3 v_color;

out vec2 tex_coord;
out vec3 color;

uniform mat4 transform;
uniform mat4 camera_world_inverse;
uniform mat4 camera_projection;
uniform vec2 tex_offset;

void main() {
    tex_coord = v_tex_coord + tex_offset;
    color = v_color;
    gl_Position = camera_projection * camera_world_inverse * transform * vec4(v_pos, 1.0);
}
