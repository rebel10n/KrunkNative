#pragma once
// Krunker v10.0.3 Stalker on the client (shared/include/stalker.h, doc/v10/stalker.md):
//   client/src/ui/hud_stalker.c   the HUD: #stalkerControlHolder (leap / stick / taunt keys),
//                                 #stalkerLivingInfoHolder, the leap timer in #zPerksHolder, #hiddenMsg,
//                                 the "You are the stalker" / "You are the Living" popups, the taunt keys
//   client/src/stalker_render.c   the model: opacity 0.1 on the body parts, a model rebuilt when
//                                 isStalker flips (a still stalker is hidden by the server's player sync)
#include <client.h>

// ---- hud_stalker.c

// Per frame from hud_ui.c (after its visibility pass): `visible` = in game with the HUD shown,
// `layout_epoch` changes whenever hud_ui.c reset the holders.
void hud_stalker_tick(Client*, int visible, int layout_epoch);
// NET_EXT_STALKER_PICKED: the Living's popup (iïiîiïí, deob-10.0.3.js:255146-255148).
void hud_stalker_on_picked(Client*);
// binds.stalkerTaunt[index] pressed in game (ui_controls.c): "stlkrt" index while we are the stalker
// (deob-10.0.3.js:260106-260110). Returns 1 when a taunt went out.
int hud_stalker_taunt(Client*, int index);

// ---- stalker_render.c

// The opacity half of modelOverrides.Stalker on a freshly built model (player_mesh.c).
void stalker_render_body_opacity(PlayerMesh*, size_t loadout_size);
// Before the world pass (main.c): a model whose Stalker look no longer matches isStalker is rebuilt.
void stalker_render_frame(Client*);
