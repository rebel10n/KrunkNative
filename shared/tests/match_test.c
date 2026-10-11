// Match logic through the shared game hooks (no sockets): two players on
// Sandstorm, hitscan damage with falloff/headshots, kill bookkeeping and kill
// score (kills.js), assists, respawn, chat sanitizing/burst limit, ping
// smoothing and the round clock -> end screen -> next round.
// Run from the repository root: ./<build>/match_test
#include <shared.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

void player_shoot(Player*); // shared/src/player.c
void player_do_map_collisions(Player*, const Input*, float move_dir, int can_jump, float delta, int recon); // shared/src/player.c

#define MAX_SENT 512

typedef struct {
    int uid;
    int type;
    uint16_t length;
    uint8_t payload[NET_MAX_PAYLOAD];
} SentPacket;

static SentPacket g_sent[MAX_SENT];
static int g_sent_count;
static int g_next_round_calls;
static int g_failures;

static void test_send(Game *game, const int uid, const int type, const void *payload, const uint16_t length) {
    (void) game;
    if (g_sent_count >= MAX_SENT) return;

    SentPacket *packet = &g_sent[g_sent_count++];
    packet->uid = uid;
    packet->type = type;
    packet->length = length;
    memcpy(packet->payload, payload, length);
}

static void test_next_round(Game *game) {
    (void) game;
    g_next_round_calls++;
}

static const SentPacket *find_sent(const int type, const int uid, const int nth) {
    int seen = 0;
    for (int i = 0; i < g_sent_count; i++) {
        if (g_sent[i].type != type || (uid != -2 && g_sent[i].uid != uid)) continue;
        if (seen++ == nth) return &g_sent[i];
    }
    return NULL;
}

#define CHECK(cond, ...) do { \
    if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_failures++; } \
} while (0)

static int near(const float a, const float b, const float eps) {
    return fabsf(a - b) <= eps;
}

// Places `player` high above the map (no geometry in the way), standing still.
static void place(Player *player, const float x, const float z) {
    player->position = (vec3) {x, 1000.0f, z};
    player->last_position = player->position;
    player->velocity = (vec3) {0.0f, 0.0f, 0.0f};
    player->direction = (vec2) {0.0f, 0.0f};
    player->spread = 0.0f;
    player->recoil_anim_y = 0.0f;
    player->air_time = 0.0f;
    player->slide_timer = 0.0f;
}

// Aim `shooter` (facing -z) at height `y` of a target `distance` ahead.
static void aim_at(Player *shooter, const float y, const float distance) {
    const float eye = shooter->position.y + shooter->height - game_constants.camera_height;
    shooter->direction.x = atanf((y - eye) / distance);
    shooter->direction.y = 0.0f;
}

static void shoot(Player *shooter) {
    shooter->ammo[shooter->loadout_index] = shooter->weapon->ammo;
    shooter->spread = 0.0f;
    player_shoot(shooter);
}

static Player *add_player(Game *game, const int uid, const char *name) {
    Player *player = player_init(game);
    player->uid = uid;
    player->class_index = 0; // Triggerman: Assault Rifle
    snprintf(player->name, sizeof(player->name), "%s", name);
    game_players_add(game, player);
    player_spawn(player);
    return player;
}

static void test_combat(Game *game) {
    printf("Running combat test...\n");

    Player *a = add_player(game, 1, "Alice");
    Player *b = add_player(game, 2, "Bob");
    Player *c = add_player(game, 3, "Carol");

    CHECK(a->weapon && !strcmp(a->weapon->name, "Assault Rifle"), "class 0 should hold the Assault Rifle (%s)", a->weapon ? a->weapon->name : "none");

    place(a, 0.0f, 0.0f);
    place(b, 0.0f, -50.0f);
    place(c, 40.0f, 40.0f);

    // Body shot at 50 units: 23 - dmgDrop(5) * (50 - 0) / 700 = 22.64.
    g_sent_count = 0;
    aim_at(a, b->position.y + 7.0f, 50.0f);
    shoot(a);
    const float body = 23.0f - 5.0f * 50.0f / 700.0f;
    CHECK(near(b->health, 100.0f - body, 0.05f), "body shot: health %.2f, expected %.2f", b->health, 100.0f - body);

    const SentPacket *hit = find_sent(NET_PACKET_HIT, 1, 0);
    CHECK(hit != NULL, "HIT packet to the shooter");
    if (hit) {
        NetHitPacket packet;
        memcpy(&packet, hit->payload, sizeof(packet));
        CHECK(packet.target_uid == 2 && packet.damage == 23 && !packet.crit && !packet.kill, "HIT {%d, %d, %d, %d}", packet.target_uid, packet.damage, packet.crit, packet.kill);
    }

    CHECK(find_sent(NET_PACKET_SHOT, NET_TARGET_EXCEPT(1), 0) != NULL, "SHOT to everyone but the shooter (spectators too)");

    const SentPacket *health = find_sent(NET_PACKET_HEALTH, 2, 0);
    CHECK(health != NULL, "HEALTH packet to the victim");
    if (health) {
        NetHealthPacket packet;
        memcpy(&packet, health->payload, sizeof(packet));
        CHECK(packet.has_source && packet.health == ceilf(b->health), "victim HEALTH carries the dealer and ceil(hp)");
    }
    CHECK(find_sent(NET_PACKET_HEALTH, NET_TARGET_EXCEPT(2), 0) != NULL, "HEALTH broadcast to everyone but the victim (players + spectators)");

    // Leg shot (below legHeight 4.2): half damage.
    const float before_leg = b->health;
    aim_at(a, b->position.y + 2.0f, 50.0f);
    shoot(a);
    CHECK(near(before_leg - b->health, body * 0.5f, 0.05f), "leg shot: %.2f, expected %.2f", before_leg - b->health, body * 0.5f);

    // Carol chips Bob for the assist (>= assistMin 20 within assistTime 5 s).
    place(c, 0.0f, 0.0f);
    c->position.x = 3.0f;
    aim_at(c, b->position.y + 7.0f, 50.0f);
    shoot(c);
    place(c, 40.0f, 40.0f);

    // Headshots (top 2 units of the hitbox, hsMlt 1.5) until Bob drops.
    g_sent_count = 0;
    int shots = 0;
    while (b->active && shots < 10) {
        aim_at(a, b->position.y + b->height - 0.5f, 50.0f);
        shoot(a);
        shots++;
    }

    CHECK(!b->active, "Bob died after %d headshots", shots);
    CHECK(b->deaths == 1 && a->kills == 1, "deaths %u kills %u", b->deaths, a->kills);
    CHECK(c->assists == 1 && c->score == GAME_ASSIST_SCORE, "assist: %u, score %d", c->assists, c->score);

    // First Blood 25 + kill 50 + Headshot 50.
    CHECK(a->score == 125, "kill score %d, expected 125", a->score);

    const SentPacket *kill = find_sent(NET_PACKET_KILL, NET_TARGET_ALL, 0);
    CHECK(kill != NULL, "KILL broadcast");
    if (kill) {
        NetKillPacket packet;
        memcpy(&packet, kill->payload, sizeof(packet));
        CHECK(packet.victim_uid == 2 && packet.killer_uid == 1 && packet.headshot && packet.score == 125 &&
              packet.victim_deaths == 1 && packet.killer_kills == 1 && packet.weapon_index == a->loadout[a->loadout_index],
              "KILL {victim %d killer %d hs %d score %d}", packet.victim_uid, packet.killer_uid, packet.headshot, packet.score);
        CHECK(near(packet.respawn_delay, GAME_DEATH_DELAY, 0.001f), "respawn delay %.2f", packet.respawn_delay);
    }

    const SentPacket *score = find_sent(NET_PACKET_SCORE, 1, 0);
    CHECK(score != NULL, "SCORE to the killer");

    const SentPacket *last_hit = NULL;
    for (int i = 0; find_sent(NET_PACKET_HIT, 1, i); i++) last_hit = find_sent(NET_PACKET_HIT, 1, i);
    if (last_hit) {
        NetHitPacket packet;
        memcpy(&packet, last_hit->payload, sizeof(packet));
        CHECK(packet.kill && packet.crit, "killing HIT is a headshot kill");
    }

    // Shooting the corpse does nothing.
    g_sent_count = 0;
    shoot(a);
    CHECK(!find_sent(NET_PACKET_HIT, 1, 0), "no hit on a dead player");

    // Leaderboard on the next tick: Alice first.
    g_sent_count = 0;
    game_tick(game, game->now + 0.033f, 0.033f);
    const SentPacket *board = find_sent(NET_PACKET_LEADERBOARD, NET_TARGET_ALL, 0);
    CHECK(board != NULL, "LEADERBOARD after the kill");
    if (board) {
        NetLeaderboardPacket packet;
        memcpy(&packet, board->payload, sizeof(packet));
        CHECK(packet.count == 3 && packet.entries[0].uid == 1 && packet.entries[0].score == 125 && packet.entries[0].kills == 1,
              "board[0] = {%d, %d, %d}", packet.entries[0].uid, packet.entries[0].score, packet.entries[0].kills);
    }

    // Respawn: full health, playing again; a second quick kill is a Double Kill? (no: >2 s apart).
    player_spawn(b);
    place(b, 0.0f, -50.0f);
    CHECK(b->active && b->health == (float) b->max_health, "respawned with full health");

    // Revenge: Bob kills Alice -> +25 Revenge.
    a->health = 10.0f;
    place(b, 0.0f, -50.0f);
    b->direction = (vec2) {0.0f, (float) M_PI}; // face +z towards Alice
    const float eye = b->position.y + b->height - game_constants.camera_height;
    b->direction.x = atanf((a->position.y + 7.0f - eye) / 50.0f);
    b->ammo[b->loadout_index] = b->weapon->ammo;
    g_sent_count = 0;
    player_shoot(b);
    CHECK(!a->active && b->kills == 1, "Bob killed Alice");
    CHECK(b->score == 50 + 25, "revenge kill score %d, expected 75", b->score);

    // Fall / death zone: suicide, no killer.
    player_spawn(a);
    place(a, 0.0f, 0.0f);
    g_sent_count = 0;
    player_kill(a, NULL, NULL, 0);
    kill = find_sent(NET_PACKET_KILL, NET_TARGET_ALL, 0);
    if (kill) {
        NetKillPacket packet;
        memcpy(&packet, kill->payload, sizeof(packet));
        CHECK(packet.killer_uid == -1 && packet.weapon_index == -1 && packet.score == 0, "suicide KILL");
    }
    CHECK(a->deaths == 2, "suicide counts a death");

    // Regen: 0.5 s ticks heal maxHealth * regen once regenDelay passed.
    player_spawn(a);
    place(a, 0.0f, 0.0f);
    a->health = 50.0f;
    a->hit_timer = 0.0f;
    a->regen_ticker = 0.0f;
    game_tick(game, game->now + 0.033f, 0.033f);
    CHECK(near(a->health, 60.0f, 0.01f), "regen: %.2f", a->health);

    printf("Combat test %s\n", g_failures ? "FAILED" : "OK");
}

static void test_projectiles(Game *game) {
    const int failures = g_failures;
    printf("Running projectile test...\n");

    Player *a = game_find_player(game, 1);
    Player *b = game_find_player(game, 2);
    if (!a->active) player_spawn(a);
    if (!b->active) player_spawn(b);
    place(a, 0.0f, 0.0f);
    place(b, 0.0f, -60.0f);

    // Crossbow bolt (dmg 200, spd 0.68/ms, no gravity) at Bob's chest.
    const Weapon *crossbow = g_weapons[13];
    const ProjectileConfig *bolt = projectile_config_for(crossbow, 0);
    CHECK(bolt && bolt->damage == 200.0f, "crossbow projectile config");
    CHECK(!projectile_config_for(g_weapons[1], 0), "the Assault Rifle fires no projectile");

    g_sent_count = 0;
    const float eye = a->position.y + a->height - game_constants.camera_height;
    const vec3 origin = {a->position.x, eye, a->position.z};
    game_spawn_projectile(game, a, 13, bolt, origin, 0.0f, atanf((b->position.y + 6.0f - eye) / 60.0f), 0);
    CHECK(find_sent(NET_PACKET_PROJECTILE, NET_TARGET_ALL, 0) != NULL, "PROJECTILE broadcast");

    const unsigned int kills_before = a->kills;
    for (int i = 0; i < 30 && b->active; i++) game_tick(game, game->now + 0.033f, 0.033f);
    CHECK(!b->active && a->kills == kills_before + 1, "bolt killed Bob (active %d)", b->active);
    CHECK(find_sent(NET_PACKET_PROJECTILE_END, NET_TARGET_ALL, 0) != NULL, "PROJECTILE_END after the hit");

    const SentPacket *kill = find_sent(NET_PACKET_KILL, NET_TARGET_ALL, 0);
    if (kill) {
        NetKillPacket packet;
        memcpy(&packet, kill->payload, sizeof(packet));
        CHECK(packet.weapon_index == 13 && packet.killer_uid == 1, "bolt KILL weapon %d", packet.weapon_index);
    }

    // Wallbang: a penetrable wall halves the Assault Rifle's damage (dmg * materialDens.default * pierce
    // is lost); a solid wall stops the bullet.
    player_spawn(b);
    place(b, 0.0f, -50.0f);
    place(a, 0.0f, 0.0f);
    Object wall = {.active = 1, .penetrable = 1, .collision_type = COLLISION_TYPE_BOX,
                   .position = {0.0f, 990.0f, -25.0f}, .scale = {40.0f, 40.0f, 2.0f}};
    Object *wall_ptr = &wall;
    Object **saved_objects = game->map->objects;
    const size_t saved_count = game->map->object_count;
    game->map->objects = &wall_ptr;
    game->map->object_count = 1;

    aim_at(a, b->position.y + 7.0f, 50.0f);
    shoot(a);
    const float through = 23.0f - 23.0f * 0.5f - 5.0f * 50.0f / 700.0f;
    CHECK(near(100.0f - b->health, through, 0.05f), "wallbang damage %.2f, expected %.2f", 100.0f - b->health, through);

    wall.penetrable = 0;
    const float before_solid = b->health;
    shoot(a);
    CHECK(b->health == before_solid, "a solid wall stops the bullet");

    game->map->objects = saved_objects;
    game->map->object_count = saved_count;

    // Rocket splash: 1 - d / radius of eDmg, a fifth for the owner.
    player_spawn(b);
    place(b, 0.0f, -60.0f);
    g_sent_count = 0;
    game_explosion(game, (vec3) {0.0f, 1000.0f, -60.0f + 17.5f}, 35.0f, 35.0f, a, 8);
    CHECK(near(b->health, 100.0f - 35.0f * 0.5f, 0.01f), "splash at half radius: %.2f", b->health);
    CHECK(find_sent(NET_PACKET_EXPLOSION, NET_TARGET_ALL, 0) != NULL, "EXPLOSION broadcast");

    const float own_before = a->health;
    game_explosion(game, a->position, 35.0f, 35.0f, a, 8);
    CHECK(near(own_before - a->health, 35.0f * 0.2f, 0.01f), "self splash: %.2f", own_before - a->health);

    printf("Projectile test %s\n", g_failures != failures ? "FAILED" : "OK");
}

// Soul Sanctum's "Refill Player Knife" trigger (te 3 onEnter, ta 23): stepping in gives the
// thrown knife back and sends "rfl" (NET_PACKET_REFILL); standing in it does not repeat
// (inObject), leaving and re-entering does.
static void test_knife_refill(Game *game) {
    const int failures = g_failures;
    printf("Running knife refill trigger test...\n");

    Player *a = game_find_player(game, 1);
    if (!a->active) player_spawn(a);
    place(a, 0.0f, 0.0f);
    Object trigger = {.active = 1, .trigger = 1, .trigger_on_enter = 1, .trigger_action = TRIGGER_ACTION_REFILL_KNIFE,
                      .collision_type = COLLISION_TYPE_BOX, .position = {0.0f, 995.0f, 0.0f}, .scale = {14.0f, 14.0f, 14.0f}};
    Object *trigger_ptr = &trigger;
    Object **saved_objects = game->map->objects;
    const size_t saved_count = game->map->object_count;
    game->map->objects = &trigger_ptr;
    game->map->object_count = 1;
    const Input input = {0};

    a->can_throw = 0;
    g_sent_count = 0;
    player_do_map_collisions(a, &input, 0.0f, 0, 0.016f, 0);
    CHECK(a->can_throw, "entering the trigger refills the knife");
    CHECK(find_sent(NET_PACKET_REFILL, 1, 0) != NULL, "REFILL sent to the player");

    a->can_throw = 0;
    player_do_map_collisions(a, &input, 0.0f, 0, 0.016f, 0);
    CHECK(!a->can_throw && !find_sent(NET_PACKET_REFILL, 1, 1), "no refill while still inside (inObject)");

    place(a, 100.0f, 0.0f);
    player_do_map_collisions(a, &input, 0.0f, 0, 0.016f, 0);
    place(a, 0.0f, 0.0f);
    player_do_map_collisions(a, &input, 0.0f, 0, 0.016f, 0);
    CHECK(a->can_throw && find_sent(NET_PACKET_REFILL, 1, 1), "re-entering refills again");

    trigger.trigger_single_use = 1;
    trigger.triggered = 1;
    a->can_throw = 0;
    place(a, 100.0f, 0.0f);
    player_do_map_collisions(a, &input, 0.0f, 0, 0.016f, 0);
    place(a, 0.0f, 0.0f);
    player_do_map_collisions(a, &input, 0.0f, 0, 0.016f, 0);
    CHECK(!a->can_throw, "a used single-use trigger does not fire");

    game->map->objects = saved_objects;
    game->map->object_count = saved_count;
    a->can_throw = 1;
    printf("Knife refill trigger test %s\n", g_failures != failures ? "FAILED" : "OK");
}

static void test_round(Game *game) {
    const int failures = g_failures;
    printf("Running round test...\n");

    // Alice leads the board.
    Player *a = game_find_player(game, 1);
    Player *b = game_find_player(game, 2);
    a->score = 500;
    b->score = 100;

    game->round_timer = 2.5f;
    game->round_seconds_sent = -1;
    g_sent_count = 0;
    g_next_round_calls = 0;

    game_tick(game, game->now + 1.0f, 1.0f);
    const SentPacket *timer = find_sent(NET_PACKET_TIMER, NET_TARGET_ALL, 0);
    CHECK(timer != NULL, "TIMER broadcast");
    if (timer) {
        NetTimerPacket packet;
        memcpy(&packet, timer->payload, sizeof(packet));
        CHECK(packet.seconds == 1 && !packet.end, "TIMER %d end %d", packet.seconds, packet.end);
    }

    g_sent_count = 0;
    game_tick(game, game->now + 2.0f, 2.0f);
    CHECK(game->end_timer > 0.0f, "round over: end timer %.1f", game->end_timer);

    const SentPacket *end_a = find_sent(NET_PACKET_END, 1, 0);
    const SentPacket *end_b = find_sent(NET_PACKET_END, 2, 0);
    CHECK(end_a && end_b, "END to every player");
    const SentPacket *end_watch = find_sent(NET_PACKET_END, NET_TARGET_NON_PLAYERS, 0);
    CHECK(end_watch != NULL, "END (no result) to spectators / menu clients");
    if (end_watch) {
        NetEndPacket pw;
        memcpy(&pw, end_watch->payload, sizeof(pw));
        CHECK(pw.result == -1, "spectator END result %d", pw.result);
    }
    if (end_a && end_b) {
        NetEndPacket pa, pb;
        memcpy(&pa, end_a->payload, sizeof(pa));
        memcpy(&pb, end_b->payload, sizeof(pb));
        CHECK(pa.result == 1 && pb.result == 0, "results %d / %d", pa.result, pb.result);
        CHECK(pa.winner_uid == 1 && pa.next_round == 25, "winner %d next %d", pa.winner_uid, pa.next_round);
        CHECK(pa.board.entries[0].uid == 1 && pa.board.entries[1].uid == 2, "end board order");
    }

    for (size_t i = 0; i < game->player_count; i++) {
        CHECK(!game->players[i]->active, "players cleared on the end screen");
    }

    g_sent_count = 0;
    game_tick(game, game->now + 10.0f, 10.0f);
    timer = find_sent(NET_PACKET_TIMER, NET_TARGET_ALL, 0);
    if (timer) {
        NetTimerPacket packet;
        memcpy(&packet, timer->payload, sizeof(packet));
        CHECK(packet.end && packet.seconds == 15, "end countdown %d (end %d)", packet.seconds, packet.end);
    }
    CHECK(!g_next_round_calls, "next round not yet");

    game_tick(game, game->now + 15.5f, 15.5f);
    CHECK(g_next_round_calls == 1, "next round hook after the end screen (%d)", g_next_round_calls);

    game_start_round(game);
    CHECK(near(game->round_timer, (float) game->config.game_time * 60.0f, 0.01f) && game->end_timer == 0.0f, "fresh round clock");

    printf("Round test %s\n", g_failures != failures ? "FAILED" : "OK");
}

static void test_chat(void) {
    const int failures = g_failures;
    printf("Running chat test...\n");

    char out[NET_CHAT_TEXT_SIZE];
    char long_text[200];
    memset(long_text, 'a', sizeof(long_text) - 1);
    long_text[sizeof(long_text) - 1] = 0;

    CHECK(net_chat_sanitize(out, sizeof(out), long_text, strlen(long_text)) && strlen(out) == NET_CHAT_MAX_CHARS, "70 characters kept (%zu)", strlen(out));
    CHECK(!net_chat_sanitize(out, sizeof(out), "   \t ", 5), "blank line dropped");
    CHECK(net_chat_sanitize(out, sizeof(out), "  hi\nthere  ", 12) && !strcmp(out, "hi there"), "trimmed / control chars: '%s'", out);

    const char *utf8 = "\xc3\xa9t\xc3\xa9 \xe2\x9c\x93";
    CHECK(net_chat_sanitize(out, sizeof(out), utf8, strlen(utf8)) && !strcmp(out, utf8), "UTF-8 kept");
    CHECK(net_chat_sanitize(out, sizeof(out), "ok\xff\xfe!", 5) && !strcmp(out, "ok!"), "invalid bytes dropped: '%s'", out);

    NetChatLimiter limiter = {0};
    CHECK(net_chat_allow(&limiter, 10.0f) && net_chat_allow(&limiter, 10.2f) && net_chat_allow(&limiter, 10.4f), "3 messages per second");
    CHECK(!net_chat_allow(&limiter, 10.6f), "4th message in the window exceeds the burst");
    CHECK(net_chat_allow(&limiter, 11.1f), "new window after 1 s");

    NetChatPacket packet;
    net_packet_chat(&packet, 7, NET_CHAT_PLAYER, "Guest_7", "hello");
    CHECK(packet.uid == 7 && packet.kind == NET_CHAT_PLAYER && !strcmp(packet.name, "Guest_7") && !strcmp(packet.text, "hello"), "chat packet");
    CHECK(sizeof(NetChatPacket) <= NET_MAX_PAYLOAD && sizeof(NetEndPacket) <= NET_MAX_PAYLOAD, "packets fit NET_MAX_PAYLOAD");

    // ping EWMA (alpha 0.35)
    CHECK(net_ping_update(-1, 0.040f) == 40, "first ping sample");
    CHECK(net_ping_update(40, 0.140f) == 75, "smoothed ping %d", net_ping_update(40, 0.140f));
    CHECK(net_ping_update(40, 9.0f) == (int) (40 + (5000 - 40) * 0.35f + 0.5f), "ping sample capped");

    printf("Chat test %s\n", g_failures != failures ? "FAILED" : "OK");
}

int main(void) {
    cJSON *sandstorm = NULL;
    if (load_json_file("assets/maps/sandstorm.json", &sandstorm) && load_json_file("../assets/maps/sandstorm.json", &sandstorm)) {
        printf("Failed to load assets/maps/sandstorm.json (run from the repository root)\n");
        return 1;
    }

    static const cJSON *maps[1];
    maps[0] = sandstorm;

    Game game = {0};
    game_configure(&game, NULL, maps, 1, NULL, 0, NULL, 0, NULL, 0);
    game_init(&game, 0, -1, 1); // is_local: this game is the authority
    game.hooks.send = test_send;
    game.hooks.next_round = test_next_round;

    if (!game.ready) {
        printf("Failed to init the game\n");
        return 1;
    }

    test_combat(&game);
    test_projectiles(&game);
    test_knife_refill(&game);
    test_round(&game);
    test_chat();

    if (g_failures) {
        printf("%d check(s) failed\n", g_failures);
        return 1;
    }

    printf("Test OK\n");
    return 0;
}
