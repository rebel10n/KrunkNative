// Match logic through the shared game hooks (no sockets): two players on
// Sandstorm, hitscan damage with falloff/headshots, kill bookkeeping and kill
// score (kills.js), assists, respawn, chat sanitizing/burst limit, ping
// smoothing and the round clock -> end screen -> next round.
// Run from the repository root: ./<build>/match_test
#include <shared.h>
#include <ai.h>
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
    TriggerData trig = {.event = TRIGGER_EVENT_ON_ENTER, .action = TRIGGER_ACTION_REFILL_KNIFE};
    Object trigger = {.active = 1, .trigger = 1, .trig = &trig,
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

    trig.single_use = 1;
    trig.triggered = 1;
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

// ---------------------------------------------------------------- map triggers
// triggers.js actions / events, interfaces, check points and teleporters on a small object
// list swapped into the Sandstorm game (shared/src/triggers.c). Every action is run once
// through game.onTrigger, the events through the code paths that raise them.

#define TM_MAX 12

typedef struct {
    Object objects[TM_MAX];
    TriggerData trigs[TM_MAX];
    Object *list[TM_MAX];
    size_t count;
    Object **saved;
    size_t saved_count;
} TestMap;

static Object *tm_add(TestMap *tm, const vec3 bottom, const vec3 scale) {
    Object *o = &tm->objects[tm->count];
    memset(o, 0, sizeof(*o));
    o->active = 1;
    o->collision_type = COLLISION_TYPE_BOX;
    o->position = bottom;
    o->scale = scale;
    o->destroyed_by = -1;
    o->wall_jumpable = 1;
    tm->list[tm->count] = o;
    tm->count++;
    return o;
}

static TriggerData *tm_trigger(TestMap *tm, Object *o, const int event, const int action) {
    TriggerData *t = &tm->trigs[o - tm->objects];
    memset(t, 0, sizeof(*t));
    t->event = event;
    t->action = action;
    t->volume = t->rate = 1.0f;
    t->timer = 1000.0f;
    o->trigger = 1;
    o->trig = t;
    if (event == TRIGGER_EVENT_ON_ENTER) o->no_shoot = 1;
    return t;
}

static void tm_use(Game *game, TestMap *tm) {
    tm->saved = game->map->objects;
    tm->saved_count = game->map->object_count;
    game->map->objects = tm->list;
    game->map->object_count = tm->count;
}

static void tm_restore(Game *game, TestMap *tm) {
    game->map->objects = tm->saved;
    game->map->object_count = tm->saved_count;
}

static Player *tm_ready(Game *game, const int uid) {
    Player *p = game_find_player(game, uid);
    if (!p->active) player_spawn(p);
    p->health = (float) p->max_health;
    p->god_mode = 0;
    return p;
}

static int sent_object(const int kind, const int uid) { // an OBJECT_STATE of that kind for object uid
    for (int i = 0; i < g_sent_count; i++) {
        if (g_sent[i].type != NET_PACKET_OBJECT_STATE) continue;
        NetObjectStatePacket p;
        memcpy(&p, g_sent[i].payload, sizeof(p));
        if (p.kind == kind && p.uid == uid) return 1;
    }
    return 0;
}

static void collide(Player *p) {
    const Input input = {0};
    player_do_map_collisions(p, &input, 0.0f, 0, 0.016f, 0);
}

// The player actions (score, kill, respawn, health, messages, popups, team, loadout, knife, scare).
static void test_trigger_player_actions(Game *game) {
    const int failures = g_failures;
    printf("Running trigger player actions test...\n");
    TestMap tm = {0};
    Object *trigger = tm_add(&tm, (vec3) {500.0f, 0.0f, 500.0f}, (vec3) {10.0f, 10.0f, 10.0f});
    TriggerData *t = tm_trigger(&tm, trigger, TRIGGER_EVENT_ON_SHOOT, TRIGGER_ACTION_GIVE_SCORE);
    tm_use(game, &tm);
    Player *a = tm_ready(game, 1);
    Player *b = tm_ready(game, 2);
    place(a, 0.0f, 0.0f);
    place(b, 0.0f, -50.0f);

    // 0 Give Player Score: triggerConstant || 1
    a->score = 0;
    g_sent_count = 0;
    game_on_trigger(game, a, trigger, NULL);
    CHECK(a->score == 1 && find_sent(NET_PACKET_SCORE, 1, 0), "Give Player Score: +1 without a value (%d)", a->score);
    t->constant = 25;
    game_on_trigger(game, a, trigger, NULL);
    CHECK(a->score == 26, "Give Player Score: +Value (%d)", a->score);
    game_on_trigger(game, NULL, trigger, NULL);
    CHECK(a->score == 26, "no player, no score");

    // 13 Set Player Score: no "5"
    t->action = TRIGGER_ACTION_SET_SCORE;
    t->constant = 300;
    g_sent_count = 0;
    game_on_trigger(game, a, trigger, NULL);
    CHECK(a->score == 300 && !find_sent(NET_PACKET_SCORE, 1, 0), "Set Player Score: 300 without SCORE (%d)", a->score);

    // 3 Change Player Health: -(constant) as damage
    t->action = TRIGGER_ACTION_CHANGE_HEALTH;
    t->constant = -30;
    game_on_trigger(game, a, trigger, NULL);
    CHECK(near(a->health, (float) a->max_health - 30.0f, 0.01f), "Change Player Health -30: %.1f", a->health);
    t->constant = 10;
    game_on_trigger(game, a, trigger, NULL);
    CHECK(near(a->health, (float) a->max_health - 20.0f, 0.01f), "Change Player Health +10 heals: %.1f", a->health);
    t->constant = -1000;
    g_sent_count = 0;
    game_on_trigger(game, a, trigger, NULL);
    CHECK(!a->active && find_sent(NET_PACKET_KILL, NET_TARGET_ALL, 0), "Change Player Health: lethal damage kills");

    // 1 Kill Player (god mode is spared)
    a = tm_ready(game, 1);
    t->action = TRIGGER_ACTION_KILL_PLAYER;
    a->god_mode = 1;
    game_on_trigger(game, a, trigger, NULL);
    CHECK(a->active, "Kill Player spares god mode");
    a->god_mode = 0;
    game_on_trigger(game, a, trigger, NULL);
    CHECK(!a->active, "Kill Player");

    // 2 Respawn Player: a spawn point, recon (POSITION to the player, momentum kept)
    a = tm_ready(game, 1);
    place(a, 0.0f, 0.0f);
    a->velocity.x = 0.25f;
    t->action = TRIGGER_ACTION_RESPAWN_PLAYER;
    g_sent_count = 0;
    game_on_trigger(game, a, trigger, NULL);
    int on_spawn = 0;
    for (size_t i = 0; i < game->map->spawn_count; i++) {
        on_spawn |= near(a->position.x, game->map->spawns[i]->position.x, 0.01f) && near(a->position.z, game->map->spawns[i]->position.z, 0.01f);
    }
    const SentPacket *pos = find_sent(NET_PACKET_POSITION, 1, 0);
    CHECK(on_spawn && pos && near(a->velocity.x, 0.25f, 1e-6f), "Respawn Player: on a spawn point, POSITION sent, momentum kept");
    if (pos) {
        NetPositionPacket p;
        memcpy(&p, pos->payload, sizeof(p));
        CHECK(!p.stop_momentum && !p.has_direction && near(p.x, a->position.x, 0.01f), "Respawn Player POSITION fields");
    }

    // 9 Kill Opposing Team / 10 Respawn Opposing Team (FFA: everyone else)
    place(a, 0.0f, 0.0f);
    b = tm_ready(game, 2);
    place(b, 0.0f, -50.0f);
    t->action = TRIGGER_ACTION_RESPAWN_OPPOSING_TEAM;
    g_sent_count = 0;
    game_on_trigger(game, a, trigger, NULL);
    CHECK(near(a->position.y, 1000.0f, 0.01f) && !near(b->position.y, 1000.0f, 0.01f) && find_sent(NET_PACKET_POSITION, 2, 0),
          "Respawn Opposing Team moves the others only");
    t->action = TRIGGER_ACTION_KILL_OPPOSING_TEAM;
    game_on_trigger(game, a, trigger, NULL);
    CHECK(a->active && !b->active, "Kill Opposing Team kills the others only");
    b = tm_ready(game, 2);

    // 16 / 17 messages ("ch" null, text, 2), 29 / 30 popups ("am" [text, null])
    snprintf(t->text, sizeof(t->text), "%s", "Hello there!");
    const int text_actions[4] = {TRIGGER_ACTION_BROADCAST_MESSAGE, TRIGGER_ACTION_SEND_MESSAGE, TRIGGER_ACTION_BROADCAST_POPUP, TRIGGER_ACTION_SEND_POPUP};
    for (int i = 0; i < 4; i++) {
        t->action = text_actions[i];
        g_sent_count = 0;
        game_on_trigger(game, a, trigger, NULL);
        const int type = i < 2 ? NET_PACKET_CHAT : NET_PACKET_POPUP;
        const SentPacket *sent = find_sent(type, i % 2 ? 1 : NET_TARGET_ALL, 0);
        CHECK(sent != NULL, "%s sent", g_trigger_actions[text_actions[i]].name);
        if (sent && type == NET_PACKET_CHAT) {
            NetChatPacket p;
            memcpy(&p, sent->payload, sizeof(p));
            CHECK(p.uid == -1 && p.kind == NET_CHAT_SERVER && !strcmp(p.text, "Hello there!"), "%s: server line", g_trigger_actions[text_actions[i]].name);
        } else if (sent) {
            NetPopupPacket p;
            memcpy(&p, sent->payload, sizeof(p));
            CHECK(!strcmp(p.text, "Hello there!"), "%s text", g_trigger_actions[text_actions[i]].name);
        }
    }
    t->text[0] = 0;
    g_sent_count = 0;
    game_on_trigger(game, a, trigger, NULL);
    CHECK(!find_sent(NET_PACKET_POPUP, -2, 0), "no popup without a text");

    // 18 Swap Player Team / 19 Set Player Team (clamped to 1..2)
    a->team = 1;
    t->action = TRIGGER_ACTION_SWAP_TEAM;
    g_sent_count = 0;
    game_on_trigger(game, a, trigger, NULL);
    CHECK(a->team == 2 && find_sent(NET_PACKET_TEAM, -2, 0), "Swap Player Team 1 -> 2 (%d)", a->team);
    game_on_trigger(game, a, trigger, NULL);
    CHECK(a->team == 1, "Swap Player Team 2 -> 1");
    t->action = TRIGGER_ACTION_SET_TEAM;
    t->constant = 7;
    game_on_trigger(game, a, trigger, NULL);
    CHECK(a->team == 2, "Set Player Team clamps 7 to 2");
    t->constant = -3;
    game_on_trigger(game, a, trigger, NULL);
    CHECK(a->team == 1, "Set Player Team clamps -3 to 1");
    a->team = 0;

    // 15 Clear Player Loadout: the knife only, "inat"
    t->action = TRIGGER_ACTION_CLEAR_LOADOUT;
    g_sent_count = 0;
    game_on_trigger(game, a, trigger, NULL);
    CHECK(a->loadout_size == 1 && a->loadout[0] == WEAPON_ID_KNIFE && find_sent(NET_PACKET_LOADOUT, NET_TARGET_ALL, 0), "Clear Player Loadout");
    g_sent_count = 0;
    game_on_trigger(game, a, trigger, NULL);
    CHECK(!find_sent(NET_PACKET_LOADOUT, -2, 0), "Clear Player Loadout: already cleared");

    // 23 Refill Player Knife ("rfl"), 31 Jump Scare ("scr" 0)
    a->can_throw = 0;
    t->action = TRIGGER_ACTION_REFILL_KNIFE;
    g_sent_count = 0;
    game_on_trigger(game, a, trigger, NULL);
    CHECK(a->can_throw && find_sent(NET_PACKET_REFILL, 1, 0), "Refill Player Knife");
    t->action = TRIGGER_ACTION_JUMP_SCARE;
    game_on_trigger(game, a, trigger, NULL);
    CHECK(find_sent(NET_PACKET_JUMP_SCARE, 1, 0) != NULL, "Jump Scare to the player");

    // 14 Broadcast Sound: from the trigger centre, or (ef) at the player
    t->action = TRIGGER_ACTION_BROADCAST_SOUND;
    t->sound = 1;
    t->volume = 0.5f;
    t->rate = 2.0f;
    g_sent_count = 0;
    game_on_trigger(game, a, trigger, NULL);
    const SentPacket *snd = find_sent(NET_PACKET_SOUND, NET_TARGET_ALL, 0);
    CHECK(snd != NULL, "Broadcast Sound sent to everyone");
    if (snd) {
        NetSoundPacket p;
        memcpy(&p, snd->payload, sizeof(p));
        CHECK(!strcmp(p.name, "siren_0") && p.uid == -1 && near(p.position[1], 5.0f, 0.01f) && near(p.volume, 0.5f, 1e-6f) && near(p.rate, 2.0f, 1e-6f),
              "Broadcast Sound from the trigger: %s uid %d y %.1f", p.name, p.uid, p.position[1]);
    }
    t->emit_from = 1;
    g_sent_count = 0;
    game_on_trigger(game, a, trigger, NULL);
    snd = find_sent(NET_PACKET_SOUND, NET_TARGET_ALL, 0);
    if (snd) {
        NetSoundPacket p;
        memcpy(&p, snd->payload, sizeof(p));
        CHECK(p.uid == 1, "Broadcast Sound from the player: uid %d", p.uid);
    }
    t->asset_sound = 1;
    t->sound = 3206;
    g_sent_count = 0;
    game_on_trigger(game, a, trigger, NULL);
    snd = find_sent(NET_PACKET_SOUND, NET_TARGET_ALL, 0);
    if (snd) {
        NetSoundPacket p;
        memcpy(&p, snd->payload, sizeof(p));
        CHECK(!strcmp(p.name, "asset:3206"), "Broadcast Sound of an asset: %s", p.name);
    }

    tm_restore(game, &tm);
    a = tm_ready(game, 1);
    player_spawn(a); // the class loadout back
    a->score = 0;
    printf("Trigger player actions test %s\n", g_failures != failures ? "FAILED" : "OK");
}

// The interface actions: destructibles, gates, deposit boxes, teleport, storage, timers.
static void test_trigger_interface_actions(Game *game) {
    const int failures = g_failures;
    printf("Running trigger interface actions test...\n");
    TestMap tm = {0};
    Object *source = tm_add(&tm, (vec3) {500.0f, 0.0f, 500.0f}, (vec3) {10.0f, 10.0f, 10.0f});
    TriggerData *t = tm_trigger(&tm, source, TRIGGER_EVENT_ON_SHOOT, TRIGGER_ACTION_DESTROY_INTERFACE);
    t->interface_target = 5;
    Object *crate = tm_add(&tm, (vec3) {600.0f, 0.0f, 600.0f}, (vec3) {10.0f, 10.0f, 10.0f});        // 1
    crate->interface_id = 5;
    crate->health = crate->start_health = 100.0f;
    crate->can_destroy = 1;
    Object *gate = tm_add(&tm, (vec3) {700.0f, 0.0f, 700.0f}, (vec3) {10.0f, 10.0f, 10.0f});         // 2
    gate->interface_id = 5;
    gate->gate = 1;
    Object *other = tm_add(&tm, (vec3) {800.0f, 0.0f, 800.0f}, (vec3) {10.0f, 10.0f, 10.0f});        // 3: interface 6
    other->interface_id = 6;
    other->health = other->start_health = 100.0f;
    other->can_destroy = 1;
    Object *chained = tm_add(&tm, (vec3) {900.0f, 0.0f, 900.0f}, (vec3) {10.0f, 10.0f, 10.0f});      // 4: onDestroy -> score
    chained->interface_id = 5;
    chained->can_destroy = 1;
    tm_trigger(&tm, chained, TRIGGER_EVENT_ON_DESTROY, TRIGGER_ACTION_GIVE_SCORE);
    Object *bank = tm_add(&tm, (vec3) {1000.0f, 0.0f, 1000.0f}, (vec3) {10.0f, 10.0f, 10.0f});       // 5
    bank->interface_id = 5;
    bank->bank = 1;
    tm_use(game, &tm);
    Player *a = tm_ready(game, 1);
    place(a, 0.0f, 0.0f);
    a->score = 0;

    // 4 Destroy Interface: health / gates / canDestroy of the target interface
    g_sent_count = 0;
    game_on_trigger(game, a, source, NULL);
    CHECK(!crate->active && crate->destroyed && crate->health == 0.0f && crate->destroyed_by == 1 && sent_object(NET_OBJECT_DESTROY, 1),
          "Destroy Interface: the crate is destroyed (\"do\")");
    CHECK(!gate->active && sent_object(NET_OBJECT_GATE, 2), "Destroy Interface: the gate opens (\"gte\")");
    CHECK(other->active, "Destroy Interface: other interfaces stay");
    CHECK(chained->destroyed && a->score == 1, "Destroy Interface: onDestroy of the target chains (score %d)", a->score);

    // 11 Respawn Destructible Interface (onRespawn via destroyedBy), 12 Toggle Destructible
    t->action = TRIGGER_ACTION_RESPAWN_DESTRUCTIBLE;
    g_sent_count = 0;
    game_on_trigger(game, a, source, NULL);
    CHECK(crate->active && !crate->destroyed && crate->health == 100.0f && sent_object(NET_OBJECT_RESPAWN, 1), "Respawn Destructible Interface");
    CHECK(chained->active && crate->destroyed_by == -1, "Respawn Destructible Interface: canDestroy objects too");
    t->action = TRIGGER_ACTION_TOGGLE_DESTRUCTIBLE;
    game_on_trigger(game, a, source, NULL);
    CHECK(crate->destroyed && chained->destroyed, "Toggle Destructible Interface destroys");
    game_on_trigger(game, a, source, NULL);
    CHECK(!crate->destroyed && crate->active && !chained->destroyed, "Toggle Destructible Interface respawns");

    // 5 Toggle Interface Gate
    t->action = TRIGGER_ACTION_TOGGLE_GATE;
    gate->active = 0;
    game_on_trigger(game, a, source, NULL);
    CHECK(gate->active, "Toggle Interface Gate closes");
    game_on_trigger(game, a, source, NULL);
    CHECK(!gate->active, "Toggle Interface Gate opens");

    // 6 Change Interface Health: +Value heals, <= 0 destroys
    t->action = TRIGGER_ACTION_CHANGE_INTERFACE_HEALTH;
    t->constant = -60;
    game_on_trigger(game, a, source, NULL);
    CHECK(near(crate->health, 40.0f, 0.01f) && crate->active, "Change Interface Health -60: %.0f", crate->health);
    t->constant = -60;
    g_sent_count = 0;
    game_on_trigger(game, a, source, NULL);
    CHECK(crate->destroyed && sent_object(NET_OBJECT_DESTROY, 1), "Change Interface Health: destroyed at 0");
    game_respawn_object(game, crate, NULL);

    // 8 Change Interface Deposit Box Amount ("bnk")
    t->action = TRIGGER_ACTION_CHANGE_DEPOSIT;
    t->constant = 150;
    g_sent_count = 0;
    game_on_trigger(game, a, source, NULL);
    CHECK(bank->deposited == 150 && sent_object(NET_OBJECT_BANK, 5), "Change Interface Deposit Box Amount: %d", bank->deposited);

    // 7 Teleport To Interface: the last matching interface's bottom centre, momentum stopped
    t->action = TRIGGER_ACTION_TELEPORT_TO_INTERFACE;
    a->velocity = (vec3) {0.3f, 0.1f, 0.2f};
    g_sent_count = 0;
    game_on_trigger(game, a, source, NULL);
    CHECK(near(a->position.x, 1000.0f, 0.01f) && near(a->position.y, 0.0f, 0.01f) && a->velocity.x == 0.0f, "Teleport To Interface: (%.0f, %.0f)", a->position.x, a->position.y);
    const SentPacket *pos = find_sent(NET_PACKET_POSITION, 1, 0);
    if (pos) {
        NetPositionPacket p;
        memcpy(&p, pos->payload, sizeof(p));
        CHECK(p.stop_momentum, "Teleport To Interface stops momentum on the client");
    } else {
        CHECK(0, "Teleport To Interface: POSITION");
    }
    place(a, 0.0f, 0.0f);

    // 25..28 storage and the onStorage* events (Event Value = threshold)
    Object *store = tm_add(&tm, (vec3) {1100.0f, 0.0f, 1100.0f}, (vec3) {10.0f, 10.0f, 10.0f}); // 6
    store->interface_id = 5;
    TriggerData *st = tm_trigger(&tm, store, TRIGGER_EVENT_ON_STORAGE_EQUALS, TRIGGER_ACTION_GIVE_SCORE);
    st->constant_event = 3;
    game->map->object_count = tm.count;
    a->score = 0;
    t->action = TRIGGER_ACTION_INCREASE_STORAGE;
    t->constant = 2;
    game_on_trigger(game, a, source, NULL);
    CHECK(st->storage == 2 && a->score == 0, "Increase Interface Storage 2 (no event)");
    t->constant = 1;
    game_on_trigger(game, a, source, NULL);
    CHECK(st->storage == 3 && a->score == 1, "Increase Interface Storage: onStorageEquals at 3");
    t->action = TRIGGER_ACTION_DECREASE_STORAGE;
    st->event = TRIGGER_EVENT_ON_STORAGE_LESSER;
    game_on_trigger(game, a, source, NULL);
    CHECK(st->storage == 2 && a->score == 2, "Decrease Interface Storage: onStorageLesser");
    t->action = TRIGGER_ACTION_SET_STORAGE;
    t->constant = 9;
    st->event = TRIGGER_EVENT_ON_STORAGE_GREATER;
    game_on_trigger(game, a, source, NULL);
    CHECK(st->storage == 9 && a->score == 3, "Set Interface Storage: onStorageGreater");
    t->action = TRIGGER_ACTION_RESET_STORAGE;
    st->event = TRIGGER_EVENT_ON_STORAGE_LESSER; // Reset only looks at Equals / Greater targets
    game_on_trigger(game, a, source, NULL);
    CHECK(st->storage == 9, "Reset Interface Storage skips onStorageLesser targets");
    st->event = TRIGGER_EVENT_ON_STORAGE_EQUALS;
    st->constant_event = 0;
    game_on_trigger(game, a, source, NULL);
    CHECK(st->storage == 0 && a->score == 4, "Reset Interface Storage: 0, onStorageEquals");

    // 21 Reset Interface: restart onTimer triggers; the onTimer loop
    st->event = TRIGGER_EVENT_ON_TIMER;
    st->constant_event = 2;
    st->timer = 500.0f;
    t->action = TRIGGER_ACTION_RESET_INTERFACE;
    game_on_trigger(game, a, source, NULL);
    CHECK(near(st->timer, 2000.0f, 0.01f), "Reset Interface: timer %.0f", st->timer);
    st->action = TRIGGER_ACTION_BROADCAST_POPUP;
    snprintf(st->text, sizeof(st->text), "%s", "tick");
    g_sent_count = 0;
    game_tick_map_objects(game, 1999.0f);
    CHECK(!find_sent(NET_PACKET_POPUP, -2, 0), "onTimer: not yet");
    game_tick_map_objects(game, 2.0f);
    CHECK(find_sent(NET_PACKET_POPUP, NET_TARGET_ALL, 0) && near(st->timer, 2000.0f, 0.01f), "onTimer fires every Event Value seconds");
    store->active = 0;
    g_sent_count = 0;
    game_tick_map_objects(game, 5000.0f);
    CHECK(!find_sent(NET_PACKET_POPUP, -2, 0), "onTimer: inactive triggers wait");
    store->active = 1;

    // 24 Destroy Self (and the selfDestruct of single-use canDestroy triggers)
    t->action = TRIGGER_ACTION_DESTROY_SELF;
    source->can_destroy = 1;
    game_on_trigger(game, a, source, NULL);
    CHECK(!source->active && source->destroyed, "Destroy Self");
    game_respawn_object(game, source, NULL);
    t->action = TRIGGER_ACTION_GIVE_SCORE;
    t->constant = 0;
    t->single_use = 1;
    t->triggered = 0;
    t->self_destruct = 1;
    a->score = 0;
    game_on_trigger(game, a, source, NULL);
    CHECK(a->score == 1 && source->destroyed, "selfDestruct: the action, then Destroy Self");
    game_respawn_object(game, source, NULL);
    t->triggered = 0;
    t->single_use = t->self_destruct = 0;
    source->can_destroy = 0;

    // destructible respawn timers (updateDestructables) with onRespawn on the destroyer
    crate->respawn_time = crate->respawn_time_start = 3000.0f;
    tm_trigger(&tm, crate, TRIGGER_EVENT_ON_RESPAWN, TRIGGER_ACTION_GIVE_SCORE);
    a->score = 0;
    game_destroy_object(game, crate, a);
    game_tick_map_objects(game, 2999.0f);
    CHECK(crate->destroyed, "destructible: still destroyed before respawnT");
    g_sent_count = 0;
    game_tick_map_objects(game, 2.0f);
    CHECK(!crate->destroyed && crate->active && sent_object(NET_OBJECT_RESPAWN, 1) && a->score == 1, "destructible respawns after respawnT, onRespawn");

    // the trigger chain limit: two triggers that destroy / respawn each other run 20 actions
    Object *ping = tm_add(&tm, (vec3) {1200.0f, 0.0f, 1200.0f}, (vec3) {10.0f, 10.0f, 10.0f}); // 7
    Object *pong = tm_add(&tm, (vec3) {1300.0f, 0.0f, 1300.0f}, (vec3) {10.0f, 10.0f, 10.0f}); // 8
    TriggerData *tp1 = tm_trigger(&tm, ping, TRIGGER_EVENT_ON_STORAGE_GREATER, TRIGGER_ACTION_INCREASE_STORAGE);
    TriggerData *tp2 = tm_trigger(&tm, pong, TRIGGER_EVENT_ON_STORAGE_GREATER, TRIGGER_ACTION_INCREASE_STORAGE);
    ping->interface_id = 21;
    pong->interface_id = 22;
    tp1->interface_target = 22;
    tp2->interface_target = 21;
    tp1->constant = tp2->constant = 1;
    game->map->object_count = tm.count;
    game_on_trigger(game, a, ping, NULL);
    // 20 onTrigger calls; the 20th finds triggerChains[k] at 0 inside its interface loop
    CHECK(tp1->storage + tp2->storage == TRIGGER_LIMIT - 1, "trigger chains stop after triggerLimit calls (%d)", tp1->storage + tp2->storage);

    // 20 Round Time (ms), 22 End Round
    const float clock = game->round_timer;
    t->action = TRIGGER_ACTION_ROUND_TIME;
    t->constant = 10000;
    game_on_trigger(game, a, source, NULL);
    CHECK(near(game->round_timer, clock + 10.0f, 0.01f), "Round Time +10000 ms: %.1f", game->round_timer);
    t->constant = -10000;
    game_on_trigger(game, a, source, NULL);
    CHECK(near(game->round_timer, clock, 0.01f) && !game->mode->skip, "Round Time -10000 ms");
    t->action = TRIGGER_ACTION_END_ROUND;
    game_on_trigger(game, a, source, NULL);
    CHECK(game->mode->skip, "End Round: gameTimer = skip");
    game->mode->skip = 0;

    tm_restore(game, &tm);
    a->score = 0;
    printf("Trigger interface actions test %s\n", g_failures != failures ? "FAILED" : "OK");
}

// onTrigger gates (team, chance, single use) and the events raised by players / hits / AIs.
static void test_trigger_events(Game *game) {
    const int failures = g_failures;
    printf("Running trigger events test...\n");
    TestMap tm = {0};
    Object *zone = tm_add(&tm, (vec3) {0.0f, 995.0f, 0.0f}, (vec3) {14.0f, 14.0f, 14.0f});
    TriggerData *t = tm_trigger(&tm, zone, TRIGGER_EVENT_ON_COLLISION, TRIGGER_ACTION_GIVE_SCORE);
    tm_use(game, &tm);
    Player *a = tm_ready(game, 1);
    place(a, 0.0f, 0.0f);
    a->score = 0;

    // team filter: a teamed trigger needs a player of that team (in team modes)
    zone->team = 2;
    a->team = 1;
    game->mode->config.teams = 1;
    game_on_trigger(game, a, zone, NULL);
    CHECK(a->score == 0, "team trigger ignores the other team");
    a->team = 2;
    game_on_trigger(game, a, zone, NULL);
    CHECK(a->score == 1, "team trigger fires for its team");
    game->mode->config.teams = 0;
    zone->team = 0;
    a->team = 0;

    // single use, chance
    t->single_use = 1;
    t->triggered = 0;
    game_on_trigger(game, a, zone, NULL);
    game_on_trigger(game, a, zone, NULL);
    CHECK(a->score == 2, "single use fires once (%d)", a->score);
    t->single_use = 0;
    t->chance = 1;
    int fired = 0;
    for (int i = 0; i < 400; i++) {
        const int before = a->score;
        game_on_trigger(game, a, zone, NULL);
        fired += a->score != before;
    }
    CHECK(fired > 0 && fired < 40, "1%% chance fired %d / 400", fired);
    t->chance = 0;

    // onCollision: solid, at most once per (Event Value || 1) s
    a->score = 0;
    t->last_collision = 0.0f;
    game->now = 100.0f;
    collide(a);
    collide(a);
    CHECK(a->score == 1, "onCollision fires once per second (%d)", a->score);
    game->now = 101.0f;
    place(a, 0.0f, 0.0f);
    collide(a);
    CHECK(a->score == 2, "onCollision again after the cooldown (%d)", a->score);

    // onInteract: hold M1 on the ground inside for (Event Value || 1) s, walk-through
    t->event = TRIGGER_EVENT_ON_INTERACT;
    t->constant_event = 1;
    a->score = 0;
    place(a, 0.0f, 0.0f);
    a->on_ground = 1;
    a->interact_progress = 0.0f;
    const Input hold = {.shoot = 1};
    for (int i = 0; i < 9; i++) {
        a->on_ground = 1;
        player_do_map_collisions(a, &hold, 0.0f, 0, 0.1f, 0);
    }
    CHECK(a->score == 0 && near(a->interact_progress, 0.9f, 0.01f) && a->doing_interact, "onInteract: 0.9 s held (%.2f)", a->interact_progress);
    CHECK(game_interact_prompt(game, a, NULL) == GAME_INTERACT_TRIGGER, "onInteract prompt (type 4)");
    a->on_ground = 1;
    player_do_map_collisions(a, &hold, 0.0f, 0, 0.11f, 0);
    CHECK(a->score == 1 && a->interact_progress == 0.0f, "onInteract fires after 1 s");
    CHECK(near(a->position.y, 1000.0f, 0.01f), "onInteract triggers are walk-through");

    // onShoot: a hitscan ray into the trigger (solid, shootable)
    t->event = TRIGGER_EVENT_ON_SHOOT;
    zone->position = (vec3) {0.0f, 990.0f, -40.0f};
    zone->scale = (vec3) {20.0f, 40.0f, 4.0f};
    place(a, 0.0f, 0.0f);
    a->score = 0;
    aim_at(a, a->position.y + 7.0f, 40.0f);
    shoot(a);
    CHECK(a->score == 1, "onShoot: a bullet hits the trigger (%d)", a->score);
    t->event = TRIGGER_EVENT_ON_ENTER;
    zone->no_shoot = 1;
    shoot(a);
    CHECK(a->score == 1, "onEnter triggers are not shootable");
    zone->no_shoot = 0;

    // onDamage / onDestroy: shooting a trigger with health
    t->event = TRIGGER_EVENT_ON_DESTROY;
    zone->health = zone->start_health = 30.0f;
    zone->can_destroy = 1;
    g_sent_count = 0;
    shoot(a);
    CHECK(zone->active && zone->health < 30.0f && find_sent(NET_PACKET_HIT, 1, 0) && a->score == 1, "a shot damages the trigger (hp %.1f)", zone->health);
    shoot(a);
    CHECK(!zone->active && zone->destroyed && a->score == 2 && sent_object(NET_OBJECT_DESTROY, 0), "onDestroy when the health runs out (score %d)", a->score);
    game_respawn_object(game, zone, NULL);
    zone->health = zone->start_health = 0.0f;
    zone->can_destroy = 0;

    // onMelee / onPunch / onProjectile / onThrowingMelee through game_object_hit
    a->score = 0;
    t->event = TRIGGER_EVENT_ON_MELEE;
    game_object_hit(game, zone, a, 50.0f, 0, GAME_HIT_MELEE);
    t->event = TRIGGER_EVENT_ON_PUNCH;
    a->can_throw = 1;
    game_object_hit(game, zone, a, 50.0f, 0, GAME_HIT_MELEE);
    CHECK(a->score == 1, "onPunch needs fists (%d)", a->score);
    a->can_throw = 0;
    game_object_hit(game, zone, a, 35.0f, 0, GAME_HIT_MELEE);
    a->can_throw = 1;
    t->event = TRIGGER_EVENT_ON_PROJECTILE;
    game_object_hit(game, zone, a, 50.0f, 0, GAME_HIT_PROJECTILE);
    t->event = TRIGGER_EVENT_ON_THROWING_MELEE;
    game_object_hit(game, zone, a, 50.0f, 0, GAME_HIT_PROJECTILE);
    CHECK(a->score == 3, "onThrowingMelee only for thrown knives (%d)", a->score);
    game_object_hit(game, zone, a, 50.0f, 1, GAME_HIT_PROJECTILE);
    t->event = TRIGGER_EVENT_ON_DAMAGE;
    game_object_hit(game, zone, a, 50.0f, 0, GAME_HIT_SHOT);
    CHECK(a->score == 5, "onMelee, onPunch, onProjectile, onThrowingMelee, onDamage (%d)", a->score);

    // onAIDeath: the BOT itself is the trigger
    GameAI ai = {0};
    ai.data.trig.event = TRIGGER_EVENT_ON_AI_DEATH;
    ai.data.trig.action = TRIGGER_ACTION_GIVE_SCORE;
    ai.data.trig.constant = 7;
    a->score = 0;
    game_trigger_ai_death(game, a, &ai);
    CHECK(a->score == 7, "onAIDeath (%d)", a->score);
    ai.data.trig.event = TRIGGER_EVENT_NONE;
    game_trigger_ai_death(game, a, &ai);
    CHECK(a->score == 7, "a BOT without Can Trigger does nothing");

    // no action / out of range action, no authority
    t->action = TRIGGER_ACTION_COUNT;
    game_on_trigger(game, a, zone, NULL);
    t->action = TRIGGER_ACTION_GIVE_SCORE;
    game->is_local = 0;
    game_on_trigger(game, a, zone, NULL);
    game->is_local = 1;
    CHECK(a->score == 7, "invalid actions and clients run nothing");

    tm_restore(game, &tm);
    a->score = 0;
    a->can_throw = 1;
    printf("Trigger events test %s\n", g_failures != failures ? "FAILED" : "OK");
}

// setCheckPoint / respawn there, checkTeleport, gates and deposit boxes (players.interact).
static void test_checkpoints_teleporters(Game *game) {
    const int failures = g_failures;
    printf("Running check point / teleporter / gate test...\n");
    TestMap tm = {0};
    Object *check = tm_add(&tm, (vec3) {0.0f, 995.0f, 0.0f}, (vec3) {14.0f, 14.0f, 14.0f});        // 0
    check->checkpoint = 1;
    check->no_shoot = 1;
    check->direction = 2;
    Object *check2 = tm_add(&tm, (vec3) {100.0f, 995.0f, 0.0f}, (vec3) {14.0f, 14.0f, 14.0f});     // 1
    check2->checkpoint = 1;
    check2->single_use = 1;
    Object *send = tm_add(&tm, (vec3) {200.0f, 995.0f, 0.0f}, (vec3) {14.0f, 14.0f, 14.0f});       // 2
    send->teleporter = 1;
    send->channel = 4;
    Object *recv = tm_add(&tm, (vec3) {300.0f, 500.0f, 300.0f}, (vec3) {14.0f, 14.0f, 14.0f});     // 3
    recv->teleporter = 1;
    recv->channel = 4;
    recv->node = 2;
    recv->stop_momentum = 1;
    recv->force_dir = 1;
    recv->direction = 1;
    tm_use(game, &tm);
    Player *a = tm_ready(game, 1);

    // check points: "chp" once per new check point, single use ones only once ever
    place(a, 0.0f, 0.0f);
    g_sent_count = 0;
    collide(a);
    collide(a);
    CHECK(a->checkpoint == 1 && find_sent(NET_PACKET_CHECKPOINT, 1, 0) && !find_sent(NET_PACKET_CHECKPOINT, 1, 1), "check point set once");
    place(a, 100.0f, 0.0f);
    collide(a);
    place(a, 0.0f, 0.0f);
    collide(a);
    place(a, 100.0f, 0.0f);
    g_sent_count = 0;
    collide(a);
    CHECK(a->checkpoint == 1 && !find_sent(NET_PACKET_CHECKPOINT, 1, 0), "a single-use check point is taken once (%d)", a->checkpoint);

    // respawn at the check point, facing its direction
    player_kill(a, NULL, NULL, 0);
    player_spawn(a);
    CHECK(near(a->position.x, 0.0f, 0.01f) && near(a->position.y, 995.0f, 0.01f) && near(a->direction.y, -(float) M_PI / 2.0f * 3.0f, 1e-4f),
          "respawn at the check point (%.1f, %.1f)", a->position.x, a->position.y);

    // teleporters: sender -> receiver of the channel (node 2: 2 s cooldown), momentum / direction
    place(a, 200.0f, 0.0f);
    a->velocity = (vec3) {0.2f, 0.0f, 0.1f};
    collide(a);
    CHECK(near(a->position.x, 300.0f, 0.01f) && near(a->position.y, 500.0f, 0.01f) && a->velocity.x == 0.0f &&
          near(a->direction.y, -(float) M_PI, 1e-4f) && near(recv->tel_cooldown, 2000.0f, 0.01f), "teleported to the receiver");
    const vec3 there = a->position;
    a->position.y += 5.0f; // stand in the receiver: node 2 sends back, but cools down first
    collide(a);
    CHECK(near(a->position.x, there.x, 0.01f), "node 2 cooldown holds");
    recv->node = 1;
    game_tick_map_objects(game, 2000.0f);
    collide(a);
    CHECK(near(a->position.x, there.x, 0.01f) && recv->tel_cooldown == 0.0f, "a receiving node does not send");

    // gates: interact pays the price ("Purchased"), toggles active ("gte")
    Object *gate = tm_add(&tm, (vec3) {0.0f, 995.0f, -6.0f}, (vec3) {10.0f, 10.0f, 2.0f});         // 4
    gate->gate = 1;
    gate->score_points = 100;
    game->map->object_count = tm.count;
    place(a, 0.0f, 0.0f);
    a->checkpoint = 0;
    a->score = 50;
    game->now += 1.0f;
    g_sent_count = 0;
    game_player_interact(game, a, 1, 0);
    CHECK(gate->active, "gate: not enough score");
    CHECK(game_interact_prompt(game, a, NULL) == GAME_INTERACT_GATE, "gate prompt");
    a->score = 150;
    game->now += 1.0f;
    game_player_interact(game, a, 1, 0);
    CHECK(!gate->active && a->score == 50 && find_sent(NET_PACKET_MEDAL, 1, 0) && sent_object(NET_OBJECT_GATE, 4), "gate purchased and opened");
    game->now += 1.0f;
    game_player_interact(game, a, 1, 0);
    CHECK(!gate->active, "an open gate stays open unless closeable");

    // deposit boxes: deposit / withdraw (wd / dp amounts, 0 = all)
    gate->gate = 0;
    gate->bank = 1;
    gate->active = 1;
    gate->deposit_amount = 30;
    gate->withdraw_amount = 0;
    gate->deposited = 0;
    game->now += 1.0f;
    game_player_interact(game, a, 1, 0);
    CHECK(gate->deposited == 30 && a->score == 20, "deposit 30 (box %d, score %d)", gate->deposited, a->score);
    game->now += 1.0f;
    game_player_interact(game, a, 1, 1);
    CHECK(gate->deposited == 0 && a->score == 50 && sent_object(NET_OBJECT_BANK, 4), "withdraw all");

    tm_restore(game, &tm);
    a->checkpoint = 0;
    a->checkpoint_list_count = 0;
    a->score = 0;
    player_spawn(a);
    printf("Check point / teleporter / gate test %s\n", g_failures != failures ? "FAILED" : "OK");
}

// map.js / objects.js parsing of the trigger and interface fields, resetAll.
static void test_trigger_map_parse(void) {
    const int failures = g_failures;
    printf("Running trigger map parse test...\n");
    const char *json =
        "{\"name\":\"t\",\"spawns\":[[0,0,0]],\"objects\":["
        "{\"i\":29,\"p\":[0,0,0],\"s\":[10,10,10],\"te\":3,\"ta\":16,\"tct\":\"Welcome!\",\"tc\":5,\"tce\":20000,\"cha\":150,"
        " \"in\":5000,\"tin\":7,\"es\":99,\"ev\":9,\"er\":0.1,\"ef\":1,\"su\":1,\"sfd\":1,\"h\":40,\"tm\":2},"
        "{\"i\":29,\"p\":[20,0,0],\"s\":[10,10,10],\"te\":15,\"tct\":\"<b>no</b>\"},"
        "{\"i\":29,\"p\":[40,0,0],\"s\":[10,10,10],\"te\":22},"
        "{\"i\":0,\"p\":[60,0,0],\"s\":[10,10,10],\"hp\":50,\"sd\":1,\"rt\":0.05,\"in\":3},"
        "{\"i\":24,\"p\":[80,0,0],\"s\":[10,10,10],\"sp\":200,\"md\":2,\"cl\":1,\"stc\":1,\"in\":3},"
        "{\"i\":31,\"p\":[100,0,0],\"s\":[10,10,10],\"wd\":-50},"
        "{\"i\":25,\"p\":[120,0,0],\"s\":[10,10,10],\"su\":1,\"d\":3,\"tm\":1},"
        "{\"i\":27,\"p\":[140,0,0],\"s\":[10,10,10],\"ch\":500,\"n\":2,\"m\":1,\"fd\":1,\"d\":1},"
        "{\"i\":0,\"p\":[160,0,0],\"s\":[10,10,10],\"h\":30,\"l\":1}"
        "]}";
    cJSON *raw = cJSON_Parse(json);
    Map *map = raw ? map_init(raw) : NULL;
    CHECK(map && map->object_count == 9, "parsed %zu objects", map ? map->object_count : 0);
    if (map && map->object_count == 9) {
        const Object *o = map->objects[0];
        const TriggerData *t = o->trig;
        CHECK(t && t->event == TRIGGER_EVENT_ON_ENTER && t->action == TRIGGER_ACTION_BROADCAST_MESSAGE && !strcmp(t->text, "Welcome!") &&
              t->constant == 5 && t->constant_event == 10000 && t->chance == 100 && t->interface_target == 7 && t->sound == 0 &&
              near(t->volume, 2.0f, 1e-6f) && near(t->rate, 0.5f, 1e-6f) && t->emit_from == 1 && t->single_use && t->self_destruct &&
              near(t->timer, 10000000.0f, 1.0f), "TRIGGER fields");
        CHECK(o->interface_id == 1000 && o->health == 40.0f && o->can_destroy && o->team == 2 && o->no_shoot, "TRIGGER interface / health / team / noShoot");
        CHECK(map->objects[1]->trig->event == TRIGGER_EVENT_NONE && !map->objects[1]->trig->text[0], "onAI events and bad texts are dropped");
        CHECK(map->objects[2]->trig->event == TRIGGER_EVENT_NONE && !map->objects[2]->no_shoot, "unknown events name nothing");
        const Object *cube = map->objects[3];
        CHECK(cube->health == 50.0f && cube->can_destroy && cube->start_destroyed && cube->destroyed && !cube->active &&
              cube->respawn_time_start == 100.0f && cube->interface_id == 3, "CUBE destructible fields");
        const Object *gate = map->objects[4];
        CHECK(gate->gate && gate->score_points == 200 && gate->gate_method == 2 && gate->closeable && gate->start_closed && !gate->active &&
              gate->interface_id == 3, "GATE fields");
        CHECK(map->objects[5]->bank && map->objects[5]->withdraw_amount == 50 && map->objects[5]->deposit_amount == 1000, "DEPOSIT_BOX fields");
        CHECK(map->objects[6]->checkpoint && map->objects[6]->single_use && map->objects[6]->direction == 3 && map->objects[6]->team == 1, "CHECK_POINT fields");
        const Object *tel = map->objects[7];
        CHECK(tel->teleporter && tel->channel == 100 && tel->node == 2 && tel->stop_momentum && tel->force_dir && tel->direction == 1, "TELEPORTER fields");
        CHECK(map->objects[8]->health == 0.0f && !map->objects[8]->can_destroy, "noCol objects are not destructible");

        Object *gate_w = map->objects[4], *cube_w = map->objects[3];
        gate_w->active = 1;
        cube_w->active = 1;
        cube_w->destroyed = 0;
        map->objects[0]->trig->triggered = 1;
        map->objects[0]->trig->storage = 4;
        map_reset(map);
        CHECK(!gate_w->active && !cube_w->active && cube_w->destroyed && !map->objects[0]->trig->triggered && !map->objects[0]->trig->storage,
              "resetAll: start state, triggered / storage cleared");
    }
    if (map) map_fini(map);
    cJSON_Delete(raw);
    printf("Trigger map parse test %s\n", g_failures != failures ? "FAILED" : "OK");
}

// players.spray (shared/src/interact.c): the ray skips noVis ("v") objects and lands on the
// rendered wall behind them, on server_bin as well (no meshes: map.c parses "v" itself).
static const SentPacket *spray_once(Game *game, Player *player) {
    g_sent_count = 0;
    player->last_spray = -1e6f;
    game_player_spray(game, player);
    return find_sent(NET_PACKET_SPRAY, NET_TARGET_ALL, 0);
}

static void test_spray_invisible(Game *game) {
    const int failures = g_failures;
    printf("Running spray invisible object test...\n");
    const char *json =
        "{\"name\":\"t\",\"spawns\":[[0,0,0]],\"objects\":["
        "{\"p\":[0,-2,0],\"s\":[200,2,200]},"          // floor
        "{\"p\":[0,0,-10],\"s\":[40,40,2],\"v\":1},"  // invisible wall, front face z = -9 (spray 0.1 before it: -8.9)
        "{\"p\":[0,0,-20],\"s\":[40,40,2]}"            // visible wall, front face z = -19
        "]}";
    cJSON *raw = cJSON_Parse(json);
    Map *map = raw ? map_init(raw) : NULL;
    Player *a = game_find_player(game, 1);
    CHECK(map && map->object_count == 3 && map->objects[1]->no_vis && !map->objects[2]->no_vis, "\"v\" parsed as noVis");
    if (map && map->object_count == 3 && a) {
        Map *saved = game->map;
        game->map = map;
        if (!a->active) player_spawn(a);
        a->position = (vec3) {0.0f, 0.0f, 0.0f};
        a->direction = (vec2) {0.0f, 0.0f}; // pitch 0, yaw 0: facing -z
        a->spray_index = 7;

        const SentPacket *sent = spray_once(game, a);
        NetSprayPacket spray = {0};
        if (sent && sent->length == sizeof(spray)) memcpy(&spray, sent->payload, sizeof(spray));
        CHECK(sent && sent->length == sizeof(spray), "a spray through the invisible wall");
        CHECK(near(spray.position[2], -18.9f, 0.011f) && spray.y_rot == 0 && spray.x_rot == 0 && spray.spray_index == 7 && spray.uid == 1,
              "lands on the visible wall behind (z %g, rot %d/%d)", spray.position[2], spray.y_rot, spray.x_rot);

        map->objects[1]->no_vis = 0; // a rendered wall in front stops the ray
        sent = spray_once(game, a);
        if (sent && sent->length == sizeof(spray)) memcpy(&spray, sent->payload, sizeof(spray));
        CHECK(sent && near(spray.position[2], -8.9f, 0.011f), "a visible wall in front takes the spray (z %g)", spray.position[2]);

        map->objects[1]->no_vis = 1;
        map->objects[2]->no_vis = 1; // only invisible walls ahead (the floor is below the ray): no spray
        CHECK(!spray_once(game, a), "no spray on invisible walls alone");

        game->map = saved;
        player_spawn(a);
    }
    if (map) map_fini(map);
    cJSON_Delete(raw);
    printf("Spray invisible object test %s\n", g_failures != failures ? "FAILED" : "OK");
}

// kills.js reward(): the killer gets "6" (NET_PACKET_KILL_MEDALS) with the [name, score]
// pairs whose sum is the SCORE delta; every 5th kill of a streak broadcasts "kst".
static void test_kill_medals(Game *game) {
    const int failures = g_failures;
    printf("Running kill medals test...\n");

    Player *a = game_find_player(game, 1);
    Player *b = game_find_player(game, 2);
    if (!a->active) player_spawn(a);
    a->kill_streak = 0;
    int streak_seen = 0;
    for (int k = 0; k < 5; k++) {
        if (!b->active) player_spawn(b);
        g_sent_count = 0;
        player_kill(b, a, NULL, 0);
        const SentPacket *medals = find_sent(NET_PACKET_KILL_MEDALS, a->uid, 0);
        const SentPacket *score = find_sent(NET_PACKET_SCORE, a->uid, 0);
        CHECK(medals && score, "kill %d: KILL_MEDALS and SCORE to the killer", k);
        if (medals && score) {
            NetKillMedalsPacket m;
            NetScorePacket sc;
            memcpy(&m, medals->payload, sizeof(m));
            memcpy(&sc, score->payload, sizeof(sc));
            int sum = 0;
            for (int i = 0; i < m.count; i++) sum += m.medals[i].score;
            CHECK(sum == sc.delta, "kill %d: medal scores %d == SCORE delta %d", k, sum, sc.delta);
            CHECK(m.kills == (int) a->kills, "kill %d: medal packet kills", k);
        }
        const SentPacket *kst = find_sent(NET_PACKET_KILL_STREAK, NET_TARGET_ALL, 0);
        if (kst) {
            NetKillStreakPacket p;
            memcpy(&p, kst->payload, sizeof(p));
            CHECK(p.uid == a->uid && p.streak == 5, "kst uid %d streak %d", p.uid, p.streak);
            streak_seen = k + 1;
        }
    }
    CHECK(streak_seen == 5, "kst only on the 5th kill (seen on %d)", streak_seen);
    if (!b->active) player_spawn(b);
    printf("Kill medals test %s\n", g_failures != failures ? "FAILED" : "OK");
}

// GameHooks.reward for the round test: uid 1 has an account (v3.1.1 30 KR cap), uid 2 is a guest.
static int test_round_reward(Game *game, const Player *player) {
    (void) game;
    if (player->uid != 1) return 0;
    const int reward = player->score / 100;
    return reward > 30 ? 30 : reward;
}

// /endless pauses the round clock where it is and resumes from there (no reset, no drift).
static void test_endless(Game *game) {
    const int failures = g_failures;
    printf("Running endless test...\n");

    Player *a = game_find_player(game, 1);
    const int score = a ? a->score : 0;
    game->round_timer = 180.4f;
    game->round_seconds_sent = -1;
    game_tick(game, game->now + 0.1f, 0.1f); // 180.3

    for (int pass = 0; pass < 3; pass++) {
        const float before = game->round_timer;
        g_sent_count = 0;
        game_set_endless(game, 1);
        const SentPacket *timer = find_sent(NET_PACKET_TIMER, NET_TARGET_ALL, 0);
        CHECK(timer != NULL, "endless on: frozen TIMER broadcast");
        if (timer) {
            NetTimerPacket packet;
            memcpy(&packet, timer->payload, sizeof(packet));
            CHECK(packet.seconds == (int) before && !packet.end, "endless TIMER %d (want %d)", packet.seconds, (int) before);
        }
        for (int i = 0; i < 100; i++) game_tick(game, game->now + 0.1f, 0.1f); // 10 s
        CHECK(near(game->round_timer, before, 0.001f), "endless: clock frozen %.2f (was %.2f)", game->round_timer, before);

        game_set_endless(game, 0);
        CHECK(near(game->round_timer, before, 0.001f) && !game->round_endless, "endless off: resumes from %.2f (got %.2f)", before, game->round_timer);
        g_sent_count = 0;
        for (int i = 0; i < 10; i++) game_tick(game, game->now + 0.1f, 0.1f); // 1 s
        CHECK(near(game->round_timer, before - 1.0f, 0.01f), "counting down again: %.2f", game->round_timer);
        CHECK(find_sent(NET_PACKET_TIMER, NET_TARGET_ALL, 0) != NULL, "TIMER resent after resume");
    }
    CHECK(near(game->round_timer, 177.3f, 0.02f), "no drift after 3 toggles: %.2f", game->round_timer);
    CHECK(game->end_timer == 0.0f && (!a || a->score == score), "round and scores untouched");

    printf("Endless test %s\n", g_failures != failures ? "FAILED" : "OK");
}

static void test_round(Game *game) {
    const int failures = g_failures;
    game->hooks.reward = test_round_reward;
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
        CHECK(pa.rewards[0] == 5 && pa.rewards[1] == 0 && pb.rewards[0] == 5, "end reward column %d / %d", pa.rewards[0], pa.rewards[1]);
        CHECK(a->reward == 5 && b->reward == 0, "player.reward %d / %d", a->reward, b->reward);
    }
    game->hooks.reward = NULL;

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
    test_trigger_player_actions(&game);
    test_trigger_interface_actions(&game);
    test_trigger_events(&game);
    test_checkpoints_teleporters(&game);
    test_trigger_map_parse();
    test_spray_invisible(&game);
    test_kill_medals(&game);
    test_endless(&game);
    test_round(&game);
    test_chat();

    if (g_failures) {
        printf("%d check(s) failed\n", g_failures);
        return 1;
    }

    printf("Test OK\n");
    return 0;
}
