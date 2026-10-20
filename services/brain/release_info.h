/*
 * services/brain/release_info.h - the latest published game server build and the "outdated"
 * rule (doc/backend.md "Build version"). Pure: no I/O, no clocks of its own, unit tested by
 * tests/test_releases.c. releases.c polls the GitHub Releases API and feeds it.
 *
 * Release metadata: .github/workflows/release.yml publishes `server-latest` (rolling prerelease)
 * and `server-v*` releases whose body carries one line
 *   <!-- kn-build {"describe":"...","commit":"<40 hex>","commit_time":<unix s>,
 *                  "build_time":"<ISO-8601>","protocol":"M.m"} -->
 * and the assets krunknative-server-{linux-x86_64.tar.gz, linux-x86_64-bullseye.tar.gz,
 * windows-x86_64.zip, macos-arm64.tar.gz}.
 *
 * Outdated rule (release_server_outdated):
 *   (a) the server's protocol major.minor is lower than this coordinator's own
 *       (NET_PROTOCOL_MAJOR.MINOR), or
 *   (b) the latest release is known (commit_time > 0), the server's commit is not the latest
 *       commit, and the server's commit_time is older than latest.commit_time - grace
 *       (servers.outdated_grace_h, default 48 h; dirty builds compare like any other), or
 *   (c) the latest release is known and the server reports no build info (a server_bin
 *       from before the build fields), or
 *   (d) servers.latest_version is configured (no commit time): the server's describe and
 *       commit both differ from it.
 * Incompatible (release_protocol_incompatible): the server announces a protocol MAJOR other
 * than the client's. Minor versions interoperate (peers send newer packets only to peers that
 * announced them, shared.h NET_PROTOCOL_MINOR_*). Servers with no protocol field are 1.x.
 */
#ifndef KN_BRAIN_RELEASE_INFO_H
#define KN_BRAIN_RELEASE_INFO_H

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

#define RELEASE_PAGE_DEFAULT "https://github.com/rebel10n/KrunkNative/releases/tag/server-latest"

typedef struct ReleaseInfo {
    int known;                     /* a release (or the config override) is set */
    int from_config;               /* servers.latest_version: compare by describe/commit only */
    char tag[64];                  /* "server-latest", "server-v1.2.0" */
    char describe[64];
    char commit[48];               /* hex, 40 from CI (12 from a server) */
    int64_t commit_time;           /* unix s, 0 unknown */
    char build_time[32];
    int proto_major, proto_minor;  /* -1 unknown */
    char html_url[256];            /* release page */
    char dl_linux[320], dl_linux_bullseye[320], dl_windows[320], dl_macos[320];
} ReleaseInfo;

typedef struct ServerBuild {
    const char *describe;          /* "" / NULL = not reported */
    const char *commit;
    int64_t commit_time;
    int proto_major, proto_minor;  /* -1 unknown */
} ServerBuild;

/* "1.3" -> 1, 3. Returns 1 when parsed (major >= 0, minor >= 0). */
int release_parse_protocol(const char *s, int *major, int *minor);
/* The kn-build line of a release body into out (keeps other fields). 1 when found. */
int release_parse_body(const char *body, ReleaseInfo *out);
/* One release object of the GitHub API: tag, html_url, body metadata, asset URLs. 1 = a
 * server release (tag server-latest or server-v*, not a draft) with kn-build metadata. */
int release_parse_release(const cJSON *rel, ReleaseInfo *out);
/* The newest server release (highest commit_time; server-v* wins a tie) of a GitHub
 * /releases array. 1 when one was found. */
int release_pick_latest(const cJSON *releases, ReleaseInfo *out);
/* servers.latest_version / servers.download_url overrides (NULL/"" = none). */
void release_apply_overrides(ReleaseInfo *r, const char *latest_version, const char *download_url);

/* 1 when the commits name the same commit (hex prefix of at least 7 digits). */
int release_same_commit(const char *a, const char *b);
int release_server_outdated(const ReleaseInfo *latest, const ServerBuild *s, int64_t grace_s, int my_major,
                            int my_minor);
int release_protocol_incompatible(int client_major, int server_major);

/* Build fields of a finder /servers entry (version, commit, commit_time, protocol) or of a
 * lobby tuple meta (build, commit, commitTime, proto). Strings borrowed from the object. */
void release_server_build_from_finder(const cJSON *entry, ServerBuild *out);
void release_server_build_from_meta(const cJSON *meta, ServerBuild *out);

/* Adds "latest" {version, commit, commit_time, build_time, protocol, tag}, "download_url"
 * (release page) and "downloads" {linux, linux_bullseye, windows, macos} to o. */
void release_add_json(const ReleaseInfo *r, cJSON *o);

#endif
