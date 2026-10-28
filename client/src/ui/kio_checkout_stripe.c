// The Stripe side of a krunker.io purchase (client/include/kio_checkout_stripe.h). Not available yet: the
// selector hides stripePayB and Xsolla (kio_checkout.c) is the card payment.
#include <kio_checkout_stripe.h>

int kio_checkout_stripe_available(void) { return 0; }

int kio_checkout_stripe_begin(const cJSON *body, const KioStripeDone done, void *user) {
    (void) body;
    if (done) done(user, KIO_STRIPE_ERROR, NULL, "Stripe is not available");
    return 0;
}

void kio_checkout_stripe_tick(void) {}

void kio_checkout_stripe_close(void) {}
