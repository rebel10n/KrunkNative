#pragma once
// The Host Game seam for community maps (client/src/ui/community_maps.c): the Host Game flow
// (host_flow_map_entries() in client/src/ui/host_flow_ui.c once it lands), the Games window's
// selectHostMap and the Host window pick their map here. A catalogue map resolves at once; a
// published map is downloaded once from kn_brain (GET /api/map-data?name=), checked like v10's
// prepareGame + validateMap (map_prepare_custom), kept for the session and given a slot in
// client->game.maps. community_host_slot() then starts that slot the way the Host window does.

#include <client.h>
#include <community_maps.h>

// community_pick(): the client's slot of the map in client->game.maps, -1 when it cannot be had
// (unknown name, no brain, the download is not a valid map). done(user, slot) runs once.
typedef void (*CommunityPickFn)(void *user, int slot);
void community_pick(Client *client, const CommunityMapRef *ref, CommunityPickFn done, void *user);

// Starts the map of `slot` as the Host window does: the slot becomes the menu's map, a running
// offline game reloads on it with `mode` (a Krunker mode id, -1 keeps client->net_mode_index),
// otherwise the offline game starts on it (offline_start_map); then the host menu (menu_host_offline).
void community_host_slot(Client *client, int slot, int mode);
