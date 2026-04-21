#include <glad/glad.h>
#include <client.h>
#include <stdio.h>
#include <stdlib.h>

unsigned int shader_compile(const char *vertex_shader_path, const char *fragment_shader_path) {
    size_t vertex_shader_length;
    unsigned char *vertex_shader;

    size_t fragment_shader_length;
    unsigned char *fragment_shader;

    if (
        read_file(vertex_shader_path, &vertex_shader, &vertex_shader_length) ||
        read_file(fragment_shader_path, &fragment_shader, &fragment_shader_length)
    ) return 0;

    const unsigned int vert = glCreateShader(GL_VERTEX_SHADER);
    const unsigned int frag = glCreateShader(GL_FRAGMENT_SHADER);

    glShaderSource(vert, 1, (const char**) &vertex_shader, NULL);
    glShaderSource(frag, 1, (const char**) &fragment_shader, NULL);

    glCompileShader(vert);
    free(vertex_shader);

    int status;
    char error[512];

    glGetShaderiv(vert, GL_COMPILE_STATUS, &status);

    if (!status) {
        glGetShaderInfoLog(vert, sizeof(error), NULL, error);
        printf("vertex shader compile error (%s):\n %s \n", vertex_shader_path, error);

        glDeleteShader(vert);
        glDeleteShader(frag);
        free(fragment_shader);

        return 0;
    }

    glCompileShader(frag);
    free(fragment_shader);

    glGetShaderiv(frag, GL_COMPILE_STATUS, &status);

    if (!status) {
        glGetShaderInfoLog(frag, sizeof(error), NULL, error);
        printf("fragment shader compile error (%s):\n %s \n", fragment_shader_path, error);

        glDeleteShader(vert);
        glDeleteShader(frag);

        return 0;
    }

    const unsigned int program = glCreateProgram();

    glAttachShader(program, vert);
    glAttachShader(program, frag);

    glLinkProgram(program);

    glDeleteShader(vert);
    glDeleteShader(frag);

    glGetProgramiv(program, GL_LINK_STATUS, &status);

    if (!status) {
        glGetProgramInfoLog(program, sizeof(error), NULL, error);
        printf("shader link error (%s, %s):\n %s \n", vertex_shader_path, fragment_shader_path, error);

        glDeleteProgram(program);
        return 0;
    }

    return program;
}

void material_update_uniforms(Material *material) {
    material->vtable->update_uniforms(material);
}

void material_fini(Material *material) {
    free(material);
}