#pragma once
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h> // offsetof (NET_ENT_PACKET_V3_SIZE)
#include <modes.h>
#include <weapons.h>
#include <cJSON.h>
#include <common.h>

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
    float crouch_lean;
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
    float chest_scale;
    float leg_scale;
    float leg_height;
    float head_scale;
    float body_height;
    float player_scale;
    float min_decel;
    float slide_time;
    float player_slide_velocity_mlt;
    float player_terrain_slide_velocity_mlt;
    float idle_anim_speed;
    float interpolation;
    float lean_sensitivity;
    float lean_max;
    float lean_pull;
    float lean_pull_z;
    float lean_mlt_z;
    float bob_pull_y;
    float bob_pull_z;
    float bob_mlt_y;
    float bob_mlt_z;
    float crouch_anim;
    float collision_padding;
    float booster_speed;
    float ladder_speed;
    float ladder_scale;
    float ladder_width;
    float death_y;
    float recoil_mlt;
    float spread_adjustment;
    float upper_arm_length;
    float lower_arm_length;
    float arm_offset;
    float anim_mlt;
    float aim_anim_mlt;
    float crouch_anim_mlt;
    float step_anim;
    float step_pull;
    float disk_spin;
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
    float boost;
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

// Krunker triggers (70.js events / actions, map.js addTrigger): the subset ported so far.
#define TRIGGER_EVENT_ON_ENTER 3        // triggers.events[3] "onEnter"
#define TRIGGER_ACTION_REFILL_KNIFE 23  // triggers.actions[23] "Refill Player Knife": refillKnife + "rfl"

typedef struct {
    unsigned int prefab;
    unsigned int texture;

    unsigned char active:1;
    unsigned char is_border:1;
    unsigned char premium:1;
    unsigned char verified:1;
    unsigned char jump_pad:1;
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
    unsigned char spawnable:1;
    unsigned char crouch:1;
    unsigned char ladder:1;
    unsigned char wall_jumpable:1;
    unsigned char ambient_enabled:1;
    unsigned char fog_enabled:1;
    unsigned char penetrable:1; // map "pe": bullets pass through (wallbang), Krunker penetrable
    unsigned char no_shoot:1;   // Krunker noShoot: hitscan ignores the object
    unsigned char force_col:1;  // /forcecol made this non-colliding object collide (restored on OFF)
    unsigned char trigger_on_enter:1;   // trigger "te" = onEnter (triggers.events[3])
    unsigned char trigger_single_use:1; // trigger "su": fires once per map load
    unsigned char triggered:1;          // a single-use trigger already fired

    unsigned char direction:2;

    unsigned int team;
    unsigned int score_points;
    int trigger_action;   // trigger "ta": index into Krunker's triggers.actions (TRIGGER_ACTION_*)
    int trigger_chance;   // trigger "cha": percent, 0 = always

    float spin;
    float bounce;

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

typedef struct {
    const cJSON *raw_data;

    MapConfig config;
    float death_y;

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

extern const MapConfig g_default_map_config;
extern const cJSON *g_maps[15];
extern const int g_rotation_maps[13];

void load_default_maps();
const cJSON **map_list_with_custom(const cJSON*, size_t*);

Map *map_init(const cJSON*);
void map_reset(Map*);
void map_fini(Map*); // NOTE: ensure no meshes from the map are part of the scene, they are free()'d!

typedef struct {
    const char *name;
    int icon_index;
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

extern const ClassConfig g_classes[14];

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

typedef struct {
    int seq;
    int move_dir;

    float delta;
    float x_dir;
    float y_dir;
    // input[12] (index.js b9): 1 - CONTROLS.speedLmt.round(1), the controller's Gradual
    // Speed (gradSpeed); procInputs scales the acceleration by limitMM(1 - it, 0, 1).
    // 0 = full speed.
    float slowdown;

    unsigned char shoot:1;
    unsigned char scope:1;
    unsigned char jump:1;
    unsigned char crouch:1;
    unsigned char reload:1;
    unsigned char swap:2;
} Input;

struct Game_t;

#define PLAYER_NAME_SIZE 32
#define PLAYER_DAMAGE_SLOTS 8

typedef struct {
    int uid;      // dealer uid, -1 = free slot
    float value;  // damage dealt by that player
    float time;   // game time of the last hit (seconds)
} PlayerDamage;

typedef struct {
    int primary;
    int secondary;

    int face;
    int hat;
    int back;
    int waist;
    int melee;
    int dye;

    int skin_color;
    int hair_color;
} PlayerSkins;

// First-person feel tweens (Krunker v3.1.1 TWEEN.Tween on landBobY / jumpBobY / jumpRot,
// started by players.js jump/resetStep/wall jump). Client-only animation state: only the
// local, non-recon player starts them, and they advance in real (unclamped) frame time
// like TWEEN.update() does. `via` is TWEEN's array target ([via, end] path, jumpRot).
typedef enum {
    PLAYER_EASE_LINEAR,
    PLAYER_EASE_BACK_OUT,
    PLAYER_EASE_ELASTIC_OUT,
    PLAYER_EASE_BACK_IN_OUT,
    PLAYER_EASE_CUBIC_OUT,      // meleeAnim swing / throw
    PLAYER_EASE_SINUSOIDAL_OUT, // meleeAnim equip flick
} PlayerEase;

typedef struct {
    float start, via, end;
    float time, duration;    // seconds
    unsigned char ease;      // PlayerEase
    unsigned char active:1;
    unsigned char has_via:1;
    unsigned char then_settle:1; // resetStep's onComplete: landBobY -> 0 over 1.1 s, Elastic.Out
} PlayerTween;

// Player.meleeAnim (players.js resetMeleeAnim / updateMsh): first-person melee arm and weapon
// offsets the weapon's `anim` (weapon->melee_anim) and swapWeapon's equip flick tween.
typedef enum {
    MELEE_ANIM_WEA_R,  // weaR: melee mesh rotation.z
    MELEE_ANIM_WEA_M,  // weaM: melee mesh position.x
    MELEE_ANIM_ARM_Y,  // armY: right arm position.y
    MELEE_ANIM_ARM_R,  // armR: right arm rotation.z
    MELEE_ANIM_L_ARM,  // lArm: left arm reach / punch
    MELEE_ANIM_R_ARM,  // rArm: right arm punch
    MELEE_ANIM_FLIP_W, // flipW: melee mesh rotation.y
    MELEE_ANIM_ARM_T,  // armT: right arm rotation.y
    MELEE_ANIM_ARM_M,  // armM: right arm position.x
    MELEE_ANIM_ARM_E,  // armE: right arm position.z
    MELEE_ANIM_ARM_S,  // armS: right arm rotation.x
    MELEE_ANIM_CHANNELS
} MeleeAnimChannel;

#define MELEE_ANIM_BIT(channel) (1u << MELEE_ANIM_##channel)

// One TWEEN.Tween(meleeAnim).to({...}, duration).delay(delay).easing(ease): the channels in
// `mask` go from their values at start() to `to` (seconds).
typedef struct {
    unsigned short mask;
    float to[MELEE_ANIM_CHANNELS];
    float duration, delay;
    unsigned char ease; // PlayerEase
} MeleeAnimStep;

// meleeAnim.anim: a step and the step its onComplete starts (has_then).
typedef struct {
    MeleeAnimStep step, then;
    float start[MELEE_ANIM_CHANNELS];
    float time;          // seconds since the running step's start()
    unsigned char phase; // 0 idle, 1 `step`, 2 `then`
    unsigned char has_then:1;
} MeleeAnimTween;

// Krunker's per-player look (jLrR6): skinColIndex / hairCol and the hat / back / waist / face / dye store items.
// Only int32 members, so it embeds unchanged in the packed net packets (ENT, SPAWN).
typedef struct {
    int32_t skin_color; // 0xRRGGBB (config.skinColors[skinColIndex].col), -1 = the class colour
    int32_t hair_color; // 0xRRGGBB, -1 = the class colour
    int32_t hat;        // store.skins index, -1 = none (hatIndex)
    int32_t back;       // backIndex
    int32_t waist;      // waistIndex
    int32_t face;       // faceIndex
    int32_t dye;        // dyeIndex
} PlayerCosmetics;

#define PLAYER_COSMETICS_NONE ((PlayerCosmetics) {-1, -1, -1, -1, -1, -1, -1})

typedef struct Player_t {
    struct Game_t* game;

    int uid;

    unsigned char is_you:1;
    unsigned char render_you:1;
    unsigned char left_handed:1;

    unsigned char active:1;
    unsigned char noclip:1;
    unsigned char god_mode:1;
    unsigned char unlimited_ammo:1;
    unsigned char force_hide:1;
    unsigned char smite:1;      // /smite: every hit this player deals does 10000 damage (Krunker player.smite)
    unsigned char interpolate:1;
    unsigned char inspecting:1;

    unsigned char is_hidden:1;
    unsigned char on_ground:1;
    unsigned char on_ladder:1;
    unsigned char on_ramp:1; // added instead of the didCollideRamp local in procInputs
    unsigned char on_terrain:1;
    unsigned char on_wall:3;
    unsigned char terrain_slipping:1;
    unsigned char wall_jump:1;

    unsigned char did_act:1;
    unsigned char did_shoot:1;
    unsigned char did_jump:1;
    unsigned char did_wall_jump:1;
    unsigned char can_slide:1;
    unsigned char can_throw:1;
    int in_object;                // Krunker inObject: map object index + 1 of the onEnter trigger stood in, 0 = none

    unsigned int kills;
    unsigned int deaths;
    unsigned int death_streak;

    // Match state (server-authoritative, mirrored to clients through KILL/SCORE/LEADERBOARD packets).
    char name[PLAYER_NAME_SIZE];  // display name (server-assigned Guest_N, synced by SPAWN)
    int secondary_index;          // chosen secondary weapon id (Select Secondary), -1 = Pistol
    PlayerCosmetics cosmetics;    // skin colour + cosmetic items (ENT -> SPAWN), PLAYER_COSMETICS_NONE by default
    int score;                    // Krunker player.score
    unsigned int assists;
    unsigned int streak;          // Krunker player.streak: multi-kill counter (reset after feedTimer)
    unsigned int kill_streak;     // Krunker player.killStreak: kills since the last death
    float last_kill_time;         // game time of the last kill (seconds)
    float death_time;             // game time of the last death (seconds)
    float hit_timer;              // Krunker hitTimer: regen delay left after taking damage (seconds)
    float regen_ticker;           // Krunker ticker: 0.5 s regen tick
    float spread_plus;            // Krunker spreadPlus: movement/fall spread
    float damage_dealt;
    int last_killer_uid;          // Krunker lastKiller (Revenge), -1 none
    PlayerDamage damage_received[PLAYER_DAMAGE_SLOTS]; // Krunker dmgReceived (assists)
    unsigned int shot_count;      // Krunker shotCntP: picks config.projSpreads for projectiles

    int input_seq;

    float dt;
    float send_rate;

    vec3 interp_pos_start;
    vec3 interp_pos_end;
    vec2 interp_dir_start;
    vec2 interp_dir_end;

    float health;
    int max_health;
    int team;

    int class_index;

    float scale;
    float height;
    float speed;

    float spread;
    float crouch_val;
    float aim_val;
    float aim_time;

    int slid_cont;

    float air_time;
    float covered_distance;

    float swap_timer;
    float reload_timer;
    float slide_timer;
    float jump_timer;
    float interact_progress;

    float *ramp_fix;

    float idle_anim;
    float hp_chase;

    vec3 position;
    vec3 last_position;
    vec3 velocity;
    vec2 direction;

    float x_vel_cylinder;
    float z_vel_cylinder;

    float jump_rotate;

    float land_bob_y;
    float land_bob_yr;
    float jump_bob_y;
    float jump_rotate_mlt;
    float step_val;
    float step_chase;
    unsigned char step_dir_neg:1; // Krunker stepDir: 0 = +1, 1 = -1 (flips at +-PI/2)

    PlayerTween land_tween;     // landBobY (landTween)
    PlayerTween jump_bob_tween; // jumpBobY (jumpTween, and the landing tween's jumpBobY -> 0)
    PlayerTween jump_rot_tween; // jumpRot
    float swap_tween_r;         // swapTweenR: weapon swap wiggle (viewmodel)
    PlayerTween swap_tween;     // swapTweenAnim
    float inspect_x;            // inspectX: weapon inspect swing (viewmodel)
    float inspect_anim[9];      // inspectAn (arZ, arX, arY, apZ, apX, apY, wpZ, wpY, wrY): melee inspect pose
    float inspect_anim_start[9];
    float inspect_anim_time;    // seconds into the 400 ms inspectTween
    unsigned char inspect_anim_active;
    float melee_anim[MELEE_ANIM_CHANNELS]; // meleeAnim: first-person melee swing / throw / equip pose
    MeleeAnimTween melee_anim_tween;       // meleeAnim.anim
    signed char hand_anim_ind;             // handAnimInd: 0 (unset), then -1 / 1 alternating per swing

    // shoot(): recoilTween on recoilTweenY/R/YM/Z (weapon.recoilAnim): phase 1 kicks the
    // masked values to the weapon's targets linearly, phase 2 eases all four back to 0.
    float recoil_tween[4];      // Y, R, YM, Z
    float recoil_tween_start[4];
    float recoil_tween_time, recoil_tween_duration, recoil_tween_return;
    unsigned char recoil_tween_phase; // 0 idle, 1 kick, 2 return
    unsigned char recoil_tween_mask;

    // Local first-person events for client systems (sound_events.c): bumped per Krunker
    // playerStep step (stepDir flip) and per resetStep landing with a vertical speed.
    unsigned short step_events;
    unsigned short land_events;

    vec3 bob_anim;
    vec3 lean_anim;
    vec3 recoil;

    float recoil_force;
    float recoil_anim;
    float recoil_anim_y;

    int *loadout;
    unsigned int *ammo;
    float *reloads;
    int burst_count;

    int loadout_size;
    int loadout_index;

    Input *input_queue;
    size_t input_queue_size;

    const Weapon *weapon;

    PlayerSkins skins;
    void *mesh;

    ModePlayerState mode;         // game mode stats (lives, caps, tier, ...), shared/src/modes
} Player;

typedef struct {
    int streak;
    int weapon_id;      // index into Game.weapons, -1 = none (fall/nuke)
    unsigned char headshot:1;
    unsigned char wallbang:1;
    unsigned char fists:1;  // Krunker fs: melee without the knife
    unsigned char thrown:1; // Krunker pj: projectile kill (thrown knife -> Bullseye)
    float distance;     // Krunker dst: fraction of the weapon range
} PlayerKillInfo;

Player *player_init(struct Game_t*);
void player_spawn(Player*);
void player_swap_weapon(Player*, int, int, int, int);
void player_kill(Player*, Player*, PlayerKillInfo*, int); // victim, killer (NULL = suicide/fall), info, skip_rewards
int player_change_health(Player*, Player*, float, int); // victim, dealer, damage (<0 heals), crit; 1 = victim died (authority only)
void player_score(Player*, int, int); // player, amount, is_kill_reward
void player_queue_input(Player*, const Input*);
void player_proc_input(Player*, const Input*, int, int);
void player_update(Player*, float);
void player_reset_feel(Player*); // spawn: step/bob/lean/recoil animation state back to rest
void player_inspect_weapon(Player*); // wInspect: toggle the weapon inspect (local player, inspKey)
void player_cancel_inspect(Player*);
void player_melee_anim_reset(Player*); // resetMeleeAnim (stop meleeAnim.anim first with player_melee_anim_stop)
void player_melee_anim_stop(Player*);  // meleeAnim.anim.stop()
void player_melee_anim_start(Player*, const MeleeAnimStep *step, const MeleeAnimStep *then); // then: onComplete step or NULL
void player_send_event();

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

    int *maps;
    int map_count;

    int *modes;
    int mode_count;
} GameConfig;

extern const GameConfig g_default_game_config;

typedef struct {
    float timer;

    int votes_for;
    int votes_req;

    Player *player;
} VoteKick;

// Server-side match events leave the shared game through these hooks; the native
// server (server/src/net.c) and the offline client's in-process game (client/src/net/local_game.c)
// route them to their connections. A NULL hook drops the event (tests, client mirror).
#define NET_TARGET_ALL (-1)
// Every connection without a player in Game.players (spectators, clients still in the menu).
#define NET_TARGET_NON_PLAYERS (-2)
// Every connection except the one owning player `uid` (players, spectators and menu clients):
// Krunker's broadcasts that skip the source (tracers, others' health).
#define NET_TARGET_EXCEPT(uid) (-3 - (uid))
#define NET_TARGET_EXCEPT_UID(target) (-3 - (target)) // decode: target <= -3

typedef struct {
    void *user;
    // Send one framed packet to the connection owning player `uid`, or to every
    // connection for NET_TARGET_ALL, NET_TARGET_NON_PLAYERS or NET_TARGET_EXCEPT(uid).
    // `type` is a NetPacketType.
    void (*send)(struct Game_t*, int uid, int type, const void *payload, uint16_t length);
    // The end screen timer ran out: rotate to the next map (the callee re-inits the
    // game; called last in game_tick, nothing touches the old players afterwards).
    void (*next_round)(struct Game_t*);
    // A mode respawns `uid` on its own (forceSpawn): the callee spawns it like an ENT
    // request. NULL: the player respawns when its client asks.
    void (*respawn)(struct Game_t*, int uid);
    // A mode replaces a player's loadout or outfit in place (Gun Game / Sharp Shooter /
    // Stalker loadouts, Infected / Stalker outfits): called with done = 0 right before the
    // change and done = 1 right after. The in-process offline client (client/src/net/
    // local_game.c), whose game is also its render state, rebuilds that player's meshes
    // around it. NULL elsewhere (server, client mirror: the LOADOUT/TEAM events do it).
    void (*player_changing)(const struct Game_t*, Player*, int done);
} GameHooks;

// Server projectiles (Krunker v3.1.1 src/data/projectile.js). Units per second
// (Krunker spd is units/ms); `range` counts down with the distance flown.
#define GAME_MAX_PROJECTILES 64

typedef struct {
    float speed;        // spd (units/ms)
    float gravity;      // grav (units/ms^2)
    float range;        // max flight distance
    float damage;       // direct hit damage
    float explode;      // explosion radius, 0 none
    float explode_damage; // eDmg at the centre
    float hit_box;      // hitB: sphere test radius around the target's middle, 0 = player box
    unsigned char no_impact:1; // nImp: no impact particles
} ProjectileConfig;

typedef struct {
    int id;             // Krunker m.sid: slot index, stable while active
    int owner_uid;      // -1 none
    int weapon_index;
    unsigned char active:1;
    unsigned char skip_move:1;
    unsigned char thrown_melee:1; // Krunker pj kill info (Bullseye)
    vec3 position;
    vec3 velocity;      // units/ms
    float speed;
    float yaw_dir;      // xD (yaw - PI) and yD (pitch) of the flight
    float pitch_dir;
    float range;
    ProjectileConfig config;
} GameProjectile;

// Krunker v3.1.1 match constants (src/config.js) in seconds.
#define GAME_END_TIMER 25.0f        // config.endTimer (end screen / map vote)
#define GAME_DEATH_DELAY 2.8f       // config.deathDelay (death card before the menu returns)
#define GAME_ASSIST_TIME 5.0f       // config.assistTime
#define GAME_ASSIST_SCORE 25        // config.assistScore
#define GAME_ASSIST_MIN 20.0f       // config.assistMin
#define GAME_FEED_TIMER 2.0f        // config.feedTimer (multi-kill window)
#define GAME_REGEN_DELAY 5.0f       // config.regenDelay
#define GAME_REGEN_TICK 0.5f        // players.tickPlayer ticker
#define GAME_SCORE_CAP 13000        // players.score: public games cap the score
#define GAME_LEADERBOARD_MAX 16     // players.syncLeaders: 16 rows

typedef struct Game_t {
    GameConfig config;
    GameMode *mode;
    Map *map;

    Weapon **weapons;
    ClassConfig *classes;
    const cJSON **maps;
    int *modes;

    size_t map_count;
    size_t mode_count;
    size_t class_count;
    size_t weapon_count;

    int current_map_index;
    int current_mode_index;

    size_t player_count;
    Player **players;

    unsigned char move_lock:1;
    unsigned char is_local:1;
    unsigned char ready:1;

    float end_timer;              // seconds of end screen left (> 0 = round over)

    // Round state (authority): Krunker game.gameTimer / lastTimer / kills.
    float round_timer;            // seconds left in the round
    float round_length;           // > 0: round length in seconds instead of config.game_time (server --round-seconds, CLI `round`)
    int round_seconds_sent;       // last whole second broadcast (TIMER packet), -1 none
    int kill_count;               // Krunker game.kills (First Blood)
    float now;                    // game time of the current tick (seconds)
    unsigned char round_endless:1;// /endless: Krunker endTimer = "inf", the round clock is stopped (game_set_endless)
    unsigned char leaders_dirty:1;// Krunker leadersUpdated: LEADERBOARD due this tick
    // The in-process offline client's game (client/src/net/local_game.c): game_end_round()
    // keeps the players active behind the end screen (v3.1.1's client plays the endAnim slow
    // motion with them) instead of players.clear(); only their input queues are dropped.
    unsigned char end_keep_players:1;
    GameHooks hooks;
    GameProjectile projectiles[GAME_MAX_PROJECTILES];

    float nuke_timer;
    Player *nuke_player;

    VoteKick *vote_kick;

    void *server; // TODO
} Game;

void game_configure(Game*, const GameConfig*, const cJSON**, size_t, int*, size_t, Weapon**, size_t, ClassConfig*, size_t);
void game_init(Game*, int, int, unsigned char);
void game_tick(Game*, float, float);
// game_tick with the world (player simulation) on its own step: the in-process offline
// client slows its world for the endAnim while the round/end clocks keep real time.
void game_tick_world(Game*, float now, float delta, float world_delta);
void game_players_add(Game*, Player*);
void game_broadcast();
int game_is_authority(const Game*);      // server or local game: runs damage/kills/score
// io_ent spawn rules: a ready round, not the end screen, not alive, config.deathDelay since
// the last death, lives left (`player` NULL: first spawn). Server and offline client alike.
int game_can_spawn(const Game*, const Player*);
void game_player_changing(const Game*, Player*, int done); // GameHooks.player_changing, when set
void game_send(Game*, int uid, int type, const void*, uint16_t); // through hooks.send
Player *game_find_player(Game*, int uid);
void game_sync_leaders(Game*, int uid);  // LEADERBOARD to `uid` (or NET_TARGET_ALL)
void game_start_round(Game*);            // fresh round timer + stats (after game_init)
void game_end_round(Game*);              // Krunker endGame(3): END packets, players cleared
void game_set_round_time(Game*, float seconds); // debug: seconds left in the running round (TIMER goes out next tick)
void game_set_endless(Game*, int on);   // /endless: stop the round clock (TIMER 0) / resume it with a fresh round length
void game_send_round_state(Game*, int uid); // TIMER (+ END while the end screen is up) for a joiner
void game_player_left(Game*, Player*);   // drop references (nuke, damage) to a leaving player
int game_pick_next_map(const Game*);     // next round's map: random rotation map (no map votes)
int game_pick_next_mode(const Game*);    // next round's game->modes index: FFA when rotated, else random (endData.mts[0])
int game_mode_index_for(Game*, int mode_id); // game->modes index of a Krunker mode id (appended when missing), -1 unknown
int game_mode_id(const Game*);           // the current round's Krunker mode id, -1 none
// Projectiles (shared/src/projectile.c), authority only.
const ProjectileConfig *projectile_config_for(const Weapon*, int thrown_melee); // NULL: the weapon fires no projectile
void game_spawn_projectile(Game*, Player *owner, int weapon_index, const ProjectileConfig*, vec3 origin, float yaw, float pitch, int thrown_melee);
void game_tick_projectiles(Game*, float delta);
void game_explosion(Game*, vec3 position, float radius, float damage, Player *owner, int weapon_index);

#define NET_DEFAULT_PORT 21015
#define NET_MAX_PAYLOAD 512
#define NET_PACKET_HEADER_SIZE 3
#define NET_BUFFER_SIZE 4096

typedef enum {
    NET_PACKET_HELLO = 1,
    NET_PACKET_INIT = 2,
    NET_PACKET_ENT = 3,
    NET_PACKET_SPAWN = 4,
    NET_PACKET_INPUT = 5,
    NET_PACKET_STATE = 6,
    NET_PACKET_CYCLE = 7,
    NET_PACKET_CHAT_SEND = 8,    // C->S: NetChatSendPacket (Krunker "ct")
    NET_PACKET_CHAT = 9,         // S->C: NetChatPacket (Krunker "ch")
    NET_PACKET_KILL = 10,        // S->C: NetKillPacket (Krunker "3")
    NET_PACKET_HIT = 11,         // S->C to the dealer: NetHitPacket (Krunker "4")
    NET_PACKET_HEALTH = 12,      // S->C: NetHealthPacket (Krunker "h")
    NET_PACKET_SCORE = 13,       // S->C to the player: NetScorePacket (Krunker "5")
    NET_PACKET_LEADERBOARD = 14, // S->C: NetLeaderboardPacket (Krunker "7")
    NET_PACKET_TIMER = 15,       // S->C: NetTimerPacket (Krunker "t")
    NET_PACKET_END = 16,         // S->C: NetEndPacket (Krunker "end")
    NET_PACKET_PING = 17,        // S->C: NetPingPacket (Krunker "pi")
    NET_PACKET_PONG = 18,        // C->S: NetPingPacket echo (Krunker "po")
    NET_PACKET_PING_RESULT = 19, // S->C: NetPingResultPacket (Krunker "l" ping field)
    NET_PACKET_LEAVE = 20,       // S->C: NetLeavePacket, the player disconnected (Krunker "2")
    NET_PACKET_SHOT = 21,        // S->C to the others: NetShotPacket, one hitscan ray (Krunker "9")
    NET_PACKET_PROJECTILE = 22,  // S->C: NetProjectilePacket, a projectile was fired (Krunker "pr")
    NET_PACKET_PROJECTILE_END = 23, // S->C: NetProjectileEndPacket (Krunker "pre")
    NET_PACKET_EXPLOSION = 24,   // S->C: NetExplosionPacket (Krunker "ex")
    NET_PACKET_MAP_INFO = 25,    // S->C before every INIT: NetMapInfoPacket, which map INIT.map_index means
    NET_PACKET_MAP_REQUEST = 26, // C->S: NetMapRequestPacket, the client lacks that map
    NET_PACKET_MAP_DATA = 27,    // S->C: NetMapDataPacket chunks of the map JSON (Krunker sends custom maps in "init")
    NET_PACKET_SPECTATE = 28,    // S->C: NetSpectatePacket, an ENT spectate request was accepted (Krunker "start" spectating arg)
    NET_PACKET_SPECTATE_TARGET = 29, // C->S: NetSpectateTargetPacket, the player a spectator follows (Krunker "fpSp")
    NET_PACKET_ERROR = 30,       // S->C right before the server closes the connection: NetErrorPacket (Krunker "error")
    NET_PACKET_VOICE_SEND = 31,  // C->S: NetSocialChunk pieces of one voice clip, a data URL (Krunker "vc", shared/include/voice.h)
    NET_PACKET_VOICE = 32,       // S->C: NetSocialChunk pieces of one relayed clip + alias + sid (Krunker "vc")
    // Game mode packets (shared/src/modes, Krunker v3.1.1 modes.js), numbered from 40 so the
    // core protocol above can grow without colliding.
    NET_PACKET_MODE_STATE = 40,  // S->C: NetModeStatePacket, full mode snapshot (round start, joiners)
    NET_PACKET_GAME_MESSAGE = 41,// S->C: NetGameMessagePacket (Krunker "gmsg")
    NET_PACKET_TEAM = 42,        // S->C: NetTeamPacket, a player's team / role (Krunker "tm" + its "h")
    NET_PACKET_TEAM_SCORE = 43,  // S->C: NetTeamScorePacket (Krunker "ts")
    NET_PACKET_FLAG = 44,        // S->C: NetFlagPacket (Krunker "ufl" / "ufp" / "ufi")
    NET_PACKET_OBJECTIVE = 45,   // S->C: NetObjectivePacket (Krunker "obj" / "wep")
    NET_PACKET_LIVES = 46,       // S->C to the player: NetLivesPacket (Krunker "lv")
    NET_PACKET_ACTION = 47,      // S->C: NetActionPacket, an action feed line (Krunker "ac")
    NET_PACKET_LOADOUT = 48,     // S->C: NetLoadoutPacket, a mode changed a loadout (Krunker "inat" / "uamo")
    NET_PACKET_LOCK = 49,        // S->C: NetLockPacket (Krunker "lock" / "lm" / "ulm")
    NET_PACKET_SPAWNABLE = 50,   // S->C: NetSpawnablePacket (Krunker "crsp" / "dsp")
    NET_PACKET_MODE_END = 51,    // S->C after every END: NetModeEndPacket (endData.tms, winner team, mode stats)
    NET_PACKET_MEDAL = 52,       // S->C to the player: NetMedalPacket (Krunker "am")
    NET_PACKET_REFILL = 53,      // S->C to the player, no payload: the thrown knife is back (Krunker "rfl" -> refillKnife)
    NET_PACKET_KILL_MEDALS = 54, // S->C to the killer: NetKillMedalsPacket, kills.js reward list (Krunker "6")
    NET_PACKET_ASSIST = 55,      // S->C to the assister, no payload: the "Assist" medal (Krunker "10")
    // Chat command effects (shared/src/chat_commands.c), numbered from 60.
    NET_PACKET_CHEAT = 60,       // S->C: NetCheatPacket (Krunker "noc" / "unl" / "fh" to the target, "borders" / "forcecol" to all)
    // Social hub (shared/include/social.h; Krunker's /api/ws socket + /api/* routes), numbered from 70.
    NET_PACKET_SOCIAL_REQUEST = 70, // C->S: NetSocialChunk pieces of one JSON request frame
    NET_PACKET_SOCIAL_REPLY = 71,   // S->C: NetSocialChunk pieces of one JSON array of reply frames
} NetPacketType;

#define NET_MAP_MAX_SIZE (4u * 1024u * 1024u) // largest map JSON streamed to clients
#define NET_MAP_CHUNK_SIZE 496                // NetMapDataPacket payload bytes per frame

// Chat (Krunker v3.1.1: #chatInput maxlength=70, server burst limit 3 messages / 1 s).
#define NET_CHAT_MAX_CHARS 70       // code points kept from a message
#define NET_CHAT_TEXT_SIZE 288      // bytes incl. NUL: 70 code points of UTF-8 always fit
#define NET_CHAT_BURST_WINDOW 1.0f  // seconds
#define NET_CHAT_BURST_MAX 3        // messages per window; more closes the connection (4008)
#define NET_PING_INTERVAL 5.0f      // seconds between ping probes (io_po setTimeout 5e3)
#define NET_PING_EWMA_ALPHA 0.35f   // packetHandlers.ts PING_EWMA_ALPHA
#define NET_PING_MAX_MS 5000        // PING_MAX_REASONABLE_MS

typedef enum {
    NET_CHAT_PLAYER = 0,  // "<name>: <text>"
    NET_CHAT_GAME = 1,    // plain game line (gB type 1)
    NET_CHAT_SERVER = 2,  // server message, #fc03ec (gB type 2)
} NetChatKind;

enum {
    NET_INPUT_SHOOT = 1 << 0,
    NET_INPUT_SCOPE = 1 << 1,
    NET_INPUT_JUMP = 1 << 2,
    NET_INPUT_CROUCH = 1 << 3,
    NET_INPUT_RELOAD = 1 << 4,
};

#pragma pack(push, 1)
typedef struct {
    uint8_t major;
    uint8_t minor;
    uint8_t patch;
} NetHelloPacket;

typedef struct {
    uint8_t major;
    uint8_t minor;
    uint8_t patch;
    int32_t map_index;
    int32_t mode_index;
    uint16_t tick_rate;
    int32_t client_id;
} NetInitPacket;

typedef struct {
    int32_t class_index;
    int32_t secondary_index; // Krunker ent[5] selectedSecondaryIndex: weapon id, -1 = class default (optional: 4-byte ENT = class only)
    PlayerCosmetics cosmetics; // optional (8-byte ENT = class + secondary): none
    uint8_t spectate;          // Krunker ent[6] (toggleSpect): join as a spectator, no spawn (optional: only sent when 1)
} NetEntPacket;

#define NET_ENT_PACKET_MIN_SIZE 4 // class_index only (older clients / bridges)
#define NET_ENT_PACKET_V2_SIZE 8  // class_index + secondary_index (before cosmetics)
#define NET_ENT_PACKET_V3_SIZE (offsetof(NetEntPacket, spectate)) // up to cosmetics: what a client sends to play

typedef struct {
    uint8_t spectating;      // 1: this connection spectates now (no player; every player's state is streamed to it)
} NetSpectatePacket;

typedef struct {
    int32_t uid;             // followed player, -1 = free cam
} NetSpectateTargetPacket;

typedef struct {
    int32_t uid;
    uint8_t is_you;
    uint8_t active;
    int32_t class_index;
    int32_t team;
    float health;
    int32_t max_health;
    float position[3];
    float direction[2];
    char name[32];
    int32_t secondary_index; // the spawned loadout's secondary weapon id, -1 class default
    PlayerCosmetics cosmetics; // optional: a 78-byte SPAWN (older servers / net-bridge) means none
} NetSpawnPacket;

#define NET_SPAWN_PACKET_V1_SIZE 78 // up to secondary_index (before cosmetics)

typedef struct {
    int32_t seq;
    int32_t move_dir;
    float delta;
    float x_dir;
    float y_dir;
    uint8_t flags;
    uint8_t swap;
    uint8_t slowdown; // Input.slowdown in tenths, clamped to 0..10 (outside [0, 1] it scales like the bound)
} NetInputPacket;

#define NET_INPUT_PACKET_V1_SIZE (offsetof(NetInputPacket, slowdown)) // 22 bytes: no slowdown (older senders, net-bridge)

typedef struct {
    int32_t uid;
    int32_t ack_seq;
    uint8_t active;
    int32_t loadout_index;
    float position[3];
    float velocity[3];
    float direction[2];
    float health;
    float aim_val;
    float crouch_val;
} NetStatePacket;

typedef struct {
    char text[NET_CHAT_TEXT_SIZE];  // NUL-terminated UTF-8
} NetChatSendPacket;

typedef struct {
    int32_t uid;                    // sender, -1 for server/game lines
    uint8_t kind;                   // NetChatKind
    char name[PLAYER_NAME_SIZE];    // sender name ("" for server lines)
    char text[NET_CHAT_TEXT_SIZE];  // raw text; the HUD escapes it
} NetChatPacket;

typedef struct {
    int32_t victim_uid;
    int32_t killer_uid;    // -1: suicide / fall / death zone
    int32_t weapon_index;  // Game.weapons index, -1 none
    uint8_t headshot;
    uint8_t wallbang;
    uint8_t fists;
    int32_t victim_deaths;
    int32_t killer_kills;
    int32_t killer_class;  // kill card icon
    int32_t score;         // kill reward the killer earned (kill card "+N"), 0 none
    float respawn_delay;   // seconds before the victim may respawn (config.deathDelay)
} NetKillPacket;

typedef struct {
    int32_t target_uid;
    int32_t damage;        // Math.round(dmg)
    uint8_t crit;          // headshot
    uint8_t kill;
} NetHitPacket;

typedef struct {
    int32_t uid;
    float health;          // Math.ceil(health)
    uint8_t has_source;    // the victim's own packet carries the dealer position
    float source_x;
    float source_z;
} NetHealthPacket;

typedef struct {
    int32_t delta;         // score gained (Krunker "5" value)
    int32_t score;         // total after the change
} NetScorePacket;

typedef struct {
    int32_t uid;
    int32_t score;
    int32_t kills;
    int32_t deaths;
} NetLeaderboardEntry;

typedef struct {
    uint8_t count;
    NetLeaderboardEntry entries[GAME_LEADERBOARD_MAX]; // sorted by score, high first
} NetLeaderboardPacket;

typedef struct {
    int32_t seconds;       // seconds left
    uint8_t end;           // 1: end screen countdown to the next round (fD(time, 1))
} NetTimerPacket;

typedef struct {
    int8_t result;         // 1 victory, 0 defeat, -1 draw (no winner)
    int32_t next_round;    // seconds until the next round
    int32_t winner_uid;    // -1 none
    NetLeaderboardPacket board;
} NetEndPacket;

typedef struct {
    uint32_t seq;
} NetPingPacket;

typedef struct {
    int32_t ping_ms;       // smoothed RTT
} NetPingResultPacket;

typedef struct {
    int32_t uid;
} NetLeavePacket;

typedef struct {
    int32_t uid;           // shooter
    int32_t weapon_index;
    float hit[3];          // impact point (or the end of the range)
    uint8_t has_normal;    // hit a map object: normal angles below are valid
    float normal_yaw;      // aM: 0 = +z, PI/2 = +x
    float normal_pitch;    // aN: PI/2 = up
    uint8_t trail;         // weapon.trail
    uint8_t first;         // first ray of the shot (shotguns send one per pellet): play the sound once
} NetShotPacket;

typedef struct {
    int32_t id;            // projectile slot
    int32_t owner_uid;
    int32_t weapon_index;  // 12 = thrown knife
    float position[3];
    float velocity[3];     // units/ms
    float gravity;         // units/ms^2
    float range;
} NetProjectilePacket;

typedef struct {
    int32_t id;
    float position[3];
    uint8_t impact;        // impact particles at the end point
} NetProjectileEndPacket;

typedef struct {
    float position[3];
    float radius;
} NetExplosionPacket;

typedef struct {
    int32_t map_index;     // the server's index, as in the INIT that follows
    uint32_t hash;         // net_map_hash() of the map JSON
    uint32_t size;         // bytes of its unformatted JSON (what MAP_DATA streams)
    char name[32];         // map "name" (logs / errors)
} NetMapInfoPacket;

typedef struct {
    uint32_t hash;
} NetMapRequestPacket;

typedef struct {
    uint32_t hash;
    uint32_t offset;
    uint16_t length;
    uint8_t data[NET_MAP_CHUNK_SIZE];
} NetMapDataPacket;     // sent with length 10 + data length

#define NET_MAP_DATA_HEADER_SIZE 10

typedef struct {
    char message[96];      // shown on the client's connection error screen
    char code[8];          // Krunker error code ("000005" = idle kick)
} NetErrorPacket;

// NET_PACKET_CHEAT: what a chat command changed (shared/src/chat_commands.c). The
// client applies it to its mirror and prints Krunker's line ("Noclip - Enabled", ...).
typedef enum {
    NET_CHEAT_NOCLIP = 0,          // "noc" -> me->noclip
    NET_CHEAT_UNLIMITED_AMMO = 1,  // "unl" -> me->unlimited_ammo
    NET_CHEAT_FORCE_HIDE = 2,      // "fh"  -> me->force_hide
    NET_CHEAT_BORDERS = 3,         // "borders" -> !game.config.disable_borders
    NET_CHEAT_FORCE_COLLIDE = 4,   // "forcecol" -> map objects forced collidable
} NetCheatFlag;

typedef struct {
    uint8_t flag;   // NetCheatFlag
    uint8_t on;
} NetCheatPacket;

// ---- game mode packets (shared/src/modes/mode_net.c builds them)
#define NET_MODE_TEXT_SIZE 32
#define NET_GAME_MESSAGE_SIZE 96
#define NET_GAME_MESSAGE_WAITING "wt" // gmsg "wt": the client shows i18n player.waiting
#define NET_MODE_END_MAX 16

typedef struct {
    float position[3];          // crystal centre (the carrier's ping while carried)
    int32_t carrier_uid;        // -1 at base / dropped
    int32_t team;               // 1 / 2
} NetModeFlag;

typedef struct {
    int32_t mode_id;            // GameModeId
    int32_t team_scores[2];     // teams 1 and 2
    char flag_info[2][NET_MODE_TEXT_SIZE]; // CTF teamFlags 1 / 2 ("AT BASE", "AWAY", carrier name)
    int32_t active_objective;   // Hardpoint / KOTH objective index, -1 none
    int32_t objective_weapon;   // Sharp Shooter weapon id, -1 none
    uint8_t move_lock;          // game.moveLock
    uint8_t locked;             // the recipient is locked (lockPlayer)
    int32_t lives;              // the recipient's lives, -1: the mode has none
    uint8_t flag_count;
    NetModeFlag flags[MODE_MAX_FLAGS];
    char message[NET_GAME_MESSAGE_SIZE]; // the current game message ("" none)
} NetModeStatePacket;

typedef struct {
    char text[NET_GAME_MESSAGE_SIZE]; // "" clears, NET_GAME_MESSAGE_WAITING = waiting for players
} NetGameMessagePacket;

typedef struct {
    int32_t uid;
    int32_t team;
    float health;               // the role's health (Boss 10000, Stalker 400, ...)
    int32_t max_health;
} NetTeamPacket;

typedef struct {
    int32_t team;
    int32_t score;
} NetTeamScorePacket;

typedef struct {
    int32_t index;              // flag index (map order)
    NetModeFlag flag;
    char info[NET_MODE_TEXT_SIZE]; // teamFlags[flag.team]
} NetFlagPacket;

typedef enum {
    NET_OBJECTIVE_POINT = 0,    // "obj": active objective index
    NET_OBJECTIVE_WEAPON = 1,   // "wep": Sharp Shooter weapon id
} NetObjectiveKind;

typedef struct {
    int32_t kind;               // NetObjectiveKind
    int32_t value;
} NetObjectivePacket;

typedef struct {
    int32_t lives;
} NetLivesPacket;

typedef struct {
    int32_t uid;                // the actor
    int32_t other_uid;          // -1 none
    char text[NET_MODE_TEXT_SIZE]; // "captured the Flag", "infected", ...
} NetActionPacket;

#define NET_LOADOUT_MAX 4

typedef struct {
    int32_t uid;
    int32_t count;
    int32_t weapons[NET_LOADOUT_MAX];
    int32_t ammo[NET_LOADOUT_MAX];
    int32_t loadout_index;
    int32_t weapon_tier;        // Gun Game tier (the clients rebuild the same loadout on respawn)
} NetLoadoutPacket;

typedef enum {
    NET_LOCK_PLAYER = 0,        // "lock": the recipient may not move or shoot
    NET_LOCK_MOVE = 1,          // "lm" / "ulm": game.moveLock for everyone
} NetLockKind;

typedef struct {
    uint8_t kind;               // NetLockKind
    uint8_t locked;
} NetLockPacket;

typedef struct {
    int32_t sid;
    uint8_t active;             // 0: dispose
    int32_t team;
    float position[3];
} NetSpawnablePacket;

typedef enum {
    NET_MODE_STAT_CAPS,
    NET_MODE_STAT_CONFIRMS,
    NET_MODE_STAT_DENIES,
    NET_MODE_STAT_OBJECTIVE,
    NET_MODE_STAT_WEAPON_TIER,
    NET_MODE_STAT_SETBACKS,
    NET_MODE_STAT_MELEES,
    NET_MODE_STAT_CONVS,
    NET_MODE_STAT_LIVES,
    NET_MODE_STAT_ASSISTS,
    NET_MODE_STAT_COUNT
} NetModeStat;

typedef struct {
    int32_t uid;
    int16_t stats[NET_MODE_STAT_COUNT];
} NetModeEndEntry;

typedef struct {
    int32_t mode_id;
    int32_t winner_team;        // 1 / 2, -1 none (FFA or draw)
    int32_t team_scores[2];
    uint8_t count;
    NetModeEndEntry entries[NET_MODE_END_MAX]; // in endSort order
} NetModeEndPacket;

typedef struct {
    char text[NET_MODE_TEXT_SIZE]; // "Capture", "Confirmed", ...
    int32_t score;
} NetMedalPacket;

// kills.js reward(): [medalName, score, ...] pairs in push order; "" names are the plain
// kill score (no popup). Sent only when the list is not empty, before the SCORE packet.
#define NET_KILL_MEDALS_MAX 13 // every medal of one kill fits (and the packet stays under NET_MAX_PAYLOAD)
typedef struct {
    uint8_t count;
    NetMedalPacket medals[NET_KILL_MEDALS_MAX];
    int32_t kills;                 // the killer's kills (Krunker "6" third value)
} NetKillMedalsPacket;
_Static_assert(sizeof(NetKillMedalsPacket) <= NET_MAX_PAYLOAD, "NetKillMedalsPacket exceeds NET_MAX_PAYLOAD");
#pragma pack(pop)

typedef struct {
    uint8_t type;
    uint16_t length;
    uint8_t payload[NET_MAX_PAYLOAD];
} NetPacket;

typedef struct {
    size_t length;
    uint8_t data[NET_BUFFER_SIZE];
} NetPacketBuffer;

int net_send_packet(int, NetPacketType, const void*, uint16_t);
int net_buffer_push(NetPacketBuffer*, const void*, size_t);
int net_buffer_next(NetPacketBuffer*, NetPacket*);
void net_packet_from_input(NetInputPacket*, const Input*);
void net_packet_to_input(Input*, const NetInputPacket*);
int net_packet_read_input(NetInputPacket*, const NetPacket*); // full or 22-byte v1 INPUT; 0 = malformed
void net_packet_from_player(NetStatePacket*, const Player*);
void net_packet_apply_player(Player*, const NetStatePacket*);
// Chat helpers shared by the native and the local server.
int net_chat_sanitize(char *out, size_t size, const char *in, size_t length); // 1 = non-empty message
typedef struct {
    float window_start;
    int count;
} NetChatLimiter;
int net_chat_allow(NetChatLimiter*, float now); // 0 = burst exceeded (Krunker closes the socket)
void net_packet_chat(NetChatPacket*, int uid, NetChatKind, const char *name, const char *text);
int net_ping_update(int previous_ms, float rtt_seconds); // EWMA smoothing, previous < 0 = first sample
int net_packet_read_ent(NetEntPacket*, const NetPacket*); // 4-byte (class only), 8-byte (no cosmetics) or full ENT; 0 = malformed
int net_packet_read_spawn(NetSpawnPacket*, const NetPacket*); // 78-byte (no cosmetics) or full SPAWN; 0 = malformed
// FNV-1a of cJSON_PrintUnformatted(map) (cached per map object); json_out/size_out
// optionally receive a malloc'd copy of that text. 0 when the map cannot be printed.
uint32_t net_map_hash(const cJSON *map, char **json_out, size_t *size_out);

char *concat(const char*, const char*);
const char *assets_path();
int ensure_asset_file(const char*); // web: fetch into MEMFS on demand, native: no-op returning 1
// web: starts a background download of the asset into MEMFS so a later ensure_asset_file()
// does not block the frame on the network; 1 = present, 0 = still downloading, -1 = the
// download failed (ensure_asset_file() reports it). Native: always 1.
int asset_prefetch(const char*);
// web: asset_prefetch() every path (full paths under assets_path()) and wait until all have
// arrived or failed, so a load step pays one round trip instead of one per file. Native: no-op.
void asset_prefetch_wait(const char *const *paths, size_t count);
int read_file(const char*, unsigned char**, size_t*);
int load_json_file(const char*, cJSON**);
int parse_hex_color(const char*, vec4*);
vec4 hex_to_vec(int);
float normalize_angle(float);
float progress_on_line(vec2, vec2, vec2);
float line_in_rect(vec3, vec3, vec3, vec3);
void angles_from_sides(float, float, float, float*);

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
