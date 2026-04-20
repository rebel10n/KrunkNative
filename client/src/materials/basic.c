#include <glad/glad.h>
#include <client.h>
#include <stdlib.h>

static unsigned int shader_program = 0;
static unsigned int blank_texture = 0;

void basic_material_update_uniforms(BasicMaterial *material) {
    const int color = glGetUniformLocation(material->base.program, "color");
    const int tex_transform = glGetUniformLocation(material->base.program, "tex_transform");
    const int texture = glGetUniformLocation(material->base.program, "tex");

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

    glUniform4f(color, material->color.x, material->color.y, material->color.z, material->color.w);
    glUniformMatrix3fv(tex_transform, 1, GL_TRUE, transform);

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

    material->color.x = 1.0f;
    material->color.y = 1.0f;
    material->color.z = 1.0f;
    material->color.w = 1.0f;

    material->texture_repeat.x = 1.0f;
    material->texture_repeat.y = 1.0f;

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
