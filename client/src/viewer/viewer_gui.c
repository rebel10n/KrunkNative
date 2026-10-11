// dat.gui port for the viewer page (see viewer_gui.h). Krunker's viewer builds its panel with
// `new dat.GUI()`, `gui.width = 322` (322 + 1, then the constructor's deferred `width -= 1`:
// the panel ends up 321px wide), folders that start closed until .open(), and controllers
// whose rows dat.gui classes by the bound value's type ("number" / "string" / "boolean" /
// "function").
#include "viewer_gui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VG_MAX_CTRLS 256
#define VG_MAX_FOLDERS 32
#define VG_CLOSE_BUTTON_HEIGHT 20 // dat.gui onResize: innerHeight - top - 20
#define VG_RESIZE_DEBOUNCE_MS 50.0

struct VgFolder {
    int id;
    int used;
    int closed;
    VgFolder *parent;
    HtmlElem li;  // li.folder (0 for the root)
    HtmlElem ul;
};

struct VgCtrl {
    int id;
    int used;
    enum { VG_BOOL, VG_OPTION, VG_FUNCTION } kind;
    VgFolder *folder;
    HtmlElem li;
    HtmlElem input; // checkbox / select
    int *bool_value;
    VgValue *value;
    VgFn fn;
    VgFn on_change;
};

static struct {
    int ready;
    int next_id;
    int hidden;
    int resize_timer;
    int skip_row;          // vgCheck ran for this click: the row's click must not toggle again
    HtmlElem container;    // .dg.ac
    HtmlElem main;         // .dg.main.a
    VgFolder folders[VG_MAX_FOLDERS];
    VgCtrl ctrls[VG_MAX_CTRLS];
} g_vg;

static void html_escape(const char *in, char *out, const size_t n) {
    size_t o = 0;
    for (; *in && o + 7 < n; in++) {
        const char *rep = NULL;
        switch (*in) {
            case '&': rep = "&amp;"; break;
            case '<': rep = "&lt;"; break;
            case '>': rep = "&gt;"; break;
            case '"': rep = "&quot;"; break;
            default: break;
        }
        if (rep) {
            const size_t l = strlen(rep);
            memcpy(out + o, rep, l);
            o += l;
        } else {
            out[o++] = *in;
        }
    }
    out[o] = 0;
}

static VgCtrl *ctrl_by_id(const int id) {
    for (int i = 0; i < VG_MAX_CTRLS; i++) {
        if (g_vg.ctrls[i].used && g_vg.ctrls[i].id == id) return &g_vg.ctrls[i];
    }
    return NULL;
}

static VgFolder *folder_by_id(const int id) {
    for (int i = 0; i < VG_MAX_FOLDERS; i++) {
        if (g_vg.folders[i].used && g_vg.folders[i].id == id) return &g_vg.folders[i];
    }
    return NULL;
}

static HtmlElem find_marked(const char *attr, const int id) {
    char selector[48];
    snprintf(selector, sizeof(selector), "[%s=\"%d\"]", attr, id);
    return html_dom_query(0, selector);
}

// onResize(): when the rows are taller than the window the root ul gets a fixed height and the
// panel scrolls (class taller-than-window), else height auto.
static void vg_on_resize(void *user) {
    (void) user;
    g_vg.resize_timer = 0;
    if (!g_vg.ready) return;

    VgFolder *root = &g_vg.folders[0];
    HtmlRect rect = {0};
    html_dom_get_rect(root->ul, &rect);

    float total = 0.0f;
    for (HtmlElem child = html_dom_first_child(root->ul); child; child = html_dom_next(child)) {
        total += html_dom_offset_height(child);
    }

    float vw = 0.0f, vh = 0.0f, dpr = 1.0f;
    html_dom_viewport(&vw, &vh, &dpr);

    const float room = vh - rect.y - VG_CLOSE_BUTTON_HEIGHT;
    if (getenv("KRUNKNATIVE_VIEWER_DEBUG")) fprintf(stderr, "[viewer] gui resize: rows %.0f px, room %.0f px\n", total, room);
    if (room < total) {
        char height[32];
        snprintf(height, sizeof(height), "%dpx", (int) room);
        html_dom_set_class(g_vg.main, "taller-than-window", 1);
        html_dom_set_style(root->ul, "height", height);
    } else {
        html_dom_set_class(g_vg.main, "taller-than-window", 0);
        html_dom_set_style(root->ul, "height", "auto");
    }
}

void vg_resize(void) {
    if (!g_vg.ready) return;
    if (g_vg.resize_timer) html_timer_cancel(g_vg.resize_timer);
    g_vg.resize_timer = html_timer_add(VG_RESIZE_DEBOUNCE_MS, vg_on_resize, NULL);
}

void vg_init(const int width) {
    vg_fini();
    memset(&g_vg, 0, sizeof(g_vg));

    char html[256];
    snprintf(html, sizeof(html), "<div class=\"dg ac\"><div class=\"dg main a\" style=\"width: %dpx;\"><ul></ul></div></div>", width - 1);
    html_dom_insert_html(html_dom_body(), HTML_INSERT_BEFORE_END, html);

    g_vg.container = html_dom_query(0, ".dg.ac");
    g_vg.main = html_dom_query(g_vg.container, ".dg.main");

    VgFolder *root = &g_vg.folders[0];
    root->used = 1;
    root->id = ++g_vg.next_id;
    root->ul = html_dom_query(g_vg.main, "ul");
    g_vg.ready = g_vg.container && root->ul;
    vg_on_resize(NULL);
}

void vg_fini(void) {
    if (g_vg.resize_timer) html_timer_cancel(g_vg.resize_timer);
    if (g_vg.ready && g_vg.container) html_dom_remove(g_vg.container);
    memset(&g_vg, 0, sizeof(g_vg));
}

VgFolder *vg_root(void) {
    return g_vg.ready ? &g_vg.folders[0] : NULL;
}

VgFolder *vg_add_folder(VgFolder *parent, const char *name) {
    if (!parent) return NULL;

    VgFolder *folder = NULL;
    for (int i = 1; i < VG_MAX_FOLDERS && !folder; i++) {
        if (!g_vg.folders[i].used) folder = &g_vg.folders[i];
    }
    if (!folder) return NULL;

    memset(folder, 0, sizeof(*folder));
    folder->used = 1;
    folder->id = ++g_vg.next_id;
    folder->parent = parent;
    folder->closed = 1;

    char escaped[128];
    char html[512];
    html_escape(name, escaped, sizeof(escaped));
    snprintf(html, sizeof(html),
             "<li class=\"folder\" data-vgf=\"%d\"><div class=\"dg\"><ul class=\"closed\"><li class=\"title\" onclick=\"vgTitle(%d)\">%s</li></ul></div></li>",
             folder->id, folder->id, escaped);
    html_dom_insert_html(parent->ul, HTML_INSERT_BEFORE_END, html);

    folder->li = find_marked("data-vgf", folder->id);
    folder->ul = folder->li ? html_dom_query(folder->li, "ul") : 0;
    vg_resize();
    return folder;
}

static void folder_set_closed(VgFolder *folder, const int closed) {
    folder->closed = closed;
    if (folder->ul) html_dom_set_class(folder->ul, "closed", closed);
    vg_resize();
}

void vg_folder_open(VgFolder *folder) {
    if (folder) folder_set_closed(folder, 0);
}

void vg_folder_remove(VgFolder *parent, VgFolder *folder) {
    (void) parent;
    if (!folder || !folder->used) return;

    for (int i = 0; i < VG_MAX_CTRLS; i++) {
        if (g_vg.ctrls[i].used && g_vg.ctrls[i].folder == folder) g_vg.ctrls[i].used = 0;
    }
    for (int i = 1; i < VG_MAX_FOLDERS; i++) {
        if (g_vg.folders[i].used && g_vg.folders[i].parent == folder) vg_folder_remove(folder, &g_vg.folders[i]);
    }
    if (folder->li) html_dom_remove(folder->li);
    folder->used = 0;
    vg_resize();
}

static VgCtrl *ctrl_new(VgFolder *folder) {
    for (int i = 0; i < VG_MAX_CTRLS; i++) {
        if (!g_vg.ctrls[i].used) {
            VgCtrl *ctrl = &g_vg.ctrls[i];
            memset(ctrl, 0, sizeof(*ctrl));
            ctrl->used = 1;
            ctrl->id = ++g_vg.next_id;
            ctrl->folder = folder;
            return ctrl;
        }
    }
    return NULL;
}

// dat.gui's row: <li class="cr TYPE"><div><span class="property-name">NAME</span><div class="c">WIDGET</div></div></li>
static void ctrl_insert(VgCtrl *ctrl, const char *row_class, const char *name, const char *widget, const int row_click) {
    char escaped[192];
    html_escape(name, escaped, sizeof(escaped));

    const size_t length = strlen(widget) + strlen(escaped) + 256;
    char *html = malloc(length);
    if (!html) return;

    char click[48] = "";
    if (row_click) snprintf(click, sizeof(click), " onclick=\"vgRow(%d)\"", ctrl->id);
    snprintf(html, length, "<li class=\"cr %s\" data-vg=\"%d\"%s><div><span class=\"property-name\">%s</span><div class=\"c\">%s</div></div></li>",
             row_class, ctrl->id, click, escaped, widget);
    html_dom_insert_html(ctrl->folder->ul, HTML_INSERT_BEFORE_END, html);
    free(html);

    ctrl->li = find_marked("data-vg", ctrl->id);
    vg_resize();
}

VgCtrl *vg_add_bool(VgFolder *folder, int *value, const char *name, const VgFn on_change) {
    if (!folder) return NULL;
    VgCtrl *ctrl = ctrl_new(folder);
    if (!ctrl) return NULL;

    ctrl->kind = VG_BOOL;
    ctrl->bool_value = value;
    ctrl->on_change = on_change;

    char widget[128];
    snprintf(widget, sizeof(widget), "<input type=\"checkbox\" onclick=\"vgCheck(%d, this.checked)\"%s>", ctrl->id, *value ? " checked=\"checked\"" : "");
    ctrl_insert(ctrl, "boolean", name, widget, 1);
    ctrl->input = ctrl->li ? html_dom_query(ctrl->li, "input") : 0;
    return ctrl;
}

VgCtrl *vg_add_function(VgFolder *folder, const VgFn fn, const char *name) {
    if (!folder) return NULL;
    VgCtrl *ctrl = ctrl_new(folder);
    if (!ctrl) return NULL;

    ctrl->kind = VG_FUNCTION;
    ctrl->fn = fn;
    ctrl_insert(ctrl, "function", name, "<div class=\"button\"></div>", 1);
    return ctrl;
}

static void option_select_value(VgCtrl *ctrl) {
    if (!ctrl->input) return;
    char text[48];
    if (ctrl->value->is_string) snprintf(text, sizeof(text), "%s", ctrl->value->str);
    else snprintf(text, sizeof(text), "%d", ctrl->value->num);
    html_dom_set_value(ctrl->input, text);
}

VgCtrl *vg_add_options(VgFolder *folder, VgValue *value, const char *name, const int count,
                       const char *const *labels, const char *const *values, const VgFn on_change) {
    if (!folder) return NULL;
    VgCtrl *ctrl = ctrl_new(folder);
    if (!ctrl) return NULL;

    ctrl->kind = VG_OPTION;
    ctrl->value = value;
    ctrl->on_change = on_change;

    size_t length = 128;
    for (int i = 0; i < count; i++) length += strlen(labels[i]) * 6 + strlen(values[i]) * 6 + 40;

    char *widget = malloc(length);
    if (!widget) return ctrl;

    size_t o = (size_t) snprintf(widget, length, "<select onchange=\"vgSelect(%d, this.value)\">", ctrl->id);
    for (int i = 0; i < count; i++) {
        char label[256], val[96];
        html_escape(labels[i], label, sizeof(label));
        html_escape(values[i], val, sizeof(val));
        o += (size_t) snprintf(widget + o, length - o, "<option value=\"%s\">%s</option>", val, label);
    }
    snprintf(widget + o, length - o, "</select>");

    ctrl_insert(ctrl, value->is_string ? "string" : "number", name, widget, 0);
    free(widget);

    ctrl->input = ctrl->li ? html_dom_query(ctrl->li, "select") : 0;
    option_select_value(ctrl);
    return ctrl;
}

void vg_remove(VgFolder *folder, VgCtrl *ctrl) {
    (void) folder;
    if (!ctrl || !ctrl->used) return;
    if (ctrl->li) html_dom_remove(ctrl->li);
    ctrl->used = 0;
    vg_resize();
}

void vg_update_display(void) {
    for (int i = 0; i < VG_MAX_CTRLS; i++) {
        VgCtrl *ctrl = &g_vg.ctrls[i];
        if (!ctrl->used || !ctrl->input) continue;

        if (ctrl->kind == VG_BOOL) {
            if (*ctrl->bool_value) html_dom_set_attr(ctrl->input, "checked", "checked");
            html_dom_set_checked(ctrl->input, *ctrl->bool_value);
        } else if (ctrl->kind == VG_OPTION) {
            option_select_value(ctrl);
        }
    }
}

void vg_toggle_hide(void) {
    if (!g_vg.ready) return;
    g_vg.hidden = !g_vg.hidden;
    html_dom_set_display(g_vg.main, g_vg.hidden ? "none" : "");
}

int vg_contains(const float x, const float y) {
    if (!g_vg.ready || g_vg.hidden) return 0;
    HtmlRect rect = {0};
    if (!html_dom_get_rect(g_vg.main, &rect)) return 0;
    return x >= rect.x && x < rect.x + rect.w && y >= rect.y && y < rect.y + rect.h;
}

// ---- events

static int arg_id(const HtmlArg *args, const int argc) {
    return argc > 0 && args[0].kind == HTML_ARG_NUM ? (int) args[0].num : -1;
}

static void bool_set(VgCtrl *ctrl, const int value) {
    *ctrl->bool_value = value;
    if (ctrl->input) {
        if (value) html_dom_set_attr(ctrl->input, "checked", "checked");
        html_dom_set_checked(ctrl->input, value);
    }
    if (ctrl->on_change) ctrl->on_change();
}

// li click: a boolean row clicks its checkbox (fakeEvent), a function row calls the function
static int h_vg_row(void *user, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) user;
    (void) ev;
    VgCtrl *ctrl = ctrl_by_id(arg_id(args, argc));

    if (g_vg.skip_row) {
        g_vg.skip_row = 0;
        return 1;
    }
    if (!ctrl) return 1;

    if (ctrl->kind == VG_BOOL) bool_set(ctrl, !*ctrl->bool_value);
    else if (ctrl->kind == VG_FUNCTION && ctrl->fn) ctrl->fn();
    return 1;
}

// the checkbox itself: setValue(checkbox.checked); stopPropagation (the row must not click it again)
static int h_vg_check(void *user, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) user;
    VgCtrl *ctrl = ctrl_by_id(arg_id(args, argc));
    g_vg.skip_row = 1;
    if (!ctrl || ctrl->kind != VG_BOOL) return 1;

    int checked = ev ? ev->checked : !*ctrl->bool_value;
    if (argc > 1 && args[1].kind == HTML_ARG_BOOL) checked = args[1].num != 0.0;
    bool_set(ctrl, checked);
    return 1;
}

// <select> change: setValue(select.value) - the property becomes the option's string
static int h_vg_select(void *user, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) user;
    VgCtrl *ctrl = ctrl_by_id(arg_id(args, argc));
    if (!ctrl || ctrl->kind != VG_OPTION) return 1;

    const char *text = argc > 1 && args[1].kind == HTML_ARG_STR && args[1].str ? args[1].str : ev && ev->value ? ev->value : "";
    snprintf(ctrl->value->str, sizeof(ctrl->value->str), "%s", text);
    ctrl->value->is_string = 1;
    ctrl->value->num = (int) strtol(text, NULL, 10);
    if (ctrl->on_change) ctrl->on_change();
    return 1;
}

static int h_vg_title(void *user, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) user;
    (void) ev;
    VgFolder *folder = folder_by_id(arg_id(args, argc));
    if (folder) folder_set_closed(folder, !folder->closed);
    return 0; // preventDefault
}

void vg_register_handlers(HtmlHandlerEntry *table, int *count) {
    table[(*count)++] = (HtmlHandlerEntry) {"vgRow", h_vg_row};
    table[(*count)++] = (HtmlHandlerEntry) {"vgCheck", h_vg_check};
    table[(*count)++] = (HtmlHandlerEntry) {"vgSelect", h_vg_select};
    table[(*count)++] = (HtmlHandlerEntry) {"vgTitle", h_vg_title};
}
