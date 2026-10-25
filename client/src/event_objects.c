// The EVENT prefab's meshes (prefab 40, models/event_0.obj: the egg hunt's eggs) by live event (Krunker v10.0.3,
// client/include/live_events_client.h, doc/v10/events.md).
//
// v10: map.generate skips every EVENT object of a custom game (deob-10.0.3.js:189737); the others are GameObjects
// (collidable, shot at: the egg hunt) whose meshes form the "event_0" group (src "event" + "_0", :189751). The "start"
// handler (our spawn, or spectating) shows that group for event 3, unless spectating, and hides it for any other
// event (:256309-256313: toggleMeshGroup("event_0", eventIndex == 3 ? !spectating : false)). The renderer keeps a
// hidden group hidden for the meshes of the next maps (addMeshGroups: visible = !hidden[groupSrc], :234342,
// :234363-234372), so a fresh page shows the group until the first start. An event with the feed's eventObjects
// shows them too (KrunkNative, live_events.h).
//
// KrunkNative: EVENT objects are map objects in every game (shared/src/prefabs.c), made inert in a custom one
// (live_events_map_objects: inactive, so no collision or shot; their meshes hidden here). A game can turn out custom
// after its INIT (the offline sandbox's GAME_CONFIG follows it): client_event_objects_frame catches that. They cast
// shadows as v10's castShadow prefab does (client/src/shadow.c, a map object like any model).
#include <live_events_client.h>
#include <game_config.h>
#include <stdio.h>

static int g_event_group_hidden; // toggleMeshGroup's hidden["event_0"]: renderer state, kept across maps
static int g_last_custom = -1;

static void event_objects_apply(Client *client, const int log) {
    Map *map = client ? client->game.map : NULL;
    if (!map) return;
    const int custom = game_config_is_custom(&client->game);
    g_last_custom = custom;
    const int count = live_events_map_objects(map, custom);
    if (!count) return;
    int shown = 0;
    for (size_t i = 0; i < map->object_count; i++) {
        const Object *object = map->objects[i];
        if (!object || object->prefab != PREFAB_EVENT || !object->mesh) continue;
        Mesh *mesh = object->mesh;
        mesh->visible = !g_event_group_hidden && object->active && !object->no_vis;
        shown += mesh->visible;
    }
    if (log) fprintf(stderr, "[events] EVENT objects: %d, %d shown (event %d%s)\n", count, shown, client->game.event_index, custom ? ", custom game" : "");
}

void client_event_objects_load(Client *client) {
    event_objects_apply(client, 1);
}

void client_event_objects_frame(Client *client) {
    const int custom = client && client->game.map && game_config_is_custom(&client->game);
    if (!client || !client->game.map || custom == g_last_custom) return;
    event_objects_apply(client, custom); // logged when the game turned custom
}

void client_event_objects_on_start(Client *client, const int spectating) {
    if (!client) return;
    g_event_group_hidden = !live_events_show_objects(client_live_events(), client->game.event_index, spectating);
    event_objects_apply(client, 1);
}
