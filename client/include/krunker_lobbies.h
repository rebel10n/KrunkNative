#pragma once
// Official krunker.io lobbies in the native client's server browser and seeks
// (doc/v10/matchmaker-token.md "Official lobbies"). Native only: the web build has none of it
// (every function is a no-op / empty there).
//
// The list is the public GET Krunker's own client makes, `<matchmaker>/game-list?hostname=krunker.io`
// (v10.js:107131 / 147575-147581), no login, no token, no Krunker credentials; answers are cached
// for KRUNKER_LOBBIES_CACHE_MS and the request times out after KRUNKER_LOBBIES_TIMEOUT_MS. Each
// game `[id, region, players, max, data{i map, g mode, c custom, oc, ds, pw, kr, h}, timeLeft]` becomes
// a server-browser tuple `[id, region, players, max, meta]` with meta.krunker = true (and i, h,
// mode, cs, oc, ds, pw, kr, timeLeft). Official lobbies speak Krunker's protocol, which this
// client does not: joining one opens https://krunker.io/?game=<id> in the system browser.
//
// The Servers window lists them on every native client in their own KRUNKER.IO section after
// ours, whatever Settings > Quickplay > Server Type says (that one only picks what Quick Match /
// F4 seek into). krunker.io refuses some networks outright (a 403 "ACCESS DENIED" HTML page):
// the section then says so and points at Settings > Client > Proxy (http_proxy.h), which by
// default carries krunker.io's requests only.
//
// KRUNKNATIVE_KRUNKER_MATCHMAKER=<url> points it elsewhere (tests: a local stub; development:
// a mirror), "off" disables it; KRUNKNATIVE_KRUNKER_URL is the site the join link opens. Headless
// runs and --render-scenario never reach krunker.io unless the matchmaker URL is given explicitly.
#include <cJSON.h>
#include <stddef.h>

#define KRUNKER_LOBBIES_CACHE_MS 20000.0 // >= 15 s between list requests
#define KRUNKER_LOBBIES_TIMEOUT_MS 6000

int krunker_lobbies_enabled(void);              // 0 on the web, with the matchmaker "off", headless without one
// Fetches the list when the cached one is older than the cache time (force: also within it,
// still never more often than every 15 s). The callback-free answer re-renders the Servers
// window (menu_rerender_if_open(2)) and runs a pending seek (krunker_lobbies_seek).
void krunker_lobbies_refresh(int force);
const cJSON *krunker_lobbies_list(void);        // tuples, most players first; NULL = none yet
int krunker_lobbies_loading(void);
const char *krunker_lobbies_error(void);        // "" = the last request answered; else a sentence for the player
// The proxy setting changed (ui_client_settings.c): the next Servers window asks again at once.
void krunker_lobbies_network_changed(void);
double krunker_lobbies_players(void);           // totalPlayerCount of the last answer
// Region code -> Krunker's place name (v10 _7517: "us-nj" -> "New York"); the code when unknown.
const char *krunker_region_name(const char *code);

// The best official lobby for the seek preferences (seek_prefs.h): public (not custom, no
// password), not full, the wanted mode and map, the most players. 1 = found (id written).
int krunker_lobbies_pick(char *id, size_t n);
// Seek an official lobby: pick one now (a fresh enough list), or once the list arrives, and
// open it (krunker_lobbies_open); none matching = a toast. 1 = started, 0 = not available.
int krunker_lobbies_seek(void);
// Open https://krunker.io/?game=<id> in the system browser and tell the player why (toast).
void krunker_lobbies_open(const char *id);
