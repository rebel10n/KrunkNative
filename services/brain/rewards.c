/*
 * services/brain/rewards.c - Krunker v10.0.2's one-time signup perks on the social socket
 * (doc/backend.md 7.9). v10's "Get Signup Rewards" popup (showSignupRewards: "1-time signup
 * perks: Gain 14 Cosmetics + 1 Day premium for free!") sends a guest to registration; after the
 * account is made, v10's server pushes POPUP_REGISTER_REWARDS ("prr", skinRewards,
 * premiumRewards) and the client shows "Welcome to the Family!". KrunkNative's menu talks to
 * kn_brain directly: the social socket's registration ("a" action 0, the menu and the web
 * build) asks P_SIGNUP_REWARDS right after the account is made, which grants once per account
 * (db_rewards.c's primary key); a later login, a saved-token login at startup or an account
 * made elsewhere (internal ops, a central account) gets no popup, as in v10.
 *
 *   (registration) -> ["prr", [skin...], premium] + "ua" (the new inventory and premiumT)
 *
 * Config (accounts.signup_rewards.*): enabled (true), skins (the 14 cosmetics; v10's list is
 * server data, KrunkNative picks 14 common store items below), premium_hours (24), sweep_s (600:
 * how often the premium that ran out is taken back).
 */
#include "acct_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "db_procs.h"
#include "knsvc/config.h"
#include "knsvc/json.h"
#include "knsvc/log.h"
#include "knsvc/loop.h"

#define MOD "rewards"

/* KrunkNative's 14 signup cosmetics (store.skins indices, all common / uncommon, sold in the
 * store): Cowboy Hat, Hard Hat, Sun Glasses, Red Bandana, Soldier Pack, Red Backpack, Police
 * Vest, the Terminal and Wink sprays, and the Digital Hunt / AK / SMG / Python / SG skins. */
static const int k_default_skins[] = {113, 121, 119, 283, 260, 832, 834, 866, 871, 3, 15, 26, 36, 46};

static kn_timer_id s_sweep_timer;

static int enabled(void) {
    return g_acct_brain && kn_config_bool(g_acct_brain->cfg, "accounts.signup_rewards.enabled", 1);
}

/* "a,b,c" of accounts.signup_rewards.skins (numbers), else the default list */
static void skins_arg(char *out, const size_t size) {
    out[0] = 0;
    size_t len = 0;
    const cJSON *list = g_acct_brain ? kn_config_get(g_acct_brain->cfg, "accounts.signup_rewards.skins") : NULL;
    if (cJSON_IsArray(list)) {
        const cJSON *it;
        cJSON_ArrayForEach(it, list) {
            if (!cJSON_IsNumber(it) || it->valuedouble < 0) continue;
            const int w = snprintf(out + len, size - len, "%s%d", len ? "," : "", it->valueint);
            if (w < 0 || (size_t) w >= size - len) break;
            len += (size_t) w;
        }
        return;
    }
    for (size_t i = 0; i < sizeof k_default_skins / sizeof k_default_skins[0]; i++) {
        const int w = snprintf(out + len, size - len, "%s%d", len ? "," : "", k_default_skins[i]);
        if (w < 0 || (size_t) w >= size - len) break;
        len += (size_t) w;
    }
}

static int64_t premium_hours(void) {
    const int64_t h = g_acct_brain ? kn_config_int(g_acct_brain->cfg, "accounts.signup_rewards.premium_hours", 24) : 24;
    return h > 0 && h < 24 * 365 ? h : 0;
}

static void account_pushed(kn_db_result *res, void *ud) {
    (void) ud;
    cJSON *account = kn_db_ok(res) ? acct_db_json(res) : NULL;
    if (!account) return;
    acct_decorate(account);
    social_push_account_update(account); /* "ua": the inventory (data[10]) and premiumT */
    cJSON_Delete(account);
}

static void signup_done(SocialSession *s, kn_db_result *res, void *extra) {
    (void) extra;
    if (!kn_db_ok(res)) {
        KN_WARN(MOD, "signup rewards of account %lld: %s", (long long) s->account_id, kn_db_error(res));
        return;
    }
    cJSON *v = acct_db_json(res);
    if (kn_json_bool(v, "granted", 0)) {
        const cJSON *skins = cJSON_GetObjectItemCaseSensitive(v, "skins");
        KN_INFO(MOD, "signup rewards for account %lld: %d cosmetics%s", (long long) s->account_id, cJSON_GetArraySize(skins),
                kn_json_bool(v, "premium", 0) ? " + premium" : "");
        /* v10 POPUP_REGISTER_REWARDS(skinRewards, premiumRewards) -> registerRewardsPop */
        social_send_n(s, "prr", 2, skins ? cJSON_Duplicate(skins, 1) : cJSON_CreateArray(),
                      cJSON_CreateBool(kn_json_bool(v, "premium", 0)));
        char id[24];
        kn_db_call(g_acct_brain->db, &P_ACCOUNT_JSON, KN_DB_ARGS(acct_itoa(id, s->account_id)), account_pushed, NULL);
    }
    cJSON_Delete(v);
}

void rewards_after_register(SocialSession *s) {
    if (!s || s->account_id <= 0 || !enabled()) return;
    char id[24], skins[512], ms[24], secs[24];
    skins_arg(skins, sizeof skins);
    const int64_t hours = premium_hours();
    social_call(s, &P_SIGNUP_REWARDS,
                KN_DB_ARGS(acct_itoa(id, s->account_id), skins, acct_itoa(ms, hours * 3600 * 1000), acct_itoa(secs, hours * 3600)),
                signup_done, NULL);
}

static void sweep_done(kn_db_result *res, void *ud) {
    (void) ud;
    if (!kn_db_ok(res)) {
        KN_WARN(MOD, "signup premium sweep: %s", kn_db_error(res));
        return;
    }
    cJSON *v = acct_db_json(res);
    if (kn_json_int(v, "swept", 0) > 0) KN_INFO(MOD, "signup premium ended for %lld accounts", (long long) kn_json_int(v, "swept", 0));
    cJSON_Delete(v);
}

static void sweep_tick(kn_loop *loop, void *ud) {
    (void) loop;
    (void) ud;
    if (g_acct_brain && g_acct_brain->db) kn_db_call(g_acct_brain->db, &P_SIGNUP_PREMIUM_SWEEP, 0, NULL, sweep_done, NULL);
}

void rewards_init(Brain *b) {
    if (!b || !b->loop) return;
    int64_t every = kn_config_int(b->cfg, "accounts.signup_rewards.sweep_s", 600);
    if (every < 1) every = 600;
    s_sweep_timer = kn_loop_add_timer(b->loop, (uint64_t) (every < 30 ? every : 30) * 1000, (uint64_t) every * 1000, sweep_tick, NULL);
}

void rewards_shutdown(Brain *b) {
    if (b && b->loop && s_sweep_timer) kn_loop_cancel_timer(b->loop, s_sweep_timer);
    s_sweep_timer = 0;
}
