#version 450 core
out vec4 frag_color;

in vec2 tex_coord;
in vec3 world_pos;
in vec3 local_pos;
in float view_depth;
in vec4 shadow_coord;
in float vertex_shade;
in vec3 vertex_color;
in vec4 clip_pos;
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
uniform float shadow_normal_bias; // v10 shadowNB: world units along the normal (0 = v7: the vertex shadow_coord)
uniform mat4 shadow_matrix;       // world -> [0,1] shadow coords (also in basic.vert)
uniform sampler2D shadow_map;

uniform mat3 tex_transform;
uniform sampler2D tex;

// The "lighting" setting (v7.1.4 render.js getMat, restart): 0 every material is a
// MeshBasicMaterial (no light, no emissive), 1 Lambert (the material's own choice: light cones
// stay Basic), 2 MeshPhongMaterial for every material (Blinn-Phong specular, shininess 30,
// specular 0x111111), 3 MeshStandardMaterial for every material (roughness 1, metalness 0:
// Lambert diffuse + GGX specular, the only one that takes rect-area lights). SpriteMaterial
// (sprites) is never lit.
uniform int lighting_mode;
uniform vec3 camera_position;

// Forward light list (basic.c basic_upload_lights, client.h BasicLight), three.js r150 lights:
// lt_type 0 point, 1 spot, 2 directional, 3 rect-area. lt_color is colour * intensity *
// (legacy lights ? PI : 1) (rect: colour * intensity); lt_direction points from the target
// to the light; lt_params = (distance, decay, coneCos, penumbraCos).
#define MAX_LIGHTS 16
uniform int point_light_count;
uniform int lt_type[MAX_LIGHTS];
uniform vec3 lt_position[MAX_LIGHTS];
uniform vec3 lt_direction[MAX_LIGHTS];
uniform vec3 lt_color[MAX_LIGHTS];
uniform vec4 lt_params[MAX_LIGHTS];
uniform vec3 lt_half_width[MAX_LIGHTS];
uniform vec3 lt_half_height[MAX_LIGHTS];

// renderer state of the map environment (map_environment.c, RenderLighting): useLegacyLights =
// !correctLights, toneMapping 0..4 with toneMappingExposure, outputEncoding (1 sRGB)
uniform bool correct_lights;
uniform int tone_mapping;
uniform float tone_mapping_exposure;
uniform int output_encoding;
uniform bool tex_srgb;  // the map texture is sRGBEncoding ("ten" 1): decoded when sampled
uniform bool sprite;    // THREE.SpriteMaterial: never lit
// three.js lightMap (texture unit 3, BASIC_LIGHT_MAP_TEXTURE_UNIT): indirect irradiance +=
// texel * lightMapIntensity (* PI with legacy lights, folded into the uniform by basic.c)
// ocean mirror pass (ocean.c): world-space clip plane, discard dot(n, p) + d < 0
uniform bool clip_enabled;
uniform vec4 clip_plane;
uniform bool use_light_map;
uniform sampler2D light_map;
uniform float light_map_intensity;

// three.js emissiveMap (texture unit 2): totalEmissiveRadiance = emissive * texel.rgb
uniform bool use_emissive_map;
uniform sampler2D emissive_map;
vec3 emissive_radiance;

// WP-F store-item effects (client/src/skin_fx.c). env: the skin's "<tex>_env" equirectangular map
// (texture unit 4) sampled along the world-space reflection. Every v7.1.4 item with `env` is a GLTF
// model (MeshStandardMaterial, metalness 1): its look is the reflection, albedo * env *
// envMapIntensity, plus its emission; the PMREM roughness blur and the Fresnel term are not modelled.
// projection_uv: screen-space texture coordinates (render.js projection: vTexCoords.xy / w / 2 + 0.5).
uniform bool use_env_map;
uniform sampler2D env_map;
uniform float env_map_intensity;
uniform bool projection_uv;

const float PI = 3.141592653589793;
const float RECIPROCAL_PI = 0.3183098861837907;

// Krunker world lighting under three.js r150 (r121 in v3.1.1: the same maths for legacy lights).
// Map materials are "StrippedLambertMaterial" (MeshLambertMaterial), so:
//   irradiance = light colour (* PI with legacy lights) * saturate(dot(N, L)) * attenuation
//   direct     = irradiance * BRDF_Lambert(diffuse) (= diffuse / PI)  [+ specular]
//   indirect   = ambient * ambientI (* PI legacy) * BRDF_Lambert(diffuse)
//   out        = indirect + direct + emissive
// The per-object "ab" map flag (ambient_enabled) turns off Krunker's vertex ambient-occlusion
// gradient, not the AmbientLight, so it does not take part here.
// three.js r150 getShadow() for BasicShadowMap: one nearest-texel compare, lit outside the
// shadow camera frustum.
// three r186 normalBias: shadowWorldPosition = worldPosition + shadowWorldNormal * normalBias.
float krunker_shadow_mask(vec3 normal) {
    if (!shadow_enabled) return 1.0;
    vec4 sc = shadow_normal_bias > 0.0 ? shadow_matrix * vec4(world_pos + normal * shadow_normal_bias, 1.0) : shadow_coord;
    vec3 coord = sc.xyz / sc.w;
    coord.z += shadow_bias;
    bool in_frustum = coord.x >= 0.0 && coord.x <= 1.0 && coord.y >= 0.0 && coord.y <= 1.0 && coord.z <= 1.0;
    if (!in_frustum) return 1.0;
    return step(coord.z, texture(shadow_map, coord.xy).r);
}

vec3 f_schlick(vec3 f0, float f90, float dot_vh) {
    float fresnel = exp2((-5.55473 * dot_vh - 6.98316) * dot_vh);
    return f0 * (1.0 - fresnel) + f90 * fresnel;
}

// three.js r150 BRDF_BlinnPhong (specular 0x111111, shininess 30): F * 0.25 * D_BlinnPhong
vec3 brdf_blinn_phong(vec3 light_dir, vec3 normal, vec3 view_dir) {
    vec3 half_dir = normalize(light_dir + view_dir);
    float dot_nh = clamp(dot(normal, half_dir), 0.0, 1.0);
    float dot_vh = clamp(dot(view_dir, half_dir), 0.0, 1.0);
    vec3 f = f_schlick(vec3(17.0 / 255.0), 1.0, dot_vh);
    return f * (0.25 * RECIPROCAL_PI * (30.0 * 0.5 + 1.0) * pow(dot_nh, 30.0));
}

// three.js r150 BRDF_GGX (MeshStandardMaterial: specularColor 0.04, specularF90 1, roughness 1)
vec3 brdf_ggx(vec3 light_dir, vec3 normal, vec3 view_dir) {
    float alpha = 1.0;
    float a2 = alpha * alpha;
    vec3 half_dir = normalize(light_dir + view_dir);
    float dot_nl = clamp(dot(normal, light_dir), 0.0, 1.0);
    float dot_nv = clamp(dot(normal, view_dir), 0.0, 1.0);
    float dot_nh = clamp(dot(normal, half_dir), 0.0, 1.0);
    float dot_vh = clamp(dot(view_dir, half_dir), 0.0, 1.0);
    vec3 f = f_schlick(vec3(0.04), 1.0, dot_vh);
    float gv = dot_nl * sqrt(a2 + (1.0 - a2) * dot_nv * dot_nv);
    float gl = dot_nv * sqrt(a2 + (1.0 - a2) * dot_nl * dot_nl);
    float v = 0.5 / max(gv + gl, 1e-6);
    float denom = dot_nh * dot_nh * (a2 - 1.0) + 1.0;
    float d = RECIPROCAL_PI * a2 / (denom * denom);
    return f * (v * d);
}

// three.js r150 getDistanceAttenuation
float distance_attenuation(float d, float cutoff, float decay) {
    if (!correct_lights) {
        return cutoff > 0.0 && decay > 0.0 ? pow(clamp(-d / cutoff + 1.0, 0.0, 1.0), decay) : 1.0;
    }
    float falloff = 1.0 / max(pow(d, decay), 0.01);
    if (cutoff > 0.0) {
        float r = d / cutoff;
        float x = clamp(1.0 - r * r * r * r, 0.0, 1.0);
        falloff *= x * x;
    }
    return falloff;
}

// three.js r150 LTC_Evaluate with mInv = identity: the clipped form factor of a rect-area light
// (RE_Direct_RectArea_Physical's diffuse term). The LTC specular needs three's ltc_1/ltc_2
// lookup textures and is not drawn (documented in client.h BasicLight).
vec3 ltc_edge_vector_form_factor(vec3 v1, vec3 v2) {
    float x = dot(v1, v2);
    float y = abs(x);
    float a = 0.8543985 + (0.4965155 + 0.0145206 * y) * y;
    float b = 3.4175940 + (4.1616724 + y) * y;
    float v = a / b;
    float theta_sintheta = x > 0.0 ? v : 0.5 * inversesqrt(max(1.0 - x * x, 1e-7)) - v;
    return cross(v1, v2) * theta_sintheta;
}

float ltc_diffuse(vec3 n, vec3 v, vec3 p, vec3 rect[4]) {
    vec3 v1 = rect[1] - rect[0];
    vec3 v2 = rect[3] - rect[0];
    vec3 light_normal = cross(v1, v2);
    if (dot(light_normal, p - rect[0]) < 0.0) return 0.0;
    vec3 t1 = normalize(v - n * dot(v, n));
    vec3 t2 = -cross(n, t1);
    mat3 m = transpose(mat3(t1, t2, n));
    vec3 c0 = normalize(m * (rect[0] - p));
    vec3 c1 = normalize(m * (rect[1] - p));
    vec3 c2 = normalize(m * (rect[2] - p));
    vec3 c3 = normalize(m * (rect[3] - p));
    vec3 f = ltc_edge_vector_form_factor(c0, c1) + ltc_edge_vector_form_factor(c1, c2) +
             ltc_edge_vector_form_factor(c2, c3) + ltc_edge_vector_form_factor(c3, c0);
    float l = length(f);
    return max((l * l + f.z) / (l + 1.0), 0.0);
}

// One punctual light (irradiance = colour * dotNL) for the material type.
vec3 krunker_direct(vec3 c, vec3 l, vec3 diffuse, vec3 normal, vec3 view_dir) {
    float dot_nl = clamp(dot(normal, l), 0.0, 1.0);
    vec3 irradiance = c * dot_nl;
    vec3 outgoing = irradiance * diffuse * RECIPROCAL_PI;
    if (lighting_mode == 2) outgoing += irradiance * brdf_blinn_phong(l, normal, view_dir);
    else if (lighting_mode == 3) outgoing += irradiance * brdf_ggx(l, normal, view_dir);
    return outgoing;
}

vec3 krunker_lambert(vec3 diffuse, vec3 normal) {
    float light_scale = correct_lights ? 1.0 : PI;
    vec3 view_dir = normalize(camera_position - world_pos);
    // AmbientLight + the map's sun (skyLight, the only shadow caster)
    vec3 irradiance = ambient_color.rgb * ambient_intensity * light_scale;
    if (use_light_map) irradiance += texture(light_map, tex_coord).rgb * light_map_intensity;
    vec3 outgoing = irradiance * diffuse * RECIPROCAL_PI + emissive_radiance;
    vec3 sun = light_color.rgb * light_intensity * light_scale * krunker_shadow_mask(normal); // getShadow scales the light
    outgoing += krunker_direct(sun, normalize(light_direction), diffuse, normal, view_dir);

    for (int i = 0; i < point_light_count && i < MAX_LIGHTS; i++) {
        int type = lt_type[i];
        vec4 params = lt_params[i];
        if (type == 3) {
            vec3 hw = lt_half_width[i], hh = lt_half_height[i], lp = lt_position[i];
            vec3 rect[4];
            rect[0] = lp + hw - hh;
            rect[1] = lp - hw - hh;
            rect[2] = lp - hw + hh;
            rect[3] = lp + hw + hh;
            outgoing += lt_color[i] * diffuse * ltc_diffuse(normal, view_dir, world_pos, rect);
            continue;
        }
        vec3 l;
        vec3 c = lt_color[i];
        if (type == 2) {
            l = lt_direction[i];
        } else {
            vec3 to_light = lt_position[i] - world_pos;
            float d = length(to_light);
            l = to_light / max(d, 1e-4);
            if (type == 1) {
                float angle_cos = dot(l, lt_direction[i]);
                c *= smoothstep(params.z, params.w, angle_cos); // getSpotAttenuation
            }
            c *= distance_attenuation(d, params.x, params.y);
        }
        outgoing += krunker_direct(c, l, diffuse, normal, view_dir);
    }

    return outgoing;
}

// three.js r150 tonemapping_pars_fragment (renderer.toneMapping, toneMappingExposure)
vec3 krunker_tone_map(vec3 color) {
    if (tone_mapping == 1) return tone_mapping_exposure * color;
    if (tone_mapping == 2) {
        color *= tone_mapping_exposure;
        return clamp(color / (vec3(1.0) + color), 0.0, 1.0);
    }
    if (tone_mapping == 3) {
        color *= tone_mapping_exposure;
        color = max(vec3(0.0), color - 0.004);
        return pow((color * (6.2 * color + 0.5)) / (color * (6.2 * color + 1.7) + 0.06), vec3(2.2));
    }
    if (tone_mapping == 4) {
        const mat3 aces_input = mat3(vec3(0.59719, 0.07600, 0.02840), vec3(0.35458, 0.90834, 0.13383), vec3(0.04823, 0.01566, 0.83777));
        const mat3 aces_output = mat3(vec3(1.60475, -0.10208, -0.00327), vec3(-0.53108, 1.10813, -0.07276), vec3(-0.07367, -0.00605, 1.07602));
        color *= tone_mapping_exposure / 0.6;
        color = aces_input * color;
        vec3 a = color * (color + 0.0245786) - 0.000090537;
        vec3 b = color * (0.983729 * color + 0.4329510) + 0.238081;
        color = aces_output * (a / b);
        return clamp(color, 0.0, 1.0);
    }
    return color;
}

// three.js r150 LinearTosRGB / sRGBToLinear
vec3 linear_to_srgb(vec3 v) {
    return mix(pow(v, vec3(0.41666)) * 1.055 - vec3(0.055), v * 12.92, vec3(lessThanEqual(v, vec3(0.0031308))));
}

vec3 srgb_to_linear(vec3 v) {
    return mix(pow(v * 0.9478672986 + vec3(0.0521327014), vec3(2.4)), v * 0.0773993808, vec3(lessThanEqual(v, vec3(0.04045))));
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
    if (clip_enabled && dot(clip_plane.xyz, world_pos) + clip_plane.w < 0.0) discard;
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
    if (projection_uv) scaled_tex_coord = clip_pos.xy / clip_pos.w * 0.5 + 0.5;
    if (use_vertex_color) object_color.rgb *= vertex_color;

    vec3 t_tex_coord = tex_transform * vec3(scaled_tex_coord, 1.0);
    vec4 texel = texture(tex, t_tex_coord.xy);
    if (tex_srgb) texel.rgb = srgb_to_linear(texel.rgb);
    vec4 base_color = texel * object_color;
    if (base_color.a < alpha_test) discard;

    emissive_radiance = emissive.rgb * emissive.a;
    if (use_emissive_map) emissive_radiance *= texture(emissive_map, t_tex_coord.xy).rgb;

    vec3 albedo = base_color.rgb;
    bool basic_material = lighting_mode == 0 || (lighting_mode == 1 && unlit) || sprite;
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

    if (use_env_map) {
        vec3 n = use_flat_normal ? normalize(flat_normal) : normalize(cross(dFdx(world_pos), dFdy(world_pos)));
        vec3 r = reflect(normalize(world_pos - camera_position), n);
        vec2 env_uv = vec2(atan(r.z, r.x) * 0.15915494309189535 + 0.5, asin(clamp(r.y, -1.0, 1.0)) * RECIPROCAL_PI + 0.5);
        base_color.rgb = albedo * texture(env_map, env_uv).rgb * env_map_intensity + emissive_radiance;
    }

    // three.js r150 fragment tail: tonemapping_fragment, encodings_fragment, then fog_fragment
    base_color.rgb = krunker_tone_map(base_color.rgb);
    if (output_encoding == 1) base_color.rgb = linear_to_srgb(base_color.rgb);

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
