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

void junk_shop_bundles_render(StrBuf *sb) {
    const V10Req *r = v10_get("/api/shop/bundles", BUNDLES_TTL_MS);
    const cJSON *data = r && r->status == 200 && r->json ? cJSON_GetObjectItem(r->json, "data") : NULL;
    if (!cJSON_IsArray(data)) return; // loading, or an error: nothing to draw
    const cJSON *b;
    cJSON_ArrayForEach(b, data) {
        const cJSON *id = cJSON_GetObjectItem(b, "ID");
        if (!cJSON_IsNumber(id)) continue;
        char art[320];
        char path[64];
        snprintf(path, sizeof path, "img/backgrounds/bundles/b_%d.png", (int) id->valuedouble);
        html_dom_asset_url(path, art, sizeof art);
        const double price = bundle_num(b, "PRICE");
        const double disc = bundle_num(b, "DISCOUNT_PCT");
        const double orig = bundle_num(b, "ORIGINAL_PRICE");
        const int discounted = disc > 0;
        const int owned = cJSON_IsTrue(cJSON_GetObjectItem(b, "OWNED")); // v10: an owned bundle draws grayscale
        sb_appendf(sb, "<div class=\"bundle-card svelte-lri212\" style=\"background-image: url(%s); background-repeat: no-repeat; background-size: cover;%s\">",
                   art, owned ? " filter: grayscale(1);" : "");
        if (discounted) sb_appendf(sb, "<div class=\"bundle-discount-label svelte-lri212\">-%d%%</div>", (int) (disc + 0.5));
        sb_append(sb, "<div class=\"bundle-icon svelte-lri212\" role=\"button\" tabindex=\"0\"><div class=\"bundle-item-name svelte-lri212\">");
        esc_into(sb, bundle_text(b, "NAME"));
        sb_append(sb, "</div></div>");
        // the skins (SKINS: comma-separated indices), four thumbnails and "+N" for the rest
        sb_append(sb, "<div class=\"bundle-highlights svelte-lri212\">");
        const char *skins = bundle_text(b, "SKINS");
        int shown = 0, total = 0;
        for (const char *p = skins; *p;) {
            char *end = NULL;
            const long idx = strtol(p, &end, 10);
            if (end == p) break;
            total++;
            if (shown < 4) {
                char thumb[320];
                store_skin_preview((int) idx, thumb, sizeof thumb);
                sb_appendf(sb, "<div class=\"bundle-item-req svelte-lri212\" style=\"background-image: url(%s)\"></div>", thumb);
                shown++;
            }
            p = end;
            while (*p == ',' || *p == ' ') p++;
        }
        if (total > shown) sb_appendf(sb, "<div class=\"bundle-item-more svelte-lri212\">+%d</div>", total - shown);
        sb_append(sb, "</div>");
        if (discounted && orig > 0) sb_appendf(sb, "<div class=\"bundle-original-price svelte-lri212\">$%.2f</div>", orig);
        // BUY posts the bundle to the brain (junk_shop_bundle_buy); an owned bundle reads "Already Purchased" (v10's text)
        if (owned)
            sb_append(sb, "<div class=\"bundle-buy-btn svelte-lri212\"><div class=\"bundle-buy-btn-text svelte-lri212\">Already Purchased</div></div></div>");
        else
            sb_appendf(sb, "<div class=\"bundle-buy-btn%s svelte-lri212\" role=\"button\" tabindex=\"0\" onclick=\"knKioShopBundleBuy(%d)\"><div class=\"bundle-buy-btn-text svelte-lri212\">BUY $%.2f</div></div></div>",
                       discounted ? " bundle-buy-btn-discount" : "", (int) id->valuedouble, price);
    }
}

// a bundle's BUY (knKioShopBundleBuy(id)): the brain's refusal (a bundle is USD, Box Office takes no payment) is shown
// as the purchase's refusal; an accepted one reloads the grid
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
