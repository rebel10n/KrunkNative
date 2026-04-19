#include <glad/glad.h>
#include <client.h>
#include <stdio.h>
#include <stdlib.h>

unsigned int shader_compile(const char *vertex_shader_path, const char *fragment_shader_path) {
    FILE *vertex_shader_file = fopen(vertex_shader_path, "rb");
    FILE *fragment_shader_file = fopen(fragment_shader_path, "rb");

    if (!vertex_shader_file || !fragment_shader_file) return 0;

    fseek(vertex_shader_file, 0, SEEK_END);
    fseek(fragment_shader_file, 0, SEEK_END);

    const size_t vertex_shader_length = ftell(vertex_shader_file);
    const size_t fragment_shader_length = ftell(fragment_shader_file);

    fseek(vertex_shader_file, 0, SEEK_SET);
    fseek(fragment_shader_file, 0, SEEK_SET);

    char *vertex_shader = malloc(vertex_shader_length + 1);
    char *fragment_shader = malloc(fragment_shader_length + 1);

    if (!vertex_shader || !fragment_shader) {
        if (vertex_shader) free(vertex_shader);
        if (fragment_shader) free(fragment_shader);

        fclose(vertex_shader_file);
        fclose(fragment_shader_file);

        return 0;
    }

    const size_t vertex_shader_read = fread(vertex_shader, 1, vertex_shader_length, vertex_shader_file);
    const size_t fragment_shader_read = fread(fragment_shader, 1, fragment_shader_length, fragment_shader_file);

    fclose(vertex_shader_file);
    fclose(fragment_shader_file);

    if (vertex_shader_read != vertex_shader_length || fragment_shader_read != fragment_shader_length) {
        free(vertex_shader);
        free(fragment_shader);

        return 0;
    }

    vertex_shader[vertex_shader_length] = 0;
    fragment_shader[fragment_shader_length] = 0;

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