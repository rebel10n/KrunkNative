// Map editor GUI builder: Krunker v3.1.1 ui/editor.js `_html`, `_build`, `update` and
// `action`, ported to C. Panels are built imperatively into Krunker's exact markup; every
// control is registered in a per-panel table and its inline handlers call
// `GUI.action(<id>, this)` (Krunker passes the blueprint path as an array literal, which
// the native handler parser does not take; the id indexes the same thing).
//
// The Pickr colour picker (libs/pickr.min.js, theme "nano", hue only, no opacity) is
// rebuilt here as markup with Pickr's class names so css/pickr.css styles it.
#include "editor_internal.h"
#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ED_PANELS 4
#define ED_MAX_CTLS 2048

typedef struct {
    EdCtl ctls[ED_MAX_CTLS];
    int count;
} EdPanel;

static EdPanel g_panels[ED_PANELS];
static cJSON *g_folder_store; // folderStore: path -> closed

// ---------------------------------------------------------------------------
// string buffer

void edb_init(EdBuf *b) { memset(b, 0, sizeof(*b)); }
void edb_free(EdBuf *b) { free(b->buf); memset(b, 0, sizeof(*b)); }

static void edb_reserve(EdBuf *b, const size_t extra) {
    if (b->len + extra + 1 <= b->cap) return;
    size_t cap = b->cap ? b->cap : 4096;
    while (cap < b->len + extra + 1) cap *= 2;
    char *n = realloc(b->buf, cap);
    if (!n) return;
    b->buf = n;
    b->cap = cap;
}

void edb_add(EdBuf *b, const char *s) {
    if (!s) return;
    const size_t n = strlen(s);
    edb_reserve(b, n);
    if (b->len + n + 1 > b->cap) return;
    memcpy(b->buf + b->len, s, n + 1);
    b->len += n;
}

void edb_addf(EdBuf *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char small[1024];
    const int n = vsnprintf(small, sizeof(small), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t) n < sizeof(small)) {
        edb_add(b, small);
        return;
    }
    char *big = malloc((size_t) n + 1);
    if (!big) return;
    va_start(ap, fmt);
    vsnprintf(big, (size_t) n + 1, fmt, ap);
    va_end(ap);
    edb_add(b, big);
    free(big);
}

void edb_add_escaped(EdBuf *b, const char *s) {
    for (; s && *s; s++) {
        switch (*s) {
            case '&': edb_add(b, "&amp;"); break;
            case '<': edb_add(b, "&lt;"); break;
            case '>': edb_add(b, "&gt;"); break;
            case '"': edb_add(b, "&quot;"); break;
            case '\'': edb_add(b, "&#39;"); break;
            default: {
                char c[2] = {*s, 0};
                edb_add(b, c);
            }
        }
    }
}

// JS Number -> string (`"" + n`): integers without a fraction, otherwise the shortest
// form that reads back the same (good enough at 12 significant digits for the GUI).
static void num_str(const double v, char *out, const size_t n) {
    if (isnan(v)) {
        snprintf(out, n, "NaN");
        return;
    }
    if (v == floor(v) && fabs(v) < 1e15) {
        snprintf(out, n, "%.0f", v);
        return;
    }
    for (int p = 1; p <= 17; p++) {
        snprintf(out, n, "%.*g", p, v);
        if (strtod(out, NULL) == v || p >= 12) break;
    }
}

// ---------------------------------------------------------------------------
// controls

void ed_gui_reset_panel(const int panel) {
    if (panel < 0 || panel >= ED_PANELS) return;
    g_panels[panel].count = 0;
}

EdCtl *ed_gui_ctl(const int panel, const int kind) {
    EdPanel *p = &g_panels[panel];
    if (p->count >= ED_MAX_CTLS) p->count = ED_MAX_CTLS - 1; // never overflow; the last slot is reused
    EdCtl *c = &p->ctls[p->count];
    memset(c, 0, sizeof(*c));
    c->id = panel * 10000 + p->count;
    c->kind = kind;
    p->count++;
    return c;
}

static EdCtl *ctl_by_id(const int id) {
    const int panel = id / 10000, index = id % 10000;
    if (panel < 0 || panel >= ED_PANELS || index < 0 || index >= g_panels[panel].count) return NULL;
    return &g_panels[panel].ctls[index];
}

void ed_ctl_bind_json(EdCtl *c, cJSON *obj, const char *key) {
    if (!c) return;
    c->bind = EB_JSON;
    c->json = obj;
    snprintf(c->key, sizeof(c->key), "%s", key);
}

void ed_ctl_bind_obj(EdCtl *c, const char *prop) {
    if (!c) return;
    c->bind = EB_OBJ;
    snprintf(c->key, sizeof(c->key), "%s", prop);
}

void ed_gui_begin(EdBuild *b, const int panel) {
    memset(b, 0, sizeof(*b));
    b->panel = panel;
    edb_init(&b->html);
    ed_gui_reset_panel(panel);
    if (!g_folder_store) g_folder_store = cJSON_CreateObject();
}

void ed_gui_end(EdBuild *b, const HtmlElem target) {
    if (target) html_dom_set_html(target, b->html.buf ? b->html.buf : "");
    edb_free(&b->html);
}

// ---------------------------------------------------------------------------
// _html: markup (ui/editor.js)

void ed_gui_panel_buttons(EdBuild *b, const int right, const char *label, const int blank) {
    // _html.panelButtons(direction, label, blank); left panel: [lock, label, hide], right: [hide, label, lock]
    const char *dir = right ? "right" : "left";
    EdBuf hide;
    edb_init(&hide);
    edb_addf(&hide, "<div class=\"%s panelButton\" style=\"%s\" onclick=\"KE.resizable.hide(this)\">\n"
                    "                <div class=\"hide\"><i class=\"material-icons hide\">keyboard_arrow_%s</i></div></div>",
             right ? "left" : "right", blank ? "border-bottom-right-radius:5px" : "", dir);
    EdBuf lock;
    edb_init(&lock);
    if (blank) edb_add(&lock, "<div></div>");
    else {
        extern int ed_panel_locked(int right);
        edb_addf(&lock, "<div class=\"%s panelButton\" onclick=\"GUI.panel.state.lock(this, '%s')\">\n"
                        "               <div class=\"lock\"\"><i class=\"material-icons lock\">lock%s</i></div></div>",
                 dir, dir, ed_panel_locked(right) ? "" : "_open");
    }
    EdBuf lab;
    edb_init(&lab);
    if (blank) edb_add(&lab, "<div></div>");
    else {
        edb_add(&lab, "<div class='panelLabel'>");
        edb_add_escaped(&lab, label);
        edb_add(&lab, "</div>");
    }
    edb_add(&b->html, "<div class=\"panelButtonLayout\">");
    if (!right) {
        edb_add(&b->html, lock.buf);
        edb_add(&b->html, lab.buf);
        edb_add(&b->html, hide.buf);
    } else {
        edb_add(&b->html, hide.buf);
        edb_add(&b->html, lab.buf);
        edb_add(&b->html, lock.buf);
    }
    edb_add(&b->html, "</div>\n");
    edb_free(&hide);
    edb_free(&lock);
    edb_free(&lab);
}

void ed_gui_rule(EdBuild *b) { edb_add(&b->html, "<hr>\n"); }

static void lower_id(const char *name, char *out, const size_t n) {
    size_t i = 0;
    for (; name[i] && i + 1 < n; i++) out[i] = (char) tolower((unsigned char) name[i]);
    out[i] = 0;
}

void ed_gui_folder_begin(EdBuild *b, const char *name, const int small, const int closed_default, const int sub, const char *state_key) {
    EdCtl *c = ed_gui_ctl(b->panel, small ? EG_FOLDER_SMALL : EG_FOLDER_STANDARD);
    snprintf(c->path, sizeof(c->path), "%d.%s", b->panel, state_key ? state_key : name);
    int closed = closed_default;
    const cJSON *stored = cJSON_GetObjectItemCaseSensitive(g_folder_store, c->path);
    if (editor_setting_bool("preserveFolderState") && cJSON_IsBool(stored)) closed = cJSON_IsTrue(stored);
    else ed_json_set_bool(g_folder_store, c->path, closed);
    char id[96];
    lower_id(name, id, sizeof(id));
    if (!small) {
        edb_addf(&b->html, "<div id=\"%sH\" class=\"header\" onclick=\"GUI.update.folder(this);GUI.action(%d, this)\">\n"
                           "                  <i class=\"material-icons\">keyboard_arrow_%s</i><a>", id, c->id, closed ? "right" : "down");
        edb_add_escaped(&b->html, name);
        edb_addf(&b->html, "</a></div><hr>\n                  <div id=\"%sB\" style=\"%s\">", id, closed ? "display:none" : "");
    } else {
        edb_addf(&b->html, "<div id=\"%sH\" class=\"%ssubheader\" style=\"cursor: pointer\"\n"
                           "                  onclick=\"GUI.update.folder(this);GUI.action(%d, this)\"><i class=\"material-icons\">keyboard_arrow_%s</i><a>",
                 id, sub ? "sub" : "", c->id, closed ? "right" : "down");
        edb_add_escaped(&b->html, name);
        edb_addf(&b->html, "</a></div>\n                  <div id=\"%sB\" style=\"%s\">", id, closed ? "display:none" : "");
    }
}

void ed_gui_folder_end(EdBuild *b, const int small) {
    edb_add(&b->html, small ? "</div>\n" : "<hr></div>\n");
}

static void line_begin(EdBuild *b, const char *name, const int sub) {
    static const char *subs[] = {"", "subsection", "subsubsection"};
    edb_addf(&b->html, "<div class=\"inlineItem\"><div class=\"section %s\">", sub >= 1 && sub <= 2 ? subs[sub] : "");
    edb_add_escaped(&b->html, name);
    edb_add(&b->html, "</div>\n                  <div>");
}

static void line_end(EdBuild *b) { edb_add(&b->html, "</div></div>\n"); }

// _html.line.switch (v7): the switch rows use an 85% / 15% grid
static void line_switch_begin(EdBuild *b, const char *name, const int sub) {
    static const char *subs[] = {"", "subsection", "subsubsection"};
    edb_addf(&b->html, "<div class=\"inlineItem\" style=\"grid-template-columns: calc(85%% - 5px) calc(15%% - 5px);\">\n"
                       "                  <div class=\"section %s\">", sub >= 1 && sub <= 2 ? subs[sub] : "");
    edb_add_escaped(&b->html, name);
    edb_add(&b->html, "</div>\n                  <div>");
}

typedef struct { double min, max, step; } SliderArgs;
static SliderArgs g_slider_args[ED_PANELS][ED_MAX_CTLS];
static const EdOption *g_select_opts[ED_PANELS][ED_MAX_CTLS];
static int g_select_count[ED_PANELS][ED_MAX_CTLS];
static const char *g_ctl_names[ED_PANELS][ED_MAX_CTLS];

EdCtl *ed_gui_text(EdBuild *b, const char *name, const char *placeholder, const int sub) {
    EdCtl *c = ed_gui_ctl(b->panel, EG_TEXT);
    c->arg = sub;
    snprintf(c->path, sizeof(c->path), "%s", placeholder ? placeholder : "");
    g_ctl_names[b->panel][c->id % 10000] = name;
    return c;
}

EdCtl *ed_gui_number(EdBuild *b, const char *name, const char *placeholder, const double min, const int sub) {
    EdCtl *c = ed_gui_ctl(b->panel, EG_NUMBER);
    c->arg = sub;
    if (min == min) snprintf(c->path, sizeof(c->path), "%s|%g", placeholder ? placeholder : "", min);
    else snprintf(c->path, sizeof(c->path), "%s|", placeholder ? placeholder : ""); // NaN: no min (min="")
    g_ctl_names[b->panel][c->id % 10000] = name;
    return c;
}

EdCtl *ed_gui_color(EdBuild *b, const char *name, const int sub) {
    EdCtl *c = ed_gui_ctl(b->panel, EG_COLOR);
    c->arg = sub;
    g_ctl_names[b->panel][c->id % 10000] = name;
    return c;
}

EdCtl *ed_gui_switch(EdBuild *b, const char *name, const int sub) {
    EdCtl *c = ed_gui_ctl(b->panel, EG_SWITCH);
    c->arg = sub;
    g_ctl_names[b->panel][c->id % 10000] = name;
    return c;
}


EdCtl *ed_gui_slider(EdBuild *b, const char *name, const double min, const double max, const double step, const int sub) {
    EdCtl *c = ed_gui_ctl(b->panel, EG_SLIDER);
    c->arg = sub;
    g_slider_args[b->panel][c->id % 10000] = (SliderArgs) {min, max, step};
    g_ctl_names[b->panel][c->id % 10000] = name;
    return c;
}

EdCtl *ed_gui_select(EdBuild *b, const char *name, const EdOption *options, const int count, const int sub) {
    EdCtl *c = ed_gui_ctl(b->panel, EG_SELECT);
    c->arg = sub;
    g_select_opts[b->panel][c->id % 10000] = options;
    g_select_count[b->panel][c->id % 10000] = count;
    g_ctl_names[b->panel][c->id % 10000] = name;
    return c;
}

EdCtl *ed_gui_button(EdBuild *b, const char *label, const EdActionFn fn) {
    EdCtl *c = ed_gui_ctl(b->panel, EG_BUTTON);
    c->action = fn;
    // line "wide": <div>button</div>
    edb_addf(&b->html, "<div><div class=\"eButton\" onclick=\"GUI.action(%d, this)\">", c->id); // v7: eButton (v3: button)
    edb_add_escaped(&b->html, label);
    edb_add(&b->html, "</div></div>\n");
    return c;
}

// a button in a standard line: _html.line.standard(name, _html.input.resizable.button(...))
EdCtl *ed_gui_button_line(EdBuild *b, const char *name, const char *label, const EdActionFn fn, const int sub) {
    EdCtl *c = ed_gui_ctl(b->panel, EG_BUTTON);
    c->action = fn;
    line_begin(b, name, sub);
    edb_addf(&b->html, "<div class=\"eButton\" onclick=\"GUI.action(%d, this)\">", c->id);
    edb_add_escaped(&b->html, label);
    edb_add(&b->html, "</div>");
    line_end(b);
    return c;
}

EdCtl *ed_gui_vector3(EdBuild *b, const char *name, const double min) {
    EdCtl *c = ed_gui_ctl(b->panel, EG_VECTOR3);
    snprintf(c->path, sizeof(c->path), "%s", min == min ? "0" : "");
    g_ctl_names[b->panel][c->id % 10000] = name;
    return c;
}

EdCtl *ed_gui_key(EdBuild *b, const char *name, const int editable, const char *const *extra, const int extra_count, const int sub) {
    EdCtl *c = ed_gui_ctl(b->panel, EG_KEY);
    c->arg = sub | (editable ? 0x100 : 0);
    g_ctl_names[b->panel][c->id % 10000] = name;
    (void) extra;
    (void) extra_count;
    return c;
}

// The value a control shows: from its binding.
static cJSON *ctl_value(const EdCtl *c) {
    if (c->bind == EB_JSON && c->json) {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(c->json, c->key);
        return v ? cJSON_Duplicate(v, 1) : cJSON_CreateNull();
    }
    if (c->bind == EB_OBJ && g_ed.proj->selected) return ed_prop_get(g_ed.proj->selected, c->key);
    return cJSON_CreateNull();
}

static double value_double(const cJSON *v) {
    if (cJSON_IsNumber(v)) return cJSON_GetNumberValue(v);
    if (cJSON_IsBool(v)) return cJSON_IsTrue(v);
    if (cJSON_IsString(v)) return strtod(v->valuestring, NULL);
    return 0.0;
}

static const char *value_string(const cJSON *v, char *tmp, const size_t n) {
    if (cJSON_IsString(v)) return v->valuestring;
    if (cJSON_IsNumber(v)) {
        num_str(cJSON_GetNumberValue(v), tmp, n);
        return tmp;
    }
    if (cJSON_IsBool(v)) return cJSON_IsTrue(v) ? "true" : "false";
    return "";
}

static int parse_color(const char *s) {
    if (!s || s[0] != '#') return 0;
    return (int) strtol(s + 1, NULL, 16);
}

void ed_gui_emit_value(EdBuild *b, EdCtl *c) {
    // Writes the control's markup with its current value. Called by the panels right
    // after binding; name/sub come from the ed_gui_* call (kept in g_ctl_names).
    cJSON *v = ctl_value(c);
    char tmp[64];
    const int index = c->id % 10000;
    const char *name = g_ctl_names[b->panel][index] ? g_ctl_names[b->panel][index] : "";
    const int sub = c->arg & 0xff;
    switch (c->kind) {
        case EG_TEXT: {
            line_begin(b, name, sub);
            edb_add(&b->html, "<input class=\"inlineInput\" type=\"text\" placeholder=\"");
            edb_add_escaped(&b->html, c->path);
            edb_add(&b->html, "\" value=\"");
            edb_add_escaped(&b->html, value_string(v, tmp, sizeof(tmp)));
            edb_addf(&b->html, "\" oninput=\"GUI.action(%d, this)\" onchange=\"GUI.update.field(this)\">", c->id);
            line_end(b);
            break;
        }
        case EG_NUMBER: {
            char placeholder[64] = "";
            const char *bar = strchr(c->path, '|');
            if (bar) snprintf(placeholder, sizeof(placeholder), "%.*s", (int) (bar - c->path), c->path);
            line_begin(b, name, sub);
            num_str(value_double(v), tmp, sizeof(tmp));
            edb_add(&b->html, "<input class=\"inlineInput\" type=\"number\" placeholder=\"");
            edb_add_escaped(&b->html, placeholder);
            edb_addf(&b->html, "\" value=\"%s\" min=\"%s\" max=\"\" step=\"\"\n"
                               "                     onmousedown=\"GUI.update.number.mousedown(this)\" onchange=\"GUI.update.field(this);GUI.update.number.change(this);GUI.action(%d, this)\">",
                     tmp, bar ? bar + 1 : "", c->id);
            line_end(b);
            break;
        }
        case EG_COLOR: {
            const char *col = cJSON_IsString(v) ? v->valuestring : "#ffffff";
            line_begin(b, name, sub);
            edb_addf(&b->html, "<input class=\"color\" style=\"background-color:%s;color:%s\"\n"
                               "                     type=\"text\" spellcheck=\"false\" value=\"%s\" onchange=\"GUI.update.field(this);GUI.update.color.change(this);GUI.action(%d, this)\"\n"
                               "                     onmousedown=\"GUI.update.color.mousedown(this)\">",
                     col, parse_color(col) < 7829367 ? "#ffffff" : "#000000", col, c->id);
            line_end(b);
            break;
        }
        case EG_SWITCH:
            line_switch_begin(b, name, sub);
            edb_addf(&b->html, "<label class=\"switch\" style=\"margin-top: 10px\"><input type=\"checkbox\"\n"
                               "                     onclick=\"GUI.action(%d, this)\" %s><span class=\"slider\"></span></label>",
                     c->id, ed_json_truthy(v) ? "checked" : "");
            line_end(b);
            break;
        case EG_SLIDER: {
            const SliderArgs *a = &g_slider_args[b->panel][index];
            char mn[32], mx[32], st[32];
            num_str(a->min, mn, sizeof(mn));
            num_str(a->max, mx, sizeof(mx));
            num_str(a->step, st, sizeof(st));
            num_str(value_double(v), tmp, sizeof(tmp));
            line_begin(b, name, sub);
            edb_addf(&b->html, "<div style=\"zoom:0.7;margin-top:12px;\">\n"
                               "                     <input type=\"number\" class=\"sliderVal\" min=\"%s\" max=\"%s\" value=\"%s\"\n"
                               "                     onchange=\"GUI.update.field(this);GUI.update.slider.val(this);GUI.action(%d, this)\"><div class=\"slidecontainer\"><input type=\"range\"\n"
                               "                     min=\"%s\" max=\"%s\" step=\"%s\" value=\"%s\" class=\"sliderM\" oninput=\"GUI.update.slider.M(this);GUI.action(%d, this)\">\n"
                               "                     </div></div>",
                     mn, mx, tmp, c->id, mn, mx, st, tmp, c->id);
            line_end(b);
            break;
        }
        case EG_SELECT: {
            const EdOption *opts = g_select_opts[b->panel][index];
            const int count = g_select_count[b->panel][index];
            line_begin(b, name, sub);
            edb_addf(&b->html, "<select onchange=\"GUI.action(%d, this)\">", c->id);
            for (int i = 0; i < count; i++) {
                const EdOption *o = &opts[i];
                char val[64];
                const char *value = o->svalue ? o->svalue : (num_str(o->value, val, sizeof(val)), val);
                int sel;
                if (o->svalue) sel = cJSON_IsString(v) && !strcmp(v->valuestring, o->svalue);
                else sel = value_double(v) == o->value;
                if (i) edb_add(&b->html, ",");
                edb_add(&b->html, "<option value=\"");
                edb_add_escaped(&b->html, value);
                edb_addf(&b->html, "\"\n                     %s>", sel ? "selected" : "");
                edb_add_escaped(&b->html, o->name);
                edb_add(&b->html, "</option>");
            }
            edb_add(&b->html, "</select>");
            line_end(b);
            break;
        }
        case EG_VECTOR3: {
            static const char *xyz[3] = {"X", "Y", "Z"};
            edb_add(&b->html, "<div class=\"section\">");
            edb_add_escaped(&b->html, name);
            edb_add(&b->html, "</div><div class=\"vector3\">\n                     ");
            for (int i = 0; i < 3; i++) {
                num_str(value_double(cJSON_GetArrayItem(v, i)), tmp, sizeof(tmp));
                edb_addf(&b->html, "%s<input type=\"number\" placeholder=\"%s\" min=\"%s\" max=\"\" step=\"\"\n"
                                   "                     onmousedown=\"GUI.update.number.mousedown(this)\" onchange=\"GUI.update.field(this);GUI.update.number.change(this);GUI.action(%d, this)\"\n"
                                   "                     value=\"%s\">",
                         i ? "\n" : "", xyz[i], c->path, c->id, tmp);
            }
            edb_add(&b->html, "</div>\n");
            break;
        }
        case EG_KEY: {
            const int editable = (c->arg & 0x100) != 0;
            line_begin(b, name, sub);
            edb_addf(&b->html, "<div class=\"inlineLabel %s\" href=\"#\" ", editable ? "" : "disabled");
            if (editable) edb_addf(&b->html, "onclick=\"GUI.action(%d, this)\"", c->id);
            edb_add(&b->html, ">");
            edb_add_escaped(&b->html, value_string(v, tmp, sizeof(tmp)));
            if (c->path[0]) {
                edb_add(&b->html, " or "); // [z, ...B].join(" or ")
                edb_add(&b->html, c->path);
            }
            edb_add(&b->html, "</div>");
            line_end(b);
            break;
        }
        default:
            break;
    }
    cJSON_Delete(v);
}

// ---------------------------------------------------------------------------
// GUI.update.* handlers

static HtmlElem child_at(const HtmlElem parent, int index) {
    HtmlElem e = html_dom_first_child(parent);
    while (e && index-- > 0) e = html_dom_next(e);
    return e;
}

static int h_update_field(void *user, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) user; (void) args; (void) argc;
    html_dom_blur(ev->target); // GUI.update.field: z.blur()
    return 1;
}

static int h_update_folder(void *user, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) user; (void) args; (void) argc;
    char id[128];
    if (html_dom_get_attr(ev->target, "id", id, sizeof(id)) < 0) return 1;
    const size_t len = strlen(id);
    if (!len) return 1;
    id[len - 1] = 'B';
    const HtmlElem body = html_dom_by_id(id);
    char display[32] = "";
    html_dom_get_style(body, "display", display, sizeof(display));
    const int was_closed = !strcmp(display, "none");
    html_dom_set_text(html_dom_first_child(ev->target), was_closed ? "keyboard_arrow_down" : "keyboard_arrow_right");
    html_dom_set_display(body, was_closed ? "" : "none");
    return 1;
}

static int h_update_number_change(void *user, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) user; (void) args; (void) argc;
    char v[64] = "";
    html_dom_get_value(ev->target, v, sizeof(v));
    if (!v[0]) html_dom_set_value(ev->target, "0");
    return 1;
}

// GUI.update.number.mousedown/mousemove/mouseup: dragging a number input changes it
static HtmlElem g_number_drag;
static double g_number_drag_y;

static int h_update_number_mousedown(void *user, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) user; (void) args; (void) argc;
    g_number_drag = ev->target;
    // the event's own position: on the web GLFW never sees the pointer over the page
    g_number_drag_y = ev->y > 0 ? ev->y * (g_ed.css_to_fb > 0 ? g_ed.css_to_fb : 1) : g_ed.mouse_y;
    return 1;
}

void ed_gui_number_drag(const double fb_y, const int released) {
    if (!g_number_drag) return;
    if (released) {
        g_number_drag = 0;
        return;
    }
    const double dy = (fb_y - g_number_drag_y) / (g_ed.css_to_fb > 0 ? g_ed.css_to_fb : 1);
    if (fabs(dy) < 1.0) return;
    g_number_drag_y = fb_y;
    char v[64] = "", step[32] = "", mn[32] = "", mx[32] = "";
    html_dom_get_value(g_number_drag, v, sizeof(v));
    html_dom_get_attr(g_number_drag, "step", step, sizeof(step));
    html_dom_get_attr(g_number_drag, "min", mn, sizeof(mn));
    html_dom_get_attr(g_number_drag, "max", mx, sizeof(mx));
    const double s = strtod(step, NULL);
    double value = strtod(v, NULL) - dy * (s ? s : 1.0);
    if (mn[0] && value < strtod(mn, NULL)) value = strtod(mn, NULL);
    if (mx[0] && value > strtod(mx, NULL)) value = strtod(mx, NULL);
    char out[64];
    num_str(ed_round(value, 4), out, sizeof(out));
    html_dom_set_value(g_number_drag, out);
    // A.onchange(): the attribute's handler runs with the new value
    char onchange[256];
    if (html_dom_get_attr(g_number_drag, "onchange", onchange, sizeof(onchange)) > 0) {
        const char *p = strstr(onchange, "GUI.action(");
        if (p) {
            const int id = atoi(p + 11);
            HtmlEvent e = {.type = "change", .target = g_number_drag, .value = out};
            HtmlArg a[2] = {{HTML_ARG_NUM, id, NULL}, {HTML_ARG_THIS, 0, NULL}};
            html_ui_dispatch("GUI.action", &e, a, 2);
        }
    }
}

static int h_update_slider_val(void *user, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) user; (void) args; (void) argc;
    // z.parentElement.children[1].children[0].value = clamp(z.value); z.value = that
    const HtmlElem range = html_dom_first_child(child_at(html_dom_parent(ev->target), 1));
    char v[64] = "", mn[32] = "", mx[32] = "";
    html_dom_get_value(ev->target, v, sizeof(v));
    html_dom_get_attr(ev->target, "min", mn, sizeof(mn));
    html_dom_get_attr(ev->target, "max", mx, sizeof(mx));
    double value = strtod(v, NULL);
    if (mx[0] && value > strtod(mx, NULL)) value = strtod(mx, NULL);
    if (mn[0] && value < strtod(mn, NULL)) value = strtod(mn, NULL);
    char out[64];
    num_str(value, out, sizeof(out));
    if (range) html_dom_set_value(range, out);
    html_dom_set_value(ev->target, out);
    return 1;
}

static int h_update_slider_m(void *user, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) user; (void) args; (void) argc;
    // z.parentElement.parentElement.children[0].value = z.value
    const HtmlElem number = html_dom_first_child(html_dom_parent(html_dom_parent(ev->target)));
    if (number) html_dom_set_value(number, ev->value);
    return 1;
}

#define TOOLBAR_PENDING 8
static struct { HtmlElem item, drop; float x, y; } g_toolbar_pending[TOOLBAR_PENDING];

static int is_descendant(HtmlElem e, const HtmlElem ancestor) {
    for (; e; e = html_dom_parent(e)) if (e == ancestor) return 1;
    return 0;
}

static void toolbar_hide_check(void *user) {
    (void) user;
    for (int i = 0; i < TOOLBAR_PENDING; i++) {
        if (!g_toolbar_pending[i].item) continue;
        // the pointer position of the mouseout event (CSS px; GLFW sees no motion over the
        // page on the web)
        const HtmlElem under = html_dom_element_at(g_toolbar_pending[i].x, g_toolbar_pending[i].y);
        if (!is_descendant(under, g_toolbar_pending[i].item)) html_dom_set_display(g_toolbar_pending[i].drop, "");
        g_toolbar_pending[i].item = 0;
    }
}

static void toolbar_defer_hide(const HtmlElem item, const HtmlElem drop, const float x, const float y) {
    int slot = -1;
    for (int i = 0; i < TOOLBAR_PENDING; i++) {
        if (g_toolbar_pending[i].item == item) return;
        if (!g_toolbar_pending[i].item && slot < 0) slot = i;
    }
    if (slot < 0) {
        html_dom_set_display(drop, "");
        return;
    }
    g_toolbar_pending[slot].item = item;
    g_toolbar_pending[slot].drop = drop;
    g_toolbar_pending[slot].x = x;
    g_toolbar_pending[slot].y = y;
    html_timer_add(0, toolbar_hide_check, NULL);
}

static int h_update_toolbar(void *user, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) user;
    // GUI.update.toolbar(z, show): z.children[0] is the text node in Krunker's markup? No:
    // children[] skips text nodes, so it is the drop-down div.
    const int show = argc >= 2 && args[1].kind == HTML_ARG_BOOL && args[1].num != 0;
    HtmlElem drop = html_dom_first_child(ev->target);
    char tag[16];
    while (drop && html_dom_tag(drop, tag, sizeof(tag)) >= 0 && strcmp(tag, "div")) drop = html_dom_next(drop);
    if (!drop) return 1;
    if (!show) {
        // onmouseout: hide once the pointer has really left the item and its drop-down. In a
        // browser the mouseout is followed by the mouseover of the child the pointer moved
        // to (which shows the menu again); the native engine re-targets after the hide, so
        // decide on the next timer tick from what is under the pointer.
        toolbar_defer_hide(ev->target, drop, ev->x, ev->y);
        return 1;
    }
    html_dom_set_display(drop, "block");
    if (show && html_dom_has_class(drop, "toolbarSubDropDown")) {
        char top[32] = "";
        if (html_dom_get_style(drop, "top", top, sizeof(top)) < 0 || !top[0] || !strcmp(top, "this.offsetTop")) {
            // z.offsetTop: the item's top inside its drop-down
            HtmlRect item, parent;
            if (html_dom_get_rect(ev->target, &item) && html_dom_get_rect(html_dom_parent(ev->target), &parent)) {
                char v[32];
                snprintf(v, sizeof(v), "%dpx", (int) roundf((item.y - parent.y) / (g_ed.elem_scale > 0 ? g_ed.elem_scale : 1)));
                html_dom_set_style(drop, "top", v);
            }
        }
    }
    return 1;
}

// ---------------------------------------------------------------------------
// colour picker (Pickr nano: palette + hue slider + hex input)

static HtmlElem g_pickr;
static HtmlElem g_pickr_target;
static float g_pickr_h, g_pickr_s = 1.0f, g_pickr_v = 1.0f;
static int g_pickr_drag; // 1 palette, 2 hue

static void hsv_to_rgb(const float h, const float s, const float v, int *r, int *g, int *b) {
    const float hh = fmodf(h, 360.0f) / 60.0f;
    const int i = (int) floorf(hh);
    const float f = hh - (float) i, p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
    float rr, gg, bb;
    switch (i % 6) {
        case 0: rr = v; gg = t; bb = p; break;
        case 1: rr = q; gg = v; bb = p; break;
        case 2: rr = p; gg = v; bb = t; break;
        case 3: rr = p; gg = q; bb = v; break;
        case 4: rr = t; gg = p; bb = v; break;
        default: rr = v; gg = p; bb = q; break;
    }
    *r = (int) roundf(rr * 255);
    *g = (int) roundf(gg * 255);
    *b = (int) roundf(bb * 255);
}

static void rgb_to_hsv(const int r, const int g, const int b, float *h, float *s, float *v) {
    const float rf = r / 255.0f, gf = g / 255.0f, bf = b / 255.0f;
    const float mx = fmaxf(rf, fmaxf(gf, bf)), mn = fminf(rf, fminf(gf, bf)), d = mx - mn;
    *v = mx;
    *s = mx ? d / mx : 0;
    if (!d) *h = 0;
    else if (mx == rf) *h = 60.0f * fmodf((gf - bf) / d + 6.0f, 6.0f);
    else if (mx == gf) *h = 60.0f * ((bf - rf) / d + 2.0f);
    else *h = 60.0f * ((rf - gf) / d + 4.0f);
}

static void pickr_ensure(void) {
    if (g_pickr) return;
    const HtmlElem body = html_dom_body();
    if (!body) return;
    // Pickr.create({theme: "nano", lockOpacity, components: {preview: false, hue: true}})
    // Pickr's own markup (libs/pickr.min.js, as the real editor's DOM has it) + the mousedown
    // hooks that drive it from C
    html_dom_insert_html(body, HTML_INSERT_BEFORE_END,
        "<div class=\"pcr-app\" data-theme=\"nano\" aria-label=\"color picker dialog\" role=\"window\" style=\"left: 0; top: 8px;\" id=\"kn-pickr\">\n"
        "          <div class=\"pcr-selection\">\n"
        "            <div class=\"pcr-color-preview\" style=\"display:none\" hidden=\"\">\n"
        "              <button type=\"button\" class=\"pcr-last-color\" aria-label=\"use previous color\"></button>\n"
        "              <div class=\"pcr-current-color\"></div>\n"
        "            </div>\n\n"
        "            <div class=\"pcr-color-palette\" onmousedown=\"GUI.kn.pickr(1)\">\n"
        "              <div class=\"pcr-picker\" style=\"left: calc(0% - 9px); top: calc(0% - 9px); background: rgb(255, 255, 255);\"></div>\n"
        "              <div class=\"pcr-palette\" tabindex=\"0\" aria-label=\"color selection area\" role=\"listbox\" style=\"background: linear-gradient(to top, rgb(0, 0, 0), transparent), linear-gradient(to left, rgb(255, 0, 0), rgb(255, 255, 255));\"></div>\n"
        "            </div>\n\n"
        "            <div class=\"pcr-color-chooser\" onmousedown=\"GUI.kn.pickr(2)\">\n"
        "              <div class=\"pcr-picker\" style=\"left: calc(0% - 9px); background-color: rgb(255, 0, 0);\"></div>\n"
        "              <div class=\"pcr-hue pcr-slider\" tabindex=\"0\" aria-label=\"hue selection slider\" role=\"slider\"></div>\n"
        "            </div>\n\n"
        "            <div class=\"pcr-color-opacity\" style=\"display:none\" hidden=\"\">\n"
        "              <div class=\"pcr-picker\"></div>\n"
        "              <div class=\"pcr-opacity pcr-slider\" tabindex=\"0\" aria-label=\"selection slider\" role=\"slider\"></div>\n"
        "            </div>\n"
        "          </div>\n\n"
        "          <div class=\"pcr-swatches \"></div>\n\n"
        "          <div class=\"pcr-interaction\" style=\"display:none\" hidden=\"\">\n"
        "            <input class=\"pcr-result\" type=\"text\" spellcheck=\"false\" style=\"display:none\" hidden=\"\" aria-label=\"color input field\">\n"
        "          </div>\n"
        "        </div>");
    g_pickr = html_dom_by_id("kn-pickr");
    if (g_pickr) html_dom_set_attr(g_pickr, "data-kn-pointer", "2"); // web: the drag follows (editor.c)
}

static void pickr_layout(void) {
    if (!g_pickr) return;
    const HtmlElem palette = html_dom_query(g_pickr, ".pcr-color-palette");
    const HtmlElem pal_picker = html_dom_query(palette, ".pcr-picker");
    const HtmlElem pal_bg = html_dom_query(palette, ".pcr-palette");
    const HtmlElem chooser = html_dom_query(g_pickr, ".pcr-color-chooser");
    const HtmlElem hue_picker = html_dom_query(chooser, ".pcr-picker");
    int r, g, b;
    hsv_to_rgb(g_pickr_h, 1, 1, &r, &g, &b);
    char css[256];
    snprintf(css, sizeof(css), "linear-gradient(to top, rgb(0, 0, 0), transparent), linear-gradient(to left, rgb(%d, %d, %d), rgb(255, 255, 255))", r, g, b);
    html_dom_set_style(pal_bg, "background", css);
    hsv_to_rgb(g_pickr_h, g_pickr_s, g_pickr_v, &r, &g, &b);
    char v[64];
    snprintf(v, sizeof(v), "calc(%.3f%% - %dpx)", g_pickr_s * 100.0f, 9);
    html_dom_set_style(pal_picker, "left", v);
    snprintf(v, sizeof(v), "calc(%.3f%% - %dpx)", (1.0f - g_pickr_v) * 100.0f, 9);
    html_dom_set_style(pal_picker, "top", v);
    snprintf(v, sizeof(v), "rgb(%d, %d, %d)", r, g, b);
    html_dom_set_style(pal_picker, "background", v);
    (void) chooser;
    snprintf(v, sizeof(v), "calc(%.3f%% - %dpx)", g_pickr_h / 360.0f * 100.0f, 9);
    html_dom_set_style(hue_picker, "left", v);
    hsv_to_rgb(g_pickr_h, 1, 1, &r, &g, &b);
    snprintf(v, sizeof(v), "rgb(%d, %d, %d)", r, g, b);
    html_dom_set_style(hue_picker, "background-color", v);
}

void ed_gui_color_hide(void) {
    if (g_pickr) html_dom_set_class(g_pickr, "visible", 0);
    g_pickr_target = 0;
    g_pickr_drag = 0;
}

static void pickr_show(const HtmlElem target) {
    pickr_ensure();
    if (!g_pickr) return;
    g_pickr_target = target;
    char value[32] = "";
    html_dom_get_value(target, value, sizeof(value));
    const int c = parse_color(value);
    rgb_to_hsv((c >> 16) & 255, (c >> 8) & 255, c & 255, &g_pickr_h, &g_pickr_s, &g_pickr_v);
    html_dom_set_class(g_pickr, "visible", 1);
    // .on("show"): place next to the input, flipped to stay inside the window
    HtmlRect rect;
    float vw = 0, vh = 0;
    html_dom_viewport(&vw, &vh, NULL);
    if (html_dom_get_rect(target, &rect)) {
        const float w = html_dom_offset_width(g_pickr), h = html_dom_offset_height(g_pickr);
        float top = rect.y, left = rect.x + rect.w + 4;
        if (vh - top < h) top = top - h + rect.h;
        if (vw - left < w) left = left - w - rect.w - 8;
        char v[32];
        snprintf(v, sizeof(v), "%dpx", (int) top);
        html_dom_set_style(g_pickr, "top", v);
        snprintf(v, sizeof(v), "%dpx", (int) left);
        html_dom_set_style(g_pickr, "left", v);
    }
    pickr_layout();
}

static void pickr_apply(void) {
    if (!g_pickr_target) return;
    int r, g, b;
    hsv_to_rgb(g_pickr_h, g_pickr_s, g_pickr_v, &r, &g, &b);
    char hex[16];
    snprintf(hex, sizeof(hex), "#%02X%02X%02X", r, g, b); // toHEXA().toString()
    html_dom_set_value(g_pickr_target, hex);
    pickr_layout();
    // B.onchange()
    char onchange[256];
    if (html_dom_get_attr(g_pickr_target, "onchange", onchange, sizeof(onchange)) > 0) {
        const char *p = strstr(onchange, "GUI.action(");
        HtmlEvent e = {.type = "change", .target = g_pickr_target, .value = hex};
        HtmlArg none[1] = {{HTML_ARG_THIS, 0, NULL}};
        extern int ed_gui_color_change_elem(HtmlElem target);
        ed_gui_color_change_elem(g_pickr_target);
        if (p) {
            HtmlArg a[2] = {{HTML_ARG_NUM, atoi(p + 11), NULL}, {HTML_ARG_THIS, 0, NULL}};
            html_ui_dispatch("GUI.action", &e, a, 2);
        }
        (void) none;
    }
}

void ed_gui_pickr_drag(const double fb_x, const double fb_y, const int released) {
    if (!g_pickr_drag || !g_pickr) return;
    if (released) {
        g_pickr_drag = 0;
        return;
    }
    const float x = (float) (fb_x / (g_ed.css_to_fb > 0 ? g_ed.css_to_fb : 1)), y = (float) (fb_y / (g_ed.css_to_fb > 0 ? g_ed.css_to_fb : 1));
    HtmlRect r;
    if (g_pickr_drag == 1 && html_dom_get_rect(html_dom_query(g_pickr, ".pcr-color-palette"), &r) && r.w > 0 && r.h > 0) {
        g_pickr_s = fminf(fmaxf((x - r.x) / r.w, 0), 1);
        g_pickr_v = 1.0f - fminf(fmaxf((y - r.y) / r.h, 0), 1);
    } else if (g_pickr_drag == 2 && html_dom_get_rect(html_dom_query(g_pickr, ".pcr-color-chooser"), &r) && r.w > 0) {
        g_pickr_h = fminf(fmaxf((x - r.x) / r.w, 0), 1) * 360.0f;
        if (g_pickr_h >= 360.0f) g_pickr_h = 359.99f;
    } else return;
    pickr_apply();
}

int ed_gui_pickr_contains(const double fb_x, const double fb_y) {
    if (!g_pickr || !g_pickr_target) return 0;
    HtmlRect r;
    const float s = g_ed.css_to_fb > 0 ? g_ed.css_to_fb : 1;
    if (!html_dom_get_rect(g_pickr, &r)) return 0;
    const float x = (float) fb_x / s, y = (float) fb_y / s;
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

int ed_gui_pickr_target(const HtmlElem e) { return g_pickr_target && e == g_pickr_target; }

static int h_kn_pickr(void *user, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) user;
    g_pickr_drag = argc >= 1 ? (int) args[0].num : 0;
    // the event's own position (CSS px): on the web GLFW sees no motion over the page
    const float s = g_ed.css_to_fb > 0 ? g_ed.css_to_fb : 1;
    ed_gui_pickr_drag(ev->x * s, ev->y * s, 0);
    return 0;
}

int ed_gui_color_change_elem(const HtmlElem target) {
    // GUI.update.color.change: keep a valid hex, recolour the input
    char value[32] = "";
    html_dom_get_value(target, value, sizeof(value));
    int valid = value[0] == '#' && (strlen(value) == 7 || strlen(value) == 4);
    for (size_t i = 1; valid && value[i]; i++) valid = isxdigit((unsigned char) value[i]) != 0;
    char css[64] = "";
    if (!valid) {
        // h.rgbToHex(...style.backgroundColor): the previous colour
        html_dom_get_style(target, "background-color", css, sizeof(css));
        if (css[0] == '#') snprintf(value, sizeof(value), "%s", css);
        else snprintf(value, sizeof(value), "#000000");
        html_dom_set_value(target, value);
    }
    html_dom_set_style(target, "background-color", value);
    html_dom_set_style(target, "color", parse_color(value) < 7829367 ? "#ffffff" : "#000000");
    return 1;
}

static int h_update_color_change(void *user, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) user; (void) args; (void) argc;
    ed_gui_color_change_elem(ev->target);
    if (g_pickr_target == ev->target) {
        char value[32] = "";
        html_dom_get_value(ev->target, value, sizeof(value));
        const int c = parse_color(value);
        rgb_to_hsv((c >> 16) & 255, (c >> 8) & 255, c & 255, &g_pickr_h, &g_pickr_s, &g_pickr_v);
        pickr_layout();
    }
    return 1;
}

static int h_update_color_mousedown(void *user, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) user; (void) args; (void) argc;
    if (ev->button == 0) pickr_show(ev->target);
    return 1;
}

static int h_pickr_hide(void *user, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) user; (void) ev; (void) args; (void) argc;
    ed_gui_color_hide();
    return 1;
}

// ---------------------------------------------------------------------------
// key choice (GUI.update.key: "Press Any Key")

static struct {
    EdCtl *ctl;
    HtmlElem elem;
    char prev[256];
} g_key_choice;

int ed_gui_key_capture(const int key, const int action) {
    if (!g_key_choice.elem || action != GLFW_PRESS) return g_key_choice.elem != 0;
    const char *name = glfwGetKeyName(key, 0);
    if (name && strlen(name) == 1) {
        char up[2] = {(char) toupper((unsigned char) name[0]), 0};
        char html[256];
        snprintf(html, sizeof(html), "%s%s%s", up, g_key_choice.ctl && g_key_choice.ctl->path[0] ? " or " : "", g_key_choice.ctl ? g_key_choice.ctl->path : "");
        html_dom_set_html(g_key_choice.elem, html);
        if (g_key_choice.ctl && g_key_choice.ctl->bind == EB_JSON) ed_json_set_str(g_key_choice.ctl->json, g_key_choice.ctl->key, up);
        if (g_key_choice.ctl && g_key_choice.ctl->on_change) {
            cJSON *v = cJSON_CreateString(up);
            g_key_choice.ctl->on_change(g_key_choice.ctl, v);
            cJSON_Delete(v);
        }
    } else {
        html_dom_set_html(g_key_choice.elem, g_key_choice.prev);
    }
    g_key_choice.elem = 0;
    g_key_choice.ctl = NULL;
    return 1;
}

// ---------------------------------------------------------------------------
// GUI.action(id, this)

static int h_action(void *user, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) user;
    if (argc < 1 || args[0].kind != HTML_ARG_NUM) return 1;
    EdCtl *c = ctl_by_id((int) args[0].num);
    if (!c) return 1;
    cJSON *value = NULL;
    switch (c->kind) {
        case EG_FOLDER_STANDARD:
        case EG_FOLDER_SMALL: {
            char id[128];
            if (html_dom_get_attr(ev->target, "id", id, sizeof(id)) > 0) {
                id[strlen(id) - 1] = 'B';
                char display[16] = "";
                html_dom_get_style(html_dom_by_id(id), "display", display, sizeof(display));
                ed_json_set_bool(g_folder_store, c->path, !strcmp(display, "none"));
            }
            return 1;
        }
        case EG_BUTTON:
        case EG_TOOLBAR_OPTION:
            if (c->action) c->action(c);
            return 1;
        case EG_KEY:
            g_key_choice.ctl = c;
            g_key_choice.elem = ev->target;
            {
                // prev = A.innerHTML: rebuild it from the value + extras (no innerHTML getter)
                char val[32] = "";
                html_dom_get_text(ev->target, val, sizeof(val));
                char *sp = strchr(val, ' ');
                if (sp) *sp = 0;
                snprintf(g_key_choice.prev, sizeof(g_key_choice.prev), "%s%s%s", val, c->path[0] ? " or " : "", c->path);
            }
            html_dom_set_text(ev->target, "Press Any Key");
            return 1;
        case EG_VECTOR3: {
            value = cJSON_CreateArray();
            for (HtmlElem e = html_dom_first_child(html_dom_parent(ev->target)); e; e = html_dom_next(e)) {
                char v[64] = "";
                html_dom_get_value(e, v, sizeof(v));
                cJSON_AddItemToArray(value, cJSON_CreateNumber(strtod(v, NULL)));
            }
            break;
        }
        case EG_SWITCH:
            value = cJSON_CreateBool(ev->checked);
            break;
        case EG_TEXT:
        case EG_COLOR: // "#rrggbb" (not a number: strtod would store 0)
            value = cJSON_CreateString(ev->value ? ev->value : "");
            break;
        case EG_SELECT:
            if (c->value_is_string) value = cJSON_CreateString(ev->value ? ev->value : "");
            else value = cJSON_CreateNumber(ev->value ? strtod(ev->value, NULL) : 0);
            break;
        default:
            value = cJSON_CreateNumber(ev->value ? strtod(ev->value, NULL) : 0);
            break;
    }
    if (c->bind == EB_JSON && c->json) {
        cJSON_DeleteItemFromObjectCaseSensitive(c->json, c->key);
        cJSON_AddItemToObject(c->json, c->key, cJSON_Duplicate(value, 1));
    } else if (c->bind == EB_OBJ && g_ed.proj->selected) {
        EditorObject *o = g_ed.proj->selected;
        const int before = ed_obj_count_class(o);
        ed_prop_set(o, c->key, value);
        g_ed.proj->obj_count[before]--;
        g_ed.proj->obj_count[ed_obj_count_class(o)]++;
        g_ed.dirty_counts = 1;
        if (ed_prop_needs_rebuild(c->key)) ed_obj_rebuild(o);
    }
    ed_mark_changed();
    if (c->on_change) c->on_change(c, value);
    cJSON_Delete(value);
    return 1;
}

static int h_noop(void *user, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) user; (void) ev; (void) args; (void) argc;
    return 1;
}

void ed_gui_install(HtmlHandlerEntry *table, size_t *count, const size_t max) {
    const HtmlHandlerEntry entries[] = {
        {"GUI.action", h_action},
        {"GUI.update.field", h_update_field},
        {"GUI.update.folder", h_update_folder},
        {"GUI.update.number.mousedown", h_update_number_mousedown},
        {"GUI.update.number.change", h_update_number_change},
        {"GUI.update.slider.val", h_update_slider_val},
        {"GUI.update.slider.M", h_update_slider_m},
        {"GUI.update.color.change", h_update_color_change},
        {"GUI.update.color.mousedown", h_update_color_mousedown},
        {"GUI.update.color.pickr.hide", h_pickr_hide},
        {"GUI.update.toolbar", h_update_toolbar},
        {"GUI.kn.pickr", h_kn_pickr},
        {"stopBubbling", h_noop},
    };
    for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]) && *count < max; i++) table[(*count)++] = entries[i];
}

void ed_gui_tick(void) {}
