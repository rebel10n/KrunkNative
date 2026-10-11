// Map editor: the right-click "Quick GUI" (editor.js initQuickGUI / updateQuickGUI over
// libs/quicksettings.js, blueprint ui/editor.js GUI._quicksettings), the hitbox fixer
// (fixHitbox / plotLine, Shift+F) and the object painter (Shift+click with Grouping >
// Object Painter on: replaceObject(fakeSelected(hit + 0.5 y), objectPainterJson, ...)).
//
// The menu is QuickSettings' markup (div.qs_main > .qs_title_bar + .qs_content >
// .qs_container) appended to <body>, positioned in CSS px at the click like
// QuickSettings.setPosition; quicksettings.js' injected stylesheet is in editor-kn.css.
// The items call KE.quickGUI.action(<index>) instead of GUI.action(['_quicksettings', ...]).
//
// Differences from v3.1.1 (intentional):
// - "Fix Hitbox" in the menu calls fixHitbox(menu object); v3.1.1 reads this.quickGUI on
//   the blueprint and throws a TypeError, so the item did nothing there.
// - Duplicate / Delete keep v3.1.1's behaviour: they test quickGUI.obj.objType, which the
//   bounding mesh never has, so a group placeholder is duplicated / removed as an object.
// - Group > Stop calls stopGroup(false, obj), whose second argument v3.1.1 ignores: it
//   stops the selected group, as here.
#include "editor_internal.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    QG_DUPLICATE, QG_DELETE, QG_GROUP_CREATE, QG_GROUP_STOP, QG_GROUP_COPY, QG_GROUP_PASTE,
    QG_FIX_HITBOX, QG_REPLACE_JSON, QG_REPLACE_FILE, QG_TEXTAREA,
};

static HtmlElem g_qs;              // div.qs_main (0 until first use)
static int g_qs_hidden = 1;
static uint32_t g_qs_obj;          // quickGUI.obj (the object's uuid)
static int g_qs_top_z = 3;         // QuickSettings._topZ
static int g_qs_dragging;
static float g_qs_x, g_qs_y, g_qs_drag_dx, g_qs_drag_dy;

// ---------------------------------------------------------------------------
// fixHitbox / plotLine

static double num_at(const cJSON *o, const char *key, const int i) {
    const cJSON *v = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(o, key), i);
    return cJSON_IsNumber(v) ? v->valuedouble : 0.0;
}

static cJSON *box(const double x, const double y, const double z, const double sx, const double sy, const double sz) {
    cJSON *o = cJSON_CreateObject();
    const double p[3] = {x, y, z}, s[3] = {sx, sy, sz};
    cJSON_AddItemToObject(o, "p", cJSON_CreateDoubleArray(p, 3));
    cJSON_AddItemToObject(o, "s", cJSON_CreateDoubleArray(s, 3));
    return o;
}

// plotLine(e, t, n, i, r, a): a Bresenham staircase of 1-wide boxes from (e, t) to (n, i)
static cJSON *plot_line(const double e, const double t, const double n, const double i, const double r, const double a) {
    const double s = n - e, o = i - t;
    double l = o * 2 - s, c = t;
    cJSON *d = cJSON_CreateArray();
    for (double x = e; x < n; x++) {
        cJSON_AddItemToArray(d, box(x, c, r, 1, 1, cJSON_GetArraySize(d) % 5 == 0 ? a : a - 2));
        if (l > 0) {
            ++c;
            l -= s * 2;
        }
        l += o * 2;
    }
    return d;
}

static void change_angle(cJSON *arr) {
    // changeAngle: swap x/z of position and size
    cJSON *o;
    cJSON_ArrayForEach(o, arr) {
        cJSON *p = cJSON_GetObjectItemCaseSensitive(o, "p"), *s = cJSON_GetObjectItemCaseSensitive(o, "s");
        const double p0 = num_at(o, "p", 0), p2 = num_at(o, "p", 2), s0 = num_at(o, "s", 0), s2 = num_at(o, "s", 2);
        if (cJSON_GetArraySize(p) >= 3) {
            cJSON_ReplaceItemInArray(p, 0, cJSON_CreateNumber(p2));
            cJSON_ReplaceItemInArray(p, 2, cJSON_CreateNumber(p0));
        }
        if (cJSON_GetArraySize(s) >= 3) {
            cJSON_ReplaceItemInArray(s, 0, cJSON_CreateNumber(s2));
            cJSON_ReplaceItemInArray(s, 2, cJSON_CreateNumber(s0));
        }
    }
}

static const char k_fix_vehicle[] = "[{\"p\":[0,0,0],\"s\":[47,9,17],\"v\":1},{\"p\":[5,9,0],\"s\":[26,6,17],\"v\":1}]";
static const char k_fix_tree[] =
    "[{\"p\":[313,10,-44],\"s\":[10,55,10],\"v\":1},{\"p\":[292,68,-37],\"s\":[22,16,19],\"v\":1},{\"p\":[329,74,-54],\"s\":[28,16,23],\"v\":1},"
    "{\"p\":[313,65,-45],\"s\":[22,16,19],\"v\":1},{\"p\":[315,81,-45],\"s\":[60,35,49],\"v\":1}]";
static const char k_fix_containerr[] =
    "[{\"p\":[0,0,0],\"s\":[57,1,25],\"c\":11739168},{\"p\":[0,24,0],\"s\":[57,2,25],\"pe\":1,\"v\":1},"
    "{\"p\":[0,0,-12],\"s\":[57,25,1],\"pe\":1,\"v\":1},{\"p\":[0,0,12],\"s\":[57,25,1],\"pe\":1,\"v\":1}]";

void ed_fix_hitbox(EditorObject *e) {
    // fixHitbox(e = objectSelected()): replaceObject(e, hitbox JSON, true, objType)
    if (!(e = e ? e : g_ed.selected)) return;
    const char *type = editor_prefabs[e->prefab].name;
    if (!strcmp(type, "VEHICLE")) ed_obj_replace(e, k_fix_vehicle, type, 0);
    else if (!strcmp(type, "TREE")) ed_obj_replace(e, k_fix_tree, type, 0);
    else if (!strcmp(type, "CONTAINERR")) ed_obj_replace(e, k_fix_containerr, type, 0);
    else if (!strcmp(type, "RAMP")) {
        cJSON *a = ed_obj_serialize(e);
        const int s = (int) ed_json_num(a, "d", 0);
        const double o = num_at(a, "p", 1), l = num_at(a, "s", 1) + num_at(a, "p", 1);
        if (s != 0 && s != 2) {
            cJSON *one = cJSON_CreateArray();
            cJSON_AddItemToArray(one, a);
            change_angle(one);
            a = cJSON_DetachItemFromArray(one, 0);
            cJSON_Delete(one);
        }
        const double t = num_at(a, "p", 0), n = num_at(a, "p", 2), i = num_at(a, "s", 0) + t, r = num_at(a, "s", 2);
        cJSON_Delete(a);
        cJSON *c = plot_line(t, o, i, l, n, r);
        ed_merge_objects(c);
        if (s == 2) ed_reflect_array(c, 0);
        if (s == 1 || s == 3) change_angle(c);
        if (s == 3) ed_reflect_array(c, 2);
        char *text = cJSON_PrintUnformatted(c);
        cJSON_Delete(c);
        if (text) ed_obj_replace(e, text, type, 0);
        free(text);
    }
}

// ---------------------------------------------------------------------------
// object painter

int ed_object_painter_click(const vec3 hit) {
    // shift + left click with Object Painter on and a JSON: 1 = handled
    const char *json = ed_json_str(g_ed.advanced, "objectPainterJson", "");
    if (!ed_json_truthy(cJSON_GetObjectItemCaseSensitive(g_ed.advanced, "objectPainter")) || !json[0]) return 0;
    // fakeSelected(x, y + 0.5, z): owner.position = position = the point, scale 1, no
    // rotation. replaceObject places at owner.position.y - scale.y / 2, which is the
    // serialized p.y of a unit box centred there. The fake is never in the scene, so
    // removeObject() on it (createGroup) removes nothing.
    cJSON *d = cJSON_CreateObject();
    const double p[3] = {hit.x, hit.y + 0.5 - 0.5, hit.z}, s[3] = {1, 1, 1};
    cJSON_AddItemToObject(d, "p", cJSON_CreateDoubleArray(p, 3));
    cJSON_AddItemToObject(d, "s", cJSON_CreateDoubleArray(s, 3));
    EditorObject *fake = ed_obj_new(d, 0);
    cJSON_Delete(d);
    if (!fake) return 1;
    char *copy = malloc(strlen(json) + 1);
    if (copy) {
        memcpy(copy, json, strlen(json) + 1);
        ed_obj_replace(fake, copy, "", editor_setting_bool("assetAutoGroup")); // i = true: e stays
        free(copy);
    }
    ed_obj_free(fake);
    return 1;
}

// ---------------------------------------------------------------------------
// the Quick GUI

static void qs_item(EdBuf *b, const char *name, const int action) {
    edb_addf(b, "<div class=\"toolbarDropDownItem\" onclick=\"KE.quickGUI.action(%d, this)\">%s</div>", action, name);
}

static void qs_menu_begin(EdBuf *b, const char *name) {
    edb_addf(b, "<div class=\"toolbarDropDownItem\" onmouseover=\"GUI.update.toolbar(this, true)\" onmouseout=\"GUI.update.toolbar(this)\">\n"
                "                  %s<div class=\"toolbarSubDropDown\" style=\"top: this.offsetTop\">", name);
}

// GUI._quicksettings.gen(): the blueprint as _build writes it
static void qs_build(EdBuf *b, const char *json) {
    qs_item(b, "Duplicate", QG_DUPLICATE); edb_add(b, "\n");
    qs_item(b, "Delete", QG_DELETE); edb_add(b, "\n<hr>\n");
    qs_menu_begin(b, "Group");
    qs_item(b, "Create", QG_GROUP_CREATE); edb_add(b, "\n");
    qs_item(b, "Stop", QG_GROUP_STOP); edb_add(b, "\n<hr>\n");
    qs_item(b, "Copy", QG_GROUP_COPY); edb_add(b, "\n");
    qs_item(b, "Paste", QG_GROUP_PASTE);
    edb_add(b, "</div></div>\n<hr>\n");
    qs_item(b, "Fix Hitbox", QG_FIX_HITBOX); edb_add(b, "\n<hr>\n");
    qs_menu_begin(b, "Replace");
    qs_item(b, "JSON", QG_REPLACE_JSON); edb_add(b, "\n");
    qs_item(b, "File", QG_REPLACE_FILE);
    edb_add(b, "</div></div>\n<hr>\n");
    qs_menu_begin(b, "JSON");
    edb_addf(b, "<div class=\"toolbarDropDownItem textarea\" onclick=\"KE.quickGUI.action(%d, this)\"><textarea class=\"qs_textarea\" rows=\"5\">", QG_TEXTAREA);
    edb_add_escaped(b, json);
    edb_add(b, "</textarea></div></div></div>");
}

static void qs_set_position(const float x, const float y) {
    char v[32];
    g_qs_x = x;
    g_qs_y = fmaxf(y, 0.0f);
    snprintf(v, sizeof(v), "%gpx", g_qs_x);
    html_dom_set_style(g_qs, "left", v);
    snprintf(v, sizeof(v), "%gpx", g_qs_y);
    html_dom_set_style(g_qs, "top", v);
}

static void qs_create(void) {
    // QuickSettings.create(0, 0, "Menu"); setCollapsible(false); hide(); setWidth(150)
    if (g_qs) return;
    html_dom_insert_html(html_dom_body(), HTML_INSERT_BEFORE_END,
                         "<div id=\"null\" class=\"qs_main\" data-kn-pointer=\"2\" style=\"z-index: 2; left: 0px; top: 0px; visibility: hidden; width: 150px;\">"
                         "<div id=\"null\" class=\"qs_title_bar\" style=\"cursor: pointer;\" onmousedown=\"KE.quickGUI.drag(this)\">Menu</div>"
                         "<div id=\"null\" class=\"qs_content\" style=\"width: 150px;\"><div id=\"null\" class=\"qs_container\"></div></div></div>");
    HtmlElem all[8];
    const int n = html_dom_query_all(html_dom_body(), ":scope > .qs_main", all, 8);
    g_qs = n > 0 ? all[n - 1] : html_dom_query(html_dom_body(), ".qs_main");
    if (!g_qs) fprintf(stderr, "[editor] quick GUI: no .qs_main after insert\n");
    g_qs_hidden = 1;
}

int ed_quickgui_hidden(void) { return g_qs_hidden; }

void ed_quickgui_hide(void) {
    if (!g_qs || g_qs_hidden) return;
    html_dom_set_style(g_qs, "visibility", "hidden");
    g_qs_hidden = 1;
    g_qs_dragging = 0;
}

void ed_quickgui_reset(void) {
    // page leave: the element goes with the page
    g_qs = 0;
    g_qs_hidden = 1;
    g_qs_obj = 0;
    g_qs_dragging = 0;
}

void ed_quickgui_show(EditorObject *o, const double fb_x, const double fb_y) {
    // updateQuickGUI(event, point, object)
    if (!o || !editor_setting_bool("quickGUI")) return;
    qs_create();
    if (!g_qs) return;
    g_qs_obj = o->uuid;
    html_dom_set_text(html_dom_query(g_qs, ".qs_title_bar"), editor_prefabs[o->prefab].label);
    // getJSON(obj): the group's members for a placeholder that owns one, else the object
    cJSON *arr = cJSON_CreateArray();
    const EditorGroup *g = ed_group_of(o);
    if (g) {
        for (size_t i = 0; i < g_ed.count; i++) {
            for (size_t k = 0; k < g->count; k++) {
                if (g->objects[k] == g_ed.objects[i]->uuid) {
                    cJSON_AddItemToArray(arr, ed_obj_serialize(g_ed.objects[i]));
                    break;
                }
            }
        }
    } else {
        cJSON_AddItemToArray(arr, ed_obj_serialize(o));
    }
    char *json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    EdBuf b;
    edb_init(&b);
    qs_build(&b, json ? json : "[]");
    free(json);
    html_dom_set_html(html_dom_query(g_qs, ".qs_container"), b.buf ? b.buf : "");
    edb_free(&b);
    const float s = g_ed.css_to_fb > 0 ? g_ed.css_to_fb : 1;
    const float cx = (float) (fb_x / s), cy = (float) (fb_y / s);
    float vw = 0, vh = 0, dpr = 1;
    html_dom_viewport(&vw, &vh, &dpr);
    float h = html_dom_offset_height(g_qs);
    if (h <= 0) h = 238.0f; // not laid out yet: the menu's height at the default font
    qs_set_position(cx, vh - cy < h ? cy - h : cy);
    char z[16];
    snprintf(z, sizeof(z), "%d", ++g_qs_top_z);
    html_dom_set_style(g_qs, "z-index", z);
    html_dom_set_style(g_qs, "visibility", "visible");
    g_qs_hidden = 0;
}

// container mousedown with the Quick GUI open button (Settings > Quick GUI > Open On:
// 0 middle, 1 right): `which == quickGUIOpen + 2 && button == quickGUIOpen + 1`
int ed_quickgui_button(const int glfw_button) {
    if (!editor_setting_bool("quickGUI")) return 0;
    const int open = (int) editor_setting_num("quickGUIOpen", 1);
    return (open == 0 && glfw_button == 2) || (open == 1 && glfw_button == 1); // GLFW middle 2, right 1
}

void ed_quickgui_cursor(const double fb_x, const double fb_y) {
    if (!g_qs_dragging || !g_qs) return;
    const float s = g_ed.css_to_fb > 0 ? g_ed.css_to_fb : 1;
    qs_set_position((float) (fb_x / s) - g_qs_drag_dx, (float) (fb_y / s) - g_qs_drag_dy);
}

void ed_quickgui_release(void) { g_qs_dragging = 0; }

static int h_qs_drag(void *user, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) user; (void) args; (void) argc;
    // QuickSettings._startDrag (title bar): the panel follows the pointer until mouseup
    if (!g_qs || g_qs_hidden) return 1;
    const float s = g_ed.css_to_fb > 0 ? g_ed.css_to_fb : 1;
    const float x = ev && ev->x ? (float) ev->x : (float) (g_ed.mouse_x / s);
    const float y = ev && ev->y ? (float) ev->y : (float) (g_ed.mouse_y / s);
    g_qs_drag_dx = x - g_qs_x;
    g_qs_drag_dy = y - g_qs_y;
    g_qs_dragging = 1;
    return 1;
}

static void replace_into(const int file, EditorObject *o) {
    // jsonInput(file, obj)
    ed_json_input_for(file, o);
}

static int h_qs_action(void *user, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) user; (void) ev;
    const int action = argc >= 1 && args[0].kind == HTML_ARG_NUM ? (int) args[0].num : -1;
    // GUI.action "toolbar": the open Quick GUI hides unless it is the textarea item
    if (action == QG_TEXTAREA) return 1;
    ed_quickgui_hide();
    EditorObject *o = ed_obj_find(g_qs_obj);
    if (!o) return 1;
    switch (action) {
        case QG_DUPLICATE: ed_duplicate_object(o); break;
        case QG_DELETE: ed_obj_remove(o, 0, 1); break;
        case QG_GROUP_CREATE: ed_copy_objects(0, 1, o); break;
        case QG_GROUP_STOP: ed_group_stop(0); break;
        case QG_GROUP_COPY: ed_copy_objects(0, 0, o); break;
        case QG_GROUP_PASTE: ed_paste_objects(o); break;
        case QG_FIX_HITBOX: ed_fix_hitbox(o); break;
        case QG_REPLACE_JSON: replace_into(0, o); break;
        case QG_REPLACE_FILE: replace_into(1, o); break;
        default: break;
    }
    return 1;
}

void ed_quickgui_install(HtmlHandlerEntry *table, size_t *count, const size_t max) {
    const HtmlHandlerEntry entries[] = {
        {"KE.quickGUI.action", h_qs_action},
        {"KE.quickGUI.drag", h_qs_drag},
    };
    for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]) && *count < max; i++) table[(*count)++] = entries[i];
}
