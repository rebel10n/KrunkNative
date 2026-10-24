// server/src/game_captcha.c - the in-game captcha of Box Office servers: v10.0.3's "cpt" / "cptR", issued and verified
// by the brain (server/include/server_captcha.h, doc/v10/captcha.md, services/brain/captcha.c captcha_node_op).
#include <server_captcha.h>
#include <server_accounts.h> // net_kick_client
#include <server_log.h>
#include <brain.h>
#include <net_ext.h>
#include <shared.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GAME_CAPTCHA_MAX 512 // connections followed at once (a lobby has far fewer)

typedef enum {
    GC_ASKING = 1, // the brain's challenge is on its way (rid)
    GC_SENT,       // NET_EXT_CAPTCHA sent: waiting for the answer
    GC_CHECKING,   // the answer is with the brain (rid)
    GC_PASSED,     // accepted: never asked again on this connection
} GameCaptchaState;

typedef struct {
    int client_id;
    unsigned char state; // GameCaptchaState, 0 = free
    uint32_t rid;
    float deadline;
    char challenge[65]; // the challenge hex handed to this connection: its answer is bound to it
} GameCaptcha;

static GameCaptcha g_cap[GAME_CAPTCHA_MAX];
static int g_enforce;

static const char *str_of(const cJSON *frame, const char *key) {
    const char *s = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(frame, key));
    return s ? s : "?";
}

int game_captcha_enforced(void) { return g_enforce && brain_linked(); }

void game_captcha_set_enforce(const cJSON *welcome) {
    const cJSON *c = cJSON_GetObjectItemCaseSensitive(welcome, "captcha");
    const int on = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(c, "enforce"));
    if (on != g_enforce) server_log("[captcha] the brain %s the in-game captcha", on ? "enforces" : "does not enforce");
    g_enforce = on;
}

void game_captcha_link_down(void) {
    // the brain's answers will not come: those asks start again once a link is back (and still enforced)
    for (int i = 0; i < GAME_CAPTCHA_MAX; i++) {
        if (g_cap[i].state && g_cap[i].state != GC_PASSED) memset(&g_cap[i], 0, sizeof(g_cap[i]));
    }
}

static GameCaptcha *entry_of(const int client_id) {
    for (int i = 0; i < GAME_CAPTCHA_MAX; i++) {
        if (g_cap[i].state && g_cap[i].client_id == client_id) return &g_cap[i];
    }
    return NULL;
}

static GameCaptcha *entry_by_rid(const uint32_t rid) {
    for (int i = 0; i < GAME_CAPTCHA_MAX; i++) {
        if ((g_cap[i].state == GC_ASKING || g_cap[i].state == GC_CHECKING) && g_cap[i].rid == rid) return &g_cap[i];
    }
    return NULL;
}

static ClientConnection *client_of(const int id) {
    for (size_t i = 0; i < g_server->client_count; i++) {
        if (g_server->clients[i].id == id) return &g_server->clients[i];
    }
    return NULL;
}

static void fail(GameCaptcha *e, const char *why) {
    server_log("[captcha] client %d: %s: closing (" GAME_CAPTCHA_FAILED ")", e->client_id, why);
    net_kick_client(e->client_id, GAME_CAPTCHA_FAILED, GAME_CAPTCHA_CODE); // v10.0.3's text (deob-10.0.3.js:111430)
    memset(e, 0, sizeof(*e));
}

static void send_ext(const ClientConnection *client, const uint16_t subtype, const char *text) {
    uint8_t buf[NET_EXT_HEADER_SIZE + NET_EXT_BODY_MAX];
    const size_t len = text ? strlen(text) + 1 : 0;
    if (len > NET_EXT_BODY_MAX) return;
    const uint16_t n = net_ext_pack(buf, sizeof(buf), subtype, text, (uint16_t) len);
    if (n) net_send_packet(client->fd, (NetPacketType) NET_PACKET_EXT, buf, n);
}

static void post(const char *op, GameCaptcha *e, const char *payload) {
    cJSON *f = cJSON_CreateObject();
    cJSON_AddStringToObject(f, "type", "captcha");
    cJSON_AddStringToObject(f, "op", op);
    e->rid = brain_next_rid();
    cJSON_AddNumberToObject(f, "rid", e->rid);
    if (payload) {
        cJSON_AddStringToObject(f, "payload", payload);
        cJSON_AddStringToObject(f, "challenge", e->challenge);
    }
    brain_post(f, 0); // takes the frame
}

void game_captcha_on_brain_frame(const cJSON *frame) {
    const cJSON *rid = cJSON_GetObjectItemCaseSensitive(frame, "rid");
    GameCaptcha *e = cJSON_IsNumber(rid) ? entry_by_rid((uint32_t) rid->valuedouble) : NULL;
    if (!e) return; // the connection left meanwhile
    ClientConnection *client = client_of(e->client_id);
    if (!client) {
        memset(e, 0, sizeof(*e));
        return;
    }
    if (e->state == GC_ASKING) {
        const cJSON *c = cJSON_GetObjectItemCaseSensitive(frame, "challenge");
        const char *hex = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(c, "challenge"));
        char *json = cJSON_IsObject(c) ? cJSON_PrintUnformatted(c) : NULL;
        if (!json || !hex || strlen(hex) != 64 || strlen(json) + 1 > NET_EXT_BODY_MAX) {
            // no challenge from the brain (no key): nothing can be asked, so nothing is refused
            server_log("[captcha] client %d: the brain gave no challenge (%s): not asked", e->client_id, str_of(frame, "error"));
            e->state = GC_PASSED;
            cJSON_free(json);
            return;
        }
        snprintf(e->challenge, sizeof(e->challenge), "%s", hex);
        send_ext(client, NET_EXT_CAPTCHA, json); // "cpt" [challenge]
        e->state = GC_SENT;
        cJSON_free(json);
        return;
    }
    // GC_CHECKING: the brain's verdict on the answer
    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(frame, "ok"))) {
        send_ext(client, NET_EXT_CAPTCHA_ANSWER, NULL); // the server's "cptR": accepted
        e->state = GC_PASSED;
        server_log("[captcha] client %d: accepted", e->client_id);
        return;
    }
    char why[64];
    snprintf(why, sizeof(why), "answer refused (%s)", str_of(frame, "result"));
    fail(e, why);
}

void game_captcha_on_answer(ClientConnection *client, const uint8_t *body, const uint16_t length) {
    GameCaptcha *e = client ? entry_of(client->id) : NULL;
    if (!e || e->state != GC_SENT) return; // not asked (or already answered): ignored
    char payload[NET_EXT_BODY_MAX + 1];
    const size_t n = length < NET_EXT_BODY_MAX ? length : NET_EXT_BODY_MAX;
    memcpy(payload, body, n);
    payload[n] = 0;
    if (!payload[0]) {
        fail(e, "no answer"); // the client could not solve it (v10.0.3 closes with CAPTCHA FAILED itself)
        return;
    }
    if (!brain_linked()) { // nothing can check it: let it through, as without a link nothing is asked
        e->state = GC_PASSED;
        return;
    }
    e->state = GC_CHECKING;
    post("verify", e, payload);
}

void game_captcha_tick(const float now) {
    // connections that left
    for (int i = 0; i < GAME_CAPTCHA_MAX; i++) {
        if (g_cap[i].state && !client_of(g_cap[i].client_id)) memset(&g_cap[i], 0, sizeof(g_cap[i]));
    }
    if (!game_captcha_enforced()) return;
    for (size_t i = 0; i < g_server->client_count; i++) {
        ClientConnection *client = &g_server->clients[i];
        if (client->fd < 0 || client->acct.join_state != SERVER_JOIN_READY) continue;
        GameCaptcha *e = entry_of(client->id);
        if (e) {
            if (e->state != GC_PASSED && now > e->deadline) fail(e, "no answer in time");
            continue;
        }
        GameCaptcha *slot = NULL;
        for (int k = 0; k < GAME_CAPTCHA_MAX && !slot; k++) {
            if (!g_cap[k].state) slot = &g_cap[k];
        }
        if (!slot) break; // full: the rest are asked once there is room
        slot->client_id = client->id;
        slot->deadline = now + GAME_CAPTCHA_ANSWER_S;
        if (client->peer_minor < NET_PROTOCOL_MINOR_EXT) { // a peer that cannot be asked cannot pass
            slot->state = GC_SENT;
            fail(slot, "the client has no EXT packets");
            continue;
        }
        slot->state = GC_ASKING;
        post("issue", slot, NULL);
    }
}
