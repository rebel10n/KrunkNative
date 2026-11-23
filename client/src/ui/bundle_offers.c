// Bundle offers on the menu (Krunker v10.0.7): the home tab's bundle card (#homeStoreAd with its discount label
// #bundleHomeDiscountLabel), the bundle popup (bundlePopup's #bundlePop) with a timed bundle's countdown
// (#timer-text / #progress-circle / #timer-center / #warning), and the short-lived "limited offer" note on the shop
// button (#ltm-offer-container).
//
// Behaviour (deob-10.0.7.js):
//   - home card: the first bundle the account does not own in v10's priority order (iìiïìîï, :16477; :215669-215684,
//     :215696-215703); the art, the name and "$PRICE" fade in after 250 ms (:215607-215646); a discounted bundle
//     shows "-N%" on the label (Math.round) and the price's discount class (:215628-215629); a click opens the
//     bundle's popup (:215630-215632). Every bundle owned: "You got them all!" / "More Incoming!" (:215704). The card
//     shares #topLeftAdHolder's swap slot with #adCon (a 300x250 ad): with canShowAds (menu_ui.c menu_ads_apply) the
//     slot flips every 20 s (0x4e20) to the empty #adCon, then back to the card with the next bundle not owned
//     (iiííìïì, :215644-215667: a counter over the rows not owned); each fill starts on the card (:215680-215682,
//     :216888).
//   - popup (:215785-215945): the discount, the sprays' keyframes, the "Time-limited offer!" / event label, the art,
//     the Junk / KR / Premium days it adds, NAME, INFO, twelve item slots, the struck price, BUY / GIFT, and the
//     countdown; the bundle's music (bundle_<ID>), bundle_<ID> and lastBundlePopupShown saved.
//   - countdown: a bundle is timed by its event (EVENT_NAME + EVENT_ENDS_AT), its 24-hour condition that starts when
//     the account first saw it (valid_for_24_hours, :203294), or its EXPIRY (:215809-215833). The seconds left are
//     taken once and counted down each second (1 s, :215945); the text is getReadableTime, the ring's dash offset
//     240 - left * 240 / 24 h (floored at 0), the state chill / fine / panic at >= 50 % / >= 25 % / below
//     (:215880-215892). At 0: "Offer is up", "Offer has ended", the ring full and the warning (:215861-215879).
//   - shop-button note: a random conditional bundle with a 24-hour condition puts "Time-limited Offer Available!"
//     with its time left on #menuItemShop; it goes away after 15 s (:215714-215739, 0x3a98).
#include <bundle_offers.h>

#include <audio.h>
#include <html_dom.h>
#include <i18n.h>
#include <junk_shop_ui.h>
#include <ks_game.h> // ks_wall_ms
#include <backend_ui.h> // store_skin_preview, active_account
#include "../viewer/viewer_store.h" // the popup's skins and their rarity colours
#include <ui_logic.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define HOME_ROTATE_MS 20000.0 // 0x4e20
#define HOME_FADE_MS 250.0     // 0xfa
#define OFFER_PERIOD_MS 86400000.0 // 0x5265c00
#define TIMER_TICK_MS 1000.0
#define LTM_NOTE_MS 15000.0    // 0x3a98
#define HOME_MAX 128
#define POPUP_SLOTS 12         // the popup's item slots (:215897)

static struct {
    int rotate_timer, fade_timer;
    int last_id;        // the bundle the card shows (0: none yet)
    int shown;          // the card was filled once
    int card_turn;      // iïìììíì: the swap slot shows the card (0: #adCon's turn)
    int rot_i;          // iiììïìi: the rotation's counter over the bundles not owned
    int fade_id;        // the bundle the pending fade fills in
    int tick_timer;
    long left_s;        // iìÌïîïï: the open popup's seconds left
    double period_ms;   // iiÍîiïï: its refresh time (24 h)
    char music[32];     // iíîìïïi: the open popup's bundle music
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

void bundle_offers_readable_time(const double ms, char *out, const size_t n) {
    long long s = ms > 0 ? (long long) floor(ms / 1000.0) : 0; // Math.max(0, floor(ms / 1000))
    long long m = s / 60, h = m / 60, d = h / 24;
    m %= 60;
    h %= 24;
    const long long y = d / 365;
    d %= 365;
    char yp[24] = "", dp[24] = "", hp[24] = "";
    if (y) snprintf(yp, sizeof yp, "%lldy ", y);
    if (d) snprintf(dp, sizeof dp, "%lldd ", d);
    if (h) snprintf(hp, sizeof hp, "%lldh ", h);
    snprintf(out, n, "%s%s%s%lldm ", yp, dp, hp, m);
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

// ---------------------------------------------------------------- v10's bundle priority

// iìiïìîï = [30, 21, 29, 28, 26, 27, 25, 24, 22, 23, 20, 19, 6, 8, 10, 9, 18, 16].reverse() (deob-10.0.7.js:16477); the
// sorts take indexOf(b.ID) - indexOf(a.ID): the highest index first, an id not listed (-1) last
static const int k_priority[] = {16, 18, 9, 10, 8, 6, 19, 20, 23, 22, 24, 25, 27, 26, 28, 29, 21, 30};

static int priority(const int id) {
    for (int i = 0; i < (int) (sizeof k_priority / sizeof k_priority[0]); i++)
        if (k_priority[i] == id) return i;
    return -1;
}

void bundle_offers_priority_sort(const cJSON **v, const int n) {
    for (int i = 1; i < n; i++) { // stable, as Array.prototype.sort
        const cJSON *x = v[i];
        int j = i;
        for (; j > 0 && priority(row_id(x)) > priority(row_id(v[j - 1])); j--) v[j] = v[j - 1];
        v[j] = x;
    }
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

// #topLeftAdHolder's swap slot: the card (on) or #adCon, which Box Office leaves empty (:215652-215656)
static void card_slot(const int on) {
    const HtmlElem card = html_dom_by_id("homeStoreAd"), ad = html_dom_by_id("adCon");
    if (card) html_dom_set_display(card, on ? "block" : "none");
    if (ad) html_dom_set_display(ad, on ? "none" : "");
}

static void rotate(void *user);

static void arm_rotation(void) {
    if (B.rotate_timer) html_timer_cancel(B.rotate_timer);
    B.rotate_timer = html_timer_add(HOME_ROTATE_MS, rotate, NULL);
}

// the rows the card goes through: the store's bundles in their order (a guest's sorted by priority, as v10's
// ALL_BUNDLE_DATA handler sorts them in place for one, :215672-215675), the owned ones left out
static int rows_not_owned(const cJSON *rows, const cJSON **out, const int max) {
    int n = 0;
    const cJSON *b;
    cJSON_ArrayForEach(b, rows) if (n < max && !row_owned(b)) out[n++] = b;
    if (!active_account()) bundle_offers_priority_sort(out, n);
    return n;
}

// the 20 s flip (iiííìïì): #adCon's turn, or the card's with the next bundle not owned (iiììïìi = (iiììïìi + 1) % n)
static void rotate(void *user) {
    (void) user;
    B.rotate_timer = 0;
    if (!B.shown) { // the rows were still loading: the card's first fill
        bundle_offers_home_refresh();
        return;
    }
    B.card_turn = !B.card_turn;
    card_slot(B.card_turn);
    if (!B.card_turn) {
        arm_rotation();
        return;
    }
    const cJSON *rows = junk_shop_bundles_rows();
    const cJSON *open[HOME_MAX];
    const int m = rows ? rows_not_owned(rows, open, HOME_MAX) : 0;
    if (m > 0) {
        B.rot_i = (B.rot_i + 1) % m;
        card_show(open[B.rot_i]);
    }
    arm_rotation();
}

void bundle_offers_home_refresh(void) {
    const cJSON *rows = junk_shop_bundles_rows();
    if (!rows || cJSON_GetArraySize(rows) == 0) {
        if (!B.rotate_timer) B.rotate_timer = html_timer_add(1000.0, rotate, NULL); // the rows are still loading
        return;
    }
    // the first bundle not owned by priority (:215673-215675 for a guest, :215696-215703 with the account's OWNED)
    const cJSON *list[HOME_MAX];
    int n = 0;
    const cJSON *b;
    cJSON_ArrayForEach(b, rows) if (n < HOME_MAX && !row_owned(b)) list[n++] = b;
    bundle_offers_priority_sort(list, n);
    const cJSON *first = n > 0 ? list[0] : NULL;
    card_slot(1);
    B.card_turn = 1;
    B.shown = 1;
    if (first) {
        if (row_id(first) != B.last_id) card_show(first);
    } else { // every bundle is owned (:215704); a fill still pending from the rows before would put one back
        if (B.fade_timer) html_timer_cancel(B.fade_timer);
        B.fade_timer = 0;
        const HtmlElem name = html_dom_by_id("homeStoreAdTx"), price = html_dom_by_id("bundleQuickBuyB");
        if (name) html_dom_set_html(name, "You got them all!");
        if (price) html_dom_set_html(price, "More Incoming!");
        B.last_id = 0;
    }
    arm_rotation();
}

// ---------------------------------------------------------------- the popup and its countdown

static int is_event(const cJSON *b) { // iiIiìíí = EVENT_NAME && EVENT_ENDS_AT
    const char *event = row_text(b, "EVENT_NAME"), *end = row_text(b, "EVENT_ENDS_AT");
    return event && *event && end && *end;
}

double bundle_offers_ends_ms(const cJSON *b, const int has_condition, const double first_seen_ms) {
    if (is_event(b)) return bundle_offers_iso_ms(row_text(b, "EVENT_ENDS_AT")); // iïÏíiîi
    if (has_condition) return first_seen_ms > 0 ? first_seen_ms + OFFER_PERIOD_MS : 0; // iïÎiíïi: no first_seen, no end
    const char *expiry = row_text(b, "EXPIRY"); // iïÏììïí
    return expiry && *expiry ? bundle_offers_iso_ms(expiry) : 0;
}

static void set_cls(const char *id, const char *cls) {
    const HtmlElem e = html_dom_by_id(id);
    if (e) html_dom_set_attr(e, "class", cls);
}

static void timer_stop(void) {
    if (B.tick_timer) html_timer_cancel(B.tick_timer);
    B.tick_timer = 0;
}

// iïÌìîïí (:215858-215897): one second of the countdown
static void tick(void *user) {
    (void) user;
    B.tick_timer = 0;
    const HtmlElem text = html_dom_by_id("timer-text");
    if (!text) return; // the popup is gone
    char buf[96];
    if (B.left_s <= 0) {
        html_dom_set_text(text, "Offer is up");
        set_cls("timer-text", "strike-timer__info-text strike-timer__info-text--time strike-timer__text-mod--panic");
        HtmlElem infos[4]; // the .strike-timer__info line that reads "Offer ends in:"
        const int ni = html_dom_query_all(0, "#bundlePop .strike-timer__info .strike-timer__info-text", infos, 4);
        for (int i = 0; i < ni; i++) {
            char t[64] = "";
            html_dom_get_text(infos[i], t, sizeof t);
            if (strstr(t, "Offer ends in:")) html_dom_set_text(infos[i], "Offer has ended");
        }
        const HtmlElem ring = html_dom_by_id("progress-circle");
        if (ring) {
            html_dom_set_style(ring, "stroke-dashoffset", "240%");
            html_dom_set_attr(ring, "class", "strike-timer__stroke-mod--panic");
        }
        set_cls("timer-center", "strike-timer__timer-center strike-timer__background-mod--panic");
        const HtmlElem warn = html_dom_by_id("warning");
        if (warn) html_dom_set_display(warn, "flex");
        return; // clearInterval
    }
    const double left = 1000.0 * (double) B.left_s, period = B.period_ms;
    const char *state = left >= 0.5 * period ? "chill" : left >= 0.25 * period ? "fine" : "panic";
    double offset = 240.0 - left * (240.0 / period);
    if (offset < 0) offset = 0;
    bundle_offers_readable_time(left, buf, sizeof buf);
    html_dom_set_text(text, buf);
    snprintf(buf, sizeof buf, "strike-timer__info-text strike-timer__info-text--time strike-timer__text-mod--%s", state);
    set_cls("timer-text", buf);
    const HtmlElem ring = html_dom_by_id("progress-circle");
    if (ring) {
        snprintf(buf, sizeof buf, "strike-timer__stroke-mod--%s", state);
        html_dom_set_attr(ring, "class", buf);
        snprintf(buf, sizeof buf, "%.15g%%", offset);
        html_dom_set_style(ring, "stroke-dashoffset", buf);
    }
    snprintf(buf, sizeof buf, "strike-timer__timer-center strike-timer__background-mod--%s", state);
    set_cls("timer-center", buf);
    const HtmlElem warn = html_dom_by_id("warning");
    if (warn) html_dom_set_display(warn, strcmp(state, "panic") ? "none" : "flex");
    B.left_s -= 1;
    B.tick_timer = html_timer_add(TIMER_TICK_MS, tick, NULL);
}

void bundle_offers_pops_reset(void) {
    if (B.music[0]) audio_stop(B.music);
    B.music[0] = 0;
    timer_stop();
}

const cJSON *ui_loadout_skin_json(int index); // ui_loadout.c: assets/skins.json's entry (store.skins[index])

// store.skins[index].collection (skins.json; the viewer table leaves it out), NULL when none
static const char *skin_collection(const int index) {
    const char *c = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(ui_loadout_skin_json(index), "collection"));
    return c && c[0] ? c : NULL;
}

// store.skins.filter(s => s && s.collection == c).length (:215905-215907)
static int collection_size(const char *c) {
    int n = 0;
    const cJSON *e;
    for (int i = 0; (e = ui_loadout_skin_json(i)) != NULL; i++) {
        const char *ec = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(e, "collection"));
        n += ec && strcmp(ec, c) == 0;
    }
    return n;
}

// the popup's twelve item slots (:215897-215919): each skin of SKINS in its rarity's border (a spray with frames
// animated), an index past SKINS or without a skin an empty grey slot. A skin of a collection takes one slot for the
// whole collection, with a "+N" count of the collection's other skins in the store; the collection's later skins in
// SKINS take no slot (v10's `continue` leaves the slot count as it is).
static void popup_items(StrBuf *sb, const char *skins, int *frames, int *nframes) {
    sb_append(sb, "<div style='margin-top:15px;'>");
    const char *p = skins ? skins : "";
    const char *seen_coll[POPUP_SLOTS];
    int nseen = 0;
    for (int slot = 0; slot < POPUP_SLOTS; slot++) {
        const ViewerSkin *k = NULL;
        int index = -1;
        if (*p) {
            char *end = NULL;
            const long v = strtol(p, &end, 10);
            const int whole = end && end != p && (*end == ',' || !*end);
            const char *comma = strchr(p, ',');
            p = comma ? comma + 1 : p + strlen(p);
            if (whole) {
                index = (int) v;
                k = viewer_skin(index);
            }
        }
        if (!k) {
            sb_append(sb, "<div class='bundlItmC' style='border: 3px solid #707070;'></div>");
            continue;
        }
        int coll_n = 0; // iiÎîiíì
        const char *coll = skin_collection(index);
        if (coll) {
            int dup = 0;
            for (int c = 0; c < nseen; c++) dup |= strcmp(seen_coll[c], coll) == 0;
            if (dup) { // already shown: this skin takes no slot
                slot--;
                continue;
            }
            if (nseen < POPUP_SLOTS) seen_coll[nseen++] = coll;
            coll_n = collection_size(coll);
        }
        char thumb[320];
        store_skin_preview(index, thumb, sizeof thumb);
        const int r = k->rarity >= 0 && k->rarity < g_viewer_rarity_count ? k->rarity : 0;
        sb_appendf(sb, "<div class='bundlItmC' style='background:url(%s);", thumb);
        if (k->type == 4 && k->frames > 0) { // iííìíìí(skin, "sprayAnmB") (:24393)
            int seen = 0;
            for (int f = 0; f < *nframes; f++) seen |= frames[f] == k->frames;
            if (!seen && *nframes < POPUP_SLOTS) frames[(*nframes)++] = k->frames;
            sb_appendf(sb, "background-size: cover !important;animation: sprayAnmB%d %.15gs steps(%d) infinite;", k->frames, 1.0 + k->frame_t / 1000.0,
                       k->frames);
        } else {
            sb_append(sb, "background-size: contain;background-position: center;");
        }
        sb_appendf(sb, "border: 3px solid %s;' onclick='viewItem(%d)'>", g_viewer_rarities[r].color, index);
        if (coll_n) sb_appendf(sb, "<div class='bundlItmCnt'>+%d</div>", coll_n - 1);
        sb_append(sb, "</div>");
    }
    sb_append(sb, "</div>");
}

void bundle_offers_popup_show(const cJSON *b, const int has_condition, const double first_seen_ms) {
    if (!b) return;
    const int id = row_id(b), owned = row_owned(b);
    const double price = row_num(b, "PRICE"), disc_pct = row_num(b, "DISCOUNT_PCT");
    const int free_bundle = price == 0; // PRICE === 0: no BUY, no GIFT (:215800)
    const long funds = (long) row_num(b, "FUNDS"), premium = (long) row_num(b, "PREMIUM"); // parseInt
    const double junk = floor(row_num(b, "JUNK") * 100.0 + 0.5) / 100.0;                  // toFixed(2)
    const cJSON *disc_v = cJSON_GetObjectItem(b, "DISCOUNT_PCT");
    const int discounted = disc_v && !cJSON_IsNull(disc_v) && disc_pct > 0; // null != DISCOUNT_PCT && DISCOUNT_PCT > 0
    const int event = is_event(b);
    const int timed = event || row_text(b, "EXPIRY") || has_condition; // iïÍîîiï
    // the seconds left and the period (:215809-215857): the event's, the condition's, else the EXPIRY's
    double ends = bundle_offers_ends_ms(b, has_condition, first_seen_ms);
    const int has_end = event || (has_condition ? first_seen_ms > 0 : row_text(b, "EXPIRY") != NULL);
    long left_s = has_end && ends > 0 ? (long) floor((ends - bundle_offers_now_ms()) / 1000.0) : 0;
    if (left_s < 0) left_s = 0;
    const double period = has_end ? OFFER_PERIOD_MS : 0;

    StrBuf items = {0};
    int frames[POPUP_SLOTS], nframes = 0;
    popup_items(&items, row_text(b, "SKINS"), frames, &nframes);
    char *items_html = sb_finish(&items);

    StrBuf sb = {0};
    if (discounted) sb_appendf(&sb, "<div class='bundlPopDiscountLabel'>-%.0f%% OFF</div>", floor(disc_pct + 0.5)); // Math.round
    if (nframes) {
        sb_append(&sb, "<style>");
        for (int f = 0; f < nframes; f++) sb_appendf(&sb, "@keyframes sprayAnmB%d {100%%{background-position:-%dpx}}", frames[f], 81 * frames[f]);
        sb_append(&sb, "</style>");
    }
    if (timed) sb_appendf(&sb, "<div class=\"bundle-popup-lmt-offer\"><span>%s</span></div>", event ? row_text(b, "EVENT_NAME") : "Time-limited offer!");
    char art[320], path[64];
    snprintf(path, sizeof path, "img/backgrounds/bundles/b_%d.png", id);
    html_dom_asset_url(path, art, sizeof art);
    sb_appendf(&sb, "<div class='bundlPopImg' style='background:url(%s);background-size: cover;'></div><div class='bundlPopCurncs'>", art);
    if (junk > 0)
        sb_appendf(&sb, "<div style='color:#cfcfcf'>+ <span style='color:#7d7d7d;'>%.2f</span> <span class='material-icons' style='color:#cc51f8;font-size:35px;margin-left:-5px;vertical-align:top;'>plumbing</span></div>",
                   junk);
    if (funds) sb_appendf(&sb, "<div style='color:#cfcfcf'>+ <span style='color:#7d7d7d;'>%ld</span> <span style='color:#fbc02d'>KR</span></div>", funds);
    if (premium)
        sb_appendf(&sb, "<div style='color:#cfcfcf'>+ <span style='color:#7d7d7d;'>%ld</span> <span style='color:#42ed56;font-size:18px;'>Premium Days</span></div>", premium);
    // NAME and INFO go in as v10 writes them (innerHTML)
    const char *name = row_text(b, "NAME"), *info = row_text(b, "INFO");
    sb_appendf(&sb, "</div><div class='bundlPopSidH'><div class='bundlPopNam'>%s</div><div class='bundlPopInfo'>%s</div>%s", name ? name : "undefined",
               info ? info : "undefined", items_html);
    free(items_html);
    sb_append(&sb, "<div class='bundlPopBuyH'>");
    const cJSON *orig = cJSON_GetObjectItem(b, "ORIGINAL_PRICE");
    if (discounted && orig && !cJSON_IsNull(orig)) sb_appendf(&sb, "<div class='bundlPopOriginalPrice'>previously $%.2f</div>", row_num(b, "ORIGINAL_PRICE"));
    if (!free_bundle) {
        if (!owned)
            sb_appendf(&sb, "<div class='bundlPopBuyC%s' onclick='selectBundle(this, %d, false, undefined, undefined)'>BUY $%.15g USD</div>",
                       discounted ? " bundlPopBuy--discount" : "", id, price);
        if (owned) sb_appendf(&sb, "<div class='bundlPopBuyGift bundlPopBuyGift--owned' onclick='selectBundleGift(this, %d)'>GIFT $%.15g USD</div>", id, price);
        else sb_appendf(&sb, "<div class='bundlPopBuyGift' onclick='selectBundleGift(this, %d)'>GIFT</div>", id);
    }
    sb_append(&sb, "</div>");
    if (timed && left_s > 0 && period > 0)
        sb_append(&sb, "<div class='strike-timer'><div class='strike-timer__holder'><div id='warning' class='strike-timer__warning' style='display: none;'><img src='./img/warning-octagon-red.svg' alt='Warning'></div>"
                       "<div class='strike-timer__info-holder'><div class='strike-timer__info'><div class='strike-timer__info-text'>Offer ends in:</div><div id='timer-text' class='strike-timer__info-text strike-timer__info-text--time strike-timer__text-mod--chill'>00:00:00</div></div></div>"
                       "<div class='strike-timer__timer-bg'><div class='strike-timer__timer-outer'><svg><circle class='strike-timer__stroke-mod--chill'></circle></svg></div>"
                       "<div class='strike-timer__timer-inner'><svg><circle id='progress-circle' class='strike-timer__stroke-mod--chill' style='stroke-dashoffset: 0%;'></circle></svg></div>"
                       "<div id='timer-center' class='strike-timer__timer-center strike-timer__background-mod--chill'></div>"
                       "<div class='strike-timer__timer-center-img'><img src='./img/skull-small.svg' alt='Timer icon'></div></div></div></div>");
    sb_append(&sb, "</div>"); // .bundlPopSidH (v10 leaves it to the parser)
    char *html = sb_finish(&sb);
    ui_loadout_reset_pops(); // resetPops(): also stops the last bundle's music and countdown
    const HtmlElem pop = html_dom_by_id("bundlePop");
    if (pop) {
        html_dom_set_display(pop, "block");
        html_dom_set_html(pop, html);
    }
    free(html);
    // the bundle's music: bundle_<ID> looped, at 1 for bundle 12 and 0.1 otherwise, rate 1.25 for bundle 6 (:215942)
    if (id > 1) {
        snprintf(B.music, sizeof B.music, "bundle_%d", id);
        audio_play(B.music, id == 12 ? 1.0f : 0.1f, 1, id == 6 ? 1.25f : 1.0f);
    }
    char key[32], now[32];
    snprintf(key, sizeof key, "bundle_%d", id);
    snprintf(now, sizeof now, "%.0f", bundle_offers_now_ms());
    html_storage_set(key, now);
    html_storage_set("lastBundlePopupShown", now);
    if (timed && left_s > 0 && period > 0) {
        B.left_s = left_s;
        B.period_ms = period;
        tick(NULL);
    }
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
    bundle_offers_readable_time(left_ms, left, sizeof left); // getReadableTime(Math.max(0, floor(end - now)))
    snprintf(path, sizeof path, "img/backgrounds/bundles/b_%d_tiny.png", bundle_id);
    html_dom_asset_url(path, url, sizeof url);
    StrBuf sb = {0};
    sb_appendf(&sb,
               "<div id=\"ltm-offer-container\" class=\"ltm-offer-container\">"
               "<div class=\"ltm-offer-container__time-bar-bg\"><div class=\"ltm-offer-container__time-bar-progress\"></div></div>"
               "<div class=\"ltm-offer-container__arrow-container\"><div class=\"ltm-offer-container__arrow\"></div></div>"
               "<div class=\"ltm-offer-container__body\"><div class=\"ltm-offer-container__text-holder\">"
               "<div class=\"ltm-offer-container__lmt-text\">Time-limited Offer Available!</div>"
               "<div class=\"ltm-offer-container__time-left\">Time left: %s</div></div>"
               "<div class=\"ltm-offer-container__img-holder\"><img src=%s alt=\"Timer icon\"></div></div></div>",
               left, url);
    char *html = sb_finish(&sb);
    html_dom_insert_html(shop, HTML_INSERT_BEFORE_END, html);
    free(html);
    B.ltm_timer = html_timer_add(LTM_NOTE_MS, ltm_remove, NULL);
}

// ---------------------------------------------------------------- a Box Office bundle's popup

// bundlePopup(true, false, id) on Box Office (deob-10.0.7.js:215745-215950): the brain's row in v10's popup. Box
// Office bundles carry no condition. With no such row v10 redraws the Store (updateWindow(14)).
void bundle_offers_popup(const int id) {
    const cJSON *b = row_by_id(junk_shop_bundles_rows(), id);
    if (!b) {
        menu_rerender_if_open(14);
        return;
    }
    bundle_offers_popup_show(b, 0, 0);
}
