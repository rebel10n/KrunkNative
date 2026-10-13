// LineMaterial (client.h): unlit single-colour lines / flat overlays through scene_render(),
// three.js LineBasicMaterial and MeshBasicMaterial. Used by the show_navmeshes overlay
// (nav_overlay.c). Fogged like three.js materials (fog: true) with the map's fog.
#include <gl.h>
#include <client.h>
#include <stdlib.h>

static unsigned int g_line_program;

static GlUniform u_color = GL_UNIFORM_INIT("color");
static GlUniform u_fog_color = GL_UNIFORM_INIT("fog_color");
static GlUniform u_fog_near = GL_UNIFORM_INIT("fog_near");
static GlUniform u_fog_far = GL_UNIFORM_INIT("fog_far");
static GlUniform u_fog_enabled = GL_UNIFORM_INIT("fog_enabled");

static void line_material_update_uniforms(Material *base) {
    LineMaterial *material = (LineMaterial *) base;
    const unsigned int program = material->base.program;
    const RenderLighting *l = &g_render_lighting;
    gl_uniform_4f(&u_color, program, material->color.x, material->color.y, material->color.z, material->color.w);
    gl_uniform_1i(&u_fog_enabled, program, material->fog && l->fog_enabled ? 1 : 0);
    gl_uniform_3f(&u_fog_color, program, l->fog_color.x, l->fog_color.y, l->fog_color.z);
    gl_uniform_1f(&u_fog_near, program, l->fog_near);
    gl_uniform_1f(&u_fog_far, program, l->fog_far);
    scene_draw_state(0, 0, 0); // double-sided, normal blending, no depth writes
    if (!material->depth_test) scene_draw_no_depth_test();
}

static const MaterialVTable line_material_vtable = {line_material_update_uniforms};

LineMaterial *line_material_init(void) {
    if (!g_line_program) {
        char *vert = concat(assets_path(), "shaders/line.vert");
        char *frag = concat(assets_path(), "shaders/line.frag");
        if (vert && frag) {
            ensure_asset_file(vert);
            ensure_asset_file(frag);
            g_line_program = shader_compile(vert, frag);
        }
        free(vert);
        free(frag);
        if (!g_line_program) return NULL;
    }
    LineMaterial *material = calloc(1, sizeof(LineMaterial));
    if (!material) return NULL;
    material->base.vtable = &line_material_vtable;
    material->base.program = g_line_program;
    material->color = (vec4) {1.0f, 1.0f, 1.0f, 1.0f};
    material->fog = 1;
    material->depth_test = 1;
    return material;
}

void line_material_fini_program(void) {
    if (!g_line_program) return;
    if (g_active_shader == g_line_program) g_active_shader = 0;
    glDeleteProgram(g_line_program);
    gl_uniform_forget_program(g_line_program);
    g_line_program = 0;
}
