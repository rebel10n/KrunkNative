// ruleset_test: the ruleset seam (shared/include/ruleset.h), KrunkNative's id ranges (weapons.h
// WEAPON_ID_KN_BASE, the legacy remap) and the NET_PACKET_EXT envelope (net_ext.h).
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <shared.h>
#include <ruleset.h>
#include <net_ext.h>
#include <object_manager.h>

static int g_checks, g_failures;
#define CHECK(cond, ...) do { g_checks++; if (!(cond)) { g_failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void test_lookup(void) {
    const Ruleset *v7 = ruleset_get(RULESET_V7_1_4), *v10 = ruleset_get(RULESET_V10_0_2);
    CHECK(v7 && v7->id == RULESET_V7_1_4 && !strcmp(v7->key, "v7") && !strcmp(v7->version, "7.1.4"), "v7 ruleset");
    CHECK(v10 && v10->id == RULESET_V10_0_2 && !strcmp(v10->key, "v10") && !strcmp(v10->version, "10.0.2"), "v10 ruleset");
    CHECK(!ruleset_get(RULESET_V3_1_1), "v3.1.1 is not built yet (TODO)");
    CHECK(!ruleset_get(RULESET_COUNT) && !ruleset_get((RulesetId) -1), "out of range");
    CHECK(ruleset_find("v7") == v7 && ruleset_find("7") == v7 && ruleset_find("7.1.4") == v7 && ruleset_find("v7.1.4") == v7, "find v7");
    CHECK(ruleset_find("v10") == v10 && ruleset_find("10.0.2") == v10 && ruleset_find("V10") == v10, "find v10");
    CHECK(!ruleset_find("v3") && !ruleset_find("") && !ruleset_find(NULL) && !ruleset_find("v1"), "unknown names");
    CHECK(ruleset_current() == v7 && RULESET_DEFAULT == RULESET_V7_1_4, "the default is v7");
    CHECK(&game_constants == v7->constants, "game_constants reads the current ruleset's");
    CHECK(v7->trigger_action_limit == 5 && v10->trigger_action_limit == 8, "trigger action limits");
    CHECK(v7->light_unit_factor == 1.0f && fabsf(v10->light_unit_factor - (float) M_PI) < 1e-6f, "light unit factor");
    CHECK(v7->map_dialect == MAP_DIALECT_V7 && v10->map_dialect == MAP_DIALECT_V10, "map dialects");
    CHECK(!v7->ui_page_dir[0] && !strcmp(v10->ui_page_dir, "v10"), "page dirs");
}

static void test_switch(void) {
    const Ruleset *v7 = ruleset_get(RULESET_V7_1_4), *v10 = ruleset_get(RULESET_V10_0_2);
    CHECK(!ruleset_set_current(RULESET_V3_1_1) && ruleset_current() == v7, "an unbuilt ruleset is refused");
    CHECK(ruleset_set_current(RULESET_V10_0_2) && ruleset_current() == v10, "switch to v10");
    CHECK(&game_constants == v10->constants && rules_weapons() == v10->weapons && rules_classes() == v10->classes, "the shortcuts follow");
    Game game;
    memset(&game, 0, sizeof(game));
    game_configure(&game, NULL, NULL, 0, NULL, 0, NULL, 0, NULL, 0);
    CHECK(game.rules == v10 && game.weapon_count == v10->weapon_count && game.weapons[1] == v10->weapons[1] && game.class_count == v10->class_count,
          "game_configure takes the current ruleset's tables");
    CHECK(ruleset_set_current(RULESET_V7_1_4) && game.rules == v10, "a configured game keeps its ruleset");
    game_configure(&game, NULL, NULL, 0, NULL, 0, NULL, 0, NULL, 0);
    CHECK(game.rules == v7, "reconfigured: v7");
    free(game.weapons);
    free(game.classes);
    free(game.modes);
}

static void test_weapon_ids(void) {
    const Ruleset *v7 = ruleset_get(RULESET_V7_1_4);
    CHECK(WEAPON_ID_KN_BASE == 64 && WEAPON_ID_TOOL_GUN == 64 && WEAPON_ID_PISS == 65 && WEAPON_ID_PHYS_GUN == 66 && WEAPON_TABLE_COUNT == 67, "KN weapon ids");
    for (int r = 0; r < RULESET_COUNT; r++) {
        const Ruleset *rs = ruleset_get((RulesetId) r);
        if (!rs) continue;
        CHECK(rs->krunker_weapon_count <= WEAPON_ID_KN_BASE && rs->weapon_count == WEAPON_TABLE_COUNT, "%s: Krunker's ids below the KN base", rs->key);
        for (int i = rs->krunker_weapon_count; i < WEAPON_ID_KN_BASE; i++) CHECK(!rs->weapons[i], "%s: id %d is a hole", rs->key, i);
        CHECK(rs->weapons[WEAPON_ID_TOOL_GUN] == &g_tool_gun && rs->weapons[WEAPON_ID_PISS] == &g_piss && rs->weapons[WEAPON_ID_PHYS_GUN] == &g_phys_gun,
              "%s: the KN weapons", rs->key);
    }
    CHECK(rules_weapon(-1) == NULL && rules_weapon(WEAPON_TABLE_COUNT) == NULL && rules_weapon(40) == NULL && rules_weapon(1) == v7->weapons[1], "rules_weapon bounds");
    // the remap of ids stored before the seam
    CHECK(weapon_id_from_legacy(29, 29) == WEAPON_ID_TOOL_GUN && weapon_id_from_legacy(30, 29) == WEAPON_ID_PISS && weapon_id_from_legacy(31, 29) == WEAPON_ID_PHYS_GUN,
          "29-31 under v7 are the KN weapons");
    CHECK(weapon_id_from_legacy(29, 31) == 29 && weapon_id_from_legacy(30, 31) == 30 && weapon_id_from_legacy(31, 31) == WEAPON_ID_PHYS_GUN,
          "a 31-weapon table (v10) keeps its own 29 / 30");
    CHECK(weapon_id_from_legacy(5, 29) == 5 && weapon_id_from_legacy(32, 29) == 32 && weapon_id_from_legacy(64, 29) == 64, "other ids unchanged");
    int id = -1;
    CHECK(object_manager_give_lookup("29", &id, NULL) == OM_GIVE_WEAPON && id == WEAPON_ID_TOOL_GUN, "`give 29` (an old script) -> the Tool Gun (%d)", id);
    CHECK(object_manager_give_lookup("65", &id, NULL) == OM_GIVE_WEAPON && id == WEAPON_ID_PISS, "`give 65` -> the Piss");
}

static void test_ext(void) {
    uint8_t buf[NET_MAX_PAYLOAD];
    const uint8_t body[3] = {1, 2, 3};
    const uint16_t n = net_ext_pack(buf, sizeof(buf), NET_EXT_PLAYER_LOCKS, body, sizeof(body));
    CHECK(n == 5 && buf[0] == 0x01 && buf[1] == 0x02 && !memcmp(buf + 2, body, 3), "pack: u16 LE subtype + body");
    uint16_t subtype = 0, len = 0;
    const uint8_t *b = NULL;
    CHECK(net_ext_unpack(buf, n, &subtype, &b, &len) && subtype == NET_EXT_PLAYER_LOCKS && len == 3 && b == buf + 2, "unpack");
    CHECK(!net_ext_unpack(buf, 1, &subtype, &b, &len), "a short envelope");
    CHECK(net_ext_pack(buf, sizeof(buf), 1, buf, NET_EXT_BODY_MAX + 1) == 0 && net_ext_pack(buf, 4, 1, body, 3) == 0, "too long");
    CHECK(NET_PACKET_EXT == 254 && NET_PACKET_EXT > NET_PACKET_BLOCK_SANDBOX + 3 && NET_PACKET_EXT < NET_PACKET_BLOCK_END, "254 is free of the sandbox block");
    CHECK(NET_PROTOCOL_MINOR >= NET_PROTOCOL_MINOR_EXT && NET_PROTOCOL_MINOR_EXT > NET_PROTOCOL_MINOR_V7, "1.4 announces EXT");
    CHECK(net_peer_has_wpc(NET_PROTOCOL_MINOR, 0), "a 1.4 peer takes the WP-C sizes");
}

int main(void) {
    test_lookup();
    test_switch();
    test_weapon_ids();
    test_ext();
    printf("ruleset_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
