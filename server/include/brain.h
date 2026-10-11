#pragma once
// Game server -> kn_brain link (port of the v3.1.1 server/brainClient.ts; protocol in
// doc/backend.md section 5, game server side in section 8).
//
// A dedicated thread runs a knsvc event loop with the internal WebSocket: hello on open,
// pong to ping, reconnect with exponential backoff (1 s .. 30 s), and routing of replies
// by `rid`. Everything game-related (frames to send, replies, brain commands) crosses to
// the main thread through brain_post() / brain_next_event(); the main thread holds
// g_server->lock while it handles them (server/src/accounts.c).
//
// Without gameserver.brain_url (or without knsvc in the build) brain_start() returns 0 and
// every other call is a no-op: server_bin is the standalone server it always was.
#include <server_config.h>
#include <cJSON.h>
#include <stdint.h>

typedef enum {
    BRAIN_EV_NONE = 0,
    BRAIN_EV_LINK_UP,      // welcomed: push a full snapshot (frame = the welcome, may be NULL)
    BRAIN_EV_LINK_DOWN,    // connection lost: pending requests failed (their replies never come)
    BRAIN_EV_FRAME,        // a brain -> node frame (assign, host, drain, auth-reply, acct-r, ...)
} BrainEventKind;

typedef struct {
    BrainEventKind kind;
    cJSON *frame;          // BRAIN_EV_FRAME: the parsed frame, caller frees with cJSON_Delete
} BrainEvent;

int brain_start(const GsConfig*);      // 1 = brain configured and thread started
int brain_enabled(void);
int brain_linked(void);                // connected and welcomed
// Sends a frame (consumed). reliable: kept and resent after reconnects until a frame
// carrying the same "rid" comes back (commits). Returns 0, or -1 when dropped.
int brain_post(cJSON *frame, int reliable);
uint32_t brain_next_rid(void);
// Main thread: next event, 0 when none.
int brain_next_event(BrainEvent*);
// Send the queued frames, wait up to wait_ms for reliable ones to be acknowledged, close
// the socket and join the thread. Safe without a brain.
void brain_stop(int wait_ms);

// Published (DB) maps: GET <brain http>/api/map-data?name= on a worker thread; the answer
// is a BRAIN_EV_FRAME {"type":"map-data","rid","ok","map"|"err"}. -1 = not started.
void brain_set_http_base(const GsConfig*);
int brain_fetch_map(uint32_t rid, const char *name);

// Join ticket (seek-game clientId): HS256 JWS signed with gameserver.join_secret (default
// brain_secret). Returns the claims (caller frees) when the signature and exp are valid.
cJSON *brain_verify_ticket(const char *ticket, char *err, size_t err_size);
// 16 hex chars of CSPRNG output (match ids), or a time-based fallback without knsvc.
void brain_random_hex(char *out, size_t size, size_t bytes);
