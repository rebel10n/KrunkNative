// The Store's krunker.io side (kio_store.h): krunker.io's own catalogue while krunker.io is the
// active account backend, loaded and drawn the way Krunker v10.0.2 does it.
//
// Requests: v10's API client _10449 (= new _10077(GAPI_URI), v10:27600) through
// krunker_account_api(KRUNKER_SVC_API): base + "/" + path, the account's fresh Bearer token (none for a
// guest: the displays are public). Answers per _10067 (v10:26094): status < 200 or >= 500 -> "Server
// error", 429 -> "Rate limit exceeded. Try again later.", 204 / 304 -> no data, else {data, error}.
//   shop/displays?tag=limited   shop.getDisplayByTag (v10:26524) via _38951 (v10:113896): the Eterno
//                               Shop tab (v10:128293-128340) mounts it; loaded once per account (the
//                               shop store keeps it; v10 refreshes it only after a purchase or a logout,
//                               _38964), an error shows "Error: <message>" and Retry
//   ranked/shop                 ranked.getShop (v10:26380) via _37760 (v10:111020): {skins: [...]},
//                               fetched when the Ranked Shop tab shows (60 s cache, like the brain's)
// Markup: the shop front _43892 (svelte-1nzacef: front grid, sections, the player-bound login card),
// the product card _39555 (svelte-14104bb: skins on the market card, currency / badge cards, the
// purchase limit, Global, countdown, SOLD OUT / EXPIRED, BUY and gift buttons), the dynamic
// (quantity) card _39766 (svelte-dif8c), the buy button _9956 (svelte-1ov1s5h), the item showcase
// _35204 (svelte-2tp35a) with the purchase section _39504 (svelte-rew7ag) or _39517 (svelte-1c1cf2k).
// Not drawn: the "wild-offer" display node (the account's Wild Offer, v10's svelte-m5s1iy card), whose
// data only comes with a signed-in krunker.io account's display.
//
// Read-only (kio_store.h): BUY, gift, the dynamic BUY and every other krunker.io purchase never send
// shop/purchase (v10 _38968 / the gift's purchaseProduct) or showPurchase's payment; they give v10's
// guest behaviour (loginOrRegister) or the read-only notice.
#include <kio_store.h>
#include <krunker_account.h>
#include <backend_ui.h>
#include <v10_ui.h>
#include <v10_menu.h>
#include <ui_logic.h>
#include <html_ui.h>
#include <ks_game.h> // ks_wall_ms: Date.now()
#include <badges.h>
#include "../viewer/viewer_store.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SLOTS 6
#define RANKED_TTL_MS 60000.0
#define ERROR_RETRY_MS 5000.0 // an error is fetched again when the tab renders this much later (v10: on its next mount)
#define DISPLAY_LIMITED "shop/displays?tag=limited"
#define RANKED_SHOP "ranked/shop"
#define CARD 300 // _39555 size (and the front grid's column)
#define QTY_MAX 16

typedef struct {
    char path[96];
    unsigned id;     // the request in flight (0 none)
    int used, inflight;
    double at_ms;    // the last answer (0 none)
    char error[160]; // the answer's error ("" none)
    V10Req req;      // status 0 pending / HTTP status / -1 unreachable; json = the answer's data (owned)
} Slot;

static Slot g_slot[SLOTS];
static unsigned g_ids, g_gen;
static struct { int display, qty; } g_qty[QTY_MAX]; // the dynamic cards' quantity sliders

int kio_store_active(void) { return account_backend() == ACCOUNT_BACKEND_KRUNKER; }

static double now_ms(void) { return ks_wall_ms(); }

static void slot_clear(Slot *s) {
    cJSON_Delete(s->req.json);
    memset(s, 0, sizeof(*s));
}

// --render-scenario exists in the native engine builds only (ui_logic.h UI_SCENARIO_SUPPORTED)
static int scenario_active(void) {
#ifdef UI_SCENARIO_SUPPORTED
    return ui_scenario_active();
#else
    return 0;
#endif
}

// a sign-in / out or a backend switch: the shop store is the old account's (v10 logout: _38964)
static void sync_gen(void) {
    if (scenario_active()) return; // --render-scenario: state.kioStore's answers stay
    const unsigned g = account_active_generation();
    if (g == g_gen) return;
    g_gen = g;
    for (int i = 0; i < SLOTS; i++) slot_clear(&g_slot[i]);
}

static Slot *slot_find(const char *path) {
    for (int i = 0; i < SLOTS; i++)
        if (g_slot[i].used && !strcmp(g_slot[i].path, path)) return &g_slot[i];
    return NULL;
}

// ISO 8601 ("2026-10-20T12:00:00Z", "2026-10-20 12:00:00") or epoch ms as epoch ms (new Date(x)); 0 unknown
static double date_ms(const cJSON *v) {
    if (cJSON_IsNumber(v)) return v->valuedouble;
    if (!cJSON_IsString(v)) return 0;
    int y, mo, d, h = 0, mi = 0;
    double s = 0;
    if (sscanf(v->valuestring, "%d-%d-%d%*c%d:%d:%lf", &y, &mo, &d, &h, &mi, &s) < 3) return 0;
    y -= mo <= 2; // days from the civil date (UTC; no timegm on every platform)
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned) (y - era * 400);
    const unsigned doy = (153u * (unsigned) (mo + (mo > 2 ? -3 : 9)) + 2u) / 5u + (unsigned) d - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    const double days = (double) era * 146097.0 + (double) doe - 719468.0;
    return ((days * 24.0 + h) * 60.0 + mi) * 60000.0 + s * 1000.0;
}

// ranked/shop: each row's expires_at as epoch ms (ranked_ui.c reads a number; v10 new Date(expiresAt))
static void ranked_rows_ms(cJSON *data) {
    cJSON *row;
    cJSON_ArrayForEach(row, cJSON_GetObjectItem(data, "skins")) {
        cJSON *e = cJSON_GetObjectItem(row, "expires_at");
        if (cJSON_IsString(e)) cJSON_ReplaceItemInObject(row, "expires_at", cJSON_CreateNumber(date_ms(e)));
    }
}

static void on_answer(void *user, const int status, const cJSON *res) {
    const unsigned id = (unsigned) (uintptr_t) user;
    Slot *s = NULL;
    for (int i = 0; i < SLOTS && !s; i++)
        if (g_slot[i].used && g_slot[i].id == id) s = &g_slot[i];
    if (!s) return; // the account changed meanwhile
    s->inflight = 0;
    s->id = 0;
    s->at_ms = now_ms();
    s->error[0] = 0;
    cJSON_Delete(s->req.json);
    s->req.json = NULL;
    s->req.status = status > 0 ? status : -1;
    if (status <= 0) snprintf(s->error, sizeof(s->error), "Failed to fetch"); // fetch's TypeError
    else if (status < 200 || status >= 500) snprintf(s->error, sizeof(s->error), "Server error");
    else if (status == 429) snprintf(s->error, sizeof(s->error), "Rate limit exceeded. Try again later.");
    else if (status != 204 && status != 304) {
        const cJSON *err = cJSON_GetObjectItem(res, "error");
        const cJSON *data = cJSON_GetObjectItem(res, "data");
        if (!cJSON_IsObject(res)) snprintf(s->error, sizeof(s->error), "Invalid JSON response from server");
        else if (cJSON_IsString(err) && err->valuestring[0]) snprintf(s->error, sizeof(s->error), "%s", err->valuestring);
        else if (data && !cJSON_IsNull(data)) s->req.json = cJSON_Duplicate(data, 1);
    }
    ranked_rows_ms(s->req.json);
    printf("[kio-store] %s: %d%s%s\n", s->path, status, s->error[0] ? " " : "", s->error);
    menu_rerender_if_open(14); // the Store
    v10_refresh();             // the Ranked popup, an open item view
}

// GET path on krunker.io's API: the cached answer, fetched when there is none, it is older than
// ttl_ms (0 = kept), or it was an error ERROR_RETRY_MS ago
static Slot *kio_get(const char *path, const double ttl_ms) {
    sync_gen();
    Slot *s = slot_find(path);
    const double now = now_ms();
    if (s && (s->inflight || (s->at_ms && !s->error[0] && (ttl_ms <= 0 || now - s->at_ms < ttl_ms)) ||
              (s->error[0] && now - s->at_ms < ERROR_RETRY_MS)))
        return s;
    if (scenario_active()) return s; // --render-scenario: never krunker.io (state.kioStore or nothing)
    if (!s) {
        for (int i = 0; i < SLOTS && !s; i++)
            if (!g_slot[i].used) s = &g_slot[i];
        if (!s) { // the oldest answer goes
            for (int i = 0; i < SLOTS; i++)
                if (!g_slot[i].inflight && (!s || g_slot[i].at_ms < s->at_ms)) s = &g_slot[i];
            if (!s) return NULL;
            slot_clear(s);
        }
        s->used = 1;
        snprintf(s->path, sizeof(s->path), "%s", path);
    }
    s->inflight = 1;
    s->id = ++g_ids;
    if (!krunker_account_api("GET", KRUNKER_SVC_API, path, NULL, on_answer, (void *) (uintptr_t) s->id)) {
        s->inflight = 0;
        s->id = 0;
        s->at_ms = now;
        s->req.status = -1;
        snprintf(s->error, sizeof(s->error), "Failed to fetch");
    }
    return s;
}

const V10Req *kio_store_ranked_shop(void) {
    static const V10Req none = {-1, NULL};
    const Slot *s = kio_get(RANKED_SHOP, RANKED_TTL_MS);
    return s ? &s->req : &none; // _37760: any failure is an empty shop
}

// ------------------------------------------------------------------ read-only

const char *kio_store_read_only_text(void) {
    return I18N("kn.kio.store.readOnly",
                "Box Office shows your krunker.io account's store but never buys, spins, redeems or trades for it. Do that on krunker.io itself.");
}

void kio_store_read_only(const char *what) {
    printf("[kio-store] read-only: %s not sent to krunker.io\n", what ? what : "action");
    if (!active_account()) { // v10: a guest's purchase button is loginOrRegister()
        v10_menu_handle("loginOrRegister", NULL, NULL, 0);
        return;
    }
    v10_view_push(v10_io_error(I18N("kn.kio.store.readOnlyTitle", "krunker.io Store"), kio_store_read_only_text()), NULL, NULL, NULL);
}

// ------------------------------------------------------------------ the display / product adapter

static const char *k_currency[4] = {NULL, "KR", "JNK", "RP"}; // _6908: currency id -> name

static const char *currency_name(const char *ref) {
    const int id = ref ? atoi(ref) : 0;
    return id >= 1 && id <= 3 ? k_currency[id] : "KR";
}

static int truthy(const cJSON *o, const char *k) {
    const cJSON *v = cJSON_GetObjectItem(o, k);
    return cJSON_IsTrue(v) || (cJSON_IsNumber(v) && v->valuedouble != 0) || (cJSON_IsString(v) && v->valuestring[0]);
}

// One shop display node with its product, as _39555 reads them.
typedef struct {
    int display_id, product_id;
    const char *display_type;
    const char *name, *desc;
    int single;           // one content (_39575: only those draw)
    const char *ctype;    // contents[0].type: skin / currency / badge
    const char *cref;     // contents[0].ref
    int ref, quantity;
    const cJSON *price;   // prices[0]
    const char *ptype;    // its type: real / virtual / free / ads
    int limit, count, global;
    double expires;       // display.expires_at, epoch ms (0 none)
    int sold, expired, giftable, gift_disabled, for_you;
    int span, span_col, span_row, gap; // data.span (gap default 10)
} ShopItem;

static ShopItem shop_item(const cJSON *display, const cJSON *product) {
    ShopItem it = {0};
    it.display_id = (int) v10_num(display, "id", 0);
    it.product_id = (int) v10_num(display, "product_id", 0);
    it.display_type = v10_str(display, "type");
    it.name = v10_str(product, "name");
    it.desc = v10_str(product, "description");
    const cJSON *contents = cJSON_GetObjectItem(product, "contents"), *c0 = cJSON_GetArrayItem(contents, 0);
    it.single = cJSON_GetArraySize(contents) == 1;
    it.ctype = v10_str(c0, "type");
    const cJSON *ref = cJSON_GetObjectItem(c0, "ref");
    it.cref = cJSON_IsString(ref) ? ref->valuestring : "";
    it.ref = cJSON_IsNumber(ref) ? ref->valueint : atoi(it.cref);
    it.quantity = (int) v10_num(c0, "quantity", 1);
    if (it.quantity <= 0) it.quantity = 1;
    it.price = cJSON_GetArrayItem(cJSON_GetObjectItem(product, "prices"), 0);
    it.ptype = v10_str(it.price, "type");
    // _39576: the tighter of the display's and the product's purchase limits
    const int dl = (int) v10_num(display, "purchase_limit", 0), dc = (int) v10_num(display, "purchase_count", 0);
    const int pl = (int) v10_num(product, "purchase_limit", 0), pc = (int) v10_num(product, "purchase_count", 0);
    const int which = dl && pl ? (dl - dc <= pl - pc ? 1 : 2) : dl ? 1 : pl ? 2 : 0; // 1 display, 2 product
    it.limit = which == 1 ? dl : which == 2 ? pl : 0;
    it.count = which == 1 ? dc : which == 2 ? pc : 0;
    const int per_user = which == 2 && !strcmp(v10_str(product, "purchase_limit_scope"), "per_user");
    it.global = which == 2 && !strcmp(v10_str(product, "purchase_limit_scope"), "global");
    it.expires = date_ms(cJSON_GetObjectItem(display, "expires_at"));
    it.expired = it.expires > 0 && now_ms() >= it.expires; // the countdown's onExpired
    it.sold = (it.limit > 0 && it.count >= it.limit) || truthy(product, "sold_out");
    const cJSON *data = cJSON_GetObjectItem(display, "data");
    it.for_you = truthy(data, "forYou");
    // _39588: gifts of skins for KR, or real-money non-subscriptions (not the Steam build)
    int all_skins = cJSON_GetArraySize(contents) > 0;
    const cJSON *c;
    cJSON_ArrayForEach(c, contents) all_skins &= !strcmp(v10_str(c, "type"), "skin");
    it.giftable = !strcmp(it.display_type, "product") && !it.for_you &&
                  ((!strcmp(it.ptype, "virtual") && all_skins) || (!strcmp(it.ptype, "real") && !cJSON_GetObjectItem(it.price, "recurring")));
    it.gift_disabled = it.expired || (it.sold && !truthy(product, "giftable") && !per_user); // _39590
    const cJSON *span = cJSON_GetObjectItem(data, "span");
    if (cJSON_IsObject(span)) {
        it.span = 1;
        it.span_col = (int) v10_num(span, "column", 1);
        it.span_row = (int) v10_num(span, "row", 1);
        it.gap = (int) v10_num(span, "gap", 10);
        if (!it.gap) it.gap = 10;
    }
    return it;
}

static int span_px(const int n, const int gap) { return (n > 0 ? n : 1) * CARD + gap * ((n > 0 ? n : 1) - 1); }

// ------------------------------------------------------------------ markup

// _9914 CurrencyDisplay with an optional color (the buy buttons pass white)
static void currency(StrBuf *sb, const double value, const char *cur, const char *size, const int centered, const char *color) {
    if (!color) {
        v10_currency(sb, value, cur, size, centered);
        return;
    }
    const int jnk = cur && !strcmp(cur, "JNK");
    const char *text = "16px", *isz = "18px";
    if (size && !strcmp(size, "small")) text = "14px", isz = "16px";
    else if (size && !strcmp(size, "large")) text = "20px", isz = "22px";
    char amount[48];
    if (jnk) snprintf(amount, sizeof(amount), "%.2f", value / 100.0);
    else v10_fmt_int(amount, sizeof(amount), value);
    sb_appendf(sb, "<div class=\"currency-display svelte-1r6bdx8%s\" style=\"--text-size: %s; --icon-size: %s;\"><span class=\"currency-amount svelte-1r6bdx8\" style=\"color: %s;\">%s</span> ",
               centered ? " centered" : "", text, isz, color, amount);
    if (cur && !strcmp(cur, "KR")) sb_appendf(sb, "<span class=\"currency-text svelte-1r6bdx8\" style=\"color: %s;\">KR</span>", color);
    else if (cur && (jnk || !strcmp(cur, "RP")))
        sb_appendf(sb, "<span class=\"material-icons currency-icon svelte-1r6bdx8\" style=\"color: %s;\">%s</span>", color, jnk ? "plumbing" : "emoji_events");
    else sb_appendf(sb, "<span class=\"currency-text svelte-1r6bdx8\" style=\"color: %s;\">\xf0\x9f\x92\x8e</span>", color);
    sb_append(sb, "</div>");
}

// _9956 the buy button (svelte-1ov1s5h)
static void buy_button(StrBuf *sb, const cJSON *price, const char *size, const int disabled, const char *onclick) {
    const char *pad = "8px 12px", *font = "14px";
    if (!strcmp(size, "small")) pad = "6px 10px", font = "12px";
    else if (!strcmp(size, "large")) pad = "12px 24px", font = "16px";
    sb_appendf(sb, "<button class=\"buy-button svelte-1ov1s5h %s\" type=\"button\" tabindex=\"0\" style=\"padding: %s; font-size: %s;\" onclick=\"%s\"%s>", size, pad,
               font, onclick, disabled ? " disabled" : "");
    const char *t = v10_str(price, "type");
    const cJSON *virt = cJSON_GetObjectItem(price, "virtual");
    if (!strcmp(t, "real")) sb_appendf(sb, "BUY $%.2f", v10_num(cJSON_GetObjectItem(price, "real"), "price_usd", 0));
    else if (!strcmp(t, "virtual")) {
        sb_append(sb, "BUY ");
        currency(sb, v10_num(virt, "amount", 0), v10_str(virt, "currency")[0] ? v10_str(virt, "currency") : "KR", "small", 0, "white");
    } else if (!strcmp(t, "free")) sb_append(sb, "<span>Free</span>");
    else if (!strcmp(t, "ads")) sb_append(sb, "<span>Watch Ad</span>");
    sb_append(sb, "</button>");
}

// _37744 the countdown (svelte-68lwmp)
static void countdown(StrBuf *sb, const double expires) {
    const double left = expires - now_ms();
    char text[48];
    if (left <= 0) snprintf(text, sizeof(text), "Expired");
    else {
        const long long ms = (long long) left, d = ms / 86400000, h = ms % 86400000 / 3600000, m = ms % 3600000 / 60000, s = ms % 60000 / 1000;
        if (d > 0) snprintf(text, sizeof(text), "%lldd %lldh %lldm", d, h, m);
        else if (h > 0) snprintf(text, sizeof(text), "%lldh %lldm %llds", h, m, s);
        else if (m > 0) snprintf(text, sizeof(text), "%lldm %llds", m, s);
        else snprintf(text, sizeof(text), "%llds", s);
    }
    sb_appendf(sb, "<div class=\"countdown-container svelte-68lwmp%s\"><span class=\"material-icons svelte-68lwmp\">schedule</span> <span class=\"time-text svelte-68lwmp\">%s</span></div>",
               left <= 0 ? " expired" : "", text);
}

static void sideways(StrBuf *sb, const ShopItem *it, const char *hash) {
    if (it->sold || it->expired)
        sb_appendf(sb, "<div class=\"sideways-overlay svelte-%s\"><div class=\"sideways-text svelte-%s\">%s</div></div>", hash, hash, it->sold ? "SOLD OUT" : "EXPIRED");
    else sb_append(sb, "<!---->");
}

// the card's for-you row and its limit / countdown row
static void card_rows(StrBuf *sb, const ShopItem *it) {
    if (it->for_you)
        sb_append(sb, "<div class=\"row for-you-row svelte-14104bb\"><div class=\"svelte-14104bb\"></div> <div class=\"for-you-tag svelte-14104bb\"><div class=\"for-you-text svelte-14104bb\">FOR YOU</div></div></div>");
    else sb_append(sb, "<!---->");
    sb_append(sb, " ");
    if (!it->expires && it->limit <= 0) {
        sb_append(sb, "<!---->");
        return;
    }
    StrBuf count = {0};
    if (it->limit > 0) sb_appendf(&count, "<div class=\"purchase-count svelte-14104bb\">%d/%d</div>", it->count, it->limit);
    else sb_append(&count, "<div class=\"svelte-14104bb\"></div>");
    char *cnt = sb_finish(&count);
    sb_append(sb, "<div class=\"row svelte-14104bb\">");
    if (it->global) sb_appendf(sb, "<div class=\"global-limit-container svelte-14104bb\"><div class=\"global-limit-text svelte-14104bb\">Global</div> %s</div>", cnt);
    else sb_append(sb, cnt);
    free(cnt);
    sb_append(sb, " ");
    if (it->expires) countdown(sb, it->expires);
    else sb_append(sb, "<!---->");
    sb_append(sb, "</div>");
}

static void gift_button(StrBuf *sb, const ShopItem *it, const char *hash) {
    sb_appendf(sb, "<button class=\"gift-button svelte-%s\" type=\"button\" title=\"%s\" onclick=\"knKioShopGift(%d,%d)\"%s><span class=\"material-icons%s%s\">card_giftcard</span></button>",
               hash, I18N("windows.store.gift", "Gift"), it->display_id, it->product_id, it->gift_disabled ? " disabled" : "", strcmp(hash, "rew7ag") ? " svelte-" : "",
               strcmp(hash, "rew7ag") ? hash : "");
}

static void badge_img(StrBuf *sb, const ShopItem *it, const cJSON *product) {
    char tex[96], url[512];
    badge_texture(it->ref, tex, sizeof(tex));
    if (tex[0]) html_dom_asset_url(tex, url, sizeof(url));
    else snprintf(url, sizeof(url), "%s", v10_str(product, "image_url")); // a badge past the client's table: the product's image
    sb_append(sb, "<img class=\"badge-img svelte-14104bb\" src=\"");
    sb_append_esc(sb, url);
    sb_append(sb, "\" alt=\"");
    sb_append_esc(sb, it->name);
    sb_append(sb, "\">");
}

// _39555 (layout "card", size 300)
static void product_card(StrBuf *sb, const cJSON *display, const cJSON *product) {
    const ShopItem it = shop_item(display, product);
    const int w = it.span ? span_px(it.span_col, it.gap) : CARD, h = it.span ? span_px(it.span_row, it.gap) : CARD;
    char buy[64], click[64];
    snprintf(buy, sizeof(buy), "knKioShopBuy(%d,%d)", it.display_id, it.product_id);
    snprintf(click, sizeof(click), "knKioShopItem(%d,%d)", it.display_id, it.product_id);
    if (it.span) sb_appendf(sb, "<div class=\"shop-item-wrapper svelte-14104bb\" style=\"grid-column: span %d; grid-row: span %d;\">", it.span_col, it.span_row);
    else sb_append(sb, "<div class=\"shop-item-wrapper svelte-14104bb\">");
    if (it.single && !strcmp(it.ctype, "skin") && viewer_skin(it.ref)) {
        StrBuf c = {0};
        sideways(&c, &it, "14104bb");
        sb_append(&c, " <div class=\"bottom-overlay svelte-14104bb\">");
        card_rows(&c, &it);
        sb_append(&c, " <div class=\"buy-actions svelte-14104bb\"><div class=\"buy-main svelte-14104bb\">");
        buy_button(&c, it.price, "small", it.sold || it.expired, buy);
        sb_append(&c, "</div> ");
        if (it.giftable) gift_button(&c, &it, "14104bb");
        else sb_append(&c, "<!---->");
        sb_append(&c, "</div></div>");
        char *children = sb_finish(&c);
        v10_market_card(sb, it.ref, w, h, "top-center", click, children);
        free(children);
    } else if (it.single && (!strcmp(it.ctype, "currency") || !strcmp(it.ctype, "badge"))) {
        sb_appendf(sb, "<div class=\"currency-shop-item svelte-14104bb\" role=\"button\" tabindex=\"0\" style=\"width: %dpx; height: %dpx;\" onclick=\"%s\">", w, h, click);
        sideways(sb, &it, "14104bb");
        sb_append(sb, " <div class=\"currency-content svelte-14104bb\"><div class=\"product-name svelte-14104bb\">");
        sb_append_esc(sb, it.name);
        sb_append(sb, "</div> ");
        if (!strcmp(it.ctype, "badge")) badge_img(sb, &it, product);
        else currency(sb, it.quantity, currency_name(it.cref), "large", 1, NULL);
        sb_append(sb, "</div> <div class=\"bottom-overlay svelte-14104bb\">");
        card_rows(sb, &it);
        sb_append(sb, " ");
        buy_button(sb, it.price, "small", it.sold || it.expired, buy);
        sb_append(sb, "</div></div>");
    }
    sb_append(sb, "</div>");
}

static int qty_of(const int display, const int min) {
    for (int i = 0; i < QTY_MAX; i++)
        if (g_qty[i].display == display && g_qty[i].qty) return g_qty[i].qty;
    return min;
}

static void qty_set(const int display, const int qty) {
    int slot = -1;
    for (int i = 0; i < QTY_MAX && slot < 0; i++)
        if (g_qty[i].display == display || !g_qty[i].display) slot = i;
    if (slot < 0) slot = 0;
    g_qty[slot].display = display;
    g_qty[slot].qty = qty;
}

static const cJSON *price_of_type(const cJSON *product, const char *type) {
    const cJSON *p;
    cJSON_ArrayForEach(p, cJSON_GetObjectItem(product, "prices")) {
        if (!strcmp(v10_str(p, "type"), type) && cJSON_IsTrue(cJSON_GetObjectItem(p, "available")) && cJSON_GetObjectItem(p, type)) return p;
    }
    return NULL;
}

// _39766 (layout "card"): a quantity slider over data.dynamic {min_quantity, max_quantity, steps}
static void dynamic_card(StrBuf *sb, const cJSON *display, const cJSON *product) {
    const ShopItem it = shop_item(display, product);
    const cJSON *dyn = cJSON_GetObjectItem(cJSON_GetObjectItem(display, "data"), "dynamic");
    const int min = dyn ? (int) v10_num(dyn, "min_quantity", 1) : 1, max = dyn ? (int) v10_num(dyn, "max_quantity", 1) : 1;
    int qty = qty_of(it.display_id, min);
    if (qty < min) qty = min;
    if (qty > max) qty = max;
    const cJSON *real = price_of_type(product, "real"), *virt = price_of_type(product, "virtual");
    const cJSON *steps = cJSON_GetObjectItem(dyn, "steps"), *st;
    const double base_usd = real ? v10_num(cJSON_GetObjectItem(real, "real"), "price_usd", 0) : 0;
    double usd = base_usd;
    const cJSON *vv = virt ? cJSON_GetObjectItem(virt, "virtual") : NULL;
    const double base_amount = vv ? v10_num(vv, "amount", 0) : 0;
    double amount = base_amount;
    const cJSON *next = NULL;
    cJSON_ArrayForEach(st, steps) {
        const int at = (int) v10_num(st, "min_quantity", 0);
        if (qty >= at && cJSON_IsNumber(cJSON_GetObjectItem(st, "price_usd"))) usd = v10_num(st, "price_usd", 0);
        const cJSON *cid = cJSON_GetObjectItem(st, "price_currency_id");
        if (vv && qty >= at && cJSON_IsNumber(cJSON_GetObjectItem(st, "price_amount")) && (!cid || cJSON_IsNull(cid) || v10_num(st, "price_currency_id", 0) == v10_num(vv, "currency_id", 0)))
            amount = v10_num(st, "price_amount", 0);
        if (!next && qty < at && (cJSON_IsNumber(cJSON_GetObjectItem(st, "price_usd")) || cJSON_IsNumber(cJSON_GetObjectItem(st, "price_amount")))) next = st;
    }
    const int discount = real && base_usd ? (int) lround(100.0 * (1.0 - usd / base_usd)) : vv && base_amount ? (int) lround(100.0 * (1.0 - amount / base_amount)) : 0;
    const int cols = it.span ? it.span_col : 2, rows = it.span ? it.span_row : 1, gap = it.span ? it.gap : 10;
    const int w = CARD * cols + gap * (cols - 1), h = CARD * rows + gap * (rows - 1);
    const char *cur = vv && v10_str(vv, "currency")[0] ? v10_str(vv, "currency") : "KR";
    sb_appendf(sb, "<div class=\"dynamic-shop-item svelte-dif8c\" style=\"grid-column: span %d; grid-row: span %d; width: %dpx; height: %dpx;\">", cols, rows, w, h);
    if (it.expired) sb_append(sb, "<div class=\"sideways-overlay svelte-dif8c\"><div class=\"sideways-text svelte-dif8c\">EXPIRED</div></div>");
    else sb_append(sb, "<!---->");
    sb_append(sb, " <div class=\"item-side svelte-dif8c\">");
    if (!strcmp(it.ctype, "skin") && viewer_skin(it.ref)) v10_market_card(sb, it.ref, 280, h - 20, "top-center", NULL, NULL);
    else if (it.ctype[0]) {
        sb_append(sb, "<div class=\"currency-side svelte-dif8c\">");
        currency(sb, (double) it.quantity * qty, currency_name(it.cref), "large", 1, NULL);
        sb_append(sb, "</div>");
    }
    sb_append(sb, "</div> <div class=\"config-side svelte-dif8c\"><div class=\"header-row svelte-dif8c\"><div class=\"product-name svelte-dif8c\">");
    sb_append_esc(sb, it.name);
    sb_append(sb, "</div> ");
    if (it.expires) countdown(sb, it.expires);
    else sb_append(sb, "<!---->");
    sb_append(sb, "</div> ");
    if (it.desc[0]) {
        sb_append(sb, "<div class=\"product-description svelte-dif8c\">");
        sb_append_esc(sb, it.desc);
        sb_append(sb, "</div>");
    } else sb_append(sb, "<!---->");
    sb_appendf(sb, " <div class=\"quantity-row svelte-dif8c\"><div class=\"quantity-label svelte-dif8c\">Quantity</div> <div class=\"quantity-value svelte-dif8c\">x%d ", qty);
    if (discount > 0) sb_appendf(sb, "<span class=\"discount-badge svelte-dif8c\">-%d%%</span>", discount);
    sb_appendf(sb, "</div></div> <input class=\"quantity-slider svelte-dif8c\" type=\"range\" step=\"1\" min=\"%d\" max=\"%d\" value=\"%d\"%s onchange=\"knKioShopQty(%d, this.value)\"> "
                   "<div class=\"range-labels svelte-dif8c\"><span class=\"svelte-dif8c\">%d</span> <span class=\"svelte-dif8c\">%d</span></div> <div class=\"unit-price-row svelte-dif8c\">",
               min, max, qty, it.expired ? " disabled" : "", it.display_id, min, max);
    if (real) {
        sb_appendf(sb, "<span class=\"unit-price svelte-dif8c\">$%.2f each ", usd);
        if (discount > 0) sb_appendf(sb, "<span class=\"unit-price-was svelte-dif8c\">$%.2f</span>", base_usd);
        sb_append(sb, "</span>");
    } else if (vv) {
        sb_append(sb, "<span class=\"unit-price unit-price-virtual svelte-dif8c\">");
        currency(sb, amount, cur, "small", 0, "white");
        sb_append(sb, " each</span>");
    }
    sb_append(sb, " ");
    if (next && cJSON_IsNumber(cJSON_GetObjectItem(next, "price_usd")))
        sb_appendf(sb, "<span class=\"next-step-hint svelte-dif8c\">$%.2f each at %d+</span>", v10_num(next, "price_usd", 0), (int) v10_num(next, "min_quantity", 0));
    else if (next) sb_appendf(sb, "<span class=\"next-step-hint svelte-dif8c\">%.0f each at %d+</span>", v10_num(next, "price_amount", 0), (int) v10_num(next, "min_quantity", 0));
    sb_append(sb, "</div> <div class=\"buy-row svelte-dif8c\">");
    if (real) sb_appendf(sb, "<button class=\"dyn-buy-button svelte-dif8c\" type=\"button\" onclick=\"knKioShopBuy(%d,%d)\"%s>BUY $%.2f</button>", it.display_id, it.product_id,
                         it.expired ? " disabled" : "", round(100.0 * usd * qty) / 100.0);
    sb_append(sb, " ");
    if (vv) {
        sb_appendf(sb, "<button class=\"dyn-buy-button virtual svelte-dif8c\" type=\"button\" onclick=\"knKioShopBuy(%d,%d)\"%s>BUY ", it.display_id, it.product_id, it.expired ? " disabled" : "");
        currency(sb, amount * qty, cur, "small", 0, "white");
        sb_append(sb, "</button>");
    }
    sb_append(sb, "</div></div></div>");
}

// _43892: the shop front, its sections and items
static void display_html(StrBuf *sb, const cJSON *d, const cJSON *products, const int depth) {
    if (!cJSON_IsObject(d) || depth > 8) return;
    const char *type = v10_str(d, "type");
    const cJSON *children = cJSON_GetObjectItem(d, "children"), *c;
    if (!strcmp(type, "front")) {
        sb_append(sb, "<div class=\"shop-front svelte-1nzacef\"><div class=\"shop-front-grid svelte-1nzacef\">");
        cJSON_ArrayForEach(c, children) {
            const char *t = v10_str(c, "type");
            if (!strcmp(t, "product") || !strcmp(t, "wild-offer") || !strcmp(t, "player-bound") || !strcmp(t, "dynamic")) display_html(sb, c, products, depth + 1);
        }
        sb_append(sb, "</div> ");
        cJSON_ArrayForEach(c, children) {
            if (!strcmp(v10_str(c, "type"), "section")) display_html(sb, c, products, depth + 1);
        }
        sb_append(sb, "</div>");
    } else if (!strcmp(type, "section")) {
        const cJSON *data = cJSON_GetObjectItem(d, "data");
        const char *title = v10_str(data, "title"), *desc = v10_str(data, "description");
        sb_append(sb, "<div class=\"shop-section svelte-1nzacef\">");
        if (title[0] || desc[0]) {
            sb_append(sb, "<div class=\"section-header svelte-1nzacef\">");
            if (title[0]) {
                sb_append(sb, "<h2 class=\"section-title svelte-1nzacef\">");
                sb_append_esc(sb, title);
                sb_append(sb, "</h2>");
            }
            sb_append(sb, " ");
            if (desc[0]) {
                sb_append(sb, "<p class=\"section-description svelte-1nzacef\">");
                sb_append_esc(sb, desc);
                sb_append(sb, "</p>");
            }
            sb_append(sb, "</div>");
        } else sb_append(sb, "<!---->");
        sb_append(sb, " <div class=\"section-content svelte-1nzacef\">");
        cJSON_ArrayForEach(c, children) {
            const char *t = v10_str(c, "type");
            if (!strcmp(t, "product") || !strcmp(t, "wild-offer") || !strcmp(t, "player-bound") || !strcmp(t, "section") || !strcmp(t, "dynamic"))
                display_html(sb, c, products, depth + 1);
        }
        sb_append(sb, "</div></div>");
    } else if (!strcmp(type, "product") || !strcmp(type, "dynamic")) {
        char key[24];
        snprintf(key, sizeof(key), "%d", (int) v10_num(d, "product_id", 0));
        const cJSON *product = cJSON_GetObjectItem(products, key);
        if (!cJSON_IsObject(product)) return;
        if (!strcmp(type, "product")) product_card(sb, d, product);
        else dynamic_card(sb, d, product);
    } else if (!strcmp(type, "player-bound")) { // the login card (it comes to guests)
        const cJSON *span = cJSON_GetObjectItem(cJSON_GetObjectItem(d, "data"), "span");
        sb_append(sb, "<div class=\"player-bound-display svelte-1nzacef\" role=\"button\" tabindex=\"0\" onclick=\"loginOrRegister()\"");
        if (cJSON_IsObject(span)) sb_appendf(sb, " style=\"grid-column: span %d; grid-row: span %d;\"", (int) v10_num(span, "column", 1), (int) v10_num(span, "row", 1));
        sb_append(sb, "><div class=\"player-bound-content svelte-1nzacef\"><div class=\"login-icon svelte-1nzacef\"><span class=\"material-icons svelte-1nzacef\">login</span></div> "
                      "<div class=\"player-bound-text svelte-1nzacef\"><div class=\"title svelte-1nzacef\">Personalized for You</div> <div class=\"subtitle svelte-1nzacef\">Please login to get personalized items</div></div> "
                      "<button class=\"login-button svelte-1nzacef\" type=\"button\"><span class=\"material-icons svelte-1nzacef\">person</span> <span class=\"svelte-1nzacef\">Login</span></button></div></div>");
    }
    // "wild-offer": not drawn (the head comment)
}

void kio_store_eterno(StrBuf *sb) {
    char vid[256];
    html_dom_asset_url("videos/limited_shop_banner.webm", vid, sizeof(vid));
    sb_appendf(sb, "<div id=\"shop-header\" class=\"svelte-gh77t9\"><video autoplay=\"\" loop=\"\" playsinline=\"\" class=\"shop-background-video svelte-gh77t9\"><source src=\"%s\" type=\"video/webm\" class=\"svelte-gh77t9\"></video> <div id=\"shop-header-name\" class=\"svelte-gh77t9\">Eterno Shop</div> <div id=\"shop-header-desc\" class=\"svelte-gh77t9\">Limited-time offers—grab them before they're gone! Check back often so you don't miss out on rare deals.</div></div> ",
               vid);
    const Slot *s = kio_get(DISPLAY_LIMITED, 0);
    if (!s || (s->inflight && !s->req.json)) { // _44014
        sb_append(sb, "<div class=\"loading-container svelte-gh77t9\"><div class=\"loading-spinner svelte-gh77t9\"></div> <div class=\"loading-text svelte-gh77t9\">Loading eterno shop...</div></div>");
    } else if (s->error[0]) { // _44021
        sb_append(sb, "<div class=\"error-container svelte-gh77t9\"><div class=\"error-text svelte-gh77t9\">Error: ");
        sb_append_esc(sb, s->error);
        sb_append(sb, "</div> <button class=\"retry-button svelte-gh77t9\" onclick=\"knKioShopRetry()\">Retry</button></div>");
    } else if (s->req.json) {
        display_html(sb, s->req.json, cJSON_GetObjectItem(s->req.json, "products"), 0);
    }
}

// ------------------------------------------------------------------ the item view (_39616)

static const cJSON *find_display(const cJSON *d, const int id, const int depth) {
    if (!cJSON_IsObject(d) || depth > 8) return NULL;
    if ((int) v10_num(d, "id", -1) == id) return d;
    const cJSON *c;
    cJSON_ArrayForEach(c, cJSON_GetObjectItem(d, "children")) {
        const cJSON *f = find_display(c, id, depth + 1);
        if (f) return f;
    }
    return NULL;
}

static int lookup(const int display_id, const cJSON **display, const cJSON **product) {
    const Slot *s = slot_find(DISPLAY_LIMITED);
    if (!s || !s->req.json) return 0;
    *display = find_display(s->req.json, display_id, 0);
    char key[24];
    snprintf(key, sizeof(key), "%d", (int) v10_num(*display, "product_id", 0));
    *product = cJSON_GetObjectItem(cJSON_GetObjectItem(s->req.json, "products"), key);
    return *display && cJSON_IsObject(*product);
}

static const char *item_type(const ViewerSkin *vs) {
    static const char *const types[] = {NULL, "Hat Item", "Body Item", "Melee Item", "Spray Item", "Dye Item", "Waist Item", "Face Item", "Shoe Item", "Pet Item",
                                        "Collectible Item", "Wrist Item", "Charm Item", "Spin Ticket Item", "Back Item", "Head Item", "Player Card"};
    if (vs && vs->type > 0 && vs->type < (int) (sizeof(types) / sizeof(types[0]))) return types[vs->type];
    if (vs && vs->type == 0 && vs->weapon > 0) return "Weapon Skin";
    return "";
}

// _35204 the showcase with the purchase section (_39504) or SOLD OUT / EXPIRED (_39517)
static void item_view_html(StrBuf *sb, const ShopItem *it) {
    const ViewerSkin *vs = viewer_skin(it->ref);
    const char *color = vs && vs->rarity >= 0 && vs->rarity < g_viewer_rarity_count && g_viewer_rarities[vs->rarity].color ? g_viewer_rarities[vs->rarity].color : "#B0B0B0";
    char img[512];
    store_skin_preview(it->ref, img, sizeof(img));
    sb_appendf(sb, "<div class=\"popup-container svelte-1wz7ymp\"><div class=\"popup svelte-1wz7ymp translucent slim-border\" style=\"width: 600px;\">"
                   "<div class=\"item-showcase-container io-popup-container svelte-2tp35a\"><div class=\"item-showcase-content io-popup-content svelte-2tp35a\">"
                   "<div class=\"image-display svelte-2tp35a\" style=\"border: 3px solid %s; background-image: url(%s);\"></div> "
                   "<div class=\"showcase-overlay svelte-2tp35a\"><!----> <div class=\"showcase-header svelte-2tp35a\"><!----> <div class=\"item-showcase-header svelte-2tp35a\">"
                   "<div class=\"item-info svelte-2tp35a\"><!----> <div class=\"item-name svelte-2tp35a\" style=\"color: %s;\">",
               color, img, color);
    sb_append_esc(sb, vs && vs->name ? vs->name : it->name);
    sb_appendf(sb, "</div> <!----> <div class=\"item-description svelte-2tp35a\">%s</div></div> <div class=\"item-showcase-btns svelte-2tp35a\">"
                   "<button class=\"more-info-btn svelte-2tp35a\" onclick=\"openSales(%d)\">More info</button></div></div></div> ",
               item_type(vs), it->ref);
    if (it->sold || it->expired)
        sb_appendf(sb, "<div class=\"not-available-section svelte-1c1cf2k\"><div class=\"status-text %s svelte-1c1cf2k\">%s</div></div>", it->sold ? "sold-out" : "expired",
                   it->sold ? "SOLD OUT" : "EXPIRED");
    else {
        char buy[64];
        snprintf(buy, sizeof(buy), "knKioShopBuy(%d,%d)", it->display_id, it->product_id);
        sb_append(sb, "<div class=\"purchase-section svelte-rew7ag\"><div class=\"io-button-group\">");
        buy_button(sb, it->price, "large", 0, buy);
        sb_append(sb, " ");
        if (it->giftable) gift_button(sb, it, "rew7ag");
        else sb_append(sb, "<!---->");
        sb_append(sb, "</div></div>");
    }
    sb_append(sb, "</div></div></div></div></div>");
}

static void open_item(const int display_id) {
    const cJSON *display, *product;
    if (!lookup(display_id, &display, &product)) return;
    const ShopItem it = shop_item(display, product);
    if (!it.single) return;
    if (!strcmp(it.ctype, "skin") && viewer_skin(it.ref)) {
        StrBuf sb = {0};
        item_view_html(&sb, &it);
        v10_view_push(sb_finish(&sb), NULL, NULL, NULL);
    } else if ((!strcmp(it.ctype, "currency") || !strcmp(it.ctype, "badge")) && !it.sold && !it.expired) {
        kio_store_read_only("shop/purchase"); // v10: currency / badge cards buy at once (_39604)
    }
}

// ------------------------------------------------------------------ --render-scenario

static void scenario_answer(const char *path, const cJSON *data) {
    Slot *s = slot_find(path);
    for (int i = 0; i < SLOTS && !s; i++)
        if (!g_slot[i].used) s = &g_slot[i];
    if (!s || !cJSON_IsObject(data)) return;
    slot_clear(s);
    s->used = 1;
    snprintf(s->path, sizeof(s->path), "%s", path);
    s->at_ms = 1e300; // never stale
    s->req.status = 200;
    s->req.json = cJSON_Duplicate(data, 1);
    ranked_rows_ms(s->req.json);
}

void kio_store_scenario(const cJSON *state) {
    if (!cJSON_IsObject(state)) return;
    scenario_answer(DISPLAY_LIMITED, cJSON_GetObjectItem(state, "limited"));
    scenario_answer(RANKED_SHOP, cJSON_GetObjectItem(state, "ranked"));
}

// ------------------------------------------------------------------ handlers

static double arg_num(const HtmlEvent *ev, const HtmlArg *args, const int argc, const int i) {
    if (!args || i >= argc) return 0;
    if (args[i].kind == HTML_ARG_NUM || args[i].kind == HTML_ARG_BOOL) return args[i].num;
    if (args[i].kind == HTML_ARG_THIS_VALUE) return ev && ev->value ? atof(ev->value) : 0;
    return args[i].str ? atof(args[i].str) : 0;
}

int kio_store_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    if (!name || strncmp(name, "knKioShop", 9)) return -1;
    const char *h = name + 9;
    if (!strcmp(h, "Buy")) { // _39604 / the dynamic card's _39732 / _39737: shop/purchase or showPurchase: never sent
        html_ui_stop_propagation();
        kio_store_read_only("shop/purchase");
    } else if (!strcmp(h, "Gift")) { // _39563: shop/purchase {gift_to} or giftProduct: never sent
        html_ui_stop_propagation();
        kio_store_read_only("shop/purchase (gift)");
    } else if (!strcmp(h, "Item")) {
        open_item((int) arg_num(ev, args, argc, 0));
    } else if (!strcmp(h, "Qty")) { // the quantity slider (_39728: clamped to the display's range)
        qty_set((int) arg_num(ev, args, argc, 0), (int) floor(arg_num(ev, args, argc, 1)));
        menu_rerender_if_open(14);
    } else if (!strcmp(h, "Retry")) { // _44015
        Slot *s = slot_find(DISPLAY_LIMITED);
        if (s && !s->inflight) slot_clear(s);
        menu_rerender_if_open(14);
    } else {
        return -1;
    }
    return 1;
}
