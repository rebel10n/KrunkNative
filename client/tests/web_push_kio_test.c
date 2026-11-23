// web_push.c on a krunker.io account (the active backend is krunker.io): v10.0.7's webpush client on gapi
// (deob-10.0.7.js:26455-26483) and its flows (:148910-149027), with web_push.c and krunker_kio_routes.c included whole
// and doubles for the browser (html_dom.h's push calls), krunker_account_api and the brain. Nothing is requested.
//   cmake-build-debug/web_push_kio_test
#include "../src/ui/web_push.c"
#include "../src/net/krunker_kio_routes.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail, g_pass;
#define CHECK(cond, ...) do { if (cond) g_pass++; else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// ---- doubles
static AccountBackend g_backend = ACCOUNT_BACKEND_KRUNKER;
AccountBackend account_backend(void) { return g_backend; }
const AccountInfo *active_account(void) { return NULL; }
const char *account_session_token(void) { return ""; }
int backend_enabled(void) { return 1; }
const char *backend_url(void) { return "http://brain.test"; }
static int g_brain_calls;
int backend_http_get(const char *path, BackendHttpFn fn, void *user) { (void) path; (void) fn; (void) user; g_brain_calls++; return 0; }
int backend_http_request(const char *method, const char *url, const char *json_body, BackendHttpFn fn, void *user) {
    (void) method; (void) url; (void) json_body; (void) fn; (void) user;
    g_brain_calls++;
    return 0;
}
int html_storage_get(const char *key, char *buf, size_t n) { (void) key; (void) buf; (void) n; return -1; }
void html_storage_set(const char *key, const char *value) { (void) key; (void) value; }

// the browser: one pending subscribe / unsubscribe / current answer at a time, given by the test
static int g_permission = 1;
static char g_browser_sub[512]; // the browser's subscription ("" = none)
static char g_subscribe_key[128];
static int g_browser_unsubscribes;
static char g_listen_key[128];
static HtmlPushEventFn g_listen_fn;
typedef struct {
    HtmlPushFn fn;
    void *user;
    int kind; // 1 subscribe, 2 unsubscribe, 3 current
} Pending;
static Pending g_pending[8];
static int g_pending_n;
int html_push_supported(void) { return 1; }
int html_push_permission(void) { return g_permission; }
static void pend(const int kind, const HtmlPushFn fn, void *user) { g_pending[g_pending_n++] = (Pending){fn, user, kind}; }
void html_push_subscribe(const char *key, const HtmlPushFn fn, void *user) {
    snprintf(g_subscribe_key, sizeof(g_subscribe_key), "%s", key);
    pend(1, fn, user);
}
void html_push_unsubscribe(const HtmlPushFn fn, void *user) { pend(2, fn, user); }
void html_push_current(const HtmlPushFn fn, void *user) { pend(3, fn, user); }
void html_push_listen(const char *key, const HtmlPushEventFn fn, void *user) {
    (void) user;
    snprintf(g_listen_key, sizeof(g_listen_key), "%s", key);
    g_listen_fn = fn;
}
// runs the browser's pending answers (subscribe: granted -> SUB, else NULL)
static const char *SUB = "{\"endpoint\":\"https://push.test/ep1\",\"expirationTime\":null,\"keys\":{\"p256dh\":\"BPk-_w\",\"auth\":\"a_b-\"}}";
static void browser(void) {
    while (g_pending_n) {
        const Pending p = g_pending[0];
        memmove(g_pending, g_pending + 1, (size_t) --g_pending_n * sizeof(Pending));
        if (p.kind == 1) {
            if (g_permission == 1) snprintf(g_browser_sub, sizeof(g_browser_sub), "%s", SUB);
            p.fn(p.user, g_permission == 1 ? g_browser_sub : NULL);
        } else if (p.kind == 2) {
            char was[512];
            snprintf(was, sizeof(was), "%s", g_browser_sub);
            g_browser_sub[0] = 0;
            if (was[0]) g_browser_unsubscribes++;
            p.fn(p.user, was[0] ? was : NULL);
        } else {
            p.fn(p.user, g_browser_sub[0] ? g_browser_sub : NULL);
        }
    }
}

// krunker_account_api: the requests, answered by the test
typedef struct {
    char method[8], path[96], body[1024];
    KrunkerApiFn fn;
    void *user;
} Api;
static Api g_api[8];
static int g_api_n, g_api_total;
int krunker_account_api(const char *method, const KrunkerService svc, const char *path, const char *body, const KrunkerApiFn fn, void *user) {
    if (svc != KRUNKER_SVC_API || g_api_n >= 8) return 0;
    Api *a = &g_api[g_api_n++];
    snprintf(a->method, sizeof(a->method), "%s", method);
    snprintf(a->path, sizeof(a->path), "%s", path);
    snprintf(a->body, sizeof(a->body), "%s", body ? body : "");
    a->fn = fn;
    a->user = user;
    g_api_total++;
    return 1;
}
static Api g_last;
// answers the oldest request with status / JSON (NULL: not JSON)
static void answer(const int status, const char *json) {
    if (!g_api_n) return;
    g_last = g_api[0];
    memmove(g_api, g_api + 1, (size_t) --g_api_n * sizeof(Api));
    cJSON *res = json ? cJSON_Parse(json) : NULL;
    g_last.fn(g_last.user, status, res);
    cJSON_Delete(res);
}

static int g_redraws;
static void redraw(void) { g_redraws++; }

static int body_is(const char *got, const char *want) {
    cJSON *a = cJSON_Parse(got), *b = cJSON_Parse(want);
    const int same = a && b && cJSON_Compare(a, b, 1);
    cJSON_Delete(a);
    cJSON_Delete(b);
    return same;
}

int main(void) {
    web_push_set_listener(redraw);

    // 1. the mount (the panel's first render): the worker's messages with krunker.io's key; no browser subscription
    //    yet, so no /webpush/status
    CHECK(!web_push_subscribed(), "not subscribed before anything");
    CHECK(!strcmp(g_listen_key, KIO_VAPID), "the worker is answered with krunker.io's VAPID key (:19502): %s", g_listen_key);
    browser();
    CHECK(g_api_total == 0, "no browser subscription: no /webpush/status (%d requests)", g_api_total);

    // 2. subscribe: the browser with krunker.io's key, then POST /webpush/subscribe with standard base64 keys
    web_push_subscribe();
    CHECK(web_push_loading(), "loading while it subscribes");
    CHECK(g_brain_calls == 0, "no brain VAPID fetch on krunker.io");
    browser();
    CHECK(!strcmp(g_subscribe_key, KIO_VAPID), "the browser subscribes with krunker.io's VAPID key");
    CHECK(g_api_n == 1 && !strcmp(g_api[0].method, "POST") && !strcmp(g_api[0].path, "/webpush/subscribe"), "POST /webpush/subscribe (%s %s)",
          g_api[0].method, g_api[0].path);
    CHECK(body_is(g_api[0].body, "{\"subscription\":{\"endpoint\":\"https://push.test/ep1\",\"keys\":{\"p256dh\":\"BPk+/w==\",\"auth\":\"a/b+\"}}}"),
          "the body is v10's {subscription: {endpoint, keys}} with btoa'd keys: %s", g_api[0].body);
    answer(200, "{\"data\":{\"success\":true,\"registration_id\":\"reg-77\"},\"error\":null}");
    CHECK(web_push_subscribed() && !web_push_loading() && !web_push_error(), "data.success: subscribed, no error");
    CHECK(!strcmp(W.kio_reg, "reg-77"), "data.registration_id is kept: %s", W.kio_reg);

    // 3. unsubscribe: the browser's goes, then POST /webpush/unsubscribe {registration_id, endpoint}
    web_push_unsubscribe();
    browser();
    CHECK(g_api_n == 1 && !strcmp(g_api[0].path, "/webpush/unsubscribe"), "POST /webpush/unsubscribe");
    CHECK(body_is(g_api[0].body, "{\"registration_id\":\"reg-77\",\"endpoint\":\"https://push.test/ep1\"}"), "the body: %s", g_api[0].body);
    answer(200, "{\"data\":{\"success\":true},\"error\":null}");
    CHECK(!web_push_subscribed() && !web_push_error() && !g_browser_sub[0], "unsubscribed, the browser's subscription gone");

    // 4. a refused subscribe ({success: false}): the browser lets go, no error shown
    web_push_subscribe();
    browser();
    answer(200, "{\"data\":{\"success\":false},\"error\":\"nope\"}");
    browser();
    CHECK(!web_push_subscribed() && !web_push_error() && !g_browser_sub[0], "refused: not subscribed, no error, the browser let go");

    // 5. a 5xx (the API client throws): the catch's error, the browser's subscription stays
    web_push_subscribe();
    browser();
    answer(503, "{\"error\":\"down\"}");
    CHECK(!web_push_subscribed() && web_push_error() && !strcmp(web_push_error(), WEB_PUSH_SUBSCRIBE_FAILED) && g_browser_sub[0],
          "5xx: \"Failed to subscribe to notifications\", the browser's kept");
    // and no answer at all is the same throw
    g_browser_sub[0] = 0;
    web_push_subscribe();
    browser();
    answer(0, NULL);
    CHECK(web_push_error() && !strcmp(web_push_error(), WEB_PUSH_SUBSCRIBE_FAILED), "no answer: the catch's error");

    // 6. permission not granted: no request, no error (v10 returns "Permission denied", it shows nothing)
    g_browser_sub[0] = 0;
    g_permission = 0;
    const int before = g_api_total;
    web_push_subscribe();
    browser();
    CHECK(g_api_total == before && !web_push_error() && !web_push_loading(), "permission refused: nothing sent, no error");
    g_permission = 1;

    // 7. a failed unsubscribe request (throws): the error, the store unchanged
    web_push_subscribe();
    browser();
    answer(200, "{\"data\":{\"success\":true,\"registration_id\":12},\"error\":null}");
    CHECK(!strcmp(W.kio_reg, "12"), "a numeric registration_id: %s", W.kio_reg);
    web_push_unsubscribe();
    browser();
    answer(500, NULL);
    CHECK(web_push_subscribed() && web_push_error() && !strcmp(web_push_error(), WEB_PUSH_UNSUBSCRIBE_FAILED),
          "a 500 on unsubscribe: \"Failed to unsubscribe from notifications\"");

    // 8. the mount's status check after a backend switch: the browser has a subscription -> POST /webpush/status {endpoint}
    snprintf(g_browser_sub, sizeof(g_browser_sub), "%s", SUB);
    g_backend = ACCOUNT_BACKEND_KRUNKIO;
    web_push_subscribed();
    g_backend = ACCOUNT_BACKEND_KRUNKER;
    CHECK(!web_push_subscribed(), "the mount starts unsubscribed (memory-only store)");
    browser();
    CHECK(g_api_n == 1 && !strcmp(g_api[0].path, "/webpush/status") && body_is(g_api[0].body, "{\"endpoint\":\"https://push.test/ep1\"}"),
          "POST /webpush/status {endpoint}: %s %s", g_api[0].path, g_api[0].body);
    answer(200, "{\"data\":{\"is_subscribed\":true},\"error\":null}");
    CHECK(web_push_subscribed() && !W.kio_reg[0], "is_subscribed: subscribed (no registration id)");
    // is_subscribed false: the browser lets go
    g_backend = ACCOUNT_BACKEND_KRUNKIO;
    web_push_subscribed();
    g_backend = ACCOUNT_BACKEND_KRUNKER;
    web_push_subscribed();
    browser();
    answer(200, "{\"data\":{\"is_subscribed\":false},\"error\":null}");
    browser();
    CHECK(!web_push_subscribed() && !g_browser_sub[0], "not is_subscribed: unsubscribed, the browser let go");

    // 9. the worker's WEBPUSH_SUBSCRIPTION_CHANGED: POST /webpush/subscription-change {old_subscription, new_subscription}
    snprintf(g_browser_sub, sizeof(g_browser_sub), "%s", SUB);
    g_listen_fn(NULL, "changed", "{\"old_subscription\":{\"endpoint\":\"https://push.test/old\"},\"new_subscription\":{\"endpoint\":\"https://push.test/ep1\"}}");
    CHECK(g_api_n == 1 && !strcmp(g_api[0].path, "/webpush/subscription-change") &&
              body_is(g_api[0].body, "{\"old_subscription\":{\"endpoint\":\"https://push.test/old\"},\"new_subscription\":{\"endpoint\":\"https://push.test/ep1\"}}"),
          "POST /webpush/subscription-change: %s", g_api[0].body);
    answer(200, "{\"data\":{\"success\":true,\"registration_id\":\"reg-88\"},\"error\":null}");
    browser();
    CHECK(web_push_subscribed() && !strcmp(W.kio_reg, "reg-88"), "success: the browser's subscription with the new registration id (%s)", W.kio_reg);
    g_listen_fn(NULL, "changed", "{\"old_subscription\":null,\"new_subscription\":{\"endpoint\":\"x\"}}");
    answer(200, "{\"data\":{\"success\":false},\"error\":null}");
    CHECK(web_push_error() && !strcmp(web_push_error(), KIO_CHANGE_FAILED), "no success: \"Failed to update subscription. Please re-subscribe.\"");

    // 10. WEBPUSH_SUBSCRIPTION_LOST: no subscription, v10.0.7's line
    g_listen_fn(NULL, "lost", NULL);
    CHECK(!web_push_subscribed() && web_push_error() && !strcmp(web_push_error(), KIO_LOST), "lost: unsubscribed, \"Push subscription was lost...\"");

    // 11. Box Office keeps its own routes: a subscribe there asks the brain's VAPID key, not gapi
    g_backend = ACCOUNT_BACKEND_KRUNKIO;
    const int api_before = g_api_total;
    g_brain_calls = 0;
    web_push_subscribe();
    CHECK(g_brain_calls == 1 && g_api_total == api_before, "Box Office: GET /api/push/vapid-key on the brain, nothing on gapi");
    g_listen_fn(NULL, "changed", "{\"old_subscription\":null,\"new_subscription\":null}");
    CHECK(g_api_total == api_before, "Box Office: a changed subscription is not sent to gapi");

    printf("web_push_kio_test: %d/%d\n", g_pass, g_pass + g_fail);
    return g_fail ? 1 : 0;
}
