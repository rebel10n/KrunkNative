/*
 * anim.c - CSS Animations and Transitions.  See anim.h for the model and the
 * interpolation rules.  Depends only on css_types.h/css_props.h (ids) and
 * util.h (atoms, vectors); keyframe values arrive already resolved.
 */
#include "anim.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"
#ifndef KN_ANIM_NO_SHEET
#include "css_sheet.h"
#include "css_values.h"
#endif

#ifndef INFINITY
#define INFINITY (1.0f / 0.0f)
#endif

/* ------------------------------------------------------------------------- */
/* Property table                                                             */
/* ------------------------------------------------------------------------- */

typedef struct PropInfo {
    uint8_t kind;          /* AnimValueKind                                   */
    uint8_t dirty;         /* ANIM_DIRTY_*                                    */
    int16_t css;           /* CSS_PROP_* longhand                             */
    const char *name;
} PropInfo;

#define P(kind, dirty, css, name) { ANIM_KIND_##kind, ANIM_DIRTY_##dirty, CSS_PROP_##css, name }
static const PropInfo PROPS[ANIM_PROP_COUNT] = {
    [ANIM_OPACITY]                    = P(FLOAT,     PAINT,  OPACITY,                    "opacity"),
    [ANIM_FONT_SIZE]                  = P(FLOAT,     LAYOUT, FONT_SIZE,                  "font-size"),
    [ANIM_LINE_HEIGHT]                = P(LENGTH,    LAYOUT, LINE_HEIGHT,                "line-height"),
    [ANIM_LETTER_SPACING]             = P(LENGTH,    LAYOUT, LETTER_SPACING,             "letter-spacing"),
    [ANIM_WIDTH]                      = P(LENGTH,    LAYOUT, WIDTH,                      "width"),
    [ANIM_HEIGHT]                     = P(LENGTH,    LAYOUT, HEIGHT,                     "height"),
    [ANIM_MIN_WIDTH]                  = P(LENGTH,    LAYOUT, MIN_WIDTH,                  "min-width"),
    [ANIM_MIN_HEIGHT]                 = P(LENGTH,    LAYOUT, MIN_HEIGHT,                 "min-height"),
    [ANIM_MAX_WIDTH]                  = P(LENGTH,    LAYOUT, MAX_WIDTH,                  "max-width"),
    [ANIM_MAX_HEIGHT]                 = P(LENGTH,    LAYOUT, MAX_HEIGHT,                 "max-height"),
    [ANIM_TOP]                        = P(LENGTH,    LAYOUT, TOP,                        "top"),
    [ANIM_RIGHT]                      = P(LENGTH,    LAYOUT, RIGHT,                      "right"),
    [ANIM_BOTTOM]                     = P(LENGTH,    LAYOUT, BOTTOM,                     "bottom"),
    [ANIM_LEFT]                       = P(LENGTH,    LAYOUT, LEFT,                       "left"),
    [ANIM_MARGIN_TOP]                 = P(LENGTH,    LAYOUT, MARGIN_TOP,                 "margin-top"),
    [ANIM_MARGIN_RIGHT]               = P(LENGTH,    LAYOUT, MARGIN_RIGHT,               "margin-right"),
    [ANIM_MARGIN_BOTTOM]              = P(LENGTH,    LAYOUT, MARGIN_BOTTOM,              "margin-bottom"),
    [ANIM_MARGIN_LEFT]                = P(LENGTH,    LAYOUT, MARGIN_LEFT,                "margin-left"),
    [ANIM_PADDING_TOP]                = P(LENGTH,    LAYOUT, PADDING_TOP,                "padding-top"),
    [ANIM_PADDING_RIGHT]              = P(LENGTH,    LAYOUT, PADDING_RIGHT,              "padding-right"),
    [ANIM_PADDING_BOTTOM]             = P(LENGTH,    LAYOUT, PADDING_BOTTOM,             "padding-bottom"),
    [ANIM_PADDING_LEFT]               = P(LENGTH,    LAYOUT, PADDING_LEFT,               "padding-left"),
    [ANIM_BORDER_TOP_WIDTH]           = P(FLOAT,     LAYOUT, BORDER_TOP_WIDTH,           "border-top-width"),
    [ANIM_BORDER_RIGHT_WIDTH]         = P(FLOAT,     LAYOUT, BORDER_RIGHT_WIDTH,         "border-right-width"),
    [ANIM_BORDER_BOTTOM_WIDTH]        = P(FLOAT,     LAYOUT, BORDER_BOTTOM_WIDTH,        "border-bottom-width"),
    [ANIM_BORDER_LEFT_WIDTH]          = P(FLOAT,     LAYOUT, BORDER_LEFT_WIDTH,          "border-left-width"),
    [ANIM_BORDER_TOP_LEFT_RADIUS]     = P(LENGTH,    PAINT,  BORDER_TOP_LEFT_RADIUS,     "border-top-left-radius"),
    [ANIM_BORDER_TOP_RIGHT_RADIUS]    = P(LENGTH,    PAINT,  BORDER_TOP_RIGHT_RADIUS,    "border-top-right-radius"),
    [ANIM_BORDER_BOTTOM_RIGHT_RADIUS] = P(LENGTH,    PAINT,  BORDER_BOTTOM_RIGHT_RADIUS, "border-bottom-right-radius"),
    [ANIM_BORDER_BOTTOM_LEFT_RADIUS]  = P(LENGTH,    PAINT,  BORDER_BOTTOM_LEFT_RADIUS,  "border-bottom-left-radius"),
    [ANIM_OUTLINE_WIDTH]              = P(FLOAT,     PAINT,  OUTLINE_WIDTH,              "outline-width"),
    [ANIM_COLOR]                      = P(COLOR,     PAINT,  COLOR,                      "color"),
    [ANIM_BACKGROUND_COLOR]           = P(COLOR,     PAINT,  BACKGROUND_COLOR,           "background-color"),
    [ANIM_BORDER_TOP_COLOR]           = P(COLOR,     PAINT,  BORDER_TOP_COLOR,           "border-top-color"),
    [ANIM_BORDER_RIGHT_COLOR]         = P(COLOR,     PAINT,  BORDER_RIGHT_COLOR,         "border-right-color"),
    [ANIM_BORDER_BOTTOM_COLOR]        = P(COLOR,     PAINT,  BORDER_BOTTOM_COLOR,        "border-bottom-color"),
    [ANIM_BORDER_LEFT_COLOR]          = P(COLOR,     PAINT,  BORDER_LEFT_COLOR,          "border-left-color"),
    [ANIM_OUTLINE_COLOR]              = P(COLOR,     PAINT,  OUTLINE_COLOR,              "outline-color"),
    [ANIM_TRANSFORM]                  = P(TRANSFORM, PAINT,  TRANSFORM,                  "transform"),
    [ANIM_FILTER]                     = P(FILTER,    PAINT,  FILTER,                     "filter"),
    [ANIM_BOX_SHADOW]                 = P(SHADOW,    PAINT,  BOX_SHADOW,                 "box-shadow"),
};
#undef P

/* Per-kind slot of each property inside AnimSnapshot (computed once). */
static uint8_t g_slot[ANIM_PROP_COUNT];
static uint8_t g_slot_count[6];
static int g_slots_ready;

static void props_init(void)
{
    if (g_slots_ready)
        return;
    memset(g_slot_count, 0, sizeof g_slot_count);
    for (int p = 0; p < ANIM_PROP_COUNT; p++)
        g_slot[p] = g_slot_count[PROPS[p].kind]++;
    g_slots_ready = 1;
}

AnimValueKind anim_prop_kind(AnimProp prop) { return (AnimValueKind)PROPS[prop].kind; }
unsigned anim_prop_dirty_flags(AnimProp prop) { return PROPS[prop].dirty; }
const char *anim_prop_name(AnimProp prop) { return PROPS[prop].name; }

int anim_prop_from_css_prop(int css_prop_id)
{
    for (int p = 0; p < ANIM_PROP_COUNT; p++)
        if (PROPS[p].css == css_prop_id)
            return p;
    return -1;
}

static uint64_t side_mask(AnimProp first) { return 0xFull << first; }

uint64_t anim_prop_mask_from_css_prop(int css_prop_id)
{
    if (css_prop_id == CSS_PROP_ALL)
        return ANIM_PROP_ALL_MASK;
    if (CSS_PROP_IS_LONGHAND(css_prop_id)) {
        int p = anim_prop_from_css_prop(css_prop_id);
        return p < 0 ? 0 : ANIM_PROP_BIT(p);
    }
    switch (css_prop_id) {
    case CSS_PROP_MARGIN:        return side_mask(ANIM_MARGIN_TOP);
    case CSS_PROP_PADDING:       return side_mask(ANIM_PADDING_TOP);
    case CSS_PROP_INSET:         return side_mask(ANIM_TOP);
    case CSS_PROP_BORDER_WIDTH:  return side_mask(ANIM_BORDER_TOP_WIDTH);
    case CSS_PROP_BORDER_COLOR:  return side_mask(ANIM_BORDER_TOP_COLOR);
    case CSS_PROP_BORDER_RADIUS: return side_mask(ANIM_BORDER_TOP_LEFT_RADIUS);
    case CSS_PROP_BORDER:        return side_mask(ANIM_BORDER_TOP_WIDTH) | side_mask(ANIM_BORDER_TOP_COLOR);
    case CSS_PROP_BORDER_TOP:    return ANIM_PROP_BIT(ANIM_BORDER_TOP_WIDTH) | ANIM_PROP_BIT(ANIM_BORDER_TOP_COLOR);
    case CSS_PROP_BORDER_RIGHT:  return ANIM_PROP_BIT(ANIM_BORDER_RIGHT_WIDTH) | ANIM_PROP_BIT(ANIM_BORDER_RIGHT_COLOR);
    case CSS_PROP_BORDER_BOTTOM: return ANIM_PROP_BIT(ANIM_BORDER_BOTTOM_WIDTH) | ANIM_PROP_BIT(ANIM_BORDER_BOTTOM_COLOR);
    case CSS_PROP_BORDER_LEFT:   return ANIM_PROP_BIT(ANIM_BORDER_LEFT_WIDTH) | ANIM_PROP_BIT(ANIM_BORDER_LEFT_COLOR);
    case CSS_PROP_OUTLINE:       return ANIM_PROP_BIT(ANIM_OUTLINE_WIDTH) | ANIM_PROP_BIT(ANIM_OUTLINE_COLOR);
    case CSS_PROP_BACKGROUND:    return ANIM_PROP_BIT(ANIM_BACKGROUND_COLOR);
    default:                     return 0;
    }
}

/* ------------------------------------------------------------------------- */
/* Field access on CssComputed                                                */
/* ------------------------------------------------------------------------- */

static float *computed_float(CssComputed *c, AnimProp p)
{
    switch (p) {
    case ANIM_OPACITY:       return &c->opacity;
    case ANIM_FONT_SIZE:     return &c->font_size;
    case ANIM_OUTLINE_WIDTH: return &c->outline_width;
    default:                 return &c->border_width[p - ANIM_BORDER_TOP_WIDTH];
    }
}

static CssLength *computed_length(CssComputed *c, AnimProp p)
{
    switch (p) {
    case ANIM_LINE_HEIGHT:    return &c->line_height;
    case ANIM_LETTER_SPACING: return &c->letter_spacing;
    case ANIM_WIDTH:          return &c->width;
    case ANIM_HEIGHT:         return &c->height;
    case ANIM_MIN_WIDTH:      return &c->min_width;
    case ANIM_MIN_HEIGHT:     return &c->min_height;
    case ANIM_MAX_WIDTH:      return &c->max_width;
    case ANIM_MAX_HEIGHT:     return &c->max_height;
    case ANIM_TOP: case ANIM_RIGHT: case ANIM_BOTTOM: case ANIM_LEFT:
        return &c->inset[p - ANIM_TOP];
    case ANIM_MARGIN_TOP: case ANIM_MARGIN_RIGHT: case ANIM_MARGIN_BOTTOM: case ANIM_MARGIN_LEFT:
        return &c->margin[p - ANIM_MARGIN_TOP];
    case ANIM_PADDING_TOP: case ANIM_PADDING_RIGHT: case ANIM_PADDING_BOTTOM: case ANIM_PADDING_LEFT:
        return &c->padding[p - ANIM_PADDING_TOP];
    default:
        return &c->border_radius[p - ANIM_BORDER_TOP_LEFT_RADIUS];
    }
}

static CssColor *computed_color(CssComputed *c, AnimProp p)
{
    switch (p) {
    case ANIM_COLOR:            return &c->color;
    case ANIM_BACKGROUND_COLOR: return &c->background_color;
    case ANIM_OUTLINE_COLOR:    return &c->outline_color;
    default:                    return &c->border_color[p - ANIM_BORDER_TOP_COLOR];
    }
}

void anim_value_get(const CssComputed *cc, AnimProp prop, AnimValue *out)
{
    CssComputed *c = (CssComputed *)cc;   /* read-only access through the field helpers */
    memset(out, 0, sizeof *out);
    switch (PROPS[prop].kind) {
    case ANIM_KIND_FLOAT:     out->f = *computed_float(c, prop); break;
    case ANIM_KIND_LENGTH:    out->len = *computed_length(c, prop); break;
    case ANIM_KIND_COLOR:     out->color = *computed_color(c, prop); break;
    case ANIM_KIND_TRANSFORM: out->transform = c->transform; break;
    case ANIM_KIND_FILTER:
        out->filter.count = c->filter_count;
        memcpy(out->filter.ops, c->filters, sizeof c->filters);
        break;
    case ANIM_KIND_SHADOW:
        out->shadow.count = c->box_shadow_count;
        out->shadow.shadow = c->box_shadow;
        break;
    }
}

void anim_value_set(CssComputed *c, AnimProp prop, const AnimValue *v)
{
    switch (PROPS[prop].kind) {
    case ANIM_KIND_FLOAT:     *computed_float(c, prop) = v->f; break;
    case ANIM_KIND_LENGTH:    *computed_length(c, prop) = v->len; break;
    case ANIM_KIND_COLOR:     *computed_color(c, prop) = v->color; break;
    case ANIM_KIND_TRANSFORM: c->transform = v->transform; break;
    case ANIM_KIND_FILTER:
        c->filter_count = v->filter.count;
        memcpy(c->filters, v->filter.ops, sizeof c->filters);
        break;
    case ANIM_KIND_SHADOW:
        c->box_shadow_count = v->shadow.count;
        c->box_shadow = v->shadow.shadow;
        break;
    }
}

/* ------------------------------------------------------------------------- */
/* Equality / interpolation                                                   */
/* ------------------------------------------------------------------------- */

static float lerpf(float a, float b, float t) { return a + (b - a) * t; }

/* CSS times are floats (0.15s == 0.1500000059); convert to milliseconds
 * rounded to the microsecond so declared end times are hit exactly. */
static double sec_to_ms(double s) { return floor(s * 1e6 + 0.5) / 1e3; }

static int length_equal(const CssLength *a, const CssLength *b)
{
    if (a->kind != b->kind)
        return 0;
    if (a->kind == CSS_LEN_VALUE)
        return a->px == b->px && a->pct == b->pct && a->em == b->em;
    if (a->kind == CSS_LEN_NUMBER)
        return a->px == b->px;
    return 1;
}

static int length_interpolable(const CssLength *a, const CssLength *b)
{
    return a->kind == b->kind && (a->kind == CSS_LEN_VALUE || a->kind == CSS_LEN_NUMBER);
}

static void length_lerp(const CssLength *a, const CssLength *b, float t, CssLength *out)
{
    if (!length_interpolable(a, b)) {
        *out = t < 0.5f ? *a : *b;
        return;
    }
    memset(out, 0, sizeof *out);
    out->kind = a->kind;
    out->px = lerpf(a->px, b->px, t);
    if (a->kind == CSS_LEN_VALUE) {
        out->pct = lerpf(a->pct, b->pct, t);
        out->em = lerpf(a->em, b->em, t);
    }
}

static int color_equal(const CssColor *a, const CssColor *b)
{
    if (a->kind != b->kind)
        return 0;
    if (a->kind == CSS_COLOR_CURRENT)
        return 1;
    return a->r == b->r && a->g == b->g && a->b == b->b && a->a == b->a;
}

static int color_interpolable(const CssColor *a, const CssColor *b)
{
    return a->kind == CSS_COLOR_RGBA && b->kind == CSS_COLOR_RGBA;
}

static uint8_t to_u8(float v)
{
    if (v <= 0.0f) return 0;
    if (v >= 255.0f) return 255;
    return (uint8_t)(v + 0.5f);
}

/* Premultiplied-alpha interpolation, as Chrome does for CSS colors. */
static void color_lerp(const CssColor *a, const CssColor *b, float t, CssColor *out)
{
    if (!color_interpolable(a, b)) {
        *out = t < 0.5f ? *a : *b;
        return;
    }
    float aa = a->a / 255.0f, ab = b->a / 255.0f;
    float alpha = lerpf(aa, ab, t);
    float r = lerpf(a->r * aa, b->r * ab, t);
    float g = lerpf(a->g * aa, b->g * ab, t);
    float bl = lerpf(a->b * aa, b->b * ab, t);
    memset(out, 0, sizeof *out);
    out->kind = CSS_COLOR_RGBA;
    if (alpha <= 0.0f) {
        out->a = 0;
        return;
    }
    if (alpha > 1.0f) alpha = 1.0f;
    out->r = to_u8(r / alpha);
    out->g = to_u8(g / alpha);
    out->b = to_u8(bl / alpha);
    out->a = to_u8(alpha * 255.0f);
}

static int transform_op_equal(const CssTransformOp *a, const CssTransformOp *b)
{
    if (a->kind != b->kind)
        return 0;
    switch (a->kind) {
    case CSS_TRANSFORM_TRANSLATE: return length_equal(&a->tx, &b->tx) && length_equal(&a->ty, &b->ty);
    case CSS_TRANSFORM_SCALE:     return a->sx == b->sx && a->sy == b->sy;
    case CSS_TRANSFORM_ROTATE:    return a->angle_deg == b->angle_deg;
    case CSS_TRANSFORM_SKEW:      return a->angle_deg == b->angle_deg && a->angle2_deg == b->angle2_deg;
    default:
        return a->m.a == b->m.a && a->m.b == b->m.b && a->m.c == b->m.c &&
               a->m.d == b->m.d && a->m.e == b->m.e && a->m.f == b->m.f;
    }
}

static int transform_equal(const CssTransform *a, const CssTransform *b)
{
    if (a->count != b->count)
        return 0;
    for (int i = 0; i < a->count; i++)
        if (!transform_op_equal(&a->ops[i], &b->ops[i]))
            return 0;
    return 1;
}

static void transform_op_identity(CssTransformOp *op, uint8_t kind)
{
    memset(op, 0, sizeof *op);
    op->kind = kind;
    switch (kind) {
    case CSS_TRANSFORM_SCALE:  op->sx = op->sy = 1.0f; break;
    case CSS_TRANSFORM_MATRIX: op->m.a = op->m.d = 1.0f; break;
    default: break;   /* translate 0 0, rotate 0, skew 0 0 */
    }
}

static void transform_identity_like(const CssTransform *shape, CssTransform *out)
{
    memset(out, 0, sizeof *out);
    out->count = shape->count;
    for (int i = 0; i < shape->count; i++)
        transform_op_identity(&out->ops[i], shape->ops[i].kind);
}

static int transform_pairable(const CssTransform *a, const CssTransform *b)
{
    if (a->count == 0 || b->count == 0)
        return 1;
    if (a->count != b->count)
        return 0;
    for (int i = 0; i < a->count; i++)
        if (a->ops[i].kind != b->ops[i].kind)
            return 0;
    return 1;
}

static void transform_op_lerp(const CssTransformOp *a, const CssTransformOp *b, float t, CssTransformOp *out)
{
    memset(out, 0, sizeof *out);
    out->kind = a->kind;
    switch (a->kind) {
    case CSS_TRANSFORM_TRANSLATE:
        length_lerp(&a->tx, &b->tx, t, &out->tx);
        length_lerp(&a->ty, &b->ty, t, &out->ty);
        break;
    case CSS_TRANSFORM_SCALE:
        out->sx = lerpf(a->sx, b->sx, t);
        out->sy = lerpf(a->sy, b->sy, t);
        break;
    case CSS_TRANSFORM_ROTATE:
        out->angle_deg = lerpf(a->angle_deg, b->angle_deg, t);
        break;
    case CSS_TRANSFORM_SKEW:
        out->angle_deg = lerpf(a->angle_deg, b->angle_deg, t);
        out->angle2_deg = lerpf(a->angle2_deg, b->angle2_deg, t);
        break;
    default:
        out->m.a = lerpf(a->m.a, b->m.a, t);
        out->m.b = lerpf(a->m.b, b->m.b, t);
        out->m.c = lerpf(a->m.c, b->m.c, t);
        out->m.d = lerpf(a->m.d, b->m.d, t);
        out->m.e = lerpf(a->m.e, b->m.e, t);
        out->m.f = lerpf(a->m.f, b->m.f, t);
        break;
    }
}

static void transform_lerp(const CssTransform *a, const CssTransform *b, float t, CssTransform *out)
{
    if (!transform_pairable(a, b)) {
        *out = t < 0.5f ? *a : *b;
        return;
    }
    CssTransform ia, ib;
    if (a->count == 0 && b->count == 0) {
        memset(out, 0, sizeof *out);
        return;
    }
    if (a->count == 0) { transform_identity_like(b, &ia); a = &ia; }
    if (b->count == 0) { transform_identity_like(a, &ib); b = &ib; }
    memset(out, 0, sizeof *out);
    out->count = a->count;
    for (int i = 0; i < a->count; i++)
        transform_op_lerp(&a->ops[i], &b->ops[i], t, &out->ops[i]);
}

static int shadow_equal(const CssShadow *a, const CssShadow *b)
{
    return color_equal(&a->color, &b->color) && a->x == b->x && a->y == b->y &&
           a->blur == b->blur && a->spread == b->spread && a->inset == b->inset;
}

static void shadow_lerp(const CssShadow *a, const CssShadow *b, float t, CssShadow *out)
{
    memset(out, 0, sizeof *out);
    color_lerp(&a->color, &b->color, t, &out->color);
    out->x = lerpf(a->x, b->x, t);
    out->y = lerpf(a->y, b->y, t);
    out->blur = lerpf(a->blur, b->blur, t);
    if (out->blur < 0.0f) out->blur = 0.0f;
    out->spread = lerpf(a->spread, b->spread, t);
    out->inset = a->inset;
}

static int filter_op_equal(const CssFilterOp *a, const CssFilterOp *b)
{
    if (a->kind != b->kind || a->amount != b->amount)
        return 0;
    return a->kind != CSS_FILTER_DROP_SHADOW || shadow_equal(&a->shadow, &b->shadow);
}

static int filter_equal(const AnimFilterList *a, const AnimFilterList *b)
{
    if (a->count != b->count)
        return 0;
    for (int i = 0; i < a->count; i++)
        if (!filter_op_equal(&a->ops[i], &b->ops[i]))
            return 0;
    return 1;
}

static void filter_op_identity(CssFilterOp *op, uint8_t kind)
{
    memset(op, 0, sizeof *op);
    op->kind = kind;
    switch (kind) {
    case CSS_FILTER_BRIGHTNESS: case CSS_FILTER_CONTRAST:
    case CSS_FILTER_SATURATE:   case CSS_FILTER_OPACITY:
        op->amount = 1.0f;
        break;
    case CSS_FILTER_DROP_SHADOW:
        op->shadow.color.kind = CSS_COLOR_RGBA;   /* transparent, 0 offset */
        break;
    default:
        break;   /* grayscale/hue-rotate/blur/invert/sepia: 0 */
    }
}

static int filter_pairable(const AnimFilterList *a, const AnimFilterList *b)
{
    if (a->count == 0 || b->count == 0)
        return 1;
    if (a->count != b->count)
        return 0;
    for (int i = 0; i < a->count; i++)
        if (a->ops[i].kind != b->ops[i].kind)
            return 0;
    return 1;
}

static void filter_lerp(const AnimFilterList *a, const AnimFilterList *b, float t, AnimFilterList *out)
{
    if (!filter_pairable(a, b)) {
        *out = t < 0.5f ? *a : *b;
        return;
    }
    AnimFilterList ia, ib;
    if (a->count == 0 && b->count == 0) {
        memset(out, 0, sizeof *out);
        return;
    }
    if (a->count == 0) {
        memset(&ia, 0, sizeof ia);
        ia.count = b->count;
        for (int i = 0; i < b->count; i++) filter_op_identity(&ia.ops[i], b->ops[i].kind);
        a = &ia;
    }
    if (b->count == 0) {
        memset(&ib, 0, sizeof ib);
        ib.count = a->count;
        for (int i = 0; i < a->count; i++) filter_op_identity(&ib.ops[i], a->ops[i].kind);
        b = &ib;
    }
    memset(out, 0, sizeof *out);
    out->count = a->count;
    for (int i = 0; i < a->count; i++) {
        CssFilterOp *o = &out->ops[i];
        o->kind = a->ops[i].kind;
        o->amount = lerpf(a->ops[i].amount, b->ops[i].amount, t);
        if (o->kind != CSS_FILTER_HUE_ROTATE && o->amount < 0.0f)
            o->amount = 0.0f;
        if (o->kind == CSS_FILTER_DROP_SHADOW)
            shadow_lerp(&a->ops[i].shadow, &b->ops[i].shadow, t, &o->shadow);
    }
}

int anim_value_equal(AnimProp prop, const AnimValue *a, const AnimValue *b)
{
    switch (PROPS[prop].kind) {
    case ANIM_KIND_FLOAT:     return a->f == b->f;
    case ANIM_KIND_LENGTH:    return length_equal(&a->len, &b->len);
    case ANIM_KIND_COLOR:     return color_equal(&a->color, &b->color);
    case ANIM_KIND_TRANSFORM: return transform_equal(&a->transform, &b->transform);
    case ANIM_KIND_FILTER:    return filter_equal(&a->filter, &b->filter);
    default:
        return a->shadow.count == b->shadow.count &&
               (a->shadow.count == 0 || shadow_equal(&a->shadow.shadow, &b->shadow.shadow));
    }
}

int anim_value_interpolable(AnimProp prop, const AnimValue *a, const AnimValue *b)
{
    switch (PROPS[prop].kind) {
    case ANIM_KIND_FLOAT:     return 1;
    case ANIM_KIND_LENGTH:    return length_interpolable(&a->len, &b->len);
    case ANIM_KIND_COLOR:     return color_interpolable(&a->color, &b->color);
    case ANIM_KIND_TRANSFORM: return transform_pairable(&a->transform, &b->transform);
    case ANIM_KIND_FILTER:    return filter_pairable(&a->filter, &b->filter);
    default:
        return a->shadow.count == 1 && b->shadow.count == 1 &&
               a->shadow.shadow.inset == b->shadow.shadow.inset &&
               color_interpolable(&a->shadow.shadow.color, &b->shadow.shadow.color);
    }
}

void anim_value_lerp(AnimProp prop, const AnimValue *a, const AnimValue *b, float t, AnimValue *out)
{
    AnimValue tmp;   /* out may alias a or b */
    memset(&tmp, 0, sizeof tmp);
    switch (PROPS[prop].kind) {
    case ANIM_KIND_FLOAT:
        tmp.f = lerpf(a->f, b->f, t);
        if (prop == ANIM_OPACITY) {
            if (tmp.f < 0.0f) tmp.f = 0.0f;
            if (tmp.f > 1.0f) tmp.f = 1.0f;
        } else if (tmp.f < 0.0f) {
            tmp.f = 0.0f;   /* font-size / border widths never go negative */
        }
        break;
    case ANIM_KIND_LENGTH:    length_lerp(&a->len, &b->len, t, &tmp.len); break;
    case ANIM_KIND_COLOR:     color_lerp(&a->color, &b->color, t, &tmp.color); break;
    case ANIM_KIND_TRANSFORM: transform_lerp(&a->transform, &b->transform, t, &tmp.transform); break;
    case ANIM_KIND_FILTER:    filter_lerp(&a->filter, &b->filter, t, &tmp.filter); break;
    default:
        if (anim_value_interpolable(prop, a, b)) {
            tmp.shadow.count = 1;
            shadow_lerp(&a->shadow.shadow, &b->shadow.shadow, t, &tmp.shadow.shadow);
        } else {
            tmp.shadow = t < 0.5f ? a->shadow : b->shadow;
        }
        break;
    }
    *out = tmp;
}

/* ------------------------------------------------------------------------- */
/* Timing functions                                                           */
/* ------------------------------------------------------------------------- */

float anim_cubic_bezier(float x1, float y1, float x2, float y2, float x)
{
    if (x <= 0.0f) return 0.0f;
    if (x >= 1.0f) return 1.0f;

    /* Polynomial coefficients (WebKit UnitBezier). */
    double cx = 3.0 * x1, bx = 3.0 * (x2 - x1) - cx, ax = 1.0 - cx - bx;
    double cy = 3.0 * y1, by = 3.0 * (y2 - y1) - cy, ay = 1.0 - cy - by;

    double t = x;
    int converged = 0;
    for (int i = 0; i < 8; i++) {
        double sx = ((ax * t + bx) * t + cx) * t - x;
        if (fabs(sx) < 1e-7) { converged = 1; break; }
        double d = (3.0 * ax * t + 2.0 * bx) * t + cx;
        if (fabs(d) < 1e-6)
            break;
        t -= sx / d;
    }
    if (!converged) {
        double lo = 0.0, hi = 1.0;
        t = x;
        for (int i = 0; i < 64 && hi - lo > 1e-7; i++) {
            double sx = ((ax * t + bx) * t + cx) * t;
            if (sx < x) lo = t; else hi = t;
            t = 0.5 * (lo + hi);
        }
    }
    return (float)(((ay * t + by) * t + cy) * t);
}

static float steps_eval(int n, uint8_t pos, float x)
{
    if (n < 1) n = 1;
    if (x < 0.0f) x = 0.0f;
    if (x >= 1.0f) return 1.0f;
    float k = floorf(x * (float)n + 1e-6f);
    float y;
    switch (pos) {
    case CSS_STEP_START:     y = (k + 1.0f) / (float)n; break;
    case CSS_STEP_JUMP_BOTH: y = (k + 1.0f) / (float)(n + 1); break;
    case CSS_STEP_JUMP_NONE: y = n > 1 ? k / (float)(n - 1) : 0.0f; break;
    default:                 y = k / (float)n; break;   /* end */
    }
    return y > 1.0f ? 1.0f : y;
}

float anim_timing_eval(const CssTiming *t, float x)
{
    switch (t->kind) {
    case CSS_TIMING_LINEAR:       return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
    case CSS_TIMING_EASE_IN:      return anim_cubic_bezier(0.42f, 0.0f, 1.0f, 1.0f, x);
    case CSS_TIMING_EASE_OUT:     return anim_cubic_bezier(0.0f, 0.0f, 0.58f, 1.0f, x);
    case CSS_TIMING_EASE_IN_OUT:  return anim_cubic_bezier(0.42f, 0.0f, 0.58f, 1.0f, x);
    case CSS_TIMING_CUBIC_BEZIER: return anim_cubic_bezier(t->x1, t->y1, t->x2, t->y2, x);
    case CSS_TIMING_STEPS:        return steps_eval(t->steps, t->step_pos, x);
    default:                      return anim_cubic_bezier(0.25f, 0.1f, 0.25f, 1.0f, x);   /* ease */
    }
}

/* ------------------------------------------------------------------------- */
/* Keyframe sets                                                              */
/* ------------------------------------------------------------------------- */

static void keyframe_set_reset(AnimKeyframeSet *set)
{
    for (uint32_t i = 0; i < set->count; i++)
        free(set->frames[i].entries);
    free(set->frames);
    set->frames = NULL;
    set->count = set->cap = 0;
    set->props = 0;
}

static AnimKeyframeSet *keyframes_find_mut(AnimSystem *sys, const char *name_atom)
{
    for (uint32_t i = 0; i < sys->set_count; i++)
        if (sys->sets[i].name == name_atom)
            return &sys->sets[i];
    return NULL;
}

const AnimKeyframeSet *anim_keyframes_find(const AnimSystem *sys, const char *name_atom)
{
    return keyframes_find_mut((AnimSystem *)sys, name_atom);
}

AnimKeyframeSet *anim_keyframes_begin(AnimSystem *sys, const char *name, size_t len)
{
    const char *atom = kn_atom(name, len);
    AnimKeyframeSet *set = keyframes_find_mut(sys, atom);
    if (set) {
        keyframe_set_reset(set);
        return set;
    }
    if (sys->set_count == sys->set_cap) {
        uint32_t cap = sys->set_cap ? sys->set_cap * 2 : 16;
        sys->sets = realloc(sys->sets, cap * sizeof *sys->sets);
        sys->set_cap = cap;
    }
    set = &sys->sets[sys->set_count++];
    memset(set, 0, sizeof *set);
    set->name = atom;
    return set;
}

AnimKeyframe *anim_keyframes_add_frame(AnimKeyframeSet *set, float offset, const CssTiming *timing)
{
    if (offset < 0.0f) offset = 0.0f;
    if (offset > 1.0f) offset = 1.0f;
    if (set->count == set->cap) {
        uint32_t cap = set->cap ? set->cap * 2 : 4;
        set->frames = realloc(set->frames, cap * sizeof *set->frames);
        set->cap = cap;
    }
    uint32_t pos = set->count;
    while (pos > 0 && set->frames[pos - 1].offset > offset)
        pos--;
    memmove(&set->frames[pos + 1], &set->frames[pos], (set->count - pos) * sizeof *set->frames);
    set->count++;
    AnimKeyframe *f = &set->frames[pos];
    memset(f, 0, sizeof *f);
    f->offset = offset;
    if (timing) {
        f->has_timing = 1;
        f->timing = *timing;
    }
    return f;
}

void anim_keyframe_set_value(AnimKeyframeSet *set, AnimKeyframe *frame, AnimProp prop, const AnimValue *v)
{
    for (uint32_t i = 0; i < frame->count; i++) {
        if (frame->entries[i].prop == prop) {
            frame->entries[i].value = *v;
            return;
        }
    }
    if (frame->count == frame->cap) {
        uint32_t cap = frame->cap ? frame->cap * 2 : 2;
        frame->entries = realloc(frame->entries, cap * sizeof *frame->entries);
        frame->cap = cap;
    }
    frame->entries[frame->count].prop = (uint8_t)prop;
    frame->entries[frame->count].value = *v;
    frame->count++;
    set->props |= ANIM_PROP_BIT(prop);
}

void anim_keyframe_set_from_computed(AnimKeyframeSet *set, AnimKeyframe *frame,
                                     const CssComputed *values, uint64_t props_mask)
{
    for (int p = 0; p < ANIM_PROP_COUNT; p++) {
        if (!(props_mask & ANIM_PROP_BIT(p)))
            continue;
        AnimValue v;
        anim_value_get(values, (AnimProp)p, &v);
        anim_keyframe_set_value(set, frame, (AnimProp)p, &v);
    }
}

void anim_keyframes_clear(AnimSystem *sys)
{
    for (uint32_t i = 0; i < sys->set_count; i++)
        keyframe_set_reset(&sys->sets[i]);
    free(sys->sets);
    sys->sets = NULL;
    sys->set_count = sys->set_cap = 0;
}

#ifndef KN_ANIM_NO_SHEET
int anim_keyframes_load_sheet(AnimSystem *sys, const CssSheet *sheet, const CssComputed *ctx)
{
    uint32_t n = css_sheet_keyframes_count(sheet);
    for (uint32_t i = 0; i < n; i++) {
        const CssKeyframes *kf = css_sheet_keyframes_at(sheet, i);
        AnimKeyframeSet *set = anim_keyframes_begin(sys, kf->name, strlen(kf->name));
        for (uint32_t k = 0; k < kf->nframes; k++) {
            const CssKeyframe *fr = &kf->frames[k];
            CssComputed tmp = *ctx;
            CssTiming timing;
            int has_timing = 0;
            uint64_t mask = 0;
            for (uint32_t d = 0; d < fr->ndecls; d++) {
                const CssDecl *decl = &fr->decls[d];
                if (decl->has_var)
                    continue;
                if (!css_computed_apply(&tmp, decl->prop, decl->tokens, decl->ntok, ctx, ctx->font_size))
                    continue;
                if (decl->prop == CSS_PROP_ANIMATION_TIMING_FUNCTION) {
                    timing = tmp.animations[0].timing;
                    has_timing = 1;
                    continue;
                }
                mask |= anim_prop_mask_from_css_prop(decl->prop);
            }
            AnimKeyframe *f = anim_keyframes_add_frame(set, fr->offset / 100.0f, has_timing ? &timing : NULL);
            anim_keyframe_set_from_computed(set, f, &tmp, mask);
        }
    }
    return (int)n;
}
#endif

static const AnimValue *frame_value(const AnimKeyframe *f, AnimProp prop)
{
    for (uint32_t i = 0; i < f->count; i++)
        if (f->entries[i].prop == prop)
            return &f->entries[i].value;
    return NULL;
}

/* 1 when every frame sits at 0 or 1 (single interval: steps() boundaries are
 * then predictable, see next_wakeup). */
static int set_single_interval(const AnimKeyframeSet *set)
{
    for (uint32_t i = 0; i < set->count; i++) {
        float o = set->frames[i].offset;
        if (o != 0.0f && o != 1.0f)
            return 0;
        if (set->frames[i].has_timing)
            return 0;
    }
    return 1;
}

typedef struct FrameRef {
    float offset;
    const AnimValue *value;
    const CssTiming *timing;   /* NULL: use the animation's */
} FrameRef;

#define ANIM_MAX_REFS 66

/* Value of `prop` at iteration progress p; implicit 0%/100% come from base. */
static int set_sample_prop(const AnimKeyframeSet *set, AnimProp prop, float p,
                           const CssTiming *anim_timing, const AnimValue *base, AnimValue *out)
{
    FrameRef refs[ANIM_MAX_REFS];
    int n = 0;
    refs[0].offset = 0.0f;   /* reserved slot for the implicit start frame */
    n = 1;
    for (uint32_t i = 0; i < set->count && n < ANIM_MAX_REFS - 1; i++) {
        const AnimValue *v = frame_value(&set->frames[i], prop);
        if (!v)
            continue;
        refs[n].offset = set->frames[i].offset;
        refs[n].value = v;
        refs[n].timing = set->frames[i].has_timing ? &set->frames[i].timing : NULL;
        n++;
    }
    if (n == 1)
        return 0;

    FrameRef *first = &refs[1];
    int start = 1;
    if (first->offset > 0.0f) {
        refs[0].value = base;
        refs[0].timing = NULL;
        start = 0;
    }
    if (refs[n - 1].offset < 1.0f) {
        refs[n].offset = 1.0f;
        refs[n].value = base;
        refs[n].timing = NULL;
        n++;
    }

    if (p >= refs[n - 1].offset) {
        *out = *refs[n - 1].value;
        return 1;
    }
    int a = start;   /* last frame at or before p; the next one is after p */
    while (a + 1 < n && refs[a + 1].offset <= p)
        a++;
    int b = a + 1;
    float span = refs[b].offset - refs[a].offset;
    float f = span > 0.0f ? (p - refs[a].offset) / span : 1.0f;
    float eased = anim_timing_eval(refs[a].timing ? refs[a].timing : anim_timing, f);
    anim_value_lerp(prop, refs[a].value, refs[b].value, eased, out);
    return 1;
}

/* ------------------------------------------------------------------------- */
/* Per-element state                                                          */
/* ------------------------------------------------------------------------- */

typedef struct AnimSnapshot {
    float f[8];
    CssLength len[24];
    CssColor color[8];
    CssTransform transform;
    AnimFilterList filter;
    AnimShadow shadow;
} AnimSnapshot;

static void snapshot_get(const AnimSnapshot *s, AnimProp p, AnimValue *out)
{
    memset(out, 0, sizeof *out);
    switch (PROPS[p].kind) {
    case ANIM_KIND_FLOAT:     out->f = s->f[g_slot[p]]; break;
    case ANIM_KIND_LENGTH:    out->len = s->len[g_slot[p]]; break;
    case ANIM_KIND_COLOR:     out->color = s->color[g_slot[p]]; break;
    case ANIM_KIND_TRANSFORM: out->transform = s->transform; break;
    case ANIM_KIND_FILTER:    out->filter = s->filter; break;
    default:                  out->shadow = s->shadow; break;
    }
}

static void snapshot_from_computed(AnimSnapshot *s, const CssComputed *c)
{
    AnimValue v;
    memset(s, 0, sizeof *s);
    for (int p = 0; p < ANIM_PROP_COUNT; p++) {
        anim_value_get(c, (AnimProp)p, &v);
        switch (PROPS[p].kind) {
        case ANIM_KIND_FLOAT:     s->f[g_slot[p]] = v.f; break;
        case ANIM_KIND_LENGTH:    s->len[g_slot[p]] = v.len; break;
        case ANIM_KIND_COLOR:     s->color[g_slot[p]] = v.color; break;
        case ANIM_KIND_TRANSFORM: s->transform = v.transform; break;
        case ANIM_KIND_FILTER:    s->filter = v.filter; break;
        default:                  s->shadow = v.shadow; break;
        }
    }
}

enum { PHASE_BEFORE = 0, PHASE_ACTIVE, PHASE_AFTER, PHASE_NONE = 255 };

typedef struct AnimRunning {
    const char *name;          /* atom                                        */
    double created_ms;         /* when the name appeared on the element       */
    float delay_s, duration_s, iterations;
    CssTiming timing;
    uint8_t direction, fill, play_state;
    double pause_local_s;      /* local time frozen while paused              */
    uint8_t last_phase;        /* dirty detection in anim_tick                */
    int32_t last_step;
} AnimRunning;

typedef struct AnimTransition {
    uint8_t prop;              /* AnimProp                                    */
    double start_ms, end_ms;   /* start includes the delay                    */
    CssTiming timing;
    AnimValue from, to;
    AnimValue reversing_start; /* CSS Transitions "reversing-adjusted start"  */
    float reversing_factor;    /* "reversing shortening factor"               */
} AnimTransition;

struct AnimElem {
    HtmlElem elem;
    AnimRunning anims[CSS_MAX_ANIMATIONS];
    uint8_t anim_count;
    AnimTransition *trans;
    uint32_t trans_count, trans_cap;
    AnimSnapshot prev;
    uint8_t has_prev;
};

/* ---- HtmlElem -> index map (open addressing, 0 empty, ~0u tombstone) ---- */

#define MAP_EMPTY 0u
#define MAP_TOMB  0xFFFFFFFFu

static uint32_t map_hash(HtmlElem e) { return (e * 2654435761u) ^ (e >> 15); }

static uint32_t map_find(const AnimSystem *sys, HtmlElem e)
{
    if (sys->map_cap == 0)
        return MAP_TOMB;
    uint32_t mask = sys->map_cap - 1;
    for (uint32_t i = map_hash(e) & mask;; i = (i + 1) & mask) {
        uint32_t k = sys->map_keys[i];
        if (k == MAP_EMPTY)
            return MAP_TOMB;
        if (k == e)
            return sys->map_vals[i];
    }
}

static void map_put_raw(uint32_t *keys, uint32_t *vals, uint32_t cap, HtmlElem e, uint32_t v)
{
    uint32_t mask = cap - 1;
    for (uint32_t i = map_hash(e) & mask;; i = (i + 1) & mask) {
        if (keys[i] == MAP_EMPTY || keys[i] == MAP_TOMB || keys[i] == e) {
            keys[i] = e;
            vals[i] = v;
            return;
        }
    }
}

static void map_rehash(AnimSystem *sys, uint32_t cap)
{
    uint32_t *keys = calloc(cap, sizeof *keys);
    uint32_t *vals = calloc(cap, sizeof *vals);
    for (uint32_t i = 0; i < sys->elem_count; i++)
        map_put_raw(keys, vals, cap, sys->elems[i]->elem, i);
    free(sys->map_keys);
    free(sys->map_vals);
    sys->map_keys = keys;
    sys->map_vals = vals;
    sys->map_cap = cap;
    sys->map_used = sys->elem_count;
}

static void map_set(AnimSystem *sys, HtmlElem e, uint32_t v)
{
    if ((sys->map_used + 1) * 2 > sys->map_cap)
        map_rehash(sys, sys->map_cap ? sys->map_cap * 2 : 64);
    uint32_t mask = sys->map_cap - 1;
    for (uint32_t i = map_hash(e) & mask;; i = (i + 1) & mask) {
        uint32_t k = sys->map_keys[i];
        if (k == e) {
            sys->map_vals[i] = v;
            return;
        }
        if (k == MAP_EMPTY || k == MAP_TOMB) {
            if (k == MAP_EMPTY)
                sys->map_used++;
            sys->map_keys[i] = e;
            sys->map_vals[i] = v;
            return;
        }
    }
}

static void map_del(AnimSystem *sys, HtmlElem e)
{
    if (sys->map_cap == 0)
        return;
    uint32_t mask = sys->map_cap - 1;
    for (uint32_t i = map_hash(e) & mask;; i = (i + 1) & mask) {
        uint32_t k = sys->map_keys[i];
        if (k == MAP_EMPTY)
            return;
        if (k == e) {
            sys->map_keys[i] = MAP_TOMB;
            return;
        }
    }
}

static AnimElem *elem_find(const AnimSystem *sys, HtmlElem e)
{
    uint32_t idx = map_find(sys, e);
    return idx == MAP_TOMB ? NULL : sys->elems[idx];
}

static AnimElem *elem_create(AnimSystem *sys, HtmlElem e)
{
    AnimElem *rec = calloc(1, sizeof *rec);
    rec->elem = e;
    if (sys->elem_count == sys->elem_cap) {
        uint32_t cap = sys->elem_cap ? sys->elem_cap * 2 : 32;
        sys->elems = realloc(sys->elems, cap * sizeof *sys->elems);
        sys->elem_cap = cap;
    }
    sys->elems[sys->elem_count] = rec;
    map_set(sys, e, sys->elem_count);
    sys->elem_count++;
    return rec;
}

static void elem_destroy(AnimSystem *sys, uint32_t idx)
{
    AnimElem *rec = sys->elems[idx];
    map_del(sys, rec->elem);
    uint32_t last = sys->elem_count - 1;
    if (idx != last) {
        sys->elems[idx] = sys->elems[last];
        map_set(sys, sys->elems[idx]->elem, idx);
    }
    sys->elem_count--;
    free(rec->trans);
    free(rec);
}

/* ------------------------------------------------------------------------- */
/* Animation sampling                                                         */
/* ------------------------------------------------------------------------- */

static double running_start_ms(const AnimRunning *r) { return r->created_ms + sec_to_ms(r->delay_s); }

static double running_local_s(const AnimRunning *r, double now_ms)
{
    if (r->play_state == CSS_ANIM_PLAY_PAUSED)
        return r->pause_local_s;
    return (now_ms - running_start_ms(r)) / 1000.0;
}

static double running_active_s(const AnimRunning *r)
{
    if (isinf(r->iterations))
        return INFINITY;
    double it = r->iterations > 0.0f ? r->iterations : 0.0;
    return (double)r->duration_s * it;
}

/* Fills *phase and the directed iteration progress; returns 1 when the
 * animation currently affects the element. */
static int running_sample(const AnimRunning *r, double now_ms, int *phase_out, float *progress_out)
{
    double local = running_local_s(r, now_ms);
    double active = running_active_s(r);
    int phase;
    if (local < 0.0)
        phase = PHASE_BEFORE;
    else if (r->duration_s <= 0.0f || local >= active)
        phase = PHASE_AFTER;
    else
        phase = PHASE_ACTIVE;
    *phase_out = phase;

    double overall;
    if (phase == PHASE_BEFORE) {
        if (r->fill != CSS_ANIM_FILL_BACKWARDS && r->fill != CSS_ANIM_FILL_BOTH)
            return 0;
        overall = 0.0;
    } else if (phase == PHASE_AFTER) {
        if (r->fill != CSS_ANIM_FILL_FORWARDS && r->fill != CSS_ANIM_FILL_BOTH)
            return 0;
        overall = isinf(r->iterations) ? 0.0 : (r->iterations > 0.0f ? r->iterations : 0.0);
    } else {
        overall = local / (double)r->duration_s;
    }

    double index = floor(overall);
    double p = overall - index;
    if (phase == PHASE_AFTER && p == 0.0 && overall > 0.0) {
        index -= 1.0;
        p = 1.0;
    }
    int forward;
    switch (r->direction) {
    case CSS_ANIM_DIRECTION_REVERSE:           forward = 0; break;
    case CSS_ANIM_DIRECTION_ALTERNATE:         forward = fmod(index, 2.0) == 0.0; break;
    case CSS_ANIM_DIRECTION_ALTERNATE_REVERSE: forward = fmod(index, 2.0) != 0.0; break;
    default:                                   forward = 1; break;
    }
    *progress_out = (float)(forward ? p : 1.0 - p);
    return 1;
}

/* steps() segment the animation is in (single-interval sets only), -1 when
 * the output changes continuously. */
static int32_t running_step_index(const AnimRunning *r, const AnimKeyframeSet *set, double now_ms)
{
    if (r->timing.kind != CSS_TIMING_STEPS || r->duration_s <= 0.0f || !set_single_interval(set))
        return -1;
    int n = r->timing.steps < 1 ? 1 : r->timing.steps;
    double local = running_local_s(r, now_ms);
    double seg = (double)r->duration_s / n;
    return (int32_t)floor(local / seg + 1e-9);
}

/* ------------------------------------------------------------------------- */
/* Transitions                                                                */
/* ------------------------------------------------------------------------- */

static float transition_progress(const AnimTransition *t, double now_ms)
{
    if (now_ms <= t->start_ms) return 0.0f;
    if (now_ms >= t->end_ms) return 1.0f;
    return (float)((now_ms - t->start_ms) / (t->end_ms - t->start_ms));
}

static void transition_value(const AnimTransition *t, double now_ms, AnimValue *out)
{
    float x = transition_progress(t, now_ms);
    if (x <= 0.0f) { *out = t->from; return; }
    if (x >= 1.0f) { *out = t->to; return; }
    anim_value_lerp((AnimProp)t->prop, &t->from, &t->to, anim_timing_eval(&t->timing, x), out);
}

static AnimTransition *transition_find(AnimElem *rec, AnimProp prop)
{
    for (uint32_t i = 0; i < rec->trans_count; i++)
        if (rec->trans[i].prop == prop)
            return &rec->trans[i];
    return NULL;
}

static void transition_remove(AnimElem *rec, uint32_t i)
{
    rec->trans[i] = rec->trans[rec->trans_count - 1];
    rec->trans_count--;
}

static AnimTransition *transition_add(AnimElem *rec)
{
    if (rec->trans_count == rec->trans_cap) {
        uint32_t cap = rec->trans_cap ? rec->trans_cap * 2 : 2;
        rec->trans = realloc(rec->trans, cap * sizeof *rec->trans);
        rec->trans_cap = cap;
    }
    AnimTransition *t = &rec->trans[rec->trans_count++];
    memset(t, 0, sizeof *t);
    return t;
}

/* Last transition-property entry covering prop with a positive combined
 * duration (CSS Transitions 1 section 3: the last matching one wins). */
static const CssTransition *transition_decl_for(const CssComputed *base, AnimProp prop)
{
    const CssTransition *match = NULL;
    for (int i = 0; i < base->transition_count && i < CSS_MAX_TRANSITIONS; i++) {
        const CssTransition *t = &base->transitions[i];
        if (t->prop < 0)
            continue;
        if (anim_prop_mask_from_css_prop(t->prop) & ANIM_PROP_BIT(prop))
            match = t;
    }
    return match;
}

static int has_transition_decl(const CssComputed *base)
{
    for (int i = 0; i < base->transition_count && i < CSS_MAX_TRANSITIONS; i++) {
        const CssTransition *t = &base->transitions[i];
        if (t->prop >= 0 && t->duration_s + t->delay_s > 0.0f && anim_prop_mask_from_css_prop(t->prop))
            return 1;
    }
    return 0;
}

static void transitions_update(AnimElem *rec, const CssComputed *base, double now_ms)
{
    for (int p = 0; p < ANIM_PROP_COUNT; p++) {
        AnimValue before, after;
        snapshot_get(&rec->prev, (AnimProp)p, &before);
        anim_value_get(base, (AnimProp)p, &after);
        AnimTransition *run = transition_find(rec, (AnimProp)p);
        const CssTransition *decl = transition_decl_for(base, (AnimProp)p);
        int matches = decl && decl->duration_s + decl->delay_s > 0.0f;

        if (!run) {
            if (!matches || anim_value_equal((AnimProp)p, &before, &after) ||
                !anim_value_interpolable((AnimProp)p, &before, &after))
                continue;
            AnimTransition *t = transition_add(rec);
            t->prop = (uint8_t)p;
            t->start_ms = now_ms + sec_to_ms(decl->delay_s);
            t->end_ms = t->start_ms + sec_to_ms(decl->duration_s);
            t->timing = decl->timing;
            t->from = before;
            t->to = after;
            t->reversing_start = before;
            t->reversing_factor = 1.0f;
            continue;
        }

        if (anim_value_equal((AnimProp)p, &run->to, &after))
            continue;   /* same target: keep running */

        AnimValue current;
        transition_value(run, now_ms, &current);
        uint32_t run_idx = (uint32_t)(run - rec->trans);
        if (!matches || anim_value_equal((AnimProp)p, &current, &after) ||
            !anim_value_interpolable((AnimProp)p, &current, &after)) {
            transition_remove(rec, run_idx);
            continue;
        }

        AnimTransition nt;
        memset(&nt, 0, sizeof nt);
        nt.prop = (uint8_t)p;
        nt.timing = decl->timing;
        nt.from = current;
        nt.to = after;
        if (anim_value_equal((AnimProp)p, &run->reversing_start, &after)) {
            /* Reversal: shorten by the progress already made. */
            float eased = anim_timing_eval(&run->timing, transition_progress(run, now_ms));
            float factor = fabsf(eased * run->reversing_factor + (1.0f - run->reversing_factor));
            if (factor > 1.0f) factor = 1.0f;
            nt.reversing_start = run->to;
            nt.reversing_factor = factor;
            double delay = decl->delay_s < 0.0f ? (double)decl->delay_s * factor : (double)decl->delay_s;
            nt.start_ms = now_ms + sec_to_ms(delay);
            nt.end_ms = nt.start_ms + sec_to_ms((double)decl->duration_s * factor);
        } else {
            nt.reversing_start = current;
            nt.reversing_factor = 1.0f;
            nt.start_ms = now_ms + sec_to_ms(decl->delay_s);
            nt.end_ms = nt.start_ms + sec_to_ms(decl->duration_s);
        }
        rec->trans[run_idx] = nt;
    }
}

/* ------------------------------------------------------------------------- */
/* System                                                                     */
/* ------------------------------------------------------------------------- */

void anim_init(AnimSystem *sys, AnimDirtyFn dirty_fn, void *dirty_user)
{
    props_init();
    memset(sys, 0, sizeof *sys);
    sys->dirty_fn = dirty_fn;
    sys->dirty_user = dirty_user;
}

void anim_free(AnimSystem *sys)
{
    anim_keyframes_clear(sys);
    for (uint32_t i = 0; i < sys->elem_count; i++) {
        free(sys->elems[i]->trans);
        free(sys->elems[i]);
    }
    free(sys->elems);
    free(sys->map_keys);
    free(sys->map_vals);
    memset(sys, 0, sizeof *sys);
}

static void running_init(AnimRunning *r, const CssAnimation *a, double now_ms)
{
    memset(r, 0, sizeof *r);
    r->name = a->name;
    r->created_ms = now_ms;
    r->last_phase = PHASE_NONE;
    r->last_step = -1;
}

static void running_sync(AnimRunning *r, const CssAnimation *a, double now_ms)
{
    if (a->play_state == CSS_ANIM_PLAY_PAUSED && r->play_state != CSS_ANIM_PLAY_PAUSED) {
        r->pause_local_s = running_local_s(r, now_ms);
    } else if (a->play_state != CSS_ANIM_PLAY_PAUSED && r->play_state == CSS_ANIM_PLAY_PAUSED) {
        /* resume: shift so local time continues from the paused value */
        r->created_ms = now_ms - r->pause_local_s * 1000.0 - sec_to_ms(a->delay_s);
    }
    r->play_state = a->play_state;
    r->delay_s = a->delay_s;
    r->duration_s = a->duration_s;
    r->iterations = a->iterations;
    r->timing = a->timing;
    r->direction = a->direction;
    r->fill = a->fill;
}

int anim_update_elem(AnimSystem *sys, HtmlElem elem, const CssComputed *base, double now_ms)
{
    props_init();
    int wants_anim = 0;
    for (int i = 0; i < base->animation_count && i < CSS_MAX_ANIMATIONS; i++)
        if (base->animations[i].name)
            wants_anim = 1;
    int wants_trans = has_transition_decl(base);

    AnimElem *rec = elem_find(sys, elem);
    if (!rec) {
        if (!wants_anim && !wants_trans)
            return 0;
        rec = elem_create(sys, elem);
    }

    /* ---- animations: match slots by position + name --------------------- */
    uint8_t n = 0;
    for (int i = 0; i < base->animation_count && i < CSS_MAX_ANIMATIONS; i++) {
        const CssAnimation *a = &base->animations[i];
        if (!a->name)
            continue;
        AnimRunning *slot = &rec->anims[n];
        if (n >= rec->anim_count || slot->name != a->name)
            running_init(slot, a, now_ms);
        running_sync(slot, a, now_ms);
        n++;
    }
    rec->anim_count = n;

    /* ---- transitions ---------------------------------------------------- */
    if (rec->has_prev)
        transitions_update(rec, base, now_ms);
    if (wants_trans) {
        snapshot_from_computed(&rec->prev, base);
        rec->has_prev = 1;
    } else {
        rec->has_prev = 0;
    }

    if (rec->anim_count == 0 && rec->trans_count == 0 && !wants_trans) {
        elem_destroy(sys, map_find(sys, elem));
        return 0;
    }
    if (rec->trans_count > 0)
        return 1;
    for (uint8_t i = 0; i < rec->anim_count; i++) {
        const AnimKeyframeSet *set = anim_keyframes_find(sys, rec->anims[i].name);
        if (set && set->props)
            return 1;
    }
    return 0;   /* only unknown names (sprayAnm): nothing to apply */
}

const CssComputed *anim_apply(AnimSystem *sys, HtmlElem elem, const CssComputed *base,
                              CssComputed *out, double now_ms)
{
    AnimElem *rec = elem_find(sys, elem);
    if (!rec || (rec->anim_count == 0 && rec->trans_count == 0))
        return base;

    int changed = 0;
    for (uint8_t i = 0; i < rec->anim_count; i++) {
        const AnimRunning *r = &rec->anims[i];
        const AnimKeyframeSet *set = anim_keyframes_find(sys, r->name);
        if (!set || set->props == 0)
            continue;
        int phase;
        float p;
        if (!running_sample(r, now_ms, &phase, &p))
            continue;
        for (int prop = 0; prop < ANIM_PROP_COUNT; prop++) {
            if (!(set->props & ANIM_PROP_BIT(prop)))
                continue;
            AnimValue bv, v;
            anim_value_get(base, (AnimProp)prop, &bv);
            if (!set_sample_prop(set, (AnimProp)prop, p, &r->timing, &bv, &v))
                continue;
            if (!changed) {
                *out = *base;
                changed = 1;
            }
            anim_value_set(out, (AnimProp)prop, &v);
        }
    }
    for (uint32_t i = 0; i < rec->trans_count; i++) {
        AnimValue v;
        transition_value(&rec->trans[i], now_ms, &v);
        if (!changed) {
            *out = *base;
            changed = 1;
        }
        anim_value_set(out, (AnimProp)rec->trans[i].prop, &v);
    }
    return changed ? out : base;
}

void anim_tick(AnimSystem *sys, double now_ms)
{
    for (uint32_t ei = 0; ei < sys->elem_count; ei++) {
        AnimElem *rec = sys->elems[ei];
        unsigned flags = 0;

        for (uint8_t i = 0; i < rec->anim_count; i++) {
            AnimRunning *r = &rec->anims[i];
            const AnimKeyframeSet *set = anim_keyframes_find(sys, r->name);
            if (!set || set->props == 0)
                continue;
            int phase;
            float p;
            running_sample(r, now_ms, &phase, &p);
            int32_t step = running_step_index(r, set, now_ms);
            int dirty = 0;
            if (phase != r->last_phase) {
                dirty = 1;
            } else if (phase == PHASE_ACTIVE && r->play_state != CSS_ANIM_PLAY_PAUSED) {
                dirty = step < 0 || step != r->last_step;
            }
            r->last_phase = (uint8_t)phase;
            r->last_step = step;
            if (!dirty)
                continue;
            for (int prop = 0; prop < ANIM_PROP_COUNT; prop++)
                if (set->props & ANIM_PROP_BIT(prop))
                    flags |= PROPS[prop].dirty;
        }

        for (uint32_t i = 0; i < rec->trans_count;) {
            AnimTransition *t = &rec->trans[i];
            if (now_ms >= t->end_ms) {
                flags |= PROPS[t->prop].dirty;
                transition_remove(rec, i);
                continue;
            }
            if (now_ms >= t->start_ms)
                flags |= PROPS[t->prop].dirty;
            i++;
        }

        if (flags && sys->dirty_fn)
            sys->dirty_fn(sys->dirty_user, rec->elem, flags);
    }
}

double anim_next_wakeup(const AnimSystem *sys, double now_ms)
{
    double best = INFINITY;
    for (uint32_t ei = 0; ei < sys->elem_count; ei++) {
        const AnimElem *rec = sys->elems[ei];
        for (uint8_t i = 0; i < rec->anim_count; i++) {
            const AnimRunning *r = &rec->anims[i];
            const AnimKeyframeSet *set = anim_keyframes_find(sys, r->name);
            if (!set || set->props == 0 || r->play_state == CSS_ANIM_PLAY_PAUSED)
                continue;
            double start = running_start_ms(r);
            if (now_ms < start) {
                if (start < best) best = start;
                continue;
            }
            double active_end = start + sec_to_ms(running_active_s(r));
            if (now_ms >= active_end) {
                /* just ended and not yet reported by anim_tick */
                if (r->last_phase != PHASE_AFTER && r->last_phase != PHASE_NONE)
                    return now_ms;
                continue;
            }
            int32_t step = running_step_index(r, set, now_ms);
            if (step < 0)
                return now_ms;
            double seg = sec_to_ms(r->duration_s) / (r->timing.steps < 1 ? 1 : r->timing.steps);
            double next = start + (double)(step + 1) * seg;
            if (next > active_end) next = active_end;
            if (next < best) best = next;
        }
        for (uint32_t i = 0; i < rec->trans_count; i++) {
            const AnimTransition *t = &rec->trans[i];
            if (now_ms < t->start_ms) {
                if (t->start_ms < best) best = t->start_ms;
            } else {
                return now_ms;   /* running, or finished and waiting for anim_tick */
            }
        }
    }
    return best;
}

int anim_has_active(const AnimSystem *sys, double now_ms)
{
    return !isinf(anim_next_wakeup(sys, now_ms));
}

void anim_remove_elem(AnimSystem *sys, HtmlElem elem)
{
    uint32_t idx = map_find(sys, elem);
    if (idx != MAP_TOMB)
        elem_destroy(sys, idx);
}

void anim_stats(const AnimSystem *sys, uint32_t *elems, uint32_t *animations, uint32_t *transitions)
{
    uint32_t a = 0, t = 0;
    for (uint32_t i = 0; i < sys->elem_count; i++) {
        a += sys->elems[i]->anim_count;
        t += sys->elems[i]->trans_count;
    }
    if (elems) *elems = sys->elem_count;
    if (animations) *animations = a;
    if (transitions) *transitions = t;
}
