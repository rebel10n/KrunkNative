#pragma once
// The krunker.io account of the client (client/src/net/krunker_account.c): the second account
// backend, beside the KrunkNative one (kn_brain's session, account_ui.c / backend_ui.h). Each server
// type has its own: KrunkNative servers see the KrunkNative account, krunker.io servers (official
// lobbies, krunker_net.h) see this one; both can be signed in at once, and either can be a guest.
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
// Endpoints (tests, mirrors): KRUNKNATIVE_KRUNKER_API (default https://api.krunker.io) and
// KRUNKNATIVE_KRUNKER_AUTH (default https://crucible.frvr.com/v1/auth); on the web ?krunkerApi= /
// ?krunkerAuth=. Natively the requests go through the client's proxy (http_proxy.h: its krunker.io
// scope covers api.krunker.io and crucible.frvr.com).

#define KRUNKER_ACCOUNT_DEFAULT_API "https://api.krunker.io"
#define KRUNKER_ACCOUNT_DEFAULT_AUTH "https://crucible.frvr.com/v1/auth"
#define KRUNKER_ACCOUNT_REFRESH_MARGIN_S 60

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
int krunker_account_submit_code(const char *code); // KRUNKER_ACCOUNT_CODE: the code asked for
void krunker_account_cancel(void);                // a running sign-in / code step ends (guest)
void krunker_account_sign_out(void);

// The access token for a krunker.io join, fresh (refreshed first when it is about to expire):
// fn(user, token) once, on the main thread (maybe before this returns); token NULL = join as a
// guest (not signed in, or the refresh failed). The token is valid during the call only.
typedef void (*KrunkerAccountTokenFn)(void *user, const char *token);
void krunker_account_fresh_token(KrunkerAccountTokenFn fn, void *user);

// The game server's ACCOUNT_RESPONSE ("a") for this account's socket: its name and level (-1 = not
// in it). Main thread.
void krunker_account_note_game(const char *name, int level);

// A username sign-in's Turnstile request is the one krunker_captcha.h shows (krunker_net leaves it open).
int krunker_account_owns_captcha(void);

// Main thread, once per frame (backend_frame): the requests' answers, the restore at startup.
void krunker_account_tick(void);

// ---- the UI (client/src/ui/krunker_account_ui.c)
// The "krunker.io account" modal (page handlers kioAccount() / knKioAccount(n)).
void krunker_account_ui_open(void);
void krunker_account_ui_tick(void); // per frame (backend_frame): the modal and the Servers window follow the account
// krunker_account_ui_accounts_html (the "Accounts" line) is in ui_logic.h (it takes a StrBuf).
