#version 450 core
// Krunker v7.1.4 ShaderToyMaterial (src/133.js C240) vertex stage: the mesh UVs as vUv
// (client/src/skin_fx.c builds the fragment stage around assets/shaders/shadertoy/<n>.glsl)
layout (location = 0) in vec3 v_pos;
layout (location = 1) in vec2 v_tex_coord;

out vec2 vUv;

uniform mat4 transform;
uniform mat4 camera_world_inverse;
uniform mat4 camera_projection;

void main() {
    vUv = v_tex_coord;
    gl_Position = camera_projection * camera_world_inverse * transform * vec4(v_pos, 1.0);
}
