#pragma once
// Krunker v10.0.3 live events on the client (doc/v10/events.md): config.events (shared/include/live_events.h) and what
// the client does with the round's eventIndex (Game.event_index, NET_EXT_INIT_EVENT / official init [2]).
//   client/src/ui/v10_events.c   the event list (kn_brain's GET /data/events; KRUNKNATIVE_GAME_EVENTS=<feed file> for
//                                a client without a brain), window.eventPopup (deob-10.0.3.js:260286-260320)
//   client/src/event_objects.c   the EVENT prefab's meshes, toggleMeshGroup("event_0") at "start" (:256309-256313)
// The Challenges window's Event Challenges (windows[32], :250765-250879) are v10_rewards.c's.
#include <client.h>
#include <html_dom.h>
#include <live_events.h>

// config.events: the brain's answer (refetched every few minutes), the file, or a render scenario's list
const LiveEvents *client_live_events(void);
double client_live_events_now_ms(void); // Date.now() of the event times (the scenario's fixed clock when it gives one)
void v10_events_tick(void);             // per frame: the first fetch, the refresh
void v10_events_want(int event_index);  // a round's eventIndex (NET_EXT_INIT_EVENT): fetch again soon when it is not listed
int  v10_events_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, int argc); // eventPopup; -1 = not ours
void v10_events_scenario(const cJSON *events, double now_ms); // --render-scenario state.gameEvents (now_ms 0: the clock)

// "start" (our spawn: spectating 0; spectate_begin: 1): the EVENT prefab's meshes show for this round's event
void client_event_objects_on_start(Client *client, int spectating);
// client_load_map: the map's EVENT meshes (none in a custom game), in the shown / hidden state of the last start
// (renderer.addMeshGroups' hidden groups, :234342); client_unload_map frees them
void client_event_objects_load(Client *client);
void client_event_objects_unload(Client *client);
void client_event_objects_frame(Client *client); // per frame: a game that turned custom after its INIT hides them
