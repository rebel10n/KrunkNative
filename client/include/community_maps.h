#pragma once
// Community maps for the Games window (windows[9]) and the Host Game flow: Krunker v10.0.2's
// searchMap(val, cat), selectHostMap(name, modURL, id, creator) and playMap / "/?play=<name>",
// served by kn_brain (/api/maps, /api/search, /api/map-data: services/brain/creations.c).
//
// The Host Game flow picks its map through community_host.h (community_pick / community_host_slot).
// client/src/ui/community_maps.c has the HTTP and the session cache; community_maps_core.c the
// data rules (no client state: client/tests/community_maps_test.c). This header needs no client.h.

#include <stddef.h>
#include <cJSON.h>

// The Official / Community split of the Games window's search (the brain's source= filter)
enum {
    COMMUNITY_SOURCE_ALL,
    COMMUNITY_SOURCE_OFFICIAL,  // the catalogue's official maps (creator "Krunker")
    COMMUNITY_SOURCE_COMMUNITY, // every other map: bundled community maps and published ones
    COMMUNITY_SOURCE_PUBLISHED, // published maps only (no catalogue map): what a dedicated server can host by name
};

// A Games / Host row: what selectHostMap(name, modURL, id, creator) and playMap(name) carry.
typedef struct {
    const char *name;    // map_name (unique among published and catalogue maps)
    const char *creator; // creatorname ("Krunker" = official); NULL / "" when unknown (playMap)
} CommunityMapRef;

// ---- data rules (community_maps_core.c)

// The Search tab's request: /api/search?type=map&val=<q>[&cat=<n>][&source=official|community].
// Writes the path (no host) into out; returns its length, 0 when it does not fit.
size_t community_search_path(char *out, size_t size, const char *query, int category, int source);

// COMMUNITY_SOURCE_* of a select value ("official", "community", anything else: all)
int community_source_of(const char *value);

// The map's game modes as text ("Free for All, Team Deathmatch") from a row's "modes" ids
// (mode_name, shared/include/modes.h); "" when the array is empty or missing. Returns the length.
size_t community_mode_names(char *out, size_t size, const cJSON *modes);

// The catalogue index a row names: rows by "Krunker" are the official maps (and the 311_ maps),
// others the community ones; a bare name (creator NULL or "") prefers the official map, like the
// server's cmd_map (prefixes too); a row with a creator names its map exactly. -1 = not in the
// catalogue (a published map).
int community_catalogue_index(const char *name, const char *creator);

// Index of json in *maps / *count (a game's map list, borrowed pointers), appended (realloc)
// when the list does not hold it yet. Existing indices never move. -1 on allocation failure.
int community_slot_of(const cJSON ***maps, size_t *count, const cJSON *json);

// ---- the session seam (community_maps.c)

// community_fetch(): a published map's checked JSON (owned by the session cache: valid until the
// process exits), downloaded once from the brain. done(user, json, error) runs once: at once from
// the cache or without a brain (json NULL), else when the download is in; error is v10's words
// for a map that does not validate ("Missing spawn points.") or "Game not found.".
typedef void (*CommunityFetchFn)(void *user, const cJSON *json, const char *error);
void community_fetch(const char *name, CommunityFetchFn done, void *user);

// The spec a game server takes for a published map: "published:<creator>/<name>" (or
// "published:<name>" without a creator; server/include/published_maps.h). Returns its length, 0
// when it does not fit.
size_t community_published_spec(char *out, size_t size, const char *name, const char *creator);

// The published maps a dedicated server can host (kn_brain's /api/search?source=published: the
// players' maps, no catalogue map), for the Host Game flow's map step. community_published_maps()
// returns the last list (empty until the first answer; owned here). community_published_refresh()
// asks the brain again (once at a time; nothing without a brain) and runs changed() when the
// answer is in.
typedef struct {
    char name[96];
    char creator[64];
    char spec[192];   // community_published_spec
    char *image;      // map_image (a data: URL) or NULL
} CommunityPublished;
size_t community_published_maps(const CommunityPublished **out);
void community_published_refresh(void (*changed)(void));
