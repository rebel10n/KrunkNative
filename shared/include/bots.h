#pragma once
// Krunker v7.1.4 bot players (TFUE / FTUE "First Time User Experience" bots): fake players
// that join the game like a connection would, with an account-style name, level, clan tag,
// class, loadout and random store skins, and an AI that pushes input frames into their
// Player exactly like a client's decoded input. Clients cannot tell them from humans except by
// behaviour: they get the same SKINS + SPAWN + PLAYER_INFO, states, kills and leaderboard rows.
//
// Reference (jails/kru/src/Krunker_v7.1.4):
//   src/data/bots.js        BotManager / Bot (the legacy "tfue" manager: spawn, update, onDeath,
//                           remove, setupLocal), src/data/names.js (generateName),
//   server/NextBotManager.ts the "nextbot" manager the dev console drives by default (identity,
//                           cosmetics, fake account), bot/nextbot/* (BotProfile presets, Idle /
//                           Hunt / Attack / Hide state machine, aim focus, dodge),
//   server/index.ts         handleDevConsoleBotCommand (bot_* console commands, BOT_CONFIG_TYPES,
//                           BOT_DIFFICULTY_PRESETS, BOT_STOP_CONFIG / BOT_RESUME_CONFIG),
//   mod/devConsole.ts       the registered bot_* commands and their help text.
//
// shared/src/bots.c runs on the authority only (server_bin and the offline in-process game):
// the commands are in shared/src/chat_commands.c (server console, rcon, chat, dev console).
// Intentional differences are documented in bots.c and doc/console.md.
#include <shared.h>

#define GAME_BOTS_MAX 32            // parseBotConfigAssignments: Math.min(32, count) per bot_add
#define GAME_BOT_LIMIT 16           // config.botLimit: the server.cfg quota cap
#define BOT_QUOTA_GRACE_MS 10000.0f // fill: an empty lobby's bots wait this long for a rejoin (server.cfg bot_quota_grace)
#define GAME_BOT_UID_BASE 1         // offline (no alloc_uid hook): uids after the local player's 0
#define GAME_BOT_ACCOUNT_BASE 2000000 // NextBot createAccount: Account(2_000_000 + id)
#define GAME_BOT_CLAN_SIZE 8

// BOT_DIFFICULTY_PRESETS (server/index.ts) / krunkerConfig.ts *_BOT_DIFFICULTY
#define BOT_DIFFICULTY_EASY 0.65f
#define BOT_DIFFICULTY_NORMAL 1.0f
#define BOT_DIFFICULTY_HARD 1.8f
#define BOT_DIFFICULTY_MAX 3.0f

typedef enum {
    BOT_TYPE_NEXTBOT = 0,           // NextBotManager: state machine AI (dev console default)
    BOT_TYPE_TFUE = 1,              // bots.js BotManager: the legacy straight-line FTUE AI
} BotType;

// bots.d.ts BotConfig (+ NextBot's retreat / autoRespawn). One copy per manager (the
// defaults bot_config / bot_difficulty / bot_stop edit) and one per bot.
typedef struct {
    unsigned char target_bots;      // targetBots: bots may target other bots
    unsigned char target_team;      // targetTeam: target everyone (ignore teams)
    unsigned char force_crouch;
    unsigned char attack_target;
    unsigned char follow_target;
    unsigned char watch_target;
    unsigned char mimic_target;
    unsigned char slide_hop;
    unsigned char can_respawn;      // canRespawn (autoRespawn sets it too)
    unsigned char use_spawn_delay;  // respawn after config.deathDelay instead of at once
    unsigned char remove_on_death;  // lives ran out: remove instead of staying dead
    unsigned char nod_yes;
    unsigned char nod_no;
    unsigned char join_message;
    unsigned char leave_message;
    unsigned char voice_chat;       // parsed only (no voice clip natively)
    unsigned char god_mode;
    unsigned char roam;
    unsigned char no_recoil;        // parsed only
    unsigned char random_cosmetics;
    unsigned char featured;
    unsigned char random_jump;
    unsigned char retreat;          // NextBot: break off and hide when low on health
    unsigned char score_on_death;   // parsed only (no score drop spawnable natively)
    unsigned char use_pathing;      // v10.0.2 usePathing (tfue bots): idle bots walk the map's "botPaths"
    float dmg_mlt;
    float speed_mlt;
    float range_mlt;
    float spread_mlt;               // parsed only
    float health_mlt;
    int level;
    int premium;
    int cosmetic_rarity;            // -1 = undefined (no cap)
    float difficulty;
    float accuracy;
    float target_distance;
    char clan[GAME_BOT_CLAN_SIZE];  // "" = 0 (no clan)
} BotConfig;

// The per-bot overrides of BotManager.add(spawnPoint, spawn, skipSync, overrides): identity and
// loadout keys of BOT_CONFIG_TYPES that are not BotConfig fields. -1 / "" = not given.
typedef struct {
    char name[PLAYER_NAME_SIZE];
    int class_index, secondary, team, lives, persistent, force_class;
    int prim_skin, hat, body, waist, face, dye, melee, skin_color, hair_color;
    int head, back, shoes, pet, wrist; // v7.1.4 slots (WP-F): headIndex, backIndex, shoeIndex, petIndex, wristIndex
} BotSpawnOptions;

// The fields a key=value list set (bot_add / bot_config): what was given wins over defaults.
typedef struct {
    BotConfig config;
    BotSpawnOptions spawn;
    unsigned long long set;         // bit per BOT_KEY_* given
    int count;                      // a bare number: how many bots (0 = not given)
    char error[96];                 // the first error ("" = ok), v7 wording
} BotAssignments;

typedef enum { BOT_STATE_IDLE, BOT_STATE_HUNT, BOT_STATE_ATTACK, BOT_STATE_HIDE, BOT_STATE_OBJECTIVE } BotState; // OBJECTIVE: Hunt toward the mode's objective (bot_objective.c)

// shared/src/bot_objective.c: the mode objective a bot plays (its Hunt goal, the combat weighting)
typedef enum { BOT_ROLE_NONE, BOT_ROLE_ATTACK, BOT_ROLE_DEFEND, BOT_ROLE_SUPPORT } BotRole;
typedef enum {
    BOT_GOAL_NONE,
    BOT_GOAL_FLAG,      // a flag to take (the enemy's, the neutral one) or return (ours, dropped)
    BOT_GOAL_HOME,      // carrying a flag: our base
    BOT_GOAL_CARRIER,   // the enemy carrier (its flag ping)
    BOT_GOAL_ESCORT,    // a teammate carrier
    BOT_GOAL_GUARD,     // a spot by our base
    BOT_GOAL_KITE,      // carrying the neutral flag: away from the enemy
    BOT_GOAL_HILL,      // the active objective (Hardpoint / KOTH)
    BOT_GOAL_ZONE,      // a Domination zone
    BOT_GOAL_TAG,       // a tag / KR Tag spawnable
    BOT_GOAL_DEPOSIT,   // the active objective with KR Tags to deposit
    BOT_GOAL_SITE,      // a bomb site
    BOT_GOAL_HUNT,      // elimination: the nearest enemy
    BOT_GOAL_REGROUP,   // elimination, outnumbered: the nearest teammate
} BotGoalKind;
typedef struct {
    BotRole role;
    BotGoalKind kind;
    int index;                      // the flag / objective uid / zone / spawnable sid / site the goal is about, -1
    vec3 goal;                      // feet position to walk to
    vec3 look;                      // the threat direction to face while holding (has_look)
    vec3 zone_center, zone_half;    // the volume the goal is in (has_zone): arrived = inside it
    unsigned char hold:1, arrived:1, has_look:1, has_zone:1, alt:1;
    float think_timer;              // ms until the next decision
    float fail_timer;               // ms left roaming instead of an unreachable goal
    float hold_time;                // ms spent holding at the goal
    float stalemate_ms;             // CTF: ms carrying while the enemy holds our flag (then hunt their carrier)
    float look_timer, look_offset;  // the hold look sweep
    int rank, team_bots;            // this bot among its team's bots (list order), their count
    int last_flag, last_tags;       // a change re-thinks at once
} BotObjective;

#define BOT_PATH_RADIUS 10.0f       // v10 BotManager.config.pathRadius: a path node counts as reached within it

// Navigation (shared/src/bot_nav.c): a smoothed A* route over the map's walkable grid.
#define BOT_PATH_MAX 64
typedef struct BotNav_t BotNav;
typedef struct {
    vec3 points[BOT_PATH_MAX];     // waypoints (feet), the first one past the start
    unsigned char kinds[BOT_PATH_MAX]; // how to reach it: 0 walk, 1 jump, 2 drop
    int count;
    int reached;                   // 0: the goal was unreachable, the route ends closest to it
} BotPath;

typedef struct {
    int id;                         // NextBot id (manager counter from 1): bot_list / bot_kick index
    char socket_id[13];             // UTILS.randomString(12)
    char name[PLAYER_NAME_SIZE];
    BotType type;
    BotConfig config;
    BotSpawnOptions options;
    PlayerCosmetics cosmetics;
    PlayerCosmeticsV7 cosmetics_v7; // WP-F: head / back / shoes / pet / wrist
    PlayerSkins skins;
    int class_index, secondary_index, team, lives;
    unsigned char persistent:1, can_respawn:1, joined:1, cosmetics_rolled:1, pinned:1, pending:1, was_active:1;
    unsigned char quota:1;          // added by the server.cfg quota (removed by it too)
    unsigned char ads:1;            // this engagement aims down sights (players ADS most fights)
    unsigned char remove:1;         // lives ran out with removeOnDeath: removed after this tick
    int level;                      // the account level PLAYER_INFO shows (rolled when config.level is 0)
    int premium, featured;
    char clan[GAME_BOT_CLAN_SIZE];
    float last_health;              // a drop means someone hit us: turn to them
    vec3 pin_position;              // bot_place / spawnPoint
    float pin_yaw;
    int uid;                        // Player.uid (kept across respawns and map changes)
    Player *player;                 // NULL until spawned / after a map change
    float spawn_delay;              // ms until the respawn (useSpawnDelay)
    int seq;                        // input seq counter

    // AI (bots.js Bot / NextBot actor state). Times in ms, angles in radians.
    int tick;
    float action_freq;              // target re-acquire timer
    int target_uid;                 // -1 none
    float yaw, pitch;               // the look the AI steers (input[3] / input[2])
    unsigned char fire_prev:1, has_roam:1, sees_target:1, have_seen:1, crouch_hold:1, should_dodge:1, coward:1, first_dodge:1;
    vec3 start;                     // startP
    vec3 roam_target;
    float roam_timer;
    BotState state;
    float state_time;
    float reaction;                 // ms before a freshly seen target is fired at
    float not_seen;                 // ms without line of sight in Attack
    float aim_offset_yaw, aim_offset_pitch, aim_offset_timer;
    float scan_timer;
    int dodge, dodge_until;
    float coward_at;
    float hide_timer;
    float ignore_timer;             // DropCurrentTarget(ms): no re-lock on ignore_uid
    int ignore_uid;
    vec3 last_known;                // the target's last seen position
    vec3 stuck_pos;
    float stuck_timer, stuck_check, recover_timer;
    int recover_dir;
    float probe_timer;
    int probe_move;                 // cached obstacle steering: move code offset, -99 none
    int probe_jump;
    Input mimic;                    // last input of the mimicked target
    BotPath path;                   // the route bot_walk follows
    int path_index;
    vec3 path_goal;
    float path_timer;               // ms until the route may be recomputed
    int path_stranded;              // the goal is unreachable and we are as close as it gets
    // v10.0.2 botPaths walking (Bot.pathInfo / pathTarget, usePathing; shared/src/bots.c bot_next_path_node)
    int route_path;                 // pathIndex: Map.bot_path_list index, -1 = null
    int route_node;                 // pathNodeIndex
    int route_dir;                  // pathDirection +1 / -1
    unsigned char has_route_target; // pathTarget != null
    vec3 route_target;
    BotObjective objective;         // the mode objective it plays (shared/src/bot_objective.c)
} GameBot;

typedef struct GameBots_t {
    GameBot list[GAME_BOTS_MAX];
    int count;
    int next_id;                    // NextBot id counter
    int next_uid;                   // fallback uid counter (no alloc_uid hook)
    BotType type;                   // bot_type
    BotConfig config[2];            // manager defaults per type (BotManager.config / NextBotManager.config)
    // server.cfg (KrunkNative addition): keep `quota` bots, or (fill) fill the game up to
    // `quota` players with bots that leave as humans join. 0 = off. Fill keeps an empty lobby
    // empty: with no human in the game the quota bots stay `quota_grace` ms, then all leave.
    int quota;
    unsigned char quota_fill:1;
    int slots;                      // lobby size for the quota (0 = game->config.max_players, -1 = no quota bots: ranked / custom)
    float quota_timer;              // ms until the quota adds / removes the next bot
    float quota_grace;              // fill: ms the bots wait for a human after the last one left
    float quota_empty;              // fill: ms the game has had no human (while quota bots remain)
    BotNav *nav;                    // the current map's grid (built on first use)
    const void *nav_failed_key;     // a map the grid could not be built for
    // KrunkNative Object Manager map edits: the grid is stale (bot_nav_invalidate); it is rebuilt
    // on the next use at least BOT_NAV_REBUILD_SECONDS after the last rebuild (Game.now)
    unsigned char nav_dirty:1;
    float nav_built_at;
    int path_budget;                // A* searches left this tick
    unsigned char stopped:1;        // bot_stop is in effect (bot_start / bot_stop off resume)
    // KrunkNative chat `autofill` (game_bots_set_autofill): off stops the quota fill (its bots leave);
    // forced: turned on by a command, so a hosted custom game fills too (never ranked: server_core.c)
    unsigned char autofill_off:1;
    unsigned char autofill_forced:1;
} GameBots;

GameBots *game_bots(Game*);                      // lazily allocated manager (NULL on OOM)
void game_bots_free(Game*);
int game_bots_count(const Game*);                // bots in the list (spawned or not)
GameBot *game_bot_of(Game*, const Player*);      // the bot driving `player`, NULL for humans
int game_player_is_bot(const Game*, const Player*);

// BotManager.add(spawnPoint, spawn, skipSync, overrides): a new bot with a generated identity;
// `a` (NULL = defaults) are bot_add's key=value overrides. at: bot_place position (NULL = spawn
// point). Spawns it now when the game is running. NULL when the list is full.
GameBot *game_bots_add(Game*, const BotAssignments *a, const vec3 *at, float yaw);
void game_bots_remove(Game*, GameBot*);          // BotManager.remove: LEAVE, frees the player
int  game_bots_remove_all(Game*);                // returns how many
void game_bots_reset(Game*);                     // BotManager.reset: the non-persistent ones
// bots.js setupLocal: the canonical 5-bot FTUE roster (persistent, fighting each other).
void game_bots_setup_local(Game*);
// BotManager.configure(patch): every live bot and the defaults; returns the live bot count.
int  game_bots_configure(Game*, const BotAssignments*);
// bot_type: switching removes every bot; returns how many.
int  game_bots_set_type(Game*, BotType);
// server.cfg bots / bot_quota (+ bot_difficulty applied with game_bots_configure).
void game_bots_set_quota(Game*, int quota, int fill);
void game_bots_set_quota_grace(Game*, float seconds);   // fill: empty-lobby grace (default 10 s)
// KrunkNative chat `autofill on|off|<n>`: the quota fill at run time, for the server's lifetime. Off:
// every quota bot leaves now and none come; on: fill to `quota` players (> 0), else the quota the
// server had, else the lobby size. Returns how many bots left.
int game_bots_set_autofill(Game*, int on, int quota);
// 1 while a fill / keep quota runs (on, a quota, a lobby that takes quota bots); *quota its size
int game_bots_autofill(const Game*, int *quota);
// game_players_add of a human (authority): a fill quota bot over the lobby size leaves at once
// (from the human's team in team modes), so a join never waits for the 300 ms quota tick.
void game_bots_human_joined(Game*, const Player*);

// game.init: every Player was freed by the host; forget them, drop the non-persistent bots
// v7 resets, respawn the rest on the next tick.
void game_bots_on_init(Game*);
// bots.update(delta * deltaMlt) before players.update (game_tick_world, authority).
void game_bots_tick(Game*, float delta);
// PLAYER_INFO of every spawned bot to `uid` (a joining connection).
void game_bots_send_infos(Game*, int uid);

// Parsing (server/index.ts parseBotConfigAssignments / parseBotConfigValue) and formatting.
void bot_parse_assignments(int argc, char **argv, BotAssignments *out);
int  bot_parse_difficulty(const char *raw, float *out);  // preset name or number > 0
// botDifficultyConfig(value): the difficulty preset's accuracy / range / attack flags.
void bot_difficulty_assignments(float difficulty, BotAssignments *out);
void bot_describe_config(const BotAssignments *a, int all, char *out, size_t size); // describeBotConfig
void bot_config_defaults(BotType, BotConfig*);
const char *bot_type_name(BotType);
int  bot_parse_type(const char *raw, BotType *out);       // parseBotType
const char *bot_state_name(BotState);
// names.js generateName: a handle (80%) or colour + separator + noun
void bot_generate_name(char *out, size_t size);

// shared/src/bot_nav.c
BotNav *bot_nav_for(Game*);                       // the current map's grid (NULL: none could be built)
void bot_nav_free(BotNav*);
// The map's solid boxes changed (Object Manager map edits): rebuild the grid on a later use, at
// most once every BOT_NAV_REBUILD_SECONDS (the old grid serves until then)
#define BOT_NAV_REBUILD_SECONDS 1.0f
void bot_nav_invalidate(Game*);
int  bot_nav_node_count(const BotNav*);
int  bot_nav_find_path(Game*, vec3 from, vec3 to, BotPath *out); // 1 = a route (maybe partial)
int  bot_nav_random_point(Game*, vec3 *out);      // a walkable point of the largest area
int  bot_nav_random_point_from(Game*, const vec3 *from, vec3 *out); // ... of the area under `from`
int  bot_object_blocks_player(const Object*);     // player_do_map_collisions' solid objects
int  bot_hazard_at(const Game*, vec3 feet);       // inside a jump pad / kill volume

// shared/src/bot_names.c
extern const char *const g_bot_name_colors[];
extern const int g_bot_name_colors_COUNT;
extern const char *const g_bot_name_nouns[];
extern const int g_bot_name_nouns_COUNT;
extern const char *const g_bot_name_separators[];
extern const int g_bot_name_separators_COUNT;
extern const char *const g_bot_name_handles[];
extern const int g_bot_name_handles_COUNT;
