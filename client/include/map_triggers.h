#pragma once
// Client side of the Krunker v3.1.1 map triggers and interfaces (client/src/map_triggers.c):
// the index.js handlers of the messages shared/src/triggers.c sends.
#include <client.h>

// NET_PACKET_OBJECT_STATE / CHECKPOINT / SOUND / JUMP_SCARE / POSITION / POPUP; 1 = handled.
int  client_triggers_handle_event(Client*, int type, const void *payload, uint16_t length);
// NET_EXT_VALUE_TEXT's body: a "cv" / "gv" whose value is a string (net_ext.h NetValueTextPacket)
void client_triggers_value_text(Client*, const void *body, uint16_t length);
void client_triggers_tick(Client*); // per frame: the jump scare timeout, player sounds following their players
// NET_EXT_PLAYER_SOUND_FALLOFF's body: the dist / roll the next NET_PACKET_PLAYER_SOUND add of that player plays with
void client_triggers_player_sound_falloff(const void *body, uint16_t length);
// The custom map GUI (Map.gui, overlay.customGUIs): whether element `index` (its position in the
// map's "gui" array) shows, after "tgui" toggles (nv). For the HUD renderer of the custom GUI.
int client_triggers_gui_visible(const Client*, int index);
