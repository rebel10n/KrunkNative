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
// Join flow (game.deob.js; matchmaker client at :147380-147690):
//   1. GET <matchmaker>/generate-token                  -> an opaque validation token (text)
//   2. GET <matchmaker>/seek-game?hostname=krunker.io&region=<r>&autoChangeGame=false
//          &validationToken=<token>[&game=<gameId>]&dataQuery={"v":"<buildVersion>"}
//      (matchmakerSeek's parameter order; accessToken / captchaToken are omitted: guest, no captcha)
//      -> {host, port, gameId, clientId, data}
//   3. WebSocket <scheme>://<host>[:port]/ws?gameId=<gameId>&clientKey=<clientId>   (:222369-222371)
//      production (the default matchmaker): wss:// and no port; a matchmaker given explicitly
//      (tests, mirrors): its own scheme (http -> ws, https -> wss) and the port seek-game returned
//   4. io-init, load / init (map + mode), our "en" when the player presses Play, then the game.
// Krunker's own client only fetches the token after a CaptchaRequired answer (seek() retries with
// captchaToken + validationToken); this one always sends a fresh token on the first seek, as the
// brief asks. Two captchas exist: the matchmaker's CaptchaRequired is Cloudflare Turnstile (needs a
// browser: reported, not solved; web: TODO show the widget, native: krunker.io opens in the system
// browser); the socket's "cpt" is an ALTCHA proof of work, answered in C (krunker_altcha.h).
//
// Transport: the browser WebSocket on the web (emscripten/websocket.h, like net_web.c, which stays
// as it is for our protocol); natively a small RFC 6455 client on http_conn.c (TLS, the client
// proxy setting / *_PROXY environment, as the official lobby list uses: krunker.io refuses some
// networks) on its own thread. Windows has no native path yet (http_conn.c is POSIX).
//
// Switches:
//   KRUNKNATIVE_KRUNKER_JOIN=client (native) / ?krunkerJoin=client (web)   official lobbies of the
//       server browser join in-client instead of opening krunker.io in the system browser (default
//       off until it works end to end against the live servers)
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
//   - CHEAT_CHECK natively (QuickJS, the CHEAT_CHECK agent's), Turnstile captchas, Windows
//   - the "cpt" answer's payload shape is ALTCHA's widget format, not readable in the bundle: unverified
//     against a live server
//   - a dropped official session does not reconnect by itself (the page's reconnect seeks anew)
#include <client.h>

int krunker_net_in_client(void);   // official lobbies join in-client (the switch above)
void krunker_net_attach(Client *client); // startup (backend_startup.c): the client the mode drives
// Seek and join an official game (game_id NULL / "": the matchmaker picks one in the region;
// region NULL: the default). Stops the knet transport / the offline game first. 1 = started.
int krunker_net_join(Client *client, const char *game_id, const char *region);
// Leave the official game (no-op when not in one). reason NULL: an intended switch (another
// session follows); else the error screen shows it.
void krunker_net_leave(Client *client, const char *reason);
int krunker_net_active(void);      // the mode owns the connection (seeking, connecting, open or dropped)
int krunker_net_open(void);        // the WebSocket is open
// client_net_send_packet while active: our packet translated and sent (or dropped). 1 = sent.
int krunker_net_send_packet(NetPacketType type, const void *payload, uint16_t length);
void krunker_net_reconnect(Client *client); // the page's reconnect: seek the same game again
void krunker_net_tick(Client *client);      // main thread, once per frame: notices, captcha
