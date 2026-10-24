#pragma once
// The client side of the v10.0.3 S2C packets doc/v10/s2c-coverage.md added (client/src/net/s2c_events.c): the EXT
// subtypes our server and the official-server link (client/src/net/krunker_link_s2c.c) send for them.
//   NET_EXT_PLAYER_ANIM              "c"   a clip of a player's animated class model
//   NET_EXT_SPEECH_BUBBLE            "sb"  the menu mascot's bubble (menu_show_speech_bubble)
//   NET_EXT_SKIN_DEFINITION_CHANGED  "sdc" a store entry replaced at run time (viewer_skin_define)
#include <client.h>
#include <stdint.h>

// 1 when `subtype` is one of the above (handled, whatever its body), 0 otherwise.
int s2c_events_on_ext(Client *client, uint16_t subtype, const uint8_t *body, uint16_t length);
// The definitions waiting for the rest of their JSON slices are dropped (a new connection).
void s2c_events_reset(void);
