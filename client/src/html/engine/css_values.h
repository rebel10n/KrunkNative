/*
 * css_values.h - CSS tokenizer, declaration splitter, value parsers and the
 * CssComputed helpers (defaults, inheritance, apply, hash).
 *
 * All parsers take a token slice (tokens, n) and return 1 on success, 0 on
 * failure.  On failure the output is left untouched.  Parsers never allocate
 * except for atoms (kn_atom) which live for the process.
 *
 * Division of labour with the cascade (css_cascade.c):
 *   - em: parsers leave em in CssLength.em; css_computed_apply() resolves it
 *     with the font size it is given (for font-size itself the parent's size).
 *   - var(): the cascade owns custom properties.  Before applying a
 *     declaration whose tokens contain var() (css_tokens_have_var()) it calls
 *     css_substitute_vars() with a resolver and applies the result.
 *   - Shorthands containing var() are emitted as CSS_TOK_SHORTHAND_REF +
 *     original tokens for every longhand (see css_props.h); after var
 *     substitution css_computed_apply() expands them transparently.
 *   - inherit / initial / unset are handled inside css_computed_apply().
 */
#ifndef KN_CSS_VALUES_H
#define KN_CSS_VALUES_H

#include <stddef.h>
#include <stdint.h>

#include "css_types.h"
#include "css_props.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/* Tokens (CSS Syntax Level 3 subset)                                         */
/* ------------------------------------------------------------------------- */

typedef enum CssTokType {
    CSS_TOK_EOF = 0,
    CSS_TOK_IDENT,          /* text/len = name (not lower-cased)              */
    CSS_TOK_FUNCTION,       /* text/len = name without '('; nested tokens follow until the matching RPAREN */
    CSS_TOK_HASH,           /* text/len = name after '#'                      */
    CSS_TOK_STRING,         /* text/len = contents without quotes             */
    CSS_TOK_URL,            /* text/len = unquoted url( contents, trimmed     */
    CSS_TOK_NUMBER,         /* num; flags & CSS_TOKF_INTEGER                  */
    CSS_TOK_PERCENTAGE,     /* num (50% -> 50)                                */
    CSS_TOK_DIMENSION,      /* num, unit (CssUnit); text/len = unit text      */
    CSS_TOK_DELIM,          /* text[0] = the character                        */
    CSS_TOK_COMMA,
    CSS_TOK_COLON,
    CSS_TOK_SEMICOLON,
    CSS_TOK_LPAREN, CSS_TOK_RPAREN,
    CSS_TOK_LBRACKET, CSS_TOK_RBRACKET,
    CSS_TOK_LBRACE, CSS_TOK_RBRACE,
    CSS_TOK_WHITESPACE,     /* kept so parsers can distinguish `1px -2px` from `1px-2px`; most parsers skip it via css_tokens_strip_ws() */
    CSS_TOK_BAD,            /* bad string / bad url                           */
    CSS_TOK_SHORTHAND_REF   /* ival = shorthand id (pending expansion marker) */
} CssTokType;

typedef enum CssUnit {
    CSS_UNIT_NONE = 0, CSS_UNIT_PX, CSS_UNIT_PERCENT, CSS_UNIT_EM, CSS_UNIT_REM,
    CSS_UNIT_S, CSS_UNIT_MS, CSS_UNIT_DEG, CSS_UNIT_RAD, CSS_UNIT_GRAD, CSS_UNIT_TURN,
    CSS_UNIT_VW, CSS_UNIT_VH, CSS_UNIT_PT, CSS_UNIT_CH, CSS_UNIT_EX, CSS_UNIT_FR,
    CSS_UNIT_VMIN, CSS_UNIT_VMAX,
    CSS_UNIT_UNKNOWN
} CssUnit;

enum {
    CSS_TOKF_INTEGER = 1,
    CSS_TOKF_ID_HASH = 2,
    CSS_TOKF_SIGNED = 4,         /* number written with an explicit + or -        */
    CSS_TOKF_FULL_POSITION = 8   /* set by shorthand expansion: item is a full <position> for background-position-x/y */
};

typedef struct CssToken {
    uint8_t type;           /* CssTokType                                     */
    uint8_t unit;           /* CssUnit (DIMENSION)                            */
    uint8_t flags;
    const char *text;       /* points into the source text (never copied)     */
    size_t len;
    double num;
    int ival;               /* SHORTHAND_REF: shorthand property id           */
} CssToken;

/* Tokenizes [text, text+len).  Returns the number of tokens written (at most
 * max); the stream is truncated silently when it does not fit.  Comments are
 * dropped; whitespace runs become one CSS_TOK_WHITESPACE token. */
int css_tokenize(const char *text, size_t len, CssToken *out, int max);

/* Removes whitespace tokens in place; returns the new count. */
int css_tokens_strip_ws(CssToken *tokens, int n);

/* Number of tokens making up the component value starting at index i: one
 * token, or a FUNCTION/LPAREN/LBRACKET block through its matching closer. */
int css_component_len(const CssToken *tokens, int n, int i);

/* Renders a token slice back to text (for diagnostics / custom props). */
size_t css_tokens_to_text(const CssToken *tokens, int n, char *out, size_t cap);

/* ------------------------------------------------------------------------- */
/* Declaration blocks                                                         */
/* ------------------------------------------------------------------------- */

/* prop_id is a longhand id or a custom property id (never a shorthand).
 * tokens exclude !important and surrounding whitespace runs are kept (use
 * css_tokens_strip_ws if you want them gone). */
typedef void (*CssDeclCb)(void *user, int prop_id, const CssToken *tokens, int n, int important);

/* Parses `name: value [!important] ; ...` and emits one callback per
 * longhand.  Shorthands are expanded (see css_props.h).  Unknown properties
 * and unparsable shorthands are dropped.  Returns the number of declarations
 * (as written) that produced at least one callback. */
int css_parse_declaration_block(const char *text, size_t len, CssDeclCb cb, void *user);

/* Same for a single declaration already split into name and value text.
 * Returns 1 when at least one callback was emitted. */
int css_parse_declaration(const char *name, size_t name_len,
                          const char *value, size_t value_len,
                          int important, CssDeclCb cb, void *user);

/* Expands a shorthand token list into longhand callbacks.  Returns the
 * number of longhands emitted, 0 when the value is invalid.  With var()
 * inside the value, every longhand is emitted with a leading
 * CSS_TOK_SHORTHAND_REF (pending expansion). */
int css_expand_shorthand(int shorthand_id, const CssToken *tokens, int n,
                         int important, CssDeclCb cb, void *user);

/* ------------------------------------------------------------------------- */
/* var()                                                                      */
/* ------------------------------------------------------------------------- */

int css_tokens_have_var(const CssToken *tokens, int n);

/* Returns the raw value text of custom property `name` (including "--"),
 * NUL-terminated and stable for the duration of the call, or NULL. */
typedef const char *(*CssVarResolver)(void *user, const char *name, size_t name_len);

/* Replaces every var(--x[, fallback]) by its resolved tokens (recursively,
 * depth-limited).  Returns the number of output tokens, or -1 when a
 * variable is missing without fallback or the output does not fit (the
 * declaration is then invalid at computed-value time: treat as `unset`). */
int css_substitute_vars(const CssToken *in, int n, CssToken *out, int max,
                        CssVarResolver resolve, void *user);

/* ------------------------------------------------------------------------- */
/* Value parsers                                                              */
/* ------------------------------------------------------------------------- */

/* Flags for css_parse_length* */
enum {
    CSS_LENF_PCT         = 1 << 0,  /* allow percentages                     */
    CSS_LENF_AUTO        = 1 << 1,  /* allow `auto`                          */
    CSS_LENF_NONE        = 1 << 2,  /* allow `none`                          */
    CSS_LENF_NORMAL      = 1 << 3,  /* allow `normal`                        */
    CSS_LENF_NUMBER      = 1 << 4,  /* allow unitless numbers -> CSS_LEN_NUMBER */
    CSS_LENF_CONTENT     = 1 << 5,  /* allow min-content/max-content/fit-content */
    CSS_LENF_NONNEG      = 1 << 6,  /* reject negative px/pct                */
    CSS_LENF_QUIRKY      = 1 << 7   /* treat unitless numbers as px          */
};

/* Whole slice must be one length (calc() allowed; em kept in .em). */
int css_parse_length(const CssToken *tokens, int n, unsigned flags, CssLength *out);
/* Parses one length component at tokens[0..], sets *consumed. */
int css_parse_length_item(const CssToken *tokens, int n, unsigned flags, CssLength *out, int *consumed);
/* Folds the em component: px += em * font_px; em = 0. */
void css_length_resolve_em(CssLength *len, float font_px);

/* Viewport (CSS px) that vw/vh/vmin/vmax resolve against.  They are absolute
 * lengths: resolved to px when a value is parsed/computed, so a viewport
 * change needs a full restyle when css_values_viewport_units_used() (any
 * viewport unit seen so far).  Also used by min()/max()/clamp(), which accept
 * comparable arguments only (all absolute, all %, all em or all numbers). */
void css_values_set_viewport(float w, float h);
int css_values_viewport_units_used(void);
/* Resolves against a reference size (pct) returning px; keyword kinds -> 0. */
float css_length_resolve(const CssLength *len, float reference_px);

int css_parse_number(const CssToken *tokens, int n, float *out);
int css_parse_integer(const CssToken *tokens, int n, int *out);
/* Chrome-like: clamps to int32 (`99999999999999999999` -> INT32_MAX); `auto` -> *is_auto=1. */
int css_parse_z_index(const CssToken *tokens, int n, int32_t *out, int *is_auto);
int css_parse_opacity(const CssToken *tokens, int n, float *out);   /* clamps 0..1, accepts % */
int css_parse_angle_deg(const CssToken *tokens, int n, float *out); /* deg/rad/grad/turn, 0 */
int css_parse_time_s(const CssToken *tokens, int n, float *out);    /* s/ms, negative allowed */

/* Colors: #rgb #rgba #rrggbb #rrggbbaa, rgb()/rgba() (comma and space syntax,
 * 3-arg rgba, `/ alpha`, percentages), hsl()/hsla(), named colors,
 * transparent, currentColor. */
int css_parse_color(const CssToken *tokens, int n, CssColor *out);
int css_parse_color_item(const CssToken *tokens, int n, CssColor *out, int *consumed);
CssColor css_color_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a);
/* kind CURRENT -> current, else itself. */
CssColor css_color_resolve(CssColor c, CssColor current);
/* color-mix(in srgb|oklab, c1 p1?, c2 p2?) of two resolved colours (premultiplied
 * interpolation, CSS Color 5 §2); w1 = weight of c1 after percentage normalisation,
 * alpha_mul = the result's alpha multiplier (sum of percentages < 100%). */
CssColor css_color_mix(CssColor c1, CssColor c2, float w1, float alpha_mul, int space /* CssColorSpace */);

/* Keyword tables: {"name", value} terminated by a NULL name. */
typedef struct CssKeyword {
    const char *name;
    int value;
} CssKeyword;
int css_parse_keyword(const CssToken *tokens, int n, const CssKeyword *table, int *out);
int css_keyword_lookup(const CssToken *tok, const CssKeyword *table, int *out);

/* Per-property parsers ---------------------------------------------------- */
int css_parse_transform(const CssToken *tokens, int n, CssTransform *out);   /* translateZ(0) dropped (has_3d set); `none` -> count 0 */
/* Transform list helpers shared by layout, paint and anim (3D-aware). */
void css_transform_op_identity(CssTransformOp *op, const CssTransformOp *like); /* neutral op of like's kind (keeps a rotation axis) */
int  css_transform_op_equal(const CssTransformOp *a, const CssTransformOp *b);
int  css_transform_op_lerp(const CssTransformOp *a, const CssTransformOp *b, float t, CssTransformOp *out); /* 0: not interpolable per op */
int  css_transform_is_3d(const CssTransform *t);   /* any op with a z / perspective component */
/* Column-major 4x4 of the op list for a w x h border box (percentages of
 * translate resolve against it); transform-origin NOT folded in. */
void css_transform_to_mat4(const CssTransform *t, float w, float h, float out[16]);
int css_parse_transform_origin(const CssToken *tokens, int n, CssLength *x, CssLength *y);
int css_parse_position_xy(const CssToken *tokens, int n, CssLength *x, CssLength *y); /* <bg-position>: keywords, `right 8px center`, 1-4 values */
int css_parse_box_shadow(const CssToken *tokens, int n, CssShadow *out, int *count);  /* keeps the first of a comma list; none -> 0 */
/* The whole list (up to `max` kept, more are valid and dropped); none -> 0. */
int css_parse_box_shadow_list(const CssToken *tokens, int n, CssShadow *out, int max, int *count);
int css_parse_text_shadow(const CssToken *tokens, int n, CssShadowList *out);        /* up to CSS_MAX_TEXT_SHADOWS */
int css_parse_shadow_item(const CssToken *tokens, int n, int allow_inset_spread, CssShadow *out, int *consumed);
/* Base URL recorded into CssImage.base by css_parse_image() (atom or NULL =
 * document).  The cascade sets it per declaration before applying it. */
void css_values_set_url_base(const char *base_atom);
int css_parse_image(const CssToken *tokens, int n, CssImage *out, int *consumed);    /* url(), gradients, none */
int css_parse_gradient(const CssToken *tokens, int n, CssGradient *out, int *consumed);
int css_parse_bg_image_list(const CssToken *tokens, int n, CssImage *out, int max, int *count);
int css_parse_bg_position_list(const CssToken *tokens, int n, int axis /*0=x,1=y*/, CssLength *out, int max, int *count);
int css_parse_bg_size_item(const CssToken *tokens, int n, CssBgSize *out, int *consumed);
int css_parse_bg_size_list(const CssToken *tokens, int n, CssBgSize *out, int max, int *count);
int css_parse_bg_repeat_item(const CssToken *tokens, int n, uint8_t *rx, uint8_t *ry, int *consumed);
int css_parse_bg_repeat_list(const CssToken *tokens, int n, uint8_t *rx, uint8_t *ry, int max, int *count);
int css_parse_filter(const CssToken *tokens, int n, CssFilterOp *out, int max, int *count);
int css_parse_clip_path(const CssToken *tokens, int n, CssClipPath *out);
int css_parse_timing(const CssToken *tokens, int n, CssTiming *out, int *consumed);
int css_parse_grid_tracks(const CssToken *tokens, int n, CssTrack *out, int max, int *count);
/* As css_parse_grid_tracks plus [line names] and repeat(auto-fill | auto-fit, ...) (names may be NULL: auto repeat rejected). */
int css_parse_grid_track_list(const CssToken *tokens, int n, CssTrack *out, int max, int *count, CssGridNames *names);
int css_parse_border_width(const CssToken *tokens, int n, CssLength *out, int *consumed); /* thin/medium/thick + lengths */
int css_parse_font_family(const CssToken *tokens, int n, const char **families, int max, int *count);
int css_parse_font_weight(const CssToken *tokens, int n, int *out);
int css_parse_line_height(const CssToken *tokens, int n, CssLength *out);
int css_parse_vertical_align(const CssToken *tokens, int n, uint8_t *kw, CssLength *len);
int css_parse_content(const CssToken *tokens, int n, CssContent *out);
int css_parse_text_decoration(const CssToken *tokens, int n, uint8_t *bits);
int css_parse_will_change(const CssToken *tokens, int n, uint8_t *bits);
/* animation-* longhand lists (comma separated).  *count receives the item count (<= max). */
int css_parse_animation_names(const CssToken *tokens, int n, const char **out, int max, int *count);
int css_parse_time_list(const CssToken *tokens, int n, float *out, int max, int *count);
int css_parse_timing_list(const CssToken *tokens, int n, CssTiming *out, int max, int *count);
int css_parse_iteration_list(const CssToken *tokens, int n, float *out, int max, int *count);
int css_parse_keyword_list(const CssToken *tokens, int n, const CssKeyword *table, uint8_t *out, int max, int *count);
int css_parse_transition_props(const CssToken *tokens, int n, int16_t *out, int max, int *count);
/* Whole-shorthand item parsers (used by css_expand_shorthand and css_computed_apply). */
int css_parse_animation_item(const CssToken *tokens, int n, CssAnimation *out, int *consumed);
int css_parse_transition_item(const CssToken *tokens, int n, CssTransition *out, int *consumed);

/* Keyword tables exported for the cascade/UA sheet and tests. */
extern const CssKeyword css_kw_display[];
extern const CssKeyword css_kw_position[];
extern const CssKeyword css_kw_overflow[];
extern const CssKeyword css_kw_cursor[];
extern const CssKeyword css_kw_border_style[];
extern const CssKeyword css_kw_timing[];

/* ------------------------------------------------------------------------- */
/* Computed style                                                             */
/* ------------------------------------------------------------------------- */

/* Fills *c with every property's initial value (font-size 16px). */
/* ---- @property (CSS Properties and Values API) ------------------------- */
typedef enum CssSyntax {
    CSS_SYNTAX_UNIVERSAL = 0,      /* "*": token stream, not interpolable     */
    CSS_SYNTAX_NUMBER, CSS_SYNTAX_INTEGER, CSS_SYNTAX_LENGTH, CSS_SYNTAX_PERCENTAGE,
    CSS_SYNTAX_LENGTH_PERCENTAGE, CSS_SYNTAX_COLOR, CSS_SYNTAX_ANGLE, CSS_SYNTAX_TIME
} CssSyntax;

typedef struct CssRegisteredProperty {
    const char *name;              /* atom "--x"                              */
    uint8_t syntax;                /* CssSyntax                               */
    uint8_t inherits;
    const char *initial;           /* atom of initial-value text, or NULL     */
} CssRegisteredProperty;

/* Typed value of a registered custom property (computed form). */
typedef struct CssTypedValue {
    uint8_t syntax;                /* CssSyntax                               */
    float num;                     /* NUMBER / INTEGER / PERCENTAGE (0..100) / ANGLE deg / TIME s */
    CssLength len;                 /* LENGTH / LENGTH_PERCENTAGE (em resolved by the caller) */
    CssColor color;                /* COLOR                                   */
} CssTypedValue;

/* Registers @property `name` (the at-rule prelude, "--x") with its descriptors
 * (raw text: syntax incl. quotes, "true"/"false", initial-value). Returns the
 * custom property id, or CSS_PROP_INVALID when the rule is invalid (unknown or
 * missing syntax, missing inherits, initial-value missing / not computationally
 * independent for a non-universal syntax). A later registration of the same
 * name replaces the earlier one (last rule wins, like Chrome's @property). */
int css_property_register(const char *name, size_t name_len, const char *syntax, size_t syntax_len,
                          const char *inherits, size_t inherits_len, const char *initial, size_t initial_len);
const CssRegisteredProperty *css_property_registration(int custom_prop_id);   /* NULL: unregistered */
void css_property_registry_clear(void);  /* new document / sheet reload */
int  css_parse_syntax(const char *text, size_t len);   /* "'<number>'" -> CssSyntax, -1 unsupported */
int  css_typed_value_parse(int syntax, const char *text, size_t len, float font_px, CssTypedValue *out);
void css_typed_value_lerp(const CssTypedValue *a, const CssTypedValue *b, float t, CssTypedValue *out);
size_t css_typed_value_format(const CssTypedValue *v, char *out, size_t cap); /* CSS text for var() substitution */

void css_computed_init_defaults(CssComputed *c);
/* The shared initial record (same contents as init_defaults). */
const CssComputed *css_computed_initial(void);
/* Copies the inherited properties (color .. custom props) from parent. */
void css_computed_inherit(CssComputed *dst, const CssComputed *parent);
/* Copies one longhand (or custom property by id) from src to dst. */
void css_computed_copy_prop(CssComputed *dst, const CssComputed *src, int prop_id);

/* Parses tokens for one longhand (or custom property) and stores the value.
 *   - inherit / initial / unset are resolved against parent (NULL == root)
 *     and the initial record.
 *   - em is folded with font_size_px (the element's own computed font size);
 *     for font-size itself em/% resolve against the parent's font size.
 *   - tokens starting with CSS_TOK_SHORTHAND_REF are expanded here.
 * Returns 1 when the declaration was valid and applied, 0 otherwise
 * (dst untouched). */
int css_computed_apply(CssComputed *dst, int prop_id, const CssToken *tokens, int n,
                       const CssComputed *parent, float font_size_px);

/* Recomputes hash_inherited / hash_non_inherited. */
void css_computed_hash(CssComputed *c);

/* CSS `zoom`: multiplies every absolute length of the record by k (font-size,
 * px parts of lengths/calc, borders, shadows, transforms' translations, ...;
 * percentages, numbers and `zoom` itself are left alone).  inherited_only
 * limits it to the inherited range.  Used by the cascade to compute an element
 * unzoomed and then apply its effective zoom (Chrome's model). */
void css_computed_zoom(CssComputed *c, float k, int inherited_only);
/* Chrome's used border width: 0 < w < 1 -> 1, otherwise floor (DPR 1). */
float css_snap_border_width(float w);

/* Custom property lookup (name includes "--"); NULL when absent. */
const char *css_computed_custom_get(const CssComputed *c, const char *name, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* KN_CSS_VALUES_H */
