// notify(): the toast of the v3.1.1 offline build's client mod
// (/home/user/jails/kru/src/Krunker_v3.1.1/mod/notify.ts), ported 1:1 over html_dom.h so
// it works on both the native engine and the web DOM. One <div> appended to <body>:
//
//   position:absolute; z-index:9999; top:10px; right:10px; padding:10px; margin:10px;
//   box-sizing:border-box; border-radius:5px; background:#0005; color:#fff
//   transform: translateX(calc(100% + 20px))       (parked off the right edge)
//   .anim { animation: slide 2s forwards }
//   @keyframes slide { 0%,100% { translateX(calc(100% + 20px)) } 10%,90% { translateX(0) } }
//
// notify(message) sets innerHTML and restarts the animation. The keyframes are stepped
// here per frame (ui_notify_tick, from hud_ui.c) with the CSS default timing function
// `ease` applied per keyframe segment, and calc(100% + 20px) resolved from the element's
// offsetWidth, so no CSS animation support is needed from either backend. Font: the
// page's `* { font-family: gamefont }` at the body's default 16px, like the original.
#include <ui_logic.h>
#include <stdio.h>
#include <string.h>

#define NOTIFY_ID "knNotify"
#define NOTIFY_DURATION_MS 2000.0
#define NOTIFY_IN_END 0.1    // keyframe 10%
#define NOTIFY_OUT_START 0.9 // keyframe 90%
#define NOTIFY_OFFSET_PX 20.0 // the "+ 20px" of calc(100% + 20px)
#define NOTIFY_TEXT_MAX 512

static struct {
    char html[NOTIFY_TEXT_MAX];
    int pending;   // notify() since the last tick: write the markup, restart the clock
    int running;
    double start_ms;
} g_notify;

// cubic-bezier(0.25, 0.1, 0.25, 1) (CSS `ease`): y at x.
static double css_ease(const double x) {
    const double x1 = 0.25, y1 = 0.1, x2 = 0.25, y2 = 1.0;
    double lo = 0.0, hi = 1.0, t = x;
    for (int i = 0; i < 40; i++) { // bisection on the monotonic x(t)
        t = (lo + hi) * 0.5;
        const double u = 1.0 - t;
        const double bx = 3.0 * u * u * t * x1 + 3.0 * u * t * t * x2 + t * t * t;
        if (bx < x) lo = t;
        else hi = t;
    }
    const double u = 1.0 - t;
    return 3.0 * u * u * t * y1 + 3.0 * u * t * t * y2 + t * t * t;
}

static HtmlElem notify_element(void) {
    HtmlElem el = html_dom_by_id(NOTIFY_ID);
    if (el) return el;
    const HtmlElem body = html_dom_body();
    if (!body) return 0;
    el = html_dom_create("div", NOTIFY_ID);
    html_dom_set_style(el, "position", "absolute");
    html_dom_set_style(el, "z-index", "9999");
    html_dom_set_style(el, "top", "10px");
    html_dom_set_style(el, "right", "10px");
    html_dom_set_style(el, "padding", "10px");
    html_dom_set_style(el, "margin", "10px");
    html_dom_set_style(el, "box-sizing", "border-box");
    html_dom_set_style(el, "border-radius", "5px");
    html_dom_set_style(el, "background", "#0005");
    html_dom_set_style(el, "color", "#fff");
    html_dom_set_display(el, "none");
    html_dom_append(body, el);
    return el;
}

void ui_notify(const char *html) {
    snprintf(g_notify.html, sizeof(g_notify.html), "%s", html ? html : "");
    g_notify.pending = 1;
}

// translateX(calc(100% + 20px)) at progress p of `slide` (0..1), as a fraction of the
// offscreen offset: 1 = parked, 0 = in place.
static double notify_offset_fraction(const double p) {
    if (p <= 0.0 || p >= 1.0) return 1.0;
    if (p < NOTIFY_IN_END) return 1.0 - css_ease(p / NOTIFY_IN_END);
    if (p <= NOTIFY_OUT_START) return 0.0;
    return css_ease((p - NOTIFY_OUT_START) / (1.0 - NOTIFY_OUT_START));
}

void ui_notify_tick(const double now_ms) {
    if (g_notify.pending) {
        g_notify.pending = 0;
        const HtmlElem el = notify_element();
        if (!el) return;
        html_dom_set_html(el, g_notify.html);              // notification.innerHTML = message
        html_dom_set_style(el, "visibility", "hidden");    // until its width is laid out (next tick)
        html_dom_set_display(el, "block");
        g_notify.running = 1;
        g_notify.start_ms = now_ms;                        // className = "anim": the animation restarts
        return;
    }
    if (!g_notify.running) return;
    const HtmlElem el = html_dom_by_id(NOTIFY_ID);
    if (!el) {
        g_notify.running = 0; // the page was rebuilt
        return;
    }
    const double p = (now_ms - g_notify.start_ms) / NOTIFY_DURATION_MS;
    if (p >= 1.0) { // `forwards`: parked off screen again
        html_dom_set_display(el, "none");
        g_notify.running = 0;
        return;
    }
    const double width = html_dom_offset_width(el);
    char transform[64];
    snprintf(transform, sizeof(transform), "translateX(%.2fpx)", (width + NOTIFY_OFFSET_PX) * notify_offset_fraction(p));
    html_dom_set_style(el, "transform", transform);
    html_dom_set_style(el, "visibility", "visible");
}
