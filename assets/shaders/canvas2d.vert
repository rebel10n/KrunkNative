#version 450 core
// Canvas2D (client/src/ks/canvas2d.c): positions in device pixels, y down, origin top-left.
layout (location = 0) in vec2 v_pos;
layout (location = 1) in vec2 v_uv;
layout (location = 2) in vec4 v_color; // premultiplied

uniform vec2 viewport_size;

out vec2 uv;
out vec4 color;

void main() {
    uv = v_uv;
    color = v_color;
    gl_Position = vec4(v_pos.x / viewport_size.x * 2.0 - 1.0, 1.0 - v_pos.y / viewport_size.y * 2.0, 0.0, 1.0);
}
