#pragma once
// WP-E (doc/v7/map-logic.md §1.9, §4.3, §5.2-5.4, §7 WP-E): the Krunker v7.1.4 custom game
// config, ported from src/config.js (serverConfig 85 keys, classConfig 79, weaponConfig 48,
// effectConfig 5, hostTypes 0-4), data/customGameConfig.js (validateConfigs, setupGameConfig)
// and data/game.js (applyConfig, endGame's endData.mts, voteMatch, init's vote tally,
// genMatchList, applyNetworking). shared/src/game_config.c.
//
// Paths that configure a game:
//   - server.cfg gameserver.game.<varN>: game_config_merge() over the server's defaults (only the
//     keys present, v7 clamps), server/src/config.c.
//   - a hosted game (brain "host" frame, server/src/accounts.c) and the offline sandbox
//     (server/src/sandbox.c): game_config_host() keeps the request; every round's map then goes
//     through game_apply_map_config() = setupGameConfig(hostIndex, map config + request) ->
//     applyConfig(cfg, 3, hostIndex). Embedded map config applies offline too
//     (decisions.md "Embedded map config"; v7's sandbox discards map settings, KN does not).
//   - a client: the server's NET_PACKET_GAME_CONFIG (game_config_receive), so prediction, the
//     HUD and the class / weapon tables match the server.
#include <shared.h>
#include <cJSON.h>

// config.hostTypes indices ("override" in a host request).
#define GAME_HOST_CUSTOM 0
#define GAME_HOST_COMP 1
#define GAME_HOST_RENT 2
#define GAME_HOST_ZOMBIES 3
#define GAME_HOST_RAID 4
#define GAME_HOST_TYPE_COUNT 5

// applyConfig(cfg, hostType): which max column clamps a number.
#define GAME_CONFIG_CLAMP_DEDICATED (-2) // maxD (rented / dedicated: the widest)
#define GAME_CONFIG_CLAMP_GUEST 0        // maxG
#define GAME_CONFIG_CLAMP_PUBLIC 1       // max
#define GAME_CONFIG_CLAMP_PRIVATE 2      // maxP
#define GAME_CONFIG_CLAMP_HOSTED 3       // maxF: server/index.ts prepareHostedGame

#define GAME_CONFIG_DATA_MAX 15000 // customGameConfig vLN15000: "Too much data."
#define GAME_CONFIG_INPUT_MAX 24   // the longest "input" option (class / weapon / effect name)

// ---- option tables (src/config.js), also the Host Game UI's rows ----
typedef enum { GC_NUM, GC_BOOL, GC_SELECT, GC_INPUT } GameConfigKind;

enum { // GameConfigOption.flags
    GC_BASIC = 1,        // basic: shown in the short settings list
    GC_DONT_CHANGE = 2,  // dontChange: UI metadata only
    GC_HIDE = 4,         // hide: not shown
    GC_HIDE_E = 8,       // hideE: not in the editor
    GC_HAS_DEF = 16,     // def is set (class / weapon / effect keys without one keep the base value)
    GC_HAS_DEF_C = 32,   // defC is set: the default on comp hosts
};
#define GC_HT(i) (1u << (i)) // GameConfigOption.host_types bit

typedef enum { GAME_CONFIG_SERVER, GAME_CONFIG_CLASS, GAME_CONFIG_WEAPON, GAME_CONFIG_EFFECT, GAME_CONFIG_TABLE_COUNT } GameConfigTable;

typedef struct {
    const char *var;            // varN
    const char *name;           // label
    const char *sub;            // serverConfig sub (group); class / weapon tables: NULL
    const char *head;           // serverConfig head (section title), NULL none
    unsigned char kind;         // GameConfigKind
    unsigned char store;        // game_config.c: where the value goes
    unsigned char flags;
    unsigned char host_types;   // GC_HT bits; 0 = v7 hostTypes null
    float def, def_c, min, max, max_p, max_f, max_d, max_g, step;
    const char *def_str;        // GC_INPUT default
    int len;                    // GC_INPUT max length (v7 len || 16)
    const char *const *options; // GC_SELECT labels
    int option_count;
    int cat;                    // classConfig cat (0 camera, 1 other), -1 none
    size_t offset;              // game_config.c: field offset
    int bit;                    // game_config.c: field id when offset can't address it
    unsigned char unit;         // game_config.c: v7 -> KN unit conversion
} GameConfigOption;

const GameConfigOption *game_config_options(GameConfigTable, size_t *count);
const GameConfigOption *game_config_find_option(GameConfigTable, const char *var); // NULL unknown
const char *game_host_type_name(int host_index); // hostTypes[i].title, NULL unknown
enum { // game_host_type_flags: config.hostTypes UI flags
    GAME_HOST_FLAG_COMP = 1,          // comp: defC defaults, comp lobby sizes
    GAME_HOST_FLAG_RESTRICT = 2,      // restrict: no mode / class / weapon choices, forceDat
    GAME_HOST_FLAG_SHOW_ALL = 4,      // showAll: every setting row, not only the basic ones
    GAME_HOST_FLAG_HIDE_HEAD = 8,     // hideHead: no section titles in the settings
    GAME_HOST_FLAG_NO_CUSTOM_MAP = 16,// noCustM: no raw map data / community maps
    GAME_HOST_FLAG_BUY_SERVER = 32,   // buyServ: the rent tab
};
unsigned int game_host_type_flags(int host_index);

// ---- serverConfig <-> GameConfig ----
// Every option at its def (defC on comp host types). maps / modes are left alone.
void game_config_defaults(GameConfig*, int host_index);
// applyConfig's serverConfig loop: every option from `cfg` (an object of varN values), the
// default when missing or of the wrong type; numbers clamped to [min, max of `clamp`].
void game_config_read(GameConfig*, const cJSON *cfg, int clamp, int host_index);
// Only the keys present in `cfg` (server.cfg game.<varN>), clamped the same way. Returns how
// many were applied.
int game_config_merge(GameConfig*, const cJSON *cfg, int clamp);
cJSON *game_config_write(const GameConfig*);  // every serverConfig varN with its value
// The value of option `var` of a table entry (SERVER: GameConfig*, CLASS: ClassConfig*, WEAPON:
// Weapon*, EFFECT: EffectConfig*) in v7 units: numbers / bools / select indices in *num, inputs
// in *str. 0 when KN does not keep that key (client-only weapon visuals) or `var` is unknown.
int game_config_value(GameConfigTable, const void *entry, const char *var, double *num, const char **str);
// v7's "the base entry has this key" (the Host Game rows; weps / prks overrides of other keys are
// dropped): entry `id` of the table (weapon id, class index, effect index).
int game_config_has_key(GameConfigTable, int id, const char *var);
void game_config_sync_teams(GameConfig*);     // team1/2_name -> team_names[1/2], team1/2_damage = team_damage[1/2]

// ---- request handling (customGameConfig.js) ----
const char *game_config_validate(const cJSON *config); // validateConfigs: NULL, or v7's error text
// A map's "config" {modes, classes, settings, weps, cls, prks} as a flat host request
// (parseSandboxMapConfig without its settings bug): settings' keys at the top level. New object.
cJSON *game_config_from_map(const cJSON *map_config);
// setupGameConfig(hostIndex, requested, extra, allowPrevented): host type resets, overDat,
// extra, comp sizes, preventCustomGames, forceDat / allowedDat. New object; `requested` is
// not modified (v7 mutates overDat in place, a bug not reproduced).
cJSON *game_config_setup(int host_index, const cJSON *requested, const cJSON *extra, int allow_prevented);

// ---- applying to a Game ----
// applyConfig(cfg, clamp, hostIndex) on a game: game->config (maps / modes pointers kept),
// config.classes, and the weps / cls / prks overrides onto the game's weapon, class and effect
// tables (reset to the base tables first). Keeps `cfg` for game_config_send. Returns 1.
int game_config_apply(Game*, const cJSON *cfg, int clamp, int host_index);
// A hosted game / the sandbox: keep `request` (may be NULL: no host settings) and the host type;
// game_apply_map_config() then builds every round's config from it and the round's map. NULL
// game->config_state stays a public game.
void game_config_host(Game*, const cJSON *request, int host_index);
int game_config_is_custom(const Game*);  // hosted / sandbox (isCustom): mts are random config pairs
void game_config_free(Game*);            // drops the state, the game back on the base tables

// setClass: config.classes (classes[0] when not listed), then the mode's classes when forceC.
int game_config_class_for(const Game*, int class_index);
// The class's ClassOverrides, or the v7 defaults when no cls[id] touched it.
const ClassOverrides *class_overrides(const ClassConfig*);
extern const ClassOverrides g_default_class_overrides;

// setupGameConfig + applyConfig(cfg, 3, hostIndex) of a map's embedded "config" (Map.game_config)
// with the host request on top: game_init calls it on authority games that were set up with
// game_config_host (hosted games, the offline sandbox). Returns 1 when something was applied.
int game_apply_map_config(Game*, const Map*);

// ---- sync (server -> client) ----
void game_config_send(Game*, int uid);     // GAME_CONFIG chunks + NETWORK_DATA (before INIT)
// A client: one GAME_CONFIG chunk; the last one applies the config. 1 when applied.
int game_config_receive(Game*, const void *payload, uint16_t length);
// A client's INIT: a server that sent no GAME_CONFIG since the last INIT plays the defaults.
void game_config_on_init(Game*);

// ---- network rates (applyNetworking) ----
void game_network_apply(Game*, int tick_hz);              // game.init: mode.sendR, server tick
const NetNetworkDataPacket *game_network_data(const Game*); // never NULL
void game_network_receive(Game*, const NetNetworkDataPacket*); // a client: the server's rates
// viewDis: should `viewer` (NULL: a connection without a player) get `target`'s state?
int game_network_relevant(const Game*, const Player *viewer, const Player *target);

// ---- end of game: endData.mts, maVote / mv, init's tally (one flow for server_bin and the sandbox) ----
void game_match_list_build(Game*);   // endGame: the GAME_MATCH picks; sends MATCH_VOTE_STATE to everyone
// maVote "mode,map" from connection `uid` (a player or a spectator): toggles that vote; 0 = rejected.
int game_vote_match(Game*, int uid, int mode_id, int map_index);
void game_send_match_votes(Game*, int uid); // MATCH_VOTE_STATE to `uid` (NET_TARGET_ALL: everyone, each with its own vote)
// The end screen ran out: the winning pick (summed weights, ties to the first, no votes ->
// random). *map_index is a game->maps index, *mode_index a game->modes index (the mode is
// appended when missing). 0: no picks, the caller rotates as before.
int game_next_match(Game*, int *map_index, int *mode_index);
void game_force_next_match(Game*, int mode_id, int map_index); // forceMap / forceMode (-1 keeps the current one)
// 1 when `map` can host mode `mode_id`: its config.modes (else the top-level "modes", else [0])
// list the mode and it has the mode's objects (objectives, flags, domination zones, bomb sites).
int game_map_offers_mode(const cJSON *map, int mode_id);
// A random playable (map, mode) pair of the rotation: a map that offers one of game->modes, then
// one of those modes (v7.1.4 server startup). *mode_index is a game->modes index. 0: none.
int game_pick_rotation_pair(const Game*, int *map_index, int *mode_index);
