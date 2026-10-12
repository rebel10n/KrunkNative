/*
 * css_types.h - value types and the computed-style record for the KrunkNative
 * HTML/CSS engine (doc/html-engine-plan.md, "Data structures").
 *
 * Everything here is plain data: no pointers except interned atom strings
 * (see util.h, css_atom()).  A CssComputed can be copied with memcpy and
 * hashed byte-wise; create it only through css_computed_init_defaults() or by
 * copying another record so padding bytes stay zero.
 *
 * Lengths
 * -------
 * CssLength keeps three linear components: px, pct (percent, 0..100 scale)
 * and em.  Parsers never resolve em (they do not know the font size); the
 * cascade folds it with css_length_resolve_em() / css_computed_apply(), so a
 * computed CssLength always has em == 0.  Percentages are kept separately
 * because the reference box is only known at layout time:
 *     calc(100% - 35px)  ->  { px = -35, pct = 100, em = 0, kind = VALUE }
 *     1.5em              ->  { px = 0,   pct = 0,   em = 1.5, kind = VALUE }
 * Keyword lengths (auto, none, normal, min-content, ...) use `kind` and leave
 * the numeric fields at 0.  line-height's unitless number is CSS_LEN_NUMBER
 * with the multiplier in `px`.
 */
#ifndef KN_CSS_TYPES_H
#define KN_CSS_TYPES_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/* Limits (centralised so the cascade/anim/paint agree)                       */
/* ------------------------------------------------------------------------- */

#define CSS_MAX_TRANSFORM_OPS   4
#define CSS_MAX_TEXT_SHADOWS    16
#define CSS_MAX_GRADIENT_STOPS  16  /* devconsole.css grip: 7 double-position stops = 13 */
#define CSS_MAX_BG_LAYERS       2
#define CSS_MAX_FILTER_OPS      6
#define CSS_MAX_ANIMATIONS      2
#define CSS_MAX_TRANSITIONS     2
#define CSS_MAX_BOX_SHADOWS     4
#define CSS_MAX_GRID_TRACKS     12  /* v7.1.4 editor.css #_toolbar: 10 columns */
#define CSS_MAX_CUSTOM_PROPS    96  /* v7.1.4: main.css 21 + bundledStyles.min.css 34 :root properties, + hud.css / social.css */
#define CSS_MAX_CLIP_POINTS     8
#define CSS_MAX_FONT_FAMILIES   4

/* ------------------------------------------------------------------------- */
/* Length                                                                     */
/* ------------------------------------------------------------------------- */

typedef enum CssLengthKind {
    CSS_LEN_VALUE = 0,     /* px + pct% (+ em before resolution)              */
    CSS_LEN_AUTO,          /* auto                                            */
    CSS_LEN_NONE,          /* none (max-width/max-height)                     */
    CSS_LEN_NORMAL,        /* normal (line-height, letter-spacing)            */
    CSS_LEN_NUMBER,        /* unitless multiplier in px (line-height: 1)      */
    CSS_LEN_MIN_CONTENT,   /* min-content                                     */
    CSS_LEN_MAX_CONTENT,   /* max-content                                     */
    CSS_LEN_FIT_CONTENT    /* fit-content                                     */
} CssLengthKind;

typedef struct CssLength {
    float px;
    float pct;             /* 50% -> 50.0f                                   */
    float em;              /* unresolved em component (0 once computed)      */
    uint8_t kind;          /* CssLengthKind                                   */
} CssLength;

/* ------------------------------------------------------------------------- */
/* Color                                                                      */
/* ------------------------------------------------------------------------- */

typedef enum CssColorKind {
    CSS_COLOR_RGBA = 0,    /* r,g,b,a valid                                   */
    CSS_COLOR_CURRENT      /* currentColor: resolve against computed `color`  */
} CssColorKind;

typedef struct CssColor {
    uint8_t r, g, b, a;    /* straight (non-premultiplied) alpha              */
    uint8_t kind;          /* CssColorKind                                    */
} CssColor;

/* ------------------------------------------------------------------------- */
/* Keyword enums (one per property family; values are stable small ints)     */
/* ------------------------------------------------------------------------- */

typedef enum CssDisplay {
    CSS_DISPLAY_INLINE = 0, CSS_DISPLAY_BLOCK, CSS_DISPLAY_NONE,
    CSS_DISPLAY_INLINE_BLOCK, CSS_DISPLAY_FLEX, CSS_DISPLAY_INLINE_FLEX,
    CSS_DISPLAY_GRID, CSS_DISPLAY_INLINE_GRID, CSS_DISPLAY_TABLE,
    CSS_DISPLAY_INLINE_TABLE, CSS_DISPLAY_TABLE_ROW, CSS_DISPLAY_TABLE_CELL,
    CSS_DISPLAY_TABLE_ROW_GROUP, CSS_DISPLAY_LIST_ITEM, CSS_DISPLAY_CONTENTS
} CssDisplay;

typedef enum CssPosition {
    CSS_POSITION_STATIC = 0, CSS_POSITION_RELATIVE, CSS_POSITION_ABSOLUTE,
    CSS_POSITION_FIXED, CSS_POSITION_STICKY
} CssPosition;

typedef enum CssFloat { CSS_FLOAT_NONE = 0, CSS_FLOAT_LEFT, CSS_FLOAT_RIGHT } CssFloat;
typedef enum CssClear { CSS_CLEAR_NONE = 0, CSS_CLEAR_LEFT, CSS_CLEAR_RIGHT, CSS_CLEAR_BOTH } CssClear;

typedef enum CssOverflow {
    CSS_OVERFLOW_VISIBLE = 0, CSS_OVERFLOW_HIDDEN, CSS_OVERFLOW_SCROLL,
    CSS_OVERFLOW_AUTO, CSS_OVERFLOW_CLIP
} CssOverflow;

typedef enum CssVisibility { CSS_VISIBILITY_VISIBLE = 0, CSS_VISIBILITY_HIDDEN, CSS_VISIBILITY_COLLAPSE } CssVisibility;
typedef enum CssPointerEvents { CSS_POINTER_EVENTS_AUTO = 0, CSS_POINTER_EVENTS_NONE, CSS_POINTER_EVENTS_ALL } CssPointerEvents;

typedef enum CssCursor {
    CSS_CURSOR_AUTO = 0, CSS_CURSOR_DEFAULT, CSS_CURSOR_POINTER, CSS_CURSOR_NONE,
    CSS_CURSOR_CROSSHAIR, CSS_CURSOR_TEXT, CSS_CURSOR_MOVE, CSS_CURSOR_NOT_ALLOWED,
    CSS_CURSOR_GRAB, CSS_CURSOR_GRABBING, CSS_CURSOR_WAIT, CSS_CURSOR_HELP,
    CSS_CURSOR_EW_RESIZE, CSS_CURSOR_NS_RESIZE, CSS_CURSOR_NWSE_RESIZE, CSS_CURSOR_NESW_RESIZE
} CssCursor;

typedef enum CssUserSelect { CSS_USER_SELECT_AUTO = 0, CSS_USER_SELECT_NONE, CSS_USER_SELECT_TEXT, CSS_USER_SELECT_ALL } CssUserSelect;
typedef enum CssDirection { CSS_DIRECTION_LTR = 0, CSS_DIRECTION_RTL } CssDirection;
typedef enum CssAppearance { CSS_APPEARANCE_AUTO = 0, CSS_APPEARANCE_NONE, CSS_APPEARANCE_TEXTFIELD, CSS_APPEARANCE_MENULIST, CSS_APPEARANCE_BUTTON } CssAppearance;
typedef enum CssBoxSizing { CSS_BOX_SIZING_CONTENT_BOX = 0, CSS_BOX_SIZING_BORDER_BOX } CssBoxSizing;
typedef enum CssTableLayout { CSS_TABLE_LAYOUT_AUTO = 0, CSS_TABLE_LAYOUT_FIXED } CssTableLayout;

typedef enum CssTextAlign {
    CSS_TEXT_ALIGN_START = 0, CSS_TEXT_ALIGN_LEFT, CSS_TEXT_ALIGN_RIGHT,
    CSS_TEXT_ALIGN_CENTER, CSS_TEXT_ALIGN_JUSTIFY, CSS_TEXT_ALIGN_END
} CssTextAlign;

typedef enum CssTextTransform { CSS_TEXT_TRANSFORM_NONE = 0, CSS_TEXT_TRANSFORM_UPPERCASE, CSS_TEXT_TRANSFORM_LOWERCASE, CSS_TEXT_TRANSFORM_CAPITALIZE } CssTextTransform;
typedef enum CssWhiteSpace { CSS_WHITE_SPACE_NORMAL = 0, CSS_WHITE_SPACE_NOWRAP, CSS_WHITE_SPACE_PRE, CSS_WHITE_SPACE_PRE_WRAP, CSS_WHITE_SPACE_PRE_LINE } CssWhiteSpace;

/* text-decoration is a bit set */
enum { CSS_TEXT_DECORATION_NONE = 0, CSS_TEXT_DECORATION_UNDERLINE = 1, CSS_TEXT_DECORATION_OVERLINE = 2, CSS_TEXT_DECORATION_LINE_THROUGH = 4 };

typedef enum CssTextOverflow { CSS_TEXT_OVERFLOW_CLIP = 0, CSS_TEXT_OVERFLOW_ELLIPSIS } CssTextOverflow;
typedef enum CssOverflowWrap { CSS_OVERFLOW_WRAP_NORMAL = 0, CSS_OVERFLOW_WRAP_BREAK_WORD, CSS_OVERFLOW_WRAP_ANYWHERE } CssOverflowWrap;
typedef enum CssWordBreak { CSS_WORD_BREAK_NORMAL = 0, CSS_WORD_BREAK_BREAK_ALL, CSS_WORD_BREAK_KEEP_ALL, CSS_WORD_BREAK_BREAK_WORD } CssWordBreak;

typedef enum CssVerticalAlign {
    CSS_VERTICAL_ALIGN_BASELINE = 0, CSS_VERTICAL_ALIGN_TOP, CSS_VERTICAL_ALIGN_MIDDLE,
    CSS_VERTICAL_ALIGN_BOTTOM, CSS_VERTICAL_ALIGN_SUB, CSS_VERTICAL_ALIGN_SUPER,
    CSS_VERTICAL_ALIGN_TEXT_TOP, CSS_VERTICAL_ALIGN_TEXT_BOTTOM,
    CSS_VERTICAL_ALIGN_LENGTH   /* use CssComputed.vertical_align_length       */
} CssVerticalAlign;

typedef enum CssImageRendering { CSS_IMAGE_RENDERING_AUTO = 0, CSS_IMAGE_RENDERING_PIXELATED, CSS_IMAGE_RENDERING_CRISP_EDGES, CSS_IMAGE_RENDERING_SMOOTH } CssImageRendering;
typedef enum CssObjectFit { CSS_OBJECT_FIT_FILL = 0, CSS_OBJECT_FIT_CONTAIN, CSS_OBJECT_FIT_COVER, CSS_OBJECT_FIT_NONE, CSS_OBJECT_FIT_SCALE_DOWN } CssObjectFit;

typedef enum CssFlexDirection { CSS_FLEX_DIRECTION_ROW = 0, CSS_FLEX_DIRECTION_ROW_REVERSE, CSS_FLEX_DIRECTION_COLUMN, CSS_FLEX_DIRECTION_COLUMN_REVERSE } CssFlexDirection;
typedef enum CssFlexWrap { CSS_FLEX_WRAP_NOWRAP = 0, CSS_FLEX_WRAP_WRAP, CSS_FLEX_WRAP_WRAP_REVERSE } CssFlexWrap;

typedef enum CssJustifyContent {
    CSS_JUSTIFY_CONTENT_FLEX_START = 0, CSS_JUSTIFY_CONTENT_FLEX_END, CSS_JUSTIFY_CONTENT_CENTER,
    CSS_JUSTIFY_CONTENT_SPACE_BETWEEN, CSS_JUSTIFY_CONTENT_SPACE_AROUND, CSS_JUSTIFY_CONTENT_SPACE_EVENLY
} CssJustifyContent;

typedef enum CssAlignItems {
    CSS_ALIGN_ITEMS_STRETCH = 0, CSS_ALIGN_ITEMS_FLEX_START, CSS_ALIGN_ITEMS_FLEX_END,
    CSS_ALIGN_ITEMS_CENTER, CSS_ALIGN_ITEMS_BASELINE,
    CSS_ALIGN_ITEMS_AUTO   /* align-self / justify-self: auto (use the container's *-items) */
} CssAlignItems;          /* also align-self, justify-self, justify-items (normal == STRETCH) */

typedef enum CssBorderStyle {
    CSS_BORDER_STYLE_NONE = 0, CSS_BORDER_STYLE_HIDDEN, CSS_BORDER_STYLE_SOLID,
    CSS_BORDER_STYLE_DASHED, CSS_BORDER_STYLE_DOTTED, CSS_BORDER_STYLE_DOUBLE,
    CSS_BORDER_STYLE_GROOVE, CSS_BORDER_STYLE_RIDGE, CSS_BORDER_STYLE_INSET, CSS_BORDER_STYLE_OUTSET
} CssBorderStyle;

typedef enum CssBorderCollapse { CSS_BORDER_COLLAPSE_SEPARATE = 0, CSS_BORDER_COLLAPSE_COLLAPSE } CssBorderCollapse;
typedef enum CssFontStyle { CSS_FONT_STYLE_NORMAL = 0, CSS_FONT_STYLE_ITALIC, CSS_FONT_STYLE_OBLIQUE } CssFontStyle;
typedef enum CssListStyleType { CSS_LIST_STYLE_DISC = 0, CSS_LIST_STYLE_NONE, CSS_LIST_STYLE_CIRCLE, CSS_LIST_STYLE_SQUARE, CSS_LIST_STYLE_DECIMAL } CssListStyleType;

typedef enum CssBgRepeat { CSS_BG_REPEAT_REPEAT = 0, CSS_BG_REPEAT_NO_REPEAT, CSS_BG_REPEAT_ROUND, CSS_BG_REPEAT_SPACE } CssBgRepeat;
typedef enum CssBgSizeKind { CSS_BG_SIZE_LENGTHS = 0 /* w,h (auto allowed) */, CSS_BG_SIZE_COVER, CSS_BG_SIZE_CONTAIN } CssBgSizeKind;

typedef enum CssTimingKind {
    CSS_TIMING_EASE = 0, CSS_TIMING_LINEAR, CSS_TIMING_EASE_IN, CSS_TIMING_EASE_OUT,
    CSS_TIMING_EASE_IN_OUT, CSS_TIMING_CUBIC_BEZIER, CSS_TIMING_STEPS
} CssTimingKind;
typedef enum CssStepPosition { CSS_STEP_END = 0, CSS_STEP_START, CSS_STEP_JUMP_NONE, CSS_STEP_JUMP_BOTH } CssStepPosition;

typedef enum CssAnimDirection { CSS_ANIM_DIRECTION_NORMAL = 0, CSS_ANIM_DIRECTION_REVERSE, CSS_ANIM_DIRECTION_ALTERNATE, CSS_ANIM_DIRECTION_ALTERNATE_REVERSE } CssAnimDirection;
typedef enum CssAnimFill { CSS_ANIM_FILL_NONE = 0, CSS_ANIM_FILL_FORWARDS, CSS_ANIM_FILL_BACKWARDS, CSS_ANIM_FILL_BOTH } CssAnimFill;
typedef enum CssAnimPlayState { CSS_ANIM_PLAY_RUNNING = 0, CSS_ANIM_PLAY_PAUSED } CssAnimPlayState;

typedef enum CssFilterKind {
    CSS_FILTER_GRAYSCALE = 0, CSS_FILTER_BRIGHTNESS, CSS_FILTER_CONTRAST, CSS_FILTER_SATURATE,
    CSS_FILTER_HUE_ROTATE, CSS_FILTER_OPACITY, CSS_FILTER_DROP_SHADOW, CSS_FILTER_BLUR,
    CSS_FILTER_INVERT, CSS_FILTER_SEPIA
} CssFilterKind;

typedef enum CssTransformKind {
    CSS_TRANSFORM_TRANSLATE = 0,  /* tx, ty (lengths, % of own border box)    */
    CSS_TRANSFORM_SCALE,          /* sx, sy                                   */
    CSS_TRANSFORM_ROTATE,         /* angle_deg                                */
    CSS_TRANSFORM_SKEW,           /* angle_deg (x), angle2_deg (y)            */
    CSS_TRANSFORM_MATRIX          /* m                                        */
} CssTransformKind;

typedef enum CssClipPathKind { CSS_CLIP_PATH_NONE = 0, CSS_CLIP_PATH_POLYGON } CssClipPathKind;
typedef enum CssContentKind { CSS_CONTENT_NORMAL = 0, CSS_CONTENT_NONE, CSS_CONTENT_STRING, CSS_CONTENT_ATTR } CssContentKind;
typedef enum CssImageKind { CSS_IMAGE_NONE = 0, CSS_IMAGE_URL, CSS_IMAGE_GRADIENT } CssImageKind;
typedef enum CssGradientKind { CSS_GRADIENT_LINEAR = 0, CSS_GRADIENT_RADIAL } CssGradientKind;
typedef enum CssRadialShape { CSS_RADIAL_ELLIPSE = 0, CSS_RADIAL_CIRCLE } CssRadialShape;
typedef enum CssRadialExtent { CSS_RADIAL_FARTHEST_CORNER = 0, CSS_RADIAL_CLOSEST_SIDE, CSS_RADIAL_CLOSEST_CORNER, CSS_RADIAL_FARTHEST_SIDE } CssRadialExtent;

/* will-change is accepted and ignored; kept as bits for completeness */
enum { CSS_WILL_CHANGE_NONE = 0, CSS_WILL_CHANGE_TRANSFORM = 1, CSS_WILL_CHANGE_OPACITY = 2, CSS_WILL_CHANGE_OTHER = 4 };

/* ------------------------------------------------------------------------- */
/* Compound value structs                                                     */
/* ------------------------------------------------------------------------- */

/* 2D affine matrix in CSS matrix(a,b,c,d,e,f) order: x' = a*x + c*y + e */
typedef struct Mat2D {
    float a, b, c, d, e, f;
} Mat2D;

typedef struct CssTransformOp {
    uint8_t kind;          /* CssTransformKind                                */
    CssLength tx, ty;      /* TRANSLATE                                       */
    float sx, sy;          /* SCALE                                           */
    float angle_deg;       /* ROTATE / SKEW x                                 */
    float angle2_deg;      /* SKEW y                                          */
    Mat2D m;               /* MATRIX                                          */
} CssTransformOp;

typedef struct CssTransform {
    CssTransformOp ops[CSS_MAX_TRANSFORM_OPS];
    uint8_t count;         /* 0 == no 2D ops.  translateZ()/3D ops are dropped. */
    uint8_t has_3d;        /* a 3D function was present (translateZ(0) etc.):  */
                           /* still `transform != none` (stacking context,     */
                           /* fixed containing block) and a composited-layer   */
                           /* hint (paint snaps the layer origin)              */
} CssTransform;

typedef struct CssShadow {
    CssColor color;        /* kind CURRENT when omitted                       */
    float x, y;            /* px                                              */
    float blur;            /* px, >= 0                                        */
    float spread;          /* px (box-shadow only)                            */
    uint8_t inset;
} CssShadow;

typedef struct CssShadowList {
    CssShadow items[CSS_MAX_TEXT_SHADOWS];
    uint8_t count;         /* 0 == none                                       */
} CssShadowList;

typedef struct CssGradientStop {
    CssColor color;
    CssLength pos;         /* px/pct along the gradient line                  */
    uint8_t has_pos;       /* 0: distribute evenly (CSS Images 3 rules)       */
} CssGradientStop;

typedef struct CssGradient {
    uint8_t kind;          /* CssGradientKind                                 */
    float angle_deg;       /* linear: 0 = to top, 90 = to right (CSS angle)   */
    uint8_t shape;         /* radial: CssRadialShape                          */
    uint8_t extent;        /* radial: CssRadialExtent                         */
    CssLength cx, cy;      /* radial centre (default 50% 50%)                 */
    CssGradientStop stops[CSS_MAX_GRADIENT_STOPS];
    uint8_t stop_count;
} CssGradient;

typedef struct CssImage {
    uint8_t kind;          /* CssImageKind                                    */
    const char *url;       /* atom, as written (relative to `base`)           */
    const char *base;      /* atom: root-relative URL of the sheet the url()  */
                           /* was declared in ("css/main.css"); NULL = the    */
                           /* document (inline style="" / DOM-ops styles)     */
    CssGradient gradient;
} CssImage;

typedef struct CssBgSize {
    uint8_t kind;          /* CssBgSizeKind                                   */
    CssLength w, h;        /* LENGTHS: auto allowed on either                 */
} CssBgSize;

typedef struct CssBgLayer {
    CssImage image;
    CssLength x, y;        /* position; `right 8px` -> {px:-8, pct:100}       */
    CssBgSize size;
    uint8_t repeat_x;      /* CssBgRepeat                                     */
    uint8_t repeat_y;
} CssBgLayer;

typedef struct CssFilterOp {
    uint8_t kind;          /* CssFilterKind                                   */
    float amount;          /* number (1 == 100%); hue-rotate: degrees; blur px */
    CssShadow shadow;      /* DROP_SHADOW                                     */
} CssFilterOp;

typedef struct CssTiming {
    uint8_t kind;          /* CssTimingKind                                   */
    float x1, y1, x2, y2;  /* CUBIC_BEZIER (also filled for the keywords)     */
    int steps;             /* STEPS                                           */
    uint8_t step_pos;      /* CssStepPosition                                 */
} CssTiming;

typedef struct CssAnimation {
    const char *name;      /* atom; NULL == none                              */
    float duration_s;
    float delay_s;         /* may be negative                                 */
    float iterations;      /* INFINITY for `infinite`                         */
    CssTiming timing;
    uint8_t direction;     /* CssAnimDirection                                */
    uint8_t fill;          /* CssAnimFill                                     */
    uint8_t play_state;    /* CssAnimPlayState                                */
} CssAnimation;

typedef struct CssTransition {
    int16_t prop;          /* CSS_PROP_* longhand id, CSS_PROP_ALL, or -1 for none */
    float duration_s;
    float delay_s;
    CssTiming timing;
} CssTransition;

typedef enum CssTrackKind {
    CSS_TRACK_SIZE = 0,    /* `size` is both sizing functions: px/pct/calc,   */
                           /* auto, min-content or max-content                */
    CSS_TRACK_FIT_CONTENT, /* fit-content(<length-percentage>): `size` = limit */
    CSS_TRACK_FR,          /* <flex>: `fr` (min sizing function auto)         */
    CSS_TRACK_MINMAX,      /* minmax(size, max)                               */
    CSS_TRACK_MINMAX_FR    /* minmax(size, <flex>): flex factor in `fr`       */
} CssTrackKind;

typedef struct CssTrack {
    CssLength size;        /* see CssTrackKind                                */
    CssLength max;         /* MINMAX: max sizing function (non-flex)          */
    float fr;              /* FR / MINMAX_FR: flex factor                     */
    uint8_t kind;          /* CssTrackKind                                    */
} CssTrack;

/* grid-row-start / grid-column-start / grid-row-end / grid-column-end:
 * auto (line 0, span 0, name NULL); [<integer> && <custom-ident>?] (line != 0,
 * negative counts from the end of the explicit grid); <custom-ident> (name,
 * line 0); span && [<integer> || <custom-ident>] (span 1, line = the count,
 * 1 when only a name is given). */
typedef struct CssGridLine {
    int16_t line;
    uint8_t span;
    uint8_t reserved_;     /* explicit padding: compared and hashed bytewise */
    uint32_t reserved2_;
    const char *name;      /* atom of the <custom-ident>, or NULL             */
} CssGridLine;

enum { CSS_GRID_ROW_START = 0, CSS_GRID_COLUMN_START, CSS_GRID_ROW_END, CSS_GRID_COLUMN_END };

/* Axis index of the per-axis grid fields: rows, then columns (the parity of
 * CSS_GRID_ROW_START / CSS_GRID_COLUMN_START). */
enum { CSS_GRID_AXIS_ROW = 0, CSS_GRID_AXIS_COLUMN = 1 };

#define CSS_MAX_GRID_AUTO_TRACKS 4

/* repeat(auto-fill | auto-fit, ...) inside a track list */
typedef enum CssGridRepeat { CSS_GRID_REPEAT_NONE = 0, CSS_GRID_REPEAT_AUTO_FILL, CSS_GRID_REPEAT_AUTO_FIT } CssGridRepeat;

/* Line names and the auto-repeat of one axis' track list.  line[i] holds the
 * names of line i (before track i) as a space-separated atom (NULL: none).
 * With an auto repeat the tracks [repeat_at, repeat_at + repeat_n) are the
 * repeated group; line[repeat_at] / line[repeat_at + repeat_n] are the names
 * just outside the repeat(), rep_first / rep_last the names at its inner
 * edges, and the lines strictly inside the group are line[]. */
typedef struct CssGridNames {
    const char *line[CSS_MAX_GRID_TRACKS + 1];
    const char *rep_first, *rep_last;
    uint8_t repeat;        /* CssGridRepeat                                   */
    uint8_t repeat_at, repeat_n;
    uint8_t reserved_[5];
} CssGridNames;

/* grid-auto-flow bits */
enum { CSS_GRID_FLOW_COLUMN = 1, CSS_GRID_FLOW_DENSE = 2 };

typedef struct CssCustomProp {
    const char *name;      /* atom including the leading "--"                 */
    const char *value;     /* atom of the raw, trimmed value text             */
} CssCustomProp;

typedef struct CssClipPath {
    uint8_t kind;          /* CssClipPathKind                                 */
    CssLength x[CSS_MAX_CLIP_POINTS], y[CSS_MAX_CLIP_POINTS];
    uint8_t point_count;
} CssClipPath;

typedef struct CssContent {
    uint8_t kind;          /* CssContentKind                                  */
    const char *str;       /* atom: STRING text or ATTR attribute name        */
} CssContent;

/* Side indices used by all [4] arrays */
enum { CSS_TOP = 0, CSS_RIGHT = 1, CSS_BOTTOM = 2, CSS_LEFT = 3 };
/* Corner indices for border_radius[4] */
enum { CSS_TOP_LEFT = 0, CSS_TOP_RIGHT = 1, CSS_BOTTOM_RIGHT = 2, CSS_BOTTOM_LEFT = 3 };

/* ------------------------------------------------------------------------- */
/* Computed style                                                             */
/* ------------------------------------------------------------------------- */
/*
 * Field order matters: all inherited properties come first (from `color` up
 * to but excluding `display`), all non-inherited ones after.  The two hashes
 * cover those byte ranges (see CSS_COMPUTED_INHERITED_BEGIN/END below).
 */
typedef struct CssComputed {
    /* ---- inherited ------------------------------------------------------ */
    CssColor color;
    const char *font_families[CSS_MAX_FONT_FAMILIES];  /* lower-cased atoms  */
    uint8_t font_family_count;
    float font_size;                  /* px, resolved                          */
    int16_t font_weight;              /* 100..1000 (999 allowed)               */
    uint8_t font_style;               /* CssFontStyle                          */
    CssLength line_height;            /* NORMAL | NUMBER | px                  */
    CssLength letter_spacing;         /* NORMAL | px                           */
    uint8_t text_align;               /* CssTextAlign                          */
    uint8_t text_transform;           /* CssTextTransform                      */
    uint8_t white_space;              /* CssWhiteSpace                         */
    uint8_t overflow_wrap;            /* CssOverflowWrap (word-wrap alias)     */
    uint8_t word_break;               /* CssWordBreak                          */
    uint8_t visibility;               /* CssVisibility                         */
    uint8_t cursor;                   /* CssCursor                             */
    uint8_t pointer_events;           /* CssPointerEvents                      */
    uint8_t user_select;              /* CssUserSelect                         */
    uint8_t direction;                /* CssDirection                          */
    uint8_t image_rendering;          /* CssImageRendering                     */
    uint8_t border_collapse;          /* CssBorderCollapse                     */
    uint8_t list_style_type;          /* CssListStyleType                      */
    CssLength border_spacing_x, border_spacing_y;
    CssShadowList text_shadow;
    CssCustomProp custom[CSS_MAX_CUSTOM_PROPS];
    uint8_t custom_count;
    /* Effective zoom (product of the `zoom` of the element and its ancestors,
     * Chrome semantics).  Every length in the record is already multiplied by
     * it (the cascade computes unzoomed, then css_computed_zoom()); layout only
     * needs it for natural sizes (images, form controls).  While the cascade
     * applies declarations it temporarily holds the element's own `zoom`. */
    float zoom;

    /* ---- non-inherited -------------------------------------------------- */
    uint8_t display;                  /* CssDisplay                            */
    uint8_t position;                 /* CssPosition                           */
    uint8_t float_;                   /* CssFloat                              */
    uint8_t clear;                    /* CssClear                              */
    uint8_t box_sizing;               /* CssBoxSizing                          */
    uint8_t table_layout;             /* CssTableLayout                        */
    uint8_t overflow_x, overflow_y;   /* CssOverflow                           */
    uint8_t appearance;               /* CssAppearance                         */
    uint8_t will_change;              /* bits, ignored by layout               */
    int32_t z_index;                  /* clamped to int32 like Chrome          */
    uint8_t z_index_auto;
    float opacity;                    /* 0..1                                  */

    CssLength inset[4];               /* top, right, bottom, left              */
    CssLength width, height;
    CssLength min_width, min_height;  /* auto == 0 for layout purposes         */
    CssLength max_width, max_height;  /* NONE when unconstrained               */
    CssLength margin[4];
    CssLength padding[4];

    float border_width[4];            /* px as specified; layout/paint use 0 when border_style is NONE/HIDDEN */
    uint8_t border_style[4];          /* CssBorderStyle                        */
    CssColor border_color[4];         /* kind CURRENT by default               */
    CssLength border_radius[4];       /* tl, tr, br, bl; pct: horizontal against width, vertical against height */
    float outline_width;
    uint8_t outline_style;            /* CssBorderStyle                        */
    CssColor outline_color;

    CssColor background_color;
    CssBgLayer bg_layers[CSS_MAX_BG_LAYERS];
    uint8_t bg_layer_count;           /* 0 == `background-image: none`         */

    CssTransform transform;
    CssLength transform_origin_x, transform_origin_y;  /* default 50% 50%     */
    CssShadow box_shadow;             /* the first (topmost) shadow            */
    CssShadow box_shadow_more[CSS_MAX_BOX_SHADOWS - 1];  /* the rest, in order */
    uint8_t box_shadow_count;         /* 0 .. CSS_MAX_BOX_SHADOWS (list length) */
    CssFilterOp filters[CSS_MAX_FILTER_OPS];
    uint8_t filter_count;
    CssClipPath clip_path;
    uint8_t object_fit;               /* CssObjectFit                          */
    CssContent content;

    uint8_t text_decoration;          /* bit set                               */
    uint8_t text_overflow;            /* CssTextOverflow                       */
    uint8_t vertical_align;           /* CssVerticalAlign                      */
    CssLength vertical_align_length;  /* when vertical_align == LENGTH         */

    uint8_t flex_direction;           /* CssFlexDirection                      */
    uint8_t flex_wrap;                /* CssFlexWrap                           */
    uint8_t justify_content;          /* CssJustifyContent                     */
    uint8_t align_items;              /* CssAlignItems                         */
    float flex_grow, flex_shrink;
    CssLength flex_basis;             /* AUTO by default                       */

    CssTrack grid_columns[CSS_MAX_GRID_TRACKS];
    uint8_t grid_column_count;        /* 0 == none                             */
    CssTrack grid_rows[CSS_MAX_GRID_TRACKS];
    uint8_t grid_row_count;           /* 0 == none                             */
    CssGridLine grid_lines[4];        /* CSS_GRID_ROW_START.. (grid-area order) */
    uint8_t justify_items;            /* CssAlignItems; legacy/normal == STRETCH */
    uint8_t justify_self, align_self; /* CssAlignItems; initial AUTO           */
    CssGridNames grid_names[2];       /* CSS_GRID_AXIS_ROW / _COLUMN           */
    CssTrack grid_auto_tracks[2][CSS_MAX_GRID_AUTO_TRACKS]; /* grid-auto-rows / -columns */
    uint8_t grid_auto_count[2];       /* >= 1 (initial: one auto track)        */
    uint8_t grid_auto_flow;           /* CSS_GRID_FLOW_* bits                  */
    uint8_t grid_area_rows, grid_area_cols; /* grid-template-areas size (0: none) */
    const char *grid_areas;           /* atom: rows '\n'-separated, cells ' '-separated, "." = null cell */
    CssLength row_gap, column_gap;    /* NORMAL == 0                           */

    CssAnimation animations[CSS_MAX_ANIMATIONS];
    uint8_t animation_count;          /* animation-name list length (<= max); name NULL == none */
    CssTransition transitions[CSS_MAX_TRANSITIONS];
    uint8_t transition_count;

    /* ---- hashes (not part of either range) ------------------------------ */
    uint64_t hash_inherited;
    uint64_t hash_non_inherited;
} CssComputed;

#define CSS_COMPUTED_INHERITED_BEGIN      offsetof(CssComputed, color)
#define CSS_COMPUTED_INHERITED_END        offsetof(CssComputed, display)
#define CSS_COMPUTED_NON_INHERITED_BEGIN  offsetof(CssComputed, display)
#define CSS_COMPUTED_NON_INHERITED_END    offsetof(CssComputed, hash_inherited)

#ifdef __cplusplus
}
#endif

#endif /* KN_CSS_TYPES_H */
