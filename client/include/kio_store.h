#pragma once
// The Store's krunker.io side (client/src/ui/kio_store.c): while krunker.io is the active account
// backend (backend_ui.h account_backend() == ACCOUNT_BACKEND_KRUNKER), the store windows show
// krunker.io's own catalogue, loaded the way Krunker v10.0.2 loads it, through its API client
// (krunker_account_api, KRUNKER_SVC_API: v10's _10449, base GAPI_URI) with the account's token:
//   shop.getDisplayByTag(tag)  GET shop/displays?tag=<tag>   v10:26524, its loader _38951 (v10:113896)
//                              with the shop store _38905; the Eterno Shop tab loads "limited"
//                              (v10:128305) and renders it with the shop front _43892 (v10:126863)
//   ranked.getShop()           GET ranked/shop               v10:26380, _37760 (v10:111020): data.skins
// Strictly read-only: nothing that spends KR / RP / JNK or changes the account (purchases, gifts,
// spins, vouchers, ranked purchases, market listings and trades) is sent to krunker.io from this
// client; those buttons keep v10's guest behaviour (loginOrRegister) and otherwise say so
// (kio_store_read_only). Box Office (krunker.io not active) is untouched by all of it.
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
// A krunker.io purchase / spin / redeem / trade button in krunker.io mode: v10's own guest behaviour
// (loginOrRegister) for a guest, else the read-only notice (v10's error io-popup). Nothing is sent.
void kio_store_read_only(const char *what); // what: the request it stands for (logged)
const char *kio_store_read_only_text(void); // the notice's text (windows with their own error modal)
// --render-scenario: state.kioStore {limited: <display>, ranked: {skins}} as krunker.io's answers
void kio_store_scenario(const cJSON *state);
int  kio_store_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, int argc); // knKioShop*; -1 = not ours
