// KrunkScript server host (server/include/ks_server.h, doc/v7/krunkscript.md WP3) over the
// offline sandbox: a map with a server script, raw protocol clients (two that speak protocol
// 1.3 and one older client), the script console (SCRIPT_LOG), GAME.NETWORK both ways
// (SCRIPT_MSG), GAME.CHAT (SCRIPT_CHAT / CHAT), the custom sync values (SCRIPT_SYNC), the
// exact v7 rate limits, the server hooks, the round lifecycle (globals persist on the
// server, onServerClosed on a map change), the server half of `scripts` never reaching a
// client, the corpus server scripts run by the host, and a compiled-JS server script run
// through the importer (WP7).
// Run from the repository root: ./<build>/ks_server_test
#include <server_sandbox.h>
#include <server.h>
#include <ks_server.h>
#include <ks_game.h>
#include <ks_net.h>
#include <triggers_v7.h>
#include <map_catalog.h>
#include <knet.h>
#include <shared.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;
static int g_checks;

#define CHECK(cond, ...) do { \
    g_checks++; \
    if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_failures++; } \
} while (0)

// ------------------------------------------------------------------ test clock

static double g_ms = 1.7e12;
static double test_clock(void) { return g_ms; }

// ------------------------------------------------------------------ raw clients

#define MAX_LOG 512
#define MAX_MSG 512

typedef struct {
    char id[KS_NET_ID_MAX + 1];
    char json[512]; // GAME.log form of the data (decoded in the client heap)
} Msg;

typedef struct {
    int fd;
    int minor;
    int uid;
    NetPacketBuffer buffer;
    int closed;
    char logs[MAX_LOG][KS_NET_LOG_TEXT_MAX + 1];
    int log_count;
    Msg msgs[MAX_MSG];
    int msg_count;
    KsNetReassembler in;
    int script_chat, plain_chat, syncs, script_packets, errors;
    char last_chat[NET_CHAT_TEXT_SIZE];
    uint32_t last_color;
    uint8_t last_sync[NET_MAX_PAYLOAD];
    uint16_t last_sync_len;
    uint32_t map_hash;
    char *map_json;
    size_t map_size;
} Client;

static KsScript *g_cs; // a client-side heap to decode messages in

static void send_packet(Client *c, const int type, const void *payload, const uint16_t length)
{
    CHECK(net_send_packet(c->fd, (NetPacketType) type, payload, length), "send type %d", type);
}

static int client_send_fn(void *user, const int type, const void *payload, const uint16_t len)
{
    Client *c = user;
    return net_send_packet(c->fd, (NetPacketType) type, payload, len);
}

static void handle(Client *c, const NetPacket *p)
{
    switch (p->type) {
        case NET_PACKET_PING:
            send_packet(c, NET_PACKET_PONG, p->payload, p->length);
            break;
        case NET_PACKET_SPAWN: {
            NetSpawnPacket s = {0};
            memcpy(&s, p->payload, p->length < sizeof(s) ? p->length : sizeof(s));
            if (s.is_you) c->uid = s.uid;
            break;
        }
        case NET_PACKET_ERROR:
            c->errors++;
            break;
        case NET_PACKET_CHAT: {
            NetChatPacket chat;
            if (p->length != sizeof(chat)) break;
            memcpy(&chat, p->payload, sizeof(chat));
            if (chat.kind == NET_CHAT_SERVER) {
                c->plain_chat++;
                snprintf(c->last_chat, sizeof(c->last_chat), "%s", chat.text);
            }
            break;
        }
        case NET_PACKET_MAP_INFO: {
            NetMapInfoPacket info;
            memcpy(&info, p->payload, sizeof(info) < p->length ? sizeof(info) : p->length);
            c->map_hash = info.hash;
            break;
        }
        case NET_PACKET_MAP_DATA: {
            NetMapDataPacket d;
            memset(&d, 0, sizeof(d));
            memcpy(&d, p->payload, p->length);
            if (d.offset + d.length > c->map_size) {
                c->map_json = realloc(c->map_json, d.offset + d.length + 1);
                c->map_size = d.offset + d.length;
                c->map_json[c->map_size] = 0;
            }
            memcpy(c->map_json + d.offset, d.data, d.length);
            break;
        }
        case NET_PACKET_SCRIPT_LOG: {
            c->script_packets++;
            int err = 0;
            if (c->log_count < MAX_LOG && ks_net_decode_log(p->payload, p->length, &err, c->logs[c->log_count], sizeof(c->logs[0])))
                c->log_count++;
            break;
        }
        case NET_PACKET_SCRIPT_CHAT: {
            c->script_packets++;
            NetScriptChatPacket chat;
            if (p->length != sizeof(chat)) break;
            memcpy(&chat, p->payload, sizeof(chat));
            c->script_chat++;
            c->last_color = chat.color;
            snprintf(c->last_chat, sizeof(c->last_chat), "%s", chat.chat.text);
            break;
        }
        case NET_PACKET_SCRIPT_SYNC:
            c->script_packets++;
            c->syncs++;
            memcpy(c->last_sync, p->payload, p->length);
            c->last_sync_len = p->length;
            break;
        case NET_PACKET_SCRIPT_MSG: {
            c->script_packets++;
            const uint8_t *body = NULL;
            size_t len = 0;
            if (ks_net_reassemble(&c->in, p->payload, p->length, &body, &len) != 1 || c->msg_count >= MAX_MSG) break;
            Msg *m = &c->msgs[c->msg_count];
            KsValue data;
            if (!ks_net_decode_msg(g_cs, body, len, m->id, sizeof(m->id), &data)) break;
            ks_to_string(g_cs, data, m->json, sizeof(m->json));
            if (data.tag == KS_OBJ) { // flatten {a: x, b: y} as "a=x b=y"
                const char *keys[] = {"status", "from", "text", "n"};
                m->json[0] = 0;
                for (int k = 0; k < 4; k++) {
                    KsValue v = ks_obj_get(g_cs, data, keys[k]);
                    if (v.tag == KS_UNDEF) continue;
                    char val[200];
                    ks_to_string(g_cs, v, val, sizeof(val));
                    size_t o = strlen(m->json);
                    snprintf(m->json + o, sizeof(m->json) - o, "%s%s=%s", o ? " " : "", keys[k], val);
                }
            }
            c->msg_count++;
            break;
        }
        default:
            break;
    }
}

static void drain(Client *c)
{
    uint8_t bytes[512];
    int n;
    while ((n = sandbox_receive(c->fd, bytes, sizeof(bytes))) > 0) {
        CHECK(net_buffer_push(&c->buffer, bytes, (size_t) n), "buffer overflow");
        NetPacket packet;
        while (net_buffer_next(&c->buffer, &packet) > 0) handle(c, &packet);
    }
    if (n < 0) c->closed = 1;
}

static Client *g_clients[4];
static int g_client_count;

static void pump(const int frames)
{
    for (int f = 0; f < frames; f++) {
        g_ms += 1000.0 / 60.0;
        sandbox_step(1.0 / 60.0);
        for (int i = 0; i < g_client_count; i++)
            if (!g_clients[i]->closed) drain(g_clients[i]);
    }
}

static Client *connect_client(const int minor)
{
    Client *c = calloc(1, sizeof(*c));
    c->fd = sandbox_connect();
    c->minor = minor;
    c->uid = -1;
    CHECK(c->fd >= NET_LOOPBACK_FD_BASE, "connect");
    const NetHelloPacket hello = {NET_PROTOCOL_MAJOR, (uint8_t) minor, 0};
    send_packet(c, NET_PACKET_HELLO, &hello, sizeof(hello));
    NetJoinPacket join;
    memset(&join, 0, sizeof(join));
    send_packet(c, NET_PACKET_JOIN, &join, (uint16_t) NET_JOIN_PACKET_V2_SIZE);
    NetEntPacket ent = {.class_index = 0, .secondary_index = -1, .cosmetics = PLAYER_COSMETICS_NONE, .slid_cont = 1, .lag_comp = 1.0f};
    send_packet(c, NET_PACKET_ENT, &ent, (uint16_t) NET_ENT_PACKET_V7_SIZE);
    g_clients[g_client_count++] = c;
    return c;
}

static void free_clients(void)
{
    for (int i = 0; i < g_client_count; i++) {
        ks_net_reassembler_free(&g_clients[i]->in);
        free(g_clients[i]->map_json);
        free(g_clients[i]);
    }
    g_client_count = 0;
}

static int has_log(const Client *c, const char *line)
{
    for (int i = 0; i < c->log_count; i++)
        if (!strcmp(c->logs[i], line)) return 1;
    return 0;
}

static int count_log_prefix(const Client *c, const char *prefix)
{
    int n = 0;
    for (int i = 0; i < c->log_count; i++) n += !strncmp(c->logs[i], prefix, strlen(prefix));
    return n;
}

static const Msg *find_msg(const Client *c, const char *id, const int nth)
{
    int seen = 0;
    for (int i = 0; i < c->msg_count; i++)
        if (!strcmp(c->msgs[i].id, id) && seen++ == nth) return &c->msgs[i];
    return NULL;
}

static int count_msg(const Client *c, const char *id)
{
    int n = 0;
    for (int i = 0; i < c->msg_count; i++) n += !strcmp(c->msgs[i].id, id);
    return n;
}

// GAME.NETWORK.send from the client script: {key: value} (num or str)
static void client_msg(Client *c, const char *id, const char *key, const char *str_value, const double num_value)
{
    KsValue data = ks_obj_new(g_cs);
    if (key) ks_obj_set(g_cs, data, key, str_value ? ks_str(g_cs, str_value, strlen(str_value)) : ks_num(num_value));
    uint16_t seq = 0;
    CHECK(ks_net_send_msg(g_cs, &seq, id, data, client_send_fn, c), "client send %s", id);
}

// ------------------------------------------------------------------ maps

static const char *k_script =
    "num rounds = 0;\n"
    "num counted = 0;\n"
    "public action start() { rounds += 1; GAME.log(\"start\", toStr rounds); }\n"
    "public action update(num delta) { }\n"
    "public action onPlayerSpawn(str id) {\n"
    "    obj p = GAME.PLAYERS.findByID(id);\n"
    "    p.myVal = 5; p.myStr = \"hi\"; p.myObj = {a: 1};\n"
    "    p.registerSyncValues(str[\"myVal\", \"myStr\", \"myObj\"]);\n"
    "    GAME.log(\"spawn\", id, toStr (p.username == \"\"), toStr lengthOf GAME.PLAYERS.list());\n"
    "}\n"
    "public action onPlayerDamage(str id, str doerID, num amount) { GAME.log(\"damage\", id, doerID, toStr amount); }\n"
    "public action onPlayerDeath(str id, str killerID) { GAME.log(\"death\", id, killerID); }\n"
    "public action onCustomTrigger(str playerID, str customParam, num value) { GAME.log(\"trigger\", playerID, customParam, toStr value); }\n"
    "public bool action shouldTrigger(str playerID, num sid, str param) { GAME.log(\"should\", playerID, toStr sid, param); return param == \"go\"; }\n"
    "public action onChatMessage(str msg, str playerID) { GAME.log(\"chat\", msg, playerID); }\n"
    "public action onPlayerLeave(str playerID) { GAME.log(\"leave\", playerID); }\n"
    "public action onGameEnd() { GAME.log(\"end\"); }\n"
    "public action onServerClosed() { GAME.log(\"closed\"); }\n"
    "public action onNetworkMessage(str id, obj data, str playerID) {\n"
    "    if (id == \"check\") { GAME.NETWORK.send(\"check\", {status: (num) data.age >= 13 ? \"Passed\" : \"Failed\"}, playerID); }\n"
    "    if (id == \"relay\") { GAME.NETWORK.broadcast(\"relayed\", {from: playerID, text: (str) data.text}); }\n"
    "    if (id == \"count\") { counted += 1; GAME.log(\"count\", toStr counted); }\n"
    "    if (id == \"burst\") {\n"
    "        num b = 0; num s = 0;\n"
    "        for (num i = 0; i < 12; i++) { if (GAME.NETWORK.broadcast(\"b\", i)) { b += 1; } }\n"
    "        for (num i = 0; i < 25; i++) { if (GAME.NETWORK.send(\"s\", i, playerID)) { s += 1; } }\n"
    "        GAME.log(\"burst\", toStr b, toStr s);\n"
    "        for (num i = 0; i < 5; i++) { GAME.CHAT.send(playerID, \"line \" + toStr i, \"#0f0\"); }\n"
    "        GAME.CHAT.broadcast(\"to every/one!\");\n"
    "    }\n"
    "    if (id == \"big\") {\n"
    "        str t = \"\"; for (num i = 0; i < 2001; i++) { t += \"a\"; }\n"
    "        str u = \"\"; for (num i = 0; i < 2000; i++) { u += \"a\"; }\n"
    "        GAME.log(\"big\", toStr GAME.NETWORK.send(\"big\", t, playerID), toStr GAME.NETWORK.send(\"big\", u, playerID),\n"
    "                 toStr GAME.NETWORK.send(\"big\", u, \"\"));\n"
    "    }\n"
    "    if (id == \"spam\") { for (num i = 0; i < 30; i++) { GAME.log(\"spam\", toStr i); } }\n"
    "    if (id == \"freeze\") { GAME.TIME.freeze(); }\n"
    "    if (id == \"unfreeze\") { GAME.TIME.unfreeze(); }\n"
    "    if (id == \"nosync\") { GAME.DEFAULT.disableServerSync(); }\n"
    "    if (id == \"oops\") { num[] a = num[]; GAME.log((str) a[3]); }\n"
    "    if (id == \"kick\") { GAME.ADMIN.kick(playerID); }\n"
    "}\n";

static cJSON *with_scripts(const char *key, const char *server, const char *client)
{
    cJSON *map = cJSON_Duplicate(map_catalog_json((size_t) map_catalog_index_of_key(key)), 1);
    cJSON_DeleteItemFromObject(map, "scripts");
    cJSON *scripts = cJSON_AddObjectToObject(map, "scripts");
    if (server) cJSON_AddStringToObject(scripts, "server", server);
    if (client) cJSON_AddStringToObject(scripts, "client", client);
    return map;
}

static Player *server_player(const int uid) { return game_find_player(&g_server->game, uid); }

// ------------------------------------------------------------------ tests

static void test_host(void)
{
    // map 0: the test script (raw source); map 1: no script; map 2: a base64 script (1.1)
    cJSON *maps_json[3] = {
        with_scripts("311_burg", k_script, "public action start() { }"),
        with_scripts("311_littletown", NULL, NULL),
        // base64 of: public action start() { GAME.log("b64 start"); }
        with_scripts("311_littletown", "cHVibGljIGFjdGlvbiBzdGFydCgpIHsgR0FNRS5sb2coImI2NCBzdGFydCIpOyB9", NULL),
    };
    const cJSON *maps[3] = {maps_json[0], maps_json[1], maps_json[2]};
    const SandboxConfig cfg = {.maps = maps, .map_count = 3, .map_index = 0, .mode_id = MODE_ID_FFA};
    ks_server_set_clock(test_clock);
    CHECK(sandbox_start(&cfg), "sandbox_start");
    CHECK(ks_game_script(&g_server->game) != NULL, "server script loaded");
    CHECK(ks_game_side(&g_server->game) == KS_SIDE_SERVER, "server side");

    Client *a = connect_client(NET_PROTOCOL_MINOR_V7);
    Client *b = connect_client(NET_PROTOCOL_MINOR_V7);
    Client *old = connect_client(2);
    pump(40);
    CHECK(a->uid >= 0 && b->uid >= 0 && old->uid >= 0, "all spawned: %d %d %d", a->uid, b->uid, old->uid);
    char aid[16], bid[16];
    snprintf(aid, sizeof(aid), "%d", a->uid);
    snprintf(bid, sizeof(bid), "%d", b->uid);

    // the startup start() ran before anyone connected; spawns reach the console of v7 peers
    char line[128];
    CHECK(count_log_prefix(a, "spawn ") == 3, "a sees 3 spawn lines: %d", count_log_prefix(a, "spawn "));
    CHECK(old->script_packets == 0, "old peer gets no KrunkScript packets (%d)", old->script_packets);

    // the server half of `scripts` never reaches a client (2.1): ask for the map JSON
    const NetMapRequestPacket request = {.hash = a->map_hash};
    send_packet(a, NET_PACKET_MAP_REQUEST, &request, sizeof(request));
    pump(2);
    CHECK(a->map_json && strstr(a->map_json, "\"client\":") != NULL, "MAP_DATA carries scripts.client");
    CHECK(a->map_json && strstr(a->map_json, "onServerClosed") == NULL && strstr(a->map_json, "\"server\":") == NULL,
          "MAP_DATA has no scripts.server");
    {
        size_t len = 0;
        char *src = ks_game_map_source(&g_server->game, KS_SIDE_SERVER, &len);
        CHECK(src && strstr(src, "onServerClosed"), "the server still has its script");
        free(src);
    }

    // custom sync values (2.6): [myVal, 5, myStr, "hi"] (objects are not sent), own player only
    pump(10);
    CHECK(a->syncs > 0 && old->syncs == 0, "SCRIPT_SYNC to v7 peers: %d / old %d", a->syncs, old->syncs);
    {
        KsValue self = ks_obj_new(g_cs);
        ks_obj_set(g_cs, self, "myVal", ks_num(0));
        ks_obj_set(g_cs, self, "myObj", ks_obj_new(g_cs));
        const int written = ks_net_apply_sync(g_cs, self, a->last_sync, a->last_sync_len);
        CHECK(written == 1, "sync writes only existing non-object values: %d", written);
        CHECK(ks_to_number(ks_obj_get(g_cs, self, "myVal")) == 5.0, "myVal synced");
        CHECK(ks_obj_get(g_cs, self, "myStr").tag == KS_UNDEF, "myStr not created on the client");
    }

    // Basic Networking: client -> server -> that client
    client_msg(a, "check", "age", NULL, 20);
    client_msg(b, "check", "age", NULL, 9);
    pump(3);
    const Msg *ma = find_msg(a, "check", 0), *mb = find_msg(b, "check", 0);
    CHECK(ma && !strcmp(ma->json, "status=Passed"), "a check: %s", ma ? ma->json : "(none)");
    CHECK(mb && !strcmp(mb->json, "status=Failed"), "b check: %s", mb ? mb->json : "(none)");
    CHECK(count_msg(a, "check") == 1 && count_msg(b, "check") == 1, "send goes to one player");

    // two clients: a relays to everyone through the server
    client_msg(a, "relay", "text", "hello b", 0);
    pump(3);
    const Msg *ra = find_msg(a, "relayed", 0), *rb = find_msg(b, "relayed", 0);
    snprintf(line, sizeof(line), "from=%s text=hello b", aid);
    CHECK(ra && !strcmp(ra->json, line), "a relayed: %s", ra ? ra->json : "(none)");
    CHECK(rb && !strcmp(rb->json, line), "b relayed: %s", rb ? rb->json : "(none)");
    CHECK(old->script_packets == 0, "old peer: still nothing");

    // a long message is chunked (several SCRIPT_MSG packets) and reassembled
    {
        char text[1500];
        memset(text, 'x', sizeof(text) - 1);
        text[sizeof(text) - 1] = 0;
        client_msg(a, "relay", "text", text, 0);
        pump(3);
        const Msg *big = find_msg(b, "relayed", 1);
        CHECK(big != NULL, "chunked relay arrives");
    }

    // rate limits in a fresh window (3.0): broadcast 10 -> 9 pass, send 20/target -> 19, CHAT.send 3 -> 2
    g_ms += 1000.0;
    pump(1);
    const int b_before = count_msg(b, "b"), chats_before = a->script_chat, old_plain = old->plain_chat;
    client_msg(a, "burst", NULL, NULL, 0);
    pump(3);
    CHECK(has_log(a, "burst 9 19"), "burst 9 19 (off-by-one of f2123)");
    CHECK(count_msg(b, "b") - b_before == 9, "b got 9 broadcasts: %d", count_msg(b, "b") - b_before);
    CHECK(count_msg(a, "s") == 19 && count_msg(b, "s") == 0, "19 sends to a: %d (b %d)", count_msg(a, "s"), count_msg(b, "s"));
    CHECK(a->script_chat - chats_before == 3, "a: 2 CHAT.send lines + 1 broadcast: %d", a->script_chat - chats_before);
    CHECK(!strcmp(a->last_chat, "to everyone") && a->last_color == 0xfc03ec, "broadcast text filtered: '%s' %06x", a->last_chat, a->last_color);
    CHECK(old->plain_chat - old_plain == 1, "old peer gets the broadcast as a plain CHAT: %d", old->plain_chat - old_plain);
    // the same window: everything refused
    client_msg(a, "burst", NULL, NULL, 0);
    pump(3);
    CHECK(has_log(a, "burst 0 0"), "same window: burst 0 0");
    // the next window opens again
    g_ms += 1000.0;
    pump(1);
    client_msg(a, "burst", NULL, NULL, 0);
    pump(3);
    CHECK(count_log_prefix(a, "burst 9 19") == 2, "next window: burst 9 19 again");

    // payload size: 2001 chars refused, 2000 sent; send without a playerID refused
    g_ms += 1000.0;
    pump(1);
    client_msg(a, "big", NULL, NULL, 0);
    pump(3);
    CHECK(has_log(a, "big false true false"), "2000-char rule");

    // inbound limit: 20 messages per player per second reach onNetworkMessage
    g_ms += 1000.0;
    pump(1);
    for (int i = 0; i < 25; i++) client_msg(b, "count", NULL, NULL, 0);
    pump(1);
    CHECK(has_log(a, "count 20") && !has_log(a, "count 21"), "inbound 20/s");

    // console rate limit: 20 lines per second, then one notice
    g_ms += 1000.0;
    pump(1);
    const int logs_before = a->log_count;
    client_msg(a, "spam", NULL, NULL, 0);
    pump(1);
    CHECK(a->log_count - logs_before == 21 && has_log(a, "... (log rate limited)"), "log limit: %d lines", a->log_count - logs_before);

    // a runtime error is contained and logged; the script keeps running
    g_ms += 1000.0;
    pump(1);
    client_msg(a, "oops", NULL, NULL, 0);
    pump(1);
    CHECK(count_log_prefix(a, "RUNTIME ERROR: ") == 1, "runtime error logged");
    client_msg(a, "count", NULL, NULL, 0);
    pump(1);
    CHECK(has_log(a, "count 21"), "still running after the error");

    // hooks: chat, damage, death, custom trigger (action 58 through Game.script_hooks)
    const char *chat_text = "hi script";
    NetChatSendPacket chat;
    memset(&chat, 0, sizeof(chat));
    snprintf(chat.text, sizeof(chat.text), "%s", chat_text);
    send_packet(b, NET_PACKET_CHAT_SEND, &chat, (uint16_t) (strlen(chat.text) + 1));
    pump(2);
    snprintf(line, sizeof(line), "chat hi script %s", bid);
    CHECK(has_log(a, line), "onChatMessage");
    CHECK(g_server->game.script_hooks && g_server->game.script_hooks->on_custom_trigger, "trigger hooks registered");
    if (g_server->game.script_hooks && g_server->game.script_hooks->on_custom_trigger)
        g_server->game.script_hooks->on_custom_trigger(&g_server->game, "NONE", "First", 3);
    pump(1);
    CHECK(has_log(a, "trigger NONE First 3"), "onCustomTrigger");
    // v10 trigger condition 12: shouldTrigger's return value (Game.script_hooks.should_trigger)
    const TriggerScriptHooks *hooks = g_server->game.script_hooks;
    CHECK(hooks && hooks->should_trigger, "shouldTrigger hook registered");
    if (hooks && hooks->should_trigger) {
        CHECK(hooks->should_trigger(&g_server->game, "7", 12, "go") == 1, "shouldTrigger true");
        CHECK(hooks->should_trigger(&g_server->game, "7", 12, "stop") == 0, "shouldTrigger false");
    }
    pump(1);
    CHECK(has_log(a, "should 7 12 go") && has_log(a, "should 7 12 stop"), "shouldTrigger arguments");
    Player *pb = server_player(b->uid), *pa = server_player(a->uid);
    if (pb && pa) {
        player_change_health(pb, pa, 10.0f, 0);
        pump(1);
        snprintf(line, sizeof(line), "damage %s %s 10", bid, aid);
        CHECK(has_log(a, line), "onPlayerDamage: %s", line);
        player_kill(pb, pa, NULL, 0);
        pump(1);
        snprintf(line, sizeof(line), "death %s %s", bid, aid);
        CHECK(has_log(a, line), "onPlayerDeath: %s", line);
    }

    // TIME.freeze stops the round clock until unfreeze
    client_msg(a, "freeze", NULL, NULL, 0);
    pump(2);
    const float frozen_at = g_server->game.round_timer;
    pump(60);
    CHECK(ks_game_round_frozen(&g_server->game) && g_server->game.round_timer == frozen_at, "frozen clock %.3f", (double) g_server->game.round_timer);
    client_msg(a, "unfreeze", NULL, NULL, 0);
    pump(30);
    CHECK(!ks_game_round_frozen(&g_server->game) && g_server->game.round_timer < frozen_at, "clock runs again");

    // the same map again (next round): globals persist, start() runs again
    sandbox_host(0, -1);
    pump(5);
    CHECK(has_log(a, "start 2"), "server globals persist across rounds (start 2)");

    // round end hook
    game_end_round(&g_server->game);
    pump(1);
    CHECK(has_log(a, "end"), "onGameEnd");

    // leave
    sandbox_disconnect(b->fd);
    b->closed = 1;
    pump(3);
    snprintf(line, sizeof(line), "leave %s", bid);
    CHECK(has_log(a, line), "onPlayerLeave");

    // another map: onServerClosed for the old script; a map without a script runs nothing
    sandbox_host(1, -1);
    pump(5);
    CHECK(has_log(a, "closed"), "onServerClosed on a map change");
    CHECK(ks_game_script(&g_server->game) == NULL, "no script on a map without one");
    // the base64 script map, then back to map 0: a fresh instance (start 1 again)
    sandbox_host(2, -1);
    pump(5);
    CHECK(has_log(a, "b64 start"), "base64 scripts.server");
    const int starts = count_log_prefix(a, "start 1");
    sandbox_host(0, -1);
    pump(5);
    CHECK(count_log_prefix(a, "start 1") == starts + 1, "a new map script starts over");

    // ADMIN.kick (a respawns first: the round end left nobody playing)
    NetEntPacket ent = {.class_index = 0, .secondary_index = -1, .cosmetics = PLAYER_COSMETICS_NONE, .slid_cont = 1, .lag_comp = 1.0f};
    send_packet(a, NET_PACKET_ENT, &ent, (uint16_t) NET_ENT_PACKET_V7_SIZE);
    pump(5);
    client_msg(a, "kick", NULL, NULL, 0);
    pump(5);
    CHECK(a->errors > 0 && a->closed, "kicked by the script (errors %d closed %d uid %d alive %d)", a->errors, a->closed, a->uid,
          server_player(a->uid) ? server_player(a->uid)->active : -1);

    // DEFAULT.disableServerSync is per script and ends with it
    ks_game_set_flag(&g_server->game, KS_FLAG_NO_SERVER_SYNC, 1);
    CHECK(ks_game_flag(&g_server->game, KS_FLAG_NO_SERVER_SYNC), "flag set");

    sandbox_stop();
    CHECK(g_server == NULL, "stopped");
    free_clients();
    for (int i = 0; i < 3; i++) cJSON_Delete(maps_json[i]);
}

// The corpus server scripts (shared/tests/ks/corpus/*_server.krnk) under the server host with
// two players, inputs, a network message and custom triggers: no runtime error, and the
// templates' visible behaviour (Basic Networking reply, Triggers & Scripts log lines).
static char *read_text(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t) n + 1);
    if (buf) buf[fread(buf, 1, (size_t) n, f)] = 0;
    fclose(f);
    return buf;
}

static void test_corpus(void)
{
    static const char *names[] = {
        "tpl01_Simple_io_Game", "tpl03_Moving_Objects", "tpl04_Bouncing_Ball", "tpl05_Popup_Window",
        "tpl06_Flashlight", "tpl10_Basic_Networking", "tpl11_Networking_Objects", "tpl13_Jetpacks",
        "tpl14_Sprinting", "tpl15_Triggers_Scripts", "tpl16_Game_of_Life",
    };
    for (size_t t = 0; t < sizeof(names) / sizeof(names[0]); t++) {
        char path[256];
        snprintf(path, sizeof(path), "shared/tests/ks/corpus/%s_server.krnk", names[t]);
        char *server = read_text(path);
        snprintf(path, sizeof(path), "shared/tests/ks/corpus/%s_client.krnk", names[t]);
        char *client = read_text(path);
        CHECK(server != NULL, "read %s", names[t]);
        if (!server) continue;
        cJSON *map = with_scripts("311_burg", server, client);
        const cJSON *maps[1] = {map};
        const SandboxConfig cfg = {.maps = maps, .map_count = 1, .map_index = 0, .mode_id = MODE_ID_FFA};
        CHECK(sandbox_start(&cfg), "%s: start", names[t]);
        CHECK(ks_game_script(&g_server->game) != NULL, "%s: compiled and loaded", names[t]);
        Client *a = connect_client(NET_PROTOCOL_MINOR_V7);
        Client *b = connect_client(NET_PROTOCOL_MINOR_V7);
        pump(20);
        for (int seq = 0; seq < 40; seq++) { // walk, jump and crouch through onPlayerUpdate
            const NetInputPacket input = {.seq = seq, .move_dir = seq % 8, .delta = 1.0f / 60.0f, .flags = (uint8_t) ((seq & 4) ? NET_INPUT_JUMP : 0)};
            send_packet(a, NET_PACKET_INPUT, &input, sizeof(input));
            pump(1);
        }
        client_msg(a, "check", "age", NULL, 20);
        pump(3);
        if (g_server->game.script_hooks && g_server->game.script_hooks->on_custom_trigger) {
            g_server->game.script_hooks->on_custom_trigger(&g_server->game, "NONE", "1", 0);
            g_server->game.script_hooks->on_custom_trigger(&g_server->game, "NONE", "2", 0);
        }
        pump(3);
        CHECK(count_log_prefix(a, "RUNTIME ERROR") == 0, "%s: no runtime error (%s)", names[t], a->log_count ? a->logs[0] : "");
        if (!strcmp(names[t], "tpl10_Basic_Networking")) {
            const Msg *m = find_msg(a, "check", 0);
            CHECK(m && !strcmp(m->json, "status=Passed"), "Basic Networking: %s", m ? m->json : "(none)");
        }
        if (!strcmp(names[t], "tpl15_Triggers_Scripts") || !strcmp(names[t], "tpl11_Networking_Objects")) {
            CHECK(has_log(a, "First Trigger") && has_log(a, "Second Trigger") && has_log(b, "First Trigger"),
                  "%s: trigger log lines", names[t]);
        }
        sandbox_stop();
        free_clients();
        cJSON_Delete(map);
        free(server);
        free(client);
    }
}

// A server script shipped as the v7 compiler's JavaScript (1.16 lowering; the corpus maps only
// ship client scripts like this, the server host takes the same path): the importer runs it,
// a new round keeps its instance (the map text's hash is stable), its hooks work.
// ------------------------------------------------------------------ entities (v10 entity manager)

// The shared script (scripts.shared): onEntityInit copies the spawn params into the state, and
// simulateEntity emits one event on the first step after the spawn.
static const char *k_entity_shared =
    "public obj action onEntityInit(str id, str archetype, obj spawn) {\n"
    "    obj state = {};\n"
    "    state.speed = spawn.speed;\n"
    "    state.hops = 0;\n"
    "    return state;\n"
    "}\n"
    "public obj action simulateEntity(str id, str archetype, obj state, obj ctx, num delta) {\n"
    "    if (state.hops == 0 && ctx.tick > 0) {\n"
    "        state.hops = 1;\n"
    "        ctx.emit(\"hop\", 1.5, 2);\n"
    "    }\n"
    "    return state;\n"
    "}\n";

// The server script runs its scenario once the first player spawns (the clients are connected by
// then): the entity API, the damage path and the ray, with the hooks logged as they arrive.
static const char *k_entity_server =
    "num scenario = 0;\n"
    "public action onPlayerSpawn(str id) {\n"
    "    if (scenario != 0) { return; }\n"
    "    scenario = 1;\n"
    "    GAME.log(\"count\", toStr lengthOf GAME.ENTITIES.list());\n"
    "    obj e = GAME.ENTITIES.findById(\"7\");\n"
    "    GAME.log(\"found\", e.archetype, e.tag, toStr e.hp);\n"
    "    GAME.log(\"pos\", toStr e.position.x, toStr e.position.y);\n"
    "    GAME.log(\"speed\", toStr (num) e.getVar(\"speed\"));\n"
    "    obj b = GAME.ENTITIES.spawn(\"Crate\", 1, 2, 3, {tag: \"t2\", speed: 9});\n"
    "    GAME.log(\"spawned\", b.id, b.tag);\n"
    "    GAME.log(\"tagged\", toStr lengthOf GAME.ENTITIES.findByTag(\"t2\"));\n"
    "    GAME.log(\"damaged\", toStr e.damage(20, id), toStr e.hp);\n"
    "    e.position.x = 40;\n"
    "    GAME.log(\"moved\", toStr e.position.x);\n"
    "    GAME.log(\"destroyed\", toStr b.damage(100, id));\n"
    "    GAME.log(\"gone\", toStr notEmpty GAME.ENTITIES.findById(\"10000\"));\n"
    "    obj r = GAME.RAYCAST.from(-500, 2, -500, 1, 0, 0, 100);\n"
    "    GAME.log(\"ray\", toStr r.hit, r.type, toStr GAME.UTILS.math.round(r.dist));\n"
    "    GAME.log(\"fromPlayer\", toStr GAME.RAYCAST.fromPlayer(\"999\", 50).hit);\n"
    "}\n"
    "public action onEntitySpawn(str id, str archetype) { GAME.log(\"hook spawn\", id, archetype); }\n"
    "public action onEntityEvent(str id, str name, num a, num b) { GAME.log(\"hook event\", id, name, toStr a, toStr b); }\n"
    "public action onEntityDamage(str id, str doerID, num amount) { GAME.log(\"hook damage\", id, toStr notEmpty doerID, toStr amount); }\n"
    "public action onEntityDestroyed(str id, str doerID) { GAME.log(\"hook destroyed\", id); }\n";

// Catalogue map + an archetype "Crate" (hp 50, data {speed: 2}), an ENTITY instance 7 at (4, 2, 4)
// with spawn params {speed: 3}, and a static cube at (-480, 0, -500) for the ray.
static cJSON *with_entities(void)
{
    cJSON *map = with_scripts("311_burg", k_entity_server, NULL);
    cJSON *scripts = cJSON_GetObjectItem(map, "scripts");
    cJSON_AddStringToObject(scripts, "shared", k_entity_shared);
    cJSON *archetypes = cJSON_AddArrayToObject(map, "archetypes");
    cJSON *crate = cJSON_CreateObject();
    cJSON_AddNumberToObject(crate, "id", 1);
    cJSON_AddStringToObject(crate, "name", "Crate");
    cJSON_AddNumberToObject(crate, "coll", 1);
    cJSON_AddNumberToObject(crate, "hp", 50);
    cJSON *data = cJSON_AddObjectToObject(crate, "data");
    cJSON_AddNumberToObject(data, "speed", 2);
    cJSON *parts = cJSON_AddArrayToObject(crate, "objects");
    cJSON *part = cJSON_CreateObject();
    cJSON_AddNumberToObject(part, "i", 1);
    cJSON_AddItemToObject(part, "p", cJSON_CreateFloatArray((const float[]) {0, 0, 0}, 3));
    cJSON_AddItemToObject(part, "s", cJSON_CreateFloatArray((const float[]) {2, 2, 2}, 3));
    cJSON_AddItemToArray(parts, part);
    cJSON_AddItemToArray(archetypes, crate);

    cJSON *objects = cJSON_GetObjectItem(map, "objects");
    cJSON *entity = cJSON_CreateObject();
    cJSON_AddNumberToObject(entity, "i", 98);
    cJSON_AddItemToObject(entity, "p", cJSON_CreateFloatArray((const float[]) {4, 2, 4}, 3));
    cJSON_AddItemToObject(entity, "r", cJSON_CreateFloatArray((const float[]) {0, 0, 0}, 3));
    cJSON_AddNumberToObject(entity, "eid", 7);
    cJSON_AddNumberToObject(entity, "ar", 1);
    cJSON_AddStringToObject(entity, "tag", "crate-a");
    cJSON *sp = cJSON_AddObjectToObject(entity, "sp");
    cJSON_AddNumberToObject(sp, "speed", 3);
    cJSON_AddItemToArray(objects, entity);
    cJSON *cube = cJSON_CreateObject();
    cJSON_AddNumberToObject(cube, "i", 0);
    cJSON_AddItemToObject(cube, "p", cJSON_CreateFloatArray((const float[]) {-480, 0, -500}, 3));
    cJSON_AddItemToObject(cube, "s", cJSON_CreateFloatArray((const float[]) {4, 4, 4}, 3));
    cJSON_AddItemToArray(objects, cube);
    return map;
}

static void test_entities(void)
{
    cJSON *map = with_entities();
    const cJSON *maps[1] = {map};
    const SandboxConfig cfg = {.maps = maps, .map_count = 1, .map_index = 0, .mode_id = MODE_ID_FFA};
    CHECK(sandbox_start(&cfg), "entities: start");
    CHECK(ks_game_script(&g_server->game) != NULL, "entities: server script loaded");
    CHECK(ks_game_shared_source_hash(&g_server->game) != 0, "entities: scripts.shared compiled and loaded");
    CHECK(g_server->game.map->archetype_count == 1, "entities: the archetype is in the map (%zu)", g_server->game.map->archetype_count);
    CHECK(g_server->game.map->entity_instance_count == 1, "entities: the ENTITY instance is in the map (%zu)", g_server->game.map->entity_instance_count);

    Client *a = connect_client(NET_PROTOCOL_MINOR_V7);
    pump(40);
    CHECK(a->uid >= 0, "entities: spawned");
    // the scenario's lines (script console), then the hooks as the entity manager queues them
    static const char *const want[] = {
        "count 1", "found Crate crate-a 50", "pos 4 2", "speed 3", "spawned 10000 t2", "tagged 1",
        "damaged false 30", "moved 40", "destroyed true", "gone false", "ray true object 18", "fromPlayer false",
        "hook spawn 7 Crate", "hook spawn 10000 Crate", "hook event 7 hop 1.5 2",
        "hook damage 10000 true 100", "hook destroyed 10000",
    };
    for (size_t i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
        CHECK(has_log(a, want[i]), "entities: log \"%s\" (script console)", want[i]);
    }
    // entity 10000 is destroyed before its first step: only entity 7 emits its hop
    CHECK(count_log_prefix(a, "hook event ") == 1, "entities: one hop (the entity that stepped): %d", count_log_prefix(a, "hook event "));
    sandbox_stop();
    free_clients();
}

// GAME.COOKIES keys (v10 customMapData.id): a custom game's id only; no id, no cookies.
static void test_cookie_keys(void)
{
    char key[128];
    CHECK(!ks_cookie_storage_key("", "score", key, sizeof(key)), "cookies: a catalogue map (no custom id) keeps none");
    CHECK(!ks_cookie_storage_key(NULL, "score", key, sizeof(key)), "cookies: NULL custom id keeps none");
    CHECK(ks_cookie_storage_key("4711", "score", key, sizeof(key)) && !strcmp(key, "krkcst_4711_score"), "cookies: keyed by the custom id: %s", key);
    CHECK(!ks_cookie_storage_key("4711", "", key, sizeof(key)), "cookies: an empty key is refused");
    CHECK(!ks_cookie_storage_key("4711", "score", key, 8), "cookies: a key that does not fit is refused");
    // no migration: the old map-name key is never produced, so stored values under it stay unread
    CHECK(ks_cookie_storage_key("4711", "score", key, sizeof(key)) && !strstr(key, "311_burg"), "cookies: the map name is not part of the key");
}

static void test_compiled_js(void)
{
    char *prelude = read_text("shared/tests/ks/js/SniperRoyale2_client.js");
    CHECK(prelude != NULL, "read the compiled-JS fixture");
    if (!prelude) return;
    char *end = strstr(prelude, "let LC = 0;\n");
    CHECK(end != NULL, "prelude end");
    if (!end) {
        free(prelude);
        return;
    }
    end[strlen("let LC = 0;\n")] = 0;
    static const char body[] =
        "let V_rounds = 0;\n"
        "function V_start() {\ntry {\nLC = 0;\n(V_rounds)++;\n"
        "this.GAME.V_log.bind(this)(\"js start\", S(V_rounds));\n} catch (e) {EH(e.message)};\n}\n"
        "function V_onNetworkMessage(V_id, V_data, V_playerID) {\ntry {\nLC = 0;\nif (V_id === \"check\") {\n"
        "this.GAME.V_NETWORK.V_send.bind(this)(\"check\", {V_status: A(V_data.V_age,F) >= 13 ? \"Passed\" : \"Failed\"}, V_playerID);\n"
        "};\n} catch (e) {EH(e.message)};\n}\n"
        "return {start: V_start.bind(this), onNetworkMessage: V_onNetworkMessage.bind(this), update: function(){}};\n";
    const size_t plen = strlen(prelude);
    char *js = malloc(plen + sizeof(body));
    memcpy(js, prelude, plen);
    memcpy(js + plen, body, sizeof(body));
    free(prelude);

    cJSON *map = with_scripts("311_burg", js, NULL);
    const cJSON *maps[1] = {map};
    const SandboxConfig cfg = {.maps = maps, .map_count = 1, .map_index = 0, .mode_id = MODE_ID_FFA};
    CHECK(sandbox_start(&cfg), "compiled JS: start");
    CHECK(ks_game_script(&g_server->game) != NULL, "compiled JS: imported and loaded");
    Client *a = connect_client(NET_PROTOCOL_MINOR_V7);
    pump(20);
    sandbox_host(0, -1); // next round on the same map: the same instance, start() again
    pump(5);
    CHECK(has_log(a, "js start 2"), "compiled JS: globals persist across rounds (js start 2)");
    client_msg(a, "check", "age", NULL, 20);
    pump(3);
    const Msg *m = find_msg(a, "check", 0);
    CHECK(m && !strcmp(m->json, "status=Passed"), "compiled JS: onNetworkMessage reply %s", m ? m->json : "(none)");
    CHECK(count_log_prefix(a, "RUNTIME ERROR") == 0, "compiled JS: no runtime error");
    sandbox_stop();
    free_clients();
    cJSON_Delete(map);
    free(js);
}

int main(void)
{
    KsDiag err;
    KsProgram *prog = ks_compile("num x = 0;", 10, KS_SIDE_CLIENT, &err, NULL, 0, NULL);
    static const KsHost host = {KS_SIDE_CLIENT, NULL, NULL, NULL};
    g_cs = ks_script_new(prog, &host, NULL);
    test_host();
    test_corpus();
    test_compiled_js();
    test_entities();
    test_cookie_keys();
    ks_script_free(g_cs);
    ks_program_free(prog);
    if (g_failures) {
        printf("%d of %d checks failed\n", g_failures, g_checks);
        return 1;
    }
    printf("Test OK (%d checks)\n", g_checks);
    return 0;
}
