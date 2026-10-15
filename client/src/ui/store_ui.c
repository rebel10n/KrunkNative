// The Krunker v7.1.4 Store window (index.js windows[13]: home, Prize Wheels, Premium,
// Purchase KR, Redeem Keys, Marketplace, the Junkyard) and the prize wheels for a signed-in
// account over the brain's social socket (services/brain/store.c):
//   prizeWheel(i, skin, multi, autoSpin, noAnim), spinButton.onclick: ["spin", wheel, ticket, multi]
//     -> io_spinResponse(err, randNum(s), itemID(s), funds, redeemedTicket): a store wheel, its
//     10x spin (one pointer per item, every second one turning the other way, the items revealed
//     highest rarity first with Next Item) or a TICKET item's spinWheel;
//   the Free KR wheel (claimReward -> prizeWheel(4)): ["fkr"] -> io_skinKR;
//   Spin History (spinHistoryPopup / getSpinHistory / showSpinStats): ["spinH", offset];
//   spinUI.onclick and the #spinUIBtns again / info / equip / next buttons.
// Wheel data is src/data/store.js `wheels` and the TICKET items' spinWheel; skins / rarities come
// from the client's generated store table (client/src/viewer/viewer_store.h). The wheel's 2D
// canvas (rarity slices sized by the odds, the pointers, centre) and the confetti are drawn by
// spin_wheel.c from the state the render loop port below keeps (index.js updateSpinUi). The
// three.js item preview is not ported: the won item shows its preview PNG (UTILS.getPreview),
// flying in and bobbing like the 3D model does.
#include <backend.h>
#include <backend_ui.h>
#include <app_mode.h>
#include <ui_logic.h>
#include <audio.h>
#include <spin_wheel.h>
#include "../viewer/viewer_store.h"
#include <weapons.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ruleset.h>
#include <store_spins.h> // v10 store.wheels / itemPools
#include <time.h>

// data/store.js wheels / a TICKET item's spinWheel (skins.json)
typedef struct {
    const char *name, *head_col, *price_t;
    int price;       // KR per spin
    int multi;       // the "Nx Spin" count, 0 = single spins only
    int icons;       // img/shop/spins/<index>_<0..icons-1>.png
    int free, kr, min_lvl;
    int ticket;      // ticketIndex (the TICKET item that redeems a spin of it), -1 none
    int ticket_only; // only through its ticket (Ultra)
    int big_rar;     // bigRar: the previews (and rolls of this rarity and up) draw from every rarity >= it
    int season;      // 0 = config.currentSeason
    double rarities[7];
    // v10 (shared/include/store_spins.h): the store.wheels entry this was made from, NULL for the
    // TICKET items' spinWheel table below. A pool wheel's slices are its pools; their weights sit
    // in rarities[] by the pool's rarity, so the slices, odds and reveal keep working per rarity.
    const StoreWheel *src;
    int hide, locked;
} UiWheel;

// store.wheels (v10 gapi /data/spins: shared/include/store_spins.h store_spins_builtin(), the
// data kn_brain spins; index = the "spin" frame's wheel index). Built once from that data, so a
// data change (new wheels, pools, prices, tickets) is all it takes.
#define UI_WHEELS_MAX 32
static UiWheel g_ui_wheels[UI_WHEELS_MAX];
static int g_ui_wheel_count = -1;

// a pool's rarity: its items' (a pool holds one rarity; the first item tells)
static int pool_rarity(const StorePool *p) {
    for (int i = 0; p && i < p->count; i++) {
        const ViewerSkin *k = viewer_skin(p->items[i]);
        if (k) return k->rarity;
    }
    return -1;
}

static void wheels_load(void) {
    if (g_ui_wheel_count >= 0) return;
    const StoreSpins *sp = store_spins_builtin();
    g_ui_wheel_count = 0;
    for (int i = 0; i < sp->wheel_count && g_ui_wheel_count < UI_WHEELS_MAX; i++) {
        const StoreWheel *w = &sp->wheels[i];
        UiWheel *u = &g_ui_wheels[g_ui_wheel_count++];
        memset(u, 0, sizeof(*u));
        u->src = w;
        u->name = w->name;
        u->head_col = w->head_col[0] ? w->head_col : "#fff";
        u->price_t = w->price_t[0] ? w->price_t : NULL;
        u->price = w->price;
        u->multi = w->multi;
        u->icons = w->icons;
        u->free = w->free;
        u->kr = w->kr;
        u->min_lvl = w->min_lvl;
        u->ticket = w->ticket;
        u->ticket_only = w->ticket_only;
        u->big_rar = w->big_rar;
        u->season = w->season;
        u->hide = w->hide;
        u->locked = !store_wheel_open(w, (int64_t) time(NULL) * 1000);
        if (w->pool_count > 0) {
            for (int k = 0; k < w->pool_count && k < w->slices; k++) {
                const int r = pool_rarity(store_spins_pool(sp, w->pools[k]));
                if (r >= 0 && r < 7) u->rarities[r] += w->weights[k];
            }
        } else {
            for (int k = 0; k < w->slices && k < 7; k++) u->rarities[k] = w->weights[k];
        }
    }
}

static int wheel_count(void) {
    wheels_load();
    return g_ui_wheel_count;
}

static const UiWheel *wheel_at(const int i) { return i >= 0 && i < wheel_count() ? &g_ui_wheels[i] : NULL; }

// the Free KR wheel's index (claimReward -> prizeWheel(it))
static int free_kr_wheel(void) {
    for (int i = 0; i < wheel_count(); i++)
        if (g_ui_wheels[i].kr) return i;
    return -1;
}

// the wheel whose ticketIndex this item is (the Universal tickets pay a store wheel), -1 none
static int wheel_of_ticket(const int skin) {
    for (int i = 0; skin >= 0 && i < wheel_count(); i++)
        if (g_ui_wheels[i].ticket == skin) return i;
    return -1;
}

// TICKET items with a spinWheel (skins.json; the brain reads the same objects). The Twitch
// Drops ticket's wheel (limGroup "S7 Playercard Drops") shows no previews: the client's store
// table marks every limGroup item VS_NOT_IN_WHEEL.
static const struct { int skin; UiWheel w; } k_ticket_wheels[] = {
    {5321, {"Starter", "#b2f252", NULL, 0, 0, 2, 0, 0, 0, -1, 0, 0, 6, {75, 22, 3, 0, 0, 0}}},
    {5322, {"Elite", "#2196F3", NULL, 0, 0, 3, 0, 0, 0, -1, 0, 0, 6, {50, 30, 15, 5, 0, 0}}},
    {5323, {"Heroic", "#fc9803", NULL, 0, 0, 4, 0, 0, 0, -1, 0, 5, 6, {0, 48, 35, 14, 2.49, 0.5, 0.01}}},
    {5324, {"Ultra", "#ed4242", NULL, 0, 0, 4, 0, 0, 0, -1, 0, 5, 6, {0, 28, 53, 13.5, 4.46, 1, 0.04}}},
    {5906, {"Twitch Drops", "#b2f252", NULL, 0, 0, 2, 0, 0, 0, -1, 0, 3, 7, {0, 0, 60, 35, 5, 0}}},
    {6423, {"Starter", "#b2f252", NULL, 0, 0, 2, 0, 0, 0, -1, 0, 0, 7, {75, 22, 3, 0, 0, 0}}},
    {6424, {"Elite", "#2196F3", NULL, 0, 0, 3, 0, 0, 0, -1, 0, 0, 7, {50, 30, 15, 5, 0, 0}}},
    {6425, {"Heroic", "#fc9803", NULL, 0, 0, 4, 0, 0, 0, -1, 0, 5, 7, {0, 48, 35, 14, 2.49, 0.5, 0.01}}},
    {6426, {"Ultra", "#ed4242", NULL, 0, 0, 4, 0, 0, 0, -1, 0, 5, 7, {0, 28, 53, 13.5, 4.46, 1, 0.04}}},
    {6909, {"Starter", "#b2f252", NULL, 0, 0, 2, 0, 0, 0, -1, 0, 0, 8, {75, 22, 3, 0, 0, 0}}},
    {6910, {"Elite", "#2196F3", NULL, 0, 0, 3, 0, 0, 0, -1, 0, 0, 8, {50, 30, 15, 5, 0, 0}}},
    {6911, {"Heroic", "#fc9803", NULL, 0, 0, 4, 0, 0, 0, -1, 0, 5, 8, {0, 48, 35, 14, 2.49, 0.5, 0.01}}},
    {6912, {"Ultra", "#ed4242", NULL, 0, 0, 4, 0, 0, 0, -1, 0, 5, 8, {0, 28, 53, 13.5, 4.46, 1, 0.04}}},
    {7507, {"Starter", "#b2f252", NULL, 0, 0, 2, 0, 0, 0, -1, 0, 0, 9, {75, 22, 3, 0, 0, 0}}},
    {7508, {"Elite", "#2196F3", NULL, 0, 0, 3, 0, 0, 0, -1, 0, 0, 9, {50, 30, 15, 5, 0, 0}}},
    {7509, {"Heroic", "#fc9803", NULL, 0, 0, 4, 0, 0, 0, -1, 0, 5, 9, {0, 48, 35, 14, 2.49, 0.5, 0.01}}},
    {7510, {"Ultra", "#ed4242", NULL, 0, 0, 4, 0, 0, 0, -1, 0, 5, 9, {0, 28, 53, 13.5, 4.46, 1, 0.04}}},
    // v10 Universal tickets: store.wheels[spinWheelId] (the brain rolls that wheel's item pools)
    {8067, {"Starter", "#b2f252", NULL, 0, 0, 2, 0, 0, 0, -1, 0, 0, 0, {75, 22, 3, 0, 0, 0}}},
    {8068, {"Elite", "#2196F3", NULL, 0, 0, 3, 0, 0, 0, -1, 0, 0, 0, {50, 30, 15, 5, 0, 0}}},
    {7993, {"Heroic", "#fc9803", NULL, 0, 0, 4, 0, 0, 0, -1, 0, 0, 0, {0, 48, 35, 14, 2.49, 0.5, 0.01}}},
};

// store.freeKR[rarity].kr[0]: the Free KR wheel's prizes
static const int k_free_kr[] = {5, 10, 20, 500};

static const UiWheel *ticket_wheel(const int skin) {
    for (size_t i = 0; i < sizeof(k_ticket_wheels) / sizeof(k_ticket_wheels[0]); i++)
        if (k_ticket_wheels[i].skin == skin) return &k_ticket_wheels[i].w;
    return NULL;
}

static int store_window_handle(const char *name, const HtmlArg *args, int argc);
static void generic_popup(const char *html, const char *cls);
static void history_add(int item, int kr);
void store_ui_sub_tab_tick(double delta_ms);
static const UiWheel *g_cur;   // v11667: the wheel on screen
static int g_spin_index = -1;     // vLN01914: its store.wheels index (-1: a ticket's wheel)
static int g_spin_ticket = -1;    // v11656: the TICKET item a spin redeems, -1 none
static int g_multi;               // v11658: wheel.multi for an Nx spin, 0 single
static int g_spinning, g_can_spin, g_can_respin, g_can_equip; // v11655 / v11659 / v11660 / v11661
static double g_spin_button_time; // spinButton click (stopPropagation: spinUI ignores it)

#define SPIN_ITEMS 16 // pointers / items of one spin (wheel.multi is 10)

// index.js render-loop state of the wheel (see store_ui_tick). The per-frame factors (the
// pointers' 0.024 approach, the item camera's 0.24) run at 60 steps per second, the frame rate
// Krunker is tuned for, so a spin takes as long as in a 60 Hz browser (~7 s).
static struct {
    double whl_mlt;             // vLN01912: the wheel grows in (deltaMs * 0.008)
    int pointers;               // vA629.length: one pointer per item
    double target[SPIN_ITEMS];  // vA629: each pointer's angle (the roll's slice)
    double rotation[SPIN_ITEMS]; // vA630
    double speed[SPIN_ITEMS];   // vA631: 0.4..1, normalised to the fastest
    double tick_counter;        // vLN01917: tick_0 every PI / 1.5 of travel
    double end_val, end_target; // vLN01915 / vLN01911: the won rarity's slice fills the wheel
    double cam_x;               // v11662.position.x: 300 -> 30 (item fly-in)
    double bob_time;            // vLN01916
    double rainbow;             // iííìíïí: the won Unobtainable slice's hue (rarityAnim), 0..360
    double last_time, step_time; // glfwGetTime of the last tick / the 60 Hz step clock
    int items[SPIN_ITEMS], item_count; // vA632: the won items, highest rarity first
    int next_item;              // vLN01913: the next item f1840 loads
    int win_skin, win_rarity;   // v11654: the item on show (win_skin -1: a Free KR prize)
    int win_kr;                 // its KR (Free KR wheel)
    int has_item;               // a result is loaded
} g_wheel = {.win_skin = -1, .win_rarity = -1};
static SpinWheelDraw g_wheel_draw;
#define WHEEL_STEP (1.0 / 60.0)

// UTILS.getPreview(skin, store): the preview on the asset host (assetsUrl), not next to the page
void store_skin_preview(const int index, char *out, const size_t n) {
    const ViewerSkin *skin = viewer_skin(index);
    if (!skin) {
        snprintf(out, n, "./img/noimg.png");
        return;
    }
    char path[256];
    viewer_skin_preview_path(skin, path, sizeof(path));
    html_dom_asset_url(path, out, n);
}

static void set_display(const char *id, const char *v) {
    HtmlElem el = html_dom_by_id(id);
    if (el) html_dom_set_display(el, v);
}

static void set_html(const char *id, const char *html) {
    HtmlElem el = html_dom_by_id(id);
    if (el) html_dom_set_html(el, html);
}

static void comma(char *out, const size_t n, const double v) {
    char d[32];
    snprintf(d, sizeof(d), "%.0f", v);
    size_t o = 0;
    const size_t len = strlen(d);
    for (size_t i = 0; i < len && o + 2 < n; i++) {
        if (i && (len - i) % 3 == 0 && d[i - 1] != '-') out[o++] = ',';
        out[o++] = d[i];
    }
    out[o] = 0;
}

static void spin_kr(void) {
    const AccountInfo *acc = account_get();
    if (!acc) return;
    char kr[32], html[128];
    comma(kr, sizeof(kr), acc->funds);
    snprintf(html, sizeof(html), "<span style='color:rgba(255,255,255,0.6)'>%s</span> KR", kr);
    set_html("spinKR", html);
}

// account.skins[ind].cnt (0 for none / guests)
static int owned_count(const int skin) {
    const AccountInfo *acc = account_get();
    const cJSON *skins = acc && acc->data ? cJSON_GetArrayItem(acc->data, 10) : NULL;
    const cJSON *e;
    cJSON_ArrayForEach(e, skins) {
        const cJSON *ind = cJSON_GetObjectItem(e, "ind"), *cnt = cJSON_GetObjectItem(e, "cnt");
        if (cJSON_IsNumber(ind) && ind->valueint == skin) return cJSON_IsNumber(cnt) ? cnt->valueint : 1;
    }
    return 0;
}

// the account's spin tickets (acc.skins of type TICKET, v10 windows[13] viewTickets / the
// Prize Wheels header's count)
static int owned_tickets(void) {
    const AccountInfo *acc = account_get();
    int n = 0;
    const cJSON *e;
    cJSON_ArrayForEach(e, (acc && acc->data ? cJSON_GetArrayItem(acc->data, 10) : NULL)) {
        const ViewerSkin *k = viewer_skin((int) cJSON_GetNumberValue(cJSON_GetObjectItem(e, "ind")));
        const cJSON *cnt = cJSON_GetObjectItem(e, "cnt");
        if (k && k->type == 13) n += cJSON_IsNumber(cnt) ? cnt->valueint : 1;
    }
    return n;
}

// store.rarities[r]: confetti count and colours (confC / confCols)
static const int k_conf_count[] = {30, 30, 30, 40, 50, 70, 100};
static const char *const k_conf_rainbow[] = {"#ff0000", "#ffa500", "#ffff00", "#008000", "#0000ff", "#4b0082", "#ee82ee"};

// UI.toggleMenuHider(on): the menu chrome hides behind the wheel and comes back with it;
// showing it again also hides #spinUI and #spinUIBtns (the buttons live outside #spinUI)
static void toggle_menu_hider(const int on) {
    set_display("menuHider", on ? "block" : "none");
    HtmlElem base = html_dom_by_id("uiBase");
    if (base) html_dom_set_class(base, "onMenu", on);
    set_display("gameNameHolder", on ? "" : "none");
    set_display("spectButton", on ? "" : "none");
    if (on) {
        set_display("spinUI", "none");
        set_display("spinUIBtns", "none");
    }
}

static const UiWheel *current_wheel(void) { return g_cur; }

// wheel.rarities[r] (0 past the wheel's list)
static double wheel_rarity(const UiWheel *w, const int r) {
    return w && r >= 0 && r < 7 ? w->rarities[r] : 0.0;
}

static void hex_rgb(const char *hex, float *rgb) {
    unsigned value = 0xffffff;
    if (hex && *hex == '#') hex++;
    if (hex) sscanf(hex, "%6x", &value);
    rgb[0] = (float) ((value >> 16) & 255) / 255.0f;
    rgb[1] = (float) ((value >> 8) & 255) / 255.0f;
    rgb[2] = (float) (value & 255) / 255.0f;
}

// v10 iïîïîîï(h): the rgb of hue h (degrees, full saturation), written to rgb[0..2]
static void hue_rgb(const double h, float *rgb) {
    const double sector_f = h / 60.0;
    const double x = 1.0 - fabs(fmod(sector_f, 2.0) - 1.0);
    const int sector = (int) floor(sector_f);
    double r = 0, g = 0, b = 0;
    switch (sector < 0 || sector > 5 ? 0 : sector) {
        case 0: r = 1; g = x; b = 0; break;
        case 1: r = x; g = 1; b = 0; break;
        case 2: r = 0; g = 1; b = x; break;
        case 3: r = 0; g = x; b = 1; break;
        case 4: r = x; g = 0; b = 1; break;
        default: r = 1; g = 0; b = x; break;
    }
    rgb[0] = (float) r;
    rgb[1] = (float) g;
    rgb[2] = (float) b;
}

// store.getSpnItems(wheel, rarity, acc, wheel.season || config.currentSeason, wheel.bigRar)
// for the previews: limited / blackM / opensea / limT / limG items never qualify (the store
// wheels have no limType / itemTypes / weaponType)
static int spin_pool(const UiWheel *w, const int rarity, int *pool, const int max) {
    int count = 0;
    if (w && w->src && w->src->pool_count > 0) { // v10 pool wheels: the pools of that rarity (store.getSpnItems)
        const StoreSpins *sp = store_spins_builtin();
        for (int k = 0; k < w->src->pool_count && count < max; k++) {
            const StorePool *p = store_spins_pool(sp, w->src->pools[k]);
            if (pool_rarity(p) != rarity) continue;
            for (int i = 0; i < p->count && count < max; i++)
                if (viewer_skin(p->items[i])) pool[count++] = p->items[i];
        }
        return count;
    }
    const int season = w && w->season ? w->season : g_viewer_current_season;
    const int big = w ? w->big_rar : 0;
    for (int s = 0; s < g_viewer_skin_count && count < max; s++) {
        const ViewerSkin *k = &g_viewer_skins[s];
        if (!(k->rarity == rarity || (big && k->rarity >= big)) || (k->flags & (VS_BLOCKED | VS_NO_SPIN | VS_NOT_IN_WHEEL))) continue;
        if ((k->seas ? k->seas : 1) != season) continue;
        pool[count++] = s;
    }
    return count;
}

// the wheel's top rarity (rarities.indexOf(rarities.filter(r > 0).slice(-1).pop()))
static int top_rarity(const UiWheel *w) {
    int last = 0;
    for (int r = 0; r < 7; r++)
        if (wheel_rarity(w, r) > 0) last = r;
    for (int r = 0; r < 7; r++)
        if (wheel_rarity(w, r) == wheel_rarity(w, last)) return r;
    return last;
}

static void open_item_sales(const int skin) {
    char url[96];
    snprintf(url, sizeof(url), "/social.html?p=itemsales&i=%d", skin);
    if (!app_route_url_target(url, 1)) ui_open_external(url);
}

static void spin_button_click(void);

// prizeWheel(i, skin, multi, autoSpin, noAnim): the spin screen of store wheel `index`, or of
// TICKET item `skin`'s spinWheel; multi = wheel.multi rolls at once; noAnim shows the wheel at
// its full size at once (spinWhlMlt = 1: the respin, and the uidiff win-spin scenarios)
static void prize_wheel(const int index, const int skin, const int multi, const int auto_spin, const int no_anim) {
    // a store wheel (paid with its ticketIndex when skin is that ticket), else the TICKET item's
    // own spinWheel (v10 prizeWheel(index, ticketIndex) / prizeWheel(null, ticket))
    int wheel = index >= 0 ? index : wheel_of_ticket(skin);
    const UiWheel *w = wheel >= 0 ? wheel_at(wheel) : skin >= 0 ? ticket_wheel(skin) : NULL;
    if (!w) return;
    if (!wheel_at(wheel)) wheel = -1;
    g_cur = w;
    g_multi = multi && w->multi ? w->multi : 0;
    set_display("spinAgainBtn", w->free ? "none" : "");
    set_display("spinInfoBtn", w->free ? "none" : "");
    set_display("spinEquipBtn", "");
    set_display("spinNextBtn", g_multi ? "" : "none");
    g_spin_index = wheel;
    g_spin_ticket = skin >= 0 ? skin : -1;
    menu_show_window(MENU_WINDOW_NONE, 0); // showWindow(0)
    g_wheel.whl_mlt = no_anim ? 1 : 0;
    g_can_spin = !g_spinning;
    g_can_respin = 0;
    g_can_equip = !w->kr;
    g_wheel.item_count = 0;
    g_wheel.pointers = 1; // vA629 = vA630 = [0]
    g_wheel.target[0] = g_wheel.rotation[0] = 0;
    g_wheel.speed[0] = 1;
    g_wheel.tick_counter = 0;
    g_wheel.end_val = g_wheel.end_target = 0;
    HtmlElem ui = html_dom_by_id("spinUI");
    char disp[16] = "";
    if (!ui || html_dom_get_style(ui, "display", disp, sizeof(disp)) < 0 || strcmp(disp, "block")) {
        toggle_menu_hider(0);
    }
    set_display("spinItem", "none");
    set_display("spinUIBtns", "none");
    set_display("spinUI", "block");
    set_display("spinItemName", "none");
    g_wheel.cam_x = 300;
    HtmlElem button = html_dom_by_id("spinButton");
    if (button) html_dom_set_style(button, "opacity", "1");
    set_display("spinText", "table-cell");
    set_html("spinHeader", "SPIN");
    const int count = g_multi ? g_multi : 1;
    char buf[512];
    snprintf(buf, sizeof(buf), "-%dKR", w->price * count);
    set_html("spinCost", buf);
    set_display("spinCost", g_spin_ticket >= 0 ? "none" : "");
    if (g_spin_ticket >= 0) {
        char thumb[256];
        store_skin_preview(g_spin_ticket, thumb, sizeof(thumb));
        snprintf(buf, sizeof(buf), "<i class='material-icons' style='margin-top: -10px;margin-left: -10px;text-shadow: none;vertical-align: middle;font-size: 38px;color: #fff;'>refresh</i> Redeem<img src=\"%s\" style=\"width: 40px;vertical-align: text-top;display: inline-block;\">", thumb);
    } else {
        snprintf(buf, sizeof(buf), "<i class='material-icons' style='margin-top: -10px;margin-left: -10px;text-shadow: none;vertical-align: middle;font-size: 38px;color: #fff;'>refresh</i> %d KR", w->price * count);
    }
    set_html("spinAgainBtn", buf);
    // the handlers spinButton / spinUI / spinAgainBtn / spinInfoBtn / spinEquipBtn / spinNextBtn
    // .onclick (set by script in v7.1.4)
    HtmlElem el;
    if ((el = html_dom_by_id("spinButton"))) html_dom_set_attr(el, "onclick", "knSpinButton()");
    if ((el = html_dom_by_id("spinUI"))) html_dom_set_attr(el, "onclick", "knSpinUI()");
    if ((el = html_dom_by_id("spinAgainBtn"))) html_dom_set_attr(el, "onclick", "knSpinRespin()");
    if ((el = html_dom_by_id("spinInfoBtn"))) html_dom_set_attr(el, "onclick", "knSpinRespinInfo()");
    if ((el = html_dom_by_id("spinEquipBtn"))) html_dom_set_attr(el, "onclick", "knSpinEquip()");
    if ((el = html_dom_by_id("spinNextBtn"))) html_dom_set_attr(el, "onclick", "knSpinNext()");

    // #spinInfo: every rarity but the noSpin NFT one (the first 7) with its chance (0% too), the
    // KR wheel its prize per rarity (rarities it can roll only), then Spin History
    StrBuf sb = {0};
    char upper[32];
    snprintf(upper, sizeof(upper), "%s", w->name);
    for (char *p = upper; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
    sb_appendf(&sb, "<div style='font-size:50px;color:#fff'>%s</div>", upper);
    for (int r = 0; r < g_viewer_rarity_count && r < 7; r++) {
        // the Free KR wheel skips its empty rarities; a v10 pool wheel lists its pools only
        if ((w->kr || (w->src && w->src->pool_count > 0)) && !wheel_rarity(w, r)) continue;
        const int anim = r == 6; // RENDER.rarityAnim && rarity.animate (Unobtainable)
        if (anim) sb_append(&sb, "<div style='margin-top:5px;color:#fff'><div class='colCub rainbowBG'></div>");
        else sb_appendf(&sb, "<div style='margin-top:5px;color:#fff'><div class='colCub' style='background-color:%s'></div>", g_viewer_rarities[r].color);
        if (w->kr) {
            sb_appendf(&sb, "<span style='color:rgba(255,255,255,0.5)'>&emsp;%d KR</span></div>", r < 4 ? k_free_kr[r] : 0);
        } else {
            char pct[16];
            snprintf(pct, sizeof(pct), "%g", wheel_rarity(w, r)); // JS number -> string (48, 2.49, 0.5)
            sb_appendf(&sb, "%s<span style='color:rgba(255,255,255,0.5);float:right'>&emsp;%s%%</span></div>", g_viewer_rarities[r].name, pct);
        }
    }
    if (!w->kr) sb_append(&sb, "<div id=\"spinHistoryBtn\" onclick=\"spinHistoryPopup()\">Spin History</div>");
    // v10 (v10.js 217254): the 1x / Nx switch under Spin History; toggleSpinMulti re-opens the wheel
    if (!w->kr && w->multi)
        sb_appendf(&sb, "<div id=\"spinMultiToggleBtn\" onclick=\"toggleSpinMulti();event.stopPropagation();\">Switch to %dx Spin</div>", g_multi ? 1 : w->multi);
    char *html = sb_finish(&sb);
    set_html("spinInfo", html);
    free(html);
    spin_kr();
    // #spinItems: four random items of the wheel's top rarity (store.getSpnItems), + More
    if (!w->kr) {
        int pool[1024];
        const int n = spin_pool(w, top_rarity(w), pool, 1024);
        for (int i = 0; i < 4 && n; i++) {
            const int id = pool[rand() % n];
            char src[160];
            store_skin_preview(id, src, sizeof(src));
            const char *col = g_viewer_rarities[g_viewer_skins[id].rarity].color;
            if (g_viewer_skins[id].type == 4) // SPRAY: its first frame in a div
                sb_appendf(&sb, "<div onclick='viewItem(%d)' class='spinSprayDiv' style='border-color:%s'><div class='spinSprayIcon' style='background: url(&quot;%s&quot;);'></div></div>", id, col, src);
            else
                sb_appendf(&sb, "<img class='spinPrevIcon' style='border-color:%s' onclick='viewItem(%d)' onerror='this.onerror=null;this.src=\"./img/noimg.png\";' src='%s' />", col, id, src);
        }
        if (n) sb_append(&sb, "<span style='color:#fff;line-height:85px;margin-left:6px'>+ More</span>");
        html = sb_finish(&sb);
        set_html("spinItems", html);
        free(html);
    } else {
        set_html("spinItems", "");
    }
    if (auto_spin) spin_button_click();
}

// UTILS.getItemDesc(item) without " Item": the type's market label (i18n social.market.*), a
// weapon skin's weapon name
static const char *item_desc(const ViewerSkin *skin) {
    static const char *const k_keys[17] = {NULL, "social.market.hat", "social.market.body", "social.market.melee",
                                           "social.market.spray", "social.market.dye", "social.market.waist", "social.market.face",
                                           "social.market.shoe", "social.market.pet", "social.market.collectible",
                                           "social.market.wrist", "social.market.charm", "social.market.ticket", "social.market.back",
                                           "social.market.head", "social.market.playerCard"};
    static const char *const k_types[17] = {NULL, "Hat Item", "Body Item", "Melee Item", "Spray Item", "Dye Item", "Waist Item",
                                            "Face Item", "Shoe Item", "Pet Item", "Collectible Item", "Wrist Item", "Charm Item",
                                            "Spin Ticket Item", "Back Item", "Head Item", "Player Card"};
    static char desc[96];
    if (!skin) return "";
    if (skin->type > 0 && skin->type < 17) { // .replace(" Item", "")
        snprintf(desc, sizeof(desc), "%s", i18n_tr(k_keys[skin->type], k_types[skin->type]));
        char *item = strstr(desc, " Item");
        if (item) memmove(item, item + 5, strlen(item + 5) + 1);
        return desc;
    }
    const Weapon *weapon = skin->weapon >= 1 && skin->weapon <= rules_krunker_weapon_count() ? rules_weapon(skin->weapon - 1) : NULL;
    return weapon && weapon->name ? weapon->name : "";
}

// #spinItemCanvas: the item instead of the three.js scene, about the wheel's size (the 256 px
// preview PNGs are cropped to the item), with the rarity's glow; the tick flies it in and bobs it
static void show_item_image(const char *src, const int rarity, const int coin) {
    StrBuf img = {0};
    if (coin) // the Free KR art's coin, cut out round (v7 shows the krunkie_0 coin model)
        sb_appendf(&img, "<div id='knSpinItemImg' style='width:34%%;height:34%%;margin:33%% auto 0;border-radius:50%%;background:url(%s) center / 165%% no-repeat;transform:scale(0)'></div>", src);
    else
        sb_appendf(&img, "<img id='knSpinItemImg' src='%s' onerror='this.onerror=null;this.src=\"./img/noimg.png\";' style='display:block;width:50%%;height:50%%;margin:25%% auto 0;transform:scale(0)'>", src);
    char *html = sb_finish(&img);
    set_html("spinItemCanvas", html);
    free(html);
    HtmlElem item_canvas = html_dom_by_id("spinItemCanvas");
    if (item_canvas && rarity >= 0 && rarity < g_viewer_rarity_count) {
        char filter[64];
        snprintf(filter, sizeof(filter), "drop-shadow(0 0 20px %s)", g_viewer_rarities[rarity].color);
        html_dom_set_style(item_canvas, "filter", filter);
    }
}

// f1840: the next won item (vA632[vLN01913]) on show: its name card and image
static void load_next_item(void) {
    if (g_wheel.next_item >= g_wheel.item_count) return;
    const int item = g_wheel.items[g_wheel.next_item++];
    const ViewerSkin *skin = viewer_skin(item);
    const int owned = owned_count(item);
    StrBuf sb = {0};
    sb_append_esc(&sb, skin ? skin->name : "?");
    sb_append(&sb, "<div style='color:rgba(255,255,255,0.5)'>");
    sb_append_esc(&sb, item_desc(skin));
    sb_append(&sb, "</div><div style='color:rgba(255,255,255,0.5)'>");
    if (owned > 1) sb_appendf(&sb, "<span style='color:#fff'>x%d</span> Owned", owned);
    else sb_append(&sb, "<span style='color:#fff'>NEW</span>");
    sb_append(&sb, "</div>");
    char *html = sb_finish(&sb);
    set_html("spinItemName", html);
    free(html);
    g_wheel.bob_time = 0;
    g_wheel.win_skin = item;
    g_wheel.win_rarity = skin ? skin->rarity : 0;
    g_wheel.win_kr = 0;
    g_wheel.has_item = 1;
    char src[160];
    store_skin_preview(item, src, sizeof(src));
    show_item_image(src, g_wheel.win_rarity, 0);
    printf("[store] unboxing %s (skin %d, %d owned)\n", skin ? skin->name : "?", item, owned);
}

// f1835 / showSpinError: "Spin Error 9" (and the brain's "Not enough KR") open Purchase KR,
// any other error a "Spin Error" popup over the re-shown wheel
static void spin_error(const char *err) {
    if (!err || !strcmp(err, "Spin Error 9") || strstr(err, "Not enough KR")) {
        const HtmlArg one = {HTML_ARG_NUM, 0, NULL};
        store_window_handle("windows.13.purchase", &one, 1); // windows[13].purchase(): out of funds
        return;
    }
    HtmlElem ui = html_dom_by_id("spinUI");
    char disp[16] = "";
    if (g_cur && ui && html_dom_get_style(ui, "display", disp, sizeof(disp)) >= 0 && !strcmp(disp, "block"))
        prize_wheel(g_spin_index, g_spin_ticket, g_multi, 0, 0);
    char html[256];
    snprintf(html, sizeof(html), "<div class=\"popHeader\">Spin Error</div>%s", strcmp(err, "DISABLED") ? err : "Spins are disabled in your country");
    generic_popup(html, "spinErrorPop");
}

// rarity of a won item (store.skins[i].rarity || 0)
static int item_rarity(const int item) {
    const ViewerSkin *k = viewer_skin(item);
    return k ? k->rarity : 0;
}

// io_spinResponse(error, randNums, itemIDs, funds, removedSkin): one number or arrays (a
// multi-spin). The items sort by rarity (highest first, their rolls with them), each gets a
// pointer that turns to its roll's slice (every second one the other way round, each at its
// own speed), the account gains them (and loses the redeemed ticket) and the first is loaded.
static void spin_response(const cJSON *frame) {
    g_spinning = 0;
    const char *err = cJSON_GetStringValue(cJSON_GetArrayItem(frame, 1));
    const cJSON *rolls = cJSON_GetArrayItem(frame, 2), *ids = cJSON_GetArrayItem(frame, 3);
    if (err || !(cJSON_IsNumber(ids) || cJSON_IsArray(ids))) {
        spin_error(err ? err : "Spin Error 3");
        return;
    }
    double roll[SPIN_ITEMS];
    int item[SPIN_ITEMS], n = 0;
    if (cJSON_IsArray(ids)) {
        for (int i = 0; i < cJSON_GetArraySize(ids) && n < SPIN_ITEMS; i++, n++) {
            item[n] = (int) cJSON_GetNumberValue(cJSON_GetArrayItem(ids, i));
            roll[n] = cJSON_IsArray(rolls) ? cJSON_GetNumberValue(cJSON_GetArrayItem(rolls, i)) : 0;
        }
    } else {
        item[0] = (int) cJSON_GetNumberValue(ids);
        roll[0] = cJSON_IsNumber(rolls) ? rolls->valuedouble : 0;
        n = 1;
    }
    for (int i = 1; i < n; i++) // stable insertion sort by rarity, highest first
        for (int j = i; j > 0 && item_rarity(item[j]) > item_rarity(item[j - 1]); j--) {
            const int ti = item[j];
            const double tr = roll[j];
            item[j] = item[j - 1];
            roll[j] = roll[j - 1];
            item[j - 1] = ti;
            roll[j - 1] = tr;
        }
    account_set_funds(cJSON_GetNumberValue(cJSON_GetArrayItem(frame, 4)));
    for (int i = 0; i < n; i++) {
        account_add_skin(item[i]);
        history_add(item[i], g_cur && g_spin_ticket < 0 ? g_cur->price : 0); // f1838
    }
    const cJSON *removed = cJSON_GetArrayItem(frame, 5);
    if (cJSON_IsNumber(removed)) account_remove_skin(removed->valueint);
    spin_kr();
    double fastest = 0;
    for (int i = 0; i < n; i++) {
        g_wheel.items[i] = item[i];
        g_wheel.target[i] = i % 2 == 0 ? M_PI * 2 * (roll[i] / 100 + 13) + M_PI / 2 : M_PI / 2 - M_PI * 2 * (13 - roll[i] / 100);
        g_wheel.rotation[i] = 0;
        g_wheel.speed[i] = 0.4 + (double) rand() / ((double) RAND_MAX + 1.0) * 0.6;
        if (g_wheel.speed[i] > fastest) fastest = g_wheel.speed[i];
    }
    for (int i = 0; i < n; i++) g_wheel.speed[i] /= fastest;
    g_wheel.pointers = n;
    g_wheel.item_count = n;
    g_wheel.next_item = 0;
    g_wheel.step_time = glfwGetTime();
    set_html("spinCost", "");
    load_next_item();
}

// io_skinKR(lastReward, randNum, freeKRIndex, funds, err): the Free KR wheel's prize. v7.1.4
// sets the v3 global spinVal here and leaves its pointer (vA629 = [0]) where it is, so the
// wheel never stops on the prize nor reveals it; KrunkNative turns the pointer to the roll
// like a store wheel's.
static void spin_kr_response(const cJSON *frame) {
    g_spinning = 0;
    const char *err = cJSON_GetStringValue(cJSON_GetArrayItem(frame, 5));
    if (err) {
        spin_error(err);
        return;
    }
    const int index = (int) cJSON_GetNumberValue(cJSON_GetArrayItem(frame, 3));
    const double roll = cJSON_GetNumberValue(cJSON_GetArrayItem(frame, 2));
    account_set_last_reward(cJSON_GetNumberValue(cJSON_GetArrayItem(frame, 1)));
    account_set_funds(cJSON_GetNumberValue(cJSON_GetArrayItem(frame, 4)));
    spin_kr();
    g_wheel.win_skin = -1;
    g_wheel.win_rarity = index >= 0 && index < 4 ? index : 0;
    g_wheel.win_kr = k_free_kr[g_wheel.win_rarity];
    g_wheel.has_item = 1;
    g_wheel.item_count = g_wheel.next_item = 0;
    char html[64];
    snprintf(html, sizeof(html), "%d KR", g_wheel.win_kr);
    set_html("spinItemName", html);
    set_html("spinCost", "");
    g_wheel.pointers = 1;
    g_wheel.target[0] = M_PI * 2 * (roll / 100 + 13) + M_PI / 2;
    g_wheel.rotation[0] = 0;
    g_wheel.speed[0] = 1;
    g_wheel.bob_time = 0;
    g_wheel.step_time = glfwGetTime();
    show_item_image("./img/shop/spins/4_0.png", g_wheel.win_rarity, 1);
}

// f1846: every pointer at its target
static int pointers_done(void) {
    for (int i = 0; i < g_wheel.pointers; i++)
        if (g_wheel.rotation[i] != g_wheel.target[i]) return 0;
    return 1;
}

// f1847(next): once every pointer stopped: (the next item,) the end animation (the item's
// slice fills the wheel), reward sound, confetti, "unbx" and the cheer
static void spin_finished(const int next) {
    if (!pointers_done()) return;
    if (next) load_next_item();
    const UiWheel *w = current_wheel();
    const int rarity = g_wheel.win_rarity;
    g_wheel.end_val = 0;
    g_wheel.end_target = 100; // v10 f1847: iîiîîiï = 100 (the won slice then covers the whole wheel)
    printf("[store] wheel stopped on %s\n", rarity >= 0 && rarity < g_viewer_rarity_count ? g_viewer_rarities[rarity].name : "?");
    audio_play("reward", 0.3f, 0, 1.0f);
    spin_wheel_confetti_reset();
    if (rarity >= 0 && rarity < 7) {
        const char *cols[2] = {g_viewer_rarities[rarity].color, "#fff"};
        if (rarity == 6) spin_wheel_confetti(k_conf_count[rarity], k_conf_rainbow, 7);
        else spin_wheel_confetti(k_conf_count[rarity], cols, 2);
    }
    backend_social_send_text("[\"unbx\"]");
    if (rarity >= 4) audio_play("cheer_1", 0.1f, 0, 1.0f);      // >= RELIC
    else if (rarity >= 1) audio_play("cheer_0", 0.1f, 0, 1.0f); // >= RARE
}

// spinButton.onclick: ["spin", wheel, ticket, multi] or the Free KR wheel's ["fkr"]
static void spin_button_click(void) {
    g_spin_button_time = glfwGetTime();
    const AccountInfo *acc = account_get();
    if (!g_cur || !acc || g_spinning || !g_can_spin) return;
    if (g_spin_ticket < 0 && acc->funds < g_cur->price) {
        spin_error("Spin Error 9");
        return;
    }
    g_spinning = 1;
    g_can_spin = 0;
    char frame[96];
    if (g_cur->kr) snprintf(frame, sizeof(frame), "[\"fkr\"]");
    else if (g_spin_ticket >= 0 && g_spin_index >= 0) // a store wheel paid with its ticket (v10 Universal tickets)
        snprintf(frame, sizeof(frame), "[\"spin\",%d,%d,%s]", g_spin_index, g_spin_ticket, g_multi ? "true" : "false");
    else if (g_spin_ticket >= 0) snprintf(frame, sizeof(frame), "[\"spin\",null,%d,false]", g_spin_ticket);
    else snprintf(frame, sizeof(frame), "[\"spin\",%d,null,%s]", g_spin_index, g_multi ? "true" : "false");
    backend_social_send_text(frame);
    set_html("spinHeader", "");
    set_html("spinCost", I18N("windows.store.purchasing", "Purchasing..."));
    audio_play("buy_1", 0.1f, 0, 1.0f);
}

// ---------------------------------------------------------------- spin history (index.js
// spinHistoryPopup / getSpinHistory / io_saveSpinHistory / showSpinStats)

static cJSON *g_hist;          // vA636: [{ind, dte (epoch ms), fds}], newest first
static int g_hist_loaded;      // v11685
static int g_hist_offset;      // vLN01921: rows fetched so far (the next "spinH" offset)
static int g_hist_shown = -1;  // v11686: rows already in #spinHistoryList, -1 = rebuild it

// f1838: a won item goes on top of a loaded history (with the wheel's price)
static void history_add(const int item, const int kr) {
    if (!g_hist_loaded || !g_hist) return;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "ind", item);
    cJSON_AddNumberToObject(o, "dte", (double) time(NULL) * 1000.0);
    cJSON_AddNumberToObject(o, "fds", kr);
    cJSON_InsertItemInArray(g_hist, 0, o);
    g_hist_offset++;
    if (g_hist_shown >= 0) g_hist_shown++;
}

// new Date(dte).toDateString(): "Mon Oct 12 2026"
static void date_string(const double ms, char *out, const size_t n) {
    const time_t t = (time_t) (ms / 1000.0);
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    strftime(out, n, "%a %b %d %Y", &tmv);
}

// f1836: one history item (spray div or preview icon)
static void history_item(StrBuf *sb, const cJSON *row) {
    const int ind = (int) cJSON_GetNumberValue(cJSON_GetObjectItem(row, "ind"));
    const ViewerSkin *k = viewer_skin(ind);
    if (!k) return;
    char src[160];
    store_skin_preview(ind, src, sizeof(src));
    const char *col = k->rarity >= 0 && k->rarity < g_viewer_rarity_count ? g_viewer_rarities[k->rarity].color : "#fff";
    if (k->type == 4)
        sb_appendf(sb, "<div onclick='viewItem(%d)' class='spinSprayDiv' style='margin:5px;border-color:%s'><div class='spinSprayIcon' style='background: url(&quot;%s&quot;);'></div></div>", ind, col, src);
    else
        sb_appendf(sb, "<img class='spinPrevIcon' style='margin:5px;border-color:%s' onclick='viewItem(%d)' src='%s' />", col, ind, src);
}

static void history_request(void) {
    char frame[48];
    snprintf(frame, sizeof(frame), "[\"spinH\",%d]", g_hist_offset);
    backend_social_send_text(frame);
}

static void history_popup(const int more) {
    if (!pointers_done()) return;
    if (!g_hist_loaded) {
        account_popup(2, NULL, "Fetching Spin History..."); // io_loadingPopup
        history_request();
        return;
    }
    const int count = g_hist ? cJSON_GetArraySize(g_hist) : 0;
    if (!count) {
        account_popup(0, "No spin history found", NULL); // io_showPopup(null, ...)
        return;
    }
    ui_loadout_reset_pops();
    set_display("spinHistoryPop", "block");
    char newest[32], oldest[32], range[96];
    date_string(cJSON_GetNumberValue(cJSON_GetObjectItem(cJSON_GetArrayItem(g_hist, 0), "dte")), newest, sizeof(newest));
    date_string(cJSON_GetNumberValue(cJSON_GetObjectItem(cJSON_GetArrayItem(g_hist, count - 1), "dte")), oldest, sizeof(oldest));
    snprintf(range, sizeof(range), "Current Date Range: %s - %s", oldest, newest);
    StrBuf sb = {0};
    if (g_hist_shown < 0) {
        sb_append(&sb, "<div class='spinHistoryHeader'><div class='spinHistoryStats' onmouseover='playTick();' onclick='playSelect();showSpinStats(this);'>Stats</div>Recent Items<div class='spinHistoryClear' onmouseover='playTick();' onclick='playSelect();getSpinHistory(this);'>Load More</div></div>");
        sb_appendf(&sb, "<div class=\"spinHistoryTime\">%s</div><div id='spinHistoryList'>", range);
        for (int i = 0; i < count; i++) history_item(&sb, cJSON_GetArrayItem(g_hist, i));
        sb_append(&sb, "</div><div class='spinHistoryWarning'>Data only tracks back to April 2020</div>");
        char *html = sb_finish(&sb);
        set_html("spinHistoryPop", html);
        free(html);
    } else {
        HtmlElem btn = html_dom_query(0, ".spinHistoryClear");
        if (btn) {
            html_dom_set_class(btn, "disabled", 0);
            html_dom_set_html(btn, "Load More");
        }
        HtmlElem t = html_dom_query(0, ".spinHistoryTime");
        if (t) html_dom_set_text(t, range);
        for (int i = g_hist_shown; i < count; i++) history_item(&sb, cJSON_GetArrayItem(g_hist, i));
        char *html = sb_finish(&sb);
        HtmlElem list = html_dom_by_id("spinHistoryList");
        if (list && html[0]) html_dom_insert_html(list, HTML_INSERT_BEFORE_END, html);
        free(html);
        if (list && more) html_dom_scroll_to_bottom(list);
    }
    g_hist_shown = count;
}

// io_saveSpinHistory(rows)
static void history_response(const cJSON *frame) {
    const cJSON *rows = cJSON_GetArrayItem(frame, 1);
    if (!g_hist) g_hist = cJSON_CreateArray();
    const cJSON *r;
    cJSON_ArrayForEach(r, rows) {
        g_hist_offset++;
        if (viewer_skin((int) cJSON_GetNumberValue(cJSON_GetObjectItem(r, "ind")))) cJSON_AddItemToArray(g_hist, cJSON_Duplicate(r, 1));
    }
    const int first = !g_hist_loaded;
    g_hist_loaded = 1;
    if (first) ui_loadout_clear_pops(); // the loading popup
    history_popup(1);
}

// showSpinStats
static void history_stats(void) {
    const int count = g_hist ? cJSON_GetArraySize(g_hist) : 0;
    if (!count) {
        account_popup(0, "No spin history found", NULL);
        return;
    }
    char newest[32], oldest[32], a[32], b[32], c[32];
    date_string(cJSON_GetNumberValue(cJSON_GetObjectItem(cJSON_GetArrayItem(g_hist, 0), "dte")), newest, sizeof(newest));
    date_string(cJSON_GetNumberValue(cJSON_GetObjectItem(cJSON_GetArrayItem(g_hist, count - 1), "dte")), oldest, sizeof(oldest));
    double spent = 0, value = 0;
    int per[7] = {0};
    for (int i = 0; i < count; i++) {
        const cJSON *row = cJSON_GetArrayItem(g_hist, i);
        const int ind = (int) cJSON_GetNumberValue(cJSON_GetObjectItem(row, "ind"));
        const int r = item_rarity(ind);
        if (r >= 0 && r < 7) per[r]++;
        spent += cJSON_GetNumberValue(cJSON_GetObjectItem(row, "fds"));
        value += social_estimate_value(ind);
    }
    comma(a, sizeof(a), spent);
    comma(b, sizeof(b), value);
    comma(c, sizeof(c), count);
    StrBuf sb = {0};
    sb_appendf(&sb, "<div class=\"popHeader\">Spin Stats</div><div class=\"spinHistoryTime\">Current Date Range: %s - %s</div><div style=\"text-align:left\">", oldest, newest);
    sb_appendf(&sb, "<div style=\"color:#fff\">Total Spent:<span style='color:rgba(255,255,255,0.5);float:right'>&emsp;%s KR</span></div>", a);
    sb_appendf(&sb, "<div style=\"color:#fff\">Est Value:<span style='color:rgba(255,255,255,0.5);float:right'>&emsp;%s KR</span></div>", b);
    sb_appendf(&sb, "<div style=\"color:#fff\">Total Spins:<span style='color:rgba(255,255,255,0.5);float:right'>&emsp;%s</span></div>", c);
    for (int r = 0; r < g_viewer_rarity_count && r < 7; r++) {
        if (r == 6) sb_append(&sb, "<div style='margin-top:5px;color:#fff'><div class='colCub rainbowBG'></div>");
        else sb_appendf(&sb, "<div style='margin-top:5px;color:#fff'><div class='colCub' style='background-color:%s'></div>", g_viewer_rarities[r].color);
        comma(a, sizeof(a), per[r]);
        sb_appendf(&sb, "%s<span style='color:rgba(255,255,255,0.5);float:right'>&emsp;%s</span></div>", g_viewer_rarities[r].name, a);
    }
    sb_append(&sb, "</div>");
    char *html = sb_finish(&sb);
    generic_popup(html, "spinHistoryStatsPop");
    free(html);
}

// the account changed: a new login starts a new history
void store_ui_account_changed(void) {
    cJSON_Delete(g_hist);
    g_hist = NULL;
    g_hist_loaded = 0;
    g_hist_offset = 0;
    g_hist_shown = -1;
}

// ---------------------------------------------------------------- windows[13] (Store)

static int g_store_tab;      // windows[13].tabIndex
static int g_store_sub;      // subTab: the home tab's banner (Battle Pass / Junkyard / Premium)
static double g_store_sub_t = 3000.0; // subTabT: ms until the next banner

// config.shopTabs
static const char *const k_shop_tabs[] = {
    "<span class='material-icons' style='color:#fbc02d;font-size:42px;vertical-align:bottom'>home</span>", "Prize Wheels", "Premium",
    "Purchase <span style='color:#fbc02d'>KR</span>", "Redeem Keys", "Marketplace",
    "<span class='material-icons' style='color:#cc51f8;font-size:42px;vertical-align:bottom'>plumbing</span>",
};

// data/store.js premium / purchases
static const struct { int days; const char *price, *col; } k_premium[] = {
    {7, "1.49", "#a1f326"}, {30, "4.99", "#2196F3"}, {60, "8.99", "#E040FB"}, {90, "12.99", "#fc9803"},
};
static const struct { int val; const char *price, *col, *tag, *tag_col; } k_purchases[] = {
    {300, "0.99", "#888888", NULL, NULL}, {600, "1.79", "#b2f251", NULL, NULL}, {2600, "7.49", "#3696f3", NULL, NULL},
    {7000, "15.99", "#e040fb", NULL, NULL}, {20000, "34.99", "#fa9704", "Popular!", "#ed4242"},
    {60000, "99.99", "#ee5356", "Best Value!", "#E040FB"},
};

static void store_asset_url(const char *path, char *out, const size_t n) { html_dom_asset_url(path, out, n); }

// UTILS.getReadableTime(ms): "0m", "5h 3m", "2d 4h"
static void readable_time(char *out, const size_t n, double ms) {
    const long m = (long) (ms / 60000.0), h = m / 60, d = h / 24;
    if (d > 0) snprintf(out, n, "%ldd %ldh", d, h % 24);
    else if (h > 0) snprintf(out, n, "%ldh %ldm", h, m % 60);
    else snprintf(out, n, "%ldm", m);
}

// windows[13].genSubBtns()
static void store_sub_btns(StrBuf *sb) {
    for (int i = 0; i < 3; i++) {
        sb_appendf(sb, "<div class='shpMTbsI' style='background:%s' onclick='event.stopPropagation();windows[13].subTabT=5000;windows[13].subTab=%d;updateWindow(14);'></div>",
                   i == g_store_sub ? "#fff" : "rgba(255,255,255,0.4)", i);
    }
}

// A random thumbnail of the wheel's top rarity (getSpnItems + yateShuffle), "" if none.
static void wheel_thumb(const UiWheel *w, char *out, const size_t n) {
    int pool[1024];
    const int count = spin_pool(w, top_rarity(w), pool, 1024);
    out[0] = 0;
    if (count) store_skin_preview(pool[rand() % count], out, n);
}

static void store_home(StrBuf *sb, const AccountInfo *acc) {
    char url[256], sub[1024];
    StrBuf subs = {0};
    store_sub_btns(&subs);
    char *sub_btns = sb_finish(&subs);
    snprintf(sub, sizeof(sub), "%s", sub_btns);
    free(sub_btns);
    sb_append(sb, "<div style='text-align: center;'>");
    store_asset_url("img/battlepass/bg_window_2.png", url, sizeof(url));
    sb_appendf(sb, "<div id='shopMainSellB' class='windowSubTabs' onclick='playSelect();showWindow(6);'  style='background:linear-gradient(rgb(0 0 0 / 38%%), #00000000, rgb(10 10 10 / 41%%)),url(img/battlepass/bg_window_2.png);background-size:100%%;%s'><div class='shopMainSellTbs'>%s</div><div id='shopMainNav'>Unlock new Items!</div><div id='shopMainHedr'><div id='shopMainSubT' style=''>Checkout the</div>Battle Pass</div></div>",
               g_store_sub == 0 ? "" : "display:none;", sub);
    sb_appendf(sb, "<div id='shopMainSellB' class='windowSubTabs' onclick='playSelect();windows[13].premium();'  style='background:linear-gradient(rgb(0 0 0 / 38%%), #00000000, rgb(10 10 10 / 41%%)),url(img/prem_bg.png);background-size:120%%;%s'><div class='shopMainSellTbs'>%s</div><div id='shopMainNav'>Unlock new benefits!</div><div id='shopMainHedr'><div id='shopMainSubT' style='color:var(--yellow)!important'>Subscribe to</div>Premium</div></div>",
               g_store_sub == 2 ? "" : "display:none;", sub);
    store_asset_url("img/backgrounds/junkyrd_0.png", url, sizeof(url));
    sb_appendf(sb, "<div id='shopMainSellB' class='windowSubTabs' onclick='windows[13].craftMenu();'  style='background: linear-gradient(rgb(10 10 10 / 37%%), #0000003b),url(%s);background-size:cover;background-position-x: -600px;%s'><div class='shopMainSellTbs'>%s</div><div id='shopMainNav'>Craft new Items!</div><div id='shopJnkDsp'>0.00<span class='material-icons' style='color:#cc51f8;font-size:38px;margin-left:8px;vertical-align:top;margin-right:-9px;'>plumbing</span></div><div id='shopMainHedr'><div id='shopMainSubT'>Enter the</div>Junkyard</div></div>",
               url, g_store_sub == 1 ? "" : "display:none;", sub);
    // the Heroic spin card (Game.store.wheels[2]): 1x / Nx spins, Redeem with its ticket
    const UiWheel *hw = wheel_at(2);
    if (!hw) return;
    const char *col = hw->head_col;
    char thumb[256], ticket[256];
    wheel_thumb(hw, thumb, sizeof(thumb));
    store_skin_preview(hw->ticket, ticket, sizeof(ticket));
    sb_appendf(sb, "<div id='shopSideSellB'%s onmouseenter='playTick()' \n\t\t\t\tstyle='color:%s;border: 4px solid;text-shadow: 0px 0px 30px rgb(0 0 0);background: rgba(0, 0, 0, 0.4);height:361px!important'>",
               acc ? "" : " onclick='showWindow(5)'", col);
    if (acc) {
        sb_append(sb, "<div class='shopActionOvrl'>");
        sb_appendf(sb, "<div class=\"shopActions\">\n\t\t\t\t\t\t\t<div class=\"shopSubAction\" onclick=\"playSelect();prizeWheel(2)\" onmouseenter='playTick()' style=\"font-size: 34px;!important\">\n\t\t\t\t\t\t\t\t1x Spin <div style=\"color:%s\">%d KR</div>\n\t\t\t\t\t\t\t</div>\n\t\t\t\t\t\t\t<div class=\"shopSubActionSep\" style=\"background:%s\"></div>\n\t\t\t\t\t\t\t<div class=\"shopSubAction\" onclick=\"playSelect();prizeWheel(2, null, %d)\" onmouseenter='playTick()' style=\"font-size: 34px;!important\">\n\t\t\t\t\t\t\t\t%dx Spin <div style=\"color:%s\">%d KR</div>\n\t\t\t\t\t\t\t</div>\n\t\t\t\t\t\t</div>",
                   col, hw->price, col, hw->multi, hw->multi, col, hw->price * hw->multi);
        sb_appendf(sb, "<div style='display: table-cell;'><div class=\"shopActionSep\" style=\"background:%s\"></div></div>", col);
        sb_appendf(sb, "<div class=\"shopAction\" onclick=\"playSelect();prizeWheel(null, %d)\"\n\t\t\t\t\t\tonmouseenter='playTick()' style=\"font-size: 34px;!important\">\n\t\t\t\t\t\tRedeem\n\t\t\t\t\t\t<div style=\"color:%s\">\n\t\t\t\t\t\t\t<img class=\"ticketImg\" src=\"%s\" style=\"width:70px!important;\">\n\t\t\t\t\t\t</div>\n\t\t\t\t\t</div>",
                   hw->ticket, col, ticket);
        sb_appendf(sb, "<div class=\"ticketCount\" style=\"font-size:22px!important;color:%s\">You have %d</div></div>", col, owned_count(hw->ticket));
    }
    sb_appendf(sb, "<div class='shopSpinValH'>Heroic Spin<div class='shopSpinValL' style='color:%s !important;'>%d KR<div class=\"ticketImgSep\">|</div><img class=\"ticketImg\" src=\"%s\" style=\"width:56px!important\"></div></div>",
               col, hw->price, ticket);
    sb_appendf(sb, "<div class='shopSpinImg' style='background-image: url(%s);'></div><div class='shopImgGrad' style='margin-left:0!important;background-image: linear-gradient(transparent, %s30);'></div></div>", thumb, col);
    // bundles: none without the store API (bundleData)
    sb_append(sb, "<div id='shopBundleHolder'><div class=\"bundleSliderH\">\n\t\t\t\t\t<div class=\"bundleSliderBtn disabled\" onmouseenter=\"playTick(0.1);\" onmousedown=\"playSelect(0.1);windows[13].updateScroll(-1);\">\n\t\t\t\t\t\t<div class=\"bundleSliderArrow\"></div>\n\t\t\t\t\t</div>\n\t\t\t\t</div><div class=\"bundleCols\" onwheel=\"windows[13].updateScrollWheel();\"></div><div class=\"bundleSliderH\">\n\t\t\t\t\t<div class=\"bundleSliderBtn\" onmouseenter=\"playTick(0.1);\" onmousedown=\"playSelect(0.1);windows[13].updateScroll(1);\">\n\t\t\t\t\t\t<div class=\"bundleSliderArrow right\"></div>\n\t\t\t\t\t</div>\n\t\t\t\t</div></div></div>");
}

static void store_wheels(StrBuf *sb, const AccountInfo *acc) {
    sb_appendf(sb, "<div class='setBodH' style='padding-top:15px'><div style='color:rgba(255,255,255,0.6);margin-bottom:10px;margin-top:0px;width:100%%;text-align:center'>%s</div></div>",
               I18N("windows.store.wheels.sub", "Try your luck and spin for a chance to win some new skins!"));
    sb_append(sb, "<div class='prizeWheelTable'>");
    char thumb[256] = "undefined", ticket[256], url[256];
    for (int i = 0; i < wheel_count(); i++) {
        const UiWheel *w = wheel_at(i);
        if (w->hide || w->locked) continue; // v7's list: the open store wheels
        const char *col = w->head_col;
        int sold = !acc || (w->free && acc->level < w->min_lvl);
        int show_kr = 1; // v11318: " KR" after the value
        char value[32];
        if (w->kr) { // UTILS.getTimeH(time left) until the Free KR wheel can be claimed
            const double ms = account_reward_ms();
            const long long t = ms > 0 ? (long long) ms / 1000 : 0;
            if (t > 0) snprintf(value, sizeof(value), "%02lld:%02lld:%02lld", t / 3600, (t / 60) % 60, t % 60);
            else snprintf(value, sizeof(value), "%s", w->price_t);
            sold = t > 0;
            show_kr = 0;
        } else if (sold) {
            snprintf(value, sizeof(value), "Sold Out");
        } else if (w->price_t) {
            snprintf(value, sizeof(value), "%s", w->price_t);
        } else {
            snprintf(value, sizeof(value), "%d", w->price);
        }
        const char *kr_suffix = w->price_t || !show_kr ? "" : " KR";
        const int has_ticket = w->ticket >= 0, ticket_only = w->ticket_only;
        if (!w->kr && acc) wheel_thumb(w, thumb, sizeof(thumb));
        if (has_ticket) store_skin_preview(w->ticket, ticket, sizeof(ticket));
        sb_appendf(sb, "<div class='shopCard%s' onmouseenter='playTick()' style='color:%s;border: 4px solid;text-shadow: 0px 0px 30px rgb(0 0 0);width:293px;height: 202px;",
                   sold ? " makeGray" : "", col);
        if (w->kr) {
            store_asset_url("img/shop/filter/vignette.png", url, sizeof(url));
            sb_appendf(sb, "background-image:url(%s),url(&quot;img/shop/spins/%d_0.png&quot;)'", url, i);
        } else {
            sb_append(sb, "background: rgba(0, 0, 0, 0.4);'");
        }
        if (!(sold || (has_ticket && !ticket_only))) {
            if (w->kr) sb_append(sb, " onclick='claimReward()'");
            else if (ticket_only) sb_appendf(sb, " onclick='prizeWheel(null, %d)'", w->ticket);
            else sb_appendf(sb, " onclick='prizeWheel(%d)'", i);
        }
        sb_append(sb, ">");
        if (has_ticket && !ticket_only && acc) {
            sb_append(sb, "<div class='shopActionOvrl'>");
            if (w->multi) {
                char multi_value[32];
                snprintf(multi_value, sizeof(multi_value), "%d", w->price * w->multi);
                sb_appendf(sb, "<div class=\"shopActions\">\n\t\t\t\t\t\t\t<div class=\"shopSubAction\" onclick=\"playSelect();prizeWheel(%d)\" onmouseenter='playTick()'>\n\t\t\t\t\t\t\t\t1x Spin\n\t\t\t\t\t\t\t\t<div style=\"color:%s\">\n\t\t\t\t\t\t\t\t\t%s%s\n\t\t\t\t\t\t\t\t</div>\n\t\t\t\t\t\t\t</div>\n\t\t\t\t\t\t\t<div class=\"shopSubActionSep\" style=\"background:%s\"></div>\n\t\t\t\t\t\t\t<div class=\"shopSubAction\" onclick=\"playSelect();prizeWheel(%d, null, %d)\" onmouseenter='playTick()'>\n\t\t\t\t\t\t\t\t%dx Spin\n\t\t\t\t\t\t\t\t<div style=\"color:%s\">\n\t\t\t\t\t\t\t\t\t%s%s\n\t\t\t\t\t\t\t\t</div>\n\t\t\t\t\t\t\t</div>\n\t\t\t\t\t\t</div>",
                           i, col, value, kr_suffix, col, i, w->multi, w->multi, col, sold ? "NaN" : multi_value, kr_suffix);
            } else {
                sb_appendf(sb, "<div class=\"shopAction\" onclick=\"playSelect();prizeWheel(%d)\" onmouseenter='playTick()'>\n\t\t\t\t\t\t\tSpend\n\t\t\t\t\t\t\t<div style=\"color:%s\">\n\t\t\t\t\t\t\t\t%s%s\n\t\t\t\t\t\t\t</div>\n\t\t\t\t\t\t</div>", i, col, value, kr_suffix);
            }
            sb_appendf(sb, "<div style='display: table-cell;'><div class=\"shopActionSep\" style=\"background:%s\"></div></div>", col);
            sb_appendf(sb, "<div class=\"shopAction\" onclick=\"playSelect();prizeWheel(null, %d)\"\n\t\t\t\t\t\tonmouseenter='playTick()'>\n\t\t\t\t\t\tRedeem\n\t\t\t\t\t\t<div style=\"color:%s\">\n\t\t\t\t\t\t\t<img class=\"ticketImg\" src=\"%s\">\n\t\t\t\t\t\t</div>\n\t\t\t\t\t</div>", w->ticket, col, ticket);
            sb_appendf(sb, "<div class=\"ticketCount\" style=\"color:%s\">You have %d</div></div>", col, owned_count(w->ticket));
        }
        if (ticket_only && acc)
            sb_appendf(sb, "<div class=\"ticketCount\" style=\"width: unset;top:7px;right: 10px;color:%s\">You have %d</div>", col, owned_count(w->ticket));
        char spin_name[96];
        I18N_F(spin_name, "windows.store.spin", "{0} Spin", w->name);
        sb_appendf(sb, "<div class='spinValH'>%s<div class='spinValL' style='color:%s !important;width:314px'>", spin_name, col);
        if (ticket_only) sb_appendf(sb, "Redeem <img class=\"ticketImg\" src=\"%s\">", ticket);
        else {
            sb_appendf(sb, "%s%s", value, kr_suffix);
            if (has_ticket) sb_appendf(sb, "<div class=\"ticketImgSep\">|</div><img class=\"ticketImg\" src=\"%s\">", ticket);
        }
        sb_append(sb, "</div></div>");
        if (!w->kr) sb_appendf(sb, "<div class='shopImg' style='background-image: url(%s);left:60px'></div>", thumb);
        sb_appendf(sb, "<div class='shopImgGrad' style='background-image: linear-gradient(transparent, %s30);'></div></div>", col);
    }
    // View Tickets: the account's TICKET items (Game.store.wheels[3]'s ticket as the picture)
    int tickets = 0;
    tickets = owned_tickets();
    store_skin_preview(wheel_at(3) ? wheel_at(3)->ticket : -1, ticket, sizeof(ticket));
    sb_appendf(sb, "<div class='shopCard' onmouseenter='playTick()' style='color:#fc038d;border: 4px solid;text-shadow: 0px 0px 30px rgb(0 0 0);width:293px;height: 202px;background: rgba(0, 0, 0, 0.4);' onclick='windows[13].viewTickets()'><div class='spinValH'>%s<div class='spinValL' style='color:#fc038d!important;width:314px'>You have %d</div></div><div class='shopImg' style='background-image: url(%s);left:60px'></div><div class='shopImgGrad' style='background-image: linear-gradient(transparent, #fc038d30);'></div></div>",
               I18N("windows.store.wheels.viewTickets", "View Tickets"), tickets, ticket);
    sb_append(sb, "</div>");
}

static void store_premium(StrBuf *sb, const AccountInfo *acc) {
    sb_appendf(sb, "<div class='setBodH' style='padding-top:15px'><div style='color:rgba(255,255,255,0.5);margin-bottom:10px'>%s <a href='javascript:;' onclick='showWindow(36)'>%s</a></div></div><div id='storeList' style='margin-bottom:10px'>",
               I18N("windows.store.buyP", "Choose a Display name & more with"), I18N("windows.store.premium2", "Krunker Premium"));
    for (size_t i = 0; i < sizeof(k_premium) / sizeof(k_premium[0]); i++) {
        sb_appendf(sb, "<div class='shopCard' onmouseenter='playTick()' onclick='premiumPopup(%d)' style='color:%s;border: 4px solid;width:293px;height:212px;background-image:linear-gradient(transparent, %s30), url(&quot;img/prem_crd.png&quot;);'>%d Days<div class='spinVal' style='margin-top:119px;width: 309px;line-height: 63px;'>$%s <span style='color:#fff'>USD</span></div></div>",
                   (int) i, k_premium[i].col, k_premium[i].col, k_premium[i].days, k_premium[i].price);
    }
    char t[32];
    readable_time(t, sizeof(t), acc && acc->premium && acc->data ? cJSON_GetNumberValue(cJSON_GetArrayItem(acc->data, 27)) : 0);
    sb_appendf(sb, "</div><div id='premiumRes'></div><div class='setBodH' style='padding-top:10px;padding-bottom:10px;'><span style='color:#fff'>Time Remaining</span><span id='premTime'>%s</span></div>", t);
}

static void store_purchase(StrBuf *sb) {
    char cc[128] = "";
    html_storage_get("krk_ccKN", cc, sizeof(cc)); // the saved Creator Code (krk_cc {code, t} in v7)
    sb_append(sb, "<div id='purchaseInfoTable'><div class='purchKRInfoH'><div style='color:rgba(255,255,255,0.5);font-size:18px;margin-top:5px;margin-bottom:5px;'>Support your favorite Content Creator by using a Creator Code</div><div><input type='text' id='creatorCodeI' class='smlInput' style='width:280px' ");
    if (cc[0]) {
        sb_append(sb, "value='");
        sb_append_esc(sb, cc);
        sb_append(sb, "'");
    } else {
        sb_append(sb, "placeholder='Enter Creator Code'");
    }
    sb_appendf(sb, " %s/><div class='creatorCodeBtn' style='background-color:%s' onclick='creatorCodeAction(%d, 0)'>%s</div></div></div>",
               cc[0] ? "disabled" : "", cc[0] ? "#eb5656" : "#41c300", cc[0] ? 0 : 1, cc[0] ? "Remove" : "Save");
    char vignette[256];
    store_asset_url("img/shop/filter/vignette.png", vignette, sizeof(vignette));
    for (size_t i = 0; i < sizeof(k_purchases) / sizeof(k_purchases[0]); i++) {
        sb_appendf(sb, "<div class='shopCard' onmouseenter='playTick()' onclick='logBtnFRVR(`buy_kr_%d`);showPurchase(%d)' style='background-image:url(%s),url(&quot;img/shop/%d.png&quot;);border: 4px solid;color:%s'>",
                   k_purchases[i].val, (int) i, vignette, (int) i, k_purchases[i].col);
        if (k_purchases[i].tag) sb_appendf(sb, "<div class='shopSale' style='background-color:%s'>%s</div>", k_purchases[i].tag_col, k_purchases[i].tag);
        sb_appendf(sb, "%d <span style='color:#fff'>KR</span><div class='spinVal' style='color:#fff'>$%s USD</div></div>", k_purchases[i].val, k_purchases[i].price);
    }
    char agree[256];
    I18N_F(agree, "windows.store.agree", "By purchasing you agree to the {0}Terms & Conditions", "<a target='_blank' href='https://frvr.com/legal/krunker#TermsofService'>");
    sb_appendf(sb, "</div><div style='font-size:15px;text-align:center;margin-bottom:10px;margin-top:10px;color:#d4d4d4;'>%s</div><div><div style='color:#d4d4d4;font-size:16px;text-align:center;margin-bottom:10px;margin-top:10px;'>%s</a>.</div>",
               I18N("windows.store.currency", "* Prices in USD can be converted to your local currency at an exchange rate defined by the payment provider or bank"), agree);
}

static void store_junkyard(StrBuf *sb, const AccountInfo *acc) {
    char url[256];
    store_asset_url("img/backgrounds/junkyrd_0.png", url, sizeof(url));
    sb_appendf(sb, "<div id='crftHeaderImg' style='background-image:linear-gradient(rgb(10 10 10 / 37%%),#0000003b),url(%s);'><div id='crftHeaderName'>Welcome to the Junkyard</div><div id='crftHeaderDesc'>Craft brand new skins &amp; items by using Junk and other Materials, which can be found scattered around every map in the game or earned from challenges.</div></div><div style='margin-top:0px;'>", url);
    // crafting is out of scope (doc/v7/decisions.md: the store economy): no recipes to load
    sb_append(sb, acc ? "<div style='color:#fff;text-align:center;margin-top:20px;'>No Recipes Found</div>"
                      : "<div style='color:#fff;text-align:center;margin-top:20px;cursor: pointer;' onclick='showWindow(5)'><span style='color:#31caec'>Register</span> to craft! </div>");
    sb_append(sb, "</div>");
}

// ---------------------------------------------------------------- the v10 page's Store
// v10.js windows[13].gen() mounts the Svelte store _44131 (store-window-mount): the store-tabs row
// (config.shopTabs: home, Prize Wheels, Premium, Purchase KR, Redeem Keys, Eterno Shop) over the
// tab's component: home _44082 (the Battle Pass / 600 KR / Junkyard panels rotating every 3 s, the
// Heroic spin card = wheels[2], the bundle slider), Prize Wheels (svelte-5s5opu: wheels 0 and 2
// plus the dated ones, each tile's hover overlay: 1x / Nx spin, Redeem with the wheel's ticket),
// Premium (svelte-1dbkhl), Purchase KR (svelte-1ko99up) and the Eterno Shop (svelte-gh77t9, its
// own music). The markup is the components' rendered DOM; the handlers are this file's.

// v10 config.purchases (_7299): KR packages
static const struct { int val; const char *price, *col, *tag, *tag_col; } k_purchases_v10[] = {
    {400, "1.29", "#888888", NULL, NULL}, {1250, "2.89", "#b2f251", NULL, NULL}, {3500, "9.99", "#3696f3", NULL, NULL},
    {8500, "18.99", "#e040fb", NULL, NULL}, {20000, "34.99", "#fa9704", "Popular!", "#ed4242"},
    {60000, "99.99", "#ee5356", "Best Value!", "#E040FB"},
};

static const char *const k_shop_tabs_v10[] = {
    "<span class=\"material-icons\" style=\"color:#fbc02d;font-size:42px;vertical-align:bottom\">home</span>", "Prize Wheels", "Premium",
    "Purchase <span style=\"color:#fbc02d\">KR</span>", "Redeem Keys", "Eterno Shop",
};
#define V10_SHOP_TABS ((int) (sizeof(k_shop_tabs_v10) / sizeof(k_shop_tabs_v10[0])))

// the tab body ("windows.store.wheels.sub" & co.) is written by these, after the tab row
static void v10_store_home(StrBuf *sb, const AccountInfo *acc) {
    static const struct { const char *nav, *sub, *sub_style, *head, *bg, *bg_style, *click; } PANELS[3] = {
        {"Unlock new Items!", "Checkout the", "color: #dd90f9 !important;", "Battle Pass", "img/battlepass/bg_window_5.png", "background-size: 100%;", NULL},
        {"Unlock new skins!", "Purchase", "color: var(--yellow) !important;", "600 KR", "img/prem_bg.png", "background-size: 120%;", "windows[13].directPurchase(1)"},
        {"Craft new Items!", "Enter the", NULL, "Junkyard", "img/backgrounds/junkyrd_0.png", "background-size: cover; background-position-x: -600px;", "windows[13].craftMenu()"},
    };
    sb_append(sb, "<div class=\"home-tab svelte-lri212\">");
    for (int i = 0; i < 3; i++) {
        char bg[256];
        store_asset_url(PANELS[i].bg, bg, sizeof(bg));
        const char *grad = i == 2 ? "linear-gradient(rgb(10 10 10 / 37%), #0000003b)" : "linear-gradient(rgb(0 0 0 / 38%), #00000000, rgb(10 10 10 / 41%))";
        sb_appendf(sb, "<div class=\"sub-tab-panel svelte-lri212\" role=\"button\" tabindex=\"0\" onclick=\"playSelect();%s\" style=\"display: %s; background: %s, url(%s); %s\"><div class=\"sub-tab-dots svelte-lri212\">",
                   i == 0 ? (acc ? "showWindow(6)" : "showWindow(5)") : PANELS[i].click, g_store_sub == i ? "" : "none", grad, bg, PANELS[i].bg_style);
        for (int d = 0; d < 3; d++)
            sb_appendf(sb, "<div class=\"sub-tab-dot svelte-lri212\" role=\"button\" tabindex=\"0\" onclick=\"event.stopPropagation();windows[13].subTabT=5000;windows[13].subTab=%d;updateWindow(14);\" style=\"background: %s\"></div>",
                       d, d == g_store_sub ? "#fff" : "rgba(255,255,255,0.4)");
        sb_appendf(sb, "</div> <div class=\"sub-tab-nav svelte-lri212\">%s</div> ", PANELS[i].nav);
        if (i == 2) // acc.junk: KrunkNative has no crafting (doc/v7/decisions.md), so no Junk either
            sb_append(sb, "<div class=\"junkyard-display svelte-lri212\">0.00 <span class=\"material-icons svelte-lri212\" style=\"color: #cc51f8; font-size: 38px; margin-left: 8px; vertical-align: top; margin-right: -9px;\">plumbing</span></div> ");
        sb_appendf(sb, "<div class=\"sub-tab-header svelte-lri212\"><div class=\"sub-tab-subtitle svelte-lri212\"%s%s%s>%s</div> %s</div></div> ",
                   PANELS[i].sub_style ? " style=\"" : "", PANELS[i].sub_style ? PANELS[i].sub_style : "", PANELS[i].sub_style ? "\"" : "", PANELS[i].sub, PANELS[i].head);
    }
    // the Heroic spin card (wheels[2])
    const UiWheel *hw = wheel_at(2);
    if (!hw) return;
    const char *col = hw->head_col;
    char thumb[256], ticket[256];
    wheel_thumb(hw, thumb, sizeof(thumb));
    store_skin_preview(hw->ticket, ticket, sizeof(ticket));
    sb_appendf(sb, "<div class=\"heroic-spin-card svelte-lri212%s\" role=\"button\" tabindex=\"0\" style=\"color: %s;\"%s onmouseenter=\"playTick()\">",
               acc ? "" : " no-account", col, acc ? "" : " onclick=\"showWindow(5)\"");
    if (acc) {
        sb_append(sb, "<div class=\"shop-action-overlay svelte-lri212\">");
        if (hw->multi)
            sb_appendf(sb, "<div class=\"shop-actions-cell svelte-lri212\"><div class=\"shop-sub-action svelte-lri212\" role=\"button\" tabindex=\"0\" style=\"font-size: 34px !important;\" onclick=\"playSelect();prizeWheel(2)\" onmouseenter=\"playTick()\">1x Spin <div class=\"svelte-lri212\" style=\"color: %s\">%d KR</div></div> <div class=\"shop-sub-action-sep svelte-lri212\" style=\"background: %s\"></div> <div class=\"shop-sub-action svelte-lri212\" role=\"button\" tabindex=\"0\" style=\"font-size: 34px !important;\" onclick=\"playSelect();prizeWheel(2, null, %d)\" onmouseenter=\"playTick()\">%dx Spin <div class=\"svelte-lri212\" style=\"color: %s\">%d KR</div></div></div>",
                       col, hw->price, col, hw->multi, hw->multi, col, hw->price * hw->multi);
        else
            sb_appendf(sb, "<div class=\"shop-action-cell svelte-lri212\" role=\"button\" tabindex=\"0\" onclick=\"playSelect();prizeWheel(2)\">Spend <div class=\"svelte-lri212\" style=\"color: %s\">%d KR</div></div>", col, hw->price);
        sb_appendf(sb, " <div style=\"display: table-cell;\" class=\"svelte-lri212\"><div class=\"shop-action-sep svelte-lri212\" style=\"background: %s\"></div></div> <div class=\"shop-action-cell svelte-lri212\" role=\"button\" tabindex=\"0\" style=\"font-size: 34px !important;\" onclick=\"playSelect();prizeWheel(null, %d)\" onmouseenter=\"playTick()\">Redeem <div class=\"svelte-lri212\" style=\"color: %s\"><img class=\"ticket-img svelte-lri212\" alt=\"ticket\" style=\"width: 70px !important;\" src=\"%s\"></div></div> <div class=\"ticket-count svelte-lri212\" style=\"font-size: 22px !important; color: %s\">You have %d</div></div>",
                   col, hw->ticket, col, ticket, col, owned_count(hw->ticket));
    }
    char spin_name[96];
    I18N_F(spin_name, "windows.store.spin", "{0} Spin", hw->name);
    sb_appendf(sb, " <div class=\"heroic-spin-name svelte-lri212\">%s <div class=\"heroic-spin-price svelte-lri212\" style=\"color: %s !important;\">%d KR <div class=\"ticket-img-sep svelte-lri212\">|</div> <img class=\"ticket-img svelte-lri212\" alt=\"ticket\" style=\"width: 56px !important;\" src=\"%s\"></div></div> <div class=\"heroic-spin-img svelte-lri212\" style=\"background-image: url(%s);\"></div> <div class=\"heroic-img-grad svelte-lri212\" style=\"margin-left: 0 !important; background-image: linear-gradient(transparent, %s30);\"></div></div> ",
               spin_name, col, hw->price, ticket, thumb, col);
    // bundles (bundleData): none without the store API
    sb_append(sb, "<div class=\"bundle-holder svelte-lri212\"><div class=\"bundleSliderH svelte-lri212\"><div class=\"bundleSliderBtn disabled svelte-lri212\" role=\"button\" tabindex=\"0\" onmouseenter=\"playTick(0.1)\"><div class=\"bundleSliderArrow svelte-lri212\"></div></div></div> <div class=\"bundleCols svelte-lri212\"></div> <div class=\"bundleSliderH svelte-lri212\"><div class=\"bundleSliderBtn svelte-lri212\" role=\"button\" tabindex=\"0\" onmouseenter=\"playTick(0.1)\"><div class=\"bundleSliderArrow right svelte-lri212\"></div></div></div></div></div>");
}

// Prize Wheels: wheels 0 (Starter) and 2 (Heroic), not hidden, not locked (store.js data; the
// dated wheels of v10's gapi /data/spins are hidden or over)
static void v10_store_wheels(StrBuf *sb, const AccountInfo *acc) {
    int tickets = 0;
    tickets = owned_tickets();
    sb_appendf(sb, "<div class=\"prize-wheels-tab svelte-5s5opu\"><div class=\"prize-wheels-header svelte-5s5opu\"><div class=\"prize-wheels-sub svelte-5s5opu\">%s</div> <div class=\"view-tickets-btn svelte-5s5opu\"><button class=\"main-button svelte-whnh40 main-button-slim\" type=\"button\" onclick=\"playSelect();windows[13].viewTickets()\">%s<span class=\"ticket-count-sep svelte-5s5opu\"></span><span class=\"ticket-count-num svelte-5s5opu\">%d</span></button></div></div> <div class=\"prize-wheel-table svelte-5s5opu\">",
               I18N("windows.store.wheels.sub", "Try your luck and spin for a chance to win some new skins!"), I18N("windows.store.wheels.viewTickets", "View Tickets"), tickets);
    // v10: wheels 0 and 2, and the dated ones while they run (not hidden / locked; the Free KR
    // wheel only off mobile, which it is hidden on anyway)
    for (int i = 0; i < wheel_count(); i++) {
        const UiWheel *w = wheel_at(i);
        const int dated = w->src && (w->src->start_ms || w->src->end_ms);
        if (!(i == 0 || i == 2 || (dated && !w->hide && !w->locked))) continue;
        const char *col = w->head_col;
        int locked = !acc || (w->free && acc->level < w->min_lvl);
        int sho_kr = 1;
        char price[32];
        if (w->kr) {
            const double ms = account_reward_ms();
            const long long t = ms > 0 ? (long long) ms / 1000 : 0;
            if (t > 0) snprintf(price, sizeof(price), "%02lld:%02lld:%02lld", t / 3600, (t / 60) % 60, t % 60);
            else snprintf(price, sizeof(price), "%s", w->price_t);
            locked = t > 0;
            sho_kr = 0;
        } else if (locked) {
            snprintf(price, sizeof(price), "Sold Out");
        } else if (w->price_t) {
            snprintf(price, sizeof(price), "%s", w->price_t);
        } else {
            snprintf(price, sizeof(price), "%d", w->price);
        }
        char display[48];
        snprintf(display, sizeof(display), "%s%s", price, w->price_t || !sho_kr ? "" : " KR");
        const int has_ticket = w->ticket >= 0, ticket_only = w->ticket_only;
        char thumb[256] = "", ticket[256] = "";
        if (!w->kr && acc) wheel_thumb(w, thumb, sizeof(thumb));
        if (has_ticket) store_skin_preview(w->ticket, ticket, sizeof(ticket));
        sb_appendf(sb, "<div class=\"shopCard prize-tile svelte-5s5opu%s\" role=\"button\" tabindex=\"0\" style=\"color: %s; --tile-glow: %s47; --tile-shadow: %s73; ", locked ? " makeGray" : "", col, col, col);
        if (w->kr) {
            char vig[256], spin[64], spin_url[256];
            store_asset_url("img/shop/filter/vignette.png", vig, sizeof(vig));
            snprintf(spin, sizeof(spin), "img/shop/spins/%d_0.png", i);
            store_asset_url(spin, spin_url, sizeof(spin_url));
            sb_appendf(sb, "background-image: url(%s), url(%s)", vig, spin_url);
        }
        sb_append(sb, "\" onmouseenter=\"playTick()\"");
        if (!locked && !(has_ticket && !ticket_only)) {
            if (w->kr) sb_append(sb, " onclick=\"claimReward()\"");
            else if (ticket_only) sb_appendf(sb, " onclick=\"prizeWheel(%d, %d)\"", i, w->ticket);
        }
        sb_append(sb, ">");
        if ((has_ticket || w->multi) && !ticket_only && acc) {
            sb_append(sb, "<div class=\"shop-action-overlay svelte-5s5opu\">");
            if (w->multi) {
                char multi[48];
                if (locked) snprintf(multi, sizeof(multi), "NaN%s", sho_kr && !w->price_t ? " KR" : "");
                else snprintf(multi, sizeof(multi), "%d%s", w->price * w->multi, sho_kr && !w->price_t ? " KR" : "");
                sb_appendf(sb, "<div class=\"shop-actions-cell svelte-5s5opu\"><div class=\"shop-sub-action svelte-5s5opu\" role=\"button\" tabindex=\"0\" onclick=\"playSelect();prizeWheel(%d)\" onmouseenter=\"playTick()\">1x Spin <div style=\"color: %s\">%s</div></div> <div class=\"shop-sub-action-sep svelte-5s5opu\" style=\"background: %s\"></div> <div class=\"shop-sub-action svelte-5s5opu\" role=\"button\" tabindex=\"0\" onclick=\"playSelect();prizeWheel(%d, null, %d)\" onmouseenter=\"playTick()\">%dx Spin <div style=\"color: %s\">%s</div></div></div>",
                           i, col, display, col, i, w->multi, w->multi, col, multi);
            } else {
                sb_appendf(sb, "<div class=\"shop-action-cell svelte-5s5opu\" role=\"button\" tabindex=\"0\" onclick=\"playSelect();prizeWheel(%d)\" onmouseenter=\"playTick()\">Spend <div style=\"color: %s\">%s</div></div>", i, col, display);
            }
            if (has_ticket)
                sb_appendf(sb, " <div style=\"display: table-cell;\"><div class=\"shop-action-sep svelte-5s5opu\" style=\"background: %s\"></div></div> <div class=\"shop-action-cell svelte-5s5opu\" role=\"button\" tabindex=\"0\" onclick=\"playSelect();prizeWheel(%d, %d)\" onmouseenter=\"playTick()\">Redeem <div style=\"color: %s\"><img class=\"ticket-img svelte-5s5opu\" alt=\"ticket\" src=\"%s\"></div></div> <div class=\"ticket-count svelte-5s5opu\" style=\"color: %s\">You have %d</div>",
                           col, i, w->ticket, col, ticket, col, owned_count(w->ticket));
            sb_append(sb, "</div>");
        }
        if (ticket_only && acc) sb_appendf(sb, "<div class=\"ticket-count ticket-count-corner svelte-5s5opu\" style=\"color: %s\">You have %d</div>", col, owned_count(w->ticket));
        char spin_name[96];
        I18N_F(spin_name, "windows.store.spin", "{0} Spin", w->name);
        sb_appendf(sb, " <div class=\"spin-name svelte-5s5opu\">%s <div class=\"spin-price svelte-5s5opu\" style=\"color: %s !important;\">", spin_name, col);
        if (ticket_only) sb_appendf(sb, "Redeem <img class=\"ticket-img svelte-5s5opu\" alt=\"ticket\" src=\"%s\">", ticket);
        else {
            sb_appendf(sb, "%s ", display);
            if (has_ticket) sb_appendf(sb, "<div class=\"ticket-img-sep svelte-5s5opu\">|</div> <img class=\"ticket-img svelte-5s5opu\" alt=\"ticket\" src=\"%s\">", ticket);
        }
        sb_append(sb, "</div></div> ");
        if (!w->kr) sb_appendf(sb, "<div class=\"shop-img svelte-5s5opu\" style=\"background-image: url(%s);\"></div>", thumb);
        sb_appendf(sb, " <div class=\"shop-img-grad svelte-5s5opu\" style=\"background-image: linear-gradient(transparent, %s30);\"></div> </div>", col);
    }
    sb_append(sb, "</div></div>");
}

// the price / terms lines under the Premium and Purchase KR tabs (and the tab's closing </div>)
static void store_legal(StrBuf *sb, const char *svelte) {
    char agree[256];
    I18N_F(agree, "windows.store.agree", "By purchasing you agree to the {0}Terms & Conditions", "<a target=\"_blank\" href=\"https://frvr.com/legal/krunker#TermsofService\">");
    sb_appendf(sb, "<div class=\"legal-text svelte-%s\">%s</div> <div class=\"terms-text svelte-%s\">%s</a>.</div></div>", svelte,
               I18N("windows.store.currency", "* Prices in USD can be converted to your local currency at an exchange rate defined by the payment provider or bank"), svelte, agree);
}

static void v10_store_premium(StrBuf *sb, const AccountInfo *acc) {
    sb_appendf(sb, "<div class=\"premium-tab svelte-1dbkhl\"><div class=\"premium-header svelte-1dbkhl\"><div style=\"color: rgba(255,255,255,0.5); margin-bottom: 10px;\">%s <a href=\"javascript:;\" onclick=\"showPremiumBenefits()\">%s</a></div></div> <div class=\"store-list svelte-1dbkhl\">",
               I18N("windows.store.buyP", "Choose a Display name & more with"), I18N("windows.store.premium2", "Krunker Premium"));
    char crd[256];
    store_asset_url("img/prem_crd.png", crd, sizeof(crd));
    for (size_t i = 0; i < sizeof(k_premium) / sizeof(k_premium[0]); i++)
        sb_appendf(sb, "<div class=\"shopCard\" role=\"button\" tabindex=\"0\" onmouseenter=\"playTick()\" onclick=\"premiumPopup(%d)\" style=\"color: %s; border: 4px solid; width: 293px; height: 212px; background-image: linear-gradient(transparent, %s30), url(%s);\">%d Days <div class=\"spinVal\" style=\"margin-top: 119px; width: 309px; line-height: 63px;\">$%s <span style=\"color: #fff\">USD</span></div></div>",
                   (int) i, k_premium[i].col, k_premium[i].col, crd, k_premium[i].days, k_premium[i].price);
    sb_append(sb, "</div> <div class=\"premium-res svelte-1dbkhl\"></div> ");
    if (acc && acc->premium > 0) {
        char t[32];
        readable_time(t, sizeof(t), acc->data ? cJSON_GetNumberValue(cJSON_GetArrayItem(acc->data, 27)) : 0);
        sb_appendf(sb, "<div class=\"premium-time-section svelte-1dbkhl\"><span style=\"color: #fff\">Time Remaining</span> <span class=\"premium-time svelte-1dbkhl\">%s</span></div>", t);
    }
    sb_append(sb, " ");
    store_legal(sb, "1dbkhl");
}

static void v10_store_kr(StrBuf *sb) {
    char vig[256];
    store_asset_url("img/shop/filter/vignette.png", vig, sizeof(vig));
    sb_append(sb, "<div class=\"purchase-kr-tab svelte-1ko99up\"><div class=\"purchase-info-table svelte-1ko99up\">");
    for (size_t i = 0; i < sizeof(k_purchases_v10) / sizeof(k_purchases_v10[0]); i++) {
        char img[48], url[256];
        snprintf(img, sizeof(img), "img/shop/%d.png", (int) i);
        store_asset_url(img, url, sizeof(url));
        sb_appendf(sb, "<div class=\"shopCard\" role=\"button\" tabindex=\"0\" onmouseenter=\"playTick()\" onclick=\"showPurchase(%d)\" style=\"background-image: url(%s), url(%s); border: 4px solid; color: %s\">",
                   (int) i, vig, url, k_purchases_v10[i].col);
        if (k_purchases_v10[i].tag) sb_appendf(sb, "<div class=\"shopSale\" style=\"background: %s\">%s</div>", k_purchases_v10[i].tag_col, k_purchases_v10[i].tag);
        sb_appendf(sb, " %d <span style=\"color: #fff\">KR</span> <div class=\"spinVal\" style=\"color: #fff\">$%s USD</div></div>", k_purchases_v10[i].val, k_purchases_v10[i].price);
    }
    sb_append(sb, "</div> ");
    store_legal(sb, "1ko99up");
}

// the Eterno Shop (svelte-gh77t9): its offers come from the shop API, which KrunkNative has not:
// the header and the component's empty state
static void v10_store_eterno(StrBuf *sb) {
    char vid[256];
    store_asset_url("videos/limited_shop_banner.webm", vid, sizeof(vid));
    sb_appendf(sb, "<div id=\"shop-header\" class=\"svelte-gh77t9\"><video autoplay=\"\" loop=\"\" playsinline=\"\" class=\"shop-background-video svelte-gh77t9\"><source src=\"%s\" type=\"video/webm\" class=\"svelte-gh77t9\"></video> <div id=\"shop-header-name\" class=\"svelte-gh77t9\">Eterno Shop</div> <div id=\"shop-header-desc\" class=\"svelte-gh77t9\">Limited-time offers—grab them before they're gone! Check back often so you don't miss out on rare deals.</div></div> <div class=\"error-container svelte-gh77t9\"><div class=\"error-text svelte-gh77t9\">No offers right now</div> <button class=\"retry-button svelte-gh77t9\" onclick=\"updateWindow(14)\">Retry</button></div>",
               vid);
}

static char *v10_store_window_gen(void) {
    const AccountInfo *acc = account_get();
    if (g_store_tab < 0 || g_store_tab >= V10_SHOP_TABS || g_store_tab == 4) g_store_tab = 0;
    StrBuf sb = {0};
    sb_append(&sb, "<div id=\"store-window-mount\"><div class=\"store-window svelte-v85byk\"><div class=\"store-header svelte-v85byk\"><div style=\"height: 15px;\"></div> <div class=\"store-tabs svelte-v85byk\" role=\"tablist\">");
    for (int i = 0; i < V10_SHOP_TABS; i++)
        sb_appendf(&sb, "<div class=\"store-tab svelte-v85byk%s\" role=\"tab\" tabindex=\"0\" style=\"font-size: 16px\" aria-selected=\"%s\" onclick=\"playSelect();windows[13].switchTab(%d)\" onmouseenter=\"SOUND.play('tick_0',0.1)\">%s</div>",
                   g_store_tab == i ? " store-tab-active" : "", g_store_tab == i ? "true" : "false", i, k_shop_tabs_v10[i]);
    sb_append(&sb, "</div></div> ");
    switch (g_store_tab) {
        case 0: v10_store_home(&sb, acc); break;
        case 1: v10_store_wheels(&sb, acc); break;
        case 2: v10_store_premium(&sb, acc); break;
        case 3: v10_store_kr(&sb); break;
        case 5: v10_store_eterno(&sb); break;
        default: break;
    }
    sb_append(&sb, "</div></div>");
    return sb_finish(&sb);
}

// windows[13].eterno / musicPlayer: #menuWindow.eterno on the Eterno tab and its music, "Eterno
// Rules the World" (eterno_lofi, looped) with the player (svelte-1ei19qi) in #menuWindowMusic;
// any other window or tab stops it (showWindow: musicPlayer)
static int g_eterno_music;

static void eterno_music(const int on) {
    HtmlElem holder = html_dom_by_id("menuWindowMusic");
    if (on && !g_eterno_music) audio_play("eterno_lofi", 0.1f, 1, 1.0f);
    if (!on && g_eterno_music) audio_stop("eterno_lofi");
    g_eterno_music = on;
    if (holder)
        html_dom_set_html(holder, on ? "<div class=\"music-player svelte-1ei19qi\"><span class=\"music-title svelte-1ei19qi\">Eterno Rules the World</span> <button class=\"music-btn svelte-1ei19qi\" title=\"Stop\" aria-label=\"Stop\" onclick=\"knStoreMusicStop()\"><svg width=\"20\" height=\"20\" viewBox=\"0 0 20 20\" fill=\"none\" aria-hidden=\"true\" xmlns=\"http://www.w3.org/2000/svg\" class=\"svelte-1ei19qi\"><rect x=\"4\" y=\"4\" width=\"12\" height=\"12\" rx=\"2\" fill=\"currentColor\" class=\"svelte-1ei19qi\"></rect></svg></button></div>" : "");
}

void store_ui_window_post(const int id) {
    eterno_music(id == 14 && g_store_tab == 5);
    if (id != 14) return;
    HtmlElem mw = html_dom_by_id("menuWindow");
    if (mw) html_dom_set_class(mw, "eterno", g_store_tab == 5);
}

// windows[13].gen()
char *store_window_gen(void) {
    return v10_store_window_gen();
}

// genericPopup(html, cls): #genericPop over #popupBack
static void generic_popup(const char *html, const char *cls) {
    ui_loadout_clear_pops();
    set_display("popupHolder", "block");
    HtmlElem pop = html_dom_by_id("genericPop");
    if (pop) {
        html_dom_set_attr(pop, "class", cls);
        html_dom_set_attr(pop, "style", !strcmp(cls, "contPop") ? "width:513px;border:;" : ""); // contPopup(html, "513px")
    }
    set_html("genericPop", html);
    set_display("genericPop", "block");
}

// windows[13] / store handlers; guests included (the -1 guard below is for the wheels)
static int store_window_handle(const char *name, const HtmlArg *args, const int argc) {
    const int a0 = argc > 0 && (args[0].kind == HTML_ARG_NUM || args[0].kind == HTML_ARG_BOOL) ? (int) args[0].num : 0;
    if (!strcmp(name, "windows.13.switchTab")) { // 5 Marketplace -> the hub, 4 Redeem -> voucherPopup()
        if (a0 != 4) { // v10: 5 is the Eterno Shop tab, the Marketplace is openExchange()
            g_store_tab = a0 >= 0 && a0 < V10_SHOP_TABS ? a0 : 0;
            menu_rerender_if_open(14);
        } else {
            store_window_handle("voucherPopup", NULL, 0);
        }
        return 1;
    }
    if (!strcmp(name, "windows.13.premium")) { g_store_tab = 2; menu_show_window(14, 1); return 1; }
    if (!strcmp(name, "windows.13.purchase")) { // purchase(1): Purchase KR; purchase(): out of funds popup
        if (argc > 0 && !a0) {
            char pop[512];
            snprintf(pop, sizeof(pop), "<div style='color:#fff;font-size:20px;position:relative;margin-bottom:15px;'>%s</div><div id='confirmBtn' style='width: calc(100%% - 20px)' onmouseover='playTick(0.1)' onclick='windows[13].purchase(1)'>Purchase More</div>",
                     I18N("funds.out", "It appears you have run out of Krunkies."));
            generic_popup(pop, "contPop");
            return 1;
        }
        ui_loadout_clear_pops();
        g_store_tab = 3;
        menu_show_window(14, 1);
        return 1;
    }
    if (!strcmp(name, "windows.13.subTab=")) { g_store_sub = a0 >= 0 && a0 < 3 ? a0 : 0; return 1; }
    if (!strcmp(name, "windows.13.subTabT=")) { g_store_sub_t = a0; return 1; }
    if (!strcmp(name, "windows.13.craftMenu")) {
        const HtmlArg tab = {.kind = HTML_ARG_NUM, .num = 3}; // v10: window.openExchange(3), the exchange's Junkyard
        html_ui_dispatch("openExchange", NULL, &tab, 1);
        return 1;
    }
    if (!strcmp(name, "windows.13.directPurchase") || !strcmp(name, "showPremiumBenefits")) { // payments: none
        if (!account_get()) menu_show_window(5, 1);
        else if (!strcmp(name, "showPremiumBenefits")) menu_show_window(36, 1);
        else ui_alert(I18N("kn.store.noPurchases", "Purchases are not available in Krunker Box Office."));
        return 1;
    }
    if (!strcmp(name, "windows.13.viewTickets")) { // the account's TICKET items, each redeems its spinWheel
        StrBuf sb = {0};
        int n = 0;
        sb_append(&sb, "<div><div class=\"popHeader\" style=\"margin-bottom: 30px;\">Spin Tickets</div>");
        const AccountInfo *acc = account_get();
        const cJSON *e;
        cJSON_ArrayForEach(e, (acc && acc->data ? cJSON_GetArrayItem(acc->data, 10) : NULL)) { // acc.skins of type TICKET
            const int skin = (int) cJSON_GetNumberValue(cJSON_GetObjectItem(e, "ind")), cnt = owned_count(skin);
            const ViewerSkin *k = viewer_skin(skin);
            if (!cnt || !k || k->type != 13 || (wheel_of_ticket(skin) < 0 && !ticket_wheel(skin))) continue;
            char thumb[256];
            store_skin_preview(skin, thumb, sizeof(thumb));
            const char *col = k->rarity >= 0 && k->rarity < g_viewer_rarity_count ? g_viewer_rarities[k->rarity].color : "#fff";
            sb_appendf(&sb, "<div class=\"ticketCard\" onmouseenter=\"playTick()\" onclick=\"playSelect();prizeWheel(%d, %d);cancelTip()\"\n\t\t\t\tdata-index='%d' onmouseover='createTip(this)' onmouseout='cancelTip()'\n\t\t\t\tstyle=\"background-image:url(%s);border-color:%s!important\">\n\t\t\t\t<div class=\"ticketCount\" style=\"width: unset;top:2px;right: 2px;color:%s\">x%d</div>\n\t\t\t\t<div class=\"ticketText\" style=\"color:%s!important\">Redeem</div>\n\t\t\t</div>",
                       wheel_of_ticket(skin), skin, skin, thumb, col, col, cnt, col);
            n++;
        }
        sb_append(&sb, "</div>");
        char *html = sb_finish(&sb);
        if (n) generic_popup(html, "ticketPop");
        else account_popup(0, "You don't have any Spin Tickets!", NULL); // io_showPopup(null, ...)
        free(html);
        return 1;
    }
    if (!strcmp(name, "windows.13.updateScroll") || !strcmp(name, "windows.13.updateScrollWheel")) return 1;
    if (!strcmp(name, "voucherPopup")) {
        if (!account_get()) {
            menu_show_window(5, 1);
            return 1;
        }
        char pop[512];
        snprintf(pop, sizeof(pop), "<div style='color:rgba(255,255,255,0.7);margin-top:-5px;font-size:19px;text-align:center;margin-bottom:5px;'>%s</div><div><input type='text' id='redVouch' class='smlInput' placeholder='Enter KR Code' /><div class='redVouchBtn' onclick='redeemVoucher()'>Redeem</div></div><div id='vouchRes' style='display:none'></div>",
                 I18N("windows.store.voucherinfo", "Redeem Vouchers from certain Streams & Game Events"));
        generic_popup(pop, "vouchPop");
        return 1;
    }
    if (!strcmp(name, "showPurchase") || !strcmp(name, "premiumPopup")) { // no payments in KrunkNative
        if (!account_get()) menu_show_window(5, 1);
        else ui_alert(I18N("kn.store.noPurchases", "Purchases are not available in Krunker Box Office."));
        return 1;
    }
    if (!strcmp(name, "creatorCodeAction")) {
        char code[128] = "";
        HtmlElem in = html_dom_by_id("creatorCodeI");
        if (in) html_dom_get_value(in, code, sizeof(code));
        html_storage_set("krk_ccKN", a0 ? code : "");
        menu_rerender_if_open(14);
        return 1;
    }
    return -1;
}

// the home tab's banner rotation (updSubTab): every 3 s while the Store shows tab 0
void store_ui_sub_tab_tick(const double delta_ms) {
    if (g_store_tab != 0 || !menu_window_is_open() || menu_ui()->active_window != 14) return;
    if ((g_store_sub_t -= delta_ms) > 0) return;
    g_store_sub_t = 3000.0;
    g_store_sub = (g_store_sub + 1) % 3;
    HtmlElem tabs[8];
    // the home tab's setInterval: the panel shows, its dots follow
    const int n = html_dom_query_all(0, "#menuWindow .sub-tab-panel", tabs, 8);
    for (int i = 0; i < n; i++) {
        html_dom_set_display(tabs[i], i == g_store_sub ? "" : "none");
        HtmlElem dots[3];
        const int nd = html_dom_query_all(tabs[i], ".sub-tab-dot", dots, 3);
        for (int d = 0; d < nd; d++) html_dom_set_style(dots[d], "background", d == g_store_sub ? "#fff" : "rgba(255,255,255,0.4)");
    }
}

int store_ui_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) ev;
    if (!name) return -1;
    const int r = store_window_handle(name, args, argc);
    if (r >= 0) return r;
    if (!backend_enabled() || !account_get()) return -1;
    if (!strcmp(name, "knStoreMusicStop")) { // the music player's Stop
        eterno_music(0);
        return 1;
    }
    if (!strcmp(name, "toggleSpinMulti")) { // v10: _72136 (spinning) || prizeWheel(index, ticket, !multi)
        html_ui_stop_propagation();
        if (!g_spinning && g_cur) prize_wheel(g_spin_index, g_spin_ticket, !g_multi, 0, 0);
        return 1;
    }
    if (!strcmp(name, "prizeWheel")) { // prizeWheel(index|null, skin|null, multi, autoSpin, noAnim)
        const int index = argc > 0 && args[0].kind == HTML_ARG_NUM ? (int) args[0].num : -1;
        const int skin = argc > 1 && args[1].kind == HTML_ARG_NUM ? (int) args[1].num : -1;
        const int multi = argc > 2 && (args[2].kind == HTML_ARG_NUM || args[2].kind == HTML_ARG_BOOL) && args[2].num != 0;
        const int auto_spin = argc > 3 && args[3].kind == HTML_ARG_BOOL && args[3].num != 0;
        const int no_anim = argc > 4 && args[4].kind == HTML_ARG_BOOL && args[4].num != 0;
        prize_wheel(index, skin, multi, auto_spin, no_anim);
        return 1;
    }
    if (!strcmp(name, "openSales")) { // openURL("/social.html?p=itemsales&i=" + i)
        if (argc > 0 && args[0].kind == HTML_ARG_NUM) open_item_sales((int) args[0].num);
        return 1;
    }
    if (!strcmp(name, "knSpinButton")) {
        spin_button_click();
        return 0;
    }
    if (!strcmp(name, "knSpinUI")) { // spinUI.onclick: closes the wheel at any time
        if (glfwGetTime() - g_spin_button_time < 0.05) return 1; // a button's click bubbling up
        menu_show_window(14, 0);
        toggle_menu_hider(1);
        spin_wheel_confetti_reset();
        backend_social_send_text("[\"unbx\"]");
        spin_kr();
        return 1;
    }
    if (!strcmp(name, "knSpinRespin")) { // spinAgainBtn: prizeWheel(i, skin, multi, true, true)
        g_spin_button_time = glfwGetTime();
        if (g_can_respin) prize_wheel(g_spin_index, g_spin_ticket, g_multi, 1, 1);
        return 0;
    }
    if (!strcmp(name, "knSpinNext")) { // spinNextBtn: f1847(true)
        g_spin_button_time = glfwGetTime();
        if (g_wheel.next_item < g_wheel.item_count) spin_finished(1);
        return 0;
    }
    if (!strcmp(name, "knSpinEquip")) { // spinEquipBtn: select the won item in its slot
        g_spin_button_time = glfwGetTime();
        const ViewerSkin *skin = g_can_equip && g_wheel.win_skin >= 0 ? viewer_skin(g_wheel.win_skin) : NULL;
        if (!skin) return 0;
        // spinEquipBtn's slot table (v7.1.4: by item type)
        static const char *const fns[] = {NULL, "selectHat", "selectBody", "selectMelee", "selectSpray", "selectDye", "selectWaist", "selectFace",
                                          "selectShoe", "selectPet", NULL, "selectWrist", NULL, NULL, "selectBack", "selectHead"};
        HtmlArg a[2] = {{HTML_ARG_NUM, g_wheel.win_skin, NULL}, {HTML_ARG_BOOL, 1, NULL}};
        const int nfns = (int) (sizeof(fns) / sizeof(fns[0]));
        if (skin->type > 0 && skin->type < nfns && fns[skin->type]) ui_loadout_handle(fns[skin->type], NULL, a, 2);
        else if (skin->type == 0 && skin->weapon > 0) {
            ui_loadout_skin_selector(skin->weapon - 1, 0);
            ui_loadout_select_skin(g_wheel.win_skin, skin->weapon - 1);
            menu_show_window(MENU_WINDOW_NONE, 0);
        }
        ui_alert("Skin equipped");
        return 0;
    }
    if (!strcmp(name, "knSpinRespinInfo")) { // spinInfoBtn
        g_spin_button_time = glfwGetTime();
        if (g_can_respin && g_wheel.win_skin >= 0) open_item_sales(g_wheel.win_skin);
        return 0;
    }
    if (!strcmp(name, "spinHistoryPopup")) {
        html_ui_stop_propagation(); // f1822(window.event)
        g_spin_button_time = glfwGetTime();
        history_popup(argc > 0 && args[0].kind == HTML_ARG_BOOL && args[0].num != 0);
        return 1;
    }
    if (!strcmp(name, "getSpinHistory")) { // "Load More" (this)
        HtmlElem btn = html_dom_query(0, ".spinHistoryClear");
        if (btn) {
            html_dom_set_class(btn, "disabled", 1);
            html_dom_set_html(btn, "Loading...");
        }
        history_request();
        return 1;
    }
    if (!strcmp(name, "showSpinStats")) {
        html_ui_stop_propagation();
        history_stats();
        return 1;
    }
    return -1;
}

const SpinWheelDraw *store_ui_wheel_draw(void) { return &g_wheel_draw; }

// --render-scenario state.spin / spinMulti: the wheel screen without the backend
// (prizeWheel(i, null, multi, false, true) in capture-v7.js)
void store_ui_scenario_wheel(const int index, const int multi) { prize_wheel(index, -1, multi, 0, 1); }

// state.spinResult {rand, item} (numbers, or arrays for a multi-spin): the brain's spin reply,
// then the spin itself skipped (every pointer at its target: the reveal runs on the next tick,
// the item already flown in)
// "live": true keeps the spin running from the first frame (pointers turn, the end animation and
// the item's fly-in play on the scenario's clock: the sequence captures at several anim_time values)
void store_ui_scenario_result(const cJSON *result) {
    const AccountInfo *acc = account_get();
    const cJSON *items = cJSON_GetObjectItem(result, "item");
    const int n = cJSON_IsArray(items) ? cJSON_GetArraySize(items) : 1;
    const int live = cJSON_IsTrue(cJSON_GetObjectItem(result, "live"));
    cJSON *frame = cJSON_CreateArray();
    cJSON_AddItemToArray(frame, cJSON_CreateString("spin"));
    cJSON_AddItemToArray(frame, cJSON_CreateNull());
    cJSON_AddItemToArray(frame, cJSON_Duplicate(cJSON_GetObjectItem(result, "rand"), 1));
    cJSON_AddItemToArray(frame, cJSON_Duplicate(items, 1));
    cJSON_AddItemToArray(frame, cJSON_CreateNumber(acc ? acc->funds - (g_cur ? g_cur->price * n : 0) : 0));
    spin_response(frame);
    cJSON_Delete(frame);
    if (live) return;
    for (int i = 0; i < g_wheel.pointers; i++) g_wheel.rotation[i] = g_wheel.target[i];
    spin_finished(0);
    g_wheel.end_val = g_wheel.end_target * 0.999999; // the next tick ends the slice fill: button gone, item shown
    g_wheel.cam_x = 30;
}

static void set_px(HtmlElem el, const char *prop, const double px) {
    if (!el) return;
    char v[32];
    snprintf(v, sizeof(v), "%.3fpx", px);
    html_dom_set_style(el, prop, v);
}

// The menu's dimming layer (#instructionHolder, 50% black) lies under #menuHolder (z-index 10) in
// Krunker, so the wheel canvas inside #spinUI is drawn undimmed above it. Our wheel is drawn
// under the whole page (spin_wheel.c), so while it shows the holder's background is swapped for
// a circular hole over the wheel disc whose spread shadow dims everything else (the class
// preview needs no hole: it is its canvas's own bitmap, html_dom_canvas_gl). The holder's own content
// (#instructionHider: CLICK TO PLAY / the loading ring, centred under the disc) is hidden
// meanwhile: Krunker's wheel canvas covers it, ours would show it through the disc.
static int g_hole_on;

static void set_hider_visibility(const char *v) {
    HtmlElem hider = html_dom_by_id("instructionHider");
    if (hider) html_dom_set_style(hider, "visibility", v);
}

static void spin_hole(const int on, const float cx, const float cy, const float r) {
    HtmlElem holder = html_dom_by_id("instructionHolder");
    if (!holder) return;
    HtmlElem hole = html_dom_by_id("knSpinHole");
    if (!on) {
        if (!g_hole_on) return;
        g_hole_on = 0;
        if (hole) html_dom_set_display(hole, "none");
        set_hider_visibility("");
        html_dom_set_style(holder, "background-color", "");
        html_dom_set_style(holder, "overflow", "");
        return;
    }
    if (!hole) {
        hole = html_dom_create("div", "knSpinHole");
        if (!hole) return;
        html_dom_append(holder, hole);
        html_dom_set_style(hole, "position", "absolute");
        html_dom_set_style(hole, "pointer-events", "none");
        html_dom_set_style(hole, "border-radius", "50%");
        html_dom_set_style(hole, "box-shadow", "0 0 0 4000px rgba(0, 0, 0, 0.5)");
    }
    g_hole_on = 1;
    set_hider_visibility("hidden");
    html_dom_set_style(holder, "background-color", "transparent");
    html_dom_set_style(holder, "overflow", "hidden");
    HtmlRect pr = {0};
    html_dom_get_rect(holder, &pr);
    const float offset_width = html_dom_offset_width(holder);
    float k = offset_width > 0.0f && pr.w > 0.0f ? pr.w / offset_width : 1.0f;
    set_px(hole, "left", (cx - r - pr.x) / k);
    set_px(hole, "top", (cy - r - pr.y) / k);
    set_px(hole, "width", 2 * r / k);
    set_px(hole, "height", 2 * r / k);
    html_dom_set_display(hole, "block");
}

// index.js render loop while #spinUI shows: spinWhlMlt, the end animation, the pointer
// (spinRotation -> spinVal, a tick_0 every PI / 1.5), the button/text sizes, the canvas
// (handed to spin_wheel.c as g_wheel_draw) and the item + name card fly-in.
void store_ui_tick(void) {
    if (g_eterno_music && (!menu_window_is_open() || menu_ui()->active_window != 14 || g_store_tab != 5)) eterno_music(0);
    const double now = glfwGetTime();
    double delta_ms = g_wheel.last_time > 0 ? (now - g_wheel.last_time) * 1000.0 : 0.0;
    g_wheel.last_time = now;
    if (delta_ms > 100) delta_ms = 100;
    store_ui_sub_tab_tick(delta_ms);
    g_wheel_draw.visible = 0;
    HtmlElem ui = html_dom_by_id("spinUI");
    char disp[16] = "";
    if (!ui || html_dom_get_style(ui, "display", disp, sizeof(disp)) < 0 || strcmp(disp, "block")) {
        g_wheel.step_time = now;
        spin_hole(0, 0, 0, 0);
        return;
    }
    const UiWheel *wheel = current_wheel();
    HtmlElem canvas = html_dom_by_id("spinWindow");
    const float w = canvas ? html_dom_offset_width(canvas) : 0.0f;
    // v7.1.4 doubled #spinWindow (620px -> 1240px in main.css) and halved what is drawn on it:
    // the item canvas is clientWidth * 2.1 / 2 and the wheel's radius width / 2 * 0.5 (v3.1.1:
    // 2.1 x and 0.95, which on v7's canvas made everything twice v7's size)
    const double item_w = w * 2.1 / 2;
    HtmlElem item_canvas = html_dom_by_id("spinItemCanvas");
    set_px(item_canvas, "width", item_w);
    set_px(item_canvas, "height", item_w);
    if (!wheel || w <= 0) {
        g_wheel.step_time = now;
        return;
    }

    // 60 Hz steps for the per-frame factors
    int steps = (int) ((now - g_wheel.step_time) / WHEEL_STEP);
    if (steps > 30) {
        steps = 30;
        g_wheel.step_time = now;
    } else {
        g_wheel.step_time += steps * WHEEL_STEP;
    }

    if (g_wheel.whl_mlt < 1 && (g_wheel.whl_mlt += delta_ms * 0.008) >= 1) g_wheel.whl_mlt = 1;
    if (g_wheel.end_val < g_wheel.end_target) {
        if ((g_wheel.end_val += delta_ms * 0.4) >= g_wheel.end_target) g_wheel.end_val = g_wheel.end_target;
        if (!wheel->free) g_can_respin = 1;
        set_display("spinItem", "block");
        set_display("spinItemName", "inline-block");
        set_display("spinUIBtns", "inline-block");
        set_display("spinEquipBtn", "inline-block");
        set_display("spinNextBtn", g_wheel.next_item < g_wheel.item_count ? "inline-block" : "none");
    }
    // every pointer approaches its target by speed * 0.024 of the rest per frame; tick_0 every
    // PI / 1.5 of travel; snaps within 0.003 / speed and calls f1847 (the reveal once all stopped)
    for (int i = 0; i < steps; i++) {
        for (int k = 0; k < g_wheel.pointers; k++) {
            if (g_wheel.rotation[k] == g_wheel.target[k]) continue;
            const double step = (g_wheel.target[k] - g_wheel.rotation[k]) * g_wheel.speed[k] * 0.024;
            g_wheel.rotation[k] += step;
            if ((g_wheel.tick_counter -= fabs(g_wheel.target[k] - g_wheel.rotation[k]) * g_wheel.speed[k] * 0.024) <= 0) {
                g_wheel.tick_counter += M_PI / 1.5;
                audio_play("tick_0", 0.2f, 0, 1.0f);
            }
            if (fabs(g_wheel.target[k] - g_wheel.rotation[k]) <= 0.003 / g_wheel.speed[k]) {
                g_wheel.rotation[k] = g_wheel.target[k];
                spin_finished(0);
            }
        }
    }

    const double opacity = 1 - (g_wheel.end_target ? g_wheel.end_val / g_wheel.end_target : 0); // spinButtonOpacity
    const double radius = w / 2.0 * 0.5 * g_wheel.whl_mlt; // spinWindow.width / 2 * 0.5 * spinWhlMlt
    const double hw = radius * 0.3 * (1 - opacity);
    HtmlElem button = html_dom_by_id("spinButton");
    if (button) {
        char v[32];
        snprintf(v, sizeof(v), "%.4f", opacity);
        html_dom_set_style(button, "opacity", v);
    }
    set_px(button, "width", radius * 2 * 0.6 * 0.8 + hw);
    set_px(button, "height", radius * 2 * 0.6 * 0.8 + hw);
    set_px(html_dom_by_id("spinText"), "font-size", radius * 2 * 0.6 * 0.15 + hw);
    set_px(html_dom_by_id("spinCost"), "font-size", radius * 2 * 0.6 * 0.08 + hw);

    // the canvas
    SpinWheelDraw *d = &g_wheel_draw;
    HtmlRect rect;
    if (!html_dom_get_rect(canvas, &rect)) return;
    d->x = rect.x;
    d->y = rect.y;
    d->w = rect.w;
    d->h = rect.h;
    d->canvas_w = w;
    d->canvas_h = html_dom_offset_height(canvas);
    { // the disc on screen: canvas px * (rect / clientWidth), centre moved by translate(2, 2)
        const float k = rect.w / w;
        spin_hole(1, rect.x + (w / 2 + 2) * k, rect.y + (d->canvas_h / 2 + 2) * k, (float) radius * k);
    }
    d->radius = (float) radius;
    d->slice_rotation = 0;
    const int win = g_wheel.win_rarity;
    if (g_wheel.end_target) {
        double hy = 0;
        for (int z = 0; z < g_viewer_rarity_count; z++) {
            if (win == z) {
                hy += wheel_rarity(wheel, z) / 2;
                break;
            }
            hy += wheel_rarity(wheel, z);
        }
        hy /= 100;
        const double t = g_wheel.end_val / g_wheel.end_target;
        hy = fmod(M_PI * 2 * hy * t, M_PI * 2);
        d->slice_rotation = (float) (hy - M_PI * t);
    }
    d->slices = 0;
    double hx = 0;
    for (int z = 0; z < g_viewer_rarity_count && d->slices < SPIN_WHEEL_MAX_SLICES; z++) {
        double share = wheel_rarity(wheel, z);
        if (g_wheel.end_target) {
            if (win == z) share += g_wheel.end_val;
            // v10 index.js: iìÌìiií -= endVal * (rarities[i] || 0 / endTarget): the operator
            // precedence makes it endVal * rarity (no division), so the losing slices run past zero
            else share -= g_wheel.end_val * wheel_rarity(wheel, z);
        }
        float rgb[3];
        hex_rgb(g_viewer_rarities[z].color, rgb);
        if (win == z && g_wheel.end_target && z == 6) { // RENDER.rarityAnim && rarity.animate (Unobtainable, as line ~419): fill = hue(round(anim)), anim += 0.3 dt
            hue_rgb(round(g_wheel.rainbow), rgb);
            if ((g_wheel.rainbow += 0.3 * delta_ms) >= 360) g_wheel.rainbow = 0;
        }
        if (share > 0) {
            d->slice_from[d->slices] = (float) hx;
            d->slice_to[d->slices] = (float) (hx + M_PI * 2 * (share / 100));
            memcpy(d->slice_rgb[d->slices], rgb, sizeof(rgb));
            d->slices++;
        }
        hx += M_PI * 2 * (share / 100);
    }
    d->pointer_count = g_wheel.pointers < SPIN_WHEEL_MAX_POINTERS ? g_wheel.pointers : SPIN_WHEEL_MAX_POINTERS;
    for (int k = 0; k < d->pointer_count; k++) d->pointer_rotation[k] = (float) fmod(g_wheel.rotation[k], M_PI * 2);
    d->pointer_scale = (float) opacity;
    d->visible = 1;

    // the won item and its name card
    HtmlElem item = html_dom_by_id("spinItem");
    char item_disp[16] = "";
    if (!g_wheel.has_item || !item || html_dom_get_style(item, "display", item_disp, sizeof(item_disp)) < 0 || strcmp(item_disp, "block")) return;
    const double focus = 1 - fabs(g_wheel.cam_x - 30) / 270; // itemNameFocus
    HtmlElem name = html_dom_by_id("spinItemName");
    if (name) {
        char v[32];
        snprintf(v, sizeof(v), "%.4f", focus);
        html_dom_set_style(name, "opacity", v);
        set_px(name, "margin-top", radius * 0.6);
        set_px(name, "font-size", radius / 6.5 * focus);
        for (HtmlElem ch = html_dom_first_child(name); ch; ch = html_dom_next(ch)) {
            char tag[8] = "";
            if (html_dom_tag(ch, tag, sizeof(tag)) > 0 && !strcmp(tag, "div")) set_px(ch, "font-size", radius / 12 * focus);
        }
        set_px(name, "padding", radius / 20 * focus);
        set_px(name, "padding-left", radius / 1.4 * focus);
        set_px(name, "padding-right", radius / 1.4 * focus);
    }
    for (int i = 0; i < steps && g_wheel.cam_x != 30; i++) {
        g_wheel.cam_x -= (g_wheel.cam_x - 30) * 0.24;
        if (fabs(30 - g_wheel.cam_x) <= 0.05) g_wheel.cam_x = 30;
    }
    g_wheel.bob_time += delta_ms * 0.0018;
    // the camera (fov 45, y 10) sees the item at distance |(cam_x, 10)|: apparent size ~ 1 / distance;
    // bob: position.y = sin(t) * 0.55 world units, 2 * 31.6 * tan(22.5 deg) = 26.2 units over the canvas
    HtmlElem img = html_dom_by_id("knSpinItemImg");
    if (img) {
        const double scale = sqrt(30.0 * 30.0 + 100.0) / sqrt(g_wheel.cam_x * g_wheel.cam_x + 100.0);
        const double bob = -sin(g_wheel.bob_time) * 0.55 * item_w / 26.2;
        char v[64];
        snprintf(v, sizeof(v), "translateY(%.2fpx) scale(%.4f)", bob, scale);
        html_dom_set_style(img, "transform", v);
    }
}

void store_ui_frame(const cJSON *frame) {
    const char *label = cJSON_GetStringValue(cJSON_GetArrayItem(frame, 0));
    if (!label) return;
    if (!strcmp(label, "spin")) spin_response(frame);
    else if (!strcmp(label, "spinKR")) spin_kr_response(frame);
    else if (!strcmp(label, "spinH")) history_response(frame);
}

// claimReward(): the Free KR wheel once its timer ran out (v7.1.4 shows an ad first)
void store_ui_free_kr_wheel(void) { prize_wheel(free_kr_wheel(), -1, 0, 0, 0); }
