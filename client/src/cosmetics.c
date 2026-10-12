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
    const int item_type = g_cosmetics[lo].type;
    // v7 HEAD / BACK items take the slot of the type their meshes come from (assetType). v7 has
    // separate head and back slots; until the player model has them, they share the hat / back slot.
    const int slot_type = item_type == COSMETIC_V7_HEAD ? COSMETIC_HAT : item_type == COSMETIC_V7_BACK ? COSMETIC_BACK : item_type;
    return slot_type == (int) type ? &g_cosmetics[lo] : NULL;
}
