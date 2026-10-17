#pragma once
// Krunker Strike's Double XP surfaces (client/src/ui/strike_double_xp.c, double_xp_offer.h): the lean end
// screen's Double XP button (FRVR 1.37.2's .lean-end-screen__double-reward: a blue frvr-button with
// the ad icon, pulsing until it is spent) and the "2x XP for:" badge (FRVR's .booster-time-left, the
// booster's top-right layout) that shows the account's buff and its time left.
#ifndef STRIKE_DOUBLE_XP_OFFER_H
#define STRIKE_DOUBLE_XP_OFFER_H

#include <html_dom.h>

void strike_double_xp_tick(void);                      // per frame: the badge in #frvrMenuMobileHolder (once a second)
void strike_double_xp_end_fill(HtmlElem end_root);     // the lean end screen: the Double XP button in its reward row
void strike_double_xp_claim(void);                     // strikeAction('double-xp'): the placeholder ad, then the grant

#endif
