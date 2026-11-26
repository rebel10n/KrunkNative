#pragma once
#include <krunker_altcha.h>
#include <krunker_proto.h>
#include <cJSON.h>
#include <stddef.h>
// krunker.io's own social socket: the one the hub page (social.html) opens to SOCIAL_URI, ported for the hub page's
// krunker.io mode (client/src/ui/social_ui.c while krunker.io is the active account backend). Source: the hub bundle
// social-CURLIuB7.js decoded (hub-10.0.7.js:7020-7110, doc/v10/parity-audit-2026-11-20-social.md):
//
//   connect   iîíìiîî: at = FRVR.auth.getFreshAccessToken(), pat = (await fetch(SOCIAL_URI + "/ws/ping")).json().token
//             (null when the fetch fails), tc = getSavedVal("testCode"); URL SOCIAL_URI/ws?tc=&at=&pat= (each part only
//             when set, in that order); the socket's connect strips http(s):// and opens ws(s):// (wss on an https page)
//   on open   the page asks for its view ("r", ...), every 20 s fetches /ws/ping again and sends PAT_REFRESH ("patR",
//             token) while connected; a closed socket (or one that never opened) shows "DISCONNECTED"
//   sign in   the socket is signed in by its at=; an account that signs in later (the auth listener) is
//             rpc(LOGIN, token) while connected and no account is shown yet (hub-10.0.7.js:7035-7039)
//   framing   the game socket's (deob-10.0.7.js:98766-98980): msgpack([type, ...args]) + the 2-byte ahNum trailer,
//             ahNum = (ahNum + 0x43e1) & 0xff before every frame but cptR (KRUNKER_AHKEY, krunker_proto.h); "io-init"
//             [socketId]; "cpt" [challenge] = the ALTCHA widget's proof of work, answered "cptR" [payload]; "cptR"
//             from the server ends it; "_0" / "_1" [rpcId, ...] resolve / reject an rpc ("_0" [rpcId, name, ...args]);
//             every other type goes to the page's handler table (hub-10.0.7.js:7073-7109)
//
// Natively the socket and the /ws/ping fetch go through the client's proxy (krunker.io scope); on the web they are the
// browser's. The Windows build has no WebSocket client for krunker.io (krunker_ws.h): the page shows DISCONNECTED.
// Endpoint: KRUNKNATIVE_KRUNKER_SOCIAL (web ?krunkerSocial=), default https://social.krunker.io (krunker_account.h).

// ---------------------------------------------------------------- the wire (no sockets: tests drive it)

typedef enum {
    KSW_BAD = -1,     // not a frame (the page's decoder throws; the frame is dropped)
    KSW_NONE = 0,     // handled by the socket itself (io-init, cptR, an rpc answer)
    KSW_PAGE,         // *frame is [type, ...args] for the page's handler table
    KSW_CAPTCHA,      // a "cpt": solving began (krunker_social_wire_captcha_step)
    KSW_CAPTCHA_FAIL, // a "cpt" this client cannot answer: the page shows "CAPTCHA FAILED" and the socket closes
} KrunkerSocialDecode;

typedef struct {
    KrunkerSession codec;
    int rpc_counter;
    double socket_id;   // io-init's socketId (-1 before)
    int solving;        // a "cpt" being solved
    int captcha_shown;  // the Security Challenge modal: from the "cpt" to the server's "cptR" (or the failure)
    KrunkerAltcha altcha;
    char rpc_names[8][16]; // the open rpcs' names by rpcId % 8 (logs)
} KrunkerSocialWire;

// A new connection: ahNum 0, no rpcs, socketId -1.
void krunker_social_wire_init(KrunkerSocialWire *w);
// socket.send(type, ...args) of a page frame [type, ...args] (cJSON array; JSON null, bools, numbers, strings, arrays
// and objects as @msgpack/msgpack encodes JS values). KRUNKER_PROTO_OK or an error (the frame is not sent).
int krunker_social_wire_encode(KrunkerSocialWire *w, const cJSON *frame, KrunkerBuf *out);
// socket.rpc(name, ...args): "_0" [rpcId, name, ...args] (args: a cJSON array or NULL). The rpcId, or -1.
int krunker_social_wire_rpc(KrunkerSocialWire *w, const char *name, const cJSON *args, KrunkerBuf *out);
// One message from the server. KSW_PAGE: *frame is the page's (owned by the caller: cJSON_Delete).
KrunkerSocialDecode krunker_social_wire_decode(KrunkerSocialWire *w, const void *data, size_t len, cJSON **frame);
// A step of the "cpt" search (budget numbers). 1 = solved, *out is the "cptR" frame; 0 = not yet; -1 = unsolvable.
int krunker_social_wire_captcha_step(KrunkerSocialWire *w, long budget, KrunkerBuf *out);

// ---------------------------------------------------------------- the client (main thread)

typedef enum {
    KRUNKER_SOCIAL_OPEN,   // the connect callback without an error: the page asks for its view
    KRUNKER_SOCIAL_FRAME,  // a frame for the page's handler table ([type, ...args])
    KRUNKER_SOCIAL_CLOSED, // closed, or never opened: the page shows "DISCONNECTED"
    KRUNKER_SOCIAL_ERROR,  // the socket's "error" event with a text (CAPTCHA FAILED)
} KrunkerSocialEvent;
typedef void (*KrunkerSocialFn)(void *user, KrunkerSocialEvent ev, const cJSON *frame, const char *text);

// Connects (a connection already up or being set up is closed first). 0 = refused at once.
int krunker_social_open(void);
// The page's socket.send; dropped when not open (the page queues before open; krunker_social queues too).
int krunker_social_send(const cJSON *frame);
// rpc(name, ...args): 0 = not connected.
int krunker_social_rpc(const char *name, const cJSON *args);
void krunker_social_close(void);
int krunker_social_is_open(void);
// 1 while the "cpt" challenge is being solved (the page's Security Challenge modal).
int krunker_social_captcha_shown(void);
// The socket's events since the last call, the PAT_REFRESH timer, the captcha slices and the LOGIN rpc (signed_in:
// the page shows an account; the krunker.io account signing in while it does not sends LOGIN). Once per frame.
void krunker_social_tick(int signed_in, KrunkerSocialFn fn, void *user);
