#pragma once
// KrunkNative Purple Drank (WEAPON_ID_DRANK, shared/src/weapons/kn_drank.c) and its class, the
// Nettspend (CLASS_ID_NETTSPEND, shared/src/rules/v10/classes_v10.c): not Krunker weapons
// (doc/v7/decisions.md "Purple Drank", "Money Spread", "Nettspend class"). The Drank is a Piss
// (shared/include/piss.h) with a purple stream and puddles: the same traced arc, the same
// SPAWNED_OBJ puddles (OM_KIND_PUDDLE, NetPuddlePayload.weapon_id = the Drank), plus
//
// - Puddle slow (both sides): a player standing in a Drank puddle (the Piss's radius / 2.8 y
//   reach, a victim its owner may damage, never the owner) moves at WeaponStream.puddle_slow x
//   (0.55): drank_puddle_slow multiplies the move accel like attributes.speed (player.c). Puddles
//   are replicated, so the client predicts it; overlapping puddles do not stack (the strongest).
// - Laced (authority): every pour-up (player_reload with the Drank held) rolls
//   WeaponStream.laced_chance (0.04) on the shared pcg32 stream; a hit sends NET_EXT_KN_LACED
//   (net_ext.h NetLacedPacket {uid, duration_ms}) to everyone and starts Player.laced_timer
//   (laced_ms 8000). Both sides count the timer down (drank_tick); the client's post-effect reads
//   laced_timer / laced_duration. Gameplay is untouched by the spell (visual only, v1).
#include <shared.h>

#define DRANK_LACED_ROLL_DENOM 10000u // the roll: pcg32_boundedrand(10000) < laced_chance * 10000

// ---- authority
// The pour-up of `player`'s Drank (player_reload): rolls the laced chance. 1 when laced (the
// timer set, NET_EXT_KN_LACED sent to everyone); 0 otherwise or without a laced weapon / authority.
int drank_pour(Player *player);

// ---- both sides
void drank_tick(Game *game, float delta); // laced timers (s)
// Client: a NET_EXT_KN_LACED body (after the subtype). 1 applied.
int drank_apply_laced(Game *game, const void *body, uint16_t length);
// The move accel x of the Drank puddles `player` stands in (1 = none): the strongest, victims only.
float drank_puddle_slow(const Game *game, const Player *player);
// 1 while `player` is laced.
int drank_is_laced(const Player *player);
