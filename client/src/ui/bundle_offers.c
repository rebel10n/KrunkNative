// Bundle offers on the menu (Krunker v10.0.7): the home tab's bundle card (#homeStoreAd with its discount label
// #bundleHomeDiscountLabel), the countdown a timed bundle's popup carries (#timer-text / #progress-circle /
// #timer-center / #warning), and the short-lived "limited offer" note on the shop button (#ltm-offer-container).
//
// Behaviour (deob-10.0.7.js):
//   - home card: the first bundle the account does not own in the store's order (:215669-215684, :215696-215703),
//     then the next one every 20 s (:215653-215667, 0x4e20); the art, the name and "$PRICE" fade in after 250 ms
//     (:215607-215646); a discounted bundle shows "-N%" on the label (Math.round) and the price's discount class
//     (:215628-215629); a click opens the bundle's popup (:215630-215632). Every bundle owned: an empty-state line
//     (:215704). There are no ads in Box Office, so the card always takes the slot (the !canShowAds branch, :215657).
//   - countdown: a bundle is timed by its event (EVENT_NAME + EVENT_ENDS_AT), its EXPIRY, or a 24-hour condition that
//     starts when the account first saw it (valid_for_24_hours, :203294, :215809-215833). Each tick (1 s, :215945)
//     shows the time left; the ring's dash offset is 240 - left * 240 / period (period 24 h, floored at 0) and the
//     state is calm / middle / urgent at >= 50 % / >= 25 % / below (:215880-215892). At 0 the timer shows that the
//     offer is over, the ring is full and the warning shows (:215861-215879).
//   - shop-button note: a random conditional bundle with a 24-hour condition puts a note on #menuItemShop with the
//     time left; it goes away after 15 s (:215714-215739, 0x3a98).
// The wording is Box Office's own (ui/i18n/kn/en.json, kn.bundle.*); the class names are the page's identifiers.
#include <bundle_offers.h>

#include <html_dom.h>
#include <i18n.h>
#include <junk_shop_ui.h>
#include <ks_game.h> // ks_wall_ms
#include <backend_ui.h> // store_skin_preview
#include "../viewer/viewer_store.h" // the popup's skins and their rarity colours
#include <ui_logic.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define HOME_ROTATE_MS 20000.0 // 0x4e20
#define HOME_FADE_MS 250.0     // 0xfa
#define OFFER_PERIOD_MS 86400000.0
#define TIMER_TICK_MS 1000.0
#define LTM_NOTE_MS 15000.0    // 0x3a98
#define HOME_MAX 128

static struct {
    int rotate_timer, fade_timer;
    int last_id;        // the bundle the card shows (0: none yet)
    int shown;          // the card was filled once
    int fade_id;        // the bundle the pending fade fills in
    int tick_timer;
    double ends_ms;     // the open popup's offer end (epoch ms), 0 = untimed
    int ltm_timer;
} B;

// epoch ms; ks_wall_ms has the Windows path (MinGW's C runtime has no timespec_get / TIME_UTC)
double bundle_offers_now_ms(void) { return ks_wall_ms(); }

// ISO-8601 (UTC "Z" or "+00:00", optional fraction) -> epoch ms; 0 when it does not parse
double bundle_offers_iso_ms(const char *s) {
    if (!s || !*s) return 0;
    int y, mo, d, h = 0, mi = 0;
    double sec = 0;
    if (sscanf(s, "%d-%d-%dT%d:%d:%lf", &y, &mo, &d, &h, &mi, &sec) < 3) return 0;
    // days from civil (Howard Hinnant)
    y -= mo <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned) (y - era * 400);
    const unsigned doy = (unsigned) ((153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const double days = (double) era * 146097 + (double) doe - 719468;
    return ((days * 24 + h) * 60 + mi) * 60000.0 + sec * 1000.0;
}

static const char *row_text(const cJSON *b, const char *key) {
    const cJSON *v = cJSON_GetObjectItem(b, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static double row_num(const cJSON *b, const char *key) { // numbers or numeric strings (parseFloat)
    const cJSON *v = cJSON_GetObjectItem(b, key);
    return cJSON_IsNumber(v) ? v->valuedouble : cJSON_IsString(v) ? atof(v->valuestring) : 0;
}

static int row_id(const cJSON *b) { return (int) row_num(b, "ID"); }
static int row_owned(const cJSON *b) { return cJSON_IsTrue(cJSON_GetObjectItem(b, "OWNED")); }

static void escape(char *out, const size_t n, const char *s) {
    size_t k = 0;
    for (; s && *s && k + 7 < n; s++) {
        const char *r = *s == '<' ? "&lt;" : *s == '>' ? "&gt;" : *s == '&' ? "&amp;" : *s == '"' ? "&quot;" : NULL;
        if (r) {
            const size_t l = strlen(r);
            memcpy(out + k, r, l);
            k += l;
        } else {
            out[k++] = *s;
        }
    }
    out[k] = 0;
}

// "1d 3h", "3h 12m", "12m 05s", "5s": the time left, compact
void bundle_offers_format_left(const double ms, char *out, const size_t n) {
    long s = ms > 0 ? (long) floor(ms / 1000.0) : 0;
    const long d = s / 86400, h = s % 86400 / 3600, m = s % 3600 / 60, sec = s % 60;
    if (d > 0) snprintf(out, n, "%ldd %ldh", d, h);
    else if (h > 0) snprintf(out, n, "%ldh %02ldm", h, m);
    else if (m > 0) snprintf(out, n, "%ldm %02lds", m, sec);
    else snprintf(out, n, "%lds", sec);
}

// ---------------------------------------------------------------- the home tab's card

static void card_style(const char *id, const char *opacity, const char *transform) {
    const HtmlElem e = html_dom_by_id(id);
    if (!e) return;
    html_dom_set_style(e, "transition", "all 0.5s ease-in-out");
    html_dom_set_style(e, "opacity", opacity);
    html_dom_set_style(e, "transform", transform);
}

static HtmlElem discount_label(void) { // created once under #homeStoreAdC (:215611-215612)
    HtmlElem l = html_dom_by_id("bundleHomeDiscountLabel");
    if (l) return l;
    const HtmlElem art = html_dom_by_id("homeStoreAdC");
    if (!art) return 0;
    l = html_dom_create("div", "bundleHomeDiscountLabel");
    html_dom_set_attr(l, "class", "bundleHomeDiscountLabel");
    html_dom_append(art, l);
    return l;
}

static const cJSON *row_by_id(const cJSON *rows, const int id) {
    const cJSON *b;
    cJSON_ArrayForEach(b, rows) if (row_id(b) == id) return b;
    return NULL;
}

static void card_fill(void *user) {
    (void) user;
    B.fade_timer = 0;
    const cJSON *b = row_by_id(junk_shop_bundles_rows(), B.fade_id);
    const HtmlElem art = html_dom_by_id("homeStoreAdC"), name = html_dom_by_id("homeStoreAdTx"), price = html_dom_by_id("bundleQuickBuyB");
    const HtmlElem label = discount_label();
    if (!b || !art || !name || !price) return;
    char path[64], url[320], css[400], text[256];
    snprintf(path, sizeof path, "img/backgrounds/bundles/b_%d.png", row_id(b));
    html_dom_asset_url(path, url, sizeof url);
    snprintf(css, sizeof css, "url(%s)", url);
    html_dom_set_style(art, "background-image", css);
    html_dom_set_style(art, "background-size", "cover");
    escape(text, sizeof text, row_text(b, "NAME"));
    html_dom_set_html(name, text);
    snprintf(text, sizeof text, "$%.15g", row_num(b, "PRICE"));
    html_dom_set_text(price, text);
    const double disc = row_num(b, "DISCOUNT_PCT");
    if (label) {
        if (disc > 0) {
            snprintf(text, sizeof text, "-%.0f%%", floor(disc + 0.5)); // Math.round
            html_dom_set_text(label, text);
            html_dom_set_display(label, "block");
        } else {
            html_dom_set_display(label, "none");
        }
    }
    html_dom_set_class(price, "bundleHomePrice--discount", disc > 0);
    char click[96];
    snprintf(click, sizeof click, "bundlePopup(true, false, %d)", row_id(b));
    const HtmlElem card = html_dom_by_id("homeStoreAd");
    if (card) html_dom_set_attr(card, "onclick", click);
    card_style("homeStoreAdC", "1", "translateY(0)");
    card_style("homeStoreAdTx", "1", "translateY(0)");
    card_style("bundleQuickBuyB", "1", "translateY(0)");
    if (disc > 0) card_style("bundleHomeDiscountLabel", "1", "translateY(0)");
}

static void card_show(const cJSON *b) {
    B.last_id = row_id(b);
    B.fade_id = B.last_id;
    discount_label();
    card_style("homeStoreAdC", "0.3", "translateY(-10px)");
    card_style("homeStoreAdTx", "0.3", "translateY(-10px)");
    card_style("bundleQuickBuyB", "0.3", "translateY(-10px)");
    card_style("bundleHomeDiscountLabel", "0.3", "translateY(-10px)");
    if (B.fade_timer) html_timer_cancel(B.fade_timer);
    B.fade_timer = html_timer_add(HOME_FADE_MS, card_fill, NULL);
}

// The card sits in #topLeftAdHolder's swap slot. Krunker never hides that row (only #topRightAdHolder goes when there
// are no ads: deob-10.0.7.js:226527, 229393); Box Office hides the whole row while it holds nothing (menu_ui.c), so
// the card brings it back as the page's flex row.
static void card_slot(const int on) {
    const HtmlElem card = html_dom_by_id("homeStoreAd"), ad = html_dom_by_id("adCon"), row = html_dom_by_id("topLeftAdHolder");
    if (card) html_dom_set_display(card, on ? "block" : "none");
    if (row) html_dom_set_display(row, on ? "flex" : "none");
    if (on && ad) html_dom_set_display(ad, "none");
}

static void rotate(void *user);

static void arm_rotation(void) {
    if (B.rotate_timer) html_timer_cancel(B.rotate_timer);
    B.rotate_timer = html_timer_add(HOME_ROTATE_MS, rotate, NULL);
}

// the next bundle not owned after the one shown, in the store's order (wrapping)
static void rotate(void *user) {
    (void) user;
    B.rotate_timer = 0;
    const cJSON *rows = junk_shop_bundles_rows();
    const cJSON *list[HOME_MAX];
    const int n = rows ? junk_shop_bundles_order(rows, list, HOME_MAX) : 0;
    const cJSON *open[HOME_MAX];
    int m = 0, at = -1;
    for (int i = 0; i < n; i++) {
        if (row_owned(list[i])) continue;
        if (row_id(list[i]) == B.last_id) at = m;
        open[m++] = list[i];
    }
    if (m > 0) card_show(open[(at + 1) % m]);
    arm_rotation();
}

void bundle_offers_home_refresh(void) {
    const cJSON *rows = junk_shop_bundles_rows();
    if (!rows || cJSON_GetArraySize(rows) == 0) {
        if (!B.rotate_timer) B.rotate_timer = html_timer_add(1000.0, rotate, NULL); // the rows are still loading
        return;
    }
    const cJSON *list[HOME_MAX];
    const int n = junk_shop_bundles_order(rows, list, HOME_MAX);
    const cJSON *first = NULL;
    for (int i = 0; i < n && !first; i++) if (!row_owned(list[i])) first = list[i];
    card_slot(1);
    B.shown = 1;
    if (first) {
        if (row_id(first) != B.last_id) card_show(first);
    } else { // every bundle is owned
        const HtmlElem name = html_dom_by_id("homeStoreAdTx"), price = html_dom_by_id("bundleQuickBuyB");
        if (name) html_dom_set_text(name, I18N("kn.bundle.allOwned", "Every bundle is already yours"));
        if (price) html_dom_set_text(price, I18N("kn.bundle.moreSoon", "New bundles are on the way"));
        B.last_id = 0;
    }
    arm_rotation();
}

// ---------------------------------------------------------------- a timed bundle's countdown

double bundle_offers_ends_ms(const cJSON *b, const double first_seen_ms) {
    const char *event = row_text(b, "EVENT_NAME"), *event_end = row_text(b, "EVENT_ENDS_AT");
    if (event && *event && event_end) return bundle_offers_iso_ms(event_end);
    const char *expiry = row_text(b, "EXPIRY");
    if (expiry && *expiry) return bundle_offers_iso_ms(expiry);
    if (first_seen_ms > 0) return first_seen_ms + OFFER_PERIOD_MS;
    return 0;
}

void bundle_offers_timer_html(StrBuf *sb, const cJSON *b, const double ends_ms) {
    if (ends_ms <= 0 || ends_ms - bundle_offers_now_ms() <= 0) return; // the timer shows only while time is left (:215942)
    const char *event = row_text(b, "EVENT_NAME");
    char title[160];
    escape(title, sizeof title, event && *event && row_text(b, "EVENT_ENDS_AT") ? event : I18N("kn.bundle.limited", "Limited-time bundle"));
    sb_appendf(sb, "<div class=\"bundle-popup-lmt-offer\"><span>%s</span></div>", title);
    sb_appendf(sb,
               "<div class=\"strike-timer\"><div class=\"strike-timer__holder\">"
               "<div id=\"warning\" class=\"strike-timer__warning\" style=\"display:none\"><span class=\"material-icons\">warning</span></div>"
               "<div class=\"strike-timer__timer\"><div class=\"strike-timer__timer-inner\"><svg><circle id=\"progress-circle\" class=\"strike-timer__stroke-mod--chill\" style=\"stroke-dashoffset:0%%\"></circle></svg></div>"
               "<div id=\"timer-center\" class=\"strike-timer__timer-center strike-timer__background-mod--chill\"></div></div>"
               "<div class=\"strike-timer__info\"><div id=\"kn-timer-label\" class=\"strike-timer__info-text\">%s</div>"
               "<div id=\"timer-text\" class=\"strike-timer__info-text strike-timer__info-text--time strike-timer__text-mod--chill\"></div></div>"
               "</div></div>",
               I18N("kn.bundle.endsIn", "Ends in"));
}

static void set_cls(const char *id, const char *cls) {
    const HtmlElem e = html_dom_by_id(id);
    if (e) html_dom_set_attr(e, "class", cls);
}

static void tick(void *user) {
    (void) user;
    B.tick_timer = 0;
    const HtmlElem text = html_dom_by_id("timer-text");
    if (!text || B.ends_ms <= 0) return; // the popup closed
    const double left = B.ends_ms - bundle_offers_now_ms();
    char buf[96];
    if (left <= 0) {
        html_dom_set_text(text, I18N("kn.bundle.over", "Expired"));
        set_cls("timer-text", "strike-timer__info-text strike-timer__info-text--time strike-timer__text-mod--panic");
        const HtmlElem label = html_dom_by_id("kn-timer-label");
        if (label) html_dom_set_text(label, I18N("kn.bundle.ended", "This bundle's offer has ended"));
        const HtmlElem ring = html_dom_by_id("progress-circle");
        if (ring) {
            html_dom_set_style(ring, "stroke-dashoffset", "240%");
            html_dom_set_attr(ring, "class", "strike-timer__stroke-mod--panic");
        }
        set_cls("timer-center", "strike-timer__timer-center strike-timer__background-mod--panic");
        const HtmlElem warn = html_dom_by_id("warning");
        if (warn) html_dom_set_display(warn, "flex");
        B.ends_ms = 0;
        return;
    }
    const char *state = left >= 0.5 * OFFER_PERIOD_MS ? "chill" : left >= 0.25 * OFFER_PERIOD_MS ? "fine" : "panic";
    double offset = 240.0 - left * (240.0 / OFFER_PERIOD_MS);
    if (offset < 0) offset = 0;
    bundle_offers_format_left(left, buf, sizeof buf);
    html_dom_set_text(text, buf);
    snprintf(buf, sizeof buf, "strike-timer__info-text strike-timer__info-text--time strike-timer__text-mod--%s", state);
    set_cls("timer-text", buf);
    const HtmlElem ring = html_dom_by_id("progress-circle");
    if (ring) {
        snprintf(buf, sizeof buf, "strike-timer__stroke-mod--%s", state);
        html_dom_set_attr(ring, "class", buf);
        snprintf(buf, sizeof buf, "%.4g%%", offset);
        html_dom_set_style(ring, "stroke-dashoffset", buf);
    }
    snprintf(buf, sizeof buf, "strike-timer__timer-center strike-timer__background-mod--%s", state);
    set_cls("timer-center", buf);
    const HtmlElem warn = html_dom_by_id("warning");
    if (warn) html_dom_set_display(warn, strcmp(state, "panic") ? "none" : "flex");
    B.tick_timer = html_timer_add(TIMER_TICK_MS, tick, NULL);
}

void bundle_offers_timer_start(const double ends_ms) {
    if (B.tick_timer) html_timer_cancel(B.tick_timer);
    B.tick_timer = 0;
    B.ends_ms = ends_ms > 0 && ends_ms - bundle_offers_now_ms() > 0 ? ends_ms : 0;
    if (B.ends_ms > 0) tick(NULL);
}

// ---------------------------------------------------------------- the shop button's note

static void ltm_remove(void *user) {
    (void) user;
    B.ltm_timer = 0;
    const HtmlElem n = html_dom_by_id("ltm-offer-container");
    if (n) html_dom_remove(n);
}

void bundle_offers_ltm_note(const int bundle_id, const double left_ms) {
    ltm_remove(NULL);
    if (B.ltm_timer) html_timer_cancel(B.ltm_timer);
    const HtmlElem shop = html_dom_by_id("menuItemShop");
    if (!shop || bundle_id <= 0) return;
    char left[48], path[64], url[320];
    bundle_offers_format_left(left_ms, left, sizeof left);
    snprintf(path, sizeof path, "img/backgrounds/bundles/b_%d.png", bundle_id);
    html_dom_asset_url(path, url, sizeof url);
    StrBuf sb = {0};
    sb_appendf(&sb,
               "<div id=\"ltm-offer-container\" class=\"ltm-offer-container\">"
               "<div class=\"ltm-offer-container__time-bar-bg\"><div class=\"ltm-offer-container__time-bar-progress\"></div></div>"
               "<div class=\"ltm-offer-container__arrow-container\"><div class=\"ltm-offer-container__arrow\"></div></div>"
               "<div class=\"ltm-offer-container__body\"><div class=\"ltm-offer-container__text-holder\">"
               "<div class=\"ltm-offer-container__lmt-text\">%s</div>"
               "<div class=\"ltm-offer-container__time-left\">%s %s</div></div>"
               "<div class=\"ltm-offer-container__img-holder\"><img src=\"%s\" alt=\"\"></div></div></div>",
               I18N("kn.bundle.ltmNote", "A limited bundle is waiting for you"), I18N("kn.bundle.endsIn", "Ends in"), left, url);
    char *html = sb_finish(&sb);
    html_dom_insert_html(shop, HTML_INSERT_BEFORE_END, html);
    free(html);
    B.ltm_timer = html_timer_add(LTM_NOTE_MS, ltm_remove, NULL);
}

// ---------------------------------------------------------------- a Box Office bundle's popup

// bundlePopup(true, false, id) on Box Office (deob-10.0.7.js:215769-215945): the bundle's skins (up to 12, each in its
// rarity's border), the discount, the art, the Junk it adds, the price before the discount, BUY unless owned, and the
// countdown for a timed bundle. BUY is the brain's purchase (junk_shop_bundle_buy). Wording: kn.bundle.*.
void bundle_offers_popup(const int id) {
    const cJSON *b = row_by_id(junk_shop_bundles_rows(), id);
    if (!b) {
        junk_shop_bundle_buy(id); // the rows are not here yet: the purchase answers as before
        return;
    }
    const int owned = row_owned(b);
    const double price = row_num(b, "PRICE"), disc = row_num(b, "DISCOUNT_PCT"), orig = row_num(b, "ORIGINAL_PRICE"), junk = row_num(b, "JUNK");
    StrBuf sb = {0};
    sb_append(&sb, "<div class='bundlItmCnt'>");
    const char *p = row_text(b, "SKINS");
    for (int n = 0; p && *p && n < 12;) {
        char *end = NULL;
        const long index = strtol(p, &end, 10);
        p = end && *end == ',' ? end + 1 : (end && end != p ? end : p + strlen(p));
        const ViewerSkin *k = viewer_skin((int) index);
        if (!k) continue;
        char thumb[320];
        store_skin_preview((int) index, thumb, sizeof thumb);
        const int r = k->rarity >= 0 && k->rarity < g_viewer_rarity_count ? k->rarity : 0;
        sb_appendf(&sb, "<div class='bundlItmC' style='background:url(%s);background-size:contain;background-position:center;border:3px solid %s;' onclick='viewItem(%ld)'></div>",
                   thumb, g_viewer_rarities[r].color, index);
        n++;
    }
    sb_append(&sb, "</div>");
    if (disc > 0) sb_appendf(&sb, "<div class='bundlPopDiscountLabel'>%s -%.0f%%</div>", I18N("kn.bundle.save", "Save"), floor(disc + 0.5));
    const double ends = bundle_offers_ends_ms(b, 0);
    bundle_offers_timer_html(&sb, b, ends);
    char art[320], path[64];
    snprintf(path, sizeof path, "img/backgrounds/bundles/b_%d.png", id);
    html_dom_asset_url(path, art, sizeof art);
    sb_appendf(&sb, "<div class='bundlPopImg' style='background:url(%s);background-size:cover;'></div><div class='bundlPopCurncs'>", art);
    if (junk > 0) sb_appendf(&sb, "<div style='color:#cfcfcf'>+ <span style='color:#7d7d7d;'>%.2f</span> <span class='material-icons'>delete</span></div>", junk);
    sb_append(&sb, "</div><div class='bundlPopBuyH'>");
    if (disc > 0 && orig > 0) sb_appendf(&sb, "<div class='bundlPopOriginalPrice'>%s $%.2f</div>", I18N("kn.bundle.was", "Regular price"), orig);
    if (!owned) sb_appendf(&sb, "<div class='bundlPopBuyC%s' onclick='knBundleBuy(%d)'>%s $%.15g</div>", disc > 0 ? " bundlPopBuy--discount" : "", id, I18N("kn.bundle.buy", "Get it for"), price);
    else sb_appendf(&sb, "<div class='bundlPopBuyC'>%s</div>", I18N("kn.bundle.owned", "Owned"));
    sb_append(&sb, "</div>");
    char *html = sb_finish(&sb);
    ui_loadout_clear_pops(); // resetPops()
    const HtmlElem holder = html_dom_by_id("popupHolder");
    if (holder) html_dom_set_display(holder, "block");
    const HtmlElem pop = html_dom_by_id("bundlePop");
    if (pop) {
        html_dom_set_html(pop, html);
        html_dom_set_display(pop, "block");
    }
    free(html);
    bundle_offers_timer_start(ends);
}
