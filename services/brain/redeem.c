/*
 * services/brain/redeem.c - Krunker v10.0.2's voucher codes ("Redeem Keys", voucherPopup) over
 * the social socket.
 *
 * v10 sends REDEEM_VOUCHER ("vo", code) and gets REDEEM_VOUCHER_RESPONSE ("vr", funds, msg): funds
 * is set on success (the account's KR, the header), msg is the line shown under the code box
 * (the error, or the success line). KrunkNative's client talks to kn_brain only, so both are
 * social socket frames here:
 *   ["vr", code] -> ["vr", funds, msg] | ["vr", null, error]
 *
 * The codes are accounts.redeem_codes in the config: {"CODE": {"skin": index, "count": n,
 * "max_uses": m}}. The code is matched after its surrounding blanks are trimmed. A code grants
 * count copies of its skin (a TICKET item, or any other item) once per account, at most max_uses
 * in all. The grant is one skin_transactions row (source "redeem", ref the code): no table of its
 * own. The check and the grant share one transaction; a code's last use is not row-locked, so two
 * accounts redeeming it in the same instant can both pass on a store without that lock.
 * Owner: svc-accounts.
 */
#include "acct_internal.h"
#include "db_procs.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "knsvc/config.h"
#include "knsvc/json.h"

#define CODE_MAX 64
#define P(i) (n > (i) ? p[i] : NULL)
#define PI(i) pint(P(i))

typedef struct {
    int64_t skin, count, max_uses;
} RedeemCode;

static int64_t pint(const char *s) { return s ? strtoll(s, NULL, 10) : 0; }

/* the code without its surrounding blanks; "" when blank, too long or not text */
static void trim_code(const char *in, char *out, size_t cap) {
    out[0] = 0;
    if (!in) return;
    while (*in && isspace((unsigned char) *in)) in++;
    size_t len = strlen(in);
    while (len && isspace((unsigned char) in[len - 1])) len--;
    if (len == 0 || len >= cap) return;
    memcpy(out, in, len);
    out[len] = 0;
}

/* the configured code (0 when it is none, or its skin is not in the catalogue) */
static int code_of(const char *code, RedeemCode *out) {
    if (!g_acct_brain || !code[0]) return 0;
    const cJSON *e = cJSON_GetObjectItemCaseSensitive(kn_config_get(g_acct_brain->cfg, "accounts.redeem_codes"), code);
    if (!e) return 0;
    out->skin = kn_json_int(e, "skin", -1);
    out->count = kn_json_int(e, "count", 1);
    out->max_uses = kn_json_int(e, "max_uses", 1);
    return out->skin >= 0 && out->count >= 1 && out->max_uses >= 1 && acct_skin(out->skin) != NULL;
}

/* (account, code, skin, count, max_uses) -> {funds, skin, count}; a refusal: once per account,
 * max_uses in all. The grant is the same skin_transactions row the spins and the daily spin leave. */
static kn_db_result *run_redeem(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    /* the players row first: concurrent redeems of one account queue behind it (postgres) */
    dbx_exec(&x, SQL(.any = "UPDATE players SET player_funds = player_funds WHERE player_id = $1"), KN_DB_ARGS(P(0)));
    if (dbx_int(&x, SQL(.any = "SELECT COUNT(*) FROM skin_transactions WHERE account_id = $1 AND source = 'redeem' AND ref = $2"),
                KN_DB_ARGS(P(0), P(1)), 0) > 0)
        dbx_refuse(&x, "Code already redeemed.");
    else if (dbx_int(&x, SQL(.any = "SELECT COUNT(*) FROM skin_transactions WHERE source = 'redeem' AND ref = $1"),
                     KN_DB_ARGS(P(1)), 0) >= PI(4))
        dbx_refuse(&x, "Code has been used up.");
    if (dbx_failed(&x)) return dbx_finish(&x, NULL);
    db_add_skin(&x, PI(0), PI(2), PI(3), "redeem", P(1), NULL);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "funds",
                            (double) dbx_int(&x, SQL(.any = "SELECT player_funds FROM players WHERE player_id = $1"), KN_DB_ARGS(P(0)), 0));
    cJSON_AddNumberToObject(o, "skin", (double) PI(2));
    cJSON_AddNumberToObject(o, "count", (double) PI(3));
    return dbx_json(&x, "redeem_voucher", o);
}
static const kn_proc P_REDEEM_VOUCHER = {"redeem_voucher", NULL, run_redeem};

/* ["ua"] after a grant: the inventory the account now holds (the ticket the player redeemed) */
static void push_account_cb(void *ud, const char *err, const cJSON *data) {
    (void) ud;
    if (!err && data) social_push_account_update(data);
}

static void redeem_done(SocialSession *s, kn_db_result *res, void *extra) {
    (void) extra;
    cJSON *v = kn_db_ok(res) ? acct_db_json(res) : NULL;
    if (!v) {
        social_send_n(s, "vr", 2, cJSON_CreateNull(), cJSON_CreateString(acct_db_error(res, "redeem")));
        return;
    }
    const int64_t funds = kn_json_int(v, "funds", 0);
    cJSON_Delete(v);
    social_send_n(s, "vr", 2, cJSON_CreateNumber((double) funds), cJSON_CreateString("Code redeemed."));
    acct_get(s->account_id, push_account_cb, NULL);
}

/* ["vr", code]: REDEEM_VOUCHER */
void sh_redeem_voucher(SocialSession *s, const cJSON *f) {
    if (s->account_id <= 0) {
        social_send_n(s, "vr", 2, cJSON_CreateNull(), cJSON_CreateString("Must be logged in"));
        return;
    }
    char code[CODE_MAX + 1];
    trim_code(cJSON_GetStringValue(cJSON_GetArrayItem(f, 1)), code, sizeof code);
    RedeemCode rc;
    if (!code_of(code, &rc)) {
        social_send_n(s, "vr", 2, cJSON_CreateNull(), cJSON_CreateString("Invalid code"));
        return;
    }
    char id[24], skin[24], count[24], max[24];
    acct_itoa(id, s->account_id);
    acct_itoa(skin, rc.skin);
    acct_itoa(count, rc.count);
    acct_itoa(max, rc.max_uses);
    social_call(s, &P_REDEEM_VOUCHER, KN_DB_ARGS(id, code, skin, count, max), redeem_done, NULL);
}
