/*
 * shared/include/double_xp.h - Krunker v10.0.2's double XP (migration 0341_double_xp): the rules the
 * brain applies (services/brain/db_double_xp.c, db_accounts.c) and the game server's per-round class
 * flag (server/src/accounts.c). Pure rules: no database, no clock (the callers pass now_ms).
 *
 * v10.0.2 (the reference; v7.1.4 kept the same rules):
 *   - 2x XP is an end-screen offer after each match (doubleXPButton, v10.js 203200): a rewarded ad,
 *     then "rqdxp" -> "rrqdxp" {playerScore}, the match's score doubled. The client shows the offer
 *     unless the match was a competitive one (isComp shows "Return to Krunker" instead) and sends
 *     the claim only when account.games % 3 == 0 (claimDoubleXpReward, v10.js 222670); the claim
 *     records lastDoubleAttempt = games, so one match is doubled at most once. The claim sets
 *     account.score = playerScore and nothing else (the RECIEVE_DOUBLE_XP handler): no clan score,
 *     no class score.
 *   - 2x class XP (isDoubleClassXP = UTILS.isDoubleClassXP: Saturday and Sunday of an even ISO week,
 *     UTC) doubles the class score of each score event of a match (game.js score, the server side).
 *     The flag is evaluated when the game turns IN_PROGRESS (updateEventFlags, from setGameState and
 *     the wait ticks before it), so a match keeps the flag of its start.
 *   - 2x raid drops (isDoubleRaidDrops, every Saturday and Sunday, UTC) doubles raid drops. The
 *     brain has no raid drops; the flag is reported in the account's "dxp" object.
 * mode.doubleXP is a mode flag (aon, md): in-game, already in the score the brain receives.
 *
 * The placeholder ad provider (accounts.double_xp.ad_provider, default "placeholder") stands in for
 * the ad: its verdict is granted at once, so the offer is claimed at once with no ad shown. "off"
 * refuses every claim. A real ad source replaces dxp_provider_verify.
 */
#ifndef KN_SHARED_DOUBLE_XP_H
#define KN_SHARED_DOUBLE_XP_H

#include <stdint.h>

/* v10's claimDoubleXpReward: the offer is claimed when games % 3 == 0 (and games > 0). */
#define DXP_CLAIM_EVERY 3
/* the XP columns are 32-bit integers */
#define DXP_XP_MAX 2147483647LL

typedef struct DxpSchedule {
    int double_class_xp;   /* v10 isDoubleClassXP (now) */
    int double_raid_drops; /* v10 isDoubleRaidDrops (now) */
} DxpSchedule;

/* The v10 schedules at now_ms (UTC). */
DxpSchedule dxp_schedule(int64_t now_ms);
/* The ISO 8601 week number and the UTC weekday (0 Sunday .. 6 Saturday) of now_ms. */
int dxp_iso_week(int64_t now_ms);
int dxp_weekday(int64_t now_ms);
/* Days since 1970-01-01 of a civil date (proleptic Gregorian). */
int64_t dxp_days_from_civil(int64_t year, unsigned month, unsigned day);

/* The class XP of a match: its XP, doubled while doubleClassXP applies (v10 players.js). */
int64_t dxp_class_xp(int64_t xp, int double_class_xp);
/* The XP a match credits: clamped to 0..DXP_XP_MAX. */
int64_t dxp_match_xp(int64_t xp);

/* v10's claimDoubleXpReward gate: 1 when the last match's offer (offered, offer_games, the match's
 * XP already stored) can be claimed now: it is the account's last match (offer_games == games), an
 * offer was made for it, games is a multiple of DXP_CLAIM_EVERY above zero, and it was not claimed
 * yet (claimed_games != games: lastDoubleAttempt). */
int dxp_offer_claimable(int offered, int64_t games, int64_t offer_games, int64_t claimed_games);

/* The ad provider (the flag). */
typedef enum DxpProvider {
    DXP_PROVIDER_OFF = 0,
    DXP_PROVIDER_PLACEHOLDER = 1
} DxpProvider;
DxpProvider dxp_provider_parse(const char *name); /* "placeholder"; anything else (incl. "") is off */
const char *dxp_provider_name(DxpProvider provider);

typedef enum DxpVerdict {
    DXP_VERDICT_GRANTED = 0, /* the ad is done: the claim may go through now */
    DXP_VERDICT_PENDING = 1, /* the ad is not finished (a real provider's callback has not come) */
    DXP_VERDICT_REFUSED = 2  /* no claim from this provider */
} DxpVerdict;
/* ad_ref: the provider's proof of the watched ad (NULL or "" for the placeholder). */
DxpVerdict dxp_provider_verify(DxpProvider provider, const char *ad_ref);

#endif
