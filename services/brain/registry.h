/*
 * services/brain/registry.h - the brain's view of every game server node and
 * lobby (v3.1.1 matchmaker/registry.ts NodeRegistry + pickBestLobby).
 *
 * Nodes are either LOCAL (connected to this brain over the internal WS; the
 * brain is the authority and writes their rows) or REMOTE (owned by another
 * brain; replaced wholesale on every federation refresh from PostgreSQL).
 * Pure in-memory and single threaded; no I/O here (federation.c persists,
 * internal_ws.c feeds it), so it is unit tested on its own.
 */
#ifndef KN_BRAIN_REGISTRY_H
#define KN_BRAIN_REGISTRY_H

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

typedef struct Brain Brain;

#define MM_ID_MAX 48
#define MM_GAME_ID_MAX 112

typedef struct MmLobby {
    char game_id[MM_GAME_ID_MAX];    /* "<nodeId>:<localId>" */
    char local_id[64];
    int players;
    int max_players;
    int reserved_remote;             /* live reservations made by other brains (from DB) */
    int custom;                      /* meta.cs */
    int protect;                     /* meta.pr */
    int draining;                    /* meta.draining or node draining */
    int mode_index;                  /* meta.mode (-1 unknown) */
    char mode[32];                   /* meta.modeName */
    char map[64];
    char version[32];                /* meta.v */
    cJSON *tuple;                    /* owned: [gameId, region, players, max, meta] */
} MmLobby;

struct NodeConn;

typedef struct MmNode {
    char node_id[MM_ID_MAX];
    char brain_id[64];
    char region[MM_ID_MAX];
    char name[96];
    char public_host[256];
    int public_port;
    char web_url[256];
    char version[32];
    int max_players;
    int max_lobbies;
    int custom;                      /* can host custom games */
    int draining;
    int local;                       /* connected to this brain */
    struct NodeConn *conn;           /* internal_ws connection (local only) */
    int64_t connected_ms;            /* wall clock */
    int64_t last_seen_ms;            /* wall clock (local: last frame; remote: heartbeat_at) */
    int dirty;                       /* local lobby set changed since last persist */
    int persisting;                  /* federation write in flight */
    int takeover;                    /* next persist claims the row (after hello) */
    MmLobby *lobbies;
    size_t lobby_count, lobby_cap;
} MmNode;

typedef struct MmReservation {
    char jti[40];
    char game_id[MM_GAME_ID_MAX];
    int64_t expires_ms;
} MmReservation;

typedef struct MmBrainInfo {
    char brain_id[64];
    char region[MM_ID_MAX];
    char public_url[256];
    int64_t heartbeat_ms;
} MmBrainInfo;

struct Registry {
    MmNode **nodes;
    size_t node_count, node_cap;
    MmReservation *res;
    size_t res_count, res_cap;
    MmBrainInfo *brains;             /* live brains (federation refresh) */
    size_t brain_count;
};
typedef struct Registry Registry;

/* Matchmaking knobs (brain.matchmaking.*). */
typedef struct MmPolicy {
    int cross_region;                /* fall back to other regions */
    int default_max_players;
    int soft_cap;                    /* 0 = off */
    int require_version;
    const cJSON *mode_caps;          /* object: modeName/index -> cap (borrowed) */
} MmPolicy;

Registry *registry_new(void);
void registry_free(Registry *r);

/* ---- nodes */
MmNode *registry_node(Registry *r, const char *node_id);
/* Add or replace a LOCAL node from a hello frame (keeps lobbies of a reconnect). */
MmNode *registry_node_hello(Registry *r, const char *node_id, const cJSON *hello,
                            const char *brain_id, const char *default_region, int64_t now_ms);
void registry_node_remove(Registry *r, const char *node_id);
int registry_valid_node_id(const char *id);

/* ---- lobbies of a node (tuples are namespaced and copied) */
int registry_lobby_upsert(Registry *r, MmNode *n, const cJSON *tuple);
void registry_lobby_remove(Registry *r, MmNode *n, const char *lobby_id);
void registry_lobbies_set(Registry *r, MmNode *n, const cJSON *tuples);
MmLobby *registry_lobby(Registry *r, const char *game_id, MmNode **node_out);

/* ---- remote view (federation refresh): drop every non-local node not in keep */
void registry_remote_begin(Registry *r);      /* mark remote nodes stale */
MmNode *registry_remote_node(Registry *r, const char *node_id);  /* get-or-create, unmarks */
void registry_remote_end(Registry *r);        /* remove still-marked remote nodes */

/* ---- reservations (seats handed out by this brain) */
void registry_reserve(Registry *r, const char *jti, const char *game_id, int64_t expires_ms);
void registry_release(Registry *r, const char *jti);
void registry_expire(Registry *r, int64_t now_ms);
int registry_reserved(const Registry *r, const char *game_id, int64_t now_ms);

/* Effective seats taken (players + local + remote reservations). */
int registry_lobby_taken(const Registry *r, const MmLobby *l, int64_t now_ms);
/* Quick-join cap of a lobby: min(max_players, mode cap). */
int registry_lobby_cap(const MmLobby *l, const MmPolicy *p);

/* ---- queries */
int registry_player_count_all(const Registry *r);
/* Stale local nodes (no frame for ttl): fills ids (<= max), returns the count. */
size_t registry_stale_local(const Registry *r, int64_t now_ms, int64_t ttl_ms, char ids[][MM_ID_MAX], size_t max);

typedef enum MmPickError {
    MM_PICK_OK = 0,
    MM_PICK_INVALID_GAME,          /* requested id unknown and autoChangeGame off */
    MM_PICK_GAME_FULL,             /* requested id full and autoChangeGame off */
    MM_PICK_NO_MATCH,              /* lobbies exist but none matches the version query */
    MM_PICK_NONE                   /* nothing with room */
} MmPickError;

typedef struct MmPickQuery {
    const char *region;            /* NULL/"" = any */
    const char *game_id;           /* requested ?game= (NULL) */
    int auto_change;               /* autoChangeGame (default 1) */
    int force;                     /* FORCE_ENTER */
    const char *version;           /* dataQuery.v (NULL = any) */
    int64_t now_ms;
} MmPickQuery;

typedef struct MmPick {
    MmNode *node;
    MmLobby *lobby;
    int changed;                   /* requested game replaced (changeReason game_changed) */
} MmPick;

MmPickError registry_pick(Registry *r, const MmPolicy *p, const MmPickQuery *q, MmPick *out);

/* Host target: custom-capable, non-draining node with spare capacity in region
 * (then anywhere when cross_region), preferring brain_id's own nodes. */
MmNode *registry_pick_host_node(Registry *r, const char *region, const char *brain_id, int cross_region,
                                int64_t now_ms);

/* Host a custom game (v3.1.1 game-socket "custom" / Host Game window): pick a
 * node (registry_pick_host_node), send it `host` (directly or through the
 * owning brain), then answer cb with the seek-game reply object for the host
 * player ({gameId, lobbyId, host, port, clientId, region, webUrl, changeReason})
 * or an error name ("NoAvailableServers", "HostTimeout", ...). cb runs exactly
 * once, possibly synchronously; reply is borrowed for the call. Returns 0. */
typedef void (*BrainHostCb)(void *ud, const char *err, const cJSON *reply);
int brain_host_game(Brain *b, const char *region, const cJSON *config, int64_t account_id,
                    const char *account_name, BrainHostCb cb, void *ud);
/* Players in every live lobby (/api/player-count). */
int registry_player_count(Brain *b);

/* JSON views (caller frees). */
cJSON *registry_game_list(const Registry *r, int64_t now_ms, int *total_players); /* array of tuples */
cJSON *registry_lobby_tuple(const Registry *r, const MmLobby *l, int64_t now_ms);  /* copy with live count */

#endif
