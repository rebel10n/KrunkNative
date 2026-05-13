#include <client.h>
#include <math.h>

Mesh *mesh_init(Geometry *geometry, Material *material) {
    Mesh *mesh = calloc(1, sizeof(Mesh));

    mesh->transform.scale.x = 1.0f;
    mesh->transform.scale.y = 1.0f;
    mesh->transform.scale.z = 1.0f;

    mesh->visible = 1;
    mesh->geometry = geometry;
    mesh->material = material;

    return mesh;
}

void mesh_update_transform_matrix(Mesh *mesh) {
    static const float identity[] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f,
    };

    memcpy(mesh->transform_matrix, identity, sizeof(identity));

    float tmp[16];
    float tmp1[16];

    const MeshTransform *transform = &mesh->transform;

    while (transform) {
        const float scale_matrix[] = {
            transform->scale.x, 0.0f, 0.0f, 0.0f,
            0.0f, transform->scale.y, 0.0f, 0.0f,
            0.0f, 0.0f, transform->scale.z, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f,
        };

        const float rotate_x_matrix[] = {
            1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, cosf(transform->rotation.x), -sinf(transform->rotation.x), 0.0f,
            0.0f, sinf(transform->rotation.x), cosf(transform->rotation.x), 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f,
        };

        const float rotate_y_matrix[] = {
            cosf(transform->rotation.y), 0.0f, sinf(transform->rotation.y), 0.0f,
            0.0f, 1.0f, 0.0f, 0.0f,
            -sinf(transform->rotation.y), 0.0f, cosf(transform->rotation.y), 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f,
        };

        const float rotate_z_matrix[] = {
            cosf(transform->rotation.z), -sinf(transform->rotation.z), 0.0f, 0.0f,
            sinf(transform->rotation.z), cosf(transform->rotation.z), 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f,
        };

        const float translate_matrix[] = {
            1.0f, 0.0f, 0.0f, transform->position.x,
            0.0f, 1.0f, 0.0f, transform->position.y,
            0.0f, 0.0f, 1.0f, transform->position.z,
            0.0f, 0.0f, 0.0f, 1.0f,
        };

        mat4x4(scale_matrix, mesh->transform_matrix, tmp);

        // Intrinsic rotation by default (THREE.js style)
        if (transform->rotation_order == ROTATION_ORDER_INTRINSIC) {
            mat4x4(rotate_z_matrix, tmp, tmp1);
            mat4x4(rotate_y_matrix, tmp1, tmp);
            mat4x4(rotate_x_matrix, tmp, tmp1);
        } else {
            mat4x4(rotate_x_matrix, tmp, tmp1);
            mat4x4(rotate_y_matrix, tmp1, tmp);
            mat4x4(rotate_z_matrix, tmp, tmp1);
        }

        mat4x4(translate_matrix, tmp1, mesh->transform_matrix);

        transform = transform->parent;
    }
}

void mesh_fini(Mesh *mesh) {
    material_fini(mesh->material);
    free(mesh);
}