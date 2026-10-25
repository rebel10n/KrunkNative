#pragma once
// Krunker v10.0.3's GameObject uids of a map (doc/v10/events.md section 2). v10's MapManager gives a uid
// (`uid: this.objects.length`) to every GameObject its builders push while map.generate walks the map's objects
// (deob-10.0.3.js:188695-189760): a collidable cube, model, plane, ramp... one each, a FLAG two (the flag and its
// trigger zone, :184685-184723), the zones / machines / pickups / ladders one each always, the lights / sprites /
// liquids / signs / spawners none, and EVENT objects none in a custom game (:189737). The official server's
// packets name objects by these uids. KrunkNative's map objects (map->objects) are every placed object in file
// order, decorations without collision too, so the official link (client/src/net/krunker_link_world.c) translates
// between the two with this table. KrunkNative's own server and client use map->objects indexes on the wire.
//
// Not modelled: a terrain map's terrain objects (addTerrObj, pushed before the map's objects): the table is
// not built for a map with terrain (map_game_uids_build returns 0) and the uids go through unchanged.
#include <cJSON.h>
#include <stddef.h>
#include <stdlib.h>

typedef struct {
    int *uid_of_index;   // map->objects index -> GameObject uid (-1: v10 makes none for it)
    size_t index_count;
    int *index_of_uid;   // GameObject uid -> map->objects index (-1: no KrunkNative object, a flag's zone)
    int uid_count;
} MapGameUids;

// The v10 GameObjects a raw map object (prefab, l / col, dyn) gives: 0, 1 or 2 (FLAG); custom: a custom game
int map_game_object_count(const cJSON *raw_obj, int custom);
// The table of a map JSON (a copy is sanitized as map_init does: pooled sizes, objects without p / s). 1 built,
// 0 not (no objects, terrain, no memory): the out table is empty then.
int map_game_uids_build(MapGameUids *out, const cJSON *map_json, int custom);

// The lookups and the free are inline: the official link's translation (client/src/net/krunker_link_world.c) uses them
// without the map code. An empty table is the identity (no table: the uids pass unchanged).
static inline void map_game_uids_free(MapGameUids *t) {
    if (!t) return;
    free(t->uid_of_index);
    free(t->index_of_uid);
    t->uid_of_index = t->index_of_uid = NULL;
    t->index_count = 0;
    t->uid_count = 0;
}
static inline int map_game_uid_of_index(const MapGameUids *t, const int index) {
    if (!t || !t->uid_of_index) return index;
    return index >= 0 && (size_t) index < t->index_count ? t->uid_of_index[index] : -1;
}
static inline int map_game_index_of_uid(const MapGameUids *t, const int uid) {
    if (!t || !t->index_of_uid) return uid;
    return uid >= 0 && uid < t->uid_count ? t->index_of_uid[uid] : -1;
}
