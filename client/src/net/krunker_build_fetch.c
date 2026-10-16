// The Krunker build an official join speaks, fetched live (client/include/krunker_build_fetch.h).
#include <krunker_build_fetch.h>
#include <krunker_build_live.h>
#include <auth_join.h> // kn_sha256_bytes / kn_sha256_hex

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifdef _WIN32
#include <direct.h>
#define kbf_mkdir(p) _mkdir(p)
#else
#define kbf_mkdir(p) mkdir((p), 0755)
#endif

#define KBF_MAX_DOC (1u << 20) // a build document (~25 KB today)
#define KBF_MAX_ADOPTED 32

static struct {
    KrunkerBuildHttpGet http;
    unsigned request;                // bumped per refresh: an older one's answers are dropped
    KrunkerBuildReady done;
    void *user;
    char url[1024];
    KrunkerBuildManifest manifest;   // the request's manifest (between the two GETs)
    time_t fetched_at;               // the last refresh that read the manifest (0: none)
    char fetched_url[1024];
    // every build made current so far (never freed: krunker_build_live.h), by the sha256 of its document
    KrunkerBuild *adopted[KBF_MAX_ADOPTED];
    char adopted_sha[KBF_MAX_ADOPTED][65];
    int adopted_count;
#ifdef __EMSCRIPTEN__
    char *mem_cache; // the web's cache: the last accepted document
    size_t mem_cache_len;
#endif
} F;

const char *krunker_build_source_name(const KrunkerBuildSource src) {
    switch (src) {
        case KRUNKER_BUILD_SRC_LIVE: return "live";
        case KRUNKER_BUILD_SRC_CACHED: return "cached";
        case KRUNKER_BUILD_SRC_REFUSED: return "refused";
        default: return "compiled";
    }
}

const char *krunker_build_fetch_url(const int production, const char *setting) {
    static char url[1024];
    if (setting && setting[0]) {
        if (!strcmp(setting, "off") || !strcmp(setting, "0")) return NULL;
        snprintf(url, sizeof(url), "%s", setting);
        return url;
    }
    return production ? KRUNKER_BUILD_DEFAULT_URL : NULL;
}

void krunker_build_fetch_set_http(const KrunkerBuildHttpGet get) { F.http = get; }

// ---------------------------------------------------------------- the cache

static void doc_sha(const char *doc, const size_t len, char out[65]) {
    uint8_t digest[32];
    kn_sha256_bytes(doc, len, digest);
    kn_sha256_hex(digest, out);
}

#ifndef __EMSCRIPTEN__
static const char *cache_path(void) {
    static char path[1100];
    static int done;
    if (done) return path;
    done = 1;
    const char *over = getenv("KRUNKNATIVE_KRUNKER_BUILD_CACHE");
    const char *xdg = getenv("XDG_CACHE_HOME");
    int n = 0;
    if (over && *over) n = snprintf(path, sizeof(path), "%s/krunker-build.json", over);
    else if (xdg && *xdg) n = snprintf(path, sizeof(path), "%s/krunknative/krunker-live/krunker-build.json", xdg);
    else {
#ifdef _WIN32
        const char *base = getenv("LOCALAPPDATA");
        if (base && *base) n = snprintf(path, sizeof(path), "%s\\krunknative\\krunker-live\\krunker-build.json", base);
#else
        const char *home = getenv("HOME");
        if (home && *home) n = snprintf(path, sizeof(path), "%s/.cache/krunknative/krunker-live/krunker-build.json", home);
#endif
    }
    if (n <= 0 || (size_t) n >= sizeof(path)) path[0] = 0;
    return path;
}

static void mkdirs_for(const char *file) {
    char dir[1100];
    snprintf(dir, sizeof(dir), "%s", file);
    char *slash = strrchr(dir, '/');
    char *bslash = strrchr(dir, '\\');
    if (bslash && (!slash || bslash > slash)) slash = bslash;
    if (!slash) return;
    *slash = 0;
    for (char *p = dir + 1; *p; p++) {
        if (*p != '/' && *p != '\\') continue;
        const char c = *p;
        *p = 0;
        kbf_mkdir(dir);
        *p = c;
    }
    kbf_mkdir(dir);
}
#endif

// the cached document (malloc'd) or NULL
static char *cache_read(size_t *len) {
#ifdef __EMSCRIPTEN__
    if (!F.mem_cache) return NULL;
    char *copy = malloc(F.mem_cache_len + 1);
    if (!copy) return NULL;
    memcpy(copy, F.mem_cache, F.mem_cache_len);
    copy[F.mem_cache_len] = 0;
    *len = F.mem_cache_len;
    return copy;
#else
    const char *path = cache_path();
    FILE *f = path[0] ? fopen(path, "rb") : NULL;
    if (!f) return NULL;
    char *buf = malloc(KBF_MAX_DOC + 1);
    const size_t n = buf ? fread(buf, 1, KBF_MAX_DOC + 1, f) : 0;
    fclose(f);
    if (!buf || !n || n > KBF_MAX_DOC) {
        free(buf);
        return NULL;
    }
    buf[n] = 0;
    *len = n;
    return buf;
#endif
}

static void cache_write(const char *doc, const size_t len) {
#ifdef __EMSCRIPTEN__
    char *copy = malloc(len);
    if (!copy) return;
    memcpy(copy, doc, len);
    free(F.mem_cache);
    F.mem_cache = copy;
    F.mem_cache_len = len;
#else
    const char *path = cache_path();
    if (!path[0]) return;
    char tmp[1110];
    mkdirs_for(path);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    int ok = f && fwrite(doc, 1, len, f) == len;
    if (f && fclose(f)) ok = 0;
    if (ok) {
#ifdef _WIN32
        remove(path);
#endif
        ok = rename(tmp, path) == 0;
    }
    if (!ok) {
        remove(tmp);
        fprintf(stderr, "[krunker] cannot cache the Krunker build in %s: %s\n", path, strerror(errno));
    }
#endif
}

// ---------------------------------------------------------------- adopting

// Make b current (taking it), or the adopted build of the same document (freeing b). 0: no room.
static int adopt(KrunkerBuild *b, const char *sha) {
    for (int i = 0; i < F.adopted_count; i++) {
        if (strcmp(F.adopted_sha[i], sha)) continue;
        krunker_build_free(b);
        krunker_build_use(F.adopted[i]);
        return 1;
    }
    if (F.adopted_count >= KBF_MAX_ADOPTED) { // (32 Krunker builds in one session: keep the last one)
        krunker_build_free(b);
        return 0;
    }
    F.adopted[F.adopted_count] = b;
    snprintf(F.adopted_sha[F.adopted_count], sizeof(F.adopted_sha[0]), "%s", sha);
    F.adopted_count++;
    krunker_build_use(b);
    return 1;
}

static void finish(const KrunkerBuildSource src, const char *client_version, const char *why) {
    const KrunkerBuildReady done = F.done;
    void *user = F.user;
    F.done = NULL;
    const KrunkerBuild *b = krunker_build();
    if (src == KRUNKER_BUILD_SRC_REFUSED) {
        printf("[krunker] Krunker build %s refused: %s\n", client_version && client_version[0] ? client_version : "(unknown)", why ? why : "");
    } else {
        printf("[krunker] Krunker build: %s %s (buildVersion %s, AHKEY 0x%llx)%s%s\n", krunker_build_source_name(src), b->client_version,
               b->build_version, (unsigned long long) b->ahkey, why && why[0] ? ": " : "", why ? why : "");
    }
    if (done) done(user, src, src == KRUNKER_BUILD_SRC_REFUSED ? client_version : b->client_version, why ? why : "");
}

// The fetch did not give a usable build: the cached document, else the compiled one.
static void fallback(const char *why) {
    size_t len = 0;
    char *doc = cache_read(&len);
    KrunkerBuild *b = NULL;
    char reason[200];
    if (doc && krunker_build_parse(doc, len, &b, reason, sizeof(reason)) == KRUNKER_BUILD_OK) {
        char sha[65];
        doc_sha(doc, len, sha);
        free(doc);
        if (adopt(b, sha)) {
            finish(KRUNKER_BUILD_SRC_CACHED, NULL, why);
            return;
        }
    } else {
        free(doc);
    }
    krunker_build_use(NULL);
    finish(KRUNKER_BUILD_SRC_COMPILED, NULL, why);
}

// The document (fetched, or the cached copy of the manifest's sha256) against the manifest.
static void take_doc(const char *doc, const size_t len, const int from_cache) {
    KrunkerBuild *b = NULL;
    char why[240];
    const int rc = krunker_build_accept(&F.manifest, doc, len, &b, why, sizeof(why));
    if (rc == KRUNKER_BUILD_ESHAPE) {
        // a build this client cannot follow: the join fails saying so (the compiled build would be stale)
        krunker_build_use(NULL);
        finish(KRUNKER_BUILD_SRC_REFUSED, F.manifest.client_version, why);
        return;
    }
    if (rc != KRUNKER_BUILD_OK) {
        char text[300];
        snprintf(text, sizeof(text), "the published build was refused (%s)", why);
        fallback(text);
        return;
    }
    if (!from_cache) cache_write(doc, len);
    if (!adopt(b, F.manifest.sha256)) {
        fallback("too many Krunker builds in one session");
        return;
    }
    finish(KRUNKER_BUILD_SRC_LIVE, NULL, "");
}

static void on_doc(void *ctx, const int status, const char *body, const size_t length) {
    if ((unsigned) (uintptr_t) ctx != F.request || !F.done) return;
    if (status != 200 || !body || !length || length > KBF_MAX_DOC) {
        char why[120];
        snprintf(why, sizeof(why), "the published build's document could not be fetched (HTTP %d)", status);
        fallback(why);
        return;
    }
    take_doc(body, length, 0);
}

static void on_manifest(void *ctx, const int status, const char *body, const size_t length) {
    if ((unsigned) (uintptr_t) ctx != F.request || !F.done) return;
    char why[240];
    if (status != 200 || !body || !length) {
        snprintf(why, sizeof(why), "%s: HTTP %d", F.url, status);
        fallback(why);
        return;
    }
    const int rc = krunker_build_manifest_parse(body, length, &F.manifest, why, sizeof(why));
    if (rc == KRUNKER_BUILD_ESHAPE) {
        krunker_build_use(NULL);
        finish(KRUNKER_BUILD_SRC_REFUSED, "", why);
        return;
    }
    if (rc != KRUNKER_BUILD_OK) {
        fallback(why);
        return;
    }
    F.fetched_at = time(NULL);
    snprintf(F.fetched_url, sizeof(F.fetched_url), "%s", F.url);
    // the same document as an adopted build, or as the cache: no download
    for (int i = 0; i < F.adopted_count; i++) {
        if (!strcmp(F.adopted_sha[i], F.manifest.sha256)) {
            krunker_build_use(F.adopted[i]);
            finish(KRUNKER_BUILD_SRC_LIVE, NULL, "");
            return;
        }
    }
    size_t len = 0;
    char *cached = cache_read(&len);
    if (cached) {
        char sha[65];
        doc_sha(cached, len, sha);
        if (!strcmp(sha, F.manifest.sha256)) {
            take_doc(cached, len, 1);
            free(cached);
            return;
        }
        free(cached);
    }
    char doc_url[1200];
    if (!krunker_build_doc_url(F.url, &F.manifest, doc_url, sizeof(doc_url))) {
        fallback("the manifest's document path does not resolve");
        return;
    }
    if (!F.http || !F.http(doc_url, on_doc, (void *) (uintptr_t) F.request)) fallback("the document could not be requested");
}

int krunker_build_refresh(const char *url, const int force, const KrunkerBuildReady done, void *user) {
    F.request++;
    F.done = done;
    F.user = user;
    if (!url || !url[0]) {
        krunker_build_use(NULL);
        finish(KRUNKER_BUILD_SRC_COMPILED, NULL, "");
        return 1;
    }
    // within the TTL of a manifest read from this URL: the current build stands
    if (!force && F.fetched_at && krunker_build()->live && !strcmp(F.fetched_url, url) &&
        time(NULL) - F.fetched_at < KRUNKER_BUILD_FETCH_TTL_S) {
        finish(KRUNKER_BUILD_SRC_LIVE, NULL, "");
        return 1;
    }
    snprintf(F.url, sizeof(F.url), "%s", url);
    if (!F.http || !F.http(url, on_manifest, (void *) (uintptr_t) F.request)) {
        fallback("the manifest could not be requested");
        return 0;
    }
    return 1;
}
