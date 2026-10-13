#pragma once
// Server-info query (doc/backend.md 8.8): a tiny, transport-independent request/response
// that server lists (the central server finder, the client's server browser, scripts)
// send to a game server to read its name, map, mode, players and tags. Source A2S style:
//
//   request:  FF FF FF FF "KNINFO" [00 <nonce, <= 32 bytes>]
//   reply:    FF FF FF FF "KNinfo" 00 <JSON object, <= SERVER_INFO_JSON_MAX bytes>
//
// JSON: {"v":1,"game":"krunknative","name","map","mode","modeId","players","max","bots",
//        "tags":[...],"community","online_mode","password","region","version","port","nonce",
//        "commit","commit_time","build_time","protocol","ruleset"}
// version = the build's git describe; commit (12 hex), commit_time (unix s), build_time (ISO-8601
// UTC) and protocol ("major.minor") come from shared/include/build_info.h. The finder compares them
// with the latest server release (services/brain/release_info.c, doc/backend.md "Build version").
//
// The 0xFFFFFFFF prefix never starts a game packet, so a UDP game transport can demux
// queries on its own socket. Pure byte buffers here: no sockets, no server state.
#include <stddef.h>
#include <stdint.h>

#define SERVER_INFO_PREFIX_LEN 4
#define SERVER_INFO_NONCE_MAX 32
#define SERVER_INFO_JSON_MAX 1200
#define SERVER_INFO_REPLY_MAX (SERVER_INFO_PREFIX_LEN + 7 + SERVER_INFO_JSON_MAX)
#define SERVER_INFO_TAGS_MAX 16

typedef struct {
    char name[64];
    char map[64];
    char mode[48];
    int mode_id;
    int players;
    int max_players;
    int bots;
    char tags[SERVER_INFO_TAGS_MAX][24];
    int tag_count;
    char community[64];
    int online_mode;
    int password;
    char region[32];
    char version[48];               // git describe of the build
    int port;                       // game port clients connect to
    char commit[16];                // 12-hex commit ("" unknown)
    int64_t commit_time;            // its committer date, unix seconds (0 unknown)
    char build_time[24];            // ISO-8601 UTC
    char protocol[8];               // NET_PROTOCOL_MAJOR.MINOR, "1.3"
    char ruleset[8];                // the Krunker version it plays (ruleset.h key): "v7", "v10"; "" = v7 (older servers)
    char nonce[SERVER_INFO_NONCE_MAX + 1];
} ServerInfo;

// 1 when buf starts with FF FF FF FF (a query-protocol datagram, not game traffic).
int server_info_is_query(const uint8_t *buf, size_t len);
// Request datagram into out; nonce may be NULL. Returns its length (0 if cap is too small).
size_t server_info_build_request(uint8_t *out, size_t cap, const char *nonce);
// Parses a request; fills nonce (cap SERVER_INFO_NONCE_MAX + 1, "" when absent).
// Returns 1 for a valid request, 0 otherwise.
int server_info_parse_request(const uint8_t *in, size_t len, char *nonce);
// Reply datagram for info (nonce taken from info->nonce). Returns its length, 0 on error.
size_t server_info_build_reply(uint8_t *out, size_t cap, const ServerInfo *info);
// Parses a reply into info. Returns 1 on success.
int server_info_parse_reply(const uint8_t *in, size_t len, ServerInfo *info);
