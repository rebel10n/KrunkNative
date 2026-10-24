#pragma once
// Shared between the official-Krunker translation files (client/src/net/krunker_link*.c); not a
// public interface (client/include/krunker_link.h is).
#include <krunker_link.h>

// One translated packet for the client's inbound queue (krunker_link.c's emit).
void krunker_link_emit(KrunkerLink *l, uint8_t type, const void *payload, uint16_t length);

// krunker_link_world.c: the mode objectives / world objects / AI / zombies / map-trigger packets
// (Domination zones, spawnables, destroyed objects and gates, builds, airdrops, AIs, game messages,
// text popups, loadouts and pickups, custom values). 1 = the packet was one of those (handled).
int krunker_link_world_frame(KrunkerLink *l, KrunkerS2C id, const KrunkerFrame *f);
// init's syncData (arg 11) and round wins (arg 3) of those packets for a joiner / a new round
// (initGame, game.deob.js:219395-219470 and :219535-219540); called by on_init_hud.
void krunker_link_world_init(KrunkerLink *l, const KrunkerFrame *f);
// krunker_link_lobby.c: the private lobby's password prompt ("code", "codeT", "codeP"). 1 = handled.
int krunker_link_lobby_frame(KrunkerLink *l, KrunkerS2C id, const KrunkerFrame *f);

// krunker_link_script.c: KrunkerScript entities ("e+" "e-" "es" "ee" "ea+" "ea-"), runtime blocks
// ("lo" "slo"), moved dyn objects ("objmv") and the script's player locks ("plk") -> the EXTs
// client/src/entities.c consumes. 1 = the packet was one of those (handled).
int krunker_link_script_frame(KrunkerLink *l, KrunkerS2C id, const KrunkerFrame *f);
// init's syncData lo / ent / entAtt / chg (:219478-219496); called by on_init_hud.
void krunker_link_script_init(KrunkerLink *l, const KrunkerFrame *f);

// krunker_link_prophunt.c: Prop Hunt's "up" / "prpR" (1 = handled), the prop a Prop-team player wears
// from its "0" entry, and our NET_EXT_PROP_PICK / _ROTATE out as "prp" / "prpR" (-1: another subtype).
int krunker_link_prophunt_frame(KrunkerLink *l, KrunkerS2C id, const KrunkerFrame *f);
void krunker_link_prophunt_spawned(KrunkerLink *l, int sid, int team, const KrunkerValue *prop_id);
int krunker_link_prophunt_send(KrunkerLink *l, uint16_t subtype, const uint8_t *body, uint16_t length);

// krunker_link_trade.c: the live trade of Trade mode ("trd" "utrd" "utrdc" "atrd" "tch" -> NET_EXT_TRADE_*, 1 = handled),
// the "ch" type 3 request line, and our NET_EXT_TRADE_* out as "strd" "trd" "utrd" "utrdc" "atrd" "dtrd" "ctrd" "tct"
// (-1: another subtype). doc/v10/trade.md.
int krunker_link_trade_frame(KrunkerLink *l, KrunkerS2C id, const KrunkerFrame *f);
void krunker_link_trade_request_line(KrunkerLink *l, const KrunkerValue *sid);
int krunker_link_trade_send(KrunkerLink *l, uint16_t subtype, const uint8_t *body, uint16_t length);

// krunker_link_s2c.c: the S2C packets doc/v10/s2c-coverage.md classifies that no other file translates: the
// player clips ("c"), the menu's speech bubble ("ready" / "sb"), the comp lobby table ("cd" / "cda" / "uid"), store
// skin definitions ("sdc"), the ranked rating card ("lrmr"), game announcements, the found-junk line ("jnk"), the map
// script's log ("ksL"), and the packets ignored on purpose (live service / ads / dev tools). 1 = handled (unknown_in
// does not count it).
int krunker_link_s2c_frame(KrunkerLink *l, KrunkerS2C id, const KrunkerFrame *f);
