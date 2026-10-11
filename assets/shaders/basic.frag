#version 450 core
out vec4 frag_color;

in vec2 tex_coord;
in vec3 world_pos;
in vec3 local_pos;
in float view_depth;
in vec4 shadow_coord;
in float vertex_shade;
in vec3 vertex_color;
flat in int face_idx;

// BasicMaterial uv_mode (client.h BasicUvMode): 0 vertex UVs, 1 Krunker cube, 2 Krunker plane
uniform int uv_mode;
uniform vec3 uv_size;
uniform bool uv_flip_sides;
uniform bool uv_flip_top;
uniform bool use_color_gradient;
uniform vec4 color_bottom;
uniform bool use_vertex_color; // three.js vertexColors: color *= the per-vertex colour
uniform float alpha_test;
uniform float ramp_shade_width; // ramp width in world units, 0 = no ambient_1 edge strips

uniform bool unlit;
uniform bool ambient_enabled;
uniform bool material_fog_enabled;
uniform bool use_flat_normal;
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

// Sun shadow (shadow.c). The depth map is bound to texture unit 1 (SHADOW_TEXTURE_UNIT).
uniform bool shadow_enabled;
uniform float shadow_bias;
uniform sampler2D shadow_map;

uniform mat3 tex_transform;
uniform sampler2D tex;

// The "lighting" setting (134.js, 55.js getMat): 0 every material is a MeshBasicMaterial
// (no light, no emissive), 1 Lambert (the material's own choice: light cones stay Basic),
// 2 MeshPhongMaterial for every material (Blinn-Phong specular, shininess 30, specular
// 0x111111) plus the map's POINT_LIGHT objects.
uniform int lighting_mode;
uniform vec3 camera_position;
#define MAX_POINT_LIGHTS 16
uniform int point_light_count;
uniform vec3 point_light_position[MAX_POINT_LIGHTS];
uniform vec3 point_light_color[MAX_POINT_LIGHTS];    // colour * intensity
uniform vec2 point_light_falloff[MAX_POINT_LIGHTS];  // distance, decay

// three.js emissiveMap (texture unit 2): totalEmissiveRadiance = emissive * texel.rgb
uniform bool use_emissive_map;
uniform sampler2D emissive_map;
vec3 emissive_radiance;

// Krunker v3.1.1 world lighting under three.js r121 (non-physical lights, linear output).
// Map materials are "StrippedLambertMaterial" (src/libs/shaders.js), but it does
// Object.assign(shader, new MeshLambertMaterial(...)), which copies type
// "MeshLambertMaterial", so three.js renders them with the stock meshlambert shader:
//   indirect = getAmbientLightIrradiance(ambient * ambientI) * BRDF_Diffuse_Lambert   (PI / PI)
//   direct   = PI * light * lightI * saturate(dot(N, L)) * BRDF_Diffuse_Lambert * shadowMask
//   out      = diffuse * (indirect + direct) + emissive
// (verified on the real client by zeroing each light: the two terms add up per channel).
// The per-object "ab" map flag (ambient_enabled) turns off Krunker's vertex ambient-
// occlusion gradient, not the AmbientLight, so it does not take part here.
// three.js r121 getShadow() for BasicShadowMap: one nearest-texel compare, lit outside
// the shadow camera frustum.
float krunker_shadow_mask() {
    if (!shadow_enabled) return 1.0;
    vec3 coord = shadow_coord.xyz / shadow_coord.w;
    coord.z += shadow_bias;
    bool in_frustum = coord.x >= 0.0 && coord.x <= 1.0 && coord.y >= 0.0 && coord.y <= 1.0 && coord.z <= 1.0;
    if (!in_frustum) return 1.0;
    return step(coord.z, texture(shadow_map, coord.xy).r);
}

// three.js r121 BRDF_Specular_BlinnPhong times the non-physical PI irradiance factor,
// specularColor 0x111111, shininess 30: F_Schlick * 0.25 * (30 / 2 + 1) * dotNH^30.
vec3 krunker_blinn_phong(vec3 light_dir, vec3 normal, vec3 view_dir) {
    vec3 half_dir = normalize(light_dir + view_dir);
    float dot_nh = clamp(dot(normal, half_dir), 0.0, 1.0);
    float dot_lh = clamp(dot(light_dir, half_dir), 0.0, 1.0);
    vec3 specular_color = vec3(17.0 / 255.0);
    float fresnel = exp2((-5.55473 * dot_lh - 6.98316) * dot_lh);
    vec3 f = (1.0 - specular_color) * fresnel + specular_color;
    return f * (0.25 * (30.0 * 0.5 + 1.0) * pow(dot_nh, 30.0));
}

vec3 krunker_lambert(vec3 diffuse, vec3 normal) {
    vec3 light_dir = normalize(light_direction);
    float dot_nl = clamp(dot(normal, light_dir), 0.0, 1.0);
    vec3 indirect = ambient_color.rgb * ambient_intensity;
    vec3 light = light_color.rgb * light_intensity * krunker_shadow_mask(); // getShadow scales the light
    vec3 direct = light * dot_nl;
    vec3 outgoing = diffuse * (indirect + direct) + emissive_radiance;

    if (lighting_mode == 2) {
        vec3 view_dir = normalize(camera_position - world_pos);
        outgoing += light * dot_nl * krunker_blinn_phong(light_dir, normal, view_dir);

        // THREE.PointLight(colr, intensity, distance, decay), non-physical:
        // irradiance = dotNL * color * (distance > 0 && decay > 0 ? saturate(1 - d / distance)^decay : 1)
        for (int i = 0; i < point_light_count && i < MAX_POINT_LIGHTS; i++) {
            vec3 to_light = point_light_position[i] - world_pos;
            float d = length(to_light);
            vec3 l = to_light / max(d, 1e-4);
            vec2 falloff = point_light_falloff[i];
            float factor = falloff.x > 0.0 && falloff.y > 0.0 ? pow(clamp(1.0 - d / falloff.x, 0.0, 1.0), falloff.y) : 1.0;
            vec3 c = point_light_color[i] * factor;
            float nl = clamp(dot(normal, l), 0.0, 1.0);
            outgoing += c * nl * (diffuse + krunker_blinn_phong(l, normal, view_dir));
        }
    }

    return outgoing;
}

// THREE.Fog(color, 1, fogD): fogFactor = smoothstep(near, far, -mvPosition.z).
vec3 krunker_fog(vec3 color) {
    float fog_amount = smoothstep(fog_near, fog_far, view_depth);
    return mix(color, fog_color.rgb, fog_amount);
}

// Krunker 31.js texture mapping. Every face is a THREE.PlaneGeometry(1, 1) rotated into
// place (axis_u/axis_v = its rotated local x/y, center = its translation); w() then scales
// the UVs by the face size / worldUV * scale and re-centres u:
//   u = 0.5 + a * size_u,  v = (b + 0.5) * size_v,  (a, b) = local plane coordinates.
// face is the native cube face (loaders.c cube_vertices: front, back, left, right, top, bottom).
vec2 krunker_face_uv(vec3 p, vec3 center, vec3 axis_u, vec3 axis_v, vec2 size) {
    vec3 d = p - center;
    return vec2(0.5 + dot(d, axis_u) * size.x, (dot(d, axis_v) + 0.5) * size.y);
}

vec2 krunker_cube_uv(int face, vec3 p) {
    vec3 s = uv_size;

    if (face == 0) { // +Z (Krunker face 4)
        if (uv_flip_sides) return krunker_face_uv(p, vec3(0.0, 0.5, 0.5), vec3(0.0, 1.0, 0.0), vec3(-1.0, 0.0, 0.0), s.yx);
        return krunker_face_uv(p, vec3(0.0, 0.5, 0.5), vec3(1.0, 0.0, 0.0), vec3(0.0, 1.0, 0.0), s.xy);
    }
    if (face == 1) { // -Z (Krunker face 5)
        if (uv_flip_sides) return krunker_face_uv(p, vec3(0.0, 0.5, -0.5), vec3(0.0, -1.0, 0.0), vec3(-1.0, 0.0, 0.0), s.yx);
        return krunker_face_uv(p, vec3(0.0, 0.5, -0.5), vec3(-1.0, 0.0, 0.0), vec3(0.0, 1.0, 0.0), s.xy);
    }
    if (face == 2) { // -X (Krunker face 1)
        if (uv_flip_sides) return krunker_face_uv(p, vec3(-0.5, 0.5, 0.0), vec3(0.0, -1.0, 0.0), vec3(0.0, 0.0, 1.0), s.yz);
        return krunker_face_uv(p, vec3(-0.5, 0.5, 0.0), vec3(0.0, 0.0, 1.0), vec3(0.0, 1.0, 0.0), s.zy);
    }
    if (face == 3) { // +X (Krunker face 0)
        if (uv_flip_sides) return krunker_face_uv(p, vec3(0.5, 0.5, 0.0), vec3(0.0, 1.0, 0.0), vec3(0.0, 0.0, 1.0), s.yz);
        return krunker_face_uv(p, vec3(0.5, 0.5, 0.0), vec3(0.0, 0.0, -1.0), vec3(0.0, 1.0, 0.0), s.zy);
    }
    if (face == 4) { // top (Krunker face 2)
        if (uv_flip_top) return krunker_face_uv(p, vec3(0.0, 1.0, 0.0), vec3(0.0, 0.0, -1.0), vec3(-1.0, 0.0, 0.0), s.zx);
        return krunker_face_uv(p, vec3(0.0, 1.0, 0.0), vec3(1.0, 0.0, 0.0), vec3(0.0, 0.0, -1.0), s.xz);
    }
    // bottom (Krunker face 3)
    if (uv_flip_top) return krunker_face_uv(p, vec3(0.0), vec3(0.0, 0.0, -1.0), vec3(1.0, 0.0, 0.0), s.zx);
    return krunker_face_uv(p, vec3(0.0), vec3(1.0, 0.0, 0.0), vec3(0.0, 0.0, 1.0), s.xz);
}

// Cube vertex colours (31.js generateCube): side faces carry color on their local-top
// vertices and color_bottom on the local-bottom ones, the top face is color, the bottom
// face color_bottom.
vec4 krunker_cube_color(int face, vec3 p) {
    if (!use_color_gradient) return color;
    if (face == 4) return color;
    if (face == 5) return color_bottom;

    vec3 axis_v = uv_flip_sides ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
    if (uv_flip_sides && face <= 1) axis_v = vec3(-1.0, 0.0, 0.0);
    float t = clamp(dot(p - vec3(0.0, 0.5, 0.0), axis_v) + 0.5, 0.0, 1.0);
    return mix(color_bottom, color, t);
}

// 94.js addRamp (unless "ab"): two copies of the ramp, ambScale * 2 = 16 wide, along its
// side edges with the "ambient_1" texture (black, alpha rising toward the outer edge).
// The alpha column of ambient_1.png (32 texels, nearest filtered):
const float RAMP_SHADE_ALPHA[32] = float[32](
    0.0, 0.0, 0.0, 0.0, 0.0, 1.0, 1.0, 1.0, 1.0, 2.0, 2.0, 3.0, 4.0, 5.0, 7.0, 8.0,
    10.0, 12.0, 14.0, 17.0, 21.0, 25.0, 29.0, 33.0, 38.0, 43.0, 49.0, 55.0, 61.0, 67.0, 73.0, 80.0
);

float krunker_ramp_strip(float across) {
    float u = (across - (ramp_shade_width * 0.5 - 16.0)) / 16.0;
    if (u < 0.0 || u > 1.0) return 0.0;
    return RAMP_SHADE_ALPHA[clamp(int(u * 32.0), 0, 31)] / 255.0;
}

void main() {
    vec2 scaled_tex_coord;
    vec4 object_color = color;

    if (uv_mode == 1) {
        scaled_tex_coord = krunker_cube_uv(face_idx, local_pos);
        object_color = krunker_cube_color(face_idx, local_pos);
    } else if (uv_mode == 2) {
        scaled_tex_coord = vec2(0.5 + local_pos.x * uv_size.x, (local_pos.y + 0.5) * uv_size.y);
        // noise planes: getColor(colr) on flat vertices, getColor(colr, 0.6) on the bumps
        if (use_color_gradient) object_color = mix(color, color_bottom, vertex_shade);
    } else {
        scaled_tex_coord = tex_coord;
    }
    if (use_vertex_color) object_color.rgb *= vertex_color;

    vec3 t_tex_coord = tex_transform * vec3(scaled_tex_coord, 1.0);
    vec4 texel = texture(tex, t_tex_coord.xy);
    vec4 base_color = texel * object_color;
    if (base_color.a < alpha_test) discard;

    emissive_radiance = emissive.rgb * emissive.a;
    if (use_emissive_map) emissive_radiance *= texture(emissive_map, t_tex_coord.xy).rgb;

    bool basic_material = lighting_mode == 0 || (lighting_mode == 1 && unlit);
    if (lighting_enabled && !basic_material) {
        vec3 normal;

        if (use_flat_normal) {
            normal = normalize(flat_normal);
        } else {
            vec3 dx = dFdx(world_pos);
            vec3 dy = dFdy(world_pos);
            // cross(dFdx, dFdy) always faces the viewer: the outward normal of a front face,
            // and -normal of a back face, which is what three.js DoubleSide lights
            // (vLightBack uses the negated normal). Single-sided meshes are culled.
            normal = normalize(cross(dx, dy));
        }

        base_color.rgb = krunker_lambert(base_color.rgb, normal);
    }

    if (fog_enabled && material_fog_enabled) { // unlit (MeshBasicMaterial light cones) is fogged too
        base_color.rgb = krunker_fog(base_color.rgb);
    }

    if (ramp_shade_width > 0.0) {
        // the black strips are lit to black, then fogged like any lambert surface
        vec3 strip = vec3(0.0);
        if (fog_enabled && material_fog_enabled) strip = krunker_fog(strip);

        float across = local_pos.x * ramp_shade_width;
        base_color.rgb = mix(base_color.rgb, strip, krunker_ramp_strip(across));
        base_color.rgb = mix(base_color.rgb, strip, krunker_ramp_strip(-across));
    }

    frag_color = base_color;
}
