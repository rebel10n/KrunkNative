#pragma once
// Native on-demand game content fetch (client/src/net/asset_fetch.c). The web build fetches every
// asset on demand (ensure_asset_file + emscripten_wget_data); natively the launcher syncs the
// content dir up front, so a file its sync skipped, a user deleted or a truncated copy used to stay
// broken forever. Here a managed file that is missing (or whose size differs from the content
// manifest) is downloaded in the background: ensure_asset_file() / asset_prefetch() ask through
// asset_set_fetch_hook(), workers download it from the asset origins (the launcher's order), verify
// it (size + sha256 from the manifest), write it atomically into the assets dir, and the render
// thread's asset_fetch_poll() hands the arrived paths to the loaders (client/src/asset_fetch_client.c:
// textures re-uploaded, OBJ models rebuilt in place, GLTF models parsed again on next use, sounds
// decoded again on next play, page images probed again).
//
// Managed files: with a content manifest (<assets>/.kn-assets.json, written by the launcher's sync,
// launcher/include/kn_assets.h) exactly the paths it lists; without one (a dev tree, a test fixture)
// the fetch is off unless forced (require_manifest 0), then every safe path below the dir.
// Never writes inside a git checkout (kn_install_dir_in_checkout, per target directory, symlinks
// resolved) unless allow_checkout. Plus, with user_assets: Krunker user assets outside the content
// set (community maps' textures / models / sounds / resource packs), from their own origins, also
// in a dev tree (user-assets/ is git-ignored).
//
// Origins (by path: <url><rel>; "objects:<url>" = content-addressed <url><aa>/<sha256>, manifest
// files only; "only=P+P@<url>" / "skip=P+P@<url>" = by path, only the paths below the prefixes P /
// all others: one Pages project of the split CDN, deploy/asset-shards.json, as the launcher hands
// them down), tried in order per file, skipping origins whose filter does not have the path;
// a 404 / checksum mismatch moves on to the next one, a
// transport failure also takes the origin out for a while. A file every origin answered with 404
// or wrong content is given up for the session; transport failures are retried with backoff up to
// ASSET_FETCH_ATTEMPTS times (user assets: ASSET_FETCH_USER_ASSET_ATTEMPTS, waits capped at
// ASSET_FETCH_ORIGIN_DOWN_MS). One log line per path: when it arrived or when it was given up.
//
// Content-addressed cache: every verified download of a manifest file is also hard-linked to
// <assets>/.kn-objects/<aa>/<sha256> (KN_ASSETS_OBJECTS, launcher/include/kn_assets.h). A file that
// is missing when it is needed (deleted, removed by a sync, or the same bytes as another path) is
// linked back from there with no request. Each object is hashed before every use; one that does
// not match is removed and the file is downloaded again. Filesystems without hard links cache nothing.
#include <stddef.h>
#include <stdint.h>

// Only the pointer is used here, so the web build needs no launcher/kn_http.h (native-only transport).
typedef struct KnHttpSink KnHttpSink;

#define ASSET_FETCH_WORKERS_DEFAULT 2  // downloads in flight
#define ASSET_FETCH_WORKERS_MAX 4
#define ASSET_FETCH_ATTEMPTS 4         // rounds over the origins before a file is given up
// the same for a Krunker user asset (a map's model: its walls): ~8 min of outage, the waits capped
#define ASSET_FETCH_USER_ASSET_ATTEMPTS 16
#define ASSET_FETCH_BACKOFF_MS 1000    // before round r: ASSET_FETCH_BACKOFF_MS << (r - 1)
#define ASSET_FETCH_ORIGIN_DOWN_MS 30000 // at most this long out after transport failures
#define ASSET_FETCH_ORIGIN_REFUSED_MS 300000 // out this long after a 401 / 403 / 429
#define ASSET_FETCH_MAX_ORIGINS 8
#define ASSET_FETCH_MAX_ENTRIES 8192   // distinct paths per session
#define ASSET_FETCH_MAX_BYTES ((int64_t) 512 << 20) // a file without a manifest size
// Without the launcher's list (KRUNKNATIVE_ASSET_ORIGINS): the Pages CDN's default project (it
// 301s the other project's directories, kn_http follows that), then kru's whole tree
// (launcher/include/kn_assets.h KN_ASSETS_DEFAULT_MIRROR, _ORIGIN; the web's kn-config.json).
#define ASSET_FETCH_DEFAULT_ORIGINS "https://assets.krunk.io/assets/ https://dl.krunk.io/assets/"
// Krunker user assets (shared/include/user_asset_path.h: user-assets/<id>/<file> a community map
// names, outside the content set): Krunker's user-asset host (what Krunker's own client loads),
// then the brain's mirror of it (services/brain/user_assets.c: it answers networks Cloudflare
// turns away from krunker.io, and caches what it fetched). <url><id>/<file>.
#define ASSET_FETCH_USER_ASSET_ORIGINS "https://user-assets.krunker.io/ https://coordinator.krunk.io/api/user-assets/"

// The transport (tests): kn_http_get's contract. NULL = kn_http (POSIX sockets + OpenSSL, WinHTTP).
typedef int (*AssetFetchGetFn)(void *user, const char *url, KnHttpSink *sink, char *err, size_t errn);

typedef struct {
    const char *assets_dir;  // required ('/' appended when missing)
    const char *origins;     // space/comma separated; NULL or "" = ASSET_FETCH_DEFAULT_ORIGINS
    int workers;             // 0 = ASSET_FETCH_WORKERS_DEFAULT
    int allow_checkout;      // write into a git checkout's files anyway
    int require_manifest;    // off without <assets>/.kn-assets.json
    // 1: Krunker user assets (user_asset_tree_rel_ok) that the content set does not list are fetched
    // from user_asset_origins (NULL or "" = ASSET_FETCH_USER_ASSET_ORIGINS) and written below
    // <assets>/user-assets/; where the content fetch is off (a git checkout, no manifest though one
    // is required) the fetch still runs for them alone. Size-capped (user_asset_max_bytes) and
    // checked (user_asset_content_ok) instead of a manifest's sha256.
    int user_assets;
    const char *user_asset_origins;
    int backoff_ms;          // 0 = ASSET_FETCH_BACKOFF_MS
    int timeout_ms;          // per request, 0 = 20000
    AssetFetchGetFn get;     // tests
    void *get_user;
} AssetFetchConfig;

// Starts the workers. 1 = running; 0 = off (why says so: no manifest, inside a checkout, ...).
int asset_fetch_start(const AssetFetchConfig *cfg, char *why, size_t whyn);
// Stops (aborts transfers in flight, joins the workers) and frees everything. Safe when not running.
void asset_fetch_stop(void);
int asset_fetch_running(void);
// The hook's answer for a full path (assets_dir + rel): 1 = read it (present and not known bad, or
// not a managed file), 0 = queued / downloading / waiting to retry (enqueue 0: only when already
// known), -1 = given up. Thread-safe.
int asset_fetch_request(const char *path, int enqueue);
// Render thread: fn(full path) for every file that arrived since the last call. Returns the count.
int asset_fetch_poll(void (*fn)(void *user, const char *path), void *user);
// Waits until nothing is queued, downloading or waiting to retry (tests). 1 = idle.
int asset_fetch_wait_idle(int timeout_ms);

typedef struct {
    size_t queued;      // paths ever queued
    size_t fetched;     // written into the assets dir (from the cache or an origin)
    size_t cached;      // of those: linked from the content-addressed cache, no request
    size_t failed;      // given up
    size_t refused;     // downloads whose size / sha256 did not match (per origin)
    size_t requests;    // HTTP requests
    size_t pending;     // queued + downloading + waiting now
    int has_manifest;
    int user_assets_only; // the content fetch is off: only user assets are fetched
} AssetFetchStats;
void asset_fetch_stats(AssetFetchStats *out);

// Client glue (client/src/asset_fetch_client.c, native only): reads KRUNKNATIVE_ASSET_FETCH (0 off,
// 1 also without a content manifest), KRUNKNATIVE_ASSET_ORIGINS, KRUNKNATIVE_ASSET_FETCH_CHECKOUT
// (1: allow a git checkout's assets/), KRUNKNATIVE_USER_ASSETS (0: no user-asset fetch),
// KRUNKNATIVE_USER_ASSET_ORIGINS and starts the fetch for assets_path(), installing the hook.
void asset_fetch_client_init(void);
void asset_fetch_client_frame(void);    // once per frame: arrived files reach the loaders
void asset_fetch_client_shutdown(void);
