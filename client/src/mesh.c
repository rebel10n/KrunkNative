#include <client.h>
#include <math.h>

Mesh *mesh_init(Geometry *geometry, Material *material) {
    Mesh *mesh = calloc(1, sizeof(Mesh));

    mesh->scale.x = 1.0f;
    mesh->scale.y = 1.0f;
    mesh->scale.z = 1.0f;

    mesh->visible = 1;
    mesh->geometry = geometry;
    mesh->material = material;

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
    float tmp1[16];

    mat4x4(rotate_z_matrix, scale_matrix, tmp);
    mat4x4(rotate_y_matrix, tmp, tmp1);
    mat4x4(rotate_x_matrix, tmp1, tmp);
    mat4x4(translate_matrix, tmp, mesh->transform_matrix);

    const MeshTransform *parent = mesh->parent;

    while (parent) {
        const float parent_scale_matrix[] = {
            parent->scale.x, 0.0f, 0.0f, 0.0f,
            0.0f, parent->scale.y, 0.0f, 0.0f,
            0.0f, 0.0f, parent->scale.z, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f,
        };

        const float parent_rotate_x_matrix[] = {
            1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, cosf(parent->rotation.x), -sinf(parent->rotation.x), 0.0f,
            0.0f, sinf(parent->rotation.x), cosf(parent->rotation.x), 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f,
        };

        const float parent_rotate_y_matrix[] = {
            cosf(parent->rotation.y), 0.0f, sinf(parent->rotation.y), 0.0f,
            0.0f, 1.0f, 0.0f, 0.0f,
            -sinf(parent->rotation.y), 0.0f, cosf(parent->rotation.y), 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f,
        };

        const float parent_rotate_z_matrix[] = {
            cosf(parent->rotation.z), -sinf(parent->rotation.z), 0.0f, 0.0f,
            sinf(parent->rotation.z), cosf(parent->rotation.z), 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f,
        };

        const float parent_translate_matrix[] = {
            1.0f, 0.0f, 0.0f, parent->position.x,
            0.0f, 1.0f, 0.0f, parent->position.y,
            0.0f, 0.0f, 1.0f, parent->position.z,
            0.0f, 0.0f, 0.0f, 1.0f,
        };

        mat4x4(parent_scale_matrix, mesh->transform_matrix, tmp);
        mat4x4(parent_rotate_x_matrix, tmp, tmp1);
        mat4x4(parent_rotate_y_matrix, tmp1, tmp);
        mat4x4(parent_rotate_z_matrix, tmp, tmp1);
        mat4x4(parent_translate_matrix, tmp1, mesh->transform_matrix);

        parent = parent->parent;
    }
}

void mesh_fini(Mesh *mesh) {
    material_fini(mesh->material);
    free(mesh);
}