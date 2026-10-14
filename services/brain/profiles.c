/*
 * services/brain/profiles.c - the social page's "r" page requests and the
 * profile features: profile rows, player search, follows, profile tabs, reports
 * and staff flags (Krunker v3.1.1 src/social.js io_0 / io_spR / io_fwsr / io_cwr /
 * io_uf / io_psd / io_rps; matchmaker/socialWs.ts "r", "f", "gfs", "gfg").
 */
#include "acct_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "knsvc/json.h"
#include "knsvc/log.h"

#define MOD "social"

/* kn_account_id($1) on every dialect */
#define ACCOUNT_ID_OF_1 "(SELECT id FROM accounts WHERE lower(username) = lower($1))"

/* ------------------------------------------------------------------ helpers */

typedef struct PageCtx {
    char page[24];
    char sub[128];
    int sub_null;
    int64_t num;
} PageCtx;

static PageCtx *page_ctx(const char *page, const cJSON *sub) {
    PageCtx *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    snprintf(c->page, sizeof c->page, "%s", page ? page : "");
    if (cJSON_IsString(sub)) snprintf(c->sub, sizeof c->sub, "%s", sub->valuestring);
    else if (cJSON_IsNumber(sub)) snprintf(c->sub, sizeof c->sub, "%lld", (long long) sub->valuedouble);
    else c->sub_null = 1;
    return c;
}

static cJSON *page_sub(const PageCtx *c) { return c->sub_null ? cJSON_CreateNull() : cJSON_CreateString(c->sub); }

/* ["0", page, sub, data] */
static void send_page(SocialSession *s, const PageCtx *c, cJSON *data) {
    cJSON *f = social_frame("0");
    cJSON_AddItemToArray(f, cJSON_CreateString(c->page));
    cJSON_AddItemToArray(f, page_sub(c));
    cJSON_AddItemToArray(f, data ? data : cJSON_CreateNull());
    social_send(s, f);
}

/* Query result = one jsonb column -> page data (NULL row -> null). */
static void page_json_done(SocialSession *s, kn_db_result *res, void *extra) {
    PageCtx *c = extra;
    if (!kn_db_ok(res)) {
        acct_db_error(res, c->page);
        send_page(s, c, NULL);
        return;
    }
    send_page(s, c, acct_db_json(res));
}

/* Escape LIKE wildcards of a search term (out >= 2*len+1). */
static void like_escape(const char *in, char *out, size_t cap) {
    size_t o = 0;
    for (; in && *in && o + 2 < cap; in++) {
        if (*in == '%' || *in == '_' || *in == '\\') out[o++] = '\\';
        out[o++] = *in;
    }
    out[o] = 0;
}

/* ------------------------------------------------------------------ profile */

/* profile query: P_PROFILE (db_social.c) */

static void profile_done(SocialSession *s, kn_db_result *res, void *extra) {
    PageCtx *c = extra;
    cJSON *obj = kn_db_ok(res) ? acct_db_json(res) : NULL;
    if (!kn_db_ok(res)) acct_db_error(res, "profile");
    cJSON *f = social_frame("0");
    cJSON_AddItemToArray(f, cJSON_CreateString("profile"));
    cJSON_AddItemToArray(f, page_sub(c));
    if (!obj) {
        cJSON_AddItemToArray(f, cJSON_CreateNull());
        social_send(s, f);
        return;
    }
    /* io_0(e, a, t=row, r=games, l=mods, c=followState, f=assets) */
    cJSON_AddItemToArray(f, cJSON_DetachItemFromObject(obj, "row"));
    cJSON_AddItemToArray(f, cJSON_DetachItemFromObject(obj, "games"));
    cJSON_AddItemToArray(f, cJSON_DetachItemFromObject(obj, "mods"));
    cJSON *follow = cJSON_CreateArray(), *st = cJSON_CreateObject();
    cJSON_AddNumberToObject(st, "f", (double) kn_json_int(obj, "follow", 0));
    cJSON_AddItemToArray(follow, st);
    cJSON_AddItemToArray(f, follow);
    cJSON_AddItemToArray(f, cJSON_DetachItemFromObject(obj, "assets"));
    cJSON_Delete(obj);
    social_send(s, f);
}

static void profile_page(SocialSession *s, PageCtx *c, const char *name) {
    char viewer[24];
    social_call(s, &P_PROFILE, KN_DB_ARGS(name, social_viewer(s, viewer)), profile_done, c);
}

/* ------------------------------------------------------------------ "r" */

void sh_page(SocialSession *s, const cJSON *f) {
    const char *page = fa_str(f, 1);
    if (!page) page = "feed";
    const cJSON *sub = fa(f, 2);
    PageCtx *c = page_ctx(page, cJSON_IsString(sub) || cJSON_IsNumber(sub) ? sub : NULL);
    if (!c) return;
    char viewer[24];

    if (!strcmp(page, "profile")) {
        const char *name = cJSON_IsString(sub) ? sub->valuestring : fa_str(f, 4);
        if (!name || !*name) {
            send_page(s, c, NULL);
            free(c);
            return;
        }
        profile_page(s, c, name);
        return;
    }
    if (!strcmp(page, "leaders")) {
        leaders_page(s, c->sub_null ? "player_score" : c->sub, fa_int(f, 6, 0));
        free(c);
        return;
    }
    if (!strcmp(page, "feed")) {
        social_call(s, &P_FEED, KN_DB_ARGS(social_viewer(s, viewer), NULL, "50", "0"), page_json_done, c);
        return;
    }
    if (!strcmp(page, "clan")) {
        if (c->sub_null) {
            send_page(s, c, NULL);
            free(c);
            return;
        }
        social_call(s, &P_CLAN_PAGE, KN_DB_ARGS(c->sub), page_json_done, c);
        return;
    }
    if (!strcmp(page, "market") || !strcmp(page, "itemsales") || !strcmp(page, "listing")) {
        free(c);
        market_page(s, page, f);
        return;
    }
    if (!strcmp(page, "map") || !strcmp(page, "mod")) {
        free(c);
        creation_page(s, page[1] == 'o', cJSON_IsString(sub) ? sub->valuestring : NULL);
        return;
    }
    if (!strcmp(page, "clanwars")) {
        send_page(s, c, cJSON_CreateObject());
        free(c);
        return;
    }
    if (!strcmp(page, "maps") || !strcmp(page, "mods")) {
        /* the lists come from the HTTP API (/api/maps, /api/mods); the page only needs the ack */
        cJSON *fr = social_frame("0");
        cJSON_AddItemToArray(fr, cJSON_CreateString(page));
        cJSON_AddItemToArray(fr, page_sub(c));
        social_send(s, fr);
        free(c);
        return;
    }
    send_page(s, c, NULL); /* search, virus, unknown pages: "page unavailable" like socialWs.ts */
    free(c);
}

/* ------------------------------------------------------------------ creations (map / mod pages) */

static void creation_done(SocialSession *s, kn_db_result *res, void *extra) { page_json_done(s, res, extra); }

void creation_page(SocialSession *s, int kind, const char *name) {
    PageCtx *c = page_ctx(kind ? "mod" : "map", NULL);
    if (!c) return;
    if (name) snprintf(c->sub, sizeof c->sub, "%s", name), c->sub_null = 0;
    if (!name) {
        send_page(s, c, NULL);
        free(c);
        return;
    }
    if (kind == 0) {
        social_call(s, &P_MAP_PAGE,
                     KN_DB_ARGS(name), creation_done, c);
    } else {
        social_call(s, &P_MOD_PAGE,
                     KN_DB_ARGS(name), creation_done, c);
    }
}

/* ------------------------------------------------------------------ search ("sp" -> "spR") */

static void search_done(SocialSession *s, kn_db_result *res, void *extra) {
    (void) extra;
    if (!kn_db_ok(res)) {
        acct_db_error(res, "search");
        social_send_n(s, "spR", 1, cJSON_CreateNull());
        return;
    }
    cJSON *rows = kn_db_result_to_json(res);
    if (!rows || cJSON_GetArraySize(rows) == 0) {
        cJSON_Delete(rows);
        rows = cJSON_CreateNull(); /* io_spR: falsy -> "none found" popup */
    }
    social_send_n(s, "spR", 1, rows);
}

void sh_search(SocialSession *s, const cJSON *f) {
    const char *q = fa_str(f, 1);
    if (!q || !*q || strlen(q) > 64) {
        social_send_n(s, "spR", 1, cJSON_CreateNull());
        return;
    }
    char pat[160];
    like_escape(q, pat, sizeof pat);
    social_query(s,
                 SQL(.pg = "SELECT player_name, player_premium, player_featured, player_hack, player_clan, clan_rank, player_score, player_id "
                           "FROM player_rows WHERE player_name ILIKE '%' || $1 || '%' "
                           "ORDER BY (lower(player_name) = lower($2)) DESC, player_score DESC LIMIT 50",
                     .any = "SELECT player_name, player_premium, player_featured, player_hack, player_clan, clan_rank, player_score, player_id "
                            "FROM player_rows WHERE player_name {ilike} '%' || $1 || '%' ESCAPE '\\' "
                            "ORDER BY (lower(player_name) = lower($2)) DESC, player_score DESC LIMIT 50"),
                 KN_DB_ARGS(pat, q), search_done, NULL);
}

/* ------------------------------------------------------------------ follows */

static void follow_done(SocialSession *s, kn_db_result *res, void *extra) {
    if (!kn_db_ok(res)) social_error(s, acct_db_error(res, "follow"));
    else if (kn_db_affected(res) > 0) social_eb_follow_changed(s->account_id, extra); /* v10 friends (social_eb.c) */
}

/* ["f", name, action]: action 1 follow, 0 unfollow (social.js followUser / instaFollo). No reply. */
void sh_follow(SocialSession *s, const cJSON *f) {
    if (!social_require_auth(s)) return;
    const char *name = fa_str(f, 1);
    if (!name) return;
    char me[24];
    social_viewer(s, me);
    if (fa_int(f, 2, 1)) {
        social_query(s,
                     SQL(.pg = "INSERT INTO follows (follower_id, followee_id) "
                               "SELECT $1, id FROM accounts WHERE lower(username) = lower($2) AND id <> $1 ON CONFLICT DO NOTHING",
                         .any = "{insert_ignore} INTO follows (follower_id, followee_id) "
                                "SELECT $1, id FROM accounts WHERE lower(username) = lower($2) AND id <> $1 {ignore_conflicts}"),
                     KN_DB_ARGS(me, name), follow_done, strdup(name));
    } else {
        social_query(s, SQL(.pg = "DELETE FROM follows WHERE follower_id = $1 AND followee_id = kn_account_id($2)",
                            .any = "DELETE FROM follows WHERE follower_id = $1 AND followee_id = "
                                   "(SELECT id FROM accounts WHERE lower(username) = lower($2))"),
                     KN_DB_ARGS(me, name), follow_done, strdup(name));
    }
}

typedef struct FollowCtx {
    char label[8];
    char name[64];
} FollowCtx;

static void follow_list_done(SocialSession *s, kn_db_result *res, void *extra) {
    FollowCtx *c = extra;
    cJSON *rows = kn_db_ok(res) ? kn_db_result_to_json(res) : cJSON_CreateArray();
    if (!kn_db_ok(res)) acct_db_error(res, c->label);
    social_send_n(s, c->label, 2, rows, cJSON_CreateString(c->name));
}

static void follow_list(SocialSession *s, const cJSON *f, int followers) {
    const char *name = fa_str(f, 1);
    FollowCtx *c = calloc(1, sizeof *c);
    if (!c) return;
    snprintf(c->label, sizeof c->label, "%s", followers ? "fwsr" : "fwr");
    snprintf(c->name, sizeof c->name, "%s", name ? name : "");
    const char *sql = followers
        ? "SELECT r.player_name AS friendname, r.player_featured AS featured, "
          "CASE WHEN r.player_premium > 0 THEN 1 ELSE 0 END AS subscription, w.created_at AS date "
          "FROM follows w JOIN player_rows r ON r.player_id = w.follower_id "
          "WHERE w.followee_id = " ACCOUNT_ID_OF_1 " ORDER BY w.created_at DESC LIMIT 500"
        : "SELECT r.player_name AS friendname, r.player_featured AS featured, "
          "CASE WHEN r.player_premium > 0 THEN 1 ELSE 0 END AS subscription, w.created_at AS date "
          "FROM follows w JOIN player_rows r ON r.player_id = w.followee_id "
          "WHERE w.follower_id = " ACCOUNT_ID_OF_1 " ORDER BY w.created_at DESC LIMIT 500";
    social_query(s, SQL(.any = sql), KN_DB_ARGS(c->name), follow_list_done, c);
}

/* ["gfs", name, id, count, captcha] -> ["fwsr", rows, name] (followers) */
void sh_followers(SocialSession *s, const cJSON *f) { follow_list(s, f, 1); }
/* ["gfg", name, id, count, captcha] -> ["fwr", rows, name] (following) */
void sh_following(SocialSession *s, const cJSON *f) { follow_list(s, f, 0); }

/* ------------------------------------------------------------------ profile tabs */

static void rows_reply_done(SocialSession *s, kn_db_result *res, void *extra) {
    const char *label = extra;
    cJSON *rows = kn_db_ok(res) ? kn_db_result_to_json(res) : NULL;
    if (!kn_db_ok(res)) acct_db_error(res, label);
    social_send_n(s, label, 1, rows ? rows : cJSON_CreateArray());
}

static void json_reply_done(SocialSession *s, kn_db_result *res, void *extra) {
    const char *label = extra;
    cJSON *v = kn_db_ok(res) ? acct_db_json(res) : NULL;
    if (!kn_db_ok(res)) acct_db_error(res, label);
    social_send_n(s, label, 1, v ? v : cJSON_CreateArray());
}

/* ["pst", name, days] -> ["psd", [{d:"YYYY-MM-DD", f:kr}]] daily market sales of the player. */
void sh_profile_stats(SocialSession *s, const cJSON *f) {
    const char *name = fa_str(f, 1);
    int64_t days = fa_int(f, 2, 30);
    if (days < 1) days = 1;
    if (days > 366) days = 366;
    char d[24];
    social_query(s,
                 SQL(.pg = "SELECT to_char(date_trunc('day', l.sold_at), 'YYYY-MM-DD') AS d, sum(l.price)::bigint AS f "
                           "FROM market_listings l WHERE l.seller_id = kn_account_id($1) AND l.sold_at IS NOT NULL "
                           "AND l.sold_at > now() - make_interval(days => $2::int) GROUP BY 1 ORDER BY 1",
                     .sqlite = "SELECT strftime('%Y-%m-%d', l.sold_at) AS d, sum(l.price) AS f__i "
                               "FROM market_listings l WHERE l.seller_id = " ACCOUNT_ID_OF_1 " AND l.sold_at IS NOT NULL "
                               "AND l.sold_at > {ago:$2 * 86400} GROUP BY 1 ORDER BY 1",
                     .mysql = "SELECT DATE_FORMAT(l.sold_at, '%Y-%m-%d') AS d, sum(l.price) AS f__i "
                              "FROM market_listings l WHERE l.seller_id = " ACCOUNT_ID_OF_1 " AND l.sold_at IS NOT NULL "
                              "AND l.sold_at > {ago:$2 * 86400} GROUP BY 1 ORDER BY 1"),
                 KN_DB_ARGS(name ? name : "", acct_itoa(d, days)), rows_reply_done, strdup("psd"));
}

/* ["guf", playerId] -> ["uf", posts] (profile activity tab) */
void sh_user_feed(SocialSession *s, const cJSON *f) {
    char viewer[24], author[24];
    const int64_t id = fa_int(f, 1, 0);
    if (id <= 0) {
        social_send_n(s, "uf", 1, cJSON_CreateArray());
        return;
    }
    social_call(s, &P_FEED, KN_DB_ARGS(social_viewer(s, viewer), acct_itoa(author, id), "50", "0"),
                 json_reply_done, strdup("uf"));
}

/* ------------------------------------------------------------------ reports and staff actions */

/* ["grp", name] -> ["rps", rows] (moderators only) */
void sh_reports(SocialSession *s, const cJSON *f) {
    if (!social_require_auth(s)) return;
    char me[24];
    social_query(s,
                 SQL(.pg = "SELECT rp.id AS rp_id, rp.created_at AS rp_date, a.username AS rp_reportername, "
                 "kn_html(rp.reason) AS rp_index, kn_html(rp.details) AS rp_text "
                 "FROM reports rp LEFT JOIN accounts a ON a.id = rp.reporter_id "
                 "WHERE rp.target_id = kn_account_id($1) "
                 "AND EXISTS (SELECT 1 FROM players p WHERE p.player_id = $2 AND (p.player_moderator > 0 OR p.player_developer > 0)) "
                 "ORDER BY rp.id DESC LIMIT 100",
                     .any = "SELECT rp.id AS rp_id, rp.created_at AS rp_date, a.username AS rp_reportername, "
                 "{html:rp.reason} AS rp_index, {html:rp.details} AS rp_text "
                 "FROM reports rp LEFT JOIN accounts a ON a.id = rp.reporter_id "
                 "WHERE rp.target_id = " ACCOUNT_ID_OF_1 " "
                 "AND EXISTS (SELECT 1 FROM players p WHERE p.player_id = $2 AND (p.player_moderator > 0 OR p.player_developer > 0)) "
                 "ORDER BY rp.id DESC LIMIT 100"),
                 KN_DB_ARGS(fa_str(f, 1) ? fa_str(f, 1) : "", social_viewer(s, me)), rows_reply_done, strdup("rps"));
}

static void report_done(SocialSession *s, kn_db_result *res, void *extra) {
    (void) extra;
    const int ok = kn_db_ok(res) && kn_db_affected(res) > 0;
    if (!kn_db_ok(res)) acct_db_error(res, "report");
    social_send_n(s, "rpr", 1, ok ? cJSON_CreateTrue() : cJSON_CreateNull());
}

/* ["rpt", name, reason, details] -> ["rpr", ok] */
void sh_report(SocialSession *s, const cJSON *f) {
    if (!social_require_auth(s)) return;
    const char *name = fa_str(f, 1), *reason = fa_str(f, 2), *details = fa_str(f, 3);
    if (!name || !reason || !details || strlen(details) > 2000 || strlen(reason) > 64) {
        social_send_n(s, "rpr", 1, cJSON_CreateNull());
        return;
    }
    char me[24];
    social_query(s,
                 SQL(.pg = "INSERT INTO reports (reporter_id, target_id, reason, details) "
                           "SELECT $1, id, $3, $4 FROM accounts WHERE lower(username) = lower($2) AND id <> $1 "
                           "AND (SELECT count(*) FROM reports WHERE reporter_id = $1 AND created_at > now() - interval '1 hour') < 20",
                     .any = "INSERT INTO reports (reporter_id, target_id, reason, details) "
                            "SELECT $1, id, $3, $4 FROM accounts WHERE lower(username) = lower($2) AND id <> $1 "
                            "AND (SELECT count(*) FROM reports WHERE reporter_id = $1 AND created_at > {ago:3600}) < 20"),
                 KN_DB_ARGS(social_viewer(s, me), name, reason, details), report_done, NULL);
}

typedef struct FlagCtx {
    char label[8];
} FlagCtx;

static void flag_done(SocialSession *s, kn_db_result *res, void *extra) {
    FlagCtx *c = extra;
    const int ok = kn_db_ok(res) && kn_db_rows(res) > 0;
    if (!kn_db_ok(res)) acct_db_error(res, c->label);
    social_send_n(s, c->label, 1, ok ? cJSON_CreateTrue() : cJSON_CreateNull());
}

/* ["flg", name, 0|1] -> ["fgr", ok] (mods/devs mark a hacker);
 * ["ver", name, 0|1] -> ["verr", ok] (devs verify). Logged in moderation_log. */
static void staff_set(SocialSession *s, const cJSON *f, int verify) {
    if (!social_require_auth(s)) return;
    FlagCtx *c = calloc(1, sizeof *c);
    if (!c) return;
    snprintf(c->label, sizeof c->label, "%s", verify ? "verr" : "fgr");
    char me[24], v[24];
    const int64_t on = fa_int(f, 2, 0) ? 1 : 0;
    social_call(s, verify ? &P_SET_FEATURED : &P_SET_HACK,
                KN_DB_ARGS(social_viewer(s, me), fa_str(f, 1) ? fa_str(f, 1) : "", acct_itoa(v, on)),
                 flag_done, c);
}

void sh_flag(SocialSession *s, const cJSON *f) { staff_set(s, f, 0); }
void sh_verify(SocialSession *s, const cJSON *f) { staff_set(s, f, 1); }
