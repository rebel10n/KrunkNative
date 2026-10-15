#include <client.h>
#include <math.h>
#include <string.h>

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

// One node of a transform chain on `in` (the child's matrix so far): out = T * R * S * in, R in the
// node's rotation order.
static void transform_apply(const MeshTransform *transform, const float *in, float *out) {
    float tmp[16];
    float tmp1[16];

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

    mat4x4(scale_matrix, in, tmp);

    // Intrinsic rotation by default (THREE.js style)
    if (transform->rotation_order == ROTATION_ORDER_INTRINSIC) {
        mat4x4(rotate_z_matrix, tmp, tmp1);
        mat4x4(rotate_y_matrix, tmp1, tmp);
        mat4x4(rotate_x_matrix, tmp, tmp1);
    } else if (transform->rotation_order == ROTATION_ORDER_YXZ) {
        // THREE.js Euler order "YXZ": R = Ry * Rx * Rz
        mat4x4(rotate_z_matrix, tmp, tmp1);
        mat4x4(rotate_x_matrix, tmp1, tmp);
        mat4x4(rotate_y_matrix, tmp, tmp1);
    } else {
        mat4x4(rotate_x_matrix, tmp, tmp1);
        mat4x4(rotate_y_matrix, tmp1, tmp);
        mat4x4(rotate_z_matrix, tmp, tmp1);
    }

    mat4x4(translate_matrix, tmp1, out);
}

static void snapshot_take(MeshTransformSnapshot *s, const MeshTransform *t) {
    memset(s, 0, sizeof(*s)); // padding too: snapshots are compared with memcmp
    s->node = t;
    s->parent = t->parent;
    s->rotation_order = t->rotation_order;
    s->position = t->position;
    s->rotation = t->rotation;
    s->scale = t->scale;
}

// 1 when every node of the chain still has the address and values transform_matrix was built from.
static int transform_cache_valid(const Mesh *mesh) {
    const int depth = mesh->_xf_cache_depth;
    if (depth <= 0 || depth > MESH_XF_CACHE_DEPTH) return 0;
    const MeshTransform *t = &mesh->transform;
    for (int i = 0; i < depth; i++, t = t->parent) {
        MeshTransformSnapshot now;
        if (!t) return 0;
        snapshot_take(&now, t);
        if (memcmp(&now, &mesh->_xf_cache[i], sizeof(now))) return 0;
    }
    return t == NULL; // the chain ended where it did
}

void mesh_update_transform_matrix(Mesh *mesh) {
    if (transform_cache_valid(mesh)) return;

    static const float identity[] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f,
    };

    memcpy(mesh->transform_matrix, identity, sizeof(identity));

    float tmp[16];
    int depth = 0;
    for (const MeshTransform *transform = &mesh->transform; transform; transform = transform->parent) {
        transform_apply(transform, mesh->transform_matrix, tmp);
        memcpy(mesh->transform_matrix, tmp, sizeof(tmp));
        if (depth < MESH_XF_CACHE_DEPTH) snapshot_take(&mesh->_xf_cache[depth], transform);
        depth++;
    }
    mesh->_xf_cache_depth = depth <= MESH_XF_CACHE_DEPTH ? depth : 0; // deeper chains: no cache
}

vec3 transform_point_to(const MeshTransform *node, const vec3 local, const MeshTransform *root) {
    float m[16] = {
        1.0f, 0.0f, 0.0f, local.x,
        0.0f, 1.0f, 0.0f, local.y,
        0.0f, 0.0f, 1.0f, local.z,
        0.0f, 0.0f, 0.0f, 1.0f,
    };
    float tmp[16];
    for (const MeshTransform *transform = node; transform && transform != root; transform = transform->parent) {
        transform_apply(transform, m, tmp);
        memcpy(m, tmp, sizeof(tmp));
    }
    return (vec3) {m[3], m[7], m[11]};
}

void mesh_fini(Mesh *mesh) {
    material_fini(mesh->material);
    free(mesh);
}
