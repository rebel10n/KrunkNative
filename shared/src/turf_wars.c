// Krunker v10.0.2 Turf Wars rules (shared/include/turf_wars.h).
#include <turf_wars.h>
#include <math.h>
#include <string.h>
#include <strings.h>

const int g_turf_maps[TURF_MAP_COUNT] = {0, 2, 3};
const double g_turf_split[TURF_SPLIT_COUNT] = {0.6, 0.3, 0.1};
static const char *const k_names[TURF_MAP_COUNT] = {"Burg", "Sandstorm", "Subzero"};

const char *turf_map_name(const int map_index) {
    for (int i = 0; i < TURF_MAP_COUNT; i++) {
        if (g_turf_maps[i] == map_index) return k_names[i];
    }
    return 0;
}

int turf_day_at(const int64_t unix_s) {
    if (unix_s < TURF_INTRO_UNIX) return 0;
    return (int) ((unix_s - TURF_INTRO_UNIX) / 86400);
}

int turf_game_eligible(const int map_index, const int mode_id, const int pub_game, const int custom_map, const int ranked) {
    if (!pub_game || custom_map || ranked) return 0;
    if (!turf_map_name(map_index)) return 0;
    return mode_id == 0 || mode_id == 1; // iïiíiïì = [0, 1]: FFA, TDM
}

int turf_kill_counts(const char *killer_clan, const int killer_is_bot, const int killer_has_account, const char *victim_clan) {
    if (killer_is_bot || !killer_has_account || !killer_clan || !killer_clan[0]) return 0;
    return !victim_clan || !victim_clan[0] || strcasecmp(killer_clan, victim_clan) != 0;
}

int64_t turf_cut(const int64_t pool, const int place) {
    if (place < 0 || place >= TURF_SPLIT_COUNT || pool <= 0) return 0;
    return (int64_t) floor((double) pool * g_turf_split[place]);
}

static const char *const k_modes[TURF_LOOT_COUNT] = {"clan_bank", "even", "top_weighted", "performance", "top_performance"};

TurfLoot turf_loot_default(void) {
    TurfLoot l = {TURF_LOOT_CLAN_BANK, 0, 20, 80, 20, 1};
    return l;
}

int turf_loot_mode(const char *name) {
    for (int i = 0; name && i < TURF_LOOT_COUNT; i++) {
        if (!strcmp(k_modes[i], name)) return i;
    }
    return -1;
}

const char *turf_loot_mode_name(const int mode) { return mode >= 0 && mode < TURF_LOOT_COUNT ? k_modes[mode] : k_modes[0]; }

static int clampi(const int v, const int lo, const int hi) { return v < lo ? lo : v > hi ? hi : v; }

void turf_loot_clamp(TurfLoot *l) {
    if (l->mode < 0 || l->mode >= TURF_LOOT_COUNT) l->mode = TURF_LOOT_CLAN_BANK;
    l->clan_pct = clampi(l->clan_pct, 0, 100);
    l->top_player_count = clampi(l->top_player_count, 1, TURF_TOP_CONTRIBUTORS);
    l->top_player_pct = clampi(l->top_player_pct, 0, 100);
    l->remainder_pct = clampi(l->remainder_pct, 0, 100 - l->top_player_pct);
    l->min_games = clampi(l->min_games, 0, 1000);
}

int64_t turf_loot_split(const TurfLoot *loot, const int64_t cut, const int *kills, const int count, int64_t *out) {
    for (int i = 0; i < count; i++) out[i] = 0;
    if (cut <= 0) return 0;
    if (!loot || loot->mode == TURF_LOOT_CLAN_BANK || count <= 0) return cut;
    int64_t bank = (int64_t) floor((double) cut * loot->clan_pct / 100.0);
    const int64_t players = cut - bank;
    int64_t given = 0;
    switch (loot->mode) {
        case TURF_LOOT_EVEN:
            for (int i = 0; i < count; i++) given += out[i] = players / count;
            break;
        case TURF_LOOT_TOP_WEIGHTED: {
            const int top = count < loot->top_player_count ? count : loot->top_player_count;
            const int rest = count - top;
            const int64_t top_pool = (int64_t) floor((double) players * loot->top_player_pct / 100.0);
            const int64_t rest_pool = rest ? (int64_t) floor((double) players * loot->remainder_pct / 100.0) : 0;
            for (int i = 0; i < top; i++) given += out[i] = top_pool / top;
            for (int i = top; i < count; i++) given += out[i] = rest_pool / rest;
            break;
        }
        case TURF_LOOT_PERFORMANCE:
        case TURF_LOOT_TOP_PERFORMANCE: {
            const int n = loot->mode == TURF_LOOT_TOP_PERFORMANCE && count > loot->top_player_count ? loot->top_player_count : count;
            int64_t total = 0;
            for (int i = 0; i < n; i++) total += kills[i] > 0 ? kills[i] : 0;
            for (int i = 0; i < n && total > 0; i++)
                given += out[i] = (int64_t) floor((double) players * (kills[i] > 0 ? kills[i] : 0) / (double) total);
            break;
        }
        default:
            break;
    }
    return cut - given; // the clan bank keeps its cut plus every rounding remainder
}
