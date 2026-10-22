/*
 * services/brain/daily_spin.c - Krunker v10.0.2's Daily Spin over the social socket.
 *
 * v10 asks gapi GET /daily-spin/progress ({data: {score, maxScore, timeRemaining, claimed,
 * stage}}) and claims with the game socket's rpc DAILY_SPIN_CLAIM "dsl" -> "dslr" {ticketId}
 * (v10.js 25996, 117993, 146915, 222181). KrunkNative's client talks to kn_brain only, so both
 * are social socket frames here:
 *   ["dsp"] -> ["dsp", {score, maxScore, timeRemaining, claimed, stage}]
 *   ["dsl"] -> ["dslr", {ticketId}] | ["dslr", null, err]
 * score: the account's match score since the UTC day began (match_results, the stats commits of
 * the game servers); claimed: a "dailyspin" grant today; stage: the grants so far. The claim
 * grants the spin ticket of the day's tier ((stage + 1) % 7 == 0 Heroic, % 3 == 0 Elite, else
 * Starter: that wheel's ticketIndex, store.c), checked and granted in one transaction (no table
 * of its own: skin_transactions keeps the grants). Owner: svc-accounts.
 */
#include "acct_internal.h"
#include "db_procs.h"
#include "v10_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "knsvc/config.h"
#include "knsvc/json.h"
#include "knsvc/log.h"
#include "knsvc/loop.h"

#define MOD "dailyspin"
#define DAY_MS 86400000LL
#define P(i) (n > (i) ? p[i] : NULL)
#define PI(i) pint(P(i))

static int64_t pint(const char *s) { return s ? strtoll(s, NULL, 10) : 0; }

static int max_score(void) { return g_acct_brain ? (int) kn_config_int(g_acct_brain->cfg, "accounts.daily_spin_score", 5000) : 5000; }

static int64_t day_start(int64_t now) { return now - now % DAY_MS; }

static int64_t today_score(Dbx *x, const char *id, const char *since) {
    return dbx_int(x, SQL(.any = "SELECT coalesce(sum(score), 0) FROM match_results WHERE account_id = $1 AND {epoch_ms:at} >= {int:$2}"),
                   KN_DB_ARGS(id, since), 0);
}

static int64_t claims(Dbx *x, const char *id, const char *since) {
    return dbx_int(x, SQL(.any = "SELECT count(*) FROM skin_transactions WHERE account_id = $1 AND source = 'dailyspin'"
                                 " AND delta > 0 AND {epoch_ms:at} >= {int:$2}"),
                   KN_DB_ARGS(id, since), 0);
}

/* (id, day_start_ms) -> {score, claimed, stage} */
static kn_db_result *run_state(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "score", (double) today_score(&x, P(0), P(1)));
    cJSON_AddBoolToObject(o, "claimed", claims(&x, P(0), P(1)) > 0);
    cJSON_AddNumberToObject(o, "stage", (double) claims(&x, P(0), "0"));
    return dbx_json(&x, "daily_spin", o);
}
static const kn_proc P_DAILY_SPIN_STATE = {"daily_spin_state", NULL, run_state};

/* (id, day_start_ms, max_score, ticket starter, elite, heroic) -> {ticketId}: one grant a day */
static kn_db_result *run_claim(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    /* the players row first: concurrent claims of one account queue behind it (postgres) */
    dbx_exec(&x, SQL(.any = "UPDATE players SET player_funds = player_funds WHERE player_id = $1"), KN_DB_ARGS(P(0)));
    if (claims(&x, P(0), P(1)) > 0) dbx_refuse(&x, "Daily spin already claimed");
    else if (today_score(&x, P(0), P(1)) < PI(2)) dbx_refuse(&x, "Play more to unlock the daily spin");
    if (dbx_failed(&x)) return dbx_finish(&x, NULL);
    const int64_t stage = claims(&x, P(0), "0") + 1;
    const int64_t ticket = stage % 7 == 0 ? PI(5) : stage % 3 == 0 ? PI(4) : PI(3);
    if (ticket < 0) {
        dbx_refuse(&x, "Daily spin unavailable");
        return dbx_finish(&x, NULL);
    }
    db_add_skin(&x, PI(0), ticket, 1, "dailyspin", P(1), NULL);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "ticketId", (double) ticket);
    return dbx_json(&x, "daily_spin_claim", o);
}
static const kn_proc P_DAILY_SPIN_CLAIM = {"daily_spin_claim", NULL, run_claim};

/* ["dsp", null, err]: every "dsp" is answered, a refusal or a failure too (the client's item says Unavailable) */
static void progress_refused(SocialSession *s, const char *err) {
    social_send_n(s, "dsp", 2, cJSON_CreateNull(), cJSON_CreateString(err));
}

static void state_done(SocialSession *s, kn_db_result *res, void *extra) {
    (void) extra;
    cJSON *v = kn_db_ok(res) ? acct_db_json(res) : NULL;
    if (!v) {
        const char *err = acct_db_error(res, "daily spin");
        KN_WARN(MOD, "progress: %s", err);
        progress_refused(s, "Box Office could not read the daily spin");
        return;
    }
    const int64_t now = kn_wall_ms();
    cJSON_AddNumberToObject(v, "maxScore", max_score());
    cJSON_AddNumberToObject(v, "timeRemaining", (double) (day_start(now) + DAY_MS - now));
    social_send_n(s, "dsp", 1, v);
}

/* ["dsp"] */
void sh_daily_spin(SocialSession *s, const cJSON *f) {
    (void) f;
    if (s->account_id <= 0) {
        progress_refused(s, "Must be logged in");
        return;
    }
    if (!g_acct_brain || !g_acct_brain->db) {
        progress_refused(s, "Box Office has no database for the daily spin");
        return;
    }
    char id[24], since[24];
    acct_itoa(id, s->account_id);
    acct_itoa(since, day_start(kn_wall_ms()));
    if (social_call(s, &P_DAILY_SPIN_STATE, KN_DB_ARGS(id, since), state_done, NULL) != 0)
        progress_refused(s, "Box Office could not read the daily spin");
}

/* GET /daily-spin/progress (v10.js 25999, the HTTP root): the same state "dsp" sends, for the account of the request */
static void progress_done(kn_db_result *res, void *ud) {
    kn_http_req *req = ud;
    cJSON *v = kn_db_ok(res) ? acct_db_json(res) : NULL;
    if (!v) {
        v10_reply_error(req, 500, "internal", acct_db_error(res, "daily spin"));
        return;
    }
    const int64_t now = kn_wall_ms();
    cJSON_AddNumberToObject(v, "maxScore", max_score());
    cJSON_AddNumberToObject(v, "timeRemaining", (double) (day_start(now) + DAY_MS - now));
    v10_reply_data(req, 200, v);
}

void daily_spin_progress_v10(kn_http_req *req, int64_t account) {
    if (!g_acct_brain || !g_acct_brain->db) {
        v10_reply_error(req, 503, "DailySpinUnavailable", "Box Office has no database for the daily spin");
        return;
    }
    char id[24], since[24];
    acct_itoa(id, account);
    acct_itoa(since, day_start(kn_wall_ms()));
    if (kn_db_call(g_acct_brain->db, &P_DAILY_SPIN_STATE, KN_DB_ARGS(id, since), progress_done, req) != 0)
        v10_reply_error(req, 503, "DailySpinUnavailable", "Box Office could not read the daily spin");
}

static void claim_done(SocialSession *s, kn_db_result *res, void *extra) {
    (void) extra;
    cJSON *v = kn_db_ok(res) ? acct_db_json(res) : NULL;
    if (!v) {
        social_send_n(s, "dslr", 2, cJSON_CreateNull(), cJSON_CreateString(acct_db_error(res, "daily spin")));
        return;
    }
    social_send_n(s, "dslr", 1, v);
}

/* ["dsl"]: DAILY_SPIN_CLAIM. Every refusal is answered "dslr" (the client clears its claim on that answer, not on "po") */
static void claim_refused(SocialSession *s, const char *err) {
    social_send_n(s, "dslr", 2, cJSON_CreateNull(), cJSON_CreateString(err));
}

void sh_daily_spin_claim(SocialSession *s, const cJSON *f) {
    (void) f;
    if (s->account_id <= 0) {
        claim_refused(s, "Must be logged in");
        return;
    }
    if (!g_acct_brain || !g_acct_brain->db) {
        claim_refused(s, "Box Office has no database for the daily spin");
        return;
    }
    char id[24], since[24], max[24], t0[24], t1[24], t2[24];
    acct_itoa(id, s->account_id);
    acct_itoa(since, day_start(kn_wall_ms()));
    acct_itoa(max, max_score());
    acct_itoa(t0, store_wheel_ticket("Starter"));
    acct_itoa(t1, store_wheel_ticket("Elite"));
    acct_itoa(t2, store_wheel_ticket("Heroic"));
    if (social_call(s, &P_DAILY_SPIN_CLAIM, KN_DB_ARGS(id, since, max, t0, t1, t2), claim_done, NULL) != 0)
        claim_refused(s, "Box Office could not claim the daily spin");
}
