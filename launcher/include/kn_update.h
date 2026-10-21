#pragma once
// KrunkNative client updates (doc/backend.md "Client updates"): the pure half, shared by the
// launcher (launcher/src), the client's Outdated check (client/src/ui/update_ui.c) and the unit
// test (launcher/tests/update_test.c). No I/O and no clocks.
//
// The updates server (tools/release/kn-updates.py, https://dl.krunk.io/updates/: kru, the host
// nginx role kn_dl_origin; assets.krunk.io/updates/ 302s there for older launchers) serves:
//   client/<channel>.json         the channel manifest (KnUpdateManifest), written last
//   client/builds/<commit12>/...  executable-only packages (exes, libs, ui/, kn-install.json)
//   assets/<version>.json         asset file list: path -> sha256, size (KnAssetList)
//   assets/objects/<aa>/<sha256>  content-addressed asset files
// Relative URLs in a manifest are resolved against the manifest's own URL.
#include <cJSON.h>
#include <stddef.h>
#include <stdint.h>

#define KN_UPDATE_DEFAULT_URL "https://dl.krunk.io/updates/client/latest.json"
#define KN_UPDATE_FORMAT 1
#define KN_UPDATE_GRACE_DEFAULT_S (48 * 3600) // servers.outdated_grace_h (doc/backend.md 8.12)
#define KN_UPDATE_URL_MAX 512
#define KN_INSTALL_MARKER "kn-install.json"
#define KN_ASSETS_STATE ".kn-assets.json"     // the asset list a sync last completed, in the assets dir
#define KN_UPDATE_MAX_MIRRORS 4
#define KN_PATH_FILTER_MAX 1024

// Which paths below assets/ an origin serves (deploy/asset-shards.json: the content is split by
// directory over two Pages projects). mode KN_PATH_ALL: every path; KN_PATH_ONLY: the ones starting
// with a prefix of list; KN_PATH_SKIP: all the others. list: the prefixes joined by '+' (paths have
// no '+', kn_update_path_ok / deploy/pages/asset_shards.py).
enum { KN_PATH_ALL = 0, KN_PATH_ONLY = 1, KN_PATH_SKIP = 2 };
typedef struct {
    int mode;
    char list[KN_PATH_FILTER_MAX];
} KnPathFilter;
// 1 when an origin with filter f serves path (NULL f: every path).
int kn_path_filter_ok(const KnPathFilter *f, const char *path);
// An origin spec of KRUNKNATIVE_ASSET_ORIGINS / --asset-mirrors: "[objects:][only=P+P@|skip=P+P@]URL"
// (URL http(s); objects: = content-addressed <url><aa>/<sha256>, no filter). url gets a trailing '/'.
// 1 on success.
int kn_origin_spec_parse(const char *spec, char *url, size_t urln, int *by_path, KnPathFilter *f);
// The spec of an origin (the launcher hands its sync's origins to the client). 1 when it fits.
int kn_origin_spec_format(char *out, size_t n, const char *url, int by_path, const KnPathFilter *f);

// Build identity: the kn-build metadata (tools/release/version-meta.sh) or kn_build_info().
typedef struct {
    char describe[64];
    char commit[48];          // hex: 40 from CI, 12 from a binary
    int64_t commit_time;      // unix s, 0 unknown
    char build_time[32];
    int proto_major, proto_minor; // -1 unknown
} KnUpdateBuild;

typedef struct {
    char url[KN_UPDATE_URL_MAX];  // absolute (resolved)
    char sha256[65];              // lowercase hex
    int64_t size;
} KnUpdateFile;

typedef struct {
    char kind[16];                // "tar.gz", "zip", "deb", "rpm", "page"
    char url[KN_UPDATE_URL_MAX];
} KnUpdateLink;

#define KN_UPDATE_MAX_LINKS 8
typedef struct {
    char id[48];                  // "linux-x64" (kn_update_platform_default)
    int has_update;
    KnUpdateFile update;          // the executable-only package the launcher installs
    KnUpdateLink links[KN_UPDATE_MAX_LINKS]; // human downloads
    int link_count;
} KnUpdatePlatform;

typedef struct {
    int format;
    char channel[32];
    KnUpdateBuild build;
    int64_t grace_s;
    char page[KN_UPDATE_URL_MAX];     // release page for humans ("" = none)
    KnUpdatePlatform *platforms;
    int platform_count;
    struct {
        char version[48];             // "" = the manifest names no asset set
        KnUpdateFile manifest;        // assets/<version>.json
        char objects[KN_UPDATE_URL_MAX]; // base URL of the objects ("…/assets/objects/")
        int64_t files, bytes;
        // paths below assets/ the packages ship (shaders, maps, ...), sorted: never part of the
        // content set, never removed by a sync (the executable update owns them)
        char **packaged;
        size_t packaged_count;
        // "mirrors": more places with the same content, tried alongside objects (launcher/src/
        // assets_sync.c): strings or {"url","layout","only"|"skip"}; layout "path" (default:
        // <url><path>, a copy of the asset tree such as the Pages CDN) or "objects"
        // (<url><aa>/<sha256>); "only" / "skip": the path prefixes it has / lacks (one Pages
        // project of the split, deploy/asset-shards.json). A filter that does not fit drops the mirror.
        // mirrors_set = the manifest had the key (an empty list turns the built-in default off).
        struct {
            char url[KN_UPDATE_URL_MAX];
            int by_path;
            KnPathFilter filter;
        } mirrors[KN_UPDATE_MAX_MIRRORS];
        int mirror_count, mirrors_set;
    } assets;
} KnUpdateManifest;

// 1 on success. err gets a short reason otherwise. manifest_url resolves relative URLs.
int kn_update_manifest_parse(const char *json, size_t len, const char *manifest_url, KnUpdateManifest *out,
                             char *err, size_t errn);
void kn_update_manifest_free(KnUpdateManifest *m);
const KnUpdatePlatform *kn_update_manifest_platform(const KnUpdateManifest *m, const char *id);
// The human download for a package kind ("deb", "rpm", "tar.gz", "zip"; NULL = the platform's
// archive), falling back to the platform's archive, then the release page. "" when none.
const char *kn_update_download_url(const KnUpdateManifest *m, const char *platform, const char *kind);

// kn-build object {"describe","commit","commit_time","build_time","protocol"}. 1 when it has a commit.
int kn_update_parse_build(const cJSON *o, KnUpdateBuild *out);
int kn_update_parse_protocol(const char *s, int *major, int *minor);
// 1 when both name the same commit (hex prefix of at least 7 digits, case-insensitive).
int kn_update_same_commit(const char *a, const char *b);
// The player-facing label of a build: its commit, 7 hex digits ("unknown" without one). Builds are
// compared by commit (kn_update_compare); a describe string is never a label.
void kn_update_build_label(char *out, size_t n, const KnUpdateBuild *b);

typedef enum {
    KN_UPDATE_CURRENT = 0,   // same commit, unknown latest, or this build is newer
    KN_UPDATE_AVAILABLE = 1, // a newer build exists (within the grace period)
    KN_UPDATE_OUTDATED = 2,  // newer protocol, unknown own build, or older than latest - grace
} KnUpdateState;
// The servers' rule (doc/backend.md 8.12, release_server_outdated) applied to a client.
KnUpdateState kn_update_compare(const KnUpdateBuild *mine, const KnUpdateBuild *latest, int64_t grace_s);
const char *kn_update_state_name(KnUpdateState s); // "current" / "available" / "outdated"

// Resolves ref against base (an absolute http(s) URL). Absolute refs pass through; "/x" is
// origin-relative; "a/b" and "../a" are relative to base's directory. 1 on success.
int kn_update_resolve_url(const char *base, const char *ref, char *out, size_t n);

// A relative path from a manifest or package is safe: non-empty, '/'-separated, no leading
// '/', no "." / ".." / empty components, no '\\' or ':' or control characters, < 400 bytes.
int kn_update_path_ok(const char *rel);

// ---------------------------------------------------------------- asset lists

typedef struct {
    char *path;          // relative to the assets dir
    char sha256[65];
    int64_t size;
} KnAssetEntry;

typedef struct {
    char version[48];
    KnAssetEntry *files; // sorted by path (strcmp), unique
    size_t count;
} KnAssetList;

// {"format":1,"version":"…","files":[["path","sha256",size],…]}. Rejects unsafe paths, bad
// hashes and duplicates. 1 on success.
int kn_asset_list_parse(const char *json, size_t len, KnAssetList *out, char *err, size_t errn);
char *kn_asset_list_json(const KnAssetList *l); // malloc'd, same format
void kn_asset_list_free(KnAssetList *l);
const KnAssetEntry *kn_asset_list_find(const KnAssetList *l, const char *path);
// Sum of sizes.
int64_t kn_asset_list_bytes(const KnAssetList *l);

// Local file probes for the diff: size (-1 = missing) and sha256 (1 = hashed).
typedef int64_t (*KnAssetStatFn)(void *user, const char *path);
typedef int (*KnAssetHashFn)(void *user, const char *path, char out_hex[65]);

typedef struct {
    size_t *fetch;        // indices into want->files to download
    size_t fetch_count;
    int64_t fetch_bytes;
    char **remove;        // managed paths (in have, not in want) to delete
    size_t remove_count;
    size_t kept;          // present and known to match (have says so, size matches)
    size_t adopted;       // present, hashed, matched (unknown to have or changed in have)
    size_t hashed;        // files hashed
} KnAssetDiff;

// 1 when path is one of the manifest's assets.packaged (package-owned files).
int kn_update_is_packaged(const KnUpdateManifest *m, const char *path);

// What a sync must do to go from the local state to want. have = the last completed list
// (KN_ASSETS_STATE; NULL = none). keep (may be NULL) = a sorted list of paths never removed
// (the manifest's assets.packaged). A file counts as present without hashing when have lists it
// with want's hash and its size matches; a file of the right size that have does not vouch
// for is hashed (hash may be NULL: then it is fetched). Only paths have lists are removed:
// files the user added are never touched. 1 on success (0 = out of memory).
int kn_asset_diff(const KnAssetList *want, const KnAssetList *have, char *const *keep, size_t keep_count,
                  KnAssetStatFn stat, KnAssetHashFn hash, void *user, KnAssetDiff *out);
void kn_asset_diff_free(KnAssetDiff *d);

// ---------------------------------------------------------------- install marker

// kn-install.json in the package's runtime/ directory (packages, tools/release): where the launcher
// and its updater find their install. Without it (dev builds) nothing self-updates.
typedef struct {
    int format;
    char platform[48];   // "linux-x64", "windows-x64", "macos-arm64", ... (the BoxOffice-<platform>.<ext> name)
    char package[16];    // "tar.gz", "zip", "deb", "rpm" (old installs)
    char channel[32];    // "latest"
    char root[128];      // the install root relative to the marker's directory (".." = above runtime/)
} KnInstallMarker;

int kn_install_marker_parse(const char *json, size_t len, KnInstallMarker *out);
// deb/rpm installs belong to the package manager: no self-update, show the download instead.
int kn_install_self_updatable(const KnInstallMarker *m);
// Compile-time platform id ("linux-x64", "windows-x64", "macos-arm64", ...);
// -DKRUNKNATIVE_PLATFORM_ID=... overrides it.
const char *kn_update_platform_default(void);
// The package kind a platform installs from: "zip" on windows, else "tar.gz".
const char *kn_update_archive_kind(const char *platform);
