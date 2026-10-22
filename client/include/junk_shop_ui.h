// The Junk-priced products of the Box Office Eterno Shop (services/brain/shop.c, db_shop.c; doc/v10/junk.md
// section 6). The products are the brain's "limited" display, drawn with kio_store's shop markup (the
// krunker.io display's own cards); a BUY posts POST /api/shop/purchase and sets the Junk balance from the
// reply, as the "uj" push would. Nothing is drawn while the display loads or when it has no product.
#ifndef KN_JUNK_SHOP_UI_H
#define KN_JUNK_SHOP_UI_H

#include <ui_logic.h> // StrBuf

// appends the products under the Eterno Shop's wild offer (store_ui.c v10_store_eterno)
void junk_shop_render(StrBuf *sb);
// knKioShopBuy on Box Office (kio_store.c): the product's purchase, one unit
void junk_shop_buy(int product_id);
// the store home's bundle grid (store_ui.c v10_store_home): the brain's GET /api/shop/bundles rows as v10's bundle
// cards (svelte-lri212, deob-10.0.3.js 144642); nothing is drawn while they load or when none are published
void junk_shop_bundles_render(StrBuf *sb);
// knKioShopBundleBuy(id) on Box Office: the bundle's purchase (POST /api/shop/bundles/purchase); v10's loginOrRegister for a guest
void junk_shop_bundle_buy(int bundle_id);

#endif
