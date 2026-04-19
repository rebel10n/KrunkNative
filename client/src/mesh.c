#include <glad/glad.h>
#include <client.h>
#include <math.h>

Mesh *mesh_init(const vertex *vertices, const unsigned int *indices, const int vertex_count, const int index_count, Material *material) {
    if (!vertex_count) return NULL;

    Mesh *mesh = calloc(1, sizeof(Mesh));

    mesh->scale.x = 1.0f;
    mesh->scale.y = 1.0f;
    mesh->scale.z = 1.0f;

    mesh->material = material;

    glGenBuffers(1, &mesh->vbo);
    glGenBuffers(1, &mesh->ebo);

    glGenVertexArrays(1, &mesh->vao);
    glBindVertexArray(mesh->vao);

    glBindBuffer(GL_ARRAY_BUFFER, mesh->vbo);
    glBufferData(GL_ARRAY_BUFFER, (long long) sizeof(vertex) * vertex_count, vertices, GL_STATIC_DRAW);

    if (index_count) {
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh->ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, (long long) sizeof(unsigned int) * index_count, indices, GL_STATIC_DRAW);
    }

    mesh->vertex_count = vertex_count;
    mesh->index_count = index_count;

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), NULL);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *) (3 * sizeof(float)));

    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);

    if (index_count) glVertexArrayElementBuffer(mesh->vao, mesh->ebo);

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
        cosf(mesh->rotation.y), 0.0f, -sinf(mesh->rotation.y), 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        sinf(mesh->rotation.y), 0.0f, cosf(mesh->rotation.y), 0.0f,
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
    mat4x4(rotate_x_matrix, scale_matrix, tmp);

    float tmp1[16];
    mat4x4(rotate_y_matrix, tmp, tmp1);

    float tmp2[16];
    mat4x4(rotate_z_matrix, tmp1, tmp2);
    mat4x4(translate_matrix, tmp2, mesh->transform_matrix);
}

void mesh_fini(Mesh *mesh) {
    glDeleteBuffers(1, &mesh->vbo);
    glDeleteBuffers(1, &mesh->ebo);
    glDeleteVertexArrays(1, &mesh->vao);

    free(mesh);
}