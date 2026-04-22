#pragma once
#include <stdlib.h>

typedef struct {
    float x;
    float y;
} vec2;

typedef struct {
    float x;
    float y;
    float z;
} vec3;

typedef struct {
    float x;
    float y;
    float z;
    float w;
} vec4;

typedef struct {
    int billboard_count;
    float mouse_sensitivity;
    float world_uv_scale;
    float player_height;
} GameConstants;

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

extern GameConstants game_constants;

typedef struct {
    int direction;
    float start_x;
    float start_z;
    float end_x;
    float end_z;
} Ramp;

typedef struct {
    float move;
    int move_direction;

    int frames;
    float frame_time;
} TextureAnimation;

typedef struct {
    unsigned int prefab;
    unsigned int texture;

    vec3 position;
    vec3 scale;

    TextureAnimation *tex_anim;
    Ramp *ramp;
    void *mesh;
} Object;

typedef struct {
    vec3 position;
    int team;
    int direction;
    int comp;
} Spawn;

typedef struct {
    size_t spawn_count;
    Spawn **spawns;

    size_t object_count;
    Object **objects;

    struct {
        vec3 min;
        vec3 max;
    } dimensions;

    vec3 camera_position;
} Map;

Map *map_init(const char*);
void map_fini(Map*); // NOTE: ensure no meshes from the map are part of the scene, they are free()'d!

typedef struct {

} GameMode;

typedef enum {
    PLAYER_MESH_HEAD
} PlayerMesh;

#define NAME player_mesh_map
#define KEY_TY PlayerMesh
#define VAL_TY void*
#include <verstable.h>

typedef struct {
    int uid;
    int active;

    int health;

    vec3 position;
    vec2 direction;

    player_mesh_map meshes;
} Player;

typedef struct {
    GameMode mode;
    Map *map;

    size_t player_count;
    Player **players;
} Game;

char *concat(const char*, const char*);
int read_file(const char*, unsigned char**, size_t*);
int parse_hex_color(const char*, vec4*);

static inline void mat3x3(const float *a, const float *b, float *out) {
#define MAT3x3 \
    X(0) X(1) X(2) \
    X(3) X(4) X(5) \
    X(6) X(7) X(8)

#define X(i) out[i] = a[(i / 3) * 3] * b[(i % 3)] + a[(i / 3) * 3 + 1] * b[(i % 3) + 3] + a[(i / 3) * 3 + 2] * b[(i % 3) + 6];
    MAT3x3

#undef X
#undef MAT3x3
}

static inline void mat4x4(const float *a, const float *b, float *out) {
#define MAT4x4 \
    X(0) X(1) X(2) X(3) \
    X(4) X(5) X(6) X(7) \
    X(8) X(9) X(10) X(11) \
    X(12) X(13) X(14) X(15)

#define X(i) out[i] = a[(i / 4) * 4] * b[(i % 4)] + a[(i / 4) * 4 + 1] * b[(i % 4) + 4] + a[(i / 4) * 4 + 2] * b[(i % 4) + 8] + a[(i / 4) * 4 + 3] * b[(i % 4) + 12];
    MAT4x4

#undef X
#undef MAT4x4
}