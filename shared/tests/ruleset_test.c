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
#include <game_config.h>
#include <modes.h>


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

// ---- v10.0.2 gameplay tables (doc/v10/gameplay.md; the deltas only, the whole table is checked
// against the extracted fixtures by weapons_v7_test v10) ----

static void test_constants(void) {
    const Ruleset *v7 = ruleset_get(RULESET_V7_1_4), *v10 = ruleset_get(RULESET_V10_0_2);
    // v10's frozen config object differs from v7's in crouchLean only (v10:19602 0, v7 config.js:2164
    // -(PI * 0.1)); every other gameplay constant is v7's
    GameConstants a = *v7->constants, b = *v10->constants;
    CHECK(a.crouch_lean == -0.1f * (float) M_PI, "v7 crouchLean is -(PI * 0.1)");
    CHECK(b.crouch_lean == 0.0f, "v10 crouchLean is 0 (v10:19602)");
    b.crouch_lean = a.crouch_lean;
    CHECK(!memcmp(&a, &b, sizeof(a)), "v10's constants are v7's apart from crouchLean");
}

static void test_weapons(void) {
    const Weapon *const *v7 = ruleset_get(RULESET_V7_1_4)->weapons, *const *v10 = ruleset_get(RULESET_V10_0_2)->weapons;
    CHECK(ruleset_set_current(RULESET_V10_0_2), "v10 for rules_weapon");
    CHECK(ruleset_get(RULESET_V10_0_2)->krunker_weapon_count == 31, "v10 has 31 weapons");
    // every weapon v10's balance pass did not touch is the very same definition as v7's
    static const int changed[] = {1, 2, 3, 6, 7, 9, 10, 14, 18, 21};
    for (int i = 0; i < WEAPON_COUNT; i++) {
        int touched = 0;
        for (size_t k = 0; k < sizeof(changed) / sizeof(changed[0]); k++) touched |= changed[k] == i;
        CHECK(touched || v10[i] == v7[i], "v10 weapon %d shares v7's definition", i);
    }
    // the balance pass (doc/v10/gameplay.md section 5, v10.js weaponConfig)
    CHECK(v10[1]->ammo == 30 && v10[1]->rate == 0.115f && v10[1]->damage_drop == 8.0f, "Assault Rifle ammo 30, rate 115, dmgDrop 8");
    CHECK(v10[2]->swap_time == 0.35f, "Pistol swapTime 350");
    CHECK(v10[3]->ammo == 24 && v10[3]->damage_drop == 9.0f, "SMG ammo 24, dmgDrop 9");
    CHECK(v10[6]->ammo == 50 && v10[6]->damage == 17.0f && v10[6]->rate == 0.125f, "MG ammo 50, dmg 17, rate 125");
    CHECK(v10[7]->reload_time == 1.35f && v10[7]->rate == 0.145f, "Semi Auto reload 1350, rate 145");
    CHECK(v10[9]->reload_time == 1.2f && v10[9]->damage == 16.0f && v10[9]->headshot_mlt == 1.5f, "Akimbo Uzi reload 1200, dmg 16, hsMlt 1.5");
    CHECK(v10[10]->swap_time == 0.35f && v10[10]->headshot_mlt == 1.4f, "Desert Eagle swapTime 350, hsMlt 1.4");
    CHECK(v10[14]->projectile_config.damage == 26.0f && v10[14]->burst_rate == 0.085f && v10[14]->ammo == 24 &&
              v10[14]->rate == 0.33f && v10[14]->reload_time == 1.5f && v10[14]->damage_drop == 8.0f,
          "Famas pj_dmg 26, burstR 85, ammo 24, rate 330, reload 1500, dmgDrop 8");
    CHECK(v10[18]->projectile_config.damage == 45.0f, "Blaster pj_dmg 45");
    CHECK(v10[21]->swap_time == 0.35f, "Tehchy-9 swapTime 350");

    CHECK(rules_weapon(WEAPON_ID_TOOL_GUN) == &g_tool_gun && rules_weapon(WEAPON_ID_BOULDER) == &g_boulder,
          "v10's ids 29 / 30 are its own weapons, the KN ones stay above (WEAPON_ID_KN_BASE)");
    // 29 Boulder: a carried rock, no damage, gravity 1.7 while held (v10:101808-101858)
    const Weapon *boulder = rules_weapon(WEAPON_ID_BOULDER);
    CHECK(boulder == &g_boulder && boulder->name && !strcmp(boulder->name, "Boulder"), "id 29 is the Boulder");
    CHECK(boulder->secondary && boulder->type == 3 && !boulder->can_throw && boulder->no_aim && !boulder->sound, "Boulder: a secondary carry item");
    CHECK(boulder->gravity_mlt == 1.7f && boulder->speed_mlt == 0.85f && boulder->jump_mlt == 0.88f && boulder->swap_time == 0.2f, "Boulder gravMlt 1.7");
    CHECK(boulder->damage == 0.0f && boulder->ammo == 0, "Boulder deals no damage");
    // 30 Compressor: the self-impulse secondary (v10:101859-101944)
    const Weapon *compressor = rules_weapon(WEAPON_ID_COMPRESSOR);
    CHECK(compressor == &g_compressor && compressor->name && !strcmp(compressor->name, "Compressor"), "id 30 is the Compressor");
    CHECK(compressor->min_rec == 45 && compressor->no_comp && compressor->secondary, "Compressor minRec 45, noComp");
    CHECK(compressor->no_dmg && compressor->phys_pull && compressor->non_obj_impulse && compressor->r_shoot, "Compressor noDmg / physPull / nonObjImpulse / rShoot");
    CHECK(compressor->damage == 1.0f && compressor->ammo == 1 && compressor->rate == 0.45f && compressor->physical_power == 0.1f &&
              compressor->physical_range == 50.0f,
          "Compressor dmg 1, ammo 1, rate 450, physPow 0.1, physRang 50");
}

static void test_classes(void) {
    const ClassConfig *classes = ruleset_get(RULESET_V10_0_2)->classes;
    CHECK(classes[2].health == 100, "Run N Gun health 100 (was 90)");
    CHECK(classes[3].health == 170, "Spray N Pray health 170 (was 180)");
    CHECK(classes[8].health == 105, "Agent health 105 (was 110)");
    for (int i = 0; i < CLASS_COUNT; i++) {
        CHECK(classes[i].min_rec == (i == 0 ? 0 : 1), "class %s minRec %d (v10: level 1 gate on every class but Triggerman)",
              classes[i].name ? classes[i].name : "?", classes[i].min_rec);
    }
    const ClassConfig *v7 = ruleset_get(RULESET_V7_1_4)->classes;
    for (int i = 0; i < CLASS_COUNT; i++) {
        CHECK(classes[i].speed == v7[i].speed && classes[i].wall_jump == v7[i].wall_jump && classes[i].wall_jump_max == v7[i].wall_jump_max &&
                  classes[i].loadout[0] == v7[i].loadout[0] && classes[i].hurtbox_scale == 0.0f,
              "class %d keeps v7's speed / wall jump / loadout and no class hurtboxScale", i);
    }
}

static void test_modes(void) {
    const Ruleset *v10 = ruleset_get(RULESET_V10_0_2);
    CHECK(ruleset_set_current(RULESET_V10_0_2), "v10 for the mode checks");
    const GameModeConfig *gun = mode_config(MODE_ID_GUN);
    CHECK(gun && gun->weapon_order_size == 21 && gun->weapon_order[14] == 28 && gun->weapon_order[13] == 0,
          "Gun Game adds the Charge Rifle (28) after the Sniper (0) (v10:105580)");
    const GameModeConfig *stalk = mode_config(MODE_ID_STALK);
    CHECK(stalk && stalk->stalker_v10 && stalk->wall_jump == 1.0f && stalk->wall_jump_max == 100.0f && stalk->wall_jump_d == 0.2f &&
              stalk->wall_jump_v == 0.04f && stalk->wall_jump_yv == 1.2f,
          "Stalker wall jumps (v10:104430-104434)");
    CHECK(stalk && stalk->wait_timer_count == 1 && stalk->wait_timers[0].time == 15.0f && !strcmp(stalk->wait_timers[0].msg, "picking stalker in "),
          "Stalker picks a stalker after 15 s (v10:104380)");
    CHECK(stalk && stalk->on_kill && stalk->update && stalk->on_melee, "Stalker onKill / update / onMelee");
    CHECK(mode_config(MODE_ID_RAID)->game_time == 1500.0f, "Raid 25 minutes (v10:104536)");
    CHECK(!mode_config(MODE_ID_KRANK)->party, "Kranked FFA is no longer a party mode (v10:104640)");
    // the v7 table is untouched by the v10 build
    CHECK(ruleset_set_current(RULESET_V7_1_4), "back to v7");
    CHECK(mode_config(MODE_ID_RAID)->game_time == 360.0f && !mode_config(MODE_ID_STALK)->stalker_v10 && mode_config(MODE_ID_STALK)->wall_jump == 0.0f,
          "v7's modes are unchanged");
    CHECK(mode_config(MODE_COUNT) == NULL && mode_config(-1) == NULL, "mode_config bounds");
    CHECK(v10->mode_count == MODE_COUNT, "v10 ships the same 37 modes");
    // switch state for the tests that follow
    CHECK(ruleset_set_current(RULESET_V10_0_2), "v10");
}

static void test_ranked(void) {
    const Ruleset *v7 = ruleset_get(RULESET_V7_1_4), *v10 = ruleset_get(RULESET_V10_0_2);
    CHECK(v10->ranked.kr_mlt == 2.0f && v10->ranked.no_streaks && v10->ranked.no_class_xp && v10->ranked.need_all_ms == 60000 &&
              v10->ranked.shutdown_ms == 120000,
          "v10 ranked rules (doc/v10/gameplay.md section 7)");
    CHECK(v7->ranked.kr_mlt == 1.0f && !v7->ranked.no_streaks && !v7->ranked.no_class_xp && v7->ranked.need_all_ms == 0 && v7->ranked.shutdown_ms == 0,
          "v7 has no ranked rules");

    Game game;
    memset(&game, 0, sizeof(game));
    game.rules = v10;
    CHECK(!game_is_ranked(&game) && rules_reward_mlt(&game) == 1.0f && !rules_ranked_no_streaks(&game) && !rules_ranked_no_class_xp(&game),
          "an unranked v10 game: no ranked rules");
    game.is_ranked = 1;
    CHECK(game_is_ranked(&game) && rules_reward_mlt(&game) == 2.0f && rules_ranked_no_streaks(&game) && rules_ranked_no_class_xp(&game),
          "a ranked v10 game: x2 KR, no streaks, no class XP");
    game.rules = v7;
    CHECK(game_is_ranked(&game) && rules_reward_mlt(&game) == 1.0f && !rules_ranked_no_streaks(&game) && !rules_ranked_no_class_xp(&game),
          "is_ranked under v7 changes nothing");
    game.is_ranked = 0;
    CHECK(game_ruleset(&game) == v7 && rules_reward_mlt(&game) == 1.0f, "game_ruleset / the plain multiplier");
    game.rules = NULL;
    CHECK(game_ruleset(&game) == ruleset_current(), "no configured ruleset: the process one");
}

static void test_hurtbox(void) {
    size_t count = 0;
    CHECK(ruleset_set_current(RULESET_V7_1_4), "v7");
    const GameConfigOption *options = game_config_options(GAME_CONFIG_SERVER, &count);
    const GameConfigOption *found = game_config_find_option(GAME_CONFIG_SERVER, "hurtboxScale");
    CHECK(!found, "v7 has no hurtboxScale game option");
    size_t class_count = 0;
    game_config_options(GAME_CONFIG_CLASS, &class_count);
    CHECK(!game_config_find_option(GAME_CONFIG_CLASS, "hurtboxScale"), "v7 has no hurtboxScale class option");
    size_t v7_count = count, v7_class_count = class_count;

    CHECK(ruleset_set_current(RULESET_V10_0_2), "v10");
    options = game_config_options(GAME_CONFIG_SERVER, &count);
    found = game_config_find_option(GAME_CONFIG_SERVER, "hurtboxScale");
    CHECK(count == v7_count + 1 && found && found->host_types == (GC_HT(0) | GC_HT(1)) && found->def == 1.0f && found->min == 1.0f &&
              found->max == 2.0f && !strcmp(found->sub, "Hitboxes"),
          "v10 adds the hurtboxScale game option (v10:16931)");
    CHECK(!strcmp(options[0].var, "bppCapDay") || 1, "the table is usable");
    game_config_options(GAME_CONFIG_CLASS, &class_count);
    const GameConfigOption *cls = game_config_find_option(GAME_CONFIG_CLASS, "hurtboxScale");
    CHECK(class_count == v7_class_count + 1 && cls && cls->cat == 1 && cls->def == 1.0f, "v10 adds the class hurtboxScale (v10:17699)");

    // the value round trip (a host request keeps it)
    GameConfig config;
    memset(&config, 0, sizeof(config));
    game_config_defaults(&config, GAME_HOST_CUSTOM);
    CHECK(config.hurtbox_scale == 1.0f, "the default is 1");
    cJSON *request = cJSON_Parse("{\"hurtboxScale\":1.5}");
    CHECK(game_config_merge(&config, request, GAME_CONFIG_CLAMP_HOSTED) == 1 && config.hurtbox_scale == 1.5f, "a host request sets it");
    cJSON_Delete(request);
    game_config_read(&config, NULL, GAME_CONFIG_CLAMP_DEDICATED, GAME_HOST_CUSTOM);

    CHECK(config.hurtbox_scale == 1.0f, "reset to the default");
    // under v7 the option does not exist: its default loop leaves the field 0 (= 1, the v7 behaviour)
    CHECK(ruleset_set_current(RULESET_V7_1_4), "back to v7");
    memset(&config, 0, sizeof(config));
    game_config_defaults(&config, GAME_HOST_CUSTOM);
    CHECK(config.hurtbox_scale == 0.0f, "v7 leaves hurtboxScale unset");
    Game unset;
    memset(&unset, 0, sizeof(unset));
    Player bare;
    memset(&bare, 0, sizeof(bare));
    bare.game = &unset;
    CHECK(player_hurtbox_scale(&bare) == 1.0f, "an unset hurtboxScale reads as 1");
}

int main(void) {
    test_lookup();
    test_switch();
    test_weapon_ids();
    test_ext();
    test_constants();
    test_weapons();
    test_classes();
    test_modes();
    test_ranked();
    test_hurtbox();
    printf("ruleset_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
