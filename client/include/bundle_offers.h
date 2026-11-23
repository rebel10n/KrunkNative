// Bundle offers on the menu (Krunker v10.0.7): the home tab's rotating bundle card, the bundle popup (bundlePopup) with
// a timed bundle's countdown, and the shop button's limited-offer note. See client/src/ui/bundle_offers.c.
#ifndef KN_BUNDLE_OFFERS_H
#define KN_BUNDLE_OFFERS_H

#include <cJSON.h>
#include <stddef.h>

// fills #homeStoreAd from the store's bundle rows (the first one the account does not own, in v10's priority order)
// and rotates it every 20 s; call when the menu shows and when the rows change
void bundle_offers_home_refresh(void);
// the epoch ms a bundle's offer ends: its event's end, else (has_condition) first_seen_ms + 24 h (first_seen_ms 0:
// none), else its EXPIRY; 0 = untimed
double bundle_offers_ends_ms(const cJSON *bundle, int has_condition, double first_seen_ms);
// v10's bundle popup (#bundlePop) for one bundle row: art, contents, items, BUY / GIFT and the countdown; plays the
// bundle's music and saves bundle_<ID> / lastBundlePopupShown
void bundle_offers_popup_show(const cJSON *bundle, int has_condition, double first_seen_ms);
// the shop button's note for a conditional bundle, removed after 15 s
void bundle_offers_ltm_note(int bundle_id, double left_ms);
// bundlePopup(true, false, id) for a Box Office bundle
void bundle_offers_popup(int id);
// resetPops(): stops the open popup's music and countdown
void bundle_offers_pops_reset(void);
// the wall clock, epoch ms
double bundle_offers_now_ms(void);
// ISO-8601 UTC ("2026-11-19T12:00:00.000Z") -> epoch ms; 0 when it does not parse
double bundle_offers_iso_ms(const char *iso);
// v10's getReadableTime: "1y 2d 3h 4m " (zero years / days / hours left out, minutes always)
void bundle_offers_readable_time(double ms, char *out, size_t n);
// sorts bundle rows by v10's priority list (the listed ones first, highest priority first; stable)
void bundle_offers_priority_sort(const cJSON **rows, int n);

#endif
