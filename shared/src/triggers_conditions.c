// Krunker v7.1.4 trigger conditions (data/triggers.js:104-320; doc/v7/map-logic.md §3.6): checked
// per action when onTrigger runs it (not again for a delayed action). The cv0..cv2 values are
// sanitized as MapManager.addTrigger does (libs/MapManager.js:1958-1985) from the raw map values
// TriggerActionData keeps: missing -> def, bool -> !!v, input -> [a-z0-9_] cut to len, select ->
// limitMM(parseInt(v) || def, 0, options - 1), a JSON number -> limitMM(parseFloat(v) || def,
// min, max), anything else -> def.
#include <shared.h>
#include "triggers_internal.h"
#include <cJSON.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    double number[3];
    char key[3][TRIGGER_V7_KEY_MAX + 1];
} ConditionValues;

// JS parseInt of a JSON value: numbers truncate, strings read their leading integer, else NaN (0 + ok 0).
static int js_parse_int(const cJSON *v, int *ok) {
    *ok = 0;
    if (cJSON_IsNumber(v)) {
        const double d = cJSON_GetNumberValue(v);
        if (!(d == d) || fabs(d) >= 2147483647.0) return 0;
        *ok = 1;
        return (int) d;
    }
    const char *text = cJSON_GetStringValue(v);
    if (!text) return 0;
    while (*text == ' ' || *text == '\t' || *text == '\n' || *text == '\r') text++;
    char *end = NULL;
    const long value = strtol(text, &end, 10);
    if (end == text) return 0;
    *ok = 1;
    return (int) value;
}

// (v + "").replace(/[^a-z0-9_]+/gmi, "").substring(0, len)
static void js_input(const cJSON *v, char *out, const int len) {
    char number[32];
    const char *text = cJSON_GetStringValue(v);
    if (!text && cJSON_IsNumber(v)) {
        snprintf(number, sizeof(number), "%.15g", cJSON_GetNumberValue(v));
        text = number;
    } else if (!text && cJSON_IsBool(v)) {
        text = cJSON_IsTrue(v) ? "true" : "false";
    }
    int n = 0;
    for (; text && *text && n < len && n < TRIGGER_V7_KEY_MAX; text++) {
        const char c = *text;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_') out[n++] = c;
    }
    out[n] = 0;
}

static void condition_values(const TriggerActionData *a, const int cd, ConditionValues *out) {
    memset(out, 0, sizeof(*out));
    const TriggerV7Condition *c = &g_trigger_v7_conditions[cd];
    for (int i = 0; i < c->val_count && i < 3; i++) {
        const TriggerV7ConditionVal *def = &c->vals[i];
        const cJSON *raw = (const cJSON *) a->condition_values[i];
        out->number[i] = def->def;
        if (!raw || cJSON_IsNull(raw)) continue; // cv == null: def
        int ok;
        switch (def->kind) {
            case TRIGGER_V7_VAL_BOOL:
                out->number[i] = cJSON_IsTrue(raw) || (cJSON_IsNumber(raw) && cJSON_GetNumberValue(raw) != 0.0) ||
                                 (cJSON_IsString(raw) && cJSON_GetStringValue(raw)[0]) || cJSON_IsObject(raw) || cJSON_IsArray(raw);
                break;
            case TRIGGER_V7_VAL_INPUT:
                js_input(raw, out->key[i], def->len);
                break;
            case TRIGGER_V7_VAL_SELECT: {
                int value = js_parse_int(raw, &ok);
                if (!ok || !value) value = (int) def->def; // parseInt(v) || def
                const int max = def->option_count > 0 ? def->option_count - 1 : 0;
                out->number[i] = value < 0 ? 0 : value > max ? max : value;
                break;
            }
            default: // number: only a JSON number is read (UTILS.isNumber)
                if (cJSON_IsNumber(raw)) {
                    double value = cJSON_GetNumberValue(raw);
                    if (!(value == value) || value == 0.0) value = def->def; // parseFloat(v) || def
                    out->number[i] = value < def->min ? def->min : value > def->max ? def->max : value;
                }
                break;
        }
    }
}

// UTILS.compareVal(type, a, b): 0 ==, 1 !=, 2 >, 3 >=, 4 <, 5 <= (other types: true)
static int compare_val(const int type, const double a, const double b) {
    switch (type) {
        case 0: return a == b;
        case 1: return a != b;
        case 2: return a > b;
        case 3: return a >= b;
        case 4: return a < b;
        case 5: return a <= b;
        default: return 1;
    }
}

static double player_value(const Player *player, const char *key) {
    // config.playerVals.indexOf(key) > -1 ? player[key] : custVals[key] || 0
    if (!strcmp(key, "health")) return player->health;
    if (!strcmp(key, "maxHealth")) return player->max_health;
    if (!strcmp(key, "score")) return player->score;
    if (!strcmp(key, "classIndex")) return player->class_index;
    const TriggerValue *v = trigger_values_find(&player->custom_values, key);
    return v ? v->value : 0.0;
}

// Players counted by condition 4 "Player Count": 0 active, 1 spectating, 2 connected clients.
// KN: spectators are connections without a Player in the game, so filter 1 counts 0 and filter 2
// counts the human players in the game (v7 connectedClients counts sockets; bots have none).
static int player_count(const Game *game, const int filter) {
    int count = 0;
    for (size_t i = 0; i < game->player_count; i++) {
        const Player *p = game->players[i];
        if (!p) continue;
        if (filter == 0 && p->active) count++;
        else if (filter == 2 && !p->is_bot) count++;
    }
    return count;
}

int trigger_condition_check(Game *game, Player *player, const TriggerSource *obj, const TriggerActionData *a) {
    int cd = a->condition;
    if (cd < 0 || cd >= TRIGGER_V7_CONDITION_COUNT) cd = 0; // !conditions[cd]: 0
    if (cd == 0) return 1;
    ConditionValues v;
    condition_values(a, cd, &v);
    AccountInfo account;
    switch (cd) {
        case 1: // Player Value: Key, Type, Val
            return player && compare_val((int) v.number[1], player_value(player, v.key[0]), v.number[2]);
        case 2: // Current Class
            return player && player->class_index == (int) v.number[0];
        case 3: { // Trigger Value: the trigger's own custVals (AIs have none: 0)
            if (!player || !obj) return 0;
            const TriggerValue *value = obj->object ? trigger_values_find(trigger_object_values(game, obj->object), v.key[0]) : NULL;
            return compare_val((int) v.number[1], value ? value->value : 0.0, v.number[2]);
        }
        case 4: // Player Count: Type, Filter, Amount (needs the trigger, not a player)
            return obj && compare_val((int) v.number[0], player_count(game, (int) v.number[1]), v.number[2]);
        case 5: // Global Value
            return player && obj && compare_val((int) v.number[1], trigger_global_value_get(game, v.key[0]), v.number[2]);
        case 6: // Player has Account
            return player && trigger_account(game, player, &account);
        case 7: // Player has Premium (account.premiumT > 0)
            return player && trigger_account(game, player, &account) && account.premium;
        case 8: // Player is Verified (account.featured)
            return player && trigger_account(game, player, &account) && account.verified;
        case 9: // Player is Partner (account.partnerApp >= 2)
            return player && trigger_account(game, player, &account) && account.partner;
        case 10: // Player Level: Type, Level (parseInt)
            if (!player) return 0;
            trigger_account(game, player, &account);
            return compare_val((int) v.number[0], account.level, (int) v.number[1]);
        case 11: // Player Inventory Value: Type, Val (parseInt)
            if (!player) return 0;
            trigger_account(game, player, &account);
            return compare_val((int) v.number[0], (double) account.inventory_value, (int) v.number[1]);
        default:
            return 1;
    }
}
