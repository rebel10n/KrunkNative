#include <gl.h>
#include <client.h>
#include <render_settings.h>
#include <GLFW/glfw3.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static unsigned int shader_program = 0;
static BasicPointLight g_point_lights[BASIC_MAX_POINT_LIGHTS];
static int g_point_light_count;

void basic_material_set_point_lights(const BasicPointLight *lights, int count) {
    if (count > BASIC_MAX_POINT_LIGHTS) count = BASIC_MAX_POINT_LIGHTS;
    if (count < 0 || !lights) count = 0;
    if (count) memcpy(g_point_lights, lights, (size_t) count * sizeof(BasicPointLight));
    g_point_light_count = count;
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

static GlUniform u_point_light_position[BASIC_MAX_POINT_LIGHTS];
static GlUniform u_point_light_color[BASIC_MAX_POINT_LIGHTS];
static GlUniform u_point_light_falloff[BASIC_MAX_POINT_LIGHTS];

static void basic_point_light_uniforms_init(void) {
    static char names[3][BASIC_MAX_POINT_LIGHTS][40];
    if (u_point_light_position[0].name) return;

    for (int i = 0; i < BASIC_MAX_POINT_LIGHTS; i++) {
        snprintf(names[0][i], sizeof(names[0][i]), "point_light_position[%d]", i);
        snprintf(names[1][i], sizeof(names[1][i]), "point_light_color[%d]", i);
        snprintf(names[2][i], sizeof(names[2][i]), "point_light_falloff[%d]", i);
        u_point_light_position[i] = (GlUniform) GL_UNIFORM_INIT(names[0][i]);
        u_point_light_color[i] = (GlUniform) GL_UNIFORM_INIT(names[1][i]);
        u_point_light_falloff[i] = (GlUniform) GL_UNIFORM_INIT(names[2][i]);
    }
}

// lighting 2: the map's point lights (only uploaded to programs that declare them)
static void basic_upload_point_lights(const unsigned int program, const int lighting) {
    if (gl_uniform_location(&u_point_light_count, program) < 0) return;

    const int count = lighting == 2 ? g_point_light_count : 0;
    gl_uniform_1i(&u_point_light_count, program, count);

    basic_point_light_uniforms_init();
    for (int i = 0; i < count; i++) {
        const BasicPointLight *light = &g_point_lights[i];
        gl_uniform_3f(&u_point_light_position[i], program, light->position.x, light->position.y, light->position.z);
        gl_uniform_3f(&u_point_light_color[i], program, light->color.x, light->color.y, light->color.z);
        gl_uniform_2f(&u_point_light_falloff[i], program, light->distance, light->decay);
    }
}

void basic_material_update_uniforms(BasicMaterial *material) {
    const unsigned int program = material->base.program;
    const ClientRenderSettings *render_settings = client_render_settings();

    gl_uniform_1i(&u_uv_mode, program, (int) material->uv_mode);
    gl_uniform_3f(&u_uv_size, program, material->uv_size.x, material->uv_size.y, material->uv_size.z);
    gl_uniform_1i(&u_uv_flip_sides, program, material->uv_flip_sides);
    gl_uniform_1i(&u_uv_flip_top, program, material->uv_flip_top);
    gl_uniform_1i(&u_use_color_gradient, program, material->use_color_gradient);
    gl_uniform_4f(&u_color_bottom, program, material->color_bottom.x, material->color_bottom.y, material->color_bottom.z, material->color_bottom.w);
    // ambientShading off: 134.js toggleMeshGroup("ambient_0"/"ambient_1", false) hides the AO planes
    gl_uniform_1f(&u_alpha_test, program, material->ambient_group && !render_settings->ambient_shading ? 2.0f : material->alpha_test);
    // ambientShading: 134.js toggleMeshGroup("ambient_1") shows/hides the ramp edge strips
    gl_uniform_1f(&u_ramp_shade_width, program, render_settings->ambient_shading ? material->ramp_shade_width : 0.0f);
    gl_uniform_1i(&u_unlit, program, material->unlit);
    gl_uniform_1i(&u_ambient_enabled, program, material->ambient_enabled);
    gl_uniform_1i(&u_material_fog_enabled, program, material->fog_enabled);
    gl_uniform_1i(&u_use_flat_normal, program, material->use_flat_normal);
    gl_uniform_3f(&u_flat_normal, program, material->flat_normal.x, material->flat_normal.y, material->flat_normal.z);

    const float repeat[] = {
        material->texture_repeat.x, 0.0f, 0.0f,
        0.0f, material->texture_repeat.y, 0.0f,
        0.0f, 0.0f, 1.0f,
    };

    const float translate[] = {
        1.0f, 0.0f, material->texture_offset.x,
        0.0f, 1.0f, material->texture_offset.y,
        0.0f, 0.0f, 1.0f,
    };

    float transform[9];
    mat3x3(repeat, translate, transform);

    // three.js texture.offset (map tox/toy) is added after the repeat
    transform[2] += material->texture_base_offset.x;
    transform[5] += material->texture_base_offset.y;

    // three.js side: FrontSide culls back faces (CCW front); blend function and depth
    // writes. scene_render() resets all three to their defaults before every draw and
    // applies only the changes (scene_draw_state), so other materials never inherit them.
    scene_draw_state(!material->double_sided, material->additive_blend, !material->no_depth_write);

    gl_uniform_4f(&u_color, program, material->color.x, material->color.y, material->color.z, material->color.w);
    gl_uniform_4f(&u_emissive, program, material->emissive.x, material->emissive.y, material->emissive.z, material->emissive.w);
    gl_uniform_4f(&u_ambient_color, program, g_render_lighting.ambient_color.x, g_render_lighting.ambient_color.y, g_render_lighting.ambient_color.z, g_render_lighting.ambient_color.w);
    gl_uniform_4f(&u_light_color, program, g_render_lighting.light_color.x, g_render_lighting.light_color.y, g_render_lighting.light_color.z, g_render_lighting.light_color.w);
    gl_uniform_4f(&u_fog_color, program, g_render_lighting.fog_color.x, g_render_lighting.fog_color.y, g_render_lighting.fog_color.z, g_render_lighting.fog_color.w);
    gl_uniform_3f(&u_light_direction, program, g_render_lighting.light_direction.x, g_render_lighting.light_direction.y, g_render_lighting.light_direction.z);
    gl_uniform_1f(&u_ambient_intensity, program, g_render_lighting.ambient_intensity);
    gl_uniform_1f(&u_light_intensity, program, g_render_lighting.light_intensity);
    gl_uniform_1f(&u_fog_near, program, g_render_lighting.fog_near);
    gl_uniform_1f(&u_fog_far, program, g_render_lighting.fog_far);
    gl_uniform_1i(&u_lighting_enabled, program, g_render_lighting.enabled);
    gl_uniform_1i(&u_fog_enabled, program, g_render_lighting.fog_enabled);

    gl_uniform_mat3(&u_tex_transform, program, GL_TRUE, transform);
    gl_uniform_1i(&u_tex, program, 0);

    // lighting (134.js, restart in v3.1.1; applied per draw here): 0 Basic, 1 Lambert, 2 Phong
    gl_uniform_1i(&u_lighting_mode, program, render_settings->lighting);
    basic_upload_point_lights(program, render_settings->lighting);

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

const MaterialVTable basic_material_vtable = { (void *) basic_material_update_uniforms };

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
