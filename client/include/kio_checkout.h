#pragma once
// krunker.io real-money purchases (client/src/ui/kio_checkout.c), run in C: the purchase selector of the page
// (#purchaseHolder: Credit/Debit Card = Xsolla, the Stripe button, Apple / Google Pay, cash, crypto; ui/index.html), the
// game socket's requests and answers (krunker_checkout.h), the waiting popup and its timeouts, and the outcome's popups.
// The payment provider's own page (Xsolla PayStation; Stripe through kio_checkout_stripe.h) opens in the player's
// system browser (ui_open_browser): never krunker.io's page, its JS or a webview in the client. The bundle learns a
// PayStation payment is done from the widget's STATUS_DONE (deob-10.0.4.js:212025-212028); a browser page cannot tell
// the client, so the outcome is the server's answer ("pur", "ppr", "purprm", "purb", "purbp", "purbpl", "pErr") when it
// sends one, or the account's funds rising (the server credits a paid payment: its "uf" push, or /player/me read every
// 5 s, krunker_checkout_wait_*); the wait popup's Done reads the account (funds, the official inventory) again, and
// after 10 minutes the wait ends on purchase.confirmSlow (doc/v10/checkout-browser.md).
// Only while krunker.io is the active account (kio_store_active); a guest gets showWindow(5). The game socket must be
// open (an official game): the requests go over it, as v10 sends them.
#include <html_dom.h>
#include <cJSON.h>

// showPurchase(index): a KR pack (store.purchases[index], deob-10.0.4.js:213019-213056): the selector.
void kio_checkout_kr_pack(int index);
// windows[13].directPurchase(index) (:217616-217619): showPurchase(index), then XsollaPopup() at once.
void kio_checkout_kr_pack_direct(int index);
// showPurchase("catalog:{display}|price_ref", _, quantity) -> showProductPurchase (:213106-213136): a real-priced catalogue
// product (price_ref: its price's reference, the product id v10 sends; name: the product's name).
void kio_checkout_catalog(const char *price_ref, const char *name, int display, int quantity);
// giftProduct(price_ref, display, name) (:213137-213161): a real-priced product as a gift to gift_to.
void kio_checkout_gift(const char *price_ref, const char *name, int display, const char *gift_to);
// showPurchase("catalog:{\"display\":N}|price_ref", _, quantity) as the page passes it (krunker_checkout_parse_catalog).
void kio_checkout_catalog_ref(const char *spec, int quantity);
// showPurchase("wildoffer:" + id) -> showWildOfferPurchase (:213162-213187): a real-priced wild offer.
void kio_checkout_wild_offer(int offer_id);
// selectBundle(_, id, _, gift) (:212892-212935): bundle id's product {id: "bundle_" + id, bundle: id}, "xstb" its Xsolla
// request. gift_to NULL: the account's own.
void kio_checkout_bundle(int bundle, const char *gift_to);
// The official store's bundles (ALL_BUNDLE_DATA "abd", OWNED from AVAILABLE_BUNDLE_DATA "gbdr": the IDs not listed are
// owned, deob-10.0.4.js iíïïìïî): v10's bundle rows for the Store's grid (junk_shop_bundles_render). NULL until the
// game server sent them.
const cJSON *kio_checkout_bundles(void);
// bundlePopup(true, false, id) (:212740-212862): the bundle's popup (#bundlePop) with BUY (selectBundle) and GIFT
// (selectBundleGift: the player's name, then "xstb" with it).
void kio_checkout_bundle_popup(int bundle);
// The window (GLFW's focus callback, main.c) or the page (focus / visibilitychange, web_dom.c) got the focus back: a
// payment waiting for the browser reads the account again (as the Done button, the wait goes on). Headless: never.
void kio_checkout_window_focused(void);
// premiumPopup(index) (:220470-220482): the Premium offer's popup (Buy / Gift), store.premium[index].
void kio_checkout_premium_popup(int index);
// selectBPTier(_, tier, _, gift) (:212948-212966) / skipLevels (:212993-213011): the battle pass's Xsolla requests.
void kio_checkout_bp_tier(int tier, const char *gift_to);
void kio_checkout_bp_levels(int levels);

// The page's handlers: XsollaPopup, StripePopup, purchaseCancel, cancelPurchase, closePurchaseSelector, selectPremium,
// selectPremiumGift, giftPremium. Box Office keeps v10's empty XsollaPopup and the selector's close. -1 = not ours.
int kio_checkout_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, int argc);
// Per frame, main thread (krunker_net_tick): the Stripe side's work.
void kio_checkout_tick(void);
