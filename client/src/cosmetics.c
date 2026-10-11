#include <cosmetics.h>

const Cosmetic *cosmetic_get(const int store_index, const CosmeticType type) {
    if (store_index < 0) return NULL;

    size_t lo = 0, hi = g_cosmetic_count;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (g_cosmetics[mid].index < store_index) lo = mid + 1;
        else hi = mid;
    }

    if (lo >= g_cosmetic_count || g_cosmetics[lo].index != store_index) return NULL;
    return g_cosmetics[lo].type == (int) type ? &g_cosmetics[lo] : NULL;
}
