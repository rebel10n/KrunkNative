#version 450 core
// Compile-time variants (materials/basic.c basic_material_select_program; shader.c injects the defines after the
// version line): KN_RT 0 is Krunker's own path with every Box Office ray-tracing block compiled out (the program of
// every draw outside the traced world / viewmodel passes, and of everything at rtMode 0); KN_RT 1 keeps them, and
// KN_RT_CINE 0 drops the Cinematic-only ones. The gate uniforms become constants, so the dead branches and the
// uniforms only they read are removed by the compiler; the live arithmetic is the same.
#ifndef KN_RT
#define KN_RT 1
#endif
#ifndef KN_RT_CINE
#define KN_RT_CINE 1
#endif
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
// Box Office ray tracing (client/include/rt.h, rt_pass.c): rt_effects bits RT_EFFECT_* (0 = Krunker's
// own path, the default), rt_result the denoised trace of this frame at RT resolution (R sun
// visibility, G ambient occlusion) read at this fragment: gl_FragCoord * rt_inv_viewport.
// RT_EFFECT_AO (bit 2) scales the indirect light only (the ambient / light-map irradiance of
// krunker_lambert and the scene-env irradiance): the direct sun, the lights and emissive stay.
#if KN_RT
uniform int rt_effects;
#else
const int rt_effects = 0;
#endif
uniform vec2 rt_inv_viewport;
// Box Office sky-dome ambient (RT_EFFECT_SKY_AMBIENT, bit 64; basic.c): krunker_ambient_irradiance
#if KN_RT
uniform int sky_ambient_mode;    // 0 Krunker's constant AmbientLight, 1 the PMREM table along the normal, 2 sky_color / fog blend
uniform float sky_ambient_scale; // luminance match for an upward normal (basic.c basic_sky_ambient_scale)
#else
const int sky_ambient_mode = 0;
uniform float sky_ambient_scale;
#endif
uniform vec3 sky_color;          // the map's sky colour (mode 2, maps without a sky dome)
uniform sampler2D rt_result;
uniform sampler2D rt_refl; // RT_EFFECT_REFLECTIONS (4): the traced reflection radiance of this pixel (rt_reflect.frag), same lookup
// RT_EFFECT_GI (32): the denoised one-bounce radiance of this pixel (rt_gi.frag, rt.h): one cosine-weighted
// sample's radiance L estimates the bounce irradiance G = PI * L. krunker_gi_irradiance composes it with the
// ambient irradiance (Krunker's constant one, or the sky-dome one of bit 64): the ambient's luminance is the
// floor, G tints it (rt_gi_tint) and raises it where G is brighter (formula in rt.h).
uniform sampler2D rt_gi;
uniform float rt_gi_tint;
// RT_EFFECT_MATERIALS (128, rt.h "material classes"): the material's Box Office class factors (rt_materials.c).
// With rt_mat_enabled the PBR branch below runs with them (GGX specular, F0 = mix(0.04, albedo, metalness)),
// whatever the lighting setting, and a class with rt_mat_reflect > 0 adds the traced reflection (rt_refl)
// with three's DFG weighting. A part mask on metal_rough_map multiplies them. Without the bit: unchanged.
uniform bool rt_mat_enabled;
uniform float rt_mat_metalness;
uniform float rt_mat_roughness;
uniform float rt_mat_reflect;
uniform float rt_mat_f0; // a metal's minimum F0 (rt.h RtMaterialClass.f0): black-painted gun textures still read as metal
// RT_EFFECT_LOCAL_LIGHTS (bit 8, rt.h phase 5): rt_local holds the traced visibility of up to 8 point /
// spot lights of the list below, two per channel at 12 bits (rt.h packing), nearest-sampled at
// rt_local_size; rt_local_index maps a light slot to its traced index (-1: untraced, visibility 1).
#define RT_LOCAL_MAX 8
uniform sampler2D rt_local;
uniform vec2 rt_local_size;
uniform int rt_local_index[16]; // MAX_LIGHTS

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
#include "rt_sky.glsl" // Box Office rtSky: psky_fog_color (krunker_fog's aerial perspective); psky_on 0 = Krunker's fog

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

// v10.0.6 wear (loadMesh's `wear`: the wearMap node, deob-10.0.6.js:95692-95722, applied at :204868 after the overlay,
// before the paint): f(L, mid, k, max) = clamp((s(L) - s(0)) / (s(1) - s(0)), 0, 1) * max, s(x) = 1 / (1 + exp(-k (x - mid)));
// rgb = mix(rgb, scratches, min(map.a * f(L, 0.5, 1, 1), 1)), then dirt by g (0.33, 5.5, 0.9), grime by b (0.2, 7, 1),
// rust by r (0.5, 8, 1). The map is data (no sRGB), on unit 13; the geometry uv.
uniform bool wear_enabled;
uniform float wear_level;
uniform sampler2D wear_map;

// v10.0.3 decal (loadMesh's `decal` node, deob-10.0.3.js:232213-232243; the Turf Wars banners' clan logo,
// client/src/wars_banners.c): in the box |local_pos - decal_position| <= decal_half_scale (the geometry's own
// space), the albedo becomes mix(albedo, decal.rgb, decal.a), decal sampled at (local.xy - position.xy + half.xy) /
// (2 half.xy). Over the paint, before the lighting. The map is on unit 12.
uniform bool decal_enabled;
uniform sampler2D decal_map;
uniform vec3 decal_position;
uniform vec3 decal_half_scale;

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
// v10.0.3 user-asset companion maps (client/src/player_class_model.c, applyAssetCompanionMaps deob-10.0.3.js:231711):
// the "s" specularMap (unit 11), whose red is MeshPhongMaterial's specularStrength on the Blinn-Phong direct specular,
// and maps_flip_v: the normal / specular maps sample at (u, 1 - v), three's flipY false over an OBJ's flipped base map
uniform bool use_specular_map;
uniform sampler2D specular_map;
uniform bool maps_flip_v;
float specular_strength = 1.0;

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
vec3 krunker_rt_gi() {
    if ((rt_effects & 32) == 0) return vec3(0.0); // RT_EFFECT_GI
    return texture(rt_gi, gl_FragCoord.xy * rt_inv_viewport).rgb;
}

// The indirect irradiance under RT_EFFECT_GI (rt.h): Krunker's maps are lit for their ambient, so one bounce never
// darkens below it; it tints it and brightens it where the bounce exceeds it.
vec3 krunker_gi_irradiance(vec3 ambient) {
    const vec3 LUM = vec3(0.2126, 0.7152, 0.0722);
    vec3 g = krunker_rt_gi() * PI;
    float la = dot(ambient, LUM), lg = dot(g, LUM);
    if (la <= 1e-5) return ambient + g; // a map without ambient: the bounce is all the indirect light
    vec3 t = vec3(1.0);
    if (lg > 1e-5) t = mix(vec3(1.0), clamp((g / lg) / max(ambient / la, vec3(1e-3)), 0.0, 4.0), rt_gi_tint);
    vec3 tinted = ambient * t;
    tinted *= la / max(dot(tinted, LUM), 1e-5); // the tint alone keeps the luminance
    return tinted * max(1.0, lg / la);
}

// Box Office bevelled cube edges (rt.h RT_EFFECT_BEVEL, rt_pass.c rt_apply_mesh_uniforms): a shading-only round
// bevel on Krunker's map boxes. rt_bevel_faces is the cube's face mask (map_face_mask bits 0 +x, 1 -x, 2 +y, 3 -y,
// 4 +z, 5 -z; 0 = not a bevelled cube). The unit cube spans x, z in [-0.5, 0.5], y in [0, 1] (loaders.c), and the
// map transform is translate * rotate * scale, so the world size of local axis i is |transform[i].xyz| and a local
// normal maps to the world as transform * (n / scale^2) (= R * S^-1 n, the inverse transpose). The face is the local
// axis the geometric normal is closest to; within `width` world units of each of its four edges the normal tilts
// toward the neighbour face (linearly up to the 45-degree bisector at the edge), unless the map masked that face out
// (the box touches another object there: no visible edge). The bevel is a rotation of the geometric normal, added to
// the incoming (possibly normal-mapped) normal so surface detail survives.
uniform mat4 transform; // also in basic.vert
uniform int rt_bevel_faces;
uniform float rt_bevel_width;
uniform float rt_bevel_face_fraction;
// Cinematic preset surface detail (basic.c, rt.h rt_cinematic): rt_cine 1 for a classed material's draw while the
// preset is on; rt_cine_detail its class's RT_DETAIL_* kind; rt_cine_untextured 1 for a flat-colour material (the
// detail and the colour variation are full strength there, a third on textured ones); rt_cine_cavity 1 when the
// normal map is the generated one, whose alpha holds a cavity term (rt_bump.frag).
#if KN_RT && KN_RT_CINE
uniform bool rt_cine;
#else
const bool rt_cine = false;
#endif
uniform int rt_cine_detail;
uniform bool rt_cine_untextured;
uniform bool rt_cine_cavity;

vec3 krunker_rt_bevel(vec3 n) {
    if ((rt_effects & 1024) == 0 || rt_bevel_faces == 0) return n; // RT_EFFECT_BEVEL
    vec3 g = normalize(cross(dFdx(world_pos), dFdy(world_pos)));
    mat3 m = mat3(transform);
    vec3 scale = vec3(length(m[0]), length(m[1]), length(m[2]));
    if (min(scale.x, min(scale.y, scale.z)) < 1e-4) return n;
    vec3 gloc = transpose(m) * g; // the local direction of the geometric normal (M^T n_w is parallel to n_l for TRS)
    vec3 ag = abs(gloc);
    int f = ag.x >= ag.y && ag.x >= ag.z ? 0 : (ag.y >= ag.z ? 1 : 2);
    vec3 d = local_pos - vec3(0.0, 0.5, 0.0); // from the cube's centre, components in [-0.5, 0.5]
    float facing = gloc[f] * d[f] >= 0.0 ? 1.0 : -1.0; // -1: an inside (back) face of a double-sided / BackSide box
    vec3 nl = vec3(0.0);
    nl[f] = d[f] >= 0.0 ? 1.0 : -1.0;
    int ua = f == 0 ? 1 : 0;
    int va = f == 2 ? 1 : 2;
    float width = rt_cine ? min(max(rt_bevel_width, 1.0), 0.3 * min(scale[ua], scale[va])) // Cinematic: wider
                          : min(rt_bevel_width, rt_bevel_face_fraction * min(scale[ua], scale[va]));
    if (width < 1e-3) return n;
    vec3 bent = nl;
    for (int k = 0; k < 2; k++) {
        int a = k == 0 ? ua : va;
        float side = d[a] >= 0.0 ? 1.0 : -1.0;
        int bit = a * 2 + (side > 0.0 ? 0 : 1); // map_face_mask: +x 0, -x 1, +y 2, -y 3, +z 4, -z 5
        if ((rt_bevel_faces & (1 << bit)) == 0) continue;
        float dist = (0.5 - abs(d[a])) * scale[a]; // world units to that edge
        float t = clamp(1.0 - dist / width, 0.0, 1.0);
        bent[a] += side * t;
    }
    if (bent == nl) return n;
    vec3 b = normalize(m * (bent / (scale * scale))) * facing;
    vec3 g_out = normalize(m * (nl / (scale * scale))) * facing; // the flat face normal on the same footing as b
    return normalize(n + (b - g_out));
}

// ---- Cinematic surface detail (rt_cine). All of it works in world units on the face of a map cube (the same face
// frame as krunker_rt_bevel), so a field of stacked blocks reads as one continuous material rather than one fill.

float rt_hash12(vec2 p) { // Dave Hoskins' hash without sine (precision-stable for map coordinates of a few thousand)
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

float rt_vnoise(vec2 p) { // value noise, smooth, 0..1
    vec2 i = floor(p);
    vec2 f = p - i;
    vec2 u = f * f * (3.0 - 2.0 * f);
    float a = rt_hash12(i), b = rt_hash12(i + vec2(1.0, 0.0));
    float c = rt_hash12(i + vec2(0.0, 1.0)), d = rt_hash12(i + vec2(1.0, 1.0));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

// An octave of `period` world units fades out before it would alias: its features under ~4 pixels vanish.
float rt_octave_fade(float period, float footprint) {
    return 1.0 - smoothstep(0.12 * period, 0.35 * period, footprint);
}

// rt_vnoise at three points at once (the height and its two finite-difference neighbours): a point in the same
// lattice cell as the first reuses its four corner hashes. Each component is exactly rt_vnoise of its own point.
vec3 rt_vnoise3(vec2 q0, vec2 q1, vec2 q2) {
    vec2 i0 = floor(q0);
    float a = rt_hash12(i0), b = rt_hash12(i0 + vec2(1.0, 0.0));
    float c = rt_hash12(i0 + vec2(0.0, 1.0)), d = rt_hash12(i0 + vec2(1.0, 1.0));
    vec2 f = q0 - i0;
    vec2 u = f * f * (3.0 - 2.0 * f);
    vec3 r;
    r.x = mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
    vec2 i1 = floor(q1);
    if (i1 == i0) {
        f = q1 - i1; u = f * f * (3.0 - 2.0 * f);
        r.y = mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
    } else {
        r.y = rt_vnoise(q1);
    }
    vec2 i2 = floor(q2);
    if (i2 == i0) {
        f = q2 - i2; u = f * f * (3.0 - 2.0 * f);
        r.z = mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
    } else {
        r.z = rt_vnoise(q2);
    }
    return r;
}

// The material's height at the face points p0 (the fragment) and p1, p2 (its finite-difference neighbours; world
// units, .x along the face's longer side): 0.5 = level. `fade` holds the octaves' rt_octave_fade weights; an octave
// whose weight is 0 adds exactly 0, so it is not evaluated (most of a frame is far enough that the fine octaves have
// faded out). Each component is the same arithmetic as one point evaluated on its own.
vec3 rt_detail_height3(vec2 p0, vec2 p1, vec2 p2, vec3 fade, int kind) {
    vec3 h = vec3(0.5);
    if (kind == 1) { // stone: faint broad mottling, slab-scale pitting, a fine grain. The broad (7-unit) octave is
                     // kept weak: at 0.55 it read as room-sized dirt stains on Kanji's floors under Cinematic.
        if (fade.x > 0.0) h += (rt_vnoise3(p0 / 7.0, p1 / 7.0, p2 / 7.0) - 0.5) * 0.12 * fade.x;
        if (fade.y > 0.0) h += (rt_vnoise3(p0 / 1.7 + 11.0, p1 / 1.7 + 11.0, p2 / 1.7 + 11.0) - 0.5) * 0.30 * fade.y;
        if (fade.z > 0.0) h += (rt_vnoise3(p0 * 2.5 + 23.0, p1 * 2.5 + 23.0, p2 * 2.5 + 23.0) - 0.5) * 0.22 * fade.z;
        return h;
    }
    if (kind == 2) { // metal: brushed streaks along the long side (long, thin cells)
        if (fade.x <= 0.0) return h;
        vec3 streak = rt_vnoise3(vec2(p0.x * 0.12, p0.y * 9.0), vec2(p1.x * 0.12, p1.y * 9.0), vec2(p2.x * 0.12, p2.y * 9.0)) * 0.7
                    + rt_vnoise3(vec2(p0.x * 0.4 + 5.0, p0.y * 3.0), vec2(p1.x * 0.4 + 5.0, p1.y * 3.0), vec2(p2.x * 0.4 + 5.0, p2.y * 3.0)) * 0.3;
        return 0.5 + (streak - 0.5) * fade.x;
    }
    if (kind == 3) { // wood: grain lines along the long side, warped
        if (fade.x > 0.0) {
            vec3 warp = rt_vnoise3(p0 * vec2(0.06, 0.35), p1 * vec2(0.06, 0.35), p2 * vec2(0.06, 0.35)) * 3.0;
            vec3 rings = 0.5 + 0.5 * sin((vec3(p0.y, p1.y, p2.y) + warp) * 5.0);
            h += (rings - 0.5) * 0.8 * fade.x;
        }
        if (fade.y > 0.0) h += (rt_vnoise3(vec2(p0.x * 0.3, p0.y * 12.0), vec2(p1.x * 0.3, p1.y * 12.0), vec2(p2.x * 0.3, p2.y * 12.0)) - 0.5) * 0.3 * fade.y;
        return h;
    }
    return h;
}

// Albedo variation, a bumped normal and a roughness offset for a map cube's face (rt_cine; rt_bevel_faces != 0 marks
// a map cube). Returns the roughness offset (metal streaks vary the gloss).
float krunker_rt_detail(inout vec3 albedo, inout vec3 normal) {
    if (!rt_cine || rt_bevel_faces == 0) return 0.0;
    mat3 m = mat3(transform);
    vec3 scale = vec3(length(m[0]), length(m[1]), length(m[2]));
    if (min(scale.x, min(scale.y, scale.z)) < 1e-4) return 0.0;
    float strength = rt_cine_untextured ? 1.0 : 0.35;

    // per-object colour variation: +/-4 % brightness hashed by the object's position (a flat field of equal
    // blocks stops reading as one fill)
    float obj = rt_hash12(transform[3].xz * 0.37 + transform[3].y * 1.31);
    albedo *= 1.0 + (obj - 0.5) * 0.08 * strength;

    if (rt_cine_detail == 0) return 0.0;
    vec3 g = normalize(cross(dFdx(world_pos), dFdy(world_pos)));
    vec3 gloc = transpose(m) * g;
    vec3 ag = abs(gloc);
    int f = ag.x >= ag.y && ag.x >= ag.z ? 0 : (ag.y >= ag.z ? 1 : 2);
    int ua = f == 0 ? 1 : 0;
    int va = f == 2 ? 1 : 2;
    if (scale[va] > scale[ua]) { int t = ua; ua = va; va = t; } // .x runs along the face's longer side
    vec3 tu = normalize(m[ua]), tv = normalize(m[va]);
    vec2 p = vec2(dot(world_pos, tu), dot(world_pos, tv));
    float footprint = max(length(fwidth(p)), 1e-4);
    int kind = rt_cine_detail;

    // the octaves' fade weights, once (rt_detail_height skips an octave that has faded out entirely)
    vec3 fade = kind == 1 ? vec3(rt_octave_fade(7.0, footprint), rt_octave_fade(1.7, footprint), rt_octave_fade(0.4, footprint))
              : kind == 2 ? vec3(rt_octave_fade(0.11, footprint), 0.0, 0.0)
                          : vec3(rt_octave_fade(1.25, footprint), rt_octave_fade(0.08, footprint), 0.0);
    float e = 0.15;
    vec3 hs = rt_detail_height3(p, p + vec2(e, 0.0), p + vec2(0.0, e), fade, kind);
    float h = hs.x, hx = hs.y, hy = hs.z;
    // slope gain per class; stone 0.45 bent the normal of slab-scale noise enough that grazing (moon / sun) light
    // drew diagonal streaks across Kanji's flat floors: stone reads through its albedo grain, the relief stays faint
    float amp = (kind == 1 ? 0.1 : kind == 2 ? 0.05 : 0.18) * strength;
    vec3 grad = (tu * (hx - h) + tv * (hy - h)) / e;
    normal = normalize(normal - grad * amp);

    float tint = kind == 1 ? 0.22 : kind == 2 ? 0.10 : 0.30; // albedo contrast per class (stone 0.32 read as dirt)
    albedo *= 1.0 + (h - 0.5) * tint * strength;
    return kind == 2 ? (h - 0.5) * 0.25 * strength : 0.0;
}

// Contact creases: a cube face darkens within ~0.6 units of an edge where the map masked the neighbour face (the
// box meets another object there: a concave corner the bevel leaves sharp).
float krunker_rt_crease() {
    if (!rt_cine || rt_bevel_faces == 0) return 1.0;
    mat3 m = mat3(transform);
    vec3 scale = vec3(length(m[0]), length(m[1]), length(m[2]));
    if (min(scale.x, min(scale.y, scale.z)) < 1e-4) return 1.0;
    vec3 gloc = transpose(m) * normalize(cross(dFdx(world_pos), dFdy(world_pos)));
    vec3 ag = abs(gloc);
    int f = ag.x >= ag.y && ag.x >= ag.z ? 0 : (ag.y >= ag.z ? 1 : 2);
    vec3 d = local_pos - vec3(0.0, 0.5, 0.0);
    int ua = f == 0 ? 1 : 0;
    int va = f == 2 ? 1 : 2;
    float occ = 1.0;
    for (int k = 0; k < 2; k++) {
        int a = k == 0 ? ua : va;
        float side = d[a] >= 0.0 ? 1.0 : -1.0;
        int bit = a * 2 + (side > 0.0 ? 0 : 1);
        if ((rt_bevel_faces & (1 << bit)) != 0) continue; // an open edge: the bevel's, not a crease
        float dist = (0.5 - abs(d[a])) * scale[a];
        occ *= 1.0 - 0.22 * (1.0 - smoothstep(0.0, 0.6, dist));
    }
    return occ;
}

// The generated relief's cavity (rt_bump.frag alpha): seams and mortar below their neighbourhood darken.
float krunker_rt_cavity(vec2 uv) {
    if (!rt_cine || !rt_cine_cavity || !use_normal_map) return 1.0;
    float c = texture(normal_map, maps_flip_v ? vec2(uv.x, 1.0 - uv.y) : uv).a;
    return 1.0 - 0.45 * clamp((0.5 - c) * 2.0, 0.0, 1.0);
}

float krunker_rt_ao() {
    if ((rt_effects & 2) == 0) return 1.0; // RT_EFFECT_AO
    return texture(rt_result, gl_FragCoord.xy * rt_inv_viewport).g;
}

// One traced light's visibility out of a packed rt_local texel (rt.h: channel = hi * 4096 + lo, 12 bits each)
float krunker_rt_local_unpack(vec4 texel, int traced) {
    float channel = traced < 2 ? texel.x : traced < 4 ? texel.y : traced < 6 ? texel.z : texel.w;
    float hi = floor(channel / 4096.0);
    return ((traced & 1) == 0 ? hi : channel - hi * 4096.0) / 4095.0;
}

// The traced shadow of light slot `slot` at this fragment (1 without RT_EFFECT_LOCAL_LIGHTS or for a
// light that was not traced). The packed target cannot be filtered by the sampler, so the four
// nearest texels are unpacked and blended by hand (the same bilinear footprint rt_result gets).
// The four texels are fetched once per fragment (the first traced light of the loop) and unpacked per light.
bool rt_local_fetched = false;
vec4 rt_local_t00, rt_local_t10, rt_local_t01, rt_local_t11;
vec2 rt_local_f;

float krunker_rt_local(int slot) {
    if ((rt_effects & 8) == 0) return 1.0; // RT_EFFECT_LOCAL_LIGHTS
    int traced = rt_local_index[slot];
    if (traced < 0) return 1.0;
    if (!rt_local_fetched) {
        rt_local_fetched = true;
        vec2 at = gl_FragCoord.xy * rt_inv_viewport * rt_local_size - 0.5;
        vec2 base = floor(at);
        rt_local_f = at - base;
        ivec2 i0 = ivec2(base);
        ivec2 size = ivec2(rt_local_size) - 1;
        rt_local_t00 = texelFetch(rt_local, clamp(i0, ivec2(0), size), 0);
        rt_local_t10 = texelFetch(rt_local, clamp(i0 + ivec2(1, 0), ivec2(0), size), 0);
        rt_local_t01 = texelFetch(rt_local, clamp(i0 + ivec2(0, 1), ivec2(0), size), 0);
        rt_local_t11 = texelFetch(rt_local, clamp(i0 + ivec2(1, 1), ivec2(0), size), 0);
    }
    float v00 = krunker_rt_local_unpack(rt_local_t00, traced);
    float v10 = krunker_rt_local_unpack(rt_local_t10, traced);
    float v01 = krunker_rt_local_unpack(rt_local_t01, traced);
    float v11 = krunker_rt_local_unpack(rt_local_t11, traced);
    return mix(mix(v00, v10, rt_local_f.x), mix(v01, v11, rt_local_f.x), rt_local_f.y);
}

float krunker_shadow_mask(vec3 normal) {
    if (!shadow_enabled) return 1.0;
    if ((rt_effects & 1) != 0) return texture(rt_result, gl_FragCoord.xy * rt_inv_viewport).r; // RT_EFFECT_SHADOWS
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
    vec3 map_n = texture(normal_map, maps_flip_v ? vec2(uv.x, 1.0 - uv.y) : uv).xyz * 2.0 - 1.0;
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
    else if (lighting_mode == 2) outgoing += irradiance * brdf_blinn_phong(l, normal, view_dir) * specular_strength;
    else if (lighting_mode == 3) outgoing += irradiance * brdf_ggx(l, normal, view_dir);
    return outgoing;
}

vec3 krunker_scene_env_sample(vec3 dir, float roughness); // below
void krunker_multiscatter(vec3 f0, vec2 dfg, out vec3 single_scatter, out vec3 multi_scatter); // below

// The indirect (ambient) irradiance at a surface with `normal`: Krunker's AmbientLight, ambient * ambientI (* PI
// with legacy lights), the same from every direction. With the Box Office sky-dome ambient (rt_effects bit 64,
// sky_ambient_mode > 0) it is the sky's cosine-weighted radiance about the normal instead, times
// sky_ambient_scale so an upward normal keeps the AmbientLight's luminance (basic.c): up faces see the dome's
// top colour, walls the horizon / fog colour, undersides the darkened fog (env_map.c paints the gradient's
// bottom as fog * 0.25, which stands in for a ground tint). Mode 1 reads the PMREM table's roughest level
// (env_map.h; a cosine-lobe filtered sky), mode 2 (no dome) blends sky_color above the horizon and fog below,
// down to fog * 0.25, the same shape as the painted gradient. Light maps are added by the caller as before.
vec3 krunker_ambient_irradiance(vec3 normal) {
    float light_scale = correct_lights ? 1.0 : PI;
    vec3 flat_ambient = ambient_color.rgb * ambient_intensity * light_scale;
    if ((rt_effects & 64) == 0 || sky_ambient_mode == 0) return flat_ambient;
    vec3 sky;
    if (sky_ambient_mode == 1) {
        sky = krunker_scene_env_sample(normal, 1.0);
    } else {
        float t = clamp(normal.y, -1.0, 1.0);
        sky = t >= 0.0 ? mix(fog_color.rgb, sky_color, t) : mix(fog_color.rgb, fog_color.rgb * 0.25, -t);
    }
    return sky * sky_ambient_scale * light_scale;
}

// Box Office material classes (RT_EFFECT_MATERIALS): the indirect specular of a classed material, three's
// indirectSpecular weighting (DFG, F0 = mix(0.04, albedo, metalness); krunker_scene_env) over
//   - the traced reflection of this pixel (rt_reflect.frag) when the class reflects and the pass ran, else
//   - the sky: the PMREM table along the reflection vector at the material's roughness (the sky-ambient path's
//     table on unit 9), or the sky_color / fog blend without a dome, or Krunker's flat ambient without the sky
//     ambient at all. Scaled like krunker_ambient_irradiance (sky_ambient_scale * light_scale, / PI since the
//     ambient there is an irradiance and this is a radiance), so a rough metal keeps the energy a Lambert
//     surface of the same albedo would have had instead of going black: diffuse lost (1 - metalness) and the
//     specular lobe gets it back from the same sky. The irradiance term of krunker_scene_env is left out:
//     the ambient / GI of krunker_lambert already carry the diffuse part.
// Box Office specular occlusion of the sky reflection (rt.h "specular occlusion"): the sky radiance is scaled by
// a sky visibility and the occluded part becomes the radiance of the shaded surroundings, half the flat
// ambient (an albedo-0.5 surface lit by it) or the pixel's GI bounce, so a metal under a roof reflects warm
// dark walls the way the traced reflection does, not the open sky and not black.
//   AO traced (rt_effects & 2): Lagarde's so = saturate(pow(NdotV + ao, exp2(-16 roughness - 1)) - 1 + ao)
//   RT_EFFECT_SKY_VIS (8192, rtMode 1, world): rt_result.g is a traced sky visibility
//   RT_EFFECT_SKY_PROBE (16384, the viewmodel): the camera's ambient cube of sky visibilities (rt_pass.c
//   sky_probe_update), weighted by the squared components of the reflection direction
uniform vec3 rt_sky_probe_pos; // visibility along +x, +y, +z
uniform vec3 rt_sky_probe_neg; // along -x, -y, -z
vec3 krunker_rt_spec_occlusion(vec3 sky_radiance, vec3 n, vec3 v, float r) {
    float so = 1.0;
    if ((rt_effects & 2) != 0) {
        float ao = texture(rt_result, gl_FragCoord.xy * rt_inv_viewport).g;
        float ndv = clamp(dot(n, v), 0.0, 1.0);
        so = clamp(pow(ndv + ao, exp2(-16.0 * r - 1.0)) - 1.0 + ao, 0.0, 1.0);
    } else if ((rt_effects & 8192) != 0) {
        so = texture(rt_result, gl_FragCoord.xy * rt_inv_viewport).g;
    } else if ((rt_effects & 16384) != 0) {
        vec3 d = reflect(-v, n);
        vec3 w = d * d; // sums to 1
        vec3 vis = mix(rt_sky_probe_neg, rt_sky_probe_pos, step(0.0, d));
        so = clamp(dot(w, vis), 0.0, 1.0);
    }
    if (so >= 1.0) return sky_radiance;
    float light_scale = correct_lights ? 1.0 : PI;
    vec3 occluded = (rt_effects & 32) != 0 ? krunker_rt_gi() : ambient_color.rgb * ambient_intensity * light_scale * RECIPROCAL_PI * 0.5;
    return mix(occluded, sky_radiance, so);
}

vec3 krunker_rt_mat_specular(vec3 albedo, float metalness, float roughness, vec3 n) {
    float r = clamp(max(roughness, 0.0525), 0.0, 1.0);
    vec3 v = normalize(camera_position - world_pos);
    vec2 dfg = texture(scene_env_dfg, vec2(r, clamp(dot(n, v), 0.0, 1.0))).rg;
    vec3 single_d, multi_d, single_m, multi_m;
    krunker_multiscatter(vec3(0.04), dfg, single_d, multi_d);
    krunker_multiscatter(albedo, dfg, single_m, multi_m);
    vec3 weight = mix(single_d, single_m, metalness);
    // The sky estimate is always computed: it is the whole answer without a traced reflection, and the fill
    // where the traced one has no data (see below).
    vec3 radiance;
    {
        float light_scale = correct_lights ? 1.0 : PI;
        vec3 dir = normalize(mix(reflect(-v, n), n, r * r * r * r));
        vec3 sky;
        if ((rt_effects & 64) == 0 || sky_ambient_mode == 0) {
            sky = ambient_color.rgb * ambient_intensity; // no sky ambient: the flat AmbientLight from every direction
            radiance = sky * light_scale * RECIPROCAL_PI;
        } else {
            if (sky_ambient_mode == 1) {
                sky = krunker_scene_env_sample(dir, r);
            } else {
                float t = clamp(dir.y, -1.0, 1.0);
                sky = t >= 0.0 ? mix(fog_color.rgb, sky_color, t) : mix(fog_color.rgb, fog_color.rgb * 0.25, -t);
            }
            radiance = sky * sky_ambient_scale * light_scale * RECIPROCAL_PI;
        }
    }
    if (rt_mat_reflect > 0.0 && (rt_effects & 4) != 0) {
        // The traced reflection. Its alpha is >= 1 where the reflect pass traced the pixel (the denoiser keeps the
        // history length there) and 0 where it skipped it (no reflective material in its prepass, outside the
        // viewmodel's scissor, a disocclusion before the history fills): there the sky estimate stands in instead
        // of black. Bilinear filtering blends the two at the edges of the traced region.
        vec4 traced = texture(rt_refl, gl_FragCoord.xy * rt_inv_viewport);
        radiance = mix(krunker_rt_spec_occlusion(radiance, n, v, r), traced.rgb, clamp(traced.a, 0.0, 1.0));
    } else {
        radiance = krunker_rt_spec_occlusion(radiance, n, v, r);
    }
    // The class's `reflect` gates whether a traced ray is spent on the pixel; it does not dim the incoming light
    // (the DFG weight already says how much the surface reflects). Above 1 it is an exposure boost of the shiny
    // classes (chrome, gold, glass, water), applied alike to the traced and the sky radiance so rtMode 1 and 3 agree.
    return radiance * weight * max(rt_mat_reflect, 1.0);
}

// Box Office RT water and glass (rt.h "water and glass", RT_EFFECT_WATER on a transparent water / glass class draw):
// a fresnel blend of a reflection and the refracted opaque frame behind the surface, composited by the shader itself
// (main's tail) since the refracted texel is not the one under the fragment.
uniform int rt_water_kind;            // 1 water, 2 glass / window
uniform bool rt_water_copy;           // rt_water_scene holds this frame's opaque world
uniform bool rt_water_traced;         // rt_refl holds the surface's traced reflection (rtMode 3)
uniform sampler2D rt_water_scene;     // the opaque frame (rt_pass.c rt_water_copy_scene)
uniform sampler2D rt_water_depth;     // the world prepass depth (RT resolution, rt_inv_viewport lookups)
uniform vec2 rt_water_inv_size;       // 1 / the copy's size
uniform vec2 rt_water_depth_ab;       // the camera projection's A, B: view depth = B / (ndc z + A)
uniform float rt_water_refract;       // screen offset per unit of wave-normal deviation (fraction of the view height)
uniform float rt_water_absorb_depth;  // water depth that mostly turns the view through it into the water colour
uniform mat4 camera_world_inverse;    // (also in basic.vert) the view rotation for the refraction offset
bool rt_water_out = false;
float rt_water_w = 1.0;
vec2 rt_water_uv = vec2(0.0);

float krunker_rt_water_view_depth(vec2 uv) {
    float ndc = texture(rt_water_depth, uv).r * 2.0 - 1.0;
    return rt_water_depth_ab.y / (ndc + rt_water_depth_ab.x);
}

// The sky radiance along `dir` at roughness r, as krunker_rt_mat_specular estimates it
vec3 krunker_rt_water_sky(vec3 dir, float r) {
    float light_scale = correct_lights ? 1.0 : PI;
    if ((rt_effects & 64) == 0 || sky_ambient_mode == 0) return ambient_color.rgb * ambient_intensity * light_scale * RECIPROCAL_PI;
    vec3 sky;
    if (sky_ambient_mode == 1) {
        sky = krunker_scene_env_sample(dir, r);
    } else {
        float t = clamp(dir.y, -1.0, 1.0);
        sky = t >= 0.0 ? mix(fog_color.rgb, sky_color, t) : mix(fog_color.rgb, fog_color.rgb * 0.25, -t);
    }
    return sky * sky_ambient_scale * light_scale * RECIPROCAL_PI;
}

// `base`: the lit surface (rgb) and its authored opacity (a). Leaves the reflection + water body colour in base.rgb and
// the coverage w in base.a; main's tail fogs / tone maps it and blends it over the refracted copy.
void krunker_rt_water(inout vec4 base, vec3 n, float roughness) {
    bool water = rt_water_kind == 1;
    vec3 v = normalize(camera_position - world_pos);
    float ndv = clamp(dot(n, v), 0.0, 1.0);
    vec2 depth_uv = gl_FragCoord.xy * rt_inv_viewport;
    // refraction: the wave normal's tilt from a flat surface, in view space (glass panes are flat: no offset)
    vec2 offset = vec2(0.0);
    if (water) {
        mat3 view_rotation = mat3(camera_world_inverse);
        vec3 tilt = view_rotation * n - view_rotation * vec3(0.0, n.y >= 0.0 ? 1.0 : -1.0, 0.0);
        offset = tilt.xy * rt_water_refract * vec2(rt_inv_viewport.x / rt_inv_viewport.y, 1.0);
    }
    float behind = krunker_rt_water_view_depth(depth_uv + offset);
    if (behind < view_depth) { // the offset lands on something in front of the water: no refraction there
        offset = vec2(0.0);
        behind = krunker_rt_water_view_depth(depth_uv);
    }
    float thickness = max(behind - view_depth, 0.0);
    float body = water ? max(base.a, 1.0 - exp(-thickness / rt_water_absorb_depth)) : base.a;
    float f0 = water ? 0.02 : 0.04;
    float fresnel = f0 + (1.0 - f0) * pow(1.0 - ndv, 5.0);
    float r = clamp(max(roughness, 0.0525), 0.0, 1.0);
    vec3 refl = krunker_rt_spec_occlusion(krunker_rt_water_sky(reflect(-v, n), r), n, v, r);
    if (rt_water_traced) {
        vec4 traced = texture(rt_refl, depth_uv + offset * 0.5); // the wave wobble on the reflection too
        refl = mix(refl, traced.rgb, clamp(traced.a, 0.0, 1.0));
    }
    refl *= max(rt_mat_reflect, 1.0); // the shiny classes' exposure boost, as krunker_rt_mat_specular
    float w = fresnel + (1.0 - fresnel) * body;
    base.rgb = (fresnel * refl + (1.0 - fresnel) * body * base.rgb) / max(w, 1e-4);
    base.a = w;
    rt_water_out = true;
    rt_water_w = w;
    rt_water_uv = gl_FragCoord.xy * rt_water_inv_size + offset;
}

// Box Office Cinematic preset (RT_EFFECT_CINEMATIC 65536, rt.h "Cinematic lighting", rt_pass.c cine_uniforms): the
// look-first balance. The ambient (directional sky ambient when bit 64 is on) is kept only at rt_cine_fill, the GI
// bounce is added at its real brightness (no ambient floor: krunker_gi_irradiance is Krunker-matched), AO darkens
// both, the sun is rt_cine_sun (map colour x strength x warmth) and krunker_tone_map applies rt_cine_exposure.
uniform float rt_cine_fill;
uniform vec3 rt_cine_sun;
uniform float rt_cine_exposure;
bool krunker_cine() { return KN_RT_CINE != 0 && (rt_effects & 65536) != 0; }

vec3 krunker_lambert(vec3 diffuse, vec3 normal) {
    float light_scale = correct_lights ? 1.0 : PI;
    vec3 view_dir = normalize(camera_position - world_pos);
    // AmbientLight (or the Box Office sky ambient) + the map's sun (skyLight, the only shadow caster)
    vec3 irradiance = krunker_ambient_irradiance(normal);
    if (krunker_cine()) {
        // the fill takes the map's authored ambient hue: the physical sky's blue turned warm maps (Sandstorm's pink-orange
        // ambient) cold; only its luminance follows the directional sky ambient (up brighter than down)
        const vec3 LUMC = vec3(0.2126, 0.7152, 0.0722);
        vec3 authored = ambient_color.rgb * ambient_intensity * light_scale;
        float la = dot(authored, LUMC), ls = dot(irradiance, LUMC);
        if (la > 1e-5) irradiance = authored * (ls / la);
        irradiance *= rt_cine_fill;
        if ((rt_effects & 32) != 0) irradiance += krunker_rt_gi() * PI; // the bounce, as it is
    } else if ((rt_effects & 32) != 0) irradiance = krunker_gi_irradiance(irradiance); // RT_EFFECT_GI (rt.h)
    if (use_light_map) irradiance += texture(light_map, tex_coord).rgb * light_map_intensity;
    // Box Office RT AO (1.0 unless RT_EFFECT_AO). Cinematic softens it (ao^0.6): in a small room every AO ray hits,
    // and with the reduced fill and the curve's toe full-strength AO crushed interiors to near black
    float ao = krunker_rt_ao();
    irradiance *= krunker_cine() ? pow(ao, 0.6) : ao;
    vec3 outgoing = irradiance * diffuse * RECIPROCAL_PI + emissive_radiance;
    vec3 sun = light_color.rgb * light_intensity * light_scale * krunker_shadow_mask(normal); // getShadow scales the light
    if (krunker_cine()) sun *= rt_cine_sun;
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
            c *= krunker_rt_local(i); // Box Office RT local light shadow (1.0 unless RT_EFFECT_LOCAL_LIGHTS)
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
    // Box Office RT reflections: the traced radiance along the reflection replaces the PMREM sample (the DFG /
    // F0 weighting below stays, so the energy matches three's indirectSpecular)
    vec3 radiance = ((rt_effects & 4) != 0 ? texture(rt_refl, gl_FragCoord.xy * rt_inv_viewport).rgb : krunker_scene_env_sample(dir, r)) * scene_env_intensity;
    vec3 irradiance = krunker_scene_env_sample(n, 1.0) * scene_env_intensity * krunker_rt_ao(); // iblIrradiance / PI
    vec2 dfg = texture(scene_env_dfg, vec2(r, clamp(dot(n, v), 0.0, 1.0))).rg;
    vec3 single_d, multi_d, single_m, multi_m;
    krunker_multiscatter(vec3(0.04), dfg, single_d, multi_d);
    krunker_multiscatter(albedo, dfg, single_m, multi_m);
    vec3 specular = radiance * mix(single_d, single_m, metalness) + mix(multi_d, multi_m, metalness) * irradiance;
    vec3 diffuse = albedo * (1.0 - metalness) * (1.0 - (single_d + multi_d)) * irradiance;
    return specular + diffuse;
}

// Box Office filmic shoulder (doc/raytracing.md "Tone mapping and bloom"; rt_filmic, any rtMode > 0, for maps
// whose own tone mapping is None or Linear, i.e. every shipped map). Krunker's frames are graded with no curve
// (values are display values), so a full ACES curve would re-grade every map (white walls 0.9 -> 0.74 at
// exposure 1). The shoulder leaves everything below RT_FILMIC_KNEE untouched and rolls the rest off
// exponentially toward 1 with slope 1 at the knee, so traced highlights (GGX sun lobes on metal, emissive,
// bright sand at a high lightI) compress instead of clipping. It runs on the brightest channel and scales the
// colour by it, which keeps the hue (a clipped orange no longer turns yellow), then desaturates what lies far
// above 1 toward white as film does. Maps with Reinhard / Cineon / ACES keep their own curve.
const float RT_FILMIC_KNEE = 0.8;
#if KN_RT
uniform bool rt_filmic;
#else
const bool rt_filmic = false;
#endif
vec3 krunker_filmic_shoulder(vec3 color) {
    float m = max(max(color.r, color.g), color.b);
    if (m <= RT_FILMIC_KNEE) return color;
    float range = 1.0 - RT_FILMIC_KNEE;
    float shoulder = RT_FILMIC_KNEE + range * (1.0 - exp(-(m - RT_FILMIC_KNEE) / range));
    vec3 c = color * (shoulder / m);
    float over = clamp((m - 1.0) * 0.25, 0.0, 0.5); // half-way to white by 3x over
    return mix(c, vec3(shoulder), over);
}

// Bloom weight (Box Office rtBloom, render_target.c / rt_bloom.frag): while rt_bloom_out is set (opaque
// draws into the half-float scene target), alpha = 1 + weight, where weight is how far the colour went above 1
// before the tone curve plus the material's emissive glow (dim emissive tints stay out of it), faded by fog.
#if KN_RT
uniform bool rt_bloom_out;
uniform float rt_bloom_scale; // RT_VM_BLOOM_SCALE for the first-person viewmodel (rt.h), else 1
#else
const bool rt_bloom_out = false;
const float rt_bloom_scale = 1.0;
#endif
const float RT_BLOOM_WEIGHT_MAX = 4.0; // rt.h RT_BLOOM_WEIGHT_MAX
const float RT_CINE_SPEC_BLOOM = 0.35; // rt.h RT_CINE_SPEC_BLOOM
float krunker_bloom_weight(vec3 color, vec3 emissive, float fog_amount) {
    float over = max(max(max(color.r, color.g), color.b) - 1.0, 0.0);
    float glow = smoothstep(0.25, 1.0, max(max(emissive.r, emissive.g), emissive.b));
    // Capped (a glint never outweighs a sign) and scrubbed: an unbounded weight from one hot specular texel spread by
    // the bloom chain into a screen-sized blob, and a NaN would poison every mip of the chain.
    // Cinematic's low bloom knee would make every hot specular highlight (a gun's sun glint) glow like a light:
    // there non-emissive over-brightness blooms at RT_CINE_SPEC_BLOOM, so mostly emissives and the sun bloom
    if (rt_cine) over *= RT_CINE_SPEC_BLOOM;
    float w = (min(over, RT_BLOOM_WEIGHT_MAX) + glow) * (1.0 - fog_amount) * rt_bloom_scale;
    return isnan(w) || isinf(w) ? 0.0 : w;
}

// Cinematic tone curve (rt.h "Cinematic colour pipeline", rt_grade.c): three.js's ACES fit (tone_mapping 4's,
// below) at RT_CINE_EXPOSURE on top of the preset's exposure, for maps whose own curve is None / Linear. Deeper
// blacks, a soft highlight roll-off and the film-like saturation the flat Krunker curve lacks.
const float RT_CINE_CURVE_EXPOSURE = 1.05; // rt.h RT_CINE_EXPOSURE
vec3 krunker_cine_curve(vec3 color) {
    const mat3 aces_input = mat3(vec3(0.59719, 0.07600, 0.02840), vec3(0.35458, 0.90834, 0.13383), vec3(0.04823, 0.01566, 0.83777));
    const mat3 aces_output = mat3(vec3(1.60475, -0.10208, -0.00327), vec3(-0.53108, 1.10813, -0.07276), vec3(-0.07367, -0.00605, 1.07602));
    vec3 x = color * RT_CINE_CURVE_EXPOSURE;
    vec3 c = aces_input * (x / 0.6);
    vec3 a = c * (c + 0.0245786) - 0.000090537;
    vec3 b = c * (0.983729 * c + 0.4329510) + 0.238081;
    vec3 per_channel = clamp(aces_output * (a / b), 0.0, 1.0);
    // ACES per channel drifts bright warm colours (sunlit sand, yellow neon) toward white: keep 60 % of the
    // hue-preserving version (the input's ratios scaled to the curve's peak)
    float m = max(max(x.r, x.g), x.b);
    vec3 hue_kept = m > 1e-4 ? x * (max(max(per_channel.r, per_channel.g), per_channel.b) / m) : per_channel;
    return clamp(mix(per_channel, hue_kept, 0.6), 0.0, 1.0);
}

// three.js r150 tonemapping_pars_fragment (renderer.toneMapping, toneMappingExposure)
vec3 krunker_tone_map(vec3 color) {
    if (krunker_cine()) color *= rt_cine_exposure; // the Cinematic preset's per-map exposure (rt_pass.c cine_exposure)
    if (krunker_cine() && tone_mapping <= 1) return krunker_cine_curve(tone_mapping == 1 ? tone_mapping_exposure * color : color);
    if (rt_filmic && tone_mapping <= 1) return krunker_filmic_shoulder(tone_mapping == 1 ? tone_mapping_exposure * color : color);
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
    vec3 fog_rgb = fog_color.rgb;
    // Box Office rtSky aerial perspective (rt_sky.glsl): the fog brightens / warms towards the sun
#if KN_RT
    if (psky_on != 0) fog_rgb = psky_fog_color(normalize(world_pos - camera_position), fog_rgb);
#endif
    return mix(color, fog_rgb, fog_amount);
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

// ---- the wear (the uniforms above): v10.0.6's wearMap node (deob-10.0.6.js:95692-95722)
float wear_curve(float level, float mid, float k, float top) {
    float s = 1.0 / (1.0 + exp(-k * (level - mid)));
    float s0 = 1.0 / (1.0 + exp(-k * (0.0 - mid)));
    float s1 = 1.0 / (1.0 + exp(-k * (1.0 - mid)));
    return clamp((s - s0) / (s1 - s0), 0.0, 1.0) * top;
}

vec3 wear_apply(vec3 base, vec2 uv) {
    vec4 m = texture(wear_map, uv);
    float rust = min(m.r * wear_curve(wear_level, 0.5, 8.0, 1.0), 1.0);
    float dirt = min(m.g * wear_curve(wear_level, 0.33, 5.5, 0.9), 1.0);
    float grime = min(m.b * wear_curve(wear_level, 0.2, 7.0, 1.0), 1.0);
    float scratch = min(m.a * wear_curve(wear_level, 0.5, 1.0, 1.0), 1.0);
    vec3 c = mix(base, vec3(0.325, 0.302, 0.294), scratch);
    c = mix(c, vec3(0.122, 0.078, 0.039), dirt);
    c = mix(c, vec3(0.129, 0.129, 0.129), grime);
    return mix(c, vec3(0.22, 0.098, 0.039), rust);
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

// deob:232218-232236: the decal over the albedo (outside its box the albedo stays)
vec3 decal_apply(vec3 base) {
    vec3 d = local_pos - decal_position;
    if (any(greaterThan(abs(d), decal_half_scale))) return base;
    vec2 uv = (d.xy + decal_half_scale.xy) / (decal_half_scale.xy * 2.0);
    vec4 decal = texture(decal_map, uv);
    return mix(base, decal.rgb, decal.a);
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
    if (wear_enabled) base_color.rgb = wear_apply(base_color.rgb, scaled_tex_coord);
    if (paint_enabled) base_color.rgb = paint_apply(base_color.rgb, scaled_tex_coord, scaled_tex_coord);
    if (decal_enabled) base_color.rgb = decal_apply(base_color.rgb);

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

        float rt_detail_rough = krunker_rt_detail(albedo, normal); // Cinematic surface detail (0 / no-op without rt_cine)
        if (rt_cine) albedo *= krunker_rt_crease() * krunker_rt_cavity(t_tex_coord.xy);
        vec3 diffuse = albedo;
        if (use_specular_map) specular_strength = texture(specular_map, maps_flip_v ? vec2(t_tex_coord.x, 1.0 - t_tex_coord.y) : t_tex_coord.xy).r;
        if (use_normal_map) normal = krunker_perturb_normal(normal, t_tex_coord.xy, krunker_normal_map_sample(t_tex_coord.xy));
        normal = krunker_rt_bevel(normal); // Box Office RT bevelled cube edges (no-op unless RT_EFFECT_BEVEL on a map cube)
        float metalness = 0.0, roughness = 1.0;
        bool rt_mat_active = (rt_effects & 128) != 0 && rt_mat_enabled; // RT_EFFECT_MATERIALS: a Box Office class
        bool rt_water_on = (rt_effects & 32768) != 0 && rt_water_kind > 0 && rt_mat_active; // RT_EFFECT_WATER (rt.h)
        if (use_metal_rough_map || use_scene_env || rt_mat_active) {
            // MeshStandardMaterial: diffuse = albedo * (1 - metalness), specular F0 = mix(0.04, albedo, metalness),
            // alpha = roughness^2; roughness is clamped as three.js does (0.04 minimum). A GLB env material is
            // shaded so with its factors alone too (v10's GLB materials are MeshStandardMaterial).
            vec3 mr = use_metal_rough_map ? texture(metal_rough_map, t_tex_coord.xy).rgb : vec3(1.0);
            float mf = rt_mat_active ? rt_mat_metalness : metalness_factor;
            float rf = rt_mat_active ? rt_mat_roughness : roughness_factor;
            // A Box Office part mask (use_metal_rough_map with a class: rt_masks.c) stores absolute roughness, metal
            // parts at 90/255 (rt_masks.c CLASS_VALUES). The class roughness is meant for the metal parts, so the mask
            // is normalised to it; multiplying the two made the gun's metal a near-mirror (0.5 x 0.35 = 0.18, 0.06
            // under Cinematic) whose sun highlight bloomed into a white blob.
            float mask_scale = rt_mat_active && use_metal_rough_map ? 255.0 / 90.0 : 1.0;
            roughness = clamp(rf * mr.g * mask_scale, 0.04, 1.0);
            metalness = clamp(mf * mr.b, 0.0, 1.0);
            if (rt_cine) { // Cinematic: smooth plastic / painted surfaces a step glossier; rough stays matte. Metal keeps
                // its class roughness (plus the brushed variation): a step glossier turned the first-person gun's
                // panels into sun mirrors that clipped to white (2x Custom's white area in the receiver crop)
                if (metalness >= 0.5) roughness = clamp(roughness + rt_detail_rough, 0.06, 1.0);
                else if (roughness < 0.6) roughness = clamp(roughness - 0.1 + rt_detail_rough, 0.06, 1.0);
            }
            pbr_active = true;
            pbr_alpha = roughness * roughness;
            // Box Office classes: the sun / point lights are analytic points, so a near-mirror class gives a GGX peak in
            // the thousands; floor the alpha of the direct lobe (roughness 0.12) like Filament's minimum roughness.
            if (rt_mat_active && !rt_water_on) pbr_alpha = max(pbr_alpha, 0.0144); // water / glass keep their sharp sun glint
            vec3 metal_f0 = rt_mat_active ? max(albedo, vec3(rt_mat_f0)) : albedo;
            pbr_f0 = mix(vec3(0.04), metal_f0, metalness);
            diffuse = albedo * (1.0 - metalness);
        }

        base_color.rgb = krunker_lambert(diffuse, normal);
        if (use_scene_env) base_color.rgb += krunker_scene_env(albedo, metalness, roughness, normal);
        else if (rt_mat_active && !rt_water_on) base_color.rgb += krunker_rt_mat_specular(max(albedo, vec3(rt_mat_f0)), metalness, roughness, normal);
        if (rt_water_on) krunker_rt_water(base_color, normal, roughness); // Box Office RT water / glass (rt.h)
    }

    if (use_env_map) {
        vec3 n = use_flat_normal ? normalize(flat_normal) : normalize(cross(dFdx(world_pos), dFdy(world_pos)));
        // the reflection follows the normal map too (three.js: normal_fragment_maps before envmap_fragment)
        if (use_normal_map) n = krunker_perturb_normal(n, t_tex_coord.xy, krunker_normal_map_sample(t_tex_coord.xy));
        vec3 r = reflect(normalize(world_pos - camera_position), n);
        vec2 env_uv = vec2(atan(r.z, r.x) * 0.15915494309189535 + 0.5, asin(clamp(r.y, -1.0, 1.0)) * RECIPROCAL_PI + 0.5);
        // Box Office RT reflections: the traced radiance instead of the skin's equirect env map
        vec3 env_radiance = (rt_effects & 4) != 0 ? texture(rt_refl, gl_FragCoord.xy * rt_inv_viewport).rgb : texture(env_map, env_uv).rgb;
        base_color.rgb = albedo * env_radiance * env_map_intensity + emissive_radiance;
    }

    // Box Office RT (dev only, KRUNKNATIVE_RT_FORCE_REFL): RT_EFFECT_FORCE_REFL (16) blends the traced reflection into
    // every lit surface so the effect can be looked at on maps without reflective materials
    if ((rt_effects & 16) != 0 && lighting_enabled && !basic_material && !use_env_map)
        base_color.rgb = mix(base_color.rgb, texture(rt_refl, gl_FragCoord.xy * rt_inv_viewport).rgb, 0.35);

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

    float bloom_weight = 0.0;
    if (rt_bloom_out) {
        vec3 pre = tone_mapping == 1 ? tone_mapping_exposure * base_color.rgb : base_color.rgb;
        float fog_amount = fog_enabled && material_fog_enabled ? smoothstep(fog_near, fog_far, view_depth) : 0.0;
        bloom_weight = krunker_bloom_weight(pre, emissive_radiance, fog_amount);
    }
    base_color.rgb = krunker_tone_map(base_color.rgb);
    frag_color = base_color;
    if (rt_bloom_out) frag_color = vec4(min(base_color.rgb, vec3(1.0)), 1.0 + bloom_weight); // opaque: no blend reads it
    // Box Office RT water / glass: the surface over the refracted opaque frame (alpha 1: the blend is done here), or,
    // without this frame's copy, the coverage as alpha for the hardware blend (krunker_rt_water)
    if (rt_water_out && !rt_bloom_out) {
        frag_color = rt_water_copy ? vec4(mix(texture(rt_water_scene, rt_water_uv).rgb, base_color.rgb, rt_water_w), 1.0)
                                   : vec4(base_color.rgb, rt_water_w);
    }
}
