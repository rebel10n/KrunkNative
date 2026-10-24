#pragma once
// Krunker v10.0.3 Stalker, the parts outside the mode rules (doc/v10/stalker.md). The rules (pickStalker,
// onKill, onMelee, camping) are shared/src/modes/mode_rules.c's, the stalker's moves (leap, wall stick,
// coyote time) shared/src/player.c's (movement_stalker). Here: isStalker, the 300 ms hide, the taunts and
// STALKER_PICKED. The client half is client/src/ui/hud_stalker.c (HUD, popups, taunt keys) and
// client/src/stalker_render.c (the model's look); a hidden stalker is left out of the server's player sync.
//
// References are deob-10.0.3.js lines (/home/user/jails/kru/src/Krunker_v10.0.3/deob/game.deob.js).
#include <shared.h>

#define STALKER_HIDE_TIME 0.3f       // isStalker && noMovT >= 300 (deob-10.0.3.js:191772)
#define STALKER_LEAP_COOLDOWN 3.0f   // now - lastStalkerLeap >= 3000 (deob-10.0.3.js:191483; the HUD timer :261070)
#define STALKER_TAUNT_COUNT 6        // binds.stalkerTaunt 0..5 (deob-10.0.3.js:235090), sounds staunt_0..5
#define STALKER_TAUNT_COOLDOWN 10.0f // players.stalkerTaunt: lastTaunt = now + 10000 (deob-10.0.3.js:198334)
// addPlayerSound(sid, "staunt_" + i, {vol: 1, rate: 1, dist: 20, roll: 1.1}) (deob-10.0.3.js:198335-198340):
// dist / roll ride in NET_EXT_PLAYER_SOUND_FALLOFF ahead of the NET_PACKET_PLAYER_SOUND
#define STALKER_TAUNT_VOLUME 1.0f
#define STALKER_TAUNT_RATE 1.0f
#define STALKER_TAUNT_DISTANCE 20.0f
#define STALKER_TAUNT_ROLLOFF 1.1f
#define STALKER_OPACITY 0.1f         // modelOverrides.Stalker.opacity (deob-10.0.3.js:118203)
#define STALKER_KNIFE_TEXTURE "melee/melee_0_5" // the stalker's knife texSrc (deob-10.0.3.js:194611)

// NET_EXT_STALKER_TAUNT (C -> S, v10 "stlkrt" i, deob-10.0.3.js:260106-260110): the stalker's taunt key i.
typedef struct {
    uint8_t index; // 0..STALKER_TAUNT_COUNT-1
} NetStalkerTauntPacket;

// player.isStalker (deob-10.0.3.js:190464-190466: mode.teamOptions[team - 1] == "Stalker") in the v10
// Stalker mode (GameModeConfig.stalker_v10; Krunker Strike's v7 Stalker has none of this file's rules).
int player_is_stalker(const Game*, const Player*);

// procInputs, outside replays (deob-10.0.3.js:191766-191779): noMovT counts the time the player had no
// velocity, isHidden = isStalker && noMovT >= 300 || forceHide. The server leaves a hidden player out of
// every player's state stream (syncState, deob-10.0.3.js:222346 / :222357); the local player's
// #hiddenMsg follows it (hud_stalker.c).
void stalker_update_hidden(Player*, float delta);

// players.stalkerTaunt (deob-10.0.3.js:198332-198342) for "stlkrt" i on the authority: the taunt sound
// at the stalker for everyone (NET_EXT_PLAYER_SOUND_FALLOFF then NET_PACKET_PLAYER_SOUND; clients keep it
// on the stalker while it plays), 10 s apart (lastTaunt, shared with the prop taunts). Returns 1 when sent.
int game_player_stalker_taunt(Game*, Player*, int index);

// pickStalker's broadcastEx(stalker.id, STALKER_PICKED) (deob-10.0.3.js:217949): everyone but the
// stalker gets NET_EXT_STALKER_PICKED ("You are the Living" popup, deob-10.0.3.js:255146-255148).
void stalker_send_picked(Game*, const Player *stalker);
