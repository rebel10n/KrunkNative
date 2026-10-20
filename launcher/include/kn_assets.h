#pragma once
// Game content sync (launcher/src/assets_sync.c): brings an assets directory to the asset list a
// channel manifest names, downloading only the files whose content differs (by sha256) from the
// content-addressed objects, verifying each, and recording the completed list in
// <assets>/.kn-assets.json. Files the user added (not in the last list) are never touched.
//
// Downloads (doc/backend.md "Client updates"): every unique sha256 once (paths with the same
// content are copied), largest first, from all origins at once: the manifest's objects plus its
// assets.mirrors (the Pages CDN: two projects, each with the only/skip path filter of its part of
// the tree, deploy/asset-shards.json), each with its own workers pulling from one queue (per group
// of objects the same origins have), so the faster origin serves more. Small files go HTTP/1.1-pipelined over
// kept-alive connections; a file that fails on one origin is retried on the others, then again
// with backoff. Partial downloads stay in <assets>/.kn-sync/<sha256>.part and are resumed
// (Range) by the next attempt or run; every file is verified (size + sha256) before it is
// renamed into place. No fsync: an interrupted sync is redone from the parts and the hashes.
#include <kn_update.h>
#include <stdint.h>

typedef struct {
    // From worker threads, serialised by the sync: phase "list" / "check" / "download" / "done".
    void (*progress)(void *user, const char *phase, int64_t done, int64_t total, size_t files_done, size_t files_total);
    void *user;
    volatile int *cancel; // non-zero stops the sync (set it with __atomic_store_n from another thread)
    int threads;      // connections per origin (default KN_ASSETS_THREADS_DEFAULT, at most 64)
    const char *mirrors; // optional: "off" = only the manifest's objects; else space-separated
                         // origin specs ("[only=P+P@|skip=P+P@]URL", kn_origin_spec_parse)
                         // replacing the manifest's / built-in mirrors
    // optional (assets_bench): only the entries this returns 1 for are part of the sync
    int (*select)(void *user, const KnAssetEntry *e);
    // 1: the first run's download (the launcher's default). Only the menu set (assets_sync.c
    // MENU_SET) is downloaded now; the rest of the list is recorded in the state, so the
    // client's on-demand fetch (client/include/asset_fetch.h) gets each file when it is first used.
    // A copy of a deferred file that is present (stale or partial) is removed, so that fetch
    // replaces it, verified. 0 = the whole list (--full-assets: offline play, CI).
    int menu_only;
} KnAssetsSyncOpts;

// The client's content-addressed cache in the assets dir: <assets>/.kn-objects/<aa>/<sha256>, the
// verified downloads of the list's files, hard-linked to their paths (client/src/net/asset_fetch.c
// writes and reads it; the sync removes the objects no entry of the list names).
#define KN_ASSETS_OBJECTS ".kn-objects"
#define KN_ASSETS_THREADS_DEFAULT 16
#define KN_ASSETS_MAX_ORIGINS 5 // the objects + KN_UPDATE_MAX_MIRRORS
// The built-in mirror of the default origin (deploy/pages/assets_cdn.py): used when a manifest
// from https://dl.krunk.io/ names no assets.mirrors (kn-updates.py publishes them). The Pages CDN's
// default project: it 301s the other project's directories (a pipelined batch does not follow
// that, the object's retry does, see assets_sync.c).
#define KN_ASSETS_DEFAULT_ORIGIN "https://dl.krunk.io/"
#define KN_ASSETS_DEFAULT_MIRROR "https://assets.krunk.io/assets/"

typedef struct {
    char url[512];
    size_t files;     // unique objects it served
    int64_t bytes;
    size_t failures;  // per-file failures (404, mismatch, transport)
    int disabled;     // stopped after repeated transport failures
} KnAssetsOriginStat;

typedef struct {
    size_t fetched, removed, kept, adopted, hashed;
    int64_t bytes;           // downloaded this run (resumed part bytes not included)
    size_t unique, copied;   // objects downloaded, paths filled by copying a duplicate
    int64_t resumed;         // bytes of earlier partial downloads reused
    size_t retries;
    size_t deferred;         // menu_only: list entries left to the client's on-demand fetch
    int64_t deferred_bytes;
    KnAssetsOriginStat origins[KN_ASSETS_MAX_ORIGINS];
    int origin_count;
} KnAssetsSyncResult;

// The version of the last completed sync ("" = none). 1 when a state file was read.
int kn_assets_local_version(const char *assets_dir, char *out, size_t n);
// 1 on success (also when nothing had to change).
int kn_assets_sync(const char *assets_dir, const KnUpdateManifest *m, const KnAssetsSyncOpts *opts,
                   KnAssetsSyncResult *result, char *err, size_t errn);
