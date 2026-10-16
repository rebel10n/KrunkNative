// Krunker v10.0.2's Wild Offers (game.deob.js g:127883-128760; the shop API g:26553-26573): one
// discounted item per player at a time, from kn_brain's social socket (services/brain/wild_offer.c:
// ["wo"] / ["wob", id] / ["wor", id], v10's shop.getWildOffer / purchaseWildOffer / refreshWildOffer).
//   - the store (iìiïiiï, g:127884): {offer, lastFetched, lastShownOfferId (getSavedVal
//     "wildOfferLastShownId")}; a fetch after sign-in, again when it is older than 5 min and the
//     Store opens;
//   - the notification (svelte-1gcqcwy, g:128032): pushed to the notification center for 30 s once
//     per offer id (shouldShowNotification: a new id and not in a ranked match), with the item,
//     the prices, the time left, Buy Now (the purchase confirm), View Shop and its close button;
//   - the shop item (svelte-m5s1iy, g:128540-128760): the 300 px item card in the Store's shop front
//     with the New Offer! refresh button, the Wild Offer header and countdown, the prices and a BUY
//     button; a click opens the item view (svelte-2tp35a) with the banner (svelte-19z3gl4) and the
//     showcase (svelte-rsgogi: original price, discount, BUY, "You save");
//   - purchase (iíîiîîï, g:128166): the confirm (Are you sure you want to purchase this item?),
//     then the success popup "Wild Offer Purchased!" / "Awesome!", the skin added, the offer gone
//     and fetched again without a notification; refresh (iíïìíïí, g:128255): its confirm ("Get a New
//     Wild Offer!", "Tired of this offer? Refresh!", "Get New Offer!"), then "Wild Offer Refreshed!".
// KrunkNative: KR (virtual) offers only (no payments: v10's real-money ones go to showPurchase);
// the Store's shop front holds only the wild offer (v10's other shop displays come from its shop
// API: KrunkNative has none), on the Eterno Shop tab above its empty state.
#include <v10_wild_offer.h>
#include <v10_notify.h>
#include <v10_ui.h>
#include <backend.h>
#include <backend_ui.h>
#include <ui_logic.h>
#include <html_ui.h>
#include <ks_game.h> // ks_wall_ms
#include "../viewer/viewer_store.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define WO_NOTIFY_MS 30000.0    // iíiìïïi(..., {duration: 30000})
#define WO_STALE_MS 300000.0    // isStale: lastFetched older than 300000 ms
#define WO_SHOWN_KEY "wildOfferLastShownId"

typedef enum { CONFIRM_NONE, CONFIRM_BUY, CONFIRM_REFRESH } Confirm;

static struct {
    cJSON *offer;          // the brain's offer (NULL: none)
    int account_id;        // whose offer
    int fetched;           // a "wo" reply arrived
    double fetched_at;     // its HUD clock ms
    int asking;            // a "wo" is on its way
    int notify_next;       // the next "wo" reply may notify (the first fetch after sign-in)
    int notif_id;          // the notification center's item, 0 none
    int purchasing, refreshing;
    Confirm confirm;
    int scenario;          // --render-scenario: no brain
    int in_ranked;         // the game is a ranked match (Game.is_ranked: no notification)
    double now_pinned;     // scenario: Date.now() (epoch ms), 0 = the wall clock
} W;

// Date.now(): epoch ms (ks_wall_ms: it has the Windows path; a --render-scenario pins it with
// state.wildOffer.now)
static double now_ms(void) { return W.now_pinned > 0.0 ? W.now_pinned : ks_wall_ms(); }

// ------------------------------------------------------------------ offer fields

static int offer_skin(const cJSON *o) {
    const cJSON *c = cJSON_GetArrayItem(cJSON_GetObjectItem(o, "contents"), 0);
    const char *ref = v10_str(c, "ref");
    return !strcmp(v10_str(c, "type"), "skin") && ref[0] ? atoi(ref) : -1;
}

static double price_of(const cJSON *o, const char *key) {
    return v10_num(cJSON_GetObjectItem(cJSON_GetObjectItem(o, key), "virtual"), "amount", 0);
}

static const char *currency_of(const cJSON *o) {
    const char *c = v10_str(cJSON_GetObjectItem(cJSON_GetObjectItem(o, "price"), "virtual"), "currency");
    return c[0] ? c : "KR";
}

static int offer_id(const cJSON *o) { return (int) v10_num(o, "id", 0); }

// expires_at "YYYY-MM-DD HH:MM:SS" (UTC; v10: replace(" ", "T") + "Z") as epoch ms, 0 unknown
static double expires_ms(const cJSON *o) {
    int y, mo, d, h, mi, s;
    if (sscanf(v10_str(o, "expires_at"), "%d-%d-%d%*c%d:%d:%d", &y, &mo, &d, &h, &mi, &s) != 6) return 0;
    // days from the civil date (UTC, no timegm on every platform)
    y -= mo <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned) (y - era * 400);
    const unsigned doy = (153u * (unsigned) (mo + (mo > 2 ? -3 : 9)) + 2u) / 5u + (unsigned) d - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    const double days = (double) era * 146097.0 + (double) doe - 719468.0;
    return ((days * 24.0 + h) * 60.0 + mi) * 60000.0 + s * 1000.0;
}

static int available(const cJSON *o) { // isAvailable: an offer, no status, not expired
    if (!o || cJSON_IsString(cJSON_GetObjectItem(o, "status"))) return 0;
    const double at = expires_ms(o);
    return !at || now_ms() < at;
}

// ------------------------------------------------------------------ markup

// the notification's time left (IÍíîííï): "Expired" or "<h>h <m>m"
static void hours_left(const cJSON *o, char *out, const size_t n) {
    const double left = expires_ms(o) - now_ms();
    if (left <= 0) snprintf(out, n, "Expired");
    else snprintf(out, n, "%dh %dm", (int) floor(left / 3600000.0), (int) floor(fmod(left, 3600000.0) / 60000.0));
}

// the countdown component (svelte-68lwmp, g:112539): d h m / h m s / m s / s
static void countdown(StrBuf *sb, const cJSON *o) {
    const double left = expires_ms(o) - now_ms();
    char t[48];
    if (left <= 0) snprintf(t, sizeof(t), "Expired");
    else {
        const int d = (int) floor(left / 86400000.0), h = (int) floor(fmod(left, 86400000.0) / 3600000.0);
        const int m = (int) floor(fmod(left, 3600000.0) / 60000.0), s = (int) floor(fmod(left, 60000.0) / 1000.0);
        if (d > 0) snprintf(t, sizeof(t), "%dd %dh %dm", d, h, m);
        else if (h > 0) snprintf(t, sizeof(t), "%dh %dm %ds", h, m, s);
        else if (m > 0) snprintf(t, sizeof(t), "%dm %ds", m, s);
        else snprintf(t, sizeof(t), "%ds", s);
    }
    sb_appendf(sb, "<div class=\"countdown-container svelte-68lwmp%s\"><span class=\"material-icons svelte-68lwmp\">schedule</span> <span class=\"time-text svelte-68lwmp\">%s</span></div>",
               left <= 0 ? " expired" : "", t);
}

// the price button (svelte-1ov1s5h): BUY <currency display>, or Buying...
static void buy_button(StrBuf *sb, const cJSON *o, const char *size, const char *onclick, const int disabled) {
    const int small = !strcmp(size, "small"), large = !strcmp(size, "large");
    sb_appendf(sb, "<button class=\"buy-button svelte-1ov1s5h %s\" type=\"button\" tabindex=\"0\" style=\"padding: %s; font-size: %s;\" onclick=\"%s\"%s>",
               size, small ? "6px 10px" : large ? "12px 24px" : "8px 12px", small ? "12px" : large ? "16px" : "14px", onclick,
               disabled || W.purchasing ? " disabled" : "");
    if (W.purchasing) sb_appendf(sb, "<span class=\"material-icons svelte-1ov1s5h\" style=\"font-size: %s\">hourglass_empty</span> <span>Buying...</span>",
                                 small ? "18px" : large ? "20px" : "24px");
    else {
        sb_append(sb, "BUY ");
        v10_currency(sb, price_of(o, "price"), currency_of(o), "small", 0);
    }
    sb_append(sb, "</button>");
}

static char *notification_html(const cJSON *o) {
    const int skin = offer_skin(o);
    const ViewerSkin *k = viewer_skin(skin);
    char left[32];
    hours_left(o, left, sizeof(left));
    StrBuf sb = {0};
    sb_appendf(&sb, "<div class=\"wild-offer-notification svelte-1gcqcwy\"><div class=\"notification-header svelte-1gcqcwy\"><div class=\"header-left svelte-1gcqcwy\">"
                    "<div class=\"notification-icon material-icons svelte-1gcqcwy\">local_fire_department</div> <div class=\"header-text svelte-1gcqcwy\"><div class=\"title-row svelte-1gcqcwy\">"
                    "<div class=\"notification-title svelte-1gcqcwy\">%s</div> <div class=\"offer-timer svelte-1gcqcwy\"><div class=\"timer-icon material-icons svelte-1gcqcwy\">schedule</div> "
                    "<div class=\"timer-text svelte-1gcqcwy\">%s</div></div></div> <div class=\"notification-message svelte-1gcqcwy\">%s</div></div></div> "
                    "<button class=\"dismiss-btn svelte-1gcqcwy\" aria-label=\"Close notification\" onclick=\"knWoDismiss()\" onmouseenter=\"playTick(0.1)\">\xe2\x9c\x95</button></div> "
                    "<div class=\"notification-content svelte-1gcqcwy\"><div class=\"product-section svelte-1gcqcwy\">",
               I18N("generic.wildOffer.title", "Wild Offer!"), left, I18N("generic.wildOffer.message0", "LUCKY YOU! Nice offer - grab it!"));
    if (k) {
        sb_append(&sb, "<div class=\"item-container svelte-1gcqcwy\">");
        v10_market_card(&sb, skin, 80, 80, "top-center", NULL, NULL);
        sb_append(&sb, "</div> <div class=\"item-info svelte-1gcqcwy\"><div class=\"item-name svelte-1gcqcwy\">");
        sb_append_esc(&sb, k->name ? k->name : "");
        sb_appendf(&sb, "</div> <div class=\"price-container svelte-1gcqcwy\"><span class=\"original-price svelte-1gcqcwy\">%.0f %s</span> "
                        "<span class=\"discount-badge svelte-1gcqcwy\">-%.0f%%</span> <span class=\"current-price svelte-1gcqcwy\">%.0f %s</span></div></div>",
                   price_of(o, "original_price"), currency_of(o), v10_num(o, "discount_pct", 0), price_of(o, "price"), currency_of(o));
    } else sb_append(&sb, "<!---->");
    sb_appendf(&sb, "</div> <div class=\"actions-section svelte-1gcqcwy\"><button class=\"buy-btn svelte-1gcqcwy\" onclick=\"knWoBuy()\" onmouseenter=\"playTick(0.1)\"%s>"
                    "<span class=\"material-icons svelte-1gcqcwy\">shopping_cart</span> %s</button> <button class=\"shop-btn svelte-1gcqcwy\" onclick=\"knWoShop()\" onmouseenter=\"playTick(0.1)\">"
                    "<span class=\"material-icons svelte-1gcqcwy\">store</span> %s</button></div></div></div>",
               W.purchasing || !strcmp(left, "Expired") ? " disabled" : "", W.purchasing ? "Purchasing..." : I18N("generic.wildOffer.buyNow", "Buy Now"),
               I18N("generic.wildOffer.viewShop", "View Shop"));
    return sb_finish(&sb);
}

void v10_wild_offer_shop(StrBuf *sb) {
    if (!W.offer || !available(W.offer) || offer_skin(W.offer) < 0) return;
    const cJSON *o = W.offer;
    const int expired = expires_ms(o) && now_ms() >= expires_ms(o);
    StrBuf c = {0};
    // children of the 300 px card (iiíïïîï): EXPIRED overlay, New Offer!, the bottom overlay
    if (expired) sb_append(&c, "<div class=\"sideways-overlay svelte-m5s1iy\"><div class=\"sideways-text svelte-m5s1iy\">EXPIRED</div></div>");
    else sb_append(&c, "<!---->");
    sb_append(&c, " ");
    if (!expired) {
        const double cost = v10_num(o, "refresh_cost", 300);
        sb_appendf(&c, "<div class=\"refresh-button-container svelte-m5s1iy\"><button class=\"refresh-button svelte-m5s1iy\"%s title=\"Get a new wild offer for %.0f KR!\" onclick=\"event.stopPropagation();knWoRefresh()\">"
                       "<span class=\"material-icons refresh-icon svelte-m5s1iy\">%s</span> <span class=\"refresh-text svelte-m5s1iy\">New Offer!</span> <span class=\"refresh-cost svelte-m5s1iy\">%.0f KR</span></button></div>",
                   W.refreshing ? " disabled" : "", cost, W.refreshing ? "refresh" : "autorenew", cost);
    } else sb_append(&c, "<!---->");
    sb_append(&c, " <div class=\"bottom-overlay svelte-m5s1iy\"><div class=\"row svelte-m5s1iy\"><div class=\"wild-offer-header svelte-m5s1iy\"><div class=\"fire-icon material-icons svelte-m5s1iy\">local_fire_department</div> "
                  "<span class=\"wild-offer-text svelte-m5s1iy\">Wild Offer</span></div> ");
    if (expires_ms(o)) countdown(&c, o);
    else sb_append(&c, "<!---->");
    sb_appendf(&c, "</div> <div class=\"row svelte-m5s1iy\"><div class=\"price-section svelte-m5s1iy\"><div class=\"price-row svelte-m5s1iy\"><span class=\"original-price svelte-m5s1iy\">%.0f %s</span> "
                   "<span class=\"discount-badge svelte-m5s1iy\">-%.0f%%</span></div> <div class=\"current-price svelte-m5s1iy\">%.0f %s</div></div> ",
               price_of(o, "original_price"), currency_of(o), v10_num(o, "discount_pct", 0), price_of(o, "price"), currency_of(o));
    buy_button(&c, o, "small", "event.stopPropagation();knWoBuy()", expired);
    sb_append(&c, "</div></div>");
    char *children = sb_finish(&c);
    sb_append(sb, "<div class=\"shop-front svelte-1nzacef\"><div class=\"shop-front-grid svelte-1nzacef\"><div class=\"shop-item-wrapper svelte-m5s1iy\">");
    v10_market_card(sb, offer_skin(o), 300, 300, "top-center", "playSelect();knWoView()", children);
    sb_append(sb, "</div></div> <!----></div>");
    free(children);
}

// the item view (iîíííiî, svelte-2tp35a) with the banner as its header and the showcase as its children
static void item_view(void) {
    const cJSON *o = W.offer;
    const int skin = offer_skin(o);
    const ViewerSkin *k = viewer_skin(skin);
    if (!k) return;
    const char *color = k->rarity >= 0 && k->rarity < g_viewer_rarity_count && g_viewer_rarities[k->rarity].color ? g_viewer_rarities[k->rarity].color : "#B0B0B0";
    char img[512];
    store_skin_preview(skin, img, sizeof(img));
    StrBuf sb = {0};
    sb_appendf(&sb, "<div class=\"popup-container svelte-1wz7ymp\"><div class=\"popup svelte-1wz7ymp translucent slim-border\" style=\"width: 600px;\">"
                    "<div class=\"item-showcase-container io-popup-container svelte-2tp35a\"><div class=\"item-showcase-content io-popup-content svelte-2tp35a\">"
                    "<div class=\"image-display svelte-2tp35a\" style=\"border: 3px solid %s; background-image: url(%s);\"></div> "
                    "<div class=\"showcase-overlay svelte-2tp35a\"><!----> <div class=\"showcase-header svelte-2tp35a\">"
                    "<div class=\"wild-offer-banner svelte-19z3gl4\"><div class=\"fire-icon material-icons svelte-19z3gl4\">local_fire_department</div> "
                    "<span class=\"wild-offer-title svelte-19z3gl4\">WILD OFFER</span> <div class=\"discount-badge svelte-19z3gl4\">-%.0f%% OFF</div></div> "
                    "<div class=\"item-showcase-header svelte-2tp35a\"><div class=\"item-info svelte-2tp35a\"><!----> <div class=\"item-name svelte-2tp35a\" style=\"color: %s;\">",
               color, img, v10_num(o, "discount_pct", 0), color);
    sb_append_esc(&sb, k->name ? k->name : "Unnamed Item");
    sb_append(&sb, "</div> <!----> <div class=\"item-description svelte-2tp35a\">");
    static const char *const types[] = {NULL, "Hat Item", "Body Item", "Melee Item", "Spray Item", "Dye Item", "Waist Item", "Face Item", "Shoe Item", "Pet Item",
                                        "Collectible Item", "Wrist Item", "Charm Item", "Spin Ticket Item", "Back Item", "Head Item", "Player Card"};
    if (k->type > 0 && k->type < (int) (sizeof(types) / sizeof(types[0]))) sb_append(&sb, types[k->type]);
    const double original = price_of(o, "original_price"), price = price_of(o, "price");
    sb_appendf(&sb, "</div> <!----></div> <div class=\"item-showcase-btns svelte-2tp35a\"><button class=\"more-info-btn svelte-2tp35a\" onclick=\"openSales(%d)\">More info</button> <!----></div></div></div> "
                    "<div class=\"wild-offer-showcase svelte-rsgogi\"><div class=\"purchase-section svelte-rsgogi\"><div class=\"price-container svelte-rsgogi\"><div class=\"original-price-container svelte-rsgogi\">"
                    "<span class=\"original-price-label svelte-rsgogi\">Original Price</span> <span class=\"original-price svelte-rsgogi\">%.0f %s</span></div> "
                    "<div class=\"discount-arrow svelte-rsgogi\"><div class=\"arrow-icon material-icons svelte-rsgogi\">arrow_downward</div> <div class=\"discount-percent svelte-rsgogi\">-%.0f%%</div></div></div> "
                    "<div class=\"io-button-group svelte-rsgogi\">",
               skin, original, currency_of(o), v10_num(o, "discount_pct", 0));
    buy_button(&sb, o, "large", "knWoBuy()", 0);
    sb_appendf(&sb, "</div> <div class=\"savings-container svelte-rsgogi\"><div class=\"savings-icon material-icons svelte-rsgogi\">savings</div> "
                    "<span class=\"savings-text svelte-rsgogi\">You save <span class=\"savings-amount svelte-rsgogi\">%.0f</span> %s!</span></div></div></div>"
                    "</div></div></div></div></div>",
               original - price, currency_of(o));
    v10_view_push(sb_finish(&sb), NULL, NULL, NULL);
}

// ------------------------------------------------------------------ actions

static void ask(const int notify) {
    if (W.scenario || !active_account()) return;
    W.asking = 1;
    W.notify_next = W.notify_next || notify;
    account_send_frame("wo");
}

static void rerender(void) {
    menu_rerender_if_open(14);
    if (W.notif_id && W.offer) { // the notification's buttons follow purchasing
        char id[32];
        snprintf(id, sizeof(id), "knNotif%d", W.notif_id);
        const HtmlElem item = html_dom_by_id(id);
        const HtmlElem body = item ? html_dom_query(item, ".wild-offer-notification") : 0;
        if (body) { // outerHTML = the component again
            char *html = notification_html(W.offer);
            html_dom_insert_html(body, HTML_INSERT_BEFORE_BEGIN, html);
            html_dom_remove(body);
            free(html);
        }
    }
}

static void send_with_id(const char *label) {
    char frame[64];
    snprintf(frame, sizeof(frame), "[\"%s\",%d]", label, offer_id(W.offer));
    backend_social_send_text(frame);
}

static void confirm_closed(const int accepted, void *user) {
    (void) user;
    const Confirm what = W.confirm;
    W.confirm = CONFIRM_NONE;
    if (!accepted || !W.offer) return;
    if (what == CONFIRM_BUY) {
        W.purchasing = 1;
        send_with_id("wob");
    } else if (what == CONFIRM_REFRESH) {
        W.refreshing = 1;
        send_with_id("wor");
    }
    rerender();
}

// iiiïìíî: the purchase confirm (item, price, currency; title / question / confirm text optional)
static void confirm(const Confirm what, const double price, const char *title, const char *question, const char *ok) {
    StrBuf sb = {0};
    sb_appendf(&sb, "<div class=\"popup-container svelte-1wz7ymp\"><div class=\"popup svelte-1wz7ymp\" style=\"width: 450px;\">"
                    "<div class=\"confirm-purchase-container io-popup-container\"><div class=\"confirm-purchase-header io-popup-header io-popup-header--slim\">"
                    "<svg xmlns=\"http://www.w3.org/2000/svg\" class=\"confirm-icon svelte-1p13km7\" viewBox=\"0 0 24 24\" fill=\"currentColor\">"
                    "<path d=\"M12 2C6.48 2 2 6.48 2 12s4.48 10 10 10 10-4.48 10-10S17.52 2 12 2zm-2 15l-5-5 1.41-1.41L10 14.17l7.59-7.59L19 8l-9 9z\" class=\"svelte-1p13km7\"></path></svg>"
                    " %s</div> <div class=\"confirm-purchase-content io-popup-content svelte-1p13km7\"><div class=\"purchase-preview svelte-1p13km7\">"
                    "<div class=\"item-display-container svelte-1p13km7\">",
               title);
    v10_market_card(&sb, offer_skin(W.offer), 120, 120, "top-center", NULL, NULL);
    sb_appendf(&sb, "</div> <div class=\"purchase-info svelte-1p13km7\"><!----> <div class=\"purchase-question svelte-1p13km7\">%s</div> <div class=\"price-display\">", question);
    v10_currency(&sb, price, currency_of(W.offer), "large", 1);
    sb_appendf(&sb, "</div></div></div> <div class=\"io-button-group svelte-1p13km7\"><button type=\"button\" class=\"io-button\" onclick=\"knViewPop(0)\">Cancel</button> "
                    "<button type=\"button\" class=\"io-button io-button--accept\" onclick=\"knViewPop(1)\">%s</button></div></div></div></div></div>",
               ok);
    W.confirm = what;
    v10_view_push(sb_finish(&sb), NULL, confirm_closed, NULL);
}

static void dismiss_notification(void) {
    if (W.notif_id) v10_notify_dismiss(W.notif_id);
    W.notif_id = 0;
}

static void show_notification(void) {
    const int id = offer_id(W.offer);
    char saved[32] = "";
    html_storage_get(WO_SHOWN_KEY, saved, sizeof(saved));
    if (saved[0] && atoi(saved) == id) return;                  // lastShownOfferId === id
    if (W.in_ranked) return;                                    // the game's isRanked
    dismiss_notification();
    char *html = notification_html(W.offer);
    W.notif_id = v10_notify_push_html("Wild Offer", html, WO_NOTIFY_MS);
    free(html);
    char v[24];
    snprintf(v, sizeof(v), "%d", id);
    html_storage_set(WO_SHOWN_KEY, v);                          // markOfferAsShown
    printf("[wildoffer] notified offer %d (skin %d)\n", id, offer_skin(W.offer));
    fflush(stdout);
}

static void set_offer(const cJSON *offer) {
    cJSON_Delete(W.offer);
    W.offer = cJSON_IsObject(offer) ? cJSON_Duplicate(offer, 1) : NULL;
    W.fetched = 1;
    W.fetched_at = now_ms();
}

int v10_wild_offer_frame(const char *label, const cJSON *frame) {
    if (!label) return 0;
    if (!strcmp(label, "wo")) { // ["wo", offer | null]
        const int notify = W.notify_next;
        W.asking = W.notify_next = 0;
        set_offer(cJSON_GetArrayItem(frame, 1));
        printf("[wildoffer] offer %d\n", W.offer ? offer_id(W.offer) : 0);
        fflush(stdout);
        if (notify && W.offer && available(W.offer)) show_notification();
        rerender();
        return 1;
    }
    if (!strcmp(label, "wob")) { // ["wob", {success, rewards, funds}] | ["wob", null, err]
        W.purchasing = 0;
        const cJSON *r = cJSON_GetArrayItem(frame, 1);
        if (!cJSON_IsObject(r)) {
            const char *err = cJSON_GetStringValue(cJSON_GetArrayItem(frame, 2));
            v10_view_push(v10_io_error("Error", err ? err : "Purchase failed. Please try again."), NULL, NULL, NULL);
            rerender();
            return 1;
        }
        const cJSON *rw;
        StrBuf sb = {0};
        cJSON_ArrayForEach(rw, cJSON_GetObjectItem(r, "rewards")) {
            if (strcmp(v10_str(rw, "type"), "skin")) continue;
            const int skin = (int) v10_num(rw, "skin_id", -1);
            account_add_skin(skin);
            const ViewerSkin *k = viewer_skin(skin);
            sb_append(&sb, "<div style=\"display: flex; justify-content: center; margin: 10px 0;\">");
            v10_market_card(&sb, skin, 120, 120, "top-center", NULL, NULL);
            sb_append(&sb, "</div>");
            printf("[wildoffer] bought offer %d: %s (skin %d)\n", offer_id(W.offer), k && k->name ? k->name : "?", skin);
            fflush(stdout);
        }
        if (cJSON_IsNumber(cJSON_GetObjectItem(r, "funds"))) account_set_funds(v10_num(r, "funds", 0));
        char *rewards = sb_finish(&sb);
        v10_view_clear();
        v10_view_push(v10_io_success("Wild Offer Purchased!", rewards ? rewards : "", "Awesome!", "accept slim"), NULL, NULL, NULL);
        free(rewards);
        dismiss_notification();
        set_offer(NULL);
        ask(0); // setTimeout(() => fetch(false), 100): the next one, no notification
        rerender();
        return 1;
    }
    if (!strcmp(label, "wor")) { // ["wor", {success, refresh_cost, offer, funds}] | ["wor", null, err]
        W.refreshing = 0;
        const cJSON *r = cJSON_GetArrayItem(frame, 1);
        if (!cJSON_IsObject(r)) {
            const char *err = cJSON_GetStringValue(cJSON_GetArrayItem(frame, 2));
            v10_view_push(v10_io_error("Error", err ? err : "Refresh failed. Please try again."), NULL, NULL, NULL);
            rerender();
            return 1;
        }
        if (cJSON_IsNumber(cJSON_GetObjectItem(r, "funds"))) account_set_funds(v10_num(r, "funds", 0));
        dismiss_notification();
        set_offer(cJSON_GetObjectItem(r, "offer"));
        printf("[wildoffer] refreshed: offer %d\n", W.offer ? offer_id(W.offer) : 0);
        fflush(stdout);
        v10_view_clear();
        v10_view_push(v10_io_success("Wild Offer Refreshed!", "You have refreshed your wild offer and received a new one.", "Awesome!", "accept slim"), NULL, NULL, NULL);
        rerender();
        return 1;
    }
    return 0;
}

void v10_wild_offer_tick(const Client *client) {
    W.in_ranked = client && client->game.is_ranked;
    const AccountInfo *acc = active_account();
    const int id = acc ? acc->id : 0;
    if (W.scenario) return;
    if (id != W.account_id) { // sign-in / out: the offer is someone else's
        cJSON_Delete(W.offer);
        W.offer = NULL;
        dismiss_notification();
        W.fetched = W.asking = W.purchasing = W.refreshing = 0;
        W.account_id = id;
        if (id) ask(1);
    }
}

void v10_wild_offer_store_opened(void) {
    if (!W.asking && active_account() && (!W.fetched || now_ms() - W.fetched_at > WO_STALE_MS)) ask(0);
}

int v10_wild_offer_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) ev;
    (void) args;
    (void) argc;
    if (!name || strncmp(name, "knWo", 4)) return -1;
    if (!strcmp(name, "knWoDismiss")) { // the notification's ✕ (onDismiss)
        dismiss_notification();
        return 1;
    }
    if (!strcmp(name, "knWoShop")) { // View Shop: windows[13].switchTab(7) (the shop front) + showWindow(14)
        dismiss_notification();
        store_ui_open_tab(5);
        return 1;
    }
    if (!W.offer || !available(W.offer)) return 1;
    if (!active_account() && !W.scenario) {
        html_ui_dispatch("loginOrRegister", NULL, NULL, 0);
        return 1;
    }
    if (!strcmp(name, "knWoView")) item_view();
    else if (!strcmp(name, "knWoBuy") && !W.purchasing)
        confirm(CONFIRM_BUY, price_of(W.offer, "price"), "Confirm purchase?", "Are you sure you want to purchase this item?", "Buy");
    else if (!strcmp(name, "knWoRefresh") && !W.refreshing)
        confirm(CONFIRM_REFRESH, v10_num(W.offer, "refresh_cost", 300), "Get a New Wild Offer!", "Tired of this offer? Refresh!", "Get New Offer!");
    return 1;
}

// --render-scenario state.wildOffer: {offer: {...}, now: epoch ms, notify: bool, view: bool} (the brain's "wo" reply)
void v10_wild_offer_scenario(const cJSON *state) {
    if (!cJSON_IsObject(state)) return;
    W.scenario = 1;
    W.now_pinned = v10_num(state, "now", 0);
    set_offer(cJSON_GetObjectItem(state, "offer"));
    if (W.offer && cJSON_IsTrue(cJSON_GetObjectItem(state, "notify"))) {
        html_storage_set(WO_SHOWN_KEY, "");
        show_notification();
    }
    rerender();
    if (W.offer && cJSON_IsTrue(cJSON_GetObjectItem(state, "view"))) item_view();
}
