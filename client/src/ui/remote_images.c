// Remote images for the native HTML engine (client/include/remote_images.h): the engine's fetcher
// (engine/image.h ImageRemoteFetch), a disk cache by URL and the downloads on the media lane.
// Everything runs on the main thread: the engine asks from image_get(), the answers arrive through
// backend_poll(); only the HTTP transfer itself is on the lane's worker.
#include <remote_images.h>
#include <backend.h>
#include <html_ui.h>
#include <shared.h> // asset_arrival_bump
#include <ui_logic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#ifndef __EMSCRIPTEN__
#include <stb_image.h>
#include <errno.h>
#include <stdint.h>
#include <sys/stat.h>
#include <time.h>
#ifdef _WIN32
#include <direct.h>
#define ri_mkdir(p) _mkdir(p)
#else
#define ri_mkdir(p) mkdir((p), 0755)
#endif

typedef enum { RI_QUEUED, RI_LOADING, RI_DONE, RI_FAILED } RiState;

typedef struct {
    uint64_t hash;
    char *url;
    RiState state;
    time_t failed_at;
    int refresh; // a stale file is shown while this download replaces it
} RiEntry;

static struct {
    RiEntry *list;
    int count, cap;
    int loading; // requests on the lane
    cJSON *scenario; // state.remoteImages {url: file} (owned)
} R;

static uint64_t fnv1a(const char *s) {
    uint64_t h = 1469598103934665603ull;
    for (; *s; s++) h = (h ^ (unsigned char) *s) * 1099511628211ull;
    return h;
}

const char *remote_images_cache_dir(void) {
    static char dir[1024];
    static int done;
    if (done) return dir;
    done = 1;
    const char *over = getenv("KRUNKNATIVE_REMOTE_IMAGE_CACHE");
    const char *xdg = getenv("XDG_CACHE_HOME");
    int n;
    if (over && *over) n = snprintf(dir, sizeof(dir), "%s", over);
    else if (xdg && *xdg) n = snprintf(dir, sizeof(dir), "%s/krunknative/remote-images", xdg);
    else {
#ifdef _WIN32
        const char *base = getenv("LOCALAPPDATA");
        n = base && *base ? snprintf(dir, sizeof(dir), "%s\\krunknative\\remote-images", base) : 0;
#else
        const char *home = getenv("HOME");
        n = home && *home ? snprintf(dir, sizeof(dir), "%s/.cache/krunknative/remote-images", home) : 0;
#endif
    }
    if (n <= 0 || (size_t) n >= sizeof(dir)) dir[0] = 0;
    return dir;
}

static void mkdirs(const char *dir) {
    char path[1024];
    snprintf(path, sizeof(path), "%s", dir);
    for (char *p = path + 1; *p; p++) {
        if (*p != '/' && *p != '\\') continue;
        const char c = *p;
        *p = 0;
        ri_mkdir(path);
        *p = c;
    }
    ri_mkdir(path);
}

static int cache_file(const uint64_t hash, char *out, const size_t n) {
    const char *dir = remote_images_cache_dir();
    if (!dir[0]) return 0;
    const int w = snprintf(out, n, "%s/%016llx.img", dir, (unsigned long long) hash);
    return w > 0 && (size_t) w < n;
}

static RiEntry *find(const uint64_t hash) {
    for (int i = 0; i < R.count; i++) {
        if (R.list[i].hash == hash) return &R.list[i];
    }
    return NULL;
}

static void start_next(void);

// The body: a raster stb_image reads, within the limits; written next to its final name, then
// renamed over it, so the engine never reads half a file.
static int store(const RiEntry *e, const char *body, const size_t length, char *why, const size_t why_n) {
    int w = 0, h = 0, comp = 0;
    if (!length || length > REMOTE_IMAGE_MAX_BYTES) {
        snprintf(why, why_n, "%zu bytes (limit %u)", length, REMOTE_IMAGE_MAX_BYTES);
        return 0;
    }
    if (!stbi_info_from_memory((const unsigned char *) body, (int) length, &w, &h, &comp)) {
        snprintf(why, why_n, "not an image (%s)", stbi_failure_reason() ? stbi_failure_reason() : "unknown format");
        return 0;
    }
    if (w <= 0 || h <= 0 || w > REMOTE_IMAGE_MAX_SIDE || h > REMOTE_IMAGE_MAX_SIDE) {
        snprintf(why, why_n, "%dx%d px (limit %d a side)", w, h, REMOTE_IMAGE_MAX_SIDE);
        return 0;
    }
    char path[1100], tmp[1120];
    if (!cache_file(e->hash, path, sizeof(path))) {
        snprintf(why, why_n, "no cache directory");
        return 0;
    }
    mkdirs(remote_images_cache_dir());
    snprintf(tmp, sizeof(tmp), "%s.%d.tmp", path, (int) (e - R.list));
    FILE *f = fopen(tmp, "wb");
    int ok = f && fwrite(body, 1, length, f) == length;
    if (f && fclose(f)) ok = 0;
    if (ok) {
#ifdef _WIN32
        remove(path);
#endif
        ok = rename(tmp, path) == 0;
    }
    if (!ok) {
        remove(tmp);
        snprintf(why, why_n, "cannot write %s: %s", path, strerror(errno));
    }
    return ok;
}

static void on_body(void *user, const int status, const char *body, const size_t length) {
    const uint64_t hash = *(const uint64_t *) user;
    free(user);
    R.loading--;
    RiEntry *e = find(hash);
    if (e) {
        char why[300] = "";
        if (status == 200 && store(e, body, length, why, sizeof(why))) {
            e->state = RI_DONE;
            printf("[remote-image] %s: %zu bytes%s\n", e->url, length, e->refresh ? " (refreshed)" : "");
            if (!e->refresh) asset_arrival_bump(); // the engine loads it (image_cache_retry_missing)
        } else {
            if (!why[0]) {
                if (status) snprintf(why, sizeof(why), "HTTP %d", status);
                else snprintf(why, sizeof(why), "%s", body && body[0] ? body : "unreachable");
            }
            // a refresh that fails keeps the old file (still shown)
            e->state = e->refresh ? RI_DONE : RI_FAILED;
            e->failed_at = time(NULL);
            fprintf(stderr, "[remote-image] %s failed: %s\n", e->url, why);
        }
    }
    start_next();
}

static int submit(RiEntry *e) {
    uint64_t *user = malloc(sizeof(*user));
    if (!user) return 0;
    *user = e->hash;
    if (!backend_http_get_media(e->url, REMOTE_IMAGE_TIMEOUT_MS, on_body, user)) {
        free(user);
        return 0;
    }
    e->state = RI_LOADING;
    R.loading++;
    return 1;
}

static void start_next(void) {
    for (int i = 0; i < R.count && R.loading < REMOTE_IMAGE_MAX_QUEUED; i++) {
        RiEntry *e = &R.list[i];
        if (e->state == RI_QUEUED && !submit(e)) {
            e->state = RI_FAILED;
            e->failed_at = time(NULL);
            fprintf(stderr, "[remote-image] %s failed: the request could not be queued\n", e->url);
        }
    }
}

static RiEntry *add(const char *url, const uint64_t hash) {
    if (R.count >= REMOTE_IMAGE_MAX_URLS) return NULL;
    if (R.count == R.cap) {
        const int cap = R.cap ? R.cap * 2 : 32;
        RiEntry *grown = realloc(R.list, (size_t) cap * sizeof(*grown));
        if (!grown) return NULL;
        R.list = grown;
        R.cap = cap;
    }
    char *copy = strdup(url);
    if (!copy) return NULL;
    RiEntry *e = &R.list[R.count++];
    memset(e, 0, sizeof(*e));
    e->hash = hash;
    e->url = copy;
    e->state = RI_QUEUED;
    return e;
}

static int file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && st.st_size > 0;
}

// ImageRemoteFetch (engine/image.h): 1 = `path` holds it, 0 = downloading into `path`, -1 = refused.
static int fetch(const char *url, char *path, const size_t cap) {
    if (!url || (strncasecmp(url, "http://", 7) && strncasecmp(url, "https://", 8))) return -1;
#ifdef UI_SCENARIO_SUPPORTED
    if (ui_scenario_active()) { // a render never reaches the network: the scenario's own files or nothing
        const char *file = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(R.scenario, url));
        if (!file || !file_exists(file)) {
            fprintf(stderr, "[remote-image] %s: not in state.remoteImages (renders fetch nothing)\n", url);
            return -1;
        }
        snprintf(path, cap, "%s", file);
        return 1;
    }
#endif
    const uint64_t hash = fnv1a(url);
    char file[1100];
    if (!cache_file(hash, file, sizeof(file)) || strlen(file) >= cap) return -1;
    snprintf(path, cap, "%s", file);
    RiEntry *e = find(hash);
    struct stat st;
    if (stat(file, &st) == 0 && st.st_size > 0) {
        // on disk: shown now; an old file is fetched again in the background (once per run)
        if (!e && time(NULL) - st.st_mtime > REMOTE_IMAGE_MAX_AGE_S && (e = add(url, hash))) {
            e->refresh = 1;
            start_next();
        }
        return 1;
    }
    if (e) {
        if (e->state == RI_FAILED) {
            if (time(NULL) - e->failed_at < REMOTE_IMAGE_RETRY_S) return -1;
            e->state = RI_QUEUED;
            e->refresh = 0;
            start_next();
        }
        return e->state == RI_DONE && !file_exists(file) ? -1 : 0;
    }
    if (!(e = add(url, hash))) {
        fprintf(stderr, "[remote-image] %s refused: more than %d remote images this run\n", url, REMOTE_IMAGE_MAX_URLS);
        return -1;
    }
    start_next();
    return 0;
}

void remote_images_init(void) { html_ui_set_remote_image_fetcher(fetch); }

void remote_images_scenario(const cJSON *map) {
    cJSON_Delete(R.scenario);
    R.scenario = cJSON_IsObject(map) ? cJSON_Duplicate(map, 1) : NULL;
    remote_images_init();
}

#else // ---------------------------------------------------------------- web: the browser loads them

const char *remote_images_cache_dir(void) { return ""; }
void remote_images_init(void) {}
void remote_images_scenario(const cJSON *map) { (void) map; }

#endif
