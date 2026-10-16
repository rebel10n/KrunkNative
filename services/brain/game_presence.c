/*
 * services/brain/game_presence.c - account -> the game it plays in, pushed to its friends
 * (game_presence.h; Krunker v10.0.2 event-bus "presence:game").
 */
#include "game_presence.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "registry.h"
#include "social_ws.h"
#include "knsvc/db.h"
#include "knsvc/log.h"

#define MOD "presence"

typedef struct GamePresence {
    int64_t account_id;
    char node_id[MM_ID_MAX];
    char game_id[MM_GAME_ID_MAX];
} GamePresence;

static GamePresence *g_rows;
static size_t g_count, g_cap;

/* "<nodeId>:<lobby>" of a node's lobby id (already a full game id when it has a ':') */
static void full_game_id(const char *node_id, const char *lobby_id, char *out, size_t n) {
    if (strchr(lobby_id, ':')) snprintf(out, n, "%s", lobby_id);
    else snprintf(out, n, "%s:%s", node_id, lobby_id);
}

static GamePresence *find(int64_t account_id) {
    for (size_t i = 0; i < g_count; i++)
        if (g_rows[i].account_id == account_id) return &g_rows[i];
    return NULL;
}

static void remove_at(size_t i) {
    g_rows[i] = g_rows[--g_count];
}

static int lobby_protected(const char *game_id) {
    Brain *b = brain_get();
    MmNode *node = NULL;
    MmLobby *l = b && b->registry ? registry_lobby(b->registry, game_id, &node) : NULL;
    return l && node ? registry_lobby_protected(node, l) : 0;
}

void game_presence_describe(const char *game_id, cJSON *obj) {
    Brain *b = brain_get();
    MmNode *node = NULL;
    MmLobby *l = obj && game_id && *game_id && b && b->registry ? registry_lobby(b->registry, game_id, &node) : NULL;
    if (!l || !node) return;
    cJSON_AddStringToObject(obj, "gameFlavor", l->flavor[0] ? l->flavor : "base"); /* registry flavor pool (strike-backend) */
    cJSON_AddStringToObject(obj, "gameMode", l->mode);
    cJSON_AddStringToObject(obj, "gameMap", l->map);
    cJSON_AddNumberToObject(obj, "gamePlayers", l->players);
    cJSON_AddNumberToObject(obj, "gameMaxPlayers", l->max_players);
}

int game_presence_get(int64_t account_id, char *game_id, size_t n, int *password_protected) {
    const GamePresence *p = account_id > 0 ? find(account_id) : NULL;
    if (game_id && n) snprintf(game_id, n, "%s", p ? p->game_id : "");
    if (password_protected) *password_protected = p ? lobby_protected(p->game_id) : 0;
    return p != NULL;
}

/* ---------------------------------------------------------------- pushes to friends */

typedef struct PushCtx {
    int64_t account_id;
} PushCtx;

/* ["eb", {"type":"presence:game","id":id,"gameInstanceId":gid|null,"gamePasswordProtected":b}]
 * (v10-social's event-bus tunnel over the social socket, social_eb.c) */
static cJSON *presence_frame(int64_t account_id) {
    char gid[MM_GAME_ID_MAX];
    int pw = 0;
    const int in_game = game_presence_get(account_id, gid, sizeof gid, &pw);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "presence:game");
    cJSON_AddNumberToObject(o, "id", (double) account_id);
    if (in_game) cJSON_AddStringToObject(o, "gameInstanceId", gid);
    else cJSON_AddNullToObject(o, "gameInstanceId");
    cJSON_AddBoolToObject(o, "gamePasswordProtected", in_game && pw);
    if (in_game) game_presence_describe(gid, o);
    cJSON *f = cJSON_CreateArray();
    cJSON_AddItemToArray(f, cJSON_CreateString("eb"));
    cJSON_AddItemToArray(f, o);
    return f;
}

static void friends_done(kn_db_result *res, void *ud) {
    PushCtx *c = ud;
    if (!kn_db_ok(res)) {
        KN_WARN(MOD, "friends of %lld: %s", (long long) c->account_id, kn_db_error(res));
        free(c);
        return;
    }
    const int rows = kn_db_rows(res);
    if (rows > 0) {
        /* the state when the friends are known (a join and a quick leave send the latter twice) */
        cJSON *frame = presence_frame(c->account_id);
        for (int i = 0; i < rows; i++) {
            const int64_t friend_id = kn_db_get_int64(res, i, 0, 0);
            if (friend_id > 0) social_push_account(friend_id, cJSON_Duplicate(frame, 1));
        }
        cJSON_Delete(frame);
    }
    free(c);
}

/* v10's friends are mutual follows ("You can add new friends by following eachother") */
static void push_to_friends(Brain *b, int64_t account_id) {
    if (!b || !b->db || account_id <= 0) return;
    PushCtx *c = calloc(1, sizeof *c);
    if (!c) return;
    c->account_id = account_id;
    char id[24];
    snprintf(id, sizeof id, "%lld", (long long) account_id);
    static const kn_sql Q = {
        .any = "SELECT a.follower_id FROM follows a JOIN follows m ON m.follower_id = a.followee_id "
               "AND m.followee_id = a.follower_id WHERE a.followee_id = $1"};
    if (kn_db_query_sql(b->db, &Q, KN_DB_ARGS(id), friends_done, c) != 0) free(c);
}

/* ---------------------------------------------------------------- feed (internal_ws.c) */

void game_presence_join(Brain *b, const char *node_id, const char *lobby_id, int64_t account_id) {
    if (account_id <= 0 || !node_id || !lobby_id || !*lobby_id) return;
    char gid[MM_GAME_ID_MAX];
    full_game_id(node_id, lobby_id, gid, sizeof gid);
    GamePresence *p = find(account_id);
    if (p && !strcmp(p->game_id, gid)) return; /* a transport reconnect into the same game */
    if (!p) {
        if (g_count == g_cap) {
            const size_t cap = g_cap ? g_cap * 2 : 64;
            GamePresence *rows = realloc(g_rows, cap * sizeof *rows);
            if (!rows) return;
            g_rows = rows;
            g_cap = cap;
        }
        p = &g_rows[g_count++];
        p->account_id = account_id;
    }
    snprintf(p->node_id, sizeof p->node_id, "%s", node_id);
    snprintf(p->game_id, sizeof p->game_id, "%s", gid);
    KN_DEBUG(MOD, "account %lld in %s", (long long) account_id, gid);
    push_to_friends(b, account_id);
}

void game_presence_leave(Brain *b, const char *node_id, const char *lobby_id, int64_t account_id) {
    if (account_id <= 0 || !node_id || !lobby_id) return;
    char gid[MM_GAME_ID_MAX];
    full_game_id(node_id, lobby_id, gid, sizeof gid);
    for (size_t i = 0; i < g_count; i++) {
        /* only the game it left: the join of the next game may arrive first (another node) */
        if (g_rows[i].account_id != account_id || strcmp(g_rows[i].game_id, gid) != 0) continue;
        remove_at(i);
        KN_DEBUG(MOD, "account %lld left %s", (long long) account_id, gid);
        push_to_friends(b, account_id);
        return;
    }
}

void game_presence_node_gone(Brain *b, const char *node_id) {
    if (!node_id) return;
    for (size_t i = 0; i < g_count;) {
        if (strcmp(g_rows[i].node_id, node_id) != 0) {
            i++;
            continue;
        }
        const int64_t id = g_rows[i].account_id;
        remove_at(i);
        push_to_friends(b, id);
    }
}
