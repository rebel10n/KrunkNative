/*
 * widgets.c - form controls: state, Chrome-default geometry, painting and
 * input behaviour.  See widgets.h for the contract.
 *
 * Chrome defaults used here (refreshed form controls, Linux/Windows), with
 * the evidence from doc/ref/krunker-v3.1.1/windows (captures are 1280x720
 * with #uiBase scaled by 0.6275 = 1280 / (1700 * 1.2), scaleUI = 0.8):
 *
 *  range    thumb centred on the input's content box, thumb x = fraction *
 *           (track_w - thumb_w): win-settings-general.png "Lag Compensation"
 *           at value = max: track #a8a8a8 rows 340..348, thumb #2196f3 rows
 *           337..351 (centre 344 = track centre), thumb right edge = track
 *           right edge; 15 px track = 9 device rows, 25 px thumb = 15 rows.
 *  select   menulist-button: inner padding 4 / 1 / 1 / 1 + a 15 px arrow box
 *           at the inline end (LayoutThemeDefault::PopupInternalPadding*,
 *           MenuListArrowWidthInDIP = scrollbar arrow width = 15).  "Local"
 *           (.inputGrey2, 12 px gamefont, padding 6): border box 45 device px
 *           = 71.7 css = 6 + 4 + 40.5 ("Local" = 3375/1000 em) + 1 + 15 + 6
 *           = 72.5; height 21 device px = 33.5 css for the 14 px preset
 *           select = 6 + 1 + 20 (line-height normal: round(15.75) +
 *           round(3.5)) + 1 + 6 = 34.  Chevron: 5x3 device px (8x5 css),
 *           centre 6.5 css from the right edge (arrow box 15 -> 7.5), colour
 *           = the select's `color`.
 *  text     width = size(20) * avgCharWidth + (maxCharWidth - avgCharWidth)
 *           (LayoutTextControlSingleLine::PreferredContentLogicalWidth):
 *           win-settings-render.png "1280x720" (.inputGrey2 12 px): 127
 *           device px = 202 css = 6 + (20 * 9 + (21 - 9)) + 6 = 204.
 *           Placeholder colour #757575 = rgb(117,117,117) sampled in
 *           win-profile-login.png and win-settings-render.png.
 *  color    swatch fills the padding box minus the wrapper padding:
 *           win-settings-display.png "Hit Indicator Color": border box
 *           33x14 device px (55x24 css, border-box), swatch 29x10 (47x16).
 *  number   .sliderVal: 43x20 device px = 70x34 css = 50 + 2*10 by 24 + 2*5,
 *           i.e. the spin button (appearance none, margin 0) takes no space
 *           and line-height applies to the inner editor.
 */
#include "widgets.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "css_sheet.h"
#include "util.h"

/* GLFW key codes (events.h mirrors only a few). */
#define WK_SPACE 32
#define WK_A 65
#define WK_C 67
#define WK_V 86
#define WK_X 88
#define WK_Y 89
#define WK_Z 90
#define WK_INSERT 260
#define WK_BACKSPACE 259
#define WK_DELETE 261
#define WK_RIGHT 262
#define WK_LEFT 263
#define WK_DOWN 264
#define WK_UP 265
#define WK_PAGE_UP 266
#define WK_PAGE_DOWN 267
#define WK_HOME 268
#define WK_END 269
#define WK_F4 293

/* Editing modifiers: Ctrl everywhere except macOS, where Cmd edits and
 * Option jumps by words (Chrome's platform conventions). */
#ifdef __APPLE__
#define WIDGETS_MOD_PRIMARY EV_MOD_SUPER
#define WIDGETS_MOD_WORD EV_MOD_ALT
#else
#define WIDGETS_MOD_PRIMARY EV_MOD_CONTROL
#define WIDGETS_MOD_WORD EV_MOD_CONTROL
#endif

#define WIDGETS_UNDO_MAX 64
enum { EDIT_NONE = 0, EDIT_TYPING, EDIT_DELETE, EDIT_OTHER };

/* ------------------------------------------------------------------------- */
/* State                                                                      */
/* ------------------------------------------------------------------------- */

typedef struct UndoEntry {
    char *value;
    int caret, anchor;
} UndoEntry;

typedef struct WidgetState {
    HtmlElem elem;
    uint8_t kind;
    uint8_t inited;
    uint8_t dragging;          /* range: thumb drag; text: selection drag; color: picker drag */
    uint8_t drag_part;         /* color picker: 1 = SV square, 2 = hue bar      */
    KnStrBuf value;            /* live value (text-like, range, color)           */
    char *commit_value;        /* value when `change` was last fired / focus     */
    char *drag_start;          /* range: value at press                           */
    int caret, anchor;         /* byte offsets into value                          */
    float scroll_x;            /* text: horizontal scroll of the inner editor     */
    float scroll_y;            /* textarea: vertical scroll (px)                   */
    float goal_x;              /* textarea: Up/Down keep this x (-1: caret x)      */
    int seen_caret;            /* textarea: caret last scrolled into view (+1)     */
    size_t seen_len;           /* textarea: value length at that time              */
    int hover_index;           /* select popup: highlighted row                    */
    float popup_scroll;        /* select popup: list scroll offset (px)            */
    float bar_grab;            /* select popup: thumb drag anchor (px into thumb)   */
    char typeahead[32];        /* select: type-ahead prefix                         */
    int typeahead_len;
    double typeahead_ms;       /* time of the last type-ahead key                   */
    UndoEntry undo[WIDGETS_UNDO_MAX], redo[WIDGETS_UNDO_MAX];  /* text: Ctrl+Z / Ctrl+Y */
    int undo_n, redo_n;
    uint8_t last_edit;         /* EDIT_*: coalescing of consecutive typing/deleting */
    int last_caret;
    double last_click_ms;      /* text: double/triple-click detection               */
    float last_click_x, last_click_y;
    int click_count;
    struct WidgetCode *code;   /* textarea: code view (widgets_set_code_view)      */
} WidgetState;

/* Code view of a <textarea> (widgets.h WidgetsCodeView). */
typedef struct WidgetCode {
    WidgetsCodeView cfg;
    WidgetsCodeSpan *spans;
    int nspans;
    WidgetsCodeLine *lines;
    int nlines;
} WidgetCode;

static void code_free(WidgetCode *c)
{
    if (!c) return;
    free(c->spans);
    free(c->lines);
    free(c);
}

struct Widgets {
    Dom *dom;
    TextSystem *text;
    WidgetsHost host;
    WidgetsTheme theme;

    WidgetState **slots;
    uint32_t slot_cap;

    HtmlElem focused;
    HtmlElem popup;            /* element owning the open popup                   */
    double blink_start_ms;
    double now_ms;             /* widgets_set_time(), used by widgets_paint_host   */
    int time_set;              /* widgets_set_time() was called: use now_ms as the clock */
    int gl_ready;
    float vw, vh;
    uint8_t in_press;          /* inside widgets_mouse_down (focus ordering)       */
    float mouse_x, mouse_y;
    KnStrBuf clipboard;        /* fallback when the host has no clipboard hooks    */
};

static const WidgetsTheme k_default_theme = {
    .caret_blink_ms = 500.0f,
    .caret_width = 1.0f,
    .selection_bg = {0x33, 0x90, 0xff, 0xff, CSS_COLOR_RGBA},
    .placeholder_color = {0x75, 0x75, 0x75, 0xff, CSS_COLOR_RGBA},
    .default_size_attr = 20,
    .menulist_arrow_box = 15.0f,
    .menulist_pad_start = 4.0f,
    .menulist_pad_end = 1.0f,
    .menulist_pad_top = 1.0f,
    .menulist_pad_bottom = 1.0f,
    .menulist_arrow_w = 7.75f,
    .menulist_arrow_h = 4.25f,
    .menulist_arrow_stroke = 1.6f,
    .popup_bg = {0xff, 0xff, 0xff, 0xff, CSS_COLOR_RGBA},
    .popup_border = {0x76, 0x76, 0x76, 0xff, CSS_COLOR_RGBA},
    .popup_hover_bg = {0x1e, 0x90, 0xff, 0xff, CSS_COLOR_RGBA},
    .popup_hover_fg = {0xff, 0xff, 0xff, 0xff, CSS_COLOR_RGBA},
    .popup_row_pad_x = 2.0f,
    .popup_row_pad_bottom = 1.0f,
    .popup_row_min_em = 1.2f,
    .popup_max_rows = 20,
    .popup_scrollbar_w = 15.0f,
    .popup_thumb_min = 18.0f,
    .popup_track = {0xf1, 0xf1, 0xf1, 0xff, CSS_COLOR_RGBA},
    .popup_thumb = {0xc1, 0xc1, 0xc1, 0xff, CSS_COLOR_RGBA},
    .control_border = {0x76, 0x76, 0x76, 0xff, CSS_COLOR_RGBA},
    .control_bg = {0xff, 0xff, 0xff, 0xff, CSS_COLOR_RGBA},
    .control_face = {0xef, 0xef, 0xef, 0xff, CSS_COLOR_RGBA},
    .control_accent = {0x00, 0x75, 0xff, 0xff, CSS_COLOR_RGBA},
    .native_range_track_h = 8.0f,
    .native_range_thumb_d = 16.0f,
    .native_checkbox_radius = 2.0f,
    .swatch_wrapper_pad_x = 2.0f,
    .swatch_wrapper_pad_y = 4.0f,
    .swatch_border = 1.0f,
    .swatch_border_color = {0x77, 0x77, 0x77, 0xff, CSS_COLOR_RGBA},
    .picker_sv_w = 180.0f,
    .picker_sv_h = 150.0f,
    .picker_hue_w = 20.0f,
    .picker_pad = 8.0f,
    .picker_preview_h = 16.0f,
};

/* ------------------------------------------------------------------------- */
/* Small helpers                                                              */
/* ------------------------------------------------------------------------- */

static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static double clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }

static char *dup_cstr(const char *s)
{
    size_t n = strlen(s);
    char *d = (char *)malloc(n + 1);
    if (d) memcpy(d, s, n + 1);
    return d;
}

static void set_cstr(char **slot, const char *s)
{
    free(*slot);
    *slot = s ? dup_cstr(s) : NULL;
}

static const char *sb_cstr(const KnStrBuf *sb) { return sb->data ? sb->data : ""; }

static void sb_set(KnStrBuf *sb, const char *s, size_t len)
{
    kn_sb_clear(sb);
    kn_sb_append(sb, s, len);
    if (!sb->data) kn_sb_append(sb, "", 0);
}

static CssColor rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    CssColor c = {r, g, b, a, CSS_COLOR_RGBA};
    return c;
}

static CssColor resolve_color(CssColor c, const CssComputed *st)
{
    if (c.kind == CSS_COLOR_CURRENT) {
        if (st) return st->color;
        return rgba(0, 0, 0, 255);
    }
    return c;
}

static float resolve_len(CssLength l, float ref, float fallback)
{
    if (l.kind != CSS_LEN_VALUE) return fallback;
    return l.px + l.pct * ref / 100.0f;
}

static int attr_eq(Dom *dom, HtmlElem e, const char *name, const char *lit)
{
    size_t len = 0;
    const char *v = dom_get_attr(dom, e, name, &len);
    return v != NULL && kn_str_ieq(v, len, lit);
}

/* Parses a floating number attribute (HTML "valid floating-point number"
 * subset).  Returns 0 when absent/invalid. */
static int attr_number(Dom *dom, HtmlElem e, const char *name, double *out)
{
    size_t len = 0;
    const char *v = dom_get_attr(dom, e, name, &len);
    char *end = NULL;
    double d;
    if (!v) return 0;
    v = kn_str_trim(v, &len);
    if (len == 0) return 0;
    d = strtod(v, &end);
    if (end == v || (size_t)(end - v) != len || d != d || d == HUGE_VAL || d == -HUGE_VAL) return 0;
    *out = d;
    return 1;
}

static int str_decimals(const char *s)
{
    const char *dot = strchr(s, '.');
    int n = 0;
    if (!dot) return 0;
    for (dot++; kn_is_digit(*dot); dot++) n++;
    return n;
}

static int attr_decimals(Dom *dom, HtmlElem e, const char *name)
{
    const char *v = dom_get_attr(dom, e, name, NULL);
    return v ? str_decimals(v) : 0;
}

/* ------------------------------------------------------------------------- */
/* Number formatting / color maths (public helpers)                           */
/* ------------------------------------------------------------------------- */

void widgets_format_number(double v, int decimals, char *out, size_t cap)
{
    char buf[64];
    char *dot;
    if (cap == 0) return;
    if (v == 0) v = 0; /* no "-0" */
    if (decimals < 0) {
        snprintf(buf, sizeof buf, "%.15g", v);
        if (strchr(buf, 'e')) snprintf(buf, sizeof buf, "%.15f", v);
    } else {
        if (decimals > 15) decimals = 15;
        snprintf(buf, sizeof buf, "%.*f", decimals, v);
    }
    dot = strchr(buf, '.');
    if (dot) {
        char *end = buf + strlen(buf);
        while (end > dot + 1 && end[-1] == '0') end--;
        if (end == dot + 1) end = dot;
        *end = '\0';
    }
    if (strcmp(buf, "-0") == 0) strcpy(buf, "0");
    snprintf(out, cap, "%s", buf);
}

int widgets_parse_hex_color(const char *s, uint8_t *r, uint8_t *g, uint8_t *b)
{
    int i;
    if (!s || s[0] != '#' || strlen(s) != 7) return 0;
    for (i = 1; i < 7; i++)
        if (!kn_is_hex(s[i])) return 0;
    *r = (uint8_t)(kn_hex_val(s[1]) * 16 + kn_hex_val(s[2]));
    *g = (uint8_t)(kn_hex_val(s[3]) * 16 + kn_hex_val(s[4]));
    *b = (uint8_t)(kn_hex_val(s[5]) * 16 + kn_hex_val(s[6]));
    return 1;
}

void widgets_rgb_to_hsv(uint8_t r8, uint8_t g8, uint8_t b8, float *h, float *s, float *v)
{
    float r = r8 / 255.0f, g = g8 / 255.0f, b = b8 / 255.0f;
    float mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    float mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    float d = mx - mn;
    *v = mx;
    *s = mx > 0 ? d / mx : 0;
    if (d <= 0) {
        *h = 0;
    } else if (mx == r) {
        *h = 60.0f * fmodf((g - b) / d, 6.0f);
    } else if (mx == g) {
        *h = 60.0f * ((b - r) / d + 2.0f);
    } else {
        *h = 60.0f * ((r - g) / d + 4.0f);
    }
    if (*h < 0) *h += 360.0f;
}

void widgets_hsv_to_rgb(float h, float s, float v, uint8_t *r, uint8_t *g, uint8_t *b)
{
    float c = v * s;
    float hh = fmodf(h, 360.0f) / 60.0f;
    float x = c * (1 - fabsf(fmodf(hh, 2.0f) - 1));
    float m = v - c;
    float rr = 0, gg = 0, bb = 0;
    if (hh < 0) hh += 6;
    if (hh < 1) { rr = c; gg = x; }
    else if (hh < 2) { rr = x; gg = c; }
    else if (hh < 3) { gg = c; bb = x; }
    else if (hh < 4) { gg = x; bb = c; }
    else if (hh < 5) { rr = x; bb = c; }
    else { rr = c; bb = x; }
    *r = (uint8_t)lroundf((rr + m) * 255.0f);
    *g = (uint8_t)lroundf((gg + m) * 255.0f);
    *b = (uint8_t)lroundf((bb + m) * 255.0f);
}

/* ------------------------------------------------------------------------- */
/* Range maths                                                                */
/* ------------------------------------------------------------------------- */

static double snap_step(double min, double max, double step, double v)
{
    if (step > 0) {
        double n = floor((v - min) / step + 0.5);
        v = min + n * step;
    }
    return clampd(v, min, max);
}

double widgets_range_value_from_x(double min, double max, double step, float track_x, float track_w, float thumb_w, float x)
{
    float usable = track_w - thumb_w;
    double fraction;
    if (max <= min) return min;
    if (usable <= 0) {
        fraction = (x - track_x) >= track_w / 2 ? 1.0 : 0.0;
    } else {
        float pos = clampf(x - track_x - thumb_w / 2, 0, usable);
        fraction = pos / usable;
    }
    return snap_step(min, max, step, min + fraction * (max - min));
}

float widgets_range_thumb_x(double min, double max, double value, float track_x, float track_w, float thumb_w)
{
    double fraction = max > min ? clampd((value - min) / (max - min), 0, 1) : 0;
    return track_x + (float)(fraction * (track_w - thumb_w));
}

/* ------------------------------------------------------------------------- */
/* Classification                                                             */
/* ------------------------------------------------------------------------- */

static WidgetKind classify(Dom *dom, HtmlElem e)
{
    lxb_tag_id_t tag = dom_tag_id(dom, e);
    size_t len = 0;
    const char *type;
    if (tag == LXB_TAG_SELECT) return WIDGET_SELECT;
    if (tag == LXB_TAG_TEXTAREA) return WIDGET_TEXTAREA;
    if (tag != LXB_TAG_INPUT) return WIDGET_NONE;
    type = dom_get_attr(dom, e, "type", &len);
    if (!type) return WIDGET_TEXT;
    type = kn_str_trim(type, &len);
    if (kn_str_ieq(type, len, "checkbox")) return WIDGET_CHECKBOX;
    if (kn_str_ieq(type, len, "radio")) return WIDGET_RADIO;
    if (kn_str_ieq(type, len, "range")) return WIDGET_RANGE;
    if (kn_str_ieq(type, len, "color")) return WIDGET_COLOR;
    if (kn_str_ieq(type, len, "number")) return WIDGET_NUMBER;
    if (kn_str_ieq(type, len, "password")) return WIDGET_PASSWORD;
    if (kn_str_ieq(type, len, "hidden") || kn_str_ieq(type, len, "submit") || kn_str_ieq(type, len, "button") ||
        kn_str_ieq(type, len, "reset") || kn_str_ieq(type, len, "image") || kn_str_ieq(type, len, "file"))
        return WIDGET_NONE;
    /* text, url, search, email, tel, undefined, input, anything unknown */
    return WIDGET_TEXT;
}

WidgetKind widgets_kind(Widgets *w, HtmlElem e)
{
    if (!w || !e || !dom_elem_data(w->dom, e)) return WIDGET_NONE;
    return classify(w->dom, e);
}

static int kind_is_text(WidgetKind k) { return k == WIDGET_TEXT || k == WIDGET_PASSWORD || k == WIDGET_NUMBER || k == WIDGET_TEXTAREA; }
static int kind_is_check(WidgetKind k) { return k == WIDGET_CHECKBOX || k == WIDGET_RADIO; }

/* ------------------------------------------------------------------------- */
/* State table                                                                */
/* ------------------------------------------------------------------------- */

static WidgetState *state_find(Widgets *w, HtmlElem e)
{
    return (e && e < w->slot_cap) ? w->slots[e] : NULL;
}

static void undo_clear(WidgetState *s);
static double clock_ms(Widgets *w);
static void select_word_at(WidgetState *s, int off);

static void state_free(WidgetState *s)
{
    if (!s) return;
    undo_clear(s);
    code_free(s->code);
    kn_sb_free(&s->value);
    free(s->commit_value);
    free(s->drag_start);
    free(s);
}

static void state_init_value(Widgets *w, WidgetState *s);

static WidgetState *state_get(Widgets *w, HtmlElem e)
{
    WidgetState *s = state_find(w, e);
    WidgetKind k;
    if (s) return s;
    if (!e || !dom_elem_data(w->dom, e)) return NULL;
    k = classify(w->dom, e);
    if (k == WIDGET_NONE) return NULL;
    if (e >= w->slot_cap) {
        uint32_t cap = w->slot_cap ? w->slot_cap : 256;
        while (cap <= e) cap *= 2;
        WidgetState **ns = (WidgetState **)realloc(w->slots, cap * sizeof *ns);
        if (!ns) return NULL;
        memset(ns + w->slot_cap, 0, (cap - w->slot_cap) * sizeof *ns);
        w->slots = ns;
        w->slot_cap = cap;
    }
    s = (WidgetState *)calloc(1, sizeof *s);
    if (!s) return NULL;
    s->elem = e;
    s->kind = (uint8_t)k;
    s->hover_index = -1;
    w->slots[e] = s;
    state_init_value(w, s);
    return s;
}

/* ------------------------------------------------------------------------- */
/* Fonts and measuring                                                        */
/* ------------------------------------------------------------------------- */

typedef struct WFont {
    TextFont *font;        /* NULL when no TextSystem                         */
    float size;
    float ascent, descent, line_height;
    unsigned synth;
} WFont;

static void font_for(Widgets *w, const CssComputed *st, WFont *out)
{
    float size = st ? st->font_size : 13.3333f;
    memset(out, 0, sizeof *out);
    out->size = size;
    if (w->text) {
        char families[256];
        size_t n = 0;
        int i;
        families[0] = '\0';
        if (st) {
            for (i = 0; i < st->font_family_count && i < CSS_MAX_FONT_FAMILIES; i++) {
                const char *f = st->font_families[i];
                if (!f) continue;
                n += (size_t)snprintf(families + n, sizeof families - n, "%s%s", n ? ", " : "", f);
                if (n >= sizeof families) break;
            }
        }
        if (families[0] == '\0') snprintf(families, sizeof families, "arial");
        out->font = text_font(w->text, families, size, st ? st->font_weight : 400,
                              st ? st->font_style != CSS_FONT_STYLE_NORMAL : 0);
        if (out->font) {
            const TextMetrics *m = text_font_metrics(out->font);
            out->ascent = m->ascent;
            out->descent = m->descent;
            out->line_height = m->line_height;
            out->synth = text_font_synth(out->font);
            return;
        }
    }
    /* synthetic Arial-like metrics */
    out->ascent = roundf(size * 0.905f);
    out->descent = roundf(size * 0.212f);
    out->line_height = out->ascent + out->descent;
}

static float measure(Widgets *w, const WFont *f, const char *s, size_t len)
{
    (void)w;
    if (f->font) return text_measure(f->font, s, len, 0);
    {
        size_t i = 0;
        int cps = 0;
        while (i < len) {
            uint32_t cp;
            i += (size_t)kn_utf8_decode(s + i, len - i, &cp);
            cps++;
        }
        return (float)cps * f->size * 0.6f;
    }
}

/* Chrome: avgCharWidth = OS/2 xAvgCharWidth, maxCharWidth = head.xMax -
 * head.xMin (both rounded).  text.h does not expose them yet; approximate
 * avg with the '0' advance (Blink's own fallback) and max with the font
 * height. */
static void char_widths(Widgets *w, const WFont *f, float *avg, float *max)
{
    if (f->font && w->host.font_char_widths &&
        w->host.font_char_widths(w->host.user, f->font, avg, max)) {
        *avg = roundf(*avg);
        *max = roundf(*max);
        if (*max < *avg) *max = *avg;
        return;
    }
    *avg = roundf(measure(w, f, "0", 1));
    *max = roundf(f->ascent + f->descent);
    if (*max < *avg) *max = *avg;
}

static float resolved_line_height(const CssComputed *st, const WFont *f)
{
    if (!st) return f->line_height;
    switch (st->line_height.kind) {
    case CSS_LEN_NUMBER: return st->line_height.px * f->size;
    case CSS_LEN_VALUE: return st->line_height.px;
    default: return f->line_height;
    }
}

/* ------------------------------------------------------------------------- */
/* Options (select)                                                           */
/* ------------------------------------------------------------------------- */

static int is_option(Dom *dom, HtmlElem e) { return dom_tag_id(dom, e) == LXB_TAG_OPTION; }

/* Visits <option> children (also inside <optgroup>). */
static HtmlElem option_first(Dom *dom, HtmlElem sel)
{
    HtmlElem c = dom_first_child(dom, sel);
    while (c) {
        if (is_option(dom, c)) return c;
        if (dom_tag_id(dom, c) == LXB_TAG_OPTGROUP) {
            HtmlElem g = dom_first_child(dom, c);
            while (g && !is_option(dom, g)) g = dom_next(dom, g);
            if (g) return g;
        }
        c = dom_next(dom, c);
    }
    return 0;
}

static HtmlElem option_next(Dom *dom, HtmlElem opt)
{
    HtmlElem n = dom_next(dom, opt);
    HtmlElem parent;
    while (n && !is_option(dom, n)) n = dom_next(dom, n);
    if (n) return n;
    parent = dom_parent(dom, opt);
    if (parent && dom_tag_id(dom, parent) == LXB_TAG_OPTGROUP) {
        n = dom_next(dom, parent);
        while (n) {
            if (is_option(dom, n)) return n;
            if (dom_tag_id(dom, n) == LXB_TAG_OPTGROUP) {
                HtmlElem g = dom_first_child(dom, n);
                while (g && !is_option(dom, g)) g = dom_next(dom, g);
                if (g) return g;
            }
            n = dom_next(dom, n);
        }
    }
    return 0;
}

static int option_count(Dom *dom, HtmlElem sel)
{
    int n = 0;
    HtmlElem o;
    for (o = option_first(dom, sel); o; o = option_next(dom, o)) n++;
    return n;
}

static HtmlElem option_at(Dom *dom, HtmlElem sel, int index)
{
    HtmlElem o;
    if (index < 0) return 0;
    for (o = option_first(dom, sel); o; o = option_next(dom, o))
        if (index-- == 0) return o;
    return 0;
}

/* Option label: text content with whitespace collapsed and trimmed (Chrome
 * strips/collapses the option text). */
static void option_label(Dom *dom, HtmlElem opt, KnStrBuf *out)
{
    size_t len = 0;
    const char *label = dom_get_attr(dom, opt, "label", &len);
    char *text = NULL;
    const char *src;
    size_t i, n;
    int pending_space = 0;
    kn_sb_clear(out);
    if (label && len) {
        src = label;
        n = len;
    } else {
        text = dom_get_text(dom, opt);
        src = text ? text : "";
        n = strlen(src);
    }
    for (i = 0; i < n; i++) {
        if (kn_is_space((unsigned char)src[i])) {
            pending_space = 1;
            continue;
        }
        if (pending_space && out->len) kn_sb_append_char(out, ' ');
        pending_space = 0;
        kn_sb_append_char(out, src[i]);
    }
    if (!out->data) kn_sb_append(out, "", 0);
    free(text);
}

static int selected_index(Dom *dom, HtmlElem sel)
{
    int i = 0, first = -1, last_selected = -1;
    HtmlElem o;
    for (o = option_first(dom, sel); o; o = option_next(dom, o), i++) {
        if (first < 0) first = 0;
        if (dom_has_attr(dom, o, "selected")) last_selected = i;
    }
    return last_selected >= 0 ? last_selected : first;
}

static void select_apply_index(Dom *dom, HtmlElem sel, int index)
{
    int i = 0;
    HtmlElem o;
    for (o = option_first(dom, sel); o; o = option_next(dom, o), i++) {
        if (i == index) {
            if (!dom_has_attr(dom, o, "selected")) dom_set_attr(dom, o, "selected", "");
        } else if (dom_has_attr(dom, o, "selected")) {
            dom_remove_attr(dom, o, "selected");
        }
    }
}

static void option_value(Dom *dom, HtmlElem opt, KnStrBuf *out)
{
    size_t len = 0;
    const char *v = dom_get_attr(dom, opt, "value", &len);
    if (v) {
        sb_set(out, v, len);
    } else {
        option_label(dom, opt, out);
    }
}

int widgets_select_index(Widgets *w, HtmlElem e)
{
    if (!w || widgets_kind(w, e) != WIDGET_SELECT) return -1;
    return selected_index(w->dom, e);
}

/* ------------------------------------------------------------------------- */
/* Values                                                                     */
/* ------------------------------------------------------------------------- */

static void mirror_value_attr(Widgets *w, WidgetState *s)
{
    const char *cur = dom_get_value(w->dom, s->elem);
    if (strcmp(cur, sb_cstr(&s->value)) != 0 || !dom_has_attr(w->dom, s->elem, "value"))
        dom_set_value(w->dom, s->elem, sb_cstr(&s->value));
    if (kind_is_text((WidgetKind)s->kind))
        dom_set_pseudo_state(w->dom, s->elem, DOM_PSEUDO_PLACEHOLDER_SHOWN,
                             dom_has_attr(w->dom, s->elem, "placeholder") && s->value.len == 0);
}

static void range_params(Dom *dom, HtmlElem e, double *min, double *max, double *step, int *decimals)
{
    double v;
    *min = attr_number(dom, e, "min", &v) ? v : 0.0;
    *max = attr_number(dom, e, "max", &v) ? v : 100.0;
    if (*max < *min) *max = *min;
    if (attr_eq(dom, e, "step", "any")) {
        *step = 0;
    } else {
        *step = attr_number(dom, e, "step", &v) && v > 0 ? v : 1.0;
    }
    *decimals = attr_decimals(dom, e, "step");
    if (attr_decimals(dom, e, "min") > *decimals) *decimals = attr_decimals(dom, e, "min");
    if (*step == 0) *decimals = -1;
}

static void number_params(Dom *dom, HtmlElem e, double *min, double *max, double *step, int *decimals)
{
    double v;
    *min = attr_number(dom, e, "min", &v) ? v : -HUGE_VAL;
    *max = attr_number(dom, e, "max", &v) ? v : HUGE_VAL;
    if (attr_eq(dom, e, "step", "any")) {
        *step = 0;
    } else {
        *step = attr_number(dom, e, "step", &v) && v > 0 ? v : 1.0;
    }
    *decimals = attr_decimals(dom, e, "step");
    if (*step == 0) *decimals = -1;
}

/* Sanitises a value for the control kind (HTML value sanitisation algorithm). */
static void sanitize_value(Widgets *w, WidgetState *s, const char *in)
{
    Dom *dom = w->dom;
    HtmlElem e = s->elem;
    char buf[64];
    switch ((WidgetKind)s->kind) {
    case WIDGET_RANGE: {
        double min, max, step, v;
        int decimals;
        char *end = NULL;
        range_params(dom, e, &min, &max, &step, &decimals);
        v = strtod(in, &end);
        if (end == in || *end != '\0' || v != v) v = min + (max - min) / 2; /* default: midpoint */
        v = snap_step(min, max, step, v);
        widgets_format_number(v, decimals, buf, sizeof buf);
        sb_set(&s->value, buf, strlen(buf));
        break;
    }
    case WIDGET_COLOR: {
        uint8_t r, g, b;
        char lower[8];
        int i;
        if (widgets_parse_hex_color(in, &r, &g, &b)) {
            for (i = 0; i < 7; i++) lower[i] = (char)kn_to_lower((unsigned char)in[i]);
            lower[7] = '\0';
            sb_set(&s->value, lower, 7);
        } else {
            sb_set(&s->value, "#000000", 7);
        }
        break;
    }
    case WIDGET_NUMBER: {
        /* Chrome: a value that is not a valid floating-point number becomes "" */
        char *end = NULL;
        size_t len = strlen(in);
        const char *t = kn_str_trim(in, &len);
        if (len == 0) {
            sb_set(&s->value, "", 0);
        } else {
            double v = strtod(t, &end);
            (void)v;
            if (end == t || (size_t)(end - t) != len) sb_set(&s->value, "", 0);
            else sb_set(&s->value, t, len);
        }
        break;
    }
    case WIDGET_TEXTAREA: {
        /* the API value normalises CRLF / CR to LF */
        KnStrBuf tmp = {0};
        for (const char *p = in; *p; p++) {
            if (*p == '\r') { kn_sb_append_char(&tmp, '\n'); if (p[1] == '\n') p++; }
            else kn_sb_append_char(&tmp, *p);
        }
        sb_set(&s->value, sb_cstr(&tmp), tmp.len);
        kn_sb_free(&tmp);
        break;
    }
    default: {
        /* text/url/password: strip CR/LF */
        KnStrBuf tmp = {0};
        const char *p;
        for (p = in; *p; p++)
            if (*p != '\n' && *p != '\r') kn_sb_append_char(&tmp, *p);
        sb_set(&s->value, sb_cstr(&tmp), tmp.len);
        kn_sb_free(&tmp);
        break;
    }
    }
}

static void state_init_value(Widgets *w, WidgetState *s)
{
    WidgetKind k = (WidgetKind)s->kind;
    s->inited = 1;
    if (k == WIDGET_SELECT || kind_is_check(k)) return;
    if (k == WIDGET_TEXTAREA && !dom_has_attr(w->dom, s->elem, "value")) {
        /* default value = the child text (the parser dropped the first newline) */
        char *txt = dom_get_text(w->dom, s->elem);
        sanitize_value(w, s, txt ? txt : "");
        free(txt);
    } else {
        sanitize_value(w, s, dom_get_value(w->dom, s->elem));
    }
    s->goal_x = -1;
    s->caret = s->anchor = (int)s->value.len;
    set_cstr(&s->commit_value, sb_cstr(&s->value));
    mirror_value_attr(w, s);
}

static void invalidate(Widgets *w, HtmlElem e, int relayout)
{
    if (w->host.invalidate) w->host.invalidate(w->host.user, e, relayout);
}

static int fire(Widgets *w, HtmlElem e, const char *type, int key)
{
    if (w->host.fire) return w->host.fire(w->host.user, e, type, key);
    return 1;
}

static void fire_input(Widgets *w, HtmlElem e) { fire(w, e, "input", 0); }

/* Fires `change` when the value moved since the last commit. */
static void commit_change(Widgets *w, WidgetState *s)
{
    if (s->commit_value && strcmp(s->commit_value, sb_cstr(&s->value)) == 0) return;
    set_cstr(&s->commit_value, sb_cstr(&s->value));
    fire(w, s->elem, "change", 0);
}

static void reset_blink(Widgets *w) { w->blink_start_ms = -1; }

static void value_changed(Widgets *w, WidgetState *s)
{
    mirror_value_attr(w, s);
    invalidate(w, s->elem, 0);
    reset_blink(w);
}

void widgets_set_value(Widgets *w, HtmlElem e, const char *value)
{
    WidgetState *s;
    if (!w || !value) return;
    s = state_get(w, e);
    if (!s) {
        if (e) dom_set_value(w->dom, e, value);
        return;
    }
    if (s->kind == WIDGET_SELECT) {
        /* select.value = v: pick the first option whose value matches */
        KnStrBuf tmp = {0};
        int i = 0;
        HtmlElem o;
        for (o = option_first(w->dom, e); o; o = option_next(w->dom, o), i++) {
            option_value(w->dom, o, &tmp);
            if (strcmp(sb_cstr(&tmp), value) == 0) {
                select_apply_index(w->dom, e, i);
                kn_sb_free(&tmp);
                invalidate(w, e, 0);
                return;
            }
        }
        kn_sb_free(&tmp);
        /* no match: Chrome deselects everything (selectedIndex = -1) */
        select_apply_index(w->dom, e, -1);
        invalidate(w, e, 0);
        return;
    }
    if (kind_is_check((WidgetKind)s->kind)) {
        dom_set_value(w->dom, e, value);
        return;
    }
    sanitize_value(w, s, value);
    s->caret = s->anchor = (int)s->value.len;
    s->scroll_x = 0;
    undo_clear(s);   /* Chrome drops the undo history on a script value write */
    /* programmatic writes do not produce `change` later */
    set_cstr(&s->commit_value, sb_cstr(&s->value));
    value_changed(w, s);
}

const char *widgets_get_value(Widgets *w, HtmlElem e)
{
    WidgetState *s;
    if (!w) return "";
    s = state_get(w, e);
    if (!s) return e ? dom_get_value(w->dom, e) : "";
    if (s->kind == WIDGET_SELECT) {
        int idx = selected_index(w->dom, e);
        HtmlElem o = option_at(w->dom, e, idx);
        if (!o) return "";
        option_value(w->dom, o, &s->value);
        return sb_cstr(&s->value);
    }
    if (kind_is_check((WidgetKind)s->kind)) {
        const char *v = dom_get_attr(w->dom, e, "value", NULL);
        return v ? v : "on";
    }
    return sb_cstr(&s->value);
}

void widgets_set_checked(Widgets *w, HtmlElem e, int on)
{
    if (!w || !e) return;
    dom_set_checked(w->dom, e, on);
    invalidate(w, e, 0);
}

int widgets_get_checked(Widgets *w, HtmlElem e)
{
    if (!w || !e) return 0;
    return dom_get_checked(w->dom, e) ? 1 : 0;
}

void widgets_activate(Widgets *w, HtmlElem e)
{
    WidgetKind k = widgets_kind(w, e);
    if (k == WIDGET_CHECKBOX) {
        dom_set_checked(w->dom, e, !dom_get_checked(w->dom, e));
    } else if (k == WIDGET_RADIO) {
        /* uncheck the other radios of the same name in the same parent form-less scope */
        size_t nlen = 0;
        const char *name = dom_get_attr(w->dom, e, "name", &nlen);
        if (name && nlen) {
            HtmlElem r;
            for (r = dom_next_in_tree(w->dom, 0, 0); r; r = dom_next_in_tree(w->dom, r, 0)) {
                size_t l2 = 0;
                const char *n2;
                if (r == e || classify(w->dom, r) != WIDGET_RADIO) continue;
                n2 = dom_get_attr(w->dom, r, "name", &l2);
                if (n2 && l2 == nlen && memcmp(n2, name, nlen) == 0 && dom_get_checked(w->dom, r)) {
                    dom_set_checked(w->dom, r, 0);
                    invalidate(w, r, 0);
                }
            }
        }
        if (dom_get_checked(w->dom, e)) return;
        dom_set_checked(w->dom, e, 1);
    } else {
        return;
    }
    invalidate(w, e, 0);
    fire(w, e, "input", 0);
    fire(w, e, "change", 0);
}

void widgets_select_set_index(Widgets *w, HtmlElem e, int index, int fire_events)
{
    int n;
    if (!w || widgets_kind(w, e) != WIDGET_SELECT) return;
    n = option_count(w->dom, e);
    if (index >= n) index = n - 1;
    if (index == selected_index(w->dom, e)) return;
    select_apply_index(w->dom, e, index);
    invalidate(w, e, 0);
    if (fire_events) {
        fire(w, e, "input", 0);
        fire(w, e, "change", 0);
    }
}

/* ------------------------------------------------------------------------- */
/* Lifetime                                                                   */
/* ------------------------------------------------------------------------- */

Widgets *widgets_create(Dom *dom, TextSystem *text, const WidgetsHost *host)
{
    Widgets *w = (Widgets *)calloc(1, sizeof *w);
    if (!w) return NULL;
    w->dom = dom;
    w->text = text;
    if (host) w->host = *host;
    w->theme = k_default_theme;
    w->blink_start_ms = -1;
    w->vw = 1280;
    w->vh = 720;
    return w;
}

void widgets_destroy(Widgets *w)
{
    uint32_t i;
    if (!w) return;
    for (i = 0; i < w->slot_cap; i++) state_free(w->slots[i]);
    free(w->slots);
    kn_sb_free(&w->clipboard);
    free(w);
}

WidgetsTheme *widgets_theme(Widgets *w) { return &w->theme; }
void widgets_set_gl_ready(Widgets *w, int ready) { w->gl_ready = ready; }
void widgets_set_viewport(Widgets *w, float vw, float vh) { w->vw = vw; w->vh = vh; }

void widgets_elem_removed(Widgets *w, HtmlElem e)
{
    HtmlElem x;
    if (!w || !e) return;
    /* the subtree: every state whose element is e or a descendant */
    if (w->popup) {
        for (x = w->popup; x; x = dom_parent(w->dom, x))
            if (x == e) { w->popup = 0; break; }
    }
    if (w->focused) {
        for (x = w->focused; x; x = dom_parent(w->dom, x))
            if (x == e) { w->focused = 0; break; }
    }
    if (e < w->slot_cap && w->slots[e]) {
        state_free(w->slots[e]);
        w->slots[e] = 0;
    }
    for (x = dom_next_in_tree(w->dom, e, e); x; x = dom_next_in_tree(w->dom, x, e)) {
        if (x < w->slot_cap && w->slots[x]) {
            state_free(w->slots[x]);
            w->slots[x] = 0;
        }
    }
}

HtmlElem widgets_popup_owner(Widgets *w) { return w ? w->popup : 0; }

/* ------------------------------------------------------------------------- */
/* Intrinsic sizes                                                            */
/* ------------------------------------------------------------------------- */

static int attr_size(Dom *dom, HtmlElem e, int def)
{
    double v;
    if (attr_number(dom, e, "size", &v) && v >= 1 && v < 1e6) return (int)v;
    return def;
}

static float range_intrinsic_h(Widgets *w, HtmlElem e, const CssComputed *st);

/* Blink NumberInputType::SizeShouldIncludeDecoration: without a size attribute a number input
 * with a finite min and max is as wide as its longest bound (the integer digits, a minus sign
 * included) plus the most fraction digits of min / max / step and the point, not the default
 * 20 characters (v7.1.4's 2FA popup: six <input type=number min=0 max=9> in one row). */
static int number_size(Dom *dom, HtmlElem e, int def)
{
    double lo, hi, step = 1;
    size_t len = 0;
    if (dom_get_attr(dom, e, "size", &len)) return attr_size(dom, e, def);
    const char *sv = dom_get_attr(dom, e, "step", &len);
    if (sv) {
        sv = kn_str_trim(sv, &len);
        if (len == 3 && (sv[0] | 32) == 'a' && (sv[1] | 32) == 'n' && (sv[2] | 32) == 'y') return def;
    }
    if (!attr_number(dom, e, "min", &lo) || !attr_number(dom, e, "max", &hi)) return def;
    if (!attr_number(dom, e, "step", &step) || step <= 0) step = 1;
    int left = 0, right = 0;
    for (int i = 0; i < 3; i++) { /* integer digits of min / max, fraction digits of all three */
        const double v = i == 0 ? lo : i == 1 ? hi : step;
        char buf[64];
        snprintf(buf, sizeof buf, "%.10g", v);
        if (strchr(buf, 'e')) return def;
        const char *dot = strchr(buf, '.');
        const int l = dot ? (int)(dot - buf) : (int)strlen(buf), r = dot ? (int)strlen(dot + 1) : 0;
        if (i < 2 && l > left) left = l;
        if (r > right) right = r;
    }
    const int n = left + (right ? right + 1 : 0);
    return n > 0 && n < 1000 ? n : def;
}

int widgets_intrinsic_size(Widgets *w, HtmlElem e, const CssComputed *st, float *out_w, float *out_h)
{
    WidgetKind k = widgets_kind(w, e);
    WFont f;
    if (k == WIDGET_NONE) return 0;
    if (!st && w->host.computed) st = w->host.computed(w->host.user, e);
    font_for(w, st, &f);
    /* fixed UA sizes scale with CSS zoom (font-based ones already do) */
    const float z = st && st->zoom > 0.0f ? st->zoom : 1.0f;
    switch (k) {
    case WIDGET_TEXT:
    case WIDGET_PASSWORD:
    case WIDGET_NUMBER: {
        float avg, max;
        int size = k == WIDGET_NUMBER ? number_size(w->dom, e, w->theme.default_size_attr)
                                      : attr_size(w->dom, e, w->theme.default_size_attr);
        char_widths(w, &f, &avg, &max);
        *out_w = ceilf(avg * (float)size) + (max - avg);
        *out_h = resolved_line_height(st, &f);
        return 1;
    }
    case WIDGET_TEXTAREA: {
        /* LayoutTextControlMultiLine: cols * avgCharWidth (+ the scrollbar
         * unless overflow is hidden) by rows * line-height (Chrome 152,
         * measured: 20 cols x 2 rows of 16px font2 = 255 x 44 content) */
        double v;
        int cols = attr_number(w->dom, e, "cols", &v) && v >= 1 && v < 1e5 ? (int)v : 20;
        int rows = attr_number(w->dom, e, "rows", &v) && v >= 1 && v < 1e5 ? (int)v : 2;
        float avg, max;
        char_widths(w, &f, &avg, &max);
        *out_w = avg * (float)cols + (st && st->overflow_y == CSS_OVERFLOW_HIDDEN ? 0 : w->theme.popup_scrollbar_w * z);
        *out_h = resolved_line_height(st, &f) * (float)rows;
        return 1;
    }
    case WIDGET_CHECKBOX:
    case WIDGET_RADIO:
        *out_w = *out_h = 13 * z;
        return 1;
    case WIDGET_RANGE:
        *out_w = 129 * z;
        *out_h = range_intrinsic_h(w, e, st);
        return 1;
    case WIDGET_COLOR:
        *out_w = (50 - 6) * z; /* 50x27 border box: 1px border + 1px 2px padding */
        *out_h = (27 - 4) * z;
        return 1;
    case WIDGET_SELECT: {
        KnStrBuf label = {0};
        float widest = 0;
        HtmlElem o;
        for (o = option_first(w->dom, e); o; o = option_next(w->dom, o)) {
            float tw;
            option_label(w->dom, o, &label);
            tw = measure(w, &f, sb_cstr(&label), label.len);
            if (tw > widest) widest = tw;
        }
        kn_sb_free(&label);
        *out_w = (w->theme.menulist_pad_start + w->theme.menulist_pad_end + w->theme.menulist_arrow_box) * z + ceilf(widest);
        *out_h = (w->theme.menulist_pad_top + w->theme.menulist_pad_bottom) * z + resolved_line_height(st, &f);
        return 1;
    }
    default:
        return 0;
    }
}

/* ------------------------------------------------------------------------- */
/* Display-list emission helpers                                              */
/* ------------------------------------------------------------------------- */

static void emit_rect(const WidgetPaintArgs *a, float x, float y, float w, float h, CssColor c)
{
    DlItem it;
    if (w <= 0 || h <= 0 || c.a == 0) return;
    memset(&it, 0, sizeof it);
    it.op = DL_RECT;
    it.u.rect.rect = dl_rect(x, y, w, h);
    it.u.rect.color = c;
    dl_push(a->out, &it);
}

static void emit_rrect(const WidgetPaintArgs *a, float x, float y, float w, float h, DlRadii r, CssColor c)
{
    DlItem it;
    if (w <= 0 || h <= 0 || c.a == 0) return;
    if (r.tl <= 0 && r.tr <= 0 && r.br <= 0 && r.bl <= 0) {
        emit_rect(a, x, y, w, h, c);
        return;
    }
    memset(&it, 0, sizeof it);
    it.op = DL_RRECT;
    it.u.rrect.rect = dl_rect(x, y, w, h);
    it.u.rrect.radii = r;
    it.u.rrect.color = c;
    dl_push(a->out, &it);
}

static void emit_border(const WidgetPaintArgs *a, float x, float y, float w, float h, DlRadii r, float bw, CssColor c)
{
    DlItem it;
    int i;
    if (bw <= 0 || c.a == 0) return;
    memset(&it, 0, sizeof it);
    it.op = DL_BORDER;
    it.u.border.rect = dl_rect(x, y, w, h);
    it.u.border.radii = r;
    for (i = 0; i < 4; i++) {
        it.u.border.width[i] = bw;
        it.u.border.color[i] = c;
        it.u.border.style[i] = DL_BORDER_SOLID;
    }
    dl_push(a->out, &it);
}

static void emit_clip(const WidgetPaintArgs *a, float x, float y, float w, float h)
{
    DlItem it;
    memset(&it, 0, sizeof it);
    it.op = DL_PUSH_CLIP_RECT;
    it.u.clip_rect.rect = dl_rect(x, y, w < 0 ? 0 : w, h < 0 ? 0 : h);
    dl_push(a->out, &it);
}

static void emit_pop_clip(const WidgetPaintArgs *a)
{
    DlItem it;
    memset(&it, 0, sizeof it);
    it.op = DL_POP_CLIP;
    dl_push(a->out, &it);
}

static void emit_transform(const WidgetPaintArgs *a, Mat2D m)
{
    DlItem it;
    memset(&it, 0, sizeof it);
    it.op = DL_PUSH_TRANSFORM;
    it.u.transform.m = m;
    dl_push(a->out, &it);
}

static void emit_pop_transform(const WidgetPaintArgs *a)
{
    DlItem it;
    memset(&it, 0, sizeof it);
    it.op = DL_POP_TRANSFORM;
    dl_push(a->out, &it);
}

/* A line segment of thickness `t` from (x0,y0) to (x1,y1) as a rotated rect. */
static void emit_segment(const WidgetPaintArgs *a, float x0, float y0, float x1, float y1, float t, CssColor c)
{
    float dx = x1 - x0, dy = y1 - y0;
    float len = sqrtf(dx * dx + dy * dy);
    float cs, sn;
    Mat2D m;
    if (len <= 0 || t <= 0) return;
    cs = dx / len;
    sn = dy / len;
    /* rotate about (x0, y0), extend by t/2 at both ends for round-ish joins */
    m.a = cs; m.b = sn; m.c = -sn; m.d = cs; m.e = x0; m.f = y0;
    emit_transform(a, m);
    emit_rect(a, -t / 2, -t / 2, len + t, t, c);
    emit_pop_transform(a);
}

static void emit_gradient_h(const WidgetPaintArgs *a, float x, float y, float w, float h,
                            const CssColor *stops, int n, int vertical)
{
    DlItem it;
    int i;
    if (n < 2 || n > CSS_MAX_GRADIENT_STOPS) return;
    memset(&it, 0, sizeof it);
    it.op = DL_GRADIENT;
    it.u.gradient.rect = dl_rect(x, y, w, h);
    it.u.gradient.box = dl_rect(x, y, w, h);
    it.u.gradient.gradient.kind = CSS_GRADIENT_LINEAR;
    it.u.gradient.gradient.angle_deg = vertical ? 180.0f : 90.0f;
    it.u.gradient.gradient.stop_count = (uint8_t)n;
    for (i = 0; i < n; i++) {
        it.u.gradient.gradient.stops[i].color = stops[i];
        it.u.gradient.gradient.stops[i].has_pos = 1;
        it.u.gradient.gradient.stops[i].pos.kind = CSS_LEN_VALUE;
        it.u.gradient.gradient.stops[i].pos.pct = 100.0f * (float)i / (float)(n - 1);
    }
    dl_push(a->out, &it);
}

/* Emits glyphs for a run; returns the advance.  Without GL/text system only
 * the advance is computed (nothing drawn). */
static float emit_text(Widgets *w, const WidgetPaintArgs *a, const WFont *f, const char *s, size_t len,
                       float pen_x, float baseline, CssColor color)
{
    float adv = 0;
    int n, i, out_n = 0;
    Glyph *glyphs;
    DlGlyphQuad *quads;
    DlItem it;
    if (len == 0 || color.a == 0) return measure(w, f, s, len);
    if (!f->font || !w->gl_ready || !a->alloc) return measure(w, f, s, len);
    n = text_shape(f->font, s, len, 0, NULL, 0, &adv);
    if (n <= 0) return adv;
    glyphs = (Glyph *)malloc((size_t)n * sizeof *glyphs);
    if (!glyphs) return adv;
    text_shape(f->font, s, len, 0, glyphs, n, &adv);
    quads = (DlGlyphQuad *)a->alloc(a->alloc_user, (size_t)n * sizeof *quads);
    if (!quads) {
        free(glyphs);
        return adv;
    }
    for (i = 0; i < n; i++) {
        GlyphQuad q;
        float device_pen = (pen_x + glyphs[i].x) * a->device_scale;
        glyphs[i].phase = text_phase(device_pen);
        if (!text_glyph_quad(w->text, &glyphs[i], f->size * a->device_scale, f->synth, &q) || !q.texture) continue;
        quads[out_n].tex = q.texture;
        quads[out_n].uv[0] = q.u0;
        quads[out_n].uv[1] = q.v0;
        quads[out_n].uv[2] = q.u1;
        quads[out_n].uv[3] = q.v1;
        quads[out_n].x = (floorf(device_pen) + q.x_off) / a->device_scale;
        quads[out_n].y = (baseline * a->device_scale + q.y_off) / a->device_scale;
        quads[out_n].w = q.w / a->device_scale;
        quads[out_n].h = q.h / a->device_scale;
        out_n++;
    }
    free(glyphs);
    if (out_n) {
        memset(&it, 0, sizeof it);
        it.op = DL_GLYPHS;
        it.u.glyphs.quads = quads;
        it.u.glyphs.n = out_n;
        it.u.glyphs.color = color;
        dl_push(a->out, &it);
    }
    return adv;
}

static DlRadii radii_from(const CssComputed *st, float bw, float bh)
{
    DlRadii r = {0, 0, 0, 0};
    float sum;
    if (!st) return r;
    r.tl = resolve_len(st->border_radius[CSS_TOP_LEFT], bw, 0);
    r.tr = resolve_len(st->border_radius[CSS_TOP_RIGHT], bw, 0);
    r.br = resolve_len(st->border_radius[CSS_BOTTOM_RIGHT], bw, 0);
    r.bl = resolve_len(st->border_radius[CSS_BOTTOM_LEFT], bw, 0);
    /* clamp like CSS: scale so adjacent radii never exceed the box */
    sum = r.tl + r.tr; if (sum > bw && sum > 0) { r.tl *= bw / sum; r.tr *= bw / sum; }
    sum = r.bl + r.br; if (sum > bw && sum > 0) { r.bl *= bw / sum; r.br *= bw / sum; }
    sum = r.tl + r.bl; if (sum > bh && sum > 0) { r.tl *= bh / sum; r.bl *= bh / sum; }
    sum = r.tr + r.br; if (sum > bh && sum > 0) { r.tr *= bh / sum; r.br *= bh / sum; }
    return r;
}

static const CssComputed *pseudo(Widgets *w, HtmlElem e, int pe)
{
    if (!w->host.pseudo_computed) return NULL;
    return w->host.pseudo_computed(w->host.user, e, pe);
}

static const CssComputed *computed(Widgets *w, HtmlElem e)
{
    if (!w->host.computed) return NULL;
    return w->host.computed(w->host.user, e);
}

/* ------------------------------------------------------------------------- */
/* Text editing geometry                                                      */
/* ------------------------------------------------------------------------- */

/* The string drawn for a text-like control: value, or bullets for password. */
static void display_text(const WidgetState *s, KnStrBuf *out)
{
    kn_sb_clear(out);
    if (s->kind == WIDGET_PASSWORD) {
        size_t i = 0;
        const char *v = sb_cstr(&s->value);
        while (i < s->value.len) {
            uint32_t cp;
            i += (size_t)kn_utf8_decode(v + i, s->value.len - i, &cp);
            kn_sb_append(out, "\xe2\x80\xa2", 3); /* U+2022 */
        }
    } else {
        kn_sb_append(out, sb_cstr(&s->value), s->value.len);
    }
    if (!out->data) kn_sb_append(out, "", 0);
}

/* Byte offset in the display string for a byte offset in the value. */
static size_t display_offset(const WidgetState *s, int value_off)
{
    if (s->kind != WIDGET_PASSWORD) return (size_t)value_off;
    {
        size_t i = 0, cps = 0;
        const char *v = sb_cstr(&s->value);
        while (i < (size_t)value_off && i < s->value.len) {
            uint32_t cp;
            i += (size_t)kn_utf8_decode(v + i, s->value.len - i, &cp);
            cps++;
        }
        return cps * 3;
    }
}

typedef struct TextLayout {
    WFont font;
    float cx, cy, cw, ch;      /* content box                                   */
    float editor_y, lh;        /* inner editor top and line height               */
    float baseline;
    float text_x;              /* pen x of the text start (alignment + scroll)   */
    float text_w;
    KnStrBuf disp;
} TextLayout;

static void text_layout(Widgets *w, WidgetState *s, const CssComputed *st, const WidgetGeom *g, TextLayout *tl)
{
    float align_off = 0;
    memset(tl, 0, sizeof *tl);
    font_for(w, st, &tl->font);
    tl->cx = widget_geom_content_x(g);
    tl->cy = widget_geom_content_y(g);
    tl->cw = widget_geom_content_w(g);
    tl->ch = widget_geom_content_h(g);
    tl->lh = resolved_line_height(st, &tl->font);
    tl->editor_y = tl->cy + (tl->ch - tl->lh) / 2;
    tl->baseline = tl->editor_y + (tl->lh - (tl->font.ascent + tl->font.descent)) / 2 + tl->font.ascent;
    display_text(s, &tl->disp);
    tl->text_w = measure(w, &tl->font, sb_cstr(&tl->disp), tl->disp.len);
    if (st && tl->text_w <= tl->cw) {
        int align = st->text_align;
        int rtl = st->direction == CSS_DIRECTION_RTL;
        if (align == CSS_TEXT_ALIGN_CENTER) align_off = (tl->cw - tl->text_w) / 2;
        else if (align == CSS_TEXT_ALIGN_RIGHT || (align == CSS_TEXT_ALIGN_END && !rtl) || (align == CSS_TEXT_ALIGN_START && rtl))
            align_off = tl->cw - tl->text_w;
    }
    /* keep the caret visible: scroll_x in [0, text_w - cw] */
    {
        float max_scroll = tl->text_w - tl->cw;
        float caret_x;
        if (max_scroll < 0) max_scroll = 0;
        caret_x = measure(w, &tl->font, sb_cstr(&tl->disp), display_offset(s, s->caret));
        if (caret_x - s->scroll_x > tl->cw) s->scroll_x = caret_x - tl->cw;
        if (caret_x - s->scroll_x < 0) s->scroll_x = caret_x;
        s->scroll_x = clampf(s->scroll_x, 0, max_scroll);
    }
    tl->text_x = tl->cx + align_off - s->scroll_x;
}

static void text_layout_free(TextLayout *tl) { kn_sb_free(&tl->disp); }

static float caret_x_for(Widgets *w, const TextLayout *tl, const WidgetState *s, int off)
{
    return tl->text_x + measure(w, &tl->font, sb_cstr(&tl->disp), display_offset(s, off));
}

/* Byte offset in the value nearest to local x. */
static int offset_from_x(Widgets *w, const TextLayout *tl, const WidgetState *s, float x)
{
    const char *v = sb_cstr(&s->value);
    size_t i = 0;
    int best = 0;
    float best_d = fabsf(x - tl->text_x);
    while (i < s->value.len) {
        uint32_t cp;
        float cx;
        float d;
        i += (size_t)kn_utf8_decode(v + i, s->value.len - i, &cp);
        cx = caret_x_for(w, tl, s, (int)i);
        d = fabsf(x - cx);
        if (d < best_d) {
            best_d = d;
            best = (int)i;
        }
    }
    return best;
}

/* ---- <textarea>: pre-wrap lines ------------------------------------------ */

typedef struct MlLine { int start, end; } MlLine;   /* value bytes [start, end), no '\n' */

typedef struct MlLayout {
    WFont font;
    float cx, cy, cw, ch;      /* content box                                   */
    float lh;                  /* line height                                   */
    float top;                 /* y of the first line (content y - scroll_y)    */
    MlLine *lines;
    int n, cap;
    /* code view (s->code): hard lines only, tab stops, gutter, horizontal scroll */
    const WidgetCode *code;
    float tx;                  /* x of column 0 (cx, or cx + gutter + pad - scroll_x) */
    float tab_w;               /* tab stop width                                 */
    float gutter_w;            /* gutter width (0: none)                         */
    float num_w;               /* line-number column width                       */
    float text_x, text_w;      /* visible text area (code view)                  */
} MlLayout;

static int next_cp(const char *s, int len, int off);
static int prev_cp(const char *s, int off);
static int caret_visible(Widgets *w, double now_ms);

static void ml_push(MlLayout *ml, int a, int b)
{
    if (ml->n == ml->cap) {
        int cap = ml->cap ? ml->cap * 2 : 8;
        MlLine *nl = realloc(ml->lines, (size_t)cap * sizeof(MlLine));
        if (!nl) return;
        ml->lines = nl;
        ml->cap = cap;
    }
    ml->lines[ml->n].start = a;
    ml->lines[ml->n].end = b;
    ml->n++;
}

/* white-space: pre-wrap + overflow-wrap: break-word: hard breaks at '\n',
 * soft breaks after spaces (trailing spaces hang), words wider than the
 * line break between characters. */
static void ml_wrap(Widgets *w, MlLayout *ml, const char *v, int len, float width)
{
    int p = 0;
    for (;;) {
        int e = p;
        while (e < len && v[e] != '\n') e++;
        int ls = p;
        if (ls == e || ml->code) ml_push(ml, ls, e);
        while (ls < e && !ml->code) {
            int i = ls, brk = -1;
            while (i < e) {
                int ni = next_cp(v, e, i);
                if (v[i] != ' ' && i > ls && measure(w, &ml->font, v + ls, (size_t)(ni - ls)) > width) break;
                if (v[i] == ' ') brk = ni;
                i = ni;
            }
            if (i >= e) { ml_push(ml, ls, e); break; }
            int cut = brk > ls ? brk : i;
            ml_push(ml, ls, cut);
            ls = cut;
        }
        if (e >= len) break;
        p = e + 1;
        if (p == len) { ml_push(ml, p, p); break; }
    }
}

static void ml_free(MlLayout *ml) { free(ml->lines); ml->lines = NULL; ml->n = ml->cap = 0; }

/* Advance of s[0, len) at the start of a line: in a code view a tab moves to
 * the next multiple of tab_w (CSS tab-size), elsewhere the font's advance. */
static float ml_measure(Widgets *w, const MlLayout *ml, const char *s, int len)
{
    if (!ml->code || ml->tab_w <= 0) return measure(w, &ml->font, s, (size_t)len);
    float x = 0;
    int a = 0;
    for (int i = 0; i <= len; i++) {
        if (i < len && s[i] != '\t') continue;
        if (i > a) x += measure(w, &ml->font, s + a, (size_t)(i - a));
        if (i < len) x = (floorf(x / ml->tab_w + 1e-3f) + 1) * ml->tab_w;
        a = i + 1;
    }
    return x;
}

static int ml_line_of(const MlLayout *ml, int off)
{
    int k = 0;
    for (int i = 0; i < ml->n; i++) if (ml->lines[i].start <= off) k = i;
    return k;
}

static float ml_x(Widgets *w, const MlLayout *ml, const char *v, int off)
{
    const MlLine *l = &ml->lines[ml_line_of(ml, off)];
    return ml->tx + ml_measure(w, ml, v + l->start, off - l->start);
}

static float ml_line_y(const MlLayout *ml, int line) { return ml->top + (float)line * ml->lh; }

static int ml_offset_in_line(Widgets *w, const MlLayout *ml, const char *v, int line, float x)
{
    const MlLine *l = &ml->lines[line];
    int best = l->start, i = l->start;
    float best_d = fabsf(x - ml->tx);
    while (i < l->end) {
        i = next_cp(v, l->end, i);
        float d = fabsf(x - (ml->tx + ml_measure(w, ml, v + l->start, i - l->start)));
        if (d < best_d) { best_d = d; best = i; }
    }
    /* a soft-wrapped line's end is the next line's start: stay on this line */
    if (best == l->end && line + 1 < ml->n && ml->lines[line + 1].start == l->end && l->end > l->start)
        best = prev_cp(v, l->end);
    return best;
}

static int ml_offset_at(Widgets *w, const MlLayout *ml, const char *v, float x, float y)
{
    int line = (int)floorf((y - ml->top) / ml->lh);
    if (line < 0) return 0;
    if (line >= ml->n) return ml->lines[ml->n - 1].end;
    return ml_offset_in_line(w, ml, v, line, x);
}

/* Lays out the value (or `text`) in the content box and keeps the caret
 * inside the visible rows (scroll_y). */
static void ml_layout(Widgets *w, WidgetState *s, const CssComputed *st, const WidgetGeom *g, const char *text, int len, MlLayout *ml)
{
    memset(ml, 0, sizeof(*ml));
    font_for(w, st, &ml->font);
    ml->cx = widget_geom_content_x(g);
    ml->cy = widget_geom_content_y(g);
    ml->cw = widget_geom_content_w(g);
    ml->ch = widget_geom_content_h(g);
    ml->lh = resolved_line_height(st, &ml->font);
    if (ml->lh <= 0) ml->lh = 1;
    ml->code = s->code;
    ml->tx = ml->cx;
    ml_wrap(w, ml, text, len, ml->cw);
    if (!ml->n) ml_push(ml, 0, 0);
    float max_scroll_x = 0;
    if (ml->code) {
        /* code view: [gutter][pad][text ... pad], scrolled horizontally */
        const WidgetsCodeView *cv = &ml->code->cfg;
        ml->tab_w = (float)(cv->tab_size > 0 ? cv->tab_size : 4) * measure(w, &ml->font, " ", 1);
        if (cv->gutter) {
            char num[16];
            int k = snprintf(num, sizeof num, "%d", ml->n);
            ml->num_w = fmaxf(cv->gutter_min_w, measure(w, &ml->font, num, (size_t)k));
            ml->gutter_w = cv->gutter_pad_left + ml->num_w + cv->gutter_pad_right + cv->gutter_extra;
        }
        ml->text_x = ml->cx + ml->gutter_w + cv->gutter_gap;
        ml->text_w = fmaxf(0, ml->cw - ml->gutter_w - cv->gutter_gap);
        float widest = 0;
        for (int i = 0; i < ml->n; i++)
            widest = fmaxf(widest, ml_measure(w, ml, text + ml->lines[i].start, ml->lines[i].end - ml->lines[i].start));
        max_scroll_x = fmaxf(0, widest + 2 * cv->text_pad + w->theme.caret_width - ml->text_w);
    }
    float content_h = (float)ml->n * ml->lh;
    float max_scroll = fmaxf(0, content_h - ml->ch);
    /* scroll the caret into view when it moved or the text changed (not on
     * wheel scrolling, which leaves the caret where it is) */
    if (text == sb_cstr(&s->value) && w->focused == s->elem && (s->seen_caret != s->caret + 1 || s->seen_len != s->value.len)) {
        const int line = ml_line_of(ml, s->caret);
        float cy = (float)line * ml->lh;
        s->seen_caret = s->caret + 1;
        s->seen_len = s->value.len;
        if (cy < s->scroll_y) s->scroll_y = cy;
        if (cy + ml->lh > s->scroll_y + ml->ch) s->scroll_y = cy + ml->lh - ml->ch;
        if (ml->code) {
            const float pad = ml->code->cfg.text_pad;
            float x = ml_measure(w, ml, text + ml->lines[line].start, s->caret - ml->lines[line].start);
            if (x < s->scroll_x) s->scroll_x = x;
            if (x + 2 * pad + w->theme.caret_width > s->scroll_x + ml->text_w) s->scroll_x = x + 2 * pad + w->theme.caret_width - ml->text_w;
        }
    }
    s->scroll_y = clampf(s->scroll_y, 0, max_scroll);
    ml->top = ml->cy - s->scroll_y;
    if (ml->code) {
        s->scroll_x = clampf(s->scroll_x, 0, max_scroll_x);
        ml->tx = ml->text_x + ml->code->cfg.text_pad - s->scroll_x;
    }
}

static void paint_code(Widgets *w, WidgetState *s, const CssComputed *st, const WidgetGeom *g, const WidgetPaintArgs *a);

static void paint_textarea(Widgets *w, WidgetState *s, const CssComputed *st, const WidgetGeom *g, const WidgetPaintArgs *a)
{
    MlLayout ml;
    if (s->code) {
        paint_code(w, s, st, g, a);
        return;
    }
    CssColor color = st ? st->color : rgba(0, 0, 0, 255);
    const int focused = w->focused == s->elem;
    const char *v = sb_cstr(&s->value);
    const char *text = v;
    int len = (int)s->value.len;
    size_t plen = 0;
    const char *ph = NULL;
    if (len == 0) {
        ph = dom_get_attr(w->dom, s->elem, "placeholder", &plen);
        if (ph && plen) { text = ph; len = (int)plen; }
    }
    ml_layout(w, s, st, g, text, len, &ml);
    /* clip to the padding box (the scrolled inner editor) */
    emit_clip(a, g->x + g->border[CSS_LEFT], g->y + g->border[CSS_TOP],
              g->w - g->border[CSS_LEFT] - g->border[CSS_RIGHT], g->h - g->border[CSS_TOP] - g->border[CSS_BOTTOM]);
    const float base_off = (ml.lh - (ml.font.ascent + ml.font.descent)) / 2 + ml.font.ascent;
    CssColor tc = color;
    if (text != v) {
        const CssComputed *pst = pseudo(w, s->elem, CSS_PE_PLACEHOLDER);
        tc = pst ? resolve_color(pst->color, st) : w->theme.placeholder_color;
    }
    const int lo = s->caret < s->anchor ? s->caret : s->anchor, hi = s->caret < s->anchor ? s->anchor : s->caret;
    for (int i = 0; i < ml.n; i++) {
        const MlLine *l = &ml.lines[i];
        const float y = ml_line_y(&ml, i);
        if (y + ml.lh < ml.cy - ml.lh || y > ml.cy + ml.ch + ml.lh) continue;
        if (text == v && focused && hi > lo && hi >= l->start && lo <= l->end) {
            int a0 = lo > l->start ? lo : l->start, a1 = hi < l->end ? hi : l->end;
            float x0 = ml.cx + measure(w, &ml.font, v + l->start, (size_t)(a0 - l->start));
            float x1 = ml.cx + measure(w, &ml.font, v + l->start, (size_t)(a1 - l->start));
            if (hi > l->end && l->end < (int)s->value.len && v[l->end] == '\n') x1 += measure(w, &ml.font, " ", 1);
            if (x1 > x0) emit_rect(a, x0, y, x1 - x0, ml.lh, w->theme.selection_bg);
        }
        if (l->end > l->start) emit_text(w, a, &ml.font, text + l->start, (size_t)(l->end - l->start), ml.cx, y + base_off, tc);
    }
    if (focused && caret_visible(w, a->now_ms)) {
        const int line = text == v ? ml_line_of(&ml, s->caret) : 0;
        const float cx = text == v ? ml_x(w, &ml, v, s->caret) : ml.cx;
        const float top = ml_line_y(&ml, line) + base_off - ml.font.ascent;
        emit_rect(a, floorf(cx), top, w->theme.caret_width, ml.font.ascent + ml.font.descent, color);
    }
    emit_pop_clip(a);
    ml_free(&ml);
}

/* First span that ends after `off` (spans are sorted). */
static int code_span_from(const WidgetCode *c, int off)
{
    int lo = 0, hi = c->nspans;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (c->spans[mid].end <= off) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

/* One line of a code view: runs of one colour between span edges and tabs. */
static void paint_code_line(Widgets *w, const MlLayout *ml, const char *v, const MlLine *l, float y, float base_off,
                            CssColor def, const WidgetPaintArgs *a)
{
    const WidgetCode *c = ml->code;
    int si = code_span_from(c, l->start);
    float x = 0;     /* relative to tx */
    int p = l->start;
    while (p < l->end) {
        if (v[p] == '\t') {
            x = (floorf(x / ml->tab_w + 1e-3f) + 1) * ml->tab_w;
            p++;
            continue;
        }
        while (si < c->nspans && c->spans[si].end <= p) si++;
        const WidgetsCodeSpan *sp = si < c->nspans && c->spans[si].start <= p ? &c->spans[si] : NULL;
        int q = sp ? sp->end : (si < c->nspans ? c->spans[si].start : l->end);
        if (q > l->end) q = l->end;
        for (int i = p; i < q; i++) if (v[i] == '\t') { q = i; break; }
        if (q <= p) q = next_cp(v, l->end, p);
        const CssColor col = sp ? sp->color : def;
        /* skip what is left of the visible text area (long lines) */
        const float adv = ml->tx + x > ml->text_x + ml->text_w ? 0 : emit_text(w, a, &ml->font, v + p, (size_t)(q - p), ml->tx + x, y + base_off, col);
        if (sp && (sp->flags & WIDGETS_SPAN_UNDERLINE) && adv > 0)
            emit_rect(a, ml->tx + x, y + base_off + fmaxf(1, roundf(ml->font.size * 0.08f)), adv, 1, col);
        x += adv;
        p = q;
    }
}

/* Code view: gutter, line backgrounds, selection, coloured text, caret. */
static void paint_code(Widgets *w, WidgetState *s, const CssComputed *st, const WidgetGeom *g, const WidgetPaintArgs *a)
{
    MlLayout ml;
    const WidgetCode *c = s->code;
    const WidgetsCodeView *cv = &c->cfg;
    CssColor color = st ? st->color : rgba(0, 0, 0, 255);
    const int focused = w->focused == s->elem;
    const char *v = sb_cstr(&s->value);
    ml_layout(w, s, st, g, v, (int)s->value.len, &ml);
    const float pad_x = g->x + g->border[CSS_LEFT], pad_y = g->y + g->border[CSS_TOP];
    const float pad_w = g->w - g->border[CSS_LEFT] - g->border[CSS_RIGHT], pad_h = g->h - g->border[CSS_TOP] - g->border[CSS_BOTTOM];
    const float base_off = (ml.lh - (ml.font.ascent + ml.font.descent)) / 2 + ml.font.ascent;
    const int lo = s->caret < s->anchor ? s->caret : s->anchor, hi = s->caret < s->anchor ? s->anchor : s->caret;
    const int caret_line = ml_line_of(&ml, s->caret);
    const float right = ml.text_x + ml.text_w;
    int first = (int)floorf((ml.cy - ml.top) / ml.lh) - 1, last = (int)ceilf((ml.cy + ml.ch - ml.top) / ml.lh) + 1;
    if (first < 0) first = 0;
    if (last > ml.n) last = ml.n;
    /* rows: the caret line, marked lines, the selection, then the text */
    emit_clip(a, ml.text_x, pad_y, fmaxf(0, pad_x + pad_w - ml.text_x), pad_h);
    if (cv->active_line_bg.a && hi == lo)
        emit_rect(a, ml.text_x, ml_line_y(&ml, caret_line), ml.text_w, ml.lh, cv->active_line_bg);
    for (int k = 0; k < c->nlines; k++) {
        const WidgetsCodeLine *cl = &c->lines[k];
        const int i = cl->line - 1;
        if (!cl->bg.a || i < first || i >= last) continue;
        const MlLine *l = &ml.lines[i];
        const float y = ml_line_y(&ml, i);
        if (cl->full) emit_rect(a, ml.text_x, y, ml.text_w, ml.lh, cl->bg);
        else {
            const float x1 = ml.tx + ml_measure(w, &ml, v + l->start, l->end - l->start);
            if (x1 > ml.tx) emit_rect(a, ml.tx, y, x1 - ml.tx, ml.lh, cl->bg);
        }
    }
    const CssColor sel_bg = cv->selection_bg.a ? cv->selection_bg : w->theme.selection_bg;
    for (int i = first; i < last && hi > lo; i++) {
        const MlLine *l = &ml.lines[i];
        if (hi < l->start || lo > l->end) continue;
        const int a0 = lo > l->start ? lo : l->start, a1 = hi < l->end ? hi : l->end;
        const float x0 = ml.tx + ml_measure(w, &ml, v + l->start, a0 - l->start);
        /* CodeMirror: a selection that runs past the line end fills the row */
        const float x1 = hi > l->end ? right : ml.tx + ml_measure(w, &ml, v + l->start, a1 - l->start);
        if (x1 > x0) emit_rect(a, x0, ml_line_y(&ml, i), x1 - x0, ml.lh, sel_bg);
    }
    for (int i = first; i < last; i++)
        if (ml.lines[i].end > ml.lines[i].start) paint_code_line(w, &ml, v, &ml.lines[i], ml_line_y(&ml, i), base_off, color, a);
    if (focused && caret_visible(w, a->now_ms)) {
        const float cx = ml_x(w, &ml, v, s->caret);
        const float top = ml_line_y(&ml, caret_line) + base_off - ml.font.ascent;
        if (cx >= ml.text_x && cx <= right)
            emit_rect(a, floorf(cx), top, w->theme.caret_width, ml.font.ascent + ml.font.descent, cv->caret.a ? cv->caret : color);
    }
    emit_pop_clip(a);
    /* the gutter: fixed horizontally, numbers right-aligned, markers right of them */
    if (cv->gutter) {
        emit_clip(a, pad_x, pad_y, fminf(ml.gutter_w, pad_w), pad_h);
        if (cv->gutter_bg.a) emit_rect(a, ml.cx, pad_y, ml.gutter_w, pad_h, cv->gutter_bg);
        for (int i = first; i < last; i++) {
            char num[16];
            const int k = snprintf(num, sizeof num, "%d", i + 1);
            const float nw = measure(w, &ml.font, num, (size_t)k);
            emit_text(w, a, &ml.font, num, (size_t)k, ml.cx + cv->gutter_pad_left + ml.num_w - nw, ml_line_y(&ml, i) + base_off, cv->gutter_fg);
        }
        for (int k = 0; k < c->nlines; k++) {
            const WidgetsCodeLine *cl = &c->lines[k];
            const int i = cl->line - 1;
            if (!cl->marker || i < first || i >= last) continue;
            char m[4];
            const int n = kn_utf8_encode(cl->marker, m);
            const float mw = measure(w, &ml.font, m, (size_t)n);
            const float mx = ml.cx + cv->gutter_pad_left + ml.num_w + cv->gutter_pad_right + (cv->gutter_extra - mw) / 2;
            emit_text(w, a, &ml.font, m, (size_t)n, mx, ml_line_y(&ml, i) + base_off, cl->marker_color.a ? cl->marker_color : cv->gutter_fg);
        }
        emit_pop_clip(a);
    }
    ml_free(&ml);
}

/* Caret offset under a local point (mouse). */
static int textarea_offset_at(Widgets *w, WidgetState *s, const WidgetGeom *g, float x, float y)
{
    MlLayout ml;
    const char *v = sb_cstr(&s->value);
    ml_layout(w, s, computed(w, s->elem), g, v, (int)s->value.len, &ml);
    int off = ml_offset_at(w, &ml, v, x, y);
    ml_free(&ml);
    return off;
}

/* Up / Down / Home / End / PageUp / PageDown inside a textarea.  Returns 1
 * when handled (caret moved). */
static int local_geom(Widgets *w, HtmlElem e, float vx, float vy, WidgetGeom *g, float *lx, float *ly);

static int textarea_nav(Widgets *w, WidgetState *s, int key, int shift)
{
    WidgetGeom g;
    float lx, ly;
    MlLayout ml;
    const char *v = sb_cstr(&s->value);
    if (!local_geom(w, s->elem, 0, 0, &g, &lx, &ly)) return 0;
    ml_layout(w, s, computed(w, s->elem), &g, v, (int)s->value.len, &ml);
    const int line = ml_line_of(&ml, s->caret);
    const int page = ml.lh > 0 ? (int)fmaxf(1, floorf(ml.ch / ml.lh) - 1) : 1;
    int target = s->caret, keep_goal = 0;
    switch (key) {
    case WK_HOME: target = ml.lines[line].start; break;
    case WK_END: {
        target = ml.lines[line].end;
        if (line + 1 < ml.n && ml.lines[line + 1].start == target && target > ml.lines[line].start) target = prev_cp(v, target);
        break;
    }
    case WK_UP: case WK_DOWN: case WK_PAGE_UP: case WK_PAGE_DOWN: {
        int d = key == WK_UP ? -1 : key == WK_DOWN ? 1 : key == WK_PAGE_UP ? -page : page;
        int nl = line + d;
        if (s->goal_x < 0) s->goal_x = ml_x(w, &ml, v, s->caret) - ml.cx;
        if (nl < 0) target = 0;
        else if (nl >= ml.n) target = (int)s->value.len;
        else target = ml_offset_in_line(w, &ml, v, nl, ml.cx + s->goal_x);
        keep_goal = 1;
        break;
    }
    default:
        ml_free(&ml);
        return 0;
    }
    ml_free(&ml);
    s->caret = target;
    if (!shift) s->anchor = target;
    if (!keep_goal) s->goal_x = -1;
    return 1;
}

/* ------------------------------------------------------------------------- */
/* Painting                                                                   */
/* ------------------------------------------------------------------------- */

static int caret_visible(Widgets *w, double now_ms)
{
    double t;
    if (w->blink_start_ms < 0) {
        w->blink_start_ms = now_ms;
        return 1;
    }
    t = now_ms - w->blink_start_ms;
    if (t < 0) t = 0;
    return ((long)(t / w->theme.caret_blink_ms) & 1) == 0;
}

static void paint_text_control(Widgets *w, WidgetState *s, const CssComputed *st, const WidgetGeom *g, const WidgetPaintArgs *a)
{
    TextLayout tl;
    CssColor color = st ? st->color : rgba(0, 0, 0, 255);
    int focused = (w->focused == s->elem);
    text_layout(w, s, st, g, &tl);
    emit_clip(a, tl.cx, tl.cy, tl.cw, tl.ch);
    if (s->value.len == 0) {
        size_t plen = 0;
        const char *ph = dom_get_attr(w->dom, s->elem, "placeholder", &plen);
        if (ph && plen) {
            const CssComputed *pst = pseudo(w, s->elem, CSS_PE_PLACEHOLDER);
            CssColor pc = pst ? resolve_color(pst->color, st) : w->theme.placeholder_color;
            float ph_w = measure(w, &tl.font, ph, plen);
            float x = tl.cx;
            if (st && ph_w <= tl.cw) {
                if (st->text_align == CSS_TEXT_ALIGN_CENTER) x += (tl.cw - ph_w) / 2;
                else if (st->text_align == CSS_TEXT_ALIGN_RIGHT || st->text_align == CSS_TEXT_ALIGN_END) x += tl.cw - ph_w;
            }
            emit_text(w, a, &tl.font, ph, plen, x, tl.baseline, pc);
        }
    } else {
        if (focused && s->caret != s->anchor) {
            int lo = s->caret < s->anchor ? s->caret : s->anchor;
            int hi = s->caret < s->anchor ? s->anchor : s->caret;
            float x0 = caret_x_for(w, &tl, s, lo), x1 = caret_x_for(w, &tl, s, hi);
            emit_rect(a, x0, tl.editor_y, x1 - x0, tl.lh, w->theme.selection_bg);
        }
        emit_text(w, a, &tl.font, sb_cstr(&tl.disp), tl.disp.len, tl.text_x, tl.baseline, color);
    }
    if (focused && caret_visible(w, a->now_ms)) {
        float cx = caret_x_for(w, &tl, s, s->caret);
        float top = tl.baseline - tl.font.ascent;
        emit_rect(a, floorf(cx), top, w->theme.caret_width, tl.font.ascent + tl.font.descent, color);
    }
    emit_pop_clip(a);
    text_layout_free(&tl);
}

/* Theme lengths are CSS px: they scale with the control's effective zoom. */
static float wz(const CssComputed *st) { return st && st->zoom > 0.0f ? st->zoom : 1.0f; }

/* Chrome's themed checkbox / radio (NativeThemeBase, measured on Chrome 152):
 * the largest centred square of the box; unchecked = white with a 1px #767676
 * border (checkbox radius 2px); checked checkbox = accent fill with a white
 * tick (0.2w,0.5h) -> +(0.2w,0.2h) -> (0.8w,0.2h), stroke 0.16h; checked
 * radio = 1px accent ring, white gap, accent dot inset by 0.2 of the size.
 * Disabled: #d1d1d1 for the accent, #ededed tick. Lengths scale with zoom. */
static void paint_checkbox(Widgets *w, WidgetState *s, const CssComputed *st, const WidgetGeom *g, const WidgetPaintArgs *a)
{
    int checked = dom_get_checked(w->dom, s->elem);
    int disabled = dom_has_attr(w->dom, s->elem, "disabled");
    const float z = wz(st);
    float d = fminf(g->w, g->h);
    float x = g->x + (g->w - d) / 2, y = g->y + (g->h - d) / 2;
    const CssColor accent = disabled ? rgba(0xd1, 0xd1, 0xd1, 255) : w->theme.control_accent;
    const CssColor border = disabled ? rgba(0xc5, 0xc5, 0xc5, 255) : w->theme.control_border;
    if (st && st->appearance == CSS_APPEARANCE_NONE) return; /* fully author-styled (.switch .slider) */
    if (d <= 0) return;
    if (s->kind == WIDGET_RADIO) {
        DlRadii r = dl_radii_all(d / 2);
        emit_rrect(a, x, y, d, d, r, w->theme.control_bg);
        emit_border(a, x, y, d, d, r, z, checked ? accent : border);
        if (checked) {
            const float in = d * 0.2f;
            emit_rrect(a, x + in, y + in, d - 2 * in, d - 2 * in, dl_radii_all(d / 2 - in), accent);
        }
        return;
    }
    DlRadii r = dl_radii_all(w->theme.native_checkbox_radius * z);
    if (checked) {
        const CssColor tick = disabled ? rgba(0xed, 0xed, 0xed, 255) : w->theme.control_bg;
        const float t = d * 0.16f;
        emit_rrect(a, x, y, d, d, r, accent);
        emit_segment(a, x + d * 0.2f, y + d * 0.5f, x + d * 0.4f, y + d * 0.7f, t, tick);
        emit_segment(a, x + d * 0.4f, y + d * 0.7f, x + d * 0.8f, y + d * 0.2f, t, tick);
    } else {
        emit_rrect(a, x, y, d, d, r, w->theme.control_bg);
        emit_border(a, x, y, d, d, r, z, border);
    }
}

/* <input type=range> geometry, Chrome's shadow tree: the content box holds a
 * row flex container with ::-webkit-slider-runnable-track (flex: 1,
 * align-self: center, box-sizing: border-box; height auto = its content = the
 * thumb) and the thumb inside the track's content box, top-aligned (authors
 * centre it with margin-top).  A native thumb (appearance auto) is 16x16 *
 * zoom whatever its CSS size (LayoutThemeDefault::AdjustSliderThumbSize).
 * With `appearance: auto` on the input the whole control is themed and the
 * track pseudo-element is ignored. */
typedef struct RangeGeom {
    float track_x, track_y, track_w, track_h;   /* track border box          */
    float tc_x, tc_y, tc_w;                     /* track content box          */
    float thumb_w, thumb_h, thumb_y;            /* thumb border box (y absolute) */
    int native_input, native_thumb;
    const CssComputed *tst, *rst;   /* point into the copies below (or NULL) */
    CssComputed tst_copy, rst_copy; /* hosts may reuse one pseudo buffer      */
} RangeGeom;

static int range_thumb_native(const CssComputed *tst) { return !tst || tst->appearance != CSS_APPEARANCE_NONE; }

static void range_thumb_size(Widgets *w, const CssComputed *st, const CssComputed *tst, float *tw, float *th)
{
    if (range_thumb_native(tst)) {
        *tw = *th = w->theme.native_range_thumb_d * wz(st);
        return;
    }
    *tw = fmaxf(0, resolve_len(tst->width, 0, 0));
    *th = fmaxf(0, resolve_len(tst->height, 0, 0));
}

/* Track border-box height for an appearance:none input (auto: the thumb). */
static float range_track_height(Widgets *w, const CssComputed *st, const CssComputed *rst, const CssComputed *tst, float avail_w)
{
    float tw, th;
    range_thumb_size(w, st, tst, &tw, &th);
    if (!rst) return th;
    float bp = resolve_len(rst->padding[CSS_TOP], avail_w, 0) + resolve_len(rst->padding[CSS_BOTTOM], avail_w, 0);
    for (int i = CSS_TOP; i <= CSS_BOTTOM; i += 2)
        if (rst->border_style[i] != CSS_BORDER_STYLE_NONE && rst->border_style[i] != CSS_BORDER_STYLE_HIDDEN) bp += rst->border_width[i];
    if (rst->height.kind == CSS_LEN_VALUE && rst->height.pct == 0)
        return fmaxf(rst->height.px, bp);                     /* box-sizing: border-box */
    return th + bp;
}

static void range_geom(Widgets *w, HtmlElem e, const CssComputed *st, const WidgetGeom *g, RangeGeom *r)
{
    float cx = widget_geom_content_x(g), cy = widget_geom_content_y(g);
    float cw = widget_geom_content_w(g), ch = widget_geom_content_h(g);
    memset(r, 0, offsetof(RangeGeom, tst_copy));
    r->tst = pseudo(w, e, CSS_PE_SLIDER_THUMB);
    if (r->tst) { r->tst_copy = *r->tst; r->tst = &r->tst_copy; }
    r->native_input = !st || st->appearance != CSS_APPEARANCE_NONE;
    r->native_thumb = range_thumb_native(r->tst);
    range_thumb_size(w, st, r->tst, &r->thumb_w, &r->thumb_h);
    if (r->native_input) {
        /* themed track: 8px (zoomed), centred, inset 1px so the thumb covers both ends */
        const float z = wz(st);
        r->track_h = w->theme.native_range_track_h * z;
        r->track_x = cx + z;
        r->track_w = fmaxf(0, cw - 2 * z);
        r->track_y = cy + (ch - r->track_h) / 2;
        r->tc_x = cx; r->tc_w = cw;
        r->thumb_y = cy + (ch - r->thumb_h) / 2;
        return;
    }
    r->rst = pseudo(w, e, CSS_PE_SLIDER_TRACK);
    if (r->rst) { r->rst_copy = *r->rst; r->rst = &r->rst_copy; }
    const CssComputed *rst = r->rst;
    float ml = rst ? resolve_len(rst->margin[CSS_LEFT], cw, 0) : 0, mr = rst ? resolve_len(rst->margin[CSS_RIGHT], cw, 0) : 0;
    float bl = 0, br = 0, bt = 0, pl = 0, pr = 0, pt = 0;
    if (rst) {
        bl = rst->border_style[CSS_LEFT] > CSS_BORDER_STYLE_HIDDEN ? rst->border_width[CSS_LEFT] : 0;
        br = rst->border_style[CSS_RIGHT] > CSS_BORDER_STYLE_HIDDEN ? rst->border_width[CSS_RIGHT] : 0;
        bt = rst->border_style[CSS_TOP] > CSS_BORDER_STYLE_HIDDEN ? rst->border_width[CSS_TOP] : 0;
        pl = resolve_len(rst->padding[CSS_LEFT], cw, 0);
        pr = resolve_len(rst->padding[CSS_RIGHT], cw, 0);
        pt = resolve_len(rst->padding[CSS_TOP], cw, 0);
    }
    r->track_x = cx + ml;
    r->track_w = fmaxf(0, cw - ml - mr);
    r->track_h = range_track_height(w, st, rst, r->tst, cw);
    r->track_y = cy + (ch - r->track_h) / 2;                    /* align-self: center */
    r->tc_x = r->track_x + bl + pl;
    r->tc_w = fmaxf(0, r->track_w - bl - br - pl - pr);
    r->tc_y = r->track_y + bt + pt;
    r->thumb_y = r->tc_y + (r->tst ? resolve_len(r->tst->margin[CSS_TOP], cw, 0) : 0);
}

/* Background colour + first gradient layer + uniform border of an author-
 * styled range part (thumb / track pseudo-element). */
static void paint_part_box(const WidgetPaintArgs *a, const CssComputed *ps, const CssComputed *st, float x, float y, float bw, float bh)
{
    DlRadii r = radii_from(ps, bw, bh);
    if (ps->box_shadow_count) {
        DlItem it;
        memset(&it, 0, sizeof it);
        it.op = DL_BOX_SHADOW;
        it.u.box_shadow.rect = dl_rect(x, y, bw, bh);
        it.u.box_shadow.radii = r;
        it.u.box_shadow.dx = ps->box_shadow.x;
        it.u.box_shadow.dy = ps->box_shadow.y;
        it.u.box_shadow.blur = ps->box_shadow.blur;
        it.u.box_shadow.spread = ps->box_shadow.spread;
        it.u.box_shadow.color = resolve_color(ps->box_shadow.color, ps);
        it.u.box_shadow.inset = ps->box_shadow.inset;
        dl_push(a->out, &it);
    }
    emit_rrect(a, x, y, bw, bh, r, resolve_color(ps->background_color, st));
    for (int i = ps->bg_layer_count - 1; i >= 0; i--) {
        const CssBgLayer *L = &ps->bg_layers[i];
        if (L->image.kind != CSS_IMAGE_GRADIENT) continue;
        DlItem it;
        int clip = r.tl > 0 || r.tr > 0 || r.br > 0 || r.bl > 0;
        if (clip) {
            memset(&it, 0, sizeof it);
            it.op = DL_PUSH_CLIP_RRECT;
            it.u.clip_rrect.rect = dl_rect(x, y, bw, bh);
            it.u.clip_rrect.radii = r;
            dl_push(a->out, &it);
        }
        memset(&it, 0, sizeof it);
        it.op = DL_GRADIENT;
        it.u.gradient.rect = dl_rect(x, y, bw, bh);
        it.u.gradient.box = dl_rect(x, y, bw, bh);
        it.u.gradient.gradient = L->image.gradient;
        for (int k = 0; k < it.u.gradient.gradient.stop_count; k++)
            it.u.gradient.gradient.stops[k].color = resolve_color(it.u.gradient.gradient.stops[k].color, ps);
        dl_push(a->out, &it);
        if (clip) emit_pop_clip(a);
    }
    if (ps->border_width[CSS_TOP] > 0 && ps->border_style[CSS_TOP] > CSS_BORDER_STYLE_HIDDEN)
        emit_border(a, x, y, bw, bh, r, ps->border_width[CSS_TOP], resolve_color(ps->border_color[CSS_TOP], ps));
}

static void paint_range(Widgets *w, WidgetState *s, const CssComputed *st, const WidgetGeom *g, const WidgetPaintArgs *a)
{
    double min, max, step, value;
    int decimals;
    RangeGeom rg;
    float tx;
    range_params(w->dom, s->elem, &min, &max, &step, &decimals);
    value = strtod(sb_cstr(&s->value), NULL);
    range_geom(w, s->elem, st, g, &rg);
    tx = widgets_range_thumb_x(min, max, value, rg.tc_x, rg.tc_w, rg.thumb_w);
    const int disabled = dom_has_attr(w->dom, s->elem, "disabled");
    const CssColor accent = disabled ? rgba(0xc5, 0xc5, 0xc5, 255) : w->theme.control_accent;
    if (rg.native_input) {
        /* Chrome's themed slider (measured, Chrome 152): 8px track with a 1px
         * border, #efefef / #b2b2b2 right of the thumb centre, accent with a
         * #3b75ba border left of it; 16px accent disc. */
        const float z = wz(st), th = rg.track_h, ty = rg.track_y;
        const DlRadii r = dl_radii_all(th / 2);
        const float split = tx + rg.thumb_w / 2;
        emit_rrect(a, rg.track_x, ty, rg.track_w, th, r, rgba(0xef, 0xef, 0xef, 255));
        emit_border(a, rg.track_x, ty, rg.track_w, th, r, z, rgba(0xb2, 0xb2, 0xb2, 255));
        if (split > rg.track_x) {
            emit_clip(a, rg.track_x, ty, split - rg.track_x, th);
            emit_rrect(a, rg.track_x, ty, rg.track_w, th, r, accent);
            if (!disabled) emit_border(a, rg.track_x, ty, rg.track_w, th, r, z, rgba(0x3b, 0x75, 0xba, 255));
            emit_pop_clip(a);
        }
    } else if (rg.rst) {
        paint_part_box(a, rg.rst, st, rg.track_x, rg.track_y, rg.track_w, rg.track_h);
    }
    if (rg.native_thumb)
        emit_rrect(a, tx, rg.thumb_y, rg.thumb_w, rg.thumb_h, dl_radii_all(rg.thumb_w / 2), accent);
    else
        paint_part_box(a, rg.tst, st, tx, rg.thumb_y, rg.thumb_w, rg.thumb_h);
}

/* Intrinsic content height: 16 (themed) or the track's border-box height. */
static float range_intrinsic_h(Widgets *w, HtmlElem e, const CssComputed *st)
{
    if (!st || st->appearance != CSS_APPEARANCE_NONE) return 16 * wz(st);
    const CssComputed *t = pseudo(w, e, CSS_PE_SLIDER_TRACK);
    CssComputed track;
    if (t) track = *t;
    return range_track_height(w, st, t ? &track : NULL, pseudo(w, e, CSS_PE_SLIDER_THUMB), 0);
}

/* value from a pointer x over the range's track (mouse down / drag) */
static double range_value_at(Widgets *w, HtmlElem e, const WidgetGeom *g, float x, int *decimals)
{
    double min, max, step;
    RangeGeom rg;
    range_params(w->dom, e, &min, &max, &step, decimals);
    range_geom(w, e, computed(w, e), g, &rg);
    return widgets_range_value_from_x(min, max, step, rg.tc_x, rg.tc_w, rg.thumb_w, x);
}

static void paint_color(Widgets *w, WidgetState *s, const CssComputed *st, const WidgetGeom *g, const WidgetPaintArgs *a)
{
    const CssComputed *wst = pseudo(w, s->elem, CSS_PE_COLOR_SWATCH_WRAPPER);
    const CssComputed *sst = pseudo(w, s->elem, CSS_PE_COLOR_SWATCH);
    float px = g->x + g->border[CSS_LEFT] + g->padding[CSS_LEFT];
    float py = g->y + g->border[CSS_TOP] + g->padding[CSS_TOP];
    float pw = widget_geom_content_w(g), ph = widget_geom_content_h(g);
    float wrap_x = wst ? resolve_len(wst->padding[CSS_LEFT], pw, 0) : w->theme.swatch_wrapper_pad_x * wz(st);
    float wrap_r = wst ? resolve_len(wst->padding[CSS_RIGHT], pw, 0) : w->theme.swatch_wrapper_pad_x * wz(st);
    float wrap_y = wst ? resolve_len(wst->padding[CSS_TOP], pw, 0) : w->theme.swatch_wrapper_pad_y * wz(st);
    float wrap_b = wst ? resolve_len(wst->padding[CSS_BOTTOM], pw, 0) : w->theme.swatch_wrapper_pad_y * wz(st);
    float sx = px + wrap_x, sy = py + wrap_y, sw = pw - wrap_x - wrap_r, sh = ph - wrap_y - wrap_b;
    float bw = sst ? (sst->border_style[CSS_TOP] != CSS_BORDER_STYLE_NONE ? sst->border_width[CSS_TOP] : 0) : w->theme.swatch_border * wz(st);
    CssColor bc = sst ? resolve_color(sst->border_color[CSS_TOP], sst) : w->theme.swatch_border_color;
    DlRadii r = sst ? radii_from(sst, sw, sh) : dl_radii_all(0);
    uint8_t cr, cg, cb;
    if (!widgets_parse_hex_color(sb_cstr(&s->value), &cr, &cg, &cb)) cr = cg = cb = 0;
    emit_rrect(a, sx, sy, sw, sh, r, rgba(cr, cg, cb, 255));
    emit_border(a, sx, sy, sw, sh, r, bw, bc);
}

static void paint_select(Widgets *w, WidgetState *s, const CssComputed *st, const WidgetGeom *g, const WidgetPaintArgs *a)
{
    WFont f;
    float cx = widget_geom_content_x(g), cy = widget_geom_content_y(g);
    float cw = widget_geom_content_w(g), ch = widget_geom_content_h(g);
    float lh, inner_h, inner_y, baseline;
    int author_none = st && st->appearance == CSS_APPEARANCE_NONE;
    const float z = wz(st);
    float pad_start = author_none ? 0 : w->theme.menulist_pad_start * z;
    float pad_end = author_none ? 0 : (w->theme.menulist_pad_end + w->theme.menulist_arrow_box) * z;
    int idx = selected_index(w->dom, s->elem);
    HtmlElem opt = option_at(w->dom, s->elem, idx);
    CssColor color = st ? st->color : rgba(0, 0, 0, 255);
    font_for(w, st, &f);
    lh = resolved_line_height(st, &f);
    inner_h = lh + (author_none ? 0 : (w->theme.menulist_pad_top + w->theme.menulist_pad_bottom) * z);
    inner_y = cy + (ch - inner_h) / 2;  /* align-items: center */
    baseline = inner_y + (author_none ? 0 : w->theme.menulist_pad_top * z) + (lh - (f.ascent + f.descent)) / 2 + f.ascent;
    if (opt) {
        KnStrBuf label = {0};
        option_label(w->dom, opt, &label);
        emit_clip(a, cx, cy, cw - pad_end, ch);
        emit_text(w, a, &f, sb_cstr(&label), label.len, cx + pad_start, baseline, color);
        emit_pop_clip(a);
        kn_sb_free(&label);
    }
    if (!author_none) {
        /* chevron centred in the arrow box inside the right border */
        float right = g->x + g->w - g->border[CSS_RIGHT];
        float acx = right - w->theme.menulist_arrow_box * z / 2;
        float acy = g->y + g->h / 2;
        float hw = w->theme.menulist_arrow_w * z / 2, hh = w->theme.menulist_arrow_h * z / 2;
        /* Chrome 152 strokes the chevron in device px: ~1.6 at zoom 1, ~2.1 at zoom 4 */
        float t = w->theme.menulist_arrow_stroke + 0.15f * (z - 1.0f);
        CssColor ac = color;
        emit_segment(a, acx - hw, acy - hh, acx, acy + hh, t, ac);
        emit_segment(a, acx, acy + hh, acx + hw, acy - hh, t, ac);
    }
}

void widgets_paint(Widgets *w, HtmlElem e, const WidgetGeom *g, const WidgetPaintArgs *args)
{
    WidgetState *s;
    const CssComputed *st;
    if (!w || !g || !args || !args->out) return;
    s = state_get(w, e);
    if (!s) return;
    st = computed(w, e);
    switch ((WidgetKind)s->kind) {
    case WIDGET_TEXT:
    case WIDGET_PASSWORD:
    case WIDGET_NUMBER:
        paint_text_control(w, s, st, g, args);
        break;
    case WIDGET_CHECKBOX:
    case WIDGET_RADIO:
        paint_checkbox(w, s, st, g, args);
        break;
    case WIDGET_RANGE:
        paint_range(w, s, st, g, args);
        break;
    case WIDGET_COLOR:
        paint_color(w, s, st, g, args);
        break;
    case WIDGET_SELECT:
        paint_select(w, s, st, g, args);
        break;
    case WIDGET_TEXTAREA:
        paint_textarea(w, s, st, g, args);
        break;
    default:
        break;
    }
}

/* ---- popups ------------------------------------------------------------- */

typedef struct PopupGeom {
    float x, y, w, h;
    float row_h;           /* select rows                                     */
    int rows;              /* option count                                    */
    int vis_rows;          /* rows that fit (Chrome: <= 20, viewport bound)    */
    float list_y, list_h;  /* inner list viewport (inside the 1px border)      */
    float scroll_max;      /* (rows * row_h) - list_h, 0 without a scrollbar   */
    float bar_x, bar_w;    /* scrollbar track (x, list_y, bar_w, list_h)       */
    float thumb_y, thumb_h;
    WFont font;
    /* color picker parts (viewport) */
    float sv_x, sv_y, sv_w, sv_h;
    float hue_x, hue_y, hue_w, hue_h;
    float prev_x, prev_y, prev_w, prev_h;
} PopupGeom;

static int owner_geom(Widgets *w, HtmlElem e, WidgetGeom *g)
{
    if (!w->host.elem_geom) return 0;
    return w->host.elem_geom(w->host.user, e, g);
}

/* Geometry + pointer in the control's untransformed space (see WidgetsHost.elem_local). */
static int local_geom(Widgets *w, HtmlElem e, float vx, float vy, WidgetGeom *g, float *lx, float *ly)
{
    if (w->host.elem_local) return w->host.elem_local(w->host.user, e, vx, vy, g, lx, ly);
    *lx = vx;
    *ly = vy;
    return owner_geom(w, e, g);
}

static void place_below(Widgets *w, const WidgetGeom *g, float ph, float *y)
{
    *y = g->y + g->h;
    if (*y + ph > w->vh && g->y - ph >= 0) *y = g->y - ph;  /* flip above like Chrome */
    if (*y + ph > w->vh) *y = w->vh - ph;
    if (*y < 0) *y = 0;
}

static int popup_geom(Widgets *w, PopupGeom *pg)
{
    WidgetGeom g;
    WidgetState *s;
    const CssComputed *st;
    memset(pg, 0, sizeof *pg);
    if (!w->popup) return 0;
    s = state_find(w, w->popup);
    if (!s || !owner_geom(w, w->popup, &g)) return 0;
    st = computed(w, w->popup);
    if (s->kind == WIDGET_SELECT) {
        KnStrBuf label = {0};
        float widest = 0, lh;
        HtmlElem o;
        font_for(w, st, &pg->font);
        lh = pg->font.line_height;
        pg->row_h = (w->theme.popup_row_min_em * pg->font.size > lh ? w->theme.popup_row_min_em * pg->font.size : lh)
                  + w->theme.popup_row_pad_bottom;
        pg->row_h = ceilf(pg->row_h);
        for (o = option_first(w->dom, w->popup); o; o = option_next(w->dom, o)) {
            float tw;
            option_label(w->dom, o, &label);
            tw = measure(w, &pg->font, sb_cstr(&label), label.len);
            if (tw > widest) widest = tw;
            pg->rows++;
        }
        kn_sb_free(&label);
        {
            /* Chrome (PopupMenu / ListPicker): up to 20 rows; below the
             * control when the list fits there, otherwise on the side with
             * more room, shortened to that room; scrollable when cut. */
            float below = w->vh - (g.y + g.h), above = g.y;
            float want = (float)(pg->rows < w->theme.popup_max_rows ? pg->rows : w->theme.popup_max_rows) * pg->row_h + 2;
            float room;
            int up = 0;
            if (want > below && above > below) up = 1;
            room = up ? above : below;
            if (room < pg->row_h + 2) room = pg->row_h + 2; /* at least one row */
            pg->h = want < room ? want : room;
            pg->vis_rows = (int)floorf((pg->h - 2) / pg->row_h);
            if (pg->vis_rows < 1) pg->vis_rows = 1;
            if (pg->vis_rows > pg->rows) pg->vis_rows = pg->rows;
            pg->list_h = pg->h - 2;
            pg->scroll_max = (float)pg->rows * pg->row_h - pg->list_h;
            if (pg->scroll_max < 0.5f) pg->scroll_max = 0;
            pg->bar_w = pg->scroll_max > 0 ? w->theme.popup_scrollbar_w : 0;
            pg->w = ceilf(widest) + 2 * w->theme.popup_row_pad_x + 2 + pg->bar_w;
            if (pg->w < g.w) pg->w = g.w;
            if (pg->w > w->vw) pg->w = w->vw;
            pg->x = g.x;
            if (pg->x + pg->w > w->vw) pg->x = w->vw - pg->w;
            if (pg->x < 0) pg->x = 0;
            pg->y = up ? g.y - pg->h : g.y + g.h;
            if (pg->y + pg->h > w->vh) pg->y = w->vh - pg->h;
            if (pg->y < 0) pg->y = 0;
            pg->list_y = pg->y + 1;
            pg->bar_x = pg->x + pg->w - 1 - pg->bar_w;
            if (pg->scroll_max > 0) {
                float total = (float)pg->rows * pg->row_h;
                float scroll = clampf(s->popup_scroll, 0, pg->scroll_max);
                pg->thumb_h = pg->list_h * pg->list_h / total;
                if (pg->thumb_h < w->theme.popup_thumb_min) pg->thumb_h = w->theme.popup_thumb_min;
                if (pg->thumb_h > pg->list_h) pg->thumb_h = pg->list_h;
                pg->thumb_y = pg->list_y + (pg->list_h - pg->thumb_h) * (scroll / pg->scroll_max);
            }
        }
        return 1;
    }
    if (s->kind == WIDGET_COLOR) {
        float pad = w->theme.picker_pad;
        pg->sv_w = w->theme.picker_sv_w;
        pg->sv_h = w->theme.picker_sv_h;
        pg->hue_w = w->theme.picker_hue_w;
        pg->hue_h = pg->sv_h;
        pg->prev_h = w->theme.picker_preview_h;
        pg->w = pad + pg->sv_w + pad + pg->hue_w + pad;
        pg->h = pad + pg->sv_h + pad + pg->prev_h + pad;
        pg->x = g.x;
        if (pg->x + pg->w > w->vw) pg->x = w->vw - pg->w;
        if (pg->x < 0) pg->x = 0;
        place_below(w, &g, pg->h, &pg->y);
        pg->sv_x = pg->x + pad;
        pg->sv_y = pg->y + pad;
        pg->hue_x = pg->sv_x + pg->sv_w + pad;
        pg->hue_y = pg->sv_y;
        pg->prev_x = pg->sv_x;
        pg->prev_y = pg->sv_y + pg->sv_h + pad;
        pg->prev_w = pg->sv_w + pad + pg->hue_w;
        return 1;
    }
    return 0;
}

int widgets_overlay_rect(Widgets *w, float *x, float *y, float *out_w, float *out_h)
{
    PopupGeom pg;
    if (!w || !popup_geom(w, &pg)) return 0;
    *x = pg.x; *y = pg.y; *out_w = pg.w; *out_h = pg.h;
    return 1;
}

static int popup_row_at(const PopupGeom *pg, const WidgetState *s, float x, float y)
{
    int row;
    if (x < pg->x + 1 || x >= pg->bar_x || y < pg->list_y || y >= pg->list_y + pg->list_h) return -1;
    row = (int)((y - pg->list_y + clampf(s->popup_scroll, 0, pg->scroll_max)) / pg->row_h);
    return row >= 0 && row < pg->rows ? row : -1;
}

static int popup_on_bar(const PopupGeom *pg, float x, float y)
{
    return pg->bar_w > 0 && x >= pg->bar_x && x < pg->bar_x + pg->bar_w && y >= pg->list_y && y < pg->list_y + pg->list_h;
}

static void popup_set_scroll(Widgets *w, WidgetState *s, const PopupGeom *pg, float v)
{
    v = clampf(v, 0, pg->scroll_max);
    if (v != s->popup_scroll) {
        s->popup_scroll = v;
        invalidate(w, s->elem, 0);
    }
}

/* Scrolls the popup list so row `i` is fully visible (nearest edge). */
static void popup_scroll_into_view(Widgets *w, WidgetState *s, int i)
{
    PopupGeom pg;
    float top, bottom;
    if (i < 0 || !popup_geom(w, &pg) || pg.scroll_max <= 0) return;
    top = (float)i * pg.row_h;
    bottom = top + pg.row_h;
    if (top < s->popup_scroll) popup_set_scroll(w, s, &pg, top);
    else if (bottom > s->popup_scroll + pg.list_h) popup_set_scroll(w, s, &pg, bottom - pg.list_h);
}

static void popup_highlight(Widgets *w, WidgetState *s, int i)
{
    s->hover_index = i;
    popup_scroll_into_view(w, s, i);
    invalidate(w, s->elem, 0);
}

int widgets_paint_overlay(Widgets *w, const WidgetPaintArgs *a)
{
    PopupGeom pg;
    WidgetState *s;
    const CssComputed *st;
    if (!w || !a || !a->out || !popup_geom(w, &pg)) return 0;
    s = state_find(w, w->popup);
    st = computed(w, w->popup);
    emit_rect(a, pg.x, pg.y, pg.w, pg.h, w->theme.popup_bg);
    emit_border(a, pg.x, pg.y, pg.w, pg.h, dl_radii_all(0), 1, w->theme.popup_border);
    if (s->kind == WIDGET_SELECT) {
        KnStrBuf label = {0};
        int i = 0;
        HtmlElem o;
        CssColor fg = st ? st->color : rgba(0, 0, 0, 255);
        float scroll = clampf(s->popup_scroll, 0, pg.scroll_max);
        float list_x = pg.x + 1, list_w = pg.bar_x - list_x;
        emit_clip(a, list_x, pg.list_y, list_w, pg.list_h);
        for (o = option_first(w->dom, w->popup); o; o = option_next(w->dom, o), i++) {
            float ry = pg.list_y + (float)i * pg.row_h - scroll;
            float baseline;
            CssColor c = fg;
            if (ry + pg.row_h <= pg.list_y || ry >= pg.list_y + pg.list_h) continue;
            baseline = ry + (pg.row_h - w->theme.popup_row_pad_bottom - (pg.font.ascent + pg.font.descent)) / 2 + pg.font.ascent;
            if (i == s->hover_index) {
                emit_rect(a, list_x, ry, list_w, pg.row_h, w->theme.popup_hover_bg);
                c = w->theme.popup_hover_fg;
            }
            option_label(w->dom, o, &label);
            emit_text(w, a, &pg.font, sb_cstr(&label), label.len, list_x + w->theme.popup_row_pad_x, baseline, c);
        }
        emit_pop_clip(a);
        if (pg.bar_w > 0) {
            emit_rect(a, pg.bar_x, pg.list_y, pg.bar_w, pg.list_h, w->theme.popup_track);
            emit_rrect(a, pg.bar_x + 2, pg.thumb_y + 2, pg.bar_w - 4, pg.thumb_h - 4,
                       dl_radii_all((pg.bar_w - 4) / 2), w->theme.popup_thumb);
        }
        kn_sb_free(&label);
        return 1;
    }
    {
        uint8_t r, g, b;
        float h, sat, v;
        CssColor stops[8];
        uint8_t hr, hg, hb;
        if (!widgets_parse_hex_color(sb_cstr(&s->value), &r, &g, &b)) r = g = b = 0;
        widgets_rgb_to_hsv(r, g, b, &h, &sat, &v);
        widgets_hsv_to_rgb(h, 1, 1, &hr, &hg, &hb);
        /* SV square: white -> hue horizontally, then transparent -> black vertically */
        stops[0] = rgba(255, 255, 255, 255);
        stops[1] = rgba(hr, hg, hb, 255);
        emit_gradient_h(a, pg.sv_x, pg.sv_y, pg.sv_w, pg.sv_h, stops, 2, 0);
        stops[0] = rgba(0, 0, 0, 0);
        stops[1] = rgba(0, 0, 0, 255);
        emit_gradient_h(a, pg.sv_x, pg.sv_y, pg.sv_w, pg.sv_h, stops, 2, 1);
        /* hue bar */
        {
            int i;
            for (i = 0; i < 7; i++) {
                uint8_t cr, cg, cb;
                widgets_hsv_to_rgb((float)i * 60.0f, 1, 1, &cr, &cg, &cb);
                stops[i] = rgba(cr, cg, cb, 255);
            }
            emit_gradient_h(a, pg.hue_x, pg.hue_y, pg.hue_w, pg.hue_h, stops, 7, 1);
        }
        /* markers */
        {
            float mx = pg.sv_x + sat * pg.sv_w, my = pg.sv_y + (1 - v) * pg.sv_h;
            float hy = pg.hue_y + h / 360.0f * pg.hue_h;
            emit_border(a, mx - 5, my - 5, 10, 10, dl_radii_all(5), 2, rgba(255, 255, 255, 255));
            emit_border(a, mx - 6, my - 6, 12, 12, dl_radii_all(6), 1, rgba(0, 0, 0, 160));
            emit_rect(a, pg.hue_x - 2, hy - 1.5f, pg.hue_w + 4, 3, rgba(255, 255, 255, 255));
            emit_border(a, pg.hue_x - 2, hy - 2.5f, pg.hue_w + 4, 5, dl_radii_all(1), 1, rgba(0, 0, 0, 160));
        }
        emit_rect(a, pg.prev_x, pg.prev_y, pg.prev_w, pg.prev_h, rgba(r, g, b, 255));
        emit_border(a, pg.prev_x, pg.prev_y, pg.prev_w, pg.prev_h, dl_radii_all(0), 1, w->theme.popup_border);
        return 1;
    }
}

double widgets_next_repaint_ms(Widgets *w, double now_ms)
{
    WidgetState *s;
    double t, period;
    if (!w || !w->focused) return -1;
    s = state_find(w, w->focused);
    if (!s || !kind_is_text((WidgetKind)s->kind)) return -1;
    if (w->blink_start_ms < 0) return now_ms;
    period = w->theme.caret_blink_ms;
    t = now_ms - w->blink_start_ms;
    return w->blink_start_ms + (floor(t / period) + 1) * period;
}

/* ------------------------------------------------------------------------- */
/* Editing primitives                                                         */
/* ------------------------------------------------------------------------- */

static int prev_cp(const char *s, int off)
{
    if (off <= 0) return 0;
    off--;
    while (off > 0 && ((unsigned char)s[off] & 0xC0) == 0x80) off--;
    return off;
}

static int next_cp(const char *s, int len, int off)
{
    if (off >= len) return len;
    off++;
    while (off < len && ((unsigned char)s[off] & 0xC0) == 0x80) off++;
    return off;
}

static int count_cps(const char *s, size_t len)
{
    size_t i = 0;
    int n = 0;
    while (i < len) {
        uint32_t cp;
        i += (size_t)kn_utf8_decode(s + i, len - i, &cp);
        n++;
    }
    return n;
}

static int has_selection(const WidgetState *s) { return s->caret != s->anchor; }

static void delete_selection(WidgetState *s)
{
    int lo = s->caret < s->anchor ? s->caret : s->anchor;
    int hi = s->caret < s->anchor ? s->anchor : s->caret;
    if (lo == hi) return;
    memmove(s->value.data + lo, s->value.data + hi, s->value.len - (size_t)hi + 1);
    s->value.len -= (size_t)(hi - lo);
    s->caret = s->anchor = lo;
}

static int maxlength_of(Dom *dom, HtmlElem e)
{
    double v;
    if (attr_number(dom, e, "maxlength", &v) && v >= 0) return (int)v;
    return -1;
}

static int number_char_ok(uint32_t cp)
{
    return (cp >= '0' && cp <= '9') || cp == '.' || cp == '-' || cp == '+' || cp == 'e' || cp == 'E';
}

static int insert_text(Widgets *w, WidgetState *s, const char *ins, size_t ins_len);

static void number_step(Widgets *w, WidgetState *s, int dir)
{
    double min, max, step, v;
    int decimals;
    char buf[64];
    char *end = NULL;
    const char *cur = sb_cstr(&s->value);
    number_params(w->dom, s->elem, &min, &max, &step, &decimals);
    if (step <= 0) step = 1;
    v = strtod(cur, &end);
    if (end == cur || *end != '\0') v = 0;
    v += dir * step;
    if (decimals >= 0) {
        double base = min != -HUGE_VAL ? min : 0;
        v = base + floor((v - base) / step + 0.5) * step;
    }
    if (decimals < str_decimals(cur)) decimals = str_decimals(cur);
    v = clampd(v, min, max);
    widgets_format_number(v, decimals, buf, sizeof buf);
    if (strcmp(buf, cur) == 0) return;
    sb_set(&s->value, buf, strlen(buf));
    s->caret = s->anchor = (int)s->value.len;
    value_changed(w, s);
    fire_input(w, s->elem);
    commit_change(w, s);
}

static void range_set_value(Widgets *w, WidgetState *s, double v, int decimals, int fire_events)
{
    char buf[64];
    widgets_format_number(v, decimals, buf, sizeof buf);
    if (strcmp(buf, sb_cstr(&s->value)) == 0) return;
    sb_set(&s->value, buf, strlen(buf));
    value_changed(w, s);
    if (fire_events) fire_input(w, s->elem);
}

static void range_key_step(Widgets *w, WidgetState *s, int dir, int big)
{
    double min, max, step, v;
    int decimals;
    range_params(w->dom, s->elem, &min, &max, &step, &decimals);
    v = strtod(sb_cstr(&s->value), NULL);
    if (step <= 0) step = (max - min) / 100.0;
    if (big) step *= 10;
    v = snap_step(min, max, step > 0 ? step : 1, v + dir * step);
    range_set_value(w, s, v, decimals, 1);
    commit_change(w, s);
}

/* ------------------------------------------------------------------------- */
/* Focus                                                                      */
/* ------------------------------------------------------------------------- */

static void blur_commit(Widgets *w, WidgetState *s)
{
    if (kind_is_text((WidgetKind)s->kind) || s->kind == WIDGET_COLOR) commit_change(w, s);
    s->anchor = s->caret;
}

void widgets_on_focus(Widgets *w, HtmlElem e)
{
    WidgetState *s;
    if (!w) return;
    if (w->focused && w->focused != e) {
        WidgetState *old = state_find(w, w->focused);
        if (old) blur_commit(w, old);
    }
    w->focused = e;
    s = state_get(w, e);
    if (!s) return;
    set_cstr(&s->commit_value, s->kind == WIDGET_SELECT ? "" : sb_cstr(&s->value));
    if (kind_is_text((WidgetKind)s->kind) && !w->in_press) {
        /* programmatic focus(): caret at the end, nothing selected */
        s->caret = s->anchor = (int)s->value.len;
    }
    reset_blink(w);
    invalidate(w, e, 0);
}

void widgets_on_blur(Widgets *w, HtmlElem e)
{
    WidgetState *s;
    if (!w) return;
    if (e == 0) e = w->focused;
    if (!e) return;
    s = state_find(w, e);
    if (w->popup == e) widgets_close_popup(w);
    if (s) {
        blur_commit(w, s);
        s->dragging = 0;
    }
    if (w->focused == e) w->focused = 0;
    invalidate(w, e, 0);
}

static void request_focus(Widgets *w, HtmlElem e)
{
    if (w->host.focus) w->host.focus(w->host.user, e);
    else widgets_on_focus(w, e);
}

static void request_blur(Widgets *w, HtmlElem e)
{
    if (w->host.blur) w->host.blur(w->host.user, e);
    else widgets_on_blur(w, e);
}

/* ------------------------------------------------------------------------- */
/* Popup control                                                              */
/* ------------------------------------------------------------------------- */

void widgets_close_popup(Widgets *w)
{
    WidgetState *s;
    HtmlElem owner;
    if (!w || !w->popup) return;
    owner = w->popup;
    w->popup = 0;
    s = state_find(w, owner);
    if (s) {
        s->hover_index = -1;
        s->dragging = 0;
        if (s->kind == WIDGET_COLOR) commit_change(w, s);
    }
    invalidate(w, owner, 0);
}

static void open_popup(Widgets *w, HtmlElem e)
{
    WidgetState *s = state_get(w, e);
    if (!s) return;
    if (w->popup && w->popup != e) widgets_close_popup(w);
    w->popup = e;
    s->hover_index = s->kind == WIDGET_SELECT ? selected_index(w->dom, e) : -1;
    s->popup_scroll = 0;
    s->dragging = 0;
    s->drag_part = 0;
    if (s->kind == WIDGET_SELECT) popup_scroll_into_view(w, s, s->hover_index);
    invalidate(w, e, 0);
}

int widgets_open_select(Widgets *w, HtmlElem e, float x, float y)
{
    (void)x; (void)y;
    if (!w || widgets_kind(w, e) != WIDGET_SELECT) return 0;
    if (dom_has_attr(w->dom, e, "disabled")) return 0;
    if (w->popup == e) {
        widgets_close_popup(w);
        return EV_WIDGET_CONSUMED;
    }
    request_focus(w, e);
    open_popup(w, e);
    {
        WidgetState *s = state_find(w, e);
        if (s) s->dragging = 1;   /* press-drag-release onto a row chooses it */
    }
    return EV_WIDGET_CAPTURE | EV_WIDGET_CONSUMED;
}

static void popup_choose(Widgets *w, int index)
{
    HtmlElem owner = w->popup;
    widgets_close_popup(w);
    if (index >= 0) widgets_select_set_index(w, owner, index, 1);
}

static void picker_apply(Widgets *w, WidgetState *s, const PopupGeom *pg, float x, float y)
{
    uint8_t r, g, b;
    float h, sat, v;
    char buf[8];
    if (!widgets_parse_hex_color(sb_cstr(&s->value), &r, &g, &b)) r = g = b = 0;
    widgets_rgb_to_hsv(r, g, b, &h, &sat, &v);
    if (s->drag_part == 1) {
        sat = clampf((x - pg->sv_x) / pg->sv_w, 0, 1);
        v = 1 - clampf((y - pg->sv_y) / pg->sv_h, 0, 1);
    } else if (s->drag_part == 2) {
        h = clampf((y - pg->hue_y) / pg->hue_h, 0, 0.9999f) * 360.0f;
    } else {
        return;
    }
    widgets_hsv_to_rgb(h, sat, v, &r, &g, &b);
    snprintf(buf, sizeof buf, "#%02x%02x%02x", r, g, b);
    if (strcmp(buf, sb_cstr(&s->value)) == 0) return;
    sb_set(&s->value, buf, 7);
    value_changed(w, s);
    fire_input(w, s->elem);
}

/* ------------------------------------------------------------------------- */
/* Mouse                                                                      */
/* ------------------------------------------------------------------------- */

static int point_in(float x, float y, float rx, float ry, float rw, float rh)
{
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

int widgets_mouse_down(Widgets *w, HtmlElem e, float x, float y, int button, int mods)
{
    WidgetState *s;
    WidgetGeom g;
    int ret = 0;
    if (!w) return 0;
    w->mouse_x = x;
    w->mouse_y = y;
    /* an open popup eats presses */
    if (w->popup) {
        PopupGeom pg;
        WidgetState *ps = state_find(w, w->popup);
        if (popup_geom(w, &pg) && ps && point_in(x, y, pg.x, pg.y, pg.w, pg.h)) {
            if (ps->kind == WIDGET_SELECT) {
                if (button != EV_BUTTON_LEFT) return EV_WIDGET_CONSUMED;
                if (popup_on_bar(&pg, x, y)) {
                    if (y >= pg.thumb_y && y < pg.thumb_y + pg.thumb_h) {
                        ps->dragging = 2;            /* thumb drag */
                        ps->bar_grab = y - pg.thumb_y;
                    } else {
                        /* track click: page towards the pointer */
                        float page = (float)(pg.vis_rows > 1 ? pg.vis_rows - 1 : 1) * pg.row_h;
                        popup_set_scroll(w, ps, &pg, ps->popup_scroll + (y < pg.thumb_y ? -page : page));
                    }
                    return EV_WIDGET_CAPTURE | EV_WIDGET_CONSUMED;
                }
                {
                    int row = popup_row_at(&pg, ps, x, y);
                    if (row >= 0) ps->hover_index = row;
                }
                ps->dragging = 1;                    /* row press: choose on release */
                invalidate(w, ps->elem, 0);
                return EV_WIDGET_CAPTURE | EV_WIDGET_CONSUMED;
            }
            if (button == EV_BUTTON_LEFT) {
                ps->drag_part = point_in(x, y, pg.sv_x, pg.sv_y, pg.sv_w, pg.sv_h) ? 1
                              : point_in(x, y, pg.hue_x, pg.hue_y, pg.hue_w, pg.hue_h) ? 2 : 0;
                if (ps->drag_part) {
                    ps->dragging = 1;
                    picker_apply(w, ps, &pg, x, y);
                    return EV_WIDGET_CAPTURE | EV_WIDGET_CONSUMED;
                }
            }
            return EV_WIDGET_CONSUMED;
        }
        /* outside: close; the press is swallowed (Chrome's popup behaviour) */
        widgets_close_popup(w);
        return EV_WIDGET_CONSUMED;
    }
    if (!e || button != EV_BUTTON_LEFT) return 0;
    s = state_get(w, e);
    if (!s || dom_has_attr(w->dom, e, "disabled")) return 0;
    if (!local_geom(w, e, x, y, &g, &x, &y)) return 0;
    w->in_press = 1;
    switch ((WidgetKind)s->kind) {
    case WIDGET_RANGE: {
        double v;
        int decimals;
        set_cstr(&s->drag_start, sb_cstr(&s->value));
        v = range_value_at(w, e, &g, x, &decimals);
        request_focus(w, e);
        range_set_value(w, s, v, decimals, 1);
        s->dragging = 1;
        ret = EV_WIDGET_CAPTURE;
        break;
    }
    case WIDGET_TEXT:
    case WIDGET_PASSWORD:
    case WIDGET_NUMBER:
    case WIDGET_TEXTAREA: {
        const CssComputed *st = computed(w, e);
        int off;
        request_focus(w, e);
        if (s->kind == WIDGET_TEXTAREA) {
            off = textarea_offset_at(w, s, &g, x, y);
            s->goal_x = -1;
        } else {
            TextLayout tl;
            text_layout(w, s, st, &g, &tl);
            off = offset_from_x(w, &tl, s, x);
            text_layout_free(&tl);
        }
        {
            /* Chrome: double-click selects a word, triple-click everything
             * (system double-click time ~500 ms, a few px of slop) */
            double now = clock_ms(w);
            if (now - s->last_click_ms < 500.0 && fabsf(x - s->last_click_x) < 4 && fabsf(y - s->last_click_y) < 4)
                s->click_count++;
            else
                s->click_count = 1;
            s->last_click_ms = now;
            s->last_click_x = x;
            s->last_click_y = y;
        }
        s->last_edit = EDIT_NONE;
        if (s->click_count == 2) {
            select_word_at(s, off);
            s->dragging = 0;
        } else if (s->click_count >= 3) {
            s->anchor = 0;
            s->caret = (int)s->value.len;
            s->dragging = 0;
        } else {
            s->caret = off;
            if (!(mods & EV_MOD_SHIFT)) s->anchor = off;
            s->dragging = 1;
        }
        reset_blink(w);
        invalidate(w, e, 0);
        ret = EV_WIDGET_CAPTURE;
        break;
    }
    case WIDGET_COLOR:
        request_focus(w, e);
        open_popup(w, e);
        ret = EV_WIDGET_CONSUMED;
        break;
    case WIDGET_SELECT:
        ret = widgets_open_select(w, e, x, y);
        break;
    default:
        /* checkbox/radio: events.c's click default action calls widgets_activate */
        break;
    }
    w->in_press = 0;
    return ret;
}

void widgets_mouse_move(Widgets *w, HtmlElem e, float x, float y, int buttons)
{
    WidgetState *s;
    WidgetGeom g;
    if (!w) return;
    w->mouse_x = x;
    w->mouse_y = y;
    if (w->popup) {
        PopupGeom pg;
        WidgetState *ps = state_find(w, w->popup);
        if (ps && popup_geom(w, &pg)) {
            if (ps->kind == WIDGET_SELECT) {
                if (ps->dragging == 2 && (buttons & 1)) {
                    float track = pg.list_h - pg.thumb_h;
                    if (track > 0) popup_set_scroll(w, ps, &pg, (y - ps->bar_grab - pg.list_y) / track * pg.scroll_max);
                } else {
                    int row = popup_row_at(&pg, ps, x, y);
                    if (row >= 0 && row != ps->hover_index) {
                        ps->hover_index = row;
                        invalidate(w, ps->elem, 0);
                    }
                }
            } else if (ps->dragging && (buttons & 1)) {
                picker_apply(w, ps, &pg, x, y);
            }
        }
        return;
    }
    if (!e) return;
    s = state_find(w, e);
    if (!s || !s->dragging || !(buttons & 1)) return;
    if (!local_geom(w, e, x, y, &g, &x, &y)) return;
    if (s->kind == WIDGET_RANGE) {
        int decimals;
        double v = range_value_at(w, e, &g, x, &decimals);
        range_set_value(w, s, v, decimals, 1);
    } else if (kind_is_text((WidgetKind)s->kind)) {
        int off;
        if (s->kind == WIDGET_TEXTAREA) {
            off = textarea_offset_at(w, s, &g, x, y);
        } else {
            TextLayout tl;
            text_layout(w, s, computed(w, e), &g, &tl);
            off = offset_from_x(w, &tl, s, x);
            text_layout_free(&tl);
        }
        if (off != s->caret) {
            s->caret = off;
            reset_blink(w);
            invalidate(w, e, 0);
        }
    }
}

void widgets_mouse_up(Widgets *w, HtmlElem e, float x, float y, int button)
{
    WidgetState *s;
    if (!w) return;
    w->mouse_x = x;
    w->mouse_y = y;
    if (w->popup) {
        PopupGeom pg;
        WidgetState *ps = state_find(w, w->popup);
        if (ps && popup_geom(w, &pg)) {
            if (ps->kind == WIDGET_SELECT) {
                int row = popup_row_at(&pg, ps, x, y);
                if (ps->dragging == 1 && button == EV_BUTTON_LEFT && row >= 0) {
                    popup_choose(w, row);
                    return;
                }
                ps->dragging = 0;
            } else if (ps->dragging) {
                ps->dragging = 0;
                ps->drag_part = 0;
            }
        }
        return;
    }
    if (!e || button != EV_BUTTON_LEFT) return;
    s = state_find(w, e);
    if (!s || !s->dragging) return;
    s->dragging = 0;
    if (s->kind == WIDGET_RANGE) {
        if (!s->drag_start || strcmp(s->drag_start, sb_cstr(&s->value)) != 0) {
            set_cstr(&s->commit_value, sb_cstr(&s->value));
            fire(w, e, "change", 0);
        }
    }
}

void widgets_pointer_moved(Widgets *w, float x, float y)
{
    if (!w) return;
    w->mouse_x = x;
    w->mouse_y = y;
    if (w->popup) widgets_mouse_move(w, w->popup, x, y, 0);
}

/* ------------------------------------------------------------------------- */
/* Keyboard                                                                   */
/* ------------------------------------------------------------------------- */

/* ---- editing helpers: word boundaries, undo, clipboard ------------------ */

/* Chrome's word granularity, simplified: runs of word characters (ASCII
 * alnum, '_', any non-ASCII), runs of other punctuation, and spaces. */
static int char_class(unsigned char c)
{
    if (c >= 0x80 || kn_is_digit(c) || kn_is_alpha(c) || c == '_') return 1;
    if (kn_is_space(c)) return 0;
    return 2;
}

/* Ctrl+Left: start of the previous word (skips spaces first). */
static int word_left(const WidgetState *s, int off)
{
    const char *v = sb_cstr(&s->value);
    int cls;
    if (s->kind == WIDGET_PASSWORD) return 0;   /* Chrome: no word stops in password fields */
    while (off > 0 && char_class((unsigned char)v[prev_cp(v, off)]) == 0) off = prev_cp(v, off);
    if (off == 0) return 0;
    cls = char_class((unsigned char)v[prev_cp(v, off)]);
    while (off > 0 && char_class((unsigned char)v[prev_cp(v, off)]) == cls) off = prev_cp(v, off);
    return off;
}

/* Ctrl+Right: Windows stops at the start of the next word, Linux/macOS at the
 * end of the current/next word (Chrome follows the platform). */
static int word_right(const WidgetState *s, int off)
{
    const char *v = sb_cstr(&s->value);
    int len = (int)s->value.len, cls;
    if (s->kind == WIDGET_PASSWORD) return len;
#ifdef _WIN32
    if (off < len) {
        cls = char_class((unsigned char)v[off]);
        if (cls != 0)
            while (off < len && char_class((unsigned char)v[off]) == cls) off = next_cp(v, len, off);
    }
    while (off < len && char_class((unsigned char)v[off]) == 0) off = next_cp(v, len, off);
#else
    while (off < len && char_class((unsigned char)v[off]) == 0) off = next_cp(v, len, off);
    if (off < len) {
        cls = char_class((unsigned char)v[off]);
        while (off < len && char_class((unsigned char)v[off]) == cls) off = next_cp(v, len, off);
    }
#endif
    return off;
}

/* Double-click: the run (word / punctuation / spaces) around `off`. */
static void select_word_at(WidgetState *s, int off)
{
    const char *v = sb_cstr(&s->value);
    int len = (int)s->value.len, lo = off, hi = off, cls;
    if (s->kind == WIDGET_PASSWORD || len == 0) { s->anchor = 0; s->caret = len; return; }
    if (off >= len) off = prev_cp(v, len);
    cls = char_class((unsigned char)v[off]);
    lo = off;
    while (lo > 0 && char_class((unsigned char)v[prev_cp(v, lo)]) == cls) lo = prev_cp(v, lo);
    hi = next_cp(v, len, off);
    while (hi < len && char_class((unsigned char)v[hi]) == cls) hi = next_cp(v, len, hi);
    s->anchor = lo;
    s->caret = hi;
}

static void undo_entry_free(UndoEntry *u) { free(u->value); u->value = NULL; }

static void undo_clear(WidgetState *s)
{
    int i;
    for (i = 0; i < s->undo_n; i++) undo_entry_free(&s->undo[i]);
    for (i = 0; i < s->redo_n; i++) undo_entry_free(&s->redo[i]);
    s->undo_n = s->redo_n = 0;
    s->last_edit = EDIT_NONE;
}

static void undo_push(UndoEntry *stack, int *n, const WidgetState *s)
{
    if (*n == WIDGETS_UNDO_MAX) {
        undo_entry_free(&stack[0]);
        memmove(stack, stack + 1, (WIDGETS_UNDO_MAX - 1) * sizeof *stack);
        (*n)--;
    }
    stack[*n].value = dup_cstr(sb_cstr(&s->value));
    stack[*n].caret = s->caret;
    stack[*n].anchor = s->anchor;
    (*n)++;
}

/* Records the state before an edit.  Consecutive typing (or consecutive
 * deleting) at the caret where the previous one left it is one undo step,
 * like Chrome's typing coalescing. */
static void undo_checkpoint(WidgetState *s, int kind)
{
    int i;
    if (kind != EDIT_OTHER && s->last_edit == kind && s->caret == s->last_caret && !has_selection(s)) return;
    undo_push(s->undo, &s->undo_n, s);
    for (i = 0; i < s->redo_n; i++) undo_entry_free(&s->redo[i]);
    s->redo_n = 0;
}

static void edit_done(WidgetState *s, int kind)
{
    s->last_edit = (uint8_t)kind;
    s->last_caret = s->caret;
}

/* Ctrl+Z (redo = 0) / Ctrl+Y, Ctrl+Shift+Z (redo = 1). */
static void undo_apply(Widgets *w, WidgetState *s, int redo)
{
    UndoEntry *from = redo ? s->redo : s->undo, *to = redo ? s->undo : s->redo;
    int *from_n = redo ? &s->redo_n : &s->undo_n, *to_n = redo ? &s->undo_n : &s->redo_n;
    UndoEntry e;
    if (*from_n == 0) return;
    undo_push(to, to_n, s);
    e = from[--(*from_n)];
    sb_set(&s->value, e.value, strlen(e.value));
    s->caret = e.caret;
    s->anchor = e.anchor;
    undo_entry_free(&e);
    s->last_edit = EDIT_NONE;
    value_changed(w, s);
    fire_input(w, s->elem);
}

static void clipboard_set(Widgets *w, const char *text, size_t len)
{
    char *z = (char *)malloc(len + 1);
    if (!z) return;
    memcpy(z, text, len);
    z[len] = '\0';
    if (w->host.clipboard_set) w->host.clipboard_set(w->host.user, z);
    else sb_set(&w->clipboard, z, len);
    free(z);
}

static const char *clipboard_get(Widgets *w)
{
    const char *t = w->host.clipboard_get ? w->host.clipboard_get(w->host.user) : sb_cstr(&w->clipboard);
    return t ? t : "";
}

static int copy_selection(Widgets *w, WidgetState *s)
{
    int lo = s->caret < s->anchor ? s->caret : s->anchor;
    int hi = s->caret < s->anchor ? s->anchor : s->caret;
    if (lo == hi || s->kind == WIDGET_PASSWORD) return 0;   /* Chrome never copies a password */
    clipboard_set(w, sb_cstr(&s->value) + lo, (size_t)(hi - lo));
    return 1;
}

/* Paste: newlines stripped (value sanitisation of single-line inputs; a
 * textarea keeps them, CRLF / CR normalised to LF), number inputs keep only
 * characters a number may contain, maxlength honoured. */
static int paste_text(Widgets *w, WidgetState *s)
{
    const char *clip = clipboard_get(w);
    KnStrBuf t = {0};
    size_t i = 0, n = strlen(clip);
    int changed;
    while (i < n) {
        uint32_t cp;
        int k = kn_utf8_decode(clip + i, n - i, &cp);
        if (s->kind == WIDGET_TEXTAREA && (cp == '\n' || cp == '\r')) {
            if (cp == '\n' || !(i + 1 < n && clip[i + 1] == '\n')) kn_sb_append(&t, "\n", 1);
        } else if (cp != '\n' && cp != '\r' && (s->kind != WIDGET_NUMBER || number_char_ok(cp)) && (cp >= 0x20 || cp == '\t'))
            kn_sb_append(&t, clip + i, (size_t)k);
        i += (size_t)k;
    }
    if (t.len == 0 && !has_selection(s)) { kn_sb_free(&t); return 0; }
    undo_checkpoint(s, EDIT_OTHER);
    changed = insert_text(w, s, sb_cstr(&t), t.len);
    kn_sb_free(&t);
    edit_done(s, EDIT_OTHER);
    return changed;
}

/* Inserts text at the caret (replacing the selection).  Returns 1 when the
 * value changed (also when only the selection was removed). */
static int insert_text(Widgets *w, WidgetState *s, const char *ins, size_t ins_len)
{
    int maxlen = maxlength_of(w->dom, s->elem);
    int had_sel = has_selection(s);
    KnStrBuf tmp = {0};
    delete_selection(s);
    if (maxlen >= 0) {
        int have = count_cps(sb_cstr(&s->value), s->value.len);
        int room = maxlen - have;
        size_t i = 0;
        if (room <= 0) return had_sel;
        while (i < ins_len && room > 0) {
            uint32_t cp;
            i += (size_t)kn_utf8_decode(ins + i, ins_len - i, &cp);
            room--;
        }
        ins_len = i;
    }
    if (ins_len == 0) return had_sel;
    kn_sb_append(&tmp, sb_cstr(&s->value), (size_t)s->caret);
    kn_sb_append(&tmp, ins, ins_len);
    kn_sb_append(&tmp, sb_cstr(&s->value) + s->caret, s->value.len - (size_t)s->caret);
    sb_set(&s->value, sb_cstr(&tmp), tmp.len);
    kn_sb_free(&tmp);
    s->caret += (int)ins_len;
    s->anchor = s->caret;
    return 1;
}

/* Deletes [lo, hi) as one edit of `kind`; returns 1 when something went. */
static int delete_range(WidgetState *s, int lo, int hi, int kind)
{
    if (lo == hi) return 0;
    undo_checkpoint(s, kind);
    s->anchor = lo;
    s->caret = hi;
    delete_selection(s);
    edit_done(s, kind);
    return 1;
}

static int key_text(Widgets *w, WidgetState *s, int key, int mods)
{
    const char *v = sb_cstr(&s->value);
    int len = (int)s->value.len;
    int shift = (mods & EV_MOD_SHIFT) != 0;
    int prim = (mods & WIDGETS_MOD_PRIMARY) != 0;   /* Ctrl (Cmd on macOS)        */
    int word = (mods & WIDGETS_MOD_WORD) != 0;      /* Ctrl (Option on macOS)     */
    int changed = 0, moved = 1;
    switch (key) {
    case WK_LEFT:
#ifdef __APPLE__
        if (prim) { s->caret = 0; if (!shift) s->anchor = 0; break; }
#endif
        if (has_selection(s) && !shift && !word) {
            s->caret = s->anchor = s->caret < s->anchor ? s->caret : s->anchor;
        } else {
            s->caret = word ? word_left(s, s->caret) : prev_cp(v, s->caret);
            if (!shift) s->anchor = s->caret;
        }
        break;
    case WK_RIGHT:
#ifdef __APPLE__
        if (prim) { s->caret = len; if (!shift) s->anchor = len; break; }
#endif
        if (has_selection(s) && !shift && !word) {
            s->caret = s->anchor = s->caret > s->anchor ? s->caret : s->anchor;
        } else {
            s->caret = word ? word_right(s, s->caret) : next_cp(v, len, s->caret);
            if (!shift) s->anchor = s->caret;
        }
        break;
    case WK_HOME:
        s->caret = 0;
        if (!shift) s->anchor = 0;
        break;
    case WK_END:
        s->caret = len;
        if (!shift) s->anchor = len;
        break;
    case WK_BACKSPACE:
        moved = 0;
        if (has_selection(s)) {
            int lo = s->caret < s->anchor ? s->caret : s->anchor, hi = s->caret ^ s->anchor ^ lo;
            changed = delete_range(s, lo, hi, EDIT_OTHER);
        } else if (s->caret > 0) {
            changed = delete_range(s, word ? word_left(s, s->caret) : prev_cp(v, s->caret), s->caret, EDIT_DELETE);
        }
        break;
    case WK_DELETE:
        moved = 0;
        if (shift && !word) {                       /* Shift+Delete = cut (Windows/Linux) */
            if (copy_selection(w, s)) changed = delete_range(s, s->caret < s->anchor ? s->caret : s->anchor,
                                                             s->caret < s->anchor ? s->anchor : s->caret, EDIT_OTHER);
        } else if (has_selection(s)) {
            int lo = s->caret < s->anchor ? s->caret : s->anchor, hi = s->caret ^ s->anchor ^ lo;
            changed = delete_range(s, lo, hi, EDIT_OTHER);
        } else if (s->caret < len) {
            changed = delete_range(s, s->caret, word ? word_right(s, s->caret) : next_cp(v, len, s->caret), EDIT_DELETE);
        }
        break;
    case WK_INSERT:
        moved = 0;
        if (prim && !shift) copy_selection(w, s);    /* Ctrl+Insert = copy  */
        else if (shift && !prim) changed = paste_text(w, s); /* Shift+Insert = paste */
        else return 0;
        break;
    case WK_A:
        if (!prim) return 0;
        s->anchor = 0;
        s->caret = len;
        break;
    case WK_C:
        if (!prim) return 0;
        copy_selection(w, s);
        return 1;
    case WK_X:
        if (!prim) return 0;
        moved = 0;
        if (copy_selection(w, s))
            changed = delete_range(s, s->caret < s->anchor ? s->caret : s->anchor,
                                   s->caret < s->anchor ? s->anchor : s->caret, EDIT_OTHER);
        break;
    case WK_V:
        if (!prim) return 0;
        moved = 0;
        changed = paste_text(w, s);
        break;
    case WK_Z:
        if (!prim) return 0;
        undo_apply(w, s, shift);
        return 1;
    case WK_Y:
        if (!prim) return 0;
        undo_apply(w, s, 1);
        return 1;
    case WK_UP:
        if (s->kind == WIDGET_NUMBER) {
            undo_checkpoint(s, EDIT_OTHER);
            number_step(w, s, +1);
            edit_done(s, EDIT_OTHER);
            return 1;
        }
        s->caret = 0;
        if (!shift) s->anchor = 0;
        break;
    case WK_DOWN:
        if (s->kind == WIDGET_NUMBER) {
            undo_checkpoint(s, EDIT_OTHER);
            number_step(w, s, -1);
            edit_done(s, EDIT_OTHER);
            return 1;
        }
        s->caret = len;
        if (!shift) s->anchor = len;
        break;
    case EV_KEY_ENTER:
    case EV_KEY_KP_ENTER:
        /* events.c fires `keypress` for Enter after this hook; the control
         * only commits (Chrome: `change` on Enter when the value moved). */
        commit_change(w, s);
        reset_blink(w);
        invalidate(w, s->elem, 0);
        return 1;
    default:
        return 0;
    }
    if (moved) s->last_edit = EDIT_NONE;   /* caret moves end typing coalescing */
    if (changed) {
        value_changed(w, s);
        fire_input(w, s->elem);
    } else {
        reset_blink(w);
        invalidate(w, s->elem, 0);
    }
    return 1;
}

static double clock_ms(Widgets *w)
{
    struct timespec ts;
    if (w->time_set) return w->now_ms;
    clock_gettime(CLOCK_REALTIME, &ts); // not C11 timespec_get: MinGW's msvcrt lacks it
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

/* Type-ahead is "in progress" for 1 s after the last letter (Chrome's
 * TypeAhead): Space then extends the prefix instead of opening/choosing. */
static int typeahead_active(Widgets *w, const WidgetState *s)
{
    return s->typeahead_len > 0 && clock_ms(w) - s->typeahead_ms < 1000.0;
}

/* Moves the selection (closed: fires input + change) or the highlight (open). */
static void select_move_to(Widgets *w, WidgetState *s, int i)
{
    int n = option_count(w->dom, s->elem);
    if (n == 0) return;
    if (i < 0) i = 0;
    if (i >= n) i = n - 1;
    if (w->popup == s->elem) popup_highlight(w, s, i);
    else widgets_select_set_index(w, s->elem, i, 1);
}

static int select_current(Widgets *w, const WidgetState *s)
{
    if (w->popup == s->elem && s->hover_index >= 0) return s->hover_index;
    return selected_index(w->dom, s->elem);
}

static int key_select(Widgets *w, WidgetState *s, int key, int mods)
{
    int n = option_count(w->dom, s->elem);
    int open = w->popup == s->elem;
    int cur;
    if (n == 0) return 0;
    cur = select_current(w, s);
    switch (key) {
    case WK_UP:
    case WK_DOWN:
        if (!open && (mods & EV_MOD_ALT)) { open_popup(w, s->elem); return 1; }
        if (open && (mods & EV_MOD_ALT)) { popup_choose(w, s->hover_index); return 1; }
        select_move_to(w, s, cur < 0 ? 0 : cur + (key == WK_UP ? -1 : 1));
        return 1;
    case WK_HOME:
        select_move_to(w, s, 0);
        return 1;
    case WK_END:
        select_move_to(w, s, n - 1);
        return 1;
    case WK_PAGE_UP:
    case WK_PAGE_DOWN: {
        PopupGeom pg;
        int page = 1;
        if (open && popup_geom(w, &pg) && pg.vis_rows > 1) page = pg.vis_rows - 1;
        select_move_to(w, s, cur + (key == WK_PAGE_UP ? -page : page));
        return 1;
    }
    case WK_F4:
        if (open) widgets_close_popup(w);
        else open_popup(w, s->elem);
        return 1;
    case WK_SPACE:
        if (typeahead_active(w, s)) return 1;   /* the ' ' char extends the type-ahead */
        if (!open) open_popup(w, s->elem);
        else popup_choose(w, s->hover_index);
        return 1;
    case EV_KEY_ENTER:
    case EV_KEY_KP_ENTER:
        if (open) popup_choose(w, s->hover_index);
        else open_popup(w, s->elem);
        return 1;
    default:
        return 0;
    }
}

/* Chrome TypeAhead: letters within 1 s build a prefix matched against the
 * option labels (case-insensitive) from the current option on; repeating one
 * letter cycles through the options that start with it. */
static void select_typeahead(Widgets *w, WidgetState *s, uint32_t cp)
{
    KnStrBuf label = {0};
    double now = clock_ms(w);
    int n = option_count(w->dom, s->elem), cur = select_current(w, s);
    int k, cycle = 1, start, plen;
    char buf[4];
    int bl;
    if (n == 0) return;
    if (now - s->typeahead_ms >= 1000.0) s->typeahead_len = 0;
    s->typeahead_ms = now;
    if (cp < 0x80) cp = (uint32_t)kn_to_lower((int)cp);
    bl = kn_utf8_encode(cp, buf);
    if (s->typeahead_len + bl < (int)sizeof s->typeahead) {
        memcpy(s->typeahead + s->typeahead_len, buf, (size_t)bl);
        s->typeahead_len += bl;
    }
    for (k = bl; k < s->typeahead_len; k += bl)
        if (memcmp(s->typeahead + k, buf, (size_t)bl) != 0) { cycle = 0; break; }
    /* a first or repeated letter searches after the current option; a longer
     * prefix re-checks the current one first */
    start = cycle ? cur + 1 : (cur < 0 ? 0 : cur);
    plen = cycle ? bl : s->typeahead_len;
    for (k = 0; k < n; k++) {
        int i = ((start + k) % n + n) % n;
        size_t j;
        int match = 1;
        option_label(w->dom, option_at(w->dom, s->elem, i), &label);
        if (label.len < (size_t)plen) continue;
        for (j = 0; j < (size_t)plen; j++)
            if (kn_to_lower((unsigned char)label.data[j]) != (unsigned char)s->typeahead[j]) { match = 0; break; }
        if (match) {
            select_move_to(w, s, i);
            break;
        }
    }
    kn_sb_free(&label);
}

/* Mouse wheel.  While a select popup is open the wheel scrolls its list when
 * the pointer is over it; elsewhere it closes the popup (Chrome hides the
 * popup when the page scrolls).  Returns 1 when consumed. */
/* Wheel over an overflowing <textarea>: scrolls its lines (consumed unless
 * already at the limit, then the page scrolls, like Chrome). */
static int textarea_wheel(Widgets *w, float dx, float dy, float x, float y)
{
    for (uint32_t i = 0; i < w->slot_cap; i++) {
        WidgetState *s = w->slots[i];
        WidgetGeom vg, g;
        float lx, ly;
        if (!s || s->kind != WIDGET_TEXTAREA || !owner_geom(w, s->elem, &vg)) continue;
        if (!point_in(x, y, vg.x, vg.y, vg.w, vg.h) || !local_geom(w, s->elem, x, y, &g, &lx, &ly)) continue;
        MlLayout ml;
        const char *v = sb_cstr(&s->value);
        ml_layout(w, s, computed(w, s->elem), &g, v, (int)s->value.len, &ml);
        const float max_scroll = fmaxf(0, (float)ml.n * ml.lh - ml.ch);
        const float old_x = s->scroll_x;
        ml_free(&ml);
        if (s->code && dx != 0) {
            /* code view: horizontal wheel; ml_layout clamps on the next paint */
            s->scroll_x = fmaxf(0, s->scroll_x - dx * EV_SCROLL_PIXELS_PER_CLICK);
            if (s->scroll_x != old_x) {
                invalidate(w, s->elem, 0);
                return 1;
            }
        }
        const float next = clampf(s->scroll_y - dy * EV_SCROLL_PIXELS_PER_CLICK, 0, max_scroll);
        if (next == s->scroll_y) return 0;
        s->scroll_y = next;
        invalidate(w, s->elem, 0);
        return 1;
    }
    return 0;
}

int widgets_scroll(Widgets *w, float dx, float dy, float x, float y)
{
    PopupGeom pg;
    WidgetState *s;
    if (w && !w->popup) return textarea_wheel(w, dx, dy, x, y);
    if (!w || !w->popup) return 0;
    s = state_find(w, w->popup);
    if (!s || !popup_geom(w, &pg)) return 0;
    if (point_in(x, y, pg.x, pg.y, pg.w, pg.h)) {
        if (s->kind == WIDGET_SELECT) {
            int row;
            popup_set_scroll(w, s, &pg, s->popup_scroll - dy * EV_SCROLL_PIXELS_PER_CLICK);
            if (popup_geom(w, &pg) && (row = popup_row_at(&pg, s, x, y)) >= 0) s->hover_index = row;
        }
        return 1;
    }
    widgets_close_popup(w);
    return 0;
}

/* <textarea> keys: Enter inserts a line feed, Up/Down/Home/End/Page keys move
 * by visual lines (Ctrl+Home/End: whole value); the rest is key_text(). */
static int key_textarea(Widgets *w, WidgetState *s, int key, int mods)
{
    const int shift = (mods & EV_MOD_SHIFT) != 0, prim = (mods & WIDGETS_MOD_PRIMARY) != 0;
    if (key == EV_KEY_ENTER || key == EV_KEY_KP_ENTER) {
        if (dom_has_attr(w->dom, s->elem, "readonly")) return 1;
        undo_checkpoint(s, EDIT_OTHER);
        if (insert_text(w, s, "\n", 1)) {
            edit_done(s, EDIT_OTHER);
            value_changed(w, s);
            fire_input(w, s->elem);
        }
        s->goal_x = -1;
        reset_blink(w);
        invalidate(w, s->elem, 0);
        return 1;
    }
    if (!prim && (key == WK_UP || key == WK_DOWN || key == WK_HOME || key == WK_END || key == WK_PAGE_UP || key == WK_PAGE_DOWN)) {
        if (textarea_nav(w, s, key, shift)) {
            reset_blink(w);
            invalidate(w, s->elem, 0);
        }
        return 1;
    }
    s->goal_x = -1;
    return key_text(w, s, key, mods);
}

int widgets_key(Widgets *w, HtmlElem e, int key, int scancode, int action, int mods)
{
    WidgetState *s;
    (void)scancode;
    if (!w || !e) return 0;
    if (action == EV_RELEASE) return w->focused == e; /* releases are consumed while focused */
    s = state_get(w, e);
    if (!s) return 0;
    if (key == EV_KEY_ESCAPE) {
        if (w->popup == e && s->kind == WIDGET_SELECT) {
            widgets_close_popup(w);
            return 1;
        }
        widgets_close_popup(w);
        request_blur(w, e);
        return 1;
    }
    if (dom_has_attr(w->dom, e, "disabled")) return 1;
    switch ((WidgetKind)s->kind) {
    case WIDGET_TEXT:
    case WIDGET_PASSWORD:
    case WIDGET_NUMBER:
        return key_text(w, s, key, mods) ? 1 : (key == EV_KEY_TAB ? 0 : 1);
    case WIDGET_TEXTAREA:
        return key_textarea(w, s, key, mods) ? 1 : (key == EV_KEY_TAB ? 0 : 1);
    case WIDGET_RANGE:
        switch (key) {
        case WK_LEFT: case WK_DOWN: range_key_step(w, s, -1, 0); return 1;
        case WK_RIGHT: case WK_UP: range_key_step(w, s, +1, 0); return 1;
        case WK_HOME: case WK_END: {
            double min, max, step;
            int decimals;
            range_params(w->dom, e, &min, &max, &step, &decimals);
            range_set_value(w, s, key == WK_HOME ? min : max, decimals, 1);
            commit_change(w, s);
            return 1;
        }
        default: return 1;
        }
    case WIDGET_CHECKBOX:
    case WIDGET_RADIO:
        if (key == WK_SPACE) {
            widgets_activate(w, e);
            return 1;
        }
        return 1;
    case WIDGET_COLOR:
        if (key == WK_SPACE || key == EV_KEY_ENTER) {
            if (w->popup == e) widgets_close_popup(w);
            else open_popup(w, e);
            return 1;
        }
        return 1;
    case WIDGET_SELECT:
        return key_select(w, s, key, mods) ? 1 : 1;
    default:
        return 0;
    }
}

int widgets_char(Widgets *w, HtmlElem e, uint32_t codepoint)
{
    WidgetState *s;
    char buf[4];
    int n;
    if (!w || !e) return 0;
    s = state_get(w, e);
    if (!s) return 0;
    if (s->kind == WIDGET_SELECT) {
        if (codepoint < 0x20 || codepoint == 0x7f) return 1;
        if (codepoint == ' ' && !typeahead_active(w, s)) return 1; /* handled as a key */
        if (!dom_has_attr(w->dom, e, "disabled")) select_typeahead(w, s, codepoint);
        return 1;
    }
    if (!kind_is_text((WidgetKind)s->kind)) return 1; /* consumed, nothing to insert */
    if (dom_has_attr(w->dom, e, "disabled") || dom_has_attr(w->dom, e, "readonly")) return 1;
    if (codepoint < 0x20 || codepoint == 0x7f) return 1;
    if (s->kind == WIDGET_NUMBER && !number_char_ok(codepoint)) return 1;
    n = kn_utf8_encode(codepoint, buf);
    undo_checkpoint(s, EDIT_TYPING);
    if (insert_text(w, s, buf, (size_t)n)) {
        edit_done(s, EDIT_TYPING);
        value_changed(w, s);
        fire_input(w, e);
    }
    return 1;
}

/* ------------------------------------------------------------------------- */
/* Text editing from C and the code view (widgets.h)                          */
/* ------------------------------------------------------------------------- */

/* A text control's state, or NULL. */
static WidgetState *text_state(Widgets *w, HtmlElem e)
{
    WidgetState *s = w && e ? state_get(w, e) : NULL;
    return s && kind_is_text((WidgetKind)s->kind) ? s : NULL;
}

/* off clamped to the value and moved back to a code point start. */
static int clamp_offset(const WidgetState *s, int off)
{
    const char *v = sb_cstr(&s->value);
    if (off < 0) off = 0;
    if (off > (int)s->value.len) off = (int)s->value.len;
    while (off > 0 && ((unsigned char)v[off] & 0xC0) == 0x80) off--;
    return off;
}

int widgets_selection(Widgets *w, HtmlElem e, int *start, int *end)
{
    WidgetState *s = text_state(w, e);
    if (!s) return 0;
    if (start) *start = s->caret < s->anchor ? s->caret : s->anchor;
    if (end) *end = s->caret < s->anchor ? s->anchor : s->caret;
    return 1;
}

void widgets_set_selection(Widgets *w, HtmlElem e, int start, int end)
{
    WidgetState *s = text_state(w, e);
    if (!s) return;
    start = clamp_offset(s, start);
    end = clamp_offset(s, end);
    if (end < start) end = start;
    s->anchor = start;
    s->caret = end;
    s->goal_x = -1;
    s->last_edit = EDIT_NONE;
    s->seen_caret = 0;   /* scroll it into view on the next layout */
    reset_blink(w);
    invalidate(w, e, 0);
}

int widgets_replace_range(Widgets *w, HtmlElem e, int start, int end, const char *text)
{
    WidgetState *s = text_state(w, e);
    if (!s) return 0;
    start = clamp_offset(s, start);
    end = clamp_offset(s, end);
    if (end < start) { int t = start; start = end; end = t; }
    undo_checkpoint(s, EDIT_OTHER);
    s->anchor = start;
    s->caret = end;
    delete_selection(s);
    if (text && *text) {
        /* bypasses maxlength / sanitisation like setRangeText */
        KnStrBuf tmp = {0};
        const size_t n = strlen(text);
        kn_sb_append(&tmp, sb_cstr(&s->value), (size_t)s->caret);
        kn_sb_append(&tmp, text, n);
        kn_sb_append(&tmp, sb_cstr(&s->value) + s->caret, s->value.len - (size_t)s->caret);
        sb_set(&s->value, sb_cstr(&tmp), tmp.len);
        kn_sb_free(&tmp);
        s->caret += (int)n;
        s->anchor = s->caret;
    }
    edit_done(s, EDIT_OTHER);
    s->goal_x = -1;
    s->seen_caret = 0;
    value_changed(w, s);
    return 1;
}

int widgets_caret_rect(Widgets *w, HtmlElem e, float *x, float *y, float *cw, float *ch)
{
    WidgetState *s = text_state(w, e);
    WidgetGeom vg, g;
    float lx, ly;
    if (!s || !owner_geom(w, e, &vg) || !local_geom(w, e, vg.x, vg.y, &g, &lx, &ly)) return 0;
    /* local -> viewport: the untransformed box scaled onto its viewport box */
    const float sx = g.w > 0 ? vg.w / g.w : 1, sy = g.h > 0 ? vg.h / g.h : 1;
    float px, py, ph;
    if (s->kind == WIDGET_TEXTAREA) {
        MlLayout ml;
        const char *v = sb_cstr(&s->value);
        ml_layout(w, s, computed(w, e), &g, v, (int)s->value.len, &ml);
        px = ml_x(w, &ml, v, s->caret);
        py = ml_line_y(&ml, ml_line_of(&ml, s->caret));
        ph = ml.lh;
        ml_free(&ml);
    } else {
        TextLayout tl;
        text_layout(w, s, computed(w, e), &g, &tl);
        px = caret_x_for(w, &tl, s, s->caret);
        py = tl.editor_y;
        ph = tl.lh;
        text_layout_free(&tl);
    }
    if (x) *x = vg.x + (px - g.x) * sx;
    if (y) *y = vg.y + (py - g.y) * sy;
    if (cw) *cw = w->theme.caret_width * sx;
    if (ch) *ch = ph * sy;
    return 1;
}

void widgets_set_code_view(Widgets *w, HtmlElem e, const WidgetsCodeView *cfg)
{
    WidgetState *s = w && e ? state_get(w, e) : NULL;
    if (!s || s->kind != WIDGET_TEXTAREA) return;
    if (!cfg) {
        code_free(s->code);
        s->code = NULL;
        s->scroll_x = 0;
    } else {
        if (!s->code) s->code = (WidgetCode *)calloc(1, sizeof *s->code);
        if (!s->code) return;
        s->code->cfg = *cfg;
    }
    invalidate(w, e, 0);
}

void widgets_set_code_spans(Widgets *w, HtmlElem e, const WidgetsCodeSpan *spans, int n)
{
    WidgetState *s = w && e ? state_find(w, e) : NULL;
    if (!s || !s->code) return;
    WidgetCode *c = s->code;
    free(c->spans);
    c->spans = NULL;
    c->nspans = 0;
    if (n > 0 && spans) {
        c->spans = (WidgetsCodeSpan *)malloc((size_t)n * sizeof *spans);
        if (c->spans) {
            memcpy(c->spans, spans, (size_t)n * sizeof *spans);
            c->nspans = n;
        }
    }
    invalidate(w, e, 0);
}

void widgets_set_code_lines(Widgets *w, HtmlElem e, const WidgetsCodeLine *lines, int n)
{
    WidgetState *s = w && e ? state_find(w, e) : NULL;
    if (!s || !s->code) return;
    WidgetCode *c = s->code;
    free(c->lines);
    c->lines = NULL;
    c->nlines = 0;
    if (n > 0 && lines) {
        c->lines = (WidgetsCodeLine *)malloc((size_t)n * sizeof *lines);
        if (c->lines) {
            memcpy(c->lines, lines, (size_t)n * sizeof *lines);
            c->nlines = n;
        }
    }
    invalidate(w, e, 0);
}

/* ------------------------------------------------------------------------- */
/* Hook table                                                                 */
/* ------------------------------------------------------------------------- */

static int hook_mouse_down(void *u, HtmlElem e, float x, float y, int b, int m) { return widgets_mouse_down((Widgets *)u, e, x, y, b, m); }
static void hook_mouse_move(void *u, HtmlElem e, float x, float y, int b) { widgets_mouse_move((Widgets *)u, e, x, y, b); }
static void hook_mouse_up(void *u, HtmlElem e, float x, float y, int b) { widgets_mouse_up((Widgets *)u, e, x, y, b); }
static int hook_key(void *u, HtmlElem e, int k, int sc, int a, int m) { return widgets_key((Widgets *)u, e, k, sc, a, m); }
static int hook_chr(void *u, HtmlElem e, uint32_t cp) { return widgets_char((Widgets *)u, e, cp); }
static void hook_focus(void *u, HtmlElem e) { widgets_on_focus((Widgets *)u, e); }
static void hook_blur(void *u, HtmlElem e) { widgets_on_blur((Widgets *)u, e); }
static int hook_open_select(void *u, HtmlElem e, float x, float y) { return widgets_open_select((Widgets *)u, e, x, y); }

void widgets_fill_event_hooks(Widgets *w, EventsWidgetHooks *out)
{
    memset(out, 0, sizeof *out);
    out->user = w;
    out->mouse_down = hook_mouse_down;
    out->mouse_move = hook_mouse_move;
    out->mouse_up = hook_mouse_up;
    out->key = hook_key;
    out->chr = hook_chr;
    out->focus = hook_focus;
    out->blur = hook_blur;
    out->open_select = hook_open_select;
}

/* ------------------------------------------------------------------------- */
/* paint.h adapter                                                            */
/* ------------------------------------------------------------------------- */

#include "layout.h"
#include "paint.h"

void widgets_set_time(Widgets *w, double now_ms)
{
    if (!w) return;
    w->now_ms = now_ms;
    w->time_set = 1;
}

static void *paint_alloc_cb(void *user, size_t size)
{
    return paint_alloc((PaintCtx *)user, size);
}

void widgets_paint_host(void *user, const struct PaintWidgetArgs *args)
{
    Widgets *w = (Widgets *)user;
    const Box *box;
    WidgetGeom g;
    WidgetPaintArgs pa;
    HtmlElem e;
    int i;
    if (!w || !args || !args->box || !args->out) return;
    box = args->box;
    e = dom_node_elem(w->dom, box->node);
    if (!e) return;
    g.x = args->x;
    g.y = args->y;
    g.w = box->w;
    g.h = box->h;
    for (i = 0; i < 4; i++) {
        g.border[i] = box->border[i];
        g.padding[i] = box->padding[i];
    }
    memset(&pa, 0, sizeof pa);
    pa.out = args->out;
    pa.alloc = paint_alloc_cb;
    pa.alloc_user = args->ctx;
    pa.device_scale = args->device_scale > 0 ? args->device_scale : 1.0f;
    pa.now_ms = w->now_ms;
    widgets_paint(w, e, &g, &pa);
}
