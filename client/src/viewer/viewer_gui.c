// dat.gui port for the viewer page (see viewer_gui.h). Krunker's viewer builds its panel with
// `new dat.GUI()`, `gui.width = 322` (322 + 1, then the constructor's deferred `width -= 1`:
// the panel ends up 321px wide), folders that start closed until .open(), and controllers
// whose rows dat.gui classes by the bound value's type ("number" / "string" / "boolean" /
// "function").
#include "viewer_gui.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VG_MAX_CTRLS 512
#define VG_MAX_FOLDERS 32
#define VG_CLOSE_BUTTON_HEIGHT 20 // dat.gui onResize: innerHeight - top - 20
#define VG_RESIZE_DEBOUNCE_MS 50.0

struct VgFolder {
    int id;
    int used;
    int closed;
    char name[48];
    VgFolder *parent;
    HtmlElem li;  // li.folder (0 for the root)
    HtmlElem ul;
};

struct VgCtrl {
    int id;
    int used;
    char name[48];
    enum { VG_BOOL, VG_OPTION, VG_FUNCTION, VG_NUMBER, VG_COLOR } kind;
    VgFolder *folder;
    HtmlElem li;
    HtmlElem input; // checkbox / select / text box
    int *bool_value;
    VgValue *value;
    VgFn fn;
    VgFn on_change;
    // VG_NUMBER (NumberControllerBox)
    double *number;
    double min, max, step, implied_step; // min / max NAN when unset, step 0 when unset
    int precision;
    HtmlElem slider, slider_fg; // NumberControllerSlider (min and max set)
    // VG_COLOR (ColorController): the bound 0xRRGGBB, its HSV state (dat.gui Color.__state)
    int *hex;
    int *format;      // VG_COLOR_* (dat.gui Color.__state.conversionName)
    double h, s, v;
    HtmlElem selector, field_knob, hue_knob, sat_field, hue_field;
};

static struct {
    int ready;
    int next_id;
    int hidden;
    int resize_timer;
    int skip_row;          // vgCheck ran for this click: the row's click must not toggle again
    int drag_id;           // controller being dragged (window mousemove / mouseup), 0 = none
    int drag_part;         // VG_DRAG_*
    float drag_prev_y;
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

enum { VG_DRAG_BOX = 1, VG_DRAG_FIELD, VG_DRAG_HUE, VG_DRAG_SLIDER };

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
    snprintf(folder->name, sizeof(folder->name), "%s", name);

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
    snprintf(ctrl->name, sizeof(ctrl->name), "%s", name);
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

// ---- numbers (dat.gui NumberController / NumberControllerBox)

void vg_js_number(const double value, char *buf, const size_t n) {
    // Number.prototype.toString: the shortest digits that read back as the same double, in
    // positional notation for 1e-7 <= |x| < 1e21
    if (!isfinite(value)) {
        snprintf(buf, n, "%s", isnan(value) ? "NaN" : value > 0 ? "Infinity" : "-Infinity");
        return;
    }
    if (value == 0.0) {
        snprintf(buf, n, "0");
        return;
    }
    char tmp[64];
    int precision = 1;
    for (; precision < 17; precision++) {
        snprintf(tmp, sizeof(tmp), "%.*e", precision - 1, value);
        if (strtod(tmp, NULL) == value) break;
    }
    snprintf(tmp, sizeof(tmp), "%.*e", precision - 1, value);
    const int exponent = atoi(strchr(tmp, 'e') + 1);
    if (exponent >= -7 && exponent < 21) {
        const int decimals = precision - 1 - exponent;
        snprintf(buf, n, "%.*f", decimals > 0 ? decimals : 0, value);
    } else {
        // d[.ddd]e+x / e-x
        char mantissa[40];
        snprintf(mantissa, sizeof(mantissa), "%.*e", precision - 1, value);
        *strchr(mantissa, 'e') = 0;
        snprintf(buf, n, "%se%s%d", mantissa, exponent > 0 ? "+" : "-", abs(exponent));
    }
}

// numDecimals(x): digits after the "." of String(x)
static int num_decimals(const double x) {
    char text[64];
    vg_js_number(x, text, sizeof(text));
    const char *dot = strchr(text, '.');
    return dot ? (int) strlen(dot + 1) : 0;
}

// Math.round (half up, toward +Infinity)
static double js_round(const double x) {
    return floor(x + 0.5);
}

static void number_display(VgCtrl *ctrl) {
    if (!ctrl->input) return;
    // roundToDecimal(value, precision)
    const double ten_to = pow(10.0, ctrl->precision);
    char text[64];
    vg_js_number(js_round(*ctrl->number * ten_to) / ten_to, text, sizeof(text));
    html_dom_set_value(ctrl->input, text);
    if (ctrl->slider_fg) { // foreground width = (value - min) / (max - min)
        char width[64];
        vg_js_number((*ctrl->number - ctrl->min) / (ctrl->max - ctrl->min) * 100.0, text, sizeof(text));
        snprintf(width, sizeof(width), "%s%%", text);
        html_dom_set_style(ctrl->slider_fg, "width", width);
    }
}

// NumberController.setValue: clamp to min / max, snap to step; then Controller.setValue
// (property, onChange, updateDisplay)
static void number_set(VgCtrl *ctrl, double value) {
    if (!isnan(ctrl->min) && value < ctrl->min) value = ctrl->min;
    else if (!isnan(ctrl->max) && value > ctrl->max) value = ctrl->max;
    if (ctrl->step != 0.0 && fmod(value, ctrl->step) != 0.0) value = js_round(value / ctrl->step) * ctrl->step;
    *ctrl->number = value;
    if (ctrl->on_change) ctrl->on_change();
    number_display(ctrl);
}

VgCtrl *vg_add_number(VgFolder *folder, double *value, const char *name, const double min, const double max,
                      const double step, const VgFn on_change) {
    if (!folder) return NULL;
    VgCtrl *ctrl = ctrl_new(folder);
    if (!ctrl) return NULL;

    ctrl->kind = VG_NUMBER;
    ctrl->number = value;
    ctrl->on_change = on_change;
    ctrl->min = min;
    ctrl->max = max;
    // the box is drawn by the constructor with the precision the initial value implies;
    // .step() changes it (and the drag increment) for later displays only
    const double initial = *value;
    ctrl->implied_step = initial == 0.0 ? 1.0 : pow(10.0, floor(log10(fabs(initial)))) / 10.0;
    ctrl->precision = num_decimals(ctrl->implied_step);

    // .min(a).max(b) on a box swaps it for a NumberControllerSlider(min, max) with a box in front
    // (li.has-slider): augmentController's min / max compose
    const int slider = !isnan(min) && !isnan(max);
    char widget[512];
    const char *box = "<div><input type=\"text\" onchange=\"vgNumber(%d, this.value)\" onmousedown=\"vgDrag(%d, 1)\"></div>";
    char box_html[192];
    snprintf(box_html, sizeof(box_html), box, ctrl->id, ctrl->id);
    if (slider) {
        snprintf(widget, sizeof(widget), "<div>%s<div class=\"slider\" onmousedown=\"vgDrag(%d, 4)\"><div class=\"slider-fg\"></div></div></div>", box_html, ctrl->id);
    } else {
        snprintf(widget, sizeof(widget), "%s", box_html);
    }
    ctrl_insert(ctrl, slider ? "number has-slider" : "number", name, widget, 0);
    ctrl->input = ctrl->li ? html_dom_query(ctrl->li, "input") : 0;
    if (slider && ctrl->li) {
        ctrl->slider = html_dom_query(ctrl->li, ".slider");
        ctrl->slider_fg = html_dom_query(ctrl->li, ".slider-fg");
    }
    number_display(ctrl);

    if (step > 0.0) {
        ctrl->step = step;
        ctrl->implied_step = step;
        ctrl->precision = num_decimals(step);
    }
    return ctrl;
}

// ---- colours (dat.gui ColorController over a HEX number or a "#rrggbb" string)

static void hex_to_hsv(const int hex, double *h, double *s, double *v) {
    // ColorMath.rgb_to_hsv; a grey keeps the previous hue (recalculateHSV)
    const double r = (hex >> 16) & 255, g = (hex >> 8) & 255, b = hex & 255;
    const double max = fmax(r, fmax(g, b)), min = fmin(r, fmin(g, b)), delta = max - min;
    if (max == 0.0) {
        *s = 0.0;
        *v = 0.0;
        return;
    }
    *s = delta / max;
    *v = max / 255.0;
    if (delta == 0.0) return;
    double hue = r == max ? (g - b) / delta : g == max ? 2.0 + (b - r) / delta : 4.0 + (r - g) / delta;
    hue /= 6.0;
    if (hue < 0.0) hue += 1.0;
    *h = hue * 360.0;
}

static int hsv_to_hex(const double h, const double s, const double v) {
    // ColorMath.hsv_to_rgb, then rgb_to_hex (component << shift truncates)
    const int hi = (int) floor(h / 60.0) % 6;
    const double f = h / 60.0 - floor(h / 60.0);
    const double p = v * (1.0 - s), q = v * (1.0 - f * s), t = v * (1.0 - (1.0 - f) * s);
    const double c[6][3] = {{v, t, p}, {q, v, p}, {p, v, t}, {p, q, v}, {t, p, v}, {v, p, q}};
    const double *rgb = c[hi < 0 ? 0 : hi];
    return ((int) (rgb[0] * 255.0) & 255) << 16 | ((int) (rgb[1] * 255.0) & 255) << 8 | ((int) (rgb[2] * 255.0) & 255);
}

static void color_display(VgCtrl *ctrl) {
    const int hex = *ctrl->hex & 0xFFFFFF;
    char css[16], text[48], style[160];
    snprintf(css, sizeof(css), "#%06x", hex);
    const int format = ctrl->format ? *ctrl->format : VG_COLOR_HEX;
    if (format == VG_COLOR_CSS_HEX) snprintf(text, sizeof(text), "%s", css);
    else if (format == VG_COLOR_CSS_RGB) snprintf(text, sizeof(text), "rgb(%d,%d,%d)", (hex >> 16) & 255, (hex >> 8) & 255, hex & 255);
    else snprintf(text, sizeof(text), "0x%x", hex); // toString of a HEX colour: no padding
    const int flip = ctrl->v < 0.5 || ctrl->s > 0.5 ? 255 : 0, shadow = 255 - flip;

    // li.style.borderLeftColor = color.toString(): "0x..." is no CSS colour, so a HEX colour's
    // row keeps the default (currentColor) border
    if (ctrl->li) html_dom_set_style(ctrl->li, "border-left-color", format == VG_COLOR_HEX ? "" : text);
    if (ctrl->input) {
        html_dom_set_value(ctrl->input, text);
        html_dom_set_style(ctrl->input, "background-color", css);
        snprintf(style, sizeof(style), "rgb(%d,%d,%d)", flip, flip, flip);
        html_dom_set_style(ctrl->input, "color", style);
        snprintf(style, sizeof(style), "0 1px 1px rgba(%d,%d,%d,.7)", shadow, shadow, shadow);
        html_dom_set_style(ctrl->input, "text-shadow", style);
    }
    char px[32];
    if (ctrl->field_knob) {
        snprintf(px, sizeof(px), "%gpx", 100.0 * ctrl->s - 7.0);
        html_dom_set_style(ctrl->field_knob, "margin-left", px);
        snprintf(px, sizeof(px), "%gpx", 100.0 * (1.0 - ctrl->v) - 7.0);
        html_dom_set_style(ctrl->field_knob, "margin-top", px);
        snprintf(style, sizeof(style), "#%06x", hsv_to_hex(ctrl->h, ctrl->s, ctrl->v));
        html_dom_set_style(ctrl->field_knob, "background-color", style);
        snprintf(style, sizeof(style), "2px solid rgb(%d,%d,%d)", flip, flip, flip);
        html_dom_set_style(ctrl->field_knob, "border", style);
    }
    if (ctrl->hue_knob) {
        snprintf(px, sizeof(px), "%gpx", 100.0 * (1.0 - ctrl->h / 360.0));
        html_dom_set_style(ctrl->hue_knob, "margin-top", px);
    }
    if (ctrl->sat_field) {
        // linearGradient(field, "left", "#fff", hue at full s / v); dat.gui writes the
        // vendor-prefixed legacy "left" form, which is "to right"
        snprintf(style, sizeof(style), "linear-gradient(to right, #fff 0%%, #%06x 100%%)", hsv_to_hex(ctrl->h, 1.0, 1.0));
        html_dom_set_style(ctrl->sat_field, "background", style);
    }
}

static void color_set_hsv(VgCtrl *ctrl) {
    *ctrl->hex = hsv_to_hex(ctrl->h, ctrl->s, ctrl->v);
    if (ctrl->on_change) ctrl->on_change();
    color_display(ctrl);
}

VgCtrl *vg_add_color(VgFolder *folder, int *hex, int *format, const char *name, const VgFn on_change) {
    if (!folder) return NULL;
    VgCtrl *ctrl = ctrl_new(folder);
    if (!ctrl) return NULL;

    ctrl->kind = VG_COLOR;
    ctrl->hex = hex;
    ctrl->format = format;
    ctrl->on_change = on_change;
    hex_to_hsv(*hex, &ctrl->h, &ctrl->s, &ctrl->v);

    char widget[1536];
    snprintf(widget, sizeof(widget),
             "<div><input type=\"text\" style=\"outline: none; text-align: center; color: #fff; border: 0; font-weight: bold; text-shadow: 0 1px 1px rgba(0,0,0,0.7);\" onchange=\"vgColor(%d, this.value)\">"
             "<div class=\"selector\" style=\"width: 122px; height: 102px; padding: 3px; background-color: #222; box-shadow: 0px 1px 3px rgba(0,0,0,0.3);\">"
             "<div class=\"field-knob\" style=\"position: absolute; width: 12px; height: 12px; box-shadow: 0px 1px 3px rgba(0,0,0,0.5); border-radius: 12px; z-index: 1;\" onmousedown=\"vgDrag(%d, 2)\"></div>"
             "<div class=\"saturation-field\" style=\"width: 100px; height: 100px; border: 1px solid #555; margin-right: 3px; display: inline-block; cursor: pointer;\" onmousedown=\"vgDrag(%d, 2)\">"
             "<div style=\"width: 100%%; height: 100%%; background: linear-gradient(to bottom, rgba(0,0,0,0) 0%%, #000 100%%);\"></div></div>"
             "<div class=\"hue-field\" style=\"width: 15px; height: 100px; border: 1px solid #555; cursor: ns-resize; position: absolute; top: 3px; right: 3px; "
             "background: linear-gradient(to bottom, #ff0000 0%%, #ff00ff 17%%, #0000ff 34%%, #00ffff 50%%, #00ff00 67%%, #ffff00 84%%, #ff0000 100%%);\" onmousedown=\"vgDrag(%d, 3)\">"
             "<div class=\"hue-knob\" style=\"position: absolute; width: 15px; height: 2px; border-right: 4px solid #fff; z-index: 1;\"></div></div>"
             "</div></div>",
             ctrl->id, ctrl->id, ctrl->id, ctrl->id);
    ctrl_insert(ctrl, "color", name, widget, 0);
    if (ctrl->li) {
        ctrl->input = html_dom_query(ctrl->li, "input");
        ctrl->selector = html_dom_query(ctrl->li, ".selector");
        ctrl->field_knob = html_dom_query(ctrl->li, ".field-knob");
        ctrl->hue_knob = html_dom_query(ctrl->li, ".hue-knob");
        ctrl->sat_field = html_dom_query(ctrl->li, ".saturation-field");
        ctrl->hue_field = html_dom_query(ctrl->li, ".hue-field");
    }
    color_display(ctrl);
    return ctrl;
}

// interpret(): "#rgb", "#rrggbb", "rgb(r,g,b)" / "rgba(r,g,b,a)" (*format: the conversion it
// matched, which the value takes from then on); -1 when nothing matches
static int parse_color(const char *text, int *format) {
    *format = VG_COLOR_CSS_HEX;
    while (*text == ' ') text++;
    if (*text == '#') {
        const size_t len = strspn(text + 1, "0123456789abcdefABCDEF");
        if (len == 6 && !text[7]) return (int) strtol(text + 1, NULL, 16);
        if (len == 3 && !text[4]) {
            const int v = (int) strtol(text + 1, NULL, 16);
            const int r = (v >> 8) & 15, g = (v >> 4) & 15, b = v & 15;
            return (r * 17) << 16 | (g * 17) << 8 | b * 17;
        }
        return -1;
    }
    int r = 0, g = 0, b = 0;
    float a = 0.0f;
    *format = VG_COLOR_CSS_RGB;
    if (sscanf(text, "rgba(%d,%d,%d,%f)", &r, &g, &b, &a) == 4 || sscanf(text, "rgb(%d,%d,%d)", &r, &g, &b) == 3) {
        return (r & 255) << 16 | (g & 255) << 8 | (b & 255);
    }
    return -1;
}

static void drag_to(VgCtrl *ctrl, const float x, const float y) {
    HtmlRect rect = {0};
    if (g_vg.drag_part == VG_DRAG_SLIDER && ctrl->slider && html_dom_get_rect(ctrl->slider, &rect) && rect.w > 0) {
        // NumberControllerSlider onMouseDrag: map(clientX, left, right, min, max)
        number_set(ctrl, ctrl->min + (ctrl->max - ctrl->min) * ((x - rect.x) / rect.w));
    } else if (g_vg.drag_part == VG_DRAG_BOX) {
        // NumberControllerBox onMouseDrag: value + (prevY - clientY) * impliedStep
        number_set(ctrl, *ctrl->number + (g_vg.drag_prev_y - y) * ctrl->implied_step);
        g_vg.drag_prev_y = y;
    } else if (g_vg.drag_part == VG_DRAG_FIELD && ctrl->sat_field && html_dom_get_rect(ctrl->sat_field, &rect) && rect.w > 0 && rect.h > 0) {
        // setSV: s = x across, v = 1 - y down the field
        ctrl->s = fmin(1.0, fmax(0.0, (x - rect.x) / rect.w));
        ctrl->v = fmin(1.0, fmax(0.0, 1.0 - (y - rect.y) / rect.h));
        color_set_hsv(ctrl);
    } else if (g_vg.drag_part == VG_DRAG_HUE && ctrl->hue_field && html_dom_get_rect(ctrl->hue_field, &rect) && rect.h > 0) {
        // setH: h = 1 - y down the field
        ctrl->h = fmin(1.0, fmax(0.0, 1.0 - (y - rect.y) / rect.h)) * 360.0;
        color_set_hsv(ctrl);
    }
}

int vg_dragging(void) {
    return g_vg.drag_id != 0;
}

void vg_drag_move(const float x, const float y) {
    VgCtrl *ctrl = ctrl_by_id(g_vg.drag_id);
    if (!ctrl) {
        g_vg.drag_id = 0;
        return;
    }
    drag_to(ctrl, x, y);
}

void vg_drag_end(void) {
    VgCtrl *ctrl = ctrl_by_id(g_vg.drag_id);
    if (ctrl && ctrl->selector) html_dom_set_class(ctrl->selector, "drag", 0);
    g_vg.drag_id = 0;
}

void vg_folder_set_visible(VgFolder *folder, const int visible) {
    // folder.domElement.style.display: the .dg inside the folder's li
    const HtmlElem dg = folder && folder->li ? html_dom_first_child(folder->li) : 0;
    if (dg) html_dom_set_display(dg, visible ? "" : "none");
    vg_resize();
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

// text box change: parseFloat; NaN is ignored
static int h_vg_number(void *user, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) user;
    VgCtrl *ctrl = ctrl_by_id(arg_id(args, argc));
    if (!ctrl || ctrl->kind != VG_NUMBER) return 1;
    const char *text = argc > 1 && args[1].kind == HTML_ARG_STR && args[1].str ? args[1].str : ev && ev->value ? ev->value : "";
    char *end = NULL;
    const double value = strtod(text, &end);
    if (end != text) number_set(ctrl, value);
    return 1;
}

// colour box change (onBlur / Enter): a colour it can read becomes the value, else the text resets
static int h_vg_color(void *user, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) user;
    VgCtrl *ctrl = ctrl_by_id(arg_id(args, argc));
    if (!ctrl || ctrl->kind != VG_COLOR) return 1;
    const char *text = argc > 1 && args[1].kind == HTML_ARG_STR && args[1].str ? args[1].str : ev && ev->value ? ev->value : "";
    int format = VG_COLOR_HEX;
    const int hex = parse_color(text, &format);
    if (hex >= 0) {
        // color.__state = interpret(value): the conversion changes too (a typed "#rrggbb" makes
        // a HEX property a string)
        hex_to_hsv(hex, &ctrl->h, &ctrl->s, &ctrl->v);
        *ctrl->hex = hex;
        if (ctrl->format) *ctrl->format = format;
        if (ctrl->on_change) ctrl->on_change();
    }
    color_display(ctrl);
    return 1;
}

// mousedown on a number box (part 1), the saturation field / knob (2) or the hue field (3):
// the drag follows the window's mouse until the button goes up (vg_drag_move / vg_drag_end)
static int h_vg_drag(void *user, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) user;
    VgCtrl *ctrl = ctrl_by_id(arg_id(args, argc));
    const int part = argc > 1 && args[1].kind == HTML_ARG_NUM ? (int) args[1].num : 0;
    if (!ctrl || !ev || (part == VG_DRAG_BOX || part == VG_DRAG_SLIDER) != (ctrl->kind == VG_NUMBER)) return 1;
    g_vg.drag_id = ctrl->id;
    g_vg.drag_part = part;
    g_vg.drag_prev_y = ev->y;
    if (part != VG_DRAG_BOX) {
        if (ctrl->selector) html_dom_set_class(ctrl->selector, "drag", 1);
        drag_to(ctrl, ev->x, ev->y);
        return 0; // preventDefault
    }
    return 1;
}

static int h_vg_title(void *user, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) user;
    (void) ev;
    VgFolder *folder = folder_by_id(arg_id(args, argc));
    if (folder) folder_set_closed(folder, !folder->closed);
    return 0; // preventDefault
}

int vg_test_action(const char *path, const char *value) {
    char folder_name[48] = "";
    const char *name = path;
    const char *slash = strrchr(path, '/');
    if (slash) {
        snprintf(folder_name, sizeof(folder_name), "%.*s", (int) (slash - path), path);
        name = slash + 1;
    }
    VgCtrl *ctrl = NULL;
    for (int i = 0; i < VG_MAX_CTRLS && !ctrl; i++) {
        VgCtrl *c = &g_vg.ctrls[i];
        if (c->used && !strcmp(c->name, name) && (!folder_name[0] || !strcmp(c->folder->name, folder_name))) ctrl = c;
    }
    if (!ctrl) { // a folder: click its title
        for (int i = 1; i < VG_MAX_FOLDERS; i++) {
            VgFolder *f = &g_vg.folders[i];
            if (f->used && !strcmp(f->name, name) && (!folder_name[0] || (f->parent && !strcmp(f->parent->name, folder_name)))) {
                folder_set_closed(f, !f->closed);
                return 1;
            }
        }
        return 0;
    }

    HtmlArg args[2] = {{.kind = HTML_ARG_NUM, .num = ctrl->id}, {.kind = HTML_ARG_STR, .str = value ? value : ""}};
    HtmlEvent ev = {.type = "change", .target = ctrl->input, .value = value ? value : ""};
    switch (ctrl->kind) {
        case VG_BOOL:
            if (value && *value) bool_set(ctrl, !strcmp(value, "true") || !strcmp(value, "1"));
            else bool_set(ctrl, !*ctrl->bool_value);
            break;
        case VG_FUNCTION: if (ctrl->fn) ctrl->fn(); break;
        case VG_OPTION:
            if (ctrl->input) html_dom_set_value(ctrl->input, value ? value : "");
            h_vg_select(NULL, &ev, args, 2);
            break;
        case VG_NUMBER:
            if (ctrl->input) html_dom_set_value(ctrl->input, value ? value : "");
            h_vg_number(NULL, &ev, args, 2);
            break;
        case VG_COLOR:
            if (ctrl->input) html_dom_set_value(ctrl->input, value ? value : "");
            h_vg_color(NULL, &ev, args, 2);
            break;
    }
    return 1;
}

void vg_register_handlers(HtmlHandlerEntry *table, int *count) {
    table[(*count)++] = (HtmlHandlerEntry) {"vgRow", h_vg_row};
    table[(*count)++] = (HtmlHandlerEntry) {"vgCheck", h_vg_check};
    table[(*count)++] = (HtmlHandlerEntry) {"vgSelect", h_vg_select};
    table[(*count)++] = (HtmlHandlerEntry) {"vgTitle", h_vg_title};
    table[(*count)++] = (HtmlHandlerEntry) {"vgNumber", h_vg_number};
    table[(*count)++] = (HtmlHandlerEntry) {"vgColor", h_vg_color};
    table[(*count)++] = (HtmlHandlerEntry) {"vgDrag", h_vg_drag};
}
