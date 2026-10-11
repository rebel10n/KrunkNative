#version 450 core
// HTML/CSS engine vertex shader (client/src/html/engine/gl_backend.c).
// Written for GLSL 450 core restricted to the GLSL ES 3.00 subset; shader.c
// rewrites the version line for WebGL2.
layout (location = 0) in vec2 a_pos;          // target pixels, y down
layout (location = 1) in vec2 a_uv;           // mode dependent (local px / texcoord)
layout (location = 2) in vec4 a_color;        // premultiplied rgba (normalized ubyte)
layout (location = 3) in vec4 a_p0;
layout (location = 4) in vec4 a_p1;
layout (location = 5) in vec4 a_p2;
layout (location = 6) in vec4 a_clip;         // x0 y0 x1 y1, target pixels
layout (location = 7) in float a_clip_radius;
layout (location = 8) in float a_mode;

uniform vec2 u_viewport;                      // target size in pixels

out vec2 v_pos;
out vec2 v_uv;
out vec4 v_color;
flat out vec4 v_p0;
flat out vec4 v_p1;
flat out vec4 v_p2;
flat out vec4 v_clip;
flat out float v_clip_radius;
flat out float v_mode;

void main() {
    v_pos = a_pos;
    v_uv = a_uv;
    v_color = a_color;
    v_p0 = a_p0;
    v_p1 = a_p1;
    v_p2 = a_p2;
    v_clip = a_clip;
    v_clip_radius = a_clip_radius;
    v_mode = a_mode;

    gl_Position = vec4(a_pos.x / u_viewport.x * 2.0 - 1.0, 1.0 - a_pos.y / u_viewport.y * 2.0, 0.0, 1.0);
}
