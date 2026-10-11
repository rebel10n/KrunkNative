// Krunker v3.1.1 Store prize wheels for a signed-in account (index.js windows[13] tab 0,
// prizeWheel, spinButton.onclick, spinResponse, spinUI.onclick, showSpinError) over the
// brain's social socket: ["spin", wheel] -> ["spin", err|null, randNum, skinIndex, funds,
// follow]. Wheel data is src/data/store.js `wheels`; skins / rarities come from the client's
// generated store table (client/src/viewer/viewer_store.h). The three.js wheel and the 3D
// item preview are not ported: the won item shows its preview PNG (UTILS.getPreview).
#include <backend.h>
#include <backend_ui.h>
#include <app_mode.h>
#include <ui_logic.h>
#include <audio.h>
#include "../viewer/viewer_store.h"
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
    {"Free KR", NULL, NULL, 0, 0, 1, 1, 1, 0, 0, 0, {0}},
    {"Twitch", NULL, "Redeem", 0, 0, 0, 0, 0, 0, 1, 0, {0, 0, 50, 35, 15, 0}},
};
#define WHEEL_COUNT ((int) (sizeof(k_wheels) / sizeof(k_wheels[0])))
#define FOLLOW_IND 40 // config.followInd

static int g_spin_index = -1;
static int g_spinning, g_can_spin, g_can_respin;
static double g_spin_button_time; // spinButton click (stopPropagation: spinUI ignores it)
static double g_reveal_at;        // the wheel "turns" until then; the item shows afterwards
static cJSON *g_pending;          // the spin reply waiting for g_reveal_at (owned)
#define SPIN_SECONDS 2.2

// UTILS.getPreview(skin, store) as a page-relative path
void store_skin_preview(const int index, char *out, const size_t n) {
    if (index < 0 || index >= g_viewer_skin_count) {
        snprintf(out, n, "./img/noimg.png");
        return;
    }
    const ViewerSkin *l = &g_viewer_skins[index];
    if (l->type == 4 && !(l->flags & VS_IS_3D)) {
        snprintf(out, n, "./textures/sprays/%d.png", l->id);
    } else if (l->type && l->type != 3) {
        snprintf(out, n, "./textures/previews/cosmetics/%d_%d%s%s.png", l->type, l->id, l->tex ? "_" : "", l->tex ? l->tex : "");
    } else if (l->type == 3) {
        char suffix[48] = "";
        if (l->pat >= 0) snprintf(suffix, sizeof(suffix), "_c%d", l->pat);
        else if (l->tex) snprintf(suffix, sizeof(suffix), "_%s", l->tex);
        snprintf(out, n, "./textures/previews/%s%d%s.png", g_viewer_types[3], l->id, suffix);
    } else {
        char tail[48];
        if (l->mid >= 0 && l->mid_t >= 0) snprintf(tail, sizeof(tail), "m%d_%d", l->mid, l->mid_t);
        else if (l->mid >= 0) snprintf(tail, sizeof(tail), "m%d", l->mid);
        else if (l->pat >= 0) snprintf(tail, sizeof(tail), "c%d", l->pat);
        else if (l->tex) snprintf(tail, sizeof(tail), "%s", l->tex);
        else snprintf(tail, sizeof(tail), "%d", l->id);
        snprintf(out, n, "./textures/previews/%s%d_%s.png", g_viewer_types[0], l->weapon, tail);
    }
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

// prizeWheel(i, autoSpin): the spin screen
static void prize_wheel(const int index, const int auto_spin) {
    if (index < 0 || index >= WHEEL_COUNT) return;
    const StoreWheel *w = &k_wheels[index];
    if (w->open_url) ui_open_external("https://www.youtube.com/channel/UCivv7CI0mzjeBiz8E-z5YPw"); // config.followURLS
    if (w->twitch) return; // no Twitch drops in KrunkNative
    g_spin_index = index;
    menu_show_window(MENU_WINDOW_NONE, 0); // showWindow(0)
    g_can_spin = !g_spinning;
    g_can_respin = 0;
    set_display("menuHider", "none");      // UI.toggleMenuHider(false)
    set_display("spinRespin", w->free ? "none" : "");
    set_display("spinRespinInfo", w->free ? "none" : "");
    set_display("spinItem", "none");
    set_display("respinUI", "none");
    set_display("spinUI", "block");
    set_display("spinItemName", "none");
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

    StrBuf sb = {0};
    char upper[32];
    snprintf(upper, sizeof(upper), "%s", w->name);
    for (char *p = upper; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
    sb_appendf(&sb, "<div style='font-size:50px;color:#fff'>%s</div>", upper);
    int last_rarity = 0;
    for (int r = 0; r < g_viewer_rarity_count && r < 7; r++) {
        if (r >= 5) continue; // store.rarities[r].noSpin (Contraband, Unobtainable)
        if (!w->rarities[r]) continue;
        last_rarity = r;
        char pct[16];
        snprintf(pct, sizeof(pct), "%g", w->rarities[r]);
        sb_appendf(&sb, "<div style='margin-top:5px;color:#fff'><div class='colCub' style='background-color:%s'></div>%s<span style='color:rgba(255,255,255,0.5);float:right'>&emsp;%s%%</span></div>",
                   g_viewer_rarities[r].color, g_viewer_rarities[r].name, pct);
    }
    char *html = sb_finish(&sb);
    set_html("spinInfo", html);
    free(html);
    spin_kr();
    // four random items of the wheel's top rarity (store.getSpnItems, current season)
    int pool[512], count = 0;
    for (int s = 0; s < g_viewer_skin_count && count < 512; s++) {
        const ViewerSkin *k = &g_viewer_skins[s];
        if (k->rarity != last_rarity || (k->flags & (VS_BLOCKED | VS_NO_SPIN))) continue;
        if ((k->seas ? k->seas : 1) != g_viewer_current_season) continue;
        pool[count++] = s;
    }
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

// spinResponse(error, randNum, itemID, funds, follow): shown once the wheel stopped
static void spin_reveal(const cJSON *frame);
static void spin_response(const cJSON *frame) {
    if (cJSON_GetStringValue(cJSON_GetArrayItem(frame, 1))) {
        spin_reveal(frame);
        return;
    }
    cJSON_Delete(g_pending);
    g_pending = cJSON_Duplicate(frame, 1);
    g_reveal_at = glfwGetTime() + SPIN_SECONDS;
    audio_play("tick_0", 0.2f, 0, 1.0f);
}

static void spin_reveal(const cJSON *frame) {
    g_spinning = 0;
    const char *err = cJSON_GetStringValue(cJSON_GetArrayItem(frame, 1));
    if (err || !cJSON_IsNumber(cJSON_GetArrayItem(frame, 3))) {
        // showSpinError("Spin Error 3"); "Not enough KR" opens the Purchase KR tab like Error 9
        if (err && strstr(err, "KR")) {
            set_display("spinUI", "none");
            set_display("menuHider", "block");
            account_ui_handle("windows.13.purchase", NULL, NULL, 0);
            return;
        }
        ui_alert(err ? err : "Spin Error 3");
        if (g_spin_index >= 0) prize_wheel(g_spin_index, 0);
        return;
    }
    const int item = (int) cJSON_GetNumberValue(cJSON_GetArrayItem(frame, 3));
    account_set_funds(cJSON_GetNumberValue(cJSON_GetArrayItem(frame, 4)));
    const int owned = account_add_skin(item);
    spin_kr();
    const ViewerSkin *skin = item >= 0 && item < g_viewer_skin_count ? &g_viewer_skins[item] : NULL;
    StrBuf sb = {0};
    sb_append_esc(&sb, skin ? skin->name : "?");
    sb_append(&sb, "<div style='color:rgba(255,255,255,0.5)'>by Krunker.io</div><div style='color:rgba(255,255,255,0.5)'>");
    if (owned > 1) sb_appendf(&sb, "<span style='color:#fff'>x%d</span> Owned", owned);
    else sb_append(&sb, "<span style='color:#fff'>NEW</span>");
    sb_append(&sb, "</div>");
    char *html = sb_finish(&sb);
    set_html("spinItemName", html);
    free(html);
    // the item instead of the 3D preview; colour = rarity
    char src[160];
    store_skin_preview(item, src, sizeof(src));
    StrBuf img = {0};
    sb_appendf(&img, "<img src='%s' onerror='this.onerror=null;this.src=\"./img/noimg.png\";' style='width:22%%;margin-top:30%%;border:5px solid %s;border-radius:6px'>", src,
               skin && skin->rarity >= 0 && skin->rarity < g_viewer_rarity_count ? g_viewer_rarities[skin->rarity].color : "#fff");
    html = sb_finish(&img);
    set_html("spinItemCanvas", html);
    free(html);
    set_html("spinCost", "");
    set_html("spinHeader", "");
    set_display("spinText", "none");
    HtmlElem button = html_dom_by_id("spinButton");
    if (button) html_dom_set_style(button, "opacity", "0");
    set_display("spinItem", "block");
    set_display("spinItemName", "block");
    if (!k_wheels[g_spin_index >= 0 ? g_spin_index : 0].free) set_display("respinUI", "block");
    g_can_respin = 1;
    printf("[store] unboxed %s (skin %d, %d owned)\n", skin ? skin->name : "?", item, owned);
}

int store_ui_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) ev;
    if (!name || !backend_enabled() || !account_get()) return -1;
    if (!strcmp(name, "prizeWheel")) {
        prize_wheel(argc > 0 && args[0].kind == HTML_ARG_NUM ? (int) args[0].num : -1, argc > 1 && args[1].kind == HTML_ARG_BOOL ? args[1].num != 0 : 0);
        return 1;
    }
    if (!strcmp(name, "knSpinButton")) {
        g_spin_button_time = glfwGetTime();
        const AccountInfo *acc = account_get();
        if (g_spinning || !g_can_spin || g_spin_index < 0) return 0;
        if (acc->funds < k_wheels[g_spin_index].price) {
            set_display("spinUI", "none");
            set_display("menuHider", "block");
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
        return 0;
    }
    if (!strcmp(name, "knSpinUI")) {
        if (glfwGetTime() - g_spin_button_time < 0.05) return 1; // the button's click bubbling up
        if (g_spinning) return 1;
        set_display("spinUI", "none");
        set_display("menuHider", "block");
        backend_social_send_text("[\"unbx\"]");
        menu_show_window(14, 0);
        return 1;
    }
    if (!strcmp(name, "knSpinRespin")) {
        g_spin_button_time = glfwGetTime();
        if (g_can_respin) prize_wheel(g_spin_index, 1);
        return 0;
    }
    if (!strcmp(name, "knSpinRespinInfo")) {
        g_spin_button_time = glfwGetTime();
        return 0;
    }
    return -1;
}

// Per frame while #spinUI shows: the wheel geometry v3.1.1 recomputes every frame
// (spinButton / spinText / spinCost sizes from #spinWindow; the canvas wheel itself is
// drawn as a white disc: no 2D canvas natively) and the delayed reveal.
void store_ui_tick(void) {
    HtmlElem ui = html_dom_by_id("spinUI");
    char disp[16] = "";
    if (!ui || html_dom_get_style(ui, "display", disp, sizeof(disp)) < 0 || strcmp(disp, "block")) return;
    HtmlElem canvas = html_dom_by_id("spinWindow");
    const float w = canvas ? html_dom_offset_width(canvas) : 0.0f;
    if (w > 0) {
        const double r = w / 2.0 * 0.95;
        const double fade = g_pending || g_can_spin || g_spinning ? 0.0 : 1.0; // 1 - spinButtonOpacity
        const double hw = r * 0.3 * fade;
        char v[32];
        snprintf(v, sizeof(v), "%.3fpx", r * 2 * 0.6 * 0.8 + hw);
        HtmlElem b = html_dom_by_id("spinButton");
        if (b) {
            html_dom_set_style(b, "width", v);
            html_dom_set_style(b, "height", v);
        }
        snprintf(v, sizeof(v), "%.3fpx", r * 2 * 0.6 * 0.15 + hw);
        HtmlElem t = html_dom_by_id("spinText");
        if (t) html_dom_set_style(t, "font-size", v);
        snprintf(v, sizeof(v), "%.3fpx", r * 2 * 0.6 * 0.08 + hw);
        HtmlElem c = html_dom_by_id("spinCost");
        if (c) html_dom_set_style(c, "font-size", v);
        HtmlElem item_canvas = html_dom_by_id("spinItemCanvas"); // spinItemCanvas = 2.1 x the wheel canvas
        if (item_canvas) {
            snprintf(v, sizeof(v), "%.3fpx", w * 2.1);
            html_dom_set_style(item_canvas, "width", v);
            html_dom_set_style(item_canvas, "height", v);
            html_dom_set_style(item_canvas, "display", "inline-block");
        }
        HtmlElem name = html_dom_by_id("spinItemName");
        char nd[16] = "";
        if (name && html_dom_get_style(name, "display", nd, sizeof(nd)) >= 0 && strcmp(nd, "none")) {
            // the item name card scales with the wheel (itemNameFocus = 1 once the camera settled)
            snprintf(v, sizeof(v), "%.3fpx", r * 0.6);
            html_dom_set_style(name, "margin-top", v);
            snprintf(v, sizeof(v), "%.3fpx", r / 6.5);
            html_dom_set_style(name, "font-size", v);
            snprintf(v, sizeof(v), "%.3fpx", r / 20);
            html_dom_set_style(name, "padding", v);
            snprintf(v, sizeof(v), "%.3fpx", r / 2);
            html_dom_set_style(name, "padding-left", v);
            html_dom_set_style(name, "padding-right", v);
            snprintf(v, sizeof(v), "%.3fpx", r / 12);
            for (HtmlElem ch = html_dom_first_child(name); ch; ch = html_dom_next(ch)) {
                char tag[8] = "";
                if (html_dom_tag(ch, tag, sizeof(tag)) > 0 && !strcmp(tag, "div")) html_dom_set_style(ch, "font-size", v);
            }
        }
        if (canvas) { // the wheel's white disc (spinCanvasContext.arc(..., wheelRadius))
            html_dom_set_style(canvas, "border-radius", "50%");
            html_dom_set_style(canvas, "background-color", "#fff");
        }
    }
    if (g_pending && glfwGetTime() >= g_reveal_at) {
        cJSON *frame = g_pending;
        g_pending = NULL;
        spin_reveal(frame);
        audio_play("reward", 0.3f, 0, 1.0f);
        cJSON_Delete(frame);
    }
}

void store_ui_frame(const cJSON *frame) {
    const char *label = cJSON_GetStringValue(cJSON_GetArrayItem(frame, 0));
    if (label && !strcmp(label, "spin")) spin_response(frame);
}
