// Double XP (client/include/double_xp_offer.h): v10's end-screen 2x XP claim over the social socket
// ("rqdxp" / "rrqdxp"), with the account's dxp.offer the brain keeps.
#include <double_xp_offer.h>
#include "klog.h"

#include <backend.h>
#include <backend_ui.h>
#include <html_ui.h>

#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CLAIM_TIMEOUT_MS 15000.0 // a claim with no answer (the socket never opened, the brain is gone)

typedef struct {
    DoubleXpDoneFn done;
    void *user;
    int timer;
} Claim;

static Claim *g_pending;      // the one claim in flight (its answer is rrqdxp)
static int g_listening;       // on_social is on the social socket
static double g_taken = -1;   // the games count the last claim was made for
int double_xp_offer_open(void) {
    const AccountInfo *a = active_account();
    return a && a->offer_claimable && !g_pending;
}

int double_xp_taken(void) {
    const AccountInfo *a = active_account();
    return a && g_taken == a->games;
}

static void finish(const DoubleXpResult result) {
    Claim *c = g_pending;
    if (!c) return;
    g_pending = NULL;
    if (c->timer) html_timer_cancel(c->timer);
    DoubleXpDoneFn done = c->done;
    void *user = c->user;
    free(c);
    done(user, result);
}

static void on_timeout(void *user) {
    (void) user;
    if (!g_pending) return;
    g_pending->timer = 0;
    printf("[double_xp] no answer to rqdxp\n");
    finish(DOUBLE_XP_ERROR);
}

// the social socket's frames: rrqdxp answers the claim in flight
static void on_social(void *user, const cJSON *frame) {
    (void) user;
    if (!g_pending || !cJSON_IsArray(frame)) return;
    const char *label = cJSON_GetStringValue(cJSON_GetArrayItem(frame, 0));
    if (!label || strcmp(label, "rrqdxp")) return;
    const cJSON *answer = cJSON_GetArrayItem(frame, 1);
    if (!cJSON_IsObject(answer)) {
        const char *message = cJSON_GetStringValue(cJSON_GetArrayItem(frame, 2));
        printf("[double_xp] claim refused: %s\n", message ? message : "");
        finish(DOUBLE_XP_ERROR);
        return;
    }
    // v10's RECIEVE_DOUBLE_XP: account.score = playerScore, and lastDoubleAttempt = games (the offer is spent)
    const cJSON *score = cJSON_GetObjectItem(answer, "playerScore");
    if (!cJSON_IsNumber(score)) {
        finish(DOUBLE_XP_ERROR);
        return;
    }
    account_double_xp_claimed(score->valuedouble);
    g_taken = active_account() ? active_account()->games : -1;
    KN_LOG_DEBUG1("[double_xp] doubled: score %.0f\n", score->valuedouble);
    finish(DOUBLE_XP_GRANTED);
}

// ["rqdxp"] on the social socket
static void send_claim(void) {
    if (!g_listening) g_listening = backend_social_listen(on_social, NULL) ? 1 : 0;
    cJSON *frame = cJSON_CreateArray();
    cJSON_AddItemToArray(frame, cJSON_CreateString("rqdxp"));
    const int sent = g_listening && backend_social_send(frame);
    cJSON_Delete(frame);
    if (!sent) {
        finish(DOUBLE_XP_ERROR);
        return;
    }
    if (g_pending) g_pending->timer = html_timer_add(CLAIM_TIMEOUT_MS, on_timeout, NULL);
}

// The claim of either button. done runs once, when rrqdxp is answered, or at once when nothing is sent.
static void start_claim(DoubleXpDoneFn done, void *user, const double games) {
    KN_LOG_DEBUG1("[double_xp] rqdxp at games %.0f\n", games);
    if (backend_available() != 1) {
        done(user, DOUBLE_XP_ERROR);
        return;
    }
    Claim *c = calloc(1, sizeof(*c));
    if (!c) {
        done(user, DOUBLE_XP_ERROR);
        return;
    }
    c->done = done;
    c->user = user;
    g_pending = c;
    send_claim();
}

// Krunker Strike's banner: claimed while the account's offer is claimable
void double_xp_claim(DoubleXpDoneFn done, void *user) {
    if (!done) return;
    const AccountInfo *a = active_account();
    if (!double_xp_offer_open()) {
        done(user, DOUBLE_XP_NOT_OFFERED);
        return;
    }
    start_claim(done, user, a->games);
}

// Box Office's end card: v10's claimDoubleXpReward (v10.js 222668) claims when the account's games is a multiple of
// 3 above zero, with an account id (a guest's click claims nothing); the offer's state does not decide it. The
// brain still refuses a match it has doubled already (dxp_offer_claimable: claimed_games != games).
void double_xp_end_claim(DoubleXpDoneFn done, void *user) {
    if (!done) return;
    const AccountInfo *a = active_account();
    if (!a || g_pending || a->games <= 0 || (long long) a->games % 3 != 0) {
        done(user, DOUBLE_XP_NOT_OFFERED);
        return;
    }
    start_claim(done, user, a->games);
}
