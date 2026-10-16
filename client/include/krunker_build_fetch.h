#pragma once
#include <stddef.h>
// The Krunker build an official join speaks (KN_KRUNKER_OFFICIAL, shared/include/krunker_build_live.h),
// fetched live so a new Krunker build needs no client rebuild.
//
// The "Krunker build" workflow (.github/workflows/krunker-build.yml) runs tools/krunker/extract_build.py
// against Krunker's live bundle (a static parse) every 30 minutes and publishes, when the values change:
//   https://dl.krunk.io/updates/client/krunker-build.json   the manifest (no-cache): build_version,
//                                                            client_version, sha256, path
//   https://dl.krunk.io/updates/krunker/<sha256>.json       the document (immutable)
// The client asks for the manifest when an official join starts (at most once per
// KRUNKER_BUILD_FETCH_TTL_S unless forced), downloads the document only when its cached copy is not that
// sha256, checks the hash, the versions and the shape (krunker_build_accept), caches it and makes it
// the current build. Outcomes:
//   live      the published build (fetched, or the cached copy of the same sha256);
//   cached    the manifest could not be fetched or read: the last published build this client accepted;
//   compiled  nothing fetched and nothing cached (or the fetch is off): krunker_build.h's values;
//   refused   the published build is one this client cannot follow (a removed packet or field, a new
//             cheat formula, ...): the join fails saying why, never a silent desync.
// The matchmaker is the judge of which build is current: krunker_net.c retries a seek refused for its
// version (InvalidVersion / NoServersMatchQuery) once with a forced refresh, then with the compiled build.
//
// Where: KRUNKNATIVE_KRUNKER_BUILD_URL (web: ?krunkerBuild=) names the manifest, "off" / "0" turns the
// fetch off (the compiled build). Unset: the dl.krunk.io manifest when the matchmaker is krunker.io's
// own, none for any other matchmaker (a stub or a test speaks the compiled build). Cache (native):
// KRUNKNATIVE_KRUNKER_BUILD_CACHE, else $XDG_CACHE_HOME/krunknative/krunker-live/krunker-build.json
// (~/.cache/..., %LOCALAPPDATA%\krunknative\...); the web keeps it in memory.
//
// Main thread only. Every adopted build stays allocated while the process runs (the socket thread may
// read the current one; there is one per Krunker build seen, a few KB each).

#define KRUNKER_BUILD_DEFAULT_URL "https://dl.krunk.io/updates/client/krunker-build.json"
#define KRUNKER_BUILD_FETCH_TTL_S 600

typedef enum {
    KRUNKER_BUILD_SRC_COMPILED = 0,
    KRUNKER_BUILD_SRC_LIVE,
    KRUNKER_BUILD_SRC_CACHED,
    KRUNKER_BUILD_SRC_REFUSED,
} KrunkerBuildSource;

// client_version: the published one (refused: the build that was refused; else the current one's).
// why: the refusal's reason, or what the fallback fell back from; "" otherwise.
typedef void (*KrunkerBuildReady)(void *user, KrunkerBuildSource src, const char *client_version, const char *why);
// The HTTP GET the fetch uses (krunker_net.c hands its own: the client's HTTP worker, its proxy).
typedef void (*KrunkerBuildHttpDone)(void *ctx, int status, const char *body, size_t length);
typedef int (*KrunkerBuildHttpGet)(const char *url, KrunkerBuildHttpDone done, void *ctx);

// The manifest URL for a join (above): NULL = no fetch. production: no matchmaker override; setting:
// KRUNKNATIVE_KRUNKER_BUILD_URL's / ?krunkerBuild's value (NULL / "" unset).
const char *krunker_build_fetch_url(int production, const char *setting);
void krunker_build_fetch_set_http(KrunkerBuildHttpGet get);
// Starts the refresh for url (NULL: the compiled build at once). done runs once, on the main thread,
// possibly before this returns (the TTL memo, no url); a newer refresh drops an older one's done.
// force: ask the manifest even within the TTL. Returns 0 when nothing could be started (done then
// already ran with the fallback).
int krunker_build_refresh(const char *url, int force, KrunkerBuildReady done, void *user);
// The outcome's name ("live", "cached", "compiled", "refused") for logs.
const char *krunker_build_source_name(KrunkerBuildSource src);
