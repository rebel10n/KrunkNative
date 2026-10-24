// The live trade of Trade mode, client side (doc/v10/trade.md): Krunker v10.0.3's windows[36] ("Trading",
// showWindow(37), deob-10.0.3.js:251107-251527), its packet handlers (:253238-253323, :259539-259561), the
// chat line "NAME wants to trade [Accept]" (:259629), acceptRequest / requestTrade (:253613, :254074), the
// in-world prompt "Press [key] to Trade with [NAME]" (:192464-192474, :197562-197567) and the window's
// cancel when another window opens (:253813). The server (server/src/trade.c) or the official game
// (client/src/net/krunker_link.c) runs the session; this file draws what it says and sends the clicks.
// v10 edits the DOM in place (insertAdjacentHTML on every utrd); KrunkNative keeps the sides here and
// redraws the lists, totals and values of a side from them (the same markup).
#include <ui_logic.h>
#include <client.h>
#include <html_dom.h>
#include <html_ui.h>
#include <i18n.h>
#include <live_trade.h>
#include <net_ext.h>
#include <player_accounts.h>
#include <player_names.h>
#include "../viewer/viewer_store.h"
#include <market_trade_list.h> // market_text_contains: v10's search
#include <ruleset.h>   // rules_weapon: the search's weapon names
#include <social_v7.h> // CHAT_CHANNEL_ALL
#include <cJSON.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TRADE_WINDOW 37 // showWindow(37) = windows[36]
#define TRADE_SEL_MAX 32
#define TRADE_GREEN "#5ce05a"
#define TRADE_RED "#ed4242"

typedef struct {
    int skin, count;
    int64_t copy;     // the instance (painted copy) id, 0 a stack: v10 `st ? 0 : id`
    cJSON *meta;      // m (nametag, wear, serial, paint, paintShader), NULL none
} TradeUiItem;

typedef struct {
    int skin, count;
    int64_t copy;
} TradeUiSel;

typedef struct {
    char *data;
    size_t len;
    int next; // the part expected next
} Chunks;

static struct {
    int active;              // windows[36].active
    int trader;              // windows[36].trader: the other player's uid
    TradeUiItem *inv[2];     // inventories[0] mine, [1] theirs (sorted as v10 sorts them)
    int ninv[2];
    TradeUiSel sel[2][TRADE_SEL_MAX]; // trades[0] / [1]
    int nsel[2];
    int64_t kr[2];           // currencies[side] KR
    int ready[2];
    int limit[2];            // tradeMaxOut / tradeMaxOutP of each side's owner
    Chunks open, result;
} g_tr;

// ---------------------------------------------------------------- helpers

static Client *ui_client(void) { return menu_ui() ? menu_ui()->client : NULL; }

static void send_ext(const uint16_t subtype, const void *body, const uint16_t length) {
    Client *client = ui_client();
    uint8_t buf[NET_EXT_HEADER_SIZE + NET_EXT_BODY_MAX];
    const uint16_t n = net_ext_pack(buf, sizeof(buf), subtype, body, length);
    if (n && client) client_net_send(client, (NetPacketType) NET_PACKET_EXT, buf, n);
}

static int my_uid(void) {
    const Client *client = ui_client();
    return client && client->me ? client->me->uid : -1;
}

static char g_scenario_trader[PLAYER_NAME_SIZE]; // --render-scenario: the trader's name (no such player in the game)

// fakeName || (premiumT ? alias : name): the name the player list shows (Hide player names applies)
static const char *player_name(const int uid, char *out, const size_t size) {
    Client *client = ui_client();
    const Player *p = client ? game_find_player(&client->game, uid) : NULL;
    if (!p && g_scenario_trader[0] && uid == g_tr.trader) return snprintf(out, size, "%s", g_scenario_trader), out;
    return player_names_shown(uid, 0, p ? p->name : "unknown", out, size);
}

static int is_premium(const int uid) {
    const NetPlayerInfoPacket *info = player_accounts_get(uid);
    return info && (info->flags & NET_PLAYER_INFO_PREMIUM);
}

static const char *rarity_color(const ViewerSkin *s) {
    return s && s->rarity >= 0 && s->rarity < g_viewer_rarity_count ? g_viewer_rarities[s->rarity].color : "#B0B0B0";
}

// free || noSale || blackM || opensea || itemShop == 1 (:251130): KrunkNative reads the item table's tradable bit
static int locked(const ViewerSkin *s) { return s && !(s->flags2 & VS2_TRADABLE); }

static void thumb(char *out, const size_t n, const ViewerSkin *s) {
    char path[256];
    viewer_skin_preview_path(s, path, sizeof(path));
    html_dom_asset_url(path, out, n);
}

static void chat_line(const char *text, const char *color) {
    if (text && *text) hud_ui_add_chat_line_on(text, color, CHAT_CHANNEL_ALL);
}

static void chunks_reset(Chunks *c) {
    free(c->data);
    memset(c, 0, sizeof(*c));
}

// one slice of a chunked record; the whole JSON (malloc'd) once its last part arrived, else NULL
static char *chunks_push(Chunks *c, const uint8_t *body, const uint16_t length, int *uid) {
    if (length < sizeof(NetTradeChunkHeader)) return NULL;
    NetTradeChunkHeader h;
    memcpy(&h, body, sizeof(h));
    if (h.part == 0) chunks_reset(c);
    if (h.part != c->next || !h.parts) {
        chunks_reset(c);
        return NULL;
    }
    const size_t n = length - sizeof(h);
    char *grown = realloc(c->data, c->len + n + 1);
    if (!grown) {
        chunks_reset(c);
        return NULL;
    }
    c->data = grown;
    memcpy(c->data + c->len, body + sizeof(h), n);
    c->len += n;
    c->data[c->len] = 0;
    c->next++;
    if (c->next < h.parts) return NULL;
    if (uid) *uid = h.uid;
    char *out = c->data;
    c->data = NULL;
    chunks_reset(c);
    return out;
}

static void free_inventories(void) {
    for (int s = 0; s < 2; s++) {
        for (int i = 0; i < g_tr.ninv[s]; i++) cJSON_Delete(g_tr.inv[s][i].meta);
        free(g_tr.inv[s]);
        g_tr.inv[s] = NULL;
        g_tr.ninv[s] = 0;
    }
}

static void clear_offers(void) {
    memset(g_tr.sel, 0, sizeof(g_tr.sel));
    g_tr.nsel[0] = g_tr.nsel[1] = 0;
    g_tr.kr[0] = g_tr.kr[1] = 0; // currencies = [[], []]
    g_tr.ready[0] = g_tr.ready[1] = 0;
}

// iíîiiíí (:28128): rarity, season (seas || 1), index, all descending
static int inv_cmp(const void *pa, const void *pb) {
    const TradeUiItem *a = pa, *b = pb;
    const ViewerSkin *sa = viewer_skin(a->skin), *sb = viewer_skin(b->skin);
    const int ra = sa ? sa->rarity : 0, rb = sb ? sb->rarity : 0;
    if (ra != rb) return rb - ra;
    const int ea = sa ? (sa->seas ? sa->seas : 1) : 0, eb = sb ? (sb->seas ? sb->seas : 1) : 0;
    if (ea != eb) return eb - ea;
    return b->skin - a->skin;
}

static void load_inventory(const int side, const cJSON *rows) {
    const int n = cJSON_IsArray(rows) ? cJSON_GetArraySize(rows) : 0;
    g_tr.inv[side] = n ? calloc((size_t) n, sizeof(TradeUiItem)) : NULL;
    int k = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, rows) {
        if (!g_tr.inv[side]) break;
        const cJSON *i = cJSON_GetObjectItemCaseSensitive(it, "i"), *c = cJSON_GetObjectItemCaseSensitive(it, "c");
        if (!cJSON_IsNumber(i)) i = cJSON_GetObjectItemCaseSensitive(it, "ind");
        if (!cJSON_IsNumber(i) || !cJSON_IsNumber(c) || c->valuedouble < 1) continue;
        TradeUiItem *row = &g_tr.inv[side][k++];
        row->skin = (int) i->valuedouble;
        row->count = (int) c->valuedouble;
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(it, "id");
        row->copy = !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(it, "st")) && cJSON_IsNumber(id) ? (int64_t) id->valuedouble : 0;
        const cJSON *m = cJSON_GetObjectItemCaseSensitive(it, "m");
        if (cJSON_IsString(m)) row->meta = cJSON_Parse(m->valuestring); // m may be a JSON string (:251133)
        else if (cJSON_IsObject(m)) row->meta = cJSON_Duplicate(m, 1);
    }
    g_tr.ninv[side] = k;
    if (k > 1) qsort(g_tr.inv[side], (size_t) k, sizeof(TradeUiItem), inv_cmp);
}

static const TradeUiItem *find_inv(const int side, const int skin, const int64_t copy) {
    for (int i = 0; i < g_tr.ninv[side]; i++) {
        const TradeUiItem *it = &g_tr.inv[side][i];
        if (it->skin == skin && (copy ? it->copy == copy : !it->copy)) return it;
    }
    return NULL;
}

static TradeUiSel *find_sel(const int side, const int skin, const int64_t copy) {
    for (int i = 0; i < g_tr.nsel[side]; i++) {
        if (g_tr.sel[side][i].skin == skin && g_tr.sel[side][i].copy == copy) return &g_tr.sel[side][i];
    }
    return NULL;
}

// the item element ids: trd_inv_<side>_<skin>[_<copy>], trd_sel_<side>_<skin>[_<copy>]
static void item_key(char *out, const size_t n, const int skin, const int64_t copy) {
    if (copy) snprintf(out, n, "%d_%lld", skin, (long long) copy);
    else snprintf(out, n, "%d", skin);
}

// the data-* attributes ui_tooltip.c reads (nametag, wear, serial, paint): v10's iïÍìîîì (:251136)
static void meta_attrs(StrBuf *sb, const cJSON *m) {
    if (!cJSON_IsObject(m)) return;
    const char *name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(m, "name"));
    if (name && *name) {
        sb_append(sb, " data-nametag='");
        sb_append_esc(sb, name);
        sb_append(sb, "'");
    }
    const cJSON *wear = cJSON_GetObjectItemCaseSensitive(m, "wear"), *serial = cJSON_GetObjectItemCaseSensitive(m, "serial");
    if (cJSON_IsNumber(wear)) sb_appendf(sb, " data-wear-float='%.4f'", wear->valuedouble);
    if (cJSON_IsNumber(serial)) sb_appendf(sb, " data-serial='%lld'", (long long) serial->valuedouble);
    const char *paint = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(m, "paint"));
    if (paint && *paint) {
        char clean[16];
        size_t k = 0;
        for (const char *p = paint; *p && k + 1 < sizeof(clean); p++) {
            if (*p == '#' || (*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F')) clean[k++] = *p;
        }
        clean[k] = 0;
        sb_appendf(sb, " data-paint='%s'", clean);
    }
    const cJSON *shader = cJSON_GetObjectItemCaseSensitive(m, "paintShader");
    if (cJSON_IsNumber(shader)) sb_appendf(sb, " data-paint-shader='%d'", (int) shader->valuedouble);
}

static void item_image(StrBuf *sb, const ViewerSkin *s) {
    char src[512];
    thumb(src, sizeof(src), s);
    // a spray's sheet (tItemSprayImg; v10 adds its sprayAnm keyframes, KrunkNative shows the first frame)
    if (s->type == 4) sb_appendf(sb, "<div class='tItemSprayImg' style='background: url(&quot;%s&quot;);'></div>", src);
    else sb_appendf(sb, "<img draggable='false' class='tItemImg' loading='lazy' src='%s'>", src);
}

static int offered_units(const int side, const int skin, const int64_t copy) {
    const TradeUiSel *s = find_sel(side, skin, copy);
    return s ? s->count : 0;
}

// an inventory card (:251146, :251184): side 0 adds, side 1 requests
static void inv_card(StrBuf *sb, const int side, const TradeUiItem *it) {
    const ViewerSkin *s = viewer_skin(it->skin);
    if (!s || !it->count) return;
    const int lock = locked(s);
    if (lock && cJSON_IsTrue(ui_setting_value("hideNonTrade"))) return; // settings.interface.hideNonTrade
    char key[48];
    item_key(key, sizeof(key), it->skin, it->copy);
    sb_appendf(sb, "<div id='trd_inv_%d_%s' class='tItemHolder'", side, key);
    if (it->copy) sb_appendf(sb, " onclick='windows[36].%s(%d,%lld)'", side ? "request" : "add", it->skin, (long long) it->copy);
    else sb_appendf(sb, " onclick='windows[36].%s(%d)'", side ? "request" : "add", it->skin);
    const int offered = offered_units(side, it->skin, it->copy);
    sb_appendf(sb, " style='border: 2px solid %s;%s' oncontextmenu='openSales(%d,this)' data-index='%d'", rarity_color(s),
               it->copy && offered ? "opacity: 0.3;" : "", it->skin, it->skin);
    meta_attrs(sb, it->meta);
    sb_append(sb, " onmouseover='createTip(this)' onmouseout='cancelTip()'>");
    item_image(sb, s);
    sb_append(sb, "<div class='tItemN'>");
    sb_append_esc(sb, s->name);
    // side 0 shows what is left once the offer takes its units (addItem / removeItem, :251339, :251373)
    sb_appendf(sb, "</div><div class='tItemCnt'>x%d</div>", side == 0 && !it->copy ? it->count - offered : it->count);
    if (lock) sb_append(sb, "<div class='tItemBound'><span class='material-icons' style='font-size:30px;color:#fff;'>lock</span></div>");
    sb_append(sb, "</div>");
}

// the selected list of a side (#tradeList0 / 1): the currency row first (prepend, :251495), then the units
static char *trade_list_html(const int side) {
    StrBuf sb = {0};
    if (g_tr.kr[side] > 0) {
        sb_appendf(&sb, "<div id='currency_item_%d_1' class='tCurrencyItem'%s><div class='tCurrencyName'>Krunkies</div>"
                        "<div class='tCurrencyAmount'>%lld KR</div></div>",
                   side, side == 0 ? " onclick='windows[36].removeCurrency(1)'" : "", (long long) g_tr.kr[side]);
    }
    for (int i = 0; i < g_tr.nsel[side]; i++) {
        const TradeUiSel *sel = &g_tr.sel[side][i];
        const ViewerSkin *s = viewer_skin(sel->skin);
        if (!s) continue;
        const TradeUiItem *it = find_inv(side, sel->skin, sel->copy);
        char key[48];
        item_key(key, sizeof(key), sel->skin, sel->copy);
        sb_appendf(&sb, "<div id='trd_sel_%d_%s' class='tItemHolder' oncontextmenu='openSales(%d,this)' ", side, key, sel->skin);
        if (side) sb_appendf(&sb, "onclick='openSales(%d,this)' ", sel->skin);
        else if (sel->copy) sb_appendf(&sb, "onclick='windows[36].remove(%d,%lld)' ", sel->skin, (long long) sel->copy);
        else sb_appendf(&sb, "onclick='windows[36].remove(%d)' ", sel->skin);
        sb_appendf(&sb, "style='border: 2px solid %s;' data-index='%d'", rarity_color(s), sel->skin);
        meta_attrs(&sb, it ? it->meta : NULL);
        sb_append(&sb, " onmouseover='createTip(this)' onmouseout='cancelTip()'>");
        item_image(&sb, s);
        sb_append(&sb, "<div class='tItemN'>");
        sb_append_esc(&sb, s->name);
        sb_appendf(&sb, "</div><div class='tItemCnt'>x%d</div></div>", sel->count);
    }
    return sb_finish(&sb);
}

// UTILS.commaNumber
static void comma(char *out, const size_t size, const long long v) {
    char digits[32];
    snprintf(digits, sizeof(digits), "%lld", v < 0 ? -v : v);
    const size_t n = strlen(digits);
    size_t o = 0;
    if (v < 0 && o + 1 < size) out[o++] = '-';
    for (size_t i = 0; i < n && o + 2 < size; i++) {
        if (i && (n - i) % 3 == 0) out[o++] = ',';
        out[o++] = digits[i];
    }
    out[o] = 0;
}

static int side_units(const int side) {
    int n = 0;
    for (int i = 0; i < g_tr.nsel[side]; i++) n += g_tr.sel[side][i].count;
    return n;
}

// Est. Value: sum of Math.round(price * count) (:251343), the market's estimates (social_estimate_value)
static long long side_value(const int side) {
    long long v = 0;
    for (int i = 0; i < g_tr.nsel[side]; i++) v += llround((double) social_estimate_value(g_tr.sel[side][i].skin) * g_tr.sel[side][i].count);
    return v;
}

// a side's list, total "n/5", Est. Value and its inventory cards' counts
static void redraw_side(const int side) {
    char id[32], text[64];
    snprintf(id, sizeof(id), "tradeList%d", side);
    const HtmlElem list = html_dom_by_id(id);
    if (list) {
        char *html = trade_list_html(side);
        html_dom_set_html(list, html);
        free(html);
    }
    snprintf(id, sizeof(id), "tradeTotal%d", side);
    snprintf(text, sizeof(text), "%d/%d", side_units(side), g_tr.limit[side]);
    html_dom_set_text(html_dom_by_id(id), text);
    snprintf(id, sizeof(id), "offerVal%d", side);
    const HtmlElem val = html_dom_by_id(id);
    char num[32];
    comma(num, sizeof(num), side_value(side));
    if (val) {
        char html[160];
        snprintf(html, sizeof(html), "Est. Value <span style='color: var(--red);'>%s</span> KR", num);
        html_dom_set_html(val, html);
    }
    for (int i = 0; side == 0 && i < g_tr.ninv[0]; i++) { // side 0's cards: what is left, a selected copy dimmed
        const TradeUiItem *it = &g_tr.inv[0][i];
        char key[48];
        item_key(key, sizeof(key), it->skin, it->copy);
        snprintf(id, sizeof(id), "trd_inv_0_%s", key);
        const HtmlElem card = html_dom_by_id(id);
        if (!card) continue;
        const int offered = offered_units(0, it->skin, it->copy);
        if (it->copy) {
            html_dom_set_style(card, "opacity", offered ? "0.3" : "1");
        } else {
            snprintf(text, sizeof(text), "x%d", it->count - offered);
            html_dom_set_text(html_dom_query(card, ".tItemCnt"), text);
        }
    }
}

// updateButtons (:251431): both READY cleared, the box enabled, "Accept Offer - 10 KR"
static void update_buttons(void) {
    g_tr.ready[0] = g_tr.ready[1] = 0;
    const HtmlElem box = html_dom_by_id("tSubOffer");
    if (box) {
        html_dom_remove_attr(box, "disabled");
        html_dom_set_checked(box, 0);
    }
    html_dom_set_style(html_dom_by_id("tHeader0"), "background-color", TRADE_RED);
    html_dom_set_style(html_dom_by_id("tHeader1"), "background-color", TRADE_RED);
    html_dom_set_text(html_dom_by_id("tradeReady0"), "");
    html_dom_set_text(html_dom_by_id("tradeReady1"), "");
    char label[48];
    snprintf(label, sizeof(label), "Accept Offer - %d KR", LIVE_TRADE_COST);
    html_dom_set_text(html_dom_by_id("tSubName"), label);
    html_dom_set_value(html_dom_by_id("currencyAmount"), ""); // updateCurrencyButtons
}

static void close_window(void) {
    if (menu_ui() && menu_ui()->active_window == TRADE_WINDOW) menu_show_window(MENU_WINDOW_NONE, 0); // showWindow(0)
}

// ---------------------------------------------------------------- the window (windows[36].gen)

char *trade_ui_window_gen(void) {
    Client *client = ui_client();
    if (!g_tr.active || !client || (!game_find_player(&client->game, g_tr.trader) && !g_scenario_trader[0])) return strdup("Error"); // findBySid fails
    char name[PLAYER_NAME_SIZE * 2];
    player_name(g_tr.trader, name, sizeof(name));
    g_tr.limit[0] = is_premium(my_uid()) ? LIVE_TRADE_MAX_OUT_PREMIUM : LIVE_TRADE_MAX_OUT;
    g_tr.limit[1] = is_premium(g_tr.trader) ? LIVE_TRADE_MAX_OUT_PREMIUM : LIVE_TRADE_MAX_OUT;
    StrBuf sb = {0};
    sb_append(&sb, "<div>");
    for (int side = 0; side < 2; side++) {
        sb_appendf(&sb, "<div class='tHolder%s'><div class='tListN inv'>", side ? "R" : "L");
        if (side) {
            sb_append_esc(&sb, name);
            sb_append(&sb, "'s Inventory");
        } else {
            sb_append(&sb, "Your Inventory");
        }
        sb_appendf(&sb, "</div><div id='invList%d' class='tList' style='height: 225px;'>", side);
        for (int i = 0; i < g_tr.ninv[side]; i++) inv_card(&sb, side, &g_tr.inv[side][i]);
        sb_append(&sb, "</div>");
        sb_appendf(&sb, "<input class='tSearch' type='text' placeholder='Search Item' onkeyup='windows[36].searchItems(this, %d)'>", side);
        if (side) {
            sb_append(&sb, "<div class='tCurrencyControls'><span>KR</span><input id='currencyAmountRequest' type='number' placeholder='Amount'"
                           " min='1' max='999999999' class='tCurrencyInput' onkeydown='if(event.keyCode==13)windows[36].requestCurrency()'>"
                           "<button onclick='windows[36].requestCurrency()' onmouseenter='playTick()' class='tCurrencyBtn'>Request</button></div>");
        } else {
            sb_append(&sb, "<div class='tCurrencyControls'><span>KR</span><input id='currencyAmount' type='number' placeholder='Amount'"
                           " min='1' class='tCurrencyInput' onkeydown='if(event.keyCode==13)windows[36].addCurrency()'>"
                           "<button onclick='windows[36].addCurrency()' onmouseenter='playTick()' class='tCurrencyBtn'>Add</button></div>");
        }
        sb_appendf(&sb, "<div id='tHeader%d' class='tListN trade'%s>", side, g_tr.ready[side] ? " style='background-color: " TRADE_GREEN ";'" : "");
        if (side) sb_append_esc(&sb, name);
        else sb_append(&sb, "You");
        // the lists, totals, values and READY marks of the state (all empty when the window opens, as v10's gen)
        char *list = trade_list_html(side), value[32];
        comma(value, sizeof(value), side_value(side));
        sb_appendf(&sb, " <span id='tradeTotal%d' class='tTotal'>%d/%d</span><span id='tradeReady%d' class='tReady'>%s</span></div>"
                        "<div id='tradeList%d' class='tList' style='height:125px;margin-bottom: 5px;'>%s</div>"
                        "<div class='estOfferVal' id='offerVal%d'>Est. Value <span style='color: var(--red);'>%s</span> KR</div>",
                   side, side_units(side), g_tr.limit[side], side, g_tr.ready[side] ? "READY" : "", side, list ? list : "", side, value);
        free(list);
        if (side) {
            sb_append(&sb, "<label class='tSubHolder' onmouseenter='playTick()'><span id='tDecName'>Decline Offer</span>"
                           "<input id='tDecOffer' type='checkbox' onclick='windows[36].decline()'><div class='tDecCheck'></div></label></div>");
        } else {
            sb_appendf(&sb, "<label class='tSubHolder' onmouseenter='playTick()'><span id='tSubName'>Accept Offer - %d KR</span>"
                            "<input id='tSubOffer' type='checkbox' onclick='windows[36].accept()'%s><div class='tSubCheck'></div></label></div>",
                       LIVE_TRADE_COST, g_tr.ready[0] ? " checked disabled" : "");
        }
    }
    // the chat: Enter sends it ("tct", :260031-260036); v10's page-wide key handler is the input's own here
    sb_append(&sb, "<div id='tradeChatHolder'><div id='tradeChatInner'><div id='tradeChatList'></div>"
                   "<input id='tradeChatInput' placeholder='Enter Message' maxlength='70' autocomplete='off'"
                   " onkeydown='if(event.keyCode==13)windows[36].sendChat(this.value)'></input></div></div>");
    sb_append(&sb, "</div>");
    return sb_finish(&sb);
}

// ---------------------------------------------------------------- packets

static void on_open(const int trader, const char *json) {
    cJSON *invs = cJSON_Parse(json);
    free_inventories();
    clear_offers();
    load_inventory(0, cJSON_GetArrayItem(invs, 0)); // windows[36].inv = invs[0]
    load_inventory(1, cJSON_GetArrayItem(invs, 1)); // .otherInv = invs[1]
    cJSON_Delete(invs);
    g_tr.trader = trader;
    g_tr.active = 1;
    g_scenario_trader[0] = 0;
    Client *client = ui_client();
    if (client) client_set_menu_open(client, 1); // document.exitPointerLock()
    menu_show_window(TRADE_WINDOW, 1);           // showWindow(37)
    redraw_side(0);
    redraw_side(1);
}

// trd [msg, sid, type] (iîîíiîî, :253238)
static void on_response(const NetTradeResponsePacket *r) {
    char msg[NET_TRADE_MSG_MAX];
    snprintf(msg, sizeof(msg), "%s", r->msg);
    switch (r->type) {
        case LIVE_TRADE_RES_REQUEST: { // iííîiíí(-1, alias, sid, 3): "NAME wants to trade [Accept]" (:259629)
            char name[PLAYER_NAME_SIZE * 2], line[PLAYER_NAME_SIZE * 2 + 32];
            snprintf(line, sizeof(line), "%s wants to trade ", player_name(r->uid, name, sizeof(name)));
            hud_ui_add_chat_line_on(line, "#ffffff", CHAT_CHANNEL_ALL); // the chat's own row (escaped, its settings)
            // ... then the [Accept] span into that row's message (the last row of #chatList)
            HtmlElem row = 0;
            for (HtmlElem it = html_dom_first_child(html_dom_by_id("chatList")); it; it = html_dom_next(it)) row = it;
            const HtmlElem msg_el = row ? html_dom_query(row, ".chatMsg") : 0;
            char span[160];
            snprintf(span, sizeof(span), "<span class='tradeMsg' style='color:%s' onclick='acceptRequest(%d, this)'>[Accept]</span>",
                     TRADE_GREEN, (int) r->uid);
            if (msg_el) html_dom_insert_html(msg_el, HTML_INSERT_BEFORE_END, span);
            break;
        }
        case LIVE_TRADE_RES_CANCELED: // windows[36].canceled(): active false, showWindow(0)
            g_tr.active = 0;
            g_tr.kr[0] = g_tr.kr[1] = 0;
            close_window();
            break;
        case LIVE_TRADE_RES_ACCEPTED: { // windows[36].accepted(sid) (:251414)
            const int side = r->uid == my_uid() ? 0 : 1;
            g_tr.ready[side] = 1;
            if (side == 0) {
                const HtmlElem box = html_dom_by_id("tSubOffer");
                html_dom_set_attr(box, "disabled", "");
                html_dom_set_checked(box, 1);
            }
            char id[16];
            snprintf(id, sizeof(id), "tHeader%d", side);
            html_dom_set_style(html_dom_by_id(id), "background-color", TRADE_GREEN);
            snprintf(id, sizeof(id), "tradeReady%d", side);
            html_dom_set_text(html_dom_by_id(id), "READY");
            break;
        }
        case LIVE_TRADE_RES_PROCESS: { // windows[36].process(n): "Loading n"
            char label[32];
            snprintf(label, sizeof(label), "Loading %d", (int) r->uid);
            html_dom_set_text(html_dom_by_id("tSubName"), label);
            break;
        }
        case LIVE_TRADE_RES_RESET:
            update_buttons();
            break;
        default:
            break;
    }
    if (msg[0]) chat_line(msg, "#fc03ec"); // iííîiíí(-1, null, msg, 2): a server line (type 2's default colour)
}

// utrd [ownerId, add, skin, copyId] (iïíîiìî, :253308): addItem / removeItem, then updateButtons
static void on_item(const NetTradeItemPacket *p) {
    if (!g_tr.active) return;
    const int side = p->owner_uid == my_uid() ? 0 : 1;
    TradeUiSel *sel = find_sel(side, p->skin, p->copy);
    if (p->add) {
        if (sel) sel->count++;
        else if (g_tr.nsel[side] < TRADE_SEL_MAX) g_tr.sel[side][g_tr.nsel[side]++] = (TradeUiSel) {.skin = p->skin, .count = 1, .copy = p->copy};
    } else if (sel && --sel->count <= 0) {
        *sel = g_tr.sel[side][--g_tr.nsel[side]];
        html_ui_dispatch("cancelTip", NULL, NULL, 0); // the removed card's tooltip
    }
    redraw_side(side);
    update_buttons();
}

// utrdc [playerId, action, currency, amount] (iîiììíì, :253316): 1 adds to the side's KR, 2 removes it
static void on_currency(const NetTradeCurrencyStatePacket *p) {
    if (!g_tr.active || p->currency != LIVE_TRADE_CURRENCY_KR) return;
    const int side = p->owner_uid == my_uid() ? 0 : 1;
    if (p->action == LIVE_TRADE_CUR_ADD) g_tr.kr[side] += p->amount;
    else if (p->action == LIVE_TRADE_CUR_REMOVE) g_tr.kr[side] = 0;
    redraw_side(side);
    update_buttons();
}

// atrd [error, funds, removed, added, meta] (iîïîiîì, :253269): done / failed
static void on_result(const char *json) {
    cJSON *a = cJSON_Parse(json);
    const char *error = cJSON_GetStringValue(cJSON_GetArrayItem(a, 0));
    g_tr.active = 0;
    g_tr.trader = -1;
    g_tr.kr[0] = g_tr.kr[1] = 0;
    close_window();
    if (error && *error) {
        chat_line(error, TRADE_RED); // windows[36].failed(error)
    } else {
        // windows[36].done(): addChat(-1, null, "modes.trade.success", 2, "#5ce05a"). The funds and the owned
        // list come back with the brain's account push ("ua": the header KR, the loadout's owned skins).
        chat_line(I18N("modes.trade.success", "Trade Successful"), TRADE_GREEN);
    }
    cJSON_Delete(a);
    free_inventories();
    clear_offers();
}

// tch [name, msg, type, color] (iiìîïii, :259539): #tradeChatList keeps 35 lines
static void on_chat(const NetTradeChatLinePacket *p) {
    const HtmlElem list = html_dom_by_id("tradeChatList");
    if (!list) return;
    while (html_dom_child_count(list) >= LIVE_TRADE_CHAT_KEEP) html_dom_remove(html_dom_first_child(list));
    char name[NET_TRADE_NAME_MAX + 1], msg[NET_TRADE_MSG_MAX + 1], color[NET_TRADE_COLOR_MAX + 1];
    snprintf(name, sizeof(name), "%.*s", NET_TRADE_NAME_MAX, p->name);
    snprintf(msg, sizeof(msg), "%.*s", NET_TRADE_MSG_MAX, p->msg);
    snprintf(color, sizeof(color), "%.*s", NET_TRADE_COLOR_MAX, p->color);
    StrBuf sb = {0};
    sb_append(&sb, "<div class='tradeChatItem'>");
    if (p->type == 2) {
        sb_append(&sb, "<span class='chatMsg' style='color:");
        sb_append_esc(&sb, color[0] ? color : "#fc03ec");
        sb_append(&sb, "'>");
        sb_append_esc(&sb, msg);
        sb_append(&sb, "</span>");
    } else {
        sb_append_esc(&sb, name[0] ? name : "unknown");
        sb_append(&sb, ": <span class='chatMsg'>");
        sb_append_esc(&sb, msg);
        sb_append(&sb, "</span>");
    }
    sb_append(&sb, "</div><br>");
    char *html = sb_finish(&sb);
    const HtmlElem row = html_dom_create("div", NULL);
    html_dom_set_html(row, html);
    html_dom_append(list, row);
    html_dom_scroll_to_bottom(list);
    free(html);
}

void trade_ui_on_ext(Client *client, const uint16_t subtype, const uint8_t *body, const uint16_t length) {
    (void) client;
    switch (subtype) {
        case NET_EXT_TRADE_RESPONSE: {
            if (length < offsetof(NetTradeResponsePacket, msg)) return;
            NetTradeResponsePacket r;
            memset(&r, 0, sizeof(r));
            memcpy(&r, body, length < sizeof(r) ? length : sizeof(r));
            r.msg[sizeof(r.msg) - 1] = 0;
            on_response(&r);
            break;
        }
        case NET_EXT_TRADE_OPEN: {
            int trader = -1;
            char *json = chunks_push(&g_tr.open, body, length, &trader);
            if (json) on_open(trader, json);
            free(json);
            break;
        }
        case NET_EXT_TRADE_ITEM:
            if (length >= sizeof(NetTradeItemPacket)) {
                NetTradeItemPacket p;
                memcpy(&p, body, sizeof(p));
                on_item(&p);
            }
            break;
        case NET_EXT_TRADE_CURRENCY_STATE:
            if (length >= sizeof(NetTradeCurrencyStatePacket)) {
                NetTradeCurrencyStatePacket p;
                memcpy(&p, body, sizeof(p));
                on_currency(&p);
            }
            break;
        case NET_EXT_TRADE_RESULT: {
            char *json = chunks_push(&g_tr.result, body, length, NULL);
            if (json) on_result(json);
            free(json);
            break;
        }
        case NET_EXT_TRADE_CHAT_LINE:
            if (length >= offsetof(NetTradeChatLinePacket, msg)) {
                NetTradeChatLinePacket p;
                memset(&p, 0, sizeof(p));
                memcpy(&p, body, length < sizeof(p) ? length : sizeof(p));
                on_chat(&p);
            }
            break;
        default:
            break;
    }
}

// ---------------------------------------------------------------- clicks

static int arg_int(const HtmlArg *args, const int argc, const int i, const int def) {
    if (i >= argc) return def;
    if (args[i].kind == HTML_ARG_NUM) return (int) args[i].num;
    if (args[i].kind == HTML_ARG_STR && args[i].str) return atoi(args[i].str);
    return def;
}

static int64_t arg_i64(const HtmlArg *args, const int argc, const int i) {
    if (i >= argc) return 0;
    if (args[i].kind == HTML_ARG_NUM) return (int64_t) args[i].num;
    if (args[i].kind == HTML_ARG_STR && args[i].str) return strtoll(args[i].str, NULL, 10);
    return 0;
}

static void send_update(const int skin, const int action, const int64_t copy) {
    if (!g_tr.active) return;
    const NetTradeUpdatePacket u = {.skin = skin, .action = (uint8_t) action, .copy = copy};
    send_ext(NET_EXT_TRADE_UPDATE, &u, sizeof(u));
}

static void send_currency(const int action, const int amount) {
    if (!g_tr.active) return;
    const NetTradeCurrencyPacket c = {.currency = LIVE_TRADE_CURRENCY_KR, .action = (uint8_t) action, .amount = amount};
    send_ext(NET_EXT_TRADE_CURRENCY, &c, sizeof(c));
}

// addCurrency / requestCurrency (:251437-251461): parseInt(value) > 0, else "Invalid amount"
static void currency_from(const char *input_id, const int action) {
    if (!g_tr.active) return;
    const HtmlElem in = html_dom_by_id(input_id);
    char value[32] = "";
    if (in) html_dom_get_value(in, value, sizeof(value));
    const long long amount = strtoll(value, NULL, 10);
    if (amount <= 0 || amount > INT32_MAX) {
        chat_line("Invalid amount", TRADE_RED);
        return;
    }
    send_currency(action, (int) amount);
    html_dom_set_value(in, "");
}

// searchItems(input, side) (:251220): name / creator / weapon match shows the card, the rest hide
static void search_items(const char *text, const int side) {
    for (int i = 0; i < g_tr.ninv[side]; i++) {
        const TradeUiItem *it = &g_tr.inv[side][i];
        const ViewerSkin *s = viewer_skin(it->skin);
        int match = !text || !*text;
        if (!match && s) {
            match = market_text_contains(s->name, text) || (s->creator && market_text_contains(s->creator, text));
            if (!match && s->weapon > 0) {
                const Weapon *w = rules_weapon(s->weapon - 1);
                match = w && w->name && market_text_contains(w->name, text);
            }
        }
        char key[48], id[64];
        item_key(key, sizeof(key), it->skin, it->copy);
        snprintf(id, sizeof(id), "trd_inv_%d_%s", side, key);
        html_dom_set_display(html_dom_by_id(id), match ? "inline-block" : "none");
    }
}

static int request_trade(const int uid) {
    Client *client = ui_client();
    if (!client || uid < 0 || !game_find_player(&client->game, uid)) return 1; // players.list.find(id)
    const NetTradeRequestPacket r = {.uid = uid};
    send_ext(NET_EXT_TRADE_REQUEST, &r, sizeof(r)); // "strd" [id]
    return 1;
}

int trade_ui_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    if (!name) return -1;
    if (!strcmp(name, "requestTrade")) return request_trade(arg_int(args, argc, 0, -1));
    if (!strcmp(name, "acceptRequest")) { // "trd" [sid, 1]; the line reads [Accepted]
        const NetTradeInitPacket r = {.uid = arg_int(args, argc, 0, -1), .accept = 1};
        send_ext(NET_EXT_TRADE_INIT, &r, sizeof(r));
        if (ev && ev->target) html_dom_set_text(ev->target, "[Accepted]");
        return 1;
    }
    if (strncmp(name, "windows.36.", 11) != 0) return -1;
    const char *m = name + 11;
    if (!strcmp(m, "add") || !strcmp(m, "request")) {
        const int skin = arg_int(args, argc, 0, -1);
        const ViewerSkin *s = viewer_skin(skin);
        if (s && !locked(s)) send_update(skin, !strcmp(m, "add") ? LIVE_TRADE_ITEM_ADD : LIVE_TRADE_ITEM_REQUEST, arg_i64(args, argc, 1));
    } else if (!strcmp(m, "remove")) {
        send_update(arg_int(args, argc, 0, -1), LIVE_TRADE_ITEM_REMOVE, arg_i64(args, argc, 1));
    } else if (!strcmp(m, "accept")) {
        const HtmlElem box = html_dom_by_id("tSubOffer");
        char disabled[8];
        if (g_tr.active && !(box && html_dom_get_attr(box, "disabled", disabled, sizeof(disabled)) >= 0)) send_ext(NET_EXT_TRADE_ACCEPT, NULL, 0);
    } else if (!strcmp(m, "decline")) {
        if (g_tr.active) send_ext(NET_EXT_TRADE_DECLINE, NULL, 0);
    } else if (!strcmp(m, "cancel") || !strcmp(m, "close")) {
        if (g_tr.active) send_ext(NET_EXT_TRADE_CANCEL, NULL, 0);
    } else if (!strcmp(m, "addCurrency")) {
        currency_from("currencyAmount", LIVE_TRADE_CUR_ADD);
    } else if (!strcmp(m, "requestCurrency")) {
        currency_from("currencyAmountRequest", LIVE_TRADE_CUR_REQUEST);
    } else if (!strcmp(m, "removeCurrency")) {
        if (g_tr.active) send_currency(LIVE_TRADE_CUR_REMOVE, 0);
    } else if (!strcmp(m, "searchItems")) {
        search_items(ev && ev->value ? ev->value : "", arg_int(args, argc, 1, 0));
    } else if (!strcmp(m, "sendChat")) {
        const char *text = ev && ev->value ? ev->value : "";
        if (g_tr.active && text[0]) {
            char line[LIVE_TRADE_CHAT_MAX * 4 + 1];
            snprintf(line, sizeof(line), "%s", text);
            send_ext(NET_EXT_TRADE_CHAT, line, (uint16_t) (strlen(line) + 1)); // "tct" [value]
        }
        const HtmlElem in = html_dom_by_id("tradeChatInput");
        html_dom_set_value(in, "");
        html_dom_blur(in);
    } else {
        return -1;
    }
    return 1;
}

// ---------------------------------------------------------------- the window's life

// showWindow(id) for another window cancels the trade (:253813); the window's own close is windows[36].close
void trade_ui_on_show_window(const int id) {
    if (g_tr.active && id != TRADE_WINDOW) {
        g_tr.active = 0; // canceled: the server answers the other side only
        send_ext(NET_EXT_TRADE_CANCEL, NULL, 0);
    }
}

void trade_ui_on_close_window(const int id) {
    if (id == TRADE_WINDOW && g_tr.active) {
        g_tr.active = 0;
        send_ext(NET_EXT_TRADE_CANCEL, NULL, 0); // windows[36].close: cancel(); cancelTip()
        html_ui_dispatch("cancelTip", NULL, NULL, 0);
    }
}

int trade_ui_active(void) { return g_tr.active; }

// The in-world prompt (:192464-192474): in a canTrade mode with no waitTimers, each other active player with an
// account level within 13.2 of us (eye to eye) is changeInteract(me, player, 5, null, 1); the last one wins.
int trade_ui_interact_target(const Client *client) {
    if (!client || !client->me || !client->me->active || !client->game.mode || !client->game.mode->config.can_trade) return -1;
    if (client->game.end_timer > 0.0f) return -1;
    const Player *me = client->me;
    int target = -1;
    for (size_t i = 0; i < client->game.player_count; i++) {
        const Player *p = client->game.players[i];
        if (!p || p == me || !p->active || p->is_you) continue;
        const NetPlayerInfoPacket *info = player_accounts_get(p->uid);
        if (!info || info->level < 1) continue; // IÍîîiìì.level
        const float dx = me->position.x - p->position.x, dy = me->position.y - p->position.y, dz = me->position.z - p->position.z;
        if (sqrtf(dx * dx + dy * dy + dz * dz) <= LIVE_TRADE_RANGE) target = p->uid; // (y + 5.5) - (y + 5.5)
    }
    return target;
}

// "Press [key] to Trade with <span style='color:#9eeb56'>[NAME]</span>" (:197566); "Tap" on a phone
void trade_ui_prompt(const int uid, const char *key_html, const int mobile, char *out, const size_t size) {
    char name[PLAYER_NAME_SIZE * 2];
    StrBuf sb = {0};
    sb_append_esc(&sb, player_name(uid, name, sizeof(name)));
    char *esc = sb_finish(&sb);
    if (mobile) snprintf(out, size, "Tap to Trade with <span style='color:#9eeb56'>[%s]</span>", esc);
    else snprintf(out, size, "Press %s to Trade with <span style='color:#9eeb56'>[%s]</span>", key_html, esc);
    free(esc);
}

void trade_ui_tick(Client *client) {
    if (!g_tr.active || g_scenario_trader[0]) return; // a scenario's window has no session
    // the round switch / the death screen close the window (:256686, :258806): closing it cancels the trade
    if (!client || !client->game.ready || !client->me || !client->me->active) {
        g_tr.active = 0;
        send_ext(NET_EXT_TRADE_CANCEL, NULL, 0);
        close_window();
    }
}

// --render-scenario "trade": {trader: name, invs: [mine, theirs] (v10 rows), offers: [[[skin, count, copy?], ...], [...]],
// kr: [mine, theirs], ready: [0 | 1, 0 | 1], premium: [0 | 1, 0 | 1]}; the scenario's "window": 37 then draws it
void trade_ui_scenario(const cJSON *t) {
    if (!cJSON_IsObject(t)) return;
    free_inventories();
    clear_offers();
    const cJSON *invs = cJSON_GetObjectItemCaseSensitive(t, "invs");
    load_inventory(0, cJSON_GetArrayItem(invs, 0));
    load_inventory(1, cJSON_GetArrayItem(invs, 1));
    const char *name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(t, "trader"));
    snprintf(g_scenario_trader, sizeof(g_scenario_trader), "%s", name && *name ? name : "Trader");
    g_tr.trader = -2;
    g_tr.active = 1;
    for (int side = 0; side < 2; side++) {
        const cJSON *it;
        cJSON_ArrayForEach(it, cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(t, "offers"), side)) {
            if (g_tr.nsel[side] >= TRADE_SEL_MAX) break;
            g_tr.sel[side][g_tr.nsel[side]++] = (TradeUiSel) {.skin = (int) cJSON_GetArrayItem(it, 0)->valuedouble,
                                                              .count = (int) cJSON_GetArrayItem(it, 1)->valuedouble,
                                                              .copy = cJSON_GetArrayItem(it, 2) ? (int64_t) cJSON_GetArrayItem(it, 2)->valuedouble : 0};
        }
        const cJSON *kr = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(t, "kr"), side);
        g_tr.kr[side] = cJSON_IsNumber(kr) ? (int64_t) kr->valuedouble : 0;
        g_tr.ready[side] = cJSON_IsTrue(cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(t, "ready"), side)) ||
                           (cJSON_IsNumber(cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(t, "ready"), side)) &&
                            cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(t, "ready"), side)->valuedouble != 0);
    }
}
