// Krunker v7.1.4 config.playerCardStats (src/config.js:2883-3050) and the death screen's class
// level (src/index.js f1975). See shared/include/player_card.h.
#include <player_card.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

typedef enum { PCS_FIELD, PCS_STAT, PCS_RATIO, PCS_WL, PCS_ACCURACY, PCS_LOSSES, PCS_MISSES } PcStatKind;

typedef struct {
    const char *name;
    PcStatKind kind;
    const char *a, *b; // account field / stats key operands (see stat_value)
} PcStat;

// In config.playerCardStats order (the pcStatIndex values).
static const PcStat PC_STATS[PLAYER_CARD_STAT_COUNT] = {
    {"SPK", PCS_RATIO, "score", "kills"},       // (score || 0) / max(kills || 0, 1)
    {"Kills", PCS_FIELD, "kills", NULL},
    {"Deaths", PCS_FIELD, "deaths", NULL},
    {"KDR", PCS_RATIO, "kills", "deaths"},
    {"KPG", PCS_RATIO, "kills", "games"},
    {"Games Played", PCS_FIELD, "games", NULL},
    {"Wins", PCS_FIELD, "wins", NULL},
    {"W/L", PCS_WL, NULL, NULL},                // wins / max(games - wins, 1)
    {"Nukes", PCS_STAT, "n", NULL},
    {"Melees", PCS_STAT, "mk", NULL},
    {"Accuracy", PCS_ACCURACY, NULL, NULL},     // (s ? min(s, h) / s * 100 : 0).round(1) + "%"
    {"KR", PCS_FIELD, "funds", NULL},
    {"Headshots", PCS_STAT, "hs", NULL},
    {"Losses", PCS_LOSSES, NULL, NULL},         // max(games - wins, 0)
    {"Wallbangs", PCS_STAT, "wb", NULL},
    {"Beatdowns", PCS_STAT, "fk", NULL},
    {"Bullseyes", PCS_STAT, "tmk", NULL},
    {"Sprays Placed", PCS_STAT, "spry", NULL},
    {"Assists", PCS_STAT, "ast", NULL},
    {"Inventory Value", PCS_FIELD, "invVal", NULL},
    {"Crouches", PCS_STAT, "crc", NULL},
    {"Shots", PCS_STAT, "s", NULL},
    {"Hits", PCS_STAT, "h", NULL},
    {"Misses", PCS_MISSES, NULL, NULL},         // s - min(s, h)
    {"Legshots", PCS_STAT, "ls", NULL},
    {"Slimers", PCS_STAT, "sl", NULL},
    {"Juggernauts", PCS_STAT, "jg", NULL},
    {"War Machines", PCS_STAT, "wm", NULL},
};

const char *player_card_stat_name(const int index) {
    return index >= 0 && index < PLAYER_CARD_STAT_COUNT ? PC_STATS[index].name : NULL;
}

// `x || 0` of a JSON value: numbers (numeric strings too, as the brain may send text).
static double num(const cJSON *item) {
    if (cJSON_IsNumber(item)) return item->valuedouble;
    if (cJSON_IsString(item) && item->valuestring) {
        double v = 0.0;
        if (sscanf(item->valuestring, "%lf", &v) == 1 && isfinite(v)) return v;
    }
    return 0.0;
}

static double field(const cJSON *account, const char *key) { return num(cJSON_GetObjectItemCaseSensitive(account, key)); }

static double stat(const cJSON *account, const char *key) {
    const cJSON *stats = cJSON_GetObjectItemCaseSensitive(account, "stats");
    return cJSON_IsObject(stats) ? num(cJSON_GetObjectItemCaseSensitive(stats, key)) : 0.0;
}

// String(v) of a JS number for the values these stats produce (integers, .round(1) results).
static void js_number(const double v, char *out, const size_t n) {
    if (v == floor(v) && fabs(v) < 1e15) snprintf(out, n, "%.0f", v);
    else snprintf(out, n, "%.15g", v);
}

void player_card_accuracy(const double shots, const double hits, char *out, const size_t n) {
    const double pct = shots ? fmin(shots, hits) / shots * 100.0 : 0.0;
    char v[32];
    js_number(round(pct * 10.0) / 10.0, v, sizeof(v)); // Number.prototype.round(1)
    snprintf(out, n, "%s%%", v);
}

int player_card_stat_format(const int index, const cJSON *account, char *out, const size_t n) {
    if (!out || !n) return 0;
    out[0] = 0;
    if (index < 0 || index >= PLAYER_CARD_STAT_COUNT || !cJSON_IsObject(account)) return 0;
    const PcStat *s = &PC_STATS[index];
    switch (s->kind) {
        case PCS_FIELD: js_number(field(account, s->a), out, n); break;
        case PCS_STAT: js_number(stat(account, s->a), out, n); break;
        case PCS_RATIO: snprintf(out, n, "%.2f", field(account, s->a) / fmax(field(account, s->b), 1.0)); break; // toFixed(2)
        case PCS_WL: {
            const double wins = field(account, "wins");
            snprintf(out, n, "%.2f", wins / fmax(field(account, "games") - wins, 1.0));
            break;
        }
        case PCS_ACCURACY: player_card_accuracy(stat(account, "s"), stat(account, "h"), out, n); break;
        case PCS_LOSSES: js_number(fmax(field(account, "games") - field(account, "wins"), 0.0), out, n); break;
        case PCS_MISSES: {
            const double shots = stat(account, "s");
            js_number(shots - fmin(shots, stat(account, "h")), out, n);
            break;
        }
    }
    return 1;
}

int player_card_class_level(const double class_stat) {
    const double level = floor(PLAYER_CARD_RANK_VAR * sqrt(class_stat > 0.0 ? class_stat : 0.0)) + 1.0;
    return level > 1.0 ? (int) level : 1;
}
