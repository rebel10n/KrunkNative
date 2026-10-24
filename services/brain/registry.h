/*
 * services/brain/registry.h - the brain's view of every game server node and
 * lobby (v3.1.1 matchmaker/registry.ts NodeRegistry + pickBestLobby).
 *
 * Nodes are either LOCAL (connected to this brain over the internal WS; the
 * brain is the authority and writes their rows) or REMOTE (owned by another
 * brain; replaced wholesale on every federation refresh from the shared database).
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
    int protect;                     /* meta.pr / meta.private: password protected or private, never quick-joined */
    int online_mode;                 /* meta.onlineMode: signed-in players only (legacy, see MmNode) */
    int draining;                    /* meta.draining or node draining */
    int mode_index;                  /* meta.mode (-1 unknown) */
    char mode[32];                   /* meta.modeName */
    char map[64];
    int map_index;                   /* meta.mapIndex: the map catalogue index (v10 seek_maps), -1 unknown */
    char version[32];                /* meta.v */
    int proto_major;                 /* meta.proto "M.m" major (-1 = not announced: a 1.x server) */
    char ruleset[8];                 /* meta.ruleset "v10" (ruleset.h; "v7" a version-toggle server on v7.1.4; "" not announced) */
    int time_left;                   /* meta.timeLeft: seconds left in the round (0 = end screen, -1 unknown) */
    int no_seek;                     /* v7.1.4 meta.sk === false (AUTO_SEEK off): joined by id only */
    char flavor[16];                 /* meta.flavor: the product flavor pool (shared/include/flavor.h key,
                                      * "strike"); "" = the base game (Krunker Box Office). A node's
                                      * hello flavor is stamped into meta of its lobbies (namespace_tuple) */
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
    int online_mode;                 /* hello.onlineMode: the node refuses guests (missing = 0). Legacy:
                                      * server_bin builds before guest-welcome online_mode sent it; current
                                      * ones admit guests and accounts alike (Krunker's lobbies) and never do */
    int password;                    /* hello.password: joining needs the server password */
    char flavor[16];                 /* hello.flavor (flavor.h key, "strike"); "" = base. Remote nodes:
                                      * learnt from their lobbies' meta.flavor (registry_node_flavor) */
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

/* A join ticket's seat. It counts towards its lobby until the node's own player count
 * includes the player: issued (seek) -> authed (ticket auth: kept, expiry shortened) ->
 * arrived (player-join) -> settled by the node's next report of that lobby, which the node
 * sends after the player-join and therefore counts the player. Settled seats no longer count;
 * federation deletes their mm_reservations rows once that count is persisted
 * (registry_take_settled): no brain ever sees a lobby without both the seat and the player,
 * which used to send concurrent seekers to empty lobbies. */
typedef struct MmReservation {
    char jti[40];
    char game_id[MM_GAME_ID_MAX];
    int64_t expires_ms;
    int arrived;                     /* player-join seen: settles on the lobby's next report */
    int settled;                     /* the node's count includes the player: not counted */
    int foreign;                     /* issued by another brain (its mm_reservations row counts it):
                                      * kept only to release that row after the report */
} MmReservation;

/* Ticket auth -> the player is READY on the node within a few seconds (or refused). */
#define MM_AUTH_GRACE_MS 10000
/* player-join -> the node's next lobby report (server_bin: 250 ms debounce). */
#define MM_ARRIVE_GRACE_MS 5000

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
/* A client-supplied id (?game=, /mm/game-info): the canonical game id or its short link
 * form (shared/include/server_names.h, CH2:kz862 = CHITOWN2:kz862 in region CH). */
MmLobby *registry_lobby_find(Registry *r, const char *id, MmNode **node_out);

/* ---- remote view (federation refresh): drop every non-local node not in keep */
void registry_remote_begin(Registry *r);      /* mark remote nodes stale */
MmNode *registry_remote_node(Registry *r, const char *node_id);  /* get-or-create, unmarks */
void registry_remote_end(Registry *r);        /* remove still-marked remote nodes */

/* ---- reservations (seats handed out by this brain) */
void registry_reserve(Registry *r, const char *jti, const char *game_id, int64_t expires_ms);
void registry_release(Registry *r, const char *jti);
void registry_expire(Registry *r, int64_t now_ms);
/* Seats of game_id still counted (issued, authed or arrived; not settled nor foreign). */
int registry_reserved(const Registry *r, const char *game_id, int64_t now_ms);
/* The ticket's player authenticated on its node: the node does not count it yet, so the seat
 * stays, but expires at until_ms at the latest (a refused player frees it soon). */
void registry_ticket_authed(Registry *r, const char *jti, int64_t until_ms);
/* player-join: the seat stays counted until the node's next report of the lobby settles it
 * (registry_lobby_upsert), or for grace_ms at most. An unknown jti with a game_id (a ticket of
 * another brain, federation) is recorded as foreign: never counted, only released later. */
void registry_arrive(Registry *r, const char *jti, const char *game_id, int64_t now_ms, int64_t grace_ms);
/* Removes up to max settled seats of node_id's lobbies, copying their jtis; returns the count
 * (federation: their mm_reservations rows go once the node's new counts are persisted). */
size_t registry_take_settled(Registry *r, const char *node_id, char jtis[][40], size_t max);

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
    MM_PICK_NONE,                  /* nothing with room */
    MM_PICK_ACCOUNT_REQUIRED,      /* requested lobby refuses guests (legacy onlineMode), the player a guest, autoChangeGame off */
    MM_PICK_INCOMPATIBLE,          /* requested lobby speaks another protocol major, autoChangeGame off */
    MM_PICK_NO_PREF_MATCH,         /* lobbies have room but none plays the seek modes / maps (v10
                                    * dataQuery dsmo / dsma): NoMatchingServers */
    MM_PICK_NO_OTHER_LOBBY         /* the only lobby with room is the excludeGame one the player leaves:
                                    * NoOtherLobby (never sent back to it) */
} MmPickError;

typedef struct MmPickQuery {
    const char *region;            /* NULL/"" = any. A v10 id no node of the pool serves is routed to the nearest
                                    * hosted region first (registry_route_region; MmPick.region says which) */
    const cJSON *pings;            /* seek ?pings= ({v10 id: ms} the client measured to the listed regions' ping
                                    * hosts, < 0 = failed; borrowed, NULL = none): the routing's "nearest" is the
                                    * lowest measured ping, else great circle */
    const char *game_id;           /* requested ?game= (NULL) */
    int auto_change;               /* autoChangeGame (default 1) */
    int force;                     /* FORCE_ENTER */
    const char *version;           /* dataQuery.v (NULL = any) */
    int authed;                    /* signed-in account: may be sent to (legacy) onlineMode lobbies */
    int64_t now_ms;
    int proto_major;               /* client's protocol major (seek ?proto=): never matched to a
                                    * lobby announcing another one; 0 = any */
    const char *ruleset;           /* seek ?ruleset= (NULL/"" = "v10", the only version): quick join skips
                                    * lobbies announcing another one (meta.ruleset "v7"); a lobby that
                                    * announces none and a requested ?game= are not filtered */
    /* outdated lobbies (releases.c, release_info.h rule) lose ties: an up-to-date lobby with
     * room is picked over any outdated one. NULL = no preference. */
    int (*outdated)(const MmLobby *l, void *ud);
    void *outdated_ud;
    const char *exclude_game;      /* seek ?excludeGame= (v10 Quick Match: the lobby the player leaves):
                                    * quick join picks another lobby when one has room, else this one
                                    * (NULL/"" = none) */
    const char *exclude_recent;    /* seek ?excludeRecent= (KrunkNative, comma list): the lobbies the
                                    * player was in before that one (client recent_lobbies.h). Skipped
                                    * like excludeGame, but they come back before it when nothing else
                                    * has room: never a bounce between the last two (NULL/"" = none) */
    /* v10 dataQuery dsmo / dsma (the saved seek_modes / seek_maps): quick join only takes lobbies
     * playing one of these mode indices (meta.mode) / map indices (meta.mapIndex); none = any.
     * A requested ?game= is not filtered. */
    const int *modes;
    int mode_count;
    const int *maps;
    int map_count;
    /* seek ?flavor= (shared/include/flavor.h key): the product flavor pool. NULL/""/"base" = the base
     * game (Krunker Box Office), never a Krunker Strike lobby; "strike" = Strike lobbies only. Also a
     * requested ?game= of another flavor is not found (InvalidGameId): the two apps share the
     * coordinator and the accounts, never a lobby. */
    const char *flavor;
} MmPickQuery;

typedef struct MmPick {
    MmNode *node;
    MmLobby *lobby;
    int changed;                   /* requested game replaced (changeReason game_changed) */
    char region[MM_ID_MAX];        /* the region the public pick looked in: the request's, or the hosted region it
                                    * was routed to (registry_route_region); "" = any */
    int routed;                    /* 1 when the requested region had no node and `region` is the nearest hosted one */
} MmPick;

/* Quick join (doc/backend.md 6.2): a requested ?game= with room, else the public pool (not
 * custom, private, password protected, v7 sk:false, draining, wrong version / protocol, or
 * a legacy onlineMode node for a guest), in the requested region first, then (cross_region) anywhere.
 * A requested region no node of the pool serves (a v10 id krunk.io has no edge in: de-fra, au-syd, ...)
 * is first routed to the nearest hosted region (registry_route_region), whatever cross_region says:
 * cross_region only widens a hosted region whose lobbies have no room. Then:
 *   1. up-to-date before outdated (releases.c)
 *   2. the most taken seats (players + live reservations): populated lobbies fill up before an
 *      empty one is used (v3.1.1 pickBestLobby, v7.1.4 ServerManager.seek "fuller-but-not-full")
 *   3. the most round time left (meta.timeLeft; end screen last)
 *   4. the least loaded node (taken seats over all its lobbies)
 * With soft_cap set, a first pass treats lobbies at the soft cap as full. */
MmPickError registry_pick(Registry *r, const MmPolicy *p, const MmPickQuery *q, MmPick *out);

/* Matchmaking eligibility of a lobby: lobbies of a legacy onlineMode node take signed-in
 * players only (that server_bin refuses a guest with "Failed to verify username"; current
 * ones never announce onlineMode, so every lobby takes guests); password protected ones
 * are never picked automatically. */
int registry_lobby_needs_account(const MmNode *n, const MmLobby *l);
int registry_lobby_protected(const MmNode *n, const MmLobby *l);

/* Host target: custom-capable, non-draining, not password protected node with spare
 * capacity in region (then anywhere when cross_region), preferring brain_id's own
 * nodes; legacy onlineMode nodes only when authed; only nodes of the flavor pool
 * (registry_flavor_match: NULL/"" = base). */
MmNode *registry_pick_host_node(Registry *r, const char *region, const char *brain_id, int cross_region,
                                int authed, int64_t now_ms, const char *flavor);

/* ---- product flavor pools (shared/include/flavor.h; doc/backend.md "Flavor pools") */
/* 1 when a lobby / node of flavor `have` ("" = base) belongs to the pool a client asked for with
 * `want`: NULL / "" / "base" = the base game only; "all" / "*" = every flavor (dashboards); any other
 * key = that flavor only (case-insensitive). */
int registry_flavor_match(const char *have, const char *want);
/* The flavor of a node: its hello's, else (a remote node) its lobbies'. "" = base. */
const char *registry_node_flavor(const MmNode *n);
/* The lobby is in that pool (lobby flavor, else its node's). */
int registry_lobby_in_pool(const MmNode *n, const MmLobby *l, const char *want);
/* registry_lobby_find limited to a flavor pool: another pool's lobby is "not found". */
MmLobby *registry_lobby_find_pool(Registry *r, const char *id, const char *flavor, MmNode **node_out);

/* Host a custom game (v3.1.1 game-socket "custom" / Host Game window): pick a
 * node (registry_pick_host_node), send it `host` (directly or through the
 * owning brain), then answer cb with the seek-game reply object for the host
 * player ({gameId, lobbyId, host, port, clientId, region, webUrl, changeReason})
 * or an error name ("NoAvailableServers", "HostTimeout", ...). cb runs exactly
 * once, possibly synchronously; reply is borrowed for the call. Returns 0. */
typedef void (*BrainHostCb)(void *ud, const char *err, const cJSON *reply);
/* flavor: the host's product flavor pool (?flavor=, the requesting node's; NULL = base): a Strike
 * game is only ever hosted on a Strike node and a Box Office one on a base node. */
int brain_host_game(Brain *b, const char *region, const cJSON *config, int64_t account_id,
                    const char *account_name, const char *guest_id, const char *flavor, BrainHostCb cb, void *ud);
/* Players in every live lobby (/api/player-count). */
int registry_player_count(Brain *b);

/* JSON views (caller frees). */
cJSON *registry_game_list(const Registry *r, int64_t now_ms, int *total_players); /* array of tuples, every flavor */
/* The game list of one flavor pool (registry_flavor_match; NULL = base): /mm/game-list ?flavor=. */
cJSON *registry_game_list_pool(const Registry *r, int64_t now_ms, const char *flavor, int *total_players);
cJSON *registry_lobby_tuple(const Registry *r, const MmLobby *l, int64_t now_ms);  /* copy with live count */

/* ---- regions (doc/v10/regions.md) */
/* The v10 region id a node's region code (MmNode.region, a KrunkNative code) is listed under; NULL = none. */
const char *registry_v10_region(const char *kn_region);
/* 1 when some node of the flavor pool serves `region` (region_eq: its KN code or its v10 id). */
int registry_region_hosted(const Registry *r, const char *region, const char *flavor);
/* The region a seek or host in `region` is made in (written to out, cap bytes): `region` itself when a node of
 * the pool serves it (or it is NULL / ""), else the nearest hosted v10 id (krunker_regions.h
 * krunker_region_nearest: the lowest of the client's `pings` ({v10 id: ms}, NULL = none) over the hosted ids,
 * else the nearest city by great circle; a region without a place takes the first hosted one). Returns 1 when
 * it routed, 0 when out is the request (also when no node is registered at all: nothing to route to). */
int registry_route_region(const Registry *r, const char *region, const char *flavor, const cJSON *pings, char *out, size_t cap);
/* The /mm/ping-list object (caller frees): {v10 region id: "host[:port]"}, one ping host per listed
 * region of the flavor pool (registry_flavor_match). A host is brain.regions[] ping_host (looked up by
 * the v10 id, then by the KrunkNative code), else the owning brain's public URL (public_url when that
 * brain is not listed). Of the nodes sharing an id the first in registry order gives the host. */
cJSON *registry_ping_list(const Registry *r, const char *flavor, const cJSON *regions, const char *public_url);

#endif
