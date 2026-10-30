// Web Push stores of the menu's Notifications panel (web_push.h). The browser side is html_dom.h's
// html_push_subscribe / html_push_unsubscribe / html_push_permission (web_dom.c, ui/sw.js); the brain side
// is the three /api/push routes (services/brain/push.c). The flows are v10's subscribe / unsubscribe
// (deob-10.0.5.js:148636-148707). Every answer carries the generation it was asked in: a later subscribe /
// unsubscribe makes it stale, and a stale answer changes nothing (except removing a browser subscription
// that is no longer wanted).
#include <web_push.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <backend.h>
#include <backend_ui.h> // active_account(), account_session_token()
#include <cJSON.h>
#include <html_dom.h>

#define WEB_PUSH_KEY "knWebPush"                 // "1" after a successful subscribe (per browser)
#define WEB_PUSH_VAPID_PATH "/api/push/vapid-key"
#define WEB_PUSH_SUBSCRIBE_PATH "/api/push/subscribe"
#define WEB_PUSH_UNSUBSCRIBE_PATH "/api/push/unsubscribe"
#define WEB_PUSH_SUBSCRIBE_FAILED "Failed to subscribe to notifications" // v10's subscribe catch (:148674)

static struct {
    int loaded;
    int subscribed;
    int loading;
    const char *error;
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
    W.subscribed = html_storage_get(WEB_PUSH_KEY, buf, sizeof(buf)) == 1 && buf[0] == '1';
}

static void set_subscribed(const int on) {
    W.subscribed = on;
    html_storage_set(WEB_PUSH_KEY, on ? "1" : "0");
}

// v10's finally: loading off, the error (NULL: none), the panel redrawn
static void finish(const char *error) {
    W.loading = 0;
    W.error = error;
    changed();
}

// signed in to a Box Office account: the brain records subscriptions only for an account (a guest has none)
static int signed_in(void) {
    return backend_enabled() && active_account() && account_session_token()[0];
}

static PushOp *op_new(void) {
    PushOp *op = calloc(1, sizeof(*op));
    if (op) op->gen = W.gen;
    return op;
}

// POST backend_url() + path with body (owned, freed here). 0 when it could not be sent.
static int post(const char *path, cJSON *body, const BackendHttpFn fn, void *user) {
    char *json = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!json) return 0;
    const size_t n = strlen(backend_url()) + strlen(path) + 1;
    char *url = malloc(n);
    int sent = 0;
    if (url) {
        snprintf(url, n, "%s%s", backend_url(), path);
        sent = backend_http_request("POST", url, json, fn, user);
    }
    free(url);
    free(json);
    return sent;
}

static void on_brain_unsubscribed(void *user, int status, const char *body, size_t length) {
    (void) user;
    (void) status; // the browser has let go already; a row that stays is replaced by the next subscribe
    (void) body;
    (void) length;
}

// the browser removed its subscription (json, NULL: there was none): the brain forgets that endpoint too
static void forget(const char *json) {
    if (!json || !signed_in()) return;
    cJSON *sub = cJSON_Parse(json);
    const cJSON *endpoint = sub ? cJSON_GetObjectItemCaseSensitive(sub, "endpoint") : NULL;
    if (cJSON_IsString(endpoint)) {
        cJSON *body = cJSON_CreateObject();
        cJSON_AddStringToObject(body, "token", account_session_token());
        cJSON_AddStringToObject(body, "endpoint", endpoint->valuestring);
        post(WEB_PUSH_UNSUBSCRIBE_PATH, body, on_brain_unsubscribed, NULL);
    }
    cJSON_Delete(sub);
}

static void on_dropped(void *user, const char *json) {
    (void) user;
    (void) json; // a subscription the brain did not take: nothing to forget there
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

// the brain's answer to the subscription: success subscribes; otherwise v10 lets the browser's subscription go
// and ends without an error (:148665-148671)
static void on_brain_subscribed(void *user, int status, const char *body, size_t length) {
    PushOp *op = user;
    (void) body;
    (void) length;
    const int current = op->gen == W.gen;
    free(op);
    if (!current) return;
    if (status >= 200 && status < 300) {
        set_subscribed(1);
    } else {
        html_push_unsubscribe(on_dropped, NULL);
    }
    finish(NULL);
}

// the browser's subscription (json, NULL: permission not granted, unsupported or failed)
static void on_subscribed(void *user, const char *json) {
    PushOp *op = user;
    if (op->gen != W.gen) {
        free(op);
        if (json && !W.subscribed) html_push_unsubscribe(on_dropped, NULL); // unwanted since
        return;
    }
    if (!json) {
        free(op);
        // v10: a permission that is not granted ends the subscribe without an error (:148643-148649); any other
        // failure is the catch's
        finish(html_push_permission() == 1 ? WEB_PUSH_SUBSCRIBE_FAILED : NULL);
        return;
    }
    cJSON *body = signed_in() ? subscribe_body(json, account_session_token()) : NULL;
    if (!body || !post(WEB_PUSH_SUBSCRIBE_PATH, body, on_brain_subscribed, op)) {
        // a guest (the brain records none) or a request that could not be sent: v10's refused subscription
        free(op);
        html_push_unsubscribe(on_dropped, NULL);
        finish(NULL);
    }
}

// the brain's VAPID public key: the browser subscribes with it (v10 has it in its config)
static void on_vapid_key(void *user, int status, const char *body, size_t length) {
    PushOp *op = user;
    if (op->gen != W.gen) {
        free(op);
        return;
    }
    cJSON *o = status == 200 && body && length ? cJSON_ParseWithLength(body, length) : NULL;
    const cJSON *key = o ? cJSON_GetObjectItemCaseSensitive(o, "publicKey") : NULL;
    if (!cJSON_IsString(key) || !key->valuestring[0]) { // no key (404: none on this server): subscribing throws
        cJSON_Delete(o);
        free(op);
        finish(WEB_PUSH_SUBSCRIBE_FAILED);
        return;
    }
    html_push_subscribe(key->valuestring, on_subscribed, op); // exactly one answer: on_subscribed owns op
    cJSON_Delete(o);
}

int web_push_supported(void) { return html_push_supported(); }

int web_push_denied(void) { return web_push_supported() && html_push_permission() < 0; }

int web_push_subscribed(void) {
    load();
    return web_push_supported() && W.subscribed;
}

int web_push_loading(void) { return W.loading; }

const char *web_push_error(void) { return W.error; }

void web_push_subscribe(void) {
    if (!web_push_supported() || W.loading) return;
    load();
    W.gen++; // whatever was in flight is stale now
    W.loading = 1;
    W.error = NULL;
    changed();
    PushOp *op = op_new();
    if (!op || !backend_http_get(WEB_PUSH_VAPID_PATH, on_vapid_key, op)) {
        free(op);
        finish(WEB_PUSH_SUBSCRIBE_FAILED);
    }
}

static void on_unsubscribed(void *user, const char *json) {
    PushOp *op = user;
    const int current = op->gen == W.gen;
    free(op);
    forget(json);
    if (!current) return;
    set_subscribed(0);
    finish(NULL);
}

void web_push_unsubscribe(void) {
    if (!web_push_supported() || W.loading) return;
    load();
    W.gen++;
    W.loading = 1;
    W.error = NULL;
    changed();
    PushOp *op = op_new();
    if (!op) {
        finish("Failed to unsubscribe from notifications"); // v10's unsubscribe catch (:148701)
        return;
    }
    html_push_unsubscribe(on_unsubscribed, op);
}

void web_push_set_listener(const WebPushChangedFn fn) { W.changed = fn; }
