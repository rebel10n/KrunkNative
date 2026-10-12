// Map editor: Krunker v3.1.1's API-backed windows and map publishing (editor.js
// window.windows[1] Assets, [4] Mods, [6] Textures, [7] Welcome; getCardsList /
// getModsList; KE.selectAsset / KE.selectMod; publishMap).
//
// The lists come from the brain's hub catalogue (doc/backend.md 7.5) through
// backend_http_get(): /api/assets?index=&cat=&username= (listAssets), ?index=2&cat=&val=
// (searchAsset), /api/mods?index=&pos=&accountId= (listMods), /api/search?type=mod&val=
// (searchMod). Rows and markup are v3.1.1's.
//
// Parity notes (intentional differences):
// - Without a brain (guest build, no kn_brain answering, HTTP error) v3.1.1's fetch would
//   fail silently and leave "Loading..." up forever; here the list shows the window's own
//   empty text ("No Assets found", "No Mods found", "No Textures found").
// - Thumbnails on user-assets.krunker.io are not reachable natively: cards use
//   ./img/noimg.png there (v3.1.1's onerror fallback); the web build keeps the URL.
// - Prefab assets (selectAsset type 3) are fetched from user-assets.krunker.io in v3.1.1;
//   KrunkNative hosts no asset files, so they report v3.1.1's "Error loading Prefab".
// - publishMap: v3.1.1 stores the map as "mapToPub" and opens social.html?p=maps&openPub,
//   whose publish popup sends ["uploadM", map, thumb, description, video, category].
//   The editor shows that popup's map part itself (#knPrompt dialog: category, video,
//   description) and sends uploadM on the social socket of this window's account
//   (account_ui.c restores the saved session in every window). The thumbnail is a PNG
//   of the 3D view (v3.1.1 asks for a file): read back right after the view is drawn,
//   scaled down until it fits config.maxThumb (80 kB, 150 kB premium; brain limit 200 kB).
// - advancedOptions.selectModel / selectAudio / selectDeath / ... do not exist in v3.1.1
//   (their "Select Custom" / "Select Sound" buttons do nothing); only selectMod and
//   selectTexture open a window.
#include "editor_internal.h"
#include <app_mode.h>
#include <backend.h>
#include <backend_ui.h>
#include <gl.h>
#include <html_ui.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#pragma GCC diagnostic ignored "-Wcast-qual"
#endif
#include "../../lib/glfw/deps/stb_image_write.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

// ---------------------------------------------------------------------------
// config.js tables

static const char *const k_asset_cat[] = {
    "Clutter", "Decoration", "Trees & Vegetation", "Vehicles", "Buildings", "Furniture", "Stones & Rubble", "Technology",
    "Weapons", "Random & Strange", "Tools", "Food & Drink", "Characters & Creatures", "Fashion & Clothes", "Game Map",
    "Walls & Blocks", "Textures", "Sounds",
};
#define ASSET_CAT_COUNT ((int) (sizeof(k_asset_cat) / sizeof(k_asset_cat[0])))

static const char *const k_map_cats[] = {
    "Other", "Zombies", "Parkour", "Trading", "Social", "Horror", "Puzzle", "Training", "Battle Royale", "Realism", "Rotation", "Raid",
};
#define MAP_CAT_COUNT ((int) (sizeof(k_map_cats) / sizeof(k_map_cats[0])))

// config.assetTabs: New, My Assets (sendUser), Search
static const char *const k_asset_tabs[] = {"New", "My Assets", "Search"};
#define ASSET_TAB_COUNT 3
#define ASSET_TAB_SEARCH 2

// config.modTabs: {n, sendID, search}
static const struct { const char *n; int send_id, search; } k_mod_tabs[] = {
    {"Popular", 0, 0}, {"Featured", 0, 0}, {"New", 0, 0}, {"Favorites", 1, 0}, {"My Mods", 1, 0}, {"Search", 0, 1},
};
#define MOD_TAB_COUNT 6

#define MAX_THUMB_KB 80   // config.maxThumb
#define MAX_THUMB_P_KB 150 // config.maxThumbP (premium)
#define MAP_DATA_MAX 500000

// ---------------------------------------------------------------------------
// list windows (windows[1] Assets, [4] Mods, [6] Textures)

enum { LW_ASSETS, LW_MODS, LW_TEXTURES, LW_COUNT };

typedef struct {
    int requested;   // listData[tab] exists (a request was made)
    int waiting;     // listData[tab].waiting
    char *dat;       // listData[tab].dat (cards so far)
} ListData;

typedef struct {
    int window;      // showWindow index (2, 5, 7)
    int tab;
    int type;        // windows[1].type / windows[6].type (null = 0)
    ListData list[MOD_TAB_COUNT];
    unsigned gen;    // bumps when the list is reset: drops stale replies
} ListWin;

static ListWin g_win[LW_COUNT] = {{2, 0, 0, {{0}}, 0}, {5, 0, 0, {{0}}, 0}, {7, 0, 0, {{0}}, 0}};

static const char *list_id(const int w) { return w == LW_ASSETS ? "assetList" : w == LW_MODS ? "modList" : "assetTList"; }
static const char *none_text(const int w) { return w == LW_ASSETS ? "No Assets found" : w == LW_MODS ? "No Mods found" : "No Textures found"; }
static const char *search_results_id(const int w) { return w == LW_ASSETS ? "searchResultsA" : w == LW_MODS ? "searchResultsM" : "searchResultsT"; }

static void list_clear(ListData *d) {
    free(d->dat);
    memset(d, 0, sizeof(*d));
}

static int brain_ok(void) { return backend_enabled() && backend_available() == 1; }

static const char *user_name(void) {
    // editor.js: userName = getSavedVal("krunker_username")
    static char name[64];
    const AccountInfo *a = account_get();
    if (a && a->name[0]) return a->name;
    if (html_storage_get("krunker_username", name, sizeof(name)) < 0) name[0] = 0;
    return name;
}

// UTILS.cleanseString: drop ' and "
static void cleanse(EdBuf *b, const char *s) {
    for (; s && *s; s++) {
        if (*s == '\'' || *s == '"') continue;
        const char c[2] = {*s, 0};
        edb_add(b, c);
    }
}

// (Date.now() - new Date(s)) / 86400000; -1 when it does not parse
static double age_days(const char *s) {
    if (!s || !*s) return -1;
    int y, mo, d, h = 0, mi = 0, sec = 0;
    if (sscanf(s, "%d-%d-%d%*c%d:%d:%d", &y, &mo, &d, &h, &mi, &sec) < 3) return -1;
    struct tm t = {0};
    t.tm_year = y - 1900;
    t.tm_mon = mo - 1;
    t.tm_mday = d;
    t.tm_hour = h;
    t.tm_min = mi;
    t.tm_sec = sec;
#ifdef _WIN32
    const time_t at = _mkgmtime(&t);
#else
    const time_t at = timegm(&t);
#endif
    if (at == (time_t) -1) return -1;
    // a trailing UTC offset ("...-04", "+05:30", "Z")
    double off = 0;
    const char *z = s + strlen(s);
    while (z > s && ((z[-1] >= '0' && z[-1] <= '9') || z[-1] == ':')) z--;
    if (z > s + 10 && (z[-1] == '+' || z[-1] == '-')) {
        int oh = 0, om = 0;
        sscanf(z, "%d:%d", &oh, &om);
        off = (oh * 3600.0 + om * 60.0) * (z[-1] == '-' ? -1 : 1);
    }
    return (difftime(time(NULL), at) + off) / 86400.0;
}

static const char *row_str(const cJSON *row, const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(row, key);
    return cJSON_IsString(v) ? v->valuestring : "";
}

static void add_num_or_str(EdBuf *b, const cJSON *v) {
    if (cJSON_IsNumber(v)) edb_addf(b, "%.15g", v->valuedouble);
    else if (cJSON_IsString(v)) edb_add(b, v->valuestring);
}

static int thumbs_remote(void) {
#ifdef __EMSCRIPTEN__
    return 1;
#else
    return 0;
#endif
}

// getCardsList(rows, type)
static void cards_list(EdBuf *b, const cJSON *rows, const int type) {
    const cJSON *row;
    cJSON_ArrayForEach(row, rows) {
        cJSON *n = cJSON_Parse(row_str(row, "dat"));
        if (!cJSON_IsObject(n)) {
            cJSON_Delete(n);
            n = cJSON_CreateObject();
        }
        const double a = age_days(row_str(row, "ini"));
        int l = type;
        if (ed_json_truthy(cJSON_GetObjectItemCaseSensitive(n, "d"))) l = 3;
        if (ed_json_truthy(cJSON_GetObjectItemCaseSensitive(n, "x"))) l = 4;
        EdBuf c;
        edb_init(&c);
        add_num_or_str(&c, cJSON_GetObjectItemCaseSensitive(row, "id"));
        if (l < 3) {
            const cJSON *k;
            cJSON_ArrayForEach(k, n) edb_add(&c, k->string);
        }
        EdBuf id;
        edb_init(&id);
        cleanse(&id, c.buf ? c.buf : "");
        edb_free(&c);
        const char *cid = id.buf ? id.buf : "";
        const int has_thumb = ed_json_truthy(cJSON_GetObjectItemCaseSensitive(n, "t"));
        edb_add(b, "<div class='mapListItem'>\n            ");
        if (a >= 0 && a <= 7) edb_add(b, "<div class='mapAgeD' style='background-color:#E040FB'>NEW</div>");
        edb_add(b, "\n            <div class='mapListThumbHolder'>\n                <img class='mapListThumb' loading='lazy' src='");
        if (has_thumb && thumbs_remote()) {
            edb_add(b, "https://user-assets.krunker.io/");
            add_num_or_str(b, cJSON_GetObjectItemCaseSensitive(row, "id"));
            edb_add(b, "/thumb.png");
        } else {
            edb_add(b, "./img/noimg.png");
        }
        edb_addf(b, "'\n                    onerror='this.onerror=null;this.src=\"./img/noimg.png;\"' onclick='KE.selectAsset(\"%s\",%d)'/>\n"
                    "\t\t\t\t<div class='assetText' style='top:4px'>%s</div>\n\t\t\t\t<div class='assetText'>",
                 cid, l, ed_json_truthy(cJSON_GetObjectItemCaseSensitive(n, "d")) ? "Prefab"
                         : ed_json_truthy(cJSON_GetObjectItemCaseSensitive(n, "x")) ? "Texture" : "Model");
        const cJSON *siz = cJSON_GetObjectItemCaseSensitive(row, "siz");
        if (ed_json_truthy(siz)) add_num_or_str(b, siz);
        else edb_add(b, "0");
        edb_addf(b, "\n\t\t\t\t\t<span style='color: rgba(255,255,255,0.6);'>kb</span>\n\t\t\t\t</div>\n            </div>\n"
                    "            <div style='margin-top:1px;text-overflow: ellipsis;overflow: hidden;white-space: nowrap;'>\n"
                    "                <a href='javascript:;' onclick='KE.selectAsset(\"%s\",%d)'/>%s</a>\n            </div>\n"
                    "            <div style='margin-top:1px;text-overflow: ellipsis;overflow: hidden;white-space: nowrap;font-size: 14px;'>\n\t\t\t\t",
                 cid, l, row_str(row, "nm"));
        int cat = (int) ed_json_num(row, "cat", 0);
        if (cat < 0 || cat >= ASSET_CAT_COUNT) cat = -1;
        edb_add(b, cat >= 0 ? k_asset_cat[cat] : "undefined");
        edb_add(b, "\n\t\t\t</div>\n\t\t\t<div style='display: flex'>\n                <span style='color:rgba(0,0,0,0.2);font-size:15px'> by\n"
                   "                    <a target='_blank' class='grey' href='/social.html?p=profile&q=");
        cleanse(b, row_str(row, "plrn"));
        edb_addf(b, "'>%s</a>\n                </span>\n            </div>\n        </div>", row_str(row, "plrn"));
        edb_free(&id);
        cJSON_Delete(n);
    }
}

// getModsList(rows)
static void mods_list(EdBuf *b, const cJSON *rows) {
    const cJSON *row;
    cJSON_ArrayForEach(row, rows) {
        const double t = age_days(row_str(row, "mod_date"));
        cJSON *s = cJSON_Parse(row_str(row, "mod_info"));
        EdBuf url;
        edb_init(&url);
        if (!strcmp(row_str(row, "mod_url"), "ua")) {
            edb_add(&url, "https://user-assets.krunker.io/md");
            add_num_or_str(&url, cJSON_GetObjectItemCaseSensitive(row, "mod_id"));
            edb_add(&url, "/mod.zip");
        } else {
            cleanse(&url, row_str(row, "mod_url"));
        }
        const char *u = url.buf ? url.buf : "";
        edb_add(b, "<div class='mapListItem'>\n            ");
        if (t >= 0 && t <= 7) edb_add(b, "<div class='mapAgeD' style='background-color:#E040FB'>NEW</div>");
        edb_add(b, "\n            <div class='mapListThumbHolder'>\n                <img class='mapListThumb' loading='lazy' src='");
        if (cJSON_IsObject(s) && ed_json_truthy(cJSON_GetObjectItemCaseSensitive(s, "t")) && thumbs_remote()) {
            edb_add(b, "https://user-assets.krunker.io/md");
            add_num_or_str(b, cJSON_GetObjectItemCaseSensitive(row, "mod_id"));
            edb_addf(b, "/thumb.png?v=%d", (int) ed_json_num(row, "mod_updatecounter", 0));
        } else {
            const char *img = row_str(row, "mod_image");
            edb_add(b, img[0] && (thumbs_remote() || !strncmp(img, "data:", 5) || !strncmp(img, "./", 2)) ? img : "./img/noimg.png");
        }
        edb_addf(b, "'\n                    onerror='this.onerror=null;this.src=\"./img/noimg.png;\"' onclick='KE.selectMod(\"%s\")'/>\n"
                    "            </div>\n            <div style='margin-top:1px;text-overflow: ellipsis;overflow: hidden;white-space: nowrap;'>\n"
                    "                <a href='javascript:;' onclick='KE.selectMod(\"%s\")'/>%s</a>\n            </div>\n"
                    "\t\t\t<div style='display: flex'>\n                <span style='color:rgba(0,0,0,0.2);font-size:15px'> by\n"
                    "                    <a target='_blank' class='grey' href='/social.html?p=profile&q=",
                 u, u, row_str(row, "mod_name"));
        cleanse(b, row_str(row, "creatorname"));
        edb_addf(b, "'>%s</a>\n                </span>\n            </div>\n        </div>", row_str(row, "creatorname"));
        edb_free(&url);
        cJSON_Delete(s);
    }
}

// ---------------------------------------------------------------------------
// requests and replies (assetListResp / modListResp / textureListResp, searchResp*)

typedef struct {
    int w, tab, search;
    unsigned gen;
} Req;

static void loaded(const int w, const int tab, const cJSON *data, const char *error);
static void search_loaded(const int w, const cJSON *data);

static void on_http(void *user, const int status, const char *body, const size_t length) {
    Req *r = user;
    const int w = r->w, tab = r->tab, search = r->search;
    const unsigned gen = r->gen;
    free(r);
    if (!g_ed.active || gen != g_win[w].gen) return;
    cJSON *res = status == 200 && body ? cJSON_ParseWithLength(body, length) : NULL;
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(res, "data");
    const cJSON *err = cJSON_GetObjectItemCaseSensitive(res, "error");
    if (search) {
        search_loaded(w, cJSON_IsArray(data) ? data : NULL);
    } else if (!res) {
        loaded(w, tab, NULL, NULL); // transport / HTTP error: the empty text (see the notes)
    } else {
        const int index = (int) ed_json_num(res, "index", tab);
        loaded(w, index >= 0 && index < MOD_TAB_COUNT ? index : tab, cJSON_IsArray(data) ? data : NULL,
               cJSON_IsString(err) && err->valuestring[0] ? err->valuestring : NULL);
    }
    cJSON_Delete(res);
}

static int request(const int w, const int tab, const int search, const char *path) {
    if (!brain_ok()) return 0;
    Req *r = calloc(1, sizeof(*r));
    if (!r) return 0;
    *r = (Req) {w, tab, search, g_win[w].gen};
    if (!backend_http_get(path, on_http, r)) {
        free(r);
        return 0;
    }
    return 1;
}

static void q_add(char *out, const size_t n, const char *key, const char *value) {
    // URLSearchParams: null values are dropped
    if (!value) return;
    char enc[512];
    backend_url_encode(enc, sizeof(enc), value);
    const size_t len = strlen(out);
    snprintf(out + len, n - len, "%c%s=%s", strchr(out, '?') ? '&' : '?', key, enc);
}

static void q_int(char *out, const size_t n, const char *key, const int v) {
    char s[24];
    snprintf(s, sizeof(s), "%d", v);
    q_add(out, n, key, s);
}

// API_CLIENT.listAssets(index, pos, username, cat) / listMods(index, pos, accountId)
static int list_request(const int w, const int tab, const int pos) {
    char path[512];
    if (w == LW_MODS) {
        snprintf(path, sizeof(path), "/api/mods");
        q_int(path, sizeof(path), "index", tab);
        q_int(path, sizeof(path), "pos", pos);
        const AccountInfo *a = account_get();
        if (a) q_int(path, sizeof(path), "accountId", a->id);
    } else {
        snprintf(path, sizeof(path), "/api/assets");
        q_int(path, sizeof(path), "index", tab);
        q_int(path, sizeof(path), "cat", w == LW_TEXTURES ? 16 : -1);
        const char *u = user_name();
        if (u[0]) q_add(path, sizeof(path), "username", u);
    }
    return request(w, tab, 0, path);
}

static void set_list_html(const int w, const char *html) {
    const HtmlElem e = html_dom_by_id(list_id(w));
    if (e) html_dom_set_html(e, html);
}

// windows[n].assetsLoaded / modsLoaded(data, index, error)
static void loaded(const int w, const int tab, const cJSON *data, const char *error) {
    ListWin *lw = &g_win[w];
    ListData *d = &lw->list[tab];
    d->waiting = 0;
    if (data && cJSON_GetArraySize(data) > 0) {
        if (error) {
            if (!d->dat) {
                set_list_html(w, error);
                list_clear(d);
            }
            return;
        }
        EdBuf b;
        edb_init(&b);
        if (d->dat) edb_add(&b, d->dat);
        if (w == LW_MODS) mods_list(&b, data);
        else cards_list(&b, data, lw->type);
        free(d->dat);
        d->dat = b.buf ? b.buf : strdup("");
        if (lw->tab == tab && g_ed.window_index == lw->window) editor_show_window(lw->window);
    } else if (!d->dat) {
        set_list_html(w, none_text(w));
        list_clear(d);
    }
}

// windows[n].searchResp(data)
static void search_loaded(const int w, const cJSON *data) {
    EdBuf b;
    edb_init(&b);
    if (data && cJSON_GetArraySize(data) > 0) {
        edb_add(&b, "<div style='height:10px'></div>");
        if (w == LW_MODS) mods_list(&b, data);
        else cards_list(&b, data, g_win[w].type);
    } else {
        edb_add(&b, none_text(w));
    }
    const HtmlElem e = html_dom_by_id(search_results_id(w));
    if (e) html_dom_set_html(e, b.buf ? b.buf : "");
    edb_free(&b);
}

// ---------------------------------------------------------------------------
// gen()

static void tabs(EdBuf *b, const int w) {
    const ListWin *lw = &g_win[w];
    const int win = w == LW_ASSETS ? 1 : w == LW_MODS ? 4 : 6;
    edb_add(b, "\n            <div class='menuTabsNew'>\n                ");
    const int n = w == LW_MODS ? MOD_TAB_COUNT : ASSET_TAB_COUNT;
    for (int t = 0; t < n; t++) {
        if (w == LW_MODS && k_mod_tabs[t].send_id) continue;
        const char *name = w == LW_MODS ? k_mod_tabs[t].n : (w == LW_TEXTURES && t == 1) ? "My Textures" : k_asset_tabs[t];
        edb_addf(b, "<div class=\"menuTabNew %s\" onclick=\"windows[%d].switchTab(%d)\">%s</div>", lw->tab == t ? " tabANew" : "", win, t, name);
    }
    edb_add(b, "\n            </div>");
}

static int is_search_tab(const int w, const int tab) { return w == LW_MODS ? k_mod_tabs[tab].search : tab == ASSET_TAB_SEARCH; }

// the list part of gen(): listData state machine
static void list_body(EdBuf *t, const int w) {
    ListWin *lw = &g_win[w];
    ListData *d = &lw->list[lw->tab];
    if (d->requested) {
        if (d->dat) {
            edb_add(t, d->dat);
            if (d->waiting) edb_add(t, "<div class='loadMoreW'>Loading...</div>");
            else list_clear(d);
        } else {
            edb_add(t, "<div class='loadMoreW'>Loading...</div>");
        }
    } else {
        d->requested = 1;
        d->waiting = 0;
        if (list_request(w, lw->tab, 0)) edb_add(t, "<div class='loadMoreW'>Loading...</div>");
        else {
            // no brain: what the list would show for an empty reply
            edb_add(t, none_text(w));
            list_clear(d);
        }
    }
}

static void gen_list_window(EdBuf *out, const int w) {
    ListWin *lw = &g_win[w];
    EdBuf e, t;
    edb_init(&e);
    edb_init(&t);
    if (w == LW_MODS) edb_add(&e, "<div class=\"windowHeader\">\n                <div>Mods</div>\n            </div>");
    else edb_addf(&e, "<div class=\"windowHeader\">\n                <div style='display: block;'>%s <div class='publishAsset' onclick='openPub()'>Publish</div></div>\n            </div>",
                  w == LW_ASSETS ? "Assets" : "Textures");
    tabs(&e, w);
    if (is_search_tab(w, lw->tab)) {
        if (w == LW_ASSETS) {
            edb_add(&t, "<input type='text' id='asSrch' class='smlInput' placeholder='Asset Name' onkeydown='if(window.event.keyCode==13)windows[1].searchAssets()' />");
            edb_add(&t, "<select id='asCat'><option value='-1'>All</option>");
            for (int n = 0; n < ASSET_CAT_COUNT; n++) {
                edb_addf(&t, "<option value='%d'>", n);
                edb_add_escaped(&t, k_asset_cat[n]);
                edb_add(&t, "</option>");
            }
            edb_add(&t, "</select>");
            edb_add(&t, "<div class='searchBtn' onclick='windows[1].searchAssets()'>Search</div><div style='color:rgba(0,0,0,0.5);margin-top:10px' id='searchResultsA'></div>");
        } else if (w == LW_MODS) {
            edb_add(&t, "<input type='text' id='mSrch' style='width: 847px;' class='smlInput' placeholder='Mod Name' onkeydown='if(window.event.keyCode==13)windows[4].searchMods()' />");
            edb_add(&t, "<div class='searchBtn' onclick='windows[4].searchMods()'>Search</div><div style='color:rgba(0,0,0,0.5);margin-top:10px' id='searchResultsM'></div>");
        } else {
            edb_add(&t, "<input type='text' id='astSrch' class='smlInput' placeholder='Texture Name' onkeydown='if(window.event.keyCode==13)windows[6].searchAssets()' />");
            edb_add(&t, "<div class='searchBtn' onclick='windows[6].searchAssets()'>Search</div><div style='color:rgba(0,0,0,0.5);margin-top:10px' id='searchResultsT'></div>");
        }
    } else {
        list_body(&t, w);
    }
    edb_addf(out, "<div style='margin-bottom:15px'>%s</div>\n            <div style='color:rgba(0,0,0,0.3)' id='%s'>%s</div>",
             e.buf ? e.buf : "", list_id(w), t.buf ? t.buf : "");
    edb_free(&e);
    edb_free(&t);
}

int ed_net_window_gen(const int index, EdBuf *out) {
    switch (index) {
        case 2: gen_list_window(out, LW_ASSETS); return 1;
        case 5: gen_list_window(out, LW_MODS); return 1;
        case 7: gen_list_window(out, LW_TEXTURES); return 1;
        case 8:
            edb_add(out, "<div style='text-align:center;font-size:30px;color:#fff'>Welcome to the Krunker Editor</div><img src='./img/map_show.png' class='introImg' />");
            return 1;
        default: return 0;
    }
}

// ---------------------------------------------------------------------------
// selection (KE.selectAsset / KE.selectMod / viewAssets / advancedOptions.select*)

void ed_net_view_assets(const int type) {
    // viewAssets(e): windows[1].type = e; showWindow(2)
    g_win[LW_ASSETS].type = type;
    editor_show_window(2);
}

void ed_net_select(EdCtl *c) {
    // advancedOptions.selectMod: showWindow(5); selectTexture: showWindow(7). selectModel,
    // selectAudio, selectDeath, selectIdle, selectAttack and selectAgro are not defined in
    // v3.1.1's advancedOptions: GUI.action stores NaN in them and nothing opens.
    const int a = c ? c->arg : 0;
    if (a == 7) editor_show_window(5);
    else if (a == 1) editor_show_window(7);
}

static void select_asset(const char *id, const int type) {
    editor_close_window();
    if (type == 3) {
        // prefab: fetched from user-assets.krunker.io/<id>/data.json, not hosted here
        editor_message("Error loading Prefab");
    } else if (type == 4) {
        if (g_ed.proj->selected) {
            cJSON *v = cJSON_CreateNumber(atoi(id));
            ed_prop_set(g_ed.proj->selected, "assetTexture", v);
            cJSON_Delete(v);
            ed_obj_rebuild(g_ed.proj->selected);
            g_ed.dirty_panel = 1;
            ed_mark_changed();
        }
    } else if (type) {
        // the Game folder's player model: every class's "aID" (the v3 map-wide game config)
        cJSON *v = cJSON_CreateString(id);
        ed_map_set_all_classes("aID", v);
        cJSON_Delete(v);
        ed_panel_left_show();
    } else {
        // new ObjectInstance({i: 44, aid: e, p: near or [0,0,0]}, true)
        const vec3 p = editor_setting_bool("createObjectNear") ? editor_near_position() : (vec3) {0};
        cJSON *d = cJSON_CreateObject();
        cJSON_AddNumberToObject(d, "i", ED_PREFAB_CUSTOM_ASSET);
        cJSON_AddStringToObject(d, "aid", id);
        cJSON *pa = cJSON_AddArrayToObject(d, "p");
        cJSON_AddItemToArray(pa, cJSON_CreateNumber(p.x));
        cJSON_AddItemToArray(pa, cJSON_CreateNumber(p.y));
        cJSON_AddItemToArray(pa, cJSON_CreateNumber(p.z));
        EditorObject *o = ed_obj_new(d, 0);
        cJSON_Delete(d);
        if (o) ed_obj_add(o, 0, 1);
    }
}

static void select_mod(const char *url) {
    editor_close_window();
    ed_json_set_str(g_ed.proj->map_config, "modURL", url ? url : "");
    ed_mark_changed();
    ed_panel_left_show();
}

// ---------------------------------------------------------------------------
// publishing (publishMap + social.js publishPopup / uploadGameAsset / io_upMp / io_pubMR)

static char *g_thumb;          // data:image/png;base64,... of the view, NULL none
static int g_capture_pending;  // read the view back after the next frame's overlays
static int g_listening;
static int g_publish_sent;     // an uploadM is waiting for pubMR / upMp

static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char *data_url_png(const unsigned char *png, const int len) {
    static const char prefix[] = "data:image/png;base64,";
    const size_t out_len = sizeof(prefix) - 1 + (size_t) (len + 2) / 3 * 4;
    char *s = malloc(out_len + 1);
    if (!s) return NULL;
    memcpy(s, prefix, sizeof(prefix) - 1);
    char *o = s + sizeof(prefix) - 1;
    for (int i = 0; i < len; i += 3) {
        const unsigned v = (unsigned) png[i] << 16 | (i + 1 < len ? (unsigned) png[i + 1] << 8 : 0) | (i + 2 < len ? png[i + 2] : 0);
        *o++ = b64[v >> 18 & 63];
        *o++ = b64[v >> 12 & 63];
        *o++ = i + 1 < len ? b64[v >> 6 & 63] : '=';
        *o++ = i + 2 < len ? b64[v & 63] : '=';
    }
    *o = 0;
    return s;
}

// UTILS.thumbnailSize(dataUrl) in kB: unpadded length * 0.75 / 1000
static double thumb_kb(const char *url) {
    size_t n = 0;
    for (const char *p = url; *p; p++) if (*p != '=') n++;
    return (double) (long) ((double) n * 0.75) / 1000.0;
}

// box-filter `src` (w x h RGBA, top row first) down to dw x dh, opaque
static unsigned char *downscale(const unsigned char *src, const int w, const int h, const int dw, const int dh) {
    unsigned char *dst = malloc((size_t) dw * (size_t) dh * 4);
    if (!dst) return NULL;
    for (int y = 0; y < dh; y++) {
        const int y0 = y * h / dh, y1 = (y + 1) * h / dh > y0 ? (y + 1) * h / dh : y0 + 1;
        for (int x = 0; x < dw; x++) {
            const int x0 = x * w / dw, x1 = (x + 1) * w / dw > x0 ? (x + 1) * w / dw : x0 + 1;
            unsigned sum[3] = {0, 0, 0}, cnt = 0;
            for (int yy = y0; yy < y1; yy++) {
                for (int xx = x0; xx < x1; xx++) {
                    const unsigned char *p = src + ((size_t) yy * (size_t) w + (size_t) xx) * 4;
                    sum[0] += p[0];
                    sum[1] += p[1];
                    sum[2] += p[2];
                    cnt++;
                }
            }
            unsigned char *q = dst + ((size_t) y * (size_t) dw + (size_t) x) * 4;
            for (int k = 0; k < 3; k++) q[k] = (unsigned char) (sum[k] / cnt);
            q[3] = 255;
        }
    }
    return dst;
}

// the 3D view as a PNG data URL within `max_kb`; NULL when it cannot be made
static char *capture_view(const double max_kb) {
    const int w = g_ed.view_w, h = g_ed.view_h;
    if (w <= 0 || h <= 0) return NULL;
    unsigned char *raw = malloc((size_t) w * (size_t) h * 4);
    if (!raw) return NULL;
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(g_ed.view_x, g_ed.view_y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, raw);
    // GL rows are bottom-up
    unsigned char *row = malloc((size_t) w * 4);
    for (int y = 0; row && y < h / 2; y++) {
        unsigned char *a = raw + (size_t) y * (size_t) w * 4, *b = raw + (size_t) (h - 1 - y) * (size_t) w * 4;
        memcpy(row, a, (size_t) w * 4);
        memcpy(a, b, (size_t) w * 4);
        memcpy(b, row, (size_t) w * 4);
    }
    free(row);
    char *url = NULL;
    for (int dw = w < 480 ? w : 480; dw >= 32 && !url; dw = dw * 3 / 4) {
        const int dh = (int) ((double) h * dw / w) > 0 ? (int) ((double) h * dw / w) : 1;
        unsigned char *small = downscale(raw, w, h, dw, dh);
        if (!small) break;
        int len = 0;
        unsigned char *png = stbi_write_png_to_mem(small, dw * 4, dw, dh, 4, &len);
        free(small);
        if (!png) break;
        char *u = data_url_png(png, len);
        free(png);
        if (u && thumb_kb(u) <= max_kb) url = u;
        else free(u);
    }
    free(raw);
    return url;
}

static void publish_status(const char *text) {
    // io_upMp(text): #gameResHolder / #gameUpResp of the publish popup
    const HtmlElem holder = html_dom_by_id("gameResHolder");
    const HtmlElem resp = html_dom_by_id("gameUpResp");
    if (holder) html_dom_set_style(holder, "display", "");
    if (resp) html_dom_set_html(resp, text);
    if (!resp) editor_message(text);
}

static void publish_close(void) {
    const HtmlElem el = html_dom_by_id("knPrompt");
    if (el) html_dom_remove(el);
}

static void on_social(void *user, const cJSON *frame) {
    (void) user;
    if (!g_ed.active) return;
    const cJSON *label = cJSON_GetArrayItem(frame, 0);
    if (!cJSON_IsString(label)) return;
    const cJSON *msg = cJSON_GetArrayItem(frame, 1);
    const char *text = cJSON_IsString(msg) ? msg->valuestring : "";
    if (!strcmp(label->valuestring, "pubMR")) {
        // io_pubMR(e): io_po("SUCCESS", null, e)
        g_publish_sent = 0;
        publish_close();
        editor_message(text);
    } else if (!strcmp(label->valuestring, "upMp")) {
        if (cJSON_GetArraySize(frame) > 2) return; // mod uploads (io_upMp(e, 1))
        g_publish_sent = 0;
        publish_status(text);
    } else if (!strcmp(label->valuestring, "__close") && g_publish_sent) {
        g_publish_sent = 0;
        publish_status("Upload Request Failed");
    }
}

static void open_publish_dialog(void) {
    // publishPopup(): the "Game" tab (map data from mapToPub = getMapExport())
    publish_close();
    ed_prompt_cancel();
    EdBuf h;
    edb_init(&h);
    edb_add(&h, "<div id='knPrompt'><div class='knPromptBox'><div class='knPromptTitle'>Publish or Update Game</div>");
    edb_add(&h, "<div class='knPromptMsg'>Category <select id='gameCategory2' class='knPromptInput' style='display:inline-block;width:auto'>");
    for (int i = 0; i < MAP_CAT_COUNT; i++) edb_addf(&h, "<option value='%d'>%s</option>", i, k_map_cats[i]);
    edb_add(&h, "</select></div>");
    edb_addf(&h, "<div class='knPromptMsg'>Thumbnail: %s</div>", g_thumb ? "editor view" : "none");
    if (g_thumb) {
        edb_add(&h, "<img src='");
        edb_add(&h, g_thumb);
        edb_add(&h, "' style='display:block;width:240px;margin-bottom:12px'>");
    }
    edb_add(&h, "<input id='gameVideoNew' type='text' placeholder='Youtube Link (Optional)' class='knPromptInput' autocomplete='off' spellcheck='false' style='margin-bottom:8px'>");
    edb_add(&h, "<textarea id='gameDescNew' maxlength='500' placeholder='Description (Optional)' class='knPromptInput' spellcheck='false' style='height:60px'></textarea>");
    edb_add(&h, "<div id='gameResHolder' style='margin-top:8px;display:none'><div id='gameUpResp' style='color:rgba(0,0,0,0.5)'></div></div>");
    edb_add(&h, "<div class='knPromptButtons'><div class='knPromptBtn knPromptOk' onclick='KE.kn.publishOk()'>Publish/Update</div>"
                "<div class='knPromptBtn' onclick='KE.kn.publishCancel()'>Cancel</div></div></div></div>");
    html_dom_insert_html(html_dom_body(), HTML_INSERT_BEFORE_END, h.buf);
    edb_free(&h);
}

static void read_value(const char *id, char *out, const size_t n) {
    out[0] = 0;
    const HtmlElem e = html_dom_by_id(id);
    if (e) html_dom_get_value(e, out, n);
}

static void publish_send(void) {
    // uploadGameAsset(): Na == 0 (a map)
    publish_status("Loading...");
    char cat[16], video[256], desc[600];
    read_value("gameCategory2", cat, sizeof(cat));
    read_value("gameVideoNew", video, sizeof(video));
    read_value("gameDescNew", desc, sizeof(desc));
    const int ci = atoi(cat);
    if (!cat[0] || ci < 0 || ci >= MAP_CAT_COUNT) {
        publish_status("Invalid Category");
        return;
    }
    if (!account_get()) {
        publish_status("Must be logged in");
        return;
    }
    char *map = ed_map_export_string();
    if (!map) return;
    if (strlen(map) > MAP_DATA_MAX) {
        free(map);
        publish_status("Failed. 500k Character Game Data Limit");
        return;
    }
    cJSON *f = cJSON_CreateArray();
    cJSON_AddItemToArray(f, cJSON_CreateString("uploadM"));
    cJSON_AddItemToArray(f, cJSON_CreateString(map));
    cJSON_AddItemToArray(f, g_thumb ? cJSON_CreateString(g_thumb) : cJSON_CreateNull());
    cJSON_AddItemToArray(f, cJSON_CreateString(desc));
    cJSON_AddItemToArray(f, cJSON_CreateString(video));
    cJSON_AddItemToArray(f, cJSON_CreateString(cat));
    free(map);
    if (!g_listening) g_listening = backend_social_listen(on_social, NULL);
    g_publish_sent = backend_social_send(f);
    cJSON_Delete(f);
    if (!g_publish_sent) publish_status("Upload Request Failed");
}

void ed_net_publish(void) {
    // publishMap(): v3.1.1 hands the map to the hub's publish popup (see the notes)
    if (!backend_enabled() || backend_available() == 0) {
        editor_message("Publishing needs the KrunkNative backend (kn_brain); use Export instead.");
        return;
    }
    if (!account_get()) {
        editor_message("Must be logged in"); // publishPopup: io_po(null, "Must be logged in")
        return;
    }
    if (!g_listening) g_listening = backend_social_listen(on_social, NULL);
    g_capture_pending = 1; // the dialog opens once the view was read back (ed_net_frame)
}

// KRUNKNATIVE_EDITOR_SCRIPT "publish[:category]": wait for the account (session restore),
// open the publish dialog, then press Publish/Update (end-to-end test of uploadM)
static int g_auto, g_auto_cat;
static double g_auto_until;

void ed_net_script_publish(const char *arg) {
    g_auto = 1;
    g_auto_cat = arg ? atoi(arg) : 0;
    g_auto_until = g_ed.now + 20.0;
}

static void auto_publish(void) {
    if (g_auto == 1 && account_get()) {
        g_auto = 2;
        ed_net_publish();
    } else if (g_auto == 2 && html_dom_by_id("gameCategory2")) {
        char v[8];
        snprintf(v, sizeof(v), "%d", g_auto_cat);
        html_dom_set_value(html_dom_by_id("gameCategory2"), v);
        html_dom_set_value(html_dom_by_id("gameDescNew"), "Published by the KRUNKNATIVE_EDITOR_SCRIPT test");
        g_auto = 0;
        publish_send();
    } else if (g_auto && g_ed.now > g_auto_until) {
        g_auto = 0;
        editor_message("publish script: no account after 20 s");
    }
}

void ed_net_frame(void) {
    // editor.c page_tick: right after the 3D view and its overlays were drawn
    if (g_auto) auto_publish();
    if (!g_capture_pending) return;
    g_capture_pending = 0;
    free(g_thumb);
    const AccountInfo *a = account_get();
    g_thumb = capture_view(a && a->premium ? MAX_THUMB_P_KB : MAX_THUMB_KB);
    if (g_thumb) fprintf(stderr, "[editor] publish thumbnail %.1f kB\n", thumb_kb(g_thumb));
    open_publish_dialog();
}

// ---------------------------------------------------------------------------
// handlers

static int arg_window(const char *name) {
    // "windows.<n>.<fn>" -> list window
    if (!strncmp(name, "windows.1.", 10)) return LW_ASSETS;
    if (!strncmp(name, "windows.4.", 10)) return LW_MODS;
    if (!strncmp(name, "windows.6.", 10)) return LW_TEXTURES;
    return -1;
}

static void search(const int w) {
    // searchAssets / searchMods: "Loading..." then API_CLIENT.searchAsset(val, cat) / searchMod(val)
    const HtmlElem res = html_dom_by_id(search_results_id(w));
    if (res) html_dom_set_html(res, "Loading...");
    char val[256], cat[16] = "16", path[600];
    read_value(w == LW_ASSETS ? "asSrch" : w == LW_MODS ? "mSrch" : "astSrch", val, sizeof(val));
    if (w == LW_ASSETS) read_value("asCat", cat, sizeof(cat));
    if (w == LW_MODS) {
        snprintf(path, sizeof(path), "/api/search");
        q_add(path, sizeof(path), "type", "mod");
        q_add(path, sizeof(path), "val", val);
    } else {
        snprintf(path, sizeof(path), "/api/assets");
        q_int(path, sizeof(path), "index", 2);
        q_add(path, sizeof(path), "cat", cat);
        q_add(path, sizeof(path), "val", val);
    }
    if (!request(w, 0, 1, path)) search_loaded(w, NULL);
}

static int h_net(void *u, const HtmlEvent *ev, const HtmlArg *a, const int n) {
    (void) u; (void) ev;
    const char *name = html_ui_current_handler();
    if (!name) return 1;
    const int w = arg_window(name);
    if (w >= 0 && strstr(name, ".switchTab")) {
        const int tab = n >= 1 ? (int) a[0].num : 0;
        const int max = w == LW_MODS ? MOD_TAB_COUNT : ASSET_TAB_COUNT;
        if (tab >= 0 && tab < max && tab != g_win[w].tab) {
            g_win[w].tab = tab;
            editor_show_window(g_win[w].window);
        }
    } else if (w >= 0 && (strstr(name, ".searchAssets") || strstr(name, ".searchMods"))) {
        search(w);
    } else if (!strcmp(name, "KE.selectAsset")) {
        char id[96] = "";
        if (n >= 1 && a[0].kind == HTML_ARG_STR && a[0].str) snprintf(id, sizeof(id), "%s", a[0].str);
        else if (n >= 1 && a[0].kind == HTML_ARG_NUM) snprintf(id, sizeof(id), "%.15g", a[0].num);
        select_asset(id, n >= 2 ? (int) a[1].num : 0);
    } else if (!strcmp(name, "KE.selectMod")) {
        select_mod(n >= 1 && a[0].kind == HTML_ARG_STR ? a[0].str : "");
    } else if (!strcmp(name, "openPub")) {
        // openPub(): openLink("/social.html?p=maps&openPub", "_blank")
        app_route_url("/social.html?p=maps&openPub");
    } else if (!strcmp(name, "KE.kn.publishOk")) {
        publish_send();
    } else if (!strcmp(name, "KE.kn.publishCancel")) {
        publish_close();
    }
    return 1;
}

int ed_net_keydown(const HtmlEvent *ev) {
    // onkeydown='if(window.event.keyCode==13)windows[n].search...()' (statements are not run
    // by the page bridge: the editor's keydown listener calls this). 1 = handled.
    if (!ev || !ev->target || !html_key_is_enter(ev->key)) return 0;
    char id[16] = "";
    if (html_dom_get_attr(ev->target, "id", id, sizeof(id)) < 0) return 0;
    if (!strcmp(id, "asSrch")) search(LW_ASSETS);
    else if (!strcmp(id, "mSrch")) search(LW_MODS);
    else if (!strcmp(id, "astSrch")) search(LW_TEXTURES);
    else return 0;
    return 1;
}

void ed_net_reset(void) {
    // page enter / leave: listData is per page load in v3.1.1
    for (int w = 0; w < LW_COUNT; w++) {
        for (int t = 0; t < MOD_TAB_COUNT; t++) list_clear(&g_win[w].list[t]);
        g_win[w].tab = 0;
        g_win[w].type = 0;
        g_win[w].gen++;
    }
    free(g_thumb);
    g_thumb = NULL;
    g_capture_pending = 0;
    g_publish_sent = 0;
}

void ed_net_install(HtmlHandlerEntry *table, size_t *count, const size_t max) {
    ed_net_reset();
    const HtmlHandlerEntry entries[] = {
        {"windows.1.switchTab", h_net}, {"windows.4.switchTab", h_net}, {"windows.6.switchTab", h_net},
        {"windows.1.searchAssets", h_net}, {"windows.4.searchMods", h_net}, {"windows.6.searchAssets", h_net},
        {"KE.selectAsset", h_net}, {"KE.selectMod", h_net}, {"openPub", h_net},
        {"KE.kn.publishOk", h_net}, {"KE.kn.publishCancel", h_net},
    };
    for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]) && *count < max; i++) table[(*count)++] = entries[i];
}
