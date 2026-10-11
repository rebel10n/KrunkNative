#version 450 core
// Scene render target composite (client/src/render_target.c): bilinear upscale of
// the resolution-scaled scene plus Krunker's in-game CSS filter (bundleFilters(0):
// saturate(saturationn * 100%) then hue-rotate(hue * 3.6deg)), each clamped like a
// CSS filter step. Matrices are uploaded row-major with transpose = GL_TRUE.
in vec2 uv;
out vec4 frag_color;

uniform sampler2D scene; // texture unit 0 (RT_SCENE_TEXTURE_UNIT)
uniform bool filtered;
uniform mat3 saturate_matrix;
uniform mat3 hue_matrix;

void main() {
    vec3 color = texture(scene, uv).rgb;
    if (filtered) {
        color = clamp(saturate_matrix * color, 0.0, 1.0);
        color = clamp(hue_matrix * color, 0.0, 1.0);
    }
    frag_color = vec4(color, 1.0);
}
