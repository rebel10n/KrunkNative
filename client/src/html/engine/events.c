/*
 * events.c - input routing, hover/active/focus, inline handler dispatch,
 * wheel scrolling and resize observers.  See events.h for the event model.
 */
#include "events.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"
#ifndef KN_EVENTS_STANDALONE
#include "layout.h"
#endif

/* ------------------------------------------------------------------------- */
/* Inline handler parser                                                      */
/* ------------------------------------------------------------------------- */

typedef struct Cur {
    const char *p;
    const char *end;
} Cur;

static int is_ident_start(int c) { return kn_is_alpha(c) || c == '_' || c == '$'; }
static int is_ident_char(int c) { return is_ident_start(c) || kn_is_digit(c); }

static void skip_ws(Cur *c)
{
    while (c->p < c->end && kn_is_space(*c->p)) c->p++;
}

static int at_end(const Cur *c) { return c->p >= c->end; }

/* Whole-word match of `w` at the cursor; consumes it when it matches. */
static int take_word(Cur *c, const char *w)
{
    size_t n = strlen(w);
    if ((size_t)(c->end - c->p) < n || memcmp(c->p, w, n) != 0) return 0;
    if (c->p + n < c->end && is_ident_char(c->p[n])) return 0;
    c->p += n;
    return 1;
}

static int parse_ident(Cur *c, const char **s, size_t *n)
{
    if (at_end(c) || !is_ident_start(*c->p)) return -1;
    *s = c->p;
    while (c->p < c->end && is_ident_char(*c->p)) c->p++;
    *n = (size_t)(c->p - *s);
    return 0;
}

/* ident ( '.' ident | '[' digits ']' )*  ->  "a.b.0.c" in buf. */
static int parse_path(Cur *c, char *buf, size_t cap)
{
    const char *s;
    size_t n, len = 0;
    if (parse_ident(c, &s, &n) < 0) return -1;
    if (n + 1 > cap) return -1;
    memcpy(buf, s, n);
    len = n;
    for (;;) {
        if (!at_end(c) && *c->p == '.' && c->p + 1 < c->end && is_ident_start(c->p[1])) {
            c->p++;
            if (parse_ident(c, &s, &n) < 0) return -1;
            if (len + 1 + n + 1 > cap) return -1;
            buf[len++] = '.';
            memcpy(buf + len, s, n);
            len += n;
        } else if (!at_end(c) && *c->p == '[') {
            c->p++;
            if (at_end(c) || !kn_is_digit(*c->p)) return -1;
            s = c->p;
            while (c->p < c->end && kn_is_digit(*c->p)) c->p++;
            n = (size_t)(c->p - s);
            if (at_end(c) || *c->p != ']') return -1;
            c->p++;
            if (len + 1 + n + 1 > cap) return -1;
            buf[len++] = '.';
            memcpy(buf + len, s, n);
            len += n;
        } else {
            break;
        }
    }
    buf[len] = 0;
    return 0;
}

static int parse_string(Cur *c, char *buf, size_t cap, size_t *out_len)
{
    char q = *c->p;
    size_t len = 0;
    c->p++;
    while (!at_end(c) && *c->p != q) {
        char ch = *c->p++;
        if (q == '`' && ch == '$' && !at_end(c) && *c->p == '{') return -1; /* template substitution */
        if (ch == '\\' && !at_end(c)) {
            char e = *c->p++;
            switch (e) {
                case 'n': ch = '\n'; break;
                case 't': ch = '\t'; break;
                case 'r': ch = '\r'; break;
                case '0': ch = 0; break;
                default: ch = e; break; /* \' \" \\ \/ */
            }
        }
        if (len + 1 >= cap) return -1;
        buf[len++] = ch;
    }
    if (at_end(c)) return -1; /* unterminated */
    c->p++;
    buf[len] = 0;
    *out_len = len;
    return 0;
}

static int parse_number(Cur *c, double *out)
{
    char buf[64];
    const char *s = c->p;
    if (!at_end(c) && (*c->p == '-' || *c->p == '+')) c->p++;
    if (at_end(c) || !(kn_is_digit(*c->p) || (*c->p == '.' && c->p + 1 < c->end && kn_is_digit(c->p[1])))) return -1;
    while (!at_end(c) && kn_is_digit(*c->p)) c->p++;
    if (!at_end(c) && *c->p == '.') {
        c->p++;
        while (!at_end(c) && kn_is_digit(*c->p)) c->p++;
    }
    if (!at_end(c) && (*c->p == 'e' || *c->p == 'E')) {
        const char *save = c->p;
        c->p++;
        if (!at_end(c) && (*c->p == '-' || *c->p == '+')) c->p++;
        if (at_end(c) || !kn_is_digit(*c->p)) c->p = save;
        else while (!at_end(c) && kn_is_digit(*c->p)) c->p++;
    }
    size_t n = (size_t)(c->p - s);
    if (n >= sizeof(buf)) return -1;
    memcpy(buf, s, n);
    buf[n] = 0;
    *out = strtod(buf, NULL);
    return 0;
}

static int parse_arg(Cur *c, HtmlArg *arg)
{
    char buf[1024];
    size_t len;
    memset(arg, 0, sizeof(*arg));
    skip_ws(c);
    if (at_end(c)) return -1;
    if (*c->p == '\'' || *c->p == '"' || *c->p == '`') {
        if (parse_string(c, buf, sizeof(buf), &len) < 0) return -1;
        arg->kind = HTML_ARG_STR;
        arg->str = kn_atom(buf, len);
        return 0;
    }
    if (*c->p == '-' || *c->p == '+' || *c->p == '.' || kn_is_digit(*c->p)) {
        arg->kind = HTML_ARG_NUM;
        return parse_number(c, &arg->num);
    }
    if (*c->p == '!' && c->p + 1 < c->end && (c->p[1] == '0' || c->p[1] == '1')) { /* minified !0 / !1 */
        arg->kind = HTML_ARG_BOOL;
        arg->num = c->p[1] == '0';
        c->p += 2;
        return 0;
    }
    if (parse_path(c, buf, sizeof(buf)) < 0) return -1;
    if (!strcmp(buf, "true")) { arg->kind = HTML_ARG_BOOL; arg->num = 1; return 0; }
    if (!strcmp(buf, "false")) { arg->kind = HTML_ARG_BOOL; arg->num = 0; return 0; }
    if (!strcmp(buf, "this")) { arg->kind = HTML_ARG_THIS; return 0; }
    if (!strcmp(buf, "this.value")) { arg->kind = HTML_ARG_THIS_VALUE; return 0; }
    if (!strcmp(buf, "this.checked")) { arg->kind = HTML_ARG_THIS_CHECKED; return 0; }
    if (!strcmp(buf, "event")) { arg->kind = HTML_ARG_EVENT; return 0; }
    /* bare identifier (undefined, null, foo.bar): keep its text */
    arg->kind = HTML_ARG_STR;
    arg->str = kn_atom_cstr(buf);
    return 0;
}

static const char *normalise_callee(const char *path)
{
    /* window.windows[0].changeTab -> windows.0.changeTab; window.open stays. */
    if (!strncmp(path, "window.", 7) && strchr(path + 7, '.')) path += 7;
    return kn_atom_cstr(path);
}

/* callee '(' args ')' ; cursor at the callee.  `path = value` (an assignment) becomes the
 * call "path=" with the value as its one argument. */
static int parse_call(Cur *c, HandlerCall *call)
{
    char path[256];
    if (parse_path(c, path, sizeof(path)) < 0) return -1;
    skip_ws(c);
    if (at_end(c) || *c->p != '(') {
        if (!at_end(c) && *c->p == '=' && !(c->p + 1 < c->end && c->p[1] == '=')) {
            c->p++;
            if (parse_arg(c, &call->args[0]) < 0) return -1;
            if (!strcmp(path, "errorNotification.style.display") && call->args[0].kind == HTML_ARG_STR &&
                !strcmp(call->args[0].str, "none")) {
                call->name = kn_atom_cstr("kn.hideErrorNotification");
                call->argc = 0;
                return 0;
            }
            char name[260];
            snprintf(name, sizeof(name), "%s=", normalise_callee(path));
            call->name = kn_atom_cstr(name);
            call->argc = 1;
            return 0;
        }
        return -1;
    }
    c->p++;
    call->name = normalise_callee(path);
    call->argc = 0;
    skip_ws(c);
    if (!at_end(c) && *c->p == ')') {
        c->p++;
        return 0;
    }
    for (;;) {
        if (call->argc >= EV_HANDLER_MAX_ARGS) return -1;
        HtmlArg *arg = &call->args[call->argc];
        if (parse_arg(c, arg) < 0) return -1;
        skip_ws(c);
        if (arg->kind == HTML_ARG_THIS_CHECKED && !at_end(c) && *c->p == '?') { /* this.checked ? a : b */
            HtmlArg yes, no;
            c->p++;
            if (parse_arg(c, &yes) < 0 || yes.kind != HTML_ARG_NUM) return -1;
            skip_ws(c);
            if (at_end(c) || *c->p != ':') return -1;
            c->p++;
            if (parse_arg(c, &no) < 0 || no.kind != HTML_ARG_NUM) return -1;
            arg->kind = HTML_ARG_NUM;
            arg->num = yes.num;
            call->tern_mask |= (uint8_t)(1u << call->argc);
            call->tern_else[call->argc] = (float) no.num;
            skip_ws(c);
        }
        call->argc++;
        if (at_end(c)) return -1;
        if (*c->p == ',') { c->p++; continue; }
        if (*c->p == ')') { c->p++; return 0; }
        return -1;
    }
}

/* `N == [window.]event.keyCode` or `[window.]event.keyCode == N` at the cursor; the key code
 * in *key.  Restores the cursor and returns -1 when the text is something else. */
static int parse_keytest(Cur *c, int *key)
{
    const char *save = c->p;
    char path[64];
    double num = 0;
    skip_ws(c);
    if (!at_end(c) && kn_is_digit(*c->p)) {
        if (parse_number(c, &num) < 0) goto fail;
        skip_ws(c);
        if (c->end - c->p < 2 || c->p[0] != '=' || c->p[1] != '=') goto fail;
        c->p += 2;
        if (!at_end(c) && *c->p == '=') c->p++;
        skip_ws(c);
        if (parse_path(c, path, sizeof(path)) < 0) goto fail;
    } else {
        if (parse_path(c, path, sizeof(path)) < 0) goto fail;
        skip_ws(c);
        if (c->end - c->p < 2 || c->p[0] != '=' || c->p[1] != '=') goto fail;
        c->p += 2;
        if (!at_end(c) && *c->p == '=') c->p++;
        skip_ws(c);
        if (parse_number(c, &num) < 0) goto fail;
    }
    if (strcmp(path, "event.keyCode") && strcmp(path, "window.event.keyCode")) goto fail;
    *key = (int) num;
    return 0;
fail:
    c->p = save;
    return -1;
}

/* One statement; may write two calls (a ternary).  Returns the number written, -1 on error. */
static int parse_stmt(Cur *c, HandlerCall *out, int room)
{
    HandlerCall *call = &out[0];
    int key = 0;
    if (room < 1) return -1;
    memset(call, 0, sizeof(*call));
    if (take_word(c, "return")) {
        skip_ws(c);
        if (take_word(c, "false") || (c->end - c->p >= 2 && c->p[0] == '!' && c->p[1] == '1' && (c->p += 2))) {
            call->prevent_default = 1;
            return 1;
        }
        if (take_word(c, "true") || (c->end - c->p >= 2 && c->p[0] == '!' && c->p[1] == '0' && (c->p += 2))) return 0;
        if (parse_call(c, call) < 0) return -1;
        call->returns = 1;
        return 1;
    }
    if (take_word(c, "if")) { /* if (event.keyCode == 13) stmt */
        skip_ws(c);
        if (at_end(c) || *c->p != '(') return -1;
        c->p++;
        if (parse_keytest(c, &key) < 0) return -1;
        skip_ws(c);
        if (at_end(c) || *c->p != ')') return -1;
        c->p++;
        int n = parse_stmt(c, out, room);
        for (int i = 0; i < n; i++) out[i].guard_key = key;
        return n;
    }
    if (parse_keytest(c, &key) == 0) { /* 13 == event.keyCode && f() */
        skip_ws(c);
        if (c->end - c->p < 2 || c->p[0] != '&' || c->p[1] != '&') return -1;
        c->p += 2;
        skip_ws(c);
        if (parse_call(c, call) < 0) return -1;
        call->guard_key = key;
        return 1;
    }
    const char *save = c->p;
    char path[256];
    if (parse_path(c, path, sizeof(path)) == 0) {
        skip_ws(c);
        if (!at_end(c) && *c->p == '?') { /* cond ? a() : b() */
            if (room < 2) return -1;
            const char *guard = normalise_callee(path);
            c->p++;
            skip_ws(c);
            if (parse_call(c, &out[0]) < 0) return -1;
            skip_ws(c);
            if (at_end(c) || *c->p != ':') return -1;
            c->p++;
            skip_ws(c);
            memset(&out[1], 0, sizeof(out[1]));
            if (parse_call(c, &out[1]) < 0) return -1;
            out[0].guard = out[1].guard = guard;
            out[1].guard_neg = 1;
            return 2;
        }
        if ((!strcmp(path, "event.stopPropagation") || !strcmp(path, "event.preventDefault")) && !at_end(c) && *c->p == '(') {
            c->p++;
            skip_ws(c);
            if (at_end(c) || *c->p != ')') return -1;
            c->p++;
            if (path[6] == 's') call->stop_propagation = 1;
            else call->prevent_default = 1;
            return 1;
        }
    }
    c->p = save;
    if (parse_call(c, call) < 0) return -1;
    return 1;
}

int events_parse_handler(const char *text, HandlerCall *out, int max)
{
    Cur c;
    int count = 0;
    if (!text) return 0;
    c.p = text;
    c.end = text + strlen(text);
    for (;;) {
        skip_ws(&c);
        if (at_end(&c)) break;
        if (*c.p == ';' || *c.p == ',') { c.p++; continue; }
        int n = parse_stmt(&c, out + count, max - count);
        if (n < 0) return -1;
        count += n;
        skip_ws(&c);
        if (at_end(&c)) break;
        if (*c.p != ';' && *c.p != ',') return -1;
        c.p++;
    }
    return count;
}

/* ------------------------------------------------------------------------- */
/* Handler cache: (elem, type atom) -> parsed calls, validated by text        */
/* ------------------------------------------------------------------------- */

struct EvHandlerEntry {
    HtmlElem elem;      /* 0 = empty */
    const char *type;   /* atom; NULL with elem == 0 means empty, non-NULL means tombstone */
    char *text;         /* attribute text at parse time */
    HandlerCall calls[EV_HANDLER_MAX_CALLS];
    int count;          /* -1 = syntax error (already logged) */
};

static char *dup_str(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

static uint32_t handler_hash(HtmlElem e, const char *type)
{
    uint64_t h = kn_hash_bytes(KN_FNV64_INIT, &e, sizeof(e));
    h = kn_hash_bytes(h, &type, sizeof(type));
    return (uint32_t)(h ^ (h >> 32));
}

static void handler_table_free(EventSystem *es)
{
    for (uint32_t i = 0; i < es->handler_cap; i++) free(es->handlers[i].text);
    free(es->handlers);
    es->handlers = NULL;
    es->handler_cap = es->handler_count = 0;
}

static EvHandlerEntry *handler_insert_raw(EvHandlerEntry *tab, uint32_t cap, HtmlElem e, const char *type)
{
    uint32_t i = handler_hash(e, type) & (cap - 1);
    for (;;) {
        EvHandlerEntry *ent = &tab[i];
        if (ent->elem == 0) return ent; /* empty or tombstone */
        i = (i + 1) & (cap - 1);
    }
}

static void handler_table_grow(EventSystem *es)
{
    uint32_t ncap = es->handler_cap ? es->handler_cap * 2 : 64;
    EvHandlerEntry *ntab = calloc(ncap, sizeof(*ntab));
    uint32_t live = 0;
    for (uint32_t i = 0; i < es->handler_cap; i++) {
        EvHandlerEntry *ent = &es->handlers[i];
        if (ent->elem == 0) { free(ent->text); continue; }
        if (!dom_elem_data(es->dom, ent->elem)) { free(ent->text); continue; } /* dead handle */
        EvHandlerEntry *dst = handler_insert_raw(ntab, ncap, ent->elem, ent->type);
        *dst = *ent;
        live++;
    }
    free(es->handlers);
    es->handlers = ntab;
    es->handler_cap = ncap;
    es->handler_count = live;
}

static EvHandlerEntry *handler_lookup(EventSystem *es, HtmlElem e, const char *type, const char *text)
{
    if (es->handler_cap == 0 || (es->handler_count + 1) * 4 > es->handler_cap * 3) handler_table_grow(es);
    uint32_t mask = es->handler_cap - 1;
    uint32_t i = handler_hash(e, type) & mask;
    EvHandlerEntry *tomb = NULL;
    EvHandlerEntry *ent;
    for (;;) {
        ent = &es->handlers[i];
        if (ent->elem == e && ent->type == type) {
            if (strcmp(ent->text, text) == 0) return ent;
            free(ent->text); /* attribute text changed: reparse in place */
            goto parse;
        }
        if (ent->elem == 0) {
            if (ent->type == NULL) break; /* empty slot: not present */
            if (!tomb) tomb = ent;        /* tombstone: reusable */
        }
        i = (i + 1) & mask;
    }
    if (tomb) ent = tomb;
    es->handler_count++;
parse:
    ent->elem = e;
    ent->type = type;
    ent->text = dup_str(text);
    ent->count = events_parse_handler(text, ent->calls, EV_HANDLER_MAX_CALLS);
    if (ent->count < 0) fprintf(stderr, "[events] cannot parse on%s=\"%s\" (elem %u)\n", type, text, e);
    return ent;
}

void events_invalidate_handlers(EventSystem *es)
{
    handler_table_free(es);
}

/* ------------------------------------------------------------------------- */
/* DOM helpers                                                                */
/* ------------------------------------------------------------------------- */

static int alive(EventSystem *es, HtmlElem e) { return e != 0 && dom_elem_data(es->dom, e) != NULL; }

static const char *attr(EventSystem *es, HtmlElem e, const char *name)
{
    size_t len;
    return alive(es, e) ? dom_get_attr(es->dom, e, name, &len) : NULL;
}

static const char *tag_of(EventSystem *es, HtmlElem e)
{
    const char *t = alive(es, e) ? dom_tag(es->dom, e) : NULL;
    return t ? t : "";
}

static int is_tag(EventSystem *es, HtmlElem e, const char *tag) { return strcmp(tag_of(es, e), tag) == 0; }

static int is_form_control(EventSystem *es, HtmlElem e)
{
    const char *t = tag_of(es, e);
    return !strcmp(t, "input") || !strcmp(t, "select") || !strcmp(t, "textarea") || !strcmp(t, "button");
}

/* lower-cased input type, "text" for anything unknown/missing (Chrome). */
static const char *input_type(EventSystem *es, HtmlElem e, char *buf, size_t cap)
{
    const char *t = attr(es, e, "type");
    size_t n = 0;
    if (!t) return "text";
    while (t[n] && n + 1 < cap) { buf[n] = (char)kn_to_lower((unsigned char)t[n]); n++; }
    buf[n] = 0;
    return buf;
}

/* A control text is typed into: <textarea>, <select>, a text-like <input>. */
static int takes_text(EventSystem *es, HtmlElem e)
{
    if (is_tag(es, e, "textarea") || is_tag(es, e, "select")) return 1;
    if (!is_tag(es, e, "input")) return dom_has_attr(es->dom, e, "contenteditable");
    char buf[16];
    const char *t = input_type(es, e, buf, sizeof(buf));
    static const char *const no_text[] = {"checkbox", "radio", "range", "button", "submit", "reset", "color", "file", "image"};
    for (size_t i = 0; i < sizeof(no_text) / sizeof(no_text[0]); i++) {
        if (!strcmp(t, no_text[i])) return 0;
    }
    return 1;
}

static int is_disabled(EventSystem *es, HtmlElem e)
{
    if (!is_form_control(es, e)) return 0;
    if (dom_pseudo_state(es->dom, e) & DOM_PSEUDO_DISABLED) return 1;
    return dom_has_attr(es->dom, e, "disabled");
}

int events_takes_text(EventSystem *es, HtmlElem e) { return alive(es, e) && takes_text(es, e); }

static int is_checkbox(EventSystem *es, HtmlElem e)
{
    char buf[32];
    if (!is_tag(es, e, "input")) return 0;
    const char *t = input_type(es, e, buf, sizeof(buf));
    return !strcmp(t, "checkbox") || !strcmp(t, "radio");
}

/* Interactive content: a label click targeting one of these is not forwarded. */
static int is_interactive(EventSystem *es, HtmlElem e)
{
    if (is_form_control(es, e)) return 1;
    return is_tag(es, e, "a") && attr(es, e, "href") != NULL;
}

int events_is_focusable(EventSystem *es, HtmlElem e)
{
    char buf[32];
    if (!alive(es, e)) return 0;
    if (is_disabled(es, e)) return 0;
    const char *t = tag_of(es, e);
    if (!strcmp(t, "input")) return strcmp(input_type(es, e, buf, sizeof(buf)), "hidden") != 0;
    if (!strcmp(t, "select") || !strcmp(t, "textarea") || !strcmp(t, "button")) return 1;
    if (!strcmp(t, "a") && attr(es, e, "href")) return 1;
    return attr(es, e, "tabindex") != NULL;
}

static int build_chain(EventSystem *es, HtmlElem e, HtmlElem *chain)
{
    int n = 0;
    while (e && n < EV_MAX_CHAIN && alive(es, e)) {
        chain[n++] = e;
        e = dom_parent(es->dom, e);
    }
    return n;
}

static int chain_contains(const HtmlElem *chain, int n, HtmlElem e)
{
    for (int i = 0; i < n; i++) if (chain[i] == e) return 1;
    return 0;
}

/* First input (not type=hidden) or select inside `root`, document order. */
static HtmlElem find_control_in(EventSystem *es, HtmlElem root, int depth)
{
    char buf[32];
    if (depth > 32) return 0;
    for (HtmlElem c = dom_first_child(es->dom, root); c; c = dom_next(es->dom, c)) {
        const char *t = tag_of(es, c);
        if (!strcmp(t, "input")) {
            if (strcmp(input_type(es, c, buf, sizeof(buf)), "hidden") != 0) return c;
        } else if (!strcmp(t, "select") || !strcmp(t, "textarea") || !strcmp(t, "button")) {
            return c;
        }
        HtmlElem r = find_control_in(es, c, depth + 1);
        if (r) return r;
    }
    return 0;
}

/* this.value: input value, or the selected option of a <select>. */
static const char *elem_value(EventSystem *es, HtmlElem e)
{
    if (!alive(es, e)) return "";
    /* dom_get_value() never returns NULL ("" when the attribute is absent),
     * so a <select> must be resolved from its options first: the option
     * carrying `selected` (widgets.c keeps exactly one), else the first. */
    if (!is_tag(es, e, "select")) return dom_get_value(es->dom, e);
    HtmlElem first = 0, chosen = 0;
    for (HtmlElem o = dom_first_child(es->dom, e); o && !chosen; o = dom_next(es->dom, o)) {
        if (is_tag(es, o, "optgroup")) {
            for (HtmlElem p = dom_first_child(es->dom, o); p; p = dom_next(es->dom, p)) {
                if (!is_tag(es, p, "option")) continue;
                if (!first) first = p;
                if (dom_has_attr(es->dom, p, "selected")) { chosen = p; break; }
            }
            continue;
        }
        if (!is_tag(es, o, "option")) continue;
        if (!first) first = o;
        if (dom_has_attr(es->dom, o, "selected")) chosen = o;
    }
    if (!chosen) chosen = first;
    if (!chosen) return "";
    const char *ov = attr(es, chosen, "value");
    if (ov) return ov;
    char *text = dom_get_text(es->dom, chosen);
    size_t len = text ? strlen(text) : 0;
    const char *s = text ? kn_str_trim(text, &len) : "";
    if (len >= sizeof(es->value_buf)) len = sizeof(es->value_buf) - 1;
    memcpy(es->value_buf, s, len);
    es->value_buf[len] = 0;
    free(text);
    return es->value_buf;
}

static int elem_checked(EventSystem *es, HtmlElem e)
{
    return alive(es, e) ? (dom_get_checked(es->dom, e) ? 1 : 0) : 0;
}

static void set_checked(EventSystem *es, HtmlElem e, int on)
{
    dom_set_checked(es->dom, e, on);
    dom_set_pseudo_state(es->dom, e, DOM_PSEUDO_CHECKED, on);
}

static void restyle(EventSystem *es)
{
    if (es->host.restyle) es->host.restyle(es->host.user);
}

static HtmlElem hit_elem(EventSystem *es, float x, float y)
{
    if (!es->host.hit_test || !es->host.box_elem) return 0;
    Box *b = es->host.hit_test(es->host.user, x, y);
    if (!b) return 0;
    HtmlElem e = es->host.box_elem(es->host.user, b);
    return alive(es, e) ? e : 0;
}

/* ------------------------------------------------------------------------- */
/* Dispatch                                                                   */
/* ------------------------------------------------------------------------- */

static void fill_event(EventSystem *es, HtmlEvent *ev, const char *type, HtmlElem target, int key, int button)
{
    memset(ev, 0, sizeof(*ev));
    ev->type = type;
    ev->target = target;
    ev->value = elem_value(es, target);
    ev->checked = elem_checked(es, target);
    ev->key = key;
    ev->button = button;
    ev->x = es->mouse_x;
    ev->y = es->mouse_y;
    ev->mods = es->mods;
}

/* DOM keyCode of a native (GLFW) key for `event.keyCode == N` guards: printable keys
 * (space, digits, A-Z) share their codes; the named ones are mapped. */
static int dom_key_code(int key)
{
    switch (key) {
        case 257: case 335: return 13; /* Enter, keypad Enter */
        case 256: return 27;           /* Escape */
        case 258: return 9;            /* Tab */
        case 259: return 8;            /* Backspace */
        case 261: return 46;           /* Delete */
        case 262: return 39; case 263: return 37; case 264: return 40; case 265: return 38; /* arrows */
        default: return key;
    }
}

/* Runs the parsed statements of one on<type> attribute; returns 0 when the
 * default action was cancelled.  *stop is set by `event.stopPropagation()`. */
static int run_handler(const EvHandlerEntry *ent, HtmlEvent *ev, int *stop)
{
    int result = 1;
    for (int i = 0; i < ent->count; i++) {
        const HandlerCall *call = &ent->calls[i];
        if (call->guard_key > 0 && dom_key_code(ev->key) != call->guard_key) continue;
        if (call->guard) {
            const int truthy = html_ui_dispatch(call->guard, ev, NULL, 0) != 0;
            if (truthy == call->guard_neg) continue;
        }
        if (call->stop_propagation) *stop = 1;
        if (!call->name) {
            if (call->prevent_default) result = 0;
            continue;
        }
        HtmlArg args[EV_HANDLER_MAX_ARGS];
        for (int a = 0; a < call->argc; a++) {
            args[a] = call->args[a];
            switch (args[a].kind) {
                case HTML_ARG_THIS_VALUE: args[a].str = ev->value; break;
                case HTML_ARG_THIS_CHECKED: args[a].num = ev->checked; break;
                default: break;
            }
            if ((call->tern_mask >> a) & 1u) args[a].num = ev->checked ? call->args[a].num : call->tern_else[a];
        }
        int r = html_ui_dispatch(call->name, ev, args, call->argc);
        if (html_ui_take_stop_propagation()) *stop = 1; /* the handler called stopPropagation() */
        if (call->returns && r == 0) result = 0;
    }
    return result;
}

static int dispatch_inline(EventSystem *es, const HtmlElem *chain, int n, const char *type, const HtmlEvent *proto)
{
    char attrname[48];
    int result = 1;
    const char *type_atom = kn_atom_cstr(type);
    snprintf(attrname, sizeof(attrname), "on%s", type);
    for (int i = 0; i < n; i++) {
        HtmlElem e = chain[i];
        const char *text = attr(es, e, attrname);
        if (!text) continue;
        EvHandlerEntry *ent = handler_lookup(es, e, type_atom, text);
        if (ent->count <= 0) continue;
        EvHandlerEntry local = *ent; /* the table may rehash while handlers mutate the DOM */
        char value[512];             /* handlers may rewrite the value mid-chain: keep a copy */
        snprintf(value, sizeof(value), "%s", elem_value(es, e));
        HtmlEvent ev = *proto;
        ev.target = e;
        ev.value = value;
        ev.checked = elem_checked(es, e);
        int stop = 0;
        if (!run_handler(&local, &ev, &stop)) result = 0;
        if (stop) break; /* event.stopPropagation(): no ancestor's on<type> runs */
    }
    return result;
}

enum { LISTEN_NONE = 0, LISTEN_DOCUMENT = 1, LISTEN_ELEMENTS = 2, LISTEN_CAPTURE_FIRST = 4 };

static int listener_policy(const char *type)
{
    if (!strcmp(type, "mousedown")) return LISTEN_ELEMENTS;
    if (!strcmp(type, "click")) return LISTEN_DOCUMENT;
    /* document capture first, then addEventListener("keyup") on the focused input and its
     * ancestors (social.js searchPInput / chatBoxInput Enter) */
    if (!strcmp(type, "keydown") || !strcmp(type, "keyup")) return LISTEN_DOCUMENT | LISTEN_CAPTURE_FIRST | LISTEN_ELEMENTS;
    return LISTEN_NONE;
}

static int dispatch_element_listeners(EventSystem *es, const HtmlElem *chain, int n, const HtmlEvent *ev)
{
    int result = 1;
    for (int i = 0; i < n; i++) {
        const char *id = attr(es, chain[i], "id");
        if (!id || !*id) continue;
        if (!html_ui_dispatch_listener(id, ev)) result = 0;
    }
    return result;
}

static int fire_ex(EventSystem *es, HtmlElem target, const char *type, int key, int button, int bubbles)
{
    HtmlElem chain[EV_MAX_CHAIN];
    int n = 0;
    int result = 1;
    if (target) {
        if (!alive(es, target)) return 1;
        n = bubbles ? build_chain(es, target, chain) : (chain[0] = target, 1);
    }
    HtmlEvent ev;
    fill_event(es, &ev, type, target, key, button);
    int policy = listener_policy(type);
    if ((policy & LISTEN_CAPTURE_FIRST) && (policy & LISTEN_DOCUMENT)) {
        if (!html_ui_dispatch_listener(NULL, &ev)) result = 0;
    }
    if (!dispatch_inline(es, chain, n, type, &ev)) result = 0;
    if (policy & LISTEN_ELEMENTS) {
        if (!dispatch_element_listeners(es, chain, n, &ev)) result = 0;
    }
    if ((policy & LISTEN_DOCUMENT) && !(policy & LISTEN_CAPTURE_FIRST)) {
        if (!html_ui_dispatch_listener(NULL, &ev)) result = 0;
    }
    return result;
}

int events_fire(EventSystem *es, HtmlElem target, const char *type, int key, int button)
{
    int bubbles = strcmp(type, "focus") && strcmp(type, "blur") && strcmp(type, "mouseenter") && strcmp(type, "mouseleave");
    return fire_ex(es, target, type, key, button, bubbles);
}

void events_fire_input_change(EventSystem *es, HtmlElem control)
{
    fire_ex(es, control, "input", 0, 0, 1);
    fire_ex(es, control, "change", 0, 0, 1);
}

/* ------------------------------------------------------------------------- */
/* Focus                                                                      */
/* ------------------------------------------------------------------------- */

static void focus_set(EventSystem *es, HtmlElem e)
{
    if (e && !events_is_focusable(es, e)) return;
    if (e == es->focus) return;
    HtmlElem old = es->focus;
    if (old) {
        es->focus = 0;
        if (alive(es, old)) {
            dom_set_pseudo_state(es->dom, old, DOM_PSEUDO_FOCUS, 0);
            if (es->widgets.blur) es->widgets.blur(es->widgets.user, old);
            fire_ex(es, old, "blur", 0, 0, 0);
        }
    }
    if (e && alive(es, e)) {
        es->focus = e;
        dom_set_pseudo_state(es->dom, e, DOM_PSEUDO_FOCUS, 1);
        if (es->widgets.focus) es->widgets.focus(es->widgets.user, e);
        fire_ex(es, e, "focus", 0, 0, 0);
    }
    restyle(es);
}

void events_focus(EventSystem *es, HtmlElem e) { focus_set(es, e); }
void events_blur(EventSystem *es) { focus_set(es, 0); }
HtmlElem events_focused(const EventSystem *es) { return es->focus; }
HtmlElem events_hovered(const EventSystem *es) { return es->hover_len ? es->hover_chain[0] : 0; }
HtmlElem events_captured(const EventSystem *es) { return es->capture; }

/* ------------------------------------------------------------------------- */
/* Hover                                                                      */
/* ------------------------------------------------------------------------- */

static void set_hover_target(EventSystem *es, HtmlElem target)
{
    HtmlElem old[EV_MAX_CHAIN], fresh[EV_MAX_CHAIN];
    int old_n = es->hover_len, new_n = 0;
    memcpy(old, es->hover_chain, (size_t)old_n * sizeof(old[0]));
    if (target) new_n = build_chain(es, target, fresh);
    if (old_n == new_n && (old_n == 0 || !memcmp(old, fresh, (size_t)old_n * sizeof(old[0])))) return;

    HtmlElem old_target = old_n ? old[0] : 0;
    for (int i = 0; i < old_n; i++) {
        if (!chain_contains(fresh, new_n, old[i]) && alive(es, old[i]))
            dom_set_pseudo_state(es->dom, old[i], DOM_PSEUDO_HOVER, 0);
    }
    for (int i = 0; i < new_n; i++) {
        if (!chain_contains(old, old_n, fresh[i]))
            dom_set_pseudo_state(es->dom, fresh[i], DOM_PSEUDO_HOVER, 1);
    }
    memcpy(es->hover_chain, fresh, (size_t)new_n * sizeof(fresh[0]));
    es->hover_len = new_n;

    if (old_target && old_target != target) fire_ex(es, old_target, "mouseout", 0, 0, 1);
    for (int i = 0; i < old_n; i++) {                  /* innermost first */
        if (!chain_contains(fresh, new_n, old[i])) fire_ex(es, old[i], "mouseleave", 0, 0, 0);
    }
    if (target && target != old_target) fire_ex(es, target, "mouseover", 0, 0, 1);
    for (int i = new_n - 1; i >= 0; i--) {             /* outermost first */
        if (!chain_contains(old, old_n, fresh[i])) fire_ex(es, fresh[i], "mouseenter", 0, 0, 0);
    }
    restyle(es);
}

static void update_cursor(EventSystem *es)
{
    HtmlElem t = events_hovered(es);
    const ElemData *d = t ? dom_elem_data(es->dom, t) : NULL;
    es->cursor = (d && d->computed) ? d->computed->cursor : (uint8_t)CSS_CURSOR_AUTO;
}

static void update_hover(EventSystem *es)
{
    HtmlElem t = es->has_mouse ? hit_elem(es, es->mouse_x, es->mouse_y) : 0;
    set_hover_target(es, t);
    update_cursor(es);
}

static void scrollbar_drag(EventSystem *es); /* below, with the scrollbar press */

void events_mouse_move(EventSystem *es, float x, float y)
{
    es->mouse_x = x;
    es->mouse_y = y;
    es->has_mouse = 1;
    if (es->capture && es->widgets.mouse_move) {
        if (alive(es, es->capture)) es->widgets.mouse_move(es->widgets.user, es->capture, x, y, (int)es->buttons);
        else es->capture = 0;
    }
    if (es->bar_elem) scrollbar_drag(es);
    update_hover(es);
}

void events_mouse_leave(EventSystem *es)
{
    es->has_mouse = 0;
    set_hover_target(es, 0);
    es->cursor = CSS_CURSOR_AUTO;
}

int events_cursor(const EventSystem *es) { return es->cursor; }

const char *events_cursor_name(int css_cursor)
{
    switch (css_cursor) {
        case CSS_CURSOR_DEFAULT: return "default";
        case CSS_CURSOR_POINTER: return "pointer";
        case CSS_CURSOR_NONE: return "none";
        case CSS_CURSOR_CROSSHAIR: return "crosshair";
        case CSS_CURSOR_TEXT: return "text";
        case CSS_CURSOR_MOVE: return "move";
        case CSS_CURSOR_NOT_ALLOWED: return "not-allowed";
        case CSS_CURSOR_GRAB: return "grab";
        case CSS_CURSOR_GRABBING: return "grabbing";
        case CSS_CURSOR_WAIT: return "wait";
        case CSS_CURSOR_HELP: return "help";
        default: return "auto";
    }
}

/* ------------------------------------------------------------------------- */
/* Active / click                                                             */
/* ------------------------------------------------------------------------- */

static void set_active_chain(EventSystem *es, HtmlElem target)
{
    for (int i = 0; i < es->active_len; i++) {
        if (alive(es, es->active_chain[i])) dom_set_pseudo_state(es->dom, es->active_chain[i], DOM_PSEUDO_ACTIVE, 0);
    }
    es->active_len = target ? build_chain(es, target, es->active_chain) : 0;
    for (int i = 0; i < es->active_len; i++) dom_set_pseudo_state(es->dom, es->active_chain[i], DOM_PSEUDO_ACTIVE, 1);
}

/* Activation behaviour of a click at `t` (already decided to happen). */
static void do_click(EventSystem *es, HtmlElem t, int depth)
{
    if (!alive(es, t) || depth > 4) return;
    int checkbox = is_checkbox(es, t);
    int prev = checkbox ? elem_checked(es, t) : 0;
    if (checkbox) {
        char buf[32];
        int radio = !strcmp(input_type(es, t, buf, sizeof(buf)), "radio");
        set_checked(es, t, radio ? 1 : !prev);
    }
    int ok = fire_ex(es, t, "click", 0, EV_BUTTON_LEFT, 1);
    if (checkbox) {
        if (!ok) set_checked(es, t, prev);
        else events_fire_input_change(es, t);
        restyle(es);
        return;
    }
    if (!ok || is_interactive(es, t)) return;
    /* label activation: forward to the contained control */
    HtmlElem chain[EV_MAX_CHAIN];
    int n = build_chain(es, t, chain);
    for (int i = 0; i < n; i++) {
        if (!is_tag(es, chain[i], "label")) continue;
        HtmlElem control = find_control_in(es, chain[i], 0);
        if (control && !is_disabled(es, control)) {
            focus_set(es, control);
            do_click(es, control, depth + 1);
        }
        return;
    }
}

void events_click(EventSystem *es, HtmlElem target)
{
    if (is_disabled(es, target)) return;
    do_click(es, target, 0);
}

/* Nearest common ancestor (inclusive) of a and b; 0 when none. */
static HtmlElem common_ancestor(EventSystem *es, HtmlElem a, HtmlElem b)
{
    HtmlElem cb[EV_MAX_CHAIN];
    int nb = build_chain(es, b, cb);
    for (HtmlElem e = a; e && alive(es, e); e = dom_parent(es->dom, e)) {
        if (chain_contains(cb, nb, e)) return e;
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Scrollbars: thumb drag, track paging                                       */
/* ------------------------------------------------------------------------- */

static void scroll_axis_to(EventSystem *es, Box *box, const EvScrollInfo *si, int vertical, float to)
{
    const float max = vertical ? si->scroll_h - si->client_h : si->scroll_w - si->client_w;
    to = to < 0.0f ? 0.0f : to > max ? (max > 0.0f ? max : 0.0f) : to;
    const float x = vertical ? si->scroll_x : to, y = vertical ? to : si->scroll_y;
    if (x != si->scroll_x || y != si->scroll_y) es->host.set_scroll(es->host.user, box, x, y);
}

/* A left press on a scrollbar of the target or a scrolling ancestor (Chrome hit-tests the bar
 * before the content): on its thumb a drag starts, on its track the box scrolls a page towards
 * the point.  Returns 1 when the press was on a scrollbar: no mousedown default (focus, text
 * selection) and no click. */
static int scrollbar_press(EventSystem *es, HtmlElem target)
{
    if (!es->host.scrollbar || !es->host.elem_box || !es->host.scroll_info || !es->host.set_scroll || !target) return 0;
    HtmlElem chain[EV_MAX_CHAIN];
    const int n = build_chain(es, target, chain);
    for (int i = 0; i < n; i++) {
        Box *box = es->host.elem_box(es->host.user, chain[i]);
        EvScrollInfo si;
        if (!box || !es->host.scroll_info(es->host.user, box, &si)) continue;
        for (int vertical = 1; vertical >= 0; vertical--) {
            EvScrollbar sb;
            if (!es->host.scrollbar(es->host.user, box, vertical, es->mouse_x, es->mouse_y, &sb) || !sb.inside) continue;
            if (sb.pos >= sb.thumb_pos && sb.pos < sb.thumb_pos + sb.thumb_len) {
                es->bar_elem = chain[i];
                es->bar_vertical = (uint8_t)vertical;
                es->bar_grab = sb.pos - sb.thumb_pos;
            } else if (vertical ? si.can_y : si.can_x) {
                const float client = vertical ? si.client_h : si.client_w;
                const float page = fmaxf(client * EV_SCROLL_PAGE_FRACTION, 1.0f);
                const float at = vertical ? si.scroll_y : si.scroll_x;
                scroll_axis_to(es, box, &si, vertical, sb.pos < sb.thumb_pos ? at - page : at + page);
            }
            return 1;
        }
    }
    return 0;
}

/* The pointer moved during a thumb drag: the thumb follows it (the grab point stays under it). */
static void scrollbar_drag(EventSystem *es)
{
    Box *box = alive(es, es->bar_elem) ? es->host.elem_box(es->host.user, es->bar_elem) : NULL;
    EvScrollInfo si;
    EvScrollbar sb;
    if (!box || !es->host.scroll_info(es->host.user, box, &si)
        || !es->host.scrollbar(es->host.user, box, es->bar_vertical, es->mouse_x, es->mouse_y, &sb)) {
        es->bar_elem = 0;
        return;
    }
    const int v = es->bar_vertical;
    const float room = sb.track_len - sb.thumb_len;
    const float range = v ? si.scroll_h - si.client_h : si.scroll_w - si.client_w;
    if (room <= 0.0f || range <= 0.0f) return;
    scroll_axis_to(es, box, &si, v, (sb.pos - es->bar_grab) / room * range);
}

int events_scrollbar_dragging(const EventSystem *es) { return es->bar_elem != 0; }

static void mouse_press(EventSystem *es, int button, int mods)
{
    es->buttons |= 1u << button;
    es->mods = mods;
    es->down_prevented = 1; /* cleared below when mousedown's default action runs */
    HtmlElem target = es->capture ? es->capture : hit_elem(es, es->mouse_x, es->mouse_y);
    if (es->widgets.mouse_down) {
        int bits = es->widgets.mouse_down(es->widgets.user, target, es->mouse_x, es->mouse_y, button, mods);
        if ((bits & EV_WIDGET_CAPTURE) && target) es->capture = target;
        if (bits & EV_WIDGET_CONSUMED) return;
    }
    if (button == EV_BUTTON_LEFT && !es->capture && scrollbar_press(es, target)) return; /* down_prevented stays 1 */
    if (button == EV_BUTTON_LEFT) {
        set_active_chain(es, target);
        es->down_target = target;
        /* focus: nearest focusable ancestor-or-self, else a label's control */
        HtmlElem chain[EV_MAX_CHAIN];
        int n = build_chain(es, target, chain);
        HtmlElem f = 0;
        for (int i = 0; i < n && !f; i++) if (events_is_focusable(es, chain[i])) f = chain[i];
        for (int i = 0; i < n && !f; i++) {
            if (!is_tag(es, chain[i], "label")) continue;
            HtmlElem c = find_control_in(es, chain[i], 0);
            if (c && events_is_focusable(es, c)) f = c;
            break;
        }
        focus_set(es, f);
        if (target && is_tag(es, target, "select") && !is_disabled(es, target) && es->widgets.open_select) {
            int bits = es->widgets.open_select(es->widgets.user, target, es->mouse_x, es->mouse_y);
            if (bits & EV_WIDGET_CAPTURE) es->capture = target;
            if (bits & EV_WIDGET_CONSUMED) { restyle(es); return; }
        }
    }
    if (target && is_disabled(es, target)) { restyle(es); return; }
    if (button == EV_BUTTON_RIGHT) fire_ex(es, target, "contextmenu", 0, button, 1);
    es->down_prevented = fire_ex(es, target, "mousedown", 0, button, 1) == 0 || es->capture != 0;
    restyle(es);
}

static void mouse_release(EventSystem *es, int button, int mods)
{
    es->buttons &= ~(1u << button);
    es->mods = mods;
    HtmlElem target = es->capture ? es->capture : hit_elem(es, es->mouse_x, es->mouse_y);
    if (target && !alive(es, target)) target = 0;
    if (button == EV_BUTTON_LEFT) es->bar_elem = 0;
    if (es->widgets.mouse_up) es->widgets.mouse_up(es->widgets.user, target, es->mouse_x, es->mouse_y, button);
    if (es->buttons == 0) es->capture = 0;
    if (!(target && is_disabled(es, target))) fire_ex(es, target, "mouseup", 0, button, 1);
    if (button == EV_BUTTON_LEFT) {
        set_active_chain(es, 0);
        HtmlElem down = es->down_target;
        es->down_target = 0;
        HtmlElem click_target = (down && target) ? common_ancestor(es, down, target) : 0;
        if (click_target && !is_disabled(es, click_target)) do_click(es, click_target, 0);
    }
    restyle(es);
}

void events_mouse_button(EventSystem *es, int button, int action, int mods)
{
    if (button < 0 || button >= 32) return;
    if (action == EV_PRESS) mouse_press(es, button, mods);
    else if (action == EV_RELEASE) mouse_release(es, button, mods);
}

void events_set_pointer_capture(EventSystem *es, Box *box)
{
    HtmlElem e = (box && es->host.box_elem) ? es->host.box_elem(es->host.user, box) : 0;
    es->capture = alive(es, e) ? e : 0;
}

void events_release_pointer_capture(EventSystem *es) { es->capture = 0; }

/* ------------------------------------------------------------------------- */
/* Wheel                                                                      */
/* ------------------------------------------------------------------------- */

static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

int events_scroll(EventSystem *es, float dx, float dy, float x, float y)
{
    es->mouse_x = x;
    es->mouse_y = y;
    es->has_mouse = 1;
    if (!es->host.elem_box || !es->host.scroll_info || !es->host.set_scroll) return 0;
    float px = -dx * EV_SCROLL_PIXELS_PER_CLICK;
    float py = -dy * EV_SCROLL_PIXELS_PER_CLICK;
    if (px == 0.0f && py == 0.0f) return 0;
    HtmlElem chain[EV_MAX_CHAIN];
    int n = build_chain(es, hit_elem(es, x, y), chain);
    for (int i = 0; i < n; i++) {
        Box *box = es->host.elem_box(es->host.user, chain[i]);
        EvScrollInfo si;
        if (!box || !es->host.scroll_info(es->host.user, box, &si)) continue;
        float nx = si.scroll_x, ny = si.scroll_y;
        if (si.can_y && py != 0.0f) ny = clampf(si.scroll_y + py, 0.0f, fmaxf(0.0f, si.scroll_h - si.client_h));
        if (si.can_x && px != 0.0f) nx = clampf(si.scroll_x + px, 0.0f, fmaxf(0.0f, si.scroll_w - si.client_w));
        if (nx != si.scroll_x || ny != si.scroll_y) {
            es->host.set_scroll(es->host.user, box, nx, ny);
            update_hover(es);
            return 1;
        }
        /* at the limit: chain to the next scrollable ancestor (Chrome) */
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Keyboard                                                                   */
/* ------------------------------------------------------------------------- */

static int key_is_enter(int key) { return key == EV_KEY_ENTER || key == EV_KEY_KP_ENTER; }

/* Sequential focus navigation (HTML "focus navigation order", simplified: document order, no
 * positive-tabindex ordering, which ui/ never uses). A candidate is focusable, rendered (has a
 * box) and has no negative tabindex. Moves from `from` forwards or
 * backwards with wrap-around. */
static int tab_candidate(EventSystem *es, HtmlElem e)
{
    if (!events_is_focusable(es, e)) return 0;
    const char *ti = attr(es, e, "tabindex");
    if (ti && atoi(ti) < 0) return 0;
    HtmlRect r;
    if (es->host.elem_rect && !es->host.elem_rect(es->host.user, e, &r)) return 0;
    return 1;
}

/* Pre-order successor of e (elements only), 0 after the last. */
static HtmlElem tree_next(EventSystem *es, HtmlElem e)
{
    HtmlElem c = dom_first_child(es->dom, e);
    if (c) return c;
    for (; e; e = dom_parent(es->dom, e)) {
        HtmlElem n = dom_next(es->dom, e);
        if (n) return n;
    }
    return 0;
}

static void focus_step(EventSystem *es, HtmlElem from, int backwards)
{
    /* document order walk from the top of from's tree: the page is a few thousand nodes */
    HtmlElem top = from;
    while (top && dom_parent(es->dom, top)) top = dom_parent(es->dom, top);
    HtmlElem first = 0, last = 0, prev = 0, next = 0;
    int seen = 0;
    for (HtmlElem e = top; e; e = tree_next(es, e)) {
        if (e == from) { seen = 1; continue; }
        if (!tab_candidate(es, e)) continue;
        if (!first) first = e;
        last = e;
        if (!seen) prev = e;
        else if (!next) next = e;
    }
    HtmlElem to = backwards ? (prev ? prev : last) : (next ? next : first);
    if (to && to != from) focus_set(es, to);
}

int events_key(EventSystem *es, int key, int scancode, int action, int mods)
{
    const char *type = action == EV_RELEASE ? "keyup" : "keydown";
    es->mods = mods;
    if (action != EV_RELEASE) es->last_key = key;
    HtmlElem f = es->focus;
    if (f && !alive(es, f)) { es->focus = 0; f = 0; }
    if (!f) {
        /* window-level listener (Escape closes a window / toggles the menu);
         * a 0 result (preventDefault) means the page consumed the key. */
        int r = fire_ex(es, 0, type, key, 0, 0);
        return r == 0;
    }
    if (action != EV_RELEASE && key == EV_KEY_ESCAPE) {
        focus_set(es, 0);
        /* A text field keeps its Escape (it only blurs). A switch / button / link that kept the
         * focus after a click does not: the key goes on to the window listener as if nothing
         * were focused (the menu's Spectate switch must not swallow the Escape that closes
         * the menu). */
        if (!takes_text(es, f)) return fire_ex(es, 0, type, key, 0, 0) == 0;
        return 1;
    }
    int r = fire_ex(es, f, type, key, 0, 1); /* document capture listener first, then on<type> up the tree */
    if (r && es->widgets.key && es->widgets.key(es->widgets.user, f, key, scancode, action, mods)) {
        /* consumed by the widget (caret, backspace, popup navigation) */
    } else if (r && action != EV_RELEASE && key == EV_KEY_TAB) {
        /* the browser's default action: Tab / Shift+Tab move the focus (login form: name -> password) */
        focus_step(es, f, (mods & EV_MOD_SHIFT) != 0);
    }
    if (action != EV_RELEASE && key_is_enter(key)) fire_ex(es, f, "keypress", key, 0, 1);
    return 1;
}

int events_char(EventSystem *es, uint32_t codepoint)
{
    HtmlElem f = es->focus;
    if (f && !alive(es, f)) { es->focus = 0; f = 0; }
    if (!f) return 0;
    if (codepoint >= 0x20 && codepoint != 0x7f) {
        if (!fire_ex(es, f, "keypress", es->last_key, 0, 1)) return 1; /* handler cancelled insertion */
    }
    if (es->widgets.chr) es->widgets.chr(es->widgets.user, f, codepoint);
    return 1;
}

/* ------------------------------------------------------------------------- */
/* Resize observers + post-layout                                             */
/* ------------------------------------------------------------------------- */

void events_observe_resize(EventSystem *es, HtmlElem e, HtmlResizeFn fn, void *user)
{
    if (!e || !fn) return;
    for (int i = 0; i < es->observer_count; i++) {
        EvResizeObserver *o = &es->observers[i];
        if (o->elem == e && o->fn == fn && o->user == user) return;
    }
    if (es->observer_count == es->observer_cap) {
        es->observer_cap = es->observer_cap ? es->observer_cap * 2 : 8;
        es->observers = realloc(es->observers, (size_t)es->observer_cap * sizeof(*es->observers));
    }
    EvResizeObserver *o = &es->observers[es->observer_count++];
    memset(o, 0, sizeof(*o));
    o->elem = e;
    o->fn = fn;
    o->user = user;
}

void events_unobserve_resize(EventSystem *es, HtmlElem e)
{
    int w = 0;
    for (int i = 0; i < es->observer_count; i++) {
        if (es->observers[i].elem != e) es->observers[w++] = es->observers[i];
    }
    es->observer_count = w;
}

static int rect_eq(const HtmlRect *a, const HtmlRect *b)
{
    return a->x == b->x && a->y == b->y && a->w == b->w && a->h == b->h;
}

void events_after_layout(EventSystem *es)
{
    if (es->in_after_layout) return;
    es->in_after_layout = 1;
    for (int i = 0; i < es->observer_count; i++) {
        EvResizeObserver *o = &es->observers[i];
        HtmlRect r = {0, 0, 0, 0};
        if (!alive(es, o->elem)) {
            es->observers[i] = es->observers[--es->observer_count];
            i--;
            continue;
        }
        if (es->host.elem_rect) {
            if (!es->host.elem_rect(es->host.user, o->elem, &r)) r = (HtmlRect){0, 0, 0, 0};
        }
        if (o->seen && rect_eq(&o->last, &r)) continue;
        o->last = r;
        o->seen = 1;
        HtmlElem e = o->elem;
        HtmlResizeFn fn = o->fn;
        void *user = o->user;
        fn(e, user); /* may observe/unobserve: re-index from scratch is unnecessary, bounds re-checked by the loop */
    }
    if (es->has_mouse) update_hover(es);
    es->in_after_layout = 0;
}

/* ------------------------------------------------------------------------- */
/* Lifecycle                                                                  */
/* ------------------------------------------------------------------------- */

void events_init(EventSystem *es, Dom *dom, const EventsHost *host)
{
    memset(es, 0, sizeof(*es));
    es->dom = dom;
    if (host) es->host = *host;
    es->cursor = CSS_CURSOR_AUTO;
}

void events_set_widget_hooks(EventSystem *es, const EventsWidgetHooks *hooks)
{
    if (hooks) es->widgets = *hooks;
    else memset(&es->widgets, 0, sizeof(es->widgets));
}

void events_fini(EventSystem *es)
{
    handler_table_free(es);
    free(es->observers);
    es->observers = NULL;
    es->observer_count = es->observer_cap = 0;
}

/* Is `e` equal to `root` or inside it?  Valid while the tree is intact. */
static int is_inside(EventSystem *es, HtmlElem e, HtmlElem root)
{
    for (; e && alive(es, e); e = dom_parent(es->dom, e)) if (e == root) return 1;
    return 0;
}

void events_elem_removed(EventSystem *es, HtmlElem root)
{
    if (!root) return;
    if (es->focus && is_inside(es, es->focus, root)) {
        /* no blur event: the element is going away (Chrome fires none either) */
        if (es->widgets.blur) es->widgets.blur(es->widgets.user, es->focus);
        es->focus = 0;
    }
    if (es->capture && is_inside(es, es->capture, root)) es->capture = 0;
    if (es->down_target && is_inside(es, es->down_target, root)) es->down_target = 0;
    if (es->hover_len && is_inside(es, es->hover_chain[0], root)) {
        /* keep the surviving ancestors hovered; no mouseout/leave events
         * for a vanishing element (Chrome fires none either) */
        int k = 0;
        while (k < es->hover_len && is_inside(es, es->hover_chain[k], root)) k++;
        memmove(es->hover_chain, es->hover_chain + k, (size_t)(es->hover_len - k) * sizeof(es->hover_chain[0]));
        es->hover_len -= k;
    }
    if (es->active_len && is_inside(es, es->active_chain[0], root)) {
        int w = 0;
        for (int i = 0; i < es->active_len; i++) {
            if (!is_inside(es, es->active_chain[i], root)) es->active_chain[w++] = es->active_chain[i];
        }
        es->active_len = w;
    }
    for (int i = 0; i < es->observer_count; i++) {
        if (is_inside(es, es->observers[i].elem, root)) es->observers[i--] = es->observers[--es->observer_count];
    }
    for (uint32_t i = 0; i < es->handler_cap; i++) {
        EvHandlerEntry *ent = &es->handlers[i];
        if (ent->elem && is_inside(es, ent->elem, root)) {
            free(ent->text);
            ent->text = NULL;
            ent->elem = 0; /* tombstone: type stays non-NULL */
            es->handler_count--;
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Host over layout.h                                                         */
/* ------------------------------------------------------------------------- */

#ifndef KN_EVENTS_STANDALONE

static Box *lh_hit_test(void *user, float x, float y)
{
    EventsLayoutLink *l = user;
    return (l->tree && *l->tree) ? layout_hit_test(*l->tree, x, y, 1) : NULL;
}

static HtmlElem lh_box_elem(void *user, Box *box)
{
    EventsLayoutLink *l = user;
    for (; box; box = box->parent) {
        if (box->node && box->node->type == LXB_DOM_NODE_TYPE_ELEMENT) return dom_node_elem(l->dom, box->node);
    }
    return 0;
}

static Box *lh_elem_box(void *user, HtmlElem e)
{
    EventsLayoutLink *l = user;
    ElemData *d = dom_elem_data(l->dom, e);
    return d ? (Box *)d->box : NULL;
}

static int lh_elem_rect(void *user, HtmlElem e, HtmlRect *out)
{
    Box *box = lh_elem_box(user, e);
    return box ? layout_box_rect_in_viewport(box, out) : 0;
}

static int overflow_scrolls(uint8_t overflow) { return overflow == CSS_OVERFLOW_AUTO || overflow == CSS_OVERFLOW_SCROLL; }

static int lh_scroll_info(void *user, Box *box, EvScrollInfo *out)
{
    (void)user;
    if (!(box->flags & BOXF_SCROLL_CONTAINER) || !box->style) return 0;
    memset(out, 0, sizeof(*out));
    out->scroll_x = box->scroll_x;
    out->scroll_y = box->scroll_y;
    out->scroll_w = box->scroll_w;
    out->scroll_h = box->scroll_h;
    out->client_w = box_padding_w(box) - ((box->flags & BOXF_VBAR) ? box->bar_w : 0.0f);
    out->client_h = box_padding_h(box) - ((box->flags & BOXF_HBAR) ? box->bar_w : 0.0f);
    out->can_x = overflow_scrolls(box->style->overflow_x);
    out->can_y = overflow_scrolls(box->style->overflow_y);
    return out->can_x || out->can_y;
}

static void lh_set_scroll(void *user, Box *box, float x, float y)
{
    EventsLayoutLink *l = user;
    if (l->tree && *l->tree) layout_set_scroll(*l->tree, box, x, y);
}

/* layout_scrollbar (the painted geometry) in the box's layout space */
static int lh_scrollbar(void *user, Box *box, int vertical, float x, float y, EvScrollbar *out)
{
    EventsLayoutLink *l = user;
    LayoutScrollbar sb;
    if (!box || !layout_scrollbar(l->tree ? *l->tree : NULL, box, vertical, &sb)) return 0;
    float lx, ly;
    layout_point_to_box(box, x, y, &lx, &ly);
    out->inside = lx >= sb.track_x && lx < sb.track_x + sb.track_w && ly >= sb.track_y && ly < sb.track_y + sb.track_h;
    out->pos = vertical ? ly - sb.track_y : lx - sb.track_x;
    out->track_len = vertical ? sb.track_h : sb.track_w;
    out->thumb_pos = vertical ? sb.thumb_y - sb.track_y : sb.thumb_x - sb.track_x;
    out->thumb_len = vertical ? sb.thumb_h : sb.thumb_w;
    return 1;
}

void events_host_layout(EventsHost *out, EventsLayoutLink *link)
{
    memset(out, 0, sizeof(*out));
    out->user = link;
    out->hit_test = lh_hit_test;
    out->box_elem = lh_box_elem;
    out->elem_box = lh_elem_box;
    out->elem_rect = lh_elem_rect;
    out->scroll_info = lh_scroll_info;
    out->set_scroll = lh_set_scroll;
    out->scrollbar = lh_scrollbar;
}

#endif /* KN_EVENTS_STANDALONE */
