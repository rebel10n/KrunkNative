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
// !correctLights, toneMapping 0..4 with toneMappingExposure. v10.0.3 has no output encoding: Krunker turns
// ColorManagement off (deob-10.0.3.js:230030), so RenderOutputNode's workingToColorSpace (deob:63877-63882) is the
// no-op ColorSpaceNode (deob:63117-63136) and outputEncoding 1 (sRGB) renders like 0.
uniform bool correct_lights;
uniform int tone_mapping;
uniform float tone_mapping_exposure;
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

// three.js emissiveMap (texture unit 2): totalEmissiveRadiance = emissive * texel.rgb; emissive_srgb: the map is
// SRGBColorSpace (a GLB asset with env, texEncode 1: BasicMaterial.gltf_srgb_maps / emissive_srgb)
uniform bool use_emissive_map;
uniform sampler2D emissive_map;
uniform bool emissive_srgb;
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

// Per-copy paint (skin_fx_apply_paint, basic.c): Krunker v10.0.3's paint material node (deob-10.0.3.js:108706-108758,
// `iïïìììï(material, {color, intensity, maskTex, shader})`). The paint is applied to the albedo before the lighting:
//   colorNode = vec4(mix(base, paintOut, clamp(paintIntensity, 0, 1) * maskTex.r), base.a)
//   paintOut  = overlay(base, paintColor)                       colour paint (no shader)
//             = overlay(base, shader(uv, time, base))           shader 0 Spectrum, 1 Synth (deob:105898-105953)
//             = shader(uv, time, base).diffuse                  "raw" shaders 2 Gold, 3 Silver, 4 Prism, 5 Codefall
//               (deob:105954-105995; their .glow is added to the emissive, times the intensity, deob:108752-108755)
//   overlay(base, paint) (deob:108672-108679): lum = pow(dot(base, luma), 0.55); hi = min(paint * 3 + 0.02, 1);
//             lum < 0.5 ? paint * lum * 2 : mix(paint, hi, lum * 2 - 1)
// A shader number the list does not know is no paint (v10: the shader is null and intensity 0, deob:233828-233833).
// paint_shader -1 = the colour paint. paint_time is the TSL `time` node (seconds). The mask (paintMask, unit 7) scales
// the intensity by its red; the metal shaders read melee_190_env.png (deob:105842, unit 8) along the reflection.
uniform bool paint_enabled;
uniform vec3 paint_color;
uniform float paint_intensity;
uniform int paint_shader;
uniform float paint_time;
uniform bool paint_mask_enabled;
uniform sampler2D paint_mask;
uniform sampler2D paint_env_map;
vec3 paint_glow; // the raw shaders' emissive (deob:108752), added to emissive_radiance in main

// glTF PBR maps (gltf.c, BasicMaterial.metal_rough_texture / normal_texture): metallicRoughness (texture
// unit 5: G = roughness, B = metalness, times the factors) and the tangent-space normal map (unit 6,
// normalScale). A material with the metal-roughness map is shaded as three.js MeshStandardMaterial (pbr_*
// below, in any lit mode); the normal map perturbs the lit normal of every lit material that has it.
uniform bool use_metal_rough_map;
uniform sampler2D metal_rough_map;
uniform float metalness_factor;
uniform float roughness_factor;
uniform bool use_normal_map;
uniform sampler2D normal_map;
uniform float normal_scale;

// v10.0.3 GLB environment reflections (client/include/env_map.h): a GLB material whose glTF extras carry "env"
// reflects render.getEnvMap() at envMapIntensity scene_env_intensity (deob-10.0.3.js:233665-233669). scene_env_map
// (unit 9) is its PMREM filtered on the CPU: column = PMREM level 0..8 (roughness level / 8), row = the direction's
// elevation (asin(y) / PI + 0.5); scene_env_dfg (unit 10) is three r18x's DFG_LUT (u roughness, v dotNV).
uniform bool use_scene_env;
uniform sampler2D scene_env_map;
uniform sampler2D scene_env_dfg;
uniform float scene_env_intensity;
bool pbr_active;  // the material's PBR shading (set in main, lit materials only)
float pbr_alpha;  // GGX alpha = roughness^2
vec3 pbr_f0;      // specular F0 = mix(0.04, albedo, metalness)

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

// three.js r150 BRDF_GGX with its alpha (roughness^2) and specular F0 (MeshStandardMaterial's specularColor)
vec3 brdf_ggx_alpha(vec3 light_dir, vec3 normal, vec3 view_dir, float alpha, vec3 f0) {
    float a2 = alpha * alpha;
    vec3 half_dir = normalize(light_dir + view_dir);
    float dot_nl = clamp(dot(normal, light_dir), 0.0, 1.0);
    float dot_nv = clamp(dot(normal, view_dir), 0.0, 1.0);
    float dot_nh = clamp(dot(normal, half_dir), 0.0, 1.0);
    float dot_vh = clamp(dot(view_dir, half_dir), 0.0, 1.0);
    vec3 f = f_schlick(f0, 1.0, dot_vh);
    float gv = dot_nl * sqrt(a2 + (1.0 - a2) * dot_nv * dot_nv);
    float gl = dot_nv * sqrt(a2 + (1.0 - a2) * dot_nl * dot_nl);
    float v = 0.5 / max(gv + gl, 1e-6);
    float denom = dot_nh * dot_nh * (a2 - 1.0) + 1.0;
    float d = RECIPROCAL_PI * a2 / (denom * denom);
    return f * (v * d);
}

// three.js r150 BRDF_GGX (MeshStandardMaterial: specularColor 0.04, specularF90 1, roughness 1)
vec3 brdf_ggx(vec3 light_dir, vec3 normal, vec3 view_dir) {
    return brdf_ggx_alpha(light_dir, normal, view_dir, 1.0, vec3(0.04));
}

// three.js r150 perturbNormal2Arb (a MeshStandardMaterial normalMap without tangents): the tangent frame from the
// screen-space derivatives of the world position and of the texture coordinates. World space stands in for
// three's view space (the same frame, rotated), so the lighting below is unchanged.
vec3 krunker_perturb_normal(vec3 n, vec2 uv, vec3 map_n) {
    vec3 q0 = dFdx(world_pos);
    vec3 q1 = dFdy(world_pos);
    vec2 st0 = dFdx(uv);
    vec2 st1 = dFdy(uv);
    vec3 q1perp = cross(q1, n);
    vec3 q0perp = cross(n, q0);
    vec3 t = q1perp * st0.x + q0perp * st1.x;
    vec3 b = q1perp * st0.y + q0perp * st1.y;
    float det = max(dot(t, t), dot(b, b));
    float scale = det == 0.0 ? 0.0 : inversesqrt(det);
    return normalize(t * (map_n.x * scale) + b * (map_n.y * scale) + n * map_n.z);
}

// The normal map's tangent-space normal (normalScale on x / y), for krunker_perturb_normal
vec3 krunker_normal_map_sample(vec2 uv) {
    vec3 map_n = texture(normal_map, uv).xyz * 2.0 - 1.0;
    map_n.xy *= normal_scale;
    return map_n;
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
    if (pbr_active) outgoing += irradiance * brdf_ggx_alpha(l, normal, view_dir, pbr_alpha, pbr_f0);
    else if (lighting_mode == 2) outgoing += irradiance * brdf_blinn_phong(l, normal, view_dir);
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

// textureCubeUV on the PMREM (deob-10.0.3.js:70449-70467): mip = clamp(roughnessToMip(roughness), -2, CUBEUV_MAX_MIP
// 6), the two PMREM levels lodMax - floor(mip) and the next one blended by fract(mip). Level = 6 - mip, so the
// table's linear filter across its 9 columns is that blend. roughnessToMip: deob:70426-70448.
vec3 krunker_scene_env_sample(vec3 dir, float roughness) {
    float mip;
    if (roughness >= 0.8) mip = (1.0 - roughness) * (-1.0 + 2.0) / (1.0 - 0.8) - 2.0;
    else if (roughness >= 0.4) mip = (0.8 - roughness) * (2.0 + 1.0) / (0.8 - 0.4) - 1.0;
    else if (roughness >= 0.305) mip = (0.4 - roughness) * (3.0 - 2.0) / (0.4 - 0.305) + 2.0;
    else if (roughness >= 0.21) mip = (0.305 - roughness) * (4.0 - 3.0) / (0.305 - 0.21) + 3.0;
    else mip = -2.0 * log2(1.16 * roughness);
    float level = 6.0 - clamp(mip, -2.0, 6.0);
    vec2 uv = vec2((level + 0.5) / 9.0, asin(clamp(dir.y, -1.0, 1.0)) * RECIPROCAL_PI + 0.5);
    return texture(scene_env_map, uv).rgb;
}

// PhysicalLightingModel.computeMultiscattering (deob:70081-70091): FssEss and Fms * Ems for one specular F0, F90 1
void krunker_multiscatter(vec3 f0, vec2 dfg, out vec3 single_scatter, out vec3 multi_scatter) {
    vec3 fss_ess = f0 * dfg.x + dfg.y;
    float ems = 1.0 - (dfg.x + dfg.y);
    vec3 f_avg = f0 + (1.0 - f0) * 0.047619;
    vec3 fms = fss_ess * f_avg / (1.0 - ems * f_avg);
    single_scatter = fss_ess;
    multi_scatter = fms * ems;
}

// The environment's light on a MeshStandardMaterial (three r18x): EnvironmentNode (deob:71245-71275) adds
//   radiance      = pmrem(normalize(mix(reflect(-V, N), N, roughness^4)), roughness) * envMapIntensity
//   iblIrradiance = pmrem(N, 1) * PI * envMapIntensity
// and PhysicalLightingModel.indirectSpecular (deob:70268-70318) lights the material with them:
//   specular += radiance * mix(single_d, single_m, metal) + mix(multi_d, multi_m, metal) * iblIrradiance / PI
//   diffuse  += albedo * (1 - metal) * (1 - (single_d + multi_d)) * iblIrradiance / PI
// with the dielectric layer's F0 0.04 and the metallic layer's F0 the albedo (setupSpecular, deob:71366-71371), the
// roughness max(r, 0.0525) (getRoughness, deob:69209; the geometry roughness of KN's flat normals is 0) and the DFG
// term from the DFG_LUT (deob:69407-69420).
vec3 krunker_scene_env(vec3 albedo, float metalness, float roughness, vec3 n) {
    float r = clamp(max(roughness, 0.0525), 0.0, 1.0);
    vec3 v = normalize(camera_position - world_pos);
    vec3 dir = normalize(mix(reflect(-v, n), n, r * r * r * r));
    vec3 radiance = krunker_scene_env_sample(dir, r) * scene_env_intensity;
    vec3 irradiance = krunker_scene_env_sample(n, 1.0) * scene_env_intensity; // iblIrradiance / PI
    vec2 dfg = texture(scene_env_dfg, vec2(r, clamp(dot(n, v), 0.0, 1.0))).rg;
    vec3 single_d, multi_d, single_m, multi_m;
    krunker_multiscatter(vec3(0.04), dfg, single_d, multi_d);
    krunker_multiscatter(albedo, dfg, single_m, multi_m);
    vec3 specular = radiance * mix(single_d, single_m, metalness) + mix(multi_d, multi_m, metalness) * irradiance;
    vec3 diffuse = albedo * (1.0 - metalness) * (1.0 - (single_d + multi_d)) * irradiance;
    return specular + diffuse;
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

// ---- the paint (the uniforms above): v10.0.3's paint node, ported from its TSL graphs
const vec3 PAINT_LUMA = vec3(0.299, 0.587, 0.114);

// deob:108672-108679: the paint over the albedo (every non-raw paint)
vec3 paint_overlay(vec3 base, vec3 paint) {
    float lum = pow(dot(base, PAINT_LUMA), 0.55);
    vec3 hi = min(paint * 3.0 + 0.02, vec3(1.0));
    return lum < 0.5 ? paint * (lum * 2.0) : mix(paint, hi, lum * 2.0 - 1.0);
}

// deob:105898-105914 Spectrum (its GLSL twin at deob:108892)
vec3 paint_spectrum(float t) {
    const float TAU = 6.28318530718;
    const float PHI = 1.61803398875;
    float hue = fract(t / 4.0);
    float sat = 0.55 + 0.45 * (0.5 + 0.5 * sin(TAU * t / (4.0 * PHI)));
    float val = 0.65 + 0.35 * (0.5 + 0.5 * sin(TAU * t / (4.0 * PHI * PHI)));
    vec3 p = abs(fract(hue + vec3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0);
    vec3 rgb = clamp(p - 1.0, 0.0, 1.0);
    return ((rgb - 1.0) * sat + 1.0) * val;
}

// deob:105915-105953 Synth (its GLSL twin at deob:108897)
vec3 paint_neon_palette(float t) {
    return vec3(0.5) + vec3(0.5) * cos(6.28318 * (vec3(0.85, 1.05, 1.25) * t + vec3(0.15, 0.45, 0.82)));
}
vec3 paint_synth(vec2 uv, float t) {
    vec2 uv0 = (uv - 0.5) * 2.0;
    float r0 = length(uv0);
    float atten = 1.0 / (1.0 + r0 * r0 * 1.4);
    const float CA = 0.9004;
    const float SA = 0.4350;
    mat2 R = mat2(CA, -SA, SA, CA);
    vec2 p = uv0;
    vec3 col = vec3(0.0);
    for (int k = 0; k < 4; k++) {
        float i = float(k);
        p = R * p;
        float ph = i * 7.0;
        vec2 drift = vec2(0.25 * sin(t * 0.13 + ph) + 0.12 * sin(t * 0.41 + ph * 1.5),
                          0.25 * cos(t * 0.17 + ph) + 0.12 * cos(t * 0.37 + ph * 1.5));
        p = fract(p * 1.7 + drift) - 0.5;
        float d = length(p) * atten;
        vec3 layer = paint_neon_palette(r0 * 0.7 + i * 0.55 + t * 0.3);
        float rings = abs(cos(d * 11.0 - t * 1.3));
        col += layer * (0.012 / pow(rings + 0.001, 0.9));
    }
    return col;
}

float paint_hash(vec2 v) { // deob:105865 (iìííììî) and the metal's inline hashes
    return fract(sin(dot(v, vec2(127.1, 311.7))) * 43758.5453);
}

// deob:105835-105864 (iiîîïîí): Gold / Silver / Prism (tint, shadow, rim at deob:105954-105975). N, V in world space:
// NdotV = max(dot(normalView, positionViewDirection), 0); r = reflectVector; the env map along equirectUV(r).
void paint_metal(int kind, vec3 base, vec2 uv, float t, vec3 N, vec3 V, out vec3 diffuse, out vec3 glow) {
    float ndv = max(dot(N, V), 0.0);
    float base_fac = mix(0.55, 1.0, smoothstep(0.02, 0.55, pow(dot(base, PAINT_LUMA), 0.6)));
    vec3 r = reflect(-V, N);
    vec2 env_uv = vec2(atan(r.z, r.x) * 0.15915494309189535 + 0.5, asin(clamp(r.y, -1.0, 1.0)) * RECIPROCAL_PI + 0.5);
    float env_l = dot(srgb_to_linear(texture(paint_env_map, env_uv).rgb), vec3(0.333));
    vec3 tint, shadow, rim;
    if (kind == 2) {
        tint = vec3(1.0, 0.77, 0.3);
        shadow = vec3(0.24, 0.11, 0.02);
        rim = vec3(1.0, 0.9, 0.6);
    } else if (kind == 3) {
        tint = vec3(0.93, 0.95, 0.98);
        shadow = vec3(0.07, 0.08, 0.1);
        rim = vec3(1.0);
    } else { // prism: the tint turns with the view and the time
        float h = ndv * 1.3 + r.y * 0.4 + t * 0.04;
        tint = cos((h + vec3(0.0, 0.33, 0.67)) * 6.2831) * 0.5 + 0.5;
        shadow = tint * 0.1;
        rim = vec3(1.0);
    }
    vec3 metal = mix(shadow, tint, smoothstep(0.0, 0.45, env_l)) * (env_l * 1.9 + 0.18);
    float fres = pow(1.0 - ndv, 3.0);
    vec2 guv = uv * 140.0;
    vec2 cell = floor(guv);
    float h1 = paint_hash(cell);
    float h2 = fract(sin(dot(cell, vec2(269.5, 183.3))) * 43758.5453);
    vec2 d = fract(guv) - (vec2(h1, h2) * 0.5 + 0.25);
    float spot = 1.0 - smoothstep(0.0, 0.16, length(d));
    float twinkle = sin(t * (h2 * 2.0 + 1.2) + h1 * 6.2831) * 0.5 + 0.5;
    float spark = spot * pow(twinkle, 3.0) * smoothstep(0.45, 0.9, env_l) * step(0.86, h1);
    glow = (metal * 0.55 + rim * fres * 0.35 + mix(tint, vec3(1.0), 0.7) * spark * 0.9) * base_fac;
    diffuse = metal * 0.5 * base_fac;
}

// deob:105868-105897 (iîiîìîî): Codefall
void paint_codefall(vec3 base, vec2 uv, float t, out vec3 diffuse, out vec3 glow) {
    float row = floor(uv.y * 14.0);
    float tick = floor(t * 9.0);
    float gate = step(0.82, paint_hash(vec2(floor(t * 1.5), 11.0)));
    float act = step(0.78, paint_hash(vec2(row, tick))) * gate;
    float shift = (paint_hash(vec2(tick, row + 3.0)) - 0.5) * 0.15 * act;
    vec2 p = vec2(uv.x + shift, uv.y) * 52.0;
    vec2 cell = floor(p);
    float ch = paint_hash(vec2(cell.x, 1.7));
    float fall = fract(ch * 9.0 + t * (ch * 0.6 + 0.35) + cell.y / 52.0);
    vec2 f = fract(p);
    vec2 sub = floor(f * vec2(3.0, 5.0));
    float tk = floor(t * (ch * 4.0 + 2.0));
    float glyph = step(0.45, paint_hash(cell * vec2(13.0, 7.0) + sub * vec2(3.1, 1.9) + tk));
    float border = step(0.12, f.x) * step(f.x, 0.88) * step(0.08, f.y) * step(f.y, 0.92);
    float mask = glyph * border;
    vec3 col2 = mix(vec3(1.0, 0.1, 0.55), vec3(0.2, 1.0, 1.0), paint_hash(vec2(row, tick + 5.0)));
    vec3 code = (mix(vec3(0.05, 1.0, 0.2), col2, act) * (pow(fall, 5.0) + act * 0.5) + vec3(0.75, 1.0, 0.8) * smoothstep(0.93, 1.0, fall)) * mask;
    float base_fac = mix(0.55, 1.0, smoothstep(0.02, 0.55, pow(dot(base, PAINT_LUMA), 0.6)));
    vec3 dark = vec3(0.015, 0.035, 0.02) * (base_fac * 1.5);
    diffuse = dark + code * 0.2;
    glow = code * 1.2 * base_fac;
}

// deob:108706-108758: the painted albedo; sets paint_glow
vec3 paint_apply(vec3 base, vec2 uv, vec2 mask_uv) {
    vec3 painted = base;
    paint_glow = vec3(0.0);
    float t = paint_time;
    if (paint_shader < 0) {
        painted = paint_overlay(base, paint_color);
    } else if (paint_shader == 0) {
        painted = paint_overlay(base, paint_spectrum(t));
    } else if (paint_shader == 1) {
        painted = paint_overlay(base, paint_synth(uv, t));
    } else if (paint_shader <= 5) {
        vec3 N = use_flat_normal ? normalize(flat_normal) : normalize(cross(dFdx(world_pos), dFdy(world_pos)));
        vec3 V = normalize(camera_position - world_pos);
        if (paint_shader == 5) paint_codefall(base, uv, t, painted, paint_glow);
        else paint_metal(paint_shader, base, uv, t, N, V, painted, paint_glow);
    } else {
        return base; // an unknown shader: unpainted
    }
    float k = clamp(paint_intensity, 0.0, 1.0);
    if (paint_mask_enabled) k *= texture(paint_mask, mask_uv).r;
    paint_glow *= k;
    return mix(base, painted, k);
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
    paint_glow = vec3(0.0);
    // the shaders and the mask read the geometry uv (TSL uv(), deob:108727 / the paintMaskMap texture node)
    if (paint_enabled) base_color.rgb = paint_apply(base_color.rgb, scaled_tex_coord, scaled_tex_coord);

    emissive_radiance = emissive.rgb * emissive.a;
    if (use_emissive_map) {
        vec3 emissive_texel = texture(emissive_map, t_tex_coord.xy).rgb;
        emissive_radiance *= emissive_srgb ? srgb_to_linear(emissive_texel) : emissive_texel;
    }
    emissive_radiance += paint_glow; // deob:108752-108755: emissiveNode = (emissiveNode || 0) + glow * intensity

    vec3 albedo = base_color.rgb;
    bool basic_material = lighting_mode == 0 || (lighting_mode == 1 && unlit) || sprite;
    pbr_active = false;
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

        vec3 diffuse = albedo;
        if (use_normal_map) normal = krunker_perturb_normal(normal, t_tex_coord.xy, krunker_normal_map_sample(t_tex_coord.xy));
        float metalness = 0.0, roughness = 1.0;
        if (use_metal_rough_map || use_scene_env) {
            // MeshStandardMaterial: diffuse = albedo * (1 - metalness), specular F0 = mix(0.04, albedo, metalness),
            // alpha = roughness^2; roughness is clamped as three.js does (0.04 minimum). A GLB env material is
            // shaded so with its factors alone too (v10's GLB materials are MeshStandardMaterial).
            vec3 mr = use_metal_rough_map ? texture(metal_rough_map, t_tex_coord.xy).rgb : vec3(1.0);
            roughness = clamp(roughness_factor * mr.g, 0.04, 1.0);
            metalness = clamp(metalness_factor * mr.b, 0.0, 1.0);
            pbr_active = true;
            pbr_alpha = roughness * roughness;
            pbr_f0 = mix(vec3(0.04), albedo, metalness);
            diffuse = albedo * (1.0 - metalness);
        }

        base_color.rgb = krunker_lambert(diffuse, normal);
        if (use_scene_env) base_color.rgb += krunker_scene_env(albedo, metalness, roughness, normal);
    }

    if (use_env_map) {
        vec3 n = use_flat_normal ? normalize(flat_normal) : normalize(cross(dFdx(world_pos), dFdy(world_pos)));
        // the reflection follows the normal map too (three.js: normal_fragment_maps before envmap_fragment)
        if (use_normal_map) n = krunker_perturb_normal(n, t_tex_coord.xy, krunker_normal_map_sample(t_tex_coord.xy));
        vec3 r = reflect(normalize(world_pos - camera_position), n);
        vec2 env_uv = vec2(atan(r.z, r.x) * 0.15915494309189535 + 0.5, asin(clamp(r.y, -1.0, 1.0)) * RECIPROCAL_PI + 0.5);
        base_color.rgb = albedo * texture(env_map, env_uv).rgb * env_map_intensity + emissive_radiance;
    }

    // v10.0.3 fragment tail (three r18x): the material's setupOutput fogs (deob-10.0.3.js:68245-68253), then the
    // renderer's output (getOutput, deob:67960-67961) tone maps it; no output encoding (ColorManagement off, see
    // the renderer state uniforms above).
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

    base_color.rgb = krunker_tone_map(base_color.rgb);
    frag_color = base_color;
}
