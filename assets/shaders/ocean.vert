#version 450 core
// three.js examples Water.js (Krunker v7.1.4 libs/Water.js) vertex shader
layout (location = 0) in vec3 v_pos;
layout (location = 1) in vec2 v_tex_coord;

out vec4 mirror_coord;
out vec4 world_position;
out float view_depth;
out vec4 shadow_coord;

uniform mat4 transform;
uniform mat4 camera_world_inverse;
uniform mat4 camera_projection;
uniform mat4 texture_matrix; // the mirror camera's bias * projection * view
uniform mat4 shadow_matrix;  // sun shadow (shadow.c)

void main() {
    world_position = transform * vec4(v_pos, 1.0);
    mirror_coord = texture_matrix * world_position;
    vec4 view_pos = camera_world_inverse * world_position;
    view_depth = -view_pos.z;
    shadow_coord = shadow_matrix * world_position;
    gl_Position = camera_projection * view_pos;
}
