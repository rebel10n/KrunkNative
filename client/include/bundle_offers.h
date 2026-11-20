// Bundle offers on the menu (Krunker v10.0.7): the home tab's rotating bundle card, a timed bundle's countdown in
// its popup, and the shop button's limited-offer note. See client/src/ui/bundle_offers.c.
#ifndef KN_BUNDLE_OFFERS_H
#define KN_BUNDLE_OFFERS_H

#include <cJSON.h>
#include <stddef.h>
#include <ui_logic.h> // StrBuf

// fills #homeStoreAd from the store's bundle rows (the first one the account does not own) and rotates it every
// 20 s; call when the menu shows and when the rows change
void bundle_offers_home_refresh(void);
// the epoch ms a bundle's offer ends: its event's end, its EXPIRY, or first_seen_ms + 24 h for a 24-hour
// conditional bundle (first_seen_ms 0: none); 0 = untimed
double bundle_offers_ends_ms(const cJSON *bundle, double first_seen_ms);
// appends the countdown block for a popup (nothing when ends_ms is 0 or already past), then start it with
// bundle_offers_timer_start(ends_ms) once the popup's HTML is in the page
void bundle_offers_timer_html(StrBuf *sb, const cJSON *bundle, double ends_ms);
void bundle_offers_timer_start(double ends_ms);
// the shop button's note for a conditional bundle, removed after 15 s
void bundle_offers_ltm_note(int bundle_id, double left_ms);
// bundlePopup(true, false, id) for a Box Office bundle: its popup, with the countdown when it is timed
void bundle_offers_popup(int id);
// the wall clock, epoch ms
double bundle_offers_now_ms(void);
// ISO-8601 UTC ("2026-11-19T12:00:00.000Z") -> epoch ms; 0 when it does not parse
double bundle_offers_iso_ms(const char *iso);
// "1d 3h" / "3h 12m" / "12m 05s" / "5s"
void bundle_offers_format_left(double ms, char *out, size_t n);

#endif
