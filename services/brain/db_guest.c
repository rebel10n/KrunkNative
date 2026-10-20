/*
 * services/brain/db_guest.c - the signed-out guest's balance (Krunker v10.0.2 keeps a guest's earned KR
 * and score on the server; migration 0350_guest_profiles). Routes: guest.c. The game servers' ops
 * (guest_credit, guest_claim): accounts.c.
 *
 * A guest is the client-generated id: 32 lowercase hex digits, a bearer identifier (never logged whole).
 * Its balance is funds (KR) and score (XP). guest_credit records one match: the (guest, match) row of
 * guest_credits is inserted with insert-ignore, and only a new row moves the balance (duplicate = 1
 * otherwise), so a replayed round end pays nothing twice. guest_claim (a login) zeroes the balance and
 * creates nothing; the credit rows stay. A valid unknown guest reads as zeros and creates no row.
 *
 * Each proc runs in its own transaction (the core's kn_proc transaction: a refusal rolls it back). The C
 * implementation runs on every driver (no .pg text).
 */
#include "db_procs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "knsvc/json.h"

/* v10's config.guestKRCap / config.guestScoreCap: a guest's balance never grows past these */
#define GUEST_KR_CAP 20
#define GUEST_SCORE_CAP 2500
#define GUEST_ID_LEN 32    /* the client's guest id: 32 lowercase hex digits */
#define GUEST_MATCH_MAX 64 /* guest_credits.match_key: VARCHAR(64) on MySQL */

#define P(i) (n > (i) ? p[i] : NULL)
#define PI(i) pint(P(i))

static int64_t pint(const char *s) { return s ? strtoll(s, NULL, 10) : 0; }

/* an integer field of a dbx_row object (the "x__i" aliases read as numbers) */
static int64_t jint(const cJSON *o, const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsNumber(v) ? (int64_t) v->valuedouble : 0;
}

int guest_id_valid(const char *id) {
    if (!id || strlen(id) != GUEST_ID_LEN) return 0;
    for (size_t i = 0; i < GUEST_ID_LEN; i++) {
        const char c = id[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return 0;
    }
    return 1;
}

int guest_match_valid(const char *match) { return match && *match && strlen(match) <= GUEST_MATCH_MAX; }

/* a user-facing refusal: the message is the proc's error text (kn_db_user_message) */
static kn_db_result *refuse(Dbx *x, const char *msg) {
    dbx_refuse(x, msg);
    return dbx_finish(x, NULL);
}

/* {funds, score}: a new object the caller owns */
static cJSON *balance_json(const int64_t funds, const int64_t score) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "funds", (double) funds);
    cJSON_AddNumberToObject(o, "score", (double) score);
    return o;
}

/* the balance of a guest: zeros when it has no row (nothing is created). 0 ok, -1 failed. */
static int balance_of(Dbx *x, const char *guest, int64_t *funds, int64_t *score) {
    cJSON *row = dbx_row(x, SQL(.any = "SELECT funds AS funds__i, score AS score__i FROM guest_profiles WHERE guest_id = $1"),
                         KN_DB_ARGS(guest));
    if (dbx_failed(x)) {
        cJSON_Delete(row);
        return -1;
    }
    *funds = row ? jint(row, "funds") : 0;
    *score = row ? jint(row, "score") : 0;
    cJSON_Delete(row);
    return 0;
}

/* $0 guest -> {funds, score} */
static kn_db_result *run_profile(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const char *guest = P(0);
    if (!guest_id_valid(guest)) return refuse(&x, "Bad guest id.");
    int64_t funds = 0, score = 0;
    if (balance_of(&x, guest, &funds, &score) < 0) return dbx_finish(&x, NULL);
    return dbx_json(&x, "guest_profile", balance_json(funds, score));
}
const kn_proc P_GUEST_PROFILE = {"guest_profile", NULL, run_profile};

/* $0 guest, $1 match key, $2 KR, $3 XP -> {funds, score, duplicate}. KR and XP below zero count as zero. */
static kn_db_result *run_credit(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const char *guest = P(0), *match = P(1);
    if (!guest_id_valid(guest)) return refuse(&x, "Bad guest id.");
    if (!guest_match_valid(match)) return refuse(&x, "Bad match.");
    int64_t kr = PI(2), xp = PI(3);
    if (kr < 0) kr = 0;
    if (xp < 0) xp = 0;
    /* the balance moves by at most the caps (funds and score stay >= 0, so the capped sum is the same):
     * clamping keeps funds + amount inside BIGINT. The credit row keeps the amounts the server sent. */
    const int64_t kr_add = kr < GUEST_KR_CAP ? kr : GUEST_KR_CAP;
    const int64_t xp_add = xp < GUEST_SCORE_CAP ? xp : GUEST_SCORE_CAP;
    /* the guest's row first (zeros when new): the credit row below references it */
    if (dbx_exec(&x, SQL(.any = "{insert_ignore} INTO guest_profiles (guest_id) VALUES ($1) {ignore_conflicts}"),
                 KN_DB_ARGS(guest)) < 0)
        return dbx_finish(&x, NULL);
    /* the (guest, match) row: 1 when it is new, 0 when the match was credited before */
    const int64_t fresh = dbx_exec(&x,
        SQL(.any = "{insert_ignore} INTO guest_credits (guest_id, match_key, kr, xp) VALUES ($1, $2, {int:$3}, {int:$4})"
                   " {ignore_conflicts}"),
        KN_DB_ARGS(guest, match, I64S(kr), I64S(xp)));
    if (fresh < 0) return dbx_finish(&x, NULL);
    if (fresh > 0 &&
        dbx_exec(&x,
                 SQL(.any = "UPDATE guest_profiles SET funds = {least:funds + {int:$2}, {int:$3}},"
                            " score = {least:score + {int:$4}, {int:$5}}, updated_at = {now} WHERE guest_id = $1"),
                 KN_DB_ARGS(guest, I64S(kr_add), I64S(GUEST_KR_CAP), I64S(xp_add), I64S(GUEST_SCORE_CAP))) < 0)
        return dbx_finish(&x, NULL);
    int64_t funds = 0, score = 0;
    if (balance_of(&x, guest, &funds, &score) < 0) return dbx_finish(&x, NULL);
    cJSON *o = balance_json(funds, score);
    cJSON_AddNumberToObject(o, "duplicate", fresh > 0 ? 0 : 1);
    return dbx_json(&x, "guest_credit", o);
}
const kn_proc P_GUEST_CREDIT = {"guest_credit", NULL, run_credit};

/* $0 guest -> {funds: 0, score: 0}: zeroes an existing balance (the credit rows stay); creates nothing */
static kn_db_result *run_claim(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const char *guest = P(0);
    if (!guest_id_valid(guest)) return refuse(&x, "Bad guest id.");
    if (dbx_exec(&x, SQL(.any = "UPDATE guest_profiles SET funds = 0, score = 0, updated_at = {now} WHERE guest_id = $1"),
                 KN_DB_ARGS(guest)) < 0)
        return dbx_finish(&x, NULL);
    return dbx_json(&x, "guest_claim", balance_json(0, 0));
}
const kn_proc P_GUEST_CLAIM = {"guest_claim", NULL, run_claim};
