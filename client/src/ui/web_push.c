// Web Push stores of the menu's Notifications panel (web_push.h). The browser side is html_dom.h's
// html_push_subscribe / html_push_unsubscribe / html_push_current / html_push_listen / html_push_permission
// (web_dom.c, ui/sw.js). Box Office: the three /api/push routes (services/brain/push.c), with v10's subscribe /
// unsubscribe flows (deob-10.0.5.js:148636-148707). krunker.io (the active backend is krunker.io): v10.0.7's own
// webpush client on gapi with FRVR's Bearer (krunker_account_api: the account's, else the anonymous session's) and
// krunker.io's VAPID key, below. Every answer carries the generation it was asked in: a later subscribe /
// unsubscribe makes it stale, and a stale answer changes nothing (except removing a browser subscription
// that is no longer wanted).
#include <web_push.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <backend.h>
#include <backend_ui.h> // active_account(), account_session_token(), account_backend()
#include <cJSON.h>
#include <html_dom.h>
#include <krunker_account.h>    // krunker_account_api: gapi with FRVR's Bearer
#include <krunker_kio_routes.h> // krunker_kio_gapi_write: the webpush writes v10.0.7 sends

#define WEB_PUSH_KEY "knWebPush"                 // "1" after a successful subscribe (per browser)
#define WEB_PUSH_VAPID_PATH "/api/push/vapid-key"
#define WEB_PUSH_SUBSCRIBE_PATH "/api/push/subscribe"
#define WEB_PUSH_UNSUBSCRIBE_PATH "/api/push/unsubscribe"
#define WEB_PUSH_SUBSCRIBE_FAILED "Failed to subscribe to notifications" // v10's subscribe catch (:148674)
#define WEB_PUSH_UNSUBSCRIBE_FAILED "Failed to unsubscribe from notifications" // v10's unsubscribe catch (:148701)
// krunker.io (deob-10.0.7.js): the page's VAPID public key (:19502), the store's other errors (:148940, :148988)
#define KIO_VAPID "BIQ4JkKF4R0khgycd7N447m-Uxk39zZBeHlW7-WzGeFNc3OhaZgh7k3_PiyHY5L_OTOoU7SU7KBVooddC8bNdcc"
#define KIO_CHANGE_FAILED "Failed to update subscription. Please re-subscribe."
#define KIO_LOST "Push subscription was lost. Please re-subscribe to continue receiving notifications."

static struct {
    int loaded;
    int subscribed;
    int loading;
    const char *error;
    unsigned gen;
    WebPushChangedFn changed;
    char vapid[128]; // the brain's VAPID key, once fetched (the worker's WEBPUSH_REQUEST_VAPID_KEY answer)
    int mounted;     // the backend the panel's state was set up for (AccountBackend + 1; 0 = none yet)
    // krunker.io: v10.0.7's store {subscribed, registrationId} (memory only: the mount's status check sets it)
    int kio_sub;
    char kio_reg[128];
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

static int kio(void) { return account_backend() == ACCOUNT_BACKEND_KRUNKER; }

// ---------------------------------------------------------------- krunker.io (deob-10.0.7.js)
//
// The webpush client (:26455-26483) on gapi: POST /webpush/subscribe {subscription: {endpoint, keys: {p256dh, auth}}},
// /webpush/unsubscribe {registration_id, endpoint}, /webpush/status {endpoint}, /webpush/subscription-change
// {old_subscription, new_subscription}. The flows (:148910-148990): subscribe asks the permission, subscribes the
// browser with KIO_VAPID and sends the keys as btoa of their bytes (iîìiíìì: standard base64); data.success keeps it
// with data.registration_id, anything else lets the browser's go with no error; a throw is the catch's error.
// Unsubscribe lets the browser's go, then tells gapi. The mount (:148982-149027) asks /webpush/status for the
// browser's subscription (is_subscribed keeps it, else it is let go) and takes the worker's messages and queued
// changes (html_push_listen). The API client throws on no answer, a status outside [200, 500) or a body that is not
// JSON (204 / 304 / 429 are answers without one, :26152-26190).

static int kio_threw(const int status, const cJSON *res) {
    if (status < 200 || status >= 500) return 1;
    if (status == 204 || status == 304 || status == 429) return 0;
    return res == NULL;
}

static int truthy(const cJSON *v) {
    if (!v || cJSON_IsNull(v) || cJSON_IsFalse(v)) return 0;
    if (cJSON_IsNumber(v)) return v->valuedouble != 0;
    if (cJSON_IsString(v)) return v->valuestring[0] != 0;
    return 1;
}

static const cJSON *kio_data(const cJSON *res, const char *key) {
    return cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(res, "data"), key);
}

// POST gapi path with body (owned, freed here), only a write krunker_kio_gapi_write allows. 0 = not sent.
static int kio_post(const char *path, cJSON *body, const KrunkerApiFn fn, void *user) {
    char *json = body ? cJSON_PrintUnformatted(body) : NULL;
    cJSON_Delete(body);
    const int sent = json && krunker_kio_gapi_write("POST", path) && krunker_account_api("POST", KRUNKER_SVC_API, path, json, fn, user);
    free(json);
    return sent;
}

// a PushSubscription key (base64url, the subscription's JSON) as btoa of its bytes: standard base64, padded
static void std_b64(const char *in, char *out, const size_t n) {
    size_t o = 0;
    for (; *in && o + 1 < n; in++) out[o++] = *in == '-' ? '+' : *in == '_' ? '/' : *in;
    while (o % 4 && o + 1 < n) out[o++] = '=';
    out[o] = 0;
}

static cJSON *kio_subscribe_body(const char *json) {
    cJSON *sub = cJSON_Parse(json);
    const cJSON *endpoint = cJSON_GetObjectItemCaseSensitive(sub, "endpoint");
    const cJSON *keys = cJSON_GetObjectItemCaseSensitive(sub, "keys");
    const char *p256dh = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(keys, "p256dh"));
    const char *auth = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(keys, "auth"));
    if (!cJSON_IsString(endpoint) || !p256dh || !auth || strlen(p256dh) > 400 || strlen(auth) > 400) {
        cJSON_Delete(sub);
        return NULL;
    }
    char p[404], a[404];
    std_b64(p256dh, p, sizeof(p));
    std_b64(auth, a, sizeof(a));
    cJSON *body = cJSON_CreateObject();
    cJSON *s = cJSON_AddObjectToObject(body, "subscription");
    cJSON_AddStringToObject(s, "endpoint", endpoint->valuestring);
    cJSON *k = cJSON_AddObjectToObject(s, "keys");
    cJSON_AddStringToObject(k, "p256dh", p);
    cJSON_AddStringToObject(k, "auth", a);
    cJSON_Delete(sub);
    return body;
}

// iïïiíiî(subscription, registrationId): the store's subscription and registration id (NULL: none)
static void kio_keep(const int subscribed, const cJSON *registration_id) {
    W.kio_sub = subscribed;
    W.kio_reg[0] = 0;
    if (!subscribed) return;
    if (cJSON_IsString(registration_id)) snprintf(W.kio_reg, sizeof(W.kio_reg), "%s", registration_id->valuestring);
    else if (cJSON_IsNumber(registration_id)) snprintf(W.kio_reg, sizeof(W.kio_reg), "%.0f", registration_id->valuedouble);
}

static void on_kio_subscribe_answer(void *user, const int status, const cJSON *res) {
    PushOp *op = user;
    const int current = op->gen == W.gen;
    free(op);
    if (!current) return;
    if (kio_threw(status, res)) { // the catch: the error, the browser's subscription stays
        finish(WEB_PUSH_SUBSCRIBE_FAILED);
        return;
    }
    if (truthy(kio_data(res, "success"))) {
        kio_keep(1, kio_data(res, "registration_id"));
    } else {
        html_push_unsubscribe(on_dropped, NULL); // {success: false, error}: returned, not shown
    }
    finish(NULL);
}

static void on_kio_subscribed(void *user, const char *json) {
    PushOp *op = user;
    if (op->gen != W.gen) {
        free(op);
        if (json && !W.kio_sub) html_push_unsubscribe(on_dropped, NULL); // unwanted since
        return;
    }
    if (!json) { // "Permission denied for notifications" is returned, not shown; a failed subscribe is the catch's
        free(op);
        finish(html_push_permission() == 1 ? WEB_PUSH_SUBSCRIBE_FAILED : NULL);
        return;
    }
    if (!kio_post("/webpush/subscribe", kio_subscribe_body(json), on_kio_subscribe_answer, op)) {
        free(op);
        finish(WEB_PUSH_SUBSCRIBE_FAILED);
    }
}

static void on_kio_unsubscribe_answer(void *user, const int status, const cJSON *res) {
    PushOp *op = user;
    const int current = op->gen == W.gen;
    free(op);
    if (!current) return;
    if (kio_threw(status, res)) {
        finish(WEB_PUSH_UNSUBSCRIBE_FAILED);
        return;
    }
    kio_keep(0, NULL);
    finish(NULL);
}

// the browser let its subscription go (json: the one it removed): gapi forgets it, by registration_id and endpoint
static void on_kio_unsubscribed(void *user, const char *json) {
    PushOp *op = user;
    if (op->gen != W.gen) {
        free(op);
        return;
    }
    cJSON *sub = json ? cJSON_Parse(json) : NULL;
    const cJSON *endpoint = cJSON_GetObjectItemCaseSensitive(sub, "endpoint");
    if (!cJSON_IsString(endpoint)) { // no subscription: nothing to tell (the store follows the browser, as v10's next check does)
        cJSON_Delete(sub);
        free(op);
        kio_keep(0, NULL);
        finish(NULL);
        return;
    }
    cJSON *body = cJSON_CreateObject();
    if (W.kio_reg[0]) cJSON_AddStringToObject(body, "registration_id", W.kio_reg);
    else cJSON_AddNullToObject(body, "registration_id");
    cJSON_AddStringToObject(body, "endpoint", endpoint->valuestring);
    cJSON_Delete(sub);
    if (!kio_post("/webpush/unsubscribe", body, on_kio_unsubscribe_answer, op)) {
        free(op);
        finish(WEB_PUSH_UNSUBSCRIBE_FAILED);
    }
}

// the mount's check: is_subscribed keeps the browser's subscription (no registration id), anything else lets it go
static void on_kio_status(void *user, const int status, const cJSON *res) {
    PushOp *op = user;
    const int current = op->gen == W.gen && kio();
    free(op);
    if (!current || kio_threw(status, res)) return;
    if (truthy(kio_data(res, "is_subscribed"))) {
        kio_keep(1, NULL);
    } else {
        html_push_unsubscribe(on_dropped, NULL);
        kio_keep(0, NULL);
    }
    changed();
}

static void on_kio_current(void *user, const char *json) {
    PushOp *op = user;
    cJSON *sub = json && op->gen == W.gen && kio() ? cJSON_Parse(json) : NULL;
    const cJSON *endpoint = cJSON_GetObjectItemCaseSensitive(sub, "endpoint");
    cJSON *body = NULL;
    if (cJSON_IsString(endpoint)) {
        body = cJSON_CreateObject();
        cJSON_AddStringToObject(body, "endpoint", endpoint->valuestring);
    }
    cJSON_Delete(sub);
    if (!body || !kio_post("/webpush/status", body, on_kio_status, op)) free(op);
}

typedef struct {
    unsigned gen;
    char reg[128];
} KioChange;

static void on_kio_change_current(void *user, const char *json) {
    KioChange *c = user;
    if (c->gen == W.gen && kio() && json) {
        cJSON *id = cJSON_CreateString(c->reg);
        kio_keep(1, c->reg[0] ? id : NULL);
        cJSON_Delete(id);
        changed();
    }
    free(c);
}

// iïiïìïï: data.success: the browser's subscription is kept with the new registration id; else the error
static void on_kio_change_answer(void *user, const int status, const cJSON *res) {
    KioChange *c = user;
    if (c->gen != W.gen || !kio()) {
        free(c);
        return;
    }
    if (kio_threw(status, res) || !truthy(kio_data(res, "success"))) {
        free(c);
        W.error = KIO_CHANGE_FAILED;
        changed();
        return;
    }
    const cJSON *id = kio_data(res, "registration_id");
    if (cJSON_IsString(id)) snprintf(c->reg, sizeof(c->reg), "%s", id->valuestring);
    else if (cJSON_IsNumber(id)) snprintf(c->reg, sizeof(c->reg), "%.0f", id->valuedouble);
    html_push_current(on_kio_change_current, c);
}

// the service worker's messages and queued changes (html_push_listen)
static void on_push_event(void *user, const char *type, const char *json) {
    (void) user;
    if (!strcmp(type, "lost")) { // WEBPUSH_SUBSCRIPTION_LOST: no subscription, and v10.0.7's line
        if (kio()) kio_keep(0, NULL);
        else set_subscribed(0);
        W.error = KIO_LOST;
        changed();
        return;
    }
    if (strcmp(type, "changed") || !kio()) return; // (the brain has no route for a changed subscription)
    cJSON *in = json ? cJSON_Parse(json) : NULL;
    cJSON *body = cJSON_CreateObject();
    const cJSON *old = cJSON_GetObjectItemCaseSensitive(in, "old_subscription"), *neu = cJSON_GetObjectItemCaseSensitive(in, "new_subscription");
    cJSON_AddItemToObject(body, "old_subscription", old ? cJSON_Duplicate(old, 1) : cJSON_CreateNull());
    cJSON_AddItemToObject(body, "new_subscription", neu ? cJSON_Duplicate(neu, 1) : cJSON_CreateNull());
    cJSON_Delete(in);
    KioChange *c = calloc(1, sizeof(*c));
    if (c) c->gen = W.gen;
    if (!c || !kio_post("/webpush/subscription-change", body, on_kio_change_answer, c)) {
        free(c);
        W.error = KIO_CHANGE_FAILED;
        changed();
    }
}

// The panel's mount for the active backend (again when it changes): the worker's messages, and on krunker.io the
// status of the browser's subscription
static void mount(void) {
    if (!html_push_supported()) return;
    const int backend = (int) account_backend() + 1;
    if (backend == W.mounted) return;
    W.mounted = backend;
    html_push_listen(kio() ? KIO_VAPID : W.vapid, on_push_event, NULL);
    if (!kio()) return;
    kio_keep(0, NULL);
    PushOp *op = op_new();
    if (op) html_push_current(on_kio_current, op);
}

// ---------------------------------------------------------------- Box Office

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
    snprintf(W.vapid, sizeof(W.vapid), "%s", key->valuestring);
    if (W.mounted == (int) ACCOUNT_BACKEND_KRUNKIO + 1) html_push_listen(W.vapid, on_push_event, NULL); // the worker's key now
    html_push_subscribe(key->valuestring, on_subscribed, op); // exactly one answer: on_subscribed owns op
    cJSON_Delete(o);
}

int web_push_supported(void) { return html_push_supported(); }

int web_push_denied(void) { return web_push_supported() && html_push_permission() < 0; }

int web_push_subscribed(void) {
    load();
    mount();
    return web_push_supported() && (kio() ? W.kio_sub : W.subscribed);
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
    if (op && kio()) { // krunker.io: its VAPID key is the page's own
        html_push_subscribe(KIO_VAPID, on_kio_subscribed, op);
        return;
    }
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
        finish(WEB_PUSH_UNSUBSCRIBE_FAILED);
        return;
    }
    html_push_unsubscribe(kio() ? on_kio_unsubscribed : on_unsubscribed, op);
}

void web_push_set_listener(const WebPushChangedFn fn) { W.changed = fn; }
