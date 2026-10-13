/*
 * services/brain/brain_internal.h - glue between the svc-matchmaker modules of
 * kn_brain (settings, internal WS, federation, tickets). Not for other owners:
 * they use brain.h / registry.h.
 */
#ifndef KN_BRAIN_INTERNAL_H
#define KN_BRAIN_INTERNAL_H

#include <stdint.h>

#include "brain.h"
#include "cJSON.h"
#include "registry.h"

/* ---- settings (brain.* config, loaded once by mm_settings_load) */
typedef struct MmSettings {
    char internal_secret[256];
    char join_secret[256];
    char public_url[256];
    int join_ttl_s;
    int node_ttl_s;
    int heartbeat_s;
    int ping_interval_s;
    int host_timeout_s;
    int federation;                  /* use the shared database's rows */
    MmPolicy policy;
    const cJSON *regions;            /* brain.regions (borrowed from cfg), may be NULL */
    int cors_any;                    /* brain.cors_origins contains "*" (or is absent) */
} MmSettings;

int mm_settings_load(Brain *b);
const MmSettings *mm_settings(void);

/* ---- accounts bridge (svc-accounts services/brain/accounts.c; weak fallback
 * in accounts_bridge.c answers "accounts unavailable"). */
#if defined(__has_include)
#if __has_include("accounts.h")
#include "accounts.h"
#define KN_HAVE_ACCOUNTS_H 1
#endif
#endif
#ifndef KN_HAVE_ACCOUNTS_H
typedef void (*AcctReplyFn)(void *ud, const char *err, const cJSON *data);
int accounts_internal_call(Brain *b, const char *op, const cJSON *args, AcctReplyFn cb, void *ud);
#endif

/* ---- internal WS (internal_ws.c) */
int internal_ws_init(Brain *b);
void internal_ws_shutdown(Brain *b);
/* Send a frame to a LOCAL node (not consumed). 0 ok, -1 not connected. */
int internal_ws_send(Brain *b, const char *node_id, const cJSON *frame);
/* Request/response to a local node: adds "rid" to frame (consumed) and calls cb
 * once with the reply frame (matching rid), or reply NULL + err on timeout or
 * disconnect. Returns 0, or -1 (cb not called) when the node is not local. */
typedef void (*InternalReplyCb)(void *ud, const cJSON *reply, const char *err);
int internal_ws_request(Brain *b, const char *node_id, cJSON *frame, int timeout_ms,
                        InternalReplyCb cb, void *ud);
/* Close a local node's link (code/reason) - used when ownership moved. */
void internal_ws_kick(Brain *b, const char *node_id, int code, const char *reason);
int internal_ws_node_count(void);

/* ---- federation (federation.c); every call is a no-op without a DB */
int federation_init(Brain *b);
void federation_shutdown(Brain *b);
void federation_node_changed(Brain *b, MmNode *n);       /* schedule a persist */
void federation_node_gone(Brain *b, const char *node_id); /* delete its rows */
void federation_reserve(Brain *b, const char *jti, const char *game_id, int64_t account_id, int ttl_s);
void federation_release(Brain *b, const char *jti);
/* Forward an `assign` heads-up to the brain owning node n. */
void federation_assign_remote(Brain *b, const MmNode *n, const cJSON *assign);
/* Ask the brain owning n to send it `host`; cb(ud, reply_or_NULL, err) like internal_ws_request. */
int federation_host_remote(Brain *b, const MmNode *n, const char *region, const cJSON *config,
                           int64_t account_id, const char *account_name, InternalReplyCb cb, void *ud);
int federation_active(void);

/* ---- tickets / seek replies (tickets.c) */
/* Build the seek-game reply for game_id on node n (issues + reserves a ticket).
 * changed -> changeReason "game_changed". Caller frees. */
cJSON *brain_seek_reply(Brain *b, MmNode *n, const char *game_id, int64_t account_id,
                        const char *account_name, int changed);
/* Same; reserve 0 = the ticket holds no seat (ranked rosters: the lobby is
 * private to its players, so their own seek ?game= must still find room). */
cJSON *brain_seek_reply_ex(Brain *b, MmNode *n, const char *game_id, int64_t account_id,
                           const char *account_name, int changed, int reserve);

/* ---- host (host.c): brain_host_game with ticket = 0 answers {gameId, lobbyId,
 * host, port, region, webUrl, nodeId} without issuing a ticket for the host. */
int brain_host_game_ex(Brain *b, const char *region, const cJSON *config, int64_t account_id,
                       const char *account_name, int ticket, BrainHostCb cb, void *ud);

/* ---- ranked (ranked.c, doc/backend.md 6.5) */
int ranked_init(Brain *b);
void ranked_shutdown(Brain *b);
/* internal WS "ranked-result": cb(ud, reply fields) once (reply borrowed) */
typedef void (*RankedResultCb)(void *ud, const char *err, const cJSON *reply);
void ranked_result(Brain *b, const cJSON *frame, RankedResultCb cb, void *ud);

/* ---- v10 Turf Wars (turf.c, 0206_turf_wars) */
int turf_init(Brain *b);
void turf_shutdown(Brain *b);
/* internal WS "turf-kills" {mapId, records}: cb once (same callback shape as ranked_result) */
void turf_record(Brain *b, const cJSON *frame, RankedResultCb cb, void *ud);

/* ---- public routes (public_routes.c) */
int public_routes_init(Brain *b);


#endif
