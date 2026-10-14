// ruleset_test: the Krunker v10.0.2 ruleset (shared/include/ruleset.h; the only version), its
// wire ids, KrunkNative's id ranges (weapons.h WEAPON_ID_KN_BASE, the legacy remap) and the
// NET_PACKET_EXT envelope (net_ext.h).
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
    const Ruleset *v10 = ruleset_current();
    CHECK(v10 && !strcmp(v10->key, "v10") && !strcmp(v10->version, "10.0.2"), "the v10 ruleset");
    CHECK(&game_constants == v10->constants, "game_constants is the ruleset's");
    CHECK(v10->trigger_action_limit == 8 && v10->entity_limit == 64 && v10->archetype_limit == 32, "trigger action / entity / archetype limits");
    CHECK(fabsf(v10->light_unit_factor - (float) M_PI) < 1e-6f, "light unit factor");
    CHECK(rules_weapons() == v10->weapons && rules_classes() == v10->classes && v10->modes == g_mode_configs, "the shortcuts");
    CHECK(game_ruleset(NULL) == v10, "no game: the ruleset");
    Game game;
    memset(&game, 0, sizeof(game));
    game_configure(&game, NULL, NULL, 0, NULL, 0, NULL, 0, NULL, 0);
    CHECK(game.rules == v10 && game.weapon_count == v10->weapon_count && game.weapons[1] == v10->weapons[1] && game.class_count == v10->class_count,
          "game_configure takes the ruleset's tables");
    free(game.weapons);
    free(game.classes);
    free(game.modes);
}

// The version toggle's wire fields carry v10 only (shared.h NET_HELLO_RULESETS / NET_INIT_RULESET)
static void test_wire(void) {
    const NetHelloPacket hello = net_hello_make();
    CHECK(hello.major == NET_PROTOCOL_MAJOR && hello.minor == NET_PROTOCOL_MINOR && hello.patch == NET_PROTOCOL_PATCH, "HELLO protocol");
    CHECK(hello.rulesets == (1u << 2) && NET_HELLO_RULESETS == (1u << 2), "HELLO: the v10.0.2 bit (2) only");
    CHECK(NET_INIT_RULESET == 3 && sizeof(NetInitPacket) == NET_INIT_PACKET_V1_SIZE + 1, "INIT: 18 bytes, ruleset 3 (v10.0.2 + 1)");
}

static void test_weapon_ids(void) {
    const Ruleset *rs = ruleset_current();
    CHECK(WEAPON_ID_KN_BASE == 64 && WEAPON_ID_TOOL_GUN == 64 && WEAPON_ID_PISS == 65 && WEAPON_ID_PHYS_GUN == 66 && WEAPON_TABLE_COUNT == 67, "KN weapon ids");
    CHECK(rs->krunker_weapon_count == 31 && rs->weapon_count == WEAPON_TABLE_COUNT, "Krunker's 31 ids below the KN base");
    for (int i = rs->krunker_weapon_count; i < WEAPON_ID_KN_BASE; i++) CHECK(!rs->weapons[i], "id %d is a hole", i);
    CHECK(rs->weapons[WEAPON_ID_TOOL_GUN] == &g_tool_gun && rs->weapons[WEAPON_ID_PISS] == &g_piss && rs->weapons[WEAPON_ID_PHYS_GUN] == &g_phys_gun,
          "the KN weapons");
    CHECK(rules_weapon(-1) == NULL && rules_weapon(WEAPON_TABLE_COUNT) == NULL && rules_weapon(40) == NULL && rules_weapon(1) == rs->weapons[1], "rules_weapon bounds");
    // the remap of ids stored before the seam
    CHECK(weapon_id_from_legacy(29, 29) == WEAPON_ID_TOOL_GUN && weapon_id_from_legacy(30, 29) == WEAPON_ID_PISS && weapon_id_from_legacy(31, 29) == WEAPON_ID_PHYS_GUN,
          "29-31 of a 29-weapon table are the KN weapons");
    CHECK(weapon_id_from_legacy(29, 31) == 29 && weapon_id_from_legacy(30, 31) == 30 && weapon_id_from_legacy(31, 31) == WEAPON_ID_PHYS_GUN,
          "a 31-weapon table (v10) keeps its own 29 / 30");
    CHECK(weapon_id_from_legacy(5, 29) == 5 && weapon_id_from_legacy(32, 29) == 32 && weapon_id_from_legacy(64, 29) == 64, "other ids unchanged");
    int id = -1;
    const int give29 = object_manager_give_lookup("29", &id, NULL);
    CHECK(give29 == OM_GIVE_NONE || (give29 == OM_GIVE_WEAPON && id == WEAPON_ID_BOULDER), "`give 29` is v10's Boulder, never the Tool Gun (%d)", id);
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

// ---- v10.0.2 gameplay tables (doc/v10/gameplay.md; the deltas from v7.1.4 only, the whole table
// is checked against the extracted fixtures by weapons_v7_test) ----

static void test_constants(void) {
    // v10's frozen config object differs from v7.1.4's in crouchLean only (v10:19602 0, v7
    // config.js:2164 -(PI * 0.1)); every other gameplay constant is v7's (border 500, gravity 0.15 ...)
    CHECK(game_constants.crouch_lean == 0.0f, "v10 crouchLean is 0 (v10:19602)");
    CHECK(game_constants.border_height == 500.0f && game_constants.gravity == 0.15f && game_constants.player_speed == 0.42f &&
              game_constants.jump_velocity == 0.072f && game_constants.player_height == 11.0f,
          "v7.1.4's border, gravity, speed, jump and height");
}

static void test_weapons(void) {
    const Weapon *const *v10 = ruleset_current()->weapons;
    CHECK(ruleset_current()->krunker_weapon_count == 31, "v10 has 31 weapons");
    // every weapon v10's balance pass did not touch is the v7.1.4 definition (shared/src/weapons/)
    CHECK(v10[0] == &g_awp && v10[4] == &g_revolver && v10[12] == &g_knife && v10[28] == &g_charge_rifle, "untouched entries are v7.1.4's");
    CHECK(v10[1] != &g_ak47 && v10[1] == &g_ak47_v10 && v10[21] == &g_tehchy9_v10, "rebalanced entries are v10's copies");
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
    const ClassConfig *classes = ruleset_current()->classes;
    CHECK(classes[2].health == 100, "Run N Gun health 100 (was 90)");
    CHECK(classes[3].health == 170, "Spray N Pray health 170 (was 180)");
    CHECK(classes[8].health == 105, "Agent health 105 (was 110)");
    for (int i = 0; i < CLASS_COUNT; i++) {
        CHECK(classes[i].min_rec == (i == 0 ? 0 : 1), "class %s minRec %d (v10: level 1 gate on every class but Triggerman)",
              classes[i].name ? classes[i].name : "?", classes[i].min_rec);
    }
    for (int i = 0; i < CLASS_COUNT; i++) CHECK(classes[i].hurtbox_scale == 0.0f, "class %d: no class hurtboxScale (1)", i);
    CHECK(classes[0].speed == 1.05f && classes[2].wall_jump && classes[15].loadout[0] == WEAPON_ID_CHARGE_RIFLE, "v7.1.4's speeds / wall jumps / loadouts");
}

static void test_modes(void) {
    const Ruleset *v10 = ruleset_current();
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
    CHECK(mode_config(MODE_COUNT) == NULL && mode_config(-1) == NULL, "mode_config bounds");
    CHECK(v10->mode_count == MODE_COUNT, "v10 ships the same 37 modes");
}

static void test_ranked(void) {
    const Ruleset *v10 = ruleset_current();
    CHECK(v10->ranked.kr_mlt == 2.0f && v10->ranked.no_streaks && v10->ranked.no_class_xp && v10->ranked.need_all_ms == 60000 &&
              v10->ranked.shutdown_ms == 120000,
          "v10 ranked rules (doc/v10/gameplay.md section 7)");

    Game game;
    memset(&game, 0, sizeof(game));
    game.rules = v10;
    CHECK(!game_is_ranked(&game) && rules_reward_mlt(&game) == 1.0f && !rules_ranked_no_streaks(&game) && !rules_ranked_no_class_xp(&game),
          "an unranked v10 game: no ranked rules");
    game.is_ranked = 1;
    CHECK(game_is_ranked(&game) && rules_reward_mlt(&game) == 2.0f && rules_ranked_no_streaks(&game) && rules_ranked_no_class_xp(&game),
          "a ranked v10 game: x2 KR, no streaks, no class XP");
    game.is_ranked = 0;
    CHECK(game_ruleset(&game) == v10 && rules_reward_mlt(&game) == 1.0f, "game_ruleset / the plain multiplier");
    game.rules = NULL;
    CHECK(game_ruleset(&game) == ruleset_current(), "no configured ruleset: the ruleset");
}

static void test_hurtbox(void) {
    size_t count = 0, class_count = 0;
    const GameConfigOption *options = game_config_options(GAME_CONFIG_SERVER, &count);
    const GameConfigOption *found = game_config_find_option(GAME_CONFIG_SERVER, "hurtboxScale");
    CHECK(found && found->host_types == (GC_HT(0) | GC_HT(1)) && found->def == 1.0f && found->min == 1.0f && found->max == 2.0f &&
              !strcmp(found->sub, "Hitboxes"),
          "the hurtboxScale game option (v10:16931)");
    for (size_t i = 0; i + 1 < count; i++)
        if (!strcmp(options[i].var, "hitBoxPad")) CHECK(!strcmp(options[i + 1].var, "hurtboxScale"), "hurtboxScale follows hitBoxPad");
    const GameConfigOption *class_options = game_config_options(GAME_CONFIG_CLASS, &class_count);
    const GameConfigOption *cls = game_config_find_option(GAME_CONFIG_CLASS, "hurtboxScale");
    CHECK(cls && cls->cat == 1 && cls->def == 1.0f, "the class hurtboxScale (v10:17699)");
    for (size_t i = 0; i + 1 < class_count; i++)
        if (!strcmp(class_options[i].var, "hbH")) CHECK(!strcmp(class_options[i + 1].var, "hurtboxScale"), "the class hurtboxScale follows hbH");

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
    Game unset;
    memset(&unset, 0, sizeof(unset));
    Player bare;
    memset(&bare, 0, sizeof(bare));
    bare.game = &unset;
    CHECK(player_hurtbox_scale(&bare) == 1.0f, "an unset hurtboxScale reads as 1");
}

int main(void) {
    test_lookup();
    test_wire();
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
