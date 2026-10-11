#include <shared.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifdef WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <sys/stat.h>
#include <errno.h>
#endif

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

// Makes sure `path` exists in MEMFS, downloading it over HTTP when it does not.
// The URL is the path without its leading slash, i.e. relative to the page
// ("/assets/models/x.obj" -> "assets/models/x.obj"). The download is
// synchronous from the caller's point of view thanks to ASYNCIFY, so the
// existing fopen()-based loaders keep working unchanged. Returns 1 when the
// file is available, 0 otherwise (a missing file is reported once).
int ensure_asset_file(const char *path) {
    if (!path || !*path) return 0;
    if (!access(path, F_OK)) return 1;
    if (web_asset_known_missing(path)) return 0;

    const char *url = path[0] == '/' ? path + 1 : path;

    void *data = NULL;
    int size = 0;
    int error = 0;

    emscripten_wget_data(url, &data, &size, &error);

    if (error || !data) {
        fprintf(stderr, "failed to fetch asset %s (HTTP error or missing file)\n", url);
        web_asset_mark_missing(path);
        free(data);
        return 0;
    }

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
// an ASYNCIFY fetch is suspended). The page keeps the in-flight / failed sets.
EM_JS(int, web_asset_prefetch, (const char *path_ptr), {
    var path = UTF8ToString(path_ptr);
    try { if (FS.analyzePath(path).exists) return 1; } catch (e) { return -1; }
    var state = Module.knPrefetch || (Module.knPrefetch = {pending: {}, failed: {}});
    if (state.failed[path]) return -1;
    if (state.pending[path]) return 0;
    state.pending[path] = 1;
    fetch(path.charAt(0) === '/' ? path.substring(1) : path).then(function (r) {
        if (!r.ok) throw new Error('HTTP ' + r.status);
        return r.arrayBuffer();
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
    if (web_asset_known_missing(path)) return -1;
    return web_asset_prefetch(path);
}

// Load-time batches: all downloads run in parallel and the caller waits once (ASYNCIFY
// sleep) instead of paying one network round trip per blocking ensure_asset_file().
// Gives up after ~20 s; anything still missing is then fetched on demand as before.
void asset_prefetch_wait(const char *const *paths, const size_t count) {
    for (size_t i = 0; i < count; i++) asset_prefetch(paths[i]);

    for (int waited = 0; waited < 20000; waited += 4) {
        int pending = 0;
        for (size_t i = 0; i < count && !pending; i++) pending = asset_prefetch(paths[i]) == 0;
        if (!pending) return;
        emscripten_sleep(4);
    }
}

#else

int ensure_asset_file(const char *path) {
    (void) path;
    return 1;
}

int asset_prefetch(const char *path) {
    (void) path;
    return 1;
}

void asset_prefetch_wait(const char *const *paths, const size_t count) {
    (void) paths;
    (void) count;
}

const char *assets_path() {
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

void file_set_redirect(const FileRedirectFn fn) {
    g_file_redirect = fn;
}

const char *file_redirect(const char *path, char *buf, const size_t n) {
    if (!g_file_redirect || !path || !buf || !n) return path;
    return g_file_redirect(path, buf, n) ? buf : path;
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

int parse_hex_color(const char *hex_str, vec4 *out) {
    const int hex_len = (int) strlen(hex_str) - 1;

    if (hex_len < 0 || hex_str[0] != '#') return -1;
    hex_str++;

    // #RGA, #RGBA, #RRGGBB, #RRGGBBAA
    if (hex_len != 3 && hex_len != 4 && hex_len != 6 && hex_len != 8) return -1;

    unsigned char parsed;
    unsigned char channels[4] = {0};

    for (int i = 0; i < hex_len; i++) {
        const char value = hex_str[i];

        if (value >= 'A' && value <= 'F') parsed = value - 'A' + 10;
        else if (value >= 'a' && value <= 'f') parsed = value - 'a' + 10;
        else if (value >= '0' && value <= '9') parsed = value - '0';
        else return -1;

        if (hex_len < 6) {
            if (i > 3) i = 3; // SILENCE, LINTER!
            channels[i] = parsed << 4 | parsed;
        } else {
            channels[i / 2] |= parsed << (i % 2 ? 0 : 4);
        }
    }

    out->x = (float) channels[0] / 255.0f;
    out->y = (float) channels[1] / 255.0f;
    out->z = (float) channels[2] / 255.0f;
    out->w = hex_len == 4 || hex_len == 8 ? (float) channels[3] / 255.0f : 1.0f;

    return 0;
}

vec4 hex_to_vec(const int hex) {
    return (vec4) {
        (float) (hex >> 16 & 0xff) / 255.0f,
        (float) (hex >> 8 & 0xff) / 255.0f,
        (float) (hex & 0xff) / 255.0f,
        1.0f,
    };
}

float normalize_angle(float angle) {
    angle = fmodf(angle + (float) M_PI, (float) M_PI * 2.0f);

    if (angle < 0) return angle + (float) M_PI;
    return angle - (float) M_PI;
}

float progress_on_line(const vec2 line_start, const vec2 line_end, const vec2 point) {
    const vec2 dir = {line_end.x - line_start.x, line_end.y - line_start.y};
    return (dir.x * (point.x - line_start.x) + (point.y - line_start.y) * dir.y) / (dir.x*dir.x + dir.y*dir.y);
}

float line_in_rect(const vec3 origin, const vec3 direction, const vec3 obj_origin, const vec3 obj_scale) {
    if (
        direction.x > 0.0f && obj_origin.x + obj_scale.x * 0.5f < origin.x ||
        direction.x < 0.0f && obj_origin.x - obj_scale.x * 0.5f > origin.x ||
        direction.y > 0.0f && obj_origin.y + obj_scale.y < origin.y ||
        direction.y < 0.0f && obj_origin.y > origin.y ||
        direction.z > 0.0f && obj_origin.z + obj_scale.z * 0.5f < origin.z ||
        direction.z < 0.0f && obj_origin.z - obj_scale.z * 0.5f > origin.z
    ) return -1.0f;

    const float tx1 = (obj_origin.x - obj_scale.x * 0.5f - origin.x) / direction.x;
    const float tx2 = (obj_origin.x + obj_scale.x * 0.5f - origin.x) / direction.x;
    const float ty1 = (obj_origin.y - origin.y) / direction.y;
    const float ty2 = (obj_origin.y + obj_scale.y - origin.y) / direction.y;
    const float tz1 = (obj_origin.z - obj_scale.z * 0.5f - origin.z) / direction.z;
    const float tz2 = (obj_origin.z + obj_scale.z * 0.5f - origin.z) / direction.z;

    const float tmin = MAX(MAX(MIN(tx1, tx2), MIN(ty1, ty2)), MIN(tz1, tz2));
    const float tmax = MIN(MIN(MAX(tx1, tx2), MAX(ty1, ty2)), MAX(tz1, tz2));

    return tmin <= tmax && tmin > 0 ? tmin : -1.0f;
}

void angles_from_sides(const float a, const float b, const float c, float *out) {
    out[0] = acosf((b*b + c*c - a*a) / (2.0f * b * c));
    out[1] = acosf((a*a + c*c - b*b) / (2.0f * a * c));
    out[2] = acosf((a*a + b*b - c*c) / (2.0f * a * b));
}
