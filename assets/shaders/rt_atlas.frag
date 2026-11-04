#version 450 core
// Ray tracing texture atlas (client/src/rt/rt_atlas.c): copies the bound world texture into its
// atlas cell (the viewport); the cell is smaller than most textures, so the sampler's mip filter
// does the downscale.
uniform sampler2D src;
in vec2 uv;
out vec4 frag_out;

void main() {
    frag_out = texture(src, uv);
}
