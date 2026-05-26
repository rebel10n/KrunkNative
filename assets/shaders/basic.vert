#version 450 core
layout (location = 0) in vec3 v_pos;
layout (location = 1) in vec2 v_tex_coord;

out vec2 tex_coord;
out vec3 world_pos;
out float view_depth;
out flat int face_idx;

uniform bool is_ramp;

uniform mat4 transform;
uniform mat4 camera_world_inverse;
uniform mat4 camera_projection;

void main() {
    tex_coord = v_tex_coord;

    if (is_ramp) {
        if (gl_VertexID < 4) face_idx = 0;
        else if (gl_VertexID < 8) face_idx = 1;
        else if (gl_VertexID - 8 < 6) face_idx = 2;
        else face_idx = 3;
    } else {
        face_idx = gl_VertexID / 4; // only works for quad mesh!
    }

    vec4 transformed_pos = transform * vec4(v_pos, 1.0f);
    vec4 view_pos = camera_world_inverse * transformed_pos;
    world_pos = transformed_pos.xyz;
    view_depth = length(view_pos.xyz);
    gl_Position = camera_projection * view_pos;
}
