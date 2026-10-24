// Krunker v10.0.3 Defuse (shared/src/defuse.c, shared/include/defuse.h) on the official Kanji (two BOMB_SITEs,
// the Bomb's WEAPON_PICKUP for team 2) through the shared game hooks: taking the bomb, the plant / equip /
// defuse prompts, planting (2 s of M1 on the ground on a site), interruptions (letting go, leaving the ground,
// leaving the site), "Bomb Planted", the planted bomb's pickup, the beeps, the explosion and its win, defusing
// (4 s) and its win, a killed carrier's dropped bomb, a carrier's fall sending it home, the bomb counting as a
// living attacker; then a 4 v 4 bot game (bot_objective.c's Defuse) that plants. Run from the repository
// root: ./<build>/defuse_test
#include <shared.h>
#include <defuse.h>
#include <bots.h>
#include <bot_objective.h>
#include <map_catalog.h>
#include <pcg_basic.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_SENT 8192

typedef struct {
    int uid;
    int type;
    uint16_t length;
    uint8_t payload[NET_MAX_PAYLOAD];
} SentPacket;

static SentPacket g_sent[MAX_SENT];
static int g_sent_count;
static int g_failures;
static int g_checks;
static const cJSON *g_kanji;

#define CHECK(cond, ...) do { \
    g_checks++; \
    if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_failures++; } \
} while (0)

static void test_send(Game *game, const int uid, const int type, const void *payload, const uint16_t length) {
    (void) game;
    if (g_sent_count >= MAX_SENT) return;
    SentPacket *packet = &g_sent[g_sent_count++];
    packet->uid = uid;
    packet->type = type;
    packet->length = length;
    memcpy(packet->payload, payload, length);
}

static void test_respawn(Game *game, const int uid) {
    Player *player = game_find_player(game, uid);
    if (player) player_spawn(player);
}

static const SentPacket *last_sent(const int type) {
    for (int i = g_sent_count - 1; i >= 0; i--) {
        if (g_sent[i].type == type) return &g_sent[i];
    }
    return NULL;
}

static int count_sound(const char *name, int *uid, vec3 *at) {
    int n = 0;
    for (int i = 0; i < g_sent_count; i++) {
        if (g_sent[i].type != NET_PACKET_SOUND) continue;
        const NetSoundPacket *s = (const NetSoundPacket *) g_sent[i].payload;
        if (strcmp(s->name, name)) continue;
        n++;
        if (uid) *uid = s->uid;
        if (at) *at = (vec3) {s->position[0], s->position[1], s->position[2]};
    }
    return n;
}

static int sent_text(const int type, const char *text) {
    for (int i = 0; i < g_sent_count; i++) {
        if (g_sent[i].type == type && !strcmp((const char *) g_sent[i].payload, text)) return 1; // text is the first field
    }
    return 0;
}

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

static void start(Game *game) {
    game_bots_remove_all(game);
    free_players(game);
    int *modes = calloc(1, sizeof(int));
    modes[0] = MODE_ID_DEF;
    const cJSON **maps = calloc(1, sizeof(cJSON *)); // game_configure owns the lists
    maps[0] = g_kanji;
    game_configure(game, NULL, maps, 1, modes, 1, NULL, 0, NULL, 0);
    game->hooks.send = test_send;
    game->hooks.respawn = test_respawn;
    game->round_length = 600.0f;
    g_sent_count = 0;
    game_init(game, 0, 0, 1);
    game->now = 0.0f;
    game->is_sandbox = 1; // the win rules (a public match has no winner: mode_public_no_winner)
}

static Player *add_player(Game *game, const int uid) {
    Player *player = player_init(game);
    player->uid = uid;
    player->class_index = 0;
    snprintf(player->name, sizeof(player->name), "P%d", uid);
    game_players_add(game, player);
    player_spawn(player);
    return player;
}

static Player *on_team(const Game *game, const int team, const int nth) {
    int seen = 0;
    for (size_t i = 0; i < game->player_count; i++) {
        if (game->players[i]->team == team && seen++ == nth) return game->players[i];
    }
    return NULL;
}

static void tick(Game *game, const float seconds, const float step) {
    for (float t = 0.0f; t < seconds - 1e-4f; t += step) game_tick(game, game->now + step, step);
}

static const Input k_idle = {.delta = 0.025f, .move_dir = -1};
static const Input k_hold = {.delta = 0.025f, .move_dir = -1, .shoot = 1};

static void frames(Player *p, const Input *input, const int count) {
    for (int i = 0; i < count; i++) player_proc_input(p, input, 0, 0);
}

// Standing on the floor at x, z (dropped from y + 2, a few idle frames)
static void stand(Player *p, const vec3 at) {
    p->position = (vec3) {at.x, at.y + 2.0f, at.z};
    p->last_position = p->position;
    p->velocity = (vec3) {0.0f, 0.0f, 0.0f};
    frames(p, &k_idle, 12);
}

static int bomb_pickup(const Game *game) {
    for (size_t i = 0; i < game->map->object_count; i++) {
        const Object *o = game->map->objects[i];
        if (o->pickup && cJSON_GetNumberValue(cJSON_GetObjectItem(o->raw, "w")) == DEFUSE_BOMB_WEAPON) return (int) i;
    }
    return -1;
}

static const Object *site(const Game *game, const int nth) {
    int seen = 0;
    for (size_t i = 0; i < game->map->object_count; i++) {
        if (game->map->objects[i]->bomb_site && seen++ == nth) return game->map->objects[i];
    }
    return NULL;
}

static int holds(const Player *p, const int weapon) {
    for (int i = 0; p->loadout && i < p->loadout_size; i++) {
        if (p->loadout[i] == weapon) return 1;
    }
    return 0;
}

static float d2(const vec3 a, const float x, const float z) { return hypotf(a.x - x, a.z - z); }

// The attacker takes the bomb at its pickup ("int" 6).
static void take_bomb(Game *game, Player *p) {
    const int uid = bomb_pickup(game);
    GamePickup k;
    game_pickup_state(game, uid, &k);
    stand(p, k.position);
    game->now += 1.0f; // config.interactTimer
    game_player_interact_v7(game, p, GAME_INTERACT_PICKUP, uid, 0);
}

static void equip_bomb(Player *p) {
    const Input swap = {.delta = 0.025f, .move_dir = -1, .swap = 4}; // swapEquipment
    frames(p, &swap, 1);
    frames(p, &k_idle, 12); // swapTime 250 ms
}

// A round past "match starting in" (10 s) with two players a side.
static void start_round(Game *game, Player **attacker, Player **attacker2, Player **defender, Player **defender2) {
    start(game);
    for (int uid = 1; uid <= 4; uid++) add_player(game, uid);
    tick(game, 10.5f, 0.5f);
    *attacker = on_team(game, DEFUSE_ATTACKERS, 0);
    *attacker2 = on_team(game, DEFUSE_ATTACKERS, 1);
    *defender = on_team(game, DEFUSE_DEFENDERS, 0);
    *defender2 = on_team(game, DEFUSE_DEFENDERS, 1);
}

// ------------------------------------------------------------------ tests

static void test_map(Game *game) {
    printf("Running Kanji Defuse setup test...\n");
    start(game);
    CHECK(game->mode && game->mode->config.site && defuse_enabled(game), "Defuse on Kanji: site mode with bomb sites");
    CHECK(site(game, 0) && site(game, 1) && !site(game, 2), "two sites");
    const int uid = bomb_pickup(game);
    GamePickup k;
    CHECK(uid >= 0 && game_pickup_state(game, uid, &k) && k.weapon == DEFUSE_BOMB_WEAPON && k.active && k.team == DEFUSE_ATTACKERS && k.replace,
          "the bomb's pickup: weapon 17, active in a site mode, tm 2, pickupRep");
    CHECK(fabsf(defuse_beep_interval(39.0f) - 2.0f) < 1e-6f && fabsf(defuse_beep_interval(20.0f) - 1.0f) < 1e-6f &&
          fabsf(defuse_beep_interval(10.0f) - 0.5f) < 1e-6f && fabsf(defuse_beep_interval(5.0f) - 0.2f) < 1e-6f &&
          fabsf(defuse_beep_interval(2.0f) - 0.15f) < 1e-6f, "updateBomb's beat: 2 / 1 / 0.5 / 0.2 / 0.15 s");
    // FFA: the bomb pickup stays out of play
    int *modes = calloc(1, sizeof(int));
    modes[0] = MODE_ID_FFA;
    const cJSON **maps = calloc(1, sizeof(cJSON *));
    maps[0] = g_kanji;
    game_configure(game, NULL, maps, 1, modes, 1, NULL, 0, NULL, 0);
    game->hooks.send = test_send;
    game_init(game, 0, 0, 1);
    CHECK(game_pickup_state(game, uid, &k) && !k.active && !defuse_enabled(game), "outside a site mode the bomb pickup is inactive");
}

static void test_plant(Game *game) {
    printf("Running plant test...\n");
    Player *a, *a2, *d, *d2_;
    start_round(game, &a, &a2, &d, &d2_);
    CHECK(a && a2 && d && d2_ && !game->move_lock, "two a side, unlocked");
    const int uid = bomb_pickup(game);
    GameInteractScan scan;
    take_bomb(game, a);
    GamePickup k;
    game_pickup_state(game, uid, &k);
    CHECK(holds(a, DEFUSE_BOMB_WEAPON) && k.weapon < 0 && a->last_picked[DEFUSE_BOMB_WEAPON] == uid + 1, "the attacker took the bomb (pickup emptied)");
    CHECK(a->weapon && a->weapon->bomb, "a picked weapon comes in hand");
    // a defender cannot take it (tm 2)
    game_pickup_set(game, uid, DEFUSE_BOMB_WEAPON, 1, NULL, -1);
    take_bomb(game, d);
    CHECK(!holds(d, DEFUSE_BOMB_WEAPON), "a defender cannot take the bomb");
    game_pickup_set(game, uid, -1, 1, NULL, -1);

    const Object *s = site(game, 0);
    stand(a, s->position);
    CHECK(a->on_ground && defuse_site_at(game, a) >= 0, "the carrier stands on site A");
    game_interact_scan(game, a, &scan);
    CHECK(scan.type == GAME_INTERACT_BOMB && scan.data == DEFUSE_PROMPT_PLANT && scan.last_type == GAME_INTERACT_NONE,
          "Plant prompt (3 / 1), not the interact key's (%d %d %d)", scan.type, scan.data, scan.last_type);
    const Input primary = {.delta = 0.025f, .move_dir = -1, .swap = 3};
    frames(a, &primary, 1);
    frames(a, &k_idle, 12);
    CHECK(a->loadout_index == 0 && !a->weapon->bomb, "the primary in hand");
    game_interact_scan(game, a, &scan);
    CHECK(scan.type == GAME_INTERACT_BOMB && scan.data == DEFUSE_PROMPT_EQUIP, "bomb in another slot: Equip Bomb (3 / 2: %d %d)", scan.type, scan.data);
    frames(a, &k_hold, 40);
    CHECK(a->interact_progress == 0.0f && !game->mode->bomb.active, "M1 without the bomb in hand plants nothing");
    stand(a, s->position);
    equip_bomb(a);
    CHECK(a->weapon && a->weapon->bomb, "the equip key: the bomb in hand");
    game_interact_scan(game, a, &scan);
    CHECK(scan.type == GAME_INTERACT_BOMB && scan.data == DEFUSE_PROMPT_PLANT, "Plant prompt (3 / 1: %d %d)", scan.type, scan.data);

    // interruptions: letting go, leaving the ground, leaving the site
    frames(a, &k_hold, 40); // 1 s
    CHECK(fabsf(a->interact_progress - 0.5f) < 0.02f && a->doing_interact && a->making_progress, "1 of 2 s held (%.3f)", a->interact_progress);
    game_interact_scan(game, a, &scan);
    CHECK(scan.type == GAME_INTERACT_NONE, "no prompt while planting (%d)", scan.type);
    frames(a, &k_idle, 1);
    CHECK(a->interact_progress == 0.0f && !a->doing_interact, "letting go restarts the plant (%.3f)", a->interact_progress);
    frames(a, &k_hold, 60);
    CHECK(fabsf(a->interact_progress - 0.75f) < 0.02f, "1.5 s held (%.3f)", a->interact_progress);
    const Input jump = {.delta = 0.025f, .move_dir = -1, .shoot = 1, .jump = 1};
    frames(a, &jump, 1);
    frames(a, &k_hold, 2);
    CHECK(!a->on_ground && a->interact_progress == 0.0f, "a jump restarts it (%d %.3f)", a->on_ground, a->interact_progress);
    stand(a, s->position);
    frames(a, &k_hold, 40);
    stand(a, (vec3) {s->position.x + 60.0f, s->position.y, s->position.z});
    frames(a, &k_hold, 1);
    CHECK(a->interact_progress == 0.0f && !game->mode->bomb.active && defuse_site_at(game, a) < 0, "leaving the site restarts it (%.3f)", a->interact_progress);
    CHECK(!game->mode->bomb.active && holds(a, DEFUSE_BOMB_WEAPON), "nothing planted yet");

    // a full plant
    stand(a, s->position);
    const int score = a->score;
    g_sent_count = 0;
    frames(a, &k_hold, 81); // 2 s (+ a frame for the float sum)
    ModeBomb *bomb = &game->mode->bomb;
    CHECK(bomb->active && bomb->team == DEFUSE_ATTACKERS && fabsf(bomb->timer - DEFUSE_BOMB_TIME) < 1e-4f && fabsf(bomb->tick - DEFUSE_BOMB_TICK) < 1e-4f,
          "planted: activeBomb {team 2, 40 s, tick 2 s}");
    CHECK(!holds(a, DEFUSE_BOMB_WEAPON) && a->loadout_index == 0 && a->last_picked[DEFUSE_BOMB_WEAPON] == 0, "the bomb left the planter's loadout");
    CHECK(game_pickup_state(game, uid, &k) && k.weapon == DEFUSE_BOMB_WEAPON && !k.active && d2(k.position, a->position.x, a->position.z) <= 1.5f &&
          fabsf(k.position.y - (a->position.y + 1.0f)) < 0.01f, "the bomb's pickup: inactive, where the planter stands (y + 1)");
    CHECK(d2(bomb->position, k.position.x, k.position.z) < 1e-4f && fabsf(bomb->position.y - k.position.y) < 1e-4f, "activeBomb x, y, z = the pickup's");
    const SentPacket *medal = last_sent(NET_PACKET_MEDAL);
    CHECK(medal && medal->uid == a->uid && !strcmp(((const NetMedalPacket *) medal->payload)->text, "Bomb Planted") &&
          ((const NetMedalPacket *) medal->payload)->score == DEFUSE_PLANT_SCORE, "\"Bomb Planted\" +200 to the planter");
    CHECK(a->score >= score + DEFUSE_PLANT_SCORE, "the planter scores 200 (%d -> %d)", score, a->score);
    CHECK(sent_text(NET_PACKET_GAME_MESSAGE, "BOMB PLANTED"), "gmsg BOMB PLANTED");
    int sound_uid = 0;
    CHECK(count_sound("bo_plant", &sound_uid, NULL) == 1 && sound_uid == -2, "bo_plant, 2D");
    const SentPacket *pk = last_sent(NET_PACKET_PICKUP);
    CHECK(pk && ((const NetPickupPacket *) pk->payload)->uid == uid && !((const NetPickupPacket *) pk->payload)->active &&
          ((const NetPickupPacket *) pk->payload)->moved, "everyone hears of the inactive, moved pickup");
    frames(a, &k_hold, 40);
    CHECK(!bomb->active || bomb->timer == DEFUSE_BOMB_TIME, "holding on: no second plant");

    // the attackers' scan: no pickup prompt for an inactive bomb; a defender's: Diffuse
    stand(d, k.position);
    game_interact_scan(game, d, &scan);
    CHECK(scan.type == GAME_INTERACT_BOMB && scan.data == DEFUSE_PROMPT_DEFUSE && defuse_bomb_pickup_at(game, d) == uid, "a defender at the bomb: Diffuse (3 / 3)");
    stand(a2, k.position);
    game_interact_scan(game, a2, &scan);
    CHECK(scan.type != GAME_INTERACT_PICKUP && scan.type != GAME_INTERACT_BOMB, "an attacker at the planted bomb: nothing to take (%d)", scan.type);

    // beeps: 3D at the bomb, faster to the end
    g_sent_count = 0;
    tick(game, 10.5f, 0.05f);
    vec3 at;
    const int beeps = count_sound("beep_0", &sound_uid, &at);
    CHECK(beeps == 5 && sound_uid == -1 && d2(at, roundf(bomb->position.x), roundf(bomb->position.z)) < 1e-3f, "10 s: 5 beeps at the bomb (%d)", beeps);
}

static void test_defuse(Game *game) {
    printf("Running defuse test...\n");
    Player *a, *a2, *d, *d2_;
    start_round(game, &a, &a2, &d, &d2_);
    take_bomb(game, a); // in hand
    stand(a, site(game, 1)->position);
    frames(a, &k_hold, 81);
    CHECK(game->mode->bomb.active, "planted on site B");
    GamePickup k;
    const int uid = bomb_pickup(game);
    game_pickup_state(game, uid, &k);
    // an attacker cannot defuse; a defender's hold, interrupted, then whole
    stand(a2, k.position);
    frames(a2, &k_hold, 200);
    CHECK(game->mode->bomb.active && a2->interact_progress == 0.0f, "an attacker's M1 at the bomb does nothing");
    stand(d, k.position);
    frames(d, &k_hold, 80); // 2 of 4 s
    CHECK(fabsf(d->interact_progress - 0.5f) < 0.02f && game->mode->bomb.active, "2 of 4 s held (%.3f)", d->interact_progress);
    frames(d, &k_idle, 1);
    CHECK(d->interact_progress == 0.0f, "letting go restarts the defuse");
    stand(d, (vec3) {k.position.x + 40.0f, k.position.y, k.position.z});
    frames(d, &k_hold, 200);
    CHECK(game->mode->bomb.active && d->interact_progress == 0.0f, "away from the bomb: nothing");
    stand(d, k.position);
    g_sent_count = 0;
    frames(d, &k_hold, 161); // 4 s
    CHECK(!game->mode->bomb.active && d->mode.diffuses == 1, "defused: activeBomb null, diffuses 1");
    int sound_uid = 0;
    CHECK(count_sound("bo_diff", &sound_uid, NULL) == 1 && sound_uid == -2, "bo_diff, 2D");
    tick(game, 0.1f, 0.05f);
    CHECK(game->end_timer > 0.0f, "the round ends");
    const SentPacket *end = last_sent(NET_PACKET_MODE_END);
    CHECK(end && ((const NetModeEndPacket *) end->payload)->winner_team == DEFUSE_DEFENDERS, "the defenders win (%d)",
          end ? ((const NetModeEndPacket *) end->payload)->winner_team : -9);
}

static void test_explosion(Game *game) {
    printf("Running explosion test...\n");
    Player *a, *a2, *d, *d2_;
    start_round(game, &a, &a2, &d, &d2_);
    take_bomb(game, a);
    stand(a, site(game, 0)->position);
    frames(a, &k_hold, 81);
    CHECK(game->mode->bomb.active, "planted");
    // every attacker dies: the planted bomb keeps the round going (def.condition counts it)
    PlayerKillInfo info = {.weapon_id = 0};
    player_kill(a, d, &info, 0);
    player_kill(a2, d, &info, 0);
    tick(game, 1.0f, 0.05f);
    CHECK(game->end_timer <= 0.0f && game->mode->bomb.active, "no attacker left, the bomb counts as one: the round goes on");
    g_sent_count = 0;
    tick(game, 38.0f, 0.05f);
    CHECK(d->active && d2_->active && game->mode->bomb.active, "39 s: the defenders still stand");
    tick(game, 1.5f, 0.05f);
    CHECK(!d->active && !d2_->active && !game->mode->bomb.active, "40 s: the bomb killed every defender");
    const SentPacket *nuke = last_sent(NET_PACKET_NUKE);
    CHECK(nuke && ((const NetNukePacket *) nuke->payload)->state == 1, "\"n\" 1: the flash");
    int kills = 0, no_killer = 1;
    for (int i = 0; i < g_sent_count; i++) {
        if (g_sent[i].type != NET_PACKET_KILL) continue;
        const NetKillPacket *kp = (const NetKillPacket *) g_sent[i].payload;
        kills++;
        no_killer &= kp->killer_uid == -1 && kp->weapon_index == -1;
    }
    CHECK(kills == 2 && no_killer, "two deaths without a killer (%d)", kills);
    const int beeps = count_sound("beep_0", NULL, NULL);
    CHECK(beeps >= 45 && beeps <= 60, "the beeps sped up (%d)", beeps);
    tick(game, 0.2f, 0.05f);
    const SentPacket *end = last_sent(NET_PACKET_MODE_END);
    CHECK(game->end_timer > 0.0f && end && ((const NetModeEndPacket *) end->payload)->winner_team == DEFUSE_ATTACKERS, "the attackers win (%d)",
          end ? ((const NetModeEndPacket *) end->payload)->winner_team : -9);
}

static void test_carrier(Game *game) {
    printf("Running carrier death test...\n");
    Player *a, *a2, *d, *d2_;
    start_round(game, &a, &a2, &d, &d2_);
    const int uid = bomb_pickup(game);
    GamePickup k, home;
    game_pickup_state(game, uid, &home);
    take_bomb(game, a);
    // killed by a defender: the bomb lies where the carrier died, active
    stand(a, site(game, 0)->position);
    const vec3 died = a->position;
    PlayerKillInfo info = {.weapon_id = 0};
    player_kill(a, d, &info, 0);
    CHECK(game_pickup_state(game, uid, &k) && k.weapon == DEFUSE_BOMB_WEAPON && k.active && d2(k.position, died.x, died.z) <= 1.5f,
          "a killed carrier drops the bomb where it died (active)");
    CHECK(!holds(a, DEFUSE_BOMB_WEAPON), "the dead carrier no longer has it");
    // the other attacker takes it there, then falls out of the map: back to its pickup's origin
    stand(a2, k.position);
    game->now += 1.0f;
    game_player_interact_v7(game, a2, GAME_INTERACT_PICKUP, uid, 0);
    CHECK(holds(a2, DEFUSE_BOMB_WEAPON) && game_pickup_state(game, uid, &k) && k.weapon < 0, "the second attacker took it");
    player_kill(a2, NULL, NULL, 0);
    CHECK(game_pickup_state(game, uid, &k) && k.weapon == DEFUSE_BOMB_WEAPON && k.active && d2(k.position, home.origin.x, home.origin.z) < 1e-4f,
          "a carrier dying without a killer sends the bomb home");

    // KrunkNative difference: a type-3 secondary (the Boulder) is not swapped for the bomb (v10 would leave it in
    // the bomb's pickup with no lastPicked, and plantBomb would run every tick)
    Player *b, *b2, *e, *e2;
    start_round(game, &b, &b2, &e, &e2);
    const int boulder = 29;
    CHECK(game->weapons[boulder] && game->weapons[boulder]->type == game->weapons[DEFUSE_BOMB_WEAPON]->type, "the Boulder is type 3 too");
    const int ids[] = {0, boulder, WEAPON_ID_KNIFE};
    mode_set_player_loadout(b, ids, 3, 0);
    take_bomb(game, b);
    CHECK(holds(b, boulder) && holds(b, DEFUSE_BOMB_WEAPON) && b->last_picked[DEFUSE_BOMB_WEAPON] == uid + 1 && game_pickup_state(game, uid, &k) &&
          k.weapon < 0, "the bomb joins a Boulder loadout (the pickup emptied, lastPicked set)");
    stand(b, site(game, 0)->position);
    g_sent_count = 0;
    frames(b, &k_hold, 200);
    int medals = 0;
    for (int i = 0; i < g_sent_count; i++) medals += g_sent[i].type == NET_PACKET_MEDAL;
    CHECK(game->mode->bomb.active && medals == 1 && !holds(b, DEFUSE_BOMB_WEAPON) && holds(b, boulder), "one plant, the Boulder kept (%d medals)", medals);
}

// ------------------------------------------------------------------ bots

static int g_plants, g_defuses, g_blasts, g_winner;

static void bot_send(Game *game, const int uid, const int type, const void *payload, const uint16_t length) {
    (void) game;
    (void) uid;
    (void) length;
    if (type == NET_PACKET_SOUND) {
        const NetSoundPacket *s = payload;
        g_plants += !strcmp(s->name, "bo_plant");
        g_defuses += !strcmp(s->name, "bo_diff");
    }
    if (type == NET_PACKET_NUKE) g_blasts += ((const NetNukePacket *) payload)->state == 1;
    if (type == NET_PACKET_MODE_END) g_winner = ((const NetModeEndPacket *) payload)->winner_team;
}

// 4 v 4 normal NextBots: an attacker takes the bomb, carries it to a site and plants it; the round ends
// on the bomb, a defuse or the elimination. A few rounds, at least one plant.
static void test_bots(Game *game) {
    printf("Running bot game test...\n");
    int rounds = 0, picked = 0, ends = 0, plants = 0;
    for (int r = 0; r < 4; r++) {
        start(game);
        game->hooks.send = bot_send;
        game->round_length = 150.0f; // the clock ends a round nobody wins sooner (the defenders' then)
        game_init(game, 0, 0, 1);
        game->is_sandbox = 1;
        for (int team = 1; team <= 2; team++) {
            for (int i = 0; i < 4; i++) {
                char team_arg[16];
                snprintf(team_arg, sizeof(team_arg), "team=%d", team);
                char *argv[] = {team_arg, "difficulty=normal"};
                BotAssignments as;
                bot_parse_assignments(2, argv, &as);
                CHECK(!as.error[0], "bot options: %s", as.error);
                CHECK(game_bots_add(game, &as, NULL, 0.0f) != NULL, "bot added");
            }
        }
        rounds++;
        g_plants = g_defuses = g_blasts = 0;
        g_winner = -9;
        int had_bomb = 0, t = 0;
        for (; t < 30 * 180 && game->end_timer <= 0.0f; t++) {
            game_tick(game, game->now + 1.0f / 30.0f, 1.0f / 30.0f);
            for (size_t i = 0; i < game->player_count && !had_bomb; i++) had_bomb |= game->players[i]->active && holds(game->players[i], DEFUSE_BOMB_WEAPON);
            if (getenv("KN_DEFUSE_TRACE") && t % 150 == 0) {
                for (int b = 0; b < game->bots->count; b++) {
                    const GameBot *gb = &game->bots->list[b];
                    const Player *p = gb->player;
                    if (!p || !p->active || !holds(p, DEFUSE_BOMB_WEAPON)) continue;
                    printf("    t %.1f carrier %d at %.0f %.0f %.0f goal %s %.0f %.0f %.0f arrived %d site %d hand %d ground %d state %s prog %.2f\n",
                           t / 30.0f, p->uid, p->position.x, p->position.y, p->position.z, bot_goal_name(gb->objective.kind), gb->objective.goal.x,
                           gb->objective.goal.y, gb->objective.goal.z, gb->objective.arrived, defuse_site_at(game, p), p->weapon && p->weapon->bomb,
                           p->on_ground, bot_state_name(gb->state), p->interact_progress);
                }
            }
        }
        picked += had_bomb;
        plants += g_plants;
        ends += game->end_timer > 0.0f;
        printf("  round %d (%.0f s): bomb taken %d, planted %d, defused %d, exploded %d, winner team %d\n", r + 1, t / 30.0f, had_bomb, g_plants,
               g_defuses, g_blasts, g_winner);
        CHECK(g_plants <= 1 && g_defuses + g_blasts <= g_plants, "one plant a round, ending in a defuse or a blast at most");
        if (g_defuses) CHECK(g_winner == DEFUSE_DEFENDERS, "a defuse wins it for the defenders (%d)", g_winner);
        if (g_blasts) CHECK(g_winner == DEFUSE_ATTACKERS, "the blast wins it for the attackers (%d)", g_winner);
    }
    CHECK(picked >= rounds - 1, "the attackers took the bomb (%d of %d rounds)", picked, rounds);
    CHECK(plants >= 1, "the bots planted (%d plants in %d rounds)", plants, rounds);
    CHECK(ends == rounds, "every round ended (%d of %d)", ends, rounds);
    game_bots_remove_all(game);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const int key = map_catalog_index_of_key("kanji");
    g_kanji = key >= 0 ? map_catalog_json((size_t) key) : NULL;
    if (!g_kanji) {
        printf("Failed to load Kanji (run from the repository root)\n");
        return 1;
    }
    if (getenv("KN_BOT_SEED")) pcg32_srandom((uint64_t) atoll(getenv("KN_BOT_SEED")), 54u);
    static Game game;
    test_map(&game);
    test_plant(&game);
    test_defuse(&game);
    test_explosion(&game);
    test_carrier(&game);
    test_bots(&game);
    game_bots_remove_all(&game);
    free_players(&game);
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
