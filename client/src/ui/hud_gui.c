// Krunker v7.1.4 custom map HUD: the map's "gui" array (game.js: UTILS.parseGUIs(gui, true),
// sorted by z) drawn every frame by overlayHelpers.updateCustomGUI on the overlay canvas (Box,
// Text, Image) plus game-overlay.js generateUIS's HTML divs ("Div (HTML)"). KN puts all four
// kinds into the page as absolutely positioned children of #customGUIHolder, ids "cusDIV<i>"
// (the index in the parsed list, like v7's divs), so the HTML layer composites them; ui-v7's
// HUD shows / hides the holder with #inGameUI (hud_ui.c). See client/include/hud_gui.h.
//
// Parity notes (documented differences):
// - Box / Text / Image are canvas drawings in v7 (below the HTML UI, above the 3D view); here
//   they are elements in the holder with the same centre, size, colour, opacity and z order.
//   Text keeps v7's anchor: the element's (x - w / 2, y - h / 2) like every kind (overlayHelpers
//   customGUI), text centred on it, alphabetic baseline f / 2 below it.
// - "Div (HTML)": v7 assigns style.width / height as bare numbers, which the browser ignores
//   (a v7 bug, fixed here: they are px); the click hook (KrunkScript onDIVClicked) is the
//   KrunkScript agent's.
// - Values: vt 1 reads the local player's health / maxHealth / score / classIndex
//   (config.playerVals) or its custom value (custVals, "cv"), vt 2 the global value ("gv" /
//   "gbl"); a missing key shows 0 (map-C triggers_v7.h mirrors). Visibility after "tgui"
//   toggles is map-C's client_triggers_gui_visible (map_triggers.h).
#include <hud_gui.h>
#include <html_dom.h>
#include <map_triggers.h>
#include <triggers_v7.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define HUD_GUI_MAX 80 // validateMap: gui elements <= 80

typedef struct {
    int type;       // t: 0 Box, 1 Text, 2 Image, 3 Div (HTML)
    int value_type; // vt: 0 input value, 1 player value, 2 global value
    float w, h, x, y;
    int z, id, font;
    float opacity;
    int hidden;     // nv
    char value[96]; // v
    char image[24]; // s (digits of an asset id)
    char color[8];  // c (#ff0000)
    HtmlElem elem;
    char shown[96]; // the text currently in the element
    int index;      // position in Map.gui (client_triggers_gui_visible)
    int shown_visible;
} HudGuiElement;

static HudGuiElement g_gui[HUD_GUI_MAX];
static int g_gui_count;
static const void *g_gui_map;      // the Map the elements were built for

static int gui_int(const cJSON *json, const int fallback) {
    if (cJSON_IsNumber(json)) return (int) cJSON_GetNumberValue(json);
    if (cJSON_IsString(json)) {
        char *end = NULL;
        const long v = strtol(cJSON_GetStringValue(json), &end, 10);
        if (end != cJSON_GetStringValue(json)) return (int) v;
    }
    if (cJSON_IsTrue(json)) return 1;
    return fallback;
}

static float gui_float(const cJSON *json, const float fallback) {
    if (cJSON_IsNumber(json)) return (float) cJSON_GetNumberValue(json);
    if (cJSON_IsString(json)) {
        char *end = NULL;
        const float v = strtof(cJSON_GetStringValue(json), &end);
        if (end != cJSON_GetStringValue(json)) return v;
    }
    return fallback;
}

static int gui_clamp(const int v, const int lo, const int hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

// utils.js parseGUIs "v": vt 0 keeps [\w:;()?,.|[\] -+*$@=^!{}\\], vt 1 / 2 [a-z0-9_]
static void gui_filter(const char *in, char *out, const size_t size, const int key) {
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *) (in ? in : ""); *p && n + 1 < size; p++) {
        const int word = isalnum(*p) || *p == '_';
        const int ok = key ? word : word || strchr(":;()?,.|[] -+*$@=^!{}\\", *p) != NULL;
        if (ok) out[n++] = (char) *p;
    }
    out[n] = 0;
}

static void gui_text_of(const cJSON *json, char *out, const size_t size) {
    if (cJSON_IsString(json)) snprintf(out, size, "%s", cJSON_GetStringValue(json));
    else if (cJSON_IsNumber(json)) snprintf(out, size, "%g", cJSON_GetNumberValue(json));
    else out[0] = 0;
}

static void gui_parse(const cJSON *raw, HudGuiElement *e) {
    memset(e, 0, sizeof(*e));
    e->type = gui_clamp(gui_int(cJSON_GetObjectItem(raw, "t"), 0), 0, 3);
    e->value_type = gui_clamp(gui_int(cJSON_GetObjectItem(raw, "vt"), 0), 0, 2);
    e->w = (float) gui_clamp(gui_int(cJSON_GetObjectItem(raw, "w"), 0), 0, 2000);
    e->h = (float) gui_clamp(gui_int(cJSON_GetObjectItem(raw, "h"), 0), 0, 2000);
    e->x = (float) gui_clamp(gui_int(cJSON_GetObjectItem(raw, "x"), 0), 0, 100);
    e->y = (float) gui_clamp(gui_int(cJSON_GetObjectItem(raw, "y"), 0), 0, 100);
    e->z = gui_clamp(gui_int(cJSON_GetObjectItem(raw, "z"), 0), 0, 2000);
    e->id = gui_clamp(gui_int(cJSON_GetObjectItem(raw, "id"), 0), 0, 2000);
    const int f = gui_int(cJSON_GetObjectItem(raw, "f"), 0);
    e->font = gui_clamp(f ? f : 10, 1, 100); // parseInt(f) || 10
    const float o = gui_float(cJSON_GetObjectItem(raw, "o"), 0.0f);
    e->opacity = o != 0.0f ? fminf(fmaxf(o, 0.0f), 1.0f) : 1.0f; // parseFloat(o) || 1
    e->hidden = gui_int(cJSON_GetObjectItem(raw, "nv"), 0) != 0;
    char text[128];
    gui_text_of(cJSON_GetObjectItem(raw, "v"), text, sizeof(text));
    gui_filter(text, e->value, sizeof(e->value), e->value_type != 0);
    gui_text_of(cJSON_GetObjectItem(raw, "s"), text, sizeof(text));
    size_t n = 0;
    for (const char *p = text; *p && n + 1 < sizeof(e->image); p++) {
        if (*p >= '0' && *p <= '9') e->image[n++] = *p;
    }
    e->image[n] = 0;
    gui_text_of(cJSON_GetObjectItem(raw, "c"), text, sizeof(text));
    int hex_ok = text[0] == '#' && (strlen(text) == 7 || strlen(text) == 4);
    for (const char *p = text + 1; hex_ok && *p; p++) hex_ok = isxdigit((unsigned char) *p);
    snprintf(e->color, sizeof(e->color), "%s", hex_ok ? text : "#ff0000");
}

// JS String(number) for the values (integers without decimals)
static void gui_number(const double v, char *out, const size_t size) {
    snprintf(out, size, "%.16g", v);
}

static void gui_value_text(const Client *client, const HudGuiElement *e, char *out, const size_t size) {
    if (e->value_type == 0) {
        snprintf(out, size, "%s", e->value[0] ? e->value : "Text"); // v || "Text"
        return;
    }
    snprintf(out, size, "0");
    if (e->value_type == 1) {
        const Player *me = client ? client->me : NULL;
        if (!me) return;
        if (!strcmp(e->value, "health")) gui_number(me->health, out, size);
        else if (!strcmp(e->value, "maxHealth")) gui_number(me->max_health, out, size);
        else if (!strcmp(e->value, "score")) gui_number(me->score, out, size);
        else if (!strcmp(e->value, "classIndex")) gui_number(me->class_index, out, size);
        else { // custVals[v] || 0: a string shows as it is ("" is falsy: 0; deob-10.0.3.js:177319, :180099)
            const TriggerValue *v = trigger_values_find(&me->custom_values, e->value);
            if (v && v->is_text) snprintf(out, size, "%s", v->text[0] ? v->text : "0");
            else gui_number(v ? v->value : 0.0, out, size);
        }
        return;
    }
    if (!client) return;
    const TriggerValue *g = trigger_values_find(trigger_global_values(&client->game), e->value); // globalVals[v] != null ? : 0
    if (g && g->is_text) snprintf(out, size, "%s", g->text);
    else gui_number(g ? g->value : 0.0, out, size);
}

static HtmlElem gui_holder(void) {
    HtmlElem holder = html_dom_by_id("customGUIHolder");
    if (holder) return holder;
    // the v3.1.1 page has none (ui-v7 imports v7's, where it is the first child of #gameUI)
    HtmlElem parent = html_dom_by_id("gameUI");
    if (!parent) parent = html_dom_body();
    if (!parent) return 0;
    holder = html_dom_create("div", "customGUIHolder");
    if (!holder) return 0;
    html_dom_append(parent, holder);
    return holder;
}

static void gui_px(HtmlElem elem, const char *prop, const float value) {
    char text[32];
    snprintf(text, sizeof(text), "%gpx", (double) value);
    html_dom_set_style(elem, prop, text);
}

static void gui_build(const Client *client, const Map *map) {
    hud_gui_clear();
    g_gui_map = map;
    const cJSON *gui = map ? map->gui : NULL;
    if (!cJSON_IsArray(gui) || !cJSON_GetArraySize(gui)) return;
    const HtmlElem holder = gui_holder();
    if (!holder) return;

    const cJSON *raw;
    int index = 0;
    cJSON_ArrayForEach(raw, gui) {
        if (g_gui_count >= HUD_GUI_MAX) break;
        gui_parse(raw, &g_gui[g_gui_count]);
        g_gui[g_gui_count++].index = index++;
    }
    // customGUIs.sort((a, b) => (a.z || 0) - (b.z || 0)) (stable)
    for (int i = 1; i < g_gui_count; i++) {
        HudGuiElement e = g_gui[i];
        int j = i - 1;
        while (j >= 0 && g_gui[j].z > e.z) {
            g_gui[j + 1] = g_gui[j];
            j--;
        }
        g_gui[j + 1] = e;
    }

    for (int i = 0; i < g_gui_count; i++) {
        HudGuiElement *e = &g_gui[i];
        char id[24], text[96];
        snprintf(id, sizeof(id), "cusDIV%d", i);
        e->elem = html_dom_create(e->type == 2 ? "img" : "div", id);
        if (!e->elem) continue;
        html_dom_append(holder, e->elem);
        html_dom_set_style(e->elem, "position", "absolute");
        char percent[16];
        snprintf(percent, sizeof(percent), "%g%%", (double) e->x);
        html_dom_set_style(e->elem, "left", percent);
        snprintf(percent, sizeof(percent), "%g%%", (double) e->y);
        html_dom_set_style(e->elem, "top", percent);
        snprintf(text, sizeof(text), "%g", (double) e->opacity);
        html_dom_set_style(e->elem, "opacity", text);
        snprintf(text, sizeof(text), "%d", e->z);
        html_dom_set_style(e->elem, "z-index", text);
        html_dom_set_style(e->elem, "pointer-events", e->type == 3 ? "auto" : "none");
        switch (e->type) {
            case 0: // fillRect(x - w / 2, y - h / 2, w, h)
                gui_px(e->elem, "width", e->w);
                gui_px(e->elem, "height", e->h);
                gui_px(e->elem, "margin-left", -e->w / 2.0f);
                gui_px(e->elem, "margin-top", -e->h / 2.0f);
                html_dom_set_style(e->elem, "background-color", e->color);
                break;
            case 1: // fillText(text, x - w / 2 - round(width / 2), y - h / 2 + f / 2), "<f>px GameFont"
                gui_px(e->elem, "font-size", (float) e->font);
                gui_px(e->elem, "line-height", (float) e->font);
                gui_px(e->elem, "margin-left", -e->w / 2.0f);
                gui_px(e->elem, "margin-top", -0.3f * (float) e->font - e->h / 2.0f); // baseline (~0.8 f below the top) at y - h / 2 + f / 2
                html_dom_set_style(e->elem, "font-family", "GameFont");
                html_dom_set_style(e->elem, "white-space", "pre");
                html_dom_set_style(e->elem, "color", e->color);
                html_dom_set_style(e->elem, "transform", "translateX(-50%)");
                break;
            case 2: { // drawImage(user-assets/<s>/texture.png, x - w / 2, y - h / 2, w, h)
                char path[64], url[256];
                snprintf(path, sizeof(path), "user-assets/%s/texture.png", e->image);
                html_dom_asset_url(path, url, sizeof(url));
                html_dom_set_attr(e->elem, "src", url);
                gui_px(e->elem, "width", e->w);
                gui_px(e->elem, "height", e->h);
                gui_px(e->elem, "margin-left", -e->w / 2.0f);
                gui_px(e->elem, "margin-top", -e->h / 2.0f);
                break;
            }
            default: // generateUIS: font-size f, colour c, translate(-50%, -50%)
                gui_px(e->elem, "width", e->w);
                gui_px(e->elem, "height", e->h);
                gui_px(e->elem, "font-size", (float) e->font);
                html_dom_set_style(e->elem, "color", e->color);
                html_dom_set_style(e->elem, "transform", "translate(-50%, -50%)");
                html_dom_set_text(e->elem, "0"); // innerHTML = 0 until the first update
                snprintf(e->shown, sizeof(e->shown), "0");
                break;
        }
        // canvas kinds draw only when o && !nv; divs: display none while nv
        html_dom_set_display(e->elem, e->hidden ? "none" : "block");
        e->shown_visible = !e->hidden;
    }
    (void) client;
}

void hud_gui_update(Client *client) {
    const Map *map = client ? client->game.map : NULL;
    if (map != g_gui_map) gui_build(client, map);
    for (int i = 0; i < g_gui_count; i++) {
        HudGuiElement *e = &g_gui[i];
        if (!e->elem) continue;
        const int visible = client_triggers_gui_visible(client, e->index);
        if (visible != e->shown_visible) {
            html_dom_set_display(e->elem, visible ? "block" : "none");
            e->shown_visible = visible;
        }
        if (e->type != 1 && e->type != 3) continue;
        char text[96];
        gui_value_text(client, e, text, sizeof(text));
        if (strcmp(text, e->shown)) {
            html_dom_set_text(e->elem, text);
            snprintf(e->shown, sizeof(e->shown), "%s", text);
        }
    }
}

void hud_gui_clear(void) {
    for (int i = 0; i < g_gui_count; i++) {
        if (g_gui[i].elem) html_dom_remove(g_gui[i].elem);
    }
    g_gui_count = 0;
    g_gui_map = NULL;
}
