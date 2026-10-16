#pragma once
// Krunker Strike's banner ad slot on the round end (FRVR 1.37.2's lean-end-screen__bottom-banner: the
// bottom banner FRVR shows when FRVR.ads.isSupported(AdType.BANNER) and the round is over). Krunker Box
// Office's placeholder ad is always supported, so the slot is always there. Its click is the match's
// Double XP claim, the same one as the reward row's button (double_xp_offer.h): double_xp_claim sends
// ["rqdxp"] on the social socket at once, with no ad. The slot grants nothing itself.
// Implementation: client/src/ui/strike_banner_ad.c.
#ifndef STRIKE_BANNER_AD_H
#define STRIKE_BANNER_AD_H

#include <html_dom.h>

// the round end's root (LEAN_END_SCREEN): inserts the slot once and refreshes its status line
void strike_banner_ad_show(HtmlElem lean_root);
// the slot's click (strikeAction('banner-ad'))
void strike_banner_ad_claim(void);

#endif
