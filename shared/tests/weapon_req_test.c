// The weapon req of the game mode (game_weapon_req, shared/src/game_config.c; v10.0.2 v10.js:98979, _34256) and
// the random pickups' pool (interact.c random_weapon, v10 onlyWeapons, v10.js:101956). A customOnly weapon (the
// Grappler) is taken only in a custom game that is not comp, or by a dev (the picker and the spawn, Player.dev);
// a noComp weapon is not taken in comp; a random pickup is never a melee, equipment, customOnly or noDmg weapon
// (the Compressor).
//   cmake-build-debug/weapon_req_test   (run from the repository root)
#include <shared.h>
#include <game_config.h>
#include <weapons.h>
#include <ruleset.h>
#include <pcg_basic.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static int g_failures, g_checks;
#define CHECK(cond, ...) do { g_checks++; \
    if (!(cond)) { g_failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void test_send(Game *game, int uid, int type, const void *payload, uint16_t length) {
    (void) game; (void) uid; (void) type; (void) payload; (void) length;
}

// is_local 1: the local game, its own authority (random pickups roll here); 0: a client's copy of a server's game
static void configure(Game *game, const cJSON *map, const unsigned char is_local) {
    memset(game, 0, sizeof(*game));
    int *modes = calloc(1, sizeof(int));
    const cJSON **maps = calloc(1, sizeof(cJSON *));
    modes[0] = MODE_ID_FFA;
    maps[0] = map;
    game_configure(game, NULL, maps, 1, modes, 1, NULL, 0, NULL, 0);
    game->hooks.send = test_send;
    game_init(game, 0, 0, is_local);
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
    configure(&game, map, 1);
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

    configure(&game, base, 1); // official krunker.io: not hosted, not comp
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

// ---- the spawn's secondary (player_build_loadout): the Grappler only in a custom game, or a dev's (v10 req's first
// clause, account.isDev: the picker lets a dev pick it in any lobby and the spawn keeps it; Player.dev, the server's
// net_client_is_dev). The spawn settles the player's secIndex (updateSecIndex: the pick, or 2 when refused) for the
// SPAWN packet, and a client's copy of the player (no authority) builds what the SPAWN says, gate or not.
static void test_spawn(const cJSON *base) {
    printf("spawn secondary (official, dev, custom, a client's copy)...\n");
    Game official, custom, copy;
    configure(&official, base, 1);
    configure(&custom, base, 1);
    game_config_host(&custom, NULL, GAME_HOST_CUSTOM);
    configure(&copy, base, 0); // a client's copy of a server's game: not the authority
    CHECK(game_is_authority(&official) && game_is_authority(&custom) && !game_is_authority(&copy), "the test games: two authorities and a copy");
    const struct { Game *game; int dev; int grappler; const char *what; } cases[] = {
        {&official, 0, 0, "official, not dev: the Pistol"},
        {&official, 1, 1, "official, dev: the Grappler"},
        {&custom, 0, 1, "custom, not dev: the Grappler"},
        {&copy, 0, 1, "a client's copy, not dev: the SPAWN's Grappler as is"},
    };
    for (size_t k = 0; k < sizeof(cases) / sizeof(cases[0]); k++) {
        Player *p = player_init(cases[k].game);
        p->uid = (int) k + 1;
        p->class_index = 0;
        p->has_account = 1;
        p->dev = (unsigned char) cases[k].dev;
        p->secondary_index = WEAPON_ID_GRAPPLER;
        game_players_add(cases[k].game, p);
        player_spawn(p);
        int has = 0;
        for (int i = 0; i < p->loadout_size; i++) has |= p->loadout[i] == WEAPON_ID_GRAPPLER;
        CHECK(has == cases[k].grappler, "%s (the loadout has the Grappler: %d)", cases[k].what, has);
        const int settled = cases[k].grappler ? WEAPON_ID_GRAPPLER : WEAPON_ID_PISTOL;
        CHECK(p->secondary_index == settled, "%s: the settled secIndex is %d, not %d", cases[k].what, settled, p->secondary_index);
    }
}

// ---- the Sharp Shooter's weapon (mode_random_weapon, v10 onlyGoodWeapons: not melee, equipment, customOnly, noDmg)
static void test_sharp_shooter(const cJSON *base) {
    printf("Sharp Shooter weapon (onlyGoodWeapons)...\n");
    const int seeds = 400;
    int compressor = 0, grappler = 0, kinds[256] = {0}, rolled = 0;
    static Game game;
    for (int k = 0; k < seeds; k++) {
        pcg32_srandom((uint64_t) (k + 1), 54u);
        int *modes = calloc(1, sizeof(int));
        const cJSON **maps = calloc(1, sizeof(cJSON *));
        modes[0] = MODE_ID_SHRP;
        maps[0] = base;
        memset(&game, 0, sizeof(game));
        game_configure(&game, NULL, maps, 1, modes, 1, NULL, 0, NULL, 0);
        game.hooks.send = test_send;
        game_init(&game, 0, 0, 1);
        if (!game.mode) continue;
        const int weapon = game.mode->active_objective;
        rolled++;
        if (weapon == WEAPON_ID_COMPRESSOR) compressor++;
        if (weapon == WEAPON_ID_GRAPPLER) grappler++;
        if (weapon >= 0 && weapon < 256) kinds[weapon] = 1;
    }
    int distinct = 0;
    for (int w = 0; w < 256; w++) distinct += kinds[w];
    CHECK(rolled >= seeds / 2, "Sharp Shooter games rolled (%d of %d)", rolled, seeds);
    CHECK(compressor == 0, "the Sharp Shooter's weapon is never the Compressor (noDmg is out of onlyGoodWeapons): %d of %d", compressor, rolled);
    CHECK(grappler == 0, "the Sharp Shooter's weapon is never the Grappler (customOnly): %d of %d", grappler, rolled);
    CHECK(distinct >= 8, "the Sharp Shooter's weapon still varies (%d weapons seen)", distinct);
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
    test_sharp_shooter(map);
    if (g_failures) {
        printf("weapon_req_test: %d of %d checks failed\n", g_failures, g_checks);
        return 1;
    }
    printf("weapon_req_test: %d checks OK\n", g_checks);
    return 0;
}
