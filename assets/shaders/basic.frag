#version 450 core
out vec4 frag_color;

in vec2 tex_coord;
in flat int face_idx;

uniform bool is_ramp;
uniform bool is_ladder;

uniform bool use_face_tex_scaling;
uniform float world_uv_scale;
uniform vec3 face_scale;

uniform vec4 color;
uniform vec4 emissive;

uniform mat3 tex_transform;
uniform sampler2D tex;

void main() {
    vec2 scaled_tex_coord;

    if (use_face_tex_scaling) {
        vec2 scale;

        if (is_ramp) {
            switch (face_idx) {
                case 0:
                    scale = vec2(face_scale.x, sqrt(face_scale.z*face_scale.z + face_scale.y*face_scale.y));
                case 1:
                    scale = face_scale.xz;
                    break;
                case 2:
                    scale = face_scale.zy;
                    break;
                case 3:
                    scale = face_scale.xy;
                    break;
            }
        } else if (is_ladder) {
            if (face_idx < 2) scale = face_scale.xy;
            else if (face_idx < 4) scale = face_scale.zy;
            else if (face_idx < 6) scale = face_scale.xz;
            else if (face_idx < 8) scale = face_scale.xy;
            else if (face_idx < 10) scale = face_scale.zy;
            else scale = face_scale.xz;
        } else {
            if (face_idx < 2) scale = face_scale.xy;
            else if (face_idx < 4) scale = face_scale.zy;
            else scale = face_scale.xz;
        }

        scaled_tex_coord = (1.0f / world_uv_scale) * scale * tex_coord;
        scaled_tex_coord.x = 0.5f + scaled_tex_coord.x - scale.x * 0.5f;
    } else {
        scaled_tex_coord = tex_coord;
    }

    vec3 t_tex_coord = tex_transform * vec3(scaled_tex_coord, 1.0f);
    frag_color = texture(tex, t_tex_coord.xy) * mix(color, emissive, 0.0f);
}