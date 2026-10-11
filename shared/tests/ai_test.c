// Krunker v3.1.1 AI (shared/src/ai.c) through the shared game hooks: Soul Sanctum's 20 map
// BOTs with objects.js limits, the built-in kinds, chasing / melee / shooting, damage from a
// player's bullets and explosions (killScore, "4" with isAI), respawn, the AI.sync packets,
// Traitor ghosts, the Raid boss condition and the /summon /ais /kill chat commands.
// Run from the repository root: ./<build>/ai_test
#include <shared.h>
#include <ai.h>
#include <chat_commands.h>
#include <map_catalog.h>
#include "../src/modes/mode_internal.h" // mode_cond_raid
#include <math.h>
#include <stdio.h>
#include <string.h>

void player_shoot(Player*); // shared/src/player.c

#define MAX_SENT 2048

typedef struct {
    int uid, type;
    uint16_t length;
    uint8_t payload[NET_MAX_PAYLOAD];
} SentPacket;

static SentPacket g_sent[MAX_SENT];
static int g_sent_count;
static int g_failures;
static char g_last_reply[2048];

#define CHECK(cond, ...) do { \
    if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_failures++; } \
} while (0)

static void test_send(Game *game, const int uid, const int type, const void *payload, const uint16_t length) {
    (void) game;
    if (type == NET_PACKET_CHAT && length >= sizeof(NetChatPacket) - NET_CHAT_TEXT_SIZE) {
        const NetChatPacket *chat = payload;
        snprintf(g_last_reply, sizeof(g_last_reply), "%s", chat->text);
    }
    if (g_sent_count >= MAX_SENT) return;
    SentPacket *packet = &g_sent[g_sent_count++];
    packet->uid = uid;
    packet->type = type;
    packet->length = length;
    memcpy(packet->payload, payload, length);
}

static int count_sent(const int type, const int uid) {
    int n = 0;
    for (int i = 0; i < g_sent_count; i++) n += g_sent[i].type == type && (uid == -100 || g_sent[i].uid == uid);
    return n;
}

static const SentPacket *last_sent(const int type) {
    for (int i = g_sent_count - 1; i >= 0; i--) {
        if (g_sent[i].type == type) return &g_sent[i];
    }
    return NULL;
}

static Player *add_player(Game *game, const int uid, const char *name) {
    Player *player = player_init(game);
    player->uid = uid;
    player->class_index = 0;
    snprintf(player->name, sizeof(player->name), "%s", name);
    game_players_add(game, player);
    player_spawn(player);
    return player;
}

static void place(Player *player, const float x, const float y, const float z) {
    player->position = (vec3) {x, y, z};
    player->last_position = player->position;
    player->velocity = (vec3) {0};
    player->direction = (vec2) {0};
    player->spread = 0.0f;
    player->recoil_anim_y = 0.0f;
}

static void tick_ai(Game *game, const float ms) {
    for (float t = 0.0f; t < ms; t += 1000.0f / 60.0f) game_ai_tick(game, 1000.0f / 60.0f);
}

// ---------------------------------------------------------------- Soul Sanctum BOTs

static void test_map_bots(Game *game) {
    printf("Running Soul Sanctum BOT test...\n");
    CHECK(game->ai && game->ai->count == 20, "20 map AIs (aiLimit), got %d", game->ai ? game->ai->count : -1);
    for (size_t i = 0; i < game->map->object_count; i++) CHECK(game->map->objects[i]->prefab != PREFAB_BOT, "BOT %zu is a map object", i);
    if (!game->ai || game->ai->count < 12) return;

    const GameAI *weeping = &game->ai->ais[0];
    CHECK(!strcmp(weeping->name, "Weeping Soul"), "first AI %s", weeping->name);
    CHECK(weeping->sid == 1, "generateSID starts at 1 (%d)", weeping->sid);
    CHECK(weeping->max_health == 10000.0f && weeping->health == 10000.0f, "Weeping Soul health %.0f", weeping->health);
    CHECK(fabsf(weeping->speed - 0.4f * 0.031f) < 1e-6f, "speed * 0.031 (%f)", weeping->speed);
    CHECK(fabsf(weeping->turn_speed - 0.002f) < 1e-6f, "turnSpd * 0.002 (%f)", weeping->turn_speed);
    CHECK(weeping->fire_rate == 300.0f && weeping->behaviour_type == 1 && weeping->data.projectile == 1, "fr 300, bh 1, pj 1");
    CHECK(weeping->scale == 14.0f && weeping->always_name, "ms 14, alwaysName");
    CHECK(weeping->hit_range == 40.0f, "canShoot: hitRange 40");
    // ct 1, te 15, ta 4, tin 1: onAIDeath -> Destroy Interface 1
    CHECK(weeping->data.trig.event == TRIGGER_EVENT_ON_AI_DEATH && weeping->data.trig.action == 4 &&
          weeping->data.trig.interface_target == 1 && weeping->data.trig.network_sound, "Weeping Soul trigger");
    CHECK(weeping->data.idle_sound == 3206 && weeping->data.death_sound == 3236 && weeping->data.attack_sound == 3200, "Weeping Soul sounds");

    const GameAI *wandering = &game->ai->ais[1];
    CHECK(!strcmp(wandering->name, "Wandering Soul") && wandering->data.can_hit && !wandering->data.can_shoot, "Wandering Soul cnh");
    CHECK(wandering->hit_range == 10.0f && wandering->scale == 4.0f && !wandering->always_name, "hitRange 10, scale 4");
    CHECK(wandering->data.trig.event == TRIGGER_EVENT_NONE, "no ct: no trigger event");

    const GameAI *lord = &game->ai->ais[11];
    CHECK(!strcmp(lord->name, "Lord of Souls") && lord->behaviour_type == 2 && lord->max_health == 20000.0f, "Lord of Souls %s", lord->name);
    CHECK(lord->scale == 23.5f && lord->fire_rate == 9000.0f, "Lord ms 23.5 fr 9000");
}

// ---------------------------------------------------------------- behaviour on a flat test map

static const char *k_flat_map =
    "{\"name\":\"AI Flat\",\"spawns\":[[0,0,0,0,0,0]],\"objects\":["
    "{\"p\":[0,-10,0],\"s\":[2000,10,2000]},"
    "{\"p\":[0,2,-60],\"s\":[9,10,9],\"i\":55,\"h\":50,\"sp\":10,\"ms\":4,\"tsd\":30,\"spd\":1.1,\"vd\":1000,\"fr\":200,\"cnh\":1,\"dth\":3206,\"ain\":\"Wandering Soul\"},"
    "{\"p\":[300,30,0],\"s\":[11,12,11],\"i\":55,\"h\":1000,\"sp\":10,\"ms\":5,\"tsd\":10,\"spd\":0,\"vd\":1000,\"fr\":3000,\"cs\":1,\"rt\":2,\"ain\":\"Soul Fiend\"},"
    "{\"p\":[0,2,500],\"s\":[9,10,9],\"i\":\"BOT\",\"ain\":\"Bad*Name#\"}"
    "]}";

static void test_behaviour(Game *game) {
    printf("Running AI behaviour test...\n");
    CHECK(game->ai && game->ai->count == 3, "3 AIs on the flat map (%d)", game->ai ? game->ai->count : -1);
    if (!game->ai || game->ai->count < 3) return;
    GameAI *soul = &game->ai->ais[0], *fiend = &game->ai->ais[1], *bot = &game->ai->ais[2];

    // objects.js limits: no "fr" -> 200 (limitMM) -> fireRate 200; no "vd" -> 1 -> vision 1; bad aiName -> "Bot"
    CHECK(!strcmp(bot->name, "Bot"), "invalid aiName -> Bot (%s)", bot->name);
    CHECK(bot->fire_rate == 200.0f && bot->vision == 1.0f, "limits: fireRate %.0f vision %.0f", bot->fire_rate, bot->vision);

    Player *p = add_player(game, 1, "Alice");
    place(p, 0.0f, 0.0f, 0.0f);
    g_sent_count = 0;

    // the Wandering Soul finds Alice (vision 1000, line of sight) and closes in to hitRange
    tick_ai(game, 3000.0f);
    CHECK(soul->target_uid == 1, "soul targets Alice (%d)", soul->target_uid);
    const float d = sqrtf(soul->position.x * soul->position.x + soul->position.z * soul->position.z);
    CHECK(d < 20.0f, "soul closed in (distance %.1f)", d);
    CHECK(p->health < 100.0f, "canHit: 25 per 800 ms (health %.0f)", p->health);
    CHECK(count_sent(NET_PACKET_PROJECTILE, -100) >= 1, "the Soul Fiend shot (stored[1])");
    const SentPacket *shot = last_sent(NET_PACKET_PROJECTILE);
    if (shot) {
        NetProjectilePacket packet;
        memcpy(&packet, shot->payload, sizeof(packet));
        CHECK(packet.ai_kind == 1 && packet.owner_uid == -1, "AI shot: kind %d owner %d", packet.ai_kind, packet.owner_uid);
    }

    // Alice shoots the Fiend: changeHealth, "4" with isAI, killScore on the kill, respawn after rt
    p->health = 100.0f;
    place(p, 300.0f, 0.0f, 60.0f);
    const float eye = p->position.y + p->height - game_constants.camera_height;
    g_sent_count = 0;
    int shots = 0;
    while (fiend->active && shots < 200) {
        p->direction.x = atanf((fiend->position.y + fiend->scale - eye) / 60.0f);
        p->direction.y = 0.0f;
        p->ammo[p->loadout_index] = p->weapon->ammo;
        p->spread = 0.0f;
        player_shoot(p);
        shots++;
    }
    CHECK(!fiend->active && fiend->health == 0.0f, "Fiend killed after %d shots", shots);
    CHECK(shots >= 40 && shots <= 50, "1000 hp at ~22.5 per bullet: %d shots", shots);
    const SentPacket *hit = last_sent(NET_PACKET_HIT);
    if (hit) {
        NetHitPacket packet;
        memcpy(&packet, hit->payload, sizeof(packet));
        CHECK(packet.ai && packet.kill && packet.target_uid == fiend->sid, "last HIT: ai %d kill %d sid %d", packet.ai, packet.kill, packet.target_uid);
    }
    CHECK(p->score == 10, "killScore 10 (score %d)", p->score);
    CHECK(fiend->respawn_timer == 2000.0f, "respawnT 2 s (%.0f)", fiend->respawn_timer);
    tick_ai(game, 2100.0f);
    CHECK(fiend->active && fiend->health == 1000.0f && fiend->position.x == 300.0f, "respawned at startP with full health");

    // the soul's death sound: "s" with "asset:3206" at the AI, volume 0.9
    g_sent_count = 0;
    game_ai_kill(game, soul, p);
    const SentPacket *sound = last_sent(NET_PACKET_SOUND);
    CHECK(sound != NULL, "death sound sent");
    if (sound) {
        NetSoundPacket packet;
        memcpy(&packet, sound->payload, sizeof(packet));
        CHECK(!strcmp(packet.name, "asset:3206") && packet.uid == -1 && fabsf(packet.volume - 0.9f) < 1e-4f, "death sound %s %.2f", packet.name, packet.volume);
    }
    soul->active = 1; // back for the sync test
    soul->health = soul->max_health;

    // explosions hurt AIs with an owner
    const float before = fiend->health;
    game_explosion(game, fiend->position, 35.0f, 35.0f, p, 9);
    CHECK(fiend->health < before && fiend->target_uid == 1, "explosion damage %.1f", before - fiend->health);

    game_player_left(game, p);
    game->player_count = 0;
}

// ---------------------------------------------------------------- sync

static void test_sync(Game *game) {
    printf("Running AI sync test...\n");
    Player *p = add_player(game, 2, "Bob");
    place(p, 0.0f, 0.0f, 0.0f);
    game->is_local = 0;
    static int server;
    game->server = &server;
    for (int i = 0; i < game->ai->count; i++) game->ai->ais[i].sent_count = 0;

    g_sent_count = 0;
    game_ai_sync(game);
    // in range of Bob: the soul (near) and the BOT at z 500; the Fiend at x 300 too (< 600)
    CHECK(count_sent(NET_PACKET_AI_ADD, 2) == 3, "first sync: 3 aai to Bob (%d)", count_sent(NET_PACKET_AI_ADD, 2));
    CHECK(count_sent(NET_PACKET_AI_SYNC, 2) == 1, "one ai list to Bob");
    g_sent_count = 0;
    game_ai_sync(game);
    CHECK(count_sent(NET_PACKET_AI_ADD, 2) == 0, "no second aai");
    const SentPacket *sync = NULL;
    for (int i = 0; i < g_sent_count; i++) if (g_sent[i].type == NET_PACKET_AI_SYNC && g_sent[i].uid == 2) sync = &g_sent[i];
    if (sync) {
        NetAISyncPacket packet;
        memset(&packet, 0, sizeof(packet));
        memcpy(&packet, sync->payload, sync->length);
        CHECK(packet.first && packet.count == 3, "ai list: first %d count %d", packet.first, packet.count);
    }

    // a client mirror applies them
    Game mirror = {0};
    for (int i = 0; i < g_sent_count; i++) {
        if (g_sent[i].uid != 2) continue;
        if (g_sent[i].type == NET_PACKET_AI_SYNC) {
            NetAISyncPacket packet;
            memset(&packet, 0, sizeof(packet));
            memcpy(&packet, g_sent[i].payload, g_sent[i].length);
            game_ai_apply_sync(&mirror, &packet);
        }
    }
    CHECK(!mirror.ai || !mirror.ai->count, "sync without aai creates nothing");
    for (int i = 0; i < game->ai->count; i++) {
        NetAIAddPacket add;
        memset(&add, 0, sizeof(add));
        add.sid = game->ai->ais[i].sid;
        add.index = game->ai->ais[i].index;
        snprintf(add.name, sizeof(add.name), "%s", game->ai->ais[i].name);
        add.has_data = 1;
        add.max_health = game->ai->ais[i].max_health;
        add.m_size = game->ai->ais[i].scale;
        game_ai_apply_add(&mirror, &add);
    }
    if (sync) {
        NetAISyncPacket packet;
        memset(&packet, 0, sizeof(packet));
        memcpy(&packet, sync->payload, sync->length);
        game_ai_apply_sync(&mirror, &packet);
    }
    const GameAI *m = mirror.ai ? game_ai_find(&mirror, game->ai->ais[0].sid) : NULL;
    CHECK(m && m->shown && fabsf(m->position.z - game->ai->ais[0].position.z) < 0.06f, "mirror follows the soul");

    game->server = NULL;
    game->is_local = 1;
    game_player_left(game, p);
    game->player_count = 0;
}

// ---------------------------------------------------------------- chat commands

static int peer_count(void *user) { (void) user; return 1; }
static int peer_at(void *user, const int index, ChatPeer *out) {
    Game *game = user;
    if (index) return 0;
    memset(out, 0, sizeof(*out));
    out->id = 3;
    out->name = "Carol";
    out->address = "";
    out->player = game_find_player(game, 3);
    return 1;
}

static void test_chat(Game *game) {
    printf("Running /summon /ais /kill test...\n");
    Player *p = add_player(game, 3, "Carol");
    place(p, 0.0f, 0.0f, 0.0f);
    ChatCommandHost host = {.game = game, .user = game, .peer_count = peer_count, .peer_at = peer_at};
    // cheats (/summon, /kill) need a DEV peer or an open host; a guest on a default server is refused
    const int ai_before_gate = game->ai ? game->ai->count : 0;
    chat_command_handle(&host, 3, "/summon fiend");
    CHECK((game->ai ? game->ai->count : 0) == ai_before_gate, "guest /summon refused on a default (cheats auth) host");
    host.cheats_open = 1;

    int count = 0;
    const GameAITemplate *templates = game_ai_templates(&count);
    CHECK(count == 6, "6 Soul Sanctum templates (%d)", count);
    if (count == 6) CHECK(!strcmp(templates[0].name, "Angered Soul") && !strcmp(templates[5].name, "Weeping Soul"), "sorted names");
    CHECK(game_ai_resolve_template("lord") && !strcmp(game_ai_resolve_template("lord")->name, "Lord of Souls"), "alias lord");
    CHECK(game_ai_resolve_template("SOUL fiend") && !strcmp(game_ai_resolve_template("SOUL fiend")->name, "Soul Fiend"), "normalised name");

    chat_command_handle(&host, 3, "/summon");
    CHECK(!strcmp(g_last_reply, "Summonable: Angered Soul, Gatekeeper, Lord of Souls, Soul Fiend, Wandering Soul, Weeping Soul"), "%s", g_last_reply);
    chat_command_handle(&host, 3, "/summon nobody");
    CHECK(!strncmp(g_last_reply, "Unknown summon 'nobody'. Try: Angered Soul", 42), "%s", g_last_reply);
    const int before = game->ai->count;
    chat_command_handle(&host, 3, "/summon fiend");
    CHECK(game->ai->count == before + 1, "summoned one");
    const GameAI *fiend = &game->ai->ais[game->ai->count - 1];
    char expected[64];
    snprintf(expected, sizeof(expected), "Summoned Soul Fiend (%d).", fiend->sid);
    CHECK(!strcmp(g_last_reply, expected), "%s", g_last_reply);
    // distance max(48, mSize 5 * 4 = 20) = 48 at (sin yaw, cos yaw) with yaw 0; y + max(2, 2.5)
    CHECK(fabsf(fiend->position.z - 48.0f) < 0.06f && fabsf(fiend->position.y - 2.5f) < 0.06f, "summon at z %.1f y %.1f", fiend->position.z, fiend->position.y);
    CHECK(fiend->fire_rate == 3000.0f && fiend->max_health == 1000.0f, "template data unclamped");

    chat_command_handle(&host, 3, "/ais");
    CHECK(!strncmp(g_last_reply, "AI (4): Wandering Soul, Soul Fiend, Bot, Soul Fiend", 51), "%s", g_last_reply);
    chat_command_handle(&host, 3, "/kill fiend");
    CHECK(!strcmp(g_last_reply, "Killed 2 AIs: Soul Fiend, Soul Fiend."), "%s", g_last_reply);
    chat_command_handle(&host, 3, "/kill soul fiend");
    CHECK(!strcmp(g_last_reply, "Kill target 'soul fiend' not found."), "%s", g_last_reply);

    game_player_left(game, p);
    game->player_count = 0;
}

// ---------------------------------------------------------------- modes

static void test_traitor(Game *game) {
    printf("Running Traitor ghost test...\n");
    const int count = game->ai->count;
    Player *p = add_player(game, 4, "Dave");
    place(p, 10.0f, 0.0f, 10.0f);
    GameModeConfig config = game->mode->config;
    const GameModeConfig *trai = mode_config(MODE_ID_TRAI);
    game->mode->config.on_kill = trai ? trai->on_kill : NULL; // modes.js trai onKill on this round
    const int waits = game->mode->wait_timer_count;
    game->mode->wait_timer_count = 0;
    player_kill(p, NULL, NULL, 0);
    CHECK(game->ai->count == count + 1 && !strcmp(game->ai->ais[count].name, "Dave's Ghost"), "Dave's Ghost spawned");
    if (game->ai->count == count + 1) CHECK(game->ai->ais[count].index == 0 && game->ai->ais[count].max_health == 0.0f, "built-in kind 0");
    game->mode->wait_timer_count = waits;
    game->mode->config = config;
    game_player_left(game, p);
    game->player_count = 0;
}

// Raid: the behavType 2 boss died -> every player's "time" = the round clock, "raid ends in " 10 s.
static void test_raid(Game *game) {
    printf("Running Raid boss test...\n");
    Player *p = add_player(game, 5, "Erin");
    p->mode.lives = 1;
    GameAIData boss = {.health = 100.0f, .behaviour = 2};
    GameAI *lord = game_ai_spawn(game, 0, 0, "Lord of Souls", (vec3) {0.0f, 0.0f, 900.0f}, &boss);
    const int waits = game->mode->wait_timer_count;
    game->mode->wait_timer_count = 0;
    game->round_timer = 123.45f;
    CHECK(mode_cond_raid(game) && !game->mode->wait_timer_count && !p->mode.has_timer, "boss alive: no end timer");
    if (lord) game_ai_kill(game, lord, NULL);
    CHECK(mode_cond_raid(game), "raid goes on while the end timer runs");
    CHECK(game->mode->wait_timer_count == 1 && !strcmp(game->mode->wait_timers[0].msg, "raid ends in "), "raid ends in 10 s");
    CHECK(p->mode.has_timer && fabsf(p->mode.timer - 123.45f) < 1e-3f, "time stat = lastTimer (%.2f)", p->mode.timer);
    game->mode->wait_timer_count = waits;
    game_player_left(game, p);
    game->player_count = 0;
}

int main(void) {
    const int soul = map_catalog_index_of_key("soul_sanctum");
    const cJSON *soul_json = soul >= 0 ? map_catalog_json((size_t) soul) : NULL;
    cJSON *flat = cJSON_Parse(k_flat_map);
    if (!soul_json || !flat) {
        printf("Failed to load the maps (run from the repository root)\n");
        return 1;
    }

    static const cJSON *maps[2];
    maps[0] = soul_json;
    maps[1] = flat;
    Game game = {0};
    game_configure(&game, NULL, maps, 2, NULL, 0, NULL, 0, NULL, 0);
    game.hooks.send = test_send;

    game_init(&game, 0, -1, 1);
    if (!game.ready) {
        printf("Failed to init the game\n");
        return 1;
    }
    test_map_bots(&game);

    game_init(&game, 1, -1, 1);
    test_behaviour(&game);
    test_sync(&game);
    test_chat(&game);
    test_traitor(&game);
    test_raid(&game);

    game_init(&game, 1, -1, 1);
    CHECK(game.ai && game.ai->count == 3, "game.init resets and respawns the map AIs (%d)", game.ai ? game.ai->count : -1);

    if (g_failures) {
        printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("Test OK\n");
    return 0;
}
