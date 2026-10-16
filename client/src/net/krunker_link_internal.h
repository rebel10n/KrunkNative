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
