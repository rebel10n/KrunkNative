// Web Push state of the web build's Notifications switch (web_push.h). The browser side is html_dom.h's
// html_push_subscribe / html_push_unsubscribe (web_dom.c, ui/sw.js); the brain side is the three
// /api/push routes (services/brain/push.c). Every answer carries the generation it was asked in: a later
// set / refresh makes it stale, and a stale answer changes nothing (except removing a subscription that
// the switch no longer wants).
#include <web_push.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <backend.h>
#include <backend_ui.h> // active_account(), account_session_token()
#include <cJSON.h>
#include <html_dom.h>

#define WEB_PUSH_KEY "knWebPush"                 // "1" when the switch is on (a per-browser convenience)
#define WEB_PUSH_VAPID_PATH "/api/push/vapid-key"
#define WEB_PUSH_SUBSCRIBE_PATH "/api/push/subscribe"
#define WEB_PUSH_UNSUBSCRIBE_PATH "/api/push/unsubscribe"

enum { WP_OK, WP_NO_SERVER, WP_FAILED }; // the last answer that failed (WP_NO_SERVER: vapid-key 404)

static struct {
    int loaded;
    int on;
    int failure;
    unsigned gen;
    WebPushChangedFn changed;
} W;

typedef struct {
    unsigned gen;
} PushOp;

static void changed(void) {
    if (W.changed) W.changed();
}

static void load(void) {
    if (W.loaded) return;
    W.loaded = 1;
    char buf[8] = {0};
    W.on = html_storage_get(WEB_PUSH_KEY, buf, sizeof(buf)) == 1 && buf[0] == '1';
}

static void store(const int on) {
    W.on = on;
    html_storage_set(WEB_PUSH_KEY, on ? "1" : "0");
}

// signed in to a Box Office account: the brain records subscriptions only for an account (a guest has none)
static int signed_in(void) {
    return backend_enabled() && active_account() && account_session_token()[0];
}

// POST backend_url() + path with body (owned, freed here). 0 when it could not be sent.
static int post(const char *path, cJSON *body, const BackendHttpFn fn) {
    char *json = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!json) return 0;
    const size_t n = strlen(backend_url()) + strlen(path) + 1;
    char *url = malloc(n);
    int sent = 0;
    if (url) {
        snprintf(url, n, "%s%s", backend_url(), path);
        sent = backend_http_request("POST", url, json, fn, NULL);
    }
    free(url);
    free(json);
    return sent;
}

static void on_unsubscribe_posted(void *user, int status, const char *body, size_t length) {
    (void) user;
    (void) body;
    (void) length;
    (void) status; // the browser has let go already; a brain row that stays is removed on the next off
}

static void on_subscribe_posted(void *user, int status, const char *body, size_t length) {
    (void) user;
    (void) body;
    (void) length;
    if (status < 200 || status >= 300) {
        W.failure = WP_FAILED;
        changed();
    }
}

// the subscription JSON (from the browser) as the brain's body: {token, endpoint, keys: {p256dh, auth}}
static cJSON *subscribe_body(const char *json, const char *token) {
    cJSON *sub = cJSON_Parse(json);
    const cJSON *endpoint = sub ? cJSON_GetObjectItemCaseSensitive(sub, "endpoint") : NULL;
    const cJSON *keys = sub ? cJSON_GetObjectItemCaseSensitive(sub, "keys") : NULL;
    const cJSON *p256dh = keys ? cJSON_GetObjectItemCaseSensitive(keys, "p256dh") : NULL;
    const cJSON *auth = keys ? cJSON_GetObjectItemCaseSensitive(keys, "auth") : NULL;
    if (!cJSON_IsString(endpoint) || !cJSON_IsString(p256dh) || !cJSON_IsString(auth)) {
        cJSON_Delete(sub);
        return NULL;
    }
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "token", token);
    cJSON_AddStringToObject(body, "endpoint", endpoint->valuestring);
    cJSON *k = cJSON_AddObjectToObject(body, "keys");
    cJSON_AddStringToObject(k, "p256dh", p256dh->valuestring);
    cJSON_AddStringToObject(k, "auth", auth->valuestring);
    cJSON_Delete(sub);
    return body;
}

// the browser removed its subscription (json, NULL: there was none): the brain forgets that endpoint too
static void on_unsubscribed(void *user, const char *json) {
    (void) user;
    if (!json || !signed_in()) return;
    cJSON *sub = cJSON_Parse(json);
    const cJSON *endpoint = sub ? cJSON_GetObjectItemCaseSensitive(sub, "endpoint") : NULL;
    if (cJSON_IsString(endpoint)) {
        cJSON *body = cJSON_CreateObject();
        cJSON_AddStringToObject(body, "token", account_session_token());
        cJSON_AddStringToObject(body, "endpoint", endpoint->valuestring);
        post(WEB_PUSH_UNSUBSCRIBE_PATH, body, on_unsubscribe_posted);
    }
    cJSON_Delete(sub);
}

// the browser's subscription (json, NULL: refused, unsupported or failed)
static void on_subscribed(void *user, const char *json) {
    PushOp *op = user;
    const int current = op->gen == W.gen;
    free(op);
    if (!current) {
        // turned off since: the browser's subscription must not outlive the switch
        if (json && !W.on) html_push_unsubscribe(on_unsubscribed, NULL);
        return;
    }
    if (!json) {
        W.failure = WP_FAILED;
        store(0);
        changed();
        return;
    }
    W.failure = WP_OK;
    if (signed_in()) {
        cJSON *body = subscribe_body(json, account_session_token());
        if (body && !post(WEB_PUSH_SUBSCRIBE_PATH, body, on_subscribe_posted)) {
            W.failure = WP_FAILED;
        }
    }
    changed();
}

// the brain's VAPID public key: the browser subscribes with it
static void on_vapid_key(void *user, int status, const char *body, size_t length) {
    PushOp *op = user;
    if (op->gen != W.gen) {
        free(op);
        return;
    }
    if (status == 404) { // no VAPID key on this server
        W.failure = WP_NO_SERVER;
        store(0);
        free(op);
        changed();
        return;
    }
    cJSON *o = status == 200 && body && length ? cJSON_ParseWithLength(body, length) : NULL;
    const cJSON *key = o ? cJSON_GetObjectItemCaseSensitive(o, "publicKey") : NULL;
    if (!cJSON_IsString(key) || !key->valuestring[0]) {
        cJSON_Delete(o);
        W.failure = WP_FAILED;
        store(0);
        free(op);
        changed();
        return;
    }
    html_push_subscribe(key->valuestring, on_subscribed, op); // exactly one answer: on_subscribed frees op
    cJSON_Delete(o);
}

// (a new generation) fetch the key, then subscribe; every older answer is stale from here on
static void subscribe_now(void) {
    PushOp *op = calloc(1, sizeof(*op));
    if (!op) return;
    op->gen = ++W.gen;
    if (!backend_http_get(WEB_PUSH_VAPID_PATH, on_vapid_key, op)) {
        free(op);
        W.failure = WP_FAILED;
        store(0);
        changed();
    }
}

int web_push_supported(void) { return html_push_supported(); }

int web_push_on(void) {
    load();
    return web_push_supported() && W.on;
}

void web_push_set(const int on) {
    if (!web_push_supported()) return;
    load();
    W.gen++; // whatever was in flight for the other state is stale now
    W.failure = WP_OK;
    if (on) {
        store(1);
        subscribe_now();
        changed();
    } else {
        store(0);
        html_push_unsubscribe(on_unsubscribed, NULL);
        changed();
    }
}

void web_push_refresh(void) {
    if (!web_push_on()) return;
    subscribe_now();
}

const char *web_push_status(void) {
    if (!web_push_supported()) return "Push notifications are not supported in your browser.";
    load();
    if (W.failure == WP_NO_SERVER) return "Push is not set up on this server";
    if (W.failure == WP_FAILED) return "Push notifications could not be turned on in this browser.";
    if (!W.on) return "Push notifications are off";
    if (!signed_in()) return "Sign in to get push notifications";
    return "Push notifications are on in this browser";
}

void web_push_set_listener(const WebPushChangedFn fn) { W.changed = fn; }
