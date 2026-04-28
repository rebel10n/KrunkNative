#include <glad/glad.h>
#include <client.h>
#include <math.h>
#include <stdio.h>

Mesh *mesh_init(const unsigned int vao, const unsigned int ebo, Material *material) {
    Mesh *mesh = calloc(1, sizeof(Mesh));

    mesh->scale.x = 1.0f;
    mesh->scale.y = 1.0f;
    mesh->scale.z = 1.0f;

    mesh->visible = 1;
    mesh->material = material;

    int ebo_size;

    glBindVertexArray(vao);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    glGetBufferParameteriv(GL_ELEMENT_ARRAY_BUFFER, GL_BUFFER_SIZE, &ebo_size);

    mesh->vao = vao;
    mesh->ebo = ebo;
    mesh->index_count = ebo_size / (int) sizeof(unsigned int);

    return mesh;
}

void mesh_update_transform_matrix(Mesh *mesh) {
    const float scale_matrix[] = {
        mesh->scale.x, 0.0f, 0.0f, 0.0f,
        0.0f, mesh->scale.y, 0.0f, 0.0f,
        0.0f, 0.0f, mesh->scale.z, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f,
    };

    const float rotate_x_matrix[] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, cosf(mesh->rotation.x), -sinf(mesh->rotation.x), 0.0f,
        0.0f, sinf(mesh->rotation.x), cosf(mesh->rotation.x), 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f,
    };

    const float rotate_y_matrix[] = {
        cosf(mesh->rotation.y), 0.0f, sinf(mesh->rotation.y), 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        -sinf(mesh->rotation.y), 0.0f, cosf(mesh->rotation.y), 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f,
    };

    const float rotate_z_matrix[] = {
        cosf(mesh->rotation.z), -sinf(mesh->rotation.z), 0.0f, 0.0f,
        sinf(mesh->rotation.z), cosf(mesh->rotation.z), 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f,
    };

    const float translate_matrix[] = {
        1.0f, 0.0f, 0.0f, mesh->position.x,
        0.0f, 1.0f, 0.0f, mesh->position.y,
        0.0f, 0.0f, 1.0f, mesh->position.z,
        0.0f, 0.0f, 0.0f, 1.0f,
    };

    float tmp[16];
    mat4x4(rotate_z_matrix, scale_matrix, tmp);

    float tmp1[16];
    mat4x4(rotate_y_matrix, tmp, tmp1);

    float tmp2[16];
    mat4x4(rotate_x_matrix, tmp1, tmp2);
    mat4x4(translate_matrix, tmp2, mesh->transform_matrix);
}

void mesh_fini(Mesh *mesh) {
    glDeleteBuffers(1, &mesh->ebo);
    glDeleteVertexArrays(1, &mesh->vao);

    free(mesh);
}