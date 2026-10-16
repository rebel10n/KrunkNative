/*
 * services/brain/strike_econ.h - Krunker Strike's economy and progression in kn_brain: FRVR's
 * gateway routes (/v1/item-definitions, /v2/spin-wheels, /v1/shop, /v1/missions,
 * /v1/players/me, /v1/game-config) under /strike on the gapi tunnel of the social socket, the
 * Strike catalogue read from migration 0320's tables, a Strike game server's join (its inventory
 * is the account's Strike items) and its match commits (Strike XP, krunkies, missions, stats).
 * Strike's data is keyed by account but kept apart from Box Office's (doc/v10/krunker-strike.md).
 */
#ifndef KN_BRAIN_STRIKE_ECON_H
#define KN_BRAIN_STRIKE_ECON_H

#include <stdint.h>

#include "cJSON.h"
#include "brain.h"
#include "accounts.h"
#include "knsvc/db.h"

struct SocialSession;
struct GapiReq;

/* BrainModule hooks (main.c): the catalogue load + reload timer */
int strike_econ_init(Brain *b);
void strike_econ_shutdown(Brain *b);

/* /strike/... requests of the gapi tunnel (social_gapi.c): 1 = handled */
int strike_econ_gapi(struct SocialSession *s, const struct GapiReq *r);

/* A Strike game server's join (internal_ws.c auth with "flavor": "strike"): `account` (the
 * account object the brain answers with) gets Strike's inventory as data[10] / "skins" ({ind, cnt}
 * of the store skins the account's Strike items wear), "funds" = its krunkies, "level" = its Strike
 * level, "strike" = {xp, level, loadout}. cb(ud, err, account) with the changed copy. */
void strike_econ_overlay(Brain *b, const cJSON *account, AcctReplyFn cb, void *ud);

/* A Strike game server's commit of one player (internal_ws.c commit with "flavor": "strike"):
 * {accountId, kills, deaths, score, won, games, timePlayed, kr, xp, prog}. Idempotent per
 * match_key. cb data: {accountId, xp, level, levelUp, kr, duplicate}. */
void strike_econ_commit(Brain *b, int64_t account_id, const cJSON *player, const char *match_key, AcctReplyFn cb, void *ud);

/* HTTP GET of a Strike leaderboard ({data: [{position, player_name, player_clan, value, level}]},
 * v10's /api/leaderboards shape): /api/strike/leaderboards/:type and /api/leaderboards/:type?flavor=strike
 * (leaderboards.c). type: xp | level | score | player_score | kills | player_kills | wins | player_wins | games. */
struct kn_http_req;
void strike_econ_http_board(Brain *b, struct kn_http_req *req, const char *type);

/* db_strike.c */
extern const kn_proc P_STRIKE_CATALOG, P_STRIKE_ME, P_STRIKE_EQUIP, P_STRIKE_SPIN, P_STRIKE_PURCHASE, P_STRIKE_MISSIONS,
    P_STRIKE_MISSION_CLAIM, P_STRIKE_TRACK_CLAIM, P_STRIKE_COMMIT, P_STRIKE_OVERLAY, P_STRIKE_LEADERBOARD, P_STRIKE_FEEDBACK;

#endif
