/*
 * services/brain/db_social.c - social socket operations (db_procs.h): feed,
 * clans, mail, chat, profile / creation pages, map
 * publishing, staff flags. .pg = the statement social_data.c / profiles.c /
 * creations.c always sent (functions of postgres/0104); run() = the same in C
 * for the other drivers, returning the same shape.
 */
#include "db_procs.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define P(i) (n > (i) ? p[i] : NULL)
#define PI(i) pint(P(i))

static int64_t pint(const char *s) { return s ? strtoll(s, NULL, 10) : 0; }

static const char *nz(const char *s) { return s ? s : ""; }

static const char *kn_json_str_or(const cJSON *o, const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

/* html-escaped string item */
static cJSON *html_item(const char *s) {
    char *h = db_html(s);
    cJSON *v = cJSON_CreateString(h ? h : "");
    free(h);
    return v;
}

/* Move a member of `from` into `to` under name `as` (null when absent). */
static void take(cJSON *to, const char *as, cJSON *from, const char *key) {
    cJSON *v = cJSON_DetachItemFromObjectCaseSensitive(from, key);
    cJSON_AddItemToObject(to, as, v ? v : cJSON_CreateNull());
}

static void html_field(cJSON *o, const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    cJSON_ReplaceItemInObjectCaseSensitive(o, key, html_item(cJSON_IsString(v) ? v->valuestring : ""));
}

static const kn_sql SQL_NOW_MS = {.any = "SELECT {epoch_ms:{now}}"};

/* ------------------------------------------------------------------ feed */

/* kn_post_json(post, viewer): NULL when missing / deleted */
static cJSON *post_json(Dbx *x, int64_t post, const char *viewer, int64_t now_ms) {
    cJSON *r = dbx_row(x,
        SQL(.any = "SELECT fp.id, fp.author_id, fp.body, fp.type, fp.link, fp.link2, {epoch_ms:fp.created_at} AS cms,"
                   " r.player_name, r.player_premium, r.player_featured, r.player_clan, r.clan_rank, r.player_followed,"
                   " (SELECT count(*) FROM feed_likes l WHERE l.post_id = fp.id) AS vot,"
                   " (SELECT count(*) FROM feed_comments c WHERE c.post_id = fp.id) AS cnt,"
                   " (SELECT count(*) FROM feed_likes l WHERE l.post_id = fp.id AND l.account_id = $2) AS dlk"
                   " FROM feed_posts fp JOIN player_rows r ON r.player_id = fp.author_id"
                   " WHERE fp.id = $1 AND fp.deleted_at IS NULL"),
        KN_DB_ARGS(I64S(post), viewer));
    if (!r) return NULL;
    cJSON *o = cJSON_CreateObject();
    take(o, "pid", r, "id");
    take(o, "pln", r, "player_name");
    take(o, "pli", r, "author_id");
    take(o, "prm", r, "player_premium");
    take(o, "ftr", r, "player_featured");
    take(o, "cln", r, "player_clan");
    take(o, "clr", r, "clan_rank");
    take(o, "flw", r, "player_followed");
    cJSON_AddItemToObject(o, "txt", html_item(kn_json_str_or(r, "body")));
    take(o, "typ", r, "type");
    cJSON_AddItemToObject(o, "lnk", html_item(kn_json_str_or(r, "link")));
    cJSON_AddItemToObject(o, "lnk2", html_item(kn_json_str_or(r, "link2")));
    const cJSON *cms = cJSON_GetObjectItemCaseSensitive(r, "cms");
    cJSON_AddNumberToObject(o, "tim", (double)(now_ms - (cJSON_IsNumber(cms) ? (int64_t)cms->valuedouble : now_ms)));
    take(o, "vot", r, "vot");
    take(o, "cnt", r, "cnt");
    const cJSON *dlk = cJSON_GetObjectItemCaseSensitive(r, "dlk");
    cJSON_AddNumberToObject(o, "dlk", cJSON_IsNumber(dlk) && dlk->valuedouble > 0 ? 1 : 0);
    cJSON_Delete(r);
    return o;
}

static kn_db_result *run_post_full(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    int64_t now = dbx_int(&x, &SQL_NOW_MS, 0, NULL, 0);
    cJSON *post = post_json(&x, PI(0), P(1), now);
    if (!post) return dbx_json(&x, "kn_post_full", NULL);
    cJSON *rows = dbx_rows(&x,
        SQL(.any = "SELECT c.post_id AS pid, c.id AS cid, r.player_name AS pln, c.author_id AS pli,"
                   " r.player_premium AS prm, r.player_featured AS ftr, r.player_clan AS cln, r.clan_rank AS clr,"
                   " r.player_followed AS flw, {html:c.body} AS txt,"
                   " {epoch_ms:{now}} - {epoch_ms:c.created_at} AS tim"
                   " FROM feed_comments c JOIN player_rows r ON r.player_id = c.author_id"
                   " WHERE c.post_id = $1 ORDER BY c.id"),
        KN_DB_ARGS(P(0)));
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "p", post);
    cJSON_AddItemToObject(o, "c", rows ? rows : cJSON_CreateArray());
    return dbx_json(&x, "kn_post_full", o);
}
const kn_proc P_POST_FULL = {"post_full", "SELECT kn_post_full($1, $2)", run_post_full};

static cJSON *feed_of(Dbx *x, const kn_sql *ids_q, int n, const char *const *p, const char *viewer, int64_t now) {
    kn_db_result *ids = dbx_q(x, ids_q, n, p);
    cJSON *a = cJSON_CreateArray();
    for (int i = 0; ids && i < kn_db_rows(ids); i++) {
        cJSON *post = post_json(x, kn_db_get_int64(ids, i, 0, 0), viewer, now);
        if (post) cJSON_AddItemToArray(a, post);
    }
    kn_db_result_free(ids);
    return a;
}

/* kn_feed(viewer, author|NULL, limit, offset) */
static kn_db_result *run_feed(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    int64_t now = dbx_int(&x, &SQL_NOW_MS, 0, NULL, 0);
    const char *viewer = P(0) ? P(0) : "0";
    cJSON *a;
    if (P(1) && PI(1) > 0) {
        a = feed_of(&x, SQL(.any = "SELECT id FROM feed_posts WHERE author_id = $1 AND deleted_at IS NULL"
                                   " ORDER BY id DESC LIMIT $2 OFFSET $3"),
                    KN_DB_ARGS(P(1), P(2), P(3)), viewer, now);
        return dbx_json(&x, "kn_feed", a);
    }
    if (strtoll(viewer, NULL, 10) > 0) {
        a = feed_of(&x, SQL(.any = "SELECT fp.id FROM feed_posts fp WHERE fp.deleted_at IS NULL AND (fp.author_id = $1"
                                   " OR fp.author_id IN (SELECT followee_id FROM follows WHERE follower_id = $1))"
                                   " ORDER BY fp.id DESC LIMIT $2 OFFSET $3"),
                    KN_DB_ARGS(viewer, P(2), P(3)), viewer, now);
        if (cJSON_GetArraySize(a) > 0) return dbx_json(&x, "kn_feed", a);
        cJSON_Delete(a);
    }
    a = feed_of(&x, SQL(.any = "SELECT id FROM feed_posts WHERE deleted_at IS NULL ORDER BY id DESC LIMIT $1 OFFSET $2"),
                KN_DB_ARGS(P(2), P(3)), viewer, now);
    return dbx_json(&x, "kn_feed", a);
}
/* two call shapes on postgres (viewer only / viewer + author): the C side takes 4 params */
const kn_proc P_FEED = {"feed", "SELECT kn_feed($1, $2, $3, $4)", run_feed};

/* ------------------------------------------------------------------ clans */

static int64_t clan_of(Dbx *x, const char *player) {
    return dbx_int(x, SQL(.any = "SELECT clan_id FROM clan_members WHERE account_id = $1"), KN_DB_ARGS(player), 0);
}

static int clan_leader(Dbx *x, const char *player, int64_t clan) {
    return dbx_exists(x, SQL(.any = "SELECT 1 FROM clans WHERE clan_id = $1 AND clan_playerid = $2"),
                      KN_DB_ARGS(I64S(clan), player));
}

static int clan_officer(Dbx *x, const char *player, int64_t clan) {
    return clan_leader(x, player, clan) ||
           dbx_exists(x, SQL(.any = "SELECT 1 FROM clan_members WHERE clan_id = $1 AND account_id = $2 AND rank >= 2"),
                      KN_DB_ARGS(I64S(clan), player));
}

/* kn_clan_data: menu clan window. v10.0.2 fields too (clan_ui.c): members' s7 (score of their
 * matches in the last 7 days while in the clan), clan_link4 (the Discord invite code), the logo
 * versions, the banner colour and a pending ownership transfer (0113_clan_v10). The pg function
 * kn_clan_data of 0104 has none of these, so every clan proc runs this C version on all drivers. */
static cJSON *clan_data(Dbx *x, int64_t clan) {
    if (clan <= 0) return NULL;
    cJSON *o = dbx_row(x,
        SQL(.any = "SELECT c.clan_id, c.clan_name, c.clan_playerid, c.clan_funds, c.clan_score, c.clan_rank, c.clan_motd,"
                   " 0 AS clan_contracts, (SELECT username FROM accounts WHERE id = c.clan_playerid) AS creatorname,"
                   " c.clan_discord AS clan_link4, c.clan_logo, c.clan_logo_pending, c.clan_banner_color,"
                   " c.clan_transfer_to, (SELECT username FROM accounts WHERE id = c.clan_transfer_to) AS transfer_name"
                   " FROM clans c WHERE c.clan_id = $1"),
        KN_DB_ARGS(I64S(clan)));
    if (!o) return NULL;
    html_field(o, "clan_motd");
    cJSON *to = cJSON_DetachItemFromObjectCaseSensitive(o, "clan_transfer_to");
    cJSON *to_name = cJSON_DetachItemFromObjectCaseSensitive(o, "transfer_name");
    if (cJSON_IsNumber(to) && to->valuedouble > 0) {
        cJSON *t = cJSON_CreateObject();
        cJSON_AddNumberToObject(t, "to_player_id", to->valuedouble);
        cJSON_AddStringToObject(t, "to_player_name", cJSON_IsString(to_name) ? to_name->valuestring : "");
        const cJSON *from = cJSON_GetObjectItemCaseSensitive(o, "creatorname");
        cJSON_AddStringToObject(t, "from_player_name", cJSON_IsString(from) ? from->valuestring : "");
        const cJSON *cn = cJSON_GetObjectItemCaseSensitive(o, "clan_name");
        cJSON_AddStringToObject(t, "clan_name", cJSON_IsString(cn) ? cn->valuestring : "");
        cJSON_AddItemToObject(o, "ownership_transfer", t);
    }
    cJSON_Delete(to);
    cJSON_Delete(to_name);
    cJSON *m = dbx_rows(x,
        SQL(.any = "SELECT m.account_id AS i, r.player_name AS p, m.rank AS r, m.score AS s,"
                   " CASE WHEN r.player_premium > 0 THEN 1 ELSE 0 END AS su, r.player_featured AS ve, r.player_hack AS h,"
                   " m.kills AS k, m.deaths AS de,"
                   " (SELECT coalesce(sum(mr.score), 0) FROM match_results mr WHERE mr.account_id = m.account_id"
                   "   AND mr.at > {ago:604800} AND mr.at >= m.joined_at) AS s7"
                   " FROM clan_members m JOIN player_rows r ON r.player_id = m.account_id"
                   " WHERE m.clan_id = $1 ORDER BY m.score DESC"),
        KN_DB_ARGS(I64S(clan)));
    cJSON *q = dbx_rows(x,
        SQL(.any = "SELECT a.username AS player_name, q.account_id AS cr_playerid FROM clan_requests q"
                   " JOIN accounts a ON a.id = q.account_id WHERE q.clan_id = $1 ORDER BY q.created_at"),
        KN_DB_ARGS(I64S(clan)));
    cJSON_AddItemToObject(o, "members", m ? m : cJSON_CreateArray());
    cJSON_AddItemToObject(o, "requests", q ? q : cJSON_CreateArray());
    return o;
}

/* clan_members.rank of player in clan, -1 when not a member */
static int64_t clan_rank_of(Dbx *x, const char *player, int64_t clan) {
    return dbx_int(x, SQL(.any = "SELECT rank FROM clan_members WHERE clan_id = $1 AND account_id = $2"), KN_DB_ARGS(I64S(clan), player), -1);
}

static int valid_tag(const char *t, int64_t max) {
    size_t n = strlen(t);
    if (n < 1 || (int64_t)n > max) return 0;
    for (size_t i = 0; i < n; i++)
        if (!isalnum((unsigned char)t[i])) return 0;
    return 1;
}

static char *trim_dup(const char *s) {
    s = nz(s);
    while (*s && isspace((unsigned char)*s)) s++;
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) n--;
    char *o = malloc(n + 1);
    if (o) {
        memcpy(o, s, n);
        o[n] = 0;
    }
    return o;
}

/* (player, tag, tag_max) */
static kn_db_result *run_clan_create(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    char *tag = trim_dup(P(1));
    if (!tag || !valid_tag(tag, PI(2))) {
        free(tag);
        return dbx_finish(&x, kn_db_result_user_error("Invalid Clan Name"));
    }
    if (clan_of(&x, P(0)) > 0) dbx_refuse(&x, "Already in a Clan");
    else if (dbx_exists(&x, SQL(.any = "SELECT 1 FROM clans WHERE lower(clan_name) = lower($1)"), KN_DB_ARGS(tag)))
        dbx_refuse(&x, "Clan Name Taken");
    int64_t clan = dbx_insert_id(&x, SQL(.pg = "INSERT INTO clans (clan_name, clan_playerid) VALUES ($1, $2) RETURNING clan_id",
                                         .sqlite = "INSERT INTO clans (clan_name, clan_playerid) VALUES ($1, $2) RETURNING clan_id",
                                         .mysql = "INSERT INTO clans (clan_name, clan_playerid) VALUES ($1, $2)"),
                                 KN_DB_ARGS(tag, P(0)));
    free(tag);
    dbx_exec(&x, SQL(.any = "INSERT INTO clan_members (account_id, clan_id, rank) VALUES ($1, $2, 3)"),
             KN_DB_ARGS(P(0), I64S(clan)));
    dbx_exec(&x, SQL(.any = "DELETE FROM clan_requests WHERE account_id = $1"), KN_DB_ARGS(P(0)));
    return dbx_json(&x, "kn_clan_create", dbx_failed(&x) ? NULL : clan_data(&x, clan));
}
const kn_proc P_CLAN_CREATE = {"clan_create", NULL /* C on every driver: v10.0.2 fields and captain rights (0113_clan_v10) */, run_clan_create};

/* (player, tag) -> {clan_id, leader} */
static kn_db_result *run_clan_request(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    if (clan_of(&x, P(0)) > 0) return dbx_finish(&x, kn_db_result_user_error("Already in a Clan"));
    char *tag = trim_dup(P(1));
    kn_db_result *r = dbx_q(&x, SQL(.any = "SELECT clan_id, clan_playerid FROM clans WHERE lower(clan_name) = lower($1)"),
                            KN_DB_ARGS(tag ? tag : ""));
    free(tag);
    if (!r) return dbx_finish(&x, NULL);
    if (kn_db_rows(r) < 1) {
        kn_db_result_free(r);
        return dbx_finish(&x, kn_db_result_user_error("Clan not found"));
    }
    int64_t clan = kn_db_get_int64(r, 0, 0, 0);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "clan_id", (double)clan);
    cJSON_AddItemToObject(o, "leader", kn_db_isnull(r, 0, 1) ? cJSON_CreateNull()
                                                             : cJSON_CreateNumber((double)kn_db_get_int64(r, 0, 1, 0)));
    kn_db_result_free(r);
    dbx_exec(&x, SQL(.any = "{insert_ignore} INTO clan_requests (clan_id, account_id) VALUES ($1, $2) {ignore_conflicts}"),
             KN_DB_ARGS(I64S(clan), P(0)));
    return dbx_json(&x, "kn_clan_request", o);
}
const kn_proc P_CLAN_REQUEST = {"clan_request", NULL /* C on every driver: v10.0.2 fields and captain rights (0113_clan_v10) */, run_clan_request};

static kn_db_result *run_clan_data_of(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    return dbx_json(&x, "kn_clan_data", clan_data(&x, clan_of(&x, P(0))));
}
const kn_proc P_CLAN_DATA_OF = {"clan_data", NULL /* C on every driver: v10.0.2 fields and captain rights (0113_clan_v10) */, run_clan_data_of};

static kn_db_result *run_clan_leave(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    int64_t clan = clan_of(&x, P(0));
    if (clan <= 0) return dbx_finish(&x, kn_db_result_user_error("Not in a Clan"));
    cJSON *o = cJSON_CreateObject();
    if (clan_leader(&x, P(0), clan)) {
        dbx_exec(&x, SQL(.any = "DELETE FROM clans WHERE clan_id = $1"), KN_DB_ARGS(I64S(clan)));
        cJSON_AddNumberToObject(o, "deleted", 1);
    } else {
        dbx_exec(&x, SQL(.any = "DELETE FROM clan_members WHERE account_id = $1"), KN_DB_ARGS(P(0)));
        cJSON_AddNumberToObject(o, "deleted", 0);
    }
    return dbx_json(&x, "kn_clan_leave", o);
}
const kn_proc P_CLAN_LEAVE = {"clan_leave", NULL /* C on every driver: v10.0.2 fields and captain rights (0113_clan_v10) */, run_clan_leave};

static kn_db_result *run_clan_kick(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    int64_t clan = clan_of(&x, P(0));
    if (clan <= 0 || !clan_officer(&x, P(0), clan)) return dbx_finish(&x, kn_db_result_user_error("No Permission"));
    if (PI(1) == PI(0) || clan_leader(&x, P(1), clan)) return dbx_finish(&x, kn_db_result_user_error("Cant kick this member"));
    /* v10: a Captain (not the owner) cannot kick another Captain */
    if (!clan_leader(&x, P(0), clan) && clan_rank_of(&x, P(1), clan) >= 2)
        return dbx_finish(&x, kn_db_result_user_error("No Permission"));
    dbx_exec(&x, SQL(.any = "DELETE FROM clan_members WHERE account_id = $1 AND clan_id = $2"), KN_DB_ARGS(P(1), I64S(clan)));
    return dbx_json(&x, "kn_clan_kick", clan_data(&x, clan));
}
const kn_proc P_CLAN_KICK = {"clan_kick", NULL /* C on every driver: v10.0.2 fields and captain rights (0113_clan_v10) */, run_clan_kick};

static kn_db_result *run_clan_role(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    int64_t clan = clan_of(&x, P(0));
    /* v10 (windows[12].gen): the owner and Captains manage roles; nobody is made Commander (the
     * owner is), a Captain cannot make Captains nor change another Captain or the owner */
    if (clan <= 0 || !clan_officer(&x, P(0), clan)) return dbx_finish(&x, kn_db_result_user_error("No Permission"));
    if (PI(2) < 0 || PI(2) > 2) return dbx_finish(&x, kn_db_result_user_error("Invalid Role"));
    if (PI(1) == PI(0) || clan_leader(&x, P(1), clan)) return dbx_finish(&x, kn_db_result_user_error("No Permission"));
    if (!clan_leader(&x, P(0), clan) && (PI(2) >= 2 || clan_rank_of(&x, P(1), clan) >= 2))
        return dbx_finish(&x, kn_db_result_user_error("No Permission"));
    if (dbx_exec(&x, SQL(.any = "UPDATE clan_members SET rank = $3 WHERE account_id = $1 AND clan_id = $2"),
                 KN_DB_ARGS(P(1), I64S(clan), P(2))) == 0)
        dbx_refuse(&x, "Member not found");
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "i", (double)PI(1));
    cJSON_AddNumberToObject(o, "r", (double)PI(2));
    return dbx_json(&x, "kn_clan_role", o);
}
const kn_proc P_CLAN_ROLE = {"clan_role", NULL /* C on every driver: v10.0.2 fields and captain rights (0113_clan_v10) */, run_clan_role};

/* (actor, amount, withdraw) -> {pf, cf} */
static kn_db_result *run_clan_funds(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    int64_t clan = clan_of(&x, P(0)), amount = PI(1), pf = 0;
    if (clan <= 0) return dbx_finish(&x, kn_db_result_user_error("Not in a Clan"));
    if (amount <= 0) return dbx_finish(&x, kn_db_result_user_error("Invalid amount"));
    if (kn_store_parse_bool(P(2), 0)) {
        if (!clan_officer(&x, P(0), clan)) return dbx_finish(&x, kn_db_result_user_error("No Permission")); /* v10: Captains withdraw */
        if (dbx_exec(&x, SQL(.any = "UPDATE clans SET clan_funds = clan_funds - {int:$2} WHERE clan_id = $1 AND clan_funds >= {int:$2}"),
                     KN_DB_ARGS(I64S(clan), I64S(amount))) == 0)
            return dbx_finish(&x, kn_db_result_user_error("Not enough Clan Funds"));
        db_add_funds(&x, PI(0), amount, "clan-withdraw", I64S(clan), &pf);
    } else {
        db_add_funds(&x, PI(0), -amount, "clan-deposit", I64S(clan), &pf);
        dbx_exec(&x, SQL(.any = "UPDATE clans SET clan_funds = clan_funds + {int:$2} WHERE clan_id = $1"),
                 KN_DB_ARGS(I64S(clan), I64S(amount - amount / 10)));
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "pf", (double)pf);
    cJSON_AddNumberToObject(o, "cf", (double)dbx_int(&x, SQL(.any = "SELECT clan_funds FROM clans WHERE clan_id = $1"),
                                                     KN_DB_ARGS(I64S(clan)), 0));
    return dbx_json(&x, "kn_clan_funds", o);
}
const kn_proc P_CLAN_FUNDS = {"clan_funds", NULL /* C on every driver: v10.0.2 fields and captain rights (0113_clan_v10) */, run_clan_funds};

/* (actor, player, accept, max members) */
static kn_db_result *run_clan_answer(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    int64_t clan = clan_of(&x, P(0));
    if (clan <= 0 || !clan_officer(&x, P(0), clan)) return dbx_finish(&x, kn_db_result_user_error("No Permission"));
    if (dbx_exec(&x, SQL(.any = "DELETE FROM clan_requests WHERE clan_id = $1 AND account_id = $2"),
                 KN_DB_ARGS(I64S(clan), P(1))) == 0)
        return dbx_finish(&x, kn_db_result_user_error("Request not found"));
    if (kn_store_parse_bool(P(2), 0)) {
        if (clan_of(&x, P(1)) > 0) return dbx_finish(&x, kn_db_result_user_error("Player already in a Clan"));
        if (PI(3) > 0 && dbx_int(&x, SQL(.any = "SELECT count(*) FROM clan_members WHERE clan_id = $1"),
                                 KN_DB_ARGS(I64S(clan)), 0) >= PI(3))
            return dbx_finish(&x, kn_db_result_user_error("Clan is full"));
        dbx_exec(&x, SQL(.any = "INSERT INTO clan_members (account_id, clan_id, rank) VALUES ($1, $2, 0)"),
                 KN_DB_ARGS(P(1), I64S(clan)));
        dbx_exec(&x, SQL(.any = "DELETE FROM clan_requests WHERE account_id = $1"), KN_DB_ARGS(P(1)));
    }
    return dbx_json(&x, "kn_clan_answer", clan_data(&x, clan));
}
const kn_proc P_CLAN_ANSWER = {"clan_answer", NULL /* C on every driver: v10.0.2 fields and captain rights (0113_clan_v10) */, run_clan_answer};

/* (actor): v10 decline-all ("clnRA", windows[12].decAll) -> {declined} */
static kn_db_result *run_clan_decline_all(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    int64_t clan = clan_of(&x, P(0));
    if (clan <= 0 || !clan_officer(&x, P(0), clan)) return dbx_finish(&x, kn_db_result_user_error("No Permission"));
    const int64_t gone = dbx_exec(&x, SQL(.any = "DELETE FROM clan_requests WHERE clan_id = $1"), KN_DB_ARGS(I64S(clan)));
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "declined", (double) (gone > 0 ? gone : 0));
    return dbx_json(&x, "kn_clan_decline_all", o);
}
const kn_proc P_CLAN_DECLINE_ALL = {"clan_decline_all", NULL, run_clan_decline_all};

/* (actor, invite code or ""): v10 cln 9 (windows[12].updateDis, owner only) -> clanData */
static kn_db_result *run_clan_discord(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    int64_t clan = clan_of(&x, P(0));
    if (clan <= 0 || !clan_leader(&x, P(0), clan)) return dbx_finish(&x, kn_db_result_user_error("No Permission"));
    dbx_exec(&x, SQL(.any = "UPDATE clans SET clan_discord = nullif($2, '') WHERE clan_id = $1"), KN_DB_ARGS(I64S(clan), nz(P(1))));
    return dbx_json(&x, "kn_clan_discord", clan_data(&x, clan));
}
const kn_proc P_CLAN_DISCORD = {"clan_discord", NULL, run_clan_discord};

/* social page "clan" */
static kn_db_result *run_clan_page(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    cJSON *o = dbx_row(&x,
        SQL(.any = "SELECT c.clan_id, c.clan_name, c.clan_rank, c.clan_score,"
                   " (SELECT username FROM accounts WHERE id = c.clan_playerid) AS creatorname"
                   " FROM clans c WHERE lower(c.clan_name) = lower($1)"),
        KN_DB_ARGS(nz(P(0))));
    if (o) {
        cJSON *id = cJSON_DetachItemFromObjectCaseSensitive(o, "clan_id");
        cJSON *m = dbx_rows(&x,
            SQL(.any = "SELECT r.player_name AS p, m.rank AS r, m.score AS s,"
                       " CASE WHEN r.player_premium > 0 THEN 1 ELSE 0 END AS su, r.player_featured AS ve, r.player_hack AS h"
                       " FROM clan_members m JOIN player_rows r ON r.player_id = m.account_id"
                       " WHERE m.clan_id = $1 ORDER BY m.score DESC"),
            KN_DB_ARGS(I64S(cJSON_IsNumber(id) ? (int64_t)id->valuedouble : 0)));
        cJSON_Delete(id);
        cJSON_AddItemToObject(o, "members", m ? m : cJSON_CreateArray());
    }
    return dbx_json(&x, "kn_clan_page", o);
}
const kn_proc P_CLAN_PAGE = {"clan_page", "SELECT kn_clan_page($1)", run_clan_page};

/* ------------------------------------------------------------------ mail */

#define MAIL_READ_COLS                                                                                         \
    "SELECT id AS km_id, created_at AS km_datesent, {html:sender} AS km_playerfrom, {html:subject} AS km_subject," \
    " {html:body} AS km_text, 1 AS km_archive, {int:$3} AS km_credited FROM mail WHERE id = $2 AND account_id = $1"

/* (me, id): mark read, credit attached KR once -> mail row (no row when not mine) */
static kn_db_result *run_mail_read(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    kn_db_result *r = dbx_q(&x, SQL(.any = "SELECT read_at IS NULL AS unread, kr FROM mail WHERE id = $2 AND account_id = $1 {for_update}"),
                            KN_DB_ARGS(P(0), P(1)));
    int unread = r && kn_db_rows(r) > 0 && kn_db_get_bool(r, 0, 0, 0);
    int64_t kr = r && kn_db_rows(r) > 0 ? kn_db_get_int64(r, 0, 1, 0) : 0;
    kn_db_result_free(r);
    dbx_exec(&x, SQL(.any = "UPDATE mail SET read_at = coalesce(read_at, {now}) WHERE id = $2 AND account_id = $1"),
             KN_DB_ARGS(P(0), P(1)));
    int credited = 0;
    if (unread && kr > 0 && db_add_funds(&x, PI(0), kr, "mail", P(1), NULL) == 0) credited = 1;
    return dbx_finish(&x, dbx_q(&x, SQL(.any = MAIL_READ_COLS), KN_DB_ARGS(P(0), P(1), credited ? "1" : "0")));
}
const kn_proc P_MAIL_READ = {
    "mail_read",
    "WITH m AS (UPDATE mail SET read_at = coalesce(read_at, now()) WHERE id = $2 AND account_id = $1 "
    "  RETURNING *, (read_at = now() AND kr > 0) AS credit), "
    "k AS (SELECT kn_add_funds($1, kr, 'mail', id::text) FROM m WHERE credit) "
    "SELECT id AS km_id, created_at AS km_datesent, kn_html(sender) AS km_playerfrom, kn_html(subject) AS km_subject, "
    "kn_html(body) AS km_text, 1 AS km_archive, (SELECT count(*) FROM k) AS km_credited FROM m",
    run_mail_read};

/* ------------------------------------------------------------------ chat */

/* (me, peer): marks their messages read -> rows {fid, fna, txt, tim} newest first */
static kn_db_result *run_chat_thread(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    kn_db_result *rows = dbx_q(&x,
        SQL(.any = "SELECT m.sender_id AS fid, a.username AS fna, {html:m.body} AS txt, {epoch_ms:m.created_at} AS tim"
                   " FROM chat_messages m JOIN accounts a ON a.id = m.sender_id"
                   " WHERE (m.sender_id = $1 AND m.receiver_id = $2) OR (m.sender_id = $2 AND m.receiver_id = $1)"
                   " ORDER BY m.id DESC LIMIT 100"),
        KN_DB_ARGS(P(0), P(1)));
    dbx_exec(&x, SQL(.any = "UPDATE chat_messages SET read_at = {now} WHERE sender_id = $2 AND receiver_id = $1 AND read_at IS NULL"),
             KN_DB_ARGS(P(0), P(1)));
    return dbx_finish(&x, rows);
}
const kn_proc P_CHAT_THREAD = {
    "chat_thread",
    "WITH r AS (UPDATE chat_messages SET read_at = now() WHERE sender_id = $2 AND receiver_id = $1 AND read_at IS NULL) "
    "SELECT m.sender_id AS fid, a.username AS fna, kn_html(m.body) AS txt, "
    "floor(extract(epoch FROM m.created_at) * 1000)::bigint AS tim "
    "FROM chat_messages m JOIN accounts a ON a.id = m.sender_id "
    "WHERE (m.sender_id = $1 AND m.receiver_id = $2) OR (m.sender_id = $2 AND m.receiver_id = $1) "
    "ORDER BY m.id DESC LIMIT 100",
    run_chat_thread};

/* ------------------------------------------------------------------ profile + creations */

#define PROFILE_SQL_PG \
    "SELECT jsonb_build_object(" \
    " 'row', to_jsonb(r) - 'clan_role' - 'player_2fa' || jsonb_build_object('player_follow', 0)," \
    " 'games', coalesce((SELECT jsonb_agg(jsonb_build_object(" \
    "     'map_id', m.map_id, 'map_name', kn_html(m.map_name), 'map_date', m.map_date, 'map_info', m.map_info::text," \
    "     'map_votes', m.map_votes, 'creatorname', r.player_name, 'map_image', m.map_image, 'map_player', m.map_plays)" \
    "     ORDER BY m.map_votes DESC) FROM published_maps m WHERE m.creator_id = r.player_id AND m.deleted_at IS NULL), '[]'::jsonb)," \
    " 'mods', coalesce((SELECT jsonb_agg(jsonb_build_object(" \
    "     'mod_id', d.mod_id, 'mod_name', kn_html(d.mod_name), 'mod_date', d.mod_date, 'mod_info', d.mod_info::text," \
    "     'mod_votes', d.mod_votes, 'mod_updatecounter', d.mod_updatecounter, 'mod_image', d.mod_image)" \
    "     ORDER BY d.mod_votes DESC) FROM published_mods d WHERE d.creator_id = r.player_id AND d.deleted_at IS NULL), '[]'::jsonb)," \
    " 'assets', coalesce((SELECT jsonb_agg(jsonb_build_object(" \
    "     'id', x.id, 'nm', kn_html(x.name), 'dat', x.data::text, 'ini', x.created_at, 'plrn', r.player_name, 'siz', x.size_kb)" \
    "     ORDER BY x.id DESC) FROM published_assets x WHERE x.creator_id = r.player_id AND x.deleted_at IS NULL), '[]'::jsonb)," \
    " 'follow', CASE WHEN EXISTS (SELECT 1 FROM follows w WHERE w.follower_id = $2 AND w.followee_id = r.player_id)" \
    "     THEN 1 ELSE 0 END)" \
    " FROM player_rows r WHERE lower(r.player_name) = lower($1)"

/* (name, viewer) -> {row, games, mods, assets, follow} */
static kn_db_result *run_profile(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    cJSON *row = dbx_row(&x, SQL(.any = "SELECT * FROM player_rows WHERE lower(player_name) = lower($1)"), KN_DB_ARGS(nz(P(0))));
    if (!row) return dbx_json(&x, "jsonb_build_object", NULL);
    const cJSON *pid = cJSON_GetObjectItemCaseSensitive(row, "player_id");
    char id[24];
    dbx_i64s(id, cJSON_IsNumber(pid) ? (int64_t)pid->valuedouble : 0);
    const cJSON *pn = cJSON_GetObjectItemCaseSensitive(row, "player_name");
    const char *name = cJSON_IsString(pn) ? pn->valuestring : "";
    cJSON_DeleteItemFromObjectCaseSensitive(row, "clan_role");
    cJSON_DeleteItemFromObjectCaseSensitive(row, "player_2fa");
    cJSON_AddNumberToObject(row, "player_follow", 0);
    cJSON *games = dbx_rows(&x,
        SQL(.any = "SELECT m.map_id, {html:m.map_name} AS map_name, m.map_date, m.map_info AS map_info__t, m.map_votes,"
                   " $2 AS creatorname, m.map_image, m.map_plays AS map_player FROM published_maps m"
                   " WHERE m.creator_id = $1 AND m.deleted_at IS NULL ORDER BY m.map_votes DESC"),
        KN_DB_ARGS(id, name));
    cJSON *mods = dbx_rows(&x,
        SQL(.any = "SELECT d.mod_id, {html:d.mod_name} AS mod_name, d.mod_date, d.mod_info AS mod_info__t, d.mod_votes,"
                   " d.mod_updatecounter, d.mod_image FROM published_mods d"
                   " WHERE d.creator_id = $1 AND d.deleted_at IS NULL ORDER BY d.mod_votes DESC"),
        KN_DB_ARGS(id));
    cJSON *assets = dbx_rows(&x,
        SQL(.any = "SELECT x.id, {html:x.name} AS nm, x.data AS dat__t, x.created_at AS ini, $2 AS plrn, x.size_kb AS siz"
                   " FROM published_assets x WHERE x.creator_id = $1 AND x.deleted_at IS NULL ORDER BY x.id DESC"),
        KN_DB_ARGS(id, name));
    int follow = dbx_exists(&x, SQL(.any = "SELECT 1 FROM follows w WHERE w.follower_id = $1 AND w.followee_id = $2"),
                            KN_DB_ARGS(nz(P(1)), id));
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "row", row);
    cJSON_AddItemToObject(o, "games", games ? games : cJSON_CreateArray());
    cJSON_AddItemToObject(o, "mods", mods ? mods : cJSON_CreateArray());
    cJSON_AddItemToObject(o, "assets", assets ? assets : cJSON_CreateArray());
    cJSON_AddNumberToObject(o, "follow", follow);
    return dbx_json(&x, "jsonb_build_object", o);
}
const kn_proc P_PROFILE = {"profile", PROFILE_SQL_PG, run_profile};

static kn_db_result *run_map_page(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    cJSON *o = dbx_row(&x,
        SQL(.any = "SELECT m.map_id, {html:m.map_name} AS map_name, a.username AS map_creator, m.map_info AS mi__t,"
                   " m.map_updatecounter, m.map_image, m.map_votes AS vt, m.map_plays AS pl,"
                   " {html:m.map_description} AS map_description, m.map_category AS ca, m.map_initialdate, m.map_date,"
                   " m.map_featured, m.map_video AS vl FROM published_maps m LEFT JOIN accounts a ON a.id = m.creator_id"
                   " WHERE lower(m.map_name) = lower($1) AND m.deleted_at IS NULL"),
        KN_DB_ARGS(nz(P(0))));
    return dbx_json(&x, "jsonb_build_object", o);
}
const kn_proc P_MAP_PAGE = {
    "map_page",
    "SELECT jsonb_build_object('map_id', m.map_id, 'map_name', kn_html(m.map_name), "
    "'map_creator', a.username, 'mi', m.map_info::text, 'map_updatecounter', m.map_updatecounter, "
    "'map_image', m.map_image, 'vt', m.map_votes, 'pl', m.map_plays, "
    "'map_description', kn_html(m.map_description), 'ca', m.map_category, "
    "'map_initialdate', m.map_initialdate, 'map_date', m.map_date, 'map_featured', m.map_featured, "
    "'vl', m.map_video) FROM published_maps m LEFT JOIN accounts a ON a.id = m.creator_id "
    "WHERE lower(m.map_name) = lower($1) AND m.deleted_at IS NULL",
    run_map_page};

static kn_db_result *run_mod_page(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    cJSON *o = dbx_row(&x,
        SQL(.any = "SELECT d.mod_id, {html:d.mod_name} AS mod_name, a.username AS mod_creator, d.mod_info AS mod_info__t,"
                   " d.mod_updatecounter, d.mod_image, d.mod_votes AS vt, {html:d.mod_description} AS mod_description,"
                   " d.mod_initialdate, d.mod_date, d.mod_featured, d.mod_url, d.mod_video AS vl"
                   " FROM published_mods d LEFT JOIN accounts a ON a.id = d.creator_id"
                   " WHERE lower(d.mod_name) = lower($1) AND d.deleted_at IS NULL"),
        KN_DB_ARGS(nz(P(0))));
    return dbx_json(&x, "jsonb_build_object", o);
}
const kn_proc P_MOD_PAGE = {
    "mod_page",
    "SELECT jsonb_build_object('mod_id', d.mod_id, 'mod_name', kn_html(d.mod_name), "
    "'mod_creator', a.username, 'mod_info', d.mod_info::text, 'mod_updatecounter', d.mod_updatecounter, "
    "'mod_image', d.mod_image, 'vt', d.mod_votes, 'mod_description', kn_html(d.mod_description), "
    "'mod_initialdate', d.mod_initialdate, 'mod_date', d.mod_date, 'mod_featured', d.mod_featured, "
    "'mod_url', d.mod_url, 'vl', d.mod_video) FROM published_mods d LEFT JOIN accounts a ON a.id = d.creator_id "
    "WHERE lower(d.mod_name) = lower($1) AND d.deleted_at IS NULL",
    run_mod_page};

/* (me, name, data, desc, video, category, thumb) -> {updated, id, taken} */
static kn_db_result *run_publish_map(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const char *video = P(4) && *P(4) ? P(4) : NULL;
    int64_t up = dbx_exec(&x,
        SQL(.any = "UPDATE published_maps SET map_data = $3, map_description = coalesce($4, ''), map_video = $5,"
                   " map_category = $6, map_image = coalesce($7, map_image), map_updatecounter = map_updatecounter + 1,"
                   " map_date = {now} WHERE lower(map_name) = lower($2) AND deleted_at IS NULL AND creator_id = $1"),
        7, (const char *const[]){P(0), P(1), P(2), P(3), video, P(5), P(6)});
    int64_t id = 0;
    if (up > 0) {
        id = dbx_int(&x, SQL(.any = "SELECT map_id FROM published_maps WHERE lower(map_name) = lower($1) AND deleted_at IS NULL"),
                     KN_DB_ARGS(P(1)), 0);
    } else if (up == 0) {
        if (dbx_exists(&x, SQL(.any = "SELECT 1 FROM published_maps WHERE lower(map_name) = lower($1) AND deleted_at IS NULL"),
                       KN_DB_ARGS(P(1))))
            return dbx_finish(&x, kn_db_result_user_error("Map name taken"));
        id = dbx_insert_id(&x,
            SQL(.any = "INSERT INTO published_maps (map_name, creator_id, map_data, map_description, map_video, map_category,"
                       " map_image) VALUES ($2, $1, $3, coalesce($4, ''), $5, $6, $7)"),
            7, (const char *const[]){P(0), P(1), P(2), P(3), video, P(5), P(6)});
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "updated", up > 0 ? 1 : 0);
    cJSON_AddNumberToObject(o, "id", (double)id);
    cJSON_AddNullToObject(o, "taken");
    return dbx_json(&x, "jsonb_build_object", o);
}
const kn_proc P_PUBLISH_MAP = {
    "publish_map",
    "WITH up AS (UPDATE published_maps SET map_data = $3, map_description = coalesce($4, ''), map_video = nullif($5, ''), "
    "  map_category = $6, map_image = coalesce($7, map_image), map_updatecounter = map_updatecounter + 1, map_date = now() "
    "  WHERE lower(map_name) = lower($2) AND deleted_at IS NULL AND creator_id = $1 RETURNING map_id), "
    "taken AS (SELECT 1 FROM published_maps WHERE lower(map_name) = lower($2) AND deleted_at IS NULL AND creator_id IS DISTINCT FROM $1), "
    "ins AS (INSERT INTO published_maps (map_name, creator_id, map_data, map_description, map_video, map_category, map_image) "
    "  SELECT $2, $1, $3, coalesce($4, ''), nullif($5, ''), $6, $7 "
    "  WHERE NOT EXISTS (SELECT 1 FROM up) AND NOT EXISTS (SELECT 1 FROM taken) RETURNING map_id) "
    "SELECT jsonb_build_object('updated', (SELECT count(*) FROM up), "
    "'id', coalesce((SELECT map_id FROM up), (SELECT map_id FROM ins)), "
    "'taken', CASE WHEN EXISTS (SELECT 1 FROM taken) THEN kn_fail('Map name taken')::text END)",
    run_publish_map};

/* staff flags: (actor, name, on) -> player_id row when changed */
static kn_db_result *staff_set(kn_dbconn *c, int n, const char *const *p, int verify) {
    Dbx x = {c, NULL};
    int allowed = verify ? dbx_exists(&x, SQL(.any = "SELECT 1 FROM players WHERE player_id = $1 AND player_developer > 0"),
                                      KN_DB_ARGS(P(0)))
                         : dbx_exists(&x, SQL(.any = "SELECT 1 FROM players WHERE player_id = $1"
                                                     " AND (player_developer > 0 OR player_moderator > 0)"),
                                      KN_DB_ARGS(P(0)));
    int64_t target = allowed ? db_account_id(&x, nz(P(1))) : 0;
    const char *names[1] = {"player_id"};
    const kn_db_type types[1] = {KN_DB_T_INT};
    kn_db_result *r = kn_db_result_new(1, names, types);
    if (target > 0 &&
        dbx_exec(&x, verify ? SQL(.any = "UPDATE players SET player_featured = $2 WHERE player_id = $1")
                            : SQL(.any = "UPDATE players SET player_hack = $2 WHERE player_id = $1"),
                 KN_DB_ARGS(I64S(target), P(2))) > 0) {
        const char *action = verify ? (PI(2) == 1 ? "verify" : "unverify") : (PI(2) == 1 ? "flag" : "unflag");
        dbx_exec(&x, SQL(.any = "INSERT INTO moderation_log (actor_id, target_id, action) VALUES ($1, $2, $3)"),
                 KN_DB_ARGS(P(0), I64S(target), action));
        const char *row[1] = {I64S(target)};
        if (r) kn_db_result_add_row(r, row);
    }
    return dbx_finish(&x, r);
}
static kn_db_result *run_set_hack(kn_dbconn *c, int n, const char *const *p) { return staff_set(c, n, p, 0); }
static kn_db_result *run_set_featured(kn_dbconn *c, int n, const char *const *p) { return staff_set(c, n, p, 1); }
const kn_proc P_SET_HACK = {
    "set_hack",
    "WITH ok AS (SELECT 1 FROM players WHERE player_id = $1 AND (player_developer > 0 OR player_moderator > 0)), "
    "u AS (UPDATE players SET player_hack = $3 WHERE player_id = kn_account_id($2) AND EXISTS (SELECT 1 FROM ok) RETURNING player_id), "
    "l AS (INSERT INTO moderation_log (actor_id, target_id, action) SELECT $1, player_id, "
    "CASE WHEN $3::int = 1 THEN 'flag' ELSE 'unflag' END FROM u) SELECT player_id FROM u",
    run_set_hack};
const kn_proc P_SET_FEATURED = {
    "set_featured",
    "WITH ok AS (SELECT 1 FROM players WHERE player_id = $1 AND player_developer > 0), "
    "u AS (UPDATE players SET player_featured = $3 WHERE player_id = kn_account_id($2) AND EXISTS (SELECT 1 FROM ok) RETURNING player_id), "
    "l AS (INSERT INTO moderation_log (actor_id, target_id, action) SELECT $1, player_id, "
    "CASE WHEN $3::int = 1 THEN 'verify' ELSE 'unverify' END FROM u) SELECT player_id FROM u",
    run_set_featured};
