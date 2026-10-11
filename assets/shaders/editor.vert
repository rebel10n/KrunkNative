#version 450 core
// Map editor overlays (client/src/editor/editor_render.c): bounding boxes, helper
// volumes, grid, death barrier, sun marker and the transform gizmo.
layout (location = 0) in vec3 v_pos;

uniform mat4 view_projection;
uniform mat4 model;

out float view_depth;

void main() {
    vec4 world = model * vec4(v_pos, 1.0);
    gl_Position = view_projection * world;
    view_depth = gl_Position.w;
}
