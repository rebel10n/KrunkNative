#pragma once
#include <cJSON.h>
// KN_KRUNKER_OFFICIAL: this client in an OFFICIAL krunker.io game, over Krunker's own protocol
// (client/src/net/krunker_net.c; the packet translation is client/src/net/krunker_link.c).
//
// The mode sits beside our own transports (knet UDP / TCP natively, WebSocket / WebRTC /
// WebTransport on the web): while it owns the connection, client_net_send_packet() hands our
// outbound packets to krunker_link (the knet transports are stopped first) and krunker_link turns
// Krunker's packets into NetPackets for the normal inbound queue (client_net_queue_packet), so the
// game, HUD and menu run unchanged.
//
// Failures say what went wrong, never a generic "can't be joined" (krunker_net.c fail_*): the player
// cancelled the security challenge, krunker.io refused its token twice, the matchmaker / game server
// is unreachable (natively: the proxy of Settings > Client named), krunker.io refused this network
// (ACCESS DENIED: points at Settings > Client > Proxy), or the game plays a map this client's catalogue
// does not have (the session is left; the error screen names the map index).
//
// Join flow (game.deob.js; matchmaker client at :147380-147690), the live client's order:
//   0. the krunker.io account's fresh access token (krunker_account.h: refreshed first when it is
//      about to expire), none for a guest
//   1. GET <matchmaker>/seek-game?hostname=krunker.io&region=<r>&autoChangeGame=false[&game=<gameId>]
//          &dataQuery={"v":"<buildVersion>"}[&accessToken=<the account's>]
//      (matchmakerSeek's parameter order; no validationToken: iïîìîíì passes undefined for a guest)
//      -> {host, port, gameId, clientId, data}, or 400 {"error":"CaptchaRequired"}:
//   1b. CaptchaRequired: the player solves Cloudflare Turnstile (krunker_captcha.h: the widget on the
//      web, krunker.io in the system browser + a pasted token / a localhost callback natively), then
//      GET <matchmaker>/generate-token (a fresh validation token) and seek-game again with
//      validationToken=<token> after autoChangeGame and captchaToken=<turnstile> after dataQuery
//      (:147445-147467; at most 2 captcha prompts per join, like the live client). Turnstile tokens
//      are single-use: sent once, wiped, never logged. The validation token is single-use too: the seek
//      that carries it spends it (wiped), and no later seek of the join sends it. A refused one
//      (InvalidClientValidationToken / ExpiredClientValidationToken) gets one fresh token and one more
//      seek per join, without the captcha (spent); a second refusal is the error.
//   3. WebSocket <scheme>://<host>[:port]/ws?gameId=<gameId>&clientKey=<clientId>[&at=<access token>]
//      (:222360-222371); a signed-in socket gets "a" ACCOUNT_RESPONSE (the account's name and level,
//      shown by the account UI) and our "en" carries the player's own loadout (krunker_link.c)
//      production (the default matchmaker): wss:// and no port; a matchmaker given explicitly
//      (tests, mirrors): its own scheme (http -> ws, https -> wss) and the port seek-game returned
//   4. io-init, load / init (map + mode), our "en" when the player presses Play, then the game.
// Two captchas exist: the matchmaker's CaptchaRequired is Cloudflare Turnstile (a human solves it),
// the socket's "cpt" is an ALTCHA proof of work, answered in C (krunker_altcha.h) with the modal's
// "verifying…" state. The live client also sends `accept-language: en-US,en;q=0.6;` on matchmaker
// requests; this one does not (a browser adds its own on the web).
//
// Transport: the browser WebSocket on the web (emscripten/websocket.h, like net_web.c, which stays
// as it is for our protocol); natively a small RFC 6455 client on http_conn.c (TLS, the client
// proxy setting / *_PROXY environment, as the official lobby list uses: krunker.io refuses some
// networks) on its own thread. Windows has no native path yet (http_conn.c is POSIX).
//
// Switches:
//   Official games are joined in this client by default: a click on an official lobby of the server
//       browser, Quick Match / Find New Game (F4) with Settings > Quickplay > Server Type Official or
//       Both (krunker_lobbies_open), and --krunker / ?krunker= below.
//   KRUNKNATIVE_KRUNKER_JOIN=browser (native) / ?krunkerJoin=browser (web)   the opt-out: lobby clicks
//       and seeks open https://krunker.io/?game=<id> in the system browser instead, as before the
//       in-client join was the default ("client" / "1" / unset = in-client). --krunker / ?krunker=
//       always join in-client: they ask for it explicitly.
//   --krunker [gameId] (native) / ?krunker=<gameId|1> (web)   join an official game at startup
//   KRUNKNATIVE_KRUNKER_MATCHMAKER=<url> (native, shared with krunker_lobbies.h) / ?krunkerMM=<url>
//   KRUNKNATIVE_KRUNKER_REGION=<id> / ?krunkerRegion=<id>      seek region (default de-fra)
//   KRUNKNATIVE_KRUNKER_TRACE=1 / ?krunkerTrace=1              log every frame in and out as JSON, and
//       the sync line below
//   KRUNKNATIVE_KRUNKER_TRACE=sync / ?krunkerTrace=sync        only one "[krunker] sync" line a second while
//       in a game: "q" windows / input frames sent, the last isn sent, the server's last ack ("l"
//       inputSN) and how far behind it is, "l" / "f" received, the corrections and the largest one
//       (units), pending inputs, the server's position of us next to ours, the locks, the CHEAT_CHECKs
//       answered / failed. Off by default.
//
// Not done (the demo bar is "joined, sees others move, can move and chat"):
//   - "q" syncTimes (the servTim of frames that fired, game.deob.js:221756): shots and melee are
//     sent as input bits only, so the server's lag compensation sees them late
//   - weapons: loadouts / ammo / reload ("inat", "uamo", "wep"), projectiles ("pr", "pre"),
//     tracers ("9"), explosions ("ex"); scores, leaderboard ("5", "7"), timers ("t"), end of round
//     ("end"), modes / objectives / teams ("tm", "ts", "obj", ...), map votes, streaks, sprays,
//     voice, sounds, every KrunkScript packet, "00" names, "uid" reconnect ids
//   - maps beyond Krunker v7.1.4's 32 official ones (init's mapIndex >= 32; custom / community
//     maps the API serves by index), the mode's own rules for the Krunker mode id
//   - cosmetics, skins: everyone is shown with default cosmetics ("0"'s cosmetic slots are not read);
//     the account (krunker_account.h) is the player's own when signed in, a guest's otherwise
//   - CHEAT_CHECK natively (QuickJS, the CHEAT_CHECK agent's), Windows; on the web a Turnstile token
//     from krunker.io can only be pasted (a page cannot take the localhost callback)
//   - the "cpt" answer's payload shape is ALTCHA's widget format, not readable in the bundle: unverified
//     against a live server
//   - a dropped official session does not reconnect by itself (the page's reconnect seeks anew)
#include <client.h>

int krunker_net_in_client(void);   // official lobbies join in-client: 1 unless the browser opt-out above is set
void krunker_net_attach(Client *client); // startup (backend_startup.c): the client the mode drives
// Seek and join an official game (game_id NULL / "": the matchmaker picks one in the region;
// region NULL: the default). Stops the knet transport / the offline game first. 1 = started.
int krunker_net_join(Client *client, const char *game_id, const char *region);
// Reload (game_reload.c: F5's relaunch, the console's `reload`): krunker_net_join for the game the
// player was in. Only when the matchmaker says it is gone (InvalidGameId) or full (GameFull): a notice
// says so and the matchmaker picks another game in the region (no excludeGame). 1 = started.
int krunker_net_reload_join(Client *client, const char *game_id, const char *region);
// Find New Game for an official game (F4 / Quick Match): use_list = 1 first asks <matchmaker>/game-list
// and joins the busiest public game with room in the same region (region NULL: the one the player is
// in) for the saved seek mode / map that is not one of the recent games (recent_lobbies.h). Without
// such a game (or a list) it seeks the way krunker.io's client does (backToOrigin's ?exclude=):
// seek-game with excludeGame = the game the player is in (or was in last) and dataQuery dsmo / dsma.
// krunker.io takes ONE excludeGame, so an answer that is another recent game is sought again excluding
// it (twice at most) and the least recent answer is joined when only recent games come back; without
// the list a bounce between two games can then remain (the matchmaker's choice). 1 = started.
int krunker_net_seek_new(Client *client, const char *region, int use_list);
// A ranked match's game socket (v10's joinAssignment / quickJoinRankedMatch, ranked_ui.c): connection is
// the assignment's "host[:port]" (v10's _33940), game_id its gameId for the log and the session ("" none).
// JOINED_RANK goes on the open game socket first, when one is open (v10 sends it on its connected socket
// only). The krunker.io account's token goes as at= (no gameId / clientKey). 1 = started, 0 = no client.
int krunker_net_join_match(const char *connection, const char *game_id);
// v10's isInOtherRankedMatch (v10:98528): the open game socket is a ranked game on another host than the
// connection's; the MATCHED for it is ignored. 1 = yes.
int krunker_net_ranked_elsewhere(const char *connection);
// 1 = the official game this client is in (or joined last), 2.. older ones; 0 = not one of the last
// RECENT_LOBBIES_MAX (recent_lobbies.h). krunker_lobbies_pick picks those last.
int krunker_net_recent_rank(const char *game_id);
const char *krunker_net_game(void); // the official game being joined / played, "" when none
// The official session (any state but off, the dropped one included): its game id ("" when none) and
// the region it was asked for ("" when none). The server info reads both (ui_console.c, servers_ui.c).
const char *krunker_net_session_game(void);
const char *krunker_net_region(void);
// copyInviteLink in an official game: <site>/?game=<id>, the id as krunker.io writes it ("NY:bbbbb"),
// the site KRUNKNATIVE_KRUNKER_URL or https://krunker.io. 1 = written; 0 = no official session.
int krunker_net_invite_link(char *out, size_t n);
// Leave the official game (no-op when not in one). reason NULL: an intended switch (another
// session follows); else the error screen shows it.
void krunker_net_leave(Client *client, const char *reason);
int krunker_net_active(void);      // the mode owns the connection (seeking, connecting, open or dropped)
// The krunker.io account the official game server signed in on this session ("a" ACCOUNT_RESPONSE,
// v10's _67362 there): 1 = one is (out filled), 0 = a guest or no official session (out zeroed).
typedef struct {
    char name[64];
    int level;
    int role;                       // player_role: 2 isMod (Marshal), 3 isDeputy
    int dev;                        // player_developer
    int job_rating, job_rating_pos; // jobrating / jobratingpositive (Helpful Reports)
} KrunkerNetAccount;
int krunker_net_account(KrunkerNetAccount *out);
// The krunker.io account was signed in during an official game (account_ui.c): the open socket
// logs in with it (v10's rpc LOGIN).
void krunker_net_account_signed_in(void);
// The official game socket's account-scoped frames, on the main thread: label + the whole frame
// ["name", args...] ("a" ACCOUNT_RESPONSE without its token, "ua", "uf", "bpdl", "bppd", "chg",
// "uchp", "cln"). One listener (account_ui.c: the krunker.io account's real data, as v10 keeps it).
typedef void (*KrunkerNetAccountFn)(const char *label, const cJSON *frame);
void krunker_net_account_listen(KrunkerNetAccountFn fn);
// A window's read-only request over the game socket for the krunker.io account, as the frame the
// window would send kn_brain (["bpdl"], ["vcln"], ["cln", 3, value]); 0 = not sent (not open, or not
// a read-only request: nothing that spends KR or changes the account goes out).
int krunker_net_send_account_frame(const cJSON *frame);
int krunker_net_open(void);        // the WebSocket is open
// client_net_send_packet while active: our packet translated and sent (or dropped). 1 = sent.
int krunker_net_send_packet(NetPacketType type, const void *payload, uint16_t length);
void krunker_net_reconnect(Client *client); // the page's reconnect: seek the same game again
// The error screen's text `error` is the one an official game ended on (a join that failed, the session
// closed with a reason) and no official join started since: the screen is krunker.io's (menu_ui.c: v10's
// Find Game, a new official game).
int krunker_net_failure_shown(const char *error);
void krunker_net_clear_failure(void); // main thread: a session is in (menu_ui.c), any later error screen is its own
void krunker_net_tick(Client *client);      // main thread, once per frame: notices, captcha
// Hosting a custom game on krunker.io's official servers (v10's createPrivateRoom, v10.js:198129: CUSTOM_SERVER
// "custom" over the official socket, answered by "cust"). config: v10's request object, a JSON object that the
// call does not consume (flags, maps / modes / classes, passCode, serverConfig varN keys, teams / specs, weps /
// cls / prks overrides). done runs once on the main thread (from krunker_net_tick): with game_id set and error
// NULL when the game was made (join it with krunker_net_join(client, game_id, region)); with game_id NULL and
// error the sentence to show otherwise: the server's refusal, or "Please try again." when no answer comes within
// the HTTP timeout. region: the official session's region.
// Returns 0 when the request was not sent and done is not called: no official connection, a signed-out account
// (v10's guest step, the security challenge "bCpt", is not implemented), a request already waiting, or a config
// that is not an object. 1 = sent.
typedef void (*KrunkerHostDoneFn)(void *user, const char *game_id, const char *region, const char *error);
int krunker_net_host_custom(const cJSON *config, KrunkerHostDoneFn done, void *user);
// Whether the official session wants no input frame built now (net/recon.c samples none): v10's gameLoop
// builds none while "lock" holds or while it has no player of its own (krunker_link_inputs_paused), and an
// init's map still loading (the packets after it held) is the latter. 0 when no official game is open.
int krunker_net_inputs_paused(void);
// Whether our socket hosts the open official custom lobby (krunker_link_is_host). 0 when no official game is open.
int krunker_net_is_host(void);
// The frame at `index` of the open "q" window fired (a shot or melee: v10's syncTime): its server time goes
// out with the window (krunker_link_mark_sync). Called on the main thread after that frame's tick, before the
// window is sent; nothing when no official game is open.
void krunker_net_mark_input_sync(int index);
