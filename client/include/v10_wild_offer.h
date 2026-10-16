#pragma once
// Krunker v10.0.2's Wild Offers (client/src/ui/v10_wild_offer.c; kn_brain services/brain/wild_offer.c):
// the account's discounted item of the day in the notification center, the Store's shop front and
// the item view, bought or refreshed for KR over the social socket ("wo" / "wob" / "wor").
#include <client.h>
#include <cJSON.h>
#include <html_dom.h>
#include <ui_logic.h>

void v10_wild_offer_tick(const Client *client);  // per menu frame: fetch after sign-in (and notify once per offer)
void v10_wild_offer_store_opened(void);          // the Store opened: fetch again when older than 5 min
int  v10_wild_offer_frame(const char *label, const cJSON *frame); // "wo" / "wob" / "wor"; 0 = not ours
void v10_wild_offer_shop(StrBuf *sb);            // the shop front's wild offer item (nothing without an offer)
int  v10_wild_offer_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, int argc); // knWo*; -1 not ours
void v10_wild_offer_scenario(const cJSON *state); // --render-scenario state.wildOffer {offer, now, notify, view}
