// Emscripten backend of the DOM-ops API (client/include/html_dom.h): the browser
// page *is* the ui/ page (index.html, social.html, editor.html or viewer.html), so every html_dom_* call is an EM_JS function over the
// real DOM. Element handles are the numeric ids handed out by ui/js/kn-shim.js
// (knId(el): data-kn-id, lazily allocated, kept in a Map so detached elements
// survive). Page -> C goes through the exported kn_ui_event() / kn_ui_resize_observed()
// that the shim ccall()s; the handler table, listeners and timers live in
// html_dom_core.c.
//
// Link with -sEXPORTED_RUNTIME_METHODS=ccall,UTF8ToString,stringToNewUTF8 and
// -sEXPORTED_FUNCTIONS=...,_kn_ui_event,_kn_ui_resize_observed (EMSCRIPTEN_KEEPALIVE
// keeps the symbols; the export list makes them reachable as Module._kn_ui_event).
#ifdef __EMSCRIPTEN__

#include "html_ui_backend.h"
#include <html_dom.h>
#include <cJSON.h>
#include <emscripten.h>
#include <GLFW/glfw3.h>
#include <ui_logic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ------------------------------------------------------------------ JS side
// All helpers go through the shim's knId()/knEl(); an element id of 0 is "none".
// Strings come back as malloc'ed UTF-8 (stringToNewUTF8) and are freed in C.

EM_JS(int, kn_js_shim_present, (void), {
    return (typeof knId === 'function' && typeof knEl === 'function') ? 1 : 0;
});

EM_JS(int, kn_js_activate, (void), {
    return (typeof knActivate === 'function' && knActivate()) ? 1 : 0;
});

EM_JS(int, kn_js_root, (void), { return knId(document.documentElement); });
EM_JS(int, kn_js_body, (void), { return knId(document.body); });

EM_JS(int, kn_js_by_id, (const char *id), {
    var el = document.getElementById(UTF8ToString(id));
    return el ? knId(el) : 0;
});

EM_JS(int, kn_js_query, (int root, const char *sel), {
    var base = root ? knEl(root) : document;
    if (!base || !base.querySelector) return 0;
    var el = null;
    try { el = base.querySelector(UTF8ToString(sel)); } catch (e) { console.error('[kn] bad selector', UTF8ToString(sel), e); }
    return el ? knId(el) : 0;
});

EM_JS(int, kn_js_query_all, (int root, const char *sel, uint32_t *out, int max), {
    var base = root ? knEl(root) : document;
    if (!base || !base.querySelectorAll) return 0;
    var list;
    try { list = base.querySelectorAll(UTF8ToString(sel)); } catch (e) { console.error('[kn] bad selector', UTF8ToString(sel), e); return 0; }
    var n = 0;
    for (var i = 0; i < list.length && n < max; i++) HEAPU32[(out >> 2) + n++] = knId(list[i]);
    return n;
});

EM_JS(int, kn_js_parent, (int id), {
    var el = knEl(id);
    return el && el.parentNode && el.parentNode.nodeType === 1 ? knId(el.parentNode) : 0;
});

EM_JS(int, kn_js_first_child, (int id), {
    var el = knEl(id);
    return el && el.firstElementChild ? knId(el.firstElementChild) : 0;
});

EM_JS(int, kn_js_next, (int id), {
    var el = knEl(id);
    return el && el.nextElementSibling ? knId(el.nextElementSibling) : 0;
});

EM_JS(int, kn_js_closest, (int id, const char *sel), {
    var el = knEl(id);
    if (!el || !el.closest) return 0;
    var r = null;
    try { r = el.closest(UTF8ToString(sel)); } catch (e) { return 0; }
    return r ? knId(r) : 0;
});

EM_JS(int, kn_js_child_count, (int id), {
    var el = knEl(id);
    return el && el.children ? el.children.length : 0;
});

EM_JS(char *, kn_js_tag, (int id), {
    var el = knEl(id);
    return stringToNewUTF8(el && el.tagName ? el.tagName.toLowerCase() : "");
});

EM_JS(int, kn_js_create, (const char *tag, const char *id), {
    var el = document.createElement(UTF8ToString(tag));
    var s = id ? UTF8ToString(id) : "";
    if (s) el.id = s;
    return knId(el);
});

EM_JS(int, kn_js_create_text, (const char *text), {
    return knId(document.createTextNode(UTF8ToString(text)));
});

EM_JS(void, kn_js_append, (int parent, int child), {
    var p = knEl(parent), c = knEl(child);
    if (p && c) p.appendChild(c);
});

EM_JS(void, kn_js_insert_html, (int ref, int pos, const char *html), {
    var el = knEl(ref);
    if (!el || !el.insertAdjacentHTML) return;
    var where = ['beforebegin', 'afterbegin', 'beforeend', 'afterend'][pos] || 'beforeend';
    el.insertAdjacentHTML(where, UTF8ToString(html));
    if (typeof knForwardHandlers === 'function') knForwardHandlers(where === 'beforebegin' || where === 'afterend' ? el.parentNode : el);
});

EM_JS(void, kn_js_remove, (int id), {
    var el = knEl(id);
    if (el && el.parentNode) el.parentNode.removeChild(el);
});

EM_JS(void, kn_js_set_html, (int id, const char *html), {
    var el = knEl(id);
    if (!el) return;
    el.innerHTML = UTF8ToString(html);
    if (typeof knForwardHandlers === 'function') knForwardHandlers(el);
});

// Rewriting the same text still replaces the text node and dirties style/layout; the HUD
// re-sends unchanged values, so a lone text child that already holds `text` is left alone.
EM_JS(void, kn_js_set_text, (int id, const char *text), {
    var el = knEl(id);
    if (!el) return;
    var s = UTF8ToString(text);
    var c = el.firstChild;
    if (c && c === el.lastChild && c.nodeType === 3 && c.data === s) return;
    el.textContent = s;
});

EM_JS(char *, kn_js_get_text, (int id), {
    var el = knEl(id);
    return stringToNewUTF8(el ? (el.textContent || "") : "");
});

EM_JS(void, kn_js_set_attr, (int id, const char *name, const char *value), {
    var el = knEl(id);
    if (!el || !el.setAttribute) return;
    var attr = UTF8ToString(name);
    el.setAttribute(attr, UTF8ToString(value));
    if (/^on/i.test(attr) && typeof knForwardHandlers === 'function') knForwardHandlers(el);
});

EM_JS(void, kn_js_remove_attr, (int id, const char *name), {
    var el = knEl(id);
    if (el && el.removeAttribute) el.removeAttribute(UTF8ToString(name));
});

// null pointer when the attribute is absent.
EM_JS(char *, kn_js_get_attr, (int id, const char *name), {
    var el = knEl(id);
    if (!el || !el.hasAttribute || !el.hasAttribute(UTF8ToString(name))) return 0;
    return stringToNewUTF8(el.getAttribute(UTF8ToString(name)) || "");
});

EM_JS(void, kn_js_set_class, (int id, const char *cls, int on), {
    var el = knEl(id);
    if (el && el.classList) el.classList.toggle(UTF8ToString(cls), !!on);
});

EM_JS(int, kn_js_has_class, (int id, const char *cls), {
    var el = knEl(id);
    return el && el.classList && el.classList.contains(UTF8ToString(cls)) ? 1 : 0;
});

EM_JS(void, kn_js_set_display, (int id, const char *value), {
    var el = knEl(id);
    if (el && el.style) el.style.display = UTF8ToString(value);
});

EM_JS(void, kn_js_set_style, (int id, const char *prop, const char *value), {
    var el = knEl(id);
    if (!el || !el.style) return;
    var p = UTF8ToString(prop), v = UTF8ToString(value), prio = "";
    /* "x !important" (accepted natively by dom_set_style) -> setProperty priority.
     * No regex: EM_JS bodies lose their backslashes. */
    var t = v.trim(), bang = t.lastIndexOf("!");
    if (bang >= 0 && t.slice(bang + 1).trim().toLowerCase() === "important") { v = t.slice(0, bang).trim(); prio = "important"; }
    if (v === "") el.style.removeProperty(p);
    else el.style.setProperty(p, v, prio);
});

EM_JS(char *, kn_js_get_style, (int id, const char *prop), {
    var el = knEl(id);
    if (!el || !el.style) return 0;
    var v = el.style.getPropertyValue(UTF8ToString(prop));
    return v ? stringToNewUTF8(v) : 0;
});

EM_JS(void, kn_js_set_value, (int id, const char *value), {
    var el = knEl(id);
    if (el && 'value' in el) el.value = UTF8ToString(value);
});

EM_JS(char *, kn_js_get_value, (int id), {
    var el = knEl(id);
    return stringToNewUTF8(el && el.value != null ? String(el.value) : "");
});

EM_JS(void, kn_js_set_checked, (int id, int checked), {
    var el = knEl(id);
    if (el) el.checked = !!checked;
});

EM_JS(int, kn_js_get_checked, (int id), {
    var el = knEl(id);
    return el && el.checked ? 1 : 0;
});

EM_JS(void, kn_js_focus, (int id), { var el = knEl(id); if (el && el.focus) el.focus(); });
EM_JS(void, kn_js_blur, (int id), { var el = knEl(id); if (el && el.blur) el.blur(); });

EM_JS(void, kn_js_scroll_to_bottom, (int id), {
    var el = knEl(id);
    if (el) el.scrollTop = el.scrollHeight;
});

EM_JS(int, kn_js_element_at, (float x, float y), {
    var el = null;
    try { el = document.elementFromPoint(x, y); } catch (e) { return 0; }
    return el ? knId(el) : 0;
});

EM_JS(int, kn_js_active_element, (void), {
    var el = document.activeElement;
    return el && el !== document.body && el !== document.documentElement ? knId(el) : 0;
});

// Writes x,y,w,h to out[0..3]; returns 0 when the element has no layout box (display:none).
EM_JS(int, kn_js_get_rect, (int id, float *out), {
    var el = knEl(id);
    if (!el || !el.getBoundingClientRect) return 0;
    var r = el.getBoundingClientRect();
    var o = out >> 2;
    HEAPF32[o] = r.left; HEAPF32[o + 1] = r.top; HEAPF32[o + 2] = r.width; HEAPF32[o + 3] = r.height;
    return (el.getClientRects && el.getClientRects().length === 0) ? 0 : 1;
});

EM_JS(float, kn_js_offset_width, (int id), { var el = knEl(id); return el && el.offsetWidth != null ? el.offsetWidth : 0; });
EM_JS(float, kn_js_offset_height, (int id), { var el = knEl(id); return el && el.offsetHeight != null ? el.offsetHeight : 0; });

EM_JS(void, kn_js_viewport, (float *out), {
    var o = out >> 2;
    HEAPF32[o] = window.innerWidth; HEAPF32[o + 1] = window.innerHeight; HEAPF32[o + 2] = window.devicePixelRatio || 1;
});

EM_JS(void, kn_js_observe, (int id, int on), {
    if (typeof knObserve === 'function') knObserve(id, !!on);
});

EM_JS(char *, kn_js_storage_get, (const char *key), {
    try {
        var v = window.localStorage.getItem(UTF8ToString(key));
        return v === null ? 0 : stringToNewUTF8(v);
    } catch (e) { return 0; }
});

EM_JS(void, kn_js_storage_set, (const char *key, const char *value), {
    try {
        if (value) window.localStorage.setItem(UTF8ToString(key), UTF8ToString(value));
        else window.localStorage.removeItem(UTF8ToString(key));
    } catch (e) { /* private mode / quota: ignore like the page did */ }
});

EM_JS(int, kn_js_selftest_requested, (void), {
    return /[?&]kn_selftest=(1|true)(&|$)/.test(location.search) ? 1 : 0;
});

// ------------------------------------------------------------------ C side

static char *dup_string(const char *s) {
    if (!s) s = "";
    const size_t len = strlen(s);
    char *copy = malloc(len + 1);
    if (copy) memcpy(copy, s, len + 1);
    return copy;
}

// Copies a malloc'ed JS string into buf (truncating) and returns the full length; -1 for NULL.
static int take_string(char *js, char *buf, const size_t n) {
    if (!js) {
        if (buf && n) buf[0] = 0;
        return -1;
    }
    const size_t len = strlen(js);
    if (buf && n) {
        const size_t copy = len < n - 1 ? len : n - 1;
        memcpy(buf, js, copy);
        buf[copy] = 0;
    }
    free(js);
    return (int) len;
}

HtmlElem html_dom_root(void) { return (HtmlElem) kn_js_root(); }
HtmlElem html_dom_body(void) { return (HtmlElem) kn_js_body(); }
HtmlElem html_dom_by_id(const char *id) { return id ? (HtmlElem) kn_js_by_id(id) : 0; }
HtmlElem html_dom_query(const HtmlElem root, const char *selector) { return selector ? (HtmlElem) kn_js_query((int) root, selector) : 0; }

int html_dom_query_all(const HtmlElem root, const char *selector, HtmlElem *out, const int max) {
    if (!selector || !out || max <= 0) return 0;
    return kn_js_query_all((int) root, selector, out, max);
}

HtmlElem html_dom_parent(const HtmlElem e) { return e ? (HtmlElem) kn_js_parent((int) e) : 0; }
HtmlElem html_dom_first_child(const HtmlElem e) { return e ? (HtmlElem) kn_js_first_child((int) e) : 0; }
HtmlElem html_dom_next(const HtmlElem e) { return e ? (HtmlElem) kn_js_next((int) e) : 0; }
HtmlElem html_dom_closest(const HtmlElem e, const char *selector) { return e && selector ? (HtmlElem) kn_js_closest((int) e, selector) : 0; }
int html_dom_child_count(const HtmlElem e) { return e ? kn_js_child_count((int) e) : 0; }

int html_dom_tag(const HtmlElem e, char *buf, const size_t n) {
    if (!e) return take_string(NULL, buf, n);
    return take_string(kn_js_tag((int) e), buf, n);
}

HtmlElem html_dom_create(const char *tag, const char *id_or_null) {
    if (!tag || !*tag) return 0;
    return (HtmlElem) kn_js_create(tag, id_or_null);
}

HtmlElem html_dom_create_text(const char *text) { return (HtmlElem) kn_js_create_text(text ? text : ""); }
void html_dom_append(const HtmlElem parent, const HtmlElem child) { if (parent && child) kn_js_append((int) parent, (int) child); }

void html_dom_insert_html(const HtmlElem ref, const HtmlInsertPos pos, const char *html) {
    if (ref && html) kn_js_insert_html((int) ref, (int) pos, html);
}

void html_dom_remove(const HtmlElem e) { if (e) kn_js_remove((int) e); }
void html_dom_set_html(const HtmlElem e, const char *html) { if (e) kn_js_set_html((int) e, html ? html : ""); }
void html_dom_set_text(const HtmlElem e, const char *text) { if (e) kn_js_set_text((int) e, text ? text : ""); }

int html_dom_get_text(const HtmlElem e, char *buf, const size_t n) {
    if (!e) return take_string(NULL, buf, n);
    return take_string(kn_js_get_text((int) e), buf, n);
}

void html_dom_set_attr(const HtmlElem e, const char *name, const char *value) {
    if (e && name) kn_js_set_attr((int) e, name, value ? value : "");
}

void html_dom_remove_attr(const HtmlElem e, const char *name) { if (e && name) kn_js_remove_attr((int) e, name); }

int html_dom_get_attr(const HtmlElem e, const char *name, char *buf, const size_t n) {
    if (!e || !name) return take_string(NULL, buf, n);
    return take_string(kn_js_get_attr((int) e, name), buf, n);
}

void html_dom_set_class(const HtmlElem e, const char *cls, const int on) { if (e && cls && *cls) kn_js_set_class((int) e, cls, on); }
int html_dom_has_class(const HtmlElem e, const char *cls) { return e && cls ? kn_js_has_class((int) e, cls) : 0; }
void html_dom_set_display(const HtmlElem e, const char *value) { if (e) kn_js_set_display((int) e, value ? value : ""); }

void html_dom_set_style(const HtmlElem e, const char *prop, const char *value) {
    if (e && prop) kn_js_set_style((int) e, prop, value ? value : "");
}

int html_dom_get_style(const HtmlElem e, const char *prop, char *buf, const size_t n) {
    if (!e || !prop) return take_string(NULL, buf, n);
    return take_string(kn_js_get_style((int) e, prop), buf, n);
}

void html_dom_set_value(const HtmlElem e, const char *value) { if (e) kn_js_set_value((int) e, value ? value : ""); }

int html_dom_get_value(const HtmlElem e, char *buf, const size_t n) {
    if (!e) return take_string(NULL, buf, n);
    return take_string(kn_js_get_value((int) e), buf, n);
}

void html_dom_set_checked(const HtmlElem e, const int checked) { if (e) kn_js_set_checked((int) e, checked); }
int html_dom_get_checked(const HtmlElem e) { return e ? kn_js_get_checked((int) e) : 0; }
void html_dom_focus(const HtmlElem e) { if (e) kn_js_focus((int) e); }
void html_dom_blur(const HtmlElem e) { if (e) kn_js_blur((int) e); }
void html_dom_scroll_to_bottom(const HtmlElem e) { if (e) kn_js_scroll_to_bottom((int) e); }

HtmlElem html_dom_element_at(const float x, const float y) { return (HtmlElem) kn_js_element_at(x, y); }
HtmlElem html_dom_active_element(void) { return (HtmlElem) kn_js_active_element(); }

int html_dom_get_rect(const HtmlElem e, HtmlRect *out) {
    float v[4] = {0, 0, 0, 0};
    const int ok = e ? kn_js_get_rect((int) e, v) : 0;
    if (out) {
        out->x = v[0];
        out->y = v[1];
        out->w = v[2];
        out->h = v[3];
    }
    return ok;
}

float html_dom_offset_width(const HtmlElem e) { return e ? kn_js_offset_width((int) e) : 0.0f; }
float html_dom_offset_height(const HtmlElem e) { return e ? kn_js_offset_height((int) e) : 0.0f; }

void html_dom_viewport(float *w, float *h, float *dpr) {
    float v[3] = {0, 0, 1};
    kn_js_viewport(v);
    if (w) *w = v[0];
    if (h) *h = v[1];
    if (dpr) *dpr = v[2];
}

// ---- resize observers: the shim's ResizeObserver calls kn_ui_resize_observed(id); the ids
// are queued and the C callbacks run from html_ui_backend_update() (one per element per frame).

typedef struct {
    HtmlElem elem;
    HtmlResizeFn fn;
    void *user;
} ResizeEntry;

static ResizeEntry *g_observers;
static int g_observer_count, g_observer_capacity;
static HtmlElem *g_resize_queue;
static int g_resize_queue_count, g_resize_queue_capacity;

void html_dom_observe_resize(const HtmlElem e, const HtmlResizeFn fn, void *user) {
    if (!e) return;
    for (int i = 0; i < g_observer_count; i++) {
        if (g_observers[i].elem == e) {
            if (!fn) {
                g_observers[i] = g_observers[--g_observer_count];
                kn_js_observe((int) e, 0);
                return;
            }
            g_observers[i].fn = fn;
            g_observers[i].user = user;
            return;
        }
    }
    if (!fn) return;
    if (g_observer_count == g_observer_capacity) {
        const int cap = g_observer_capacity ? g_observer_capacity * 2 : 8;
        ResizeEntry *grown = realloc(g_observers, sizeof(ResizeEntry) * (size_t) cap);
        if (!grown) return;
        g_observers = grown;
        g_observer_capacity = cap;
    }
    g_observers[g_observer_count++] = (ResizeEntry) {e, fn, user};
    kn_js_observe((int) e, 1);
}

// controls.js keydown/keyup preventDefault while CONTROLS.enabled: the shim's window
// bubble listeners ask after GLFW's capture listener ran the key through C (ui_controls.c).
EMSCRIPTEN_KEEPALIVE int kn_ui_key_prevent(void) {
    return ui_controls_take_game_key();
}

EMSCRIPTEN_KEEPALIVE void kn_ui_resize_observed(const int id) {
    if (id <= 0) return;
    for (int i = 0; i < g_resize_queue_count; i++) {
        if (g_resize_queue[i] == (HtmlElem) id) return;
    }
    if (g_resize_queue_count == g_resize_queue_capacity) {
        const int cap = g_resize_queue_capacity ? g_resize_queue_capacity * 2 : 8;
        HtmlElem *grown = realloc(g_resize_queue, sizeof(HtmlElem) * (size_t) cap);
        if (!grown) return;
        g_resize_queue = grown;
        g_resize_queue_capacity = cap;
    }
    g_resize_queue[g_resize_queue_count++] = (HtmlElem) id;
}

static void flush_resize_queue(void) {
    while (g_resize_queue_count > 0) {
        const HtmlElem e = g_resize_queue[--g_resize_queue_count];
        for (int i = 0; i < g_observer_count; i++) {
            if (g_observers[i].elem == e) {
                g_observers[i].fn(e, g_observers[i].user);
                break;
            }
        }
    }
}

// ---- page address + clipboard (html_dom.h)

EM_JS(char *, kn_js_page_query, (void), {
    var q = "";
    try { q = location.search.replace(/^[?]/, ""); } catch (e) {}
    return stringToNewUTF8(q);
});

// v3.1.1 ClientManager.seek / toggleStrm: history.replaceState|pushState(document.title, document.title, href)
EM_JS(void, kn_js_page_set_url, (const char *href, int push), {
    try {
        var h = UTF8ToString(href);
        var cur = location.pathname + location.search + location.hash;
        if (h === cur) return;
        if (push) history.pushState(document.title, document.title, h);
        else history.replaceState(document.title, document.title, h);
    } catch (e) {}
});

EM_JS(char *, kn_js_page_origin, (void), {
    var o = "";
    try { o = location.protocol + "//" + location.host; } catch (e) {}
    return stringToNewUTF8(o);
});

// UTILS.copyToClipboard: navigator.clipboard, else the hidden-textarea execCommand fallback
EM_JS(void, kn_js_clipboard_write, (const char *text), {
    var t = UTF8ToString(text);
    var fallback = function () {
        try {
            var a = document.createElement("textarea");
            a.value = t;
            a.style.position = "fixed";
            a.style.opacity = "0";
            document.body.appendChild(a);
            a.select();
            document.execCommand("copy");
            document.body.removeChild(a);
        } catch (e) {}
    };
    try {
        if (navigator.clipboard && navigator.clipboard.writeText) navigator.clipboard.writeText(t).catch(fallback);
        else fallback();
    } catch (e) { fallback(); }
});

int html_page_query(char *buf, const size_t n) { return take_string(kn_js_page_query(), buf, n); }
void html_page_set_url(const char *href, const int push) {
    if (href && *href) kn_js_page_set_url(href, push);
}
void html_page_origin(char *buf, const size_t n) {
    if (buf && n) take_string(kn_js_page_origin(), buf, n);
}
void html_clipboard_write(const char *text) {
    if (text) kn_js_clipboard_write(text);
}

// ---- storage

int html_storage_get(const char *key, char *buf, const size_t n) {
    if (!key || !*key) return take_string(NULL, buf, n);
    return take_string(kn_js_storage_get(key), buf, n);
}

void html_storage_set(const char *key, const char *value) {
    if (key && *key) kn_js_storage_set(key, value);
}

// ---- page -> C

// Last dispatch, kept for the self-test and for debugging from the console.
static char g_last_handler[128];
static int g_last_argc;
static HtmlArg g_last_args[8];
static char g_last_arg_text[8][64];
static char g_last_listener[64];
static int g_last_listener_key;

static void remember_dispatch(const char *name, const HtmlArg *args, const int argc) {
    snprintf(g_last_handler, sizeof(g_last_handler), "%s", name ? name : "");
    g_last_argc = argc < 8 ? argc : 8;
    for (int i = 0; i < g_last_argc; i++) {
        g_last_args[i] = args[i];
        snprintf(g_last_arg_text[i], sizeof(g_last_arg_text[i]), "%s", args[i].str ? args[i].str : "");
        g_last_args[i].str = g_last_arg_text[i];
    }
}

// args_json is a JSON array built by the shim: strings/numbers/booleans as themselves,
// {"kn":"this"} / {"kn":"event"} / {"kn":"this.value"} / {"kn":"this.checked"} markers.
static int parse_args(const char *args_json, HtmlArg *out, const int max) {
    if (!args_json || !*args_json) return 0;
    cJSON *root = cJSON_Parse(args_json);
    if (!root) {
        fprintf(stderr, "[ui] kn_ui_event: bad args json %s\n", args_json);
        return 0;
    }
    int n = 0;
    if (cJSON_IsArray(root)) {
        cJSON *item;
        cJSON_ArrayForEach(item, root) {
            if (n == max) break;
            HtmlArg *a = &out[n];
            a->num = 0;
            a->str = NULL;
            if (cJSON_IsNumber(item)) {
                a->kind = HTML_ARG_NUM;
                a->num = item->valuedouble;
            } else if (cJSON_IsBool(item)) {
                a->kind = HTML_ARG_BOOL;
                a->num = cJSON_IsTrue(item) ? 1 : 0;
            } else if (cJSON_IsString(item)) {
                a->kind = HTML_ARG_STR;
                a->str = dup_string(item->valuestring);
            } else if (cJSON_IsObject(item)) {
                const cJSON *kn = cJSON_GetObjectItemCaseSensitive(item, "kn");
                const cJSON *id = cJSON_GetObjectItemCaseSensitive(item, "id");
                const char *marker = cJSON_IsString(kn) ? kn->valuestring : "";
                if (!strcmp(marker, "event")) a->kind = HTML_ARG_EVENT;
                else if (!strcmp(marker, "this.value")) a->kind = HTML_ARG_THIS_VALUE;
                else if (!strcmp(marker, "this.checked")) a->kind = HTML_ARG_THIS_CHECKED;
                else a->kind = HTML_ARG_THIS;  // "this" or any other element
                if (cJSON_IsNumber(id)) a->num = id->valuedouble;
            } else {
                // null / undefined: an empty string keeps positional arguments aligned.
                a->kind = HTML_ARG_STR;
                a->str = dup_string("");
            }
            n++;
        }
    }
    cJSON_Delete(root);
    return n;
}

static void free_args(HtmlArg *args, const int n) {
    for (int i = 0; i < n; i++) {
        if (args[i].kind == HTML_ARG_STR) free((char *) args[i].str);
    }
}

// Mouse buttons and the wheel over the page never reach GLFW on the web (the emscripten
// port listens on the canvas only), so the shim forwards them here and they take the
// same route main.c's mouse_button_callback / scroll_callback take natively while the
// menu is open: the controls window's "Press any Key" capture (M2/M4/M5, SCROLL).
// Returns 1 when the event was consumed (the page must preventDefault).
static int web_menu_pointer(const HtmlEvent *ev) {
    Client *client = menu_ui()->client;
    if (!strcmp(ev->type, "mousedown")) {
        // DOM MouseEvent.button -> GLFW button, like libglfw.js (1 middle <-> 2 right swap).
        const int glfw = ev->button == 1 ? GLFW_MOUSE_BUTTON_MIDDLE : ev->button == 2 ? GLFW_MOUSE_BUTTON_RIGHT : ev->button;
        return ui_controls_mouse_event(client, glfw, GLFW_PRESS, 0);
    }
    if (!strcmp(ev->type, "wheel")) {
        const double dy = ev->value ? strtod(ev->value, NULL) : 0.0;  // GLFW sign: > 0 = up
        return dy != 0.0 && ui_controls_scroll_event(client, dy, 0);
    }
    return 0;
}

// Returns 1 for default behaviour, 0 to preventDefault (handler returned false).
// `handler` is either "<event type>|<inline handler name>" ("click|showWindow",
// "input|windows.1.searchList") or "listener:<elemId>:<type>" for the shim's
// addEventListener equivalents (elemId empty = window/document).
EMSCRIPTEN_KEEPALIVE int kn_ui_event(const char *handler, const char *args_json, const int target, const char *value,
                                     const int checked, const int key, const int button, const float x, const float y) {
    if (!handler || !*handler) return 1;

    HtmlEvent ev = {0};
    ev.target = target > 0 ? (HtmlElem) target : 0;
    ev.value = value ? value : "";
    ev.checked = checked;
    ev.key = key;
    ev.button = button;
    ev.x = x;
    ev.y = y;

    if (!strncmp(handler, "listener:", 9)) {
        char elem_id[64];
        const char *p = handler + 9;
        const char *colon = strchr(p, ':');
        if (!colon) return 1;
        const size_t len = (size_t) (colon - p) < sizeof(elem_id) - 1 ? (size_t) (colon - p) : sizeof(elem_id) - 1;
        memcpy(elem_id, p, len);
        elem_id[len] = 0;
        ev.type = colon + 1;
        snprintf(g_last_listener, sizeof(g_last_listener), "%s:%s", elem_id, ev.type);
        g_last_listener_key = key;
        if (!elem_id[0] && web_menu_pointer(&ev)) return 0;
        return html_ui_dispatch_listener(elem_id[0] ? elem_id : NULL, &ev);
    }

    // Inline handler: the shim prefixes the DOM event type ("input|setSetting"); a bare
    // name (direct call from the console) counts as a click.
    const char *bar = strchr(handler, '|');
    char type_buf[24] = "click";
    const char *name = handler;
    if (bar) {
        const size_t len = (size_t) (bar - handler) < sizeof(type_buf) - 1 ? (size_t) (bar - handler) : sizeof(type_buf) - 1;
        memcpy(type_buf, handler, len);
        type_buf[len] = 0;
        name = bar + 1;
    }
    ev.type = type_buf;

    HtmlArg args[16];
    const int argc = parse_args(args_json, args, 16);
    remember_dispatch(name, args, argc);
    const int r = html_ui_dispatch(name, &ev, args, argc);
    free_args(args, argc);
    return r;
}

// ------------------------------------------------------------------ pages (html_ui_load_page)
// On the web every page is its own document (index.html, social.html, editor.html,
// viewer.html; app_mode.c navigates the browser between them), so the only page this
// backend can "load" is the one the browser already shows.

EM_JS(int, kn_js_is_document, (const char *page), {
    var f = '';
    try { f = location.pathname.substring(location.pathname.lastIndexOf('/') + 1); } catch (e) {}
    if (f && f.indexOf('.') < 0) f += '.html'; // Cloudflare Pages serves social.html as /social
    return (f || 'index.html') === UTF8ToString(page) ? 1 : 0;
});

static int g_web_ready;
static int g_pending_resize;

int html_ui_backend_load_page(const char *page) {
    if (!page || !*page || !kn_js_is_document(page)) {
        fprintf(stderr, "[ui] web_dom: %s is not this document; pages are separate URLs on the web\n", page ? page : "");
        return 0;
    }
    g_pending_resize = 1;  // the page's resize listener sees the viewport once
    html_ui_page_ready();
    return 1;
}

// html_ui_add_stylesheet: a <link rel=stylesheet> appended to <head> once per href.
EM_JS(int, kn_js_add_stylesheet, (const char *href_ptr), {
    var href = UTF8ToString(href_ptr);
    var links = document.head.querySelectorAll('link[data-kn-sheet]');
    for (var i = 0; i < links.length; i++) if (links[i].getAttribute('data-kn-sheet') === href) return 1;
    var link = document.createElement('link');
    link.rel = 'stylesheet';
    link.href = href;
    link.setAttribute('data-kn-sheet', href);
    document.head.appendChild(link);
    return 1;
});

int html_ui_backend_add_stylesheet(const char *href) {
    return href && *href && kn_js_add_stylesheet(href);
}

// <style id=knCustomCSS> at the end of <head> (after every <link>), replaced or removed.
EM_JS(int, kn_js_set_custom_css, (const char *css_ptr), {
    var css = css_ptr ? UTF8ToString(css_ptr) : '';
    var el = document.getElementById('knCustomCSS');
    if (!css) { if (el) el.remove(); return 1; }
    if (!el) { el = document.createElement('style'); el.id = 'knCustomCSS'; }
    el.textContent = css;
    document.head.appendChild(el);
    return 1;
});

int html_ui_backend_set_custom_css(const char *css) {
    return kn_js_set_custom_css(css);
}

// ------------------------------------------------------------------ backend entry points

static int g_selftest_pending;

static void run_selftest(void);

int html_ui_backend_init(const char *ui_dir, const int width, const int height) {
    (void) ui_dir;
    (void) width;
    (void) height;
    if (!kn_js_shim_present()) {
        fprintf(stderr, "[ui] web_dom: ui/js/kn-shim.js is not loaded (knId/knEl missing); DOM ops will fail\n");
    }
    // kn-shim.js installs the page's handler globals and listeners now.
    if (!kn_js_activate()) fprintf(stderr, "[ui] web_dom: knActivate() failed (Module._kn_ui_event not exported?); page events will not reach C\n");
    g_web_ready = 1;
    g_pending_resize = 1;  // fired from the first update so listeners registered after init see it
    g_selftest_pending = kn_js_selftest_requested();
    html_ui_page_ready();  // the DOM already exists
    return 1;
}

void html_ui_backend_fini(void) {
    g_web_ready = 0;
}

void html_ui_backend_resize(const int width, const int height) {
    (void) width;
    (void) height;
}

void html_ui_backend_update(void) {
    if (!g_web_ready) return;
    if (g_pending_resize) {
        g_pending_resize = 0;
        HtmlEvent ev = {0};
        ev.type = "resize";
        ev.value = "";
        html_dom_viewport(&ev.x, &ev.y, NULL);
        html_ui_dispatch_listener(NULL, &ev);
    }
    flush_resize_queue();
    html_timers_tick(emscripten_get_now());
    if (g_selftest_pending) {
        g_selftest_pending = 0;
        run_selftest();
    }
}

void html_ui_backend_render(UI *ui) { (void) ui; }

void html_ui_backend_mouse_move(const double x, const double y) { (void) x; (void) y; }
void html_ui_backend_mouse_button(const int b, const int a, const int m) { (void) b; (void) a; (void) m; }
void html_ui_backend_scroll(const double dx, const double dy) { (void) dx; (void) dy; }
void html_ui_backend_key(const int k, const int s, const int a, const int m) { (void) k; (void) s; (void) a; (void) m; }
void html_ui_backend_char(const unsigned int c) { (void) c; }
int html_ui_backend_available(void) { return 1; }

// ------------------------------------------------------------------ self-test (?kn_selftest=1)
// Exercises every DOM op against the live page and prints PASS/FAIL lines to the console.

static int g_st_pass, g_st_fail;

static void st_check(const int ok, const char *what) {
    if (ok) g_st_pass++;
    else g_st_fail++;
    printf("[kn-selftest] %s %s\n", ok ? "PASS" : "FAIL", what);
}

static int g_st_timer_fired;
static void st_timer_cb(void *user) { g_st_timer_fired = *(int *) user; }
static int g_st_resize_fired;
static void st_resize_cb(const HtmlElem e, void *user) { (void) e; g_st_resize_fired = *(int *) user; }

EM_JS(void, kn_js_selftest_fire, (int holder, int input), {
    var h = knEl(holder), i = knEl(input);
    if (h) h.dispatchEvent(new MouseEvent('mousedown', {bubbles: true, cancelable: true, button: 0, clientX: 5, clientY: 6}));
    if (i) { i.value = 'v42'; i.dispatchEvent(new Event('input', {bubbles: true})); }
    window.dispatchEvent(new KeyboardEvent('keydown', {key: 'Escape', keyCode: 27, which: 27, bubbles: true, cancelable: true}));
});

EM_JS(void, kn_js_selftest_finish, (int pass, int fail), {
    window.__kn_selftest = {pass: pass, fail: fail, done: true};
    console.log('[kn-selftest] DONE pass=' + pass + ' fail=' + fail);
});

static void st_part2(void *user);

static void run_selftest(void) {
    char buf[256];
    printf("[kn-selftest] start\n");

    const HtmlElem body = html_dom_body();
    st_check(body != 0, "body handle");
    st_check(html_dom_root() != 0 && html_dom_parent(body) == html_dom_root(), "root/parent");
    html_dom_tag(body, buf, sizeof(buf));
    st_check(!strcmp(buf, "body"), "tag");

    const HtmlElem instr = html_dom_by_id("instructions");
    st_check(instr != 0, "by_id(#instructions)");
    st_check(html_dom_by_id("instructions") == instr, "by_id stable id");
    st_check(html_dom_query(0, "#instructionHolder") == html_dom_by_id("instructionHolder"), "query == by_id");
    st_check(html_dom_closest(instr, "#instructionHolder") == html_dom_by_id("instructionHolder"), "closest");

    // Scratch container (removed at the end).
    const HtmlElem box = html_dom_create("div", "knSelfTestBox");
    st_check(box != 0, "create");
    html_dom_append(body, box);
    st_check(html_dom_parent(box) == body, "append/parent");
    html_dom_set_html(box, "<span class=\"a\">one</span><span class=\"b\">two</span>");
    st_check(html_dom_child_count(box) == 2, "set_html/child_count");
    const HtmlElem first = html_dom_first_child(box);
    const HtmlElem second = html_dom_next(first);
    st_check(first && second && html_dom_has_class(second, "b"), "first_child/next/has_class");
    HtmlElem all[4];
    st_check(html_dom_query_all(box, "span", all, 4) == 2 && all[0] == first && all[1] == second, "query_all");
    html_dom_set_text(first, "hello & <world>");
    html_dom_get_text(first, buf, sizeof(buf));
    st_check(!strcmp(buf, "hello & <world>"), "set_text/get_text");
    st_check(html_dom_get_text(first, NULL, 0) == (int) strlen("hello & <world>"), "get_text length");
    html_dom_insert_html(second, HTML_INSERT_AFTER_END, "<i id=\"knStI\">x</i>");
    st_check(html_dom_child_count(box) == 3 && html_dom_next(second) == html_dom_by_id("knStI"), "insert_html");
    const HtmlElem text = html_dom_create_text("tn");
    html_dom_append(box, text);
    html_dom_get_text(box, buf, sizeof(buf));
    st_check(strstr(buf, "twoxtn") != NULL, "create_text/append");
    html_dom_remove(html_dom_by_id("knStI"));
    st_check(html_dom_child_count(box) == 2 && html_dom_by_id("knStI") == 0, "remove");

    html_dom_set_attr(box, "data-x", "y&\"z");
    st_check(html_dom_get_attr(box, "data-x", buf, sizeof(buf)) == 4 && !strcmp(buf, "y&\"z"), "set_attr/get_attr");
    html_dom_remove_attr(box, "data-x");
    st_check(html_dom_get_attr(box, "data-x", buf, sizeof(buf)) == -1, "remove_attr");
    html_dom_set_class(box, "kn-c", 1);
    st_check(html_dom_has_class(box, "kn-c"), "set_class on");
    html_dom_set_class(box, "kn-c", 0);
    st_check(!html_dom_has_class(box, "kn-c"), "set_class off");

    html_dom_set_display(box, "none");
    st_check(html_dom_get_style(box, "display", buf, sizeof(buf)) == 4 && !strcmp(buf, "none"), "set_display/get_style");
    HtmlRect r;
    st_check(html_dom_get_rect(box, &r) == 0, "get_rect display:none -> 0");
    html_dom_set_display(box, "");
    st_check(html_dom_get_style(box, "display", buf, sizeof(buf)) == -1, "set_display clear");
    html_dom_set_style(box, "width", "123px");
    html_dom_set_style(box, "--kn-test", "7");
    st_check(html_dom_get_style(box, "--kn-test", buf, sizeof(buf)) == 1 && !strcmp(buf, "7"), "set_style custom prop");
    st_check(html_dom_offset_width(box) == 123.0f, "offset_width");
    st_check(html_dom_get_rect(box, &r) == 1 && r.w == 123.0f && r.h == html_dom_offset_height(box), "get_rect");
    html_dom_set_style(box, "width", "");
    st_check(html_dom_get_style(box, "width", buf, sizeof(buf)) == -1, "set_style remove");

    float vw, vh, dpr;
    html_dom_viewport(&vw, &vh, &dpr);
    st_check(vw > 0 && vh > 0 && dpr > 0, "viewport");

    html_dom_insert_html(box, HTML_INSERT_BEFORE_END,
                         "<input id=\"knSelfTestInput\" oninput=\"setSetting('knSelfTest', this.value)\">"
                         "<input id=\"knSelfTestCheck\" type=\"checkbox\">");
    const HtmlElem input = html_dom_by_id("knSelfTestInput");
    const HtmlElem check = html_dom_by_id("knSelfTestCheck");
    html_dom_set_value(input, "abc");
    st_check(html_dom_get_value(input, buf, sizeof(buf)) == 3 && !strcmp(buf, "abc"), "set_value/get_value");
    html_dom_set_checked(check, 1);
    st_check(html_dom_get_checked(check) == 1, "set_checked");
    html_dom_set_checked(check, 0);
    st_check(html_dom_get_checked(check) == 0, "set_checked off");
    html_dom_focus(input);
    html_dom_blur(input);
    html_dom_scroll_to_bottom(box);

    html_storage_set("knSelfTest", "{\"v\":1}");
    st_check(html_storage_get("knSelfTest", buf, sizeof(buf)) == 7 && !strcmp(buf, "{\"v\":1}"), "storage set/get");
    html_storage_set("knSelfTest", NULL);
    st_check(html_storage_get("knSelfTest", buf, sizeof(buf)) == -1, "storage remove");

    // Events: synthetic mousedown on #instructionHolder, input on the inline setSetting
    // handler, Escape on window. All go page -> shim -> ccall -> here synchronously.
    g_last_handler[0] = 0;
    g_last_listener[0] = 0;
    kn_js_selftest_fire((int) html_dom_by_id("instructionHolder"), (int) input);
    st_check(!strcmp(g_last_handler, "setSetting") && g_last_argc == 2 && g_last_args[0].kind == HTML_ARG_STR &&
                     !strcmp(g_last_args[0].str, "knSelfTest") && g_last_args[1].kind == HTML_ARG_STR &&
                     !strcmp(g_last_args[1].str, "v42"),
             "inline setSetting('knSelfTest', this.value) reached C with parsed args");
    st_check(!strcmp(g_last_listener, ":keydown") && html_key_is_escape(g_last_listener_key),
             "window keydown Escape -> listener:keydown");

    // Timer and resize observer complete on later frames.
    static int timer_token = 7, resize_token = 9;
    g_st_timer_fired = 0;
    g_st_resize_fired = 0;
    html_dom_observe_resize(box, st_resize_cb, &resize_token);
    html_dom_set_style(box, "height", "40px");
    const int cancelled = html_timer_add(1, st_timer_cb, &timer_token);
    html_timer_cancel(cancelled);
    html_timer_add(30, st_timer_cb, &timer_token);
    html_timer_add(250, st_part2, NULL);
}

static void st_part2(void *user) {
    (void) user;
    st_check(g_st_timer_fired == 7, "timer fired (cancelled one did not)");
    st_check(g_st_resize_fired == 9, "observe_resize -> kn_ui_resize_observed -> callback");
    const HtmlElem box = html_dom_by_id("knSelfTestBox");
    html_dom_observe_resize(box, NULL, NULL);
    html_dom_remove(box);
    st_check(html_dom_by_id("knSelfTestBox") == 0, "cleanup");
    kn_js_selftest_finish(g_st_pass, g_st_fail);
}

#else
typedef int html_ui_web_dom_backend_not_compiled;
#endif
