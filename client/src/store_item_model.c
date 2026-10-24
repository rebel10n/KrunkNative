// A store item's model (client/include/store_item_model.h). Line numbers: deob-10.0.3.js.
#include <gl.h> // first: glad before any other GL header
#include <store_item_model.h>
#include <skin_fx.h>
#include "viewer/viewer_store.h" // viewer_item_keys
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *asset(const char *dir, const char *key, const char *ext) {
    const size_t n = strlen(assets_path()) + strlen(dir) + strlen(key) + strlen(ext) + 2;
    char *path = malloc(n);
    if (path) snprintf(path, n, "%s%s/%s%s", assets_path(), dir, key, ext);
    return path;
}

static unsigned int texture_for(const char *key) {
    char *path = asset("textures", key, ".png");
    const unsigned int texture = path ? load_texture(path) : 0;
    free(path);
    return texture;
}

static Geometry *model_for(const char *key) {
    char *path = asset("models", key, ".obj");
    Geometry *geometry = path ? load_obj_model(path, 0) : NULL;
    free(path);
    return geometry;
}

int store_item_model_build(const ViewerSkin *n, MeshTransform *parent, const vec3 position, const float rot_y, const float scale,
                           const int glow_text, const int emissive, const int no_tex, StoreItemModel *out) {
    memset(out, 0, sizeof(*out));
    if (!n) return 0;
    char model[128], texture[192];
    viewer_item_keys(n, model, sizeof(model), texture, sizeof(texture));

    if (n->flags & VS_HAS_ANIM) { // loadAnimMesh (fileExt gltf / glb)
        char rel[192];
        snprintf(rel, sizeof(rel), "models/%s.%s", model, n->file_ext ? n->file_ext : "gltf");
        char *path = concat(assets_path(), rel);
        GltfInstance *instance = path ? gltf_instance_create(gltf_model_load(path), n->anim_s > 0.0f ? n->anim_s : 1.0f) : NULL;
        free(path);
        if (instance) {
            instance->root.position = position;
            instance->root.rotation.y = rot_y;
            instance->root.scale = (vec3) {scale, scale, scale};
            instance->root.parent = parent;
            skin_fx_apply_gltf(instance, n, model);
            out->model = instance;
            return 1;
        }
    }

    // loadMesh: the OBJ with texSrc, else the model's own texture (none with noTex)
    Geometry *geometry = model_for(model);
    if (!geometry) return 0;
    unsigned int tex = 0;
    if (!no_tex) {
        tex = texture[0] ? texture_for(texture) : 0;
        if (!tex) tex = texture_for(model);
    }
    BasicMaterial *material = basic_material_init();
    if (!material) return 0;
    material->texture = tex;
    Mesh *mesh = mesh_init(geometry, (Material *) material);
    if (!mesh) {
        material_fini((Material *) material);
        return 0;
    }
    mesh->transform.position = position;
    mesh->transform.rotation.y = rot_y;
    mesh->transform.scale = (vec3) {scale, scale, scale};
    mesh->transform.parent = parent;
    skin_fx_apply(mesh, n, texture[0] ? texture : NULL, model, glow_text, emissive);
    out->mesh = mesh;
    return 1;
}

void store_item_model_fini(StoreItemModel *m) {
    if (!m) return;
    if (m->mesh) {
        skin_fx_forget(m->mesh);
        mesh_fini(m->mesh); // the material with it; the geometry and textures are the loaders' caches
    }
    if (m->model) {
        skin_fx_forget_gltf(m->model);
        gltf_instance_fini(m->model);
    }
    memset(m, 0, sizeof(*m));
}
