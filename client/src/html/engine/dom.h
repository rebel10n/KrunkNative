/*
 * dom.h - Lexbor wrapper for the KrunkNative HTML/CSS engine.
 *
 * Owns the lxb_html_document_t, the HtmlElem handle table, the id map, inline
 * style declarations, pseudo-class state and the style-dirty bookkeeping that
 * css_cascade.c consumes.  Every element node carries an ElemData in
 * node->user; text/comment nodes keep user == NULL.
 *
 * Handles: HtmlElem is an index into the per-document node table, 0 == none.
 * A handle stays valid until dom_remove() destroys the element (or the
 * subtree it lives in); handles are never reused inside one Dom.
 *
 * Threading: none.  All functions are main-thread only.
 */
#ifndef KN_HTML_DOM_H
#define KN_HTML_DOM_H

#include <stddef.h>
#include <stdint.h>

#include <lexbor.h>

#include "css_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t HtmlElem;
typedef struct Dom Dom;

/* Dirty flags kept in ElemData.dirty.  dom.c sets the DOM_DIRTY_STYLE* and
 * DOM_DIRTY_CHILDREN/TEXT bits; css_cascade.c clears STYLE/DESCENDANTS/
 * HAS_DIRTY after a restyle and raises STYLE_CHANGED when the computed
 * record of an element (or its ::before/::after) actually changed.  The
 * layout agent owns the LAYOUT bit (and clears CHILDREN/TEXT/STYLE_CHANGED). */
enum DomDirty {
    DOM_DIRTY_STYLE         = 1u << 0, /* recompute this element's style          */
    DOM_DIRTY_DESCENDANTS   = 1u << 1, /* recompute every descendant too          */
    DOM_DIRTY_HAS_DIRTY     = 1u << 2, /* some descendant has STYLE/DESCENDANTS   */
    DOM_DIRTY_CHILDREN      = 1u << 3, /* child list changed (insert/remove/html) */
    DOM_DIRTY_TEXT          = 1u << 4, /* text content changed                    */
    DOM_DIRTY_STYLE_CHANGED = 1u << 5, /* cascade: computed values changed        */
    DOM_DIRTY_LAYOUT        = 1u << 6  /* reserved for layout.c                   */
};

/* Pseudo-class state bits.  Each one is stored as an attribute Lexbor's
 * matcher already understands (hover/active/focus/checked/disabled) or as
 * the attribute css_sheet.c rewrites `:placeholder-shown` into
 * ("kn-placeholder-shown"), so no custom matcher is needed. */
enum DomPseudoState {
    DOM_PSEUDO_HOVER             = 1u << 0,
    DOM_PSEUDO_ACTIVE            = 1u << 1,
    DOM_PSEUDO_FOCUS             = 1u << 2,
    DOM_PSEUDO_CHECKED           = 1u << 3,
    DOM_PSEUDO_DISABLED          = 1u << 4,
    DOM_PSEUDO_PLACEHOLDER_SHOWN = 1u << 5
};

/* Where dom_insert_html() puts the fragment (insertAdjacentHTML names). */
typedef enum DomInsertWhere {
    DOM_INSERT_BEFORE_BEGIN = 0,
    DOM_INSERT_AFTER_BEGIN,
    DOM_INSERT_BEFORE_END,
    DOM_INSERT_AFTER_END
} DomInsertWhere;

/* One declaration of the inline style="" attribute, raw text, in source
 * order.  `name` is lower-cased unless it is a custom property. */
typedef struct DomInlineDecl {
    char *name;
    char *value;
    uint8_t important;
} DomInlineDecl;

typedef struct ElemData {
    HtmlElem handle;
    lxb_dom_node_t *node;

    /* Owned by css_cascade.c (NULL until the first restyle). */
    CssComputed *computed;
    CssComputed *before;        /* ::before, NULL when content is none/normal */
    CssComputed *after;         /* ::after                                    */

    uint32_t dirty;             /* enum DomDirty bits                          */
    uint8_t explicit_inherit;   /* css_cascade.c: a non-inherited property reads
                                 * the parent (`inherit`, or var() that may be),
                                 * so a parent change must restyle this element */
    uint32_t pseudo;            /* enum DomPseudoState bits                    */

    DomInlineDecl *inline_decls;
    uint32_t inline_count;
    uint32_t inline_cap;

    void *box;                  /* layout.c                                    */
    void *anim;                 /* anim.c (reserved)                           */
} ElemData;

/* ---- lifetime ---------------------------------------------------------- */

Dom *dom_create(void);
void dom_destroy(Dom *dom);
/* Parses a complete document (HTML5 tree construction).  Replaces any
 * previous content.  Returns 0 on success. */
int dom_load_html(Dom *dom, const char *html, size_t len);

lxb_html_document_t *dom_document(Dom *dom);
HtmlElem dom_root(Dom *dom);   /* <html> */
HtmlElem dom_head(Dom *dom);
HtmlElem dom_body(Dom *dom);
/* One past the largest handle ever issued; side tables indexed by handle can
 * use this as their size. */
uint32_t dom_elem_capacity(Dom *dom);
/* Number of live elements. */
uint32_t dom_elem_count(Dom *dom);

/* ---- handles <-> nodes ------------------------------------------------- */

lxb_dom_node_t *dom_elem_node(Dom *dom, HtmlElem e);
HtmlElem dom_node_elem(Dom *dom, const lxb_dom_node_t *node);
ElemData *dom_elem_data(Dom *dom, HtmlElem e);
static inline ElemData *dom_node_data(const lxb_dom_node_t *node)
{
    return (node != NULL && node->type == LXB_DOM_NODE_TYPE_ELEMENT) ? (ElemData *)node->user : NULL;
}

/* ---- lookup ------------------------------------------------------------ */

HtmlElem dom_by_id(Dom *dom, const char *id);
/* root == 0 searches the whole document.  Selectors are parsed by Lexbor. */
HtmlElem dom_query(Dom *dom, HtmlElem root, const char *selector);
size_t dom_query_all(Dom *dom, HtmlElem root, const char *selector, HtmlElem *out, size_t cap);
int dom_matches(Dom *dom, HtmlElem e, const char *selector);
HtmlElem dom_closest(Dom *dom, HtmlElem e, const char *selector);

/* ---- tree navigation (elements only; text nodes are skipped) ------------ */

HtmlElem dom_parent(Dom *dom, HtmlElem e);
HtmlElem dom_first_child(Dom *dom, HtmlElem e);
HtmlElem dom_last_child(Dom *dom, HtmlElem e);
HtmlElem dom_next(Dom *dom, HtmlElem e);
HtmlElem dom_prev(Dom *dom, HtmlElem e);
size_t dom_child_count(Dom *dom, HtmlElem e);
/* Pre-order successor inside the subtree rooted at `root` (0 == document);
 * returns 0 when the traversal is done. */
HtmlElem dom_next_in_tree(Dom *dom, HtmlElem e, HtmlElem root);
/* Lower-case tag name (NUL-terminated, owned by Lexbor). */
const char *dom_tag(Dom *dom, HtmlElem e);
lxb_tag_id_t dom_tag_id(Dom *dom, HtmlElem e);

/* ---- mutation ---------------------------------------------------------- */

HtmlElem dom_create_element(Dom *dom, const char *tag);
void dom_append(Dom *dom, HtmlElem parent, HtmlElem child);
/* Inserts `child` before `ref` (same parent as ref). */
void dom_insert_before(Dom *dom, HtmlElem ref, HtmlElem child);
/* Detaches and destroys the element and its subtree; its handle dies. */
void dom_remove(Dom *dom, HtmlElem e);
/* innerHTML = ...: fragment parse with `e` as the context element. */
int dom_set_html(Dom *dom, HtmlElem e, const char *html, size_t len);
/* insertAdjacentHTML. */
int dom_insert_html(Dom *dom, HtmlElem e, DomInsertWhere where, const char *html, size_t len);
/* textContent = ... */
void dom_set_text(Dom *dom, HtmlElem e, const char *text, size_t len);
/* textContent getter: malloc'd NUL-terminated copy, caller frees. */
char *dom_get_text(Dom *dom, HtmlElem e);
/* html_dom_map_text: every text node and the title / placeholder (/ button value) attributes under
 * `root` (0 = <html>), skipping script / style / textarea / noscript / template and elements
 * matching skip_selector with their subtrees.  fn gets the owner element and the slot (child
 * index of a text node; -1 title, -2 placeholder, -3 value) and returns the replacement or NULL; `changed` is
 * told each parent whose text changed.  Returns the number of strings replaced. */
typedef const char *(*DomTextMapFn)(void *user, HtmlElem owner, int slot, const char *text);
size_t dom_map_text(Dom *dom, HtmlElem root, const char *skip_selector, DomTextMapFn fn, void *user,
                    void (*changed)(void *user, HtmlElem parent), void *changed_user);
/* Appends a text node (createTextNode + appendChild). */
void dom_append_text(Dom *dom, HtmlElem parent, const char *text, size_t len);

/* ---- attributes -------------------------------------------------------- */

/* Returns NULL when absent.  The pointer is NUL-terminated and owned by the
 * node; it is invalidated by the next write to that attribute. */
const char *dom_get_attr(Dom *dom, HtmlElem e, const char *name, size_t *len);
int dom_has_attr(Dom *dom, HtmlElem e, const char *name);
void dom_set_attr(Dom *dom, HtmlElem e, const char *name, const char *value);
void dom_remove_attr(Dom *dom, HtmlElem e, const char *name);

/* class attribute helpers (classList semantics, whitespace-separated). */
int dom_has_class(Dom *dom, HtmlElem e, const char *cls);
void dom_add_class(Dom *dom, HtmlElem e, const char *cls);
void dom_remove_class(Dom *dom, HtmlElem e, const char *cls);
/* force: -1 toggles, 0 removes, 1 adds.  Returns the new presence. */
int dom_toggle_class(Dom *dom, HtmlElem e, const char *cls, int force);
void dom_set_class(Dom *dom, HtmlElem e, const char *value);

/* value / checked convenience (stored as attributes). */
const char *dom_get_value(Dom *dom, HtmlElem e);
void dom_set_value(Dom *dom, HtmlElem e, const char *value);
int dom_get_checked(Dom *dom, HtmlElem e);
void dom_set_checked(Dom *dom, HtmlElem e, int on);

/* ---- inline style ------------------------------------------------------ */

/* element.style.setProperty(prop, value) / removeProperty when value is NULL
 * or "".  Updates the declaration list and the style attribute text.
 * `prop` is a CSS property name (e.g. "background-size", "--kn-cross-size"). */
void dom_set_style(Dom *dom, HtmlElem e, const char *prop, const char *value);
/* Returns the raw value text or NULL. */
const char *dom_get_style(Dom *dom, HtmlElem e, const char *prop);
const DomInlineDecl *dom_inline_decls(Dom *dom, HtmlElem e, size_t *count);

/* ---- pseudo-class state ------------------------------------------------- */

void dom_set_pseudo_state(Dom *dom, HtmlElem e, uint32_t bits, int on);
uint32_t dom_pseudo_state(Dom *dom, HtmlElem e);

/* ---- rule-aware invalidation ------------------------------------------- */
/*
 * The cascade hands the Dom a map of which selector features (classes, ids,
 * attribute names, state pseudo-classes) appear in which position of which
 * selector, so a mutation marks only what a matching rule could affect:
 *   SUBJECT      the feature is in a subject compound   -> restyle E
 *   ANCESTOR     left of a descendant/child combinator  -> restyle E's subtree
 *   SIBLING      left of + / ~                          -> restyle E's following siblings
 *   SIBLING_DESC left of + / ~ with a descendant/child further right
 *                                                       -> ... and their subtrees
 * State pseudo-classes are keyed by the most specific simple selector of
 * their compound (id > class > tag > universal), so `.menuItem:hover .icon`
 * only invalidates descendants of elements with class menuItem.
 * Without a map (before the first restyle) mutations invalidate
 * conservatively (self + subtree + following siblings).
 */
enum DomInvFlags {
    DOM_INV_SUBJECT      = 1u << 0,
    DOM_INV_ANCESTOR     = 1u << 1,
    DOM_INV_SIBLING      = 1u << 2,
    DOM_INV_SIBLING_DESC = 1u << 3,
    DOM_INV_ALL          = 0xfu
};

/* Global (whole-map) flags. */
enum DomInvGlobal {
    DOM_INV_G_SIBLING_COMBINATOR = 1u << 0, /* some selector uses + or ~             */
    DOM_INV_G_STRUCTURAL         = 1u << 1, /* :first-child, :nth-child, :nth-of-type */
    DOM_INV_G_STRUCTURAL_BACK    = 1u << 2  /* :last-child, :nth-last-*, :only-*, :empty */
};

/* Key kinds for state entries (match css_sheet's index keys). */
enum { DOM_INV_KEY_ANY = 0, DOM_INV_KEY_TAG = 't', DOM_INV_KEY_CLASS = '.', DOM_INV_KEY_ID = '#' };

typedef struct DomInvalidation DomInvalidation;

DomInvalidation *dom_inv_create(void);
void dom_inv_destroy(DomInvalidation *inv);
void dom_inv_clear(DomInvalidation *inv);
void dom_inv_add_class(DomInvalidation *inv, const char *name, size_t len, unsigned flags);
void dom_inv_add_id(DomInvalidation *inv, const char *name, size_t len, unsigned flags);
void dom_inv_add_attr(DomInvalidation *inv, const char *name, size_t len, unsigned flags);
/* state: one enum DomPseudoState bit; key_kind/key: the compound's key. */
void dom_inv_add_state(DomInvalidation *inv, uint32_t state, int key_kind, const char *key, size_t key_len, unsigned flags);
void dom_inv_add_global(DomInvalidation *inv, unsigned global_flags);
/* A compound whose match depends on its siblings (a structural pseudo-class, or the right side
 * of + / ~), by its key (DOM_INV_KEY_ANY: any element).  A child-list change then restyles only
 * the following siblings that carry one of these keys, not every one of them. */
void dom_inv_add_structural(DomInvalidation *inv, int key_kind, const char *key, size_t key_len);
/* The same for a tag / universal compound under an ancestor compound keyed ctx (class / id):
 * only siblings with such an ancestor count. */
void dom_inv_add_structural_ctx(DomInvalidation *inv, int key_kind, const char *key, size_t key_len,
                                int ctx_kind, const char *ctx, size_t ctx_len);
/* ORs every entry of src into dst. */
void dom_inv_merge(DomInvalidation *dst, const DomInvalidation *src);
uint32_t dom_inv_entry_count(const DomInvalidation *inv);

/* Installs the map used by subsequent mutations (not owned; NULL restores
 * the conservative behaviour). */
void dom_set_invalidation(Dom *dom, const DomInvalidation *inv);

/* ---- dirty bookkeeping -------------------------------------------------- */

/* OR `flags` into the element and raise DOM_DIRTY_HAS_DIRTY on every
 * ancestor. */
void dom_mark_dirty(Dom *dom, HtmlElem e, uint32_t flags);
/* Marks the element dirty the way a selector-relevant mutation does:
 * self + descendants + following siblings and their descendants. */
void dom_mark_selector_dirty(Dom *dom, HtmlElem e);
void dom_mark_all_dirty(Dom *dom);
int dom_any_style_dirty(Dom *dom);

#ifdef __cplusplus
}
#endif

#endif /* KN_HTML_DOM_H */
