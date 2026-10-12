// KrunkScript persistence & services (doc/v7/krunkscript.md 2.6, 3.1.12, 3.1.22, WP6):
//   1. the storageUpdt data rules (shared/src/ks_storage_data.c): stripping, the key / value /
//      key-count messages in v7's order, UTF-16 lengths, the operation types;
//   2. GAME.STORAGE on the offline sandbox's file store: validation messages, the in-flight
//      locks, the 10 s limit, callbacks, "No data", transact; the Save Data / Load Data map
//      triggers next to the script's own keys; then an offline round trip (sandbox stopped and
//      started again: the data comes back from $XDG_CONFIG_HOME/krunknative);
//   3. the offline store through installed I/O (the client's html_storage, localStorage on
//      the web);
//   4. the kn_brain backend's frames and replies (brain sink): storage / storage_load /
//      map_reward, errors, "No data found", the link going down, giveKR and Reward KR;
//   5. ADMIN.ban: the banned address cannot come back.
// Run from the repository root: ./<build>/ks_storage_test
#include <server_sandbox.h>
#include <server.h>
#include <ks_server.h>
#include <ks_storage.h>
#include <ks_game.h>
#include <ks_net.h>
#include <triggers_v7.h>
#include <map_catalog.h>
#include <knet.h>
#include <shared.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#include <process.h>
#else
#include <unistd.h> // mkdtemp (macOS declares it here, not in stdlib.h)
#endif

static int g_failures;
static int g_checks;

#define CHECK(cond, ...) do { \
    g_checks++; \
    if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_failures++; } \
} while (0)

static double g_ms = 1.7e12;
static double test_clock(void) { return g_ms; }

// ------------------------------------------------------------------ raw clients (ks_server_test's)

#define MAX_LOG 512

typedef struct {
    int fd;
    int uid;
    NetPacketBuffer buffer;
    int closed;
    int errors;
    char logs[MAX_LOG][KS_NET_LOG_TEXT_MAX + 1];
    int log_count;
    int chats;
    char last_chat[NET_CHAT_TEXT_SIZE];
    int popups;
} Client;

static KsScript *g_cs;
static Client *g_clients[8];
static int g_client_count;

static void send_packet(Client *c, const int type, const void *payload, const uint16_t length)
{
    net_send_packet(c->fd, (NetPacketType) type, payload, length);
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
            c->chats++;
            snprintf(c->last_chat, sizeof(c->last_chat), "%s", chat.text);
            break;
        }
        case NET_PACKET_POPUP:
            c->popups++;
            break;
        case NET_PACKET_SCRIPT_LOG: {
            int err = 0;
            if (c->log_count < MAX_LOG && ks_net_decode_log(p->payload, p->length, &err, c->logs[c->log_count], sizeof(c->logs[0])))
                c->log_count++;
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
        net_buffer_push(&c->buffer, bytes, (size_t) n);
        NetPacket packet;
        while (net_buffer_next(&c->buffer, &packet) > 0) handle(c, &packet);
    }
    if (n < 0) c->closed = 1;
}

static void pump(const int frames)
{
    for (int f = 0; f < frames; f++) {
        g_ms += 1000.0 / 60.0;
        sandbox_step(1.0 / 60.0);
        for (int i = 0; i < g_client_count; i++)
            if (!g_clients[i]->closed) drain(g_clients[i]);
    }
}

static Client *connect_client(void)
{
    Client *c = calloc(1, sizeof(*c));
    c->fd = sandbox_connect();
    c->uid = -1;
    const NetHelloPacket hello = {NET_PROTOCOL_MAJOR, NET_PROTOCOL_MINOR_V7, 0};
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
    for (int i = 0; i < g_client_count; i++) free(g_clients[i]);
    g_client_count = 0;
}

static int has_log(const Client *c, const char *line)
{
    for (int i = 0; i < c->log_count; i++)
        if (!strcmp(c->logs[i], line)) return 1;
    return 0;
}

static int count_log(const Client *c, const char *line)
{
    int n = 0;
    for (int i = 0; i < c->log_count; i++) n += !strcmp(c->logs[i], line);
    return n;
}

static void dump_logs(const Client *c)
{
    for (int i = 0; i < c->log_count; i++) printf("    log: %s\n", c->logs[i]);
}

static void msg(Client *c, const char *id)
{
    uint16_t seq = 0;
    CHECK(ks_net_send_msg(g_cs, &seq, id, ks_obj_new(g_cs), client_send_fn, c), "client send %s", id);
}

static ClientConnection *conn_of(const Client *c)
{
    for (size_t i = 0; g_server && i < g_server->client_count; i++)
        if (g_server->clients[i].player && (int) g_server->clients[i].player->uid == c->uid) return &g_server->clients[i];
    return NULL;
}

// ------------------------------------------------------------------ 1. data rules

static void test_data_rules(void)
{
    KsStorageData d;
    ks_storage_data_init(&d);
    ks_storage_data_add(&d, "V_co,i'n\"s", "1,2,3");
    CHECK(!ks_storage_data_error(&d) && d.count == 1 && !strcmp(d.pairs[0].key, "V_coins") && !strcmp(d.pairs[0].value, "123"),
          ", ' \" are removed: %s=%s", d.pairs[0].key, d.pairs[0].value);

    ks_storage_data_init(&d);
    ks_storage_data_add(&d, "abcdefghijklmnopqrst", "v");   // 20: ok
    ks_storage_data_add(&d, "abcdefghijklmnopqrstu", "v");  // 21
    CHECK(d.count == 1 && ks_storage_data_error(&d) && !strcmp(ks_storage_data_error(&d), "Key too long"), "Key too long");
    ks_storage_data_init(&d);
    ks_storage_data_add(&d, "abcdefghijklmnopqrst,,,", "v"); // 20 after stripping
    CHECK(!ks_storage_data_error(&d), "the length counts after stripping");

    char value[256];
    memset(value, 'x', 101);
    value[101] = 0;
    ks_storage_data_init(&d);
    ks_storage_data_add(&d, "k", value);
    CHECK(d.error && !strcmp(d.error, "Value too long"), "Value too long");
    value[100] = 0;
    ks_storage_data_init(&d);
    ks_storage_data_add(&d, "k", value);
    CHECK(!ks_storage_data_error(&d), "100 characters are fine");
    // UTF-16 lengths: 10 emoji = 20 units (ok as a key), 11 = 22
    char emoji[64] = "";
    for (int i = 0; i < 10; i++) strcat(emoji, "\xF0\x9F\x98\x80");
    ks_storage_data_init(&d);
    ks_storage_data_add(&d, emoji, "v");
    CHECK(!ks_storage_data_error(&d), "10 emoji = 20 UTF-16 units");
    strcat(emoji, "\xF0\x9F\x98\x80");
    ks_storage_data_add(&d, emoji, "v");
    CHECK(d.error && !strcmp(d.error, "Key too long"), "11 emoji = 22 units");
    CHECK(ks_storage_u16len("\xC3\xA9") == 1 && ks_storage_u16len("\xF0\x9F\x98\x80") == 2, "u16len");

    // the key count is checked after the loop: 31 keys -> too many, unless a key failed first
    ks_storage_data_init(&d);
    for (int i = 0; i < 31; i++) {
        char k[8];
        snprintf(k, sizeof(k), "%d", i);
        ks_storage_data_add(&d, k, "1");
    }
    CHECK(!strcmp(ks_storage_data_error(&d), "Too many keys: max 30") && d.count == 30, "Too many keys: max 30");
    ks_storage_data_init(&d);
    for (int i = 0; i < 30; i++) ks_storage_data_add(&d, "k", "1");
    ks_storage_data_add(&d, "abcdefghijklmnopqrstuvwxyz", "1");
    CHECK(!strcmp(ks_storage_data_error(&d), "Key too long"), "a bad key wins over the count");
    CHECK(!strcmp(ks_storage_data_error(NULL), "Missing storage data"), "no data");

    // operation types
    char out[128];
    CHECK(ks_storage_apply(KS_STORAGE_SET, "1", "abc", out, sizeof(out)) == 1 && !strcmp(out, "abc"), "set");
    CHECK(ks_storage_apply(KS_STORAGE_SET_HIGHER, "5", "7", out, sizeof(out)) == 1 && !strcmp(out, "7"), "higher: 7 > 5");
    CHECK(ks_storage_apply(KS_STORAGE_SET_HIGHER, "5", "3", out, sizeof(out)) == 0, "higher: 3 kept 5");
    CHECK(ks_storage_apply(KS_STORAGE_SET_HIGHER, NULL, "3", out, sizeof(out)) == 1, "higher: no row");
    CHECK(ks_storage_apply(KS_STORAGE_SET_HIGHER, "abc", "3", out, sizeof(out)) == 1, "higher: old not a number");
    CHECK(ks_storage_apply(KS_STORAGE_SET_LOWER, "5", "3", out, sizeof(out)) == 1 && !strcmp(out, "3"), "lower");
    CHECK(ks_storage_apply(KS_STORAGE_SET_LOWER, "5", "5", out, sizeof(out)) == 0, "lower: equal kept");
    CHECK(ks_storage_apply(KS_STORAGE_INCREASE, NULL, "5", out, sizeof(out)) == 1 && !strcmp(out, "5"), "increase from nothing");
    CHECK(ks_storage_apply(KS_STORAGE_INCREASE, "0.1", "0.2", out, sizeof(out)) == 1 && !strcmp(out, "0.30000000000000004"),
          "increase formats like JS: %s", out);
    CHECK(ks_storage_apply(KS_STORAGE_INCREASE, "12abc", "1", out, sizeof(out)) == 1 && !strcmp(out, "13"), "parseFloat of the old value");
    CHECK(ks_storage_apply(KS_STORAGE_DECREASE, "10", "2.5", out, sizeof(out)) == 1 && !strcmp(out, "7.5"), "decrease");
    CHECK(ks_storage_apply(KS_STORAGE_INCREASE, "1", "x", out, sizeof(out)) == 0, "a non-number does not count");
    CHECK(ks_storage_apply(KS_STORAGE_DELETE, "1", "", out, sizeof(out)) == -1, "delete");
    CHECK(ks_storage_apply(KS_STORAGE_DELETE, NULL, "", out, sizeof(out)) == 0, "delete nothing");
    CHECK(!strcmp(ks_storage_script_key("V_coins"), "coins") && !strcmp(ks_storage_script_key("trig"), "trig"), "script keys");
    printf("data rules done\n");
}

// ------------------------------------------------------------------ the map

static const char *k_script =
    "str acct = \"\";\n"
    "action saved(obj data, bool ok, str name) { GAME.log(\"saved\", toStr ok, name, (str) data.coins, (str) data.gems); }\n"
    "action updated(obj data, bool ok, str name) { GAME.log(\"updated\", toStr ok, name, (str) data.coins); }\n"
    "action loaded(obj data, bool ok, str name) { GAME.log(\"loaded\", toStr ok, name, (str) data.coins, (str) data.gems, (str) data.trig); }\n"
    "action failed(obj data, bool ok, str name) { GAME.log(\"failed\", toStr ok, name); }\n"
    "public action start() { }\n"
    "public action onNetworkMessage(str id, obj data, str playerID) {\n"
    "    obj p = GAME.PLAYERS.findByID(playerID);\n"
    "    acct = (str) p.accountName;\n"
    "    if (id == \"acct\") { GAME.log(\"acct\", acct, (str) p.accountID); }\n"
    "    if (id == \"set2\") { GAME.STORAGE.set(acct, {coins: 5, gems: \"a,b'c\"}, \"private\", saved);\n"
    "                         GAME.STORAGE.set(acct, {coins: 6}, \"private\", saved); }\n"
    "    if (id == \"set\") { GAME.STORAGE.set(acct, {coins: 5, gems: \"a,b'c\"}, \"private\", saved); }\n"
    "    if (id == \"update\") { GAME.STORAGE.update(acct, {coins: 2}, \"private\", updated); }\n"
    "    if (id == \"load\") { GAME.STORAGE.load(acct, \"\", loaded); }\n"
    "    if (id == \"loadnone\") { GAME.STORAGE.load(\"nobody\", \"\", loaded); }\n"
    "    if (id == \"loadother\") { GAME.STORAGE.load(acct, \"Some Other Map\", loaded); }\n"
    "    if (id == \"bad\") {\n"
    "        GAME.STORAGE.set(\"k1\", {abcdefghijklmnopqrs: 1}, \"private\", failed);\n"
    "        GAME.STORAGE.set(\"k2\", {abcdefghijklmnopqr: 1}, \"private\", failed);\n"
    "        str long = \"\"; for (num i = 0; i < 101; i++) { long += \"x\"; }\n"
    "        GAME.STORAGE.set(\"k3\", {v: long}, \"private\", failed);\n"
    "        num[] many = num[]; for (num i = 0; i < 31; i++) { addTo many i; }\n"
    "        GAME.STORAGE.set(\"k5\", many, \"private\", failed);\n"
    "        GAME.STORAGE.set(\"k6\", 5, \"private\", failed);\n"
    "        GAME.STORAGE.transact(\"k7\", {a: 1}, \"private\", failed);\n"
    "        GAME.STORAGE.update(\"k8\", {a: 1}, \"global\");\n"
    "    }\n"
    "    if (id == \"kr\") { GAME.PAYMENTS.giveKR(p, 5); GAME.PAYMENTS.giveKR(playerID, 5); }\n"
    "    if (id == \"ban\") { GAME.ADMIN.ban(playerID); }\n"
    "}\n";

// 311_burg with the script and two "player spawn" triggers: Save Data "Trig" (increase by 7)
// and Load Data
static cJSON *storage_map(void)
{
    cJSON *map = cJSON_Duplicate(map_catalog_json((size_t) map_catalog_index_of_key("311_burg")), 1);
    cJSON_DeleteItemFromObject(map, "scripts");
    cJSON *scripts = cJSON_AddObjectToObject(map, "scripts");
    cJSON_AddStringToObject(scripts, "server", k_script);
    cJSON *objects = cJSON_GetObjectItem(map, "objects");
    cJSON_AddItemToArray(objects, cJSON_Parse("{\"i\":29,\"p\":[0,-500,0],\"s\":[10,10,10],\"te\":17,\"tas\":[{\"ta\":49,\"tct\":\"Trig\",\"tc\":7,\"tc2\":3}]}"));
    cJSON_AddItemToArray(objects, cJSON_Parse("{\"i\":29,\"p\":[50,-500,0],\"s\":[10,10,10],\"te\":17,\"tas\":[{\"ta\":57}]}"));
    return map;
}

static int start(const cJSON *map)
{
    const cJSON *maps[1] = {map};
    const SandboxConfig cfg = {.maps = maps, .map_count = 1, .map_index = 0, .mode_id = MODE_ID_FFA};
    ks_server_set_clock(test_clock);
    return sandbox_start(&cfg);
}

static char g_dir[512];

static char *read_store(const char *map_name)
{
    char key[64], path[700];
    ks_storage_offline_key(map_name, key, sizeof(key));
    snprintf(path, sizeof(path), "%s/krunknative/%s.json", g_dir, key);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *text = calloc(1, 65536);
    if (text) fread(text, 1, 65535, f);
    fclose(f);
    return text;
}

// ------------------------------------------------------------------ 2. offline store

static void test_offline(void)
{
    cJSON *map = storage_map();
    const char *map_name = cJSON_GetStringValue(cJSON_GetObjectItem(map, "name"));
    CHECK(start(map), "sandbox_start");
    CHECK(ks_game_script(&g_server->game) != NULL, "script loaded");
    Client *a = connect_client();
    pump(40);
    CHECK(a->uid >= 0, "spawned");
    Player *pa = game_find_player(&g_server->game, a->uid);
    // Save Data (increase "trig" by 7) then Load Data on spawn: the custVals get the stored value
    CHECK(pa && player_custom_value_get(pa, "trig") == 7.0, "Save Data + Load Data: custVals.trig = %g", pa ? player_custom_value_get(pa, "trig") : -1.0);

    msg(a, "acct");
    pump(2);
    CHECK(has_log(a, "acct local 0"), "the sandbox player has an account: local / 0");

    // two sets in one entry: the second finds the lock; the first completes later
    msg(a, "set2");
    pump(1);
    CHECK(has_log(a, "storage.set already in progress for local"), "in-flight lock");
    pump(2);
    CHECK(has_log(a, "saved true local 5 abc"), "set completes with the stored values");
    // the 10 s limit: one set per window
    msg(a, "set");
    pump(2);
    CHECK(has_log(a, "Calling storage.set too much"), "10 s limit");
    g_ms += 10000.0;
    msg(a, "set");
    pump(3);
    CHECK(count_log(a, "saved true local 5 abc") == 2, "the next window saves again");
    // update increments
    msg(a, "update");
    pump(3);
    CHECK(has_log(a, "updated true local 7"), "update adds: 5 + 2");
    msg(a, "update");
    pump(3);
    CHECK(has_log(a, "Calling storage.update too much"), "update has its own 10 s limit");
    // load: the script's keys and the trigger's key (no V_ prefix) both read back
    msg(a, "load");
    pump(3);
    CHECK(has_log(a, "loaded true local 7 abc 7"), "load");
    msg(a, "load");
    pump(3);
    CHECK(count_log(a, "loaded true local 7 abc 7") == 2, "load has no 10 s limit");
    msg(a, "loadnone");
    pump(3);
    CHECK(has_log(a, "No data"), "an account without rows: No data (no callback)");
    msg(a, "loadother");
    pump(3);
    CHECK(count_log(a, "No data") == 2, "another map's private rows are not readable");

    // validation messages, each with the callback ({}, false, name) right away
    msg(a, "bad");
    pump(1);
    const char *expect[] = {
        "Key too long", "failed false k1", "Player not connected", "failed false k2", "Value too long", "failed false k3",
        "Too many keys: max 30", "failed false k5", "Missing storage data", "failed false k6", "Transactions disabled for now",
    };
    for (size_t i = 0; i < sizeof(expect) / sizeof(expect[0]); i++) CHECK(has_log(a, expect[i]), "bad: %s", expect[i]);
    CHECK(count_log(a, "Player not connected") == 2, "update without a callback still validates");
    for (int i = 0; i < a->log_count; i++) // in order: the error first, then the callback
        if (!strcmp(a->logs[i], "Key too long")) CHECK(i + 1 < a->log_count && !strcmp(a->logs[i + 1], "failed false k1"), "error, then callback");

    // the store is written within a second
    pump(70);
    char *text = read_store(map_name);
    CHECK(text && strstr(text, "\"V_coins\"") && strstr(text, "\"trig\"") && strstr(text, "\"local\""), "offline store file: %s", text ? text : "(none)");
    free(text);

    // round trip: an update right before the stop (not flushed yet) is written by the stop;
    // start again: the data is there
    g_ms += 10000.0;
    msg(a, "update");
    pump(2);
    CHECK(count_log(a, "updated true local 9") == 1, "update before the stop");
    sandbox_stop();
    free_clients();
    text = read_store(map_name);
    CHECK(text && strstr(text, "\"V_coins\":{\"v\":\"9\""), "the stop flushes the store: %s", text ? text : "(none)");
    free(text);
    CHECK(start(map), "sandbox_start again");
    a = connect_client();
    pump(40);
    pa = game_find_player(&g_server->game, a->uid);
    CHECK(pa && player_custom_value_get(pa, "trig") == 14.0, "Save Data increased the stored 7: %g", pa ? player_custom_value_get(pa, "trig") : -1.0);
    CHECK(pa && player_custom_value_get(pa, "V_coins") == 9.0, "Load Data sees the script's keys too (V_coins)");
    msg(a, "load");
    pump(3);
    CHECK(has_log(a, "loaded true local 9 abc 14"), "round trip: the stored values come back");
    if (!has_log(a, "loaded true local 9 abc 14")) dump_logs(a);
    sandbox_stop();
    free_clients();
    cJSON_Delete(map);
    printf("offline store done\n");
}

// ------------------------------------------------------------------ 3. installed offline I/O

static char g_mem_key[64], *g_mem_text;
static int g_mem_writes;

static int mem_read(const char *key, char *buf, const size_t n)
{
    if (!g_mem_text || strcmp(key, g_mem_key)) return -1;
    if (buf && n) snprintf(buf, n, "%s", g_mem_text);
    return (int) strlen(g_mem_text);
}

static void mem_write(const char *key, const char *text)
{
    g_mem_writes++;
    snprintf(g_mem_key, sizeof(g_mem_key), "%s", key);
    free(g_mem_text);
    g_mem_text = text ? strdup(text) : NULL;
}

static void test_offline_io(void)
{
    ks_storage_set_offline_io(mem_read, mem_write);
    cJSON *map = storage_map();
    cJSON_ReplaceItemInObject(map, "name", cJSON_CreateString("Storage IO Map"));
    CHECK(start(map), "sandbox_start");
    Client *a = connect_client();
    pump(40);
    msg(a, "set");
    pump(70);
    char key[64];
    ks_storage_offline_key("Storage IO Map", key, sizeof(key));
    CHECK(g_mem_writes > 0 && !strcmp(g_mem_key, key) && g_mem_text && strstr(g_mem_text, "\"V_gems\":{\"v\":\"abc\""),
          "the document goes through the installed I/O: %s", g_mem_text ? g_mem_text : "(none)");
    CHECK(!read_store("Storage IO Map"), "nothing written to files");
    sandbox_stop();
    free_clients();
    // and is read back through it
    CHECK(start(map), "sandbox_start again");
    a = connect_client();
    pump(40);
    msg(a, "load");
    pump(3);
    CHECK(has_log(a, "loaded true local 5 abc 14"), "loaded through the installed I/O (trig saved by both spawns)");
    sandbox_stop();
    free_clients();
    cJSON_Delete(map);
    ks_storage_set_offline_io(NULL, NULL);
    free(g_mem_text);
    g_mem_text = NULL;
    printf("offline I/O done\n");
}

// ------------------------------------------------------------------ 4. kn_brain frames

static cJSON *g_frames[64];
static int g_frame_count;

static void sink(cJSON *frame)
{
    if (g_frame_count < 64) g_frames[g_frame_count++] = frame;
    else cJSON_Delete(frame);
}

static const cJSON *frame_op(const char *op, const int nth)
{
    int seen = 0;
    for (int i = 0; i < g_frame_count; i++) {
        const char *o = cJSON_GetStringValue(cJSON_GetObjectItem(g_frames[i], "op"));
        if (o && !strcmp(o, op) && seen++ == nth) return g_frames[i];
    }
    return NULL;
}

static void reply(const cJSON *frame, const char *err, const char *data_json)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "type", "acct-r");
    cJSON_AddNumberToObject(r, "rid", cJSON_GetObjectItem(frame, "rid")->valuedouble);
    if (err) cJSON_AddStringToObject(r, "err", err);
    else cJSON_AddNullToObject(r, "err");
    cJSON_AddItemToObject(r, "data", data_json ? cJSON_Parse(data_json) : cJSON_CreateNull());
    CHECK(ks_storage_on_brain_frame(r) == 1, "reply consumed");
    cJSON_Delete(r);
}

static void test_brain(void)
{
    ks_storage_set_brain_sink(sink);
    cJSON *map = storage_map();
    const char *map_name = cJSON_GetStringValue(cJSON_GetObjectItem(map, "name"));
    CHECK(start(map), "sandbox_start");
    Client *a = connect_client();
    pump(40);
    ClientConnection *conn = conn_of(a);
    CHECK(conn != NULL, "connection");
    if (conn) conn->acct.account_id = 42;

    // spawn triggers: Save Data -> "storage" type 3 [["trig","7"]], Load Data -> "storage_load"
    const cJSON *save = frame_op("storage", 0), *load = frame_op("storage_load", 0);
    const cJSON *args = save ? cJSON_GetObjectItem(save, "args") : NULL;
    char *text = args ? cJSON_PrintUnformatted(args) : NULL;
    char want[256];
    snprintf(want, sizeof(want), "{\"id\":0,\"name\":\"local\",\"map\":\"%s\",\"type\":3,\"scope\":0,\"data\":[[\"trig\",\"7\"]]}", map_name);
    CHECK(text && !strcmp(text, want), "Save Data frame: %s", text ? text : "(none)");
    free(text);
    CHECK(save && !strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(save, "type")), "acct"), "an acct frame");
    if (load) reply(load, NULL, "{\"rows\":[[\"trig\",\"21\"],[\"V_coins\",\"9\"],[\"word\",\"abc\"]]}");
    pump(3); // a server tick (30 Hz) runs at least once
    Player *pa = game_find_player(&g_server->game, a->uid);
    CHECK(pa && player_custom_value_get(pa, "trig") == 21.0 && player_custom_value_get(pa, "V_coins") == 9.0, "Load Data reply -> custVals: %g %g (load frame %d)",
          pa ? player_custom_value_get(pa, "trig") : -1.0, pa ? player_custom_value_get(pa, "V_coins") : -1.0, load != NULL);
    if (save) reply(save, NULL, "{\"rows\":[[\"trig\",\"21\"]]}");

    // STORAGE.set -> frame with the account id; the reply's rows reach the callback
    msg(a, "set");
    pump(3); // a server tick (30 Hz) runs at least once
    const cJSON *set = frame_op("storage", 1);
    args = set ? cJSON_GetObjectItem(set, "args") : NULL;
    text = args ? cJSON_PrintUnformatted(cJSON_GetObjectItem(args, "data")) : NULL;
    CHECK(text && !strcmp(text, "[[\"V_coins\",\"5\"],[\"V_gems\",\"abc\"]]") && cJSON_GetObjectItem(args, "id")->valueint == 42 &&
          cJSON_GetObjectItem(args, "type")->valueint == 0, "set frame: %s", text ? text : "(none)");
    free(text);
    pump(5);
    CHECK(!has_log(a, "saved true local 9 x"), "nothing before the reply");
    if (set) reply(set, NULL, "{\"rows\":[[\"V_coins\",\"9\"],[\"V_gems\",\"x\"]]}");
    pump(3); // a server tick (30 Hz) runs at least once
    CHECK(has_log(a, "saved true local 9 x"), "the brain's rows reach the callback");
    // an error reply: logged, callback ({}, false)
    g_ms += 10000.0;
    msg(a, "set");
    pump(3); // a server tick (30 Hz) runs at least once
    set = frame_op("storage", 2);
    if (set) reply(set, "Storage only supported on published maps", NULL);
    pump(3); // a server tick (30 Hz) runs at least once
    CHECK(has_log(a, "Storage only supported on published maps") && has_log(a, "saved false local undefined undefined"), "error reply");
    // load: rows -> callback; an error -> "No data found"; no rows -> "No data"
    msg(a, "load");
    pump(3); // a server tick (30 Hz) runs at least once
    const cJSON *l = frame_op("storage_load", 1);
    if (l) reply(l, NULL, "{\"rows\":[[\"V_gems\",\"g\"],[\"trig\",\"3\"],[\"V_coins\",\"1\"]]}");
    pump(3); // a server tick (30 Hz) runs at least once
    CHECK(has_log(a, "loaded true local 1 g 3"), "load reply");
    msg(a, "load");
    pump(3); // a server tick (30 Hz) runs at least once
    l = frame_op("storage_load", 2);
    if (l) reply(l, "Server error.", NULL);
    pump(3); // a server tick (30 Hz) runs at least once
    CHECK(has_log(a, "No data found"), "a failed load: No data found");
    msg(a, "loadother");
    pump(3); // a server tick (30 Hz) runs at least once
    l = frame_op("storage_load", 3);
    CHECK(l && !strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(l, "args"), "from")), "Some Other Map"), "load from another map");
    if (l) reply(l, NULL, "{\"rows\":[]}");
    pump(3); // a server tick (30 Hz) runs at least once
    CHECK(has_log(a, "No data"), "no rows: No data");
    // the link goes down: pending requests fail
    msg(a, "load");
    pump(3); // a server tick (30 Hz) runs at least once
    ks_storage_link_down();
    pump(3); // a server tick (30 Hz) runs at least once
    CHECK(count_log(a, "No data found") == 2, "link down: the pending load fails");
    // a reply nobody waits for is not ours
    cJSON *stray = cJSON_Parse("{\"type\":\"acct-r\",\"rid\":5,\"err\":null,\"data\":{}}");
    CHECK(ks_storage_on_brain_frame(stray) == 0, "stray reply ignored");
    cJSON_Delete(stray);

    // PAYMENTS.giveKR: one reward in flight per player (the second call waits), then the chat line
    const int chats = a->chats, popups = a->popups;
    msg(a, "kr");
    pump(3); // a server tick (30 Hz) runs at least once
    const cJSON *kr = frame_op("map_reward", 0);
    args = kr ? cJSON_GetObjectItem(kr, "args") : NULL;
    CHECK(args && cJSON_GetObjectItem(args, "id")->valueint == 42 && cJSON_GetObjectItem(args, "amount")->valueint == 5 &&
          !strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(args, "map")), map_name), "map_reward frame");
    CHECK(!frame_op("map_reward", 1), "player.waitRew: one at a time");
    if (kr) reply(kr, NULL, "{\"rsp\":\"Success\",\"funds\":105}");
    pump(2);
    CHECK(a->chats == chats + 1 && !strcmp(a->last_chat, "Rewarded 5 KR") && a->popups == popups + 1, "reward chat + popup: %s", a->last_chat);
    CHECK(conn_of(a) && conn_of(a)->acct.funds == 105, "funds updated");
    // Reward KR trigger (32) through GameHooks.reward_kr; a refusal sends nothing
    if (g_server->game.hooks.reward_kr && pa) g_server->game.hooks.reward_kr(&g_server->game, pa, -3); // |amount|
    kr = frame_op("map_reward", 1);
    CHECK(kr && cJSON_GetObjectItem(cJSON_GetObjectItem(kr, "args"), "amount")->valueint == 3, "Reward KR trigger frame");
    if (kr) reply(kr, NULL, "{\"rsp\":\"Not enough map funds\"}");
    pump(2);
    CHECK(a->chats == chats + 1, "refused reward: no chat line");
    // the rate limit p_givKR: 4 per second (3 pass)
    g_ms += 1000.0;
    for (int i = 0; i < 3; i++) {
        msg(a, "kr");
        pump(3); // a server tick (30 Hz) runs at least once
        const cJSON *k = frame_op("map_reward", 2 + i);
        if (k) reply(k, NULL, "{\"rsp\":\"Success\",\"funds\":1}");
        pump(3); // a server tick (30 Hz) runs at least once
    }
    int rewards = 0;
    while (frame_op("map_reward", rewards)) rewards++;
    CHECK(rewards <= 2 + 3, "giveKR rate: %d frames", rewards);

    sandbox_stop();
    free_clients();
    cJSON_Delete(map);
    for (int i = 0; i < g_frame_count; i++) cJSON_Delete(g_frames[i]);
    g_frame_count = 0;
    ks_storage_set_brain_sink(NULL);
    printf("brain frames done\n");
}

// ------------------------------------------------------------------ 5. ADMIN.ban

static void test_ban(void)
{
    cJSON *map = storage_map();
    CHECK(start(map), "sandbox_start");
    Client *a = connect_client();
    pump(40);
    msg(a, "ban");
    pump(5);
    CHECK(a->errors > 0 && a->closed, "banned by the script");
    Client *b = connect_client();
    pump(10);
    CHECK(b->closed && b->uid < 0, "the banned address is refused");
    sandbox_stop();
    free_clients();
    cJSON_Delete(map);
    printf("ban done\n");
}

// A private config dir for the offline store ($TMPDIR, else /tmp), removed at the end.
static int make_config_dir(void)
{
#ifdef _WIN32
    const char *tmp = getenv("TEMP");
    snprintf(g_dir, sizeof(g_dir), "%s/kn-ks-storage-%d", tmp && *tmp ? tmp : ".", _getpid());
    _mkdir(g_dir);
    return _putenv_s("XDG_CONFIG_HOME", g_dir) == 0;
#else
    const char *tmp = getenv("TMPDIR");
    snprintf(g_dir, sizeof(g_dir), "%s/kn-ks-storage-XXXXXX", tmp && *tmp ? tmp : "/tmp");
    return mkdtemp(g_dir) && setenv("XDG_CONFIG_HOME", g_dir, 1) == 0;
#endif
}

static void remove_config_dir(void)
{
#ifndef _WIN32
    char cmd[800];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_dir);
    if (strstr(g_dir, "/kn-ks-storage-")) (void) !system(cmd);
#endif
}

int main(void)
{
    if (!make_config_dir()) {
        printf("no temporary config dir\n");
        return 1;
    }

    KsDiag err;
    KsProgram *prog = ks_compile("num x = 0;", 10, KS_SIDE_CLIENT, &err, NULL, 0, NULL);
    static const KsHost host = {KS_SIDE_CLIENT, NULL, NULL, NULL};
    g_cs = ks_script_new(prog, &host, NULL);
    test_data_rules();
    test_offline();
    test_offline_io();
    test_brain();
    test_ban();
    ks_script_free(g_cs);
    ks_program_free(prog);
    remove_config_dir();
    if (g_failures) {
        printf("%d of %d checks failed\n", g_failures, g_checks);
        return 1;
    }
    printf("Test OK (%d checks)\n", g_checks);
    return 0;
}
