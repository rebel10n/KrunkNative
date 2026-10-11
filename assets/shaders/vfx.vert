#version 450 core
// Combat effects (client/src/vfx.c): sprites, tracer/casing boxes and explosion
// debris arrive pre-transformed in world space (screen_space: NDC) with a
// per-vertex colour that already includes lighting where Krunker lights them.
layout (location = 0) in vec3 v_pos;
layout (location = 1) in vec2 v_tex_coord;
layout (location = 2) in vec4 v_color;

out vec2 tex_coord;
out vec4 color;
out float view_depth;

uniform bool screen_space;
uniform mat4 camera_world_inverse;
uniform mat4 camera_projection;

// No early `return` from main(): the NVIDIA driver (RTX 2070 SUPER, Linux) miscompiled the
// earlier `if (screen_space) { ...; return; }` form and every varying (colour, UV, view
// depth) reached the fragment shader scaled by about 1 / w, so tracers and casings went
// black with distance and textured sprites (muzzle flash, smoke) vanished. Mesa/llvmpipe
// and WebGL were unaffected. Both paths are computed and selected instead.
void main() {
    tex_coord = v_tex_coord;
    color = v_color;

    vec4 view_pos = camera_world_inverse * vec4(v_pos, 1.0);
    view_depth = screen_space ? 0.0 : length(view_pos.xyz);
    gl_Position = screen_space ? vec4(v_pos.xy, 0.0, 1.0) : camera_projection * view_pos;
}
