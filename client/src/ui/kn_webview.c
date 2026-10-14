// The challenge window of kn_webview.h: WebKitGTK (webkit2gtk-4.1) on Linux when built with
// KN_WEBVIEW, a stub everywhere else. CMake builds this file into its own library (kn_webview) with
// WebKitGTK's include paths, so GTK's and FreeType's system headers never reach the rest of the client.
//
// What the window does (and does not):
//   - navigates to a REAL krunker.io URL (https only; plain http only to a loopback host, for the
//     tests' stub page) in an ephemeral web context (no cookies or storage on disk);
//   - injects one user script, which does nothing unless the top document's origin is that URL's; it
//     draws a card over the page and renders Cloudflare's widget with krunker.io's sitekey, loading
//     challenges.cloudflare.com's api.js like the live client (or using the page's own `turnstile`);
//   - the widget's callbacks hand "kind:value" strings to a second user script in an isolated script
//     world (BRIDGE_WORLD; through a DOM attribute + event on the top document), which alone can post to
//     the "knCaptcha" script-message handler: frames of other origins (and the page's own scripts'
//     window.webkit) cannot reach it. Only the first token is taken, only while the main frame is on
//     the expected origin; it goes to the caller (krunker_captcha_ui.c) once and is wiped here. It is
//     never logged;
//   - main-frame navigations away from the origin and every new window are refused;
//   - it never clicks, types, solves, rewrites requests or touches the challenge's scripts or frames.
#include <kn_webview.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(KN_WEBVIEW) && defined(__linux__) && !defined(__EMSCRIPTEN__)
#include <gtk/gtk.h>
#include <webkit2/webkit2.h>

#define KN_WEBVIEW_TOKEN_MAX 4096 // KRUNKER_CAPTCHA_TOKEN_MAX
#define TURNSTILE_API "https://challenges.cloudflare.com/turnstile/v0/api.js?render=explicit"
#define TURNSTILE_ORIGIN "https://challenges.cloudflare.com"
#define BRIDGE_WORLD "knCaptcha"      // the isolated script world holding the message handler
#define BRIDGE_ATTR "data-kn-captcha" // on <html>: the message, read and removed at once by the bridge
#define BRIDGE_EVENT "kn-captcha"

static struct {
    int gtk;            // 0 not tried, 1 up, -1 failed
    char gtk_err[160];
    GtkWidget *window;
    WebKitWebView *view;
    int closing;        // we are destroying the window (not the player)
    int taken;          // this window gave its token
    unsigned http_err;  // the page's HTTP status when >= 400 (its note stays in the modal)
    char origin[200];   // scheme://host[:port] of the page
    // pending events, polled in this order
    int ev_token, ev_failed, ev_closed, ev_status;
    char token[KN_WEBVIEW_TOKEN_MAX + 1];
    char msg[300];
} W;

static void wipe(void *p, size_t n) {
    volatile unsigned char *v = p;
    while (n--) *v++ = 0;
}

static void set_msg(const char *text) { snprintf(W.msg, sizeof(W.msg), "%s", text ? text : ""); }

// scheme://host[:port] of an absolute http(s) URL, lower-cased; 0 when it is not one.
static int origin_of(const char *url, char *out, const size_t n) {
    if (!url) return 0;
    size_t skip;
    if (!g_ascii_strncasecmp(url, "https://", 8)) skip = 8;
    else if (!g_ascii_strncasecmp(url, "http://", 7)) skip = 7;
    else return 0;
    size_t end = skip;
    while (url[end] && url[end] != '/' && url[end] != '?' && url[end] != '#') end++;
    if (end == skip || end >= n) return 0;
    for (size_t i = 0; i < end; i++) {
        const char c = url[i];
        if (!(g_ascii_isalnum(c) || c == '.' || c == '-' || c == ':' || c == '/' || c == '[' || c == ']')) return 0;
        out[i] = g_ascii_tolower(c);
    }
    out[end] = 0;
    return !strchr(out + skip, '@'); // (the character test above already refuses userinfo)
}

static int is_loopback_origin(const char *origin) {
    const char *h = strstr(origin, "://");
    if (!h) return 0;
    h += 3;
    return !strncmp(h, "127.0.0.1", 9) || !strncmp(h, "localhost", 9) || !strncmp(h, "[::1]", 5);
}

static int same_origin(const char *url) {
    char o[sizeof(W.origin)];
    return W.origin[0] && origin_of(url, o, sizeof(o)) && !strcmp(o, W.origin);
}

// A JS string literal (double quotes) of s.
static void js_string(GString *out, const char *s) {
    g_string_append_c(out, '"');
    for (const unsigned char *p = (const unsigned char *) s; p && *p; p++) {
        if (*p == '"' || *p == '\\') g_string_append_printf(out, "\\%c", *p);
        else if (*p < 0x20 || *p == '<' || *p == '>') g_string_append_printf(out, "\\u%04x", *p);
        else if (p[0] == 0xe2 && p[1] == 0x80 && (p[2] == 0xa8 || p[2] == 0xa9)) { // U+2028 / U+2029
            g_string_append_printf(out, "\\u202%c", p[2] == 0xa8 ? '8' : '9');
            p += 2;
        } else g_string_append_c(out, (char) *p);
    }
    g_string_append_c(out, '"');
}

// The user script: a card over the krunker.io page with Cloudflare's widget in it.
static char *user_script(const char *sitekey, const char *title) {
    GString *s = g_string_new("(function () {\n'use strict';\nvar ORIGIN = ");
    js_string(s, W.origin);
    g_string_append(s, ", SITEKEY = ");
    js_string(s, sitekey);
    g_string_append(s, ", TITLE = ");
    js_string(s, title);
    g_string_append(s, ", API = ");
    js_string(s, TURNSTILE_API);
    g_string_append(
        s,
        ";\n"
        "if (window.top !== window || location.origin !== ORIGIN || window.__knCaptcha) return;\n"
        "window.__knCaptcha = 1;\n"
        "var post = function (m) { var d = document.documentElement; d.setAttribute('" BRIDGE_ATTR "', m);"
        " document.dispatchEvent(new Event('" BRIDGE_EVENT "')); d.removeAttribute('" BRIDGE_ATTR "'); };\n"
        "var el = function (tag, css, text) { var e = document.createElement(tag); e.style.cssText = css;"
        " if (text) e.textContent = text; return e; };\n"
        "var shade = el('div', 'position:fixed;top:0;left:0;right:0;bottom:0;z-index:2147483646;background:#1b1b1b;"
        "display:flex;align-items:center;justify-content:center;font:15px/1.4 sans-serif;margin:0');\n"
        "var card = el('div', 'background:#fff;color:#222;padding:22px 24px;border-radius:6px;width:400px;max-width:90vw;"
        "text-align:center;box-shadow:0 4px 24px rgba(0,0,0,.5)');\n"
        "var head = el('div', 'font-size:22px;font-weight:bold;text-transform:uppercase;margin-bottom:6px', TITLE);\n"
        "var hint = el('div', 'color:#555;font-size:14px', 'Solve Cloudflare\\u2019s check for krunker.io below. Krunker Box Office"
        " takes the result once and closes this window.');\n"
        "var holder = el('div', 'min-height:70px;display:flex;align-items:center;justify-content:center;margin:14px 0 6px');\n"
        "var status = el('div', 'color:#555;font-size:13px;min-height:18px');\n"
        "var retry = el('button', 'display:none;margin:10px auto 0;padding:6px 16px;font-size:14px;cursor:pointer', 'Try again');\n"
        "var where = el('div', 'color:#999;font-size:12px;margin-top:10px', location.origin);\n"
        "card.appendChild(head); card.appendChild(hint); card.appendChild(holder); card.appendChild(status);"
        " card.appendChild(retry); card.appendChild(where); shade.appendChild(card);\n"
        "if (window._cf_chl_opt) { post('interstitial:'); return; }\n" // Cloudflare's own check of the visit: never covered
        "(document.body || document.documentElement).appendChild(shade);\n"
        "var say = function (t) { status.textContent = t || ''; };\n"
        "var widget = null, loading = null;\n"
        "var load = function () {\n"
        "  if (window.turnstile) return Promise.resolve();\n"
        "  if (!loading) loading = new Promise(function (ok, fail) {\n"
        "    var sc = document.createElement('script'); sc.src = API; sc.async = true;\n"
        "    sc.onload = function () { ok(); }; sc.onerror = function () { loading = null; fail(new Error('load')); };\n"
        "    (document.head || document.documentElement).appendChild(sc);\n"
        "  });\n"
        "  return loading;\n"
        "};\n"
        "var failed = function (t, m) { say(t); retry.style.display = ''; post(m); };\n"
        "var render = function () {\n"
        "  retry.style.display = 'none';\n"
        "  say('Loading Cloudflare\\u2019s challenge\\u2026');\n"
        "  load().then(function () {\n"
        "    var ts = window.turnstile;\n"
        "    if (!ts) { failed('Cloudflare\\u2019s challenge did not load.', 'error:noscript'); return; }\n"
        "    try { if (widget != null) ts.remove(widget); } catch (e) {}\n"
        "    widget = null;\n"
        "    try {\n"
        "      widget = ts.render(holder, {\n"
        "        sitekey: SITEKEY, appearance: 'always',\n"
        "        callback: function (t) { say('Done.'); post('token:' + t); },\n"
        "        'error-callback': function (c) { c = c == null ? 'unknown' : String(c);"
        " failed('Cloudflare error ' + c + '.', 'error:' + c); return true; },\n"
        "        'expired-callback': function () { failed('The challenge expired.', 'expired:'); },\n"
        "        'timeout-callback': function () { failed('The challenge timed out.', 'timeout:'); },\n"
        "        'before-interactive-callback': function () { say(''); post('interactive:'); }\n"
        "      });\n"
        "      if (widget == null) failed('Cloudflare\\u2019s challenge could not be shown.', 'error:render');\n"
        "      else { say(''); post('shown:'); }\n"
        "    } catch (e) { failed('Cloudflare\\u2019s challenge could not be shown.', 'error:render'); }\n"
        "  }, function () { failed('Cloudflare\\u2019s challenge did not load (blocked or offline).', 'error:noscript'); });\n"
        "};\n"
        "retry.onclick = render;\n"
        "render();\n"
        "})();\n");
    return g_string_free(s, FALSE);
}

// The bridge, in the isolated world BRIDGE_WORLD of the top frame: the only script that can post to
// the handler. It reads the main-world script's message off <html> (the DOM is shared between worlds).
static char *bridge_script(void) {
    GString *s = g_string_new("(function () {\n'use strict';\nvar ORIGIN = ");
    js_string(s, W.origin);
    g_string_append(s, ";\n"
                       "if (window.top !== window || location.origin !== ORIGIN) return;\n"
                       "var h = window.webkit && window.webkit.messageHandlers && window.webkit.messageHandlers.knCaptcha;\n"
                       "if (!h) return;\n"
                       "document.addEventListener('" BRIDGE_EVENT "', function () {\n"
                       "  var d = document.documentElement, m = d.getAttribute('" BRIDGE_ATTR "');\n"
                       "  d.removeAttribute('" BRIDGE_ATTR "');\n"
                       "  if (typeof m === 'string' && m.length <= 8192) h.postMessage(m);\n"
                       "}, true);\n"
                       "})();\n");
    return g_string_free(s, FALSE);
}

// ---------------------------------------------------------------- signals

static void on_message(WebKitUserContentManager *m, WebKitJavascriptResult *r, gpointer user) {
    (void) m;
    (void) user;
    if (!W.view) return;
    JSCValue *v = webkit_javascript_result_get_js_value(r);
    if (!v || !jsc_value_is_string(v)) return;
    char *s = jsc_value_to_string(v);
    if (!s) return;
    const size_t len = strlen(s);
    if (!same_origin(webkit_web_view_get_uri(W.view))) {
        printf("[webview] a message while the page is not on %s: ignored\n", W.origin);
    } else if (!strncmp(s, "token:", 6)) {
        const size_t n = len - 6;
        if (W.taken) {
            printf("[webview] a second token: ignored (one per window)\n");
        } else if (n == 0 || n > KN_WEBVIEW_TOKEN_MAX) {
            printf("[webview] the widget answered something that is not a token (%zu chars)\n", n);
            W.ev_status = 1;
            set_msg("Cloudflare answered something that is not a token.");
        } else {
            memcpy(W.token, s + 6, n + 1);
            W.taken = 1;
            W.ev_token = 1;
            printf("[webview] the challenge was solved: a token (%zu chars)\n", n);
        }
    } else if (!strncmp(s, "error:", 6)) {
        char code[17];
        snprintf(code, sizeof(code), "%s", s + 6);
        for (char *c = code; *c; c++)
            if (!g_ascii_isalnum(*c)) *c = '_';
        printf("[webview] the widget reported error %s\n", code);
        char text[200];
        if (!strncmp(code, "1101", 4) || !strncmp(code, "1102", 4)) {
            snprintf(text, sizeof(text), "Cloudflare refused krunker.io's challenge on %s (error %s).", W.origin, code);
            W.ev_failed = 1;
        } else if (!strcmp(code, "noscript")) {
            snprintf(text, sizeof(text), "Cloudflare's challenge did not load in the window (blocked or offline).");
            W.ev_status = 1;
        } else {
            snprintf(text, sizeof(text), "The challenge failed (Cloudflare error %s): \"Try again\" in its window.", code);
            W.ev_status = 1;
        }
        set_msg(text);
    } else if (!strncmp(s, "expired:", 8) || !strncmp(s, "timeout:", 8)) {
        printf("[webview] the challenge %s\n", s[0] == 'e' ? "expired" : "timed out");
        W.ev_status = 1;
        set_msg(s[0] == 'e' ? "The challenge expired: \"Try again\" in its window." : "The challenge timed out: \"Try again\" in its window.");
    } else if (!strncmp(s, "shown:", 6)) {
        printf("[webview] Cloudflare's widget is up\n");
        if (!W.http_err) {
            W.ev_status = 1;
            set_msg("Cloudflare's widget is up in the challenge window.");
        }
    } else if (!strncmp(s, "interstitial:", 13)) {
        printf("[webview] Cloudflare checks the visit to %s first\n", W.origin);
        W.ev_status = 1;
        set_msg("Cloudflare checks your visit to krunker.io first: follow it in the challenge window.");
    } else if (!strncmp(s, "interactive:", 12)) {
        W.ev_status = 1;
        set_msg("Cloudflare wants you to tick the box in the challenge window.");
    }
    wipe(s, len);
    g_free(s);
    // (the window is closed by the next kn_webview_captcha_poll, outside this signal)
}

static int nav_allowed(const char *uri) {
    if (!uri) return 0;
    if (!strncmp(uri, "about:", 6) || !strncmp(uri, "data:", 5) || !strncmp(uri, "blob:", 5)) return 1;
    char o[sizeof(W.origin)];
    if (!origin_of(uri, o, sizeof(o))) return 0;
    return !strcmp(o, W.origin) || !strcmp(o, TURNSTILE_ORIGIN);
}

static gboolean on_policy(WebKitWebView *view, WebKitPolicyDecision *d, const WebKitPolicyDecisionType type, gpointer user) {
    (void) view;
    (void) user;
    if (type == WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION) { // Cloudflare's "Privacy" / "Help" links
        printf("[webview] a new window was refused\n");
        webkit_policy_decision_ignore(d);
        return TRUE;
    }
    if (type == WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION) {
        WebKitNavigationAction *a = webkit_navigation_policy_decision_get_navigation_action(WEBKIT_NAVIGATION_POLICY_DECISION(d));
        WebKitURIRequest *req = a ? webkit_navigation_action_get_request(a) : NULL;
        const char *uri = req ? webkit_uri_request_get_uri(req) : NULL;
        if (!nav_allowed(uri)) {
            char o[sizeof(W.origin)];
            printf("[webview] a navigation to %s was refused\n", origin_of(uri, o, sizeof(o)) ? o : "(another scheme)");
            webkit_policy_decision_ignore(d);
            return TRUE;
        }
    }
    return FALSE; // WebKit's default
}

static gboolean on_load_failed(WebKitWebView *view, WebKitLoadEvent ev, gchar *uri, GError *error, gpointer user) {
    (void) view;
    (void) ev;
    (void) user;
    if (error && ((error->domain == WEBKIT_NETWORK_ERROR && error->code == WEBKIT_NETWORK_ERROR_CANCELLED) ||
                  error->domain == WEBKIT_POLICY_ERROR))
        return FALSE; // a navigation we refused / replaced
    if (!same_origin(uri)) return FALSE;
    printf("[webview] %s did not load: %s\n", W.origin, error ? error->message : "?");
    char text[300];
    snprintf(text, sizeof(text), "%s did not load in the challenge window (%s).", W.origin, error ? error->message : "error");
    set_msg(text);
    W.ev_failed = 1;
    return FALSE;
}

// krunker.io answers some networks with an error page ("ACCESS DENIED", 403): said in the modal (the
// widget still works on it, the page being krunker.io's; the matchmaker decides).
static void on_load_changed(WebKitWebView *view, const WebKitLoadEvent ev, gpointer user) {
    (void) user;
    if (ev != WEBKIT_LOAD_FINISHED || !same_origin(webkit_web_view_get_uri(view))) return;
    WebKitWebResource *res = webkit_web_view_get_main_resource(view);
    WebKitURIResponse *resp = res ? webkit_web_resource_get_response(res) : NULL;
    const unsigned status = resp ? webkit_uri_response_get_status_code(resp) : 0;
    if (status < 400) return;
    W.http_err = status;
    printf("[webview] %s answered HTTP %u\n", W.origin, status);
    char text[300];
    snprintf(text, sizeof(text), "krunker.io answered HTTP %u in the challenge window: it blocks some networks"
                                 " (Settings > Client > Network > Proxy).", status);
    set_msg(text);
    W.ev_status = 1;
}

static gboolean on_tls_error(WebKitWebView *view, gchar *uri, GTlsCertificate *cert, GTlsCertificateFlags errors, gpointer user) {
    (void) view;
    (void) uri;
    (void) cert;
    (void) user;
    printf("[webview] %s: the TLS certificate was refused (flags 0x%x)\n", W.origin, (unsigned) errors);
    set_msg("krunker.io's certificate was not valid in the challenge window.");
    W.ev_failed = 1;
    return FALSE; // never bypassed: WebKit shows its error
}

static void on_terminated(WebKitWebView *view, WebKitWebProcessTerminationReason reason, gpointer user) {
    (void) view;
    (void) user;
    printf("[webview] the web process ended (reason %d)\n", (int) reason);
    set_msg("The challenge window's web process stopped.");
    W.ev_failed = 1;
}

static void on_destroy(GtkWidget *w, gpointer user) {
    (void) w;
    (void) user;
    W.window = NULL;
    W.view = NULL;
    if (!W.closing) {
        printf("[webview] the player closed the challenge window\n");
        W.ev_closed = 1;
    }
}

// ---------------------------------------------------------------- API

static int env_off(void) {
    const char *e = getenv("KRUNKNATIVE_WEBVIEW");
    return e && (!strcmp(e, "0") || !g_ascii_strcasecmp(e, "off"));
}

int kn_webview_available(void) { return !env_off() && W.gtk >= 0; }

static int toolkit_up(void) {
    if (W.gtk) return W.gtk > 0;
    // GTK must not touch the process locale (the game parses and prints "C" numbers)
    gtk_disable_setlocale();
    if (!gtk_init_check(NULL, NULL)) {
        W.gtk = -1;
        snprintf(W.gtk_err, sizeof(W.gtk_err), "GTK found no display for the challenge window");
        printf("[webview] %s\n", W.gtk_err);
        return 0;
    }
    W.gtk = 1;
    printf("[webview] WebKitGTK %u.%u.%u\n", webkit_get_major_version(), webkit_get_minor_version(), webkit_get_micro_version());
    return 1;
}

int kn_webview_captcha_open(const char *url, const char *sitekey, const char *title, const char *proxy, char *err, const size_t errn) {
    if (err && errn) err[0] = 0;
    if (env_off()) {
        if (err) snprintf(err, errn, "the webview is turned off (KRUNKNATIVE_WEBVIEW=0)");
        return 0;
    }
    if (W.window) { // already up: bring it forward
        gtk_window_present(GTK_WINDOW(W.window));
        return 1;
    }
    if (!url || !url[0]) {
        const char *e = getenv("KRUNKNATIVE_KRUNKER_CAPTCHA_URL");
        url = e && e[0] ? e : KN_WEBVIEW_CAPTCHA_URL;
    }
    char origin[sizeof(W.origin)];
    if (!origin_of(url, origin, sizeof(origin)) || (strncmp(origin, "https://", 8) && !is_loopback_origin(origin))) {
        if (err) snprintf(err, errn, "the challenge URL must be https:// (or http:// on this computer)");
        return 0;
    }
    for (const char *k = sitekey; k && *k; k++)
        if (!g_ascii_isalnum(*k) && *k != '_' && *k != '-') sitekey = NULL;
    if (!sitekey || !sitekey[0]) {
        if (err) snprintf(err, errn, "no Turnstile sitekey");
        return 0;
    }
    if (!toolkit_up()) {
        if (err) snprintf(err, errn, "%s", W.gtk_err);
        return 0;
    }
    snprintf(W.origin, sizeof(W.origin), "%s", origin);
    W.taken = W.closing = 0;
    W.http_err = 0;
    W.ev_token = W.ev_failed = W.ev_closed = W.ev_status = 0;
    wipe(W.token, sizeof(W.token));

    WebKitWebContext *ctx = webkit_web_context_new_ephemeral(); // nothing of it reaches the disk
    if (proxy && proxy[0]) { // the matchmaker's proxy (the player's setting / environment), loopback excluded
        static const char *const direct[] = {"localhost", "127.0.0.0/8", "::1", NULL};
        WebKitNetworkProxySettings *ps = webkit_network_proxy_settings_new(proxy, direct);
        webkit_website_data_manager_set_network_proxy_settings(webkit_web_context_get_website_data_manager(ctx), WEBKIT_NETWORK_PROXY_MODE_CUSTOM, ps);
        webkit_network_proxy_settings_free(ps);
    }
    WebKitUserContentManager *ucm = webkit_user_content_manager_new();
    g_signal_connect(ucm, "script-message-received::knCaptcha", G_CALLBACK(on_message), NULL);
    webkit_user_content_manager_register_script_message_handler_in_world(ucm, "knCaptcha", BRIDGE_WORLD);
    char *js = bridge_script();
    WebKitUserScript *script = webkit_user_script_new_for_world(js, WEBKIT_USER_CONTENT_INJECT_TOP_FRAME, WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,
                                                                BRIDGE_WORLD, NULL, NULL);
    g_free(js);
    webkit_user_content_manager_add_script(ucm, script);
    webkit_user_script_unref(script);
    js = user_script(sitekey, title && title[0] ? title : "Security Challenge"); // the main world: Turnstile's
    script = webkit_user_script_new(js, WEBKIT_USER_CONTENT_INJECT_TOP_FRAME, WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_END, NULL, NULL);
    g_free(js);
    webkit_user_content_manager_add_script(ucm, script);
    webkit_user_script_unref(script);

    W.view = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "web-context", ctx, "user-content-manager", ucm, NULL));
    g_object_unref(ucm);
    g_object_unref(ctx);
    WebKitSettings *settings = webkit_web_view_get_settings(W.view);
    webkit_settings_set_javascript_can_open_windows_automatically(settings, FALSE);
    webkit_settings_set_enable_developer_extras(settings, FALSE);
    g_signal_connect(W.view, "decide-policy", G_CALLBACK(on_policy), NULL);
    g_signal_connect(W.view, "load-failed", G_CALLBACK(on_load_failed), NULL);
    g_signal_connect(W.view, "load-changed", G_CALLBACK(on_load_changed), NULL);
    g_signal_connect(W.view, "load-failed-with-tls-errors", G_CALLBACK(on_tls_error), NULL);
    g_signal_connect(W.view, "web-process-terminated", G_CALLBACK(on_terminated), NULL);

    W.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    char wtitle[200];
    snprintf(wtitle, sizeof(wtitle), "%s - krunker.io", title && title[0] ? title : "Security Challenge");
    gtk_window_set_title(GTK_WINDOW(W.window), wtitle);
    gtk_window_set_default_size(GTK_WINDOW(W.window), 520, 600);
    gtk_window_set_position(GTK_WINDOW(W.window), GTK_WIN_POS_CENTER);
    gtk_window_set_keep_above(GTK_WINDOW(W.window), TRUE); // over a fullscreen game window
    g_signal_connect(W.window, "destroy", G_CALLBACK(on_destroy), NULL);
    gtk_container_add(GTK_CONTAINER(W.window), GTK_WIDGET(W.view));
    gtk_widget_show_all(W.window);
    gtk_window_present(GTK_WINDOW(W.window));

    printf("[webview] challenge window: %s (%s)%s\n", url, W.origin, proxy && proxy[0] ? ", through the client's proxy" : "");
    webkit_web_view_load_uri(W.view, url);
    kn_webview_pump();
    return 1;
}

int kn_webview_captcha_is_open(void) { return W.window != NULL; }

void kn_webview_captcha_close(void) {
    if (W.window) {
        W.closing = 1;
        GtkWidget *w = W.window;
        if (W.view) webkit_web_view_stop_loading(W.view);
        gtk_widget_destroy(w); // the view, its context and web processes go with it
        W.window = NULL;
        W.view = NULL;
        W.closing = 0;
        printf("[webview] challenge window closed\n");
    }
    if (!W.ev_token) wipe(W.token, sizeof(W.token));
}

void kn_webview_pump(void) {
    if (W.gtk <= 0) return;
    for (int i = 0; i < 256 && g_main_context_iteration(NULL, FALSE); i++) {
    }
}

int kn_webview_captcha_poll(char *token, const size_t n, char *err, const size_t errn) {
    kn_webview_pump();
    if (err && errn) err[0] = 0;
    if (W.ev_token) {
        W.ev_token = 0;
        const size_t len = strlen(W.token);
        const int ok = token && len < n;
        if (ok) memcpy(token, W.token, len + 1);
        wipe(W.token, sizeof(W.token));
        kn_webview_captcha_close(); // done: the window goes at once
        if (ok) return KN_WEBVIEW_TOKEN;
        if (err) snprintf(err, errn, "the token did not fit");
        return KN_WEBVIEW_FAILED;
    }
    if (W.ev_failed) {
        W.ev_failed = W.ev_status = 0;
        kn_webview_captcha_close();
        if (err) snprintf(err, errn, "%s", W.msg);
        return KN_WEBVIEW_FAILED;
    }
    if (W.ev_closed) {
        W.ev_closed = W.ev_status = 0;
        return KN_WEBVIEW_CLOSED;
    }
    if (W.ev_status) {
        W.ev_status = 0;
        if (err) snprintf(err, errn, "%s", W.msg);
        return KN_WEBVIEW_STATUS;
    }
    return KN_WEBVIEW_NONE;
}

#else // no webview backend in this build

int kn_webview_available(void) { return 0; }

int kn_webview_captcha_open(const char *url, const char *sitekey, const char *title, const char *proxy, char *err, const size_t errn) {
    (void) url;
    (void) proxy;
    (void) sitekey;
    (void) title;
    if (err && errn) snprintf(err, errn, "this build has no webview");
    return 0;
}

int kn_webview_captcha_is_open(void) { return 0; }
void kn_webview_captcha_close(void) {}
void kn_webview_pump(void) {}

int kn_webview_captcha_poll(char *token, const size_t n, char *err, const size_t errn) {
    (void) token;
    (void) n;
    if (err && errn) err[0] = 0;
    return 0;
}

#endif
