// v10's Junkyard tab, ported from its Svelte components (svelte-1vqzycn: the page, the filter bar,
// the grid; svelte-ggnigm: the recipe card; v10.0.7: deob-10.0.7.js:137930-138260) and windows[13]'s
// craft functions (craftDatRes / craftRes / startCraft / sendCraft, deob-10.0.7.js:220747-220790).
// The order and the states follow v10 (see the comments on each step):
//   - the recipes: ["recps"] once the tab opens (CRAFT_DATA_REQUEST), answered ["recps", rows(, junk)];
//   - a card's Craft: sendCraft's confirmation (genericPopup "confPremiumPop": Junk, Materials, Craft time),
//     its Craft is startCraft (the craft_0 sound, "Please Wait...", ["crft", SID]), answered
//     ["crftr", SID, ok, msg]: ok closes the popups (clearPops), a refusal is v10's response popup;
//   - a card's or a material's click: the item view (svelte-2tp35a, More info / Share).
// The transport: a Box Office account's frames are kn_brain's social socket (services/brain/craft.c, whose
// answer carries the junk balance as its third slot); a krunker.io account's are the official game socket's
// (krunker_net_send_account_frame: v10 sends both there), its junk the account's (acc.junk, "uf").
//
// Deviations: kn_brain grants the skin at the request (craft.c: no crafting timer), so a Box Office craft
// never shows v10's "Crafting..." / "<t> remaining" / "Complete" states (STATORD / EXP), and the recipes are asked
// again after it (their CNT and the junk balance changed; v10 keeps its first answer). A krunker.io craft's
// answer sets v10's STATORD 1 / EXP (the card's "<t> remaining"). A Box Office tab asks for the recipes each time
// it opens (the brain's junk balance travels with them); krunker.io's are asked once, as v10 caches craftData.
#include <audio.h>
#include <backend.h>
#include <backend_ui.h>
#include <economy_client.h> // econ_black_market_find: the item view's Black Market badge
#include <krunker_net.h>    // krunker_net_send_account_frame: a krunker.io account's frames
#include <html_dom.h>
#include <i18n.h>
#include <ui_logic.h>
#include <v10_ui.h> // v10_popup_show (genericPopup), v10_view_push (the item view)
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
    int status;                 // cftSts: STATORD (0 ready, 1 crafting, 2 complete)
    int has_exp;                // crftTim: EXP is set (the "<t> remaining" line, else "Crafting...")
    double exp;
    int n;
    int mat[MATS_MAX], mat_n[MATS_MAX];
    int is_new, affordable, mats_ok;
} JyItem;

static struct {
    int inited, listening;
    cJSON *rows;                // the recps rows (NULL until answered, or failed)
    int loading, failed;
    int timer;                  // the deadline of the recps request in flight (html_timer id, 0 none)
    int timed_out;              // failed because no answer came by the deadline (not a logged-out null)
    double junk;                // the junk balance the recps answer carried (Box Office: craft.c's third slot)
    int junk_known;             // ... it carried one (krunker.io's has none: the account's acc.junk then)
    char search[SEARCH_MAX];
    int sort;
    int share_timer;            // the item view's Share "Copied!" (1.5 s)
} J;

static JyItem s_items[JY_MAX];  // the recipes as built (build_items)
static JyItem s_list[JY_MAX];   // the grid's list (visible_items)

static void jy_init(void) {
    if (J.inited) return;
    J.inited = 1;
}

static const AccountInfo *acc(void) { return active_account(); }
static int kio(void) { return account_backend() == ACCOUNT_BACKEND_KRUNKER; }

// v10's `account.junk` (cantCraft, the order): the recps answer's balance on Box Office, the account's on krunker.io
static double junk_balance(void) {
    if (J.junk_known) return J.junk;
    const AccountInfo *a = acc();
    return a ? a->junk : 0;
}

static double num(const cJSON *o, const char *k, const double def) {
    const cJSON *v = o ? cJSON_GetObjectItemCaseSensitive(o, k) : NULL;
    if (cJSON_IsNumber(v)) return v->valuedouble;
    if (cJSON_IsString(v) && v->valuestring[0]) return atof(v->valuestring); // parseFloat(JUNK): krunker.io's may be text
    return def;
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

// iííìïïï(ms) with no other argument (deob-10.0.7.js:23957): years, days, hours when non-zero, then the minutes
// always ("0m "), no seconds: sendCraft's "Craft time" (1000 * DUR) and a crafting card's "<t> remaining" (EXP)
static void duration(char *out, const size_t n, const double ms) {
    long long s = ms > 0 ? (long long) floor(ms / 1000.0) : 0;
    long long m = s / 60, h = m / 60, d = h / 24;
    const long long y = d / 365;
    m %= 60;
    h %= 24;
    d %= 365;
    size_t o = 0;
    out[0] = 0;
    if (y && o < n) o += (size_t) snprintf(out + o, n - o, "%lldy ", y);
    if (d && o < n) o += (size_t) snprintf(out + o, n - o, "%lldd ", d);
    if (h && o < n) o += (size_t) snprintf(out + o, n - o, "%lldh ", h);
    if (o < n) snprintf(out + o, n - o, "%lldm ", m);
}

// ------------------------------------------------------------------ the recipes

// "skin,count,skin,count" -> the pairs (up to MATS_MAX; kn_brain has checked every row)
static void parse_mats(JyItem *it, const char *sin) {
    char buf[160], *save = NULL;
    snprintf(buf, sizeof buf, "%s", sin ? sin : "");
    for (char *tok = strtok_r(buf, ",", &save); tok && it->n < MATS_MAX; tok = strtok_r(NULL, ",", &save)) {
        char *cnt = strtok_r(NULL, ",", &save);
        it->mat[it->n] = atoi(tok);
        it->mat_n[it->n] = cnt ? atoi(cnt) : 0; // parseInt(count || 0)
        it->n++;
        if (!cnt) break;
    }
}

static cJSON *row_of(const int sid) {
    cJSON *row;
    cJSON_ArrayForEach(row, J.rows) {
        if ((int) num(row, "SID", -1) == sid) return row;
    }
    return NULL;
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
        it->cnt = (int) num(row, "CNT", 0); // parseInt(CNT || 0)
        it->junk = num(row, "JUNK", 0);
        it->dat = num(row, "DAT", 0);
        it->status = (int) num(row, "STATORD", 0);
        const cJSON *exp = cJSON_GetObjectItemCaseSensitive(row, "EXP");
        it->has_exp = cJSON_IsNumber(exp);
        it->exp = it->has_exp ? exp->valuedouble : 0;
        parse_mats(it, cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(row, "SIN")));
        it->mats_ok = 1;
        for (int i = 0; i < it->n; i++)
            if (it->mat[i] && owned(it->mat[i]) < it->mat_n[i]) it->mats_ok = 0;
        // v10's order (deob-10.0.7.js:137950-137973): the junk price; -20000 while crafting or complete (STATORD > 0);
        // else +10000 when NEW, then +15000 when it can be crafted now, -15000 when not
        it->is_new = fabs(now - it->dat) < NEW_MS;
        it->affordable = logged && junk_balance() >= it->junk && it->mats_ok;
        if (it->status > 0) it->order = it->junk - 20000.0;
        else it->order = it->junk + (it->is_new ? 10000.0 : 0.0) + (it->affordable ? 15000.0 : -15000.0);
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

// the select's sorts (deob-10.0.7.js:137983-137994): rarity high to low, name A to Z, junk price low to high
static int less_default(const JyItem *a, const JyItem *b) { return order_or_m1(a) > order_or_m1(b); }
static int less_rarity(const JyItem *a, const JyItem *b) { return a->skin->rarity > b->skin->rarity; }
static int less_junk(const JyItem *a, const JyItem *b) { return a->junk < b->junk; }

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
    char needle[SEARCH_MAX];
    size_t a = 0;
    while (J.search[a] == ' ') a++;
    snprintf(needle, sizeof needle, "%s", J.search + a);
    for (size_t e = strlen(needle); e > 0 && needle[e - 1] == ' '; e--) needle[e - 1] = 0;
    JyItem *v = s_list;
    for (int i = 0; i < n; i++) v[i] = s_items[i];
    sort_items(v, n, less_default);
    int m = 0;
    for (int i = 0; i < n; i++)
        if (ci_contains(v[i].skin->name ? v[i].skin->name : "", needle)) v[m++] = v[i];
    if (J.sort == SORT_RARITY) sort_items(v, m, less_rarity);
    else if (J.sort == SORT_NAME) sort_items(v, m, less_name);
    else if (J.sort == SORT_JUNK) sort_items(v, m, less_junk);
    return m;
}

// v10's cantCraft: the junk first, Sold out over it, Missing materials only when nothing else
static const char *cant_reason(const JyItem *it) {
    const char *r = NULL;
    if (!acc() || junk_balance() < it->junk) r = "Not enough Junk";
    if (it->cnt == 0) r = "Sold out";
    for (int i = 0; i < it->n && !r; i++) {
        const ViewerSkin *m = it->mat[i] ? viewer_skin(it->mat[i]) : NULL;
        if (m && owned(it->mat[i]) < it->mat_n[i]) r = "Missing materials"; // a material the store lacks is skipped
    }
    return r;
}

// ------------------------------------------------------------------ markup

static void mat_thumb_block(StrBuf *sb, const JyItem *it, const int i) {
    const ViewerSkin *m = it->mat[i] ? viewer_skin(it->mat[i]) : NULL;
    if (!m) return;
    const int have = owned(it->mat[i]) >= it->mat_n[i];
    char url[512];
    thumb(url, sizeof url, m);
    sb_appendf(sb, "<div class=\"craft-item-req svelte-ggnigm%s\" role=\"button\" tabindex=\"0\" style=\"border: 2px solid %s; background-image: url(%s);\" onclick=\"v10jy.item(%d)\">"
                   "<div class=\"craft-req-count svelte-ggnigm\">x%d</div></div>",
               have ? "" : " missing", have ? rarity_color(m) : "#5e5e5e", url, it->mat[i], it->mat_n[i]);
}

// the recipe card (svelte-ggnigm, deob-10.0.7.js:133701 and its states :138083-138195)
static void card(StrBuf *sb, const JyItem *it) {
    const ViewerSkin *s = it->skin;
    const char *col = rarity_color(s);
    char url[512];
    thumb(url, sizeof url, s);
    sb_append(sb, "<div class=\"craft-recipe-card svelte-ggnigm\">");
    sb_appendf(sb, "<div class=\"craft-icon svelte-ggnigm\" role=\"button\" tabindex=\"0\" style=\"border: 3px solid %s;\" onclick=\"v10jy.item(%d)\">", col, it->sout);
    sb_appendf(sb, "<img style=\"height: 100%%\" class=\"svelte-ggnigm\" src=\"%s\" alt=\"", url);
    sb_append_esc(sb, s->name ? s->name : "");
    sb_append(sb, "\"> <div class=\"craft-item-name svelte-ggnigm\" style=\"color: ");
    sb_append(sb, col);
    sb_append(sb, "\">");
    sb_append_esc(sb, s->name ? s->name : "");
    sb_appendf(sb, "</div> <div class=\"craft-item-junk svelte-ggnigm\">%.2f <span class=\"material-icons svelte-ggnigm\" style=\"color: #cc51f8; font-size: 34px; margin-left: 5px; vertical-align: top;\">plumbing</span></div>",
               it->junk);
    if (it->is_new && it->status == 0) sb_append(sb, "<div class=\"craft-new-tag svelte-ggnigm\">NEW</div>");
    if (it->cnt > 0) sb_appendf(sb, "<div class=\"craft-limit-amount svelte-ggnigm\" title=\"%d left in the world\">x%d</div>", it->cnt, it->cnt);
    if (it->status == 1) sb_append(sb, "<div class=\"craft-hammer-animation svelte-ggnigm\"><span class=\"material-icons craft-hammer-icon svelte-ggnigm\">plumbing</span></div>");
    if (it->status == 2)
        sb_append(sb, "<div class=\"done-craft-icon svelte-ggnigm\"><span class=\"material-icons svelte-ggnigm\" style=\"font-size: 160px; color: #38cd3a; line-height: 175px;\">done</span></div>");
    sb_append(sb, "</div> <div class=\"craft-requirements-list svelte-ggnigm\">");
    for (int i = 0; i < it->n; i++) mat_thumb_block(sb, it, i);
    sb_append(sb, "</div> ");
    const char *reason = cant_reason(it);
    if (it->status == 1) {
        char t[64] = "Crafting...";
        if (it->has_exp) {
            duration(t, sizeof t, it->exp);
            strncat(t, " remaining", sizeof t - strlen(t) - 1);
        }
        sb_appendf(sb, "<div class=\"craft-timer svelte-ggnigm\">%s</div>", t);
    } else if (it->status == 2) {
        sb_append(sb, "<div class=\"craft-timer svelte-ggnigm\">Complete</div>");
    } else if (reason) {
        sb_append(sb, "<div class=\"craft-timer svelte-ggnigm\">");
        sb_append_esc(sb, reason);
        sb_append(sb, "</div>");
    } else {
        sb_appendf(sb, "<div class=\"craft-button svelte-ggnigm\" id=\"v10jyCraft%d\" role=\"button\" tabindex=\"0\" onmouseenter=\"playTick(0.1)\" onclick=\"v10jy.craft(%d)\">Craft</div>",
                   it->sid, it->sid);
    }
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
    if (!e || !acc() || !J.rows) return; // the grid holds the Register / status line then, not cards
    StrBuf sb = {0};
    grid_html(&sb);
    char *html = sb_finish(&sb);
    html_dom_set_html(e, html);
    free(html);
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
    // v10 (deob-10.0.7.js:138020-138054): the filter bar fills the anchor between the header and .craft-grid
    // (only with recipes); the guest's Register line, the Loading status and the cards all sit
    // inside .craft-grid, a wrapping flex row, so those one-line divs shrink to their text at the grid's left
    // (their text-align: center has no width to centre in) below its 5px padding.
    if (acc() && J.rows) {
        sb_append(sb, "<div class=\"jy-filter-bar svelte-1vqzycn\"><input type=\"search\" id=\"v10jySearch\" placeholder=\"Search items...\" class=\"jy-search-input svelte-1vqzycn\" value=\"");
        sb_append_esc(sb, J.search);
        sb_append(sb, "\" oninput=\"v10jy.search(this.value)\"> <select class=\"jy-sort-select svelte-1vqzycn\" id=\"v10jySort\" onchange=\"v10jy.sort(this.value)\">");
        for (int i = 0; i < SORT_COUNT; i++)
            sb_appendf(sb, "<option value=\"%s\"%s>%s</option>", k_sort_value[i], J.sort == i ? " selected" : "", k_sort_label[i]);
        sb_append(sb, "</select></div>");
    }
    sb_append(sb, " <div class=\"craft-grid svelte-1vqzycn\" id=\"jyGrid\">");
    if (!acc()) {
        sb_append(sb, "<div style=\"color: #fff; text-align: center; margin-top: 20px; cursor: pointer;\" role=\"button\" tabindex=\"0\" onclick=\"showWindow(5)\">"
                      "<span style=\"color: #31caec\">Register</span> to craft!</div>");
    } else if (!J.rows) {
        // v10 shows "Loading Recipes..." until rows come: its "No Recipes Found" reads craftData.error, which craftDatRes
        // never sets (it sets `err`, deob-10.0.7.js:220749 vs :138042), so a null answer reads Loading too. A request
        // that got no answer at all (the socket dropped, the brain is down) is failed after its deadline here: the
        // screen says so rather than "Loading Recipes..." for good (KrunkNative's)
        const char *text = J.timed_out ? I18N("kn.jy.loadFailed", "Couldn't load the recipes. Open the Junkyard again to retry.") : "Loading Recipes...";
        sb_appendf(sb, "<div id=\"v10jyStatus\" style=\"color: #fff; text-align: center; margin-top: 20px;\">%s</div>", text);
    } else {
        grid_html(sb);
    }
    sb_append(sb, "</div></div></div>");
}

// ------------------------------------------------------------------ the item view (iîiíìîi, svelte-2tp35a)

static const char *item_type(const ViewerSkin *vs) {
    static const char *const types[] = {NULL, "Hat Item", "Body Item", "Melee Item", "Spray Item", "Dye Item", "Waist Item", "Face Item", "Shoe Item", "Pet Item",
                                        "Collectible Item", "Wrist Item", "Charm Item", "Spin Ticket Item", "Back Item", "Head Item", "Player Card"};
    if (vs && vs->type > 0 && vs->type < (int) (sizeof(types) / sizeof(types[0]))) return types[vs->type];
    if (vs && vs->type == 0 && vs->weapon > 0) return "Weapon Skin";
    return "";
}

// IÎïïiîí (deob-10.0.7.js:137995-138011): a card's or a material's click opens the item view of that store skin
// ({...skin, index, rarity_meta}): the image, the name, the type, the status badge (deob-10.0.7.js:106727-106746:
// Black Market, Vaulted, Limited), More info (openSales) and Share (the ?item= link)
static void item_view(const int index) {
    const ViewerSkin *s = viewer_skin(index);
    if (!s) return;
    const char *col = rarity_color(s);
    const int animate = s->rarity == 6, dark = s->rarity == 5; // rarity_meta.animate (Unobtainable) / .dark (Contraband)
    char img[512];
    store_skin_preview(index, img, sizeof img);
    StrBuf sb = {0};
    sb_appendf(&sb, "<div class=\"popup-container svelte-1wz7ymp\"><div class=\"popup svelte-1wz7ymp translucent slim-border\" style=\"width: 600px;\">"
                    "<div class=\"item-showcase-container io-popup-container svelte-2tp35a\"><div class=\"item-showcase-content io-popup-content svelte-2tp35a\">"
                    "<div class=\"image-display svelte-2tp35a%s\" style=\"border: 3px solid %s; background-image: url(%s);\"></div> "
                    "<div class=\"showcase-overlay svelte-2tp35a\"><!----> <div class=\"showcase-header svelte-2tp35a\"><!----> <div class=\"item-showcase-header svelte-2tp35a\">"
                    "<div class=\"item-info svelte-2tp35a\"><!----> <div class=\"item-name svelte-2tp35a%s%s\" style=\"color: %s;\">",
               animate ? " rainbow" : "", col, img, animate ? " rainbow" : "", dark ? " dark" : "", col);
    sb_append_esc(&sb, s->name && s->name[0] ? s->name : "Unnamed Item");
    sb_appendf(&sb, "</div> <!----> <div class=\"item-description svelte-2tp35a\">%s</div> ", item_type(s));
    const int seas = s->seas ? s->seas : 1;
    if (econ_black_market_find(index)) sb_append(&sb, "<div class=\"item-status-badge svelte-2tp35a\" style=\"background-color: #00ffff;\">Black Market</div>");
    else if (s->type != 18 && seas != g_viewer_current_season) sb_append(&sb, "<div class=\"item-status-badge svelte-2tp35a\" style=\"background-color: #666666;\">Vaulted</div>");
    else if (s->flags & VS_BLOCKED) sb_append(&sb, "<div class=\"item-status-badge svelte-2tp35a\" style=\"background-color: #faa332;\">Limited</div>");
    else sb_append(&sb, "<!---->");
    sb_appendf(&sb, "</div> <div class=\"item-showcase-btns svelte-2tp35a\"><button class=\"more-info-btn svelte-2tp35a\" onclick=\"openSales(%d)\">More info</button> "
                    "<button class=\"share-btn svelte-1524ito\" type=\"button\" title=\"Copy link\" id=\"v10jyShareBtn\" onclick=\"v10jy.share(%d)\"><i class=\"material-icons svelte-1524ito\">share</i> "
                    "<span class=\"svelte-1524ito\">Share</span></button></div></div></div> <!----></div></div></div></div></div>",
               index, index);
    v10_view_push(sb_finish(&sb), NULL, NULL, NULL);
}

// ShareButton's copied state (title, icon, label, .share-copied for 1.5 s)
static void share_copied_set(const int copied) {
    const HtmlElem btn = html_dom_by_id("v10jyShareBtn");
    if (!btn) return;
    html_dom_set_attr(btn, "title", copied ? I18N("generic.copied", "Copied!") : "Copy link");
    html_dom_set_class(btn, "share-copied", copied);
    const HtmlElem icon = html_dom_query(btn, "i"), label = html_dom_query(btn, "span");
    if (icon) html_dom_set_text(icon, copied ? "check" : "share");
    if (label) html_dom_set_text(label, copied ? I18N("generic.copied", "Copied!") : "Share");
}
static void share_copied_done(void *user) {
    (void) user;
    J.share_timer = 0;
    share_copied_set(0);
}

// ------------------------------------------------------------------ the protocol

// A Box Office account's frames go to kn_brain's social socket, a krunker.io account's to the official game socket
// (v10 sends CRAFT_DATA_REQUEST / CRAFT_REQUEST there: io.send)
static int send_frame(cJSON *f) {
    const int sent = kio() ? krunker_net_send_account_frame(f) : backend_social_send(f);
    cJSON_Delete(f);
    return sent;
}

// The recipes' deadline: a recps frame whose answer never comes (the socket dropped with it in the queue,
// the brain went away) fails the request, so the tab shows its error and entering the tab asks again.
#define RECPS_WAIT_MS 15000
static void recps_expired(void *user) {
    (void) user;
    J.timer = 0;
    if (!J.loading) return;
    J.loading = 0;
    J.failed = 1;
    J.timed_out = 1;
    v10_market_junkyard_refresh();
}

static void request_recipes(void) {
    cJSON *f = cJSON_CreateArray();
    cJSON_AddItemToArray(f, cJSON_CreateString("recps"));
    html_timer_cancel(J.timer);
    J.timer = html_timer_add(RECPS_WAIT_MS, recps_expired, NULL);
    J.loading = 1;
    J.timed_out = 0;
    if (!send_frame(f)) { // not sent: no answer can come
        html_timer_cancel(J.timer);
        J.timer = 0;
        J.loading = 0;
        // krunker.io's game server takes recps only once the player is in the game (krunker_net.c): asked again by
        // the tab's next render, "Loading Recipes..." meanwhile; no Box Office socket at all is a failure
        J.failed = !kio();
    }
}

// windows[13].sendCraft(SID) (deob-10.0.7.js:220779-220789): genericPopup(<the confirmation>, "confPremiumPop"):
// "Craft <name>", the Junk it costs, the materials (rarity-coloured, not by ownership as on the card), the craft
// time, then #confirmBtn (startCraft) / #declineBtn (clearPops). v10's markup, its unclosed url( included.
static void send_craft_popup(const int sid) {
    const cJSON *row = row_of(sid);
    const ViewerSkin *out = row ? viewer_skin((int) num(row, "SOUT", -1)) : NULL;
    if (!out) return;
    JyItem it;
    memset(&it, 0, sizeof it);
    parse_mats(&it, cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(row, "SIN")));
    StrBuf mats = {0};
    sb_append(&mats, "");
    for (int i = 0; i < it.n; i++) {
        const ViewerSkin *m = it.mat[i] ? viewer_skin(it.mat[i]) : NULL;
        if (!m) continue;
        char url[512];
        thumb(url, sizeof url, m);
        sb_appendf(&mats, "<div class='crftRecSml' style='%sborder: 2px solid %s;background-image:url(%s'></div>", i ? "" : "margin-left:-5px;", rarity_color(m), url);
    }
    char *mats_html = sb_finish(&mats);
    char t[64];
    duration(t, sizeof t, 1000.0 * num(row, "DUR", 0));
    StrBuf sb = {0};
    sb_appendf(&sb, "<div class='pubHed'>Craft <span style='color:%s'>", rarity_color(out));
    sb_append_esc(&sb, out->name ? out->name : "");
    sb_appendf(&sb, "</span></div><div style='color:#fff;font-size:20px;position:relative;margin-bottom:15px;'>"
                    "<div style='color:#979797;'>Junk<span style='float:right;color:#e5e5e5;'>-%.2f<span class='material-icons' style='color:#cc51f8;font-size:30px;vertical-align:middle;margin-top:-7px;margin-left:5px;margin-right:-5px;'>plumbing</span></span></div>"
                    "<div style='color:#979797;'>Materials<div style='float:right;'>-%s</div></div>"
                    "<div style='color:#979797;'>Craft time<span style='float:right;color:#e5e5e5;'>%s</span></div></div>"
                    "<div id='confirmBtn' onclick='logBtnFRVR(`craft_item_confirm`);windows[13].startCraft(%d)' onmouseover='playTick(0.1)'>Craft</div><div id='declineBtn' onclick='clearPops()' onmouseover='playTick(0.1)'>Cancel</div>",
               num(row, "JUNK", 0), mats_html ? mats_html : "", t, sid);
    free(mats_html);
    char *html = sb_finish(&sb);
    v10_popup_show("confPremiumPop", html, NULL);
    free(html);
}

// windows[13].startCraft(SID) (deob-10.0.7.js:220774-220778): the craft sound, "Please Wait...", CRAFT_REQUEST
static void start_craft(const int sid) {
    audio_play("craft_0", 0.4f, 0, 1.0f);
    ui_loading_popup("Please Wait...");
    cJSON *f = cJSON_CreateArray();
    cJSON_AddItemToArray(f, cJSON_CreateString("crft"));
    cJSON_AddItemToArray(f, cJSON_CreateNumber(sid));
    if (!send_frame(f)) ui_loadout_response_popup("Error"); // nothing went out: no answer would close the wait
}

static int truthy(const cJSON *v) {
    return (cJSON_IsNumber(v) && v->valuedouble != 0) || cJSON_IsTrue(v) || (cJSON_IsString(v) && v->valuestring[0]);
}

// The recps / crftr answers (Box Office: the social socket's; krunker.io: the game socket's, v10_junkyard_kio_frame)
static void on_frame(const cJSON *frame) {
    const char *label = cJSON_GetStringValue(cJSON_GetArrayItem(frame, 0));
    if (!label) return;
    if (!strcmp(label, "recps")) { // craftDatRes(rows): {dat: rows, err: !rows}
        const cJSON *rows = cJSON_GetArrayItem(frame, 1), *junk = cJSON_GetArrayItem(frame, 2);
        html_timer_cancel(J.timer);
        J.timer = 0;
        J.loading = 0;
        cJSON_Delete(J.rows);
        J.rows = cJSON_IsArray(rows) ? cJSON_Duplicate(rows, 1) : NULL;
        J.failed = J.rows == NULL;
        J.junk_known = cJSON_IsNumber(junk);
        J.junk = J.junk_known ? junk->valuedouble : 0;
        v10_market_junkyard_refresh();
    } else if (!strcmp(label, "crftr")) { // craftRes(SID, ok, msg) (deob-10.0.7.js:220755-220772)
        const cJSON *sid_j = cJSON_GetArrayItem(frame, 1), *ok_j = cJSON_GetArrayItem(frame, 2), *msg_j = cJSON_GetArrayItem(frame, 3);
        if (truthy(ok_j)) {
            // the recipe's EXP = 1000 * DUR and STATORD = 1 (the card reads "<t> remaining"), clearPops, updateWindow(14)
            cJSON *row = kio() && cJSON_IsNumber(sid_j) ? row_of(sid_j->valueint) : NULL;
            if (row) {
                const double dur = num(row, "DUR", 0);
                cJSON_DeleteItemFromObjectCaseSensitive(row, "EXP");
                cJSON_DeleteItemFromObjectCaseSensitive(row, "STATORD");
                cJSON_AddNumberToObject(row, "EXP", 1000.0 * dur);
                cJSON_AddNumberToObject(row, "STATORD", 1);
            }
            ui_loadout_clear_pops(); // the confirmation / "Please Wait..." had replaced the exchange: it is closed now
            menu_rerender_if_open(14); // updateWindow(14): the store's junk balance
            if (!kio()) request_recipes(); // Box Office: granted at once, so the counts and the junk balance changed
            v10_market_junkyard_refresh();
        } else {
            const char *msg = cJSON_GetStringValue(msg_j);
            ui_loadout_response_popup(msg && msg[0] ? msg : "Error"); // showPopup(null, msg || "Error")
        }
    } else if (!strcmp(label, "ua") || !strcmp(label, "uf")) {
        v10_market_junkyard_refresh(); /* the owned materials, the junk */
    }
}

static void on_social(void *user, const cJSON *frame) {
    (void) user;
    if (!kio()) on_frame(frame);
}

void v10_junkyard_kio_frame(const cJSON *frame) {
    if (kio()) on_frame(frame);
}

// ------------------------------------------------------------------ entry and handlers

// The tab opened. v10 asks once: `!craftData && account && io` sends CRAFT_DATA_REQUEST (deob-10.0.7.js:137938) and
// keeps the answer (krunker.io's here). A Box Office tab asks each time it opens (the brain's junk balance travels with
// the recipes).
void v10_junkyard_enter(void) {
    jy_init();
    if (!J.listening) J.listening = backend_social_listen(on_social, NULL);
    if (kio()) {
        if (acc() && !J.rows && !J.loading) request_recipes();
        return;
    }
    cJSON_Delete(J.rows);
    J.rows = NULL;
    J.failed = 0;
    if (acc()) request_recipes();
}

// The account changed (a login, a logout, the session restored after the tab opened, the backend switched): the
// recipes are the account's (counts, junk), so they are asked for again by the next render of a logged-in tab.
void v10_junkyard_account_changed(void) {
    jy_init();
    html_timer_cancel(J.timer);
    J.timer = 0;
    J.loading = 0;
    J.failed = 0;
    J.timed_out = 0;
    cJSON_Delete(J.rows);
    J.rows = NULL;
    J.junk = 0;
    J.junk_known = 0;
}

// The tab's render asks for the recipes when they are missing: a logged-in tab whose recipes were never asked
// for (it was opened as a guest, or the session came after it) has nothing else that would ask
void v10_junkyard_ensure(void) {
    jy_init();
    if (acc() && !J.rows && !J.loading && !J.failed) request_recipes();
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
    if (!name) return -1;
    // v10's own names (windows[13]): the confirmation replaced the exchange in #genericPop (genericPopup), so these
    // run with the exchange closed, as the item view's buttons do
    if (!strcmp(name, "windows.13.startCraft")) {
        jy_init();
        if (acc()) start_craft(arg_int(args, argc, 0, -1));
        return 1;
    }
    if (!strcmp(name, "windows.13.sendCraft")) {
        jy_init();
        if (acc()) send_craft_popup(arg_int(args, argc, 0, -1));
        return 1;
    }
    if (strncmp(name, "v10jy.", 6) != 0) return -1;
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
        // the card's onCraftClick: playTick + playSelect, logBtnFRVR("craft_item"), windows[13].sendCraft(SID)
        audio_play("tick_0", 0.1f, 0, 1.0f);
        audio_play("select_0", 0.1f, 0, 1.0f);
        if (acc()) send_craft_popup(arg_int(args, argc, 0, -1));
    } else if (!strcmp(h, "item")) {
        item_view(arg_int(args, argc, 0, -1));
    } else if (!strcmp(h, "share")) { // ShareButton (?item=<index>), "Copied!" for 1.5 s
        html_ui_stop_propagation();
        char origin[256], url[320];
        html_page_origin(origin, sizeof origin);
        snprintf(url, sizeof url, "%s/?item=%d", origin, arg_int(args, argc, 0, -1));
        html_clipboard_write(url);
        if (J.share_timer) html_timer_cancel(J.share_timer);
        share_copied_set(1);
        J.share_timer = html_timer_add(1500, share_copied_done, NULL);
    }
    return 1;
}
