// The Krunker v10.0.2 page's Change Loadout (v10.js 210927-216800, doc/v10/ui.md "Windows"):
//   - windows[2] (showWindow(3)) is a 3-tab shell: #loadoutToolbar (the player's name, the tabs
//     Class / Loadout / Customize, _70483) over #loadoutBody = #charPreviewSlot (the live class
//     preview, the menu's #classPreviewCanvas moved in by toggleWindowCharPreview) and
//     #loadoutContent. Class and Loadout are v7's tabs 0 and 1 (ui_loadout.c); Customize is
//     windows[52].gen(): v7's Customize slots plus the colour / Playercard Stat / Title rows that
//     were v7's Other tab
//   - showWindow(53) is rewritten to window 3 on the Customize tab; windows[52].goBack too
//   - windows[53] (showWindow(54)) Title: the titles catalogue (gapi /titles, assets/titles.json),
//     owned first, the rest locked with their requirement; selectTitle(id) saves titleIndex
//   - the picker windows (_71634: 9 15 16 17 20 26 31 32 34 38 39 40 41 43 44 45 46 50 51 52) are
//     drawn in the same shell: _70483(loadoutTab) + .wcpRow(#charPreviewSlot, .wcpItems(gen())).
//     Their gen() is v7's (ui_loadout.c) without the close button, the cosmetics going back to
//     the Customize tab (goBackWindow(52))
#include <ui_logic.h>
#include <backend_ui.h>
#include <online_mode.h>
#include <client.h>
#include "../viewer/viewer_store.h"
#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CUSTOMIZE_TAB 2   // windows[2].tabIndex of the Customize tab
#define TITLE_WINDOW 54   // windows[53] title_selection (showTitlesWindow)

// ---------------------------------------------------------------- page


// ---------------------------------------------------------------- titles (gapi /titles)

// assets/titles.json: v10's GET /titles data [{id, name, desc, rarity, color?, requirements:
// {kind, text, skin?}, active}] in its order (the Map _34389 keeps it)
static cJSON *g_titles;
static int g_titles_loaded, g_titles_requested;
static cJSON *g_owned_titles; // acc.titles (brain "ttl"), NULL none (or still unknown)

// account.titles (v10 _34392 getTitles): the brain's "ttl" reply; asked once
static void titles_request(void) {
    if (g_titles_requested || !active_account()) return;
    g_titles_requested = 1;
    account_send_frame("ttl"); // ["ttl"]
}

static const cJSON *titles(void) {
    titles_request();
    if (!g_titles_loaded) {
        g_titles_loaded = 1;
        char *path = concat(assets_path(), "titles.json");
        unsigned char *buf = NULL;
        size_t len = 0;
        if (path && ensure_asset_file(path) && !read_file(path, &buf, &len)) g_titles = cJSON_ParseWithLength((const char *) buf, len);
        if (!cJSON_IsArray(g_titles)) fprintf(stderr, "[ui] assets/titles.json unreadable: no titles\n");
        free(buf);
        free(path);
    }
    return g_titles;
}

static int title_id(const cJSON *t) {
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(t, "id");
    return cJSON_IsNumber(id) ? id->valueint : -1;
}

static const cJSON *title_get(const int id) { // _34399
    const cJSON *t;
    if (id < 0) return NULL;
    cJSON_ArrayForEach(t, titles()) if (title_id(t) == id) return t;
    return NULL;
}

// acc.titles.indexOf(id) >= 0. A guest has none, and so has an account while the brain has no
// grants for it (sh_titles: nothing grants titles yet)
static int title_owned(const int id) {
    if (!active_account()) return 0;
    const cJSON *e;
    cJSON_ArrayForEach(e, g_owned_titles) if (cJSON_IsNumber(e) && e->valueint == id) return 1;
    return 0;
}

// _34401: title.color || rarities[rarity].color || "#cccccc"
static const char *title_color(const cJSON *t) {
    const char *c = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(t, "color"));
    if (c && c[0]) return c;
    const cJSON *r = cJSON_GetObjectItemCaseSensitive(t, "rarity");
    const int rarity = cJSON_IsNumber(r) ? r->valueint : -1;
    return rarity >= 0 && rarity < g_viewer_rarity_count && g_viewer_rarities[rarity].color ? g_viewer_rarities[rarity].color : "#cccccc";
}

// _34404: the tag style (a Contraband title without its own colour gets a white outline)
static void title_style(StrBuf *out, const cJSON *t) {
    const cJSON *r = cJSON_GetObjectItemCaseSensitive(t, "rarity");
    const int outline = !cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(t, "color")) && cJSON_IsNumber(r) && r->valueint == 5;
    sb_appendf(out, "color:%s;", title_color(t));
    if (outline) sb_append(out, "text-shadow:-1px -1px 0 #ffffff, 1px -1px 0 #ffffff, -1px 1px 0 #ffffff, 1px 1px 0 #ffffff;");
}

// titleIndex (getSavedVal("titleIndex") || -1); only a title the player has is worn
int v10_customize_title(void) {
    char buf[16];
    const int id = html_storage_get("titleIndex", buf, sizeof(buf)) >= 0 ? atoi(buf) : -1;
    return id >= 0 && title_get(id) && title_owned(id) ? id : -1;
}

void v10_customize_frame(const cJSON *frame) {
    // ["ttl", [ids]]: the account's titles
    const cJSON *ids = cJSON_GetArrayItem(frame, 1);
    cJSON_Delete(g_owned_titles);
    g_owned_titles = cJSON_IsArray(ids) ? cJSON_Duplicate(ids, 1) : NULL;
    menu_rerender_if_open(TITLE_WINDOW);
    menu_rerender_if_open(3);
}

// ---------------------------------------------------------------- the shell

// _70483(tab): #loadoutToolbar
static void toolbar(StrBuf *out, const int tab) {
    static const char *const NAMES[3] = {"Class", "Loadout", "Customize"};
    static const char *const ICONS[3][2] = {{"class.png", "pixelated"}, {"loadout.png", "crisp-edges"}, {"outfit.png", "crisp-edges"}};
    const AccountInfo *acc = active_account();
    sb_append(out, "<div id='loadoutToolbar'><div id='loadoutCharName'>");
    sb_append_esc(out, acc ? (acc->alias[0] ? acc->alias : acc->name) : "Guest");
    sb_append(out, "</div><div id='loadoutTabs'>");
    for (int i = 0; i < 3; i++) {
        char src[96];
        snprintf(src, sizeof(src), "/img/customize/%s", ICONS[i][0]);
        char url[160];
        ui_img_url(src, url, sizeof(url));
        sb_appendf(out, "<div class='loadoutTab%s' onmouseenter='playTick()' onclick='playSelect(0.1);windows[2].tabIndex=%d;showWindow(3,true,true)'><img loading='lazy' class='loadoutTabIcon %s' src='%s'>%s</div>",
                   tab == i ? " loadoutTabSel" : "", i, ICONS[i][1], url, NAMES[i]);
    }
    sb_append(out, "</div></div>");
}

// windows[52].gen() tab 0, after the slots: #otherContainer (colours, Playercard Stat, Title)
static void other_container(StrBuf *out) {
    const AccountInfo *acc = active_account();
    // KrunkNative's offline player counts as an account (doc/v7/decisions.md): the colours show
    const int signed_in = acc || !online_mode_online();
    const int premium = acc && acc->premium > 0;
    sb_append(out, "<div class=\"otherContainer\" style=\"width: 100%;height: 205px;\">");
    sb_appendf(out, "<div class='settName' style='border-bottom:unset;'>%s", I18N("windows.loadout.color.skin", "Skin Color"));
    const int skin = ui_loadout_saved("skinColIndex");
    if (premium) sb_append(out, "<input class='skinColorItem' type='color' style='border-radius:2px;width:40px;' onmouseenter='playTick()' onchange='selectSkinColor(this.value)'>");
    else if (signed_in) sb_append(out, "<i class='material-icons premiumSkinCol' onclick='windows[13].premium()'>add_circle_outline</i>");
    else sb_append(out, "<div class=\"registerToUnlockSkinColor\"> Register to unlock</div>");
    for (int i = ui_loadout_skin_color_count() - 1; signed_in && i >= 0; i--) // req: account
        sb_appendf(out, "<div class='skinColorItem%s' style='background-color:#%s' onmouseenter='playTick()' onclick='selectSkinColor(%d)'></div>",
                   skin == i ? " activeSkin" : "", ui_loadout_skin_color_hex(i), i);
    sb_append(out, "</div>");
    sb_appendf(out, "<div class='settName' style='border-bottom:unset;'>%s", I18N("windows.loadout.color.hair", "Hair Color"));
    if (premium) sb_append(out, "<input class='skinColorItem' type='color' style='border-radius:2px;width:40px;' onmouseenter='playTick()' onchange='selectHairColor(this.value)'>");
    else sb_append(out, "<i class='material-icons premiumSkinCol' onclick='windows[13].premium()'>add_circle_outline</i>");
    sb_append(out, "</div>");
    sb_appendf(out, "<div class='settName' style='border-bottom:unset;'>%s", I18N("windows.loadout.color.chat", "Chat Name Color"));
    if (premium) {
        char col[16] = "#ffffff";
        if (html_storage_get("chatCol", col, sizeof(col)) < 0 || col[0] != '#') snprintf(col, sizeof(col), "#ffffff");
        sb_appendf(out, "<input class='skinColorItem' value='%s' type='color' style='border-radius:2px;width:40px;' onmouseenter='playTick()' onchange='selectChatColor(this.value)'>", col);
    } else {
        sb_append(out, "<i class='material-icons premiumSkinCol' onclick='windows[13].premium()'>add_circle_outline</i>");
    }
    sb_append(out, "</div>");
    const int stat = ui_loadout_saved("pcStatIndex");
    sb_appendf(out, "<div class='settName' style='border-bottom:unset;'>%s<select onchange='selectStat(this.value)' class='inputGrey2' style='font-size:16px;padding:4px;margin-bottom: 5px;'><option value='-1' %s>None</option>",
               I18N("windows.loadout.stat", "Playercard Stat"), stat == -1 ? "selected" : "");
    for (int i = 0; i < ui_loadout_pc_stat_count(); i++)
        sb_appendf(out, "<option value='%d' %s>%s</option>", i, stat == i ? "selected" : "", ui_loadout_pc_stat_name(i));
    sb_append(out, "</select></div>");
    const cJSON *t = title_get(v10_customize_title());
    sb_appendf(out, "<div class='settName' style='border-bottom:unset;'>%s<div class='titleSlotSelect' onmouseenter='playTick()' onclick='playSelect(0.1);showTitlesWindow()'>",
               I18N("windows.loadout.title", "Title"));
    if (t) {
        sb_append(out, "<span class='titleSlotTag' style='");
        title_style(out, t);
        sb_append(out, "'>");
        sb_append_esc(out, cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(t, "name")));
        sb_append(out, "</span>");
    } else {
        sb_appendf(out, "<span class='titleSlotNone'>%s</span>", I18N("generic.none", "None"));
    }
    sb_append(out, "<span class='material-icons titleSlotArrow'>chevron_right</span></div></div>");
    sb_append(out, "</div>");
}

// windows[2].gen()
static char *gen_loadout(void) {
    int tab = ui_loadout_tab();
    if (tab > CUSTOMIZE_TAB) { // v7's Other tab is part of Customize on this page
        tab = CUSTOMIZE_TAB;
        ui_loadout_set_tab(tab);
    }
    StrBuf out = {0};
    toolbar(&out, tab);
    sb_append(&out, "<div id='loadoutBody'><div id='charPreviewSlot'></div><div id='loadoutContent'>");
    if (tab == CUSTOMIZE_TAB) { // windows[52].gen()
        sb_append(&out, "<div id='customizeContainer' style='margin-bottom: -10px;text-align: center;'>");
        ui_loadout_tab_html(&out, CUSTOMIZE_TAB);
        other_container(&out);
        sb_append(&out, "</div>");
    } else {
        sb_append(&out, "<div id='customizeContainer'>");
        ui_loadout_tab_html(&out, tab);
        sb_append(&out, "</div>");
    }
    sb_append(&out, "</div></div>");
    return sb_finish(&out);
}

// windows[53].gen(): the titles, owned first (each group in catalogue order)
static char *gen_titles(void) {
    const int sel = v10_customize_title();
    StrBuf out = {0};
    toolbar(&out, CUSTOMIZE_TAB);
    sb_append(&out, "<div class='wcpRow'><div id='charPreviewSlot'></div><div class='wcpItems'>");
    sb_append(&out, "<div id='itemSearchH'><div class='custBack' onclick='playSelect(0.1);goBackWindow(52)' onmouseenter='playTick();'><span class='material-icons custBackArr'>arrow_back</span></div></div><div id='skinList' class='titleListWrap' style='overflow-y: auto;height: calc(100% - 95px);'>");
    char equipped[128];
    snprintf(equipped, sizeof(equipped), "<span class='titleRowEquipped'>%s</span>", I18N("windows.loadout.titleEquipped", "Equipped"));
    sb_appendf(&out, "<div class='titleRow%s' style='color:#cfcfcf' onmouseenter='playTick()' onclick='playSelect(0.1);selectTitle(-1)'><div class='titleRowInfo'><div class='titleRowName'>%s</div><div class='titleRowDesc'>%s</div></div>%s</div>",
               sel < 0 ? " titleRowSel" : "", I18N("generic.none", "None"), I18N("generic.default", "Default"), sel < 0 ? equipped : "");
    for (int pass = 0; pass < 2; pass++) {
        const cJSON *t;
        cJSON_ArrayForEach(t, titles()) {
            const int id = title_id(t), owned = title_owned(id);
            if (id < 0 || owned != !pass) continue;
            const char *name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(t, "name"));
            const char *desc = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(t, "desc"));
            if (owned) {
                sb_appendf(&out, "<div class='titleRow%s' style='color:%s' onmouseenter='playTick()' onclick='playSelect(0.1);selectTitle(%d)'><div class='titleRowInfo'><div class='titleRowName'>",
                           sel == id ? " titleRowSel" : "", title_color(t), id);
                sb_append_esc(&out, name ? name : "");
                sb_append(&out, "</div>");
                if (desc && desc[0]) {
                    sb_append(&out, "<div class='titleRowDesc'>");
                    sb_append_esc(&out, desc);
                    sb_append(&out, "</div>");
                }
                sb_appendf(&out, "</div>%s</div>", sel == id ? equipped : "");
            } else if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(t, "active"))) {
                const char *req = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(t, "requirements"), "text"));
                sb_append(&out, "<div class='titleRow titleRowLocked' onmouseenter='playTick()'><div class='titleRowInfo'><div class='titleRowName'>");
                sb_append_esc(&out, name ? name : "");
                sb_append(&out, "</div><div class='titleRowDesc'><span class='material-icons titleLockIco'>lock</span>");
                sb_append_esc(&out, req && req[0] ? req : I18N("windows.loadout.titleLocked", "Locked"));
                sb_append(&out, "</div></div></div>");
            }
        }
    }
    sb_append(&out, "</div></div></div>");
    return sb_finish(&out);
}

// _71634: the windows drawn in the shell and the tab they belong to
static int shell_tab(const int id) {
    switch (id) {
        case 15: case 20: case 26: case 31: case 32: case 41: case 45: case 46: return 1;
        case 9: case 16: case 17: case 34: case 38: case 39: case 40: case 43: case 44: case 50: case 51: case 52: return 2;
        default: return -1;
    }
}

// The windows whose _71266 / gen() go back to windows[52] (the Customize tab) instead of windows[2]
static int back_to_customize(const int id) {
    switch (id) {
        case 9: case 16: case 17: case 34: case 38: case 39: case 40: case 43: case 44: case 46: case 50: case 51: case 52: return 1;
        default: return 0;
    }
}

static const char WIN_CLOSE[] = "<div class='winClose' onclick='playSelect(0.1);closWind()' onmouseenter='playTick();'><span class='material-icons winCloseArr'>close</span></div>";

static char *gen_shell(const int id) {
    char *inner = ui_loadout_generate(id, NULL);
    if (!inner) return NULL; // the captured template already is the shell
    StrBuf a = {0};
    sb_append_replace_all(&a, inner, WIN_CLOSE, "");
    free(inner);
    char *no_close = sb_finish(&a);
    StrBuf b = {0};
    if (back_to_customize(id)) sb_append_replace_all(&b, no_close, "goBackWindow(2)", "goBackWindow(52)");
    else sb_append(&b, no_close);
    free(no_close);
    char *body = sb_finish(&b);
    StrBuf out = {0};
    toolbar(&out, shell_tab(id));
    sb_append(&out, "<div class='wcpRow'><div id='charPreviewSlot'></div><div class='wcpItems'>");
    sb_append(&out, body);
    sb_append(&out, "</div></div>");
    free(body);
    return sb_finish(&out);
}

char *v10_customize_window_gen(const int id) {
    if (id == 3) return gen_loadout();
    if (id == TITLE_WINDOW) return gen_titles();
    if (shell_tab(id) >= 0) return gen_shell(id);
    return NULL;
}

int v10_customize_window_alias(const int id) {
    if (id == 53) { // showWindow: 53 == id && (windows[2].tabIndex = 2, id = 3)
        ui_loadout_set_tab(CUSTOMIZE_TAB);
        return 3;
    }
    return id;
}

// ---------------------------------------------------------------- the preview column

// toggleWindowCharPreview(null): the canvas goes back before #menuClassNameTag
void v10_customize_before_render(void) {
    const HtmlElem canvas = html_dom_by_id("classPreviewCanvas");
    const HtmlElem home = html_dom_by_id("menuClassContainer");
    if (!canvas || !home || html_dom_parent(canvas) == home) return;
    html_dom_append(home, canvas); // then the tag and the footer after it: insertBefore(menuClassNameTag)
    const HtmlElem tag = html_dom_by_id("menuClassNameTag"), foot = html_dom_by_id("menuClassFooter");
    if (tag) html_dom_append(home, tag);
    if (foot) html_dom_append(home, foot);
    menu_schedule_rect();
}

// toggleWindowCharPreview(#charPreviewSlot): the menu's preview canvas shows in the window
void v10_customize_window_post(const int id) {
    (void) id;
    const HtmlElem slot = html_dom_by_id("charPreviewSlot");
    if (!slot) return;
    HtmlElem canvas = html_dom_by_id("classPreviewCanvas");
    if (!canvas) { // lost with an old #menuWindow: a new one (class_preview.c finds it by id)
        canvas = html_dom_create("canvas", "classPreviewCanvas");
        if (!canvas) return;
    }
    html_dom_append(slot, canvas);
    menu_schedule_rect();
}

// #charPreviewSlot pointerdown / pointermove / pointerup: drag to turn the player. The engine
// sends mousedown to element listeners; the drag follows the cursor while the button is held.
static struct { int down; double x; } g_drag;

static int l_slot_mousedown(void *user, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) user; (void) args; (void) argc;
    if (!ev || ev->button != 0) return 1;
    Client *client = menu_ui()->client;
    double x = ev->x, y;
    if (client && client->window) glfwGetCursorPos(client->window, &x, &y);
    g_drag.down = 1;
    g_drag.x = x;
    return 1;
}

void v10_customize_tick(void) {
    v10_daily_spin_tick(); // the sidebar's Daily Spin follows the account (v10_daily_spin.c)
    static int listening;
    if (!listening && html_dom_by_id("charPreviewSlot")) {
        listening = 1;
        html_ui_set_listener("charPreviewSlot", "mousedown", l_slot_mousedown);
    }
    if (!g_drag.down) return;
    Client *client = menu_ui()->client;
    if (!client || !client->window || glfwGetMouseButton(client->window, GLFW_MOUSE_BUTTON_LEFT) != GLFW_PRESS || !html_dom_by_id("charPreviewSlot")) {
        g_drag.down = 0;
        return;
    }
    double x, y;
    glfwGetCursorPos(client->window, &x, &y);
    if (x != g_drag.x) client_class_preview_turn(client, (float) (x - g_drag.x));
    g_drag.x = x;
}

// ---------------------------------------------------------------- handlers

static int arg_int(const HtmlArg *args, const int argc, const int i, const int def) {
    if (i >= argc) return def;
    if (args[i].kind == HTML_ARG_NUM || args[i].kind == HTML_ARG_BOOL) return (int) args[i].num;
    if (args[i].kind == HTML_ARG_STR && args[i].str) return atoi(args[i].str);
    return def;
}

int v10_customize_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) ev;
    if (!name) return -1;
    if (!strcmp(name, "windows.2.tabIndex=")) { // the toolbar: windows[2].tabIndex = n; showWindow(3, true, true)
        ui_loadout_set_tab(arg_int(args, argc, 0, 1));
        return 1;
    }
    if (!strcmp(name, "goBackWindow") && arg_int(args, argc, 0, 2) == 52) { // windows[52].goBack(): showWindow(53)
        menu_show_window(53, 0);
        return 1;
    }
    if (!strcmp(name, "showTitlesWindow")) {
        menu_show_window(TITLE_WINDOW, 0);
        return 1;
    }
    if (!strcmp(name, "selectTitle")) { // saveVal("titleIndex", id); showWindow(53) unless noShow
        int id = arg_int(args, argc, 0, -1);
        if (!title_get(id) || !title_owned(id)) id = -1;
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", id);
        html_storage_set("titleIndex", buf);
        if (!arg_int(args, argc, 1, 0)) menu_show_window(53, 0);
        return 1;
    }
    if (!strcmp(name, "windows.53.clear")) { // selectTitle(-1, true, true)
        html_ui_stop_propagation();
        html_storage_set("titleIndex", "-1");
        menu_rerender_if_open(3);
        return 1;
    }
    return -1;
}
