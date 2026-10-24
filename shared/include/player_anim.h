#pragma once
// v10.0.3 player clips (shared/src/player_anim.c): players.playClip(player, name, reps) (deob-10.0.3.js:193806). A player
// whose class has an animated model (hasAnims: config.aID holds "g", "q" or "f", :194877 / :195127) plays "Jump"
// (players.jump :195272), "Melee" (players.melee :195909) and, our own player only, "Shoot" (players.shoot :196418) on
// it; the authority tells the other clients with NET_EXT_PLAYER_ANIM ("c" PLAYER_ANIM, shared/include/net_ext.h), a
// client plays it on its own player's model (client/src/player_class_model.c).
#include <shared.h>

// hasAnims: the class's aID names a GLTF / GLB / FBX model ("g" / "q" / "f")
int player_class_has_anims(const Player *player);
// playClip (:193806-193820), for an active player with hasAnims. The authority (isServer, :193808-193815) sends clip
// NET_PLAYER_CLIP_* to every spectator and to every other active player within mode.viewDis || 600. A client (its own
// predicted player) plays it on the drawn class model, but not while net_reconcile replays inputs (`if (!recon)`).
void player_play_clip(Player *player, int clip);
// net_reconcile's replay of the pending inputs (procInputs with recon): no clips (players.jump's `if (!recon)` :195271)
void player_anim_set_replay(int replaying);
// players.updateAssetID (:197964-197975), KrunkScript's `player.assetID = v` (:207883-207887): `v += ""`; with
// config.mdl and a non-empty text the authority broadcasts "uai" (NET_EXT_PLAYER_ASSET_ID) and a client swaps its own
// model's asset (player.assetTxt, client/src/player_class_model.c). Nothing for a class without a model.
void player_update_asset_id(Player *player, const char *asset_txt);
// The authority's spawn: player_spawn runs KrunkScript's onPlayerSpawn before the SPAWN is sent, so a swap it makes
// would reach clients before the player does (v10's client drops a "uai" for a sid it has not added, :257784).
// player_anim_spawning(player, 1 / 0) brackets the hook; a swap made inside it is held until player_anim_spawn_sent
// (server/src/net.c, shared/src/bots.c, right after the SPAWN broadcast) sends it.
void player_anim_spawning(Player *player, int spawning);
void player_anim_spawn_sent(Player *player);
