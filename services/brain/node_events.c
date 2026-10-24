/*
 * services/brain/node_events.c - the game servers' Krunker v10.0.3 events that need the brain (node_events.h): the
 * ops they ask (Turf Wars clan, map prize wheel spin, a studio game's monetization and charges) and the pushes to the
 * node an account plays on (unboxings, raffle wins).
 *
 * Line numbers are deob-10.0.3.js (/home/user/jails/kru/src/Krunker_v10.0.3/deob/game.deob.js). 10.0.3's client shows
 * "unb" UNBOX_MESSAGE [name, itemIndex, kr] (:254321) and "lotWin" LOTTERY_PRIZE_WON [lotteryId, entryId, prize,
 * playerId] (:255154) as chat lines of the game it plays; the bundle does not have the server side that sends them.
 * KrunkNative's choice: the brain, where the store spins, Free KR spins and lottery draws happen, tells the game
 * server the account plays on (game_presence: the player-join / player-leave of this brain's nodes), and that server
 * writes the line to its players (server/src/brain_events.c). With brain.federation a player on another brain's node
 * gets no line (game_presence's limit; the deployment runs one coordinator).
 */
#include "node_events.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "acct_internal.h"
#include "brain_internal.h"
#include "game_presence.h"
#include "studios.h"
#include "knsvc/json.h"
#include "knsvc/log.h"

#define MOD "node-events"

/* ------------------------------------------------------------------ pushes */

/* the account's node: 1 with node_id set, 0 when it plays nowhere on this brain */
static int node_of(int64_t account_id, char *node_id, size_t n) {
    char game_id[128];
    if (account_id <= 0 || !game_presence_get(account_id, game_id, sizeof game_id, NULL)) return 0;
    const char *colon = strchr(game_id, ':'); /* "<nodeId>:<lobby>" (node ids have no ':') */
    const size_t len = colon ? (size_t) (colon - game_id) : strlen(game_id);
    if (!len || len >= n) return 0;
    memcpy(node_id, game_id, len);
    node_id[len] = 0;
    return 1;
}

static void push(int64_t account_id, cJSON *frame) { /* consumes frame */
    char node[96];
    if (g_acct_brain && node_of(account_id, node, sizeof node) && internal_ws_send(g_acct_brain, node, frame) != 0)
        KN_DEBUG(MOD, "%s for account %lld: node %s is gone", kn_json_str(frame, "type", "?"), (long long) account_id, node);
    cJSON_Delete(frame);
}

void node_events_unbox(int64_t account_id, int item, int kr, int rarity) {
    if (item < 0) return;
    cJSON *f = cJSON_CreateObject();
    cJSON_AddStringToObject(f, "type", "unbox");
    cJSON_AddNumberToObject(f, "accountId", (double) account_id);
    cJSON_AddNumberToObject(f, "item", item);
    cJSON_AddNumberToObject(f, "kr", kr > 0 ? kr : 0);
    cJSON_AddNumberToObject(f, "rarity", rarity);
    push(account_id, f);
}

void node_events_raffle_win(int64_t account_id, int64_t lottery_id, int64_t draw_id, const char *type, const char *ref,
                            double quantity) {
    cJSON *f = cJSON_CreateObject();
    cJSON_AddStringToObject(f, "type", "raffle-win");
    cJSON_AddNumberToObject(f, "accountId", (double) account_id);
    cJSON_AddNumberToObject(f, "lotteryId", (double) lottery_id);
    cJSON_AddNumberToObject(f, "drawId", (double) draw_id);
    cJSON *prize = cJSON_AddObjectToObject(f, "prize");
    cJSON_AddStringToObject(prize, "type", type ? type : "");
    cJSON_AddStringToObject(prize, "ref", ref ? ref : "");
    cJSON_AddNumberToObject(prize, "quantity", quantity);
    push(account_id, f);
}

/* ------------------------------------------------------------------ studio games (db_studios.c) */

typedef struct StudioCtx {
    AcctReplyFn cb;
    void *ud;
    int charge;
} StudioCtx;

/* a studio_charge refusal code -> the game server's text (logged by it; the script only sees success false) */
static const char *charge_error(const char *code) {
    if (!strcmp(code, "MONETIZATION_DISABLED")) return "Monetization is not enabled for this studio";
    if (!strcmp(code, "STUDIO_CHARGE_INVALID")) return "Invalid charge";
    return code;
}

static void on_studio(kn_db_result *res, void *ud) {
    StudioCtx *c = ud;
    if (!kn_db_ok(res)) {
        c->cb(c->ud, charge_error(acct_db_error(res, c->charge ? "studio charge" : "studio game")), NULL);
        free(c);
        return;
    }
    cJSON *data = acct_db_json(res);
    if (!data) data = cJSON_CreateObject();
    cJSON *account = cJSON_DetachItemFromObjectCaseSensitive(data, "account");
    if (account && !kn_json_int(data, "duplicate", 0)) { /* the menu's KR follows the charge ("ua") */
        acct_decorate(account);
        social_push_account_update(account);
    }
    cJSON_Delete(account);
    c->cb(c->ud, NULL, data);
    cJSON_Delete(data);
    free(c);
}

static int studio_op(int charge, const cJSON *args, AcctReplyFn cb, void *ud) {
    if (!g_acct_brain || !g_acct_brain->db) {
        cb(ud, "Accounts are unavailable.", NULL);
        return 0;
    }
    StudioCtx *c = calloc(1, sizeof *c);
    if (!c) {
        cb(ud, "Server error.", NULL);
        return 0;
    }
    c->cb = cb;
    c->ud = ud;
    c->charge = charge;
    char id[24], amount[24];
    if (charge)
        kn_db_call(g_acct_brain->db, &P_STUDIO_CHARGE,
                   KN_DB_ARGS(acct_itoa(id, kn_json_int(args, "id", 0)), kn_json_str(args, "map", ""),
                              acct_itoa(amount, kn_json_int(args, "amount", 0)), kn_json_str(args, "key", "")),
                   on_studio, c);
    else
        kn_db_call(g_acct_brain->db, &P_STUDIO_GAME, KN_DB_ARGS(kn_json_str(args, "map", "")), on_studio, c);
    return 0;
}

/* ------------------------------------------------------------------ ops */

int node_events_op(const char *op, const cJSON *args, AcctReplyFn cb, void *ud) {
    if (!op) return 0;
    if (!strcmp(op, "turf_clan")) return turf_clan_op(args, cb, ud), 1;
    if (!strcmp(op, "map_spin")) return store_map_spin_op(args, cb, ud), 1;
    if (!strcmp(op, "studio_game")) return studio_op(0, args, cb, ud), 1;
    if (!strcmp(op, "studio_charge")) return studio_op(1, args, cb, ud), 1;
    return 0;
}
