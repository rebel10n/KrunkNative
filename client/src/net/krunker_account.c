// The krunker.io account (client/include/krunker_account.h): sign in / out against krunker.io's API
// and FRVR's auth service, the session's tokens, a fresh access token for krunker_net's joins.
// Requests go over http_stream.h (headers on every platform, the client proxy natively); their
// answers arrive on the main thread from krunker_account_tick.
#include <krunker_account.h>
#include <krunker_captcha.h>
#include <http_stream.h>
#include <html_dom.h>
#include <html_ui.h>
#include <ui_logic.h>
#include <progression.h>
#include <cJSON.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#define KEY_ACCESS "kio_access_token"
#define KEY_REFRESH "kio_refresh_token"
#define KEY_NAME "kio_username"
#define KEY_LEVEL "kio_level"
#define TOKEN_MAX 4096
#define BODY_MAX (256u * 1024u)
#define REQUEST_TIMEOUT_S 25.0 // a sign-in step / refresh with no answer by then fails (http_stream waits 180 s)
#define RANK_VAR 0.03          // config.rankVar (Account.updateLevel: floor(rankVar * sqrt(score)))
#define TOKEN_WAITERS 24 // the joins, plus the windows' API requests waiting on one refresh
#define API_SLOTS 16

typedef enum {
    REQ_NONE = 0,
    REQ_LOGIN,   // /auth/login/username | /auth/login/email
    REQ_CODE,    // /auth/2fa/challenge/<id> | /auth/code-challenge/<id>
    REQ_PROFILE, // /player/me
    REQ_REFRESH, // <auth>/refresh
    REQ_RECOVER, // /auth/recover-account
    REQ_API,     // krunker_account_api: a window's request to krunker.io's services
} ReqKind;

typedef struct {
    HttpStream *stream;
    ReqKind kind;
    int status;
    char *body;
    size_t len;
    double started;
} Req;

static struct {
    int loaded;
    KrunkerAccountState state;
    char access[TOKEN_MAX + 1], refresh[TOKEN_MAX + 1];
    char name[64];
    int level;
    // the account row's rankedPoints (v10 Account.rankedPoints): the game server's "a" / "ua" carry it, so
    // it is known once a krunker.io game has signed the account in this session (not saved)
    int rp_known;
    double rp;
    double score, funds; // /player/me (this run only: the menu shows them while it knows them)
    char message[300];
    int message_error;
    unsigned generation;
    // the sign-in in progress
    unsigned attempt; // bumped per sign-in / cancel: a late Turnstile answer of an older one is ignored
    int email;        // the login is an email
    char login[256], password[256];
    char challenge[128];
    char code_kind[8];  // "2fa" | "email"
    int captcha_open;   // our Turnstile request is the one krunker_captcha.h shows
    Req auth, refresh_req, recover_req;
    struct {
        KrunkerAccountTokenFn fn;
        void *user;
    } waiters[TOKEN_WAITERS];
    int waiter_count;
    int resuming;      // a krunker_account_resume's refresh is in flight
} A;

static KrunkerAccountEventFn g_event_fn;
static void *g_event_user;
static void emit(const KrunkerAccountEvent ev) {
    if (g_event_fn) g_event_fn(g_event_user, ev);
}

void krunker_account_listen_events(const KrunkerAccountEventFn fn, void *user) {
    g_event_fn = fn;
    g_event_user = user;
}

// krunker_account_api's requests (Req first: on_done gets the Req back)
typedef struct {
    Req r;
    int used;
    unsigned id;
    char method[8];
    char url[1024];
    char *body;
    KrunkerApiFn fn;
    void *user;
} ApiReq;
static ApiReq g_api[API_SLOTS];
static unsigned g_api_ids;

static void wipe(void *p, size_t n) {
    volatile unsigned char *v = p;
    while (n--) *v++ = 0;
}

static double now_s(void) { return (double) time(NULL); } // (whole seconds: only the request timeout uses it)

static void changed(void) { A.generation++; }

static void set_message(const int error, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void set_message(const int error, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(A.message, sizeof(A.message), fmt, ap);
    va_end(ap);
    A.message_error = error;
    changed();
}

// ---------------------------------------------------------------- endpoints

#ifdef __EMSCRIPTEN__
static char *query(const char *name) {
    return (char *) EM_ASM_PTR({
        var v = null;
        try { v = new URLSearchParams(location.search).get(UTF8ToString($0)); } catch (e) {}
        return v !== null ? stringToNewUTF8(v) : 0;
    }, name);
}
#endif

static void endpoint(const char *env, const char *param, const char *fallback, char *out, const size_t n) {
    out[0] = 0;
#ifdef __EMSCRIPTEN__
    (void) env;
    char *v = query(param);
    if (v && v[0]) snprintf(out, n, "%s", v);
    free(v);
#else
    (void) param;
    const char *v = getenv(env);
    if (v && v[0]) snprintf(out, n, "%s", v);
#endif
    if (!out[0]) snprintf(out, n, "%s", fallback);
    size_t len = strlen(out);
    while (len && out[len - 1] == '/') out[--len] = 0;
}

static void api_url(char *out, const size_t n, const char *path) {
    char base[256];
    endpoint("KRUNKNATIVE_KRUNKER_API", "krunkerApi", KRUNKER_ACCOUNT_DEFAULT_API, base, sizeof(base));
    snprintf(out, n, "%s%s", base, path);
}

void krunker_service_url(const KrunkerService svc, const char *path, char *out, const size_t n) {
    char base[256];
    switch (svc) {
        case KRUNKER_SVC_SOCIAL: endpoint("KRUNKNATIVE_KRUNKER_SOCIAL", "krunkerSocial", KRUNKER_ACCOUNT_DEFAULT_SOCIAL, base, sizeof(base)); break;
        case KRUNKER_SVC_EVENT_BUS: endpoint("KRUNKNATIVE_KRUNKER_EVENT_BUS", "krunkerEventBus", KRUNKER_ACCOUNT_DEFAULT_EVENT_BUS, base, sizeof(base)); break;
        default: endpoint("KRUNKNATIVE_KRUNKER_API", "krunkerApi", KRUNKER_ACCOUNT_DEFAULT_API, base, sizeof(base)); break;
    }
    // the API client joins base and path with one '/' (v10:27526)
    while (path && *path == '/') path++;
    snprintf(out, n, "%s/%s", base, path ? path : "");
}

static void auth_url(char *out, const size_t n, const char *path) {
    char base[256];
    endpoint("KRUNKNATIVE_KRUNKER_AUTH", "krunkerAuth", KRUNKER_ACCOUNT_DEFAULT_AUTH, base, sizeof(base));
    snprintf(out, n, "%s%s", base, path);
}

void krunker_host_url(const KrunkerHost host, const char *path, char *out, const size_t n) {
    char base[256];
    switch (host) {
        case KRUNKER_HOST_GAPI: endpoint("KRUNKNATIVE_KRUNKER_GAPI", "krunkerGapi", KRUNKER_ACCOUNT_DEFAULT_GAPI, base, sizeof(base)); break;
        case KRUNKER_HOST_GAMEFRONTEND:
            endpoint("KRUNKNATIVE_KRUNKER_GAMEFRONTEND", "krunkerGamefrontend", KRUNKER_ACCOUNT_DEFAULT_GAMEFRONTEND, base, sizeof(base));
            break;
        default: { // the data client (v10 API_URI, not gapi): a mirror that sets only KRUNKNATIVE_KRUNKER_API serves it there too
            char api[256];
            endpoint("KRUNKNATIVE_KRUNKER_API", "krunkerApi", KRUNKER_ACCOUNT_DEFAULT_DATA_API, api, sizeof(api));
            endpoint("KRUNKNATIVE_KRUNKER_DATA_API", "krunkerDataApi", api, base, sizeof(base));
            break;
        }
    }
    snprintf(out, n, "%s%s", base, path);
}

// ---------------------------------------------------------------- storage

static void storage_get(const char *key, char *out, const size_t n) {
    out[0] = 0;
    if (html_storage_get(key, out, n) < 0) out[0] = 0;
}

static void put(const char *key, const char *value) { html_storage_set(key, value && value[0] ? value : NULL); } // "" = removed

static void save(void) {
    put(KEY_ACCESS, A.access);
    put(KEY_REFRESH, A.refresh);
    put(KEY_NAME, A.name);
    char level[16] = "";
    if (A.level > 0) snprintf(level, sizeof(level), "%d", A.level);
    put(KEY_LEVEL, level);
}

static void load(void) {
    if (A.loaded) return;
    A.loaded = 1;
    storage_get(KEY_ACCESS, A.access, sizeof(A.access));
    storage_get(KEY_REFRESH, A.refresh, sizeof(A.refresh));
    storage_get(KEY_NAME, A.name, sizeof(A.name));
    char level[16];
    storage_get(KEY_LEVEL, level, sizeof(level));
    A.level = atoi(level);
    if (A.access[0] || A.refresh[0]) {
        A.state = KRUNKER_ACCOUNT_SIGNED_IN;
        printf("[kio-account] restored the krunker.io account %s\n", A.name[0] ? A.name : "(name unknown)");
    } else {
        A.name[0] = 0;
        A.level = 0;
    }
    changed();
}

// ---------------------------------------------------------------- JWT exp

static int b64url_value(const int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-' || c == '+') return 62;
    if (c == '_' || c == '/') return 63;
    return -1;
}

// The JWT's payload (its middle part) as JSON, NULL when the token is not a JWT.
static cJSON *jwt_payload(const char *token) {
    const char *a = strchr(token, '.');
    const char *b = a ? strchr(a + 1, '.') : NULL;
    if (!a || !b || b - a - 1 <= 0 || b - a > 8192) return NULL;
    char out[6200];
    size_t o = 0;
    unsigned acc = 0;
    int bits = 0;
    for (const char *p = a + 1; p < b; p++) {
        const int v = b64url_value((unsigned char) *p);
        if (v < 0) break;
        acc = (acc << 6) | (unsigned) v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o + 1 < sizeof(out)) out[o++] = (char) ((acc >> bits) & 0xff);
        }
    }
    out[o] = 0;
    return cJSON_Parse(out);
}

// Seconds the access token stays valid (exp - now), a large number when it carries no exp.
static double access_left(void) {
    if (!A.access[0]) return -1;
    cJSON *p = jwt_payload(A.access);
    const cJSON *exp = cJSON_GetObjectItemCaseSensitive(p, "exp");
    const double left = cJSON_IsNumber(exp) ? exp->valuedouble - (double) time(NULL) : 1e9;
    cJSON_Delete(p);
    return left;
}

// A name in the token's claims (username / name), when it has one.
static void name_from_token(void) {
    cJSON *p = jwt_payload(A.access);
    const char *keys[] = {"username", "name", "preferred_username"};
    for (size_t i = 0; p && i < sizeof(keys) / sizeof(keys[0]); i++) {
        const char *v = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(p, keys[i]));
        if (v && v[0]) {
            snprintf(A.name, sizeof(A.name), "%s", v);
            break;
        }
    }
    cJSON_Delete(p);
}

// ---------------------------------------------------------------- requests

static void on_open(void *user, const int status, const char *content_type) {
    (void) content_type;
    ((Req *) user)->status = status;
}

static void on_data(void *user, const char *data, const size_t len) {
    Req *r = user;
    if (r->len + len > BODY_MAX) return;
    char *grown = realloc(r->body, r->len + len + 1);
    if (!grown) return;
    r->body = grown;
    memcpy(r->body + r->len, data, len);
    r->len += len;
    r->body[r->len] = 0;
}

static void on_done(void *user, int status, const char *error);

static void req_clear(Req *r) {
    if (r->body) wipe(r->body, r->len);
    free(r->body);
    r->body = NULL;
    r->len = 0;
    r->status = 0;
    r->kind = REQ_NONE;
    r->stream = NULL;
}

static void req_cancel(Req *r) {
    if (r->stream) http_stream_cancel(r->stream);
    req_clear(r);
}

// A fresh X-Idempotency-Key (crypto.randomUUID's shape; the API client sets one per POST)
static void idempotency_key(char *out, const size_t n) {
    static int seeded;
    if (!seeded) {
        srand((unsigned) time(NULL) ^ (unsigned) (uintptr_t) out);
        seeded = 1;
    }
    static const char hex[] = "0123456789abcdef";
    const char *shape = "xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx";
    size_t i = 0;
    for (; shape[i] && i + 1 < n; i++) {
        const int r = rand() & 15;
        out[i] = shape[i] == 'x' ? hex[r] : shape[i] == 'y' ? hex[(r & 3) | 8] : shape[i];
    }
    out[i] = 0;
}

// headers: extra lines (never logged). body: JSON (POST). 1 = started. The log shows the URL up to
// a code step's challenge id (the id is not logged either).
static int req_start(Req *r, const ReqKind kind, const char *method, const char *url, const char *bearer, const char *body) {
    req_cancel(r);
    char headers[TOKEN_MAX + 160];
    size_t o = 0;
    o += (size_t) snprintf(headers + o, sizeof(headers) - o, "Accept: application/json\r\n");
    if (!strcmp(method, "POST")) {
        char key[40];
        idempotency_key(key, sizeof(key));
        o += (size_t) snprintf(headers + o, sizeof(headers) - o, "X-Idempotency-Key: %s\r\n", key);
    }
    if (bearer && bearer[0] && o < sizeof(headers)) snprintf(headers + o, sizeof(headers) - o, "Authorization: Bearer %s\r\n", bearer);
    static const HttpStreamCallbacks cb = {on_open, on_data, on_done};
    r->kind = kind;
    r->started = now_s();
    r->stream = http_stream_start(method, url, headers, body, &cb, r);
    wipe(headers, sizeof(headers));
    const char *cut = kind == REQ_CODE ? strrchr(url, '/') : NULL;
    printf("[kio-account] %s %.*s%s\n", method, cut ? (int) (cut - url + 1) : (int) strlen(url), url, cut ? "(challenge)" : "");
    if (!r->stream) {
        r->kind = REQ_NONE;
        return 0;
    }
    return 1;
}

static const char *json_str(const cJSON *o, const char *key) { return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, key)); }

static void escape_json(char *out, const size_t n, const char *s) {
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *) s; *p && o + 7 < n; p++) {
        if (*p == '"' || *p == '\\') {
            out[o++] = '\\';
            out[o++] = (char) *p;
        } else if (*p < 0x20) {
            o += (size_t) snprintf(out + o, n - o, "\\u%04x", *p);
        } else {
            out[o++] = (char) *p;
        }
    }
    out[o] = 0;
}

// ---------------------------------------------------------------- token waiters

static void serve_waiters(const char *token) {
    const int count = A.waiter_count;
    A.waiter_count = 0;
    for (int i = 0; i < count; i++) {
        if (A.waiters[i].fn) A.waiters[i].fn(A.waiters[i].user, token);
    }
}

// ---------------------------------------------------------------- session

static void forget(void) {
    wipe(A.access, sizeof(A.access));
    wipe(A.refresh, sizeof(A.refresh));
    A.name[0] = 0;
    A.level = 0;
    A.rp_known = 0;
    A.rp = 0;
    A.score = A.funds = 0;
    A.state = KRUNKER_ACCOUNT_GUEST;
    save();
}

static void end_attempt(void) {
    wipe(A.password, sizeof(A.password));
    wipe(A.challenge, sizeof(A.challenge));
    A.code_kind[0] = 0;
    A.attempt++;
    if (A.captcha_open) krunker_captcha_close(); // (never a join's challenge: krunker_net owns those)
    A.captcha_open = 0;
}

static void fail_sign_in(const char *text) {
    end_attempt();
    A.state = A.access[0] || A.refresh[0] ? KRUNKER_ACCOUNT_SIGNED_IN : KRUNKER_ACCOUNT_GUEST;
    printf("[kio-account] sign-in failed: %s\n", text);
    set_message(1, "%s", text);
    emit(KRUNKER_EV_SIGN_IN_FAILED);
}

static void signed_in(const char *access, const char *refresh) {
    A.rp_known = 0; // another balance until a game server says it (the account may differ)
    A.rp = 0;
    snprintf(A.access, sizeof(A.access), "%s", access);
    snprintf(A.refresh, sizeof(A.refresh), "%s", refresh ? refresh : "");
    if (!A.email && A.login[0]) snprintf(A.name, sizeof(A.name), "%s", A.login);
    else if (A.email) {
        const char *at = strchr(A.login, '@');
        snprintf(A.name, sizeof(A.name), "%.*s", at ? (int) (at - A.login) : (int) strlen(A.login), A.login);
    }
    name_from_token();
    A.level = 0;
    end_attempt();
    A.state = KRUNKER_ACCOUNT_SIGNED_IN;
    save();
    printf("[kio-account] signed in to krunker.io as %s\n", A.name);
    set_message(0, "%s", ""); // v10: the login modal closes (account set), nothing else is said
    // best effort: the account's own name (the game server's "a" names it too, krunker_account_note_game)
    char url[512];
    api_url(url, sizeof(url), "/player/me");
    req_start(&A.auth, REQ_PROFILE, "GET", url, A.access, NULL);
    emit(KRUNKER_EV_SIGNED_IN);
}

// The challenge loop's next step (iííìiïï): login_ok, a code, or a step for krunker.io itself.
static void on_auth_answer(const int status, cJSON *res) {
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(res, "data");
    const char *error = json_str(res, "error");
    const char *type = json_str(data, "type");
    char text[300];
    if (status == 429) {
        fail_sign_in(I18N("kn.kio.account.rateLimit", "krunker.io: too many sign-in attempts. Try again later."));
        return;
    }
    if (status < 200 || status >= 500 || !res) {
        char code[16];
        snprintf(code, sizeof(code), "%d", status);
        I18N_F(text, "kn.kio.account.serverError", "krunker.io's account service answered HTTP {0}: try again later.", code);
        fail_sign_in(text);
        return;
    }
    if (status != 200 || !cJSON_IsObject(data) || !type) {
        if (error && error[0]) I18N_F(text, "kn.kio.account.refused", "krunker.io: {0}", error);
        else snprintf(text, sizeof(text), "%s", I18N("kn.kio.account.failed", "krunker.io did not accept the sign-in."));
        fail_sign_in(text);
        return;
    }
    if (!strcmp(type, "login_ok")) {
        const char *access = json_str(data, "access_token"), *refresh = json_str(data, "refresh_token");
        if (!access || !access[0] || strlen(access) > TOKEN_MAX || (refresh && strlen(refresh) > TOKEN_MAX)) {
            fail_sign_in(I18N("kn.kio.account.failed", "krunker.io did not accept the sign-in."));
            return;
        }
        signed_in(access, refresh);
        return;
    }
    const char *challenge = json_str(data, "challenge_id");
    if ((!strcmp(type, "check_2fa") || !strcmp(type, "enter_code")) && challenge && challenge[0] && strlen(challenge) < sizeof(A.challenge)) {
        snprintf(A.challenge, sizeof(A.challenge), "%s", challenge);
        snprintf(A.code_kind, sizeof(A.code_kind), "%s", !strcmp(type, "check_2fa") ? "2fa" : "email");
        A.state = KRUNKER_ACCOUNT_CODE;
        printf("[kio-account] krunker.io asks for a %s code\n", A.code_kind);
        emit(KRUNKER_EV_CODE_ASKED);
        set_message(0, "%s", !strcmp(type, "check_2fa") ? I18N("kn.kio.account.ask2fa", "Enter the 6-digit code from your authenticator app.")
                                                        : I18N("kn.kio.account.askEmailCode", "Enter the code krunker.io sent to your email."));
        return;
    }
    // ensure_verified, ensure_migrated, select_username, update_password, kid_age_gate,
    // kid_parental_consent: steps of krunker.io's own page
    printf("[kio-account] krunker.io asks for a step of its own page (%s)\n", type);
    I18N_F(text, "kn.kio.account.finishOnSite", "krunker.io needs a step this client does not do ({0}): sign in once on krunker.io itself, then here again.", type);
    fail_sign_in(text);
}

static void on_profile(const int status, cJSON *res) {
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(res, "data");
    if (status != 200 || !cJSON_IsObject(data)) {
        printf("[kio-account] /player/me: HTTP %d (the name typed stays)\n", status);
        return;
    }
    const char *keys[] = {"username", "player_name", "name"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        const char *v = json_str(data, keys[i]);
        if (v && v[0]) {
            snprintf(A.name, sizeof(A.name), "%s", v);
            break;
        }
    }
    const cJSON *level = cJSON_GetObjectItemCaseSensitive(data, "level");
    const cJSON *score = cJSON_GetObjectItemCaseSensitive(data, "player_score");
    if (cJSON_IsNumber(level)) A.level = level->valueint;
    else if (cJSON_IsNumber(score)) A.level = progression_level(score->valuedouble, RANK_VAR);
    if (cJSON_IsNumber(score)) A.score = score->valuedouble;
    const char *funds_keys[] = {"player_funds", "funds", "kr"};
    for (size_t i = 0; i < sizeof(funds_keys) / sizeof(funds_keys[0]); i++) {
        const cJSON *f = cJSON_GetObjectItemCaseSensitive(data, funds_keys[i]);
        if (cJSON_IsNumber(f)) {
            A.funds = f->valuedouble;
            break;
        }
    }
    save();
    changed();
}

static void on_refresh(const int status, cJSON *res) {
    const char *access = json_str(res, "accessToken"), *refresh = json_str(res, "refreshToken");
    const int resuming = A.resuming;
    A.resuming = 0;
    if ((status == 200 || status == 201) && access && access[0] && strlen(access) <= TOKEN_MAX && (!refresh || strlen(refresh) <= TOKEN_MAX)) {
        snprintf(A.access, sizeof(A.access), "%s", access);
        if (refresh && refresh[0]) snprintf(A.refresh, sizeof(A.refresh), "%s", refresh);
        save();
        if (resuming) {
            A.state = KRUNKER_ACCOUNT_SIGNED_IN;
            printf("[kio-account] resumed the krunker.io account %s\n", A.name);
        } else {
            printf("[kio-account] the krunker.io session was refreshed\n");
        }
        changed();
        serve_waiters(A.access);
        emit(resuming ? KRUNKER_EV_RESUMED : KRUNKER_EV_REFRESHED);
        return;
    }
    if (resuming && status != 401) { // unreachable / an odd answer: nothing is resumed
        printf("[kio-account] resuming the krunker.io account failed (HTTP %d)\n", status);
        forget();
        set_message(1, "%s", I18N("kn.kio.account.unreachable", "Could not reach krunker.io's account service. Check your connection, or the proxy in Settings > Client."));
        serve_waiters(NULL);
        emit(KRUNKER_EV_RESUME_FAILED);
        return;
    }
    if (status == 401) { // FRVR's INVALID_CREDENTIALS: the refresh token is no good any more
        printf("[kio-account] the krunker.io session ended (refresh: HTTP %d): signed out\n", status);
        emit(KRUNKER_EV_EXPIRED);
        forget();
        set_message(1, "%s", I18N("kn.kio.account.expired", "Your krunker.io session ended: sign in again. Joining krunker.io servers as a guest until then."));
        printf("[kio-account] %s\n", A.message); // logged only: the session end shows in the account header, not as a toast
        serve_waiters(NULL);
        return;
    }
    // unreachable / an odd answer: the session stays; this join goes on with what is left of it
    printf("[kio-account] refreshing the krunker.io session failed (HTTP %d)\n", status);
    serve_waiters(access_left() > 5 ? A.access : NULL);
}

static void on_done(void *user, const int status, const char *error) {
    Req *r = user;
    const ReqKind kind = r->kind;
    const int http = status ? (r->status ? r->status : status) : 0;
    cJSON *res = http && r->body ? cJSON_Parse(r->body) : NULL;
    r->stream = NULL; // finished: freed by http_stream_poll
    if (!http) printf("[kio-account] request failed: %s\n", error ? error : "no answer");
    switch (kind) {
        case REQ_LOGIN:
        case REQ_CODE:
            if (!http) fail_sign_in(I18N("kn.kio.account.unreachable", "Could not reach krunker.io's account service. Check your connection, or the proxy in Settings > Client."));
            else on_auth_answer(http, res);
            break;
        case REQ_API: {
            ApiReq *q = (ApiReq *) r;
            const KrunkerApiFn fn = q->fn;
            void *u = q->user;
            free(q->body);
            q->body = NULL;
            q->used = 0;
            q->fn = NULL;
            if (fn) fn(u, http, res);
            break;
        }
        case REQ_PROFILE: on_profile(http, res); break;
        case REQ_RECOVER: // recoverAccount (v10 _39188): any answer is "sent", a failure "Error"
            if (http >= 200 && http < 300) set_message(0, "%s", I18N("generic.auth.recoverSuccess", "Email with recovery instructions was sent. Check inbox and spam folder."));
            else set_message(1, "%s", I18N("generic.error", "Error"));
            break;
        case REQ_REFRESH: on_refresh(http, res); break;
        default: break;
    }
    cJSON_Delete(res);
    if (r->kind == kind) req_clear(r); // (a new request may have started from the handler)
    else if (r->body && !r->stream) req_clear(r);
}

// ---------------------------------------------------------------- sign in

static void post_login(const char *turnstile) {
    char url[512], login[600], password[600], body[2400];
    escape_json(login, sizeof(login), A.login);
    escape_json(password, sizeof(password), A.password);
    if (A.email) {
        api_url(url, sizeof(url), "/auth/login/email");
        snprintf(body, sizeof(body), "{\"email\":\"%s\",\"password\":\"%s\"}", login, password);
    } else {
        char token[KRUNKER_CAPTCHA_TOKEN_MAX + 8] = "";
        if (turnstile) escape_json(token, sizeof(token), turnstile);
        api_url(url, sizeof(url), "/auth/login/username");
        // (the body would not fit a full-length token next to the rest: such a token is not sent)
        if (token[0] && strlen(token) + strlen(login) + strlen(password) + 64 < sizeof(body))
            snprintf(body, sizeof(body), "{\"username\":\"%s\",\"password\":\"%s\",\"b\":\"%s\"}", login, password, token);
        else snprintf(body, sizeof(body), "{\"username\":\"%s\",\"password\":\"%s\"}", login, password);
        wipe(token, sizeof(token));
    }
    wipe(password, sizeof(password));
    wipe(A.password, sizeof(A.password)); // sent once: never kept
    A.state = KRUNKER_ACCOUNT_BUSY;
    const int ok = req_start(&A.auth, REQ_LOGIN, "POST", url, NULL, body);
    wipe(body, sizeof(body));
    if (!ok) fail_sign_in(I18N("kn.kio.account.unreachable", "Could not reach krunker.io's account service. Check your connection, or the proxy in Settings > Client."));
    changed();
}

static void turnstile_done(void *user, const char *token, const int unavailable) {
    if ((unsigned) (uintptr_t) user != A.attempt || A.state != KRUNKER_ACCOUNT_BUSY) return;
    A.captcha_open = 0;
    if (!token) { // the challenge could not be completed: a login without its token is refused (the generic failure)
        (void) unavailable;
        fail_sign_in(I18N("kn.kio.account.failed", "krunker.io did not accept the sign-in."));
        return;
    }
    post_login(token);
}

int krunker_account_sign_in(const char *login, const char *password) {
    load();
    if (A.state == KRUNKER_ACCOUNT_BUSY) {
        set_message(1, "%s", I18N("kn.kio.account.busy", "A sign-in is already running."));
        return 0;
    }
    if (!login || !login[0] || !password || !password[0] || strlen(login) >= sizeof(A.login) || strlen(password) >= sizeof(A.password)) {
        set_message(1, "%s", I18N("generic.auth.fillAllFields", "Please fill in all fields"));
        return 0;
    }
    req_cancel(&A.auth);
    end_attempt();
    snprintf(A.login, sizeof(A.login), "%s", login);
    snprintf(A.password, sizeof(A.password), "%s", password);
    A.email = strchr(login, '@') != NULL;
    A.state = KRUNKER_ACCOUNT_BUSY;
    set_message(0, "%s", I18N("generic.wait", "Please wait..."));
    printf("[kio-account] signing in to krunker.io (%s)\n", A.email ? "email" : "username");
    if (A.email) {
        post_login(NULL); // loginEmail has no Turnstile step
    } else {
        // the live client's getCaptchaToken before loginUsername: the player solves Turnstile
        A.captcha_open = 1;
        krunker_captcha_ask(1, 0, turnstile_done, (void *) (uintptr_t) A.attempt);
    }
    return 1;
}

// loginEmailCode (v10 _39177, "Send Email Code"): POST /auth/login/email-code {"email"}; krunker.io
// answers enter_code and mails the code (the code step), or refuses.
int krunker_account_email_code(const char *email) {
    load();
    if (A.state == KRUNKER_ACCOUNT_BUSY) {
        set_message(1, "%s", I18N("kn.kio.account.busy", "A sign-in is already running."));
        return 0;
    }
    if (!email || !email[0] || strlen(email) >= sizeof(A.login)) {
        set_message(1, "%s", I18N("generic.auth.fillAllFields", "Please fill in all fields"));
        return 0;
    }
    req_cancel(&A.auth);
    end_attempt();
    snprintf(A.login, sizeof(A.login), "%s", email);
    A.email = 1;
    char url[512], mail[600], body[700];
    escape_json(mail, sizeof(mail), email);
    api_url(url, sizeof(url), "/auth/login/email-code");
    snprintf(body, sizeof(body), "{\"email\":\"%s\"}", mail);
    A.state = KRUNKER_ACCOUNT_BUSY;
    set_message(0, "%s", I18N("generic.wait", "Please wait..."));
    printf("[kio-account] signing in to krunker.io (email code)\n");
    if (!req_start(&A.auth, REQ_LOGIN, "POST", url, NULL, body)) {
        fail_sign_in(I18N("kn.kio.account.unreachable", "Could not reach krunker.io's account service. Check your connection, or the proxy in Settings > Client."));
        return 0;
    }
    changed();
    return 1;
}

// recoverAccount (v10 _39188): POST /auth/recover-account {"email"}; krunker.io mails the player
// a recovery link. The outcome is krunker_account_message().
int krunker_account_recover(const char *email) {
    load();
    if (!email || !email[0] || strlen(email) >= 256) {
        set_message(1, "%s", I18N("generic.auth.fillAllFields", "Please fill in all fields"));
        return 0;
    }
    char url[512], mail[600], body[700];
    escape_json(mail, sizeof(mail), email);
    api_url(url, sizeof(url), "/auth/recover-account");
    snprintf(body, sizeof(body), "{\"email\":\"%s\"}", mail);
    set_message(0, "%s", I18N("generic.wait", "Please wait..."));
    if (!req_start(&A.recover_req, REQ_RECOVER, "POST", url, NULL, body)) {
        set_message(1, "%s", I18N("generic.error", "Error"));
        return 0;
    }
    return 1;
}

int krunker_account_submit_code(const char *code) {
    if (A.state != KRUNKER_ACCOUNT_CODE || !A.challenge[0]) return 0;
    char digits[16];
    size_t n = 0;
    for (const char *p = code ? code : ""; *p && n + 1 < sizeof(digits); p++) {
        if (*p >= '0' && *p <= '9') digits[n++] = *p;
    }
    digits[n] = 0;
    if (n < 4) {
        set_message(1, "%s", I18N("kn.kio.account.badCode", "Enter the code (digits only)."));
        return 0;
    }
    char url[512], path[200], body[64];
    snprintf(path, sizeof(path), "%s%s", !strcmp(A.code_kind, "2fa") ? "/auth/2fa/challenge/" : "/auth/code-challenge/", A.challenge);
    api_url(url, sizeof(url), path);
    snprintf(body, sizeof(body), "{\"code\":\"%s\"}", digits);
    A.state = KRUNKER_ACCOUNT_BUSY;
    set_message(0, "%s", I18N("generic.wait", "Please wait..."));
    const int ok = req_start(&A.auth, REQ_CODE, "POST", url, NULL, body);
    wipe(body, sizeof(body));
    wipe(digits, sizeof(digits));
    if (!ok) {
        fail_sign_in(I18N("kn.kio.account.unreachable", "Could not reach krunker.io's account service. Check your connection, or the proxy in Settings > Client."));
        return 0;
    }
    return 1;
}

void krunker_account_cancel(void) {
    if (A.state != KRUNKER_ACCOUNT_BUSY && A.state != KRUNKER_ACCOUNT_CODE) return;
    if (A.auth.kind == REQ_LOGIN || A.auth.kind == REQ_CODE) req_cancel(&A.auth);
    end_attempt();
    A.state = A.access[0] || A.refresh[0] ? KRUNKER_ACCOUNT_SIGNED_IN : KRUNKER_ACCOUNT_GUEST;
    A.message[0] = 0;
    changed();
}

void krunker_account_sign_out(void) {
    load();
    krunker_account_cancel();
    req_cancel(&A.auth);
    req_cancel(&A.refresh_req);
    const int was = A.access[0] || A.refresh[0];
    forget();
    serve_waiters(NULL);
    if (was) printf("[kio-account] signed out of krunker.io\n");
    set_message(0, "%s", I18N("kn.kio.account.signedOut", "Signed out of krunker.io: krunker.io servers are joined as a guest."));
}

// ---------------------------------------------------------------- the windows' API requests

static void api_with_token(void *user, const char *token) {
    const unsigned id = (unsigned) (uintptr_t) user;
    ApiReq *q = NULL;
    for (int i = 0; i < API_SLOTS && !q; i++)
        if (g_api[i].used && g_api[i].id == id) q = &g_api[i];
    if (!q) return; // cancelled meanwhile (signed out)
    if (!req_start(&q->r, REQ_API, q->method, q->url, token, q->body)) {
        const KrunkerApiFn fn = q->fn;
        void *u = q->user;
        free(q->body);
        q->body = NULL;
        q->used = 0;
        if (fn) fn(u, 0, NULL);
    }
}

int krunker_account_api(const char *method, const KrunkerService svc, const char *path, const char *body, const KrunkerApiFn fn, void *user) {
    load();
    ApiReq *q = NULL;
    for (int i = 0; i < API_SLOTS && !q; i++)
        if (!g_api[i].used) q = &g_api[i];
    if (!q || !method || !path) return 0;
    memset(q, 0, sizeof(*q));
    q->used = 1;
    q->id = ++g_api_ids;
    snprintf(q->method, sizeof(q->method), "%s", method);
    krunker_service_url(svc, path, q->url, sizeof(q->url));
    q->body = body ? strdup(body) : NULL;
    q->fn = fn;
    q->user = user;
    // the API client's interceptor: Authorization: Bearer <FRVR's fresh access token>, refreshed
    // first when it is about to expire; a guest's request goes without (public endpoints)
    krunker_account_fresh_token(api_with_token, (void *) (uintptr_t) q->id);
    return 1;
}

// ---------------------------------------------------------------- the join's token

// <auth>/refresh with the stored refresh token (FRVR's AUTH_REFRESH); 0 = not started
static int start_refresh(void) {
    char url[512], token[TOKEN_MAX + 8], body[TOKEN_MAX + 64];
    auth_url(url, sizeof(url), "/refresh");
    escape_json(token, sizeof(token), A.refresh);
    snprintf(body, sizeof(body), "{\"refreshToken\":\"%s\"}", token);
    const int ok = req_start(&A.refresh_req, REQ_REFRESH, "POST", url, NULL, body);
    wipe(token, sizeof(token));
    wipe(body, sizeof(body));
    return ok;
}

int krunker_account_resume(const char *name, const char *refresh) {
    load();
    if (A.state == KRUNKER_ACCOUNT_BUSY) {
        set_message(1, "%s", I18N("kn.kio.account.busy", "A sign-in is already running."));
        return 0;
    }
    if (!name || !name[0] || strlen(name) >= sizeof(A.name) || !refresh || !refresh[0] || strlen(refresh) > TOKEN_MAX) return 0;
    req_cancel(&A.auth);
    if (A.refresh_req.kind == REQ_REFRESH) {
        req_cancel(&A.refresh_req);
        serve_waiters(NULL);
    }
    end_attempt();
    wipe(A.access, sizeof(A.access));
    snprintf(A.refresh, sizeof(A.refresh), "%s", refresh);
    snprintf(A.name, sizeof(A.name), "%s", name);
    A.level = 0;
    A.rp_known = 0;
    A.rp = 0;
    A.resuming = 1;
    A.state = KRUNKER_ACCOUNT_BUSY;
    printf("[kio-account] resuming the krunker.io account %s\n", A.name);
    changed();
    if (!start_refresh()) {
        A.resuming = 0;
        forget();
        emit(KRUNKER_EV_RESUME_FAILED);
    }
    return 1;
}

int krunker_account_refresh_token(char *out, const size_t n) {
    load();
    if (!out || !n) return 0;
    out[0] = 0;
    if (!A.refresh[0]) return 0;
    snprintf(out, n, "%s", A.refresh);
    return 1;
}

void krunker_account_fresh_token(const KrunkerAccountTokenFn fn, void *user) {
    load();
    if (!A.access[0] && !A.refresh[0]) {
        if (fn) fn(user, NULL);
        return;
    }
    const double left = access_left();
    if (left > KRUNKER_ACCOUNT_REFRESH_MARGIN_S || !A.refresh[0]) {
        if (fn) fn(user, left > 0 ? A.access : NULL);
        return;
    }
    if (A.waiter_count >= TOKEN_WAITERS) {
        if (fn) fn(user, left > 5 ? A.access : NULL);
        return;
    }
    A.waiters[A.waiter_count].fn = fn;
    A.waiters[A.waiter_count].user = user;
    A.waiter_count++;
    if (A.refresh_req.kind == REQ_REFRESH) return; // one refresh serves every waiter
    if (!start_refresh()) serve_waiters(left > 5 ? A.access : NULL);
}

// ---------------------------------------------------------------- state

void krunker_account_note_game(const char *name, const int level) {
    load();
    if (!A.access[0] && !A.refresh[0]) return;
    int moved = 0;
    if (name && name[0] && strcmp(name, A.name)) {
        snprintf(A.name, sizeof(A.name), "%s", name);
        moved = 1;
    }
    if (level > 0 && level != A.level) {
        A.level = level;
        moved = 1;
    }
    if (moved) {
        save();
        changed();
        printf("[kio-account] the krunker.io game server names the account %s (level %d)\n", A.name, A.level);
    }
}

void krunker_account_note_points(const double points) {
    load();
    if (!krunker_account_signed_in()) return;
    if (A.rp_known && A.rp == points) return;
    A.rp_known = 1;
    A.rp = points;
    changed();
}

// v10 changes its balance itself after a ranked claim (rankedPoints += reward, v10:98744) and a purchase
// (rankedPoints -= price, v10:111042); the same change to the held balance. Nothing when it is not known.
void krunker_account_add_points(const double delta) {
    load();
    if (!krunker_account_signed_in() || !A.rp_known) return;
    A.rp += delta;
    changed();
}

int krunker_account_ranked_points(double *out) {
    load();
    if (!out || !krunker_account_signed_in() || !A.rp_known) return 0;
    *out = A.rp;
    return 1;
}

void krunker_account_tick(void) {
    load();
    http_stream_poll();
    if (A.captcha_open && !krunker_captcha_asking()) {
        // a join's challenge took the modal over (krunker_captcha_ask replaces a request), or it closed
        A.captcha_open = 0;
        if (A.state == KRUNKER_ACCOUNT_BUSY && A.auth.kind == REQ_NONE)
            fail_sign_in(I18N("kn.kio.account.failed", "krunker.io did not accept the sign-in."));
    }
    const double now = now_s();
    Req *reqs[] = {&A.auth, &A.refresh_req, &A.recover_req};
    for (size_t i = 0; i < sizeof(reqs) / sizeof(reqs[0]); i++) {
        Req *r = reqs[i];
        if (!r->stream || now - r->started < REQUEST_TIMEOUT_S) continue;
        const ReqKind kind = r->kind;
        req_cancel(r);
        printf("[kio-account] request timed out\n");
        if (kind == REQ_LOGIN || kind == REQ_CODE)
            fail_sign_in(I18N("kn.kio.account.unreachable", "Could not reach krunker.io's account service. Check your connection, or the proxy in Settings > Client."));
        else if (kind == REQ_REFRESH) serve_waiters(access_left() > 5 ? A.access : NULL);
        else if (kind == REQ_RECOVER) set_message(1, "%s", I18N("generic.error", "Error"));
    }
    for (int i = 0; i < API_SLOTS; i++) {
        ApiReq *q = &g_api[i];
        if (!q->used || !q->r.stream || now - q->r.started < REQUEST_TIMEOUT_S) continue;
        req_cancel(&q->r);
        printf("[kio-account] API request timed out\n");
        const KrunkerApiFn fn = q->fn;
        void *u = q->user;
        free(q->body);
        q->body = NULL;
        q->used = 0;
        if (fn) fn(u, 0, NULL);
    }
}

KrunkerAccountState krunker_account_state(void) {
    load();
    return A.state;
}

int krunker_account_signed_in(void) {
    load();
    return A.access[0] || A.refresh[0];
}

const char *krunker_account_name(void) {
    load();
    return krunker_account_signed_in() ? A.name : "";
}

int krunker_account_level(void) {
    load();
    return krunker_account_signed_in() ? A.level : 0;
}

double krunker_account_score(void) { return krunker_account_signed_in() ? A.score : 0; }
double krunker_account_funds(void) { return krunker_account_signed_in() ? A.funds : 0; }

const char *krunker_account_message(void) { return A.message; }
int krunker_account_message_is_error(void) { return A.message_error; }
const char *krunker_account_code_kind(void) { return A.code_kind; }

unsigned krunker_account_generation(void) {
    load();
    return A.generation;
}

int krunker_account_owns_captcha(void) { return A.captcha_open; }
