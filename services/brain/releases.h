/*
 * services/brain/releases.h - latest game server release (GitHub Releases poll) and the
 * outdated flags on the server lists (release_info.h has the rule, doc/backend.md
 * "Build version").
 *
 * Config:
 *   servers.latest_poll_s    poll interval (600; 0 = never poll)
 *   servers.releases_url     https://api.github.com/repos/rebel10n/KrunkNative/releases?per_page=30
 *   servers.latest_version   describe or commit of the current build: wins over the poll (no poll)
 *   servers.download_url     release page shown to players (default: the poll's html_url,
 *                            else RELEASE_PAGE_DEFAULT)
 *   servers.outdated_grace_h a server is outdated only this long after the latest commit (48)
 */
#ifndef KN_BRAIN_RELEASES_H
#define KN_BRAIN_RELEASES_H

#include "brain.h"
#include "release_info.h"

struct MmLobby;

int releases_init(Brain *b);
void releases_shutdown(Brain *b);

/* The latest release (known = 0 until a poll answered or without config). */
const ReleaseInfo *releases_latest(void);

/* finder GET /servers: per entry outdated/incompatible (+ "protocol" kept), top level
 * latest / download_url / downloads. client_major < 0 = unknown client. */
void releases_annotate_finder(cJSON *reply, cJSON *servers, int client_major);
/* /mm/game-list: meta.outdated / meta.incompatible per tuple + the top-level fields. */
void releases_annotate_tuples(cJSON *reply, cJSON *tuples, int client_major);
/* registry_pick callback (MmPickQuery.outdated). */
int releases_lobby_outdated(const struct MmLobby *l, void *ud);
/* The client's protocol major from a request's ?proto=M.m (-1 when absent). */
int releases_client_major(const char *proto);

#endif
