// Krunker Strike's map pool (shared/include/strike_maps.h, doc/v10/krunker-strike-maps.md).
//
// Source: FRVR Krunker Strike build 1.37.2 (/home/user/src/KrunkerStrike_v1.37.2/, text only): the
// map table `xh` in public/assets/index-Cz2uYXJQ.deob.js ({name, fileName}, Krunker v7's maps.js
// order) and its round setup, which fills config.maps from gameConfig.remoteFeatures.mapPool by
// mapName and mapWeight. The pool's names are the seven maps FRVR ships for Strike; the weights are
// server data we do not have, so every map weighs 1.

#include <strike_maps.h>

#include <ctype.h>
#include <string.h>

#define STRIKE_MAP(key_, name_, file_, index_) {key_, name_, file_, "maps/strike/" key_ ".json", index_, 1}

static const StrikeMap k_pool[] = {
    STRIKE_MAP("burg", "Burg", "burg_new", 0),
    STRIKE_MAP("sandstorm", "Sandstorm", "sandstorm_v3", 2),
    STRIKE_MAP("subzero", "Subzero", "subzero_new", 3),
    STRIKE_MAP("lostworld", "Lostworld", "lostworld_s6", 7),
    STRIKE_MAP("oasis", "Oasis", "oasis_v3", 9),
    STRIKE_MAP("skytemple", "SkyTemple", "skytemple", 15),
    STRIKE_MAP("habitat", "Habitat", "habitat", 22),
};

#define POOL_COUNT (sizeof(k_pool) / sizeof(k_pool[0]))

size_t strike_map_pool(const StrikeMap **out) {
    if (out) *out = k_pool;
    return POOL_COUNT;
}

size_t strike_map_pool_expand(const StrikeMap *pool, const size_t count, int *out, const size_t max) {
    size_t n = 0;
    for (size_t i = 0; pool && i < count; i++) {
        for (int k = 0; k < pool[i].weight; k++, n++) {
            if (out && n < max) out[n] = pool[i].catalog_index;
        }
    }
    return out && n > max ? max : n;
}

size_t strike_map_rotation(int *out, const size_t max) {
    return strike_map_pool_expand(k_pool, POOL_COUNT, out, max);
}

const StrikeMap *strike_map_for_catalog(const size_t index) {
    for (size_t i = 0; i < POOL_COUNT; i++) {
        if ((size_t) k_pool[i].catalog_index == index) return &k_pool[i];
    }
    return NULL;
}

static int equal_ci(const char *a, const char *b) {
    for (; *a && tolower((unsigned char) *a) == tolower((unsigned char) *b); a++, b++) {}
    return !*a && !*b;
}

const StrikeMap *strike_map_by_key(const char *key) {
    if (!key || !key[0]) return NULL;
    for (size_t i = 0; i < POOL_COUNT; i++) {
        if (equal_ci(k_pool[i].key, key) || equal_ci(k_pool[i].name, key)) return &k_pool[i];
    }
    return NULL;
}
