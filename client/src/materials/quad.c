#include <glad/glad.h>
#include <client.h>
#include <stdlib.h>

static unsigned int shader_program = 0;

void quad_material_update_uniforms(QuadMaterial *material) {
    const int color = glGetUniformLocation(material->base.program, "color");
    const int aspect = glGetUniformLocation(material->base.program, "aspect");
    const int r_clip = glGetUniformLocation(material->base.program, "r_clip");

    const int border_bottom_left_radius = glGetUniformLocation(material->base.program, "border_bottom_left_radius");
    const int border_bottom_right_radius = glGetUniformLocation(material->base.program, "border_bottom_right_radius");
    const int border_top_left_radius = glGetUniformLocation(material->base.program, "border_top_left_radius");
    const int border_top_right_radius = glGetUniformLocation(material->base.program, "border_top_right_radius");

    const int texture = glGetUniformLocation(material->base.program, "tex");
    const int texture_viewport = glGetUniformLocation(material->base.program, "tex_viewport");

    glUniform4f(color, material->color.x, material->color.y, material->color.z, material->color.w);
    glUniform1f(aspect, material->aspect);
    glUniform1f(r_clip, material->r_clip);

    glUniform1f(border_bottom_left_radius, material->border_bottom_left_radius);
    glUniform1f(border_bottom_right_radius, material->border_bottom_right_radius);
    glUniform1f(border_top_left_radius, material->border_top_left_radius);
    glUniform1f(border_top_right_radius, material->border_top_right_radius);

    if (material->texture && g_active_texture == material->texture || !material->texture && g_blank_texture && g_active_texture == g_blank_texture) return;

    glUniform1fv(texture_viewport, 4, material->texture_viewport);
    glUniform1i(texture, 0);
    glActiveTexture(GL_TEXTURE0);

    if (material->texture) {
        glBindTexture(GL_TEXTURE_2D, material->texture);
        g_active_texture = material->texture;
    } else {
        if (!g_blank_texture) {
            glCreateTextures(GL_TEXTURE_2D, 1, &g_blank_texture);
            glBindTexture(GL_TEXTURE_2D, g_blank_texture);

            const unsigned char blank[] = {255, 255, 255};
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 1, 1, 0, GL_RGB, GL_UNSIGNED_BYTE, blank);
        }

        glBindTexture(GL_TEXTURE_2D, g_blank_texture);
        g_active_texture = g_blank_texture;
    }
}

static MaterialVTable quad_material_vtable = { (void *) quad_material_update_uniforms };

QuadMaterial *quad_material_init() {
    QuadMaterial *material = calloc(1, sizeof(QuadMaterial));
    material->base.vtable = &quad_material_vtable;

    if (!shader_program) {
        char *vert_shader = concat(client_assets_path(), "shaders/quad.vert");
        char *frag_shader = concat(client_assets_path(), "shaders/quad.frag");

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
