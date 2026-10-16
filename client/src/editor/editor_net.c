// Map editor: Krunker v7.1.4's API-backed windows, asset selection and publishing (editor.js
// window.windows[1] Assets, [4] Mods, [6] Textures, [8] Sounds, [13] Maps (My Games); f954 /
// f955 / f956 card builders; KE.selectAsset / selectMod / viewAssets / viewModels /
// viewTextures / viewSounds / myAssets; publishMap / preloadMap / publishGamePopup /
// uploadGame / publishMapResp / mapResp; publishAssetPopup / updateAssetType / uploadAsset;
// openEdit / loadMap / grabMap / editMap / updateMapLB / calcMapFee / chngMapFunds /
// mapFundsRes; deleteCreationConf / deleteCreation; testSound / copyAssetID; io_po / f959).
// doc/v7/editor.md 1.10 and WP6.
//
// Everything goes through kn_brain (doc/backend.md 7.4 / 7.5); the client never touches the
// database. HTTP: GET /api/assets?index=&cat=&username= (listAssets), ?index=2&cat=&val=
// (searchAsset), /api/mods?index=&pos=&accountId= (listMods), /api/search?type=mod&val=
// (searchMod), /api/maps?index=4&pos=0&accountId= (listMaps, My Games). Social socket:
// uploadM -> pubMR / upMp, uploadA -> pubR / upMp, edMp -> edMp, delC -> po, ulb, mfnds -> mfr.
//
// Intentional differences from v7.1.4:
// - Without a brain (guest build, no kn_brain answering, HTTP error) v7's fetch fails and the
//   list shows "Loading..." forever; here it shows the window's empty text ("No Assets found",
//   "No Mods found", "No Textures found", "No Sounds found", "No Games found!").
// - Tab indices: v7's config.modTabs / mapTabs order (Home, Popular, ...) is not the brain's
//   /api index space (0 Popular, 1 Featured, 2 New, 3 Favorites, 4 My Games / My Mods; the same
//   mapping as the hub, social_ui.c). The asset lists send `username` only on the My Assets
//   tab (config.assetTabs sendUser): kn_brain filters by it whenever it is given.
// - Thumbnails, textures and sounds live under assets/user-assets/<id>/ (the v7 import), not on
//   user-assets.krunker.io: natively a card shows the local file when it exists, else
//   ./img/noimg.png (v7's onerror fallback); the web build uses the asset host's URL. A map card
//   shows the thumbnail kn_brain stored with the map (map_image, a PNG data URL).
// - Prefab assets (selectAsset type 3) read user-assets/<id>/data.json from the asset tree.
// - publishMap sends "plm" in v7; kn_brain has no such message, so the editor looks the map
//   name up in the account's My Games list (/api/maps index 4) instead: found = "Update Game"
//   with that row (description, version), else "Publish Game".
// - uploadM: kn_brain takes the v3.1.1 shape [map, thumbDataUrl, description, video, category]
//   with the thumbnail inline (it hosts no /upload-user-asset), followed by v7's Post to Hub and
//   Mobile Friendly flags (kn_brain ignores them for now). The thumbnail defaults to a PNG of the
//   3D view (v7 needs a picked file); Select picks a PNG instead. kn_brain keeps thumbnails up to
//   200 kB of data URL, so larger ones are refused here.
// - uploadA is sent as v7 sends it; kn_brain answers ["upMp", "File uploads are not supported on
//   this server"] (no asset hosting), shown in the popup. A pubR reply (a server that hosts
//   files) is answered with "Upload Request Failed": the client has no multipart POST.
// - updateMapLB (ulb) is v10's: no reply timeout, so the "Please wait..." popup stays until the
//   user closes it, and the upMp reply goes through uploadResp into #gameUpResp, which the
//   leaderboard popup does not have (nothing shows). kn_brain stores the settings on the map's
//   LEADERBOARD objects and answers upMp "Leaderboard Updated!" (creations.c sh_update_map_lb).
// - chngMapFunds (mfnds) is sent as in v7; a server that never answers (kn_brain has neither
//   yet) ends the "Please Wait..." popup after REPLY_TIMEOUT_S with an error instead of waiting
//   forever.
// - edMp with an unknown map ("edMp", null) shows "Map not found" (v7 ignores it).
// - The Sounds window's empty / error text goes to its own list (v7 writes #assetTList).
// - v7's mapResp share link and the update video field come from an archive.org capture
//   ("https://web.archive.org/web/.../https://youtube.com/..."): the plain URLs are used.
// - Asset ids are written into the cards without ' and " (v7 inserts them raw).
#include "editor_internal.h"
#include "editor_model.h"
#include <app_mode.h>
#include <audio.h>
#include <backend.h>
#include <backend_ui.h>
#include <gl.h>
#include <html_ui.h>
#include <map_validate.h> // publishMap: validateMap before the upload popup
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
    "Walls & Blocks", "Textures", "Sounds", "Triggers",
};
#define ASSET_CAT_COUNT ((int) (sizeof(k_asset_cat) / sizeof(k_asset_cat[0])))

static const char *const k_map_cats[] = {
    "Other", "Parkour", "Puzzle", "Trading", "Training", "Social", "Horror", "Adventure", "Battle Royale", "Realism", "Roleplay", "Rotation",
    "Racing", "Raid", "Remake", "NCR", "Zombies", "Simulation", "Relaxation", "Action", "Tycoon", "Mini-Games", "Single-player", "Education",
};
#define MAP_CAT_COUNT ((int) (sizeof(k_map_cats) / sizeof(k_map_cats[0])))

// config.assetTabs: New, My Assets (sendUser), Search
static const char *const k_asset_tabs[] = {"New", "My Assets", "Search"};
#define ASSET_TAB_COUNT 3
#define ASSET_TAB_MINE 1
#define ASSET_TAB_SEARCH 2

// config.modTabs (v7.1.4); api = the brain's /api/mods index (see the notes), -1 none
static const struct { const char *n; int api, search, no_load; } k_mod_tabs[] = {
    {"Home", 1, 0, 0}, {"Popular", 0, 0, 0}, {"New", 2, 0, 0}, {"Favorites", 3, 0, 0}, {"My Mods", 4, 0, 0}, {"Loaded", -1, 0, 1}, {"Search", -1, 1, 0},
};
#define MOD_TAB_COUNT ((int) (sizeof(k_mod_tabs) / sizeof(k_mod_tabs[0])))
#define MAP_TAB_OWNED 5 // config.mapTabs.findIndex(t => t.owned): My Games
#define MAP_API_OWNED 4 // the brain's My Games index

// config.assetThumb(P) / assetIcon (kB), assetSize(P) / assetTexture* / assetSound / assetVideo (MB)
#define ASSET_THUMB 250
#define ASSET_THUMB_P 500
#define ASSET_ICON 100
#define ASSET_SIZE 100
#define ASSET_SIZE_P 250
#define ASSET_TEXTURE 10
#define ASSET_SOUND 5
#define ASSET_VIDEO 20
#define MAP_DATA_MAX 500000
#define BRAIN_THUMB_MAX (200 * 1024) // kn_brain's limit of a thumbnail data URL (creations.c THUMB_MAX)
#define REPLY_TIMEOUT_S 8.0
#define USER_ASSETS_URL "https://user-assets.krunker.io"

// ---------------------------------------------------------------------------
// small helpers

static int brain_ok(void) { return backend_enabled() && backend_available() == 1; }

// KE.account: the hub session (account_ui.c); net:popup renders with a free stand-in account, as
// v7ref.js's net-* captures fake one
static AccountInfo g_fake_account;
static int g_fake;

static const AccountInfo *account(void) { return g_fake ? &g_fake_account : active_account(); }

static void cleanse(EdBuf *b, const char *s) {
    for (; s && *s; s++) {
        if (*s == '\'' || *s == '"') continue;
        const char c[2] = {*s, 0};
        edb_add(b, c);
    }
}

// UTILS.escapeStr: \ " ' get a backslash
static void escape_str(EdBuf *b, const char *s) {
    for (; s && *s; s++) {
        const char c[3] = {'\\', *s, 0};
        edb_add(b, *s == '\\' || *s == '"' || *s == '\'' ? c : c + 1);
    }
}

static const char *row_str(const cJSON *row, const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(row, key);
    return cJSON_IsString(v) ? v->valuestring : "";
}

static void add_num_or_str(EdBuf *b, const cJSON *v) {
    if (cJSON_IsNumber(v)) edb_addf(b, "%.15g", v->valuedouble);
    else if (cJSON_IsString(v)) edb_add(b, v->valuestring);
}

static void js_number(EdBuf *b, const double v) {
    char s[40];
    ed_model_number(v, s, sizeof(s));
    edb_add(b, s);
}

// UTILS.getAssetData: JSON.parse or {}
static cJSON *asset_data(const cJSON *v) {
    cJSON *n = cJSON_IsString(v) ? cJSON_Parse(v->valuestring) : cJSON_IsObject(v) ? cJSON_Duplicate(v, 1) : NULL;
    if (!cJSON_IsObject(n)) {
        cJSON_Delete(n);
        n = cJSON_CreateObject();
    }
    return n;
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

// UTILS.formatBytes(bytes, decimals)
static void format_bytes(EdBuf *b, const double bytes, const int decimals) {
    if (!(bytes > 0) && !(bytes < 0)) {
        edb_add(b, "0B");
        return;
    }
    static const char *const units[] = {"B", "KB", "MB", "GB", "TB", "PB", "EB", "ZB", "YB"};
    int i = (int) floor(log(fabs(bytes)) / log(1000));
    if (i < 0) i = 0;
    if (i > 8) i = 8;
    const double p = pow(10, decimals < 0 ? 0 : decimals);
    js_number(b, round(bytes / pow(1000, i) * p) / p);
    edb_add(b, units[i]);
}

// UTILS.commaFormatNum
static void comma_num(EdBuf *b, const double v) {
    char s[40], o[60];
    ed_model_number(v, s, sizeof(s));
    const char *dot = strchr(s, '.');
    const size_t ilen = dot ? (size_t) (dot - s) : strlen(s);
    size_t j = 0;
    for (size_t i = 0; i < ilen && j + 2 < sizeof(o); i++) {
        o[j++] = s[i];
        const size_t left = ilen - i - 1;
        if (left && left % 3 == 0 && s[i] != '-') o[j++] = ',';
    }
    o[j] = 0;
    edb_add(b, o);
    if (dot) edb_add(b, dot);
}

// a user asset file's <img> src: the asset tree's copy natively when it exists, else noimg
static void user_asset_src(EdBuf *b, const char *rel) {
    char url[512];
#ifdef __EMSCRIPTEN__
    html_dom_asset_url(rel, url, sizeof(url));
    edb_add(b, url);
#else
    char path[1024];
    snprintf(path, sizeof(path), "%s%s", assets_path(), rel);
    FILE *f = fopen(path, "rb");
    if (f) {
        fclose(f);
        html_dom_asset_url(rel, url, sizeof(url));
        edb_add(b, url);
    } else {
        edb_add(b, "./img/noimg.png");
    }
#endif
}

static void read_value(const char *id, char *out, const size_t n) {
    out[0] = 0;
    const HtmlElem e = html_dom_by_id(id);
    if (e) html_dom_get_value(e, out, n);
}

static int read_checked(const char *id) {
    const HtmlElem e = html_dom_by_id(id);
    return e ? html_dom_get_checked(e) : 0;
}

// ---------------------------------------------------------------------------
// popups: io_po / f959 (genericPopup)

static void io_po(const char *type, const char *err, const char *msg) {
    EdBuf h;
    edb_init(&h);
    if (type && !strcmp(type, "SUCCESS"))
        edb_addf(&h, "<div style='width:100%%;text-align:center'><div><i class='material-icons' style='font-size:80px;color:#3be354!important;'>done</i></div>"
                     "<div style='text-align:center;font-size:20px;color:rgba(255,255,255,0.5)'>%s</div></div>", msg ? msg : "Success!");
    else if (type && !strcmp(type, "INFO"))
        edb_addf(&h, "<div style='width:100%%;text-align:center'><div><i class='material-icons' style='font-size:80px;color:rgba(255,255,255,0.9)!important;'>info</i></div>"
                     "<div style='text-align:center;font-size:20px;color:rgba(255,255,255,0.9)'>%s</div></div>", msg ? msg : "Warning!");
    else
        edb_addf(&h, "<div style='width:100%%;text-align:center'><div><i class='material-icons' style='font-size:80px;color:#e33b3b!important;'>clear</i></div>"
                     "<div style='text-align:center;font-size:20px;color:rgba(255,255,255,0.5)'>%s</div></div>", err ? err : "Failed. Try Again");
    ed_generic_popup(h.buf, type && !strcmp(type, "INFO") ? "infoPop" : "responsePop");
    edb_free(&h);
}

void ed_response_popup(const char *type, const char *err, const char *msg) { io_po(type, err, msg); }

static void wait_popup(const char *text) {
    char h[512];
    snprintf(h, sizeof(h), "<div style=\"width:100%%;text-align:center\"><div class=\"lds-ring sm\"><div></div><div></div><div></div><div></div></div>"
                           "<div style=\"font-size:20px;color:rgba(255,255,255,0.5)\">%s</div></div>", text);
    ed_generic_popup(h, "loadingPop");
}

// KE.uploadResp(text, mod): #gameResHolder / #gameUpResp of the publish popups
static void upload_resp(const char *text) {
    const HtmlElem holder = html_dom_by_id("gameResHolder");
    const HtmlElem resp = html_dom_by_id("gameUpResp");
    if (holder) html_dom_set_style(holder, "display", "");
    if (resp) html_dom_set_html(resp, text);
}

// ---------------------------------------------------------------------------
// social socket requests that wait for a reply

static int g_listening;
static char g_wait[16];       // the reply label a request waits for ("" none)
static double g_wait_until;
static int g_wait_upload;     // the wait belongs to a publish popup (replies go to #gameUpResp)

static void on_social(void *user, const cJSON *frame);

static int social_send(cJSON *f, const char *reply, const int upload) {
    if (!g_listening) g_listening = backend_social_listen(on_social, NULL);
    const int ok = backend_social_send(f);
    cJSON_Delete(f);
    if (ok) {
        snprintf(g_wait, sizeof(g_wait), "%s", reply ? reply : "");
        g_wait_until = g_ed.now + REPLY_TIMEOUT_S;
        g_wait_upload = upload;
    }
    return ok;
}

static void wait_done(void) {
    g_wait[0] = 0;
    g_wait_upload = 0;
}

static void wait_failed(const char *text) {
    const int upload = g_wait_upload;
    wait_done();
    if (upload) upload_resp(text);
    else io_po(NULL, text, NULL);
}

static cJSON *frame_new(const char *label) {
    cJSON *f = cJSON_CreateArray();
    cJSON_AddItemToArray(f, cJSON_CreateString(label));
    return f;
}

// ---------------------------------------------------------------------------
// list windows (windows[1] Assets, [4] Mods, [6] Textures, [8] Sounds, [13] Maps)

enum { LW_ASSETS, LW_MODS, LW_TEXTURES, LW_SOUNDS, LW_MAPS, LW_COUNT };
#define LW_TABS 8

static const struct {
    int window, js;             // showWindow index, windows[] index
    const char *list_id, *none, *results, *input;
    int cat;                    // listAssets / searchAsset category (-1 all)
} k_lw[LW_COUNT] = {
    {2, 1, "assetList", "No Assets found", "searchResultsA", "asSrch", -1},
    {5, 4, "modList", "No Mods found", "searchResultsM", "mSrch", 0},
    {7, 6, "assetTList", "No Textures found", "searchResultsT", "astSrch", 16},
    {9, 8, "assetSList", "No Sounds found", "searchResultsS", "assSrch", 17},
    {14, 13, "mapList", "No Games found!", NULL, NULL, 0},
};

typedef struct {
    int requested;   // listData[tab] exists
    int waiting;     // listData[tab].waiting
    char *dat;       // listData[tab].dat (cards so far)
} ListData;

typedef struct {
    int tab;
    int sort;        // sortIndex (Assets, Textures): 1 = alphabetical
    int only_show;   // windows[1].onlyShow as a bit set of asset kinds; 0 = null (all)
    char from[96];   // gen(from): the selectAsset action of the cards ("" = undefined)
    ListData list[LW_TABS];
    unsigned gen;    // bumps when the lists are reset: drops stale replies
} ListWin;

static ListWin g_win[LW_COUNT];
static int g_keep_from; // a re-render (switchTab, a reply) or an opener with its own from

static void list_clear(ListData *d) {
    free(d->dat);
    memset(d, 0, sizeof(*d));
}

static int lw_of_window(const int index) {
    for (int w = 0; w < LW_COUNT; w++) if (k_lw[w].window == index) return w;
    return -1;
}

// showWindow(n, false, this.from): keeps the window's from
static void rerender(const int w) {
    g_keep_from = 1;
    editor_show_window(k_lw[w].window);
    g_keep_from = 0;
}

static void open_list(const int w, const char *from) {
    snprintf(g_win[w].from, sizeof(g_win[w].from), "%s", from ? from : "");
    rerender(w);
}

// f954(rows, onlyShow, from, sort): asset cards
static int cmp_nm(const void *a, const void *b) {
    const char *x = row_str(*(const cJSON *const *) a, "nm"), *y = row_str(*(const cJSON *const *) b, "nm");
    return strcmp(x, y);
}

static void cards_list(EdBuf *b, const cJSON *rows, const int only_show, const char *from, const int sort) {
    const int n = cJSON_GetArraySize(rows);
    const cJSON **list = n > 0 ? malloc((size_t) n * sizeof(*list)) : NULL;
    if (n > 0 && !list) return;
    for (int i = 0; i < n; i++) list[i] = cJSON_GetArrayItem(rows, i);
    if (sort && n > 1) qsort(list, (size_t) n, sizeof(*list), cmp_nm);
    const AccountInfo *acc = account();
    for (int i = 0; i < n; i++) {
        const cJSON *row = list[i];
        cJSON *d = asset_data(cJSON_GetObjectItemCaseSensitive(row, "dat"));
#define FLAG(k) ed_json_truthy(cJSON_GetObjectItemCaseSensitive(d, k))
        int kind = 0;
        if (FLAG("d")) kind = 3;
        if (FLAG("x")) kind = 4;
        if (FLAG("u")) kind = 5;
        if (only_show && !(only_show & 1 << kind)) {
            cJSON_Delete(d);
            continue;
        }
        EdBuf raw, id, idn;
        edb_init(&raw);
        edb_init(&id);
        edb_init(&idn);
        add_num_or_str(&idn, cJSON_GetObjectItemCaseSensitive(row, "id"));
        edb_add(&raw, idn.buf ? idn.buf : "");
        if (kind < 3) {
            const cJSON *k;
            cJSON_ArrayForEach(k, d) edb_add(&raw, k->string);
        }
        cleanse(&id, raw.buf ? raw.buf : "");
        const char *aid = id.buf ? id.buf : "", *num = idn.buf ? idn.buf : "0";
        char rel[160];
        EdBuf src;
        edb_init(&src);
        if (FLAG("x") && !FLAG("v")) {
            snprintf(rel, sizeof(rel), "user-assets/%s/texture.png", num);
            user_asset_src(&src, rel);
        } else if (FLAG("t")) {
            snprintf(rel, sizeof(rel), "user-assets/%s/thumb.png", num);
            user_asset_src(&src, rel);
        } else {
            edb_add(&src, "./img/noimg.png");
        }
        const int owner = acc && acc->name[0] && !strcmp(acc->name, row_str(row, "plrn"));
        const int show_cat = !FLAG("v") && !FLAG("u") && !FLAG("x");
        edb_addf(b, "<div class='assetItem' oncontextmenu='copyAssetID(\"%s\")'>\n\t\t\t", aid);
        if (FLAG("u")) edb_addf(b, "<span class='material-icons testSound' onclick='testSound(%s)'>play_arrow</span>", num);
        edb_addf(b, "\n\t\t\t<div class='assetItemHolder'>\n\t\t\t\t<img class='assetItemThumb' draggable=\"false\" loading='lazy' src='%s'\n"
                    "\t\t\t\t\tonerror='this.onerror=null;this.src=\"./img/noimg.png\";' onclick='KE.selectAsset(\"%s\",%d,\"%s\")'/>\n"
                    "\t\t\t\t<div class='assetItemText' style='top:4px'>\n\t\t\t\t\t",
                 src.buf ? src.buf : "", aid, kind, from);
        if (owner)
            edb_addf(b, "<span title='Delete' class='material-icons creationAction del'style='vertical-align: middle;margin-top: -6px;'onclick='deleteCreationConf(%s,2)'>delete_forever</span>", num);
        edb_addf(b, "\n\t\t\t\t\t%s\n\t\t\t\t</div>\n\t\t\t\t<div class='assetItemText' style='top:35px;'>%s</div>\n"
                    "\t\t\t\t<div class='assetItemText' style='top:95px;left:6px;right:auto;'>\n\t\t\t\t\t",
                 FLAG("v") ? "Video" : FLAG("u") ? "Sound" : FLAG("d") ? "Prefab" : FLAG("x") ? "Texture" : "Model", aid);
        const cJSON *siz = cJSON_GetObjectItemCaseSensitive(row, "siz");
        format_bytes(b, (cJSON_IsNumber(siz) ? siz->valuedouble : cJSON_IsString(siz) ? atof(siz->valuestring) : 0) * 1000, 0);
        edb_add(b, "\n\t\t\t\t</div>\n\t\t\t\t");
        if (show_cat) {
            int cat = (int) ed_json_num(row, "cat", 0);
            edb_addf(b, "<div class='assetItemText' style='top:95px;'>%s</div>", cat >= 0 && cat < ASSET_CAT_COUNT ? k_asset_cat[cat] : "undefined");
        }
        edb_addf(b, "\n\t\t\t</div>\n\t\t\t<div class='assetItemName' onclick='KE.selectAsset(\"%s\",%d,\"%s\")'/>%s</div>\n\t\t</div>", aid, kind, from,
                 row_str(row, "nm"));
#undef FLAG
        edb_free(&src);
        edb_free(&raw);
        edb_free(&id);
        edb_free(&idn);
        cJSON_Delete(d);
    }
    free(list);
}

// f955(rows): My Games cards
static void maps_list(EdBuf *b, const cJSON *rows) {
    const cJSON *row;
    cJSON_ArrayForEach(row, rows) {
        EdBuf id, name;
        edb_init(&id);
        edb_init(&name);
        add_num_or_str(&id, cJSON_GetObjectItemCaseSensitive(row, "map_id"));
        escape_str(&name, row_str(row, "map_name"));
        const char *mid = id.buf ? id.buf : "0";
        edb_addf(b, "<div class='mapListItem'>\n\t\t\t<div class='mapActionOvrl'>\n\t\t\t\t<div class='mapActionHol'>\n"
                    "\t\t\t\t\t<div class='mapActionB' onclick='openEdit(&quot;%s&quot;\n\t\t\t\t\t,%s,",
                 name.buf ? name.buf : "", mid);
        const cJSON *fund = cJSON_GetObjectItemCaseSensitive(row, "fund");
        if (cJSON_IsNumber(fund) || cJSON_IsString(fund)) add_num_or_str(b, fund);
        else edb_add(b, "undefined");
        edb_addf(b, ")' title='Edit' style='margin-top:0px;'>\n\t\t\t\t\t\t<span class='material-icons' style='font-size:50px;color:#fff;'>create</span>\n"
                    "\t\t\t\t\t</div>\n\t\t\t\t\t<div class='mapActionSep' style='margin-left:2px'></div>\n"
                    "\t\t\t\t\t<div class='mapActionB' onclick='deleteCreationConf(%s,0)' title='Delete' style='margin-top:0px;'>\n"
                    "\t\t\t\t\t\t<span class='material-icons' style='font-size:50px;color:#fff;'>delete_forever</span>\n\t\t\t\t\t</div>\n\t\t\t\t</div>\n\t\t\t</div>\n"
                    "\t\t\t<div class='mapListThumbHolder'>\n\t\t\t\t<img class='mapListThumb' loading='lazy' src='",
                 mid);
        const char *image = row_str(row, "map_image");
        if (!strncmp(image, "data:image/", 11)) edb_add(b, image);
        else edb_add(b, "./img/noimg.png");
        edb_addf(b, "'\n\t\t\t\t\tonerror='this.onerror=null;this.src=\"./img/noimg.png\";'/>\n\t\t\t</div>\n\t\t\t<div class='mapListName'>%s</div>\n\t\t</div>",
                 row_str(row, "map_name"));
        edb_free(&id);
        edb_free(&name);
    }
}

// f956(rows): mod cards
static void mods_list(EdBuf *b, const cJSON *rows) {
    const cJSON *row;
    cJSON_ArrayForEach(row, rows) {
        // the short row keys (dte, mna, mid, minf, ctn, url, vte, upd)
        const char *date = row_str(row, "dte")[0] ? row_str(row, "dte") : row_str(row, "mod_date");
        const char *name = row_str(row, "mna")[0] ? row_str(row, "mna") : row_str(row, "mod_name");
        const cJSON *mid = cJSON_GetObjectItemCaseSensitive(row, "mid");
        if (!mid || cJSON_IsNull(mid)) mid = cJSON_GetObjectItemCaseSensitive(row, "mod_id");
        const cJSON *minf = cJSON_GetObjectItemCaseSensitive(row, "minf");
        if (!minf || cJSON_IsNull(minf)) minf = cJSON_GetObjectItemCaseSensitive(row, "mod_info");
        const char *url = row_str(row, "url")[0] ? row_str(row, "url") : row_str(row, "mod_url");
        const double t = age_days(date);
        cJSON *info = asset_data(minf);
        EdBuf u;
        edb_init(&u);
        if (!strcmp(url, "ua")) {
            edb_add(&u, USER_ASSETS_URL "/md");
            add_num_or_str(&u, mid);
            edb_add(&u, "/mod.zip");
        } else {
            cleanse(&u, url);
        }
        edb_add(b, "<div class='mapListItem'>\n\t\t\t");
        if (t >= 0 && t <= 7) edb_add(b, "<div class='mapAgeD' style='background-color:#E040FB'>NEW</div>");
        edb_add(b, "\n\t\t\t<div class='mapListThumbHolder'>\n\t\t\t\t<img class='mapListThumb' loading='lazy' src='");
        const char *img = row_str(row, "mod_image");
        if (ed_json_truthy(cJSON_GetObjectItemCaseSensitive(info, "t"))) {
            EdBuf rel;
            edb_init(&rel);
            edb_add(&rel, "user-assets/md");
            add_num_or_str(&rel, mid);
            edb_add(&rel, "/thumb.png");
            user_asset_src(b, rel.buf);
            edb_free(&rel);
        } else {
            edb_add(b, img[0] && (!strncmp(img, "data:", 5) || !strncmp(img, "./", 2)) ? img : "./img/noimg.png");
        }
        edb_addf(b, "'\n\t\t\t\t\tonerror='this.onerror=null;this.src=\"./img/noimg.png\";' onclick='KE.selectMod(\"%s\")'/>\n"
                    "\t\t\t</div>\n\t\t\t<div class='mapListName'>%s</div>\n\t\t</div>",
                 u.buf ? u.buf : "", name);
        edb_free(&u);
        cJSON_Delete(info);
    }
}

// ---------------------------------------------------------------------------
// requests and replies (f945..f953: assetsLoaded / modsLoaded / mapsLoaded / searchResp)

typedef struct {
    int w, tab, search;
    unsigned gen;
} Req;

static void loaded(int w, int tab, const cJSON *data, const char *error);
static void search_loaded(int w, const cJSON *data);

static void on_http(void *user, const int status, const char *body, const size_t length) {
    Req *r = user;
    const int w = r->w, tab = r->tab, search = r->search;
    const unsigned gen = r->gen;
    free(r);
    if (!g_ed.active || gen != g_win[w].gen) return;
    cJSON *res = status == 200 && body ? cJSON_ParseWithLength(body, length) : NULL;
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(res, "data");
    const cJSON *err = cJSON_GetObjectItemCaseSensitive(res, "error");
    if (search) search_loaded(w, cJSON_IsArray(data) ? data : NULL);
    else loaded(w, tab, cJSON_IsArray(data) ? data : NULL, cJSON_IsString(err) && err->valuestring[0] ? err->valuestring : NULL);
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
    if (!value) return; // URLSearchParams drops null values
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

// API_CLIENT.listAssets / listMods / listMaps
static int list_request(const int w, const int tab) {
    char path[512];
    const AccountInfo *a = account();
    if (w == LW_MODS) {
        if (k_mod_tabs[tab].api < 0) return 0;
        snprintf(path, sizeof(path), "/api/mods");
        q_int(path, sizeof(path), "index", k_mod_tabs[tab].api);
        q_int(path, sizeof(path), "pos", 0);
        if (a) q_int(path, sizeof(path), "accountId", a->id);
    } else if (w == LW_MAPS) {
        if (!a) return 0; // My Games of nobody: the brain's index 4 without an account lists every map
        snprintf(path, sizeof(path), "/api/maps");
        q_int(path, sizeof(path), "index", MAP_API_OWNED);
        q_int(path, sizeof(path), "pos", 0);
        q_int(path, sizeof(path), "accountId", a->id);
    } else {
        snprintf(path, sizeof(path), "/api/assets");
        q_int(path, sizeof(path), "index", tab);
        q_int(path, sizeof(path), "cat", k_lw[w].cat);
        if (tab == ASSET_TAB_MINE && a) q_add(path, sizeof(path), "username", a->name);
        else if (tab == ASSET_TAB_MINE) return 0;
    }
    return request(w, tab, 0, path);
}

static void set_list_html(const int w, const char *html) {
    const HtmlElem e = html_dom_by_id(k_lw[w].list_id);
    if (e) html_dom_set_html(e, html);
}

static void loaded(const int w, const int tab, const cJSON *data, const char *error) {
    ListWin *lw = &g_win[w];
    if (tab < 0 || tab >= LW_TABS) return;
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
        else if (w == LW_MAPS) maps_list(&b, data);
        else cards_list(&b, data, w == LW_ASSETS ? lw->only_show : 0, lw->from, w == LW_SOUNDS ? 0 : lw->sort);
        free(d->dat);
        d->dat = b.buf ? b.buf : strdup("");
        d->requested = 1;
        if (lw->tab == tab && g_ed.window_index == k_lw[w].window) rerender(w);
    } else if (!d->dat) {
        set_list_html(w, k_lw[w].none);
        list_clear(d);
    }
}

static void search_loaded(const int w, const cJSON *data) {
    EdBuf b;
    edb_init(&b);
    if (data && cJSON_GetArraySize(data) > 0) {
        edb_add(&b, "<div style='height:10px'></div>");
        if (w == LW_MODS) mods_list(&b, data);
        else cards_list(&b, data, w == LW_ASSETS ? g_win[w].only_show : 0, g_win[w].from, 0);
    } else {
        edb_add(&b, k_lw[w].none);
    }
    const HtmlElem e = k_lw[w].results ? html_dom_by_id(k_lw[w].results) : 0;
    if (e) html_dom_set_html(e, b.buf ? b.buf : "");
    edb_free(&b);
}

// ---------------------------------------------------------------------------
// gen()

static void tabs(EdBuf *b, const int w) {
    const ListWin *lw = &g_win[w];
    const int js = k_lw[w].js;
    edb_add(b, "\n\t\t\t<div class='menuTabsNew'>\n\t\t\t\t");
    const int n = w == LW_MODS ? MOD_TAB_COUNT : ASSET_TAB_COUNT;
    for (int t = 0; t < n; t++) {
        if (w == LW_MODS && k_mod_tabs[t].no_load) continue;
        const char *name = w == LW_MODS ? k_mod_tabs[t].n
                         : t == 1 && w == LW_TEXTURES ? "My Textures"
                         : t == 1 && w == LW_SOUNDS ? "My Sounds" : k_asset_tabs[t];
        edb_addf(b, "<div class=\"menuTabNew %s\" onclick=\"windows[%d].switchTab(%d)\">%s</div>", lw->tab == t ? " tabANew" : "", js, t, name);
    }
    edb_add(b, "\n\t\t\t</div>");
}

static int is_search_tab(const int w, const int tab) {
    if (w == LW_MAPS) return 0;
    return w == LW_MODS ? k_mod_tabs[tab].search : tab == ASSET_TAB_SEARCH;
}

// the list part of gen(): the listData state machine (Maps keeps its list: v7's gen never drops it)
static void list_body(EdBuf *t, const int w) {
    ListWin *lw = &g_win[w];
    ListData *d = &lw->list[lw->tab];
    if (d->requested) {
        if (d->dat) {
            edb_add(t, d->dat);
            if (d->waiting) edb_add(t, "<div class='loadMoreW'>Loading...</div>");
            else if (w != LW_MAPS) list_clear(d);
        } else {
            edb_add(t, "<div class='loadMoreW'>Loading...</div>");
        }
    } else {
        d->requested = 1;
        d->waiting = 0;
        if (list_request(w, lw->tab)) {
            edb_add(t, "<div class='loadMoreW'>Loading...</div>");
        } else {
            edb_add(t, k_lw[w].none); // no brain / no account: what an empty reply shows
            list_clear(d);
        }
    }
}

static void sort_select(EdBuf *e, const int w) {
    const int s = g_win[w].sort;
    edb_addf(e, "\n\t\t\t\t\t<div class='assetSort'>\n\t\t\t\t\t\t<select onchange=\"windows[%d].updateSort(this)\">\n"
                "\t\t\t\t\t\t\t<option value=\"0\"%s>New/Updated</option>\n\t\t\t\t\t\t\t<option value=\"1\"%s>Alphabetical</option>\n"
                "\t\t\t\t\t\t</select>\n\t\t\t\t\t</div>",
             k_lw[w].js, s == 0 ? " selected" : "", s == 1 ? " selected" : "");
}

static void gen_list_window(EdBuf *out, const int w) {
    ListWin *lw = &g_win[w];
    if (!g_keep_from) lw->from[0] = 0; // showWindow(n) without a from: gen(undefined)
    EdBuf body;
    edb_init(&body);
    if (w == LW_MAPS) {
        list_body(&body, w);
        edb_addf(out, "<div style='color:rgba(0,0,0,0.3)' id='mapList'>%s</div>", body.buf ? body.buf : "");
        edb_free(&body);
        return;
    }
    EdBuf e;
    edb_init(&e);
    if (w == LW_MODS) {
        edb_add(&e, "<div class=\"windowHeader\">\n\t\t\t\t<div>Mods</div>\n\t\t\t</div>");
    } else if (w == LW_SOUNDS) {
        edb_add(&e, "<div class=\"windowHeader\">\n\t\t\t\t<div style='display: block;'>Sounds <div class='publishAsset' onclick='KE.publishAssetPopup()'>Publish</div></div>\n\t\t\t</div>");
    } else {
        edb_addf(&e, "<div class=\"windowHeader\">\n\t\t\t\t<div style='display: block;'>%s\n\t\t\t\t\t<div class='publishAsset' onclick='KE.publishAssetPopup()'>Publish</div>",
                 w == LW_ASSETS ? "Assets" : "Textures");
        sort_select(&e, w);
        edb_add(&e, "\n\t\t\t\t</div>\n\t\t\t</div>");
    }
    tabs(&e, w);
    if (is_search_tab(w, lw->tab)) {
        const int js = k_lw[w].js;
        if (w == LW_ASSETS) {
            edb_add(&body, "<input type='text' id='asSrch' class='smlInput' placeholder='Asset Name' onkeydown='if(window.event.keyCode==13)windows[1].searchAssets()' />");
            edb_add(&body, "<select id='asCat'><option value='-1'>All</option>");
            for (int n = 0; n < ASSET_CAT_COUNT; n++) edb_addf(&body, "<option value='%d'>%s</option>", n, k_asset_cat[n]);
            edb_add(&body, "</select>");
        } else if (w == LW_MODS) {
            edb_add(&body, "<input type='text' id='mSrch' style='width: 847px;' class='smlInput' placeholder='Mod Name' onkeydown='if(window.event.keyCode==13)windows[4].searchMods()' />");
        } else {
            edb_addf(&body, "<input type='text' id='%s' class='smlInput' placeholder='%s Name' onkeydown='if(window.event.keyCode==13)windows[%d].searchAssets()' />",
                     k_lw[w].input, w == LW_TEXTURES ? "Texture" : "Sound", js);
        }
        edb_addf(&body, "<div class='searchBtn' onclick='windows[%d].%s()'>Search</div><div style='color:rgba(0,0,0,0.5);margin-top:10px' id='%s'></div>", js,
                 w == LW_MODS ? "searchMods" : "searchAssets", k_lw[w].results);
    } else {
        list_body(&body, w);
    }
    edb_addf(out, "<div style='margin-bottom:15px'>%s</div>\n\t\t\t<div style='color:rgba(0,0,0,0.3)%s' id='%s'>%s</div>", e.buf ? e.buf : "",
             w == LW_ASSETS ? ";text-align: center" : "", k_lw[w].list_id, body.buf ? body.buf : "");
    edb_free(&e);
    edb_free(&body);
}

int ed_net_window_gen(const int index, EdBuf *out) {
    const int w = lw_of_window(index);
    if (w < 0) return 0;
    gen_list_window(out, w);
    return 1;
}

// ---------------------------------------------------------------------------
// opening (viewAssets / viewModels / viewTextures / viewSounds / myAssets / advancedOptions.select*)

void ed_net_view_assets(const int type) {
    // 0: createObjects[CUSTOM_ASSET] = viewAssets([0, 3], "createObjects"); 1: viewModels([..]) for a
    // Select Asset target of the config panels (editor_gameconfig.c applies the id)
    g_win[LW_ASSETS].only_show = type ? 1 << 0 : 1 << 0 | 1 << 3;
    open_list(LW_ASSETS, type ? "" : "createObjects");
}

void ed_net_my_assets(void) {
    // KE.myAssets: windows[1].tabIndex = 1; showWindow(2, false, "createObjects")
    g_win[LW_ASSETS].tab = ASSET_TAB_MINE;
    open_list(LW_ASSETS, "createObjects");
}

// the ed_net_select codes of the inspector / environment panels (editor_inspector.c, editor_env.c)
static const struct { int code, lw; const char *from; } k_select[] = {
    {0, LW_ASSETS, "selectModel"}, {1, LW_TEXTURES, "selectTexture"}, {2, LW_SOUNDS, "selectAudio"},
    {3, LW_SOUNDS, "selectDeath"}, {4, LW_SOUNDS, "selectIdle"}, {5, LW_SOUNDS, "selectShoot"}, {6, LW_SOUNDS, "selectAgro"},
    {8, LW_SOUNDS, "selectShootHit"}, {9, LW_SOUNDS, "selectMelee"}, {10, LW_SOUNDS, "selectMeleeHit"},
    {11, LW_ASSETS, "selectProjModel"}, {12, LW_TEXTURES, "selectCloudEN"}, {13, LW_TEXTURES, "selectEmissiveCloudEN"},
    {14, LW_SOUNDS, "selectAmbEN"}, {20, LW_TEXTURES, "selectEmissiveCloud"}, {21, LW_TEXTURES, "selectCloud"}, {22, LW_SOUNDS, "selectAmb"},
};

void ed_net_select(EdCtl *c) {
    const int a = c ? c->arg : 0;
    if (a == 7) { // advancedOptions.selectMod: showWindow(5)
        editor_show_window(5);
        return;
    }
    for (size_t i = 0; i < sizeof(k_select) / sizeof(k_select[0]); i++) {
        if (k_select[i].code != a) continue;
        if (k_select[i].lw == LW_ASSETS) g_win[LW_ASSETS].only_show = 1 << 0; // viewModels: onlyShow = [0]
        open_list(k_select[i].lw, k_select[i].from);
        return;
    }
}

// ---------------------------------------------------------------------------
// selection (KE.selectAsset / KE.selectMod)

static void set_selected(const char *prop, cJSON *v, const int regen) {
    EditorObject *o = g_ed.proj->selected;
    if (o) {
        ed_prop_set(o, prop, v);
        if (regen) ed_obj_rebuild(o);
        g_ed.dirty_panel = 1; // updateObjConfigGUI
        ed_mark_changed();
    }
    cJSON_Delete(v);
}

static void set_map_int(const char *key, const char *id) {
    // mapConfig[key] = parseInt(id) || 0; panel.state.refresh("panel_right_environment"); regenSky
    ed_json_set_num(g_ed.proj->map_config, key, atoi(id));
    editor_apply_lighting();
    ed_panel_refresh_all();
    ed_mark_changed();
}

static vec3 create_position(void) {
    return editor_setting_bool("createObjectNear") ? editor_near_position() : (vec3) {0};
}

static void add_new_object(const char *key, const char *id, const int prefab) {
    // addObject(new ObjectInstance({i, <key>: id, p}, true))
    const vec3 p = create_position();
    cJSON *d = cJSON_CreateObject();
    if (prefab >= 0) cJSON_AddNumberToObject(d, "i", prefab);
    cJSON_AddStringToObject(d, key, id);
    cJSON *pa = cJSON_AddArrayToObject(d, "p");
    cJSON_AddItemToArray(pa, cJSON_CreateNumber(p.x));
    cJSON_AddItemToArray(pa, cJSON_CreateNumber(p.y));
    cJSON_AddItemToArray(pa, cJSON_CreateNumber(p.z));
    EditorObject *o = ed_obj_new(d, 0);
    cJSON_Delete(d);
    if (o) ed_obj_add(o, 0, 1);
}

static void create_prefab_asset(const char *id) {
    // fetch(user-assets/<id>/data.json): an array of objects (or {objects}); a map is refused
    char rel[160], path[1024];
    snprintf(rel, sizeof(rel), "user-assets/%d/data.json", atoi(id));
    snprintf(path, sizeof(path), "%s%s", assets_path(), rel);
    char *text = NULL;
    if (ensure_asset_file(rel)) {
        FILE *f = fopen(path, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            const long n = ftell(f);
            fseek(f, 0, SEEK_SET);
            text = n >= 0 ? malloc((size_t) n + 1) : NULL;
            if (text) text[fread(text, 1, (size_t) n, f)] = 0;
            fclose(f);
        }
    }
    cJSON *data = text ? cJSON_Parse(text) : NULL;
    free(text);
    if (!data) {
        editor_message("Error loading Prefab");
        return;
    }
    if (!cJSON_IsArray(data) && cJSON_GetObjectItemCaseSensitive(data, "map")) {
        cJSON_Delete(data);
        editor_message("Invalid Prefab");
        return;
    }
    const cJSON *objects = cJSON_GetObjectItemCaseSensitive(data, "objects");
    char *json = cJSON_PrintUnformatted(objects ? objects : data);
    cJSON_Delete(data);
    if (json) ed_obj_replace_at(create_position(), json, editor_setting_bool("assetAutoGroup"));
    free(json);
}

static void select_asset(const char *id, const int type, const char *from_arg) {
    // the Select Asset / Icon / Image / Custom targets of the config, GUI and trigger panels
    if (ed_asset_target_apply(id)) return;
    char from[96];
    snprintf(from, sizeof(from), "%s", from_arg ? from_arg : "");
    char *us = strchr(from, '_'); // "selectX_<index>": the index part is the asset targets' (above)
    if (us) *us = 0;
    editor_close_window();
    static const struct { const char *from, *prop; int number, regen; } k_props[] = {
        {"selectTexture", "assetTexture", 1, 1}, {"selectModel", "assetID", 0, 1}, {"selectCloudEN", "skyDomeTexA", 1, 0},
        {"selectEmissiveCloudEN", "skyDomeEmisTex", 1, 0}, {"selectAmbEN", "ambIndC", 1, 0}, {"selectAgro", "agroSound", 1, 0},
        {"selectShoot", "shootSound", 1, 0}, {"selectShootHit", "shootHitSound", 1, 0}, {"selectMelee", "melSound", 1, 0},
        {"selectMeleeHit", "melHitSound", 1, 0}, {"selectDeath", "deathSound", 1, 0}, {"selectIdle", "idleSound", 1, 0},
        {"selectProjModel", "projA", 0, 0}, {"selectAudio", "assetSound", 1, 0},
    };
    for (size_t i = 0; i < sizeof(k_props) / sizeof(k_props[0]); i++) {
        if (strcmp(from, k_props[i].from)) continue;
        set_selected(k_props[i].prop, k_props[i].number ? cJSON_CreateNumber(atoi(id)) : cJSON_CreateString(id), k_props[i].regen);
        return;
    }
    if (!strcmp(from, "selectCloud")) set_map_int("skyDomeTexA", id);
    else if (!strcmp(from, "selectEmissiveCloud")) set_map_int("skyDomeEmisTex", id);
    else if (!strcmp(from, "selectAmb")) set_map_int("ambIndC", id);
    else if (!strcmp(from, "createObjects")) {
        if (type == 3) create_prefab_asset(id);
        else if (type == 4) add_new_object("at", id, -1); // a cube with the asset texture
        else if (!type) add_new_object("aid", id, ED_PREFAB_CUSTOM_ASSET);
    } else {
        editor_message("Unknown Asset"); // alert("Unknown Asset")
    }
}

static void select_mod(const char *url) {
    editor_close_window();
    ed_json_set_str(g_ed.proj->map_config, "modURL", url ? url : "");
    ed_mark_changed();
    ed_panel_left_show();
}

// ---------------------------------------------------------------------------
// publishing a game (publishMap / preloadMap / publishGamePopup / uploadGame / mapResp)

static char *g_thumb;          // data:image/png;base64,... (the view or a picked file), NULL none
static int g_capture_pending;  // read the view back after the next frame's overlays
static int g_publish_sent;     // an uploadM is waiting for pubMR / upMp
static int g_pub_state;        // 1: waiting for the view capture and the My Games lookup
static int g_pub_lookup;       // the My Games lookup is still running
static cJSON *g_pub_row;       // the published map of that name (Update Game), NULL = new
static char *g_pub_map;        // localStorage "mapToPub": getMapExport() of publishMap
static char g_pub_name[64];    // the map name of the last uploadM (mapResp share link)
static unsigned g_pub_gen;

static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char *data_url(const char *mime, const unsigned char *data, const size_t len) {
    char prefix[64];
    snprintf(prefix, sizeof(prefix), "data:%s;base64,", mime);
    const size_t pl = strlen(prefix), out_len = pl + (len + 2) / 3 * 4;
    char *s = malloc(out_len + 1);
    if (!s) return NULL;
    memcpy(s, prefix, pl);
    char *o = s + pl;
    for (size_t i = 0; i < len; i += 3) {
        const unsigned v = (unsigned) data[i] << 16 | (i + 1 < len ? (unsigned) data[i + 1] << 8 : 0) | (i + 2 < len ? data[i + 2] : 0);
        *o++ = b64[v >> 18 & 63];
        *o++ = b64[v >> 12 & 63];
        *o++ = i + 1 < len ? b64[v >> 6 & 63] : '=';
        *o++ = i + 2 < len ? b64[v & 63] : '=';
    }
    *o = 0;
    return s;
}

// UTILS.thumbnailSize(dataUrl) in kB: unpadded base64 length * 0.75 / 1000
static double thumb_kb(const char *url) {
    const char *comma = strchr(url, ',');
    size_t n = 0;
    for (const char *p = comma ? comma + 1 : url; *p; p++) if (*p != '=') n++;
    return (double) (long) ((double) n * 0.75) / 1000.0;
}

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

// the 3D view as a PNG data URL within `max_kb` (and kn_brain's data URL limit); NULL when it cannot be made
static char *capture_view(const double max_kb) {
    const int w = g_ed.view_w, h = g_ed.view_h;
    if (w <= 0 || h <= 0) return NULL;
    unsigned char *raw = malloc((size_t) w * (size_t) h * 4);
    if (!raw) return NULL;
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(g_ed.view_x, g_ed.view_y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, raw);
    unsigned char *row = malloc((size_t) w * 4);
    for (int y = 0; row && y < h / 2; y++) { // GL rows are bottom-up
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
        char *u = data_url("image/png", png, (size_t) len);
        free(png);
        if (u && thumb_kb(u) <= max_kb && strlen(u) <= BRAIN_THUMB_MAX) url = u;
        else free(u);
    }
    free(raw);
    return url;
}

static int thumb_limit_kb(void) {
    const AccountInfo *a = account();
    return a && a->premium ? ASSET_THUMB_P : ASSET_THUMB;
}

static void thumb_size_text(EdBuf *h, const double kb, const int limit) {
    js_number(h, round(kb * 10000) / 10000);
    edb_addf(h, " / %d <span style='color:rgba(255, 255, 255, 0.4)!important;'>KB</span>", limit);
}

// publishGamePopup(isNew, row)
static void publish_game_popup(const int is_new, const cJSON *row) {
    const int limit = thumb_limit_kb();
    EdBuf h;
    edb_init(&h);
    edb_addf(&h, "<div><div class='pubHed'>%s Game</div><div id='publishGame' style='display:block'>", is_new ? "Publish" : "Update");
    edb_add(&h, "<div class='publishLeft' id='gameThumbnailH'><img id='gameThumbnailPrev' src='");
    const char *image = row_str(row, "map_image");
    edb_add(&h, g_thumb ? g_thumb : !strncmp(image, "data:image/", 11) ? image : "./img/noimg.png");
    edb_add(&h, "' onerror='this.onerror=null;this.src=\"./img/noimg.png\";' style='width: 100%;height: 100%;'><div id='gameThumbnailSize'>");
    thumb_size_text(&h, g_thumb ? thumb_kb(g_thumb) : 0, limit);
    edb_addf(&h, "</div><input onclick='selectFile(&quot;gameThumbnail&quot;)' type='button' id='gameThumbnailSelect' value='Select'><div class='changeThumb'>Select Thumbnail</div>"
                 "<input id='gameThumbnail' type='file' accept='image/png' style='width:1px;visibility:hidden;' onchange='updateFile(this.id,0,%d)'></div>", limit);
    edb_add(&h, "<div class='publishRight'><div style='color:rgba(255, 255, 255, 0.6)!important;margin-top: 4px;margin-bottom: 4px;'>Category<select id='gameCategory2' class='mapInput2'>");
    const int ca = (int) ed_json_num(row, "ca", ed_json_num(row, "map_category", 0));
    for (int i = 0; i < MAP_CAT_COUNT; i++) edb_addf(&h, "<option value='%d'%s>%s</option>", i, !is_new && i == ca ? " selected" : "", k_map_cats[i]);
    edb_add(&h, "</select></div>");
    edb_add(&h, "<div style='color:rgba(255, 255, 255, 0.6)!important;margin-top: 4px;margin-bottom: 4px;'>Post to Hub <label class='switch2'><input id='gamePostNew' type='checkbox'><span class='slider2'></span></label></div>");
    edb_addf(&h, "<div style='color:rgba(255, 255, 255, 0.6)!important;margin-top: 4px;margin-bottom: 4px;'>Mobile Friendly <label class='switch2'><input id='gameMobNew' type='checkbox'%s><span class='slider2'></span></label></div>",
             !is_new && ed_json_truthy(cJSON_GetObjectItemCaseSensitive(row, "mob")) ? " checked" : "");
    edb_add(&h, "</div>");
    // the map data is set as the field's value below (the attribute would need quoting)
    edb_add(&h, "<input id='gameDataNew' type='text' class='mapInput' placeholder='Paste Data' autocomplete='off' autocorrect='off' autocapitalize='off' spellcheck='false'>");
    edb_add(&h, "<input id='gameVideoNew' type='text' placeholder='Youtube Link (Optional)' class='mapInput' ");
    const char *vl = row_str(row, "vl");
    if (!is_new && vl[0]) {
        edb_add(&h, " value='https://youtube.com/watch?v=");
        cleanse(&h, vl);
        edb_add(&h, "'");
    }
    edb_add(&h, " autocomplete='off' autocorrect='off' autocapitalize='off' spellcheck='false'><textarea id='gameDescNew' type='text' maxlength='500' placeholder='Description (Optional)' class='mapInput'  autocomplete='off' autocorrect='off' autocapitalize='off' spellcheck='false'>");
    if (!is_new) edb_add(&h, row_str(row, "map_description"));
    edb_add(&h, "</textarea></div>");
    const double version = fmax(1, ed_json_num(row, "map_updatecounter", 0) + 1) / 10;
    edb_add(&h, "<div id='gameResHolder' style='margin-top:8px;margin-bottom:5px;display:none'><div id='gameUpResp' style='color:rgba(0,0,0,0.3);display: inline-block;'></div></div>"
                "<div id='pubGame' style='margin-top:10px;width: calc(100% - 11px);' onclick='KE.uploadGame()'>");
    if (is_new) edb_add(&h, "Publish");
    else {
        edb_add(&h, "Publish v");
        js_number(&h, version);
    }
    edb_add(&h, "</div>");
    ed_generic_popup(h.buf, "publishGamePop");
    edb_free(&h);
    // getSavedVal("mapToPub"); deleteVal("mapToPub")
    if (g_pub_map) {
        const HtmlElem data = html_dom_by_id("gameDataNew");
        if (data) html_dom_set_value(data, g_pub_map);
        free(g_pub_map);
        g_pub_map = NULL;
    }
}

static void preload_ready(void) {
    if (g_pub_state != 1 || g_capture_pending || g_pub_lookup) return;
    g_pub_state = 0;
    ed_clear_pops();
    publish_game_popup(g_pub_row == NULL, g_pub_row);
    cJSON_Delete(g_pub_row);
    g_pub_row = NULL;
}

typedef struct {
    unsigned gen;
    char name[64];
} PubLookup;

static void on_pub_lookup(void *user, const int status, const char *body, const size_t length) {
    PubLookup *l = user;
    const int stale = l->gen != g_pub_gen;
    if (!stale && g_ed.active) {
        cJSON *res = status == 200 && body ? cJSON_ParseWithLength(body, length) : NULL;
        const cJSON *row;
        cJSON_ArrayForEach(row, cJSON_GetObjectItemCaseSensitive(res, "data")) {
            const char *n = row_str(row, "map_name");
            size_t i = 0;
            while (n[i] && l->name[i] && (n[i] | 32) == (l->name[i] | 32)) i++;
            if (!n[i] && !l->name[i]) {
                cJSON_Delete(g_pub_row);
                g_pub_row = cJSON_Duplicate(row, 1);
                break;
            }
        }
        cJSON_Delete(res);
        g_pub_lookup = 0;
        preload_ready();
    }
    free(l);
}

void ed_net_publish(void) {
    // publishMap()
    if (!backend_enabled() || backend_available() == 0) {
        editor_message("Publishing needs the Krunker Box Office backend (kn_brain); use Export instead.");
        return;
    }
    const AccountInfo *a = account();
    if (!a) {
        io_po(NULL, "Must be logged in", NULL);
        return;
    }
    // v10 publishMap: a named map is checked first (validateMap(JSON.parse(getMapExport()), cb,
    // false)) and its first error is the failure popup; nothing is uploaded. An unnamed map opens
    // the publish form unchecked, as in v10 (its name comes later; uploadM checks it then).
    char *exported = ed_map_export_string();
    if (ed_json_str(g_ed.proj->map_config, "name", "")[0]) {
        cJSON *doc = exported ? cJSON_Parse(exported) : NULL;
        const char *error = doc ? map_validate_error(doc, 0) : "Map error occured.";
        if (error) {
            char copy[256];
            snprintf(copy, sizeof(copy), "%s", error); // map_validate_error's storage is static
            cJSON_Delete(doc);
            free(exported);
            io_po(NULL, copy, NULL);
            return;
        }
        cJSON_Delete(doc);
    }
    if (!g_listening) g_listening = backend_social_listen(on_social, NULL);
    free(g_pub_map);
    g_pub_map = exported; // saveVal("mapToPub", getMapExport())
    cJSON_Delete(g_pub_row);
    g_pub_row = NULL;
    g_pub_gen++;
    g_pub_state = 1;
    g_capture_pending = 1; // the view is read back after the next frame (ed_net_frame)
    g_pub_lookup = 0;
    const char *name = ed_json_str(g_ed.proj->map_config, "name", "");
    if (name[0]) {
        wait_popup("Please Wait...");
        // io.send("plm", name) -> preloadMap: the account's My Games list instead (see the notes)
        PubLookup *l = calloc(1, sizeof(*l));
        char path[256] = "/api/maps";
        q_int(path, sizeof(path), "index", MAP_API_OWNED);
        q_int(path, sizeof(path), "pos", 0);
        q_int(path, sizeof(path), "accountId", a->id);
        q_add(path, sizeof(path), "val", name);
        if (l) {
            l->gen = g_pub_gen;
            snprintf(l->name, sizeof(l->name), "%s", name);
            g_pub_lookup = backend_http_get(path, on_pub_lookup, l);
            if (!g_pub_lookup) free(l);
        }
    }
}

// KE.uploadGame()
static void upload_game(void) {
    upload_resp("Loading...");
    char cat[16], video[256], desc[600];
    read_value("gameCategory2", cat, sizeof(cat));
    const int ci = atoi(cat);
    if (!cat[0] || ci < 0 || ci >= MAP_CAT_COUNT) {
        upload_resp("Invalid Category");
        return;
    }
    char *data = malloc(MAP_DATA_MAX + 2);
    if (!data) return;
    read_value("gameDataNew", data, MAP_DATA_MAX + 2);
    if (strlen(data) > MAP_DATA_MAX) {
        free(data);
        upload_resp("Failed. 500k Character Game Data Limit");
        return;
    }
    read_value("gameDescNew", desc, sizeof(desc));
    read_value("gameVideoNew", video, sizeof(video));
    const int post = read_checked("gamePostNew"), mob = read_checked("gameMobNew");
    if (!account()) {
        free(data);
        upload_resp("Must be logged in");
        return;
    }
    cJSON *map = cJSON_Parse(data);
    snprintf(g_pub_name, sizeof(g_pub_name), "%s", ed_json_str(map, "name", ""));
    cJSON_Delete(map);
    cJSON *f = frame_new("uploadM");
    cJSON_AddItemToArray(f, cJSON_CreateString(data));
    cJSON_AddItemToArray(f, g_thumb ? cJSON_CreateString(g_thumb) : cJSON_CreateNull());
    cJSON_AddItemToArray(f, cJSON_CreateString(desc));
    cJSON_AddItemToArray(f, cJSON_CreateString(video));
    cJSON_AddItemToArray(f, cJSON_CreateString(cat));
    cJSON_AddItemToArray(f, cJSON_CreateBool(post));
    cJSON_AddItemToArray(f, cJSON_CreateBool(mob));
    free(data);
    g_publish_sent = social_send(f, "pubMR", 1);
    if (!g_publish_sent) upload_resp("Upload Request Failed");
}

// KE.mapResp(text, name)
static void map_resp(const char *text, const char *name) {
    EdBuf h, n;
    edb_init(&h);
    edb_init(&n);
    escape_str(&n, name);
    edb_addf(&h, "<div style='width:100%%;text-align:center'><div><i class='material-icons' style='font-size:80px;color:#3be354'>done</i></div>"
                 "<div style='font-size:20px;color:rgba(255,255,255,0.5);margin-bottom: 20px;'>%s</div>"
                 "<div class='eButton' style='padding-top: 7px;' onclick='shareMapLink(this, &quot;%s&quot;)'>Share the game here</div></div>",
             text, n.buf ? n.buf : "");
    ed_generic_popup(h.buf, "mapRespPop");
    edb_free(&h);
    edb_free(&n);
}

static void share_map_link(const HtmlElem button, const char *name) {
    char origin[256], url[512];
    html_page_origin(origin, sizeof(origin));
    snprintf(url, sizeof(url), "%s/?play=%s", origin, name ? name : "");
    html_clipboard_write(url);
    if (button) html_dom_set_text(button, "Copied"); // menu.btn.invite.clicked (v7 restores the text after 1.25 s)
}

// ---------------------------------------------------------------------------
// publishing an asset (publishAssetPopup / updateAssetType / selectFile / updateFile / uploadAsset)

enum { AF_THUMB, AF_SOUND, AF_VIDEO, AF_TEXTURE, AF_DIFFUSE, AF_EMISSIVE, AF_NORMAL, AF_SPECULAR, AF_ALPHA, AF_DISPLACEMENT,
       AF_ROUGHNESS, AF_METALNESS, AF_BUMP, AF_LIGHT, AF_MODEL, AF_ICON, AF_THUMBNAIL, AF_COUNT };

// the popup's file fields: element id, FormData key, accept, limit (MB, or kB when kb), shown for asset type
static const struct { const char *id, *key, *accept, *label; int limit, kb; } k_files[AF_COUNT] = {
    {"gameThumb", "thumb", "image/png", "Thumbnail (.png):", 0, 1},
    {"gameSound", "sound", "audio/mpeg", "Sound (.mp3):", ASSET_SOUND, 0},
    {"gameVideo", "video", "video/mp4", "Video (.mp4):", ASSET_VIDEO, 0},
    {"gameTexture", "texture", "image/png", "Texture / Image (.png):", ASSET_TEXTURE, 0},
    {"gameDiffuse", "diffuse", "image/png", "Diffuse Texture (.png):", ASSET_TEXTURE, 0},
    {"gameEmissive", "emissive", "image/png", "Emissive Texture (.png):", ASSET_TEXTURE, 0},
    {"gameNormal", "normal", "image/png", "Normal Texture (.png):", ASSET_TEXTURE, 0},
    {"gameSpecular", "specular", "image/png", "Specular Texture (.png):", ASSET_TEXTURE, 0},
    {"gameAlpha", "alpha", "image/png", "Alpha Texture (.png):", ASSET_TEXTURE, 0},
    {"gameDisplacement", "displacement", "image/png", "Displacement Texture (.png):", ASSET_TEXTURE, 0},
    {"gameRoughness", "roughness", "image/png", "Roughness Texture (.png):", ASSET_TEXTURE, 0},
    {"gameMetalness", "metalness", "image/png", "Metalness Texture (.png):", ASSET_TEXTURE, 0},
    {"gameBump", "bump", "image/png", "Bump Texture (.png):", ASSET_TEXTURE, 0},
    {"gameLight", "light", "image/png", "Light Texture (.png):", ASSET_TEXTURE, 0},
    {"gameModel", "model", ".obj,.gltf,.glb,.fbx", "Model (.obj/.gltf/.glb/.fbx):", -1, 0},
    {"gameIcon", "icon", "image/png", "Icon (.png):", ASSET_ICON, 1},
    {"gameThumbnail", "thumb", "image/png", NULL, 0, 1}, // the publish game popup's
};

static struct {
    double size;     // bytes, 0 = none
    char name[128];  // file name (the model's extension)
} g_files[AF_COUNT];
static int g_asset_type; // vLN01461: 0 model, 1 prefab, 2 texture, 3 sound, 4 video

static void files_reset(void) { memset(g_files, 0, sizeof(g_files)); }

static void file_field(EdBuf *h, const int f, const int hidden) {
    int limit = k_files[f].limit;
    if (f == AF_THUMB) limit = thumb_limit_kb();
    edb_addf(h, "<div style='color:rgba(255, 255, 255, 0.6)!important;font-size:18px;%smargin-bottom: 5px;' id='%sH'>"
                "<div style='color:rgba(255, 255, 255, 0.6)!important;font-size:16px%s'>%s<input onclick='selectFile(&quot;%s&quot;)' type='button' id='%sSelect' class='fileSelect' value='Select'>"
                "<input id='%s' name='%s' type='file' accept='%s' style='width:1px;visibility:hidden;' onchange='updateFile(this.id",
             hidden ? "display:none;" : "", k_files[f].id, f == AF_EMISSIVE ? ";" : "", k_files[f].label, k_files[f].id, k_files[f].id,
             k_files[f].id, f == AF_THUMB ? "thumbnail" : f == AF_SOUND ? "audio" : k_files[f].key, k_files[f].accept);
    if (limit < 0) edb_addf(h, ")'><span style='color:rgba(255, 255, 255, 0.6)!important;font-size: 14px;float:right;margin-top: 5px;' id='%sSize'></span></div></div>", k_files[f].id);
    else
        edb_addf(h, ",%s%d)'><span style='color:rgba(255, 255, 255, 0.6)!important;font-size: 14px;float:right;margin-top: 5px;' id='%sSize'>0 / %d "
                    "<span style='color:rgba(255, 255, 255, 0.4)!important;'>%s</span></span></div></div>",
                 k_files[f].kb ? "0," : " 1, ", limit, k_files[f].id, limit, k_files[f].kb ? "KB" : "MB");
}

// the texture slots of v10's per-map UV channel options (publishAssetPopup, E10:20417-20456), in
// its order: the popup's row names (the element ids are game<slot>Chan[H]) and the uploadA keys
#define UV_SLOTS 10
static const char *const k_uv_slot[UV_SLOTS] = {"Diffuse", "Emissive", "Normal", "Specular", "Alpha", "Displacement", "Roughness", "Metalness", "Bump", "Light"};
static const char *const k_uv_key[UV_SLOTS] = {"diffuse", "emissive", "normal", "specular", "alpha", "displacement", "roughness", "metalness", "bump", "light"};

// the "Advanced: per-map UV channel" checkbox and one number row (channel 0..7) per texture slot;
// the rows show while the checkbox is on, and uploadAsset sends the non-zero channels (uv_channels)
static void uv_channel_rows(EdBuf *h) {
    edb_add(h, "<div style='color:rgba(255, 255, 255, 0.6)!important;font-size:18px;margin-bottom: 5px;' id='gameAdvancedTexH'>"
               "<div style='color:rgba(255, 255, 255, 0.6)!important;font-size:16px'>Advanced: per-map UV channel"
               "<input type='checkbox' id='gameAdvancedTex' style='float:right;margin-top:8px;' onchange='KE.toggleAdvancedTexOptions()'></div></div>");
    for (int i = 0; i < UV_SLOTS; i++)
        edb_addf(h, "<div class='gameTexChanRow' id='game%sChanH' style='display:none;color:rgba(255, 255, 255, 0.6)!important;font-size:14px;margin:2px 0 6px 20px;'>"
                    "%s UV Channel<input type='number' id='game%sChan' value='0' min='0' max='7' style='width:48px;float:right;'></div>",
                 k_uv_slot[i], k_uv_slot[i], k_uv_slot[i]);
}

// KE.toggleAdvancedTexOptions(): the channel rows follow the checkbox
static void toggle_advanced_tex(void) {
    const int on = read_checked("gameAdvancedTex");
    for (int i = 0; i < UV_SLOTS; i++) {
        char id[48];
        snprintf(id, sizeof(id), "game%sChanH", k_uv_slot[i]);
        html_dom_set_display(html_dom_by_id(id), on ? "block" : "none");
    }
}

// uploadAsset's uvc: {slot: channel} for the channels 1..7 (parseInt, v10 E10:20479-20490), null when none
static cJSON *uv_channels(void) {
    if (!read_checked("gameAdvancedTex")) return NULL;
    cJSON *uvc = NULL;
    for (int i = 0; i < UV_SLOTS; i++) {
        char id[48], v[16];
        snprintf(id, sizeof(id), "game%sChan", k_uv_slot[i]);
        read_value(id, v, sizeof(v));
        const int ch = atoi(v);
        if (ch <= 0 || ch > 7) continue;
        if (!uvc) uvc = cJSON_CreateObject();
        cJSON_AddNumberToObject(uvc, k_uv_key[i], ch);
    }
    return uvc;
}

void ed_net_publish_asset(void) {
    // publishAssetPopup()
    g_asset_type = 0;
    files_reset();
    const AccountInfo *a = account();
    if (!a) {
        io_po(NULL, "Must be logged in", NULL);
        return;
    }
    if (!g_listening) g_listening = backend_social_listen(on_social, NULL);
    EdBuf h;
    edb_init(&h);
    edb_add(&h, "<div><div class='pubHed'>Publish or Update Asset</div><div id='publishAsset' style='display:block'>");
    edb_add(&h, "<div style='color:rgba(255, 255, 255, 0.6)!important;margin-top: 4px;margin-bottom: 4px;'>Type<select id='gameType' class='mapInput2' onchange='KE.updateAssetType(this.value)'>"
                "<option value='0'>Model</option><option value='1'>Object Prefab</option><option value='2'>Texture / Image</option><option value='3'>Sound</option><option value='4'>Video</option></select></div>");
    edb_add(&h, "<div id='gameCategoryH' style='color:rgba(255, 255, 255, 0.6)!important;margin-top: 4px;margin-bottom: 4px;'>Category<select id='gameCategory' class='mapInput2'>");
    for (int i = 0; i < ASSET_CAT_COUNT; i++) if (i != 16 && i != 17) edb_addf(&h, "<option value='%d'>%s</option>", i, k_asset_cat[i]);
    edb_add(&h, "</select></div>");
    edb_add(&h, "<div style='color:rgba(255, 255, 255, 0.6)!important;margin-top:4px;margin-bottom:10px;border-bottom:2px solid;padding-bottom:10px;'>Private <label class='switch2'><input id='gamePrivNew' type='checkbox'><span class='slider2'></span></label></div>");
    for (int f = AF_THUMB; f <= AF_ICON; f++) file_field(&h, f, f == AF_SOUND || f == AF_VIDEO || f == AF_TEXTURE);
    uv_channel_rows(&h);
    edb_addf(&h, "<div style='color:rgba(255, 255, 255, 0.6)!important;font-size:14px;margin-top:12px;border-top:2px solid;padding-top:10px;' id='maxSizeH'>Max Size"
                 "<span style='color:rgba(255, 255, 255, 0.6)!important;font-size:14px;float:right;' id='gameSize'>0 / %d <span style='color:rgba(255, 255, 255, 0.4)!important;'>mb</span></span></div>",
             a->premium ? ASSET_SIZE_P : ASSET_SIZE);
    edb_add(&h, "<input id='gameNameNew' type='text' maxlength='500' placeholder='Name' class='mapInput'  autocomplete='off' autocorrect='off' autocapitalize='off' spellcheck='false'>");
    edb_add(&h, "<input id='gameDataNew2' style='display:none' type='text' placeholder='Paste Data' class='mapInput'  autocomplete='off' autocorrect='off' autocapitalize='off' spellcheck='false'></div>");
    edb_add(&h, "<div id='gameResHolder' style='margin-top:8px;margin-bottom:5px;display:none'><div id='gameUpResp' style='color:rgba(0,0,0,0.3);display: inline-block;'></div></div>"
                "<div id='pubGame' style='margin-top:10px' onclick='KE.uploadAsset()'>Publish/Update</div>");
    ed_generic_popup(h.buf, "publishPop");
    edb_free(&h);
}

// KE.updateAssetType(type)
static void update_asset_type(const int t) {
    g_asset_type = t;
    static const struct { const char *id; int show_for; } shows[] = { // show_for: type, -1 model only
        {"gameTextureH", 2}, {"gameDiffuseH", 0}, {"gameEmissiveH", 0}, {"gameNormalH", 0}, {"gameSpecularH", 0}, {"gameAlphaH", 0},
        {"gameModelH", 0}, {"gameIconH", 0}, {"maxSizeH", 0}, {"gameDataNew2", 1}, {"gameSoundH", 3}, {"gameVideoH", 4},
        {"gameDisplacementH", 0}, {"gameRoughnessH", 0}, {"gameMetalnessH", 0}, {"gameBumpH", 0}, {"gameLightH", 0},
    };
    for (size_t i = 0; i < sizeof(shows) / sizeof(shows[0]); i++) html_dom_set_display(html_dom_by_id(shows[i].id), t == shows[i].show_for ? "block" : "none");
    html_dom_set_display(html_dom_by_id("gameCategoryH"), t > 1 ? "none" : "block");
}

static int file_index(const char *id) {
    for (int f = 0; f < AF_COUNT; f++) if (id && !strcmp(id, k_files[f].id)) return f;
    return -1;
}

// selectFile(id) -> a picked file -> updateFile(id, mb, limit)
static void file_picked(void *user, const char *path) {
    const int f = (int) (intptr_t) user;
    if (!path || !path[0] || f < 0 || f >= AF_COUNT) return;
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        upload_resp("Failed");
        return;
    }
    fseek(fp, 0, SEEK_END);
    const long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    unsigned char *data = size > 0 ? malloc((size_t) size) : NULL;
    const size_t got = data ? fread(data, 1, (size_t) size, fp) : 0;
    fclose(fp);
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    g_files[f].size = (double) got;
    snprintf(g_files[f].name, sizeof(g_files[f].name), "%s", base);
    char id[48];
    snprintf(id, sizeof(id), "%sH", k_files[f].id);
    html_dom_set_attr(html_dom_by_id(id), "title", base);
    snprintf(id, sizeof(id), "%sSelect", k_files[f].id);
    html_dom_set_value(html_dom_by_id(id), "Change");
    if (f == AF_THUMBNAIL && data) {
        // the publish game thumbnail: its preview and the data URL sent with uploadM
        static const unsigned char sig[8] = {137, 'P', 'N', 'G', 13, 10, 26, 10};
        if (got < 8 || memcmp(data, sig, 8)) {
            upload_resp("Invalid Thumbnail (PNG only)");
        } else {
            char *url = data_url("image/png", data, got);
            if (url && (thumb_kb(url) > thumb_limit_kb() || strlen(url) > BRAIN_THUMB_MAX)) {
                upload_resp(strlen(url) > BRAIN_THUMB_MAX ? "Thumbnail too large for this server (150 KB)" : "Thumbnail too large");
                free(url);
            } else if (url) {
                free(g_thumb);
                g_thumb = url;
                html_dom_set_attr(html_dom_by_id("gameThumbnailPrev"), "src", g_thumb);
            }
        }
    }
    free(data);
    const int mb = !k_files[f].kb;
    int limit = k_files[f].limit;
    if (f == AF_THUMB || f == AF_THUMBNAIL) limit = thumb_limit_kb();
    snprintf(id, sizeof(id), "%sSize", k_files[f].id);
    const HtmlElem s = html_dom_by_id(id);
    if (s) {
        EdBuf t;
        edb_init(&t);
        js_number(&t, round((double) got / (mb ? 1000000.0 : 1000.0) * 10000) / 10000);
        if (limit > 0) edb_addf(&t, " / %d", limit);
        edb_addf(&t, " <span style='color:rgba(255, 255, 255, 0.4)!important;'>%s</span>", mb ? "MB" : "KB");
        html_dom_set_html(s, t.buf);
        edb_free(&t);
    }
    if (g_asset_type == 0) {
        // #gameSize: the MB fields' sum
        double total = 0;
        for (int i = 0; i < AF_THUMBNAIL; i++) if (!k_files[i].kb) total += g_files[i].size / 1000000.0;
        const AccountInfo *a = account();
        EdBuf t;
        edb_init(&t);
        js_number(&t, round(total * 10000) / 10000);
        edb_addf(&t, " / %d <span style='color:rgba(255, 255, 255, 0.4)!important;'>mb</span>", a && a->premium ? ASSET_SIZE_P : ASSET_SIZE);
        const HtmlElem gs = html_dom_by_id("gameSize");
        if (gs) html_dom_set_html(gs, t.buf);
        edb_free(&t);
    }
}

static void add_sizes(cJSON *sizes, const int *fields, const int n) {
    for (int i = 0; i < n; i++)
        if (g_files[fields[i]].size > 0) cJSON_AddNumberToObject(sizes, k_files[fields[i]].key, g_files[fields[i]].size);
}

// KE.uploadAsset()
static void upload_asset(void) {
    upload_resp("Loading...");
    const AccountInfo *a = account();
    if (!a) {
        upload_resp("Must be logged in");
        return;
    }
    char name[512], cat[16];
    read_value("gameNameNew", name, sizeof(name));
    const int priv = read_checked("gamePrivNew");
    cJSON *sizes = cJSON_CreateObject();
    cJSON *f = frame_new("uploadA");
    cJSON_AddItemToArray(f, cJSON_CreateString(name));
    cJSON_AddItemToArray(f, cJSON_CreateString(""));
    if (g_asset_type < 2) {
        read_value("gameCategory", cat, sizeof(cat));
        const int ci = atoi(cat);
        if (!cat[0] || ci < 0 || ci >= ASSET_CAT_COUNT) {
            cJSON_Delete(sizes);
            cJSON_Delete(f);
            upload_resp("Invalid Category");
            return;
        }
        char *data = malloc(MAP_DATA_MAX + 2);
        if (data) read_value("gameDataNew2", data, MAP_DATA_MAX + 2);
        if (data && strlen(data) > MAP_DATA_MAX) {
            free(data);
            cJSON_Delete(sizes);
            cJSON_Delete(f);
            upload_resp("Failed. 500k Character Game Data Limit");
            return;
        }
        if (g_asset_type == 1) {
            cJSON_AddNumberToObject(sizes, "thumb", g_files[AF_THUMB].size);
            if (!(g_files[AF_THUMB].size > 0)) cJSON_DeleteItemFromObject(sizes, "thumb");
            cJSON_AddNumberToObject(sizes, "data", data ? (double) strlen(data) : 0);
        } else {
            static const int model[] = {AF_THUMB, AF_DIFFUSE, AF_EMISSIVE, AF_NORMAL, AF_SPECULAR, AF_ALPHA, AF_BUMP, AF_DISPLACEMENT,
                                        AF_ROUGHNESS, AF_LIGHT, AF_METALNESS, AF_MODEL, AF_ICON};
            add_sizes(sizes, model, (int) (sizeof(model) / sizeof(model[0])));
            double total = 0;
            const cJSON *it;
            cJSON_ArrayForEach(it, sizes) total += it->valuedouble;
            if (total / 1000000 > (a->premium ? ASSET_SIZE_P : ASSET_SIZE)) {
                free(data);
                cJSON_Delete(sizes);
                cJSON_Delete(f);
                upload_resp("File size Limit Reached");
                return;
            }
        }
        const char *dot = strrchr(g_files[AF_MODEL].name, '.');
        cJSON_AddItemToArray(f, cJSON_CreateNumber(ci));
        cJSON_AddItemToArray(f, cJSON_CreateBool(priv));
        cJSON_AddItemToArray(f, cJSON_CreateString(data ? data : ""));
        cJSON_AddItemToArray(f, sizes);
        cJSON_AddItemToArray(f, cJSON_CreateString(g_files[AF_MODEL].size > 0 && dot ? dot + 1 : "obj"));
        cJSON_AddItemToArray(f, cJSON_CreateNull()); // v10's 8th argument (undefined)
        cJSON *uvc = uv_channels();
        cJSON_AddItemToArray(f, uvc ? uvc : cJSON_CreateNull()); // uploadA's uvc: the per-map UV channels or null
        free(data);
    } else {
        // texture (2) / sound (3) / video (4): category 16 / 17 / 16, kind = type
        const int file = g_asset_type == 2 ? AF_TEXTURE : g_asset_type == 3 ? AF_SOUND : AF_VIDEO;
        const int fields[2] = {AF_THUMB, file};
        add_sizes(sizes, fields, 2);
        cJSON_AddItemToArray(f, cJSON_CreateNumber(g_asset_type == 3 ? 17 : 16));
        cJSON_AddItemToArray(f, cJSON_CreateBool(priv));
        cJSON_AddItemToArray(f, cJSON_CreateString(""));
        cJSON_AddItemToArray(f, sizes);
        cJSON_AddItemToArray(f, cJSON_CreateNull());
        cJSON_AddItemToArray(f, cJSON_CreateNumber(g_asset_type));
    }
    if (!social_send(f, "pubR", 1)) upload_resp("Upload Request Failed");
}

// ---------------------------------------------------------------------------
// My Games: openEdit / loadMap / updateMapLB / calcMapFee / chngMapFunds, deleteCreation(Conf)

static void open_edit(const char *name, const double id, const double fund) {
    EdBuf h, n;
    edb_init(&h);
    edb_init(&n);
    escape_str(&n, name);
    char sid[32];
    ed_model_number(id, sid, sizeof(sid));
    edb_addf(&h, "<div style='font-size:20px;color:#fff'>%s<div style='float:right' class='editLink' onclick='loadMap(&quot;%s&quot;)'>Load in Editor</div></div><div></div>"
                 "<div style='margin-top:25px;font-size:24px;color:#b2b2b2'>Leaderboard Display:</div>"
                 "<div style='font-size:18px;color:#fff'>Type:<select id='leaderType' class='inputGrey2' style='padding: 4px;margin-top: unset;width: unset;'><option value='0'>Score</option><option value='1'>Time</option></select></div>"
                 "<div style='margin-top: 5px;font-size:18px;color:#fff'>Custom Type:<input id='leaderTypeCust' class='inputGrey2' style='padding: 4px;'></div>"
                 "<div style='margin-top:5px;font-size:18px;color:#fff'>Order:<select id='leaderOrder' class='inputGrey2' style='padding: 4px;margin-top: unset;width: unset;'><option value='0'>ASC</option><option value='1'>DESC</option></select></div>"
                 "<div class='mapLeaderB' onclick='updateMapLB(%s)'>Update Leaderboard</div>"
                 "<div style='margin-top:25px;font-size:24px;color:#b2b2b2'>Map Funds:</div>"
                 "<div style='font-size:18px;color:#fff'>Funds <span style='float:right;color:rgba(255,255,255,0.5)'>",
             name, n.buf ? n.buf : "", sid);
    comma_num(&h, fund);
    edb_addf(&h, " <span style='color:#fff'>KR</span></span></div>"
                 "<div style='font-size:18px;color:#fff'>KR Fee <span  id='mapFeeVal' style='float:right;color:rgba(255,255,255,0.5)'>None</span></div>"
                 "<input type='number' oninput='calcMapFee(this)' id='mapFundsInp' placeholder='Enter KR Amount'></input>"
                 "<div class='mapFndB' onclick='chngMapFunds(%s, 1)' style='background-color:#fc5a4e'>Withdraw</div>"
                 "<div class='mapFndB' onclick='chngMapFunds(%s, 0)' style='margin-left:16px;background-color:#4ca9f5'>Deposit</div>",
             sid, sid);
    ed_generic_popup(h.buf, "editPop");
    edb_free(&h);
    edb_free(&n);
}

static char g_load_name[64];

static void load_map_confirmed(void *user, const char *value) {
    (void) user;
    if (!value || !account()) return; // grabMap: only with an account
    cJSON *f = frame_new("edMp");
    cJSON_AddItemToArray(f, cJSON_CreateString(g_load_name));
    if (!social_send(f, "edMp", 0)) io_po(NULL, "Upload Request Failed", NULL);
}

static void load_map(const char *name) {
    snprintf(g_load_name, sizeof(g_load_name), "%s", name ? name : "");
    ed_confirm("Are you sure you want to load this map? This will clear any loaded map data!", load_map_confirmed, NULL);
}

static void calc_map_fee(const HtmlElem input) {
    char v[64] = "";
    if (input) html_dom_get_value(input, v, sizeof(v));
    double kr = atof(v);
    if (kr < 0) kr = 0;
    if (kr && input) {
        char s[40];
        ed_model_number(kr, s, sizeof(s));
        html_dom_set_value(input, s);
    }
    const double fee = floor(kr * 0.1);
    const HtmlElem out = html_dom_by_id("mapFeeVal");
    if (!out) return;
    if (!fee) {
        html_dom_set_html(out, "None");
        return;
    }
    EdBuf h;
    edb_init(&h);
    edb_add(&h, "<span style='color:rgba(255,255,255,0.7)'>-");
    comma_num(&h, fee);
    edb_add(&h, " <span style='color:#fff'>KR</span></span>");
    html_dom_set_html(out, h.buf);
    edb_free(&h);
}

static void change_map_funds(const double id, const int withdraw) {
    char v[64];
    read_value("mapFundsInp", v, sizeof(v));
    const int kr = atoi(v);
    if (!kr) return;
    wait_popup("Please Wait...");
    cJSON *f = frame_new("mfnds");
    cJSON_AddItemToArray(f, cJSON_CreateNumber(id));
    cJSON_AddItemToArray(f, cJSON_CreateNumber(withdraw));
    cJSON_AddItemToArray(f, cJSON_CreateNumber(kr));
    if (!social_send(f, "mfr", 0)) io_po(NULL, "Upload Request Failed", NULL);
}

static void update_map_lb(const double id) {
    char type[16], cust[128], order[16];
    read_value("leaderType", type, sizeof(type));
    read_value("leaderTypeCust", cust, sizeof(cust));
    read_value("leaderOrder", order, sizeof(order));
    wait_popup("Please wait...");
    cJSON *f = frame_new("ulb");
    cJSON_AddItemToArray(f, cJSON_CreateNumber(id));
    cJSON_AddItemToArray(f, cJSON_CreateNumber(atoi(type)));
    cJSON_AddItemToArray(f, cJSON_CreateString(cust));
    cJSON_AddItemToArray(f, cJSON_CreateNumber(atoi(order)));
    if (!social_send(f, "upMp", 0)) io_po(NULL, "Upload Request Failed", NULL);
    else g_wait_until = HUGE_VAL; // v10 sets no timeout for ulb: the popup waits until it is closed
}

static void delete_creation_conf(const double id, const int kind) {
    if (!account()) {
        io_po(NULL, "Must be logged in", NULL);
        return;
    }
    char sid[32];
    ed_model_number(id, sid, sizeof(sid));
    char h[768];
    snprintf(h, sizeof(h), "<div style='color:#fff;font-size:19px;position:relative;margin-bottom:15px;'>Are you sure you want to delete this %s?</div>"
                           "<div id='confirmBtn' onclick='deleteCreation(%s, %d)'>YES</div><div id='declineBtn' onclick='clearPops()'>NO</div>",
             kind == 0 ? "Map" : kind == 1 ? "Mod" : "Asset", sid, kind);
    ed_generic_popup(h, "confSubmitPop");
}

static void delete_creation(const double id, const int kind) {
    wait_popup("Please Wait...");
    cJSON *f = frame_new("delC");
    cJSON_AddItemToArray(f, cJSON_CreateNumber(id));
    cJSON_AddItemToArray(f, cJSON_CreateNumber(kind));
    if (!social_send(f, "po", 0)) {
        io_po(NULL, "Upload Request Failed", NULL);
        return;
    }
    // the cached lists still hold it: the next open asks the brain again (KrunkNative)
    const int w = kind == 0 ? LW_MAPS : kind == 1 ? LW_MODS : -1;
    for (int l = 0; l < LW_COUNT; l++) {
        if (w >= 0 && l != w) continue;
        for (int t = 0; t < LW_TABS; t++) list_clear(&g_win[l].list[t]);
        g_win[l].gen++;
    }
}

// KE.editMap(json)
static void edit_map(const cJSON *v) {
    if (!cJSON_IsString(v)) {
        io_po(NULL, "Map not found", NULL);
        return;
    }
    cJSON *map = cJSON_Parse(v->valuestring);
    char *text = NULL;
    if (map) {
        const cJSON *o;
        cJSON_ArrayForEach(o, cJSON_GetObjectItemCaseSensitive(map, "objects")) {
            if (cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(o, "col"))) cJSON_DeleteItemFromObjectCaseSensitive((cJSON *) o, "col");
            if (cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(o, "l"))) cJSON_DeleteItemFromObjectCaseSensitive((cJSON *) o, "l");
        }
        text = cJSON_PrintUnformatted(map);
        cJSON_Delete(map);
    }
    ed_map_import(text ? text : v->valuestring);
    free(text);
    editor_close_window();
    ed_clear_pops();
}

// ---------------------------------------------------------------------------
// the social socket's replies

static void on_social(void *user, const cJSON *frame) {
    (void) user;
    if (!g_ed.active) return;
    const cJSON *label = cJSON_GetArrayItem(frame, 0);
    if (!cJSON_IsString(label)) return;
    const char *l = label->valuestring;
    const cJSON *a1 = cJSON_GetArrayItem(frame, 1);
    const char *text = cJSON_IsString(a1) ? a1->valuestring : "";
    if (!strcmp(l, "pubMR")) {
        // publishMapResp: kn_brain answers "Map Published!" / "Map Updated!" (the thumbnail came inline)
        if (!g_publish_sent) return;
        g_publish_sent = 0;
        wait_done();
        map_resp(strstr(text, "Updated") ? "Game Updated!" : "Game Uploaded!", g_pub_name);
    } else if (!strcmp(l, "upMp")) {
        if (cJSON_GetArraySize(frame) > 2) return; // mod uploads: uploadResp(e, 1)
        // v10's uploadResp for every upMp, ulb's included (see the notes): #gameUpResp when the popup has it
        g_publish_sent = 0;
        wait_done();
        upload_resp(text);
    } else if (!strcmp(l, "pubR")) {
        // publishAssetResp: the files go to <srv>/upload-user-asset as multipart (not possible here)
        if (strcmp(g_wait, "pubR")) return;
        wait_done();
        upload_resp("Upload Request Failed");
    } else if (!strcmp(l, "edMp")) {
        if (strcmp(g_wait, "edMp")) return;
        wait_done();
        edit_map(a1);
    } else if (!strcmp(l, "mfr")) {
        if (strcmp(g_wait, "mfr")) return;
        wait_done();
        if (!a1 || cJSON_IsNull(a1)) io_po("Error", NULL, NULL);
        else io_po("SUCCESS", NULL, NULL);
    } else if (!strcmp(l, "po")) {
        // io_po(type, err, msg): delC's "Deleted", or a request's error
        if (!g_wait[0]) return;
        const int upload = g_wait_upload;
        wait_done();
        const cJSON *a2 = cJSON_GetArrayItem(frame, 2), *a3 = cJSON_GetArrayItem(frame, 3);
        if (upload) upload_resp(cJSON_IsString(a2) ? a2->valuestring : text);
        else io_po(cJSON_IsString(a1) ? a1->valuestring : NULL, cJSON_IsString(a2) ? a2->valuestring : NULL, cJSON_IsString(a3) ? a3->valuestring : NULL);
        g_publish_sent = 0;
    } else if (!strcmp(l, "__close")) {
        if (g_wait[0]) wait_failed("Upload Request Failed");
        g_publish_sent = 0;
    }
}

// ---------------------------------------------------------------------------
// per frame, script driver

// KRUNKNATIVE_EDITOR_SCRIPT "publish[:category]": wait for the account (session restore), run
// publishMap, then press the popup's Publish with that category (end-to-end test of uploadM)
static int g_auto, g_auto_cat;
static double g_auto_until;

void ed_net_script_publish(const char *arg) {
    g_auto = 1;
    g_auto_cat = arg ? atoi(arg) : 0;
    g_auto_until = g_ed.now + 20.0;
}

static void auto_publish(void) {
    if (g_auto == 1 && account() && brain_ok()) {
        g_auto = 2;
        ed_net_publish();
    } else if (g_auto == 2 && html_dom_by_id("gameCategory2") && !g_pub_state) {
        char v[8];
        snprintf(v, sizeof(v), "%d", g_auto_cat);
        html_dom_set_value(html_dom_by_id("gameCategory2"), v);
        html_dom_set_value(html_dom_by_id("gameDescNew"), "Published by the KRUNKNATIVE_EDITOR_SCRIPT test");
        g_auto = 0;
        upload_game();
    } else if (g_auto && g_ed.now > g_auto_until) {
        g_auto = 0;
        editor_message("publish script: no account after 20 s");
    }
}

void ed_net_frame(void) {
    // editor.c page_tick: right after the 3D view and its overlays were drawn
    if (g_auto) auto_publish();
    if (g_wait[0] && g_ed.now > g_wait_until) wait_failed("No reply from the server");
    if (!g_capture_pending) return;
    g_capture_pending = 0;
    free(g_thumb);
    g_thumb = capture_view(fmin(thumb_limit_kb(), BRAIN_THUMB_MAX * 0.75 / 1000));
    if (g_thumb) fprintf(stderr, "[editor] publish thumbnail %.1f kB\n", thumb_kb(g_thumb));
    preload_ready();
}

// ---------------------------------------------------------------------------
// handlers

static int arg_window(const char *name) {
    // "windows.<n>.<fn>" -> list window
    for (int w = 0; w < LW_COUNT; w++) {
        char p[16];
        snprintf(p, sizeof(p), "windows.%d.", k_lw[w].js);
        if (!strncmp(name, p, strlen(p))) return w;
    }
    return -1;
}

static void search(const int w) {
    // searchAssets / searchMods: "Loading..." then API_CLIENT.searchAsset(val, cat) / searchMod(val)
    const HtmlElem res = k_lw[w].results ? html_dom_by_id(k_lw[w].results) : 0;
    if (res) html_dom_set_html(res, "Loading...");
    char val[256], cat[16], path[600];
    snprintf(cat, sizeof(cat), "%d", k_lw[w].cat);
    read_value(k_lw[w].input, val, sizeof(val));
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

static double arg_num(const HtmlArg *a, const int n, const int i) {
    if (i >= n) return 0;
    if (a[i].kind == HTML_ARG_NUM || a[i].kind == HTML_ARG_BOOL) return a[i].num;
    if (a[i].kind == HTML_ARG_STR && a[i].str) return atof(a[i].str);
    return 0;
}

static const char *arg_str(const HtmlArg *a, const int n, const int i, char *buf, const size_t size) {
    buf[0] = 0;
    if (i < n && a[i].kind == HTML_ARG_STR && a[i].str) snprintf(buf, size, "%s", a[i].str);
    else if (i < n && a[i].kind == HTML_ARG_NUM) ed_model_number(a[i].num, buf, size);
    return buf;
}

static int h_net(void *u, const HtmlEvent *ev, const HtmlArg *a, const int n) {
    (void) u;
    const char *name = html_ui_current_handler();
    if (!name) return 1;
    char s1[256], s2[96];
    const int w = arg_window(name);
    if (w >= 0 && strstr(name, ".switchTab")) {
        const int tab = (int) arg_num(a, n, 0);
        const int max = w == LW_MODS ? MOD_TAB_COUNT : ASSET_TAB_COUNT;
        if (tab >= 0 && tab < max && tab != g_win[w].tab) {
            g_win[w].tab = tab;
            rerender(w);
        }
    } else if (w >= 0 && strstr(name, ".updateSort")) {
        // updateSort(select): sortIndex = selectedIndex; listData[1] = null; tabIndex = 0; switchTab(1)
        char v[8] = "0";
        if (ev && ev->target) html_dom_get_value(ev->target, v, sizeof(v));
        g_win[w].sort = atoi(v) == 1;
        list_clear(&g_win[w].list[1]);
        g_win[w].tab = 1;
        rerender(w);
    } else if (w >= 0 && (strstr(name, ".searchAssets") || strstr(name, ".searchMods"))) {
        search(w);
    } else if (!strcmp(name, "KE.selectAsset")) {
        select_asset(arg_str(a, n, 0, s1, sizeof(s1)), (int) arg_num(a, n, 1), arg_str(a, n, 2, s2, sizeof(s2)));
    } else if (!strcmp(name, "KE.selectMod")) {
        select_mod(arg_str(a, n, 0, s1, sizeof(s1)));
    } else if (!strcmp(name, "testSound")) {
        // SOUND.stopALL(); SOUND.play(id, 1, false, 1, true): the asset's sound.mp3
        char snd[64];
        snprintf(snd, sizeof(snd), "asset:%d", (int) arg_num(a, n, 0));
        audio_stop_all();
        audio_play(snd, 1, 0, 1);
    } else if (!strcmp(name, "copyAssetID")) {
        html_clipboard_write(arg_str(a, n, 0, s1, sizeof(s1)));
    } else if (!strcmp(name, "deleteCreationConf")) {
        delete_creation_conf(arg_num(a, n, 0), (int) arg_num(a, n, 1));
    } else if (!strcmp(name, "deleteCreation")) {
        delete_creation(arg_num(a, n, 0), (int) arg_num(a, n, 1));
    } else if (!strcmp(name, "openEdit")) {
        open_edit(arg_str(a, n, 0, s1, sizeof(s1)), arg_num(a, n, 1), arg_num(a, n, 2));
    } else if (!strcmp(name, "loadMap")) {
        load_map(arg_str(a, n, 0, s1, sizeof(s1)));
    } else if (!strcmp(name, "calcMapFee")) {
        calc_map_fee(ev ? ev->target : 0);
    } else if (!strcmp(name, "chngMapFunds")) {
        change_map_funds(arg_num(a, n, 0), (int) arg_num(a, n, 1));
    } else if (!strcmp(name, "updateMapLB")) {
        update_map_lb(arg_num(a, n, 0));
    } else if (!strcmp(name, "KE.publishAssetPopup")) {
        ed_net_publish_asset();
    } else if (!strcmp(name, "KE.updateAssetType")) {
        char v[8] = "0";
        if (n >= 1 && a[0].kind == HTML_ARG_THIS_VALUE && ev && ev->target) html_dom_get_value(ev->target, v, sizeof(v));
        else arg_str(a, n, 0, v, sizeof(v));
        update_asset_type(atoi(v));
    } else if (!strcmp(name, "KE.uploadAsset")) {
        upload_asset();
    } else if (!strcmp(name, "KE.uploadGame")) {
        upload_game();
    } else if (!strcmp(name, "KE.toggleAdvancedTexOptions")) {
        toggle_advanced_tex();
    } else if (!strcmp(name, "selectFile")) {
        const int f = file_index(arg_str(a, n, 0, s1, sizeof(s1)));
        if (f >= 0) ed_pick_file(k_files[f].accept, file_picked, (void *) (intptr_t) f);
    } else if (!strcmp(name, "shareMapLink")) {
        share_map_link(ev ? ev->target : 0, arg_str(a, n, 1, s1, sizeof(s1)));
    }
    return 1;
}

int ed_net_keydown(const HtmlEvent *ev) {
    // onkeydown='if(window.event.keyCode==13)windows[n].search...()' (statements are not run
    // by the page bridge: the editor's keydown listener calls this). 1 = handled.
    if (!ev || !ev->target || !html_key_is_enter(ev->key)) return 0;
    char id[16] = "";
    if (html_dom_get_attr(ev->target, "id", id, sizeof(id)) < 0) return 0;
    for (int w = 0; w < LW_COUNT; w++) {
        if (k_lw[w].input && !strcmp(id, k_lw[w].input)) {
            search(w);
            return 1;
        }
    }
    return 0;
}

// KRUNKNATIVE_EDITOR_SCRIPT "net:<call>" (the editor-6 publish test): net:publish-asset,
// net:asset-type:<n>, net:upload-asset, net:load:<name>, net:delete:<id>:<kind>, net:lb:<id>,
// net:funds:<id>:<withdraw>:<kr>, net:edit:<name>:<id>:<fund>, net:select:<id>:<type>:<from>,
// net:pending:<window> (the list as while a request is pending: v7's offline capture, uidiff),
// net:popup:<name> (v7ref.js's net-* popups with a stand-in account; tools/editor/windows_check.py)
int ed_net_script(const char *cmd, const char *arg) {
    if (strcmp(cmd, "net") || !arg) return 0;
    char a[256];
    snprintf(a, sizeof(a), "%s", arg);
    char *p[4] = {a, NULL, NULL, NULL};
    for (int i = 1; i < 4; i++) {
        p[i] = p[i - 1] ? strchr(p[i - 1], ':') : NULL;
        if (p[i]) *p[i]++ = 0;
    }
    if (!strcmp(p[0], "publish-asset")) ed_net_publish_asset();
    else if (!strcmp(p[0], "asset-type") && p[1]) update_asset_type(atoi(p[1]));
    else if (!strcmp(p[0], "upload-asset")) upload_asset();
    else if (!strcmp(p[0], "load") && p[1]) {
        snprintf(g_load_name, sizeof(g_load_name), "%s", p[1]);
        load_map_confirmed(NULL, "1");
    } else if (!strcmp(p[0], "delete") && p[1]) delete_creation(atof(p[1]), p[2] ? atoi(p[2]) : 0);
    else if (!strcmp(p[0], "lb") && p[1]) update_map_lb(atof(p[1]));
    else if (!strcmp(p[0], "funds") && p[1] && p[2] && p[3]) {
        html_dom_set_value(html_dom_by_id("mapFundsInp"), p[3]);
        change_map_funds(atof(p[1]), atoi(p[2]));
    } else if (!strcmp(p[0], "edit") && p[1] && p[2]) open_edit(p[1], atof(p[2]), p[3] ? atof(p[3]) : 0);
    else if (!strcmp(p[0], "select") && p[1] && p[2]) select_asset(p[1], atoi(p[2]), p[3] ? p[3] : "");
    else if (!strcmp(p[0], "popup") && p[1]) {
        // the popups of v7ref.js's net-* states (doc/ref/krunker-v7.1.4/editor/net/), same arguments
        memset(&g_fake_account, 0, sizeof(g_fake_account));
        snprintf(g_fake_account.name, sizeof(g_fake_account.name), "visuser");
        g_fake_account.id = 1;
        g_fake = 1;
        char *thumb = g_thumb, *map = g_pub_map;
        g_thumb = g_pub_map = NULL;
        if (!strcmp(p[1], "publish-game")) publish_game_popup(1, NULL);
        else if (!strcmp(p[1], "update-game")) {
            cJSON *row = cJSON_Parse("{\"map_id\":7,\"map_name\":\"VisMap\",\"map_description\":\"about\",\"map_updatecounter\":1,\"ca\":2,\"mob\":1}");
            publish_game_popup(0, row);
            cJSON_Delete(row);
        } else if (!strncmp(p[1], "publish-asset", 13)) {
            ed_net_publish_asset();
            if (!strcmp(p[1], "publish-asset-sound")) {
                html_dom_set_value(html_dom_by_id("gameType"), "3");
                update_asset_type(3);
            }
        } else if (!strcmp(p[1], "edit-map")) open_edit("VisMap", 7, 1234);
        else if (!strcmp(p[1], "map-resp")) map_resp("Game Uploaded!", "VisMap");
        else if (!strcmp(p[1], "delete-map")) delete_creation_conf(7, 0);
        g_thumb = thumb;
        g_pub_map = map;
        g_fake = 0;
    } else if (!strcmp(p[0], "uv") && p[1]) {
        // test: "uv:on" ticks "Advanced: per-map UV channel" (and shows its rows, as the checkbox's
        // onchange does); "uv:<slot>:<channel>" sets a slot's channel (slot = the popup's order)
        if (!strcmp(p[1], "on")) {
            html_dom_set_checked(html_dom_by_id("gameAdvancedTex"), 1);
            toggle_advanced_tex();
        } else if (p[2] && atoi(p[1]) >= 0 && atoi(p[1]) < UV_SLOTS) {
            char id[48];
            snprintf(id, sizeof(id), "game%sChan", k_uv_slot[atoi(p[1])]);
            html_dom_set_value(html_dom_by_id(id), p[2]);
        }
    } else if (!strcmp(p[0], "uvc")) {
        // test: the uploadA uvc the popup would send (null when the checkbox is off)
        cJSON *uvc = uv_channels();
        char *text = uvc ? cJSON_PrintUnformatted(uvc) : NULL;
        fprintf(stderr, "[editor] uvc %s\n", text ? text : "null");
        free(text);
        cJSON_Delete(uvc);
    } else if (!strcmp(p[0], "pending") && p[1] && lw_of_window(atoi(p[1])) >= 0) {
        // the list of window N as while its request has not been answered (v7's capture state; uidiff)
        const int w = lw_of_window(atoi(p[1]));
        ListData *d = &g_win[w].list[g_win[w].tab];
        list_clear(d);
        d->requested = 1;
        if (g_ed.window_index == k_lw[w].window) rerender(w);
    }
    else fprintf(stderr, "[editor] net: unknown \"%s\"\n", arg);
    return 1;
}

void ed_net_reset(void) {
    // page enter / leave: listData is per page load in v7
    for (int w = 0; w < LW_COUNT; w++) {
        for (int t = 0; t < LW_TABS; t++) list_clear(&g_win[w].list[t]);
        g_win[w].tab = w == LW_MAPS ? MAP_TAB_OWNED : 0;
        g_win[w].sort = 0;
        g_win[w].only_show = 0;
        g_win[w].from[0] = 0;
        g_win[w].gen++;
    }
    free(g_thumb);
    g_thumb = NULL;
    free(g_pub_map);
    g_pub_map = NULL;
    cJSON_Delete(g_pub_row);
    g_pub_row = NULL;
    g_pub_gen++;
    g_pub_state = 0;
    g_pub_lookup = 0;
    g_capture_pending = 0;
    g_publish_sent = 0;
    g_auto = 0;
    files_reset();
    wait_done();
}

void ed_net_install(HtmlHandlerEntry *table, size_t *count, const size_t max) {
    ed_net_reset();
    const HtmlHandlerEntry entries[] = {
        {"windows.1.switchTab", h_net}, {"windows.4.switchTab", h_net}, {"windows.6.switchTab", h_net}, {"windows.8.switchTab", h_net},
        {"windows.1.updateSort", h_net}, {"windows.6.updateSort", h_net},
        {"windows.1.searchAssets", h_net}, {"windows.4.searchMods", h_net}, {"windows.6.searchAssets", h_net}, {"windows.8.searchAssets", h_net},
        {"KE.selectAsset", h_net}, {"KE.selectMod", h_net}, {"testSound", h_net}, {"copyAssetID", h_net},
        {"deleteCreationConf", h_net}, {"deleteCreation", h_net}, {"openEdit", h_net}, {"loadMap", h_net}, {"calcMapFee", h_net},
        {"chngMapFunds", h_net}, {"updateMapLB", h_net}, {"KE.publishAssetPopup", h_net}, {"KE.updateAssetType", h_net},
        {"KE.uploadAsset", h_net}, {"KE.uploadGame", h_net}, {"KE.toggleAdvancedTexOptions", h_net}, {"selectFile", h_net}, {"shareMapLink", h_net},
    };
    for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]) && *count < max; i++) table[(*count)++] = entries[i];
}
