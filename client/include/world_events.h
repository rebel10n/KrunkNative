#pragma once
// Client side of the map-logic WP-B world packets (client/src/world_events.c): the v7.1.4
// index.js handlers of "zn" (Game.updateZone), "bo" (Game.buildObj), "gt" (Game.logTime) and the
// speed-run "t", and "ana" with isObj (io_servAnim). Object node moves ("mo" / "mf") and the
// "chp" reset arrive through client/src/map_triggers.c.
#include <client.h>

// NET_PACKET_ZONE / BUILD / LOG_TIME / OBJECT_ANIM; 1 = handled.
int client_world_handle_event(Client*, int type, const void *payload, uint16_t length);

// The last speed-run time the server sent (NET_PACKET_LOG_TIME), for the HUD: 0 = none this round.
int client_world_log_time(int *time_ms, int *time_log, int *kind, int *show_ms);
void client_world_reset(void); // INIT: a new round

// "ana" for a map object: the renderer that plays model clips registers here (NULL clears).
typedef void (*ClientWorldAnimFn)(Client*, Object*, const char *clip, int raw);
void client_world_set_anim_handler(ClientWorldAnimFn);
