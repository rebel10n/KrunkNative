// The weapon req of the game mode (game_weapon_req, shared/src/game_config.c; v10.0.2 v10.js:98979, _34256) and
// the random pickups' pool (interact.c random_weapon, v10 onlyWeapons, v10.js:101956). A customOnly weapon (the
// Grappler) is taken only in a custom game that is not comp, or by a dev; a noComp weapon is not taken in comp; a
// random pickup is never a melee, equipment, customOnly or noDmg weapon (the Compressor).
//   cmake-build-debug/weapon_req_test   (run from the repository root)
#include <shared.h>
#include <game_config.h>
#include <weapons.h>
#include <ruleset.h>
#include <pcg_basic.h>
#include <stdio.h>
#include <string.h>

static int g_failures, g_checks;
#define CHECK(cond, ...) do { g_checks++; \
    if (!(cond)) { g_failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void test_send(Game *game, int uid, int type, const void *payload, uint16_t length) {
    (void) game; (void) uid; (void) type; (void) payload; (void) length;
}

static void configure(Game *game, const cJSON *map) {
    memset(game, 0, sizeof(*game));
    int *modes = calloc(1, sizeof(int));
    const cJSON **maps = calloc(1, sizeof(cJSON *));
    modes[0] = MODE_ID_FFA;
    maps[0] = map;
    game_configure(game, NULL, maps, 1, modes, 1, NULL, 0, NULL, 0);
    game->hooks.send = test_send;
    game_init(game, 0, 0, 1); // the local game: its own authority (random pickups roll here)
    game->now = 0.0f;
}

// ---- the random pool: a map with random weapon pickups, re-rolled round after round
static void test_random_pool(const cJSON *base) {
    printf("random pickups (onlyWeapons)...\n");
    cJSON *map = cJSON_Duplicate(base, 1);
    cJSON *objects = cJSON_GetObjectItem(map, "objects");
    const cJSON *src = NULL;
    cJSON *o;
    cJSON_ArrayForEach(o, cJSON_GetObjectItem(base, "objects")) {
        if (!src && cJSON_GetObjectItem(o, "w")) src = o;
    }
    if (!src) { CHECK(0, "the test map has a weapon pickup to copy"); cJSON_Delete(map); return; }
    const int copies = 40;
    for (int i = 0; i < copies; i++) {
        cJSON *c = cJSON_Duplicate(src, 1);
        cJSON_DeleteItemFromObject(c, "rwg");
        cJSON_AddTrueToObject(c, "rw");
        cJSON_AddItemToArray(objects, c);
    }
    Game game;
    configure(&game, map);
    int pickups = 0, compressor = 0, grappler = 0, distinct[256] = {0}, rounds = 30;
    for (int r = 0; r < rounds; r++) {
        game_start_round(&game);
        for (size_t uid = 0; uid < game.map->object_count; uid++) {
            if (!game.map->objects[uid]->pickup) continue;
            GamePickup st;
            if (!game_pickup_state(&game, (int) uid, &st) || !st.random) continue;
            pickups++;
            if (st.weapon == WEAPON_ID_COMPRESSOR) compressor++;
            if (st.weapon == WEAPON_ID_GRAPPLER) grappler++;
            if (st.weapon >= 0 && st.weapon < 256) distinct[st.weapon] = 1;
        }
    }
    int kinds = 0;
    for (int w = 0; w < 256; w++) kinds += distinct[w];
    CHECK(pickups >= copies * rounds / 2, "random pickups rolled (%d)", pickups);
    CHECK(compressor == 0, "no random pickup is the Compressor (noDmg is out of onlyWeapons): %d of %d", compressor, pickups);
    CHECK(grappler == 0, "no random pickup is the Grappler (customOnly): %d of %d", grappler, pickups);
    CHECK(kinds >= 8, "the random pool still varies (%d weapons seen)", kinds);
    cJSON_Delete(map);
}

// ---- the game mode's req: official (not custom), custom, comp and dev
static void test_req(const cJSON *base) {
    printf("game_weapon_req (official, custom, comp, dev)...\n");
    Game game;
    const Weapon *grappler = rules_weapon(WEAPON_ID_GRAPPLER);
    const Weapon *pistol = rules_weapon(WEAPON_ID_PISTOL);
    const Weapon *alien = rules_weapon(11); // the Alien Blaster: noComp
    CHECK(grappler && grappler->custom_only && pistol && alien && alien->no_comp, "the table: the Grappler customOnly, the Alien Blaster noComp");

    configure(&game, base); // official krunker.io: not hosted, not comp
    CHECK(!game_weapon_req(&game, grappler, 0), "official, not dev: the Grappler is refused");
    CHECK(game_weapon_req(&game, grappler, 1), "official, dev: the Grappler is allowed");
    CHECK(game_weapon_req(&game, pistol, 0), "official: the Pistol is allowed");
    CHECK(game_weapon_req(&game, alien, 0), "official: a noComp weapon is allowed outside comp");

    game_config_host(&game, NULL, GAME_HOST_CUSTOM); // a custom game (hosted), not comp
    CHECK(game_config_is_custom(&game), "the hosted game is custom");
    CHECK(game_weapon_req(&game, grappler, 0), "custom, not comp: the Grappler is allowed");

    game.config.comp = 1; // a comp custom game
    CHECK(!game_weapon_req(&game, grappler, 0), "custom comp: the Grappler is refused");
    CHECK(!game_weapon_req(&game, alien, 0), "custom comp: a noComp weapon is refused");
    CHECK(game_weapon_req(&game, pistol, 0), "custom comp: the Pistol is allowed");
    CHECK(game_weapon_req(&game, grappler, 1), "custom comp, dev: the Grappler is allowed");
}

// ---- the spawn's secondary (player_build_loadout): the Grappler only in a custom game
static void test_spawn(const cJSON *base) {
    printf("spawn secondary...\n");
    Game official, custom;
    configure(&official, base);
    configure(&custom, base);
    game_config_host(&custom, NULL, GAME_HOST_CUSTOM);
    for (int k = 0; k < 2; k++) {
        Game *game = k ? &custom : &official;
        Player *p = player_init(game);
        p->uid = 1;
        p->class_index = 0;
        p->has_account = 1;
        p->secondary_index = WEAPON_ID_GRAPPLER;
        game_players_add(game, p);
        player_spawn(p);
        int has = 0;
        for (int i = 0; i < p->loadout_size; i++) has |= p->loadout[i] == WEAPON_ID_GRAPPLER;
        if (k == 0) CHECK(!has, "official spawn: the Grappler is not the secondary");
        else CHECK(has, "custom spawn: the Grappler is the secondary");
    }
}

int main(void) {
    cJSON *map = NULL;
    if (load_json_file("shared/tests/maps/v7_all_prefabs.json", &map) && load_json_file("../shared/tests/maps/v7_all_prefabs.json", &map)) {
        printf("Failed to load shared/tests/maps/v7_all_prefabs.json (run from the repository root)\n");
        return 1;
    }
    test_req(map);
    test_spawn(map);
    test_random_pool(map);
    if (g_failures) {
        printf("weapon_req_test: %d of %d checks failed\n", g_failures, g_checks);
        return 1;
    }
    printf("weapon_req_test: %d checks OK\n", g_checks);
    return 0;
}
