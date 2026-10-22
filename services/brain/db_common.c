/*
 * services/brain/db_common.c - helpers for the C kn_procs (db_procs.h): the
 * synchronous Dbx query wrappers and the C versions of the shared SQL
 * functions of postgres/0103-0104 (kn_html, kn_level, kn_active_ban,
 * kn_add_funds, kn_add_skin, kn_account_json, kn_account_id).
 */
#include "db_procs.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "junk.h"
#include "knsvc/loop.h"
#include "v10_paint.h"

/* ------------------------------------------------------------------ Dbx */

const char *dbx_i64s(char buf[24], int64_t v) {
    snprintf(buf, 24, "%lld", (long long)v);
    return buf;
}

int dbx_failed(const Dbx *x) { return x->err != NULL; }
int dbx_is_pg(const Dbx *x) { return !strcmp(kn_dbconn_dialect(x->c), "postgres"); }
int dbx_is_mysql(const Dbx *x) { return !strcmp(kn_dbconn_dialect(x->c), "mysql"); }

kn_db_result *dbx_q(Dbx *x, const kn_sql *q, int n, const char *const *p) {
    if (x->err) return NULL;
    return kn_dbconn_try(x->c, q, n, p, &x->err);
}

int64_t dbx_exec(Dbx *x, const kn_sql *q, int n, const char *const *p) {
    kn_db_result *r = dbx_q(x, q, n, p);
    if (!r) return -1;
    int64_t a = kn_db_affected(r);
    kn_db_result_free(r);
    return a;
}

int64_t dbx_insert_id(Dbx *x, const kn_sql *q, int n, const char *const *p) {
    kn_db_result *r = dbx_q(x, q, n, p);
    if (!r) return -1;
    /* INSERT .. RETURNING id (postgres / sqlite texts) gives a row; mysql the auto-increment id */
    int64_t id = kn_db_rows(r) > 0 ? kn_db_get_int64(r, 0, 0, 0) : kn_db_last_id(r);
    kn_db_result_free(r);
    return id;
}

int64_t dbx_int(Dbx *x, const kn_sql *q, int n, const char *const *p, int64_t def) {
    kn_db_result *r = dbx_q(x, q, n, p);
    if (!r) return def;
    int64_t v = kn_db_rows(r) > 0 ? kn_db_get_int64(r, 0, 0, def) : def;
    kn_db_result_free(r);
    return v;
}

char *dbx_text(Dbx *x, const kn_sql *q, int n, const char *const *p) {
    kn_db_result *r = dbx_q(x, q, n, p);
    if (!r) return NULL;
    char *s = kn_db_rows(r) > 0 && !kn_db_isnull(r, 0, 0) ? strdup(kn_db_get(r, 0, 0)) : NULL;
    kn_db_result_free(r);
    return s;
}

int dbx_exists(Dbx *x, const kn_sql *q, int n, const char *const *p) {
    kn_db_result *r = dbx_q(x, q, n, p);
    if (!r) return 0;
    int yes = kn_db_rows(r) > 0;
    kn_db_result_free(r);
    return yes;
}

int dbx_refuse(Dbx *x, const char *msg) {
    if (!x->err) x->err = kn_db_result_user_error(msg);
    return -1;
}

kn_db_result *dbx_finish(Dbx *x, kn_db_result *res) {
    if (x->err) {
        kn_db_result_free(res);
        kn_db_result *e = x->err;
        x->err = NULL;
        return e;
    }
    return res ? res : kn_db_result_error("53200", "out of memory");
}

kn_db_result *dbx_json(Dbx *x, const char *name, cJSON *v) {
    if (x->err) {
        cJSON_Delete(v);
        return dbx_finish(x, NULL);
    }
    return kn_db_result_json(name, v);
}

cJSON *dbx_rows(Dbx *x, const kn_sql *q, int n, const char *const *p) {
    kn_db_result *r = dbx_q(x, q, n, p);
    if (!r) return NULL;
    cJSON *a = kn_db_result_to_json(r);
    kn_db_result_free(r);
    return a;
}

cJSON *dbx_row(Dbx *x, const kn_sql *q, int n, const char *const *p) {
    kn_db_result *r = dbx_q(x, q, n, p);
    if (!r) return NULL;
    cJSON *o = kn_db_rows(r) > 0 ? kn_db_row_to_json(r, 0) : NULL;
    kn_db_result_free(r);
    return o;
}

/* ------------------------------------------------------------------ small SQL functions */

char *db_html(const char *s) {
    if (!s) s = "";
    size_t n = 0;
    for (const char *p = s; *p; p++) {
        switch (*p) {
        case '&': n += 5; break;
        case '<': case '>': n += 4; break;
        case '"': n += 6; break;
        case '\'': n += 5; break;
        default: n++;
        }
    }
    char *o = malloc(n + 1), *w = o;
    if (!o) return NULL;
    for (const char *p = s; *p; p++) {
        const char *rep = NULL;
        switch (*p) {
        case '&': rep = "&amp;"; break;
        case '<': rep = "&lt;"; break;
        case '>': rep = "&gt;"; break;
        case '"': rep = "&quot;"; break;
        case '\'': rep = "&#39;"; break;
        default: *w++ = *p; continue;
        }
        size_t l = strlen(rep);
        memcpy(w, rep, l);
        w += l;
    }
    *w = 0;
    return o;
}

int db_level(int64_t score, double rank_var) {
    int l = (int)floor(rank_var * sqrt((double)(score > 0 ? score : 0)));
    return l < 1 ? 1 : l;
}

int64_t db_account_id(Dbx *x, const char *name) {
    return dbx_int(x, SQL(.any = "SELECT id FROM accounts WHERE lower(username) = lower($1)"), KN_DB_ARGS(name), 0);
}

int db_active_ban(Dbx *x, int64_t account_id, const char *ip, char *reason, size_t reason_len, int64_t *expires_s) {
    if (reason && reason_len) reason[0] = 0;
    if (expires_s) *expires_s = 0;
    if (account_id <= 0 && !ip) return 0;
    kn_db_result *r = dbx_q(x,
        SQL(.any = "SELECT reason, {epoch_ms:expires_at} AS exp_ms FROM bans WHERE revoked_at IS NULL"
                   " AND (expires_at IS NULL OR expires_at > {now})"
                   " AND ((account_id = $1 AND $1 > 0) OR (ip = $2))"
                   " ORDER BY (expires_at IS NULL) DESC, expires_at DESC LIMIT 1"),
        KN_DB_ARGS(I64S(account_id > 0 ? account_id : 0), ip));
    if (!r) return 0;
    int found = kn_db_rows(r) > 0;
    if (found) {
        if (reason && reason_len) snprintf(reason, reason_len, "%s", kn_db_get(r, 0, 0));
        if (expires_s) *expires_s = kn_db_isnull(r, 0, 1) ? 0 : kn_db_get_int64(r, 0, 1, 0) / 1000;
    }
    kn_db_result_free(r);
    return found;
}

int db_add_funds(Dbx *x, int64_t id, int64_t amount, const char *source, const char *ref, int64_t *funds) {
    return db_add_funds_ex(x, id, amount, source, ref, 0, 0, funds);
}

/* One KR movement: the balance, bounded below by 0, and its ledger row (reason, reference,
 * counterparty, fee). The row is written in the same transaction as the balance, so a rollback
 * takes both back and the ledger can never disagree with player_funds. */
int db_add_funds_ex(Dbx *x, int64_t id, int64_t amount, const char *source, const char *ref,
                    int64_t counterparty, int64_t fee, int64_t *funds) {
    int64_t n = dbx_exec(x,
        SQL(.any = "UPDATE players SET player_funds = player_funds + {int:$2}, updated_at = {now}"
                   " WHERE player_id = $1 AND player_funds + {int:$2} >= 0"),
        KN_DB_ARGS(I64S(id), I64S(amount)));
    if (n < 0) return -1;
    if (n == 0) {
        int exists = dbx_exists(x, SQL(.any = "SELECT 1 FROM players WHERE player_id = $1"), KN_DB_ARGS(I64S(id)));
        return dbx_refuse(x, exists ? "Not enough KR" : "Account not found.");
    }
    /* Movements without a counterparty or fee (rewards, turf, challenges, ...) write the pre-0111
     * row, so they never depend on 0111's columns (a brain started before its migration keeps
     * paying them). */
    if (amount != 0 && !counterparty && !fee &&
        dbx_exec(x, SQL(.any = "INSERT INTO kr_transactions (account_id, amount, source, ref) VALUES ($1, $2, $3, $4)"),
                 KN_DB_ARGS(I64S(id), I64S(amount), source, ref)) < 0)
        return -1;
    if (amount != 0 && (counterparty || fee) &&
        dbx_exec(x, SQL(.any = "INSERT INTO kr_transactions (account_id, amount, source, ref, counterparty_id, fee)"
                              " VALUES ($1, $2, $3, $4, NULLIF($5, 0), {int:$6})"),
                 KN_DB_ARGS(I64S(id), I64S(amount), source, ref, I64S(counterparty), I64S(fee))) < 0)
        return -1;
    if (funds)
        *funds = dbx_int(x, SQL(.any = "SELECT player_funds FROM players WHERE player_id = $1"), KN_DB_ARGS(I64S(id)), 0);
    return dbx_failed(x) ? -1 : 0;
}

int db_add_skin(Dbx *x, int64_t id, int64_t skin, int64_t delta, const char *source, const char *ref, int64_t *count) {
    if (delta >= 0) {
        if (dbx_exec(x,
                SQL(.sqlite = "INSERT INTO inventory (account_id, skin_id, count) VALUES ($1, $2, $3)"
                              " ON CONFLICT (account_id, skin_id) DO UPDATE SET count = count + excluded.count",
                    .mysql = "INSERT INTO inventory (account_id, skin_id, count) VALUES ($1, $2, $3)"
                             " ON DUPLICATE KEY UPDATE count = count + VALUES(count)",
                    .pg = "INSERT INTO inventory (account_id, skin_id, count) VALUES ($1::int, $2::int, $3::int)"
                           " ON CONFLICT (account_id, skin_id) DO UPDATE SET count = inventory.count + EXCLUDED.count"),
                KN_DB_ARGS(I64S(id), I64S(skin), I64S(delta))) < 0)
            return -1;
    } else {
        /* a painted unit (skin_copies, 0352) is not sold, listed, traded or crafted: only count - painted copies go */
        int64_t n = dbx_exec(x,
            SQL(.any = "UPDATE inventory SET count = count + {int:$3}"
                       " WHERE account_id = $1 AND skin_id = $2 AND count + {int:$3} >= 0"
                       " AND count + {int:$3} >= (SELECT count(*) FROM skin_copies c"
                       " WHERE c.account_id = inventory.account_id AND c.skin_id = inventory.skin_id)"),
            KN_DB_ARGS(I64S(id), I64S(skin), I64S(delta)));
        if (n < 0) return -1;
        if (n == 0) {
            const int64_t have = dbx_int(x, SQL(.any = "SELECT count FROM inventory WHERE account_id = $1 AND skin_id = $2"),
                                         KN_DB_ARGS(I64S(id), I64S(skin)), 0);
            return dbx_refuse(x, have + delta >= 0 ? "Only painted copies of that item are left" : "Item not owned");
        }
    }
    if (delta != 0 &&
        dbx_exec(x,
                 SQL(.any = "INSERT INTO skin_transactions (account_id, skin_id, delta, source, ref) VALUES ($1, $2, $3, $4, $5)"),
                 KN_DB_ARGS(I64S(id), I64S(skin), I64S(delta), source, ref)) < 0)
        return -1;
    if (count)
        *count = dbx_int(x, SQL(.any = "SELECT count FROM inventory WHERE account_id = $1 AND skin_id = $2"),
                         KN_DB_ARGS(I64S(id), I64S(skin)), 0);
    return dbx_failed(x) ? -1 : 0;
}

/* ------------------------------------------------------------------ painted copies (doc/v10/paint-copy.md) */

int64_t db_unpainted_units(Dbx *x, int64_t acc, int64_t skin) {
    return dbx_int(x,
        SQL(.any = "SELECT i.count - (SELECT count(*) FROM skin_copies c WHERE c.account_id = i.account_id"
                   " AND c.skin_id = i.skin_id) FROM inventory i WHERE i.account_id = $1 AND i.skin_id = $2"),
        KN_DB_ARGS(I64S(acc), I64S(skin)), 0);
}

int copy_take(Dbx *x, int64_t acc, int64_t copy, int64_t skin, CopyPaint *paint, const char *source, const char *ref) {
    kn_db_result *r = dbx_q(x,
        SQL(.any = "SELECT paint_color, paint_shader FROM skin_copies WHERE copy_id = $1 AND account_id = $2"
                   " AND skin_id = $3 {for_update}"),
        KN_DB_ARGS(I64S(copy), I64S(acc), I64S(skin)));
    if (!r) return -1;
    if (kn_db_rows(r) < 1) {
        kn_db_result_free(r);
        return dbx_refuse(x, "You don't own that copy.");
    }
    CopyPaint p;
    memset(&p, 0, sizeof p);
    p.shader = -1;
    if (!kn_db_isnull(r, 0, 0)) snprintf(p.color, sizeof p.color, "%s", kn_db_get(r, 0, 0));
    if (!kn_db_isnull(r, 0, 1)) p.shader = kn_db_get_int64(r, 0, 1, 0);
    kn_db_result_free(r);
    /* the copy row goes first: db_add_skin's guard then sees one painted unit fewer */
    if (dbx_exec(x, SQL(.any = "DELETE FROM skin_copies WHERE copy_id = $1"), KN_DB_ARGS(I64S(copy))) < 0) return -1;
    if (db_add_skin(x, acc, skin, -1, source, ref, NULL) < 0) return -1;
    if (paint) *paint = p;
    return 0;
}

int copy_give(Dbx *x, int64_t acc, int64_t skin, const CopyPaint *paint, const char *source, const char *ref, int64_t *copy_id) {
    if (db_add_skin(x, acc, skin, 1, source, ref, NULL) < 0) return -1;
    const int colour = paint->color[0] != '\0';
    char shader_t[24];
    if (!colour) snprintf(shader_t, sizeof shader_t, "%lld", (long long) paint->shader);
    const int64_t id = dbx_insert_id(x,
        SQL(.pg = "INSERT INTO skin_copies (account_id, skin_id, paint_color, paint_shader) VALUES ($1::int, $2::int, $3, $4::int)"
                  " RETURNING copy_id",
            .sqlite = "INSERT INTO skin_copies (account_id, skin_id, paint_color, paint_shader) VALUES ($1, $2, $3, $4)"
                      " RETURNING copy_id",
            .mysql = "INSERT INTO skin_copies (account_id, skin_id, paint_color, paint_shader) VALUES ($1, $2, $3, $4)"),
        KN_DB_ARGS(I64S(acc), I64S(skin), colour ? paint->color : NULL, colour ? NULL : shader_t));
    if (id < 0) return -1;
    if (copy_id) *copy_id = id;
    return 0;
}

int db_take_units(Dbx *x, int64_t acc, int64_t skin, int64_t want, const char *source, const char *ref) {
    const int64_t unpainted = db_unpainted_units(x, acc, skin);
    const int64_t from_stack = want < unpainted ? want : unpainted;
    if (from_stack > 0 && db_add_skin(x, acc, skin, -from_stack, source, ref, NULL) < 0) return -1;
    int64_t left = want - from_stack;
    if (left <= 0) return 0;
    kn_db_result *r = dbx_q(x, SQL(.any = "SELECT copy_id FROM skin_copies WHERE account_id = $1 AND skin_id = $2 ORDER BY copy_id"),
                            KN_DB_ARGS(I64S(acc), I64S(skin)));
    if (!r) return -1;
    for (int i = 0; i < kn_db_rows(r) && left > 0; i++) {
        if (copy_take(x, acc, kn_db_get_int64(r, i, 0, 0), skin, NULL, source, ref) < 0) break;
        left--;
    }
    kn_db_result_free(r);
    if (dbx_failed(x)) return -1;
    return left > 0 ? dbx_refuse(x, "Item not owned") : 0;
}

/* ------------------------------------------------------------------ account object */

cJSON *db_player_row(Dbx *x, int64_t id) {
    return dbx_row(x, SQL(.any = "SELECT * FROM player_rows WHERE player_id = $1"), KN_DB_ARGS(I64S(id)));
}

static cJSON *dup_item(const cJSON *o, const char *k) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return v ? cJSON_Duplicate(v, 1) : cJSON_CreateNull();
}

static double num(const cJSON *o, const char *k) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsNumber(v)) return v->valuedouble;
    if (cJSON_IsBool(v)) return cJSON_IsTrue(v);
    if (cJSON_IsString(v)) return strtod(v->valuestring, NULL);
    return 0;
}

static cJSON *num_item(const cJSON *o, const char *k) { return cJSON_CreateNumber(num(o, k)); }

/* coalesce(nullif(btrim(alias), ''), name) */
static cJSON *display_name(const cJSON *r) {
    const cJSON *a = cJSON_GetObjectItemCaseSensitive(r, "player_alias");
    if (cJSON_IsString(a)) {
        const char *s = a->valuestring;
        while (*s == ' ') s++;
        size_t n = strlen(s);
        while (n && s[n - 1] == ' ') n--;
        if (n) {
            char buf[128];
            snprintf(buf, sizeof buf, "%.*s", (int)n, s);
            return cJSON_CreateString(buf);
        }
    }
    return dup_item(r, "player_name");
}

/* The owned list (v10.0.3's $account.skins, the account's data[10]), with the paints of the copies
 * (doc/v10/paint-copy.md): one {ind, cnt} row per skin with unpainted units (cnt = those units), then one
 * {ind, cnt: 1, id, m} row per painted copy (id = its copy id, m = {paint} or {paintShader}). NULL on a DB error. */
cJSON *db_owned_skins(Dbx *x, int64_t id) {
    cJSON *rows = dbx_rows(x,
        SQL(.any = "SELECT i.skin_id AS ind, i.count - (SELECT count(*) FROM skin_copies c"
                   " WHERE c.account_id = i.account_id AND c.skin_id = i.skin_id) AS cnt"
                   " FROM inventory i WHERE i.account_id = $1 AND i.count > (SELECT count(*) FROM skin_copies c"
                   " WHERE c.account_id = i.account_id AND c.skin_id = i.skin_id) ORDER BY i.skin_id"),
        KN_DB_ARGS(I64S(id)));
    if (dbx_failed(x)) {
        cJSON_Delete(rows);
        return NULL;
    }
    cJSON *copies = dbx_rows(x,
        SQL(.any = "SELECT copy_id AS id, skin_id AS ind, paint_color, paint_shader FROM skin_copies"
                   " WHERE account_id = $1 ORDER BY copy_id"),
        KN_DB_ARGS(I64S(id)));
    if (dbx_failed(x)) {
        cJSON_Delete(rows);
        cJSON_Delete(copies);
        return NULL;
    }
    cJSON *stack;
    cJSON_ArrayForEach(stack, rows) cJSON_AddNumberToObject(stack, "st", 1); /* official addSkin: a stack is st 1 */
    cJSON *row;
    cJSON_ArrayForEach(row, copies) {
        cJSON *c = cJSON_CreateObject();
        cJSON_AddNumberToObject(c, "ind", (double) kn_json_int(row, "ind", 0));
        cJSON_AddNumberToObject(c, "cnt", 1);
        cJSON_AddNumberToObject(c, "st", 0); /* a copy is st 0 (addSkin / setSkin, deob-10.0.3.js:129099-129105) */
        cJSON_AddNumberToObject(c, "id", (double) kn_json_int(row, "id", 0));
        cJSON *m = v10_paint_meta(kn_json_str(row, "paint_color", ""), (int) kn_json_int(row, "paint_shader", -1));
        if (m) cJSON_AddItemToObject(c, "m", m);
        cJSON_AddItemToArray(rows, c);
    }
    cJSON_Delete(copies);
    return rows;
}

cJSON *db_account_json(Dbx *x, int64_t id) {
    if (dbx_is_pg(x)) {
        char *s = dbx_text(x, SQL(.pg = "SELECT kn_account_json($1::int)::text"), KN_DB_ARGS(I64S(id)));
        cJSON *v = s ? cJSON_Parse(s) : NULL;
        free(s);
        /* the SQL's data[10] counts every unit: the owned list (painted copies apart) is built here, for every driver */
        cJSON *data = v ? cJSON_GetObjectItemCaseSensitive(v, "data") : NULL;
        cJSON *owned = data && cJSON_GetArraySize(data) > 10 ? db_owned_skins(x, id) : NULL;
        if (owned) cJSON_ReplaceItemInArray(data, 10, owned);
        else cJSON_Delete(owned);
        return v;
    }
    cJSON *r = db_player_row(x, id);
    if (!r) return NULL;
    char reason[256];
    int64_t expires = 0;
    int banned = db_active_ban(x, id, NULL, reason, sizeof reason, &expires);
    cJSON *inv = db_owned_skins(x, id);
    if (!inv) {
        cJSON_Delete(r);
        return NULL;
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "id", num_item(r, "player_id"));
    cJSON_AddItemToObject(o, "username", dup_item(r, "player_name"));
    cJSON_AddItemToObject(o, "alias", display_name(r));
    cJSON_AddItemToObject(o, "score", num_item(r, "player_score"));
    cJSON_AddItemToObject(o, "funds", num_item(r, "player_funds"));
    cJSON_AddItemToObject(o, "clan", dup_item(r, "player_clan"));
    cJSON_AddItemToObject(o, "clan_rank", num_item(r, "clan_rank"));
    cJSON_AddItemToObject(o, "clan_role", dup_item(r, "clan_role"));
    cJSON_AddItemToObject(o, "premium", num_item(r, "player_premium"));
    cJSON_AddItemToObject(o, "dev", num_item(r, "player_developer"));
    cJSON_AddItemToObject(o, "mod", num_item(r, "player_moderator"));
    cJSON_AddItemToObject(o, "hack", num_item(r, "player_hack"));
    cJSON_AddItemToObject(o, "featured", num_item(r, "player_featured"));
    /* 0210_creator_badge: v10's creator badge (players.creator_badge; not in player_rows) */
    cJSON_AddNumberToObject(o, "creator_badge",
        (double) dbx_int(x, SQL(.any = "SELECT creator_badge FROM players WHERE player_id = $1"), KN_DB_ARGS(I64S(id)), 0));
    /* 0347_junk_wallet: the Junk balance (hundredths in players.player_junk), as the decimal text "12.34" */
    char junk_text[24];
    junk_format(junk_balance(x, id), junk_text, sizeof junk_text);
    cJSON_AddStringToObject(o, "junk", junk_text);
    cJSON_AddItemToObject(o, "elo", num_item(r, "player_elo"));
    cJSON_AddItemToObject(o, "elo2", num_item(r, "player_elo2"));
    cJSON_AddItemToObject(o, "elo4", num_item(r, "player_elo4"));
    cJSON_AddItemToObject(o, "chal", num_item(r, "player_chal"));
    cJSON_AddBoolToObject(o, "banned", banned);
    cJSON_AddItemToObject(o, "ban_reason", banned ? cJSON_CreateString(reason) : cJSON_CreateNull());
    cJSON_AddNumberToObject(o, "ban_expires", (double)expires);
    /* Account.getData() of src/data/account.js: 40 slots, same order as kn_account_json */
    cJSON *d = cJSON_CreateArray();
    const cJSON *stats_text = cJSON_GetObjectItemCaseSensitive(r, "player_stats");
    cJSON *stats = cJSON_IsString(stats_text) ? cJSON_Parse(stats_text->valuestring) : NULL;
    cJSON *slots[40] = {
        num_item(r, "player_id"), num_item(r, "player_kills"), num_item(r, "player_wins"),
        num_item(r, "player_games_played"), num_item(r, "player_deaths"), num_item(r, "player_funds"),
        num_item(r, "player_score"), dup_item(r, "player_clan"), num_item(r, "player_timeplayed"),
        num_item(r, "player_featured"), inv ? inv : cJSON_CreateArray(), num_item(r, "player_lastreward"),
        num_item(r, "player_developer"), num_item(r, "player_following"), num_item(r, "player_followed"),
        cJSON_CreateNumber(0), stats ? stats : cJSON_CreateObject(), num_item(r, "player_hack"),
        dup_item(r, "partner_name"), num_item(r, "partner_approved"), num_item(r, "player_region"),
        num_item(r, "player_moderator"), num_item(r, "player_elo"), num_item(r, "player_elo2"),
        num_item(r, "player_elo4"), num_item(r, "player_chal"), num_item(r, "player_infected"),
        num_item(r, "player_premium"), cJSON_CreateNumber(num(r, "player_2fa") ? 1 : 0), display_name(r),
        dup_item(r, "player_datenew"), dup_item(r, "tw_twitchname"), num_item(r, "player_mapmod"),
        num_item(r, "clan_deaths"), num_item(r, "clan_kills"), cJSON_CreateNull(), num_item(r, "clan_regionid"),
        num_item(r, "clan_rank"), num_item(r, "jobrating"), num_item(r, "jobratingpositive")};
    for (int i = 0; i < 40; i++) cJSON_AddItemToArray(d, slots[i]);
    cJSON_AddItemToObject(o, "data", d);
    cJSON_Delete(r);
    /* WP-E part 2c (0107_progression): the earned badges, as kn_account_json's "badges" */
    cJSON *rows = dbx_rows(x, SQL(.any = "SELECT badge AS b__i FROM player_badges WHERE account_id = $1 ORDER BY badge"),
                           KN_DB_ARGS(I64S(id)));
    cJSON *badges = cJSON_CreateArray();
    const cJSON *row;
    cJSON_ArrayForEach(row, rows) cJSON_AddItemToArray(badges, cJSON_CreateNumber(num(row, "b")));
    cJSON_Delete(rows);
    cJSON_AddItemToObject(o, "badges", badges);
    /* v10 double XP: the end-screen 2x XP offer and the weekend schedules (kn_account_json's "dxp", 0341_double_xp) */
    cJSON_AddItemToObject(o, "dxp", db_dxp_state(x, id, kn_wall_ms()));
    return o;
}
