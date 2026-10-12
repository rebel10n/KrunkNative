// player-viewer WP-E part 2c: Krunker v7.1.4 account progression. See shared/include/progression.h.
#include <progression.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ------------------------------------------------------------------ level

int progression_level(const double score, const double rank_var) {
    const int level = (int) floor(rank_var * sqrt(score > 0.0 ? score : 0.0));
    return level < 1 ? 1 : level;
}

// getLevelProg(): parseInt(score - lo) / parseInt(hi - lo) * 100 (parseInt truncates toward 0)
double progression_level_prog(const double score, const double rank_var) {
    if (rank_var <= 0.0) return 0.0;
    const int level = progression_level(score, rank_var);
    const double lo = level > 1 ? pow(level / rank_var, 2.0) : 0.0;
    const double hi = pow((level + 1) / rank_var, 2.0);
    const double span = trunc(hi - lo);
    return span > 0.0 ? trunc(score - lo) / span * 100.0 : 0.0;
}

// ------------------------------------------------------------------ medals and stats

const char *const g_progression_medals[PROGRESSION_MEDAL_COUNT] = {
    "Humiliation", "Promotion", "First Blood", "Revenge", "Bullseye", "Beatdown", "Execution", "No Scope", "Quick Scope",
    "Headshot", "Wallbang", "Longshot", "Snatched", "Close Call", "Comeback", "Buzzkill", "Driftkill", "Mid Air", "Turbo Kill",
    "Bomb Planted", "Secured", "Team Work", "Capture", "Return", "Pickup", "Defender", "Attacker", "Clock Work", "Recharge",
    "Denied", "Confirmed", "Assist", "Double Kill", "Triple Kill", "Quad Kill", "Multi Kill", "Mega Kill", "Ultra Kill",
    "Super Kill", "Leveled Up", "Nuked", "Boosted", "Sleight of Hand", "Rapid Fire", "Siphon", "Rooted", "Dizzy", "Poisoned",
    "Collected", "Deposited", "Bombed", "Jump Boost", "Zapped", "Strength", "Scavenger", "Cycled"};

int progression_medal_index(const char *name) {
    for (int i = 0; name && i < PROGRESSION_MEDAL_COUNT; i++)
        if (!strcmp(g_progression_medals[i], name)) return i;
    return -1;
}

const char *const g_progression_stats[PROGRESSION_STAT_COUNT] = {
    "s", "h", "hs", "ls", "wb", "mk", "fk", "tmk", "ast", "spry", "crc", "n", "ad", "sad", "cad", "sl", "jg", "kjg", "jgk", "wm"};

int progression_stat_index(const char *key) {
    for (int i = 0; key && i < PROGRESSION_STAT_COUNT; i++)
        if (!strcmp(g_progression_stats[i], key)) return i;
    return -1;
}

// ------------------------------------------------------------------ challenges

static const char *const ADD_KEYS[] = {
    "score", "k", "w", "gp", "d", "stk", "hs", "ls", "mk", "fk", "ast", "spry", "wb", "airk", "qs", "ns",
    "lns", "cc", "drk", "tk", "tmk", "fb", "cps", "rk", "cb", "bzk", "dmg", "sad", "cad", "bst", "find", "rcm",
    "twm", "atkm", "defm", "dny", "cnf", "eggs", "geggs", "amp", "wpb", "cvrt", "aik", "prks", "pump", "mirror", "mybx"};
static const char *const SET_KEYS[] = {"kstk", "tower", "pkr", "island", "time"};
static const char *const MAX_KEYS[] = {"amc", "uwk", "rnd"};

static int in_list(const char *key, const char *const *list, const size_t n) {
    for (size_t i = 0; key && i < n; i++)
        if (!strcmp(list[i], key)) return 1;
    return 0;
}

ChallengeKeyKind challenge_key_kind(const char *key) {
    if (!key) return CHALLENGE_KEY_UNKNOWN;
    if (!strcmp(key, "chall")) return CHALLENGE_KEY_META;
    if (in_list(key, ADD_KEYS, sizeof(ADD_KEYS) / sizeof(*ADD_KEYS))) return CHALLENGE_KEY_ADD;
    if (in_list(key, SET_KEYS, sizeof(SET_KEYS) / sizeof(*SET_KEYS))) return CHALLENGE_KEY_SET;
    if (in_list(key, MAX_KEYS, sizeof(MAX_KEYS) / sizeof(*MAX_KEYS))) return CHALLENGE_KEY_MAX;
    return CHALLENGE_KEY_UNKNOWN;
}

// A DAT / REW field as a number: numbers and numeric strings; *has = 0 for null / missing.
static double field_num(const cJSON *obj, const char *key, int *has) {
    const cJSON *v = obj ? cJSON_GetObjectItemCaseSensitive(obj, key) : NULL;
    if (cJSON_IsNumber(v)) {
        if (has) *has = 1;
        return v->valuedouble;
    }
    if (cJSON_IsBool(v)) {
        if (has) *has = 1;
        return cJSON_IsTrue(v) ? 1.0 : 0.0;
    }
    if (cJSON_IsString(v) && v->valuestring && v->valuestring[0]) {
        char *end = NULL;
        const double d = strtod(v->valuestring, &end);
        if (end && !*end) {
            if (has) *has = 1;
            return d;
        }
    }
    if (has) *has = 0;
    return 0.0;
}

static int field_truthy(const cJSON *obj, const char *key) {
    int has = 0;
    const double v = field_num(obj, key, &has);
    return has && v != 0.0;
}

static const char *field_str(const cJSON *obj, const char *key) {
    const cJSON *v = obj ? cJSON_GetObjectItemCaseSensitive(obj, key) : NULL;
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static void set_num(cJSON *obj, const char *key, const double v) {
    cJSON *n = cJSON_CreateNumber(v);
    if (cJSON_GetObjectItemCaseSensitive(obj, key)) cJSON_ReplaceItemInObjectCaseSensitive(obj, key, n);
    else cJSON_AddItemToObject(obj, key, n);
}

// The match filters of checkChallenge (mp, mid, md, cls, ranked).
static int match_ok(const cJSON *c, const cJSON *dat, const ChallengeMatch *m) {
    int has = 0;
    double v = field_num(dat, "mp", &has);
    if (has && (m->map_index < 0 || (int) v != m->map_index)) return 0;
    v = field_num(dat, "mid", &has);
    if (has && (m->map_id < 0 || (int) v != m->map_id)) return 0;
    v = field_num(dat, "md", &has);
    if (has && (int) v != m->mode) return 0;
    v = field_num(dat, "cls", &has);
    if (has && (int) v != m->class_index) return 0;
    const char *type = field_str(c, "T");
    if (type && !strcmp(type, "ranked") && !m->ranked) return 0;
    return 1;
}

// checkChallenge over the tallies: 1 when an event touched the challenge, *pv = its progVal.
static int check_challenge(const cJSON *c, const cJSON *events, const ChallengeMatch *m, double *pv) {
    const cJSON *dat = cJSON_GetObjectItemCaseSensitive(c, "DAT");
    const char *key = field_str(dat, "ky");
    const ChallengeKeyKind kind = challenge_key_kind(key);
    if (kind == CHALLENGE_KEY_UNKNOWN || kind == CHALLENGE_KEY_META || field_num(c, "PT", NULL) == 1.0) return 0;
    if (!match_ok(c, dat, m)) return 0;
    int has_wep = 0, has_stk = 0, has_item = 0;
    const double wep = field_num(dat, "wep", &has_wep), stk = field_num(dat, "stk", &has_stk);
    field_num(dat, "item", &has_item);
    if (has_item) return 0; // item events (store items used in a match) are not tallied
    const int kstk = !strcmp(key, "kstk");
    double value = field_num(c, "PV", NULL);
    int touched = 0;
    const cJSON *ev;
    cJSON_ArrayForEach(ev, events) {
        const cJSON *k = cJSON_GetArrayItem(ev, 0);
        if (!cJSON_IsString(k) || strcmp(k->valuestring, key) != 0) continue;
        const double v = cJSON_GetNumberValue(cJSON_GetArrayItem(ev, 1));
        const cJSON *w = cJSON_GetArrayItem(ev, 2), *s = cJSON_GetArrayItem(ev, 3);
        const int ev_wep = cJSON_IsNumber(w) ? w->valueint : -1, ev_stk = cJSON_IsNumber(s) ? s->valueint : -1;
        if (isnan(v)) continue;
        // kstk: DAT.wep == null takes the kill streak (nonWep, the weapon -1 rows)
        if (kstk ? (has_wep ? ev_wep != (int) wep : ev_wep >= 0) : (has_wep && ev_wep != (int) wep)) continue;
        if (has_stk && ev_stk != (int) stk) continue;
        if (kind == CHALLENGE_KEY_ADD) value += v;
        else if (kind == CHALLENGE_KEY_MAX || kstk) value = v > value ? v : value;
        else value = v; // SET
        touched = 1;
    }
    *pv = value;
    return touched;
}

static int reached(const cJSON *dat, const double pv, const int lesser_ok) {
    const double val = field_num(dat, "val", NULL);
    if (lesser_ok && field_truthy(dat, "lesser")) return pv != 0.0 && pv <= val;
    return pv >= val;
}

static void add_reward(cJSON *reward, const cJSON *rew) {
    if (!reward || !rew) return;
    const double kr = field_num(rew, "kr", NULL);
    if (kr > 0.0) set_num(reward, "kr", field_num(reward, "kr", NULL) + kr);
    cJSON *skins = cJSON_GetObjectItemCaseSensitive(reward, "skins");
    if (!skins) skins = cJSON_AddArrayToObject(reward, "skins");
    const cJSON *sk = cJSON_GetObjectItemCaseSensitive(rew, "sk");
    if (cJSON_IsNumber(sk)) {
        cJSON_AddItemToArray(skins, cJSON_CreateNumber(sk->valueint));
    } else if (cJSON_IsArray(sk)) {
        const cJSON *it;
        cJSON_ArrayForEach(it, sk) {
            if (cJSON_IsNumber(it)) cJSON_AddItemToArray(skins, cJSON_CreateNumber(it->valueint));
            else if (cJSON_IsString(it) && it->valuestring[0]) cJSON_AddItemToArray(skins, cJSON_CreateNumber(atoi(it->valuestring)));
        }
    } else if (cJSON_IsString(sk) && sk->valuestring) { // "1,2,3"
        const char *p = sk->valuestring;
        while (*p) {
            char *end = NULL;
            const long v = strtol(p, &end, 10);
            if (end == p) break;
            cJSON_AddItemToArray(skins, cJSON_CreateNumber((double) v));
            p = *end == ',' ? end + 1 : end;
        }
    }
}

// finalize()'s bookkeeping for one challenge whose progVal is `pv` (v13487 / v13489).
static int finalize_one(cJSON *c, const double pv, const int set_or_max, cJSON *results, cJSON *completed, cJSON *reward) {
    const cJSON *dat = cJSON_GetObjectItemCaseSensitive(c, "DAT");
    const int pt = field_num(c, "PT", NULL) == 1.0;
    const double old = field_num(c, "PV", NULL);
    const int sm = field_truthy(dat, "sm");
    const int done = !pt && reached(dat, pv, 1);
    int save = !(pt || pv == old || (sm && !done));
    if (set_or_max && !done) save = 0;
    if (save) {
        if (done) {
            set_num(c, "PT", 1);
            if (completed) cJSON_AddItemToArray(completed, cJSON_Duplicate(c, 1));
            add_reward(reward, cJSON_GetObjectItemCaseSensitive(c, "REW"));
        }
        set_num(c, "PV", pv);
        if (results) {
            cJSON_AddItemToArray(results, cJSON_CreateNumber(done ? 1 : 0));
            cJSON_AddItemToArray(results, cJSON_CreateNumber(field_num(c, "CI", NULL)));
            cJSON_AddItemToArray(results, cJSON_CreateNumber(pv));
        }
    }
    if (sm && !save) set_num(c, "PV", 0); // single-match challenges restart (PV = SV = 0)
    return save;
}

// The "chall" filter: a completed challenge of the same type and the meta challenge's filters.
static int same_field(const cJSON *meta_dat, const cJSON *dat, const char *key) {
    int has = 0;
    const double want = field_num(meta_dat, key, &has);
    if (!has) return 1;
    int has2 = 0;
    const double v = field_num(dat, key, &has2);
    return has2 && v == want;
}

int challenges_finalize(cJSON *active, const cJSON *events, const ChallengeMatch *match, cJSON *results, cJSON *completed,
                        cJSON *reward) {
    if (!cJSON_IsArray(active) || !match) return 0;
    int changed = 0;
    if (reward && !cJSON_GetObjectItemCaseSensitive(reward, "kr")) cJSON_AddNumberToObject(reward, "kr", 0);
    if (reward && !cJSON_GetObjectItemCaseSensitive(reward, "skins")) cJSON_AddArrayToObject(reward, "skins");
    cJSON *c;
    cJSON_ArrayForEach(c, active) {
        const cJSON *dat = cJSON_GetObjectItemCaseSensitive(c, "DAT");
        if (match->quit && !field_truthy(dat, "nem")) continue;
        double pv = 0.0;
        if (!check_challenge(c, events, match, &pv)) continue;
        const ChallengeKeyKind kind = challenge_key_kind(field_str(dat, "ky"));
        changed += finalize_one(c, pv, kind == CHALLENGE_KEY_SET || kind == CHALLENGE_KEY_MAX, results, completed, reward);
    }
    // "chall": the number of completed challenges of the same type (and filters)
    cJSON_ArrayForEach(c, active) {
        const cJSON *dat = cJSON_GetObjectItemCaseSensitive(c, "DAT");
        const char *key = field_str(dat, "ky");
        if (!key || strcmp(key, "chall") != 0 || (match->quit && !field_truthy(dat, "nem"))) continue;
        const char *type = field_str(c, "T");
        int count = 0;
        const cJSON *o;
        cJSON_ArrayForEach(o, active) {
            const cJSON *odat = cJSON_GetObjectItemCaseSensitive(o, "DAT");
            const char *otype = field_str(o, "T");
            if (field_num(o, "PT", NULL) != 1.0 || !type || !otype || strcmp(type, otype) != 0) continue;
            if (!same_field(dat, odat, "wep") || !same_field(dat, odat, "cls") || !same_field(dat, odat, "mp") ||
                !same_field(dat, odat, "mid") || !same_field(dat, odat, "md") || !same_field(dat, odat, "evt") ||
                !same_field(dat, odat, "seas"))
                continue;
            count++;
        }
        changed += finalize_one(c, count, 0, results, completed, reward);
    }
    return changed;
}

// i18n/en.js chall.name.<ky> ({0} = DAT.val, {1} = "with <weapon>" or "")
static const char *const NAME_TEMPLATES[][2] = {
    {"hs", "Get {0} headshots {1}"},       {"ns", "Get {0} no-scopes {1}"},     {"ls", "Get {0} legshots {1}"},
    {"tmk", "Get {0} throwing knife kills {1}"}, {"fb", "Get {0} first bloods {1}"}, {"mk", "Get {0} melee kills {1}"},
    {"fk", "Get {0} fist kills {1}"},      {"qs", "Get {0} quickscopes {1}"},   {"wb", "Get {0} wallbangs {1}"},
    {"airk", "Get {0} mid-air kills {1}"}, {"lns", "Get {0} longshots {1}"},    {"tk", "Get {0} turbo kills {1}"},
    {"drk", "Get {0} drift kills {1}"},    {"kstk", "Get a {0} killstreak {1}"}, {"w", "Win {0} matches"},
    {"d", "Die {0} times"},                {"score", "Get {0} score"},          {"dmg", "Deal {0} damage {1}"},
    {"bzk", "Get {0} buzz kills {1}"},     {"cc", "Get {0} close-calls {1}"},   {"rk", "Deal {0} revenge kills {1}"},
};

void challenge_name(const cJSON *challenge, const char *(*weapon_name)(int index), char *out, const size_t size) {
    if (!out || !size) return;
    out[0] = 0;
    const char *given = field_str(challenge, "N");
    if (given && given[0]) {
        snprintf(out, size, "%s", given);
        return;
    }
    const cJSON *dat = cJSON_GetObjectItemCaseSensitive(challenge, "DAT");
    const char *key = field_str(dat, "ky");
    const char *tmpl = NULL;
    for (size_t i = 0; key && i < sizeof(NAME_TEMPLATES) / sizeof(*NAME_TEMPLATES); i++)
        if (!strcmp(NAME_TEMPLATES[i][0], key)) tmpl = NAME_TEMPLATES[i][1];
    if (!tmpl) return;
    char val[32], with[96] = "";
    snprintf(val, sizeof(val), "%.15g", field_num(dat, "val", NULL));
    int has_wep = 0;
    const double wep = field_num(dat, "wep", &has_wep);
    const char *type = field_str(challenge, "T");
    const char *wname = has_wep && type && !strcmp(type, "daily") && weapon_name ? weapon_name((int) wep) : NULL;
    if (wname && wname[0]) snprintf(with, sizeof(with), "with %s", wname);
    size_t o = 0;
    for (const char *p = tmpl; *p && o + 1 < size; p++) {
        const char *put = NULL;
        if (p[0] == '{' && (p[1] == '0' || p[1] == '1') && p[2] == '}') {
            put = p[1] == '0' ? val : with;
            p += 2;
        }
        if (put) {
            for (; *put && o + 1 < size; put++) out[o++] = *put;
        } else {
            out[o++] = *p;
        }
    }
    out[o] = 0;
}

// ------------------------------------------------------------------ badges

int badge_requirement_met(const int index, const cJSON *stats) {
    int has = 0;
    const double nukes = field_num(stats, "n", &has);
    switch (index) {
        case 11: return nukes >= 500;  // Nuclear I
        case 13: return nukes >= 750;  // Nuclear II
        case 14: return nukes >= 1000; // Nuclear III
        case 15: return nukes >= 1500; // Nuclear IV
        default: return 0;             // season badges: Battle Pass rules (not in KrunkNative)
    }
}
