// Krunker Strike's map pool (shared/include/strike_maps.h, doc/v10/krunker-strike-maps.md).
//
// The seven pool maps are Krunker v10.0.2's own official maps: each entry names its v10 file under
// assets/maps/official/ (krunker_maps.h, pinned in tools/krunker/maps.lock.json), which is also the file
// the base catalogue loads for that index. Krunker has one v10 file per map name, so each pool map's
// latest version is that file. The names and the rotation order are FRVR Krunker Strike's (build 1.37.2
// mapPool: config.maps gets mapWeight copies of each map, then a uniform pick). The weights are server
// data we do not have, so every map weighs 1.

#include <strike_maps.h>

#include <ctype.h>
#include <string.h>

#define STRIKE_MAP(key_, name_, file_, index_) {key_, name_, file_, "maps/official/" file_ ".json", index_, 1}

static const StrikeMap k_pool[] = {
    STRIKE_MAP("burg", "Burg", "00_burg", 0),
    STRIKE_MAP("sandstorm", "Sandstorm", "02_sandstorm", 2),
    STRIKE_MAP("subzero", "Subzero", "03_subzero", 3),
    STRIKE_MAP("lostworld", "Lostworld", "07_lostworld", 7),
    STRIKE_MAP("oasis", "Oasis", "09_oasis", 9),
    STRIKE_MAP("skytemple", "SkyTemple", "15_skytemple", 15),
    STRIKE_MAP("habitat", "Habitat", "22_habitat", 22),
};

#define POOL_COUNT (sizeof(k_pool) / sizeof(k_pool[0]))

// Krunker Strike's copy of the controls tutorial's sandbox (FRVR 1.37.2's maps/bot_ftue_v2.json, the file
// the build manifest pins). Krunker v10.0.2 has no official file for it (its bundle names it only in an
// isBotFTUE branch and is not in the official map list), so it stays under assets/maps/strike/. Outside
// the pool: no rotation (weight 0) and no catalogue index.
static const StrikeMap k_ftue_map = {"bot_ftue_v2", "Mini Industry Tutorial", "bot_ftue_v2", "maps/strike/bot_ftue_v2.json", -1, 0};

const StrikeMap *strike_ftue_map(void) { return &k_ftue_map; }

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
