#include <glad/glad.h>
#include <client.h>
#include <math.h>
#include <stdio.h>
#include <stb_image.h>
#include <pcg_basic.h>

typedef enum {
    PREFAB_CUBE,
    PREFAB_CRATE,
    PREFAB_BARREL,
    PREFAB_LADDER,
    PREFAB_PLANE,
    PREFAB_SPAWN_POINT,
    PREFAB_CAMERA_POSITION,
    PREFAB_VEHICLE,
    PREFAB_STACK,
    PREFAB_RAMP,
    PREFAB_SCORE_ZONE,
    PREFAB_BILLBOARD,
    PREFAB_DEATH_ZONE,
    PREFAB_PARTICLES,
    PREFAB_OBJECTIVE,
    PREFAB_TREE,
    PREFAB_CONE,
    PREFAB_CONTAINER,
    PREFAB_GRASS,
    PREFAB_CONTAINER_R,
    PREFAB_ACID_BARREL,
    PREFAB_DOOR,
    PREFAB_WINDOW,
    PREFAB_FLAG,
    PREFAB_GATE,
    PREFAB_CHECK_POINT,
    PREFAB_WEAPON_PICKUP,
    PREFAB_TELEPORTER,
    PREFAB_TEDDY,
    PREFAB_TRIGGER,
    PREFAB_SIGN,
    PREFAB_DEPOSIT_BOX,
    PREFAB_LIGHT_CONE,
    PREFAB_SPECTATE_CAM,
    PREFAB_SPHERE,
    PREFAB_PLACEHOLDER,
    PREFAB_CARD_B,
    PREFAB_PALLET,
    PREFAB_LIQUID,
    PREFAB_SOUND_EMITTER,
    PREFAB_EVENT,
    PREFAB_TERMINAL,
    PREFAB_PREMIUM_ZONE,
    PREFAB_VERIFIED_ZONE,
    PREFAB_CUSTOM_ASSET,
    PREFAB_BOMB_SITE,
    PREFAB_BOOST_PAD,
    PREFAB_TEAM_ZONE,
    PREFAB_CYLINDER,
    PREFAB_POLICE,
    PREFAB_CAGE,
    PREFAB_E_BARREL,
    PREFAB_SHOWCASE,
    PREFAB_POINT_LIGHT,
    PREFAB_GHOST,
    PREFAB_GHOST_AI,
    PREFAB_PUMPKIN,
    PREFAB_RUNE,
    PREFAB_SKELETON,
    PREFAB_KNIGHT,
} Prefab;

typedef struct {
    const char *filename;
    const float scale;
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
    {"grass_0", 32.0f},
    {"containerr_0", 7.0f},
    {"acidbarrel_0", 4.0f},
    {"door_0", 5.0f},
    {"window_0", 6.0f},
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

const char *prefab_textures[] = {
    "wall",
    "dirt",
    "floor",
    "grid",
    "grey",
    "default",
    "roof",
    "flag",
    "grass",
    "check",
    "lines",
    "brick",
    "link",
    "liquid",
    "grain",
    "fabric",
    "tile",
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

    vec4 color = {1.0f, 1.0f, 1.0f, 1.0f};

    if (cJSON_IsString(raw_color)) parse_hex_color(cJSON_GetStringValue(raw_color), &color);
    else if (cJSON_IsNumber(raw_color_idx)) color = colors[(int) cJSON_GetNumberValue(raw_color_idx)];

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

        stbi_set_flip_vertically_on_load(1);

        unsigned int texture_id = 0;
        asset_cache_map_itr texture_cache_itr = vt_get(&g_texture_cache, full_texture_path);

        if (!vt_is_end(texture_cache_itr)) {
            texture_id = texture_cache_itr.data->val;
            free(full_texture_path);
        } else {
            int texture_width, texture_height, _;
            unsigned char *texture = stbi_load(full_texture_path, &texture_width, &texture_height, &_, 4);

            if (texture) {
                glGenTextures(1, &texture_id);
                glBindTexture(GL_TEXTURE_2D, texture_id);

                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, texture_width, texture_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, texture);
                glGenerateMipmap(GL_TEXTURE_2D);

                stbi_image_free(texture);
                vt_insert(&g_texture_cache, full_texture_path, texture_id);
            } else {
                free(full_texture_path);
            }
        }

        unsigned long long model = 0;
        asset_cache_map_itr model_cache_itr = vt_get(&g_model_cache, full_model_path);

        if (!vt_is_end(model_cache_itr)) {
            model = model_cache_itr.data->val;
            free(full_model_path);
        } else {
            model = load_obj_model(full_model_path);

            if (model) vt_insert(&g_model_cache, full_model_path, model);
            else free(full_model_path);
        }

        BasicMaterial *material = basic_material_init();
        Mesh *mesh = mesh_init((unsigned int) model, (unsigned int) (model >> 32), (Material *) material);

        mesh->visible = visible;
        mesh->position = object->position;
        mesh->rotation = rotation;
        mesh->scale.x = mesh->scale.y = mesh->scale.z = prefab_model.scale;

        material->texture = texture_id;
        material->color = color;

        // === DEBUG CODE START ===
        // mesh->material->wireframe = 1;
        // === DEBUG CODE END ===

        return mesh;
    }

    const cJSON *t = cJSON_GetObjectItem(raw_obj, "t");
    const int tex_id = cJSON_IsNumber(t) ? (int) cJSON_GetNumberValue(t) : 0;

    char *texture_path;

    if (object->prefab == PREFAB_BILLBOARD) {
        const cJSON *bb = cJSON_GetObjectItem(raw_obj, "bb");
        int billboard_id = cJSON_IsNumber(bb) ? (int) cJSON_GetNumberValue(bb) : 0;

        if (!billboard_id) billboard_id = (int) (pcg32_boundedrand(6) % 6) + 1;

        const int texture_path_length = snprintf(NULL, 0, "textures/pubs/b_%d.png", billboard_id);

        texture_path = malloc(texture_path_length + 1);
        snprintf(texture_path, texture_path_length + 1, "textures/pubs/b_%d.png", billboard_id);
    } else {
        const int texture_path_length = snprintf(NULL, 0, "textures/%s_%d.png", prefab_textures[tex_id], tex_id == 8);

        texture_path = malloc(texture_path_length + 1);
        snprintf(texture_path, texture_path_length + 1, "textures/%s_%d.png", prefab_textures[tex_id], tex_id == 8);
    }

    char *full_texture_path = concat(client_assets_path(), texture_path);
    free(texture_path);

    unsigned int texture_id = 0;
    asset_cache_map_itr texture_cache_itr = vt_get(&g_texture_cache, full_texture_path);

    if (!vt_is_end(texture_cache_itr)) {
        texture_id = texture_cache_itr.data->val;
        free(full_texture_path);
    } else if (tex_id != 5) {
        stbi_set_flip_vertically_on_load(1);

        int texture_width, texture_height, _;
        unsigned char *texture = stbi_load(full_texture_path, &texture_width, &texture_height, &_, 4);

        if (texture) {
            glGenTextures(1, &texture_id);
            glBindTexture(GL_TEXTURE_2D, texture_id);

            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, texture_width, texture_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, texture);
            glGenerateMipmap(GL_TEXTURE_2D);

            stbi_image_free(texture);
            vt_insert(&g_texture_cache, full_texture_path, texture_id);
        } else free(full_texture_path);
    } else free(full_texture_path);

    if (object->prefab == PREFAB_CUBE || object->prefab == PREFAB_PLANE || object->prefab == PREFAB_BILLBOARD) {
        if (object->prefab == PREFAB_CUBE && !g_cube_model) g_cube_model = create_cube_model();
        else if (!g_plane_model) g_plane_model = create_plane_model();

        const unsigned long long model = object->prefab == PREFAB_CUBE ? g_cube_model : g_plane_model;
        if (!model) return NULL;

        BasicMaterial *material = basic_material_init();
        Mesh *mesh = mesh_init((unsigned int) model, (unsigned int) (model >> 32), (Material *) material);

        mesh->visible = visible;
        mesh->position = object->position;
        mesh->rotation = rotation;
        mesh->scale = object->scale;

        material->texture = texture_id;
        material->color = color;

        material->use_face_tex_scaling = object->prefab != PREFAB_BILLBOARD;
        material->face_scale = object->scale;

        if (object->prefab != PREFAB_CUBE) {
            mesh->scale.y = 0.01f;

            material->face_scale.x = object->scale.x;
            material->face_scale.y = object->scale.z;
        }

        // === DEBUG CODE START ===
        // mesh->material->wireframe = 1;
        // === DEBUG CODE END ===

        return mesh;
    }

    return NULL;
}
