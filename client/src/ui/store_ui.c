// Krunker v3.1.1 Store prize wheels for a signed-in account (index.js windows[13] tab 0,
// prizeWheel, spinButton.onclick, spinResponse, spinUI.onclick, showSpinError) over the
// brain's social socket: ["spin", wheel] -> ["spin", err|null, randNum, skinIndex, funds,
// follow]. Wheel data is src/data/store.js `wheels`; skins / rarities come from the client's
// generated store table (client/src/viewer/viewer_store.h). The wheel's 2D canvas (rarity
// slices sized by the odds, pointer, centre) and the confetti are drawn by spin_wheel.c from
// the state the render loop port below keeps (index.js: spinWhlMlt, spinRotation/spinVal,
// tickCounter, endAnimVal/endAnimTarget, spinItemCamera). The three.js item preview is not
// ported: the won item shows its preview PNG (UTILS.getPreview), flying in and bobbing like
// the 3D model does.
#include <backend.h>
#include <backend_ui.h>
#include <app_mode.h>
#include <ui_logic.h>
#include <audio.h>
#include <spin_wheel.h>
#include "../viewer/viewer_store.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *name, *head_col, *price_t;
    int price, icons, free, kr, min_lvl, locked, twitch, open_url;
    double rarities[7];
} StoreWheel;

// store.js wheels (index = the spin index the server knows)
static const StoreWheel k_wheels[] = {
    {"Starter", "#b2f252", NULL, 50, 2, 0, 0, 0, 0, 0, 0, {75, 22, 3, 0, 0, 0}},
    {"Elite", "#2196F3", NULL, 100, 3, 0, 0, 0, 0, 0, 0, {50, 30, 15, 5, 0, 0}},
    {"Heroic", "#fc9803", NULL, 500, 5, 0, 0, 0, 0, 0, 0, {0, 48, 35, 14, 2.5, 0.5}},
    {"Valour", NULL, NULL, 0, 1, 0, 0, 0, 1, 0, 0, {43, 33, 16, 6, 2, 0}},
    {"Free", NULL, "Follow", 0, 2, 1, 0, 5, 0, 0, 1, {0, 48, 35, 14, 3, 0}},
    {"Free KR", NULL, NULL, 0, 0, 1, 1, 1, 0, 0, 0, {51, 19, 15, 8, 5, 1.9, 0.1}},
    {"Twitch", NULL, "Redeem", 0, 0, 0, 0, 0, 0, 1, 0, {0, 0, 50, 35, 15, 0}},
};
#define WHEEL_COUNT ((int) (sizeof(k_wheels) / sizeof(k_wheels[0])))
#define FOLLOW_IND 40 // config.followInd

static int g_spin_index = -1;
static int g_spinning, g_can_spin, g_can_respin;
static double g_spin_button_time; // spinButton click (stopPropagation: spinUI ignores it)

// index.js render-loop state of the wheel (see store_ui_tick). The per-frame factors
// (spinRotation's 0.024 approach, the item camera's 0.24) run at 60 steps per second, the
// frame rate v3.1.1 is tuned for, so the spin takes as long as in a 60 Hz browser (~7 s).
static struct {
    double whl_mlt;          // spinWhlMlt: the wheel grows in (deltaMs * 0.008)
    double val, rotation;    // spinVal / spinRotation (radians, the pointer)
    double tick_counter;     // tickCounter: tick_0 every PI / 1.5 of rotation
    double end_val, end_target; // endAnimVal / endAnimTarget (the won rarity's slice fills the wheel)
    double cam_x;            // spinItemCamera.position.x: 300 -> 30 (item fly-in)
    double bob_time;         // spinItemBobTime
    double last_time, step_time; // glfwGetTime of the last tick / the 60 Hz step clock
    int win_skin, win_rarity; // winItem (-1 = none yet)
    int has_item;            // spinnerScene: a result is loaded
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

// windows[13].gen() tab 0: one .shopCard per wheel except the Free KR one
void store_wheels_html(StrBuf *sb) {
    const AccountInfo *acc = account_get();
    sb_append(sb, "<div id='skinList' style='margin-top:15px'>");
    for (int i = 0; i < WHEEL_COUNT; i++) {
        const StoreWheel *w = &k_wheels[i];
        if (w->kr) continue;
        const int follow_ind = acc && acc->data ? (int) cJSON_GetNumberValue(cJSON_GetArrayItem(acc->data, 15)) : 0;
        const int sold = !acc || (w->free && (follow_ind == FOLLOW_IND || acc->level < w->min_lvl)) || w->locked;
        char value[32];
        if (w->locked) snprintf(value, sizeof(value), "Coming Soon");
        else if (sold) snprintf(value, sizeof(value), "Sold Out");
        else if (w->price_t) snprintf(value, sizeof(value), "%s", w->price_t);
        else snprintf(value, sizeof(value), "%d", w->price);
        const int icon = w->icons > 1 ? rand() % w->icons : 0;
        sb_appendf(sb, "<div class='shopCard%s' onmouseenter='playTick()' style='color:%s;background-image: url(&quot;./img/shop/spins/%d_%d.png&quot;)'",
                   sold ? " makeGray" : "", w->head_col ? w->head_col : "#fff", i, icon);
        if (!sold) {
            if (w->twitch) sb_append(sb, " onclick='showDrops()'");
            else sb_appendf(sb, " onclick='prizeWheel(%d)'", i);
        }
        sb_appendf(sb, ">%s Spin<div class='spinVal'>%s%s</div></div>", w->name, value, w->price_t || w->locked ? "" : " <span style='color:#fff'>KR</span>");
    }
    sb_append(sb, "</div>");
}

// store.rarities[r]: confetti count and colours (confC / confCols)
static const int k_conf_count[] = {30, 30, 30, 40, 50, 70, 100};
static const char *const k_conf_rainbow[] = {"#ff0000", "#ffa500", "#ffff00", "#008000", "#0000ff", "#4b0082", "#ee82ee"};

// UI.toggleMenuHider(on): the menu chrome hides behind the wheel and comes back with it;
// showing it again also hides #spinUI and #respinUI (the respin button lives outside #spinUI)
static void toggle_menu_hider(const int on) {
    set_display("menuHider", on ? "block" : "none");
    HtmlElem base = html_dom_by_id("uiBase");
    if (base) html_dom_set_class(base, "onMenu", on);
    set_display("gameNameHolder", on ? "" : "none");
    set_display("spectButton", on ? "" : "none");
    set_display("supportMapCreator", "none");
    if (on) {
        set_display("spinUI", "none");
        set_display("respinUI", "none");
    }
}

static const StoreWheel *current_wheel(void) {
    return g_spin_index >= 0 && g_spin_index < WHEEL_COUNT ? &k_wheels[g_spin_index] : NULL;
}

// spinWheel.getRarities(acc)[r] (0 past the wheel's list)
static double wheel_rarity(const StoreWheel *w, const int r) {
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

// store.getSpnItems(wheel, rarity, acc, config): the wheels the store offers have no
// limType / itemTypes / weaponType, so limited, illicit and limT items never qualify
static int spin_pool(const int rarity, int *pool, const int max) {
    int count = 0;
    for (int s = 0; s < g_viewer_skin_count && count < max; s++) {
        const ViewerSkin *k = &g_viewer_skins[s];
        if (k->rarity != rarity || (k->flags & (VS_BLOCKED | VS_NO_SPIN | VS_NOT_IN_WHEEL))) continue;
        if ((k->seas ? k->seas : 1) != g_viewer_current_season) continue;
        pool[count++] = s;
    }
    return count;
}

static void open_item_sales(const int skin) {
    char url[96];
    snprintf(url, sizeof(url), "/social.html?p=itemsales&i=%d", skin);
    if (!app_route_url_target(url, 1)) ui_open_external(url);
}

// prizeWheel(i, autoSpin): the spin screen
static void prize_wheel(const int index, const int auto_spin) {
    if (index < 0 || index >= WHEEL_COUNT) return;
    const StoreWheel *w = &k_wheels[index];
    if (w->open_url) ui_open_external("https://www.youtube.com/channel/UCivv7CI0mzjeBiz8E-z5YPw"); // config.followURLS
    if (w->twitch) return; // no Twitch drops in KrunkNative
    set_display("spinRespin", w->free || w->twitch ? "none" : "");
    set_display("spinRespinInfo", w->free || w->twitch ? "none" : "");
    g_spin_index = index;
    menu_show_window(MENU_WINDOW_NONE, 0); // showWindow(0)
    g_wheel.whl_mlt = 0;
    g_can_spin = !g_spinning;
    g_can_respin = 0;
    g_wheel.val = g_wheel.rotation = g_wheel.tick_counter = 0;
    g_wheel.end_val = g_wheel.end_target = 0;
    HtmlElem ui = html_dom_by_id("spinUI");
    char disp[16] = "";
    if (!ui || html_dom_get_style(ui, "display", disp, sizeof(disp)) < 0 || strcmp(disp, "block")) {
        toggle_menu_hider(0);
    }
    set_display("spinItem", "none");
    set_display("respinUI", "none");
    set_display("spinUI", "block");
    set_display("spinItemName", "none");
    g_wheel.cam_x = 300;
    HtmlElem button = html_dom_by_id("spinButton");
    if (button) html_dom_set_style(button, "opacity", "1");
    set_display("spinText", "table-cell");
    set_html("spinHeader", "SPIN");
    char buf[256];
    snprintf(buf, sizeof(buf), "-%dKR", w->price);
    set_html("spinCost", buf);
    snprintf(buf, sizeof(buf), "<i class='material-icons' style='margin-left:-10px;text-shadow:none;vertical-align:sub;font-size:35px;'>refresh</i> %d KR", w->price);
    set_html("spinRespin", buf);
    // the inline handlers spinButton/spinUI/spinRespin(Info).onclick (set by script in v3.1.1)
    HtmlElem el;
    if ((el = html_dom_by_id("spinButton"))) html_dom_set_attr(el, "onclick", "knSpinButton()");
    if ((el = html_dom_by_id("spinUI"))) html_dom_set_attr(el, "onclick", "knSpinUI()");
    if ((el = html_dom_by_id("spinRespin"))) html_dom_set_attr(el, "onclick", "knSpinRespin()");
    if ((el = html_dom_by_id("spinRespinInfo"))) html_dom_set_attr(el, "onclick", "knSpinRespinInfo()");

    // #spinInfo: every rarity the wheel can roll with its chance (no store.rarities entry is
    // noSpin in v3.1.1: Heroic lists Contraband 0.5%)
    StrBuf sb = {0};
    char upper[32];
    snprintf(upper, sizeof(upper), "%s", w->name);
    for (char *p = upper; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
    sb_appendf(&sb, "<div style='font-size:50px;color:#fff'>%s</div>", upper);
    int last_rarity = 0;
    for (int r = 0; r < g_viewer_rarity_count && r < 7; r++) {
        if (!wheel_rarity(w, r)) continue;
        last_rarity = r;
        char pct[16];
        snprintf(pct, sizeof(pct), "%g", wheel_rarity(w, r)); // JS number -> string (48, 2.5, 0.5)
        sb_appendf(&sb, "<div style='margin-top:5px;color:#fff'><div class='colCub' style='background-color:%s'></div>%s<span style='color:rgba(255,255,255,0.5);float:right'>&emsp;%s%%</span></div>",
                   g_viewer_rarities[r].color, g_viewer_rarities[r].name, pct);
    }
    char *html = sb_finish(&sb);
    set_html("spinInfo", html);
    free(html);
    spin_kr();
    // four random items of the wheel's top rarity (store.getSpnItems, current season)
    int pool[512];
    const int count = spin_pool(last_rarity, pool, 512);
    for (int i = 0; i < 4 && count; i++) {
        const int id = pool[rand() % count];
        char src[160];
        store_skin_preview(id, src, sizeof(src));
        sb_appendf(&sb, "<img class='spinPrevIcon' style='border-color:%s' onclick='openSales(%d)' onerror='this.onerror=null;this.src=\"./img/noimg.png\";' src='%s' />", g_viewer_rarities[g_viewer_skins[id].rarity].color, id, src);
    }
    sb_append(&sb, "<span style='color:#fff;line-height:85px;margin-left:6px'>+ More</span>");
    html = sb_finish(&sb);
    set_html("spinItems", html);
    free(html);
    if (auto_spin) {
        extern int store_ui_handle(const char *, const HtmlEvent *, const HtmlArg *, int);
        store_ui_handle("knSpinButton", NULL, NULL, 0);
    }
}

// spinResponse(error, randNum, itemID, funds, follow): the item is loaded now, the wheel
// turns to randNum (the server's roll in [0, 100), which picked the rarity) and the render
// loop reveals it once the pointer stops
static void spin_response(const cJSON *frame) {
    g_spinning = 0;
    const char *err = cJSON_GetStringValue(cJSON_GetArrayItem(frame, 1));
    if (err || !cJSON_IsNumber(cJSON_GetArrayItem(frame, 3))) {
        // showSpinError("Spin Error 3"); "Not enough KR" opens the Purchase KR tab like Error 9
        if (err && strstr(err, "KR")) {
            toggle_menu_hider(1);
            account_ui_handle("windows.13.purchase", NULL, NULL, 0);
            return;
        }
        ui_alert(err ? err : "Spin Error 3");
        HtmlElem ui = html_dom_by_id("spinUI");
        char disp[16] = "";
        if (g_spin_index >= 0 && ui && html_dom_get_style(ui, "display", disp, sizeof(disp)) >= 0 && !strcmp(disp, "block")) prize_wheel(g_spin_index, 0);
        return;
    }
    const double rand_num = cJSON_GetNumberValue(cJSON_GetArrayItem(frame, 2));
    const int item = (int) cJSON_GetNumberValue(cJSON_GetArrayItem(frame, 3));
    account_set_funds(cJSON_GetNumberValue(cJSON_GetArrayItem(frame, 4)));
    const int owned = account_add_skin(item);
    spin_kr();
    const ViewerSkin *skin = viewer_skin(item);
    StrBuf sb = {0};
    sb_append_esc(&sb, skin ? skin->name : "?");
    sb_append(&sb, "<div style='color:rgba(255,255,255,0.5)'>by ");
    sb_append_esc(&sb, skin && skin->creator ? skin->creator : "Krunker.io");
    sb_append(&sb, "</div><div style='color:rgba(255,255,255,0.5)'>");
    if (owned > 1) sb_appendf(&sb, "<span style='color:#fff'>x%d</span> Owned", owned);
    else sb_append(&sb, "<span style='color:#fff'>NEW</span>");
    sb_append(&sb, "</div>");
    char *html = sb_finish(&sb);
    set_html("spinItemName", html);
    free(html);
    g_wheel.val = M_PI * 2 * (rand_num / 100 + 13) + M_PI / 2;
    set_html("spinCost", "");
    g_wheel.rotation = 0;
    g_wheel.bob_time = 0;
    g_wheel.win_skin = item;
    g_wheel.win_rarity = skin ? skin->rarity : 0;
    g_wheel.has_item = 1;
    g_wheel.step_time = glfwGetTime();
    // the item's preview in #spinItemCanvas instead of the three.js scene, about the wheel's
    // size (the 256 px PNGs are cropped to the item); the tick flies it in
    char src[160];
    store_skin_preview(item, src, sizeof(src));
    StrBuf img = {0};
    sb_appendf(&img, "<img id='knSpinItemImg' src='%s' onerror='this.onerror=null;this.src=\"./img/noimg.png\";' style='display:block;width:50%%;height:50%%;margin:25%% auto 0;transform:scale(0)'>", src);
    html = sb_finish(&img);
    set_html("spinItemCanvas", html);
    free(html);
    printf("[store] unboxing %s (skin %d, roll %.2f, %d owned)\n", skin ? skin->name : "?", item, rand_num, owned);
}

// the pointer stopped (render loop: spinRotation reached spinVal)
static void spin_finished(void) {
    const StoreWheel *w = current_wheel();
    const int rarity = g_wheel.win_rarity;
    g_wheel.end_target = 100 - wheel_rarity(w, rarity);
    printf("[store] wheel stopped on %s\n", rarity >= 0 && rarity < g_viewer_rarity_count ? g_viewer_rarities[rarity].name : "?");
    audio_play("reward", 0.3f, 0, 1.0f);
    if (rarity >= 0 && rarity < 7) {
        const char *cols[2] = {g_viewer_rarities[rarity].color, "#fff"};
        if (rarity == 6) spin_wheel_confetti(k_conf_count[rarity], k_conf_rainbow, 7);
        else spin_wheel_confetti(k_conf_count[rarity], cols, 2);
    }
    backend_social_send_text("[\"unbx\"]");
    if (rarity >= 1) audio_play("cheer_0", 0.1f, 0, 1.0f);
}

int store_ui_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) ev;
    if (!name || !backend_enabled() || !account_get()) return -1;
    if (!strcmp(name, "prizeWheel")) {
        prize_wheel(argc > 0 && args[0].kind == HTML_ARG_NUM ? (int) args[0].num : -1, argc > 1 && args[1].kind == HTML_ARG_BOOL ? args[1].num != 0 : 0);
        return 1;
    }
    if (!strcmp(name, "openSales")) { // openURL("/social.html?p=itemsales&i=" + i)
        if (argc > 0 && args[0].kind == HTML_ARG_NUM) open_item_sales((int) args[0].num);
        return 1;
    }
    if (!strcmp(name, "knSpinButton")) {
        g_spin_button_time = glfwGetTime();
        const AccountInfo *acc = account_get();
        if (g_spinning || !g_can_spin || g_spin_index < 0) return 0;
        if (acc->funds < k_wheels[g_spin_index].price) {
            toggle_menu_hider(1);
            account_ui_handle("windows.13.purchase", NULL, NULL, 0); // showSpinError("Spin Error 9")
            return 0;
        }
        g_spinning = 1;
        g_can_spin = 0;
        char frame[32];
        snprintf(frame, sizeof(frame), "[\"spin\",%d]", g_spin_index);
        backend_social_send_text(frame);
        set_html("spinHeader", "");
        set_html("spinCost", "Purchasing...");
        audio_play("buy_1", 0.1f, 0, 1.0f);
        return 0;
    }
    if (!strcmp(name, "knSpinUI")) { // spinUI.onclick: closes the wheel at any time
        if (glfwGetTime() - g_spin_button_time < 0.05) return 1; // the button's click bubbling up
        menu_show_window(14, 0);
        toggle_menu_hider(1);
        spin_wheel_confetti_reset();
        backend_social_send_text("[\"unbx\"]");
        spin_kr();
        return 1;
    }
    if (!strcmp(name, "knSpinRespin")) {
        g_spin_button_time = glfwGetTime();
        if (g_can_respin) prize_wheel(g_spin_index, 1);
        return 0;
    }
    if (!strcmp(name, "knSpinRespinInfo")) {
        g_spin_button_time = glfwGetTime();
        if (g_can_respin && g_wheel.win_skin >= 0) open_item_sales(g_wheel.win_skin);
        return 0;
    }
    return -1;
}

const SpinWheelDraw *store_ui_wheel_draw(void) { return &g_wheel_draw; }

static void set_px(HtmlElem el, const char *prop, const double px) {
    if (!el) return;
    char v[32];
    snprintf(v, sizeof(v), "%.3fpx", px);
    html_dom_set_style(el, prop, v);
}

// The menu's dimming layer (#instructionHolder, 50% black) lies under #menuHolder (z-index 10) in
// Krunker, so the wheel canvas inside #spinUI is drawn undimmed above it. Our wheel is drawn
// under the whole page (spin_wheel.c), so while it shows the holder's background is swapped for
// a circular hole over the wheel disc whose spread shadow dims everything else, the way
// menu_ui.c keeps the class preview bright (#knPreviewHole).
static int g_hole_on;

static void spin_hole(const int on, const float cx, const float cy, const float r) {
    HtmlElem holder = html_dom_by_id("instructionHolder");
    if (!holder) return;
    HtmlElem hole = html_dom_by_id("knSpinHole");
    if (!on) {
        if (!g_hole_on) return;
        g_hole_on = 0;
        if (hole) html_dom_set_display(hole, "none");
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
    const double now = glfwGetTime();
    double delta_ms = g_wheel.last_time > 0 ? (now - g_wheel.last_time) * 1000.0 : 0.0;
    g_wheel.last_time = now;
    if (delta_ms > 100) delta_ms = 100;
    g_wheel_draw.visible = 0;
    HtmlElem ui = html_dom_by_id("spinUI");
    char disp[16] = "";
    if (!ui || html_dom_get_style(ui, "display", disp, sizeof(disp)) < 0 || strcmp(disp, "block")) {
        g_wheel.step_time = now;
        spin_hole(0, 0, 0, 0);
        return;
    }
    const StoreWheel *wheel = current_wheel();
    HtmlElem canvas = html_dom_by_id("spinWindow");
    const float w = canvas ? html_dom_offset_width(canvas) : 0.0f;
    HtmlElem item_canvas = html_dom_by_id("spinItemCanvas"); // spinItemCanvas = 2.1 x the wheel canvas
    set_px(item_canvas, "width", w * 2.1);
    set_px(item_canvas, "height", w * 2.1);
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
        if (!wheel->free && !wheel->twitch) g_can_respin = 1;
        set_display("spinItem", "block");
        set_display("spinItemName", "inline-block");
        set_display("respinUI", "inline-block");
    }
    for (int i = 0; i < steps && g_wheel.rotation != g_wheel.val; i++) {
        g_wheel.rotation += (g_wheel.val - g_wheel.rotation) * 0.024;
        if ((g_wheel.tick_counter -= (g_wheel.val - g_wheel.rotation) * 0.024) <= 0) {
            g_wheel.tick_counter += M_PI / 1.5;
            audio_play("tick_0", 0.2f, 0, 1.0f);
        }
        if (g_wheel.val - g_wheel.rotation <= 0.002) {
            g_wheel.rotation = g_wheel.val;
            spin_finished();
        }
    }

    const double opacity = 1 - (g_wheel.end_target ? g_wheel.end_val / g_wheel.end_target : 0); // spinButtonOpacity
    const double radius = w / 2.0 * 0.95 * g_wheel.whl_mlt;
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
            else share -= g_wheel.end_val * (wheel_rarity(wheel, z) / g_wheel.end_target);
        }
        if (share > 0) {
            d->slice_from[d->slices] = (float) hx;
            d->slice_to[d->slices] = (float) (hx + M_PI * 2 * (share / 100));
            hex_rgb(g_viewer_rarities[z].color, d->slice_rgb[d->slices]);
            d->slices++;
        }
        hx += M_PI * 2 * (share / 100);
    }
    d->pointer_rotation = (float) g_wheel.rotation;
    d->pointer_scale = (float) opacity;
    // (getBoundingClientRect().width / offsetWidth - 1): the :hover / :active scale
    HtmlRect brect;
    const float bw = button ? html_dom_offset_width(button) : 0.0f;
    d->pointer_hover = button && bw > 0 && html_dom_get_rect(button, &brect) ? (float) ((brect.w / bw - 1) * (radius * 0.6) * 0.3) : 0.0f;
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
        set_px(name, "padding-left", radius / 2 * focus);
        set_px(name, "padding-right", radius / 2 * focus);
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
        const double bob = -sin(g_wheel.bob_time) * 0.55 * (w * 2.1) / 26.2;
        char v[64];
        snprintf(v, sizeof(v), "translateY(%.2fpx) scale(%.4f)", bob, scale);
        html_dom_set_style(img, "transform", v);
    }
}

void store_ui_frame(const cJSON *frame) {
    const char *label = cJSON_GetStringValue(cJSON_GetArrayItem(frame, 0));
    if (label && !strcmp(label, "spin")) spin_response(frame);
}
