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

#endif
