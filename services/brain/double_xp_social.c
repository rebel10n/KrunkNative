/*
 * services/brain/double_xp_social.c - Krunker v10.0.2's 2x XP claim over the social socket
 * (double_xp.h, db_double_xp.c). Owner: svc-accounts.
 *
 *   ["rqdxp"]                       v10 "rqdxp" (after the end-screen offer's ad)
 *   ["rqdxp", {"ad": "<proof>"}]    the same with a real ad source's proof (optional)
 *       -> ["rrqdxp", {playerScore, xp, games}]     v10 "rrqdxp" {playerScore}: the match doubled
 *       -> ["rrqdxp", null, error]                  nothing to double, the ad refused or pending
 *
 * accounts.double_xp.ad_provider: "placeholder" (default) grants the claim at once with no ad shown
 * or watched; "off" refuses every claim. A real ad source replaces dxp_provider_verify.
 * The offer is the account's last match (db_accounts.c records it at the commit); the claim rule
 * is v10's (games % 3 == 0, once per games count).
 */
#include "acct_internal.h"
#include "db_procs.h"
#include "double_xp.h"

#include <stdio.h>
#include <string.h>

#include "knsvc/config.h"
#include "knsvc/json.h"
#include "knsvc/loop.h"

#define DXP_REF_MAX 64

static DxpProvider dxp_provider(void) {
    const kn_config *cfg = g_acct_brain ? g_acct_brain->cfg : NULL;
    if (!cfg) return DXP_PROVIDER_PLACEHOLDER;
    return dxp_provider_parse(kn_config_str(cfg, "accounts.double_xp.ad_provider", "placeholder"));
}

static void claim_done(SocialSession *s, kn_db_result *res, void *extra) {
    (void) extra;
    cJSON *v = kn_db_ok(res) ? acct_db_json(res) : NULL;
    if (!v) {
        social_send_n(s, "rrqdxp", 2, cJSON_CreateNull(), cJSON_CreateString(acct_db_error(res, "double XP")));
        return;
    }
    social_send_n(s, "rrqdxp", 1, v);
}

static void refuse(SocialSession *s, const char *msg) {
    social_send_n(s, "rrqdxp", 2, cJSON_CreateNull(), cJSON_CreateString(msg));
}

/* ["rqdxp"] / ["rqdxp", {ad}] */
void sh_double_xp_claim(SocialSession *s, const cJSON *f) {
    if (s->account_id <= 0) {
        refuse(s, "Must be logged in");
        return;
    }
    const cJSON *arg = fa(f, 1);
    char ad_ref[DXP_REF_MAX + 1];
    snprintf(ad_ref, sizeof ad_ref, "%s", kn_json_str(arg, "ad", ""));
    const DxpProvider provider = dxp_provider();
    switch (dxp_provider_verify(provider, ad_ref)) {
    case DXP_VERDICT_GRANTED:
        break;
    case DXP_VERDICT_PENDING:
        refuse(s, "The ad is not finished");
        return;
    case DXP_VERDICT_REFUSED:
    default:
        refuse(s, "Rewarded ads are not available right now");
        return;
    }
    /* the placeholder watches nothing: it records no proof (a real provider's proof is kept) */
    const char *proof = provider != DXP_PROVIDER_PLACEHOLDER && ad_ref[0] ? ad_ref : NULL;
    const int64_t now = kn_wall_ms();
    char id[24], nw[24];
    social_call(s, &P_DXP_CLAIM, KN_DB_ARGS(acct_itoa(id, s->account_id), acct_itoa(nw, now), dxp_provider_name(provider), proof),
                claim_done, NULL);
}
