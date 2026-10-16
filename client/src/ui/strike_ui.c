// Krunker Strike's page and screen stack (strike_ui.h). Sources: FRVR Krunker Strike 1.37.2,
// assets/index-Cz2uYXJQ.js (read as text; names below are its minified identifiers, which the
// .deob.js copy keeps):
//   iwe()/swe  the Mithril mount into #frvrMenuMobileHolder: oncreate hides the static
//              #frvrLoadingScreen and keeps --page-width / --page-height on the holder; view =
//              [yz() screens, rwe() in-game UI + controls, .frvr-notifications-holder, CAe,
//              .frvr-loading-holder]
//   Pi/Pt/kt/nu/yz  the screen stack: Pt pushes (skipped when the same view is on it), kt pops,
//              nu replaces; yz renders the top plus the translucent ones below it and keepAlive ones
//   vb/fz/Fm/RA  the loading screen (LOADING_SCREEN): curtains, cog, logo, sparkles, info, tips, bar;
//              Fm shows it (resolves after pz = 500 ms), RA completes it u_ = 1500 ms after it was
//              created at the earliest, then Az = 700 ms of curtains opening before it goes
// The boot order is FRVR's: index.html's static loading screen ("Loading Assets", the 10 s
// first-load bar) stays up while this client loads the game (FRVR: the bundle and its assets),
// then the mount swaps it for "Loading Game" without an enter animation and MAIN_MENU is pushed.
#include <strike_ui.h>
#include <flavor.h>
#include <html_ui.h>
#include <ui_logic.h>
#include <shared.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <cJSON.h>

#ifdef __EMSCRIPTEN__
#define STRIKE_SCREENS_DIR "/ui/strike/screens/" // --preload-file ui/strike/screens (CMakeLists.txt)
#endif

#define STRIKE_STACK_MAX 8
#define LOADING_MIN_MS 1500.0 // u_
#define LOADING_OUT_MS 700.0  // Az
#define LOADING_FM_MS 500.0   // pz
#define BOOT_MAX_MS 15000.0   // the static screen never outlives a dead connection

static const char *const SCREEN_IDS[STRIKE_SCREEN_COUNT] = {
    [STRIKE_SCREEN_NONE] = "",
    [STRIKE_SCREEN_MAIN_MENU] = "MAIN_MENU",
    [STRIKE_SCREEN_CLASS_SELECTION] = "CLASS_SELECTION",
    [STRIKE_SCREEN_TAP_TO_PLAY] = "TAP_TO_PLAY",
    [STRIKE_SCREEN_SETTINGS] = "SETTINGS",
    [STRIKE_SCREEN_CUSTOMIZATION] = "CUSTOMIZATION_SCREEN",
    [STRIKE_SCREEN_STORE] = "STORE",
    [STRIKE_SCREEN_SPIN] = "SPIN",
    [STRIKE_SCREEN_MISSIONS] = "MISSION_TAB_SCREEN",
    [STRIKE_SCREEN_BATTLEPASS] = "BATTLEPASS_SCREEN",
    [STRIKE_SCREEN_REWARD_TO_CLAIM] = "REWARD_TO_CLAIM",
    [STRIKE_SCREEN_CREATE_MATCH] = "CREATE_MATCH",
    [STRIKE_SCREEN_CUSTOMIZE_CONTROLS] = "CUSTOMIZE_CONTROLS",
    [STRIKE_SCREEN_LEAN_END] = "LEAN_END_SCREEN",
    [STRIKE_SCREEN_MATCH_DETAILS] = "MATCH_DETAILS_SCREEN",
    [STRIKE_SCREEN_LEADERBOARD_PLAYERS] = "LEADERBOARD_PLAYERS",
    [STRIKE_SCREEN_POPUP] = "POPUP",
    [STRIKE_SCREEN_POPUP_WARNING] = "POPUP_WARNING",
    [STRIKE_SCREEN_PROFILE] = "PROFILE",
    [STRIKE_SCREEN_ITEM_SELECTION] = "ITEM_SELECTION_SCREEN",
    [STRIKE_SCREEN_REPORT_PLAYER] = "REPORT_PLAYER",
    [STRIKE_SCREEN_POPUP_SUCCESS] = "POPUP_SUCCESS",
    [STRIKE_SCREEN_POPUP_ERROR] = "POPUP_ERROR",
    [STRIKE_SCREEN_REWARD] = "REWARD",
    [STRIKE_SCREEN_REWARD_CHOICE] = "REWARD_CHOICE",
    [STRIKE_SCREEN_INPUT_BLOCKER] = "INPUT_BLOCKER",
    [STRIKE_SCREEN_PROFILE_DETAILS] = "PROFILE-DETAILS",
    [STRIKE_SCREEN_POPUP_MESSAGE] = "POPUP_MESSAGE",
};

// view.opts: translucent (the screens below stay rendered), keepAlive (rendered while buried)
static const struct { unsigned char translucent, keep_alive; } SCREEN_OPTS[STRIKE_SCREEN_COUNT] = {
    [STRIKE_SCREEN_SETTINGS] = {0, 1},
    [STRIKE_SCREEN_STORE] = {0, 1},
    [STRIKE_SCREEN_BATTLEPASS] = {0, 1},
    [STRIKE_SCREEN_POPUP] = {1, 0},
    // REWARD_TO_CLAIM sits in the POPUP frame (In: translucent); the round end is nu(Ad, ...,
    // {keepAlive: true}), so MATCH_DETAILS_SCREEN pushed over it keeps it (and its fill) rendered
    [STRIKE_SCREEN_REWARD_TO_CLAIM] = {1, 0},
    [STRIKE_SCREEN_LEAN_END] = {0, 1},
    [STRIKE_SCREEN_REPORT_PLAYER] = {1, 0}, // Hy opts {preventReplace, translucent}
    [STRIKE_SCREEN_INPUT_BLOCKER] = {1, 0}, // ws opts {translucent}
    [STRIKE_SCREEN_PROFILE_DETAILS] = {0, 1}, // jC: keepAlive, so the Settings tab's name field survives an alert
};

// Dk: generic.tips.* in English (the 1.37.2 i18n table, tools/strike/strike_data.json)
static const char *const TIPS[] = {
    "Intense game loading\xE2\x80\xA6 warm up your fingers while you wait",
    "Never gonna give you up",
    "*Crickets*",
    "Must keep Krunking...",
    "ERROR 403: Secret loading text forbidden",
    "No aim, no brain, must be a shotgun main - Sun Tzu",
    "Rubberbanding? Make sure youre playing on a region nearest you.",
    "No Krunkies?",
    "Practice makes perfect",
    "KR is short for Krunkies, the in-game currency",
    "We have been trying to reach you about your KRs extended warranty",
    "ERROR 404: Loading text not found",
    "Watching 1s and 0s roll by...",
    "[Insert 5 KR to play]",
    "Register an account and level up to access all of Krunker's features",
    "FRVR forever!",
    "No scope!",
    "grass.png loading...",
    "Eat, Sleep, Krunk, Repeat",
};

typedef struct {
    StrikeScreen screen;
    HtmlElem root; // 0 while buried and not rendered (yz)
} StackEntry;

static struct {
    Client *client;
    char screens_dir[512];
    int dom_ready, mounted, booted;
    int boot_timeout;      // BOOT_MAX_MS passed (timer)
    int fm_done;           // Fm's pz = 500 ms passed since the mount (timer)
    HtmlElem holder, notifications, loading_holder;
    StackEntry stack[STRIKE_STACK_MAX];
    int depth;
    unsigned serial;
    float vw, vh;
    // the loading screen (al / vb / fz)
    struct {
        int shown, complete, no_anim, width, tip;
        int bar_timer, min_timer, remove_timer;
        int min_done;      // u_ = 1500 ms passed since the screen was created (vb: Se.loadingStarted)
        int want_complete; // RA() was called
        char info[128];
        HtmlElem root;
    } ld;
} S;

int strike_ui_active(void) {
    return flavor_get() == FLAVOR_STRIKE && html_ui_available();
}

char *strike_ui_page_dir(const char *base_dir) {
    const char *base = base_dir ? base_dir : "";
    if (flavor_get() != FLAVOR_STRIKE) return concat(base, "");
    char *dir = concat(base, "strike/");
#ifndef __EMSCRIPTEN__
    snprintf(S.screens_dir, sizeof(S.screens_dir), "%sscreens/", dir ? dir : "");
#endif
    return dir;
}

const char *strike_screen_id(const StrikeScreen s) {
    return s > STRIKE_SCREEN_NONE && s < STRIKE_SCREEN_COUNT ? SCREEN_IDS[s] : "";
}

// ---------------------------------------------------------------- templates

static char *template_text(const char *name) {
    char path[640];
#ifdef __EMSCRIPTEN__
    snprintf(path, sizeof(path), "%s%s.html", STRIKE_SCREENS_DIR, name);
#else
    snprintf(path, sizeof(path), "%s%s.html", S.screens_dir[0] ? S.screens_dir : "../ui/strike/screens/", name);
#endif
    unsigned char *data = NULL;
    size_t length = 0;
    if (read_file(path, &data, &length) != 0) {
        fprintf(stderr, "[strike] template %s missing (%s)\n", name, path);
        return NULL;
    }
    // the generator's provenance comment line (tools/strike/strike_screens.py) is not markup
    size_t skip = 0;
    if (length >= 4 && !memcmp(data, "<!--", 4)) {
        const char *end = strstr((const char *) data, "-->");
        if (end) {
            skip = (size_t) (end - (const char *) data) + 3;
            while (skip < length && (data[skip] == '\n' || data[skip] == '\r')) skip++;
        }
    }
    char *text = malloc(length - skip + 1);
    if (!text) {
        free(data);
        return NULL;
    }
    memcpy(text, data + skip, length - skip);
    text[length - skip] = 0;
    free(data);
    return text;
}

// "<!-- part NAME -->" lines split a template file into its main markup (before the first one)
// and named parts: the row / card components of a screen transcribed from the bundle
// (MATCH_DETAILS_SCREEN.html, strike_meta.c). part NULL = the main markup.
#define PART_MARK "<!-- part "
static char *template_section(const char *name, const char *part) {
    char *text = template_text(name);
    if (!text) return NULL;
    char *start = text;
    if (part) {
        char mark[96];
        snprintf(mark, sizeof(mark), PART_MARK "%s -->", part);
        const char *m = strstr(text, mark);
        if (!m) {
            fprintf(stderr, "[strike] template %s has no part %s\n", name, part);
            free(text);
            return NULL;
        }
        start = text + (m - text) + strlen(mark);
        while (*start == '\n' || *start == '\r') start++;
    }
    const char *mark = strstr(start, PART_MARK);
    size_t n = mark ? (size_t) (mark - start) : strlen(start);
    if (mark) { // the newline before the next part's marker is not markup
        while (n > 0 && (start[n - 1] == '\n' || start[n - 1] == '\r')) n--;
    }
    memmove(text, start, n);
    text[n] = 0;
    return text;
}

static char *substitute(char *tpl, const StrikeVar *vars, const int count) {
    if (!tpl) return NULL;
    StrBuf sb = {0};
    const char *cur = tpl;
    for (;;) {
        const char *open = strstr(cur, "{{");
        if (!open) break;
        const char *close = strstr(open + 2, "}}");
        if (!close) break;
        sb_append_n(&sb, cur, (size_t) (open - cur));
        const char *key = open + 2;
        const int raw = *key == '!';
        if (raw) key++;
        const size_t klen = (size_t) (close - key);
        const char *value = "";
        for (int i = 0; i < count; i++) {
            if (vars[i].key && strlen(vars[i].key) == klen && !strncmp(vars[i].key, key, klen)) {
                value = vars[i].value ? vars[i].value : "";
                break;
            }
        }
        if (raw) sb_append(&sb, value);
        else sb_append_esc(&sb, value);
        cur = close + 2;
    }
    sb_append(&sb, cur);
    free(tpl);
    return sb_finish(&sb);
}

char *strike_template(const char *name, const StrikeVar *vars, const int count) {
    return substitute(template_section(name, NULL), vars, count);
}

char *strike_template_part(const char *name, const char *part, const StrikeVar *vars, const int count) {
    return substitute(template_section(name, part), vars, count);
}

// ---------------------------------------------------------------- holder layers

// Insert html (one root element) before `anchor` and return the new root. The root is found
// through a temporary data attribute (insertAdjacentHTML returns nothing).
static HtmlElem insert_before(const HtmlElem anchor, const char *html) {
    if (!anchor || !html) return 0;
    const char *lt = strchr(html, '<');
    if (!lt) return 0;
    const char *end = lt + 1;
    while (*end && *end != ' ' && *end != '>' && *end != '/' && *end != '\n') end++;
    char mark[32];
    snprintf(mark, sizeof(mark), "%u", ++S.serial);
    StrBuf sb = {0};
    sb_append_n(&sb, html, (size_t) (end - html));
    sb_appendf(&sb, " data-kn-new=\"%s\"", mark);
    sb_append(&sb, end);
    char *marked = sb_finish(&sb);
    html_dom_insert_html(anchor, HTML_INSERT_BEFORE_BEGIN, marked);
    free(marked);
    char sel[48];
    snprintf(sel, sizeof(sel), "[data-kn-new=\"%s\"]", mark);
    const HtmlElem root = html_dom_query(S.holder, sel);
    if (root) html_dom_remove_attr(root, "data-kn-new");
    return root;
}

// yz(): screens render first in the holder, so a new screen root goes before the first
// rendered root of a screen above it, else before the in-game layer / notifications holder.
static HtmlElem screen_anchor(const int index) {
    for (int i = index + 1; i < S.depth; i++) {
        if (S.stack[i].root) return S.stack[i].root;
    }
    const HtmlElem first_game = html_dom_query(S.holder, ".frvr-in-game-ui, .control-buttons, .joystick-zones, .effect-flash, .debug-info");
    return first_game ? first_game : S.notifications;
}

static void render_entry(const int index) {
    StackEntry *e = &S.stack[index];
    if (e->root) return;
    const char *file = strike_menu_template(e->screen);
    char *html = file ? strike_template(file, NULL, 0) : NULL;
    if (!html) return;
    e->root = insert_before(screen_anchor(index), html);
    free(html);
    if (e->root) strike_menu_render(e->screen, e->root);
}

static void unrender_entry(const int index) {
    StackEntry *e = &S.stack[index];
    if (e->root) html_dom_remove(e->root);
    e->root = 0;
}

// yz(): which entries are rendered: the top, the translucent run below it, keepAlive ones.
static void sync_stack(void) {
    if (!S.mounted) return;
    int keep[STRIKE_STACK_MAX] = {0};
    if (S.depth > 0) {
        const int top = S.depth - 1;
        keep[top] = 1;
        if (SCREEN_OPTS[S.stack[top].screen].translucent) {
            for (int i = top - 1; i >= 0; i--) {
                keep[i] = 1;
                if (!SCREEN_OPTS[S.stack[i].screen].translucent) break;
            }
        }
        for (int i = top - 1; i >= 0; i--) {
            if (SCREEN_OPTS[S.stack[i].screen].keep_alive) keep[i] = 1;
        }
    }
    for (int i = 0; i < S.depth; i++) {
        if (!keep[i]) unrender_entry(i);
    }
    for (int i = 0; i < S.depth; i++) {
        if (keep[i]) render_entry(i);
    }
}

int strike_screen_depth(void) { return S.depth; }

StrikeScreen strike_screen_top(void) {
    return S.depth > 0 ? S.stack[S.depth - 1].screen : STRIKE_SCREEN_NONE;
}

int strike_screen_shown(const StrikeScreen s) {
    for (int i = 0; i < S.depth; i++) {
        if (S.stack[i].screen == s) return 1;
    }
    return 0;
}

HtmlElem strike_screen_root(const StrikeScreen s) {
    for (int i = S.depth - 1; i >= 0; i--) {
        if (S.stack[i].screen == s) return S.stack[i].root;
    }
    return 0;
}

void strike_screen_rerender(const StrikeScreen s) {
    for (int i = 0; i < S.depth; i++) {
        if (S.stack[i].screen != s || !S.stack[i].root) continue;
        unrender_entry(i);
        render_entry(i);
    }
}

void strike_screen_push(const StrikeScreen s) {
    if (s <= STRIKE_SCREEN_NONE || s >= STRIKE_SCREEN_COUNT) return;
    if (strike_screen_shown(s)) return; // Pt: Pi.find(same view) -> no second copy
    if (!strike_menu_template(s)) {      // a view this port has no capture of yet (README.md)
        printf("[strike] screen %s not ported yet\n", strike_screen_id(s));
        return;
    }
    if (S.depth == STRIKE_STACK_MAX) {
        unrender_entry(0);
        memmove(&S.stack[0], &S.stack[1], sizeof(S.stack[0]) * (STRIKE_STACK_MAX - 1));
        S.depth--;
    }
    S.stack[S.depth++] = (StackEntry) {.screen = s};
    printf("[strike] screen %s\n", strike_screen_id(s));
    sync_stack();
    strike_loading_complete(); // RA()
}

void strike_screen_pop(void) {
    if (S.depth <= 0) return;
    unrender_entry(S.depth - 1);
    S.depth--;
    sync_stack();
}

// The topmost entry of that screen goes, without a back (the connection popup once it is answered).
void strike_screen_remove(const StrikeScreen s) {
    for (int i = S.depth - 1; i >= 0; i--) {
        if (S.stack[i].screen != s) continue;
        unrender_entry(i);
        memmove(&S.stack[i], &S.stack[i + 1], sizeof(S.stack[0]) * (size_t) (S.depth - i - 1));
        S.depth--;
        sync_stack();
        return;
    }
}

void strike_screen_replace(const StrikeScreen s) {
    strike_screen_clear();
    strike_screen_push(s);
}

void strike_screen_clear(void) {
    while (S.depth > 0) {
        unrender_entry(S.depth - 1);
        S.depth--;
    }
}

// ---------------------------------------------------------------- loading screen

static void loading_render(void) {
    if (!S.ld.shown || !S.loading_holder) return;
    const int c = S.ld.complete, n = S.ld.no_anim;
#define CLS(base) base, c ? " " base "--complete" : "", n ? " " base "--no-anim" : ""
    StrBuf sb = {0};
    sb_appendf(&sb, "<div class=\"loading-screen%s\">", n ? " loading-screen--no-anim" : "");
    sb_append(&sb, "<div class=\"loading-screen__curtains\">");
    sb_appendf(&sb, "<div class=\"%s%s%s\"></div>", CLS("loading-screen__left-curtain"));
    sb_appendf(&sb, "<div class=\"%s%s%s\"></div>", CLS("loading-screen__right-curtain"));
    sb_append(&sb, "</div><div class=\"loading-screen__center\">");
    sb_appendf(&sb, "<div class=\"%s%s%s\"><span>", CLS("loading-screen__loading-info"));
    sb_append_esc(&sb, S.ld.info);
    sb_append(&sb, "</span></div>");
    sb_appendf(&sb, "<div class=\"%s%s%s\">", CLS("loading-screen__cog-holder"));
    sb_appendf(&sb, "<div class=\"loading-screen__cog%s\"><img src=\"./img/cogwheel.svg\"></div></div>", n ? " loading-screen__cog--no-anim" : "");
    sb_appendf(&sb, "<div class=\"%s%s%s\"><img src=\"./img/krunker_logo.png\"></div>", CLS("loading-screen__logo"));
    sb_appendf(&sb, "<div class=\"%s%s%s\"><img src=\"./img/loading-sparkles.svg\"></div>", CLS("loading-screen__sparkles"));
    sb_appendf(&sb, "<div class=\"%s%s%s\"><span>", CLS("loading-screen__tips"));
    sb_append_esc(&sb, TIPS[S.ld.tip]);
    sb_append(&sb, "</span></div></div>");
    sb_appendf(&sb, "<div class=\"%s%s%s\"></div>", CLS("loading-screen__loading-bar-bg"));
    sb_appendf(&sb, "<div class=\"%s%s%s\" style=\"--loading-bar-w: %d%%;\"></div>", CLS("loading-screen__loading-bar"), S.ld.width);
    sb_append(&sb, "</div>");
#undef CLS
    char *html = sb_finish(&sb);
    html_dom_set_html(S.loading_holder, html);
    free(html);
    S.ld.root = html_dom_first_child(S.loading_holder);
}

// fz's state change after the first render: only the bar width / the --complete classes move
// (Mithril patches the existing elements, so running animations are not restarted).
static void loading_patch(void) {
    if (!S.ld.root) return;
    static const char *const PARTS[] = {
        ".loading-screen__left-curtain", ".loading-screen__right-curtain", ".loading-screen__loading-info",
        ".loading-screen__cog-holder", ".loading-screen__logo", ".loading-screen__sparkles", ".loading-screen__tips",
        ".loading-screen__loading-bar-bg", ".loading-screen__loading-bar",
    };
    for (size_t i = 0; i < sizeof(PARTS) / sizeof(PARTS[0]); i++) {
        const HtmlElem el = html_dom_query(S.ld.root, PARTS[i]);
        if (!el) continue;
        char cls[96];
        snprintf(cls, sizeof(cls), "%s--complete", PARTS[i] + 1);
        html_dom_set_class(el, cls, S.ld.complete);
    }
    const HtmlElem bar = html_dom_query(S.ld.root, ".loading-screen__loading-bar");
    if (bar) {
        char w[16];
        snprintf(w, sizeof(w), "%d%%", S.ld.width);
        html_dom_set_style(bar, "--loading-bar-w", w);
    }
}

static int rand_int(const int n) { return n > 0 ? rand() % n : 0; }

// fz: setInterval(randomInterval): +1..10 % while <= 90 and not complete
static void loading_bar_tick(void *user) {
    const int interval = (int) (intptr_t) user;
    S.ld.bar_timer = 0;
    if (!S.ld.shown || S.ld.complete) return;
    if (S.ld.width <= 90) {
        S.ld.width += rand_int(10) + 1;
        loading_patch();
    }
    S.ld.bar_timer = html_timer_add(interval, loading_bar_tick, (void *) (intptr_t) interval);
}

static void loading_cancel_timers(void) {
    if (S.ld.bar_timer) html_timer_cancel(S.ld.bar_timer);
    if (S.ld.min_timer) html_timer_cancel(S.ld.min_timer);
    if (S.ld.remove_timer) html_timer_cancel(S.ld.remove_timer);
    S.ld.bar_timer = S.ld.min_timer = S.ld.remove_timer = 0;
}

static void loading_completed(void);

// RA's setTimeout(u_ - elapsed): the screen has been up for 1500 ms
static void loading_min_fire(void *user) {
    (void) user;
    S.ld.min_timer = 0;
    S.ld.min_done = 1;
    if (S.ld.want_complete) loading_completed();
}

void strike_loading_show(const char *info, const int no_anim) {
    const int was_shown = S.ld.shown;
    // Fm: a loading screen already up is re-rendered with noEnterAnimation; its vb instance (and
    // Se.loadingStarted) is the same while the previous one has not gone
    if (S.ld.bar_timer) html_timer_cancel(S.ld.bar_timer);
    if (S.ld.remove_timer) html_timer_cancel(S.ld.remove_timer);
    S.ld.bar_timer = S.ld.remove_timer = 0;
    snprintf(S.ld.info, sizeof(S.ld.info), "%s", info ? info : "");
    S.ld.no_anim = no_anim || was_shown;
    S.ld.complete = 0;
    S.ld.want_complete = 0;
    if (!was_shown) {
        S.ld.width = 0;
        S.ld.tip = rand_int((int) (sizeof(TIPS) / sizeof(TIPS[0])));
        S.ld.min_done = 0;
        if (S.ld.min_timer) html_timer_cancel(S.ld.min_timer);
        S.ld.min_timer = html_timer_add(LOADING_MIN_MS, loading_min_fire, NULL);
    }
    S.ld.shown = 1;
    loading_render();
    const int interval = rand_int(300) + 200; // randomInterval, fixed for the setInterval
    S.ld.bar_timer = html_timer_add(interval, loading_bar_tick, (void *) (intptr_t) interval);
}

int strike_loading_shown(void) { return S.ld.shown; }

int strike_loading_up(void) { return strike_ui_active() && (!S.mounted || S.ld.shown); }

static void loading_removed(void *user) {
    (void) user;
    S.ld.remove_timer = 0;
    S.ld.shown = 0;
    S.ld.complete = 0;
    S.ld.want_complete = 0;
    S.ld.root = 0;
    if (S.loading_holder) html_dom_set_html(S.loading_holder, "");
}

// Se.loadingComplete = true: the curtains open, Az = 700 ms later the screen goes
static void loading_completed(void) {
    S.ld.complete = 1;
    S.ld.width = 100; // fz.view: loadingComplete -> 100 %, the interval cleared
    if (S.ld.bar_timer) html_timer_cancel(S.ld.bar_timer);
    S.ld.bar_timer = 0;
    loading_patch();
    S.ld.remove_timer = html_timer_add(LOADING_OUT_MS, loading_removed, NULL);
}

void strike_loading_complete(void) {
    if (!S.ld.shown || S.ld.complete || S.ld.want_complete) return;
    S.ld.want_complete = 1;
    if (S.ld.min_done) loading_completed();
}

// ---------------------------------------------------------------- page

static void boot_timeout_fire(void *user);

int strike_ui_dom_present(void) {
    return html_dom_by_id("frvrMenuMobileHolder") != 0;
}

#define STRIKE_CSS_HEIGHT 360.0f // the reference phone viewport's height (CSS px)

// :root --base-scale is .2777777778vh, or .15625vw under (max-aspect-ratio: 16/9). STOPGAP until
// the native engine evaluates @media (the ui-e3-layout agent): set the narrow-window value inline
// on <html>; remove this function once @media lands. The browser applies the @media rule itself.
static void apply_viewport(void) {
    float w = 0.0f, h = 0.0f, dpr = 1.0f;
    html_dom_viewport(&w, &h, &dpr);
#ifndef __EMSCRIPTEN__
    // FRVR's page is a phone page: laid out on a phone's CSS viewport and drawn at its devicePixelRatio
    // (the references: 640x360 CSS px at DPR 2, tools/uidiff/capture-strike.js). Natively the window is
    // that phone: STRIKE_CSS_HEIGHT CSS px tall whatever its size, so the page's absolute px (body's
    // 16px line, .material-icons 24px, 1px borders, margin-left: -2px) keep their size against its
    // --base-scale units, as on the phone. The browser applies its own devicePixelRatio.
    if (h > 0.0f && dpr > 0.0f) {
        const float want = h * dpr / STRIKE_CSS_HEIGHT;
        if (fabsf(want - dpr) > 1e-3f) {
            html_dom_set_device_pixel_ratio(want);
            html_dom_viewport(&w, &h, &dpr);
        }
    }
#endif
    if (w <= 0.0f || h <= 0.0f || (w == S.vw && h == S.vh)) return;
    S.vw = w;
    S.vh = h;
    char buf[32];
    // swe.oncreate: --page-width / --page-height (window.innerWidth / innerHeight)
    snprintf(buf, sizeof(buf), "%d", (int) w);
    html_dom_set_style(S.holder, "--page-width", buf);
    snprintf(buf, sizeof(buf), "%d", (int) h);
    html_dom_set_style(S.holder, "--page-height", buf);
#ifndef __EMSCRIPTEN__
    const int narrow = w / h < 16.0f / 9.0f;
    html_dom_set_style(html_dom_root(), "--base-scale", narrow ? ".15625vw" : "");
    html_dom_set_style(S.holder, "--base-scale-px", narrow ? "calc(var(--page-width) / 640 * 1px)" : "");
#endif
}

// Native page set-up that the browser does not need.
static void native_page(void) {
#ifndef __EMSCRIPTEN__
    const HtmlElem root = html_dom_root();
    // STOPGAP (engine gap: no env()): :root's --*-safe-area are var(--discord-safe-area-inset-*,
    // env(safe-area-inset-*)), which the native engine cannot evaluate; a desktop window has no
    // safe-area insets, so they are 0 (what env() answers there). Remove once env() is supported.
    static const char *const SIDES[] = {"--left-safe-area", "--right-safe-area", "--top-safe-area", "--bottom-safe-area"};
    for (size_t i = 0; i < sizeof(SIDES) / sizeof(SIDES[0]); i++) html_dom_set_style(root, SIDES[i], "0px");
    // The game is drawn under the page natively (FRVR's three.js canvas sat inside it at z-index
    // -2 over the black body); the black would hide it. The web build keeps the page's black.
    html_dom_set_style(root, "background", "transparent");
    if (html_dom_body()) html_dom_set_style(html_dom_body(), "background", "transparent");
#endif
}

void strike_ui_dom_init(Client *client) {
    S.client = client;
    native_page();
    S.holder = html_dom_by_id("frvrMenuMobileHolder");
    S.dom_ready = 1;
    html_timer_add(BOOT_MAX_MS, boot_timeout_fire, NULL);
    srand((unsigned) time(NULL));
    printf("[strike] Krunker Strike page ready (FRVR 1.37.2 mobile UI)\n");
    // the persistent parts of swe.view
    html_dom_set_html(S.holder, "<div class=\"frvr-notifications-holder\"></div><div class=\"frvr-loading-holder\"></div>");
    S.notifications = html_dom_query(S.holder, ".frvr-notifications-holder");
    S.loading_holder = html_dom_query(S.holder, ".frvr-loading-holder");
    apply_viewport();
    strike_menu_install();
    strike_hud_install();
}

static void fm_fire(void *user) { (void) user; S.fm_done = 1; }
static void boot_timeout_fire(void *user) { (void) user; S.boot_timeout = 1; }

// iwe(): the static loading screen gives way to the Mithril mount, which starts on "Loading Game"
// without the enter animation (the bootstrap's Fm(Al, {info: "Loading Game", noEnterAnimation}),
// whose promise resolves pz = 500 ms later)
static void mount(void) {
    S.mounted = 1;
    const HtmlElem stat = html_dom_by_id("frvrLoadingScreen");
    if (stat) html_dom_set_display(stat, "none");
    strike_loading_show("Loading Game", 1);
    html_timer_add(LOADING_FM_MS, fm_fire, NULL);
}

// The game this client loads (FRVR: bundle + assets + gateway) is ready to show the menu.
static int boot_done(const Client *client) {
    const MenuUi *ui = menu_ui();
    if (ui->connect_error[0]) return 1;
    if (client && client->game.ready && ui->connected) return 1;
    return S.boot_timeout;
}

void strike_ui_tick(Client *client) {
    S.client = client;
    if (!S.dom_ready) return;
    apply_viewport();
    menu_send_preview_rect(); // the class preview's box follows #classPreviewCanvas (deduped there)
    if (!S.mounted && boot_done(client)) mount();
    if (S.mounted && !S.booted && S.fm_done) {
        S.booted = 1;
        if (html_ui_is_menu_open()) strike_screen_replace(STRIKE_SCREEN_MAIN_MENU);
    }
    if (!S.booted) return;
    strike_connect_tick(); // the connection popups (strike_connect.c) follow the transport
    strike_menu_tick(client);
    strike_hud_tick(client);
    strike_touch_tick(client);
}

void strike_ui_menu_open(Client *client, const int open) {
    (void) client;
    if (!S.booted) return;
    if (open) {
        // Yh(false): between lives / paused in a game TAP_TO_PLAY (nu(Yw)), else the main menu
        strike_touch_reset();
        if (S.depth == 0) strike_screen_push(client && client->in_game ? STRIKE_SCREEN_TAP_TO_PLAY : STRIKE_SCREEN_MAIN_MENU);
    } else {
        // Yh(true): the game; the screens go, RA() completes a loading screen
        strike_screen_clear();
        strike_loading_complete();
    }
}

int strike_ui_pointer_to_page(void) {
    return !S.booted || S.depth > 0 || S.ld.shown || html_ui_is_menu_open();
}

// ---------------------------------------------------------------- --render-scenario

// state.strike (tools/uidiff/scenarios/strike/*.json, ui_scenario.c): the page booted at once (no
// loading screen) unless "boot" keeps index.html's static one, then
//   "screen": "<VIEW_ID>"   the stack becomes that one screen (the capture's Pi = [view])
//   "settingsTab": n        SETTINGS' tab n (Presets 0 .. Other 4)
//   "leaderboard": [{name, classIndex, you, score, kills}]  LEADERBOARD_PLAYERS' rows (no score / kills:
//                           empty stat cells, as the capture's)
//   "loading": "<info>"     FRVR's Mithril loading screen (Fm(Al, {info})) from the capture clock 0
//   "connection": {"state": "lost" | "updating" | "reconnecting", ...} (strike_connect.c)
//   "killfeed": [{killer, victim, killerIsYou, victimIsYou, weapon: "icon_1", headshot}]
//   "killcard": {killer, weapon, level, score, classIndex}
//   "end": 1 | 0 | -1       the round end (victory / defeat / draw)
//   and strike_meta.c's keys (strike_meta_scenario): "progression", "claimed", "reward",
//   "battlepassTab", "matchDetails"
void strike_ui_scenario(Client *client, const cJSON *st) {
    if (!st || !strike_ui_active() || !S.dom_ready) return;
    S.client = client;
    const cJSON *boot = cJSON_GetObjectItemCaseSensitive(st, "boot");
    if (cJSON_IsTrue(boot)) return; // index.html's static loading screen stays
    if (!S.mounted) {
        S.mounted = 1;
        const HtmlElem stat = html_dom_by_id("frvrLoadingScreen");
        if (stat) html_dom_set_display(stat, "none");
    }
    S.booted = S.fm_done = 1;
    loading_cancel_timers();
    loading_removed(NULL);
    // the fixtures first: the screen fills itself from them as it is rendered
    strike_menu_scenario(cJSON_GetObjectItemCaseSensitive(st, "account"), client ? client->menu_name : NULL);
    strike_menu_scenario_board(cJSON_GetObjectItemCaseSensitive(st, "leaderboard"));
    const cJSON *screen = cJSON_GetObjectItemCaseSensitive(st, "screen");
    if (cJSON_IsString(screen)) {
        strike_screen_clear();
        for (int i = 1; i < STRIKE_SCREEN_COUNT; i++) {
            if (!strcmp(SCREEN_IDS[i], screen->valuestring)) strike_screen_push((StrikeScreen) i);
        }
    } else if (html_ui_is_menu_open() && S.depth == 0) {
        strike_screen_push(STRIKE_SCREEN_MAIN_MENU);
    }
    const cJSON *tab = cJSON_GetObjectItemCaseSensitive(st, "settingsTab");
    if (cJSON_IsNumber(tab)) {
        char a[32];
        snprintf(a, sizeof(a), "settings-tab:%d", tab->valueint);
        strike_menu_action(a);
    }
    const cJSON *loading = cJSON_GetObjectItemCaseSensitive(st, "loading");
    if (cJSON_IsString(loading)) strike_loading_show(loading->valuestring, 0);
    strike_connect_scenario(st); // connection: the Lost Connection / Game Updating popups, Reconnecting...
    strike_hud_scenario(client, st);
    strike_meta_scenario(client, st); // progression / reward / battlepassTab / matchDetails (strike_meta.c)
    strike_menu_scenario_state(st);   // itemSelection (T5 over Customize), product (U2) (strike_menu.c)
    strike_reward_scenario(st);       // rewardScreen / rewardChoice (strike_reward.c)
}
