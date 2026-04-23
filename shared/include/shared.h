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
    float camera_height;
    float climb_height;
    float min_delta;
    float max_delta;
    float jump_velocity;
    float jump_aim_slow;
    float crouch_jump;
    float crouch_speed;
    float player_speed;
    float slipping_speed;
    float air_speed;
    float aim_slow;
    float crouch_slow;
    float player_slipping_jump_cooldown;
    float terrain_slide_threshold;
    float terrain_slip_decel;
    float terrain_slide_decel;
    float terrain_decel;
    float slide_decel;
    float air_decel;
    float ground_decel;
    float ladder_decel;
    float crouch_distance;
    float gravity;
    float arm_scale;
    float arm_inset;
    float chest_width;
    float player_scale;
    float min_decel;
} GameConstants;

extern const GameConstants game_constants;

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
    PREFAB_BOT,
    PREFAB_PUMPKIN,
    PREFAB_RUNE,
    PREFAB_SKELETON,
    PREFAB_KNIGHT,
} Prefab;

typedef struct {
    int direction;
    vec2 start;
    vec2 end;
} Ramp;

typedef struct {
    float move;
    int move_direction;

    int frames;
    float frame_time;
} TextureAnimation;

typedef enum {
    COLLISION_TYPE_BOX,
    COLLISION_TYPE_CYLINDER,
    COLLISION_TYPE_NONE,
} CollisionType;

typedef struct {
    unsigned int prefab;
    unsigned int texture;
    CollisionType collision_type;

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
    int seq;
    int move_dir;

    float delta;
    float x_dir;
    float y_dir;

    unsigned char shoot:1;
    unsigned char jump:1;
    unsigned char crouch:1;
    unsigned char reload:1;
} Input;

struct Game_t;

typedef struct {
    struct Game_t* game;

    int uid;
    int active;
    int noclip;

    int on_ground;
    int on_ladder;
    int on_wall;
    int on_terrain;
    int terrain_slipping;

    int did_act;
    int did_jump;
    int did_wall_jump;

    int input_seq;

    int health;
    int max_health;

    float scale;
    float height;
    float speed;

    float crouch_val;
    float aim_val;

    int slid_cont;

    float slide_timer;
    float jump_timer;

    vec3 position;
    vec3 last_position;
    vec3 velocity;
    vec2 direction;

    player_mesh_map meshes;
} Player;

Player *player_init(struct Game_t*);
void player_spawn(Player*);
void player_proc_input(Player*, const Input*, int, int);

typedef struct Game_t {
    GameMode mode;
    Map *map;

    size_t player_count;
    Player **players;
} Game;

void game_players_add(Game*, Player*, int);

char *concat(const char*, const char*);
int read_file(const char*, unsigned char**, size_t*);
int parse_hex_color(const char*, vec4*);
float normalize_angle(float);
float progress_on_line(vec2, vec2, vec2);

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

#define CROP(x, v) (x > v ? v : x < -v ? -v : x)
#define CLAMP(x, min, max) (x < min ? min : x > max ? max : x)
#define MIN(a, b) (a < b ? a : b)
#define MAX(a, b) (a > b ? a : b)