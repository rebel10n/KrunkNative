// The Stripe purchase path (client/src/ui/kio_checkout_stripe.c): the request sequence, bodies, the poll timer, the 30-minute
// limit, the failure outcomes, and the log's URL redaction. The implementation is included so the hooks (the transport,
// the clock, the browser, the sign-in check) can be replaced by fakes: nothing here touches the network, and the real hosts
// are never contacted. The live services are not exercised: no card is linked to the test account.
#include "../src/ui/kio_checkout_stripe.c"

#include <stdio.h>
#include <string.h>

// ---- fakes: the transport records each request and keeps its answer for the test to give
typedef struct {
    char method[8];
    KrunkerService svc;
    char path[256];
    char body[1024];
    int has_body;
    KrunkerApiFn fn;
    void *user;
} Sent;
static Sent sent[16];
static int nsent;
static int send_ok = 1;
static double fake_now;
static int signed_in = 1;
static int nopen;
static char opened[512];

static int fake_send(const char *method, const KrunkerService svc, const char *path, const char *body, const KrunkerApiFn fn, void *user) {
    if (!send_ok || nsent >= 16) return 0;
    Sent *s = &sent[nsent++];
    snprintf(s->method, sizeof(s->method), "%s", method);
    s->svc = svc;
    snprintf(s->path, sizeof(s->path), "%s", path);
    s->has_body = body != NULL;
    snprintf(s->body, sizeof(s->body), "%s", body ? body : "");
    s->fn = fn;
    s->user = user;
    return 1;
}

// the real symbols the implementation names: never called (the hooks are replaced below)
int krunker_account_api(const char *method, KrunkerService svc, const char *path, const char *body, KrunkerApiFn fn, void *user) {
    (void) method; (void) svc; (void) path; (void) body; (void) fn; (void) user;
    return 0;
}
int krunker_account_signed_in(void) { return 1; }
void ui_open_browser(const char *url) {
    nopen++;
    snprintf(opened, sizeof(opened), "%s", url);
}
double ks_wall_ms(void) { return fake_now; }

static int fake_signed_in(void) { return signed_in; }
static double fake_clock(void) { return fake_now; }
static void fake_open(const char *url) { ui_open_browser(url); }

// ---- the purchase's outcome
static int calls, outcome_v;
static char err_v[256];
static int claim_seen;
static double funds_v;
static void done_cb(void *u, const int o, const cJSON *c, const char *e) {
    (void) u;
    calls++;
    outcome_v = o;
    snprintf(err_v, sizeof(err_v), "%s", e ? e : "");
    claim_seen = c != NULL;
    if (c) {
        const cJSON *f = cJSON_GetObjectItemCaseSensitive(c, "funds");
        funds_v = cJSON_IsNumber(f) ? f->valuedouble : -1;
    }
}

static int failures;
#define CHECK(cond, ...)                                  \
    do {                                                  \
        if (!(cond)) {                                    \
            failures++;                                   \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);   \
            printf(__VA_ARGS__);                          \
            printf("\n");                                 \
        }                                                 \
    } while (0)

static int has(const char *hay, const char *needle) { return strstr(hay, needle) != NULL; }

static void reset(void) {
    kio_checkout_stripe_close(); // ends a purchase a previous test left running (no done)
    nsent = 0;
    calls = 0;
    outcome_v = -9;
    err_v[0] = 0;
    claim_seen = 0;
    funds_v = -1;
    nopen = 0;
    opened[0] = 0;
    send_ok = 1;
    signed_in = 1;
    fake_now = 1000000.0;
}

// Answers the most recent request with status and a JSON body (NULL = no body).
static void reply(const int status, const char *json) {
    Sent *s = &sent[nsent - 1];
    cJSON *r = json ? cJSON_Parse(json) : NULL;
    s->fn(s->user, status, r);
    cJSON_Delete(r);
}

static int begin_pack(void) {
    cJSON *b = cJSON_Parse("{\"pack\":5}");
    const int r = kio_checkout_stripe_begin(b, done_cb, NULL);
    cJSON_Delete(b);
    return r;
}

// Drives a purchase to the page being open: checkout and payment-intent answered, the page's URL known.
static void to_open_page(const char *url) {
    char json[512];
    CHECK(begin_pack(), "begin starts");
    reply(200, "{\"data\":{\"payment\":{\"paymentId\":\"P1\"}}}");
    snprintf(json, sizeof(json), "{\"transactionId\":\"T1\",\"payment\":{\"checkout\":{\"mode\":\"hosted\",\"url\":\"%s\"}},"
                                 "\"order\":{\"reference\":\"kr_500\"}}", url);
    reply(200, json);
}

int main(void) {
    kio_checkout_stripe_close();
    send_fn = fake_send;
    signed_in_fn = fake_signed_in;
    open_fn = fake_open;
    clock_fn = fake_clock;

    // 1. the log's redaction: scheme and host only, never a path, query or token
    {
        char out[128];
        CHECK(kio_stripe_redact("https://checkout.stripe.com/c/pay/cs_test_abc?x=1", out, sizeof(out)) == 1, "redacts a URL");
        CHECK(!strcmp(out, "https://checkout.stripe.com/..."), "redacted to host and '/...': %s", out);
        CHECK(!has(out, "cs_test") && !has(out, "x=1"), "no session or query in the log form");
        CHECK(kio_stripe_redact("https://krunker.io", out, sizeof(out)) == 1 && !strcmp(out, "https://krunker.io"),
              "a bare host is kept: %s", out);
        CHECK(kio_stripe_redact("not a url", out, sizeof(out)) == 0 && out[0] == 0, "a non-URL is refused");
    }

    // 2. a guest is refused before any request
    reset();
    signed_in = 0;
    CHECK(begin_pack() == 0, "guest: begin refused");
    CHECK(calls == 1 && outcome_v == KIO_STRIPE_ERROR, "guest: done with an error");
    CHECK(nsent == 0, "guest: nothing sent (%d)", nsent);

    // 3. begin: iap/checkout on api.krunker.io with the body as built
    reset();
    CHECK(begin_pack() == 1, "begin starts");
    CHECK(nsent == 1, "one request (%d)", nsent);
    CHECK(!strcmp(sent[0].method, "POST") && sent[0].svc == KRUNKER_SVC_DATA && !strcmp(sent[0].path, "iap/checkout"),
          "POST iap/checkout on the data client: %s %s", sent[0].method, sent[0].path);
    CHECK(has(sent[0].body, "\"pack\":5"), "the checkout body is the begin body: %s", sent[0].body);
    CHECK(calls == 0, "done waits for the answer");

    // 4. the checkout answer -> payment-intent for the hosted page, on the crucible wallet, for GPEFWO1
    reply(200, "{\"data\":{\"payment\":{\"paymentId\":\"P1\"}}}");
    CHECK(nsent == 2, "payment-intent sent (%d)", nsent);
    CHECK(!strcmp(sent[1].method, "POST") && sent[1].svc == KRUNKER_SVC_CRUCIBLE
          && !strcmp(sent[1].path, "wallet/payment-intent/GPEFWO1"), "POST the payment-intent: %s", sent[1].path);
    CHECK(has(sent[1].body, "\"paymentId\":\"P1\"") && has(sent[1].body, "\"checkout\":\"hosted\"")
          && has(sent[1].body, "\"returnUrl\":\"https://krunker.io/\""), "the payment-intent body: %s", sent[1].body);

    // 5. the intent answer -> the page opens in the browser; no poll before the 2 s are up
    reply(200, "{\"transactionId\":\"T1\",\"payment\":{\"checkout\":{\"mode\":\"hosted\",\"url\":\"https://checkout.stripe.com/c/pay/cs_test_1\"}},"
               "\"order\":{\"reference\":\"kr_500\"}}");
    CHECK(nopen == 1 && !strcmp(opened, "https://checkout.stripe.com/c/pay/cs_test_1"), "the page opens: %s", opened);
    CHECK(nsent == 2 && calls == 0, "waiting, nothing new sent (%d, %d)", nsent, calls);
    fake_now += 1000;
    kio_checkout_stripe_tick();
    CHECK(nsent == 2, "no poll at 1 s (%d)", nsent);
    fake_now += 1000;
    kio_checkout_stripe_tick();
    CHECK(nsent == 3 && sent[2].svc == KRUNKER_SVC_CRUCIBLE && !strcmp(sent[2].path, "wallet/transactions/T1") && !strcmp(sent[2].method, "GET"),
          "poll at 2 s: GET wallet/transactions/T1 (%d %s)", nsent, nsent == 3 ? sent[2].path : "");

    // 6. pending -> poll again at 2 s; SUCCESS -> the claim, then consume and done with the claim's data
    reply(200, "{\"status\":\"pending\"}");
    kio_checkout_stripe_tick();
    CHECK(nsent == 3, "no poll before the next 2 s");
    fake_now += 2000;
    kio_checkout_stripe_tick();
    CHECK(nsent == 4, "poll again (%d)", nsent);
    reply(200, "{\"status\":\"SUCCESS\"}");
    CHECK(nsent == 5 && !strcmp(sent[4].path, "iap/claim") && sent[4].svc == KRUNKER_SVC_DATA, "the claim on the data client (%s)", sent[4].path);
    CHECK(has(sent[4].body, "\"channelId\":\"stripe\"") && has(sent[4].body, "\"productId\":\"kr_500\"")
          && has(sent[4].body, "\"purchaseId\":\"T1\""), "the claim body: %s", sent[4].body);
    reply(200, "{\"data\":{\"funds\":500,\"pack\":5,\"price\":4.99}}");
    CHECK(nsent == 6 && !strcmp(sent[5].path, "wallet/transactions/T1/consume") && !strcmp(sent[5].method, "POST")
          && !strcmp(sent[5].body, "{}"), "consumed after the claim (%s)", nsent == 6 ? sent[5].path : "");
    CHECK(calls == 1 && outcome_v == KIO_STRIPE_DONE, "done with DONE (%d, %d)", calls, outcome_v);
    CHECK(claim_seen && funds_v == 500, "done carries the claim's data (funds %.2f)", funds_v);

    // 7. the claim's error is the purchase's error text
    reset();
    to_open_page("https://checkout.stripe.com/c/pay/cs_test_2");
    fake_now += 2000;
    kio_checkout_stripe_tick();
    reply(200, "{\"status\":\"SUCCESS\"}");
    reply(200, "{\"error\":{\"message\":\"Boom\"}}");
    CHECK(calls == 1 && outcome_v == KIO_STRIPE_ERROR && !strcmp(err_v, "Boom") && !claim_seen,
          "claim error: done with the message (%d %d %s)", calls, outcome_v, err_v);

    // 8. REFUNDED fails the purchase; no claim is sent
    reset();
    to_open_page("https://checkout.stripe.com/c/pay/cs_test_3");
    fake_now += 2000;
    kio_checkout_stripe_tick();
    reply(200, "{\"status\":\"REFUNDED\"}");
    CHECK(calls == 1 && outcome_v == KIO_STRIPE_ERROR && !strcmp(err_v, "The payment was refunded."), "REFUNDED: error (%d %s)", outcome_v, err_v);
    CHECK(nsent == 3, "REFUNDED: no claim sent (%d)", nsent);

    // 9. a declined payment's poll error fails it; a plain poll error is still waiting
    reset();
    to_open_page("https://checkout.stripe.com/c/pay/cs_test_4");
    fake_now += 2000;
    kio_checkout_stripe_tick();
    reply(402, "{\"error\":{\"code\":\"PAYMENT_REJECTED\"}}");
    CHECK(calls == 1 && outcome_v == KIO_STRIPE_ERROR && !strcmp(err_v, "The payment was declined."), "declined: error (%s)", err_v);
    reset();
    to_open_page("https://checkout.stripe.com/c/pay/cs_test_5");
    fake_now += 2000;
    kio_checkout_stripe_tick();
    reply(500, NULL);
    CHECK(calls == 0 && nsent == 3, "a transport error keeps waiting (%d %d)", calls, nsent);

    // 10. the 30-minute limit ends the wait with an error and no more polls
    reset();
    to_open_page("https://checkout.stripe.com/c/pay/cs_test_6");
    fake_now += 31.0 * 60.0 * 1000.0;
    kio_checkout_stripe_tick();
    CHECK(calls == 1 && outcome_v == KIO_STRIPE_ERROR, "timeout: error (%d %d)", calls, outcome_v);
    CHECK(nsent == 2, "timeout: no poll after the limit (%d)", nsent);

    // 11. the request cannot be sent: begin refuses with the message
    reset();
    send_ok = 0;
    CHECK(begin_pack() == 0, "send failure: begin refused");
    CHECK(calls == 1 && outcome_v == KIO_STRIPE_ERROR && !strcmp(err_v, "Could not reach krunker.io. Try again in a moment."),
          "send failure: the message (%s)", err_v);

    // 12. a second begin while one runs is refused; the first is untouched
    reset();
    CHECK(begin_pack() == 1, "first begin");
    CHECK(begin_pack() == 0, "second begin refused");
    CHECK(calls == 1 && outcome_v == KIO_STRIPE_ERROR && has(err_v, "already running"), "second begin: done with an error");
    CHECK(nsent == 1, "the second begin sent nothing (%d)", nsent);

    // 13. the intent without a hosted page: no browser, an error
    reset();
    CHECK(begin_pack() == 1, "begin");
    reply(200, "{\"data\":{\"payment\":{\"paymentId\":\"P2\"}}}");
    reply(200, "{\"transactionId\":\"T2\",\"payment\":{\"checkout\":{\"mode\":\"embedded\"}}}");
    CHECK(nopen == 0 && calls == 1 && outcome_v == KIO_STRIPE_ERROR, "embedded mode is not opened: error");

    // 14. the checkout without a paymentId: an error
    reset();
    CHECK(begin_pack() == 1, "begin");
    reply(200, "{\"data\":{}}");
    CHECK(calls == 1 && outcome_v == KIO_STRIPE_ERROR, "no paymentId: error");

    // 15. close ends the purchase without done; the late answer of the closed one is ignored
    reset();
    CHECK(begin_pack() == 1, "begin");
    reply(200, "{\"data\":{\"payment\":{\"paymentId\":\"P3\"}}}");
    kio_checkout_stripe_close();
    reply(200, "{\"transactionId\":\"T3\",\"payment\":{\"checkout\":{\"mode\":\"hosted\",\"url\":\"https://checkout.stripe.com/x\"}}}");
    CHECK(calls == 0 && nopen == 0, "close: no done, no browser (%d %d)", calls, nopen);

    // 16. the consume's failure does not change a purchase that has been claimed
    reset();
    to_open_page("https://checkout.stripe.com/c/pay/cs_test_7");
    fake_now += 2000;
    kio_checkout_stripe_tick();
    reply(200, "{\"status\":\"SUCCESS\"}");
    send_ok = 0;
    reply(200, "{\"data\":{\"funds\":250}}");
    CHECK(calls == 1 && outcome_v == KIO_STRIPE_DONE && funds_v == 250, "consume failure: still DONE (%d %d %.0f)", calls, outcome_v, funds_v);

    kio_checkout_stripe_close();
    if (failures) {
        printf("kio_checkout_stripe_test: %d failures\n", failures);
        return 1;
    }
    printf("kio_checkout_stripe_test: OK\n");
    return 0;
}
