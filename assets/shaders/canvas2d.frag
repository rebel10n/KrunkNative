#version 450 core
// Canvas2D: premultiplied output, blended with (ONE, ONE_MINUS_SRC_ALPHA).
//   mode 0: solid colour
//   mode 1: glyph atlas coverage (R8) * colour
//   mode 2: premultiplied RGBA texture * colour (images: colour = globalAlpha; the layer composite: 1)
in vec2 uv;
in vec4 color;

uniform int mode;
uniform sampler2D tex; // texture unit 0

out vec4 frag_color;

void main() {
    if (mode == 1) {
        frag_color = color * texture(tex, uv).r;
    } else if (mode == 2) {
        frag_color = texture(tex, uv) * color;
    } else {
        frag_color = color;
    }
}
