// Krunker v10.0.2's end-of-match Svelte components on the end screen's results card
// (client/src/ui/hud_endscreen_v10.c; v10 = ~/.cache/kn-official/game.deob.js, `g:N` = its line):
//   - the rank change card (svelte-19e82s0, g:195637): $latestRank / $latestMMRChange;
//   - Other Earnings (.otherEarnings, svelte-m5jxz9 / g:195699): "+N RP" from $rankedPointsChange;
//   - the raid promo (#raidPromoContainer, g:195990);
//   - the Collect Rewards button (svelte-3svau8, g:195975): a 3 s pulse, a 10 s auto-collect;
//   - the rewards overlay (svelte-1sac6ik, g:195767): the cards flip, then the items fly to the
//     backpack (the chained setTimeouts of g:195771-195973 as one absolute-time schedule), and the
//     flying item / icon (svelte-5yw9uo, svelte-ory46c).
// hud_ui.c only calls the hooks below (the end card is built there).
#ifndef HUD_ENDSCREEN_V10_H
#define HUD_ENDSCREEN_V10_H

#include <cJSON.h>
#include <client.h>

// A new match (net INIT / the end screen leaves): the stores go back to 0 (g:218985-218992), the
// pending items, the overlay and the collect state are dropped.
void hud_endscreen_v10_reset(void);

// showEndScreen (the END): the raid promo's roll (otherData.showRaidPromo, g:191404: not Raid, not
// custom, a public game, 50%), and the held items / overlay / collect state of the previous end are
// cleared. The Collect countdown does not start here.
void hud_endscreen_v10_begin(const Client *client, double now_ms);

// endScreen.init (the results, 5 s after the END: config.endAnim, v10:219683-219684): autoCollectTime
// = now + 10 s (g:203310). The countdown starts here, not at the END.
void hud_endscreen_v10_results(double now_ms);

// lrmr (LATEST_RANKED_MMR_CHANGE: mmrChange, latestRank) and UPDATE_BALANCES' ranked points
// change (g:208371): the rank card / "+N RP" appear (or update) at once, whatever tab is up.
void hud_endscreen_v10_ranked(int mmr_change, int rank, int rp_change);

// otherData.items: a skin the match gave. KrunkNative has no event kill pools, so the skins a
// match gave (challenge rewards, finds) are offered the same way, with the "gift" icon (the
// default of g:195992). Returns 1 when held back for the Collect button, 0 when it should go
// straight into the Rewards tab (already collected / no results yet is fine: it is held).
int hud_endscreen_v10_offer_skin(int skin);

// GUI.endScreen.show() ran (hud_end_card_render): mount the components onto the card.
void hud_endscreen_v10_shown(void);

// v10's end-card button (v10.js 203200): #doubleXPButton (Double XP) after every match but a comp one, and
// #returnButton (Return to Krunker) on a comp one. The card builder (hud_ui.c) puts it in #endButtonsHolder, a
// sibling of .endCard in #endTabbedView, before the Collect container (hud_endscreen_v10_shown fills that).
const char *hud_endscreen_v10_end_button_html(const Client *client);

// Every frame (client_hud_html_tick); now_ms is the HUD clock. Drives the countdown, the
// auto-collect and the overlay's schedule.
void hud_endscreen_v10_tick(double now_ms);

// --render-scenario state.endscreen: {rank: {rank, mmr}, rp, items: [{skin, r?}], raid: 0|1,
// doubleRaid, collect: "overlay"|"auto"}. Called before the end screen is shown.
void hud_endscreen_v10_scenario(const cJSON *end);

// --render-scenario state.raidPromo: 0 / 1 fixes the raid promo's roll of the next END (the v10
// sandbox's seeded Math.random), -1 rolls again.
void hud_endscreen_v10_scenario_raid(int raid);

// 1 while the rewards overlay is up (tests).
int hud_endscreen_v10_overlay_active(void);

#endif
