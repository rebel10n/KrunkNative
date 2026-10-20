#pragma once
// Double XP on Krunker Box Office and Krunker Strike (client/src/ui/double_xp_offer.c): Krunker v10.0.2's
// end-screen 2x XP offer, with the brain's rules (shared/include/double_xp.h, services/brain/).
//
//   * the offer: the account's dxp.offer = {games, xp, claimable} (kn_brain's account payload, social
//     "a" / "ua" / "upd" info). The Box Office end card follows v10's own rules instead: #doubleXPButton
//     after every match but a comp one, signed in or not (hud_endscreen_v10.c). Krunker Strike's banner
//     follows the offer: it shows and claims while offer.claimable is true (games % 3 == 0, once per match);
//   * the claim: the button sends the social frame ["rqdxp"] at once (no ad) and the brain answers
//     ["rrqdxp", {playerScore, xp, games}] (the match's score doubled; account.score = playerScore, v10's
//     RECIEVE_DOUBLE_XP handler, which pops "XP Doubled!") or ["rrqdxp", null, message]. The Box Office
//     button sends it when the account's games is a multiple of 3 above zero (v10's claimDoubleXpReward,
//     v10.js 222670); Strike's while the offer is claimable;
//   * the badge: "2x XP" follows the mode's doubleXP flag (v10's #doubleXPAd: aon, md), not the account.
// Class XP and raid drops ("2x Class XP", "2x Raid Drops") follow v10's Saturday / Sunday schedules
// (v10_menu.c), as before.
#ifndef DOUBLE_XP_OFFER_H
#define DOUBLE_XP_OFFER_H

typedef enum {
    DOUBLE_XP_GRANTED = 0, // rrqdxp answered with the doubled score
    DOUBLE_XP_NOT_OFFERED, // nothing sent: no account, the offer not claimable (Strike) or games % 3 (Box Office), a claim in flight
    DOUBLE_XP_ERROR,       // rrqdxp refused (its message) or no answer
} DoubleXpResult;
typedef void (*DoubleXpDoneFn)(void *user, DoubleXpResult result);

// Krunker Strike's Double XP banner: the account's offer is claimable now (dxp.offer.claimable)
int  double_xp_offer_open(void);
// The offer was claimed for this match (the button then reads Collected on Strike)
int  double_xp_taken(void);

// Krunker Strike's Double XP claim: ["rqdxp"] on the social socket while the offer is claimable. done runs once,
// when rrqdxp is answered, or at once when nothing is sent.
void double_xp_claim(DoubleXpDoneFn done, void *user);
// Box Office's end-card claim (hud_endscreen_v10.c): ["rqdxp"] on the social socket when the account's games is a
// multiple of 3 above zero, whatever the offer says (v10's claimDoubleXpReward). done as double_xp_claim.
void double_xp_end_claim(DoubleXpDoneFn done, void *user);

#endif
