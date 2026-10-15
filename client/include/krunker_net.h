#pragma once
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
//   1. GET <matchmaker>/seek-game?hostname=krunker.io&region=<r>&autoChangeGame=false[&game=<gameId>]
//          &dataQuery={"v":"<buildVersion>"}
//      (matchmakerSeek's parameter order; no validationToken: iïîìîíì passes undefined for a guest)
//      -> {host, port, gameId, clientId, data}, or 400 {"error":"CaptchaRequired"}:
//   1b. CaptchaRequired: the player solves Cloudflare Turnstile (krunker_captcha.h: the widget on the
//      web, krunker.io in the system browser + a pasted token / a localhost callback natively), then
//      GET <matchmaker>/generate-token (a fresh validation token) and seek-game again with
//      validationToken=<token> after autoChangeGame and captchaToken=<turnstile> after dataQuery
//      (:147445-147467; at most 2 captcha prompts per join, like the live client). Turnstile tokens
//      are single-use: sent once, wiped, never logged.
//   3. WebSocket <scheme>://<host>[:port]/ws?gameId=<gameId>&clientKey=<clientId>   (:222369-222371)
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
//   KRUNKNATIVE_KRUNKER_TRACE=1 / ?krunkerTrace=1              log every frame in and out as JSON
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
//   - cosmetics, skins, accounts: everyone is shown with default cosmetics, we join as a guest
//     (no FRVR access token: `at=` and accessToken omitted)
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
// Find New Game for an official game (F4 / Quick Match): use_list = 1 first asks <matchmaker>/game-list
// and joins the busiest public game with room in the same region (region NULL: the one the player is
// in) for the saved seek mode / map that is not one of the recent games (recent_lobbies.h). Without
// such a game (or a list) it seeks the way krunker.io's client does (backToOrigin's ?exclude=):
// seek-game with excludeGame = the game the player is in (or was in last) and dataQuery dsmo / dsma.
// krunker.io takes ONE excludeGame, so an answer that is another recent game is sought again excluding
// it (twice at most) and the least recent answer is joined when only recent games come back; without
// the list a bounce between two games can then remain (the matchmaker's choice). 1 = started.
int krunker_net_seek_new(Client *client, const char *region, int use_list);
// 1 = the official game this client is in (or joined last), 2.. older ones; 0 = not one of the last
// RECENT_LOBBIES_MAX (recent_lobbies.h). krunker_lobbies_pick picks those last.
int krunker_net_recent_rank(const char *game_id);
const char *krunker_net_game(void); // the official game being joined / played, "" when none
// Leave the official game (no-op when not in one). reason NULL: an intended switch (another
// session follows); else the error screen shows it.
void krunker_net_leave(Client *client, const char *reason);
int krunker_net_active(void);      // the mode owns the connection (seeking, connecting, open or dropped)
int krunker_net_open(void);        // the WebSocket is open
// client_net_send_packet while active: our packet translated and sent (or dropped). 1 = sent.
int krunker_net_send_packet(NetPacketType type, const void *payload, uint16_t length);
void krunker_net_reconnect(Client *client); // the page's reconnect: seek the same game again
void krunker_net_tick(Client *client);      // main thread, once per frame: notices, captcha
