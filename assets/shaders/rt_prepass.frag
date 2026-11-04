#version 450 core
// Ray tracing prepass: the world-space face normal (the same dFdx/dFdy normal basic.frag lights
// with, so the traced shadow and the lit surface agree) and a per-mesh dynamic flag in w
// (RT_INST_DYNAMIC: the denoiser keeps less history there). The second attachment is the
// instance's reflection material (reflect intensity, roughness, metalness; rt_reflect.frag traces
// a reflection ray only where .r > 0).
// The third attachment is the motion vector (phase 6, rt_denoise.frag): .xy = this pixel's uv
// minus the uv the same surface point had last frame (mesh motion and camera motion), .z = its
// view depth (clip w) last frame for the denoiser's disocclusion test, .w = 1 where written.
in vec3 world_pos;
in vec3 vert_normal; // the vertex normal in world space (rt_prepass.vert), zero without the attribute
uniform int use_vertex_normal; // 1: the first-person viewmodel (rt_pass.c rt_viewmodel_frame)
in vec4 cur_clip;
in vec4 prev_clip;
uniform float dynamic;
uniform vec3 material_refl;
layout (location = 0) out vec4 frag_normal;
layout (location = 1) out vec4 frag_material;
layout (location = 2) out vec4 frag_velocity;

void main() {
    vec3 n = normalize(cross(dFdx(world_pos), dFdy(world_pos)));
    if (use_vertex_normal != 0 && dot(vert_normal, vert_normal) > 0.25) {
        vec3 vn = normalize(vert_normal);
        n = dot(vn, n) < 0.0 ? -vn : vn; // facing as the derivative normal does (a double-sided back face)
    }
    frag_normal = vec4(n, dynamic);
    frag_material = vec4(material_refl, 1.0);
    vec2 cur_uv = cur_clip.xy / cur_clip.w * 0.5 + 0.5;
    // behind the previous camera (w <= 0): no usable history, flag the pixel as unreprojectable
    if (prev_clip.w <= 0.0) {
        frag_velocity = vec4(0.0, 0.0, -1.0, 1.0);
    } else {
        vec2 prev_uv = prev_clip.xy / prev_clip.w * 0.5 + 0.5;
        frag_velocity = vec4(cur_uv - prev_uv, prev_clip.w, 1.0);
    }
}
