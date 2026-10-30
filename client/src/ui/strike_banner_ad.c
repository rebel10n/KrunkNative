// Krunker Strike's banner ad slot (client/include/strike_banner_ad.h). The slot shows the match's
// Double XP state and its click plays the placeholder ad (kn_ad.h), then claims it through double_xp_offer.h
// (double_xp_claim: ["rqdxp"]). Its status line says what is on: "2x XP" once the match is doubled, and why there is no
// offer. The weekend schedules (2x class XP, 2x raid drops) are not on the banner yet: dxp-client exports
// no accessor for them. Guests are asked to sign in; nothing is watched and nothing is requested for them.
#include <strike_banner_ad.h>
#include "klog.h"

#include <backend_ui.h>
#include <double_xp_offer.h>
#include <kn_ad.h> // the banner's ad: the placeholder

#include <stdio.h>
#include <string.h>

#define BANNER_MARKUP "<div class=\"strike-banner-ad\" id=\"strikeBannerAd\" onclick=\"strikeAction('banner-ad')\">" \
    "<span class=\"strike-banner-ad__label\">Watch an ad for Double XP</span>" \
    "<span class=\"strike-banner-ad__status\"></span>" \
    "<span class=\"strike-banner-ad__button\">Double XP</span></div>"

static struct {
    int error; // the last click's claim failed (the status says so until the next click)
} g_slot;

static void append(char *text, const size_t n, const char *part) {
    if (text[0]) strncat(text, " | ", n - strlen(text) - 1);
    strncat(text, part, n - strlen(text) - 1);
}

// the button (Double XP, or Collected once taken; dimmed when no offer is claimable) and the status line
static void refresh(void) {
    const HtmlElem root = html_dom_by_id("strikeBannerAd");
    if (!root) return;
    const int guest = !active_account();
    const int taken = double_xp_taken();
    const int dimmed = !guest && !double_xp_offer_open();
    const HtmlElem button = html_dom_query(root, ".strike-banner-ad__button");
    if (button) {
        html_dom_set_text(button, taken ? "Collected" : "Double XP");
        html_dom_set_class(button, "strike-banner-ad__button--off", dimmed);
    }
    const HtmlElem status = html_dom_query(root, ".strike-banner-ad__status");
    if (!status) return;
    char text[256] = "";
    if (guest) {
        append(text, sizeof(text), "Sign in to claim Double XP.");
    } else {
        if (taken) append(text, sizeof(text), "2x XP");
        if (g_slot.error) append(text, sizeof(text), "Double XP could not be granted. Try again.");
        else if (!taken && !double_xp_offer_open()) append(text, sizeof(text), "No Double XP for this match.");
    }
    html_dom_set_text(status, text);
}

void strike_banner_ad_show(const HtmlElem lean_root) {
    if (!lean_root) return;
    const HtmlElem bottom = html_dom_query(lean_root, ".lean-end-screen__bottom");
    if (!bottom) return;
    if (!html_dom_by_id("strikeBannerAd")) {
        // the strip FRVR's .lean-end-screen__bottom-banner leaves below the row (bottom: 25 units)
        html_dom_insert_html(bottom, HTML_INSERT_BEFORE_END, BANNER_MARKUP);
    }
    html_dom_set_class(bottom, "lean-end-screen__bottom-banner", 1);
    g_slot.error = 0;
    refresh();
}

// double_xp_claim's answer: the slot shows it (the claim itself is dxp's)
static void on_claim(void *user, const DoubleXpResult result) {
    (void) user;
    g_slot.error = result == DOUBLE_XP_ERROR;
    const char *name = result == DOUBLE_XP_GRANTED ? "granted" : result == DOUBLE_XP_NOT_OFFERED ? "not offered" : "error";
    KN_LOG_VERBOSE2("[strike] banner ad: double XP %s\n", name);
    refresh();
}

// the banner's ad has been watched (KrunkNative's placeholder, kn_ad.h): the claim
static void after_ad(void *user) {
    (void) user;
    double_xp_claim(on_claim, NULL);
}

void strike_banner_ad_claim(void) {
    g_slot.error = 0;
    if (!active_account()) { // a guest has no account to claim for: nothing is requested
        KN_LOG_VERBOSE2("[strike] banner ad: double XP guest\n");
        refresh();
        return;
    }
    if (!double_xp_offer_open()) { // no offer: the claim says so at once, no ad
        double_xp_claim(on_claim, NULL);
        return;
    }
    kn_ad_show_rewarded(after_ad, NULL); // "Watch an ad for Double XP": the placeholder ad, then the claim
}
