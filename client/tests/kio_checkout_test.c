// krunker_checkout: the real-money purchase's frames, their shapes, the S2C labels, PayStation's page, the log's URL,
// the catalog spec and the browser wait (client/src/net/krunker_checkout.c; the flow: client/src/ui/kio_checkout.c).
#include <krunker_checkout.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;
#define CHECK(cond, ...)                                       \
    do {                                                       \
        if (!(cond)) {                                         \
            failures++;                                        \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);        \
            printf(__VA_ARGS__);                               \
            printf("\n");                                      \
        }                                                      \
    } while (0)

static int json_is(const cJSON *frame, const char *want) {
    char *text = cJSON_PrintUnformatted(frame);
    const int ok = text && !strcmp(text, want);
    if (!ok) printf("  got %s\n  want %s\n", text ? text : "(null)", want);
    cJSON_free(text);
    return ok;
}

static void test_frames(void) {
    const KrunkerCheckoutOpts plain = {.lang = "en", .method = -1, .flag = NULL};
    const KrunkerCheckoutOpts cash = {.lang = "de", .method = 3175, .flag = "paybycash"};
    // XsollaPopup: "xst" [pIndex, lang, a, b, paymentsChannelId] (deob-10.0.4.js:212348)
    cJSON *f = krunker_checkout_kr(3, &plain);
    CHECK(json_is(f, "[\"xst\",3,\"en\",null,null,null]"), "xst, no method");
    CHECK(krunker_checkout_frame_ok(f), "xst passes the allowlist");
    cJSON_Delete(f);
    f = krunker_checkout_kr(0, &cash);
    CHECK(json_is(f, "[\"xst\",0,\"de\",3175,\"paybycash\",null]"), "xst with a method and a flag");
    CHECK(krunker_checkout_frame_ok(f), "xst with a method passes");
    cJSON_Delete(f);
    CHECK(krunker_checkout_kr(-1, &plain) == NULL, "a negative pack is refused");
    // iïiïïíi: "xpurchase" [id, lang, a, b, channelId, display, quantity, gift] (:212396)
    f = krunker_checkout_product("abc_price", 12, 2, NULL, &plain);
    CHECK(json_is(f, "[\"xpurchase\",\"abc_price\",\"en\",null,null,null,12,2,null]"), "xpurchase");
    CHECK(krunker_checkout_frame_ok(f), "xpurchase passes");
    cJSON_Delete(f);
    f = krunker_checkout_product("wildoffer:9", -1, 0, "Friend", &plain);
    CHECK(json_is(f, "[\"xpurchase\",\"wildoffer:9\",\"en\",null,null,null,null,1,\"Friend\"]"), "a wild offer gift: display null, quantity 1");
    CHECK(krunker_checkout_frame_ok(f), "the wild offer's xpurchase passes");
    cJSON_Delete(f);
    CHECK(krunker_checkout_product("", 1, 1, NULL, &plain) == NULL, "an empty product id is refused");
    // "xstb" [bundle, lang, a, b, channelId, gift] (:212927), "xstp" (:220228), "xstbp" (:212959), "xstbpl" (:213007)
    f = krunker_checkout_bundle(42, "Pal", &plain); // selectBundleGift's name: selectBundle(el, id, false, name) (:212863-212892)
    CHECK(json_is(f, "[\"xstb\",42,\"en\",null,null,null,\"Pal\"]"), "xstb, a gift to Pal");
    CHECK(krunker_checkout_frame_ok(f), "the gift's xstb passes");
    cJSON_Delete(f);
    f = krunker_checkout_bundle(42, NULL, &plain); // BUY: no gift, the last argument null
    CHECK(json_is(f, "[\"xstb\",42,\"en\",null,null,null,null]"), "xstb, for the account itself");
    CHECK(krunker_checkout_frame_ok(f), "xstb passes");
    cJSON_Delete(f);
    CHECK(krunker_checkout_bundle(-1, "Pal", &plain) == NULL, "a negative bundle is refused");
    f = krunker_checkout_premium(1, NULL, &plain);
    CHECK(json_is(f, "[\"xstp\",1,\"en\",null,null,null,null]"), "xstp");
    CHECK(krunker_checkout_frame_ok(f), "xstp passes");
    cJSON_Delete(f);
    f = krunker_checkout_bp_tier(1, "Pal", "en");
    CHECK(json_is(f, "[\"xstbp\",1,\"en\",null,null,null,\"Pal\"]"), "xstbp, a gift");
    CHECK(krunker_checkout_frame_ok(f), "xstbp passes");
    cJSON_Delete(f);
    f = krunker_checkout_bp_levels(5, "en");
    CHECK(json_is(f, "[\"xstbpl\",5,\"en\",null,null,null]"), "xstbpl");
    CHECK(krunker_checkout_frame_ok(f), "xstbpl passes");
    cJSON_Delete(f);
    CHECK(krunker_checkout_bp_levels(0, "en") == NULL, "zero levels are refused");
    f = krunker_checkout_invoice(123456);
    CHECK(json_is(f, "[\"xsin\",123456]"), "xsin");
    CHECK(krunker_checkout_frame_ok(f), "xsin passes");
    cJSON_Delete(f);
}

static void test_allowlist(void) {
    const char *bad[] = {
        "[\"xst\",\"3\",\"en\",null,null,null]",              // pIndex a string
        "[\"xst\",3,\"en\",null,null]",                       // one argument short
        "[\"xst\",3,\"en\",null,null,null,7]",                // one argument too many
        "[\"xstbp\",1,\"en\",5,null,null,null]",               // xstbp's method is always null
        "[\"xpurchase\",\"p\",\"en\",null,null,null,1,true,null]", // quantity not a number
        "[\"spin\",1]",                                       // not a purchase frame
        "[\"xstb\",1,\"en\",null,null,null,{\"x\":1}]",       // an object
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        cJSON *f = cJSON_Parse(bad[i]);
        CHECK(f && !krunker_checkout_frame_ok(f), "refused: %s", bad[i]);
        cJSON_Delete(f);
    }
    char longid[200];
    memset(longid, 'a', sizeof(longid) - 1);
    longid[sizeof(longid) - 1] = 0;
    char text[300];
    snprintf(text, sizeof(text), "[\"xpurchase\",\"%s\",\"en\",null,null,null,1,1,null]", longid);
    cJSON *f = cJSON_Parse(text);
    CHECK(f && !krunker_checkout_frame_ok(f), "a string of 128 or more is refused (the socket's copy is 128)");
    cJSON_Delete(f);
}

static void test_labels(void) {
    const char *ours[] = {"xst", "pur", "ppr", "purprm", "purb", "purbp", "purbpl", "pErr", "uprm", "abd", "gbdr"};
    for (size_t i = 0; i < sizeof(ours) / sizeof(ours[0]); i++) CHECK(krunker_checkout_label(ours[i]), "label %s is the checkout's", ours[i]);
    const char *theirs[] = {"uf", "a", "vr", "spin", "xs", "purbpll", ""};
    for (size_t i = 0; i < sizeof(theirs) / sizeof(theirs[0]); i++) CHECK(!krunker_checkout_label(theirs[i]), "label %s is not", theirs[i]);
    CHECK(!krunker_checkout_label(NULL), "NULL is not a label");
}

static void test_urls(void) {
    char url[256], log[256];
    CHECK(krunker_checkout_paystation_url("ab+c/=", 0, url, sizeof(url)) &&
              !strcmp(url, "https://secure.xsolla.com/paystation2/?access_token=ab%2Bc%2F%3D"), "PayStation, the token encoded: %s", url);
    CHECK(krunker_checkout_paystation_url("tok", 1, url, sizeof(url)) && !strcmp(url, "https://sandbox-secure.xsolla.com/paystation2/?access_token=tok"),
          "the sandbox's PayStation: %s", url);
    CHECK(!krunker_checkout_paystation_url("", 0, url, sizeof(url)), "no token, no page");
    CHECK(!krunker_checkout_paystation_url("tok", 0, url, 20), "a URL that does not fit is refused");
    krunker_checkout_log_url("https://secure.xsolla.com/paystation2/?access_token=SECRET123", log, sizeof(log));
    CHECK(!strcmp(log, "https://secure.xsolla.com/paystation2/?access_token=(not logged)") && !strstr(log, "SECRET"), "the token never logged: %s", log);
    krunker_checkout_log_url("https://krunker.io/?game=NY:bbbbb", log, sizeof(log));
    CHECK(!strcmp(log, "https://krunker.io/?game=NY:bbbbb"), "an official lobby's ?game= stays: %s", log);
    krunker_checkout_log_url("https://x.io/cb?state=1&Token=abc&code=99#kn-captcha=zz", log, sizeof(log));
    CHECK(!strcmp(log, "https://x.io/cb?state=1&Token=(not logged)&code=(not logged)#(not logged)"), "secrets and the fragment: %s", log);
    krunker_checkout_log_url("https://x.io/a?atx=1&at=2", log, sizeof(log));
    CHECK(!strcmp(log, "https://x.io/a?atx=1&at=(not logged)"), "a whole name only: %s", log);
    char small[16];
    krunker_checkout_log_url("https://example.com/very/long/path", small, sizeof(small));
    CHECK(strlen(small) == sizeof(small) - 1, "a short buffer is cut and terminated");
}

static void test_catalog(void) {
    int display = 0;
    char ref[64];
    CHECK(krunker_checkout_parse_catalog("catalog:{\"display\":12}|price_abc", &display, ref, sizeof(ref)) && display == 12 && !strcmp(ref, "price_abc"),
          "catalog spec: display %d ref %s", display, ref);
    CHECK(krunker_checkout_parse_catalog("catalog:{\"display\":null}|r1", &display, ref, sizeof(ref)) && display == -1 && !strcmp(ref, "r1"),
          "a null display");
    CHECK(!krunker_checkout_parse_catalog("catalog:{\"display\":1}|", &display, ref, sizeof(ref)), "no ref");
    CHECK(!krunker_checkout_parse_catalog("wildoffer:3", &display, ref, sizeof(ref)), "not a catalog spec");
}

static void test_wait(void) {
    KrunkerCheckoutWait w;
    // the account's funds known at the start: a rise is the payment
    krunker_checkout_wait_begin(&w, 1000, 1, 500);
    CHECK(krunker_checkout_wait_tick(&w, 1000) == KRUNKER_CHECKOUT_WAIT_IDLE, "no poll at once");
    CHECK(krunker_checkout_wait_tick(&w, 1000 + KRUNKER_CHECKOUT_POLL_MS) == KRUNKER_CHECKOUT_WAIT_POLL, "a poll after 5 s");
    CHECK(krunker_checkout_wait_tick(&w, 1000 + KRUNKER_CHECKOUT_POLL_MS + 10) == KRUNKER_CHECKOUT_WAIT_IDLE, "not again at once");
    CHECK(krunker_checkout_wait_tick(&w, 1000 + 2 * KRUNKER_CHECKOUT_POLL_MS + 10) == KRUNKER_CHECKOUT_WAIT_POLL, "the next 5 s later");
    CHECK(krunker_checkout_wait_funds(&w, 500) == KRUNKER_CHECKOUT_WAIT_IDLE, "the same funds: not paid");
    CHECK(krunker_checkout_wait_funds(&w, 450) == KRUNKER_CHECKOUT_WAIT_IDLE, "fewer funds: not paid");
    CHECK(krunker_checkout_wait_funds(&w, 1750) == KRUNKER_CHECKOUT_WAIT_PAID && !w.active, "a rise: paid, the wait ends");
    CHECK(krunker_checkout_wait_funds(&w, 9000) == KRUNKER_CHECKOUT_WAIT_IDLE, "nothing after the end");
    CHECK(krunker_checkout_wait_tick(&w, 1e9) == KRUNKER_CHECKOUT_WAIT_IDLE, "no ticks after the end");
    // no funds known at the start: the first read is the baseline
    krunker_checkout_wait_begin(&w, 0, 0, 0);
    CHECK(krunker_checkout_wait_funds(&w, 300) == KRUNKER_CHECKOUT_WAIT_IDLE && w.has_baseline && w.baseline == 300, "the first read is the baseline");
    CHECK(krunker_checkout_wait_funds(&w, 700) == KRUNKER_CHECKOUT_WAIT_PAID, "then a rise is paid");
    // ten minutes, nothing credited
    krunker_checkout_wait_begin(&w, 0, 1, 10);
    CHECK(krunker_checkout_wait_tick(&w, KRUNKER_CHECKOUT_WAIT_MS - 1) == KRUNKER_CHECKOUT_WAIT_POLL, "still polling just before the limit");
    CHECK(krunker_checkout_wait_tick(&w, KRUNKER_CHECKOUT_WAIT_MS) == KRUNKER_CHECKOUT_WAIT_EXPIRED && !w.active, "expired at 10 min");
    CHECK(krunker_checkout_wait_tick(&w, KRUNKER_CHECKOUT_WAIT_MS + 1) == KRUNKER_CHECKOUT_WAIT_IDLE, "expired once");
    // the server's answer ends it
    krunker_checkout_wait_begin(&w, 0, 1, 10);
    krunker_checkout_wait_end(&w);
    CHECK(krunker_checkout_wait_funds(&w, 99) == KRUNKER_CHECKOUT_WAIT_IDLE && krunker_checkout_wait_tick(&w, 6000) == KRUNKER_CHECKOUT_WAIT_IDLE,
          "an ended wait does nothing");
}

int main(void) {
    test_frames();
    test_allowlist();
    test_labels();
    test_urls();
    test_catalog();
    test_wait();
    if (failures) {
        printf("kio_checkout_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("kio_checkout_test: OK\n");
    return 0;
}
