/*
 * v10_translate.h - the Krunker v10 client's packets <-> KrunkerBox native (knet TCP) packets.
 *
 * C port of net-bridge/v10Translate.ts (the spec: every mapping and its output, the same field
 * order and the same stop reasons). Pure: no sockets, no clock, no globals. The bridge
 * (v10_bridge.c) owns the sockets, the session state and the order the packets run in.
 *
 * v10 side: msgpack [label, ...args] plus the 2-byte ahNum trailer, through the official codec
 * (shared/include/krunker_proto.h: @msgpack/msgpack's dialect). Native side: shared/include/shared.h
 * NET_PACKET_* frames, [type u8][len u16 BE][payload].
 *
 * A mapping writes the v10 messages of one native packet into a V10Out. When it stops the
 * translation (the TS `stop`), o->stop is set and the messages are not to be sent.
 * Values in a V10Out live in its own arena until v10_out_free(); a message's args are copied
 * into it, so a caller may pass stack arrays.
 */
#ifndef KN_V10_TRANSLATE_H
#define KN_V10_TRANSLATE_H

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "krunker_proto.h"

/* ---- v10 labels (v10Translate.ts V10_OUT / V10_IN) */
#define V10_OUT_LOAD "load"
#define V10_OUT_ENTER_GAME "en"
#define V10_OUT_CHAT_MESSAGE "ct"

#define V10_IN_IO_INIT "io-init"
#define V10_IN_INIT_LOAD "load"
#define V10_IN_GAME_INSTANCE_ID "inst-id"
#define V10_IN_INIT_GAME "init"
#define V10_IN_UPDATE_GLOBAL_VALUE "gv"
#define V10_IN_PLAYER_INTERACTION "inat" /* the pickup half: updatePickup(uid, weapon, null, active, pos) (v10.js 219827) */
#define V10_IN_ADD_PLAYER_SOUND "asnd"
#define V10_IN_REMOVE_PLAYER_SOUND "rsnd"
#define V10_IN_UPDATE_TIMER "t"
#define V10_IN_GAME_STATE_CHANGED "gsc"
#define V10_IN_ADD_PLAYERS "0"
#define V10_IN_UPDATE_LOCKOUT_TIM "lkt"
#define V10_IN_SET_OBJECTIVE "obj"
#define V10_IN_SET_WEAPON "wep"
#define V10_IN_C_SPAWN "aai"
#define V10_IN_UPDATE_DOM_ZONE "uct"
#define V10_IN_UPDATE_PLAYERS "k"
#define V10_IN_ADD_CHAT "ch"
#define V10_IN_SYNC_ALL_AI "ai"
#define V10_IN_UPDATE_LEADERS "7"
#define V10_IN_START_GAME "start"
#define V10_IN_GAME_ANNOUNCEMENT "gameAnnouncement"
#define V10_IN_UPDATE_FLAG "ufl"
#define V10_IN_UPDATE_FLAG_INFO "ufi"
#define V10_IN_TEAM_SCORE "ts"
#define V10_IN_UPDATE_LIVES "lv"
#define V10_IN_LOCK_PLAYER "lock"
#define V10_IN_UPDATE_GAME_MESSAGE "gmsg"
#define V10_IN_DESTROY_GAMEOBJECT "do"
#define V10_IN_RESPAWN_OBJ "ro"
#define V10_IN_UPDATE_GATE "gte"
#define V10_IN_UPDATE_BANK "bnk"
#define V10_IN_TELEPORT_TO_NODE "mo"
#define V10_IN_TELEPORT_FROM_NODE "mf"
#define V10_IN_NOCLIP "noc"
#define V10_IN_UNLIMITED_AMMO "unl"
#define V10_IN_FORCE_HIDE "fh"
#define V10_IN_FROZEN "frz"

/* ---- sizes (the TS constants; the layouts are checked against shared.h in the tests) */
#define V10_ENT_SIZE 101        /* ENT_V9_SIZE: NetEntPacket with the v7 cosmetics and show_badges */
#define V10_CHAT_TEXT_SIZE 288  /* NET_CHAT_TEXT_SIZE */
#define V10_ADD_RECORD_FIELDS 53
#define V10_INIT_ARGS 20
#define V10_STATE_FIELDS 13
#define V10_SKIN_COLORS_N 5

/* HELLO payload: protocol 1.4.4 and the Krunker v10 ruleset bit (the TS HELLO_PAYLOAD). */
#define V10_HELLO_PAYLOAD_SIZE 4
void v10_hello_payload(uint8_t out[V10_HELLO_PAYLOAD_SIZE]);

/* ---- messages of one native packet */

typedef struct V10Msg {
    const char *label;
    KrunkerValue *args; /* owned by the V10Out's arena */
    uint32_t argc;
} V10Msg;

typedef struct V10Block V10Block;

typedef struct V10Out {
    V10Block *blocks;   /* arena for values, strings and arrays (freed by v10_out_free) */
    V10Msg *msgs;
    size_t count, cap;
    const char *stop;   /* NULL: translated; else why the packet stops the translation (TS mapped.stop) */
    int oom;            /* an allocation failed: the caller treats it as a stop */
} V10Out;

void v10_out_init(V10Out *o);
void v10_out_free(V10Out *o);
/* Append a message; its args are copied. 0 ok, -1 out of memory (o->oom set). */
int v10_push(V10Out *o, const char *label, const KrunkerValue *args, uint32_t argc);
/* Arena storage (zeroed); NULL on out of memory. */
void *v10_arena(V10Out *o, size_t n);
/* Arena copies: an array of values, a map (string keys) and a text (UTF-8 sanitised). */
KrunkerValue v10_arr(V10Out *o, const KrunkerValue *items, uint32_t n);
KrunkerValue v10_obj(V10Out *o, const char *const *keys, const KrunkerValue *vals, uint32_t n);
KrunkerValue v10_text(V10Out *o, const uint8_t *s, size_t n);

/* JavaScript truthiness of a decoded value (NULL: undefined). */
int v10_truthy(const KrunkerValue *v);

/* ---- client -> bridge (v10 labels) */

/* The KN ENT (V9) for an "en" array (v10Translate encodeEnter). 0 when `en` is not the v10 shape. */
int v10_encode_enter(const KrunkerValue *en, uint8_t out[V10_ENT_SIZE]);
/* The KN CHAT_SEND text for a "ct" text (encodeChat): bytes written to out (NUL-terminated), 0 when
 * the text is not a string or is blank (out needs V10_CHAT_TEXT_SIZE bytes). */
size_t v10_encode_chat(const KrunkerValue *text, uint8_t out[V10_CHAT_TEXT_SIZE]);

/* ---- native -> v10: the map, the config and the network data (before the INIT) */

typedef struct V10MapInfo {
    int32_t map_index;
    uint32_t hash;
    uint32_t size;
    char name[100]; /* UTF-8, sanitised: up to 32 bytes raw */
} V10MapInfo;
int v10_parse_map_info(const uint8_t *p, size_t n, V10MapInfo *out);
/* MAP_REQUEST payload: the map hash (encodeMapRequest). */
void v10_encode_map_request(uint32_t hash, uint8_t out[4]);
/* MAP_DATA: hash, offset, data (the chunk's bytes inside p). 0 when malformed. */
int v10_parse_map_data(const uint8_t *p, size_t n, uint32_t *hash, uint32_t *offset, const uint8_t **data, size_t *dlen);
/* GAME_CONFIG chunk: total, offset, data. 0 when malformed. */
int v10_parse_game_config_chunk(const uint8_t *p, size_t n, uint32_t *total, uint32_t *offset, const uint8_t **data, size_t *dlen);

typedef struct V10Network {
    uint16_t snf, tf, sf;
} V10Network;
int v10_parse_network_data(const uint8_t *p, size_t n, V10Network *out);

/* GLOBAL_VALUE (NetValuePacket): reset flag, key (NUL-terminated copy, at most 71 bytes), value. */
typedef struct V10GlobalValue {
    int reset;
    char key[220]; /* UTF-8, sanitised: up to 71 bytes raw */
    double value;
} V10GlobalValue;
int v10_parse_global_value(const uint8_t *p, size_t n, V10GlobalValue *out);

typedef struct V10PlayerSound {
    int32_t uid;
    int add, force, loop;
    double volume, rate;
    char name[100]; /* UTF-8, sanitised: up to 24 bytes raw */
} V10PlayerSound;
int v10_parse_player_sound(const uint8_t *p, size_t n, V10PlayerSound *out);

typedef struct V10Timer {
    int32_t seconds;
    int end;
} V10Timer;
int v10_parse_timer(const uint8_t *p, size_t n, V10Timer *out);

/* INIT (NetInitPacket): the 17 bytes before the ruleset, and the ruleset (0 when absent). */
typedef struct V10Init {
    int32_t map_index, mode_index;
    uint16_t tick_rate;
    int32_t client_id;
    uint8_t ruleset;
} V10Init;
int v10_parse_init(const uint8_t *p, size_t n, V10Init *out);

/* The INIT_GAME message (v10 server's 20 arguments, buildInitArgs). `config` and `network` are
 * values from the caller's V10Out (v10_config_value / v10_network_value). */
void v10_init_game(V10Out *o, const V10Init *init, const uint8_t *map, size_t map_len,
                   KrunkerValue config, KrunkerValue network, int ranked);
/* The networkData object of the init (snf, tf, sf). */
KrunkerValue v10_network_value(V10Out *o, const V10Network *net);

/* The config of a GAME_CONFIG JSON text (buildConfig): the parsed root, NULL when the text is not
 * JSON or is the null value (TS: msg.settings throws). Free with cJSON_Delete. */
cJSON *v10_config_parse(const uint8_t *text, size_t len);
/* The config object: the root's settings (an object or an array, as the spread takes them), then
 * the classes array when the root has one (buildConfig). */
KrunkerValue v10_config_value(V10Out *o, const cJSON *root);

/* A chunk assembly (ChunkAssembly): the chunks of one text, in order. */
typedef struct V10Chunks {
    uint8_t *buf;
    size_t total, end;
} V10Chunks;
int v10_chunks_init(V10Chunks *c, size_t total); /* 0 ok, -1 too large or out of memory */
void v10_chunks_free(V10Chunks *c);
int v10_chunks_add(V10Chunks *c, uint32_t offset, const uint8_t *data, size_t n); /* 1 ok, 0 refused */
int v10_chunks_complete(const V10Chunks *c);

/* ---- MODE_STATE, OBJECT_STATE, GAME_ANNOUNCEMENT */

void v10_map_mode_state(V10Out *o, const uint8_t *p, size_t n);
void v10_map_object_state(V10Out *o, const uint8_t *p, size_t n);
/* PICKUP (NetPickupPacket, 26 bytes): one pickup's state as the "inat" message (mapPickup in v10Translate.ts). */
void v10_map_pickup(V10Out *o, const uint8_t *p, size_t n);
/* CHEAT (NetCheatPacket: uint8 flag, uint8 on): 0 when malformed. */
int v10_parse_cheat(const uint8_t *p, size_t n, uint8_t *flag, int *on);
/* The v10 label of a cheat flag (noc, unl, fh, frz), NULL when the flag has none (borders, forcecol, ...). */
const char *v10_cheat_label(uint8_t flag);
void v10_map_game_announcement(V10Out *o, const uint8_t *body, size_t n);

/* ---- SKINS, SPAWN, the add record, START, LEADERBOARD, LEADER_STATS */

typedef struct V10Skins {
    int32_t uid, primary, secondary, melee;
} V10Skins;
int v10_parse_skins(const uint8_t *p, size_t n, V10Skins *out);

typedef struct V10Spawn {
    int32_t uid;
    int is_you, active;
    int32_t class_index, team;
    double health;
    int32_t max_health;
    double position[3];
    double direction[2];
    char name[100]; /* UTF-8, sanitised: up to 32 bytes raw */
    int32_t secondary_index;
    int32_t skin_color, hair_color, hat, body, waist, face, dye;
    int32_t attach_index;
    double head_mlt;
    int32_t head, back, shoe, pet, wrist, charm[2], player_card, badge;
} V10Spawn;
int v10_parse_spawn(const uint8_t *p, size_t n, V10Spawn *out);

/* The add record (ADD_PLAYERS "0" arguments: the record, then 1). socket_id is our own player's id. */
KrunkerValue v10_player_add_record(V10Out *o, const V10Spawn *spawn, const V10Skins *skins, int has_skins, const char *socket_id);
/* START_GAME ("start") for our own spawn: gameTimer, team, spectating, challMode, moveLock, 1, null, classIndex. */
void v10_start_game(const V10Spawn *spawn, int32_t game_timer, int32_t chall_mode, KrunkerValue out[8]);

/* LEADERBOARD (FFA only): the "7" rows, flat [uid, score, kills, deaths] per row. The rows are
 * copied into `rows` (room for 64 int32). Returns the row count via *count. */
void v10_map_leaderboard(V10Out *o, const uint8_t *p, size_t n, int32_t mode_index, int32_t *rows, size_t *count);
/* LEADER_STATS repeats the rows of the last LEADERBOARD: NULL when consumed, else the stop reason. */
const char *v10_check_leader_stats(const uint8_t *p, size_t n, const int32_t *rows, size_t row_count, int have_rows);

/* ---- MODE_SYNC_V7, DOM_UPDATE, AI_SYNC_V7, AI_ADD_V7, STATE, CHAT_V7 */

void v10_map_mode_sync(V10Out *o, const uint8_t *p, size_t n, int objective_weapon);
void v10_map_dom_update(V10Out *o, const uint8_t *p, size_t n);

typedef struct V10BotEntry {
    int32_t sid;
    double x, y, z, xd, yd;
    int32_t health;
    int did_move;
    int32_t team;
} V10BotEntry;
typedef struct V10BotChunk {
    int first;
    size_t count;
    V10BotEntry entries[16];
} V10BotChunk;
int v10_parse_bot_sync_chunk(const uint8_t *p, size_t n, V10BotChunk *out);
/* The "ai" list: 9 values per bot (sid, x, y, z, xD, yD, floor(health), didMov, team), arena array. */
KrunkerValue v10_bot_sync_list(V10Out *o, const V10BotEntry *entries, size_t count);

void v10_map_bot_add(V10Out *o, const uint8_t *p, size_t n);

typedef struct V10StateRec {
    int32_t uid;
    double x, y, z;
    double x_dire, y_dire;
    double step_val;
    int on_ground;
    double crouch;
    int32_t loadout;
    int aim;
    int grapple_on;
    double grapple[3];
    double ping;
} V10StateRec;
int v10_map_state(const uint8_t *p, size_t n, V10StateRec *out);
/* The 13-field "k" record (V10_STATE_FIELDS values, arena). */
KrunkerValue v10_state_record(V10Out *o, const V10StateRec *r);

void v10_map_chat_v7(V10Out *o, const uint8_t *p, size_t n);

/* HTML escape of UTF-8 text (EV.escapeHtml), into the arena. */
KrunkerValue v10_escape_html(V10Out *o, const uint8_t *s, size_t n);

#endif
