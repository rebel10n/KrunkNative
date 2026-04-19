#include <glad/glad.h>
#include <client.h>
#include <stdlib.h>

static unsigned int shader_program = 0;
static unsigned int blank_texture = 0;

void basic_material_update_uniforms(BasicMaterial *material) {
    const int color = glGetUniformLocation(material->base.program, "color");
    const int texture = glGetUniformLocation(material->base.program, "tex");

    glUniform4f(color, material->color.x, material->color.y, material->color.z, material->color.w);

    glUniform1i(texture, 0);
    glActiveTexture(GL_TEXTURE0);

    if (material->texture) {
        glBindTexture(GL_TEXTURE_2D, material->texture);
    } else {
        if (!blank_texture) {
            glCreateTextures(GL_TEXTURE_2D, 1, &blank_texture);
            glBindTexture(GL_TEXTURE_2D, blank_texture);

            const unsigned char blank[] = {255, 255, 255};
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 1, 1, 0, GL_RGB, GL_UNSIGNED_BYTE, blank);
        }

        glBindTexture(GL_TEXTURE_2D, blank_texture);
    }
}

static MaterialVTable basic_material_vtable = { (void *) basic_material_update_uniforms };

BasicMaterial *basic_material_init() {
    BasicMaterial *material = calloc(1, sizeof(BasicMaterial));
    material->base.vtable = &basic_material_vtable;

    if (!shader_program) {
        char *vert_shader = concat(client_assets_path(), "shaders/basic.vert");
        char *frag_shader = concat(client_assets_path(), "shaders/basic.frag");

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
