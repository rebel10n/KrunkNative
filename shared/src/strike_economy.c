// Krunker Strike's economy rules over the generated tables (shared/include/strike_economy.h).
#include <strike_economy.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

const char *strike_currency_key(const StrikeCurrency c) {
    return c >= 0 && c < STRIKE_CUR_COUNT ? strike_economy()->currency_keys[c] : NULL;
}

StrikeCurrency strike_currency_from_key(const char *key) {
    for (int c = 0; key && c < STRIKE_CUR_COUNT; c++)
        if (!strcmp(strike_economy()->currency_keys[c], key)) return (StrikeCurrency) c;
    return STRIKE_CUR_COUNT;
}

int strike_item_index(const char *id) {
    const StrikeEconomy *e = strike_economy();
    for (int i = 0; id && i < e->item_count; i++)
        if (!strcmp(e->items[i].id, id)) return i;
    return -1;
}

const StrikeItem *strike_item(const char *id) {
    const int i = strike_item_index(id);
    return i >= 0 ? &strike_economy()->items[i] : NULL;
}

const StrikeItem *strike_item_by_skin(const int skin) {
    const StrikeEconomy *e = strike_economy();
    for (int i = 0; skin >= 0 && i < e->item_count; i++)
        if (e->items[i].skin == skin) return &e->items[i];
    return NULL;
}

const StrikeItem *strike_default_item(const int type, const int weapon) {
    const StrikeEconomy *e = strike_economy();
    for (int i = 0; i < e->item_count; i++) {
        const StrikeItem *it = &e->items[i];
        if (it->is_default && it->type == type && it->type != STRIKE_ITEM_CLASS && (type != STRIKE_ITEM_WEAPON || it->weapon == weapon))
            return it;
    }
    return NULL;
}

#define FIND(fn, T, arr, n, field)                      \
    const T *fn(const char *id) {                       \
        const StrikeEconomy *e = strike_economy();      \
        for (int i = 0; id && i < e->n; i++)            \
            if (!strcmp(e->arr[i].field, id)) return &e->arr[i]; \
        return NULL;                                    \
    }
FIND(strike_pool, StrikePool, pools, pool_count, id)
FIND(strike_wheel, StrikeWheel, wheels, wheel_count, id)
FIND(strike_product, StrikeProduct, products, product_count, id)
FIND(strike_mission, StrikeMission, missions, mission_count, id)
FIND(strike_track_slot, StrikeTrackSlot, track, track_count, slot_id)
#undef FIND

const StrikeTrackSlot *strike_track_level(const int level) {
    const StrikeEconomy *e = strike_economy();
    for (int i = 0; i < e->track_count; i++)
        if (e->track[i].level == level) return &e->track[i];
    return NULL;
}

const StrikeRarity *strike_rarity(const int rarity) {
    return rarity >= 0 && rarity < STRIKE_RARITY_COUNT ? &strike_economy()->rarities[rarity] : NULL;
}

int strike_item_slot(const StrikeItem *item, char *out, const size_t size) {
    if (!item || !out || size < 8) return 0;
    switch (item->type) {
    case STRIKE_ITEM_WEAPON:
        if (item->weapon < 0) return 0;
        snprintf(out, size, "w%d", item->weapon);
        return 1;
    case STRIKE_ITEM_HAT: case STRIKE_ITEM_BACK: case STRIKE_ITEM_MELEE: case STRIKE_ITEM_SPRAY: case STRIKE_ITEM_DYE:
    case STRIKE_ITEM_WAIST: case STRIKE_ITEM_FACE: case STRIKE_ITEM_SHOE: case STRIKE_ITEM_PET: case STRIKE_ITEM_WRIST:
    case STRIKE_ITEM_CHARM:
        snprintf(out, size, "t%d", item->type);
        return 1;
    default:
        return 0;
    }
}

// ---------------------------------------------------------------- levels (the bundle's LA / B9)

static int64_t level_cost(const int level) {
    const StrikeEconomy *e = strike_economy();
    return (int64_t) e->xp_base + (int64_t) e->xp_step * (int64_t) floor((double) level / e->xp_divisor);
}

// LA(xp): {let e=1,t=0,n=0; for(;t<=i;){n=t; const o=100+100*floor(e/3.5); t+=o, t<=i&&e++} ...}
StrikeLevel strike_level_for_xp(int64_t xp) {
    if (xp < 0) xp = 0;
    int level = 1;
    int64_t total = 0, start = 0;
    while (total <= xp) {
        start = total;
        total += level_cost(level);
        if (total <= xp) level++;
    }
    return (StrikeLevel) {.level = level, .xp_in_level = xp - start, .xp_to_next = total - start, .total_needed = total};
}

int64_t strike_xp_for_level(const int level) {
    int64_t xp = 0;
    for (int l = 1; l < level; l++) xp += level_cost(l);
    return xp;
}

// ---------------------------------------------------------------- rolls

int strike_wheel_slice_for_roll(const double *weights, const int count, double roll) {
    double total = 0.0;
    int last = -1;
    for (int i = 0; i < count; i++)
        if (weights[i] > 0.0) total += weights[i], last = i;
    if (last < 0) return -1;
    if (roll < 0.0) roll = 0.0;
    double acc = 0.0;
    const double target = roll * total;
    for (int i = 0; i < count; i++) {
        if (weights[i] <= 0.0) continue;
        acc += weights[i];
        if (target < acc) return i;
    }
    return last; // roll at the top edge (float sums)
}

static int pool_pick(const StrikePool *pool, const double pick) {
    if (!pool || pool->count <= 0) return -1;
    int k = (int) (pick * pool->count);
    if (k < 0) k = 0;
    if (k >= pool->count) k = pool->count - 1;
    return pool->items[k];
}

int strike_wheel_spin(const StrikeWheel *wheel, const int count, const double *rolls, const double *picks, int *out_items,
                      int *out_slices) {
    if (!wheel || count < 1 || count > 10 || !rolls || !picks || !out_items || !out_slices || wheel->slice_count <= 0) return 0;
    double weights[16];
    const int n = wheel->slice_count < 16 ? wheel->slice_count : 16;
    for (int i = 0; i < n; i++) {
        const StrikePool *p = strike_pool(wheel->slices[i].pool);
        weights[i] = p && p->count > 0 ? wheel->slices[i].weight : 0.0;
    }
    for (int r = 0; r < count; r++) {
        const int s = strike_wheel_slice_for_roll(weights, n, rolls[r]);
        if (s < 0) return 0;
        out_slices[r] = s;
        out_items[r] = pool_pick(strike_pool(wheel->slices[s].pool), picks[r]);
    }
    // guaranteedIfSpinCountIs: a spin of that many rolls holds one in the slice or a rarer one
    for (int g = 0; g < n; g++) {
        if (wheel->slices[g].guaranteed_if != count || weights[g] <= 0.0) continue;
        const StrikePool *gp = strike_pool(wheel->slices[g].pool);
        int met = 0;
        for (int r = 0; r < count && !met; r++) {
            const StrikePool *p = strike_pool(wheel->slices[out_slices[r]].pool);
            met = p && gp && p->rarity >= gp->rarity;
        }
        if (!met) {
            out_slices[count - 1] = g;
            out_items[count - 1] = pool_pick(gp, picks[count - 1]);
        }
    }
    return count;
}

int strike_golden_meter_add(int *count, const int spins) {
    const int meter = strike_economy()->golden_spin_meter;
    if (!count || meter <= 0 || spins <= 0) return 0;
    *count += spins;
    const int tickets = *count / meter;
    *count %= meter;
    return tickets;
}

// ---------------------------------------------------------------- daily picks

uint64_t strike_hash(const char *s, uint64_t seed) {
    uint64_t h = 1469598103934665603ULL ^ seed;
    for (; s && *s; s++) h = (h ^ (unsigned char) *s) * 1099511628211ULL;
    return h;
}

static uint64_t splitmix(uint64_t *x) {
    uint64_t z = (*x += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

int strike_day(const int64_t epoch_ms) { return (int) (epoch_ms >= 0 ? epoch_ms / 86400000LL : (epoch_ms - 86399999LL) / 86400000LL); }

int strike_daily_missions(const int day, const int64_t account_id, int *out, int n) {
    const StrikeEconomy *e = strike_economy();
    if (!out || n <= 0) return 0;
    if (n > e->mission_count) n = e->mission_count;
    uint64_t state = ((uint64_t) (uint32_t) day << 32) ^ (uint64_t) account_id ^ 0x5354524B4D49ULL; // "STRKMI"
    int taken[64] = {0}, got = 0;
    while (got < n) {
        int total = 0;
        for (int i = 0; i < e->mission_count && i < 64; i++)
            if (!taken[i]) total += e->missions[i].weight > 0 ? e->missions[i].weight : 0;
        if (total <= 0) break;
        int r = (int) (splitmix(&state) % (uint64_t) total);
        for (int i = 0; i < e->mission_count && i < 64; i++) {
            if (taken[i] || e->missions[i].weight <= 0) continue;
            if (r < e->missions[i].weight) {
                taken[i] = 1;
                out[got++] = i;
                break;
            }
            r -= e->missions[i].weight;
        }
    }
    return got;
}

static int deal_candidate(const StrikeItem *it) {
    return it->spinnable && it->type == STRIKE_ITEM_WEAPON && it->rarity >= 1 && it->rarity <= 4;
}

int strike_daily_deals(const int day, int *out, int n) {
    const StrikeEconomy *e = strike_economy();
    int candidates = 0;
    for (int i = 0; i < e->item_count; i++) candidates += deal_candidate(&e->items[i]);
    if (!out || n <= 0 || candidates <= 0) return 0;
    if (n > candidates) n = candidates;
    uint64_t state = ((uint64_t) (uint32_t) day << 32) ^ 0x5354524B444CULL; // "STRKDL"
    int got = 0;
    while (got < n) {
        int k = (int) (splitmix(&state) % (uint64_t) candidates);
        int idx = -1;
        for (int i = 0; i < e->item_count; i++) {
            if (!deal_candidate(&e->items[i])) continue;
            if (k-- == 0) {
                idx = i;
                break;
            }
        }
        int dup = 0;
        for (int j = 0; j < got; j++) dup |= out[j] == idx;
        if (!dup && idx >= 0) out[got++] = idx;
    }
    return got;
}
