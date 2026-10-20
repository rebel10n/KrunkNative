#pragma once
// Social hub protocol and service (Krunker v3.1.1 public/social.html).
//
// Krunker's hub talks to the matchmaker over its own WebSocket (/api/ws, msgpack frames
// ["label", ...args], matchmaker/socialWs.ts) plus a few HTTP routes (/api/maps, /api/mods,
// /api/search, ..., matchmaker/publicRoutes.ts). KrunkNative carries the same frames as
// JSON text over the game connection: the client's hub (client/src/ui/social_ui.c) sends
// one request frame, the server (server/src/net.c, or the in-process local server)
// answers with a JSON array of reply frames built by social_service_handle().
//
// Wire: NET_PACKET_SOCIAL_REQUEST / NET_PACKET_SOCIAL_REPLY packets carry NetSocialChunk
// pieces (NET_MAX_PAYLOAD-sized) of one message; the reply reuses the request's id.
//
// Request frames (Krunker's labels; the "api" frame stands in for the HTTP routes):
//   ["r", page, query, ...]                 page data      -> ["0", page, query, data...]
//   ["a", action, [name, pass, token]]      login          -> ["a", error, id, name, data, token]
//   ["api", "/maps"|"/mods"|"/assets"|"/search", {params}]  -> ["api", path, {data, index, pos, error}]
//   ["pst", name, days]                     profile stats  -> ["psd", []]
//   ["guf", playerId]                       activity tab   -> ["uf", []]
// Anything else is dropped silently, like socialWs.ts does for unknown labels.
//
// KrunkNative servers keep no accounts, posts, market or clans: every page answers like
// the v3.1.1 offline server does for data it does not have (null / empty lists), logins
// fail, and the map catalogue is the server's own map list.
#include <shared.h>

#define NET_SOCIAL_CHUNK_HEADER 8
#define NET_SOCIAL_CHUNK_DATA (NET_MAX_PAYLOAD - NET_SOCIAL_CHUNK_HEADER)
#define NET_SOCIAL_MAX_MESSAGE (512u * 1024u)

#pragma pack(push, 1)
typedef struct {
    uint32_t id;       // request id (the reply echoes it)
    uint16_t index;    // chunk index, 0..count-1, sent in order
    uint16_t count;    // chunks in the message (>= 1)
    uint8_t data[NET_SOCIAL_CHUNK_DATA];
} NetSocialChunk;      // sent with length NET_SOCIAL_CHUNK_HEADER + data bytes
#pragma pack(pop)

// Sends one packet; returns 0 on failure (stops the message).
typedef int (*SocialSendFn)(void *user, NetPacketType type, const void *payload, uint16_t length);

// Splits `msg` into chunks of `type`. Returns 0 when a send failed or msg is too large.
int social_send_message(NetPacketType type, uint32_t id, const char *msg, size_t len, SocialSendFn send, void *user);

// Reassembles chunks (in order, one message at a time per assembly).
typedef struct {
    uint32_t id;
    uint16_t count;
    uint16_t next;
    char *buf;
    size_t len;
    size_t cap;
} SocialAssembly;

// Feeds one packet's payload. Returns 1 with *out (malloc'd, NUL-terminated, caller
// frees) and *id when the message is complete, 0 while incomplete, -1 on a bad chunk
// (the partial message is dropped).
int social_assembly_push(SocialAssembly *a, const void *payload, size_t length, char **out, uint32_t *id);
void social_assembly_free(SocialAssembly *a);

// --- service (server side) -------------------------------------------------------

typedef struct {
    const char *name;
    const char *creator;      // map JSON "creator", else "Krunker"
    const char *description;  // map JSON "description", else ""
    int featured;             // Krunker's own maps (assets/maps), 0 for custom maps
    // the map's category (map_catalog.h index, -1 none) and its name ("" none): the brain's map_category and
    // category. The catalogue keeps no categories (services/brain/creations.c catalog_row), so the server's maps have none
    int category;
    const char *category_name;
} SocialMapInfo;

typedef struct {
    const SocialMapInfo *maps;
    size_t map_count;
} SocialHost;

// Fills `out` with the maps a game rotates through (Game.maps). Returns the count.
size_t social_maps_from_game(const struct Game_t *game, SocialMapInfo *out, size_t max);

// Answers one request frame. Returns the reply (a JSON array of frames, malloc'd; free
// with free()) or NULL when nothing is sent back.
char *social_service_handle(const SocialHost *host, const char *request);
