#include <glad/glad.h>
#include <client.h>
#include <math.h>
#include <stdio.h>
#include <fast_obj.h>

unsigned long long create_cube_model() {
    static const vertex vertices[] = {
        // FRONT
        {-0.5f, 0.0f, 0.5f, 0.0f, 0.0f},
        {0.5f, 1.0f, 0.5f, 1.0f, 1.0f},
        {-0.5f, 1.0f, 0.5f, 0.0f, 1.0f},
        {0.5f, 0.0f, 0.5f, 1.0f, 0.0f},

        // BACK
        {0.5f, 0.0f, -0.5f, 0.0f, 0.0f},
        {-0.5f, 1.0f, -0.5f, 1.0f, 1.0f},
        {0.5f, 1.0f, -0.5f, 0.0f, 1.0f},
        {-0.5f, 0.0f, -0.5f, 1.0f, 0.0f},

        // LEFT
        {-0.5f, 0.0f, -0.5f, 0.0f, 0.0f},
        {-0.5f, 1.0f, 0.5f, 1.0f, 1.0f},
        {-0.5f, 1.0f, -0.5f, 0.0f, 1.0f},
        {-0.5f, 0.0f, 0.5f, 1.0f, 0.0f},

        // RIGHT
        {0.5f, 0.0f, 0.5f, 0.0f, 0.0f},
        {0.5f, 1.0f, -0.5f, 1.0f, 1.0f},
        {0.5f, 1.0f, 0.5f, 0.0f, 1.0f},
        {0.5f, 0.0f, -0.5f, 1.0f, 0.0f},

        // TOP
        {-0.5f, 1.0f, 0.5f, 0.0f, 0.0f},
        {0.5f, 1.0f, -0.5f, 1.0f, 1.0f},
        {-0.5f, 1.0f, -0.5f, 0.0f, 1.0f},
        {0.5f, 1.0f, 0.5f, 1.0f, 0.0f},

        // BOTTOM
        {-0.5f, 0.0f, -0.5f, 0.0f, 0.0f},
        {0.5f, 0.0f, 0.5f, 1.0f, 1.0f},
        {-0.5f, 0.0f, 0.5f, 0.0f, 1.0f},
        {0.5f, 0.0f, -0.5f, 1.0f, 0.0f},
    };

    static const unsigned int indices[] = {
        0, 1, 2, 0, 3, 1, // FRONT
        4, 5, 6, 4, 7, 5, // BACK
        8, 9, 10, 8, 11, 9, // LEFT
        12, 13, 14, 12, 15, 13, // RIGHT
        16, 17, 18, 16, 19, 17, // TOP
        20, 21, 22, 20, 23, 21, // BOTTOM
    };

    unsigned int vbo;
    unsigned int ebo;

    glGenBuffers(1, &vbo);
    glGenBuffers(1, &ebo);

    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

    return (unsigned long long) ebo << 32 | vbo;
}

unsigned long long load_obj_model(const char *path) {
    fastObjMesh *mesh = fast_obj_read(path);
    size_t idx = 0;

    size_t naive_vertex_count = 0;
    size_t alloc_vertex_count = 0;

    float min_y = 0.0f;

    for (size_t f = 0; f < mesh->face_count; f++) {
        const unsigned int fv = mesh->face_vertices[f];
        const unsigned int extra = fv - 3;

        naive_vertex_count += 3 + 3 * extra;
    }

    vertex *vertices = calloc(naive_vertex_count, sizeof(vertex));
    if (!vertices) return 0;

    for (size_t f = 0; f < mesh->face_count; f++) {
        const unsigned int fv = mesh->face_vertices[f];
        fastObjIndex anchor = {0};

        for (size_t v = 0; v < fv; v++) {
            const fastObjIndex indices = mesh->indices[idx];

            if (!v || v < 2) {
                if (!v) anchor = indices;

                idx++;
                continue;
            }

            const fastObjIndex prev = mesh->indices[idx-1];

            vertex v0 = {};
            vertex v1 = {};
            vertex v2 = {};

            v0.position.x = mesh->positions[anchor.p * 3];
            v0.position.y = mesh->positions[anchor.p * 3 + 1];
            v0.position.z = mesh->positions[anchor.p * 3 + 2];
            v0.tex_coord.x = mesh->texcoords[anchor.t * 2];
            v0.tex_coord.y = mesh->texcoords[anchor.t * 2 + 1];

            v1.position.x = mesh->positions[prev.p * 3];
            v1.position.y = mesh->positions[prev.p * 3 + 1];
            v1.position.z = mesh->positions[prev.p * 3 + 2];
            v1.tex_coord.x = mesh->texcoords[prev.t * 2];
            v1.tex_coord.y = mesh->texcoords[prev.t * 2 + 1];

            v2.position.x = mesh->positions[indices.p * 3];
            v2.position.y = mesh->positions[indices.p * 3 + 1];
            v2.position.z = mesh->positions[indices.p * 3 + 2];
            v2.tex_coord.x = mesh->texcoords[indices.t * 2];
            v2.tex_coord.y = mesh->texcoords[indices.t * 2 + 1];

            if (v0.position.y < min_y) min_y = v0.position.y;
            if (v1.position.y < min_y) min_y = v1.position.y;
            if (v2.position.y < min_y) min_y = v2.position.y;

            vertices[alloc_vertex_count] = v0;
            vertices[alloc_vertex_count + 1] = v1;
            vertices[alloc_vertex_count + 2] = v2;

            alloc_vertex_count += 3;
            idx++;
        }
    }

    for (size_t i = 0; i < naive_vertex_count; i++) {
        vertex *vertex = &vertices[i];
        vertex->position.y -= min_y;
    }

    unsigned int vbo;

    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, (long long) (naive_vertex_count * sizeof(vertex)), vertices, GL_STATIC_DRAW);

    free(vertices);
    fast_obj_destroy(mesh);

    return vbo;
}

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