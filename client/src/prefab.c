#include <glad/glad.h>
#include <client.h>
#include <math.h>
#include <stdio.h>
#include <stb_image.h>
#include <pcg_basic.h>

typedef struct {
    const char *filename;
    const float scale;
    const unsigned char transparent:1;
    const int frames;
    const float frame_time;
} PrefabModel;

const PrefabModel prefab_models[] = {
    {NULL}, // cube (software-generated)
    {"crate_0", 6.0f},
    {"barrel_0", 4.0f},
    {NULL}, // ladder (software-generated)
    {NULL}, // plane (software-generated)
    {NULL}, // spawnpoint
    {NULL}, // camera position
    {"vehicle_0", 20.0f},
    {"stack_0", 6.0f},
    {NULL}, // ramp (software-generated)
    {NULL}, // score zone
    {NULL}, // billboard (software-generated)
    {NULL}, // death zone
    {NULL}, // particles
    {NULL}, // objective
    {"tree_0", 10.0f},
    {"cone_0", 4.0f},
    {"container_0", 7.0f},
    {"grass_0", 32.0f, 1, 4, 0.180f},
    {"containerr_0", 7.0f},
    {"acidbarrel_0", 4.0f},
    {"door_0", 5.0f},
    {"window_0", 6.0f, 1},
    {NULL}, // flag
    {NULL}, // gate
    {NULL}, // check point
    {NULL}, // weapon pickup (software-generated)
    {NULL}, // teleporter
    {"teddy_0", 6.0f},
    {NULL}, // trigger
    {NULL}, // sign (software-generated)
    {NULL}, // deposit box
    {NULL}, // light cone
    {NULL}, // spectate cam
    {NULL}, // sphere (software-generated)
    {NULL}, // placeholder
    {"cardb_0", 5.0f},
    {"pallet_0", 6.0f},
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
    {"police_0", 4.0f},
    {"cage_0", 6.0f},
    {"ebarrel_0", 4.0f},
    {NULL}, // showcase
    {NULL}, // point light
    {"ghost_0", 4.0f},
    {NULL}, // bot (unsupported - for now at least)
    {"pumpkin_0", 4.0f},
    {NULL}, // rune (software-generated)
    {"skeleton_0", 4.0f},
    {"knight_0", 4.0f},
};

typedef struct {
    const char *name;
    const unsigned char transparent:1;
} PrefabTexture;

const PrefabTexture prefab_textures[] = {
    {"wall"},
    {"dirt"},
    {"floor"},
    {"grid"},
    {"grey"},
    {"default"},
    {"roof"},
    {"flag"},
    {"grass"},
    {"check"},
    {"lines"},
    {"brick"},
    {"link", 1},
    {"liquid", 1},
    {"grain"},
    {"fabric"},
    {"tile"},
};

Mesh *prefab_init(Object *object, const vec4 *colors, const cJSON *raw_obj) {
    if (object->prefab < 0 || object->prefab >= sizeof(prefab_models) / sizeof(prefab_models[0])) {
        return NULL;
    }

    const PrefabModel prefab_model = prefab_models[object->prefab];

    const cJSON *raw_visibility = cJSON_GetObjectItem(raw_obj, "v");
    const cJSON *raw_rotation = cJSON_GetObjectItem(raw_obj, "r");
    const cJSON *raw_color = cJSON_GetObjectItem(raw_obj, "c");
    const cJSON *raw_color_idx = cJSON_GetObjectItem(raw_obj, "ci");
    const cJSON *raw_emissive = cJSON_GetObjectItem(raw_obj, "e");
    const cJSON *raw_emissive_idx = cJSON_GetObjectItem(raw_obj, "ei");

    vec4 color = {1.0f, 1.0f, 1.0f, 1.0f};
    vec4 emissive = {1.0f, 1.0f, 1.0f, 1.0f};

    if (cJSON_IsString(raw_color)) parse_hex_color(cJSON_GetStringValue(raw_color), &color);
    else if (cJSON_IsNumber(raw_color_idx)) color = colors[(int) cJSON_GetNumberValue(raw_color_idx)];

    if (cJSON_IsString(raw_emissive)) parse_hex_color(cJSON_GetStringValue(raw_emissive), &emissive);
    else if (cJSON_IsNumber(raw_emissive_idx)) emissive = colors[(int) cJSON_GetNumberValue(raw_emissive_idx)];

    const int visible = cJSON_GetNumberValue(raw_visibility) != 1;
    vec3 rotation = {0};

    if (cJSON_IsArray(raw_rotation)) {
        rotation.x = (float) cJSON_GetNumberValue(cJSON_GetArrayItem(raw_rotation, 0));
        rotation.y = (float) cJSON_GetNumberValue(cJSON_GetArrayItem(raw_rotation, 1));
        rotation.z = (float) cJSON_GetNumberValue(cJSON_GetArrayItem(raw_rotation, 2));

        if (rotation.x == NAN) rotation.x = 0.0f;
        if (rotation.y == NAN) rotation.y = 0.0f;
        if (rotation.z == NAN) rotation.z = 0.0f;
    }

    if (prefab_model.frames) {
        TextureAnimation *tex_anim = object->tex_anim;

        if (!tex_anim) {
            tex_anim = calloc(1, sizeof(TextureAnimation));
            object->tex_anim = tex_anim;
        }

        if (tex_anim) {
            tex_anim->frames = prefab_model.frames;
            tex_anim->frame_time = prefab_model.frame_time;
        }
    }

    if (prefab_model.filename) {
        const int model_path_length = snprintf(NULL, 0, "models/%s.obj", prefab_model.filename);
        const int texture_path_length = snprintf(NULL, 0, "textures/%s.png", prefab_model.filename);

        char *model_path = malloc(model_path_length + 1);
        if (!model_path) return NULL;

        char *texture_path = malloc(texture_path_length + 1);

        if (!texture_path) {
            free(model_path);
            return NULL;
        }

        snprintf(model_path, model_path_length + 1, "models/%s.obj", prefab_model.filename);
        snprintf(texture_path, texture_path_length + 1, "textures/%s.png", prefab_model.filename);

        char *full_model_path = concat(client_assets_path(), model_path);
        char *full_texture_path = concat(client_assets_path(), texture_path);
        
        free(model_path);
        free(texture_path);

        const unsigned int texture_id = load_texture(full_texture_path);
        free(full_texture_path);

        Geometry *geometry = load_obj_model(full_model_path);
        free(full_model_path);

        if (!geometry) return NULL;

        BasicMaterial *material = basic_material_init();
        Mesh *mesh = mesh_init(geometry, (Material *) material);

        mesh->visible = visible;
        mesh->position = object->position;
        mesh->rotation = rotation;
        mesh->scale.x = mesh->scale.y = mesh->scale.z = prefab_model.scale;

        material->base.transparent = prefab_model.transparent;
        material->texture = texture_id;
        material->color = color;
        material->emissive = emissive;

        return mesh;
    }

    const cJSON *t = cJSON_GetObjectItem(raw_obj, "t");
    const int tex_id = cJSON_IsNumber(t) ? (int) cJSON_GetNumberValue(t) : 0;

    char *texture_path;

    if (object->prefab == PREFAB_BILLBOARD) {
        const cJSON *bb = cJSON_GetObjectItem(raw_obj, "bb");
        int billboard_id = cJSON_IsNumber(bb) ? (int) cJSON_GetNumberValue(bb) : 0;

        if (!billboard_id) billboard_id = (int) (pcg32_boundedrand(game_constants.billboard_count) % game_constants.billboard_count) + 1;

        const int texture_path_length = snprintf(NULL, 0, "textures/pubs/b_%d.png", billboard_id);

        texture_path = malloc(texture_path_length + 1);
        snprintf(texture_path, texture_path_length + 1, "textures/pubs/b_%d.png", billboard_id);
    } else {
        const int texture_path_length = snprintf(NULL, 0, "textures/%s_%d.png", prefab_textures[tex_id].name, tex_id == 8);

        texture_path = malloc(texture_path_length + 1);
        snprintf(texture_path, texture_path_length + 1, "textures/%s_%d.png", prefab_textures[tex_id].name, tex_id == 8);
    }

    char *full_texture_path = concat(client_assets_path(), texture_path);
    free(texture_path);

    const unsigned int texture_id = load_texture(full_texture_path);
    free(full_texture_path);

    const cJSON *ts = cJSON_GetObjectItem(raw_obj, "ts");
    const cJSON *td = cJSON_GetObjectItem(raw_obj, "td");

    if (cJSON_IsNumber(ts)) {
        TextureAnimation *tex_anim = object->tex_anim;

        if (!tex_anim) {
            tex_anim = calloc(1, sizeof(TextureAnimation));
            object->tex_anim = tex_anim;
        }

        if (tex_anim) {
            tex_anim->move = (float) cJSON_GetNumberValue(ts) / 10.0f;
            tex_anim->move_direction = cJSON_IsNumber(td) ? (int) cJSON_GetNumberValue(td) % 2 : 0;
        }
    }

    if (object->prefab == PREFAB_CUBE || object->prefab == PREFAB_PLANE || object->prefab == PREFAB_BILLBOARD) {
        Geometry *geometry = object->prefab == PREFAB_CUBE ? create_cube_geo() : create_plane_geo();
        if (!geometry) return NULL;

        BasicMaterial *material = basic_material_init();
        Mesh *mesh = mesh_init(geometry, (Material *) material);

        mesh->visible = visible;
        mesh->position = object->position;
        mesh->rotation = rotation;
        mesh->scale = object->scale;

        material->texture = texture_id;
        material->color = color;
        material->emissive = emissive;
        material->base.transparent = object->prefab != PREFAB_BILLBOARD && prefab_textures[tex_id].transparent;

        material->use_face_tex_scaling = object->prefab != PREFAB_BILLBOARD;
        material->face_scale = object->scale;

        if (object->prefab != PREFAB_CUBE) {
            mesh->scale.y = 0.01f;

            material->face_scale.x = object->scale.x;
            material->face_scale.y = object->scale.z;
        }

        return mesh;
    }

    if (object->prefab == PREFAB_RAMP) {
        Geometry *geometry = create_ramp_geo();
        if (!geometry) return NULL;

        BasicMaterial *material = basic_material_init();
        Mesh *mesh = mesh_init(geometry, (Material *) material);

        mesh->visible = visible;
        mesh->position = object->position;
        mesh->rotation.y = -(float) M_PI / 2.0f * (1.0f + (float) object->ramp->direction);
        mesh->scale.y = object->scale.y;

        mesh->scale.x = object->ramp->direction % 2 ? object->scale.x : object->scale.z;
        mesh->scale.z = object->ramp->direction % 2 ? object->scale.z : object->scale.x;

        material->texture = texture_id;
        material->color = color;
        material->emissive = emissive;

        material->is_ramp = 1;
        material->use_face_tex_scaling = 1;
        material->face_scale = mesh->scale;

        return mesh;
    }

    return NULL;
}
