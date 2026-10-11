#version 450 core
// Depth is written by the fixed pipeline; the colour output is unused (no colour attachment).
out vec4 frag_color;

void main() {
    frag_color = vec4(1.0);
}
