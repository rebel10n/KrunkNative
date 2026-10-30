// Krunker v7.1.4 AI (shared/src/ai.c, shared/src/map_ai.c) through the shared game hooks: Soul
// Sanctum's 20 map AIs (v3.1.1 BOT objects and the v7 official map), buildAIData / addAI clamps,
// chasing / melee / shooting, damage from a player's bullets and explosions (killScore, "4" with
// isAI), respawn and removal ("rai"), paths of every path type, roaming, AI_REDIRECTION, team
// targeting, onAI* triggers, sd / rsc / Spawn AI by interface, the 40 limit, Force AI Target, the
// v3.1.1 and v7 sync packets (stride 9, range 800) and their client mirror, Traitor ghosts, the
// /summon /ais /kill chat commands and the CPU cost of 40 AIs.
// Run from the repository root: ./<build>/ai_test
#include <shared.h>
#include <ai.h>
#include <map_ai.h>
#include <chat_commands.h>
#include <map_catalog.h>
#include <triggers_v7.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void player_shoot(Player*); // shared/src/player.c

#define MAX_SENT 4096

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

static GameAI *ai_named(Game *game, const char *name) {
    for (int i = 0; game->ai && i < game->ai->count; i++) {
        if (!game->ai->ais[i].removed && !strcmp(game->ai->ais[i].name, name)) return &game->ai->ais[i];
    }
    return NULL;
}

static cJSON *load_json(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *text = malloc((size_t) size + 1);
    if (!text) {
        fclose(f);
        return NULL;
    }
    const size_t n = fread(text, 1, (size_t) size, f);
    fclose(f);
    text[n] = 0;
    cJSON *json = cJSON_Parse(text);
    free(text);
    return json;
}

// A game on one map, the shared test hooks.
static void game_on(Game *game, const cJSON *map) {
    static const cJSON *maps[1];
    maps[0] = map;
    game_configure(game, NULL, maps, 1, NULL, 0, NULL, 0, NULL, 0);
    game->hooks.send = test_send;
    game_init(game, 0, 0, 1); // the first mode of the default list: FFA (no player teams)
}

// ---------------------------------------------------------------- Soul Sanctum

static void test_v3_sanctum(Game *game) {
    printf("Running Soul Sanctum (v3.1.1 BOT) test...\n");
    CHECK(game->ai && game->ai->count == 20, "20 map AIs, got %d", game->ai ? game->ai->count : -1);
    for (size_t i = 0; i < game->map->object_count; i++) CHECK(game->map->objects[i]->prefab != PREFAB_AI_SPAWNER, "AI_SPAWNER %zu is a map object", i);
    if (!game->ai || game->ai->count < 12) return;

    const GameAI *weeping = &game->ai->ais[0];
    CHECK(!strcmp(weeping->name, "Weeping Soul"), "first AI %s", weeping->name);
    CHECK(weeping->sid == 1, "generateSID starts at 1 (%d)", weeping->sid);
    CHECK(weeping->max_health == 10000.0f && weeping->health == 10000.0f, "Weeping Soul health %.0f", weeping->health);
    CHECK(fabsf(weeping->speed - 0.4f * 0.031f) < 1e-6f, "speed * 0.031 (%f)", weeping->speed);
    CHECK(fabsf(weeping->turn_speed - 0.002f) < 1e-6f, "turnSpd * 0.002 (%f)", weeping->turn_speed);
    CHECK(weeping->fire_rate == 300.0f && weeping->behaviour_type == 1 && weeping->data.can_shoot, "fr 300, bh 1, cs");
    CHECK(weeping->scale == 14.0f && weeping->height == 14.0f && weeping->always_name, "ms 14, height 14, alwaysName");
    CHECK(fabsf(weeping->hit_range - 7.0f) < 1e-4f, "meleeChaseRange = max(mlr 10) * 0.7 (%f)", weeping->hit_range);
    // addAI on an AI without aid: fct 2, fs 5, rs 2 (the ghost strip, double sided)
    CHECK(weeping->data.frames == 2 && weeping->data.frame_time == 500.0f && weeping->data.side == 2, "ghost strip %d %.0f %d",
          weeping->data.frames, weeping->data.frame_time, weeping->data.side);
    // ct 1, te 15, ta 4, tin 1: onAIDeath -> Destroy Interface 1 (tas[0] from the legacy keys)
    CHECK(weeping->data.trig.event == 15 && weeping->data.trig.action == 4 && weeping->data.trig.action_count == 1 &&
          weeping->data.trig.interface_target == 1, "Weeping Soul trigger %d %d %d", weeping->data.trig.event, weeping->data.trig.action,
          weeping->data.trig.action_count);
    CHECK(weeping->data.idle_sound == 3206 && weeping->data.death_sound == 3236 && weeping->data.attack_sound == 3200, "Weeping Soul sounds");
    CHECK(weeping->data.targets_players && !weeping->data.targets_ai, "tPl == null: targets players only");

    const GameAI *wandering = &game->ai->ais[1];
    CHECK(!strcmp(wandering->name, "Wandering Soul") && wandering->data.can_hit && !wandering->data.can_shoot, "Wandering Soul cnh");
    CHECK(wandering->scale == 4.0f && !wandering->always_name, "scale 4");
    CHECK(wandering->data.trig.event == -1, "no ct: no trigger event");

    const GameAI *lord = &game->ai->ais[11];
    CHECK(!strcmp(lord->name, "Lord of Souls") && lord->behaviour_type == 2 && lord->max_health == 20000.0f, "Lord of Souls %s", lord->name);
    CHECK(lord->scale == 23.5f && lord->fire_rate == 9000.0f, "Lord ms 23.5 fr 9000");
}

static void test_v7_sanctum(Game *game) {
    printf("Running Soul Sanctum (v7.1.4) test...\n");
    CHECK(game->ai && game->ai->count == 20, "v7 sanctum: 20 AIs (%d)", game->ai ? game->ai->count : -1);
    if (!game->ai) return;
    int named = 0, bosses = 0;
    for (int i = 0; i < game->ai->count; i++) {
        named += game->ai->ais[i].name[0] != 0;
        bosses += game->ai->ais[i].behaviour_type == 1 || game->ai->ais[i].behaviour_type == 2;
    }
    CHECK(named == 20 && bosses == 2, "named %d, bosses %d", named, bosses);
    CHECK(game->ai->ais[0].team == 2 && game->ai->ais[0].data.targets_players, "tm 2, targets players");

    // a player walks into the first hall (the spawn room is closed off): the souls react, nothing
    // breaks over a minute of play
    Player *p = add_player(game, 1, "Alice");
    place(p, 230.0f, 2.0f, 0.0f);
    p->god_mode = 1;
    for (int s = 0; s < 60; s++) {
        tick_ai(game, 1000.0f);
        CHECK(game->ai->count <= GAME_AI_MAX, "list bounded");
    }
    int targeting = 0;
    for (int i = 0; i < game->ai->count; i++) targeting += game->ai->ais[i].target_uid == 1;
    printf("  %d of %d souls target the player after 60 s\n", targeting, game->ai->count);
    CHECK(targeting >= 3, "the hall's souls hunt the player (%d)", targeting);
    game_player_left(game, p);
    game->player_count = 0;
}

// ---------------------------------------------------------------- behaviour on a flat test map

static const char *k_flat_map =
    "{\"name\":\"AI Flat\",\"spawns\":[[0,0,0,0,0,0]],\"objects\":["
    "{\"p\":[0,-10,0],\"s\":[2000,10,2000]},"
    "{\"p\":[0,2,-60],\"s\":[9,10,9],\"i\":55,\"h\":50,\"sp\":10,\"ms\":4,\"tsd\":30,\"spd\":1.1,\"vd\":1000,\"fr\":200,\"cnh\":1,\"dth\":3206,\"ain\":\"Wandering Soul\"},"
    "{\"p\":[300,30,0],\"s\":[11,12,11],\"i\":55,\"h\":1000,\"sp\":10,\"ms\":5,\"tsd\":10,\"spd\":0,\"vd\":1000,\"fr\":3000,\"cs\":1,\"rt\":2,\"ain\":\"Soul Fiend\"},"
    "{\"p\":[0,2,700],\"s\":[9,10,9],\"i\":55,\"ain\":\"Bad*Name#\"}"
    "]}";

static void test_behaviour(Game *game) {
    printf("Running AI behaviour test...\n");
    CHECK(game->ai && game->ai->count == 3, "3 AIs on the flat map (%d)", game->ai ? game->ai->count : -1);
    if (!game->ai || game->ai->count < 3) return;
    GameAI *soul = &game->ai->ais[0], *fiend = &game->ai->ais[1], *bot = &game->ai->ais[2];

    // buildAIData defaults + addAI limits: fr || 2000, vd || 120, spd / tsd / grv 1, invalid aiName -> ""
    CHECK(!bot->name[0], "invalid aiName -> \"\" (%s)", bot->name);
    CHECK(bot->fire_rate == 2000.0f && bot->vision == 120.0f, "defaults: fireRate %.0f vision %.0f", bot->fire_rate, bot->vision);
    CHECK(fabsf(bot->speed - 0.031f) < 1e-6f && fabsf(bot->gravity - 0.1f) < 1e-6f, "spd / grv default 1");
    CHECK(bot->max_health == 0.0f && bot->active, "no h: health 0, still spawned");

    Player *p = add_player(game, 1, "Alice");
    place(p, 0.0f, 0.0f, 0.0f);
    g_sent_count = 0;

    // the Wandering Soul finds Alice (vision 1000, line of sight) and closes in to melee range
    tick_ai(game, 3000.0f);
    CHECK(soul->target_uid == 1 && soul->target.kind == AI_TARGET_PLAYER, "soul targets Alice (%d)", soul->target_uid);
    const float d = sqrtf(soul->position.x * soul->position.x + soul->position.z * soul->position.z);
    CHECK(d < 20.0f, "soul closed in (distance %.1f)", d);
    CHECK(p->health < 100.0f, "canHit: meldmg 10 per hit (health %.0f)", p->health);
    CHECK(count_sent(NET_PACKET_PROJECTILE, -100) >= 1, "the Soul Fiend shot (stored[1])");
    const SentPacket *shot = last_sent(NET_PACKET_PROJECTILE);
    if (shot) {
        NetProjectilePacket packet;
        memcpy(&packet, shot->payload, sizeof(packet));
        CHECK(packet.ai_kind == 1 && packet.owner_uid == -1, "AI shot: kind %d owner %d", packet.ai_kind, packet.owner_uid);
        const float speed = sqrtf(packet.velocity[0] * packet.velocity[0] + packet.velocity[1] * packet.velocity[1] + packet.velocity[2] * packet.velocity[2]);
        CHECK(fabsf(speed - 0.1f) < 1e-3f && packet.range == 500.0f, "v7 stored[1]: spd 0.1 * pjSp 1, range 500 (%f)", speed);
    }
    if (shot) { // the in-process game keeps the shooter per projectile slot (servers also send NET_PACKET_AI_SHOT)
        NetProjectilePacket packet;
        memcpy(&packet, shot->payload, sizeof(packet));
        CHECK(game->ai->shots[packet.id].used && game->ai->shots[packet.id].sid == fiend->sid, "shot by the Fiend (%d)", game->ai->shots[packet.id].sid);
    }

    // Alice shoots the Fiend: changeHealth, "4" with isAI, killScore on the kill, respawn after rt
    p->health = 100.0f;
    place(p, 300.0f, 30.0f, 60.0f);
    const float eye = p->position.y + p->height - game_constants.camera_height;
    g_sent_count = 0;
    int shots = 0;
    while (fiend->active && shots < 200) {
        p->direction.x = atanf((fiend->position.y + fiend->height - eye) / 60.0f);
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

    // explosions hurt AIs with an owner (and aggro them)
    const float before = fiend->health;
    game_explosion(game, fiend->position, 35.0f, 35.0f, p, 9);
    CHECK(fiend->health < before && fiend->target_uid == 1, "explosion damage %.1f", before - fiend->health);

    // the soul's death: "s" with "asset:3206" at the AI, volume 0.9; no respawnT -> removeFully
    g_sent_count = 0;
    const int soul_sid = soul->sid;
    soul->sent_to[soul->sent_count++] = 1; // as if Alice had its "aai"
    soul->data.is_boss = 1; // a boss's death: every active non-bot player gets bossKills (deob-10.0.3.js:206316-206324)
    const int boss_before = p->mode.boss_kills;
    game_ai_kill(game, soul, p);
    CHECK(p->mode.boss_kills == boss_before + 1, "bossKills %d -> %d", boss_before, p->mode.boss_kills);
    const SentPacket *sound = last_sent(NET_PACKET_SOUND);
    CHECK(sound != NULL, "death sound sent");
    if (sound) {
        NetSoundPacket packet;
        memcpy(&packet, sound->payload, sizeof(packet));
        CHECK(!strcmp(packet.name, "asset:3206") && packet.uid == -1 && fabsf(packet.volume - 0.9f) < 1e-4f, "death sound %s %.2f", packet.name, packet.volume);
    }
    CHECK(!game_ai_find(game, soul_sid), "no respawnT: removed from the list");
    CHECK(count_sent(NET_PACKET_AI_REMOVE, 1) == 1, "\"rai\" to the viewer");
    game_ai_tick(game, 1.0f); // the slot is dropped at the next tick (pointers held by callers stay valid)
    CHECK(game->ai->count == 2, "2 AIs left (%d)", game->ai->count);
    fiend = &game->ai->ais[0];

    game_player_left(game, p);
    game->player_count = 0;
}

// ---------------------------------------------------------------- sync

static void test_sync(Game *game) {
    printf("Running AI sync test...\n");
    CHECK(game_ai_packet_for_peer(NET_PACKET_AI_SYNC, NET_PROTOCOL_MINOR_V7) == 0 && game_ai_packet_for_peer(NET_PACKET_AI_SYNC, 2) == 1,
          "v3.1.1 AI_SYNC only for older peers");
    CHECK(game_ai_packet_for_peer(NET_PACKET_AI_SYNC_V7, NET_PROTOCOL_MINOR_V7) == 1 && game_ai_packet_for_peer(NET_PACKET_AI_ADD_V7, 2) == 0 &&
          game_ai_packet_for_peer(NET_PACKET_AI_REMOVE, 1) == 0 && game_ai_packet_for_peer(NET_PACKET_HIT, 0) == 1,
          "v7 AI packets only for v7 peers, the rest untouched");

    Player *p = add_player(game, 2, "Bob");
    place(p, 0.0f, 0.0f, 0.0f);
    game->is_local = 0;
    static int server;
    game->server = &server;
    for (int i = 0; i < game->ai->count; i++) game->ai->ais[i].sent_count = 0;

    g_sent_count = 0;
    game_ai_sync(game);
    // in range (800) of Bob: the Fiend at x 300 and the nameless AI at z 700 (v3.1.1's 600 missed it)
    CHECK(count_sent(NET_PACKET_AI_ADD_V7, 2) == 2 && count_sent(NET_PACKET_AI_ADD, 2) == 2, "first sync: 2 aai to Bob (%d)",
          count_sent(NET_PACKET_AI_ADD_V7, 2));
    CHECK(count_sent(NET_PACKET_AI_SYNC_V7, 2) == 1, "one ai list to Bob");
    g_sent_count = 0;
    game->ai->ais[0].did_move = 1;
    game->ai->ais[0].team = 2;
    game_ai_sync(game);
    CHECK(count_sent(NET_PACKET_AI_ADD_V7, 2) == 0, "no second aai");
    const SentPacket *sync = NULL;
    for (int i = 0; i < g_sent_count; i++) if (g_sent[i].type == NET_PACKET_AI_SYNC_V7 && g_sent[i].uid == 2) sync = &g_sent[i];
    NetAISyncV7Packet packet;
    memset(&packet, 0, sizeof(packet));
    if (sync) memcpy(&packet, sync->payload, sync->length);
    CHECK(sync && packet.first && packet.count == 2 && sync->length == 2 + 2 * sizeof(NetAISyncV7Entry), "ai list: first %d count %d", packet.first, packet.count);
    CHECK(packet.entries[0].did_move == 1 && packet.entries[0].team == 2, "stride 9: didMov and team");

    // the v7 client mirror
    Game mirror = {0};
    for (int i = 0; i < g_sent_count; i++) {
        if (g_sent[i].uid == 2 && g_sent[i].type == NET_PACKET_AI_SYNC_V7) {
            NetAISyncV7Packet s;
            memset(&s, 0, sizeof(s));
            memcpy(&s, g_sent[i].payload, g_sent[i].length);
            game_ai_apply_sync_v7(&mirror, &s);
        }
    }
    CHECK(!mirror.ai || !mirror.ai->count, "sync without aai creates nothing");
    for (int i = 0; i < game->ai->count; i++) {
        game->ai->ais[i].sent_count = 0;
    }
    g_sent_count = 0;
    game_ai_sync(game);
    for (int i = 0; i < g_sent_count; i++) {
        if (g_sent[i].uid == 2 && g_sent[i].type == NET_PACKET_AI_ADD_V7) {
            NetAIAddV7Packet add;
            memcpy(&add, g_sent[i].payload, sizeof(add));
            game_ai_apply_add_v7(&mirror, &add);
        }
    }
    game_ai_sync(game);
    for (int i = 0; i < g_sent_count; i++) {
        if (g_sent[i].uid == 2 && g_sent[i].type == NET_PACKET_AI_SYNC_V7) {
            NetAISyncV7Packet s;
            memset(&s, 0, sizeof(s));
            memcpy(&s, g_sent[i].payload, g_sent[i].length);
            game_ai_apply_sync_v7(&mirror, &s);
        }
    }
    const GameAI *m = mirror.ai ? game_ai_find(&mirror, game->ai->ais[0].sid) : NULL;
    CHECK(m && m->shown && fabsf(m->position.x - game->ai->ais[0].position.x) < 0.06f && m->team == 2, "mirror follows the Fiend");
    CHECK(m && m->max_health == game->ai->ais[0].max_health && fabsf(m->scale - 5.0f) < 1e-4f, "mirror data: health, mSize");
    const NetAIRemovePacket rai = {.sid = game->ai->ais[0].sid};
    game_ai_apply_remove(&mirror, &rai);
    CHECK(!game_ai_find(&mirror, rai.sid), "rai removes the mirrored AI");
    free(mirror.ai);

    game->server = NULL;
    game->is_local = 1;
    game_player_left(game, p);
    game->player_count = 0;
}

// A player sees an AI only once it was sent the AI's "aai": AI.sync keeps sentTo, an unbounded list of the player
// ids it went to (deob-10.0.5.js:183213). KN's sent_to held 32 uids and never forgot one, so on a busy server a
// long-lived AI (a map NPC, a boss) filled up with players who had left: every later player got the "aai" and then
// an "ai" list without the AI, every tick (hidden), and never saw it. A 40-player lobby (maxPS) overflowed it too.
static int synced_to(const int uid, const int sid) {
    for (int i = 0; i < g_sent_count; i++) {
        if (g_sent[i].uid != uid || g_sent[i].type != NET_PACKET_AI_SYNC_V7) continue;
        NetAISyncV7Packet s;
        memset(&s, 0, sizeof(s));
        memcpy(&s, g_sent[i].payload, g_sent[i].length);
        for (int k = 0; k < s.count; k++) if (s.entries[k].sid == sid) return 1;
    }
    return 0;
}

static void drop_player(Game *game, Player *p) {
    game_player_left(game, p);
    for (size_t i = 0; i < game->player_count; i++) {
        if (game->players[i] != p) continue;
        game->players[i] = game->players[--game->player_count];
        break;
    }
    free(p->loadout); // as the server's LEAVE frees it
    free(p->ammo);
    free(p->reloads);
    free(p->input_queue);
    free(p);
}

static void test_sync_churn(Game *game) {
    printf("Running AI sync churn test...\n");
    game->is_local = 0;
    static int server;
    game->server = &server;
    GameAI *ai = &game->ai->ais[0];
    const int sid = ai->sid;
    const vec3 at = ai->position;
    for (int i = 0; i < game->ai->count; i++) game->ai->ais[i].sent_count = 0;

    // 40 players come near the AI one after another, see it, and leave
    for (int uid = 100; uid < 140; uid++) {
        Player *p = add_player(game, uid, "Passer");
        place(p, at.x, at.y, at.z);
        g_sent_count = 0;
        game_ai_sync(game);
        game_ai_sync(game);
        CHECK(synced_to(uid, sid), "player %d (one of 40 passing through) sees the AI", uid);
        drop_player(game, p);
    }
    // a newcomer after them
    Player *late = add_player(game, 200, "Late");
    place(late, at.x, at.y, at.z);
    g_sent_count = 0;
    game_ai_sync(game);
    CHECK(count_sent(NET_PACKET_AI_ADD_V7, 200) >= 1, "the newcomer gets the AI's aai");
    g_sent_count = 0;
    game_ai_sync(game);
    CHECK(count_sent(NET_PACKET_AI_ADD_V7, 200) == 0 && synced_to(200, sid), "...once, then the AI is in its ai list");
    drop_player(game, late);

    // a full 40-player lobby (maxPS) around the AI: every one of them sees it
    Player *crowd[40];
    for (int i = 0; i < 40; i++) {
        crowd[i] = add_player(game, 300 + i, "Crowd");
        place(crowd[i], at.x, at.y, at.z);
    }
    g_sent_count = 0;
    game_ai_sync(game);
    g_sent_count = 0;
    game_ai_sync(game);
    int seen = 0;
    for (int i = 0; i < 40; i++) seen += synced_to(300 + i, sid);
    CHECK(seen == 40, "all 40 players of a full lobby see the AI (%d)", seen);
    for (int i = 0; i < 40; i++) drop_player(game, crowd[i]);

    game->server = NULL;
    game->is_local = 1;
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
    const int before = game->ai->count;
    chat_command_handle(&host, 3, "/summon fiend");
    CHECK(game->ai->count == before + 1, "summoned one");
    const GameAI *fiend = &game->ai->ais[game->ai->count - 1];
    char expected[64];
    snprintf(expected, sizeof(expected), "Summoned Soul Fiend (%d).", fiend->sid);
    CHECK(!strcmp(g_last_reply, expected), "%s", g_last_reply);
    CHECK(fabsf(fiend->position.z - 48.0f) < 0.06f && fabsf(fiend->position.y - 2.5f) < 0.06f, "summon at z %.1f y %.1f", fiend->position.z, fiend->position.y);
    CHECK(fiend->fire_rate == 3000.0f && fiend->max_health == 1000.0f, "template data");

    chat_command_handle(&host, 3, "/kill fiend");
    CHECK(!strcmp(g_last_reply, "Killed 2 AIs: Soul Fiend, Soul Fiend."), "%s", g_last_reply);
    game_ai_tick(game, 1.0f);

    game_player_left(game, p);
    game->player_count = 0;
}

// ---------------------------------------------------------------- Traitor ghosts

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
    if (game->ai->count == count + 1) {
        const GameAI *ghost = &game->ai->ais[count];
        CHECK(ghost->index == 0 && ghost->max_health == 0.0f && ghost->bob_d == 1.0f && !ghost->data.glow && !ghost->data.frames,
              "built-in kind 0: the ghost (no frames, no glow: v7's dat conf)");
    }
    game->mode->wait_timer_count = waits;
    game->mode->config = config;
    game_player_left(game, p);
    game->player_count = 0;
}

// ---------------------------------------------------------------- v7_ai_paths.json

static void test_paths_fixture(Game *game) {
    printf("Running path / roam / redirection / team test (v7_ai_paths.json)...\n");
    CHECK(game->ai && game->ai->count == 7, "7 of 8 AIs spawned (Hidden waits: cdy + sd), got %d", game->ai ? game->ai->count : -1);
    if (!game->ai) return;
    GameAI *looper = ai_named(game, "Looper");
    CHECK(looper && looper->data.path_count == 4 && looper->data.path_type == AI_PATH_LOOP, "Looper path 1,2,3,4 Loop");

    // Loop: 0 -> 1 -> 2 -> 3 -> 0 ...
    int seen[16], seen_count = 0, last = -1;
    for (int f = 0; f < 60 * 12 && seen_count < 16; f++) {
        game_ai_tick(game, 1000.0f / 60.0f);
        looper = ai_named(game, "Looper");
        if (looper && looper->path_index != last) seen[seen_count++] = last = looper->path_index;
    }
    CHECK(seen_count >= 6, "the Looper advanced (%d)", seen_count);
    int ok = 1;
    for (int i = 1; i < seen_count; i++) ok &= seen[i] == (seen[i - 1] + 1) % 4;
    CHECK(ok, "Loop order");
    if (looper) CHECK(looper->position.y > -0.5f && looper->position.y < 0.5f, "grv 0: stays on the nodes' height (%.2f)", looper->position.y);

    // roam: within rmr of startP
    GameAI *roamer = ai_named(game, "Roamer");
    if (roamer) {
        const float dx = roamer->position.x - roamer->start.x, dz = roamer->position.z - roamer->start.z;
        const float r = sqrtf(dx * dx + dz * dz);
        CHECK(roamer->has_roam_target && r > 1.0f && r <= 101.0f, "roams within 100 (%.1f)", r);
    }

    // AI_REDIRECTION pushes the pMov Drifter along d 1 (+z)
    GameAI *drifter = ai_named(game, "Drifter");
    if (drifter) CHECK(drifter->position.z > 5.0f && drifter->position.x > 1040.0f, "redirected: x %.1f z %.1f", drifter->position.x, drifter->position.z);

    // teams: Red (tm 1, tAi) fights Blue (tm 2), never its mate (tm 1, nearer)
    GameAI *red = ai_named(game, "Red"), *mate = ai_named(game, "Red Mate"), *blue = ai_named(game, "Blue");
    if (red && mate && blue) {
        CHECK(red->target.kind == AI_TARGET_AI && red->target.id == blue->sid, "Red targets Blue (%d:%d)", red->target.kind, red->target.id);
        CHECK(blue->health < blue->max_health, "Blue took melee damage (%.0f)", blue->health);
        CHECK(mate->health == mate->max_health, "Red Mate untouched (%.0f)", mate->health);
        CHECK(mate->target.kind == AI_TARGET_AI && mate->target.id == blue->sid, "the mate targets Blue too");
    } else {
        CHECK(blue == NULL && red != NULL, "Blue may have died: Red %p Mate %p Blue %p", (void *) red, (void *) mate, (void *) blue);
    }

    // onAIDeath (ct, te 15, tas [{ta 0, tc 7}]): the killer gets 7 score
    Player *p = add_player(game, 7, "Gina");
    place(p, -2000.0f, 0.0f, -1900.0f);
    GameAI *pinata = ai_named(game, "Pinata");
    CHECK(pinata && pinata->data.trig.event == 15 && pinata->data.trig.action_count == 1, "Pinata onAIDeath");
    if (pinata) game_ai_damage(game, pinata, p, 100.0f, 0);
    CHECK(p->score == 7, "onAIDeath ran Give Player Score 7 (score %d)", p->score);

    // sd + cdy: pending only; trigger action 36 with interfaceT 5 spawns it
    CHECK(!ai_named(game, "Hidden"), "Hidden starts destroyed");
    CHECK(map_ai_spawn_interface(game, 0, 5) == 1 && ai_named(game, "Hidden"), "Spawn AI (interface 5)");
    CHECK(map_ai_spawn_interface(game, 5, 5) == 0, "not the source's own interface");

    // Force AI Target: every AI chases the point for 30 s
    map_ai_force_target_point(game, (vec3) {123.0f, 0.0f, 456.0f});
    game_ai_tick(game, 16.0f);
    GameAI *any = ai_named(game, "Roamer");
    CHECK(any && any->target.kind == AI_TARGET_POINT && any->target.point.x == 123.0f, "forced target point");
    tick_ai(game, 31000.0f);
    CHECK(game->ai->force_target_timer == 0.0f, "force target expires");

    game_player_left(game, p);
    game->player_count = 0;
}

// Every path type on a 4-node square: the order of path indices it walks.
static void test_path_types(void) {
    printf("Running path type test...\n");
    static const char *names[] = {"Loop", "Do Once", "Ping Pong", "Random"};
    for (int type = 0; type < 4; type++) {
        char text[2048];
        snprintf(text, sizeof(text),
                 "{\"name\":\"paths\",\"spawns\":[[0,0,500,0,0,0]],\"objects\":[{\"p\":[0,-10,0],\"s\":[2000,10,2000]},"
                 "{\"i\":64,\"p\":[0,0,0],\"s\":[2,2,2],\"nid\":11},{\"i\":64,\"p\":[60,0,0],\"s\":[2,2,2],\"nid\":12},"
                 "{\"i\":64,\"p\":[60,0,60],\"s\":[2,2,2],\"nid\":13},{\"i\":64,\"p\":[0,0,60],\"s\":[2,2,2],\"nid\":14},"
                 "{\"i\":55,\"p\":[0,0,0],\"s\":[9,10,9],\"ain\":\"Walker\",\"h\":10,\"phi\":\"11,12,13,14\",\"pht\":%d,\"spd\":4,\"tsd\":30,"
                 "\"grv\":0,\"vd\":1,\"tPl\":1}]}",
                 type);
        cJSON *map = cJSON_Parse(text);
        Game game = {0};
        game_on(&game, map);
        int seq[64], n = 0, last = -1;
        for (int f = 0; f < 60 * 20 && n < 64; f++) {
            game_ai_tick(&game, 1000.0f / 60.0f);
            const GameAI *w = ai_named(&game, "Walker");
            if (w && w->path_index != last) seq[n++] = last = w->path_index;
        }
        int ok = n >= 2;
        switch (type) {
            case AI_PATH_LOOP:
                for (int i = 1; i < n; i++) ok &= seq[i] == (seq[i - 1] + 1) % 4;
                break;
            case AI_PATH_ONCE: // (0, left on the first update) 1 2 3 and stays
                ok = n == 3 && seq[0] == 1 && seq[2] == 3;
                break;
            case AI_PATH_PING_PONG: { // 1 2 3 2 1 0 1 ...
                const int expect[] = {1, 2, 3, 2, 1, 0, 1, 2, 3, 2};
                for (int i = 0; i < n && i < 10; i++) ok &= seq[i] == expect[i];
                ok &= n >= 8;
                break;
            }
            default: // Random: indices 0..3, more than one of them
                for (int i = 0; i < n; i++) ok &= seq[i] >= 0 && seq[i] <= 3;
                ok &= n >= 4;
                break;
        }
        printf("  %-9s:", names[type]);
        for (int i = 0; i < n && i < 12; i++) printf(" %d", seq[i]);
        printf("\n");
        CHECK(ok, "path type %s order", names[type]);
        game_ai_free(&game);
        map_fini(game.map);
        mode_fini(game.mode);
        cJSON_Delete(map);
    }
}

// onAIDamage / onAIHalfHealth / onAIKillPlayer (WP-C's trigger_ai_event runs their tas).
static void test_ai_events(void) {
    printf("Running onAI* trigger test...\n");
#ifdef TRIGGER_HAVE_AI_EVENT
    static const char *k_map =
        "{\"name\":\"ai events\",\"spawns\":[[0,0,0,0,0,0]],\"objects\":[{\"p\":[0,-10,0],\"s\":[2000,10,2000]},"
        "{\"i\":55,\"p\":[0,0,50],\"s\":[9,10,9],\"ain\":\"Damage\",\"h\":1000,\"spd\":0,\"vd\":1,\"ct\":1,\"te\":20,\"tas\":[{\"ta\":0,\"tc\":1}]},"
        "{\"i\":55,\"p\":[100,0,50],\"s\":[9,10,9],\"ain\":\"Half\",\"h\":100,\"spd\":0,\"vd\":1,\"ct\":1,\"te\":29,\"tas\":[{\"ta\":0,\"tc\":50}]},"
        "{\"i\":55,\"p\":[0,0,-5],\"s\":[9,10,9],\"ain\":\"Killer\",\"h\":100,\"spd\":0,\"vd\":1000,\"cnh\":1,\"mld\":200,\"ct\":1,\"te\":31,\"tas\":[{\"ta\":0,\"tc\":300}]}]}";
    cJSON *map = cJSON_Parse(k_map);
    Game game = {0};
    game_on(&game, map);
    Player *p = add_player(&game, 1, "Hank");
    place(p, 300.0f, 0.0f, 300.0f);
    GameAI *dmg = ai_named(&game, "Damage"), *half = ai_named(&game, "Half");
    if (dmg) game_ai_damage(&game, dmg, p, 10.0f, 0);
    if (dmg) game_ai_damage(&game, dmg, p, 10.0f, 0);
    CHECK(p->score == 2, "onAIDamage twice: +1 +1 (score %d)", p->score);
    if (half) game_ai_damage(&game, half, p, 40.0f, 0); // 100 -> 60: no
    if (half) game_ai_damage(&game, half, p, 20.0f, 0); // 60 -> 40: crosses half
    if (half) game_ai_damage(&game, half, p, 10.0f, 0); // 40 -> 30: once only
    CHECK(p->score == 52, "onAIHalfHealth once: +50 (score %d)", p->score);
    // the Killer's 200 melee kills Hank; onAIKillPlayer gives the dead player 300
    place(p, 0.0f, 0.0f, 0.0f);
    const int score = p->score;
    tick_ai(&game, 3000.0f);
    CHECK(!p->active && p->score >= score + 300, "onAIKillPlayer (score %d, active %d)", p->score, p->active);
    game_player_left(&game, p);
    game.player_count = 0;
    game_ai_free(&game);
    map_fini(game.map);
    mode_fini(game.mode);
    cJSON_Delete(map);
#else
    printf("  skipped: trigger_ai_event (WP-C) is not in this build\n");
#endif
}

// config.aiLimit: 45 AI_SPAWNERs -> 40 AIs; forced spawns pass it.
static void test_limit(void) {
    printf("Running aiLimit 40 test...\n");
    char *text = malloc(64 * 1024);
    int n = snprintf(text, 64 * 1024, "{\"name\":\"limit\",\"spawns\":[[0,0,0,0,0,0]],\"objects\":[{\"p\":[0,-10,0],\"s\":[4000,10,4000]}");
    for (int i = 0; i < 45; i++) n += snprintf(text + n, (size_t) (64 * 1024 - n), ",{\"i\":55,\"p\":[%d,0,%d],\"s\":[9,10,9],\"h\":10,\"vd\":1}", (i % 9) * 40, (i / 9) * 40);
    snprintf(text + n, (size_t) (64 * 1024 - n), "]}");
    cJSON *map = cJSON_Parse(text);
    free(text);
    Game game = {0};
    game_on(&game, map);
    CHECK(game.ai && game.ai->count == GAME_AI_LIMIT, "40 of 45 map AIs (%d)", game.ai ? game.ai->count : -1);
    CHECK(!game_ai_spawn(&game, 0, 0, "over", (vec3) {0}, NULL), "AI.spawn refuses the 41st");
    cJSON *bag = cJSON_Parse("{\"h\":77,\"ain\":\"Zombie\",\"tm\":2,\"spd\":2,\"cnh\":1}");
    const int sid = map_ai_spawn(&game, bag, (vec3) {5.0f, 0.0f, 5.0f}, -1);
    const GameAI *zombie = sid > 0 ? game_ai_find(&game, sid) : NULL;
    CHECK(zombie && zombie->max_health == 77.0f && zombie->team == 2 && !strcmp(zombie->name, "Zombie"), "map_ai_spawn (forced) past the limit");
    CHECK(game.ai->count == GAME_AI_LIMIT + 1, "41 listed");
    cJSON_Delete(bag);
    game_ai_free(&game);
    map_fini(game.map);
    mode_fini(game.mode);
    cJSON_Delete(map);
}

// CPU cost: 40 fighting AIs and 8 players on a real map, 30 Hz server ticks.
static void test_cost(void) {
    printf("Running AI cost test...\n");
    const int index = map_catalog_index_of_key("sandstorm_v3"); // the v7.1.4 Sandstorm
    const cJSON *json = index >= 0 ? map_catalog_json((size_t) index) : NULL;
    if (!json) {
        CHECK(0, "no sandstorm map");
        return;
    }
    Game game = {0};
    game_on(&game, json);
    for (int i = 0; i < 8; i++) {
        Player *p = add_player(&game, 10 + i, "P");
        const Spawn *spawn = game.map->spawns[i % game.map->spawn_count];
        place(p, spawn->position.x, spawn->position.y, spawn->position.z);
        p->god_mode = 1;
    }
    for (int i = 0; game.ai && i < game.ai->count; i++) game_ai_remove(&game, &game.ai->ais[i]);
    game_ai_tick(&game, 0.0f); // the removed slots go at the next tick
    for (int i = 0; i < GAME_AI_LIMIT; i++) {
        GameAIData d;
        game_ai_data_defaults(&d);
        d.health = 500.0f;
        d.speed = 1.0f;
        d.turn_speed = 10.0f;
        d.gravity = 1.0f;
        d.vision = 500;
        d.targets_players = 1;
        d.targets_ai = i % 2;
        d.team = 1 + i % 2;
        d.can_shoot = (unsigned char) (i % 3 == 0);
        d.can_hit = (unsigned char) (i % 3 != 0);
        d.fire_rate = 1500;
        d.melee_range = 10.0f;
        d.chase_distance = 30.0f;
        d.roam_radius = 100.0f;
        d.roam_time = 2000.0f;
        const Spawn *spawn = game.map->spawns[(i * 7) % game.map->spawn_count];
        game_ai_spawn(&game, 0, 0, "Cost", (vec3) {spawn->position.x + (float) (i % 5) * 10.0f, spawn->position.y + 2.0f, spawn->position.z}, &d);
    }
    CHECK(game.ai && game.ai->count == GAME_AI_LIMIT, "40 AIs (%d)", game.ai ? game.ai->count : -1);
    const int ticks = 30 * 20;
    const clock_t start = clock();
    for (int t = 0; t < ticks; t++) {
        game_ai_tick(&game, 1000.0f / 30.0f);
        game_tick_projectiles(&game, 1.0f / 30.0f);
    }
    const double ms = (double) (clock() - start) * 1000.0 / CLOCKS_PER_SEC / ticks;
    printf("  40 AIs + 8 players, Sandstorm (%zu objects): %.3f ms per 30 Hz tick (%.1f%% of a 33 ms tick)\n", game.map->object_count, ms,
           ms / (1000.0 / 30.0) * 100.0);
    CHECK(ms < 5.0, "40 AIs cost %.3f ms per tick", ms);

    // AI.sync as a server: "aai" / "ai" (v3.1.1 + v7 encodings, the per-peer gate drops one) to 8 players
    static int server;
    game.server = &server;
    game.is_local = 0;
    const int syncs = 200;
    g_sent_count = 0;
    const clock_t sync_start = clock();
    for (int t = 0; t < syncs; t++) {
        g_sent_count = 0;
        game_ai_sync(&game);
    }
    const double sync_ms = (double) (clock() - sync_start) * 1000.0 / CLOCKS_PER_SEC / syncs;
    printf("  AI.sync to 8 players: %.3f ms per sync (5 per second), %d packets\n", sync_ms, g_sent_count);
    CHECK(sync_ms < 2.0, "sync cost %.3f ms", sync_ms);
    game.server = NULL;
    game.is_local = 1;
    for (size_t i = 0; i < game.player_count; i++) game_player_left(&game, game.players[i]);
    game.player_count = 0;
    game_ai_free(&game);
    map_fini(game.map);
    mode_fini(game.mode);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const int soul = map_catalog_index_of_key("311_soul_sanctum"); // the v3.1.1 Soul Sanctum
    const int soul7 = map_catalog_index_of_key("sanctum");        // the v7.1.4 official one
    const cJSON *soul_json = soul >= 0 ? map_catalog_json((size_t) soul) : NULL;
    const cJSON *soul7_json = soul7 >= 0 ? map_catalog_json((size_t) soul7) : NULL;
    cJSON *flat = cJSON_Parse(k_flat_map);
    cJSON *paths = load_json("shared/tests/maps/v7_ai_paths.json");
    if (!soul_json || !soul7_json || !flat || !paths) {
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
    test_v3_sanctum(&game);

    game_init(&game, 1, -1, 1);
    test_behaviour(&game);
    test_sync(&game);
    test_sync_churn(&game);
    test_chat(&game);
    test_traitor(&game);

    game_init(&game, 1, -1, 1);
    CHECK(game.ai && game.ai->count == 3, "game.init resets and respawns the map AIs (%d)", game.ai ? game.ai->count : -1);

    Game v7 = {0};
    game_on(&v7, soul7_json);
    test_v7_sanctum(&v7);

    Game fixture = {0};
    game_on(&fixture, paths);
    tick_ai(&fixture, 6000.0f);
    test_paths_fixture(&fixture);

    test_path_types();
    test_ai_events();
    test_limit();
    test_cost();

    if (g_failures) {
        printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("Test OK\n");
    return 0;
}
