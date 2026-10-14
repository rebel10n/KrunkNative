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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "knsvc/config.h"
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

static int text_ok(const char *t, int max) { return t && *t && (int) strlen(t) <= max; }

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

/* ["ps", {txt, typ, lnk, lnk2}] -> ["psr", 1] | ["psr", null, err] */
void sh_post(SocialSession *s, const cJSON *f) {
    if (s->account_id <= 0) {
        social_send_n(s, "psr", 2, cJSON_CreateNull(), cJSON_CreateString("Login required."));
        return;
    }
    const cJSON *body = fa(f, 1);
    const char *txt = kn_json_str(body, "txt", NULL);
    if (!txt || !*txt) {
        social_send_n(s, "psr", 2, cJSON_CreateNull(), cJSON_CreateString("Empty post."));
        return;
    }
    char text[4096];
    snprintf(text, sizeof text, "%.*s", g_acct.post_max_len > 0 && g_acct.post_max_len < 4000 ? g_acct.post_max_len : 1024, txt);
    char me[24], typ[24];
    const int64_t type = kn_json_int(body, "typ", 0);
    Ctx *c = ctx_new("ps", 0, 0);
    const cJSON *lnk = cJSON_GetObjectItemCaseSensitive(body, "lnk"), *lnk2 = cJSON_GetObjectItemCaseSensitive(body, "lnk2");
    char l1[256] = "", l2[256] = "";
    if (cJSON_IsString(lnk)) snprintf(l1, sizeof l1, "%s", lnk->valuestring);
    else if (cJSON_IsNumber(lnk)) snprintf(l1, sizeof l1, "%lld", (long long) lnk->valuedouble);
    if (cJSON_IsString(lnk2)) snprintf(l2, sizeof l2, "%s", lnk2->valuestring);
    /* at most 20 posts an hour */
    social_query(s,
                 SQL(.pg = "INSERT INTO feed_posts (author_id, body, type, link, link2) SELECT $1, $2, $3, nullif($4, ''), nullif($5, '') "
                           "WHERE (SELECT count(*) FROM feed_posts WHERE author_id = $1 AND created_at > now() - interval '1 hour') < 20",
                     .any = "INSERT INTO feed_posts (author_id, body, type, link, link2) SELECT $1, $2, $3, nullif($4, ''), nullif($5, '') "
                            "FROM accounts WHERE id = $1 "
                            "AND (SELECT count(*) FROM feed_posts WHERE author_id = $1 AND created_at > {ago:3600}) < 20"),
                 KN_DB_ARGS(me_str(s, me), text, acct_itoa(typ, type >= 0 && type < 16 ? type : 0), l1, l2), post_done, c);
}

/* ["com", postId, {txt}] -> ["psr", 1, null, 2] | ["psr", null, err, 2] */
void sh_comment(SocialSession *s, const cJSON *f) {
    if (s->account_id <= 0) {
        social_send_n(s, "psr", 3, cJSON_CreateNull(), cJSON_CreateString("Login required."), cJSON_CreateNumber(2));
        return;
    }
    const char *txt = kn_json_str(fa(f, 2), "txt", NULL);
    if (!txt || !*txt) {
        social_send_n(s, "psr", 3, cJSON_CreateNull(), cJSON_CreateString("Empty comment."), cJSON_CreateNumber(2));
        return;
    }
    char text[2048], me[24], pid[24];
    snprintf(text, sizeof text, "%.*s", g_acct.comment_max_len > 0 && g_acct.comment_max_len < 2000 ? g_acct.comment_max_len : 500, txt);
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

/* ["chtb"] -> ["chtb", [{fid, fna, cht}]] conversations, newest first, cht = unread */
void sh_chat_blobs(SocialSession *s, const cJSON *f) {
    (void) f;
    if (s->account_id <= 0) return;
    char me[24];
    social_query(s,
                 SQL(.any = "SELECT peer AS fid, a.username AS fna, "
                            "(SELECT count(*) FROM chat_messages m WHERE m.sender_id = peer AND m.receiver_id = $1 AND m.read_at IS NULL) AS cht "
                            "FROM (SELECT CASE WHEN sender_id = $1 THEN receiver_id ELSE sender_id END AS peer, max(id) AS last "
                            "      FROM chat_messages WHERE sender_id = $1 OR receiver_id = $1 GROUP BY 1) t "
                            "JOIN accounts a ON a.id = t.peer ORDER BY t.last DESC LIMIT 50"),
                 KN_DB_ARGS(me_str(s, me)), rows_done, ctx_new("chtb", 0, 0));
}

/* ["ctd", peerId] -> ["chtl", [{fid, fna, txt, tim}]] newest first; marks them read */
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
}

static void chat_sent(SocialSession *s, kn_db_result *res, void *extra) {
    Ctx *c = extra;
    if (!kn_db_ok(res) || kn_db_affected(res) < 1) {
        social_send_n(s, "chf", 2, cJSON_CreateFalse(),
                      cJSON_CreateString(kn_db_ok(res) ? "Message not Sent" : acct_db_error(res, "ict")));
        return;
    }
    social_send_n(s, "chf", 1, cJSON_CreateTrue());
    /* live update for the recipient's open hub pages: refresh their conversation list */
    cJSON *push = social_frame("chtb");
    cJSON *rows = cJSON_CreateArray(), *row = cJSON_CreateObject();
    cJSON_AddNumberToObject(row, "fid", (double) s->account_id);
    cJSON_AddStringToObject(row, "fna", s->username);
    cJSON_AddNumberToObject(row, "cht", 1);
    cJSON_AddItemToArray(rows, row);
    cJSON_AddItemToArray(push, rows);
    social_push_account(c->a, push);
}

/* ["ict", peerId, text] -> ["chf", true] | ["chf", false, err] */
void sh_chat_send(SocialSession *s, const cJSON *f) {
    if (s->account_id <= 0) return;
    const char *txt = fa_str(f, 2);
    const int64_t peer = fa_int(f, 1, 0);
    if (!text_ok(txt, g_acct.chat_max_len > 0 ? g_acct.chat_max_len : 300) || peer <= 0 || peer == s->account_id) {
        social_send_n(s, "chf", 2, cJSON_CreateFalse(), cJSON_CreateString("Message not Sent"));
        return;
    }
    char me[24], p[24];
    social_query(s,
                 SQL(.pg = "INSERT INTO chat_messages (sender_id, receiver_id, body) SELECT $1, id, $3 FROM accounts WHERE id = $2 "
                           "AND (SELECT count(*) FROM chat_messages WHERE sender_id = $1 AND created_at > now() - interval '1 minute') < 30 "
                           "RETURNING id",
                     .any = "INSERT INTO chat_messages (sender_id, receiver_id, body) SELECT $1, id, $3 FROM accounts WHERE id = $2 "
                            "AND (SELECT count(*) FROM chat_messages WHERE sender_id = $1 AND created_at > {ago:60}) < 30"),
                 KN_DB_ARGS(me_str(s, me), acct_itoa(p, peer), txt), chat_sent, ctx_new("ict", peer, 0));
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

/* Social page: ["cln", tag] join request -> ["cln", msg, ok].
 * Menu clan window: ["cln", action, value, extra] -> ["cln", msg, data, ok, action]:
 *   1 create(tag) 2 request(tag) 3 my clan 4 leave 5 kick(id) 6 role(id,{r}) 7 funds(kr,{wd}) 8 contract */
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
    default:
        free(c);
        clan_menu_reply(s, "Coming Soon", NULL, 0, action);
        (void) v3;
        return;
    }
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

/* ["nme", alias] -> ["nmeR", err] | ["nmeR", null, alias] */
void sh_alias(SocialSession *s, const cJSON *f) {
    if (s->account_id <= 0) {
        social_send_n(s, "nmeR", 1, cJSON_CreateString("Not logged in."));
        return;
    }
    OpWait *w = calloc(1, sizeof *w);
    if (!w) return;
    w->s = social_hold(s);
    w->kind = 0;
    acct_set_alias(s->account_id, fa_str(f, 1), op_wait_done, w);
}

/* ["upp", old, new, repeat] -> ["upp", msg, newToken?] */
void sh_password(SocialSession *s, const cJSON *f) {
    if (s->account_id <= 0) {
        social_send_n(s, "upp", 1, cJSON_CreateString("Not logged in."));
        return;
    }
    const char *nw = fa_str(f, 2), *rep = fa_str(f, 3);
    if (!nw || !rep || strcmp(nw, rep) != 0) {
        social_send_n(s, "upp", 1, cJSON_CreateString("Passwords do not match."));
        return;
    }
    OpWait *w = calloc(1, sizeof *w);
    if (!w) return;
    w->s = social_hold(s);
    w->kind = 1;
    acct_change_password(s->account_id, fa_str(f, 1), nw, op_wait_done, w);
}

/* ------------------------------------------------------------------ mail */

static void mail_list_done(SocialSession *s, kn_db_result *res, void *extra) {
    Ctx *c = extra;
    cJSON *rows = kn_db_ok(res) ? kn_db_result_to_json(res) : NULL;
    if (!kn_db_ok(res)) acct_db_error(res, "kml");
    social_send_n(s, "kml", 2, cJSON_CreateNumber((double) c->a), rows ? rows : cJSON_CreateArray());
}

#define MAIL_COLS(html_l, html_r) \
    "SELECT id AS km_id, created_at AS km_datesent, " html_l "sender" html_r " AS km_playerfrom, " html_l "subject" html_r " AS km_subject, " \
    html_l "body" html_r " AS km_text, CASE WHEN read_at IS NULL THEN 0 ELSE 1 END AS km_archive, kr AS km_kr FROM mail "

/* ["kml", tab] -> ["kml", tab, rows] (windows[29].onData; tab 0 unread, 1 read) */
void sh_mail_list(SocialSession *s, const cJSON *f) {
    if (s->account_id <= 0) return;
    char me[24];
    const int64_t tab = fa_int(f, 1, 0) ? 1 : 0;
    social_query(s, SQL(.pg = MAIL_COLS("kn_html(", ")") "WHERE account_id = $1 ORDER BY id DESC LIMIT 100",
                        .any = MAIL_COLS("{html:", "}") "WHERE account_id = $1 ORDER BY id DESC LIMIT 100"),
                 KN_DB_ARGS(me_str(s, me)),
                 mail_list_done, ctx_new("kml", tab, 0));
}

static void mail_read_done(SocialSession *s, kn_db_result *res, void *extra) {
    (void) extra;
    cJSON *rows = kn_db_ok(res) && kn_db_rows(res) > 0 ? kn_db_result_to_json(res) : NULL;
    if (!kn_db_ok(res)) acct_db_error(res, "rml");
    social_send_n(s, "rml", 1, rows ? rows : cJSON_CreateNull());
}

/* ["rml", id] -> ["rml", [mail]] marks it read, credits attached KR once */
void sh_mail_read(SocialSession *s, const cJSON *f) {
    if (s->account_id <= 0) return;
    char me[24], id[24];
    social_call(s, &P_MAIL_READ,
                 KN_DB_ARGS(me_str(s, me), acct_itoa(id, fa_int(f, 1, 0))), mail_read_done, NULL);
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

