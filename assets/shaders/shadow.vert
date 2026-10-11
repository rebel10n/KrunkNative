#version 450 core
// Depth-only pass for the sun shadow map (shadow.c).
layout (location = 0) in vec3 v_pos;

uniform mat4 transform;
uniform mat4 light_view_projection;

void main() {
    gl_Position = light_view_projection * transform * vec4(v_pos, 1.0);
}
