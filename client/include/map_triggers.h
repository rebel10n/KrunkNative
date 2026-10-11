#pragma once
// Client side of the Krunker v3.1.1 map triggers and interfaces (client/src/map_triggers.c):
// the index.js handlers of the messages shared/src/triggers.c sends.
#include <client.h>

// NET_PACKET_OBJECT_STATE / CHECKPOINT / SOUND / JUMP_SCARE / POSITION / POPUP; 1 = handled.
int  client_triggers_handle_event(Client*, int type, const void *payload, uint16_t length);
void client_triggers_tick(Client*); // per frame: the jump scare timeout
