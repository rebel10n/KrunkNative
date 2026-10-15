/*
 * css_sheet.c - stylesheet loading on top of Lexbor's CSS parser.  See
 * css_sheet.h.
 *
 * Lexbor facts this relies on (verified against the vendored commit):
 *   - a rule whose selector list contains a pseudo-element (or :visited)
 *     comes back as LXB_CSS_RULE_BAD_STYLE with the raw selector text and a
 *     parsed declaration list;
 *   - every declaration keeps byte offsets of its name and value into the
 *     source text (lxb_css_rule_declaration_t.offset), so the raw text is
 *     available for known, unknown (CUSTOM) and unparsable (UNDEF) ones;
 *   - @keyframes is a CUSTOM at-rule whose block is parsed as rules
 *     (`from` -> STYLE, `50%` -> BAD_STYLE), @font-face is FONT_FACE with a
 *     DECLARATION_LIST block;
 *   - :placeholder-shown matches "has a placeholder attribute", so it is
 *     rewritten to the attribute selector [kn-placeholder-shown] that dom.c
 *     maintains;
 *   - selector matching treats ids/classes case-insensitively, so index keys
 *     are lower-cased on both sides.
 */
#include "css_sheet.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

typedef struct U32Vec { uint32_t *data; size_t len, cap; } U32Vec;
typedef struct DeclVec { CssDecl *data; size_t len, cap; } DeclVec;

typedef struct Bucket {
    char *key;              /* kind byte + lower-cased name                  */
    U32Vec sels;
} Bucket;

struct CssSheet {
    char *name;
    char *source;
    size_t source_len;

    lxb_css_memory_t *memory;
    lxb_css_stylesheet_t *lxb;
    lxb_css_parser_t *parser;       /* owns the re-parsed selector lists     */

    struct { CssRule *data; size_t len, cap; } rules;
    struct { CssSelector *data; size_t len, cap; } selectors;
    struct { CssKeyframes *data; size_t len, cap; } keyframes;
    struct { CssFontFace *data; size_t len, cap; } fonts;
    struct { CssPropertyRule *data; size_t len, cap; } properties;   /* @property */

    DomInvalidation *inv;
    uint32_t serial;                /* unique per css_sheet_load()           */

    Bucket *buckets;
    size_t bucket_cap;
    size_t bucket_count;
    U32Vec universal;
};

/* ------------------------------------------------------------------------- */
/* Helpers                                                                    */
/* ------------------------------------------------------------------------- */

static const char *src_slice(const CssSheet *sheet, size_t begin, size_t end, size_t *len)
{
    if (begin > sheet->source_len || end > sheet->source_len || end < begin) {
        *len = 0;
        return "";
    }
    *len = end - begin;
    return sheet->source + begin;
}

/* Splits on top-level commas (outside (), [], quotes).  Calls fn per part. */
typedef void (*PartFn)(CssSheet *sheet, const char *s, size_t len, void *ctx);

static void split_commas(CssSheet *sheet, const char *s, size_t len, PartFn fn, void *ctx)
{
    size_t start = 0;
    int depth = 0;
    char quote = 0;
    for (size_t i = 0; i <= len; i++) {
        char c = i < len ? s[i] : ',';
        if (quote != 0) {
            if (c == '\\' && i + 1 < len) {
                i++;
            } else if (c == quote) {
                quote = 0;
            }
            continue;
        }
        if (c == '"' || c == '\'') {
            quote = c;
        } else if (c == '(' || c == '[') {
            depth++;
        } else if (c == ')' || c == ']') {
            if (depth > 0) {
                depth--;
            }
        } else if (c == ',' && depth == 0) {
            const char *p = s + start;
            size_t plen = i - start;
            p = kn_str_trim(p, &plen);
            if (plen > 0) {
                fn(sheet, p, plen, ctx);
            }
            start = i + 1;
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Declarations                                                               */
/* ------------------------------------------------------------------------- */

static void decl_cb(void *user, int prop_id, const CssToken *tokens, int n, int important)
{
    DeclVec *v = (DeclVec *)user;
    CssDecl d;
    if (n < 0 || n > 65535) {
        return;
    }
    d.prop = prop_id;
    d.important = (uint8_t)(important != 0);
    d.ntok = (uint16_t)n;
    d.has_var = (uint8_t)(css_tokens_have_var(tokens, n) != 0);
    d.tokens = (CssToken *)malloc((size_t)(n > 0 ? n : 1) * sizeof(CssToken));
    if (d.tokens == NULL) {
        return;
    }
    memcpy(d.tokens, tokens, (size_t)n * sizeof(CssToken));
    KN_VEC_PUSH(v, d);
}

/* Appends the declarations of a Lexbor declaration list to `out`, re-parsed
 * from the raw source text (so names like `COLOR` are handled and shorthands
 * are expanded). */
static void collect_decls(CssSheet *sheet, lxb_css_rule_declaration_list_t *list, DeclVec *out)
{
    lxb_css_rule_t *r;
    if (list == NULL) {
        return;
    }
    for (r = list->first; r != NULL; r = r->next) {
        lxb_css_rule_declaration_t *d;
        const char *name;
        const char *value;
        size_t nlen, vlen;
        if (r->type != LXB_CSS_RULE_DECLARATION) {
            continue;
        }
        d = lxb_css_rule_declaration(r);
        name = src_slice(sheet, d->offset.name_begin, d->offset.name_end, &nlen);
        value = src_slice(sheet, d->offset.value_begin, d->offset.value_end, &vlen);
        name = kn_str_trim(name, &nlen);
        value = kn_str_trim(value, &vlen);
        if (nlen == 0) {
            continue;
        }
        css_parse_declaration(name, nlen, value, vlen, d->important, decl_cb, out);
    }
}

static void decls_free(CssDecl *decls, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        free(decls[i].tokens);
    }
    free(decls);
}

/* ------------------------------------------------------------------------- */
/* Selector text filtering                                                    */
/* ------------------------------------------------------------------------- */

static const struct {
    const char *name;
    int pe;
} k_pseudo_elements[] = {
    { "before", CSS_PE_BEFORE },
    { "after", CSS_PE_AFTER },
    { "placeholder", CSS_PE_PLACEHOLDER },
    { "-webkit-input-placeholder", CSS_PE_PLACEHOLDER },
    { "-webkit-scrollbar", CSS_PE_SCROLLBAR },
    { "-webkit-scrollbar-track", CSS_PE_SCROLLBAR_TRACK },
    { "-webkit-scrollbar-thumb", CSS_PE_SCROLLBAR_THUMB },
    { "-webkit-slider-thumb", CSS_PE_SLIDER_THUMB },
    { "-webkit-slider-runnable-track", CSS_PE_SLIDER_TRACK },
    { "-webkit-inner-spin-button", CSS_PE_INNER_SPIN_BUTTON },
    { "-webkit-outer-spin-button", CSS_PE_OUTER_SPIN_BUTTON },
    { "-webkit-color-swatch", CSS_PE_COLOR_SWATCH },
    { "-webkit-color-swatch-wrapper", CSS_PE_COLOR_SWATCH_WRAPPER },
    { "selection", CSS_PE_SELECTION },
};

static int pseudo_element_kind(const char *name, size_t len)
{
    for (size_t i = 0; i < sizeof(k_pseudo_elements) / sizeof(k_pseudo_elements[0]); i++) {
        if (kn_str_ieq(name, len, k_pseudo_elements[i].name)) {
            return k_pseudo_elements[i].pe;
        }
    }
    return -1;
}

static int is_ident_char(int c)
{
    return kn_is_alpha(c) || kn_is_digit(c) || c == '-' || c == '_' || c < 0;
}

int css_sheet_strip_pseudo_element(const char *sel, size_t len, char *out, size_t cap, size_t *out_len)
{
    int pe = CSS_PE_NONE;
    size_t i = 0;
    size_t o = 0;
    int depth = 0;
    char quote = 0;

    while (i < len) {
        char c = sel[i];
        if (quote != 0) {
            if (o + 1 >= cap) {
                return -1;
            }
            out[o++] = c;
            if (c == '\\' && i + 1 < len) {
                out[o++] = sel[++i];
            } else if (c == quote) {
                quote = 0;
            }
            i++;
            continue;
        }
        if (c == '"' || c == '\'') {
            quote = c;
        } else if (c == '(' || c == '[') {
            depth++;
        } else if (c == ')' || c == ']') {
            if (depth > 0) {
                depth--;
            }
        } else if (c == ':' && depth == 0) {
            size_t j = i + 1;
            int two = 0;
            size_t ns, ne;
            int kind;
            if (j < len && sel[j] == ':') {
                two = 1;
                j++;
            }
            ns = j;
            while (j < len && is_ident_char((unsigned char)sel[j])) {
                j++;
            }
            ne = j;
            kind = pseudo_element_kind(sel + ns, ne - ns);
            /* legacy single-colon forms only for before/after */
            if (kind >= 0 && (two || kind == CSS_PE_BEFORE || kind == CSS_PE_AFTER)) {
                if (pe != CSS_PE_NONE) {
                    return -1; /* two pseudo-elements: never matches */
                }
                pe = kind;
                i = ne;
                continue;
            }
            if (two) {
                return -1; /* ::-moz-range-thumb, ::-moz-selection, ... */
            }
            /* a pseudo-class: copy through */
        }
        if (o + 1 >= cap) {
            return -1;
        }
        out[o++] = c;
        i++;
    }
    out[o] = '\0';
    /* a pseudo-element alone (`::before {}`) means `*::before` */
    {
        size_t t = o;
        const char *p = kn_str_trim(out, &t);
        if (t == 0 && pe != CSS_PE_NONE) {
            if (cap < 2) {
                return -1;
            }
            out[0] = '*';
            out[1] = '\0';
            o = 1;
        } else if (pe != CSS_PE_NONE && p + t < out + o && (p[t] == ' ' || p[t] == '\t' || p[t] == '\n')) {
            /* `.logs ::selection`: a descendant combinator before the pseudo-element
             * means `.logs *::selection` */
            if (t + 3 > cap) {
                return -1;
            }
            memmove(out, p, t);
            out[t] = ' ';
            out[t + 1] = '*';
            out[t + 2] = '\0';
            o = t + 2;
        } else if (p != out || t != o) {
            memmove(out, p, t);
            out[t] = '\0';
            o = t;
        }
    }
    *out_len = o;
    return pe;
}

/* Case-insensitive search for a pseudo-class name (":name") at paren depth
 * >= 0 anywhere in the text. */
static int contains_pseudo(const char *s, size_t len, const char *name)
{
    size_t nlen = strlen(name);
    for (size_t i = 0; i + nlen + 1 <= len; i++) {
        if (s[i] == ':' && kn_str_ieq(s + i + 1, nlen, name)
            && (i + nlen + 1 == len || !is_ident_char((unsigned char)s[i + nlen + 1]))) {
            return 1;
        }
    }
    return 0;
}

/* Replaces every ":placeholder-shown" with "[kn-placeholder-shown]". */
static size_t rewrite_placeholder_shown(const char *s, size_t len, char *out, size_t cap)
{
    static const char from[] = ":placeholder-shown";
    static const char to[] = "[kn-placeholder-shown]";
    size_t flen = sizeof(from) - 1;
    size_t tlen = sizeof(to) - 1;
    size_t o = 0;
    for (size_t i = 0; i < len;) {
        if (i + flen <= len && kn_str_ieq(s + i, flen, from)
            && (i + flen == len || !is_ident_char((unsigned char)s[i + flen]))) {
            if (o + tlen >= cap) {
                break;
            }
            memcpy(out + o, to, tlen);
            o += tlen;
            i += flen;
        } else {
            if (o + 1 >= cap) {
                break;
            }
            out[o++] = s[i++];
        }
    }
    out[o] = '\0';
    return o;
}

/* ------------------------------------------------------------------------- */
/* Rule index                                                                 */
/* ------------------------------------------------------------------------- */

static Bucket *bucket_slot(Bucket *tab, size_t cap, const char *key, size_t klen)
{
    size_t mask = cap - 1;
    size_t i = (size_t)kn_hash_bytes(KN_FNV64_INIT, key, klen) & mask;
    for (;;) {
        Bucket *b = &tab[i];
        if (b->key == NULL || (strlen(b->key) == klen && memcmp(b->key, key, klen) == 0)) {
            return b;
        }
        i = (i + 1) & mask;
    }
}

static Bucket *bucket_get(CssSheet *sheet, int kind, const char *name, size_t nlen, int create)
{
    char key[256];
    size_t klen;
    Bucket *b;
    if (nlen + 2 > sizeof(key)) {
        nlen = sizeof(key) - 2;
    }
    key[0] = (char)kind;
    for (size_t i = 0; i < nlen; i++) {
        key[1 + i] = (char)kn_to_lower((unsigned char)name[i]);
    }
    klen = nlen + 1;
    key[klen] = '\0';
    if (sheet->bucket_cap == 0) {
        if (!create) {
            return NULL;
        }
        sheet->bucket_cap = 512;
        sheet->buckets = (Bucket *)calloc(sheet->bucket_cap, sizeof(Bucket));
    }
    if (create && sheet->bucket_count * 2 >= sheet->bucket_cap) {
        size_t ncap = sheet->bucket_cap * 2;
        Bucket *nt = (Bucket *)calloc(ncap, sizeof(Bucket));
        for (size_t i = 0; i < sheet->bucket_cap; i++) {
            if (sheet->buckets[i].key != NULL) {
                *bucket_slot(nt, ncap, sheet->buckets[i].key, strlen(sheet->buckets[i].key)) = sheet->buckets[i];
            }
        }
        free(sheet->buckets);
        sheet->buckets = nt;
        sheet->bucket_cap = ncap;
    }
    b = bucket_slot(sheet->buckets, sheet->bucket_cap, key, klen);
    if (b->key == NULL) {
        if (!create) {
            return NULL;
        }
        b->key = (char *)malloc(klen + 1);
        memcpy(b->key, key, klen + 1);
        sheet->bucket_count++;
    }
    return b;
}

/* Picks the index key of a parsed complex selector from its rightmost
 * compound: id > class > tag > universal. */
static void index_selector(CssSheet *sheet, CssSelector *cs, uint32_t index)
{
    lxb_css_selector_t *s;
    const lxb_css_selector_t *best = NULL;
    int best_rank = 0;

    for (s = cs->list->last; s != NULL; s = s->prev) {
        int rank = 0;
        if (s->type == LXB_CSS_SELECTOR_TYPE_ID) {
            rank = 3;
        } else if (s->type == LXB_CSS_SELECTOR_TYPE_CLASS) {
            rank = 2;
        } else if (s->type == LXB_CSS_SELECTOR_TYPE_ELEMENT) {
            rank = 1;
        }
        if (rank > best_rank) {
            best_rank = rank;
            best = s;
        }
        if (s->combinator != LXB_CSS_SELECTOR_COMBINATOR_CLOSE) {
            break; /* `s` is the first simple selector of the compound */
        }
    }
    if (best == NULL) {
        cs->key_kind = CSS_KEY_UNIVERSAL;
        cs->key = NULL;
        KN_VEC_PUSH(&sheet->universal, index);
        return;
    }
    cs->key_kind = best_rank == 3 ? CSS_KEY_ID : best_rank == 2 ? CSS_KEY_CLASS : CSS_KEY_TAG;
    cs->key = kn_atom_lower((const char *)best->name.data, best->name.length);
    KN_VEC_PUSH(&bucket_get(sheet, cs->key_kind, cs->key, strlen(cs->key), 1)->sels, index);
}

/* ------------------------------------------------------------------------- */
/* Invalidation features                                                      */
/* ------------------------------------------------------------------------- */

static uint32_t state_of_pseudo(unsigned type)
{
    switch (type) {
    case LXB_CSS_SELECTOR_PSEUDO_CLASS_HOVER: return DOM_PSEUDO_HOVER;
    case LXB_CSS_SELECTOR_PSEUDO_CLASS_ACTIVE: return DOM_PSEUDO_ACTIVE;
    case LXB_CSS_SELECTOR_PSEUDO_CLASS_FOCUS:
    case LXB_CSS_SELECTOR_PSEUDO_CLASS_FOCUS_VISIBLE: return DOM_PSEUDO_FOCUS;
    case LXB_CSS_SELECTOR_PSEUDO_CLASS_CHECKED: return DOM_PSEUDO_CHECKED;
    case LXB_CSS_SELECTOR_PSEUDO_CLASS_DISABLED:
    case LXB_CSS_SELECTOR_PSEUDO_CLASS_ENABLED: return DOM_PSEUDO_DISABLED;
    default: return 0;
    }
}

static unsigned structural_of_pseudo(unsigned type)
{
    switch (type) {
    case LXB_CSS_SELECTOR_PSEUDO_CLASS_FIRST_CHILD:
    case LXB_CSS_SELECTOR_PSEUDO_CLASS_FIRST_OF_TYPE:
        return DOM_INV_G_STRUCTURAL;
    case LXB_CSS_SELECTOR_PSEUDO_CLASS_LAST_CHILD:
    case LXB_CSS_SELECTOR_PSEUDO_CLASS_LAST_OF_TYPE:
    case LXB_CSS_SELECTOR_PSEUDO_CLASS_ONLY_CHILD:
    case LXB_CSS_SELECTOR_PSEUDO_CLASS_ONLY_OF_TYPE:
    case LXB_CSS_SELECTOR_PSEUDO_CLASS_EMPTY:
    case LXB_CSS_SELECTOR_PSEUDO_CLASS_BLANK:
        return DOM_INV_G_STRUCTURAL_BACK;
    default:
        return 0;
    }
}

static int is_compound_start(const lxb_css_selector_list_t *list, const lxb_css_selector_t *s)
{
    return s == list->first || s->combinator != LXB_CSS_SELECTOR_COMBINATOR_CLOSE;
}

static void inv_list(DomInvalidation *inv, const lxb_css_selector_list_t *list, unsigned outer_flags,
                     int outer_kind, const char *outer_key, size_t outer_len);

/* Records the simple selectors of the compound [first, end) with the given
 * position flags and compound key. */
static void inv_compound(DomInvalidation *inv, const lxb_css_selector_t *first, const lxb_css_selector_t *end,
                         unsigned flags, int key_kind, const char *key, size_t key_len, int structural_done)
{
    const lxb_css_selector_t *s;
    for (s = first; s != end; s = s->next) {
        const char *nm = (const char *)s->name.data;
        size_t nl = s->name.length;
        switch (s->type) {
        case LXB_CSS_SELECTOR_TYPE_CLASS:
            dom_inv_add_class(inv, nm, nl, flags);
            break;
        case LXB_CSS_SELECTOR_TYPE_ID:
            dom_inv_add_id(inv, nm, nl, flags);
            break;
        case LXB_CSS_SELECTOR_TYPE_ATTRIBUTE:
            dom_inv_add_attr(inv, nm, nl, flags);
            if (kn_str_ieq(nm, nl, "kn-placeholder-shown")) {
                dom_inv_add_state(inv, DOM_PSEUDO_PLACEHOLDER_SHOWN, key_kind, key, key_len, flags);
            }
            break;
        case LXB_CSS_SELECTOR_TYPE_PSEUDO_CLASS: {
            uint32_t st = state_of_pseudo(s->u.pseudo.type);
            if (st != 0) {
                dom_inv_add_state(inv, st, key_kind, key, key_len, flags);
            }
            if (structural_of_pseudo(s->u.pseudo.type) != 0) {
                dom_inv_add_global(inv, structural_of_pseudo(s->u.pseudo.type));
                if (!structural_done) dom_inv_add_structural(inv, key_kind, key, key_len);
            }
            break;
        }
        case LXB_CSS_SELECTOR_TYPE_PSEUDO_CLASS_FUNCTION:
            switch (s->u.pseudo.type) {
            case LXB_CSS_SELECTOR_PSEUDO_CLASS_FUNCTION_NOT:
            case LXB_CSS_SELECTOR_PSEUDO_CLASS_FUNCTION_IS:
            case LXB_CSS_SELECTOR_PSEUDO_CLASS_FUNCTION_WHERE:
                inv_list(inv, (const lxb_css_selector_list_t *)s->u.pseudo.data, flags, key_kind, key, key_len);
                break;
            case LXB_CSS_SELECTOR_PSEUDO_CLASS_FUNCTION_HAS:
                /* descendants affect the subject: be conservative */
                inv_list(inv, (const lxb_css_selector_list_t *)s->u.pseudo.data, DOM_INV_ALL, 0, NULL, 0);
                dom_inv_add_global(inv, DOM_INV_G_STRUCTURAL | DOM_INV_G_STRUCTURAL_BACK | DOM_INV_G_SIBLING_COMBINATOR);
                dom_inv_add_structural(inv, DOM_INV_KEY_ANY, NULL, 0);
                break;
            case LXB_CSS_SELECTOR_PSEUDO_CLASS_FUNCTION_NTH_CHILD:
            case LXB_CSS_SELECTOR_PSEUDO_CLASS_FUNCTION_NTH_OF_TYPE: {
                const lxb_css_selector_anb_of_t *anb = (const lxb_css_selector_anb_of_t *)s->u.pseudo.data;
                dom_inv_add_global(inv, DOM_INV_G_STRUCTURAL);
                if (!structural_done) dom_inv_add_structural(inv, key_kind, key, key_len);
                if (anb != NULL && anb->of != NULL) {
                    inv_list(inv, anb->of, flags | DOM_INV_SIBLING | DOM_INV_SIBLING_DESC, key_kind, key, key_len);
                    dom_inv_add_structural(inv, DOM_INV_KEY_ANY, NULL, 0); /* `of S` counts other siblings */
                }
                break;
            }
            case LXB_CSS_SELECTOR_PSEUDO_CLASS_FUNCTION_NTH_LAST_CHILD:
            case LXB_CSS_SELECTOR_PSEUDO_CLASS_FUNCTION_NTH_LAST_OF_TYPE: {
                const lxb_css_selector_anb_of_t *anb = (const lxb_css_selector_anb_of_t *)s->u.pseudo.data;
                dom_inv_add_global(inv, DOM_INV_G_STRUCTURAL_BACK);
                if (!structural_done) dom_inv_add_structural(inv, key_kind, key, key_len);
                if (anb != NULL && anb->of != NULL) {
                    inv_list(inv, anb->of, DOM_INV_ALL, 0, NULL, 0);
                    dom_inv_add_structural(inv, DOM_INV_KEY_ANY, NULL, 0);
                }
                break;
            }
            default:
                break;
            }
            break;
        default:
            break;
        }
    }
}

/* Does the compound [first, end) test the element's place among its siblings (a structural
 * pseudo-class)?  Arguments of :not() / :is() are left to inv_compound (conservative). */
static int compound_structural(const lxb_css_selector_t *first, const lxb_css_selector_t *end)
{
    for (const lxb_css_selector_t *s = first; s != end; s = s->next) {
        if (s->type == LXB_CSS_SELECTOR_TYPE_PSEUDO_CLASS && structural_of_pseudo(s->u.pseudo.type) != 0) {
            return 1;
        }
        if (s->type == LXB_CSS_SELECTOR_TYPE_PSEUDO_CLASS_FUNCTION
            && (s->u.pseudo.type == LXB_CSS_SELECTOR_PSEUDO_CLASS_FUNCTION_NTH_CHILD
                || s->u.pseudo.type == LXB_CSS_SELECTOR_PSEUDO_CLASS_FUNCTION_NTH_OF_TYPE
                || s->u.pseudo.type == LXB_CSS_SELECTOR_PSEUDO_CLASS_FUNCTION_NTH_LAST_CHILD
                || s->u.pseudo.type == LXB_CSS_SELECTOR_PSEUDO_CLASS_FUNCTION_NTH_LAST_OF_TYPE)) {
            const lxb_css_selector_anb_of_t *anb = (const lxb_css_selector_anb_of_t *)s->u.pseudo.data;
            if (anb == NULL || anb->of == NULL) {
                return 1;
            }
        }
    }
    return 0;
}

/* Most specific key of the compound [first, end): id > class > tag > any. */
static void compound_key(const lxb_css_selector_t *first, const lxb_css_selector_t *end,
                         int *kind, const char **key, size_t *len)
{
    int rank = 0;
    *kind = DOM_INV_KEY_ANY;
    *key = NULL;
    *len = 0;
    for (const lxb_css_selector_t *s = first; s != end; s = s->next) {
        int r = s->type == LXB_CSS_SELECTOR_TYPE_ID ? 3 : s->type == LXB_CSS_SELECTOR_TYPE_CLASS ? 2
              : s->type == LXB_CSS_SELECTOR_TYPE_ELEMENT ? 1 : 0;
        if (r > rank) {
            rank = r;
            *kind = r == 3 ? DOM_INV_KEY_ID : r == 2 ? DOM_INV_KEY_CLASS : DOM_INV_KEY_TAG;
            *key = (const char *)s->name.data;
            *len = s->name.length;
        }
    }
}

/* One complex selector.  outer_flags == 0: top level (positions computed
 * from the combinators).  Otherwise it is the argument of a functional
 * pseudo-class: a single-compound argument inherits the outer position and
 * key; a complex argument is recorded conservatively. */
static void inv_complex(DomInvalidation *inv, const lxb_css_selector_list_t *list, unsigned outer_flags,
                        int outer_kind, const char *outer_key, size_t outer_len)
{
    const lxb_css_selector_t *s = list->first;
    int multi = 0;
    for (const lxb_css_selector_t *t = list->first; t != NULL; t = t->next) {
        if (t != list->first && t->combinator != LXB_CSS_SELECTOR_COMBINATOR_CLOSE) {
            multi = 1;
            if (t->combinator == LXB_CSS_SELECTOR_COMBINATOR_SIBLING || t->combinator == LXB_CSS_SELECTOR_COMBINATOR_FOLLOWING) {
                dom_inv_add_global(inv, DOM_INV_G_SIBLING_COMBINATOR);
                if (outer_flags != 0) {
                    dom_inv_add_structural(inv, DOM_INV_KEY_ANY, NULL, 0); /* inside :is() etc.: any element */
                }
            }
        }
    }
    if (outer_flags != 0) {
        if (!multi) {
            inv_compound(inv, list->first, NULL, outer_flags, outer_kind, outer_key, outer_len, 0);
            return;
        }
        while (s != NULL) {
            const lxb_css_selector_t *end = s->next;
            int kind;
            const char *key;
            size_t len;
            while (end != NULL && !is_compound_start(list, end)) {
                end = end->next;
            }
            compound_key(s, end, &kind, &key, &len);
            inv_compound(inv, s, end, DOM_INV_ALL, kind, key, len, 0);
            s = end;
        }
        return;
    }
    int ctx_kind = DOM_INV_KEY_ANY; /* the class / id key of the compound left of a descendant / child combinator */
    const char *ctx_key = NULL;
    size_t ctx_len = 0;
    while (s != NULL) {
        const lxb_css_selector_t *end = s->next;
        unsigned flags;
        int kind;
        const char *key;
        size_t len;
        while (end != NULL && !is_compound_start(list, end)) {
            end = end->next;
        }
        if (s != list->first && s->combinator != LXB_CSS_SELECTOR_COMBINATOR_DESCENDANT
            && s->combinator != LXB_CSS_SELECTOR_COMBINATOR_CHILD) {
            ctx_kind = DOM_INV_KEY_ANY; /* a sibling step: no ancestor context */
        }
        if (end == NULL) {
            flags = DOM_INV_SUBJECT;
        } else if (end->combinator == LXB_CSS_SELECTOR_COMBINATOR_SIBLING
                   || end->combinator == LXB_CSS_SELECTOR_COMBINATOR_FOLLOWING) {
            flags = DOM_INV_SIBLING;
            for (const lxb_css_selector_t *t = end->next; t != NULL; t = t->next) {
                if (t->combinator == LXB_CSS_SELECTOR_COMBINATOR_DESCENDANT
                    || t->combinator == LXB_CSS_SELECTOR_COMBINATOR_CHILD) {
                    flags |= DOM_INV_SIBLING_DESC;
                    break;
                }
            }
        } else {
            flags = DOM_INV_ANCESTOR;
        }
        compound_key(s, end, &kind, &key, &len);
        const int structural = compound_structural(s, end);
        inv_compound(inv, s, end, flags, kind, key, len, structural);
        if (structural) {
            /* `.lds-ring div:nth-child(1)`: only divs under a .lds-ring care where they are */
            if ((kind == DOM_INV_KEY_TAG || kind == DOM_INV_KEY_ANY) && ctx_kind != DOM_INV_KEY_ANY) {
                dom_inv_add_structural_ctx(inv, kind, key, len, ctx_kind, ctx_key, ctx_len);
            } else {
                dom_inv_add_structural(inv, kind, key, len);
            }
        }
        if (s != list->first && (s->combinator == LXB_CSS_SELECTOR_COMBINATOR_SIBLING
                                 || s->combinator == LXB_CSS_SELECTOR_COMBINATOR_FOLLOWING)) {
            dom_inv_add_structural(inv, kind, key, len); /* the right side of + / ~ */
        }
        if (kind == DOM_INV_KEY_CLASS || kind == DOM_INV_KEY_ID) {
            ctx_kind = kind;
            ctx_key = key;
            ctx_len = len;
        }
        s = end;
    }
}

static void inv_list(DomInvalidation *inv, const lxb_css_selector_list_t *list, unsigned outer_flags,
                     int outer_kind, const char *outer_key, size_t outer_len)
{
    for (; list != NULL; list = list->next) {
        inv_complex(inv, list, outer_flags, outer_kind, outer_key, outer_len);
    }
}

/* ------------------------------------------------------------------------- */
/* Rules                                                                      */
/* ------------------------------------------------------------------------- */

typedef struct SelCtx {
    uint32_t rule;
} SelCtx;

/* Specificity from the AST.  Lexbor's own value is unusable: it gives plain
 * pseudo-classes (:hover, :checked, :first-child, ...) and every
 * pseudo-class inside :not() zero weight, so `.a:hover` would tie with `.a`.
 * Rules (Selectors 4): id -> a; class, attribute, pseudo-class -> b;
 * type, pseudo-element -> c; :not/:is/:has -> most specific argument;
 * :where -> 0; :nth-child(An+B of S) -> b + most specific of S. */
typedef struct Spec { uint32_t a, b, c; } Spec;

static Spec spec_list_max(const lxb_css_selector_list_t *list);

static Spec spec_add(Spec x, Spec y)
{
    Spec r = { x.a + y.a, x.b + y.b, x.c + y.c };
    return r;
}

static int spec_less(Spec x, Spec y)
{
    if (x.a != y.a) {
        return x.a < y.a;
    }
    if (x.b != y.b) {
        return x.b < y.b;
    }
    return x.c < y.c;
}

static Spec spec_complex(const lxb_css_selector_list_t *list)
{
    Spec sp = { 0, 0, 0 };
    const lxb_css_selector_t *s;
    for (s = list->first; s != NULL; s = s->next) {
        switch (s->type) {
        case LXB_CSS_SELECTOR_TYPE_ID:
            sp.a++;
            break;
        case LXB_CSS_SELECTOR_TYPE_CLASS:
        case LXB_CSS_SELECTOR_TYPE_ATTRIBUTE:
        case LXB_CSS_SELECTOR_TYPE_PSEUDO_CLASS:
            sp.b++;
            break;
        case LXB_CSS_SELECTOR_TYPE_ELEMENT:
        case LXB_CSS_SELECTOR_TYPE_PSEUDO_ELEMENT:
        case LXB_CSS_SELECTOR_TYPE_PSEUDO_ELEMENT_FUNCTION:
            sp.c++;
            break;
        case LXB_CSS_SELECTOR_TYPE_PSEUDO_CLASS_FUNCTION:
            switch (s->u.pseudo.type) {
            case LXB_CSS_SELECTOR_PSEUDO_CLASS_FUNCTION_NOT:
            case LXB_CSS_SELECTOR_PSEUDO_CLASS_FUNCTION_IS:
            case LXB_CSS_SELECTOR_PSEUDO_CLASS_FUNCTION_HAS:
                sp = spec_add(sp, spec_list_max((const lxb_css_selector_list_t *)s->u.pseudo.data));
                break;
            case LXB_CSS_SELECTOR_PSEUDO_CLASS_FUNCTION_WHERE:
                break;
            case LXB_CSS_SELECTOR_PSEUDO_CLASS_FUNCTION_NTH_CHILD:
            case LXB_CSS_SELECTOR_PSEUDO_CLASS_FUNCTION_NTH_LAST_CHILD: {
                const lxb_css_selector_anb_of_t *anb = (const lxb_css_selector_anb_of_t *)s->u.pseudo.data;
                sp.b++;
                if (anb != NULL && anb->of != NULL) {
                    sp = spec_add(sp, spec_list_max(anb->of));
                }
                break;
            }
            default:
                sp.b++;
                break;
            }
            break;
        default: /* `*` */
            break;
        }
    }
    return sp;
}

static Spec spec_list_max(const lxb_css_selector_list_t *list)
{
    Spec best = { 0, 0, 0 };
    for (; list != NULL; list = list->next) {
        Spec sp = spec_complex(list);
        if (spec_less(best, sp)) {
            best = sp;
        }
    }
    return best;
}

static uint32_t pack_specificity(const lxb_css_selector_list_t *list, int pe)
{
    Spec sp = spec_complex(list);
    if (pe != CSS_PE_NONE) {
        sp.c++;
    }
    if (sp.a > 1023) {
        sp.a = 1023;
    }
    if (sp.b > 1023) {
        sp.b = 1023;
    }
    if (sp.c > 1023) {
        sp.c = 1023;
    }
    return (sp.a << 20) | (sp.b << 10) | sp.c;
}

static void add_selector(CssSheet *sheet, const char *text, size_t len, void *ctx)
{
    SelCtx *sc = (SelCtx *)ctx;
    char stripped[1024];
    char rewritten[1100];
    size_t slen = 0, rlen;
    int pe;
    lxb_css_selector_list_t *list;
    CssSelector cs;

    memset(&cs, 0, sizeof(cs));
    cs.rule = sc->rule;
    cs.text = kn_atom(text, len);

    if (contains_pseudo(text, len, "visited")) {
        return; /* never matches */
    }
    pe = css_sheet_strip_pseudo_element(text, len, stripped, sizeof(stripped), &slen);
    if (pe < 0) {
        return;
    }
    rlen = rewrite_placeholder_shown(stripped, slen, rewritten, sizeof(rewritten));
    list = lxb_css_selectors_parse(sheet->parser, (const lxb_char_t *)rewritten, rlen);
    if (list == NULL) {
        if (getenv("KRUNKNATIVE_HTML_VERBOSE") != NULL) {
            fprintf(stderr, "[css] %s: cannot parse selector '%.*s'\n", sheet->name, (int)len, text);
        }
        return;
    }
    if (list->next != NULL) {
        /* should not happen (we split on commas); keep only the first */
        lxb_css_selector_list_destroy_chain(list->next);
        list->next = NULL;
    }
    cs.list = list;
    cs.pe = (uint8_t)pe;
    cs.specificity = pack_specificity(list, pe);
    inv_complex(sheet->inv, list, 0, 0, NULL, 0);
    KN_VEC_PUSH(&sheet->selectors, cs);
    index_selector(sheet, &sheet->selectors.data[sheet->selectors.len - 1], (uint32_t)(sheet->selectors.len - 1));
}

static void add_rule(CssSheet *sheet, const char *prelude, size_t plen, lxb_css_rule_declaration_list_t *decls)
{
    CssRule rule;
    DeclVec dv = { 0 };
    SelCtx sc;

    collect_decls(sheet, decls, &dv);
    memset(&rule, 0, sizeof(rule));
    rule.decls = dv.data;
    rule.ndecls = (uint32_t)dv.len;
    rule.order = (uint32_t)sheet->rules.len;
    KN_VEC_PUSH(&sheet->rules, rule);
    sc.rule = rule.order;
    split_commas(sheet, prelude, plen, add_selector, &sc);
}

/* ---- @keyframes --------------------------------------------------------- */

typedef struct FrameCtx {
    CssKeyframes *kf;
    lxb_css_rule_declaration_list_t *decls;
} FrameCtx;

static void add_keyframe(CssSheet *sheet, const char *text, size_t len, void *ctx)
{
    FrameCtx *fc = (FrameCtx *)ctx;
    CssKeyframe f;
    DeclVec dv = { 0 };
    CssKeyframe *arr;

    if (kn_str_ieq(text, len, "from")) {
        f.offset = 0.0f;
    } else if (kn_str_ieq(text, len, "to")) {
        f.offset = 100.0f;
    } else if (len > 1 && text[len - 1] == '%') {
        char buf[32];
        size_t n = len - 1 < sizeof(buf) - 1 ? len - 1 : sizeof(buf) - 1;
        memcpy(buf, text, n);
        buf[n] = '\0';
        f.offset = (float)atof(buf);
    } else {
        return;
    }
    collect_decls(sheet, fc->decls, &dv);
    f.decls = dv.data;
    f.ndecls = (uint32_t)dv.len;
    arr = (CssKeyframe *)realloc(fc->kf->frames, (fc->kf->nframes + 1) * sizeof(CssKeyframe));
    if (arr == NULL) {
        decls_free(f.decls, f.ndecls);
        return;
    }
    fc->kf->frames = arr;
    /* insertion sort by offset, stable */
    {
        uint32_t i = fc->kf->nframes;
        while (i > 0 && arr[i - 1].offset > f.offset) {
            arr[i] = arr[i - 1];
            i--;
        }
        arr[i] = f;
    }
    fc->kf->nframes++;
}

static void add_keyframes(CssSheet *sheet, const char *prelude, size_t plen, lxb_css_rule_list_t *block)
{
    CssKeyframes kf;
    lxb_css_rule_t *r;

    prelude = kn_str_trim(prelude, &plen);
    if (plen == 0) {
        return;
    }
    /* quoted names */
    if (plen >= 2 && (prelude[0] == '"' || prelude[0] == '\'') && prelude[plen - 1] == prelude[0]) {
        prelude++;
        plen -= 2;
    }
    memset(&kf, 0, sizeof(kf));
    kf.name = kn_atom(prelude, plen);
    if (block != NULL) {
        for (r = block->first; r != NULL; r = r->next) {
            FrameCtx fc;
            const char *sel;
            size_t slen;
            fc.kf = &kf;
            if (r->type == LXB_CSS_RULE_STYLE) {
                lxb_css_rule_style_t *s = lxb_css_rule_style(r);
                sel = src_slice(sheet, s->prelude_begin, s->prelude_end, &slen);
                fc.decls = s->declarations;
            } else if (r->type == LXB_CSS_RULE_BAD_STYLE) {
                lxb_css_rule_bad_style_t *b = lxb_css_rule_bad_style(r);
                sel = src_slice(sheet, b->prelude_begin, b->prelude_end, &slen);
                fc.decls = b->declarations;
            } else {
                continue;
            }
            split_commas(sheet, sel, slen, add_keyframe, &fc);
        }
    }
    /* a later @keyframes with the same name replaces the earlier one */
    for (size_t i = 0; i < sheet->keyframes.len; i++) {
        if (sheet->keyframes.data[i].name == kf.name) {
            CssKeyframes *old = &sheet->keyframes.data[i];
            for (uint32_t k = 0; k < old->nframes; k++) {
                decls_free(old->frames[k].decls, old->frames[k].ndecls);
            }
            free(old->frames);
            *old = kf;
            return;
        }
    }
    KN_VEC_PUSH(&sheet->keyframes, kf);
}

/* ---- @font-face --------------------------------------------------------- */

static const char *unquote_atom(const char *s, size_t len)
{
    s = kn_str_trim(s, &len);
    if (len >= 2 && (s[0] == '"' || s[0] == '\'') && s[len - 1] == s[0]) {
        s++;
        len -= 2;
    }
    return kn_atom(s, len);
}

static void add_font_face(CssSheet *sheet, lxb_css_rule_list_t *block)
{
    CssFontFace ff;
    lxb_css_rule_t *lr;

    memset(&ff, 0, sizeof(ff));
    ff.weight = 400;
    if (block == NULL) {
        return;
    }
    for (lr = block->first; lr != NULL; lr = lr->next) {
        lxb_css_rule_declaration_list_t *list;
        lxb_css_rule_t *r;
        if (lr->type != LXB_CSS_RULE_DECLARATION_LIST) {
            continue;
        }
        list = lxb_css_rule_declaration_list(lr);
        for (r = list->first; r != NULL; r = r->next) {
            lxb_css_rule_declaration_t *d;
            const char *name;
            const char *value;
            size_t nlen, vlen;
            if (r->type != LXB_CSS_RULE_DECLARATION) {
                continue;
            }
            d = lxb_css_rule_declaration(r);
            name = src_slice(sheet, d->offset.name_begin, d->offset.name_end, &nlen);
            value = src_slice(sheet, d->offset.value_begin, d->offset.value_end, &vlen);
            name = kn_str_trim(name, &nlen);
            value = kn_str_trim(value, &vlen);
            if (kn_str_ieq(name, nlen, "font-family")) {
                ff.family = unquote_atom(value, vlen);
            } else if (kn_str_ieq(name, nlen, "src")) {
                CssToken toks[64];
                int n = css_tokenize(value, vlen, toks, 64);
                for (int i = 0; i < n && ff.src_count < 4; i++) {
                    if (toks[i].type == CSS_TOK_URL) {
                        ff.src[ff.src_count++] = unquote_atom(toks[i].text, toks[i].len);
                    } else if (toks[i].type == CSS_TOK_FUNCTION && kn_str_ieq(toks[i].text, toks[i].len, "url")
                               && i + 1 < n && toks[i + 1].type == CSS_TOK_STRING) {
                        ff.src[ff.src_count++] = unquote_atom(toks[i + 1].text, toks[i + 1].len);
                    }
                }
            } else if (kn_str_ieq(name, nlen, "font-weight")) {
                CssToken toks[8];
                int n = css_tokens_strip_ws(toks, css_tokenize(value, vlen, toks, 8));
                int w;
                if (css_parse_font_weight(toks, n, &w)) {
                    ff.weight = (int16_t)w;
                }
            } else if (kn_str_ieq(name, nlen, "font-style")) {
                ff.style = kn_str_ieq(value, vlen, "italic") ? CSS_FONT_STYLE_ITALIC
                         : kn_str_ieq(value, vlen, "oblique") ? CSS_FONT_STYLE_OBLIQUE : CSS_FONT_STYLE_NORMAL;
            } else if (kn_str_ieq(name, nlen, "unicode-range")) {
                ff.unicode_range = kn_atom(value, vlen);
            }
        }
    }
    if (ff.family != NULL) {
        KN_VEC_PUSH(&sheet->fonts, ff);
    }
}

/* ---- @property ---------------------------------------------------------- */

/* Parses the `{ ... }` block that follows the prelude in the source text
 * (descriptors are not CSS properties, so the declaration lists Lexbor builds
 * are not used): `name: value` pairs split on top-level `;`. */
static void add_property(CssSheet *sheet, const char *prelude, size_t plen, size_t prelude_end)
{
    CssPropertyRule pr;
    int has_inherits = 0;
    size_t i = prelude_end, end;
    char q = 0;
    int depth = 0;

    prelude = kn_str_trim(prelude, &plen);
    if (plen < 3 || prelude[0] != '-' || prelude[1] != '-') {
        return;
    }
    while (i < sheet->source_len && sheet->source[i] != '{') {
        i++;
    }
    if (i >= sheet->source_len) {
        return;
    }
    i++;
    memset(&pr, 0, sizeof(pr));
    pr.name = kn_atom(prelude, plen);
    for (size_t start = i; i <= sheet->source_len; i++) {
        char c = i < sheet->source_len ? sheet->source[i] : '}';
        if (q) {
            if (c == '\\') i++;
            else if (c == q) q = 0;
            continue;
        }
        if (c == '"' || c == '\'') { q = c; continue; }
        if (c == '(' || c == '[') { depth++; continue; }
        if ((c == ')' || c == ']') && depth > 0) { depth--; continue; }
        if (depth > 0 || (c != ';' && c != '}')) continue;
        end = i;
        {
            size_t dlen = end - start;
            const char *d = kn_str_trim(sheet->source + start, &dlen);
            const char *colon = d ? memchr(d, ':', dlen) : NULL;
            if (colon != NULL) {
                size_t nlen = (size_t)(colon - d), vlen = dlen - nlen - 1;
                const char *nm = kn_str_trim(d, &nlen);
                const char *v = kn_str_trim(colon + 1, &vlen);
                if (kn_str_ieq(nm, nlen, "syntax")) {
                    if (vlen >= 2 && (v[0] == '"' || v[0] == '\'') && v[vlen - 1] == v[0]) { v++; vlen -= 2; }
                    pr.syntax = kn_atom(v, vlen);
                } else if (kn_str_ieq(nm, nlen, "inherits")) {
                    has_inherits = kn_str_ieq(v, vlen, "true") || kn_str_ieq(v, vlen, "false");
                    pr.inherits = (uint8_t)kn_str_ieq(v, vlen, "true");
                } else if (kn_str_ieq(nm, nlen, "initial-value")) {
                    pr.initial = kn_atom(v, vlen);
                }
            }
        }
        start = i + 1;
        if (c == '}') break;
    }
    if (pr.syntax == NULL || !has_inherits) {
        return;
    }
    KN_VEC_PUSH(&sheet->properties, pr);
}

/* ---- walk --------------------------------------------------------------- */

static void walk_rules(CssSheet *sheet, lxb_css_rule_t *r)
{
    for (; r != NULL; r = r->next) {
        switch (r->type) {
        case LXB_CSS_RULE_LIST:
            walk_rules(sheet, lxb_css_rule_list(r)->first);
            break;
        case LXB_CSS_RULE_STYLE: {
            lxb_css_rule_style_t *s = lxb_css_rule_style(r);
            size_t plen;
            const char *prelude = src_slice(sheet, s->prelude_begin, s->prelude_end, &plen);
            add_rule(sheet, prelude, plen, s->declarations);
            break;
        }
        case LXB_CSS_RULE_BAD_STYLE: {
            lxb_css_rule_bad_style_t *b = lxb_css_rule_bad_style(r);
            size_t plen;
            const char *prelude = src_slice(sheet, b->prelude_begin, b->prelude_end, &plen);
            add_rule(sheet, prelude, plen, b->declarations);
            break;
        }
        case LXB_CSS_RULE_AT_RULE: {
            lxb_css_rule_at_t *at = lxb_css_rule_at(r);
            if (at->type == LXB_CSS_AT_RULE_FONT_FACE) {
                add_font_face(sheet, at->u.font_face->block);
            } else if (at->type == LXB_CSS_AT_RULE__CUSTOM) {
                lxb_css_at_rule__custom_t *c = at->u.custom;
                size_t nlen = c->name.length;
                const char *nm = (const char *)c->name.data;
                if (kn_str_ieq(nm, nlen, "keyframes") || kn_str_ieq(nm, nlen, "-webkit-keyframes")) {
                    /* Blink drops @-moz-keyframes */
                    size_t plen;
                    const char *prelude = src_slice(sheet, at->prelude_begin, at->prelude_end, &plen);
                    add_keyframes(sheet, prelude, plen, c->block);
                } else if (kn_str_ieq(nm, nlen, "property")) {
                    size_t plen;
                    const char *prelude = src_slice(sheet, at->prelude_begin, at->prelude_end, &plen);
                    add_property(sheet, prelude, plen, at->prelude_end);
                }
                /* @media / @import / @supports: not used by the UI, ignored */
            }
            break;
        }
        default:
            break;
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Public                                                                     */
/* ------------------------------------------------------------------------- */

CssSheet *css_sheet_load(const char *text, size_t len, const char *name)
{
    CssSheet *sheet = (CssSheet *)calloc(1, sizeof(*sheet));
    if (sheet == NULL) {
        return NULL;
    }
    if (name == NULL) {
        name = "(sheet)";
    }
    sheet->name = (char *)malloc(strlen(name) + 1);
    if (sheet->name != NULL) {
        memcpy(sheet->name, name, strlen(name) + 1);
    }
    sheet->source = (char *)malloc(len + 1);
    if (sheet->source == NULL) {
        css_sheet_destroy(sheet);
        return NULL;
    }
    memcpy(sheet->source, text, len);
    sheet->source[len] = '\0';
    sheet->source_len = len;

    static uint32_t next_serial = 1;
    sheet->serial = next_serial++;
    sheet->inv = dom_inv_create();
    if (sheet->inv == NULL) {
        css_sheet_destroy(sheet);
        return NULL;
    }
    sheet->parser = lxb_css_parser_create();
    if (sheet->parser == NULL || lxb_css_parser_init(sheet->parser, NULL) != LXB_STATUS_OK
        || lxb_css_parser_selectors_init(sheet->parser) != LXB_STATUS_OK) {
        css_sheet_destroy(sheet);
        return NULL;
    }
    sheet->memory = lxb_css_memory_create();
    if (sheet->memory == NULL || lxb_css_memory_init(sheet->memory, 4096) != LXB_STATUS_OK) {
        css_sheet_destroy(sheet);
        return NULL;
    }
    sheet->lxb = lxb_css_stylesheet_create(sheet->memory);
    if (sheet->lxb == NULL) {
        css_sheet_destroy(sheet);
        return NULL;
    }
    if (lxb_css_stylesheet_parse(sheet->lxb, sheet->parser, (const lxb_char_t *)sheet->source, len) != LXB_STATUS_OK) {
        css_sheet_destroy(sheet);
        return NULL;
    }
    walk_rules(sheet, sheet->lxb->root);
    return sheet;
}

void css_sheet_destroy(CssSheet *sheet)
{
    if (sheet == NULL) {
        return;
    }
    for (size_t i = 0; i < sheet->selectors.len; i++) {
        if (sheet->selectors.data[i].list != NULL) {
            lxb_css_selector_list_destroy_chain(sheet->selectors.data[i].list);
        }
    }
    KN_VEC_FREE(&sheet->selectors);
    for (size_t i = 0; i < sheet->rules.len; i++) {
        decls_free(sheet->rules.data[i].decls, sheet->rules.data[i].ndecls);
    }
    KN_VEC_FREE(&sheet->rules);
    for (size_t i = 0; i < sheet->keyframes.len; i++) {
        CssKeyframes *kf = &sheet->keyframes.data[i];
        for (uint32_t k = 0; k < kf->nframes; k++) {
            decls_free(kf->frames[k].decls, kf->frames[k].ndecls);
        }
        free(kf->frames);
    }
    KN_VEC_FREE(&sheet->keyframes);
    KN_VEC_FREE(&sheet->fonts);
    KN_VEC_FREE(&sheet->properties);
    for (size_t i = 0; i < sheet->bucket_cap; i++) {
        free(sheet->buckets[i].key);
        KN_VEC_FREE(&sheet->buckets[i].sels);
    }
    free(sheet->buckets);
    dom_inv_destroy(sheet->inv);
    KN_VEC_FREE(&sheet->universal);
    if (sheet->lxb != NULL) {
        lxb_css_stylesheet_destroy(sheet->lxb, false);
    }
    if (sheet->parser != NULL) {
        /* the parser shares the sheet's memory pool after
         * lxb_css_stylesheet_parse(); destroy the pool once, below */
        if (sheet->parser->memory != NULL && sheet->parser->memory != sheet->memory) {
            lxb_css_memory_destroy(sheet->parser->memory, true);
        }
        sheet->parser->memory = NULL;
        lxb_css_parser_selectors_destroy(sheet->parser);
        lxb_css_parser_destroy(sheet->parser, true);
    }
    if (sheet->memory != NULL) {
        lxb_css_memory_destroy(sheet->memory, true);
    }
    free(sheet->source);
    free(sheet->name);
    free(sheet);
}

const char *css_sheet_name(const CssSheet *sheet) { return sheet->name; }
uint32_t css_sheet_rule_count(const CssSheet *sheet) { return (uint32_t)sheet->rules.len; }
CssRule *css_sheet_rule(const CssSheet *sheet, uint32_t i) { return i < sheet->rules.len ? &sheet->rules.data[i] : NULL; }
uint32_t css_sheet_selector_count(const CssSheet *sheet) { return (uint32_t)sheet->selectors.len; }
const CssSelector *css_sheet_selector(const CssSheet *sheet, uint32_t i) { return i < sheet->selectors.len ? &sheet->selectors.data[i] : NULL; }

const uint32_t *css_sheet_bucket(const CssSheet *sheet, int kind, const char *key, size_t len, uint32_t *count)
{
    Bucket *b = bucket_get((CssSheet *)sheet, kind, key, len, 0);
    if (b == NULL) {
        *count = 0;
        return NULL;
    }
    *count = (uint32_t)b->sels.len;
    return b->sels.data;
}

const uint32_t *css_sheet_universal(const CssSheet *sheet, uint32_t *count)
{
    *count = (uint32_t)sheet->universal.len;
    return sheet->universal.data;
}

const CssKeyframes *css_sheet_keyframes(const CssSheet *sheet, const char *name)
{
    size_t len;
    const char *atom;
    if (name == NULL) {
        return NULL;
    }
    len = strlen(name);
    atom = kn_atom(name, len);
    for (size_t i = 0; i < sheet->keyframes.len; i++) {
        if (sheet->keyframes.data[i].name == atom) {
            return &sheet->keyframes.data[i];
        }
    }
    return NULL;
}

const CssPropertyRule *css_sheet_properties(const CssSheet *sheet, uint32_t *count)
{
    *count = sheet != NULL ? (uint32_t)sheet->properties.len : 0;
    return sheet != NULL ? sheet->properties.data : NULL;
}

uint32_t css_sheet_keyframes_count(const CssSheet *sheet) { return (uint32_t)sheet->keyframes.len; }
const CssKeyframes *css_sheet_keyframes_at(const CssSheet *sheet, uint32_t i) { return i < sheet->keyframes.len ? &sheet->keyframes.data[i] : NULL; }

const CssFontFace *css_sheet_font_faces(const CssSheet *sheet, uint32_t *count)
{
    *count = (uint32_t)sheet->fonts.len;
    return sheet->fonts.data;
}

const DomInvalidation *css_sheet_invalidation(const CssSheet *sheet)
{
    return sheet->inv;
}

uint32_t css_sheet_serial(const CssSheet *sheet)
{
    return sheet != NULL ? sheet->serial : 0;
}
