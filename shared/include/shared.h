#pragma once
#include <stdlib.h>
#include <modes.h>

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
    float border_height;
    float camera_height;
    float climb_height;
    float min_delta;
    float max_delta;
    float jump_velocity;
    float jump_aim_slow;
    float jump_push;
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
    float slide_time;
    float player_slide_velocity_mlt;
    float player_terrain_slide_velocity_mlt;
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

    unsigned char is_border:1;
    unsigned char premium:1;
    unsigned char verified:1;
    unsigned char team_zone:1;
    unsigned char score_zone:1;
    unsigned char teleporter:1;
    unsigned char checkpoint:1;
    unsigned char pickup:1;
    unsigned char flag:1;
    unsigned char trigger:1;
    unsigned char kill:1;
    unsigned char objective:1;
    unsigned char bomb_site:1;

    unsigned int team;
    unsigned int score_points;

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

typedef enum {
    CAMERA_TYPE_NORMAL,
    CAMERA_TYPE_TOP_DOWN,
    CAMERA_TYPE_PLATFORMER_X,
    CAMERA_TYPE_PLATFORMER_Z,
} CameraType;

typedef enum {
    MODEL_TYPE_NORMAL,
    MODEL_TYPE_SPRITE,
} ModelType;

typedef struct {
    CameraType cam_type;
    unsigned char cam_rotation:1;
    vec3 cam_offset;

    ModelType model;

    vec3 speed;

    float ladder_accel;
    float air_accel;
    float slide_time;
    float slide_accel;
    float jump_cooldown;

    unsigned char infinite_jump:1;
} MapConfig;

extern const MapConfig g_default_map_config;

typedef struct {
    MapConfig config;

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
    const char *name;
    const char **texts;
    const int *loadout;
    unsigned char secondary:1;
    unsigned char wall_jump:1;
    int colors[6];
    int health;
    int health_segments;
    float regen;
    float speed;
} ClassConfig;

extern const ClassConfig g_classes[];

typedef struct {

} Weapon;

typedef enum {
    TEAM_OPTIONS_NONE,
    TEAM_OPTIONS_HIDERS,
    TEAM_OPTIONS_SEEKERS,
    TEAM_OPTIONS_SURVIVORS,
    TEAM_OPTIONS_INFECTED,
    TEAM_OPTIONS_FOLLOWERS,
    TEAM_OPTIONS_SIMON,
    TEAM_OPTIONS_PROP,
    TEAM_OPTIONS_HEROES,
    TEAM_OPTIONS_BOSS,
    TEAM_OPTIONS_LIVING,
    TEAM_OPTIONS_STALKER,
    TEAM_OPTIONS_INNOCENT,
    TEAM_OPTIONS_TRAITOR
} TeamOptions;

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
    unsigned char swap:2;
} Input;

struct Game_t;

typedef struct {
    struct Game_t* game;

    int uid;
    unsigned char active:1;
    unsigned char noclip:1;

    unsigned char on_ground:1;
    unsigned char on_ladder:1;
    unsigned char on_wall:1;
    unsigned char on_terrain:1;
    unsigned char terrain_slipping:1;

    unsigned char did_act:1;
    unsigned char did_jump:1;
    unsigned char did_wall_jump:1;
    unsigned char can_slide:1;

    int input_seq;

    int health;
    int max_health;
    int team;

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

    int *loadout;
    int loadout_size;
    int loadout_index;

    Weapon *weapon;
    player_mesh_map meshes;
} Player;

Player *player_init(struct Game_t*);
void player_spawn(Player*);
void player_proc_input(Player*, const Input*, int, int);

typedef struct {
    int max_players;
    int min_players;
    int game_time;
    float warmup_time;
    int auto_respawn;
    int lives;
    int score_limit;
    float gravity_mlt;
    float jump_mlt;
    float delta_mlt;
    float slide_time;
    float impulse_mlt;
    float wall_jump;
    float strafe_speed;
    unsigned int can_slide:1;
    unsigned int air_strafe:1;
    unsigned int auto_jump:1;
    unsigned int bullet_drop:1;
    unsigned int health_regen:1;
    unsigned int kill_rewards:1;
    unsigned int headshots_only:1;
    unsigned int no_secondaries:1;
    unsigned int no_streaks:1;
    unsigned int disable_borders:1;
    unsigned int throwable_melees:1;
    unsigned int select_team:1;
    unsigned int allow_spectate:1;
    unsigned int third_person:1;
    unsigned int hide_nametags:1;
    char *team1_name;
    char *team2_name;
    float team1_damage;
    float team2_damage;
    float health_mlt;
    float fire_rate;
    float reload_speed;
} GameConfig;

extern const GameConfig g_default_game_config;

typedef struct Game_t {
    GameConfig config;
    GameMode *mode;
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

#define CROP(x, lim) (x <= lim && x >= -lim ? 0 : x)
#define CLAMP(x, min, max) (x < min ? min : x > max ? max : x)
#define MIN(a, b) (a < b ? a : b)
#define MAX(a, b) (a > b ? a : b)