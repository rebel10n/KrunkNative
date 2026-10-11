/*
 * anim.h - CSS Animations (@keyframes) and CSS Transitions for the HTML/CSS
 * engine (doc/html-engine-plan.md, phase 4).
 *
 * The cascade owns the "base" computed style of an element (everything but
 * animation/transition effects).  After it recomputes an element it calls
 *
 *     anim_update_elem(sys, elem, base, now_ms);
 *     style = anim_apply(sys, elem, base, &scratch, now_ms);
 *
 * anim_update_elem() syncs the running animations with `animation-name` and
 * starts/retargets transitions when a transitioned property changed since
 * the previous restyle.  anim_apply() returns `base` untouched when nothing
 * is animating, or fills `scratch` with the animated copy.
 *
 * Each frame the engine calls anim_tick(sys, now_ms); it invokes the
 * AnimDirtyFn for every element whose animated output changed (ANIM_DIRTY_*
 * says whether layout or only paint is affected).  anim_next_wakeup() tells
 * the engine when it has to repaint next (now_ms for continuous animations,
 * the next step boundary for steps() timing, +inf when idle).
 *
 * Model (CSS Animations 1 / CSS Transitions 1 / Web Animations 1):
 *  - start time = time the name appeared + animation-delay (negative delays
 *    start the animation part-way through); iteration count may be infinite;
 *    direction normal/reverse/alternate/alternate-reverse; fill
 *    none/forwards/backwards/both; play-state paused freezes local time.
 *  - the timing function applies per keyframe interval (a keyframe's own
 *    animation-timing-function overrides the animation's for the interval it
 *    starts).  Missing 0%/100% frames are synthesised from the element's base
 *    computed value, so a `from`-only set (`grow`) animates towards the
 *    element's own style, as Chrome does.
 *  - unknown animation names (`sprayAnm`) are kept as inert slots: no-op.
 *  - transitions start when the base value of a matching property
 *    (transition-property longhand, shorthand or `all`) changes between two
 *    restyles, from the current (possibly mid-transition) value, with the
 *    spec's reversing-adjusted shortening when a transition is reversed.
 *    Snapshots are kept only for elements whose style declares a transition.
 *  - animations are applied first, transitions on top (CSS cascade order:
 *    transition declarations beat animation declarations).
 *
 * Interpolation rules (anim_value_lerp):
 *  - numbers / px widths: linear.
 *  - CssLength: px and pct components lerped when both are CSS_LEN_VALUE
 *    (so 0 <-> 25.25em and 10px <-> 50% both work); unitless numbers
 *    (line-height) lerped together; any keyword side -> discrete at 50%.
 *  - colors: premultiplied-alpha RGBA linear interpolation (Chrome), rounded
 *    to the nearest 8-bit value; currentColor on either side -> discrete.
 *  - transform: op lists with the same length and pairwise kinds are
 *    interpolated per op (translate lengths, scale factors, rotate/skew
 *    angles, matrix components).  `none` on one side becomes the identity of
 *    the other side's structure.  Otherwise discrete at 50%.
 *  - filter: same scheme (hue-rotate angle, brightness/contrast/... amounts,
 *    drop-shadow components); `none` -> per-function identity.
 *  - box-shadow: one shadow on both sides with equal inset flag -> lerp.
 *
 * Only the properties listed in AnimProp are animatable; animating anything
 * else is a no-op.  CssComputed hashes are copied from base unchanged.
 */
#ifndef KN_HTML_ANIM_H
#define KN_HTML_ANIM_H

#include <stddef.h>
#include <stdint.h>

#include "css_types.h"
#include "css_props.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Same handle as html_dom.h / dom.h (index into the node table, 0 == none). */
typedef uint32_t HtmlElem;

/* ------------------------------------------------------------------------- */
/* Animatable properties                                                      */
/* ------------------------------------------------------------------------- */

typedef enum AnimProp {
    ANIM_OPACITY = 0,
    ANIM_FONT_SIZE,
    ANIM_LINE_HEIGHT,
    ANIM_LETTER_SPACING,
    ANIM_WIDTH, ANIM_HEIGHT,
    ANIM_MIN_WIDTH, ANIM_MIN_HEIGHT,
    ANIM_MAX_WIDTH, ANIM_MAX_HEIGHT,
    ANIM_TOP, ANIM_RIGHT, ANIM_BOTTOM, ANIM_LEFT,                      /* inset[] */
    ANIM_MARGIN_TOP, ANIM_MARGIN_RIGHT, ANIM_MARGIN_BOTTOM, ANIM_MARGIN_LEFT,
    ANIM_PADDING_TOP, ANIM_PADDING_RIGHT, ANIM_PADDING_BOTTOM, ANIM_PADDING_LEFT,
    ANIM_BORDER_TOP_WIDTH, ANIM_BORDER_RIGHT_WIDTH, ANIM_BORDER_BOTTOM_WIDTH, ANIM_BORDER_LEFT_WIDTH,
    ANIM_BORDER_TOP_LEFT_RADIUS, ANIM_BORDER_TOP_RIGHT_RADIUS,
    ANIM_BORDER_BOTTOM_RIGHT_RADIUS, ANIM_BORDER_BOTTOM_LEFT_RADIUS,
    ANIM_OUTLINE_WIDTH,
    ANIM_COLOR,
    ANIM_BACKGROUND_COLOR,
    ANIM_BORDER_TOP_COLOR, ANIM_BORDER_RIGHT_COLOR, ANIM_BORDER_BOTTOM_COLOR, ANIM_BORDER_LEFT_COLOR,
    ANIM_OUTLINE_COLOR,
    ANIM_TRANSFORM,
    ANIM_FILTER,
    ANIM_BOX_SHADOW,
    ANIM_PROP_COUNT
} AnimProp;

#define ANIM_PROP_BIT(p) ((uint64_t)1 << (p))
#define ANIM_PROP_ALL_MASK (ANIM_PROP_BIT(ANIM_PROP_COUNT) - 1)

/* Value kinds behind an AnimProp (anim_prop_kind). */
typedef enum AnimValueKind {
    ANIM_KIND_FLOAT = 0,   /* opacity, font-size, border/outline widths (px) */
    ANIM_KIND_LENGTH,      /* CssLength                                       */
    ANIM_KIND_COLOR,       /* CssColor                                        */
    ANIM_KIND_TRANSFORM,   /* CssTransform                                    */
    ANIM_KIND_FILTER,      /* AnimFilterList                                  */
    ANIM_KIND_SHADOW       /* AnimShadow (box-shadow)                         */
} AnimValueKind;

typedef struct AnimFilterList {
    CssFilterOp ops[CSS_MAX_FILTER_OPS];
    uint8_t count;
} AnimFilterList;

typedef struct AnimShadow {
    CssShadow shadow;
    uint8_t count;         /* 0 or 1                                          */
} AnimShadow;

/* One animatable value; the active member follows anim_prop_kind(prop). */
typedef union AnimValue {
    float f;
    CssLength len;
    CssColor color;
    CssTransform transform;
    AnimFilterList filter;
    AnimShadow shadow;
} AnimValue;

/* Dirty classes reported through AnimDirtyFn. */
enum {
    ANIM_DIRTY_PAINT  = 1,  /* opacity, colors, transform, filter, shadow     */
    ANIM_DIRTY_LAYOUT = 2   /* font-size, lengths, border widths (and paint)  */
};

AnimValueKind anim_prop_kind(AnimProp prop);
unsigned      anim_prop_dirty_flags(AnimProp prop);
const char   *anim_prop_name(AnimProp prop);
/* Longhand id -> its AnimProp, or -1 when the property is not animatable. */
int           anim_prop_from_css_prop(int css_prop_id);
/* Longhand, shorthand (border, margin, ...) or CSS_PROP_ALL -> bit mask of
 * the animatable longhands it covers (0 for non-animatable properties). */
uint64_t      anim_prop_mask_from_css_prop(int css_prop_id);

/* Read / write one property of a computed record. */
void anim_value_get(const CssComputed *c, AnimProp prop, AnimValue *out);
void anim_value_set(CssComputed *c, AnimProp prop, const AnimValue *v);
int  anim_value_equal(AnimProp prop, const AnimValue *a, const AnimValue *b);
/* 1 when a and b can be interpolated smoothly (transitions only start then). */
int  anim_value_interpolable(AnimProp prop, const AnimValue *a, const AnimValue *b);
/* out = a + (b - a) * t with the rules in the header comment; t may lie
 * outside [0,1] for overshooting beziers (clamped where the type needs it). */
void anim_value_lerp(AnimProp prop, const AnimValue *a, const AnimValue *b, float t, AnimValue *out);

/* ------------------------------------------------------------------------- */
/* Timing functions                                                           */
/* ------------------------------------------------------------------------- */

/* Output progress for input progress x in [0,1].  Keywords use their
 * standard control points regardless of CssTiming.x1..y2; steps() follows
 * CSS Easing 1 (jump-end default). */
float anim_timing_eval(const CssTiming *t, float x);
/* Solves y for x on cubic-bezier(x1,y1,x2,y2). */
float anim_cubic_bezier(float x1, float y1, float x2, float y2, float x);

/* ------------------------------------------------------------------------- */
/* Keyframes                                                                  */
/* ------------------------------------------------------------------------- */

typedef struct AnimKeyframeEntry {
    uint8_t prop;          /* AnimProp                                        */
    AnimValue value;
} AnimKeyframeEntry;

typedef struct AnimKeyframe {
    float offset;          /* 0..1 (from == 0, to == 1)                       */
    uint8_t has_timing;    /* animation-timing-function declared in the frame */
    CssTiming timing;
    AnimKeyframeEntry *entries;
    uint32_t count, cap;
} AnimKeyframe;

typedef struct AnimKeyframeSet {
    const char *name;      /* atom                                            */
    AnimKeyframe *frames;  /* sorted by offset, stable                        */
    uint32_t count, cap;
    uint64_t props;        /* union of all frames' properties                 */
} AnimKeyframeSet;

typedef struct AnimSystem AnimSystem;

/* Creates (or empties and returns) the set called name[0..len). */
AnimKeyframeSet *anim_keyframes_begin(AnimSystem *sys, const char *name, size_t len);
/* Appends a frame at `offset` (inserted in sorted order, after equal offsets). */
AnimKeyframe *anim_keyframes_add_frame(AnimKeyframeSet *set, float offset, const CssTiming *timing_or_null);
/* Sets one property on a frame (replaces an earlier entry for the prop). */
void anim_keyframe_set_value(AnimKeyframeSet *set, AnimKeyframe *frame, AnimProp prop, const AnimValue *v);
/* Copies every property in `props_mask` out of a CssComputed that the
 * caller filled with css_computed_apply() for the frame's declarations
 * (props_mask comes from anim_prop_mask_from_css_prop(prop_id) per decl). */
void anim_keyframe_set_from_computed(AnimKeyframeSet *set, AnimKeyframe *frame,
                                     const CssComputed *values, uint64_t props_mask);
const AnimKeyframeSet *anim_keyframes_find(const AnimSystem *sys, const char *name_atom);
/* Drops every set (running animations become inert until re-registered). */
void anim_keyframes_clear(AnimSystem *sys);

#ifndef KN_ANIM_NO_SHEET
typedef struct CssSheet CssSheet;
/* Registers every @keyframes block of a loaded sheet (later sheets/sets of
 * the same name replace earlier ones).  Frame declarations are resolved with
 * css_computed_apply() against `ctx` (em/% of font-size, inherit/initial),
 * normally the root element's computed style; a frame's own
 * animation-timing-function is honoured; var() inside keyframes is skipped.
 * Returns the number of sets loaded.  (Compiled out with KN_ANIM_NO_SHEET
 * for standalone builds without css_sheet.c/Lexbor.) */
int anim_keyframes_load_sheet(AnimSystem *sys, const CssSheet *sheet, const CssComputed *ctx);
#endif

/* ------------------------------------------------------------------------- */
/* System                                                                     */
/* ------------------------------------------------------------------------- */

typedef void (*AnimDirtyFn)(void *user, HtmlElem elem, unsigned dirty_flags);

typedef struct AnimElem AnimElem;

struct AnimSystem {
    AnimKeyframeSet *sets;     /* looked up by name atom on every sample      */
    uint32_t set_count, set_cap;

    AnimElem **elems;          /* dense, unordered                            */
    uint32_t elem_count, elem_cap;
    uint32_t *map_keys;        /* open addressing: HtmlElem -> elems index    */
    uint32_t *map_vals;
    uint32_t map_cap, map_used;

    AnimDirtyFn dirty_fn;
    void *dirty_user;
};

void anim_init(AnimSystem *sys, AnimDirtyFn dirty_fn, void *dirty_user);
void anim_free(AnimSystem *sys);

/* Call after the cascade computed `base` for elem (every restyle of it).
 * Returns 1 when the element currently has animation/transition effects, so
 * the caller should run anim_apply(). */
int anim_update_elem(AnimSystem *sys, HtmlElem elem, const CssComputed *base, double now_ms);

/* Returns base when nothing applies; otherwise copies base into *out,
 * overwrites the animated properties and returns out. */
const CssComputed *anim_apply(AnimSystem *sys, HtmlElem elem, const CssComputed *base,
                              CssComputed *out, double now_ms);

/* Advances time: reports every element whose output changed since the last
 * tick through the dirty callback and drops finished transitions. */
void anim_tick(AnimSystem *sys, double now_ms);

/* 1 when some animation or transition will still change its output. */
int    anim_has_active(const AnimSystem *sys, double now_ms);
/* Earliest time (ms) the output may change: now_ms when something is
 * animating continuously, the next steps()/delay boundary otherwise,
 * +INFINITY when idle. */
double anim_next_wakeup(const AnimSystem *sys, double now_ms);

/* Element removed from the document: forget its state. */
void anim_remove_elem(AnimSystem *sys, HtmlElem elem);
/* Diagnostics: number of elements with state, running animations and
 * transitions (any pointer may be NULL). */
void anim_stats(const AnimSystem *sys, uint32_t *elems, uint32_t *animations, uint32_t *transitions);

#ifdef __cplusplus
}
#endif

#endif /* KN_HTML_ANIM_H */
