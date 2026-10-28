// The v10.0.4 map loading screen: see v10_loading.h. Elements (the page's ids, ui/index.html):
//   #loadingBg          the card: --load-bg (the thumbnail as a background), --load-accent, class loadingOut
//   #loadingMapThumb    img: the map thumbnail (display block when there is one)
//   #loadingMapNext     "UP NEXT" (generic.loading.nextMap)
//   #loadingMapName     the map name
//   #loadingMapMode     the mode name, uppercased
//   #loadingMapText     "LOADING" (generic.loading.simple, uppercased)
// The page does not have the last five yet: until it does, the calls find no element and write nothing.
#include "v10_loading.h"

#include <stddef.h>
#include <stdio.h>

#include "html_dom.h"

static int g_loads;          // loads begun this session (iíîíïiì, :223024)
static int g_visible;        // the card is on screen
static double g_hide_at = -1.0;   // the fade starts at this time (iiïiiìi, :223025); -1: none scheduled
static double g_gone_at = -1.0;   // the card goes display none at this time; -1: none

double v10_loading_hold_ms(const int loads_before) {
    return loads_before > 0 ? V10_LOADING_HOLD_MS : 0.0;
}

static void set_display_by_id(const char *id, const char *value) {
    const HtmlElem el = html_dom_by_id(id);
    if (el) html_dom_set_display(el, value);
}

static void set_text_by_id(const char *id, const char *text) {
    const HtmlElem el = html_dom_by_id(id);
    if (el) html_dom_set_text(el, text);
}

// loadingBg.style.setProperty("--load-bg", url(...) | none) and the thumbnail img (:223030-223032).
static void write_thumb(const char *thumb) {
    const HtmlElem bg = html_dom_by_id("loadingBg");
    char value[256];
    if (thumb) snprintf(value, sizeof(value), "url(%s)", thumb);
    else snprintf(value, sizeof(value), "none");
    if (bg) html_dom_set_style(bg, "--load-bg", value);
    const HtmlElem img = html_dom_by_id("loadingMapThumb");
    if (!img) return;
    html_dom_set_display(img, thumb ? "block" : "none");
    if (thumb) html_dom_set_attr(img, "src", thumb);
}

void v10_loading_begin(const V10LoadingMap *map, const double now_ms) {
    const int loads_before = g_loads++;
    const double hold = v10_loading_hold_ms(loads_before);
    g_hide_at = now_ms + hold; // a newer load replaces an older one's schedule (the generation check, :223011)
    g_gone_at = -1.0;
    if (loads_before == 0) return; // the first load: no card, the boot curtain covers it (:223024-223026)
    const V10LoadingMap none = {0};
    if (!map) map = &none;

    write_thumb(map->map_thumb);
    set_text_by_id("loadingMapNext", "UP NEXT");
    set_text_by_id("loadingMapName", map->map_name ? map->map_name : "");
    set_text_by_id("loadingMapMode", map->mode_name ? map->mode_name : "");
    set_display_by_id("loadingMapMode", map->mode_name ? "" : "none");
    const HtmlElem bg = html_dom_by_id("loadingBg");
    if (bg) {
        html_dom_set_style(bg, "--load-accent", map->accent ? map->accent : "#fff");
        html_dom_set_class(bg, "loadingOut", 0); // loadingBg.classList.remove("loadingOut") (:223039)
        html_dom_set_display(bg, "block");
    }
    set_text_by_id("loadingMapText", "LOADING");
    g_visible = 1;
}

void v10_loading_tick(const double now_ms) {
    if (!g_visible) return;
    if (g_hide_at >= 0.0 && now_ms >= g_hide_at) {
        g_hide_at = -1.0;
        const HtmlElem card = html_dom_by_id("loadingBg");
        if (card) html_dom_set_class(card, "loadingOut", 1); // classList.add("loadingOut") (:223012)
        g_gone_at = now_ms + V10_LOADING_FADE_MS;
    }
    if (g_gone_at >= 0.0 && now_ms >= g_gone_at) {
        g_gone_at = -1.0;
        set_display_by_id("loadingBg", "none"); // :223013
        g_visible = 0;
    }
}

int v10_loading_visible(void) {
    return g_visible;
}

void v10_loading_reset(void) {
    g_loads = 0;
    g_visible = 0;
    g_hide_at = -1.0;
    g_gone_at = -1.0;
}
