#include <client.h>
#include <stdio.h>
#include <fast_obj.h>
#include <math.h>

typedef struct {
    const char *path;
    const float scale;
} PrefabModel;

const PrefabModel prefab_models[] = {
    {NULL}, // cube (software-generated)
    {"models/crate_0.obj", 6.0f},
    {"models/barrel_0.obj", 4.0f},
    {NULL}, // ladder (software-generated)
    {NULL}, // plane (software-generated)
    {NULL}, // spawnpoint
    {NULL}, // camera position
    {"models/vehicle_0.obj", 20.0f},
    {"models/stack_0.obj", 6.0f},
    {NULL}, // ramp (software-generated)
    {NULL}, // score zone
    {NULL}, // billboard (software-generated)
    {NULL}, // death zone
    {NULL}, // particles
    {NULL}, // objective
    {"models/tree_0.obj", 10.0f},
    {"models/cone_0.obj", 4.0f},
    {"models/container_0.obj", 7.0f},
    {"models/grass_0.obj", 32.0f},
    {"models/containerr_0.obj", 7.0f},
    {"models/acidbarrel_0.obj", 4.0f},
    {"models/door_0.obj", 5.0f},
    {"models/window_0.obj", 6.0f},
    {NULL}, // flag
    {NULL}, // gate
    {NULL}, // check point
    {NULL}, // weapon pickup (software-generated)
    {NULL}, // teleporter
    {"models/teddy_0.obj", 6.0f},
    {NULL}, // trigger
    {NULL}, // sign (software-generated)
    {NULL}, // deposit box
    {NULL}, // light cone
    {NULL}, // spectate cam
    {NULL}, // sphere (software-generated)
    {NULL}, // placeholder
    {"models/cardb_0.obj", 5.0f},
    {"models/pallet_0.obj", 6.0f},
    {NULL}, // liquid
    {NULL}, // sound emitter
    {NULL}, // event
    {NULL}, // terminal
    {NULL}, // premium zone
    {NULL}, // verified zone
    {NULL}, // custom asset
    {NULL}, // bomb site
    {NULL}, // boost pad (software-generated)
    {NULL}, // team zone
    {NULL}, // cylinder (software-generated)
    {"models/police_0.obj", 4.0f},
    {"models/cage_0.obj", 6.0f},
    {"models/ebarrel_0.obj", 4.0f},
    {NULL}, // showcase
    {NULL}, // point light
    {"models/ghost_0.obj", 4.0f},
    {NULL}, // bot (unsupported - for now at least)
    {"models/pumpkin_0.obj", 4.0f},
    {NULL}, // rune (software-generated)
    {"models/skeleton_0.obj", 4.0f},
    {"models/knight_0.obj", 4.0f},
};

Mesh *map_mesh_init(Object *object, const cJSON *raw_obj) {
    const PrefabModel prefab_model = prefab_models[object->prefab];

    if (prefab_model.path) {
        char *model_path = concat(client_assets_path(), prefab_model.path);
        printf("loading model path=%s for prefab=%u\n", model_path, object->prefab);

        fastObjMesh *model = fast_obj_read(model_path);
        free(model_path);

        vertex *vertices = calloc(model->index_count, sizeof(vertex));

        if (!vertices) {
            fast_obj_destroy(model);
            return NULL;
        }

        for (size_t i = 0; i < model->index_count; i++) {
            const fastObjIndex idx = model->indices[i];
            vertex vtx = {0};

            if (idx.p) {
                vtx.position.x = model->positions[idx.p * 3];
                vtx.position.y = model->positions[idx.p * 3 + 1];
                vtx.position.z = model->positions[idx.p * 3 + 2];
            }

            if (idx.t) {
                vtx.tex_coord.x = model->texcoords[idx.t * 2];
                vtx.tex_coord.y = model->texcoords[idx.t * 2 + 1];
            }

            vertices[i] = vtx;
        }

        BasicMaterial *material = basic_material_init();
        Mesh *mesh = mesh_init(vertices, NULL, (int) model->index_count, 0, (Material *) material);

        mesh->position = object->position;
        mesh->scale.x = mesh->scale.y = mesh->scale.z = prefab_model.scale;

        const cJSON *raw_rotation = cJSON_GetObjectItem(raw_obj, "r");

        if (cJSON_IsArray(raw_rotation)) {
            mesh->rotation.x = (float) cJSON_GetNumberValue(cJSON_GetArrayItem(raw_rotation, 0));
            mesh->rotation.y = (float) cJSON_GetNumberValue(cJSON_GetArrayItem(raw_rotation, 1));
            mesh->rotation.z = (float) cJSON_GetNumberValue(cJSON_GetArrayItem(raw_rotation, 2));

            if (mesh->rotation.x == NAN) mesh->rotation.x = 0.0f;
            if (mesh->rotation.y == NAN) mesh->rotation.y = 0.0f;
            if (mesh->rotation.z == NAN) mesh->rotation.z = 0.0f;
        }

        // === DEBUG CODE START ===
        mesh->material->wireframe = 1;

        material->color.x = 1.0f;
        material->color.y = 1.0f;
        material->color.z = 1.0f;
        material->color.w = 1.0f;
        // === DEBUG CODE END ===

        fast_obj_destroy(model);
        free(vertices);

        return mesh;
    }

    return NULL;
}
