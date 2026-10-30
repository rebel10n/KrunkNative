// network_sounds_test: config.networkSounds of Krunker v10.0.5 (shared/include/network_sounds.h) and the
// sound references old maps hold.
//
//   - the table against shared/tests/data/v10/networkSounds.json (tools/krunker/extract_sounds.py from
//     deob-10.0.5.js:18263) and, when the decoded bundle is on this machine, against the bundle itself
//     (KN_DEOB_1005, default ~/jails/kru/src/Krunker_v10.0.5/deob/game.deob.js)
//   - the ids the bundle sends by name (its indexOf calls), config.taunts' ids, the weapons' shot sounds
//   - the id space: v7.1.4's 265 names and v3.1.1's 51 are a prefix (one v3.1.1 slot differs); checked
//     against those reference trees too when they are present
//   - old maps: a v3.1.1-shaped trigger, the v3.1.1 Soul Sanctum, a v7.1.4 custom map, the official
//     maps' SOUND_EMITTERs, and every map under assets/maps: each sound reference resolves
//     (trigger_sound_name / trigger_emitter_parse, shared/include/triggers.h)
//
// Run from the repository root (or its build directory).
#include <shared.h>
#include <network_sounds.h>
#include <ruleset.h>
#include <social_v7.h>
#include <triggers.h>
#include <weapons.h>

#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
static int checks = 0;

#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const char *g_root = "";

static cJSON *load_json(const char *relative) {
    char path[512];
    snprintf(path, sizeof(path), "%s%s", g_root, relative);
    unsigned char *data;
    size_t length;
    if (read_file(path, &data, &length)) return NULL;
    cJSON *parsed = cJSON_Parse((char *) data);
    free(data);
    return parsed;
}

// The string literals of the array that opens at `anchor` in a JS file: NULL when the file or the
// anchor is missing. *line = the 1-based line of the anchor.
static char **js_string_array(const char *path, const char *anchor, int *count, int *line) {
    unsigned char *data;
    size_t length;
    *count = 0;
    if (read_file(path, &data, &length)) return NULL;
    char *text = (char *) data;
    char *at = strstr(text, anchor);
    if (!at) {
        free(data);
        return NULL;
    }
    *line = 1;
    for (const char *c = text; c < at; c++) *line += *c == '\n';
    char **names = NULL;
    int n = 0, capacity = 0;
    char *p = strchr(at, '[') + 1;
    while (*p) {
        while (*p == ' ' || *p == ',' || *p == '\n') p++;
        if (*p != '"' && *p != '\'') break; // the 10.0.4/10.0.5 bundles mix single and double quotes
        const char quote = *p;
        char *end = strchr(p + 1, quote);
        if (!end) break;
        if (n == capacity) {
            capacity = capacity ? capacity * 2 : 512;
            names = realloc(names, (size_t) capacity * sizeof(char *));
        }
        names[n] = calloc(1, (size_t) (end - p));
        memcpy(names[n++], p + 1, (size_t) (end - p - 1));
        p = end + 1;
    }
    free(data);
    *count = n;
    return names;
}

static void free_names(char **names, const int count) {
    for (int i = 0; i < count; i++) free(names[i]);
    free(names);
}

static void check_fixture(void) {
    printf("Running the table against networkSounds.json...\n");
    cJSON *fixture = load_json("shared/tests/data/v10/networkSounds.json");
    CHECK(fixture != NULL, "missing shared/tests/data/v10/networkSounds.json (regenerate: python3 -I tools/krunker/extract_sounds.py <deob>)");
    if (!fixture) return;
    const cJSON *list = cJSON_GetObjectItem(fixture, "networkSounds");
    const cJSON *source = cJSON_GetObjectItem(fixture, "source");
    CHECK(cJSON_IsString(source) && !strcmp(source->valuestring, "deob-10.0.5.js:18263"), "the fixture is deob-10.0.5.js:18263's (%s)",
          cJSON_IsString(source) ? source->valuestring : "?");
    CHECK(cJSON_GetArraySize(list) == NETWORK_SOUND_COUNT, "%d names in the fixture, %d in the table", cJSON_GetArraySize(list), NETWORK_SOUND_COUNT);
    int id = 0, differ = 0;
    const cJSON *name;
    cJSON_ArrayForEach(name, list) {
        const char *ours = network_sound_name(id);
        if (!cJSON_IsString(name) || !ours || strcmp(ours, name->valuestring)) {
            if (differ++ < 5) CHECK(0, "id %d: fixture %s, table %s", id, cJSON_IsString(name) ? name->valuestring : "?", ours ? ours : "NULL");
        }
        id++;
    }
    CHECK(!differ, "%d ids differ from the fixture", differ);
    cJSON_Delete(fixture);
}

static void check_bundle(void) {
    char path[512];
    const char *env = getenv("KN_DEOB_1005");
    const char *home = getenv("HOME");
    if (env && env[0]) snprintf(path, sizeof(path), "%s", env);
    else snprintf(path, sizeof(path), "%s/jails/kru/src/Krunker_v10.0.5/deob/game.deob.js", home ? home : "");
    int count = 0, line = 0;
    FILE *probe = fopen(path, "rb");
    if (!probe) {
        printf("Skipping the deob-10.0.5.js check (no %s)\n", path);
        return;
    }
    fclose(probe);
    // the decoder's quote style changes between builds (10.0.4 opened with 'spray', 10.0.5 with "spray")
    char **names = js_string_array(path, "[\"spray\", \"taunt_0\"", &count, &line);
    CHECK(names != NULL, "config.networkSounds' literal is in %s", path);
    if (!names) return;
    printf("Running the table against %s...\n", path);
    CHECK(line == 18263, "config.networkSounds opens on line %d (the citations say :18263)", line);
    CHECK(count == NETWORK_SOUND_COUNT, "the bundle lists %d names", count);
    int differ = 0;
    for (int i = 0; i < count && i < NETWORK_SOUND_COUNT; i++) {
        if (strcmp(names[i], g_network_sounds[i]) && differ++ < 5) CHECK(0, "id %d: bundle %s, table %s", i, names[i], g_network_sounds[i]);
    }
    CHECK(!differ, "%d ids differ from the bundle", differ);
    free_names(names, count);
}

// The ids the bundle derives from names (networkSounds.indexOf) and the ids it writes down.
static void check_ids(void) {
    printf("Running the bundle's ids...\n");
    static const struct {
        const char *name;
        int id;
        const char *where;
    } k_ids[] = {
        {"spray", 0, "players.spray :195672"},
        {"collect_1", 35, "medals onPickup :176579"},
        {"headshot_0", 37, "getKillSnd :198642"},
        {"step_0", 48, "players step :195240"},
        {"adrop_1", 77, "airDrop :203915"},
        {"gift_0", 177, "medals :176623"},
        {"egg_1", 200, "medals :176749"},
        {"zap_0", 206, "\"so\" getSoundIndex(\"zap_0\") :197936"},
        {"reload_3", 217, "medals :176815"},
        {"weapon_29_blast", 221, "shoot weapon.sound + \"_blast\" :196406"},
        {"bhop_reset", 230, "players :196118"},
        {"weapon_upgraded", 242, "upgradeWeapons sound :202655"},
        {"aim_1", 264, "medals :177267 (v7.1.4's last name)"},
        {"weapon_31", 265, "v10's first name past v7.1.4"},
        {"staunt_0", 372, "stalkerTaunt addPlayerSound"},
        {"weapon_4_36", 398, "the last name"},
    };
    for (size_t i = 0; i < sizeof(k_ids) / sizeof(k_ids[0]); i++) {
        CHECK(network_sound_index(k_ids[i].name) == k_ids[i].id && !strcmp(network_sound_name(k_ids[i].id), k_ids[i].name),
              "%s is %d (%s): %d", k_ids[i].name, k_ids[i].id, k_ids[i].where, network_sound_index(k_ids[i].name));
    }
    // config.taunts {nm, id, tm} (deob-10.0.3.js:21244): players.taunt plays id (:198329), nm is its name
    static const int k_taunt_ids[TAUNT_COUNT] = {1, 2, 3, 4, 28, 104};
    for (int i = 0; i < TAUNT_COUNT; i++) {
        CHECK(network_sound_index(g_taunts[i].sound) == k_taunt_ids[i], "taunt %d %s: id %d (config.taunts says %d)", i, g_taunts[i].sound,
              network_sound_index(g_taunts[i].sound), k_taunt_ids[i]);
    }
    // a name the list lacks has no id: the melee pickup's indexOf("melee_equip") (:176880) is -1
    CHECK(network_sound_index("melee_equip") == -1 && network_sound_index("spray_2") == -1 && network_sound_index(NULL) == -1 &&
              network_sound_index("") == -1, "melee_equip / spray_2 / NULL / \"\" have no id");
    CHECK(!network_sound_name(-1) && !network_sound_name(NETWORK_SOUND_COUNT) && network_sound_name(NETWORK_SOUND_COUNT - 1),
          "network_sound_name is bounds-checked");
    // weapon_17_4 twice: indexOf answers the first
    CHECK(network_sound_index("weapon_17_4") == 135 && !strcmp(network_sound_name(155), "weapon_17_4"), "weapon_17_4 at 135 and 155");
    int duplicates = 0;
    for (int i = 0; i < NETWORK_SOUND_COUNT; i++) duplicates += network_sound_index(g_network_sounds[i]) != i;
    CHECK(duplicates == 1, "one duplicate name (%d)", duplicates);
    // the sound packets carry names: every one fits the patch-7 field, only id 267 inspect_monte_carlo_spin is past the V1 one
    int too_long = 0, past_v1 = 0;
    for (int i = 0; i < NETWORK_SOUND_COUNT; i++) {
        too_long += strlen(g_network_sounds[i]) >= NET_SOUND_NAME_SIZE;
        past_v1 += strlen(g_network_sounds[i]) >= NET_SOUND_NAME_SIZE_V1;
    }
    CHECK(!too_long && past_v1 == 1 && strlen(network_sound_name(267)) == NET_SOUND_NAME_SIZE_V1,
          "every name fits NET_SOUND_NAME_SIZE (%d past it), only 267 is past the V1 field (%d)", too_long, past_v1);

    // shoot / tryGrapple: networkSounds.indexOf(weapon.sound) (:196406, :196177): every Krunker gun with a
    // sound has an id (the knife's "melee" is played by the client's swing, not sent)
    const Weapon *const *weapons = rules_weapons();
    for (size_t i = 0; i < rules_weapon_count(); i++) {
        const Weapon *w = weapons[i]; // the index is the weapon id
        if (!w || i >= WEAPON_ID_KN_BASE || w->melee || !w->sound || !w->sound[0]) continue;
        CHECK(network_sound_index(w->sound) >= 0, "weapon %zu %s: its sound %s has an id", i, w->name ? w->name : "?", w->sound);
    }
}

// The sound packets' patch-7 layout (64-byte names) and the V1 one older peers get (shared.h).
static void check_wire(void) {
    printf("Running the sound packets' V1 / V2 layouts...\n");
    CHECK(!net_peer_has_sound_v2(NET_PROTOCOL_MINOR_EXT, NET_PROTOCOL_PATCH_SOUND_V2 - 1) && net_peer_has_sound_v2(NET_PROTOCOL_MINOR_EXT, 7) &&
              !net_peer_has_sound_v2(NET_PROTOCOL_MINOR_V7, 9) && net_peer_has_sound_v2(NET_PROTOCOL_MINOR_EXT + 1, 0) &&
              NET_PROTOCOL_PATCH >= NET_PROTOCOL_PATCH_SOUND_V2,
          "net_peer_has_sound_v2: 1.4.7 and later");
    CHECK(sizeof(NetSoundPacket) == 88 && sizeof(NetStopSoundPacket) == 65 && sizeof(NetPlayerSoundPacket) == 79, "V2 sizes %zu %zu %zu",
          sizeof(NetSoundPacket), sizeof(NetStopSoundPacket), sizeof(NetPlayerSoundPacket));

    uint8_t v1[NET_SOUND_PACKET_V1_MAX];
    NetSoundPacket s = {.name = "weapon_1", .uid = 7, .position = {1, 2, 3}, .volume = 0.85f, .rate = 1.05f}, back;
    uint16_t n = net_sound_packet_to_v1(NET_PACKET_SOUND, &s, sizeof(s), v1);
    int32_t uid;
    float rate;
    memcpy(&uid, v1 + 24, 4);
    memcpy(&rate, v1 + 44, 4);
    CHECK(n == NET_SOUND_PACKET_V1_SIZE && !strcmp((const char *) v1, "weapon_1") && !v1[23] && uid == 7 && rate == 1.05f,
          "SOUND V1: name @0 (24), uid @24, rate @44 (%u)", n);
    memset(&back, 0x55, sizeof(back));
    CHECK(net_sound_packet_read(NET_PACKET_SOUND, v1, n, &back) && !strcmp(back.name, "weapon_1") && back.uid == 7 && back.position[2] == 3.0f &&
              back.volume == 0.85f && back.rate == 1.05f && !back.name[NET_SOUND_NAME_SIZE - 1],
          "SOUND V1 reads back");
    snprintf(s.name, sizeof(s.name), "%s", network_sound_name(267));
    CHECK(!net_sound_packet_to_v1(NET_PACKET_SOUND, &s, sizeof(s), v1), "a name past 23 characters is not sent to a V1 peer");
    CHECK(net_sound_packet_read(NET_PACKET_SOUND, &s, sizeof(s), &back) && !strcmp(back.name, "inspect_monte_carlo_spin"), "SOUND V2 reads");

    NetStopSoundPacket stop = {.name = "asset:30001", .asset = 1}, stop_back;
    n = net_sound_packet_to_v1(NET_PACKET_STOP_SOUND, &stop, sizeof(stop), v1);
    CHECK(n == NET_STOP_SOUND_PACKET_V1_SIZE && v1[24] == 1 && net_sound_packet_read(NET_PACKET_STOP_SOUND, v1, n, &stop_back) &&
              !strcmp(stop_back.name, "asset:30001") && stop_back.asset == 1,
          "STOP_SOUND V1: name @0, asset @24 (%u)", n);

    NetPlayerSoundPacket ps = {.uid = 3, .add = 1, .force = 0, .loop = 1, .volume = 0.5f, .rate = 1.25f, .name = "jug_0"}, ps_back;
    n = net_sound_packet_to_v1(NET_PACKET_PLAYER_SOUND, &ps, sizeof(ps), v1);
    CHECK(n == NET_PLAYER_SOUND_PACKET_V1_SIZE && !strcmp((const char *) v1 + 15, "jug_0") && v1[6] == 1 &&
              net_sound_packet_read(NET_PACKET_PLAYER_SOUND, v1, n, &ps_back) && ps_back.uid == 3 && ps_back.add == 1 && ps_back.loop == 1 &&
              ps_back.volume == 0.5f && ps_back.rate == 1.25f && !strcmp(ps_back.name, "jug_0"),
          "PLAYER_SOUND V1: the name last @15 (%u)", n);

    CHECK(!net_sound_packet_read(NET_PACKET_SOUND, v1, 47, &back) && !net_sound_packet_read(NET_PACKET_CHAT, v1, 48, &back) &&
              !net_sound_packet_to_v1(NET_PACKET_SOUND, &s, 48, v1) && !net_sound_packet_to_v1(NET_PACKET_SPRAY, &s, sizeof(s), v1),
          "other lengths and types are refused");
}

// v3.1.1 and v7.1.4 use the same id space: their lists are prefixes of v10.0.3's.
static void check_old_lists(void) {
    printf("Running the v3.1.1 / v7.1.4 id space...\n");
    // v3.1.1 1.js:875 (51 names): v10.0.3's first 51 but id 32, "weapon_17" a second time there
    CHECK(!strcmp(network_sound_name(32), "weapon_17_0") && network_sound_index("weapon_17") == 22, "v3.1.1's id 32 (weapon_17) is weapon_17_0");
    CHECK(!strcmp(network_sound_name(50), "step_2"), "v3.1.1's last id 50 is step_2");
    const char *home = getenv("HOME");
    static const struct {
        const char *path, *anchor;
        int count, differ_at;
    } k_refs[] = {
        {"src/Krunker.Unpacked-v3.1.1/1.js", "networkSounds = [", 51, 32},
        {"jails/kru/src/Krunker_v7.1.4/src/config.js", "networkSounds = [", 265, -1},
    };
    for (size_t r = 0; r < sizeof(k_refs) / sizeof(k_refs[0]); r++) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", home ? home : "", k_refs[r].path);
        int count = 0, line = 0;
        char **names = js_string_array(path, k_refs[r].anchor, &count, &line);
        if (!names) {
            printf("  (skipping %s: not present)\n", path);
            continue;
        }
        CHECK(count == k_refs[r].count, "%s lists %d names", k_refs[r].path, count);
        int differ = 0;
        for (int i = 0; i < count && i < NETWORK_SOUND_COUNT; i++) {
            if (strcmp(names[i], g_network_sounds[i])) {
                CHECK(i == k_refs[r].differ_at, "%s id %d: %s, v10.0.3 %s", k_refs[r].path, i, names[i], g_network_sounds[i]);
                differ++;
            }
        }
        CHECK(differ == (k_refs[r].differ_at >= 0), "%s: %d ids differ", k_refs[r].path, differ);
        free_names(names, count);
    }
}

// ---------------------------------------------------------------- old maps

static int known_sound(const char *name) {
    if (!strncmp(name, "asset:", 6)) {
        if (!name[6]) return 0;
        for (const char *c = name + 6; *c; c++) {
            if ((*c < '0' || *c > '9') && !(c == name + 6 && *c == '-')) return 0;
        }
        return 1;
    }
    for (int i = 0; i < TRIGGER_EMITTER_SOUND_COUNT; i++) {
        if (!strcmp(g_trigger_emitter_sounds[i], name)) return 1;
    }
    return 0;
}

// Every trigger action's and SOUND_EMITTER's sound of one map; *references counts them. 0 when it did not load.
static int check_map_sounds(const char *relative, int *references) {
    cJSON *json = load_json(relative);
    if (!json) return 0;
    Map *map = map_init_ex(json, 0);
    CHECK(map != NULL, "%s loads", relative);
    for (size_t i = 0; map && i < map->object_count; i++) {
        const TriggerData *t = map->objects[i]->trig;
        for (int a = 0; t && a < t->action_count; a++) {
            char name[TRIGGER_SOUND_NAME_SIZE];
            trigger_sound_name(t->actions[a].sound, t->actions[a].asset_sound, name, sizeof(name));
            CHECK(known_sound(name), "%s object %zu action %d: sound %s", relative, i, a, name);
            ++*references;
        }
    }
    const cJSON *object;
    cJSON_ArrayForEach(object, cJSON_GetObjectItem(json, "objects")) {
        const cJSON *id = cJSON_GetObjectItem(object, "i");
        if (!cJSON_IsNumber(id) || id->valueint != PREFAB_SOUND_EMITTER) continue;
        TriggerEmitter e;
        CHECK(trigger_emitter_parse(object, &e) && known_sound(e.sound), "%s emitter: sound %s", relative, e.sound);
        ++*references;
    }
    if (map) {
        map_fini(map);
        free(map);
    }
    cJSON_Delete(json);
    return 1;
}

static const TriggerActionData *first_sound_action(const Map *map) {
    for (size_t i = 0; map && i < map->object_count; i++) {
        const TriggerData *t = map->objects[i]->trig;
        for (int a = 0; t && a < t->action_count; a++) {
            if (t->actions[a].action == TRIGGER_ACTION_BROADCAST_SOUND || t->actions[a].action == TRIGGER_ACTION_SEND_SOUND ||
                t->actions[a].sound || t->actions[a].asset_sound)
                return &t->actions[a];
        }
    }
    return NULL;
}

static void expect_trigger_sound(const char *relative, const char *expected, const float volume) {
    cJSON *json = load_json(relative);
    if (!json) {
        CHECK(0, "%s missing (run from the repository root)", relative);
        return;
    }
    Map *map = map_init_ex(json, 0);
    const TriggerActionData *a = first_sound_action(map);
    char name[TRIGGER_SOUND_NAME_SIZE] = "";
    if (a) trigger_sound_name(a->sound, a->asset_sound, name, sizeof(name));
    CHECK(a && !strcmp(name, expected) && fabsf(a->volume - volume) < 1e-6f, "%s: the trigger plays %s at %g (%s, %g)", relative, expected, volume,
          name, a ? a->volume : 0.0f);
    if (map) {
        map_fini(map);
        free(map);
    }
    cJSON_Delete(json);
}

static void check_old_maps(void) {
    printf("Running old maps' sounds...\n");
    // a v3.1.1-shaped TRIGGER (no "tas"): es 16 was jumpS_0 in v3.1.1, which never shipped; v7.1.4 and
    // v10.0.3 read emitterSounds[16] scare_0. er 0.2 is kept (emitterRateMin 0.1; v3.1.1 0.5)
    expect_trigger_sound("shared/tests/maps/v7_legacy_v3.json", "scare_0", 1.0f);
    // the v3.1.1 Soul Sanctum: Broadcast Sound of user asset 3222 (esa), ev 0.3
    expect_trigger_sound("assets/maps/311_soul_sanctum.json", "asset:3222", 0.3f);
    // a v7.1.4 custom map: tas[0] Broadcast Sound es 14 = emitterSounds[14] store
    expect_trigger_sound("assets/maps/custom/zombie_high.json", "store", 1.0f);

    // the official Krunk Plaza's SOUND_EMITTERs (user asset loops), as addEmitter limits them
    cJSON *json = load_json("assets/maps/official/20_krunk_plaza.json");
    CHECK(json != NULL, "20_krunk_plaza.json missing");
    int emitters = 0;
    const cJSON *object, *objects = json ? cJSON_GetObjectItem(json, "objects") : NULL;
    cJSON_ArrayForEach(object, objects) {
        const cJSON *id = cJSON_GetObjectItem(object, "i");
        if (!cJSON_IsNumber(id) || id->valueint != PREFAB_SOUND_EMITTER) continue;
        TriggerEmitter e;
        trigger_emitter_parse(object, &e);
        if (!emitters++) {
            CHECK(!strcmp(e.sound, "asset:30354") && e.asset && fabsf(e.volume - 1.6f) < 1e-6f && e.rate == 1.0f && e.distance == 2.0f &&
                      e.distance_model == 1 && fabsf(e.rolloff - 0.36f) < 1e-6f, "Krunk Plaza emitter 0: %s vol %g rate %g ed %g edm %d ero %g",
                  e.sound, e.volume, e.rate, e.distance, e.distance_model, e.rolloff);
        }
    }
    CHECK(emitters == 2, "Krunk Plaza has 2 emitters (%d)", emitters);
    cJSON_Delete(json);

    // addEmitter's defaults and limits (deob-10.0.3.js:189197-189204, :186574-186579)
    static const struct {
        const char *json, *sound;
        int asset;
        float volume, rate, distance, rolloff;
        int model;
    } k_cases[] = {
        {"{}", "nuke_0", 0, 1.0f, 1.0f, 0.0f, 0.0f, 0},
        {"{\"es\": 73}", "scare_1", 0, 1.0f, 1.0f, 0.0f, 0.0f, 0},
        {"{\"es\": 74}", "nuke_0", 0, 1.0f, 1.0f, 0.0f, 0.0f, 0},                       // past the list: emitterSounds[0]
        {"{\"es\": \"16\"}", "scare_0", 0, 1.0f, 1.0f, 0.0f, 0.0f, 0},                  // parseInt of a string
        {"{\"es\": 3, \"esa\": 0}", "asset:3", 1, 1.0f, 1.0f, 0.0f, 0.0f, 0},           // esa != null, parseInt(0 || 3)
        {"{\"esa\": 30001, \"es\": 5}", "asset:30001", 1, 1.0f, 1.0f, 0.0f, 0.0f, 0},
        {"{\"ev\": 9, \"er\": 0.01, \"ed\": 9000.7, \"edm\": 7, \"ero\": 3}", "nuke_0", 0, 2.0f, 0.1f, 5000.0f, 1.0f, 2},
        {"{\"ev\": 0.01, \"er\": 0, \"ed\": 12.9, \"edm\": 1.5, \"ero\": -1}", "nuke_0", 0, 0.05f, 1.0f, 12.0f, 0.0f, 1},
    };
    for (size_t i = 0; i < sizeof(k_cases) / sizeof(k_cases[0]); i++) {
        cJSON *o = cJSON_Parse(k_cases[i].json);
        TriggerEmitter e;
        const int ok = trigger_emitter_parse(o, &e);
        CHECK(ok && !strcmp(e.sound, k_cases[i].sound) && e.asset == k_cases[i].asset && fabsf(e.volume - k_cases[i].volume) < 1e-6f &&
                  fabsf(e.rate - k_cases[i].rate) < 1e-6f && e.distance == k_cases[i].distance && e.rolloff == k_cases[i].rolloff &&
                  e.distance_model == k_cases[i].model,
              "emitter %s: %s asset %d vol %g rate %g ed %g ero %g edm %d", k_cases[i].json, e.sound, e.asset, e.volume, e.rate, e.distance,
              e.rolloff, e.distance_model);
        cJSON_Delete(o);
    }

    // every map we ship: each trigger action's and emitter's sound resolves
    static const char *const k_dirs[] = {"assets/maps", "assets/maps/custom", "assets/maps/official", "shared/tests/maps"};
    int maps = 0, references = 0;
    for (size_t d = 0; d < sizeof(k_dirs) / sizeof(k_dirs[0]); d++) {
        char dir_path[512];
        snprintf(dir_path, sizeof(dir_path), "%s%s", g_root, k_dirs[d]);
        DIR *dir = opendir(dir_path);
        if (!dir) continue;
        const struct dirent *entry;
        while ((entry = readdir(dir))) {
            const size_t n = strlen(entry->d_name);
            if (n < 6 || strcmp(entry->d_name + n - 5, ".json")) continue;
            char relative[512];
            snprintf(relative, sizeof(relative), "%s/%s", k_dirs[d], entry->d_name);
            maps += check_map_sounds(relative, &references);
        }
        closedir(dir);
    }
    printf("  %d maps, %d sound references\n", maps, references);
    CHECK(maps >= 20 && references > 100, "the shipped maps were read (%d maps, %d references)", maps, references);
}

int main(void) {
    FILE *probe = fopen("shared/tests/data/v10/networkSounds.json", "rb");
    if (!probe) g_root = "../";
    else fclose(probe);
    check_fixture();
    check_bundle();
    check_ids();
    check_wire();
    check_old_lists();
    check_old_maps();
    printf("network_sounds_test: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
