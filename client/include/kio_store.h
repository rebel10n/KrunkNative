#pragma once
// The Store's krunker.io side (client/src/ui/kio_store.c): while krunker.io is the active account
// backend (backend_ui.h account_backend() == ACCOUNT_BACKEND_KRUNKER), the store windows show
// krunker.io's own catalogue, loaded the way Krunker v10.0.2 loads it, through its API client
// (krunker_account_api, KRUNKER_SVC_API: v10's _10449, base GAPI_URI) with the account's token:
//   shop.getDisplayByTag(tag)  GET shop/displays?tag=<tag>   v10:26524, its loader _38951 (v10:113896)
//                              with the shop store _38905; the Eterno Shop tab loads "limited"
//                              (v10:128305) and renders it with the shop front _43892 (v10:126863)
//   ranked.getShop()           GET ranked/shop               v10:26380, _37760 (v10:111020): data.skins
// Official: the account actions (store purchases, ranked purchases, KR gifts, trades, item locks, the market)
// are sent to krunker.io with the account's Bearer token through kio_store_write, as v10 sends them
// (deob-10.0.4.js:26150 iìîííìì; the request shapes cite at each caller). A guest gets v10's loginOrRegister
// (the purchase buttons). Box Office (krunker.io not active) is untouched by all of it.
#include <ui_logic.h> // StrBuf
#include <html_dom.h>
#include <cJSON.h>

struct V10Req;

int  kio_store_active(void);              // krunker.io is the active backend: the store is krunker.io's
// The Eterno Shop tab's body (v10 svelte-gh77t9: the header, then loading / error / the shop front of
// krunker.io's "limited" display). Starts the request when nothing is cached.
void kio_store_eterno(StrBuf *sb);
// ranked.getShop()'s data ({skins: [...]}, each row's expires_at as epoch ms) in the shape of the
// brain's /mm/ranked/shop answer, for ranked_ui.c's Shop tab. Never NULL.
const struct V10Req *kio_store_ranked_shop(void);
// The brain's Junk-priced display (junk_shop_ui.c) in the shop's markup: the display's "products" map is read
// from it. Gift buttons are left out; a card's BUY is junk_shop_buy on Box Office.
void kio_store_display_html(StrBuf *sb, const cJSON *display);
// An answer of a krunker.io write: data is the answer's data (NULL when there is none, e.g. 204), and
// code / message the error (NULL on success). A {code, message} error's code is its i18n key (iíìïïïì,
// deob-10.0.4.js:104310); the callee copies what it keeps, data is freed after the call.
typedef void (*KioWriteFn)(void *user, int status, const cJSON *data, const char *code, const char *message);
// A krunker.io request with the account's Bearer token (krunker_account_api, KRUNKER_SVC_API). fn runs once,
// also when the request is refused at once (status -1). Returns 1 when the request started.
int kio_store_write(const char *method, const char *path, const char *body, KioWriteFn fn, void *user);
// Buys one product of a display (shop.purchaseProduct, deob-10.0.4.js:116077, quantity 1). gift_to: the
// recipient for a gift (:117751), NULL for the account itself. A guest gets loginOrRegister instead. 1 = sent.
int kio_store_buy(int display_id, int product_id, const char *gift_to);
// updateDisplayPurchaseCount (deob-10.0.6.js:115979-116030): the "limited" display's node and its product count one more
// purchase now, so a bought limited card reads sold out at once (the store re-renders when it is open). Also called for
// the checkout's paid catalogue purchases and a Stripe claim answered alreadyClaimed (kio_checkout.c).
void kio_store_display_purchased(int display_id);
// Spins a store wheel: POST spins/execute {index, ticket, multi, from: 0} (deob-10.0.4.js:27413; the spin button
// :221350). index / ticket are -1 for null (a TICKET item's spin has no wheel). fn gets the answer's data
// (fakeRands, itemIds, funds, usedTicket) or the error, as kio_store_write does. 1 = sent.
int kio_store_spin(int index, int ticket, int multi, KioWriteFn fn, void *user);
// Redeem Keys (the shop's tab 4, voucherPopup deob-10.0.4.js:221051): asks for a KR code and sends it over the game
// socket as "vo" [code]. The account's own sign-in rule applies (a guest gets loginOrRegister).
void kio_store_redeem_prompt(void);
// The game socket's "vr" answer (REDEEM_VOUCHER_RESPONSE, deob-10.0.4.js:221042): funds when has_funds, and the
// message, shown with its HTML stripped.
void kio_store_voucher_answer(int has_funds, double funds, const char *message);
// A krunker.io action KrunkNative does not send yet: the notice says so (never a read-only refusal).
void kio_store_unwired(const char *what); // what: the request it stands for (logged)
const char *kio_store_unwired_text(void); // the notice's text
// --render-scenario: state.kioStore {limited: <display>, ranked: {skins}} as krunker.io's answers
void kio_store_scenario(const cJSON *state);
int  kio_store_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, int argc); // knKioShop*; -1 = not ours
