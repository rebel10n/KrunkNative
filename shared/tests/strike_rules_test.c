// Krunker Strike's rules (shared/include/strike_rules.h) against their source,
// tools/strike/strike_data.json (FRVR Krunker Strike 1.37.2, extract_strike.py): the generated tables
// (strike_data.c, tools/strike/gen_rules.py) and the engine tables built from them (strike_ruleset(),
// strike_game_config()) carry every class, weapon, mode, serverConfig default and lobby limit the
// engine plays by. A weapon key is checked by writing Strike's value into a copy of the engine's
// weapon (strike_weapon_apply): the copy must not change. FLAVOR_BASE stays Krunker v10.0.2's.
// Run from the repository root: ./<build>/strike_rules_test [path/to/strike_data.json] [-v]
#include <shared.h>
#include <flavor.h>
#include <flavor_rules.h>
#include <game_config.h>
#include <modes.h>
#include <ruleset.h>
#include <strike_rules.h>
#include <cJSON.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static int g_failures, g_checks, g_verbose;
#define CHECK(cond, ...) do { \
    g_checks++; \
    if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_failures++; } \
} while (0)

static int near(const double a, const double b) { return fabs(a - b) <= 1e-5 * fmax(1.0, fabs(b)); }

static cJSON *load_json(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *text = size > 0 ? malloc((size_t) size + 1) : NULL;
    cJSON *json = NULL;
    if (text && fread(text, 1, (size_t) size, f) == (size_t) size) {
        text[size] = 0;
        json = cJSON_Parse(text);
    }
    free(text);
    fclose(f);
    return json;
}

static const cJSON *get(const cJSON *o, const char *key) { return cJSON_GetObjectItemCaseSensitive(o, key); }
static double num(const cJSON *o, const char *key, const double fallback) {
    const cJSON *v = get(o, key);
    if (cJSON_IsBool(v)) return cJSON_IsTrue(v) ? 1.0 : 0.0;
    return cJSON_IsNumber(v) ? v->valuedouble : fallback;
}
static const char *str(const cJSON *o, const char *key) {
    const cJSON *v = get(o, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}
// {"value": n} (an evaluated constant) or n
static double value_of(const cJSON *o, const char *key) {
    const cJSON *v = get(o, key);
    return cJSON_IsObject(v) ? num(v, "value", NAN) : num(o, key, NAN);
}

static void check_classes(const cJSON *data) {
    printf("classes...\n");
    const StrikeRules *r = strike_rules();
    const cJSON *classes = get(data, "classes");
    CHECK((size_t) cJSON_GetArraySize(classes) == r->class_count && r->class_count == 6, "6 classes (json %d, table %zu)",
          cJSON_GetArraySize(classes), r->class_count);
    CHECK(r->default_class == 0, "starter class Triggerman (0)");
    const ClassConfig *table = rules_classes(); // FLAVOR_STRIKE: strike_ruleset()'s
    int seen[CLASS_TABLE_COUNT] = {0};
    const cJSON *c;
    cJSON_ArrayForEach(c, classes) {
        const int id = (int) num(c, "id", -1);
        const cJSON *e = get(c, "effective");
        const StrikeClass *sc = strike_class(NULL, id);
        CHECK(sc != NULL, "class %d in strike_rules()", id);
        if (!sc || id < 0 || (size_t) id >= rules_class_count()) continue;
        seen[id] = 1;
        const cJSON *loadout = get(e, "loadout");
        const int primary = cJSON_GetArraySize(loadout) > 0 ? cJSON_GetArrayItem(loadout, 0)->valueint : -1;
        CHECK(!strcmp(sc->name, str(e, "name")) && sc->health == (int) num(e, "health", -1) && near(sc->regen, num(e, "regen", -1)) &&
                  near(sc->speed, num(e, "speed", -1)) && sc->loadout == primary && sc->secondary == (num(e, "secondary", 0) != 0) &&
                  sc->segments == (int) num(e, "segs", 0),
              "class %d %s: name / health / regen / speed / loadout / secondary / segs", id, sc->name);
        CHECK(near(sc->ground_accel, num(e, "grA", NAN)) && near(sc->air_accel, num(e, "arA", NAN)) && near(sc->ground_decel, num(e, "grDec", NAN)) &&
                  near(sc->air_decel, num(e, "airDec", NAN)) && near(sc->crouch_speed, num(e, "crchSpd", NAN)),
              "class %d movement (grA %g)", id, (double) sc->ground_accel);
        CHECK(sc->wall_jump == (num(e, "wallJ", 0) != 0) && sc->wall_jump_max == (int) num(e, "wallJM", 0) && sc->leap == (num(e, "leap", 0) != 0) &&
                  sc->throw_charge_ms == (int) num(e, "throwChrg", 0),
              "class %d wall jump / leap / throw charge", id);
        CHECK(!sc->secondary && near(sc->regen, 0.15), "class %d: no secondary, regen 0.15", id);
        // the engine's row
        const ClassConfig *cc = &table[id];
        CHECK(!cc->hide && !strcmp(cc->name, sc->name) && cc->health == sc->health && near(cc->regen, sc->regen) && near(cc->speed, sc->speed) &&
                  cc->loadout && cc->loadout[0] == sc->loadout && !cc->secondary && cc->health_segments == sc->segments &&
                  near(cc->movement.ground_accel, sc->ground_accel) && near(cc->movement.crouch_speed, sc->crouch_speed),
              "engine class %d plays Strike's row (%s %d HP)", id, cc->name, cc->health);
        if (g_verbose) printf("  %d %-12s %3d HP speed %.2f weapon %d\n", id, sc->name, sc->health, (double) sc->speed, sc->loadout);
    }
    for (size_t id = 0; id < rules_class_count(); id++) {
        if (seen[id]) continue;
        CHECK(table[id].hide && !flavor_class_allowed((int) id) && flavor_class_pick((int) id) == 0, "class %zu (%s) is not Strike's: hidden, picks fall back",
              id, table[id].name ? table[id].name : "?");
    }
}

// A key the engine leaves at 0 and reads with Krunker's fallback at use: Strike spelling the
// fallback out is the same weapon (player.c: `charge_mlt > 0 ? charge_mlt : 0.9`, chrgMlt ?? .9).
static int engine_default_holds(const Weapon *engine, const char *key, const double value) {
    static const struct { const char *key; double def; } k_defaults[] = {{"chrgMlt", 0.9}};
    for (size_t i = 0; i < sizeof(k_defaults) / sizeof(k_defaults[0]); i++) {
        if (strcmp(key, k_defaults[i].key) || !near(value, k_defaults[i].def)) continue;
        Weapon zero;
        memcpy(&zero, engine, sizeof(zero));
        return strike_weapon_apply(&zero, key, 0.0) && !memcmp(&zero, engine, sizeof(zero));
    }
    return 0;
}

// Every scalar weaponConfig key the engine has a field for: writing Strike's value into a copy of
// the engine's weapon leaves it unchanged.
static void check_weapons(const cJSON *data) {
    printf("weapons...\n");
    const StrikeRules *r = strike_rules();
    const cJSON *weapons = get(data, "weapons");
    CHECK((size_t) cJSON_GetArraySize(weapons) == r->weapon_count && r->weapon_count == 29, "29 weapons (json %d, table %zu)",
          cJSON_GetArraySize(weapons), r->weapon_count);
    CHECK(rules_krunker_weapon_count() == 29 && !rules_weapon(WEAPON_ID_BOULDER) && !rules_weapon(WEAPON_ID_COMPRESSOR),
          "engine: 29 Krunker weapons, no Boulder / Compressor");
    int keys = 0, applied = 0, overrides = 0;
    const cJSON *w;
    cJSON_ArrayForEach(w, weapons) {
        const int id = (int) num(w, "index", -1);
        const StrikeWeapon *sw = strike_weapon(NULL, id);
        const Weapon *engine = rules_weapon(id);
        CHECK(sw && engine && !strcmp(sw->name, str(w, "name")), "weapon %d %s", id, str(w, "name"));
        if (!sw || !engine) continue;
        CHECK(engine->name && !strcmp(engine->name, sw->name), "weapon %d engine name %s (Strike %s)", id, engine->name, sw->name);
        for (size_t k = 0; k < sw->override_count; k++, overrides++) {
            Weapon copy;
            memcpy(&copy, engine, sizeof(copy));
            CHECK(strike_weapon_apply(&copy, sw->overrides[k].key, sw->overrides[k].value) && !memcmp(&copy, engine, sizeof(copy)),
                  "weapon %d %s: override %s = %g is in the engine table", id, sw->name, sw->overrides[k].key, sw->overrides[k].value);
        }
        const cJSON *v;
        cJSON_ArrayForEach(v, get(w, "data")) {
            if (!cJSON_IsNumber(v) && !cJSON_IsBool(v)) continue;
            keys++;
            Weapon copy;
            memcpy(&copy, engine, sizeof(copy));
            const double value = cJSON_IsBool(v) ? (cJSON_IsTrue(v) ? 1.0 : 0.0) : v->valuedouble;
            if (!strike_weapon_apply(&copy, v->string, value)) continue; // no engine field (cosmetic / viewmodel)
            applied++;
            int same = !memcmp(&copy, engine, sizeof(copy));
            if (!same && engine_default_holds(engine, v->string, value)) same = 1;
            CHECK(same, "weapon %d %s: %s = %g differs in the engine table", id, sw->name, v->string, value);
        }
    }
    printf("  %d scalar keys, %d with an engine field, %d overrides of v10\n", keys, applied, overrides);
    CHECK(applied > 29 * 20, "most weapon stats are checked (%d)", applied);
    // spot values (strike_data.json weapons_vs_v10): the Sniper hits harder, the AR fires slower
    CHECK(rules_weapon(0)->damage == 165.0f && near(rules_weapon(0)->reload_time, 1.8), "Sniper: dmg 165, reload 1.8 s");
    CHECK(rules_weapon(1)->ammo == 35 && rules_weapon(1)->damage == 20.0f && near(rules_weapon(1)->rate, 0.13), "AR: ammo 35, dmg 20, rate 130 ms");
}

static void check_modes(const cJSON *data) {
    printf("modes...\n");
    const StrikeRules *r = strike_rules();
    const cJSON *modes = get(get(data, "modes"), "list");
    const cJSON *i18n = get(data, "i18n");
    CHECK((size_t) cJSON_GetArraySize(modes) == r->mode_count && r->mode_count == 35, "35 modes (json %d, table %zu)", cJSON_GetArraySize(modes),
          r->mode_count);
    size_t offered = 0;
    const cJSON *m;
    cJSON_ArrayForEach(m, modes) {
        const char *alias = str(m, "id");
        const int index = (int) num(m, "index", -1);
        const StrikeMode *sm = NULL;
        for (size_t i = 0; i < r->mode_count; i++) {
            if (r->modes[i].index == index) sm = &r->modes[i];
        }
        CHECK(sm && alias && !strcmp(sm->alias, alias) && !strcmp(sm->name, str(m, "name")), "mode %d %s", index, alias ? alias : "?");
        if (!sm) continue;
        const GameModeConfig *engine = mode_config(sm->mode_id);
        CHECK(engine && engine->alias && !strcasecmp(engine->alias, alias), "mode %s is KrunkNative mode %d (%s)", alias, sm->mode_id,
              engine && engine->alias ? engine->alias : "?");
        const cJSON *d = get(m, "data");
        CHECK(sm->score_limit == (int) num(d, "scoreLimit", 0) && sm->teams == (num(m, "teams", 0) != 0) && sm->party == (num(m, "party", 0) != 0),
              "mode %s score limit / teams / party", alias);
        CHECK(engine && engine->score_limit == sm->score_limit && engine->party == sm->party, "engine mode %s score limit %d", alias,
              engine ? engine->score_limit : -1);
        const cJSON *gt = get(d, "gameTime");
        if (cJSON_IsNumber(gt) && engine) CHECK(near(engine->game_time, gt->valuedouble / 1000.0), "engine mode %s game time %g s", alias, engine->game_time);
        const cJSON *order = get(d, "weaponOrder");
        if (cJSON_IsArray(order) && engine) {
            int same = engine->weapon_order_size == cJSON_GetArraySize(order);
            for (int i = 0; same && i < engine->weapon_order_size; i++) same = engine->weapon_order[i] == cJSON_GetArrayItem(order, i)->valueint;
            CHECK(same, "engine mode %s weapon order (%d)", alias, engine->weapon_order_size);
        }
        CHECK(flavor_mode_allowed(sm->mode_id), "mode %s allowed under Strike", alias);
        const char *desc = str(d, "descriptionLang");
        const int is_offered = desc && get(i18n, desc);
        CHECK(sm->offered == is_offered, "mode %s offered %d", alias, is_offered);
        if (is_offered) {
            CHECK(offered < r->rotation_mode_count && r->rotation_modes[offered] == sm->mode_id, "rotation[%zu] = %s", offered, alias);
            offered++;
        }
    }
    CHECK(offered == r->rotation_mode_count, "rotation: %zu offered modes", offered);
    CHECK(!flavor_mode_allowed(MODE_ID_TRADE) && !flavor_mode_allowed(MODE_ID_IMP), "no Trade / Impulsed");
    CHECK(mode_config(MODE_ID_FFA)->score_limit == 1500 && mode_config(MODE_ID_TDM)->score_limit == 2500, "FFA 1500, TDM 2500");
}

static void check_config(const cJSON *data) {
    printf("serverConfig defaults and limits...\n");
    const StrikeRules *r = strike_rules();
    const cJSON *config = get(data, "config");
    size_t n = 0;
    const cJSON *e;
    cJSON_ArrayForEach(e, get(config, "serverConfig")) {
        const char *var = str(e, "varN");
        const cJSON *def = get(e, "def");
        if (!var || (!cJSON_IsNumber(def) && !cJSON_IsBool(def))) continue;
        const StrikeConfigDefault *d = strike_config_default(NULL, var);
        CHECK(d && near(d->def, num(e, "def", NAN)), "serverConfig %s default %g", var, num(e, "def", NAN));
        n++;
    }
    CHECK(n == r->config_default_count, "%zu serverConfig defaults (table %zu)", n, r->config_default_count);

    const cJSON *k = get(config, "constants");
    const cJSON *cap = get(config, "player_cap");
    const cJSON *timing = get(config, "timing");
    CHECK(r->max_players == (int) value_of(k, "maxPlayers") && r->max_players == 10, "lobby cap %d", r->max_players);
    CHECK(r->private_capacity == (int) num(cap, "private_match_capacity", -1) && r->private_capacity == 12, "private capacity %d", r->private_capacity);
    CHECK(r->max_bots == (int) value_of(k, "maxBots") && r->games_per_server == (int) value_of(k, "gamesPerServer") &&
              r->max_players_total == (int) value_of(k, "maxPlayersTotal"),
          "bots %d, games per server %d, total %d", r->max_bots, r->games_per_server, r->max_players_total);
    CHECK(r->server_send_frequency == (int) value_of(k, "serverSendFrequency") && r->game_start_delay_ms == (int) value_of(k, "gameStartDelay") &&
              r->kick_timer_ms == (int) value_of(k, "kickTimer") && near(r->server_tick_ms, value_of(k, "serverTickRate")),
          "tick / send / start delay / kick timer");
    CHECK(r->game_time_min == (int) num(get(timing, "gameTime"), "def", -1) && r->intermission_s == (int) num(get(timing, "intermTmr"), "def", -1),
          "match %d min, intermission %d s", r->game_time_min, r->intermission_s);
    CHECK(!strcmp(r->game_version, str(get(data, "meta"), "game_version")), "game version %s", r->game_version);

    const GameConfig *gc = strike_game_config();
    CHECK(gc == flavor_default_game_config() && gc->max_players == r->max_players && gc->game_time == r->game_time_min, "lobby config: cap %d, %d min",
          gc->max_players, gc->game_time);
    CHECK(near(gc->strafe_speed, strike_config_default(NULL, "strafeSpd")->def), "strafe speed %g", (double) gc->strafe_speed);
    CHECK(gc->no_streaks == (strike_config_default(NULL, "noStreaks")->def != 0) && gc->select_team == (strike_config_default(NULL, "selTeam")->def != 0),
          "streaks / team select");
}

int main(const int argc, char **argv) {
    const char *path = "tools/strike/strike_data.json";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v")) g_verbose = 1;
        else path = argv[i];
    }
    cJSON *data = load_json(path);
    if (!data) {
        printf("strike_rules_test: cannot read %s (run from the repository root)\n", path);
        return 1;
    }

    printf("base flavor...\n");
    flavor_set(FLAVOR_BASE);
    CHECK(ruleset_current() == ruleset_v10() && rules_weapon(WEAPON_ID_BOULDER) && rules_classes()[0].health == 100, "base: Krunker v10.0.2's tables");
    CHECK(mode_config(MODE_ID_FFA)->score_limit == 0 && flavor_mode_allowed(MODE_ID_TRADE), "base: v10 modes");

    flavor_set(FLAVOR_STRIKE);
    CHECK(flavor_current()->rules == strike_rules() && ruleset_current() == strike_ruleset(), "strike: FlavorConfig.rules, strike_ruleset()");
    CHECK(!strcmp(strike_ruleset()->key, ruleset_v10()->key), "strike rides v10's ruleset key");
    check_classes(data);
    check_weapons(data);
    check_modes(data);
    check_config(data);

    flavor_set(FLAVOR_BASE);
    CHECK(ruleset_current() == ruleset_v10() && rules_classes()[0].health == 100 && rules_weapon(0)->damage == 100.0f, "base again: untouched");
    cJSON_Delete(data);
    if (g_failures) {
        printf("strike_rules_test: %d of %d checks failed\n", g_failures, g_checks);
        return 1;
    }
    printf("strike_rules_test: all %d checks passed\n", g_checks);
    return 0;
}
