// The in-game captcha of Box Office servers on the client (client/include/kn_captcha_game.h, doc/v10/captcha.md).
#include <kn_captcha_game.h>
#include <kn_captcha.h>
#include <net_ext.h>
#include <shared.h>
#include "klog.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>

static struct {
    Client *client;
    unsigned seq; // the session the challenge came on (Client.net_session_seq)
    unsigned id;  // its kn_captcha (0 none)
} G;

static unsigned session_seq(Client *client) {
    pthread_mutex_lock(&client->net_lock);
    const unsigned seq = client->net_session_seq;
    pthread_mutex_unlock(&client->net_lock);
    return seq;
}

// v10.0.3's failure branch: _hideCaptcha, the "error" event with "CAPTCHA FAILED", close (deob-10.0.3.js:111428-111431)
static void fail(Client *client) {
    printf("[captcha] game: the server's security challenge cannot be answered: closing (" KN_CAPTCHA_GAME_FAILED ")\n");
    client_net_disconnect(client, KN_CAPTCHA_GAME_FAILED, NET_ERROR_CODE_CAPTCHA);
}

static void on_solved(void *user, const char *payload) {
    (void) user;
    Client *client = G.client;
    G.id = 0;
    if (!client || !client->net_connected || session_seq(client) != G.seq) return; // asked on a session that is gone
    const size_t len = payload ? strlen(payload) + 1 : 0;
    if (!payload || len > NET_EXT_BODY_MAX) {
        fail(client);
        return;
    }
    uint8_t buf[NET_EXT_HEADER_SIZE + NET_EXT_BODY_MAX];
    const uint16_t n = net_ext_pack(buf, sizeof(buf), NET_EXT_CAPTCHA_ANSWER, payload, (uint16_t) len);
    if (n) client_net_send(client, (NetPacketType) NET_PACKET_EXT, buf, n); // "cptR" [payload]
    KN_LOG_DEBUG1("[captcha] game: the server's challenge answered\n");
}

void kn_captcha_game_packet(Client *client, const uint16_t subtype, const uint8_t *body, const uint16_t length) {
    if (!client) return;
    if (subtype == NET_EXT_CAPTCHA_ANSWER) { // the server's "cptR": accepted
        KN_LOG_DEBUG1("[captcha] game: the server accepted the answer\n");
        return;
    }
    if (subtype != NET_EXT_CAPTCHA) return;
    char json[NET_EXT_BODY_MAX + 1];
    const size_t n = length < NET_EXT_BODY_MAX ? length : NET_EXT_BODY_MAX;
    memcpy(json, body, n);
    json[n] = 0;
    kn_captcha_cancel(G.id); // a new challenge replaces one still being solved
    G.client = client;
    G.seq = session_seq(client);
    G.id = kn_captcha_solve("game", json, on_solved, NULL);
    if (!G.id) fail(client); // not a challenge this client solves (not SHA-256 ALTCHA, over the cap)
    else KN_LOG_DEBUG1("[captcha] game: the server asks a security challenge (solving it, nothing shown)\n");
}
