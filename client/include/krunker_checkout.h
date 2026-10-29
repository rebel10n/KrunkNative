#pragma once
// krunker.io real-money purchases over the official game socket (client/src/ui/kio_checkout.c drives them): the
// request frames v10 sends, their shapes (krunker_net_send_account_frame's allowlist), the S2C answers' labels and
// the payment provider's page. Pure (no UI, no socket): client/tests/kio_checkout_test.c.
//
// deob-10.0.4.js: XsollaPopup (:212333-212349) "xst" [pIndex, lang, a, b, paymentsChannelId] for a KR pack;
// iïiïïíi (:212391-212397) "xpurchase" [id, lang, a || null, b || null, channelId, display, quantity || 1, gift] for a
// catalogue product (a gift, a wild offer); the products' own senders: "xstb" [bundle, lang, a, b, channelId, gift]
// (:212927), "xstp" [premium, lang, a, b, channelId, gift || null] (:220228), "xstbp" [tier, lang, null, null,
// channelId, gift || null] (:212959), "xstbpl" [levels, lang, null, null, channelId] (:213007); "xsin" [invoice] when
// PayStation reports a payment done (:212025-212028). paymentsChannelId is null outside the Microsoft PWA (index.html:
// window.paymentsChannelId = isMsPwa ? "microsoft_pwa" : null), so it is always null here.
// The answer to a request is S2C LOAD_XSOLLA "xst" [token, sandbox] (:212041, the S2C table :226151): PayStation's
// page, https://secure.xsolla.com/paystation2/?access_token=<token> (sandbox: sandbox-secure.xsolla.com; the widget's
// getPaystationUrl, static.xsolla.com/embed/paystation/1.0.7/widget.min.js). The purchase's outcome: "pur" (a KR
// pack, :212158), "ppr" (a product, :212193), "purprm" (premium, :212263), "purb" (a bundle, :212283), "purbp" /
// "purbpl" (the battle pass, windows[5].purchaseRes / purchaseLvlRes), "pErr" (purchaseError, :213229), "uprm"
// (the premium time, :212469).
#include <cJSON.h>
#include <stddef.h>

typedef struct {
    const char *lang; // the player's language (settings lang: i18n_locale)
    double method;    // XsollaPopup's first argument: a PayStation method (3175 Apple Pay, 3431 Google Pay, 2682 crypto); < 0 none
    const char *flag; // its second ("paybycash"); NULL none
} KrunkerCheckoutOpts;

// The request frames (["name", args...]; the caller deletes them). display < 0 / gift NULL / quantity < 1 are v10's
// null / null / 1. NULL when an argument is out of shape (an empty id, a negative index).
cJSON *krunker_checkout_kr(int pack, const KrunkerCheckoutOpts *o);                                                   // "xst"
cJSON *krunker_checkout_product(const char *id, int display, int quantity, const char *gift, const KrunkerCheckoutOpts *o); // "xpurchase"
cJSON *krunker_checkout_bundle(int bundle, const char *gift, const KrunkerCheckoutOpts *o);                             // "xstb"
cJSON *krunker_checkout_premium(int premium, const char *gift, const KrunkerCheckoutOpts *o);                           // "xstp"
cJSON *krunker_checkout_bp_tier(int tier, const char *gift, const char *lang);                                          // "xstbp"
cJSON *krunker_checkout_bp_levels(int levels, const char *lang);                                                        // "xstbpl"
cJSON *krunker_checkout_invoice(double invoice);                                                                        // "xsin"

// 1 = one of the frames above, in its shape (each argument's type and count): what the game socket may carry.
int krunker_checkout_frame_ok(const cJSON *frame);
// 1 = an S2C frame kio_checkout.c takes: a purchase answer (xst, pur, ppr, purprm, purb, purbp, purbpl, pErr, uprm) or
// the bundle store's data (ALL_BUNDLE_DATA "abd" [rows], AVAILABLE_BUNDLE_DATA "gbdr" [available ids, ...]: the server
// pushes them, deob-10.0.4.js: iìîíííí / iíïïìïî, the S2C table :226151).
int krunker_checkout_label(const char *label);

// showPurchase("catalog:{\"display\":N}|price_ref", ...) (deob-10.0.4.js:117793, :213060-213105): the display (-1 when
// null / absent) and the product's price_ref. 1 = parsed.
int krunker_checkout_parse_catalog(const char *spec, int *display, char *ref, size_t n);

// The wait for a payment made in the system browser (KrunkNative; doc/v10/checkout-browser.md): PayStation's widget
// STATUS_DONE never reaches the client, so the server's answer ("pur" / "ppr" / ...) ends it when it comes; meanwhile
// the account's funds are read every KRUNKER_CHECKOUT_POLL_MS and a rise above the funds at the start is the payment
// credited. After KRUNKER_CHECKOUT_WAIT_MS the wait ends unconfirmed (purchase.confirmSlow). now: ms, any clock.
#define KRUNKER_CHECKOUT_POLL_MS 5000.0
#define KRUNKER_CHECKOUT_WAIT_MS 600000.0
typedef struct {
    int active;
    double start_ms, next_poll_ms;
    int has_baseline;
    double baseline; // the account's funds when the page opened (or at the first answer)
} KrunkerCheckoutWait;
enum { KRUNKER_CHECKOUT_WAIT_IDLE = 0, KRUNKER_CHECKOUT_WAIT_POLL, KRUNKER_CHECKOUT_WAIT_PAID, KRUNKER_CHECKOUT_WAIT_EXPIRED };
void krunker_checkout_wait_begin(KrunkerCheckoutWait *w, double now, int has_funds, double funds);
// Per frame: POLL when a funds read is due (the next one is scheduled), EXPIRED once (the wait ends), else IDLE.
int krunker_checkout_wait_tick(KrunkerCheckoutWait *w, double now);
// The account's funds as read (a poll's answer, or the server's own "uf" push): PAID when they rose above the baseline
// (the wait ends); the first funds known become the baseline. IDLE otherwise (also when no wait runs).
int krunker_checkout_wait_funds(KrunkerCheckoutWait *w, double funds);
void krunker_checkout_wait_end(KrunkerCheckoutWait *w);
// PayStation's page for the "xst" answer's token. 1 = written (the token is percent-encoded; never log the URL).
int krunker_checkout_paystation_url(const char *token, int sandbox, char *out, size_t n);

// A URL as a log may show it (ui_open_browser): the fragment becomes "#(not logged)" and the value of every secret query
// parameter (access_token, token, at, key, secret, code, password, session, sid, auth) "(not logged)"; the rest (?game=)
// stays. Always NUL-terminates out (n > 0).
void krunker_checkout_log_url(const char *url, char *out, size_t n);
