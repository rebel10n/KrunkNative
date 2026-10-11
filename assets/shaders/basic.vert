#version 450 core
layout (location = 0) in vec3 v_pos;
layout (location = 1) in vec2 v_tex_coord;

out vec2 tex_coord;
out vec3 world_pos;
out vec3 local_pos;
out float view_depth;
out vec4 shadow_coord;
out float vertex_shade;
flat out int face_idx;

uniform mat4 transform;
uniform mat4 camera_world_inverse;
uniform mat4 camera_projection;
uniform mat4 shadow_matrix; // sun shadow: world -> [0,1] shadow-map coords (shadow.c)

// Krunker liquids (55.js render, objectAnim): vertex z = wHeight * sin(index / 5 + liquidVal * wSpeed / 4)
uniform float wave_height;   // 0: no waves
uniform float wave_speed;
uniform float liquid_value;  // liquidVal at the last 50 ms liquid update

void main() {
    tex_coord = v_tex_coord;
    local_pos = v_pos; // unit cube/plane position, for the Krunker UV modes in basic.frag
    face_idx = gl_VertexID / 4; // cube face (4 vertices per face, see loaders.c cube_vertices)
    // noise-plane bumps (31.js generatePlane noise): raised vertices (z >= 1) carry the darker colour
    vertex_shade = v_pos.z >= 1.0 ? 1.0 : 0.0;

    vec3 position = v_pos;
    if (wave_height != 0.0) position.z += wave_height * sin(float(gl_VertexID) / 5.0 + liquid_value * wave_speed / 4.0);

    vec4 transformed_pos = transform * vec4(position, 1.0);
    vec4 view_pos = camera_world_inverse * transformed_pos;
    world_pos = transformed_pos.xyz;
    shadow_coord = shadow_matrix * transformed_pos;
    view_depth = -view_pos.z; // three.js fogDepth = -mvPosition.z (view-space depth, not distance)
    gl_Position = camera_projection * view_pos;
}
