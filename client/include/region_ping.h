#pragma once
// The matchmaker region a seek looks in when the player chose none: Krunker v10.0.2's
// ClientManager.getRegion (v10.js:147469-147507). The matchmaker's ping list
// ({region: "host[:port]"}, GET <matchmaker>/ping-list) names one ping host per region; every
// eligible region's http(s)://<host>/ping is fetched at the same time and the FIRST to answer, the
// lowest latency, is the region (v10 then caches it: setDefaultRegion). A ping that does not answer
// within REGION_PING_TIMEOUT_MS counts as failed (pingFetch's AbortController); every ping failing,
// or nothing to ping, is v10's "All pings failed." / "No pingable regions.": the caller falls back.
// Used by both matchmakers: krunker.io's (krunker_lobbies.h, official seeks) and the brain's
// (servers_ui.c, our own seeks). Both platforms (the web's fetch() runs the pings concurrently).
#include <cJSON.h>
#include <stddef.h>

#define REGION_PING_TIMEOUT_MS 5000 // v10 ClientManager.PING_TIMEOUT_MS
// region_ping_measure's value for a region whose ping host failed the TLS handshake (its certificate or
// name did not verify): a failed ping like -1, told apart so krunker_lobbies.c can leave that region's
// official lobbies out of the list. Any other failure (refused, timed out, no route) stays -1.
#define REGION_PING_TLS_FAILED (-2)

// region = the winner, NULL = every ping failed (or nothing was pingable). Main thread.
typedef void (*RegionPingFn)(void *user, const char *region);
// Whether a listed region may be pinged (v10: in availableRegions, not in dedicatedOnlyRegions);
// NULL = every listed region.
typedef int (*RegionPingEligible)(const char *region);

// Starts the race over ping_list (copied: the caller keeps it). fn runs exactly once, from
// backend_poll, never from inside this call; 0 = nothing could be started (fn is NOT called).
int region_ping_race(const cJSON *ping_list, RegionPingEligible eligible, RegionPingFn fn, void *user);

// The server browser's row pings (v10 getRegionPing, v10.js:147494-147530): every listed region's /ping
// at once, each round trip in ms (-1 when the ping failed, v10's catch; REGION_PING_TLS_FAILED when its TLS
// handshake failed). fn runs once every ping answered
// (from backend_poll, never inside this call) with `pings`, an object {region: ms} the callee owns; 0 =
// nothing could be started (fn is NOT called). Unlike the race, every listed region is pinged (v10 pings
// all of its ping list, dedicated-only regions too).
typedef void (*RegionPingsFn)(void *user, cJSON *pings);
int region_ping_measure(const cJSON *ping_list, RegionPingsFn fn, void *user);

// http(s)://<value>/ping for a ping-list value, v10's rule (getRegion): "ip_*", "localhost*" and
// "127.0.0.1*" keep their port and are http:// unless the port is 443; any other host is
// https://<host>/ping without its port. KrunkNative's brains list "host:6767" for a plain-HTTP
// brain (doc/backend.md /mm/ping-list): kept as http://host:6767/ping like v3.1.1's rule.
void region_ping_url(const char *value, char *out, size_t n);
