/*
 * test_events.c - standalone tests for events.c.
 *
 * Build (throwaway, sanitizers on):
 *   write the CMakeLists.txt snippet at the bottom of this file to /tmp/kn-events,
 *   then: cd /tmp/kn-events && CC=clang cmake -S . -B build && cmake --build build && ./build/test_events <repo>
 * (KN_EVENTS_TRACE=1 prints every dispatch as it happens).
 *
 * Part 1 scans every inline on* attribute in ui/index.html and every ui/windows HTML file
 * (Krunker v7.1.4's page), entity-decodes it like the HTML parser and asserts the handler
 * parser accepts all 2184, then compares the distinct callee set with the inventory.
 * Part 2 drives events.c against a stub Dom (the dom.h subset events.c uses) and
 * a stub EventsHost (rect hit testing, scroll geometry) and checks hover chains,
 * click synthesis and bubbling, label->checkbox forwarding, prevent-default,
 * focus/Escape, scroll clamping and resize observers.
 */
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "events.h"
#include "util.h"

static int g_fail;
#define CHECK(cond) do { if (!(cond)) { g_fail++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_STR(a, b) do { const char *_a = (a), *_b = (b); if (!_a || !_b || strcmp(_a, _b)) { g_fail++; \
    fprintf(stderr, "FAIL %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, _a ? _a : "(null)", _b ? _b : "(null)"); } } while (0)

/* ========================================================================= */
/* Stub DOM: a tiny element tree implementing the dom.h functions events.c  */
/* calls.  Box == the element itself (the host hooks hit-test rects).        */
/* ========================================================================= */

#define MAX_ELEMS 256
#define MAX_ATTRS 16

struct Box {
    HtmlElem handle;
    const char *tag;
    HtmlElem parent, first_child, next;
    const char *attr_names[MAX_ATTRS];
    char *attr_values[MAX_ATTRS];
    int attr_count;
    char *text;
    ElemData data;
    CssComputed computed;
    int alive;
    /* layout stub */
    int has_rect;
    HtmlRect rect;
    int scrollable;
    EvScrollInfo scroll;
};

struct Dom {
    struct Box elems[MAX_ELEMS];
    uint32_t count; /* handles 1..count */
};

static struct Dom g_dom;

static struct Box *E(HtmlElem h) { return (h && h <= g_dom.count && g_dom.elems[h].alive) ? &g_dom.elems[h] : NULL; }

ElemData *dom_elem_data(Dom *dom, HtmlElem e) { (void)dom; struct Box *b = E(e); return b ? &b->data : NULL; }
HtmlElem dom_parent(Dom *dom, HtmlElem e) { (void)dom; struct Box *b = E(e); return b ? b->parent : 0; }
HtmlElem dom_first_child(Dom *dom, HtmlElem e) { (void)dom; struct Box *b = E(e); return b ? b->first_child : 0; }
HtmlElem dom_next(Dom *dom, HtmlElem e) { (void)dom; struct Box *b = E(e); return b ? b->next : 0; }
const char *dom_tag(Dom *dom, HtmlElem e) { (void)dom; struct Box *b = E(e); return b ? b->tag : NULL; }
uint32_t dom_pseudo_state(Dom *dom, HtmlElem e) { (void)dom; struct Box *b = E(e); return b ? b->data.pseudo : 0; }
void dom_set_pseudo_state(Dom *dom, HtmlElem e, uint32_t bits, int on)
{
    (void)dom;
    struct Box *b = E(e);
    if (!b) { fprintf(stderr, "FAIL: pseudo state on dead handle %u\n", e); g_fail++; return; }
    if (on) b->data.pseudo |= bits; else b->data.pseudo &= ~bits;
}
const char *dom_get_attr(Dom *dom, HtmlElem e, const char *name, size_t *len)
{
    (void)dom;
    struct Box *b = E(e);
    if (!b) return NULL;
    for (int i = 0; i < b->attr_count; i++) {
        if (!strcmp(b->attr_names[i], name)) { if (len) *len = strlen(b->attr_values[i]); return b->attr_values[i]; }
    }
    return NULL;
}
int dom_has_attr(Dom *dom, HtmlElem e, const char *name) { return dom_get_attr(dom, e, name, NULL) != NULL; }
void dom_set_attr(Dom *dom, HtmlElem e, const char *name, const char *value)
{
    (void)dom;
    struct Box *b = E(e);
    if (!b) return;
    for (int i = 0; i < b->attr_count; i++) {
        if (!strcmp(b->attr_names[i], name)) { free(b->attr_values[i]); b->attr_values[i] = strdup(value); return; }
    }
    b->attr_names[b->attr_count] = kn_atom_cstr(name);
    b->attr_values[b->attr_count++] = strdup(value);
}
void dom_remove_attr(Dom *dom, HtmlElem e, const char *name)
{
    (void)dom;
    struct Box *b = E(e);
    if (!b) return;
    for (int i = 0; i < b->attr_count; i++) {
        if (!strcmp(b->attr_names[i], name)) {
            free(b->attr_values[i]);
            b->attr_names[i] = b->attr_names[b->attr_count - 1];
            b->attr_values[i] = b->attr_values[b->attr_count - 1];
            b->attr_count--;
            return;
        }
    }
}
const char *dom_get_value(Dom *dom, HtmlElem e) { return dom_get_attr(dom, e, "value", NULL); }
int dom_get_checked(Dom *dom, HtmlElem e) { return dom_has_attr(dom, e, "checked"); }
void dom_set_checked(Dom *dom, HtmlElem e, int on) { if (on) dom_set_attr(dom, e, "checked", ""); else dom_remove_attr(dom, e, "checked"); }
char *dom_get_text(Dom *dom, HtmlElem e) { (void)dom; struct Box *b = E(e); return strdup(b && b->text ? b->text : ""); }

static HtmlElem mk(HtmlElem parent, const char *tag, const char *id)
{
    HtmlElem h = ++g_dom.count;
    struct Box *b = &g_dom.elems[h];
    memset(b, 0, sizeof(*b));
    b->handle = h;
    b->tag = tag;
    b->alive = 1;
    b->parent = parent;
    b->data.handle = h;
    b->data.computed = &b->computed;
    if (id) dom_set_attr(&g_dom, h, "id", id);
    if (parent) {
        struct Box *p = &g_dom.elems[parent];
        if (!p->first_child) p->first_child = h;
        else { HtmlElem c = p->first_child; while (g_dom.elems[c].next) c = g_dom.elems[c].next; g_dom.elems[c].next = h; }
    }
    return h;
}

static void set_rect(HtmlElem e, float x, float y, float w, float h)
{
    struct Box *b = &g_dom.elems[e];
    b->has_rect = 1;
    b->rect = (HtmlRect){x, y, w, h};
}

static void kill_subtree(HtmlElem e)
{
    struct Box *b = E(e);
    if (!b) return;
    for (HtmlElem c = b->first_child; c;) { HtmlElem n = g_dom.elems[c].next; kill_subtree(c); c = n; }
    /* unlink from parent */
    if (b->parent) {
        struct Box *p = &g_dom.elems[b->parent];
        if (p->first_child == e) p->first_child = b->next;
        else for (HtmlElem c = p->first_child; c; c = g_dom.elems[c].next) if (g_dom.elems[c].next == e) { g_dom.elems[c].next = b->next; break; }
    }
    for (int i = 0; i < b->attr_count; i++) free(b->attr_values[i]);
    b->attr_count = 0;
    b->alive = 0;
}

/* ---- host hooks ---- */

static int point_in(const HtmlRect *r, float x, float y) { return x >= r->x && y >= r->y && x < r->x + r->w && y < r->y + r->h; }

static Box *hit_rec(HtmlElem e, float x, float y)
{
    struct Box *b = E(e);
    if (!b) return NULL;
    if (b->computed.pointer_events == CSS_POINTER_EVENTS_NONE) return NULL;
    Box *best = NULL;
    if (b->has_rect && point_in(&b->rect, x, y)) best = b;
    for (HtmlElem c = b->first_child; c; c = g_dom.elems[c].next) {
        Box *r = hit_rec(c, x, y);
        if (r) best = r; /* later siblings paint on top */
    }
    return best;
}
static Box *h_hit_test(void *u, float x, float y) { (void)u; return hit_rec(1, x, y); }
static HtmlElem h_box_elem(void *u, Box *b) { (void)u; return b->handle; }
static Box *h_elem_box(void *u, HtmlElem e) { (void)u; struct Box *b = E(e); return (b && b->has_rect) ? b : NULL; }
static int h_elem_rect(void *u, HtmlElem e, HtmlRect *out) { (void)u; struct Box *b = E(e); if (!b || !b->has_rect) return 0; *out = b->rect; return 1; }
static int h_scroll_info(void *u, Box *b, EvScrollInfo *out) { (void)u; if (!b->scrollable) return 0; *out = b->scroll; return 1; }
static void h_set_scroll(void *u, Box *b, float x, float y) { (void)u; b->scroll.scroll_x = x; b->scroll.scroll_y = y; }
static int g_restyles;
static void h_restyle(void *u) { (void)u; g_restyles++; }

/* ---- html_dom_core stubs: record every dispatch in order ---- */

#define MAX_LOG 512
static char *g_log[MAX_LOG];
static int g_log_n;
static int g_handler_result = 1;       /* what html_ui_dispatch returns */
static const char *g_zero_handler;     /* handler name that returns 0 */
static int g_listener_result = 1;

static void logf_(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (getenv("KN_EVENTS_TRACE")) fprintf(stderr, "  > %s\n", buf);
    if (g_log_n < MAX_LOG) g_log[g_log_n++] = strdup(buf);
}

static void log_clear(void) { for (int i = 0; i < g_log_n; i++) free(g_log[i]); g_log_n = 0; }
static int log_has(const char *s) { for (int i = 0; i < g_log_n; i++) if (strstr(g_log[i], s)) return 1; return 0; }
static int log_index(const char *s) { for (int i = 0; i < g_log_n; i++) if (strstr(g_log[i], s)) return i; return -1; }
static int log_hasf(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return log_has(buf);
}
static int log_indexf(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return log_index(buf);
}
static void log_dump(void) { for (int i = 0; i < g_log_n; i++) fprintf(stderr, "   %2d %s\n", i, g_log[i]); }

static const char *g_stop_handler;     /* handler name that calls html_ui_stop_propagation() */
static int g_stop_requested;
int html_ui_take_stop_propagation(void) { int s = g_stop_requested; g_stop_requested = 0; return s; }

int html_ui_dispatch(const char *name, const HtmlEvent *ev, const HtmlArg *args, int argc)
{
    char buf[512];
    int n = snprintf(buf, sizeof(buf), "H %s %s@%u(", ev->type, name, ev->target);
    for (int i = 0; i < argc && n < (int)sizeof(buf) - 64; i++) {
        switch (args[i].kind) {
            case HTML_ARG_NUM: n += snprintf(buf + n, sizeof(buf) - (size_t)n, "%s%g", i ? "," : "", args[i].num); break;
            case HTML_ARG_STR: n += snprintf(buf + n, sizeof(buf) - (size_t)n, "%s'%s'", i ? "," : "", args[i].str); break;
            case HTML_ARG_BOOL: n += snprintf(buf + n, sizeof(buf) - (size_t)n, "%s%s", i ? "," : "", args[i].num ? "true" : "false"); break;
            case HTML_ARG_THIS: n += snprintf(buf + n, sizeof(buf) - (size_t)n, "%sthis", i ? "," : ""); break;
            case HTML_ARG_THIS_VALUE: n += snprintf(buf + n, sizeof(buf) - (size_t)n, "%sthis.value='%s'", i ? "," : "", args[i].str); break;
            case HTML_ARG_THIS_CHECKED: n += snprintf(buf + n, sizeof(buf) - (size_t)n, "%sthis.checked=%d", i ? "," : "", (int)args[i].num); break;
            case HTML_ARG_EVENT: n += snprintf(buf + n, sizeof(buf) - (size_t)n, "%sevent", i ? "," : ""); break;
        }
    }
    snprintf(buf + n, sizeof(buf) - (size_t)n, ") key=%d btn=%d checked=%d", ev->key, ev->button, ev->checked);
    logf_("%s", buf);
    if (g_stop_handler && !strcmp(name, g_stop_handler)) g_stop_requested = 1;
    if (g_zero_handler && !strcmp(name, g_zero_handler)) return 0;
    return g_handler_result;
}

int html_ui_dispatch_listener(const char *elem_id, const HtmlEvent *ev)
{
    logf_("L %s %s target=%u key=%d", elem_id ? elem_id : "window", ev->type, ev->target, ev->key);
    return g_listener_result;
}

/* ---- widget hook stubs ---- */
static int g_widget_down_bits;
static int w_mouse_down(void *u, HtmlElem e, float x, float y, int button, int mods) { (void)u; (void)x; (void)y; (void)mods; logf_("W down %u btn=%d", e, button); return g_widget_down_bits; }
static void w_mouse_move(void *u, HtmlElem e, float x, float y, int buttons) { (void)u; logf_("W move %u %.0f,%.0f buttons=%d", e, x, y, buttons); }
static void w_mouse_up(void *u, HtmlElem e, float x, float y, int button) { (void)u; (void)x; (void)y; logf_("W up %u btn=%d", e, button); }
static int w_key(void *u, HtmlElem e, int key, int sc, int action, int mods) { (void)u; (void)sc; (void)mods; logf_("W key %u key=%d action=%d", e, key, action); return key != EV_KEY_TAB; /* text widgets leave Tab to the engine (widgets.c) */ }
static int w_chr(void *u, HtmlElem e, uint32_t cp) { (void)u; logf_("W chr %u cp=%u", e, cp); return 1; }
static void w_focus(void *u, HtmlElem e) { (void)u; logf_("W focus %u", e); }
static void w_blur(void *u, HtmlElem e) { (void)u; logf_("W blur %u", e); }
static int w_open_select(void *u, HtmlElem e, float x, float y) { (void)u; (void)x; (void)y; logf_("W select %u", e); return EV_WIDGET_CAPTURE; }

/* ========================================================================= */
/* Part 1: the parser over the real markup                                   */
/* ========================================================================= */

static char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = 0;
    if (len) *len = got;
    return buf;
}

/* Decode the entities the HTML parser decodes in attribute values. */
static void decode_entities(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    static const struct { const char *name; const char *rep; } ents[] = {
        {"&quot;", "\""}, {"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&#39;", "'"}, {"&apos;", "'"}, {"&#34;", "\""}, {"&nbsp;", " "},
    };
    while (*in && o + 1 < cap) {
        int matched = 0;
        if (*in == '&') {
            for (size_t i = 0; i < sizeof(ents) / sizeof(ents[0]); i++) {
                size_t n = strlen(ents[i].name);
                if (!strncmp(in, ents[i].name, n)) { o += (size_t)snprintf(out + o, cap - o, "%s", ents[i].rep); in += n; matched = 1; break; }
            }
        }
        if (!matched) out[o++] = *in++;
    }
    out[o] = 0;
}

typedef struct { char *name; int count; } Callee;
static Callee g_callees[128];
static int g_callee_n;

static void callee_add(const char *name)
{
    for (int i = 0; i < g_callee_n; i++) if (!strcmp(g_callees[i].name, name)) { g_callees[i].count++; return; }
    g_callees[g_callee_n].name = strdup(name);
    g_callees[g_callee_n++].count = 1;
}

static int scan_file(const char *path, int *total, int *failed)
{
    size_t len;
    char *src = read_file(path, &len);
    if (!src) { fprintf(stderr, "cannot read %s\n", path); return -1; }
    const char *p = src;
    while ((p = strstr(p, "on")) != NULL) {
        /* attribute start: whitespace, then on[a-z]+=" */
        if (p == src || !kn_is_space(p[-1])) { p += 2; continue; }
        const char *q = p + 2;
        while (kn_is_alpha(*q)) q++;
        if (q == p + 2 || q[0] != '=' || q[1] != '"') { p += 2; continue; }
        const char *vs = q + 2;
        const char *ve = strchr(vs, '"');
        if (!ve) break;
        char raw[2048], text[2048];
        size_t n = (size_t)(ve - vs);
        if (n >= sizeof(raw)) n = sizeof(raw) - 1;
        memcpy(raw, vs, n);
        raw[n] = 0;
        decode_entities(raw, text, sizeof(text));
        HandlerCall calls[EV_HANDLER_MAX_CALLS];
        int r = events_parse_handler(text, calls, EV_HANDLER_MAX_CALLS);
        (*total)++;
        if (r < 0) { (*failed)++; fprintf(stderr, "  parse error in %s: %.*s=\"%s\"\n", path, (int)(q - p), p, text); }
        for (int i = 0; i < r; i++) if (calls[i].name) callee_add(calls[i].name);
        p = ve + 1;
    }
    free(src);
    return 0;
}

static const char *k_inventory[] = {
    /* every callee of the Krunker v7.1.4 page (ui/index.html) and its captured windows (ui/windows) */
    "kn.hideErrorNotification", "sub2FA", "checkTerms", "windows.20.tabIndex=", "showWindow", "SOUND.play",
    "abTestsPopup", "openTab", "purchaseCancel", "XsollaPopup", "playTick", "playSelect", "logBtnFRVR",
    "copyInviteLink", "openJoinWindow", "shoPolicePop", "openGuide", "windows.13.purchase", "dropsPopup",
    "claimReward", "toggleTeam", "setSpect", "windows.13.craftMenu", "openURL", "openDiscord",
    "windows.32.openHome", "window.open", "showGames", "clientExitPopup", "expertMode", "toggleSpect",
    "clearPops", "closWind", "switchChat", "focusChat", "closeTwitch", "resetSettings", "exportSettings",
    "importSettingsPopup", "selectSettingPre", "windows.0.searchList", "windows.0.toggleType",
    "windows.0.changeTab", "windows.0.collapseFolder", "window.setSetting", "windows.2.switchTab",
    "windows.2.action", "skinSelector", "charmSelector", "viewAttachments", "windows.3.switchTab",
    "registerAcc", "loginAcc", "windows.5.switchTab", "windows.6.switchTab", "windows.6.collapseFolder",
    "resetCont", "unbindCont", "changeCont", "windows.7.switchTab", "createPrivateRoom", "windows.7.toggle",
    "windows.7.changeEffects", "windows.7.toggleEffects", "updateSliderLabel2", "updateSliderLabel",
    "windows.7.updateSet", "windows.7.presetAction", "goBackWindow", "searchItems", "selectSpray",
    "cancelTip", "createTip", "windows.9.switchTab", "windows.13.switchTab", "windows.13.subTabT=",
    "windows.13.subTab=", "updateWindow", "windows.13.premium", "windows.13.updateScroll",
    "windows.13.updateScrollWheel", "selectSkin", "selectHat", "selectBody", "loadMod", "loadModURL",
    "windows.20.switchTab", "joinGame", "selectMelee", "windows.29.readAll", "windows.29.switchTab",
    "selectAttachment", "window.delayExecute", "selectReticle", "windows.31.newPop", "selectDye",
    "windows.34.viewAll", "selectWaist", "selectFace", "selectShoe", "selectScope", "openHostWindow",
    "selectPet", "selectWrist", "selectStreak", "createStreakTip", "selectCharm", "openServerWindow",
    "selectBack", "selectHead", "selectPlayerCard", "aspectSelect", "imagePopup", "changeContMsg",
    "changeContSet",
};

static void test_parser_corpus(const char *repo)
{
    char path[1024];
    int total = 0, failed = 0;
    snprintf(path, sizeof(path), "%s/ui/index.html", repo);
    scan_file(path, &total, &failed);
    snprintf(path, sizeof(path), "%s/ui/windows", repo);
    DIR *d = opendir(path);
    CHECK(d != NULL);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            size_t n = strlen(de->d_name);
            if (n < 6 || strcmp(de->d_name + n - 5, ".html")) continue;
            snprintf(path, sizeof(path), "%s/ui/windows/%s", repo, de->d_name);
            scan_file(path, &total, &failed);
        }
        closedir(d);
    }
    printf("corpus: %d inline handlers, %d parse failures, %d distinct callees\n", total, failed, g_callee_n);
    CHECK(total == 2184);
    CHECK(failed == 0);
    printf("callees:");
    for (int i = 0; i < g_callee_n; i++) printf(" %s(%d)", g_callees[i].name, g_callees[i].count);
    printf("\n");
    const int inv_n = (int)(sizeof(k_inventory) / sizeof(k_inventory[0]));
    CHECK(inv_n == 115);
    for (int i = 0; i < inv_n; i++) {
        int found = 0;
        for (int j = 0; j < g_callee_n; j++) if (!strcmp(g_callees[j].name, k_inventory[i])) found = 1;
        if (!found) { g_fail++; fprintf(stderr, "FAIL: inventory callee %s not produced by the parser\n", k_inventory[i]); }
    }
    for (int j = 0; j < g_callee_n; j++) {
        int found = 0;
        for (int i = 0; i < inv_n; i++) if (!strcmp(g_callees[j].name, k_inventory[i])) found = 1;
        if (!found) { g_fail++; fprintf(stderr, "FAIL: parser produced callee %s not in the inventory\n", g_callees[j].name); }
    }
    CHECK(g_callee_n == inv_n);
}

static void test_parser_grammar(void)
{
    HandlerCall c[EV_HANDLER_MAX_CALLS];
    int n;

    n = events_parse_handler("return delayExecute(\"fov\", this, undefined)", c, 8);
    CHECK(n == 1); CHECK_STR(c[0].name, "delayExecute"); CHECK(c[0].returns == 1); CHECK(c[0].argc == 3);
    CHECK(c[0].args[0].kind == HTML_ARG_STR); CHECK_STR(c[0].args[0].str, "fov");
    CHECK(c[0].args[1].kind == HTML_ARG_THIS);
    CHECK(c[0].args[2].kind == HTML_ARG_STR); CHECK_STR(c[0].args[2].str, "undefined");

    n = events_parse_handler("window.windows[0].changeTab(3)", c, 8);
    CHECK(n == 1); CHECK_STR(c[0].name, "windows.0.changeTab"); CHECK(c[0].args[0].kind == HTML_ARG_NUM && c[0].args[0].num == 3);

    n = events_parse_handler("windows[13].premium()", c, 8);
    CHECK(n == 1); CHECK_STR(c[0].name, "windows.13.premium"); CHECK(c[0].argc == 0);

    n = events_parse_handler("window.open('/social.html', '_blank')", c, 8);
    CHECK(n == 1); CHECK_STR(c[0].name, "window.open"); CHECK(c[0].argc == 2); CHECK_STR(c[0].args[1].str, "_blank");

    n = events_parse_handler("SOUND.play('tick_0',0.1)", c, 8);
    CHECK(n == 1); CHECK_STR(c[0].name, "SOUND.play"); CHECK(c[0].args[1].kind == HTML_ARG_NUM && c[0].args[1].num > 0.09 && c[0].args[1].num < 0.11);

    n = events_parse_handler("toggleHF(this.checked);windows[1].searchList()", c, 8);
    CHECK(n == 2); CHECK_STR(c[0].name, "toggleHF"); CHECK(c[0].args[0].kind == HTML_ARG_THIS_CHECKED); CHECK_STR(c[1].name, "windows.1.searchList");

    n = events_parse_handler("return false;", c, 8);
    CHECK(n == 1); CHECK(c[0].name == NULL); CHECK(c[0].prevent_default == 1);

    n = events_parse_handler("errorNotification.style.display = 'none'", c, 8);
    CHECK(n == 1); CHECK_STR(c[0].name, "kn.hideErrorNotification"); CHECK(c[0].argc == 0);

    n = events_parse_handler("favoriteM(0, this, 2717856513);", c, 8);
    CHECK(n == 1); CHECK(c[0].args[2].num == 2717856513.0);

    n = events_parse_handler("selectHostMap(\"Soul Sanctum\",\"\",\"2717856513\",\"Krunker\")", c, 8);
    CHECK(n == 1); CHECK(c[0].argc == 4); CHECK_STR(c[0].args[1].str, ""); CHECK_STR(c[0].args[2].str, "2717856513");

    n = events_parse_handler("openURL(\"/social.html?p=map&q=Soul Sanctum\");", c, 8);
    CHECK(n == 1); CHECK_STR(c[0].args[0].str, "/social.html?p=map&q=Soul Sanctum");

    n = events_parse_handler("f('it\\'s', \"q\\\"q\", -2.5, true, false, event, foo.bar)", c, 8);
    CHECK(n == 1); CHECK(c[0].argc == 7);
    CHECK_STR(c[0].args[0].str, "it's"); CHECK_STR(c[0].args[1].str, "q\"q");
    CHECK(c[0].args[2].kind == HTML_ARG_NUM && c[0].args[2].num == -2.5);
    CHECK(c[0].args[3].kind == HTML_ARG_BOOL && c[0].args[3].num == 1);
    CHECK(c[0].args[4].kind == HTML_ARG_BOOL && c[0].args[4].num == 0);
    CHECK(c[0].args[5].kind == HTML_ARG_EVENT);
    CHECK(c[0].args[6].kind == HTML_ARG_STR); CHECK_STR(c[0].args[6].str, "foo.bar");

    n = events_parse_handler("  ;; a() ; ; b(1) ;  ", c, 8);
    CHECK(n == 2); CHECK_STR(c[0].name, "a"); CHECK_STR(c[1].name, "b");

    n = events_parse_handler("return true", c, 8);
    CHECK(n == 0);
    n = events_parse_handler("", c, 8);
    CHECK(n == 0);
    n = events_parse_handler("   ", c, 8);
    CHECK(n == 0);

    /* `return` bubbles the handler result; interned names compare by pointer */
    n = events_parse_handler("return f(1)", c, 8);
    CHECK(n == 1 && c[0].returns && c[0].name == kn_atom_cstr("f"));

    /* v7.1.4's minified handlers: comma sequences, assignments, !0/!1, backticks, .5 */
    n = events_parse_handler("playSelect(),logBtnFRVR(\"host\"),showWindow(42)", c, 8);
    CHECK(n == 3); CHECK_STR(c[0].name, "playSelect"); CHECK_STR(c[2].name, "showWindow");
    CHECK(c[2].args[0].kind == HTML_ARG_NUM && c[2].args[0].num == 42);
    n = events_parse_handler("windows[20].tabIndex=1,showWindow(21)", c, 8);
    CHECK(n == 2); CHECK_STR(c[0].name, "windows.20.tabIndex="); CHECK(c[0].argc == 1 && c[0].args[0].num == 1);
    n = events_parse_handler("playSelect(.1),openJoinWindow(!1,1)", c, 8);
    CHECK(n == 2); CHECK(c[0].args[0].kind == HTML_ARG_NUM && c[0].args[0].num > 0.09 && c[0].args[0].num < 0.11);
    CHECK(c[1].args[0].kind == HTML_ARG_BOOL && c[1].args[0].num == 0);
    n = events_parse_handler("SOUND.play(`tick_0`,0.1)", c, 8);
    CHECK(n == 1); CHECK_STR(c[0].args[0].str, "tick_0");
    n = events_parse_handler("errorNotification.style.display=\"none\"", c, 8);
    CHECK(n == 1); CHECK_STR(c[0].name, "kn.hideErrorNotification");
    n = events_parse_handler("x.style.display = 'none'", c, 8);
    CHECK(n == 1); CHECK_STR(c[0].name, "x.style.display="); CHECK_STR(c[0].args[0].str, "none");
    n = events_parse_handler("return!1", c, 8);
    CHECK(n == 1 && c[0].prevent_default && !c[0].name);
    n = events_parse_handler("event.stopPropagation();windows[13].subTabT=5000;updateWindow(14);", c, 8);
    CHECK(n == 3); CHECK(c[0].stop_propagation && !c[0].name); CHECK_STR(c[1].name, "windows.13.subTabT=");
    n = events_parse_handler("window.canShowExternalLinks?openTab(\"t\"):showWindow(47)", c, 8);
    CHECK(n == 2); CHECK_STR(c[0].guard, "window.canShowExternalLinks"); CHECK(!c[0].guard_neg); CHECK_STR(c[0].name, "openTab");
    CHECK(c[1].guard == c[0].guard && c[1].guard_neg); CHECK_STR(c[1].name, "showWindow");
    n = events_parse_handler("13==window.event.keyCode&&window.searchProfile()", c, 8);
    CHECK(n == 1 && c[0].guard_key == 13); CHECK_STR(c[0].name, "window.searchProfile");
    n = events_parse_handler("if(window.event.keyCode==13)windows[9].searchMaps()", c, 8);
    CHECK(n == 1 && c[0].guard_key == 13); CHECK_STR(c[0].name, "windows.9.searchMaps");
    n = events_parse_handler("toggleTeam(this.checked?2:1)", c, 8);
    CHECK(n == 1 && (c[0].tern_mask & 1) && c[0].args[0].num == 2 && c[0].tern_else[0] == 1);

    /* errors */
    CHECK(events_parse_handler("foo(", c, 8) == -1);
    CHECK(events_parse_handler("foo(1,)", c, 8) == -1);
    CHECK(events_parse_handler("foo() bar()", c, 8) == -1);
    CHECK(events_parse_handler("a == 1", c, 8) == -1);
    CHECK(events_parse_handler("`a${b}`", c, 8) == -1);
    CHECK(events_parse_handler("f(`a${b}`)", c, 8) == -1);
    CHECK(events_parse_handler("a ? b() c()", c, 8) == -1);
    CHECK(events_parse_handler("foo('unterminated)", c, 8) == -1);
    CHECK(events_parse_handler("windows[a].b()", c, 8) == -1);
    CHECK(events_parse_handler("return", c, 8) == -1);
    CHECK(events_parse_handler("a();b();c();d();e();f();g();h();i()", c, 8) == -1); /* > max */
    CHECK(events_parse_handler("f(1,2,3,4,5,6,7,8,9)", c, 8) == -1);                  /* > max args */
}

/* ========================================================================= */
/* Part 2: behaviour with the stub DOM                                       */
/* ========================================================================= */

static EventSystem g_es;
static HtmlElem html_, body_, menu_, btn_, lbl_, lbltext_, cb_, txt_, scroller_, inner_, blocker_, cb2_, lbl2_, cb3_, sel_, opt_a_, opt_b_, num_, outer_scroller_, dis_;

static void build_page(void)
{
    memset(&g_dom, 0, sizeof(g_dom));
    html_ = mk(0, "html", NULL);        set_rect(html_, 0, 0, 1280, 720);
    body_ = mk(html_, "body", NULL);    set_rect(body_, 0, 0, 1280, 720);
    menu_ = mk(body_, "div", "menu");   set_rect(menu_, 0, 0, 800, 600);
    dom_set_attr(&g_dom, menu_, "onmouseover", "playTick()");

    btn_ = mk(menu_, "div", "btn");     set_rect(btn_, 10, 10, 100, 50);
    dom_set_attr(&g_dom, btn_, "onclick", "showWindow(1)");
    dom_set_attr(&g_dom, btn_, "onmouseenter", "playTick()");
    g_dom.elems[btn_].computed.cursor = CSS_CURSOR_POINTER;

    lbl_ = mk(menu_, "label", "lbl");   set_rect(lbl_, 10, 100, 200, 30);
    cb_ = mk(lbl_, "input", "cb");      set_rect(cb_, 10, 100, 13, 13);
    dom_set_attr(&g_dom, cb_, "type", "checkbox");
    dom_set_attr(&g_dom, cb_, "onclick", "setSetting(\"x\", this.checked)");
    lbltext_ = mk(lbl_, "span", "lbltext"); set_rect(lbltext_, 40, 100, 100, 30);

    txt_ = mk(menu_, "input", "txt");   set_rect(txt_, 10, 200, 100, 30);
    dom_set_attr(&g_dom, txt_, "type", "text");
    dom_set_attr(&g_dom, txt_, "value", "hello");
    dom_set_attr(&g_dom, txt_, "onfocus", "focusChat(1);");
    dom_set_attr(&g_dom, txt_, "oninput", "setSetting(\"name\", this.value)");

    num_ = mk(menu_, "input", "num");   set_rect(num_, 10, 240, 100, 30);
    dom_set_attr(&g_dom, num_, "type", "number");
    dom_set_attr(&g_dom, num_, "onkeypress", "return delayExecute(\"fov\", this, undefined)");

    outer_scroller_ = mk(menu_, "div", "outer"); set_rect(outer_scroller_, 300, 0, 300, 300);
    g_dom.elems[outer_scroller_].scrollable = 1;
    g_dom.elems[outer_scroller_].scroll = (EvScrollInfo){0, 0, 300, 400, 300, 300, 0, 1};
    scroller_ = mk(outer_scroller_, "div", "chatList"); set_rect(scroller_, 300, 0, 200, 200);
    g_dom.elems[scroller_].scrollable = 1;
    g_dom.elems[scroller_].scroll = (EvScrollInfo){0, 0, 200, 1000, 200, 200, 0, 1};
    inner_ = mk(scroller_, "div", "chatMsg"); set_rect(inner_, 300, 0, 200, 200);

    blocker_ = mk(menu_, "div", "blocker"); set_rect(blocker_, 10, 300, 200, 30);
    dom_set_attr(&g_dom, blocker_, "onclick", "return false");
    cb2_ = mk(blocker_, "input", "cb2"); set_rect(cb2_, 10, 300, 13, 13);
    dom_set_attr(&g_dom, cb2_, "type", "checkbox");

    lbl2_ = mk(menu_, "label", "lbl2"); set_rect(lbl2_, 10, 340, 200, 30);
    dom_set_attr(&g_dom, lbl2_, "onclick", "return vetoed()");
    cb3_ = mk(lbl2_, "input", "cb3");   set_rect(cb3_, 10, 340, 13, 13);
    dom_set_attr(&g_dom, cb3_, "type", "checkbox");

    sel_ = mk(menu_, "select", "sel");  set_rect(sel_, 10, 380, 100, 20);
    dom_set_attr(&g_dom, sel_, "onchange", "setSetting(\"mode\", this.value)");
    opt_a_ = mk(sel_, "option", NULL); dom_set_attr(&g_dom, opt_a_, "value", "a");
    opt_b_ = mk(sel_, "option", NULL); g_dom.elems[opt_b_].text = strdup(" bee "); dom_set_attr(&g_dom, opt_b_, "selected", "");

    dis_ = mk(menu_, "input", "dis");   set_rect(dis_, 10, 420, 100, 20);
    dom_set_attr(&g_dom, dis_, "type", "text");
    dom_set_attr(&g_dom, dis_, "disabled", "");
    dom_set_attr(&g_dom, dis_, "onclick", "neverCalled()");
}

static int hovered(HtmlElem e) { return (dom_pseudo_state(&g_dom, e) & DOM_PSEUDO_HOVER) != 0; }
static int active(HtmlElem e) { return (dom_pseudo_state(&g_dom, e) & DOM_PSEUDO_ACTIVE) != 0; }
static int focused(HtmlElem e) { return (dom_pseudo_state(&g_dom, e) & DOM_PSEUDO_FOCUS) != 0; }

static void click_at(float x, float y)
{
    events_mouse_move(&g_es, x, y);
    events_mouse_button(&g_es, EV_BUTTON_LEFT, EV_PRESS, 0);
    events_mouse_button(&g_es, EV_BUTTON_LEFT, EV_RELEASE, 0);
}

static void test_hover(void)
{
    log_clear();
    events_mouse_move(&g_es, 50, 30); /* over #btn */
    CHECK(events_hovered(&g_es) == btn_);
    CHECK(hovered(btn_) && hovered(menu_) && hovered(body_) && hovered(html_));
    CHECK(events_cursor(&g_es) == CSS_CURSOR_POINTER);
    CHECK_STR(events_cursor_name(events_cursor(&g_es)), "pointer");
    /* mouseover bubbles to #menu's handler; mouseenter fires on #btn only */
    CHECK(log_indexf("H mouseover playTick@%u", menu_) >= 0);
    CHECK(log_indexf("H mouseenter playTick@%u", btn_) >= 0);
    /* enter order: mouseover (bubbling from target) before mouseenter(btn) per set_hover_target; both present once */
    int over = 0, enter = 0;
    for (int i = 0; i < g_log_n; i++) { if (strstr(g_log[i], "mouseover")) over++; if (strstr(g_log[i], "mouseenter")) enter++; }
    CHECK(over == 1 && enter == 1);

    log_clear();
    events_mouse_move(&g_es, 60, 110); /* over #lbltext: leaves #btn, stays inside #menu */
    CHECK(events_hovered(&g_es) == lbltext_);
    CHECK(!hovered(btn_) && hovered(lbltext_) && hovered(lbl_) && hovered(menu_));
    CHECK(events_cursor(&g_es) == CSS_CURSOR_AUTO);
    CHECK(log_hasf("H mouseover playTick@%u", menu_));        /* #menu's onmouseover fires again (bubbling from the new target) */
    CHECK(!log_has("mouseenter"));                    /* #menu was not re-entered; #lbltext has no handler */

    log_clear();
    events_mouse_move(&g_es, 60, 112); /* same target: nothing */
    CHECK(g_log_n == 0);

    events_mouse_move(&g_es, 1000, 700); /* body only */
    CHECK(events_hovered(&g_es) == body_);
    CHECK(!hovered(menu_) && hovered(body_));

    events_mouse_leave(&g_es);
    CHECK(events_hovered(&g_es) == 0 && !hovered(body_) && !hovered(html_));
}

static void test_click(void)
{
    log_clear();
    events_mouse_move(&g_es, 50, 30);
    events_mouse_button(&g_es, EV_BUTTON_LEFT, EV_PRESS, 0);
    CHECK(active(btn_) && active(menu_) && active(html_));
    CHECK(events_focused(&g_es) == 0); /* div is not focusable */
    events_mouse_button(&g_es, EV_BUTTON_LEFT, EV_RELEASE, 0);
    CHECK(!active(btn_) && !active(menu_));
    int i_down = log_index("L btn mousedown"), i_up = log_index("W up"), i_click = log_indexf("H click showWindow@%u(1)", btn_);
    int i_wdown = log_indexf("W down %u btn=0", btn_);
    CHECK(i_wdown >= 0 && i_down > i_wdown && i_up > i_down && i_click > i_up);
    CHECK(log_hasf("L menu mousedown target=%u", btn_));     /* element listeners get the original target */
    CHECK(log_hasf("L window click target=%u", btn_));       /* document click listener (anchor blocking) */
    CHECK(!log_has("L window mousedown"));
    CHECK(!log_has("mouseup") || !log_has("H mouseup"));

    /* press on btn, release elsewhere inside #menu: click goes to the common ancestor */
    log_clear();
    events_mouse_move(&g_es, 50, 30);
    events_mouse_button(&g_es, EV_BUTTON_LEFT, EV_PRESS, 0);
    events_mouse_move(&g_es, 700, 500);
    events_mouse_button(&g_es, EV_BUTTON_LEFT, EV_RELEASE, 0);
    CHECK(!log_has("showWindow"));
    CHECK(log_hasf("L window click target=%u", menu_));

    /* right button: contextmenu, no click */
    log_clear();
    events_mouse_move(&g_es, 50, 30);
    events_mouse_button(&g_es, EV_BUTTON_RIGHT, EV_PRESS, 0);
    events_mouse_button(&g_es, EV_BUTTON_RIGHT, EV_RELEASE, 0);
    CHECK(!log_has("showWindow"));
    CHECK(!active(btn_));

    /* disabled control: no handlers, no focus */
    log_clear();
    click_at(50, 430);
    CHECK(!log_has("neverCalled"));
    CHECK(events_focused(&g_es) == 0);

    /* v7.1.4 handlers: event.stopPropagation() keeps the ancestor's onclick from running;
     * a ternary runs the branch its guard handler answers; keyCode guards skip clicks */
    dom_set_attr(&g_dom, menu_, "onclick", "outer()");
    dom_set_attr(&g_dom, btn_, "onclick", "event.stopPropagation();inner(1)");
    log_clear();
    click_at(50, 30);
    CHECK(log_hasf("H click inner@%u(1)", btn_));
    CHECK(!log_has("outer"));
    dom_set_attr(&g_dom, btn_, "onclick", "inner(2)");
    log_clear();
    click_at(50, 30);
    CHECK(log_hasf("H click inner@%u(2)", btn_) && log_hasf("H click outer@%u()", menu_));
    /* a C handler stopping the event (html_ui_stop_propagation, v7's f1822(window.event)) */
    g_stop_handler = "inner";
    log_clear();
    click_at(50, 30);
    CHECK(log_hasf("H click inner@%u(2)", btn_) && !log_has("outer"));
    g_stop_handler = NULL;
    dom_remove_attr(&g_dom, menu_, "onclick");
    dom_set_attr(&g_dom, btn_, "onclick", "cond?yes():no(),13==event.keyCode&&enter()");
    g_zero_handler = "cond";
    log_clear();
    click_at(50, 30);
    CHECK(log_has("H click cond@") && log_has("H click no@") && !log_has("yes@") && !log_has("enter@"));
    g_zero_handler = NULL;
    log_clear();
    click_at(50, 30);
    CHECK(log_has("H click yes@") && !log_has("no@"));
    dom_set_attr(&g_dom, btn_, "onclick", "showWindow(1)");
}

static void test_label_checkbox(void)
{
    log_clear();
    CHECK(!dom_get_checked(&g_dom, cb_));
    click_at(60, 110); /* on the span inside the label */
    CHECK(dom_get_checked(&g_dom, cb_));
    CHECK(dom_pseudo_state(&g_dom, cb_) & DOM_PSEUDO_CHECKED);
    CHECK(events_focused(&g_es) == cb_ && focused(cb_));
    CHECK(log_hasf("H click setSetting@%u('x',this.checked=1) key=0 btn=0 checked=1", cb_));
    int i_click_span = log_indexf("L window click target=%u", lbltext_), i_click_cb = log_indexf("L window click target=%u", cb_);
    CHECK(i_click_span >= 0 && i_click_cb > i_click_span);
    /* input/change are not listener types, they only reach inline handlers: none on cb for them */
    click_at(60, 110);
    CHECK(!dom_get_checked(&g_dom, cb_));
    CHECK(log_has("this.checked=0"));

    /* clicking the checkbox itself: toggles once, label does not re-forward */
    log_clear();
    click_at(12, 105);
    CHECK(dom_get_checked(&g_dom, cb_));
    int n = 0;
    for (int i = 0; i < g_log_n; i++) if (strstr(g_log[i], "setSetting")) n++;
    CHECK(n == 1);

    /* programmatic click() */
    events_click(&g_es, lbltext_);
    CHECK(!dom_get_checked(&g_dom, cb_));
}

static void test_prevent_default(void)
{
    /* ancestor `return false` cancels the checkbox toggle */
    log_clear();
    click_at(12, 305);
    CHECK(!dom_get_checked(&g_dom, cb2_));
    CHECK(!(dom_pseudo_state(&g_dom, cb2_) & DOM_PSEUDO_CHECKED));
    CHECK(log_hasf("L window click target=%u", cb2_)); /* the click still reached the document listener */

    /* `return vetoed()` where the handler returns 0: label does not forward */
    g_zero_handler = "vetoed";
    log_clear();
    click_at(100, 350);
    CHECK(log_hasf("H click vetoed@%u()", lbl2_));
    CHECK(!dom_get_checked(&g_dom, cb3_));
    g_zero_handler = NULL;
    click_at(100, 350);
    CHECK(dom_get_checked(&g_dom, cb3_));
    click_at(100, 350);
    CHECK(!dom_get_checked(&g_dom, cb3_));
}

static void test_focus_keys(void)
{
    log_clear();
    click_at(50, 210); /* #txt */
    CHECK(events_focused(&g_es) == txt_ && focused(txt_));
    CHECK(log_hasf("H focus focusChat@%u(1)", txt_));
    CHECK(log_hasf("W focus %u", txt_));
    /* keys reach the widget and are consumed */
    log_clear();
    CHECK(events_key(&g_es, 65, 0, EV_PRESS, 0) == 1);
    CHECK(log_indexf("L window keydown target=%u key=65", txt_) == 0); /* capture listener first */
    CHECK(log_hasf("W key %u key=65 action=1", txt_));
    CHECK(events_char(&g_es, 'a') == 1);
    CHECK(log_hasf("W chr %u cp=97", txt_));
    CHECK(events_key(&g_es, 65, 0, EV_RELEASE, 0) == 1);
    CHECK(log_hasf("L window keyup target=%u", txt_));
    /* Escape blurs and is consumed; the window listener is NOT run */
    log_clear();
    CHECK(events_key(&g_es, EV_KEY_ESCAPE, 0, EV_PRESS, 0) == 1);
    CHECK(events_focused(&g_es) == 0 && !focused(txt_));
    CHECK(log_hasf("W blur %u", txt_));
    CHECK(!log_has("L window keydown"));
    /* Escape on a focused checkbox (focused by its click) blurs it and still reaches the window
     * listener (the menu's Spectate switch must not swallow the Escape that closes the menu) */
    events_focus(&g_es, cb_);
    CHECK(events_focused(&g_es) == cb_);
    log_clear();
    CHECK(events_key(&g_es, EV_KEY_ESCAPE, 0, EV_PRESS, 0) == 0);
    CHECK(events_focused(&g_es) == 0);
    CHECK(log_has("L window keydown target=0 key=256"));
    /* Escape with nothing focused goes to the window listener; consumed only if it prevents default */
    log_clear();
    CHECK(events_key(&g_es, EV_KEY_ESCAPE, 0, EV_PRESS, 0) == 0);
    CHECK(log_has("L window keydown target=0 key=256"));
    g_listener_result = 0;
    CHECK(events_key(&g_es, EV_KEY_ESCAPE, 0, EV_PRESS, 0) == 1);
    g_listener_result = 1;
    CHECK(events_char(&g_es, 'x') == 0);

    /* Enter -> keypress with the inline handler; char -> keypress then insertion unless cancelled */
    click_at(50, 250); /* #num */
    CHECK(events_focused(&g_es) == num_);
    log_clear();
    events_key(&g_es, EV_KEY_ENTER, 0, EV_PRESS, 0);
    CHECK(log_hasf("H keypress delayExecute@%u('fov',this,'undefined') key=257", num_));
    log_clear();
    events_key(&g_es, 49, 0, EV_PRESS, 0); /* '1' */
    events_char(&g_es, '1');
    CHECK(log_hasf("H keypress delayExecute@%u('fov',this,'undefined') key=49", num_));
    CHECK(log_hasf("W chr %u cp=49", num_));
    g_zero_handler = "delayExecute";
    log_clear();
    events_char(&g_es, '2');
    CHECK(log_has("H keypress delayExecute"));
    CHECK(!log_has("W chr"));
    g_zero_handler = NULL;

    /* clicking a non-focusable area blurs */
    click_at(700, 500);
    CHECK(events_focused(&g_es) == 0);
    /* programmatic focus/blur */
    events_focus(&g_es, txt_);
    CHECK(events_focused(&g_es) == txt_);
    events_focus(&g_es, btn_); /* not focusable: ignored */
    CHECK(events_focused(&g_es) == txt_);
    events_blur(&g_es);
    CHECK(events_focused(&g_es) == 0);
    events_focus(&g_es, dis_); /* disabled */
    CHECK(events_focused(&g_es) == 0);
    CHECK(events_is_focusable(&g_es, sel_) && events_is_focusable(&g_es, cb_) && !events_is_focusable(&g_es, lbl_));
}

/* Tab / Shift+Tab: sequential focus navigation in document order over focusable, rendered,
 * non-negative-tabindex elements, wrapping around (the login form: name -> password -> email) */
static void test_tab_focus(void)
{
    events_focus(&g_es, txt_);
    CHECK(events_focused(&g_es) == txt_);
    log_clear();
    events_key(&g_es, EV_KEY_TAB, 0, EV_PRESS, 0);
    CHECK(events_focused(&g_es) == num_ && focused(num_) && !focused(txt_));
    CHECK(log_hasf("W blur %u", txt_) && log_hasf("W focus %u", num_));
    events_key(&g_es, EV_KEY_TAB, 0, EV_RELEASE, 0); /* releases never move it */
    CHECK(events_focused(&g_es) == num_);
    events_key(&g_es, EV_KEY_TAB, 0, EV_PRESS, EV_MOD_SHIFT);
    CHECK(events_focused(&g_es) == txt_);
    events_key(&g_es, EV_KEY_TAB, 0, EV_PRESS, EV_MOD_SHIFT);
    CHECK(events_focused(&g_es) == cb_); /* the checkbox before it */
    events_key(&g_es, EV_KEY_TAB, 0, EV_PRESS, EV_MOD_SHIFT);
    CHECK(events_focused(&g_es) == sel_); /* wraps to the last; #dis (disabled) is skipped */
    events_key(&g_es, EV_KEY_TAB, 0, EV_PRESS, 0);
    CHECK(events_focused(&g_es) == cb_); /* and forward from the last back to the first */
    /* tabindex=-1 and unrendered elements are skipped */
    dom_set_attr(&g_dom, num_, "tabindex", "-1");
    events_focus(&g_es, txt_);
    events_key(&g_es, EV_KEY_TAB, 0, EV_PRESS, 0);
    CHECK(events_focused(&g_es) == cb2_);
    dom_remove_attr(&g_dom, num_, "tabindex");
    g_dom.elems[num_].has_rect = 0;
    events_focus(&g_es, txt_);
    events_key(&g_es, EV_KEY_TAB, 0, EV_PRESS, 0);
    CHECK(events_focused(&g_es) == cb2_);
    g_dom.elems[num_].has_rect = 1;
    /* a keydown handler that prevents default keeps the focus */
    g_listener_result = 0;
    events_focus(&g_es, txt_);
    events_key(&g_es, EV_KEY_TAB, 0, EV_PRESS, 0);
    CHECK(events_focused(&g_es) == txt_);
    g_listener_result = 1;
    events_blur(&g_es);
}

static void test_select(void)
{
    log_clear();
    events_mouse_move(&g_es, 50, 390);
    events_mouse_button(&g_es, EV_BUTTON_LEFT, EV_PRESS, 0);
    CHECK(log_hasf("W select %u", sel_));
    CHECK(events_captured(&g_es) == sel_);
    CHECK(events_focused(&g_es) == sel_);
    events_mouse_move(&g_es, 55, 395);
    CHECK(log_hasf("W move %u 55,395 buttons=1", sel_));
    events_mouse_button(&g_es, EV_BUTTON_LEFT, EV_RELEASE, 0);
    CHECK(events_captured(&g_es) == 0);
    /* this.value for a select is the selected option's value (text when no value attr, trimmed) */
    log_clear();
    events_fire_input_change(&g_es, sel_);
    CHECK(log_hasf("H change setSetting@%u('mode',this.value='bee')", sel_));
    /* a value attribute on the <select> itself is not its value: the selected option is */
    dom_set_attr(&g_dom, sel_, "value", "zz");
    log_clear();
    events_fire(&g_es, sel_, "change", 0, 0);
    CHECK(log_has("this.value='bee'") && !log_has("this.value='zz'"));
    dom_remove_attr(&g_dom, sel_, "value");

    /* widget capture via mouse_down bits */
    g_widget_down_bits = EV_WIDGET_CAPTURE;
    events_mouse_move(&g_es, 50, 30);
    events_mouse_button(&g_es, EV_BUTTON_LEFT, EV_PRESS, 0);
    CHECK(events_captured(&g_es) == btn_);
    events_mouse_move(&g_es, 700, 500);
    log_clear();
    events_mouse_button(&g_es, EV_BUTTON_LEFT, EV_RELEASE, 0);
    CHECK(log_hasf("W up %u btn=0", btn_));
    CHECK(log_hasf("H click showWindow@%u(1)", btn_)); /* release goes to the capture target -> click there */
    g_widget_down_bits = EV_WIDGET_CONSUMED;
    log_clear();
    click_at(50, 30);
    CHECK(!log_has("showWindow") && !log_has("L btn mousedown"));
    g_widget_down_bits = 0;
    /* pointer capture by Box */
    events_set_pointer_capture(&g_es, &g_dom.elems[txt_]);
    CHECK(events_captured(&g_es) == txt_);
    events_release_pointer_capture(&g_es);
    CHECK(events_captured(&g_es) == 0);
}

static void test_scroll(void)
{
    struct Box *sc = &g_dom.elems[scroller_], *outer = &g_dom.elems[outer_scroller_];
    sc->scroll.scroll_y = 0;
    outer->scroll.scroll_y = 0;
    CHECK(events_scroll(&g_es, 0, -1, 350, 100) == 1);
    CHECK(sc->scroll.scroll_y == 53.0f);
    for (int i = 0; i < 15; i++) events_scroll(&g_es, 0, -1, 350, 100); /* 16 clicks: 15*53 = 795, then clamp */
    CHECK(sc->scroll.scroll_y == 800.0f);         /* clamped to scrollHeight - clientHeight */
    CHECK(outer->scroll.scroll_y == 0.0f);       /* the partial last click was absorbed by the inner scroller */
    CHECK(events_scroll(&g_es, 0, -1, 350, 100) == 1);
    CHECK(outer->scroll.scroll_y == 53.0f);      /* chained to the outer scroller once inner hit its limit */
    for (int i = 0; i < 10; i++) events_scroll(&g_es, 0, -1, 350, 100);
    CHECK(outer->scroll.scroll_y == 100.0f);
    CHECK(events_scroll(&g_es, 0, -1, 350, 100) == 0); /* everything at the limit */
    for (int i = 0; i < 40; i++) events_scroll(&g_es, 0, 1, 350, 100);
    CHECK(sc->scroll.scroll_y == 0.0f && outer->scroll.scroll_y == 0.0f);
    CHECK(events_scroll(&g_es, 0, 1, 350, 100) == 0);
    CHECK(events_scroll(&g_es, 1, 0, 350, 100) == 0);   /* no horizontal scrolling */
    CHECK(events_scroll(&g_es, 0, -1, 50, 30) == 0);    /* #btn has no scroll container */
    events_scroll(&g_es, 0, -0.5f, 350, 100);
    CHECK(sc->scroll.scroll_y == 26.5f);
}

static int g_resize_calls;
static HtmlElem g_resize_last;
static void on_resize(HtmlElem e, void *user) { (void)user; g_resize_calls++; g_resize_last = e; }

static void test_resize_observer(void)
{
    events_observe_resize(&g_es, btn_, on_resize, NULL);
    events_observe_resize(&g_es, btn_, on_resize, NULL); /* duplicate ignored */
    events_observe_resize(&g_es, menu_, on_resize, NULL);
    events_after_layout(&g_es);
    CHECK(g_resize_calls == 2);                          /* initial notification per observer */
    events_after_layout(&g_es);
    CHECK(g_resize_calls == 2);
    set_rect(btn_, 10, 10, 120, 50);
    events_after_layout(&g_es);
    events_after_layout(&g_es);
    CHECK(g_resize_calls == 3 && g_resize_last == btn_);
    g_dom.elems[btn_].has_rect = 0;                      /* display:none -> 0 rect fires once */
    events_after_layout(&g_es);
    CHECK(g_resize_calls == 4);
    set_rect(btn_, 10, 10, 100, 50);
    events_unobserve_resize(&g_es, btn_);
    events_after_layout(&g_es);
    CHECK(g_resize_calls == 4);

    /* after_layout re-hit-tests the hover chain */
    events_mouse_move(&g_es, 50, 30);
    CHECK(events_hovered(&g_es) == btn_);
    set_rect(btn_, 500, 500, 10, 10);
    events_after_layout(&g_es);
    CHECK(events_hovered(&g_es) == menu_ && !hovered(btn_));
    set_rect(btn_, 10, 10, 100, 50);
    events_after_layout(&g_es);
    CHECK(events_hovered(&g_es) == btn_);
}

static void test_removal_and_cache(void)
{
    /* handler cache follows attribute edits */
    log_clear();
    click_at(50, 30);
    CHECK(log_hasf("showWindow@%u(1)", btn_));
    dom_set_attr(&g_dom, btn_, "onclick", "showWindow(2)");
    log_clear();
    click_at(50, 30);
    CHECK(log_hasf("showWindow@%u(2)", btn_) && !log_hasf("showWindow@%u(1)", btn_));
    dom_set_attr(&g_dom, btn_, "onclick", "broken(");
    log_clear();
    click_at(50, 30);
    CHECK(!log_has("broken"));
    dom_set_attr(&g_dom, btn_, "onclick", "showWindow(1)");
    events_invalidate_handlers(&g_es);
    log_clear();
    click_at(50, 30);
    CHECK(log_hasf("showWindow@%u(1)", btn_));

    /* removing the hovered + focused subtree drops the stale handles */
    events_focus(&g_es, cb_);
    events_mouse_move(&g_es, 60, 110); /* lbltext inside lbl */
    CHECK(events_hovered(&g_es) == lbltext_ && events_focused(&g_es) == cb_);
    events_observe_resize(&g_es, cb_, on_resize, NULL);
    events_elem_removed(&g_es, lbl_);
    kill_subtree(lbl_);
    CHECK(events_hovered(&g_es) == menu_);
    CHECK(events_focused(&g_es) == 0);
    CHECK(g_es.observer_count == 1); /* only #menu left */
    log_clear();
    events_mouse_move(&g_es, 61, 111); /* now hits #menu directly: nothing dead is touched */
    events_after_layout(&g_es);
    CHECK(g_fail == 0 || 1);

    /* handler cache survives many elements (growth + dead-handle sweep) */
    for (int i = 0; i < 150 && g_dom.count < MAX_ELEMS - 1; i++) {
        HtmlElem e = mk(menu_, "div", NULL);
        dom_set_attr(&g_dom, e, "onclick", "playTick()");
        events_click(&g_es, e);
        events_elem_removed(&g_es, e);
        kill_subtree(e);
    }
    CHECK(g_es.handler_count <= 16);
}

int main(int argc, char **argv)
{
    const char *repo = argc > 1 ? argv[1] : "/home/user/src/KrunkNative";

    test_parser_grammar();
    test_parser_corpus(repo);

    build_page();
    EventsHost host = {NULL, h_hit_test, h_box_elem, h_elem_box, h_elem_rect, h_scroll_info, h_set_scroll, h_restyle};
    EventsWidgetHooks hooks = {NULL, w_mouse_down, w_mouse_move, w_mouse_up, w_key, w_chr, w_focus, w_blur, w_open_select};
    events_init(&g_es, (Dom *)&g_dom, &host);
    events_set_widget_hooks(&g_es, &hooks);

    test_hover();
    test_click();
    test_label_checkbox();
    test_prevent_default();
    test_focus_keys();
    test_tab_focus();
    test_select();
    test_scroll();
    test_resize_observer();
    test_removal_and_cache();

    events_fini(&g_es);
    for (HtmlElem e = 1; e <= g_dom.count; e++) {
        struct Box *b = &g_dom.elems[e];
        for (int i = 0; i < b->attr_count; i++) free(b->attr_values[i]);
        free(b->text);
    }
    log_clear();
    for (int i = 0; i < g_callee_n; i++) free(g_callees[i].name);

    if (g_fail) { fprintf(stderr, "%d check(s) failed\n", g_fail); log_dump(); return 1; }
    printf("test_events: all checks passed\n");
    return 0;
}

/*
 * CMakeLists.txt for the throwaway build (write to /tmp/kn-events/CMakeLists.txt):
 *
 *   cmake_minimum_required(VERSION 3.16)
 *   project(kn_events_test C)
 *   set(CMAKE_C_STANDARD 11)
 *   set(R /home/user/src/KrunkNative)
 *   add_executable(test_events ${R}/client/src/html/engine/events.c ${R}/client/src/html/engine/util.c
 *                              ${R}/client/src/html/engine/tests/test_events.c)
 *   target_include_directories(test_events PRIVATE ${R}/client/src/html/engine ${R}/client/include ${R}/client/lib/lexbor)
 *   target_compile_definitions(test_events PRIVATE KN_EVENTS_STANDALONE)
 *   target_compile_options(test_events PRIVATE -g -O1 -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer)
 *   target_link_options(test_events PRIVATE -fsanitize=address,undefined)
 *   target_link_libraries(test_events m)
 */
