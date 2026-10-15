// Spawns every class in a local Game on Sandstorm and Kanji, checks the class/loadout setup against
// Krunker v7.1.4 (src/data/classes.js / weapons.js) and drives a short input script (walk, jump, crouch, aim, shoot,
// reload, swap) asserting the player state never goes NaN/inf.
#include <math.h>
#include <shared.h>
#include <stdio.h>
#include <string.h>
#include <ruleset.h>

typedef struct {
    const char *name;
    int primary;
    int secondary;
    int wall_jump;
    int health;
    int segments;
    float regen;
    float speed;
} ExpectedClass;

// Krunker v10.0.2's class table (shared/src/rules/v10/classes_v10.c), v7.1.4 src/data/classes.js
// where v10 kept it (v7 changes: Hunter speed 1.05 -> 1.0, Spray N Pray speed 0.95 -> 0.9, Rocketeer
// gets a secondary, Trooper speed 1.0 -> 0.9; new Survivor and Infiltrator). v10.0.2 moved three
// healths back from v7.1.4's: Run N Gun 90 -> 100, Spray N Pray 180 -> 170, Agent 110 -> 105.
static const ExpectedClass expected_classes[] = {
    {"Triggerman", 1, 1, 0, 100, 6, 0.1f, 1.05f},
    {"Hunter", 0, 1, 0, 60, 5, 0.1f, 1.0f},
    {"Run N Gun", 3, 0, 1, 100, 6, 0.1f, 1.18f},
    {"Spray N Pray", 6, 0, 0, 170, 7, 0.05f, 0.9f},
    {"Vince", 5, 1, 0, 90, 6, 0.1f, 1.0f},
    {"Detective", 4, 0, 0, 100, 6, 0.1f, 1.0f},
    {"Marksman", 7, 1, 0, 90, 6, 0.1f, 1.0f},
    {"Rocketeer", 8, 1, 0, 130, 7, 0.1f, 0.86f},
    {"Agent", 9, 0, 1, 105, 6, 0.1f, 1.2f},
    {"Runner", 12, 0, 1, 120, 6, 0.2f, 1.0f},
    {"Deagler", 10, 0, 0, 60, 5, 0.1f, 1.0f},
    {"Bowman", 13, 1, 0, 100, 6, 0.1f, 1.0f},
    {"Commando", 14, 1, 0, 100, 6, 0.1f, 1.0f},
    {"Trooper", 18, 0, 1, 100, 6, 0.1f, 0.9f},
    {"Survivor", 19, 0, 1, 150, 6, 0.1f, 0.8f},
    {"Infiltrator", 28, 1, 0, 90, 5, 0.1f, 1.0f},
};

// v7.1.4 weapon names by id (src/data/weapons.js order; 0-18 as in v3.1.1).
static const char *expected_weapons[] = {
    "Sniper Rifle", "Assault Rifle", "Pistol", "Submachine Gun", "Revolver", "Shotgun", "Machine Gun",
    "Semi Auto", "Rocket Launcher", "Akimbo Uzi", "Desert Eagle", "Alien Blaster", "Combat Knife",
    "Crossbow", "Famas", "Sawed Off", "Auto Pistol", "Bomb", "Blaster",
    "Build Tool", "Grappler", "Tehchy-9", "Noob Tube", "Slimer", "Zapper", "Minigun", "War Machine",
    "Akimbo Pistol", "Charge Rifle",
};

static int failures = 0;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int finite3(const vec3 v) {
    return isfinite(v.x) && isfinite(v.y) && isfinite(v.z);
}

static const cJSON *load_json(const char *path) {
    size_t length;
    unsigned char *data;
    if (read_file(path, &data, &length)) return NULL;
    const cJSON *parsed = cJSON_Parse((char *) data);
    free(data);
    return parsed;
}

static void check_weapon_table(void) {
    printf("Checking weapon table (%d weapons)...\n", WEAPON_COUNT);
    CHECK(sizeof(expected_weapons) / sizeof(expected_weapons[0]) == WEAPON_COUNT, "expected weapon list size");

    for (int i = 0; i < WEAPON_COUNT; i++) {
        const Weapon *w = rules_weapon(i);
        CHECK(w != NULL, "weapon %d missing", i);
        if (!w) continue;

        CHECK(w->name && !strcmp(w->name, expected_weapons[i]), "weapon %d is %s, expected %s", i, w->name ? w->name : "(null)", expected_weapons[i]);
        CHECK(w->swap_time > 0.0f, "%s swap_time must be > 0 (swap animation divides by it)", w->name);
        CHECK(w->aim_speed > 0.0f, "%s aim_speed must be > 0 (aim divides by it)", w->name);
        CHECK(w->zoom >= 1.0f, "%s zoom %f", w->name, w->zoom);
        CHECK(w->offset.z < 0.0f, "%s viewmodel offset must sit in front of the camera (z=%f)", w->name, w->offset.z);
        CHECK(w->melee || w->src, "%s has no model", w->name);
        if (w->src) CHECK(w->scale > 0.0f, "%s scale", w->name);
        if (!w->melee && !w->equipment) {
            CHECK(w->ammo > 0, "%s ammo", w->name);
            CHECK(w->rate > 0.0f, "%s rate", w->name);
            CHECK(w->reload_time > 0.0f, "%s reload_time", w->name);
        }
        if (w->custom_spread) { // v7.1.4 cSpread[ammo % 2]: two patterns of `shots` pairs
            CHECK(w->shots > 0 && w->custom_spread_count == 2 * w->shots, "%s cSpread has %u pairs, needs %u", w->name, w->custom_spread_count, 2 * w->shots);
        }
        if (w->burst) CHECK(w->burst_count > 1 && w->burst_rate > 0.0f, "%s burst", w->name);
    }
}

static void tick(Player *player, Input *input, const int frames) {
    for (int i = 0; i < frames; i++) {
        input->seq++;
        player_proc_input(player, input, 0, 0);
        input->swap = 0;
        input->reload = 0;
    }
}

static int player_state_ok(const Player *player) {
    return finite3(player->position) && finite3(player->velocity) &&
           isfinite(player->direction.x) && isfinite(player->direction.y) &&
           isfinite(player->aim_val) && isfinite(player->crouch_val) && isfinite(player->height) &&
           isfinite(player->recoil_anim) && isfinite(player->recoil_anim_y) && isfinite(player->recoil_force) &&
           isfinite(player->swap_timer) && isfinite(player->reload_timer) && isfinite(player->spread) &&
           player->weapon != NULL;
}

static void check_class(Game *game, const int class_index, const char *map_name) {
    const ExpectedClass *expected = &expected_classes[class_index];
    const ClassConfig *config = &game->classes[class_index];

    Player *player = player_init(game);
    CHECK(player != NULL, "player_init");
    if (!player) return;

    player->class_index = class_index;
    player_spawn(player);

    printf("  %-13s on %-9s: loadout", expected->name, map_name);
    for (int i = 0; i < player->loadout_size; i++) printf(" %s", game->weapons[player->loadout[i]]->name);
    printf("\n");

    CHECK(!strcmp(config->name, expected->name), "class %d name %s", class_index, config->name);
    CHECK(config->loadout && config->loadout[0] == expected->primary, "%s loadout id", expected->name);
    CHECK(config->secondary == expected->secondary, "%s secondary flag", expected->name);
    CHECK(config->wall_jump == expected->wall_jump, "%s wallJ", expected->name);
    CHECK(config->health_segments == expected->segments, "%s segs", expected->name);
    CHECK(fabsf(config->regen - expected->regen) < 1e-6f, "%s regen", expected->name);

    CHECK(player->active, "%s active after spawn", expected->name);
    CHECK(player->loadout_size == (expected->primary == WEAPON_ID_KNIFE ? 1 : expected->secondary ? 3 : 2), "%s loadout size %d", expected->name, player->loadout_size);
    CHECK(player->loadout_size > 0 && player->loadout[0] == expected->primary, "%s primary is %d, expected %d", expected->name, player->loadout_size ? player->loadout[0] : -1, expected->primary);
    if (expected->secondary) CHECK(player->loadout_size > 1 && player->loadout[1] == WEAPON_ID_PISTOL, "%s secondary", expected->name);
    CHECK(player->loadout[player->loadout_size - 1] == WEAPON_ID_KNIFE, "%s knife last", expected->name);
    CHECK(player->weapon == game->weapons[expected->primary], "%s holds primary", expected->name);
    CHECK(player->max_health == expected->health && fabsf(player->health - (float) expected->health) < 1e-3f, "%s health %f/%d", expected->name, player->health, player->max_health);
    CHECK(fabsf(player->speed - expected->speed) < 1e-6f, "%s speed %f", expected->name, player->speed);
    CHECK(player->cosmetics.skin_color == -1 && player->cosmetics.hat == -1 && player->cosmetics.dye == -1, "%s default cosmetics", expected->name);
    CHECK(player->skins.primary == -1 && player->skins.secondary == -1 && player->skins.melee == -1, "%s default weapon skins", expected->name);
    CHECK(player->wall_jump == expected->wall_jump, "%s player wall_jump", expected->name);
    for (int i = 0; i < player->loadout_size; i++) {
        CHECK(player->ammo[i] == (int) game->weapons[player->loadout[i]]->ammo, "%s ammo slot %d", expected->name, i);
    }

    Input input = {.delta = 1.0f / 60.0f, .move_dir = -1, .y_dir = player->direction.y, .x_dir = 0.0f};
    tick(player, &input, 30); // settle
    CHECK(player_state_ok(player), "%s NaN after settle", expected->name);

    input.move_dir = 0; // forward
    tick(player, &input, 30);
    input.jump = 1;
    tick(player, &input, 5);
    input.jump = 0;
    tick(player, &input, 40);
    input.crouch = 1;
    tick(player, &input, 30);
    input.crouch = 0;
    input.move_dir = -1;
    tick(player, &input, 20);
    CHECK(player_state_ok(player), "%s NaN after movement", expected->name);

    // aim then shoot from the hip and aimed
    input.scope = 1;
    tick(player, &input, 40);
    if (!player->weapon->no_aim) CHECK(player->aim_val > 0.99f, "%s should be fully aimed (aim_val %f)", expected->name, player->aim_val);
    input.scope = 0;
    tick(player, &input, 40);
    CHECK(player->aim_val == 0.0f, "%s aim_val %f after release", expected->name, player->aim_val);

    const int ammo_before = player->ammo[0];
    input.shoot = 1;
    tick(player, &input, 2);
    input.shoot = 0;
    tick(player, &input, 60);
    // v7.1.4: the Build Tool builds instead of firing (not ported), the Charge Rifle's normal
    // shots cost no ammo (fullChrgSht): both fire without spending a round here.
    const int spends_ammo = !player->weapon->melee && !player->weapon->build && !player->weapon->full_charge_shot;
    if (spends_ammo) CHECK(player->ammo[0] < ammo_before, "%s did not fire (%d -> %d)", expected->name, ammo_before, player->ammo[0]);
    if (player->weapon->burst) CHECK(ammo_before - player->ammo[0] == (int) player->weapon->burst_count, "%s one click should fire a %u-round burst (fired %d)", expected->name, player->weapon->burst_count, ammo_before - player->ammo[0]);
    else if (player->weapon->no_auto && !player->weapon->melee) CHECK(ammo_before - player->ammo[0] == 1, "%s semi-auto click fired %d", expected->name, ammo_before - player->ammo[0]);
    CHECK(player_state_ok(player), "%s NaN after shooting", expected->name);

    input.shoot = 1;
    tick(player, &input, 300); // empty the magazine (auto weapons) / keep clicking
    input.shoot = 0;
    input.reload = 1;
    tick(player, &input, 1);
    tick(player, &input, 300);
    if (!player->weapon->melee) CHECK(player->ammo[0] == (int) player->weapon->ammo, "%s reload refills (%d/%u)", expected->name, player->ammo[0], player->weapon->ammo);
    CHECK(player_state_ok(player), "%s NaN after reload", expected->name);

    // swap: E = secondary, Q = melee
    for (int swap = 1; swap <= 2; swap++) {
        input.swap = swap;
        tick(player, &input, 1);
        tick(player, &input, 60);
        const Weapon *held = player->weapon;
        if (swap == 1 && expected->secondary) CHECK(held->secondary, "%s E should hold secondary (holding %s)", expected->name, held->name);
        if (swap == 2 && player->loadout_size > 1) CHECK(held->melee, "%s Q should hold melee (holding %s)", expected->name, held->name);
        input.shoot = 1;
        tick(player, &input, 2);
        input.shoot = 0;
        tick(player, &input, 60);
        CHECK(player_state_ok(player), "%s NaN after swap %d", expected->name, swap);
    }

    free(player->loadout);
    free(player->ammo);
    free(player->reloads);
    free(player->input_queue);
    free(player);
}

// ENT / SPAWN keep accepting the shorter layouts of older peers (net-bridge sends a 78-byte SPAWN).
static void check_cosmetics_packets(void) {
    printf("Checking ENT/SPAWN cosmetics compatibility...\n");

    NetPacket packet = {0};
    NetEntPacket ent = {.class_index = 11, .secondary_index = 16, .cosmetics = {0xA77860, -1, 1890, 1487, -1, 1842, 1891}};
    memcpy(packet.payload, &ent, sizeof(ent));

    const uint16_t ent_sizes[] = {NET_ENT_PACKET_MIN_SIZE, NET_ENT_PACKET_V2_SIZE, sizeof(NetEntPacket)};
    for (int i = 0; i < 3; i++) {
        NetEntPacket read;
        memset(&read, 0x55, sizeof(read));
        packet.length = ent_sizes[i];
        CHECK(net_packet_read_ent(&read, &packet), "ENT of %u bytes rejected", packet.length);
        CHECK(read.class_index == 11, "ENT class");
        CHECK(read.secondary_index == (i ? 16 : -1), "ENT secondary for %u bytes", packet.length);
        CHECK(read.cosmetics.hat == (i == 2 ? 1890 : -1) && read.cosmetics.skin_color == (i == 2 ? 0xA77860 : -1), "ENT cosmetics for %u bytes", packet.length);
        CHECK(read.slid_cont == (i == 2 ? 0 : 1), "ENT slidCont for %u bytes (absent = on)", packet.length); // ent.slid_cont = 0 above
    }
    // ent[15] slidCont = 1 rides in the last byte; a V3 ENT (cosmetics, no tail) reads as on
    ent.slid_cont = 1;
    memcpy(packet.payload, &ent, sizeof(ent));
    packet.length = NET_ENT_PACKET_V4_SIZE;
    NetEntPacket full;
    CHECK(net_packet_read_ent(&full, &packet) && full.slid_cont == 1 && !full.spectate, "V4 ENT slidCont");
    packet.length = NET_ENT_PACKET_V3_SIZE;
    CHECK(net_packet_read_ent(&full, &packet) && full.slid_cont == 1, "V3 ENT slidCont default");
    // ent[1] sprayIndex rides after slidCont; shorter ENTs read as the default spray 0
    ent.spray_index = 870;
    memcpy(packet.payload, &ent, sizeof(ent));
    packet.length = NET_ENT_PACKET_V5_SIZE;
    CHECK(net_packet_read_ent(&full, &packet) && full.spray_index == 870, "V5 ENT sprayIndex");
    packet.length = NET_ENT_PACKET_V4_SIZE;
    CHECK(net_packet_read_ent(&full, &packet) && full.spray_index == 0, "V4 ENT sprayIndex default");
    packet.length = NET_ENT_PACKET_V4_SIZE + 2;
    CHECK(!net_packet_read_ent(&full, &packet), "ENT with a cut sprayIndex accepted");
    // ent[2] weapon skins + ent[9] meleeIndex ride after sprayIndex (V6); shorter ENTs read as none
    ent.skins = (PlayerSkins) {406, 1203, 1650};
    memcpy(packet.payload, &ent, sizeof(ent));
    packet.length = NET_ENT_PACKET_V6_SIZE;
    CHECK(net_packet_read_ent(&full, &packet) && full.spray_index == 870 && full.skins.primary == 406 && full.skins.secondary == 1203 &&
          full.skins.melee == 1650, "V6 ENT weapon skins");
    packet.length = NET_ENT_PACKET_V5_SIZE;
    CHECK(net_packet_read_ent(&full, &packet) && full.spray_index == 870 && full.skins.primary == -1 && full.skins.secondary == -1 &&
          full.skins.melee == -1, "V5 ENT weapon skins default");
    packet.length = NET_ENT_PACKET_V5_SIZE + 4;
    CHECK(!net_packet_read_ent(&full, &packet), "ENT with cut weapon skins accepted");
    // ent[11] lagComp (Settings > Network slider) rides after the weapon skins (V7); shorter ENTs read as 1
    ent.lag_comp = 0.3f;
    memcpy(packet.payload, &ent, sizeof(ent));
    packet.length = NET_ENT_PACKET_V7_SIZE;
    CHECK(net_packet_read_ent(&full, &packet) && full.lag_comp == 0.3f && full.skins.melee == 1650, "V7 ENT lagComp");
    packet.length = NET_ENT_PACKET_V6_SIZE;
    CHECK(net_packet_read_ent(&full, &packet) && full.lag_comp == 1.0f && full.skins.melee == 1650, "V6 ENT lagComp default 1");
    packet.length = NET_ENT_PACKET_V6_SIZE + 2;
    CHECK(!net_packet_read_ent(&full, &packet), "ENT with a cut lagComp accepted");
    packet.length = NET_ENT_PACKET_V7_SIZE + 1;
    CHECK(!net_packet_read_ent(&full, &packet), "ENT with a cut attachIndex accepted");
    // WP-C: ent[12] attachIndex, ent[7] challMode, ent[19] anonMode ride after lagComp (V8, 1.3.1);
    // shorter ENTs read as 0 / off / off, longer ones (a newer peer) as their V8 prefix
    packet.length = NET_ENT_PACKET_V7_SIZE;
    CHECK(net_packet_read_ent(&full, &packet) && full.attach_index == DEFAULT_ATTACHMENT_INDEX && !full.chall_mode && !full.anon_mode, "V7 ENT v7 flags default");
    ent.attach_index = 1;
    ent.chall_mode = 1;
    ent.anon_mode = 7; // any non-zero byte reads as on
    memcpy(packet.payload, &ent, sizeof(ent));
    packet.length = NET_ENT_PACKET_V8_SIZE;
    CHECK(net_packet_read_ent(&full, &packet) && full.attach_index == 1 && full.chall_mode == 1 && full.anon_mode == 1 && full.lag_comp == 0.3f, "V8 ENT attachIndex / challMode / anonMode");
    CHECK(full.cosmetics_v7.pet == -1 && full.cosmetics_v7.charms[1] == -1 && full.show_badges == PLAYER_SHOW_BADGES_ALL, "V8 ENT: no v7 slots, every badge shown");
    packet.length = NET_ENT_PACKET_V8_SIZE + 9;
    CHECK(net_packet_read_ent(&full, &packet) && full.attach_index == 1 && full.anon_mode == 1 && full.cosmetics_v7.head == -1, "ENT cut inside the WP-F tail reads as V8");
    // WP-F: the v7 slots (ent[15] shoe, [21] pet, [26] wrist, [30] charms, [31] back, [32] head, [33] card)
    // and ent[22..25] showBadges ride after anonMode (V9, 1.3.1); longer ENTs read their V9 prefix
    ent.cosmetics_v7 = (PlayerCosmeticsV7) {.head = 6401, .back = 246, .shoe = 2860, .pet = 2964, .wrist = 4249, .charms = {5000, -1}, .player_card = 6000, .badge = 3};
    ent.show_badges = PLAYER_SHOW_BADGE_PREMIUM | 0xF0;
    memcpy(packet.payload, &ent, sizeof(ent));
    packet.length = NET_ENT_PACKET_V9_SIZE;
    CHECK(net_packet_read_ent(&full, &packet) && full.cosmetics_v7.head == 6401 && full.cosmetics_v7.back == 246 && full.cosmetics_v7.shoe == 2860 &&
          full.cosmetics_v7.pet == 2964 && full.cosmetics_v7.wrist == 4249 && full.cosmetics_v7.charms[0] == 5000 && full.cosmetics_v7.player_card == 6000 &&
          full.show_badges == PLAYER_SHOW_BADGE_PREMIUM && full.attach_index == 1, "V9 ENT v7 slots / showBadges");
    packet.length = NET_ENT_PACKET_V9_SIZE + 7;
    CHECK(net_packet_read_ent(&full, &packet) && full.cosmetics_v7.pet == 2964 && full.anon_mode == 1, "ENT longer than V9 reads its prefix");
    ent.cosmetics_v7 = PLAYER_COSMETICS_V7_NONE;
    ent.show_badges = PLAYER_SHOW_BADGES_ALL;
    ent.attach_index = 0;
    ent.chall_mode = ent.anon_mode = 0;
    ent.lag_comp = 1.0f;
    ent.skins = PLAYER_SKINS_NONE;
    ent.spray_index = 0;
    // a client sends the long ENT only to servers announcing the protocol that reads it (INIT major.minor):
    // 1.0 servers (older server_bin builds, community servers) drop anything past V4 without a spawn,
    // 1.1 servers anything past V6
    CHECK(net_ent_size_for(1, 0, 0) == NET_ENT_PACKET_V4_SIZE, "ENT to a 1.0 server must stay V4 (%u)", net_ent_size_for(1, 0, 0));
    CHECK(net_ent_size_for(1, 1, 5) == NET_ENT_PACKET_V6_SIZE, "ENT to a 1.1 server must stay V6 (%u)", net_ent_size_for(1, 1, 5));
    CHECK(net_ent_size_for(1, 2, 1) == NET_ENT_PACKET_V7_SIZE && net_ent_size_for(1, 3, 0) == NET_ENT_PACKET_V7_SIZE, "ENT to a 1.2 / 1.3.0 server is V7");
    // 1.3.1 servers prefix-read longer ENTs: they get the WP-F V9 (a pre-WP-F 1.3.1 build reads its V8 prefix)
    CHECK(net_ent_size_for(1, 3, 1) == NET_ENT_PACKET_V9_SIZE && net_ent_size_for(1, 4, 0) == NET_ENT_PACKET_V9_SIZE, "ENT to a 1.3.1+ server is V9");
    CHECK(net_ent_size_for(2, 0, 0) == NET_ENT_PACKET_V9_SIZE && net_ent_size_for(0, 9, 9) == NET_ENT_PACKET_V4_SIZE, "ENT size by major");
    CHECK(NET_PROTOCOL_MAJOR == 1 && NET_PROTOCOL_MINOR >= 2, "server_bin announces protocol 1.2+");
    CHECK(net_peer_has_wpc(NET_PROTOCOL_MINOR, NET_PROTOCOL_PATCH), "this build takes its own WP-C sizes");
    CHECK(NET_ENT_PACKET_V7_SIZE == 58 && NET_ENT_PACKET_V6_SIZE == 54 && NET_ENT_PACKET_V8_SIZE == 64 && NET_ENT_PACKET_V9_SIZE == 101, "ENT V6 / V7 / V8 / V9 sizes (net-bridge, kn_proto.py)");
    ent.slid_cont = 0;
    memcpy(packet.payload, &ent, sizeof(ent));
    packet.length = 6;
    NetEntPacket bad;
    CHECK(!net_packet_read_ent(&bad, &packet), "6-byte ENT accepted");

    NetSpawnPacket spawn = {.uid = 3, .class_index = 5, .secondary_index = -1, .cosmetics = {-1, -1, 1890, -1, -1, -1, 1891}};
    memcpy(packet.payload, &spawn, sizeof(spawn));
    spawn.attach_index = 1;
    spawn.head_mlt = 1.5f;
    spawn.cosmetics_v7 = (PlayerCosmeticsV7) {.head = 6401, .back = -1, .shoe = 2860, .pet = 2964, .wrist = -1, .charms = {-1, -1}, .player_card = -1, .badge = 2};
    memcpy(packet.payload, &spawn, sizeof(spawn));
    // V1, V2 (1.0 - 1.3.0 servers), V3 (WP-C: attachIndex, headMlt), a V3 cut inside the WP-F tail,
    // V4 (WP-F: the v7 slots) and a longer one (V4 prefix)
    const uint16_t spawn_sizes[] = {NET_SPAWN_PACKET_V1_SIZE, NET_SPAWN_PACKET_V2_SIZE, NET_SPAWN_PACKET_V3_SIZE, NET_SPAWN_PACKET_V3_SIZE + 4,
                                    NET_SPAWN_PACKET_V4_SIZE, NET_SPAWN_PACKET_V4_SIZE + 4};
    for (int i = 0; i < 6; i++) {
        NetSpawnPacket read;
        memset(&read, 0x55, sizeof(read));
        packet.length = spawn_sizes[i];
        CHECK(net_packet_read_spawn(&read, &packet), "SPAWN of %u bytes rejected", packet.length);
        CHECK(read.uid == 3 && read.class_index == 5, "SPAWN fields");
        CHECK(read.cosmetics.hat == (i ? 1890 : -1) && read.cosmetics.dye == (i ? 1891 : -1), "SPAWN cosmetics for %u bytes", packet.length);
        CHECK(read.attach_index == (i >= 2 ? 1 : DEFAULT_ATTACHMENT_INDEX) && read.head_mlt == (i >= 2 ? 1.5f : 1.0f), "SPAWN attachIndex / headMlt for %u bytes", packet.length);
        CHECK(i >= 4 ? read.cosmetics_v7.head == 6401 && read.cosmetics_v7.pet == 2964 && read.cosmetics_v7.badge == 2 && read.cosmetics_v7.wrist == -1
                     : read.cosmetics_v7.head == -1 && read.cosmetics_v7.pet == -1 && read.cosmetics_v7.badge == -1, "SPAWN v7 slots for %u bytes", packet.length);
    }
    packet.length = NET_SPAWN_PACKET_V2_SIZE + 4;
    NetSpawnPacket cut;
    CHECK(!net_packet_read_spawn(&cut, &packet), "SPAWN with a cut headMlt accepted");
}

int main(void) {
    check_weapon_table();
    check_cosmetics_packets();

    const char *maps[] = {"311_sandstorm", "311_kanji"}; // the v3.1.1 maps
    for (size_t m = 0; m < sizeof(maps) / sizeof(maps[0]); m++) {
        char path[128];
        snprintf(path, sizeof(path), "assets/maps/%s.json", maps[m]);
        const cJSON *map = load_json(path);
        if (!map) {
            snprintf(path, sizeof(path), "../assets/maps/%s.json", maps[m]);
            map = load_json(path);
        }
        if (!map) {
            printf("Failed to load %s (run from the repository root)\n", maps[m]);
            return 1;
        }

        Game game = {};
        game_configure(&game, NULL, &map, 1, NULL, 0, NULL, 0, NULL, 0);
        game_init(&game, 0, -1, 1);

        // Krunker's 16 classes, then the KrunkNative Nettspend (CLASS_TABLE_COUNT; custom games only,
        // so a public game like this one spawns it as the Triggerman: shared/tests/nettspend_test.c)
        CHECK(sizeof(expected_classes) / sizeof(expected_classes[0]) == CLASS_COUNT && game.class_count == CLASS_TABLE_COUNT, "class count %zu", game.class_count);
        CHECK(game.weapon_count == WEAPON_TABLE_COUNT, "weapon count %zu", game.weapon_count);

        printf("Spawning %d classes on %s...\n", CLASS_COUNT, maps[m]);
        for (int c = 0; c < CLASS_COUNT; c++) check_class(&game, c, maps[m]);
    }

    if (failures) {
        printf("class_spawn_test: %d failure(s)\n", failures);
        return 1;
    }

    printf("Test OK\n");
    return 0;
}
