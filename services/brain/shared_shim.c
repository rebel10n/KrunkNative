/*
 * services/brain/shared_shim.c - the four shared/src/utils.c helpers that
 * shared/src/map_catalog.c needs, so kn_brain can compile the v3.1.1 map
 * catalogue (shared/include/map_catalog.h) without linking the game's shared
 * sources. Assets are read from <repo root>/assets/ (config root), or
 * accounts.assets_dir when set.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "brain.h"
#include "knsvc/config.h"

const char *assets_path(void);
char *concat(const char *a, const char *b);
int read_file(const char *path, unsigned char **buf, size_t *length);
void asset_prefetch_wait(const char *const *paths, size_t count);

const char *assets_path(void) {
    static char path[1024];
    if (!path[0]) {
        const Brain *b = brain_get();
        const char *rel = b ? kn_config_str(b->cfg, "accounts.assets_dir", "assets") : "assets";
        if (!b || !kn_config_resolve_path(b->cfg, rel, path, sizeof path - 1)) snprintf(path, sizeof path - 1, "%s", rel);
        const size_t n = strlen(path);
        if (n && path[n - 1] != '/') path[n] = '/', path[n + 1] = 0;
    }
    return path;
}

char *concat(const char *a, const char *b) {
    const size_t la = strlen(a), lb = strlen(b);
    char *out = malloc(la + lb + 1);
    if (!out) return NULL;
    memcpy(out, a, la);
    memcpy(out + la, b, lb + 1);
    return out;
}

int read_file(const char *path, unsigned char **buf, size_t *length) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;
    fseek(fp, 0, SEEK_END);
    const long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (n < 0) {
        fclose(fp);
        return -1;
    }
    *buf = malloc((size_t) n + 1);
    if (!*buf) {
        fclose(fp);
        return -1;
    }
    *length = fread(*buf, 1, (size_t) n, fp);
    (*buf)[*length] = 0;
    fclose(fp);
    return *length == (size_t) n ? 0 : -1;
}

void asset_prefetch_wait(const char *const *paths, size_t count) {
    (void) paths;
    (void) count;
}
