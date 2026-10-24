// The EVENT prefab's meshes (prefab 40, models/event_0.obj: the egg hunt's eggs) by live event (Krunker v10.0.3,
// client/include/live_events_client.h, doc/v10/events.md).
//
// v10: map.generate skips every EVENT object of a custom game (deob-10.0.3.js:189737); the others become meshes of
// the "event_0" group (src "event" + "_0", :189751). The "start" handler (our spawn, or spectating) shows that group
// for event 3, unless spectating, and hides it for any other event (:256309-256313: toggleMeshGroup("event_0",
// eventIndex == 3 ? !spectating : false)). The renderer keeps a hidden group hidden for the meshes of the next maps
// (addMeshGroups: visible = !hidden[groupSrc], :234342, :234363-234372), so a fresh page shows the group until the
// first start. An event with the feed's eventObjects shows them too (KrunkNative, live_events.h).
//
// KrunkNative keeps EVENT objects out of map->objects (PREFAB_R_NO_OBJECT, shared/src/map.c: no uid, no collision;
// v10's noCollision prefab gets a GameObject there). So this file builds their meshes itself when a map loads
// (map_event_object_from_json: the map builder's own parse and prefab_init, the same model, size and texture
// variant), adds them to the scene, and frees them when the map goes. Not ported: the egg hunt they serve in v10
// (game.foundEventItem, :220700-220728: a shot or a hit on an egg, the database's egg count, the "event.easter.*"
// lines); and they cast no shadow map shadow (shadow.c takes its casters from map->objects).
#include <live_events_client.h>
#include <game_config.h>
#include <stdio.h>
#include <stdlib.h>

static int g_event_group_hidden; // toggleMeshGroup's hidden["event_0"]: renderer state, kept across maps

static struct {
    Object **objects;
    size_t count;
    Scene *scene;
} g_eo;

// A game that turns out custom after its map loaded (the offline sandbox's GAME_CONFIG follows its INIT) hides them
static void event_objects_apply(const Client *client, const int log) {
    const int custom = client && game_config_is_custom(&client->game);
    int shown = 0;
    for (size_t i = 0; i < g_eo.count; i++) {
        const Object *object = g_eo.objects[i];
        Mesh *mesh = object->mesh;
        mesh->visible = !g_event_group_hidden && !custom && !object->no_vis;
        shown += mesh->visible;
    }
    if (log && g_eo.count)
        fprintf(stderr, "[events] EVENT objects: %zu, %d shown (event %d%s)\n", g_eo.count, shown, client ? client->game.event_index : 0,
                custom ? ", custom game" : "");
}

void client_event_objects_unload(Client *client) {
    for (size_t i = 0; i < g_eo.count; i++) {
        Object *object = g_eo.objects[i];
        if (object->mesh && g_eo.scene) scene_remove_mesh(g_eo.scene, object->mesh);
        map_object_free(object); // its mesh too
    }
    free(g_eo.objects);
    g_eo.objects = NULL;
    g_eo.count = 0;
    g_eo.scene = NULL;
    (void) client;
}

void client_event_objects_load(Client *client) {
    client_event_objects_unload(client);
    const Map *map = client ? client->game.map : NULL;
    if (!map || !map->raw_data) return;
    if (game_config_is_custom(&client->game)) { // generate(..., isCustom): no EVENT object at all
        int skipped = 0;
        const cJSON *raw;
        cJSON_ArrayForEach(raw, cJSON_GetObjectItem(map->raw_data, "objects")) skipped += map_raw_object_prefab(raw) == PREFAB_EVENT;
        if (skipped) fprintf(stderr, "[events] EVENT objects: %d skipped (custom game)\n", skipped);
        return;
    }
    const cJSON *raw;
    cJSON_ArrayForEach(raw, cJSON_GetObjectItem(map->raw_data, "objects")) {
        if (map_raw_object_prefab(raw) != PREFAB_EVENT) continue;
        Object *object = map_event_object_from_json(map, raw, 1);
        if (!object) continue;
        Object **grown = object->mesh ? realloc(g_eo.objects, (g_eo.count + 1) * sizeof(*grown)) : NULL;
        if (!grown) {
            map_object_free(object);
            continue;
        }
        g_eo.objects = grown;
        g_eo.objects[g_eo.count++] = object;
        scene_add_mesh(client->scene, object->mesh);
    }
    g_eo.scene = g_eo.count ? client->scene : NULL;
    event_objects_apply(client, 1);
}

void client_event_objects_frame(Client *client) {
    static int last_custom = -1;
    const int custom = client && game_config_is_custom(&client->game);
    if (!g_eo.count || custom == last_custom) return;
    last_custom = custom;
    event_objects_apply(client, custom); // logged when it hides them
}

void client_event_objects_on_start(Client *client, const int spectating) {
    if (!client) return;
    g_event_group_hidden = !live_events_show_objects(client_live_events(), client->game.event_index, spectating);
    event_objects_apply(client, 1);
}
