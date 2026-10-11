// Spawns every class in a local Game on Sandstorm and Kanji, checks the class/loadout setup against
// Krunker v3.1.1 (53.js / 27.js) and drives a short input script (walk, jump, crouch, aim, shoot,
// reload, swap) asserting the player state never goes NaN/inf.
#include <math.h>
#include <shared.h>
#include <stdio.h>
#include <string.h>

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

// Copied from Krunker v3.1.1 53.js (regen defaults to config.regenVal = 0.1, speed to 1).
static const ExpectedClass expected_classes[] = {
    {"Triggerman", 1, 1, 0, 100, 6, 0.1f, 1.05f},
    {"Hunter", 0, 1, 0, 60, 5, 0.1f, 1.05f},
    {"Run N Gun", 3, 0, 1, 100, 6, 0.1f, 1.18f},
    {"Spray N Pray", 6, 0, 0, 170, 7, 0.05f, 0.95f},
    {"Vince", 5, 1, 0, 90, 6, 0.1f, 1.0f},
    {"Detective", 4, 0, 0, 100, 6, 0.1f, 1.0f},
    {"Marksman", 7, 1, 0, 90, 6, 0.1f, 1.0f},
    {"Rocketeer", 8, 0, 0, 130, 7, 0.1f, 0.86f},
    {"Agent", 9, 0, 1, 100, 6, 0.1f, 1.2f},
    {"Runner", 12, 0, 1, 120, 6, 0.2f, 1.0f},
    {"Deagler", 10, 0, 0, 60, 5, 0.1f, 1.0f},
    {"Bowman", 13, 1, 0, 100, 6, 0.1f, 1.0f},
    {"Commando", 14, 1, 0, 100, 6, 0.1f, 1.0f},
    {"Trooper", 18, 0, 1, 100, 6, 0.1f, 1.0f},
};

// v3.1.1 weapon names by id (27.js order).
static const char *expected_weapons[] = {
    "Sniper Rifle", "Assault Rifle", "Pistol", "Submachine Gun", "Revolver", "Shotgun", "Machine Gun",
    "Semi Auto", "Rocket Launcher", "Akimbo Uzi", "Desert Eagle", "Alien Blaster", "Combat Knife",
    "Crossbow", "Famas", "Sawed Off", "Auto Pistol", "Bomb", "Blaster",
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
        const Weapon *w = g_weapons[i];
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
        if (w->custom_spread) {
            CHECK(w->shots > 0 && w->custom_spread_count >= w->ammo * w->shots, "%s cSpread has %u pairs, needs %u", w->name, w->custom_spread_count, w->ammo * w->shots);
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
    if (!player->weapon->melee) CHECK(player->ammo[0] < ammo_before, "%s did not fire (%d -> %d)", expected->name, ammo_before, player->ammo[0]);
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
    }
    packet.length = 6;
    NetEntPacket bad;
    CHECK(!net_packet_read_ent(&bad, &packet), "6-byte ENT accepted");

    NetSpawnPacket spawn = {.uid = 3, .class_index = 5, .secondary_index = -1, .cosmetics = {-1, -1, 1890, -1, -1, -1, 1891}};
    memcpy(packet.payload, &spawn, sizeof(spawn));
    const uint16_t spawn_sizes[] = {NET_SPAWN_PACKET_V1_SIZE, sizeof(NetSpawnPacket)};
    for (int i = 0; i < 2; i++) {
        NetSpawnPacket read;
        memset(&read, 0x55, sizeof(read));
        packet.length = spawn_sizes[i];
        CHECK(net_packet_read_spawn(&read, &packet), "SPAWN of %u bytes rejected", packet.length);
        CHECK(read.uid == 3 && read.class_index == 5, "SPAWN fields");
        CHECK(read.cosmetics.hat == (i ? 1890 : -1) && read.cosmetics.dye == (i ? 1891 : -1), "SPAWN cosmetics for %u bytes", packet.length);
    }
}

int main(void) {
    check_weapon_table();
    check_cosmetics_packets();

    const char *maps[] = {"sandstorm", "kanji"};
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

        CHECK(game.class_count == sizeof(expected_classes) / sizeof(expected_classes[0]), "class count %zu", game.class_count);
        CHECK(game.weapon_count == WEAPON_COUNT, "weapon count %zu", game.weapon_count);

        printf("Spawning %zu classes on %s...\n", game.class_count, maps[m]);
        for (int c = 0; c < (int) game.class_count; c++) check_class(&game, c, maps[m]);
    }

    if (failures) {
        printf("class_spawn_test: %d failure(s)\n", failures);
        return 1;
    }

    printf("Test OK\n");
    return 0;
}
