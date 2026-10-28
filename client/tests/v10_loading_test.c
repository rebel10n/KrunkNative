// v10_loading_test: the v10.0.4 map card's timing and DOM writes (client/src/ui/v10_loading.c) against
// deob-10.0.4.js:223011-223041. The DOM ops are stubbed here: a small element table records what the page
// would show (display, the loadingOut class, --load-bg, the texts).
//   v10_loading_test
#include "v10_loading.h"

#include <stdio.h>
#include <string.h>

#include "html_dom.h"

static int failures, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

// The page's ids the loader writes; index + 1 is the HtmlElem handle
static const char *const IDS[] = {"loadingBg", "loadingMapThumb", "loadingMapNext", "loadingMapName", "loadingMapMode", "loadingMapText"};
#define N_IDS ((int) (sizeof IDS / sizeof IDS[0]))
typedef struct {
    char display[16];
    char text[64];
    char load_bg[128];
    char load_accent[32];
    int loading_out;
} El;
static El els[N_IDS];

HtmlElem html_dom_by_id(const char *id) {
    for (int i = 0; i < N_IDS; i++) if (!strcmp(IDS[i], id)) return (HtmlElem) (i + 1);
    return 0;
}
void html_dom_set_display(HtmlElem el, const char *value) { snprintf(els[el - 1].display, sizeof els[0].display, "%s", value); }
void html_dom_set_text(HtmlElem el, const char *text) { snprintf(els[el - 1].text, sizeof els[0].text, "%s", text); }
void html_dom_set_attr(HtmlElem el, const char *name, const char *value) { (void) el; (void) name; (void) value; }
void html_dom_set_class(HtmlElem el, const char *cls, const int on) {
    if (!strcmp(cls, "loadingOut")) els[el - 1].loading_out = on != 0;
}
void html_dom_set_style(HtmlElem el, const char *prop, const char *value) {
    if (!strcmp(prop, "--load-bg")) snprintf(els[el - 1].load_bg, sizeof els[0].load_bg, "%s", value);
    else if (!strcmp(prop, "--load-accent")) snprintf(els[el - 1].load_accent, sizeof els[0].load_accent, "%s", value);
}

static void reset_els(void) {
    memset(els, 0, sizeof els);
    v10_loading_reset();
}

int main(void) {
    // loadInTime: none for the session's first load, 2000 ms after (deob-10.0.4.js:16549, :223025)
    CHECK(v10_loading_hold_ms(0) == 0.0 && v10_loading_hold_ms(1) == 2000.0 && v10_loading_hold_ms(7) == 2000.0,
          "hold: %g %g %g", v10_loading_hold_ms(0), v10_loading_hold_ms(1), v10_loading_hold_ms(7));

    // the first load: no card (the boot curtain covers it), the page is untouched
    reset_els();
    V10LoadingMap first = {.map_thumb = "./img/maps/map_3.png", .map_name = "Sandstorm", .mode_name = "Free for All"};
    v10_loading_begin(&first, 0.0);
    CHECK(!v10_loading_visible() && els[0].display[0] == '\0', "first load shows no card");

    // a later load: the card shows the map (thumbnail, name, mode, accent) at once
    V10LoadingMap second = {.map_thumb = "./img/maps/map_3.png", .map_name = "Sandstorm", .mode_name = "Free for All"};
    v10_loading_begin(&second, 1000.0);
    CHECK(v10_loading_visible(), "a second load shows the card");
    CHECK(!strcmp(els[0].display, "block") && !els[0].loading_out, "card display block, not fading (%s, %d)", els[0].display, els[0].loading_out);
    CHECK(!strcmp(els[0].load_bg, "url(./img/maps/map_3.png)"), "--load-bg: %s", els[0].load_bg);
    CHECK(!strcmp(els[0].load_accent, "#fff"), "no accent from the mode table: %s", els[0].load_accent);
    CHECK(!strcmp(els[3].text, "Sandstorm") && !strcmp(els[4].text, "Free for All") && !strcmp(els[5].text, "LOADING"),
          "texts: %s / %s / %s", els[3].text, els[4].text, els[5].text);
    CHECK(!strcmp(els[2].text, "UP NEXT"), "generic.loading.nextMap: %s", els[2].text);

    // the hold: no fade before 3000 ms (1000 + 2000), the fade class at 3000, display none 300 ms later
    v10_loading_tick(2999.0);
    CHECK(!els[0].loading_out && v10_loading_visible(), "still held at 2999");
    v10_loading_tick(3000.0);
    CHECK(els[0].loading_out && v10_loading_visible(), "fading at 3000");
    v10_loading_tick(3299.0);
    CHECK(v10_loading_visible() && strcmp(els[0].display, "none") != 0, "still on screen during the fade");
    v10_loading_tick(3300.0);
    CHECK(!v10_loading_visible() && !strcmp(els[0].display, "none"), "gone at 3300");

    // a custom map (no thumbnail) and a mode-less map: --load-bg none, the mode line hidden
    V10LoadingMap custom = {.map_thumb = NULL, .map_name = "Mine", .mode_name = NULL};
    v10_loading_begin(&custom, 5000.0);
    CHECK(!strcmp(els[0].load_bg, "none") && !strcmp(els[4].display, "none"), "custom map: bg %s, mode display %s", els[0].load_bg, els[4].display);

    // a newer load replaces an older one's schedule: the old hold (due at 7000) does not hide the new card
    V10LoadingMap next = {.map_thumb = "./img/maps/map_2.png", .map_name = "Pit", .mode_name = "Team Deathmatch"};
    v10_loading_begin(&next, 6000.0);
    v10_loading_tick(7000.0);
    CHECK(!els[0].loading_out, "a newer load is held for its own 2000 ms (fading at 8000, not 7000)");
    v10_loading_tick(8000.0);
    CHECK(els[0].loading_out, "fades at its own deadline");

    printf("v10_loading_test: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
