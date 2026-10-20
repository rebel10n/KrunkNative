#pragma once
// Official krunker.io lobbies in the native client's server browser and seeks
// (doc/v10/matchmaker-token.md "Official lobbies"). Native only: the web build has none of it
// (every function is a no-op / empty there).
//
// The list is the public GET Krunker's own client makes, `<matchmaker>/game-list?hostname=krunker.io`
// (v10.js:107131 / 147575-147581), no login, no token, no Krunker credentials; answers are cached
// for KRUNKER_LOBBIES_CACHE_MS (10 s, v10's serverBrowserRate) and the request times out after KRUNKER_LOBBIES_TIMEOUT_MS. Each
// game `[id, region, players, max, data{i map, g mode, c custom, oc, ds, pw, kr, h}, timeLeft]` becomes
// a server-browser tuple `[id, region, players, max, meta]` with meta.krunker = true (and i, h,
// mode, cs, oc, ds, pw, kr, timeLeft). Official lobbies speak Krunker's protocol: joining one joins it
// in this client (KN_KRUNKER_OFFICIAL, krunker_net.h), or, with the KRUNKNATIVE_KRUNKER_JOIN=browser
// opt-out (and on Windows, which has no native WebSocket client for it yet), opens
// https://krunker.io/?game=<id> in the system browser.
//
// The Servers window lists them on every native client in their own KRUNKER.IO section after
// ours, whatever Settings > Quickplay > Server Type says (that one picks what Quick Match / F4 seek
// into; the menu's Popular Now never counts these games, menu_sources.h). krunker.io refuses
// some networks outright (a 403 "ACCESS DENIED" HTML page):
// the section then says so and points at Settings > Client > Proxy (http_proxy.h), which by
// default carries krunker.io's requests only.
//
// KRUNKNATIVE_KRUNKER_MATCHMAKER=<url> points it elsewhere (tests: a local stub; development:
// a mirror), "off" disables it; KRUNKNATIVE_KRUNKER_URL is the site the join link opens. Headless
// runs and --render-scenario never reach krunker.io unless the matchmaker URL is given explicitly.
#include <client.h>
#include <cJSON.h>
#include <stddef.h>

#define KRUNKER_LOBBIES_CACHE_MS 10000.0 // v10 server browser (serverBrowserRate): a list older than this is asked again on open
#define KRUNKER_LOBBIES_TIMEOUT_MS 6000

int krunker_lobbies_enabled(void);              // 0 on the web, with the matchmaker "off", headless without one
// Fetches the list when the cached one is older than the cache time (force: also within it,
// still never more often than every 15 s). The callback-free answer re-renders the Servers
// window (menu_rerender_if_open(2)) and runs a pending seek (krunker_lobbies_seek).
void krunker_lobbies_refresh(int force);
const cJSON *krunker_lobbies_list(void);        // tuples in krunker.io's order (v10's serverListData); NULL = none yet
// When krunker.io's list last answered (glfwGetTime * 1000, the Servers window's clock): the base of its rows'
// "Ends in" (v10's startTime for the list). -1e18 = never answered.
double krunker_lobbies_started_ms(void);
// The tuple of lobby <id> in the current list ([id, region, players, max, meta, timeLeft]); NULL = not listed
const cJSON *krunker_lobbies_find(const char *id);
int krunker_lobbies_loading(void);
const char *krunker_lobbies_error(void);        // "" = the last request answered; else a sentence for the player
// The proxy setting changed (ui_client_settings.c): the next Servers window asks again at once.
void krunker_lobbies_network_changed(void);
// krunker.io's region ping (v10 pingData, ms; -1 = the ping failed, REGION_PING_TLS_FAILED = its TLS handshake did)
// from the last list load: the row's icon.
// 1 = measured (*ms written), 0 = not measured yet.
int krunker_lobbies_region_ping_ms(const char *region, double *ms);
double krunker_lobbies_players(void);           // totalPlayerCount of the last answer
// Region code -> Krunker's place name (v10 _7517: "us-nj" -> "New York"); the code when unknown.
const char *krunker_region_name(const char *code);

// The region official seeks look in, v10's seek: KRUNKNATIVE_KRUNKER_REGION (v10's __REGION__), else
// Settings > Quickplay > krunker.io Region, else the region the ping race picked (v10 getRegion,
// region_ping.h: krunker.io's /ping-list, every available, non-dedicated-only region's /ping at once,
// the first to answer; cached like v10's setDefaultRegion), else, for a minute after every ping
// failed, the list's first region. "" = not known yet (krunker_seek_region_resolve runs the race).
const char *krunker_seek_region(void);
const char *krunker_seek_region_auto(void); // the ping race's cached region, "" = none (the Region row shows it)
typedef void (*KrunkerRegionFn)(void *user, const char *region);
// fn(user, region) now when krunker_seek_region() is known, else once the race ended (the web: at once, "").
void krunker_seek_region_resolve(KrunkerRegionFn fn, void *user);

// The best official lobby for the seek preferences (seek_prefs.h): public (not custom, no
// password), not full, the wanted mode and map, in the seek's region (krunker_seek_region; any region
// only when it has none), the most players; one this client was not in lately
// first (krunker_net_recent_rank), else the least recent of those, the one it is in last. 1 = found
// (id written).
int krunker_lobbies_pick(char *id, size_t n);
// Seek an official lobby: pick one now (a fresh enough list), or once the list arrives, and
// open it (krunker_lobbies_open); none matching = a toast, only the one the player is in = a toast
// (it stays). No list at all (krunker.io refused it) and joining in-client: the matchmaker picks
// (krunker_net_seek_new). 1 = started, 0 = not available.
int krunker_lobbies_seek(void);
// Find Game's Raids / Zombies / Trade Plaza (openServerWindow 2-4): joins the official custom game of that
// mode (v10 mode id) with room, the krunker.io Region first: v10's popups (the Searching ring while the
// list is fetched, "Took to long to find" 2 s after a join, "Couldnt find any" when none is listed).
void krunker_lobbies_find_custom(int mode);
// Join official lobby <id> in this client (krunker_net_join; the Servers window closes, failures are
// explained by krunker_net), or, when krunker_net_in_client() says no (the browser opt-out, Windows),
// open https://krunker.io/?game=<id> in the system browser and say why (toast).
void krunker_lobbies_open(const char *id);
// A fixed list instead of krunker.io's (--render-scenario `state.krunkerLobbies`, tests): `answer` is a
// /game-list body ({games: [...], totalPlayerCount}) or {error: "<sentence>"}; never fetched while set.
// NULL goes back to krunker.io's. No-op on the web.
void krunker_lobbies_set_fixed(const cJSON *answer);
void krunker_lobbies_attach(Client *client); // startup: the client an in-client join drives
