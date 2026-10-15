#pragma once
// The map of an official Krunker game (KN_KRUNKER_OFFICIAL, krunker_net.c). Main thread only.
//
// How the live client gets it (game.deob.js 10.0.3, tools/krunker/maps.py): `init`'s mapIndex is a
// position in Krunker's official list (GET <gapi>/maps at startup); a map is fetched on demand with
// GET <gapi>/maps/<mapIndex> -> {"data": map}; a custom (community) game sends its map inline in
// init arg 9 ({data: "<map JSON>", id, creator, ...}). This client ships the whole official list
// (shared/include/krunker_maps.h, generated; the JSON in assets/, pinned in tools/krunker/maps.lock.json),
// so the resolution order is:
//   1. arg 9 data (a custom game): used as sent, and saved in the user cache as custom_<id>.json
//   2. an index of the table: its assets/ file (asset_prefetch: the web never blocks the frame on it)
//   3. arg 9 with an id and no data: the user cache's custom_<id>.json, else GET <API>/maps/<id> (the route for a
//      community id is unverified, see krunker_map_source.c), cached as custom_<id>.json when it arrives
//   4. an index past the table (a map Krunker added after the list was fetched): the user cache's
//      official_<index>.json, else GET <API>/maps/<index> as the live client does (cached when it arrives)
//   5. none of these: KRUNKER_MAP_UNKNOWN with a reason for the player (the transport leaves the game)
// The user cache: $KRUNKNATIVE_KRUNKER_MAP_CACHE, else $XDG_CACHE_HOME/krunknative/krunker-maps
// (~/.cache/..., %LOCALAPPDATA%\krunknative\krunker-maps on Windows); none on the web (memory only).
// The API origin: $KRUNKNATIVE_KRUNKER_MAPS_API (tests, a stub), else KRUNKER_MAPS_API_URL.
#include <cJSON.h>
#include <stddef.h>

typedef enum {
    KRUNKER_MAP_READY = 0, // *out is the map (owned here, valid for the program's lifetime)
    KRUNKER_MAP_PENDING,   // a download is running: poll again next frame
    KRUNKER_MAP_UNKNOWN,   // nothing can show this map: `why` says what was tried
} KrunkerMapStatus;

int krunker_official_map_count(void);              // KRUNKER_OFFICIAL_MAP_COUNT
const char *krunker_official_map_name(int index);  // the official list's name, NULL beyond the table
const char *krunker_official_map_file(int index);  // assets-relative JSON path, NULL beyond the table
// Blocking load of a table map (tests, tools; natively the file is local): parsed, sanitised
// (map_sanitize_json), cached for the program. NULL when the index is outside the table or the file is bad.
const cJSON *krunker_official_map_json(int index);

// The network for step 4: GET url, then done(ctx, status, body, length) later on the main thread
// (status 0 = no response). Set by the transport (krunker_net.c: backend HTTP, the client's proxy);
// NULL = no network, an index past the table then resolves from the cache only.
typedef void (*KrunkerMapFetchDone)(void *ctx, int status, const char *body, size_t length);
typedef int (*KrunkerMapFetchFn)(const char *url, KrunkerMapFetchDone done, void *ctx); // 1 = started
void krunker_map_source_set_fetch(KrunkerMapFetchFn fn);

typedef struct {
    int index;          // init's mapIndex
    char custom_id[48]; // arg 9 .id ("" none)
    cJSON *custom;      // arg 9 .data, parsed: owned by the request until it resolves
    int stage;          // internal
    unsigned fetch_gen; // internal: the download this request waits for (0 none)
} KrunkerMapRequest;

// Starts resolving (takes `custom`, may be NULL). A request that is not polled to an end is cancelled.
void krunker_map_request_begin(KrunkerMapRequest *r, int index, cJSON *custom, const char *custom_id);
KrunkerMapStatus krunker_map_request_poll(KrunkerMapRequest *r, const cJSON **out, char *why, size_t why_size);
void krunker_map_request_cancel(KrunkerMapRequest *r);

// The user cache directory in use ("" when there is none, e.g. the web).
const char *krunker_map_cache_dir(void);
