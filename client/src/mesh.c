#include <glad/glad.h>
#include <client.h>
#include <math.h>
#include <stdio.h>

Mesh *mesh_init(const unsigned int vbo, const unsigned int ebo, Material *material) {
    Mesh *mesh = calloc(1, sizeof(Mesh));

    mesh->scale.x = 1.0f;
    mesh->scale.y = 1.0f;
    mesh->scale.z = 1.0f;

    mesh->visible = 1;
    mesh->material = material;

    glGenVertexArrays(1, &mesh->vao);
    glBindVertexArray(mesh->vao);

    int vbo_size;

    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glGetBufferParameteriv(GL_ARRAY_BUFFER, GL_BUFFER_SIZE, &vbo_size);

    mesh->vbo = vbo;
    mesh->vertex_count = vbo_size / (int) sizeof(vertex);

    if (ebo) {
        int ebo_size;

        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
        glGetBufferParameteriv(GL_ELEMENT_ARRAY_BUFFER, GL_BUFFER_SIZE, &ebo_size);

        mesh->ebo = ebo;
        mesh->index_count = ebo_size / (int) sizeof(unsigned int);
    }

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), NULL);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *) (3 * sizeof(float)));

    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);

    if (ebo) glVertexArrayElementBuffer(mesh->vao, mesh->ebo);

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
    glDeleteBuffers(1, &mesh->vbo);
    glDeleteBuffers(1, &mesh->ebo);
    glDeleteVertexArrays(1, &mesh->vao);

    free(mesh);
}