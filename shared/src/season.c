// Krunker v7.1.4 seasons and the Battle Pass. See shared/include/season.h.
#include <season.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int bp_parse_items(BattlePass *bp, const char *itemdata) {
    if (!bp) return 0;
    bp->count = 0;
    if (!itemdata) return 0;
    // UTILS.arrayChunk(itemdata.split(","), 4): [id, tier, col, sk]
    int group[4], n = 0;
    const char *p = itemdata;
    while (*p) {
        char *end = NULL;
        const long v = strtol(p, &end, 10);
        if (end == p) { // empty / junk field: parseInt -> NaN; the group is dropped
            while (*p && *p != ',') p++;
            group[n++] = -1;
        } else {
            group[n++] = (int) v;
            p = end;
            while (*p && *p != ',') p++;
        }
        if (*p == ',') p++;
        if (n == 4) {
            if (group[0] > 0 && (group[1] == 0 || group[1] == 1) && group[2] >= 0 && group[3] >= 0 && bp->count < BP_ITEMS_MAX) {
                BattlePassItem *it = &bp->items[bp->count++];
                it->id = group[0];
                it->tier = group[1];
                it->level = group[2];
                it->skin = group[3];
                it->kr = 0;
                it->jnk = 0.0;
            }
            n = 0;
        }
    }
    return bp->count;
}

size_t bp_format_items(const BattlePass *bp, char *out, const size_t size) {
    if (!out || !size) return 0;
    out[0] = 0;
    size_t len = 0;
    for (int i = 0; bp && i < bp->count; i++) {
        const BattlePassItem *it = &bp->items[i];
        const int w = snprintf(out + len, size - len, "%s%d,%d,%d,%d", i ? "," : "", it->id, it->tier, it->level, it->skin);
        if (w < 0 || (size_t) w >= size - len) {
            out[len] = 0;
            return len;
        }
        len += (size_t) w;
    }
    return len;
}

// "a:b,a:b" pairs (v10 leveldata / krdata / jnkdata: String(x).split(",") then split(":"))
static int next_pair(const char **pp, long *a, double *b) {
    const char *p = *pp;
    while (p && *p) {
        char *end = NULL;
        const long k = strtol(p, &end, 10);
        int ok = end != p && *end == ':';
        double v = 0.0;
        if (ok) {
            const char *q = end + 1;
            v = strtod(q, &end);
            ok = end != q;
        }
        while (*p && *p != ',') p++;
        if (*p == ',') p++;
        if (ok) {
            *pp = p;
            *a = k;
            *b = v;
            return 1;
        }
    }
    *pp = p;
    return 0;
}

void bp_parse_levels(BattlePass *bp, const char *leveldata) {
    if (!bp) return;
    memset(bp->level_bpp, 0, sizeof(bp->level_bpp));
    const char *p = leveldata;
    long level;
    double need;
    // _52102: level >= 1 && level <= itemcount && bpp >= 0
    while (next_pair(&p, &level, &need))
        if (level >= 1 && level <= bp->item_count && level <= BP_LEVELS_MAX && need >= 0) bp->level_bpp[level - 1] = (int) need > 0 ? (int) need : -1;
}

void bp_parse_rewards(BattlePass *bp, const char *krdata, const char *jnkdata) {
    if (!bp) return;
    for (int pass = 0; pass < 2; pass++) {
        const char *p = pass ? jnkdata : krdata;
        long id;
        double v;
        while (next_pair(&p, &id, &v)) {
            for (int i = 0; i < bp->count; i++) {
                if (bp->items[i].id != id) continue;
                if (pass) bp->items[i].jnk = v;      // parseFloat
                else bp->items[i].kr = (int) v;     // parseInt
            }
        }
    }
}

int bp_level_need(const BattlePass *bp, const int level) {
    if (!bp || level < 1) return 0;
    const int own = level <= BP_LEVELS_MAX ? bp->level_bpp[level - 1] : 0;
    return own > 0 ? own : own < 0 ? 0 : bp->bpp_per_level > 0 ? bp->bpp_per_level : 0; // -1: a free level (leveldata "n:0")
}

int bp_item_has_reward(const BattlePassItem *it) { return it && (it->skin > 0 || it->kr > 0 || it->jnk > 0.0); }

int bp_level(const BattlePass *bp, int64_t bpp, int64_t *points, int *complete) {
    const int count = bp && bp->item_count > 0 ? bp->item_count : 1;
    if (bpp < 0) bpp = 0;
    // _52111: the first level whose cost the remaining bpp does not cover; past the last: maxed
    int64_t before = 0;
    for (int level = 1; level <= count; level++) {
        const int need = bp ? bp_level_need(bp, level) : 1;
        if (bpp < before + need) {
            if (points) *points = bpp - before;
            if (complete) *complete = 0;
            return level;
        }
        before += need;
    }
    if (points) *points = bp ? bp_level_need(bp, count) : 1;
    if (complete) *complete = 1;
    return count;
}

int64_t bp_max_bpp(const BattlePass *bp) {
    if (!bp || bp->item_count <= 0 || bp->bpp_per_level <= 0) return 0;
    int64_t total = 0;
    for (int level = 1; level <= bp->item_count; level++) total += bp_level_need(bp, level);
    return total;
}

int64_t bp_cap_total(const BattlePass *bp, const int64_t now_s) {
    if (!bp) return 0;
    // Math.floor((Date.now() - new Date(startdate)) / 86400000) + 1
    const int64_t elapsed = now_s - bp->start_s;
    const int64_t days = elapsed >= 0 ? elapsed / 86400 : -((-elapsed + 86399) / 86400);
    const int64_t total = (days + 1) * BP_CAP_DAY;
    return total > 0 ? total : 0;
}

int64_t bp_available(const BattlePass *bp, const int64_t bpp, const int64_t now_s) {
    const int64_t left = bp_cap_total(bp, now_s) - bpp;
    return left > 0 ? left : 0;
}

int64_t bp_match_points(const BpMatchInput *in) {
    // endGame's block: if (this.isPubType() && this.endIndex == null), and the award needs an account
    if (!in || !in->pub_type) return 0;
    const double mlt = in->reward_mlt > 0.0 ? in->reward_mlt : 1.0; // mode.rewardMlt || 1
    double base = floor(in->score * mlt * 0.01);                    // Math.floor(score * mlt * 0.01)
    if (base > BP_MATCH_SCORE_MAX) base = BP_MATCH_SCORE_MAX;       // Math.min(20, ...)
    double play = 0.0;
    if (!in->empty_lobby && !in->raid) {
        double frac = floor(in->play_ms / in->elapsed_ms * 10.0) / 10.0; // Math.floor(playTime / elapsed * 10) / 10
        if (frac > 1.0) frac = 1.0;                                     // Math.min(1, ...)
        // JS: 0 over a 0 clock is NaN, which would corrupt the balance; KN counts that term as 0
        if (!isnan(frac)) play = frac * BP_MATCH_PLAY_MAX;
    }
    return (int64_t) floor(base + play + 0.5); // Math.round
}

BpAward bp_validate_progress(const BattlePass *bp, const int64_t bpp, const int64_t requested, const int64_t now_s) {
    BpAward r = {0, 0};
    const int64_t cur = bpp > 0 ? bpp : 0;
    const int64_t want = requested > 0 ? requested : 0;
    int64_t limit = bp_cap_total(bp, now_s);
    const int64_t max = bp_max_bpp(bp);
    if (limit > max) limit = max;
    int64_t room = limit - cur;
    if (room < 0) room = 0;
    r.award = want < room ? want : room;
    r.remaining = room - r.award;
    return r;
}

const BattlePassItem *bp_item(const BattlePass *bp, const int id) {
    for (int i = 0; bp && i < bp->count; i++)
        if (bp->items[i].id == id) return &bp->items[i];
    return NULL;
}

const BattlePassItem *bp_item_at(const BattlePass *bp, const int tier, const int level0) {
    for (int i = 0; bp && i < bp->count; i++)
        if (bp->items[i].tier == tier && bp->items[i].level == level0) return &bp->items[i];
    return NULL;
}

const BattlePassItem *bp_next_reward(const BattlePass *bp, const int64_t bpp) {
    int complete = 0;
    const int level = bp_level(bp, bpp, NULL, &complete);
    if (!bp || complete || level >= bp->item_count) return NULL;
    const BattlePassItem *vip = bp_item_at(bp, 1, level);
    if (bp_item_has_reward(vip)) return vip;
    const BattlePassItem *free_item = bp_item_at(bp, 0, level);
    return bp_item_has_reward(free_item) ? free_item : NULL;
}

int bp_is_claimed(const char *items_claimed, const int id) {
    // (items_claimed + "").split(",").includes(id + "")
    const char *p = items_claimed;
    while (p && *p) {
        char *end = NULL;
        const long v = strtol(p, &end, 10);
        if (end != p && (*end == ',' || !*end) && v == id) return 1;
        while (*p && *p != ',') p++;
        if (*p == ',') p++;
    }
    return 0;
}

void bp_add_claimed(const char *items_claimed, const int id, char *out, const size_t size) {
    if (!out || !size) return;
    if (items_claimed && items_claimed[0]) snprintf(out, size, "%s,%d", items_claimed, id);
    else snprintf(out, size, "%d", id);
}

BpClaimResult bp_claim_check(const BattlePass *bp, const int64_t bpp, const int tier, const char *items_claimed, const int id) {
    const BattlePassItem *it = bp_item(bp, id);
    if (!bp_item_has_reward(it)) return BP_CLAIM_NOT_FOUND;
    if (bp_is_claimed(items_claimed, id)) return BP_CLAIM_CLAIMED;
    if (it->tier >= 1 && tier < 1) return BP_CLAIM_TIER;
    if (bp_level(bp, bpp, NULL, NULL) < it->level + 1) return BP_CLAIM_LEVEL;
    return BP_CLAIM_OK;
}

const char *bp_claim_error(const BpClaimResult r) {
    switch (r) {
        case BP_CLAIM_OK: return "";
        case BP_CLAIM_NOT_FOUND: return "Item not found";
        case BP_CLAIM_LEVEL: return "Level not reached";
        case BP_CLAIM_TIER: return "VIP Tier required";
        case BP_CLAIM_CLAIMED: return "Item already claimed";
    }
    return "Error";
}

int bp_skip_max(const BattlePass *bp, const int64_t bpp) {
    const int level = bp_level(bp, bpp, NULL, NULL);
    const int left = (bp ? bp->item_count : 0) - level;
    return left > 1 ? left : 1;
}

int64_t bp_skip_bpp(const BattlePass *bp, const int64_t bpp, const int levels) {
    const int64_t max = bp_max_bpp(bp);
    int64_t next = bpp > 0 ? bpp : 0;
    const int from = bp_level(bp, next, NULL, NULL);
    for (int i = 0; bp && i < levels; i++) next += bp_level_need(bp, from + i <= bp->item_count ? from + i : bp->item_count);
    return next > max ? max : next;
}

// ------------------------------------------------------------------ map rewards

// {skin, mapReward, seas} of every store skin with a mapReward (src/data/store.js / skins.json)
static const int MAP_REWARDS[][3] = {
    {5236, 17, 6}, {5237, 0, 6},  {5238, 8, 6},   {5239, 13, 6}, {5240, 6, 6},  {5241, 11, 6}, {5242, 10, 6},
    {5243, 16, 6}, {5244, 12, 6}, {5245, 9, 6},   {5246, 2, 6},  {5247, 4, 6},  {5248, 14, 6}, {5249, 15, 6},
    {5250, 1, 6},  {5251, 19, 6}, {5252, 3, 6},   {5253, 7, 6},  {5923, 28, 7},
};

int season_map_reward(const int season, const int map_index) {
    for (size_t i = 0; i < sizeof(MAP_REWARDS) / sizeof(*MAP_REWARDS); i++)
        if (MAP_REWARDS[i][1] == map_index && MAP_REWARDS[i][2] == season) return MAP_REWARDS[i][0];
    return 0;
}

// ------------------------------------------------------------------ season badges

static int owns(const SeasonBadgeAccount *a, const int badge) {
    for (int i = 0; a->badges && i < a->badge_count; i++)
        if (a->badges[i] == badge) return 1;
    return 0;
}

// badges.js Founder: currentSeason == season && BP.tier && bought in the first week
static int founder(const SeasonBadgeAccount *a, const int season) {
    if (a->season != season || a->bp_tier < 1) return 0;
    const int64_t until = a->bp_start_s + (int64_t) BP_FOUNDER_DAYS * 86400;
    const int64_t when = a->bp_tier_at_s > 0 ? a->bp_tier_at_s : a->now_s;
    return when < until;
}

int season_badge_rule(const int index, const SeasonBadgeAccount *a) {
    if (index < 22 || index > 28) return -1;
    if (!a) return 0;
    switch (index) {
        case 22: return founder(a, 6); // Season 6 Founder
        case 23: {                     // S6 Map Master: the 17 Season 6 map reward skins
            if (a->season != 6 || !a->has_skin) return 0;
            for (int skin = 5237; skin <= 5253; skin++)
                if (!a->has_skin(a->ud, skin)) return 0;
            return 1;
        }
        case 24: return a->season == 6 && a->bp_tier >= 1 && a->bpp >= BP_MASTER_BPP; // S6 Battle Pass Master
        case 25: return a->season == 6 && owns(a, 22) && owns(a, 23) && owns(a, 24);   // S6 Badge Master
        case 26: return founder(a, 7);                                                  // Season 7 Founder
        case 27: return a->season == 7 && a->bp_tier >= 1 && a->bpp >= BP_MASTER_BPP; // S7 Battle Pass Master
        case 28: return a->season == 7 && owns(a, 26) && owns(a, 27);                  // S7 Badge Master
        default: return 0;
    }
}
