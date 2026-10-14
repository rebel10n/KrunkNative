// Online / offline mode and the online-mode join handshake (client/include/online_mode.h).
#include <client.h>
#include <shared.h>
#include <online_mode.h>
#include <auth_join.h>
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

void online_mode_on_challenge(void *client_ptr, const void *challenge_in) {
    Client *client = client_ptr;
    NetAuthChallengePacket ch;
    memcpy(&ch, challenge_in, sizeof(ch));
    ch.server_nonce[sizeof(ch.server_nonce) - 1] = 0;
    ch.public_key[sizeof(ch.public_key) - 1] = 0;
    ch.auth_url[sizeof(ch.auth_url) - 1] = 0;
    g_client = client;
    online_mode_on_connect(); // one challenge per connection: late replies of the previous one are dropped
    if (!ch.online_mode) return; // offline_mode server: the guest JOIN already sent is all it needs
    const AccountInfo *acc = account_get();
    char saved[8] = "";
    if (g_online && !acc && !g_join.no_defer && html_storage_get("krunker_token", saved, sizeof(saved)) > 0 && saved[0] && backend_enabled()) {
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

void online_mode_tick(void *client) {
    if (g_join.has_deferred && (account_get() || glfwGetTime() - g_join.deferred_at > DEFER_SECONDS)) {
        NetAuthChallengePacket ch = g_join.deferred;
        g_join.has_deferred = 0;
        g_join.no_defer = 1; // no second deferral: signed in by now, or a guest
        online_mode_on_challenge(client, &ch);
        g_join.no_defer = 0;
        return;
    }
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
