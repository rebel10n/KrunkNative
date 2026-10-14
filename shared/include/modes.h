#pragma once
// Krunker v7.1.4 game modes (src/data/gameModes.js, the 37-entry engine table; modes/<id>.js;
// doc/v7/map-logic.md §4.1). Ids are table indices, exactly as the server's "init" modeIndex and
// the map / config "modes" lists. Server-authoritative: the rules below run on the authority
// (native server / offline sandbox server); clients only mirror the state through the mode
// packets (shared.h, NET_PACKET_MODE_STATE.. and the WP-D block 140-149) and use the static
// config (teams, speed multipliers, realMov, ...) for prediction.
//
// v3.1.1 -> v7.1.4: ids 0-12 and 15-23 keep their index; v3's 13 / 14 (both Ranked) are Classic
// FFA and Deposit in v7. v7.1.4 has no ranked mode: a ranked match is a comp lobby (game.isComp,
// config.hostTypes[1] "Comp Server Setup") playing Hardpoint (services/brain/ranked.c,
// server/src/accounts.c). Ids 37 / 38 held KrunkNative's own Ranked modes until the v7 ranked
// port and stay reserved for ranked (doc/v7/decisions.md "Ranked"): never reuse them.
#include <stddef.h>
#include <stdint.h>
#include <common.h>

struct Game_t;
struct Player_t;
struct Object_t;

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
    MODE_ID_CLAS = 13,     // Classic FFA (v3.1.1 13: Ranked objective variant)
    MODE_ID_DEPO = 14,     // Deposit (v3.1.1 14: Ranked)
    MODE_ID_STALK,         // Stalker
    MODE_ID_KING,          // King of the Hill
    MODE_ID_OINC,          // One in the Chamber
    MODE_ID_TRADE,         // Trade
    MODE_ID_KC,            // Kill Confirmed
    MODE_ID_DEF,           // Defuse
    MODE_ID_SHRP,          // Sharp Shooter
    MODE_ID_TRAI,          // Traitor
    MODE_ID_RAID,          // Raid
    MODE_ID_BLITZ = 24,    // Blitz
    MODE_ID_DOM,           // Domination
    MODE_ID_SDM,           // Squad Deathmatch
    MODE_ID_KRANK,         // Kranked FFA
    MODE_ID_TDF,           // Team Defender
    MODE_ID_DEPOFFA,       // Deposit FFA
    MODE_ID_CAR,           // Carrier
    MODE_ID_AON,           // All or Nothing
    MODE_ID_MD,            // Mag Dump
    MODE_ID_CHS,           // Chaos Snipers
    MODE_ID_BHFFA,         // Bighead FFA
    MODE_ID_ZOM,           // Zombies (economy, waves: map-logic WP-H stage 2, mode_zombies.c)
    MODE_ID_IMP,           // Impulsed
    MODE_V7_TABLE_SIZE,    // 37: gameModes.js
    MODE_COUNT = MODE_V7_TABLE_SIZE
} GameModeId;

// Reserved for ranked (doc/v7/decisions.md "Ranked"): KrunkNative's former Ranked modes. No table
// entry, never assigned to another mode; mode_config() returns NULL for them.
#define MODE_ID_RESERVED_RANKED_37 37
#define MODE_ID_RESERVED_RANKED_38 38

// sdm multiTeam: team 0 (none / FFA) plus teams 1..5 (players.js setTeam, nameTeam1..5, t1..5Dmg)
#define MODE_TEAMS 6
#define MODE_MAX_WAIT_TIMERS 6
#define MODE_MAX_OBJECTIVES 16
#define MODE_MAX_FLAGS 8
#define MODE_MAX_DOM_ZONES 16
#define MODE_MAX_SPAWNABLES 64
#define MODE_TEXT_SIZE 32
#define MODE_GAME_TIME_CONFIG (-1.0f) // gameTime == null: config.gameTime minutes

// Krunker v7.1.4 src/config.js mode constants (seconds / world units).
#define MODE_FLAG_SCALE 12.0f        // config.flagScale
#define MODE_FLAG_OFF 6.0f           // config.flagOff
#define MODE_FLAG_HOFF 15.0f         // config.flagHOff (carried flag above the carrier)
#define MODE_FLAG_ZONE_SIZE 24.0f    // config.flagZoneS
#define MODE_FLAG_ZONE_HEIGHT 36.0f  // config.flagZoneH
#define MODE_FLAG_UPDATE 2.5f        // config.flagUpdate (carrier ping)
#define MODE_FLAG_IDLE 45.0f         // config.flagIdle: a dropped flag goes home (v7, game.js updateFlags)
#define MODE_OBJECTIVE_TICK 1.5f     // point / king / dom / tdf objectiveTimer
#define MODE_OBJECTIVE_ROTATE 60.0f  // nextObjective: (config.objtvTime || 1) * 60000 ms
#define MODE_WEAPON_ROTATE 20.0f     // nextWeapon: 20000 ms
#define MODE_CONVERT_TIME_BONUS 10.0f// playerCollisions: gameTimer += 10000
#define MODE_INTERMISSION 10.0f      // endGame rounds: (config.intermTmr || 10) * 1000
#define MODE_OVERTIME 60.0f          // noDraws overtime: gameTimer = 60000 ("+60 Seconds")
#define MODE_TAG_TIMER 8.0f          // medals.js Tag (0).timer (Kill Confirmed tag)
#define MODE_TAG_SCALE 6.0f          // Tag.scale
#define MODE_TAG_YOFF 6.0f           // Tag.yOff
#define MODE_INFECTED_SKIN 5857617   // modelOverrides.Infected.skinColor
#define MODE_INFECTED_HAT 122        // modelOverrides.Infected.headIndex
#define MODE_STALKER_SKIN 0          // modelOverrides.Stalker (all colours 0, noCosmetics)

typedef void (*ModeTriggerFn)(struct Game_t*, float overflow);   // waitTimers[i].trigger(game, -time)
typedef int (*ModeConditionFn)(const struct Game_t*);           // 0: the round ends now
typedef void (*ModeGameStartFn)(struct Game_t*);
typedef void (*ModeKillFn)(struct Game_t*, struct Player_t *victim, struct Player_t *killer, int weapon_id);
typedef int (*ModeSortFn)(const struct Player_t*, const struct Player_t*); // < 0: a ranks first
typedef void (*ModeUpdateFn)(struct Game_t*, float delta);      // mode.update (server only, delta seconds)
typedef void (*ModeCollisionFn)(struct Game_t*, struct Player_t*); // mode.collisionCheck (player.onZone)
typedef float (*ModeMeleeFn)(struct Game_t*, struct Player_t *dealer, struct Player_t *victim, float damage); // v10 onMelee factor

// winCondition result: a team (1..5), a player, or nobody.
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
    unsigned char dont_wait:1; // dontWait: runs without minPlayers (round intermission)
} ModeWaitTimer;

typedef struct {
    int id;                  // GameModeId
    const char *key;         // modes/<id>.js id ("ffa")
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
    unsigned char can_trade:1;
    unsigned char prevent_custom:1;  // preventCustomGames: not offered in Host Game
    unsigned char no_free_cam:1;

    const char *team_options[2];     // teamOptions, NULL: config team names
    const char *team_colors[2];      // teamColors, NULL: default HUD colours
    const char *conv_word;           // convWord
    const char *leader_stat;         // the first leaderStats entry, NULL = "score"
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
    int health;                      // maxHealth (v3.1.1 aK3; 0 = class health)
    int ammo_limit;                  // ammoLimit (0 none unless has_ammo_limit)
    float hitbox_pad;                // hitBoxPad (0 = 1; v7: unused, the server config hitBoxPad applies)
    // WP-B v7.1.4 physPow knockback modifiers (players.js shoot): impulseMlt (only with
    // has_impulse_mlt: 0 is valid, Zombies / Classic), noImpulseDist (full push at any distance),
    // nonObjImpulse (a fixed 0.04 push whatever the ray hits: Impulse mode).
    float impulse_mlt;
    unsigned char has_impulse_mlt:1;
    unsigned char no_impulse_dist:1;
    unsigned char non_obj_impulse:1;

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
    ModeConditionFn condition;       // condition[0] (every v7.1.4 mode has at most one)
    ModeWinFn win_condition;
    ModeSortFn end_sort;
    ModeGameStartFn game_start;
    ModeKillFn on_kill;

    // ---- WP-D: Krunker v7.1.4 mode keys (doc/v7/map-logic.md §4.1 "New mode keys")
    const char *sub;                 // sub: the warmup / intermission popup line ("Destroy the Enemy")
    const char *const *leader_stats; // leaderStats (client leaderboard columns), NULL = ["score"]
    ModeSortFn leader_sort;          // leaderSort (client), NULL = by score
    ModeUpdateFn update;             // update (server, game.update #23)
    ModeCollisionFn collision_check; // collisionCheck (sets player.onZone; king / dom)
    ModeGameStartFn on_pre_end;      // onPreEnd (endGame, before didWin)
    ModeGameStartFn on_end;          // onEnd (endGame, after the end packets)
    int event;                       // event index (aon 4, md 5), 0 none
    unsigned char party:1;           // party / tag "Party" (client lists)
    unsigned char dom:1;             // Domination zones
    unsigned char oneflag:1;         // neutral flag (tm -1), anyone carries it, no capture (tdf / car)
    unsigned char rush:1;            // Blitz: capture while your flag is away, the capper respawns, no "ac"
    unsigned char deposit:1;         // KR Tag deposits (depo / depoffa)
    unsigned char no_knives:1;       // noKnives: canThrow false
    unsigned char no_melee:1;        // noMelee: no knife slot
    unsigned char no_sliding:1;      // noSliding
    unsigned char no_wall_jump:1;    // noWallJ
    unsigned char fr_slid:1;         // frSlid
    unsigned char has_wall_jump:1;   // wallJ / wallJM set
    unsigned char has_ammo_limit:1;  // ammoLimit != null (aon: ammoLimit 0)
    unsigned char no_scr_limit:1;    // noScrLimit: no 13000 cap
    unsigned char double_xp:1;       // doubleXP
    unsigned char bighead:1;         // bighead: headMlt grows with kills
    unsigned char no_minimap:1;
    unsigned char bots_as_ai:1;      // botsAsAI
    unsigned char bot_bypass_class:1;// botBypassClass
    unsigned char no_client_kick:1;  // noClientKick (v3.1.1 noKick on Trade)
    unsigned char no_hp_bars:1;      // noHPbars
    unsigned char log_time:1;        // logTime (Parkour / Race speed-run times, WP-B "gt")
    unsigned char own_time:1;        // ownTime (Parkour: per-player clock when gameTime is 0)
    unsigned char allow_reset:1;     // allowReset (input [13] resets, WP-B)
    unsigned char show_hour:1;       // showHour
    unsigned char prop:1;            // prop (Prop Hunt disguises)
    unsigned char no_class_scr:1;    // noClassScr
    unsigned char team_see:1;        // teamSee (Simon Says)
    unsigned char finite_ammo:1;     // finiteAmmo (Zombies: reserve ammo)
    unsigned char unrestricted_weapons:1; // unrestrictedWeapons (Zombies)
    unsigned char force_charge:1;    // forceCharge (Zombies)
    float points_per_hit;            // pointsPerHit (Zombies 0.085 x damage)
    int starter_score;               // starterScore (Zombies 150)
    float regen_mlt;                 // regenMlt (Zombies 0.1; 0 = 1)
    int multi_team;                // multiTeam: number of teams (sdm 4), 0 = 2 when teams
    float capture_time;              // dom captureTime (seconds, 10)
    float lockout_mlt;               // dom lockoutMlt (1.5)
    float per_player_mlt;            // dom perPlayerMlt (0.2)
    float kill_mlt;                  // killMlt: kill score x (killMlt + player.killMlt); 0 none
    float hill_mlt;                  // king hillMlt: player.killMlt += per Defender kill
    float carrier_mlt;               // oneflag kill score: carrier / its team / everyone else
    float team_carrier_mlt;
    float non_carrier_mlt;
    int tag_hold_limit;              // tagHoldLimit (KR Tags carried)
    int max_per_cluster;             // maxPerCluster (KR Tags per dropped spawnable)
    float kranked_timer;             // krankedTimer (seconds, 15)
    float bonus_speed;               // bonuses.speed / reload / firerate while kranked (0 = 1)
    float bonus_reload;
    float bonus_fire_rate;
    float head_mlt;                  // bhffa headMlt (0.1 per kill)
    float head_mlt_max;              // headMltMax (3)
    float grav_mlt;                  // gravMlt (0 = 1)
    float jump_mlt;                  // jumpMlt (0 = 1)
    float ground_accel;              // groundAccel (0 = 1)
    float air_accel;                 // airAccel (no consumer in v7.1.4)
    float reload_mlt;                // reloadMlt (0 = 1)
    float wall_jump;                 // wallJ (has_wall_jump)
    float wall_jump_max;             // wallJM
    float wall_jump_d;               // wallJD (0 = class)
    float wall_jump_v;               // wallJV (0 = class)
    float wall_jump_yv;              // wallJYV (0 = 1)
    float expl_mlt;                  // explMlt: explosion damage to others (0 = 1)
    float expl_size_mlt;             // explSMlt: explosion radius (0 = 1)
    float rand_class_scr;            // randClassScr (class score to a random class; client stats)
    float view_dis;                  // viewDis: server relevance culling distance (0 none, WP-E)
    int name_dist;                   // nameDist (Trade)
    int prop_switches;               // propSwitches
    const int *disabled_streaks;     // disabledStreaks (streaks.js indices)
    int disabled_streak_count;

    // ---- Krunker v10.0.2 mode keys (doc/v10/gameplay.md section 6)
    ModeMeleeFn on_melee;            // onMelee as a melee damage factor (v10 Stalker); NULL = none
    unsigned char stalker_v10:1;     // the v10 Stalker rework: the stalker's leap (input[6]), wall stick,
                                     // coyote time and notWallJ walls; canSlide = isStalker (only it slides)
} GameModeConfig;

extern const GameModeConfig g_default_game_mode_config;
extern const GameModeConfig g_mode_configs[MODE_COUNT];   // Krunker v10.0.2's (shared/src/modes/modes.c)
// map.js rotationModes: the public rotation (FFA, Hardpoint, CTF, Kill Confirmed, Domination).
extern const int g_rotation_modes[5];

// Per-player mode stats (Krunker Player fields used by the modes / endStats).
typedef struct {
    int lives;                // Krunker lives (0 = out / unlimited when the mode has none)
    int caps;                 // CTF / Domination captures
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
    // ---- WP-D: v7.1.4 player fields
    int deposits;             // Deposit: KR Tags deposited
    int dog_tags;             // dogTags: KR Tags carried
    int pickups;              // Mag Dump: weapon boxes
    int on_zone;              // onZone: objective / dom zone uid the player stood on this tick, -1 none
    float kill_mlt;           // player.killMlt (king Defender bonus)
    unsigned char is_kranked:1; // Kranked: a kill armed the timer
    float kranked_timer;      // krankedTimer (seconds left)
    float head_mlt;           // bhffa headMlt (0 = 1)
    vec3 last_ground;         // players.setOnGround lastGround (kill drop points), tracked by mode_tick
    unsigned char has_last_ground:1;
    // ---- map-logic WP-H: Zombies endStats
    int ai_kills;             // aiKills: AIs this player killed (AI.kill, not silent)
    int rounds;               // rounds: waves that ended while this player was in the game
} ModePlayerState;

typedef struct {
    vec3 position;            // centre
    vec3 half;                // half extents (width, height, length)
    int link;                 // obi (raw)
    int uid;                  // v7 objective uid (obi - 1, else MapManager.addObjective's auto uid)
    int on_zone;              // king: players on the hill ("uobj")
} ModeObjective;

typedef struct {
    vec3 origin;              // orgX/Y/Z (centre of the crystal)
    vec3 position;
    vec3 ping;                // img: where the HUD shows it (carrier, refreshed every flagUpdate)
    int team;                 // 1 / 2, -1 neutral (tm -1)
    int carrier_uid;          // -1 none
    float ping_timer;
    float idle_timer;         // v7 idleTimer: a dropped flag goes home when it runs out (0 none)
    unsigned char neutral:1;  // tm -1: only oneflag modes use it, only oneflag modes ignore the others
    unsigned char active:1;   // resetFlags: in play for this mode
} ModeFlag;

// game.js domzones (MapManager.addDominationZone): uid = index in map->dom_zones
typedef struct {
    vec3 position;            // centre
    vec3 half;
    int captured_by;          // team, 0 none
    float capture_t;          // seconds of capture progress
    int last_team_on;         // 0 none
    int teams_on[MODE_TEAMS]; // teamsOn (team ids, in arrival order; a 2nd team stops the scan)
    int teams_on_count;
    int on_point;             // onPoint.length: players of the single team on it
    float mlt;                // capture speed
} ModeDomZone;

// game.map.manager.spawnables (medals.js getData)
typedef struct {
    int sid;                  // network id
    int type;                 // medals.js index (shared/include/spawnables.h SPAWNABLE_*)
    vec3 position;            // centre (y includes yOff)
    float half;               // width = length = scale * padMlt, height = width / 2
    int team;                 // the owner's team
    float timer;              // seconds left (0: never despawns)
    int data_c;               // data.c (tag count, ammo), 0 none
    int data_w;               // data.wId (weapon box), -1 none
    float data_xd;            // data.xD (rotation)
    float data_zd;            // data.zD (pads)
    int owner_uid;            // playerSpecific pid, -1 none
    unsigned char active:1;
    unsigned char dont_destroy:1; // a partly picked KR Tag stack stays
} ModeSpawnable;

typedef struct {
    float timer;              // seconds until it blows (40 s)
    int team;
    vec3 position;
    unsigned char active:1;
} ModeBomb;

typedef struct GameMode_t {
    GameModeConfig config;

    int team_scores[MODE_TEAMS];             // game.teamScores
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

    int active_objective;                    // activeObjective (objective uid / Sharp Shooter weapon), -1 none
    float objective_timer;                   // objectiveTimer (seconds)
    float objective_time;                    // objtvTime: seconds left on the active objective
    int objective_count;
    ModeObjective objectives[MODE_MAX_OBJECTIVES];

    int flag_count;
    ModeFlag flags[MODE_MAX_FLAGS];
    int flag_team;                           // game.flagTeam: the carrier's team (oneflag kill scores), 0 none
    int flag_carrier_uid;                    // game.flagCarrier, -1 none

    ModeSpawnable spawnables[MODE_MAX_SPAWNABLES];
    int next_spawnable_sid;

    int dom_zone_count;
    ModeDomZone dom_zones[MODE_MAX_DOM_ZONES];

    ModeBomb bomb;
    int target_score;                        // Race
    float round_clock;                       // Race / timed: elapsed seconds

    // ---- WP-D: v7.1.4 rounds (endGame / finishInit: gamRounds, bstOfR, noDraws, intermTmr)
    int round_count;                         // roundCount: rounds left after this one
    int game_rounds;                         // config.gamRounds (best of: the match is won at gamRounds / 2)
    int team_round_wins[MODE_TEAMS];         // teamRoundWins
    unsigned char best_of:1;                 // bestOf
    unsigned char need_winner:1;             // needWinner (noDraws: overtime)
    unsigned char flipped_spawns:1;          // flippedSpawns: teams 1 / 2 swap spawns
    unsigned char in_overtime:1;
    unsigned char not_winnable:1;            // point comp: the trailing team can no longer catch up
    unsigned char has_round_wins:1;          // teamRoundWins != null
    float started_timer;                     // startedTimer: the round clock each round restarts from
    float lockout_timer;                     // lockoutTimer (seconds; "lkt")
} GameMode;

// Mode table
int mode_count(void);                                // MODE_COUNT
const GameModeConfig *mode_config(int id);           // NULL for an unknown id (the current ruleset's table)
const GameModeConfig *mode_list(int *count);         // every mode, indexed by id
int mode_find(const char *query);                    // id by alias/key/name/number, -1 none
const char *mode_name(int id);                       // "unknown mode" for an unknown id
const char *mode_team_name(const struct Game_t*, int team); // teamOptions[team - 1] / config team names
int mode_hostable(int id);                           // offered in Host Game (v7 table, not preventCustomGames)
int mode_team_count(const struct Game_t*);           // teams in play: multiTeam, 2 for team modes, 0 FFA

// Lifecycle (game_init)
GameMode *mode_init(int id);
void mode_fini(GameMode*);
void mode_round_start(struct Game_t*);              // finishInit: wait timers, conditions, objectives, flags, gameStart

// Round flow (authority, called from game_tick)
enum { MODE_ROUND_HOLD, MODE_ROUND_RUN, MODE_ROUND_END };
int mode_tick_round(struct Game_t*, float delta);    // waitTimers + conditions: hold the clock / run it / endGame()
void mode_tick(struct Game_t*, float delta);         // flags, dom zones, mode.update, conversions, respawns
int mode_waiting(const struct Game_t*);              // waitTimers active and the clock is not running
// endGame() rounds: 1 when the match goes on (next round with an intermission, or noDraws
// overtime), so the caller must not end it. Called by game.c before game_end_round.
int mode_end_game(struct Game_t*);
ModeWinner mode_pick_winner(struct Game_t*);         // endGame(): winner by winCondition / team score / best player
void mode_sort_players(struct Game_t*, struct Player_t **players, size_t count); // endSort / orderByScore
int mode_active_count(const struct Game_t*);
void mode_on_game_end(struct Game_t*);               // onPreEnd + onEnd hooks (game_end_round)

// Player hooks (authority)
void mode_player_added(struct Game_t*, struct Player_t*);    // players.add: setTeam + lives
int mode_can_spawn(const struct Game_t*, const struct Player_t*); // lives left / not locked out
void mode_player_spawned(struct Game_t*, struct Player_t*);  // setClass overrides applied: loadout + spawn point
void mode_player_left(struct Game_t*, struct Player_t*);     // drop its flag
void mode_on_kill(struct Game_t*, struct Player_t *victim, struct Player_t *killer, int weapon_id);
int mode_score_gain(const struct Game_t*, int amount, int is_kill); // what players.score adds (noScoreK)
// players.score multipliers before anything is added (is_kill: 1 kill reward, 2 objective, 0
// other): killMlt + player.killMlt, the oneflag carrier multipliers. The amount to use.
int mode_score_scale(const struct Game_t*, const struct Player_t*, int amount, int is_kill);
int mode_score_cap(const struct Game_t*);             // 0: no 13000 cap (noScrLimit)
void mode_on_score(struct Game_t*, struct Player_t*, int amount, int is_kill); // objectiveScore + team score
float mode_melee_damage(struct Game_t*, struct Player_t *dealer, struct Player_t *victim, float damage); // onMelee
int mode_input_locked(const struct Player_t*);               // lockPlayer
int mode_team_score(const struct Game_t*, int team);
void mode_send_state(struct Game_t*, int uid);       // NET_PACKET_MODE_STATE to `uid` (NET_TARGET_ALL: one per player)
void mode_send_end(struct Game_t*, ModeWinner, struct Player_t **sorted, size_t count); // NET_PACKET_MODE_END
// WP-E: one mode.leaderStats value of a player as v7 syncLeaders sends it (config.endForm for
// objective / confirms / denies / dmg / zombies / downs / points, else the player field).
int mode_leader_stat_value(const struct Player_t*, const char *key);
// players.syncLeaders order: leaderSort, else killSort, else orderByScore (stable).
void mode_sort_leaders(const struct Game_t*, struct Player_t **players, size_t count);

// ---- WP-D: v7.1.4 mode keys read by the simulation (player.c / projectile.c / effects.c are
// WP-B / player-B files; they call these instead of reading GameModeConfig). Every accessor
// takes a NULL game / no mode and then returns the "no mode" value.
float mode_grav_mlt(const struct Game_t*);           // gravMlt (1)
float mode_jump_mlt(const struct Game_t*);           // jumpMlt (1)
float mode_ground_accel(const struct Game_t*);       // groundAccel (1), ground and air accel
int mode_fr_slid(const struct Game_t*);              // frSlid
int mode_no_sliding(const struct Game_t*);           // noSliding (perks 4 / 5 still slide)
int mode_no_wall_jump(const struct Game_t*);         // noWallJ: wallJP 0
// wallJ / wallJM / wallJD / wallJV / wallJYV: 1 when the mode sets them (out parameters untouched otherwise)
int mode_wall_jump(const struct Game_t*, float *wall_jump, float *wall_jump_max, float *d, float *v, float *yv);
float mode_reload_mlt(const struct Game_t*, const struct Player_t*); // reloadMlt x kranked bonuses.reload
float mode_fire_rate_mlt(const struct Game_t*, const struct Player_t*); // kranked bonuses.firerate
float mode_speed_mlt(const struct Game_t*, const struct Player_t*);     // kranked bonuses.speed
int mode_ammo_limit(const struct Game_t*, int weapon_ammo); // ammoLimit != null ? ammoLimit : weapon ammo
int mode_no_knives(const struct Game_t*);            // noKnives: canThrow false
int mode_no_melee(const struct Game_t*);             // noMelee: no knife in the loadout
float mode_explosion_damage_mlt(const struct Game_t*, const struct Player_t *source, const struct Player_t *target); // explMlt (not the source)
float mode_explosion_size_mlt(const struct Game_t*); // explSMlt
float mode_head_mlt(const struct Player_t*);         // player.headMlt (bhffa), 1
int mode_streak_disabled(const struct Game_t*, int streak); // disabledStreaks

// Client side (mirrors)
void mode_set_player_loadout(struct Player_t*, const int *weapons, int count, int keep_index); // updateLoadout
int mode_is_team(const struct Game_t*);                      // mode.teams
int mode_team_class(const struct Game_t*, int team);         // the class a team is forced to, -1 none
int mode_apply_outfit(const struct Game_t*, struct Player_t*); // modelOverrides (Infected, Stalker) into Player.cosmetics; 1 = changed
// Domination capture interpolation between "uct" packets (game.js updateDomZones, client half),
// and the players' onZone (player.js update: mode.collisionCheck runs on the client too)
void mode_dom_client_tick(struct Game_t*, float delta);
int mode_has_all_zones(const struct Game_t*);        // hasAllZones: the team owning every dom zone, 0 none
// The objective a uid names (map objectives), NULL none; the active one with uid = active_objective.
const ModeObjective *mode_objective_by_uid(const struct Game_t*, int uid);

// ---- Zombies (35): WP-D ships the mode shell in shared/src/modes/mode_zombies.c (table entry,
// condition / winCondition, onKill score loss, onPreEnd wave score); map-logic WP-H stage 2 owns
// that file and fills the waves, AI templates, power-ups, perks and easter eggs (§4.1b).
struct GameAI_t;
void mode_zombies_game_start(struct Game_t*);
void mode_zombies_update(struct Game_t*, float delta);
void mode_zombies_on_kill(struct Game_t*, struct Player_t *victim, struct Player_t *killer, int weapon_id);
void mode_zombies_on_pre_end(struct Game_t*);
void mode_zombies_on_ai_death(struct Game_t*, struct GameAI_t *ai, struct Player_t *killer);
void mode_zombies_on_trigger(struct Game_t*, struct Player_t*, int value);
// medals.js onPickup / onShoot of the Zombies spawnables (power-ups 11-15, perk bottle 19, portal
// 20 by an AI, skull 21 shot, free upgrade 22): spawnables.c calls it, WP-H implements the effects.
void mode_zombies_spawnable(struct Game_t*, struct Player_t *player, struct GameAI_t *ai, int type);
