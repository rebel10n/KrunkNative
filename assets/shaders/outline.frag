#version 450 core
// KrunkNative Object Manager selection outline: one flat colour (client/src/om_outline.c).
uniform vec4 color;

out vec4 frag_color;

void main() {
    frag_color = color;
}
