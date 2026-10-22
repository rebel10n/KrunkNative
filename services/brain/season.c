/*
 * services/brain/season.c - Krunker v7.1.4 seasons and the Battle Pass on the social socket
 * (doc/backend.md 7.8). v7 sends these on its game socket (index.js windows[5]: io.send("bpdl"),
 * "bpclm", "xstbp" / "stpbpV", "xstbpl" / "stpbplV"); KrunkNative's menu talks to kn_brain
 * directly, like its other account requests. The database work is db_season.c; the match-end
 * pushes ("bppd", "bpEnd") come from the stats commit (accounts.c progress_done).
 *
 *   ["bpdl"]               -> ["bpdl", pass | null] then ["bppd", account.BP] (signed in)
 *   ["bpclm", itemId]      -> ["bpclmr", null, null, sk, null, null, {id, tier, col, sk}] +
 *                             ["bppd", account.BP] | ["po", null, err]   (v7.1.4 claimItemRes; the
 *                             v10 client reads it too)
 *   ["bpclmAll"]           -> ["bpclmr", null, funds, null, null, 0, [{id, tier, col, sk, kr, jnk}...]]
 *                             + "bppd" + "ua" | ["po", null, err]   (v10.0.2 BP_CLAIM_ALL /
 *                             claimItemRes(_, funds, _, _, junk, items))
 *   ["xstbp", tier, code, null, null, channel, giftName]
 *                          -> ["purbp", "kr", null, null, tier, price, "KR", txid] + "bppd" +
 *                             ["nbdgr", [badge...]] + "ua" (new KR) | ["purbp", "kr", {err}, "1"]
 *                             KrunkNative: the VIP tier is paid in KR (v7: Xsolla / Steam). v10
 *                             selectBPGift (giftName): [... txid, null, null, true, name] (purchaseRes
 *                             gifted, recipient) and the recipient's sessions get "bppd" + "nbdgr"
 *   ["xstbpl", levels]     -> ["purbpl", "kr", null, null, levels, bpp, price, "KR", txid, null,
 *                             badge] + "bppd" + "ua" | ["purbpl", "kr", {err}, null, levels]
 *   ["sst", name, season?] -> ["sst", {season, name, kills, deaths, wins, games, score,
 *                             timePlayed, bpp, tier, level, levels} | null]  (KrunkNative: the
 *                             profile's season block)
 *
 * accounts.current_season (config.currentSeason, default SEASON_CURRENT 10) is the season; kn_brain records it
 * in `seasons` at start (P_SEASON_ENSURE: insert only, a rollover closes the older open ones).
 */
#include "acct_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "db_procs.h"
#include "knsvc/config.h"
#include "knsvc/json.h"
#include "knsvc/log.h"
#include "season.h"

#define MOD "season"

int acct_current_season(void) {
    const int64_t s = g_acct_brain ? kn_config_int(g_acct_brain->cfg, "accounts.current_season", SEASON_CURRENT) : SEASON_CURRENT;
    return s > 0 && s < 1000 ? (int) s : SEASON_CURRENT;
}

static void ensure_done(kn_db_result *res, void *ud) {
    (void) ud;
    if (!kn_db_ok(res)) {
        KN_WARN(MOD, "season %d not recorded: %s", acct_current_season(), kn_db_error(res));
        return;
    }
    cJSON *v = acct_db_json(res);
    KN_INFO(MOD, "season %d%s", acct_current_season(), kn_json_bool(v, "created", 0) ? " started (rollover)" : "");
    cJSON_Delete(v);
}

void season_init(void) {
    if (!g_acct_brain || !g_acct_brain->db) return;
    char season[24];
    kn_db_call(g_acct_brain->db, &P_SEASON_ENSURE, KN_DB_ARGS(acct_itoa(season, acct_current_season())), ensure_done, NULL);
}

static const char *season_arg(char buf[24]) { return acct_itoa(buf, acct_current_season()); }
static const char *now_arg(char buf[24]) { return acct_itoa(buf, (int64_t) time(NULL)); }

/* ------------------------------------------------------------------ "bpdl" */

static void bp_data_done(SocialSession *s, kn_db_result *res, void *extra) {
    (void) extra;
    cJSON *v = kn_db_ok(res) ? acct_db_json(res) : NULL;
    if (!kn_db_ok(res)) acct_db_error(res, "bpdl");
    const cJSON *bp = cJSON_GetObjectItemCaseSensitive(v, "bp");
    const cJSON *pr = cJSON_GetObjectItemCaseSensitive(v, "progress");
    social_send_n(s, "bpdl", 1, bp && !cJSON_IsNull(bp) ? cJSON_Duplicate(bp, 1) : cJSON_CreateNull());
    if (pr && !cJSON_IsNull(pr)) social_send_n(s, "bppd", 1, cJSON_Duplicate(pr, 1));
    cJSON_Delete(v);
}

void sh_bp_data(SocialSession *s, const cJSON *f) {
    (void) f;
    char id[24], season[24];
    social_call(s, &P_BP_DATA, KN_DB_ARGS(acct_itoa(id, s->account_id > 0 ? s->account_id : 0), season_arg(season)), bp_data_done, NULL);
}

/* ------------------------------------------------------------------ "bpclm" */

static void account_pushed(kn_db_result *res, void *ud);

typedef struct {
    int all; /* "bpclmAll": the v10 reply */
} ClaimCtx;

static void bp_claim_done(SocialSession *s, kn_db_result *res, void *extra) {
    ClaimCtx *cx = extra;
    const int all = cx && cx->all;
    if (!kn_db_ok(res)) {
        social_error(s, acct_db_error(res, all ? "bpclmAll" : "bpclm"));
        return;
    }
    cJSON *v = acct_db_json(res);
    const cJSON *items = cJSON_GetObjectItemCaseSensitive(v, "items");
    if (all) {
        /* v10 claimItemRes(_, funds, _, _, junk, items) */
        social_send_n(s, "bpclmr", 6, cJSON_CreateNull(), cJSON_CreateNumber((double) kn_json_int(v, "funds", 0)), cJSON_CreateNull(),
                      cJSON_CreateNull(), cJSON_CreateNumber(0), items ? cJSON_Duplicate(items, 1) : cJSON_CreateArray());
    } else {
        const int sk = (int) kn_json_int(v, "sk", 0);
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "id", (double) kn_json_int(v, "id", 0));
        cJSON_AddNumberToObject(item, "tier", (double) kn_json_int(v, "tier", 0));
        cJSON_AddNumberToObject(item, "col", (double) kn_json_int(v, "col", 0));
        cJSON_AddNumberToObject(item, "sk", sk);
        const cJSON *first = cJSON_GetArrayItem(items, 0);
        if (kn_json_int(first, "kr", 0) > 0) cJSON_AddNumberToObject(item, "kr", (double) kn_json_int(first, "kr", 0));
        /* claimItemRes(_, _, skin, _, _, {id, tier, col, sk}) */
        social_send_n(s, "bpclmr", 6, cJSON_CreateNull(), cJSON_CreateNull(), cJSON_CreateNumber(sk), cJSON_CreateNull(), cJSON_CreateNull(), item);
    }
    const cJSON *pr = cJSON_GetObjectItemCaseSensitive(v, "progress");
    if (pr) social_send_n(s, "bppd", 1, cJSON_Duplicate(pr, 1));
    /* a KR card (v10 krdata) changed the wallet: the header's KR */
    int kr = 0, jnk = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, items) {
        kr += (int) kn_json_int(it, "kr", 0);
        const cJSON *j = cJSON_GetObjectItemCaseSensitive(it, "jnk"); /* a junk card: the wallet changed */
        if (cJSON_IsNumber(j) && j->valuedouble > 0) jnk = 1;
    }
    if (kr > 0 || jnk > 0) { /* the wallet changed: the account is pushed again (its "ua" carries the junk) */
        char id[24];
        kn_db_call(g_acct_brain->db, &P_ACCOUNT_JSON, KN_DB_ARGS(acct_itoa(id, s->account_id)), account_pushed, NULL);
    }
    cJSON_Delete(v);
}

static ClaimCtx *claim_ctx(const int all) {
    ClaimCtx *c = calloc(1, sizeof *c);
    if (c) c->all = all;
    return c;
}

void sh_bp_claim(SocialSession *s, const cJSON *f) {
    if (!social_require_auth(s)) return;
    const int64_t item = fa_int(f, 1, -1);
    if (item <= 0) {
        social_error(s, "Item not found");
        return;
    }
    char id[24], it[24], season[24];
    social_call(s, &P_BP_CLAIM, KN_DB_ARGS(acct_itoa(id, s->account_id), acct_itoa(it, item), season_arg(season), "0"), bp_claim_done,
                claim_ctx(0));
}

void sh_bp_claim_all(SocialSession *s, const cJSON *f) {
    (void) f;
    if (!social_require_auth(s)) return;
    char id[24], season[24];
    social_call(s, &P_BP_CLAIM, KN_DB_ARGS(acct_itoa(id, s->account_id), "0", season_arg(season), "1"), bp_claim_done, claim_ctx(1));
}

/* ------------------------------------------------------------------ "xstbp" / "xstbpl" (KR) */

typedef struct {
    int64_t arg; /* tier / levels */
} BuyCtx;

/* purchaseError(provider, error, product): error.err is the message */
static cJSON *purchase_err(const char *msg) {
    cJSON *e = cJSON_CreateObject();
    cJSON_AddStringToObject(e, "err", msg ? msg : "Purchase Error");
    return e;
}

static BuyCtx *buy_ctx(const int64_t arg) {
    BuyCtx *c = calloc(1, sizeof *c);
    if (c) c->arg = arg;
    return c;
}

static void account_pushed(kn_db_result *res, void *ud) {
    (void) ud;
    cJSON *account = kn_db_ok(res) ? acct_db_json(res) : NULL;
    if (!account) return;
    acct_decorate(account);
    social_push_account_update(account); /* "ua": the header's KR after the KR purchase */
    cJSON_Delete(account);
}

/* The new account.BP, any new badge and the live header (KR) after a purchase. */
static void after_purchase(SocialSession *s, const cJSON *v) {
    const cJSON *badges = cJSON_GetObjectItemCaseSensitive(v, "newBadges");
    if (cJSON_GetArraySize(badges) > 0) social_send_n(s, "nbdgr", 1, cJSON_Duplicate(badges, 1));
    const cJSON *pr = cJSON_GetObjectItemCaseSensitive(v, "progress");
    if (pr) social_send_n(s, "bppd", 1, cJSON_Duplicate(pr, 1));
    char id[24];
    kn_db_call(g_acct_brain->db, &P_ACCOUNT_JSON, KN_DB_ARGS(acct_itoa(id, s->account_id)), account_pushed, NULL);
}

static void bp_buy_done(SocialSession *s, kn_db_result *res, void *extra) {
    BuyCtx *c = extra;
    char tier[24];
    acct_itoa(tier, c->arg);
    if (!kn_db_ok(res)) {
        /* purchaseRes(provider, error, product) -> purchaseError */
        social_send_n(s, "purbp", 3, cJSON_CreateString("kr"), purchase_err(acct_db_error(res, "xstbp")), cJSON_CreateString(tier));
        return;
    }
    cJSON *v = acct_db_json(res);
    char tx[48];
    snprintf(tx, sizeof tx, "bp-%lld-%lld", (long long) s->account_id, (long long) time(NULL));
    const cJSON *gift = cJSON_GetObjectItemCaseSensitive(v, "gift");
    if (cJSON_IsObject(gift)) {
        /* v10 purchaseRes(provider, error, product, tier, price, currency, transactionId,
         * externalId, _, gifted, recipient): "Battlepass Gifted to <name>!" */
        social_send_n(s, "purbp", 11, cJSON_CreateString("kr"), cJSON_CreateNull(), cJSON_CreateNull(),
                      cJSON_CreateNumber((double) kn_json_int(v, "tier", c->arg)), cJSON_CreateNumber((double) kn_json_int(v, "price", 0)),
                      cJSON_CreateString("KR"), cJSON_CreateString(tx), cJSON_CreateNull(), cJSON_CreateNull(), cJSON_CreateBool(1),
                      cJSON_CreateString(kn_json_str(gift, "name", "")));
        /* the recipient's open sessions: their pass and badges now, the mail count */
        const int64_t to = kn_json_int(gift, "id", 0);
        const cJSON *gp = cJSON_GetObjectItemCaseSensitive(gift, "progress");
        if (to > 0 && gp) {
            cJSON *fr = social_frame("bppd");
            cJSON_AddItemToArray(fr, cJSON_Duplicate(gp, 1));
            social_push_account_ex(to, fr, 1);
        }
        const cJSON *gb = cJSON_GetObjectItemCaseSensitive(gift, "newBadges");
        if (to > 0 && cJSON_GetArraySize(gb) > 0) {
            cJSON *fr = social_frame("nbdgr");
            cJSON_AddItemToArray(fr, cJSON_Duplicate(gb, 1));
            social_push_account_ex(to, fr, 1);
        }
        if (to > 0) social_push_mail_count(to);
    } else {
        /* purchaseRes(provider, error, product, tier, price, currency, transactionId, externalId) */
        social_send_n(s, "purbp", 8, cJSON_CreateString("kr"), cJSON_CreateNull(), cJSON_CreateNull(),
                      cJSON_CreateNumber((double) kn_json_int(v, "tier", c->arg)), cJSON_CreateNumber((double) kn_json_int(v, "price", 0)),
                      cJSON_CreateString("KR"), cJSON_CreateString(tx), cJSON_CreateNull());
    }
    after_purchase(s, v);
    cJSON_Delete(v);
}

void sh_bp_buy(SocialSession *s, const cJSON *f) {
    const int64_t tier = fa_int(f, 1, 1);
    if (s->account_id <= 0) {
        social_send_n(s, "purbp", 3, cJSON_CreateString("kr"), purchase_err("Must be logged in"), cJSON_CreateString("1"));
        return;
    }
    BuyCtx *c = buy_ctx(tier);
    if (!c) return;
    char id[24], t[24], season[24], now[24], gift[64] = "";
    /* v10 selectBPTier(el, tier, steamId, giftName): io.send("xstbp", tier, lang, null, null, channel, giftName) */
    const char *g = fa_str(f, 6);
    if (g && *g) snprintf(gift, sizeof gift, "%s", g);
    social_call(s, &P_BP_BUY, KN_DB_ARGS(acct_itoa(id, s->account_id), acct_itoa(t, tier), season_arg(season), now_arg(now), gift[0] ? gift : NULL),
                bp_buy_done, c);
}

static void bp_skip_done(SocialSession *s, kn_db_result *res, void *extra) {
    BuyCtx *c = extra;
    if (!kn_db_ok(res)) {
        /* purchaseLvlRes(provider, error, _, levels) -> purchaseError(.., "battlepass_levels_" + levels) */
        social_send_n(s, "purbpl", 4, cJSON_CreateString("kr"), purchase_err(acct_db_error(res, "xstbpl")), cJSON_CreateNull(),
                      cJSON_CreateNumber((double) c->arg));
        return;
    }
    cJSON *v = acct_db_json(res);
    char tx[48];
    snprintf(tx, sizeof tx, "bpl-%lld-%lld", (long long) s->account_id, (long long) time(NULL));
    const cJSON *badges = cJSON_GetObjectItemCaseSensitive(v, "newBadges");
    const cJSON *pr = cJSON_GetObjectItemCaseSensitive(v, "progress");
    /* purchaseLvlRes(provider, error, _, levels, bpp, price, currency, transactionId, externalId, badge) */
    social_send_n(s, "purbpl", 10, cJSON_CreateString("kr"), cJSON_CreateNull(), cJSON_CreateNull(),
                  cJSON_CreateNumber((double) kn_json_int(v, "levels", c->arg)), cJSON_CreateNumber((double) kn_json_int(pr, "bpp", 0)),
                  cJSON_CreateNumber((double) kn_json_int(v, "price", 0)), cJSON_CreateString("KR"), cJSON_CreateString(tx), cJSON_CreateNull(),
                  cJSON_GetArraySize(badges) > 0 ? cJSON_Duplicate(cJSON_GetArrayItem(badges, 0), 1) : cJSON_CreateNull());
    after_purchase(s, v);
    cJSON_Delete(v);
}

void sh_bp_skip(SocialSession *s, const cJSON *f) {
    const int64_t levels = fa_int(f, 1, 1);
    if (s->account_id <= 0) {
        social_send_n(s, "purbpl", 4, cJSON_CreateString("kr"), purchase_err("Must be logged in"), cJSON_CreateNull(),
                      cJSON_CreateNumber((double) levels));
        return;
    }
    BuyCtx *c = buy_ctx(levels);
    if (!c) return;
    char id[24], l[24], season[24], now[24];
    social_call(s, &P_BP_SKIP, KN_DB_ARGS(acct_itoa(id, s->account_id), acct_itoa(l, levels), season_arg(season), now_arg(now)),
                bp_skip_done, c);
}

/* ------------------------------------------------------------------ "sst" (profile season block) */

static void season_stats_done(SocialSession *s, kn_db_result *res, void *extra) {
    (void) extra;
    cJSON *v = kn_db_ok(res) ? acct_db_json(res) : NULL;
    if (!kn_db_ok(res)) acct_db_error(res, "sst");
    social_send_n(s, "sst", 1, v ? v : cJSON_CreateNull());
}

void sh_season_stats(SocialSession *s, const cJSON *f) {
    const cJSON *who = cJSON_GetArrayItem(f, 1);
    char name[64], season[24];
    if (cJSON_IsString(who) && who->valuestring && who->valuestring[0]) snprintf(name, sizeof name, "%s", who->valuestring);
    else if (s->account_id > 0) acct_itoa(name, s->account_id);
    else {
        social_send_n(s, "sst", 1, cJSON_CreateNull());
        return;
    }
    const int64_t want = fa_int(f, 2, 0);
    acct_itoa(season, want > 0 && want <= acct_current_season() ? want : acct_current_season());
    social_call(s, &P_SEASON_STATS, KN_DB_ARGS(name, season), season_stats_done, NULL);
}
