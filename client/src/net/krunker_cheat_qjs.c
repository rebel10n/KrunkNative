/*
 * krunker_cheat_qjs.c - native eval backend for CHEAT_CHECK, on QuickJS.
 *
 * The web build evals cc payloads in the real page (krunker_cheat.c, EM_JS).
 * The native client has no JS engine, so it embeds QuickJS (client/lib/quickjs,
 * Fabrice Bellard's release 2026-06-04, MIT, see its LICENSE / VERSION). This
 * file and the engine are compiled into the native client_bin only: never into
 * server/ or shared/, and CMake drops both from the Emscripten build.
 *
 * Sandbox. The payload is untrusted remote code; every challenge gets:
 *   - a FRESH JSRuntime + JSContext, freed afterwards (no state carries over);
 *   - JS_SetMemoryLimit / JS_SetMaxStackSize (KRUNKER_CHEAT_QJS_* in
 *     krunker_cheat_internal.h): an allocation past the cap throws
 *     "out of memory" instead of growing the client;
 *   - a wall-clock deadline in an interrupt handler: an endless loop gets
 *     QuickJS's uncatchable "interrupted" error (the payload's own try/catch
 *     cannot swallow it) and the challenge yields no ccr;
 *   - only the ECMAScript intrinsics of JS_NewContext (no quickjs-libc: no
 *     std / os / bjson, no module loader, no require/import/process/fs, no
 *     timers or workers), Atomics.wait disabled (JS_SetCanBlock 0), and no
 *     pending-job pump, so promises the payload schedules never run;
 *   - the browser stand-ins below, written in JS inside the same sandbox.
 *
 * Browser stand-ins. A cc payload runs in the game page and may read the
 * environment: navigator (webdriver, userAgent, platform, languages,
 * hardwareConcurrency, plugins), window/self/top identity, document
 * (readyState, createElement, getElementById, querySelector), location,
 * screen / innerWidth / devicePixelRatio, performance.now / timeOrigin,
 * localStorage, atob/btoa, console, requestAnimationFrame, matchMedia, and
 * `Function.prototype.toString(fn)` containing "[native code]" (the game has
 * such a check at game.deob.js:327). The sandbox provides plausible values for
 * all of those, keyed to the build platform. Matching the live page:
 * navigator.webdriver is an own non-configurable, non-writable data property
 * whose value is undefined, because the game redefines it that way at startup
 * (game.deob.js:197663).
 *
 * What cannot be faked (a payload depending on it answers differently from a
 * browser, and the server may tell): a real DOM and the game's own page
 * contents and globals (FRVR, the game's window.* state), canvas/WebGL
 * fingerprints, Intl, WebAssembly, fetch/XHR/WebSocket, real event timing,
 * cookies and the user's real localStorage, host objects' prototypes and
 * brands (`navigator instanceof Navigator`, Object.prototype.toString tags),
 * and engine-specific error message text (QuickJS vs V8 wording flows into
 * {r:null,e:{_eval: msg}}).
 */

#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L /* clock_gettime under -std=c11 */
#endif

#include "krunker_cheat.h"
#include "krunker_cheat_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "../../lib/quickjs/quickjs.h" /* client/lib/quickjs: no include path, so its
                                         * cutils.h/list.h never shadow project headers */

/* ------------------------------------------------------------------ */
/* WHATWG UTF-8 decode (TextDecoder)                                   */
/* ------------------------------------------------------------------ */

static size_t put_utf8(char *out, uint32_t cp)
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xc0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3f));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xe0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
        out[2] = (char)(0x80 | (cp & 0x3f));
        return 3;
    }
    out[0] = (char)(0xf0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
    out[3] = (char)(0x80 | (cp & 0x3f));
    return 4;
}

/* The Encoding Standard's UTF-8 decoder: a leading BOM is dropped (the
 * TextDecoder default), each maximal invalid subpart becomes one U+FFFD and
 * the byte that broke a sequence is decoded again. Output is well-formed. */
size_t krunker_cheat_utf8_sanitize(const uint8_t *in, size_t len, char *out)
{
    size_t i = 0, o = 0;
    if (len >= 3 && in[0] == 0xef && in[1] == 0xbb && in[2] == 0xbf)
        i = 3;
    while (i < len) {
        uint8_t b = in[i];
        if (b < 0x80) {
            out[o++] = (char)b;
            i++;
            continue;
        }
        int need;
        uint32_t cp;
        uint8_t lower = 0x80, upper = 0xbf;
        if (b >= 0xc2 && b <= 0xdf) {
            need = 1;
            cp = b & 0x1fu;
        } else if (b >= 0xe0 && b <= 0xef) {
            need = 2;
            cp = b & 0x0fu;
            if (b == 0xe0) lower = 0xa0;
            if (b == 0xed) upper = 0x9f;
        } else if (b >= 0xf0 && b <= 0xf4) {
            need = 3;
            cp = b & 0x07u;
            if (b == 0xf0) lower = 0x90;
            if (b == 0xf4) upper = 0x8f;
        } else {
            o += put_utf8(out + o, 0xfffd);
            i++;
            continue;
        }
        size_t j = i + 1;
        int seen = 0;
        while (seen < need && j < len && in[j] >= lower && in[j] <= upper) {
            cp = (cp << 6) | (in[j] & 0x3fu);
            lower = 0x80;
            upper = 0xbf;
            j++;
            seen++;
        }
        o += put_utf8(out + o, seen == need ? cp : 0xfffd);
        i = j; /* on error j is the offending byte: it is decoded again */
    }
    return o;
}

/* ------------------------------------------------------------------ */
/* Deadline                                                            */
/* ------------------------------------------------------------------ */

static int64_t now_ms(void)
{
#ifdef _WIN32
    return (int64_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

static int cheat_interrupt(JSRuntime *rt, void *opaque)
{
    (void)rt;
    return now_ms() > *(const int64_t *)opaque; /* non-zero aborts the script */
}

/* ------------------------------------------------------------------ */
/* Browser stand-ins (run inside the sandbox before the payload)        */
/* ------------------------------------------------------------------ */

#if defined(_WIN32)
#define KN_CHEAT_UA_OS "Windows NT 10.0; Win64; x64"
#define KN_CHEAT_PLATFORM "Win32"
#define KN_CHEAT_UAD_PLATFORM "Windows"
#elif defined(__APPLE__)
#define KN_CHEAT_UA_OS "Macintosh; Intel Mac OS X 10_15_7"
#define KN_CHEAT_PLATFORM "MacIntel"
#define KN_CHEAT_UAD_PLATFORM "macOS"
#else
#define KN_CHEAT_UA_OS "X11; Linux x86_64"
#define KN_CHEAT_PLATFORM "Linux x86_64"
#define KN_CHEAT_UAD_PLATFORM "Linux"
#endif
#define KN_CHEAT_UA                                                            \
    "Mozilla/5.0 (" KN_CHEAT_UA_OS ") AppleWebKit/537.36 (KHTML, like Gecko) " \
    "Chrome/141.0.0.0 Safari/537.36"

/* __KN_CORES__ is set from C before this runs and deleted by it. */
static const char kBootstrap[] =
    "(function(){\n"
    "var g = globalThis, cores = g.__KN_CORES__ | 0 || 4;\n"
    "delete g.__KN_CORES__;\n"
    "var NATIVE = new WeakMap(), fts = Function.prototype.toString;\n"
    "function nat(fn, name){ NATIVE.set(fn, name); return fn; }\n"
    "var toString = function toString(){\n"
    "  var n = NATIVE.get(this);\n"
    "  return n !== undefined ? 'function ' + n + '() { [native code] }' : fts.call(this);\n"
    "};\n"
    "Object.defineProperty(Function.prototype, 'toString', {value: nat(toString, 'toString'), writable: true, configurable: true, enumerable: false});\n"
    "function fn(name, f){ return nat(f, name); }\n"
    "var noop = function(){};\n"
    "function stub(name, ret){ return fn(name, function(){ return ret; }); }\n"
    "g.window = g; g.self = g; g.top = g; g.parent = g; g.frames = g;\n"
    "var nav = {\n"
    "  userAgent: '" KN_CHEAT_UA "',\n"
    "  appVersion: '" KN_CHEAT_UA "'.slice(8),\n"
    "  appName: 'Netscape', appCodeName: 'Mozilla', product: 'Gecko', productSub: '20030107',\n"
    "  platform: '" KN_CHEAT_PLATFORM "', vendor: 'Google Inc.', vendorSub: '',\n"
    "  language: 'en-US', languages: Object.freeze(['en-US', 'en']),\n"
    "  hardwareConcurrency: cores, deviceMemory: 8, maxTouchPoints: 0,\n"
    "  onLine: true, cookieEnabled: true, pdfViewerEnabled: true, doNotTrack: null,\n"
    "  plugins: [], mimeTypes: [],\n"
    "  userAgentData: { brands: [{brand: 'Chromium', version: '141'}, {brand: 'Google Chrome', version: '141'}, {brand: 'Not?A_Brand', version: '8'}],\n"
    "                   mobile: false, platform: '" KN_CHEAT_UAD_PLATFORM "' },\n"
    "  javaEnabled: stub('javaEnabled', false), sendBeacon: stub('sendBeacon', true),\n"
    "  vibrate: stub('vibrate', false)\n"
    "};\n"
    /* the game: Object.defineProperty(navigator, 'webdriver', {configurable:false, writable:false}) */
    "Object.defineProperty(nav, 'webdriver', {configurable: false, writable: false, enumerable: false, value: undefined});\n"
    "g.navigator = g.clientInformation = nav;\n"
    "var loc = { href: 'https://krunker.io/', origin: 'https://krunker.io', protocol: 'https:',\n"
    "  host: 'krunker.io', hostname: 'krunker.io', port: '', pathname: '/', search: '', hash: '',\n"
    "  assign: fn('assign', noop), replace: fn('replace', noop), reload: fn('reload', noop),\n"
    "  toString: fn('toString', function(){ return this.href; }) };\n"
    "g.location = loc;\n"
    "g.screen = { width: 1920, height: 1080, availWidth: 1920, availHeight: 1040, availLeft: 0, availTop: 0,\n"
    "  colorDepth: 24, pixelDepth: 24, orientation: { type: 'landscape-primary', angle: 0 } };\n"
    "g.innerWidth = 1920; g.innerHeight = 969; g.outerWidth = 1920; g.outerHeight = 1040;\n"
    "g.screenX = 0; g.screenY = 0; g.scrollX = 0; g.scrollY = 0; g.pageXOffset = 0; g.pageYOffset = 0;\n"
    "g.devicePixelRatio = 1; g.isSecureContext = true; g.crossOriginIsolated = false;\n"
    "g.origin = loc.origin; g.name = ''; g.closed = false; g.length = 0;\n"
    "var t0 = Date.now() - 30000;\n"
    "g.performance = { timeOrigin: t0, now: fn('now', function(){ return Date.now() - t0; }),\n"
    "  mark: fn('mark', noop), measure: fn('measure', noop),\n"
    "  getEntries: stub('getEntries', []), getEntriesByType: fn('getEntriesByType', function(){ return []; }),\n"
    "  getEntriesByName: fn('getEntriesByName', function(){ return []; }),\n"
    "  timing: { navigationStart: t0 }, navigation: { type: 0, redirectCount: 0 } };\n"
    "function rect(){ return { x: 0, y: 0, width: 0, height: 0, top: 0, left: 0, right: 0, bottom: 0 }; }\n"
    "function elem(tag){ var e = {\n"
    "  tagName: String(tag || 'div').toUpperCase(), nodeName: String(tag || 'div').toUpperCase(), nodeType: 1,\n"
    "  id: '', className: '', innerHTML: '', innerText: '', textContent: '', value: '',\n"
    "  style: {}, dataset: {}, attributes: [], children: [], childNodes: [], parentNode: null,\n"
    "  classList: { add: noop, remove: noop, toggle: noop, contains: function(){ return false; } },\n"
    "  setAttribute: fn('setAttribute', noop), getAttribute: fn('getAttribute', function(){ return null; }),\n"
    "  removeAttribute: fn('removeAttribute', noop), hasAttribute: fn('hasAttribute', function(){ return false; }),\n"
    "  appendChild: fn('appendChild', function(c){ return c; }), removeChild: fn('removeChild', function(c){ return c; }),\n"
    "  insertBefore: fn('insertBefore', function(c){ return c; }), remove: fn('remove', noop),\n"
    "  addEventListener: fn('addEventListener', noop), removeEventListener: fn('removeEventListener', noop),\n"
    "  dispatchEvent: fn('dispatchEvent', function(){ return true; }),\n"
    "  querySelector: fn('querySelector', function(){ return null; }), querySelectorAll: fn('querySelectorAll', function(){ return []; }),\n"
    "  getElementsByTagName: fn('getElementsByTagName', function(){ return []; }),\n"
    "  getElementsByClassName: fn('getElementsByClassName', function(){ return []; }),\n"
    "  getBoundingClientRect: fn('getBoundingClientRect', rect), getClientRects: fn('getClientRects', function(){ return []; }),\n"
    "  getContext: fn('getContext', function(){ return null; }), toDataURL: fn('toDataURL', function(){ return 'data:,'; }),\n"
    "  focus: fn('focus', noop), blur: fn('blur', noop), click: fn('click', noop),\n"
    "  cloneNode: fn('cloneNode', function(){ return elem(tag); })\n"
    "}; return e; }\n"
    "var html = elem('html'), head = elem('head'), body = elem('body');\n"
    "g.document = {\n"
    "  nodeType: 9, nodeName: '#document', readyState: 'complete', visibilityState: 'visible', hidden: false,\n"
    "  title: 'Krunker', cookie: '', referrer: '', domain: 'krunker.io', URL: loc.href, documentURI: loc.href,\n"
    "  characterSet: 'UTF-8', charset: 'UTF-8', contentType: 'text/html', compatMode: 'CSS1Compat',\n"
    "  location: loc, documentElement: html, head: head, body: body, activeElement: body,\n"
    "  fullscreenElement: null, pointerLockElement: null, scripts: [], forms: [], images: [], links: [],\n"
    "  hasFocus: fn('hasFocus', function(){ return true; }),\n"
    "  createElement: fn('createElement', function(t){ return elem(t); }),\n"
    "  createElementNS: fn('createElementNS', function(n, t){ return elem(t); }),\n"
    "  createTextNode: fn('createTextNode', function(t){ var e = elem('#text'); e.nodeType = 3; e.textContent = String(t); return e; }),\n"
    "  getElementById: fn('getElementById', function(){ return null; }),\n"
    "  getElementsByClassName: fn('getElementsByClassName', function(){ return []; }),\n"
    "  getElementsByTagName: fn('getElementsByTagName', function(){ return []; }),\n"
    "  getElementsByName: fn('getElementsByName', function(){ return []; }),\n"
    "  querySelector: fn('querySelector', function(){ return null; }),\n"
    "  querySelectorAll: fn('querySelectorAll', function(){ return []; }),\n"
    "  addEventListener: fn('addEventListener', noop), removeEventListener: fn('removeEventListener', noop),\n"
    "  dispatchEvent: fn('dispatchEvent', function(){ return true; })\n"
    "};\n"
    "g.addEventListener = fn('addEventListener', noop); g.removeEventListener = fn('removeEventListener', noop);\n"
    "g.dispatchEvent = fn('dispatchEvent', function(){ return true; });\n"
    "var ids = 0;\n"
    "g.requestAnimationFrame = fn('requestAnimationFrame', function(){ return ++ids; });\n"
    "g.cancelAnimationFrame = fn('cancelAnimationFrame', noop);\n"
    "g.requestIdleCallback = fn('requestIdleCallback', function(){ return ++ids; });\n"
    "g.cancelIdleCallback = fn('cancelIdleCallback', noop);\n"
    "g.setTimeout = fn('setTimeout', function(){ return ++ids; }); g.clearTimeout = fn('clearTimeout', noop);\n"
    "g.setInterval = fn('setInterval', function(){ return ++ids; }); g.clearInterval = fn('clearInterval', noop);\n"
    "g.queueMicrotask = fn('queueMicrotask', noop);\n"
    "g.matchMedia = fn('matchMedia', function(q){ return { matches: false, media: String(q), onchange: null,\n"
    "  addListener: noop, removeListener: noop, addEventListener: noop, removeEventListener: noop }; });\n"
    "g.getComputedStyle = fn('getComputedStyle', function(){ return { getPropertyValue: function(){ return ''; } }; });\n"
    "function storage(){ var m = Object.create(null), s = {\n"
    "  getItem: fn('getItem', function(k){ k = String(k); return k in m ? m[k] : null; }),\n"
    "  setItem: fn('setItem', function(k, v){ m[String(k)] = String(v); }),\n"
    "  removeItem: fn('removeItem', function(k){ delete m[String(k)]; }),\n"
    "  clear: fn('clear', function(){ m = Object.create(null); }),\n"
    "  key: fn('key', function(i){ var k = Object.keys(m); return i < k.length ? k[i] : null; }) };\n"
    "  Object.defineProperty(s, 'length', { get: function(){ return Object.keys(m).length; } });\n"
    "  return s; }\n"
    "g.localStorage = storage(); g.sessionStorage = storage();\n"
    "g.console = {}; ['log','info','warn','error','debug','trace','dir','table','group','groupEnd','time','timeEnd','assert','count']\n"
    "  .forEach(function(k){ g.console[k] = fn(k, noop); });\n"
    "g.alert = fn('alert', noop); g.confirm = stub('confirm', false); g.prompt = stub('prompt', null);\n"
    "var B64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';\n"
    "function domerr(m){ var e = new Error(m); e.name = 'InvalidCharacterError'; return e; }\n"
    "g.btoa = fn('btoa', function(s){\n"
    "  s = String(s); var o = '';\n"
    "  for (var i = 0; i < s.length; i += 3) {\n"
    "    var a = s.charCodeAt(i), b = s.charCodeAt(i + 1), c = s.charCodeAt(i + 2);\n"
    "    if (a > 255 || b > 255 || c > 255) throw domerr(\"Failed to execute 'btoa' on 'Window': The string to be encoded contains characters outside of the Latin1 range.\");\n"
    "    o += B64[a >> 2] + B64[((a & 3) << 4) | (b >> 4)] + (i + 1 < s.length ? B64[((b & 15) << 2) | (c >> 6)] : '=') + (i + 2 < s.length ? B64[c & 63] : '=');\n"
    "  }\n"
    "  return o; });\n"
    "g.atob = fn('atob', function(s){\n"
    "  s = String(s).replace(/[\\t\\n\\f\\r ]/g, '');\n"
    "  if (s.length % 4 === 0) s = s.replace(/==?$/, '');\n"
    "  if (s.length % 4 === 1 || /[^A-Za-z0-9+\\/]/.test(s)) throw domerr(\"Failed to execute 'atob' on 'Window': The string to be decoded is not correctly encoded.\");\n"
    "  var o = '', acc = 0, n = 0;\n"
    "  for (var i = 0; i < s.length; i++) { acc = (acc << 6) | B64.indexOf(s[i]); n += 6; if (n >= 8) { n -= 8; o += String.fromCharCode((acc >> n) & 255); } }\n"
    "  return o; });\n"
    "})();\n";

/* The game's handler body (game.deob.js:218180-218194), run on __KN_DOC__.
 * __KN_RAW__ (test hook) skips JSON.parse. Both globals are removed before
 * the payload runs. Returns "\0U" for an undefined stringify, "\0J"+text
 * otherwise; anything that escapes (JSON.parse/stringify throwing, `.message`
 * of a null throw value, the uncatchable interrupt) becomes an exception. */
static const char kHandler[] =
    "(function(){\n"
    "var g = globalThis, doc = g.__KN_DOC__, raw = g.__KN_RAW__;\n"
    "delete g.__KN_DOC__; delete g.__KN_RAW__;\n"
    "var src = raw ? doc : JSON.parse(doc), R;\n"
    "try { R = (0, eval)(src); }\n"
    "catch (e) { R = { r: null, e: { _eval: e.message || 'eval_error' } }; }\n"
    "var out = JSON.stringify(R);\n"
    "return out === undefined ? '\\0U' : '\\0J' + String(out);\n"
    "})()";

static int cpu_count(void)
{
#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (int)si.dwNumberOfProcessors;
#elif defined(_SC_NPROCESSORS_ONLN)
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 4;
#else
    return 4;
#endif
}

/* One challenge in a fresh runtime. `text` must be well-formed UTF-8 (it may
 * contain NULs: it is passed with its length). */
static int run_sandboxed(const char *text, size_t len, int raw, char **json)
{
    *json = NULL;
    JSRuntime *rt = JS_NewRuntime();
    if (!rt)
        return -1;
    JS_SetMemoryLimit(rt, KRUNKER_CHEAT_QJS_MEMORY_LIMIT);
    JS_SetMaxStackSize(rt, KRUNKER_CHEAT_QJS_STACK_LIMIT);
    JS_SetCanBlock(rt, 0);
    int64_t deadline = now_ms() + KRUNKER_CHEAT_QJS_TIME_LIMIT_MS;
    JS_SetInterruptHandler(rt, cheat_interrupt, &deadline);

    int rc = -1;
    JSContext *ctx = JS_NewContext(rt);
    if (!ctx) {
        JS_FreeRuntime(rt);
        return -1;
    }

    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "__KN_CORES__", JS_NewInt32(ctx, cpu_count()));
    JSValue v = JS_Eval(ctx, kBootstrap, sizeof(kBootstrap) - 1, "<browser>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v)) {
        /* our own script: only a bug (or a too-small memory cap) gets here */
        JS_FreeValue(ctx, JS_GetException(ctx));
        goto done;
    }
    JS_FreeValue(ctx, v);

    /* The payload is handed over as a string value, never spliced into code. */
    JS_SetPropertyStr(ctx, global, "__KN_DOC__", JS_NewStringLen(ctx, text, len));
    JS_SetPropertyStr(ctx, global, "__KN_RAW__", JS_NewBool(ctx, raw));

    v = JS_Eval(ctx, kHandler, sizeof(kHandler) - 1, "<cc>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
    } else if (JS_IsString(v)) {
        size_t n = 0;
        const char *s = JS_ToCStringLen(ctx, &n, v);
        if (s && n >= 2 && s[0] == '\0' && s[1] == 'U') {
            rc = 0;
        } else if (s && n >= 2 && s[0] == '\0' && s[1] == 'J') {
            char *copy = (char *)malloc(n - 1);
            if (copy) {
                /* an embedded NUL can only come from a payload-replaced
                 * JSON.stringify; the C-string ccr path stops there */
                memcpy(copy, s + 2, n - 2);
                copy[n - 2] = '\0';
                *json = copy;
                rc = 0;
            }
        }
        if (s)
            JS_FreeCString(ctx, s);
    }
    JS_FreeValue(ctx, v);

done:
    JS_FreeValue(ctx, global);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return rc;
}

int krunker_cheat_eval(const uint8_t *plain, size_t len, char **json)
{
    *json = NULL;
    char *text = (char *)malloc(len * 3 + 1);
    if (!text)
        return -1;
    size_t n = krunker_cheat_utf8_sanitize(plain, len, text);
    int rc = run_sandboxed(text, n, 0, json);
    free(text);
    return rc;
}

int krunker_cheat_eval_source(const char *src, size_t len, char **json)
{
    return run_sandboxed(src, len, 1, json);
}
