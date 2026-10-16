/*
 * services/brain/social_eb.c - Krunker v10.0.2's friends "event bus" (the Social hub's Friends
 * tab: friends list, online presence, direct messages) over the brain's social socket.
 *
 * v10 opens its own WebSocket (`<eventBusURL>/ws?at=<access token>`) and speaks JSON objects
 * {"type": ...}. KrunkNative carries the same objects over the social socket the client already
 * has open (doc/backend.md 7.4): ["eb", {...}] both ways, JSON clients only. Deviation: one socket
 * instead of two, so auth is the socket's session and no access token travels in a URL.
 *
 * Friends are mutual follows (v10's own rule, i18n generic.tips.privateMessages: "Follow someone
 * back to join their games through the friends tab & send private messages"). Messages are the
 * chat_messages rows of the v3.1.1 hub chat (0101_social.sql), so the hub page and the v10
 * Friends tab share one history. Online = a signed-in social socket on any brain (Redis presence,
 * hot.h; this brain's sockets without Redis).
 *
 * client -> brain                      brain -> client
 *   friends:list {offset}                friends:list {friends:[...], offset, hasMore}
 *   presence:ping                        presence:pong
 *   msg:send {to, text}                  msg:sent {id, to, ts} (sender) + msg:recv {id, from, text, ts} (peer)
 *   msg:history {peer, before, limit}    msg:history:result {peer, requestBefore, messages, hasMore}
 *   msg:read {peer}                      -
 *   game:invite {to, gameId}             game:invite:sent {to, gameId} (sender) + game:invite {from, name,
 *                                        gameId} (peer; KrunkNative, Krunker Strike's invites)
 *   (login)                              unread:summary {counts: {<id>: {count, name}}}
 *   (presence changes)                   presence:online / presence:offline {id} to online friends
 *   (a follow made / broke a friendship) friends:list {offset: 0} to both, when online
 *   (a new follow, KrunkNative)          friend:request / friend:accepted {from, name} to the followee
 *   (errors)                             error {code, message}: EB_NOT_FRIEND, EB_TARGET_OFFLINE,
 *                                        EB_RATE_LIMITED, EB_TEXT_TOO_LONG, EB_MSG_INVALID, EB_INVITE_INVALID
 * Friend rows: {id, name, premium, online, unread, lastId, lastText, lastFromMe, lastTs} plus,
 * while the friend is in a game, gameInstanceId / gamePasswordProtected (game_presence_get() of
 * the v10-party module; its presence:game pushes are its own) and the lobby's gameFlavor /
 * gameMode / gameMap / gamePlayers / gameMaxPlayers (game_presence_describe: Krunker Strike). Messages are online-only like
 * v10 ("Messages are online-only in v1"): an offline friend answers EB_TARGET_OFFLINE.
 */
#include "game_presence.h" /* game_presence_get: a friend's game (v10-party) */
#include "acct_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hot.h"
#include "knsvc/json.h"
#include "knsvc/log.h"
#include "knsvc/loop.h"

#define MOD "eventbus"

#define EB_PAGE 50          /* friends:list page */
#define EB_TEXT_MAX 512     /* v10 conversation textarea maxlength (canSend: length <= 512) */
#define EB_HISTORY_MAX 50   /* msg:history limit cap (v10 asks 15) */
#define EB_RATE_PER_MIN 30  /* messages per sender and minute (the hub chat's limit) */


/* mutual follows of $1 */
#define FRIENDS_OF_1 \
    "SELECT f1.followee_id AS fid FROM follows f1 JOIN follows f2 ON f2.follower_id = f1.followee_id " \
    "AND f2.followee_id = f1.follower_id WHERE f1.follower_id = $1"

/* $1 = the viewer, $2 limit, $3 offset; newest conversation first, then by name (v10 sorts again) */
#define FRIENDS_SQL \
    "SELECT a.id AS id__i, a.username AS name, coalesce(p.player_premium, 0) AS premium__i, " \
    "coalesce(u.n, 0) AS unread__i, coalesce(m.id, 0) AS last_id__i, m.body AS last_text, " \
    "coalesce(m.sender_id, 0) AS last_from__i, coalesce({epoch_ms:m.created_at}, 0) AS last_ts__i " \
    "FROM (" FRIENDS_OF_1 ") fr JOIN accounts a ON a.id = fr.fid " \
    "LEFT JOIN players p ON p.player_id = a.id " \
    "LEFT JOIN (SELECT sender_id, count(*) AS n FROM chat_messages WHERE receiver_id = $1 AND read_at IS NULL " \
    "           GROUP BY sender_id) u ON u.sender_id = a.id " \
    "LEFT JOIN (SELECT CASE WHEN sender_id = $1 THEN receiver_id ELSE sender_id END AS peer, max(id) AS mid " \
    "           FROM chat_messages WHERE sender_id = $1 OR receiver_id = $1 GROUP BY 1) lm ON lm.peer = a.id " \
    "LEFT JOIN chat_messages m ON m.id = lm.mid " \
    "ORDER BY coalesce(m.id, 0) DESC, lower(a.username) LIMIT {int:$2} OFFSET {int:$3}"

/* ------------------------------------------------------------------ helpers */

static cJSON *eb_frame(cJSON *obj) {
    cJSON *f = social_frame("eb");
    cJSON_AddItemToArray(f, obj);
    return f;
}

static cJSON *eb_obj(const char *type) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", type);
    return o;
}

static void eb_send(SocialSession *s, cJSON *obj) {
    if (!s->json || !social_open(s)) {
        cJSON_Delete(obj);
        return;
    }
    social_send(s, eb_frame(obj));
}

static void eb_push(int64_t account_id, cJSON *obj) { social_push_account_ex(account_id, eb_frame(obj), 1); }

static void eb_error(SocialSession *s, const char *code, const char *message) {
    cJSON *o = eb_obj("error");
    cJSON_AddStringToObject(o, "code", code);
    cJSON_AddStringToObject(o, "message", message);
    eb_send(s, o);
}

static int64_t obj_int(const cJSON *o, const char *k, int64_t def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsNumber(v)) return (int64_t) v->valuedouble;
    if (cJSON_IsString(v) && v->valuestring[0]) return strtoll(v->valuestring, NULL, 10);
    return def;
}

/* ------------------------------------------------------------------ online lookup */

typedef void (*EbOnlineFn)(void *ud, const int *online, size_t n);

typedef struct OnlineWait {
    int64_t *ids;
    int *local;
    size_t n;
    EbOnlineFn cb;
    void *ud;
} OnlineWait;

static void online_redis(void *ud, const int *online, size_t n) {
    OnlineWait *w = ud;
    if (online)
        for (size_t i = 0; i < n && i < w->n; i++) w->local[i] |= online[i];
    w->cb(w->ud, w->local, w->n);
    free(w->ids);
    free(w->local);
    free(w);
}

/* cb(ud, online[], n): this brain's sockets, ORed with Redis presence (every brain) when it is up.
 * cb may run before this returns. */
static void eb_online(const int64_t *ids, size_t n, EbOnlineFn cb, void *ud) {
    OnlineWait *w = calloc(1, sizeof *w);
    int *local = calloc(n ? n : 1, sizeof *local);
    int64_t *copy = calloc(n ? n : 1, sizeof *copy);
    if (!w || !local || !copy) {
        free(w);
        free(copy);
        if (local)
            for (size_t i = 0; i < n; i++) local[i] = social_account_online(ids[i]);
        static const int none[1] = {0};
        cb(ud, local ? local : none, local ? n : 0);
        free(local);
        return;
    }
    for (size_t i = 0; i < n; i++) {
        copy[i] = ids[i];
        local[i] = social_account_online(ids[i]);
    }
    w->ids = copy;
    w->local = local;
    w->n = n;
    w->cb = cb;
    w->ud = ud;
    if (n == 0 || hot_presence_online(copy, n, online_redis, w) != 0) online_redis(w, NULL, 0);
}

void social_online_lookup(const int64_t *ids, size_t n, void (*cb)(void *ud, const int *online, size_t n), void *ud) {
    eb_online(ids, n, cb, ud);
}

/* ------------------------------------------------------------------ friends:list */

static void add_game(cJSON *row, int64_t id) {
    char game[96] = "";
    int locked = 0;
    if (game_presence_get(id, game, sizeof game, &locked) == 1 && game[0]) {
        cJSON_AddStringToObject(row, "gameInstanceId", game);
        cJSON_AddBoolToObject(row, "gamePasswordProtected", locked);
        game_presence_describe(game, row); /* gameFlavor / gameMode / gameMap / gamePlayers / gameMaxPlayers */
    }
}

/* a FRIENDS_SQL result kept as JSON rows until the online flags arrive */
typedef struct FriendsWait {
    SocialSession *s;  /* held (reply on this socket), or NULL: push to `me` */
    int64_t me, offset;
    cJSON *rows;       /* [{id, name, premium, unread, last_id, last_text, last_from, last_ts}] */
    int more;
} FriendsWait;

static void friends_online(void *ud, const int *online, size_t n) {
    FriendsWait *w = ud;
    cJSON *list = cJSON_CreateArray();
    size_t i = 0;
    const cJSON *r;
    cJSON_ArrayForEach(r, w->rows) {
        const int64_t id = kn_json_int(r, "id", 0);
        const int on = i < n && online[i];
        i++;
        cJSON *row = cJSON_CreateObject();
        cJSON_AddNumberToObject(row, "id", (double) id);
        cJSON_AddStringToObject(row, "name", kn_json_str(r, "name", ""));
        cJSON_AddBoolToObject(row, "premium", kn_json_int(r, "premium", 0) > 0);
        cJSON_AddBoolToObject(row, "online", on);
        cJSON_AddNumberToObject(row, "unread", (double) kn_json_int(r, "unread", 0));
        const int64_t last = kn_json_int(r, "last_id", 0);
        if (last > 0) {
            cJSON_AddNumberToObject(row, "lastId", (double) last);
            cJSON_AddStringToObject(row, "lastText", kn_json_str(r, "last_text", ""));
            cJSON_AddBoolToObject(row, "lastFromMe", kn_json_int(r, "last_from", 0) == w->me);
            cJSON_AddNumberToObject(row, "lastTs", (double) kn_json_int(r, "last_ts", 0));
        }
        if (on) add_game(row, id);
        cJSON_AddItemToArray(list, row);
    }
    cJSON *o = eb_obj("friends:list");
    cJSON_AddItemToObject(o, "friends", list);
    cJSON_AddNumberToObject(o, "offset", (double) w->offset);
    cJSON_AddBoolToObject(o, "hasMore", w->more);
    if (w->s) {
        eb_send(w->s, o);
        social_unhold(w->s);
    } else {
        eb_push(w->me, o);
    }
    cJSON_Delete(w->rows);
    free(w);
}

/* rows of a FRIENDS_SQL result -> the online lookup -> friends_online */
static void friends_rows(kn_db_result *res, SocialSession *s, int64_t me, int64_t offset) {
    FriendsWait *w = calloc(1, sizeof *w);
    if (!w) return;
    w->s = s ? social_hold(s) : NULL;
    w->me = me;
    w->offset = offset;
    w->rows = cJSON_CreateArray();
    const int rows = kn_db_rows(res);
    w->more = rows > EB_PAGE;
    int64_t ids[EB_PAGE];
    size_t n = 0;
    for (int r = 0; r < rows && r < EB_PAGE; r++) {
        cJSON *o = cJSON_CreateObject();
        ids[n++] = kn_db_get_int64(res, r, 0, 0);
        cJSON_AddNumberToObject(o, "id", (double) ids[n - 1]);
        cJSON_AddStringToObject(o, "name", kn_db_get(res, r, 1));
        cJSON_AddNumberToObject(o, "premium", (double) kn_db_get_int64(res, r, 2, 0));
        cJSON_AddNumberToObject(o, "unread", (double) kn_db_get_int64(res, r, 3, 0));
        cJSON_AddNumberToObject(o, "last_id", (double) kn_db_get_int64(res, r, 4, 0));
        cJSON_AddStringToObject(o, "last_text", kn_db_get(res, r, 5));
        cJSON_AddNumberToObject(o, "last_from", (double) kn_db_get_int64(res, r, 6, 0));
        cJSON_AddNumberToObject(o, "last_ts", (double) kn_db_get_int64(res, r, 7, 0));
        cJSON_AddItemToArray(w->rows, o);
    }
    eb_online(ids, n, friends_online, w);
}

static void friends_done(SocialSession *s, kn_db_result *res, void *extra) {
    const int64_t offset = *(int64_t *) extra;
    if (!kn_db_ok(res)) {
        eb_error(s, "EB_INTERNAL", acct_db_error(res, "friends:list"));
        return;
    }
    friends_rows(res, s, s->account_id, offset);
}

static void friends_list(SocialSession *s, int64_t offset) {
    char me[24], lim[24], off[24];
    if (offset < 0) offset = 0;
    int64_t *x = malloc(sizeof *x);
    if (!x) return;
    *x = offset;
    social_query(s, SQL(.any = FRIENDS_SQL), KN_DB_ARGS(social_viewer(s, me), acct_itoa(lim, EB_PAGE + 1), acct_itoa(off, offset)),
                 friends_done, x);
}

/* the account's first friends page, pushed to its sockets (v10 replaces its list on offset 0) */
static void refresh_done(kn_db_result *res, void *ud) {
    int64_t *id = ud;
    if (kn_db_ok(res)) friends_rows(res, NULL, *id, 0);
    free(id);
}

static void refresh_list_of(int64_t account_id) {
    int64_t *id = malloc(sizeof *id);
    if (!id) return;
    *id = account_id;
    char me[24], lim[24];
    if (kn_db_query_sql(g_acct_brain->db, SQL(.any = FRIENDS_SQL), KN_DB_ARGS(acct_itoa(me, account_id), acct_itoa(lim, EB_PAGE + 1), "0"),
                        refresh_done, id) != 0)
        free(id);
}

/* ------------------------------------------------------------------ messages */

typedef struct SendCtx {
    int64_t to;
    char *text;
} SendCtx;

static void send_ctx_free(SendCtx *x) {
    if (!x) return;
    free(x->text);
    free(x);
}

static void sent_done(SocialSession *s, kn_db_result *res, void *extra) {
    SendCtx *x = extra;
    const int returned = kn_db_ok(res) && kn_db_rows(res) > 0; /* RETURNING; mysql: the insert id */
    if (!kn_db_ok(res) || (!returned && kn_db_affected(res) < 1)) {
        eb_error(s, "EB_MSG_INVALID", kn_db_ok(res) ? "Message not sent." : acct_db_error(res, "msg:send"));
        free(x->text);
        x->text = NULL;
        return;
    }
    const int64_t id = returned ? kn_db_get_int64(res, 0, 0, 0) : kn_db_last_id(res);
    const int64_t ts = returned ? kn_db_get_int64(res, 0, 1, 0) : kn_wall_ms();
    cJSON *o = eb_obj("msg:sent");
    cJSON_AddNumberToObject(o, "id", (double) id);
    cJSON_AddNumberToObject(o, "to", (double) x->to);
    cJSON_AddNumberToObject(o, "ts", (double) ts);
    eb_send(s, o);
    cJSON *r = eb_obj("msg:recv");
    cJSON_AddNumberToObject(r, "id", (double) id);
    cJSON_AddNumberToObject(r, "from", (double) s->account_id);
    cJSON_AddStringToObject(r, "text", x->text ? x->text : "");
    cJSON_AddNumberToObject(r, "ts", (double) ts);
    eb_push(x->to, r);
    free(x->text);
    x->text = NULL;
}

typedef struct CheckWait {
    SocialSession *s; /* held */
    SendCtx *x;
} CheckWait;

static void send_online(void *ud, const int *online, size_t n) {
    CheckWait *w = ud;
    SocialSession *s = w->s;
    SendCtx *x = w->x;
    free(w);
    if (!social_open(s)) {
        send_ctx_free(x);
    } else if (n < 1 || !online[0]) {
        eb_error(s, "EB_TARGET_OFFLINE", "That friend is offline right now.");
        send_ctx_free(x);
    } else {
        char me[24], peer[24];
        social_query(s,
                     SQL(.mysql = "INSERT INTO chat_messages (sender_id, receiver_id, body) VALUES ($1, $2, $3)",
                         .any = "INSERT INTO chat_messages (sender_id, receiver_id, body) VALUES ($1, $2, $3) "
                                "RETURNING id, {epoch_ms:created_at} AS ts"),
                     KN_DB_ARGS(social_viewer(s, me), acct_itoa(peer, x->to), x->text), sent_done, x);
    }
    social_unhold(s);
}

static void send_checked(SocialSession *s, kn_db_result *res, void *extra) {
    SendCtx *x = extra;
    if (!kn_db_ok(res) || kn_db_rows(res) < 1) {
        eb_error(s, "EB_MSG_INVALID", kn_db_ok(res) ? "Message not sent." : acct_db_error(res, "msg:send"));
    } else if (!kn_db_get_int64(res, 0, 0, 0)) {
        eb_error(s, "EB_NOT_FRIEND", "You can only message friends.");
    } else if (kn_db_get_int64(res, 0, 1, 0) >= EB_RATE_PER_MIN) {
        eb_error(s, "EB_RATE_LIMITED", "You are sending messages too quickly.");
    } else {
        CheckWait *w = calloc(1, sizeof *w);
        SendCtx *y = calloc(1, sizeof *y);
        if (w && y) {
            *y = *x;  /* the text moves to the insert's context (social_query frees `extra` itself) */
            x->text = NULL;
            w->s = social_hold(s);
            w->x = y;
            eb_online(&y->to, 1, send_online, w);
            return;
        }
        free(w);
        free(y);
    }
    free(x->text);
    x->text = NULL;
}

static void msg_send(SocialSession *s, const cJSON *o) {
    const int64_t to = obj_int(o, "to", 0);
    const char *text = kn_json_str(o, "text", NULL);
    if (!text || to <= 0 || to == s->account_id) {
        eb_error(s, "EB_MSG_INVALID", "Invalid message.");
        return;
    }
    while (*text == ' ' || *text == '\n' || *text == '\t' || *text == '\r') text++;
    size_t len = strlen(text);
    while (len && (text[len - 1] == ' ' || text[len - 1] == '\n' || text[len - 1] == '\t' || text[len - 1] == '\r')) len--;
    if (!len) {
        eb_error(s, "EB_MSG_INVALID", "Invalid message.");
        return;
    }
    size_t chars = 0; /* ~ the JS string length (code points) */
    for (size_t i = 0; i < len; i++) chars += ((unsigned char) text[i] & 0xC0) != 0x80;
    if (chars > EB_TEXT_MAX) {
        eb_error(s, "EB_TEXT_TOO_LONG", "Message is too long.");
        return;
    }
    SendCtx *x = calloc(1, sizeof *x);
    if (!x) return;
    x->to = to;
    x->text = malloc(len + 1);
    if (!x->text) {
        free(x);
        return;
    }
    memcpy(x->text, text, len);
    x->text[len] = 0;
    char me[24], peer[24];
    social_query(s,
                 SQL(.pg = "SELECT (EXISTS (SELECT 1 FROM follows WHERE follower_id = $1 AND followee_id = $2) "
                           "AND EXISTS (SELECT 1 FROM follows WHERE follower_id = $2 AND followee_id = $1))::int AS friend__i, "
                           "(SELECT count(*) FROM chat_messages WHERE sender_id = $1 AND created_at > now() - interval '1 minute') AS recent__i",
                     .any = "SELECT CASE WHEN EXISTS (SELECT 1 FROM follows WHERE follower_id = $1 AND followee_id = $2) "
                            "AND EXISTS (SELECT 1 FROM follows WHERE follower_id = $2 AND followee_id = $1) THEN 1 ELSE 0 END AS friend__i, "
                            "(SELECT count(*) FROM chat_messages WHERE sender_id = $1 AND created_at > {ago:60}) AS recent__i"),
                 KN_DB_ARGS(social_viewer(s, me), acct_itoa(peer, to)), send_checked, x);
}

/* ------------------------------------------------------------------ game invites */

/* KrunkNative: Krunker Strike's game invite (FRVR's social API, en.sendGameInvite(friendId, gameId):
 * the menu friends panel's Invite and Private Match's friend list; Krunker v10 has no invite).
 * A friend who is online gets ["eb", {"type":"game:invite","from":id,"name":username,"gameId":gid}]
 * and the sender ["eb", {"type":"game:invite:sent","to":id,"gameId":gid}]. The game id is the
 * sender's (a "<nodeId>:<lobby>" game or a private lobby code), joined the way a game link is. */
#define EB_INVITES_PER_MIN 20
#define EB_GAME_ID_MAX 96

typedef struct InviteCtx {
    int64_t to;
    char game[EB_GAME_ID_MAX];
} InviteCtx;

/* invites per sender and minute, in memory (a ring of the last senders) */
static struct { int64_t account; int64_t window_ms; int count; } g_invite_rate[64];

static int invite_rate_ok(int64_t account) {
    const int64_t now = kn_wall_ms();
    int slot = -1, oldest = 0;
    for (int i = 0; i < 64; i++) {
        if (g_invite_rate[i].account == account) { slot = i; break; }
        if (g_invite_rate[i].window_ms < g_invite_rate[oldest].window_ms) oldest = i;
    }
    if (slot < 0 || now - g_invite_rate[slot].window_ms >= 60000) {
        if (slot < 0) slot = oldest;
        g_invite_rate[slot].account = account;
        g_invite_rate[slot].window_ms = now;
        g_invite_rate[slot].count = 0;
    }
    return ++g_invite_rate[slot].count <= EB_INVITES_PER_MIN;
}

typedef struct InviteWait {
    SocialSession *s; /* held */
    InviteCtx x;
} InviteWait;

static void invite_online(void *ud, const int *online, size_t n) {
    InviteWait *w = ud;
    SocialSession *s = w->s;
    if (social_open(s)) {
        if (n < 1 || !online[0]) {
            eb_error(s, "EB_TARGET_OFFLINE", "That friend is offline right now.");
        } else {
            cJSON *r = eb_obj("game:invite");
            cJSON_AddNumberToObject(r, "from", (double) s->account_id);
            cJSON_AddStringToObject(r, "name", s->username);
            cJSON_AddStringToObject(r, "gameId", w->x.game);
            eb_push(w->x.to, r);
            cJSON *o = eb_obj("game:invite:sent");
            cJSON_AddNumberToObject(o, "to", (double) w->x.to);
            cJSON_AddStringToObject(o, "gameId", w->x.game);
            eb_send(s, o);
        }
    }
    social_unhold(s);
    free(w);
}

static void invite_checked(SocialSession *s, kn_db_result *res, void *extra) {
    InviteCtx *x = extra;
    if (!kn_db_ok(res) || kn_db_rows(res) < 1) {
        eb_error(s, "EB_INVITE_INVALID", kn_db_ok(res) ? "Invite not sent." : acct_db_error(res, "game:invite"));
        return;
    }
    if (!kn_db_get_int64(res, 0, 0, 0)) {
        eb_error(s, "EB_NOT_FRIEND", "You can only invite friends.");
        return;
    }
    InviteWait *w = calloc(1, sizeof *w);
    if (!w) return;
    w->s = social_hold(s);
    w->x = *x;
    eb_online(&w->x.to, 1, invite_online, w);
}

static void game_invite(SocialSession *s, const cJSON *o) {
    const int64_t to = obj_int(o, "to", 0);
    const char *game = kn_json_str(o, "gameId", "");
    size_t len = strlen(game);
    int ok = to > 0 && to != s->account_id && len > 0 && len < EB_GAME_ID_MAX;
    for (size_t i = 0; ok && i < len; i++) {
        const char c = game[i];
        ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ':' || c == '-' || c == '_' || c == '.';
    }
    if (!ok) {
        eb_error(s, "EB_INVITE_INVALID", "Invalid invite.");
        return;
    }
    if (!invite_rate_ok(s->account_id)) {
        eb_error(s, "EB_RATE_LIMITED", "You are sending invites too quickly.");
        return;
    }
    InviteCtx *x = calloc(1, sizeof *x);
    if (!x) return;
    x->to = to;
    snprintf(x->game, sizeof x->game, "%s", game);
    char me[24], peer[24];
    social_query(s,
                 SQL(.pg = "SELECT (EXISTS (SELECT 1 FROM follows WHERE follower_id = $1 AND followee_id = $2) "
                           "AND EXISTS (SELECT 1 FROM follows WHERE follower_id = $2 AND followee_id = $1))::int AS friend__i",
                     .any = "SELECT CASE WHEN EXISTS (SELECT 1 FROM follows WHERE follower_id = $1 AND followee_id = $2) "
                            "AND EXISTS (SELECT 1 FROM follows WHERE follower_id = $2 AND followee_id = $1) THEN 1 ELSE 0 END AS friend__i"),
                 KN_DB_ARGS(social_viewer(s, me), acct_itoa(peer, to)), invite_checked, x);
}

typedef struct HistCtx {
    int64_t peer, before, limit;
} HistCtx;

static void history_done(SocialSession *s, kn_db_result *res, void *extra) {
    HistCtx *x = extra;
    if (!kn_db_ok(res)) {
        eb_error(s, "EB_INTERNAL", acct_db_error(res, "msg:history"));
        return;
    }
    const int rows = kn_db_rows(res);
    const int n = rows > x->limit ? (int) x->limit : rows;
    cJSON *msgs = cJSON_CreateArray();
    for (int r = n - 1; r >= 0; r--) { /* newest first from the query; oldest first on the wire */
        cJSON *m = cJSON_CreateObject();
        cJSON_AddNumberToObject(m, "id", (double) kn_db_get_int64(res, r, 0, 0));
        cJSON_AddNumberToObject(m, "from", (double) kn_db_get_int64(res, r, 1, 0));
        cJSON_AddStringToObject(m, "text", kn_db_get(res, r, 2));
        cJSON_AddNumberToObject(m, "ts", (double) kn_db_get_int64(res, r, 3, 0));
        cJSON_AddItemToArray(msgs, m);
    }
    cJSON *o = eb_obj("msg:history:result");
    cJSON_AddNumberToObject(o, "peer", (double) x->peer);
    if (x->before > 0) cJSON_AddNumberToObject(o, "requestBefore", (double) x->before);
    else cJSON_AddNullToObject(o, "requestBefore");
    cJSON_AddItemToObject(o, "messages", msgs);
    cJSON_AddBoolToObject(o, "hasMore", rows > x->limit);
    eb_send(s, o);
}

static void msg_history(SocialSession *s, const cJSON *o) {
    HistCtx *x = calloc(1, sizeof *x);
    if (!x) return;
    x->peer = obj_int(o, "peer", 0);
    x->before = obj_int(o, "before", 0);
    x->limit = obj_int(o, "limit", 15);
    if (x->before < 0) x->before = 0;
    if (x->limit < 1) x->limit = 1;
    if (x->limit > EB_HISTORY_MAX) x->limit = EB_HISTORY_MAX;
    char me[24], p[24], b[24], l[24];
    social_query(s,
                 SQL(.any = "SELECT id AS id__i, sender_id AS from_id__i, body, {epoch_ms:created_at} AS ts__i FROM chat_messages "
                            "WHERE ((sender_id = $1 AND receiver_id = $2) OR (sender_id = $2 AND receiver_id = $1)) "
                            "AND ({int:$3} = 0 OR id < {int:$3}) ORDER BY id DESC LIMIT {int:$4}"),
                 KN_DB_ARGS(social_viewer(s, me), acct_itoa(p, x->peer), acct_itoa(b, x->before), acct_itoa(l, x->limit + 1)),
                 history_done, x);
}

static void read_done(SocialSession *s, kn_db_result *res, void *extra) {
    (void) s;
    (void) extra;
    if (!kn_db_ok(res)) acct_db_error(res, "msg:read");
}

static void msg_read(SocialSession *s, const cJSON *o) {
    const int64_t peer = obj_int(o, "peer", 0);
    if (peer <= 0) return;
    char me[24], p[24];
    social_query(s,
                 SQL(.pg = "UPDATE chat_messages SET read_at = now() WHERE receiver_id = $1 AND sender_id = $2 AND read_at IS NULL",
                     .any = "UPDATE chat_messages SET read_at = {now} WHERE receiver_id = $1 AND sender_id = $2 AND read_at IS NULL"),
                 KN_DB_ARGS(social_viewer(s, me), acct_itoa(p, peer)), read_done, NULL);
}

/* ------------------------------------------------------------------ unread summary (login) */

static void unread_done(SocialSession *s, kn_db_result *res, void *extra) {
    (void) extra;
    if (!kn_db_ok(res)) return;
    cJSON *counts = cJSON_CreateObject();
    for (int r = 0; r < kn_db_rows(res); r++) {
        cJSON *c = cJSON_CreateObject();
        cJSON_AddNumberToObject(c, "count", (double) kn_db_get_int64(res, r, 1, 0));
        cJSON_AddStringToObject(c, "name", kn_db_get(res, r, 2));
        cJSON_AddItemToObject(counts, kn_db_get(res, r, 0), c);
    }
    cJSON *o = eb_obj("unread:summary");
    cJSON_AddItemToObject(o, "counts", counts);
    eb_send(s, o);
}

void social_eb_after_login(SocialSession *s) {
    if (!s->json || s->account_id <= 0) return;
    char me[24];
    social_query(s,
                 SQL(.any = "SELECT m.sender_id AS id__i, count(*) AS n__i, a.username AS name FROM chat_messages m "
                            "JOIN accounts a ON a.id = m.sender_id WHERE m.receiver_id = $1 AND m.read_at IS NULL "
                            "GROUP BY m.sender_id, a.username"),
                 KN_DB_ARGS(social_viewer(s, me)), unread_done, NULL);
}

/* ------------------------------------------------------------------ presence pushes */

typedef struct PresenceCtx {
    int64_t id;
    int online;
    int64_t *friends;
    size_t n;
} PresenceCtx;

static void presence_push(void *ud, const int *online, size_t n) {
    PresenceCtx *p = ud;
    for (size_t i = 0; i < n && i < p->n; i++) {
        if (!online[i]) continue;
        cJSON *o = eb_obj(p->online ? "presence:online" : "presence:offline");
        cJSON_AddNumberToObject(o, "id", (double) p->id);
        eb_push(p->friends[i], o);
    }
    free(p->friends);
    free(p);
}

static void presence_friends_done(kn_db_result *res, void *ud) {
    PresenceCtx *p = ud;
    const int rows = kn_db_ok(res) ? kn_db_rows(res) : 0;
    p->friends = calloc(rows > 0 ? (size_t) rows : 1, sizeof *p->friends);
    if (!p->friends) {
        free(p);
        return;
    }
    for (int r = 0; r < rows; r++) p->friends[p->n++] = kn_db_get_int64(res, r, 0, 0);
    eb_online(p->friends, p->n, presence_push, p);
}

static void presence_announce(int64_t account_id, int online) {
    PresenceCtx *p = calloc(1, sizeof *p);
    if (!p) return;
    p->id = account_id;
    p->online = online;
    char id[24];
    if (kn_db_query_sql(g_acct_brain->db, SQL(.any = FRIENDS_OF_1), KN_DB_ARGS(acct_itoa(id, account_id)), presence_friends_done, p) != 0)
        free(p);
}

static void offline_checked(void *ud, const int *online, size_t n) {
    int64_t *id = ud;
    if (!(n > 0 && online[0])) presence_announce(*id, 0); /* not signed in on another brain either */
    free(id);
}

/* The account's first socket here signed in (online = 1) or its last one closed (social_ws.c). */
void social_eb_presence(int64_t account_id, int online) {
    if (account_id <= 0 || !g_acct_brain || !g_acct_brain->db) return;
    if (online) {
        presence_announce(account_id, 1);
        return;
    }
    int64_t *id = malloc(sizeof *id);
    if (!id) return;
    *id = account_id;
    eb_online(id, 1, offline_checked, id);
}

/* ------------------------------------------------------------------ follows (profiles.c) */

static void follow_checked(kn_db_result *res, void *ud) {
    int64_t *a = ud;
    if (kn_db_ok(res) && kn_db_rows(res) > 0) {
        const int64_t b = kn_db_get_int64(res, 0, 0, 0);
        const int a_follows = kn_db_get_int64(res, 0, 1, 0) != 0, b_follows = kn_db_get_int64(res, 0, 2, 0) != 0;
        const char *a_name = kn_db_get(res, 0, 3);
        if (b_follows) { /* the other side follows back: a friendship was made or broken */
            refresh_list_of(*a);
            refresh_list_of(b);
        }
        if (a_follows && b > 0 && b != *a) {
            /* KrunkNative (the v10 client's notification center, v10_notify.c): a follow that is
             * not followed back is a friend request; one that is, its acceptance */
            cJSON *o = eb_obj(b_follows ? "friend:accepted" : "friend:request");
            cJSON_AddNumberToObject(o, "from", (double) *a);
            cJSON_AddStringToObject(o, "name", a_name ? a_name : "");
            eb_push(b, o);
        }
    }
    free(a);
}

/* `a` followed / unfollowed the player `name` (profiles.c "f", social_gapi.c follow): when that
 * player follows `a`, the follow made or broke a friendship, so both sides get their list again.
 * A new follow also tells that player: "friend:request" {from, name} (not followed back yet) or
 * "friend:accepted" {from, name} (`a` followed back), when online. */
void social_eb_follow_changed(int64_t a, const char *name) {
    if (a <= 0 || !name || !*name || !g_acct_brain || !g_acct_brain->db) return;
    int64_t *x = malloc(sizeof *x);
    if (!x) return;
    *x = a;
    char ids[24];
    if (kn_db_query_sql(g_acct_brain->db,
                        SQL(.any = "SELECT b.id AS id__i, "
                                   "CASE WHEN EXISTS (SELECT 1 FROM follows WHERE follower_id = $1 AND followee_id = b.id) THEN 1 ELSE 0 END AS ab__i, "
                                   "CASE WHEN EXISTS (SELECT 1 FROM follows WHERE follower_id = b.id AND followee_id = $1) THEN 1 ELSE 0 END AS ba__i, "
                                   "(SELECT username FROM accounts WHERE id = $1) AS an "
                                   "FROM accounts b WHERE lower(b.username) = lower($2)"),
                        KN_DB_ARGS(acct_itoa(ids, a), name), follow_checked, x) != 0)
        free(x);
}

/* ------------------------------------------------------------------ dispatch */

/* ["eb", {type, ...}] */
void sh_eb(SocialSession *s, const cJSON *f) {
    const cJSON *o = fa(f, 1);
    const char *type = kn_json_str(o, "type", "");
    s->json = 1; /* the event bus is JSON only */
    if (!strcmp(type, "presence:ping")) {
        eb_send(s, eb_obj("presence:pong"));
        return;
    }
    if (s->account_id <= 0) {
        eb_error(s, "EB_UNAUTHORIZED", "Not signed in.");
        return;
    }
    if (!strcmp(type, "friends:list")) friends_list(s, obj_int(o, "offset", 0));
    else if (!strcmp(type, "msg:send")) msg_send(s, o);
    else if (!strcmp(type, "msg:history")) msg_history(s, o);
    else if (!strcmp(type, "msg:read")) msg_read(s, o);
    else if (!strcmp(type, "game:invite")) game_invite(s, o);
    else KN_DEBUG(MOD, "%s: unknown event-bus type \"%s\"", s->socket_id, type);
}
