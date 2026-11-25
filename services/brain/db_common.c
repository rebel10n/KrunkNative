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

#include "account_life.h"
#include "brain_internal.h" /* ranked_season_now: the account's ranked_rank */
#include "ranked.h"         /* ranked_rank_of, RANKED_PLACEMENT_MATCHES */
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

void copy_meta_none(CopyPaint *p) {
    memset(p, 0, sizeof *p);
    p->shader = -1;
}

int copy_meta_any(const CopyPaint *p) {
    int counted = 0;
    for (int i = 0; i < COPY_STAT_COUNT; i++) counted |= p->stats[i] > 0;
    return p->color[0] != '\0' || p->shader >= 0 || p->has_wear || p->serial > 0 || p->name[0] != '\0' || counted;
}

const char *copy_stats_text(const CopyPaint *p, char *buf, const size_t cap) {
    static const char *const stats[COPY_STAT_COUNT] = COPY_STATS;
    size_t n = 0;
    for (int i = 0; i < COPY_STAT_COUNT && n < cap; i++) {
        if (p->stats[i] <= 0) continue;
        const int w = snprintf(buf + n, cap - n, "%s\"%s\":%lld", n ? "," : "{", stats[i], (long long) p->stats[i]);
        if (w < 0 || (size_t) w >= cap - n) return NULL;
        n += (size_t) w;
    }
    if (!n || n + 2 > cap) return NULL;
    buf[n++] = '}';
    buf[n] = '\0';
    return buf;
}

void copy_stats_parse(const char *text, CopyPaint *p) {
    static const char *const stats[COPY_STAT_COUNT] = COPY_STATS;
    memset(p->stats, 0, sizeof p->stats);
    cJSON *o = text && *text ? cJSON_Parse(text) : NULL;
    for (int i = 0; cJSON_IsObject(o) && i < COPY_STAT_COUNT; i++) {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, stats[i]);
        if (cJSON_IsNumber(v) && v->valuedouble >= 1) p->stats[i] = (int64_t) v->valuedouble;
    }
    cJSON_Delete(o);
}

void copy_meta_read(const kn_db_result *r, const int row, const int col, CopyPaint *p) {
    copy_meta_none(p);
    if (!kn_db_isnull(r, row, col)) snprintf(p->color, sizeof p->color, "%s", kn_db_get(r, row, col));
    if (!kn_db_isnull(r, row, col + 1)) p->shader = kn_db_get_int64(r, row, col + 1, -1);
    if (!kn_db_isnull(r, row, col + 2)) {
        p->has_wear = 1;
        p->wear = kn_db_get_double(r, row, col + 2, 0.0);
    }
    if (!kn_db_isnull(r, row, col + 3)) p->serial = kn_db_get_int64(r, row, col + 3, 0);
    if (!kn_db_isnull(r, row, col + 4)) snprintf(p->name, sizeof p->name, "%s", kn_db_get(r, row, col + 4));
}

const char *copy_meta_name(const CopyPaint *p) { return p->name[0] ? p->name : NULL; }

int copy_name_tag_valid(const char *in, char *out, const size_t out_size) {
    if (!in || !out || out_size < 24) return 0;
    while (*in == ' ') in++;
    size_t n = strlen(in);
    while (n && in[n - 1] == ' ') n--;
    if (!n || n > 23) return 0;
    int chars = 0;
    for (size_t i = 0; i < n;) {
        const unsigned char c = (unsigned char) in[i];
        const int len = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
        if (!len || i + (size_t) len > n) return 0;
        for (int k = 1; k < len; k++)
            if (((unsigned char) in[i + k] & 0xC0) != 0x80) return 0;
        if (len == 1 && (c < 0x20 || c == 0x7F || c == '"' || c == '\'' || c == '<' || c == '>' || c == '\\' || c == '`'))
            return 0;
        i += (size_t) len;
        chars++;
    }
    if (chars > 20) return 0;
    memcpy(out, in, n);
    out[n] = '\0';
    return 1;
}

void copy_meta_params(const CopyPaint *p, const char **color, const char **shader, const char **wear, const char **serial,
                      char buf[3][32]) {
    *color = p->color[0] ? p->color : NULL;
    *shader = NULL;
    if (!p->color[0] && p->shader >= 0) {
        snprintf(buf[0], 32, "%lld", (long long) p->shader);
        *shader = buf[0];
    }
    *wear = NULL;
    if (p->has_wear) {
        snprintf(buf[1], 32, "%.17g", p->wear);
        *wear = buf[1];
    }
    *serial = NULL;
    if (p->serial > 0) {
        snprintf(buf[2], 32, "%lld", (long long) p->serial);
        *serial = buf[2];
    }
}

cJSON *copy_meta_json(const CopyPaint *p) {
    cJSON *m = v10_paint_meta(p->color, p->color[0] ? -1 : (int) p->shader);
    int counted = 0;
    for (int i = 0; i < COPY_STAT_COUNT; i++) counted |= p->stats[i] > 0;
    if (!p->has_wear && p->serial <= 0 && !p->name[0] && !counted) return m;
    if (!m) m = cJSON_CreateObject();
    if (!m) return NULL;
    if (p->has_wear) cJSON_AddNumberToObject(m, "wear", p->wear);
    if (p->serial > 0) cJSON_AddNumberToObject(m, "serial", (double) p->serial);
    if (p->name[0]) cJSON_AddStringToObject(m, "name", p->name);
    /* a counter skin's count is the copy's m[stat] (deob-10.0.7.js:191932); a listing's m carries it to the item
     * item view, which draws each of the eight stats it finds (:106682, :106774) */
    static const char *const stats[COPY_STAT_COUNT] = COPY_STATS;
    for (int i = 0; i < COPY_STAT_COUNT; i++)
        if (p->stats[i] > 0) cJSON_AddNumberToObject(m, stats[i], (double) p->stats[i]);
    return m;
}

int copy_take(Dbx *x, int64_t acc, int64_t copy, int64_t skin, CopyPaint *paint, const char *source, const char *ref) {
    kn_db_result *r = dbx_q(x,
        SQL(.any = "SELECT " COPY_META_COLS " FROM skin_copies WHERE copy_id = $1 AND account_id = $2"
                   " AND skin_id = $3 {for_update}"),
        KN_DB_ARGS(I64S(copy), I64S(acc), I64S(skin)));
    if (!r) return -1;
    if (kn_db_rows(r) < 1) {
        kn_db_result_free(r);
        return dbx_refuse(x, "You don't own that copy.");
    }
    CopyPaint p;
    copy_meta_read(r, 0, 0, &p);
    kn_db_result_free(r);
    /* a counter skin's counts are the copy's m[stat] in v10 (takeSkinStats, deob-10.0.7.js:191932): they leave the
     * account with the copy (a trade, a listing) and are gone with it (a quick sale, a craft) */
    static const char *const stats[COPY_STAT_COUNT] = COPY_STATS;
    kn_db_result *sr = dbx_q(x, SQL(.any = "SELECT stat, count FROM skin_stats WHERE account_id = $1 AND skin_id = $2 AND copy_id = $3"),
                             KN_DB_ARGS(I64S(acc), I64S(skin), I64S(copy)));
    if (!sr) return -1;
    for (int i = 0; i < kn_db_rows(sr); i++)
        for (int k = 0; k < COPY_STAT_COUNT; k++)
            if (!strcmp(kn_db_get(sr, i, 0), stats[k])) p.stats[k] += kn_db_get_int64(sr, i, 1, 0);
    kn_db_result_free(sr);
    if (dbx_exec(x, SQL(.any = "DELETE FROM skin_stats WHERE account_id = $1 AND skin_id = $2 AND copy_id = $3"),
                 KN_DB_ARGS(I64S(acc), I64S(skin), I64S(copy))) < 0)
        return -1;
    /* the copy row goes first: db_add_skin's guard then sees one painted unit fewer */
    if (dbx_exec(x, SQL(.any = "DELETE FROM skin_copies WHERE copy_id = $1"), KN_DB_ARGS(I64S(copy))) < 0) return -1;
    /* a style set on the copy (item_styles, 0393) does not outlive it */
    if (dbx_exec(x, SQL(.any = "DELETE FROM item_styles WHERE account_id = $1 AND skin_id = $2 AND copy_id = $3"),
                 KN_DB_ARGS(I64S(acc), I64S(skin), I64S(copy))) < 0)
        return -1;
    if (db_add_skin(x, acc, skin, -1, source, ref, NULL) < 0) return -1;
    if (paint) *paint = p;
    return 0;
}

int copy_give(Dbx *x, int64_t acc, int64_t skin, const CopyPaint *paint, const char *source, const char *ref, int64_t *copy_id) {
    if (db_add_skin(x, acc, skin, 1, source, ref, NULL) < 0) return -1;
    const char *color, *shader, *wear, *serial;
    char buf[3][32];
    copy_meta_params(paint, &color, &shader, &wear, &serial, buf);
    const int64_t id = dbx_insert_id(x,
        SQL(.pg = "INSERT INTO skin_copies (account_id, skin_id, " COPY_META_COLS ")"
                  " VALUES ($1::int, $2::int, $3, $4::int, $5::double precision, $6::int, $7) RETURNING copy_id",
            .sqlite = "INSERT INTO skin_copies (account_id, skin_id, " COPY_META_COLS ")"
                      " VALUES ($1, $2, $3, $4, $5, $6, $7) RETURNING copy_id",
            .mysql = "INSERT INTO skin_copies (account_id, skin_id, " COPY_META_COLS ")"
                     " VALUES ($1, $2, $3, $4, $5, $6, $7)"),
        KN_DB_ARGS(I64S(acc), I64S(skin), color, shader, wear, serial, copy_meta_name(paint)));
    if (id < 0) return -1;
    /* the counts the copy carried (copy_take, an escrow's stats column) are the new copy's */
    static const char *const stats[COPY_STAT_COUNT] = COPY_STATS;
    for (int i = 0; i < COPY_STAT_COUNT; i++) {
        if (paint->stats[i] <= 0) continue;
        if (dbx_exec(x, SQL(.pg = "INSERT INTO skin_stats (account_id, skin_id, copy_id, stat, count)"
                                  " VALUES ($1::int, $2::int, $3::bigint, $4, $5::bigint)",
                            .sqlite = "INSERT INTO skin_stats (account_id, skin_id, copy_id, stat, count) VALUES ($1, $2, $3, $4, $5)",
                            .mysql = "INSERT INTO skin_stats (account_id, skin_id, copy_id, stat, count) VALUES ($1, $2, $3, $4, $5)"),
                     KN_DB_ARGS(I64S(acc), I64S(skin), I64S(id), stats[i], I64S(paint->stats[i]))) < 0)
            return -1;
    }
    if (copy_id) *copy_id = id;
    return 0;
}

int copy_mint(Dbx *x, int64_t acc, int64_t skin, double wear, const char *source, const char *ref, int64_t *copy_id, int64_t *serial) {
    if (dbx_exec(x, SQL(.pg = "INSERT INTO item_mints (skin_id, minted) VALUES ($1::int, 1)"
                              " ON CONFLICT (skin_id) DO UPDATE SET minted = item_mints.minted + 1",
                        .sqlite = "INSERT INTO item_mints (skin_id, minted) VALUES ($1, 1)"
                                  " ON CONFLICT (skin_id) DO UPDATE SET minted = item_mints.minted + 1",
                        .mysql = "INSERT INTO item_mints (skin_id, minted) VALUES ($1, 1)"
                                 " ON DUPLICATE KEY UPDATE minted = minted + 1"),
                 KN_DB_ARGS(I64S(skin))) < 0)
        return -1;
    const int64_t n = dbx_int(x, SQL(.any = "SELECT minted FROM item_mints WHERE skin_id = $1"), KN_DB_ARGS(I64S(skin)), 0);
    if (dbx_failed(x) || n <= 0) return -1;
    CopyPaint m;
    copy_meta_none(&m);
    m.has_wear = 1;
    m.wear = wear < 0.0 ? 0.0 : wear >= 1.0 ? 0.9999 : wear;
    m.serial = n;
    if (copy_give(x, acc, skin, &m, source, ref, copy_id) < 0) return -1;
    if (serial) *serial = n;
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
 * {ind, cnt: 1, id, m} row per copy (id = its copy id, m = {paint} or {paintShader}, and a minted copy's wear and
 * serial, v10.0.7's instances: 0398). NULL on a DB error. */
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
        SQL(.any = "SELECT copy_id AS id, skin_id AS ind, paint_color, paint_shader, wear, serial, name_tag AS nametag"
                   " FROM skin_copies WHERE account_id = $1 ORDER BY copy_id"),
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
        cJSON *m = v10_copy_meta_row(row); /* {paint | paintShader, wear, serial, name} (0352, 0398, 0401) */
        if (m) cJSON_AddItemToObject(c, "m", m);
        cJSON_AddItemToArray(rows, c);
    }
    cJSON_Delete(copies);
    /* v10 counter skins: a counter skin's stat is the m[stat] of the copy that counted it (takeSkinStats adds to that copy,
     * deob-10.0.7.js:191929-191932; the base is the worn copy's m[stat] || 0, :173031). skin_stats keeps it per copy (0406):
     * a copy's count goes on the entry with that id (none when the copy left the account), a stack's (copy 0) on the
     * stack, else on the index's first entry (a stack row whose units were all painted since). */
    cJSON *counts = dbx_rows(x,
        SQL(.any = "SELECT skin_id AS ind, copy_id AS id, stat, count FROM skin_stats WHERE account_id = $1 AND count > 0"
                   " ORDER BY skin_id, copy_id"),
        KN_DB_ARGS(I64S(id)));
    if (dbx_failed(x)) {
        cJSON_Delete(counts);
        cJSON_Delete(rows);
        return NULL;
    }
    cJSON_ArrayForEach(row, counts) {
        const int64_t ind = kn_json_int(row, "ind", -1), copy = kn_json_int(row, "id", 0);
        const char *stat = kn_json_str(row, "stat", "");
        if (ind < 0 || !*stat) continue;
        cJSON *entry, *target = NULL, *first = NULL;
        cJSON_ArrayForEach(entry, rows) {
            if (kn_json_int(entry, "ind", -2) != ind) continue;
            if (!first) first = entry;
            if (kn_json_int(entry, "id", 0) == copy) { /* a stack has no id: 0 */
                target = entry;
                break;
            }
        }
        if (!target && copy <= 0) target = first;
        if (!target) continue;
        cJSON *m = cJSON_GetObjectItemCaseSensitive(target, "m");
        if (!cJSON_IsObject(m)) {
            cJSON_DeleteItemFromObjectCaseSensitive(target, "m");
            m = cJSON_AddObjectToObject(target, "m");
        }
        if (!m) continue;
        /* a stack row landing on a copy that has its own count: the two add up */
        const cJSON *had = cJSON_GetObjectItemCaseSensitive(m, stat);
        const double n = (double) kn_json_int(row, "count", 0) + (cJSON_IsNumber(had) ? had->valuedouble : 0.0);
        cJSON_DeleteItemFromObjectCaseSensitive(m, stat);
        cJSON_AddNumberToObject(m, stat, n);
    }
    cJSON_Delete(counts);
    /* v10.0.7 item styles (item_styles, 0393): m.style on the stack (copy_id 0) or the copy it was set on; the card and
     * the game read it for the styles[n] thumbnail and the _s<n> texture (deob-10.0.7.js:223502, 173602) */
    cJSON *styled = dbx_rows(x, SQL(.any = "SELECT skin_id AS ind, copy_id AS id, style FROM item_styles WHERE account_id = $1"),
                             KN_DB_ARGS(I64S(id)));
    if (dbx_failed(x)) {
        cJSON_Delete(styled);
        cJSON_Delete(rows);
        return NULL;
    }
    cJSON_ArrayForEach(row, styled) {
        const int64_t ind = kn_json_int(row, "ind", -1), copy = kn_json_int(row, "id", 0);
        cJSON *entry;
        cJSON_ArrayForEach(entry, rows) {
            if (kn_json_int(entry, "ind", -2) != ind || kn_json_int(entry, "id", 0) != copy) continue;
            cJSON *m = cJSON_GetObjectItemCaseSensitive(entry, "m");
            if (!cJSON_IsObject(m)) {
                cJSON_DeleteItemFromObjectCaseSensitive(entry, "m");
                m = cJSON_AddObjectToObject(entry, "m");
            }
            if (m) {
                cJSON_DeleteItemFromObjectCaseSensitive(m, "style");
                cJSON_AddNumberToObject(m, "style", (double) kn_json_int(row, "style", 0));
            }
            break;
        }
    }
    cJSON_Delete(styled);
    return rows;
}

/* chat_banned: a staff chat ban in force (feature_bans 'chat', v10_mods.c). Game servers refuse the player's chat lines
 * but not the forfeit vote (v10.0.4 "Chat-restricted players can now vote to forfeit"; server/src/net.c). */
static void db_account_chat_ban(Dbx *x, const int64_t id, cJSON *o) {
    if (!o) return;
    const int banned = dbx_exists(x, SQL(.any = "SELECT 1 FROM feature_bans WHERE account_id = {int:$1} AND feature = 'chat'"
                                                " AND revoked_at IS NULL AND (expires_at IS NULL OR expires_at > {now})"),
                                  KN_DB_ARGS(I64S(id)));
    cJSON_DeleteItemFromObjectCaseSensitive(o, "chat_banned");
    cJSON_AddBoolToObject(o, "chat_banned", banned);
}

/* the account's completed trades (v10's account.trades = player_trades, deob-10.0.7.js:115126): its accepted trade offers,
 * sent or received (trades.status 'accepted', db_economy.c run_trade_accept), and its settled Trade mode trades
 * (live_trades, 0357), either side */
int64_t db_completed_trades(Dbx *x, int64_t id) {
    return dbx_int(x,
        SQL(.any = "SELECT (SELECT count(*) FROM trades WHERE status = 'accepted' AND (sender_id = {int:$1} OR receiver_id = {int:$1}))"
                   " + (SELECT count(*) FROM live_trades WHERE account_a = {int:$1} OR account_b = {int:$1})"),
        KN_DB_ARGS(I64S(id)), 0);
}

/* "trades": the count above, which the social socket's account info carries (social_account_info) for the client's
 * account.trades (setData slot 33, deob-10.0.7.js:115184; the Black Market's accVar "trades" item) */
static void db_account_trades(Dbx *x, const int64_t id, cJSON *o) {
    if (!o) return;
    cJSON_DeleteItemFromObjectCaseSensitive(o, "trades");
    cJSON_AddNumberToObject(o, "trades", (double) db_completed_trades(x, id));
}

/* "ranked_rank": v10's account.rankedRank (setData `ranked_rank || 0`, deob-10.0.7.js:115141), which the game server sends
 * as ADD_PLAYERS' rankedRank (getPlayerAddData :176753; server/src/accounts.c, NET_EXT_KN_PLAYER_RANK_FLAG). The same row
 * the profile's rank chip reads (db_social.c, social_profile_rank): the latest season's profile, a placed one and the best
 * rated first, its tier when that season is the running one, else 0 Unranked. */
#define DB_RANKED_STR_(x) #x
#define DB_RANKED_STR(x) DB_RANKED_STR_(x)
static void db_account_ranked_rank(Dbx *x, const int64_t id, cJSON *o) {
    if (!o) return;
    cJSON *ranked = dbx_row(x,
        SQL(.any = "SELECT season AS season__i, cr AS cr__f, wins + losses + draws AS matches__i FROM ranked_profiles"
                   " WHERE account_id = $1 ORDER BY season DESC, (wins + losses + draws >= " DB_RANKED_STR(RANKED_PLACEMENT_MATCHES) ") DESC,"
                   " cr DESC LIMIT 1"),
        KN_DB_ARGS(I64S(id)));
    int rank = 0;
    if (cJSON_IsObject(ranked) && kn_json_int(ranked, "season", -1) == ranked_season_now())
        rank = ranked_rank_of(kn_json_num(ranked, "cr", 0), (int) kn_json_int(ranked, "matches", 0));
    cJSON_Delete(ranked);
    cJSON_DeleteItemFromObjectCaseSensitive(o, "ranked_rank");
    cJSON_AddNumberToObject(o, "ranked_rank", rank);
}

/* 0401_skin_maker: v10's acc.isSkinMaker (~~player_skinmaker, deob-10.0.7.js:115136), players.player_skinmaker. The game
 * server gives a skin maker the season 6+ store items no creator made (getSkins, :217781-217789); developers set it ("skm",
 * profiles.c). Added in C for every driver (kn_account_json is unchanged). */
static void db_account_skin_maker(Dbx *x, const int64_t id, cJSON *o) {
    if (!o) return;
    cJSON_DeleteItemFromObjectCaseSensitive(o, "player_skinmaker");
    cJSON_AddNumberToObject(o, "player_skinmaker",
        (double) dbx_int(x, SQL(.any = "SELECT player_skinmaker FROM players WHERE player_id = $1"), KN_DB_ARGS(I64S(id)), 0));
}

/* "titles": v10's account.titles (setData; the client wears a titleIndex only when acc.titles.indexOf(id) >= 0), the
 * player_titles ids (0361: Black Market and challenge grants) in id order, as the "ttl" reply lists them (social_data.c).
 * The game server sends the ENT's titleIndex as ADD_PLAYERS' [52] only for one of them (server/src/accounts.c,
 * NET_EXT_KN_PLAYER_TITLE). Added in C for every driver (kn_account_json is unchanged). */
static void db_account_titles(Dbx *x, const int64_t id, cJSON *o) {
    if (!o) return;
    cJSON *rows = dbx_rows(x, SQL(.any = "SELECT title_id AS t__i FROM player_titles WHERE account_id = $1 ORDER BY title_id"),
                           KN_DB_ARGS(I64S(id)));
    cJSON *titles = cJSON_CreateArray();
    const cJSON *row;
    cJSON_ArrayForEach(row, rows) cJSON_AddItemToArray(titles, cJSON_CreateNumber(num(row, "t")));
    cJSON_Delete(rows);
    cJSON_DeleteItemFromObjectCaseSensitive(o, "titles");
    cJSON_AddItemToObject(o, "titles", titles);
}

/* PostgreSQL's kn_account_json (and the functions that return it, e.g. kn_commit_stats) builds the account in SQL; what
 * the other drivers' C adds is added here: the owned list (the SQL's data[10] counts every unit; painted copies apart,
 * counter skins' m[stat]), reqDel / twitchId / raidStats, chat_banned, trades, ranked_rank and titles. Every account a PostgreSQL proc hands out goes
 * through this (db_accounts.c: session_account, account_json, commit_stats). */
void db_account_pg_finish(Dbx *x, int64_t id, cJSON *v) {
    if (!v || id <= 0) return;
    cJSON *data = cJSON_GetObjectItemCaseSensitive(v, "data");
    cJSON *owned = data && cJSON_GetArraySize(data) > 10 ? db_owned_skins(x, id) : NULL;
    if (owned) cJSON_ReplaceItemInArray(data, 10, owned);
    else cJSON_Delete(owned);
    db_account_life_extras(x, id, v); /* v10.0.3 reqDel, twitchId, raidStats (account_life.c, 0359 / 0360) */
    db_account_chat_ban(x, id, v);
    db_account_trades(x, id, v);
    db_account_ranked_rank(x, id, v);
    db_account_skin_maker(x, id, v);
    db_account_titles(x, id, v);
}

cJSON *db_account_json(Dbx *x, int64_t id) {
    if (dbx_is_pg(x)) {
        char *s = dbx_text(x, SQL(.pg = "SELECT kn_account_json($1::int)::text"), KN_DB_ARGS(I64S(id)));
        cJSON *v = s ? cJSON_Parse(s) : NULL;
        free(s);
        db_account_pg_finish(x, id, v);
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
    db_account_life_extras(x, id, o); /* v10.0.3 reqDel, twitchId, raidStats (account_life.c, 0359 / 0360) */
    db_account_chat_ban(x, id, o);
    db_account_trades(x, id, o);
    db_account_ranked_rank(x, id, o);
    db_account_skin_maker(x, id, o);
    db_account_titles(x, id, o);
    return o;
}
