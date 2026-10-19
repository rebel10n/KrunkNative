// v10's Junkyard tab, ported from its Svelte components (svelte-1vqzycn: the page, the filter bar,
// the grid; svelte-ggnigm: the recipe card; v10.js 132366-132400, 136440-136770). The order and
// the states follow v10 (see the comments on each step); the protocol is kn_brain's craft.c, so
// a craft is one ["crft", SID] and its answer ["crftr", SID, ok, msg].
//
// Deviations (doc/v7/decisions.md, Crafting): the skin is granted at the request, so there is no
// crafting timer (v10's STATORD / EXP): the card shows "Crafting..." only while the answer is out,
// and the confirmation shows no craft time. The recipe rows are the brain's table
// (assets/junkyard_recipes.json), empty until v10's rows are copied in: until then the grid says
// "No items match your search.", as v10 does for an empty list. The item click opens the
// exchange's item page (Market) for that skin.
#include <backend.h>
#include <backend_ui.h>
#include <html_dom.h>
#include <ui_logic.h>
#include <cJSON.h>

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "v10_junkyard.h"
#include "../viewer/viewer_store.h"

#define JY_MAX 512        // the recipes the grid shows (the table is far smaller)
#define MATS_MAX 5        // v10's SIN: up to five materials
#define SEARCH_MAX 64
#define NEW_MS 172800000.0 // v10: a recipe added within two days is NEW (Math.abs(now - DAT) < 172800000)
#define OWNED_TAB 10      // acc.data[10]: the account's skins ({ind, cnt})

enum { SORT_DEFAULT, SORT_RARITY, SORT_NAME, SORT_JUNK, SORT_COUNT };
// v10's select: the option values and labels
static const char *const k_sort_value[SORT_COUNT] = {"default", "rarity", "name", "junk"};
static const char *const k_sort_label[SORT_COUNT] = {"Default Sort", "Sort by Rarity", "Sort by Name", "Sort by Junk Price"};

// one recipe as the grid sees it (v10's `cftItm` / `requirements` / the order keys)
typedef struct {
    const ViewerSkin *skin;     // the result (SOUT)
    int sid, sout, cnt;         // CNT: copies left in the world, -1 unlimited, 0 sold out
    double junk, dat, order;
    int n;
    int mat[MATS_MAX], mat_n[MATS_MAX];
    int is_new, affordable, mats_ok;
} JyItem;

static struct {
    int inited, listening;
    cJSON *rows;                // the recps rows (NULL until answered, or failed)
    int loading, failed;
    double junk;                // the account's junk balance (recps)
    char search[SEARCH_MAX];
    int sort;
    int confirm;                // SID of the craft confirmation, -1 none
    int crafting;               // SID of the craft in flight, -1 none
    int done_sid, done_skin;    // the result popup, -1 none
    char err[200];              // the error popup's text, "" none
} J;

static JyItem s_items[JY_MAX];  // the recipes as built (build_items)
static JyItem s_list[JY_MAX];   // the grid's list (visible_items); overlay_html reads it after grid_html

static void jy_init(void) {
    if (J.inited) return;
    J.inited = 1;
    J.confirm = -1;
    J.crafting = -1;
    J.done_sid = -1;
}

static const AccountInfo *acc(void) { return active_account(); }

static double num(const cJSON *o, const char *k, const double def) {
    const cJSON *v = o ? cJSON_GetObjectItemCaseSensitive(o, k) : NULL;
    return cJSON_IsNumber(v) ? v->valuedouble : def;
}

// acc.skins[ind].cnt (0 for none / guests)
static int owned(const int skin) {
    const AccountInfo *a = acc();
    const cJSON *skins = a && a->data ? cJSON_GetArrayItem(a->data, OWNED_TAB) : NULL, *e;
    cJSON_ArrayForEach(e, skins) {
        const cJSON *ind = cJSON_GetObjectItem(e, "ind"), *cnt = cJSON_GetObjectItem(e, "cnt");
        if (cJSON_IsNumber(ind) && ind->valueint == skin) return cJSON_IsNumber(cnt) ? cnt->valueint : 1;
    }
    return 0;
}

static const char *rarity_color(const ViewerSkin *s) {
    return s && s->rarity >= 0 && s->rarity < g_viewer_rarity_count ? g_viewer_rarities[s->rarity].color : "#B0B0B0";
}

static void thumb(char *out, const size_t n, const ViewerSkin *s) {
    char path[256];
    viewer_skin_preview_path(s, path, sizeof path);
    html_dom_asset_url(path, out, n);
}

// ------------------------------------------------------------------ the recipes

// "skin,count,skin,count" -> the pairs (up to MATS_MAX; kn_brain has checked every row)
static void parse_mats(JyItem *it, const char *sin) {
    char buf[160], *save = NULL;
    snprintf(buf, sizeof buf, "%s", sin ? sin : "");
    for (char *tok = strtok_r(buf, ",", &save); tok && it->n < MATS_MAX; tok = strtok_r(NULL, ",", &save)) {
        char *cnt = strtok_r(NULL, ",", &save);
        if (!cnt) break;
        it->mat[it->n] = atoi(tok);
        it->mat_n[it->n] = atoi(cnt);
        it->n++;
    }
}

// the recps rows as JyItems (v10 craftData.dat: rows whose result the store lacks are not shown)
static int build_items(void) {
    int n = 0;
    if (!J.rows) return 0;
    const double now = (double) time(NULL) * 1000.0;
    const int logged = acc() != NULL;
    const cJSON *row;
    cJSON_ArrayForEach(row, J.rows) {
        if (n >= JY_MAX) break;
        JyItem *it = &s_items[n];
        memset(it, 0, sizeof *it);
        it->sid = (int) num(row, "SID", -1);
        it->sout = (int) num(row, "SOUT", -1);
        it->skin = viewer_skin(it->sout);
        if (!it->skin) continue;
        it->cnt = (int) num(row, "CNT", -1);
        it->junk = num(row, "JUNK", 0);
        it->dat = num(row, "DAT", 0);
        parse_mats(it, cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(row, "SIN")));
        it->mats_ok = 1;
        for (int i = 0; i < it->n; i++)
            if (owned(it->mat[i]) < it->mat_n[i]) it->mats_ok = 0;
        // v10's order: the junk price; +10000 when NEW; +15000 when it can be crafted now, else -15000
        it->is_new = fabs(now - it->dat) < NEW_MS;
        it->affordable = logged && J.junk >= it->junk && it->mats_ok;
        it->order = it->junk + (it->is_new ? 10000.0 : 0.0) + (it->affordable ? 15000.0 : -15000.0);
        n++;
    }
    return n;
}

// v10's `order || -1`: a falsy order sorts as -1
static double order_or_m1(const JyItem *it) { return it->order == 0 ? -1.0 : it->order; }

// stable insertion sort: a moves before b only when less(a, b)
static void sort_items(JyItem *v, const int n, int (*less)(const JyItem *, const JyItem *)) {
    for (int i = 1; i < n; i++) {
        const JyItem t = v[i];
        int j = i;
        while (j > 0 && less(&t, &v[j - 1])) {
            v[j] = v[j - 1];
            j--;
        }
        v[j] = t;
    }
}

static int less_default(const JyItem *a, const JyItem *b) { return order_or_m1(a) > order_or_m1(b); }
static int less_rarity(const JyItem *a, const JyItem *b) { return a->skin->rarity > b->skin->rarity; }
static int less_junk(const JyItem *a, const JyItem *b) { return a->junk > b->junk; }

static int ci_compare(const char *a, const char *b) {
    for (;; a++, b++) {
        const int ca = tolower((unsigned char) *a), cb = tolower((unsigned char) *b);
        if (ca != cb) return ca - cb;
        if (!ca) return 0;
    }
}
static int less_name(const JyItem *a, const JyItem *b) {
    return ci_compare(a->skin->name ? a->skin->name : "", b->skin->name ? b->skin->name : "") < 0;
}

static int ci_contains(const char *hay, const char *needle) {
    const size_t nl = strlen(needle);
    if (!nl) return 1;
    for (; *hay; hay++) {
        size_t k = 0;
        while (k < nl && hay[k] && tolower((unsigned char) hay[k]) == tolower((unsigned char) needle[k])) k++;
        if (k == nl) return 1;
    }
    return 0;
}

// the list the grid shows: v10's order, then the search (case-insensitive name), then the sort
static int visible_items(void) {
    const int n = build_items();
    int m = 0;
    char needle[SEARCH_MAX];
    size_t a = 0;
    while (J.search[a] == ' ') a++;
    snprintf(needle, sizeof needle, "%s", J.search + a);
    for (size_t e = strlen(needle); e > 0 && needle[e - 1] == ' '; e--) needle[e - 1] = 0;
    // v10 sorts by the order first; the order is kept by the stable sorts below
    JyItem *v = s_list;
    for (int i = 0; i < n; i++) {
        if (!ci_contains(s_items[i].skin->name ? s_items[i].skin->name : "", needle)) continue;
        v[m++] = s_items[i];
    }
    sort_items(v, m, less_default);
    if (J.sort == SORT_RARITY) sort_items(v, m, less_rarity);
    else if (J.sort == SORT_NAME) sort_items(v, m, less_name);
    else if (J.sort == SORT_JUNK) sort_items(v, m, less_junk);
    return m;
}

// v10's cantCraft: the junk first, Sold out over it, Missing materials only when nothing else
static const char *cant_reason(const JyItem *it) {
    const char *r = NULL;
    if (!acc() || J.junk < it->junk) r = "Not enough Junk";
    if (it->cnt == 0) r = "Sold out";
    if (!r && !it->mats_ok) r = "Missing materials";
    return r;
}

// ------------------------------------------------------------------ markup

static void mat_thumb_block(StrBuf *sb, const JyItem *it, const int i) {
    const ViewerSkin *m = viewer_skin(it->mat[i]);
    if (!m) return;
    const int have = owned(it->mat[i]) >= it->mat_n[i];
    char url[512];
    thumb(url, sizeof url, m);
    sb_appendf(sb, "<div class=\"craft-item-req svelte-ggnigm%s\" role=\"button\" tabindex=\"0\" style=\"border: 2px solid %s; background-image: url(%s);\" onclick=\"v10jy.item(%d)\">"
                   "<div class=\"craft-req-count svelte-ggnigm\">x%d</div></div>",
               have ? "" : " missing", have ? rarity_color(m) : "#5e5e5e", url, it->mat[i], it->mat_n[i]);
}

static void card(StrBuf *sb, const JyItem *it) {
    const ViewerSkin *s = it->skin;
    const char *col = rarity_color(s);
    const int crafting = J.crafting == it->sid;
    char url[512];
    thumb(url, sizeof url, s);
    sb_append(sb, "<div class=\"craft-recipe-card svelte-ggnigm\">");
    sb_appendf(sb, "<div class=\"craft-icon svelte-ggnigm\" role=\"button\" tabindex=\"0\" style=\"border: 3px solid %s;\" onclick=\"v10jy.item(%d)\">", col, it->sout);
    sb_appendf(sb, "<img style=\"height: 100%%\" class=\"svelte-ggnigm\" src=\"%s\" alt=\"", url);
    sb_append_esc(sb, s->name ? s->name : "");
    sb_append(sb, "\"> <div class=\"craft-item-name svelte-ggnigm\" style=\"color: ");
    sb_append(sb, col);
    sb_append(sb, ";\">");
    sb_append_esc(sb, s->name ? s->name : "");
    sb_appendf(sb, "</div> <div class=\"craft-item-junk svelte-ggnigm\">%.2f <span class=\"material-icons svelte-ggnigm\" style=\"color: #cc51f8; font-size: 34px; margin-left: 5px; vertical-align: top;\">plumbing</span></div>",
               it->junk);
    if (it->is_new && !crafting) sb_append(sb, "<div class=\"craft-new-tag svelte-ggnigm\">NEW</div>");
    if (it->cnt > 0) sb_appendf(sb, "<div class=\"craft-limit-amount svelte-ggnigm\" title=\"%d left in the world\">x%d</div>", it->cnt, it->cnt);
    if (crafting) sb_append(sb, "<div class=\"craft-hammer-animation svelte-ggnigm\"><span class=\"material-icons craft-hammer-icon svelte-ggnigm\">plumbing</span></div>");
    sb_append(sb, "</div>");
    sb_append(sb, "<div class=\"craft-requirements-list svelte-ggnigm\">");
    for (int i = 0; i < it->n; i++) mat_thumb_block(sb, it, i);
    sb_append(sb, "</div>");
    const char *reason = cant_reason(it);
    if (crafting) sb_append(sb, "<div class=\"craft-timer svelte-ggnigm\">Crafting...</div>");
    else if (reason) {
        sb_append(sb, "<div class=\"craft-timer svelte-ggnigm\">");
        sb_append_esc(sb, reason);
        sb_append(sb, "</div>");
    } else sb_appendf(sb, "<div class=\"craft-button svelte-ggnigm\" id=\"v10jyCraft%d\" role=\"button\" tabindex=\"0\" onclick=\"v10jy.craft(%d)\">Craft</div>", it->sid, it->sid);
    sb_append(sb, "</div>");
}

// the grid's cards, or v10's empty line
static void grid_html(StrBuf *sb) {
    const int n = visible_items();
    if (!n) {
        sb_append(sb, "<div class=\"jy-empty svelte-1vqzycn\">No items match your search.</div>");
        return;
    }
    for (int i = 0; i < n; i++) card(sb, &s_list[i]);
}

static void refresh_grid(void) {
    const HtmlElem e = html_dom_by_id("jyGrid");
    if (!e) return;
    StrBuf sb = {0};
    grid_html(&sb);
    char *html = sb_finish(&sb);
    html_dom_set_html(e, html);
    free(html);
}

// the craft confirmation (v10's sendCraft popup, without the craft time), the result and the error
static void overlay_html(StrBuf *sb) {
    if (J.confirm >= 0) {
        const int n = visible_items();
        const JyItem *it = NULL;
        for (int i = 0; i < n; i++)
            if (s_list[i].sid == J.confirm) it = &s_list[i];
        if (!it) return;
        const char *col = rarity_color(it->skin);
        sb_append(sb, "<div class=\"modal-overlay svelte-1ugt6n9\" role=\"dialog\" aria-modal=\"true\" aria-label=\"Craft confirmation\" tabindex=\"-1\" onclick=\"v10jy.cclose()\">"
                      "<div class=\"modal-content svelte-1ugt6n9\" role=\"document\" onclick=\"v10jy.stop()\"><h3 class=\"svelte-1ugt6n9\">Craft <span style=\"color: ");
        sb_append(sb, col);
        sb_append(sb, "\">");
        sb_append_esc(sb, it->skin->name ? it->skin->name : "");
        sb_appendf(sb, "</span></h3><div style=\"color: #979797;\">Junk<span style=\"float: right; color: #e5e5e5;\">-%.2f <span class=\"material-icons\" style=\"color: #cc51f8; font-size: 30px; vertical-align: middle;\">plumbing</span></span></div>", it->junk);
        sb_append(sb, "<div style=\"color: #979797;\">Materials</div><div style=\"display: flex; gap: 6px; justify-content: flex-end; margin-bottom: 12px;\">");
        for (int i = 0; i < it->n; i++) {
            const ViewerSkin *m = viewer_skin(it->mat[i]);
            if (!m) continue;
            char url[512];
            thumb(url, sizeof url, m);
            sb_appendf(sb, "<div class=\"craft-item-req svelte-ggnigm\" style=\"border: 2px solid %s; background-image: url(%s);\"><div class=\"craft-req-count svelte-ggnigm\">x%d</div></div>",
                       owned(it->mat[i]) >= it->mat_n[i] ? rarity_color(m) : "#5e5e5e", url, it->mat_n[i]);
        }
        sb_append(sb, "</div><div style=\"display: flex; gap: 10px;\">");
        sb_append(sb, "<button class=\"main-button svelte-whnh40\" id=\"v10jyCraftGo\" type=\"button\" onclick=\"v10jy.cgo()\">Craft</button>");
        sb_append(sb, "<button class=\"main-button svelte-whnh40\" id=\"v10jyCraftNo\" type=\"button\" onclick=\"v10jy.cclose()\">Cancel</button>");
        sb_append(sb, "</div></div></div>");
    } else if (J.done_sid >= 0) {
        const ViewerSkin *s = viewer_skin(J.done_skin);
        char url[512] = "";
        if (s) thumb(url, sizeof url, s);
        sb_append(sb, "<div class=\"modal-overlay svelte-1ugt6n9\" role=\"dialog\" aria-modal=\"true\" aria-label=\"Craft result\" tabindex=\"-1\" onclick=\"v10jy.rok()\">"
                      "<div class=\"modal-content svelte-1ugt6n9\" role=\"document\" onclick=\"v10jy.stop()\">"
                      "<div style=\"text-align: center;\"><span class=\"material-icons\" style=\"font-size: 96px; color: #38cd3a; line-height: 110px; pointer-events: none;\">done</span></div>"
                      "<h3 class=\"svelte-1ugt6n9\">Crafted <span style=\"color: ");
        sb_append(sb, rarity_color(s));
        sb_append(sb, "\">");
        sb_append_esc(sb, s && s->name ? s->name : "");
        sb_appendf(sb, "</span></h3><img draggable=\"false\" src=\"%s\" alt=\"\" style=\"height: 120px; margin: 8px auto 12px; display: block; pointer-events: none;\">", url);
        sb_append(sb, "<button class=\"main-button svelte-whnh40\" id=\"v10jyDoneOk\" type=\"button\" onclick=\"v10jy.rok()\">OK</button></div></div>");
    } else if (J.err[0]) {
        sb_append(sb, "<div class=\"modal-overlay svelte-1ugt6n9\" role=\"dialog\" aria-modal=\"true\" aria-label=\"Error message\" tabindex=\"-1\" onclick=\"v10jy.errok()\">"
                      "<div class=\"modal-content svelte-1ugt6n9\" role=\"document\" onclick=\"v10jy.stop()\"><h3 class=\"svelte-1ugt6n9\">Error</h3><p>");
        sb_append_esc(sb, J.err);
        sb_append(sb, "</p><button class=\"main-button svelte-whnh40\" id=\"v10jyErrOk\" type=\"button\" onclick=\"v10jy.errok()\">OK</button></div></div>");
    }
}

void v10_junkyard_render(StrBuf *sb) {
    jy_init();
    char bg[256];
    html_dom_asset_url("img/backgrounds/junkyrd_0.png", bg, sizeof bg);
    sb_appendf(sb, "<div class=\"junkyard-tab svelte-1vqzycn\"><div class=\"jy-scroll svelte-1vqzycn\">"
                   "<div class=\"craft-header-img svelte-1vqzycn\" style=\"background-image: linear-gradient(rgb(10 10 10 / 37%%), #0000003b), url(%s);\">"
                   "<div class=\"craft-header-name svelte-1vqzycn\">Welcome to the Junkyard</div> <div class=\"craft-header-desc svelte-1vqzycn\">"
                   "Craft brand new skins &amp; items by using Junk and other Materials, which can be found scattered around every map in the game or earned from challenges.</div></div> ",
               bg);
    if (!acc()) {
        sb_append(sb, "<div style=\"color: #fff; text-align: center; margin-top: 20px; cursor: pointer;\" role=\"button\" tabindex=\"0\" onclick=\"showWindow(5)\">"
                      "<span style=\"color: #31caec\">Register</span> to craft!</div>");
    } else if (!J.rows) {
        sb_appendf(sb, "<div style=\"color: #fff; text-align: center; margin-top: 20px;\">%s</div>", J.failed ? "No Recipes Found" : "Loading Recipes...");
    } else {
        sb_append(sb, "<div class=\"jy-filter-bar svelte-1vqzycn\"><input type=\"search\" id=\"v10jySearch\" placeholder=\"Search items...\" class=\"jy-search-input svelte-1vqzycn\" value=\"");
        sb_append_esc(sb, J.search);
        sb_append(sb, "\" oninput=\"v10jy.search(this.value)\"> <select class=\"jy-sort-select svelte-1vqzycn\" id=\"v10jySort\" onchange=\"v10jy.sort(this.value)\">");
        for (int i = 0; i < SORT_COUNT; i++)
            sb_appendf(sb, "<option value=\"%s\"%s>%s</option>", k_sort_value[i], J.sort == i ? " selected" : "", k_sort_label[i]);
        sb_append(sb, "</select></div>");
        sb_append(sb, "<div class=\"craft-grid svelte-1vqzycn\" id=\"jyGrid\">");
        grid_html(sb);
        sb_append(sb, "</div>");
    }
    overlay_html(sb);
    sb_append(sb, "</div></div>");
}

// ------------------------------------------------------------------ the protocol

static void send_frame(cJSON *f) {
    backend_social_send(f);
    cJSON_Delete(f);
}

static void request_recipes(void) {
    cJSON *f = cJSON_CreateArray();
    cJSON_AddItemToArray(f, cJSON_CreateString("recps"));
    J.loading = 1;
    send_frame(f);
}

static void send_craft(const int sid) {
    cJSON *f = cJSON_CreateArray();
    cJSON_AddItemToArray(f, cJSON_CreateString("crft"));
    cJSON_AddItemToArray(f, cJSON_CreateNumber(sid));
    J.crafting = sid;
    send_frame(f);
}

static int sout_of(const int sid) {
    const cJSON *row;
    cJSON_ArrayForEach(row, J.rows) {
        if ((int) num(row, "SID", -1) == sid) return (int) num(row, "SOUT", -1);
    }
    return -1;
}

static const cJSON *frame_at(const cJSON *frame, const int i) { return cJSON_GetArrayItem(frame, i); }

static void on_social(void *user, const cJSON *frame) {
    (void) user;
    const char *label = cJSON_GetStringValue(cJSON_GetArrayItem(frame, 0));
    if (!label) return;
    if (!strcmp(label, "recps")) {
        const cJSON *rows = frame_at(frame, 1), *junk = frame_at(frame, 2);
        J.loading = 0;
        cJSON_Delete(J.rows);
        J.rows = cJSON_IsArray(rows) ? cJSON_Duplicate(rows, 1) : NULL;
        J.failed = J.rows == NULL;
        J.junk = cJSON_IsNumber(junk) ? junk->valuedouble : 0;
        v10_market_junkyard_refresh();
    } else if (!strcmp(label, "crftr")) {
        const cJSON *sid_j = frame_at(frame, 1), *ok_j = frame_at(frame, 2), *msg_j = frame_at(frame, 3);
        const int crafted = cJSON_IsNumber(sid_j) ? sid_j->valueint : -1;
        if (crafted == J.crafting) J.crafting = -1;
        if (cJSON_IsNumber(ok_j) && ok_j->valueint == 1) {
            J.done_sid = crafted;
            J.done_skin = sout_of(crafted);
            request_recipes(); /* the counts and the junk after the craft */
        } else {
            const char *msg = cJSON_GetStringValue(msg_j);
            snprintf(J.err, sizeof J.err, "%s", msg && msg[0] ? msg : "Error");
        }
        v10_market_junkyard_refresh();
    } else if (!strcmp(label, "ua")) {
        v10_market_junkyard_refresh(); /* the owned materials and the result changed */
    }
}

// ------------------------------------------------------------------ entry and handlers

void v10_junkyard_enter(void) {
    jy_init();
    if (!J.listening) J.listening = backend_social_listen(on_social, NULL);
    J.confirm = -1;
    J.done_sid = -1;
    J.err[0] = 0;
    cJSON_Delete(J.rows);
    J.rows = NULL;
    J.failed = 0;
    if (acc()) request_recipes();
}

static const char *arg_text(const HtmlEvent *ev, const HtmlArg *args, const int argc, const int i) {
    if (!args || i >= argc) return "";
    if (args[i].kind == HTML_ARG_THIS_VALUE) return ev && ev->value ? ev->value : "";
    return args[i].str ? args[i].str : "";
}

static int arg_int(const HtmlArg *args, const int argc, const int i, const int def) {
    if (!args || i >= argc) return def;
    if (args[i].kind == HTML_ARG_NUM) return (int) args[i].num;
    return args[i].str ? atoi(args[i].str) : def;
}

int v10_junkyard_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    if (!name || strncmp(name, "v10jy.", 6) != 0) return -1;
    jy_init();
    const char *h = name + 6;
    if (!strcmp(h, "stop")) {
        html_ui_stop_propagation();
    } else if (!strcmp(h, "search")) {
        snprintf(J.search, sizeof J.search, "%s", arg_text(ev, args, argc, 0));
        refresh_grid();
    } else if (!strcmp(h, "sort")) {
        const char *v = arg_text(ev, args, argc, 0);
        for (int i = 0; i < SORT_COUNT; i++)
            if (!strcmp(v, k_sort_value[i])) J.sort = i;
        refresh_grid();
    } else if (!strcmp(h, "craft")) {
        // v10's onCraftClick: the confirmation first (nothing while a craft is out)
        if (acc() && J.crafting < 0) {
            J.confirm = arg_int(args, argc, 0, -1);
            v10_market_junkyard_refresh();
        }
    } else if (!strcmp(h, "cgo")) {
        if (J.confirm >= 0 && J.crafting < 0 && acc()) {
            const int sid = J.confirm;
            J.confirm = -1;
            send_craft(sid);
            v10_market_junkyard_refresh();
        }
    } else if (!strcmp(h, "cclose")) {
        J.confirm = -1;
        v10_market_junkyard_refresh();
    } else if (!strcmp(h, "rok")) {
        J.done_sid = -1;
        v10_market_junkyard_refresh();
    } else if (!strcmp(h, "errok")) {
        J.err[0] = 0;
        v10_market_junkyard_refresh();
    } else if (!strcmp(h, "item")) {
        // the skin's item page: the exchange's Market tab (v10_market_open(TAB_MARKET, skin))
        v10_market_open(0, arg_int(args, argc, 0, -1));
    }
    return 1;
}
