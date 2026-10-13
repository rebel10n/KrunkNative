// map-logic WP-H stage 2 against Krunker v7.1.4 (modes/zom.js, data/game.js mystery box / krum
// machine / power-ups, players.js interact 13-15, medals.js, data/airDrop.js, airDropPackages.js;
// doc/v7/map-logic.md §2.7, §4.1b, §4.4) on shared/tests/maps/v7_zombies.json: wave progression
// and scaling, power-up drop rates with a seeded RNG, mystery box / krum machine / perk machine
// states and prices, the perk cap, finiteAmmo reserves, insta-kill / double points / fire sale,
// airdrop tiers, falls and claims, the joiner sync and the client mirror.
// Run from the repository root: ./<build>/zombies_test
#include <shared.h>
#include <zombies.h>
#include <spawnables.h>
#include <ai.h>
#include <map_ai.h>
#include <pcg_basic.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

void player_reload(Player*); // shared/src/player.c

static int g_failures, g_checks;
#define CHECK(cond, ...) do { g_checks++; \
    if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_failures++; } \
} while (0)

// every packet counted by type; the last one of each kept
static int g_count[256];
static uint8_t g_last[256][NET_MAX_PAYLOAD];
static int g_last_uid[256];
static int g_spawnable_drops[SPAWNABLE_COUNT];

static void test_send(Game *game, const int uid, const int type, const void *payload, const uint16_t length) {
    (void) game;
    if (type < 0 || type > 255) return;
    g_count[type]++;
    g_last_uid[type] = uid;
    memcpy(g_last[type], payload, length < NET_MAX_PAYLOAD ? length : NET_MAX_PAYLOAD);
    if (type == NET_PACKET_SPAWNABLE_V7) {
        NetSpawnableV7Packet p;
        memcpy(&p, payload, sizeof(p));
        if (p.active && p.type < SPAWNABLE_COUNT) g_spawnable_drops[p.type]++;
    }
}

static void test_respawn(Game *game, const int uid) {
    Player *player = game_find_player(game, uid);
    if (player) player_spawn(player);
}

static void reset_sent(void) {
    memset(g_count, 0, sizeof(g_count));
    memset(g_spawnable_drops, 0, sizeof(g_spawnable_drops));
}

static const cJSON *g_map;

static void free_players(Game *game) {
    for (size_t i = 0; i < game->player_count; i++) {
        Player *player = game->players[i];
        free(player->loadout);
        free(player->ammo);
        free(player->reloads);
        free(player->input_queue);
        free(player);
    }
    free(game->players);
    game->players = NULL;
    game->player_count = 0;
}

static void start(Game *game, const int mode_id, const int is_local) {
    free_players(game);
    int *modes = calloc(1, sizeof(int));
    modes[0] = mode_id;
    const cJSON **maps = calloc(1, sizeof(cJSON *));
    maps[0] = g_map;
    game_configure(game, NULL, maps, 1, modes, 1, NULL, 0, NULL, 0);
    game->hooks.send = test_send;
    game->hooks.respawn = test_respawn;
    reset_sent();
    game_init(game, 0, 0, (unsigned char) is_local);
    game->now = 0.0f;
}

static Player *add_player(Game *game, const int uid) {
    Player *player = player_init(game);
    player->uid = uid;
    player->class_index = 0;
    player->has_account = 1;
    snprintf(player->name, sizeof(player->name), "P%d", uid);
    game_players_add(game, player);
    player_spawn(player);
    player->god_mode = 1; // the zombies walk in while the clock runs
    return player;
}

static void tick(Game *game, const float seconds, const float step) {
    for (float t = 0.0f; t < seconds - 1e-4f; t += step) game_tick(game, game->now + step, step);
}

static void place(Player *player, const float x, const float z) {
    player->position = (vec3) {x, 0.0f, z};
    player->last_position = player->position;
    player->velocity = (vec3) {0.0f, 0.0f, 0.0f};
    player->on_ground = 1;
}

static int object_of_prefab(const Game *game, const unsigned int prefab, const int nth) {
    int seen = 0;
    for (size_t i = 0; i < game->map->object_count; i++) {
        if (game->map->objects[i]->prefab == prefab && seen++ == nth) return (int) i;
    }
    return -1;
}

static void kill_all_ais(Game *game, Player *killer) {
    for (int i = 0; game->ai && i < game->ai->count; i++) {
        if (game->ai->ais[i].active) game_ai_kill(game, &game->ai->ais[i], killer);
    }
}

static int has_weapon(const Player *player, const int id) {
    for (int i = 0; i < player->loadout_size; i++) {
        if (player->loadout[i] == id) return 1;
    }
    return 0;
}

// ------------------------------------------------------------------ waves

static void test_waves(Game *game) {
    printf("waves...\n");
    pcg32_srandom(7, 11);
    start(game, MODE_ID_ZOM, 1);
    Player *a = add_player(game, 1);
    Player *b = add_player(game, 2);
    GameZombies *z = &game->zombies;
    CHECK(z->wave == 0 && z->conc == 5 && z->counts[0] == 10 && z->counts[3] == 1, "gameStart: zWaveData");
    CHECK(z->pap_order[0] != z->pap_order[1] && z->pap_order[1] != z->pap_order[2], "eePAPOrder is a permutation");

    tick(game, 12.5f, 0.5f); // the lives modes' 12 s start wait
    tick(game, 0.5f, 0.5f);
    CHECK(z->wave == 1 && g_count[NET_PACKET_ZOMBIE_WAVE] == 1, "wave 1 starts (%d)", z->wave);
    CHECK(z->conc == 7 && z->left == 11 && z->pending_count == 11 && z->counts[0] == 10 && z->counts[3] == 1, "wave 1: 10 Walkers, 1 Runner, 7 at once");
    tick(game, 7.5f, 0.5f);
    CHECK(game_ai_active_count(game, 2) == 7 && z->left == 4, "7 spawn after 7.5 s (%d, left %d)", game_ai_active_count(game, 2), z->left);
    int walker_level = -1;
    for (int i = 0; i < game->ai->count; i++) {
        const GameAI *ai = &game->ai->ais[i];
        if (ai->active && !strcmp(ai->name, "Walker")) walker_level = ai->data.lvl;
    }
    int walker_order = 0;
    for (int i = 0; i < 3; i++) walker_order = z->pap_order[i] == 0 ? i : walker_order;
    CHECK(walker_level == 10 + walker_order + 1, "Walker level = ail + portal order index + 1 (%d)", walker_level);

    kill_all_ais(game, a);
    tick(game, 10.0f, 0.5f);
    CHECK(game_ai_active_count(game, 2) == 4 && z->left == 0, "the other 4 next");
    a->score = 1000;
    player_kill(b, NULL, NULL, 0);
    CHECK(!b->active && a->score == 900, "a death costs 10%% (%d)", a->score);
    b->score = 400;
    kill_all_ais(game, a);
    tick(game, 10.0f, 0.5f);
    CHECK(z->wave == 2 && z->counts[0] == 11 && z->counts[1] == 1 && z->counts[2] == 0 && z->counts[3] == 1, "wave 2 counts");
    CHECK(fabsf(z->hp_mlt - 1.1f) < 1e-5f && z->dmg_mlt == 1.0f, "hp +0.1 every 2 waves");
    CHECK(b->active && b->mode.lives == 1 && b->score == 200, "the downed come back with half their score (%d)", b->score);

    // wave 3: zombCnts[3] grows (v7's Math.min(zombCnts[2], 4) bug fixed: Runners stay)
    while (z->wave < 3) {
        kill_all_ais(game, a);
        z->left = 0;
        z->pending_count = 0;
        tick(game, 10.0f, 0.5f);
    }
    CHECK(z->counts[3] == 2, "wave 3: 2 Runners (%d)", z->counts[3]);
    tick(game, 7.5f, 0.5f);
    float walker_hp = 0.0f;
    for (int i = 0; i < game->ai->count; i++) {
        if (game->ai->ais[i].active && !strcmp(game->ai->ais[i].name, "Walker")) walker_hp = game->ai->ais[i].max_health;
    }
    CHECK(walker_hp == 220.0f, "Walkers have round(200 x 1.1) HP (%.0f)", walker_hp);
    while (z->wave < 5) {
        kill_all_ais(game, a);
        z->left = 0;
        z->pending_count = 0;
        tick(game, 10.0f, 0.5f);
    }
    NetZombieWavePacket wave;
    memcpy(&wave, g_last[NET_PACKET_ZOMBIE_WAVE], sizeof(wave));
    CHECK(wave.wave == 5 && wave.special[0], "wave 5 is a special wave (%s)", wave.special);
    CHECK(z->counts[2] == 1 && fabsf(z->spd_mlt - 1.1f) < 1e-5f, "wave 4: a Colossal more, speed +0.1");

    // the last kill of a 5th wave drops an Ammo Cache
    tick(game, 7.5f, 0.5f);
    z->left = 0;
    z->pending_count = 0;
    memset(g_spawnable_drops, 0, sizeof(g_spawnable_drops));
    kill_all_ais(game, NULL);
    CHECK(g_spawnable_drops[SPAWNABLE_AMMO_CACHE] == 1, "Ammo Cache on the 5th wave's last kill (%d)", g_spawnable_drops[SPAWNABLE_AMMO_CACHE]);

    mode_on_game_end(game);
    CHECK(a->score == 500, "onPreEnd: score = wave x 100 (%d)", a->score);
    // endStats zombies (aiKills) / rounds (one per zWave++ for everyone in the list)
    CHECK(a->mode.rounds == 5 && b->mode.rounds == 5, "rounds: 5 waves (%d / %d)", a->mode.rounds, b->mode.rounds);
    CHECK(a->mode.ai_kills >= 11 && b->mode.ai_kills == 0, "aiKills of the killer only (%d / %d)", a->mode.ai_kills, b->mode.ai_kills);
    NetZombieStatsPacket stats;
    memcpy(&stats, g_last[NET_PACKET_ZOMBIE_STATS], sizeof(stats));
    int stats_a = -1;
    for (int i = 0; i < stats.count; i++) stats_a = stats.entries[i].uid == a->uid ? i : stats_a;
    CHECK(g_count[NET_PACKET_ZOMBIE_STATS] == 1 && stats.count == 2 && stats_a >= 0 && stats.entries[stats_a].zombies == a->mode.ai_kills &&
          stats.entries[stats_a].rounds == 5, "onPreEnd sends the zombies / rounds end stats");
}

// ------------------------------------------------------------------ drops

static void test_drops(Game *game) {
    printf("power-up drops...\n");
    start(game, MODE_ID_ZOM, 1);
    Player *a = add_player(game, 1);
    GameAI ai;
    memset(&ai, 0, sizeof(ai));
    ai.position = (vec3) {0.0f, 0.0f, 50.0f};
    pcg32_srandom(1234, 5);
    memset(g_spawnable_drops, 0, sizeof(g_spawnable_drops));
    const int n = 20000;
    for (int i = 0; i < n; i++) mode_v7_on_ai_death(game, &ai, a);
    const int t1 = g_spawnable_drops[SPAWNABLE_AMMO_CACHE] + g_spawnable_drops[SPAWNABLE_DOUBLE_POINTS];
    const int t2 = g_spawnable_drops[SPAWNABLE_MINIGUN] + g_spawnable_drops[SPAWNABLE_GRENADE_LAUNCHER];
    const int t3 = g_spawnable_drops[SPAWNABLE_NUKE] + g_spawnable_drops[SPAWNABLE_INSTA_KILL] + g_spawnable_drops[SPAWNABLE_DISCOUNT];
    const int bottles = g_spawnable_drops[SPAWNABLE_PERK_BOTTLE];
    CHECK(abs(t1 - n * 5 / 100) < n / 100, "5%% ammo cache / double points (%d)", t1);
    CHECK(abs(t2 - n * 4 / 100) < n / 100, "4%% minigun / launcher (%d)", t2);
    CHECK(abs(t3 - n * 2 / 100) < n / 100, "2%% nuke / insta-kill / discount (%d)", t3);
    CHECK(abs(bottles - n / 100) < n / 200, "1%% perk bottle (%d)", bottles);
    CHECK(g_spawnable_drops[SPAWNABLE_AMMO_CACHE] && g_spawnable_drops[SPAWNABLE_DOUBLE_POINTS] && g_spawnable_drops[SPAWNABLE_DISCOUNT], "every power-up drops");
    memset(g_spawnable_drops, 0, sizeof(g_spawnable_drops));
    for (int i = 0; i < 100; i++) mode_v7_on_ai_death(game, &ai, NULL);
    CHECK(!g_spawnable_drops[SPAWNABLE_PERK_BOTTLE] && !g_spawnable_drops[SPAWNABLE_NUKE], "no drops without a player kill");
}

// ------------------------------------------------------------------ power-ups

static void test_powerups(Game *game) {
    printf("power-ups...\n");
    start(game, MODE_ID_ZOM, 1);
    Player *a = add_player(game, 1);
    const cJSON *bag = cJSON_GetArrayItem(cJSON_GetObjectItem(g_map, "objects"), 0); // any key bag: a 1 HP AI
    cJSON *walker = cJSON_Parse("{\"ain\":\"Dummy\",\"h\":500,\"spd\":0,\"tm\":2}");
    (void) bag;
    const int sid = map_ai_spawn(game, walker, (vec3) {0.0f, 0.0f, 400.0f}, -1);
    cJSON_Delete(walker);
    GameAI *ai = game_ai_find(game, sid);
    CHECK(ai && ai->health == 500.0f, "a 500 HP AI");
    mode_zombies_spawnable(game, a, NULL, SPAWNABLE_INSTA_KILL);
    CHECK(zombies_insta_kill(game) && g_count[NET_PACKET_POWERUP] == 1, "Insta-kill: 30 s + \"pwup\"");
    if (ai) game_ai_damage(game, ai, a, 1.0f, 0);
    CHECK(ai && !ai->active, "insta-kill: one hit");
    const int score = a->score;
    mode_zombies_spawnable(game, a, NULL, SPAWNABLE_DOUBLE_POINTS);
    player_score(a, 10, 0);
    CHECK(a->score == score + 20, "Double Points (%d)", a->score - score);
    player_score(a, -10, 0);
    CHECK(a->score == score + 10, "losses are not doubled");
    tick(game, 30.5f, 0.5f);
    CHECK(!zombies_insta_kill(game) && !zombies_double_points(game), "both run out after 30 s");
    NetPowerupPacket off;
    memcpy(&off, g_last[NET_PACKET_POWERUP], sizeof(off));
    CHECK(off.ms == 0, "\"pwup\" 0 when one ends");
}

// ------------------------------------------------------------------ machines

static void test_machines(Game *game) {
    printf("mystery box / krum machine / perk machine...\n");
    pcg32_srandom(99, 3);
    start(game, MODE_ID_ZOM, 1);
    Player *a = add_player(game, 1);
    GameZombies *z = &game->zombies;
    CHECK(z->box_count == 2 && z->krum_count == 1 && z->perk_machine_count == 1, "the machines of the map (%d / %d / %d)", z->box_count, z->krum_count, z->perk_machine_count);
    const int box0 = object_of_prefab(game, PREFAB_MYSTERY_BOX, 0), box1 = object_of_prefab(game, PREFAB_MYSTERY_BOX, 1);
    const int on = zombies_active_mystery_box(game);
    CHECK((on == box0) != (game->map->objects[box1]->active != 0) && on >= 0 && g_count[NET_PACKET_MYSTERY_BOX_SWITCH] == 1, "one box on (switchMysteryBox)");
    const int off = on == box0 ? box1 : box0;
    tick(game, 12.5f, 0.5f); // machines wait for the clock (waitTimers[0].contTime), as gates do

    // a roll: 950, shared box price 475; then collect
    Object *box = game->map->objects[on];
    place(a, box->position.x, box->position.z + 15.0f);
    a->score = 100;
    game_player_interact(game, a, 1, 0);
    CHECK(zombies_machine(game, on)->state == 0, "not enough score");
    a->score = 2000;
    a->sharing_is_caring = 1; // no teddy, half price
    CHECK(zombies_mystery_box_price(game, a, box) == 475, "sharingIsCaring: ceil(950 / 2)");
    CHECK(game_interact_prompt(game, a, NULL) == GAME_INTERACT_MYSTERY_BOX, "the type 13 prompt");
    a->last_interact = -1.0f;
    game_player_interact(game, a, 1, 0);
    const ZombieMachine *m = zombies_machine(game, on);
    CHECK(m->state == 1 && m->purchased_by == a->uid && m->pending_weapon >= 0 && a->score == 2000 - 475, "rolling (%d, weapon %d, score %d)", m->state, m->pending_weapon, a->score);
    const int weapon = m->pending_weapon;
    CHECK(game_interact_prompt(game, a, NULL) != GAME_INTERACT_MYSTERY_BOX, "no prompt while rolling");
    tick(game, 5.0f, 0.5f);
    CHECK(m->state == 2, "collect after 5 s");
    a->last_interact = -1.0f;
    game_player_interact(game, a, 1, 0);
    CHECK(m->state == 0 && m->pending_weapon == ZOMBIES_NONE && has_weapon(a, weapon), "the weapon is taken (%d)", weapon);
    CHECK(a->loadout_size == 3 && a->loadout[a->loadout_size - 1] == WEAPON_ID_KNIFE, "zombies loadout: 2 guns + knife");

    // nobody collects: idle again after 15 s
    a->sharing_is_caring = 0;
    a->last_interact = -2.0f;
    pcg32_srandom(5, 5);
    int teddy = 0;
    for (int i = 0; i < 200 && !teddy; i++) teddy = zombies_mystery_box_weapon(game, ZOMBIES_TEDDY_CHANCE) == -1;
    CHECK(teddy, "a teddy can come up");
    CHECK(zombies_mystery_box_weapon(game, 100) == -1, "teddy chance 100");

    // the teddy: the box moves after its 8 s
    ZombieMachine *mm = (ZombieMachine *) zombies_machine(game, on);
    mm->state = 1;
    mm->timer = ZOMBIES_MYSTERY_BOX_ANIM;
    mm->pending_weapon = -1;
    mm->purchased_by = a->uid;
    tick(game, 5.0f + 8.0f, 0.5f);
    CHECK(zombies_active_mystery_box(game) == off && !game->map->objects[on]->active, "the teddy moves the box");

    // fire sale: every box on at 10, no teddy, back to one box after 20 s
    mode_zombies_spawnable(game, a, NULL, SPAWNABLE_DISCOUNT);
    CHECK(game->map->objects[box0]->active && game->map->objects[box1]->active && zombies_mystery_box_price(game, a, box) == 10, "fire sale: every box, price 10");
    CHECK(zombies_mystery_box_weapon(game, 100) != -1, "no teddy in a fire sale");
    tick(game, 20.5f, 0.5f);
    CHECK((game->map->objects[box0]->active != 0) + (game->map->objects[box1]->active != 0) == 1, "fire sale over: one box");

    // krum machine: 5000, the held pistol comes back upgraded
    const int krum = object_of_prefab(game, PREFAB_KRUM_MACHINE, 0);
    Object *machine = game->map->objects[krum];
    player_spawn(a);
    a->god_mode = 1;
    place(a, machine->position.x, machine->position.z + 15.0f);
    a->score = 6000;
    CHECK(a->loadout[a->loadout_index] == WEAPON_ID_PISTOL && game_interact_prompt(game, a, NULL) == GAME_INTERACT_KRUM, "the type 15 prompt with the pistol");
    a->last_interact = -1.0f;
    game_player_interact(game, a, 1, 0);
    const ZombieMachine *k = zombies_machine(game, krum);
    CHECK(k->state == 1 && k->pending_weapon == WEAPON_ID_PISTOL && !has_weapon(a, WEAPON_ID_PISTOL) && a->score == 1000, "the pistol goes in (score %d)", a->score);
    tick(game, 5.0f, 0.5f);
    a->last_interact = -1.0f;
    game_player_interact(game, a, 1, 0);
    int slot = -1;
    for (int i = 0; i < a->loadout_size; i++) slot = a->loadout[i] == WEAPON_ID_PISTOL ? i : slot;
    CHECK(slot >= 0 && (a->upgraded_slots >> slot & 1) && a->weapon == weapon_upgraded(WEAPON_ID_PISTOL) && k->state == 0, "upgraded pistol back in hand");
    CHECK(game_interact_prompt(game, a, NULL) != GAME_INTERACT_KRUM, "no krum prompt for an upgraded weapon");

    // perk machine (zp 2, 2500): the crouch bonus once, the perk, the cap of 4
    const int perk = object_of_prefab(game, PREFAB_PERK_MACHINE, 0);
    Object *perks = game->map->objects[perk];
    player_spawn(a);
    a->god_mode = 1;
    place(a, perks->position.x, perks->position.z + 10.0f);
    a->score = 0;
    zombies_perk_machine_touch(game, a, perk);
    CHECK(a->score == 0, "standing: no free points");
    a->crouch_val = 1.0f;
    zombies_perk_machine_touch(game, a, perk);
    zombies_perk_machine_touch(game, a, perk);
    CHECK(a->score == 50, "crouching on it: +50 once (%d)", a->score);
    a->score = 3000;
    CHECK(game_interact_prompt(game, a, NULL) == GAME_INTERACT_PERK, "the type 14 prompt");
    a->last_interact = -1.0f;
    game_player_interact(game, a, 1, 0);
    CHECK(player_has_perk(a, PERK_FAST_RELOAD) && a->score == 500, "Western Hands bought (%d)", a->score);
    player_spawn(a);
    a->god_mode = 1;
    place(a, perks->position.x, perks->position.z + 10.0f);
    a->score = 3000;
    player_give_perk(a, 0);
    player_give_perk(a, 1);
    player_give_perk(a, 3);
    player_give_perk(a, 4);
    a->last_interact = -1.0f;
    game_player_interact(game, a, 1, 0);
    CHECK(!player_has_perk(a, PERK_FAST_RELOAD) && a->score == 3000, "maxZombiePerks 4");
    // the Perk Bottle goes past 4 (perks.length < zombiePerks.length)
    mode_zombies_spawnable(game, a, NULL, SPAWNABLE_PERK_BOTTLE);
    CHECK(a->perk_count == 5, "a perk bottle gives a fifth perk");

    // joiner sync: power-ups, both boxes, the krum machine
    reset_sent();
    game_send_round_state(game, 7);
    CHECK(g_count[NET_PACKET_ZOMBIE_MACHINE] == 3, "mybx x2 + krm to a joiner (%d)", g_count[NET_PACKET_ZOMBIE_MACHINE]);
}

// ------------------------------------------------------------------ easter eggs

static int find_spawnable(const Game *game, const int type) {
    for (int s = 0; s < MODE_MAX_SPAWNABLES; s++) {
        if (game->mode->spawnables[s].active && game->mode->spawnables[s].type == type) return s;
    }
    return -1;
}

static GameAI *spawn_named(Game *game, const char *name, const vec3 at) {
    char json[128];
    snprintf(json, sizeof(json), "{\"ain\":\"%s\",\"h\":100,\"spd\":0,\"tm\":2}", name);
    cJSON *bag = cJSON_Parse(json);
    const int sid = map_ai_spawn(game, bag, at, -1);
    cJSON_Delete(bag);
    return game_ai_find(game, sid);
}

static void test_easter_egg(Game *game) {
    printf("Zombie Portal / skulls...\n");
    pcg32_srandom(31, 7);
    start(game, MODE_ID_ZOM, 1);
    Player *a = add_player(game, 1);
    tick(game, 12.5f, 0.5f);
    GameZombies *z = &game->zombies;
    static const char *const names[] = {"Walker", "Brute", "Flesh Colossal", "Runner"};
    mode_v7_on_trigger(game, a, 0);
    int portal = find_spawnable(game, SPAWNABLE_ZOMBIE_PORTAL);
    CHECK(portal >= 0 && z->pap_active, "action 71 opens the portal at portal_spawn");
    mode_v7_on_trigger(game, a, 0);
    CHECK(find_spawnable(game, SPAWNABLE_ZOMBIE_PORTAL) == portal, "only one portal at a time");
    const vec3 at = game->mode->spawnables[portal].position;
    // a wrong zombie first: the order restarts, the portal closes
    const int wrong = z->pap_order[1];
    spawnable_pickup_ai(game, spawn_named(game, names[wrong], at));
    CHECK(z->pap_progress == 0 && !z->pap_active && find_spawnable(game, SPAWNABLE_ZOMBIE_PORTAL) < 0, "wrong zombie: reset, portal gone");
    for (int i = 0; i < 3; i++) {
        mode_v7_on_trigger(game, a, 0);
        GameAI *ai = spawn_named(game, names[z->pap_order[i]], at);
        CHECK(spawnable_pickup_ai(game, ai) && !ai->active, "zombie %d of the order goes in", i);
    }
    CHECK(z->pap_completed && z->dance_timer > 0.0f, "the easter egg is complete");
    int skulls = 0;
    for (int s = 0; s < MODE_MAX_SPAWNABLES; s++) skulls += game->mode->spawnables[s].active && game->mode->spawnables[s].type == SPAWNABLE_SKULL_EE;
    CHECK(skulls == 2, "a skull at each skull_spawn (%d)", skulls);
    memset(g_spawnable_drops, 0, sizeof(g_spawnable_drops));
    for (int k = 0; k < 2; k++) {
        const int s = find_spawnable(game, SPAWNABLE_SKULL_EE);
        const ModeSpawnable *skull = &game->mode->spawnables[s];
        place(a, skull->position.x - 30.0f, skull->position.z);
        a->position.y = skull->position.y - a->height + game_constants.camera_height;
        const vec3 eye = {a->position.x, skull->position.y, a->position.z};
        zombies_shoot_spawnables(game, a, eye, (vec3) {100.0f, 0.0f, 0.0f}, 2.0f);
    }
    CHECK(z->dance_completed && game->ai->force_dance_timer > 0.0f, "both skulls shot: the AIs dance");
    CHECK(g_spawnable_drops[SPAWNABLE_FREE_UPGRADE] + g_spawnable_drops[SPAWNABLE_PERK_BOTTLE] == 4, "4 rewards at the first spawn");
}

// ------------------------------------------------------------------ finiteAmmo

static void test_finite_ammo(Game *game) {
    printf("finiteAmmo...\n");
    start(game, MODE_ID_ZOM, 1);
    Player *a = add_player(game, 1);
    const Weapon *pistol = game->weapons[WEAPON_ID_PISTOL];
    const int full = (int) pistol->ammo * (pistol->mag_reserve ? pistol->mag_reserve : 1);
    CHECK(zombies_finite_ammo(a) && player_reserve_ammo(a, 0) == full, "spawn: magReserve full mags (%d)", player_reserve_ammo(a, 0));
    a->ammo[0] = 2;
    player_reload(a);
    CHECK(a->reload_timer > 0.0f, "reloading");
    a->reload_timer = 0.001f;
    Input input;
    memset(&input, 0, sizeof(input));
    input.delta = 10.0f;
    input.move_dir = -1;
    player_proc_input(a, &input, 0, 0);
    CHECK(a->ammo[0] == pistol->ammo && player_reserve_ammo(a, 0) == full - ((int) pistol->ammo - 2), "the refill comes from the reserve (%u, %d)", a->ammo[0], player_reserve_ammo(a, 0));
    player_set_reserve_ammo(a, 0, 3);
    a->ammo[0] = 0;
    player_reload(a);
    a->reload_timer = 0.001f;
    player_proc_input(a, &input, 0, 0);
    CHECK(a->ammo[0] == 3 && player_reserve_ammo(a, 0) == 0, "the last 3 rounds");
    a->ammo[0] = 0;
    player_reload(a);
    CHECK(a->reload_timer == 0.0f, "an empty reserve: no reload");
    mode_zombies_spawnable(game, a, NULL, SPAWNABLE_NUKE); // (nothing to do with ammo: the Nuke kills AIs, +400)
    reset_sent();
    spawnable_drop(game, SPAWNABLE_AMMO_CACHE, NULL, 0, a->position);
    spawnables_update(game, 0.1f);
    CHECK(player_reserve_ammo(a, 0) == full && g_count[NET_PACKET_RESERVE_AMMO] >= 1, "the Ammo Cache refills the reserves");
}

// ------------------------------------------------------------------ airdrops

static void test_airdrops(Game *game) {
    printf("airdrops...\n");
    start(game, MODE_ID_FFA, 1);
    Player *a = add_player(game, 1);
    Player *b = add_player(game, 2);
    CHECK(!airdrop_allowed(game) && !airdrop_streak_activate(game, a), "offline: no airdrops");
    game->is_local = 0;
    game->server = game; // a public server lobby
    CHECK(airdrop_allowed(game), "a public lobby");

    // tiers (seeded): Starter rarities 75 / 22 / 3; no streak package without mode.disabledStreaks
    pcg32_srandom(2024, 1);
    int rarity[7] = {0}, streak = 0, trapped = 0;
    for (int i = 0; i < 20000; i++) {
        int imitate = -1;
        const int p = airdrop_pick_package(game, 0, &imitate);
        rarity[g_airdrop_packages[p].rarity]++;
        streak += g_airdrop_packages[p].streak >= 0;
        if (g_airdrop_packages[p].imitate) trapped += imitate >= 0 && !g_airdrop_packages[imitate].imitate;
    }
    CHECK(abs(rarity[0] - 15000) < 400 && abs(rarity[1] - 4400) < 300 && abs(rarity[2] - 600) < 150 && !rarity[3], "Starter 75 / 22 / 3 (%d %d %d %d)", rarity[0], rarity[1], rarity[2], rarity[3]);
    CHECK(!streak && trapped > 0, "no streak packages in FFA; traps imitate a real package (%d)", trapped);
    for (int i = 0; i < 2000; i++) streak += g_airdrop_packages[airdrop_pick_package(game, 60, NULL)].streak >= 0;
    CHECK(!streak, "Heroic in FFA: still no streak packages");

    // the streak: a clear sky, 500 up, falls 0.05 u/ms to the floor
    place(a, 0.0f, -50.0f);
    reset_sent();
    CHECK(airdrop_streak_activate(game, a), "the Airdrop streak drops");
    AirDrop *d = &game->zombies.drops[0];
    CHECK(d->active && !d->can_claim && fabsf(d->position.y - 500.0f) < 1e-3f && d->fall > 400.0f && d->fall <= 1000.0f, "500 above the caller (fall %.1f)", d->fall);
    CHECK(g_count[NET_PACKET_AIRDROP] == 3, "\"ad\" to both players and the non-players (%d)", g_count[NET_PACKET_AIRDROP]);
    tick(game, 12.0f, 0.5f);
    CHECK(d->can_claim && d->fall == 0.0f, "landed (y %.1f)", d->position.y);

    // a trapped package: the owner sees it, the others see its disguise
    reset_sent();
    const int sid = airdrop_drop(game, a, (vec3) {200.0f, 0.0f, 0.0f}, 0.0f, 14);
    CHECK(sid > 0 && game->zombies.drops[sid - 1].can_claim, "a landed trap");
    NetAirDropPacket seen;
    memcpy(&seen, g_last[NET_PACKET_AIRDROP], sizeof(seen));
    CHECK(seen.index != 14 && g_last_uid[NET_PACKET_AIRDROP] == NET_TARGET_NON_PLAYERS, "others see the imitation (%d)", seen.index);

    // b claims a's package: hold interact on the ground for claimTime
    place(b, d->position.x, d->position.z + 10.0f);
    b->position.y = d->position.y - 5.0f;
    CHECK(airdrop_interact_prompt(game, b) == 0 && game_interact_prompt(game, b, NULL) == GAME_INTERACT_CLAIM, "the type 10 prompt");
    Input input;
    memset(&input, 0, sizeof(input));
    input.interact_held = 1;
    reset_sent();
    // player.js:1718-1726: interProg += dt / claimTime; the claim happens on the frame it reaches 1
    // (50 ms steps: float sums may need one more step, never one fewer)
    const int claim_frames = (int) ceilf(d->claim_time / 50.0f - 1e-3f);
    b->interact_progress = 0.0f;
    int frames = 0;
    for (; frames < 200 && d->active; frames++) airdrop_player_collide(b, &input, 0.05f);
    CHECK(d->claim_time > 0.0f && frames >= claim_frames && frames <= claim_frames + 1, "claimed after claimTime %.0f ms (%d x 50 ms, want %d)", d->claim_time, frames, claim_frames);
    CHECK(!d->active && g_count[NET_PACKET_AIRDROP_REMOVE] == 1 && g_count[NET_PACKET_AIRDROP_CLAIM] == 1, "claimed after the hold");
    NetAirDropClaimPacket krp;
    memcpy(&krp, g_last[NET_PACKET_AIRDROP_CLAIM], sizeof(krp));
    CHECK(krp.owner_uid == a->uid && krp.claimer_uid == b->uid, "krp: a thief");

    // the trap: Poisoned at max stacks
    AirDrop *trap = &game->zombies.drops[sid - 1];
    place(b, trap->position.x, trap->position.z);
    b->position.y = trap->position.y;
    for (int i = 0; i < 20 && trap->active; i++) airdrop_player_collide(b, &input, 0.1f);
    CHECK(!trap->active && b->effects[EFFECT_POISONED] == 1, "Trapped: Poisoned");

    // below deathY: lost
    const int lost = airdrop_drop(game, a, (vec3) {0.0f, game->map->death_y - 1.0f, 0.0f}, 0.0f, 0);
    tick(game, 0.1f, 0.1f);
    CHECK(!game->zombies.drops[lost - 1].active, "a drop below deathY is removed");

    // the client mirror
    Game client;
    memset(&client, 0, sizeof(client));
    int *modes = calloc(1, sizeof(int));
    modes[0] = MODE_ID_ZOM;
    const cJSON **maps = calloc(1, sizeof(cJSON *));
    maps[0] = g_map;
    game_configure(&client, NULL, maps, 1, modes, 1, NULL, 0, NULL, 0);
    game_init(&client, 0, 0, 0);
    CHECK(!game_is_authority(&client) && client.zombies.box_count == 2, "a client game");
    const int box = object_of_prefab(&client, PREFAB_MYSTERY_BOX, 1);
    const NetZombieMachinePacket mybx = {.kind = ZOMBIE_MACHINE_MYSTERY_BOX, .active = 1, .state = 1, .uid = box, .timer = 5000.0f, .pending_weapon = 3, .purchased_by = 9};
    CHECK(zombies_apply_packet(&client, NULL, NET_PACKET_ZOMBIE_MACHINE, &mybx, sizeof(mybx)) && client.map->objects[box]->active &&
          zombies_machine(&client, box)->state == 1 && zombies_machine(&client, box)->purchased_by == 9, "mybx mirrored");
    const NetAirDropPacket ad = {.sid = 3, .fall = 10.0f, .position = {1.0f, 20.0f, 2.0f}, .index = 5, .owner_uid = 1};
    zombies_apply_packet(&client, NULL, NET_PACKET_AIRDROP, &ad, sizeof(ad));
    CHECK(client.zombies.drop_count == 3 && client.zombies.drops[2].active && !client.zombies.drops[2].can_claim, "ad mirrored");
    zombies_tick(&client, 0.5f);
    CHECK(client.zombies.drops[2].can_claim && fabsf(client.zombies.drops[2].position.y - 10.0f) < 1e-3f, "the client fall");
    const NetPowerupPacket pwup = {.index = ZOMBIES_POWERUP_DOUBLE_POINTS, .ms = 1000};
    zombies_apply_packet(&client, NULL, NET_PACKET_POWERUP, &pwup, sizeof(pwup));
    CHECK(zombies_double_points(&client), "pwup mirrored");

    CHECK(NET_PACKET_ZOMBIE_MACHINE == 150 && NET_PACKET_SHARING == 155 && NET_PACKET_AIRDROP == 177 && NET_PACKET_AIRDROP_CLAIM == 179, "blocks 150-159 / 177-179");
    CHECK(sizeof(NetZombieMachinePacket) <= NET_MAX_PAYLOAD && sizeof(NetReserveAmmoPacket) <= NET_MAX_PAYLOAD && sizeof(NetAirDropPacket) <= NET_MAX_PAYLOAD, "packets fit");
    game->server = NULL;
    game->is_local = 1;
}

// ------------------------------------------------------------------ friendly fire / team damage

static void test_friendly_fire(Game *game) {
    printf("friendly fire...\n");
    start(game, MODE_ID_TDM, 1);
    Player *a = add_player(game, 1);
    Player *b = add_player(game, 2);
    Player *c = add_player(game, 3);
    Player *mate = b->team == a->team ? b : c;
    Player *foe = b->team == a->team ? c : b;
    mate->god_mode = foe->god_mode = 0;
    mate->health = foe->health = 100.0f;
    player_change_health(mate, a, 10.0f, 0);
    CHECK(mate->health == 100.0f, "no team damage");
    game->config.friendly_fire = 1;
    player_change_health(mate, a, 10.0f, 0);
    CHECK(mate->health == 90.0f, "frFire: team damage (%.1f)", mate->health);
    game->config.team_damage[a->team] = 0.5f;
    player_change_health(foe, a, 10.0f, 0);
    CHECK(foe->health == 95.0f, "t%dDmg 0.5 (%.1f)", a->team, foe->health);
    game->config.friendly_fire = 0;
    game->config.team_damage[a->team] = 1.0f;
}

int main(void) {
    cJSON *map = NULL;
    if (load_json_file("shared/tests/maps/v7_zombies.json", &map) && load_json_file("../shared/tests/maps/v7_zombies.json", &map)) {
        printf("Failed to load shared/tests/maps/v7_zombies.json (run from the repository root)\n");
        return 1;
    }
    g_map = map;
    Game game;
    memset(&game, 0, sizeof(game));
    test_waves(&game);
    test_drops(&game);
    test_powerups(&game);
    test_machines(&game);
    test_easter_egg(&game);
    test_finite_ammo(&game);
    test_airdrops(&game);
    test_friendly_fire(&game);

    if (g_failures) {
        printf("zombies_test: %d of %d checks failed\n", g_failures, g_checks);
        return 1;
    }
    printf("zombies_test: %d checks OK\n", g_checks);
    return 0;
}
