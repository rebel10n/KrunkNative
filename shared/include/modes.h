#pragma once
// Krunker v3.1.1 game modes (src/data/modes.js, the 24-entry `modes` table; ids are
// table indices, exactly as the server's "init" modeIndex). Server-authoritative:
// the rules below run on the authority (native server / offline local server);
// clients only mirror the state through the MODE_* packets (shared.h) and use the
// static config (teams, speed multipliers, realMov, ...) for prediction.
#include <stddef.h>
#include <stdint.h>
#include <common.h>

struct Game_t;
struct Player_t;

typedef enum {
    MODE_ID_FFA = 0,       // Free for All
    MODE_ID_TDM,           // Team Deathmatch
    MODE_ID_POINT,         // Hardpoint
    MODE_ID_CTF,           // Capture the Flag
    MODE_ID_BHOP,          // Parkour
    MODE_ID_HIDE,          // Hide & Seek
    MODE_ID_INFECT,        // Infected
    MODE_ID_RACE,          // Race
    MODE_ID_LMS,           // Last Man Standing
    MODE_ID_SIMON,         // Simon Says
    MODE_ID_GUN,           // Gun Game
    MODE_ID_PROP,          // Prop Hunt
    MODE_ID_BOSS,          // Boss Hunt
    MODE_ID_RANKED_POINT,  // Ranked (objective variant, modes[13])
    MODE_ID_RANKED,        // Ranked (first to 10 kills, modes[14])
    MODE_ID_STALK,         // Stalker
    MODE_ID_KING,          // King of the Hill
    MODE_ID_OINC,          // One in the Chamber
    MODE_ID_TRADE,         // Trade
    MODE_ID_KC,            // Kill Confirmed
    MODE_ID_DEF,           // Defuse
    MODE_ID_SHRP,          // Sharp Shooter
    MODE_ID_TRAI,          // Traitor
    MODE_ID_RAID,          // Raid
    MODE_COUNT
} GameModeId;

#define MODE_TEAMS 3                 // team 0 (none / FFA), 1 and 2
#define MODE_MAX_WAIT_TIMERS 4
#define MODE_MAX_OBJECTIVES 16
#define MODE_MAX_FLAGS 4
#define MODE_MAX_SPAWNABLES 32
#define MODE_TEXT_SIZE 32
#define MODE_GAME_TIME_CONFIG (-1.0f) // gameTime == null: config.gameTime minutes

// Krunker v3.1.1 src/config.js mode constants (seconds / world units).
#define MODE_FLAG_SCALE 12.0f        // config.flagScale
#define MODE_FLAG_OFF 6.0f           // config.flagOff
#define MODE_FLAG_HOFF 15.0f         // config.flagHOff (carried flag above the carrier)
#define MODE_FLAG_ZONE_SIZE 24.0f    // config.flagZoneS
#define MODE_FLAG_ZONE_HEIGHT 36.0f  // config.flagZoneH
#define MODE_FLAG_UPDATE 2.5f        // config.flagUpdate (carrier ping)
#define MODE_OBJECTIVE_TICK 1.5f     // game.update objectiveTimer
#define MODE_OBJECTIVE_ROTATE 60.0f  // nextObjective: 60000 ms
#define MODE_WEAPON_ROTATE 20.0f     // nextWeapon: 20000 ms
#define MODE_CONVERT_TIME_BONUS 10.0f// playerCollisions: gameTimer += 10000
#define MODE_TAG_TIMER 8.0f          // spawnables[0].timer (Kill Confirmed tag)
#define MODE_TAG_SCALE 6.0f          // spawnables[0].scale
#define MODE_TAG_YOFF 6.0f           // spawnables[0].yOff
#define MODE_INFECTED_SKIN 5857617   // config.modeSpecific.infected.skin
#define MODE_INFECTED_HAT 122        // players.js jLrR6: Infected wear hatIndex 122
#define MODE_STALKER_SKIN 0          // config.modeSpecific.stalker.skin (body all 0 too)

typedef void (*ModeTriggerFn)(struct Game_t*, float overflow);   // waitTimers[i].trigger(game, -time)
typedef int (*ModeConditionFn)(const struct Game_t*);           // 0: the round ends now
typedef void (*ModeGameStartFn)(struct Game_t*);
typedef void (*ModeKillFn)(struct Game_t*, struct Player_t *victim, struct Player_t *killer, int weapon_id);
typedef int (*ModeSortFn)(const struct Player_t*, const struct Player_t*); // < 0: a ranks first

// winCondition result: a team (1/2), a player, or nobody.
typedef struct {
    int team;
    struct Player_t *player;
} ModeWinner;

typedef ModeWinner (*ModeWinFn)(const struct Game_t*);

typedef struct {
    float time;              // seconds
    const char *msg;         // "picking seeker in " + m:ss
    ModeTriggerFn trigger;
    unsigned char no_join:1; // noJoin
    unsigned char no_clear:1;// noClear: keep the last game message
    unsigned char comp:1;    // comp: spawn spread around the comp spawns
    unsigned char cont_time:1; // contTime: the round clock keeps running
} ModeWaitTimer;

typedef struct {
    int id;                  // GameModeId
    const char *key;         // modes.js id ("ffa")
    const char *name;        // "Free for All"
    const char *alias;       // "ffa"

    unsigned char real_movement:1;   // realMov
    unsigned char no_weapons:1;      // noWeap: class 9 (Runner)
    unsigned char no_regen:1;
    unsigned char no_reloads:1;
    unsigned char no_pickups:1;
    unsigned char no_streaks:1;
    unsigned char no_clan:1;
    unsigned char teams:1;
    unsigned char dmg_team:1;        // dmgTeam
    unsigned char site:1;
    unsigned char flags:1;
    unsigned char convert_team:1;    // convTeam set (see conv_team)
    unsigned char kill_on_convert:1; // killConv
    unsigned char friendly:1;
    unsigned char clan_war:1;
    unsigned char objective:1;
    unsigned char obj_weapon:1;      // objWep
    unsigned char no_score_c:1;      // noScoreC: player score does not feed the team score
    unsigned char no_score_k:1;      // noScoreK: kill rewards give no score
    unsigned char no_kick:1;
    unsigned char no_reward:1;
    unsigned char no_assists:1;
    unsigned char no_kill_feed:1;
    unsigned char no_dead_chat:1;
    unsigned char hide_names:1;
    unsigned char show_team:1;
    unsigned char team_sort:1;
    unsigned char fake_names:1;
    unsigned char timed:1;           // the round clock counts up
    unsigned char show_ms:1;
    unsigned char keep_loadout:1;
    unsigned char kill_sort:1;
    unsigned char is_ranked:1;
    unsigned char need_all:1;
    unsigned char inf_end_timer:1;
    unsigned char can_trade:1;
    unsigned char prevent_custom:1;  // preventCustomGames: not offered in Host Game
    unsigned char no_free_cam:1;

    const char *team_options[2];     // teamOptions, NULL: config team names
    const char *team_colors[2];      // teamColors, NULL: default HUD colours
    const char *conv_word;           // convWord
    const char *leader_stat;         // leaderStat, NULL = "score"
    const char *const *end_stats;    // endStats, NULL = config.endStats
    int conv_team;                   // convTeam (0 none)

    float game_time;                 // seconds, MODE_GAME_TIME_CONFIG = config.gameTime
    float force_spawn;               // forceSpawn (seconds, 0 none)
    int min_players;
    int lives;
    int score_limit;
    int obj_score;                   // objScore (0 = 10)
    int tag_limit;
    int reward_mlt;
    int send_rate;                   // sendR (0 default)

    const int *starting_loadout;
    int starting_loadout_size;
    const int *weapon_order;
    int weapon_order_size;

    int team_class[MODE_TEAMS];      // teamClass, -1 none
    float speed_mlt[MODE_TEAMS];     // spdMlt per team, 1 default
    int force_class;                 // forceClass, -1 none
    int health;                      // aK3 (0 = class health)
    int ammo_limit;                  // ammoLimit (0 none)
    float hitbox_pad;                // hitBoxPad (0 = 1)

    // render.js initScene(map, mode) overrides: `q.x != null ? q.x : map.x`. Only used
    // when the matching has_* flag is set (Stalker); colours are 0xRRGGBB.
    int amb_ind;                     // ambInd: ambient sound index (0 = the map's)
    unsigned char has_sky_col:1;
    unsigned char has_fog_col:1;
    unsigned char has_fog_dist:1;
    unsigned char has_light_col:1;
    unsigned char has_amb_col:1;
    int sky_col;                     // skyCol: clear colour (renderer.init also skips the sky dome)
    int fog_col;                     // fogC
    float fog_dist;                  // fogD (THREE.Fog far)
    int light_col;                   // lightCol: the directional light's colour
    int amb_col;                     // ambCol: the ambient light's colour

    const ModeWaitTimer *wait_timers;
    int wait_timer_count;
    ModeConditionFn condition;       // condition[0] (every v3.1.1 mode has at most one)
    ModeWinFn win_condition;
    ModeSortFn end_sort;
    ModeGameStartFn game_start;
    ModeKillFn on_kill;
} GameModeConfig;

extern const GameModeConfig g_default_game_mode_config;
extern const GameModeConfig g_mode_configs[MODE_COUNT];
// map.js rotationModes: the public rotation (FFA, TDM, Hardpoint, CTF, Kill Confirmed).
extern const int g_rotation_modes[5];

// Per-player mode stats (Krunker Player fields used by modes.js / endStats).
typedef struct {
    int lives;                // Krunker lives (0 = out / unlimited when the mode has none)
    int caps;                 // CTF captures
    int confirms;             // Kill Confirmed
    int denies;
    int weapon_tier;          // Gun Game
    int setbacks;
    int melees;               // melee kills
    int convs;                // infections / finds
    int objective_score;      // objectiveScore
    int diffuses;
    int flag;                 // carried flag index + 1, 0 none
    unsigned char locked:1;   // lockPlayer: inputs ignored (seekers)
    unsigned char no_regen:1; // a role with regen = 0 (Boss, Stalker)
    unsigned char finished:1; // Race: reached the target score
    float race_time;          // Race: game clock when finished (seconds)
    unsigned char has_timer:1;// Krunker player.timer set (Race finish / Raid boss kill): the "time" end stat
    float timer;              // player.timer = game.lastTimer: the round clock shown then (seconds)
    float respawn_timer;      // forceSpawn countdown (seconds, 0 none)
} ModePlayerState;

typedef struct {
    vec3 position;            // centre
    vec3 half;                // half extents (width, height, length)
    int link;                 // objLink (obi)
} ModeObjective;

typedef struct {
    vec3 origin;              // orgX/Y/Z (centre of the crystal)
    vec3 position;
    vec3 ping;                // img: where the HUD shows it (carrier, refreshed every flagUpdate)
    int team;                 // 1 / 2
    int carrier_uid;          // -1 none
    float ping_timer;
} ModeFlag;

typedef struct {
    int sid;                  // network id
    vec3 position;            // centre
    int team;                 // the victim's team
    float timer;              // seconds left
    unsigned char active:1;
} ModeSpawnable;

typedef struct {
    float timer;              // seconds until it blows (40 s)
    int team;
    vec3 position;
    unsigned char active:1;
} ModeBomb;

typedef struct GameMode_t {
    GameModeConfig config;

    int team_scores[MODE_TEAMS];             // game.teams
    char flag_info[MODE_TEAMS][MODE_TEXT_SIZE]; // teamFlags: "AT BASE" / "AWAY" / carrier name

    ModeWaitTimer wait_timers[MODE_MAX_WAIT_TIMERS]; // game.waitTimers (copies)
    int wait_timer_count;
    int wait_second_sent;                    // lastTimerW
    unsigned char waiting_sent:1;            // the "waiting for players" message is up
    unsigned char message_sent:1;            // some game message is up
    unsigned char started:1;                 // gameStart ran
    unsigned char skip:1;                    // gameTimer = "skip": end this tick

    ModeConditionFn conditions[3];
    int condition_count;
    int score_limit;
    int min_players;
    float force_spawn;
    int lives;                               // config.lives || mode.lives

    int active_objective;                    // activeObjective (Hardpoint index / Sharp Shooter weapon)
    float objective_timer;
    int objective_count;
    ModeObjective objectives[MODE_MAX_OBJECTIVES];

    int flag_count;
    ModeFlag flags[MODE_MAX_FLAGS];

    ModeSpawnable spawnables[MODE_MAX_SPAWNABLES];
    int next_spawnable_sid;

    ModeBomb bomb;
    int target_score;                        // Race
    float round_clock;                       // Race / timed: elapsed seconds
} GameMode;

// Mode table
int mode_count(void);                                // MODE_COUNT
const GameModeConfig *mode_config(int id);           // NULL for an unknown id
const GameModeConfig *mode_list(int *count);         // every v3.1.1 mode, indexed by id
int mode_find(const char *query);                    // id by alias/key/name/number, -1 none (first "ranked" wins)
const char *mode_name(int id);                       // "unknown mode" for an unknown id
const char *mode_team_name(const struct Game_t*, int team); // teamOptions[team - 1] / config team names
int mode_hostable(int id);                           // offered in Host Game (not preventCustomGames)

// Lifecycle (game_init)
GameMode *mode_init(int id);
void mode_fini(GameMode*);
void mode_round_start(struct Game_t*);              // finishInit: wait timers, conditions, objectives, flags, gameStart

// Round flow (authority, called from game_tick)
enum { MODE_ROUND_HOLD, MODE_ROUND_RUN, MODE_ROUND_END };
int mode_tick_round(struct Game_t*, float delta);    // waitTimers + conditions: hold the clock / run it / endGame()
void mode_tick(struct Game_t*, float delta);         // objectives, flags, tags, conversions, respawns
int mode_waiting(const struct Game_t*);              // waitTimers active and the clock is not running
ModeWinner mode_pick_winner(struct Game_t*);         // endGame(): winner by winCondition / team score / best player
void mode_sort_players(struct Game_t*, struct Player_t **players, size_t count); // endSort / orderByScore
int mode_active_count(const struct Game_t*);

// Player hooks (authority)
void mode_player_added(struct Game_t*, struct Player_t*);    // players.add: setTeam + lives
int mode_can_spawn(const struct Game_t*, const struct Player_t*); // lives left / not locked out
void mode_player_spawned(struct Game_t*, struct Player_t*);  // setClass overrides applied: loadout + spawn point
void mode_player_left(struct Game_t*, struct Player_t*);     // drop its flag
void mode_on_kill(struct Game_t*, struct Player_t *victim, struct Player_t *killer, int weapon_id);
int mode_score_gain(const struct Game_t*, int amount, int is_kill); // what players.score adds (noScoreK)
void mode_on_score(struct Game_t*, struct Player_t*, int amount, int is_kill); // objectiveScore + team score
float mode_melee_damage(struct Game_t*, struct Player_t *dealer, struct Player_t *victim, float damage); // onMelee
int mode_input_locked(const struct Player_t*);               // lockPlayer
int mode_team_score(const struct Game_t*, int team);
void mode_send_state(struct Game_t*, int uid);       // NET_PACKET_MODE_STATE to `uid` (NET_TARGET_ALL: one per player)
void mode_send_end(struct Game_t*, ModeWinner, struct Player_t **sorted, size_t count); // NET_PACKET_MODE_END

// Client side (mirrors)
void mode_set_player_loadout(struct Player_t*, const int *weapons, int count, int keep_index); // updateLoadout
int mode_is_team(const struct Game_t*);                      // mode.teams
int mode_team_class(const struct Game_t*, int team);         // the class a team is forced to, -1 none
int mode_apply_outfit(const struct Game_t*, struct Player_t*); // jLrR6 mode outfits (Infected, Stalker) into Player.cosmetics; 1 = changed
