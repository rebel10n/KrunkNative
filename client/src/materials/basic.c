#include <gl.h>
#include <client.h>
#include <render_settings.h>
#include <map_dialect.h> // map_dialect_light_unit_factor: v7 1, v10 PI (THREE r186 light units)
#include <GLFW/glfw3.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static unsigned int shader_program = 0;

// ---- map-G: the forward light list (client.h BasicLight). Slot i is used when
// g_lights_used[i]; handles are slot + 1. The v3.1.1 basic_material_set_point_lights() set is
// a group of slots of its own (g_lights_legacy) so both APIs can coexist.
static BasicLight g_lights[BASIC_MAX_LIGHTS];
static unsigned char g_lights_used[BASIC_MAX_LIGHTS];
static unsigned char g_lights_legacy[BASIC_MAX_LIGHTS];

int basic_lights_add(const BasicLight *light) {
    if (!light) return 0;
    for (int i = 0; i < BASIC_MAX_LIGHTS; i++) {
        if (g_lights_used[i]) continue;
        g_lights[i] = *light;
        g_lights_used[i] = 1;
        g_lights_legacy[i] = 0;
        return i + 1;
    }
    return 0;
}

void basic_lights_update(const int handle, const BasicLight *light) {
    if (handle < 1 || handle > BASIC_MAX_LIGHTS || !light || !g_lights_used[handle - 1]) return;
    g_lights[handle - 1] = *light;
}

static BasicLight g_lights_override[BASIC_MAX_LIGHTS];
static int g_lights_override_count = -1;

void basic_lights_override(const BasicLight *lights, int count) {
    if (count > BASIC_MAX_LIGHTS) count = BASIC_MAX_LIGHTS;
    if (count > 0 && lights) memcpy(g_lights_override, lights, (size_t) count * sizeof(BasicLight));
    g_lights_override_count = lights || count <= 0 ? count : -1;
}

void basic_lights_remove(const int handle) {
    if (handle < 1 || handle > BASIC_MAX_LIGHTS) return;
    g_lights_used[handle - 1] = 0;
    g_lights_legacy[handle - 1] = 0;
}

static float g_clip_plane[4];
static int g_clip_enabled;

void basic_material_set_clip_plane(const float plane[4]) {
    g_clip_enabled = plane != NULL;
    if (plane) memcpy(g_clip_plane, plane, sizeof(g_clip_plane));
}

void basic_material_set_point_lights(const BasicPointLight *lights, int count) {
    for (int i = 0; i < BASIC_MAX_LIGHTS; i++) {
        if (g_lights_legacy[i]) basic_lights_remove(i + 1);
    }
    if (count < 0 || !lights) count = 0;
    for (int i = 0; i < count; i++) {
        // color is colour * intensity already
        const BasicLight light = {
            .type = BASIC_LIGHT_POINT, .visible = 1, .color = lights[i].color, .intensity = 1.0f,
            .distance = lights[i].distance, .decay = lights[i].decay, .position = lights[i].position,
        };
        const int handle = basic_lights_add(&light);
        if (!handle) break;
        g_lights_legacy[handle - 1] = 1;
    }
}

// Uniform handles (client.h GlUniform): locations and last values are cached per program,
// so the lighting/fog uniforms that repeat on every draw are uploaded once per change.
#define U(var, name) static GlUniform var = GL_UNIFORM_INIT(name)
U(u_point_light_count, "point_light_count");
U(u_uv_mode, "uv_mode");
U(u_uv_size, "uv_size");
U(u_uv_flip_sides, "uv_flip_sides");
U(u_uv_flip_top, "uv_flip_top");
U(u_use_color_gradient, "use_color_gradient");
U(u_color_bottom, "color_bottom");
U(u_use_vertex_color, "use_vertex_color");
U(u_alpha_test, "alpha_test");
U(u_ramp_shade_width, "ramp_shade_width");
U(u_unlit, "unlit");
U(u_ambient_enabled, "ambient_enabled");
U(u_material_fog_enabled, "material_fog_enabled");
U(u_use_flat_normal, "use_flat_normal");
U(u_flat_normal, "flat_normal");
U(u_color, "color");
U(u_emissive, "emissive");
U(u_ambient_color, "ambient_color");
U(u_light_color, "light_color");
U(u_fog_color, "fog_color");
U(u_light_direction, "light_direction");
U(u_ambient_intensity, "ambient_intensity");
U(u_light_intensity, "light_intensity");
U(u_fog_near, "fog_near");
U(u_fog_far, "fog_far");
U(u_lighting_enabled, "lighting_enabled");
U(u_fog_enabled, "fog_enabled");
U(u_tex_transform, "tex_transform");
U(u_tex, "tex");
U(u_lighting_mode, "lighting_mode");
U(u_wave_height, "wave_height");
U(u_wave_speed, "wave_speed");
U(u_liquid_value, "liquid_value");
U(u_use_emissive_map, "use_emissive_map");
U(u_emissive_map, "emissive_map");
#undef U

// map-G uniforms (v7.1.4 materials, lights and renderer output)
#define U(var, name) static GlUniform var = GL_UNIFORM_INIT(name)
U(u_tex_srgb, "tex_srgb");
U(u_sprite, "sprite");
U(u_correct_lights, "correct_lights");
U(u_tone_mapping, "tone_mapping");
U(u_tone_mapping_exposure, "tone_mapping_exposure");
U(u_output_encoding, "output_encoding");
U(u_use_light_map, "use_light_map");
U(u_clip_enabled, "clip_enabled");
U(u_clip_plane, "clip_plane");
U(u_light_map, "light_map");
U(u_light_map_intensity, "light_map_intensity");
#undef U

// WP-F store-item effects (skin_fx.c): env map and projected texture coordinates
#define U(var, name) static GlUniform var = GL_UNIFORM_INIT(name)
U(u_use_env_map, "use_env_map");
U(u_env_map, "env_map");
U(u_env_map_intensity, "env_map_intensity");
U(u_projection_uv, "projection_uv");
#undef U

// basic.frag light arrays (MAX_LIGHTS): lt_type 0 point, 1 spot, 2 directional, 3 rect
enum { LU_TYPE, LU_POSITION, LU_DIRECTION, LU_COLOR, LU_PARAMS, LU_HALF_WIDTH, LU_HALF_HEIGHT, LU_COUNT };
static GlUniform u_lights[LU_COUNT][BASIC_MAX_LIGHTS];

static void basic_light_uniforms_init(void) {
    static const char *const fields[LU_COUNT] = {"lt_type", "lt_position", "lt_direction", "lt_color", "lt_params", "lt_half_width", "lt_half_height"};
    static char names[LU_COUNT][BASIC_MAX_LIGHTS][40];
    if (u_lights[0][0].name) return;

    for (int f = 0; f < LU_COUNT; f++) {
        for (int i = 0; i < BASIC_MAX_LIGHTS; i++) {
            snprintf(names[f][i], sizeof(names[f][i]), "%s[%d]", fields[f], i);
            u_lights[f][i] = (GlUniform) GL_UNIFORM_INIT(names[f][i]);
        }
    }
}

static vec3 basic_vec3_normalize(const vec3 v) {
    const float length = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
    return length > 0.0f ? (vec3) {v.x / length, v.y / length, v.z / length} : (vec3) {0.0f, 1.0f, 0.0f};
}

// three.js r150 WebGLLights.setup: colour * intensity * (useLegacyLights ? PI : 1) for point,
// spot and directional lights, colour * intensity for rect-area lights; the PI cancels against
// BRDF_Lambert in basic.frag. Light directions point from the target to the light.
// v10.0.2 (THREE r186, physical lights only) multiplies the map's point / spot / directional
// intensities by config.lightIntensityScaling = PI instead (render.js addPointLight etc.,
// L200274-200296); rect-area lights keep intensity || 1. The factor is the ruleset's.
static void basic_upload_lights(const unsigned int program, const int lighting) {
    if (gl_uniform_location(&u_point_light_count, program) < 0) return;

    basic_light_uniforms_init();
    const float scale = (g_render_lighting.correct_lights ? 1.0f : (float) M_PI) * map_dialect_light_unit_factor();
    int count = 0;

    const int overridden = g_lights_override_count >= 0;
    for (int i = 0; i < (overridden ? g_lights_override_count : BASIC_MAX_LIGHTS) && lighting > 0; i++) {
        const BasicLight *light = overridden ? &g_lights_override[i] : &g_lights[i];
        if ((!overridden && !g_lights_used[i]) || !light->visible) continue;
        if (light->type == BASIC_LIGHT_RECT && lighting != 3) continue; // only MeshStandardMaterial takes rect-area lights

        const float s = light->type == BASIC_LIGHT_RECT ? 1.0f : scale;
        vec3 direction = basic_vec3_normalize((vec3) {light->position.x - light->target.x, light->position.y - light->target.y, light->position.z - light->target.z});
        float params[4] = {light->distance, light->decay, 0.0f, 0.0f};
        vec3 half_width = {0}, half_height = {0};

        if (light->type == BASIC_LIGHT_SPOT) {
            params[2] = cosf(light->angle);                                 // coneCos
            params[3] = cosf(light->angle * (1.0f - light->penumbra));      // penumbraCos
        } else if (light->type == BASIC_LIGHT_RECT) {
            // rotateY(ry) rotateX(rx) rotateZ(rz): R = Ry * Rx * Rz applied to (w/2, 0, 0) / (0, h/2, 0)
            const float cx = cosf(light->rotation.x), sx = sinf(light->rotation.x);
            const float cy = cosf(light->rotation.y), sy = sinf(light->rotation.y);
            const float cz = cosf(light->rotation.z), sz = sinf(light->rotation.z);
            const float m[9] = { // row-major Ry(ry) * Rx(rx) * Rz(rz) (three makeRotationFromEuler "YXZ")
                cy * cz + sy * sx * sz, sy * sx * cz - cy * sz, sy * cx,
                cx * sz, cx * cz, -sx,
                cy * sx * sz - sy * cz, sz * sy + cy * sx * cz, cy * cx,
            };
            const float hw = light->width * 0.5f, hh = light->height * 0.5f;
            half_width = (vec3) {m[0] * hw, m[3] * hw, m[6] * hw};
            half_height = (vec3) {m[1] * hh, m[4] * hh, m[7] * hh};
        }

        const int type = light->type == BASIC_LIGHT_POINT ? 0 : light->type == BASIC_LIGHT_SPOT ? 1 : light->type == BASIC_LIGHT_DIRECTIONAL ? 2 : 3;
        gl_uniform_1i(&u_lights[LU_TYPE][count], program, type);
        gl_uniform_3f(&u_lights[LU_POSITION][count], program, light->position.x, light->position.y, light->position.z);
        gl_uniform_3f(&u_lights[LU_DIRECTION][count], program, direction.x, direction.y, direction.z);
        gl_uniform_3f(&u_lights[LU_COLOR][count], program, light->color.x * light->intensity * s, light->color.y * light->intensity * s, light->color.z * light->intensity * s);
        gl_uniform_4f(&u_lights[LU_PARAMS][count], program, params[0], params[1], params[2], params[3]);
        gl_uniform_3f(&u_lights[LU_HALF_WIDTH][count], program, half_width.x, half_width.y, half_width.z);
        gl_uniform_3f(&u_lights[LU_HALF_HEIGHT][count], program, half_height.x, half_height.y, half_height.z);
        count++;
    }

    gl_uniform_1i(&u_point_light_count, program, count);
}

void basic_material_update_uniforms(Material *base) {
    BasicMaterial *material = (BasicMaterial *) base;
    const unsigned int program = material->base.program;
    const ClientRenderSettings *render_settings = client_render_settings();

    gl_uniform_1i(&u_uv_mode, program, (int) material->uv_mode);
    gl_uniform_3f(&u_uv_size, program, material->uv_size.x, material->uv_size.y, material->uv_size.z);
    gl_uniform_1i(&u_uv_flip_sides, program, material->uv_flip_sides);
    gl_uniform_1i(&u_uv_flip_top, program, material->uv_flip_top);
    gl_uniform_1i(&u_use_color_gradient, program, material->use_color_gradient);
    gl_uniform_4f(&u_color_bottom, program, material->color_bottom.x, material->color_bottom.y, material->color_bottom.z, material->color_bottom.w);
    gl_uniform_1i(&u_use_vertex_color, program, material->use_vertex_color);
    // ambientShading off: 134.js toggleMeshGroup("ambient_0"/"ambient_1", false) hides the AO planes
    gl_uniform_1f(&u_alpha_test, program, material->ambient_group && !render_settings->ambient_shading ? 2.0f : material->alpha_test);
    // ambientShading: 134.js toggleMeshGroup("ambient_1") shows/hides the ramp edge strips
    gl_uniform_1f(&u_ramp_shade_width, program, render_settings->ambient_shading ? material->ramp_shade_width : 0.0f);
    gl_uniform_1i(&u_unlit, program, material->unlit);
    gl_uniform_1i(&u_ambient_enabled, program, material->ambient_enabled);
    gl_uniform_1i(&u_material_fog_enabled, program, material->fog_enabled);
    gl_uniform_1i(&u_use_flat_normal, program, material->use_flat_normal);
    gl_uniform_3f(&u_flat_normal, program, material->flat_normal.x, material->flat_normal.y, material->flat_normal.z);

    // three.js Texture.updateMatrix: setUvTransform(offset, repeat, rotation, center (0, 0)):
    //   u' = sx * (c * u + s * v) + tx,  v' = sy * (-s * u + c * v) + ty
    // with offset = map tox/toy + the animation offset (kept in repeat units: frame index /
    // scroll progress, main.c) and rotation the map "tro" (v7 Texture.rotation).
    const float sx = material->texture_repeat.x, sy = material->texture_repeat.y;
    const float rc = cosf(material->texture_rotation), rs = sinf(material->texture_rotation);
    const float transform[9] = {
        sx * rc, sx * rs, material->texture_offset.x * sx + material->texture_base_offset.x,
        -sy * rs, sy * rc, material->texture_offset.y * sy + material->texture_base_offset.y,
        0.0f, 0.0f, 1.0f,
    };

    // three.js side: FrontSide culls back faces (CCW front), BackSide front faces; blend
    // function and depth writes. scene_render() resets them to their defaults before every
    // draw and applies only the changes (scene_draw_state), so other materials never inherit them.
    scene_draw_state(!material->double_sided, material->additive_blend, !material->no_depth_write);
    if (!material->double_sided && material->cull_front) scene_draw_cull_front();

    gl_uniform_4f(&u_color, program, material->color.x, material->color.y, material->color.z, material->color.w);
    gl_uniform_4f(&u_emissive, program, material->emissive.x, material->emissive.y, material->emissive.z, material->emissive.w);
    gl_uniform_4f(&u_ambient_color, program, g_render_lighting.ambient_color.x, g_render_lighting.ambient_color.y, g_render_lighting.ambient_color.z, g_render_lighting.ambient_color.w);
    gl_uniform_4f(&u_light_color, program, g_render_lighting.light_color.x, g_render_lighting.light_color.y, g_render_lighting.light_color.z, g_render_lighting.light_color.w);
    gl_uniform_4f(&u_fog_color, program, g_render_lighting.fog_color.x, g_render_lighting.fog_color.y, g_render_lighting.fog_color.z, g_render_lighting.fog_color.w);
    gl_uniform_3f(&u_light_direction, program, g_render_lighting.light_direction.x, g_render_lighting.light_direction.y, g_render_lighting.light_direction.z);
    // v10 sceneInit: ambientI / lightI * lightIntensityScaling (L201738, L201743); v7: x 1
    const float light_units = map_dialect_light_unit_factor();
    gl_uniform_1f(&u_ambient_intensity, program, g_render_lighting.ambient_intensity * light_units);
    gl_uniform_1f(&u_light_intensity, program, g_render_lighting.light_intensity * light_units);
    gl_uniform_1f(&u_fog_near, program, g_render_lighting.fog_near);
    gl_uniform_1f(&u_fog_far, program, g_render_lighting.fog_far);
    gl_uniform_1i(&u_lighting_enabled, program, g_render_lighting.enabled);
    gl_uniform_1i(&u_fog_enabled, program, g_render_lighting.fog_enabled);

    gl_uniform_mat3(&u_tex_transform, program, GL_TRUE, transform);
    gl_uniform_1i(&u_tex, program, 0);

    // lighting (restart in v7.1.4; applied per draw here): 0 Basic, 1 Lambert, 2 Phong,
    // 3 Physical (MeshStandardMaterial, roughness 1, metalness 0)
    gl_uniform_1i(&u_lighting_mode, program, render_settings->lighting);
    basic_upload_lights(program, render_settings->lighting);
    gl_uniform_1i(&u_tex_srgb, program, material->texture_srgb);
    gl_uniform_1i(&u_sprite, program, material->sprite);
    gl_uniform_1i(&u_correct_lights, program, g_render_lighting.correct_lights);
    gl_uniform_1i(&u_tone_mapping, program, g_render_lighting.tone_mapping);
    gl_uniform_1f(&u_tone_mapping_exposure, program, g_render_lighting.tone_mapping_exposure);
    gl_uniform_1i(&u_output_encoding, program, g_render_lighting.output_encoding);
    gl_uniform_1i(&u_clip_enabled, program, g_clip_enabled);
    if (g_clip_enabled) gl_uniform_4f(&u_clip_plane, program, g_clip_plane[0], g_clip_plane[1], g_clip_plane[2], g_clip_plane[3]);

    // liquids: 55.js render() adds delta * 0.003 to liquidVal every frame and re-displaces the
    // vertices every liquidFreq (50) ms; objectAnim off never displaces them (flat)
    // (basic.vert reads liquid_value only when wave_height != 0: skip the clock otherwise)
    const float wave_height = render_settings->object_anim ? material->wave_height : 0.0f;
    gl_uniform_1f(&u_wave_height, program, wave_height);
    gl_uniform_1f(&u_wave_speed, program, material->wave_speed);
    if (wave_height != 0.0f) {
        const double now_ms = glfwGetTime() * 1000.0;
        gl_uniform_1f(&u_liquid_value, program, (float) (floor(now_ms / 50.0) * 50.0 * 0.003));
    }

    gl_uniform_1i(&u_use_emissive_map, program, material->emissive_texture != 0);
    gl_uniform_1i(&u_emissive_map, program, BASIC_EMISSIVE_TEXTURE_UNIT);

    if (material->emissive_texture) {
        glActiveTexture(GL_TEXTURE0 + BASIC_EMISSIVE_TEXTURE_UNIT);
        glBindTexture(GL_TEXTURE_2D, material->emissive_texture);
        glActiveTexture(GL_TEXTURE0); // g_active_texture tracks unit 0 only
    }

    // lightMap: legacy lights upload lightMapIntensity * PI (three r150 refreshUniformsCommon);
    // v10 sets lightMapIntensity = i * lightIntensityScaling (L199446)
    gl_uniform_1i(&u_use_light_map, program, material->light_map != 0);
    if (material->light_map) {
        gl_uniform_1i(&u_light_map, program, BASIC_LIGHT_MAP_TEXTURE_UNIT);
        gl_uniform_1f(&u_light_map_intensity, program,
                      material->light_map_intensity * (g_render_lighting.correct_lights ? 1.0f : (float) M_PI) * light_units);
        glActiveTexture(GL_TEXTURE0 + BASIC_LIGHT_MAP_TEXTURE_UNIT);
        glBindTexture(GL_TEXTURE_2D, material->light_map);
        glActiveTexture(GL_TEXTURE0);
    }

    // WP-F: skin env map (texture unit 4) and projection
    gl_uniform_1i(&u_use_env_map, program, material->env_map != 0);
    if (material->env_map) {
        gl_uniform_1i(&u_env_map, program, BASIC_ENV_MAP_TEXTURE_UNIT);
        gl_uniform_1f(&u_env_map_intensity, program, material->env_map_intensity);
        glActiveTexture(GL_TEXTURE0 + BASIC_ENV_MAP_TEXTURE_UNIT);
        glBindTexture(GL_TEXTURE_2D, material->env_map);
        glActiveTexture(GL_TEXTURE0);
    }
    gl_uniform_1i(&u_projection_uv, program, material->projection_uv);

    if (material->texture && g_active_texture == material->texture || !material->texture && g_blank_texture && g_active_texture == g_blank_texture) return;

    if (material->texture) {
        glBindTexture(GL_TEXTURE_2D, material->texture);
        g_active_texture = material->texture;
    } else {
        if (!g_blank_texture) {
            glGenTextures(1, &g_blank_texture);
            glBindTexture(GL_TEXTURE_2D, g_blank_texture);

            const unsigned char blank[] = {255, 255, 255};
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 1, 1, 0, GL_RGB, GL_UNSIGNED_BYTE, blank);
        }

        glBindTexture(GL_TEXTURE_2D, g_blank_texture);
        g_active_texture = g_blank_texture;
    }
}

const MaterialVTable basic_material_vtable = { basic_material_update_uniforms };

BasicMaterial *basic_material_init() {
    BasicMaterial *material = calloc(1, sizeof(BasicMaterial));
    material->base.vtable = &basic_material_vtable;

    material->color.x = 1.0f;
    material->color.y = 1.0f;
    material->color.z = 1.0f;
    material->color.w = 1.0f;

    material->texture_repeat.x = 1.0f;
    material->texture_repeat.y = 1.0f;
    material->uv_mode = BASIC_UV_MESH;
    material->uv_size = (vec3) {1.0f, 1.0f, 1.0f};
    material->color_bottom = material->color;
    material->double_sided = 1;
    material->ambient_enabled = 1;
    material->fog_enabled = 1;
    material->flat_normal.y = 1.0f;

    if (!shader_program) {
        char *vert_shader = concat(assets_path(), "shaders/basic.vert");
        char *frag_shader = concat(assets_path(), "shaders/basic.frag");

        shader_program = shader_compile(vert_shader, frag_shader);

        free(vert_shader);
        free(frag_shader);
    }

    if (!shader_program) {
        free(material);
        return NULL;
    }

    material->base.program = shader_program;
    return material;
}
