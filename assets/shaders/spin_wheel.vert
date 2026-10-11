#version 450 core
// Prize wheel canvas + confetti (client/src/ui/spin_wheel.c). v_pos is already in clip space;
// v_canvas is the fragment's position in the wheel canvas's own px (wheel pass only).
layout (location = 0) in vec2 v_pos;
layout (location = 1) in vec2 v_canvas;
layout (location = 2) in vec4 v_color;

out vec2 canvas_pos;
out vec4 color;

void main() {
    canvas_pos = v_canvas;
    color = v_color;
    gl_Position = vec4(v_pos, 0.0, 1.0);
}
