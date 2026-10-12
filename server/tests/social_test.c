// player-viewer WP-E part 2b over the offline sandbox server (server_sandbox.h): the v7.1.4 text &
// voice channels (NET_PACKET_CHAT_V7 / VOICE_V7 and the bridge to v3 peers), multi-spray with the
// spray wheel's index (NET_PACKET_SPRAY_V7, slots, store items, invisible walls) and prop-hunt
// taunts. Raw protocol clients: three v7 peers and one v3 peer.
// Run from the repository root: ./<build>/social_test
#include <server_sandbox.h>
#include <server.h>
#include <shared.h>
#include <social_v7.h>
#include <voice.h>
#include <account.h>
#include <modes.h>
#include <map_catalog.h>
#include <knet.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;
static int g_checks;

#define CHECK(cond, ...) do { \
    g_checks++; \
    if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_failures++; } \
} while (0)

#define MAX_RECEIVED 2048
#define CLIENTS 4

typedef struct {
    int fd;
    int conn_id; // the server's ClientConnection.id of this link
    int v7;
    NetPacketBuffer buffer;
    NetPacket packets[MAX_RECEIVED];
    int count;
    SocialAssembly voice;
    char *clips[16]; // reassembled VOICE / VOICE_V7 messages
    int clip_v7[16];
    uint32_t clip_sid[16];
    int clip_count;
} TestClient;

static TestClient g_clients[CLIENTS];

static void send_to(TestClient *c, const int type, const void *payload, const uint16_t length) {
    CHECK(net_send_packet(c->fd, (NetPacketType) type, payload, length), "send type %d", type);
}

static void drain(TestClient *c) {
    uint8_t bytes[2048];
    int n;
    while ((n = sandbox_receive(c->fd, bytes, sizeof(bytes))) > 0) {
        CHECK(net_buffer_push(&c->buffer, bytes, (size_t) n), "buffer overflow");
        NetPacket packet;
        while (net_buffer_next(&c->buffer, &packet) > 0) {
            if (packet.type == NET_PACKET_PING && packet.length == sizeof(NetPingPacket)) send_to(c, NET_PACKET_PONG, packet.payload, packet.length);
            if (packet.type == NET_PACKET_VOICE || packet.type == NET_PACKET_VOICE_V7) {
                char *msg = NULL;
                uint32_t sid = 0;
                if (social_assembly_push(&c->voice, packet.payload, packet.length, &msg, &sid) == 1 && c->clip_count < 16) {
                    c->clip_v7[c->clip_count] = packet.type == NET_PACKET_VOICE_V7;
                    c->clip_sid[c->clip_count] = sid;
                    c->clips[c->clip_count++] = msg;
                } else {
                    free(msg);
                }
                continue;
            }
            if (c->count < MAX_RECEIVED) c->packets[c->count++] = packet;
        }
    }
}

static void pump(const int frames) {
    for (int f = 0; f < frames; f++) {
        sandbox_step(1.0 / 60.0);
        for (int i = 0; i < CLIENTS; i++) {
            if (g_clients[i].fd >= NET_LOOPBACK_FD_BASE) drain(&g_clients[i]);
        }
    }
}

static void reset_received(void) {
    for (int i = 0; i < CLIENTS; i++) {
        TestClient *c = &g_clients[i];
        c->count = 0;
        for (int k = 0; k < c->clip_count; k++) free(c->clips[k]);
        c->clip_count = 0;
    }
}

static int count_type(const TestClient *c, const int type) {
    int n = 0;
    for (int i = 0; i < c->count; i++) n += c->packets[i].type == type;
    return n;
}

static const NetPacket *find(const TestClient *c, const int type, const int nth) {
    int seen = 0;
    for (int i = 0; i < c->count; i++) {
        if (c->packets[i].type == type && seen++ == nth) return &c->packets[i];
    }
    return NULL;
}

static ClientConnection *conn(const TestClient *c) {
    for (size_t i = 0; i < g_server->client_count; i++) {
        if (g_server->clients[i].id == c->conn_id) return &g_server->clients[i];
    }
    return NULL;
}

static Player *player_of(const TestClient *c) {
    ClientConnection *cc = conn(c);
    return cc ? cc->player : NULL;
}

static void join_all(void) {
    for (int i = 0; i < CLIENTS; i++) {
        TestClient *c = &g_clients[i];
        c->fd = sandbox_connect();
        CHECK(c->fd >= NET_LOOPBACK_FD_BASE && g_server->client_count, "connect %d", i);
        c->conn_id = g_server->client_count ? g_server->clients[g_server->client_count - 1].id : -1;
        // v7 peers announce 1.3.0 (the 1.3.0 ENT keeps this test's ENT size), the last one 1.0.0
        const NetHelloPacket hello = {NET_PROTOCOL_MAJOR, (uint8_t) (c->v7 ? NET_PROTOCOL_MINOR_V7 : 0), 0};
        send_to(c, NET_PACKET_HELLO, &hello, sizeof(hello));
        NetJoinPacket join;
        memset(&join, 0, sizeof(join));
        send_to(c, NET_PACKET_JOIN, &join, (uint16_t) NET_JOIN_PACKET_V2_SIZE);
        NetEntPacket ent = {.class_index = 0, .secondary_index = -1, .cosmetics = PLAYER_COSMETICS_NONE, .slid_cont = 1, .lag_comp = 1.0f};
        send_to(c, NET_PACKET_ENT, &ent, (uint16_t) NET_ENT_PACKET_V7_SIZE);
        pump(2);
    }
    pump(60);
    for (int i = 0; i < CLIENTS; i++) {
        const TestClient *c = &g_clients[i];
        CHECK(player_of(c) != NULL, "client %d spawned", i);
        // after the HELLO (the INIT went out before it): the server takes the v7 social packets
        const NetPacket *caps = find(c, NET_PACKET_SOCIAL_V7, 0);
        if (c->v7) CHECK(caps && caps->length == 1 && caps->payload[0] == NET_SOCIAL_V7_VERSION, "SOCIAL_V7 to v7 peer %d", i);
        else CHECK(!caps, "no SOCIAL_V7 to the v3 peer");
    }
}

static void leave_all(void) {
    for (int i = 0; i < CLIENTS; i++) {
        sandbox_disconnect(g_clients[i].fd);
        g_clients[i].fd = -1;
        social_assembly_free(&g_clients[i].voice);
    }
    reset_received();
    sandbox_stop();
}

static int start(const cJSON *map, const int mode) {
    static const cJSON *maps[1];
    maps[0] = map;
    const SandboxConfig cfg = {.maps = maps, .map_count = 1, .map_index = 0, .mode_id = mode};
    memset(g_clients, 0, sizeof(g_clients));
    for (int i = 0; i < CLIENTS; i++) g_clients[i].v7 = i < CLIENTS - 1;
    if (!sandbox_start(&cfg)) return 0;
    join_all();
    return 1;
}

// ---------------------------------------------------------------- chat

static void chat_v7(TestClient *c, const int channel, const char *text) {
    NetChatV7SendPacket packet;
    memset(&packet, 0, sizeof(packet));
    packet.channel = (uint8_t) channel;
    snprintf(packet.text, sizeof(packet.text), "%s", text);
    send_to(c, NET_PACKET_CHAT_V7, &packet, (uint16_t) (1 + strlen(packet.text) + 1));
}

static void chat_v3(TestClient *c, const char *text) {
    NetChatSendPacket packet;
    memset(&packet, 0, sizeof(packet));
    snprintf(packet.text, sizeof(packet.text), "%s", text);
    send_to(c, NET_PACKET_CHAT_SEND, &packet, (uint16_t) (strlen(packet.text) + 1));
}

// The line `text` as `c` got it: 2 = NET_PACKET_CHAT_V7 (channel / account out), 1 = v3 CHAT, 0 none.
static int got_chat(const TestClient *c, const char *text, int *channel, int *account) {
    for (int i = 0; i < c->count; i++) {
        const NetPacket *p = &c->packets[i];
        if (p->type == NET_PACKET_CHAT_V7 && p->length == sizeof(NetChatV7Packet)) {
            NetChatV7Packet chat;
            memcpy(&chat, p->payload, sizeof(chat));
            chat.chat.text[sizeof(chat.chat.text) - 1] = 0;
            if (strcmp(chat.chat.text, text) != 0) continue;
            if (channel) *channel = chat.channel;
            if (account) *account = chat.account_id;
            return 2;
        }
        if (p->type == NET_PACKET_CHAT && p->length == sizeof(NetChatPacket)) {
            NetChatPacket chat;
            memcpy(&chat, p->payload, sizeof(chat));
            chat.text[sizeof(chat.text) - 1] = 0;
            if (!strcmp(chat.text, text)) return 1;
        }
    }
    return 0;
}

static void test_chat_channels(void) {
    printf("Running chat channel test...\n");
    const int failures = g_failures;
    TestClient *a = &g_clients[0], *b = &g_clients[1], *c = &g_clients[2], *d = &g_clients[3];
    // teams: A, B and the v3 peer D on team 1, C on team 2
    player_of(a)->team = 1;
    player_of(b)->team = 1;
    player_of(c)->team = 2;
    player_of(d)->team = 1;
    conn(a)->acct.account_id = 77; // the account id rides along (mute_chat)

    reset_received();
    chat_v7(a, CHAT_CHANNEL_GLOBAL, "hello all");
    pump(3);
    int channel = -5, account = -5;
    CHECK(got_chat(a, "hello all", &channel, &account) == 2 && channel == 0 && account == 77, "global: the sender's echo (ch %d acc %d)", channel, account);
    CHECK(got_chat(b, "hello all", &channel, NULL) == 2 && channel == 0, "global: teammate");
    CHECK(got_chat(c, "hello all", &channel, NULL) == 2 && channel == 0, "global: enemy");
    CHECK(got_chat(d, "hello all", NULL, NULL) == 1, "global: the v3 peer gets NET_PACKET_CHAT");
    CHECK(!count_type(d, NET_PACKET_CHAT_V7), "no CHAT_V7 for the v3 peer");

    reset_received();
    chat_v7(a, CHAT_CHANNEL_TEAM, "team only");
    pump(3);
    CHECK(got_chat(a, "team only", &channel, NULL) == 2 && channel == 1, "team: echo on channel 1");
    CHECK(got_chat(b, "team only", &channel, NULL) == 2 && channel == 1, "team: teammate on channel 1");
    CHECK(!got_chat(c, "team only", NULL, NULL), "team: the enemy gets nothing");
    CHECK(got_chat(d, "team only", NULL, NULL) == 1, "team: the v3 teammate gets the v3 line");

    reset_received();
    chat_v3(d, "from v3");
    pump(3);
    CHECK(got_chat(a, "from v3", &channel, &account) == 2 && channel == 0 && account == 0, "v3 ct: global for v7 peers");
    CHECK(got_chat(c, "from v3", NULL, NULL) == 2, "v3 ct: enemy v7 peer too");
    CHECK(got_chat(d, "from v3", NULL, NULL) == 1, "v3 ct: its own echo");

    reset_received();
    chat_v7(b, 7, "odd channel"); // getChatTeam: anything but 1 is global
    pump(3);
    CHECK(got_chat(c, "odd channel", &channel, NULL) == 2 && channel == 0, "channel 7 -> global");
    pump(61); // io_ct burst window
    printf("Chat channel test %s\n", g_failures != failures ? "FAILED" : "OK");
}

static void test_chat_no_teams(void) {
    printf("Running chat without teams test...\n");
    const int failures = g_failures;
    reset_received();
    chat_v7(&g_clients[0], CHAT_CHANNEL_TEAM, "ffa team");
    pump(3);
    int channel = -5;
    // sendPlayerChat: teamChannel = requested == 1 && game.mode.teams
    CHECK(got_chat(&g_clients[2], "ffa team", &channel, NULL) == 2 && channel == 0, "FFA: the team channel is global (%d)", channel);
    CHECK(got_chat(&g_clients[3], "ffa team", NULL, NULL) == 1, "FFA: the v3 peer too");
    pump(61);
    printf("Chat without teams test %s\n", g_failures != failures ? "FAILED" : "OK");
}

// ---------------------------------------------------------------- voice

static int voice_send_fn(void *user, const NetPacketType type, const void *payload, const uint16_t length) {
    send_to(user, type, payload, length);
    return 1;
}

#define CLIP "data:audio/ogg; codecs=opus;base64,T2dnUwACAAAAAAAAAAA="

static const char *clip_v7(const TestClient *c, VoiceV7Meta *meta, char *alias, const size_t alias_size, uint32_t *sid) {
    for (int i = 0; i < c->clip_count; i++) {
        if (!c->clip_v7[i]) continue;
        const char *audio = NULL;
        if (voice_parse_relay_v7(c->clips[i], meta, alias, alias_size, &audio)) {
            if (sid) *sid = c->clip_sid[i];
            return audio;
        }
    }
    return NULL;
}

static const char *clip_v3(const TestClient *c, char *alias, const size_t alias_size) {
    for (int i = 0; i < c->clip_count; i++) {
        const char *audio = NULL;
        if (!c->clip_v7[i] && voice_parse_relay(c->clips[i], alias, alias_size, &audio)) return audio;
    }
    return NULL;
}

static void test_voice_channels(void) {
    printf("Running voice channel test...\n");
    const int failures = g_failures;
    TestClient *a = &g_clients[0], *b = &g_clients[1], *c = &g_clients[2], *d = &g_clients[3];
    VoiceV7Meta meta;
    char alias[64];
    uint32_t sid = 0;

    // team channel, volume over 1 (clamped): the teammates only, the v3 teammate as v3 "vc"
    reset_received();
    VoiceV7Send send = {.channel = CHAT_CHANNEL_TEAM, .volume = 2.5f, .quality = 9000};
    CHECK(voice_send_clip_v7(CLIP, strlen(CLIP), 1, &send, voice_send_fn, a), "send v7 clip");
    pump(3);
    const char *audio = clip_v7(b, &meta, alias, sizeof(alias), &sid);
    CHECK(audio && !strcmp(audio, CLIP) && meta.channel == 1 && fabsf(meta.volume - 1.0f) < 1e-4f && !meta.is_final && meta.account_id == 77,
          "team clip to the teammate (ch %d vol %.2f acc %d)", meta.channel, (double) meta.volume, meta.account_id);
    CHECK(audio && (int) sid == conn(a)->id && !strcmp(alias, conn(a)->name), "speaker sid %u alias %s", sid, alias);
    CHECK(!c->clip_count, "team clip: not to the enemy (KrunkNative fix)");
    CHECK(!a->clip_count, "no echo to the speaker");
    audio = clip_v3(d, alias, sizeof(alias));
    CHECK(audio && !strcmp(audio, CLIP) && d->clip_count == 1 && !d->clip_v7[0], "team clip: the v3 teammate gets v3 \"vc\"");

    // global channel, half volume: everyone else
    reset_received();
    send = (VoiceV7Send) {.channel = CHAT_CHANNEL_GLOBAL, .volume = 0.5f, .quality = 6000};
    voice_send_clip_v7(CLIP, strlen(CLIP), 2, &send, voice_send_fn, b);
    pump(3);
    CHECK(clip_v7(a, &meta, alias, sizeof(alias), NULL) && meta.channel == 0 && fabsf(meta.volume - 0.5f) < 1e-3f && meta.account_id == 0, "global clip");
    CHECK(clip_v7(c, &meta, alias, sizeof(alias), NULL) && meta.channel == 0, "global clip to the enemy");
    CHECK(clip_v3(d, alias, sizeof(alias)) != NULL, "global clip to the v3 peer");

    // not a data:audio/ URL: dropped
    reset_received();
    const char *bad = "data:text/plain;base64,AAAA";
    voice_send_clip_v7(bad, strlen(bad), 3, &send, voice_send_fn, a);
    pump(3);
    CHECK(!b->clip_count && !c->clip_count && !d->clip_count, "non-audio clip dropped");

    // a v3 clip: v7 peers hear it on every channel at full volume
    reset_received();
    CHECK(voice_send_clip(CLIP, strlen(CLIP), 4, voice_send_fn, d), "send v3 clip");
    pump(3);
    CHECK(clip_v7(a, &meta, alias, sizeof(alias), NULL) && meta.channel == CHAT_CHANNEL_ALL && meta.volume == 1.0f, "v3 clip -> v7 channel -1");
    CHECK(clip_v7(c, &meta, alias, sizeof(alias), NULL) != NULL, "v3 clip to every v7 peer");
    CHECK(!d->clip_count, "v3 clip: no echo");

    // a spectator (no voice player) is not relayed
    reset_received();
    conn(c)->voice_player = 0;
    voice_send_clip_v7(CLIP, strlen(CLIP), 5, &send, voice_send_fn, c);
    pump(3);
    CHECK(!a->clip_count && !d->clip_count, "no player: dropped");
    conn(c)->voice_player = 1;
    printf("Voice channel test %s\n", g_failures != failures ? "FAILED" : "OK");
}

// ---------------------------------------------------------------- taunts

static int taunt_sounds(const TestClient *c, const char *name) {
    int n = 0;
    for (int i = 0; i < c->count; i++) {
        if (c->packets[i].type != NET_PACKET_SOUND || c->packets[i].length != sizeof(NetSoundPacket)) continue;
        NetSoundPacket sound;
        memcpy(&sound, c->packets[i].payload, sizeof(sound));
        sound.name[sizeof(sound.name) - 1] = 0;
        if (!strcmp(sound.name, name) && fabsf(sound.volume - TAUNT_VOLUME) < 1e-4f && sound.rate >= 0.9f && sound.rate <= 1.0f) n++;
    }
    return n;
}

static void test_taunts(void) {
    printf("Running taunt test...\n");
    const int failures = g_failures;
    TestClient *a = &g_clients[0], *c = &g_clients[2];
    Player *pa = player_of(a), *pc = player_of(c);
    pa->team = 1; // "Prop"
    pc->team = 2; // "Seekers"
    pa->active = pc->active = 1;
    CHECK(game_player_is_prop(&g_server->game, pa) && !game_player_is_prop(&g_server->game, pc), "Prop / Seekers teams");

    reset_received();
    NetTauntPacket taunt = {.index = 2};
    send_to(a, NET_PACKET_TAUNT, &taunt, sizeof(taunt));
    pump(2);
    CHECK(taunt_sounds(c, "taunt_2") == 1, "a prop's taunt plays for everyone (%d)", taunt_sounds(c, "taunt_2"));
    send_to(a, NET_PACKET_TAUNT, &taunt, sizeof(taunt));
    pump(2);
    CHECK(taunt_sounds(c, "taunt_2") == 1, "taunt_2 waits its 2.3 s");
    pump(140);
    send_to(a, NET_PACKET_TAUNT, &taunt, sizeof(taunt));
    pump(2);
    CHECK(taunt_sounds(c, "taunt_2") == 2, "after 2.3 s it plays again");

    reset_received();
    taunt.index = 0;
    send_to(c, NET_PACKET_TAUNT, &taunt, sizeof(taunt));
    taunt.index = 9;
    send_to(a, NET_PACKET_TAUNT, &taunt, sizeof(taunt));
    pump(2);
    CHECK(!taunt_sounds(a, "taunt_0") && !count_type(a, NET_PACKET_SOUND), "no seeker taunt, no taunt 9");
    printf("Taunt test %s\n", g_failures != failures ? "FAILED" : "OK");
}

static void test_taunt_outside_prop(void) {
    reset_received();
    const NetTauntPacket taunt = {.index = 1};
    send_to(&g_clients[0], NET_PACKET_TAUNT, &taunt, sizeof(taunt));
    pump(2);
    CHECK(!taunt_sounds(&g_clients[1], "taunt_1"), "no taunts outside prop hunt");
}

// ---------------------------------------------------------------- sprays

// A floor, an invisible wall at z -10 and a visible one at z -20 (front face -19), a block to
// the right whose top face is at y 10.
static cJSON *spray_map(void) {
    return cJSON_Parse("{\"name\":\"spray test\",\"spawns\":[[0,0,0]],\"objects\":["
                       "{\"p\":[0,-2,0],\"s\":[400,2,400]},"
                       "{\"p\":[0,0,-10],\"s\":[60,40,2],\"v\":1},"
                       "{\"p\":[0,0,-20],\"s\":[60,40,2]},"
                       "{\"p\":[100,0,0],\"s\":[40,10,40]}"
                       "]}");
}

static int spray_v7(const TestClient *c, const int nth, NetSprayV7Packet *out) {
    const NetPacket *p = find(c, NET_PACKET_SPRAY_V7, nth);
    if (!p || p->length != sizeof(*out)) return 0;
    memcpy(out, p->payload, sizeof(*out));
    return 1;
}

static void spray(TestClient *c, const int index) {
    const NetSprayRequestPacket request = {.index = index};
    send_to(c, NET_PACKET_SPRAY_V7, &request, sizeof(request));
    pump(64); // sprayTimer: 1 s between sprays
}

static void aim(TestClient *c, const vec3 position, const float pitch, const float yaw) {
    Player *p = player_of(c);
    p->position = position;
    p->direction = (vec2) {pitch, yaw};
    p->active = 1;
}

static void test_sprays(void) {
    printf("Running multi-spray test...\n");
    const int failures = g_failures;
    TestClient *a = &g_clients[0], *b = &g_clients[1], *d = &g_clients[3];
    CHECK(g_server->game.is_sandbox && game_spray_limit(&g_server->game, player_of(a)) == SPRAY_MAX_FEATURED, "sandbox: maxSpraysF");

    reset_received();
    aim(a, (vec3) {0, 0, 0}, 0.0f, 0.0f); // facing -z: through the invisible wall onto the visible one
    spray(a, -1);
    NetSprayV7Packet s;
    CHECK(spray_v7(b, 0, &s) && s.uid == conn(a)->id && s.slot == 0 && s.skin == SPRAY_DEFAULT_INDEX && s.dye == -1,
          "plain spry: the equipped spray (ENT 0 = store 2482), slot 0 (skin %d slot %d)", s.skin, s.slot);
    CHECK(fabsf(s.position[2] - (-18.9f)) < 0.011f && s.y_rot == 0 && s.x_rot == 0 && s.z_rot == 0, "lands on the visible wall (z %g)", (double) s.position[2]);
    CHECK(!count_type(b, NET_PACKET_SPRAY), "v7 peers get no v3 \"sp\"");
    const NetPacket *legacy = find(d, NET_PACKET_SPRAY, 0);
    NetSprayPacket old = {0};
    if (legacy && legacy->length == sizeof(old)) memcpy(&old, legacy->payload, sizeof(old));
    CHECK(legacy && old.spray_index == 0 && fabsf(old.position[2] - s.position[2]) < 1e-4f, "v3 peer: \"sp\" with Game.sprays index 0");
    CHECK(!count_type(d, NET_PACKET_SPRAY_V7), "v3 peer: no SPRAY_V7");

    // wheel indices: any spray in the sandbox; not a spray -> 2482; slots cycle at 3
    spray(a, 866);
    spray(a, 5);
    spray(a, 2535);
    NetSprayV7Packet s1, s2, s3;
    CHECK(spray_v7(b, 1, &s1) && s1.skin == 866 && s1.slot == 1, "wheel spray 866 in slot 1 (%d / %d)", s1.skin, s1.slot);
    CHECK(spray_v7(b, 2, &s2) && s2.skin == SPRAY_DEFAULT_INDEX && s2.slot == 2, "a weapon skin index sprays 2482");
    CHECK(spray_v7(b, 3, &s3) && s3.skin == 2535 && s3.slot == 0, "the 4th spray reuses slot 0 (slot %d)", s3.slot);
    legacy = find(d, NET_PACKET_SPRAY, 3);
    if (legacy && legacy->length == sizeof(old)) memcpy(&old, legacy->payload, sizeof(old));
    CHECK(legacy && old.spray_index == 2535, "v3 peer: store index outside the legacy range unchanged");

    // the sprayTimer
    reset_received();
    const NetSprayRequestPacket request = {.index = -1};
    send_to(a, NET_PACKET_SPRAY_V7, &request, sizeof(request));
    send_to(a, NET_PACKET_SPRAY_V7, &request, sizeof(request));
    pump(3);
    CHECK(count_type(b, NET_PACKET_SPRAY_V7) == 1, "one spray per sprayTimer");
    pump(64);

    // only invisible walls ahead: no spray
    reset_received();
    g_server->game.map->objects[2]->no_vis = 1;
    aim(a, (vec3) {0, 0, 0}, 0.0f, 0.0f);
    spray(a, -1);
    CHECK(!count_type(b, NET_PACKET_SPRAY_V7) && !count_type(d, NET_PACKET_SPRAY), "no spray on invisible walls");
    g_server->game.map->objects[2]->no_vis = 0;

    // the top face of the block: rotation.x -90, rotation.z turns with the yaw (round(yaw / 90°) * 90)
    reset_received();
    aim(a, (vec3) {100, 12, 0}, (float) -M_PI / 2.0f + 0.01f, (float) M_PI / 2.0f);
    spray(a, -1);
    CHECK(spray_v7(b, 0, &s) && s.x_rot == -90 && s.y_rot == 0 && s.z_rot == 90, "top face: rot x %d y %d z %d", s.x_rot, s.y_rot, s.z_rot);
    printf("Multi-spray test %s\n", g_failures != failures ? "FAILED" : "OK");
}

// The spray choice outside the sandbox: premium accounts choose from the wheel (free sprays, or
// owned ones), everyone else sprays the equipped one.
static int g_premium, g_verified;
static int test_account_info(const Game *game, const Player *player, AccountInfo *out) {
    (void) game;
    (void) player;
    out->has_account = 1;
    out->premium = (unsigned char) g_premium;
    out->verified = (unsigned char) g_verified;
    return 1;
}

static void test_spray_rules(void) {
    printf("Running spray rule test...\n");
    const int failures = g_failures;
    Game *game = &g_server->game;
    Player *p = player_of(&g_clients[0]);
    const unsigned char sandbox = game->is_sandbox;
    int (*saved)(const Game*, const Player*, AccountInfo*) = game->hooks.account_info;
    game->is_sandbox = 0;
    game->hooks.account_info = test_account_info;
    p->spray_index = 3; // a v3-era ENT: Game.sprays[3] = store 2485
    g_premium = 0;
    CHECK(game_spray_limit(game, p) == SPRAY_MAX_DEFAULT, "maxSprays 1");
    CHECK(game_spray_pick(game, p, 866, 1) == 2485, "no premium: the equipped spray, the wheel index ignored");
    p->spray_index = 100; // a weapon skin (past the legacy 0..52)
    CHECK(game_spray_pick(game, p, -1, 0) == SPRAY_DEFAULT_INDEX, "equipped non-spray -> 2482");
    g_premium = 1;
    CHECK(game_spray_limit(game, p) == SPRAY_MAX_PREMIUM, "maxSpraysP 2");
    CHECK(game_spray_pick(game, p, 866, 0) == SPRAY_DEFAULT_INDEX, "premium: an unowned paid spray -> 2482");
    CHECK(game_spray_pick(game, p, 866, 1) == 866, "premium: an owned spray");
    CHECK(game_spray_pick(game, p, 2535, 0) == 2535, "premium: a free spray");
    p->spray_index = 2489;
    CHECK(game_spray_pick(game, p, -1, 0) == 2489, "premium plain spry: the equipped spray (KrunkNative fix)");
    g_verified = 1;
    CHECK(game_spray_limit(game, p) == SPRAY_MAX_FEATURED, "featured: maxSpraysF 3");
    g_premium = g_verified = 0;
    game->hooks.account_info = saved;
    game->is_sandbox = sandbox;
    p->spray_index = 0;
    printf("Spray rule test %s\n", g_failures != failures ? "FAILED" : "OK");
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    cJSON *sprays = spray_map();
    const cJSON *burg = map_catalog_json((size_t) map_catalog_index_of_key("311_burg"));
    CHECK(sprays && burg, "maps");

    if (start(burg, MODE_ID_TDM)) {
        test_chat_channels();
        test_voice_channels();
        test_taunt_outside_prop();
        leave_all();
    } else CHECK(0, "sandbox TDM");

    if (start(burg, MODE_ID_FFA)) {
        test_chat_no_teams();
        leave_all();
    } else CHECK(0, "sandbox FFA");

    if (start(burg, MODE_ID_PROP)) {
        test_taunts();
        leave_all();
    } else CHECK(0, "sandbox prop hunt");

    if (start(sprays, MODE_ID_FFA)) {
        test_sprays();
        test_spray_rules();
        leave_all();
    } else CHECK(0, "sandbox spray map");

    cJSON_Delete(sprays);
    printf("%d checks, %d failures\n", g_checks, g_failures);
    printf(g_failures ? "Test FAILED\n" : "Test OK\n");
    return g_failures ? 1 : 0;
}
