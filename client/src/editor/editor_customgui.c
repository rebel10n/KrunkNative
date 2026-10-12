// Map editor Custom GUI component (Components > GUI): Krunker v7.1.4 ui/editor.js
// `panel.right.gui` (UI:6288-6560), the Editor methods addGUIElement / removeGUIElement /
// duplicateGUIElement (E:6357-6395), KE.selectAsset "selectImage" and the preview that
// EditorOverlay.render draws with overlayHelpers.updateCustomGUI / customGUI (EO,
// libs/overlayHelpers.js:18-69) on the editor's overlay canvas (editor_overlay.c).
// doc/v7/editor.md WP4 (section 1.9). The elements are EditorProject.gui (importMap reads them
// with UTILS.parseGUIs(gui, true), getMapExport writes parseGUIs(gui) through editor_model.c);
// the GUI limit warnings are editor.c's (warningCheck "gui").
//
// The preview is v7's canvas drawing, ported to Canvas2D here: the game's HUD (client/src/ui/
// hud_gui.c) builds page elements bound to a running Client and Map, which the editor does not
// have. Like v7's editor (no player, no game) a Player Value text shows its key and a Global
// Value text shows 0; "Div (HTML)" elements are not drawn on the canvas (v7: generateUIS runs in
// the game only).
//
// Intentional differences from v7.1.4:
// - "Element Label" has two onChange handlers in v7 and only the second (the delayed refresh)
//   exists, so the label is kept as typed until the export sanitises it; the same here.
// - "Show GUI" (EditorOverlay.enabled) is one switch for every open project, as v7's single
//   overlay; it is not saved.
#include "editor_internal.h"
#include "editor_panel_kit.h"
#include "editor_model.h"
#include <canvas2d.h>
#include <html_dom.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_enabled;     // EditorOverlay.enabled
static int g_label_timer; // delayTimer("ElementLabel" + id)

int ed_customgui_enabled(void) { return g_enabled; }

static void refresh(void) { ed_panel_right_show("gui"); }

static void refresh_timer(void *user) {
    (void) user;
    g_label_timer = 0;
    if (!strcmp(ed_panel_right_active(), "gui")) refresh();
}

static void on_refresh(EdCtl *c, const cJSON *v) { (void) c; (void) v; refresh(); }

static void on_label(EdCtl *c, const cJSON *v) {
    (void) c; (void) v;
    if (g_label_timer) html_timer_cancel(g_label_timer);
    g_label_timer = html_timer_add(1000, refresh_timer, NULL);
}

static void on_enabled(EdCtl *c, const cJSON *v) {
    (void) c;
    g_enabled = ed_json_truthy(v);
    refresh();
}

static void on_hide(EdCtl *c, const cJSON *v) {
    // customGUIs[id].nv = ~~!!value
    if (!c->json) return;
    const int on = ed_json_truthy(v); // (v may be the item replaced here)
    if (cJSON_GetObjectItemCaseSensitive(c->json, "nv")) cJSON_ReplaceItemInObjectCaseSensitive(c->json, "nv", cJSON_CreateNumber(on ? 1 : 0));
    else cJSON_AddNumberToObject(c->json, "nv", on ? 1 : 0);
}

static cJSON *gui_list(void) {
    if (!cJSON_IsArray(g_ed.proj->gui)) {
        cJSON_Delete(g_ed.proj->gui);
        g_ed.proj->gui = cJSON_CreateArray();
    }
    return g_ed.proj->gui;
}

static void changed(void) {
    ed_mark_changed();
    g_ed.dirty_counts = 1; // warningCheck("gui")
    refresh();
}

static void on_add(EdCtl *c) {
    // addGUIElement
    (void) c;
    cJSON *e = cJSON_CreateObject();
    cJSON_AddNumberToObject(e, "t", 0);
    cJSON_AddStringToObject(e, "s", "");
    cJSON_AddStringToObject(e, "v", "");
    cJSON_AddNumberToObject(e, "vt", 0);
    cJSON_AddNumberToObject(e, "w", 100);
    cJSON_AddNumberToObject(e, "h", 50);
    cJSON_AddNumberToObject(e, "x", 50);
    cJSON_AddNumberToObject(e, "y", 50);
    cJSON_AddNumberToObject(e, "f", 10);
    cJSON_AddStringToObject(e, "c", "#ff0000");
    cJSON_AddNumberToObject(e, "o", 1);
    cJSON_AddNumberToObject(e, "z", 0);
    cJSON_AddNumberToObject(e, "id", 0);
    cJSON_AddNumberToObject(e, "nv", 0);
    cJSON_AddStringToObject(e, "lbl", "");
    cJSON_AddItemToArray(gui_list(), e);
    changed();
}

static void on_remove(EdCtl *c) {
    // removeGUIElement
    cJSON *list = gui_list();
    if (c->arg >= 0 && c->arg < cJSON_GetArraySize(list)) cJSON_DeleteItemFromArray(list, c->arg);
    changed();
}

static void on_duplicate(EdCtl *c) {
    // duplicateGUIElement: a copy appended at the end
    cJSON *list = gui_list();
    const cJSON *src = cJSON_GetArrayItem(list, c->arg);
    if (src) cJSON_AddItemToArray(list, cJSON_Duplicate(src, 1));
    changed();
}

static void on_select_image(EdCtl *c) { ed_asset_target_open(ED_ASSET_GUI_IMAGE, c->arg); }

void ed_customgui_set_image(const int index, const char *id) {
    // KE.selectAsset "selectImage": customGUIs[i].s = id
    cJSON *e = cJSON_GetArrayItem(gui_list(), index);
    if (!e || !id) return;
    ed_json_set_str(e, "s", id);
    changed();
}

// a label as the folder title shows it: toString().replace(/[^\w\s]+/gmi, "")
static void clean_label(const cJSON *v, char *out, const size_t n) {
    char num[32];
    const char *s = cJSON_IsString(v) ? v->valuestring : NULL;
    if (!s && cJSON_IsNumber(v)) {
        ed_model_number(v->valuedouble, num, sizeof(num));
        s = num;
    }
    size_t j = 0;
    for (; s && *s && j + 1 < n; s++) {
        const unsigned char ch = (unsigned char) *s;
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == ' ' || ch == '\t' ||
            ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v') out[j++] = (char) ch;
    }
    out[j] = 0;
}

static int elem_int(const cJSON *e, const char *key, const int fallback) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(e, key);
    if (cJSON_IsNumber(v)) return (int) v->valuedouble;
    if (cJSON_IsString(v)) return atoi(v->valuestring);
    if (cJSON_IsBool(v)) return cJSON_IsTrue(v);
    return fallback;
}

void ed_panel_right_gui(void) {
    if (ed_project_background()) {
        g_ed.ui_stale = 1;
        return;
    }
    g_ed.panel_hidden[1] = 0;
    EdBuild b;
    if (!ed_panel_right_begin(&b, "gui")) return;
    if (g_label_timer) {
        html_timer_cancel(g_label_timer);
        g_label_timer = 0;
    }
    ed_gui_panel_buttons(&b, 1, "Custom GUIs", 0);
    ed_gui_rule(&b);
    static cJSON *flag; // {enabled}: the switch's binding (EditorOverlay.enabled)
    if (!flag) flag = cJSON_CreateObject();
    cJSON_DeleteItemFromObjectCaseSensitive(flag, "enabled");
    cJSON_AddBoolToObject(flag, "enabled", g_enabled);
    edp_json_switch(&b, "Show GUI", flag, "enabled", 0, on_enabled);
    ed_gui_button(&b, "Add Element", on_add);
    int n;
    EdOption *types = edp_opts_from_names(editor_data_get("custGUITypes"), &n);
    const int type_count = n;
    EdOption *vtypes = edp_opts_from_names(editor_data_get("inputTypesGUI"), &n);
    const int vtype_count = n;
    int i = 0;
    cJSON *e;
    cJSON_ArrayForEach(e, gui_list()) {
        char title[128], label[96], state[32], text[64];
        clean_label(cJSON_GetObjectItemCaseSensitive(e, "lbl"), label, sizeof(label));
        if (label[0]) snprintf(title, sizeof(title), "Element %d - %s", i, label);
        else snprintf(title, sizeof(title), "Element %d", i);
        snprintf(state, sizeof(state), "gui.e%d", i);
        ed_gui_folder_begin(&b, title, 0, 0, 0, state);
        snprintf(text, sizeof(text), "Remove Element %d", i);
        ed_gui_button(&b, text, on_remove)->arg = i;
        snprintf(text, sizeof(text), "Duplicate Element %d", i);
        ed_gui_button(&b, text, on_duplicate)->arg = i;
        edp_json_text(&b, "Element Label", e, "lbl", "", 1, on_label);
        edp_json_select(&b, "Type", e, "t", types, type_count, 1, on_refresh);
        const int t = elem_int(e, "t", 0);
        if (t == 1 || t == 3) {
            const int vt = elem_int(e, "vt", 0);
            edp_json_select(&b, "Value Type", e, "vt", vtypes, vtype_count, 0, on_refresh);
            edp_json_text(&b, vt ? "Key" : "Text", e, "v", vt ? "Key" : "Text", 1, NULL);
            edp_json_slider(&b, "Font Size", e, "f", 0, 100, 1, 1, NULL);
        }
        if (t == 2) {
            edp_json_text(&b, "Image Asset ID", e, "s", "ID", 1, NULL);
            ed_gui_button(&b, "Select Image", on_select_image)->arg = i;
        } else {
            edp_json_color(&b, "Color", e, "c", 1, NULL);
        }
        edp_json_slider(&b, "Opacity", e, "o", 0, 1, 0.1, 1, NULL);
        edp_json_slider(&b, "X", e, "x", 0, 100, 1, 1, NULL);
        edp_json_slider(&b, "Y", e, "y", 0, 100, 1, 1, NULL);
        edp_json_slider(&b, "Width", e, "w", 0, 2000, 1, 1, NULL);
        edp_json_slider(&b, "Height", e, "h", 0, 2000, 1, 1, NULL);
        edp_json_slider(&b, "Order Index", e, "z", 0, 2000, 1, 1, NULL);
        edp_json_switch(&b, "Hide", e, "nv", 1, on_hide);
        edp_json_slider(&b, "Element Id", e, "id", 0, 2000, 1, 1, NULL);
        ed_gui_folder_end(&b, 0);
        i++;
    }
    ed_gui_end(&b, g_ed.el_right_list);
}

// ---------------------------------------------------------------------------
// preview: overlayHelpers.updateCustomGUI(ctx, w, h, null, null, config, customGUIs)

static double elem_num(const cJSON *e, const char *key) {
    // (e.k || 0) as a number (JS coerces numeric strings in arithmetic)
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(e, key);
    if (cJSON_IsNumber(v)) return v->valuedouble;
    if (cJSON_IsString(v)) return strtod(v->valuestring, NULL);
    if (cJSON_IsBool(v)) return cJSON_IsTrue(v);
    return 0;
}

static void draw_element(Canvas2D *cv, const cJSON *e, const int width, const int height) {
    // customGUI(ctx, e, w, h)
    c2d_save(cv);
    c2d_set_global_alpha(cv, elem_num(e, "o"));
    const double w = elem_num(e, "w"), h = elem_num(e, "h");
    const double x = elem_num(e, "x") / 100.0 * width - w / 2, y = elem_num(e, "y") / 100.0 * height - h / 2;
    float rgba[4] = {1, 0, 0, 1};
    const cJSON *col = cJSON_GetObjectItemCaseSensitive(e, "c");
    if (!cJSON_IsString(col) || !col->valuestring[0] || !c2d_parse_color(col->valuestring, rgba)) c2d_parse_color("#ff0000", rgba);
    c2d_set_fill_color(cv, rgba[0], rgba[1], rgba[2], rgba[3]);
    const int t = elem_int(e, "t", 0);
    if (t == 0) c2d_fill_rect(cv, x, y, w, h);
    if (t == 1) {
        // vt 0: v || "Text"; vt 1: the key (no player in the editor); vt 2: 0 (no game)
        const int vt = elem_int(e, "vt", 0);
        char text[128] = "";
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(e, "v");
        if (vt == 2) snprintf(text, sizeof(text), "0");
        else if (cJSON_IsString(v) && v->valuestring[0]) snprintf(text, sizeof(text), "%s", v->valuestring);
        else if (cJSON_IsNumber(v) && (vt == 1 || v->valuedouble != 0)) ed_model_number(v->valuedouble, text, sizeof(text));
        else if (vt == 0) snprintf(text, sizeof(text), "Text");
        double f = elem_num(e, "f");
        if (f == 0 || f != f) f = 10;
        c2d_set_font(cv, f, "GameFont");
        const double half = (double) (long) (c2d_measure_text(cv, text) / 2 + 0.5); // Math.round(width / 2)
        c2d_fill_text(cv, text, x - half, y + f / 2, 0, 0);
    }
    if (t == 2) {
        // user-assets/<digits of s>/texture.png, drawn once loaded
        char digits[32] = "";
        const cJSON *s = cJSON_GetObjectItemCaseSensitive(e, "s");
        size_t j = 0;
        char num[32];
        const char *src = cJSON_IsString(s) ? s->valuestring : NULL;
        if (!src && cJSON_IsNumber(s)) {
            ed_model_number(s->valuedouble, num, sizeof(num));
            src = num;
        }
        for (; src && *src && j + 1 < sizeof(digits); src++) if (*src >= '0' && *src <= '9') digits[j++] = *src;
        digits[j] = 0;
        char rel[64];
        snprintf(rel, sizeof(rel), "user-assets/%s/texture.png", digits);
        char *path = concat(assets_path(), rel);
        C2DImage *img = path ? c2d_image(cv, path) : NULL;
        free(path);
        if (img && c2d_image_state(img) == 1) c2d_draw_image(cv, img, x, y, w, h);
    }
    c2d_restore(cv);
}

void ed_customgui_draw(Canvas2D *cv, const int width, const int height) {
    if (!g_enabled || !cv) return;
    const cJSON *list = g_ed.proj ? g_ed.proj->gui : NULL;
    const int n = cJSON_GetArraySize(list);
    if (n <= 0) return;
    // [...customGUIs].sort((a, b) => (a.z || 0) - (b.z || 0)): a stable sort by z
    const cJSON **order = malloc(sizeof(*order) * (size_t) n);
    if (!order) return;
    int k = 0;
    const cJSON *e;
    cJSON_ArrayForEach(e, list) order[k++] = e;
    for (int i = 1; i < k; i++) {
        const cJSON *x = order[i];
        int j = i - 1;
        while (j >= 0 && elem_num(order[j], "z") > elem_num(x, "z")) {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = x;
    }
    for (int i = 0; i < k; i++) {
        // if (e.o && !e.nv)
        if (elem_num(order[i], "o") != 0 && !elem_num(order[i], "nv")) draw_element(cv, order[i], width, height);
    }
    free(order);
}

// KRUNKNATIVE_EDITOR_SCRIPT commands of the GUI panel (editor.c run_script_text), for the parity
// tests: gui:add, gui:remove:N, gui:dup:N, gui:show:0|1, gui:set:N:<key>:<json> (a control's
// GUI.action on element N). 1 = handled.
int ed_customgui_script(const char *cmd, const char *arg) {
    if (strcmp(cmd, "gui") || !arg) return 0;
    EdCtl fake;
    memset(&fake, 0, sizeof(fake));
    if (!strcmp(arg, "add")) on_add(&fake);
    else if (!strncmp(arg, "remove:", 7)) {
        fake.arg = atoi(arg + 7);
        on_remove(&fake);
    } else if (!strncmp(arg, "dup:", 4)) {
        fake.arg = atoi(arg + 4);
        on_duplicate(&fake);
    } else if (!strncmp(arg, "show:", 5)) {
        g_enabled = atoi(arg + 5) != 0;
    } else if (!strncmp(arg, "set:", 4)) {
        char key[32];
        const int index = atoi(arg + 4);
        const char *k = strchr(arg + 4, ':');
        const char *colon = k ? strchr(k + 1, ':') : NULL;
        cJSON *e = cJSON_GetArrayItem(gui_list(), index);
        if (!e || !colon || (size_t) (colon - k - 1) >= sizeof(key)) return 1;
        snprintf(key, sizeof(key), "%.*s", (int) (colon - k - 1), k + 1);
        cJSON *v = cJSON_Parse(colon + 1);
        if (!v) return 1;
        if (cJSON_GetObjectItemCaseSensitive(e, key)) cJSON_ReplaceItemInObjectCaseSensitive(e, key, v); // keeps its place, as v7
        else cJSON_AddItemToObject(e, key, v);
        if (!strcmp(key, "nv")) {
            fake.json = e;
            on_hide(&fake, v);
        }
        ed_mark_changed();
        if (!strcmp(ed_panel_right_active(), "gui")) refresh();
    }
    return 1;
}
