// mesh_cache_test: mesh_update_transform_matrix's cached world matrix (client/src/mesh.c) is the
// matrix a fresh computation gives after every kind of change: own position / rotation / scale /
// rotation order, a parent's values, a re-parent, a parent removed, a copied mesh, zeroed stack
// meshes and chains deeper than the cache.
//   mesh_cache_test
#include "../src/mesh.c"

#include <stdio.h>

void material_fini(Material *material) { (void) material; }

static int failures, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

// The matrix with no cache: a zeroed copy of the mesh (cache depth 0) on the same chain.
static void fresh(const Mesh *mesh, float out[16]) {
    Mesh copy;
    memset(&copy, 0, sizeof(copy));
    copy.transform = mesh->transform;
    mesh_update_transform_matrix(&copy);
    memcpy(out, copy.transform_matrix, sizeof(copy.transform_matrix));
}

static void expect_fresh(Mesh *mesh, const char *what) {
    mesh_update_transform_matrix(mesh);
    float want[16];
    fresh(mesh, want);
    CHECK(!memcmp(want, mesh->transform_matrix, sizeof(want)), "%s: cached matrix is stale", what);
}

static MeshTransform node(const float x, const float y, const float z) {
    MeshTransform t = {0};
    t.position = (vec3) {x, y, z};
    t.scale = (vec3) {1.0f, 1.0f, 1.0f};
    return t;
}

int main(void) {
    MeshTransform grand = node(5.0f, 0.0f, 0.0f), parent = node(0.0f, 2.0f, 0.0f), other = node(-3.0f, 0.0f, 1.0f);
    parent.parent = &grand;
    Mesh *mesh = mesh_init(NULL, NULL);
    mesh->transform.parent = &parent;
    mesh->transform.position = (vec3) {1.0f, 0.0f, 0.0f};

    expect_fresh(mesh, "first build");
    CHECK(mesh->_xf_cache_depth == 3, "three nodes cached (%d)", mesh->_xf_cache_depth);
    CHECK(mesh->transform_matrix[3] == 6.0f && mesh->transform_matrix[7] == 2.0f, "translation 6, 2 (%g, %g)",
          mesh->transform_matrix[3], mesh->transform_matrix[7]);
    expect_fresh(mesh, "unchanged");

    mesh->transform.position.z = 4.0f;
    expect_fresh(mesh, "own position");
    mesh->transform.rotation.y = 0.7f;
    expect_fresh(mesh, "own rotation");
    mesh->transform.scale.x = 2.0f;
    expect_fresh(mesh, "own scale");
    mesh->transform.rotation_order = ROTATION_ORDER_YXZ;
    mesh->transform.rotation.x = 0.3f;
    expect_fresh(mesh, "rotation order");
    parent.rotation.z = 1.1f;
    expect_fresh(mesh, "parent rotation");
    grand.position.y = -7.0f;
    expect_fresh(mesh, "grandparent position");
    CHECK(mesh->transform_matrix[7] != 2.0f, "the grandparent moved the mesh");
    mesh->transform.parent = &other;
    expect_fresh(mesh, "re-parented");
    CHECK(mesh->_xf_cache_depth == 2, "two nodes cached (%d)", mesh->_xf_cache_depth);
    other.parent = &grand;
    expect_fresh(mesh, "parent gained a parent");
    other.parent = NULL;
    mesh->transform.parent = NULL;
    expect_fresh(mesh, "parent removed");

    // a copied mesh: its cache names the original's node, so it rebuilds
    Mesh copy = *mesh;
    copy.transform.position.x = 9.0f;
    mesh_update_transform_matrix(&copy);
    CHECK(copy.transform_matrix[3] != mesh->transform_matrix[3], "a copied mesh with a new position rebuilds");

    // a chain deeper than the cache: never cached, always right
    MeshTransform chain[MESH_XF_CACHE_DEPTH + 2];
    for (int i = 0; i < MESH_XF_CACHE_DEPTH + 2; i++) {
        chain[i] = node(1.0f, 0.0f, 0.0f);
        chain[i].parent = i + 1 < MESH_XF_CACHE_DEPTH + 2 ? &chain[i + 1] : NULL;
    }
    Mesh deep;
    memset(&deep, 0, sizeof(deep));
    deep.transform = node(0.0f, 0.0f, 0.0f);
    deep.transform.parent = &chain[0];
    mesh_update_transform_matrix(&deep);
    CHECK(deep._xf_cache_depth == 0 && deep.transform_matrix[3] == (float) (MESH_XF_CACHE_DEPTH + 2), "deep chain: no cache, x %g",
          deep.transform_matrix[3]);
    chain[MESH_XF_CACHE_DEPTH + 1].position.x = 11.0f;
    mesh_update_transform_matrix(&deep);
    CHECK(deep.transform_matrix[3] == (float) (MESH_XF_CACHE_DEPTH + 12), "deep chain follows its root (x %g)", deep.transform_matrix[3]);

    mesh_fini(mesh);
    printf("mesh_cache_test: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
