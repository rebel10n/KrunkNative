#pragma once
// Krunker v10.0.3 game-server events that need kn_brain (server/src/brain_events.c; the brain's side is
// services/brain/node_events.c, doc/v10/s2c-coverage.md). Line numbers: deob-10.0.3.js.
//   warsClan  territorialWars.setClanInfo (:215092-215098): at an eligible round start (isEligibleGame + isEligibleMap,
//             the Turf Wars kill rule) the brain names the clan holding the map ("turf_clan"); WARS_CLAN goes to
//             everyone, and to each joiner after its INIT (init syncData.warsClan, :220412-220420)
//   spinW     a map PRIZE_WHEEL (MapManager.spinners): interact 9 offers the spin
//             (CONFIRM_INTERACTION [uid, 2], :197177-197181), "cnfm" 2 spins it through the brain's KR wallet
//             ("map_spin"), SPIN_WHEEL_RESPONSE to everyone
//   unbS      the server's updateSpinners (:219434-219479) stops the pointer: everyone for rarity >= 2, else the spinner
//   unb       a store / Free KR spin of a player of this game (the brain's "unbox" push)
//   lotWin    a lottery prize of a player of this game (the brain's "raffle-win" push)
//   chreq     isMonetized() of a hosted published map ("studio_game") and the KR of a consented charge
//             ("studio_charge"); the consent itself is GAME.PAYMENTS' (server/src/ks_server_host.c)
// Without a brain link none of these happen (the offline sandbox, standalone servers).
#include <server.h>
#include <cJSON.h>
#include <stdint.h>

#define BRAIN_EVENTS_REPLY_S 15.0f // a brain request without its answer fails (spin: nothing happens; charge: false)

// accounts_round_changed: a new round. turf_map: the Turf Wars map index when the round is eligible (accounts.c
// acc_turf_round_start), -1 otherwise.
void brain_events_round_start(int turf_map);
// net_send_init: what a joiner's INIT resets and init's syncData carries (the clan holding the map).
void brain_events_send_init(ClientConnection *client);
// accounts.c acc_handle_frame: 1 when the frame was ours (an "acct-r" of our requests, "unbox", "raffle-win").
int brain_events_on_brain_frame(const cJSON *frame);
void brain_events_link_down(void);                      // pending requests fail
void brain_events_tick(float now);                      // the spinners' pointers, request timeouts

// net.c: "int" type 9 (uid = the spinner's uid) and "cnfm" kind 2 of a connection's player.
void brain_events_spinner_interact(ClientConnection *client, int uid);
void brain_events_spinner_confirm(ClientConnection *client);

// GAME.PAYMENTS (ks_server_host.c). game.isMonetized(): the hosted published map's studio is monetized.
int brain_events_monetized(void);
// canCustomCurrency (:219754): a connection with an account (not flagged), on a hosted published map, brain linked.
int brain_events_can_charge(const ClientConnection *client);
// customCurrency.transact's KR part: takes `amount` KR of the connection's account for the map's studio. done(user,
// success, balance) runs once from brain_events_tick / the reply (never from inside this call). 0 = sent, -1 = not
// (done is not called).
typedef void (*BrainChargeDone)(void *user, int success, double balance);
int brain_events_studio_charge(const ClientConnection *client, int amount, BrainChargeDone done, void *user);
