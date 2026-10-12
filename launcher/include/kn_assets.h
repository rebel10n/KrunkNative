#pragma once
// Game content sync (launcher/src/assets_sync.c): brings an assets directory to the asset list a
// channel manifest names, downloading only the files whose content differs (by sha256) from the
// content-addressed objects, verifying each, and recording the completed list in
// <assets>/.kn-assets.json. Files the user added (not in the last list) are never touched.
#include <kn_update.h>
#include <stdint.h>

typedef struct {
    // From worker threads, serialised by the sync: phase "list" / "check" / "download" / "done".
    void (*progress)(void *user, const char *phase, int64_t done, int64_t total, size_t files_done, size_t files_total);
    void *user;
    volatile int *cancel;
    int threads;      // parallel downloads (default 4)
} KnAssetsSyncOpts;

typedef struct {
    size_t fetched, removed, kept, adopted, hashed;
    int64_t bytes;
} KnAssetsSyncResult;

// The version of the last completed sync ("" = none). 1 when a state file was read.
int kn_assets_local_version(const char *assets_dir, char *out, size_t n);
// 1 on success (also when nothing had to change).
int kn_assets_sync(const char *assets_dir, const KnUpdateManifest *m, const KnAssetsSyncOpts *opts,
                   KnAssetsSyncResult *result, char *err, size_t errn);
