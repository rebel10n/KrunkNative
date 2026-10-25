// Krunker v7.1.4 user assets (MapManager.addAsset -> render.js loadMesh / loadAnimMesh) from
// assets/user-assets/<id>/. See client/include/user_asset.h.
//
// Parity notes:
// - Only the base map, emissive map (e), alpha test (a) and lightmap (l) flags change the
//   picture: BasicMaterial has no normal / specular / metalness / roughness / displacement /
//   bump maps (n s m r p b are parsed and ignored, documented).
// - FBX assets (f) are not supported (no FBX loader in KN): no mesh.
// - Texture filtering: v7 uses LinearFilter unless "txs" (noSmooth); KN's texture cache is
//   nearest-filtered, so every asset texture is nearest (documented difference).
#include <user_asset.h>
#include <map_video.h>
#include <render_settings.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int user_asset_parse(const char *aid, UserAssetFlags *out) {
    memset(out, 0, sizeof(*out));
    if (!aid) return 0;
    size_t n = 0;
    for (const char *p = aid; *p; p++) {
        if (*p >= '0' && *p <= '9') {
            if (n + 1 < sizeof(out->id)) out->id[n++] = *p;
            continue;
        }
        switch (*p) { // MapManager.addAsset: (asset + "").includes(letter)
            case 'e': out->emissive = 1; break;
            case 'n': out->normal = 1; break;
            case 's': out->specular = 1; break;
            case 'a': out->alpha = 1; break;
            case 'g': out->gltf = 1; break;
            case 'q': out->glb = 1; break;
            case 'f': out->fbx = 1; break;
            case 'm': out->metal = 1; break;
            case 'r': out->rough = 1; break;
            case 'l': out->lightmap = 1; break;
            case 'p': out->displace = 1; break;
            case 'b': out->bump = 1; break;
            default: break;
        }
    }
    out->id[n] = 0;
    return n > 0;
}

static char *user_asset_path(const UserAssetFlags *flags, const char *file) {
    char relative[96];
    snprintf(relative, sizeof(relative), "user-assets/%s/%s", flags->id, file);
    return concat(assets_path(), relative);
}

// a texture of the asset: the missing-texture checker when it does not ship (as the map's user-asset
// textures, prefab.c), its placeholder while it downloads (asset_fetch.h); 0 only when the path cannot be built
static unsigned int user_asset_texture(const UserAssetFlags *flags, const char *file) {
    char *path = user_asset_path(flags, file);
    if (!path) return 0;
    FILE *f = ensure_asset_file(path) ? fopen(path, "rb") : NULL;
    unsigned int texture = 0;
    if (f) {
        fclose(f);
        texture = load_texture(path);
    } else if (asset_prefetch(path) == 0) {
        texture = load_texture(path); // still downloading (asset_fetch.h): its placeholder, filled when it arrives
    } else {
        texture = texture_checker(); // not shipped and not downloading: the checker
    }
    free(path);
    return texture;
}

int user_asset_load(const UserAssetFlags *flags, UserAssetModel *out) {
    memset(out, 0, sizeof(*out));
    if (!flags || !flags->id[0]) return 0;

    if (flags->gltf || flags->glb || flags->fbx) {
        char *path = user_asset_path(flags, flags->glb ? "model.glb" : flags->fbx ? "model.fbx" : "model.gltf");
        out->gltf = path ? gltf_model_load(path) : NULL;
        free(path);
        return out->gltf != NULL;
    }

    char *path = user_asset_path(flags, "model.obj");
    Geometry *geometry = path ? load_obj_model(path, 0) : NULL;
    free(path);
    if (!geometry) return 0;
    BasicMaterial *material = basic_material_init();
    if (!material) return 0;
    out->mesh = mesh_init(geometry, (Material *) material);

    if (!client_render_settings()->no_textures) {
        material->texture = user_asset_texture(flags, "model.png");
        if (flags->emissive) {
            material->emissive = (vec4) {1.0f, 1.0f, 1.0f, 1.0f};
            material->emissive_texture = user_asset_texture(flags, "model_e.png");
        }
        if (flags->lightmap) {
            material->light_map = user_asset_texture(flags, "model_l.png");
            material->light_map_intensity = 1.5f;
        }
    }
    if (flags->alpha) material->alpha_test = 0.02f;
    material->double_sided = 0;
    return 1;
}

// ---- GLTF map assets: carrier meshes and their instances

typedef struct {
    Mesh *carrier;
    GltfInstance *instance;
} UserAssetInstance;

static UserAssetInstance *g_assets;
static size_t g_asset_count;
static Scene *g_asset_scene;

static float user_asset_json_float(const cJSON *json, const float fallback) {
    if (cJSON_IsNumber(json)) return (float) cJSON_GetNumberValue(json);
    if (cJSON_IsString(json)) return (float) atof(cJSON_GetStringValue(json));
    return fallback;
}

static int user_asset_present(const cJSON *json) {
    return json && !cJSON_IsNull(json);
}

// ---- models that arrive after the map was built (asset_fetch.h): the map object's mesh exists from
// the start (an OBJ placeholder, a GLTF carrier without its instance) and gets its model when the file
// lands, like a late texture fills its placeholder (user_asset_arrived)

typedef struct {
    Object *object;     // the map object (the map's: valid until user_asset_map_unload)
    Mesh *mesh;         // OBJ: the mesh to re-centre; GLTF: the carrier
    char path[600];     // the model file being downloaded
    int gltf;
    float scale, alpha_test, opacity;
    int side;
} UserAssetPending;

static UserAssetPending *g_pending;
static size_t g_pending_count;

static void user_asset_pending_add(Object *object, Mesh *mesh, const char *path, const int gltf, const float scale,
                                   const float alpha_test, const float opacity, const int side) {
    UserAssetPending *grown = realloc(g_pending, (g_pending_count + 1) * sizeof(*grown));
    if (!grown) return;
    g_pending = grown;
    UserAssetPending *p = &g_pending[g_pending_count++];
    *p = (UserAssetPending) {object, mesh, "", gltf, scale, alpha_test, opacity, side};
    snprintf(p->path, sizeof(p->path), "%s", path);
}

// loadMesh(asset): the geometry re-centred on its bounding-box centre, placed at
// p + R * (0, s[1] / 2, 0) (similatePosRot with offsetH)
static void user_asset_place_obj(Mesh *mesh, const Object *object, const float scale) {
    const vec3 r = object->rotation;
    const float cx = cosf(r.x), sx = sinf(r.x), cy = cosf(r.y), sy = sinf(r.y), cz = cosf(r.z), sz = sinf(r.z);
    const float local_y = object->scale.y * 0.5f - mesh->geometry->bound_center[1] * scale;
    // second column of Rx * Ry * Rz
    const vec3 up = {-cy * sz, cx * cz - sx * sy * sz, sx * cz + cx * sy * sz};
    mesh->transform.position = (vec3) {object->position.x + up.x * local_y, object->position.y + up.y * local_y, object->position.z + up.z * local_y};
    mesh->transform.rotation = r;
    mesh->transform.scale = (vec3) {scale, scale, scale};
}

// a GLTF / GLB model's instance under its carrier (loadAnimMesh), registered for user_asset_map_*
static int user_asset_attach_gltf(Mesh *carrier, GltfModel *model, const Object *object, const float alpha_test,
                                  const float opacity, const int side, const int late) {
    const float anim_speed = object->anim_speed != 0.0f ? object->anim_speed : 1.0f;
    GltfInstance *instance = gltf_instance_create(model, anim_speed);
    if (!instance) return 0;
    UserAssetInstance *grown = realloc(g_assets, (g_asset_count + 1) * sizeof(UserAssetInstance));
    if (!grown) {
        gltf_instance_fini(instance);
        return 0;
    }
    g_assets = grown;
    instance->root.parent = &carrier->transform;
    for (size_t i = 0; i < instance->mesh_count; i++) {
        Mesh *mesh = instance->meshes[i];
        if (!mesh || mesh->material->vtable != &basic_material_vtable) continue;
        BasicMaterial *m = (BasicMaterial *) mesh->material;
        if (alpha_test > 0.0f) m->alpha_test = alpha_test;
        if (opacity != 1.0f) {
            m->color.w *= opacity;
            m->base.transparent = 1;
        }
        if (side == 2) m->double_sided = 1;
        // v10.0.3 model setup (deob-10.0.3.js:233656-233663): no env on a map asset, so every map takes
        // encodingTypes[texEncode || 0] (1 sRGB, else raw); asset noFog or the material's userData.noFog (gltf.c)
        m->texture_srgb = m->emissive_srgb = object->tex_encoding == 1;
        m->gltf_srgb_maps = 0;
        if (!object->fog_enabled) m->fog_enabled = 0;
    }
    if (object->anim_start) gltf_instance_play_clip(instance, object->anim_start, 0);
    g_assets[g_asset_count++] = (UserAssetInstance) {carrier, instance};
    if (late && g_asset_scene) scene_add_gltf(g_asset_scene, instance); // the map is in the scene already
    return 1;
}

Mesh *user_asset_object_mesh(Object *object, const cJSON *raw_obj, const vec4 *colors) {
    const cJSON *aid = cJSON_GetObjectItem(raw_obj, "aid");
    char aid_text[64];
    if (cJSON_IsString(aid)) snprintf(aid_text, sizeof(aid_text), "%s", cJSON_GetStringValue(aid));
    else if (cJSON_IsNumber(aid)) snprintf(aid_text, sizeof(aid_text), "%.0f", cJSON_GetNumberValue(aid));
    else return NULL;

    UserAssetFlags flags;
    if (!user_asset_parse(aid_text, &flags) || flags.fbx) return NULL;
    // the model file still downloading (asset_fetch.h): built when it arrives
    char *model_path = user_asset_path(&flags, flags.glb ? "model.glb" : flags.gltf ? "model.gltf" : "model.obj");
    const int downloading = model_path && asset_prefetch(model_path) == 0;
    UserAssetModel model;
    const int loaded = user_asset_load(&flags, &model);
    if (!loaded && !(downloading && (flags.gltf || flags.glb))) {
        free(model_path);
        return NULL;
    }

    // loadMesh / loadAnimMesh: rotation from r (x = r0), scale mSize || 1 (limitMM 0.01..100)
    const cJSON *ms = cJSON_GetObjectItem(raw_obj, "ms");
    float scale = user_asset_json_float(ms, 0.0f);
    scale = scale != 0.0f ? fminf(fmaxf(roundf(scale * 100.0f) / 100.0f, 0.01f), 100.0f) : 1.0f;
    const cJSON *v = cJSON_GetObjectItem(raw_obj, "v");
    const int visible = !user_asset_present(v);
    const cJSON *o = cJSON_GetObjectItem(raw_obj, "o");
    const float opacity = user_asset_present(o) ? user_asset_json_float(o, 1.0f) : 1.0f;
    vec4 color = prefab_raw_color(raw_obj, colors); // loadMesh color: colr || #ffffff
    color.w = opacity;
    // alphaTest: limitMM(parseInt(alt) || 0, 0, 1) || (a ? 0.02 : 0); v7 truncates "alt" with
    // parseInt (a bug, not reproduced: the float is used)
    const float alpha_test = object->alpha_test > 0.0f ? object->alpha_test : flags.alpha ? 0.02f : 0.0f;
    const int side = object->render_side > 0 ? object->render_side : 0; // limitMM(parseInt(rs) || 0, 0, 2)

    if (loaded && model.mesh) {
        Mesh *mesh = model.mesh;
        BasicMaterial *material = (BasicMaterial *) mesh->material;
        user_asset_place_obj(mesh, object, scale);
        // the OBJ loader's placeholder until the file lands: re-centred on the real model then
        if (downloading) user_asset_pending_add(object, mesh, model_path, 0, scale, alpha_test, opacity, side);
        free(model_path);
        mesh->visible = visible;
        material->color = color;
        material->alpha_test = alpha_test;
        material->base.transparent = object->force_trans || opacity != 1.0f;
        material->double_sided = side == 2;
        material->cull_front = side == 1;
        material->texture_srgb = object->tex_encoding == 1;
        material->fog_enabled = object->fog_enabled;
        return mesh;
    }

    // GLTF / GLB: loadAnimMesh places the model at p with rotation r and scale mSize (no re-centring)
    BasicMaterial *material = basic_material_init();
    Mesh *carrier = material ? mesh_init(gltf_empty_geometry(), (Material *) material) : NULL;
    if (!carrier) {
        free(model_path);
        return NULL;
    }
    carrier->transform.position = object->position;
    carrier->transform.rotation = object->rotation;
    carrier->transform.scale = (vec3) {scale, scale, scale};
    carrier->visible = visible;
    if (!loaded) {
        user_asset_pending_add(object, carrier, model_path, 1, scale, alpha_test, opacity, side);
        free(model_path);
        return carrier; // its instance comes with the file
    }
    free(model_path);
    if (!user_asset_attach_gltf(carrier, model.gltf, object, alpha_test, opacity, side, 0)) {
        mesh_fini(carrier);
        return NULL;
    }
    return carrier;
}

void user_asset_arrived(const char *path) {
    if (!path) return;
    for (size_t i = 0; i < g_pending_count;) {
        UserAssetPending *p = &g_pending[i];
        if (strcmp(p->path, path) != 0) {
            i++;
            continue;
        }
        if (p->gltf) {
            GltfModel *model = gltf_model_load(path);
            if (model) user_asset_attach_gltf(p->mesh, model, p->object, p->alpha_test, p->opacity, p->side, 1);
        } else if (p->mesh->geometry) {
            user_asset_place_obj(p->mesh, p->object, p->scale); // geometry_cache_reload rebuilt it in place
        }
        g_pending[i] = g_pending[--g_pending_count];
    }
}

// The instances live as long as their carriers (the map's object meshes: user_asset_mesh_freed), not
// as long as the map is in the scene: a new round on the same map (game_init's map_reset) keeps the
// meshes, unloads and loads the map again, and must find its models where they were.
void user_asset_map_load(Scene *scene) {
    g_asset_scene = scene;
    for (size_t i = 0; i < g_asset_count; i++) scene_add_gltf(scene, g_assets[i].instance);
    map_video_resume();
}

void user_asset_map_unload(Scene *scene) {
    for (size_t i = 0; i < g_asset_count; i++)
        if (scene) scene_remove_gltf(scene, g_assets[i].instance);
    g_asset_scene = NULL;
    map_video_park(); // no decoding while no map shows them
}

void user_asset_mesh_freed(const void *mesh) {
    if (!mesh) return;
    for (size_t i = 0; i < g_asset_count;) {
        if (g_assets[i].carrier != mesh) {
            i++;
            continue;
        }
        if (g_asset_scene) scene_remove_gltf(g_asset_scene, g_assets[i].instance);
        g_assets[i].instance->root.parent = NULL;
        gltf_instance_fini(g_assets[i].instance);
        g_assets[i] = g_assets[--g_asset_count];
    }
    for (size_t i = 0; i < g_pending_count;) {
        if (g_pending[i].mesh == mesh) g_pending[i] = g_pending[--g_pending_count];
        else i++;
    }
}

void user_asset_object_added(const void *carrier) {
    for (size_t i = 0; carrier && g_asset_scene && i < g_asset_count; i++)
        if (g_assets[i].carrier == carrier) scene_add_gltf(g_asset_scene, g_assets[i].instance);
}

void user_asset_frame(void) {
    const double now = gltf_clock();
    map_video_frame(now);
    for (size_t i = 0; i < g_asset_count; i++) {
        gltf_instance_set_visible(g_assets[i].instance, g_assets[i].carrier->visible);
        if (client_render_settings()->object_anim) gltf_instance_update(g_assets[i].instance, now);
    }
}

void user_asset_play_clip(const Mesh *carrier, const char *clip, const int reps) {
    if (!carrier || !clip) return;
    for (size_t i = 0; i < g_asset_count; i++) {
        if (g_assets[i].carrier == carrier) gltf_instance_play_clip(g_assets[i].instance, clip, reps);
    }
}
