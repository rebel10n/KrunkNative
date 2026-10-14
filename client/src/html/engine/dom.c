/*
 * dom.c - Lexbor wrapper: document, handles, id map, mutation + invalidation,
 * inline style declarations and pseudo-class state.  See dom.h.
 */
#include "dom.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ------------------------------------------------------------------------- */
/* Small string helpers (kept local so dom.c links without util.c)           */
/* ------------------------------------------------------------------------- */

static int is_space(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }
static int to_lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static char *dup_slice(const char *s, size_t len)
{
    char *out = (char *)malloc(len + 1);
    if (out == NULL) {
        return NULL;
    }
    memcpy(out, s, len);
    out[len] = '\0';
    return out;
}

static void trim(const char **s, size_t *len)
{
    while (*len > 0 && is_space((unsigned char)(*s)[0])) {
        (*s)++;
        (*len)--;
    }
    while (*len > 0 && is_space((unsigned char)(*s)[*len - 1])) {
        (*len)--;
    }
}

static int str_ieq(const char *a, size_t alen, const char *b, size_t blen)
{
    if (alen != blen) {
        return 0;
    }
    for (size_t i = 0; i < alen; i++) {
        if (to_lower((unsigned char)a[i]) != to_lower((unsigned char)b[i])) {
            return 0;
        }
    }
    return 1;
}

static uint64_t hash_str(const char *s, size_t len)
{
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < len; i++) {
        h ^= (unsigned char)s[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

/* ------------------------------------------------------------------------- */
/* Dom                                                                        */
/* ------------------------------------------------------------------------- */

typedef struct IdEntry {
    char *id;
    HtmlElem elem;
} IdEntry;

struct Dom {
    lxb_html_document_t *doc;
    lxb_css_parser_t *parser;        /* selector parser for dom_query()     */
    lxb_selectors_t *selectors;

    ElemData **table;                /* handle -> ElemData (NULL when dead) */
    uint32_t table_len;              /* next handle to issue                */
    uint32_t table_cap;
    uint32_t live;

    IdEntry *ids;                    /* open addressing, cap power of two   */
    uint32_t id_cap;
    uint32_t id_count;
    int ids_stale;

    int any_dirty;
    const DomInvalidation *inv;      /* rule-aware invalidation (not owned) */
};

static ElemData *elem_data(Dom *dom, HtmlElem e)
{
    if (e == 0 || e >= dom->table_len) {
        return NULL;
    }
    return dom->table[e];
}

static lxb_dom_node_t *elem_node(Dom *dom, HtmlElem e)
{
    ElemData *d = elem_data(dom, e);
    return d != NULL ? d->node : NULL;
}

static lxb_dom_element_t *elem_element(Dom *dom, HtmlElem e)
{
    return lxb_dom_interface_element(elem_node(dom, e));
}

static HtmlElem node_handle(const lxb_dom_node_t *node)
{
    ElemData *d = dom_node_data(node);
    return d != NULL ? d->handle : 0;
}

/* ---- handle table ------------------------------------------------------- */

static ElemData *register_node(Dom *dom, lxb_dom_node_t *node)
{
    ElemData *d;

    if (node->type != LXB_DOM_NODE_TYPE_ELEMENT) {
        return NULL;
    }
    if (node->user != NULL) {
        return (ElemData *)node->user;
    }
    if (dom->table_len == dom->table_cap) {
        uint32_t cap = dom->table_cap != 0 ? dom->table_cap * 2 : 1024;
        ElemData **t = (ElemData **)realloc(dom->table, cap * sizeof(*t));
        if (t == NULL) {
            return NULL;
        }
        memset(t + dom->table_cap, 0, (cap - dom->table_cap) * sizeof(*t));
        dom->table = t;
        dom->table_cap = cap;
    }
    if (dom->table_len == 0) {
        dom->table_len = 1; /* handle 0 is reserved */
    }
    d = (ElemData *)calloc(1, sizeof(*d));
    if (d == NULL) {
        return NULL;
    }
    d->handle = dom->table_len++;
    d->node = node;
    d->dirty = DOM_DIRTY_STYLE | DOM_DIRTY_CHILDREN | DOM_DIRTY_TEXT;
    node->user = d;
    dom->table[d->handle] = d;
    dom->live++;
    return d;
}

static void inline_clear(ElemData *d)
{
    for (uint32_t i = 0; i < d->inline_count; i++) {
        free(d->inline_decls[i].name);
        free(d->inline_decls[i].value);
    }
    free(d->inline_decls);
    d->inline_decls = NULL;
    d->inline_count = d->inline_cap = 0;
}

static void unregister_node(Dom *dom, lxb_dom_node_t *node)
{
    ElemData *d = dom_node_data(node);
    if (d == NULL) {
        return;
    }
    dom->table[d->handle] = NULL;
    dom->live--;
    inline_clear(d);
    free(d->computed);
    free(d->before);
    free(d->after);
    node->user = NULL;
    free(d);
}

/* ---- subtree walks ------------------------------------------------------ */

static void parse_style_attr(Dom *dom, ElemData *d);

static void register_subtree(Dom *dom, lxb_dom_node_t *root)
{
    lxb_dom_node_t *node = root;
    while (node != NULL) {
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT && node->user == NULL) {
            ElemData *d = register_node(dom, node);
            if (d != NULL) {
                parse_style_attr(dom, d);
                if (lxb_dom_element_has_attribute(lxb_dom_interface_element(node), (const lxb_char_t *)"checked", 7)) {
                    d->pseudo |= DOM_PSEUDO_CHECKED;
                }
                if (lxb_dom_element_has_attribute(lxb_dom_interface_element(node), (const lxb_char_t *)"disabled", 8)) {
                    d->pseudo |= DOM_PSEUDO_DISABLED;
                }
                if (node->local_name == LXB_TAG_INPUT
                    && lxb_dom_element_has_attribute(lxb_dom_interface_element(node), (const lxb_char_t *)"placeholder", 11)) {
                    size_t vlen = 0;
                    const lxb_char_t *v = lxb_dom_element_get_attribute(lxb_dom_interface_element(node), (const lxb_char_t *)"value", 5, &vlen);
                    if (v == NULL || vlen == 0) {
                        d->pseudo |= DOM_PSEUDO_PLACEHOLDER_SHOWN;
                        lxb_dom_element_set_attribute(lxb_dom_interface_element(node), (const lxb_char_t *)"kn-placeholder-shown", 20, (const lxb_char_t *)"", 0);
                    }
                }
            }
        }
        if (node->first_child != NULL) {
            node = node->first_child;
            continue;
        }
        while (node != root && node->next == NULL) {
            node = node->parent;
        }
        if (node == root) {
            break;
        }
        node = node->next;
    }
}

static void unregister_subtree(Dom *dom, lxb_dom_node_t *root)
{
    lxb_dom_node_t *node = root;
    while (node != NULL) {
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            unregister_node(dom, node);
        }
        if (node->first_child != NULL) {
            node = node->first_child;
            continue;
        }
        while (node != root && node->next == NULL) {
            node = node->parent;
        }
        if (node == root) {
            break;
        }
        node = node->next;
    }
}

static int subtree_has_id(lxb_dom_node_t *root)
{
    lxb_dom_node_t *node = root;
    while (node != NULL) {
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT && lxb_dom_interface_element(node)->attr_id != NULL) {
            return 1;
        }
        if (node->first_child != NULL) {
            node = node->first_child;
            continue;
        }
        while (node != root && node->next == NULL) {
            node = node->parent;
        }
        if (node == root) {
            break;
        }
        node = node->next;
    }
    return 0;
}

/* ---- id map ------------------------------------------------------------- */

static void ids_free(Dom *dom)
{
    for (uint32_t i = 0; i < dom->id_cap; i++) {
        free(dom->ids[i].id);
    }
    free(dom->ids);
    dom->ids = NULL;
    dom->id_cap = dom->id_count = 0;
}

static IdEntry *ids_slot(Dom *dom, const char *id, size_t len)
{
    uint32_t mask = dom->id_cap - 1;
    uint32_t i = (uint32_t)hash_str(id, len) & mask;
    for (;;) {
        IdEntry *s = &dom->ids[i];
        if (s->id == NULL || (strlen(s->id) == len && memcmp(s->id, id, len) == 0)) {
            return s;
        }
        i = (i + 1) & mask;
    }
}

static void ids_insert(Dom *dom, const char *id, size_t len, HtmlElem e)
{
    IdEntry *s;
    if (dom->id_count * 2 >= dom->id_cap) {
        uint32_t old_cap = dom->id_cap;
        IdEntry *old = dom->ids;
        dom->id_cap = old_cap != 0 ? old_cap * 2 : 256;
        dom->ids = (IdEntry *)calloc(dom->id_cap, sizeof(IdEntry));
        dom->id_count = 0;
        for (uint32_t i = 0; i < old_cap; i++) {
            if (old[i].id != NULL) {
                IdEntry *d = ids_slot(dom, old[i].id, strlen(old[i].id));
                *d = old[i];
                dom->id_count++;
            }
        }
        free(old);
    }
    s = ids_slot(dom, id, len);
    if (s->id != NULL) {
        return; /* first in document order wins */
    }
    s->id = dup_slice(id, len);
    s->elem = e;
    dom->id_count++;
}

static void ids_rebuild(Dom *dom)
{
    lxb_dom_node_t *root = lxb_dom_interface_node(dom->doc);
    lxb_dom_node_t *node = root;

    for (uint32_t i = 0; i < dom->id_cap; i++) {
        free(dom->ids[i].id);
        dom->ids[i].id = NULL;
    }
    dom->id_count = 0;

    while (node != NULL) {
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            lxb_dom_element_t *el = lxb_dom_interface_element(node);
            if (el->attr_id != NULL && el->attr_id->value != NULL && el->attr_id->value->length > 0) {
                ids_insert(dom, (const char *)el->attr_id->value->data, el->attr_id->value->length, node_handle(node));
            }
        }
        if (node->first_child != NULL) {
            node = node->first_child;
            continue;
        }
        while (node != root && node->next == NULL) {
            node = node->parent;
        }
        if (node == root) {
            break;
        }
        node = node->next;
    }
    dom->ids_stale = 0;
}

/* ---- dirty flags -------------------------------------------------------- */

static void mark_ancestors(Dom *dom, lxb_dom_node_t *node)
{
    dom->any_dirty = 1;
    for (node = node->parent; node != NULL; node = node->parent) {
        ElemData *d = dom_node_data(node);
        if (d == NULL) {
            continue;
        }
        if (d->dirty & DOM_DIRTY_HAS_DIRTY) {
            break;
        }
        d->dirty |= DOM_DIRTY_HAS_DIRTY;
    }
}

static void mark_node(Dom *dom, lxb_dom_node_t *node, uint32_t flags)
{
    ElemData *d = dom_node_data(node);
    if (d == NULL) {
        return;
    }
    d->dirty |= flags;
    mark_ancestors(dom, node);
}

static void mark_selector_dirty_node(Dom *dom, lxb_dom_node_t *node)
{
    lxb_dom_node_t *n;
    if (node == NULL || node->type != LXB_DOM_NODE_TYPE_ELEMENT) {
        return;
    }
    mark_node(dom, node, DOM_DIRTY_STYLE | DOM_DIRTY_DESCENDANTS);
    /* `+` / `~` / :nth-child / :first-child on following siblings. */
    for (n = node->next; n != NULL; n = n->next) {
        if (n->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            mark_node(dom, n, DOM_DIRTY_STYLE | DOM_DIRTY_DESCENDANTS);
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Invalidation map                                                           */
/* ------------------------------------------------------------------------- */

typedef struct InvEntry {
    char *key;                       /* kind byte(s) + lower-cased name      */
    uint32_t len;
    unsigned flags;
} InvEntry;

struct DomInvalidation {
    InvEntry *slots;                 /* open addressing, cap power of two    */
    uint32_t cap;
    uint32_t count;
    unsigned global;
    int structural_ctx;              /* dom_inv_add_structural_ctx entries */
};

DomInvalidation *dom_inv_create(void)
{
    return (DomInvalidation *)calloc(1, sizeof(DomInvalidation));
}

void dom_inv_clear(DomInvalidation *inv)
{
    if (inv == NULL) {
        return;
    }
    for (uint32_t i = 0; i < inv->cap; i++) {
        free(inv->slots[i].key);
    }
    free(inv->slots);
    inv->slots = NULL;
    inv->cap = inv->count = 0;
    inv->global = 0;
    inv->structural_ctx = 0;
}

void dom_inv_destroy(DomInvalidation *inv)
{
    dom_inv_clear(inv);
    free(inv);
}

static InvEntry *inv_slot(InvEntry *slots, uint32_t cap, const char *key, size_t len)
{
    uint32_t mask = cap - 1;
    uint32_t i = (uint32_t)hash_str(key, len) & mask;
    for (;;) {
        InvEntry *e = &slots[i];
        if (e->key == NULL || (e->len == len && memcmp(e->key, key, len) == 0)) {
            return e;
        }
        i = (i + 1) & mask;
    }
}

static void inv_put(DomInvalidation *inv, const char *key, size_t len, unsigned flags)
{
    InvEntry *e;
    if (flags == 0) {
        return;
    }
    if ((inv->count + 1) * 2 > inv->cap) {
        uint32_t ncap = inv->cap != 0 ? inv->cap * 2 : 256;
        InvEntry *ns = (InvEntry *)calloc(ncap, sizeof(InvEntry));
        if (ns == NULL) {
            return;
        }
        for (uint32_t i = 0; i < inv->cap; i++) {
            if (inv->slots[i].key != NULL) {
                *inv_slot(ns, ncap, inv->slots[i].key, inv->slots[i].len) = inv->slots[i];
            }
        }
        free(inv->slots);
        inv->slots = ns;
        inv->cap = ncap;
    }
    e = inv_slot(inv->slots, inv->cap, key, len);
    if (e->key == NULL) {
        e->key = dup_slice(key, len);
        e->len = (uint32_t)len;
        inv->count++;
    }
    e->flags |= flags;
}

static unsigned inv_get(const DomInvalidation *inv, const char *key, size_t len)
{
    const InvEntry *e;
    if (inv == NULL || inv->cap == 0) {
        return 0;
    }
    e = inv_slot(inv->slots, inv->cap, key, len);
    return e->key != NULL ? e->flags : 0;
}

/* key = prefix bytes + lower-cased name */
static size_t inv_key(char *out, size_t cap, const char *prefix, size_t plen, const char *name, size_t len)
{
    size_t o = 0;
    if (plen + len + 1 > cap) {
        len = cap > plen + 1 ? cap - plen - 1 : 0;
    }
    memcpy(out, prefix, plen);
    o = plen;
    for (size_t i = 0; i < len; i++) {
        out[o++] = (char)to_lower((unsigned char)name[i]);
    }
    out[o] = '\0';
    return o;
}

static int state_index(uint32_t state)
{
    for (int i = 0; i < 8; i++) {
        if (state == (1u << i)) {
            return i;
        }
    }
    return -1;
}

void dom_inv_add_class(DomInvalidation *inv, const char *name, size_t len, unsigned flags)
{
    char key[256];
    size_t k = inv_key(key, sizeof(key), ".", 1, name, len);
    inv_put(inv, key, k, flags);
}

void dom_inv_add_id(DomInvalidation *inv, const char *name, size_t len, unsigned flags)
{
    char key[256];
    size_t k = inv_key(key, sizeof(key), "#", 1, name, len);
    inv_put(inv, key, k, flags);
}

void dom_inv_add_attr(DomInvalidation *inv, const char *name, size_t len, unsigned flags)
{
    char key[256];
    size_t k = inv_key(key, sizeof(key), "[", 1, name, len);
    inv_put(inv, key, k, flags);
}

void dom_inv_add_state(DomInvalidation *inv, uint32_t state, int key_kind, const char *k_name, size_t k_len, unsigned flags)
{
    char key[256];
    char prefix[3];
    int idx = state_index(state);
    size_t k;
    if (idx < 0) {
        return;
    }
    prefix[0] = ':';
    prefix[1] = (char)('0' + idx);
    prefix[2] = (char)(key_kind != 0 ? key_kind : '*');
    k = inv_key(key, sizeof(key), prefix, 3, key_kind != 0 ? k_name : "", key_kind != 0 ? k_len : 0);
    inv_put(inv, key, k, flags);
}

void dom_inv_add_structural_ctx(DomInvalidation *inv, int key_kind, const char *k_name, size_t k_len,
                                int ctx_kind, const char *ctx, size_t ctx_len)
{
    char name[256], key[300];
    const char prefix[2] = { '^', (char)(key_kind != 0 ? key_kind : '*') };
    int n = snprintf(name, sizeof(name), "%.*s|%c%.*s", key_kind != 0 ? (int)k_len : 0, key_kind != 0 ? k_name : "",
                     ctx_kind, (int)ctx_len, ctx);
    if (n <= 0 || (size_t)n >= sizeof(name)) {
        dom_inv_add_structural(inv, key_kind, k_name, k_len); /* too long: unconditional */
        return;
    }
    size_t k = inv_key(key, sizeof(key), prefix, 2, name, (size_t)n);
    inv_put(inv, key, k, DOM_INV_SUBJECT);
    inv->structural_ctx = 1;
}

void dom_inv_add_global(DomInvalidation *inv, unsigned global_flags)
{
    inv->global |= global_flags;
}

void dom_inv_add_structural(DomInvalidation *inv, int key_kind, const char *k_name, size_t k_len)
{
    char key[256];
    const char prefix[2] = { '~', (char)(key_kind != 0 ? key_kind : '*') };
    size_t k = inv_key(key, sizeof(key), prefix, 2, key_kind != 0 ? k_name : "", key_kind != 0 ? k_len : 0);
    inv_put(inv, key, k, DOM_INV_SUBJECT);
}

void dom_inv_merge(DomInvalidation *dst, const DomInvalidation *src)
{
    if (dst == NULL || src == NULL) {
        return;
    }
    for (uint32_t i = 0; i < src->cap; i++) {
        if (src->slots[i].key != NULL) {
            inv_put(dst, src->slots[i].key, src->slots[i].len, src->slots[i].flags);
        }
    }
    dst->global |= src->global;
    dst->structural_ctx |= src->structural_ctx;
}

uint32_t dom_inv_entry_count(const DomInvalidation *inv)
{
    return inv != NULL ? inv->count : 0;
}

void dom_set_invalidation(Dom *dom, const DomInvalidation *inv)
{
    dom->inv = inv;
}

/* ---- lookups for one element ------------------------------------------- */

static unsigned inv_class(const DomInvalidation *inv, const char *name, size_t len)
{
    char key[256];
    size_t k = inv_key(key, sizeof(key), ".", 1, name, len);
    return inv_get(inv, key, k);
}

static unsigned inv_id(const DomInvalidation *inv, const char *name, size_t len)
{
    char key[256];
    size_t k = inv_key(key, sizeof(key), "#", 1, name, len);
    return inv_get(inv, key, k);
}

static unsigned inv_attr(const DomInvalidation *inv, const char *name, size_t len)
{
    char key[256];
    size_t k = inv_key(key, sizeof(key), "[", 1, name, len);
    return inv_get(inv, key, k);
}

static unsigned inv_state_key(const DomInvalidation *inv, int idx, int kind, const char *name, size_t len)
{
    char key[256];
    char prefix[3] = { ':', (char)('0' + idx), (char)kind };
    size_t k = inv_key(key, sizeof(key), prefix, 3, name, len);
    return inv_get(inv, key, k);
}

/* Flags for a state change of `node`: entries keyed by the element's
 * tag, id, classes and the universal key. */
static unsigned inv_state(const DomInvalidation *inv, lxb_dom_node_t *node, uint32_t state)
{
    lxb_dom_element_t *el = lxb_dom_interface_element(node);
    int idx = state_index(state);
    unsigned flags;
    size_t len = 0;
    const char *tag;
    if (idx < 0) {
        return 0;
    }
    flags = inv_state_key(inv, idx, '*', "", 0);
    tag = (const char *)lxb_dom_element_qualified_name(el, &len);
    if (tag != NULL) {
        flags |= inv_state_key(inv, idx, 't', tag, len);
    }
    if (el->attr_id != NULL && el->attr_id->value != NULL && el->attr_id->value->length > 0) {
        flags |= inv_state_key(inv, idx, '#', (const char *)el->attr_id->value->data, el->attr_id->value->length);
    }
    if (el->attr_class != NULL && el->attr_class->value != NULL) {
        const char *c = (const char *)el->attr_class->value->data;
        size_t clen = el->attr_class->value->length;
        for (size_t i = 0; i < clen;) {
            size_t st;
            while (i < clen && is_space((unsigned char)c[i])) {
                i++;
            }
            st = i;
            while (i < clen && !is_space((unsigned char)c[i])) {
                i++;
            }
            if (i > st) {
                flags |= inv_state_key(inv, idx, '.', c + st, i - st);
            }
        }
    }
    return flags;
}

/* Marks what `flags` say a feature change on `node` can affect. */
static void apply_inv(Dom *dom, lxb_dom_node_t *node, unsigned flags)
{
    lxb_dom_node_t *n;
    if (flags & DOM_INV_SUBJECT) {
        mark_node(dom, node, DOM_DIRTY_STYLE);
    }
    if (flags & DOM_INV_ANCESTOR) {
        mark_node(dom, node, DOM_DIRTY_DESCENDANTS);
    }
    if (flags & (DOM_INV_SIBLING | DOM_INV_SIBLING_DESC)) {
        uint32_t f = DOM_DIRTY_STYLE | ((flags & DOM_INV_SIBLING_DESC) ? DOM_DIRTY_DESCENDANTS : 0);
        for (n = node->next; n != NULL; n = n->next) {
            if (n->type == LXB_DOM_NODE_TYPE_ELEMENT) {
                mark_node(dom, n, f);
            }
        }
    }
}

static unsigned inv_structural_key(const DomInvalidation *inv, int kind, const char *name, size_t len)
{
    char key[256];
    const char prefix[2] = { '~', (char)kind };
    size_t k = inv_key(key, sizeof(key), prefix, 2, name, len);
    return inv_get(inv, key, k);
}

static int inv_structural_ctx_key(const DomInvalidation *inv, int kind, const char *name, size_t len, int ctx_kind,
                                  const char *ctx, size_t ctx_len)
{
    char buf[256], key[300];
    const char prefix[2] = { '^', (char)kind };
    int n = snprintf(buf, sizeof(buf), "%.*s|%c%.*s", (int)len, name, ctx_kind, (int)ctx_len, ctx);
    if (n <= 0 || (size_t)n >= sizeof(buf)) {
        return 0;
    }
    size_t k = inv_key(key, sizeof(key), prefix, 2, buf, (size_t)n);
    return inv_get(inv, key, k) != 0;
}

/* A tag / universal structural compound under a .class / #id ancestor compound
 * (dom_inv_add_structural_ctx): does one of `node`'s ancestors carry that key? */
static int inv_structural_ctx(const DomInvalidation *inv, lxb_dom_node_t *node, const char *tag, size_t tag_len)
{
    for (lxb_dom_node_t *a = node->parent; a != NULL && a->type == LXB_DOM_NODE_TYPE_ELEMENT; a = a->parent) {
        lxb_dom_element_t *ae = lxb_dom_interface_element(a);
        if (ae->attr_id != NULL && ae->attr_id->value != NULL && ae->attr_id->value->length > 0) {
            const char *id = (const char *)ae->attr_id->value->data;
            size_t il = ae->attr_id->value->length;
            if ((tag != NULL && inv_structural_ctx_key(inv, 't', tag, tag_len, '#', id, il))
                || inv_structural_ctx_key(inv, '*', "", 0, '#', id, il)) {
                return 1;
            }
        }
        if (ae->attr_class != NULL && ae->attr_class->value != NULL) {
            const char *c = (const char *)ae->attr_class->value->data;
            size_t clen = ae->attr_class->value->length;
            for (size_t i = 0; i < clen;) {
                size_t st;
                while (i < clen && is_space((unsigned char)c[i])) {
                    i++;
                }
                st = i;
                while (i < clen && !is_space((unsigned char)c[i])) {
                    i++;
                }
                if (i > st && ((tag != NULL && inv_structural_ctx_key(inv, 't', tag, tag_len, '.', c + st, i - st))
                               || inv_structural_ctx_key(inv, '*', "", 0, '.', c + st, i - st))) {
                    return 1;
                }
            }
        }
    }
    return 0;
}

/* Can a structure-sensitive compound (dom_inv_add_structural) match `node`: its tag, id, a
 * class or the universal key? */
static int inv_structural(const DomInvalidation *inv, lxb_dom_node_t *node)
{
    lxb_dom_element_t *el = lxb_dom_interface_element(node);
    size_t len = 0;
    const char *tag;
    if (inv_structural_key(inv, '*', "", 0)) {
        return 1;
    }
    tag = (const char *)lxb_dom_element_qualified_name(el, &len);
    if (tag != NULL && inv_structural_key(inv, 't', tag, len)) {
        return 1;
    }
    if (inv->structural_ctx && inv_structural_ctx(inv, node, tag, len)) {
        return 1;
    }
    if (el->attr_id != NULL && el->attr_id->value != NULL && el->attr_id->value->length > 0
        && inv_structural_key(inv, '#', (const char *)el->attr_id->value->data, el->attr_id->value->length)) {
        return 1;
    }
    if (el->attr_class != NULL && el->attr_class->value != NULL) {
        const char *c = (const char *)el->attr_class->value->data;
        size_t clen = el->attr_class->value->length;
        for (size_t i = 0; i < clen;) {
            size_t st;
            while (i < clen && is_space((unsigned char)c[i])) {
                i++;
            }
            st = i;
            while (i < clen && !is_space((unsigned char)c[i])) {
                i++;
            }
            if (i > st && inv_structural_key(inv, '.', c + st, i - st)) {
                return 1;
            }
        }
    }
    return 0;
}

/* Structural change among `parent`'s children (insert/remove): siblings see
 * a new :nth-child / + context.  `first` is the first node at or after the
 * change point (NULL: end). */
static void mark_structural(Dom *dom, lxb_dom_node_t *parent, lxb_dom_node_t *first)
{
    lxb_dom_node_t *n;
    unsigned g;
    if (parent == NULL) {
        return;
    }
    if (dom->inv == NULL) {
        g = DOM_INV_G_SIBLING_COMBINATOR | DOM_INV_G_STRUCTURAL;
    } else {
        g = dom->inv->global;
    }
    if (g & DOM_INV_G_STRUCTURAL_BACK) {
        first = parent->first_child; /* :last-child etc. look backwards */
        if (dom->inv == NULL || inv_structural(dom->inv, parent)) {
            mark_node(dom, parent, DOM_DIRTY_STYLE); /* :empty */
        }
    }
    if ((g & (DOM_INV_G_SIBLING_COMBINATOR | DOM_INV_G_STRUCTURAL | DOM_INV_G_STRUCTURAL_BACK)) == 0) {
        return;
    }
    for (n = first; n != NULL; n = n->next) {
        /* only siblings a structure-sensitive compound can match (inv: by key) */
        if (n->type == LXB_DOM_NODE_TYPE_ELEMENT && (dom->inv == NULL || inv_structural(dom->inv, n))) {
            mark_node(dom, n, DOM_DIRTY_STYLE | DOM_DIRTY_DESCENDANTS);
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Lifetime                                                                   */
/* ------------------------------------------------------------------------- */

Dom *dom_create(void)
{
    Dom *dom = (Dom *)calloc(1, sizeof(*dom));
    if (dom == NULL) {
        return NULL;
    }
    dom->parser = lxb_css_parser_create();
    if (dom->parser == NULL || lxb_css_parser_init(dom->parser, NULL) != LXB_STATUS_OK
        || lxb_css_parser_selectors_init(dom->parser) != LXB_STATUS_OK) {
        dom_destroy(dom);
        return NULL;
    }
    dom->selectors = lxb_selectors_create();
    if (dom->selectors == NULL || lxb_selectors_init(dom->selectors) != LXB_STATUS_OK) {
        dom_destroy(dom);
        return NULL;
    }
    return dom;
}

static void dom_clear_document(Dom *dom)
{
    if (dom->doc != NULL) {
        unregister_subtree(dom, lxb_dom_interface_node(dom->doc));
        lxb_html_document_destroy(dom->doc);
        dom->doc = NULL;
    }
    for (uint32_t i = 0; i < dom->table_len; i++) {
        if (dom->table[i] != NULL) {
            ElemData *d = dom->table[i];
            inline_clear(d);
            free(d->computed);
            free(d->before);
            free(d->after);
            free(d);
            dom->table[i] = NULL;
        }
    }
    dom->table_len = 0;
    dom->live = 0;
    ids_free(dom);
    dom->ids_stale = 1;
}

void dom_destroy(Dom *dom)
{
    if (dom == NULL) {
        return;
    }
    dom_clear_document(dom);
    free(dom->table);
    if (dom->selectors != NULL) {
        lxb_selectors_destroy(dom->selectors, true);
    }
    if (dom->parser != NULL) {
        /* selector lists parsed for queries live in a pool the parser
         * creates lazily on the first parse */
        if (dom->parser->memory != NULL) {
            lxb_css_memory_destroy(dom->parser->memory, true);
            dom->parser->memory = NULL;
        }
        lxb_css_parser_selectors_destroy(dom->parser);
        lxb_css_parser_destroy(dom->parser, true);
    }
    free(dom);
}

int dom_load_html(Dom *dom, const char *html, size_t len)
{
    dom_clear_document(dom);
    dom->doc = lxb_html_document_create();
    if (dom->doc == NULL) {
        return -1;
    }
    if (lxb_html_document_parse(dom->doc, (const lxb_char_t *)html, len) != LXB_STATUS_OK) {
        return -1;
    }
    register_subtree(dom, lxb_dom_interface_node(dom->doc));
    dom->ids_stale = 1;
    dom_mark_all_dirty(dom);
    return 0;
}

lxb_html_document_t *dom_document(Dom *dom) { return dom->doc; }

HtmlElem dom_root(Dom *dom)
{
    if (dom->doc == NULL) {
        return 0;
    }
    return node_handle(lxb_dom_interface_node(lxb_dom_document_element(lxb_dom_interface_document(dom->doc))));
}

HtmlElem dom_head(Dom *dom)
{
    return dom->doc != NULL ? node_handle(lxb_dom_interface_node(dom->doc->head)) : 0;
}

HtmlElem dom_body(Dom *dom)
{
    return dom->doc != NULL ? node_handle(lxb_dom_interface_node(dom->doc->body)) : 0;
}

uint32_t dom_elem_capacity(Dom *dom) { return dom->table_len; }
uint32_t dom_elem_count(Dom *dom) { return dom->live; }

/* ------------------------------------------------------------------------- */
/* Handles                                                                    */
/* ------------------------------------------------------------------------- */

lxb_dom_node_t *dom_elem_node(Dom *dom, HtmlElem e) { return elem_node(dom, e); }
HtmlElem dom_node_elem(Dom *dom, const lxb_dom_node_t *node) { (void)dom; return node_handle(node); }
ElemData *dom_elem_data(Dom *dom, HtmlElem e) { return elem_data(dom, e); }

/* ------------------------------------------------------------------------- */
/* Lookup                                                                     */
/* ------------------------------------------------------------------------- */

HtmlElem dom_by_id(Dom *dom, const char *id)
{
    IdEntry *s;
    size_t len;
    if (dom->doc == NULL || id == NULL) {
        return 0;
    }
    if (dom->ids_stale) {
        ids_rebuild(dom);
    }
    if (dom->id_cap == 0) {
        return 0;
    }
    len = strlen(id);
    s = ids_slot(dom, id, len);
    return s->id != NULL ? s->elem : 0;
}

typedef struct QueryCtx {
    HtmlElem *out;
    size_t cap;
    size_t count;
    int first_only;
} QueryCtx;

static lxb_status_t query_cb(lxb_dom_node_t *node, lxb_css_selector_specificity_t spec, void *ctx)
{
    QueryCtx *q = (QueryCtx *)ctx;
    (void)spec;
    if (q->count < q->cap && q->out != NULL) {
        q->out[q->count] = node_handle(node);
    }
    q->count++;
    return q->first_only ? LXB_STATUS_STOP : LXB_STATUS_OK;
}

static lxb_css_selector_list_t *parse_selector(Dom *dom, const char *selector)
{
    return lxb_css_selectors_parse(dom->parser, (const lxb_char_t *)selector, strlen(selector));
}

static size_t query_impl(Dom *dom, HtmlElem root, const char *selector, HtmlElem *out, size_t cap, int first_only)
{
    lxb_css_selector_list_t *list;
    lxb_dom_node_t *root_node;
    QueryCtx q = { out, cap, 0, first_only };

    if (dom->doc == NULL) {
        return 0;
    }
    root_node = root != 0 ? elem_node(dom, root) : lxb_dom_interface_node(dom->doc);
    if (root_node == NULL) {
        return 0;
    }
    list = parse_selector(dom, selector);
    if (list == NULL) {
        return 0;
    }
    lxb_selectors_find(dom->selectors, root_node, list, query_cb, &q);
    lxb_css_selector_list_destroy_chain(list);
    return q.count;
}

HtmlElem dom_query(Dom *dom, HtmlElem root, const char *selector)
{
    HtmlElem out = 0;
    query_impl(dom, root, selector, &out, 1, 1);
    return out;
}

size_t dom_query_all(Dom *dom, HtmlElem root, const char *selector, HtmlElem *out, size_t cap)
{
    return query_impl(dom, root, selector, out, cap, 0);
}

static lxb_status_t match_cb(lxb_dom_node_t *node, lxb_css_selector_specificity_t spec, void *ctx)
{
    (void)node;
    (void)spec;
    *(int *)ctx = 1;
    return LXB_STATUS_OK;
}

static int node_matches_list(Dom *dom, lxb_dom_node_t *node, lxb_css_selector_list_t *list)
{
    int hit = 0;
    lxb_selectors_match_node(dom->selectors, node, list, match_cb, &hit);
    return hit;
}

int dom_matches(Dom *dom, HtmlElem e, const char *selector)
{
    lxb_dom_node_t *node = elem_node(dom, e);
    lxb_css_selector_list_t *list;
    int hit;
    if (node == NULL) {
        return 0;
    }
    list = parse_selector(dom, selector);
    if (list == NULL) {
        return 0;
    }
    hit = node_matches_list(dom, node, list);
    lxb_css_selector_list_destroy_chain(list);
    return hit;
}

/* html_dom_map_text: pre-order walk of root's subtree (root itself included for attributes). */
static int map_attr(Dom *dom, lxb_dom_node_t *n, const char *name, int slot, DomTextMapFn fn, void *user)
{
    const HtmlElem h = node_handle(n);
    size_t len = 0;
    const char *v = h ? dom_get_attr(dom, h, name, &len) : NULL;
    if (v == NULL || len == 0) {
        return 0;
    }
    char *copy = dup_slice(v, len);
    if (copy == NULL) {
        return 0;
    }
    const char *r = fn(user, h, slot, copy);
    int changed = r != NULL && strcmp(r, copy) != 0;
    if (changed) {
        dom_set_attr(dom, h, name, r);
    }
    free(copy);
    return changed;
}

size_t dom_map_text(Dom *dom, HtmlElem root, const char *skip_selector, DomTextMapFn fn, void *user,
                    void (*changed)(void *user, HtmlElem parent), void *changed_user)
{
    lxb_dom_node_t *top = elem_node(dom, root != 0 ? root : dom_root(dom));
    lxb_css_selector_list_t *skip = NULL;
    size_t count = 0;
    if (top == NULL || fn == NULL) {
        return 0;
    }
    if (skip_selector != NULL && *skip_selector) {
        skip = parse_selector(dom, skip_selector);
    }
    lxb_dom_node_t *n = top;
    while (n != NULL) {
        int descend = 1;
        if (n->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            const lxb_tag_id_t tag = n->local_name;
            if (tag == LXB_TAG_SCRIPT || tag == LXB_TAG_STYLE || tag == LXB_TAG_TEXTAREA || tag == LXB_TAG_NOSCRIPT ||
                tag == LXB_TAG_TEMPLATE || (skip != NULL && node_matches_list(dom, n, skip))) {
                descend = 0;
            } else {
                count += (size_t) map_attr(dom, n, "title", -1, fn, user);
                count += (size_t) map_attr(dom, n, "placeholder", -2, fn, user);
                if (tag == LXB_TAG_INPUT) {
                    size_t tl = 0;
                    const HtmlElem h = node_handle(n);
                    const char *type = h ? dom_get_attr(dom, h, "type", &tl) : NULL;
                    char t[8] = "";
                    for (size_t i = 0; type != NULL && i < tl && i + 1 < sizeof t; i++) {
                        t[i] = (char)tolower((unsigned char)type[i]);
                    }
                    if (tl < sizeof t && (!strcmp(t, "button") || !strcmp(t, "submit") || !strcmp(t, "reset"))) {
                        count += (size_t) map_attr(dom, n, "value", -3, fn, user);
                    }
                }
            }
        } else if (n->type == LXB_DOM_NODE_TYPE_TEXT && n->parent != NULL) {
            lxb_dom_character_data_t *cd = lxb_dom_interface_character_data(n);
            if (cd->data.data != NULL && cd->data.length > 0 && node_handle(n->parent)) {
                int slot = 0;
                for (const lxb_dom_node_t *p = n->prev; p != NULL; p = p->prev) {
                    slot++;
                }
                const char *r = fn(user, node_handle(n->parent), slot, (const char *)cd->data.data);
                if (r != NULL && strcmp(r, (const char *)cd->data.data) != 0) {
                    lxb_dom_character_data_replace(cd, (const lxb_char_t *)r, strlen(r), 0, cd->data.length);
                    mark_node(dom, n->parent, DOM_DIRTY_CHILDREN | DOM_DIRTY_TEXT);
                    if (changed != NULL && node_handle(n->parent)) {
                        changed(changed_user, node_handle(n->parent));
                    }
                    count++;
                }
            }
        }
        /* next in pre-order, bounded by top */
        if (descend && n->first_child != NULL) {
            n = n->first_child;
            continue;
        }
        while (n != NULL && n != top && n->next == NULL) {
            n = n->parent;
        }
        n = (n == NULL || n == top) ? NULL : n->next;
    }
    if (skip != NULL) {
        lxb_css_selector_list_destroy_chain(skip);
    }
    return count;
}

HtmlElem dom_closest(Dom *dom, HtmlElem e, const char *selector)
{
    lxb_dom_node_t *node = elem_node(dom, e);
    lxb_css_selector_list_t *list;
    HtmlElem found = 0;
    if (node == NULL) {
        return 0;
    }
    list = parse_selector(dom, selector);
    if (list == NULL) {
        return 0;
    }
    for (; node != NULL && node->type == LXB_DOM_NODE_TYPE_ELEMENT; node = node->parent) {
        if (node_matches_list(dom, node, list)) {
            found = node_handle(node);
            break;
        }
    }
    lxb_css_selector_list_destroy_chain(list);
    return found;
}

/* ------------------------------------------------------------------------- */
/* Navigation                                                                 */
/* ------------------------------------------------------------------------- */

static lxb_dom_node_t *next_element(lxb_dom_node_t *n)
{
    while (n != NULL && n->type != LXB_DOM_NODE_TYPE_ELEMENT) {
        n = n->next;
    }
    return n;
}

static lxb_dom_node_t *prev_element(lxb_dom_node_t *n)
{
    while (n != NULL && n->type != LXB_DOM_NODE_TYPE_ELEMENT) {
        n = n->prev;
    }
    return n;
}

HtmlElem dom_parent(Dom *dom, HtmlElem e)
{
    lxb_dom_node_t *n = elem_node(dom, e);
    return n != NULL ? node_handle(n->parent) : 0;
}

HtmlElem dom_first_child(Dom *dom, HtmlElem e)
{
    lxb_dom_node_t *n = elem_node(dom, e);
    return n != NULL ? node_handle(next_element(n->first_child)) : 0;
}

HtmlElem dom_last_child(Dom *dom, HtmlElem e)
{
    lxb_dom_node_t *n = elem_node(dom, e);
    return n != NULL ? node_handle(prev_element(n->last_child)) : 0;
}

HtmlElem dom_next(Dom *dom, HtmlElem e)
{
    lxb_dom_node_t *n = elem_node(dom, e);
    return n != NULL ? node_handle(next_element(n->next)) : 0;
}

HtmlElem dom_prev(Dom *dom, HtmlElem e)
{
    lxb_dom_node_t *n = elem_node(dom, e);
    return n != NULL ? node_handle(prev_element(n->prev)) : 0;
}

size_t dom_child_count(Dom *dom, HtmlElem e)
{
    lxb_dom_node_t *n = elem_node(dom, e);
    size_t count = 0;
    if (n == NULL) {
        return 0;
    }
    for (n = n->first_child; n != NULL; n = n->next) {
        count += n->type == LXB_DOM_NODE_TYPE_ELEMENT;
    }
    return count;
}

HtmlElem dom_next_in_tree(Dom *dom, HtmlElem e, HtmlElem root)
{
    lxb_dom_node_t *n = elem_node(dom, e);
    lxb_dom_node_t *root_node = root != 0 ? elem_node(dom, root) : (dom->doc != NULL ? lxb_dom_interface_node(dom->doc) : NULL);
    if (n == NULL) {
        return 0;
    }
    for (;;) {
        if (n->first_child != NULL) {
            n = n->first_child;
        } else {
            while (n != root_node && n->next == NULL) {
                n = n->parent;
            }
            if (n == root_node || n == NULL) {
                return 0;
            }
            n = n->next;
        }
        if (n->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            return node_handle(n);
        }
    }
}

const char *dom_tag(Dom *dom, HtmlElem e)
{
    lxb_dom_element_t *el = elem_element(dom, e);
    size_t len;
    return el != NULL ? (const char *)lxb_dom_element_qualified_name(el, &len) : NULL;
}

lxb_tag_id_t dom_tag_id(Dom *dom, HtmlElem e)
{
    lxb_dom_node_t *n = elem_node(dom, e);
    return n != NULL ? (lxb_tag_id_t)n->local_name : LXB_TAG__UNDEF;
}

/* ------------------------------------------------------------------------- */
/* Mutation                                                                   */
/* ------------------------------------------------------------------------- */

static void after_insert(Dom *dom, lxb_dom_node_t *node)
{
    register_subtree(dom, node);
    if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
        mark_node(dom, node, DOM_DIRTY_STYLE | DOM_DIRTY_DESCENDANTS);
        mark_structural(dom, node->parent, node->next);
    } else {
        /* text node: element siblings are unaffected (selectors ignore
         * text nodes) but the parent must relayout */
        dom->any_dirty = 1;
    }
    if (node->parent != NULL) {
        mark_node(dom, node->parent, DOM_DIRTY_CHILDREN | DOM_DIRTY_TEXT);
    }
    if (subtree_has_id(node)) {
        dom->ids_stale = 1;
    }
}

HtmlElem dom_create_element(Dom *dom, const char *tag)
{
    lxb_dom_element_t *el;
    ElemData *d;
    if (dom->doc == NULL || tag == NULL) {
        return 0;
    }
    el = lxb_dom_interface_element(lxb_html_document_create_element(dom->doc, (const lxb_char_t *)tag, strlen(tag), NULL));
    if (el == NULL) {
        return 0;
    }
    d = register_node(dom, lxb_dom_interface_node(el));
    return d != NULL ? d->handle : 0;
}

void dom_append(Dom *dom, HtmlElem parent, HtmlElem child)
{
    lxb_dom_node_t *p = elem_node(dom, parent);
    lxb_dom_node_t *c = elem_node(dom, child);
    if (p == NULL || c == NULL || p == c) {
        return;
    }
    if (c->parent != NULL) {
        lxb_dom_node_remove(c);
    }
    lxb_dom_node_insert_child(p, c);
    after_insert(dom, c);
}

void dom_insert_before(Dom *dom, HtmlElem ref, HtmlElem child)
{
    lxb_dom_node_t *r = elem_node(dom, ref);
    lxb_dom_node_t *c = elem_node(dom, child);
    if (r == NULL || c == NULL || r == c || r->parent == NULL) {
        return;
    }
    if (c->parent != NULL) {
        lxb_dom_node_remove(c);
    }
    lxb_dom_node_insert_before(r, c);
    after_insert(dom, c);
}

static void remove_node(Dom *dom, lxb_dom_node_t *node)
{
    lxb_dom_node_t *parent = node->parent;
    lxb_dom_node_t *next = node->next;
    int is_elem = node->type == LXB_DOM_NODE_TYPE_ELEMENT;

    if (is_elem && subtree_has_id(node)) {
        dom->ids_stale = 1;
    }
    unregister_subtree(dom, node);
    lxb_dom_node_remove(node);
    lxb_dom_node_destroy_deep(node);

    if (parent != NULL) {
        mark_node(dom, parent, DOM_DIRTY_CHILDREN | DOM_DIRTY_TEXT);
        if (is_elem) {
            mark_structural(dom, parent, next);
        }
    }
}

void dom_remove(Dom *dom, HtmlElem e)
{
    lxb_dom_node_t *n = elem_node(dom, e);
    if (n == NULL) {
        return;
    }
    remove_node(dom, n);
}

static void remove_children(Dom *dom, lxb_dom_node_t *parent)
{
    while (parent->first_child != NULL) {
        lxb_dom_node_t *c = parent->first_child;
        if (c->type == LXB_DOM_NODE_TYPE_ELEMENT && subtree_has_id(c)) {
            dom->ids_stale = 1;
        }
        unregister_subtree(dom, c);
        lxb_dom_node_remove(c);
        lxb_dom_node_destroy_deep(c);
    }
    mark_node(dom, parent, DOM_DIRTY_CHILDREN | DOM_DIRTY_TEXT);
    if (dom->inv == NULL || ((dom->inv->global & DOM_INV_G_STRUCTURAL_BACK) != 0 && inv_structural(dom->inv, parent))) {
        mark_node(dom, parent, DOM_DIRTY_STYLE); /* :empty */
    }
}

/* Parses `html` as a fragment with `ctx` as the context element and returns
 * the detached fragment root whose children are the parsed nodes. */
static lxb_dom_node_t *parse_fragment(Dom *dom, lxb_dom_node_t *ctx, const char *html, size_t len)
{
    return lxb_html_document_parse_fragment(dom->doc, lxb_dom_interface_element(ctx), (const lxb_char_t *)html, len);
}

/* Moves every child of `frag` so that it ends up before `before` under
 * `parent` (before == NULL appends). */
static void splice_fragment(Dom *dom, lxb_dom_node_t *frag, lxb_dom_node_t *parent, lxb_dom_node_t *before)
{
    int ids = 0;
    while (frag->first_child != NULL) {
        lxb_dom_node_t *c = frag->first_child;
        lxb_dom_node_remove(c);
        if (before != NULL) {
            lxb_dom_node_insert_before(before, c);
        } else {
            lxb_dom_node_insert_child(parent, c);
        }
        register_subtree(dom, c);
        if (c->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            mark_node(dom, c, DOM_DIRTY_STYLE | DOM_DIRTY_DESCENDANTS);
            ids |= subtree_has_id(c);
        }
    }
    if (ids) {
        dom->ids_stale = 1;
    }
    lxb_dom_node_destroy(frag);
}

int dom_set_html(Dom *dom, HtmlElem e, const char *html, size_t len)
{
    lxb_dom_node_t *n = elem_node(dom, e);
    lxb_dom_node_t *frag;
    if (n == NULL) {
        return -1;
    }
    remove_children(dom, n);
    frag = parse_fragment(dom, n, html, len);
    if (frag == NULL) {
        return -1;
    }
    splice_fragment(dom, frag, n, NULL);
    mark_node(dom, n, DOM_DIRTY_CHILDREN | DOM_DIRTY_TEXT);
    return 0;
}

int dom_insert_html(Dom *dom, HtmlElem e, DomInsertWhere where, const char *html, size_t len)
{
    lxb_dom_node_t *n = elem_node(dom, e);
    lxb_dom_node_t *frag;
    lxb_dom_node_t *parent;
    lxb_dom_node_t *before;
    lxb_dom_node_t *first_following;
    if (n == NULL) {
        return -1;
    }
    switch (where) {
    case DOM_INSERT_BEFORE_BEGIN:
        if (n->parent == NULL || n->parent->type != LXB_DOM_NODE_TYPE_ELEMENT) {
            return -1;
        }
        parent = n->parent;
        before = n;
        break;
    case DOM_INSERT_AFTER_BEGIN:
        parent = n;
        before = n->first_child;
        break;
    case DOM_INSERT_BEFORE_END:
        parent = n;
        before = NULL;
        break;
    case DOM_INSERT_AFTER_END:
    default:
        if (n->parent == NULL || n->parent->type != LXB_DOM_NODE_TYPE_ELEMENT) {
            return -1;
        }
        parent = n->parent;
        before = n->next;
        break;
    }
    frag = parse_fragment(dom, parent, html, len);
    if (frag == NULL) {
        return -1;
    }
    first_following = before;
    splice_fragment(dom, frag, parent, before);
    /* siblings after the insertion point see new :nth-child / + context */
    mark_structural(dom, parent, first_following);
    mark_node(dom, parent, DOM_DIRTY_CHILDREN | DOM_DIRTY_TEXT);
    return 0;
}

void dom_set_text(Dom *dom, HtmlElem e, const char *text, size_t len)
{
    lxb_dom_node_t *n = elem_node(dom, e);
    if (n == NULL) {
        return;
    }
    remove_children(dom, n);
    if (len > 0) {
        lxb_dom_text_t *t = lxb_dom_document_create_text_node(lxb_dom_interface_document(dom->doc), (const lxb_char_t *)text, len);
        if (t != NULL) {
            lxb_dom_node_insert_child(n, lxb_dom_interface_node(t));
        }
    }
    mark_node(dom, n, DOM_DIRTY_CHILDREN | DOM_DIRTY_TEXT);
}

void dom_append_text(Dom *dom, HtmlElem parent, const char *text, size_t len)
{
    lxb_dom_node_t *n = elem_node(dom, parent);
    lxb_dom_text_t *t;
    if (n == NULL) {
        return;
    }
    t = lxb_dom_document_create_text_node(lxb_dom_interface_document(dom->doc), (const lxb_char_t *)text, len);
    if (t != NULL) {
        lxb_dom_node_insert_child(n, lxb_dom_interface_node(t));
    }
    mark_node(dom, n, DOM_DIRTY_CHILDREN | DOM_DIRTY_TEXT);
}

char *dom_get_text(Dom *dom, HtmlElem e)
{
    lxb_dom_node_t *n = elem_node(dom, e);
    size_t len = 0;
    lxb_char_t *t;
    char *out;
    if (n == NULL) {
        return NULL;
    }
    t = lxb_dom_node_text_content(n, &len);
    if (t == NULL) {
        return dup_slice("", 0);
    }
    out = dup_slice((const char *)t, len);
    lxb_dom_document_destroy_text(n->owner_document, t);
    return out;
}

/* ------------------------------------------------------------------------- */
/* Attributes                                                                 */
/* ------------------------------------------------------------------------- */

const char *dom_get_attr(Dom *dom, HtmlElem e, const char *name, size_t *len)
{
    lxb_dom_element_t *el = elem_element(dom, e);
    size_t vlen = 0;
    const lxb_char_t *v;
    if (el == NULL || name == NULL) {
        if (len != NULL) {
            *len = 0;
        }
        return NULL;
    }
    v = lxb_dom_element_get_attribute(el, (const lxb_char_t *)name, strlen(name), &vlen);
    if (len != NULL) {
        *len = vlen;
    }
    return (const char *)v;
}

int dom_has_attr(Dom *dom, HtmlElem e, const char *name)
{
    lxb_dom_element_t *el = elem_element(dom, e);
    return el != NULL && name != NULL && lxb_dom_element_has_attribute(el, (const lxb_char_t *)name, strlen(name));
}

/* Attributes that no selector in the UI can observe: changing them only
 * affects the element itself (style) or nothing style-wise. */
static int attr_is_style_only(const char *name, size_t len)
{
    return str_ieq(name, len, "style", 5);
}

static int attr_is_layout_only(const char *name, size_t len)
{
    return str_ieq(name, len, "value", 5) || str_ieq(name, len, "title", 5) || str_ieq(name, len, "width", 5)
        || str_ieq(name, len, "height", 6) || str_ieq(name, len, "alt", 3)
        || (len > 2 && to_lower((unsigned char)name[0]) == 'o' && to_lower((unsigned char)name[1]) == 'n');
}

static uint32_t state_for_attr(const char *name, size_t len);

/* Flags of every class token present in exactly one of the two lists. */
static unsigned class_diff_flags(const DomInvalidation *inv, const char *a, size_t alen, const char *b, size_t blen)
{
    unsigned flags = 0;
    for (int pass = 0; pass < 2; pass++) {
        const char *x = pass == 0 ? a : b;
        size_t xlen = pass == 0 ? alen : blen;
        const char *y = pass == 0 ? b : a;
        size_t ylen = pass == 0 ? blen : alen;
        for (size_t i = 0; i < xlen;) {
            size_t st;
            while (i < xlen && is_space((unsigned char)x[i])) {
                i++;
            }
            st = i;
            while (i < xlen && !is_space((unsigned char)x[i])) {
                i++;
            }
            if (i > st) {
                size_t tlen = i - st;
                int found = 0;
                for (size_t j = 0; j < ylen && !found;) {
                    size_t s2;
                    while (j < ylen && is_space((unsigned char)y[j])) {
                        j++;
                    }
                    s2 = j;
                    while (j < ylen && !is_space((unsigned char)y[j])) {
                        j++;
                    }
                    found = j - s2 == tlen && memcmp(y + s2, x + st, tlen) == 0;
                }
                if (!found) {
                    flags |= inv_class(inv, x + st, tlen);
                }
            }
        }
    }
    return flags;
}

/* Called after attribute `name` of `d` changed from `old` (NULL: absent;
 * only captured for class and id). */
static void after_attr_change(Dom *dom, ElemData *d, const char *name, size_t len, const char *old, size_t old_len)
{
    const DomInvalidation *inv = dom->inv;
    unsigned flags;
    int is_class = str_ieq(name, len, "class", 5);
    int is_id = str_ieq(name, len, "id", 2);
    size_t vlen = 0;
    const char *v;

    if (is_id) {
        dom->ids_stale = 1;
    }
    if (attr_is_style_only(name, len)) {
        parse_style_attr(dom, d);
        mark_node(dom, d->node, DOM_DIRTY_STYLE);
        return;
    }
    if (attr_is_layout_only(name, len)) {
        mark_node(dom, d->node, DOM_DIRTY_TEXT);
    }
    if (inv == NULL) {
        if (!attr_is_layout_only(name, len)) {
            mark_selector_dirty_node(dom, d->node);
        }
        return;
    }
    v = (const char *)lxb_dom_element_get_attribute(lxb_dom_interface_element(d->node), (const lxb_char_t *)name, len, &vlen);
    flags = inv_attr(inv, name, len);
    if (is_class) {
        flags |= class_diff_flags(inv, old != NULL ? old : "", old != NULL ? old_len : 0, v != NULL ? v : "", v != NULL ? vlen : 0);
    } else if (is_id) {
        if (old != NULL) {
            flags |= inv_id(inv, old, old_len);
        }
        if (v != NULL) {
            flags |= inv_id(inv, v, vlen);
        }
    } else {
        uint32_t st = state_for_attr(name, len);
        if (st != 0) {
            flags |= inv_state(inv, d->node, st);
        }
        /* content: attr(x) on ::before/::after */
        if (d->before != NULL || d->after != NULL) {
            flags |= DOM_INV_SUBJECT;
        }
    }
    /* state pseudo-classes keyed by class/id: a class or id change can
     * also change which state rules apply, but only while that state is on */
    if ((is_class || is_id) && d->pseudo != 0) {
        for (uint32_t bit = 1; bit <= DOM_PSEUDO_PLACEHOLDER_SHOWN; bit <<= 1) {
            if (d->pseudo & bit) {
                flags |= inv_state(inv, d->node, bit);
            }
        }
    }
    apply_inv(dom, d->node, flags);
}

static void set_attr_raw(lxb_dom_element_t *el, const char *name, size_t nlen, const char *value, size_t vlen)
{
    lxb_dom_element_set_attribute(el, (const lxb_char_t *)name, nlen, (const lxb_char_t *)value, vlen);
}

void dom_set_attr(Dom *dom, HtmlElem e, const char *name, const char *value)
{
    ElemData *d = elem_data(dom, e);
    size_t nlen;
    if (d == NULL || name == NULL) {
        return;
    }
    nlen = strlen(name);
    if (value == NULL) {
        value = "";
    }
    {
        char *old = NULL;
        size_t old_len = 0;
        if (dom->inv != NULL && (str_ieq(name, nlen, "class", 5) || str_ieq(name, nlen, "id", 2))) {
            const char *o = (const char *)lxb_dom_element_get_attribute(lxb_dom_interface_element(d->node), (const lxb_char_t *)name, nlen, &old_len);
            if (o != NULL) {
                if (old_len == strlen(value) && memcmp(o, value, old_len) == 0) {
                    return; /* unchanged */
                }
                old = dup_slice(o, old_len);
            }
        }
        set_attr_raw(lxb_dom_interface_element(d->node), name, nlen, value, strlen(value));
        after_attr_change(dom, d, name, nlen, old, old_len);
        free(old);
    }
}

void dom_remove_attr(Dom *dom, HtmlElem e, const char *name)
{
    ElemData *d = elem_data(dom, e);
    size_t nlen;
    if (d == NULL || name == NULL) {
        return;
    }
    nlen = strlen(name);
    if (!lxb_dom_element_has_attribute(lxb_dom_interface_element(d->node), (const lxb_char_t *)name, nlen)) {
        return;
    }
    {
        char *old = NULL;
        size_t old_len = 0;
        if (str_ieq(name, nlen, "class", 5) || str_ieq(name, nlen, "id", 2)) {
            const char *o = (const char *)lxb_dom_element_get_attribute(lxb_dom_interface_element(d->node), (const lxb_char_t *)name, nlen, &old_len);
            if (o != NULL) {
                old = dup_slice(o, old_len);
            }
        }
        lxb_dom_element_remove_attribute(lxb_dom_interface_element(d->node), (const lxb_char_t *)name, nlen);
        if (str_ieq(name, nlen, "style", 5)) {
            inline_clear(d);
        }
        after_attr_change(dom, d, name, nlen, old, old_len);
        free(old);
    }
}

/* ---- class list --------------------------------------------------------- */

static int class_find(const char *list, size_t len, const char *cls, size_t clen, size_t *start, size_t *end)
{
    size_t i = 0;
    while (i < len) {
        size_t s;
        while (i < len && is_space((unsigned char)list[i])) {
            i++;
        }
        s = i;
        while (i < len && !is_space((unsigned char)list[i])) {
            i++;
        }
        if (i > s && i - s == clen && memcmp(list + s, cls, clen) == 0) {
            if (start != NULL) {
                *start = s;
            }
            if (end != NULL) {
                *end = i;
            }
            return 1;
        }
    }
    return 0;
}

int dom_has_class(Dom *dom, HtmlElem e, const char *cls)
{
    size_t len = 0;
    const char *v = dom_get_attr(dom, e, "class", &len);
    return v != NULL && cls != NULL && class_find(v, len, cls, strlen(cls), NULL, NULL);
}

void dom_add_class(Dom *dom, HtmlElem e, const char *cls)
{
    size_t len = 0;
    const char *v = dom_get_attr(dom, e, "class", &len);
    size_t clen;
    char *buf;
    if (cls == NULL || elem_data(dom, e) == NULL) {
        return;
    }
    clen = strlen(cls);
    if (v != NULL && class_find(v, len, cls, clen, NULL, NULL)) {
        return;
    }
    buf = (char *)malloc(len + clen + 2);
    if (buf == NULL) {
        return;
    }
    if (v != NULL && len > 0) {
        memcpy(buf, v, len);
        buf[len] = ' ';
        memcpy(buf + len + 1, cls, clen);
        buf[len + 1 + clen] = '\0';
    } else {
        memcpy(buf, cls, clen + 1);
    }
    dom_set_attr(dom, e, "class", buf);
    free(buf);
}

void dom_remove_class(Dom *dom, HtmlElem e, const char *cls)
{
    size_t len = 0;
    const char *v = dom_get_attr(dom, e, "class", &len);
    size_t s, t;
    char *buf;
    size_t out = 0;
    if (v == NULL || cls == NULL || !class_find(v, len, cls, strlen(cls), &s, &t)) {
        return;
    }
    buf = (char *)malloc(len + 1);
    if (buf == NULL) {
        return;
    }
    /* copy tokens except the removed one, single-space separated */
    for (size_t i = 0; i < len;) {
        size_t a, b;
        while (i < len && is_space((unsigned char)v[i])) {
            i++;
        }
        a = i;
        while (i < len && !is_space((unsigned char)v[i])) {
            i++;
        }
        b = i;
        if (a == b || a == s) {
            continue;
        }
        if (out > 0) {
            buf[out++] = ' ';
        }
        memcpy(buf + out, v + a, b - a);
        out += b - a;
    }
    buf[out] = '\0';
    dom_set_attr(dom, e, "class", buf);
    free(buf);
}

int dom_toggle_class(Dom *dom, HtmlElem e, const char *cls, int force)
{
    int has = dom_has_class(dom, e, cls);
    int want = force < 0 ? !has : force;
    if (want && !has) {
        dom_add_class(dom, e, cls);
    } else if (!want && has) {
        dom_remove_class(dom, e, cls);
    }
    return want;
}

void dom_set_class(Dom *dom, HtmlElem e, const char *value)
{
    dom_set_attr(dom, e, "class", value != NULL ? value : "");
}

const char *dom_get_value(Dom *dom, HtmlElem e)
{
    const char *v = dom_get_attr(dom, e, "value", NULL);
    return v != NULL ? v : "";
}

void dom_set_value(Dom *dom, HtmlElem e, const char *value)
{
    ElemData *d = elem_data(dom, e);
    if (d == NULL) {
        return;
    }
    dom_set_attr(dom, e, "value", value);
    if (d->node->local_name == LXB_TAG_INPUT && dom_has_attr(dom, e, "placeholder")) {
        dom_set_pseudo_state(dom, e, DOM_PSEUDO_PLACEHOLDER_SHOWN, value == NULL || value[0] == '\0');
    }
}

int dom_get_checked(Dom *dom, HtmlElem e)
{
    return dom_has_attr(dom, e, "checked");
}

void dom_set_checked(Dom *dom, HtmlElem e, int on)
{
    dom_set_pseudo_state(dom, e, DOM_PSEUDO_CHECKED, on);
}

/* ------------------------------------------------------------------------- */
/* Inline style                                                               */
/* ------------------------------------------------------------------------- */

static void inline_push(ElemData *d, const char *name, size_t nlen, const char *value, size_t vlen, int important)
{
    DomInlineDecl *decl;
    if (d->inline_count == d->inline_cap) {
        uint32_t cap = d->inline_cap != 0 ? d->inline_cap * 2 : 4;
        DomInlineDecl *p = (DomInlineDecl *)realloc(d->inline_decls, cap * sizeof(*p));
        if (p == NULL) {
            return;
        }
        d->inline_decls = p;
        d->inline_cap = cap;
    }
    decl = &d->inline_decls[d->inline_count++];
    decl->name = dup_slice(name, nlen);
    decl->value = dup_slice(value, vlen);
    decl->important = (uint8_t)important;
    if (nlen < 2 || name[0] != '-' || name[1] != '-') {
        for (char *p = decl->name; *p != '\0'; p++) {
            *p = (char)to_lower((unsigned char)*p);
        }
    }
}

static DomInlineDecl *inline_find(ElemData *d, const char *name, size_t nlen)
{
    for (uint32_t i = 0; i < d->inline_count; i++) {
        if (str_ieq(d->inline_decls[i].name, strlen(d->inline_decls[i].name), name, nlen)) {
            return &d->inline_decls[i];
        }
    }
    return NULL;
}

/* Splits a declaration block on top-level ';' (outside parens/strings) and
 * each declaration on the first ':'.  Tolerant of garbage like Chrome. */
static void parse_declaration_block(ElemData *d, const char *text, size_t len)
{
    size_t i = 0;
    while (i < len) {
        size_t start = i;
        int depth = 0;
        char quote = 0;
        const char *name;
        const char *value;
        size_t nlen, vlen;
        const char *colon = NULL;
        int important = 0;

        while (i < len) {
            char c = text[i];
            if (quote != 0) {
                if (c == '\\' && i + 1 < len) {
                    i++;
                } else if (c == quote) {
                    quote = 0;
                }
            } else if (c == '"' || c == '\'') {
                quote = c;
            } else if (c == '(') {
                depth++;
            } else if (c == ')') {
                if (depth > 0) {
                    depth--;
                }
            } else if (c == ':' && colon == NULL && depth == 0) {
                colon = text + i;
            } else if (c == ';' && depth == 0) {
                break;
            }
            i++;
        }
        if (colon != NULL) {
            name = text + start;
            nlen = (size_t)(colon - name);
            value = colon + 1;
            vlen = (size_t)((text + i) - value);
            trim(&name, &nlen);
            trim(&value, &vlen);
            /* !important suffix */
            if (vlen >= 10) {
                size_t k = vlen;
                while (k > 0 && is_space((unsigned char)value[k - 1])) {
                    k--;
                }
                if (k >= 9 && str_ieq(value + k - 9, 9, "important", 9)) {
                    size_t j = k - 9;
                    while (j > 0 && is_space((unsigned char)value[j - 1])) {
                        j--;
                    }
                    if (j > 0 && value[j - 1] == '!') {
                        important = 1;
                        vlen = j - 1;
                        trim(&value, &vlen);
                    }
                }
            }
            if (nlen > 0) {
                /* later duplicate wins: drop the earlier one */
                DomInlineDecl *prev = inline_find(d, name, nlen);
                if (prev != NULL) {
                    free(prev->name);
                    free(prev->value);
                    memmove(prev, prev + 1, (size_t)(&d->inline_decls[d->inline_count] - (prev + 1)) * sizeof(*prev));
                    d->inline_count--;
                }
                inline_push(d, name, nlen, value, vlen, important);
            }
        }
        i++; /* skip ';' */
    }
}

static void parse_style_attr(Dom *dom, ElemData *d)
{
    size_t len = 0;
    const lxb_char_t *v;
    (void)dom;
    inline_clear(d);
    v = lxb_dom_element_get_attribute(lxb_dom_interface_element(d->node), (const lxb_char_t *)"style", 5, &len);
    if (v != NULL && len > 0) {
        parse_declaration_block(d, (const char *)v, len);
    }
}

static void write_style_attr(Dom *dom, ElemData *d)
{
    size_t total = 1;
    char *buf;
    size_t out = 0;
    (void)dom;
    for (uint32_t i = 0; i < d->inline_count; i++) {
        total += strlen(d->inline_decls[i].name) + strlen(d->inline_decls[i].value) + 16;
    }
    buf = (char *)malloc(total);
    if (buf == NULL) {
        return;
    }
    for (uint32_t i = 0; i < d->inline_count; i++) {
        DomInlineDecl *decl = &d->inline_decls[i];
        out += (size_t)sprintf(buf + out, "%s%s: %s%s;", i > 0 ? " " : "", decl->name, decl->value,
                               decl->important ? " !important" : "");
    }
    buf[out] = '\0';
    set_attr_raw(lxb_dom_interface_element(d->node), "style", 5, buf, out);
    free(buf);
}

void dom_set_style(Dom *dom, HtmlElem e, const char *prop, const char *value)
{
    ElemData *d = elem_data(dom, e);
    const char *v = value;
    size_t vlen;
    size_t plen;
    int important = 0;
    DomInlineDecl *decl;
    if (d == NULL || prop == NULL) {
        return;
    }
    plen = strlen(prop);
    while (plen > 0 && is_space((unsigned char)prop[0])) {
        prop++;
        plen--;
    }
    vlen = v != NULL ? strlen(v) : 0;
    if (v != NULL) {
        trim(&v, &vlen);
        /* accept a trailing "!important" like setProperty(.., "important") */
        if (vlen >= 10 && str_ieq(v + vlen - 9, 9, "important", 9)) {
            size_t j = vlen - 9;
            while (j > 0 && is_space((unsigned char)v[j - 1])) {
                j--;
            }
            if (j > 0 && v[j - 1] == '!') {
                important = 1;
                vlen = j - 1;
                trim(&v, &vlen);
            }
        }
    }
    decl = inline_find(d, prop, plen);
    if (vlen == 0) {
        if (decl == NULL) {
            return;
        }
        free(decl->name);
        free(decl->value);
        memmove(decl, decl + 1, (size_t)(&d->inline_decls[d->inline_count] - (decl + 1)) * sizeof(*decl));
        d->inline_count--;
    } else {
        if (decl != NULL) {
            if (strlen(decl->value) == vlen && memcmp(decl->value, v, vlen) == 0 && decl->important == important) {
                return; /* unchanged */
            }
            free(decl->value);
            decl->value = dup_slice(v, vlen);
            decl->important = (uint8_t)important;
        } else {
            inline_push(d, prop, plen, v, vlen, important);
        }
    }
    write_style_attr(dom, d);
    mark_node(dom, d->node, DOM_DIRTY_STYLE);
}

const char *dom_get_style(Dom *dom, HtmlElem e, const char *prop)
{
    ElemData *d = elem_data(dom, e);
    DomInlineDecl *decl;
    if (d == NULL || prop == NULL) {
        return NULL;
    }
    decl = inline_find(d, prop, strlen(prop));
    return decl != NULL ? decl->value : NULL;
}

const DomInlineDecl *dom_inline_decls(Dom *dom, HtmlElem e, size_t *count)
{
    ElemData *d = elem_data(dom, e);
    if (d == NULL) {
        if (count != NULL) {
            *count = 0;
        }
        return NULL;
    }
    if (count != NULL) {
        *count = d->inline_count;
    }
    return d->inline_decls;
}

/* ------------------------------------------------------------------------- */
/* Pseudo-class state                                                         */
/* ------------------------------------------------------------------------- */

static const struct {
    uint32_t bit;
    const char *attr;
} k_pseudo_attrs[] = {
    { DOM_PSEUDO_HOVER, "hover" },
    { DOM_PSEUDO_ACTIVE, "active" },
    { DOM_PSEUDO_FOCUS, "focus" },
    { DOM_PSEUDO_CHECKED, "checked" },
    { DOM_PSEUDO_DISABLED, "disabled" },
    { DOM_PSEUDO_PLACEHOLDER_SHOWN, "kn-placeholder-shown" },
};

void dom_set_pseudo_state(Dom *dom, HtmlElem e, uint32_t bits, int on)
{
    ElemData *d = elem_data(dom, e);
    uint32_t changed = 0;
    if (d == NULL) {
        return;
    }
    for (size_t i = 0; i < sizeof(k_pseudo_attrs) / sizeof(k_pseudo_attrs[0]); i++) {
        uint32_t bit = k_pseudo_attrs[i].bit;
        const char *attr = k_pseudo_attrs[i].attr;
        size_t alen = strlen(attr);
        int has;
        if ((bits & bit) == 0) {
            continue;
        }
        has = (d->pseudo & bit) != 0
            || lxb_dom_element_has_attribute(lxb_dom_interface_element(d->node), (const lxb_char_t *)attr, alen);
        if (on && !has) {
            set_attr_raw(lxb_dom_interface_element(d->node), attr, alen, "", 0);
            d->pseudo |= bit;
            changed |= bit;
        } else if (!on && has) {
            lxb_dom_element_remove_attribute(lxb_dom_interface_element(d->node), (const lxb_char_t *)attr, alen);
            d->pseudo &= ~bit;
            changed |= bit;
        } else if (on) {
            d->pseudo |= bit;
        } else {
            d->pseudo &= ~bit;
        }
    }
    if (changed == 0) {
        return;
    }
    if (dom->inv == NULL) {
        mark_selector_dirty_node(dom, d->node);
        return;
    }
    {
        unsigned flags = 0;
        for (size_t i = 0; i < sizeof(k_pseudo_attrs) / sizeof(k_pseudo_attrs[0]); i++) {
            if (changed & k_pseudo_attrs[i].bit) {
                /* the state pseudo-class itself, plus attribute selectors
                 * naming the backing attribute ([checked], [disabled]) */
                flags |= inv_state(dom->inv, d->node, k_pseudo_attrs[i].bit);
                flags |= inv_attr(dom->inv, k_pseudo_attrs[i].attr, strlen(k_pseudo_attrs[i].attr));
            }
        }
        apply_inv(dom, d->node, flags);
    }
}

static uint32_t state_for_attr(const char *name, size_t len)
{
    for (size_t i = 0; i < sizeof(k_pseudo_attrs) / sizeof(k_pseudo_attrs[0]); i++) {
        if (str_ieq(name, len, k_pseudo_attrs[i].attr, strlen(k_pseudo_attrs[i].attr))) {
            return k_pseudo_attrs[i].bit;
        }
    }
    return 0;
}

uint32_t dom_pseudo_state(Dom *dom, HtmlElem e)
{
    ElemData *d = elem_data(dom, e);
    return d != NULL ? d->pseudo : 0;
}

/* ------------------------------------------------------------------------- */
/* Dirty bookkeeping                                                          */
/* ------------------------------------------------------------------------- */

void dom_mark_dirty(Dom *dom, HtmlElem e, uint32_t flags)
{
    lxb_dom_node_t *n = elem_node(dom, e);
    if (n != NULL) {
        mark_node(dom, n, flags);
    }
}

void dom_mark_selector_dirty(Dom *dom, HtmlElem e)
{
    mark_selector_dirty_node(dom, elem_node(dom, e));
}

void dom_mark_all_dirty(Dom *dom)
{
    HtmlElem root = dom_root(dom);
    if (root != 0) {
        mark_node(dom, elem_node(dom, root), DOM_DIRTY_STYLE | DOM_DIRTY_DESCENDANTS);
    }
    dom->any_dirty = 1;
}

int dom_any_style_dirty(Dom *dom)
{
    HtmlElem root = dom_root(dom);
    ElemData *d = elem_data(dom, root);
    if (!dom->any_dirty) {
        return 0;
    }
    if (d == NULL) {
        dom->any_dirty = 0;
        return 0;
    }
    if ((d->dirty & (DOM_DIRTY_STYLE | DOM_DIRTY_DESCENDANTS | DOM_DIRTY_HAS_DIRTY)) == 0) {
        dom->any_dirty = 0;
        return 0;
    }
    return 1;
}
