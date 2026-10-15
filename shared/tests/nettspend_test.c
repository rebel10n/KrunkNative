// KrunkNative Nettspend (doc/v7/decisions.md "Nettspend class", "Purple Drank", "Money Spread";
// shared/include/drank.h) through procInputs frames (player_proc_input) on an authority game, the
// way server_bin and the offline sandbox run it:
//   the tables (ids 67 / 68 after the Physics Gun, class 16 after Krunker's, flags, Object Manager
//   tiles and `give` names); a Nettspend spawn carries Drank + Money Spread + knife in every lobby
//   (custom, public, comp) and bots may play it, in every KrunkNative lobby type and mode, and is
//   refused in an official krunker.io game (game_class_allowed, Game.is_official); the Drank's stream (60 ms, 4 a
//   hit) and purple puddles (10 dps, 6 s) that slow the players standing in them (x0.55, not the
//   owner, replicated to a client's copy); the Money Spread's 8 bills a throw fading with
//   distance; the pour-up's laced roll (4% a reload on the seeded pcg32 stream: deterministic,
//   NET_EXT_KN_LACED to everyone, 8 s on both sides, never rolled off the authority).
// Run from the repository root: ./<build>/nettspend_test
#include <shared.h>
#include <object_manager.h>
#include <drank.h>
#include <bots.h>
#include <net_ext.h>
#include <piss.h>
#include <game_config.h>
#include <pcg_basic.h>
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

static int g_seq;
static int g_shots, g_drank_shots, g_laced, g_laced_all;
static NetLacedPacket g_last_laced;
static Game *g_client; // receives every SPAWNED_OBJ / EXT the authority sends (NULL: none)

static void test_send(Game *game, const int uid, const int type, const void *payload, const uint16_t length) {
    (void) game;
    if (type == NET_PACKET_SHOT && length == sizeof(NetShotPacket)) {
        g_shots++;
        if (((const NetShotPacket *) payload)->weapon_index == WEAPON_ID_DRANK) g_drank_shots++;
    }
    if (type == NET_PACKET_EXT) {
        uint16_t subtype, body_length;
        const uint8_t *body;
        if (!net_ext_unpack(payload, length, &subtype, &body, &body_length) || subtype != NET_EXT_KN_LACED) return;
        g_laced++;
        g_laced_all += uid == NET_TARGET_ALL;
        if (body_length == sizeof(g_last_laced)) memcpy(&g_last_laced, body, sizeof(g_last_laced));
        if (g_client) drank_apply_laced(g_client, body, body_length);
        return;
    }
    if (type != NET_PACKET_SPAWNED_OBJ || length < sizeof(NetSpawnedObjPacket)) return;
    if (g_client) object_manager_apply_spawned(g_client, payload, length);
}

static Player *add_player(Game *game, const int uid, const int class_index) {
    Player *player = player_init(game);
    player->uid = uid;
    player->class_index = class_index;
    snprintf(player->name, sizeof(player->name), "p%d", uid);
    game_players_add(game, player);
    player_spawn(player);
    player->max_health = 100000;
    player->health = 100000.0f;
    return player;
}

static void stand(Player *player, const float x, const float z, const float pitch) {
    player->position = (vec3) {x, 0.0f, z};
    player->last_position = player->position;
    player->velocity = (vec3) {0.0f, 0.0f, 0.0f};
    player->direction = (vec2) {pitch, 0.0f}; // yaw 0: facing -z
    player->on_ground = 1;
    player->recoil_anim_y = 0.0f;
    player->recoil_force = 0.0f;
}

// `frames` 10 ms procInputs frames holding fire (or not), moving in `move_dir` (-1 still); the shots fired
static int frames(Player *player, const int count, const int shoot, const int move_dir) {
    int shots = 0;
    for (int f = 0; f < count; f++) {
        const int slot = player->loadout_index;
        const float before = player->reloads[slot];
        const Input input = {.seq = ++g_seq, .delta = DT, .move_dir = move_dir, .shoot = (unsigned char) shoot,
                             .x_dir = player->direction.x, .y_dir = player->direction.y};
        player_proc_input(player, &input, 0, 0);
        if (player->reloads[slot] > before + 1e-6f) shots++;
    }
    return shots;
}

static int slot_of(const Player *player, const int weapon_id) {
    for (int i = 0; i < player->loadout_size; i++)
        if (player->loadout[i] == weapon_id) return i;
    return -1;
}

static void hold(Player *player, const int weapon_id) {
    const int slot = slot_of(player, weapon_id);
    if (slot < 0) return;
    player_swap_weapon(player, slot, 1, 1, 0);
    player->swap_timer = 0.0f;
    player->reloads[slot] = 0.0f;
    player->reload_timer = 0.0f;
}

static void test_tables(const Game *game) {
    printf("Tables: the Drank, the Money Spread and the Nettspend after Krunker's...\n");
    const Weapon *d = rules_weapon(WEAPON_ID_DRANK), *m = rules_weapon(WEAPON_ID_MONEY_SPREAD);
    CHECK(WEAPON_ID_DRANK == 67 && WEAPON_ID_MONEY_SPREAD == 68 && WEAPON_TABLE_COUNT == 69, "ids 67 / 68 after the Physics Gun (table %d)", WEAPON_TABLE_COUNT);
    CHECK(d == &g_drank && m == &g_money_spread && game->weapons[WEAPON_ID_DRANK] == d && game->weapons[WEAPON_ID_MONEY_SPREAD] == m,
          "in the ruleset's and the game's weapon tables");
    CHECK(d && !strcmp(d->name, "Purple Drank") && d->stream && d->custom_only && d->no_comp && !d->secondary && d->type == 0, "the Drank: a customOnly / noComp primary with a stream");
    CHECK(d && d->stream->color == 0x7b2cbf && d->stream->puddle_slow == 0.55f && d->stream->laced_chance == 0.04f && d->stream->laced_ms == 8000.0f,
          "purple, puddle slow x0.55, laced 4%% for 8 s");
    CHECK(d && d->ammo == 40 && fabsf(d->rate - 0.06f) < 1e-6f && fabsf(d->reload_time - 1.4f) < 1e-6f && d->damage == 4.0f, "40 sips, 60 ms, a 1.4 s pour-up, 4 a sip");
    CHECK(d && d->stream->puddle_dps == 10.0f && d->stream->puddle_life == 6000.0f && d->stream->puddle_radius == 3.0f, "puddles 10 dps, 6 s, radius 3");
    CHECK(m && !strcmp(m->name, "Money Spread") && m->secondary && m->type == 1 && m->custom_only && m->no_comp && !m->stream, "the Money Spread: a customOnly / noComp secondary");
    CHECK(m && m->shots == 8 && m->custom_spread_count == 8 && m->damage == 7.0f && m->damage_drop == 7.0f && m->range == 60.0f && m->ammo == 5,
          "8 bills of 7, fading to 0 at 60, 5 a magazine");
    CHECK(d && m && !d->src && !m->src && !d->tint && !m->tint, "no stand-in models or tints (the client draws the cup and the cash)");
    CHECK(!rules_weapon(WEAPON_ID_PISS)->stream->puddle_slow && !rules_weapon(WEAPON_ID_PISS)->stream->laced_chance, "the Piss neither slows nor laces");
    CHECK(object_manager_weapon_listed(WEAPON_ID_DRANK) && object_manager_weapon_listed(WEAPON_ID_MONEY_SPREAD) &&
          object_manager_weapon_allowed(WEAPON_ID_DRANK) && object_manager_weapon_allowed(WEAPON_ID_MONEY_SPREAD), "Object Manager Weapons tiles");
    int id = -1;
    CHECK(object_manager_give_lookup("drank", &id, NULL) == OM_GIVE_WEAPON && id == WEAPON_ID_DRANK, "`give drank` (%d)", id);
    CHECK(object_manager_give_lookup("lean", &id, NULL) == OM_GIVE_WEAPON && id == WEAPON_ID_DRANK, "`give lean` (%d)", id);
    CHECK(object_manager_give_lookup("purple", &id, NULL) == OM_GIVE_WEAPON && id == WEAPON_ID_DRANK, "`give purple` (%d)", id);
    CHECK(object_manager_give_lookup("money", &id, NULL) == OM_GIVE_WEAPON && id == WEAPON_ID_MONEY_SPREAD, "`give money` (%d)", id);
    CHECK(object_manager_give_lookup("cash", &id, NULL) == OM_GIVE_WEAPON && id == WEAPON_ID_MONEY_SPREAD, "`give cash` (%d)", id);
    CHECK(object_manager_give_lookup("68", &id, NULL) == OM_GIVE_WEAPON && id == WEAPON_ID_MONEY_SPREAD, "`give 68` (%d)", id);

    const ClassConfig *classes = rules_classes();
    CHECK(CLASS_COUNT == 16 && CLASS_ID_NETTSPEND == 16 && CLASS_TABLE_COUNT == 17 && rules_class_count() == CLASS_TABLE_COUNT && game->class_count == CLASS_TABLE_COUNT,
          "class 16 after Krunker's 16 (table %zu, game %zu)", rules_class_count(), game->class_count);
    const ClassConfig *n = &classes[CLASS_ID_NETTSPEND];
    CHECK(!strcmp(n->name, "Nettspend") && n->icon_index == 16 && n->loadout && n->loadout[0] == WEAPON_ID_DRANK && n->secondary &&
          n->kn_secondary == WEAPON_ID_MONEY_SPREAD + 1, "the Nettspend: Drank, always the Money Spread, icon 16");
    CHECK(!n->hide && n->health == 100 && n->speed == 1.05f && !n->wall_jump, "open to everyone, a Triggerman baseline");
    int others = 0;
    for (int c = 0; c < CLASS_COUNT; c++) others |= classes[c].kn_secondary;
    CHECK(!others, "Krunker's classes are untouched");
    CHECK(!strcmp(game->classes[CLASS_ID_NETTSPEND].name, "Nettspend"), "in the game's class table");
}

static int nettspend_loadout_ok(const Player *p) {
    return p->class_index == CLASS_ID_NETTSPEND && p->loadout_size == 3 && p->loadout[0] == WEAPON_ID_DRANK && p->loadout[1] == WEAPON_ID_MONEY_SPREAD &&
           p->loadout[2] == WEAPON_ID_KNIFE;
}

// A mode that sets the spawn's class itself (forceC with noWeap / forceClass / teamClass): the spawn's
// class is the mode's, so the spawn check is for the pick only.
static int mode_forces_class(const Game *game) {
    if (!game->mode || !game->config.force_classes) return 0;
    const GameModeConfig *m = &game->mode->config;
    if (m->no_weapons || m->force_class >= 0) return 1;
    for (int t = 0; t < MODE_TEAMS; t++) if (m->team_class[t] >= 0) return 1;
    return 0;
}

// The gating rule (game_config.h game_class_allowed): the Nettspend is playable in every KrunkNative
// lobby (public, custom, comp, ranked, rent, zombies, raid, the offline sandbox) in every mode, and is
// NOT in an official krunker.io game (Game.is_official), where a spawn falls back to the Triggerman.
static void test_gating(const cJSON **maps) {
    printf("Gating: the Nettspend in every KrunkNative lobby and mode, never in an official krunker.io game...\n");
    static const struct {
        const char *name;
        int host, comp, ranked, sandbox;
    } lobbies[] = {
        {"public", -1, 0, 0, 0}, {"custom", GAME_HOST_CUSTOM, 0, 0, 0}, {"comp", GAME_HOST_COMP, 1, 0, 0},
        {"ranked", GAME_HOST_COMP, 1, 1, 0}, {"rent", GAME_HOST_RENT, 0, 0, 0}, {"zombies", GAME_HOST_ZOMBIES, 0, 0, 0},
        {"raid", GAME_HOST_RAID, 0, 0, 0}, {"sandbox", GAME_HOST_CUSTOM, 0, 0, 1},
    };
    int lobby_checks = 0, lobby_bad = 0, official_checks = 0, official_bad = 0, spawn_checks = 0, spawn_bad = 0, skipped = 0;
    unsigned char played[MODE_COUNT] = {0};
    const int modes = mode_count(); // every Krunker mode id, each game's whole mode list (game_init picks the one)
    for (size_t l = 0; l < sizeof(lobbies) / sizeof(lobbies[0]); l++) {
        for (int mode = 0; mode < modes; mode++) {
            Game *game = calloc(1, sizeof(Game));
            int *list = malloc((size_t) modes * sizeof(int)); // the game owns it (game_configure / game_add_mode)
            if (!game || !list) {
                free(game);
                free(list);
                continue;
            }
            for (int m = 0; m < modes; m++) list[m] = m;
            game_configure(game, NULL, maps, 1, list, (size_t) modes, NULL, 0, NULL, 0);
            game->hooks.send = test_send;
            game->lag_comp = 0;
            if (lobbies[l].host >= 0) game_config_host(game, NULL, lobbies[l].host);
            game->config.comp = (unsigned char) lobbies[l].comp;
            game->is_ranked = (unsigned char) lobbies[l].ranked;
            game->is_sandbox = (unsigned char) lobbies[l].sandbox;
            game_init(game, 0, mode, 1);
            if (!game->ready) {
                skipped++;
                free(game);
                continue;
            }
            if (game_mode_id(game) >= 0 && game_mode_id(game) < MODE_COUNT) played[game_mode_id(game)] = 1;
            // the lobby's own rule: KrunkNative's class is playable here
            lobby_checks++;
            if (!game_class_allowed(game, CLASS_ID_NETTSPEND)) lobby_bad++;
            if (!mode_forces_class(game)) {
                spawn_checks++;
                Player *p = add_player(game, 1, CLASS_ID_NETTSPEND);
                // the class and its Drank + Money Spread (+ the knife, unless the mode is noMelee: Classic)
                const int knife = !mode_no_melee(game);
                if (p->class_index != CLASS_ID_NETTSPEND || p->loadout_size != 2 + knife || p->loadout[0] != WEAPON_ID_DRANK ||
                    p->loadout[1] != WEAPON_ID_MONEY_SPREAD || (knife && p->loadout[2] != WEAPON_ID_KNIFE)) spawn_bad++;
            }
            // the same lobby as an official krunker.io game: out, and a spawn falls back to the Triggerman
            game->is_official = 1;
            official_checks++;
            if (game_class_allowed(game, CLASS_ID_NETTSPEND) || !game_class_allowed(game, 0)) official_bad++;
            if (!mode_forces_class(game)) {
                Player *q = add_player(game, 2, CLASS_ID_NETTSPEND);
                if (q->class_index != 0) official_bad++;
            }
            free(game);
        }
    }
    int distinct = 0;
    for (int m = 0; m < MODE_COUNT; m++) distinct += played[m];
    printf("  %d lobby x mode games (%d not ready), %d of %d modes played, %d spawn checks, %d official games\n", lobby_checks, skipped, distinct,
           modes, spawn_checks, official_checks);
    CHECK(distinct == modes, "every mode was played (%d of %d)", distinct, modes);
    CHECK(lobby_checks > 0 && lobby_bad == 0, "KrunkNative lobbies: the Nettspend allowed in all %d lobby x mode games (%d not allowed, %d skipped)",
          lobby_checks, lobby_bad, skipped);
    CHECK(spawn_checks > 0 && spawn_bad == 0, "a Nettspend spawn keeps the class and its loadout in %d games (%d not)", spawn_checks, spawn_bad);
    CHECK(official_checks > 0 && official_bad == 0, "official krunker.io games: the Nettspend refused in all %d games (%d not), the Triggerman spawns",
          official_checks, official_bad);
    Game plain = {0};
    game_configure(&plain, NULL, maps, 1, NULL, 0, NULL, 0, NULL, 0);
    CHECK(game_class_allowed(&plain, CLASS_ID_NETTSPEND) && !game_class_allowed(&plain, CLASS_TABLE_COUNT) && game_class_allowed(&plain, 0),
          "game_class_allowed: Krunker's classes always, the Nettspend in a plain game, nothing past the table");
}

static void test_class_spawn(Game *custom, Game *pub) {
    printf("Spawns: Drank + Money Spread + knife in every lobby (custom, public, comp)...\n");
    Player *p = add_player(custom, 10, CLASS_ID_NETTSPEND);
    p->secondary_index = 15; // a Sawed Off pick changes nothing
    player_spawn(p);
    CHECK(nettspend_loadout_ok(p), "custom: %d weapons (%d %d %d)", p->loadout_size, p->loadout[0], p->loadout[1], p->loadout[2]);
    CHECK(p->weapon == &g_drank && p->ammo[0] == 40 && p->ammo[1] == 5 && p->max_health == 100, "holds the Drank (40), the Money Spread (5), 100 health");

    custom->config.comp = 1; // comp: the class and its bound Money Spread stay (noComp gates picks, not the class's own)
    player_spawn(p);
    CHECK(nettspend_loadout_ok(p), "comp: still the Nettspend (class %d, %d %d)", p->class_index, p->loadout[0], p->loadout[1]);
    custom->config.comp = 0;
    stand(p, 500.0f, 500.0f, 0.0f); // out of the way of the stream tests

    Player *q = add_player(pub, 11, CLASS_ID_NETTSPEND);
    CHECK(nettspend_loadout_ok(q), "a public game: the Nettspend too (class %d, %d %d)", q->class_index, q->loadout[0], q->loadout_size > 1 ? q->loadout[1] : -1);
    Player *r = add_player(pub, 12, 0); // a Triggerman picking the Money Spread in a public game: still the customOnly gate
    r->secondary_index = WEAPON_ID_MONEY_SPREAD;
    player_spawn(r);
    CHECK(r->class_index == 0 && slot_of(r, WEAPON_ID_MONEY_SPREAD) < 0, "another class can't pick the Money Spread in public (secondary %d)", r->loadout[1]);

    // bots: a forced Nettspend bot keeps it, and the random class roll reaches it
    BotAssignments a;
    char *argv[] = {"class=16", "roam=off"};
    bot_parse_assignments(2, argv, &a);
    GameBot *bot = !a.error[0] ? game_bots_add(pub, &a, NULL, 0.0f) : NULL;
    CHECK(bot && bot->class_index == CLASS_ID_NETTSPEND, "a class=16 bot plays the Nettspend (%d, %s)", bot ? bot->class_index : -2, a.error);
    game_bots_remove_all(pub);
    char *argv2[] = {"roam=off"};
    bot_parse_assignments(1, argv2, &a);
    int rolled = 0;
    for (int i = 0; i < 400 && !rolled; i++) {
        GameBot *b = game_bots_add(pub, &a, NULL, 0.0f);
        rolled = b && b->class_index == CLASS_ID_NETTSPEND;
        game_bots_remove_all(pub);
    }
    CHECK(rolled, "the bots' random class roll includes the Nettspend");
}

static void test_drank_stream(Game *game, Player *p, Player *target) {
    printf("Drank stream: 60 ms sips of 4, a purple puddle where it lands...\n");
    hold(p, WEAPON_ID_DRANK);
    stand(p, 0.0f, 0.0f, 0.0f);
    stand(target, 0.0f, -15.0f, 0.0f);
    for (int i = 0; i < 20; i++) player_store_state(target);
    piss_round_start(game);
    g_drank_shots = 0;
    const float health = target->health;
    const unsigned int ammo = p->ammo[p->loadout_index];
    const int shots = frames(p, 100, 1, -1); // 1 s
    const float dealt = health - target->health;
    printf("  %d sips in 1 s, %.1f damage, %d SHOT packets, %d puddles\n", shots, dealt, g_drank_shots, game->puddles.count);
    CHECK(shots >= 15 && shots <= 18, "a sip every 60 ms (%d in a second)", shots);
    CHECK(fabsf(dealt - 4.0f * (float) shots) < 1e-3f, "4 a sip (%.1f for %d)", dealt, shots);
    CHECK(g_drank_shots == shots && ammo - p->ammo[p->loadout_index] == (unsigned int) shots, "a SHOT each, a sip of ammo each");
    CHECK(game->puddles.count == 0, "a player hit leaves no puddle");
    frames(p, 20, 0, -1);

    stand(target, 300.0f, 300.0f, 0.0f);
    stand(p, 0.0f, 0.0f, -0.4f);
    frames(p, 6, 1, -1);
    CHECK(game->puddles.count == 1, "a sip on the floor: a puddle (%d)", game->puddles.count);
    if (game->puddles.count) {
        const GamePuddle *puddle = &game->puddles.items[0];
        CHECK(puddle->color == 0x7b2cbf && puddle->weapon_id == WEAPON_ID_DRANK && puddle->radius == 3.0f && puddle->owner == p->uid, "purple, the Drank's, radius 3");
    }
    frames(p, 20, 0, -1);
}

static void test_puddle_slow(Game *game, Player *p, Player *target, Player *third, Game *client, Player *client_target) {
    printf("Drank puddle: 10 dps, and the players in it move at x0.55...\n");
    piss_round_start(game);
    piss_round_start(client);
    g_client = client;
    const vec3 at = {50.0f, PISS_PUDDLE_LIFT, 50.0f};
    const int id = piss_puddle_spawn(game, p, WEAPON_ID_DRANK, at);
    CHECK(id >= 0 && game->puddles.count == 1 && client->puddles.count == 1, "spawned on both sides (%d / %d)", game->puddles.count, client->puddles.count);
    stand(target, 51.0f, 51.0f, 0.0f);
    stand(p, 50.0f, 49.0f, 0.0f);
    stand(third, 50.0f, 50.0f, 0.0f);
    third->position.y = 3.0f;
    CHECK(drank_puddle_slow(game, target) == 0.55f, "a victim in it: x0.55 (%.2f)", drank_puddle_slow(game, target));
    CHECK(drank_puddle_slow(game, p) == 1.0f, "not its owner");
    CHECK(drank_puddle_slow(game, third) == 1.0f, "not 3 units above it");
    stand(client_target, 51.0f, 51.0f, 0.0f);
    CHECK(drank_puddle_slow(client, client_target) == 0.55f, "the client's copy slows the same (%.2f)", drank_puddle_slow(client, client_target));
    third->position.y = 0.0f;
    stand(third, 200.0f, 200.0f, 0.0f);

    // the damage: 2.5 a 250 ms tick
    const float t0 = target->health;
    for (int i = 0; i < 20; i++) piss_tick(game, 0.05f);
    CHECK(fabsf(t0 - target->health - 10.0f) < 1e-3f, "10 in a second (%.2f)", t0 - target->health);

    // the slow: the same short run (6 frames: still inside the 3-unit puddle) with and without it underfoot
    hold(target, WEAPON_ID_KNIFE);
    stand(target, 50.0f, 50.0f, 0.0f);
    frames(target, 6, 0, 0);
    const vec3 v_slow = target->velocity;
    const float slow_speed = sqrtf(v_slow.x * v_slow.x + v_slow.z * v_slow.z);
    const float slow_moved = sqrtf((target->position.x - 50.0f) * (target->position.x - 50.0f) + (target->position.z - 50.0f) * (target->position.z - 50.0f));
    piss_round_start(game); // no puddle
    stand(target, 50.0f, 50.0f, 0.0f);
    frames(target, 6, 0, 0);
    const vec3 v_free = target->velocity;
    const float free_speed = sqrtf(v_free.x * v_free.x + v_free.z * v_free.z);
    const float free_moved = sqrtf((target->position.x - 50.0f) * (target->position.x - 50.0f) + (target->position.z - 50.0f) * (target->position.z - 50.0f));
    printf("  60 ms of running: %.3f units at %.4f in the puddle, %.3f at %.4f without\n", slow_moved, slow_speed, free_moved, free_speed);
    CHECK(free_moved > 0.1f && free_moved < 3.0f && slow_moved > 0.0f && slow_moved < free_moved * 0.7f, "slower in the puddle (%.2f of the free run)", slow_moved / free_moved);
    CHECK(slow_speed < free_speed * 0.65f && slow_speed > free_speed * 0.45f, "about x0.55 the speed (%.2f)", slow_speed / free_speed);

    // dries up at 6 s
    piss_puddle_spawn(game, p, WEAPON_ID_DRANK, at);
    int alive = 1;
    float t = 0.0f;
    while (alive && t < 10.0f) {
        piss_tick(game, 0.05f);
        t += 0.05f;
        alive = game->puddles.count > 0;
    }
    CHECK(!alive && fabsf(t - 6.0f) < 0.06f, "lives 6 s (%.2f)", t);
    CHECK(client->puddles.count == 0, "gone on the client too");
    g_client = NULL;
}

static void test_money_spread(Game *game, Player *p, Player *target) {
    printf("Money Spread: 8 bills a throw, 7 each fading with distance...\n");
    hold(p, WEAPON_ID_MONEY_SPREAD);
    CHECK(p->weapon == &g_money_spread && p->ammo[p->loadout_index] == 5, "holds the Money Spread (5 throws)");
    stand(p, 0.0f, 0.0f, 0.0f);
    stand(target, 0.0f, -8.0f, 0.0f);
    for (int i = 0; i < 20; i++) player_store_state(target);
    const float health = target->health;
    const int shots_before = p->life_shots, hits_before = p->life_hits;
    const int throws = frames(p, 1, 1, -1);
    const int bills = p->life_shots - shots_before, hits = p->life_hits - hits_before;
    const float dealt = health - target->health;
    printf("  %d throw: %d bills, %d hit at 8 units for %.2f (%.2f a bill)\n", throws, bills, hits, dealt, hits ? dealt / (float) hits : 0.0f);
    CHECK(throws == 1 && bills == 8, "one throw of 8 bills (%d, %d)", throws, bills);
    CHECK(hits >= 2 && hits <= 8, "a fan: some of them hit a player 8 units away (%d)", hits);
    CHECK(hits && dealt / (float) hits > 5.6f && dealt / (float) hits < 6.6f, "7 less the drop at 8 of 60 units (%.2f a bill)", hits ? dealt / (float) hits : 0.0f);
    CHECK(p->ammo[p->loadout_index] == 4 && fabsf(p->reloads[p->loadout_index] - 0.42f) < 1e-3f, "a throw of ammo, the next in 420 ms");
    frames(p, 2, 1, -1);
    CHECK(p->ammo[p->loadout_index] == 4, "noAuto: holding fire throws once");
    frames(p, 50, 0, -1);

    stand(target, 0.0f, -70.0f, 0.0f);
    for (int i = 0; i < 20; i++) player_store_state(target);
    const float far = target->health;
    frames(p, 1, 1, -1);
    CHECK(target->health == far, "nothing past 60 units");
    frames(p, 50, 0, -1);
}

static int pour(Player *p) {
    p->ammo[p->loadout_index] = 0;
    p->reload_timer = 0.0f;
    const int before = g_laced;
    player_reload(p);
    return g_laced > before;
}

static void test_laced(Game *game, Player *p, Game *client, Player *client_p) {
    printf("Laced: 4%% of the pour-ups, seeded, 8 s on both sides...\n");
    hold(p, WEAPON_ID_DRANK);
    g_client = client;
    g_laced = g_laced_all = 0;
    enum { POURS = 2000 };
    static unsigned char first[POURS], again[POURS];
    pcg32_srandom(42, 54);
    int laced = 0, timer_ok = 1, packet_ok = 1, client_ok = 1;
    for (int i = 0; i < POURS; i++) {
        p->laced_timer = p->laced_duration = 0.0f;
        client_p->laced_timer = 0.0f;
        first[i] = (unsigned char) pour(p);
        if (!first[i]) {
            timer_ok &= p->laced_timer == 0.0f && !drank_is_laced(p);
            continue;
        }
        laced++;
        timer_ok &= p->laced_timer == 8.0f && p->laced_duration == 8.0f && drank_is_laced(p);
        packet_ok &= g_last_laced.uid == p->uid && g_last_laced.duration_ms == 8000;
        client_ok &= client_p->laced_timer == 8.0f && client_p->laced_duration == 8.0f;
    }
    printf("  %d of %d pour-ups laced (%.1f%%)\n", laced, POURS, 100.0f * (float) laced / (float) POURS);
    CHECK(laced >= 45 && laced <= 120, "about 4%% (%d of %d)", laced, POURS);
    CHECK(timer_ok, "laced_timer 8 s on a hit, 0 otherwise");
    CHECK(packet_ok && g_laced == laced && g_laced_all == laced, "NET_EXT_KN_LACED {uid, 8000} to everyone each time (%d packets)", g_laced);
    CHECK(client_ok, "the client's copy of the player is laced for 8 s");
    CHECK(fabsf(p->reload_timer - 1.4f) < 1e-3f, "the pour-up takes 1.4 s");

    pcg32_srandom(42, 54);
    int same = 1;
    for (int i = 0; i < POURS; i++) {
        again[i] = (unsigned char) pour(p);
        same &= again[i] == first[i];
    }
    CHECK(same, "the same seed rolls the same pour-ups");

    // ticks down on both sides; the spell restarts on a new hit; a spawn sobers up
    p->laced_timer = p->laced_duration = 8.0f;
    client_p->laced_timer = client_p->laced_duration = 8.0f;
    for (int i = 0; i < 30; i++) {
        game_tick(game, game->now + 0.1f, 0.1f);
        game_tick(client, client->now + 0.1f, 0.1f);
    }
    CHECK(fabsf(p->laced_timer - 5.0f) < 0.05f && fabsf(client_p->laced_timer - 5.0f) < 0.05f, "3 s later: 5 s left on both (%.2f / %.2f)", p->laced_timer, client_p->laced_timer);
    for (int i = 0; i < 60; i++) {
        game_tick(game, game->now + 0.1f, 0.1f);
        game_tick(client, client->now + 0.1f, 0.1f);
    }
    CHECK(p->laced_timer == 0.0f && !drank_is_laced(p) && client_p->laced_timer == 0.0f && !p->laced_duration, "sober after 8 s");
    p->laced_timer = p->laced_duration = 8.0f;
    player_spawn(p);
    CHECK(p->laced_timer == 0.0f, "a new life is sober");

    // a bad body, a stranger, the authority: ignored
    const NetLacedPacket bogus = {.uid = 999, .duration_ms = 8000};
    CHECK(!drank_apply_laced(client, &bogus, sizeof(bogus)) && !drank_apply_laced(client, &bogus, 3), "unknown uid / short body ignored");
    const NetLacedPacket mine = {.uid = p->uid, .duration_ms = 8000};
    CHECK(!drank_apply_laced(game, &mine, sizeof(mine)) && p->laced_timer == 0.0f, "the authority takes no LACED");

    // no roll off the authority: a client's predicted reload never laces
    hold(client_p, WEAPON_ID_DRANK);
    pcg32_srandom(42, 54);
    int client_laced = 0;
    for (int i = 0; i < POURS; i++) {
        client_p->ammo[client_p->loadout_index] = 0;
        client_p->reload_timer = 0.0f;
        player_reload(client_p);
        client_laced += client_p->laced_timer > 0.0f;
    }
    CHECK(!client_laced, "a client's own reloads never lace (%d)", client_laced);
    g_client = NULL;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    // a floor (top at y 0) and a wall whose face is at z = -60
    cJSON *map = cJSON_Parse("{\"name\":\"nettspend\",\"spawns\":[[0,0,0],[0,0,-15]],\"objects\":["
                             "{\"p\":[0,-10,0],\"s\":[1200,10,1200]},"
                             "{\"p\":[0,0,-65],\"s\":[600,300,10]}]}");
    static const cJSON *maps[1];
    maps[0] = map;
    Game game = {0};
    game_configure(&game, NULL, maps, 1, NULL, 0, NULL, 0, NULL, 0);
    game_init(&game, 0, -1, 1);
    game.hooks.send = test_send;
    game.lag_comp = 0;
    game_config_host(&game, NULL, GAME_HOST_CUSTOM); // a custom game (the sandbox / a hosted lobby)

    Game pub = {0}; // a public game: no KN class
    game_configure(&pub, NULL, maps, 1, NULL, 0, NULL, 0, NULL, 0);
    game_init(&pub, 0, -1, 1);
    pub.hooks.send = test_send;

    Game client = {0};
    game_configure(&client, NULL, maps, 1, NULL, 0, NULL, 0, NULL, 0);
    game_init(&client, 0, -1, 0);
    client.ready = 1;
    game_config_host(&client, NULL, GAME_HOST_CUSTOM);

    test_tables(&game);
    test_class_spawn(&game, &pub);
    test_gating(maps);

    Player *p = add_player(&game, 1, CLASS_ID_NETTSPEND);
    Player *target = add_player(&game, 2, 0);
    Player *third = add_player(&game, 3, 0);
    stand(third, 40.0f, 0.0f, 0.0f);
    CHECK(p->weapon == &g_drank && slot_of(p, WEAPON_ID_MONEY_SPREAD) == 1, "the Nettspend spawned with the Drank and the Money Spread");
    Player *client_p = add_player(&client, 1, CLASS_ID_NETTSPEND); // the client's copies (the same uids)
    Player *client_target = add_player(&client, 2, 0);
    add_player(&client, 3, 0);

    test_drank_stream(&game, p, target);
    test_puddle_slow(&game, p, target, third, &client, client_target);
    test_money_spread(&game, p, target);
    test_laced(&game, p, &client, client_p);

    cJSON_Delete(map);
    if (g_failures) {
        printf("nettspend_test: %d of %d checks failed\n", g_failures, g_checks);
        return 1;
    }
    printf("nettspend_test: %d checks OK\n", g_checks);
    return 0;
}
