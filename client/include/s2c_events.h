#pragma once
// The client side of the v10.0.3 S2C packets doc/v10/s2c-coverage.md added (client/src/net/s2c_events.c): the EXT
// subtypes our server and the official-server link (client/src/net/krunker_link_s2c.c) send for them.
//   NET_EXT_PLAYER_ANIM              "c"   a clip of a player's animated class model (player_class_model.h)
//   NET_EXT_PLAYER_ASSET_ID          "uai" a player's class model takes another asset, rebuilt at once
//   NET_EXT_SPEECH_BUBBLE            "sb"  the menu mascot's bubble (menu_show_speech_bubble)
//   NET_EXT_SKIN_DEFINITION_CHANGED  "sdc" a store entry replaced at run time (viewer_skin_define)
//   NET_EXT_PLAYER_INTER_PROG        "p" / "mp" the spectated player's interact / charge bar (s2c_events_spectate_bar)
//   NET_EXT_UNBOX_MESSAGE            "unb" / "unbS" "<name> unboxed [item]" (hud_ui_unboxed)
//   NET_EXT_LOTTERY_PRIZE_WON        "lotWin" "<name> won <prize> in the raffle!" (hud_ui_raffle_won)
//   NET_EXT_SPIN_WHEEL_RESPONSE      "spinW" a map prize wheel spins (prize_wheels.c), ours pays the account
//   NET_EXT_CHARGE_CONSENT_REQUEST   "chreq" the KR charge prompt (s2c_ui.c)
//   NET_EXT_WARS_CLAN                "warsClan" the Turf Wars clan holding the map (s2c_ui.c)
//   NET_EXT_KN_OFFICIAL_EDIT_MAP     "edMp" the editor opens the map (s2c_ui.c)
#include <client.h>
#include <stdint.h>

// 1 when `subtype` is one of the above (handled, whatever its body), 0 otherwise.
int s2c_events_on_ext(Client *client, uint16_t subtype, const uint8_t *body, uint16_t length);
// Drops a definition / map still waiting for the rest of its slices (the next slice 0 drops it too).
void s2c_events_reset(void);
// INIT (client_game_events_reset): the spectator bars, the prize wheels, the charge prompt and the Turf Wars clan
// start over.
void s2c_events_on_init(Client *client);
// Per frame (client_game_events_tick): the prize wheels' steps, the Turf Wars popup on entering.
void s2c_events_tick(Client *client);
// The NET_INTER_BAR_* bar of the player we spectate, 0 when we do not spectate (a new spectate starts at 0).
float s2c_events_spectate_bar(int bar);
