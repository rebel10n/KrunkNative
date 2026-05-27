#include <glad/glad.h>
#include <client.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
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
    const char *src;
    const unsigned char transparent:1;
} PrefabTexture;

const PrefabTexture prefab_textures[] = {
    {"WALL", "wall_0"},
    {"DIRT", "dirt_0"},
    {"FLOOR", "floor_0"},
    {"GRID", "grid_0"},
    {"GREY", "grey_0"},
    {"DEFAULT", "default"},
    {"ROOF", "roof_0"},
    {"FLAG", "flag_0"},
    {"GRASS", "grass_1"},
    {"CHECK", "check_0"},
    {"LINES", "lines_0"},
    {"BRICK", "brick_0"},
    {"LINK", "link_0", 1},
    {"LIQUID", "liquid_0", 1},
    {"GRAIN", "grain_0"},
    {"FABRIC", "fabric_0"},
    {"TILE", "tile_0"},
};

static int prefab_texture_string_equals(const char *a, const char *b) {
    if (!a || !b) return 0;

    while (*a && *b) {
        const char ca = *a >= 'a' && *a <= 'z' ? *a - 32 : *a;
        const char cb = *b >= 'a' && *b <= 'z' ? *b - 32 : *b;
        if (ca != cb) return 0;

        a++;
        b++;
    }

    return *a == *b;
}

static int prefab_texture_id_from_string(const char *name) {
    if (!name) return 0;

    char *end = NULL;
    const long parsed_id = strtol(name, &end, 10);
    if (end && *end == '\0') return (int) parsed_id;

    for (size_t i = 0; i < sizeof(prefab_textures) / sizeof(prefab_textures[0]); i++) {
        if (
            prefab_texture_string_equals(name, prefab_textures[i].name) ||
            prefab_texture_string_equals(name, prefab_textures[i].src)
        ) {
            return (int) i;
        }
    }

    return 0;
}

static int prefab_face_mask(const cJSON *raw_faces) {
    if (!cJSON_IsString(raw_faces)) return 0x3f;

    const long bits = strtol(cJSON_GetStringValue(raw_faces), NULL, 16);
    int mask = 0;

    for (int i = 0; i < 6; i++) {
        if (bits & (1 << (5 - i))) mask |= 1 << i;
    }

    return mask;
}

static int prefab_hidden_in_game(const int prefab) {
    switch (prefab) {
        case PREFAB_SPAWN_POINT:
        case PREFAB_CAMERA_POSITION:
        case PREFAB_SCORE_ZONE:
        case PREFAB_DEATH_ZONE:
        case PREFAB_PARTICLES:
        case PREFAB_OBJECTIVE:
        case PREFAB_FLAG:
        case PREFAB_CHECK_POINT:
        case PREFAB_WEAPON_PICKUP:
        case PREFAB_TELEPORTER:
        case PREFAB_TRIGGER:
        case PREFAB_SPECTATE_CAM:
        case PREFAB_PLACEHOLDER:
        case PREFAB_SOUND_EMITTER:
        case PREFAB_EVENT:
        case PREFAB_PREMIUM_ZONE:
        case PREFAB_VERIFIED_ZONE:
        case PREFAB_CUSTOM_ASSET:
        case PREFAB_BOMB_SITE:
        case PREFAB_TEAM_ZONE:
        case PREFAB_SHOWCASE:
        case PREFAB_POINT_LIGHT:
            return 1;
        default:
            return 0;
    }
}

static void prefab_apply_lighting_controls(BasicMaterial *material, const Object *object) {
    material->ambient_enabled = object->ambient_enabled;
    material->fog_enabled = object->fog_enabled;
}

static vec3 prefab_rotated_up_normal(const vec3 rotation) {
    const float sx = sinf(rotation.x);
    const float cx = cosf(rotation.x);
    const float sy = sinf(rotation.y);
    const float cy = cosf(rotation.y);
    const float sz = sinf(rotation.z);
    const float cz = cosf(rotation.z);

    vec3 normal = {-sz, cz, 0.0f};
    normal = (vec3) {cy * normal.x + sy * normal.z, normal.y, -sy * normal.x + cy * normal.z};
    normal = (vec3) {normal.x, cx * normal.y - sx * normal.z, sx * normal.y + cx * normal.z};

    const float length = sqrtf(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
    if (length > 0.0f) {
        normal.x /= length;
        normal.y /= length;
        normal.z /= length;
    }

    return normal;
}

static Geometry *prefab_cube_geo(const int face_mask) {
    static Geometry *cached[64];
    static const vertex vertices[] = {
        // FRONT (Krunker face 4)
        {-0.5f, 0.0f, 0.5f, 0.0f, 0.0f},
        {0.5f, 1.0f, 0.5f, 1.0f, 1.0f},
        {-0.5f, 1.0f, 0.5f, 0.0f, 1.0f},
        {0.5f, 0.0f, 0.5f, 1.0f, 0.0f},

        // BACK (Krunker face 5)
        {0.5f, 0.0f, -0.5f, 0.0f, 0.0f},
        {-0.5f, 1.0f, -0.5f, 1.0f, 1.0f},
        {0.5f, 1.0f, -0.5f, 0.0f, 1.0f},
        {-0.5f, 0.0f, -0.5f, 1.0f, 0.0f},

        // LEFT (Krunker face 1)
        {-0.5f, 0.0f, -0.5f, 0.0f, 0.0f},
        {-0.5f, 1.0f, 0.5f, 1.0f, 1.0f},
        {-0.5f, 1.0f, -0.5f, 0.0f, 1.0f},
        {-0.5f, 0.0f, 0.5f, 1.0f, 0.0f},

        // RIGHT (Krunker face 0)
        {0.5f, 0.0f, 0.5f, 0.0f, 0.0f},
        {0.5f, 1.0f, -0.5f, 1.0f, 1.0f},
        {0.5f, 1.0f, 0.5f, 0.0f, 1.0f},
        {0.5f, 0.0f, -0.5f, 1.0f, 0.0f},

        // TOP (Krunker face 2)
        {-0.5f, 1.0f, 0.5f, 0.0f, 0.0f},
        {0.5f, 1.0f, -0.5f, 1.0f, 1.0f},
        {-0.5f, 1.0f, -0.5f, 0.0f, 1.0f},
        {0.5f, 1.0f, 0.5f, 1.0f, 0.0f},

        // BOTTOM (Krunker face 3)
        {-0.5f, 0.0f, -0.5f, 0.0f, 0.0f},
        {0.5f, 0.0f, 0.5f, 1.0f, 1.0f},
        {-0.5f, 0.0f, 0.5f, 0.0f, 1.0f},
        {0.5f, 0.0f, -0.5f, 1.0f, 0.0f},
    };
    static const unsigned int face_indices[][6] = {
        {0, 1, 2, 0, 3, 1},
        {4, 5, 6, 4, 7, 5},
        {8, 9, 10, 8, 11, 9},
        {12, 13, 14, 12, 15, 13},
        {16, 17, 18, 16, 19, 17},
        {20, 21, 22, 20, 23, 21},
    };
    static const int krunker_to_native[] = {3, 2, 4, 5, 0, 1};

    if (face_mask == 0x3f) return create_cube_geo();
    if (face_mask >= 0 && face_mask < 64 && cached[face_mask]) return cached[face_mask];

    unsigned int indices[36];
    size_t index_count = 0;

    for (int i = 0; i < 6; i++) {
        if (!(face_mask & (1 << i))) continue;

        const int native_face = krunker_to_native[i];
        memcpy(indices + index_count, face_indices[native_face], sizeof(face_indices[native_face]));
        index_count += 6;
    }

    Geometry *geometry = calloc(1, sizeof(Geometry));
    if (!geometry) return NULL;

    unsigned int vbo;
    glGenVertexArrays(1, &geometry->vao);
    glGenBuffers(1, &geometry->ebo);
    glGenBuffers(1, &vbo);

    glBindVertexArray(geometry->vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, geometry->ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (int) (index_count * sizeof(unsigned int)), indices, GL_STATIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), NULL);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *) (3 * sizeof(float)));
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexArrayElementBuffer(geometry->vao, geometry->ebo);

    geometry->index_count = (int) index_count;
    if (face_mask >= 0 && face_mask < 64) cached[face_mask] = geometry;
    return geometry;
}

static Geometry *prefab_cylinder_geo(const int segments, const int cone) {
    static Geometry *cached_cylinder;
    static Geometry *cached_cone;

    if (!cone && cached_cylinder) return cached_cylinder;
    if (cone && cached_cone) return cached_cone;

    const int side_vertices = segments * 4;
    const int cap_vertices = cone ? 0 : (segments + 1) * 2;
    const int vertex_count = side_vertices + cap_vertices;
    const int index_count = segments * 6 + (cone ? 0 : segments * 6);

    vertex *vertices = calloc(vertex_count, sizeof(vertex));
    unsigned int *indices = calloc(index_count, sizeof(unsigned int));

    if (!vertices || !indices) {
        free(vertices);
        free(indices);
        return NULL;
    }

    int v = 0;
    int idx = 0;

    for (int i = 0; i < segments; i++) {
        const float a0 = (float) i / (float) segments * 2.0f * (float) M_PI;
        const float a1 = (float) (i + 1) / (float) segments * 2.0f * (float) M_PI;
        const float x0 = cosf(a0) * 0.5f;
        const float z0 = sinf(a0) * 0.5f;
        const float x1 = cosf(a1) * 0.5f;
        const float z1 = sinf(a1) * 0.5f;
        const float u0 = (float) i / (float) segments;
        const float u1 = (float) (i + 1) / (float) segments;
        const int base = v;

        vertices[v++] = (vertex) {x0, 0.0f, z0, u0, 0.0f};
        vertices[v++] = (vertex) {cone ? 0.0f : x0, 1.0f, cone ? 0.0f : z0, u0, 1.0f};
        vertices[v++] = (vertex) {cone ? 0.0f : x1, 1.0f, cone ? 0.0f : z1, u1, 1.0f};
        vertices[v++] = (vertex) {x1, 0.0f, z1, u1, 0.0f};

        indices[idx++] = base;
        indices[idx++] = base + 1;
        indices[idx++] = base + 2;
        indices[idx++] = base;
        indices[idx++] = base + 2;
        indices[idx++] = base + 3;
    }

    if (!cone) {
        const int bottom_center = v;
        vertices[v++] = (vertex) {0.0f, 0.0f, 0.0f, 0.5f, 0.5f};

        for (int i = 0; i < segments; i++) {
            const float a = (float) i / (float) segments * 2.0f * (float) M_PI;
            vertices[v++] = (vertex) {cosf(a) * 0.5f, 0.0f, sinf(a) * 0.5f, cosf(a) * 0.5f + 0.5f, sinf(a) * 0.5f + 0.5f};
        }

        for (int i = 0; i < segments; i++) {
            indices[idx++] = bottom_center;
            indices[idx++] = bottom_center + 1 + ((i + 1) % segments);
            indices[idx++] = bottom_center + 1 + i;
        }

        const int top_center = v;
        vertices[v++] = (vertex) {0.0f, 1.0f, 0.0f, 0.5f, 0.5f};

        for (int i = 0; i < segments; i++) {
            const float a = (float) i / (float) segments * 2.0f * (float) M_PI;
            vertices[v++] = (vertex) {cosf(a) * 0.5f, 1.0f, sinf(a) * 0.5f, cosf(a) * 0.5f + 0.5f, sinf(a) * 0.5f + 0.5f};
        }

        for (int i = 0; i < segments; i++) {
            indices[idx++] = top_center;
            indices[idx++] = top_center + 1 + i;
            indices[idx++] = top_center + 1 + ((i + 1) % segments);
        }
    }

    Geometry *geometry = calloc(1, sizeof(Geometry));
    if (!geometry) {
        free(vertices);
        free(indices);
        return NULL;
    }

    unsigned int vbo;
    glGenVertexArrays(1, &geometry->vao);
    glGenBuffers(1, &geometry->ebo);
    glGenBuffers(1, &vbo);

    glBindVertexArray(geometry->vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, vertex_count * sizeof(vertex), vertices, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, geometry->ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, index_count * sizeof(unsigned int), indices, GL_STATIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), NULL);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *) (3 * sizeof(float)));
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexArrayElementBuffer(geometry->vao, geometry->ebo);

    geometry->index_count = index_count;
    if (cone) cached_cone = geometry;
    else cached_cylinder = geometry;

    free(vertices);
    free(indices);
    return geometry;
}

static Geometry *prefab_sphere_geo() {
    static Geometry *cached;
    if (cached) return cached;

    const int rings = 12;
    const int segments = 24;
    const int vertex_count = (rings + 1) * (segments + 1);
    const int index_count = rings * segments * 6;

    vertex *vertices = calloc(vertex_count, sizeof(vertex));
    unsigned int *indices = calloc(index_count, sizeof(unsigned int));

    if (!vertices || !indices) {
        free(vertices);
        free(indices);
        return NULL;
    }

    int v = 0;
    for (int y = 0; y <= rings; y++) {
        const float vy = (float) y / (float) rings;
        const float phi = vy * (float) M_PI;

        for (int x = 0; x <= segments; x++) {
            const float vx = (float) x / (float) segments;
            const float theta = vx * 2.0f * (float) M_PI;
            vertices[v++] = (vertex) {
                sinf(phi) * cosf(theta) * 0.5f,
                0.5f + cosf(phi) * 0.5f,
                sinf(phi) * sinf(theta) * 0.5f,
                vx,
                vy,
            };
        }
    }

    int idx = 0;
    for (int y = 0; y < rings; y++) {
        for (int x = 0; x < segments; x++) {
            const int a = y * (segments + 1) + x;
            const int b = a + segments + 1;

            indices[idx++] = a;
            indices[idx++] = b;
            indices[idx++] = a + 1;
            indices[idx++] = a + 1;
            indices[idx++] = b;
            indices[idx++] = b + 1;
        }
    }

    Geometry *geometry = calloc(1, sizeof(Geometry));
    if (!geometry) {
        free(vertices);
        free(indices);
        return NULL;
    }

    unsigned int vbo;
    glGenVertexArrays(1, &geometry->vao);
    glGenBuffers(1, &geometry->ebo);
    glGenBuffers(1, &vbo);

    glBindVertexArray(geometry->vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, vertex_count * sizeof(vertex), vertices, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, geometry->ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, index_count * sizeof(unsigned int), indices, GL_STATIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), NULL);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *) (3 * sizeof(float)));
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexArrayElementBuffer(geometry->vao, geometry->ebo);

    geometry->index_count = index_count;
    cached = geometry;

    free(vertices);
    free(indices);
    return geometry;
}

Mesh *prefab_init(Object *object, const vec4 *colors, const cJSON *raw_obj) {
    if (object->prefab < 0 || object->prefab >= sizeof(prefab_models) / sizeof(prefab_models[0])) {
        return NULL;
    }
    if (prefab_hidden_in_game(object->prefab)) return NULL;

    const PrefabModel prefab_model = prefab_models[object->prefab];

    const cJSON *raw_visibility = cJSON_GetObjectItem(raw_obj, "v");
    const cJSON *raw_rotation = cJSON_GetObjectItem(raw_obj, "r");
    const cJSON *raw_color = cJSON_GetObjectItem(raw_obj, "c");
    const cJSON *raw_color_idx = cJSON_GetObjectItem(raw_obj, "ci");
    const cJSON *raw_emissive = cJSON_GetObjectItem(raw_obj, "e");
    const cJSON *raw_emissive_idx = cJSON_GetObjectItem(raw_obj, "ei");
    const cJSON *raw_opacity = cJSON_GetObjectItem(raw_obj, "o");
    const cJSON *raw_model_size = cJSON_GetObjectItem(raw_obj, "ms");

    const float opacity = cJSON_IsNumber(raw_opacity) ? (float) cJSON_GetNumberValue(raw_opacity) : 1.0f;

    vec4 color = {1.0f, 1.0f, 1.0f, 1.0f};
    vec4 emissive = {0.0f, 0.0f, 0.0f, 0.0f};

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

        char *full_model_path = concat(assets_path(), model_path);
        char *full_texture_path = concat(assets_path(), texture_path);

        free(model_path);
        free(texture_path);

        const unsigned int texture_id = load_texture(full_texture_path);
        free(full_texture_path);

        Geometry *geometry = load_obj_model(full_model_path, 1);
        free(full_model_path);

        if (!geometry) return NULL;

        BasicMaterial *material = basic_material_init();
        Mesh *mesh = mesh_init(geometry, (Material *) material);

        mesh->visible = visible;
        mesh->transform.position = object->position;
        mesh->transform.rotation = rotation;
        mesh->transform.scale.x = mesh->transform.scale.y = mesh->transform.scale.z = cJSON_IsNumber(raw_model_size) ? (float) cJSON_GetNumberValue(raw_model_size) : prefab_model.scale;

        material->base.transparent = prefab_model.transparent || opacity != 1.0f;
        material->texture = texture_id;
        material->color = color;
        material->color.w = opacity;
        material->emissive = emissive;
        prefab_apply_lighting_controls(material, object);

        return mesh;
    }

    const cJSON *t = cJSON_GetObjectItem(raw_obj, "t");
    int tex_id = cJSON_IsString(t) ? prefab_texture_id_from_string(cJSON_GetStringValue(t)) : cJSON_IsNumber(t) ? (int) cJSON_GetNumberValue(t) : 0;
    if (tex_id < 0 || tex_id >= (int) (sizeof(prefab_textures) / sizeof(prefab_textures[0]))) tex_id = object->prefab == PREFAB_LADDER ? 2 : 0;

    char *texture_path = NULL;

    if (object->prefab == PREFAB_LIGHT_CONE) {
        const int cone_frame = cJSON_GetObjectItem(raw_obj, "fc") == NULL ? 0 : 1;
        const int texture_path_length = snprintf(NULL, 0, "textures/lightcone_%d.png", cone_frame);

        texture_path = malloc(texture_path_length + 1);
        snprintf(texture_path, texture_path_length + 1, "textures/lightcone_%d.png", cone_frame);
    } else if (object->prefab == PREFAB_BILLBOARD) {
        const cJSON *bb = cJSON_GetObjectItem(raw_obj, "bb");
        int billboard_id = cJSON_IsNumber(bb) ? (int) cJSON_GetNumberValue(bb) : 0;

        if (!billboard_id) billboard_id = (int) (pcg32_boundedrand(game_constants.billboard_count) % game_constants.billboard_count) + 1;

        const int texture_path_length = snprintf(NULL, 0, "textures/pubs/b_%d.png", billboard_id);

        texture_path = malloc(texture_path_length + 1);
        snprintf(texture_path, texture_path_length + 1, "textures/pubs/b_%d.png", billboard_id);
    } else {
        const char *src = prefab_textures[tex_id].src;

        if (strcmp(src, "default")) {
            const int texture_path_length = snprintf(NULL, 0, "textures/%s.png", src);

            texture_path = malloc(texture_path_length + 1);
            snprintf(texture_path, texture_path_length + 1, "textures/%s.png", src);
        }
    }

    unsigned int texture_id = 0;

    if (texture_path) {
        char *full_texture_path = concat(assets_path(), texture_path);
        free(texture_path);

        texture_id = load_texture(full_texture_path);
        free(full_texture_path);
    }

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

    if (object->prefab == PREFAB_CUBE || object->prefab == PREFAB_PLANE || object->prefab == PREFAB_BILLBOARD || object->prefab == PREFAB_SIGN || object->prefab == PREFAB_LIQUID || object->prefab == PREFAB_RUNE || object->prefab == PREFAB_BOOST_PAD) {
        Geometry *geometry = object->prefab == PREFAB_CUBE ? prefab_cube_geo(prefab_face_mask(cJSON_GetObjectItem(raw_obj, "f"))) : create_plane_geo();
        if (!geometry) return NULL;

        BasicMaterial *material = basic_material_init();
        Mesh *mesh = mesh_init(geometry, (Material *) material);

        mesh->visible = visible;
        mesh->transform.position = object->position;
        mesh->transform.rotation = rotation;
        mesh->transform.scale = object->scale;

        material->texture = texture_id;
        material->color = color;
        material->color.w = opacity;
        material->emissive = emissive;
        material->base.transparent = (object->prefab != PREFAB_BILLBOARD && prefab_textures[tex_id].transparent) || opacity != 1.0f || object->prefab == PREFAB_LIQUID || object->prefab == PREFAB_RUNE;
        prefab_apply_lighting_controls(material, object);

        material->use_face_tex_scaling = object->prefab != PREFAB_BILLBOARD;
        material->face_scale = object->scale;

        if (object->prefab != PREFAB_CUBE) {
            if (object->prefab != PREFAB_BILLBOARD) {
                material->use_flat_normal = 1;
                material->flat_normal = prefab_rotated_up_normal(rotation);
            }

            mesh->transform.scale.y = 0.01f;

            material->face_scale.x = object->scale.x;
            material->face_scale.y = object->scale.z;
        }

        return mesh;
    }

    if (object->prefab == PREFAB_CYLINDER || object->prefab == PREFAB_SPHERE || object->prefab == PREFAB_LIGHT_CONE) {
        Geometry *geometry = object->prefab == PREFAB_SPHERE ? prefab_sphere_geo() : prefab_cylinder_geo(32, object->prefab == PREFAB_LIGHT_CONE);
        if (!geometry) return NULL;

        BasicMaterial *material = basic_material_init();
        Mesh *mesh = mesh_init(geometry, (Material *) material);

        mesh->visible = visible;
        mesh->transform.position = object->position;
        mesh->transform.rotation = rotation;
        mesh->transform.scale = object->scale;

        material->texture = texture_id;
        material->color = color;
        material->color.w = object->prefab == PREFAB_LIGHT_CONE && opacity == 1.0f ? 0.35f : opacity;
        material->emissive = emissive;
        material->base.transparent = opacity != 1.0f || object->prefab == PREFAB_LIGHT_CONE || prefab_textures[tex_id].transparent;
        material->use_face_tex_scaling = 0;
        material->unlit = object->prefab == PREFAB_LIGHT_CONE;
        prefab_apply_lighting_controls(material, object);

        return mesh;
    }

    if (object->prefab == PREFAB_RAMP) {
        Geometry *geometry = create_ramp_geo();
        if (!geometry) return NULL;

        BasicMaterial *material = basic_material_init();
        Mesh *mesh = mesh_init(geometry, (Material *) material);

        mesh->visible = visible;
        mesh->transform.position = object->position;
        mesh->transform.rotation.y = -(float) M_PI / 2.0f * (1.0f + (float) object->direction);
        mesh->transform.scale.y = object->scale.y;

        mesh->transform.scale.x = object->direction % 2 ? object->scale.x : object->scale.z;
        mesh->transform.scale.z = object->direction % 2 ? object->scale.z : object->scale.x;

        material->texture = texture_id;
        material->color = color;
        material->emissive = emissive;
        prefab_apply_lighting_controls(material, object);

        material->is_ramp = 1;
        material->use_face_tex_scaling = 1;
        material->face_scale = mesh->transform.scale;

        return mesh;
    }

    if (object->prefab == PREFAB_LADDER) {
        Geometry *geometry = create_ladder_geo(object->scale.y);
        if (!geometry) return NULL;

        BasicMaterial *material = basic_material_init();
        Mesh *mesh = mesh_init(geometry, (Material *) material);

        mesh->visible = visible;
        mesh->transform.position = object->position;
        mesh->transform.rotation.y = (float) M_PI * 0.5f * (float) (object->direction - 1);

        material->texture = texture_id;
        material->color = color;
        material->emissive = emissive;
        prefab_apply_lighting_controls(material, object);

        material->is_ladder = 1;
        material->use_face_tex_scaling = 1;

        material->face_scale.x = game_constants.ladder_width * 2.0f;
        material->face_scale.z = game_constants.ladder_scale * 2.0f;
        material->face_scale.y = mesh->transform.scale.y;

        return mesh;
    }

    // return NULL;

    BasicMaterial *debug_material = basic_material_init();
    Mesh *debug_mesh = mesh_init(create_cube_geo(), (Material *) debug_material);

    // debug_material->base.wireframe = 1;
    debug_mesh->transform.position = object->position;
    debug_mesh->transform.scale = object->scale;

    return debug_mesh;
}
