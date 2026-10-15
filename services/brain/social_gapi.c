/*
 * services/brain/social_gapi.c - Krunker v10.0.2's gapi (https://gapi.svc.krunker.io) for the
 * in-game Social hub, tunnelled through the social socket (doc/backend.md 7.4):
 *
 *   ["gapi", rid, method, "/path?query", body] -> ["gapi", rid, status, {"data": ...}]
 *                                                  or ["gapi", rid, status, {"error": {code, message}}]
 *
 * Deviation from v10: v10 sends HTTPS requests with a FRVR bearer token; KrunkNative's client
 * already holds an authenticated social socket, so the same paths and bodies travel over it and
 * the session is the auth. Answers keep v10's {data, error} shape and error codes (i18n
 * errors.*: SOCIAL_*, PLAYERS_*).
 *
 *   GET  /social/feed?tab=global|following|hot|giveaways|clan[&limit]   {posts}
 *   GET  /social/feed?player_id=<id>[&limit]                           {posts}
 *   GET  /social/post/<id>                                              {post, comments}
 *   POST /social/post {type, text, funds?}                              {post_id, player_funds}
 *   POST /social/post/<id>/vote {value 0|1}                             {value, votes}
 *   POST /social/post/<id>/comment {text}                               {comment_id}
 *   GET  /social/news                                                   {news: [html]}
 *   GET  /players/search?q&limit                                        {results}
 *   GET  /players/<name>                                                profile object
 *   POST /players/<name>/follow {follow}                                {following}
 *   GET  /players/<name>/followers | /following                         [{player_id, player_name, ...}]
 *   POST /players/<name>/gift {amount, message}                         {funds, amount, fee}
 *   POST /players/<name>/report {reason, text}                          {}
 *   GET  /players/<name>/sales?range=<0..6>                             [{d, f}]
 *   GET  /players/<name>/listings                                       [{marketid, skinindex, m, funds}]
 *   GET  /clans/<id>/profile, /clans/by-name/<name>                     clan profile (the clan view)
 *   POST /clans/<id>/apply                                              {}
 * Other /clans/* paths are clan_v10.c's (the clan window's settings tab), the /studios paths studios.c's.
 */
#include "acct_internal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hot.h"
#include "strike_econ.h"
#include "studios.h"
#include "knsvc/config.h"
#include "knsvc/json.h"
#include "knsvc/log.h"
#include "knsvc/loop.h"

#define MOD "gapi"

/* ------------------------------------------------------------------ framing */

static cJSON *gapi_frame(int64_t rid, int status, cJSON *payload) {
    cJSON *f = social_frame("gapi");
    cJSON_AddItemToArray(f, cJSON_CreateNumber((double) rid));
    cJSON_AddItemToArray(f, cJSON_CreateNumber(status));
    cJSON_AddItemToArray(f, payload);
    return f;
}

void gapi_reply(SocialSession *s, int64_t rid, cJSON *data) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "data", data ? data : cJSON_CreateNull());
    if (!social_open(s)) {
        cJSON_Delete(o);
        return;
    }
    social_send(s, gapi_frame(rid, 200, o));
}

void gapi_error(SocialSession *s, int64_t rid, int status, const char *code, const char *message) {
    cJSON *o = cJSON_CreateObject(), *e = cJSON_CreateObject();
    cJSON_AddStringToObject(e, "code", code ? code : "UNKNOWN");
    cJSON_AddStringToObject(e, "message", message ? message : "An unexpected error occurred. Please try again.");
    cJSON_AddItemToObject(o, "error", e);
    if (!social_open(s)) {
        cJSON_Delete(o);
        return;
    }
    social_send(s, gapi_frame(rid, status, o));
}

static int hexv(int c) { return isdigit(c) ? c - '0' : (tolower(c) >= 'a' && tolower(c) <= 'f') ? tolower(c) - 'a' + 10 : -1; }

/* in-place URL decoding ('+' = space in queries) */
static void url_decode(char *s, int plus) {
    char *o = s;
    for (; *s; s++) {
        if (*s == '%' && hexv(s[1]) >= 0 && hexv(s[2]) >= 0) {
            *o++ = (char) (hexv(s[1]) * 16 + hexv(s[2]));
            s += 2;
        } else {
            *o++ = plus && *s == '+' ? ' ' : *s;
        }
    }
    *o = 0;
}

int gapi_query(const GapiReq *r, const char *key, char *out, size_t n) {
    const size_t kl = strlen(key);
    for (const char *p = r->query; p && *p;) {
        const char *amp = strchr(p, '&');
        const size_t len = amp ? (size_t) (amp - p) : strlen(p);
        if (len > kl && p[kl] == '=' && !strncmp(p, key, kl)) {
            snprintf(out, n, "%.*s", (int) (len - kl - 1), p + kl + 1);
            url_decode(out, 1);
            return 1;
        }
        p = amp ? amp + 1 : NULL;
    }
    if (n) out[0] = 0;
    return 0;
}

int gapi_is(const GapiReq *r, const char *method, const char *pattern) {
    if (strcmp(r->method, method)) return 0;
    char pat[256];
    snprintf(pat, sizeof pat, "%s", pattern);
    int i = 0;
    for (char *save = NULL, *t = strtok_r(pat, "/", &save); t; t = strtok_r(NULL, "/", &save), i++) {
        if (i >= r->nseg) return 0;
        if (t[0] != ':' && strcmp(t, r->seg[i])) return 0;
    }
    return i == r->nseg;
}

static int64_t body_int(const cJSON *b, const char *k, int64_t def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(b, k);
    if (cJSON_IsNumber(v)) return (int64_t) v->valuedouble;
    if (cJSON_IsBool(v)) return cJSON_IsTrue(v);
    if (cJSON_IsString(v) && v->valuestring[0]) return strtoll(v->valuestring, NULL, 10);
    return def;
}

static int64_t query_int(const GapiReq *r, const char *k, int64_t def) {
    char v[32];
    return gapi_query(r, k, v, sizeof v) && v[0] ? strtoll(v, NULL, 10) : def;
}

/* async context: the request id plus a few values */
typedef struct GCtx {
    int64_t rid;
    int64_t a, b;
    char name[64];
} GCtx;

static GCtx *gctx(const GapiReq *r, int64_t a, int64_t b, const char *name) {
    GCtx *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->rid = r->rid;
    c->a = a;
    c->b = b;
    if (name) snprintf(c->name, sizeof c->name, "%s", name);
    return c;
}

static int need_login(SocialSession *s, const GapiReq *r) {
    if (s->account_id > 0) return 1;
    gapi_error(s, r->rid, 401, "UNAUTHORIZED", "You must be signed in.");
    return 0;
}

/* a proc / query answering one JSON value (acct_db_json) as data */
static void json_data_done(SocialSession *s, kn_db_result *res, void *extra) {
    GCtx *c = extra;
    if (!kn_db_ok(res)) {
        gapi_error(s, c->rid, 400, c->name[0] ? c->name : "UNKNOWN", acct_db_error(res, "gapi"));
        return;
    }
    gapi_reply(s, c->rid, acct_db_json(res));
}

/* ------------------------------------------------------------------ routes */

static int route(SocialSession *s, const GapiReq *r);

/* ["gapi", rid, method, path, body] */
void sh_gapi(SocialSession *s, const cJSON *f) {
    GapiReq r;
    memset(&r, 0, sizeof r);
    r.rid = fa_int(f, 1, 0);
    const char *method = fa_str(f, 2), *path = fa_str(f, 3);
    r.body = fa(f, 4);
    s->json = 1;
    if (!method || !path || path[0] != '/' || strlen(path) > 1000) {
        gapi_error(s, r.rid, 400, "BAD_REQUEST", "Bad request.");
        return;
    }
    snprintf(r.method, sizeof r.method, "%s", method);
    for (char *m = r.method; *m; m++) *m = (char) toupper((unsigned char) *m);
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", path);
    char *q = strchr(buf, '?');
    if (q) {
        *q++ = 0;
        snprintf(r.query, sizeof r.query, "%s", q);
    }
    for (char *save = NULL, *t = strtok_r(buf, "/", &save); t && r.nseg < GAPI_MAX_SEG; t = strtok_r(NULL, "/", &save)) {
        url_decode(t, 0);
        r.seg[r.nseg++] = t;
    }
    if (r.nseg > 0 && !strcmp(r.seg[0], "clans") && clan_v10_gapi(s, &r)) return;
    if (r.nseg > 0 && !strcmp(r.seg[0], "strike") && strike_econ_gapi(s, &r)) return; /* Krunker Strike (strike_econ.c) */
    if (r.nseg > 0 && !strcmp(r.seg[0], "studios") && studios_gapi(s, &r)) return; /* v10 Studios (studios.c) */
    if (route(s, &r)) return;
    gapi_error(s, r.rid, 404, "NOT_FOUND", "Not found.");
}

/* ["ua"] to the account's JSON sockets: the header KR after a giveaway was paid / won */
static void push_account_cb(void *ud, const char *err, const cJSON *data) {
    (void) ud;
    if (!err && data) social_push_account_update(data);
}

static void push_funds(int64_t account_id) {
    if (account_id > 0) acct_get(account_id, push_account_cb, NULL);
}

/* ------------------------------------------------------------------ write limits */

/* per account and kind: at most `limit` writes in `window_s` (also across brains with Redis) */
static int write_ok(SocialSession *s, const char *kind, int limit, int window_s) {
    static struct {
        int64_t account;
        char kind[12];
        int64_t start_ms;
        int count;
    } slots[256];
    const int64_t now = kn_wall_ms();
    unsigned h = (unsigned) (s->account_id * 2654435761u);
    for (const char *k = kind; *k; k++) h = h * 31u + (unsigned char) *k;
    const unsigned i = h % 256u;
    if (slots[i].account != s->account_id || strcmp(slots[i].kind, kind) || now - slots[i].start_ms > window_s * 1000LL) {
        slots[i].account = s->account_id;
        snprintf(slots[i].kind, sizeof slots[i].kind, "%s", kind);
        slots[i].start_ms = now;
        slots[i].count = 0;
    }
    const int local = ++slots[i].count <= limit;
    char key[32];
    return hot_limit(kind, acct_itoa(key, s->account_id), limit, window_s, local);
}

/* ------------------------------------------------------------------ /social/feed, /social/post */

/* v10 post object {pid, pli, pln, prm, ftr, cln, clr, flw, tim, typ, txt, lnk, lnk2, vot, dlk, cnt,
 * exp, fds} ($1 = the viewer). txt stays raw: v10 shows it as text (the v3.1.1 hub's kn_post_json
 * escapes it for innerHTML). */
#define POST_COLS \
    "SELECT fp.id AS pid__i, fp.author_id AS pli__i, r.player_name AS pln, coalesce(r.player_premium, 0) AS prm__i, " \
    "coalesce(r.player_featured, 0) AS ftr__i, coalesce(r.player_clan, '') AS cln, coalesce(r.clan_rank, 0) AS clr__i, " \
    "coalesce(r.player_followed, 0) AS flw__i, {epoch_ms:{now}} - {epoch_ms:fp.created_at} AS tim__i, fp.type AS typ__i, " \
    "fp.body AS txt, coalesce(fp.link, '') AS lnk, coalesce(fp.link2, '') AS lnk2, " \
    "(SELECT count(*) FROM feed_likes l WHERE l.post_id = fp.id) AS vot__i, " \
    "(SELECT count(*) FROM feed_likes l WHERE l.post_id = fp.id AND l.account_id = $1) AS dlk__i, " \
    "(SELECT count(*) FROM feed_comments c WHERE c.post_id = fp.id) AS cnt__i, " \
    "CASE WHEN fp.type = 9 AND fp.settled_at IS NULL AND fp.ends_at > {now} THEN {epoch_ms:fp.ends_at} - {epoch_ms:{now}} ELSE 0 END AS exp__i, " \
    "fp.funds AS fds__i " \
    "FROM feed_posts fp JOIN player_rows r ON r.player_id = fp.author_id WHERE fp.deleted_at IS NULL "

static void feed_done(SocialSession *s, kn_db_result *res, void *extra) {
    GCtx *c = extra;
    if (!kn_db_ok(res)) {
        acct_db_error(res, "feed");
        gapi_error(s, c->rid, 500, "SOCIAL_FEED_FAILED", "Failed to load feed");
        return;
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "posts", kn_db_result_to_json(res));
    gapi_reply(s, c->rid, o);
}

static void social_feed(SocialSession *s, const GapiReq *r) {
    char me[24], lim[24], who[24], tab[24];
    int64_t limit = query_int(r, "limit", 30);
    if (limit < 1 || limit > 50) limit = 30;
    social_viewer(s, me);
    acct_itoa(lim, limit);
    GCtx *c = gctx(r, 0, 0, NULL);
    const int64_t player = query_int(r, "player_id", 0);
    if (player > 0) {
        social_query(s, SQL(.any = POST_COLS "AND fp.author_id = $2 ORDER BY fp.id DESC LIMIT {int:$3}"),
                     KN_DB_ARGS(me, acct_itoa(who, player), lim), feed_done, c);
        return;
    }
    if (!gapi_query(r, "tab", tab, sizeof tab) || !tab[0]) snprintf(tab, sizeof tab, "global");
    if (!strcmp(tab, "following"))
        social_query(s, SQL(.any = POST_COLS "AND fp.author_id IN (SELECT followee_id FROM follows WHERE follower_id = $1) "
                                             "ORDER BY fp.id DESC LIMIT {int:$2}"),
                     KN_DB_ARGS(me, lim), feed_done, c);
    else if (!strcmp(tab, "hot")) /* the last week's posts by likes + 2 x comments */
        social_query(s, SQL(.any = "SELECT * FROM (" POST_COLS "AND fp.created_at > {ago:604800}) t "
                                   "ORDER BY t.vot__i + 2 * t.cnt__i DESC, t.pid__i DESC LIMIT {int:$2}"),
                     KN_DB_ARGS(me, lim), feed_done, c);
    else if (!strcmp(tab, "giveaways")) /* running giveaways, ending soonest first */
        social_query(s, SQL(.any = POST_COLS "AND fp.type = 9 AND fp.settled_at IS NULL AND fp.ends_at > {now} "
                                             "ORDER BY fp.ends_at LIMIT {int:$2}"),
                     KN_DB_ARGS(me, lim), feed_done, c);
    else if (!strcmp(tab, "clan")) /* the viewer's clan members' posts (none without a clan) */
        social_query(s, SQL(.any = POST_COLS "AND fp.author_id IN (SELECT m.account_id FROM clan_members m WHERE m.clan_id = "
                                             "(SELECT x.clan_id FROM clan_members x WHERE x.account_id = $1)) "
                                             "ORDER BY fp.id DESC LIMIT {int:$2}"),
                     KN_DB_ARGS(me, lim), feed_done, c);
    else
        social_query(s, SQL(.any = POST_COLS "ORDER BY fp.id DESC LIMIT {int:$2}"), KN_DB_ARGS(me, lim), feed_done, c);
}

/* post detail: the post, then its comments */
static void comments_done(SocialSession *s, kn_db_result *res, void *extra) {
    GCtx *c = extra; /* a = the post JSON (owned, as a pointer in b) */
    cJSON *post = (cJSON *) (intptr_t) c->b;
    if (!kn_db_ok(res)) {
        cJSON_Delete(post);
        acct_db_error(res, "post comments");
        gapi_error(s, c->rid, 500, "SOCIAL_POST_FAILED", "Failed to load post");
        return;
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "post", post);
    cJSON_AddItemToObject(o, "comments", kn_db_result_to_json(res));
    gapi_reply(s, c->rid, o);
}

static void post_done(SocialSession *s, kn_db_result *res, void *extra) {
    GCtx *c = extra;
    if (!kn_db_ok(res) || kn_db_rows(res) < 1) {
        if (!kn_db_ok(res)) acct_db_error(res, "post");
        gapi_error(s, c->rid, 404, "SOCIAL_POST_NOT_FOUND", "Post not found");
        return;
    }
    cJSON *rows = kn_db_result_to_json(res);
    cJSON *post = cJSON_DetachItemFromArray(rows, 0);
    cJSON_Delete(rows);
    GCtx *d = calloc(1, sizeof *d);
    if (!d) {
        cJSON_Delete(post);
        return;
    }
    d->rid = c->rid;
    d->b = (int64_t) (intptr_t) post;
    char pid[24];
    /* comments oldest first: {pid (the comment's id), pli, pln, prm, ftr, cln, tim, txt} */
    if (social_query(s,
                     SQL(.any = "SELECT c.id AS pid__i, c.author_id AS pli__i, r.player_name AS pln, coalesce(r.player_premium, 0) AS prm__i, "
                                "coalesce(r.player_featured, 0) AS ftr__i, coalesce(r.player_clan, '') AS cln, "
                                "{epoch_ms:{now}} - {epoch_ms:c.created_at} AS tim__i, c.body AS txt "
                                "FROM feed_comments c JOIN player_rows r ON r.player_id = c.author_id WHERE c.post_id = $1 ORDER BY c.id"),
                     KN_DB_ARGS(acct_itoa(pid, c->a)), comments_done, d) != 0)
        cJSON_Delete(post);
}

static void social_post(SocialSession *s, const GapiReq *r, int64_t id) {
    char me[24], pid[24];
    social_query(s, SQL(.any = POST_COLS "AND fp.id = $2"), KN_DB_ARGS(social_viewer(s, me), acct_itoa(pid, id)), post_done,
                 gctx(r, id, 0, NULL));
}

static void create_done(SocialSession *s, kn_db_result *res, void *extra) {
    GCtx *c = extra;
    if (!kn_db_ok(res)) {
        const char *code = acct_db_error(res, "post create");
        gapi_error(s, c->rid, 400, !strncmp(code, "SOCIAL_", 7) ? code : "SOCIAL_CREATE_FAILED", code);
        return;
    }
    cJSON *v = acct_db_json(res);
    if (v && kn_json_int(v, "player_funds", -1) >= 0 && c->a == 9) push_funds(s->account_id);
    gapi_reply(s, c->rid, v);
}

static void social_create(SocialSession *s, const GapiReq *r) {
    if (!need_login(s, r)) return;
    if (!write_ok(s, "spost", 20, 3600)) {
        gapi_error(s, r->rid, 429, "SOCIAL_RATE_LIMITED", "Rate limit exceeded. Try again later.");
        return;
    }
    const char *text = kn_json_str(r->body, "text", "");
    while (*text == ' ' || *text == '\n' || *text == '\t') text++;
    char me[24], type[24], funds[24], max[24], hours[24];
    const int64_t t = body_int(r->body, "type", 0) == 9 ? 9 : 0;
    social_call(s, &P_POST_CREATE_V10,
                KN_DB_ARGS(social_viewer(s, me), acct_itoa(type, t), text, acct_itoa(funds, body_int(r->body, "funds", 0)),
                           acct_itoa(max, g_acct.post_max_len > 0 ? g_acct.post_max_len : 1024), "20",
                           acct_itoa(hours, kn_config_int(g_acct_brain->cfg, "accounts.giveaway_hours", 24))),
                create_done, gctx(r, t, 0, NULL));
}

static void vote_done(SocialSession *s, kn_db_result *res, void *extra) {
    GCtx *c = extra;
    if (!kn_db_ok(res)) {
        gapi_error(s, c->rid, 400, "SOCIAL_VOTE_FAILED", acct_db_error(res, "vote"));
        return;
    }
    if (c->b == 1 && kn_db_affected(res) < 1) { /* the insert matched no post */
        gapi_error(s, c->rid, 404, "SOCIAL_POST_NOT_FOUND", "Post not found");
        return;
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "value", (double) c->b);
    gapi_reply(s, c->rid, o);
}

static void social_vote(SocialSession *s, const GapiReq *r, int64_t id) {
    if (!need_login(s, r)) return;
    if (!write_ok(s, "svote", 120, 60)) {
        gapi_error(s, r->rid, 429, "SOCIAL_RATE_LIMITED", "Rate limit exceeded. Try again later.");
        return;
    }
    const int64_t value = body_int(r->body, "value", 1) ? 1 : 0;
    char me[24], pid[24];
    if (value)
        social_query(s,
                     SQL(.pg = "INSERT INTO feed_likes (post_id, account_id) SELECT id, $1 FROM feed_posts WHERE id = $2 "
                               "AND deleted_at IS NULL ON CONFLICT DO NOTHING",
                         .any = "{insert_ignore} INTO feed_likes (post_id, account_id) SELECT id, $1 FROM feed_posts WHERE id = $2 "
                                "AND deleted_at IS NULL {ignore_conflicts}"),
                     KN_DB_ARGS(social_viewer(s, me), acct_itoa(pid, id)), vote_done, gctx(r, id, 2, NULL));
    else
        social_query(s, SQL(.any = "DELETE FROM feed_likes WHERE account_id = $1 AND post_id = $2"),
                     KN_DB_ARGS(social_viewer(s, me), acct_itoa(pid, id)), vote_done, gctx(r, id, 0, NULL));
}

static void comment_done(SocialSession *s, kn_db_result *res, void *extra) {
    GCtx *c = extra;
    if (!kn_db_ok(res) || kn_db_affected(res) < 1) {
        if (!kn_db_ok(res)) acct_db_error(res, "comment");
        gapi_error(s, c->rid, kn_db_ok(res) ? 404 : 500, kn_db_ok(res) ? "SOCIAL_POST_NOT_FOUND" : "SOCIAL_COMMENT_FAILED",
                   "Failed to post comment");
        return;
    }
    gapi_reply(s, c->rid, cJSON_CreateObject());
}

static void social_comment(SocialSession *s, const GapiReq *r, int64_t id) {
    if (!need_login(s, r)) return;
    const char *text = kn_json_str(r->body, "text", "");
    while (*text == ' ' || *text == '\n' || *text == '\t') text++;
    if (!*text) {
        gapi_error(s, r->rid, 400, "SOCIAL_COMMENT_TEXT_REQUIRED", "Comment text required");
        return;
    }
    if (!write_ok(s, "scom", 30, 600)) {
        gapi_error(s, r->rid, 429, "SOCIAL_RATE_LIMITED", "Rate limit exceeded. Try again later.");
        return;
    }
    char body[2048], me[24], pid[24];
    const int max = g_acct.comment_max_len > 0 && g_acct.comment_max_len < 2000 ? g_acct.comment_max_len : 500;
    size_t len = strlen(text);
    if ((int) len > max) {
        len = (size_t) max;
        while (len > 0 && ((unsigned char) text[len] & 0xC0) == 0x80) len--;
    }
    snprintf(body, sizeof body, "%.*s", (int) len, text);
    social_query(s,
                 SQL(.any = "INSERT INTO feed_comments (post_id, author_id, body) SELECT id, $1, $3 FROM feed_posts "
                            "WHERE id = $2 AND deleted_at IS NULL"),
                 KN_DB_ARGS(social_viewer(s, me), acct_itoa(pid, id), body), comment_done, gctx(r, id, 0, NULL));
}

/* GET /social/news: accounts.news (HTML strings), v10's daily news items */
static void social_news(SocialSession *s, const GapiReq *r) {
    cJSON *o = cJSON_CreateObject(), *list = cJSON_CreateArray();
    const cJSON *news = kn_config_get(g_acct_brain->cfg, "accounts.news"), *it;
    cJSON_ArrayForEach(it, news) if (cJSON_IsString(it) && it->valuestring[0]) cJSON_AddItemToArray(list, cJSON_CreateString(it->valuestring));
    cJSON_AddItemToObject(o, "news", list);
    gapi_reply(s, r->rid, o);
}

/* ------------------------------------------------------------------ /players */

static void like_escape(const char *in, char *out, size_t cap) {
    size_t o = 0;
    for (; in && *in && o + 2 < cap; in++) {
        if (*in == '%' || *in == '_' || *in == '\\') out[o++] = '\\';
        out[o++] = *in;
    }
    out[o] = 0;
}

static void search_done(SocialSession *s, kn_db_result *res, void *extra) {
    GCtx *c = extra;
    if (!kn_db_ok(res)) {
        acct_db_error(res, "player search");
        gapi_error(s, c->rid, 500, "PLAYERS_SEARCH_FAILED", "Search failed");
        return;
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "results", kn_db_result_to_json(res));
    gapi_reply(s, c->rid, o);
}

/* GET /players/search?q&limit -> {results: [{player_id, player_name, player_score, player_premium,
 * player_featured, player_clan, player_hack}]} (exact name first, then by score) */
static void players_search(SocialSession *s, const GapiReq *r) {
    char q[96], pat[200], lim[24];
    gapi_query(r, "q", q, sizeof q);
    const size_t len = strlen(q);
    if (len < 2 || len > 64) {
        gapi_error(s, r->rid, 400, "PLAYERS_SEARCH_INVALID", "Enter at least 2 characters to search.");
        return;
    }
    const int64_t limit = query_int(r, "limit", 20);
    if (limit < 1 || limit > 50) {
        gapi_error(s, r->rid, 400, "PLAYERS_SEARCH_LIMIT_INVALID", "Invalid search limit.");
        return;
    }
    like_escape(q, pat, sizeof pat);
    social_query(s,
                 SQL(.pg = "SELECT player_id AS player_id__i, player_name, player_score AS player_score__i, "
                           "coalesce(player_premium, 0) AS player_premium__i, coalesce(player_featured, 0) AS player_featured__i, "
                           "coalesce(player_clan, '') AS player_clan, coalesce(player_hack, 0) AS player_hack__i "
                           "FROM player_rows WHERE player_name ILIKE '%' || $1 || '%' "
                           "ORDER BY (lower(player_name) = lower($2)) DESC, player_score DESC LIMIT $3::int",
                     .any = "SELECT player_id AS player_id__i, player_name, player_score AS player_score__i, "
                            "coalesce(player_premium, 0) AS player_premium__i, coalesce(player_featured, 0) AS player_featured__i, "
                            "coalesce(player_clan, '') AS player_clan, coalesce(player_hack, 0) AS player_hack__i "
                            "FROM player_rows WHERE player_name {ilike} '%' || $1 || '%' ESCAPE '\\' "
                            "ORDER BY (lower(player_name) = lower($2)) DESC, player_score DESC LIMIT {int:$3}"),
                 KN_DB_ARGS(pat, q, acct_itoa(lim, limit)), search_done, gctx(r, 0, 0, NULL));
}

/* GET /players/<name>: v10's profile data from P_PROFILE (player_rows + follow + creations) */
typedef struct ProfileWait {
    SocialSession *s; /* held */
    int64_t rid;
    cJSON *data;
} ProfileWait;

static void profile_online(void *ud, const int *online, size_t n) {
    ProfileWait *w = ud;
    cJSON_AddNumberToObject(w->data, "online", n > 0 && online[0] ? 1 : 0);
    gapi_reply(w->s, w->rid, w->data);
    social_unhold(w->s);
    free(w);
}

static cJSON *take(cJSON *from, const char *key) {
    cJSON *v = cJSON_DetachItemFromObjectCaseSensitive(from, key);
    return v ? v : cJSON_CreateNull();
}

static void profile_done(SocialSession *s, kn_db_result *res, void *extra) {
    GCtx *c = extra;
    cJSON *obj = kn_db_ok(res) ? acct_db_json(res) : NULL;
    cJSON *row = obj ? cJSON_DetachItemFromObjectCaseSensitive(obj, "row") : NULL;
    if (!cJSON_IsObject(row)) {
        if (!kn_db_ok(res)) acct_db_error(res, "profile");
        cJSON_Delete(obj);
        cJSON_Delete(row);
        gapi_error(s, c->rid, 404, "PLAYERS_NOT_FOUND", "Profile not found");
        return;
    }
    /* v10 reads player_stats as an object or a JSON string; player_badge / player_titles as
     * "1,5,9"; raid_stats / match_history arrays */
    const cJSON *stats = cJSON_GetObjectItemCaseSensitive(row, "player_stats");
    if (cJSON_IsString(stats)) {
        cJSON *parsed = cJSON_Parse(stats->valuestring);
        if (parsed) cJSON_ReplaceItemInObjectCaseSensitive(row, "player_stats", parsed);
    }
    cJSON_AddNumberToObject(row, "follow_state", kn_json_int(obj, "follow", 0) > 0 ? 1 : 0);
    cJSON_DeleteItemFromObjectCaseSensitive(row, "player_follow");
    cJSON *maps = cJSON_CreateArray(), *mods = cJSON_CreateArray(), *m;
    cJSON_ArrayForEach(m, cJSON_GetObjectItemCaseSensitive(obj, "games")) {
        cJSON *g = cJSON_CreateObject();
        cJSON_AddNumberToObject(g, "mid", (double) kn_json_int(m, "map_id", 0));
        cJSON_AddStringToObject(g, "mna", kn_json_str(m, "map_name", ""));
        cJSON_AddNumberToObject(g, "vte", (double) kn_json_int(m, "map_votes", 0));
        cJSON_AddItemToArray(maps, g);
    }
    cJSON_ArrayForEach(m, cJSON_GetObjectItemCaseSensitive(obj, "mods")) {
        cJSON *g = cJSON_CreateObject();
        cJSON_AddStringToObject(g, "mna", kn_json_str(m, "mod_name", ""));
        cJSON_AddNumberToObject(g, "vte", (double) kn_json_int(m, "mod_votes", 0));
        cJSON_AddNumberToObject(g, "ftr", 0);
        cJSON_AddItemToArray(mods, g);
    }
    cJSON_AddItemToObject(row, "maps", maps);
    cJSON_AddItemToObject(row, "mods", mods);
    cJSON_AddItemToObject(row, "assets", take(obj, "assets"));
    cJSON_AddItemToObject(row, "raid_stats", cJSON_CreateArray());
    cJSON_AddItemToObject(row, "match_history", cJSON_CreateArray());
    if (!cJSON_HasObjectItem(row, "ranked_rank")) cJSON_AddNumberToObject(row, "ranked_rank", 0);
    cJSON_Delete(obj);
    ProfileWait *w = calloc(1, sizeof *w);
    if (!w) {
        cJSON_Delete(row);
        return;
    }
    w->s = social_hold(s);
    w->rid = c->rid;
    w->data = row;
    const int64_t id = kn_json_int(row, "player_id", 0);
    social_online_lookup(&id, 1, profile_online, w);
}

static void players_profile(SocialSession *s, const GapiReq *r, const char *name) {
    char viewer[24];
    social_call(s, &P_PROFILE, KN_DB_ARGS(name, social_viewer(s, viewer)), profile_done, gctx(r, 0, 0, NULL));
}

static void follow_done(SocialSession *s, kn_db_result *res, void *extra) {
    GCtx *c = extra;
    if (!kn_db_ok(res)) {
        gapi_error(s, c->rid, 400, "PLAYERS_FOLLOW_FAILED", acct_db_error(res, "follow"));
        return;
    }
    if (c->a && kn_db_affected(res) < 1) { /* no such player (or yourself); already following is not an error */
        gapi_error(s, c->rid, 404, "PLAYERS_NOT_FOUND", "That player could not be found.");
        return;
    }
    if (kn_db_affected(res) > 0) social_eb_follow_changed(s->account_id, c->name);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "following", c->a != 0);
    gapi_reply(s, c->rid, o);
}

/* POST /players/<name>/follow {follow} -> {following} (follow back = friends, social_eb.c) */
static void players_follow(SocialSession *s, const GapiReq *r, const char *name) {
    if (!need_login(s, r)) return;
    if (!write_ok(s, "follow", 60, 60)) {
        gapi_error(s, r->rid, 429, "RATE_LIMITED", "Rate limit exceeded. Try again later.");
        return;
    }
    const int follow = body_int(r->body, "follow", 1) != 0;
    char me[24];
    social_viewer(s, me);
    if (follow)
        social_query(s,
                     SQL(.pg = "INSERT INTO follows (follower_id, followee_id) SELECT $1, id FROM accounts "
                               "WHERE lower(username) = lower($2) AND id <> $1 ON CONFLICT (follower_id, followee_id) DO UPDATE "
                               "SET created_at = follows.created_at",
                         .sqlite = "INSERT INTO follows (follower_id, followee_id) SELECT $1, id FROM accounts "
                                   "WHERE lower(username) = lower($2) AND id <> $1 ON CONFLICT (follower_id, followee_id) DO UPDATE "
                                   "SET created_at = follows.created_at",
                         .mysql = "INSERT INTO follows (follower_id, followee_id) SELECT $1, id FROM accounts "
                                  "WHERE lower(username) = lower($2) AND id <> $1 ON DUPLICATE KEY UPDATE created_at = created_at"),
                     KN_DB_ARGS(me, name), follow_done, gctx(r, 1, 0, name));
    else
        social_query(s, SQL(.any = "DELETE FROM follows WHERE follower_id = $1 AND followee_id = "
                                   "(SELECT id FROM accounts WHERE lower(username) = lower($2))"),
                     KN_DB_ARGS(me, name), follow_done, gctx(r, 0, 0, name));
}

static void rows_data_done(SocialSession *s, kn_db_result *res, void *extra) {
    GCtx *c = extra;
    if (!kn_db_ok(res)) {
        gapi_error(s, c->rid, 500, c->name[0] ? c->name : "FAILED", acct_db_error(res, "gapi rows"));
        return;
    }
    gapi_reply(s, c->rid, kn_db_result_to_json(res));
}

/* GET /players/<name>/followers | /following -> [{player_id, player_name, premium, player_featured,
 * date, follow_state (the viewer follows them)}] */
static void players_follows(SocialSession *s, const GapiReq *r, const char *name, int followers) {
    char me[24];
    social_viewer(s, me);
    if (followers)
        social_query(s,
                     SQL(.any = "SELECT p.player_id AS player_id__i, p.player_name, coalesce(p.player_premium, 0) AS premium__i, "
                                "coalesce(p.player_featured, 0) AS player_featured__i, f.created_at AS date, "
                                "CASE WHEN EXISTS (SELECT 1 FROM follows v WHERE v.follower_id = $2 AND v.followee_id = p.player_id) "
                                "THEN 1 ELSE 0 END AS follow_state__i FROM follows f JOIN player_rows p ON p.player_id = f.follower_id "
                                "WHERE f.followee_id = (SELECT id FROM accounts WHERE lower(username) = lower($1)) "
                                "ORDER BY f.created_at DESC LIMIT 200"),
                     KN_DB_ARGS(name, me), rows_data_done, gctx(r, 0, 0, "PLAYERS_FOLLOWERS_FAILED"));
    else
        social_query(s,
                     SQL(.any = "SELECT p.player_id AS player_id__i, p.player_name, coalesce(p.player_premium, 0) AS premium__i, "
                                "coalesce(p.player_featured, 0) AS player_featured__i, f.created_at AS date, "
                                "CASE WHEN EXISTS (SELECT 1 FROM follows v WHERE v.follower_id = $2 AND v.followee_id = p.player_id) "
                                "THEN 1 ELSE 0 END AS follow_state__i FROM follows f JOIN player_rows p ON p.player_id = f.followee_id "
                                "WHERE f.follower_id = (SELECT id FROM accounts WHERE lower(username) = lower($1)) "
                                "ORDER BY f.created_at DESC LIMIT 200"),
                     KN_DB_ARGS(name, me), rows_data_done, gctx(r, 0, 0, "PLAYERS_FOLLOWING_FAILED"));
}

/* ------------------------------------------------------------------ /clans (the clan view) */

/* {clan_id, clan_name, clan_logo, creator_name, clan_score, clan_link4, members[{p, r, s7, su, ve, h}]};
 * s7 = the member's clan score (KrunkNative keeps no 7-day window per member) */
#define CLAN_PROFILE_COLS \
    "SELECT c.clan_id AS clan_id__i, c.clan_name, (SELECT username FROM accounts WHERE id = c.clan_playerid) AS creator_name, " \
    "c.clan_score AS clan_score__i, coalesce(c.clan_discord, '') AS clan_link4 FROM clans c "

static void clan_members_done(SocialSession *s, kn_db_result *res, void *extra) {
    GCtx *c = extra;
    cJSON *clan = (cJSON *) (intptr_t) c->b;
    if (!kn_db_ok(res)) {
        cJSON_Delete(clan);
        gapi_error(s, c->rid, 500, "CLAN_PROFILE_FAILED", acct_db_error(res, "clan members"));
        return;
    }
    cJSON_AddItemToObject(clan, "members", kn_db_result_to_json(res));
    gapi_reply(s, c->rid, clan);
}

static void clan_profile_done(SocialSession *s, kn_db_result *res, void *extra) {
    GCtx *c = extra;
    if (!kn_db_ok(res) || kn_db_rows(res) < 1) {
        if (!kn_db_ok(res)) acct_db_error(res, "clan profile");
        gapi_error(s, c->rid, 404, "CLAN_NOT_FOUND", "Clan not found");
        return;
    }
    cJSON *rows = kn_db_result_to_json(res);
    cJSON *clan = cJSON_DetachItemFromArray(rows, 0);
    cJSON_Delete(rows);
    cJSON_AddNumberToObject(clan, "clan_logo", 0); /* logo version: none (clan_v10.c serves uploads) */
    GCtx *d = calloc(1, sizeof *d);
    if (!d) {
        cJSON_Delete(clan);
        return;
    }
    d->rid = c->rid;
    d->b = (int64_t) (intptr_t) clan;
    char id[24];
    if (social_query(s,
                     SQL(.any = "SELECT r.player_name AS p, m.rank AS r__i, m.score AS s7__i, "
                                "CASE WHEN r.player_premium > 0 THEN 1 ELSE 0 END AS su__i, coalesce(r.player_featured, 0) AS ve__i, "
                                "coalesce(r.player_hack, 0) AS h__i FROM clan_members m JOIN player_rows r ON r.player_id = m.account_id "
                                "WHERE m.clan_id = $1 ORDER BY m.score DESC"),
                     KN_DB_ARGS(acct_itoa(id, kn_json_int(clan, "clan_id", 0))), clan_members_done, d) != 0)
        cJSON_Delete(clan);
}

static void clans_profile(SocialSession *s, const GapiReq *r, int64_t id, const char *name) {
    char v[24];
    if (name)
        social_query(s, SQL(.any = CLAN_PROFILE_COLS "WHERE lower(c.clan_name) = lower($1)"), KN_DB_ARGS(name), clan_profile_done,
                     gctx(r, 0, 0, NULL));
    else
        social_query(s, SQL(.any = CLAN_PROFILE_COLS "WHERE c.clan_id = $1"), KN_DB_ARGS(acct_itoa(v, id)), clan_profile_done,
                     gctx(r, 0, 0, NULL));
}

static void apply_done(SocialSession *s, kn_db_result *res, void *extra) {
    GCtx *c = extra;
    if (!kn_db_ok(res)) {
        gapi_error(s, c->rid, 400, "CLAN_APPLY_FAILED", acct_db_error(res, "clan apply"));
        return;
    }
    gapi_reply(s, c->rid, cJSON_CreateObject());
}

static void apply_tag_done(SocialSession *s, kn_db_result *res, void *extra) {
    GCtx *c = extra;
    if (!kn_db_ok(res) || kn_db_rows(res) < 1) {
        gapi_error(s, c->rid, 404, "CLAN_NOT_FOUND", "Clan not found");
        return;
    }
    GCtx *d = calloc(1, sizeof *d);
    if (!d) return;
    d->rid = c->rid;
    char me[24];
    social_call(s, &P_CLAN_REQUEST, KN_DB_ARGS(social_viewer(s, me), kn_db_get(res, 0, 0)), apply_done, d);
}

/* POST /clans/<id>/apply: v10's "Request to Join" (the same request as the clan window's cln 2) */
static void clans_apply(SocialSession *s, const GapiReq *r, int64_t id) {
    if (!need_login(s, r)) return;
    if (!write_ok(s, "capply", 20, 600)) {
        gapi_error(s, r->rid, 429, "RATE_LIMITED", "Rate limit exceeded. Try again later.");
        return;
    }
    char v[24];
    social_query(s, SQL(.any = "SELECT clan_name FROM clans WHERE clan_id = $1"), KN_DB_ARGS(acct_itoa(v, id)), apply_tag_done,
                 gctx(r, 0, 0, NULL));
}

/* ------------------------------------------------------------------ route table */

static int route(SocialSession *s, const GapiReq *r) {
    const int n = r->nseg;
    const char *a = n > 0 ? r->seg[0] : "";
    if (!strcmp(a, "social")) {
        if (gapi_is(r, "GET", "/social/feed")) social_feed(s, r);
        else if (gapi_is(r, "GET", "/social/news")) social_news(s, r);
        else if (gapi_is(r, "POST", "/social/post")) social_create(s, r);
        else if (gapi_is(r, "GET", "/social/post/:id")) social_post(s, r, strtoll(r->seg[2], NULL, 10));
        else if (gapi_is(r, "POST", "/social/post/:id/vote")) social_vote(s, r, strtoll(r->seg[2], NULL, 10));
        else if (gapi_is(r, "POST", "/social/post/:id/comment")) social_comment(s, r, strtoll(r->seg[2], NULL, 10));
        else return 0;
        return 1;
    }
    if (!strcmp(a, "players")) {
        if (gapi_is(r, "GET", "/players/search")) players_search(s, r);
        else if (gapi_is(r, "GET", "/players/:name")) players_profile(s, r, r->seg[1]);
        else if (gapi_is(r, "POST", "/players/:name/follow")) players_follow(s, r, r->seg[1]);
        else if (gapi_is(r, "GET", "/players/:name/followers")) players_follows(s, r, r->seg[1], 1);
        else if (gapi_is(r, "GET", "/players/:name/following")) players_follows(s, r, r->seg[1], 0);
        else return 0;
        return 1;
    }
    if (!strcmp(a, "clans")) {
        if (gapi_is(r, "GET", "/clans/by-name/:name")) clans_profile(s, r, 0, r->seg[2]);
        else if (gapi_is(r, "GET", "/clans/:id/profile")) clans_profile(s, r, strtoll(r->seg[1], NULL, 10), NULL);
        else if (gapi_is(r, "POST", "/clans/:id/apply")) clans_apply(s, r, strtoll(r->seg[1], NULL, 10));
        else return 0;
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ giveaway draws */

static void settle_next(void);

static void settle_done(kn_db_result *res, void *ud) {
    (void) ud;
    cJSON *v = kn_db_ok(res) ? acct_db_json(res) : NULL;
    if (!kn_db_ok(res)) acct_db_error(res, "giveaway settle");
    if (cJSON_IsObject(v)) {
        const int64_t winner = kn_json_int(v, "winner", 0), author = kn_json_int(v, "author", 0);
        KN_INFO(MOD, "giveaway %lld settled: %lld KR to %s %lld", (long long) kn_json_int(v, "post_id", 0),
                (long long) kn_json_int(v, "prize", 0), winner ? "winner" : "author (no entrant)", (long long) (winner ? winner : author));
        push_funds(winner ? winner : author);
        if (winner) social_push_mail_count(winner);
        cJSON_Delete(v);
        settle_next(); /* the next ended one, if any */
        return;
    }
    cJSON_Delete(v);
}

static void settle_next(void) {
    if (g_acct_brain && g_acct_brain->db) kn_db_call(g_acct_brain->db, &P_GIVEAWAY_SETTLE, 0, NULL, settle_done, NULL);
}

static void settle_tick(kn_loop *loop, void *ud) {
    (void) loop;
    (void) ud;
    settle_next();
}

static kn_timer_id g_settle_timer;

void social_gapi_init(Brain *b) {
    if (!b || !b->db || !b->loop || g_settle_timer) return;
    const uint64_t every = (uint64_t) kn_config_int(b->cfg, "accounts.giveaway_check_ms", 30000);
    g_settle_timer = kn_loop_add_timer(b->loop, every, every, settle_tick, NULL);
}
