#version 450 core
layout (location = 0) in vec3 v_pos;
layout (location = 1) in vec2 v_tex_coord;

out vec2 tex_coord;  // the texture's uv (scrolled)
out vec2 sphere_uv;  // the geometry uv (SkyDome getGradient)

uniform mat4 transform;
uniform mat4 camera_world_inverse;
uniform mat4 camera_projection;
uniform vec2 tex_offset;

void main() {
    tex_coord = v_tex_coord + tex_offset;
    sphere_uv = v_tex_coord;
    gl_Position = camera_projection * camera_world_inverse * transform * vec4(v_pos, 1.0);
}
