/*
 * services/brain/accounts.h - accounts, sessions, profiles and stats of the
 * brain (Krunker v3.1.1 server/accounts.ts + the account dbOps of
 * matchmaker/dbBroker.ts), on any store driver (services/db/migrations/<dialect>/01xx_*.sql).
 *
 * Two entry points:
 *   - the social WebSocket (social_ws.h) for clients (login/register/...),
 *   - accounts_internal_call() for the internal game-server WebSocket
 *     (svc-matchmaker's internalWs routes {"type":"acct","rid",op,args} here and
 *     answers {"type":"acct-r","rid","err","data"} from the callback).
 *
 * Internal ops (args/data are JSON objects; doc/backend.md "Accounts & social"):
 *   login    {name, pass, ip}            -> account (+ new session token)
 *   register {name, pass, ip, ref}       -> account (+ new session token)
 *   token    {token, name?}              -> account (session lookup; alias "session")
 *   get      {id}                        -> account
 *   logout   {token}                     -> {}
 *   alias    {id, alias}                 -> account
 *   password {id, old, new}              -> {token}  (other sessions revoked)
 *   stats    {id, match, kills, deaths, score, won, games, time_ms|timePlayed,
 *             kr, xp, class|classIndex, mode, map, region, server, stats{}}
 *                                        -> account + {kr_awarded, duplicate}
 *   rewards  {items:[[id,kr],...]}       -> {funds:[...]}
 *   give_skin {id, skin, source?}        -> {skin, count}
 *   elo      {id, mode:1|2|4, elo}       -> {}
 *   challenge {id, chal, kr}             -> {chal, funds}
 *   ban_check {ip, id?}                  -> {banned, reason, expires}
 * Account object: {id, username, alias, level, levelProg, score, funds, clan,
 *   clan_rank, premium, dev, mod, hack, featured, banned, ban_reason,
 *   ban_expires, elo, elo2, elo4, chal, data:[40-slot Account.getData()], token?}
 */
#ifndef KN_BRAIN_ACCOUNTS_H
#define KN_BRAIN_ACCOUNTS_H

#include <stdint.h>

#include "cJSON.h"
#include "brain.h"

/* err: NULL on success, else a user-facing message (forward it verbatim to the
 * client, e.g. as the "a" packet error). data: borrowed for the duration of the
 * call (NULL on error). Called exactly once per accounts_internal_call(), on the
 * loop thread, possibly synchronously from inside accounts_internal_call(). */
typedef void (*AcctReplyFn)(void *ud, const char *err, const cJSON *data);

/* Returns 0 when the op was accepted (cb will fire), -1 for an unknown op or no
 * database (cb has already been called with an error in both cases). */
int accounts_internal_call(Brain *b, const char *op, const cJSON *args, AcctReplyFn cb, void *ud);

/* BrainModule hooks (main.c k_brain_modules). */
int accounts_init(Brain *b);
void accounts_shutdown(Brain *b);

/* Krunker level from XP (config.rankVar 0.03): max(1, floor(rankVar*sqrt(score))). */
int accounts_level(int64_t score, int *prog_percent);

#endif
