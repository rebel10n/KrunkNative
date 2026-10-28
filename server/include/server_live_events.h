#pragma once
// Krunker v10.0.3 live events on the game server (server/src/live_events_server.c, doc/v10/events.md): the brain's
// ongoing events ("welcome" gameEvents, "game-events" pushes, shared/include/live_events.h) give a round its
// eventIndex (updateEventIndex), its limitedTimeMode vote pick (genMatchList) and, at its end, the event's reward
// skin (endGame :221732-221752). gameserver.force_event is v10's game.forceEvent.
#include <live_events.h>
#include <server.h>

// The ongoing events as the brain sent them (NULL / not an array: none)
void server_events_set(const cJSON *events);
const LiveEvents *server_events(void);
// The round's eventIndex (deob-10.0.3.js:219733-219751): with no host and not custom / official / comp, the first
// event running now; then forceEvent (gameserver.force_event, 0 off) wins in any game, as v10's does.
int server_events_round_index(const Game *game, int custom);
// endGame's event reward of one player's account (deob-10.0.3.js:221732-221752): the store skin to give
// (giveSkin "eventreward"), -1 none. map_index: the official map index (-1 none).
int server_events_reward(const ClientConnection *client, int map_index);
// The round's map is built: a custom game's EVENT objects are inert (live_events_map_objects), the egg keys' uids
void server_events_round(Game *game, int custom);
// GameHooks.event_item: the egg hunt (game.foundEventItem); GameHooks.skin_rarity / has_skin: v10.0.4 AI skin rewards
void server_events_install_game_hooks(Game *game);
// store.skins[skin].rarity (assets/skins.json, loaded once), -1 when `skin` is not a store skin
int server_store_skin_rarity(int skin);
// kn_brain's "acct-r" to an "egg_found" request: the lines and the challenge progress. 1 = it was one
int server_events_on_brain_frame(const cJSON *frame);
// A connection is leaving or changing account: its tempEggs and pending egg answers belong to that session only.
void server_events_forget_client(int client_id);
