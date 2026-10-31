// krunker.io real-money purchases, run in C (client/include/kio_checkout.h; the frames: krunker_checkout.h).
//
// v10's flow (deob-10.0.4.js:212330-213260) as v10 runs it, the provider's page excepted:
//   - a KR pack: showPurchase(index) opens the selector (iìiïiiî :212378: xsollaPayB "Credit/Debit Card", stripePayB when
//     Stripe can be offered, paypal empty); XsollaPopup(a, b) sends "xst" (:212333-212349) and waits "opening";
//   - a product (a catalogue product, a gift, a wild offer, premium, a bundle): iiïïìíì (:212388) shows the selector with
//     the product pending when Stripe can be offered, else iïiïïíi (:212391) sends its request at once;
//   - the battle pass: selectBPTier / skipLevels send "xstbp" / "xstbpl" at once (:212948-213011);
//   - the waiting state (iiíiîiî :212352): #purchaseHolder.waiting with purchase.opening, 30 s, then purchaseError
//     (purchase.openFailed); purchase.confirming, 60 s, then an INFO popup (purchase.confirmSlow);
//   - the server's "xst" [token, sandbox] (:212041) opens PayStation; its widget's INIT closes the selector (:212018-
//     212020, iîîîìîî :212370);
//   - the outcome: "pur" (:212158), "ppr" (:212193), "purprm" (:212263), "purb" (:212283), "purbp" / "purbpl"
//     (battlepass_ui_frame), "pErr" = purchaseError (:213229); purchaseCancel (:213218); cancelPurchase (:212330).
// KrunkNative: PayStation opens in the system browser (ui_open_browser). Its STATUS_DONE (the invoice for "xsin") never
// reaches the client, so the browser wait ends with the server's answer when it sends one, or when the account's funds
// rise (the server credits a paid payment: its "uf" push, or /player/me read every 5 s; krunker_checkout_wait_*); its
// Done button reads the account again, and after 10 minutes it ends on purchase.confirmSlow (doc/v10/checkout-browser.md).
// "xsin" is never sent: no invoice is known.
#include <kio_checkout.h>
#include <kio_checkout_stripe.h>
#include <krunker_checkout.h>
#include <krunker_net.h>
#include <kio_store.h>
#include <kio_inventory.h> // the official inventory, read again once a payment went through
#include <backend_ui.h>
#include <v10_ui.h>
#include <v10_wild_offer.h>
#include <ui_logic.h>
#include <html_ui.h>
#include <i18n.h>
#include "klog.h"
#include "../viewer/viewer_store.h" // the bundle popup's skins and their rarity colours
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KIO_CHECKOUT_OPEN_MS 30000.0      // iiíiîiî: "opening" gives up after 0x7530
#define KIO_CHECKOUT_CONFIRM_MS 60000.0   // "confirming" after 0xea60

typedef enum { KP_PRODUCT = 0, KP_BUNDLE, KP_PREMIUM } ProductKind;

// a product as iiïïìíì / iïiïïíi take it ({id, name, display, quantity, gift, bundle | premium})
typedef struct {
    ProductKind kind;
    char id[96], name[128], gift[64];
    int display, quantity, index; // display -1 = null; index: the bundle / premium
} Product;

static struct {
    int listening;       // krunker_net_checkout_listen is ours
    int requested;       // a request went out: a token answers it (a stray "xst" never opens a page)
    int kr_index;        // iiiïïíi: showPurchase's KR pack
    int has_product;     // iîíîiîî: a product waits in the selector
    Product product;
    int wait;            // iíïìííi: 0 none, 1 "open", 2 "confirm"
    int wait_timer;      // its timeout (html_timer)
    char provider[16];   // the waiting request's provider and product (logs, purchaseError)
    char product_id[96];
    int browser;         // the provider's page is in the system browser: the wait popup waits for the answer
    int watch;           // the account's funds are watched (Xsolla); Stripe's side polls its own transaction
    int polling;         // a /player/me read is in flight
    int done_pressed;    // the wait popup's Done: the next read decides
    unsigned browser_gen;
    cJSON *bundles;      // the official bundle rows ("abd") and the available ids ("gbdr")
    cJSON *available;
    int error_shown;     // iìiíîíí: purchaseError's #purchaseResponse is up
} K;

// ---------------------------------------------------------------- the page

static void set_display(const char *id, const char *v) {
    const HtmlElem e = html_dom_by_id(id);
    if (e) html_dom_set_display(e, v);
}

static int official(void) { return kio_store_active(); }

// genericPopup(html, cls): #genericPop over #popupBack (store_ui.c's)
static void generic_popup(const char *html, const char *cls) {
    ui_loadout_clear_pops();
    set_display("popupHolder", "block");
    const HtmlElem pop = html_dom_by_id("genericPop");
    if (pop) {
        html_dom_set_attr(pop, "class", cls);
        html_dom_set_attr(pop, "style", "");
        html_dom_set_html(pop, html);
        html_dom_set_display(pop, "block");
    }
}

// iìiiìïì(type, error, text) (:219789): SUCCESS / INFO / an error, genericPopup "responsePop" / "infoPop". The texts are
// escaped (v10 inserts them as HTML; a gift's name is the player's).
static void io_popup(const char *type, const char *error, const char *text) {
    StrBuf sb = {0};
    const int success = type && !strcmp(type, "SUCCESS"), info = type && !strcmp(type, "INFO");
    sb_appendf(&sb, "<div style='width:100%%;text-align:center'><div><i class='material-icons' style='font-size:80px;color:%s'>%s</i></div>"
                    "<div style='font-size:20px;color:%s'>",
               success ? "#3be354" : info ? "rgba(255,255,255,0.9)" : "#e33b3b", success ? "done" : info ? "info" : "clear",
               info ? "rgba(255,255,255,0.9)" : "rgba(255,255,255,0.5)");
    if (success) sb_append_esc(&sb, text && text[0] ? text : "Success!");
    else if (info) sb_append_esc(&sb, text && text[0] ? text : "Warning!");
    else sb_append_esc(&sb, error && error[0] ? error : I18N("social.market.failed", "Failed. Try Again"));
    sb_append(&sb, "</div></div>");
    char *html = sb_finish(&sb);
    generic_popup(html, info ? "infoPop" : "responsePop");
    free(html);
}

// ---------------------------------------------------------------- the selector and its waiting state

static void wait_clear(void) { // iîîìîìî (:212365)
    if (K.wait_timer) html_timer_cancel(K.wait_timer);
    K.wait_timer = 0;
    K.wait = 0;
    const HtmlElem h = html_dom_by_id("purchaseHolder");
    if (h) html_dom_set_class(h, "waiting", 0);
}

static void close_selector(void) { // closePurchaseSelector (:213214)
    wait_clear();
    set_display("purchaseHolder", "none");
}

static void close_if_waiting(void) { // iîîîìîî (:212370)
    if (K.wait) close_selector();
}

static void purchase_load_done(void) { // iííïîíî (:212373)
    set_display("purchaseLoad", "none");
    close_if_waiting();
}

// iìiïiiî (:212378): the selector, Stripe's button shown when it can be offered
static void selector_show(const int stripe) {
    wait_clear();
    set_display("purchaseHolder", "block");
    const HtmlElem x = html_dom_by_id("xsollaPayB");
    if (x) {
        char img[256], html[512];
        html_dom_asset_url("img/xsolla.png", img, sizeof(img));
        snprintf(html, sizeof(html), "<img src='%s' style='width:26px;margin-right:13px;margin-bottom:-4px' />Credit/Debit Card", img);
        html_dom_set_html(x, html);
    }
    const HtmlElem s = html_dom_by_id("stripePayB");
    if (s) {
        html_dom_set_display(s, stripe ? "inline-block" : "none");
        html_dom_set_text(s, I18N("purchase.stripe", "Card, Apple Pay & Google Pay"));
    }
    const HtmlElem p = html_dom_by_id("paypal-button");
    if (p) html_dom_set_html(p, "");
}

// purchaseError(provider, error, product) (:213229): #purchaseResponse with error.err, and v10's "click here" unless
// the purchase limit was reached
static void purchase_error(const char *provider, const char *err, const char *product) {
    close_if_waiting();
    K.error_shown = 1;
    const char *text = err && err[0] ? err : "Purchase Error";
    const int click = !strstr(text, "limit reached") && !strstr(text, "Can't buy any more.");
    StrBuf sb = {0};
    sb_append_esc(&sb, text);
    if (click) sb_appendf(&sb, " <a href='javascript:;' onclick='cancelPurchase()'>%s</a>", I18N("purchase.error.click", "click here</a> and try again!"));
    char *html = sb_finish(&sb);
    const HtmlElem h = html_dom_by_id("purchaseRespH");
    if (h) html_dom_set_html(h, html);
    free(html);
    set_display("purchaseResponse", "block");
    printf("[checkout] %s purchase of %s failed: %s\n", provider && provider[0] ? provider : "?", product && product[0] ? product : "?", text);
}

// the err argument of the answers: v10 reads error.err (an object); anything else is "Purchase Error"
static const char *err_text(const cJSON *e) {
    const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(e, "err"));
    return t && t[0] ? t : NULL;
}

static int err_set(const cJSON *e) { return e && !cJSON_IsNull(e) && !cJSON_IsFalse(e) && !(cJSON_IsString(e) && !e->valuestring[0]); }

static void purchase_cancel(const char *provider, const char *product) { // purchaseCancel (:213218)
    close_selector();
    K.has_product = 0;
    KN_LOG_VERBOSE2("[checkout] %s purchase of %s cancelled\n", provider ? provider : "?", product ? product : "?");
}

static void wait_timeout(void *user) {
    (void) user;
    K.wait_timer = 0;
    const int confirming = K.wait == 2;
    close_selector();
    if (confirming) io_popup("INFO", NULL, I18N("purchase.confirmSlow", "Your payment is still being confirmed. It will be added to your account as soon as it goes through."));
    else purchase_error(K.provider, I18N("purchase.openFailed", "Checkout couldn't open. Check your connection or ad blocker and try again."), K.product_id);
}

// iiíiîiî(provider, product, confirm) (:212352): the selector waits, "opening" or "confirming"
static void wait_begin(const char *provider, const char *product, const int confirm) {
    wait_clear();
    K.wait = confirm ? 2 : 1;
    snprintf(K.provider, sizeof(K.provider), "%s", provider);
    snprintf(K.product_id, sizeof(K.product_id), "%s", product ? product : "");
    const HtmlElem h = html_dom_by_id("purchaseHolder");
    if (h) html_dom_set_class(h, "waiting", 1);
    set_display("purchaseHolder", "block");
    const HtmlElem l = html_dom_by_id("purchaseWaitLabel");
    if (l) html_dom_set_text(l, confirm ? I18N("purchase.confirming", "Confirming your purchase...") : I18N("purchase.opening", "Opening checkout..."));
    K.wait_timer = html_timer_add(confirm ? KIO_CHECKOUT_CONFIRM_MS : KIO_CHECKOUT_OPEN_MS, wait_timeout, NULL);
}

// ---------------------------------------------------------------- the browser wait (KrunkNative)

static KrunkerCheckoutWait g_wait;

static int browser_popup_up(void) {
    const HtmlElem pop = html_dom_by_id("genericPop");
    return pop && html_dom_has_class(pop, "knCheckoutWait") && html_dom_is_shown(pop);
}

static void browser_end(void) {
    if (!K.browser) return;
    K.browser = 0;
    K.done_pressed = 0;
    K.browser_gen++;
    krunker_checkout_wait_end(&g_wait);
    if (browser_popup_up()) ui_loadout_clear_pops(); // ours, still up
}

// the wait popup (io_loadingPopup's look): the line, and Done when the funds are watched (the player is back)
static void browser_popup(const int done_button) {
    StrBuf sb = {0};
    sb_append(&sb, "<div style='width:100%;text-align:center'><div class='lds-ring'><div></div><div></div><div></div><div></div></div>"
                   "<div style='font-size:20px;color:rgba(255,255,255,0.5)'>");
    sb_append_esc(&sb, I18N("kn.checkout.browser", "Finish your payment in the browser window that opened. It is added to your account once it goes through."));
    sb_append(&sb, "</div>");
    if (done_button) {
        sb_append(&sb, "<div id='confirmBtn' style='width:calc(100% - 20px);margin-top:15px' onmouseover='playTick(0.1)' onclick='knCheckoutDone()'>");
        sb_append_esc(&sb, I18N("kn.checkout.done", "Done"));
        sb_append(&sb, "</div>");
    }
    sb_append(&sb, "</div>");
    char *html = sb_finish(&sb);
    generic_popup(html, "loadingPop knCheckoutWait");
    free(html);
}

static void browser_begin(const char *provider, const char *product, const int watch) {
    browser_end();
    snprintf(K.provider, sizeof(K.provider), "%s", provider);
    snprintf(K.product_id, sizeof(K.product_id), "%s", product ? product : "");
    K.browser = 1;
    K.watch = watch;
    K.done_pressed = 0;
    const AccountInfo *acc = active_account();
    if (watch) krunker_checkout_wait_begin(&g_wait, html_timer_now_ms(), acc != NULL, acc ? acc->funds : 0);
    browser_popup(watch);
}

static void after_success(void);

// The account's funds rose while the page was open: the server credited the payment
static void browser_paid(const double funds) {
    const double gained = g_wait.has_baseline ? funds - g_wait.baseline : 0;
    char product[96];
    snprintf(product, sizeof(product), "%s", K.product_id);
    browser_end();
    account_set_funds(funds);
    if (gained > 0) {
        char amount[32], text[160];
        v10_fmt_int(amount, sizeof(amount), gained);
        I18N_F(text, "purchase.kr.success", "Added {0} KR to your account", amount);
        io_popup("SUCCESS", NULL, text);
    } else {
        io_popup("SUCCESS", NULL, I18N("kn.checkout.success", "Purchase successful!"));
    }
    printf("[checkout] %s payment for %s credited (funds %.0f)\n", K.provider[0] ? K.provider : "?", product[0] ? product : "?", funds);
    after_success();
}

// ten minutes, or Done with nothing credited: v10's confirm timeout says the payment may still go through
static void browser_unconfirmed(void) {
    browser_end();
    kio_inventory_load(NULL, NULL); // an item product may have landed
    io_popup("INFO", NULL, I18N("purchase.confirmSlow", "Your payment is still being confirmed. It will be added to your account as soon as it goes through."));
}

// GET /player/me (getMe, deob-10.0.4.js:26367-26371): the account's funds
static void me_answered(void *user, const int status, const cJSON *data, const char *code, const char *message) {
    (void) status;
    (void) code;
    (void) message;
    K.polling = 0;
    if ((unsigned) (uintptr_t) user != K.browser_gen || !K.browser) return;
    static const char *const keys[] = {"player_funds", "funds", "kr"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        const cJSON *f = cJSON_GetObjectItem(data, keys[i]);
        if (!cJSON_IsNumber(f)) continue;
        if (krunker_checkout_wait_funds(&g_wait, f->valuedouble) == KRUNKER_CHECKOUT_WAIT_PAID) {
            browser_paid(f->valuedouble);
            return;
        }
        break;
    }
    if (K.done_pressed) browser_unconfirmed();
}

static void poll_funds(void) {
    if (K.polling) return;
    K.polling = 1;
    if (!kio_store_write("GET", "player/me", NULL, me_answered, (void *) (uintptr_t) K.browser_gen)) K.polling = 0;
}

// ---------------------------------------------------------------- requests

static void frame_listen(const char *label, const cJSON *frame);

static void listen_once(void) {
    if (K.listening) return;
    K.listening = 1;
    krunker_net_checkout_listen(frame_listen);
}

static KrunkerCheckoutOpts opts(const double method, const char *flag) {
    const KrunkerCheckoutOpts o = {.lang = i18n_locale(), .method = method, .flag = flag};
    return o;
}

// The request goes over the game socket (an official game must be open, as v10's socket always is)
static void send_request(cJSON *frame) {
    listen_once();
    const int sent = frame && krunker_net_send_account_frame(frame);
    cJSON_Delete(frame);
    if (sent) {
        K.requested = 1;
        KN_LOG_VERBOSE2("[checkout] %s request for %s sent\n", K.provider, K.product_id);
        return;
    }
    close_selector();
    purchase_error(K.provider, I18N("kn.checkout.offline", "Not connected to krunker.io: join an official game, then try again."), K.product_id);
}

static int signed_in(void) {
    if (active_account()) return 1;
    menu_show_window(5, 1); // showWindow(5)
    return 0;
}

static int kr_pack_val(const int index) { return store_ui_kr_pack_val(index); }

// iïiïïíi(product, a, b) (:212391): the product's Xsolla request
static void xsolla_product(const Product *p, const double method, const char *flag) {
    wait_begin("xsolla", p->id, 0);
    const KrunkerCheckoutOpts o = opts(method, flag);
    const char *gift = p->gift[0] ? p->gift : NULL;
    cJSON *f = p->kind == KP_BUNDLE ? krunker_checkout_bundle(p->index, gift, &o)
             : p->kind == KP_PREMIUM ? krunker_checkout_premium(p->index, gift, &o)
                                     : krunker_checkout_product(p->id, p->display, p->quantity, gift, &o);
    send_request(f);
}

// iiïïìíì(product) || iïiïïíi(product) (:212388-212397)
static void start_product(const Product *p) {
    if (!official() || !signed_in()) return;
    if (kio_checkout_stripe_available()) {
        K.product = *p;
        K.has_product = 1;
        selector_show(1);
        return;
    }
    xsolla_product(p, -1, NULL);
}

// XsollaPopup(a, b) (:212333-212349)
static void xsolla_popup(const double method, const char *flag) {
    if (!official()) return; // Box Office: v10's payment hook does nothing
    if (K.has_product) {
        const Product p = K.product;
        K.has_product = 0;
        xsolla_product(&p, method, flag);
        return;
    }
    char id[32];
    snprintf(id, sizeof(id), "kr_%d", kr_pack_val(K.kr_index));
    wait_begin("xsolla", id, 0);
    const KrunkerCheckoutOpts o = opts(method, flag);
    send_request(krunker_checkout_kr(K.kr_index, &o));
}

// ---------------------------------------------------------------- Stripe (kio_checkout_stripe.h)

static void success_kr(const char *provider, const cJSON *funds, int pack);
static void success_product(const char *provider, const char *product_id, const char *name, const char *gift, int display_id);
static void claimed_again(const char *product_id, const char *name, const char *gift, int display_id);
static void success_premium(int premium, const char *gift);
static void success_bundle(int bundle, const cJSON *funds, const cJSON *junk, int gifted, const char *gift);

// iiíïïïi (:212404-212418): the claim's data, as the socket's answers
static void stripe_done(void *user, const int outcome, const cJSON *claim, const char *error) {
    Product *p = user;
    browser_end();
    if (outcome == KIO_STRIPE_CANCELLED) purchase_cancel("stripe", p->id);
    else if (outcome != KIO_STRIPE_DONE) purchase_error("stripe", error, p->id);
    else if (cJSON_GetObjectItem(claim, "pack") && !cJSON_IsNull(cJSON_GetObjectItem(claim, "pack")))
        success_kr("stripe", cJSON_GetObjectItem(claim, "funds"), (int) cJSON_GetNumberValue(cJSON_GetObjectItem(claim, "pack")));
    else if (cJSON_GetObjectItem(claim, "bundle") && !cJSON_IsNull(cJSON_GetObjectItem(claim, "bundle")))
        success_bundle((int) cJSON_GetNumberValue(cJSON_GetObjectItem(claim, "bundle")), cJSON_GetObjectItem(claim, "funds"), cJSON_GetObjectItem(claim, "junk"),
                       cJSON_GetStringValue(cJSON_GetObjectItem(claim, "giftName")) != NULL, cJSON_GetStringValue(cJSON_GetObjectItem(claim, "giftName")));
    else if (cJSON_GetObjectItem(claim, "premium") && !cJSON_IsNull(cJSON_GetObjectItem(claim, "premium")))
        success_premium((int) cJSON_GetNumberValue(cJSON_GetObjectItem(claim, "premium")), cJSON_GetStringValue(cJSON_GetObjectItem(claim, "giftName")));
    else if (cJSON_IsTrue(cJSON_GetObjectItem(claim, "alreadyClaimed"))) // v10.0.6 (:213778-213790): shown, not granted again
        claimed_again(cJSON_GetStringValue(cJSON_GetObjectItem(claim, "productId")), cJSON_GetStringValue(cJSON_GetObjectItem(claim, "productName")),
                      cJSON_GetStringValue(cJSON_GetObjectItem(claim, "giftName")), (int) cJSON_GetNumberValue(cJSON_GetObjectItem(claim, "displayId")));
    else
        success_product("stripe", cJSON_GetStringValue(cJSON_GetObjectItem(claim, "productId")), cJSON_GetStringValue(cJSON_GetObjectItem(claim, "productName")),
                        cJSON_GetStringValue(cJSON_GetObjectItem(claim, "giftName")), (int) cJSON_GetNumberValue(cJSON_GetObjectItem(claim, "displayId")));
    free(p);
}

// iïíîíìï(product) (:212420-212446): iap.checkout's body
static void stripe_begin(const Product *p, const int pack) {
    cJSON *body = cJSON_CreateObject();
    const char *gift = p->gift[0] ? p->gift : NULL;
    if (pack >= 0) cJSON_AddNumberToObject(body, "pack", pack);
    else if (p->kind == KP_BUNDLE) {
        cJSON_AddNumberToObject(body, "bundle", p->index);
        if (gift) cJSON_AddStringToObject(body, "giftPlayerName", gift);
    } else if (p->kind == KP_PREMIUM) {
        cJSON_AddNumberToObject(body, "premium", p->index);
        if (gift) cJSON_AddStringToObject(body, "giftPlayerName", gift);
    } else {
        cJSON_AddStringToObject(body, "productId", p->id);
        if (p->display >= 0) cJSON_AddNumberToObject(body, "display", p->display);
        else cJSON_AddNullToObject(body, "display");
        cJSON_AddNumberToObject(body, "quantity", p->quantity >= 1 ? p->quantity : 1);
        if (gift) cJSON_AddStringToObject(body, "giftPlayerName", gift);
    }
    Product *u = malloc(sizeof(*u));
    if (!u) {
        cJSON_Delete(body);
        return;
    }
    *u = *p;
    close_selector();
    browser_begin("stripe", p->id, 0);
    kio_checkout_stripe_begin(body, stripe_done, u); // done runs (now, on a failure to start) and frees u
    cJSON_Delete(body);
}

// StripePopup() (:212481-212497)
static void stripe_popup(void) {
    if (!official()) return;
    if (K.has_product) {
        const Product p = K.product;
        K.has_product = 0;
        stripe_begin(&p, -1);
        return;
    }
    const int val = kr_pack_val(K.kr_index);
    char id[32];
    snprintf(id, sizeof(id), "kr_%d", val);
    if (!kio_checkout_stripe_available() || val <= 0) {
        close_selector();
        purchase_error("stripe", NULL, id);
        return;
    }
    Product p = {.kind = KP_PRODUCT, .display = -1, .quantity = 1};
    snprintf(p.id, sizeof(p.id), "%s", id);
    snprintf(p.name, sizeof(p.name), "%d KR", val);
    stripe_begin(&p, K.kr_index);
}

// ---------------------------------------------------------------- entry points

void kio_checkout_kr_pack(const int index) { // showPurchase(index) (:213019-213056)
    if (!official() || !signed_in() || kr_pack_val(index) <= 0) return;
    K.kr_index = index;
    K.has_product = 0;
    selector_show(kio_checkout_stripe_available());
}

void kio_checkout_kr_pack_direct(const int index) { // directPurchase(index) (:217616-217619)
    kio_checkout_kr_pack(index);
    if (official() && active_account() && kr_pack_val(index) > 0) xsolla_popup(-1, NULL);
}

void kio_checkout_catalog(const char *price_ref, const char *name, const int display, const int quantity) {
    if (!price_ref || !price_ref[0]) return;
    Product p = {.kind = KP_PRODUCT, .display = display, .quantity = quantity >= 1 ? quantity : 1};
    snprintf(p.id, sizeof(p.id), "%s", price_ref);
    snprintf(p.name, sizeof(p.name), "%s", name && name[0] ? name : price_ref);
    start_product(&p);
}

void kio_checkout_catalog_ref(const char *spec, const int quantity) {
    int display = -1;
    char ref[96];
    if (!krunker_checkout_parse_catalog(spec, &display, ref, sizeof(ref))) return;
    kio_checkout_catalog(ref, ref, display, quantity); // v10 names it store.catalog[ref].name; the ref otherwise
}

void kio_checkout_gift(const char *price_ref, const char *name, const int display, const char *gift_to) {
    if (!price_ref || !price_ref[0] || !gift_to || !gift_to[0]) return;
    Product p = {.kind = KP_PRODUCT, .display = display, .quantity = 1};
    snprintf(p.id, sizeof(p.id), "%s", price_ref);
    snprintf(p.name, sizeof(p.name), "%s", name && name[0] ? name : price_ref);
    snprintf(p.gift, sizeof(p.gift), "%s", gift_to);
    start_product(&p);
}

void kio_checkout_wild_offer(const int offer_id) {
    Product p = {.kind = KP_PRODUCT, .display = -1, .quantity = 1};
    snprintf(p.id, sizeof(p.id), "wildoffer:%d", offer_id);
    snprintf(p.name, sizeof(p.name), "Wild Offer %d", offer_id);
    start_product(&p);
}

// selectBundle(_, id, _, gift) (:212892-212935): {id: "bundle_" + id, name: "Bundle " + id, bundle: id, gift}
void kio_checkout_bundle(const int bundle, const char *gift_to) {
    if (bundle <= 0) return;
    Product p = {.kind = KP_BUNDLE, .display = -1, .quantity = 1, .index = bundle};
    snprintf(p.id, sizeof(p.id), "bundle_%d", bundle);
    snprintf(p.name, sizeof(p.name), "Bundle %d", bundle);
    if (gift_to && gift_to[0]) snprintf(p.gift, sizeof(p.gift), "%s", gift_to);
    start_product(&p);
}

const cJSON *kio_checkout_bundles(void) { return K.bundles; }

static const cJSON *bundle_row(const int id) {
    const cJSON *b;
    cJSON_ArrayForEach(b, K.bundles) {
        const cJSON *bid = cJSON_GetObjectItem(b, "ID");
        if (cJSON_IsNumber(bid) && (int) bid->valuedouble == id) return b;
    }
    return NULL;
}

static double row_num(const cJSON *b, const char *key) { // the rows hold numbers or numeric strings (parseFloat)
    const cJSON *v = cJSON_GetObjectItem(b, key);
    return cJSON_IsNumber(v) ? v->valuedouble : cJSON_IsString(v) ? atof(v->valuestring) : 0;
}

// bundlePopup(true, false, id) (deob-10.0.4.js:212740-212862): the bundle's skins (up to 12 of SKINS, each in its
// rarity's border), the discount label, its art, the Junk it adds, the struck original price, BUY "$PRICE USD" unless
// OWNED (selectBundle) and GIFT (selectBundleGift; "GIFT $PRICE USD" once owned). The offer timer is left out.
void kio_checkout_bundle_popup(const int id) {
    if (!official() || !signed_in()) return;
    const cJSON *b = bundle_row(id);
    if (!b) return;
    const int owned = cJSON_IsTrue(cJSON_GetObjectItem(b, "OWNED"));
    const double price = row_num(b, "PRICE"), disc = row_num(b, "DISCOUNT_PCT"), orig = row_num(b, "ORIGINAL_PRICE"), junk = row_num(b, "JUNK");
    StrBuf sb = {0};
    sb_append(&sb, "<div class='bundlItmCnt'>");
    const char *skins = cJSON_GetStringValue(cJSON_GetObjectItem(b, "SKINS"));
    const char *p = skins ? skins : "";
    for (int n = 0; n < 12 && *p;) {
        char *end = NULL;
        const long index = strtol(p, &end, 10);
        p = end && *end == ',' ? end + 1 : (end && end != p ? end : p + strlen(p));
        const ViewerSkin *k = viewer_skin((int) index);
        if (!k) continue;
        char thumb[320];
        store_skin_preview((int) index, thumb, sizeof(thumb));
        const int r = k->rarity >= 0 && k->rarity < g_viewer_rarity_count ? k->rarity : 0;
        sb_appendf(&sb, "<div class='bundlItmC' style='background:url(%s);background-size: contain;background-position: center;border: 3px solid %s;' onclick='viewItem(%ld)'></div>",
                   thumb, g_viewer_rarities[r].color, index);
        n++;
    }
    sb_append(&sb, "</div>");
    if (disc > 0) sb_appendf(&sb, "<div class='bundlPopDiscountLabel'>-%.0f%% OFF</div>", floor(disc + 0.5));
    char art[320], path[64];
    snprintf(path, sizeof(path), "img/backgrounds/bundles/b_%d.png", id);
    html_dom_asset_url(path, art, sizeof(art));
    sb_appendf(&sb, "<div class='bundlPopImg' style='background:url(%s);background-size: cover;'></div><div class='bundlPopCurncs'>", art);
    if (junk > 0) sb_appendf(&sb, "<div style='color:#cfcfcf'>+ <span style='color:#7d7d7d;'>%.2f</span> <span class='material-icons'>delete</span></div>", junk);
    sb_append(&sb, "</div><div class='bundlPopBuyH'>");
    if (disc > 0 && orig > 0) sb_appendf(&sb, "<div class='bundlPopOriginalPrice'>previously $%.2f</div>", orig);
    if (!owned) sb_appendf(&sb, "<div class='bundlPopBuyC%s' onclick='selectBundle(this, %d, false, undefined, undefined)'>BUY $%.15g USD</div>", disc > 0 ? " bundlPopBuy--discount" : "", id, price);
    if (owned) sb_appendf(&sb, "<div class='bundlPopBuyGift bundlPopBuyGift--owned' onclick='selectBundleGift(this, %d)'>GIFT $%.15g USD</div>", id, price);
    else sb_appendf(&sb, "<div class='bundlPopBuyGift' onclick='selectBundleGift(this, %d)'>GIFT</div>", id);
    sb_append(&sb, "</div>");
    char *html = sb_finish(&sb);
    ui_loadout_clear_pops(); // resetPops()
    set_display("popupHolder", "block");
    const HtmlElem pop = html_dom_by_id("bundlePop");
    if (pop) {
        html_dom_set_html(pop, html);
        html_dom_set_display(pop, "block");
    }
    free(html);
}

// selectBundleGift(el, id) (:212863-212892): "Gifting Bundle", the player's name (trimmed), then selectBundle(el, id, false, name)
static void bundle_gift_named(const int accepted, const char *text, void *user) {
    const int id = (int) (intptr_t) user;
    if (!accepted || !text) return;
    while (*text == ' ' || *text == '\t') text++;
    char name[64];
    snprintf(name, sizeof(name), "%s", text);
    for (size_t n = strlen(name); n > 0 && (name[n - 1] == ' ' || name[n - 1] == '\t'); n--) name[n - 1] = 0;
    if (name[0]) kio_checkout_bundle(id, name);
}

// iíïïìïî (AVAILABLE_BUNDLE_DATA): a bundle is OWNED when the available list does not hold its ID
static void bundles_owned(void) {
    if (!cJSON_IsArray(K.bundles) || !cJSON_IsArray(K.available)) return;
    cJSON *b;
    cJSON_ArrayForEach(b, K.bundles) {
        const cJSON *id = cJSON_GetObjectItem(b, "ID");
        int listed = 0;
        const cJSON *a;
        cJSON_ArrayForEach(a, K.available) listed |= cJSON_IsNumber(a) && cJSON_IsNumber(id) && a->valuedouble == id->valuedouble;
        cJSON_DeleteItemFromObject(b, "OWNED");
        cJSON_AddBoolToObject(b, "OWNED", !listed);
    }
}

// iîíïïïi(index, gift) (:220220-220232): premium's product, "xstp" its Xsolla request
static void premium_start(const int index, const char *gift) {
    const int days = store_ui_premium_days(index);
    if (days <= 0) return;
    Product p = {.kind = KP_PREMIUM, .display = -1, .quantity = 1, .index = index};
    snprintf(p.id, sizeof(p.id), "premium_%d", index);
    snprintf(p.name, sizeof(p.name), "Premium %d Days", days);
    if (gift) snprintf(p.gift, sizeof(p.gift), "%s", gift);
    start_product(&p);
}

void kio_checkout_premium_popup(const int index) { // premiumPopup(index) (:220470-220482)
    const int days = store_ui_premium_days(index);
    const char *price = store_ui_premium_price(index);
    if (days <= 0 || !price) return;
    // iiíïiíî (:16479), the first ten
    static const char *const benefits[] = {"Reduced Ads", "Golden Badge on Profile", "Custom Profile Pictures", "Custom Display Name",
                                           "Increased Customization Options", "Custom Skin, Hair & Chat Colors", "Access to Spray Wheel & +1 Spray limit",
                                           "Increased Max KR per Round to 40", "Custom Commands: /god /noclip /kill /smite", "Can host 16 player lobbies"};
    char img[256];
    html_dom_asset_url("img/prem_bg.png", img, sizeof(img));
    StrBuf sb = {0};
    sb_appendf(&sb, "<div class='bundlPopImg' style='background:url(%s);background-size: cover;background-position: center;'></div><div class='bundlPopSidH'>"
                    "<div class='bundlPopNam'>%d days of Premium</div><div class='bundlPopInfo'>",
               img, days);
    for (size_t i = 0; i < sizeof(benefits) / sizeof(benefits[0]); i++) {
        sb_append(&sb, "<li>");
        sb_append_esc(&sb, benefits[i]);
        sb_append(&sb, "</li>");
    }
    char gift[64];
    snprintf(gift, sizeof(gift), "%s", I18N("windows.store.gift", "Gift"));
    for (char *c = gift; *c; c++) *c = (char) (*c >= 'a' && *c <= 'z' ? *c - 32 : *c);
    sb_appendf(&sb, "<li>&amp; more</li></div><div class='bundlPopBuyH'><div class='bundlPopBuyC' onclick='selectPremium(%d)'>Buy - %s USD</div>"
                    "<div class='bundlPopBuyGift' onclick='selectPremiumGift(this, %d)'>%s</div></div></div>",
               index, price, index, gift);
    char *html = sb_finish(&sb);
    ui_loadout_clear_pops(); // resetPops()
    set_display("popupHolder", "block");
    const HtmlElem pop = html_dom_by_id("premiumPop");
    if (pop) {
        html_dom_set_html(pop, html);
        html_dom_set_display(pop, "block");
    }
    free(html);
}

static void premium_gift_named(const int accepted, const char *text, void *user) { // giftPremium(index, name.trim())
    const int index = (int) (intptr_t) user;
    if (!accepted || !text) return;
    while (*text == ' ' || *text == '\t') text++;
    char name[64];
    snprintf(name, sizeof(name), "%s", text);
    for (size_t n = strlen(name); n > 0 && (name[n - 1] == ' ' || name[n - 1] == '\t'); n--) name[n - 1] = 0;
    if (name[0] && official() && signed_in()) premium_start(index, name);
}

void kio_checkout_bp_tier(const int tier, const char *gift_to) { // selectBPTier (:212948-212966)
    if (!official() || !signed_in()) return;
    char id[32];
    snprintf(id, sizeof(id), "battlepass_%d", tier);
    wait_begin("xsolla", id, 0);
    send_request(krunker_checkout_bp_tier(tier, gift_to && gift_to[0] ? gift_to : NULL, i18n_locale()));
}

void kio_checkout_bp_levels(const int levels) { // skipLevels (:212993-213011)
    if (!official() || !signed_in()) return;
    char id[40];
    snprintf(id, sizeof(id), "battlepass_levels_%d", levels);
    wait_begin("xsolla", id, 0);
    send_request(krunker_checkout_bp_levels(levels, i18n_locale()));
}

// ---------------------------------------------------------------- the server's answers

static void after_success(void) {
    browser_end();
    kio_inventory_load(NULL, NULL); // the items the purchase gave (the official inventory)
    menu_rerender_if_open(14); // updateWindow(null, true): the Store
    menu_rerender_if_open(5);
    purchase_load_done();
}

static void success_kr(const char *provider, const cJSON *funds, const int pack) { // iïíîííï's success (:212164-212173)
    const int val = kr_pack_val(pack);
    if (active_account() && cJSON_IsNumber(funds)) {
        char amount[32], text[160];
        v10_fmt_int(amount, sizeof(amount), val);
        I18N_F(text, "purchase.kr.success", "Added {0} KR to your account", amount);
        io_popup("SUCCESS", NULL, text);
        account_set_funds(funds->valuedouble);
    }
    printf("[checkout] %s purchase of %d KR done\n", provider ? provider : "?", val);
    after_success();
}

// The success line of a product purchase (iîíïíïí :212198-212245; the alreadyClaimed claim the same, deob-10.0.6.js:213784-213789)
static void product_message(const char *pid, const char *name, const char *gift) {
    char text[256];
    if (!strncmp(pid, "username-renew:", 15)) { // the renewal's days: v10's second argument, not sent with the answer here
        I18N_F(text, "account.usernames.renew.success", "{0} renewed for {1} days", name ? name : "", "");
        io_popup("SUCCESS", NULL, text);
    } else if (!strncmp(pid, "username:", 9)) {
        I18N_F(text, "account.usernames.buy.success", "{0} added to your usernames", name ? name : "");
        io_popup("SUCCESS", NULL, text);
    } else if (gift && gift[0]) {
        I18N_F(text, "windows.store.gift.sentTo", "{0} has received '{1}'.", gift, name ? name : "");
        io_popup("SUCCESS", NULL, text);
    } else if (active_account()) {
        I18N_F(text, "shop.popup.success", "{0} purchased successfully!", name ? name : "");
        io_popup("SUCCESS", NULL, text);
    }
}

// A display purchase counts on the display at once, unless it was a gift (:213596, :213783: !giftName && displayId > 0)
static void count_display(const char *gift, const int display_id) {
    if ((!gift || !gift[0]) && display_id > 0) kio_store_display_purchased(display_id);
}

static void success_product(const char *provider, const char *product_id, const char *name, const char *gift, const int display_id) {
    const char *pid = product_id ? product_id : "";
    count_display(gift, display_id); // updateDisplayPurchaseCount before the result (deob-10.0.6.js:213596)
    product_message(pid, name, gift);
    if (strstr(pid, "wildoffer:")) v10_wild_offer_purchased(); // the offer is spent (:212235)
    printf("[checkout] %s purchase of %s done%s%s\n", provider ? provider : "?", pid, gift && gift[0] ? " as a gift to " : "", gift && gift[0] ? gift : "");
    after_success();
}

// A Stripe claim answered alreadyClaimed (v10.0.6, deob-10.0.6.js:213778-213790): the purchase was granted before (an
// earlier claim of the same payment), so nothing is granted again: the waiting ends (iíìiiìï :213729), the display counts
// it and the result line shows; with no product name nothing is shown.
static void claimed_again(const char *product_id, const char *name, const char *gift, const int display_id) {
    browser_end();
    purchase_load_done();
    if (!name || !name[0]) return;
    count_display(gift, display_id);
    product_message(product_id ? product_id : "", name, gift);
    printf("[checkout] stripe claim of %s was already granted\n", product_id ? product_id : "?");
}

static void success_premium(const int premium, const char *gift) { // iîiiïíí (:212263-212281)
    const int days = store_ui_premium_days(premium);
    if (gift && gift[0]) {
        char what[64], text[192];
        snprintf(what, sizeof(what), "%d", days);
        char daystext[96];
        I18N_F(daystext, "windows.store.premium.days", "{0} days of Premium", what);
        I18N_F(text, "windows.store.gift.sentTo", "{0} has received '{1}'.", gift, daystext);
        io_popup("SUCCESS", NULL, text);
    } else if (active_account() && days > 0) {
        io_popup("SUCCESS", NULL, "Premium Purchased");
    }
    printf("[checkout] premium %d days done%s%s\n", days, gift && gift[0] ? " as a gift to " : "", gift && gift[0] ? gift : "");
    after_success();
}

static void success_bundle(const int bundle, const cJSON *funds, const cJSON *junk, const int gifted, const char *gift) { // iìiïíîi (:212283-212327)
    if (gifted) {
        char text[160];
        if (gift && gift[0]) snprintf(text, sizeof(text), "Bundle Gifted to %s!", gift);
        else snprintf(text, sizeof(text), "Bundle Purchased and Gifted");
        io_popup("SUCCESS", NULL, text);
    } else if (active_account()) {
        if (cJSON_IsNumber(funds)) account_set_funds(funds->valuedouble);
        (void) junk; // the junk balance: the account's next "uf" carries it
        io_popup("SUCCESS", NULL, "Bundle Purchased");
    }
    printf("[checkout] bundle %d done\n", bundle);
    after_success();
}

static const cJSON *arg(const cJSON *frame, const int i) { return cJSON_GetArrayItem(frame, i); }
static const char *arg_str(const cJSON *frame, const int i) { return cJSON_GetStringValue(arg(frame, i)); }
static int arg_int(const cJSON *frame, const int i) { return cJSON_IsNumber(arg(frame, i)) ? arg(frame, i)->valueint : 0; }

// LOAD_XSOLLA ["xst", token, sandbox] (iíìííìî :212041): PayStation in the browser; the widget's INIT closes the selector
static void on_token(const cJSON *frame) {
    const char *token = arg_str(frame, 1);
    if (!K.requested || !token || !token[0]) return;
    const cJSON *sb = arg(frame, 2);
    const int sandbox = cJSON_IsTrue(sb) || (cJSON_IsNumber(sb) && sb->valuedouble != 0);
    char url[1024];
    if (!krunker_checkout_paystation_url(token, sandbox, url, sizeof(url))) {
        purchase_error(K.provider, I18N("purchase.openFailed", "Checkout couldn't open. Check your connection or ad blocker and try again."), K.product_id);
        return;
    }
    char product[96];
    snprintf(product, sizeof(product), "%s", K.product_id);
    close_if_waiting();
    K.requested = 0; // one page per request: a stray second token opens nothing
    browser_begin("xsolla", product, 1);
    ui_open_browser(url);
    memset(url, 0, sizeof(url));
    KN_LOG_VERBOSE2("[checkout] PayStation opened in the browser for %s%s\n", product, sandbox ? " (sandbox)" : "");
}

static void frame_listen(const char *label, const cJSON *frame) {
    if (!official()) return;
    if (!strcmp(label, "abd")) { // ALL_BUNDLE_DATA [rows] (iìîíííí): the store's bundle grid
        cJSON_Delete(K.bundles);
        const cJSON *rows = arg(frame, 1);
        K.bundles = cJSON_IsArray(rows) ? cJSON_Duplicate(rows, 1) : NULL;
        bundles_owned();
        menu_rerender_if_open(14);
    } else if (!strcmp(label, "gbdr")) { // AVAILABLE_BUNDLE_DATA [available ids, ...] (iíïïìïî)
        cJSON_Delete(K.available);
        const cJSON *ids = arg(frame, 1);
        K.available = cJSON_IsArray(ids) ? cJSON_Duplicate(ids, 1) : NULL;
        bundles_owned();
        menu_rerender_if_open(14);
    } else if (!strcmp(label, "xst")) {
        on_token(frame);
    } else if (!strcmp(label, "pur")) { // [provider, err, funds, pIndex, price, currency, txn, ext]
        browser_end();
        if (err_set(arg(frame, 2))) {
            char id[32];
            snprintf(id, sizeof(id), "kr_%d", kr_pack_val(arg_int(frame, 4)));
            purchase_error(arg_str(frame, 1), err_text(arg(frame, 2)), id);
        } else success_kr(arg_str(frame, 1), arg(frame, 3), arg_int(frame, 4));
    } else if (!strcmp(label, "ppr")) { // [provider, err, productId, name, displayId, items, premiumT, sub, price, cur, txn, ext, giftName]
        browser_end();
        if (err_set(arg(frame, 2))) purchase_error(arg_str(frame, 1), err_text(arg(frame, 2)), arg_str(frame, 3));
        else success_product(arg_str(frame, 1), arg_str(frame, 3), arg_str(frame, 4), arg_str(frame, 13), arg_int(frame, 5));
    } else if (!strcmp(label, "purprm")) { // [provider, err, premium, premiumT, price, cur, txn, ext, _, giftName]
        browser_end();
        if (err_set(arg(frame, 2))) {
            char id[32];
            snprintf(id, sizeof(id), "premium_%d", store_ui_premium_days(arg_int(frame, 3)));
            purchase_error(arg_str(frame, 1), err_text(arg(frame, 2)), id);
        } else success_premium(arg_int(frame, 3), arg_str(frame, 10));
    } else if (!strcmp(label, "purb")) { // [provider, err, funds, junk, skins, bundle, price, cur, txn, ext, gifted, giftName]
        browser_end();
        if (err_set(arg(frame, 2))) {
            char id[32];
            snprintf(id, sizeof(id), "bundle_%d", arg_int(frame, 6));
            purchase_error(arg_str(frame, 1), err_text(arg(frame, 2)), id);
        } else success_bundle(arg_int(frame, 6), arg(frame, 3), arg(frame, 4), cJSON_IsTrue(arg(frame, 11)), arg_str(frame, 12));
    } else if (!strcmp(label, "purbp") || !strcmp(label, "purbpl")) { // windows[5].purchaseRes / purchaseLvlRes
        browser_end();
        purchase_load_done();
        battlepass_ui_frame(label, frame);
    } else if (!strcmp(label, "pErr")) { // purchaseError(provider, err, product)
        browser_end();
        purchase_error(arg_str(frame, 1), err_text(arg(frame, 2)), arg_str(frame, 3));
    } else if (!strcmp(label, "uprm")) { // UPDATE_PREMIUM_AMOUNT (iìíììíî :212469): the account's premium time
        menu_rerender_if_open(5);
    }
}

// ---------------------------------------------------------------- handlers

static double num_arg(const HtmlArg *args, const int argc, const int i, const double def) {
    if (!args || i >= argc) return def;
    if (args[i].kind == HTML_ARG_NUM || args[i].kind == HTML_ARG_BOOL) return args[i].num;
    if (args[i].kind == HTML_ARG_STR && args[i].str && args[i].str[0]) return atof(args[i].str);
    return def;
}

static const char *str_arg(const HtmlArg *args, const int argc, const int i) {
    return args && i < argc && args[i].kind == HTML_ARG_STR ? args[i].str : NULL;
}

int kio_checkout_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) ev;
    if (!name) return -1;
    if (!strcmp(name, "XsollaPopup")) { // XsollaPopup(method, flag): Box Office does nothing (no payments)
        xsolla_popup(num_arg(args, argc, 0, -1), str_arg(args, argc, 1));
        return 1;
    }
    if (!strcmp(name, "StripePopup")) {
        stripe_popup();
        return 1;
    }
    if (!strcmp(name, "purchaseCancel")) { // #purchaseBack: purchaseHolder.classList.contains("waiting") || purchaseCancel()
        if (!K.wait) purchase_cancel(K.has_product ? "selector" : "xsolla", K.has_product ? K.product.id : NULL);
        return 1;
    }
    if (!strcmp(name, "closePurchaseSelector")) {
        close_selector();
        return 1;
    }
    if (!strcmp(name, "cancelPurchase")) { // the error's "click here": #purchaseResponse goes (:212330)
        set_display("purchaseResponse", "none");
        if (K.error_shown) {
            K.kr_index = 0;
            purchase_load_done();
            K.error_shown = 0;
        }
        return 1;
    }
    if (!official()) return -1;
    if (!strcmp(name, "knCheckoutDone")) { // the wait popup's Done: read the account again; nothing credited = confirmSlow
        if (!K.browser) return 1;
        K.done_pressed = 1;
        kio_inventory_load(NULL, NULL);
        if (K.watch) poll_funds();
        else browser_unconfirmed();
        return 1;
    }
    if (!strcmp(name, "selectBundle")) { // selectBundle(el, id, survey, gift, steam) (:212892-212935)
        const char *gift = str_arg(args, argc, 3);
        if (signed_in()) kio_checkout_bundle((int) num_arg(args, argc, 1, 0), gift && gift[0] ? gift : NULL);
        return 1;
    }
    if (!strcmp(name, "selectBundleGift")) { // selectBundleGift(el, id): the name prompt
        const int id = (int) num_arg(args, argc, 1, 0);
        if (id > 0 && signed_in())
            ui_prompt("Enter the player name below to send them this bundle. We will let them know you have gifted it to them.", bundle_gift_named,
                      (void *) (intptr_t) id);
        return 1;
    }
    if (!strcmp(name, "selectPremium")) { // selectPremium(index) (:220415)
        if (signed_in()) premium_start((int) num_arg(args, argc, 0, -1), NULL);
        return 1;
    }
    if (!strcmp(name, "selectPremiumGift")) { // selectPremiumGift(el, index) (:220440): the gift form, then giftPremium
        const int index = (int) num_arg(args, argc, argc > 1 ? 1 : 0, -1);
        const int days = store_ui_premium_days(index);
        if (days <= 0 || !signed_in()) return 1;
        char daystext[96], what[16], desc[256];
        snprintf(what, sizeof(what), "%d", days);
        I18N_F(daystext, "windows.store.premium.days", "{0} days of Premium", what);
        I18N_F(desc, "windows.store.gift.desc", "Enter the name of the player you want to send '{0}' to. We will let them know you gifted it.", daystext);
        ui_prompt(desc, premium_gift_named, (void *) (intptr_t) index);
        return 1;
    }
    if (!strcmp(name, "giftPremium")) { // giftPremium(index, name)
        const char *who = str_arg(args, argc, 1);
        if (who && who[0] && signed_in()) premium_start((int) num_arg(args, argc, 0, -1), who);
        return 1;
    }
    return -1;
}

// The client's window (GLFW's focus callback) or the page (focus / visibilitychange, web_dom.c) is back: while a payment
// waits for the browser, the account is read again (the Done button's read, without ending the wait)
void kio_checkout_window_focused(void) {
    if (!K.browser || !official()) return;
    kio_inventory_load(NULL, NULL);
    if (K.watch) poll_funds();
}

void kio_checkout_tick(void) {
    kio_checkout_stripe_tick();
    if (!K.browser || !K.watch) return;
    const AccountInfo *acc = official() ? active_account() : NULL;
    if (acc && krunker_checkout_wait_funds(&g_wait, acc->funds) == KRUNKER_CHECKOUT_WAIT_PAID) { // the server's "uf" push
        browser_paid(acc->funds);
        return;
    }
    switch (krunker_checkout_wait_tick(&g_wait, html_timer_now_ms())) {
        case KRUNKER_CHECKOUT_WAIT_POLL: poll_funds(); break;
        case KRUNKER_CHECKOUT_WAIT_EXPIRED: browser_unconfirmed(); break;
        default: break;
    }
}
