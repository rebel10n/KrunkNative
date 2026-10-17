#pragma once
// Double XP on Krunker Box Office and Krunker Strike (client/src/ui/double_xp_offer.c): Krunker v10.0.2's
// end-screen 2x XP offer, with the brain's rules (shared/include/double_xp.h, services/brain/).
//
//   * the offer: the account's dxp.offer = {games, xp, claimable} (kn_brain's account payload, social
//     "a" / "ua" / "upd" info). v10 offers after each match (doubleXPButton); the client shows the
//     button only while offer.claimable is true (games % 3 == 0, once per match);
//   * the claim: the button sends the social frame ["rqdxp"] at once (no ad) and the brain answers
//     ["rrqdxp", {playerScore, xp, games}] (the match's score doubled; account.score = playerScore, v10's
//     RECIEVE_DOUBLE_XP handler, which pops "XP Doubled!") or ["rrqdxp", null, message];
//   * the badge: "2x XP" follows the mode's doubleXP flag (v10's #doubleXPAd: aon, md), not the account.
// Class XP and raid drops ("2x Class XP", "2x Raid Drops") follow v10's Saturday / Sunday schedules
// (v10_menu.c), as before.
#ifndef DOUBLE_XP_OFFER_H
#define DOUBLE_XP_OFFER_H

typedef enum {
    DOUBLE_XP_GRANTED = 0, // rrqdxp answered with the doubled score
    DOUBLE_XP_NOT_OFFERED, // the offer is not claimable now (no account, games % 3, taken already): nothing sent
    DOUBLE_XP_ERROR,       // rrqdxp refused (its message) or no answer
} DoubleXpResult;
typedef void (*DoubleXpDoneFn)(void *user, DoubleXpResult result);

// The end screen shows the button: the account's offer is claimable now (dxp.offer.claimable)
int  double_xp_offer_open(void);
// The offer was claimed for this match (the button then reads Collected on Strike)
int  double_xp_taken(void);

// The Double XP button's action: ["rqdxp"] on the social socket. done runs once, when rrqdxp is answered,
// or at once when nothing is sent.
void double_xp_claim(DoubleXpDoneFn done, void *user);

#endif
