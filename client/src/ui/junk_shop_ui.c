// junk_shop_ui.c - the Junk-priced products of the Box Office Eterno Shop (junk_shop_ui.h). The brain's
// "limited" display (GET /api/shop/displays?tag=limited, shop.c) is drawn by kio_store's display markup
// (the krunker.io shop's own cards, kio_store_display_html); a BUY posts POST /api/shop/purchase with the
// account's session token in the URL (v10's ?token=, v10_url_token) and sets the Junk balance from the
// reply. A guest's BUY is v10's loginOrRegister, as on every v10 shop card.
#include <junk_shop_ui.h>
#include "klog.h"
#include <kio_store.h>
#include <backend_ui.h>
#include <v10_ui.h>
#include <v10_menu.h>
#include <ui_logic.h>
#include <i18n.h>
#include <html_dom.h>
#include <store_spins.h> // store_iso_ms: a bundle's DATE
#include "../viewer/viewer_store.h" // the bundle's skins and their rarity colours

#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SHOP_TTL_MS 60000 // the display's cache (v10's shop store keeps it until a purchase)
#define STORE_WINDOW 14    // windows[13] / the Store: menu_rerender_if_open(14)

static void purchase_done(void *user, const int status, const cJSON *json) {
    const int product = (int) (intptr_t) user;
    v10_expire("/api/shop/displays"); // the purchase count and the limit read again
    const cJSON *data = json ? cJSON_GetObjectItem(json, "data") : NULL;
    if (status == 200 && cJSON_IsObject(data)) {
        const cJSON *junk = cJSON_GetObjectItem(data, "junk");
        if (cJSON_IsString(junk)) account_ui_set_junk(junk->valuestring);
        const cJSON *kr = cJSON_GetObjectItem(data, "kr");
        if (cJSON_IsNumber(kr)) account_ui_set_funds(kr->valuedouble);
        KN_LOG_DEBUG1("[junkshop] bought product %d: junk %s\n", product, cJSON_IsString(junk) ? junk->valuestring : "?");
        fflush(stdout);
    } else {
        printf("[junkshop] purchase of product %d refused (status %d)\n", product, status);
        fflush(stdout);
        const cJSON *message = json ? cJSON_GetObjectItem(json, "message") : NULL;
        const char *text = cJSON_IsString(message) && message->valuestring[0] ? message->valuestring : "Purchase failed";
        v10_view_push(v10_io_error(I18N("kn.junkshop.failedTitle", "Purchase failed"), text), NULL, NULL, NULL);
    }
    menu_rerender_if_open(STORE_WINDOW);
}

// v10_get's completion redraws its popups and views, not the menu windows: while the display loads the open
// Store is drawn again a moment later, so its "Loading eterno shop..." gives way to the products (or the error)
static int g_load_timer;
static void load_poll(void *user) {
    (void) user;
    g_load_timer = 0;
    menu_rerender_if_open(STORE_WINDOW);
}

int junk_shop_render(StrBuf *sb) {
    char t[256], path[400];
    v10_url_token(t, sizeof t);
    snprintf(path, sizeof path, "/api/shop/displays?tag=limited%s%s", t[0] ? "&token=" : "", t);
    const V10Req *r = v10_get(path, SHOP_TTL_MS);
    if (!r || r->status == 0) {
        if (!g_load_timer) g_load_timer = html_timer_add(250.0, load_poll, NULL);
        return 0;
    }
    if (r->status != 200) return -1;
    const cJSON *data = r->json ? cJSON_GetObjectItem(r->json, "data") : NULL;
    const cJSON *products = cJSON_IsObject(data) ? cJSON_GetObjectItem(data, "products") : NULL;
    if (!products || cJSON_GetArraySize(products) == 0) return 1; // no product: nothing to draw
    kio_store_display_html(sb, data);
    return 1;
}

// the bundle card's text: the brain's rows are staff data, so the markup is escaped
static void esc_into(StrBuf *sb, const char *s) {
    for (; s && *s; s++) {
        switch (*s) {
        case '<': sb_append(sb, "&lt;"); break;
        case '>': sb_append(sb, "&gt;"); break;
        case '&': sb_append(sb, "&amp;"); break;
        case '"': sb_append(sb, "&quot;"); break;
        case '\'': sb_append(sb, "&#39;"); break;
        default: sb_appendf(sb, "%c", *s); break;
        }
    }
}

static const char *bundle_text(const cJSON *b, const char *key) {
    const cJSON *v = cJSON_GetObjectItem(b, key);
    return cJSON_IsString(v) ? v->valuestring : "";
}

static double bundle_num(const cJSON *b, const char *key) {
    const cJSON *v = cJSON_GetObjectItem(b, key);
    return cJSON_IsNumber(v) ? v->valuedouble : 0.0;
}

#define BUNDLES_TTL_MS 60000
#define BUNDLES_MAX 128

// v10_get's completion does not redraw the menu windows: while the rows load the open Store is drawn again a moment
// later, so the grid fills in once they arrive
static int g_bundles_timer;
static void bundles_poll(void *user) {
    (void) user;
    g_bundles_timer = 0;
    menu_rerender_if_open(STORE_WINDOW);
}

// IIííîíí's three Array.prototype.sort passes (stable), deob-10.0.3.js:144811-144821
static void bundles_sort(const cJSON **v, const int n, int (*cmp)(const cJSON *, const cJSON *)) {
    for (int i = 1; i < n; i++) {
        const cJSON *x = v[i];
        int j = i;
        for (; j > 0 && cmp(v[j - 1], x) > 0; j--) v[j] = v[j - 1];
        v[j] = x;
    }
}

// new Date(b.DATE) - new Date(a.DATE): newest first; a DATE that does not parse is NaN, which sort() takes as equal
static int by_date(const cJSON *a, const cJSON *b) {
    const int64_t da = store_iso_ms(bundle_text(a, "DATE")), db = store_iso_ms(bundle_text(b, "DATE"));
    if (!da || !db) return 0;
    return db > da ? 1 : db < da ? -1 : 0;
}

static int by_price(const cJSON *a, const cJSON *b) { // a.PRICE - b.PRICE
    const double pa = bundle_num(a, "PRICE"), pb = bundle_num(b, "PRICE");
    return pa > pb ? 1 : pa < pb ? -1 : 0;
}

static int by_owned(const cJSON *a, const cJSON *b) { // (b.OWNED ? -1 : 1) - (a.OWNED ? -1 : 1): owned last
    return (cJSON_IsTrue(cJSON_GetObjectItem(b, "OWNED")) ? -1 : 1) - (cJSON_IsTrue(cJSON_GetObjectItem(a, "OWNED")) ? -1 : 1);
}

// IIííîíí(list): newest first, then cheapest first for a guest or an account of level 10 or below, then owned last
static int bundles_ordered(const cJSON *list, const cJSON **out, const int max) {
    int n = 0;
    const cJSON *b;
    cJSON_ArrayForEach(b, list) if (n < max) out[n++] = b;
    const AccountInfo *acc = active_account();
    bundles_sort(out, n, by_date);
    if (!acc || acc->level <= 10) bundles_sort(out, n, by_price);
    bundles_sort(out, n, by_owned);
    return n;
}

// skins[SKINS.split(",")[k]]: a store index only when the piece is one as JS would key it ("12", not " 12" or "012")
static const ViewerSkin *bundle_skin(const char **p, int *index) {
    const char *s = *p, *e = strchr(s, ',');
    const size_t len = e ? (size_t) (e - s) : strlen(s);
    *p = e ? e + 1 : s + len;
    int ok = len > 0 && len < 7 && (len == 1 || s[0] != '0');
    for (size_t k = 0; ok && k < len; k++) ok = isdigit((unsigned char) s[k]) != 0;
    *index = ok ? atoi(s) : -1;
    return ok ? viewer_skin(*index) : NULL;
}

// One bundle card (svelte-lri212, deob-10.0.3.js:144630-144642, 144978-145057): the art on .bundle-icon with the name,
// the discount label and the struck original price; up to four skin thumbnails in their rarity's border (three and
// "+N" past four), each viewItem(id); BUY "BUY $" + PRICE, or "Already Purchased" (the card grayscale). The art and
// BUY both call playSelect(0.1) then bundlePopup(true, false, ID) (store_ui.c; Box Office has no bundle popup).
static void bundle_card(StrBuf *sb, const cJSON *b, const int id) {
    char art[320], path[64];
    snprintf(path, sizeof path, "img/backgrounds/bundles/b_%d.png", id);
    html_dom_asset_url(path, art, sizeof art);
    const cJSON *disc = cJSON_GetObjectItem(b, "DISCOUNT_PCT"), *orig = cJSON_GetObjectItem(b, "ORIGINAL_PRICE");
    const int discounted = cJSON_IsNumber(disc) && disc->valuedouble > 0;
    const int owned = cJSON_IsTrue(cJSON_GetObjectItem(b, "OWNED"));
    sb_appendf(sb, "<div class=\"bundle-card svelte-lri212\"%s><div class=\"bundle-icon svelte-lri212\" role=\"button\" tabindex=\"0\" style=\"background-image: url(%s); background-repeat: no-repeat; background-size: cover;\" onclick=\"playSelect(0.1);bundlePopup(true,false,%d)\" onmouseenter=\"playTick(0.1)\"><div class=\"bundle-item-name svelte-lri212\">",
               owned ? " style=\"filter: grayscale(1)\"" : "", art, id);
    esc_into(sb, bundle_text(b, "NAME"));
    sb_append(sb, "</div> ");
    if (discounted) sb_appendf(sb, "<div class=\"bundle-discount-label svelte-lri212\">-%.15g%%</div>", floor(disc->valuedouble + 0.5)); // Math.round
    sb_append(sb, " ");
    if (discounted && cJSON_IsNumber(orig)) sb_appendf(sb, "<div class=\"bundle-original-price svelte-lri212\">$%.2f</div>", orig->valuedouble);
    sb_append(sb, "</div> <div class=\"bundle-highlights svelte-lri212\">");
    const char *skins = bundle_text(b, "SKINS");
    int pieces = 1;
    for (const char *c = skins; *c; c++) pieces += *c == ',';
    const int shown = pieces > 4 ? 3 : 4; // past SKINS' end the pieces are undefined: empty boxes
    const char *p = skins;
    for (int k = 0; k < shown; k++) {
        int index = -1;
        const ViewerSkin *skin = k < pieces ? bundle_skin(&p, &index) : NULL;
        if (!skin) {
            sb_append(sb, "<div class=\"bundle-item-req svelte-lri212\"></div>");
            continue;
        }
        char thumb[320];
        store_skin_preview(index, thumb, sizeof thumb);
        const int r = skin->rarity >= 0 && skin->rarity < g_viewer_rarity_count ? skin->rarity : 0;
        sb_appendf(sb, "<div class=\"bundle-item-req svelte-lri212\" role=\"button\" tabindex=\"0\" style=\"border: 2px solid %s; background-image: url(%s)\" onclick=\"viewItem(%d)\"></div>",
                   g_viewer_rarities[r].color, thumb, index);
    }
    sb_append(sb, " ");
    if (pieces > 4) sb_appendf(sb, "<div class=\"bundle-item-req svelte-lri212\"><div class=\"bundle-item-more svelte-lri212\">+%d</div></div>", pieces - shown);
    sb_appendf(sb, "</div> <div class=\"bundle-buy-btn svelte-lri212%s\" role=\"button\" tabindex=\"0\" onclick=\"playSelect(0.1);bundlePopup(true,false,%d)\" onmouseenter=\"playTick(0.1)\"><div class=\"bundle-buy-btn-text svelte-lri212\">",
               discounted ? " bundle-buy-btn-discount" : "", id);
    if (owned) sb_append(sb, "Already Purchased");
    else sb_appendf(sb, "BUY $%.15g", bundle_num(b, "PRICE")); // "BUY $" + PRICE: the JS number, unformatted
    sb_append(sb, "</div></div></div>");
}

// The home tab's bundle columns: [...conditionalBundleData, ...bundleData], each ordered (deob-10.0.3.js:144823). The
// brain sends the account's OWNED with its token. Box Office has no conditional bundles (v10's come with the account's
// bundle-availability push, 242876), so the brain's rows are the list.
void junk_shop_bundles_render(StrBuf *sb) {
    char t[256], path[320];
    v10_url_token(t, sizeof t);
    snprintf(path, sizeof path, "/api/shop/bundles%s%s", t[0] ? "?token=" : "", t);
    const V10Req *r = v10_get(path, BUNDLES_TTL_MS);
    if (!r || r->status == 0) { // loading
        if (!g_bundles_timer) g_bundles_timer = html_timer_add(250.0, bundles_poll, NULL);
        return;
    }
    const cJSON *data = r->status == 200 && r->json ? cJSON_GetObjectItem(r->json, "data") : NULL;
    if (!cJSON_IsArray(data)) return; // an error: nothing to draw
    const cJSON *list[BUNDLES_MAX];
    const int n = bundles_ordered(data, list, BUNDLES_MAX);
    for (int i = 0; i < n; i++) {
        const cJSON *id = cJSON_GetObjectItem(list[i], "ID");
        if (cJSON_IsNumber(id)) bundle_card(sb, list[i], (int) id->valuedouble);
    }
}

// bundlePopup(true, false, ID) from a bundle card (store_ui.c): Box Office has no bundle popup (v10's bundlePop, deob-
// 10.0.3.js:242965-243225: the items, the offer timer, BUY / GIFT through the payment provider), so the card goes
// straight to the bundle's purchase, whose answer is the brain's: a bundle is USD and Box Office takes no payment.
// The brain's refusal is shown as the purchase's refusal; an accepted one reloads the grid
static void bundle_purchase_done(void *user, const int status, const cJSON *json) {
    const int bundle = (int) (intptr_t) user;
    v10_expire("/api/shop/bundles");
    if (status == 200) {
        KN_LOG_DEBUG1("[junkshop] bought bundle %d\n", bundle);
        fflush(stdout);
    } else {
        printf("[junkshop] purchase of bundle %d refused (status %d)\n", bundle, status);
        fflush(stdout);
        const cJSON *message = json ? cJSON_GetObjectItem(json, "message") : NULL;
        const char *text = cJSON_IsString(message) && message->valuestring[0] ? message->valuestring : "Purchase failed";
        v10_view_push(v10_io_error(I18N("kn.junkshop.failedTitle", "Purchase failed"), text), NULL, NULL, NULL);
    }
    menu_rerender_if_open(STORE_WINDOW);
}

void junk_shop_bundle_buy(const int bundle_id) {
    if (!active_account()) {
        v10_menu_handle("loginOrRegister", NULL, NULL, 0);
        return;
    }
    char t[256], path[320], body[64];
    v10_url_token(t, sizeof t);
    snprintf(path, sizeof path, "/api/shop/bundles/purchase?token=%s", t);
    snprintf(body, sizeof body, "{\"bundle_id\":%d}", bundle_id);
    v10_send("POST", path, body, bundle_purchase_done, (void *) (intptr_t) bundle_id);
}

void junk_shop_buy(const int product_id) {
    if (!active_account()) {
        v10_menu_handle("loginOrRegister", NULL, NULL, 0);
        return;
    }
    char t[256], path[320], body[64];
    v10_url_token(t, sizeof t);
    snprintf(path, sizeof path, "/api/shop/purchase?token=%s", t);
    snprintf(body, sizeof body, "{\"product_id\":%d,\"quantity\":1}", product_id);
    v10_send("POST", path, body, purchase_done, (void *) (intptr_t) product_id);
}
