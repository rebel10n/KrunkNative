/*
 * css_props.h - the property table for the HTML/CSS engine.
 *
 * Longhands are listed with CSS_PROP(id, "name", inherited, "initial value").
 * The initial value is CSS text parsed once at startup by css_values.c to
 * build the initial CssComputed (see css_computed_init_defaults()).
 *
 * Shorthands (CSS_SHORTHAND) never reach the cascade: css_parse_declaration()
 * expands them into longhand callbacks at parse time.  When a shorthand's
 * value contains var() it cannot be split yet; each of its longhands is then
 * emitted with a leading CSS_TOK_SHORTHAND_REF token followed by the whole
 * shorthand value, and css_computed_apply() finishes the expansion after the
 * cascade has substituted the variables (see css_values.h).
 *
 * Vendor aliases (CSS_ALIAS) map prefixed names to the canonical id.  Only
 * aliases Blink (the reference renderer) honours are listed: Chrome drops
 * -moz-/-o-/-ms-/-khtml- declarations, so mapping them would let e.g. a later
 * `-moz-box-shadow` override the real `box-shadow`.  -webkit-box-flex is a
 * separate legacy property (display:-webkit-box only), not flex-grow.
 * tests/test_css_values.c checks this list against Chrome's.
 * Prefixed *values* Blink accepts (-webkit-calc(), legacy
 * -webkit-linear-gradient(left, ...), -webkit-optimize-contrast) are handled by
 * the value parsers; -moz-/-o- values and -webkit-sticky are rejected like Chrome.
 *
 * Property ids:  0 .. CSS_PROP_LONGHAND_COUNT-1   longhands
 *                CSS_PROP_LONGHAND_COUNT .. CSS_PROP_COUNT-1  shorthands
 *                CSS_PROP_ALL                      transition-property: all
 *                >= CSS_PROP_CUSTOM_BASE           custom properties (--x),
 *                                                  registered on first lookup
 *                CSS_PROP_INVALID (-1)             unknown property
 */
#ifndef KN_CSS_PROPS_H
#define KN_CSS_PROPS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* X(id, name, inherited, initial) --------------------------------------- */
#define CSS_PROP_LIST(X) \
    /* ---- inherited ---------------------------------------------------- */ \
    X(COLOR,                      "color",                      1, "#000") \
    X(FONT_FAMILY,                "font-family",                1, "serif") \
    X(FONT_SIZE,                  "font-size",                  1, "16px") \
    X(FONT_WEIGHT,                "font-weight",                1, "400") \
    X(FONT_STYLE,                 "font-style",                 1, "normal") \
    X(LINE_HEIGHT,                "line-height",                1, "normal") \
    X(LETTER_SPACING,             "letter-spacing",             1, "normal") \
    X(TEXT_ALIGN,                 "text-align",                 1, "start") \
    X(TEXT_TRANSFORM,             "text-transform",             1, "none") \
    X(WHITE_SPACE,                "white-space",                1, "normal") \
    X(OVERFLOW_WRAP,              "overflow-wrap",              1, "normal") \
    X(WORD_BREAK,                 "word-break",                 1, "normal") \
    X(VISIBILITY,                 "visibility",                 1, "visible") \
    X(CURSOR,                     "cursor",                     1, "auto") \
    X(POINTER_EVENTS,             "pointer-events",             1, "auto") \
    X(USER_SELECT,                "user-select",                1, "auto") \
    X(DIRECTION,                  "direction",                  1, "ltr") \
    X(IMAGE_RENDERING,            "image-rendering",            1, "auto") \
    X(BORDER_COLLAPSE,            "border-collapse",            1, "separate") \
    X(LIST_STYLE_TYPE,            "list-style-type",            1, "disc") \
    X(BORDER_SPACING,             "border-spacing",             1, "0") \
    X(TEXT_SHADOW,                "text-shadow",                1, "none") \
    X(TEXT_FILL_COLOR,            "-webkit-text-fill-color",    1, "currentcolor") \
    X(ACCENT_COLOR,               "accent-color",               1, "auto") \
    /* ---- non-inherited ------------------------------------------------ */ \
    X(DISPLAY,                    "display",                    0, "inline") \
    X(POSITION,                   "position",                   0, "static") \
    X(FLOAT,                      "float",                      0, "none") \
    X(CLEAR,                      "clear",                      0, "none") \
    X(BOX_SIZING,                 "box-sizing",                 0, "content-box") \
    X(TABLE_LAYOUT,               "table-layout",               0, "auto") \
    X(TEXT_WRAP,                  "text-wrap",                  0, "normal") \
    X(OVERFLOW_X,                 "overflow-x",                 0, "visible") \
    X(OVERFLOW_Y,                 "overflow-y",                 0, "visible") \
    X(APPEARANCE,                 "appearance",                 0, "none") \
    X(WILL_CHANGE,                "will-change",                0, "auto") \
    X(Z_INDEX,                    "z-index",                    0, "auto") \
    X(OPACITY,                    "opacity",                    0, "1") \
    X(TOP,                        "top",                        0, "auto") \
    X(RIGHT,                      "right",                      0, "auto") \
    X(BOTTOM,                     "bottom",                     0, "auto") \
    X(LEFT,                       "left",                       0, "auto") \
    X(WIDTH,                      "width",                      0, "auto") \
    X(HEIGHT,                     "height",                     0, "auto") \
    X(MIN_WIDTH,                  "min-width",                  0, "auto") \
    X(MIN_HEIGHT,                 "min-height",                 0, "auto") \
    X(MAX_WIDTH,                  "max-width",                  0, "none") \
    X(MAX_HEIGHT,                 "max-height",                 0, "none") \
    X(MARGIN_TOP,                 "margin-top",                 0, "0") \
    X(MARGIN_RIGHT,               "margin-right",               0, "0") \
    X(MARGIN_BOTTOM,              "margin-bottom",              0, "0") \
    X(MARGIN_LEFT,                "margin-left",                0, "0") \
    X(PADDING_TOP,                "padding-top",                0, "0") \
    X(PADDING_RIGHT,              "padding-right",              0, "0") \
    X(PADDING_BOTTOM,             "padding-bottom",             0, "0") \
    X(PADDING_LEFT,               "padding-left",               0, "0") \
    X(BORDER_TOP_WIDTH,           "border-top-width",           0, "medium") \
    X(BORDER_RIGHT_WIDTH,         "border-right-width",         0, "medium") \
    X(BORDER_BOTTOM_WIDTH,        "border-bottom-width",        0, "medium") \
    X(BORDER_LEFT_WIDTH,          "border-left-width",          0, "medium") \
    X(BORDER_TOP_STYLE,           "border-top-style",           0, "none") \
    X(BORDER_RIGHT_STYLE,         "border-right-style",         0, "none") \
    X(BORDER_BOTTOM_STYLE,        "border-bottom-style",        0, "none") \
    X(BORDER_LEFT_STYLE,          "border-left-style",          0, "none") \
    X(BORDER_TOP_COLOR,           "border-top-color",           0, "currentcolor") \
    X(BORDER_RIGHT_COLOR,         "border-right-color",         0, "currentcolor") \
    X(BORDER_BOTTOM_COLOR,        "border-bottom-color",        0, "currentcolor") \
    X(BORDER_LEFT_COLOR,          "border-left-color",          0, "currentcolor") \
    X(BORDER_TOP_LEFT_RADIUS,     "border-top-left-radius",     0, "0") \
    X(BORDER_TOP_RIGHT_RADIUS,    "border-top-right-radius",    0, "0") \
    X(BORDER_BOTTOM_RIGHT_RADIUS, "border-bottom-right-radius", 0, "0") \
    X(BORDER_BOTTOM_LEFT_RADIUS,  "border-bottom-left-radius",  0, "0") \
    X(OUTLINE_WIDTH,              "outline-width",              0, "medium") \
    X(OUTLINE_STYLE,              "outline-style",              0, "none") \
    X(OUTLINE_COLOR,              "outline-color",              0, "currentcolor") \
    X(BACKGROUND_COLOR,           "background-color",           0, "transparent") \
    X(BACKGROUND_IMAGE,           "background-image",           0, "none") \
    X(BACKGROUND_POSITION_X,      "background-position-x",      0, "0%") \
    X(BACKGROUND_POSITION_Y,      "background-position-y",      0, "0%") \
    X(BACKGROUND_SIZE,            "background-size",            0, "auto") \
    X(BACKGROUND_REPEAT,          "background-repeat",          0, "repeat") \
    X(TRANSFORM,                  "transform",                  0, "none") \
    X(TRANSFORM_ORIGIN,           "transform-origin",           0, "50% 50%") \
    X(BOX_SHADOW,                 "box-shadow",                 0, "none") \
    X(FILTER,                     "filter",                     0, "none") \
    X(CLIP_PATH,                  "clip-path",                  0, "none") \
    X(OBJECT_FIT,                 "object-fit",                 0, "fill") \
    X(CONTENT,                    "content",                    0, "normal") \
    X(TEXT_DECORATION,            "text-decoration",            0, "none") \
    X(TEXT_OVERFLOW,              "text-overflow",              0, "clip") \
    X(VERTICAL_ALIGN,             "vertical-align",             0, "baseline") \
    X(FLEX_DIRECTION,             "flex-direction",             0, "row") \
    X(FLEX_WRAP,                  "flex-wrap",                  0, "nowrap") \
    X(JUSTIFY_CONTENT,            "justify-content",            0, "normal") \
    X(ALIGN_CONTENT,              "align-content",              0, "normal") \
    X(ALIGN_ITEMS,                "align-items",                0, "stretch") \
    X(FLEX_GROW,                  "flex-grow",                  0, "0") \
    X(FLEX_SHRINK,                "flex-shrink",                0, "1") \
    X(FLEX_BASIS,                 "flex-basis",                 0, "auto") \
    X(GRID_TEMPLATE_COLUMNS,      "grid-template-columns",      0, "none") \
    X(GRID_TEMPLATE_ROWS,         "grid-template-rows",         0, "none") \
    X(GRID_ROW_START,             "grid-row-start",             0, "auto") \
    X(GRID_COLUMN_START,          "grid-column-start",          0, "auto") \
    X(GRID_ROW_END,               "grid-row-end",               0, "auto") \
    X(GRID_COLUMN_END,            "grid-column-end",            0, "auto") \
    X(JUSTIFY_ITEMS,              "justify-items",              0, "legacy") \
    X(JUSTIFY_SELF,               "justify-self",               0, "auto") \
    X(ALIGN_SELF,                 "align-self",                 0, "auto") \
    X(GRID_TEMPLATE_AREAS,        "grid-template-areas",        0, "none") \
    X(GRID_AUTO_ROWS,             "grid-auto-rows",             0, "auto") \
    X(GRID_AUTO_COLUMNS,          "grid-auto-columns",          0, "auto") \
    X(GRID_AUTO_FLOW,             "grid-auto-flow",             0, "row") \
    X(ROW_GAP,                    "row-gap",                    0, "normal") \
    X(COLUMN_GAP,                 "column-gap",                 0, "normal") \
    X(ANIMATION_NAME,             "animation-name",             0, "none") \
    X(ANIMATION_DURATION,         "animation-duration",         0, "0s") \
    X(ANIMATION_TIMING_FUNCTION,  "animation-timing-function",  0, "ease") \
    X(ANIMATION_DELAY,            "animation-delay",            0, "0s") \
    X(ANIMATION_ITERATION_COUNT,  "animation-iteration-count",  0, "1") \
    X(ANIMATION_DIRECTION,        "animation-direction",        0, "normal") \
    X(ANIMATION_FILL_MODE,        "animation-fill-mode",        0, "none") \
    X(ANIMATION_PLAY_STATE,       "animation-play-state",       0, "running") \
    X(TRANSITION_PROPERTY,        "transition-property",        0, "all") \
    X(TRANSITION_DURATION,        "transition-duration",        0, "0s") \
    X(TRANSITION_TIMING_FUNCTION, "transition-timing-function", 0, "ease") \
    X(TRANSITION_DELAY,           "transition-delay",           0, "0s") \
    X(ZOOM,                       "zoom",                       0, "1") \
    X(BACKDROP_FILTER,            "backdrop-filter",            0, "none") \
    X(PERSPECTIVE,                "perspective",                0, "none") \
    X(PERSPECTIVE_ORIGIN,         "perspective-origin",         0, "50% 50%") \
    X(TRANSFORM_STYLE,            "transform-style",            0, "flat") \
    X(BACKFACE_VISIBILITY,        "backface-visibility",        0, "visible") \
    X(MASK_IMAGE,                 "mask-image",                 0, "none") \
    X(MASK_POSITION,              "mask-position",              0, "0% 0%") \
    X(MASK_SIZE,                  "mask-size",                  0, "auto") \
    X(MASK_REPEAT,                "mask-repeat",                0, "repeat") \
    X(MASK_MODE,                  "mask-mode",                  0, "match-source") \
    X(BACKGROUND_CLIP,            "background-clip",            0, "border-box") \
    X(BACKGROUND_ORIGIN,          "background-origin",          0, "padding-box") \
    X(OUTLINE_OFFSET,             "outline-offset",             0, "0") \
    X(ASPECT_RATIO,               "aspect-ratio",               0, "auto") \
    X(LINE_CLAMP,                 "-webkit-line-clamp",         0, "none") \
    X(BOX_ORIENT,                 "-webkit-box-orient",         0, "horizontal") \
    /* ---- accepted and ignored (parse never fails on them) -------------- */ \
    X(FONT_FEATURE_SETTINGS,      "font-feature-settings",      1, "normal") \
    X(FONT_SMOOTHING,             "-webkit-font-smoothing",     1, "auto") \
    X(TOUCH_CALLOUT,              "-webkit-touch-callout",      1, "default") \
    X(USER_DRAG,                  "user-drag",                  0, "auto") \
    X(TEXT_RENDERING,             "text-rendering",             1, "auto")

/* X(id, name) ----------------------------------------------------------- */
#define CSS_SHORTHAND_LIST(X) \
    X(MARGIN,               "margin") \
    X(PADDING,              "padding") \
    X(BORDER,               "border") \
    X(BORDER_TOP,           "border-top") \
    X(BORDER_RIGHT,         "border-right") \
    X(BORDER_BOTTOM,        "border-bottom") \
    X(BORDER_LEFT,          "border-left") \
    X(BORDER_COLOR,         "border-color") \
    X(BORDER_WIDTH,         "border-width") \
    X(BORDER_STYLE,         "border-style") \
    X(BORDER_RADIUS,        "border-radius") \
    X(OUTLINE,              "outline") \
    X(BACKGROUND,           "background") \
    X(BACKGROUND_POSITION,  "background-position") \
    X(ANIMATION,            "animation") \
    X(TRANSITION,           "transition") \
    X(OVERFLOW,             "overflow") \
    X(FLEX_FLOW,            "flex-flow") \
    X(FLEX,                 "flex") \
    X(INSET,                "inset") \
    X(GAP,                  "gap") \
    X(FONT,                 "font") \
    X(LIST_STYLE,           "list-style") \
    X(GRID_ROW,             "grid-row") \
    X(GRID_COLUMN,          "grid-column") \
    X(GRID_AREA,            "grid-area") \
    X(PLACE_SELF,           "place-self") \
    X(GRID_TEMPLATE,        "grid-template") \
    X(GRID,                 "grid") \
    X(MASK,                 "mask")

/* X(name, canonical_id) ------------------------------------------------- */
#define CSS_ALIAS_LIST(X) \
    X("-webkit-transform",                  TRANSFORM) \
    X("-webkit-transform-origin",           TRANSFORM_ORIGIN) \
    X("-webkit-box-shadow",                 BOX_SHADOW) \
    X("-webkit-border-radius",              BORDER_RADIUS) \
    X("-webkit-animation",                  ANIMATION) \
    X("-webkit-animation-name",             ANIMATION_NAME) \
    X("-webkit-animation-duration",         ANIMATION_DURATION) \
    X("-webkit-animation-timing-function",  ANIMATION_TIMING_FUNCTION) \
    X("-webkit-animation-delay",            ANIMATION_DELAY) \
    X("-webkit-animation-iteration-count",  ANIMATION_ITERATION_COUNT) \
    X("-webkit-animation-direction",        ANIMATION_DIRECTION) \
    X("-webkit-animation-fill-mode",        ANIMATION_FILL_MODE) \
    X("-webkit-animation-play-state",       ANIMATION_PLAY_STATE) \
    X("-webkit-transition",                 TRANSITION) \
    X("-webkit-transition-property",        TRANSITION_PROPERTY) \
    X("-webkit-transition-duration",        TRANSITION_DURATION) \
    X("-webkit-transition-timing-function", TRANSITION_TIMING_FUNCTION) \
    X("-webkit-transition-delay",           TRANSITION_DELAY) \
    X("-webkit-user-select",                USER_SELECT) \
    X("-webkit-appearance",                 APPEARANCE) \
    X("-webkit-filter",                     FILTER) \
    X("-webkit-clip-path",                  CLIP_PATH) \
    X("-webkit-flex-grow",                  FLEX_GROW) \
    X("-webkit-flex-direction",             FLEX_DIRECTION) \
    X("-webkit-flex-wrap",                  FLEX_WRAP) \
    X("-webkit-justify-content",            JUSTIFY_CONTENT) \
    X("-webkit-align-items",                ALIGN_ITEMS) \
    X("-webkit-align-self",                 ALIGN_SELF) \
    X("-webkit-box-sizing",                 BOX_SIZING) \
    X("-webkit-font-feature-settings",      FONT_FEATURE_SETTINGS) \
    X("-webkit-user-drag",                  USER_DRAG) \
    X("text-decoration-line",               TEXT_DECORATION) \
    X("word-wrap",                          OVERFLOW_WRAP) \
    X("grid-row-gap",                       ROW_GAP) \
    X("grid-column-gap",                    COLUMN_GAP) \
    X("grid-gap",                           GAP) \
    X("-webkit-backdrop-filter",            BACKDROP_FILTER) \
    X("-webkit-perspective",                PERSPECTIVE) \
    X("-webkit-perspective-origin",         PERSPECTIVE_ORIGIN) \
    X("-webkit-transform-style",            TRANSFORM_STYLE) \
    X("-webkit-backface-visibility",        BACKFACE_VISIBILITY) \
    X("-webkit-mask",                       MASK) \
    X("-webkit-mask-image",                 MASK_IMAGE) \
    X("-webkit-mask-position",              MASK_POSITION) \
    X("-webkit-mask-size",                  MASK_SIZE) \
    X("-webkit-mask-repeat",                MASK_REPEAT) \
    X("-webkit-mask-mode",                  MASK_MODE) \
    X("-webkit-background-clip",            BACKGROUND_CLIP) \
    X("-webkit-background-origin",          BACKGROUND_ORIGIN)

typedef enum CssPropId {
#define X(id, name, inh, init) CSS_PROP_##id,
    CSS_PROP_LIST(X)
#undef X
    CSS_PROP_LONGHAND_COUNT,
    CSS_PROP_SHORTHAND_FIRST_ = CSS_PROP_LONGHAND_COUNT - 1,
#define X(id, name) CSS_PROP_##id,
    CSS_SHORTHAND_LIST(X)
#undef X
    CSS_PROP_COUNT,
    CSS_PROP_ALL = CSS_PROP_COUNT,      /* transition-property: all */
    CSS_PROP_CUSTOM_BASE = 1024,        /* custom properties: base + index */
    CSS_PROP_CUSTOM_MAX = 1024 + 256,
    CSS_PROP_INVALID = -1
} CssPropId;

#define CSS_PROP_IS_LONGHAND(id)  ((id) >= 0 && (id) < CSS_PROP_LONGHAND_COUNT)
#define CSS_PROP_IS_SHORTHAND(id) ((id) >= CSS_PROP_LONGHAND_COUNT && (id) < CSS_PROP_COUNT)
#define CSS_PROP_IS_CUSTOM(id)    ((id) >= CSS_PROP_CUSTOM_BASE && (id) < CSS_PROP_CUSTOM_MAX)

/* Case-insensitive lookup by name; resolves vendor aliases; registers
 * "--custom" names on first sight (returns CSS_PROP_CUSTOM_BASE + n).
 * Returns CSS_PROP_INVALID for unknown names. */
int css_prop_lookup(const char *name, size_t len);

/* Canonical name for an id (custom ids return the "--name" atom). */
const char *css_prop_name(int id);

/* 1 for inherited longhands and custom properties. */
int css_prop_is_inherited(int id);

/* Initial value text from the table (longhands only, else NULL). */
const char *css_prop_initial_text(int id);

#ifdef __cplusplus
}
#endif

#endif /* KN_CSS_PROPS_H */
