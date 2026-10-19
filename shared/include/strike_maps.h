#ifndef KRUNKNATIVE_STRIKE_MAPS_H
#define KRUNKNATIVE_STRIKE_MAPS_H

// Krunker Strike's map pool (doc/v10/krunker-strike-maps.md): the seven maps Krunker Strike rotates.
// Pure C, no engine / net / GL dependency.
//
// The pool is Burg, Sandstorm, Subzero, Lostworld, Oasis, SkyTemple and Habitat, in FRVR's Strike
// table order (the names of FRVR 1.37.2's mapPool; it picks each round's map from
// gameConfig.remoteFeatures.mapPool.prod, {mapName, mapWeight}). Each pool map is Krunker v10.0.2's own
// official map of that name, and its catalogue index is that map's position in the v10 official list
// (krunker_maps.h, mapIndex 0, 2, 3, 7, 9, 15, 22). The files are the v10 files under
// assets/maps/official/, unmodified, so Strike plays the same map data as v10 (the base catalogue loads
// the same files). FRVR's client shows no map images, so the pool carries no thumbnails.
//
// Under FLAVOR_STRIKE the catalogue loads a pool map from `path` and its public rotation is this pool
// (shared/src/map_catalog.c); FLAVOR_BASE is unchanged.

#include <stddef.h>

typedef struct {
    const char *key;   // "burg": the Strike map id
    const char *name;  // the map name, the mapPool mapName: "Burg", "Lostworld", "SkyTemple", ...
    const char *file;  // the v10.0.2 file stem under assets/maps/official/ ("00_burg"); FTUE: "bot_ftue_v2"
    const char *path;  // "maps/official/00_burg.json", relative to the assets directory (MapCatalogEntry.path)
    int catalog_index; // the catalogue map it is (the v10.0.2 mapIndex for these seven); -1 none
    int weight;        // mapPool mapWeight; 1 each until FRVR's /v1/game-config values are known
} StrikeMap;

// The pool, in FRVR's table order (catalogue indices 0, 2, 3, 7, 9, 15, 22). Returns the count;
// *out (optional) points at the static table.
size_t strike_map_pool(const StrikeMap **out);
// The public rotation FRVR builds from the pool: each map's catalogue index listed `weight` times
// (mapPool: config.maps gets mapWeight copies, then a uniform pick); weight <= 0 leaves a map out.
// Returns the full count (at most max written; with out, the return is clamped to max).
size_t strike_map_rotation(int *out, size_t max);
// The same for any pool (strike_map_rotation over the static one); the rule in one place.
size_t strike_map_pool_expand(const StrikeMap *pool, size_t count, int *out, size_t max);
// The pool map that stands in for catalogue map `index`, NULL when that map is not in the pool.
const StrikeMap *strike_map_for_catalog(size_t index);
// A pool map by key ("skytemple") or FRVR name ("SkyTemple"), case-insensitive; NULL none.
const StrikeMap *strike_map_by_key(const char *key);
// The controls tutorial's sandbox (FRVR's ftue fileName "bot_ftue_v2", "Mini Industry Tutorial"): not in the pool
// (catalog_index -1, no rotation). Strike's first run plays it (client/src/ui/strike_ftue_controls.c).
const StrikeMap *strike_ftue_map(void);

#endif
