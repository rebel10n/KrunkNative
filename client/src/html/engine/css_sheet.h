/*
 * css_sheet.h - a loaded stylesheet: rules (declaration blocks parsed through
 * css_values), per-selector entries (Lexbor selector lists tagged with the
 * stripped pseudo-element), the rule index by rightmost compound, @font-face
 * descriptors and @keyframes.
 *
 * Lexbor only parses; selector specificity comes from its AST, pseudo-element
 * selectors (which Lexbor rejects as "bad style" rules) are stripped and
 * re-parsed here, and declarations are re-parsed from their raw text by
 * css_parse_declaration() so calc()/var()/shorthands all go through one value
 * parser.  Token slices point into the sheet's private copy of the source
 * text, which lives as long as the sheet.
 *
 * @media: rules inside an @media block (nested blocks AND their queries) are
 * parsed and kept like any other; only those whose media queries match the
 * current media environment (css_media_set_env: the viewport in CSS px, ...)
 * are in the rule index the cascade reads.  css_sheet_media_update()
 * re-evaluates the queries and rebuilds the index when a result flipped;
 * the caller then restyles everything (engine.c does on resize).
 */
#ifndef KN_CSS_SHEET_H
#define KN_CSS_SHEET_H

#include <stddef.h>
#include <stdint.h>

#include <lexbor.h>

#include "css_types.h"
#include "css_values.h"
#include "dom.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum CssPseudoElem {
    CSS_PE_NONE = 0,
    CSS_PE_BEFORE,
    CSS_PE_AFTER,
    CSS_PE_PLACEHOLDER,
    CSS_PE_SCROLLBAR,
    CSS_PE_SCROLLBAR_TRACK,
    CSS_PE_SCROLLBAR_THUMB,
    CSS_PE_SLIDER_THUMB,
    CSS_PE_INNER_SPIN_BUTTON,
    CSS_PE_OUTER_SPIN_BUTTON,
    CSS_PE_COLOR_SWATCH,
    CSS_PE_COLOR_SWATCH_WRAPPER,
    CSS_PE_SLIDER_TRACK,            /* ::-webkit-slider-runnable-track */
    CSS_PE_SELECTION,               /* ::selection (selection.c: highlight colours) */
    CSS_PE_COUNT
} CssPseudoElem;

/* One longhand (or custom property) declaration. */
typedef struct CssDecl {
    int prop;               /* CSS_PROP_* longhand or custom id              */
    uint8_t important;
    uint8_t has_var;        /* tokens contain var(): substitute at cascade   */
    uint16_t ntok;
    CssToken *tokens;       /* owned by the sheet; text points into source   */
} CssDecl;

/* One declaration block in source order. */
typedef struct CssRule {
    CssDecl *decls;
    uint32_t ndecls;
    uint32_t order;         /* 0-based position among the sheet's rules      */
    uint32_t stamp;         /* cascade scratch: last restyle that visited it */
    int32_t media;          /* innermost enclosing @media block, -1: none    */
} CssRule;

/* Index key kinds (the rightmost compound's most specific simple selector). */
enum { CSS_KEY_UNIVERSAL = 0, CSS_KEY_TAG = 't', CSS_KEY_CLASS = '.', CSS_KEY_ID = '#' };

/* One complex selector (a single Lexbor list, no comma chain). */
typedef struct CssSelector {
    lxb_css_selector_list_t *list;  /* NULL when the selector never matches  */
    uint32_t specificity;           /* (a << 20) | (b << 10) | c             */
    uint32_t rule;                  /* index into CssSheet.rules             */
    uint8_t pe;                     /* CssPseudoElem                         */
    uint8_t key_kind;
    const char *key;                /* lower-cased atom (id/class/tag)       */
    const char *text;               /* selector as written (diagnostics)     */
} CssSelector;

typedef struct CssFontFace {
    const char *family;             /* atom, as written (quotes removed)     */
    const char *src[4];             /* url() list, atoms, as written         */
    uint8_t src_count;
    int16_t weight;                 /* 400 default                           */
    uint8_t style;                  /* CssFontStyle                          */
    const char *unicode_range;      /* raw text or NULL                      */
} CssFontFace;

typedef struct CssKeyframe {
    float offset;                   /* 0..100 (percent)                      */
    CssDecl *decls;
    uint32_t ndecls;
} CssKeyframe;

typedef struct CssKeyframes {
    const char *name;               /* atom                                  */
    CssKeyframe *frames;            /* sorted by offset, stable              */
    uint32_t nframes;
} CssKeyframes;

typedef struct CssSheet CssSheet;

/* Parses `text` (copied) into a sheet.  `name` is only used in diagnostics
 * (may be NULL).  Never fails on broken CSS; returns NULL only on OOM. */
CssSheet *css_sheet_load(const char *text, size_t len, const char *name);
void css_sheet_destroy(CssSheet *sheet);

const char *css_sheet_name(const CssSheet *sheet);
uint32_t css_sheet_rule_count(const CssSheet *sheet);
CssRule *css_sheet_rule(const CssSheet *sheet, uint32_t i);
uint32_t css_sheet_selector_count(const CssSheet *sheet);
const CssSelector *css_sheet_selector(const CssSheet *sheet, uint32_t i);

/* Candidate selectors whose rightmost compound carries this key.  `key` is
 * matched case-insensitively (Lexbor's matcher is case-insensitive for ids
 * and classes too).  Returns a pointer to `*count` selector indices. */
const uint32_t *css_sheet_bucket(const CssSheet *sheet, int kind, const char *key, size_t len, uint32_t *count);
/* Selectors with no id/class/tag in the rightmost compound. */
const uint32_t *css_sheet_universal(const CssSheet *sheet, uint32_t *count);

const CssKeyframes *css_sheet_keyframes(const CssSheet *sheet, const char *name);
uint32_t css_sheet_keyframes_count(const CssSheet *sheet);
const CssKeyframes *css_sheet_keyframes_at(const CssSheet *sheet, uint32_t i);
const CssFontFace *css_sheet_font_faces(const CssSheet *sheet, uint32_t *count);

/* @property --name { syntax; inherits; initial-value } (CSS Properties and
 * Values API 1).  Descriptors as written: validity (syntax / initial-value
 * agreement) is checked by the registry (anim_property_register).  Rules
 * without a syntax or inherits descriptor are dropped here. */
typedef struct CssPropertyRule {
    const char *name;      /* atom including "--"                             */
    const char *syntax;    /* atom, quotes removed ("<number>", "*")          */
    const char *initial;   /* atom of the trimmed initial-value, or NULL      */
    uint8_t inherits;
} CssPropertyRule;
const CssPropertyRule *css_sheet_properties(const CssSheet *sheet, uint32_t *count);

/* Which selector features (classes, ids, attributes, state pseudo-classes)
 * appear where in this sheet's selectors; see DomInvalidation in dom.h. */
const DomInvalidation *css_sheet_invalidation(const CssSheet *sheet);
/* Unique id of this load (distinguishes a reloaded sheet at a reused address). */
uint32_t css_sheet_serial(const CssSheet *sheet);

/* ---- @media (Media Queries 4) -------------------------------------------
 * The environment queries are evaluated against.  Native defaults are a
 * desktop Chrome: screen, hover/fine pointer, light scheme, no reduced
 * motion, 1 dppx.  Lengths are CSS px (the engine's viewport: 1 CSS px per
 * framebuffer px); `em`/`rem` in a query are the initial 16px. */
typedef struct CssMediaEnv {
    float width, height;            /* viewport, CSS px                      */
    float dppx;                     /* resolution / device-pixel-ratio       */
    uint8_t dark;                   /* prefers-color-scheme: dark            */
    uint8_t reduced_motion;         /* prefers-reduced-motion: reduce        */
    uint8_t coarse;                 /* pointer: coarse, hover: none (touch)  */
} CssMediaEnv;

/* Sets the process-wide media environment (sheets loaded afterwards use it;
 * call css_sheet_media_update() on the existing ones). */
void css_media_set_env(const CssMediaEnv *env);
const CssMediaEnv *css_media_env(void);
/* 1 when the media query list `text` (an @media prelude, "" == all) matches
 * the current environment.  Unknown features / syntax errors evaluate to
 * false for that query, like Blink. */
int css_media_query_matches(const char *text, size_t len);
/* Re-evaluates the sheet's @media blocks against the current environment;
 * returns 1 when a block's result changed (the rule index was rebuilt and
 * every element needs restyling), 0 otherwise. */
int css_sheet_media_update(CssSheet *sheet);
/* Number of @media blocks in the sheet / how many currently match. */
uint32_t css_sheet_media_count(const CssSheet *sheet);
uint32_t css_sheet_media_matching(const CssSheet *sheet);

/* Strips a trailing pseudo-element from selector text in place of `out`
 * (returns the kind, CSS_PE_NONE when none, -1 when the selector can never
 * match: -moz-* or unknown pseudo-elements).  Exposed for tests. */
int css_sheet_strip_pseudo_element(const char *sel, size_t len, char *out, size_t cap, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* KN_CSS_SHEET_H */
