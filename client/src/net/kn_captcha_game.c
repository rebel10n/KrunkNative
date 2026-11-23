// The in-game captcha of Box Office servers on the client (client/include/kn_captcha_game.h, doc/v10/captcha.md).
//
// v10.0.7's "cpt" (deob-10.0.7.js:98801-98806): showCaptcha(challenge, cb, false), the Security Challenge card
// (altcha_modal.c) that the player answers ("I'm not a robot"); its value goes back as "cptR" and the card stays until
// the server's "cptR" (:98809-98810) or "error" (:98823), or the socket's reset. The bundle's failure branch
// (`err ? (_hideCaptcha, "CAPTCHA FAILED", close)`) is never taken in v10.0.7, whose widget calls back with a value
// only: a challenge that cannot be verified leaves the widget's own error, and the server ends the session when no
// answer comes. Without a page to show the card (no HTML engine, another page) the client answers it itself.
#include <kn_captcha_game.h>
#include <kn_captcha.h>
#include <altcha_modal.h>
#include <net_ext.h>
#include <shared.h>
#include "klog.h"
#include <pthread.h>
#include <stdint.h>
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

// The pageless answer that cannot be had: v10.0.3's failure branch (_hideCaptcha, "CAPTCHA FAILED", close,
// deob-10.0.3.js:111428-111431), so the session does not wait for the server's timeout with nothing on screen
static void fail(Client *client) {
    printf("[captcha] game: the server's security challenge cannot be answered: closing (" KN_CAPTCHA_GAME_FAILED ")\n");
    client_net_disconnect(client, KN_CAPTCHA_GAME_FAILED, NET_ERROR_CODE_CAPTCHA);
}

// "cptR" [value]
static int send_answer(Client *client, const char *payload) {
    const size_t len = payload ? strlen(payload) + 1 : 0;
    if (!payload || len > NET_EXT_BODY_MAX) return 0;
    uint8_t buf[NET_EXT_HEADER_SIZE + NET_EXT_BODY_MAX];
    const uint16_t n = net_ext_pack(buf, sizeof(buf), NET_EXT_CAPTCHA_ANSWER, payload, (uint16_t) len);
    if (n) client_net_send(client, (NetPacketType) NET_PACKET_EXT, buf, n);
    KN_LOG_DEBUG1("[captcha] game: the server's challenge answered\n");
    return 1;
}

static int session_alive(Client *client, const unsigned seq) {
    return client && client->net_connected && session_seq(client) == seq;
}

// the card's: the session it was asked on is still there (else it goes, as the socket's reset removes it)
static int card_alive(void *user) {
    (void) user;
    return session_alive(G.client, G.seq);
}

// the card's callback: cb(value, null) -> send(cptR, value) (:98805)
static void on_card(void *user, const char *payload) {
    (void) user;
    if (session_alive(G.client, G.seq)) send_answer(G.client, payload);
}

// pageless: solved here
static void on_solved(void *user, const char *payload) {
    (void) user;
    Client *client = G.client;
    G.id = 0;
    if (!session_alive(client, G.seq)) return; // asked on a session that is gone
    if (!send_answer(client, payload)) fail(client);
}

void kn_captcha_game_packet(Client *client, const uint16_t subtype, const uint8_t *body, const uint16_t length) {
    if (!client) return;
    if (subtype == NET_EXT_CAPTCHA_ANSWER) { // the server's "cptR": accepted, _hideCaptcha
        KN_LOG_DEBUG1("[captcha] game: the server accepted the answer\n");
        altcha_modal_hide();
        return;
    }
    if (subtype != NET_EXT_CAPTCHA) return;
    char json[NET_EXT_BODY_MAX + 1];
    const size_t n = length < NET_EXT_BODY_MAX ? length : NET_EXT_BODY_MAX;
    memcpy(json, body, n);
    json[n] = 0;
    kn_captcha_cancel(G.id); // a new challenge replaces one still being solved
    G.id = 0;
    G.client = client;
    G.seq = session_seq(client);
    if (altcha_modal_show(json, 0, on_card, card_alive, NULL)) {
        KN_LOG_DEBUG1("[captcha] game: the server asks a security challenge (the Security Challenge card)\n");
        return;
    }
    G.id = kn_captcha_solve("game", json, on_solved, NULL);
    if (!G.id) fail(client); // not a challenge this client solves (not SHA-256 / -384 / -512 ALTCHA, over the cap)
    else KN_LOG_DEBUG1("[captcha] game: the server asks a security challenge (no page: solving it, nothing shown)\n");
}
