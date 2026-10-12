#ifndef KRUNKNATIVE_MAP_CATALOG_H
#define KRUNKNATIVE_MAP_CATALOG_H

// The KrunkNative map catalogue: every map the game ships, in its order, with the ids and names
// its UI, rotation, host window, Games window and `map` command use.
//
//   index 0..31   Krunker v7.1.4's official maps (src/data/maps.js), so a catalogue index here
//                 IS the v7.1.4 mapIndex (assets/maps/v7/<fileName>.json, imported unmodified by
//                 tools/maps/import_v7_maps.py; thumbnails ui/img/maps/map_<index>.png)
//   index 32..    the community maps: Krunker_v3.1.1/custom_maps/ (assets/maps/custom/<key>.json,
//                 ASCII filename order), then the v3.1.1 official maps as "v3_<key>"
//                 (assets/maps/<key>.json, their v3.1.1 MAPS order)
//
// The JSON files are reference data, copied unmodified. Several maps share a name ("Burg" is
// burg_new, old_burg and v3_burg), so `key` is the unique id.

#include <stddef.h>
#include <stdint.h>
#include <cJSON.h>

typedef enum {
    MAP_SOURCE_OFFICIAL, // maps/: map_featured 1, creatorname "Krunker"
    MAP_SOURCE_CUSTOM,   // custom_maps/: map_featured 0, creatorname body.creator || "Community"
} MapSource;

typedef struct {
    const char *key;     // unique id: the file stem ("burg_new", "old_burg", "Bhop_VIP"), "v3_<stem>"
    const char *name;    // the JSON "name": the display name (map_name)
    int index;           // v7.1.4 mapIndex (data/maps.js) for official maps, -1 for community maps
    MapSource source;
    unsigned char rotation; // in v7 src/data/map.js rotationMaps (public round rotation)
    unsigned char hostable; // in the Host Game window (the official maps)
    const char *creator;    // publicRoutes creatorname: body.creator || "Krunker" / "Community"
    const char *mod_url;    // body.modURL || ""
    // Host Game card (windows[7] genHost): "by", year and version labels, "" = none
    const char *host_by, *host_year, *host_version;
    const char *path;       // the JSON file, relative to the assets directory ("maps/v7/kanji.json")
} MapCatalogEntry;

#define MAP_CATALOG_OFFICIAL_COUNT 32

size_t map_catalog_count(void);
const MapCatalogEntry *map_catalog_at(size_t index); // NULL out of range
int map_catalog_index_of_key(const char *key);       // exact key (case-sensitive), -1 none

// v3.1.1 packetHandlers.ts cmd_map matching: official maps first (the name's first word
// equals the query, or the name starts with it, case-insensitive), then community maps by
// key (exact, then prefix, case-insensitive). Also takes a catalogue index ("13") or an
// exact key. Returns the catalogue index, -1 = no match.
int map_catalog_find(const char *query);

// Console lookup (client dev console `map`, server_bin `map`/`changelevel`); not a v3.1.1
// port. Surrounding quotes and whitespace are stripped and names compare case-insensitively
// on their letters and digits only, so "Soul Sanctum", "soul_sanctum" and "soulsanctum" are
// equal. Order: an exact key, an exact official name, an exact community name, then a unique
// prefix of the label /maps shows (official name, community key). Numbers are not indices
// here (callers own their list's numbering). Returns the catalogue index, -1 = no match,
// -2 = ambiguous: the matching catalogue indices go to `candidates` (up to `max`) and their
// total to `*candidate_count`. `candidates` / `candidate_count` may be NULL.
int map_catalog_resolve(const char *query, int *candidates, size_t max, size_t *candidate_count);

// publicRoutes.ts map_id: fnv1a(name.toLowerCase()) (old_burg shares Burg's id, as in v3.1.1)
uint32_t map_catalog_map_id(const MapCatalogEntry *entry);
// "img/maps/map_<index>.png" for official maps (public/img/maps), NULL for community maps
// (their map_image is "": the card keeps img/noimg.png)
const char *map_catalog_thumb(const MapCatalogEntry *entry, char *out, size_t size);

// v3.1.1 server/game.ts sanitizeCustomMapJson, in place: an object without "s" but with
// "si" takes s = xyz[si*3 .. si*3+2] (newer editor exports pool sizes in a top-level
// "xyz"); objects still without a 3-number "p" / "s" are dropped. Idempotent; returns how
// many objects it changed or dropped. map_init() runs it too.
int map_sanitize_json(cJSON *map);

// The map's JSON, read and parsed on first use (prefetch-aware on the web) and sanitised
// like the v3.1.1 server's sanitizeCustomMapJson (map_sanitize_json). Cached for the
// program's lifetime: callers never free it. NULL when the file is missing or invalid.
const cJSON *map_catalog_json(size_t index);
// Loads every official map (one prefetch batch on the web); returns how many loaded.
size_t map_catalog_load_official(void);
// Catalogue index of a JSON returned by map_catalog_json, -1 when it is not one of them.
int map_catalog_index_of_json(const cJSON *json);

// The public rotation (src/data/map.js rotationMaps): catalogue indices, returns the count.
size_t map_catalog_rotation(int *out, size_t max);

// calloc'd array of the catalogue maps at `indices` (loaded on demand; missing ones are
// skipped), for game_configure(). The array is the caller's, the JSON is the catalogue's.
const cJSON **map_catalog_list(const int *indices, size_t count, size_t *out_count);

// Index of catalogue map `index` in a game's map list (Game.maps / map_count), appending it
// (realloc) when the list does not hold it yet. -1 on failure. Existing indices never move.
int map_catalog_slot(const cJSON ***maps, size_t *map_count, size_t index);

// The Games window (windows[9]) tabs: 0 Popular, 1 Featured, 2 New, 3 Favorites (empty), 4 My
// Games. Featured is the official maps; Popular / New list them first, then the community maps
// in the order the v3.1.1 offline server answered a guest (doc/ref/krunker-v3.1.1/windows/
// win-games.html). Writes catalogue indices, returns the count.
size_t map_catalog_games_tab(int tab, int *out, size_t max);

#endif
