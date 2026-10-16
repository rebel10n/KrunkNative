#ifndef KRUNKNATIVE_STRIKE_MAPS_H
#define KRUNKNATIVE_STRIKE_MAPS_H

// Krunker Strike's map pool (doc/v10/krunker-strike-maps.md): the seven maps FRVR's Krunker Strike
// rotates, with the map files it plays them from. Pure C, no engine / net / GL dependency.
//
// FRVR's Strike client (build 1.37.2, /home/user/src/KrunkerStrike_v1.37.2/, read as text only)
// keeps Krunker v7's map table (src/data/maps.js order) and picks each round's map from its server's
// gameConfig.remoteFeatures.mapPool.prod ({mapName, mapWeight}, looked up by name in that table),
// fetching "maps/<fileName>.json". The pool is Burg, Sandstorm, Subzero, Lostworld, Oasis, SkyTemple
// and Habitat; their table index is the v7 mapIndex, which is also our catalogue index
// (map_catalog.h). The files are FRVR's own copies, shipped unmodified as assets/maps/strike/<key>.json:
// Habitat and SkyTemple are the first versions of those maps (not v7's habitat_v2 / skytemple_v2),
// the other five are v7 maps as FRVR last exported them.
//
// Under FLAVOR_STRIKE the catalogue loads a pool map from `path` instead of its own file and its
// public rotation is this pool (shared/src/map_catalog.c); FLAVOR_BASE is unchanged. FRVR's
// client shows no map images, so the pool carries no thumbnails.

#include <stddef.h>

typedef struct {
    const char *key;       // "burg": the Strike map id and the file stem of assets/maps/strike/<key>.json
    const char *name;      // FRVR's map name, the mapPool mapName: "Burg", "Lostworld", "SkyTemple", ...
    const char *frvr_file; // FRVR 1.37.2's fileName ("burg_new"), provenance only
    const char *path;      // "maps/strike/burg.json", relative to the assets directory (MapCatalogEntry.path)
    int catalog_index;     // the base catalogue map it stands in for (v7 mapIndex == FRVR's table index)
    int weight;            // mapPool mapWeight; 1 each until FRVR's /v1/game-config values are known
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
