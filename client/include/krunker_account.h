#pragma once
#include <stddef.h>
#include <cJSON.h>
// The krunker.io account of the client (client/src/net/krunker_account.c): the second account
// backend, beside the krunk.io one (kn_brain's session, account_ui.c). Which one the client shows
// and signs in to is the ACTIVE BACKEND (backend_ui.h active_account()): this one while an official
// krunker.io game is joined or played, krunk.io everywhere else. There is no UI of its own: v10's
// login modal (v10_login.c), header and Account window drive it when it is the active backend.
// Each backend keeps its own session; either can be a guest.
//
// Only the player's own account, signed in by the player: no account creation, no automation of
// anything krunker.io asks a human to do. The flow is the live client's (game.deob.js, READ ONLY;
// the FRVR SDK https://krunker.io/libs/frvr-sdk.min.js for the refresh):
//
//   sign in    username: Cloudflare Turnstile first (getCaptchaToken, :115623, the same widget and
//              sitekey as the matchmaker's, shown by krunker_captcha.h: the player solves it), then
//              POST <api>/auth/login/username {"username","password","b":<turnstile token>}
//              (auth.loginUsername :26191; "a", the login form's gzip'd pointer/keystroke telemetry, is
//              optional and never sent: this client does not fabricate it)
//              email (the login has an '@'): POST <api>/auth/login/email {"email","password"} (:26204)
//              Answer {data, error} (iïiiïïì :26109): data.type "login_ok" -> access_token +
//              refresh_token; "check_2fa" -> POST <api>/auth/2fa/challenge/<challenge_id> {"code"};
//              "enter_code" -> POST <api>/auth/code-challenge/<challenge_id> {"code"} (the challenge loop,
//              iííìiïï :115667). The other steps (ensure_verified, ensure_migrated, select_username,
//              update_password, kid_*) are finished on krunker.io itself: the player is told which.
//              POSTs carry Accept: application/json, Content-Type: application/json and a fresh
//              X-Idempotency-Key (the API client's fetch, :27545).
//   profile    GET <api>/player/me with Authorization: Bearer <access> (player.getMe :26326), best
//              effort: a name in it replaces the one typed; the game server's own "a" (ACCOUNT_RESPONSE)
//              then names the account and its level (krunker_account_note_game)
//   refresh    the access token is a JWT; within KRUNKER_ACCOUNT_REFRESH_MARGIN_S of its exp (or after)
//              POST <auth>/refresh {"refreshToken"} -> 201 {accessToken, refreshToken} (FRVR SDK
//              AUTH_REFRESH, apiBaseURL https://crucible.frvr.com/v1/auth); 401 = the session is over
//              (signed out, said so); other failures keep the session, the join goes on as a guest
//   use        krunker_net: seek-game's accessToken (matchmakerSeek :147630) and the game socket's
//              `at=` (:222367) carry krunker_account_fresh_token()'s token; nothing else sees it
//   sign out   the tokens are dropped here (FRVR.auth.logout is local: :210108)
//
// Tokens and passwords are never logged. The tokens are kept like the KrunkNative session
// (html_storage: kio_access_token, kio_refresh_token, beside kio_username / kio_level); passwords and
// 2FA codes are wiped once their request is sent.
//
// Endpoints (tests, mirrors): KRUNKNATIVE_KRUNKER_API (default https://gapi.svc.krunker.io) and
// KRUNKNATIVE_KRUNKER_AUTH (default https://crucible.frvr.com/v1/auth); on the web ?krunkerApi= /
// ?krunkerAuth=. Natively the requests go through the client's proxy (http_proxy.h: its krunker.io
// scope covers *.krunker.io and crucible.frvr.com).

// v10 has two API clients (v10:16439, 27594, 27600). The data client `_10444 = new _9997(_7283)`, _7283 = API_URI =
// https://api.krunker.io: playerProfile (/player-profile), matchHistory, leaderboards, ranked/me/connection and
// the claim-milestone (doc/v10/ranked.md). The gapi client `_10449 = new _10077(_7284)`, _7284 = GAPI_URI =
// https://gapi.svc.krunker.io: the account services and ranked shop / purchase. KRUNKER_HOST_API below is the
// data client; KRUNKER_ACCOUNT_DEFAULT_API is the gapi one (krunker_service_url, KRUNKER_SVC_API).
#define KRUNKER_ACCOUNT_DEFAULT_DATA_API "https://api.krunker.io"
#define KRUNKER_ACCOUNT_DEFAULT_API "https://gapi.svc.krunker.io"
#define KRUNKER_ACCOUNT_DEFAULT_GAPI "https://gapi.svc.krunker.io"
#define KRUNKER_ACCOUNT_DEFAULT_GAMEFRONTEND "https://gamefrontend.svc.krunker.io"
#define KRUNKER_ACCOUNT_DEFAULT_AUTH "https://crucible.frvr.com/v1/auth"
#define KRUNKER_ACCOUNT_DEFAULT_SOCIAL "https://social.krunker.io"           // v10 SOCIAL_URI
#define KRUNKER_ACCOUNT_DEFAULT_EVENT_BUS "wss://event-bus.svc.krunker.io"   // v10 EVENT_BUS_URI (friends)
#define KRUNKER_ACCOUNT_DEFAULT_CRUCIBLE_IAP "https://crucible.frvr.com/v1/iap" // FRVR's iap wallet (the SDK's iap base, sdk:4779)
#define KRUNKER_ACCOUNT_DEFAULT_EDITOR "https://editor.krunker.io"           // v10 EDITOR_URI (the Servers footer's "in Editor")
#define KRUNKER_ACCOUNT_REFRESH_MARGIN_S 60

// The krunker.io hosts v10.0.2's client talks to (_7283 api, _7284 gapi, _7287 gamefrontend). Tests and
// mirrors override each: KRUNKNATIVE_KRUNKER_DATA_API (the data client, on the web ?krunkerDataApi=) /
// KRUNKNATIVE_KRUNKER_API (the gapi-side API base; the data client follows it when its own is not set) /
// _GAPI / _GAMEFRONTEND. krunker_host_url writes the host's base + path (path starts with '/'); the base
// has no trailing slash.
typedef enum {
    KRUNKER_HOST_API = 0, // the data client (v10 API_URI): ranked profile, boards, history, connection, claim
    KRUNKER_HOST_GAPI,
    KRUNKER_HOST_GAMEFRONTEND,
} KrunkerHost;
void krunker_host_url(KrunkerHost host, const char *path, char *out, size_t n);

typedef enum {
    KRUNKER_ACCOUNT_GUEST = 0, // nobody signed in: krunker.io servers are joined as a guest
    KRUNKER_ACCOUNT_BUSY,      // a request (or the Turnstile step) is running
    KRUNKER_ACCOUNT_CODE,      // krunker.io asks for a code (2FA app or the email's)
    KRUNKER_ACCOUNT_SIGNED_IN,
} KrunkerAccountState;

KrunkerAccountState krunker_account_state(void);
int krunker_account_signed_in(void);
const char *krunker_account_name(void);   // the account's name ("" as a guest)
int krunker_account_level(void);          // its level (0 = not known yet)
double krunker_account_score(void);       // /player/me's player_score (0 = not known)
double krunker_account_funds(void);       // /player/me's KR (0 = not known)
// The last outcome for the account UI (an error, "signed out: the session expired", ...), "" none.
const char *krunker_account_message(void);
int krunker_account_message_is_error(void);
// KRUNKER_ACCOUNT_CODE: which code ("2fa": the authenticator app, "email": the email's).
const char *krunker_account_code_kind(void);
// Bumped by every change (state, name, message): the UI re-renders when it moved.
unsigned krunker_account_generation(void);

// Start a sign-in (login: a username, or an email when it has an '@'). 0 = refused at once (empty
// fields, one already running): krunker_account_message says why.
int krunker_account_sign_in(const char *login, const char *password);
int krunker_account_email_code(const char *email); // v10 "Send Email Code": loginEmailCode, then the code step
int krunker_account_recover(const char *email);    // v10's recover card: recoverAccount (the outcome: krunker_account_message)
int krunker_account_submit_code(const char *code); // KRUNKER_ACCOUNT_CODE: the code asked for
void krunker_account_cancel(void);                // a running sign-in / code step ends (guest)
void krunker_account_sign_out(void);
// The refresh token of the signed-in account (copied; it is a session secret: never logged, never shown).
// 1 = out filled, 0 = not signed in.
int krunker_account_refresh_token(char *out, size_t n);
// The account's sign-in and session events, for one listener (the Account Manager, alt_manager_ui.c). Called
// on the main thread. EXPIRED comes before the account is forgotten (its name still reads).
typedef enum {
    KRUNKER_EV_SIGNED_IN,      // a sign-in succeeded (krunker_account_name())
    KRUNKER_EV_SIGN_IN_FAILED, // a sign-in failed (krunker_account_message says why)
    KRUNKER_EV_EXPIRED,        // the session ended at the refresh (401): forgotten
    KRUNKER_EV_REFRESHED,      // the access token was refreshed (the refresh token may have rotated)
    KRUNKER_EV_CODE_ASKED,     // the sign-in asks for a code (krunker_account_code_kind): krunker_account_submit_code
} KrunkerAccountEvent;
typedef void (*KrunkerAccountEventFn)(void *user, KrunkerAccountEvent ev);
void krunker_account_listen_events(KrunkerAccountEventFn fn, void *user); // one listener (NULL = none)

// The access token for a krunker.io join, fresh (refreshed first when it is about to expire):
// fn(user, token) once, on the main thread (maybe before this returns); token NULL = join as a
// guest (not signed in, or the refresh failed). The token is valid during the call only.
typedef void (*KrunkerAccountTokenFn)(void *user, const char *token);
void krunker_account_fresh_token(KrunkerAccountTokenFn fn, void *user);

// The game server's ACCOUNT_RESPONSE ("a") for this account's socket: its name and level (-1 = not
// in it). Main thread.
void krunker_account_note_game(const char *name, int level);
// The account's rankedPoints (v10's Account.rankedPoints, row[47]): the game server's "a" / "ua" carry the
// row (krunker_net_tick passes it on, krunker_account_note_points). Session only: a sign-in or sign-out
// forgets it. krunker_account_ranked_points: 1 = known (out filled), 0 = not yet (no krunker.io game has
// signed this account in). Nothing else asks krunker.io for the balance (v10 never calls /player/me).
void krunker_account_note_points(double points);
int krunker_account_ranked_points(double *out);
// The held balance moves as v10's moves it after a ranked claim (+reward, v10:98744) or a purchase (-price, v10:111042).
void krunker_account_add_points(double delta);

// ---- krunker.io's services for the windows (the krunker.io account's real data while krunker.io is
// the active backend, backend_ui.h): v10's API client (v10:26094 _10067, v10:27520 fetch): base +
// "/" + path, Accept: application/json, POST bodies as JSON with an X-Idempotency-Key, and the
// account's fresh access token as Authorization: Bearer (refreshed first; none for a guest).
// Endpoints: KRUNKNATIVE_KRUNKER_API / _SOCIAL / _EVENT_BUS (web ?krunkerApi= / ?krunkerSocial= /
// ?krunkerEventBus=). Natively through the client's proxy like the sign-in.
typedef enum {
    KRUNKER_SVC_API = 0,   // gapi.svc.krunker.io (v10 GAPI_URI): auth, player, shop, ranked, maps ({data, error} answers)
    KRUNKER_SVC_SOCIAL,    // social.krunker.io: profiles, clans, feeds
    KRUNKER_SVC_EVENT_BUS, // event-bus.svc.krunker.io: the friends socket (/ws?at=<token>)
    KRUNKER_SVC_DATA,      // api.krunker.io (v10 API_URI, the data client): /daily-spin/progress ({data, error} answers)
    KRUNKER_SVC_EDITOR,    // editor.krunker.io (v10 EDITOR_URI): /player-count, the Servers footer's "in Editor"
    KRUNKER_SVC_CRUCIBLE,  // crucible.frvr.com/v1/iap: FRVR's iap wallet (the Stripe payment, doc/v10/checkout-stripe.md)
} KrunkerService;
void krunker_service_url(KrunkerService svc, const char *path, char *out, size_t n);
// The answer on the main thread: status 0 = unreachable / timed out; res = the parsed JSON body
// (NULL when it was not JSON), valid during the call only.
typedef void (*KrunkerApiFn)(void *user, int status, const cJSON *res);
// method "GET" / "POST" / ...; body JSON or NULL. 0 = refused at once (every slot busy).
int krunker_account_api(const char *method, KrunkerService svc, const char *path, const char *body, KrunkerApiFn fn, void *user);

// A username sign-in's Turnstile request is the one krunker_captcha.h shows (krunker_net leaves it open).
int krunker_account_owns_captcha(void);

// Main thread, once per frame (backend_frame): the requests' answers, the restore at startup.
void krunker_account_tick(void);
