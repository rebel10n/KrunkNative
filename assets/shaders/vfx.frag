#version 450 core
out vec4 frag_color;

in vec2 tex_coord;
in vec4 color;
in float view_depth;

uniform bool use_texture;
uniform sampler2D tex; // texture unit 0 (vfx.c VFX_TEXTURE_UNIT)

uniform bool fog_enabled;
uniform vec4 fog_color;
uniform float fog_near;
uniform float fog_far;

void main() {
    vec4 base_color = color;
    if (use_texture) base_color *= texture(tex, tex_coord);

    // Same fog curve as basic.frag so effects fade with the map geometry.
    if (fog_enabled) {
        float fog_amount = smoothstep(fog_near, fog_far, view_depth);
        base_color.rgb = mix(base_color.rgb, fog_color.rgb, fog_amount);
    }

    frag_color = base_color;
}
