#version 450 core
// LineMaterial (client/src/materials/line.c): unlit single-colour lines and flat overlays drawn
// through scene_render(), which uploads transform / camera_world_inverse / camera_projection.
layout (location = 0) in vec3 v_pos;

uniform mat4 transform;
uniform mat4 camera_world_inverse;
uniform mat4 camera_projection;

out float view_depth;

void main() {
    vec4 view_pos = camera_world_inverse * transform * vec4(v_pos, 1.0);
    view_depth = -view_pos.z; // three.js fogDepth = -mvPosition.z
    gl_Position = camera_projection * view_pos;
}
