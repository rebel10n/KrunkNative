/*
 * services/brain/social_data.c - feed, chat, clans, settings sync, display
 * name / password and mail of the social socket (KR gifts, the market and
 * trades are economy.c)
 * (Krunker v3.1.1 src/social.js + the menu account/clan/mail windows of
 * src/index.js; offline server matchmaker/socialWs.ts + server/social.ts).
 * Multi-statement operations are kn_procs (db_procs.h, db_social.c): the SQL
 * functions of postgres/0104_social_functions.sql or their C versions.
 */
#include "acct_internal.h"
#include "game_presence.h" /* game_presence_status: "playing <map>" (the friends, the chat header) */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "knsvc/config.h"
#include "knsvc/crypto.h"
#include "knsvc/json.h"
#include "knsvc/log.h"

#define MOD "social"

/* ------------------------------------------------------------------ helpers */

typedef struct Ctx {
    char label[16];
    int64_t a, b;
    char text[128];
} Ctx;

static Ctx *ctx_new(const char *label, int64_t a, int64_t b) {
    Ctx *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    snprintf(c->label, sizeof c->label, "%s", label ? label : "");
    c->a = a;
    c->b = b;
    return c;
}

static const char *me_str(const SocialSession *s, char buf[24]) { return social_viewer(s, buf); }

/* a message the box can hold: non-empty, at most max characters (JS length), not max bytes */
static int text_ok(const char *t, int max) {
    return t && *t && max > 0 && brain_utf16_prefix(t, (size_t) max) == strlen(t);
}

/* Error (DB refusal or failure) as an io_po error popup. */
static void popup_error_done(SocialSession *s, kn_db_result *res, void *extra) {
    Ctx *c = extra;
    if (!kn_db_ok(res)) social_error(s, acct_db_error(res, c ? c->label : "social"));
}


/* ------------------------------------------------------------------ feed */

static void post_done(SocialSession *s, kn_db_result *res, void *extra) {
    Ctx *c = extra;
    const int comment = c->a == 2;
    if (!kn_db_ok(res) || kn_db_affected(res) < 1) {
        const char *err = !kn_db_ok(res) ? acct_db_error(res, "post") : (comment ? "Post not found." : "Slow down");
        if (comment) social_send_n(s, "psr", 3, cJSON_CreateNull(), cJSON_CreateString(err), cJSON_CreateNumber(2));
        else social_send_n(s, "psr", 2, cJSON_CreateNull(), cJSON_CreateString(err));
        return;
    }
    if (comment) social_send_n(s, "psr", 3, cJSON_CreateNumber(1), cJSON_CreateNull(), cJSON_CreateNumber(2));
    else social_send_n(s, "psr", 1, cJSON_CreateNumber(1));
}

/* the hub Post button carries the trimmed text until the level check comes back */
typedef struct HubPost {
    char text[4096];
    char l1[256], l2[256];
    int type;
} HubPost;

/* level 10, the same gate as the compose box (deob-10.0.7.js errors.social.levelTooLow) */
static void post_scored(SocialSession *s, kn_db_result *res, void *extra) {
    HubPost *p = extra;
    if (!kn_db_ok(res)) {
        social_send_n(s, "psr", 2, cJSON_CreateNull(), cJSON_CreateString(acct_db_error(res, "post")));
        return;
    }
    const int64_t score = kn_db_rows(res) > 0 ? kn_db_get_int64(res, 0, 0, 0) : 0;
    if (db_level(score, 0.03) < 10) {
        social_send_n(s, "psr", 2, cJSON_CreateNull(), cJSON_CreateString("You must be level 10 or higher to post."));
        return;
    }
    char me[24], typ[24];
    /* at most 20 posts an hour */
    social_query(s,
                 SQL(.pg = "INSERT INTO feed_posts (author_id, body, type, link, link2) SELECT $1, $2, $3, nullif($4, ''), nullif($5, '') "
                           "WHERE (SELECT count(*) FROM feed_posts WHERE author_id = $1 AND created_at > now() - interval '1 hour') < 20",
                     .any = "INSERT INTO feed_posts (author_id, body, type, link, link2) SELECT $1, $2, $3, nullif($4, ''), nullif($5, '') "
                            "FROM accounts WHERE id = $1 "
                            "AND (SELECT count(*) FROM feed_posts WHERE author_id = $1 AND created_at > {ago:3600}) < 20"),
                 KN_DB_ARGS(me_str(s, me), p->text, acct_itoa(typ, p->type), p->l1, p->l2), post_done, ctx_new("ps", 0, 0));
}

/* ["ps", {txt, typ, lnk, lnk2}] -> ["psr", 1] | ["psr", null, err]
 * #postInput has no maxlength. The hub Post button is the compose box (deob-10.0.7.js:141204, :141216):
 * trim, then at most 500 characters, never a cut in the middle of a letter. Level 10, as that box. */
void sh_post(SocialSession *s, const cJSON *f) {
    if (s->account_id <= 0) {
        social_send_n(s, "psr", 2, cJSON_CreateNull(), cJSON_CreateString("Login required."));
        return;
    }
    const cJSON *body = fa(f, 1);
    const char *raw = kn_json_str(body, "txt", NULL);
    while (raw && *raw && isspace((unsigned char) *raw)) raw++;
    size_t n = raw ? strlen(raw) : 0;
    while (n && isspace((unsigned char) raw[n - 1])) n--;
    char trimmed[4096];
    if (n >= sizeof trimmed) n = sizeof trimmed - 1;
    if (raw && n) memcpy(trimmed, raw, n);
    trimmed[n] = 0;
    if (!trimmed[0]) {
        social_send_n(s, "psr", 2, cJSON_CreateNull(), cJSON_CreateString("Empty post."));
        return;
    }
    int max = g_acct.post_max_len > 0 ? g_acct.post_max_len : 500;
    if (max > 500) max = 500;
    const size_t cut = brain_utf16_prefix(trimmed, (size_t) max);
    HubPost *p = calloc(1, sizeof *p);
    if (!p) {
        social_send_n(s, "psr", 2, cJSON_CreateNull(), cJSON_CreateString("Server unavailable"));
        return;
    }
    memcpy(p->text, trimmed, cut);
    p->text[cut] = 0;
    const int64_t type = kn_json_int(body, "typ", 0);
    p->type = type >= 0 && type < 16 ? (int) type : 0;
    const cJSON *lnk = cJSON_GetObjectItemCaseSensitive(body, "lnk"), *lnk2 = cJSON_GetObjectItemCaseSensitive(body, "lnk2");
    if (cJSON_IsString(lnk)) snprintf(p->l1, sizeof p->l1, "%s", lnk->valuestring);
    else if (cJSON_IsNumber(lnk)) snprintf(p->l1, sizeof p->l1, "%lld", (long long) lnk->valuedouble);
    if (cJSON_IsString(lnk2)) snprintf(p->l2, sizeof p->l2, "%s", lnk2->valuestring);
    char me[24];
    social_query(s, SQL(.any = "SELECT player_score FROM players WHERE player_id = $1"),
                 KN_DB_ARGS(me_str(s, me)), post_scored, p);
}

/* ["com", postId, {txt}] -> ["psr", 1, null, 2] | ["psr", null, err, 2] */
void sh_comment(SocialSession *s, const cJSON *f) {
    if (s->account_id <= 0) {
        social_send_n(s, "psr", 3, cJSON_CreateNull(), cJSON_CreateString("Login required."), cJSON_CreateNumber(2));
        return;
    }
    /* the reply box (deob-10.0.7.js:107297): trim, then the maxlength of 500 */
    const char *raw = kn_json_str(fa(f, 2), "txt", NULL);
    while (raw && *raw && isspace((unsigned char) *raw)) raw++;
    size_t n = raw ? strlen(raw) : 0;
    while (n && isspace((unsigned char) raw[n - 1])) n--;
    char trimmed[2048];
    if (n >= sizeof trimmed) n = sizeof trimmed - 1;
    if (raw && n) memcpy(trimmed, raw, n);
    trimmed[n] = 0;
    if (!trimmed[0]) {
        social_send_n(s, "psr", 3, cJSON_CreateNull(), cJSON_CreateString("Empty comment."), cJSON_CreateNumber(2));
        return;
    }
    char text[2048], me[24], pid[24];
    int max = g_acct.comment_max_len > 0 && g_acct.comment_max_len < 2000 ? g_acct.comment_max_len : 500;
    if (max > 500) max = 500;
    /* the cap counts UTF-16 units, as the reply box's maxlength does, never a cut code point */
    const size_t cut = brain_utf16_prefix(trimmed, (size_t) max);
    snprintf(text, sizeof text, "%.*s", (int) cut, trimmed);
    social_query(s,
                 SQL(.any = "INSERT INTO feed_comments (post_id, author_id, body) SELECT id, $1, $3 FROM feed_posts "
                            "WHERE id = $2 AND deleted_at IS NULL"),
                 KN_DB_ARGS(me_str(s, me), acct_itoa(pid, fa_int(f, 1, 0)), text), post_done, ctx_new("com", 2, 0));
}

static void get_post_done(SocialSession *s, kn_db_result *res, void *extra) {
    (void) extra;
    cJSON *v = kn_db_ok(res) ? acct_db_json(res) : NULL;
    if (!v) {
        social_send_n(s, "psr", 3, cJSON_CreateNull(),
                      cJSON_CreateString(kn_db_ok(res) ? "Post not found." : acct_db_error(res, "gps")), cJSON_CreateNumber(1));
        return;
    }
    social_send_n(s, "psr", 3, v, cJSON_CreateNull(), cJSON_CreateNumber(1));
}

/* ["gps", postId] -> ["psr", {p, c}, null, 1] */
void sh_get_post(SocialSession *s, const cJSON *f) {
    char me[24], pid[24];
    social_call(s, &P_POST_FULL, KN_DB_ARGS(acct_itoa(pid, fa_int(f, 1, 0)), me_str(s, me)),
                 get_post_done, NULL);
}

/* ["lps", postId, 1 like | 0 unlike] (no reply) */
void sh_like_post(SocialSession *s, const cJSON *f) {
    if (s->account_id <= 0) return;
    char me[24], pid[24];
    if (fa_int(f, 2, 1)) {
        social_query(s,
                     SQL(.pg = "INSERT INTO feed_likes (post_id, account_id) SELECT id, $1 FROM feed_posts WHERE id = $2 "
                               "AND deleted_at IS NULL ON CONFLICT DO NOTHING",
                         .any = "{insert_ignore} INTO feed_likes (post_id, account_id) SELECT id, $1 FROM feed_posts WHERE id = $2 "
                                "AND deleted_at IS NULL {ignore_conflicts}"),
                     KN_DB_ARGS(me_str(s, me), acct_itoa(pid, fa_int(f, 1, 0))), popup_error_done, NULL);
    } else {
        social_query(s, SQL(.any = "DELETE FROM feed_likes WHERE account_id = $1 AND post_id = $2"),
                     KN_DB_ARGS(me_str(s, me), acct_itoa(pid, fa_int(f, 1, 0))), popup_error_done, NULL);
    }
}

/* ["dps", postId] own post (or staff) (no reply) */
void sh_delete_post(SocialSession *s, const cJSON *f) {
    if (s->account_id <= 0) return;
    char me[24], pid[24];
    social_query(s,
                 SQL(.pg = "UPDATE feed_posts SET deleted_at = now() WHERE id = $2 AND deleted_at IS NULL AND (author_id = $1 OR "
                           "EXISTS (SELECT 1 FROM players WHERE player_id = $1 AND (player_moderator > 0 OR player_developer > 0)))",
                     .any = "UPDATE feed_posts SET deleted_at = {now} WHERE id = $2 AND deleted_at IS NULL AND (author_id = $1 OR "
                            "EXISTS (SELECT 1 FROM players WHERE player_id = $1 AND (player_moderator > 0 OR player_developer > 0)))"),
                 KN_DB_ARGS(me_str(s, me), acct_itoa(pid, fa_int(f, 1, 0))), popup_error_done, NULL);
}

/* ------------------------------------------------------------------ chat */

static void rows_done(SocialSession *s, kn_db_result *res, void *extra) {
    Ctx *c = extra;
    cJSON *rows = kn_db_ok(res) ? kn_db_result_to_json(res) : NULL;
    if (!kn_db_ok(res)) acct_db_error(res, c->label);
    social_send_n(s, c->label, 1, rows ? rows : cJSON_CreateArray());
}

/* ["chtb"] -> ["chtb", [{fid, fna, cht, st, sm, sub, plf, gid}]]: the hub's chat sidebar and Friends popup are the
 * viewer's friends (mutual follows, as social_eb.c's friends:list), hub-10.0.7.js:2203-2207 (the request), 2321-2346
 * (the bubbles: online first, a status dot, the unread count, startChat with the status text and game) and 5324-5343
 * (showFriends: premium mark and picture, verified mark, Join while in a game, Remove Friend). cht = unread from that
 * friend, st = online (any brain), sm = "playing <map>" and gid = the game id while the friend is in a game (game_presence),
 * sub = premium, plf = verified.
 * The same rows are the game page's FRIENDS_LIST_RESPONSE ["gfrnd", rows] (deob-10.0.7.js:98673, 225186-225194: sorted
 * by st, windows[4].friendsLoaded), which v10's server pushes after the sign-in for the profile window's Friends tab
 * (windows[4] tab 1, :219085-219101: st, sm, sub / fid's picture, plf, fna, gid). kn_brain pushes it after the sign-in
 * (social_ws.c) and again to both sides when a follow makes or breaks a friendship (social_eb.c). */
typedef struct ChatFriends {
    SocialSession *s; /* held; NULL: pushed to the account's sockets */
    int64_t account;
    char label[8];
    cJSON *rows;
} ChatFriends;

#define CHAT_FRIENDS_SQL                                                                                               \
    "SELECT a.id, a.username, "                                                                                        \
    "(SELECT count(*) FROM chat_messages m WHERE m.sender_id = a.id AND m.receiver_id = $1 AND m.read_at IS NULL), "   \
    "coalesce(p.player_premium, 0), coalesce(p.player_featured, 0) "                                                   \
    "FROM follows f1 JOIN follows f2 ON f2.follower_id = f1.followee_id AND f2.followee_id = f1.follower_id "         \
    "JOIN accounts a ON a.id = f1.followee_id LEFT JOIN players p ON p.player_id = a.id "                              \
    "WHERE f1.follower_id = $1 ORDER BY lower(a.username) LIMIT 200"

static void chat_friends_online(void *ud, const int *online, size_t n) {
    ChatFriends *w = ud;
    size_t i = 0;
    cJSON *r;
    cJSON_ArrayForEach(r, w->rows) {
        const int on = i < n && online[i];
        i++;
        cJSON_AddNumberToObject(r, "st", on);
        char game[96] = "", sm[96] = "";
        if (on && game_presence_status((int64_t) cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(r, "fid")), sm, sizeof sm, game, sizeof game) == 1
            && game[0]) {
            if (sm[0]) cJSON_AddStringToObject(r, "sm", sm);
            cJSON_AddStringToObject(r, "gid", game);
        }
    }
    if (w->s) {
        social_send_n(w->s, w->label, 1, w->rows);
        social_unhold(w->s);
    } else {
        cJSON *f = social_frame(w->label);
        cJSON_AddItemToArray(f, w->rows);
        social_push_account_ex(w->account, f, 1);
    }
    free(w);
}

/* the rows of the query, then their presence; w is consumed */
static void chat_friends_rows(ChatFriends *w, kn_db_result *res) {
    const int n = kn_db_rows(res);
    int64_t *ids = calloc(n > 0 ? (size_t) n : 1, sizeof *ids);
    if (!ids) {
        if (w->s) social_unhold(w->s);
        free(w);
        return;
    }
    w->rows = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        ids[i] = kn_db_get_int64(res, i, 0, 0);
        cJSON_AddNumberToObject(o, "fid", (double) ids[i]);
        cJSON_AddStringToObject(o, "fna", kn_db_get(res, i, 1) ? kn_db_get(res, i, 1) : "");
        cJSON_AddNumberToObject(o, "cht", (double) kn_db_get_int64(res, i, 2, 0));
        cJSON_AddNumberToObject(o, "sub", kn_db_get_int64(res, i, 3, 0) > 0);
        cJSON_AddNumberToObject(o, "plf", kn_db_get_int64(res, i, 4, 0) > 0);
        cJSON_AddItemToArray(w->rows, o);
    }
    social_online_lookup(ids, (size_t) n, chat_friends_online, w);
    free(ids);
}

static void chat_friends_done(SocialSession *s, kn_db_result *res, void *extra) {
    const char *label = extra ? (const char *) extra : "chtb";
    if (!kn_db_ok(res)) {
        acct_db_error(res, label);
        social_send_n(s, label, 1, cJSON_CreateArray());
        return;
    }
    ChatFriends *w = calloc(1, sizeof *w);
    if (!w) return;
    snprintf(w->label, sizeof w->label, "%s", label);
    w->s = social_hold(s);
    chat_friends_rows(w, res);
}

static void chat_friends_query(SocialSession *s, const char *label) {
    if (s->account_id <= 0) return;
    char me[24];
    char *l = strdup(label);
    if (!l) return;
    social_query(s, SQL(.any = CHAT_FRIENDS_SQL), KN_DB_ARGS(me_str(s, me)), chat_friends_done, l);
}

void sh_chat_blobs(SocialSession *s, const cJSON *f) {
    (void) f;
    chat_friends_query(s, "chtb");
}

/* ["gfrnd", rows] after the sign-in (JSON clients: the v3.1.1 page has no such label) */
void social_push_friends(SocialSession *s) {
    if (s->json) chat_friends_query(s, "gfrnd");
}

static void friends_account_done(kn_db_result *res, void *ud) {
    ChatFriends *w = ud;
    if (!kn_db_ok(res)) {
        acct_db_error(res, "gfrnd");
        free(w);
        return;
    }
    chat_friends_rows(w, res);
}

/* ["gfrnd", rows] to every JSON socket of the account (its friends changed) */
void social_push_friends_of(int64_t account_id) {
    if (account_id <= 0 || !g_acct_brain || !g_acct_brain->db) return;
    ChatFriends *w = calloc(1, sizeof *w);
    if (!w) return;
    w->account = account_id;
    snprintf(w->label, sizeof w->label, "gfrnd");
    char me[24];
    if (kn_db_query_sql(g_acct_brain->db, SQL(.any = CHAT_FRIENDS_SQL), KN_DB_ARGS(acct_itoa(me, account_id)), friends_account_done, w) != 0)
        free(w);
}

/* ["chs", {id, st, sm}]: the chat peer's presence for the header (v10: Online / Offline, or the
 * "playing <map>" status text), sent with the thread on "ctd" */
typedef struct ChatStatus {
    SocialSession *s; /* held */
    int64_t id;
} ChatStatus;

static void chat_status_done(void *ud, const int *online, size_t n) {
    ChatStatus *w = ud;
    const int on = n > 0 && online[0];
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "id", (double) w->id);
    cJSON_AddBoolToObject(o, "st", on);
    char sm[96] = "";
    if (on && game_presence_status(w->id, sm, sizeof sm, NULL, 0) == 1 && sm[0]) cJSON_AddStringToObject(o, "sm", sm);
    social_send_n(w->s, "chs", 1, o);
    social_unhold(w->s);
    free(w);
}

/* ["ctd", peerId] -> ["chtl", [{fid, fna, txt, tim}]] newest first; marks them read; ["chs", ...] */
void sh_chat_thread(SocialSession *s, const cJSON *f) {
    if (s->account_id <= 0) return;
    char me[24], peer[24];
    const int64_t peer_id = fa_int(f, 1, 0);
    if (peer_id <= 0) {
        social_send_n(s, "chtl", 1, cJSON_CreateArray());
        return;
    }
    social_call(s, &P_CHAT_THREAD,
                 KN_DB_ARGS(me_str(s, me), acct_itoa(peer, peer_id)), rows_done, ctx_new("chtl", 0, 0));
    ChatStatus *w = calloc(1, sizeof *w);
    if (w) {
        w->s = social_hold(s);
        w->id = peer_id;
        social_online_lookup(&peer_id, 1, chat_status_done, w);
    }
}

typedef struct ChatSend {
    int64_t a;    /* the recipient */
    char text[2048];
} ChatSend;

static void chat_sent(SocialSession *s, kn_db_result *res, void *extra) {
    ChatSend *c = extra;
    if (!kn_db_ok(res) || kn_db_affected(res) < 1) {
        social_send_n(s, "chf", 2, cJSON_CreateFalse(),
                      cJSON_CreateString(kn_db_ok(res) ? "Message not Sent" : acct_db_error(res, "ict")));
        return;
    }
    social_send_n(s, "chf", 1, cJSON_CreateTrue());
    /* the recipient's open hub pages: ["msg", fromId, text] (hub-10.0.7.js:6932-6950: the open thread gets the line,
     * else the sender's bubble counts one more unread) */
    cJSON *push = social_frame("msg");
    cJSON_AddItemToArray(push, cJSON_CreateNumber((double) s->account_id));
    cJSON_AddItemToArray(push, cJSON_CreateString(c->text));
    social_push_account(c->a, push);
}

/* ["ict", peerId, text] -> ["chf", true] | ["chf", false, err] */
void sh_chat_send(SocialSession *s, const cJSON *f) {
    if (s->account_id <= 0) return;
    /* the same trim as a direct message (msg:send): a line of spaces is not a message */
    const char *raw = fa_str(f, 2);
    while (raw && (*raw == ' ' || *raw == '\n' || *raw == '\t' || *raw == '\r')) raw++;
    size_t n = raw ? strlen(raw) : 0;
    while (n && (raw[n - 1] == ' ' || raw[n - 1] == '\n' || raw[n - 1] == '\t' || raw[n - 1] == '\r')) n--;
    char text[2048];
    if (n >= sizeof text) n = sizeof text - 1;
    if (raw && n) memcpy(text, raw, n);
    text[n] = 0;
    const int64_t peer = fa_int(f, 1, 0);
    if (!text_ok(text, g_acct.chat_max_len > 0 ? g_acct.chat_max_len : 512) || peer <= 0 || peer == s->account_id) {
        social_send_n(s, "chf", 2, cJSON_CreateFalse(), cJSON_CreateString("Message not Sent"));
        return;
    }
    char me[24], p[24];
    ChatSend *c = calloc(1, sizeof *c);
    if (!c) return;
    c->a = peer;
    snprintf(c->text, sizeof c->text, "%s", text);
    social_query(s,
                 /* a staff chat ban (feature_bans 'chat', in force) refuses the line: no row, chf false */
                 SQL(.pg = "INSERT INTO chat_messages (sender_id, receiver_id, body) SELECT $1, id, $3 FROM accounts WHERE id = $2 "
                           "AND (SELECT count(*) FROM chat_messages WHERE sender_id = $1 AND created_at > now() - interval '1 minute') < 30 "
                           "AND NOT EXISTS (SELECT 1 FROM feature_bans WHERE account_id = $1 AND feature = 'chat' AND revoked_at IS NULL "
                           "AND (expires_at IS NULL OR expires_at > now())) "
                           "RETURNING id",
                     .any = "INSERT INTO chat_messages (sender_id, receiver_id, body) SELECT $1, id, $3 FROM accounts WHERE id = $2 "
                            "AND (SELECT count(*) FROM chat_messages WHERE sender_id = $1 AND created_at > {ago:60}) < 30 "
                            "AND NOT EXISTS (SELECT 1 FROM feature_bans WHERE account_id = $1 AND feature = 'chat' AND revoked_at IS NULL "
                            "AND (expires_at IS NULL OR expires_at > {now}))"),
                 KN_DB_ARGS(me_str(s, me), acct_itoa(p, peer), c->text), chat_sent, c);
}

/* ------------------------------------------------------------------ clans */

/* menu reply: ["cln", msg, data, ok, action] (index.js windows[12].resp) */
static void clan_menu_reply(SocialSession *s, const char *msg, cJSON *data, int ok, int64_t action) {
    social_send_n(s, "cln", 4, msg ? cJSON_CreateString(msg) : cJSON_CreateNull(), data ? data : cJSON_CreateNull(),
                  cJSON_CreateNumber(ok), cJSON_CreateNumber((double) action));
}

static void clan_menu_done(SocialSession *s, kn_db_result *res, void *extra) {
    Ctx *c = extra;
    if (!kn_db_ok(res)) {
        clan_menu_reply(s, acct_db_error(res, "cln"), NULL, 0, c->a);
        return;
    }
    cJSON *v = acct_db_json(res);
    if (c->a == 4) { /* leave / delete */
        cJSON_Delete(v);
        social_send_n(s, "clnL", 0);
        return;
    }
    if (c->a == 2) {
        cJSON_Delete(v);
        clan_menu_reply(s, "Request Sent", NULL, 1, c->a);
        return;
    }
    clan_menu_reply(s, NULL, v, v ? 1 : 0, c->a);
}

static void clan_social_done(SocialSession *s, kn_db_result *res, void *extra) {
    (void) extra;
    /* clanRSr(msg, ok) */
    if (!kn_db_ok(res)) social_send_n(s, "cln", 2, cJSON_CreateString(acct_db_error(res, "cln")), cJSON_CreateFalse());
    else social_send_n(s, "cln", 2, cJSON_CreateString("Request Sent"), cJSON_CreateTrue());
}

/* v10 updateDis: /discord(?:\.com|app\.com|\.gg)\/(?:invite\/)?([a-zA-Z0-9\-]{2,32})/i -> the code */
static int discord_code(const char *v, char *out, size_t n) {
    if (!v || n < 33) return 0;
    for (const char *p = v; *p; p++) {
        if (strncasecmp(p, "discord", 7)) continue;
        const char *q = p + 7;
        if (!strncasecmp(q, ".com/", 5)) q += 5;
        else if (!strncasecmp(q, "app.com/", 8)) q += 8;
        else if (!strncasecmp(q, ".gg/", 4)) q += 4;
        else continue;
        if (!strncasecmp(q, "invite/", 7)) q += 7;
        size_t len = 0;
        while (len < 32 && (isalnum((unsigned char) q[len]) || q[len] == '-')) len++;
        if (len < 2) continue;
        memcpy(out, q, len);
        out[len] = 0;
        return 1;
    }
    return 0;
}

/* Social page: ["cln", tag] join request -> ["cln", msg, ok].
 * Menu clan window: ["cln", action, value, extra] -> ["cln", msg, data, ok, action]:
 *   1 create(tag) 2 request(tag) 3 my clan 4 leave 5 kick(id) 6 role(id,{r}) 7 funds(kr,{wd}) 8 contract
 *   9 Discord link (v10: the invite URL; the brain keeps its code, clanData.clan_link4) */
void sh_clan(SocialSession *s, const cJSON *f) {
    char me[24], v1[24], v2[24], v3[24];
    if (cJSON_IsString(fa(f, 1))) {
        if (!social_require_auth(s)) return;
        social_call(s, &P_CLAN_REQUEST, KN_DB_ARGS(me_str(s, me), fa_str(f, 1)), clan_social_done, NULL);
        return;
    }
    const int64_t action = fa_int(f, 1, 0);
    if (s->account_id <= 0) {
        clan_menu_reply(s, "Must be logged in", NULL, 0, action);
        return;
    }
    me_str(s, me);
    Ctx *c = ctx_new("cln", action, 0);
    switch (action) {
    case 1:
        social_call(s, &P_CLAN_CREATE,
                     KN_DB_ARGS(me, fa_str(f, 2) ? fa_str(f, 2) : "", acct_itoa(v1, g_acct.clan_tag_max)), clan_menu_done, c);
        return;
    case 2:
        social_call(s, &P_CLAN_REQUEST, KN_DB_ARGS(me, fa_str(f, 2) ? fa_str(f, 2) : ""),
                     clan_menu_done, c);
        return;
    case 3:
        social_call(s, &P_CLAN_DATA_OF, KN_DB_ARGS(me), clan_menu_done, c);
        return;
    case 4:
        social_call(s, &P_CLAN_LEAVE, KN_DB_ARGS(me), clan_menu_done, c);
        return;
    case 5:
        social_call(s, &P_CLAN_KICK, KN_DB_ARGS(me, acct_itoa(v1, fa_int(f, 2, 0))), clan_menu_done, c);
        return;
    case 6:
        social_call(s, &P_CLAN_ROLE,
                     KN_DB_ARGS(me, acct_itoa(v1, fa_int(f, 2, 0)), acct_itoa(v2, kn_json_int(fa(f, 3), "r", 0))),
                     clan_menu_done, c);
        return;
    case 7:
        social_call(s, &P_CLAN_FUNDS,
                     KN_DB_ARGS(me, acct_itoa(v1, fa_int(f, 2, 0)), kn_json_int(fa(f, 3), "wd", 0) ? "true" : "false"),
                     clan_menu_done, c);
        return;
    case 9: { /* v10 updateDis: the Discord invite (owner) */
        char code[40];
        if (!discord_code(fa_str(f, 2), code, sizeof code)) {
            free(c);
            clan_menu_reply(s, "Invalid Discord", NULL, 0, action);
            return;
        }
        social_call(s, &P_CLAN_DISCORD, KN_DB_ARGS(me, code), clan_menu_done, c);
        return;
    }
    default:
        free(c);
        clan_menu_reply(s, "Coming Soon", NULL, 0, action);
        (void) v3;
        return;
    }
}

static void clan_decline_all_done(SocialSession *s, kn_db_result *res, void *extra) {
    (void) extra;
    if (!kn_db_ok(res)) clan_menu_reply(s, acct_db_error(res, "clnRA"), NULL, 0, 0);
}

/* ["clnRA"] v10 / v7 windows[12].decAll(): decline every join request (owner, Captains); no reply */
void sh_clan_decline_all(SocialSession *s, const cJSON *f) {
    (void) f;
    if (s->account_id <= 0) return;
    char me[24];
    social_call(s, &P_CLAN_DECLINE_ALL, KN_DB_ARGS(me_str(s, me)), clan_decline_all_done, NULL);
}

/* ["vcln"] v10 OPEN_CLAN_TAB: sent on every clan window render; v10's server reaction is not in
 * the client bundle (doc/v10/clans.md), so the brain accepts it without a reply */
void sh_clan_viewed(SocialSession *s, const cJSON *f) {
    (void) s;
    (void) f;
}

/* ["clnR", playerId, 1 accept | 0 decline] (menu); errors -> ["cln", msg, null, 0, 0] */
void sh_clan_request(SocialSession *s, const cJSON *f) {
    if (s->account_id <= 0) return;
    char me[24], who[24], max[24];
    social_call(s, &P_CLAN_ANSWER,
                 KN_DB_ARGS(me_str(s, me), acct_itoa(who, fa_int(f, 1, 0)), fa_int(f, 2, 0) == 1 ? "true" : "false",
                            acct_itoa(max, g_acct.clan_max_members)),
                 clan_menu_done, ctx_new("clnR", 9, 0));
}

/* ------------------------------------------------------------------ settings / name / password */

static void settings_up_done(SocialSession *s, kn_db_result *res, void *extra) {
    (void) extra;
    if (!kn_db_ok(res)) {
        social_send_n(s, "pRes", 2, cJSON_CreateNull(), cJSON_CreateString(acct_db_error(res, "uploadS")));
        return;
    }
    social_send_n(s, "pRes", 3, cJSON_CreateString("SUCCESS"), cJSON_CreateNull(), cJSON_CreateString("Settings Uploaded"));
}

static void titles_done(SocialSession *s, kn_db_result *res, void *extra) {
    (void) extra;
    cJSON *ids = cJSON_CreateArray();
    if (kn_db_ok(res))
        for (int i = 0; i < kn_db_rows(res); i++) cJSON_AddItemToArray(ids, cJSON_CreateNumber(atoi(kn_db_get(res, i, 0))));
    else acct_db_error(res, "ttl");
    social_send_n(s, "ttl", 1, ids);
}

/* ["ttl"] -> ["ttl", [id, ...]]: the account's titles, v10.0.3's acc.titles (setData slot 55,
 * deob-10.0.3.js:129024). Box Office grants titles with their Black Market items (player_titles,
 * 0361; db_economy.c run_blackm_buy), not yet for challenges or ranked: the client shows the others
 * locked with their requirement. A failed read answers an empty list. */
void sh_titles(SocialSession *s, const cJSON *f) {
    (void) f;
    if (!social_require_auth(s)) return;
    static const kn_sql q = {.any = "SELECT title_id FROM player_titles WHERE account_id = $1 ORDER BY title_id"};
    char me[24];
    social_query(s, &q, KN_DB_ARGS(acct_itoa(me, s->account_id)), titles_done, NULL);
}

/* ["uploadS", settings] -> ["pRes", "SUCCESS", null, msg] | ["pRes", null, err] (popupResponse) */
void sh_settings_up(SocialSession *s, const cJSON *f) {
    if (s->account_id <= 0) {
        social_send_n(s, "pRes", 2, cJSON_CreateNull(), cJSON_CreateString("Must be logged in"));
        return;
    }
    const cJSON *v = fa(f, 1);
    char *text = cJSON_IsString(v) ? strdup(v->valuestring) : kn_json_print(v);
    if (!text || strlen(text) > 200000) {
        free(text);
        social_send_n(s, "pRes", 2, cJSON_CreateNull(), cJSON_CreateString("Settings too large"));
        return;
    }
    char me[24];
    social_query(s, SQL(.pg = "UPDATE accounts SET settings = $2, settings_at = now() WHERE id = $1",
                        .any = "UPDATE accounts SET settings = $2, settings_at = {now} WHERE id = $1"),
                 KN_DB_ARGS(me_str(s, me), text), settings_up_done, NULL);
    free(text);
}

static void settings_get_done(SocialSession *s, kn_db_result *res, void *extra) {
    (void) extra;
    if (!kn_db_ok(res) || kn_db_rows(res) < 1 || kn_db_isnull(res, 0, 0)) {
        social_send_n(s, "pRes", 2, cJSON_CreateNull(), cJSON_CreateString("No Settings Found"));
        return;
    }
    social_send_n(s, "loadS", 1, cJSON_CreateString(kn_db_get(res, 0, 0)));
}

/* ["getS"] -> ["loadS", settings] */
void sh_settings_get(SocialSession *s, const cJSON *f) {
    (void) f;
    if (s->account_id <= 0) {
        social_send_n(s, "pRes", 2, cJSON_CreateNull(), cJSON_CreateString("Must be logged in"));
        return;
    }
    char me[24];
    social_query(s, SQL(.any = "SELECT settings FROM accounts WHERE id = $1"), KN_DB_ARGS(me_str(s, me)), settings_get_done, NULL);
}

typedef struct OpWait {
    SocialSession *s;
    int kind; /* 0 alias, 1 password */
} OpWait;

static void op_wait_done(void *ud, const char *err, const cJSON *data) {
    OpWait *w = ud;
    SocialSession *s = w->s;
    if (social_open(s)) {
        if (w->kind == 0) {
            if (err) social_send_n(s, "nmeR", 1, cJSON_CreateString(err));
            else social_send_n(s, "nmeR", 2, cJSON_CreateNull(), cJSON_CreateString(kn_json_str(data, "alias", "")));
        } else {
            if (err) {
                social_send_n(s, "upp", 1, cJSON_CreateString(err));
            } else {
                const char *token = kn_json_str(data, "token", "");
                snprintf(s->token, sizeof s->token, "%s", token);
                social_send_n(s, "upp", 2, cJSON_CreateString("Password updated."), cJSON_CreateString(token));
            }
        }
    }
    free(w);
    social_unhold(s);
}

/* ["nme", alias] -> ["nmeR", err] | ["nmeR", null, alias]. ["nme"] / ["nme", null] (v10.0.7's clearName and an empty
 * changeName, deob-10.0.7.js:223607-223620) clears the display name: the answer's alias is the username. */
void sh_alias(SocialSession *s, const cJSON *f) {
    if (s->account_id <= 0) {
        social_send_n(s, "nmeR", 1, cJSON_CreateString("Not logged in."));
        return;
    }
    OpWait *w = calloc(1, sizeof *w);
    if (!w) return;
    w->s = social_hold(s);
    w->kind = 0;
    const cJSON *a = fa(f, 1);
    if (!a || cJSON_IsNull(a)) acct_clear_alias(s->account_id, op_wait_done, w);
    else acct_set_alias(s->account_id, fa_str(f, 1), op_wait_done, w);
}

static int pw_chars(const char *s) {
    int n = 0;
    for (; s && *s; s++) n += ((unsigned char) *s & 0xC0) != 0x80;
    return n;
}

/* v10.0.3's Account Security: "You can update your password every 30 minutes" (deob-10.0.3.js:247680) */
#define PASSWORD_CHANGE_EVERY_MS (30 * 60 * 1000)

typedef struct PasswordReq {
    int has_old;              /* 0: the v10.0.3 shape (the session's proof) */
    char old_pass[401], new_pass[401]; /* password_max characters, each up to 4 bytes */
} PasswordReq;

static void password_clock_done(SocialSession *s, kn_db_result *res, void *extra) {
    PasswordReq *r = extra; /* social_query frees it after this */
    if (!kn_db_ok(res)) {
        social_send_n(s, "upp", 1, cJSON_CreateString(acct_db_error(res, "upp")));
    } else if (kn_db_rows(res) > 0 && !kn_db_isnull(res, 0, 0) && (int64_t) kn_wall_ms() - kn_db_get_int64(res, 0, 0, 0) < PASSWORD_CHANGE_EVERY_MS) {
        social_send_n(s, "upp", 1, cJSON_CreateString("You can update your password every 30 minutes."));
    } else {
        OpWait *w = calloc(1, sizeof *w);
        if (w) {
            w->s = social_hold(s);
            w->kind = 1;
            if (r->has_old) acct_change_password(s->account_id, r->old_pass, r->new_pass, op_wait_done, w);
            else acct_change_password_session(s->account_id, r->new_pass, op_wait_done, w);
        }
    }
    memset(r, 0, sizeof *r);
}

/* ["upp", new, repeat, token] (v10.0.3 changePass, deob-10.0.3.js:244645-244650: the new password, its repeat and the
 * FRVR refresh token; no current password) -> ["upp", msg, newToken?]. The token is the socket's own session token
 * (KrunkNative's refresh token: proof that this client signed in). ["upp", old, new, repeat] (v7.1.4, the hub page)
 * still checks the old password. Either changes it once per 30 minutes (accounts.password_changed_at, 0359). */
void sh_password(SocialSession *s, const cJSON *f) {
    if (s->account_id <= 0) {
        social_send_n(s, "upp", 1, cJSON_CreateString("Not logged in."));
        return;
    }
    const char *proof = fa_str(f, 3);
    const int v10 = proof && s->token[0] && strlen(proof) == strlen(s->token) && kn_ct_streq(proof, s->token);
    const char *nw = v10 ? fa_str(f, 1) : fa_str(f, 2), *rep = v10 ? fa_str(f, 2) : fa_str(f, 3);
    if (!nw || !rep || strcmp(nw, rep) != 0) {
        social_send_n(s, "upp", 1, cJSON_CreateString("Passwords do not match."));
        return;
    }
    if (!v10 && !fa_str(f, 1)) {
        social_send_n(s, "upp", 1, cJSON_CreateString("Incorrect current password."));
        return;
    }
    /* counted in characters, as acct_validate_password does, not in bytes */
    if (pw_chars(nw) > g_acct.pass_max || strlen(nw) >= sizeof(((PasswordReq *) 0)->new_pass) ||
        (!v10 && fa_str(f, 1) && (pw_chars(fa_str(f, 1)) > g_acct.pass_max ||
                                  strlen(fa_str(f, 1)) >= sizeof(((PasswordReq *) 0)->old_pass)))) {
        social_send_n(s, "upp", 1, cJSON_CreateString("Password is too long."));
        return;
    }
    PasswordReq *r = calloc(1, sizeof *r);
    if (!r) return;
    r->has_old = !v10;
    snprintf(r->new_pass, sizeof r->new_pass, "%s", nw);
    if (!v10) snprintf(r->old_pass, sizeof r->old_pass, "%s", fa_str(f, 1));
    char me[24];
    social_query(s, SQL(.any = "SELECT {epoch_ms:password_changed_at} FROM accounts WHERE id = $1"), KN_DB_ARGS(me_str(s, me)),
                 password_clock_done, r);
}

/* ------------------------------------------------------------------ mail */

static void mail_list_done(SocialSession *s, kn_db_result *res, void *extra) {
    Ctx *c = extra;
    cJSON *rows = kn_db_ok(res) ? kn_db_result_to_json(res) : NULL;
    if (!kn_db_ok(res)) acct_db_error(res, "kml");
    social_send_n(s, "kml", 2, cJSON_CreateNumber((double) c->a), rows ? rows : cJSON_CreateArray());
}

/* v10's notification window (windows[29], v10.js _71355): a row shows in the tab of its km_notificationtype
 * (0 Followers, 1 Clan, 2 Hub, 3 Inbox; 4 and 5 count as Inbox) and in Archive once read (km_archive).
 * A Followers row (kind 0, written by social_eb.c for a follow not followed back) carries the follower's
 * name as km_link ("X has followed you!", Follow Back). KrunkNative's other mail (gifts, sales, trades) is Inbox mail
 * (kind 3); a giveaway's winner gets Hub mail (kind 2) whose km_type 5 links its "Post!" to the post (km_link, 0400). */
#define MAIL_COLS(html_l, html_r) \
    "SELECT id AS km_id, created_at AS km_datesent, " html_l "sender" html_r " AS km_playerfrom, " html_l "subject" html_r " AS km_subject, " \
    html_l "body" html_r " AS km_text, CASE WHEN read_at IS NULL THEN 0 ELSE 1 END AS km_archive, kr AS km_kr, " \
    "kind AS km_notificationtype__i, mail_type AS km_type__i, CASE WHEN kind = 0 THEN sender ELSE coalesce(link, '') END AS km_link FROM mail "

/* ["kml", archive, tabIndex] -> ["kml", tabIndex, rows]: windows[29] sends (0, or 1 for Archive, and its tabIndex)
 * (deob-10.0.7.js:221326, 221348) and onData(tabIndex, rows) takes the tab back (:221237-221246). Every row comes,
 * read or not: the window filters them (km_archive) and counts the unread per tab. A request without the tab (an
 * older client) gets its first value back. */
void sh_mail_list(SocialSession *s, const cJSON *f) {
    if (s->account_id <= 0) return;
    char me[24];
    const int64_t tab = cJSON_GetArraySize(f) > 2 ? fa_int(f, 2, 0) : fa_int(f, 1, 0) ? 1 : 0;
    social_query(s, SQL(.pg = MAIL_COLS("kn_html(", ")") "WHERE account_id = $1 ORDER BY id DESC LIMIT 100",
                        .any = MAIL_COLS("{html:", "}") "WHERE account_id = $1 ORDER BY id DESC LIMIT 100"),
                 KN_DB_ARGS(me_str(s, me)),
                 mail_list_done, ctx_new("kml", tab, 0));
}

static void mail_read_done(SocialSession *s, kn_db_result *res, void *extra) {
    const Ctx *c = extra;
    cJSON *rows = kn_db_ok(res) && kn_db_rows(res) > 0 ? kn_db_result_to_json(res) : NULL;
    if (!kn_db_ok(res)) acct_db_error(res, "rml");
    social_send_n(s, "rml", 2, rows ? rows : cJSON_CreateNull(), cJSON_CreateNumber(c ? (double) c->a : 3));
}

/* ["rml", id, type] -> ["rml", [mail], type] marks it read, credits attached KR once. The type is the row's
 * km_notificationtype as windows[29].readMail sends it (GET_MAIL, deob-10.0.7.js:221261-221264), echoed for
 * onReadData(rows, type) (:223757-223758, 221247-221259): no text goes into a Followers (0) row. */
void sh_mail_read(SocialSession *s, const cJSON *f) {
    if (s->account_id <= 0) return;
    char me[24], id[24];
    social_call(s, &P_MAIL_READ, KN_DB_ARGS(me_str(s, me), acct_itoa(id, fa_int(f, 1, 0))), mail_read_done,
                ctx_new("rml", fa_int(f, 2, 3), 0));
}

/* ["raml"] mark every mail read */
void sh_mail_read_all(SocialSession *s, const cJSON *f) {
    (void) f;
    if (s->account_id <= 0) return;
    char me[24];
    social_query(s, SQL(.pg = "UPDATE mail SET read_at = now() WHERE account_id = $1 AND read_at IS NULL",
                        .any = "UPDATE mail SET read_at = {now} WHERE account_id = $1 AND read_at IS NULL"),
                 KN_DB_ARGS(me_str(s, me)), popup_error_done, NULL);
}

static void mail_count_done(SocialSession *s, kn_db_result *res, void *extra) {
    (void) extra;
    if (kn_db_ok(res) && kn_db_rows(res) > 0) social_send_n(s, "gmc", 1, cJSON_CreateNumber((double) kn_db_get_int64(res, 0, 0, 0)));
}

static void push_count_done(kn_db_result *res, void *ud) {
    int64_t *id = ud;
    if (kn_db_ok(res) && kn_db_rows(res) > 0) {
        cJSON *f = social_frame("gmc");
        cJSON_AddItemToArray(f, cJSON_CreateNumber((double) kn_db_get_int64(res, 0, 0, 0)));
        social_push_account_ex(*id, f, 1);
    }
    free(id);
}

void social_push_mail_count(int64_t account_id) {
    if (account_id <= 0 || !g_acct_brain || !g_acct_brain->db) return;
    int64_t *id = malloc(sizeof *id);
    if (!id) return;
    *id = account_id;
    char ids[24];
    kn_db_query_sql(g_acct_brain->db, SQL(.any = "SELECT count(*) FROM mail WHERE account_id = $1 AND read_at IS NULL"),
                    KN_DB_ARGS(acct_itoa(ids, account_id)), push_count_done, id);
}

void social_after_login(SocialSession *s) {
    if (!s->json) return; /* "gmc" is a menu label: the v3.1.1 social page has no handler */
    char me[24];
    social_query(s, SQL(.any = "SELECT count(*) FROM mail WHERE account_id = $1 AND read_at IS NULL"), KN_DB_ARGS(me_str(s, me)),
                 mail_count_done, NULL);
}

