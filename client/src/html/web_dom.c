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
#include "klog.h"
#ifdef __EMSCRIPTEN__

#include "html_ui_backend.h"
#include <html_dom.h>
#include <alt_web.h> // the web vault self-test (client/src/alt/alt_web_test.c)
#include <flavor.h> // html_storage_prefix: the Krunker Strike app's own storage keys
#include <cJSON.h>
#include <emscripten.h>
#include <emscripten/html5.h> // WebGL contexts of the page canvases (html_dom_canvas_gl)
#include <gl.h>
#include <GLFW/glfw3.h>
#include <ui_logic.h>
#include <kio_checkout.h> // kio_checkout_window_focused (the page's focus)
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

// html_dom_map_text in two steps (one call per string would cross into C thousands of times):
// collect the text nodes / attributes under root as a JSON array of [text, owner id, slot] (the
// nodes are kept in knI18nNodes), then apply [[index, text], ...].
EM_JS(char *, kn_js_text_collect, (int root, const char *skip), {
    var top = root ? knEl(root) : document.documentElement;
    var sel = skip ? UTF8ToString(skip) : "";
    var out = [], nodes = [];
    if (!top) { window.knI18nNodes = nodes; return stringToNewUTF8("[]"); }
    var SKIP = { SCRIPT: 1, STYLE: 1, TEXTAREA: 1, NOSCRIPT: 1, TEMPLATE: 1 };
    var walk = function(el) {
        for (var c = el.firstChild; c; c = c.nextSibling) {
            if (c.nodeType === 3) {
                if (c.data) {
                    var slot = 0;
                    for (var p = c.previousSibling; p; p = p.previousSibling) slot++;
                    nodes.push([c, null]);
                    out.push([c.data, knId(el), slot]);
                }
            } else if (c.nodeType === 1) {
                if (SKIP[c.tagName]) continue;
                try { if (sel && c.matches(sel)) continue; } catch (e) {}
                visit(c);
            }
        }
    };
    var visit = function(el) {
        var attrs = ["title", "placeholder"];
        if (el.tagName === "INPUT" && /^(button|submit|reset)$/i.test(el.getAttribute("type") || "")) attrs.push("value");
        for (var i = 0; i < attrs.length; i++) {
            var v = el.getAttribute(attrs[i]);
            if (v) { nodes.push([el, attrs[i]]); out.push([v, knId(el), -1 - i]); }
        }
        walk(el);
    };
    visit(top);
    window.knI18nNodes = nodes;
    return stringToNewUTF8(JSON.stringify(out));
});

EM_JS(void, kn_js_text_apply, (const char *json), {
    var nodes = window.knI18nNodes || [];
    var list = [];
    try { list = JSON.parse(UTF8ToString(json)); } catch (e) {}
    for (var i = 0; i < list.length; i++) {
        var n = nodes[list[i][0]];
        if (!n) continue;
        if (n[1]) n[0].setAttribute(n[1], list[i][1]);
        else n[0].data = list[i][1];
    }
    window.knI18nNodes = null;
});

EM_JS(void, kn_js_set_lang, (const char *lang), {
    var l = UTF8ToString(lang);
    if (l) document.documentElement.setAttribute("lang", l);
    else document.documentElement.removeAttribute("lang");
});

EM_JS(char *, kn_js_get_text, (int id), {
    var el = knEl(id);
    return stringToNewUTF8(el ? (el.textContent || "") : "");
});

// html_dom_canvas_put: canvas.width/height = w/h (only when they differ: assigning clears the
// bitmap) and putImageData of a copy of the C pixels.
EM_JS(void, kn_js_canvas_put, (int id, const unsigned char *rgba, int w, int h), {
    var el = knEl(id);
    if (!el || !el.getContext) return;
    if (el.width !== w) el.width = w;
    if (el.height !== h) el.height = h;
    var ctx = el.getContext('2d');
    if (!ctx) return;
    ctx.putImageData(new ImageData(new Uint8ClampedArray(HEAPU8.buffer, rgba, w * h * 4).slice(), w, h), 0, 0);
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

EM_JS(int, kn_js_get_scroll, (int id, float *out), {
    var el = knEl(id);
    if (!el) return 0;
    var cs = getComputedStyle(el);
    if (cs.overflowX == 'visible' && cs.overflowY == 'visible') return 0;
    var v = [el.scrollLeft, el.scrollTop, el.scrollWidth, el.scrollHeight, el.clientWidth, el.clientHeight];
    for (var i = 0; i < 6; i++) HEAPF32[(out >> 2) + i] = v[i];
    return 1;
});

EM_JS(void, kn_js_set_scroll, (int id, float left, float top), {
    var el = knEl(id);
    if (el) { el.scrollLeft = left; el.scrollTop = top; }
});

// ---- text editing + code view (html_dom.h): offsets are UTF-8 bytes in C, UTF-16 indices
// in the page. The code view is a <div class="kn-code-overlay"> (gutter + <pre> of coloured
// spans + the caret-line bar) under the textarea, whose own text is transparent; the overlay
// follows its scroll, value and selection. window.__knText holds the helpers (installed once).
EM_JS(void, kn_js_text_init, (void), {
    if (window.__knText) return;
    var T = {};
    T.u8 = function (s, end) { // UTF-8 length of s[0, end)
        var n = 0;
        for (var i = 0; i < end && i < s.length; i++) {
            var c = s.charCodeAt(i);
            if (c < 0x80) n += 1;
            else if (c < 0x800) n += 2;
            else if (c >= 0xD800 && c < 0xDC00 && i + 1 < s.length) { n += 4; i++; }
            else n += 3;
        }
        return n;
    };
    T.u16 = function (s, bytes) { // UTF-16 index of UTF-8 offset `bytes`
        var n = 0, i = 0;
        while (i < s.length && n < bytes) {
            var c = s.charCodeAt(i);
            if (c < 0x80) n += 1;
            else if (c < 0x800) n += 2;
            else if (c >= 0xD800 && c < 0xDC00 && i + 1 < s.length) { n += 4; i++; }
            else n += 3;
            i++;
        }
        return i;
    };
    T.css = function (c) {
        c = c >>> 0;
        return 'rgba(' + (c >>> 24) + ',' + ((c >>> 16) & 255) + ',' + ((c >>> 8) & 255) + ',' + ((c & 255) / 255) + ')';
    };
    T.esc = function (s) { return s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;'); };
    T.style = null;
    // overlay geometry: the textarea's box, its padding-left = gutter + text pad
    T.place = function (el) {
        var st = el.__knCode;
        if (!st) return;
        var o = st.ov.style;
        o.left = el.offsetLeft + 'px'; o.top = el.offsetTop + 'px';
        o.width = el.offsetWidth + 'px'; o.height = el.offsetHeight + 'px';
        var lines = el.value.split('\n').length;
        var cfg = st.cfg;
        var gw = 0;
        if (cfg.gutter) {
            st.meas.textContent = String(lines);
            gw = cfg.pl + Math.max(cfg.minw, st.meas.getBoundingClientRect().width) + cfg.pr + cfg.extra;
        }
        st.gw = gw;
        st.gutter.style.width = gw + 'px';
        el.style.paddingLeft = (gw + cfg.gap + cfg.tpad) + 'px';
        st.text.style.left = (gw + cfg.gap) + 'px';
        st.pre.style.paddingLeft = cfg.tpad + 'px';
        T.scroll(el);
    };
    T.scroll = function (el) {
        var st = el.__knCode;
        if (!st) return;
        st.pre.style.transform = 'translate(' + (-el.scrollLeft) + 'px,' + (-el.scrollTop) + 'px)';
        st.gnum.style.transform = 'translateY(' + (-el.scrollTop) + 'px)';
        var v = el.value, a = el.selectionStart, b = el.selectionEnd;
        var line = v.slice(0, b).split('\n').length - 1;
        st.active.style.display = st.cfg.active && a === b ? '' : 'none';
        st.active.style.top = (st.padTop + line * st.lh - el.scrollTop) + 'px';
    };
    // the overlay's font and line box follow the textarea's (font-size changes with the
    // script font setting)
    T.font = function (el) {
        var st = el.__knCode, cs = getComputedStyle(el);
        var key = cs.fontStyle + ' ' + cs.fontWeight + ' ' + cs.fontSize + ' ' + cs.fontFamily;
        if (st.font === key) return;
        st.font = key;
        st.meas.style.font = key;          // the shorthand without the textarea's line-height
        st.meas.style.lineHeight = 'normal';
        st.meas.textContent = 'Xg';
        st.lh = Math.ceil(st.meas.getBoundingClientRect().height) || Math.ceil(parseFloat(cs.fontSize) * 1.2);
        el.style.lineHeight = st.lh + 'px';
        var font = 'font:' + getComputedStyle(el).font + ';tab-size:' + st.cfg.tab + ';-moz-tab-size:' + st.cfg.tab;
        st.pre.style.cssText = 'margin:0;white-space:pre;transform-origin:0 0;' + font + ';color:' + st.color + ';padding-top:' + st.padTop + 'px';
        st.gnum.style.cssText = font + ';padding-top:' + st.padTop + 'px;padding-right:' + (st.cfg.pr + st.cfg.extra) + 'px';
        st.active.style.height = st.lh + 'px';
    };
    // pre + gutter from the value, the spans and the line marks
    T.render = function (el) {
        var st = el.__knCode;
        if (!st) return;
        T.font(el);
        var v = el.value, lines = v.split('\n'), spans = st.spans || [], marks = st.marks || {};
        var html = [], gut = [], si = 0, bytes = 0;
        for (var ln = 0; ln < lines.length; ln++) {
            var text = lines[ln], out = '', pos = 0;
            var lb = bytes, le = bytes + T.u8(text, text.length);
            while (si < spans.length && spans[si][1] <= lb) si++;
            var k = si;
            while (pos < text.length) {
                var at = lb + T.u8(text, pos);
                while (k < spans.length && spans[k][1] <= at) k++;
                if (k < spans.length && spans[k][0] <= at) {
                    var end = Math.min(T.u16(text, spans[k][1] - lb), text.length);
                    if (end <= pos) end = pos + 1;
                    out += '<span style="color:' + T.css(spans[k][2]) + (spans[k][3] & 1 ? ';text-decoration:underline' : '') + '">' + T.esc(text.slice(pos, end)) + '</span>';
                    pos = end;
                } else {
                    var nx = k < spans.length ? Math.min(T.u16(text, spans[k][0] - lb), text.length) : text.length;
                    if (nx <= pos) nx = pos + 1;
                    out += T.esc(text.slice(pos, nx));
                    pos = nx;
                }
            }
            var m = marks[ln + 1];
            if (m && m.bg && !m.full) out = '<span style="background:' + T.css(m.bg) + '">' + out + '</span>';
            html.push('<div style="height:' + st.lh + 'px' + (m && m.bg && m.full ? ';background:' + T.css(m.bg) : '') + '">' + (out || ' ') + '</div>');
            gut.push('<div style="height:' + st.lh + 'px;position:relative">' + (ln + 1) +
                     (m && m.marker ? '<span style="position:absolute;left:100%;width:' + st.cfg.extra + 'px;text-align:center;color:' + T.css(m.mcol || st.cfg.gfg) + '">' + String.fromCodePoint(m.marker) + '</span>' : '') + '</div>');
            bytes = le + 1;
        }
        st.pre.innerHTML = html.join('');
        st.gnum.innerHTML = gut.join('');
        T.place(el);
    };
    T.view = function (el, cfg) {
        var st = el.__knCode;
        if (!cfg) {
            if (!st) return;
            st.ov.remove();
            el.style.background = el.style.color = el.style.caretColor = el.style.paddingLeft = el.style.lineHeight = '';
            el.classList.remove('kn-code-ta');
            el.removeEventListener('scroll', st.onscroll);
            delete el.__knCode;
            return;
        }
        if (!T.style) {
            T.style = document.createElement('style');
            document.head.appendChild(T.style);
        }
        T.style.textContent = '.kn-code-ta::selection{background:' + T.css(cfg.sel || 0x3390ffff) + '}';
        if (!st) {
            var cs = getComputedStyle(el);
            st = el.__knCode = { spans: [], marks: {} };
            st.ov = document.createElement('div');
            st.ov.className = 'kn-code-overlay';
            st.ov.style.cssText = 'position:absolute;overflow:hidden;pointer-events:none;box-sizing:border-box;background:' + cs.backgroundColor + ';z-index:' + (parseInt(cs.zIndex) || 0);
            st.gutter = document.createElement('div');
            st.gutter.style.cssText = 'position:absolute;left:0;top:0;bottom:0;overflow:hidden;text-align:right;box-sizing:border-box';
            st.gnum = document.createElement('div');
            st.gutter.appendChild(st.gnum);
            st.text = document.createElement('div');
            st.text.style.cssText = 'position:absolute;top:0;right:0;bottom:0;overflow:hidden';
            st.active = document.createElement('div');
            st.active.style.cssText = 'position:absolute;left:0;right:0';
            st.pre = document.createElement('pre');
            st.pre.style.cssText = 'margin:0;white-space:pre;transform-origin:0 0';
            st.text.appendChild(st.active);
            st.text.appendChild(st.pre);
            st.meas = document.createElement('span');
            // an inline-block: its height is the line box (line-height normal), not the glyph box
            st.meas.style.cssText = 'position:absolute;visibility:hidden;white-space:pre;display:inline-block;line-height:normal';
            st.ov.appendChild(st.gutter);
            st.ov.appendChild(st.text);
            st.ov.appendChild(st.meas);
            el.parentNode.insertBefore(st.ov, el);
            // one line box for both (line-height: normal differs between a textarea and a pre):
            // T.font, on every render
            st.padTop = parseFloat(cs.paddingTop) || 0;
            st.color = cs.color;
            st.font = null;
            el.style.background = 'transparent';
            el.style.color = 'transparent';
            el.style.position = el.style.position || cs.position;
            el.style.zIndex = (parseInt(cs.zIndex) || 0) + 1;
            el.style.overflow = 'auto'; // the browser's own scrolling (the native code view scrolls inside the widget)
            el.classList.add('kn-code-ta');
            st.onscroll = function () { T.scroll(el); };
            el.addEventListener('scroll', st.onscroll);
            el.addEventListener('input', function () { if (el.__knCode) T.render(el); });
            document.addEventListener('selectionchange', function () { if (el.__knCode && document.activeElement === el) T.scroll(el); });
        }
        st.cfg = cfg;
        st.gutter.style.background = T.css(cfg.gbg);
        st.gutter.style.color = T.css(cfg.gfg);
        st.gutter.style.display = cfg.gutter ? '' : 'none';
        st.font = null; // re-applied with this configuration
        st.active.style.background = T.css(cfg.active);
        el.style.caretColor = cfg.caret ? T.css(cfg.caret) : st.color;
        T.render(el);
    };
    window.__knText = T;
});

static void text_init(void) {
    static int done;
    if (!done) kn_js_text_init();
    done = 1;
}

EM_JS(int, kn_js_text_selection, (int id, int *out), {
    var el = knEl(id), T = window.__knText;
    if (!el || typeof el.selectionStart !== 'number') return 0;
    HEAP32[out >> 2] = T.u8(el.value, el.selectionStart);
    HEAP32[(out >> 2) + 1] = T.u8(el.value, el.selectionEnd);
    return 1;
});

EM_JS(void, kn_js_text_set_selection, (int id, int a, int b), {
    var el = knEl(id), T = window.__knText;
    if (!el || typeof el.setSelectionRange !== 'function') return;
    el.setSelectionRange(T.u16(el.value, a), T.u16(el.value, b));
    // keep the caret in view (setSelectionRange does not scroll an unfocused textarea)
    if (el.__knCode) {
        var st = el.__knCode, line = el.value.slice(0, el.selectionEnd).split('\n').length - 1, y = line * st.lh;
        if (y < el.scrollTop) el.scrollTop = y;
        else if (y + st.lh > el.scrollTop + el.clientHeight - 2 * st.padTop) el.scrollTop = y + st.lh - el.clientHeight + 2 * st.padTop;
        T.scroll(el);
    }
});

EM_JS(void, kn_js_text_replace, (int id, int a, int b, const char *text), {
    var el = knEl(id), T = window.__knText;
    if (!el || typeof el.setSelectionRange !== 'function') return;
    var t = UTF8ToString(text), v = el.value, ia = T.u16(v, a), ib = T.u16(v, b), ok = false;
    // insertText keeps the browser's undo stack; setRangeText when the field is not focused
    if (document.activeElement === el) {
        el.setSelectionRange(ia, ib);
        try { ok = t.length ? document.execCommand('insertText', false, t) : (ia === ib || document.execCommand('delete')); } catch (e) { ok = false; }
    }
    if (!ok) el.setRangeText(t, ia, ib, 'end');
    if (el.__knCode) T.render(el);
});

EM_JS(int, kn_js_text_caret_rect, (int id, float *out), {
    var el = knEl(id);
    if (!el || typeof el.selectionEnd !== 'number') return 0;
    var cs = getComputedStyle(el), st = el.__knCode;
    var v = el.value, pos = el.selectionEnd, ls = v.lastIndexOf('\n', pos - 1) + 1;
    var line = v.slice(0, ls).split('\n').length - 1, col = '', tab = st ? st.cfg.tab : 8;
    for (var i = ls; i < pos; i++) col += v[i] === '\t' ? ' '.repeat(tab - col.length % tab) : v[i];
    var c = window.__knText.cv || (window.__knText.cv = document.createElement('canvas').getContext('2d'));
    c.font = cs.font;
    var lh = st ? st.lh : (parseFloat(cs.lineHeight) || parseFloat(cs.fontSize) * 1.2);
    var r = el.getBoundingClientRect(), o = out >> 2;
    HEAPF32[o] = r.left + (parseFloat(cs.borderLeftWidth) || 0) + (parseFloat(cs.paddingLeft) || 0) + c.measureText(col).width - el.scrollLeft;
    HEAPF32[o + 1] = r.top + (parseFloat(cs.borderTopWidth) || 0) + (parseFloat(cs.paddingTop) || 0) + line * lh - el.scrollTop;
    HEAPF32[o + 2] = 1;
    HEAPF32[o + 3] = lh;
    return 1;
});

EM_JS(void, kn_js_code_view, (int id, int on, int tab, int gutter, uint32_t gfg, uint32_t gbg, float pl, float pr, float minw, float extra,
                              float gap, float tpad, uint32_t active, uint32_t caret, uint32_t sel), {
    var el = knEl(id);
    if (!el) return;
    window.__knText.view(el, on ? { tab: tab > 0 ? tab : 4, gutter: gutter, gfg: gfg >>> 0, gbg: gbg >>> 0, pl: pl, pr: pr, minw: minw,
                                    extra: extra, gap: gap, tpad: tpad, active: active >>> 0, caret: caret >>> 0, sel: sel >>> 0 } : null);
});

EM_JS(void, kn_js_code_spans, (int id, const int *data, int n), {
    var el = knEl(id);
    if (!el || !el.__knCode) return;
    var o = data >> 2, spans = [];
    for (var i = 0; i < n; i++, o += 4) spans.push([HEAP32[o], HEAP32[o + 1], HEAPU32[o + 2], HEAP32[o + 3]]);
    el.__knCode.spans = spans;
    window.__knText.render(el);
});

EM_JS(void, kn_js_code_lines, (int id, const int *data, int n), {
    var el = knEl(id);
    if (!el || !el.__knCode) return;
    var o = data >> 2, marks = {};
    for (var i = 0; i < n; i++, o += 5) marks[HEAP32[o]] = { bg: HEAPU32[o + 1], full: HEAP32[o + 2], marker: HEAPU32[o + 3], mcol: HEAPU32[o + 4] };
    el.__knCode.marks = marks;
    window.__knText.render(el);
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

// html_dom_is_shown: rendered, no fully transparent ancestor, and a non-empty area of its own
// (visibility: visible) or of a descendant (absolutely positioned HUD parts in empty holders).
EM_JS(int, kn_js_is_shown, (int id), {
    var el = knEl(id);
    if (!el || !el.getClientRects || el.getClientRects().length === 0) return 0;
    for (var p = el; p && p.nodeType === 1; p = p.parentElement) if (parseFloat(getComputedStyle(p).opacity) <= 0.01) return 0;
    var paints = function (e) {
        var r = e.getBoundingClientRect();
        return r.width > 0 && r.height > 0 && getComputedStyle(e).visibility === 'visible';
    };
    if (paints(el)) return 1;
    var all = el.getElementsByTagName('*');
    for (var i = 0; i < all.length; i++) if (paints(all[i]) && (!all[i].checkVisibility || all[i].checkVisibility({opacityProperty: true}))) return 1;
    return 0;
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

// cross-tab channel (html_channel_*): one BroadcastChannel per name, received messages queued
// for C to take
EM_JS(int, kn_js_channel_open, (const char *name), {
    try {
        var n = UTF8ToString(name);
        var all = Module.knChannels || (Module.knChannels = {});
        if (all[n]) return 1;
        if (typeof BroadcastChannel === "undefined") return 0;
        var c = {bc: new BroadcastChannel(n), q: []};
        c.bc.onmessage = function (e) { if (typeof e.data === "string" && c.q.length < 1024) c.q.push(e.data); };
        all[n] = c;
        return 1;
    } catch (e) { return 0; }
});

EM_JS(void, kn_js_channel_post, (const char *name, const char *text), {
    try {
        var c = (Module.knChannels || {})[UTF8ToString(name)];
        if (c) c.bc.postMessage(UTF8ToString(text));
    } catch (e) {}
});

EM_JS(char *, kn_js_channel_take, (const char *name), {
    var c = (Module.knChannels || {})[UTF8ToString(name)];
    var m = c ? c.q.shift() : undefined;
    return m === undefined ? 0 : stringToNewUTF8(m);
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

int html_dom_map_text(const HtmlElem root, const char *skip_selector, HtmlTextMapFn fn, void *user) {
    if (!fn) return 0;
    char *json = kn_js_text_collect((int) root, skip_selector ? skip_selector : "");
    cJSON *list = json ? cJSON_Parse(json) : NULL;
    free(json);
    cJSON *changes = cJSON_CreateArray();
    int count = 0, index = 0;
    const cJSON *item;
    cJSON_ArrayForEach(item, list) {
        const char *text = cJSON_GetStringValue(cJSON_GetArrayItem(item, 0));
        const HtmlElem owner = (HtmlElem) cJSON_GetNumberValue(cJSON_GetArrayItem(item, 1));
        const int slot = (int) cJSON_GetNumberValue(cJSON_GetArrayItem(item, 2));
        const char *r = text ? fn(user, owner, slot, text) : NULL;
        if (r && strcmp(r, text)) {
            cJSON *pair = cJSON_CreateArray();
            cJSON_AddItemToArray(pair, cJSON_CreateNumber(index));
            cJSON_AddItemToArray(pair, cJSON_CreateString(r));
            cJSON_AddItemToArray(changes, pair);
            count++;
        }
        index++;
    }
    char *out = cJSON_PrintUnformatted(changes);
    kn_js_text_apply(out ? out : "[]");
    free(out);
    cJSON_Delete(changes);
    cJSON_Delete(list);
    return count;
}

void html_dom_set_lang(const char *lang) { kn_js_set_lang(lang ? lang : ""); }

int html_dom_missing_glyphs(const HtmlElem e, uint32_t *first_cp) {
    (void) e;
    if (first_cp) *first_cp = 0;
    return -1; // the browser draws the text (its own font fallback)
}

void html_dom_set_attr(const HtmlElem e, const char *name, const char *value) {
    if (e && name) kn_js_set_attr((int) e, name, value ? value : "");
}

void html_dom_canvas_put(const HtmlElem e, const unsigned char *rgba, const int w, const int h) {
    if (e && rgba && w > 0 && h > 0) kn_js_canvas_put((int) e, rgba, w, h);
}

// ---- html_dom_canvas_gl: a GL render target shown in the page's own <canvas>
//
// The page is the browser's DOM above the WebGL canvas, and a 2D canvas cannot sample a GL texture.
// Reading the target back into the page (pixel-pack buffers, glReadPixels) cost a GPU readback every
// frame and made Chromium warn "READ-usage buffer was written, then fenced, but written again before
// being read back". So the page canvas is a WebGL canvas of its own: a second context, with alpha and
// premultiplied output like a 2D canvas, that the browser composites in place, and no pixels come back
// to the CPU. The main context's canvas has no alpha (main.c), so it cannot carry the target's alpha
// itself: the frame crosses it in two passes. Pass 1 draws the target's premultiplied colour into the
// canvas's box of the main canvas, pass 2 its alpha as grey. The own context uploads each pass straight
// from the main canvas (a GPU copy) and composes premultiplied RGBA from the two. The main canvas's
// pixels under the box are copied first and drawn back after, so the scene below the page keeps its
// look. Costs per frame: two canvas uploads, a few quads, one box copy; the element's bitmap is w x h.
#define WEB_GL_CANVASES 4
#define WEB_GL_CANVAS_STATS_EVERY 30

// GLSL ES 3.00 (the shader sources are authored as ES: web builds use them as they are).
static const char *const WEB_GL_QUAD_VS =
    "#version 300 es\n"
    "layout(location = 0) in vec2 p;\n"
    "void main() { gl_Position = vec4(p, 0.0, 1.0); }\n";

// Main context: the box (viewport) takes `src` (premultiplied RGBA, row 0 at the bottom, `size` px):
// mode 0 its colour (rgb), mode 1 its alpha as grey. Also restores a box from a copy of it.
static const char *const WEB_GL_PASS_FS =
    "#version 300 es\n"
    "precision highp float;\n"
    "uniform highp sampler2D src;\n"
    "uniform vec2 size;\n"
    "uniform vec2 origin;\n" // the box's bottom-left, in the drawing buffer's px
    "uniform int mode;\n"
    "out vec4 o;\n"
    "void main() {\n"
    "    vec4 t = texture(src, (floor(gl_FragCoord.xy - origin) + 0.5) / size);\n"
    "    o = vec4(mode == 1 ? vec3(t.a) : t.rgb, 1.0);\n"
    "}\n";

// Own context: the page canvas's pixel (x, y) from its bottom-left takes the main canvas's pixel
// (origin.x + x, origin.y + rows - 1 - y) (the main canvas's rows run from its top): colour from one
// upload, alpha (red) from the other.
static const char *const WEB_GL_COMPOSE_FS =
    "#version 300 es\n"
    "precision highp float;\n"
    "uniform highp sampler2D colour;\n"
    "uniform highp sampler2D alpha;\n"
    "uniform ivec2 origin;\n"
    "uniform int rows;\n"
    "out vec4 o;\n"
    "void main() {\n"
    "    ivec2 px = ivec2(gl_FragCoord.xy);\n"
    "    ivec2 at = ivec2(origin.x + px.x, origin.y + rows - 1 - px.y);\n"
    "    o = vec4(texelFetch(colour, at, 0).rgb, texelFetch(alpha, at, 0).r);\n"
    "}\n";

// The page's canvas: its own context (created on first use) and the composing resources in it.
typedef struct {
    HtmlElem e;
    EMSCRIPTEN_WEBGL_CONTEXT_HANDLE ctx;
    GLuint prog, vao, vbo, tex_colour, tex_alpha;
    GLint loc_colour, loc_alpha, loc_origin, loc_rows;
    int w, h;   // the bitmap's size
    int ok;     // context and resources exist
} WebGlCanvas;

// The main context: the box passes and the copy of the box that is drawn back after the frame.
typedef struct {
    EMSCRIPTEN_WEBGL_CONTEXT_HANDLE main;
    GLuint prog, vao, vbo, save_tex;
    GLint loc_src, loc_size, loc_origin, loc_mode;
    int save_w, save_h;
    int ok;
} WebGlMain;

static WebGlCanvas g_gl_canvases[WEB_GL_CANVASES];
static WebGlMain g_gl_main;
static struct { int frames; double total_ms, max_ms; } g_gl_canvas_stats;

// Marks the page's canvas with data-kn-gl=<slot>: emscripten_webgl_create_context looks it up by that selector.
EM_JS(void, kn_js_gl_mark, (int id, int slot), {
    var el = knEl(id);
    if (el && el.setAttribute) el.setAttribute('data-kn-gl', String(slot));
});
// The element's box in the main canvas's drawing buffer, top-left origin (-1 when unknown).
EM_JS(int, kn_js_gl_box_left, (int id), {
    var el = knEl(id), mc = Module['canvas'];
    if (!el || !mc || !el.getBoundingClientRect) return -1;
    var r = el.getBoundingClientRect(), m = mc.getBoundingClientRect();
    return Math.round((r.left - m.left) * (mc.width / (m.width || 1)));
});
EM_JS(int, kn_js_gl_box_top, (int id), {
    var el = knEl(id), mc = Module['canvas'];
    if (!el || !mc || !el.getBoundingClientRect) return -1;
    var r = el.getBoundingClientRect(), m = mc.getBoundingClientRect();
    return Math.round((r.top - m.top) * (mc.height / (m.height || 1)));
});
EM_JS(int, kn_js_gl_main_w, (void), { return Module['canvas'] ? Module['canvas'].width : 0; });
EM_JS(int, kn_js_gl_main_h, (void), { return Module['canvas'] ? Module['canvas'].height : 0; });
// The current context's bound texture (unit 0) <- the main canvas, as the browser composes it.
EM_JS(void, kn_js_gl_upload_main, (void), {
    var gl = GL.currentContext.GLctx;
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, Module['canvas']);
});
// The page canvas's bitmap size (assigning it clears the bitmap, so only when it changes).
EM_JS(void, kn_js_canvas_gl_size, (int id, int w, int h), {
    var el = knEl(id);
    if (!el) return;
    if (el.width !== w) el.width = w;
    if (el.height !== h) el.height = h;
});
// html_dom_canvas_gl's cost (tests read it): frames delivered, mean / max ms per frame.
EM_JS(void, kn_js_canvas_gl_stats, (int frames, double mean_ms, double max_ms, int w, int h), {
    window.__knCanvasGl = {frames: frames, meanMs: mean_ms, maxMs: max_ms, width: w, height: h};
});

// Current context switches; false when the handle is not a live context.
static int web_gl_use(const EMSCRIPTEN_WEBGL_CONTEXT_HANDLE ctx) {
    return ctx > 0 && emscripten_webgl_make_context_current(ctx) == EMSCRIPTEN_RESULT_SUCCESS;
}

// A linked program from the two sources; 0 on failure (logged).
static GLuint web_gl_program(const char *vs_src, const char *fs_src) {
    GLint ok = 0;
    char log[512];
    GLuint vs = glCreateShader(GL_VERTEX_SHADER), fs = glCreateShader(GL_FRAGMENT_SHADER), prog = glCreateProgram();
    glShaderSource(vs, 1, &vs_src, NULL);
    glCompileShader(vs);
    glGetShaderiv(vs, GL_COMPILE_STATUS, &ok);
    if (!ok) { glGetShaderInfoLog(vs, sizeof(log), NULL, log); fprintf(stderr, "[web_dom] canvas GL vertex shader: %s\n", log); }
    glShaderSource(fs, 1, &fs_src, NULL);
    glCompileShader(fs);
    glGetShaderiv(fs, GL_COMPILE_STATUS, &ok);
    if (!ok) { glGetShaderInfoLog(fs, sizeof(log), NULL, log); fprintf(stderr, "[web_dom] canvas GL fragment shader: %s\n", log); }
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glBindAttribLocation(prog, 0, "p");
    glLinkProgram(prog);
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) { glGetProgramInfoLog(prog, sizeof(log), NULL, log); fprintf(stderr, "[web_dom] canvas GL link: %s\n", log); }
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (ok) return prog;
    glDeleteProgram(prog);
    return 0;
}

// A VAO with the full-screen quad at attribute 0 (the caller's bindings are restored by the caller).
static void web_gl_quad(GLuint *vao, GLuint *vbo) {
    static const GLfloat quad[8] = {-1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f};
    glGenVertexArrays(1, vao);
    glBindVertexArray(*vao);
    glGenBuffers(1, vbo);
    glBindBuffer(GL_ARRAY_BUFFER, *vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
}

static void web_gl_texture(GLuint *tex) {
    glGenTextures(1, tex);
    glBindTexture(GL_TEXTURE_2D, *tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

// The main context's state a frame touches; put back after it (the page's render is next).
typedef struct {
    GLint draw_fbo, read_fbo, viewport[4], program, vao, vbo, active_tex, tex0;
    GLboolean scissor, blend, depth, cull, depth_mask, mask[4];
} WebGlSaved;

static void web_gl_save(WebGlSaved *s) {
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &s->draw_fbo);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &s->read_fbo);
    glGetIntegerv(GL_VIEWPORT, s->viewport);
    s->scissor = glIsEnabled(GL_SCISSOR_TEST);
    s->blend = glIsEnabled(GL_BLEND);
    s->depth = glIsEnabled(GL_DEPTH_TEST);
    s->cull = glIsEnabled(GL_CULL_FACE);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &s->depth_mask);
    glGetBooleanv(GL_COLOR_WRITEMASK, s->mask);
    glGetIntegerv(GL_CURRENT_PROGRAM, &s->program);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &s->vao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &s->vbo);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &s->active_tex);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &s->tex0);
    glActiveTexture((GLenum) s->active_tex);
}

static void web_gl_restore(const WebGlSaved *s) {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint) s->read_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint) s->draw_fbo);
    glViewport(s->viewport[0], s->viewport[1], s->viewport[2], s->viewport[3]);
    if (s->scissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    if (s->blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (s->depth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (s->cull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    glDepthMask(s->depth_mask);
    glColorMask(s->mask[0], s->mask[1], s->mask[2], s->mask[3]);
    glUseProgram((GLuint) s->program);
    glBindVertexArray((GLuint) s->vao);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint) s->vbo);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, (GLuint) s->tex0);
    glActiveTexture((GLenum) s->active_tex);
}

// The main context's passes, made once (the page's GL canvas is the current context on first use).
static int web_gl_main_init(void) {
    if (g_gl_main.ok) return 1;
    g_gl_main.main = emscripten_webgl_get_current_context();
    if (g_gl_main.main <= 0) return 0;
    g_gl_main.prog = web_gl_program(WEB_GL_QUAD_VS, WEB_GL_PASS_FS);
    if (!g_gl_main.prog) return 0;
    g_gl_main.loc_src = glGetUniformLocation(g_gl_main.prog, "src");
    g_gl_main.loc_size = glGetUniformLocation(g_gl_main.prog, "size");
    g_gl_main.loc_origin = glGetUniformLocation(g_gl_main.prog, "origin");
    g_gl_main.loc_mode = glGetUniformLocation(g_gl_main.prog, "mode");
    web_gl_quad(&g_gl_main.vao, &g_gl_main.vbo);
    glGenTextures(1, &g_gl_main.save_tex);
    g_gl_main.ok = 1;
    return 1;
}

// One page canvas's own context: created on first use, with its composing program and textures.
static int web_gl_canvas_init(WebGlCanvas *c) {
    if (c->ok) return 1;
    if (!c->ctx) {
        EmscriptenWebGLContextAttributes attrs;
        char selector[48];
        const int slot = (int) (c - g_gl_canvases);
        emscripten_webgl_init_context_attributes(&attrs);
        attrs.alpha = EM_TRUE;
        attrs.premultipliedAlpha = EM_TRUE;
        attrs.depth = EM_FALSE;
        attrs.stencil = EM_FALSE;
        attrs.antialias = EM_FALSE;
        attrs.preserveDrawingBuffer = EM_FALSE;
        attrs.majorVersion = 2;
        attrs.minorVersion = 0;
        kn_js_gl_mark((int) c->e, slot);
        snprintf(selector, sizeof(selector), "[data-kn-gl=\"%d\"]", slot);
        c->ctx = emscripten_webgl_create_context(selector, &attrs);
        if (c->ctx <= 0) { c->ctx = 0; return 0; }
    }
    if (!web_gl_use(c->ctx)) return 0;
    c->prog = web_gl_program(WEB_GL_QUAD_VS, WEB_GL_COMPOSE_FS);
    if (c->prog) {
        glUseProgram(c->prog);
        c->loc_colour = glGetUniformLocation(c->prog, "colour");
        c->loc_alpha = glGetUniformLocation(c->prog, "alpha");
        c->loc_origin = glGetUniformLocation(c->prog, "origin");
        c->loc_rows = glGetUniformLocation(c->prog, "rows");
        glUniform1i(c->loc_colour, 0);
        glUniform1i(c->loc_alpha, 1);
        web_gl_quad(&c->vao, &c->vbo);
        web_gl_texture(&c->tex_colour);
        web_gl_texture(&c->tex_alpha);
    }
    c->ok = c->prog != 0;
    web_gl_use(g_gl_main.main);
    return c->ok;
}

// The main canvas's copy of the box (w x h px at the bottom-left `gl_x, gl_y`, GL rows): a texture the
// frame draws back over the box once the page's pixels have been taken from it.
static void web_gl_save_box(const int gl_x, const int gl_y, const int w, const int h) {
    glActiveTexture(GL_TEXTURE0);
    if (g_gl_main.save_w != w || g_gl_main.save_h != h) {
        glBindTexture(GL_TEXTURE_2D, g_gl_main.save_tex);
        // RGB8: the default framebuffer has no alpha (main.c), and a copy needs the same format
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, w, h, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
        g_gl_main.save_w = w;
        g_gl_main.save_h = h;
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, g_gl_main.save_tex); // unit 0, as web_gl_restore puts it back
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, gl_x, gl_y, w, h);
}

// Draws the main context's box (viewport) from `src` (mode 0 colour, 1 alpha, see WEB_GL_PASS_FS).
static void web_gl_pass(const GLuint src, const int gl_x, const int gl_y, const int w, const int h, const int mode) {
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glViewport(gl_x, gl_y, w, h);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glUseProgram(g_gl_main.prog);
    glBindVertexArray(g_gl_main.vao);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, src);
    glUniform1i(g_gl_main.loc_src, 0);
    glUniform2f(g_gl_main.loc_size, (GLfloat) w, (GLfloat) h);
    glUniform2f(g_gl_main.loc_origin, (GLfloat) gl_x, (GLfloat) gl_y);
    glUniform1i(g_gl_main.loc_mode, mode);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

// tex 0: the page canvas's bitmap is transparent again.
static void web_gl_canvas_clear(WebGlCanvas *c) {
    if (!c->ok || !web_gl_use(c->ctx)) return;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, c->w, c->h);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    web_gl_use(g_gl_main.main);
}

static WebGlCanvas *web_gl_canvas(const HtmlElem e, const int create) {
    WebGlCanvas *free_slot = NULL;
    for (int i = 0; i < WEB_GL_CANVASES; i++) {
        if (g_gl_canvases[i].e == e) return &g_gl_canvases[i];
        if (!g_gl_canvases[i].e && !free_slot) free_slot = &g_gl_canvases[i];
    }
    if (!create || !free_slot) return NULL;
    memset(free_slot, 0, sizeof(*free_slot));
    free_slot->e = e;
    return free_slot;
}

void html_dom_canvas_gl(const HtmlElem e, const unsigned tex, const unsigned fbo, const int w, const int h) {
    (void) fbo; // the target is sampled by its texture (the main context is always current here)
    if (!e) return;
    if (!tex || w <= 0 || h <= 0) {
        WebGlCanvas *c = web_gl_canvas(e, 0);
        if (c) web_gl_canvas_clear(c);
        return;
    }
    if (!web_gl_main_init()) return;
    WebGlCanvas *c = web_gl_canvas(e, 1);
    if (!c || !web_gl_canvas_init(c)) return;

    const int left = kn_js_gl_box_left((int) e), top = kn_js_gl_box_top((int) e);
    const int page_w = kn_js_gl_main_w(), page_h = kn_js_gl_main_h();
    if (left < 0 || top < 0 || left + w > page_w || top + h > page_h) return; // the box is off the page canvas
    const int gl_x = left, gl_y = page_h - top - h; // the box's bottom-left, GL rows
    const double start = emscripten_get_now();
    if (c->w != w || c->h != h) {
        kn_js_canvas_gl_size((int) e, w, h);
        c->w = w;
        c->h = h;
    }

    WebGlSaved saved;
    web_gl_save(&saved);
    web_gl_save_box(gl_x, gl_y, w, h);
    // 1. the colour: premultiplied, over whatever the box holds (the copy is drawn back below)
    web_gl_pass(tex, gl_x, gl_y, w, h, 0);
    web_gl_use(c->ctx);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, c->tex_colour);
    kn_js_gl_upload_main();
    web_gl_use(g_gl_main.main);
    // 2. the alpha, as grey over the box
    web_gl_pass(tex, gl_x, gl_y, w, h, 1);
    web_gl_use(c->ctx);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, c->tex_alpha);
    kn_js_gl_upload_main();
    web_gl_use(g_gl_main.main);
    // 3. the scene under the box comes back
    web_gl_pass(g_gl_main.save_tex, gl_x, gl_y, w, h, 0);
    web_gl_restore(&saved);
    // 4. the page canvas composes colour and alpha into premultiplied RGBA
    web_gl_use(c->ctx);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, w, h);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(c->prog);
    glBindVertexArray(c->vao);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, c->tex_colour);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, c->tex_alpha);
    glActiveTexture(GL_TEXTURE0);
    glUniform2i(c->loc_origin, left, top);
    glUniform1i(c->loc_rows, h);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    web_gl_use(g_gl_main.main);

    const double ms = emscripten_get_now() - start;
    g_gl_canvas_stats.frames++;
    g_gl_canvas_stats.total_ms += ms;
    if (ms > g_gl_canvas_stats.max_ms) g_gl_canvas_stats.max_ms = ms;
    if (g_gl_canvas_stats.frames % WEB_GL_CANVAS_STATS_EVERY == 0) {
        kn_js_canvas_gl_stats(g_gl_canvas_stats.frames, g_gl_canvas_stats.total_ms / g_gl_canvas_stats.frames,
                              g_gl_canvas_stats.max_ms, w, h);
    }
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
int html_dom_get_scroll(const HtmlElem e, HtmlScroll *out) {
    float v[6] = {0};
    const int ok = e ? kn_js_get_scroll((int) e, v) : 0;
    if (out) *out = (HtmlScroll) {v[0], v[1], v[2], v[3], v[4], v[5]};
    return ok;
}
void html_dom_set_scroll(const HtmlElem e, const float left, const float top) { if (e) kn_js_set_scroll((int) e, left, top); }

int html_dom_text_selection(const HtmlElem e, int *start, int *end) {
    int v[2] = {0, 0};
    text_init();
    const int ok = e ? kn_js_text_selection((int) e, v) : 0;
    if (start) *start = v[0];
    if (end) *end = v[1];
    return ok;
}

void html_dom_text_set_selection(const HtmlElem e, const int start, const int end) {
    text_init();
    if (e) kn_js_text_set_selection((int) e, start, end);
}

void html_dom_text_replace(const HtmlElem e, const int start, const int end, const char *text) {
    text_init();
    if (e) kn_js_text_replace((int) e, start, end, text ? text : "");
}

int html_dom_text_caret_rect(const HtmlElem e, HtmlRect *out) {
    float v[4] = {0, 0, 0, 0};
    text_init();
    const int ok = e ? kn_js_text_caret_rect((int) e, v) : 0;
    if (out) {
        out->x = v[0];
        out->y = v[1];
        out->w = v[2];
        out->h = v[3];
    }
    return ok;
}

void html_dom_code_view(const HtmlElem e, const HtmlCodeView *c) {
    text_init();
    if (!e) return;
    if (!c) kn_js_code_view((int) e, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    else kn_js_code_view((int) e, 1, c->tab_size, c->gutter, c->gutter_fg, c->gutter_bg, c->gutter_pad_left, c->gutter_pad_right, c->gutter_min_w,
                         c->gutter_extra, c->gutter_gap, c->text_pad, c->active_line_bg, c->caret, c->selection_bg);
}

// The span / line arrays cross as plain int32 records (start, end, color, flags) /
// (line, bg, full, marker, marker_color).
void html_dom_code_spans(const HtmlElem e, const HtmlCodeSpan *spans, const int n) {
    text_init();
    if (!e) return;
    int *buf = malloc(sizeof(int) * 4 * (size_t) (n > 0 ? n : 1));
    if (!buf) return;
    for (int i = 0; i < n; i++) {
        buf[i * 4] = spans[i].start;
        buf[i * 4 + 1] = spans[i].end;
        buf[i * 4 + 2] = (int) spans[i].color;
        buf[i * 4 + 3] = spans[i].flags;
    }
    kn_js_code_spans((int) e, buf, n > 0 ? n : 0);
    free(buf);
}

void html_dom_code_lines(const HtmlElem e, const HtmlCodeLine *lines, const int n) {
    text_init();
    if (!e) return;
    int *buf = malloc(sizeof(int) * 5 * (size_t) (n > 0 ? n : 1));
    if (!buf) return;
    for (int i = 0; i < n; i++) {
        buf[i * 5] = lines[i].line;
        buf[i * 5 + 1] = (int) lines[i].bg;
        buf[i * 5 + 2] = lines[i].full;
        buf[i * 5 + 3] = (int) lines[i].marker;
        buf[i * 5 + 4] = (int) lines[i].marker_color;
    }
    kn_js_code_lines((int) e, buf, n > 0 ? n : 0);
    free(buf);
}

HtmlElem html_dom_element_at(const float x, const float y) { return (HtmlElem) kn_js_element_at(x, y); }
// the browser hit-tests pointer-events aware only (elementFromPoint): the closest it has
HtmlElem html_dom_painted_at(const float x, const float y) { return (HtmlElem) kn_js_element_at(x, y); }
// the page's DOM is always above the canvas: nothing the client draws in GL covers it
void html_dom_set_top_layer(const HtmlElem e) { (void) e; }
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

int html_dom_is_shown(const HtmlElem e) { return e ? kn_js_is_shown((int) e) : 0; }

float html_dom_offset_width(const HtmlElem e) { return e ? kn_js_offset_width((int) e) : 0.0f; }
float html_dom_offset_height(const HtmlElem e) { return e ? kn_js_offset_height((int) e) : 0.0f; }

void html_dom_viewport(float *w, float *h, float *dpr) {
    float v[3] = {0, 0, 1};
    kn_js_viewport(v);
    if (w) *w = v[0];
    if (h) *h = v[1];
    if (dpr) *dpr = v[2];
}

// The browser's devicePixelRatio applies on the web (native engine only).
void html_dom_set_device_pixel_ratio(const float dpr) { (void) dpr; }

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

// The page got the focus back (window focus, or document visibilitychange to visible; installed once by
// html_ui_backend_init): kio_checkout_window_focused (a payment waiting for the provider's tab reads the account again)
EM_JS(void, kn_js_focus_listen, (void), {
    if (window.__knFocusListen) return;
    window.__knFocusListen = 1;
    var back = function () { if (!document.visibilityState || document.visibilityState === 'visible') _kn_ui_window_focused(); };
    window.addEventListener('focus', back);
    document.addEventListener('visibilitychange', back);
});

EMSCRIPTEN_KEEPALIVE void kn_ui_window_focused(void) { kio_checkout_window_focused(); }

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
// A relative href ("./?game=...") resolves against the page's own address, not its <base> (the
// Krunker Strike page /strike/ has <base href="../">: platform/web/pwa.py), so it keeps the page.
EM_JS(void, kn_js_page_set_url, (const char *href, int push), {
    try {
        var u = new URL(UTF8ToString(href), location.href);
        var h = u.pathname + u.search + u.hash;
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

typedef struct {
    HtmlClipboardFn fn;
    void *user;
} ClipboardRead;

// navigator.clipboard.readText()'s answer (text malloc'd by stringToNewUTF8, 0 = refused)
EMSCRIPTEN_KEEPALIVE void kn_ui_clipboard_read_done(ClipboardRead *req, char *text) {
    if (req && req->fn) req->fn(req->user, text);
    free(text);
    free(req);
}

EM_JS(void, kn_js_clipboard_read, (void *req), {
    if (!navigator.clipboard || !navigator.clipboard.readText) {
        _kn_ui_clipboard_read_done(req, 0);
        return;
    }
    navigator.clipboard.readText().then(function (t) {
        _kn_ui_clipboard_read_done(req, stringToNewUTF8(t || ""));
    }).catch(function () { _kn_ui_clipboard_read_done(req, 0); });
});

void html_clipboard_read(const HtmlClipboardFn fn, void *user) {
    ClipboardRead *req = calloc(1, sizeof(*req));
    if (!req) {
        if (fn) fn(user, NULL);
        return;
    }
    req->fn = fn;
    req->user = user;
    kn_js_clipboard_read(req);
}

// ---- Web Push (html_dom.h): ui/sw.js is the page's service worker; the browser subscription is made
// through it. The answer is the PushSubscription's JSON (malloc'd by stringToNewUTF8, 0 = none), freed
// here like the clipboard's text. The promise settles once, whatever happens (the guard below).

typedef struct {
    HtmlPushFn fn;
    void *user;
} PushReq;

EMSCRIPTEN_KEEPALIVE void kn_ui_push_done(PushReq *req, char *json) {
    if (req && req->fn) req->fn(req->user, json);
    free(json);
    free(req);
}

EM_JS(int, kn_js_push_supported, (void), {
    return window.isSecureContext && ('serviceWorker' in navigator) && ('PushManager' in window) ? 1 : 0;
});

// the browser's subscription for the VAPID key (the existing one when there is one), as JSON
EM_JS(void, kn_js_push_subscribe, (void *req, const char *key), {
    var finished = false;
    var done = function (json) {
        if (finished) return;
        finished = true;
        _kn_ui_push_done(req, json ? stringToNewUTF8(json) : 0);
    };
    if (!window.isSecureContext || !('serviceWorker' in navigator) || !('PushManager' in window)) {
        done(null);
        return;
    }
    var b64 = UTF8ToString(key).replace(/-/g, '+').replace(/_/g, '/');
    while (b64.length % 4) b64 += '=';
    navigator.serviceWorker.register('sw.js').then(function () {
        return navigator.serviceWorker.ready;
    }).then(function (reg) {
        return reg.pushManager.getSubscription().then(function (sub) {
            if (sub) return sub;
            var raw = atob(b64), bytes = new Uint8Array(raw.length);
            for (var i = 0; i < raw.length; i++) bytes[i] = raw.charCodeAt(i);
            return reg.pushManager.subscribe({ userVisibleOnly: true, applicationServerKey: bytes });
        });
    }).then(function (sub) {
        done(sub ? JSON.stringify(sub) : null);
    }).catch(function () {
        done(null);
    });
});

// removes the browser's subscription; the one it removed comes back as JSON (NULL: there was none)
EM_JS(void, kn_js_push_unsubscribe, (void *req), {
    var finished = false;
    var done = function (json) {
        if (finished) return;
        finished = true;
        _kn_ui_push_done(req, json ? stringToNewUTF8(json) : 0);
    };
    if (!window.isSecureContext || !('serviceWorker' in navigator)) {
        done(null);
        return;
    }
    navigator.serviceWorker.getRegistration().then(function (reg) {
        return reg ? reg.pushManager.getSubscription() : null;
    }).then(function (sub) {
        if (!sub) return null;
        var json = JSON.stringify(sub);
        return sub.unsubscribe().catch(function () {}).then(function () { return json; });
    }).then(function (json) {
        done(json);
    }).catch(function () {
        done(null);
    });
});

int html_push_supported(void) { return kn_js_push_supported(); }

void html_push_subscribe(const char *vapid_public_key_b64url, const HtmlPushFn fn, void *user) {
    PushReq *req = calloc(1, sizeof(*req));
    if (!req) {
        if (fn) fn(user, NULL);
        return;
    }
    req->fn = fn;
    req->user = user;
    kn_js_push_subscribe(req, vapid_public_key_b64url ? vapid_public_key_b64url : "");
}

void html_push_unsubscribe(const HtmlPushFn fn, void *user) {
    PushReq *req = calloc(1, sizeof(*req));
    if (!req) {
        if (fn) fn(user, NULL);
        return;
    }
    req->fn = fn;
    req->user = user;
    kn_js_push_unsubscribe(req);
}

// ---- cross-tab channel

int html_channel_open(const char *name) { return name && *name ? kn_js_channel_open(name) : 0; }
void html_channel_post(const char *name, const char *text) {
    if (name && text) kn_js_channel_post(name, text);
}
char *html_channel_take(const char *name) { return name ? kn_js_channel_take(name) : NULL; }

// ---- storage

const char *html_storage_prefix(void) {
    return flavor_get() == FLAVOR_STRIKE ? HTML_STORAGE_STRIKE_PREFIX : "";
}

// "<prefix><key>" in buf (html_storage_prefix: the Strike app's own namespace); NULL when too long
static const char *storage_key(const char *key, char *buf, const size_t n) {
    const int len = snprintf(buf, n, "%s%s", html_storage_prefix(), key);
    return len > 0 && (size_t) len < n ? buf : NULL;
}

int html_storage_get(const char *key, char *buf, const size_t n) {
    char full[1024];
    if (!key || !*key || !storage_key(key, full, sizeof(full))) return take_string(NULL, buf, n);
    return take_string(kn_js_storage_get(full), buf, n);
}

void html_storage_set(const char *key, const char *value) {
    char full[1024];
    if (key && *key && storage_key(key, full, sizeof(full))) kn_js_storage_set(full, value);
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

// Returns bit 0 = 1 for default behaviour (0 = preventDefault: the handler returned false)
// and bit 1 = the handler asked for event.stopPropagation() (html_ui_stop_propagation, the
// native engine's stop for ancestor inline handlers; the shim calls it on the browser event).
// `handler` is either "<event type>|<inline handler name>" ("click|showWindow",
// "input|windows.1.searchList") or "listener:<elemId>:<type>" for the shim's
// addEventListener equivalents (elemId empty = window/document).
EMSCRIPTEN_KEEPALIVE int kn_ui_event(const char *handler, const char *args_json, const int target, const char *value,
                                     const int checked, const int key, const int button, const float x, const float y, const int mods) {
    if (!handler || !*handler) return 1;

    HtmlEvent ev = {0};
    ev.target = target > 0 ? (HtmlElem) target : 0;
    ev.value = value ? value : "";
    ev.checked = checked;
    ev.key = key;
    ev.button = button;
    ev.x = x;
    ev.y = y;
    ev.mods = mods; // GLFW_MOD_* bits from the event's shiftKey / ctrlKey / altKey / metaKey (kn-shim send)

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
    return (r ? 1 : 0) | (html_ui_take_stop_propagation() ? 2 : 0);
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

// the browser loads http(s) images itself
void html_ui_backend_set_remote_image_fetcher(const HtmlRemoteImageFetch fetch) { (void) fetch; }

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
    kn_js_focus_listen();
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

void html_ui_backend_render(UI *ui, const int layers) { (void) ui; (void) layers; }

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
    KN_LOG_DEBUG1("[kn-selftest] %s %s\n", ok ? "PASS" : "FAIL", what);
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
    KN_LOG_DEBUG1("[kn-selftest] start\n");
    // the web vault (client/src/alt/alt_web_test.c): on a throwaway IndexedDB database, never the page's own
    st_check(alt_web_selftest() == 0, "alt vault: SubtleCrypto seal and open, the stored key, the stored record, wrong key and tamper");

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
