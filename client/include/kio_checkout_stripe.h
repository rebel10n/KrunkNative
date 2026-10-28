#pragma once
// The Stripe side of a krunker.io real-money purchase (client/src/ui/kio_checkout_stripe.c), called by
// kio_checkout.c when the player picks Stripe in v10's purchase selector (stripePayB, deob-10.0.4.js:212383-212384;
// shown when canShowStripe: the web site, not Steam / Epic / the Microsoft PWA, index.html).
// The bundle's flow (iïíîíìï, deob-10.0.4.js:212420-212470): FRVR's iap.checkout(body) -> data.payment ->
// FRVR.iap.purchase(payment) (the FRVR SDK's card popup) -> iap.claim(channelId || "stripe", productId, purchaseId)
// (:212404-212418) -> the funds / bundle / premium / gift / username result. Here the requests are C (REST to
// api.krunker.io and FRVR's wallet with the krunker.io account's FRVR token) and only Stripe's hosted Checkout
// page is shown, in the system browser (ui_open_browser): never krunker.io's page, the FRVR SDK or a webview.
// The findings, request shapes and the one missing piece (a crucible iap service): doc/v10/checkout-stripe.md.
#include <cJSON.h>

enum {
    KIO_STRIPE_DONE = 0,     // claimed: claim is the claim answer's data ({funds, pack, price} / {bundle} / {premium} / ...)
    KIO_STRIPE_CANCELLED = 1, // the player closed the payment page
    KIO_STRIPE_ERROR = 2,     // error: the message to show (purchaseError's)
};
// Called once per begin, on the main thread. claim is valid only during the call.
typedef void (*KioStripeDone)(void *user, int outcome, const cJSON *claim, const char *error);

// 1 = Stripe can be offered in this build and for this account (the selector shows stripePayB).
int kio_checkout_stripe_available(void);
// body: iap.checkout's body as the bundle builds it ({pack} / {bundle, giftPlayerName} / {premium, giftPlayerName} /
// {productId, display, quantity, giftPlayerName}, :212430-212446). 1 = started (done runs later); 0 = not started
// (done already ran with KIO_STRIPE_ERROR).
int kio_checkout_stripe_begin(const cJSON *body, KioStripeDone done, void *user);
// Once per frame while a purchase runs (the page's messages / redirects, the requests' answers).
void kio_checkout_stripe_tick(void);
// Ends a running purchase without calling done (the player left the store, the account changed).
void kio_checkout_stripe_close(void);
