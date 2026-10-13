// Map resource packs, see client/include/mod_pack.h.
#include <mod_pack.h>
#include <zip_read.h>
#include <shared.h>
#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#include <process.h>
#define MP_MKDIR(path) _mkdir(path)
#define MP_RMDIR(path) _rmdir(path)
#define MP_PID() _getpid()
#else
#include <sys/stat.h>
#include <unistd.h>
#define MP_MKDIR(path) mkdir(path, 0755)
#define MP_RMDIR(path) rmdir(path)
#define MP_PID() getpid()
#endif

#define MP_PATH_MAX 1024

typedef enum { MP_TEXTURE, MP_MODEL_OBJ, MP_MODEL_GLTF, MP_SOUND } ModFileKind;

typedef struct {
    char *rel;  // the game file it replaces, relative to assets_path() ("textures/weapons/weapon_1.png")
    char *file; // the extracted copy
    ModFileKind kind;
} ModFile;

static struct {
    char url[512];       // the active pack, "" none
    char dir[MP_PATH_MAX]; // its extraction directory
    ModFile *files;
    int count, cap;
    char **dirs;         // directories created under dir (deepest last), removed in reverse
    int dir_count, dir_cap;
    int serial;
    ModPackHooks hooks;
} g_mp;

void mod_pack_set_hooks(const ModPackHooks *hooks) {
    if (hooks) g_mp.hooks = *hooks;
    else memset(&g_mp.hooks, 0, sizeof(g_mp.hooks));
}

const char *mod_pack_active_url(void) {
    return g_mp.url;
}

int mod_pack_file_count(void) {
    return g_mp.count;
}

// ---------------------------------------------------------------- paths

static int starts_with(const char *s, const char *prefix) {
    return !strncmp(s, prefix, strlen(prefix));
}

static int ends_with(const char *s, const char *suffix) {
    const size_t n = strlen(s), m = strlen(suffix);
    return n >= m && !strcmp(s + n - m, suffix);
}

// a relative path that stays inside its directory: no absolute paths, "..", '\\' or ':'
static int safe_relative(const char *path) {
    if (!path || !*path || path[0] == '/' || strchr(path, '\\') || strchr(path, ':')) return 0;
    for (const char *p = path; *p;) {
        const char *slash = strchr(p, '/');
        const size_t n = slash ? (size_t) (slash - p) : strlen(p);
        if (n == 0 || (n == 2 && p[0] == '.' && p[1] == '.') || (n == 1 && p[0] == '.')) return 0;
        p += n + (slash ? 1 : 0);
    }
    return 1;
}

char *mod_pack_resolve_url(const char *url) {
    if (!url || !*url || strchr(url, '&')) return NULL; // v7: no "&" in a pack URL
    // UTILS.isUserAsset / replaceUserAsset: the user-asset host and its older CDN names
    static const char *const schemes[] = {"https://www.", "http://www.", "https://", "http://", ""};
    static const char *const hosts[] = {"user-assets.krunker.io/", "krunker-user-assets.nyc3.cdn.digitaloceanspaces.com/",
                                        "krunker-user-assets.nyc3.digitaloceanspaces.com/", "storage.googleapis.com/user-assets.krunker.io/"};
    const char *rest = NULL;
    for (size_t s = 0; s < sizeof(schemes) / sizeof(schemes[0]) && !rest; s++) {
        if (!starts_with(url, schemes[s])) continue;
        for (size_t h = 0; h < sizeof(hosts) / sizeof(hosts[0]) && !rest; h++)
            if (starts_with(url + strlen(schemes[s]), hosts[h])) rest = url + strlen(schemes[s]) + strlen(hosts[h]);
    }
    if (!rest) return NULL;
    char path[MP_PATH_MAX];
    const size_t n = strcspn(rest, "?#");
    if (n == 0 || n >= sizeof(path) - 16) return NULL;
    memcpy(path, rest, n);
    path[n] = 0;
    if (!safe_relative(path)) return NULL;
    char full[MP_PATH_MAX];
    snprintf(full, sizeof(full), "%suser-assets/%s", assets_path(), path);
    return concat(full, "");
}

// the game file a pack entry replaces (relative to assets_path()), 0 when v7 does not use it
static int entry_target(const char *name, char *rel, const size_t n, ModFileKind *kind) {
    if (!safe_relative(name)) return 0;
    if (starts_with(name, "textures/") || starts_with(name, "img/")) {
        // f1874: updateTexture(filename minus "textures/" / "img/" and ".png"); special keys go to
        // the page or the overlay instead (logo, medals)
        const char *key = name + (name[0] == 't' ? strlen("textures/") : strlen("img/"));
        if (!ends_with(key, ".png") || !strcmp(name, "textures/logo.png") || starts_with(name, "img/medals/")) return 0;
        snprintf(rel, n, "textures/%s", key);
        *kind = MP_TEXTURE;
        return 1;
    }
    if (starts_with(name, "sound/")) {
        // f1875: updateSound(filename.split("/")[1] minus ".mp3")
        const char *file = name + strlen("sound/");
        if (strchr(file, '/') || !ends_with(file, ".mp3")) return 0;
        snprintf(rel, n, "%s", name);
        *kind = MP_SOUND;
        return 1;
    }
    if (starts_with(name, "models/") && !strstr(name, "event_0")) {
        // f1876: updateMesh(filename minus "models/" and the extension, data, extension)
        if (ends_with(name, ".obj")) *kind = MP_MODEL_OBJ;
        else if (ends_with(name, ".gltf") || ends_with(name, ".glb")) *kind = MP_MODEL_GLTF;
        else return 0; // .fbx (no FBX loader), .mtl (OBJ materials are not read)
        snprintf(rel, n, "%s", name);
        return 1;
    }
    return 0; // scripts/, shaders/, css/, css-img/ and anything else
}

static int remember_dir(const char *path) {
    if (g_mp.dir_count == g_mp.dir_cap) {
        const int cap = g_mp.dir_cap ? g_mp.dir_cap * 2 : 16;
        char **grown = realloc(g_mp.dirs, (size_t) cap * sizeof(*grown));
        if (!grown) return 0;
        g_mp.dirs = grown;
        g_mp.dir_cap = cap;
    }
    g_mp.dirs[g_mp.dir_count] = concat(path, "");
    if (!g_mp.dirs[g_mp.dir_count]) return 0;
    g_mp.dir_count++;
    return 1;
}

// creates every missing directory of `file` below g_mp.dir
static void make_parents(const char *file) {
    char buffer[MP_PATH_MAX];
    snprintf(buffer, sizeof(buffer), "%s", file);
    for (char *p = buffer + strlen(g_mp.dir) + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        int known = 0;
        for (int i = 0; i < g_mp.dir_count && !known; i++) known = !strcmp(g_mp.dirs[i], buffer);
        if (!known && MP_MKDIR(buffer) == 0) remember_dir(buffer); // (an existing one is not ours to remove)
        *p = '/';
    }
}

// ---------------------------------------------------------------- the overlay

static int mod_redirect(const char *path, char *out, const size_t n) {
    if (!g_mp.count || !path) return 0;
    const char *assets = assets_path();
    const size_t prefix = assets ? strlen(assets) : 0;
    if (!prefix || strncmp(path, assets, prefix) != 0) return 0;
    const char *rel = path + prefix;
    while (rel[0] == '.' && rel[1] == '/') rel += 2;
    for (int i = 0; i < g_mp.count; i++) {
        if (strcmp(g_mp.files[i].rel, rel) != 0) continue;
        snprintf(out, n, "%s", g_mp.files[i].file);
        return 1;
    }
    return 0;
}

// the loaded copies of every replaced file follow the current overlay
static void refresh_caches(void) {
    for (int i = 0; i < g_mp.count; i++) {
        const ModFile *f = &g_mp.files[i];
        char *full = concat(assets_path(), f->rel);
        if (!full) continue;
        switch (f->kind) {
        case MP_TEXTURE:
            if (g_mp.hooks.texture) g_mp.hooks.texture(full);
            break;
        case MP_MODEL_OBJ:
            if (g_mp.hooks.geometry) g_mp.hooks.geometry(full);
            break;
        case MP_MODEL_GLTF:
            if (g_mp.hooks.gltf) g_mp.hooks.gltf(full);
            break;
        case MP_SOUND:
            if (g_mp.hooks.sound) {
                char name[256];
                snprintf(name, sizeof(name), "%.*s", (int) (strlen(f->rel) - strlen("sound/") - strlen(".mp3")), f->rel + strlen("sound/"));
                g_mp.hooks.sound(name);
            }
            break;
        }
        free(full);
    }
}

static void delete_files(void) {
    for (int i = 0; i < g_mp.count; i++) {
        remove(g_mp.files[i].file);
        free(g_mp.files[i].rel);
        free(g_mp.files[i].file);
    }
    free(g_mp.files);
    g_mp.files = NULL;
    g_mp.count = g_mp.cap = 0;
    for (int i = g_mp.dir_count - 1; i >= 0; i--) {
        MP_RMDIR(g_mp.dirs[i]);
        free(g_mp.dirs[i]);
    }
    free(g_mp.dirs);
    g_mp.dirs = NULL;
    g_mp.dir_count = g_mp.dir_cap = 0;
    if (g_mp.dir[0]) MP_RMDIR(g_mp.dir);
    g_mp.dir[0] = 0;
}

// ---------------------------------------------------------------- loading

static int extract_entry(const char *name, const unsigned char *data, const size_t size, void *user) {
    (void) user;
    char rel[MP_PATH_MAX];
    ModFileKind kind;
    if (!entry_target(name, rel, sizeof(rel), &kind)) return 1;
    for (int i = 0; i < g_mp.count; i++) // a later entry for the same file wins (v7 applies them in order)
        if (!strcmp(g_mp.files[i].rel, rel)) {
            g_mp.count--;
            remove(g_mp.files[i].file);
            free(g_mp.files[i].rel);
            free(g_mp.files[i].file);
            g_mp.files[i] = g_mp.files[g_mp.count];
            break;
        }
    char file[MP_PATH_MAX];
    if ((size_t) snprintf(file, sizeof(file), "%s/%s", g_mp.dir, rel) >= sizeof(file)) return 1;
    make_parents(file);
    FILE *out = fopen(file, "wb");
    if (!out) return 1;
    const int written = fwrite(data, 1, size, out) == size;
    fclose(out);
    if (!written) {
        remove(file);
        return 1;
    }
    if (g_mp.count == g_mp.cap) {
        const int cap = g_mp.cap ? g_mp.cap * 2 : 32;
        ModFile *grown = realloc(g_mp.files, (size_t) cap * sizeof(*grown));
        if (!grown) {
            remove(file);
            return 0;
        }
        g_mp.files = grown;
        g_mp.cap = cap;
    }
    ModFile *f = &g_mp.files[g_mp.count];
    f->rel = concat(rel, "");
    f->file = concat(file, "");
    f->kind = kind;
    if (!f->rel || !f->file) {
        free(f->rel);
        free(f->file);
        remove(file);
        return 0;
    }
    g_mp.count++;
    return 1;
}

static void temp_root(char *out, const size_t n) {
#ifdef __EMSCRIPTEN__
    snprintf(out, n, "/tmp"); // MEMFS
#elif defined(_WIN32)
    const char *dir = getenv("TEMP");
    snprintf(out, n, "%s", dir && *dir ? dir : ".");
#else
    const char *dir = getenv("TMPDIR");
    snprintf(out, n, "%s", dir && *dir ? dir : "/tmp");
#endif
    const size_t len = strlen(out);
    if (len > 1 && out[len - 1] == '/') out[len - 1] = 0;
}

void mod_pack_clear(void) {
    if (!g_mp.url[0] && !g_mp.count) return;
    if (g_mp.url[0]) printf("[mod] removed %s: the game's own files are back\n", g_mp.url);
    file_set_overlay(NULL); // first: the refresh reads the game's own files
    refresh_caches();
    delete_files();
    g_mp.url[0] = 0;
}

void mod_pack_shutdown(void) {
    file_set_overlay(NULL);
    delete_files();
    g_mp.url[0] = 0;
}

int mod_pack_load(const char *url) {
    if (!url || !*url) {
        mod_pack_clear();
        return 0;
    }
    if (g_mp.url[0] && !strcmp(g_mp.url, url)) return 1;
    char *zip_path = mod_pack_resolve_url(url);
    if (!zip_path) {
        // v7 also loads Dropbox links; KrunkNative fetches nothing outside its asset host
        printf("[mod] %s: not on the user-asset host, not loaded\n", url);
        mod_pack_clear();
        return 0;
    }
    unsigned char *zip = NULL;
    size_t size = 0;
    const int read_error = read_file(zip_path, &zip, &size);
    if (read_error || !zip) {
        printf("[mod] %s: cannot read %s\n", url, zip_path);
        free(zip_path);
        free(zip);
        mod_pack_clear();
        return 0;
    }
    mod_pack_clear();

    char root[MP_PATH_MAX];
    temp_root(root, sizeof(root));
    MP_MKDIR(root); // (MEMFS /tmp, or a TMPDIR that does not exist yet)
    snprintf(g_mp.dir, sizeof(g_mp.dir), "%s/kn_modpack_%d_%d", root, (int) MP_PID(), ++g_mp.serial);
    MP_MKDIR(g_mp.dir);
    const int entries = zip_read_entries(zip, size, extract_entry, NULL);
    free(zip);
    if (!entries || !g_mp.count) {
        printf("[mod] %s: %s (%d entries, none replaces a game file)\n", url, entries ? "nothing to apply" : "not a zip archive", entries);
        delete_files();
        free(zip_path);
        return 0;
    }
    snprintf(g_mp.url, sizeof(g_mp.url), "%s", url);
    file_set_overlay(mod_redirect);
    refresh_caches();
    int counts[4] = {0, 0, 0, 0};
    for (int i = 0; i < g_mp.count; i++) counts[g_mp.files[i].kind]++;
    printf("[mod] loaded %s (%s): %d files replace the game's (%d textures, %d models, %d sounds)\n", url, zip_path, g_mp.count,
           counts[MP_TEXTURE], counts[MP_MODEL_OBJ] + counts[MP_MODEL_GLTF], counts[MP_SOUND]);
    free(zip_path);
    return 1;
}

void mod_pack_map_loaded(const cJSON *map_json) {
    const char *url = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(map_json, "modURL"));
    if (url && *url) mod_pack_load(url); // (the same pack again: nothing to do, v11759 != modURL)
    else mod_pack_clear();               // KrunkNative: the pack belongs to its map (mod_pack.h)
}
