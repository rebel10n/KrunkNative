#include <glad/glad.h>
#include <client.h>
#include <stdlib.h>

static unsigned int shader_program = 0;

void basic_material_update_uniforms(BasicMaterial *material) {
    const int is_ramp = glGetUniformLocation(material->base.program, "is_ramp");
    const int use_face_tex_scaling = glGetUniformLocation(material->base.program, "use_face_tex_scaling");
    const int world_uv_scale = glGetUniformLocation(material->base.program, "world_uv_scale");
    const int face_scale = glGetUniformLocation(material->base.program, "face_scale");

    glUniform1i(is_ramp, material->is_ramp);
    glUniform1i(use_face_tex_scaling, material->use_face_tex_scaling);
    glUniform1f(world_uv_scale, game_constants.world_uv_scale);
    glUniform3f(face_scale, material->face_scale.x, material->face_scale.y, material->face_scale.z);

    const int color = glGetUniformLocation(material->base.program, "color");
    const int emissive = glGetUniformLocation(material->base.program, "emissive");
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
    glUniform4f(emissive, material->emissive.x, material->emissive.y, material->emissive.z, material->emissive.w);

    glUniformMatrix3fv(tex_transform, 1, GL_TRUE, transform);
    glUniform1i(texture, 0);

    glActiveTexture(GL_TEXTURE0);

    if (material->texture) {
        glBindTexture(GL_TEXTURE_2D, material->texture);
    } else {
        if (!g_blank_texture) {
            glCreateTextures(GL_TEXTURE_2D, 1, &g_blank_texture);
            glBindTexture(GL_TEXTURE_2D, g_blank_texture);

            const unsigned char blank[] = {255, 255, 255};
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 1, 1, 0, GL_RGB, GL_UNSIGNED_BYTE, blank);
        }

        glBindTexture(GL_TEXTURE_2D, g_blank_texture);
    }
}

MaterialVTable basic_material_vtable = { (void *) basic_material_update_uniforms };

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
