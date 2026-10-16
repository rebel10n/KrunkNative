// seek_filter_test: what Quick Play, the launch seek, Find New Game and the lobby lists may pick
// (client/include/seek_filter.h, seek_prefs.h). Each mode and map is a switch: the defaults are the
// public rotation (v10.0.2 rotationModes [0, 2, 3, 19, 25] and rotationMaps, v10.js:167202 / :167210),
// a switched-on item is sought, nothing else is, an empty choice is the rotation again, and the saved
// lists read like v10's _69484 with dataQuery dsma / dsmo in v10's order.
#include <seek_filter.h>
#include <flavor.h>
#include <flavor_rules.h>
#include <modes.h>
#include <krunker_maps.h> // KRUNKER_OFFICIAL_MAP_COUNT
#include <ranked.h>       // ranked_pool_map_name
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...)                                       \
    do {                                                       \
        if (!(cond)) {                                         \
            failures++;                                        \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);        \
            printf(__VA_ARGS__);                               \
            printf("\n");                                      \
        }                                                      \
    } while (0)

#define MAP_BURG 0
#define MAP_SANDSTORM 2
#define MAP_SHIPMENT 5 // not in v10's rotationMaps
#define MAP_BAZAAR 32  // in v10's rotationMaps (added in v10)

static int parse_json(const char *json, int *out, int max, int (*valid)(int)) {
    cJSON *v = cJSON_Parse(json);
    const int n = seek_filter_parse(v, out, max, valid);
    cJSON_Delete(v);
    return n;
}

static int below_ten(const int id) { return id < 10; }

// The filter the settings give (seek_prefs.c current_filter): the switched-on lists, or the defaults
// when none is on. on_* = the saved lists (0 = none switched on).
static void make_filter(SeekFilter *f, const int *on_modes, const int on_mode_count, const int *on_maps, const int on_map_count) {
    const int *rot_modes = NULL, *rot_maps = NULL;
    const size_t rot_mode_count = flavor_rotation_modes(FLAVOR_BASE, &rot_modes);
    const size_t rot_map_count = seek_filter_rotation_maps(&rot_maps);
    memset(f, 0, sizeof(*f));
    f->mode_count = seek_filter_effective(on_modes, on_mode_count, rot_modes, (int) rot_mode_count, f->modes, SEEK_FILTER_MAX);
    f->modes_chosen = on_mode_count > 0;
    f->map_count = seek_filter_effective(on_maps, on_map_count, rot_maps, (int) rot_map_count, f->maps, SEEK_FILTER_MAX);
    for (int i = 0; i < f->map_count; i++) f->map_names[i] = f->maps[i] == MAP_BURG ? "Burg" : f->maps[i] == MAP_SANDSTORM ? "Sandstorm" : f->maps[i] == MAP_SHIPMENT ? "Shipment" : NULL;
}

static void test_parse(void) {
    int out[SEEK_FILTER_MAX];
    int n = parse_json("\"0,2,19\"", out, SEEK_FILTER_MAX, NULL);
    CHECK(n == 3 && out[0] == 0 && out[1] == 2 && out[2] == 19, "\"0,2,19\" -> %d entries", n);
    n = parse_json("\"3,,x,5,3\"", out, SEEK_FILTER_MAX, NULL); // NaN entries out, the duplicate too
    CHECK(n == 2 && out[0] == 3 && out[1] == 5, "\"3,,x,5,3\" -> %d entries", n);
    n = parse_json("[1, \"4\", 4, -1, 2.5]", out, SEEK_FILTER_MAX, NULL);
    CHECK(n == 2 && out[0] == 1 && out[1] == 4, "array -> %d entries", n);
    n = parse_json("7", out, SEEK_FILTER_MAX, NULL);
    CHECK(n == 1 && out[0] == 7, "number -> %d", n);
    n = parse_json("\"\"", out, SEEK_FILTER_MAX, NULL);
    CHECK(n == 0, "\"\" (every switch off) -> %d", n);
    n = parse_json("null", out, SEEK_FILTER_MAX, NULL);
    CHECK(n == 0, "null (never switched) -> %d", n);
    n = parse_json("\"4,12,9\"", out, SEEK_FILTER_MAX, below_ten);
    CHECK(n == 2 && out[0] == 4 && out[1] == 9, "valid() filters -> %d", n);
}

// the rotation is v10's own data: rotationModes [0, 2, 3, 19, 25], rotationMaps (34 maps)
static void test_rotation_data(void) {
    const int *modes = NULL;
    const size_t mode_count = flavor_rotation_modes(FLAVOR_BASE, &modes);
    static const int v10_modes[] = {0, 2, 3, 19, 25};
    CHECK(mode_count == 5, "the base flavor's rotation has %zu modes, v10 has 5", mode_count);
    for (size_t i = 0; i < 5 && i < mode_count; i++) CHECK(modes[i] == v10_modes[i], "rotation mode[%zu] = %d, v10 %d", i, modes[i], v10_modes[i]);
    CHECK(MODE_ID_FFA == 0 && MODE_ID_POINT == 2 && MODE_ID_CTF == 3 && MODE_ID_KC == 19 && MODE_ID_DOM == 25, "mode ids are Krunker's indices");
    CHECK(MODE_ID_TRADE == 18, "Trade is mode 18 (the dsmo of a Trade pick)");
    const int *maps = NULL;
    const size_t map_count = seek_filter_rotation_maps(&maps);
    CHECK(map_count == 34, "v10's rotationMaps has %zu maps, v10 has 34", map_count);
    CHECK(maps[0] == 0 && maps[map_count - 1] == 45, "rotationMaps runs from Burg (0) to Barnyard (45)");
}

// the defaults: every rotation switch on, every other switch off
static void test_default_is_rotation(void) {
    SeekFilter f;
    make_filter(&f, NULL, 0, NULL, 0);
    CHECK(f.modes_chosen == 0, "nothing switched on: no hosted lobby is sought");
    CHECK(seek_filter_lobby_ok(&f, MODE_ID_FFA, "Burg", 0), "default: Free for All on Burg is sought");
    CHECK(seek_filter_lobby_ok(&f, MODE_ID_DOM, "Burg", 0), "default: Domination is sought");
    CHECK(seek_filter_lobby_ok(&f, MODE_ID_FFA, "Sandstorm", 0), "default: a rotation map is sought");
    CHECK(!seek_filter_lobby_ok(&f, MODE_ID_TRADE, "Burg", 0), "default: never Trade");
    CHECK(!seek_filter_lobby_ok(&f, MODE_ID_BHOP, "Burg", 0), "default: never Parkour");
    CHECK(!seek_filter_lobby_ok(&f, MODE_ID_TDM, "Burg", 0), "default: never TDM (v10 took it out of the rotation)");
    CHECK(!seek_filter_lobby_ok(&f, MODE_ID_FFA, "Shipment", 0), "default: never a map outside the rotation (Shipment)");
    CHECK(!seek_filter_lobby_ok(&f, -1, "Burg", 0), "an unknown mode is never sought");
    CHECK(!seek_filter_lobby_ok(&f, MODE_ID_FFA, "Burg", SEEK_LOBBY_CUSTOM), "a custom game is never sought");
    CHECK(!seek_filter_lobby_ok(&f, MODE_ID_FFA, "Burg", SEEK_LOBBY_PASSWORD), "a password lobby is never sought");
    CHECK(!seek_filter_lobby_ok(&f, MODE_ID_FFA, "Burg", SEEK_LOBBY_HOSTED), "default: no official custom / dedicated lobby");
}

// a switched-on mode and map (outside the rotation) are sought; the others are not
static void test_switched_on(void) {
    const int *rot_modes = NULL, *rot_maps = NULL;
    const int rot_mode_count = (int) flavor_rotation_modes(FLAVOR_BASE, &rot_modes);
    const int rot_map_count = (int) seek_filter_rotation_maps(&rot_maps);
    int on_modes[SEEK_FILTER_MAX], on_maps[SEEK_FILTER_MAX], out[SEEK_FILTER_MAX];
    // Trade switched on from the rotation: the rotation plus 18, ascending
    int n_modes = seek_filter_toggle(rot_modes, rot_mode_count, rot_modes, rot_mode_count, MODE_ID_TRADE, 1, out, SEEK_FILTER_MAX);
    memcpy(on_modes, out, sizeof(int) * (size_t) n_modes);
    CHECK(n_modes == 6 && out[0] == 0 && out[3] == 18 && out[5] == 25, "Trade switched on: rotation plus 18 (ascending) -> %d entries", n_modes);
    // Shipment switched on from the map rotation: the rotation plus 5, ascending
    int n_maps = seek_filter_toggle(rot_maps, rot_map_count, rot_maps, rot_map_count, MAP_SHIPMENT, 1, out, SEEK_FILTER_MAX);
    memcpy(on_maps, out, sizeof(int) * (size_t) n_maps);
    CHECK(n_maps == rot_map_count + 1 && on_maps[4] == 4 && on_maps[5] == MAP_SHIPMENT && on_maps[6] == 6, "Shipment switched on, in order among the rotation: %d entries", n_maps);
    SeekFilter f;
    make_filter(&f, on_modes, n_modes, on_maps, n_maps);
    CHECK(seek_filter_lobby_ok(&f, MODE_ID_TRADE, "Burg", 0), "Trade switched on: a Trade lobby is sought");
    CHECK(seek_filter_lobby_ok(&f, MODE_ID_TRADE, "Burg", SEEK_LOBBY_HOSTED), "Trade switched on: a hosted Trade lobby too");
    CHECK(seek_filter_lobby_ok(&f, MODE_ID_FFA, "Shipment", 0), "Shipment switched on: Shipment is sought");
    CHECK(seek_filter_lobby_ok(&f, MODE_ID_FFA, "Burg", 0), "the rotation stays on with Trade and Shipment on");
    CHECK(!seek_filter_lobby_ok(&f, MODE_ID_TDM, "Burg", 0), "a mode not switched on (TDM) is still not sought");
    CHECK(!seek_filter_lobby_ok(&f, MODE_ID_TRADE, "Burg", SEEK_LOBBY_CUSTOM), "Trade switched on: still never a custom game");
    CHECK(!seek_filter_lobby_ok(&f, MODE_ID_TRADE, "Burg", SEEK_LOBBY_PASSWORD), "Trade switched on: still never a password lobby");
    // Free for All switched off: not sought; Trade still is
    n_modes = seek_filter_toggle(on_modes, n_modes, rot_modes, rot_mode_count, MODE_ID_FFA, 0, out, SEEK_FILTER_MAX);
    memcpy(on_modes, out, sizeof(int) * (size_t) n_modes);
    make_filter(&f, on_modes, n_modes, on_maps, n_maps);
    CHECK(!seek_filter_lobby_ok(&f, MODE_ID_FFA, "Burg", 0), "FFA switched off: not sought");
    CHECK(seek_filter_lobby_ok(&f, MODE_ID_TRADE, "Burg", 0), "FFA switched off: Trade still sought");
    // Burg switched off: not sought; Shipment and Sandstorm still are
    n_maps = seek_filter_toggle(on_maps, n_maps, rot_maps, rot_map_count, MAP_BURG, 0, out, SEEK_FILTER_MAX);
    memcpy(on_maps, out, sizeof(int) * (size_t) n_maps);
    make_filter(&f, on_modes, n_modes, on_maps, n_maps);
    CHECK(!seek_filter_lobby_ok(&f, MODE_ID_TRADE, "Burg", 0), "Burg switched off: not sought");
    CHECK(seek_filter_lobby_ok(&f, MODE_ID_TRADE, "Shipment", 0), "Shipment switched on: sought");
    CHECK(seek_filter_lobby_ok(&f, MODE_ID_TRADE, "sandstorm", 0), "a switched-on map matches without case");
}

// the picks are never empty for a seek: all switches off is the rotation again
static void test_empty_falls_back(void) {
    int on_modes[SEEK_FILTER_MAX], out[SEEK_FILTER_MAX];
    SeekFilter f;
    // every switch off: the toggle result is empty, which the settings store as "" (never an empty pool)
    const int *rot = NULL;
    const int rot_count = (int) flavor_rotation_modes(FLAVOR_BASE, &rot);
    int n = seek_filter_toggle(rot, rot_count, rot, rot_count, MODE_ID_FFA, 0, out, SEEK_FILTER_MAX);
    CHECK(n == rot_count - 1, "FFA switched off from the rotation: %d left", n);
    int left = n;
    memcpy(on_modes, out, sizeof(int) * (size_t) n);
    while (left > 0) { // switch the rest off one by one
        n = seek_filter_toggle(on_modes, left, rot, rot_count, on_modes[0], 0, out, SEEK_FILTER_MAX);
        memcpy(on_modes, out, sizeof(int) * (size_t) n);
        left = n;
    }
    CHECK(left == 0, "every switch off -> none switched on (%d)", left);
    make_filter(&f, NULL, 0, NULL, 0); // the empty saved list reads as none on: the rotation
    CHECK(f.mode_count == rot_count && f.modes[0] == 0 && f.modes[4] == 25, "empty choice: the rotation is sought (%d modes)", f.mode_count);
    CHECK(!f.modes_chosen, "empty choice: hosted lobbies are not sought");
    CHECK(seek_filter_lobby_ok(&f, MODE_ID_FFA, "Burg", 0), "empty choice: Free for All is sought");
    char dq[1200];
    seek_filter_dq_fields(&f, dq, sizeof(dq));
    CHECK(strstr(dq, ",\"dsmo\":[0,2,3,19,25]") != NULL, "empty choice: dsmo is the rotation -> '%.60s...'", dq);
    CHECK(strstr(dq, ",\"dsma\":[0,1,2,") != NULL, "empty choice: dsma is the map rotation");
    CHECK(strstr(dq, "[]") == NULL, "no empty list is ever sent");
}

static void test_toggle_order(void) {
    int out[SEEK_FILTER_MAX];
    const int saved[] = {25, 2};
    const int n = seek_filter_toggle(saved, 2, NULL, 0, 18, 1, out, SEEK_FILTER_MAX);
    CHECK(n == 3 && out[0] == 2 && out[1] == 18 && out[2] == 25, "toggle keeps the list ascending (%d)", n);
    const int m = seek_filter_toggle(saved, 2, NULL, 0, 2, 1, out, SEEK_FILTER_MAX);
    CHECK(m == 2 && out[0] == 2 && out[1] == 25, "switching on an entry already on adds nothing (%d)", m);
    const int k = seek_filter_toggle(saved, 2, NULL, 0, 99, 0, out, SEEK_FILTER_MAX);
    CHECK(k == 2 && out[0] == 2 && out[1] == 25, "an unknown id switched off changes nothing (%d)", k);
}

static void test_flags(void) {
    cJSON *t = cJSON_CreateTrue(), *one = cJSON_CreateNumber(1), *zero = cJSON_CreateNumber(0), *no = cJSON_CreateFalse();
    CHECK(seek_filter_flag(t) && seek_filter_flag(one), "krunker.io's flags: true and 1 are set");
    CHECK(!seek_filter_flag(zero) && !seek_filter_flag(no) && !seek_filter_flag(NULL), "0, false and absent are not");
    cJSON_Delete(t);
    cJSON_Delete(one);
    cJSON_Delete(zero);
    cJSON_Delete(no);
}

// dsma then dsmo, v10's order; a switched-on Trade and Shipment are sent as their ids
static void test_data_query(void) {
    SeekFilter f;
    make_filter(&f, NULL, 0, NULL, 0);
    char out[1200];
    seek_filter_dq_fields(&f, out, sizeof(out));
    CHECK(strncmp(out, ",\"dsma\":[0,1,2,3,4,6,7,8,9,10,11,12,13,14,15,16,19,21,22,23,24,25,26,28,32,33,34,36,37,39,40,42,44,45],\"dsmo\":[0,2,3,19,25]", 110) == 0,
          "default -> the rotation's dsma and dsmo -> '%.80s...'", out);
    const int trade[] = {0, 2, 3, 18, 19, 25};
    const int shipment[] = {0, 1, 2, 3, 4, 5, 6};
    make_filter(&f, trade, 6, shipment, 7);
    seek_filter_dq_fields(&f, out, sizeof(out));
    CHECK(!strcmp(out, ",\"dsma\":[0,1,2,3,4,5,6],\"dsmo\":[0,2,3,18,19,25]"), "switched on: dsma then dsmo, v10's order -> '%s'", out);
    seek_filter_dq_fields(&f, out, 8);
    CHECK(strlen(out) < 8, "a short buffer is never overrun (%zu)", strlen(out));
}

// hosted (official custom / dedicated) lobbies need a mode the player switched on, and never the rotation alone
static void test_hosted(void) {
    SeekFilter f;
    make_filter(&f, NULL, 0, NULL, 0);
    CHECK(!seek_filter_lobby_ok(&f, MODE_ID_FFA, "Burg", SEEK_LOBBY_HOSTED), "rotation only: no hosted lobby");
    const int ffa[] = {0};
    make_filter(&f, ffa, 1, NULL, 0);
    CHECK(seek_filter_lobby_ok(&f, MODE_ID_FFA, "Burg", SEEK_LOBBY_HOSTED), "FFA switched on: a hosted FFA lobby is sought");
    CHECK(!seek_filter_lobby_ok(&f, MODE_ID_DOM, "Burg", SEEK_LOBBY_HOSTED), "FFA switched on: a hosted Domination lobby is not");
}

// ---- Quickplay presets (seek_filter.h SeekPreset): the fixed lists, and applying one saves the text the
// switches would save for the same choice, so the switches show it and a switch after it works as before

static int has_id(const int *list, const int n, const int v) {
    for (int i = 0; i < n; i++) {
        if (list[i] == v) return 1;
    }
    return 0;
}

// The saved list after a player flips the switches from the rotation (nothing saved) to `want`: each switch
// on the rotation and not wanted is turned off, each wanted switch not on the rotation is turned on, every
// step through seek_filter_toggle as Settings > Quickplay saves it. Returns the count written to out.
static int flip_from_rotation(const int *rot, const int rot_n, const int *want, const int want_n, int *out) {
    int saved[SEEK_FILTER_MAX], tmp[SEEK_FILTER_MAX], n = 0;
    for (int i = 0; i < rot_n; i++) {
        if (has_id(want, want_n, rot[i])) continue;
        const int m = seek_filter_toggle(saved, n, rot, rot_n, rot[i], 0, tmp, SEEK_FILTER_MAX);
        memcpy(saved, tmp, sizeof(int) * (size_t) m);
        n = m;
    }
    // when every switch was turned off above, saved is empty and the toggle reads the rotation again (as the page does)
    for (int i = 0; i < want_n; i++) {
        if (has_id(rot, rot_n, want[i])) continue;
        const int m = seek_filter_toggle(saved, n, rot, rot_n, want[i], 1, tmp, SEEK_FILTER_MAX);
        memcpy(saved, tmp, sizeof(int) * (size_t) m);
        n = m;
    }
    memcpy(out, saved, sizeof(int) * (size_t) n);
    return n;
}

static void test_presets(void) {
    SeekPresetLists p[SEEK_PRESET_COUNT];
    char text[512];
    CHECK(SEEK_PRESET_COUNT == 5, "five presets, %d", SEEK_PRESET_COUNT);
    CHECK(!seek_preset_lists(SEEK_PRESET_COUNT, &p[0]) && !seek_preset_lists(-1, &p[0]), "an out of range preset has no lists");
    CHECK(seek_preset_key(SEEK_PRESET_COUNT) == NULL && seek_preset_key(-1) == NULL, "an out of range preset has no key");
    static const char *const keys[SEEK_PRESET_COUNT] = {"rotation", "team", "ffa", "ranked", "any"};
    for (int i = 0; i < SEEK_PRESET_COUNT; i++) {
        CHECK(seek_preset_lists(i, &p[i]), "preset %d has lists", i);
        CHECK(seek_preset_key(i) && !strcmp(seek_preset_key(i), keys[i]), "preset %d is \"%s\"", i, keys[i]);
        // every entry in range, ascending, no duplicates
        for (int k = 0; k < p[i].mode_count; k++) {
            CHECK(p[i].modes[k] >= 0 && p[i].modes[k] < MODE_COUNT, "%s: mode %d in range", keys[i], p[i].modes[k]);
            CHECK(k == 0 || p[i].modes[k - 1] < p[i].modes[k], "%s: modes ascending", keys[i]);
        }
        for (int k = 0; k < p[i].map_count; k++) {
            CHECK(p[i].maps[k] >= 0 && p[i].maps[k] < KRUNKER_OFFICIAL_MAP_COUNT, "%s: map %d in range", keys[i], p[i].maps[k]);
            CHECK(k == 0 || p[i].maps[k - 1] < p[i].maps[k], "%s: maps ascending", keys[i]);
        }
    }
    // the lists the table names (modes.h ids, the Ranked pool of ranked.h)
    CHECK(p[SEEK_PRESET_ROTATION].mode_count == 0 && p[SEEK_PRESET_ROTATION].map_count == 0, "rotation: both lists empty (the rotation)");
    seek_filter_list_text(p[SEEK_PRESET_TEAM].modes, p[SEEK_PRESET_TEAM].mode_count, text, sizeof(text));
    CHECK(!strcmp(text, "1,2,3,19,25,26") && p[SEEK_PRESET_TEAM].map_count == 0, "team: modes \"%s\" (TDM, Hardpoint, CTF, KC, Domination, SDM), rotation maps", text);
    seek_filter_list_text(p[SEEK_PRESET_FFA].modes, p[SEEK_PRESET_FFA].mode_count, text, sizeof(text));
    CHECK(!strcmp(text, "0,8,10,13,27,34") && p[SEEK_PRESET_FFA].map_count == 0, "ffa: modes \"%s\" (FFA, LMS, Gun Game, Classic FFA, Kranked, Bighead), rotation maps", text);
    seek_filter_list_text(p[SEEK_PRESET_RANKED].modes, p[SEEK_PRESET_RANKED].mode_count, text, sizeof(text));
    CHECK(!strcmp(text, "2") && p[SEEK_PRESET_RANKED].map_count == 7, "ranked: Hardpoint only (\"%s\"), the 7-map Ranked pool", text);
    for (int k = 0; k < p[SEEK_PRESET_RANKED].map_count; k++) {
        CHECK(ranked_pool_map_name(p[SEEK_PRESET_RANKED].maps[k]) != NULL, "ranked: map %d is in the Ranked pool", p[SEEK_PRESET_RANKED].maps[k]);
    }
    CHECK(p[SEEK_PRESET_ANY].mode_count == MODE_COUNT && p[SEEK_PRESET_ANY].map_count == KRUNKER_OFFICIAL_MAP_COUNT,
          "any: every mode (%d) and every official map (%d)", p[SEEK_PRESET_ANY].mode_count, p[SEEK_PRESET_ANY].map_count);
    // the list text: "" for none, the entries that fit otherwise
    CHECK(seek_filter_list_text(NULL, 0, text, sizeof(text)) == 0 && text[0] == 0, "no entries: \"\" (the rotation)");
    const int three[] = {1, 2, 3};
    CHECK(seek_filter_list_text(three, 3, text, 4) == 3 && !strcmp(text, "1,2"), "a list that does not fit keeps the whole entries (\"%s\")", text);
}

// applying a preset saves what the switches save for the same choice (the same text), for modes and maps
static void test_presets_match_switches(void) {
    const int *rot_modes = NULL, *rot_maps = NULL;
    const int rot_mode_n = (int) flavor_rotation_modes(FLAVOR_BASE, &rot_modes);
    const int rot_map_n = (int) seek_filter_rotation_maps(&rot_maps);
    static const char *const keys[SEEK_PRESET_COUNT] = {"rotation", "team", "ffa", "ranked", "any"};
    for (int i = 0; i < SEEK_PRESET_COUNT; i++) {
        SeekPresetLists p;
        seek_preset_lists(i, &p);
        int flipped[SEEK_FILTER_MAX];
        char switches[512], preset[512];
        int n = flip_from_rotation(rot_modes, rot_mode_n, p.modes, p.mode_count, flipped);
        seek_filter_list_text(flipped, n, switches, sizeof(switches));
        seek_filter_list_text(p.modes, p.mode_count, preset, sizeof(preset));
        CHECK(!strcmp(switches, preset), "%s: the switches save modes \"%s\", the preset \"%s\"", keys[i], switches, preset);
        n = flip_from_rotation(rot_maps, rot_map_n, p.maps, p.map_count, flipped);
        seek_filter_list_text(flipped, n, switches, sizeof(switches));
        seek_filter_list_text(p.maps, p.map_count, preset, sizeof(preset));
        CHECK(!strcmp(switches, preset), "%s: the switches save maps \"%s\", the preset \"%s\"", keys[i], switches, preset);
        // what the seek then looks for is the same too (the effective lists)
        int eff_a[SEEK_FILTER_MAX], eff_b[SEEK_FILTER_MAX];
        const int na = seek_filter_effective(flipped, n, rot_maps, rot_map_n, eff_a, SEEK_FILTER_MAX);
        const int nb = seek_filter_effective(p.maps, p.map_count, rot_maps, rot_map_n, eff_b, SEEK_FILTER_MAX);
        CHECK(na == nb && !memcmp(eff_a, eff_b, sizeof(int) * (size_t) na), "%s: the same maps are sought", keys[i]);
    }
    // a preset is a starting point: a switch after it still changes the saved list (the team preset plus Parkour)
    SeekPresetLists team;
    seek_preset_lists(SEEK_PRESET_TEAM, &team);
    int out[SEEK_FILTER_MAX], n = seek_filter_toggle(team.modes, team.mode_count, rot_modes, rot_mode_n, MODE_ID_BHOP, 1, out, SEEK_FILTER_MAX);
    char text[512];
    seek_filter_list_text(out, n, text, sizeof(text));
    CHECK(!strcmp(text, "1,2,3,4,19,25,26"), "team preset, then Parkour switched on: \"%s\"", text);
}

int main(void) {
    test_parse();
    test_rotation_data();
    test_default_is_rotation();
    test_switched_on();
    test_empty_falls_back();
    test_toggle_order();
    test_flags();
    test_data_query();
    test_hosted();
    test_presets();
    test_presets_match_switches();
    if (failures) {
        printf("seek_filter_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("seek_filter_test: ok\n");
    return 0;
}
