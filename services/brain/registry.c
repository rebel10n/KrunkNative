/*
 * services/brain/registry.c - see registry.h. Port of v3.1.1
 * matchmaker/registry.ts (NodeRegistry, pickBestLobby) extended with regions,
 * reservations and remote (federated) nodes.
 */
#include "registry.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "knsvc/http.h"
#include "knsvc/json.h"
#include "server_names.h"

static void copy_str(char *dst, size_t cap, const char *src) {
    if (!cap) return;
    if (!src) src = "";
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

Registry *registry_new(void) {
    return calloc(1, sizeof(Registry));
}

static void lobby_clear(MmLobby *l) {
    cJSON_Delete(l->tuple);
    l->tuple = NULL;
}

static void node_free(MmNode *n) {
    if (!n) return;
    for (size_t i = 0; i < n->lobby_count; i++) lobby_clear(&n->lobbies[i]);
    free(n->lobbies);
    free(n);
}

void registry_free(Registry *r) {
    if (!r) return;
    for (size_t i = 0; i < r->node_count; i++) node_free(r->nodes[i]);
    free(r->nodes);
    free(r->res);
    free(r->brains);
    free(r);
}

/* A flavor key as the registry keeps it (shared/include/flavor.h): lowercase, "base" = "" (the base
 * game, Krunker Box Office: what every node and lobby announcing none plays). */
static void flavor_norm(char *dst, size_t cap, const char *src) {
    size_t n = 0;
    for (const char *p = src ? src : ""; *p && n + 1 < cap; p++) dst[n++] = (char) tolower((unsigned char) *p);
    dst[n] = 0;
    if (!strcmp(dst, "base")) dst[0] = 0;
}

int registry_valid_node_id(const char *id) {
    if (!id || !*id) return 0;
    size_t n = 0;
    for (const char *p = id; *p; p++, n++) {
        if (!(isalnum((unsigned char) *p) || *p == '_' || *p == '-' || *p == '.')) return 0;
    }
    return n < MM_ID_MAX;
}

MmNode *registry_node(Registry *r, const char *node_id) {
    if (!node_id) return NULL;
    for (size_t i = 0; i < r->node_count; i++) {
        if (!strcmp(r->nodes[i]->node_id, node_id)) return r->nodes[i];
    }
    return NULL;
}

static MmNode *node_add(Registry *r, const char *node_id) {
    if (r->node_count == r->node_cap) {
        size_t cap = r->node_cap ? r->node_cap * 2 : 16;
        MmNode **nn = realloc(r->nodes, cap * sizeof(*nn));
        if (!nn) return NULL;
        r->nodes = nn;
        r->node_cap = cap;
    }
    MmNode *n = calloc(1, sizeof(MmNode));
    if (!n) return NULL;
    copy_str(n->node_id, sizeof(n->node_id), node_id);
    n->max_players = 8;
    n->max_lobbies = 1;
    r->nodes[r->node_count++] = n;
    return n;
}

MmNode *registry_node_hello(Registry *r, const char *node_id, const cJSON *hello,
                            const char *brain_id, const char *default_region, int64_t now_ms) {
    if (!registry_valid_node_id(node_id)) return NULL;
    MmNode *n = registry_node(r, node_id);
    if (!n) n = node_add(r, node_id);
    if (!n) return NULL;
    const char *region = kn_json_str(hello, "region", NULL);
    copy_str(n->region, sizeof(n->region), region && *region ? region : default_region);
    copy_str(n->brain_id, sizeof(n->brain_id), brain_id);
    copy_str(n->name, sizeof(n->name), kn_json_str(hello, "name", node_id));
    copy_str(n->public_host, sizeof(n->public_host), kn_json_str(hello, "publicHost", ""));
    n->public_port = (int) kn_json_int(hello, "publicPort", 0);
    copy_str(n->web_url, sizeof(n->web_url), kn_json_str(hello, "webUrl", ""));
    copy_str(n->version, sizeof(n->version), kn_json_str(hello, "version", ""));
    n->max_players = (int) kn_json_int(hello, "maxPlayers", 8);
    if (n->max_players <= 0) n->max_players = 8;
    n->max_lobbies = (int) kn_json_int(hello, "maxLobbies", 1);
    if (n->max_lobbies <= 0) n->max_lobbies = 1;
    n->custom = kn_json_bool(hello, "custom", 0);
    /* "the node refuses guests": only server_bin builds before guest-welcome online_mode
     * send it (1 with online_mode on); current ones admit everyone and omit it (0) */
    n->online_mode = kn_json_bool(hello, "onlineMode", 0);
    n->password = kn_json_bool(hello, "password", 0);
    /* the product flavor pool (flavor.h): a Krunker Strike server_bin says "strike"; base ones say nothing */
    flavor_norm(n->flavor, sizeof(n->flavor), kn_json_str(hello, "flavor", ""));
    /* lobbies kept from before a reconnect follow the node's current mode */
    for (size_t i = 0; i < n->lobby_count; i++) n->lobbies[i].online_mode = n->online_mode;
    n->draining = 0;
    n->local = 1;
    n->connected_ms = now_ms;
    n->last_seen_ms = now_ms;
    n->dirty = 1;
    return n;
}

void registry_node_remove(Registry *r, const char *node_id) {
    for (size_t i = 0; i < r->node_count; i++) {
        if (!strcmp(r->nodes[i]->node_id, node_id)) {
            node_free(r->nodes[i]);
            memmove(&r->nodes[i], &r->nodes[i + 1], (r->node_count - i - 1) * sizeof(MmNode *));
            r->node_count--;
            return;
        }
    }
}

/* ---- lobbies */

static void lobby_from_tuple(MmLobby *l, const MmNode *n, cJSON *tuple) {
    lobby_clear(l);
    l->tuple = tuple;
    const cJSON *id = cJSON_GetArrayItem(tuple, 0);
    copy_str(l->game_id, sizeof(l->game_id), cJSON_IsString(id) ? id->valuestring : "");
    const char *colon = strchr(l->game_id, ':');
    copy_str(l->local_id, sizeof(l->local_id), colon ? colon + 1 : l->game_id);
    const cJSON *pl = cJSON_GetArrayItem(tuple, 2), *mx = cJSON_GetArrayItem(tuple, 3);
    l->players = cJSON_IsNumber(pl) ? (int) pl->valuedouble : 0;
    if (l->players < 0) l->players = 0;
    l->max_players = cJSON_IsNumber(mx) && mx->valuedouble > 0 ? (int) mx->valuedouble : n->max_players;
    const cJSON *meta = cJSON_GetArrayItem(tuple, 4);
    l->custom = kn_json_bool(meta, "cs", 0) || kn_json_bool(meta, "isCustom", 0);
    /* pr: password protected; private (v7 host config "private", server_bin WP-E): a private
     * custom game is joined by id only, never quick-joined (older servers never send it) */
    l->protect = kn_json_bool(meta, "pr", 0) || kn_json_bool(meta, "private", 0);
    l->online_mode = n->online_mode || kn_json_bool(meta, "onlineMode", 0);
    l->draining = kn_json_bool(meta, "draining", 0);
    const cJSON *mode = cJSON_GetObjectItemCaseSensitive(meta, "mode");
    l->mode_index = cJSON_IsNumber(mode) ? (int) mode->valuedouble
                  : (int) kn_json_int(meta, "modeIndex", -1);
    copy_str(l->mode, sizeof(l->mode), kn_json_str(meta, "modeName", cJSON_IsString(mode) ? mode->valuestring : ""));
    copy_str(l->map, sizeof(l->map), kn_json_str(meta, "map", kn_json_str(meta, "mapName", kn_json_str(meta, "i", ""))));
    l->map_index = (int) kn_json_int(meta, "mapIndex", -1);
    copy_str(l->version, sizeof(l->version), kn_json_str(meta, "v", ""));
    /* meta.proto "M.m" (server_bin with build info); absent = a 1.x server */
    const char *proto = kn_json_str(meta, "proto", "");
    l->proto_major = proto[0] >= '0' && proto[0] <= '9' ? atoi(proto) : -1;
    /* meta.ruleset ("v10", ruleset.h; "v7" from a version-toggle server playing v7.1.4); "" = not
     * announced (a server from before the toggle, a test fixture) */
    copy_str(l->ruleset, sizeof(l->ruleset), kn_json_str(meta, "ruleset", ""));
    const cJSON *tl = cJSON_GetObjectItemCaseSensitive(meta, "timeLeft");
    l->time_left = cJSON_IsNumber(tl) && tl->valuedouble >= 0 ? (int) tl->valuedouble : -1;
    /* v7.1.4 ServerManager.seek skips !meta.sk (AUTO_SEEK off); absent = seekable */
    l->no_seek = cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(meta, "sk"));
    /* meta.flavor (flavor.h key; a Strike server_bin sends it, namespace_tuple stamps its node's) */
    flavor_norm(l->flavor, sizeof(l->flavor), kn_json_str(meta, "flavor", n->flavor));
}

/* A node's report of a lobby counts every player whose player-join came before it (frames
 * of one node arrive in order): their seats stop counting (registry_arrive). */
static void settle_arrived(Registry *r, const char *game_id) {
    for (size_t i = 0; i < r->res_count; i++) {
        MmReservation *e = &r->res[i];
        if (e->arrived && !e->settled && !strcmp(e->game_id, game_id)) e->settled = 1;
    }
}

/* o[key] = value with JS assignment semantics: replaced in place, else appended. */
static void set_meta_str(cJSON *o, const char *key, const char *value) {
    cJSON *v = cJSON_CreateString(value);
    if (!v) return;
    if (cJSON_GetObjectItemCaseSensitive(o, key)) cJSON_ReplaceItemInObjectCaseSensitive(o, key, v);
    else cJSON_AddItemToObject(o, key, v);
}

/* Copy + namespace a raw node tuple (registry.ts _namespace). NULL if invalid. */
static cJSON *namespace_tuple(const MmNode *n, const cJSON *tuple) {
    if (!cJSON_IsArray(tuple) || cJSON_GetArraySize(tuple) < 4) return NULL;
    const cJSON *id = cJSON_GetArrayItem(tuple, 0);
    if (!cJSON_IsString(id) || !id->valuestring[0]) return NULL;
    char ns[MM_GAME_ID_MAX];
    if (strchr(id->valuestring, ':')) copy_str(ns, sizeof(ns), id->valuestring);
    else {
        size_t a = strlen(n->node_id), b = strlen(id->valuestring);
        if (a + 1 + b >= sizeof(ns)) return NULL;
        memcpy(ns, n->node_id, a);
        ns[a] = ':';
        memcpy(ns + a + 1, id->valuestring, b + 1);
    }
    cJSON *out = cJSON_Duplicate(tuple, 1);
    if (!out) return NULL;
    cJSON_ReplaceItemInArray(out, 0, cJSON_CreateString(ns));
    const cJSON *region = cJSON_GetArrayItem(out, 1);
    if (!cJSON_IsString(region) || !region->valuestring[0])
        cJSON_ReplaceItemInArray(out, 1, cJSON_CreateString(n->region));
    /* registry.ts: out[4] = {...out[4], lobbyId: ns, nodeId} when meta is an object:
     * an existing key keeps its position, a new one is appended (JS spread order) */
    cJSON *meta = cJSON_GetArrayItem(out, 4);
    if (cJSON_IsObject(meta)) {
        set_meta_str(meta, "lobbyId", ns);
        set_meta_str(meta, "nodeId", n->node_id);
        /* KrunkNative: legacy onlineMode nodes take signed-in players only; the flag travels
         * with the tuple so other brains (federation, mm_lobbies.tuple) see it too */
        if (n->online_mode && !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(meta, "onlineMode"))) {
            cJSON_DeleteItemFromObjectCaseSensitive(meta, "onlineMode");
            cJSON_AddBoolToObject(meta, "onlineMode", 1);
        }
        /* the node's product flavor travels with the tuple too (federation, mm_lobbies.tuple), so
         * every brain keeps the pools apart; base tuples stay exactly as the node sent them */
        if (n->flavor[0] && !cJSON_IsString(cJSON_GetObjectItemCaseSensitive(meta, "flavor"))) set_meta_str(meta, "flavor", n->flavor);
    }
    return out;
}

static MmLobby *node_lobby(MmNode *n, const char *game_id) {
    for (size_t i = 0; i < n->lobby_count; i++) {
        if (!strcmp(n->lobbies[i].game_id, game_id)) return &n->lobbies[i];
    }
    return NULL;
}

int registry_lobby_upsert(Registry *r, MmNode *n, const cJSON *tuple) {
    cJSON *ns = namespace_tuple(n, tuple);
    if (!ns) return -1;
    const char *gid = cJSON_GetArrayItem(ns, 0)->valuestring;
    MmLobby *l = node_lobby(n, gid);
    if (!l) {
        if (n->lobby_count == n->lobby_cap) {
            size_t cap = n->lobby_cap ? n->lobby_cap * 2 : 4;
            MmLobby *nl = realloc(n->lobbies, cap * sizeof(*nl));
            if (!nl) {
                cJSON_Delete(ns);
                return -1;
            }
            n->lobbies = nl;
            n->lobby_cap = cap;
        }
        l = &n->lobbies[n->lobby_count++];
        memset(l, 0, sizeof(*l));
    }
    lobby_from_tuple(l, n, ns);
    if (n->local) settle_arrived(r, l->game_id);
    n->dirty = 1;
    return 0;
}

void registry_lobby_remove(Registry *r, MmNode *n, const char *lobby_id) {
    (void) r;
    if (!lobby_id || !*lobby_id) return;
    char ns[MM_GAME_ID_MAX];
    if (strchr(lobby_id, ':')) copy_str(ns, sizeof(ns), lobby_id);
    else {
        size_t a = strlen(n->node_id), b = strlen(lobby_id);
        if (a + 1 + b >= sizeof(ns)) return;
        memcpy(ns, n->node_id, a);
        ns[a] = ':';
        memcpy(ns + a + 1, lobby_id, b + 1);
    }
    for (size_t i = 0; i < n->lobby_count; i++) {
        if (!strcmp(n->lobbies[i].game_id, ns)) {
            lobby_clear(&n->lobbies[i]);
            memmove(&n->lobbies[i], &n->lobbies[i + 1], (n->lobby_count - i - 1) * sizeof(MmLobby));
            n->lobby_count--;
            n->dirty = 1;
            return;
        }
    }
}

void registry_lobbies_set(Registry *r, MmNode *n, const cJSON *tuples) {
    for (size_t i = 0; i < n->lobby_count; i++) lobby_clear(&n->lobbies[i]);
    n->lobby_count = 0;
    n->dirty = 1;
    const cJSON *t;
    cJSON_ArrayForEach(t, tuples) registry_lobby_upsert(r, n, t);
}

MmLobby *registry_lobby(Registry *r, const char *game_id, MmNode **node_out) {
    if (!game_id) return NULL;
    for (size_t i = 0; i < r->node_count; i++) {
        MmLobby *l = node_lobby(r->nodes[i], game_id);
        if (l) {
            if (node_out) *node_out = r->nodes[i];
            return l;
        }
    }
    return NULL;
}

/* A requested ?game= id: the canonical "<nodeId>:<lobbyId>", else its short link form
 * "<short node>:<lobbyId>" (shared/src/server_names.c: CH2:kz862 for CHITOWN2:kz862 in CH),
 * matched against the lobby's region (tuple[1], what clients see) and its node's region. */
MmLobby *registry_lobby_find(Registry *r, const char *id, MmNode **node_out) {
    MmLobby *exact = registry_lobby(r, id, node_out);
    if (exact || !id) return exact;
    const char *colon = strchr(id, ':');
    if (!colon || colon == id || !colon[1]) return NULL;
    for (size_t i = 0; i < r->node_count; i++) {
        MmNode *n = r->nodes[i];
        for (size_t j = 0; j < n->lobby_count; j++) {
            MmLobby *l = &n->lobbies[j];
            if (strcmp(l->local_id, colon + 1) != 0) continue;
            const char *region = cJSON_GetStringValue(cJSON_GetArrayItem(l->tuple, 1));
            if (kn_game_id_matches(id, l->game_id, region) || kn_game_id_matches(id, l->game_id, n->region)) {
                if (node_out) *node_out = n;
                return l;
            }
        }
    }
    return NULL;
}

/* ---- product flavor pools */

int registry_flavor_match(const char *have, const char *want) {
    char h[16], w[16];
    flavor_norm(h, sizeof(h), have);
    flavor_norm(w, sizeof(w), want);
    if (!strcmp(w, "all") || !strcmp(w, "*")) return 1;
    return !strcmp(h, w);
}

const char *registry_node_flavor(const MmNode *n) {
    if (!n) return "";
    if (n->flavor[0] || n->local) return n->flavor;
    /* a remote node (federation): its mm_nodes row has no flavor, its lobby tuples do */
    for (size_t j = 0; j < n->lobby_count; j++) {
        if (n->lobbies[j].flavor[0]) return n->lobbies[j].flavor;
    }
    return "";
}

int registry_lobby_in_pool(const MmNode *n, const MmLobby *l, const char *want) {
    return registry_flavor_match(l->flavor[0] ? l->flavor : registry_node_flavor(n), want);
}

MmLobby *registry_lobby_find_pool(Registry *r, const char *id, const char *flavor, MmNode **node_out) {
    MmNode *n = NULL;
    MmLobby *l = registry_lobby_find(r, id, &n);
    if (l && !registry_lobby_in_pool(n, l, flavor)) {
        /* the short link form may name lobbies of both pools (CH1:abcde of CHITOWN1 and of a Strike
         * node CHITOWNSTRIKE1): look for one in this pool */
        l = NULL;
        const char *colon = id ? strchr(id, ':') : NULL;
        for (size_t i = 0; colon && !l && i < r->node_count; i++) {
            MmNode *c = r->nodes[i];
            for (size_t j = 0; j < c->lobby_count; j++) {
                MmLobby *cl = &c->lobbies[j];
                if (strcmp(cl->local_id, colon + 1) != 0 || !registry_lobby_in_pool(c, cl, flavor)) continue;
                const char *region = cJSON_GetStringValue(cJSON_GetArrayItem(cl->tuple, 1));
                if (!strcmp(cl->game_id, id) || kn_game_id_matches(id, cl->game_id, region) ||
                    kn_game_id_matches(id, cl->game_id, c->region)) {
                    n = c;
                    l = cl;
                    break;
                }
            }
        }
    }
    if (node_out) *node_out = l ? n : NULL;
    return l;
}

/* ---- remote view */

/* Remote nodes with dirty == -1 are marked stale during a refresh. */
void registry_remote_begin(Registry *r) {
    for (size_t i = 0; i < r->node_count; i++) {
        if (!r->nodes[i]->local) r->nodes[i]->dirty = -1;
    }
}

MmNode *registry_remote_node(Registry *r, const char *node_id) {
    MmNode *n = registry_node(r, node_id);
    if (n && n->local) return NULL; /* ours: the local copy is authoritative */
    if (!n) n = node_add(r, node_id);
    if (!n) return NULL;
    n->dirty = 0;
    for (size_t i = 0; i < n->lobby_count; i++) lobby_clear(&n->lobbies[i]);
    n->lobby_count = 0;
    return n;
}

void registry_remote_end(Registry *r) {
    for (size_t i = 0; i < r->node_count;) {
        MmNode *n = r->nodes[i];
        if (!n->local && n->dirty == -1) {
            node_free(n);
            memmove(&r->nodes[i], &r->nodes[i + 1], (r->node_count - i - 1) * sizeof(MmNode *));
            r->node_count--;
        } else {
            i++;
        }
    }
}

/* ---- reservations */

static MmReservation *res_add(Registry *r, const char *jti, const char *game_id, int64_t expires_ms) {
    if (r->res_count == r->res_cap) {
        size_t cap = r->res_cap ? r->res_cap * 2 : 32;
        MmReservation *nr = realloc(r->res, cap * sizeof(*nr));
        if (!nr) return NULL;
        r->res = nr;
        r->res_cap = cap;
    }
    MmReservation *e = &r->res[r->res_count++];
    memset(e, 0, sizeof(*e));
    copy_str(e->jti, sizeof(e->jti), jti);
    copy_str(e->game_id, sizeof(e->game_id), game_id);
    e->expires_ms = expires_ms;
    return e;
}

static MmReservation *res_find(Registry *r, const char *jti) {
    if (!jti || !*jti) return NULL;
    for (size_t i = 0; i < r->res_count; i++) {
        if (!strcmp(r->res[i].jti, jti)) return &r->res[i];
    }
    return NULL;
}

void registry_reserve(Registry *r, const char *jti, const char *game_id, int64_t expires_ms) {
    res_add(r, jti, game_id, expires_ms);
}

void registry_ticket_authed(Registry *r, const char *jti, int64_t until_ms) {
    MmReservation *e = res_find(r, jti);
    if (e && !e->arrived && e->expires_ms > until_ms) e->expires_ms = until_ms;
}

void registry_arrive(Registry *r, const char *jti, const char *game_id, int64_t now_ms, int64_t grace_ms) {
    MmReservation *e = res_find(r, jti);
    if (!e) {
        if (!jti || !*jti || !game_id || !*game_id) return;
        e = res_add(r, jti, game_id, now_ms + grace_ms);
        if (!e) return;
        e->foreign = 1;
    }
    if (e->arrived) return;
    e->arrived = 1;
    if (e->expires_ms > now_ms + grace_ms) e->expires_ms = now_ms + grace_ms;
}

size_t registry_take_settled(Registry *r, const char *node_id, char jtis[][40], size_t max) {
    size_t c = 0, len = node_id ? strlen(node_id) : 0;
    if (!len) return 0;
    for (size_t i = 0; i < r->res_count && c < max;) {
        const MmReservation *e = &r->res[i];
        if (e->settled && !strncmp(e->game_id, node_id, len) && e->game_id[len] == ':') {
            copy_str(jtis[c++], 40, e->jti);
            r->res[i] = r->res[--r->res_count];
        } else {
            i++;
        }
    }
    return c;
}

void registry_release(Registry *r, const char *jti) {
    if (!jti) return;
    for (size_t i = 0; i < r->res_count; i++) {
        if (!strcmp(r->res[i].jti, jti)) {
            r->res[i] = r->res[--r->res_count];
            return;
        }
    }
}

void registry_expire(Registry *r, int64_t now_ms) {
    for (size_t i = 0; i < r->res_count;) {
        if (r->res[i].expires_ms <= now_ms) r->res[i] = r->res[--r->res_count];
        else i++;
    }
}

int registry_reserved(const Registry *r, const char *game_id, int64_t now_ms) {
    int c = 0;
    for (size_t i = 0; i < r->res_count; i++) {
        const MmReservation *e = &r->res[i];
        if (e->expires_ms > now_ms && !e->settled && !e->foreign && !strcmp(e->game_id, game_id)) c++;
    }
    return c;
}

int registry_lobby_taken(const Registry *r, const MmLobby *l, int64_t now_ms) {
    return l->players + l->reserved_remote + registry_reserved(r, l->game_id, now_ms);
}

int registry_lobby_cap(const MmLobby *l, const MmPolicy *p) {
    int cap = l->max_players > 0 ? l->max_players : (p && p->default_max_players > 0 ? p->default_max_players : 8);
    if (p && p->mode_caps) {
        const cJSON *c = NULL;
        if (l->mode[0]) {
            const cJSON *it;
            cJSON_ArrayForEach(it, p->mode_caps) {
                if (it->string && !strcasecmp(it->string, l->mode)) c = it;
            }
        }
        if (!c && l->mode_index >= 0) {
            char key[16];
            snprintf(key, sizeof(key), "%d", l->mode_index);
            c = cJSON_GetObjectItemCaseSensitive(p->mode_caps, key);
        }
        if (cJSON_IsNumber(c) && c->valuedouble > 0 && (int) c->valuedouble < cap) cap = (int) c->valuedouble;
    }
    return cap;
}

/* ---- queries */

int registry_player_count_all(const Registry *r) {
    int total = 0;
    for (size_t i = 0; i < r->node_count; i++) {
        for (size_t j = 0; j < r->nodes[i]->lobby_count; j++) total += r->nodes[i]->lobbies[j].players;
    }
    return total;
}

size_t registry_stale_local(const Registry *r, int64_t now_ms, int64_t ttl_ms, char ids[][MM_ID_MAX], size_t max) {
    size_t c = 0;
    for (size_t i = 0; i < r->node_count; i++) {
        const MmNode *n = r->nodes[i];
        if (n->local && now_ms - n->last_seen_ms > ttl_ms) {
            if (c < max) copy_str(ids[c], MM_ID_MAX, n->node_id);
            c++;
        }
    }
    return c;
}

static int lobby_joinable(const MmNode *n, const MmLobby *l) {
    return !n->draining && !l->draining;
}

int registry_lobby_needs_account(const MmNode *n, const MmLobby *l) {
    return n->online_mode || l->online_mode;
}

int registry_lobby_protected(const MmNode *n, const MmLobby *l) {
    return n->password || l->protect;
}

/* may the seeking player be sent here without asking for it by id */
static int lobby_auto_ok(const MmNode *n, const MmLobby *l, const MmPickQuery *q) {
    return !registry_lobby_protected(n, l) && (q->authed || !registry_lobby_needs_account(n, l));
}

static int version_ok(const MmLobby *l, const char *version) {
    return !version || !*version || !l->version[0] || !strcmp(l->version, version);
}

/* the client can speak the lobby's protocol (same major; release_info.h) */
static int proto_ok(const MmLobby *l, const MmPickQuery *q) {
    return q->proto_major <= 0 || l->proto_major < 0 || l->proto_major == q->proto_major;
}

/* the lobby plays the client's Krunker version (ruleset.h, "v10"): quick join never puts a client
 * into a lobby that announces another one (a version-toggle server playing v7.1.4) */
static int ruleset_ok(const MmLobby *l, const MmPickQuery *q) {
    const char *want = q->ruleset && *q->ruleset ? q->ruleset : "v10";
    return !l->ruleset[0] || !strcmp(l->ruleset, want);
}

/* v10 seek_modes / seek_maps (dataQuery dsmo / dsma): the lobby plays a wanted mode and map */
static int int_in(const int *list, int count, int v) {
    for (int i = 0; i < count; i++) {
        if (list[i] == v) return 1;
    }
    return 0;
}

static int prefs_ok(const MmLobby *l, const MmPickQuery *q) {
    if (q->mode_count > 0 && !int_in(q->modes, q->mode_count, l->mode_index)) return 0;
    if (q->map_count > 0 && !int_in(q->maps, q->map_count, l->map_index)) return 0;
    return 1;
}

static int lobby_outdated(const MmLobby *l, const MmPickQuery *q) {
    return q->outdated ? q->outdated(l, q->outdated_ud) != 0 : 0;
}

static int region_eq(const MmNode *n, const char *region) {
    return !region || !*region || !strcmp(n->region, region);
}

/* Taken seats over every lobby of a node (players + live reservations). */
static int node_taken(const Registry *r, const MmNode *n, int64_t now_ms) {
    int t = 0;
    for (size_t j = 0; j < n->lobby_count; j++) t += registry_lobby_taken(r, &n->lobbies[j], now_ms);
    return t;
}

/* Quick-join rank of a candidate (registry.h registry_pick): compared field by field. */
typedef struct PickRank {
    int outdated;                  /* lower first */
    int taken;                     /* higher first */
    int time_left;                 /* higher first (unknown = a full round: not penalised) */
    int node_load;                 /* lower first */
} PickRank;

static int rank_better(const PickRank *a, const PickRank *b) {
    if (a->outdated != b->outdated) return a->outdated < b->outdated;
    if (a->taken != b->taken) return a->taken > b->taken;
    if (a->time_left != b->time_left) return a->time_left > b->time_left;
    return a->node_load < b->node_load;
}

/* id is one of the comma-separated ids of list (whole entries only) */
static int id_in_list(const char *id, const char *list) {
    const size_t n = strlen(id);
    for (const char *p = list; p && *p;) {
        const char *end = strchr(p, ',');
        const size_t len = end ? (size_t) (end - p) : strlen(p);
        if (len == n && !strncmp(p, id, n)) return 1;
        p = end ? end + 1 : NULL;
    }
    return 0;
}

/* Steps 2-4 of pickBestLobby over the public pool (optionally one region); exclude / recent:
 * lobbies skipped (excludeGame, excludeRecent's list), NULL = none. */
static int pick_public(Registry *r, const MmPolicy *p, const MmPickQuery *q, const char *region,
                       int use_soft_cap, const char *exclude, const char *recent, MmPick *out) {
    MmNode *best_n = NULL;
    MmLobby *best = NULL;
    PickRank best_rank = {0};
    for (size_t i = 0; i < r->node_count; i++) {
        MmNode *n = r->nodes[i];
        if (!region_eq(n, region)) continue;
        int load = -1; /* computed once per node, when one of its lobbies qualifies */
        for (size_t j = 0; j < n->lobby_count; j++) {
            MmLobby *l = &n->lobbies[j];
            if (l->custom || l->no_seek || !registry_lobby_in_pool(n, l, q->flavor) || !lobby_joinable(n, l) || !lobby_auto_ok(n, l, q) ||
                !version_ok(l, q->version) || !proto_ok(l, q) || !ruleset_ok(l, q) || !prefs_ok(l, q) ||
                (exclude && !strcmp(l->game_id, exclude)) || (recent && id_in_list(l->game_id, recent)))
                continue;
            int taken = registry_lobby_taken(r, l, q->now_ms);
            int cap = registry_lobby_cap(l, p);
            if (use_soft_cap && p->soft_cap > 0 && p->soft_cap < cap) cap = p->soft_cap;
            if (taken >= cap) continue;
            if (load < 0) load = node_taken(r, n, q->now_ms);
            const PickRank rank = {.outdated = lobby_outdated(l, q), .taken = taken,
                                   .time_left = l->time_left < 0 ? 1 << 30 : l->time_left, .node_load = load};
            if (!best || rank_better(&rank, &best_rank)) {
                best = l;
                best_n = n;
                best_rank = rank;
            }
        }
    }
    if (!best) return 0;
    out->node = best_n;
    out->lobby = best;
    return 1;
}

MmPickError registry_pick(Registry *r, const MmPolicy *p, const MmPickQuery *q, MmPick *out) {
    memset(out, 0, sizeof(*out));
    /* 1. requested lobby */
    if (q->game_id && *q->game_id) {
        MmNode *n = NULL;
        /* another flavor's lobby is not found: a Box Office client is never ticketed into a Strike
         * lobby (its server would refuse the HELLO) and the other way round */
        MmLobby *l = registry_lobby_find_pool(r, q->game_id, q->flavor, &n);
        if (l && lobby_joinable(n, l) && !proto_ok(l, q)) {
            /* another protocol major: the client could not play there */
            if (!q->auto_change) return MM_PICK_INCOMPATIBLE;
        } else if (l && lobby_joinable(n, l) && !q->authed && registry_lobby_needs_account(n, l)) {
            /* a guest asking for a legacy onlineMode lobby: that server would refuse it */
            if (!q->auto_change) return MM_PICK_ACCOUNT_REQUIRED;
        } else if (l && lobby_joinable(n, l)) {
            int taken = registry_lobby_taken(r, l, q->now_ms);
            if (q->force || taken < l->max_players) {
                out->node = n;
                out->lobby = l;
                return MM_PICK_OK;
            }
            if (!q->auto_change) return MM_PICK_GAME_FULL;
        } else if (!q->auto_change) {
            return MM_PICK_INVALID_GAME;
        }
        out->changed = 1;
    }
    /* 2./3. the best public lobby with room (registry.h): region first, then all; with
     * excludeGame (v10 Quick Match) / excludeRecent every other lobby first, then the recent ones.
     * The excludeGame lobby itself is never answered (KrunkNative: the player leaves it, so a seek
     * that excludes it reports MM_PICK_NO_OTHER_LOBBY instead of sending them back). */
    int soft = p->soft_cap > 0;
    const char *exclude = q->exclude_game && *q->exclude_game ? q->exclude_game : NULL;
    const char *recent = q->exclude_recent && *q->exclude_recent ? q->exclude_recent : NULL;
    for (int ex = recent ? 0 : exclude ? 1 : 2; ex < (exclude ? 2 : 3); ex++) {
        const char *skip = ex <= 1 ? exclude : NULL, *skip_recent = ex == 0 ? recent : NULL;
        for (int pass = soft ? 0 : 1; pass < 2; pass++) {
            int use_soft = pass == 0;
            if (pick_public(r, p, q, q->region, use_soft, skip, skip_recent, out)) return MM_PICK_OK;
            if (p->cross_region && q->region && *q->region && pick_public(r, p, q, NULL, use_soft, skip, skip_recent, out))
                return MM_PICK_OK;
        }
    }
    /* 4. nothing with room (the v3.1.1 overflow pick would put a player into a
     *    full lobby; the game server would refuse it, so report it instead). */
    int any = 0, any_version = 0, any_room = 0;
    for (size_t i = 0; i < r->node_count; i++) {
        for (size_t j = 0; j < r->nodes[i]->lobby_count; j++) {
            const MmLobby *l = &r->nodes[i]->lobbies[j];
            if (l->custom || l->no_seek || !registry_lobby_in_pool(r->nodes[i], l, q->flavor) || !lobby_auto_ok(r->nodes[i], l, q)) continue;
            any = 1;
            if (!version_ok(l, q->version) || !proto_ok(l, q) || !ruleset_ok(l, q)) continue;
            any_version = 1;
            /* a seat for this client, only not in a wanted mode / map */
            if (lobby_joinable(r->nodes[i], l) && registry_lobby_taken(r, l, q->now_ms) < registry_lobby_cap(l, p)) any_room = 1;
        }
    }
    out->changed = 0;
    if (any && !any_version) return MM_PICK_NO_MATCH;
    /* the only lobby with room is the one the seek excludes: say so (the client stays put) */
    if (exclude) {
        MmNode *en = NULL;
        const MmLobby *el = registry_lobby_find_pool(r, exclude, q->flavor, &en);
        if (el && !el->custom && !el->no_seek && lobby_joinable(en, el) && lobby_auto_ok(en, el, q) && version_ok(el, q->version) &&
            proto_ok(el, q) && ruleset_ok(el, q) && prefs_ok(el, q) && registry_lobby_taken(r, el, q->now_ms) < registry_lobby_cap(el, p))
            return MM_PICK_NO_OTHER_LOBBY;
    }
    return any_room && (q->mode_count > 0 || q->map_count > 0) ? MM_PICK_NO_PREF_MATCH : MM_PICK_NONE;
}

static int node_host_free(const Registry *r, const MmNode *n, int authed, int64_t now_ms) {
    if (!n->custom || n->draining || n->password) return 0;
    /* remote nodes: the flags are known through their lobby tuples only */
    if (!authed && n->online_mode) return 0;
    if (!authed && !n->local && n->lobby_count == 0) return 0;
    for (size_t j = 0; j < n->lobby_count; j++) {
        const MmLobby *l = &n->lobbies[j];
        /* a private custom game (ranked, passworded host) does not lock the node; the
         * public lobby of a server-wide sv_password does */
        if ((l->protect && !l->custom) || (!authed && l->online_mode)) return 0;
    }
    if ((int) n->lobby_count < n->max_lobbies) return 1;
    for (size_t j = 0; j < n->lobby_count; j++) {
        const MmLobby *l = &n->lobbies[j];
        /* idle = nobody playing and no seat handed out (a quick-join ticket in flight) */
        if (!l->custom && !l->draining && registry_lobby_taken(r, l, now_ms) == 0) return 1;
    }
    return 0;
}

static int node_load(const MmNode *n) {
    int p = 0;
    for (size_t j = 0; j < n->lobby_count; j++) p += n->lobbies[j].players;
    return p;
}

MmNode *registry_pick_host_node(Registry *r, const char *region, const char *brain_id, int cross_region,
                                int authed, int64_t now_ms, const char *flavor) {
    for (int pass = 0; pass < 2; pass++) {
        const char *reg = pass == 0 ? region : NULL;
        if (pass == 1 && (!cross_region || !region || !*region)) break;
        MmNode *best = NULL;
        int best_score = 0;
        for (size_t i = 0; i < r->node_count; i++) {
            MmNode *n = r->nodes[i];
            if (!region_eq(n, reg) || !registry_flavor_match(registry_node_flavor(n), flavor) ||
                !node_host_free(r, n, authed, now_ms)) continue;
            /* own nodes first, then least loaded */
            int score = (brain_id && !strcmp(n->brain_id, brain_id) ? 0 : 100000) + node_load(n);
            if (!best || score < best_score) {
                best = n;
                best_score = score;
            }
        }
        if (best) return best;
    }
    return NULL;
}

/* ---- /mm/ping-list (public_routes.c h_ping_list; the keys: doc/v10/regions.md) */

/* "http://h:p/x" -> "h:p", without the port when it is 443 (registry.ts regions()).
 * ClientManager.getRegion() then fetches http://<value>/ping when the value ends
 * with ":6767", https://<value>/ping otherwise. */
static void url_to_ping_host(const char *url, char *out, size_t cap) {
    kn_url u;
    if (kn_url_parse(url, &u) == 0) {
        if (u.port && u.port != 443) snprintf(out, cap, "%s:%d", u.host, u.port);
        else snprintf(out, cap, "%s", u.host);
        return;
    }
    snprintf(out, cap, "%s", url);
}

/* brain.regions[] entry by id (NULL when none) */
static const cJSON *ping_region_conf(const cJSON *regions, const char *id) {
    const cJSON *rc;
    cJSON_ArrayForEach(rc, regions) {
        if (!strcmp(kn_json_str(rc, "id", ""), id)) return rc;
    }
    return NULL;
}

/* The region ids the v10.0.2 client pings (getRegion: availableRegions; the production list, plus
 * ["local"] on the dev host). Dedicated-only ids (us-il, ...) are never pinged, so never listed. */
static const char *k_v10_pinged[] = {"de-fra", "us-ca-sv", "au-syd", "jb-hnd", "sgp", "us-nj",
                                      "as-mb", "us-tx", "brz", "me-bhn", "sss", "local"};

/* KrunkNative node region codes (inventory kn_gameserver_region) -> the v10 id each is listed under
 * (doc/v10/regions.md has the reason per node). */
static const struct {
    const char *kn, *v10;
} k_v10_regions[] = {
    {"SV", "us-ca-sv"}, /* Santa Clara */
    {"LA", "us-ca-sv"}, /* Los Angeles: us-ca-la is not pinged; Silicon Valley is the nearest */
    {"CH", "us-nj"},    /* Chicago: us-il is dedicated-only; New York is the nearest pinged */
    {"VA", "us-nj"},    /* Ashburn, Virginia: New York is the nearest pinged */
};

const char *registry_v10_region(const char *kn_region) {
    if (!kn_region || !*kn_region) return NULL;
    for (size_t i = 0; i < sizeof(k_v10_regions) / sizeof(k_v10_regions[0]); i++) {
        if (!strcmp(kn_region, k_v10_regions[i].kn)) return k_v10_regions[i].v10;
    }
    /* a node already named by a v10 id (de-fra, us-nj, local, ...) is listed under it */
    for (size_t i = 0; i < sizeof(k_v10_pinged) / sizeof(k_v10_pinged[0]); i++) {
        if (!strcmp(kn_region, k_v10_pinged[i])) return k_v10_pinged[i];
    }
    return NULL;
}

cJSON *registry_ping_list(const Registry *r, const char *flavor, const cJSON *regions, const char *public_url) {
    cJSON *o = cJSON_CreateObject();
    for (size_t i = 0; i < r->node_count; i++) {
        const MmNode *n = r->nodes[i];
        const char *id = registry_v10_region(n->region);
        if (!id || !*id || cJSON_GetObjectItemCaseSensitive(o, id)) continue;
        if (!registry_flavor_match(registry_node_flavor(n), flavor)) continue;
        char host[300] = "";
        const cJSON *rc = ping_region_conf(regions, id);
        if (!rc) rc = ping_region_conf(regions, n->region);
        const char *ping = kn_json_str(rc, "ping_host", NULL);
        if (ping && *ping) {
            snprintf(host, sizeof(host), "%s", ping);
        } else {
            const char *url = public_url ? public_url : "";
            for (size_t k = 0; k < r->brain_count; k++) {
                if (!strcmp(r->brains[k].brain_id, n->brain_id) && r->brains[k].public_url[0])
                    url = r->brains[k].public_url;
            }
            url_to_ping_host(url, host, sizeof(host));
        }
        cJSON_AddStringToObject(o, id, host);
    }
    return o;
}

cJSON *registry_lobby_tuple(const Registry *r, const MmLobby *l, int64_t now_ms) {
    (void) r;
    (void) now_ms;
    cJSON *t = cJSON_Duplicate(l->tuple, 1);
    if (!t) return NULL;
    if (l->draining) {
        cJSON *meta = cJSON_GetArrayItem(t, 4);
        if (cJSON_IsObject(meta) && !cJSON_GetObjectItemCaseSensitive(meta, "draining"))
            cJSON_AddBoolToObject(meta, "draining", 1);
    }
    return t;
}

cJSON *registry_game_list(const Registry *r, int64_t now_ms, int *total_players) {
    return registry_game_list_pool(r, now_ms, "all", total_players);
}

cJSON *registry_game_list_pool(const Registry *r, int64_t now_ms, const char *flavor, int *total_players) {
    cJSON *arr = cJSON_CreateArray();
    int total = 0;
    for (size_t i = 0; i < r->node_count; i++) {
        const MmNode *n = r->nodes[i];
        for (size_t j = 0; j < n->lobby_count; j++) {
            const MmLobby *l = &n->lobbies[j];
            if (!registry_lobby_in_pool(n, l, flavor)) continue;
            cJSON *t = registry_lobby_tuple(r, l, now_ms);
            if (n->draining) {
                cJSON *meta = cJSON_GetArrayItem(t, 4);
                if (cJSON_IsObject(meta) && !cJSON_GetObjectItemCaseSensitive(meta, "draining"))
                    cJSON_AddBoolToObject(meta, "draining", 1);
            }
            if (t) cJSON_AddItemToArray(arr, t);
            total += l->players;
        }
    }
    if (total_players) *total_players = total;
    return arr;
}
