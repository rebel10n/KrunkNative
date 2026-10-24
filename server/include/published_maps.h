#pragma once
// Published (kn_brain) maps as a game server's maps (server/src/published_maps.c): the command
// line's map argument, server.cfg's gameserver.maps entries and --set gameserver.maps= name one as
//
//     published:<creator>/<name>     (the account that published it, then the map's name)
//     published:<name>               (any creator: published names are unique)
//
// The map is fetched from the brain (GET <brain http>/api/map-data?name=&creator=), checked the way
// v10 checks a custom map before playing it (prepareGame's "(" ")" replacement, then validateMap
// with a creator's limits: map_prepare_custom_json + map_validate_error(map, 1)), sanitized, kept
// in memory for the process and written to a disk cache. When the brain cannot answer, the disk
// cache's copy is played; with neither, the map is skipped with a log line and the server falls
// back to the rest of its list (the command line: gameserver.maps, else Krunker's rotation).
// A brain "host" request with a published map (accounts.c) goes through the same check and cache.
#include <cJSON.h>
#include <stddef.h>

#include <server_config.h>

#define PUBLISHED_MAP_PREFIX "published:"

// "published:<creator>/<name>" / "published:<name>" -> 1 with name (and creator, "" when none);
// 0 for anything else (a catalogue key, a file path). A creator never contains '/', so the first
// '/' after the prefix ends it and the name may hold any character.
int published_map_parse(const char *spec, char *name, size_t name_size, char *creator, size_t creator_size);

// The spec of a published map ("published:alice/Arena", or "published:Arena" without a creator).
void published_map_format(char *out, size_t size, const char *name, const char *creator);

// v10's checks on a fetched map, in place: prepareGame's paren replacement, validateMap (a
// creator's limits), then map_sanitize_json. NULL when it may be played, else v10's message.
const char *published_map_verify(cJSON *map);

// The process's copy of a published map (owned here, valid until exit): memory, else the brain
// (blocking, timeout_ms; call before the tick loop runs or from a worker), else the disk cache.
// NULL with err filled when none can be had or the map does not verify.
const cJSON *published_map_load(const GsConfig *cfg, const char *name, const char *creator, int timeout_ms, char *err, size_t err_size);

// The process's copy when published_map_load already has it (never fetches: safe on the tick).
const cJSON *published_map_cached(const char *name, const char *creator);
// The brain's id of a published map (v10.0.3 customMapData.id, KrunkScript player.giveSkin's map list): the latest
// noted one, else a loaded map's; 0 when unknown (not fetched, or the disk cache's copy was played).
int published_map_id(const char *name);
// Notes the id a fetch outside published_map_load gave (a hosted game's map-data frame "id", accounts.c); 0 forgets it.
void published_map_note_id(const char *name, int map_id);

// The disk cache ($XDG_CACHE_HOME/krunknative/published-maps, else ~/.cache/krunknative/published-maps):
// store writes a verified map, read returns a copy (caller frees) or NULL.
void published_map_cache_store(const char *name, const char *creator, const cJSON *map);
cJSON *published_map_cache_read(const char *name, const char *creator);
