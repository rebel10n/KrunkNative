#version 450 core
// Krunker v10 "custom" skin shaders (deob-10.0.4.js:91682 build list, :196092 table): the node graph reads the mesh's
// local position, its normal and the view direction; client/src/skin_fx.c builds the fragment stage from
// assets/shaders/skin_custom/<n>.frag. KN meshes carry no normal attribute: the fragment derives the face normal.
layout (location = 0) in vec3 v_pos;
layout (location = 1) in vec2 v_tex_coord;

out vec2 vUv;
out vec3 vLocal; // positionLocal
out vec3 vView;  // positionView

uniform mat4 transform;
uniform mat4 camera_world_inverse;
uniform mat4 camera_projection;

void main() {
    vUv = v_tex_coord;
    vLocal = v_pos;
    vec4 view = camera_world_inverse * transform * vec4(v_pos, 1.0);
    vView = view.xyz;
    gl_Position = camera_projection * view;
}
