#version 420 core
out vec4 frag_color;

in vec2 tex_coord;
in flat int face_idx;

uniform bool use_face_tex_scaling;
uniform vec3 face_scale;

uniform vec4 color;
uniform mat3 tex_transform;
uniform sampler2D tex;

void main() {
    vec2 scaled_tex_coord;

    if (use_face_tex_scaling) {
        vec2 scale;

        if (face_idx < 2) scale = face_scale.xy;
        else if (face_idx < 4) scale = face_scale.zy;
        else scale = face_scale.xz;

        scaled_tex_coord = (1.0f / 60.0f) * scale * tex_coord;
        scaled_tex_coord.x = 0.5f + scaled_tex_coord.x - scale.x / 2.0f;
    } else {
        scaled_tex_coord = tex_coord;
    }

    vec3 t_tex_coord = tex_transform * vec3(scaled_tex_coord, 1.0f);
    frag_color = texture(tex, t_tex_coord.xy) * color;
}