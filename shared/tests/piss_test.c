// KrunkNative Piss (shared/include/piss.h, doc/v7/decisions.md "Piss") through procInputs frames
// (player_proc_input) on an authority game, the way server_bin and the offline sandbox run it:
//   the stream (55 ms rate, 4 a hit) damages the player it reaches and leaves no puddle; a stream
//   on the floor leaves one puddle where the arc lands (later shots at the same spot refresh it);
//   a stream on a wall leaves it at the wall's foot; a player standing in a puddle takes
//   18 per second in 250 ms ticks (not its owner, not 2.8 above it); the puddle goes after 5 s;
//   24 puddles per owner, 64 in the game; an owner leaving takes theirs; SPAWNED_OBJ
//   (OM_KIND_PUDDLE) ADD / REMOVE rebuild the puddles on a client and a joiner gets the live ones.
// Run from the repository root: ./<build>/piss_test
#include <shared.h>
#include <object_manager.h>
#include <piss.h>
#include <cJSON.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <ruleset.h>

static int g_failures, g_checks;

#define CHECK(cond, ...) do { g_checks++; \
    if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_failures++; } \
} while (0)

#define DT 0.01f
#define WALL_FACE_Z -60.0f

static int g_seq;
static int g_adds, g_removes, g_shots, g_stream_shots;
static Game *g_client; // receives every SPAWNED_OBJ the authority sends (NULL: none)
static int g_joiner_adds;

static void test_send(Game *game, const int uid, const int type, const void *payload, const uint16_t length) {
    (void) game;
    if (type == NET_PACKET_SHOT && length == sizeof(NetShotPacket)) {
        g_shots++;
        if (((const NetShotPacket *) payload)->weapon_index == WEAPON_ID_PISS) g_stream_shots++;
    }
    if (type != NET_PACKET_SPAWNED_OBJ || length < sizeof(NetSpawnedObjPacket)) return;
    NetSpawnedObjPacket head;
    memcpy(&head, payload, sizeof(head));
    if (head.kind != OM_KIND_PUDDLE) return;
    if (uid == 77) { // the joiner
        g_joiner_adds += head.op == OM_SPAWNED_ADD;
        return;
    }
    g_adds += head.op == OM_SPAWNED_ADD;
    g_removes += head.op == OM_SPAWNED_REMOVE;
    if (g_client) object_manager_apply_spawned(g_client, payload, length);
}

static Player *add_player(Game *game, const int uid) {
    Player *player = player_init(game);
    player->uid = uid;
    player->class_index = 0;
    snprintf(player->name, sizeof(player->name), "p%d", uid);
    game_players_add(game, player);
    player_spawn(player);
    player->max_health = 100000;
    player->health = 100000.0f;
    return player;
}

static void give_piss(Player *player) {
    const int ids[1] = {WEAPON_ID_PISS};
    player_set_extra_weapons(player, ids, 1);
    for (int i = 0; i < player->loadout_size; i++) {
        if (player->loadout[i] != WEAPON_ID_PISS) continue;
        player_swap_weapon(player, i, 1, 1, 0);
        player->swap_timer = 0.0f;
        player->reloads[i] = 0.0f;
    }
}

static void stand(Player *player, const float x, const float z, const float pitch) {
    player->position = (vec3) {x, 0.0f, z};
    player->last_position = player->position;
    player->velocity = (vec3) {0.0f, 0.0f, 0.0f};
    player->direction = (vec2) {pitch, 0.0f}; // yaw 0: facing -z
    player->on_ground = 1;
    player->recoil_anim_y = 0.0f; // the last test's kick
    player->recoil_force = 0.0f;
}

// `frames` 10 ms procInputs frames holding fire (or not); the shots fired
static int hold(Player *player, const int frames, const int shoot) {
    int shots = 0;
    for (int f = 0; f < frames; f++) {
        const int slot = player->loadout_index;
        const float before = player->reloads[slot];
        const Input input = {.seq = ++g_seq, .delta = DT, .move_dir = -1, .shoot = (unsigned char) shoot,
                             .x_dir = player->direction.x, .y_dir = player->direction.y};
        player_proc_input(player, &input, 0, 0);
        if (player->reloads[slot] > before + 1e-6f) shots++;
    }
    return shots;
}

// where the arc from the hose meets y 0 (the floor's top)
static vec3 floor_landing(const Player *p, const float pitch) {
    const WeaponStream *s = rules_weapon(WEAPON_ID_PISS)->stream;
    const vec3 src = piss_stream_source(p, s, pitch, 0.0f);
    const vec3 v = {0.0f, sinf(pitch) * s->speed, -cosf(pitch) * s->speed};
    const float a = 0.5f * s->gravity, b = -v.y, c = -src.y;
    const float t = (-b + sqrtf(b * b - 4.0f * a * c)) / (2.0f * a);
    return piss_stream_at(src, v, s->gravity, t);
}

static int puddle_index_alive(const Game *game, const int id) {
    for (int i = 0; i < game->puddles.count; i++)
        if (game->puddles.items[i].id == id) return 1;
    return 0;
}

static const GamePuddle *puddle_of(const Game *game, const int owner) {
    for (int i = game->puddles.count - 1; i >= 0; i--)
        if (game->puddles.items[i].owner == owner) return &game->puddles.items[i];
    return NULL;
}

static void test_weapon(void) {
    printf("Weapon table: the Piss after v7's weapons...\n");
    const Weapon *w = rules_weapon(WEAPON_ID_PISS);
    CHECK(WEAPON_ID_PISS >= WEAPON_COUNT && WEAPON_ID_PISS < WEAPON_TABLE_COUNT, "a KrunkNative id after v7's %d", WEAPON_COUNT);
    CHECK(w && !strcmp(w->name, "Piss") && w->stream, "rules_weapon(%d) is the Piss with a stream", WEAPON_ID_PISS);
    CHECK(w && fabsf(w->rate - 0.055f) < 1e-6f && w->ammo == 999 && fabsf(w->damage - 4.0f) < 1e-6f, "rate 55 ms, 999 rounds, 4 a hit");
    CHECK(w && w->stream->puddle_dps == 18.0f && w->stream->puddle_tick == 250.0f && w->stream->puddle_life == 5000.0f &&
          w->stream->puddle_radius == 3.2f && w->stream->puddles_max == 64, "puddles: 18 dps / 250 ms, 5 s, radius 3.2, 64 a game");
    CHECK(object_manager_weapon_listed(WEAPON_ID_PISS), "an Object Manager Weapons tile");
    int in_loadout = 0;
    for (int c = 0; c < CLASS_COUNT; c++) in_loadout |= rules_classes()[c].loadout && rules_classes()[c].loadout[0] == WEAPON_ID_PISS;
    CHECK(!in_loadout, "in no class loadout");
}

static void test_stream_hit(Game *game, Player *p, Player *target) {
    printf("Stream: a player in the way takes 4 a shot, no puddle...\n");
    stand(p, 0.0f, 0.0f, 0.0f);
    stand(target, 0.0f, -15.0f, 0.0f);
    for (int i = 0; i < 20; i++) player_store_state(target);
    piss_round_start(game);
    g_stream_shots = 0;
    const float health = target->health;
    const int shots = hold(p, 100, 1); // 1 s
    const float dealt = health - target->health;
    printf("  %d shots in 1 s, %.1f damage, %d SHOT packets, %d puddles\n", shots, dealt, g_stream_shots, game->puddles.count);
    // 55 ms on 10 ms frames: a shot every 6th frame (the timer fires once it is <= 0), 16-17 a second
    CHECK(shots >= 16 && shots <= 19, "fires every 55 ms (%d shots in a second)", shots);
    CHECK(fabsf(dealt - 4.0f * (float) shots) < 1e-3f, "4 damage a shot (%.1f for %d)", dealt, shots);
    CHECK(g_stream_shots == shots, "the others see each shot (%d SHOT)", g_stream_shots);
    CHECK(game->puddles.count == 0, "a player hit leaves no puddle");
    hold(p, 20, 0);
}

static void test_floor(Game *game, Player *p, Player *target) {
    printf("Stream on the floor: a puddle where it lands...\n");
    stand(target, 300.0f, 300.0f, 0.0f);
    const float pitch = -0.4f;
    stand(p, 0.0f, 0.0f, pitch);
    piss_round_start(game);
    const int adds = g_adds;
    PissStream trace;
    CHECK(player_stream_trace(p, pitch, 0.0f, 1, &trace) && trace.hit == PISS_HIT_OBJECT && trace.puddle, "the stream lands on the floor");
    const int shots = hold(p, 6, 1);
    const vec3 expect = floor_landing(p, pitch);
    const GamePuddle *puddle = puddle_of(game, p->uid);
    CHECK(shots >= 1 && puddle != NULL, "a puddle (%d shots)", shots);
    if (puddle) {
        printf("  puddle at %.2f %.2f %.2f (landing %.2f %.2f %.2f), radius %.1f\n", puddle->position.x, puddle->position.y, puddle->position.z,
               expect.x, expect.y, expect.z, puddle->radius);
        CHECK(fabsf(puddle->position.x - expect.x) < 0.2f && fabsf(puddle->position.z - (expect.z + 0.3f)) < 0.2f,
              "at the landing spot (a step back along the stream)");
        CHECK(fabsf(puddle->position.y - PISS_PUDDLE_LIFT) < 1e-3f, "on the floor's top (+0.03)");
    }
    hold(p, 100, 1);
    CHECK(game->puddles.count == 1 && g_adds == adds + 1, "more shots at the spot refresh it (%d puddles, %d ADDs)", game->puddles.count, g_adds - adds);
    hold(p, 20, 0);
}

static void test_puddle_damage(Game *game, Player *p, Player *target, Player *third) {
    printf("Puddle damage: 18 per second in 250 ms ticks, then it dries up...\n");
    piss_round_start(game);
    const vec3 at = {50.0f, PISS_PUDDLE_LIFT, 50.0f};
    const int id = piss_puddle_spawn(game, p, WEAPON_ID_PISS, at);
    CHECK(id >= 0 && game->puddles.count == 1, "spawned");
    stand(target, 51.0f, 51.0f, 0.0f);
    stand(p, 50.0f, 49.0f, 0.0f);     // the owner stands in it too
    stand(third, 50.0f, 50.0f, 0.0f); // and someone 3 units above
    third->position.y = 3.0f;
    const float t0 = target->health, o0 = p->health, h0 = third->health;
    float ticks_health[5];
    for (int i = 0; i < 20; i++) { // 1 s in 50 ms steps
        piss_tick(game, 0.05f);
        if ((i + 1) % 5 == 0) ticks_health[(i + 1) / 5] = target->health;
    }
    ticks_health[0] = t0;
    const float dealt = t0 - target->health;
    printf("  1 s in the puddle: %.2f damage (%.2f %.2f %.2f %.2f a tick)\n", dealt, ticks_health[0] - ticks_health[1], ticks_health[1] - ticks_health[2],
           ticks_health[2] - ticks_health[3], ticks_health[3] - ticks_health[4]);
    CHECK(fabsf(dealt - 18.0f) < 1e-3f, "18 in a second (%.3f)", dealt);
    CHECK(fabsf(ticks_health[0] - ticks_health[1] - 4.5f) < 1e-3f, "4.5 a 250 ms tick");
    CHECK(p->health == o0, "not its owner");
    CHECK(third->health == h0, "not 3 units above it");

    // a second overlapping puddle of the owner's (not merged: 2 units away) does not double it
    piss_puddle_spawn(game, p, WEAPON_ID_PISS, (vec3) {52.0f, PISS_PUDDLE_LIFT, 50.0f});
    const float t1 = target->health;
    piss_tick(game, 0.25f);
    CHECK(game->puddles.count == 2 && fabsf(t1 - target->health - 4.5f) < 1e-3f, "two puddles underfoot: still 4.5 a tick (%.2f)", t1 - target->health);

    // dries up at 5 s
    const int removes = g_removes;
    int gone_at = -1;
    for (int i = 0; i < 100 && gone_at < 0; i++) {
        piss_tick(game, 0.05f);
        if (puddle_index_alive(game, id) == 0) gone_at = i;
    }
    const float total = t0 - target->health;
    printf("  the first puddle went %.2f s after it came; %.1f damage in all\n", 1.25f + (gone_at + 1) * 0.05f, total);
    CHECK(gone_at >= 0 && fabsf(1.25f + (gone_at + 1) * 0.05f - 5.0f) < 0.051f, "lives 5 s");
    CHECK(g_removes > removes, "REMOVE sent");
    for (int i = 0; i < 40; i++) piss_tick(game, 0.05f);
    CHECK(game->puddles.count == 0, "all dry");
    const float after = target->health;
    piss_tick(game, 1.0f);
    CHECK(target->health == after, "no damage without a puddle");
}

static void test_wall(Game *game, Player *p) {
    printf("Stream on a wall: the puddle at its foot...\n");
    piss_round_start(game);
    stand(p, 0.0f, WALL_FACE_Z + 15.0f, 0.1f);
    PissStream trace;
    CHECK(player_stream_trace(p, 0.1f, 0.0f, 1, &trace), "traced");
    printf("  hit %d at %.2f %.2f %.2f after %.0f ms; puddle %d at %.2f %.2f %.2f\n", trace.hit, trace.end.x, trace.end.y, trace.end.z, trace.time,
           trace.puddle, trace.puddle_at.x, trace.puddle_at.y, trace.puddle_at.z);
    CHECK(trace.hit == PISS_HIT_OBJECT && fabsf(trace.end.z - WALL_FACE_Z) < 0.05f && trace.end.y > 1.0f, "on the wall face, above the floor");
    hold(p, 3, 1);
    const GamePuddle *puddle = puddle_of(game, p->uid);
    CHECK(puddle && fabsf(puddle->position.z - (WALL_FACE_Z + 0.3f)) < 0.11f && fabsf(puddle->position.y - PISS_PUDDLE_LIFT) < 1e-3f,
          "at the foot of the wall on the floor");
    hold(p, 20, 0);
}

static void test_caps(Game *game, Player *a, Player *b, Player *c) {
    printf("Caps: 24 per owner, 64 in the game...\n");
    piss_round_start(game);
    const int first = piss_puddle_spawn(game, a, WEAPON_ID_PISS, (vec3) {0.0f, PISS_PUDDLE_LIFT, 100.0f});
    for (int i = 1; i < 30; i++) piss_puddle_spawn(game, a, WEAPON_ID_PISS, (vec3) {i * 10.0f, PISS_PUDDLE_LIFT, 100.0f});
    CHECK(piss_puddle_count(game, a->uid) == 24, "24 of 30 for one owner (%d)", piss_puddle_count(game, a->uid));
    CHECK(puddle_index_alive(game, first) == 0, "the oldest went first");
    for (int i = 0; i < 30; i++) piss_puddle_spawn(game, b, WEAPON_ID_PISS, (vec3) {i * 10.0f, PISS_PUDDLE_LIFT, 200.0f});
    for (int i = 0; i < 30; i++) piss_puddle_spawn(game, c, WEAPON_ID_PISS, (vec3) {i * 10.0f, PISS_PUDDLE_LIFT, 300.0f});
    printf("  %d / %d / %d puddles, %d in the game\n", piss_puddle_count(game, a->uid), piss_puddle_count(game, b->uid),
           piss_puddle_count(game, c->uid), game->puddles.count);
    CHECK(game->puddles.count == 64, "64 in the game (%d)", game->puddles.count);
    CHECK(piss_puddle_count(game, a->uid) == 16 && piss_puddle_count(game, c->uid) == 24, "the game's oldest went (a's)");
    piss_player_left(game, b);
    CHECK(piss_puddle_count(game, b->uid) == 0 && game->puddles.count == 40, "an owner leaving takes theirs");
}

static void test_replication(Game *game, Player *p, Game *client) {
    printf("Replication: SPAWNED_OBJ puddles on a client and a joiner...\n");
    piss_round_start(game);
    piss_round_start(client);
    g_client = client;
    for (int i = 0; i < 5; i++) piss_puddle_spawn(game, p, WEAPON_ID_PISS, (vec3) {i * 10.0f, 1.5f, -10.0f});
    CHECK(client->puddles.count == 5, "5 ADDs, 5 puddles on the client (%d)", client->puddles.count);
    int same = client->puddles.count == game->puddles.count;
    for (int i = 0; same && i < game->puddles.count; i++) {
        const GamePuddle *s = &game->puddles.items[i], *c = &client->puddles.items[i];
        same = s->id == c->id && s->owner == c->owner && s->position.x == c->position.x && s->position.y == c->position.y &&
               s->radius == c->radius && s->color == c->color && c->weapon_id == WEAPON_ID_PISS;
    }
    CHECK(same, "same ids, owners, spots, radius, colour");
    g_joiner_adds = 0;
    piss_send_state(game, 77);
    CHECK(g_joiner_adds == 5, "a joiner gets the 5 live puddles (%d)", g_joiner_adds);
    for (int i = 0; i < 120; i++) piss_tick(game, 0.05f);
    CHECK(game->puddles.count == 0 && client->puddles.count == 0, "they dry up on both (%d / %d)", game->puddles.count, client->puddles.count);
    g_client = NULL;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    // a floor (top at y 0) and a wall whose face is at z = -60
    cJSON *map = cJSON_Parse("{\"name\":\"piss\",\"spawns\":[[0,0,0],[0,0,-15]],\"objects\":["
                             "{\"p\":[0,-10,0],\"s\":[1200,10,1200]},"
                             "{\"p\":[0,0,-65],\"s\":[600,300,10]}]}");
    static const cJSON *maps[1];
    maps[0] = map;
    Game game = {0};
    game_configure(&game, NULL, maps, 1, NULL, 0, NULL, 0, NULL, 0);
    game_init(&game, 0, -1, 1);
    game.hooks.send = test_send;
    game.lag_comp = 0;

    Game client = {0};
    game_configure(&client, NULL, maps, 1, NULL, 0, NULL, 0, NULL, 0);
    game_init(&client, 0, -1, 0);
    client.ready = 1;

    Player *p = add_player(&game, 1);
    Player *target = add_player(&game, 2);
    Player *third = add_player(&game, 3);
    give_piss(p);
    CHECK(p->weapon == rules_weapon(WEAPON_ID_PISS), "the extra slot holds the Piss");

    test_weapon();
    test_stream_hit(&game, p, target);
    test_floor(&game, p, target);
    test_puddle_damage(&game, p, target, third);
    test_wall(&game, p);
    test_caps(&game, p, target, third);
    test_replication(&game, p, &client);

    cJSON_Delete(map);
    if (g_failures) {
        printf("piss_test: %d of %d checks failed\n", g_failures, g_checks);
        return 1;
    }
    printf("piss_test: %d checks OK\n", g_checks);
    return 0;
}
