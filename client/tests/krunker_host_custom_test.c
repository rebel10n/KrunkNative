// krunker_net_host_custom (client/src/net/krunker_net.c): a custom game hosted on krunker.io's official
// servers. v10's createPrivateRoom (v10.js:198129-198130) sends ["custom", config, rid, rehost] over the
// official socket (rid nil for a signed-in account; a guest's security-challenge answer is not implemented
// here), and the reply "cust" [message, gameId, teams] (v10's customRoomResponse, _67341, v10.js:197942)
// answers it once. The reply goes through krunker_net.c's own socket dispatch (krunker_link_on_frame under
// K.lock, as ws_thread does) and is delivered by its tick; the clock is fake, so the timeout runs without
// sleeping. The account and the clock are doubles; nothing opens a socket.
//   cmake-build-debug/krunker_host_custom_test
#include "../src/net/krunker_net.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail, g_pass;
#define CHECK(cond, ...) do { if (cond) g_pass++; else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// ---- doubles: the clock and the krunker.io account
static double g_now = 100.0;
double glfwGetTime(void) { return g_now; }
static int g_signed_in = 1;
int krunker_account_signed_in(void) { return g_signed_in; }

// ---- the transport's other callers: the client, HTTP, the matchmaker, the captcha, the maps and the UI. The
// host path never reaches them; a call aborts the test, so one that is ever reached is seen.
#define NOT_REACHED() abort()
int backend_http_get_external(const char *url, int timeout_ms, BackendHttpFn fn, void *user) { (void) url; (void) timeout_ms; (void) fn; (void) user; NOT_REACHED(); }
void client_clear_players(Client *c) { (void) c; NOT_REACHED(); }
void client_leave_game(Client *c) { (void) c; NOT_REACHED(); }
int client_map_list_add(Client *c, const cJSON *map) { (void) c; (void) map; NOT_REACHED(); }
void client_net_log_packet(const char *dir, NetPacketType type, const void *payload, uint16_t length) { (void) dir; (void) type; (void) payload; (void) length; NOT_REACHED(); }
void client_net_queue_packet(Client *c, const NetPacket *packet) { (void) c; (void) packet; NOT_REACHED(); }
void client_net_release(Client *c) { (void) c; NOT_REACHED(); }
void client_net_set_drop(Client *c, const char *tag, int close_code, const char *fmt, ...) { (void) c; (void) tag; (void) close_code; (void) fmt; NOT_REACHED(); }
void client_set_custom_game_id(Client *c, const cJSON *map, const char *id) { (void) c; (void) map; (void) id; NOT_REACHED(); }
const cJSON *g_maps[46];
int html_ui_available(void) { NOT_REACHED(); }
void http_conn_close(HttpConn *c) { (void) c; NOT_REACHED(); }
int http_conn_open(HttpConn *c, const HttpAddr *a, int timeout_ms) { (void) c; (void) a; (void) timeout_ms; NOT_REACHED(); }
int http_conn_pending(const HttpConn *c) { (void) c; NOT_REACHED(); }
ssize_t http_conn_recv(HttpConn *c, void *buf, size_t len) { (void) c; (void) buf; (void) len; NOT_REACHED(); }
int http_conn_send(HttpConn *c, const void *data, size_t len) { (void) c; (void) data; (void) len; NOT_REACHED(); }
int http_proxy_for(const char *host, int tls, HttpProxy *out) { (void) host; (void) tls; (void) out; NOT_REACHED(); }
void http_proxy_label(const HttpProxy *p, char *out, size_t n) { (void) p; (void) out; (void) n; NOT_REACHED(); }
const char *i18n_replace(char *out, size_t n, const char *text, int argc, const char *const *argv) { (void) out; (void) n; (void) text; (void) argc; (void) argv; NOT_REACHED(); }
const char *i18n_tr(const char *key, const char *english) { (void) key; (void) english; NOT_REACHED(); }
const char *i18n_trf(char *out, size_t n, const char *key, const char *english, int argc, const char *const *argv) { (void) out; (void) n; (void) key; (void) english; (void) argc; (void) argv; NOT_REACHED(); }
JoinErrorPlace join_error_place(int in_game) { (void) in_game; NOT_REACHED(); }
void join_error_text(const char *code, char *out, size_t n) { (void) code; (void) out; (void) n; NOT_REACHED(); }
void krunker_account_fresh_token(KrunkerAccountTokenFn fn, void *user) { (void) fn; (void) user; NOT_REACHED(); }
const char *krunker_account_name(void) { NOT_REACHED(); }
void krunker_account_note_game(const char *name, int level) { (void) name; (void) level; NOT_REACHED(); }
void krunker_account_note_points(double points) { (void) points; NOT_REACHED(); }
int krunker_account_owns_captcha(void) { NOT_REACHED(); }
void krunker_build_fetch_set_http(KrunkerBuildHttpGet get) { (void) get; NOT_REACHED(); }
const char *krunker_build_fetch_url(int production, const char *setting) { (void) production; (void) setting; NOT_REACHED(); }
int krunker_build_refresh(const char *url, int force, KrunkerBuildReady done, void *user) { (void) url; (void) force; (void) done; (void) user; NOT_REACHED(); }
void krunker_captcha_ask(int attempt, int rejected, KrunkerCaptchaDone done, void *user) { (void) attempt; (void) rejected; (void) done; (void) user; NOT_REACHED(); }
void krunker_captcha_close(void) { NOT_REACHED(); }
void krunker_captcha_pow_begin(void) { NOT_REACHED(); }
void krunker_captcha_pow_end(int solved) { (void) solved; NOT_REACHED(); }
void krunker_captcha_tick(void) { NOT_REACHED(); }
void krunker_ccr_free(KrunkerCcr *out) { (void) out; NOT_REACHED(); }
int krunker_cheat_answer(const char *nonce, const char *ciphertext_b64, KrunkerCcr *out) { (void) nonce; (void) ciphertext_b64; (void) out; NOT_REACHED(); }
void krunker_host_url(KrunkerHost host, const char *path, char *out, size_t n) { (void) host; (void) path; (void) out; (void) n; NOT_REACHED(); }
int krunker_link_prophunt_frame(KrunkerLink *l, KrunkerS2C id, const KrunkerFrame *f) { (void) l; (void) id; (void) f; NOT_REACHED(); }
void krunker_link_prophunt_spawned(KrunkerLink *l, int sid, int team, const KrunkerValue *prop_id) { (void) l; (void) sid; (void) team; (void) prop_id; NOT_REACHED(); }
int krunker_link_prophunt_send(KrunkerLink *l, uint16_t subtype, const uint8_t *body, uint16_t length) { (void) l; (void) subtype; (void) body; (void) length; NOT_REACHED(); }
void krunker_map_request_begin(KrunkerMapRequest *r, int index, cJSON *custom, const char *custom_id) { (void) r; (void) index; (void) custom; (void) custom_id; NOT_REACHED(); }
void krunker_map_request_cancel(KrunkerMapRequest *r) { (void) r; NOT_REACHED(); }
KrunkerMapStatus krunker_map_request_poll(KrunkerMapRequest *r, const cJSON **out, char *why, size_t why_size) { (void) r; (void) out; (void) why; (void) why_size; NOT_REACHED(); }
void krunker_map_source_set_fetch(KrunkerMapFetchFn fn) { (void) fn; } // link_setup's; no maps are fetched here
const char *krunker_seek_region(void) { NOT_REACHED(); }
void krunker_seek_region_resolve(KrunkerRegionFn fn, void *user) { (void) fn; (void) user; NOT_REACHED(); }
int menu_connecting(void) { NOT_REACHED(); }
void menu_show_connecting_map(const char *map) { (void) map; NOT_REACHED(); }
void menu_connect_stopped(void) {} // quick_play_retry.c's cancel (no Quick Play search runs here)
void ui_loading_popup(const char *text) { (void) text; NOT_REACHED(); }
void ui_loadout_clear_pops(void) {} // quick_play_retry.c's end (its popup is never up here)
const GameModeConfig *mode_config(int id) { (void) id; NOT_REACHED(); }
void on_client_socket_connect(Client *c) { (void) c; NOT_REACHED(); }
void on_client_socket_disconnect(Client *c) { (void) c; NOT_REACHED(); }
void on_client_socket_failed(Client *c, const char *message) { (void) c; (void) message; NOT_REACHED(); }
int ranked_match_host(const char *connection, char *host, size_t host_n) { (void) connection; (void) host; (void) host_n; NOT_REACHED(); }
int ranked_match_url(const char *connection, int production, int secure, const char *token, char *url, size_t url_n, char *log, size_t log_n) {
    (void) connection; (void) production; (void) secure; (void) token; (void) url; (void) url_n; (void) log; (void) log_n; NOT_REACHED();
}
size_t rules_class_count(void) { NOT_REACHED(); }
const ClassConfig *rules_classes(void) { NOT_REACHED(); }
const Weapon *rules_weapon(int id) { (void) id; NOT_REACHED(); }
int seek_prefs_map(void) { NOT_REACHED(); }
int seek_prefs_mode(void) { NOT_REACHED(); }
void seek_prefs_dq_fields(char *out, size_t n) { (void) out; (void) n; NOT_REACHED(); }
int seek_prefs_flag(const void *value) { (void) value; NOT_REACHED(); }
const char *seek_prefs_krunker_region(void) { NOT_REACHED(); }
int seek_prefs_lobby_ok(int mode, const char *map_name, unsigned flags) { (void) mode; (void) map_name; (void) flags; NOT_REACHED(); }
void ui_error_popup(const char *text) { (void) text; NOT_REACHED(); }

// ---- the done callback: how often it ran and what it got
static struct {
    int calls;
    int game_set, error_set;
    char game[96], region[48], error[256];
} g_done;

static void on_done(void *user, const char *game_id, const char *region, const char *error) {
    (void) user;
    g_done.calls++;
    g_done.game_set = game_id != NULL;
    g_done.error_set = error != NULL;
    snprintf(g_done.game, sizeof(g_done.game), "%s", game_id ? game_id : "");
    snprintf(g_done.region, sizeof(g_done.region), "%s", region ? region : "");
    snprintf(g_done.error, sizeof(g_done.error), "%s", error ? error : "");
}

static void drain_out(void) {
    while (K.out_head) {
        OutFrame *next = K.out_head->next;
        free(K.out_head);
        K.out_head = next;
    }
    K.out_tail = NULL;
}

static void reset_all(void) {
    memset(&g_done, 0, sizeof(g_done));
    memset(&hosting, 0, sizeof(hosting));
    g_signed_in = 1;
    K.state = KS_OPEN;
    snprintf(K.region, sizeof(K.region), "%s", "de-fra");
    drain_out();
}

// v10's request object, as createPrivateRoom builds it (v10.js:198000-198128): the flags, the lists, the
// rule overrides (serverConfig varN keys), the comp rosters and the weapon / class overrides.
static cJSON *sample_config(void) {
    cJSON *c = cJSON_CreateObject();
    cJSON_AddBoolToObject(c, "private", 1);
    cJSON_AddBoolToObject(c, "krRewards", 0);
    cJSON_AddBoolToObject(c, "defaultSetts", 1);
    cJSON_AddBoolToObject(c, "share", 0);
    cJSON_AddNullToObject(c, "customMap");
    cJSON_AddNullToObject(c, "rawMapData");
    cJSON *maps = cJSON_AddArrayToObject(c, "maps");
    cJSON_AddItemToArray(maps, cJSON_CreateNumber(0));
    cJSON_AddItemToArray(maps, cJSON_CreateNumber(2));
    cJSON *modes = cJSON_AddArrayToObject(c, "modes");
    cJSON_AddItemToArray(modes, cJSON_CreateNumber(1));
    cJSON *classes = cJSON_AddArrayToObject(c, "classes");
    cJSON_AddItemToArray(classes, cJSON_CreateNumber(0));
    cJSON_AddItemToArray(classes, cJSON_CreateNumber(3));
    cJSON_AddNumberToObject(c, "override", 0);
    cJSON_AddStringToObject(c, "passCode", "letmein");
    cJSON_AddNumberToObject(c, "gravMlt", 1.5);
    cJSON_AddNumberToObject(c, "health", 100);
    cJSON_AddBoolToObject(c, "nameTags", 1);
    cJSON_AddNumberToObject(c, "gameTime", 600);
    cJSON *teams = cJSON_AddArrayToObject(c, "teams");
    cJSON *t1 = cJSON_CreateArray();
    cJSON_AddItemToArray(t1, cJSON_CreateString("alice"));
    cJSON_AddItemToArray(t1, cJSON_CreateString("bob"));
    cJSON_AddItemToArray(teams, t1);
    cJSON_AddItemToArray(teams, cJSON_CreateArray());
    cJSON *specs = cJSON_AddArrayToObject(c, "specs");
    cJSON_AddItemToArray(specs, cJSON_CreateString("dave"));
    cJSON *weps = cJSON_AddObjectToObject(c, "weps");
    cJSON *w0 = cJSON_AddObjectToObject(weps, "0");
    cJSON_AddNumberToObject(w0, "dmg", 25);
    cJSON_AddNumberToObject(w0, "rate", 0.25);
    return c;
}

// the decoded value holds the same keys and values as the JSON it was sent from (no extra keys)
static int same_value(const cJSON *j, const KrunkerValue *v) {
    if (!v) return 0;
    if (cJSON_IsNull(j)) return v->type == KRUNKER_NIL;
    if (cJSON_IsBool(j)) return v->type == KRUNKER_BOOL && v->u.boolean == cJSON_IsTrue(j);
    if (cJSON_IsNumber(j)) return v->type == KRUNKER_NUM && v->u.num == j->valuedouble;
    if (cJSON_IsString(j)) return v->type == KRUNKER_STR && v->len == strlen(j->valuestring) && !memcmp(v->u.str, j->valuestring, v->len);
    if (cJSON_IsArray(j)) {
        if (v->type != KRUNKER_ARRAY || v->len != (uint32_t) cJSON_GetArraySize(j)) return 0;
        uint32_t i = 0;
        const cJSON *c;
        cJSON_ArrayForEach(c, j) {
            if (!same_value(c, krunker_at(v, i++))) return 0;
        }
        return 1;
    }
    if (cJSON_IsObject(j)) {
        if (v->type != KRUNKER_MAP || v->len != (uint32_t) cJSON_GetArraySize(j)) return 0;
        const cJSON *c;
        cJSON_ArrayForEach(c, j) {
            if (!same_value(c, krunker_get(v, c->string))) return 0;
        }
        return 1;
    }
    return 0;
}

// the official server's answer, through krunker_link_on_frame under K.lock (ws_thread's dispatch)
static void deliver_cust(const char *message, const char *game_id, const int teams) {
    KrunkerValue args[3];
    args[0] = message ? KRUNKER_V_STR(message) : KRUNKER_V_NIL;
    args[1] = game_id ? KRUNKER_V_STR(game_id) : KRUNKER_V_NIL;
    args[2] = KRUNKER_V_BOOL(teams);
    KrunkerBuf buf = {0};
    CHECK(krunker_proto_encode_ah(0, "cust", args, 3, &buf) == KRUNKER_PROTO_OK, "encode the cust frame");
    pthread_mutex_lock(&K.lock);
    krunker_link_on_frame(&K.link, buf.data, buf.len);
    pthread_mutex_unlock(&K.lock);
    krunker_buf_free(&buf);
}

// (a) the request frame: name "custom", the config as v10 sends it, rid nil (signed in), rehost false
static void test_request_frame(void) {
    reset_all();
    cJSON *cfg = sample_config();
    CHECK(krunker_net_host_custom(cfg, on_done, NULL) == 1, "a signed-in host over an open official socket is sent");
    CHECK(K.out_head && !K.out_head->next, "exactly one frame left the socket");
    if (K.out_head) {
        KrunkerFrame f;
        const int err = krunker_proto_decode(K.out_head->data, K.out_head->len, 0, &f);
        CHECK(err == KRUNKER_PROTO_OK, "the frame decodes (%s)", krunker_proto_strerror(err));
        if (err == KRUNKER_PROTO_OK) {
            CHECK(f.name_len == 6 && !memcmp(f.name, "custom", 6), "packet name custom, got %.*s", (int) f.name_len, f.name);
            CHECK(f.argc == 3, "three args after the name (config, rid, rehost), got %u", (unsigned) f.argc);
            if (f.argc == 3) {
                CHECK(same_value(cfg, krunker_frame_arg(&f, 0)), "the config decodes back to the same keys and values");
                CHECK(krunker_frame_arg(&f, 1) && krunker_frame_arg(&f, 1)->type == KRUNKER_NIL, "rid is nil for a signed-in account");
                CHECK(krunker_frame_arg(&f, 2) && krunker_frame_arg(&f, 2)->type == KRUNKER_BOOL && !krunker_frame_arg(&f, 2)->u.boolean,
                      "rehost is false");
            }
        }
        krunker_frame_free(&f);
    }
    drain_out();
    CHECK(g_done.calls == 0, "the send does not call done");
    cJSON_Delete(cfg);
}

// the refusals: no official connection, a guest, a config that is not an object, no done, a second request
static void test_refusals(void) {
    cJSON *cfg = sample_config();

    reset_all();
    K.state = KS_OFF;
    CHECK(krunker_net_host_custom(cfg, on_done, NULL) == 0, "no official connection: not sent");
    CHECK(!K.out_head, "nothing left the socket without a connection");

    reset_all();
    g_signed_in = 0;
    CHECK(krunker_net_host_custom(cfg, on_done, NULL) == 0, "a guest is refused (the security challenge is not implemented)");
    CHECK(!K.out_head, "a guest sends nothing");
    g_signed_in = 1;

    reset_all();
    cJSON *list = cJSON_CreateArray();
    CHECK(krunker_net_host_custom(list, on_done, NULL) == 0, "a config that is not an object is refused");
    cJSON_Delete(list);
    CHECK(krunker_net_host_custom(cfg, NULL, NULL) == 0, "no done callback: refused");

    reset_all();
    CHECK(krunker_net_host_custom(cfg, on_done, NULL) == 1, "first request sent");
    CHECK(krunker_net_host_custom(cfg, on_done, NULL) == 0, "a second request while one waits is refused");
    CHECK(K.out_head && !K.out_head->next, "only the first request left the socket");
    drain_out();
    memset(&hosting, 0, sizeof(hosting));
    cJSON_Delete(cfg);
}

// (b) the reply: done once, with the game id, from the tick; a later duplicate is dropped
static void test_reply_game(void) {
    reset_all();
    cJSON *cfg = sample_config();
    CHECK(krunker_net_host_custom(cfg, on_done, NULL) == 1, "request sent");
    deliver_cust("Game Created", "NY:bbbbb", 1);
    CHECK(g_done.calls == 0, "the reply is delivered by the tick, not on the socket thread");
    host_custom_tick(g_now + 0.5);
    CHECK(g_done.calls == 1, "done ran once on the tick, got %d", g_done.calls);
    CHECK(g_done.game_set && !strcmp(g_done.game, "NY:bbbbb"), "done gets the game id, got '%s'", g_done.game);
    CHECK(!g_done.error_set, "no error with a game id");
    CHECK(!strcmp(g_done.region, "de-fra"), "done gets the session's region, got '%s'", g_done.region);
    host_custom_tick(g_now + 1.0);
    deliver_cust("Game Created", "NY:late", 1);
    host_custom_tick(g_now + 2.0);
    CHECK(g_done.calls == 1, "a duplicate reply does not call done again");
    cJSON_Delete(cfg);
}

// the server refused: its message is the error; no answer at all: v10's custom.tryagain
static void test_reply_error(void) {
    cJSON *cfg = sample_config();

    reset_all();
    CHECK(krunker_net_host_custom(cfg, on_done, NULL) == 1, "request sent");
    deliver_cust("Select at least one Mode.", NULL, 0);
    host_custom_tick(g_now + 0.5);
    CHECK(g_done.calls == 1 && !g_done.game_set && g_done.error_set, "a refusal calls done with an error, not a game");
    CHECK(!strcmp(g_done.error, "Select at least one Mode."), "the server's message is the error, got '%s'", g_done.error);

    reset_all();
    CHECK(krunker_net_host_custom(cfg, on_done, NULL) == 1, "request sent");
    deliver_cust(NULL, NULL, 0);
    host_custom_tick(g_now + 0.5);
    CHECK(g_done.calls == 1 && !g_done.game_set && !strcmp(g_done.error, "Please try again."), "an empty refusal reads Please try again., got '%s'",
          g_done.error);

    reset_all();
    CHECK(krunker_net_host_custom(cfg, on_done, NULL) == 1, "request sent");
    deliver_cust("Game Created", "NY:bad id!", 1);
    host_custom_tick(g_now + 0.5);
    CHECK(g_done.calls == 1 && !g_done.game_set && !strcmp(g_done.error, "Please try again."), "a game id that is not a Krunker id is refused, got '%s'",
          g_done.error);
    cJSON_Delete(cfg);
}

// (c) no answer: done once with "Please try again." after the HTTP timeout; a late answer is ignored
static void test_timeout(void) {
    cJSON *cfg = sample_config();
    reset_all();
    g_now = 100.0;
    CHECK(krunker_net_host_custom(cfg, on_done, NULL) == 1, "request sent");
    host_custom_tick(g_now + KRUNKER_HTTP_TIMEOUT_MS / 1000.0 - 1.0);
    CHECK(g_done.calls == 0, "no done before the timeout");
    host_custom_tick(g_now + KRUNKER_HTTP_TIMEOUT_MS / 1000.0 + 0.5);
    CHECK(g_done.calls == 1, "done after the timeout, got %d", g_done.calls);
    CHECK(!g_done.game_set && g_done.error_set && !strcmp(g_done.error, "Please try again."), "the timeout error is Please try again., got '%s'", g_done.error);
    deliver_cust("Game Created", "NY:late", 1);
    host_custom_tick(g_now + 20.0);
    CHECK(g_done.calls == 1, "a late answer does not call done again");
    CHECK(krunker_net_host_custom(cfg, on_done, NULL) == 1, "a new request is sent after the timeout");
    drain_out();
    cJSON_Delete(cfg);
}

int main(void) {
    link_setup();
    test_request_frame();
    test_refusals();
    test_reply_game();
    test_reply_error();
    test_timeout();
    drain_out();
    printf("krunker_host_custom_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
