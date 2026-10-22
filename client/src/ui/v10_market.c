// Krunker v10.0.2 "Market & Trading" (doc/v10/ui.md section 4): the exchange popup v10 opens with
// window.openExchange(tab, itemIndex) from the main menu's #menuBtnMarket (genericPopup({component:
// _46780}, 'exchangeModal'), v10.js 132601-136984), ported from its Svelte components to C over
// html_dom.h. The markup is v10's templates verbatim (class names with their svelte-* hashes, so
// ui/css/svelte.css styles them), rebuilt into #genericPop as the state changes:
//
//   sidebar   Market / Trading / Black Mkt / Junkyard / Catalog / Guide (_44583, svelte-berov0)
//   Market    Browse / My Inventory / My Sales / History / Stats (_46437, svelte-1ugt6n9), the filter
//             header (_45793), the market grid and cards (_45681, _45592, _36396), Listing Details
//             (svelte-olnzsm) with its Listings / Stats (sales charts) / Owners tabs, the Purchase /
//             List / Unlist / Error modals, Quick Sell (_46234) and the notifications (_45504)
//   Trading   incoming / outgoing / history offers (_46536), Accept / Decline / Cancel / Decline All,
//             Start a Trade (player search) and the Create Trade window (_36706, _36585, _36471)
//   Black Mkt the Black Market cards and purchase (svelte-dbcods)
//   Junkyard  the crafting page (svelte-1vqzycn)
//   Catalog   every store item (_46722) and its detail page (_45008: Info, Listings, Sales, Units
//             Sold, Owners)
//   Guide     the trading guide (svelte-4log2q, v10_market_guide.h)
//
// Transport: v10 talks to its REST API (api.krunker.io /market, /trades, /items); here every call is
// the matching kn_brain social-socket frame of the economy (doc/v7/economy.md section 4, the frames
// built by economy_client.c, which the v7 hub uses too). Mapping, all KrunkNative deviations from
// v10 (doc/v10/ui.md "Market & Trading"):
//   GET /market/listings          ["fil"] (filters, price order; "Latest" sorted here by listing time)
//                                 / ["r","market","market",..,item] (one item's listings)
//   GET /market/inventory|sales|history|stats   ["r","market",<sub>]
//   POST /market/list|purchase|unlist|quick-sell   ["li"] / ["pi"] / ["uli"] / ["qsi"] with {op} keys
//   /market/items/<i>/sales-statistics, /items/<i>/sales   ["st", item, days | range]
//   /items/<i>/view (stats, owners)                     ["r","itemsales",..,item]
//   /trades/history, create, accept, cancel, decline-all ["r","market","trades"] / ["ctr"] / ["trA"]
//                                                         / ["trC"] / ["trCA"]
//   /trades/player-search, /trades/inventory/<id>         ["sp"] / ["ri"]
//   /market/black-market(/purchase)                       the store table / ["pbm"]
// What does not exist here: Junk and crafting (the Junkyard has no recipes), item instances (wear,
// mints, serials, paint, styles, name tags: every item is a stack) and the 3D showcase. The estimates of
// /webhooks/general/items/prices are ["r","market","prices"]: this economy's own mean sale prices,
// then the bundled data/prices.js snapshot (social_estimate_value); with neither, v10's "~ Unknown".
#include <backend.h>
#include <i18n.h>
#include <backend_ui.h>
#include <economy_client.h>
#include <html_dom.h>
#include <html_ui.h>
#include <ruleset.h>
#include <ui_logic.h>
#include <weapons.h>
#include <v10_ui.h>
#include <kio_store.h> // krunker.io active: the market's writes are read-only
#include <app_mode.h>
#include <cJSON.h>
#include <market_stats.h> // the Stats reply keeps its rows on an error (market_stats_test)
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifndef _WIN32
#include <sys/time.h>
#endif

#include "../viewer/viewer_store.h"
#include "v10_market_guide.h"
#include "v10_junkyard.h"

#define UNUSED (void) ev; (void) args; (void) argc
#define PAGE 60          // the inventory / catalog grids' page (v10: 60, then 60 more per scroll)
#define LISTING_PAGE 60  // Browse: item groups per page (v10 pages the API by cursor)
#define NOTE_MS 5000     // market notifications (v10: 5 s)
#define TNOTE_MS 4000    // trading / black market notifications (v10: 4 s)
#define SALE_MAX_KR 10000000
#define SEASONS 10

enum { TAB_MARKET, TAB_TRADING, TAB_BLACKM, TAB_JUNKYARD, TAB_CATALOG, TAB_GUIDE, TAB_COUNT };
enum { MT_BROWSE, MT_INVENTORY, MT_SALES, MT_HISTORY, MT_STATS, MT_COUNT };
enum {
    MODAL_NONE, MODAL_PURCHASE, MODAL_LIST, MODAL_UNLIST, MODAL_QS, MODAL_BM, MODAL_DECLINE_ALL, MODAL_START,
    MODAL_TRADE_CONFIRM
};
// the action whose reply (or "po" refusal) is awaited
enum { ACT_NONE, ACT_BUY, ACT_LIST, ACT_UNLIST, ACT_QS, ACT_BM, ACT_ACCEPT, ACT_DECLINE, ACT_CANCEL, ACT_DECLINE_ALL, ACT_CREATE, ACT_SEARCH, ACT_TRADE_INV };

static const struct { const char *icon, *label; } k_side[TAB_COUNT] = {
    {"storefront", "Market"}, {"swap_horiz", "Trading"}, {"shopping_bag", "Black Mkt"},
    {"plumbing", "Junkyard"}, {"inventory_2", "Catalog"}, {"menu_book", "Guide"},
};
static const char *const k_mtabs[MT_COUNT] = {"Browse", "My Inventory", "My Sales", "History", "Stats"};
// v10 social.market.* by item type (1..18), then the weapon's name
static const char *const k_type_label[19] = {
    "", "Hat Item", "Body Item", "Melee Item", "Spray Item", "Dye Item", "Waist Item", "Face Item", "Shoe Item", "Pet Item",
    "Collectible Item", "Wrist Item", "Charm Item", "Spin Ticket Item", "Back Item", "Head Item", "Player Card", "Emoji", "Paint",
};
// the filter panel's Cosmetics labels (store.types path prefixes, capitalised)
static const char *const k_type_names[19] = {
    "", "Hats", "Body", "Melee", "Sprays", "Dyes", "Waist", "Faces", "Shoes", "Pets", "Collectibles", "Wrist", "Charms",
    "Tickets", "Back", "Head", "Playercards", "Emoji", "Paints",
};
static const char *const k_wear_names[5] = {"Flawless", "Tested", "Trashed", "Wrecked", "No Wear"};

// One filter header's state (_45793): reset whenever its view is rebuilt, as v10's are
typedef struct {
    char search[64];
    int season, expanded, ftab;
    char sort[12];         // name | rarity | season | type | new | old
    char category[32];     // catalog: all | blackM | drop | ranked | challenge | spinable | tag:X
    int only_missing, hide_non_trade, only_paintable, only_painted, group_wear;
    unsigned char rar[8], wep[64], typ[19], wear[5];
} Filter;

typedef struct {
    int id, ok;
    char msg[160];
} Note;

static struct {
    int open, tab, mtab;
    int listening;
    Filter f;
    // Browse (["fil"] -> "fr")
    char bsort[16];        // latest | price_asc | price_desc
    cJSON *listings;
    int fil_token, bloading, bsearching, bshown, search_timer;
    char last_search[64];
    // the other sub-views (["r","market",sub])
    cJSON *inv, *sales, *hist, *stats;
    int inv_loading, sales_loading, hist_loading, stats_loading, gshown;
    char hcol[8];
    int hasc;
    // Listing Details (svelte-olnzsm) / the catalog's item detail (_45008)
    int detail, dtab, days, crange;   // Listing Details: item (-1 none), tab, Stats days
    int cdetail, ctab;                // the item detail page (_45008): item (-1 none), tab
    int data_item, dgd_range;         // whose listings / sales / owners are loaded; dgd: a range index (else days)
    cJSON *dlist, *dgd, *dview, *downers;
    int dlist_loading, dgd_loading, dview_loading;
    // the reads that got no answer by their deadline (LOST_*: the screen says so, not "Loading" for good)
    int lost, wait_timer, account_id;
    // modals
    int modal, busy, pending;
    int sel_skin, sel_listing, sel_price, sel_count;
    char price_in[16];
    char op[48];
    int err_open;
    char err_msg[256];
    int qs_mode;
    char qs_err[160];
    Note notes[6];
    int nnotes, note_seq;
    // Trading
    int ttab, actioning;
    cJSON *trades;
    int trades_loading, trades_loaded;
    char tnote[160];
    int tnote_err, tnote_seq;
    char start_name[40], start_err[160];
    int start_searching;
    // the Create Trade window
    int ct_open, ct_loading, ct_target_id, ct_missing, ct_submitting;
    char ct_target[40], ct_err[160], ct_msg[504], ct_search[2][48];
    cJSON *ct_inv;         // [mine, theirs] rows {i, c, nt}
    EconTradeItem ct_sel[2][40];
    int ct_n[2];
    // Black Market
    char bm_note[160];
    int bm_note_err, bm_note_seq, bm_buying;
    // Catalog
    int cshown;
    // Guide
    int gtab;
} M = {.days = 30, .crange = 3, .detail = -1, .cdetail = -1, .data_item = -1};

// ------------------------------------------------------------------ helpers

static double now_ms(void) {
#ifdef _WIN32
    return (double) time(NULL) * 1000.0;
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double) tv.tv_sec * 1000.0 + (double) tv.tv_usec / 1000.0;
#endif
}

// _8986: the integer part with a comma every three digits
static void fmt(char *out, const size_t n, const double v) {
    char raw[48];
    snprintf(raw, sizeof(raw), "%.0f", v);
    const char *d = raw[0] == '-' ? raw + 1 : raw;
    const size_t len = strlen(d);
    size_t o = 0;
    if (raw[0] == '-' && o + 1 < n) out[o++] = '-';
    for (size_t i = 0; i < len && o + 2 < n; i++) {
        if (i && (len - i) % 3 == 0) out[o++] = ',';
        out[o++] = d[i];
    }
    out[o] = 0;
}

// _9001: "{d}d ago" / "{h}h ago" / "{m}m ago" / "just now"
static void ago(char *out, const size_t n, const double ms) {
    if (ms < 0) {
        snprintf(out, n, "just now");
        return;
    }
    const long long d = (long long) (ms / 86400000.0), h = (long long) (ms / 3600000.0) % 24, m = (long long) (ms / 60000.0) % 60;
    if (d) snprintf(out, n, "%lldd ago", d);
    else if (h) snprintf(out, n, "%lldh ago", h);
    else if (m) snprintf(out, n, "%lldm ago", m);
    else snprintf(out, n, "just now");
}

// _8991: "{y}y {d}d {h}h {m}m " (no seconds, the trailing space kept)
static void age_text(char *out, const size_t n, const double ms) {
    const long long mins = (long long) (ms / 60000.0), y = mins / 525600, d = mins / 1440 % 365, h = mins / 60 % 24, m = mins % 60;
    size_t o = 0;
    out[0] = 0;
    if (y) o += (size_t) snprintf(out + o, n - o, "%lldy ", y);
    if (d && o < n) o += (size_t) snprintf(out + o, n - o, "%lldd ", d);
    if (h && o < n) o += (size_t) snprintf(out + o, n - o, "%lldh ", h);
    if (o < n) snprintf(out + o, n - o, "%lldm ", m);
}

static double jnum(const cJSON *o, const char *k) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsNumber(v)) return v->valuedouble;
    if (cJSON_IsString(v) && v->valuestring[0]) return strtod(v->valuestring, NULL);
    return 0;
}
static int jint(const cJSON *o, const char *k, const int def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsNumber(v)) return v->valueint;
    if (cJSON_IsString(v) && v->valuestring[0]) return atoi(v->valuestring);
    return def;
}
static const char *jstr(const cJSON *o, const char *k) {
    const char *s = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, k));
    return s ? s : "";
}

static double arg_n(const HtmlArg *args, const int argc, const int i, const double def) {
    if (!args || i >= argc) return def;
    if (args[i].kind == HTML_ARG_NUM || args[i].kind == HTML_ARG_BOOL) return args[i].num;
    if (args[i].str) return strtod(args[i].str, NULL);
    return def;
}
static const char *arg_s(const HtmlEvent *ev, const HtmlArg *args, const int argc, const int i) {
    if (!args || i >= argc) return "";
    if (args[i].kind == HTML_ARG_THIS_VALUE) return ev && ev->value ? ev->value : "";
    return args[i].str ? args[i].str : "";
}

static int ci_contains(const char *hay, const char *needle) {
    if (!needle || !needle[0]) return 1;
    if (!hay) return 0;
    const size_t n = strlen(needle);
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < n && p[i] && tolower((unsigned char) p[i]) == tolower((unsigned char) needle[i])) i++;
        if (i == n) return 1;
    }
    return 0;
}

static void trim_lower(char *out, const size_t n, const char *in) {
    while (*in && isspace((unsigned char) *in)) in++;
    size_t o = 0;
    for (; *in && o + 1 < n; in++) out[o++] = (char) tolower((unsigned char) *in);
    while (o && isspace((unsigned char) out[o - 1])) o--;
    out[o] = 0;
}

static const ViewerSkin *skin(const int i) { return viewer_skin(i); }

static const char *rarity_color(const ViewerSkin *s) {
    return s && s->rarity >= 0 && s->rarity < g_viewer_rarity_count ? g_viewer_rarities[s->rarity].color : "#B0B0B0";
}
static int rarity_animate(const ViewerSkin *s) { return s && s->rarity == 6; } // Unobtainable
static int rarity_dark(const ViewerSkin *s) { return s && s->rarity == 5; }   // Contraband

static void thumb(char *out, const size_t n, const ViewerSkin *s) {
    char path[256];
    viewer_skin_preview_path(s, path, sizeof(path));
    html_dom_asset_url(path, out, n);
}

// their i18n keys (v10 iîìiîíí, game.deob.js:24551): i18n.get(key) is the label
static const char *const k_type_key[19] = {
    NULL, "social.market.hat", "social.market.body", "social.market.melee", "social.market.spray", "social.market.dye", "social.market.waist", "social.market.face", "social.market.shoe", "social.market.pet",
    "social.market.collectible", "social.market.wrist", "social.market.charm", "social.market.ticket", "social.market.back", "social.market.head", "social.market.playerCard", "social.market.emoji", "social.market.paint",
};

static const char *type_label(const ViewerSkin *s) {
    if (s->type >= 1 && s->type <= 18) return i18n_tr(k_type_key[s->type], k_type_label[s->type]);
    const Weapon *w = s->weapon >= 1 ? rules_weapon(s->weapon - 1) : NULL;
    return w && w->name ? w->name : "";
}

static int weapon_count(void) {
    const int n = rules_krunker_weapon_count();
    return n < 64 ? n : 64;
}
static int weapon_melee(const int i) {
    const Weapon *w = rules_weapon(i);
    return !w || w->melee;
}

// _44632: v10's sellable check: the item table's tradable bit (owned rows also carry the server's nt)
static int skin_bound(const ViewerSkin *s) {
    return !s || !(s->flags2 & VS2_TRADABLE);
}

// the list price minimum (_44634): 1 for collectibles, else the rarity's sell value
static int list_min_price(const ViewerSkin *s) {
    if (!s) return 1;
    if (s->type == 10) return 1;
    return econ_rarity_sell(s->rarity);
}

static const AccountInfo *acc(void) { return active_account(); }
static int logged_in(void) { return active_account() != NULL; }

static int account_owned(const int skin_index) {
    const AccountInfo *a = acc();
    const cJSON *skins = a && a->data ? cJSON_GetArrayItem(a->data, 10) : NULL, *e;
    cJSON_ArrayForEach(e, skins) {
        if (jint(e, "ind", -1) == skin_index) return jint(e, "cnt", 1);
    }
    return 0;
}

static HtmlElem el(const char *id) { return html_dom_by_id(id); }

static void set_html(const char *id, const char *html) {
    const HtmlElem e = el(id);
    if (e) html_dom_set_html(e, html);
}

static void svg_data_url(StrBuf *sb, const char *svg) {
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const size_t len = strlen(svg);
    sb_append(sb, "data:image/svg+xml;base64,");
    char quad[5] = {0};
    for (size_t i = 0; i < len; i += 3) {
        const unsigned v = (unsigned char) svg[i] << 16 | (i + 1 < len ? (unsigned char) svg[i + 1] << 8 : 0) | (i + 2 < len ? (unsigned char) svg[i + 2] : 0);
        quad[0] = b64[v >> 18 & 63];
        quad[1] = b64[v >> 12 & 63];
        quad[2] = i + 1 < len ? b64[v >> 6 & 63] : '=';
        quad[3] = i + 2 < len ? b64[v & 63] : '=';
        sb_append(sb, quad);
    }
}

// ------------------------------------------------------------------ transport

// The reads in flight (their loading flags). A reply can be lost for good (the socket dropped with the request
// in it, a brain that refused the query), and the flag would then stay set: every sent frame arms one
// deadline, and when it expires the flags still set are cleared and the screen shows its error (M.lost). A
// reply that arrives after a reopen is still applied.
#define MARKET_WAIT_MS 15000
enum { LOST_TRADES = 1, LOST_DLIST = 2, LOST_DGD = 4, LOST_DVIEW = 8 };

static void reads_expired(void *user);

// 1 when the frame went out (0: no socket, so no reply will come)
static int send_frame(cJSON *f) {
    int sent = 0;
    if (f) sent = backend_social_send(f);
    cJSON_Delete(f);
    if (sent) {
        html_timer_cancel(M.wait_timer);
        M.wait_timer = html_timer_add(MARKET_WAIT_MS, reads_expired, NULL);
    }
    return sent;
}

// ["r", page, sub, K, q, s, region, item]: the brain's market pages
static int request_page(const char *page, const char *sub, const int item) {
    const AccountInfo *a = acc();
    cJSON *f = cJSON_CreateArray();
    cJSON_AddItemToArray(f, cJSON_CreateString("r"));
    cJSON_AddItemToArray(f, cJSON_CreateString(page));
    cJSON_AddItemToArray(f, sub ? cJSON_CreateString(sub) : cJSON_CreateNull());
    cJSON_AddItemToArray(f, a ? cJSON_CreateNumber(a->id) : cJSON_CreateNull());
    cJSON_AddItemToArray(f, a ? cJSON_CreateString(a->name) : cJSON_CreateNull());
    cJSON_AddItemToArray(f, cJSON_CreateNull());
    cJSON_AddItemToArray(f, cJSON_CreateNumber(0));
    cJSON_AddItemToArray(f, cJSON_CreateNumber(item));
    return send_frame(f);
}

static void send_econ(const char *label, const int n, const double *v, const char *key) {
    send_frame(econ_frame(label, n, v, key));
}

// ------------------------------------------------------------------ filters

static void filter_reset(Filter *f, const char *sort) {
    memset(f, 0, sizeof(*f));
    snprintf(f->sort, sizeof(f->sort), "%s", sort);
    snprintf(f->category, sizeof(f->category), "all");
    for (int i = 0; i < 7; i++) f->rar[i] = 1; // every rarity but NFT
    for (int i = 0; i < 64; i++) f->wep[i] = 1;
    for (int i = 1; i <= 18; i++) f->typ[i] = 1;
    for (int i = 0; i < 5; i++) f->wear[i] = 1;
}

static int set_count(const unsigned char *set, const int from, const int to) {
    int n = 0;
    for (int i = from; i < to; i++) n += set[i] != 0;
    return n;
}
static int weapons_all(const Filter *f) {
    for (int i = 0; i < weapon_count(); i++) if (!weapon_melee(i) && !f->wep[i]) return 0;
    return 1;
}
static int weapons_none(const Filter *f) {
    for (int i = 0; i < weapon_count(); i++) if (!weapon_melee(i) && f->wep[i]) return 0;
    return 1;
}

// hasActiveFilters
static int filter_active(const Filter *f, const char *default_sort, const int wear_filter, const int category) {
    if (f->search[0] || f->season || f->only_missing || f->hide_non_trade || f->only_paintable || f->only_painted) return 1;
    if (set_count(f->rar, 0, 7) != 7 || !weapons_all(f) || set_count(f->typ, 1, 19) != 18) return 1;
    if (wear_filter && set_count(f->wear, 0, 5) != 5) return 1;
    if (strcmp(f->sort, default_sort) != 0) return 1;
    return category && strcmp(f->category, "all") != 0;
}

// the client-side filters of the inventory / sales / stats / catalog headers
static int filter_match(const Filter *f, const ViewerSkin *s) {
    if (!s) return 0;
    if (f->search[0] && !ci_contains(s->name, f->search)) return 0;
    if (f->season && (s->seas ? s->seas : 1) != f->season) return 0;
    if (f->hide_non_trade && skin_bound(s)) return 0;
    if (f->only_paintable || f->only_painted) return 0; // no paintable / painted items in this store
    if (s->rarity >= 0 && s->rarity < 8 && set_count(f->rar, 0, 8) && !f->rar[s->rarity]) return 0;
    if (s->weapon >= 1) {
        if (s->weapon - 1 < 64 && !weapons_none(f) && !f->wep[s->weapon - 1]) return 0;
    } else if (s->type >= 1 && s->type <= 18) {
        if (set_count(f->typ, 1, 19) && !f->typ[s->type]) return 0;
    }
    if (set_count(f->wear, 0, 5) && !f->wear[4]) return 0; // every item here is "No Wear"
    if (strcmp(f->category, "all") != 0) {
        const int index = (int) (s - g_viewer_skins);
        if (!strcmp(f->category, "blackM")) return econ_black_market_find(index) != NULL;
        if (!strcmp(f->category, "spinable")) return (s->seas ? s->seas : 1) == g_viewer_current_season && !(s->flags & (VS_NOT_IN_WHEEL | VS_BLOCKED));
        return 0; // drop / ranked / challenge / tag:X: no such fields in the store table
    }
    return 1;
}

static const char *g_sort_key = "rarity";
static int cmp_skins(const void *pa, const void *pb) {
    const int a = *(const int *) pa, b = *(const int *) pb;
    const ViewerSkin *sa = skin(a), *sb = skin(b);
    if (!sa || !sb) return (sa != NULL) - (sb != NULL);
    int d = 0;
    if (!strcmp(g_sort_key, "name")) d = strcmp(sa->name ? sa->name : "", sb->name ? sb->name : "");
    else if (!strcmp(g_sort_key, "rarity")) d = sb->rarity - sa->rarity;
    else if (!strcmp(g_sort_key, "season")) d = (sb->seas ? sb->seas : 1) - (sa->seas ? sa->seas : 1);
    else if (!strcmp(g_sort_key, "type")) d = sa->type - sb->type;
    else if (!strcmp(g_sort_key, "new")) d = b - a;
    else if (!strcmp(g_sort_key, "old")) d = a - b;
    return d ? d : a - b;
}

typedef struct {
    const char *title;
    int count, has_more;
    const char *default_sort;
    int search_in_header, season, sort_select, category, newest, wear, checkboxes; // checkbox bits: 1 missing 2 nontrade 4 paintable 8 painted 16 group
    const char *title_extra; // html
    const char *header_controls; // html (Browse's sort select)
} FilterView;

static void filter_header(StrBuf *sb, const Filter *f, const FilterView *v) {
    sb_appendf(sb, "<div class=\"inventory-filter svelte-ga3en0\"><div class=\"filter-header svelte-ga3en0%s\" role=\"button\" tabindex=\"0\" onclick=\"v10mk.fhead()\">"
                   "<h3 class=\"svelte-ga3en0\" id=\"v10mkFTitle\">", f->expanded ? " expanded" : "");
    sb_append_esc(sb, v->title);
    sb_appendf(sb, " (%d%s items)", v->count, v->has_more ? "+" : "");
    if (v->title_extra) sb_append(sb, v->title_extra);
    sb_append(sb, "</h3> <div class=\"filter-controls svelte-ga3en0\" onclick=\"v10mk.stop()\">");
    if (v->search_in_header && !f->expanded) {
        sb_append(sb, "<input type=\"search\" placeholder=\"Search items...\" class=\"search-input header-search svelte-ga3en0\" id=\"v10mkHSearch\" value=\"");
        sb_append_esc(sb, f->search);
        sb_append(sb, "\" oninput=\"v10mk.search(this.value)\">");
    }
    sb_append(sb, " ");
    if (v->header_controls) sb_append(sb, v->header_controls);
    sb_append(sb, " ");
    if (filter_active(f, v->default_sort, v->wear, v->category)) sb_append(sb, "<button class=\"reset-btn svelte-ga3en0\" onclick=\"v10mk.reset()\">Reset Filters</button>");
    sb_appendf(sb, " <button class=\"expand-btn svelte-ga3en0\" onclick=\"v10mk.fhead()\"><i class=\"material-icons svelte-ga3en0\">%s</i></button></div></div> ",
               f->expanded ? "expand_less" : "expand_more");
    if (f->expanded) {
        sb_append(sb, "<div class=\"filter-content svelte-ga3en0\"><div class=\"filter-row svelte-ga3en0\"><input type=\"search\" placeholder=\"Search items...\" class=\"search-input svelte-ga3en0\" id=\"v10mkSearch\" value=\"");
        sb_append_esc(sb, f->search);
        sb_append(sb, "\" oninput=\"v10mk.search(this.value)\"> ");
        if (v->season) {
            sb_append(sb, "<select class=\"season-select svelte-ga3en0\" onchange=\"v10mk.season(this.value)\">");
            sb_appendf(sb, "<option value=\"0\"%s>All Seasons</option>", f->season == 0 ? " selected" : "");
            for (int s = 1; s <= SEASONS; s++) sb_appendf(sb, "<option value=\"%d\"%s>Season %d</option>", s, f->season == s ? " selected" : "", s);
            sb_append(sb, "</select>");
        }
        sb_append(sb, " ");
        if (v->category) {
            static const char *const cats[][2] = {{"all", "All Categories"}, {"blackM", "Black Market"}, {"drop", "Drops"}, {"ranked", "Ranked"},
                                                  {"challenge", "Challenge"}, {"spinable", "Spinable"}};
            static const char *const tags[] = {"Debug", "Easter", "Twitch", "Clan Wars", "Charity", "Raid", "Canteen", "Gift", "Free", "Christmas",
                                               "Burg", "Sandstorm", "Littletown", "Site", "Industry", "Subzero", "Freight", "Undergrowth",
                                               "Lostworld", "Kanji", "Citadel", "Oasis", "Evacuation", "Craftable", "NFT", "Ranked", "FRVR",
                                               "Halloween", "Battle Pass", "Premium Spin"};
            sb_append(sb, "<select class=\"category-select svelte-ga3en0\" onchange=\"v10mk.cat(this.value)\">");
            for (size_t i = 0; i < sizeof(cats) / sizeof(cats[0]); i++)
                sb_appendf(sb, "<option value=\"%s\"%s>%s</option>", cats[i][0], !strcmp(f->category, cats[i][0]) ? " selected" : "", cats[i][1]);
            for (size_t i = 0; i < sizeof(tags) / sizeof(tags[0]); i++) {
                char val[40];
                snprintf(val, sizeof(val), "tag:%s", tags[i]);
                sb_appendf(sb, "<option value=\"%s\"%s>%s</option>", val, !strcmp(f->category, val) ? " selected" : "", tags[i]);
            }
            sb_append(sb, "</select>");
        }
        sb_append(sb, " ");
        if (v->sort_select) {
            static const char *const sorts[][2] = {{"name", "Sort by Name"}, {"rarity", "Sort by Rarity"}, {"season", "Sort by Season"}, {"type", "Sort by Type"}};
            sb_append(sb, "<select class=\"sort-select svelte-ga3en0\" onchange=\"v10mk.fsort(this.value)\">");
            for (int i = 0; i < 4; i++) sb_appendf(sb, "<option value=\"%s\"%s>%s</option>", sorts[i][0], !strcmp(f->sort, sorts[i][0]) ? " selected" : "", sorts[i][1]);
            if (v->newest)
                sb_appendf(sb, "<option value=\"new\"%s>Sort by Newest</option> <option value=\"old\"%s>Sort by Oldest</option>",
                           !strcmp(f->sort, "new") ? " selected" : "", !strcmp(f->sort, "old") ? " selected" : "");
            sb_append(sb, "</select>");
        }
        sb_append(sb, "</div> ");
        if (v->checkboxes) {
            static const char *const boxes[5] = {"Only Show Missing", "Hide Non-Tradable", "Show Only Paintable", "Only Painted", "Group by Wear"};
            const int on[5] = {f->only_missing, f->hide_non_trade, f->only_paintable, f->only_painted, f->group_wear};
            sb_append(sb, "<div class=\"filter-row svelte-ga3en0\">");
            for (int i = 0; i < 5; i++) {
                if (i) sb_append(sb, " ");
                if (!(v->checkboxes & (1 << i))) continue;
                sb_appendf(sb, "<label class=\"filter-checkbox svelte-ga3en0\"><input type=\"checkbox\" class=\"svelte-ga3en0\"%s onchange=\"v10mk.fchk(%d)\"> "
                               "<span class=\"svelte-ga3en0\">%s</span></label>", on[i] ? " checked" : "", i, boxes[i]);
            }
            sb_append(sb, "</div> ");
        }
        static const char *const ftabs[4] = {"Rarities", "Weapons", "Cosmetics", "Wear"};
        sb_append(sb, "<div class=\"filter-tabs svelte-ga3en0\">");
        for (int i = 0; i < 4; i++) {
            if (i == 3 && !v->wear) continue;
            sb_appendf(sb, "<button class=\"filter-tab svelte-ga3en0%s\" onclick=\"v10mk.ftab(%d)\">%s</button> ", f->ftab == i ? " active" : "", i, ftabs[i]);
        }
        sb_append(sb, "<button class=\"toggle-all-btn svelte-ga3en0\" onclick=\"v10mk.fall()\">Toggle All</button></div> <div class=\"filter-panel svelte-ga3en0\"><div class=\"filter-grid svelte-ga3en0\">");
        if (f->ftab == 0) {
            for (int r = 0; r < 7 && r < g_viewer_rarity_count; r++)
                sb_appendf(sb, "<div class=\"filter-item-wrapper svelte-ga3en0\"><label class=\"filter-item rarity-item svelte-ga3en0\"><input type=\"checkbox\" class=\"svelte-ga3en0\"%s onchange=\"v10mk.fset(0,%d)\"> "
                               "<div class=\"rarity-color svelte-ga3en0\" style=\"background-color: %s\"></div> <span class=\"svelte-ga3en0\">%s</span></label> "
                               "<button class=\"only-btn svelte-ga3en0\" onclick=\"v10mk.fonly(0,%d)\">only</button></div>",
                           f->rar[r] ? " checked" : "", r, g_viewer_rarities[r].color, g_viewer_rarities[r].name, r);
        } else if (f->ftab == 1) {
            for (int w = 0; w < weapon_count(); w++) {
                if (weapon_melee(w)) continue;
                const Weapon *wp = rules_weapon(w);
                char name[48];
                if (wp && wp->name) snprintf(name, sizeof(name), "%s", wp->name);
                else snprintf(name, sizeof(name), "Weapon %d", w);
                sb_appendf(sb, "<div class=\"filter-item-wrapper svelte-ga3en0\"><label class=\"filter-item svelte-ga3en0\"><input type=\"checkbox\" class=\"svelte-ga3en0\"%s onchange=\"v10mk.fset(1,%d)\"> "
                               "<span class=\"svelte-ga3en0\">", f->wep[w] ? " checked" : "", w);
                sb_append_esc(sb, name);
                sb_appendf(sb, "</span></label> <button class=\"only-btn svelte-ga3en0\" onclick=\"v10mk.fonly(1,%d)\">only</button></div>", w);
            }
        } else if (f->ftab == 2) {
            for (int t = 1; t <= 18; t++)
                sb_appendf(sb, "<div class=\"filter-item-wrapper svelte-ga3en0\"><label class=\"filter-item svelte-ga3en0\"><input type=\"checkbox\" class=\"svelte-ga3en0\"%s onchange=\"v10mk.fset(2,%d)\"> "
                               "<span class=\"svelte-ga3en0\">%s</span></label> <button class=\"only-btn svelte-ga3en0\" onclick=\"v10mk.fonly(2,%d)\">only</button></div>",
                           f->typ[t] ? " checked" : "", t, k_type_names[t], t);
        } else {
            for (int w = 0; w < 5; w++)
                sb_appendf(sb, "<div class=\"filter-item-wrapper svelte-ga3en0\"><label class=\"filter-item svelte-ga3en0\"><input type=\"checkbox\" class=\"svelte-ga3en0\"%s onchange=\"v10mk.fset(3,%d)\"> "
                               "<span class=\"svelte-ga3en0\">%s</span></label> <button class=\"only-btn svelte-ga3en0\" onclick=\"v10mk.fonly(3,%d)\">only</button></div>",
                           f->wear[w] ? " checked" : "", w, k_wear_names[w], w);
        }
        sb_append(sb, "</div></div></div>");
    }
    sb_append(sb, "</div>");
}

// ------------------------------------------------------------------ cards

// _36396: the generic item card (svelte-r5taai)
static void item_card(StrBuf *sb, const ViewerSkin *s, const char *size, const int top_center, const char *onclick, const int owned_count) {
    const int index = (int) (s - g_viewer_skins);
    char url[512];
    thumb(url, sizeof(url), s);
    const char *col = rarity_color(s);
    const int tiny = size && atoi(size) > 0 && atoi(size) <= 80;
    sb_appendf(sb, "<div class=\"market-card svelte-r5taai%s%s\" role=\"button\" tabindex=\"0\" style=\"border: 2px solid %s; width: %s; height: %s;\" data-index=\"%d\"",
               rarity_animate(s) ? " rainbow" : "", tiny ? " tiny" : "", col, size ? size : "136px", size ? size : "136px", index);
    if (onclick) sb_appendf(sb, " onclick=\"%s\"", onclick);
    sb_append(sb, ">");
    if (s->type == 4) {
        sb_appendf(sb, "<div class=\"itemImg sprayMarketImg svelte-r5taai\" style=\"background-image: url(%s)\"></div>", url);
    } else {
        sb_appendf(sb, "<img draggable=\"false\" loading=\"lazy\" class=\"itemImg marketImg svelte-r5taai%s\" src=\"%s\" alt=\"", (s->flags & VS_RGB) ? " rgbHue" : "", url);
        sb_append_esc(sb, s->name ? s->name : "");
        sb_append(sb, "\">");
    }
    sb_append(sb, " ");
    if (s->name && !strcmp(s->name, "Snowball Machine")) sb_append(sb, "<img class=\"snowy-bg svelte-r5taai\" alt=\"snowy background\" src=\"./img/snowy_bg.webp\">");
    sb_appendf(sb, " <div class=\"itemInfo svelte-r5taai%s\"> <div class=\"itemName svelte-r5taai%s\" style=\"color: %s\">", top_center ? " top-center" : "",
               rarity_dark(s) ? " dark" : "", col);
    sb_append_esc(sb, s->name && s->name[0] ? s->name : "Unnamed Item");
    sb_append(sb, "</div>  <div class=\"itemType svelte-r5taai\">");
    sb_append_esc(sb, type_label(s));
    sb_append(sb, "</div> ");
    if (owned_count > 0 && !top_center) sb_appendf(sb, "<div class=\"itemDesc svelte-r5taai\">%d owned</div>", owned_count);
    sb_append(sb, "</div> ");
    if (s->puls_t > 0 || s->frames > 0 || s->mov_t > 0 || s->r_anim > 0 || (s->flags & VS_RGB)) sb_append(sb, "<div class=\"material-icons isAnim svelte-r5taai\">loop</div>");
}

typedef struct {
    int listing_id, price;    // a listing (Browse): Buy
    const char *seller;
    int count;                // count badge (Browse: "N on sale", inventory: "xN")
    int on_sale;              // count is a listing count
    int sale_id;              // My Sales: Unlist
    double listed_ms;         // My Sales: listing age
    int list;                 // inventory: List
    int locked;               // inventory: Not sellable
} CardOpts;

// _45592: the market card (svelte-hk8nq9): the item card + its overlay
static void market_card(StrBuf *sb, const int index, const CardOpts *o) {
    const ViewerSkin *s = skin(index);
    if (!s) return;
    char onclick[48], n[32];
    snprintf(onclick, sizeof(onclick), "v10mk.item(%d)", index);
    sb_append(sb, "<div class=\"market-card-shell svelte-hk8nq9\">");
    item_card(sb, s, "100%", 1, onclick, 0);
    sb_append(sb, "<div class=\"overlay-container svelte-hk8nq9\">");
    if (o->count > 0) {
        if (o->on_sale) sb_appendf(sb, "<div class=\"count-badge svelte-hk8nq9\">%d on sale</div>", o->count);
        else sb_appendf(sb, "<div class=\"count-badge svelte-hk8nq9\">x%d</div>", o->count);
    }
    sb_append(sb, " ");
    if (o->listed_ms > 0) {
        char t[32];
        ago(t, sizeof(t), now_ms() - o->listed_ms);
        sb_appendf(sb, "<div class=\"listing-age-badge svelte-hk8nq9\">%s</div>", t);
    }
    sb_append(sb, " <div class=\"estimate-badge svelte-hk8nq9\">~ ");
    const int est = social_estimate_value(index);
    if (est > 0) {
        fmt(n, sizeof(n), est);
        sb_appendf(sb, "%s <span class=\"kr-text svelte-hk8nq9\">KR</span>", n);
    } else {
        sb_append(sb, "Unknown");
    }
    sb_append(sb, "</div> ");
    const AccountInfo *a = acc();
    const int own_listing = a && o->seller && !strcmp(o->seller, a->name);
    if (o->listing_id > 0 && !own_listing) {
        fmt(n, sizeof(n), o->price);
        sb_appendf(sb, "<div class=\"inline-action svelte-hk8nq9\"><span class=\"inline-price svelte-hk8nq9\">%s <span class=\"kr-text svelte-hk8nq9\">KR</span></span> "
                       "<button class=\"icon-btn buy svelte-hk8nq9\" id=\"v10mkBuy%d\" aria-label=\"Buy\"%s onclick=\"v10mk.buy(%d,%d,%d)\"><i class=\"material-icons svelte-hk8nq9\">shopping_cart</i></button></div>",
                   n, o->listing_id, M.busy ? " disabled" : "", o->listing_id, index, o->price);
    } else if (o->list && !o->locked) {
        sb_appendf(sb, "<div class=\"inline-action svelte-hk8nq9\"><button class=\"icon-btn list with-label svelte-hk8nq9\" id=\"v10mkList%d\" aria-label=\"List for sale\"%s onclick=\"v10mk.list(%d)\">"
                       "<i class=\"material-icons svelte-hk8nq9\">sell</i> <span class=\"btn-label svelte-hk8nq9\">List</span></button></div>",
                   index, M.busy ? " disabled" : "", index);
    } else if (o->sale_id > 0) {
        fmt(n, sizeof(n), o->price);
        sb_appendf(sb, "<div class=\"inline-action svelte-hk8nq9\"><span class=\"inline-price svelte-hk8nq9\">%s <span class=\"kr-text svelte-hk8nq9\">KR</span></span> "
                       "<button class=\"icon-btn unlist svelte-hk8nq9\" id=\"v10mkUnlist%d\" aria-label=\"Unlist\"%s onclick=\"v10mk.unlist(%d,%d)\"><i class=\"material-icons svelte-hk8nq9\">close</i></button></div>",
                   n, o->sale_id, M.sales_loading ? " disabled" : "", o->sale_id, index);
    } else if (o->locked) {
        sb_append(sb, "<div class=\"inline-action svelte-hk8nq9\"><span class=\"not-sellable svelte-hk8nq9\"><i class=\"material-icons svelte-hk8nq9\">lock</i> Not sellable</span></div>");
    }
    sb_append(sb, "</div></div></div>");
}

// ------------------------------------------------------------------ Market: data

static int rows_count(const cJSON *a) { return cJSON_IsArray(a) ? cJSON_GetArraySize(a) : 0; }

static void set_json(cJSON **slot, const cJSON *v) {
    cJSON_Delete(*slot);
    *slot = cJSON_IsArray(v) ? cJSON_Duplicate(v, 1) : cJSON_CreateArray();
}

static int filter_none_left(const Filter *f) {
    // a set the user emptied keeps v10's meaning ("send nothing" = all); only Wear and Painted can
    // rule out every item here (all items are "No Wear", none painted)
    return (set_count(f->wear, 0, 5) && !f->wear[4]) || f->only_painted;
}

// Browse: ["fil", n, text, rarities, weapons, types, season, order] -> ["fr", n, rows | null]
static void browse_load(void) {
    if (!logged_in()) return;
    char t[64];
    trim_lower(t, sizeof(t), M.f.search);
    if (!t[0] && filter_none_left(&M.f)) {
        set_json(&M.listings, NULL);
        M.bloading = 0;
        return;
    }
    cJSON *f = cJSON_CreateArray(), *rar = cJSON_CreateObject(), *wep = cJSON_CreateObject(), *typ = cJSON_CreateObject();
    char key[8];
    if (!t[0]) { // a search ignores the filters and the sort select (v10: items=ids&sort=price_asc)
        if (set_count(M.f.rar, 0, 7) < 7)
            for (int r = 0; r < 8; r++) if (M.f.rar[r]) { snprintf(key, sizeof(key), "%d", r); cJSON_AddNumberToObject(rar, key, 1); }
        if (!weapons_all(&M.f))
            for (int w = 0; w < weapon_count(); w++) if (!weapon_melee(w) && M.f.wep[w]) { snprintf(key, sizeof(key), "%d", w + 1); cJSON_AddNumberToObject(wep, key, 1); }
        if (set_count(M.f.typ, 1, 19) < 18)
            for (int ty = 1; ty <= 18; ty++) if (M.f.typ[ty]) { snprintf(key, sizeof(key), "%d", ty); cJSON_AddNumberToObject(typ, key, 1); }
    }
    char season[8];
    snprintf(season, sizeof(season), "%d", t[0] ? 0 : M.f.season);
    cJSON_AddItemToArray(f, cJSON_CreateString("fil"));
    cJSON_AddItemToArray(f, cJSON_CreateNumber(++M.fil_token));
    cJSON_AddItemToArray(f, cJSON_CreateString(t));
    cJSON_AddItemToArray(f, rar);
    cJSON_AddItemToArray(f, wep);
    cJSON_AddItemToArray(f, typ);
    cJSON_AddItemToArray(f, cJSON_CreateString(season));
    cJSON_AddItemToArray(f, cJSON_CreateString(!t[0] && !strcmp(M.bsort, "price_desc") ? "0" : "1"));
    M.bloading = 1;
    M.bsearching = t[0] != 0;
    if (!send_frame(f)) M.bloading = M.bsearching = 0; // no socket: no reply will come
}

static int cmp_listing_time(const void *pa, const void *pb) {
    const double a = jnum(*(cJSON *const *) pa, "ms"), b = jnum(*(cJSON *const *) pb, "ms");
    return a < b ? 1 : a > b ? -1 : 0;
}

static void browse_loaded(const cJSON *rows) {
    M.bloading = M.bsearching = 0;
    set_json(&M.listings, rows);
    char t[64];
    trim_lower(t, sizeof(t), M.f.search);
    if (!t[0] && !strcmp(M.bsort, "latest")) { // "Latest Listings": newest first
        const int n = rows_count(M.listings);
        cJSON **v = calloc((size_t) (n ? n : 1), sizeof(*v));
        if (v) {
            for (int i = 0; i < n; i++) v[i] = cJSON_GetArrayItem(M.listings, i);
            qsort(v, (size_t) n, sizeof(*v), cmp_listing_time);
            cJSON *sorted = cJSON_CreateArray();
            for (int i = 0; i < n; i++) cJSON_AddItemToArray(sorted, cJSON_Duplicate(v[i], 1));
            free(v);
            cJSON_Delete(M.listings);
            M.listings = sorted;
        }
    }
    M.bshown = LISTING_PAGE;
}

static void inventory_load(void) {
    if (!logged_in() || M.inv_loading) return;
    M.inv_loading = 1;
    if (!request_page("market", "inventory", -1)) M.inv_loading = 0;
}
static void sales_load(void) {
    if (!logged_in() || M.sales_loading) return;
    M.sales_loading = 1;
    if (!request_page("market", "sales", -1)) M.sales_loading = 0;
}
static void history_load(void) {
    if (!logged_in() || M.hist_loading) return;
    M.hist_loading = 1;
    if (!request_page("market", "history", -1)) M.hist_loading = 0;
}
static void stats_load(void) {
    if (!logged_in() || M.stats_loading) return;
    M.stats_loading = 1;
    // no socket: no reply will clear the flag, so the next open of the tab asks again
    if (!request_page("market", "stats", -1)) M.stats_loading = 0;
}

static int inv_count(const int index) {
    const cJSON *r;
    cJSON_ArrayForEach(r, M.inv) if (jint(r, "i", -1) == index) return jint(r, "c", 1);
    return M.inv ? 0 : account_owned(index);
}
static const cJSON *inv_row(const int index) {
    const cJSON *r;
    cJSON_ArrayForEach(r, M.inv) if (jint(r, "i", -1) == index) return r;
    return NULL;
}

// the item's listings (the detail's Listings tab): ["r","market","market",..,item]
static void detail_listings_load(void) {
    if (M.data_item < 0) return;
    M.dlist_loading = 1;
    M.lost &= ~LOST_DLIST;
    if (!request_page("market", "market", M.data_item)) {
        M.dlist_loading = 0;
        M.lost |= LOST_DLIST;
    }
}
// the sales statistics: ["st", item, days] -> ["gd", [{d, f, t}]]
static void detail_stats_load(const int days_or_range) {
    if (M.data_item < 0) return;
    M.dgd_loading = 1;
    M.lost &= ~LOST_DGD;
    M.dgd_range = days_or_range < 7;
    cJSON_Delete(M.dgd);
    M.dgd = NULL;
    cJSON *f = cJSON_CreateArray();
    cJSON_AddItemToArray(f, cJSON_CreateString("st"));
    cJSON_AddItemToArray(f, cJSON_CreateNumber(M.data_item));
    cJSON_AddItemToArray(f, cJSON_CreateNumber(days_or_range));
    if (!send_frame(f)) {
        M.dgd_loading = 0;
        M.lost |= LOST_DGD;
    }
}
// owners + market stats: ["r","itemsales",..,item] -> ["0","itemsales",sub,{ind,onS,inC,high,low},owners]
static void detail_view_load(void) {
    if (M.data_item < 0) return;
    M.dview_loading = 1;
    M.lost &= ~LOST_DVIEW;
    if (!request_page("itemsales", NULL, M.data_item)) {
        M.dview_loading = 0;
        M.lost |= LOST_DVIEW;
    }
}

// ------------------------------------------------------------------ notifications (_45504)

static void render_notes(void);
static void views_sync(void);
static void render(void);
static void render_content(void);
static void render_modal(void);

static void note_expire(void *user) {
    const int id = (int) (intptr_t) user;
    for (int i = 0; i < M.nnotes; i++) {
        if (M.notes[i].id != id) continue;
        memmove(&M.notes[i], &M.notes[i + 1], sizeof(Note) * (size_t) (M.nnotes - i - 1));
        M.nnotes--;
        break;
    }
    render_notes();
}

static void notify(const int ok, const char *msg) {
    if (M.nnotes == (int) (sizeof(M.notes) / sizeof(M.notes[0]))) {
        memmove(&M.notes[0], &M.notes[1], sizeof(Note) * (size_t) (M.nnotes - 1));
        M.nnotes--;
    }
    Note *n = &M.notes[M.nnotes++];
    n->id = ++M.note_seq;
    n->ok = ok;
    snprintf(n->msg, sizeof(n->msg), "%s", msg ? msg : "");
    html_timer_add(NOTE_MS, note_expire, (void *) (intptr_t) n->id);
    render_notes();
}

static void notes_html(StrBuf *sb) {
    sb_append(sb, "<div class=\"notification-list svelte-ekpmow\">");
    for (int i = 0; i < M.nnotes; i++) {
        sb_appendf(sb, "<div class=\"notification-item %s svelte-ekpmow\"><i class=\"material-icons notification-icon svelte-ekpmow\">%s</i> <span class=\"notification-message svelte-ekpmow\">",
                   M.notes[i].ok ? "success" : "error", M.notes[i].ok ? "check_circle" : "error");
        sb_append_esc(sb, M.notes[i].msg);
        sb_append(sb, "</span></div>");
    }
    sb_append(sb, "</div>");
}

static void show_error(const char *msg) {
    M.err_open = 1;
    snprintf(M.err_msg, sizeof(M.err_msg), "%s", msg && msg[0] ? msg : "An unexpected error occurred. Please try again.");
    render_modal();
}

// ------------------------------------------------------------------ Market: views

static const char *g_loading_overlay = "<div class=\"loading-overlay svelte-1ugt6n9\"><div class=\"loading-spinner svelte-1ugt6n9\"><i class=\"material-icons svelte-1ugt6n9\">hourglass_empty</i></div></div>";

static void browse_grid(StrBuf *sb) {
    const int n = rows_count(M.listings);
    int *order = calloc((size_t) (n ? n : 1), sizeof(int)), *count = calloc((size_t) (n ? n : 1), sizeof(int));
    const cJSON **first = calloc((size_t) (n ? n : 1), sizeof(*first));
    int groups = 0;
    for (int i = 0; order && count && first && i < n; i++) {
        const cJSON *r = cJSON_GetArrayItem(M.listings, i);
        const int sk = jint(r, "i", -1);
        int g = 0;
        while (g < groups && order[g] != sk) g++;
        if (g == groups) {
            if (!skin(sk)) continue;
            order[groups] = sk;
            first[groups] = r;
            groups++;
        }
        count[g]++;
    }
    sb_append(sb, "<div class=\"market-grid svelte-zhgogl\" id=\"v10mkGrid\">");
    for (int g = 0; g < groups && g < M.bshown; g++) {
        const cJSON *r = first[g];
        sb_appendf(sb, "<div class=\"item-wrapper svelte-zhgogl%s\">", M.detail == order[g] ? " selected" : "");
        const CardOpts o = {.listing_id = jint(r, "m", 0), .price = jint(r, "f", 0), .seller = jstr(r, "p"), .count = count[g], .on_sale = 1};
        market_card(sb, order[g], &o);
        sb_append(sb, " </div>");
    }
    sb_append(sb, "</div> ");
    if (!n && !M.bloading && !M.bsearching) sb_append(sb, "<div class=\"empty-message svelte-zhgogl\">No listings available</div>");
    sb_append(sb, " ");
    if (groups > M.bshown) sb_append(sb, "<div class=\"load-more-sentinel svelte-zhgogl\"><button class=\"load-more-btn svelte-zhgogl\" onclick=\"v10mk.more()\">Load More</button></div>");
    free(order);
    free(count);
    free(first);
}

static int browse_groups(void) {
    const int n = rows_count(M.listings);
    int *seen = calloc((size_t) (n ? n : 1), sizeof(int)), groups = 0;
    for (int i = 0; seen && i < n; i++) {
        const int sk = jint(cJSON_GetArrayItem(M.listings, i), "i", -1);
        int g = 0;
        while (g < groups && seen[g] != sk) g++;
        if (g == groups && skin(sk)) seen[groups++] = sk;
    }
    free(seen);
    return groups;
}

static void view_browse(StrBuf *sb) {
    if (!logged_in()) {
        sb_append(sb, "<div class=\"empty-message svelte-111gya4\">Please log in to view market listings</div>");
        return;
    }
    StrBuf hc = {0};
    sb_appendf(&hc, "<select class=\"sort-select svelte-111gya4\" onchange=\"v10mk.bsort(this.value)\"><option value=\"price_desc\"%s>Price: High to Low</option>"
                    "<option value=\"price_asc\"%s>Price: Low to High</option><option value=\"latest\"%s>Latest Listings</option></select> ",
               !strcmp(M.bsort, "price_desc") ? " selected" : "", !strcmp(M.bsort, "price_asc") ? " selected" : "", !strcmp(M.bsort, "latest") ? " selected" : "");
    if (M.bsearching) sb_append(&hc, "<span class=\"searching-indicator svelte-111gya4\">Searching market...</span>");
    char *controls = sb_finish(&hc);
    const FilterView v = {.title = "Listings", .count = browse_groups(), .default_sort = "name", .search_in_header = 1, .season = 1,
                          .wear = 1, .checkboxes = 8, .header_controls = controls};
    filter_header(sb, &M.f, &v);
    free(controls);
    sb_append(sb, " <div id=\"v10mkGridHolder\">");
    browse_grid(sb);
    sb_append(sb, "</div>");
}

// the filtered + sorted skin indices of a client-side view (inventory, sales, stats, catalog)
typedef struct {
    int *idx;
    int n;
} IndexList;

static void list_push(IndexList *l, const int v, int *cap) {
    if (l->n == *cap) {
        *cap = *cap ? *cap * 2 : 64;
        int *g = realloc(l->idx, sizeof(int) * (size_t) *cap);
        if (!g) return;
        l->idx = g;
    }
    l->idx[l->n++] = v;
}

static IndexList inventory_list(void) {
    IndexList l = {0};
    int cap = 0;
    if (M.f.only_missing) {
        for (int i = 0; i < g_viewer_skin_count; i++) {
            const ViewerSkin *s = skin(i);
            if (!s || (s->flags & VS_BLOCKED) || inv_count(i) > 0 || !filter_match(&M.f, s)) continue;
            list_push(&l, i, &cap);
        }
    } else {
        const cJSON *r;
        cJSON_ArrayForEach(r, M.inv) {
            const int i = jint(r, "i", -1);
            const ViewerSkin *s = skin(i);
            if (!s || jint(r, "c", 1) <= 0) continue;
            if (M.f.hide_non_trade && jint(r, "nt", 0)) continue;
            if (!filter_match(&M.f, s)) continue;
            list_push(&l, i, &cap);
        }
    }
    g_sort_key = M.f.sort;
    if (l.n) qsort(l.idx, (size_t) l.n, sizeof(int), cmp_skins);
    return l;
}

static void inventory_grid(StrBuf *sb, const IndexList *l) {
    sb_append(sb, "<div class=\"market-grid svelte-18k2d4k\" id=\"v10mkGrid\">");
    for (int k = 0; k < l->n && k < M.gshown; k++) {
        const int i = l->idx[k];
        const cJSON *r = inv_row(i);
        const int cnt = r ? jint(r, "c", 1) : 0;
        const int sellable = cnt > 0 && r && !jint(r, "nt", 0);
        sb_appendf(sb, "<div class=\"item-wrapper svelte-18k2d4k%s\">", M.detail == i ? " selected" : "");
        const CardOpts o = {.count = cnt, .list = sellable, .locked = cnt > 0 && !sellable};
        market_card(sb, i, &o);
        sb_append(sb, " ");
        const int qs = r ? jint(r, "qs", 0) : 0, destroy = r && (jint(r, "nqs", 0) || qs <= 0);
        if (cnt > 0 && (qs > 0 || destroy)) {
            const ViewerSkin *s = skin(i);
            sb_append(sb, " <button type=\"button\" class=\"quick-sell-icon svelte-18k2d4k\" aria-label=\"Quick sell ");
            sb_append_esc(sb, s && s->name ? s->name : "");
            sb_appendf(sb, "\" id=\"v10mkQS%d\" onclick=\"v10mk.qs(%d)\"><i class=\"material-icons svelte-18k2d4k\">sell</i></button> ", i, i);
        }
        sb_append(sb, "</div>");
    }
    sb_append(sb, "</div> ");
    if (l->n > M.gshown) sb_append(sb, "<div class=\"market-grid-sentinel svelte-18k2d4k\"><button class=\"load-more-btn svelte-zhgogl\" onclick=\"v10mk.more()\">Load More</button></div> ");
    if (!l->n) sb_append(sb, "<div class=\"empty-message svelte-18k2d4k\">No items in inventory</div>");
}

static void view_inventory(StrBuf *sb) {
    if (!logged_in()) {
        sb_append(sb, "<div class=\"empty-message svelte-htwh29\">Please log in to view your inventory</div>");
        return;
    }
    IndexList l = inventory_list();
    const FilterView v = {.title = "Inventory", .count = l.n, .default_sort = "rarity", .search_in_header = 0, .season = 1, .sort_select = 1,
                          .wear = 1, .checkboxes = 31};
    filter_header(sb, &M.f, &v);
    sb_append(sb, " <div id=\"v10mkGridHolder\">");
    inventory_grid(sb, &l);
    sb_append(sb, "</div>");
    free(l.idx);
}

static void sales_grid(StrBuf *sb, int *count_out) {
    int n = 0;
    sb_append(sb, "<div class=\"market-grid svelte-1l3mtrv\" id=\"v10mkGrid\">");
    // the filter header sorts the entries; sales keep their order among equal keys (newest first)
    const int total = rows_count(M.sales);
    int *idx = calloc((size_t) (total ? total : 1), sizeof(int));
    for (int k = 0; idx && k < total; k++) idx[k] = k;
    for (int a = 1; idx && a < total; a++) { // stable insertion sort by the header's key
        for (int b = a; b > 0; b--) {
            const ViewerSkin *sa = skin(jint(cJSON_GetArrayItem(M.sales, idx[b - 1]), "i", -1)), *sbk = skin(jint(cJSON_GetArrayItem(M.sales, idx[b]), "i", -1));
            if (!sa || !sbk) break;
            int d = 0;
            if (!strcmp(M.f.sort, "name")) d = strcmp(sa->name ? sa->name : "", sbk->name ? sbk->name : "");
            else if (!strcmp(M.f.sort, "rarity")) d = sbk->rarity - sa->rarity;
            else if (!strcmp(M.f.sort, "season")) d = (sbk->seas ? sbk->seas : 1) - (sa->seas ? sa->seas : 1);
            else if (!strcmp(M.f.sort, "type")) d = sa->type - sbk->type;
            if (d <= 0) break;
            const int t = idx[b];
            idx[b] = idx[b - 1];
            idx[b - 1] = t;
        }
    }
    for (int k = 0; idx && k < total; k++) {
        const cJSON *r = cJSON_GetArrayItem(M.sales, idx[k]);
        const int i = jint(r, "i", -1);
        if (!filter_match(&M.f, skin(i))) continue;
        n++;
        sb_appendf(sb, "<div class=\"item-wrapper svelte-1l3mtrv%s\">", M.detail == i ? " selected" : "");
        const CardOpts o = {.sale_id = jint(r, "m", 0), .price = jint(r, "f", 0), .listed_ms = jnum(r, "ms")};
        market_card(sb, i, &o);
        sb_append(sb, " </div>");
    }
    free(idx);
    sb_append(sb, "</div> ");
    if (!total) sb_append(sb, "<div class=\"empty-message svelte-1l3mtrv\">No active listings</div>");
    if (count_out) *count_out = n;
}

static void view_sales(StrBuf *sb) {
    if (!logged_in()) {
        sb_append(sb, "<div class=\"empty-message svelte-3m1nvd\">Please log in to view your sales</div>");
        return;
    }
    StrBuf grid = {0};
    int n = 0;
    sales_grid(&grid, &n);
    char extra[96];
    // v10: max 20 (40 with premium); here the server's maxListings (doc/v7/economy.md)
    snprintf(extra, sizeof(extra), "<span class=\"max-badge svelte-3m1nvd\">max %d</span>", (int) g_econ_rules.max_listings);
    const FilterView v = {.title = "My Sales", .count = n, .default_sort = "rarity", .season = 1, .sort_select = 1, .checkboxes = 8, .title_extra = extra};
    filter_header(sb, &M.f, &v);
    char *g = sb_finish(&grid);
    sb_append(sb, " <div id=\"v10mkGridHolder\">");
    sb_append(sb, g);
    sb_append(sb, "</div>");
    free(g);
}

static int cmp_hist_col(const cJSON *a, const cJSON *b) {
    if (!strcmp(M.hcol, "amount")) {
        const double x = jnum(a, "amount"), y = jnum(b, "amount");
        return x < y ? -1 : x > y;
    }
    if (!strcmp(M.hcol, "item")) {
        const ViewerSkin *x = skin(jint(a, "skinindex", -1)), *y = skin(jint(b, "skinindex", -1));
        return strcmp(x && x->name ? x->name : "", y && y->name ? y->name : "");
    }
    const double x = jnum(a, "date"), y = jnum(b, "date");
    return x < y ? -1 : x > y;
}

static void view_history(StrBuf *sb) {
    if (!logged_in()) {
        sb_append(sb, "<div class=\"empty-message svelte-23mnz9\">Please log in to view your history</div>");
        return;
    }
    static const struct { const char *key, *label, *width, *align; int sortable; } cols[6] = {
        {"type", "Type", "80px", NULL, 0}, {"date", "Date", "200px", NULL, 1}, {"item", "Item", NULL, NULL, 1},
        {"amount", "Amount", "120px", "right", 1}, {"who", "Who", NULL, NULL, 0}, {"status", "Status", "100px", NULL, 0},
    };
    sb_append(sb, "<div class=\"history-container svelte-23mnz9\"><div class=\"table-wrapper svelte-69sryq\"><div class=\"table-container svelte-69sryq\"><table class=\"data-table svelte-69sryq\"><thead class=\"svelte-69sryq\"><tr class=\"svelte-69sryq\">");
    for (int c = 0; c < 6; c++) {
        const int sorted = cols[c].sortable && !strcmp(M.hcol, cols[c].key);
        sb_appendf(sb, "<th class=\"table-header svelte-69sryq%s%s%s\" tabindex=\"%d\" style=\"", cols[c].sortable ? " sortable" : "",
                   sorted ? (M.hasc ? " sort-asc" : " sort-desc") : "", cols[c].align ? " align-right" : "", cols[c].sortable ? 0 : -1);
        if (cols[c].width) sb_appendf(sb, "width: %s; ", cols[c].width);
        sb_appendf(sb, "text-align: %s;\"", cols[c].align ? cols[c].align : "left");
        if (cols[c].sortable) sb_appendf(sb, " onclick=\"v10mk.hsort('%s')\"", cols[c].key);
        sb_appendf(sb, ">%s</th>", cols[c].label);
    }
    sb_append(sb, "</tr></thead><tbody class=\"svelte-69sryq\">");
    const int n = rows_count(M.hist);
    const cJSON **rows = calloc((size_t) (n ? n : 1), sizeof(*rows));
    for (int i = 0; rows && i < n; i++) rows[i] = cJSON_GetArrayItem(M.hist, i);
    for (int a = 1; rows && a < n; a++) // stable sort on the column
        for (int b = a; b > 0; b--) {
            int d = cmp_hist_col(rows[b - 1], rows[b]);
            if (!M.hasc) d = -d;
            if (d <= 0) break;
            const cJSON *t = rows[b];
            rows[b] = rows[b - 1];
            rows[b - 1] = t;
        }
    for (int i = 0; rows && i < n; i++) {
        const cJSON *r = rows[i];
        const int sk = jint(r, "skinindex", -1);
        const ViewerSkin *s = skin(sk);
        sb_appendf(sb, "<tr class=\"svelte-69sryq clickable\" tabindex=\"0\" onclick=\"v10mk.item(%d)\">", sk);
        sb_appendf(sb, "<td class=\"table-cell svelte-69sryq\" style=\"text-align: left;\">%s</td>", !strcmp(jstr(r, "type"), "buy") ? "Buy" : "Sell");
        const double ms = jnum(r, "date");
        char date[64] = "\xe2\x80\x94";
        if (ms > 0) {
            const time_t t = (time_t) (ms / 1000.0);
            struct tm tmv;
#ifdef _WIN32
            localtime_s(&tmv, &t);
#else
            localtime_r(&t, &tmv);
#endif
            // toLocaleDateString() + ' ' + toLocaleTimeString() (en-US)
            const int h12 = tmv.tm_hour % 12 ? tmv.tm_hour % 12 : 12;
            snprintf(date, sizeof(date), "%d/%d/%d %d:%02d:%02d %s", tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_year + 1900, h12, tmv.tm_min, tmv.tm_sec,
                     tmv.tm_hour < 12 ? "AM" : "PM");
        }
        sb_appendf(sb, "<td class=\"table-cell svelte-69sryq\" style=\"text-align: left;\">%s</td><td class=\"table-cell svelte-69sryq\" style=\"text-align: left;\">", date);
        if (s && s->name) sb_append_esc(sb, s->name);
        else sb_appendf(sb, "Item %d", sk);
        char amt[32];
        fmt(amt, sizeof(amt), jnum(r, "amount"));
        sb_appendf(sb, "</td><td class=\"table-cell svelte-69sryq\" style=\"text-align: right;\">%s KR</td><td class=\"table-cell svelte-69sryq\" style=\"text-align: left;\">", amt);
        if (jstr(r, "who")[0]) sb_append_esc(sb, jstr(r, "who"));
        else sb_append(sb, "\xe2\x80\x94");
        sb_appendf(sb, "</td><td class=\"table-cell svelte-69sryq\" style=\"text-align: left;\">%s</td></tr>", !strcmp(jstr(r, "status"), "completed") ? "Completed" : "Cancelled");
    }
    free(rows);
    sb_append(sb, "</tbody></table> <div class=\"table-empty-slot svelte-69sryq\">");
    if (!M.hist_loading && !n) sb_append(sb, "<div class=\"empty-history svelte-23mnz9\">No transaction history found</div>");
    sb_append(sb, "</div></div></div> </div>");
}

static void view_stats(StrBuf *sb) {
    StrBuf grid = {0};
    int shown = 0;
    sb_append(&grid, "<div class=\"stats-notice svelte-1tteuk0\">Stats update every 120 minutes</div> <div class=\"stats-grid svelte-1tteuk0\">");
    const int total = rows_count(M.stats);
    int *idx = calloc((size_t) (total ? total : 1), sizeof(int));
    for (int k = 0; idx && k < total; k++) idx[k] = jint(cJSON_GetArrayItem(M.stats, k), "ski", -1);
    g_sort_key = M.f.sort;
    if (idx && total) qsort(idx, (size_t) total, sizeof(int), cmp_skins);
    for (int k = 0; idx && k < total; k++) {
        const ViewerSkin *s = skin(idx[k]);
        if (!filter_match(&M.f, s)) continue;
        const cJSON *r = NULL, *it;
        cJSON_ArrayForEach(it, M.stats) if (jint(it, "ski", -1) == idx[k]) r = it;
        shown++;
        const int sellable = !skin_bound(s), on_sale = sellable && jint(r, "onS", 0) > 0;
        char a[32], b[32], c[32], d[32];
        fmt(a, sizeof(a), jnum(r, "inC"));
        fmt(b, sizeof(b), jnum(r, "onS"));
        fmt(c, sizeof(c), jnum(r, "low"));
        fmt(d, sizeof(d), jnum(r, "high"));
        char onclick[32];
        snprintf(onclick, sizeof(onclick), "v10mk.item(%d)", idx[k]);
        sb_appendf(&grid, "<div class=\"stat-card svelte-1tteuk0\" onclick=\"%s\">", onclick);
        item_card(&grid, s, "200px", 0, NULL, 0);
        sb_append(&grid, "</div>");
        sb_appendf(&grid, " <div class=\"stat-details svelte-1tteuk0\"><div class=\"stat-row svelte-1tteuk0\"><span class=\"stat-label svelte-1tteuk0\">Circulation:</span> <span class=\"stat-value svelte-1tteuk0\">%s</span></div>"
                          " <div class=\"stat-row svelte-1tteuk0\"><span class=\"stat-label svelte-1tteuk0\">On Sale:</span> <span class=\"stat-value svelte-1tteuk0%s\">%s</span></div>",
                   a, on_sale ? "" : " not-available", !sellable ? "Cannot be sold" : on_sale ? b : "Not on sale");
        sb_appendf(&grid, " <div class=\"stat-row svelte-1tteuk0\"><span class=\"stat-label svelte-1tteuk0\">Low:</span> <span class=\"stat-value svelte-1tteuk0%s\">",
                   on_sale ? " price" : " not-available");
        if (!sellable) sb_append(&grid, "Cannot be sold");
        else if (on_sale) sb_appendf(&grid, "%s KR", c);
        else sb_append(&grid, "Not on sale");
        sb_appendf(&grid, "</span></div> <div class=\"stat-row svelte-1tteuk0\"><span class=\"stat-label svelte-1tteuk0\">High:</span> <span class=\"stat-value svelte-1tteuk0%s\">",
                   on_sale ? " price" : " not-available");
        if (!sellable) sb_append(&grid, "Cannot be sold");
        else if (on_sale) sb_appendf(&grid, "%s KR", d);
        else sb_append(&grid, "Not on sale");
        sb_append(&grid, "</span></div></div></div>");
    }
    free(idx);
    sb_append(&grid, "</div> ");
    if (!total) sb_append(&grid, "<div class=\"empty-message svelte-1tteuk0\">No statistics available</div>");
    const FilterView v = {.title = "Market Stats", .count = shown, .default_sort = "rarity", .season = 1, .sort_select = 1};
    filter_header(sb, &M.f, &v);
    char *g = sb_finish(&grid);
    sb_append(sb, " <div id=\"v10mkGridHolder\">");
    sb_append(sb, g);
    sb_append(sb, "</div>");
    free(g);
}

// ------------------------------------------------------------------ Listing Details (svelte-olnzsm)

// _5136: the popup button (svelte-whnh40)
static void button_id(StrBuf *sb, const char *variant, const int slim, const int disabled, const char *onclick, const char *text, const char *id);

static void button(StrBuf *sb, const char *variant, const int slim, const int disabled, const char *onclick, const char *text) {
    button_id(sb, variant, slim, disabled, onclick, text, NULL);
}

static void button_id(StrBuf *sb, const char *variant, const int slim, const int disabled, const char *onclick, const char *text, const char *id) {
    sb_appendf(sb, "<button class=\"main-button svelte-whnh40%s%s%s%s\" type=\"button\"%s", variant && variant[0] ? " main-button-" : "",
               variant ? variant : "", slim ? " main-button-slim" : "", disabled ? " main-button-disabled" : "", disabled ? " disabled" : "");
    if (id) sb_appendf(sb, " id=\"%s\"", id);
    if (onclick && !disabled) sb_appendf(sb, " onclick=\"%s\"", onclick);
    sb_append(sb, ">");
    sb_append_esc(sb, text);
    sb_append(sb, "</button>");
}

// _44642: the tabs strip (svelte-1o3qul6)
static void tabs_strip(StrBuf *sb, const char *const *labels, const int n, const int sel, const unsigned disabled_mask, const char *handler) {
    sb_append(sb, "<div class=\"tabs-container svelte-1o3qul6\" role=\"tablist\" style=\"margin-bottom: 0px;\">");
    for (int i = 0; i < n; i++) {
        const int dis = (disabled_mask >> i) & 1;
        sb_appendf(sb, "<div class=\"tab svelte-1o3qul6%s%s\" id=\"v10mkTab%d\" role=\"tab\" aria-selected=\"%s\" tabindex=\"%d\" onmouseenter=\"playTick(0.1)\"", i == sel ? " tab-active" : "",
                   dis ? " tab-disabled" : "", i, i == sel ? "true" : "false", dis ? -1 : 0);
        if (!dis) sb_appendf(sb, " onclick=\"%s(%d)\"", handler, i);
        sb_appendf(sb, ">%s</div>", labels[i]);
    }
    sb_append(sb, "</div>");
}

// The sales series of the last `days` local days from ["gd"] rows {d: "YYYY-MM-DD", f: KR, t: units}
typedef struct {
    int n;
    double price[400], units[400];
    char label[400][12];
} Series;

static void build_series(Series *s, const cJSON *rows, const int days) {
    static const char *const mon[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    s->n = days < 400 ? days : 400;
    const time_t now = time(NULL);
    for (int i = 0; i < s->n; i++) {
        const time_t t = now - (time_t) (s->n - 1 - i) * 86400;
        struct tm tmv;
#ifdef _WIN32
        localtime_s(&tmv, &t);
#else
        localtime_r(&t, &tmv);
#endif
        char key[16];
        snprintf(key, sizeof(key), "%04d-%02d-%02d", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
        snprintf(s->label[i], sizeof(s->label[i]), "%02d %s %02d", tmv.tm_mday, mon[tmv.tm_mon], tmv.tm_year % 100);
        s->price[i] = s->units[i] = 0;
        const cJSON *r;
        cJSON_ArrayForEach(r, rows) {
            if (strcmp(jstr(r, "d"), key) != 0) continue;
            const double units = jnum(r, "t"), kr = jnum(r, "f");
            s->units[i] = units;
            s->price[i] = units > 0 ? floor(kr / units) : 0;
        }
    }
}

// _36123 (Chart.js line chart) as an SVG image: the native engine has no <canvas> 2D context for
// pages; nanosvg draws no text, so the axis labels are HTML next to it (KrunkNative)
static void line_chart(StrBuf *sb, const double *v, const int n, const char *fill, const char *line, const char *unit, char (*labels)[12]) {
    double max = 0;
    for (int i = 0; i < n; i++) if (v[i] > max) max = v[i];
    const double top = max > 0 ? max : 1;
    StrBuf svg = {0};
    sb_appendf(&svg, "<svg xmlns='http://www.w3.org/2000/svg' width='400' height='170' viewBox='0 0 400 170' preserveAspectRatio='none'>"
                     "<path d='M0 170");
    for (int i = 0; i < n; i++) sb_appendf(&svg, " L%.2f %.2f", n > 1 ? 400.0 * i / (n - 1) : 0.0, 170.0 - 160.0 * v[i] / top);
    sb_appendf(&svg, " L400 170 Z' fill='%s'/><path d='", fill);
    for (int i = 0; i < n; i++) sb_appendf(&svg, "%s%.2f %.2f", i ? " L" : "M", n > 1 ? 400.0 * i / (n - 1) : 0.0, 170.0 - 160.0 * v[i] / top);
    sb_appendf(&svg, "' fill='none' stroke='%s' stroke-width='2'/></svg>", line);
    char *text = sb_finish(&svg);
    char mx[32];
    fmt(mx, sizeof(mx), max);
    sb_append(sb, "<div style=\"position: relative; width: 100%; height: 100%;\"><img style=\"position: absolute; left: 34px; right: 0; top: 10px; width: calc(100% - 34px); height: 160px;\" alt=\"\" src=\"");
    svg_data_url(sb, text);
    free(text);
    sb_appendf(sb, "\"><span style=\"position: absolute; left: 0; top: 4px; font-size: 10px; color: #444;\">%s</span><span style=\"position: absolute; left: 0; top: 160px; font-size: 10px; color: #444;\">0</span>", mx, unit);
    if (n > 0) {
        sb_appendf(sb, "<span style=\"position: absolute; left: 34px; bottom: 0; font-size: 10px; color: #666;\">%s</span>", labels[0]);
        sb_appendf(sb, "<span style=\"position: absolute; right: 0; bottom: 0; font-size: 10px; color: #666;\">%s</span>", labels[n - 1]);
    }
    sb_append(sb, "</div>");
}

static void trend_text(StrBuf *sb, const double trend) {
    sb_appendf(sb, "<div class=\"stat-trending svelte-olnzsm\" style=\"background-color: %s\">%s %.0f%%</div>", trend >= 0 ? "#7ed633" : "#F44336",
               trend > 0 ? "+" : trend == 0 ? "~" : "-", fabs(trend));
}

static void detail_stats(StrBuf *sb) {
    static const int days[5] = {7, 14, 30, 60, 90};
    sb_appendf(sb, "<div class=\"stats-section svelte-olnzsm\"><div class=\"stats-controls svelte-olnzsm\"><label for=\"days-back-select\" class=\"days-back-label svelte-olnzsm\">Time Range:</label> "
                   "<select id=\"days-back-select\" class=\"days-back-select svelte-olnzsm\"%s onchange=\"v10mk.days(this.value)\">", M.dgd_loading ? " disabled" : "");
    for (int i = 0; i < 5; i++) sb_appendf(sb, "<option value=\"%d\"%s>%d days</option>", days[i], M.days == days[i] ? " selected" : "", days[i]);
    sb_append(sb, "</select></div> ");
    if (M.lost & LOST_DGD) {
        sb_appendf(sb, "<div class=\"stats-loading svelte-olnzsm\"><span>%s</span></div></div>", I18N("kn.mk.statsFailed", "Couldn't load statistics. Open the item again to retry."));
        return;
    }
    if (M.dgd_loading || !M.dgd) {
        sb_append(sb, "<div class=\"stats-loading svelte-olnzsm\"><div class=\"loading-spinner svelte-olnzsm\"></div> <span>Loading statistics...</span></div></div>");
        return;
    }
    if (!rows_count(M.dgd)) {
        sb_append(sb, "<div class=\"empty-stats svelte-olnzsm\"><div>No sales data available for this item</div></div></div>");
        return;
    }
    Series *s = calloc(1, sizeof(Series));
    if (!s) return;
    build_series(s, M.dgd, M.days);
    double units = 0, price_sum = 0;
    int first = -1, last = -1;
    for (int i = 0; i < s->n; i++) {
        units += s->units[i];
        price_sum += s->price[i];
        if (s->units[i] > 0) {
            if (first < 0) first = i;
            last = i;
        }
    }
    const double avg = floor(price_sum / (rows_count(M.dgd) ? rows_count(M.dgd) : 1));
    double ptrend = 0, utrend = 0;
    if (first >= 0 && last != first) {
        ptrend = s->price[first] ? round((s->price[last] - s->price[first]) / s->price[first] * 100.0) : 0;
        utrend = s->units[first] ? round((s->units[last] - s->units[first]) / s->units[first] * 100.0) : 0;
    }
    char a[32], b[32];
    fmt(a, sizeof(a), units);
    fmt(b, sizeof(b), avg);
    sb_appendf(sb, "<div class=\"stats-headers svelte-olnzsm\"><div class=\"stat-header svelte-olnzsm\"><div class=\"stat-label svelte-olnzsm\">Total Sold</div> <div class=\"stat-value svelte-olnzsm\">%s</div> ", a);
    trend_text(sb, utrend);
    sb_appendf(sb, "</div> <div class=\"stat-header svelte-olnzsm\"><div class=\"stat-label svelte-olnzsm\">Average Cost</div> <div class=\"stat-value svelte-olnzsm\">%s <span style=\"color: #fff\">KR</span></div> ", b);
    trend_text(sb, ptrend);
    sb_append(sb, "</div></div> <div class=\"charts-container svelte-olnzsm\"><div class=\"chart-wrapper svelte-olnzsm\"><div class=\"chart-title svelte-olnzsm\">Average Price</div> <div class=\"chart-container svelte-olnzsm\">");
    line_chart(sb, s->price, s->n, "#85bb6555", "#85bb65", "KR", s->label);
    sb_append(sb, "</div></div> <div class=\"chart-wrapper svelte-olnzsm\"><div class=\"chart-title svelte-olnzsm\">Units Sold</div> <div class=\"chart-container svelte-olnzsm\">");
    line_chart(sb, s->units, s->n, "#fbc02d55", "#fbc02d", "Units", s->label);
    sb_append(sb, "</div></div></div></div>");
    free(s);
}

static void owners_list(StrBuf *sb, const int catalog) {
    const char *h = catalog ? "items-detail-owner" : "owner";
    if (M.lost & LOST_DVIEW) {
        sb_appendf(sb, "<div class=\"items-detail-empty svelte-dmu4q2\">%s</div>", I18N("kn.mk.ownersFailed", "Couldn't load the owners."));
        return;
    }
    if (M.dview_loading || !M.downers) {
        if (catalog) sb_append(sb, "<div class=\"items-detail-loading svelte-dmu4q2\"><i class=\"material-icons spin svelte-dmu4q2\">hourglass_empty</i></div>");
        else sb_append(sb, "<div class=\"stats-loading svelte-olnzsm\"><div class=\"loading-spinner svelte-olnzsm\"></div> <span>Loading owners...</span></div>");
        return;
    }
    if (!rows_count(M.downers)) {
        sb_appendf(sb, "<div class=\"%s\">No owners yet.</div>", catalog ? "items-detail-empty svelte-dmu4q2" : "empty-stats svelte-olnzsm");
        return;
    }
    const char *hash = catalog ? "svelte-dmu4q2" : "svelte-olnzsm";
    if (!catalog) sb_appendf(sb, "<h4 class=\"listings-title svelte-olnzsm\">Owners (%d)</h4> ", rows_count(M.downers));
    sb_appendf(sb, "<ol class=\"%s %s\">", catalog ? "items-detail-owners-list" : "owners-list", hash);
    int i = 0;
    const cJSON *o;
    cJSON_ArrayForEach(o, M.downers) {
        sb_appendf(sb, "<li class=\"%s-row %s\"><span class=\"%s-rank %s\">#%d</span> ", h, hash, h, hash, ++i);
        sb_append(sb, " ");
        if (jint(o, "su", 0)) sb_appendf(sb, "<i class=\"material-icons %s premium %s\" title=\"Subscriber\">beenhere</i>", catalog ? "items-detail-owner-badge" : "owner-badge", hash);
        sb_append(sb, " ");
        if (jint(o, "ft", 0)) sb_appendf(sb, "<i class=\"material-icons %s featured %s\" title=\"Featured\">check_circle</i>", catalog ? "items-detail-owner-badge" : "owner-badge", hash);
        sb_appendf(sb, " <button class=\"%s-name %s\" type=\"button\">", h, hash);
        sb_append_esc(sb, jstr(o, "nm"));
        char c[32];
        fmt(c, sizeof(c), jnum(o, "ct"));
        sb_appendf(sb, "</button> <span class=\"%s-count %s\">\xc3\x97%s</span></li>", h, hash, c);
    }
    sb_append(sb, "</ol>");
}

static int detail_owned(void) {
    const int n = inv_count(M.detail);
    return n > 0 ? n : account_owned(M.detail);
}

static void view_detail(StrBuf *sb) {
    const ViewerSkin *s = skin(M.detail);
    if (!s) return;
    const int owned = detail_owned();
    const cJSON *r = inv_row(M.detail);
    const int can_list = owned > 0 && !skin_bound(s) && !(r && jint(r, "nt", 0));
    sb_append(sb, "<div class=\"item-detail-container svelte-olnzsm\"><div class=\"item-detail-header svelte-olnzsm\"><button class=\"back-button svelte-olnzsm\" aria-label=\"Back\" onclick=\"v10mk.back()\">"
                  "<i class=\"material-icons svelte-olnzsm\">arrow_back</i></button> <h3 class=\"item-detail-title svelte-olnzsm\">Listing Details</h3></div> "
                  "<div class=\"content-row svelte-olnzsm\"><div class=\"item-showcase-section svelte-olnzsm\"><div class=\"showcase-container svelte-olnzsm\">");
    item_card(sb, s, "370px", 1, NULL, 0);
    if (owned > 0) sb_appendf(sb, "<div class=\"item-ownership-overlay svelte-olnzsm\"><div class=\"ownership-count svelte-olnzsm\">%d owned</div></div>", owned);
    sb_append(sb, "</div></div> <div class=\"item-info-section svelte-olnzsm\"><div class=\"item-creator svelte-olnzsm\">by  ");
    if (!s->creator || !strcmp(s->creator, "Krunker.io")) {
        sb_append(sb, "Krunker.io");
    } else { // "a, b" -> buttons
        char buf[256];
        snprintf(buf, sizeof(buf), "%s", s->creator);
        int k = 0;
        for (char *t = strtok(buf, ","); t; t = strtok(NULL, ",")) {
            while (*t == ' ') t++;
            if (!*t) continue;
            if (k++) sb_append(sb, ", ");
            sb_append(sb, "<button type=\"button\" class=\"creator-link svelte-olnzsm\">");
            sb_append_esc(sb, t);
            sb_append(sb, "</button>");
        }
    }
    sb_append(sb, "</div>  <div class=\"full-details-container svelte-olnzsm\"><button type=\"button\" class=\"full-details-btn svelte-olnzsm\" onclick=\"v10mk.info()\"><i class=\"material-icons svelte-olnzsm\">info</i> <span>Item Info</span></button></div> ");
    if (can_list) {
        sb_append(sb, "<div class=\"list-button-container svelte-olnzsm\">");
        char oc[32];
        snprintf(oc, sizeof(oc), "v10mk.list(%d)", M.detail);
        button(sb, "accept", 1, M.busy, oc, "List Item");
        sb_append(sb, "</div>");
    }
    sb_append(sb, "</div></div> <div class=\"tabs-content-section svelte-olnzsm\"><div class=\"tabs-section svelte-olnzsm\">");
    static const char *const dtabs[3] = {"Listings", "Stats", "Owners"};
    tabs_strip(sb, dtabs, 3, M.dtab, 0, "v10mk.dtab");
    sb_append(sb, "</div> <div class=\"tab-content svelte-olnzsm\">");
    if (M.dtab == 0) {
        const AccountInfo *a = acc();
        int n = 0;
        const cJSON *l;
        cJSON_ArrayForEach(l, M.dlist) if (!a || strcmp(jstr(l, "p"), a->name) != 0) n++; // "Items sold by you are not shown here"
        sb_appendf(sb, "<div class=\"listings-section svelte-olnzsm\"><h4 class=\"listings-title svelte-olnzsm\">Available Listings (%d)</h4> ", n);
        if (!n) {
            sb_append(sb, "<div class=\"empty-listings svelte-olnzsm\"><div class=\"svelte-olnzsm\">No listings available for this item</div> <div class=\"empty-note svelte-olnzsm\">Items sold by you are not shown here</div></div>");
        } else {
            sb_append(sb, "<div class=\"listings-list svelte-olnzsm\">");
            cJSON_ArrayForEach(l, M.dlist) { // the brain sends them cheapest first
                if (a && !strcmp(jstr(l, "p"), a->name)) continue;
                char p[32];
                fmt(p, sizeof(p), jnum(l, "f"));
                sb_appendf(sb, "<div class=\"listing-item svelte-olnzsm\"><div class=\"listing-info svelte-olnzsm\"><div class=\"listing-price svelte-olnzsm\">%s KR    </div> ", p);
                if (jstr(l, "p")[0]) {
                    sb_append(sb, "<div class=\"listing-seller svelte-olnzsm\">Seller:  <button type=\"button\" class=\"seller-link svelte-olnzsm\">");
                    sb_append_esc(sb, jstr(l, "p"));
                    sb_append(sb, "</button></div>");
                }
                sb_append(sb, " ");
                const double ms = jnum(l, "ms");
                if (ms > 0) {
                    const time_t t = (time_t) (ms / 1000.0);
                    struct tm tmv;
#ifdef _WIN32
                    localtime_s(&tmv, &t);
#else
                    localtime_r(&t, &tmv);
#endif
                    sb_appendf(sb, "<div class=\"listing-time svelte-olnzsm\">Listed: %d/%d/%d</div>", tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_year + 1900);
                }
                sb_append(sb, "</div> ");
                if (a) {
                    char oc[64];
                    snprintf(oc, sizeof(oc), "v10mk.buy(%d,%d,%d)", jint(l, "m", 0), M.detail, jint(l, "f", 0));
                    button(sb, "accept", 1, M.busy, oc, "Purchase");
                }
                sb_append(sb, "</div>");
            }
            sb_append(sb, "</div>");
        }
        sb_append(sb, "</div>");
    } else if (M.dtab == 1) {
        detail_stats(sb);
    } else {
        sb_append(sb, "<div class=\"owners-section svelte-olnzsm\">");
        owners_list(sb, 0);
        sb_append(sb, "</div>");
    }
    sb_append(sb, "</div></div></div></div>");
}

static void view_market(StrBuf *sb) {
    const int detail = M.detail >= 0;
    sb_appendf(sb, "<div class=\"io-popup-content market-container svelte-1ugt6n9\"%s>", detail ? " style=\"display: none;\"" : "");
    tabs_strip(sb, k_mtabs, MT_COUNT, M.mtab, logged_in() ? 0u : 0xEu, "v10mk.mtab");
    sb_append(sb, "  <div class=\"content-wrapper svelte-1ugt6n9\"><div class=\"tab-content svelte-1ugt6n9\" id=\"v10mkScroll\">");
    switch (M.mtab) {
        case MT_BROWSE: view_browse(sb); break;
        case MT_INVENTORY: view_inventory(sb); break;
        case MT_SALES: view_sales(sb); break;
        case MT_HISTORY: view_history(sb); break;
        default: view_stats(sb); break;
    }
    sb_append(sb, " ");
    if (M.busy) sb_append(sb, g_loading_overlay);
    sb_append(sb, "</div></div></div> ");
    if (detail) view_detail(sb);
}

// ------------------------------------------------------------------ modals

static const char *list_error(const ViewerSkin *s, const char *text, int *valid) {
    static char err[96];
    *valid = 0;
    err[0] = 0;
    if (!text[0]) return NULL;
    char *end;
    const long p = strtol(text, &end, 10);
    if (end == text) return NULL;
    const int min = list_min_price(s);
    char n[32];
    if (p < min) {
        fmt(n, sizeof(n), min);
        snprintf(err, sizeof(err), "Minimum price is %s KR", n);
        return err;
    }
    if (p > SALE_MAX_KR) return "Maximum price is 10,000,000 KR";
    *valid = 1;
    return NULL;
}

static void list_modal_dynamic(StrBuf *sb) {
    const ViewerSkin *s = skin(M.sel_skin);
    int valid = 0;
    const char *err = list_error(s, M.price_in, &valid);
    if (err) {
        sb_append(sb, "<div class=\"price-error svelte-1pf0608\">");
        sb_append_esc(sb, err);
        sb_append(sb, "</div>");
    }
    sb_append(sb, " ");
    if (M.price_in[0] && valid) {
        // KrunkNative: the fee the server charges (marketFee %, rounded down; v10 shows ceil(10 %))
        char fee[32], label[48];
        fmt(fee, sizeof(fee), econ_list_fee(atof(M.price_in)));
        snprintf(label, sizeof(label), "Listing Fee (%d%%):", (int) g_econ_rules.market_fee);
        sb_appendf(sb, "<div class=\"fee-breakdown svelte-1pf0608\"><div class=\"fee-row svelte-1pf0608\"><span class=\"svelte-1pf0608\">%s</span> <span class=\"fee-value fee-cost svelte-1pf0608\">%s KR</span></div></div>", label, fee);
    }
    sb_append(sb, " <div class=\"modal-actions svelte-1pf0608\">");
    button(sb, NULL, 0, M.busy, "v10mk.mclose()", "Cancel");
    sb_append(sb, " ");
    button_id(sb, "accept", 0, M.busy || !valid, "v10mk.mok()", "List", "v10mkModalOk");
    sb_append(sb, "</div>");
}

static void qs_values(const int index, int *kr, int *can_kr, int *can_destroy) {
    const cJSON *r = inv_row(index);
    *kr = r ? jint(r, "qs", 0) : 0;
    *can_kr = *kr > 0 && !(r && jint(r, "nqs", 0));
    *can_destroy = !*can_kr; // noQuickSell or no KR value (Black Market items): destroy only (economy.c)
}

// the Quick Sell window (_46234), pushed on the shared v10 view stack (v10_ui.h)
static void qs_html(StrBuf *sb) {
    char n[32];
    const ViewerSkin *s = skin(M.sel_skin);
    if (!s) return;
    int kr, can_kr, can_destroy;
    qs_values(M.sel_skin, &kr, &can_kr, &can_destroy);
    const int count = M.sel_count;
    char url[512];
    thumb(url, sizeof(url), s);
    const char *col = rarity_color(s);
    sb_appendf(sb, "<div class=\"qs-overlay svelte-1o11rsf\" role=\"presentation\" onclick=\"v10mk.mclose()\"><div class=\"qs-window svelte-1o11rsf\" role=\"dialog\" aria-modal=\"true\" aria-label=\"Quick sell\" onclick=\"v10mk.stop()\">"
                   "<div class=\"qs-title svelte-1o11rsf\">%s</div> <div class=\"qs-item-preview svelte-1o11rsf\" style=\"border-color: %s;\">", can_destroy ? "Destroy Item" : "Quick Sell", col);
    if (s->type == 4) sb_appendf(sb, "<div class=\"qs-thumb qs-thumb-spray svelte-1o11rsf\" style=\"background-image: url(%s)\"></div>", url);
    else {
        sb_appendf(sb, "<img class=\"qs-thumb svelte-1o11rsf\" src=\"%s\" alt=\"", url);
        sb_append_esc(sb, s->name ? s->name : "Item");
        sb_append(sb, "\">");
    }
    sb_appendf(sb, " <div class=\"qs-item-info svelte-1o11rsf\"><div class=\"qs-item-name svelte-1o11rsf\" style=\"color: %s;\">", col);
    sb_append_esc(sb, s->name ? s->name : "Unnamed item");
    sb_append(sb, "</div> ");
    if (count > 1) sb_appendf(sb, "<div class=\"qs-item-sub svelte-1o11rsf\">%d owned</div>", count);
    sb_append(sb, "</div></div> ");
    if (can_destroy) {
        sb_append(sb, "<div class=\"qs-no-value svelte-1o11rsf\">This item has no value. You can destroy it to free up inventory.</div>");
    } else {
        fmt(n, sizeof(n), kr);
        sb_appendf(sb, "<div class=\"qs-values svelte-1o11rsf\"><div class=\"qs-value-row svelte-1o11rsf\"><span class=\"qs-value-label svelte-1o11rsf\">KR</span> "
                       "<span class=\"qs-value-each svelte-1o11rsf\"><span class=\"qs-value-num svelte-1o11rsf\">%s</span> KR each</span></div> </div>", n);
    }
    sb_append(sb, " ");
    if (count > 1) {
        sb_appendf(sb, "<label class=\"qs-amount-label svelte-1o11rsf\" for=\"qs-amount-select\">Amount</label> <select id=\"qs-amount-select\" class=\"qs-amount-select svelte-1o11rsf\"%s onchange=\"v10mk.qsmode(this.value)\">"
                       "<option value=\"0\"%s>1 Item</option><option value=\"1\"%s>All But 1</option><option value=\"2\"%s>All</option></select> ",
                   M.busy ? " disabled" : "", M.qs_mode == 0 ? " selected" : "", M.qs_mode == 1 ? " selected" : "", M.qs_mode == 2 ? " selected" : "");
        const int units = M.qs_mode == 0 ? 1 : M.qs_mode == 1 ? (count - 1 > 0 ? count - 1 : 0) : count;
        if (can_kr && units > 0) {
            fmt(n, sizeof(n), (double) kr * units);
            sb_appendf(sb, "<div class=\"qs-total svelte-1o11rsf\">You will receive: <span class=\"qs-total-num svelte-1o11rsf\">%s</span> KR total (%d \xc3\x97)  </div>", n, units);
        }
    }
    sb_append(sb, " ");
    if (M.qs_err[0]) {
        sb_append(sb, "<div class=\"qs-submit-error svelte-1o11rsf\">");
        sb_append_esc(sb, M.qs_err);
        sb_append(sb, "</div>");
    }
    sb_appendf(sb, " <div class=\"qs-actions svelte-1o11rsf\"><button type=\"button\" class=\"qs-btn qs-btn-cancel svelte-1o11rsf\"%s onclick=\"v10mk.mclose()\">Cancel</button> ", M.busy ? " disabled" : "");
    if (can_destroy)
        sb_appendf(sb, "<button type=\"button\" class=\"qs-btn qs-btn-destroy svelte-1o11rsf\" id=\"v10mkQsGo\"%s onclick=\"v10mk.qsgo(2)\">%s</button>", M.busy ? " disabled" : "", M.busy ? "Destroying\xe2\x80\xa6" : "Destroy");
    else
        sb_appendf(sb, "<button type=\"button\" class=\"qs-btn qs-btn-kr svelte-1o11rsf\" id=\"v10mkQsGo\"%s onclick=\"v10mk.qsgo(0)\">%s</button> ", M.busy ? " disabled" : "", M.busy ? "\xe2\x80\xa6" : "Get KR");
    sb_append(sb, "</div></div></div>");
}

static void modal_html(StrBuf *sb) {
    char n[32];
    switch (M.modal) {
        case MODAL_PURCHASE:
            fmt(n, sizeof(n), M.sel_price);
            sb_appendf(sb, "<div class=\"modal-overlay svelte-1ugt6n9\" role=\"dialog\" aria-modal=\"true\" aria-label=\"Purchase confirmation\" tabindex=\"-1\" onclick=\"v10mk.mclose()\">"
                           "<div class=\"modal-content svelte-1ugt6n9\" role=\"document\" onclick=\"v10mk.stop()\"><h3 class=\"svelte-1ugt6n9\">Confirm Purchase</h3> "
                           "<p class=\"svelte-1ugt6n9\">Purchase this item for %s KR?</p> <div class=\"modal-actions svelte-1ugt6n9\">", n);
            button(sb, NULL, 0, M.busy, "v10mk.mclose()", "Cancel");
            sb_append(sb, " ");
            button_id(sb, "accept", 0, M.busy, "v10mk.mok()", "Confirm", "v10mkModalOk");
            sb_append(sb, "</div></div></div>");
            break;
        case MODAL_UNLIST:
            sb_append(sb, "<div class=\"modal-overlay svelte-1ugt6n9\" role=\"dialog\" aria-modal=\"true\" aria-label=\"Unlist item confirmation\" tabindex=\"-1\" onclick=\"v10mk.mclose()\">"
                          "<div class=\"modal-content svelte-1ugt6n9\" role=\"document\" onclick=\"v10mk.stop()\"><h3 class=\"svelte-1ugt6n9\">Unlist Item</h3> "
                          "<p class=\"svelte-1ugt6n9\">Are you sure you want to remove this item from the market?</p> <div class=\"modal-actions svelte-1ugt6n9\">");
            button(sb, NULL, 0, M.busy, "v10mk.mclose()", "Cancel");
            sb_append(sb, " ");
            button_id(sb, "danger", 0, M.busy, "v10mk.mok()", "Unlist", "v10mkModalOk");
            sb_append(sb, "</div></div></div>");
            break;
        case MODAL_LIST: {
            const ViewerSkin *s = skin(M.sel_skin);
            fmt(n, sizeof(n), list_min_price(s));
            int valid = 0;
            const char *err = list_error(s, M.price_in, &valid);
            sb_append(sb, "<div class=\"modal-overlay svelte-1pf0608\" role=\"dialog\" aria-modal=\"true\" aria-label=\"List item confirmation\" tabindex=\"-1\" onclick=\"v10mk.mclose()\">"
                          "<div class=\"modal-content svelte-1pf0608\" role=\"document\" onclick=\"v10mk.stop()\"><h3 class=\"svelte-1pf0608\">List Item for Sale</h3> <p class=\"item-name-text svelte-1pf0608\">");
            sb_append_esc(sb, s && s->name ? s->name : "Item");
            sb_appendf(sb, "</p> <p class=\"svelte-1pf0608\">Enter the price to list this item:</p> <input type=\"number\" class=\"price-input svelte-1pf0608%s\" id=\"v10mkPrice\" placeholder=\"Price in KR (min: %s)\" min=\"%d\" max=\"10000000\" value=\"",
                       err ? " error" : "", n, list_min_price(s));
            sb_append_esc(sb, M.price_in);
            sb_append(sb, "\" oninput=\"v10mk.price(this.value)\" onkeydown=\"v10mk.pkey(event)\"> <div id=\"v10mkListDyn\">");
            list_modal_dynamic(sb);
            sb_append(sb, "</div></div></div>");
            break;
        }
        default:
            break;
    }
    if (M.err_open) {
        sb_append(sb, "<div class=\"modal-overlay svelte-1k428dj\" role=\"dialog\" aria-modal=\"true\" aria-label=\"Error message\" tabindex=\"-1\" onclick=\"v10mk.errok()\">"
                      "<div class=\"modal-content svelte-1k428dj\" role=\"document\" onclick=\"v10mk.stop()\"><div class=\"error-header svelte-1k428dj\"><i class=\"material-icons error-icon svelte-1k428dj\">error</i> "
                      "<h3 class=\"svelte-1k428dj\">Error</h3></div> <p class=\"error-message svelte-1k428dj\">");
        sb_append_esc(sb, M.err_msg);
        sb_append(sb, "</p> <div class=\"modal-actions svelte-1k428dj\">");
        button(sb, NULL, 0, 0, "v10mk.errok()", "OK");
        sb_append(sb, "</div></div></div>");
    }
}

// ------------------------------------------------------------------ Trading (svelte-1r9fijr)

static int trade_level_ok(void) {
    const AccountInfo *a = acc();
    // v10 hardcodes level 10 (its tradeMinLvl); here the server's rule (ecfg tradeMinLvl)
    return a && a->level >= (int) g_econ_rules.trade_min_lvl;
}

// "1,1,5," -> skin indices
static int parse_skins(const char *s, int *out, const int max) {
    int n = 0;
    while (s && *s && n < max) {
        char *end;
        const long v = strtol(s, &end, 10);
        if (end == s) break;
        out[n++] = (int) v;
        s = *end == ',' ? end + 1 : end;
    }
    return n;
}

// _46462: the trade item grid (svelte-z9tzua), compact
static void trade_grid(StrBuf *sb, const int *items, const int n, const double kr, const int fill) {
    sb_append(sb, "<div class=\"trade-item-grid svelte-z9tzua compact\">");
    for (int k = 0; k < n; k++) {
        const ViewerSkin *s = skin(items[k]);
        if (!s) continue;
        char url[512];
        thumb(url, sizeof(url), s);
        sb_appendf(sb, "<div class=\"trade-item svelte-z9tzua\" role=\"button\" tabindex=\"0\" style=\"border-color: %s;\" data-index=\"%d\" onclick=\"v10mk.citem(%d)\">", rarity_color(s), items[k], items[k]);
        if (s->type == 4) sb_appendf(sb, "<div class=\"trade-spray svelte-z9tzua\" style=\"background-image: url('%s'); \"></div>", url);
        else {
            sb_appendf(sb, "<img class=\"trade-item-img svelte-z9tzua%s\" draggable=\"false\" src=\"%s\" alt=\"", (s->flags & VS_RGB) ? " rgbHue" : "", url);
            sb_append_esc(sb, s->name ? s->name : "");
            sb_append(sb, "\">");
        }
        sb_append(sb, "   <div class=\"trade-item-paint svelte-z9tzua\"></div></div>");
    }
    const int empty = fill > 0 ? (fill - n > 0 ? fill - n : 0) : (n == 0 ? 5 : (n % 5 ? 5 - n % 5 : 0));
    sb_append(sb, " ");
    for (int k = 0; k < empty; k++) sb_append(sb, "<div class=\"trade-item trade-item-empty svelte-z9tzua\" aria-hidden=\"true\"></div>");
    sb_append(sb, " ");
    if (kr > 0) {
        char n2[32];
        fmt(n2, sizeof(n2), kr);
        sb_appendf(sb, "<div class=\"trade-currency-line svelte-z9tzua\"><span class=\"trade-currency-item svelte-z9tzua\">+%s KR</span></div>", n2);
    }
    sb_append(sb, " </div>");
}

// _46536: one offer (svelte-1agthf7). section 0 incoming, 1 outgoing, 2 history
static void offer_html(StrBuf *sb, const cJSON *t, const int section) {
    const AccountInfo *a = acc();
    const int mine = a && jint(t, "pl", 0) == a->id, trid = jint(t, "trid", 0);
    int yours[256], theirs[256];
    const int ny = parse_skins(jstr(t, mine ? "sksell" : "skbuy"), yours, 256), nt = parse_skins(jstr(t, mine ? "skbuy" : "sksell"), theirs, 256);
    const double ykr = jnum(t, mine ? "krsell" : "krbuy"), tkr = jnum(t, mine ? "krbuy" : "krsell");
    const char *name = jstr(t, mine ? "buyer" : "player");
    const int scam_give = section == 0 && (ny > 0 || ykr > 0) && nt == 0 && tkr <= 0, scam_get = section == 0 && ny == 0 && nt > 0;
    const char *msg = jstr(t, "message");
    sb_appendf(sb, "<div class=\"offer-holder svelte-1agthf7%s%s\" id=\"v10mkOffer%d\"><div class=\"offer-top svelte-1agthf7\"><div class=\"offer-from svelte-1agthf7\">",
               scam_give ? " scam-give" : "", scam_get ? " scam-get" : "", trid);
    if (msg[0]) sb_append(sb, "<span class=\"material-icons offer-msg svelte-1agthf7\">markunread</span>");
    sb_appendf(sb, " Offer %s <span class=\"offer-name svelte-1agthf7\" role=\"button\" tabindex=\"%d\"", section == 0 ? "from" : section == 1 ? "to" : "with", name[0] ? 0 : -1);
    if (name[0]) {
        sb_append(sb, " onclick=\"v10mk.tname('");
        sb_append_esc(sb, name);
        sb_append(sb, "')\"");
    }
    sb_append(sb, ">");
    sb_append_esc(sb, name[0] ? name : "Unknown");
    char age[64] = "";
    const double dte = jnum(t, "dte");
    if (dte > 0) age_text(age, sizeof(age), now_ms() - dte);
    sb_appendf(sb, "</span></div> <div class=\"offer-time svelte-1agthf7\">%s</div> ", age);
    if (section != 2) {
        sb_append(sb, "<div class=\"offer-actions svelte-1agthf7\">");
        char oc[48], id[32];
        if (section == 0) {
            snprintf(oc, sizeof(oc), "v10mk.taccept(%d)", trid);
            snprintf(id, sizeof(id), "v10mkAccept%d", trid);
            button_id(sb, "accept", 1, M.actioning == trid, oc, "Accept", id);
            sb_append(sb, " ");
            snprintf(oc, sizeof(oc), "v10mk.tdecline(%d)", trid);
            snprintf(id, sizeof(id), "v10mkDecline%d", trid);
            button_id(sb, "danger", 1, M.actioning == trid, oc, "Decline", id);
        } else {
            snprintf(oc, sizeof(oc), "v10mk.tcancel(%d)", trid);
            snprintf(id, sizeof(id), "v10mkCancel%d", trid);
            button_id(sb, "danger", 1, M.actioning == trid, oc, "Cancel", id);
        }
        sb_append(sb, "</div>");
    }
    double yv = 0, tv = 0;
    for (int k = 0; k < ny; k++) yv += social_estimate_value(yours[k]);
    for (int k = 0; k < nt; k++) tv += social_estimate_value(theirs[k]);
    const int most = ny > nt ? ny : nt, fill = most == 0 ? 5 : 5 * ((most + 4) / 5);
    char v1[32], v2[32];
    fmt(v1, sizeof(v1), yv);
    fmt(v2, sizeof(v2), tv);
    sb_appendf(sb, "</div> <div class=\"offer-items-row svelte-1agthf7\"><div class=\"offer-contents svelte-1agthf7\"><div class=\"offer-contents-label svelte-1agthf7\">Your Items [%d]</div> ", ny);
    trade_grid(sb, yours, ny, ykr, fill);
    sb_appendf(sb, " <div class=\"offer-est svelte-1agthf7\">~%s KR</div></div> <div class=\"offer-swap svelte-1agthf7\"><span class=\"material-icons svelte-1agthf7\">swap_horiz</span></div> "
                   "<div class=\"offer-contents svelte-1agthf7\"><div class=\"offer-contents-label svelte-1agthf7\">", v1);
    sb_append_esc(sb, name);
    sb_appendf(sb, "'s Items [%d]</div> ", nt);
    trade_grid(sb, theirs, nt, tkr, fill);
    sb_appendf(sb, " <div class=\"offer-est svelte-1agthf7\">~%s KR</div></div></div> ", v2);
    if (msg[0]) {
        sb_append(sb, "<div class=\"offer-message svelte-1agthf7\">\"");
        sb_append_esc(sb, msg);
        sb_append(sb, "\"</div>");
    }
    sb_append(sb, "</div>");
}

static void trades_load(void) {
    if (!logged_in() || M.trades_loading) return;
    M.trades_loading = 1;
    M.lost &= ~LOST_TRADES;
    if (!request_page("market", "trades", -1)) {
        M.trades_loading = 0;
        M.lost |= LOST_TRADES;
    }
}

static void view_trading(StrBuf *sb) {
    sb_append(sb, "<div class=\"trading-tab svelte-1r9fijr\">");
    if (!trade_level_ok()) {
        sb_appendf(sb, "<div class=\"trading-locked svelte-1r9fijr\"><i class=\"material-icons\" style=\"font-size: 48px; color: rgba(255,255,255,0.3);\">lock</i> <div>Must be level %d to trade</div></div>",
                   (int) g_econ_rules.trade_min_lvl);
    } else {
        static const char *const ttabs[2] = {"Trading", "History"};
        sb_append(sb, "<div class=\"trading-header svelte-1r9fijr\">");
        tabs_strip(sb, ttabs, 2, M.ttab, 0, "v10mk.ttab");
        sb_append(sb, " <div class=\"trading-header-actions svelte-1r9fijr\">");
        button_id(sb, "accept", 1, 0, "v10mk.tcreate()", "+ Create Trade", "v10mkCreate");
        sb_append(sb, "</div></div> <div class=\"trading-content svelte-1r9fijr\">");
        const cJSON *open = cJSON_GetArrayItem(M.trades, 0), *hist = cJSON_GetArrayItem(M.trades, 1), *t;
        const AccountInfo *a = acc();
        if (M.lost & LOST_TRADES) {
            sb_appendf(sb, "<div class=\"trading-status svelte-1r9fijr\" id=\"v10mkTradeStatus\">%s</div>", I18N("kn.mk.tradesFailed", "Couldn't load trades. Open Trading again to retry."));
        } else if (M.trades_loading || !M.trades_loaded) {
            sb_append(sb, "<div class=\"trading-status svelte-1r9fijr\" id=\"v10mkTradeStatus\">Loading trades...</div>");
        } else if (M.ttab == 0) {
            int nin = 0, nout = 0;
            cJSON_ArrayForEach(t, open) {
                if (a && jint(t, "pl", 0) == a->id) nout++;
                else nin++;
            }
            sb_append(sb, "<div class=\"trading-split svelte-1r9fijr\"><div class=\"trading-column svelte-1r9fijr\"><div class=\"trading-column-header svelte-1r9fijr\"><div class=\"trading-column-title svelte-1r9fijr\">Incoming</div> ");
            if (nin) button(sb, "danger", 1, M.actioning != 0, "v10mk.tdeclineall()", "Decline All");
            sb_append(sb, "</div> ");
            if (!nin) sb_append(sb, "<div class=\"trading-status svelte-1r9fijr\">No incoming trade offers</div>");
            else {
                sb_append(sb, "<div class=\"trade-list svelte-1r9fijr\">");
                cJSON_ArrayForEach(t, open) if (!(a && jint(t, "pl", 0) == a->id)) offer_html(sb, t, 0);
                sb_append(sb, "</div>");
            }
            sb_append(sb, "</div> <div class=\"trading-divider svelte-1r9fijr\"></div> <div class=\"trading-column svelte-1r9fijr\"><div class=\"trading-column-header svelte-1r9fijr\"><div class=\"trading-column-title svelte-1r9fijr\">Outgoing</div></div> ");
            if (!nout) sb_append(sb, "<div class=\"trading-status svelte-1r9fijr\">No outgoing trade offers</div>");
            else {
                sb_append(sb, "<div class=\"trade-list svelte-1r9fijr\">");
                cJSON_ArrayForEach(t, open) if (a && jint(t, "pl", 0) == a->id) offer_html(sb, t, 1);
                sb_append(sb, "</div>");
            }
            sb_append(sb, "</div></div>");
        } else if (!rows_count(hist)) {
            sb_append(sb, "<div class=\"trading-status svelte-1r9fijr\">No trade history</div>");
        } else {
            sb_append(sb, "<div class=\"trade-list svelte-1r9fijr\">");
            cJSON_ArrayForEach(t, hist) offer_html(sb, t, 2);
            sb_append(sb, " </div>");
        }
        sb_append(sb, "</div>");
    }
    sb_append(sb, " ");
    if (M.tnote[0]) {
        sb_appendf(sb, "<div class=\"trading-notification svelte-1r9fijr%s\">", M.tnote_err ? " error" : "");
        sb_append_esc(sb, M.tnote);
        sb_append(sb, "</div>");
    }
    sb_append(sb, "</div>");
}

// the Create Trade window (_36706 svelte-kfskmv, _36585 svelte-1lqen9w)
static int ct_max_items(void) {
    const AccountInfo *a = acc();
    return (int) (a && a->premium ? g_econ_rules.trade_max_out_p : g_econ_rules.trade_max_out);
}
static int ct_offer_count(const int side) {
    int n = 0;
    for (int k = 0; k < M.ct_n[side]; k++) n += M.ct_sel[side][k].c;
    return n;
}
static int ct_offered(const int side, const int skin_index) {
    for (int k = 0; k < M.ct_n[side]; k++) if (M.ct_sel[side][k].i == skin_index) return M.ct_sel[side][k].c;
    return 0;
}
static int ct_fee(void) {
    const AccountInfo *a = acc();
    if (a && a->premium) return 0;
    int fee = 0;
    for (int side = 0; side < 2; side++)
        for (int k = 0; k < M.ct_n[side]; k++) {
            const ViewerSkin *s = skin(M.ct_sel[side][k].i);
            if (s) fee += econ_rarity_trade_fee(s->rarity) * M.ct_sel[side][k].c;
        }
    return fee;
}

static int cmp_trade_inv(const void *pa, const void *pb) { // _9415: rarity, season, index, all descending
    const int a = jint(*(cJSON *const *) pa, "i", 0), b = jint(*(cJSON *const *) pb, "i", 0);
    const ViewerSkin *sa = skin(a), *sb = skin(b);
    if (sa && sb && sa->rarity != sb->rarity) return sb->rarity - sa->rarity;
    if (sa && sb && (sa->seas ? sa->seas : 1) != (sb->seas ? sb->seas : 1)) return (sb->seas ? sb->seas : 1) - (sa->seas ? sa->seas : 1);
    return b - a;
}

static void ct_item(StrBuf *sb, const int side, const int index, const int count, const int offer, const int locked) {
    const ViewerSkin *s = skin(index);
    if (!s) return;
    char url[512];
    thumb(url, sizeof(url), s);
    const int remaining = offer ? count : count - ct_offered(side, index);
    sb_appendf(sb, "<div class=\"%s svelte-1lqen9w%s%s\" id=\"v10mkCt%c%d_%d\" role=\"button\" tabindex=\"0\" style=\"border-color: %s;\" data-index=\"%d\" onclick=\"v10mk.%s(%d,%d)\" onmouseenter=\"playTick(0.05)\">",
               "trade-inv-item", locked ? " locked" : "", !offer && remaining <= 0 && !locked ? " depleted" : "", offer ? 'O' : 'I', side, index,
               rarity_color(s), index, offer ? "crem" : "cadd", side, index);
    if (s->type == 4) sb_appendf(sb, "<div class=\"trade-spray-img svelte-1lqen9w\" style=\"background-image: url('%s'); \"></div>", url);
    else {
        sb_appendf(sb, "<img class=\"trade-item-img svelte-1lqen9w%s\" draggable=\"false\" src=\"%s\" alt=\"", (s->flags & VS_RGB) ? " rgbHue" : "", url);
        sb_append_esc(sb, s->name ? s->name : "");
        sb_append(sb, "\">");
    }
    sb_append(sb, " <div class=\"trade-item-name svelte-1lqen9w\">");
    sb_append_esc(sb, s->name ? s->name : "");
    sb_appendf(sb, "</div> <div class=\"trade-item-paint svelte-1lqen9w\"></div> <div class=\"trade-item-count svelte-1lqen9w\">x%d</div> ", remaining);
    if (locked) sb_append(sb, "<div class=\"trade-item-locked svelte-1lqen9w\"><span class=\"material-icons\" style=\"font-size: 30px; color: #fff;\">lock</span></div>");
    sb_append(sb, "</div>");
}

static void ct_side(StrBuf *sb, const int side) {
    const cJSON *inv = cJSON_GetArrayItem(M.ct_inv, side), *other = cJSON_GetArrayItem(M.ct_inv, !side), *r;
    sb_append(sb, "<div class=\"trade-side svelte-1lqen9w\"><div class=\"trade-label inv svelte-1lqen9w\">");
    if (side == 0) sb_append(sb, "Your Inventory");
    else {
        sb_append_esc(sb, M.ct_target);
        sb_append(sb, "'s Inventory");
    }
    sb_append(sb, "</div> <div class=\"trade-inv-scroll svelte-1lqen9w\">");
    const int n = rows_count(inv);
    cJSON **rows = calloc((size_t) (n ? n : 1), sizeof(*rows));
    int k = 0;
    cJSON_ArrayForEach(r, inv) if (rows) rows[k++] = (cJSON *) r;
    if (rows && k) qsort(rows, (size_t) k, sizeof(*rows), cmp_trade_inv);
    for (int j = 0; rows && j < k; j++) {
        const int i = jint(rows[j], "i", -1), cnt = jint(rows[j], "c", 1);
        const ViewerSkin *s = skin(i);
        if (!s || cnt <= 0) continue;
        if (M.ct_missing) { // only what the other player does not own
            int has = 0;
            const cJSON *o;
            cJSON_ArrayForEach(o, other) if (jint(o, "i", -1) == i) has = 1;
            if (has) continue;
        }
        if (M.ct_search[side][0] && !ci_contains(s->name, M.ct_search[side]) && !(s->name && ci_contains(M.ct_search[side], s->name))) continue;
        ct_item(sb, side, i, cnt, 0, jint(rows[j], "nt", 0));
    }
    free(rows);
    sb_appendf(sb, "</div> <input class=\"trade-search svelte-1lqen9w\" type=\"text\" placeholder=\"Search Item\" id=\"v10mkCtSearch%d\" value=\"", side);
    sb_append_esc(sb, M.ct_search[side]);
    sb_appendf(sb, "\" oninput=\"v10mk.csearch(%d,this.value)\"> <div class=\"trade-label offer svelte-1lqen9w\">Trade List %d/%d</div> <div class=\"trade-offer-scroll svelte-1lqen9w\">",
               side, ct_offer_count(side), ct_max_items());
    double value = 0;
    for (int j = 0; j < M.ct_n[side]; j++) {
        ct_item(sb, side, M.ct_sel[side][j].i, M.ct_sel[side][j].c, 1, 0);
        value += (double) social_estimate_value(M.ct_sel[side][j].i) * M.ct_sel[side][j].c;
    }
    char v[32];
    fmt(v, sizeof(v), value);
    sb_appendf(sb, "</div> <div class=\"trade-est svelte-1lqen9w\">Est. Value <span class=\"trade-kr svelte-1lqen9w\">%s</span> KR</div></div>", v);
}

static void create_trade_html(StrBuf *sb) {
    sb_append(sb, "<div class=\"trade-overlay svelte-kfskmv\" role=\"dialog\" aria-modal=\"true\" tabindex=\"-1\" onclick=\"v10mk.cclose()\"><div class=\"trade-window svelte-kfskmv\" role=\"document\" onclick=\"v10mk.stop()\">");
    if (M.ct_loading) {
        sb_append(sb, "<div class=\"trade-loading svelte-kfskmv\"><i class=\"material-icons trade-spinner svelte-kfskmv\">hourglass_empty</i> <div>Fetching Inventories...</div></div>");
    } else if (M.ct_err[0] && !M.ct_inv) {
        sb_append(sb, "<div class=\"trade-loading svelte-kfskmv\"><div style=\"color: #f44336;\">");
        sb_append_esc(sb, M.ct_err);
        sb_append(sb, "</div></div>");
    } else {
        sb_append(sb, "<div class=\"trade-sides svelte-kfskmv\">");
        ct_side(sb, 0);
        sb_append(sb, " ");
        ct_side(sb, 1);
        sb_appendf(sb, "</div> <button class=\"trade-toggle-missing svelte-kfskmv%s\" title=\"Toggle Missing Items\" onclick=\"v10mk.cmiss()\"><span class=\"material-icons svelte-kfskmv\">rule</span></button> "
                       "<textarea class=\"trade-message svelte-kfskmv\" placeholder=\"Add a message to the offer\" maxlength=\"500\" id=\"v10mkCtMsg\" oninput=\"v10mk.cmsg(this.value)\">",
                   M.ct_missing ? " active" : "");
        sb_append_esc(sb, M.ct_msg);
        sb_append(sb, "</textarea> ");
        if (M.ct_err[0]) {
            sb_append(sb, "<div class=\"trade-error svelte-kfskmv\">");
            sb_append_esc(sb, M.ct_err);
            sb_append(sb, "</div>");
        }
        const int fee = ct_fee(), empty = !M.ct_n[0] && !M.ct_n[1];
        char label[64];
        if (M.ct_submitting) snprintf(label, sizeof(label), "Submitting...");
        else if (fee > 0) snprintf(label, sizeof(label), "Submit Offer - %d KR", fee);
        else snprintf(label, sizeof(label), "Submit Offer");
        sb_appendf(sb, " <div class=\"trade-submit svelte-kfskmv%s\" role=\"button\" tabindex=\"0\" onmouseenter=\"playTick(0.1)\" onclick=\"v10mk.csubmit()\" id=\"v10mkCtSubmit\">%s</div>",
                   M.ct_submitting || M.modal == MODAL_TRADE_CONFIRM || empty ? " disabled" : "", label);
    }
    sb_append(sb, "</div></div>");
}

// Start a Trade's Cancel / Start Trade buttons (re-rendered as the name is typed)
static void start_actions(StrBuf *sb) {
    button(sb, "danger", 1, M.start_searching, "v10mk.mclose()", "Cancel");
    sb_append(sb, " ");
    char t[64];
    trim_lower(t, sizeof(t), M.start_name);
    button_id(sb, "accept", 1, !t[0] || M.start_searching, "v10mk.sgo()", M.start_searching ? "Searching..." : "Start Trade", "v10mkStartGo");
}

static void trading_modals(StrBuf *sb) {
    if (M.modal == MODAL_DECLINE_ALL) {
        sb_append(sb, "<div class=\"confirm-overlay svelte-1r9fijr\" role=\"dialog\" aria-modal=\"true\" aria-label=\"Decline all incoming trades confirmation\" tabindex=\"-1\" onclick=\"v10mk.mclose()\">"
                      "<div class=\"confirm-box svelte-1r9fijr\" role=\"document\" onclick=\"v10mk.stop()\"><div class=\"confirm-title svelte-1r9fijr\">Decline all incoming trades?</div> "
                      "<div class=\"confirm-desc svelte-1r9fijr\">This will decline every incoming trade offer and can\xe2\x80\x99t be undone.</div> <div class=\"confirm-actions svelte-1r9fijr\">");
        button(sb, NULL, 1, M.actioning != 0, "v10mk.mclose()", "Cancel");
        sb_append(sb, " ");
        button(sb, "danger", 1, M.actioning != 0, "v10mk.mok()", "Decline All");
        sb_append(sb, "</div></div></div>");
    } else if (M.modal == MODAL_START) {
        sb_append(sb, "<div class=\"start-overlay svelte-1r9fijr\" role=\"dialog\" aria-modal=\"true\" tabindex=\"-1\" onclick=\"v10mk.mclose()\"><div class=\"start-box svelte-1r9fijr\" role=\"document\" onclick=\"v10mk.stop()\">"
                      "<div class=\"start-title svelte-1r9fijr\">Start a Trade</div> <div class=\"start-label svelte-1r9fijr\">Enter the name of the player you want to trade with.</div> "
                      "<input class=\"start-input svelte-1r9fijr\" type=\"text\" placeholder=\"Player name\" id=\"v10mkStartName\" value=\"");
        sb_append_esc(sb, M.start_name);
        sb_append(sb, "\" oninput=\"v10mk.sinput(this.value)\" onkeydown=\"v10mk.skey(event)\"> ");
        if (M.start_err[0]) {
            sb_append(sb, "<div class=\"start-error svelte-1r9fijr\">");
            sb_append_esc(sb, M.start_err);
            sb_append(sb, "</div>");
        }
        sb_append(sb, " <div class=\"start-actions svelte-1r9fijr\" id=\"v10mkStartActs\">");
        start_actions(sb);
        sb_append(sb, "</div></div></div>");
    }
    if (M.ct_open) create_trade_html(sb);
}

// the trade offer confirmation (_36471 in a popup frame), pushed on the shared v10 view stack
static void trade_confirm_html(StrBuf *sb) {
    char n[32];
    fmt(n, sizeof(n), ct_fee());
    sb_append(sb, "<div class=\"popup-container svelte-1wz7ymp\"><div class=\"popup svelte-1wz7ymp\" style=\"width: 450px;\" onclick=\"v10mk.stop()\">"
                  "<div class=\"confirm-purchase-container io-popup-container\"><div class=\"confirm-purchase-header io-popup-header io-popup-header--slim\">"
                  "<i class=\"material-icons confirm-icon svelte-1p13km7\">check_circle</i> Confirm trade offer</div> <div class=\"confirm-purchase-content io-popup-content svelte-1p13km7\">"
                  "<div class=\"purchase-preview svelte-1p13km7\"> <div class=\"purchase-info svelte-1p13km7\">");
    if (!M.ct_n[1]) sb_append(sb, "<div class=\"purchase-warning svelte-1p13km7\">You get no items from this offer!</div>");
    sb_appendf(sb, " <div class=\"purchase-question svelte-1p13km7\">Are you sure you want to submit this offer?</div> <div class=\"price-display\">"
                   "<div class=\"currency-display svelte-1r6bdx8 centered\" style=\"--text-size: 20px; --icon-size: 22px;\"><span class=\"currency-amount svelte-1r6bdx8\" style=\"color: #fbc02d;\">%s</span> "
                   "<span class=\"currency-text svelte-1r6bdx8\" style=\"color: #fbc02d;\">KR</span></div></div></div></div> <div class=\"io-button-group svelte-1p13km7\">"
                   "<button type=\"button\" class=\"io-button\" onclick=\"v10mk.mclose()\">Cancel</button> <button type=\"button\" class=\"io-button io-button--accept\" id=\"v10mkModalOk\" onclick=\"v10mk.mok()\">Submit</button></div></div></div></div></div>",
               n);
}

// ------------------------------------------------------------------ Black Market (svelte-dbcods)

static void view_blackm(StrBuf *sb) {
    const AccountInfo *a = acc();
    sb_append(sb, "<div class=\"blackmarket-tab svelte-dbcods\"><div class=\"bm-scroll svelte-dbcods\"><div class=\"bm-header-img svelte-dbcods\" style=\"background-image: linear-gradient(rgb(10 10 10 / 37%), #0000003b), url(./img/backgrounds/blackmarket_0.png);\">"
                  "<div class=\"bm-header-name svelte-dbcods\">Black Market</div> <div class=\"bm-header-desc svelte-dbcods\">Exclusive items earned through mastery and dedication. Meet the requirements to unlock these rare collectibles.</div></div> "
                  "<div class=\"bm-grid svelte-dbcods\">");
    int n = 0;
    const EconBlackMarketItem *items = econ_black_market(&n);
    for (int k = 0; k < n; k++) {
        const ViewerSkin *s = skin(items[k].skin);
        if (!s) continue;
        const int owned = account_owned(items[k].skin) > 0 || inv_count(items[k].skin) > 0;
        int req = 0;
        if (a) {
            if (items[k].age_ms > 0) {
                const cJSON *made = a->data ? cJSON_GetArrayItem(a->data, 30) : NULL;
                req = cJSON_IsNumber(made) && now_ms() - made->valuedouble >= items[k].age_ms;
            } else if (items[k].stat) {
                const cJSON *v = a->stats ? cJSON_GetObjectItemCaseSensitive(a->stats, items[k].stat) : NULL;
                req = cJSON_IsNumber(v) && v->valuedouble >= items[k].min;
            }
        }
        const int can_buy = a && !owned && req && a->funds >= items[k].funds;
        const char *col = rarity_color(s);
        char url[512], price[32];
        thumb(url, sizeof(url), s);
        sb_appendf(sb, "<div class=\"bm-card svelte-dbcods%s\"><div class=\"bm-card-icon svelte-dbcods%s\" role=\"button\" tabindex=\"0\" style=\"border-color: %s;\" onclick=\"v10mk.citem(%d)\">",
                   owned ? " owned" : "", rarity_animate(s) ? " rainbow" : "", col, items[k].skin);
        if (s->type == 4) sb_appendf(sb, "<div class=\"bm-spray-img svelte-dbcods\" style=\"background-image: url('%s'); \"></div>", url);
        else {
            sb_appendf(sb, "<img class=\"bm-item-img svelte-dbcods%s\" draggable=\"false\" src=\"%s\" alt=\"", (s->flags & VS_RGB) ? " rgbHue" : "", url);
            sb_append_esc(sb, s->name ? s->name : "");
            sb_append(sb, "\">");
        }
        sb_appendf(sb, " <div class=\"bm-item-name svelte-dbcods\" style=\"color: %s;\">", col);
        sb_append_esc(sb, s->name ? s->name : "");
        fmt(price, sizeof(price), items[k].funds);
        sb_append(sb, "</div> ");
        if (items[k].funds) sb_appendf(sb, "<div class=\"bm-item-price svelte-dbcods\">%s <span class=\"kr-label svelte-dbcods\">KR</span></div>", price);
        else sb_append(sb, "<div class=\"bm-item-price svelte-dbcods\"><span class=\"kr-label svelte-dbcods\">FREE</span></div>");
        sb_append(sb, " ");
        if (owned) sb_append(sb, "<div class=\"bm-owned-overlay svelte-dbcods\"><span class=\"material-icons\" style=\"font-size: 100px; color: #38cd3a;\">done</span></div>");
        sb_append(sb, " ");
        if (M.bm_buying == items[k].skin) sb_append(sb, "<div class=\"bm-owned-overlay svelte-dbcods\"><span class=\"material-icons bm-spinner svelte-dbcods\">hourglass_empty</span></div>");
        sb_append(sb, "</div> ");
        if (items[k].req_t) {
            sb_append(sb, "<div class=\"bm-req-text svelte-dbcods\">");
            sb_append_esc(sb, items[k].req_t);
            sb_append(sb, "</div>");
        }
        sb_append(sb, "  <div class=\"bm-actions svelte-dbcods\">");
        if (owned) sb_append(sb, "<div class=\"bm-action-btn bm-owned-btn svelte-dbcods\">Owned</div>");
        else if (can_buy) sb_appendf(sb, "<div class=\"bm-action-btn bm-purchase-btn svelte-dbcods\" id=\"v10mkBm%d\" role=\"button\" tabindex=\"0\" onmouseenter=\"playTick(0.1)\" onclick=\"v10mk.bmbuy(%d)\">Purchase</div>", items[k].skin, items[k].skin);
        else sb_appendf(sb, "<div class=\"bm-action-btn bm-locked-btn svelte-dbcods\">%s</div>", !a ? "Login Required" : !req ? "Requirements Not Met" : "Insufficient KR");
        sb_append(sb, "</div></div>");
    }
    sb_append(sb, "</div></div> ");
    if (M.bm_note[0]) {
        sb_appendf(sb, "<div class=\"bm-notification svelte-dbcods%s\">", M.bm_note_err ? " error" : "");
        sb_append_esc(sb, M.bm_note);
        sb_append(sb, "</div>");
    }
    sb_append(sb, "</div>");
}

static void bm_modal(StrBuf *sb) {
    const EconBlackMarketItem *it = econ_black_market_find(M.sel_skin);
    const ViewerSkin *s = skin(M.sel_skin);
    const AccountInfo *a = acc();
    if (!it || !s || !a) return;
    char bal[32], cost[32], rem[32];
    fmt(bal, sizeof(bal), a->funds);
    fmt(cost, sizeof(cost), it->funds);
    fmt(rem, sizeof(rem), a->funds - it->funds);
    sb_appendf(sb, "<div class=\"modal-overlay svelte-dbcods\" role=\"dialog\" aria-modal=\"true\" aria-label=\"Purchase confirmation\" tabindex=\"-1\" onclick=\"v10mk.mclose()\">"
                   "<div class=\"modal-content svelte-dbcods\" role=\"document\" onclick=\"v10mk.stop()\"><h3 class=\"svelte-dbcods\">Confirm Purchase</h3> <div class=\"purchase-details svelte-dbcods\">"
                   "<div class=\"purchase-item-name svelte-dbcods\" style=\"color: %s;\">", rarity_color(s));
    sb_append_esc(sb, s->name ? s->name : "");
    sb_appendf(sb, "</div> <div class=\"purchase-pricing svelte-dbcods\"><div class=\"price-row svelte-dbcods\">Balance: <span class=\"kr-value svelte-dbcods\">%s</span> KR</div> "
                   "<div class=\"price-row svelte-dbcods\">Cost: <span class=\"kr-value svelte-dbcods\">-%s</span> KR</div> <div class=\"price-row total svelte-dbcods%s\">Remaining: "
                   "<span class=\"kr-value svelte-dbcods\">%s</span> KR</div></div></div> <div class=\"modal-actions svelte-dbcods\">",
               bal, cost, a->funds - it->funds < 0 ? " negative" : "", rem);
    button(sb, NULL, 0, 0, "v10mk.mclose()", "Cancel");
    sb_append(sb, " ");
    button_id(sb, "accept", 0, 0, "v10mk.mok()", "Purchase", "v10mkModalOk");
    sb_append(sb, "</div></div></div>");
}

// ------------------------------------------------------------------ Junkyard (svelte-1vqzycn)

// The crafting page (v10_junkyard.c): the recipes, the craft confirmation and its result
static int g_scenario;
static void view_junkyard(StrBuf *sb) {
    if (!g_scenario) v10_junkyard_ensure(); // a logged-in tab with no recipes asked for yet asks now
    v10_junkyard_render(sb);
}

// ------------------------------------------------------------------ Catalog (_46722 svelte-19zccg6, _45008 svelte-dmu4q2)

static IndexList catalog_list(void) {
    IndexList l = {0};
    int cap = 0;
    for (int i = 0; i < g_viewer_skin_count; i++) {
        const ViewerSkin *s = skin(i);
        if (!s || (s->flags & VS_BLOCKED) || !filter_match(&M.f, s)) continue;
        list_push(&l, i, &cap);
    }
    g_sort_key = M.f.sort;
    if (l.n) qsort(l.idx, (size_t) l.n, sizeof(int), cmp_skins);
    return l;
}

static void catalog_grid(StrBuf *sb, const IndexList *l) {
    if (!l->n) {
        sb_append(sb, "<div class=\"items-view-empty svelte-19zccg6\">No items match your filters.</div>");
        return;
    }
    sb_append(sb, "<div class=\"items-view-grid svelte-19zccg6\" role=\"list\" id=\"v10mkGrid\">");
    for (int k = 0; k < l->n && k < M.cshown; k++) {
        char oc[32];
        snprintf(oc, sizeof(oc), "v10mk.citem(%d)", l->idx[k]);
        sb_append(sb, "<div class=\"items-view-cell svelte-19zccg6\">");
        item_card(sb, skin(l->idx[k]), "100%", 0, oc, 0);
        sb_append(sb, "</div></div>");
    }
    sb_append(sb, " ");
    if (l->n > M.cshown) sb_append(sb, "<div class=\"items-view-sentinel svelte-19zccg6\" style=\"height: auto; text-align: center;\"><button class=\"load-more-btn svelte-zhgogl\" onclick=\"v10mk.more()\">Load More</button></div>");
    sb_append(sb, "</div>");
}

static void view_catalog(StrBuf *sb) {
    IndexList l = catalog_list();
    sb_append(sb, "<div class=\"items-view-root svelte-19zccg6 embedded\"> <div class=\"items-view-content svelte-19zccg6\">");
    const FilterView v = {.title = "Items", .count = l.n, .default_sort = "rarity", .search_in_header = 1, .season = 1, .sort_select = 1, .category = 1, .newest = 1};
    filter_header(sb, &M.f, &v);
    sb_append(sb, " <div id=\"v10mkGridHolder\" style=\"display: contents;\">");
    catalog_grid(sb, &l);
    sb_append(sb, "</div></div></div>");
    free(l.idx);
}

// the item detail page (_45008), pushed over the exchange
static void catalog_detail(StrBuf *sb) {
    const ViewerSkin *s = skin(M.cdetail);
    if (!s) return;
    const int index = M.cdetail, bound = skin_bound(s), tradable = !bound;
    const char *col = rarity_color(s);
    char url[512];
    thumb(url, sizeof(url), s);
    sb_append(sb, "<div class=\"items-detail-root svelte-dmu4q2\" role=\"dialog\" aria-label=\"");
    sb_append_esc(sb, s->name ? s->name : "Item detail");
    sb_append(sb, "\" onclick=\"v10mk.stop()\"><div class=\"items-detail-header-bar svelte-dmu4q2\"><button class=\"items-detail-back svelte-dmu4q2\" type=\"button\" aria-label=\"Back\" onclick=\"v10mk.back()\">"
                  "<i class=\"material-icons svelte-dmu4q2\">arrow_back</i></button> <i class=\"material-icons items-detail-header-icon svelte-dmu4q2\">inventory_2</i> <span class=\"items-detail-header-title svelte-dmu4q2\">");
    sb_append_esc(sb, s->name ? s->name : "");
    const int flat = s->type == 16 || s->type == 17 || s->type == 18;
    sb_append(sb, "</span> <div class=\"items-detail-header-actions svelte-dmu4q2\">");
    if (!flat) sb_append(sb, "<button class=\"items-detail-header-btn svelte-dmu4q2\" type=\"button\" onclick=\"v10mk.viewer()\"><i class=\"material-icons svelte-dmu4q2\">open_in_new</i> <span>Open Viewer</span></button>");
    sb_append(sb, " <button class=\"share-btn svelte-1524ito\" type=\"button\" title=\"Copy link\" onclick=\"v10mk.share()\"><i class=\"material-icons svelte-1524ito\">share</i> <span class=\"svelte-1524ito\">Share</span></button></div></div> "
                  "<div class=\"items-detail-body svelte-dmu4q2\"><div class=\"items-detail-preview-panel svelte-dmu4q2\">");
    // KrunkNative: the flat preview image instead of the 3D viewer iframe (no iframes in the engine)
    sb_appendf(sb, "<div class=\"items-detail-image svelte-dmu4q2%s\" style=\"border: 3px solid %s; background-image: url(%s);\"></div>", rarity_animate(s) ? " rainbow" : "", col, url);
    sb_append(sb, " <div class=\"items-detail-header-overlay svelte-dmu4q2\"><div class=\"items-detail-badges-top svelte-dmu4q2\">");
    const int seas = s->seas ? s->seas : 1;
    if (econ_black_market_find(index)) sb_append(sb, "<span class=\"items-detail-badge svelte-dmu4q2\" style=\"background-color: #00ffff;\">Black Market</span>");
    else if (s->type != 18 && seas != g_viewer_current_season) sb_append(sb, "<span class=\"items-detail-badge svelte-dmu4q2\" style=\"background-color: #666666;\">Vaulted</span>");
    else if (s->flags & VS_BLOCKED) sb_append(sb, "<span class=\"items-detail-badge svelte-dmu4q2\" style=\"background-color: #faa332;\">Limited</span>");
    sb_append(sb, " ");
    if (s->type != 18) sb_appendf(sb, "<span class=\"items-detail-season svelte-dmu4q2\">Season %d</span>", seas);
    sb_append(sb, " ");
    if (bound) sb_append(sb, "<i class=\"material-icons items-detail-lock svelte-dmu4q2\" title=\"Bound to account\">lock</i>");
    sb_append(sb, "</div></div>   </div> <div class=\"items-detail-tabs-panel svelte-dmu4q2\"><div class=\"items-detail-tabs-bar svelte-dmu4q2\"><div class=\"simple-tabs svelte-1oqd18e simple-tabs-small\" role=\"tablist\">");
    // tabs: Info, Listings, Sales, Units Sold (tradable items), Owners
    static const char *const labels[5] = {"Info", "Listings", "Sales", "Units Sold", "Owners"};
    for (int t = 0; t < 5; t++) {
        if (!tradable && t >= 1 && t <= 3) continue;
        sb_appendf(sb, "<div class=\"simple-tab svelte-1oqd18e%s\" role=\"tab\" aria-selected=\"%s\" tabindex=\"0\" onmouseenter=\"playTick(0.1)\" onclick=\"v10mk.ctab(%d)\"> <span class=\"simple-tab-label svelte-1oqd18e\">%s</span></div>",
                   M.ctab == t ? " simple-tab-active" : "", M.ctab == t ? "true" : "false", t, labels[t]);
    }
    sb_append(sb, "</div></div> <div class=\"items-detail-tab-content svelte-dmu4q2\">");
    if (M.ctab == 0) {
        sb_appendf(sb, "<div class=\"items-detail-info-panel svelte-dmu4q2\"><div class=\"items-detail-name svelte-dmu4q2%s%s\" style=\"color: %s;\">", rarity_animate(s) ? " rainbow" : "",
                   rarity_dark(s) ? " dark" : "", col);
        sb_append_esc(sb, s->name && s->name[0] ? s->name : "Unnamed");
        sb_append(sb, "</div> ");
        if (s->creator && s->creator[0]) {
            sb_append(sb, "<div class=\"items-detail-creator svelte-dmu4q2\">by <button class=\"items-detail-creator-link svelte-dmu4q2\" type=\"button\">");
            sb_append_esc(sb, s->creator);
            sb_append(sb, "</button> </div>");
        }
        sb_append(sb, " <div class=\"items-detail-description svelte-dmu4q2\">");
        sb_append_esc(sb, type_label(s));
        sb_append(sb, "</div>     <div class=\"items-detail-stats-section svelte-dmu4q2\"><div class=\"items-detail-stats-header svelte-dmu4q2\">Market</div> ");
        if (M.lost & LOST_DVIEW) {
            sb_appendf(sb, "<div class=\"items-detail-empty svelte-dmu4q2\">%s</div>", I18N("kn.mk.statsDetailFailed", "Couldn't load the market stats."));
        } else if (M.dview_loading || !M.dview) {
            sb_append(sb, "<div class=\"items-detail-loading svelte-dmu4q2\"><i class=\"material-icons spin svelte-dmu4q2\">hourglass_empty</i></div>");
        } else {
            char a[32], b[32], c[32], d[32];
            fmt(a, sizeof(a), jnum(M.dview, "onS"));
            fmt(b, sizeof(b), jnum(M.dview, "inC"));
            fmt(c, sizeof(c), jnum(M.dview, "high"));
            fmt(d, sizeof(d), jnum(M.dview, "low"));
            sb_append(sb, "<div class=\"items-detail-stats-grid svelte-dmu4q2\"><div class=\"items-detail-stat\"><div class=\"items-detail-stat-label svelte-dmu4q2\">On Sale</div> <div class=\"items-detail-stat-value svelte-dmu4q2\">");
            if (bound) sb_append(sb, "Cannot be Sold");
            else sb_appendf(sb, "%s units", a);
            sb_appendf(sb, "</div></div> <div class=\"items-detail-stat\"><div class=\"items-detail-stat-label svelte-dmu4q2\">Circulation</div> <div class=\"items-detail-stat-value svelte-dmu4q2\">%s units</div></div> "
                           "<div class=\"items-detail-stat\"><div class=\"items-detail-stat-label svelte-dmu4q2\">Highest Listed</div> <div class=\"items-detail-stat-value items-detail-stat-high svelte-dmu4q2\"><i class=\"material-icons svelte-dmu4q2\">arrow_upward</i> %s</div></div> "
                           "<div class=\"items-detail-stat\"><div class=\"items-detail-stat-label svelte-dmu4q2\">Lowest Listed</div> <div class=\"items-detail-stat-value items-detail-stat-low svelte-dmu4q2\"><i class=\"material-icons svelte-dmu4q2\">arrow_downward</i> %s</div></div></div>",
                       b, c, d);
        }
        sb_append(sb, " </div> </div>");
    } else if (M.ctab == 1) {
        sb_append(sb, "<div class=\"items-detail-listings-panel svelte-dmu4q2\">");
        if (M.lost & LOST_DLIST) sb_appendf(sb, "<div class=\"items-detail-empty svelte-dmu4q2\">%s</div>", I18N("kn.mk.listingsFailed", "Couldn't load the listings."));
        else if (M.dlist_loading || !M.dlist) sb_append(sb, "<div class=\"items-detail-loading svelte-dmu4q2\"><i class=\"material-icons spin svelte-dmu4q2\">hourglass_empty</i></div>");
        else if (!rows_count(M.dlist)) sb_append(sb, "<div class=\"items-detail-empty svelte-dmu4q2\">No active listings.</div>");
        else {
            sb_append(sb, "<ol class=\"items-detail-listings-list svelte-dmu4q2\">");
            const cJSON *l;
            cJSON_ArrayForEach(l, M.dlist) {
                char p[32];
                fmt(p, sizeof(p), jnum(l, "f"));
                sb_appendf(sb, "<li class=\"items-detail-listing-row svelte-dmu4q2\"><span class=\"items-detail-listing-price svelte-dmu4q2\">%s KR</span> <button class=\"items-detail-listing-seller svelte-dmu4q2\" type=\"button\">", p);
                sb_append_esc(sb, jstr(l, "p"));
                sb_append(sb, "</button> ");
                const double ms = jnum(l, "ms");
                if (ms > 0) {
                    const time_t t = (time_t) (ms / 1000.0);
                    struct tm tmv;
#ifdef _WIN32
                    localtime_s(&tmv, &t);
#else
                    localtime_r(&t, &tmv);
#endif
                    sb_appendf(sb, "<span class=\"items-detail-listing-time svelte-dmu4q2\">%d/%d/%d</span>", tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_year + 1900);
                }
                sb_append(sb, "</li>");
            }
            sb_append(sb, "</ol>");
        }
        sb_append(sb, "</div>");
    } else if (M.ctab == 2 || M.ctab == 3) {
        static const char *const ranges[7] = {"1 Week", "2 Weeks", "3 Weeks", "1 Month", "2 Months", "3 Months", "4 Months"};
        static const int range_days[7] = {7, 14, 21, 30, 60, 90, 120};
        sb_append(sb, "<div class=\"items-detail-graph-ranges svelte-dmu4q2\">");
        for (int r = 0; r < 7; r++)
            sb_appendf(sb, "<button class=\"items-detail-range-btn svelte-dmu4q2%s\" type=\"button\" onclick=\"v10mk.crange(%d)\">%s</button>", M.crange == r ? " active" : "", r, ranges[r]);
        sb_append(sb, "</div> <div class=\"items-detail-graph-canvas svelte-dmu4q2\">");
        if (M.lost & LOST_DGD) sb_appendf(sb, "<div class=\"items-detail-empty svelte-dmu4q2\">%s</div>", I18N("kn.mk.salesFailed", "Couldn't load the sales."));
        else if (M.dgd_loading || !M.dgd) sb_append(sb, "<div class=\"items-detail-loading svelte-dmu4q2\"><i class=\"material-icons spin svelte-dmu4q2\">hourglass_empty</i></div>");
        else if (!rows_count(M.dgd)) sb_append(sb, "<div class=\"items-detail-empty svelte-dmu4q2\">No sales in this window.</div>");
        else {
            Series *se = calloc(1, sizeof(Series));
            if (se) {
                build_series(se, M.dgd, range_days[M.crange]);
                if (M.ctab == 2) { // round(f / t)
                    for (int i = 0; i < se->n; i++) se->price[i] = se->units[i] > 0 ? round(se->price[i]) : 0;
                    line_chart(sb, se->price, se->n, "#85bb6555", "#85bb65", "KR", se->label);
                } else {
                    line_chart(sb, se->units, se->n, "#85bb6555", "#85bb65", "Units", se->label);
                }
                free(se);
            }
        }
        sb_append(sb, "</div>");
    } else {
        sb_append(sb, "<div class=\"items-detail-owners-panel svelte-dmu4q2\">");
        owners_list(sb, 1);
        sb_append(sb, "</div>");
    }
    sb_append(sb, "</div></div></div></div>");
}

// ------------------------------------------------------------------ Guide (svelte-4log2q)

static void view_guide(StrBuf *sb) {
    static const char *const gtabs[5] = {"Basics", "Trading", "Safety", "Glossary", "Tips"};
    static const char *const controls[6][2] = {{"WASD", "Move around"}, {"Mouse", "Look around"}, {"E", "Interact / send trade request"},
                                               {"Enter", "Open chat"}, {"Tab", "View scoreboard"}, {"Esc", "Open menu"}};
    static const char *const glossary[16][2] = {
        {"Pure", "Pure KR (Krunkies), an offer with no items involved"},
        {"Upgrade (upg)", "Trading several smaller items for one bigger item, e.g. multiple relics for a contra"},
        {"Downgrade (dg)", "One big item for several smaller ones, e.g. a big contra for cheaper contras"},
        {"Overpay", "Paying extra on top when upgrading (not expected on downgrades)"},
        {"Quick-Sell", "Instantly selling an item for KR from the Market tab"},
        {"Lowball", "Offering well below market price"},
        {"Market Price", "The current average price of an item"},
        {"Low Units", "Limited items with fewer than ~100 units in existence"},
        {"W/L", "Win or loss. Was that trade a W or an L?"},
        {"Flip", "Buying an item cheap and trading it above value"},
        {"p", "Stands in for a decimal point in trade chat (1p5 = 1.5, 10p5 = 10.5)"},
        {"Demand", "How sought-after an item is. High demand means easy to trade"},
        {"GL", "Good luck"}, {"LF", "Looking for"}, {"KR", "Krunker's in-game currency"},
        {"Limited", "Limited-edition items, only obtainable for a time"},
    };
    sb_append(sb, "<div class=\"guide-tab svelte-4log2q\"><div class=\"guide-scroll svelte-4log2q\"><div class=\"guide-header-img svelte-4log2q\" style=\"background-image: linear-gradient(rgb(10 10 10 / 37%), #0000003b), url(./img/backgrounds/server_trade.png);\">"
                  "<div class=\"guide-header-name svelte-4log2q\">Trading Guide</div> <div class=\"guide-header-desc svelte-4log2q\">Everything you need to trade with confidence, from your first swap in Trade Plaza to spotting scams before they happen.</div></div> "
                  "<div class=\"guide-nav svelte-4log2q\">");
    tabs_strip(sb, gtabs, 5, M.gtab, 0, "v10mk.gtab");
    sb_append(sb, "</div> ");
    StrBuf fill = {0};
    const char *anchor = NULL;
    if (M.gtab == 0) {
        anchor = "<div class=\"guide-controls svelte-4log2q\">";
        for (int i = 0; i < 6; i++)
            sb_appendf(&fill, "<div class=\"guide-control svelte-4log2q\"><span class=\"guide-key svelte-4log2q\">%s</span> <span class=\"guide-control-desc svelte-4log2q\">%s</span></div>",
                       controls[i][0], controls[i][1]);
    } else if (M.gtab == 1) {
        anchor = "<div class=\"guide-rarities svelte-4log2q\">";
        for (int r = 6; r >= 0; r--)
            sb_appendf(&fill, "<span class=\"guide-rarity svelte-4log2q%s%s\" style=\"background-color: %s;\">%s</span>", r == 6 ? " rainbowBG" : "", r == 5 ? " dark" : "",
                       g_viewer_rarities[r].color, g_viewer_rarities[r].name);
    } else if (M.gtab == 3) {
        anchor = "<div class=\"guide-glossary svelte-4log2q\">";
        for (int i = 0; i < 16; i++) {
            sb_append(&fill, "<div class=\"guide-term svelte-4log2q\"><div class=\"guide-term-name svelte-4log2q\">");
            sb_append_esc(&fill, glossary[i][0]);
            sb_append(&fill, "</div> <div class=\"guide-term-def svelte-4log2q\">");
            sb_append_esc(&fill, glossary[i][1]);
            sb_append(&fill, "</div></div>");
        }
    }
    char *fill_html = sb_finish(&fill);
    const char *section = k_v10mk_guide[M.gtab < 5 ? M.gtab : 0];
    const char *at = anchor ? strstr(section, anchor) : NULL;
    if (at) {
        const size_t pre = (size_t) (at - section) + strlen(anchor);
        sb_append_n(sb, section, pre);
        sb_append(sb, fill_html);
        sb_append(sb, section + pre);
    } else {
        sb_append(sb, section);
    }
    free(fill_html);
    sb_append(sb, "</div></div>");
}

// ------------------------------------------------------------------ rendering

static int is_open(void) {
    if (!M.open) return 0;
    const HtmlElem pop = el("genericPop");
    if (!pop || !el("v10mkRoot") || !html_dom_has_class(pop, "exchangeModal")) {
        M.open = 0;
        return 0;
    }
    return 1;
}

static void content_html(StrBuf *sb) {
    switch (M.tab) {
        case TAB_MARKET: view_market(sb); break;
        case TAB_TRADING: view_trading(sb); break;
        case TAB_BLACKM: view_blackm(sb); break;
        case TAB_JUNKYARD: view_junkyard(sb); break;
        case TAB_CATALOG: view_catalog(sb); break;
        default: view_guide(sb); break;
    }
}

static void modal_layer_html(StrBuf *sb) {
    if (M.tab == TAB_MARKET) modal_html(sb);
    else if (M.err_open) modal_html(sb);
    if (M.tab == TAB_TRADING) trading_modals(sb);
    if (M.modal == MODAL_BM) bm_modal(sb);
}

static void render(void) {
    if (!M.open) return;
    const HtmlElem pop = el("genericPop");
    if (!pop) return;
    StrBuf sb = {0};
    sb_append(&sb, "<div class=\"exchange-root svelte-1s37jef\" id=\"v10mkRoot\"><div class=\"exchange-sidebar svelte-1s37jef\">");
    for (int i = 0; i < TAB_COUNT; i++)
        sb_appendf(&sb, "<div class=\"sidebar-tab svelte-berov0%s\" id=\"v10mkSide%d\" role=\"tab\" tabindex=\"0\" aria-selected=\"%s\" onmouseenter=\"playTick(0.1)\" onclick=\"playSelect(0.1),v10mk.side(%d)\">"
                        "<i class=\"material-icons sidebar-tab-icon svelte-berov0\">%s</i> <span class=\"sidebar-tab-label svelte-berov0\">%s</span> </div>",
                   M.tab == i ? " active" : "", i, M.tab == i ? "true" : "false", i, k_side[i].icon, k_side[i].label);
    sb_append(&sb, "</div> <div class=\"window-container svelte-623zut\"><div class=\"exchange-content svelte-1s37jef\" id=\"v10mkContent\">");
    content_html(&sb);
    sb_append(&sb, "</div></div><div id=\"v10mkModal\">");
    modal_layer_html(&sb);
    sb_append(&sb, "</div><div id=\"v10mkNotes\">");
    notes_html(&sb);
    sb_append(&sb, "</div></div>");
    char *html = sb_finish(&sb);
    html_dom_set_html(pop, html);
    free(html);
    views_sync();
}

static void render_content(void) {
    if (!is_open()) return;
    StrBuf sb = {0};
    content_html(&sb);
    char *html = sb_finish(&sb);
    set_html("v10mkContent", html);
    free(html);
}

// The views v10 pushes on its view stack (_5235(...).push()): Quick Sell, the trade offer
// confirmation and the item detail page. They live on the page's shared stack (v10_ui.h, #app) and
// follow the state: views_sync() pushes, replaces or pops ours to match it.
enum { VIEW_NONE, VIEW_QS, VIEW_TRADE_CONFIRM, VIEW_ITEM };
static int g_view; // ours on the shared stack (VIEW_NONE: none)
// the Wallet and Inventory's sell and item views (v10_wallet.c) are up with the exchange closed:
// v10 pushes its Quick Sell and item view over the wallet, not over an exchange
static int g_standalone;

static int view_wanted(void) {
    if (!M.open && !g_standalone) return VIEW_NONE;
    if (M.modal == MODAL_QS) return VIEW_QS;
    if (M.modal == MODAL_TRADE_CONFIRM) return VIEW_TRADE_CONFIRM;
    if (M.cdetail >= 0) return VIEW_ITEM;
    return VIEW_NONE;
}

static char *view_markup(const int kind) {
    StrBuf sb = {0};
    if (kind == VIEW_QS) qs_html(&sb);
    else if (kind == VIEW_TRADE_CONFIRM) trade_confirm_html(&sb);
    else if (kind == VIEW_ITEM) catalog_detail(&sb);
    return sb_finish(&sb);
}

static void render_modal(void);

// popped by someone else (its backdrop, clearPops' v10_view_clear): the state follows
static void view_closed(const int accepted, void *user) {
    (void) accepted;
    const int kind = (int) (intptr_t) user;
    if (kind != g_view) return; // popped by views_sync itself
    g_view = VIEW_NONE;
    if (kind == VIEW_QS && M.modal == MODAL_QS && !M.busy) M.modal = MODAL_NONE;
    else if (kind == VIEW_TRADE_CONFIRM && M.modal == MODAL_TRADE_CONFIRM) M.modal = MODAL_NONE;
    else if (kind == VIEW_ITEM) M.cdetail = -1;
    render_modal();
}

static void views_sync(void) {
    const int want = (is_open() || g_standalone) ? view_wanted() : VIEW_NONE;
    if (g_view != want && g_view != VIEW_NONE) {
        g_view = VIEW_NONE;
        v10_view_pop(0);
    }
    if (want == VIEW_NONE) return;
    if (g_view == want) {
        v10_view_set(view_markup(want));
    } else {
        g_view = want;
        v10_view_push(view_markup(want), NULL, view_closed, (void *) (intptr_t) want);
    }
}

static void render_modal(void) {
    if (is_open()) {
        StrBuf sb = {0};
        modal_layer_html(&sb);
        char *html = sb_finish(&sb);
        set_html("v10mkModal", html);
        free(html);
    }
    views_sync();
}

static void render_notes(void) {
    if (!is_open()) return;
    StrBuf sb = {0};
    notes_html(&sb);
    char *html = sb_finish(&sb);
    set_html("v10mkNotes", html);
    free(html);
}

static void render_both(void) {
    render_content();
    render_modal();
}

// the grid alone (typing in a search field keeps its focus; Load More keeps the scroll position)
static void render_grid(void) {
    if (!is_open()) return;
    StrBuf sb = {0};
    char title[96] = "";
    if (M.tab == TAB_CATALOG) {
        IndexList l = catalog_list();
        catalog_grid(&sb, &l);
        snprintf(title, sizeof(title), "Items (%d items)", l.n);
        free(l.idx);
    } else if (M.mtab == MT_BROWSE) {
        browse_grid(&sb);
        snprintf(title, sizeof(title), "Listings (%d items)", browse_groups());
    } else if (M.mtab == MT_INVENTORY) {
        IndexList l = inventory_list();
        inventory_grid(&sb, &l);
        snprintf(title, sizeof(title), "Inventory (%d items)", l.n);
        free(l.idx);
    } else {
        render_content();
        return;
    }
    char *html = sb_finish(&sb);
    set_html("v10mkGridHolder", html);
    free(html);
    const HtmlElem t = el("v10mkFTitle");
    if (t) html_dom_set_text(t, title);
}

// ------------------------------------------------------------------ actions

static void close_modal(void) {
    M.modal = MODAL_NONE;
    M.qs_err[0] = 0;
    render_modal();
}

// the listings / sales / owners of one item, shared by Listing Details and the item detail page
static void item_data(const int index) {
    if (M.data_item == index) return;
    M.data_item = index;
    cJSON_Delete(M.dlist);
    cJSON_Delete(M.dgd);
    cJSON_Delete(M.dview);
    cJSON_Delete(M.downers);
    M.dlist = M.dgd = M.dview = M.downers = NULL;
    M.dlist_loading = M.dgd_loading = M.dview_loading = 0;
    if (!g_scenario) {
        detail_listings_load();
        detail_view_load();
    }
}

static void open_detail(const int index) { // Listing Details (market tab)
    if (!skin(index)) return;
    M.detail = index;
    M.dtab = 0;
    item_data(index);
    render_content();
}

static void open_cdetail(const int index) { // the item detail page, over the exchange (view stack)
    if (!skin(index)) return;
    M.cdetail = index;
    M.ctab = 0;
    item_data(index);
    render_modal();
}

static void market_reload_after(void) {
    if (g_scenario) return;
    browse_load();
    inventory_load();
    sales_load();
    if (M.mtab == MT_HISTORY) history_load();
    if (M.data_item >= 0) detail_listings_load();
}

static void switch_mtab(const int t) {
    if (t < 0 || t >= MT_COUNT || (t >= MT_INVENTORY && t <= MT_HISTORY && !logged_in())) return;
    M.mtab = t;
    filter_reset(&M.f, t == MT_BROWSE ? "name" : "rarity");
    M.gshown = PAGE;
    if (!g_scenario) {
        if (t == MT_BROWSE) browse_load();
        else if (t == MT_INVENTORY) inventory_load();
        else if (t == MT_SALES) sales_load();
        else if (t == MT_HISTORY) history_load();
        else stats_load();
    }
    render_content();
}

static void switch_tab(const int t) {
    if (t < 0 || t >= TAB_COUNT) return;
    M.tab = t;
    M.modal = MODAL_NONE;
    M.detail = M.cdetail = -1;
    if (t == TAB_CATALOG) {
        filter_reset(&M.f, "rarity");
        M.f.expanded = 1; // v10's catalog header starts expanded
        M.cshown = PAGE;
    } else if (t == TAB_MARKET) {
        filter_reset(&M.f, M.mtab == MT_BROWSE ? "name" : "rarity");
        if (!g_scenario) market_reload_after();
    } else if (t == TAB_TRADING && !g_scenario) {
        trades_load();
    } else if (t == TAB_BLACKM && !g_scenario) {
        inventory_load();
    } else if (t == TAB_JUNKYARD && !g_scenario) {
        v10_junkyard_enter();
    }
    render();
}

static void tnote(const int ok, const char *msg);
static void on_social(void *user, const cJSON *frame);

void v10_market_open(int tab, const int item) {
    if (tab < 0 || tab >= TAB_COUNT) tab = 0;
    if (!M.listening) M.listening = backend_social_listen(on_social, NULL);
    ui_loadout_clear_pops();
    const HtmlElem holder = el("popupHolder"), pop = el("genericPop");
    if (!pop) return;
    if (holder) html_dom_set_display(holder, "block");
    html_dom_set_attr(pop, "class", "exchangeModal");
    html_dom_set_attr(pop, "style", "");
    html_dom_set_display(pop, "block");
    g_standalone = 0;
    M.open = 1;
    M.mtab = MT_BROWSE;
    snprintf(M.bsort, sizeof(M.bsort), "latest");
    M.bshown = LISTING_PAGE;
    M.modal = MODAL_NONE;
    M.err_open = 0;
    M.busy = 0;
    M.detail = M.cdetail = -1;
    M.ttab = 0;
    M.gtab = 0;
    M.ct_open = 0;
    M.tnote[0] = M.bm_note[0] = 0;
    M.last_search[0] = 0;
    // the replies that came while the exchange was closed were dropped: their flags are stale, so the tab asks again
    M.bloading = M.bsearching = M.inv_loading = M.sales_loading = M.hist_loading = M.stats_loading = 0;
    M.trades_loading = M.dlist_loading = M.dgd_loading = M.dview_loading = M.ct_loading = 0;
    M.lost = 0;
    html_timer_cancel(M.wait_timer);
    M.wait_timer = 0;
    if (!g_scenario) send_frame(econ_rules_request());
    if (!g_scenario) request_page("market", "prices", -1); // v10: getPrices on the exchange's mount (_36377)
    M.tab = -1;
    switch_tab(tab);
    if (tab == TAB_MARKET && item >= 0) open_detail(item);
}

static void tnote_expire(void *user) {
    if ((int) (intptr_t) user != M.tnote_seq) return;
    M.tnote[0] = 0;
    render_content();
}
static void tnote(const int ok, const char *msg) {
    snprintf(M.tnote, sizeof(M.tnote), "%s", msg ? msg : "");
    M.tnote_err = !ok;
    html_timer_add(TNOTE_MS, tnote_expire, (void *) (intptr_t) ++M.tnote_seq);
    render_content();
}
static void bm_note_expire(void *user) {
    if ((int) (intptr_t) user != M.bm_note_seq) return;
    M.bm_note[0] = 0;
    render_content();
}
static void bm_note(const int ok, const char *msg) {
    snprintf(M.bm_note, sizeof(M.bm_note), "%s", msg ? msg : "");
    M.bm_note_err = !ok;
    html_timer_add(TNOTE_MS, bm_note_expire, (void *) (intptr_t) ++M.bm_note_seq);
    render_content();
}

static void search_fire(void *user) {
    if ((int) (intptr_t) user != M.search_timer) return;
    char t[64];
    trim_lower(t, sizeof(t), M.f.search);
    if (!strcmp(t, M.last_search)) return;
    if (t[0]) { // v10: only when some item name matches (else the listings stay)
        int any = 0;
        for (int i = 0; i < g_viewer_skin_count && !any; i++) any = skin(i) && ci_contains(skin(i)->name, t);
        if (!any) return;
    }
    snprintf(M.last_search, sizeof(M.last_search), "%s", t);
    browse_load();
    render_grid();
}

// Start a Trade / a name in an offer: ["sp", name] -> ["spR", rows]
static void start_search(const char *name) {
    const AccountInfo *a = acc();
    char t[40];
    snprintf(t, sizeof(t), "%s", name);
    if (a && !strcmp(t, a->name)) {
        if (M.modal == MODAL_START) {
            snprintf(M.start_err, sizeof(M.start_err), "Can't trade with yourself");
            render_modal();
        } else {
            tnote(0, "Can't trade with yourself");
        }
        return;
    }
    snprintf(M.start_name, sizeof(M.start_name), "%s", t);
    M.start_searching = 1;
    M.start_err[0] = 0;
    M.pending = ACT_SEARCH;
    if (M.modal == MODAL_START) render_modal();
    cJSON *f = cJSON_CreateArray();
    cJSON_AddItemToArray(f, cJSON_CreateString("sp"));
    cJSON_AddItemToArray(f, cJSON_CreateString(t));
    send_frame(f);
}

static void search_failed(const char *msg) {
    M.start_searching = 0;
    if (M.modal == MODAL_START) {
        snprintf(M.start_err, sizeof(M.start_err), "%s", msg);
        render_modal();
    } else {
        tnote(0, msg);
    }
}

static void open_create_trade(const int id, const char *name) {
    M.modal = MODAL_NONE;
    M.start_searching = 0;
    M.ct_open = 1;
    M.ct_loading = 1;
    M.ct_target_id = id;
    snprintf(M.ct_target, sizeof(M.ct_target), "%s", name);
    M.ct_err[0] = M.ct_msg[0] = M.ct_search[0][0] = M.ct_search[1][0] = 0;
    M.ct_n[0] = M.ct_n[1] = 0;
    M.ct_missing = M.ct_submitting = 0;
    cJSON_Delete(M.ct_inv);
    M.ct_inv = NULL;
    M.pending = ACT_TRADE_INV;
    cJSON *f = cJSON_CreateArray();
    cJSON_AddItemToArray(f, cJSON_CreateString("ri"));
    cJSON_AddItemToArray(f, cJSON_CreateString(name));
    cJSON_AddItemToArray(f, cJSON_CreateNumber(id));
    if (!send_frame(f)) { // no socket: no inventories will come
        M.ct_loading = 0;
        snprintf(M.ct_err, sizeof(M.ct_err), "%s", I18N("kn.mk.inventoriesFailed", "Couldn't load the inventories. Try again."));
    }
    render_modal();
}

// krunker.io is the active backend: market purchases, listings, quick sells, the Black Market and
// trades change a real krunker.io account, which this client never does (kio_store.h): the market's
// error modal says so and nothing is sent. 1 = refused.
static int kio_refused(const char *what) {
    if (!kio_store_active()) return 0;
    printf("[kio-store] read-only: market %s not sent to krunker.io\n", what);
    M.modal = MODAL_NONE;
    show_error(kio_store_read_only_text());
    return 1;
}

static void trade_action(const char *label, const int id, const int act) {
    if (kio_refused(label)) return;
    char key[48];
    double v[2] = {id, 1};
    M.actioning = id;
    M.pending = act;
    if (act == ACT_ACCEPT) {
        snprintf(key, sizeof(key), "acc-%d", id);
        send_econ("trA", 1, v, key);
    } else {
        snprintf(key, sizeof(key), "trc-%d", id);
        send_econ(label, act == ACT_DECLINE ? 2 : 1, v, key);
    }
    render_content();
}

static void confirm_modal(void) {
    if (M.modal != MODAL_NONE && M.modal != MODAL_START && kio_refused("action")) return;
    const AccountInfo *a = acc();
    switch (M.modal) {
        case MODAL_PURCHASE: {
            if (M.busy) return;
            M.busy = 1;
            M.pending = ACT_BUY;
            char key[48];
            snprintf(key, sizeof(key), "buy-%d", M.sel_listing);
            const double v[2] = {M.sel_listing, 0};
            send_econ("pi", 2, v, key);
            render_both();
            break;
        }
        case MODAL_LIST: {
            if (M.busy) return;
            const ViewerSkin *s = skin(M.sel_skin);
            char *end;
            const long p = strtol(M.price_in, &end, 10);
            if (end == M.price_in || p < 1) {
                show_error("Invalid price");
                return;
            }
            if (!s || skin_bound(s) || (inv_row(M.sel_skin) && jint(inv_row(M.sel_skin), "nt", 0))) {
                show_error("This item cannot be sold");
                return;
            }
            const int min = list_min_price(s);
            if (p < min) {
                char msg[96];
                snprintf(msg, sizeof(msg), "Price must be at least %d KR for this rarity", min);
                show_error(msg);
                return;
            }
            if (p > SALE_MAX_KR) {
                show_error("Price cannot exceed 10,000,000 KR");
                return;
            }
            M.busy = 1;
            M.pending = ACT_LIST;
            M.sel_price = (int) p;
            const double v[4] = {M.sel_skin, -1, 0, (double) p}; // li(skin, skinid, uid, price)
            send_econ("li", 4, v, M.op);
            render_both();
            break;
        }
        case MODAL_UNLIST: {
            if (M.busy) return;
            M.busy = 1;
            M.pending = ACT_UNLIST;
            char key[48];
            snprintf(key, sizeof(key), "uli-%d", M.sel_listing);
            const double v[3] = {M.sel_skin, M.sel_listing, 0};
            send_econ("uli", 3, v, key);
            render_both();
            break;
        }
        case MODAL_BM: {
            M.modal = MODAL_NONE;
            if (!a) return;
            M.bm_buying = M.sel_skin;
            M.pending = ACT_BM;
            const double v[1] = {M.sel_skin};
            send_econ("pbm", 1, v, M.op);
            render_both();
            break;
        }
        case MODAL_DECLINE_ALL: {
            M.modal = MODAL_NONE;
            if (M.actioning) return;
            M.actioning = -1;
            M.pending = ACT_DECLINE_ALL;
            send_econ("trCA", 0, NULL, M.op);
            render_both();
            break;
        }
        case MODAL_TRADE_CONFIRM: {
            M.modal = MODAL_NONE;
            M.ct_submitting = 1;
            M.pending = ACT_CREATE;
            send_frame(econ_offer_frame(M.ct_target_id, M.ct_sel[0], M.ct_n[0], M.ct_sel[1], M.ct_n[1], M.ct_msg, 0, 0, M.op));
            render_modal();
            break;
        }
        default:
            break;
    }
}

// ------------------------------------------------------------------ page handlers

static int handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    const char *h = name + 6; // after "v10mk."
    const int a0 = (int) arg_n(args, argc, 0, 0), a1 = (int) arg_n(args, argc, 1, 0), a2 = (int) arg_n(args, argc, 2, 0);
    if (!strcmp(h, "stop")) {
        html_ui_stop_propagation();
    } else if (!strcmp(h, "side")) {
        switch_tab(a0);
    } else if (!strcmp(h, "mtab")) {
        switch_mtab(a0);
    } else if (!strcmp(h, "fhead")) {
        html_ui_stop_propagation();
        M.f.expanded = !M.f.expanded;
        render_content();
    } else if (!strcmp(h, "search")) {
        snprintf(M.f.search, sizeof(M.f.search), "%s", arg_s(ev, args, argc, 0));
        if (M.tab == TAB_MARKET && M.mtab == MT_BROWSE) {
            char t[64];
            trim_lower(t, sizeof(t), M.f.search);
            if (!t[0] && M.last_search[0]) { // cleared: back to the filtered listings
                M.last_search[0] = 0;
                browse_load();
                render_grid();
            } else if (t[0]) {
                html_timer_add(500, search_fire, (void *) (intptr_t) ++M.search_timer); // v10's 500 ms debounce
            }
        } else {
            M.gshown = M.cshown = PAGE;
            render_grid();
        }
    } else if (!strcmp(h, "season")) {
        M.f.season = atoi(arg_s(ev, args, argc, 0));
        if (M.tab == TAB_MARKET && M.mtab == MT_BROWSE) browse_load();
        render_content();
    } else if (!strcmp(h, "fsort")) {
        snprintf(M.f.sort, sizeof(M.f.sort), "%s", arg_s(ev, args, argc, 0));
        render_content();
    } else if (!strcmp(h, "cat")) {
        snprintf(M.f.category, sizeof(M.f.category), "%s", arg_s(ev, args, argc, 0));
        M.cshown = PAGE;
        render_content();
    } else if (!strcmp(h, "fchk")) {
        int *flags[5] = {&M.f.only_missing, &M.f.hide_non_trade, &M.f.only_paintable, &M.f.only_painted, &M.f.group_wear};
        if (a0 >= 0 && a0 < 5) *flags[a0] = !*flags[a0];
        if (M.tab == TAB_MARKET && M.mtab == MT_BROWSE) browse_load();
        M.gshown = PAGE;
        render_content();
    } else if (!strcmp(h, "ftab")) {
        M.f.ftab = a0;
        render_content();
    } else if (!strcmp(h, "fset") || !strcmp(h, "fonly")) {
        const int only = h[1] == 'o';
        if (a0 == 0 && a1 >= 0 && a1 < 8) {
            if (only) memset(M.f.rar, 0, sizeof(M.f.rar));
            M.f.rar[a1] = only ? 1 : !M.f.rar[a1];
        } else if (a0 == 1 && a1 >= 0 && a1 < 64) {
            if (only) { memset(M.f.wep, 0, sizeof(M.f.wep)); memset(M.f.typ, 0, sizeof(M.f.typ)); }
            M.f.wep[a1] = only ? 1 : !M.f.wep[a1];
        } else if (a0 == 2 && a1 >= 1 && a1 <= 18) {
            if (only) { memset(M.f.typ, 0, sizeof(M.f.typ)); memset(M.f.wep, 0, sizeof(M.f.wep)); }
            M.f.typ[a1] = only ? 1 : !M.f.typ[a1];
        } else if (a0 == 3 && a1 >= 0 && a1 < 5) {
            if (only) memset(M.f.wear, 0, sizeof(M.f.wear));
            M.f.wear[a1] = only ? 1 : !M.f.wear[a1];
        }
        if (M.tab == TAB_MARKET && M.mtab == MT_BROWSE) browse_load();
        M.gshown = M.cshown = PAGE;
        render_content();
    } else if (!strcmp(h, "fall")) {
        if (M.f.ftab == 0) {
            const int all = set_count(M.f.rar, 0, 7) == 7;
            for (int i = 0; i < 7; i++) M.f.rar[i] = !all;
        } else if (M.f.ftab == 1) {
            const int all = weapons_all(&M.f);
            for (int i = 0; i < weapon_count(); i++) if (!weapon_melee(i)) M.f.wep[i] = !all;
        } else if (M.f.ftab == 2) {
            const int all = set_count(M.f.typ, 1, 19) == 18;
            for (int i = 1; i <= 18; i++) M.f.typ[i] = !all;
        } else {
            const int all = set_count(M.f.wear, 0, 5) == 5;
            for (int i = 0; i < 5; i++) M.f.wear[i] = !all;
        }
        if (M.tab == TAB_MARKET && M.mtab == MT_BROWSE) browse_load();
        render_content();
    } else if (!strcmp(h, "reset")) {
        const int expanded = M.f.expanded;
        filter_reset(&M.f, M.tab == TAB_MARKET && M.mtab == MT_BROWSE ? "name" : "rarity");
        M.f.expanded = expanded;
        M.last_search[0] = 0;
        if (M.tab == TAB_MARKET && M.mtab == MT_BROWSE) browse_load();
        render_content();
    } else if (!strcmp(h, "bsort")) {
        snprintf(M.bsort, sizeof(M.bsort), "%s", arg_s(ev, args, argc, 0));
        M.f.search[0] = M.last_search[0] = 0; // v10: a sort change clears the search
        browse_load();
        render_content();
    } else if (!strcmp(h, "more")) {
        if (M.tab == TAB_CATALOG) M.cshown += PAGE;
        else if (M.mtab == MT_BROWSE) M.bshown += LISTING_PAGE;
        else M.gshown += PAGE;
        render_grid();
    } else if (!strcmp(h, "item")) {
        open_detail(a0);
    } else if (!strcmp(h, "citem")) { // the item detail page (_45008) over the exchange
        open_cdetail(a0);
    } else if (!strcmp(h, "info")) { // Listing Details' "Item Info": the item detail page
        open_cdetail(M.detail);
    } else if (!strcmp(h, "back")) {
        html_ui_stop_propagation();
        if (M.cdetail >= 0) {
            M.cdetail = -1;
            render_modal();
        } else {
            M.detail = -1;
            render_content();
        }
    } else if (!strcmp(h, "dtab")) {
        M.dtab = a0;
        if (a0 == 1 && !g_scenario && ((!M.dgd && !M.dgd_loading) || M.dgd_range)) detail_stats_load(M.days);
        render_content();
    } else if (!strcmp(h, "days")) {
        M.days = atoi(arg_s(ev, args, argc, 0));
        if (!g_scenario) detail_stats_load(M.days);
        render_content();
    } else if (!strcmp(h, "ctab")) {
        M.ctab = a0;
        if ((a0 == 2 || a0 == 3) && !g_scenario && ((!M.dgd && !M.dgd_loading) || !M.dgd_range)) detail_stats_load(M.crange); // a range index 0..6
        render_modal();
    } else if (!strcmp(h, "crange")) {
        M.crange = a0;
        if (!g_scenario) detail_stats_load(a0);
        render_modal();
    } else if (!strcmp(h, "viewer")) {
        const ViewerSkin *s = skin(M.cdetail);
        if (s) {
            char url[160];
            snprintf(url, sizeof(url), "/viewer.html?nobg&showcase=%d", M.cdetail);
            app_route_url_target(url, 1);
        }
    } else if (!strcmp(h, "share")) {
        char origin[256], url[320];
        html_page_origin(origin, sizeof(origin));
        snprintf(url, sizeof(url), "%s/?item=%d", origin, M.cdetail);
        html_clipboard_write(url);
    } else if (!strcmp(h, "buy")) {
        html_ui_stop_propagation();
        M.sel_listing = a0;
        M.sel_skin = a1;
        M.sel_price = a2;
        M.modal = MODAL_PURCHASE;
        render_modal();
    } else if (!strcmp(h, "list")) {
        html_ui_stop_propagation();
        M.sel_skin = a0;
        M.price_in[0] = 0;
        econ_new_op(M.op, sizeof(M.op), "li");
        M.modal = MODAL_LIST;
        render_modal();
        const HtmlElem in = el("v10mkPrice");
        if (in) html_dom_focus(in);
    } else if (!strcmp(h, "unlist")) {
        html_ui_stop_propagation();
        M.sel_listing = a0;
        M.sel_skin = a1;
        M.modal = MODAL_UNLIST;
        render_modal();
    } else if (!strcmp(h, "price")) {
        snprintf(M.price_in, sizeof(M.price_in), "%s", arg_s(ev, args, argc, 0));
        int valid = 0;
        const HtmlElem in = el("v10mkPrice");
        if (in) html_dom_set_class(in, "error", list_error(skin(M.sel_skin), M.price_in, &valid) != NULL);
        StrBuf sb = {0};
        list_modal_dynamic(&sb);
        char *html = sb_finish(&sb);
        set_html("v10mkListDyn", html);
        free(html);
    } else if (!strcmp(h, "pkey")) {
        int valid = 0;
        list_error(skin(M.sel_skin), M.price_in, &valid);
        if (ev && html_key_is_escape(ev->key)) close_modal();
        else if (ev && html_key_is_enter(ev->key) && valid && !M.busy) confirm_modal();
    } else if (!strcmp(h, "mclose")) {
        if (M.busy || M.start_searching) return 1;
        if (M.modal == MODAL_START) M.start_err[0] = 0;
        close_modal();
        if (M.ct_open) render_modal();
    } else if (!strcmp(h, "mok")) {
        confirm_modal();
    } else if (!strcmp(h, "errok")) {
        html_ui_stop_propagation();
        M.err_open = 0;
        render_modal();
    } else if (!strcmp(h, "qs")) {
        html_ui_stop_propagation();
        M.sel_skin = a0;
        M.sel_count = inv_count(a0);
        M.qs_mode = 0;
        M.qs_err[0] = 0;
        econ_new_op(M.op, sizeof(M.op), "qs");
        M.modal = MODAL_QS;
        render_modal();
    } else if (!strcmp(h, "qsmode")) {
        M.qs_mode = atoi(arg_s(ev, args, argc, 0));
        render_modal();
    } else if (!strcmp(h, "qsgo")) {
        if (M.busy || kio_refused("quick-sell")) return 1;
        M.busy = 1;
        M.qs_err[0] = 0;
        M.pending = ACT_QS;
        const double v[4] = {a0 == 2 ? 2 : 0, M.sel_skin, 0, M.sel_count > 1 ? M.qs_mode : 0}; // qsi(mode, skin, uid, amount)
        send_econ("qsi", 4, v, M.op);
        render_modal();
    } else if (!strcmp(h, "hsort")) {
        const char *col = arg_s(ev, args, argc, 0);
        if (!strcmp(M.hcol, col)) M.hasc = !M.hasc;
        else {
            snprintf(M.hcol, sizeof(M.hcol), "%s", col);
            M.hasc = 0;
        }
        render_content();
    } else if (!strcmp(h, "ttab")) {
        M.ttab = a0;
        render_content();
    } else if (!strcmp(h, "tcreate")) {
        M.start_name[0] = M.start_err[0] = 0;
        M.start_searching = 0;
        M.modal = MODAL_START;
        render_modal();
        const HtmlElem in = el("v10mkStartName");
        if (in) html_dom_focus(in);
    } else if (!strcmp(h, "taccept")) {
        if (!M.actioning) trade_action("trA", a0, ACT_ACCEPT);
    } else if (!strcmp(h, "tdecline")) {
        if (!M.actioning) trade_action("trC", a0, ACT_DECLINE);
    } else if (!strcmp(h, "tcancel")) {
        if (!M.actioning) trade_action("trC", a0, ACT_CANCEL);
    } else if (!strcmp(h, "tdeclineall")) {
        if (M.actioning) return 1;
        econ_new_op(M.op, sizeof(M.op), "trca");
        M.modal = MODAL_DECLINE_ALL;
        render_modal();
    } else if (!strcmp(h, "tname")) {
        start_search(arg_s(ev, args, argc, 0));
    } else if (!strcmp(h, "sinput")) {
        snprintf(M.start_name, sizeof(M.start_name), "%s", arg_s(ev, args, argc, 0));
        // only the buttons depend on the text: rebuild them (the field keeps its focus)
        StrBuf sb = {0};
        start_actions(&sb);
        char *html = sb_finish(&sb);
        set_html("v10mkStartActs", html);
        free(html);
    } else if (!strcmp(h, "skey")) {
        if (ev && html_key_is_enter(ev->key)) {
            char t[64];
            trim_lower(t, sizeof(t), M.start_name);
            if (t[0] && !M.start_searching) start_search(M.start_name);
        } else if (ev && html_key_is_escape(ev->key) && !M.start_searching) {
            close_modal();
        }
    } else if (!strcmp(h, "sgo")) {
        char t[64];
        trim_lower(t, sizeof(t), M.start_name);
        if (t[0] && !M.start_searching) start_search(M.start_name);
    } else if (!strcmp(h, "cadd") || !strcmp(h, "crem")) {
        const int side = a0 ? 1 : 0, index = a1;
        if (h[1] == 'a') {
            const cJSON *row = NULL, *r;
            cJSON_ArrayForEach(r, cJSON_GetArrayItem(M.ct_inv, side)) if (jint(r, "i", -1) == index) row = r;
            if (!row || jint(row, "nt", 0) || ct_offer_count(side) >= ct_max_items() || jint(row, "c", 1) - ct_offered(side, index) <= 0) return 1;
            int k = 0;
            while (k < M.ct_n[side] && M.ct_sel[side][k].i != index) k++;
            if (k == M.ct_n[side]) {
                if (k >= 40) return 1;
                M.ct_sel[side][k] = (EconTradeItem) {index, 0};
                M.ct_n[side]++;
            }
            M.ct_sel[side][k].c++;
        } else {
            for (int k = 0; k < M.ct_n[side]; k++) {
                if (M.ct_sel[side][k].i != index) continue;
                if (--M.ct_sel[side][k].c <= 0) {
                    memmove(&M.ct_sel[side][k], &M.ct_sel[side][k + 1], sizeof(EconTradeItem) * (size_t) (M.ct_n[side] - k - 1));
                    M.ct_n[side]--;
                }
                break;
            }
        }
        M.ct_err[0] = 0;
        render_modal();
    } else if (!strcmp(h, "csearch")) {
        const int side = a0 ? 1 : 0;
        snprintf(M.ct_search[side], sizeof(M.ct_search[side]), "%s", arg_s(ev, args, argc, 1));
        render_modal();
        char id[32];
        snprintf(id, sizeof(id), "v10mkCtSearch%d", side);
        const HtmlElem in = el(id);
        if (in) {
            html_dom_focus(in);
            const int len = (int) strlen(M.ct_search[side]);
            html_dom_text_set_selection(in, len, len);
        }
    } else if (!strcmp(h, "cmiss")) {
        M.ct_missing = !M.ct_missing;
        render_modal();
    } else if (!strcmp(h, "cmsg")) {
        snprintf(M.ct_msg, sizeof(M.ct_msg), "%s", arg_s(ev, args, argc, 0));
    } else if (!strcmp(h, "cclose")) {
        M.ct_open = 0;
        M.modal = MODAL_NONE;
        render_modal();
    } else if (!strcmp(h, "csubmit")) {
        if (M.ct_submitting || M.modal == MODAL_TRADE_CONFIRM) return 1;
        if (!M.ct_n[0] && !M.ct_n[1]) {
            snprintf(M.ct_err, sizeof(M.ct_err), "No items selected");
            render_modal();
            return 1;
        }
        econ_new_op(M.op, sizeof(M.op), "ctr");
        M.modal = MODAL_TRADE_CONFIRM;
        render_modal();
    } else if (!strcmp(h, "bmbuy")) {
        M.sel_skin = a0;
        econ_new_op(M.op, sizeof(M.op), "pbm");
        M.modal = MODAL_BM;
        render_modal();
    } else if (!strcmp(h, "gtab")) {
        M.gtab = a0;
        render_content();
    } else {
        return -1;
    }
    return 1;
}

// ------------------------------------------------------------------ the Wallet and Inventory (v10_wallet.c)

// The wallet's Quick Sell (v10's _46234 pushed over the wallet): the market's own view, with the
// exchange closed. Its values are the market inventory's (loaded here when it is not yet).
void v10_market_wallet_sell(const int index) {
    if (!skin(index)) return;
    if (!logged_in()) return;
    g_standalone = 1;
    inventory_load();
    M.sel_skin = index;
    M.sel_count = inv_count(index);
    M.qs_mode = 0;
    M.qs_err[0] = 0;
    econ_new_op(M.op, sizeof(M.op), "qs");
    M.modal = MODAL_QS;
    render_modal();
}

// The wallet's item tap: the item detail page (the Catalog's, catalog_detail), pushed over the wallet.
void v10_market_wallet_item(const int index) {
    if (!skin(index)) return;
    g_standalone = 1;
    open_cdetail(index);
    render_modal();
}

// The wallet closed: its views (the Quick Sell or the item page over it) go with it. The exchange
// is not open (v10_market_open clears g_standalone when it opens).
void v10_market_wallet_release(void) {
    g_standalone = 0;
    if (!M.open) {
        if (M.modal == MODAL_QS && !M.busy) M.modal = MODAL_NONE;
        M.cdetail = -1;
    }
    render_modal();
}

int v10_market_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    if (!name) return -1;
    if (!strcmp(name, "openExchange")) { // window.openExchange(tab = 0, itemIndex = -1)
        v10_market_open((int) arg_n(args, argc, 0, 0), (int) arg_n(args, argc, 1, -1));
        return 1;
    }
    if (!strcmp(name, "openItems")) { // the standalone catalog (itemsModal): the exchange's Catalog tab here
        v10_market_open(TAB_CATALOG, -1);
        return 1;
    }
    if (strncmp(name, "v10jy.", 6) == 0) return is_open() ? v10_junkyard_handle(name, ev, args, argc) : 1;
    if (strncmp(name, "v10mk.", 6) != 0) return -1;
    if (!is_open() && !g_standalone) return 1;
    return handle(name, ev, args, argc);
}

// v10_junkyard.c: the Junkyard's replies and popups re-render the exchange's content
void v10_market_junkyard_refresh(void) {
    if (is_open() && M.tab == TAB_JUNKYARD) render_content();
}

// The deadline of the reads in flight (send_frame): a read whose reply never came is failed here, so its screen
// shows its error (or the empty state) instead of "Loading" for good. The flags are the ones the replies clear.
static void reads_expired(void *user) {
    (void) user;
    M.wait_timer = 0;
    int any = 0;
    if (M.bloading) { M.bloading = M.bsearching = 0; any = 1; }
    if (M.inv_loading) { M.inv_loading = 0; any = 1; }
    if (M.sales_loading) { M.sales_loading = 0; any = 1; }
    if (M.hist_loading) { M.hist_loading = 0; any = 1; }
    if (M.stats_loading) { M.stats_loading = 0; any = 1; }
    if (M.trades_loading) { M.trades_loading = 0; M.lost |= LOST_TRADES; any = 1; }
    if (M.dlist_loading) { M.dlist_loading = 0; M.lost |= LOST_DLIST; any = 1; }
    if (M.dgd_loading) { M.dgd_loading = 0; M.lost |= LOST_DGD; any = 1; }
    if (M.dview_loading) { M.dview_loading = 0; M.lost |= LOST_DVIEW; any = 1; }
    if (M.ct_loading) {
        M.ct_loading = 0;
        snprintf(M.ct_err, sizeof(M.ct_err), "%s", I18N("kn.mk.inventoriesFailed", "Couldn't load the inventories. Try again."));
        any = 1;
    }
    if (any && is_open()) render_both();
}

// The active account changed (a login, a logout, the session restored after the exchange opened): what the open
// tab shows is the account's, and a tab opened as a guest (or before the session came) asks now.
void v10_market_account_changed(void) {
    const AccountInfo *a = active_account();
    const int id = a ? a->id : 0;
    if (id == M.account_id) return;
    M.account_id = id;
    v10_junkyard_account_changed();
    if (!is_open() || g_scenario) return;
    M.lost = 0;
    if (M.tab == TAB_TRADING) trades_load();
    else if (M.tab == TAB_MARKET) {
        market_reload_after();
        if (M.mtab == MT_STATS) stats_load();
    } else if (M.tab == TAB_BLACKM) inventory_load();
    render();
}

// ------------------------------------------------------------------ social frames

static void apply_funds(const double f) {
    if (f > 0 || f == 0) account_set_funds(f);
}

static void finish_action(void) {
    M.busy = 0;
    M.pending = ACT_NONE;
}

static void on_social(void *user, const cJSON *frame) {
    (void) user;
    const char *label = cJSON_GetStringValue(cJSON_GetArrayItem(frame, 0));
    if (!label) return;
    const cJSON *a1 = cJSON_GetArrayItem(frame, 1), *a2 = cJSON_GetArrayItem(frame, 2), *a3 = cJSON_GetArrayItem(frame, 3);
    if (!strcmp(label, "ecfg")) {
        econ_rules_apply(a1);
        return;
    }
    if (!is_open()) return;
    if (!strcmp(label, "fr")) {
        if ((int) cJSON_GetNumberValue(a1) != M.fil_token) return;
        browse_loaded(cJSON_IsArray(a2) ? a2 : NULL);
        if (M.tab == TAB_MARKET && M.mtab == MT_BROWSE) render_content();
    } else if (!strcmp(label, "0")) {
        const char *page = cJSON_GetStringValue(a1), *sub = cJSON_GetStringValue(a2);
        if (!page) return;
        if (!strcmp(page, "itemsales")) {
            M.dview_loading = 0;
            M.lost &= ~LOST_DVIEW;
            cJSON_Delete(M.dview);
            M.dview = cJSON_IsObject(a3) ? cJSON_Duplicate(a3, 1) : cJSON_CreateObject();
            set_json(&M.downers, cJSON_GetArrayItem(frame, 4));
        } else if (!strcmp(page, "market") && sub) {
            if (!strcmp(sub, "inventory")) { M.inv_loading = 0; set_json(&M.inv, a3); }
            else if (!strcmp(sub, "sales")) { M.sales_loading = 0; set_json(&M.sales, a3); }
            else if (!strcmp(sub, "history")) { M.hist_loading = 0; set_json(&M.hist, a3); }
            else if (!strcmp(sub, "stats")) { M.stats_loading = 0; market_stats_apply(&M.stats, a3); } // an error reply keeps the rows
            else if (!strcmp(sub, "prices")) social_set_estimates(a3); // the cards' "~ N KR" (social_estimate_value)
            else if (!strcmp(sub, "trades")) {
                M.trades_loading = 0;
                M.trades_loaded = 1;
                M.lost &= ~LOST_TRADES;
                cJSON_Delete(M.trades);
                M.trades = cJSON_IsArray(a3) ? cJSON_Duplicate(a3, 1) : cJSON_CreateArray();
            } else if (!strcmp(sub, "market")) { M.dlist_loading = 0; M.lost &= ~LOST_DLIST; set_json(&M.dlist, a3); }
            else return;
        } else {
            return;
        }
        render_both();
    } else if (!strcmp(label, "gd")) {
        M.dgd_loading = 0;
        M.lost &= ~LOST_DGD;
        set_json(&M.dgd, a1);
        render_both();
    } else if (!strcmp(label, "br")) { // purchase
        const int ok = cJSON_IsObject(a1);
        finish_action();
        M.modal = MODAL_NONE;
        if (ok) {
            apply_funds(jnum(a1, "f"));
            account_add_skin(M.sel_skin);
            notify(1, "Item purchased successfully!");
            market_reload_after();
        } else {
            show_error("Item Unavailable");
        }
        render_both();
    } else if (!strcmp(label, "lir")) { // list
        finish_action();
        const cJSON *resp = cJSON_GetObjectItemCaseSensitive(a1, "response");
        if (cJSON_IsString(resp) && !strcmp(resp->valuestring, "MAX")) {
            M.modal = MODAL_NONE;
            show_error("Max Items on Sale");
            notify(0, "Failed to list item");
        } else if (cJSON_IsObject(a1)) {
            M.modal = MODAL_NONE;
            if (cJSON_GetObjectItemCaseSensitive(a1, "funds")) apply_funds(jnum(a1, "funds"));
            account_remove_skin(M.sel_skin);
            market_reload_after();
            notify(1, "Item listed successfully");
        } else {
            show_error("Failed to list item");
            notify(0, "Failed to list item");
        }
        render_both();
    } else if (!strcmp(label, "ulir")) { // unlist
        finish_action();
        M.modal = MODAL_NONE;
        const char *res = cJSON_GetStringValue(a1);
        if (res && !strcmp(res, "SUCCESS")) {
            account_add_skin(M.sel_skin);
            market_reload_after();
            notify(1, "Item unlisted successfully");
        } else {
            show_error(res && !strcmp(res, "SOLD") ? "Item already Sold" : cJSON_GetStringValue(a3) ? cJSON_GetStringValue(a3) : "Failed to unlist item");
            notify(0, "Failed to unlist item");
            market_reload_after();
        }
        render_both();
    } else if (!strcmp(label, "qsr")) { // quick sell
        finish_action();
        if (cJSON_IsObject(a1)) {
            const ViewerSkin *s = skin(M.sel_skin);
            const int left = jint(a1, "c", 0), sold = M.sel_count - left;
            if (cJSON_GetObjectItemCaseSensitive(a1, "f")) apply_funds(jnum(a1, "f"));
            for (int k = 0; k < sold; k++) account_remove_skin(M.sel_skin);
            char msg[160];
            const int destroyed = jnum(a1, "kr") <= 0;
            snprintf(msg, sizeof(msg), destroyed ? "Destroyed %s" : "Sold %s for KR", s && s->name ? s->name : "item");
            M.modal = MODAL_NONE;
            notify(1, msg);
            inventory_load();
            if (M.data_item >= 0) detail_listings_load();
        } else {
            snprintf(M.qs_err, sizeof(M.qs_err), "Quick sell failed");
        }
        render_both();
    } else if (!strcmp(label, "bmr")) { // Black Market purchase
        M.bm_buying = 0;
        M.pending = ACT_NONE;
        if (cJSON_IsObject(a1)) {
            apply_funds(jnum(a1, "f"));
            account_add_skin(jint(a1, "item", M.sel_skin));
            inventory_load();
            bm_note(1, "Item purchased successfully!");
        } else {
            bm_note(0, "Purchase failed");
        }
        render_both();
    } else if (!strcmp(label, "spR")) { // Start a Trade: the player search
        if (M.pending != ACT_SEARCH) return;
        M.pending = ACT_NONE;
        const cJSON *row = NULL, *r;
        cJSON_ArrayForEach(r, a1) {
            const char *nm = jstr(r, "player_name");
            if (strlen(nm) == strlen(M.start_name) && ci_contains(nm, M.start_name)) row = r;
        }
        const AccountInfo *a = acc();
        if (!row) search_failed("Player not found");
        else if (a && jint(row, "player_id", 0) == a->id) search_failed("Can't trade with yourself");
        else if ((int) floor(0.03 * sqrt(jnum(row, "player_score"))) < (int) g_econ_rules.trade_min_lvl) {
            char msg[96];
            snprintf(msg, sizeof(msg), "%s must be level %d to trade", jstr(row, "player_name"), (int) g_econ_rules.trade_min_lvl);
            search_failed(msg);
        } else {
            open_create_trade(jint(row, "player_id", 0), jstr(row, "player_name"));
        }
    } else if (!strcmp(label, "trD")) { // the two inventories
        if (!M.ct_open) return;
        M.ct_loading = 0;
        M.pending = ACT_NONE;
        cJSON_Delete(M.ct_inv);
        M.ct_inv = cJSON_IsArray(a3) ? cJSON_Duplicate(a3, 1) : NULL;
        if (!M.ct_inv) snprintf(M.ct_err, sizeof(M.ct_err), "Failed to load inventories");
        render_modal();
    } else if (!strcmp(label, "trR")) {
        const int act = M.pending;
        M.pending = ACT_NONE;
        M.actioning = 0;
        if (cJSON_IsObject(a1) && cJSON_GetObjectItemCaseSensitive(a1, "funds")) apply_funds(jnum(a1, "funds"));
        const int ok = cJSON_IsObject(a1);
        if (act == ACT_CREATE) {
            M.ct_submitting = 0;
            if (ok) {
                M.ct_open = 0;
                tnote(1, "Trade offer sent!");
            } else {
                snprintf(M.ct_err, sizeof(M.ct_err), "Failed to create trade");
            }
        } else if (ok) {
            tnote(1, act == ACT_ACCEPT ? "Trade accepted!" : act == ACT_DECLINE ? "Trade declined" : "Trade cancelled");
        } else {
            tnote(0, act == ACT_ACCEPT ? "Failed to accept trade" : act == ACT_DECLINE ? "Failed to decline trade" : "Failed to cancel trade");
        }
        trades_load();
        render_both();
    } else if (!strcmp(label, "pRes")) { // trCA: "Declined All"
        if (M.pending != ACT_DECLINE_ALL) return;
        M.pending = ACT_NONE;
        M.actioning = 0;
        const char *kind = cJSON_GetStringValue(a1);
        tnote(kind && !strcmp(kind, "SUCCESS"), kind && !strcmp(kind, "SUCCESS") ? "All trades declined" : "Failed to decline all trades");
        trades_load();
    } else if (!strcmp(label, "econN")) { // someone else's trade / sale touched this account
        if (M.tab == TAB_TRADING) trades_load();
        else if (M.tab == TAB_MARKET) market_reload_after();
    } else if (!strcmp(label, "po")) { // a refusal: the pending action's error
        const char *kind = cJSON_GetStringValue(a1);
        if (kind && !strcmp(kind, "SUCCESS")) return;
        const char *err = cJSON_GetStringValue(a2);
        const int act = M.pending;
        if (act == ACT_NONE) return;
        finish_action();
        M.actioning = 0;
        switch (act) {
            case ACT_BUY: M.modal = MODAL_NONE; show_error(err ? err : "Failed to purchase item"); break;
            case ACT_LIST: show_error(err ? err : "Failed to list item"); notify(0, "Failed to list item"); break;
            case ACT_UNLIST: M.modal = MODAL_NONE; show_error(err ? err : "Failed to unlist item"); notify(0, "Failed to unlist item"); break;
            case ACT_QS: snprintf(M.qs_err, sizeof(M.qs_err), "%s", err ? err : "Quick sell failed"); break;
            case ACT_BM: M.bm_buying = 0; bm_note(0, err ? err : "Purchase failed"); break;
            case ACT_CREATE: M.ct_submitting = 0; snprintf(M.ct_err, sizeof(M.ct_err), "%s", err ? err : "Failed to create trade"); break;
            case ACT_SEARCH: search_failed(err ? err : "Player not found"); return;
            case ACT_TRADE_INV: M.ct_loading = 0; snprintf(M.ct_err, sizeof(M.ct_err), "%s", err ? err : "Failed to load inventories"); break;
            default: tnote(0, err ? err : "Failed. Try Again"); break;
        }
        render_both();
    } else if (!strcmp(label, "ua")) { // the account changed server side (funds, inventory)
        if (M.tab == TAB_BLACKM) render_content();
    }
}

// The exchange owns these replies while it is open (account_ui.c leaves "pRes" to it then).
int v10_market_owns_frame(const char *label) {
    return label && !strcmp(label, "pRes") && M.open && M.pending == ACT_DECLINE_ALL && is_open();
}

// ------------------------------------------------------------------ menu entry

// v10's #menuBtnMarket (icon sell, "Market & Trading", visible when logged in: _52937 = !account)
static int g_menu_timer;
static void menu_sync(void *user) {
    (void) user;
    const HtmlElem title = el("menuBtnMarket");
    const HtmlElem item = title ? html_dom_closest(title, ".menuItem") : 0;
    if (item) {
        char oc[64] = "";
        if (html_dom_get_attr(item, "onclick", oc, sizeof(oc)) < 0 || !oc[0]) {
            html_dom_set_attr(item, "onclick", "playSelect(0.1),openExchange()");
            html_dom_set_attr(item, "onmouseenter", "playTick(0.1)");
        }
        // only on a change: a style write invalidates the page's layout (every 500 ms otherwise)
        char cur[16] = "";
        const int has = html_dom_get_style(item, "display", cur, sizeof(cur)) >= 0 && cur[0];
        const int shown = !has || strcmp(cur, "none") != 0;
        if (shown != logged_in()) html_dom_set_display(item, logged_in() ? "" : "none");
    }
    g_menu_timer = html_timer_add(500, menu_sync, NULL);
}

// the language changed: the open Market shows its item type labels (social.market.*) in it
static void market_on_locale(void *user) {
    (void) user;
    if (is_open()) render();
}

void v10_market_menu_setup(void) {
    if (!M.listening) M.listening = backend_social_listen(on_social, NULL);
    i18n_on_change(market_on_locale, NULL); // registered once (i18n_on_change ignores repeats)
    if (g_menu_timer) html_timer_cancel(g_menu_timer);
    g_menu_timer = 0;
    if (!el("menuBtnMarket")) return; // not the v10 page
    menu_sync(NULL);
}

// ------------------------------------------------------------------ --render-scenario

static void scenario_rows(cJSON **slot, const cJSON *v) {
    if (cJSON_IsArray(v)) set_json(slot, v);
}

// state.market: {tab, mtab, item, dtab, listings, inventory, sales, history, stats, trades, sales_stats,
// owners, view, modal ("purchase" | "list" | "unlist" | "quicksell" | "start" | "create" | "declineAll"
// | "blackm"), sel, price, ct: {target, id, inv, mine, theirs}}: the exchange without a brain
void v10_market_scenario(const cJSON *state) {
    if (!cJSON_IsObject(state)) return;
    g_scenario = 1;
    v10_market_menu_setup();
    const int tab = jint(state, "tab", 0);
    v10_market_open(tab, -1);
    const cJSON *listings = cJSON_GetObjectItem(state, "listings");
    if (cJSON_IsArray(listings) && jint(state, "item", -1) < 0) browse_loaded(listings);
    scenario_rows(&M.inv, cJSON_GetObjectItem(state, "inventory"));
    scenario_rows(&M.sales, cJSON_GetObjectItem(state, "sales"));
    scenario_rows(&M.hist, cJSON_GetObjectItem(state, "history"));
    scenario_rows(&M.stats, cJSON_GetObjectItem(state, "stats"));
    const cJSON *trades = cJSON_GetObjectItem(state, "trades");
    if (cJSON_IsArray(trades)) {
        cJSON_Delete(M.trades);
        M.trades = cJSON_Duplicate(trades, 1);
        M.trades_loaded = 1;
    }
    if (tab == TAB_MARKET && cJSON_IsNumber(cJSON_GetObjectItem(state, "mtab"))) switch_mtab(jint(state, "mtab", 0));
    const int item = jint(state, "item", -1);
    if (item >= 0) {
        if (tab == TAB_CATALOG || cJSON_IsTrue(cJSON_GetObjectItem(state, "page"))) {
            open_cdetail(item);
            M.ctab = jint(state, "dtab", 0);
        } else {
            open_detail(item);
            M.dtab = jint(state, "dtab", 0);
        }
        M.dgd_range = tab == TAB_CATALOG;
        scenario_rows(&M.dlist, cJSON_GetObjectItem(state, "listings"));
        scenario_rows(&M.dgd, cJSON_GetObjectItem(state, "sales_stats"));
        scenario_rows(&M.downers, cJSON_GetObjectItem(state, "owners"));
        const cJSON *view = cJSON_GetObjectItem(state, "view");
        if (cJSON_IsObject(view)) {
            cJSON_Delete(M.dview);
            M.dview = cJSON_Duplicate(view, 1);
        }
    }
    const char *modal = cJSON_GetStringValue(cJSON_GetObjectItem(state, "modal"));
    const cJSON *sel = cJSON_GetObjectItem(state, "sel");
    M.sel_skin = jint(sel, "skin", 0);
    M.sel_listing = jint(sel, "listing", 0);
    M.sel_price = jint(sel, "price", 0);
    M.sel_count = jint(sel, "count", 1);
    snprintf(M.price_in, sizeof(M.price_in), "%s", jstr(state, "price"));
    if (modal) {
        if (!strcmp(modal, "purchase")) M.modal = MODAL_PURCHASE;
        else if (!strcmp(modal, "list")) M.modal = MODAL_LIST;
        else if (!strcmp(modal, "unlist")) M.modal = MODAL_UNLIST;
        else if (!strcmp(modal, "quicksell")) M.modal = MODAL_QS;
        else if (!strcmp(modal, "start")) M.modal = MODAL_START;
        else if (!strcmp(modal, "declineAll")) M.modal = MODAL_DECLINE_ALL;
        else if (!strcmp(modal, "blackm")) M.modal = MODAL_BM;
        else if (!strcmp(modal, "error")) show_error(jstr(state, "error"));
    }
    const cJSON *ct = cJSON_GetObjectItem(state, "ct");
    if (cJSON_IsObject(ct)) {
        M.ct_open = 1;
        M.ct_target_id = jint(ct, "id", 2);
        snprintf(M.ct_target, sizeof(M.ct_target), "%s", jstr(ct, "target"));
        const cJSON *inv = cJSON_GetObjectItem(ct, "inv");
        M.ct_inv = cJSON_IsArray(inv) ? cJSON_Duplicate(inv, 1) : NULL;
        for (int side = 0; side < 2; side++) {
            const cJSON *picks = cJSON_GetObjectItem(ct, side ? "theirs" : "mine"), *p;
            cJSON_ArrayForEach(p, picks) if (M.ct_n[side] < 40) M.ct_sel[side][M.ct_n[side]++] = (EconTradeItem) {jint(p, "i", 0), jint(p, "c", 1)};
        }
    }
    const cJSON *notes = cJSON_GetObjectItem(state, "notes"), *n;
    cJSON_ArrayForEach(n, notes) notify(1, cJSON_GetStringValue(n));
    if (cJSON_IsNumber(cJSON_GetObjectItem(state, "gtab"))) M.gtab = jint(state, "gtab", 0);
    if (cJSON_IsNumber(cJSON_GetObjectItem(state, "ttab"))) M.ttab = jint(state, "ttab", 0);
    if (cJSON_IsTrue(cJSON_GetObjectItem(state, "expanded"))) M.f.expanded = 1;
    render();
}
