// Krunker v10.0.2 ranked rules (shared/include/ranked.h, doc/v10/ranked-impl.md).
#include <ranked.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

const int g_ranked_classes[RANKED_CLASS_COUNT] = {0, 1, 2, 3, 4, 5, 6, 8, 12, 13, 15};

int ranked_class_allowed(const int class_index) {
    for (int i = 0; i < RANKED_CLASS_COUNT; i++) {
        if (g_ranked_classes[i] == class_index) return 1;
    }
    return 0;
}

// ------------------------------------------------------------------ v7.1.4 Elo (history, seed)

double ranked_expected(const double rating, const double opponents) {
    return 1.0 / (1.0 + pow(10.0, (opponents - rating) / RANKED_ELO_DIVISOR));
}

double ranked_delta(const double rating, const double opponents, const double score, const int abandoned, const double k) {
    double delta = abandoned ? -RANKED_DEDUCTION : k * (score - ranked_expected(rating, opponents));
    if (rating + delta < 0.0) delta = -rating; // floored at 0
    return delta;
}

int ranked_rank_index(const double rating) {
    int level = (int) floor((rating > 0.0 ? rating : 0.0) / RANKED_ELO_PER);
    if (level > RANKED_MAX_LEVEL) level = RANKED_MAX_LEVEL;
    if (level < 1) level = 1;
    return level - 1;
}

int ranked_rank_number(const double rating) {
    return ranked_rank_index(rating) + 1;
}

// ------------------------------------------------------------------ ranks (v10 enum + tables)

static const struct {
    const char *name, *color, *icon;
    int min; // display MMR
} k_ranks[RANK_COUNT] = {
    {"Unranked", "#AAAAAA", "img/ranked/ranks/rank_unranked.svg", -1},
    {"Bronze 1", "#CD7F32", "img/ranked/ranks/rank_bronze.svg", 0},
    {"Bronze 2", "#CD7F32", "img/ranked/ranks/rank_bronze.svg", 200},
    {"Bronze 3", "#CD7F32", "img/ranked/ranks/rank_bronze.svg", 400},
    {"Silver 1", "#C0C0C0", "img/ranked/ranks/rank_silver.svg", 700},
    {"Silver 2", "#C0C0C0", "img/ranked/ranks/rank_silver.svg", 900},
    {"Silver 3", "#C0C0C0", "img/ranked/ranks/rank_silver.svg", 1100},
    {"Gold 1", "#FFD700", "img/ranked/ranks/rank_gold.svg", 1300},
    {"Gold 2", "#FFD700", "img/ranked/ranks/rank_gold.svg", 1600},
    {"Gold 3", "#FFD700", "img/ranked/ranks/rank_gold.svg", 2000},
    {"Platinum", "#41bdea", "img/ranked/ranks/rank_platinum.svg", 2300},
    {"Diamond", "#B9F2FF", "img/ranked/ranks/rank_diamond.svg", 3000},
    {"Master", "#FF8C00", "img/ranked/ranks/rank_master.svg", 3300},
    {"Kracked", "#FF323A", "img/ranked/ranks/rank_kracked.svg", 4700},
};

const char *ranked_rank_name(const int rank) { return rank >= 0 && rank < RANK_COUNT ? k_ranks[rank].name : "Unknown"; }
const char *ranked_rank_color(const int rank) { return rank >= 0 && rank < RANK_COUNT ? k_ranks[rank].color : "#FFFFFF"; }
const char *ranked_rank_icon(const int rank) { return k_ranks[rank >= 0 && rank < RANK_COUNT ? rank : 0].icon; }
int ranked_rank_min(const int rank) { return rank >= 0 && rank < RANK_COUNT ? k_ranks[rank].min : -1; }

double ranked_conservative(const RankedSkill s) { return s.mu - 3.0 * s.sigma; }

int ranked_display(const double conservative) { return 1000 + (int) lround(100.0 * conservative); }

int ranked_rank_of(const double conservative, const int matches) {
    if (matches < RANKED_PLACEMENT_MATCHES) return RANK_UNRANKED;
    const int mmr = ranked_display(conservative);
    int rank = RANK_BRONZE1;
    for (int r = RANK_BRONZE1; r < RANK_COUNT; r++) {
        if (mmr >= k_ranks[r].min) rank = r;
    }
    return rank;
}

// ------------------------------------------------------------------ TrueSkill (two teams)

static double pdf(const double x) { return exp(-0.5 * x * x) / sqrt(2.0 * M_PI); }
static double cdf(const double x) { return 0.5 * erfc(-x / sqrt(2.0)); }

// v / w of the win case (t = performance difference / c, e = margin / c)
static void win_vw(const double t, const double e, double *v, double *w) {
    const double x = t - e;
    double denom = cdf(x);
    if (denom < 1e-12) { // far in the tail: the asymptote v -> -x
        *v = -x;
        *w = 1.0;
        return;
    }
    *v = pdf(x) / denom;
    *w = *v * (*v + x);
}

static void draw_vw(const double t, const double e, double *v, double *w) {
    const double a = e - t, b = -e - t;
    const double denom = cdf(a) - cdf(b);
    if (denom < 1e-12) {
        *v = t < 0 ? -t - e : -t + e;
        *w = 1.0;
        return;
    }
    *v = (pdf(b) - pdf(a)) / denom;
    *w = *v * *v + (a * pdf(a) - b * pdf(b)) / denom;
}

void ranked_update(RankedSkill *skills, const int *team, const int *abandoned, const int n, const int winner) {
    if (!skills || !team || n <= 0 || n > 32) return;
    double mu[2] = {0, 0}, var = 0;
    int count[2] = {0, 0};
    for (int i = 0; i < n; i++) {
        const int t = team[i] ? 1 : 0;
        skills[i].sigma = sqrt(skills[i].sigma * skills[i].sigma + RANKED_TAU * RANKED_TAU); // dynamics
        mu[t] += skills[i].mu;
        var += skills[i].sigma * skills[i].sigma;
        count[t]++;
    }
    if (!count[0] || !count[1]) return; // a one-sided "match" rates nobody
    const double c = sqrt(var + n * RANKED_BETA * RANKED_BETA);
    const double margin = RANKED_DRAW_Z * sqrt((double) n) * RANKED_BETA;
    // order: team `first` is the winner (team 0 for a draw)
    const int first = winner == 1 ? 1 : 0;
    const double t = (mu[first] - mu[1 - first]) / c, e = margin / c;
    double v, w;
    if (winner == 0 || winner == 1) win_vw(t, e, &v, &w);
    else draw_vw(t, e, &v, &w);
    /* an abandoner is rated as a loss of its own team (whatever the result), keeps its sigma
     * (no information gained) and loses RANKED_ABANDON_MU more: always a net loss */
    double lv[2], lw[2];
    for (int t = 0; t < 2; t++) win_vw((mu[1 - t] - mu[t]) / c, e, &lv[t], &lw[t]);
    for (int i = 0; i < n; i++) {
        const int tm = team[i] ? 1 : 0;
        const double s2 = skills[i].sigma * skills[i].sigma;
        if (abandoned && abandoned[i]) {
            skills[i].mu -= s2 / c * lv[tm] + RANKED_ABANDON_MU;
            continue;
        }
        const double sign = tm == first ? 1.0 : -1.0;
        skills[i].mu += sign * s2 / c * v;
        double f = 1.0 - s2 / (c * c) * w;
        if (f < 1e-4) f = 1e-4;
        skills[i].sigma = sqrt(s2 * f);
    }
}

RankedSkill ranked_decay(RankedSkill s, const double idle_days) {
    if (idle_days > RANKED_DECAY_DAYS) {
        const double days = idle_days - RANKED_DECAY_DAYS;
        s.sigma = sqrt(s.sigma * s.sigma + days * RANKED_TAU * RANKED_TAU * 25.0);
        if (s.sigma > RANKED_SIGMA0) s.sigma = RANKED_SIGMA0;
    }
    return s;
}

RankedSkill ranked_season_seed(const RankedSkill *previous, const int v7_elo) {
    RankedSkill s = ranked_skill_default();
    if (previous) {
        s.mu = RANKED_MU0 + (previous->mu - RANKED_MU0) * 0.5;
        s.sigma = previous->sigma > RANKED_SIGMA0 * 2.0 / 3.0 ? previous->sigma : RANKED_SIGMA0 * 2.0 / 3.0;
    } else if (v7_elo >= 0) { // a v7 ranked player (0201/0204 history)
        // v7 Elo 0 ... ~1000 (eloPer 120 ranks): +1 mu per 60 Elo, at most +10, sigma 2/3 sigma0
        double bonus = v7_elo / 60.0;
        if (bonus > 10.0) bonus = 10.0;
        s.mu = RANKED_MU0 + bonus;
        s.sigma = RANKED_SIGMA0 * 2.0 / 3.0;
    }
    return s;
}

// ------------------------------------------------------------------ seasons + map pool

int ranked_season_at(const int64_t unix_s) {
    const time_t t = (time_t) unix_s;
    struct tm tm;
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    const int year = tm.tm_year + 1900, month = tm.tm_mon + 1;
    if (year < RANKED_SEASON_START_YEAR || (year == RANKED_SEASON_START_YEAR && month < RANKED_SEASON_START_MONTH)) return 0;
    return 12 * (year - RANKED_SEASON_START_YEAR) + (month - RANKED_SEASON_START_MONTH + 1);
}

const int g_ranked_map_pool[RANKED_POOL_COUNT] = {2, 4, 11, 14, 17, 39, 42};
static const char *const k_pool_names[RANKED_POOL_COUNT] = {"Sandstorm", "Undergrowth", "Industry", "Site", "Bureau",
                                                            "Eterno Simulator", "Frontier"};

const char *ranked_pool_map_name(const int map_index) {
    for (int i = 0; i < RANKED_POOL_COUNT; i++) {
        if (g_ranked_map_pool[i] == map_index) return k_pool_names[i];
    }
    return NULL;
}

static int cmp_int(const void *a, const void *b) { return *(const int *) a - *(const int *) b; }

void ranked_season_maps(const int season, int out[RANKED_SEASON_MAPS]) {
    int pool[RANKED_POOL_COUNT];
    int len = 0;
    for (int i = 0; i < RANKED_POOL_COUNT; i++) {
        if (g_ranked_map_pool[i] != RANKED_ALWAYS_MAP) pool[len++] = g_ranked_map_pool[i];
    }
    qsort(pool, (size_t) len, sizeof(int), cmp_int);
    // x = 2654435761 * (season + 1) mod 2^32 (exact in JS doubles), then the LCG
    uint32_t x = (uint32_t) ((2654435761ull * (uint64_t) (uint32_t) (season + 1)) & 0xffffffffull);
    for (int i = 0; i < RANKED_ROTATION_SLOTS && i < len; i++) {
        x = (uint32_t) ((1664525ull * x + 1013904223ull) & 0xffffffffull);
        const int j = i + (int) (x % (uint32_t) (len - i));
        const int tmp = pool[i];
        pool[i] = pool[j];
        pool[j] = tmp;
    }
    out[0] = RANKED_ALWAYS_MAP;
    for (int i = 0; i < RANKED_ROTATION_SLOTS; i++) out[i + 1] = pool[i];
    qsort(out, RANKED_SEASON_MAPS, sizeof(int), cmp_int);
}

// ------------------------------------------------------------------ Ranked Points

int64_t ranked_season_points(const double conservative, const int matches, const int season) {
    const int64_t mmr = ranked_display(conservative);
    if (mmr <= 0) return 0;
    if (season == 0) return mmr;                              // Beta Season: 1:1
    if (matches < RANKED_PLACEMENT_MATCHES) return 0;
    if (matches >= RANKED_MAX_MATCHES) return mmr;            // 100 matches = 100%
    return mmr * matches / RANKED_MAX_MATCHES;                // floor(MMR * matches / 100), exact
}

int ranked_match_points(const int reward) {
    int p = reward / 2;
    return p < 0 ? 0 : p > 60 ? 60 : p;
}

const RankedMilestone g_ranked_milestones[RANKED_MILESTONE_COUNT] = {
    {10, 100}, {25, 200}, {50, 350}, {75, 500}, {100, 750},
};

// ------------------------------------------------------------------ regions

static const struct {
    const char *code, *name;
    int index;
} k_regions[RANKED_REGION_COUNT] = {{"as", "Asia", 2}, {"eu", "Europe", 3}, {"na", "North America", 4}};

const char *ranked_region_code(const int index) {
    for (int i = 0; i < RANKED_REGION_COUNT; i++) {
        if (k_regions[i].index == index) return k_regions[i].code;
    }
    return NULL;
}

int ranked_region_index(const char *code) {
    if (!code || !*code) return -1;
    if (code[0] >= '0' && code[0] <= '9') return ranked_region_code(atoi(code)) ? atoi(code) : -1;
    for (int i = 0; i < RANKED_REGION_COUNT; i++) {
        if (!strcmp(k_regions[i].code, code)) return k_regions[i].index;
    }
    return -1;
}

const char *ranked_region_name(const char *code) {
    const int index = ranked_region_index(code);
    for (int i = 0; i < RANKED_REGION_COUNT; i++) {
        if (k_regions[i].index == index) return k_regions[i].name;
    }
    return "";
}
