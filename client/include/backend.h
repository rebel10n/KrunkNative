#pragma once
// Brain client: KrunkNative's backend (services/brain, kn_brain = Krunker v3.1.1's
// matchmaker "brain") seen from the native and web clients. Two transports:
//
//   * public HTTP routes (v3.1.1 matchmaker/publicRoutes.ts): /mm/ping-list,
//     /mm/game-list, /mm/game-info, /mm/seek-game, /api/player-count, /api/maps, ...
//     backend_http_get() is asynchronous; the callback runs on the main thread from
//     backend_poll().
//   * the social socket (v3.1.1 matchmaker/socialWs.ts, socialURL + "/ws"): frames
//     ["label", ...args]. KrunkNative speaks JSON text frames (`?enc=json`, which the
//     brain accepts next to v3.1.1's msgpack); listeners get every frame as a cJSON
//     array on the main thread. Two synthetic frames report the connection itself:
//     ["__open"] after the socket opened, ["__close"] after it dropped (it reconnects
//     on its own with a backoff while anything wants it open).
//
// Native: worker threads + queues; POSIX over http_conn.c (https:// / wss:// with OpenSSL),
// Windows over WinHTTP (backend_winhttp.c: TLS, system proxy, WebSockets on Windows 8+). Web:
// XMLHttpRequest (emscripten_async_wget2_data) and the browser WebSocket.
//
// The brain URL (no trailing slash) comes from, first match wins:
//   native: --brain <url>, KRUNKNATIVE_BRAIN_URL, the `knBrainUrl` storage key, else the
//           online mode's account service (online_mode.h; probed; never config/config.json)
//   web:    ?brain=<url>, Module.krunkNativeBrainUrl, else the page origin
//           (`?brain=0` / `?offline=1` disable it).
// Without a URL every call is a no-op: offline / singleplayer and direct connects
// (--connect) behave exactly as before the backend existed.
#include <cJSON.h>
#include <stddef.h>

// status 0 = transport error; body is then the reason as plain text ("cannot connect to ...",
// "host not found (WinHTTP error 12007)"), never JSON, and the failure is already logged as
// "[http] <method> <url> failed: <reason>". A brain GET that got no answer is tried once more before
// this is reported (backend_native.c http_retry); a POST and any real answer are not.
// BACKEND_HTTP_TLS_FAILED (-2): the target's TLS handshake failed (its certificate or name did not
// verify). Only backend_http_get_parallel reports it; every other request sees 0 for it.
#define BACKEND_HTTP_TLS_FAILED (-2)
typedef void (*BackendHttpFn)(void *user, int status, const char *body, size_t length);
typedef void (*BackendSocialFn)(void *user, const cJSON *frame);

// Startup: the CLI value (--brain, may be NULL). Call once before backend_poll().
void backend_init(const char *cli_url);
int backend_enabled(void);
const char *backend_url(void);          // "" when disabled
void backend_set_url(const char *url);  // runtime change (settings); "" disables
int backend_url_is_default(void);       // the URL is a default (web: page origin; native: the online-mode account service): probed
// A default URL is only used once GET /mm/status (kn_brain only; net-bridge's fallback
// routes do not answer it) confirmed a brain: 1 = brain, 0 = none, -1 = probing / not yet.
// Explicit URLs count as available. The social socket stays closed until it is 1.
int backend_available(void);
void backend_probe_start(void);         // (re)probe a default URL; no-op while one runs

// Asynchronous GET of backend_url() + path (path includes the query, starts with '/').
// Returns 0 when it could not be queued (backend disabled, out of memory).
int backend_http_get(const char *path, BackendHttpFn fn, void *user);
// Any absolute URL (the auth service / server finder are not the brain): method "GET" /
// "POST", json_body sent as application/json (NULL = none). Native https:// needs a build
// with OpenSSL (KRUNKNATIVE_HAVE_TLS); web uses fetch(). Same callback rules as above;
// status is the HTTP status (204 has an empty body), 0 = transport error.
int backend_http_request(const char *method, const char *url, const char *json_body, BackendHttpFn fn, void *user);
// The brain gate (account_ui.c registers it): while it answers 1 the brain is not the active backend
// (the player is connected to an official krunker.io server, backend_ui.h account_backend()). Every brain
// request (backend_http_get, backend_http_request / parallel / media GETs on a URL under backend_url())
// then fails at once (0), the callers' failure path; requests to other hosts (krunker.io, its lists, the
// media hosts) are not affected. NULL = no gate (the brain is always the backend, as before).
typedef int (*BackendBrainGateFn)(void);
void backend_set_brain_gate(BackendBrainGateFn fn);
// 1 while the gate refuses brain requests (what backend_http_get would answer 0 to right now, without the
// refusal log): a caller choosing where a request goes (servers_ui.c's seeks) reads the gate's own state.
int backend_brain_gated(void);
#ifndef __EMSCRIPTEN__
// Native: GET of a URL on another host (krunker.io's game list, krunker_lobbies.c) on its own
// worker, with its own connect / receive timeout, so it never queues behind (or delays) the
// brain's requests. Same callback rules as backend_http_get.
int backend_http_get_external(const char *url, int timeout_ms, BackendHttpFn fn, void *user);
// Native: GET of page media (remote images, remote_images.h) on a third worker of its own, so a
// page's thumbnails never queue in front of the game list. Bodies are binary (length counts).
int backend_http_get_media(const char *url, int timeout_ms, BackendHttpFn fn, void *user);
// Native: a GET on a thread of its own, at once and alongside any other (a region ping race,
// region_ping.h: every region's /ping in flight together, the answers delivered in the order they
// came). Same callback rules as backend_http_get, and this one alone reports BACKEND_HTTP_TLS_FAILED.
// The web's backend_http_request is concurrent already.
int backend_http_get_parallel(const char *url, int timeout_ms, BackendHttpFn fn, void *user);
// Native: the social socket reconnects (same brain), e.g. through a changed proxy (http_proxy.h).
void backend_social_reconnect(void);
#endif

// Main thread, once per frame: delivers HTTP completions and social frames.
void backend_poll(void);

// Social socket. backend_social_open() is idempotent; backend_social_send() queues
// the frame (JSON array) until the socket is open. Returns 0 when disabled.
void backend_social_open(void);
int backend_social_is_open(void);
int backend_social_send(const cJSON *frame);
int backend_social_send_text(const char *json);
// The krunk.io account is not the active one (backend_ui.h: an official krunker.io game): the
// social socket is held, every frame is refused (0) so nothing done under the krunker.io account
// reaches the krunk.io account; only that session's own upkeep (its token restore) still goes, with
// backend_social_send_session.
void backend_social_hold(int on);
int backend_social_held(void);
// While held, a frame goes to this instead (account_ui.c: the krunker.io account's read-only requests
// over the official game socket, krunker_net_send_account_frame); 0 = refused.
typedef int (*BackendHeldSender)(const char *json);
void backend_social_set_held_sender(BackendHeldSender fn);
int backend_social_send_session(const cJSON *frame);
int backend_social_send_raw(const char *json); // (the platform halves: queue the frame, no hold)
int backend_social_listen(BackendSocialFn fn, void *user); // up to 8 listeners; returns 0 when full
void backend_social_unlisten(BackendSocialFn fn, void *user);

// Helpers shared by the native/web halves (backend_common.c).
void backend_url_encode(char *out, size_t n, const char *s); // application/x-www-form-urlencoded value
