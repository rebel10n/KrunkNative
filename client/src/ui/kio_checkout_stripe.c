// The Stripe purchase path of a krunker.io real-money purchase (client/include/kio_checkout_stripe.h; the findings
// and their cites: doc/v10/checkout-stripe.md). Krunker's flow in C:
//   1. api.krunker.io iap/checkout (the body from begin)           -> data.payment {paymentId}     (deob-10.0.4.js:212420-212446)
//   2. FRVR wallet payment-intent {paymentId, checkout: "hosted"}   -> checkout.url                (channel:2600-2627, :2534-2560)
//   3. the system browser opens checkout.url (Stripe's own page)     (ui_open_browser; the URL is never logged whole)
//   4. the FRVR wallet transaction, polled every 2 s for 30 min:    SUCCESS done, REFUNDED / a declined code failed (channel:2834-2906)
//   5. api.krunker.io iap/claim {channelId: "stripe", productId, purchaseId}: the answer's data is the result (:212404-212418)
//   6. the wallet consume of the transaction, errors ignored         (:212410, sdk:4544-4560)
// Every request is krunker_account_api with the account's FRVR token as Bearer. The transport, the clock, the browser
// and the sign-in check are file-scope hooks so client/tests/kio_checkout_stripe_test.c drives the sequence without a
// network: the real hosts are never contacted from the tests.
#include <kio_checkout_stripe.h>
#include <krunker_account.h>
#include <cJSON.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void ui_open_browser(const char *url);      // ui_logic.h (declared here: that header is the whole UI)
double ks_wall_ms(void);                    // ks_game.h: the wall clock, ms

#define STRIPE_CHANNEL "stripe"             // the claim's channelId (deob-10.0.4.js:212410, :26079-26088)
#define STRIPE_GAME_ID "GPEFWO1"            // FRVR.config.gameId (index.html): the payment-intent's game (sdk:4779)
#define STRIPE_RETURN_URL "https://krunker.io/" // the page the bundle returns to (channel:1887-1893); the outcome never comes from it
#define STRIPE_POLL_MS 2000.0               // channel:2887-2906
#define STRIPE_TIMEOUT_MS (30.0 * 60.0 * 1000.0) // channel:2394 (1 800 000 ms)

typedef enum {
    ST_IDLE,
    ST_CHECKOUT,  // iap/checkout in flight
    ST_INTENT,    // payment-intent in flight
    ST_WAIT,      // the page is open: the next poll is due at S.due
    ST_POLL,      // a transaction poll in flight
    ST_CLAIM,     // iap/claim in flight
} StripeState;

// The hooks (tests replace them). Defaults: the account layer and the real clock.
typedef int (*StripeSendFn)(const char *method, KrunkerService svc, const char *path, const char *body, KrunkerApiFn fn, void *user);
typedef int (*StripeSignedInFn)(void);
typedef void (*StripeOpenFn)(const char *url);
typedef double (*StripeClockFn)(void);
static StripeSendFn send_fn = krunker_account_api;
static StripeSignedInFn signed_in_fn = krunker_account_signed_in;
static StripeOpenFn open_fn = ui_open_browser;
static StripeClockFn clock_fn = ks_wall_ms;

static struct {
    StripeState st;
    KioStripeDone done;
    void *user;
    cJSON *body;                 // begin's body (kept for the claim's productId)
    char payment_id[160];
    char transaction_id[160];    // also the claim's purchaseId (channel:2669-2682)
    char product[256];           // the claim's productId: the intent's order.reference, else the body's productId
    double started, due;
    int inflight;
    unsigned gen;                // bumped whenever a purchase ends: a late answer of an older one is ignored
} S;

// The redacted form of a URL for the log: scheme, host, and "/..." when there is a path or query. Never the token.
int kio_stripe_redact(const char *url, char *out, const size_t n) {
    if (!out || !n) return 0;
    out[0] = 0;
    const char *s = url ? strstr(url, "://") : NULL;
    if (!s) return 0;
    s += 3;
    const char *end = s + strcspn(s, "/?#");
    size_t host = (size_t) (end - url);
    if (host >= n) host = n - 1;
    memcpy(out, url, host);
    out[host] = 0;
    if (*end) snprintf(out + host, n - host, "/...");
    return 1;
}

static const char *str_of(const cJSON *o, const char *key) {
    const cJSON *v = o ? cJSON_GetObjectItemCaseSensitive(o, key) : NULL;
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static const cJSON *obj_of(const cJSON *o, const char *key) {
    const cJSON *v = o ? cJSON_GetObjectItemCaseSensitive(o, key) : NULL;
    return cJSON_IsObject(v) ? v : NULL;
}

// The answer's error message, when it has one (the {data, error} shape, :25929-25958).
static const char *error_message(const cJSON *res) {
    const cJSON *err = res ? cJSON_GetObjectItemCaseSensitive(res, "error") : NULL;
    if (cJSON_IsObject(err)) return str_of(err, "message");
    return cJSON_IsString(err) ? err->valuestring : NULL;
}

static void finish(const int outcome, const cJSON *claim, const char *message) {
    const KioStripeDone done = S.done;
    void *user = S.user;
    S.st = ST_IDLE;
    S.done = NULL;
    S.user = NULL;
    S.inflight = 0;
    S.gen++;
    cJSON_Delete(S.body);
    S.body = NULL;
    S.payment_id[0] = S.transaction_id[0] = S.product[0] = 0;
    if (done) done(user, outcome, claim, message);
}

static void on_checkout(void *user, int status, const cJSON *res);
static void on_intent(void *user, int status, const cJSON *res);
static void on_poll(void *user, int status, const cJSON *res);
static void on_claim(void *user, int status, const cJSON *res);
static void on_ignore(void *user, int status, const cJSON *res) {
    (void) user;
    (void) status;
    (void) res; // the consume's answer is not read (errors ignored, as the bundle does)
}

// Sends one request tagged with the current generation. 0 = not sent: the purchase ends with an error.
static int request(const StripeState st, const char *method, const KrunkerService svc, const char *path, const char *body,
                   const KrunkerApiFn fn) {
    S.st = st;
    S.inflight = 1;
    if (!send_fn(method, svc, path, body, fn, (void *) (uintptr_t) S.gen)) {
        finish(KIO_STRIPE_ERROR, NULL, "Could not reach krunker.io. Try again in a moment.");
        return 0;
    }
    return 1;
}

static int send_json(const StripeState st, const KrunkerService svc, const char *path, cJSON *body, const KrunkerApiFn fn) {
    char *text = body ? cJSON_PrintUnformatted(body) : NULL;
    cJSON_Delete(body);
    if (body && !text) {
        finish(KIO_STRIPE_ERROR, NULL, "Purchase failed");
        return 0;
    }
    const int sent = request(st, "POST", svc, path, text, fn);
    free(text);
    return sent;
}

// Answer checks for a request of the current purchase. 0 = ignore (an older purchase's answer).
static int current(const void *user) { return (unsigned) (uintptr_t) user == S.gen && S.st != ST_IDLE; }

// iiìîiíï (deob-10.0.4.js:212398-212400): canShowStripe && !steam && FRVR.iap && stripeEnabled. canShowStripe is
// !isMsPwa && !isSteamClient && !isEpicClient (index.html) and stripeEnabled is on (:16476): none of those builds is
// this client's, so Stripe is offered whenever the krunker.io account is signed in (FRVR's iap needs its token).
int kio_checkout_stripe_available(void) { return signed_in_fn && signed_in_fn(); }

int kio_checkout_stripe_begin(const cJSON *body, const KioStripeDone done, void *user) {
    if (S.st != ST_IDLE) {
        if (done) done(user, KIO_STRIPE_ERROR, NULL, "A Stripe purchase is already running.");
        return 0;
    }
    if (!signed_in_fn || !signed_in_fn()) {
        if (done) done(user, KIO_STRIPE_ERROR, NULL, "Sign in to krunker.io to buy.");
        return 0;
    }
    if (!body) {
        if (done) done(user, KIO_STRIPE_ERROR, NULL, "Purchase failed");
        return 0;
    }
    S.body = cJSON_Duplicate(body, 1);
    if (!S.body) {
        if (done) done(user, KIO_STRIPE_ERROR, NULL, "Purchase failed");
        return 0;
    }
    S.done = done;
    S.user = user;
    S.started = clock_fn();
    S.payment_id[0] = S.transaction_id[0] = S.product[0] = 0;
    // iap.checkout: the body as built (:212430-212446); the answer carries data.payment. request() ends the
    // purchase itself when it cannot send.
    char *text = cJSON_PrintUnformatted(S.body);
    if (!text) {
        finish(KIO_STRIPE_ERROR, NULL, "Purchase failed");
        return 0;
    }
    const int sent = request(ST_CHECKOUT, "POST", KRUNKER_SVC_DATA, "iap/checkout", text, on_checkout);
    free(text);
    return sent;
}

static void on_checkout(void *user, const int status, const cJSON *res) {
    if (!current(user)) return;
    S.inflight = 0;
    const cJSON *payment = obj_of(obj_of(res, "data"), "payment");
    const char *pid = str_of(payment, "paymentId");
    if (status == 0 || error_message(res) || !pid) {
        finish(KIO_STRIPE_ERROR, NULL, error_message(res) ? error_message(res) : "Purchase failed");
        return;
    }
    snprintf(S.payment_id, sizeof(S.payment_id), "%s", pid);
    // payment-intent for the hosted Checkout page (channel:2600-2627, the wallet's answer is the page's URL)
    cJSON *b = cJSON_CreateObject();
    cJSON_AddStringToObject(b, "paymentId", S.payment_id);
    cJSON_AddStringToObject(b, "checkout", "hosted");
    cJSON_AddStringToObject(b, "returnUrl", STRIPE_RETURN_URL);
    char path[160];
    snprintf(path, sizeof(path), "wallet/payment-intent/%s", STRIPE_GAME_ID);
    send_json(ST_INTENT, KRUNKER_SVC_CRUCIBLE, path, b, on_intent);
}

static void on_intent(void *user, const int status, const cJSON *res) {
    if (!current(user)) return;
    S.inflight = 0;
    const cJSON *payment = obj_of(res, "payment");
    const cJSON *checkout = obj_of(payment, "checkout");
    const char *url = str_of(checkout, "url");
    const char *tid = str_of(res, "transactionId");
    const char *mode = str_of(checkout, "mode");
    if (status == 0 || error_message(res) || !tid || !url || !mode || strcmp(mode, "hosted")) {
        finish(KIO_STRIPE_ERROR, NULL, error_message(res) ? error_message(res) : "Could not open the payment page.");
        return;
    }
    snprintf(S.transaction_id, sizeof(S.transaction_id), "%s", tid);
    // the claim's productId: the order's reference (channel:2669-2682), else the body's productId
    const char *ref = str_of(obj_of(res, "order"), "reference");
    const char *sku = str_of(S.body, "productId");
    if (ref) snprintf(S.product, sizeof(S.product), "%s", ref);
    else if (sku) snprintf(S.product, sizeof(S.product), "%s", sku);
    else {
        finish(KIO_STRIPE_ERROR, NULL, "Purchase failed");
        return;
    }
    char shown[256];
    if (kio_stripe_redact(url, shown, sizeof(shown))) printf("[kio-stripe] payment page: %s\n", shown);
    open_fn(url);
    S.st = ST_WAIT;
    S.due = clock_fn() + STRIPE_POLL_MS;
}

// Main thread, once per frame: the next poll when it is due, and the 30-minute limit (channel:2394).
void kio_checkout_stripe_tick(void) {
    if (S.st != ST_WAIT) return;
    const double now = clock_fn();
    if (now - S.started >= STRIPE_TIMEOUT_MS) {
        finish(KIO_STRIPE_ERROR, NULL, "The payment was not confirmed in time. Check your purchases on krunker.io.");
        return;
    }
    if (now < S.due) return;
    char path[320];
    snprintf(path, sizeof(path), "wallet/transactions/%s", S.transaction_id);
    request(ST_POLL, "GET", KRUNKER_SVC_CRUCIBLE, path, NULL, on_poll);
}

static void on_poll(void *user, const int status, const cJSON *res) {
    if (!current(user)) return;
    S.inflight = 0;
    const char *code = str_of(obj_of(res, "error"), "code");
    if (status != 0 && code && (!strcmp(code, "PAYMENT_REJECTED") || !strcmp(code, "PAYMENT_REFUNDED"))) {
        finish(KIO_STRIPE_ERROR, NULL, "The payment was declined.");
        return;
    }
    // a poll error is still waiting, unless it says the payment is refused (channel:2870-2884)
    const char *st = str_of(res, "status");
    if (st && !strcmp(st, "REFUNDED")) {
        finish(KIO_STRIPE_ERROR, NULL, "The payment was refunded.");
        return;
    }
    if (st && !strcmp(st, "SUCCESS")) {
        // the claim: productId and purchaseId = the transaction (channel:2669-2682, :212404-212418)
        cJSON *b = cJSON_CreateObject();
        cJSON_AddStringToObject(b, "channelId", STRIPE_CHANNEL);
        cJSON_AddStringToObject(b, "productId", S.product);
        cJSON_AddStringToObject(b, "purchaseId", S.transaction_id);
        send_json(ST_CLAIM, KRUNKER_SVC_DATA, "iap/claim", b, on_claim);
        return;
    }
    S.st = ST_WAIT;
    S.due = clock_fn() + STRIPE_POLL_MS;
}

static void on_claim(void *user, const int status, const cJSON *res) {
    if (!current(user)) return;
    S.inflight = 0;
    const cJSON *data = obj_of(res, "data");
    if (status == 0 || error_message(res) || !data) {
        finish(KIO_STRIPE_ERROR, NULL, error_message(res) ? error_message(res) : "Purchase failed");
        return;
    }
    // consumed once the claim is accepted (:212410): the wallet is not asked again at the next start. Fire and
    // forget: its answer and its failure to send do not change the purchase, which the claim has settled.
    char path[320];
    snprintf(path, sizeof(path), "wallet/transactions/%s/consume", S.transaction_id);
    send_fn("POST", KRUNKER_SVC_CRUCIBLE, path, "{}", on_ignore, (void *) (uintptr_t) S.gen);
    finish(KIO_STRIPE_DONE, data, NULL);
}

// The player left the store or the account changed: the purchase ends without done.
void kio_checkout_stripe_close(void) {
    if (S.st == ST_IDLE) return;
    cJSON_Delete(S.body);
    S.body = NULL;
    S.st = ST_IDLE;
    S.done = NULL;
    S.user = NULL;
    S.inflight = 0;
    S.gen++;
}
