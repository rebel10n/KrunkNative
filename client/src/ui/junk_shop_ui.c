// junk_shop_ui.c - the Junk-priced products of the Box Office Eterno Shop (junk_shop_ui.h). The brain's
// "limited" display (GET /api/shop/displays?tag=limited, shop.c) is drawn by kio_store's display markup
// (the krunker.io shop's own cards, kio_store_display_html); a BUY posts POST /api/shop/purchase with the
// account's session token in the URL (v10's ?token=, v10_url_token) and sets the Junk balance from the
// reply. A guest's BUY is v10's loginOrRegister, as on every v10 shop card.
#include <junk_shop_ui.h>
#include <kio_store.h>
#include <backend_ui.h>
#include <v10_ui.h>
#include <v10_menu.h>
#include <ui_logic.h>
#include <i18n.h>

#include <stdint.h>
#include <stdio.h>
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
        printf("[junkshop] bought product %d: junk %s\n", product, cJSON_IsString(junk) ? junk->valuestring : "?");
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

void junk_shop_render(StrBuf *sb) {
    char t[256], path[400];
    v10_url_token(t, sizeof t);
    snprintf(path, sizeof path, "/api/shop/displays?tag=limited%s%s", t[0] ? "&token=" : "", t);
    const V10Req *r = v10_get(path, SHOP_TTL_MS);
    const cJSON *data = r && r->status == 200 && r->json ? cJSON_GetObjectItem(r->json, "data") : NULL;
    const cJSON *products = cJSON_IsObject(data) ? cJSON_GetObjectItem(data, "products") : NULL;
    if (!products || cJSON_GetArraySize(products) == 0) return; // loading, no product, or an error: nothing to draw
    kio_store_display_html(sb, data);
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
