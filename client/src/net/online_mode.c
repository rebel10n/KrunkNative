// Online / offline mode and the online-mode join handshake (client/include/online_mode.h).
#include <client.h>
#include <shared.h>
#include <online_mode.h>
#include <auth_join.h>
#include <net_ext.h> // NET_EXT_KN_ACCOUNT_AUTH / _RIGHTS: live sign-in
#include <i18n.h>
#include <ui_logic.h> // ui_notify, object_manager_ui_rights_changed
#include <backend.h>
#include <backend_ui.h>
#include <html_dom.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

static int g_online = 1;
static char g_finder[256] = ONLINE_DEFAULT_FINDER_URL;
static Client *g_client;

// the pending join of the current connection
static struct {
    char server_nonce[sizeof(((NetAuthChallengePacket *) 0)->server_nonce)];
    char client_nonce[AUTH_JOIN_NONCE_LEN + 1];
    char server_hash[AUTH_SERVER_HASH_LEN + 1];
    int in_flight;
    unsigned generation;
    NetAuthChallengePacket deferred; // challenge seen before the saved session was restored
    int has_deferred;
    int no_defer;                    // replaying a deferred challenge
    double deferred_at;
} g_join;

#define DEFER_SECONDS 3.5 // < the server's join_timeout (5 s): the restore usually takes < 1 s

static int is_off(const char *v) {
    return !v || !*v || !strcmp(v, "0") || !strcmp(v, "off") || !strcmp(v, "none") || !strcmp(v, "false");
}

#ifdef __EMSCRIPTEN__
static char *web_param(const char *name) {
    return (char *) EM_ASM_PTR({
        try {
            var v = new URLSearchParams(location.search).get(UTF8ToString($0));
            return v === null ? 0 : stringToNewUTF8(v);
        } catch (e) { return 0; }
    }, name);
}
#endif

void online_mode_init(const char *cli_mode, const char *cli_finder) {
    char stored[256] = "";
    const char *mode = cli_mode && *cli_mode ? cli_mode : getenv("KRUNKNATIVE_MODE");
    char *web_mode = NULL, *web_finder = NULL;
#ifdef __EMSCRIPTEN__
    web_mode = web_param("mode");
    web_finder = web_param("finder");
    if ((!mode || !*mode) && web_mode) mode = web_mode;
#endif
    if ((!mode || !*mode) && html_storage_get("knMode", stored, sizeof(stored)) > 0) mode = stored;
    snprintf(g_finder, sizeof(g_finder), "%s", ONLINE_DEFAULT_FINDER_URL); // a re-init starts from the default
    g_online = !(mode && !strcmp(mode, "offline"));

    const char *finder = cli_finder ? cli_finder : getenv("KRUNKNATIVE_FINDER_URL");
    char stored_finder[256] = "";
    if (!finder && web_finder) finder = web_finder;
    if (!finder && html_storage_get("knFinderUrl", stored_finder, sizeof(stored_finder)) >= 0) finder = stored_finder;
    if (finder) snprintf(g_finder, sizeof(g_finder), "%s", is_off(finder) ? "" : finder);
    size_t n = strlen(g_finder);
    while (n && g_finder[n - 1] == '/') g_finder[--n] = 0;
    free(web_mode);
    free(web_finder);
    printf("[mode] %s%s%s\n", g_online ? "online" : "offline (guest, all skins, nothing saved)",
           g_finder[0] ? ", server finder " : "", g_finder);
}

int online_mode_online(void) { return g_online; }
const char *online_mode_name(void) { return g_online ? "online" : "offline"; }
const char *online_finder_url(void) { return g_finder; }
const char *online_default_account_url(void) { return g_online ? ONLINE_DEFAULT_ACCOUNT_URL : ""; }

void online_mode_on_connect(void) {
    g_join.generation++;
    g_join.in_flight = 0;
    g_join.server_hash[0] = 0;
}

// 32 hex chars from the OS (getrandom-free: /dev/urandom natively, crypto on the web)
static void random_nonce(char out[AUTH_JOIN_NONCE_LEN + 1]) {
    unsigned char raw[AUTH_JOIN_NONCE_LEN / 2];
    int ok = 0;
#ifdef __EMSCRIPTEN__
    EM_ASM({
        var a = new Uint8Array(16);
        crypto.getRandomValues(a);
        HEAPU8.set(a, $0);
    }, raw);
    ok = 1;
#else
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) {
        ok = fread(raw, 1, sizeof(raw), f) == sizeof(raw);
        fclose(f);
    }
#endif
    if (!ok) {
        srand((unsigned) time(NULL) ^ (unsigned) (uintptr_t) out);
        for (size_t i = 0; i < sizeof(raw); i++) raw[i] = (unsigned char) (rand() & 0xff);
    }
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(raw); i++) {
        out[i * 2] = hex[raw[i] >> 4];
        out[i * 2 + 1] = hex[raw[i] & 15];
    }
    out[AUTH_JOIN_NONCE_LEN] = 0;
}

// No account (signed out, offline mode): tell an online_mode server no hasJoined is coming, so it
// admits us as a guest at once instead of after its join_timeout (servers before guest-welcome
// online_mode refused guests either way).
static void send_guest_join(Client *client) {
    NetJoinPacket join;
    memset(&join, 0, sizeof(join));
    pthread_mutex_lock(&client->net_lock);
    snprintf(join.game_id, sizeof(join.game_id), "%s", client->net_join.game_id);
    pthread_mutex_unlock(&client->net_lock);
    snprintf(join.client_key, sizeof(join.client_key), "%s", AUTH_JOIN_GUEST_KEY);
    client_net_send(client, NET_PACKET_JOIN, &join, NET_JOIN_PACKET_V2_SIZE);
}

static void send_hj_join(Client *client) {
    NetJoinPacket join;
    memset(&join, 0, sizeof(join));
    pthread_mutex_lock(&client->net_lock);
    snprintf(join.game_id, sizeof(join.game_id), "%s", client->net_join.game_id);
    pthread_mutex_unlock(&client->net_lock);
    snprintf(join.client_key, sizeof(join.client_key), AUTH_JOIN_KEY_PREFIX "%s", g_join.client_nonce);
    const AccountInfo *acc = account_get();
    snprintf(join.name, sizeof(join.name), "%s", acc ? acc->name : "");
    client_net_send(client, NET_PACKET_JOIN, &join, sizeof(join));
    printf("[auth] joining as %s (serverHash %.12s...)\n", join.name, g_join.server_hash);
}

static void on_join_done(void *user, const int status, const char *body, const size_t length) {
    (void) length;
    const unsigned generation = (unsigned) (uintptr_t) user;
    if (generation != g_join.generation || !g_client) return; // a newer connection
    g_join.in_flight = 0;
    if (status != 200) {
        cJSON *res = cJSON_Parse(body);
        const char *err = cJSON_GetStringValue(cJSON_GetObjectItem(res, "error"));
        fprintf(stderr, "[auth] /auth/join failed (%d): %s\n", status, err ? err : status ? "error" : "account service unreachable");
        cJSON_Delete(res);
        // the hasJoined will fail and the server admits us as a guest (Krunker connects without
        // "at=" when its token does not refresh); the JOIN still goes out so that happens now
    }
    send_hj_join(g_client);
}

static void live_note_challenge(const NetAuthChallengePacket *ch);

void online_mode_on_challenge(void *client_ptr, const void *challenge_in) {
    Client *client = client_ptr;
    NetAuthChallengePacket ch;
    memcpy(&ch, challenge_in, sizeof(ch));
    ch.server_nonce[sizeof(ch.server_nonce) - 1] = 0;
    ch.public_key[sizeof(ch.public_key) - 1] = 0;
    ch.auth_url[sizeof(ch.auth_url) - 1] = 0;
    g_client = client;
    online_mode_on_connect(); // one challenge per connection: late replies of the previous one are dropped
    live_note_challenge(&ch); // a sign-in later in the session answers the same challenge
    if (!ch.online_mode) return; // offline_mode server: the guest JOIN already sent is all it needs
    const AccountInfo *acc = account_get();
    // a saved session (the alt vault's, or the legacy key): account_session_token() holds it at startup
    if (g_online && !acc && !g_join.no_defer && account_session_token()[0] && backend_enabled()) {
        // startup: the token restore on the social socket is still running; retry from the tick
        g_join.deferred = ch;
        g_join.has_deferred = 1;
        g_join.deferred_at = glfwGetTime();
        return;
    }
    g_join.has_deferred = 0;
    if (!g_online || !acc || !account_session_token()[0]) {
        printf("[auth] the server verifies accounts; not signed in: joining as a guest\n");
        send_guest_join(client);
        return;
    }
    if (!kn_auth_is_hex(ch.server_nonce, AUTH_JOIN_NONCE_LEN)) {
        fprintf(stderr, "[auth] bad server nonce in the challenge: joining as a guest\n");
        send_guest_join(client);
        return;
    }
    snprintf(g_join.server_nonce, sizeof(g_join.server_nonce), "%s", ch.server_nonce);
    random_nonce(g_join.client_nonce);
    kn_server_hash(ch.server_nonce, ch.public_key, g_join.client_nonce, g_join.server_hash);
    // The session token goes only to the account service that issued it (our backend URL);
    // a server naming another auth service cannot verify our account.
    if (ch.auth_url[0] && strcmp(ch.auth_url, backend_url()) != 0)
        fprintf(stderr, "[auth] the server verifies against %s, this client's account is at %s\n", ch.auth_url, backend_url());
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "token", account_session_token());
    cJSON_AddStringToObject(body, "serverHash", g_join.server_hash);
    char *text = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    char url[320];
    snprintf(url, sizeof(url), "%s/auth/join", backend_url());
    g_join.in_flight = 1;
    if (!text || !backend_http_request("POST", url, text, on_join_done, (void *) (uintptr_t) g_join.generation)) {
        g_join.in_flight = 0;
        fprintf(stderr, "[auth] could not reach %s\n", url);
    }
    free(text);
}

static void live_tick(Client *client);

void online_mode_tick(void *client) {
    if (g_join.has_deferred && (account_get() || glfwGetTime() - g_join.deferred_at > DEFER_SECONDS)) {
        NetAuthChallengePacket ch = g_join.deferred;
        g_join.has_deferred = 0;
        g_join.no_defer = 1; // no second deferral: signed in by now, or a guest
        online_mode_on_challenge(client, &ch);
        g_join.no_defer = 0;
        return;
    }
    live_tick(client);
    if (g_online) return;
    // offline: no account, so the Login / Register bar becomes the mode notice (once)
    static int done;
    if (done) return;
    HtmlElem bar = html_dom_by_id("signedOutHeaderBar");
    if (!bar) return;
    done = 1;
    html_dom_set_html(bar, "<div class=\"button buttonP lgn\" style=\"width:300px;margin-right:0px;padding-top:3px;padding-bottom:15px;\" "
                           "onmouseenter=\"playTick()\" onclick=\"showWindow(1)\" title=\"Settings > Client > Account Mode\">Offline Mode <span class=\"material-icons\" "
                           "style=\"vertical-align:bottom;color:#fff;font-size:30px;margin-bottom:-1px;\">cloud_off</span></div>");
}

// ------------------------------------------------------------------ live sign-in
// The server's NET_EXT_KN_ACCOUNT_RIGHTS says who we are on this connection and that it takes live
// sign-ins. Whenever the menu's identity (a session token, or a guest) differs from the one the
// server has, it goes out once as NET_EXT_KN_ACCOUNT_AUTH; an identity the server did not take
// (refused, not verifiable, no answer) is not sent again on this connection.

#define LIVE_GUEST "guest"
#define LIVE_ANSWER_WAIT 15.0 // s: the server answers within its own 10 s auth timeout
#define LIVE_BUSY_RETRY 5.0   // s after a BUSY answer (rate limit, a verification running)
#define LIVE_DOWN_RETRY 30.0  // s after UNAVAILABLE (the server's account service link is down)

static struct {
    int have;                // a RIGHTS of connection conn_id came: this server takes live sign-ins
    int conn_id;             // NetInitPacket.client_id of that connection
    unsigned rights, kinds;  // NET_RIGHTS_*, NET_ACCOUNT_LIVE_*
    int account_id;          // the server's account for us, 0 guest
    char synced[72];         // the identity the server has: LIVE_GUEST or the token it verified ("?" unknown)
    char attempt[72];        // the last identity it did not take: never again on this connection
    char pending_key[72];    // the identity in flight
    char notified[72];       // the identity whose failure was shown (one toast each)
    int pending;
    double pending_at, retry_at;
    uint8_t seq;
    unsigned generation;     // /auth/join answers of an older attempt or connection are dropped
    char server_nonce[sizeof(((NetAuthChallengePacket *) 0)->server_nonce)];
    char public_key[sizeof(((NetAuthChallengePacket *) 0)->public_key)];
    int nonce_ok;            // the connection's challenge named an online_mode server nonce
    char hj_nonce[AUTH_JOIN_NONCE_LEN + 1];
    char hj_name[32];
} g_live;

static void live_note_challenge(const NetAuthChallengePacket *ch) {
    g_live.nonce_ok = ch->online_mode && kn_auth_is_hex(ch->server_nonce, AUTH_JOIN_NONCE_LEN);
    snprintf(g_live.server_nonce, sizeof(g_live.server_nonce), "%s", ch->server_nonce);
    snprintf(g_live.public_key, sizeof(g_live.public_key), "%s", ch->public_key);
}

void online_mode_session_ended(void) {
    g_live.have = 0;
    g_live.pending = 0;
    g_live.rights = 0;
    g_live.account_id = 0;
    g_live.nonce_ok = 0;
    g_live.generation++;
}

int online_mode_live_ok(const void *client_ptr) {
    const Client *client = client_ptr;
    return client && g_live.have && client->net_connected && !client->offline && g_live.conn_id == client->net_client_id;
}

unsigned online_mode_server_rights(void) { return g_live.have ? g_live.rights : 0; }

// the identity the menu has now: the session token while signed in (online mode), else a guest
static const char *live_desired(void) {
    const char *token = account_session_token();
    return g_online && account_get() && token && token[0] ? token : LIVE_GUEST;
}

static void live_send_auth(Client *client, const int kind, const char *name, const char *key) {
    NetAccountAuthPacket req;
    memset(&req, 0, sizeof(req));
    req.kind = (uint8_t) kind;
    req.seq = g_live.seq;
    snprintf(req.name, sizeof(req.name), "%s", name ? name : "");
    snprintf(req.key, sizeof(req.key), "%s", key ? key : "");
    uint8_t buf[NET_EXT_HEADER_SIZE + sizeof(req)];
    const uint16_t n = net_ext_pack(buf, sizeof(buf), NET_EXT_KN_ACCOUNT_AUTH, &req, sizeof(req));
    if (n) client_net_send(client, (NetPacketType) NET_PACKET_EXT, buf, n);
}

static void on_live_join_done(void *user, const int status, const char *body, const size_t length) {
    (void) length;
    if ((unsigned) (uintptr_t) user != g_live.generation || !g_client || !g_live.pending) return; // another attempt / connection
    if (status != 200) {
        cJSON *res = cJSON_Parse(body);
        const char *err = cJSON_GetStringValue(cJSON_GetObjectItem(res, "error"));
        fprintf(stderr, "[auth] /auth/join failed (%d): %s\n", status, err ? err : status ? "error" : "account service unreachable");
        cJSON_Delete(res);
        // the server's hasJoined fails and changes nothing; it answers REFUSED
    }
    char key[sizeof(AUTH_JOIN_KEY_PREFIX) + AUTH_JOIN_NONCE_LEN];
    snprintf(key, sizeof(key), AUTH_JOIN_KEY_PREFIX "%s", g_live.hj_nonce);
    live_send_auth(g_client, NET_ACCOUNT_AUTH_HAS_JOINED, g_live.hj_name, key);
}

static void live_start(Client *client, const char *want, const double now) {
    const int guest = !strcmp(want, LIVE_GUEST);
    int kind = NET_ACCOUNT_AUTH_GUEST;
    if (!guest) {
        // as the JOIN: the token only to a lobby the brain handed out (net_join.c), hasJoined elsewhere
        if (servers_ui_brain_mode() && (g_live.kinds & NET_ACCOUNT_LIVE_SESSION)) kind = NET_ACCOUNT_AUTH_SESSION;
        else if ((g_live.kinds & NET_ACCOUNT_LIVE_HAS_JOINED) && g_live.nonce_ok) kind = NET_ACCOUNT_AUTH_HAS_JOINED;
        else {
            snprintf(g_live.attempt, sizeof(g_live.attempt), "%s", want);
            printf("[auth] signed in; this server cannot verify accounts mid-session: playing on as before\n");
            return;
        }
    }
    if (!++g_live.seq) g_live.seq = 1; // 0 is a push's
    g_live.pending = 1;
    g_live.pending_at = now;
    snprintf(g_live.pending_key, sizeof(g_live.pending_key), "%s", want);
    g_client = client;
    if (kind == NET_ACCOUNT_AUTH_GUEST) {
        printf("[auth] signed out: back to a guest on this connection\n");
        live_send_auth(client, kind, NULL, NULL);
        return;
    }
    const AccountInfo *acc = account_get();
    if (kind == NET_ACCOUNT_AUTH_SESSION) {
        printf("[auth] signed in as %s: verifying it on this connection (session)\n", acc ? acc->name : "?");
        live_send_auth(client, kind, NULL, want);
        return;
    }
    // hasJoined: POST <account service>/auth/join {token, serverHash}, then the "hj1:" (online_mode_on_challenge)
    snprintf(g_live.hj_name, sizeof(g_live.hj_name), "%s", acc ? acc->name : "");
    random_nonce(g_live.hj_nonce);
    char hash[AUTH_SERVER_HASH_LEN + 1];
    kn_server_hash(g_live.server_nonce, g_live.public_key, g_live.hj_nonce, hash);
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "token", want);
    cJSON_AddStringToObject(body, "serverHash", hash);
    char *text = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    char url[320];
    snprintf(url, sizeof(url), "%s/auth/join", backend_url());
    printf("[auth] signed in as %s: verifying it on this connection (hasJoined %.12s...)\n", g_live.hj_name, hash);
    g_live.generation++;
    if (!text || !backend_http_request("POST", url, text, on_live_join_done, (void *) (uintptr_t) g_live.generation)) {
        fprintf(stderr, "[auth] could not reach %s\n", url);
        on_live_join_done((void *) (uintptr_t) g_live.generation, 0, NULL, 0);
    }
    free(text);
}

static void live_tick(Client *client) {
    if (!online_mode_live_ok(client)) return;
    const double now = glfwGetTime();
    if (g_live.pending) {
        if (now - g_live.pending_at < LIVE_ANSWER_WAIT) return;
        g_live.pending = 0; // no answer: this identity is not sent again
        snprintf(g_live.attempt, sizeof(g_live.attempt), "%s", g_live.pending_key);
        fprintf(stderr, "[auth] the server did not answer the sign-in\n");
    }
    if (now < g_live.retry_at) return;
    const char *want = live_desired();
    if (!strcmp(want, g_live.synced) || !strcmp(want, g_live.attempt)) return;
    live_start(client, want, now);
}

static void live_rights_text(const unsigned rights, char *out, const size_t size) {
    static const char *const names[] = {"dev", "host", "op", "mod"}; // NET_RIGHTS_* bit order
    size_t len = 0;
    out[0] = 0;
    for (unsigned i = 0; i < 4; i++)
        if (rights & (1u << i)) len += (size_t) snprintf(out + len, len < size ? size - len : 0, "%s%s", len ? "+" : "", names[i]);
    if (!len) snprintf(out, size, "none");
}

void online_mode_on_rights(void *client_ptr, const void *body, const unsigned length) {
    Client *client = client_ptr;
    if (!client || !body || length < 4) return;
    NetAccountRightsPacket r;
    memset(&r, 0, sizeof(r));
    memcpy(&r, body, length < sizeof(r) ? length : sizeof(r));
    r.name[sizeof(r.name) - 1] = 0;
    r.message[sizeof(r.message) - 1] = 0;
    const int first = !g_live.have || g_live.conn_id != r.conn_id;
    const unsigned old_rights = first ? 0u : g_live.rights;
    const int old_account = first ? 0 : g_live.account_id;
    const int answer = !first && g_live.pending && r.seq == g_live.seq && r.status != NET_ACCOUNT_RIGHTS_STATE;
    g_live.have = 1;
    g_live.conn_id = r.conn_id;
    g_live.kinds = r.live_kinds;
    if (first) {
        g_live.pending = 0;
        g_live.attempt[0] = 0;
        g_live.notified[0] = 0;
        g_live.retry_at = 0.0;
        // the join's identity: the menu's token when the server has an account for us
        const char *want = live_desired();
        snprintf(g_live.synced, sizeof(g_live.synced), "%s", !r.account_id ? LIVE_GUEST : strcmp(want, LIVE_GUEST) ? want : "?");
    } else if (r.status == NET_ACCOUNT_RIGHTS_STATE && !r.account_id) {
        snprintf(g_live.synced, sizeof(g_live.synced), "%s", LIVE_GUEST);
    }
    if (answer) {
        g_live.pending = 0;
        if (r.status == NET_ACCOUNT_RIGHTS_OK) {
            snprintf(g_live.synced, sizeof(g_live.synced), "%s", g_live.pending_key);
            g_live.attempt[0] = 0;
        } else if (r.status == NET_ACCOUNT_RIGHTS_BUSY || r.status == NET_ACCOUNT_RIGHTS_UNAVAILABLE) {
            // throttled / a verification running, or the server's account service is out of reach: later
            const double wait = r.status == NET_ACCOUNT_RIGHTS_BUSY ? LIVE_BUSY_RETRY : LIVE_DOWN_RETRY;
            g_live.retry_at = glfwGetTime() + wait;
            printf("[auth] the server could not take the sign-in now (%s): again in %.0f s\n", r.message, wait);
        } else {
            snprintf(g_live.attempt, sizeof(g_live.attempt), "%s", g_live.pending_key);
            printf("[auth] the server did not take the sign-in: %s\n", r.message[0] ? r.message : "refused");
        }
        if (r.status != NET_ACCOUNT_RIGHTS_OK && r.status != NET_ACCOUNT_RIGHTS_BUSY && strcmp(g_live.notified, g_live.pending_key)) {
            snprintf(g_live.notified, sizeof(g_live.notified), "%s", g_live.pending_key);
            char text[192];
            I18N_F(text, "kn.auth.liveRefused", "This server could not verify your account: you play on as before ({0})",
                   r.message[0] ? r.message : I18N("kn.auth.refused", "refused"));
            ui_notify(text);
        }
    }
    g_live.rights = r.rights;
    g_live.account_id = r.account_id;
    if (first || r.rights != old_rights || r.account_id != old_account) {
        char was[48], now[48];
        live_rights_text(old_rights, was, sizeof(was));
        live_rights_text(r.rights, now, sizeof(now));
        printf("[auth] connection %d: account %d -> %d, rights %s -> %s (%s)\n", r.conn_id, old_account, r.account_id, first ? "?" : was, now,
               r.status == NET_ACCOUNT_RIGHTS_STATE ? "pushed" : "sign-in");
        object_manager_ui_rights_changed(); // the spawn menu and the KN guns ask for their caps again
    }
}
