#version 450 core
out vec4 frag_color;

in vec2 tex_coord;
in vec3 world_pos;
in float view_depth;
in flat int face_idx;

uniform bool is_ramp;
uniform bool is_ladder;

uniform bool use_face_tex_scaling;
uniform bool unlit;
uniform bool ambient_enabled;
uniform bool material_fog_enabled;
uniform bool use_flat_normal;
uniform float world_uv_scale;
uniform vec3 face_scale;
uniform vec3 flat_normal;

uniform vec4 color;
uniform vec4 emissive;
uniform vec4 ambient_color;
uniform vec4 light_color;
uniform vec4 fog_color;
uniform vec3 light_direction;
uniform float ambient_intensity;
uniform float light_intensity;
uniform float fog_near;
uniform float fog_far;
uniform bool lighting_enabled;
uniform bool fog_enabled;

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
                    break;
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
    vec4 texel = texture(tex, t_tex_coord.xy);
    vec4 base_color = texel * color;

    if (lighting_enabled && !unlit) {
        vec3 normal;

        if (use_flat_normal) {
            normal = normalize(flat_normal);
        } else {
            vec3 dx = dFdx(world_pos);
            vec3 dy = dFdy(world_pos);
            normal = normalize(cross(dx, dy));
            if (!gl_FrontFacing) normal = -normal;
        }

        float lambert = max(dot(normal, normalize(light_direction)), 0.0f);
        vec3 ambient = ambient_enabled ? ambient_color.rgb * ambient_intensity : vec3(0.0f);
        vec3 direct = light_color.rgb * light_intensity * lambert;
        vec3 lit = base_color.rgb * (ambient + direct);

        base_color.rgb = max(lit, emissive.rgb * emissive.a);
    }

    if (fog_enabled && material_fog_enabled && !unlit) {
        float fog_amount = smoothstep(fog_near, fog_far, view_depth);
        base_color.rgb = mix(base_color.rgb, fog_color.rgb, fog_amount);
    }

    frag_color = base_color;
}
