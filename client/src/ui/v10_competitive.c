// Krunker v10.0.2's competitive popups on the v10 page and the shared v10 popup API (client/include/v10_ui.h):
//
//   * the popup frame (genericPopup({component, props}, cls), v10:215938): the component's markup in
//     #popupHolder > #genericPop with the popup's class, re-rendered in place when its data arrives;
//   * the view stack (_5216 viewStack / _5235(component).push(), mounted in #app, v10:223075): the
//     io-popup dialogs above everything (confirm purchase, error, success, Turf Wars' views);
//   * a request cache over the brain's JSON routes (v10's _33975 request cache);
//   * the shared Svelte components: tabs (svelte-1o3qul6), main-button (svelte-whnh40), the generic
//     table (svelte-69sryq), the currency display (svelte-1r6bdx8), the market card (svelte-r5taai);
//   * the Leaderboards popup (#menuBtnLeaderboards, _49281 in a defaultModal, v10:138716-139420):
//     the category grid (Players / Clans / Raids / Ranked / KrunkCup) and each category's view;
//   * the menu entries: #menuBtnRanked (openRankedMenu, ranked_ui.c), #menuBtnTurfWars (turf_ui.c),
//     #menuBtnLeaderboards (openLeaderboards).
//
// The markup is v10's templates (_4461 strings) with the same classes, so ui/css/svelte.css
// styles it; the events that Svelte binds with __click are inline handlers here. Data comes from
// kn_brain: /api/leaderboards/* (services/brain/leaderboards.c), /mm/ranked/* (ranked.c) and
// /api/territorial-wars/* (turf.c).
#include <v10_ui.h>
#include <backend.h>
#include <backend_ui.h>
#include <html_ui.h>
#include <ranked.h>
#include <ruleset.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../viewer/viewer_store.h"

// ------------------------------------------------------------------ JSON / format helpers

double v10_num(const cJSON *o, const char *key, const double def) {
    const cJSON *v = cJSON_GetObjectItem(o, key);
    if (cJSON_IsNumber(v)) return v->valuedouble;
    if (cJSON_IsString(v) && v->valuestring[0]) {
        char *end;
        const double d = strtod(v->valuestring, &end);
        if (end != v->valuestring) return d;
    }
    if (cJSON_IsBool(v)) return cJSON_IsTrue(v);
    return def;
}

const char *v10_str(const cJSON *o, const char *key) {
    const char *s = cJSON_GetStringValue(cJSON_GetObjectItem(o, key));
    return s ? s : "";
}

void v10_fmt_int(char *out, const size_t n, const double v) {
    char d[48];
    snprintf(d, sizeof(d), "%.0f", v);
    size_t o = 0;
    const size_t len = strlen(d);
    const size_t start = d[0] == '-' ? 1 : 0;
    for (size_t i = 0; i < len && o + 2 < n; i++) {
        if (i > start && (len - i) % 3 == 0) out[o++] = ',';
        out[o++] = d[i];
    }
    out[o] = 0;
}

void v10_url_token(char *out, const size_t n) {
    const char *t = account_session_token();
    backend_url_encode(out, n, t ? t : "");
}

// ------------------------------------------------------------------ request cache

#define REQ_SLOTS 96

typedef struct {
    char *path;
    V10Req req;
    double at_ms;        // answer time (html clock), 0 = pending
    int ttl_ms;
    int inflight;
    unsigned gen;        // forget() generation the request was made in
} ReqSlot;

static ReqSlot g_req[REQ_SLOTS];
static unsigned g_req_gen = 1;
static int g_scenario;                 // --render-scenario: answers come from the scenario only

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec * 1000.0 + (double) ts.tv_nsec / 1e6;
}

static ReqSlot *req_find(const char *path) {
    for (int i = 0; i < REQ_SLOTS; i++)
        if (g_req[i].path && !strcmp(g_req[i].path, path)) return &g_req[i];
    return NULL;
}

static void req_clear(ReqSlot *s) {
    free(s->path);
    cJSON_Delete(s->req.json);
    memset(s, 0, sizeof(*s));
}

static ReqSlot *req_new(const char *path) {
    ReqSlot *s = NULL;
    for (int i = 0; i < REQ_SLOTS && !s; i++)
        if (!g_req[i].path) s = &g_req[i];
    if (!s) { // evict the oldest finished answer
        for (int i = 0; i < REQ_SLOTS; i++) {
            if (g_req[i].inflight) continue;
            if (!s || g_req[i].at_ms < s->at_ms) s = &g_req[i];
        }
        if (!s) return NULL;
        req_clear(s);
    }
    s->path = strdup(path);
    return s->path ? s : NULL;
}

static void on_req(void *user, const int status, const char *body, const size_t length) {
    (void) length;
    char *path = user;
    ReqSlot *s = req_find(path);
    free(path);
    if (!s) return;
    s->inflight = 0;
    if (s->gen != g_req_gen) { // forgotten while in flight: ask again on the next render
        req_clear(s);
        v10_refresh();
        return;
    }
    cJSON_Delete(s->req.json);
    s->req.json = body && status > 0 ? cJSON_Parse(body) : NULL;
    s->req.status = status > 0 ? status : -1;
    s->at_ms = now_ms();
    v10_refresh();
}

// scenario answers: exact path first, then the path without its query
static const V10Req *scenario_answer(const char *path) {
    ReqSlot *s = req_find(path);
    if (s) return &s->req;
    char base[512];
    snprintf(base, sizeof(base), "%s", path);
    char *q = strchr(base, '?');
    if (q) *q = 0;
    s = req_find(base);
    static const V10Req missing = {404, NULL};
    return s ? &s->req : &missing;
}

const V10Req *v10_get(const char *path, const int ttl_ms) {
    if (g_scenario) return scenario_answer(path);
    ReqSlot *s = req_find(path);
    const double now = now_ms();
    if (s && (s->inflight || !s->at_ms || ttl_ms <= 0 || now - s->at_ms < ttl_ms)) return &s->req;
    if (backend_available() != 1) {
        static const V10Req offline = {-1, NULL};
        return s ? &s->req : &offline;
    }
    if (!s) s = req_new(path);
    if (!s) {
        static const V10Req oom = {-1, NULL};
        return &oom;
    }
    s->ttl_ms = ttl_ms;
    s->gen = g_req_gen;
    char *key = strdup(path);
    s->inflight = 1;
    if (!key || !backend_http_get(path, on_req, key)) {
        free(key);
        s->inflight = 0;
        s->req.status = -1;
        s->at_ms = now;
    }
    // a stale answer stays visible while the new one loads
    return &s->req;
}

void v10_forget(const char *prefix) {
    if (g_scenario) return;
    const size_t n = prefix ? strlen(prefix) : 0;
    for (int i = 0; i < REQ_SLOTS; i++) {
        if (!g_req[i].path || strncmp(g_req[i].path, prefix ? prefix : "", n)) continue;
        if (g_req[i].inflight) g_req[i].gen = 0; // its answer is dropped
        else req_clear(&g_req[i]);
    }
    g_req_gen++;
    if (!g_req_gen) g_req_gen = 1;
}

typedef struct {
    V10DoneFn fn;
    void *user;
} SendCtx;

static void on_send(void *user, const int status, const char *body, const size_t length) {
    (void) length;
    SendCtx *c = user;
    cJSON *json = body && status > 0 ? cJSON_Parse(body) : NULL;
    if (c->fn) c->fn(c->user, status > 0 ? status : -1, json);
    cJSON_Delete(json);
    free(c);
}

int v10_send(const char *method, const char *path, const char *json_body, V10DoneFn fn, void *user) {
    if (g_scenario || backend_available() != 1) {
        if (fn) fn(user, -1, NULL);
        return 0;
    }
    SendCtx *c = calloc(1, sizeof(*c));
    if (!c) return 0;
    c->fn = fn;
    c->user = user;
    char url[1024];
    snprintf(url, sizeof(url), "%s%s", backend_url(), path);
    if (!backend_http_request(method, url, json_body, on_send, c)) {
        free(c);
        if (fn) fn(user, -1, NULL);
        return 0;
    }
    return 1;
}

// ------------------------------------------------------------------ genericPopup

static struct {
    char cls[48];
    V10RenderFn render;
} g_pop;

static HtmlElem generic_pop(void) { return html_dom_by_id("genericPop"); }

int v10_popup_open(const char *cls) {
    const HtmlElem pop = generic_pop();
    if (!pop || !g_pop.cls[0] || strcmp(g_pop.cls, cls)) return 0;
    char have[64] = "";
    html_dom_get_attr(pop, "class", have, sizeof(have));
    if (strcmp(have, cls)) return 0; // another popup took #genericPop (resetPops)
    char disp[16] = "";
    if (html_dom_get_style(pop, "display", disp, sizeof(disp)) >= 0 && !strcmp(disp, "none")) return 0;
    const HtmlElem holder = html_dom_by_id("popupHolder");
    if (holder && html_dom_get_style(holder, "display", disp, sizeof(disp)) >= 0 && !strcmp(disp, "none")) return 0;
    return 1;
}

void v10_popup_show(const char *cls, const char *html, V10RenderFn render) {
    ui_loadout_reset_pops(); // resetPops(): #popupHolder shown, every other popup hidden
    const HtmlElem pop = generic_pop();
    if (!pop) return;
    snprintf(g_pop.cls, sizeof(g_pop.cls), "%s", cls);
    g_pop.render = render;
    html_dom_set_attr(pop, "class", cls);
    html_dom_set_attr(pop, "style", "");
    html_dom_set_html(pop, html ? html : "");
    html_dom_set_display(pop, "block");
    menu_schedule_rect();
}

// innerHTML keeping the scroll offsets: v10 updates its components in place, so the lists the
// player scrolled stay where they were when an answer re-renders the popup
#define SCROLL_MAX 4096
static void set_html_keep_scroll(const HtmlElem root, const char *html) {
    static HtmlElem elems[SCROLL_MAX];
    static struct { int index; float left, top; } saved[64];
    int n_saved = 0;
    int n = html_dom_query_all(root, "*", elems, SCROLL_MAX);
    for (int i = 0; i < n && n_saved < 64; i++) {
        HtmlScroll sc;
        if (html_dom_get_scroll(elems[i], &sc) && (sc.top > 0.0f || sc.left > 0.0f)) {
            saved[n_saved].index = i;
            saved[n_saved].left = sc.left;
            saved[n_saved].top = sc.top;
            n_saved++;
        }
    }
    html_dom_set_html(root, html ? html : "");
    if (!n_saved) return;
    n = html_dom_query_all(root, "*", elems, SCROLL_MAX);
    for (int i = 0; i < n_saved; i++)
        if (saved[i].index < n) html_dom_set_scroll(elems[saved[i].index], saved[i].left, saved[i].top);
}

void v10_popup_set(const char *cls, const char *html) {
    if (!v10_popup_open(cls)) return;
    set_html_keep_scroll(generic_pop(), html);
}

void v10_popup_close(void) {
    g_pop.cls[0] = 0;
    g_pop.render = NULL;
    ui_loadout_clear_pops();
}

// ------------------------------------------------------------------ view stack

#define VIEW_MAX 6

typedef struct {
    char *html;
    V10RenderFn render;
    V10ViewClose on_close;
    void *user;
} View;

static View g_views[VIEW_MAX];
static int g_view_count;

static void views_render(void) {
    const HtmlElem app = html_dom_by_id("app");
    if (!app) return;
    if (!g_view_count) {
        html_dom_set_html(app, "<div class=\"view-stack svelte-sdh4ve\"><!----></div>");
        return;
    }
    StrBuf sb = {0};
    sb_append(&sb, "<div class=\"view-stack svelte-sdh4ve\">");
    for (int i = 0; i < g_view_count; i++) {
        // every view but the top one is display:none (none of these is translucent)
        sb_append(&sb, "<div class=\"view-stack-overlay svelte-sdh4ve\" onclick=\"knViewBackdrop()\">"
                       "<div class=\"view-wrapper svelte-sdh4ve\" style=\"display: ");
        sb_append(&sb, i == g_view_count - 1 ? "block" : "none");
        sb_append(&sb, "\">");
        sb_append(&sb, g_views[i].html ? g_views[i].html : "");
        sb_append(&sb, "</div></div>");
    }
    sb_append(&sb, "</div>");
    char *html = sb_finish(&sb);
    set_html_keep_scroll(app, html);
    free(html);
}

void v10_view_push(char *html, V10RenderFn render, V10ViewClose on_close, void *user) {
    if (g_view_count == VIEW_MAX) v10_view_pop(0);
    View *v = &g_views[g_view_count++];
    v->html = html;
    v->render = render;
    v->on_close = on_close;
    v->user = user;
    views_render();
}

void v10_view_set(char *html) {
    if (!g_view_count) {
        free(html);
        return;
    }
    free(g_views[g_view_count - 1].html);
    g_views[g_view_count - 1].html = html;
    views_render();
}

void v10_view_pop(const int accepted) {
    if (!g_view_count) return;
    View v = g_views[--g_view_count];
    memset(&g_views[g_view_count], 0, sizeof(View));
    free(v.html);
    views_render();
    if (v.on_close) v.on_close(accepted, v.user);
}

void v10_view_clear(void) {
    while (g_view_count) v10_view_pop(0);
}

int v10_view_depth(void) { return g_view_count; }

void v10_refresh(void) {
    if (g_pop.cls[0] && g_pop.render) {
        if (v10_popup_open(g_pop.cls)) g_pop.render();
        else {
            g_pop.cls[0] = 0;
            g_pop.render = NULL;
        }
    }
    if (g_view_count && g_views[g_view_count - 1].render) g_views[g_view_count - 1].render();
}

// ------------------------------------------------------------------ components

void v10_tabs(StrBuf *sb, const char *const *labels, const int count, const int active, const char *handler) {
    sb_append(sb, "<div class=\"tabs-container svelte-1o3qul6\" role=\"tablist\" style=\"margin-bottom: 0px;\">");
    for (int i = 0; i < count; i++) {
        sb_appendf(sb, "<div class=\"tab svelte-1o3qul6%s\" role=\"tab\" aria-selected=\"%s\" tabindex=\"0\" onclick=\"playSelect();%s(%d)\""
                       " onmouseenter=\"playTick(0.1)\">",
                   i == active ? " tab-active" : "", i == active ? "true" : "false", handler, i);
        sb_append_esc(sb, labels[i]);
        sb_append(sb, "</div>");
    }
    sb_append(sb, "</div>");
}

void v10_back_button(StrBuf *sb, const char *onclick) {
    sb_appendf(sb, "<button class=\"main-button svelte-whnh40\" type=\"button\" onclick=\"%s\">\xe2\x86\x90 Back</button>", onclick);
}

// The colour emoji of v10's texts. The native engine has no colour emoji font, so a text emoji drew as
// a missing-glyph box; these are drawn as the Noto Color Emoji artwork Chrome uses (ui/img/kn/emoji/,
// ui/README.md). Any other text is escaped as it is. cls (may be "") is added to the image's class.
void v10_emoji(StrBuf *sb, const char *utf8, const char *cls) {
    static const struct { const char *utf8, *file; } k_emoji[] = {
        {"\xf0\x9f\x94\x92", "1f512"},             // 🔒 Level / Login Required
        {"\xf0\x9f\x94\x8d", "1f50d"},             // 🔍 Searching
        {"\xe2\x9c\xa8", "2728"},                  // ✨ Match found
        {"\xe2\x9d\x8c", "274c"},                  // ❌ Internal Error
        {"\xe2\x9a\x94\xef\xb8\x8f", "2694-fe0f"}, // ⚔️ Ranked, Turf Wars tips
        {"\xe2\x9a\xa0\xef\xb8\x8f", "26a0-fe0f"}, // ⚠️ Turf Wars note
        {"\xf0\x9f\x8e\xaf", "1f3af"},             // 🎯 Turf Wars: Earn Kills
        {"\xf0\x9f\x92\xb0", "1f4b0"},             // 💰 Turf Wars: Earn Rewards, Top Grossing Clans
        {"\xf0\x9f\x91\xa5", "1f465"},             // 👥 Turf Wars tips
        {"\xf0\x9f\x93\x8a", "1f4ca"},             // 📊 Turf Wars tips
        {"\xf0\x9f\x8e\xae", "1f3ae"},             // 🎮 Turf Wars tips
        {"\xf0\x9f\xa5\x87", "1f947"},             // 🥇 Turf Wars medal
        {"\xf0\x9f\xa5\x88", "1f948"},             // 🥈 Turf Wars medal
        {"\xf0\x9f\xa5\x89", "1f949"},             // 🥉 Turf Wars medal
        {"\xf0\x9f\x8f\x86", "1f3c6"},             // 🏆 KrunkCup card, Turf Wars trophies
        {"\xe2\xad\x90", "2b50"},                  // ⭐ Turf Wars: Top Performing Players
    };
    for (size_t i = 0; utf8 && i < sizeof(k_emoji) / sizeof(k_emoji[0]); i++)
        if (!strcmp(utf8, k_emoji[i].utf8)) {
            sb_appendf(sb, "<img class=\"kn-emoji%s%s\" src=\"./img/kn/emoji/%s.png\" alt=\"\">", cls && *cls ? " " : "", cls ? cls : "", k_emoji[i].file);
            return;
        }
    sb_append_esc(sb, utf8 ? utf8 : "");
}

// v10's currency table (_6909): KR text, RP material emoji_events, JNK material plumbing
void v10_currency(StrBuf *sb, const double value, const char *currency, const char *size, const int centered) {
    const char *color = "#2196f3", *icon = "\xf0\x9f\x92\x8e", *kind = "emoji";
    int decimals = 0;
    if (currency && !strcmp(currency, "KR")) color = "#fbc02d", icon = "KR", kind = "text";
    else if (currency && !strcmp(currency, "RP")) color = "#4caf50", icon = "emoji_events", kind = "material";
    else if (currency && !strcmp(currency, "JNK")) color = "#cc51f8", icon = "plumbing", kind = "material", decimals = 2;
    const char *text = "16px", *isz = "18px";
    if (size && !strcmp(size, "small")) text = "14px", isz = "16px";
    else if (size && !strcmp(size, "large")) text = "20px", isz = "22px";
    char amount[48];
    if (decimals) snprintf(amount, sizeof(amount), "%.2f", value / 100.0);
    else v10_fmt_int(amount, sizeof(amount), value);
    sb_appendf(sb, "<div class=\"currency-display svelte-1r6bdx8%s\" style=\"--text-size: %s; --icon-size: %s;\">"
                   "<span class=\"currency-amount svelte-1r6bdx8\" style=\"color: %s;\">%s</span> ",
               centered ? " centered" : "", text, isz, color, amount);
    if (!strcmp(kind, "material")) sb_appendf(sb, "<span class=\"material-icons currency-icon svelte-1r6bdx8\" style=\"color: %s;\">%s</span>", color, icon);
    else sb_appendf(sb, "<span class=\"currency-text svelte-1r6bdx8\" style=\"color: %s;\">%s</span>", color, icon);
    sb_append(sb, "</div>");
}

void v10_table_begin(StrBuf *sb, const V10Col *cols, const int n, const int sort_col, const int sort_desc, const char *sort_fmt) {
    sb_append(sb, "<div class=\"table-wrapper svelte-69sryq\"><div class=\"table-container svelte-69sryq\"><table class=\"data-table svelte-69sryq\">"
                  "<thead class=\"svelte-69sryq\"><tr class=\"svelte-69sryq\">");
    for (int i = 0; i < n; i++) {
        const V10Col *c = &cols[i];
        sb_appendf(sb, "<th class=\"table-header svelte-69sryq%s%s%s\" tabindex=\"%d\" style=\"", c->sortable ? " sortable" : "",
                   sort_col == i ? (sort_desc ? " sort-desc" : " sort-asc") : "", c->right ? " align-right" : "", c->sortable ? 0 : -1);
        if (c->width) sb_appendf(sb, "width: %s; ", c->width);
        sb_appendf(sb, "text-align: %s;\"", c->right ? "right" : "left");
        if (c->sortable && sort_fmt) {
            char click[96];
            snprintf(click, sizeof(click), sort_fmt, i);
            sb_appendf(sb, " onclick=\"playSelect(0.1);%s\"", click);
        }
        sb_append(sb, ">");
        sb_append_esc(sb, c->label);
        sb_append(sb, "</th>");
    }
    sb_append(sb, "</tr></thead><tbody class=\"svelte-69sryq\">");
}

void v10_table_row(StrBuf *sb, const char *onclick, const int highlight) {
    sb_appendf(sb, "<tr class=\"svelte-69sryq%s%s\" tabindex=\"%d\"", highlight ? " highlight-row" : "", onclick ? " clickable" : "", onclick ? 0 : -1);
    if (onclick) sb_appendf(sb, " onclick=\"%s\"", onclick);
    sb_append(sb, ">");
}

void v10_table_cell_html(StrBuf *sb, const V10Col *col, const char *html) {
    sb_appendf(sb, "<td class=\"table-cell svelte-69sryq\" style=\"text-align: %s;\">", col && col->right ? "right" : "left");
    sb_append(sb, html ? html : "");
    sb_append(sb, "</td>");
}

void v10_table_cell(StrBuf *sb, const V10Col *col, const char *text) {
    sb_appendf(sb, "<td class=\"table-cell svelte-69sryq\" style=\"text-align: %s;\">", col && col->right ? "right" : "left");
    sb_append_esc(sb, text ? text : "");
    sb_append(sb, "</td>");
}

void v10_table_row_end(StrBuf *sb) { sb_append(sb, "</tr>"); }

void v10_table_end(StrBuf *sb) { sb_append(sb, "</tbody></table> <!----></div></div>"); }

// _37770 error popup (popup-container 450 px, "Dismiss" closes it)
char *v10_io_error(const char *title, const char *message) {
    StrBuf sb = {0};
    sb_append(&sb, "<div class=\"popup-container svelte-1wz7ymp\"><div class=\"popup svelte-1wz7ymp\" style=\"width: 450px;\">"
                   "<div class=\"error-popup-container io-popup-container\"><div class=\"error-popup-header io-popup-header io-popup-header--slim\">"
                   "<svg xmlns=\"http://www.w3.org/2000/svg\" class=\"error-icon svelte-1t9wm2m\" viewBox=\"0 0 24 24\" fill=\"currentColor\">"
                   "<path d=\"M12 2C6.48 2 2 6.48 2 12s4.48 10 10 10 10-4.48 10-10S17.52 2 12 2zm1 15h-2v-2h2v2zm0-4h-2V7h2v6z\" class=\"svelte-1t9wm2m\"></path></svg> ");
    sb_append_esc(&sb, title ? title : "Error");
    sb_append(&sb, "</div> <div class=\"error-popup-content io-popup-content svelte-1t9wm2m\"><p class=\"text-error svelte-1t9wm2m\">");
    sb_append_esc(&sb, message ? message : "An unexpected error occurred.");
    sb_append(&sb, "</p> <div class=\"io-button-group\"><button type=\"button\" class=\"io-button\" onclick=\"knViewPop(0)\">Dismiss</button></div></div></div></div></div>");
    return sb_finish(&sb);
}

// _37790 success popup: message_html is a component's markup (or escaped text)
char *v10_io_success(const char *title, const char *message_html, const char *button, const char *button_mods) {
    StrBuf sb = {0};
    sb_append(&sb, "<div class=\"popup-container svelte-1wz7ymp\"><div class=\"popup svelte-1wz7ymp\" style=\"width: 450px;\">"
                   "<div class=\"success-popup-container io-popup-container\"><div class=\"success-popup-header io-popup-header io-popup-header--slim\">"
                   "<svg xmlns=\"http://www.w3.org/2000/svg\" class=\"success-icon svelte-12d6f79\" viewBox=\"0 0 24 24\" fill=\"currentColor\">"
                   "<path d=\"M9 16.17L4.83 12l-1.42 1.41L9 19 21 7l-1.41-1.41z\" class=\"svelte-12d6f79\"></path></svg> ");
    sb_append_esc(&sb, title ? title : "Success");
    sb_append(&sb, "</div> <div class=\"success-popup-content io-popup-content svelte-12d6f79\">");
    sb_append(&sb, message_html ? message_html : "");
    sb_append(&sb, " <div class=\"io-button-group\"><button type=\"button\" class=\"io-button");
    for (const char *m = button_mods; m && *m;) { // "accept slim" -> io-button--accept io-button--slim
        const char *sp = strchr(m, ' ');
        const size_t len = sp ? (size_t) (sp - m) : strlen(m);
        sb_append(&sb, " io-button--");
        sb_append_n(&sb, m, len);
        m = sp ? sp + 1 : NULL;
    }
    sb_append(&sb, " svelte-12d6f79\" onclick=\"knViewPop(1)\">");
    sb_append_esc(&sb, button ? button : "OK");
    sb_append(&sb, "</button></div></div></div></div></div>");
    return sb_finish(&sb);
}

// i18n 'social.market.*' of _9448 (item types 1..16); weapon skins say the weapon's name
static const char *item_type_label(const ViewerSkin *skin) {
    static const char *const types[] = {NULL, "Hat Item", "Body Item", "Melee Item", "Spray Item", "Dye Item", "Waist Item", "Face Item",
                                        "Shoe Item", "Pet Item", "Collectible Item", "Wrist Item", "Charm Item", "Spin Ticket Item",
                                        "Back Item", "Head Item", "Player Card"};
    static const char *const keys[] = {NULL, "social.market.hat", "social.market.body", "social.market.melee", "social.market.spray",
                                       "social.market.dye", "social.market.waist", "social.market.face", "social.market.shoe",
                                       "social.market.pet", "social.market.collectible", "social.market.wrist", "social.market.charm",
                                       "social.market.ticket", "social.market.back", "social.market.head", "social.market.playerCard"};
    if (skin->type > 0 && skin->type < (int) (sizeof(types) / sizeof(types[0]))) return i18n_tr(keys[skin->type], types[skin->type]);
    const Weapon *const *w = rules_weapons();
    const int n = rules_krunker_weapon_count();
    if (w && skin->weapon >= 1 && skin->weapon <= n && w[skin->weapon - 1]) return w[skin->weapon - 1]->name;
    return "";
}

void v10_market_card(StrBuf *sb, const int skin_index, const int w, const int h, const char *position, const char *onclick, const char *children) {
    const ViewerSkin *skin = viewer_skin(skin_index);
    const char *color = "#B0B0B0";
    if (skin && skin->rarity >= 0 && skin->rarity < g_viewer_rarity_count && g_viewer_rarities[skin->rarity].color)
        color = g_viewer_rarities[skin->rarity].color;
    char img[512];
    store_skin_preview(skin_index, img, sizeof(img));
    const int tiny = w <= 80 || h <= 80;
    sb_appendf(sb, "<div class=\"market-card svelte-r5taai%s\" role=\"button\" tabindex=\"0\" style=\"border: 2px solid %s; width: %dpx; height: %dpx;\"",
               tiny ? " tiny" : "", color, w, h);
    if (onclick) sb_appendf(sb, " onclick=\"%s\"", onclick);
    sb_append(sb, ">");
    if (skin && skin->type == 4) sb_appendf(sb, "<div class=\"itemImg sprayMarketImg svelte-r5taai\" style=\"background-image: url(%s);\"></div>", img);
    else {
        sb_appendf(sb, "<img draggable=\"false\" loading=\"lazy\" class=\"itemImg marketImg svelte-r5taai%s\" src=\"%s\" alt=\"",
                   skin && (skin->flags & VS_RGB) ? " rgbHue" : "", img);
        sb_append_esc(sb, skin ? skin->name : "");
        sb_append(sb, "\">");
    }
    sb_appendf(sb, " <!----> <div class=\"itemInfo svelte-r5taai%s\"><!----> <div class=\"itemName svelte-r5taai\" style=\"color: %s;\">",
               position && !strcmp(position, "top-center") ? " top-center" : "", color);
    sb_append_esc(sb, skin && skin->name ? skin->name : "Unnamed Item");
    sb_append(sb, "</div> <!----> <div class=\"itemType svelte-r5taai\">");
    sb_append_esc(sb, skin ? item_type_label(skin) : "");
    sb_append(sb, "</div> <!----></div> <!----> <!---->");
    if (children) sb_append(sb, children);
    sb_append(sb, "</div>");
}

// ------------------------------------------------------------------ Leaderboards popup (_49281)

enum { LB_GRID = -1, LB_PLAYERS, LB_CLANS, LB_RAIDS, LB_RANKED, LB_KRUNKCUP, LB_CATEGORIES };

static const struct {
    const char *label, *type;
} k_player_tabs[] = {{"Level", "player_score"},      {"Kills", "player_kills"},       {"Wins", "player_wins"},
                     {"Time", "player_timeplayed"},  {"KR", "player_funds"},          {"Followers", "player_followed"},
                     {"Nukes", "player_nukes"},      {"Inv Value", "player_skinvalue"}, {"Events", "player_eventcount"}};
#define PLAYER_TABS ((int) (sizeof(k_player_tabs) / sizeof(k_player_tabs[0])))
static const struct {
    const char *label, *sort;
} k_clan_tabs[] = {{"Score", "score"}, {"Members", "members"}};
static const struct {
    const char *label;
    int map;
} k_raid_tabs[] = {{"Tortuga", 18}, {"Laboratory", 29}, {"Soul Sanctum", 31}, {"Khepri", 35}, {"Bastion", 43}};
#define RAID_TABS ((int) (sizeof(k_raid_tabs) / sizeof(k_raid_tabs[0])))
static const struct {
    const char *label;
    int region; // regionIndex, 0 = Global
} k_ranked_regions[] = {{"Global", 0}, {"Europe", 3}, {"N. America", 4}, {"Asia", 2}};
static const char *const k_months[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

static struct {
    int cat;
    int player_tab, clan_tab, raid_tab, ranked_tab;
    int ranked_region;    // k_ranked_regions index
    int ranked_season;    // -1 = current
    int matches_season;   // 0 = All Time
    int raid_completions; // sort: 0 best_time, 1 completions
    char raid_month[16];  // "all-time" or "YYYY-MM"
    int kc_team;          // KrunkCup team (jersey style) id, -1 = standings
    int kc_sort;          // KrunkCup table sort column, -1 = v10's defaultSort
    int kc_sort_desc;
} LB = {.cat = LB_GRID, .ranked_season = -1, .raid_month = "all-time", .kc_team = -1, .kc_sort = -1, .kc_sort_desc = 1};

static int season_now(void) { return ranked_season_at((int64_t) time(NULL)); }

// raid months: 0 before 2026-03, else months since 2026-03 inclusive (_7329)
static int raid_months_now(void) {
    const time_t t = time(NULL);
    struct tm tm;
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    const int y = tm.tm_year + 1900, m = tm.tm_mon + 1;
    return y < 2026 || (y == 2026 && m < 3) ? 0 : 12 * (y - 2026) + (m - 3 + 1);
}

static void lb_render(void);

static void lb_loading_or(StrBuf *sb, const V10Req *r, const char *hash, const char *loading, const char *fail) {
    if (r->status == 0) sb_appendf(sb, "<div class=\"loading svelte-%s\">%s</div>", hash, loading);
    else {
        sb_appendf(sb, "<div class=\"error svelte-%s\">", hash);
        const char *msg = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(r->json, "error"), "message"));
        if (!msg) msg = cJSON_GetStringValue(cJSON_GetObjectItem(r->json, "error"));
        sb_append_esc(sb, msg && msg[0] ? msg : fail);
        sb_append(sb, "</div>");
    }
}

static const cJSON *data_array(const V10Req *r) {
    if (r->status != 200) return NULL;
    const cJSON *d = cJSON_GetObjectItem(r->json, "data");
    return cJSON_IsArray(d) ? d : NULL;
}

static void player_label(char *out, const size_t n, const cJSON *row) {
    const char *name = v10_str(row, "player_name"), *clan = v10_str(row, "player_clan");
    if (clan[0]) snprintf(out, n, "%s [%s]", name, clan);
    else snprintf(out, n, "%s", name);
}

// _49157 value formats
static void player_value(char *out, const size_t n, const char *type, const double v) {
    char num[48];
    v10_fmt_int(num, sizeof(num), v);
    if (!strcmp(type, "player_score")) {
        int lvl = (int) (0.03 * sqrt(v > 0 ? v : 0));
        snprintf(out, n, "Lvl %d", lvl < 1 ? 1 : lvl);
    } else if (!strcmp(type, "player_kills")) snprintf(out, n, "%s kills", num);
    else if (!strcmp(type, "player_wins")) snprintf(out, n, "%s wins", num);
    else if (!strcmp(type, "player_timeplayed")) {
        long long s = (long long) (1.2 * (v > 0 ? v : 0) / 1000.0);
        long long m = s / 60, h = m / 60;
        s %= 60;
        m %= 60;
        const long long d = h / 24;
        h %= 24;
        char dd[24] = "", hh[24] = "";
        if (d) snprintf(dd, sizeof(dd), "%lldd ", d);
        if (h) snprintf(hh, sizeof(hh), "%lldh ", h);
        snprintf(out, n, "%s%s%lldm %llds", dd, hh, m, s);
    } else if (!strcmp(type, "player_funds")) snprintf(out, n, "%s KR", num);
    else if (!strcmp(type, "player_followed")) snprintf(out, n, "%s followers", num);
    else if (!strcmp(type, "player_nukes")) snprintf(out, n, "%s nukes", num);
    else if (!strcmp(type, "player_skinvalue")) snprintf(out, n, "~%s KR", num);
    else if (!strcmp(type, "player_eventcount")) snprintf(out, n, "%s found", num);
    else if (!strcmp(type, "player_ranked_matches")) snprintf(out, n, "%s matches", num);
    else snprintf(out, n, "%s", num);
}

static const char *player_value_label(const char *type) {
    for (int i = 0; i < PLAYER_TABS; i++)
        if (!strcmp(type, k_player_tabs[i].type)) return k_player_tabs[i].label[0] == 'T' ? "Time Played" : k_player_tabs[i].label;
    return !strcmp(type, "player_ranked_matches") ? "Matches" : "Value";
}

// _49157: the players / clans leaderboard component (svelte-12wkeob)
static void lb_component(StrBuf *sb, const char *type, const char *sort) {
    char path[160];
    if (sort) snprintf(path, sizeof(path), "/api/leaderboards/%s?sort=%s", type, sort);
    else snprintf(path, sizeof(path), "/api/leaderboards/%s", type);
    const V10Req *r = v10_get(path, 30000);
    sb_append(sb, "<div class=\"leaderboard-component svelte-12wkeob\">");
    const cJSON *rows = data_array(r);
    if (!rows) lb_loading_or(sb, r, "12wkeob", "Loading leaderboard...", "Failed to load leaderboard");
    else if (!cJSON_GetArraySize(rows)) sb_append(sb, "<div class=\"no-data svelte-12wkeob\">No data available.</div>");
    else if (!strcmp(type, "player_clan")) {
        static const V10Col cols[] = {{"#", "50px", 1, 0}, {"Clan", NULL, 0, 0}, {"Score", NULL, 1, 0}, {"Members", NULL, 1, 0}};
        v10_table_begin(sb, cols, 4, -1, 1, NULL);
        const cJSON *row;
        cJSON_ArrayForEach(row, rows) {
            char pos[24], score[32], members[32];
            snprintf(pos, sizeof(pos), "#%.0f", v10_num(row, "position", 0));
            v10_fmt_int(score, sizeof(score), v10_num(row, "clan_score", 0));
            v10_fmt_int(members, sizeof(members), v10_num(row, "member_count", 0));
            v10_table_row(sb, NULL, 0);
            v10_table_cell(sb, &cols[0], pos);
            v10_table_cell(sb, &cols[1], v10_str(row, "clan_name"));
            v10_table_cell(sb, &cols[2], score);
            v10_table_cell(sb, &cols[3], members);
            v10_table_row_end(sb);
        }
        v10_table_end(sb);
    } else {
        const V10Col cols[] = {{"#", "50px", 1, 0}, {"Player", NULL, 0, 0}, {player_value_label(type), NULL, 1, 0}};
        v10_table_begin(sb, cols, 3, -1, 1, NULL);
        const cJSON *row;
        cJSON_ArrayForEach(row, rows) {
            char pos[24], name[96], value[64];
            snprintf(pos, sizeof(pos), "#%.0f", v10_num(row, "position", 0));
            player_label(name, sizeof(name), row);
            player_value(value, sizeof(value), type, v10_num(row, "value", 0));
            v10_table_row(sb, NULL, 0);
            v10_table_cell(sb, &cols[0], pos);
            v10_table_cell(sb, &cols[1], name);
            v10_table_cell(sb, &cols[2], value);
            v10_table_row_end(sb);
        }
        v10_table_end(sb);
    }
    sb_append(sb, "</div>");
}

static void raid_time(char *out, const size_t n, const double ms) {
    if (ms <= 0) snprintf(out, n, "--:--");
    else snprintf(out, n, "%d:%02d", (int) (ms / 60000.0), (int) ((long long) ms % 60000 / 1000));
}

// _49216 raids component (svelte-9t0md3)
static void lb_raids(StrBuf *sb) {
    const int map = k_raid_tabs[LB.raid_tab].map, monthly = strcmp(LB.raid_month, "all-time") != 0;
    char path[160];
    if (!monthly) snprintf(path, sizeof(path), "/api/leaderboards/raids/%d%s", map, LB.raid_completions ? "?sort=completions" : "");
    else snprintf(path, sizeof(path), "/api/leaderboards/raids/%d/monthly?month=%s%s", map, LB.raid_month, LB.raid_completions ? "" : "&sort=best_time");
    const V10Req *r = v10_get(path, 30000);
    sb_appendf(sb, "<div class=\"leaderboard-component svelte-9t0md3\"><div class=\"controls-row svelte-9t0md3\"><div class=\"sort-toggle svelte-9t0md3\">"
                   "<button type=\"button\" class=\"sort-btn svelte-9t0md3%s\" onclick=\"knLbRaidSort(0)\">Best Time</button> "
                   "<button type=\"button\" class=\"sort-btn svelte-9t0md3%s\" onclick=\"knLbRaidSort(1)\">Most Clears</button></div> ",
               LB.raid_completions ? "" : " active", LB.raid_completions ? " active" : "");
    const int months = raid_months_now();
    if (months > 0) {
        sb_append(sb, "<select class=\"month-select svelte-9t0md3\" onchange=\"knLbRaidMonth(this.value)\">");
        sb_appendf(sb, "<option class=\"svelte-9t0md3\" value=\"all-time\"%s>All Time</option>", monthly ? "" : " selected");
        for (int i = months; i >= 1; i--) {
            const int idx = i - 1 + 2, y = 2026 + idx / 12, m = idx % 12;
            char v[16];
            snprintf(v, sizeof(v), "%d-%02d", y, m + 1);
            sb_appendf(sb, "<option class=\"svelte-9t0md3\" value=\"%s\"%s>%s %d</option>", v, !strcmp(v, LB.raid_month) ? " selected" : "", k_months[m], y);
        }
        sb_append(sb, "</select>");
    } else sb_append(sb, "<!---->");
    sb_append(sb, "</div> ");
    const cJSON *rows = data_array(r);
    if (!rows) lb_loading_or(sb, r, "9t0md3", "Loading leaderboard...", "Failed to load raid leaderboard");
    else if (!cJSON_GetArraySize(rows)) sb_append(sb, "<div class=\"no-data svelte-9t0md3\">No raid data available.</div>");
    else {
        const int best = !LB.raid_completions;
        const V10Col all[] = {{"#", "50px", 1, 0}, {"Player", NULL, 0, 0}, {"Best Time", NULL, 1, 0}, {"Clears", NULL, 1, 0}};
        const V10Col mon[] = {{"#", "50px", 1, 0}, {"Player", NULL, 0, 0}, {best ? "Best Time" : "Completions", NULL, 1, 0}};
        const V10Col *cols = monthly ? mon : all;
        v10_table_begin(sb, cols, monthly ? 3 : 4, -1, 1, NULL);
        const cJSON *row;
        cJSON_ArrayForEach(row, rows) {
            char pos[24], name[96], a[32], b[32];
            snprintf(pos, sizeof(pos), "#%.0f", v10_num(row, "position", 0));
            player_label(name, sizeof(name), row);
            v10_table_row(sb, NULL, 0);
            v10_table_cell(sb, &cols[0], pos);
            v10_table_cell(sb, &cols[1], name);
            if (monthly) {
                if (best) raid_time(a, sizeof(a), v10_num(row, "best_time", 0));
                else v10_fmt_int(a, sizeof(a), v10_num(row, "completions", 0));
                v10_table_cell(sb, &cols[2], a);
            } else {
                raid_time(a, sizeof(a), v10_num(row, "best_time", 0));
                v10_fmt_int(b, sizeof(b), v10_num(row, "completions", 0));
                v10_table_cell(sb, &cols[2], a);
                v10_table_cell(sb, &cols[3], b);
            }
            v10_table_row_end(sb);
        }
        v10_table_end(sb);
    }
    sb_append(sb, "</div>");
}

static void season_label(char *out, const size_t n, const int s) {
    const int idx = s - 1 + 7; // season 1 = Aug 2025
    snprintf(out, n, "S%d - %s %d", s, k_months[idx % 12], RANKED_SEASON_START_YEAR + idx / 12);
}

// _49226 Rating tab (svelte-1gdnqq5)
static void lb_ranked_rating(StrBuf *sb) {
    const int now = season_now(), season = LB.ranked_season >= 0 ? LB.ranked_season : now;
    const int region = k_ranked_regions[LB.ranked_region].region;
    char path[160];
    if (region) snprintf(path, sizeof(path), "/mm/ranked/leaderboard?season=%d&region=%d&limit=40&offset=0", season, region);
    else snprintf(path, sizeof(path), "/mm/ranked/leaderboard?season=%d&region=global&limit=40&offset=0", season);
    const V10Req *r = v10_get(path, 60000);
    sb_append(sb, "<div class=\"ranked-rating-tab svelte-1gdnqq5\"><div class=\"controls-row svelte-1gdnqq5\"><div class=\"region-tabs svelte-1gdnqq5\">");
    for (int i = 0; i < (int) (sizeof(k_ranked_regions) / sizeof(k_ranked_regions[0])); i++)
        sb_appendf(sb, "<button type=\"button\" class=\"region-btn svelte-1gdnqq5%s\" onclick=\"knLbRegion(%d)\">%s</button>", i == LB.ranked_region ? " active" : "", i,
                   k_ranked_regions[i].label);
    sb_append(sb, "</div> <select class=\"season-select svelte-1gdnqq5\" onchange=\"knLbSeason(this.value)\">");
    for (int s = now; s >= 1; s--) {
        char label[48];
        season_label(label, sizeof(label), s);
        sb_appendf(sb, "<option class=\"svelte-1gdnqq5\" value=\"%d\"%s>%s</option>", s, s == season ? " selected" : "", label);
    }
    sb_append(sb, "</select></div> <div class=\"table-area svelte-1gdnqq5\">");
    const cJSON *rows = r->status == 200 ? cJSON_GetObjectItem(r->json, "leaderboard") : NULL;
    if (r->status != 200 && r->status != 0) {
        sb_append(sb, "<div class=\"status-msg error-msg svelte-1gdnqq5\">");
        const char *e = v10_str(r->json, "error");
        sb_append_esc(sb, e[0] ? e : "Failed to load ranked leaderboard");
        sb_append(sb, "</div>");
    } else if (r->status == 0 && !cJSON_GetArraySize(rows)) {
        sb_append(sb, "<div class=\"status-msg svelte-1gdnqq5\">Loading...</div>");
    } else if (!cJSON_GetArraySize(rows)) {
        sb_append(sb, "<div class=\"status-msg svelte-1gdnqq5\">No ranked data for this season.</div>");
    } else {
        sb_append(sb, "<div class=\"table-wrapper svelte-1gdnqq5\">");
        sb_append(sb, r->status == 0 ? "<div class=\"loading-overlay svelte-1gdnqq5\"><div class=\"loading-spinner svelte-1gdnqq5\"></div></div>" : "<!---->");
        sb_append(sb, " <table class=\"header-table svelte-1gdnqq5\"><thead><tr><th class=\"col-pos svelte-1gdnqq5\">#</th><th class=\"col-name svelte-1gdnqq5\">Player</th>"
                      "<th class=\"col-rank svelte-1gdnqq5\">Rank</th><th class=\"col-rating svelte-1gdnqq5\">Rating</th><th class=\"col-matches svelte-1gdnqq5\">Matches</th>"
                      "</tr></thead></table> <div class=\"table-scroll svelte-1gdnqq5\"><table class=\"data-table svelte-1gdnqq5\"><tbody>");
        const cJSON *row;
        cJSON_ArrayForEach(row, rows) {
            const cJSON *p = cJSON_GetObjectItem(row, "profile");
            const int rank = (int) v10_num(p, "rank", 0), kracked = rank == RANK_KRACKED;
            const char *name = v10_str(p, "player_name")[0] ? v10_str(p, "player_name") : v10_str(row, "player_name");
            sb_appendf(sb, "<tr class=\"%ssvelte-1gdnqq5\"><td class=\"col-pos svelte-1gdnqq5\">%.0f</td><td class=\"col-name svelte-1gdnqq5\">", kracked ? "kracked-row " : "",
                       v10_num(row, "position", 0));
            sb_append_esc(sb, name);
            sb_appendf(sb, "</td><td class=\"col-rank svelte-1gdnqq5\"><div class=\"rank-display svelte-1gdnqq5\"><img class=\"rank-icon svelte-1gdnqq5%s\" src=\"./%s\" alt=\"%s\"> "
                           "<span class=\"rank-text svelte-1gdnqq5\" style=\"color: %s\">%s</span></div></td><td class=\"col-rating svelte-1gdnqq5\">%d</td>"
                           "<td class=\"col-matches svelte-1gdnqq5\">%.0f</td></tr>",
                       kracked ? " kracked-icon" : "", ranked_rank_icon(rank), ranked_rank_name(rank), ranked_rank_color(rank), ranked_rank_name(rank),
                       ranked_display(v10_num(p, "conservative_rating", 0)), v10_num(p, "total_matches", 0));
        }
        sb_append(sb, "</tbody></table></div></div>");
    }
    sb_append(sb, "</div></div>");
}

// _49234 Matches tab (svelte-81ueoe): player_ranked_matches, All Time or a season
static void lb_ranked_matches(StrBuf *sb) {
    const int now = season_now();
    char path[160];
    if (LB.matches_season > 0) snprintf(path, sizeof(path), "/api/leaderboards/player_ranked_matches?season=%d", LB.matches_season);
    else snprintf(path, sizeof(path), "/api/leaderboards/player_ranked_matches");
    const V10Req *r = v10_get(path, 30000);
    sb_appendf(sb, "<div class=\"ranked-matches-tab svelte-81ueoe\"><div class=\"controls-row svelte-81ueoe\"><select class=\"season-select svelte-81ueoe\" "
                   "onchange=\"knLbMatchesSeason(this.value)\"><option class=\"svelte-81ueoe\" value=\"\"%s>All Time</option>",
               LB.matches_season > 0 ? "" : " selected");
    for (int s = now; s >= 1; s--) {
        char label[48];
        season_label(label, sizeof(label), s);
        sb_appendf(sb, "<option class=\"svelte-81ueoe\" value=\"%d\"%s>%s</option>", s, s == LB.matches_season ? " selected" : "", label);
    }
    sb_append(sb, "</select></div> <div class=\"table-area svelte-81ueoe\">");
    const cJSON *rows = data_array(r);
    if (r->status != 200 && r->status != 0) {
        sb_append(sb, "<div class=\"status-msg error-msg svelte-81ueoe\">Failed to load leaderboard</div>");
    } else if (!rows && r->status == 0) {
        sb_append(sb, "<div class=\"status-msg svelte-81ueoe\">Loading...</div>");
    } else if (!cJSON_GetArraySize(rows)) {
        sb_append(sb, "<div class=\"status-msg svelte-81ueoe\">No data available.</div>");
    } else {
        sb_append(sb, "<div class=\"table-wrapper svelte-81ueoe\"><!----> ");
        static const V10Col cols[] = {{"#", "50px", 1, 0}, {"Player", NULL, 0, 0}, {"Matches", NULL, 1, 0}};
        v10_table_begin(sb, cols, 3, -1, 1, NULL);
        const cJSON *row;
        cJSON_ArrayForEach(row, rows) {
            char pos[24], name[96], value[48];
            snprintf(pos, sizeof(pos), "#%.0f", v10_num(row, "position", 0));
            player_label(name, sizeof(name), row);
            player_value(value, sizeof(value), "player_ranked_matches", v10_num(row, "value", 0));
            v10_table_row(sb, NULL, 0);
            v10_table_cell(sb, &cols[0], pos);
            v10_table_cell(sb, &cols[1], name);
            v10_table_cell(sb, &cols[2], value);
            v10_table_row_end(sb);
        }
        v10_table_end(sb);
        sb_append(sb, "</div>");
    }
    sb_append(sb, "</div></div>");
}

// ------------------------------------------------------------------ KrunkCup (_49257, svelte-1xp8pv1)
// A team is a style of the KrunkCup Jerseys item (game.store.skins[7794], v10.js _10603[7794]) and its
// id is the style index. v10 names it styles[id] with " Away" removed (_49536; "Team <id>" past the
// list) and draws its jersey preview (_49539 -> _9305, getStylePreview: cosmetics/<dir>_<id>_<tex>_s<id>.png).
// The names are the KrunkCup Jerseys `styles` of assets/skins.json (index 7794); the viewer store does
// not carry them. v10 never shows the brain's team "name".
#define KC_JERSEY_SKIN 7794
static const char *const k_kc_styles[] = {
    "Algeria", "Argentina Away", "Australia", "Austria", "Belgium", "Brazil Away", "Canada Away", "Chile",
    "Colombia", "Costa Rica", "C\xc3\xb4te d'Ivoire", "Croatia Away", "Czechia", "England Away", "France",
    "Germany Away", "Ghana", "Greece", "Hungary", "Iceland", "Italy", "Jamaica", "Japan Away", "Mexico",
    "Morocco", "Netherlands", "New Zealand", "Nigeria Away", "Norway", "Paraguay", "Poland", "Portugal",
    "Qatar", "Saudi Arabia", "Senegal Away", "Spain", "Sweden", "Switzerland", "Turkey", "UAE", "Ukraine",
    "Uruguay", "USA Away", "Venezuela", "Wales", "Egypt", "Iraq", "Scotland",
};
#define KC_STYLES ((int) (sizeof(k_kc_styles) / sizeof(k_kc_styles[0])))

// the sort key of each table column (column 0 is the # rank column, which does not sort)
static const char *const k_kc_team_keys[] = {NULL, NULL, "total_score", "total_kills", "total_games"};
static const char *const k_kc_contrib_keys[] = {NULL, NULL, "kills", "score", "games"};

// v10's defaultSort: Score, descending (column 2 of the standings, column 3 of a team's contributors)
static int kc_sort_col(void) { return LB.kc_team >= 0 ? 3 : 2; }

static void kc_reset_sort(void) {
    LB.kc_sort = -1;
    LB.kc_sort_desc = 1;
}

static void kc_team_name(char *out, const size_t n, const int id) {
    if (id < 0 || id >= KC_STYLES) {
        snprintf(out, n, "Team %d", id);
        return;
    }
    snprintf(out, n, "%s", k_kc_styles[id]);
    const size_t len = strlen(out);
    if (len > 5 && !strcmp(out + len - 5, " Away")) out[len - 5] = 0; // replace(/\s+Away\b/gi, '')
}

// _9305(item, style) as an asset URL ("" when the jersey item is not in the store)
static void kc_team_thumb(char *out, const size_t n, const int id) {
    const ViewerSkin *s = viewer_skin(KC_JERSEY_SKIN);
    char path[256];
    out[0] = 0;
    if (!s || !s->name || strcmp(s->name, "KrunkCup Jerseys")) return;
    const int dir = s->asset_type ? s->asset_type : s->type;
    if (s->pat >= 0) snprintf(path, sizeof(path), "textures/previews/cosmetics/%d_%d_c%d_s%d.png", dir, s->id, s->pat, id);
    else if (viewer_skin_tex_truthy(s)) snprintf(path, sizeof(path), "textures/previews/cosmetics/%d_%d_%s_s%d.png", dir, s->id, s->tex, id);
    else snprintf(path, sizeof(path), "textures/previews/cosmetics/%d_%d_s%d.png", dir, s->id, id);
    html_dom_asset_url(path, out, n);
}

enum { KC_LOADING, KC_ERROR, KC_LOADED };

static int kc_truthy(const cJSON *v) {
    if (!v) return 0;
    if (cJSON_IsBool(v)) return cJSON_IsTrue(v);
    if (cJSON_IsNumber(v)) return v->valuedouble != 0;
    if (cJSON_IsString(v)) return v->valuestring && v->valuestring[0] != 0;
    return cJSON_IsObject(v) || cJSON_IsArray(v);
}

// the statusText of a 5xx answer (v10 throws "Server error: " + statusText)
static const char *kc_status_text(const int status) {
    switch (status) {
        case 500: return "Internal Server Error";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        default: return "";
    }
}

// v10 _49550 / _49543: the request fails (the catch shows its message) on a transport failure, a 5xx,
// a rate limit, an unreadable body or a body with an `error`; an error object's message wins over the
// fallback text `fail`.
static int kc_request(const V10Req *r, const char *fail, const cJSON **rows, char *msg, const size_t mn) {
    *rows = NULL;
    snprintf(msg, mn, "%s", fail);
    if (r->status == 0) return KC_LOADING;
    if (r->status < 0) {
        snprintf(msg, mn, "Failed to fetch");
        return KC_ERROR;
    }
    if (r->status >= 500) {
        snprintf(msg, mn, "Server error: %s", kc_status_text(r->status));
        return KC_ERROR;
    }
    if (r->status == 429) return KC_ERROR; // v10 answers with a string: error.message is undefined, so the fallback
    if (r->status == 204 || r->status == 304) return KC_LOADED;
    if (!r->json) {
        snprintf(msg, mn, "Invalid JSON response from server");
        return KC_ERROR;
    }
    const cJSON *err = cJSON_GetObjectItem(r->json, "error");
    if (kc_truthy(err)) {
        const char *m = cJSON_GetStringValue(cJSON_GetObjectItem(err, "message"));
        if (m && m[0]) snprintf(msg, mn, "%s", m);
        return KC_ERROR;
    }
    const cJSON *d = cJSON_GetObjectItem(r->json, "data");
    if (cJSON_IsArray(d)) *rows = d;
    return KC_LOADED;
}

// v10's generic table comparator (_46109): a missing value sorts last whatever the direction
static int kc_cmp(const cJSON *a, const cJSON *b, const char *key, const int desc) {
    const cJSON *va = cJSON_GetObjectItem(a, key), *vb = cJSON_GetObjectItem(b, key);
    if (!cJSON_IsNumber(va)) return 1;
    if (!cJSON_IsNumber(vb)) return -1;
    const double x = va->valuedouble, y = vb->valuedouble;
    return desc ? (y > x) - (y < x) : (x > y) - (x < y);
}

// stable, like Array.prototype.sort
static void kc_sort_rows(const cJSON **rows, const int n, const char *key, const int desc) {
    for (int i = 1; i < n; i++) {
        const cJSON *x = rows[i];
        int j = i;
        while (j > 0 && kc_cmp(rows[j - 1], x, key, desc) > 0) {
            rows[j] = rows[j - 1];
            j--;
        }
        rows[j] = x;
    }
}

static void kc_team_cell(StrBuf *sb, const V10Col *col, const int id) {
    char thumb[512], name[96];
    kc_team_thumb(thumb, sizeof(thumb), id);
    kc_team_name(name, sizeof(name), id);
    StrBuf cell = {0};
    sb_appendf(&cell, "<span class=\"team-cell svelte-zls0ao\"><span class=\"thumb svelte-zls0ao\" style=\"background-image: url('%s')\"></span> "
                      "<span class=\"name svelte-zls0ao\">", thumb);
    sb_append_esc(&cell, name);
    sb_append(&cell, "</span></span>");
    char *html = sb_finish(&cell);
    v10_table_cell_html(sb, col, html);
    free(html);
}

// v10's generic table (_46109) with rank: the standings (_49557) or a team's contributors (_49565)
static void kc_table(StrBuf *sb, const cJSON *rows, const int n, const int team) {
    static const V10Col team_cols[] = {{"#", "50px", 1, 0}, {"Team", NULL, 0, 0}, {"Score", NULL, 1, 1}, {"Kills", NULL, 1, 1}, {"Games", NULL, 1, 1}};
    static const V10Col contrib_cols[] = {{"#", "50px", 1, 0}, {"Player", NULL, 0, 0}, {"Kills", NULL, 1, 1}, {"Score", NULL, 1, 1}, {"Games", NULL, 1, 1}};
    const V10Col *cols = team >= 0 ? contrib_cols : team_cols;
    const char *const *keys = team >= 0 ? k_kc_contrib_keys : k_kc_team_keys;
    const int sort = LB.kc_sort >= 0 ? LB.kc_sort : kc_sort_col();
    const int desc = LB.kc_sort >= 0 ? LB.kc_sort_desc : 1;
    const cJSON **sorted = malloc(sizeof(*sorted) * (size_t) n);
    if (!sorted) return;
    int k = 0;
    const cJSON *row;
    cJSON_ArrayForEach(row, rows) sorted[k++] = row;
    kc_sort_rows(sorted, n, keys[sort], desc);
    v10_table_begin(sb, cols, 5, sort, desc, "knLbKcSort(%d)");
    for (int i = 0; i < n; i++) {
        const cJSON *e = sorted[i];
        char rank[24], a[32], b[32], c[32];
        snprintf(rank, sizeof(rank), "#%d", i + 1);
        if (team >= 0) {
            char who[96];
            player_label(who, sizeof(who), e);
            v10_fmt_int(a, sizeof(a), v10_num(e, "kills", 0));
            v10_fmt_int(b, sizeof(b), v10_num(e, "score", 0));
            v10_fmt_int(c, sizeof(c), v10_num(e, "games", 0));
            v10_table_row(sb, NULL, 0);
            v10_table_cell(sb, &cols[0], rank);
            v10_table_cell(sb, &cols[1], who);
            v10_table_cell(sb, &cols[2], a);
            v10_table_cell(sb, &cols[3], b);
            v10_table_cell(sb, &cols[4], c);
        } else {
            char click[96];
            snprintf(click, sizeof(click), "playTick(0.5);knLbKcTeam(%d)", (int) v10_num(e, "id", -1));
            v10_fmt_int(a, sizeof(a), v10_num(e, "total_score", 0));
            v10_fmt_int(b, sizeof(b), v10_num(e, "total_kills", 0));
            v10_fmt_int(c, sizeof(c), v10_num(e, "total_games", 0));
            v10_table_row(sb, click, 0);
            v10_table_cell(sb, &cols[0], rank);
            kc_team_cell(sb, &cols[1], (int) v10_num(e, "id", -1));
            v10_table_cell(sb, &cols[2], a);
            v10_table_cell(sb, &cols[3], b);
            v10_table_cell(sb, &cols[4], c);
        }
        v10_table_row_end(sb);
    }
    v10_table_end(sb);
    free(sorted);
}

// _49257 KrunkCup: the team standings, or a team's top contributors once a team is picked
static void lb_krunkcup(StrBuf *sb) {
    const int team = LB.kc_team;
    char thumb[512], name[96], path[128], msg[160];
    sb_append(sb, "<div class=\"krunkcup-component svelte-1xp8pv1\"><div class=\"kc-header svelte-1xp8pv1\">"
                  "<button type=\"button\" class=\"back-btn svelte-1xp8pv1\" onclick=\"knLbKcBack()\">&#8592; Back</button> ");
    if (team >= 0) {
        kc_team_thumb(thumb, sizeof(thumb), team);
        kc_team_name(name, sizeof(name), team);
        sb_appendf(sb, "<span class=\"kc-thumb svelte-1xp8pv1\" style=\"background-image: url('%s')\"></span> <span class=\"kc-title svelte-1xp8pv1\">", thumb);
        sb_append_esc(sb, name);
        sb_append(sb, "</span>");
    } else sb_append(sb, "<span class=\"kc-title svelte-1xp8pv1\">KrunkCup</span>");
    sb_append(sb, "</div> ");
    if (team >= 0) snprintf(path, sizeof(path), "/api/leaderboards/krunkcup/teams/%d/contributors", team);
    else snprintf(path, sizeof(path), "/api/leaderboards/krunkcup/teams");
    const V10Req *r = v10_get(path, 30000);
    const cJSON *rows = NULL;
    const int st = kc_request(r, team >= 0 ? "Failed to fetch KrunkCup contributors" : "Failed to fetch KrunkCup teams", &rows, msg, sizeof(msg));
    const int n = rows ? cJSON_GetArraySize(rows) : 0;
    if (st == KC_LOADING) sb_append(sb, "<div class=\"loading svelte-1xp8pv1\">Loading...</div>");
    else if (st == KC_ERROR) {
        sb_append(sb, "<div class=\"error svelte-1xp8pv1\">");
        sb_append_esc(sb, msg);
        sb_append(sb, "</div>");
    } else if (!n)
        sb_appendf(sb, "<div class=\"no-data svelte-1xp8pv1\">%s</div>",
                   team >= 0 ? "No contributors for this team yet." : "No KrunkCup data yet. Equip a KrunkCup jersey with a team style and play!");
    else kc_table(sb, rows, n, team);
    sb_append(sb, "</div>");
}

static void lb_view(StrBuf *sb, const char *title, const char *const *tabs, const int ntabs, const int active, const char *tab_handler) {
    sb_append(sb, "<div class=\"category-view svelte-1k16b2z\"><div class=\"view-header svelte-1k16b2z\">");
    v10_back_button(sb, "playSelect();knLbBack()");
    sb_appendf(sb, " <h2 class=\"view-title svelte-1k16b2z\">%s</h2></div> ", title);
    v10_tabs(sb, tabs, ntabs, active, tab_handler);
    sb_append(sb, " <div class=\"tab-content svelte-1k16b2z\">");
}

static char *lb_gen(void) {
    StrBuf sb = {0};
    sb_append(&sb, "<div class=\"window-container svelte-623zut\"><div class=\"io-popup-content svelte-1k16b2z\">");
    const char *labels[16];
    switch (LB.cat) {
        case LB_PLAYERS:
            for (int i = 0; i < PLAYER_TABS; i++) labels[i] = k_player_tabs[i].label;
            lb_view(&sb, "Player Leaderboards", labels, PLAYER_TABS, LB.player_tab, "knLbTab");
            lb_component(&sb, k_player_tabs[LB.player_tab].type, NULL);
            break;
        case LB_CLANS:
            for (int i = 0; i < 2; i++) labels[i] = k_clan_tabs[i].label;
            lb_view(&sb, "Clan Leaderboards", labels, 2, LB.clan_tab, "knLbTab");
            lb_component(&sb, "player_clan", k_clan_tabs[LB.clan_tab].sort);
            break;
        case LB_RAIDS:
            for (int i = 0; i < RAID_TABS; i++) labels[i] = k_raid_tabs[i].label;
            lb_view(&sb, "Raid Leaderboards", labels, RAID_TABS, LB.raid_tab, "knLbTab");
            lb_raids(&sb);
            break;
        case LB_RANKED:
            labels[0] = "Rating";
            labels[1] = "Matches";
            lb_view(&sb, "Ranked Leaderboards", labels, 2, LB.ranked_tab, "knLbTab");
            if (LB.ranked_tab == 0) lb_ranked_rating(&sb);
            else lb_ranked_matches(&sb);
            break;
        case LB_KRUNKCUP:
            sb_append(&sb, "<div class=\"category-view svelte-1k16b2z\"><div class=\"tab-content svelte-1k16b2z\">");
            lb_krunkcup(&sb);
            break;
        default:
            sb_append(&sb, "<div class=\"category-grid svelte-1k16b2z\">"
                           "<button type=\"button\" class=\"category-card svelte-1k16b2z\" onclick=\"playSelect();knLbCategory(0)\"><div class=\"card-icon svelte-1k16b2z\">&#9733;</div> "
                           "<h3 class=\"card-title svelte-1k16b2z\">Players</h3> <p class=\"card-description svelte-1k16b2z\">Level, Kills, Wins, Time, KR, Followers, and more.</p></button> "
                           "<button type=\"button\" class=\"category-card svelte-1k16b2z\" onclick=\"playSelect();knLbCategory(1)\"><div class=\"card-icon svelte-1k16b2z\">&#9876;</div> "
                           "<h3 class=\"card-title svelte-1k16b2z\">Clans</h3> <p class=\"card-description svelte-1k16b2z\">Top clans ranked by score or member count.</p></button> "
                           "<button type=\"button\" class=\"category-card svelte-1k16b2z\" onclick=\"playSelect();knLbCategory(2)\"><div class=\"card-icon svelte-1k16b2z\">&#9760;</div> "
                           "<h3 class=\"card-title svelte-1k16b2z\">Raids</h3> <p class=\"card-description svelte-1k16b2z\">Best completion times per raid map.</p></button> "
                           "<button type=\"button\" class=\"category-card svelte-1k16b2z\" onclick=\"playSelect();knLbCategory(3)\"><div class=\"card-icon svelte-1k16b2z\">&#9884;</div> "
                           "<h3 class=\"card-title svelte-1k16b2z\">Ranked</h3> <p class=\"card-description svelte-1k16b2z\">Top players by rating and matches across seasons.</p></button> "
                           "<button type=\"button\" class=\"category-card svelte-1k16b2z\" onclick=\"playSelect();knLbCategory(4)\"><div class=\"card-icon svelte-1k16b2z\">");
            v10_emoji(&sb, "\xf0\x9f\x8f\x86", ""); // Chrome draws 🏆 in colour (Noto Color Emoji); ⚔ ☠ ⚜ ★ stay text (README)
            sb_append(&sb, "</div> "
                           "<h3 class=\"card-title svelte-1k16b2z\">KrunkCup</h3> <p class=\"card-description svelte-1k16b2z\">Team standings and top contributors for the KrunkCup.</p></button></div>"
                           "<!----></div><!----></div>");
            return sb_finish(&sb);
    }
    sb_append(&sb, "</div></div><!----></div><!----></div>");
    return sb_finish(&sb);
}

static void lb_render(void) {
    char *html = lb_gen();
    v10_popup_set("defaultModal", html);
    free(html);
}

// window.openLeaderboards(initialCategory): genericPopup({component: _49281}, 'defaultModal')
static void lb_open(const char *category) {
    static const char *const names[LB_CATEGORIES] = {"players", "clans", "raids", "ranked", "krunkcup"};
    LB.cat = LB_GRID;
    for (int i = 0; category && i < LB_CATEGORIES; i++)
        if (!strcmp(category, names[i])) LB.cat = i;
    LB.kc_team = -1;
    kc_reset_sort();
    char *html = lb_gen();
    v10_popup_show("defaultModal", html, lb_render);
    free(html);
}

// ------------------------------------------------------------------ handlers

static int arg_int(const HtmlArg *args, const int argc, const int i, const int def) {
    if (i >= argc) return def;
    if (args[i].kind == HTML_ARG_NUM || args[i].kind == HTML_ARG_BOOL) return (int) args[i].num;
    if (args[i].kind == HTML_ARG_STR && args[i].str) return atoi(args[i].str);
    return def;
}

// this.value of a select / input (args[i] is HTML_ARG_THIS_VALUE)
static void arg_value(const HtmlEvent *ev, const HtmlArg *args, const int argc, const int i, char *out, const size_t n) {
    out[0] = 0;
    if (i >= argc) return;
    if (args[i].kind == HTML_ARG_STR && args[i].str) snprintf(out, n, "%s", args[i].str);
    else if (args[i].kind == HTML_ARG_NUM) snprintf(out, n, "%g", args[i].num);
    else if (ev && ev->value && ev->value[0]) snprintf(out, n, "%s", ev->value);
    else if (ev && ev->target) html_dom_get_value(ev->target, out, n);
}

int ranked_ui_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, int argc);
int turf_ui_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, int argc);

int v10_competitive_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    if (!name) return -1;
    if (!strcmp(name, "openLeaderboards")) {
        char cat[32] = "";
        if (argc > 0 && args[0].kind == HTML_ARG_STR && args[0].str) snprintf(cat, sizeof(cat), "%s", args[0].str);
        lb_open(cat[0] ? cat : NULL);
        return 1;
    }
    if (!strcmp(name, "knViewPop")) {
        v10_view_pop(arg_int(args, argc, 0, 0));
        return 1;
    }
    if (!strcmp(name, "knViewBackdrop")) { // only a click on the overlay itself dismisses
        HtmlElem hit = ev ? html_dom_element_at(ev->x, ev->y) : 0;
        if (hit && html_dom_has_class(hit, "view-stack-overlay")) v10_view_pop(0);
        return 1;
    }
    if (strncmp(name, "knLb", 4)) return -1;
    const int a0 = arg_int(args, argc, 0, 0);
    char v[32];
    if (!strcmp(name, "knLbCategory")) {
        if (a0 >= 0 && a0 < LB_CATEGORIES) LB.cat = a0;
        LB.kc_team = -1;
        kc_reset_sort();
    } else if (!strcmp(name, "knLbBack")) {
        LB.cat = LB_GRID;
    } else if (!strcmp(name, "knLbTab")) {
        switch (LB.cat) {
            case LB_PLAYERS: if (a0 >= 0 && a0 < PLAYER_TABS) LB.player_tab = a0; break;
            case LB_CLANS: if (a0 >= 0 && a0 < 2) LB.clan_tab = a0; break;
            case LB_RAIDS: if (a0 >= 0 && a0 < RAID_TABS) LB.raid_tab = a0; break;
            case LB_RANKED: if (a0 >= 0 && a0 < 2) LB.ranked_tab = a0; break;
            default: break;
        }
    } else if (!strcmp(name, "knLbRegion")) {
        if (a0 >= 0 && a0 < (int) (sizeof(k_ranked_regions) / sizeof(k_ranked_regions[0]))) LB.ranked_region = a0;
    } else if (!strcmp(name, "knLbSeason")) {
        arg_value(ev, args, argc, 0, v, sizeof(v));
        LB.ranked_season = v[0] ? atoi(v) : -1;
    } else if (!strcmp(name, "knLbMatchesSeason")) {
        arg_value(ev, args, argc, 0, v, sizeof(v));
        LB.matches_season = v[0] ? atoi(v) : 0;
    } else if (!strcmp(name, "knLbRaidSort")) {
        LB.raid_completions = a0 != 0;
    } else if (!strcmp(name, "knLbRaidMonth")) {
        arg_value(ev, args, argc, 0, v, sizeof(v));
        snprintf(LB.raid_month, sizeof(LB.raid_month), "%s", v[0] ? v : "all-time");
    } else if (!strcmp(name, "knLbKcTeam")) {
        LB.kc_team = a0;
        kc_reset_sort();
    } else if (!strcmp(name, "knLbKcSort")) {
        const int cur = LB.kc_sort >= 0 ? LB.kc_sort : kc_sort_col();
        const int desc = LB.kc_sort >= 0 ? LB.kc_sort_desc : 1;
        if (a0 >= 2 && a0 <= 4) {
            LB.kc_sort = a0;
            LB.kc_sort_desc = a0 == cur ? !desc : 1; // v10 _46124: the same column toggles, a new one starts descending
        }
    } else if (!strcmp(name, "knLbKcBack")) {
        if (LB.kc_team >= 0) LB.kc_team = -1;
        else LB.cat = LB_GRID;
        kc_reset_sort();
    } else {
        return -1;
    }
    if (v10_popup_open("defaultModal")) lb_render();
    return 1;
}

// ------------------------------------------------------------------ the page's menu entries

static void set_click(const char *id, const char *closest, const char *handler) {
    HtmlElem el = html_dom_by_id(id);
    if (!el) return;
    if (closest) {
        HtmlElem c = html_dom_closest(el, closest);
        if (c) el = c;
    }
    char have[160] = "";
    if (html_dom_get_attr(el, "onclick", have, sizeof(have)) >= 0 && have[0]) return; // the page wires it
    html_dom_set_attr(el, "onclick", handler);
}

// v10's menu: _50410 actionCard Ranked -> openRankedMenu() (v10:147331); _50477 nav items Turf
// Wars -> genericPopup(_51011, 'defaultModal') (v10:147059) and Leaderboards ->
// genericPopup(_49281, 'defaultModal') (v10:147139). The nav item's click is on .menuItem.
void v10_competitive_setup(void) {
    if (!html_dom_query(0, ".actionCard")) return; // not the v10 page
    set_click("menuBtnRanked", NULL, "playSelect(0.1);openRankedMenu()");
    set_click("menuBtnTurfWars", ".menuItem", "playSelect(0.1);openTurfWars()");
    set_click("menuBtnLeaderboards", ".menuItem", "playSelect(0.1);openLeaderboards()");
    views_render();
}

void v10_scenario_answer(const char *path, const cJSON *json) {
    g_scenario = 1;
    ReqSlot *s = req_find(path);
    if (!s) s = req_new(path);
    if (!s) return;
    cJSON_Delete(s->req.json);
    const cJSON *status = cJSON_GetObjectItem(json, "__status");
    s->req.status = cJSON_IsNumber(status) ? status->valueint : 200;
    s->req.json = cJSON_Duplicate(json, 1);
    s->at_ms = 1;
}

// --render-scenario: state.leaderboards {category, tab}; state.v10responses {path: answer} feeds
// the request cache for every popup of this file, ranked_ui.c and turf_ui.c
void v10_competitive_scenario(const cJSON *state) {
    const cJSON *responses = cJSON_GetObjectItem(state, "v10responses");
    if (cJSON_IsObject(responses)) {
        g_scenario = 1;
        const cJSON *r;
        cJSON_ArrayForEach(r, responses) v10_scenario_answer(r->string, r);
    }
    const cJSON *lb = cJSON_GetObjectItem(state, "leaderboards");
    if (cJSON_IsObject(lb)) {
        g_scenario = 1;
        const char *cat = v10_str(lb, "category");
        lb_open(cat[0] ? cat : NULL);
        const int tab = (int) v10_num(lb, "tab", 0);
        if (LB.cat == LB_PLAYERS && tab < PLAYER_TABS) LB.player_tab = tab;
        if (LB.cat == LB_CLANS && tab < 2) LB.clan_tab = tab;
        if (LB.cat == LB_RAIDS && tab < RAID_TABS) LB.raid_tab = tab;
        if (LB.cat == LB_RANKED && tab < 2) LB.ranked_tab = tab;
        LB.ranked_region = (int) v10_num(lb, "region", 0) & 3;
        LB.ranked_season = (int) v10_num(lb, "season", -1);
        if (LB.cat == LB_KRUNKCUP) LB.kc_team = (int) v10_num(lb, "team", -1);
        lb_render();
    }
}
