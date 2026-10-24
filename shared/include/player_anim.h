#pragma once
// v10.0.3 player clips (shared/src/player_anim.c): players.playClip(player, name, reps) (deob-10.0.3.js:193806). A player
// whose class has an animated model (hasAnims: config.aID holds "g", "q" or "f", :194877 / :195127) plays "Jump"
// (players.jump :195272) and "Melee" (players.melee :195909) on it; the authority tells the other clients with
// NET_EXT_PLAYER_ANIM ("c" PLAYER_ANIM, shared/include/net_ext.h).
#include <shared.h>

// hasAnims: the class's aID names a GLTF / GLB / FBX model ("g" / "q" / "f")
int player_class_has_anims(const Player *player);
// playClip's isServer branch (:193808-193815): on the authority, an active player with hasAnims sends clip
// NET_PLAYER_CLIP_* to every spectator and to every other active player within mode.viewDis || 600. No-op elsewhere.
void player_play_clip(Player *player, int clip);
