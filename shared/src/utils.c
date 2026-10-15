#include <shared.h>
#include <user_asset_path.h> // web: a user asset's mirror URL
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifdef WIN32
#include <direct.h>
__declspec(dllimport) void __stdcall Sleep(unsigned long ms); // asset_prefetch_wait (no windows.h here)
#else
#include <time.h>
#include <unistd.h>
#endif

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <sys/stat.h>
#include <errno.h>
#endif

static void (*g_asset_yield_hook)(const char *path);
static int (*g_asset_fetch_hook)(const char *path, int enqueue);

void asset_set_yield_hook(void (*hook)(const char *path)) {
    g_asset_yield_hook = hook;
}

void asset_set_fetch_hook(int (*hook)(const char *path, int enqueue)) {
    g_asset_fetch_hook = hook;
}

static volatile unsigned g_asset_arrivals;

unsigned asset_arrival_generation(void) {
    return __atomic_load_n(&g_asset_arrivals, __ATOMIC_RELAXED);
}

void asset_arrival_bump(void) {
    __atomic_add_fetch(&g_asset_arrivals, 1, __ATOMIC_RELAXED);
}

char *concat(const char *a, const char *b) {
    const size_t len_a = strlen(a);
    const size_t len_b = strlen(b);

    char *result = malloc(len_a + len_b + 1);
    if (!result) return NULL;

    memcpy(result, a, len_a);
    memcpy(result + len_a, b, len_b);

    result[len_a + len_b] = 0;

    return result;
}

#ifdef __EMSCRIPTEN__

// On the web every asset lives at an absolute MEMFS path below /assets/, which
// mirrors the HTTP layout of the hosted game (dist/web/assets/...). Files are
// fetched lazily by ensure_asset_file() the first time something opens them.
const char *assets_path() {
    return "/assets/";
}

const char *assets_default_path() {
    return "/assets/";
}

const char *ui_default_path() {
    return "/ui/";
}

static int web_mkdir_parents(const char *path) {
    char buffer[512];
    const size_t length = strlen(path);
    if (length >= sizeof(buffer)) return 0;

    memcpy(buffer, path, length + 1);

    for (char *cursor = buffer + 1; *cursor; cursor++) {
        if (*cursor != '/') continue;

        *cursor = 0;
        if (mkdir(buffer, 0777) && errno != EEXIST) return 0;
        *cursor = '/';
    }

    return 1;
}

// Paths that already failed to download. Callers such as load_texture() retry
// missing files every frame, so a 404 must not turn into a request per frame.
static char **web_missing_assets;
static size_t web_missing_asset_count;

static int web_asset_known_missing(const char *path) {
    for (size_t i = 0; i < web_missing_asset_count; i++) {
        if (!strcmp(web_missing_assets[i], path)) return 1;
    }

    return 0;
}

static void web_asset_mark_missing(const char *path) {
    char **grown = realloc(web_missing_assets, (web_missing_asset_count + 1) * sizeof(char *));
    if (!grown) return;

    web_missing_assets = grown;
    web_missing_assets[web_missing_asset_count] = concat(path, "");
    if (web_missing_assets[web_missing_asset_count]) web_missing_asset_count++;
}

// The download URL of an asset path: the path without its leading slash below
// Module.krunkNativeAssetBase ("" = relative to the page, else e.g.
// "https://assets.krunk.io/"), plus "?v=<Module.krunkNativeAssetVersion>" when the
// asset host published one (assets/version.txt, read by ui/js/loader.js): versioned
// URLs are immutable, unversioned ones revalidate (deploy/pages/README.md "Caching").
// The primary origin is per path once the content is split over two Pages projects
// (deploy/asset-shards.json): loader.js's Module.krunkNativeAssetBaseFor("textures/x.png")
// answers the shard's host for its directories, else Module.krunkNativeAssetBase.
// fallback = 1: the same path below Module.krunkNativeAssetFallback, the second asset
// origin a failed download is retried on (loader.js, kn-config.json "asset_fallback", kru's
// dl.krunk.io with the whole tree; deploy/pages/README.md "Asset origins"); NULL when there is none.
// Returned string is malloc'd and owned by the caller.
EM_JS(char *, web_asset_url_at, (const char *path_ptr, int fallback), {
    var path = UTF8ToString(path_ptr);
    var rel = path.charAt(0) === '/' ? path.substring(1) : path;
    if (rel.lastIndexOf('assets/', 0) === 0) rel = rel.substring(7);
    var base = fallback ? Module.krunkNativeAssetFallback
                        : Module.krunkNativeAssetBaseFor ? Module.krunkNativeAssetBaseFor(rel) : Module.krunkNativeAssetBase;
    if (fallback && !base) return 0;
    var url = (base || '') + (path.charAt(0) === '/' ? path.substring(1) : path);
    if (Module.krunkNativeAssetVersion) url += '?v=' + encodeURIComponent(Module.krunkNativeAssetVersion);
    return stringToNewUTF8(url);
});

static char *web_asset_url(const char *path) {
    return web_asset_url_at(path, 0);
}

// A Krunker user asset ("/assets/user-assets/<id>/<file>", user_asset_path.h) the asset origins do
// not have (a community map's texture / model): the brain's mirror of Krunker's user-asset host,
// <Module.krunkNativeUserAssetBase>, else <Module.krunkNativeBrainUrl or this origin>/api/user-assets/,
// + "<id>/<file>" (services/brain/user_assets.c). user-assets.krunker.io itself sends no CORS
// headers, so the page cannot read it. NULL for any other path. malloc'd.
EM_JS(char *, web_user_asset_url_js, (const char *rel_ptr), {
    var rel = UTF8ToString(rel_ptr);
    var base = Module.krunkNativeUserAssetBase;
    if (!base) { // no backslash in EM_JS: the preprocessor's stringification would eat it
        base = Module.krunkNativeBrainUrl || '';
        while (base.charAt(base.length - 1) === '/') base = base.substring(0, base.length - 1);
        base += '/api/user-assets/';
    }
    return stringToNewUTF8(base + rel);
});

static char *web_user_asset_url(const char *path) {
    const char *rel = path && path[0] == '/' ? path + 1 : path;
    if (rel && !strncmp(rel, "assets/", 7)) rel += 7;
    if (!user_asset_tree_rel_ok(rel)) return NULL;
    return web_user_asset_url_js(rel + sizeof(USER_ASSET_DIR) - 1);
}

char *asset_url(const char *path) {
    return path ? web_asset_url(path) : NULL;
}

// Makes sure `path` exists in MEMFS, downloading it over HTTP when it does not
// (URL: web_asset_url, "/assets/models/x.obj" -> "<base>assets/models/x.obj"). The download is
// synchronous from the caller's point of view thanks to ASYNCIFY, so the
// existing fopen()-based loaders keep working unchanged. Returns 1 when the
// file is available, 0 otherwise (a missing file is reported once).
int ensure_asset_file(const char *path) {
    if (!path || !*path) return 0;
    char redirected[1024];
    path = file_redirect(path, redirected, sizeof(redirected)); // a resource pack's file (mod_pack.c)
    if (!access(path, F_OK)) return 1;
    if (web_asset_known_missing(path)) return 0;

    if (g_asset_yield_hook) g_asset_yield_hook(path);
    char *url = web_asset_url(path);

    void *data = NULL;
    int size = 0;
    int error = 0;

    emscripten_wget_data(url, &data, &size, &error);

    char *fallback = error || !data ? web_asset_url_at(path, 1) : NULL;
    if (fallback) { // the primary origin failed (missing there, or down): the other one
        free(data);
        data = NULL;
        error = 0;
        free(url);
        url = fallback;
        emscripten_wget_data(url, &data, &size, &error);
        if (!error && data) fprintf(stderr, "asset %s: loaded from the fallback origin\n", path);
    }
    char *mirror = error || !data ? web_user_asset_url(path) : NULL;
    if (mirror) { // a community map's user asset: the brain's mirror of Krunker's host
        free(data);
        data = NULL;
        error = 0;
        free(url);
        url = mirror;
        emscripten_wget_data(url, &data, &size, &error);
        if (!error && data && !user_asset_content_ok(path, data, (size_t) size)) error = 1; // a block / error page
    }

    if (error || !data) {
        fprintf(stderr, "failed to fetch asset %s (HTTP error or missing file)\n", url);
        web_asset_mark_missing(path);
        free(url);
        free(data);
        return 0;
    }
    free(url);

    int written = 0;

    if (web_mkdir_parents(path)) {
        FILE *file = fopen(path, "wb");

        if (file) {
            written = fwrite(data, 1, (size_t) size, file) == (size_t) size;
            fclose(file);
        }
    }

    free(data);

    if (!written) {
        fprintf(stderr, "failed to store asset %s in the virtual filesystem\n", path);
        remove(path);
        return 0;
    }

    return 1;
}

// Background download straight into MEMFS (no call back into wasm, so nothing runs while
// an ASYNCIFY fetch is suspended). The page keeps the in-flight / failed sets. mirror_ptr: a user
// asset's mirror URL (web_user_asset_url), tried last; NULL for other paths.
EM_JS(int, web_asset_prefetch, (const char *path_ptr, const char *mirror_ptr), {
    var path = UTF8ToString(path_ptr);
    try { if (FS.analyzePath(path).exists) return 1; } catch (e) { return -1; }
    var state = Module.knPrefetch || (Module.knPrefetch = {pending: {}, failed: {}});
    if (state.failed[path]) return -1;
    if (state.pending[path]) return 0;
    state.pending[path] = 1;
    var rel = path.charAt(0) === '/' ? path.substring(1) : path;
    var query = Module.krunkNativeAssetVersion ? '?v=' + encodeURIComponent(Module.krunkNativeAssetVersion) : '';
    var below = rel.lastIndexOf('assets/', 0) === 0 ? rel.substring(7) : rel;
    var primary = Module.krunkNativeAssetBaseFor ? Module.krunkNativeAssetBaseFor(below)
                                                 : Module.krunkNativeAssetBase; // its shard (web_asset_url_at)
    function get(base) {
        return fetch((base || '') + rel + query).then(function (r) {
            if (!r.ok) throw new Error('HTTP ' + r.status);
            return r.arrayBuffer();
        });
    }
    // the second asset origin when the first fails (web_asset_url_at)
    var mirror = mirror_ptr ? UTF8ToString(mirror_ptr) : '';
    get(primary).catch(function (e) {
        if (!Module.krunkNativeAssetFallback) throw e;
        return get(Module.krunkNativeAssetFallback);
    }).catch(function (e) {
        if (!mirror) throw e;
        return fetch(mirror).then(function (r) {
            if (!r.ok) throw new Error('HTTP ' + r.status);
            return r.arrayBuffer();
        }).then(function (buffer) {
            var head = new Uint8Array(buffer, 0, Math.min(64, buffer.byteLength));
            var text = String.fromCharCode.apply(null, head).toLowerCase();
            text = text.trim(); // an HTML page (a CDN's block page) is never an asset (user_asset_content_ok)
            if (['<!doctype', '<html', '<head', '<!--'].some(function (t) { return text.indexOf(t) === 0; }))
                throw new Error('not an asset');
            return buffer;
        });
    }).then(function (buffer) {
        delete state.pending[path];
        if (FS.analyzePath(path).exists) return;
        FS.mkdirTree(path.substring(0, path.lastIndexOf('/')));
        FS.writeFile(path, new Uint8Array(buffer));
    }).catch(function () {
        delete state.pending[path];
        state.failed[path] = 1;
    });
    return 0;
});

int asset_prefetch(const char *path) {
    if (!path || !*path) return -1;
    char redirected[1024];
    path = file_redirect(path, redirected, sizeof(redirected)); // a resource pack's file (mod_pack.c)
    if (web_asset_known_missing(path)) return -1;
    char *mirror = web_user_asset_url(path);
    const int state = web_asset_prefetch(path, mirror);
    free(mirror);
    return state;
}

// Load-time batches: all downloads run in parallel and the caller waits once (ASYNCIFY
// sleep) instead of paying one network round trip per blocking ensure_asset_file().
// Gives up after ~20 s; anything still missing is then fetched on demand as before.
void asset_prefetch_wait(const char *const *paths, const size_t count) {
    for (size_t i = 0; i < count; i++) asset_prefetch(paths[i]);
    int hooked = 0;

    for (int waited = 0; waited < 20000; waited += 4) {
        int pending = 0;
        for (size_t i = 0; i < count && !pending; i++) pending = asset_prefetch(paths[i]) == 0;
        if (!pending) return;
        if (!hooked && g_asset_yield_hook && count) g_asset_yield_hook(paths[0]);
        hooked = 1;
        emscripten_sleep(4);
    }
}

#else

// The fetch hook (client/src/asset_fetch_client.c) for a path no resource pack / swapper
// redirects: 1 read it, 0 a download is queued, -1 gave up. 1 without a hook.
static int native_fetch_state(const char *path) {
    if (!g_asset_fetch_hook || !path || !*path) return 1;
    char redirected[1024];
    if (file_redirect(path, redirected, sizeof(redirected)) != path) return 1;
    return g_asset_fetch_hook(path, 1);
}

int ensure_asset_file(const char *path) {
    if (g_asset_yield_hook && path && *path) g_asset_yield_hook(path); // where the web build may block
    return native_fetch_state(path) > 0;
}

char *asset_url(const char *path) {
    return path ? concat(path, "") : NULL;
}

int asset_prefetch(const char *path) {
    return native_fetch_state(path);
}

// The web's batch wait, natively: every path the fetch hook is downloading (a community map's user
// assets, game content the launcher's sync missed) is waited for, up to KRUNKNATIVE_ASSET_WAIT_MS
// (default 10 s) in all, so a map is built with its files instead of placeholders that pop in (or,
// for GLTF models, never do: the map object is built without one). Present / unmanaged paths cost
// nothing; a path given up (-1) or still missing at the deadline is left to its loader.
void asset_prefetch_wait(const char *const *paths, const size_t count) {
    if (g_asset_yield_hook && paths && count) g_asset_yield_hook(paths[0]);
    if (!g_asset_fetch_hook || !paths || !count) return;
    for (size_t i = 0; i < count; i++) native_fetch_state(paths[i]); // queues the missing ones
    const char *env = getenv("KRUNKNATIVE_ASSET_WAIT_MS");
    const long budget = env && *env ? atol(env) : 10000;
    for (long waited = 0; waited < budget; waited += 10) {
        int pending = 0;
        for (size_t i = 0; i < count && !pending; i++) {
            char redirected[1024];
            if (!paths[i] || file_redirect(paths[i], redirected, sizeof(redirected)) != paths[i]) continue;
            pending = g_asset_fetch_hook(paths[i], 0) == 0;
        }
        if (!pending) return;
#ifdef WIN32
        Sleep(10);
#else
        const struct timespec ts = {0, 10 * 1000 * 1000};
        nanosleep(&ts, NULL);
#endif
    }
}

// KRUNKNATIVE_ASSETS_DIR (the launcher sets it: the game content in a user-writable data dir),
// else the cwd-based default below. Read once; child windows inherit the variable.
const char *assets_path() {
    static char override[1024];
    static int checked;
    if (!checked) {
        checked = 1;
        const char *dir = getenv("KRUNKNATIVE_ASSETS_DIR");
        const size_t len = dir ? strlen(dir) : 0;
        if (len && len + 2 < sizeof(override))
            snprintf(override, sizeof(override), "%s%s", dir, dir[len - 1] == '/' || dir[len - 1] == '\\' ? "" : "/");
    }
    return override[0] ? override : assets_default_path();
}

// ui/ beside the default assets dir, without going through it: "./assets/../ui/" does not resolve
// when ./assets is missing (the launcher's layout keeps the game content elsewhere).
const char *ui_default_path() {
    const char *assets = assets_default_path();
    return assets && !strncmp(assets, "../", 3) ? "../ui/" : "./ui/";
}

const char *assets_default_path() {
    char path[256];
    if (!getcwd(path, sizeof(path))) return NULL;

#ifdef WIN32
    const char slash = '\\';
    const char *cmake_debug_suffix = "\\cmake-build-debug";
    const char *cmake_release_suffix = "\\cmake-build-release";
#else
    const char slash = '/';
    const char *cmake_debug_suffix = "/cmake-build-debug";
    const char *cmake_release_suffix = "/cmake-build-release";
#endif

    const size_t path_length = strlen(path);
    const size_t debug_length = strlen(cmake_debug_suffix);
    const size_t release_length = strlen(cmake_release_suffix);

    int devel = 0;

    if (path_length >= debug_length && !strcmp(path + path_length - debug_length, cmake_debug_suffix)) devel = 1;
    if (path_length > debug_length && !strcmp(path + path_length - debug_length - 1, cmake_debug_suffix) && path[path_length-1] == slash) devel = 1;

    if (path_length >= release_length && !strcmp(path + path_length - release_length, cmake_release_suffix)) devel = 1;
    if (path_length > release_length && !strcmp(path + path_length - release_length - 1, cmake_release_suffix) && path[path_length-1] == slash) devel = 1;

    const char *devel_assets = "../assets/";
    const char *assets = "./assets/";

    return devel ? devel_assets : assets;
}

#endif

static FileRedirectFn g_file_redirect;
static FileRedirectFn g_file_overlay;

void file_set_redirect(const FileRedirectFn fn) {
    g_file_redirect = fn;
}

void file_set_overlay(const FileRedirectFn fn) {
    g_file_overlay = fn;
}

const char *file_redirect(const char *path, char *buf, const size_t n) {
    if (!path || !buf || !n) return path;
    if (g_file_overlay && g_file_overlay(path, buf, n)) return buf; // the map's resource pack
    if (g_file_redirect && g_file_redirect(path, buf, n)) return buf; // the resource swapper
    return path;
}

int read_file(const char *path, unsigned char **buf, size_t *length) {
    char swapped[1024];
    path = file_redirect(path, swapped, sizeof(swapped)); // resource swapper (client)
    ensure_asset_file(path);

    FILE *file = fopen(path, "rb");
    if (!file) return -1;

    fseek(file, 0, SEEK_END);
    *length = ftell(file);

    fseek(file, 0, SEEK_SET);
    *buf = malloc(*length + 1);

    if (!*buf) {
        fclose(file);
        return -1;
    }

    if (fread(*buf, 1, *length, file) != *length) {
        fclose(file);
        free(*buf);

        *buf = NULL;
        return -1;
    }

    fclose(file);
    (*buf)[*length] = 0;

    return 0;
}

int load_json_file(const char *path, cJSON **out) {
    unsigned char *buffer;
    size_t length;

    if (!out) return -1;
    *out = NULL;

    if (read_file(path, &buffer, &length)) return -1;

    cJSON *json = cJSON_Parse((char *) buffer);
    free(buffer);

    if (!json) return -1;

    *out = json;
    return 0;
}

