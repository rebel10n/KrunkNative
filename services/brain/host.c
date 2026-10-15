/*
 * services/brain/host.c - hosting custom games (doc/backend.md 6.3).
 *
 * v3.1.1 spawns custom lobbies inside the game node the host is connected to
 * (packetHandlers.ts "custom" -> hub.spawnCustomLobby). KrunkNative runs one
 * lobby per server_bin, so the brain picks an idle custom-capable node
 * (registry_pick_host_node), sends it `host`, and hands the host player a
 * seek-game style reply for the new lobby.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "brain_internal.h"
#include "knsvc/json.h"
#include "knsvc/log.h"
#include "knsvc/loop.h"

typedef struct HostCtx {
    Brain *b;
    int ticket;
    char node_id[MM_ID_MAX];
    int64_t account_id;
    char account_name[64];
    BrainHostCb cb;
    void *ud;
} HostCtx;

static void host_fail(HostCtx *h, const char *err) {
    BrainHostCb cb = h->cb;
    void *ud = h->ud;
    free(h);
    if (cb) cb(ud, err, NULL);
}

static void on_host_reply(void *ud, const cJSON *reply, const char *err) {
    HostCtx *h = ud;
    if (!reply) {
        KN_WARN("host", "node %s: host request failed (%s)", h->node_id, err ? err : "?");
        host_fail(h, err && !strcmp(err, "timeout") ? "HostTimeout" : "NoAvailableServers");
        return;
    }
    if (!kn_json_bool(reply, "ok", 0)) {
        host_fail(h, kn_json_str(reply, "err", "HostFailed"));
        return;
    }
    MmNode *n = registry_node(h->b->registry, h->node_id);
    const char *lobby = kn_json_str(reply, "gameId", kn_json_str(reply, "lobbyId", ""));
    if (!n || !*lobby) {
        host_fail(h, !n ? "NoAvailableServers" : "HostFailed");
        return;
    }
    char game_id[MM_GAME_ID_MAX];
    if (strchr(lobby, ':')) snprintf(game_id, sizeof(game_id), "%s", lobby);
    else snprintf(game_id, sizeof(game_id), "%s:%s", n->node_id, lobby);
    cJSON *seek;
    if (h->ticket) {
        seek = brain_seek_reply(h->b, n, game_id, h->account_id, h->account_name, 0);
    } else {
        seek = cJSON_CreateObject();
        cJSON_AddStringToObject(seek, "gameId", game_id);
        cJSON_AddStringToObject(seek, "lobbyId", game_id);
        cJSON_AddStringToObject(seek, "host", n->public_host);
        cJSON_AddNumberToObject(seek, "port", n->public_port);
        cJSON_AddStringToObject(seek, "region", n->region);
        cJSON_AddStringToObject(seek, "nodeId", n->node_id);
        if (n->web_url[0]) cJSON_AddStringToObject(seek, "webUrl", n->web_url);
        else cJSON_AddNullToObject(seek, "webUrl");
    }
    KN_INFO("host", "custom game %s hosted on %s (account %lld)", game_id, n->node_id, (long long) h->account_id);
    BrainHostCb cb = h->cb;
    void *cud = h->ud;
    free(h);
    if (cb) cb(cud, seek ? NULL : "internal", seek);
    cJSON_Delete(seek);
}

int brain_host_game(Brain *b, const char *region, const cJSON *config, int64_t account_id,
                    const char *account_name, const char *flavor, BrainHostCb cb, void *ud) {
    return brain_host_game_ex(b, region, config, account_id, account_name, 1, flavor, cb, ud);
}

int brain_host_game_ex(Brain *b, const char *region, const cJSON *config, int64_t account_id,
                       const char *account_name, int ticket, const char *flavor, BrainHostCb cb, void *ud) {
    if (b->draining) {
        if (cb) cb(ud, "updating", NULL);
        return 0;
    }
    /* ticket == 0: ranked (every player holds an account ticket) */
    MmNode *n = registry_pick_host_node(b->registry, region, b->id, mm_settings()->policy.cross_region,
                                        account_id > 0 || !ticket, kn_wall_ms(), flavor);
    if (!n) {
        if (cb) cb(ud, "NoAvailableServers", NULL);
        return 0;
    }
    HostCtx *h = calloc(1, sizeof(*h));
    if (!h) {
        if (cb) cb(ud, "internal", NULL);
        return 0;
    }
    h->b = b;
    h->ticket = ticket;
    snprintf(h->node_id, sizeof(h->node_id), "%s", n->node_id);
    h->account_id = account_id;
    snprintf(h->account_name, sizeof(h->account_name), "%s", account_name ? account_name : "");
    h->cb = cb;
    h->ud = ud;
    int timeout_ms = mm_settings()->host_timeout_s * 1000;
    KN_INFO("host", "hosting a custom game on node %s (brain %s, region %s)", n->node_id, n->brain_id, n->region);
    if (n->local) {
        cJSON *frame = cJSON_CreateObject();
        cJSON_AddStringToObject(frame, "type", "host");
        cJSON_AddItemToObject(frame, "config", config ? cJSON_Duplicate(config, 1) : cJSON_CreateObject());
        if (account_id > 0) cJSON_AddNumberToObject(frame, "hostAccountId", (double) account_id);
        cJSON_AddStringToObject(frame, "hostName", h->account_name);
        if (internal_ws_request(b, n->node_id, frame, timeout_ms, on_host_reply, h)) host_fail(h, "NoAvailableServers");
        return 0;
    }
    if (federation_host_remote(b, n, region, config, account_id, account_name, on_host_reply, h))
        host_fail(h, "NoAvailableServers");
    return 0;
}

int registry_player_count(Brain *b) {
    return registry_player_count_all(b->registry);
}
