#pragma once
// Krunker v10.0.2's ranked services at krunker.io, for the Ranked popup (client/src/ui/ranked_ui.c)
// while the active account is a krunker.io one (ranked_route.h). The same hosts and paths as v10's
// client (krunker_host_url, krunker_account.h):
//   api           GET /player-profile/me/season/<s>/region/<r>, GET /leaderboards/rankings/season/<s>/region/<r>,
//                 GET /match-history/me/season/<s>/gameMode/undefined/isRanked/true/region/<r>,
//                 GET /ranked/me/connection, POST /player-profile/me/claim-milestone/region/<r>   (v10:25937-25985)
//   gapi          GET /ranked/shop, POST /ranked/purchase {skin_id}                              (v10:26311-26386)
//   gamefrontend  the queue socket /v1/matchmaking/queue?token=&maps=&regions=                 (v10:98420-98460)
//   (the match's game socket: krunker_net.h, krunker_net_join_match; the RP balance: krunker_account.h,
//   the account row's rankedPoints. v10 never asks krunker.io for the balance: no /player/me)
// Every request carries Authorization: Bearer <the krunker.io account's fresh access token>, and POSTs
// an X-Idempotency-Key (v10's API client, v10:27594-27605). The answers are v10's {data, error}
// envelope; a 204 is data null. Not signed in, or no token: nothing is sent.
#include <cJSON.h>
#include <stddef.h>

typedef enum {
    KRUNKER_RANKED_API = 0, // krunker_host_url KRUNKER_HOST_API
    KRUNKER_RANKED_GAPI,    // KRUNKER_HOST_GAPI
} KrunkerRankedHost;

// Answer of one request, called once on the main thread. status: the HTTP status (0 = a transport
// error). body: the parsed envelope ({data, error}), NULL when the answer had none.
typedef void (*KrunkerRankedFn)(void *user, int status, const cJSON *body);

// 1 = sent: fn runs once, later (from krunker_account_tick / http_stream_poll). 0 = not sent (no
// krunker.io session): fn never runs, the caller reports it.
int krunker_ranked_request(KrunkerRankedHost host, const char *method, const char *path, const char *json_body,
                           KrunkerRankedFn fn, void *user);

// The queue socket: v10's connectWebSocket (v10:98420). regions: the comma list ("na", or "as,eu,na").
// on_frame gets each text frame's parsed JSON ({type, payload}), NULL for a frame that does not parse; on_done runs once when the socket
// ends (failed = it never opened). krunker_ranked_queue_stop ends it with no callback. One queue at a
// time. 1 = started, 0 = not (no session, a queue already running, or no socket on this platform).
typedef void (*KrunkerQueueFrameFn)(void *user, const cJSON *frame);
typedef void (*KrunkerQueueDoneFn)(void *user, int failed);
int krunker_ranked_queue_start(const char *regions, KrunkerQueueFrameFn on_frame, KrunkerQueueDoneFn on_done, void *user);
void krunker_ranked_queue_stop(void);
int krunker_ranked_queue_active(void);

// Main thread, once per frame (backend_frame): the queue's frames and its end.
void krunker_ranked_tick(void);
