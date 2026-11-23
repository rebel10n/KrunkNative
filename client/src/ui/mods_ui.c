// client/src/ui/mods_ui.c - the menu's mods: the Mod Manager (window 4, Krunker's windows[3]), the Mod Loader
// (window 18, windows[17]) and Upload Mod (window 19, windows[18] "mods_pub").
//
// Krunker v10.0.7's behaviour this follows (game.deob.js):
//   - the Mod Manager's tabs (:18566-18600: Home, Popular, New, Favorites, My Mods, Loaded, Search) list
//     /webhooks/general/mods/list?index&pos&accountId (:133240) and search /webhooks/general/mods/search?val (:133247);
//     a card's Load calls loadUserMod(name, url, id, creator, thumb) (:223418, :225756), which loads the pack
//     (loadModPack) and adds it to the Loaded tab; Reset Mods drops every loaded mod (:218812-218823).
//   - mod_url "ua" is a Box Office / Krunker hosted pack: userAssetsUrl("/md" + mod_id + "/mod.zip") (:217728, :223453).
//   - the Mod Loader loads a pack by URL (loadModURL, :225764) or from a picked .zip (loadMod, :225961).
//   - Upload Mod (mods_pub, :220943-221050): signed out, the window asks to log in (the static 19.html); signed in,
//     v10's markup: Thumbnail (PNG) (level 10, else "Level 10 required" and a Locked link; 250 / 500 KB premium,
//     KB = 1e3 bytes), Mod File (.zip) (25 / 50 MB premium; MB = 1e6), the name, #modUpResp and Publish
//     (windows[18].submit). onFileChange writes a file's name into #<id>H's title and round(size, 2) / limit into
//     #<id>Size. submit: "Mod name is required", "Select a .zip mod file", "Mod .zip exceeds N MB limit", "Thumbnails
//     require level 10+", then "Requesting upload key..." api.mods.publishKey({name, assetSizes}), "Uploading..."
//     publishUpload(uploadUrl, fileId, zip, thumb) -> {data: {mod_id}}; errors through errors.mods.* (else "Mod
//     upload failed. Please try again."); on success the window closes, "Mod Published!" shows, and the Mod Manager
//     forgets its lists and opens on My Mods.
//   - ?mod=<name> on the page loads that mod by name (:226181-226190; here GET /mods/by-name/<name>).
//   - the cards (iìïïííî, :223416-223459): the Favorite star (favoriteM, :228231-228245: `sel` = not a favourite,
//     FAVORITE_MAPMOD(1, id, wasSel)), NEW! / UPDATED, the thumbnail, the name opening the mod view, "<n> likes" and
//     the liked-by-friend line; Home is the featured banner (bigMFeatHold); a load writes #modLInfo ("loading mod...",
//     "Success!", "mod error!", "mods disabled", "invalid mod file!", :225733-225762, :225961).
//   - the mod view (openModDetail, svelte-1v2j2f2, :108350-108478): version, votes, creator, Created / Updated,
//     description, Share (copies <origin>/?mod=<name>), Use, Report, and for a developer Feature / Delete.
//   - Reset Mods (:218818-218831) answers the SUCCESS popup "Will take effect after respawning."; Ctrl+/ (:209629-209632)
//     clears the last mod and resets quietly ("Mods Cleared" in chat); a map's modURL loads as "Server Mod" (:226475).
//
// Here (kn_brain, services/brain/asset_upload.c): publishKey is POST /mods/publish/key?token=, the files go to
// uploadUrl (/mods/publish) as chunks (?fileId=&key=&offset=, then &done=1) like the editor's assets, because the brain
// takes request bodies of 1 MiB; a chunk whose request failed outright (a brain restart: its tickets survive it) is
// sent again a few times before the upload gives up. Errors come back as MODS_* codes shown through errors.mods.* (i18n).
//
// Native differences: the page's <input type=file> does nothing natively, so selectFile (the Select links) and the
// Mod Loader's drop area (knModPickLocal) open the platform file dialog (ed_pick_file); the picked file's name, size
// and preview are written in place as onFileChange does.
#include <creation_vote.h>
#include <backend.h>
#include <backend_ui.h>
#include <html_dom.h>
#include <html_ui.h>
#include <i18n.h>
#include <krunker_net.h> // FAVORITE_MAPMOD on the game socket for a krunker.io account
#include <mod_pack.h>
#include <ui_logic.h>
#include <v10_social.h> // the mod view's creator: openPlayerProfile
#include <v10_ui.h>     // the mod view on the view stack

#include <cJSON.h>
#include <stdint.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <ctype.h>
#include <time.h>

// the platform file dialog (client/src/editor/editor_tools.c; editor_internal.h pulls in GL)
typedef void (*EdFileFn)(void *user, const char *path);
void ed_pick_file(const char *accept, EdFileFn fn, void *user);

#define MODS_WINDOW 4
#define LOADER_WINDOW 18
#define PUBLISH_WINDOW 19
#define USER_ASSETS "https://user-assets.krunker.io"

// the thumbnails' user-assets host (iíiiiîì, config-10.0.7.js:4729-4744): Krunker's for a krunker.io account, else
// kn_brain's /user-assets, which has a Box Office mod's md<id>/thumb.png (the remote image loader fetches the URL as
// given; mod.zip goes through mod_pack.c, which knows both hosts)
static const char *thumb_host(void) {
    static char host[300];
    if (account_backend() == ACCOUNT_BACKEND_KRUNKER || !backend_url()[0]) return USER_ASSETS;
    snprintf(host, sizeof host, "%s/user-assets", backend_url());
    return host;
}
#define CHUNK (512u * 1024u)        // kn_brain's request bodies are 1 MiB at most
#define CHUNK_TIMEOUT_MS 60000
#define CHUNK_RETRIES 20            // a request that failed outright (status 0): a restarting brain
#define CHUNK_RETRY_MS 1500
#define LOADED_MAX 16
#define UPLOAD_FAILED "Mod upload failed. Please try again." // v10's text for an error without errors.mods.* (:223326-223340)

enum { TAB_HOME, TAB_POPULAR, TAB_NEW, TAB_FAV, TAB_MINE, TAB_LOADED, TAB_SEARCH, TAB_COUNT };

static const char *const k_tab_names[TAB_COUNT] = {"Home", "Popular", "New", "Favorites", "My Mods", "Loaded", "Search"};

typedef struct {
    char *html;   // the cards so far
    cJSON *list;  // the last page's rows (listData[tab].list: the Home tab's featured mod)
    int used, waiting, page;
} TabData;

typedef struct {
    char name[128], url[512], id[24], creator[64];
    int thumb;
} LoadedMod;

static struct {
    int tab;
    TabData data[TAB_COUNT];
    unsigned gen;                // bumped by a reset of the lists: older answers are dropped
    LoadedMod loaded[LOADED_MAX];
    int nloaded;
    char search_html_pending;
} M;

// ------------------------------------------------------------------ helpers

static void esc(StrBuf *sb, const char *s) { sb_append_esc(sb, s ? s : ""); }

// a value for a single-quoted onclick argument: quotes and backslashes dropped
static void arg(StrBuf *sb, const char *s) {
    for (const char *p = s ? s : ""; *p; p++) {
        if (*p == '\'' || *p == '"' || *p == '\\' || *p == '<' || *p == '>' || *p == '&' || (unsigned char) *p < 0x20) continue;
        char c[2] = {*p, 0};
        sb_append(sb, c);
    }
}

static const char *jstr(const cJSON *o, const char *a, const char *b) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, a);
    if (!cJSON_IsString(v) && b) v = cJSON_GetObjectItemCaseSensitive(o, b);
    return cJSON_IsString(v) ? v->valuestring : "";
}

static double jnum(const cJSON *o, const char *a, const char *b) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, a);
    if (!v && b) v = cJSON_GetObjectItemCaseSensitive(o, b);
    return cJSON_IsNumber(v) ? v->valuedouble : cJSON_IsString(v) ? atof(v->valuestring) : 0;
}

static void jid(const cJSON *o, char *out, const size_t n) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, "mid");
    if (!v || cJSON_IsNull(v)) v = cJSON_GetObjectItemCaseSensitive(o, "mod_id");
    if (cJSON_IsNumber(v)) snprintf(out, n, "%.0f", v->valuedouble);
    else snprintf(out, n, "%s", cJSON_IsString(v) ? v->valuestring : "");
}

// mod_info's "t" (a thumbnail was uploaded): mod_info is JSON text or an object
static int info_thumb(const cJSON *row) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(row, "minf");
    if (!v || cJSON_IsNull(v)) v = cJSON_GetObjectItemCaseSensitive(row, "mod_info");
    cJSON *parsed = cJSON_IsString(v) ? cJSON_Parse(v->valuestring) : NULL;
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(parsed ? parsed : v, "t");
    const int on = t && !cJSON_IsFalse(t) && !(cJSON_IsNumber(t) && t->valuedouble == 0);
    cJSON_Delete(parsed);
    return on;
}

// the pack's URL: "ua" is a hosted pack, md<id>/mod.zip on the user-asset host (Box Office ids from the brain)
static void mod_url_of(const char *url, const char *id, char *out, const size_t n) {
    if (url && !strcmp(url, "ua")) snprintf(out, n, USER_ASSETS "/md%s/mod.zip", id ? id : "");
    else snprintf(out, n, "%s", url ? url : "");
}

static const char *error_text(const char *code, const char *fallback) {
    static const struct { const char *code, *key; } k[] = {
        {"MODS_PUBLISH_LEVEL_TOO_LOW", "errors.mods.publishLevelTooLow"}, {"MODS_PUBLISH_COOLDOWN", "errors.mods.publishCooldown"},
        {"MODS_PUBLISH_NAME_RESTRICTED", "errors.mods.publishNameRestricted"}, {"MODS_PUBLISH_NAME_PROFANE", "errors.mods.publishNameProfane"},
        {"MODS_PUBLISH_SIZE_LIMIT", "errors.mods.publishSizeLimit"}, {"MODS_PUBLISH_THUMB_LEVEL", "errors.mods.publishThumbLevel"},
        {"MODS_PUBLISH_HACKER_FLAGGED", "errors.mods.publishHackerFlagged"}, {"MODS_UPLOAD_INVALID_KEY", "errors.mods.uploadInvalidKey"},
        {"MODS_UPLOAD_SIZE_MISMATCH", "errors.mods.uploadSizeMismatch"}, {"MODS_UPLOAD_STORAGE_FAIL", "errors.mods.uploadStorageFail"},
        {"MODS_UPLOAD_MISSING_FILE", "errors.mods.uploadMissingFile"}, {"MODS_UPLOAD_WRONG_TYPE", "errors.mods.uploadWrongType"},
        {"MODS_UPLOAD_NAME_TAKEN", "errors.mods.uploadNameTaken"},
    };
    for (size_t i = 0; code && i < sizeof k / sizeof k[0]; i++)
        if (!strcmp(code, k[i].code)) return i18n_tr(k[i].key, fallback);
    return fallback;
}

// the {"error": {code, message}} of an answer: the i18n text of its code, else its message, else `fallback`
static void answer_error(const char *body, char *out, const size_t n, const char *fallback) {
    cJSON *j = body ? cJSON_Parse(body) : NULL;
    const cJSON *e = cJSON_GetObjectItemCaseSensitive(j, "error");
    const char *code = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(e, "code"));
    const char *msg = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(e, "message"));
    const char *t = error_text(code, NULL);
    snprintf(out, n, "%s", t ? t : msg && *msg ? msg : cJSON_IsString(e) ? e->valuestring : fallback);
    cJSON_Delete(j);
}

static void popup(const int kind, const char *text) { account_popup(kind, NULL, text); }

// the Mod Loader's #modLInfo line (iiìíiiï, deob-10.0.7.js:225733-225740): loadModPack's progress and errors
static void loader_info(const char *text) {
    HtmlElem e = html_dom_by_id("modLInfo");
    if (e) html_dom_set_text(e, text);
}

// ------------------------------------------------------------------ the player's loaded mods

// windows[3].loaded (deob-10.0.7.js:218805-218817): once per name; no name is "Auto Loaded" for the last mod loaded
// again at start, "Server Mod" for a map's pack, else "Mod"
static void loaded_add(const char *name, const char *url, const char *id, const char *creator, const int thumb) {
    if (!name || !*name) name = "Mod";
    for (int i = 0; i < M.nloaded; i++)
        if (!strcmp(M.loaded[i].name, name)) return;
    if (M.nloaded == LOADED_MAX) {
        memmove(&M.loaded[0], &M.loaded[1], sizeof(M.loaded[0]) * (LOADED_MAX - 1));
        M.nloaded--;
    }
    LoadedMod *m = &M.loaded[M.nloaded++];
    snprintf(m->name, sizeof m->name, "%s", name);
    snprintf(m->url, sizeof m->url, "%s", url ? url : "");
    snprintf(m->id, sizeof m->id, "%s", id ? id : "");
    snprintf(m->creator, sizeof m->creator, "%s", creator ? creator : "");
    m->thumb = thumb;
}

// loadModPack(url, true, server, name, id, creator, thumb) (deob-10.0.7.js:225865-225955) for the player's mod (the
// Mod Manager's Load, the Mod Loader's URL, ?mod=): canLoadMods, then setLastMod (not for ?mod=), the #modVote of a
// named mod, windows[3].loaded, and the pack (mod_pack.h, kept over map changes); its progress and errors in #modLInfo
static int load_user_mod(const char *name, const char *url, const char *id, const char *creator, const int thumb, const int server) {
    char full[600];
    mod_url_of(url, id, full, sizeof full);
    if (!mods_ui_can_load()) {
        loader_info(I18N("mod.disabled", "mods disabled"));
        return 0;
    }
    if (!server && !ui_setting_is_false("autoLoadLast") && ui_setting_value("autoLoadLast")) html_storage_set("krk_lastMod", full);
    creation_vote_set_mod(id ? atoi(id) : 0, name, creator); // #modVote: a named mod's vote (:225872-225880)
    loaded_add(name, full, id, creator, thumb);
    menu_rerender_if_open(MODS_WINDOW);
    if (!mod_pack_load_user(full)) {
        loader_info(mod_pack_refused() ? I18N("mod.disabled", "mods disabled") : I18N("mod.error", "mod error!"));
        return 0;
    }
    loader_info(I18N("generic.success", "Success!")); // the last entry extracted (iììiíii.close, :225747-225752)
    return 1;
}

// loadMod (deob-10.0.7.js:225961-225966): a .zip says "loading mod..." and loads (loadModPack(dataURL): no name, no
// last mod, no vote), anything else "invalid mod file!"
static void picked_local(void *user, const char *path) {
    (void) user;
    const size_t n = path ? strlen(path) : 0;
    if (n < 4 || strcasecmp(path + n - 4, ".zip")) {
        loader_info(I18N("mod.invalid", "invalid mod file!"));
        return;
    }
    loader_info(I18N("mod.loading", "loading mod..."));
    if (!mods_ui_can_load()) {
        loader_info(I18N("mod.disabled", "mods disabled"));
        return;
    }
    creation_vote_set_mod(0, NULL, NULL);
    loaded_add(NULL, path, "", "", 0);
    menu_rerender_if_open(MODS_WINDOW);
    if (!mod_pack_load_file(path)) {
        loader_info(mod_pack_refused() ? I18N("mod.disabled", "mods disabled") : I18N("mod.error", "mod error!"));
        return;
    }
    loader_info(I18N("generic.success", "Success!"));
}

// ------------------------------------------------------------------ the Mod Manager (window 4)

static void list_request(int tab, int page);

// UTILS.formatNumYT (deob-10.0.7.js:23796-23806): from 1000 on, thousands to two significant digits, else one, so at
// most two digits remain; one decimal when fractional ("1.2k", "12k")
static void format_num_yt(char *out, const size_t n, const double v) {
    static const char *const suffix[] = {"", "k", "m", "b", "t"};
    if (!(v >= 1000)) {
        snprintf(out, n, "%.15g", v > 0 ? v : 0);
        return;
    }
    char whole[40];
    snprintf(whole, sizeof whole, "%.15g", v);
    int k = (int) (strlen(whole) / 3);
    if (k > 4) k = 4;
    const double scaled = v / pow(1000.0, k);
    double r = scaled;
    for (int p = 2; p >= 1; p--) {
        char buf[40];
        snprintf(buf, sizeof buf, "%.*g", p, scaled); // toPrecision(p) -> parseFloat
        r = atof(buf);
        snprintf(buf, sizeof buf, "%.15g", r);
        size_t d = 0;
        for (const char *q = buf; *q; q++) d += isalnum((unsigned char) *q) != 0;
        if (d <= 2) break;
    }
    if (r != floor(r)) snprintf(out, n, "%.1f%s", r, suffix[k]);
    else snprintf(out, n, "%.15g%s", r, suffix[k]);
}

// (v || 0).toString() with the thousands commas (deob-10.0.7.js:23949-23951)
static void format_commas(char *out, const size_t n, const double v) {
    char raw[48];
    snprintf(raw, sizeof raw, "%.0f", v);
    const size_t len = strlen(raw), lead = raw[0] == '-';
    size_t o = 0;
    for (size_t i = 0; i < len && o + 2 < n; i++) {
        if (i > lead && (len - i) % 3 == 0) out[o++] = ',';
        out[o++] = raw[i];
    }
    out[o] = 0;
}

// days since a row's mod_date ("2026-10-15T10:00:00Z" / "2026-10-15 10:00:00+00"), -1 when it does not parse
// ((Date.now() - new Date(date)) / 864e5, deob-10.0.7.js:223444-223449)
static double days_since(const char *date) {
    int y, mo, d, h = 0, mi = 0, sec = 0;
    if (!date || sscanf(date, "%d-%d-%d%*[T ]%d:%d:%d", &y, &mo, &d, &h, &mi, &sec) < 3) return -1;
    y -= mo <= 2;
    const long era = (y >= 0 ? y : y - 399) / 400;
    const long yoe = y - era * 400, doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const long days = era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
    const double then = (double) days * 86400.0 + h * 3600.0 + mi * 60.0 + sec;
    const double age = ((double) time(NULL) - then) / 86400.0;
    return age >= 0 ? age : 0;
}

// ---- the account's favourite mods (iìíîîïì[1], deob-10.0.7.js:228180-228217): ids loaded once from the Favorites
// tab's first page for the signed-in account, with the toggles made meanwhile (edits) laid over them
#define FAV_MAX 256
typedef struct {
    long long id;
    int fav;
} FavId;
static struct {
    int acc;    // the account the set belongs to, 0 none
    int loaded; // the ids came back (before that, the edits only)
    unsigned gen;
    int n;
    FavId e[FAV_MAX]; // the loaded ids (fav 1) and the edits: one entry per id, the last word wins
} F;

static void fav_put(const long long id, const int fav) {
    for (int i = 0; i < F.n; i++)
        if (F.e[i].id == id) {
            F.e[i].fav = fav;
            return;
        }
    if (F.n < FAV_MAX) F.e[F.n++] = (FavId) {id, fav};
}

// -1 unknown, else 0 / 1
static int fav_known(const char *id) {
    const long long v = id ? strtoll(id, NULL, 10) : 0;
    if (v <= 0 || !F.acc) return -1;
    for (int i = 0; i < F.n; i++)
        if (F.e[i].id == v) return F.e[i].fav;
    return F.loaded ? 0 : -1;
}

// iíììïïï(1): every mod star on the page follows the set (sel = not favourited)
static void fav_paint(void) {
    if (!F.loaded) return;
    HtmlElem stars[256];
    const int n = html_dom_query_all(0, ".mapActionB[data-fav='1']", stars, 256);
    for (int i = 0; i < n; i++) {
        char id[32] = "";
        if (html_dom_get_attr(stars[i], "data-favid", id, sizeof id) < 0) continue;
        const int fav = fav_known(id) == 1;
        html_dom_set_class(stars[i], "sel", !fav);
        HtmlElem icon = html_dom_query(stars[i], ".material-icons");
        if (icon) html_dom_set_text(icon, fav ? "star" : "star_border");
    }
}

static void on_fav_ids(void *user, const int status, const char *body, const size_t length) {
    (void) length;
    if ((unsigned) (uintptr_t) user != F.gen) return;
    cJSON *j = status == 200 && body ? cJSON_Parse(body) : NULL;
    const cJSON *rows = cJSON_GetObjectItemCaseSensitive(j, "data");
    if (!cJSON_IsArray(rows)) { // an error: forgotten, so the next showing asks again
        F.acc = 0;
        F.n = 0;
        cJSON_Delete(j);
        return;
    }
    FavId edits[FAV_MAX]; // the toggles made while the list was on its way go over the ids
    const int ne = F.n;
    memcpy(edits, F.e, sizeof(F.e[0]) * (size_t) ne);
    F.n = 0;
    const cJSON *r;
    cJSON_ArrayForEach(r, rows) {
        char id[24];
        jid(r, id, sizeof id);
        if (id[0]) fav_put(strtoll(id, NULL, 10), 1);
    }
    for (int i = 0; i < ne; i++) fav_put(edits[i].id, edits[i].fav);
    F.loaded = 1;
    cJSON_Delete(j);
    fav_paint();
}

// iìíìiíí(1) (the window's post): the set for the signed-in account, once
static void fav_sync(void) {
    const AccountInfo *acc = active_account();
    if (!acc || acc->id <= 0) return;
    if (F.acc == acc->id && (F.loaded || F.n)) {
        fav_paint();
        return;
    }
    F.acc = acc->id;
    F.loaded = 0;
    F.n = 0;
    F.gen++;
    char path[160];
    snprintf(path, sizeof path, "/webhooks/general/mods/list?index=%d&pos=0&accountId=%d", TAB_FAV, acc->id);
    backend_http_get(path, on_fav_ids, (void *) (uintptr_t) F.gen);
}

// iìïïííî(rows, "mod", tab) for one row (deob-10.0.7.js:223416-223459)
static void card(StrBuf *sb, const cJSON *r, const int tab) {
    char id[24], url[600], likes[32];
    jid(r, id, sizeof id);
    const char *name = jstr(r, "mna", "mod_name"), *creator = jstr(r, "ctn", "creatorname");
    const int thumb = info_thumb(r);
    mod_url_of(jstr(r, "url", "mod_url"), id, url, sizeof url);
    format_num_yt(likes, sizeof likes, jnum(r, "vte", "mod_votes"));
    const int fav = jnum(r, "fav", NULL) != 0 || tab == TAB_FAV || fav_known(id) == 1;
    sb_append(sb, "<div style='display:inline-block;'><div class='mapListItem'><div class='mapActionOvrl'><div class='mapActionHol'>"
                  "<div class='mapActionB' title='Load' onclick='loadUserMod(&quot;");
    arg(sb, name);
    sb_append(sb, "&quot;,&quot;");
    arg(sb, url);
    sb_appendf(sb, "&quot;,&quot;%s&quot;,&quot;", id);
    arg(sb, creator);
    sb_appendf(sb, "&quot;,%d)'><span class='material-icons' style='font-size:70px;color:#fff;'>play_arrow</span></div>"
                   "<div class='mapActionSep'></div>", thumb);
    // the star: sel while NOT a favourite (favoriteM flips it, deob-10.0.7.js:228231-228236)
    sb_appendf(sb, "<div class='mapActionB%s' data-fav='1' data-favid='%s' onclick='favoriteM(1,this, %s);'>"
                   "<span class='material-icons' style='font-size:70px;color:#fff;'>%s</span></div></div></div>",
               fav ? "" : " sel", id, id[0] ? id : "0", fav ? "star" : "star_border");
    const double age = days_since(jstr(r, "dte", "mod_date"));
    if (age >= 0 && age <= 7) {
        const int upd = jnum(r, "upd", "mod_updatecounter") > 1;
        sb_appendf(sb, "<div class='mapAgeD' style='background-image:%s'>%s</div>",
                   upd ? "linear-gradient(to right,#E040FB,#6240fb);" : "linear-gradient(to right,#fb9c40,#fb4040);", upd ? "UPDATED" : "NEW!");
    }
    sb_append(sb, "<div style='height:200px;width:100%;overflow: hidden;'><img class='mapListThumb' loading='lazy' src='");
    if (thumb) sb_appendf(sb, "%s/md%s/thumb.png", thumb_host(), id);
    else {
        const char *img = jstr(r, "mod_image", NULL);
        esc(sb, *img ? img : "./img/noimg.png");
    }
    sb_append(sb, "' /></div><div style='margin-top:5px;padding-left:5px;padding-right:5px;line-height:30px;margin-bottom:0px;'>"
                  "<div style='display:inline-block;max-width:250px;overflow:hidden;white-space:nowrap'><a href='javascript:;' "
                  "onclick='window.openModDetail(&quot;");
    arg(sb, name);
    sb_append(sb, "&quot;)' />");
    esc(sb, name);
    sb_appendf(sb, "</a></div><span style='font-size:15px;text-align:right;padding-right:2px;float:right;color:#fff;'>%s "
                   "<span style='color:rgba(255,255,255,0.5)'>likes</span></span></div></div>", likes);
    const char *friendname = jstr(r, "friendname", NULL);
    if (*friendname) {
        sb_append(sb, "<div class='likdByFrnd'><span class='material-icons' style='color:#fff;font-size:23px;vertical-align:middle;"
                      "margin-right:6px;margin-top:-4px;'>thumb_up</span>liked by <span style='color:rgba(255,255,255,0.6);'>");
        esc(sb, friendname);
        sb_append(sb, "</span></div>");
    } else {
        sb_append(sb, "<div style='height:10px;'></div>");
    }
    sb_append(sb, "</div>");
}

// windows[3].modsLoaded(data, index, error) (deob-10.0.7.js:218782-218798): a page's cards are added to the tab's; an
// answer with an error and nothing before shows the error in the list; an empty answer with nothing before shows
// "No Mods found"; both of those forget the tab, so it asks again when shown next
static void on_list(void *user, const int status, const char *body, const size_t length) {
    (void) length;
    (void) status;
    const unsigned gen = (unsigned) ((uintptr_t) user >> 8);
    const int tab = (int) ((uintptr_t) user & 0xff);
    if (gen != M.gen || tab < 0 || tab >= TAB_COUNT) return;
    TabData *d = &M.data[tab];
    d->waiting = 0;
    cJSON *j = body ? cJSON_Parse(body) : NULL;
    const cJSON *rows = cJSON_GetObjectItemCaseSensitive(j, "data");
    const cJSON *err = cJSON_GetObjectItemCaseSensitive(j, "error");
    const int n = cJSON_GetArraySize(rows);
    const char *empty_text = NULL;
    if (n && cJSON_IsString(err) && err->valuestring[0]) {
        if (!d->html) empty_text = err->valuestring;
    } else if (n) {
        StrBuf sb = {0};
        if (d->html) sb_append(&sb, d->html);
        const cJSON *r;
        cJSON_ArrayForEach(r, rows) card(&sb, r, tab);
        free(d->html);
        d->html = sb_finish(&sb);
        cJSON_Delete(d->list);
        d->list = cJSON_Duplicate(rows, 1);
        if (M.tab == tab) menu_rerender_if_open(MODS_WINDOW);
    } else if (!d->html) {
        empty_text = I18N("windows.mods.none", "No Mods found");
    }
    if (empty_text) {
        HtmlElem list = M.tab == tab ? html_dom_by_id("modList") : 0;
        if (list) html_dom_set_html(list, empty_text);
        cJSON_Delete(d->list);
        memset(d, 0, sizeof *d);
    }
    cJSON_Delete(j);
}

static void list_request(const int tab, const int page) {
    const AccountInfo *acc = active_account();
    char path[160];
    snprintf(path, sizeof path, "/webhooks/general/mods/list?index=%d&pos=%d&accountId=%d", tab, page, acc ? acc->id : 0);
    M.data[tab].waiting = 1;
    M.data[tab].used = 1;
    M.data[tab].page = page;
    if (!backend_http_get(path, on_list, (void *) (uintptr_t) (M.gen << 8 | (unsigned) tab))) M.data[tab].waiting = 0;
}

// windows[3].searchResp(data) (deob-10.0.7.js:218769-218774): the cards of the Search tab (searchId), else none
static void on_search(void *user, const int status, const char *body, const size_t length) {
    (void) user;
    (void) length;
    cJSON *j = status == 200 && body ? cJSON_Parse(body) : NULL;
    const cJSON *rows = cJSON_GetObjectItemCaseSensitive(j, "data");
    StrBuf sb = {0};
    if (cJSON_GetArraySize(rows)) {
        sb_append(&sb, "<div style='height:10px'></div>");
        const cJSON *r;
        cJSON_ArrayForEach(r, rows) card(&sb, r, TAB_SEARCH);
    } else {
        sb_append(&sb, I18N("windows.mods.none", "No Mods found"));
    }
    char *html = sb_finish(&sb);
    HtmlElem out = html_dom_by_id("searchResultsMO");
    if (out) html_dom_set_html(out, html);
    free(html);
    cJSON_Delete(j);
    fav_paint();
}

static void search(void) {
    char val[128] = "", enc[400], path[480];
    HtmlElem in = html_dom_by_id("mdSrch");
    if (in) html_dom_get_value(in, val, sizeof val);
    backend_url_encode(enc, sizeof enc, val);
    snprintf(path, sizeof path, "/webhooks/general/mods/search?val=%s", enc);
    HtmlElem out = html_dom_by_id("searchResultsMO");
    if (out) html_dom_set_html(out, "Loading...");
    backend_http_get(path, on_search, NULL);
}

// the Home tab's featured mod: a random row of the tab's last page (deob-10.0.7.js:218849-218857)
static void featured(StrBuf *sb, const cJSON *rows) {
    const int n = cJSON_GetArraySize(rows);
    if (n <= 0) return;
    const cJSON *r = cJSON_GetArrayItem(rows, rand() % n);
    char id[24], url[600], votes[48];
    jid(r, id, sizeof id);
    const int thumb = info_thumb(r);
    mod_url_of(jstr(r, "url", "mod_url"), id, url, sizeof url);
    format_commas(votes, sizeof votes, jnum(r, "vte", "mod_votes"));
    StrBuf c = {0};
    arg(&c, jstr(r, "ctn", "creatorname")); // iìïìîiï: the creator's quotes dropped
    char *creator = sb_finish(&c);
    if (!creator) creator = strdup("");
    const char *name = jstr(r, "mna", "mod_name");
    // url(<thumbnail> + ".png"): v10 appends .png to the thumbnail's URL (or to nothing)
    sb_append(sb, "<div class='bigMFeatHold' style='background-image:linear-gradient(to right, black, rgba(0, 0, 0, 0.5)),url(");
    if (thumb) sb_appendf(sb, "%s/md%s/thumb.png", thumb_host(), id);
    sb_append(sb, ".png);'><div id='bigMFeatName' style='top:200px;'>");
    esc(sb, name);
    sb_appendf(sb, "</div><div id='bigMFeatVbar' style='top:275px;'>%s<span class='material-icons' style='margin-left:10px;font-size:33px;"
                   "vertical-align:top;color:rgba(255,255,255,0.5);'>thumb_up</span><div style='color:#7d7d7d;margin-left:20px;"
                   "margin-top:-12px;display:inline-block;vertical-align:middle;'>by <a>", votes);
    esc(sb, creator);
    sb_append(sb, "</a></div></div><div id='bigMFeatPBtn' style='bottom:30px;width:230pxheight:40px;padding-top:10px;' onclick='loadUserMod(`");
    arg(sb, name);
    sb_append(sb, "`,`");
    arg(sb, url);
    sb_appendf(sb, "`,`%s`,`%s`,%d)'>Load Mod <span class='material-icons' style='font-size:40px;color:#fff;vertical-align:top;"
                   "margin-top:-3px;margin-left:5px'>move_to_inbox</span></div><div id='bigMFeatHBtn' style='bottom:30px;right:320px;"
                   "width:auto;height:40px;padding-top:8px;' onclick='openPlayerProfile(`%s`)'>by <span style='color:#fff;'>",
               id, creator, thumb, creator);
    esc(sb, creator);
    sb_append(sb, "</span></div></div><div class='mapListCat' style='margin-bottom:10px;padding-bottom:10px;margin-top:20px;'>Featured Mods</div>");
    free(creator);
}

// windows[3].gen (deob-10.0.7.js:218832-218859)
static char *manager_gen(void) {
    StrBuf sb = {0};
    sb_append(&sb, "<a href='javascript:;' onclick='showWindow(18)' class='menuLink'>Load Mod</a> | "
                   "<a href='javascript:;' onclick='showWindow(19)' class='menuLink'>Upload Mod</a>"
                   "<div style='float:right'> <span style='color:rgba(255,255,255,0.5)'>30,000+</span> Mods | "
                   "<a href='/viewer.html' class='menuLink'>Model Viewer</a></div>"
                   "<div style='margin-top:30px;margin-bottom:10px'><div class='bigMenTabs'>");
    for (int i = 0; i < TAB_COUNT; i++) {
        sb_appendf(&sb, "<div class='bigMenTab%s' onmouseenter='playTick(0.1);' onclick='playSelect();windows[3].switchTab(%d)'>",
                   M.tab == i ? " bigMenTabA" : "", i);
        if (i == TAB_HOME) sb_append(&sb, "<span class='material-icons-outlined' style='color:#fbc02d;font-size:42px;vertical-align:bottom'>home</span>");
        else if (i == TAB_SEARCH) sb_append(&sb, "<span class='material-icons' style='color:#ffffff;font-size:42px;vertical-align:bottom'>search</span>");
        else sb_append(&sb, k_tab_names[i]);
        sb_append(&sb, "</div>");
    }
    sb_append(&sb, "</div></div>");
    StrBuf list = {0};
    if (M.tab == TAB_SEARCH) {
        sb_append(&list, "<input type='text' id='mdSrch' class='smlInput' placeholder='Mod Name or Keyword' "
                         "onkeydown='if(window.event.keyCode==13)windows[3].searchMods()' /><div class='searchBtn' onclick='windows[3].searchMods()'>"
                         "<span class='material-icons' style='color:#fff;font-size:45px;margin-bottom:-30px;line-height:75px'>search</span></div>"
                         "<div style='color:rgba(0,0,0,0.5);margin-top:10px' id='searchResultsMO'></div>");
    } else if (M.tab == TAB_LOADED) {
        for (int i = 0; i < M.nloaded; i++) {
            const LoadedMod *m = &M.loaded[i];
            sb_append(&list, "<div class='mapListItem'><div style='height:200px;width:100%;overflow: hidden;'><img class='mapListThumb' loading='lazy' src='");
            if (m->thumb && m->id[0]) sb_appendf(&list, "%s/md%s/thumb.png", thumb_host(), m->id);
            else sb_append(&list, "./img/noimg.png");
            sb_append(&list, "' /></div><div style='margin-top:1px'><a href='javascript:;'>");
            esc(&list, m->name);
            sb_append(&list, "</a></div></div>");
        }
        if (M.nloaded) sb_append(&list, "<div class='loadMoreD' onclick='windows[3].reset()'>Reset Mods</div>");
        else sb_append(&list, I18N("windows.mods.none", "No Mods found"));
    } else {
        TabData *d = &M.data[M.tab];
        if (!d->used) list_request(M.tab, 0);
        if (d->html) {
            sb_append(&list, d->html);
            if (d->waiting) sb_append(&list, "<div class='loadMoreW'>Loading...</div>");
            else if (M.tab != TAB_FAV && M.tab != TAB_HOME) // fav / noPages
                sb_append(&list, "<div class='loadMoreD' onclick='windows[3].loadMore(this)'>Load More</div>");
        } else {
            sb_append(&list, "<div class='loadMoreW'>Loading...</div>");
        }
        if (M.tab == TAB_HOME && d->list) featured(&sb, d->list);
    }
    sb_append(&sb, "<div style='color:rgba(255,255,255,0.3)' id='modList'>");
    char *l = sb_finish(&list);
    if (l) sb_append(&sb, l);
    free(l);
    sb_append(&sb, "</div>");
    return sb_finish(&sb);
}

static void lists_reset(void) {
    for (int i = 0; i < TAB_COUNT; i++) {
        free(M.data[i].html);
        cJSON_Delete(M.data[i].list);
    }
    memset(M.data, 0, sizeof M.data);
    M.gen++;
}

// ------------------------------------------------------------------ Upload Mod (window 19)

static struct {
    char zip[1024], thumb[1024], zip_name[256];
    double zip_size, thumb_size;
    int inflight;
    char resp[256];
    int resp_err;
    // the hub's Publish / Update Mod popup (mods_ui_hub_publish): done != NULL; name / thumbnail of the mod updated
    void (*done)(void);
    int update;
    char prefill[160], thumb_src[600];
    // the upload: the files in order (mod, then thumb), chunk by chunk
    unsigned gen;
    char base[512], file_id[40];
    unsigned char *data;
    size_t len, sent, at, n;
    int part, retries;
} P;

static int level_ok(const AccountInfo *a) { return a && a->level >= 10; }
static int thumb_kb(const AccountInfo *a) { return a && a->premium ? 500 : 250; }
static int zip_mb(const AccountInfo *a) { return a && a->premium ? 50 : 25; }

static void set_resp(const char *text, const int err) {
    snprintf(P.resp, sizeof P.resp, "%s", text ? text : "");
    P.resp_err = err;
    if (P.done) { // the hub's popup: uploadResp (hub-10.0.7.js:6806-6813) shows #modResHolder and writes the text
        HtmlElem holder = html_dom_by_id("modResHolder");
        if (holder) html_dom_set_style(holder, "display", "");
    }
    HtmlElem el = html_dom_by_id("modUpResp");
    if (!el) return;
    if (!P.done) html_dom_set_style(el, "color", err ? "#e33b3b" : "rgba(255,255,255,0.7)");
    html_dom_set_text(el, P.resp);
}

// The hub's Publish / Update Mod popup, v10.0.7's markup (publishModPopup, hub-10.0.7.js:6734-6754): the thumbnail on
// the left (Select, its KB limit), the .zip on the right (Select, its MB limit), the name, the response line and the
// Publish / Update button. selectFile opens the platform file dialog (mods_ui_handle), uploadMod is publish().
static char *thumb_data_url(const char *path);

static char *hub_publish_form(void) {
    const AccountInfo *acc = active_account();
    if (!acc) return NULL;
    const int kb = thumb_kb(acc), mb = zip_mb(acc);
    StrBuf sb = {0};
    sb_append(&sb, "<div id='publishMod' style='display:block'><div class='publishLeft' id='modThumbH'><img id='modThumbPrev' src='");
    esc(&sb, P.thumb_src[0] ? P.thumb_src : "./img/noimg.png");
    sb_appendf(&sb, "' onerror='this.onerror=null;this.src=\"./img/noimg.png\";' style='width: 100%%;height: 100%%;'><div id='modThumbSize'>0 / %d "
                    "<span style='color:rgba(255, 255, 255, 0.4)!important;'>KB</span></div><input onclick='selectFile(&quot;modThumb&quot;)' "
                    "type='button' id='modThumbSelect' value='Select'><div class='changeThumb'>Select Thumbnail</div><input id='modThumb' "
                    "type='file' accept='image/png' style='width:1px;visibility:hidden;' onchange='updateFile(this.id,0,0,%d)'></div>",
               kb, kb);
    sb_appendf(&sb, "<div class='publishRight'><div style='color:rgba(255, 255, 255, 0.6)!important;font-size:18px;margin-bottom: 5px;' id='modZipH'>"
                    "<div style='color:rgba(255, 255, 255, 0.6)!important;font-size:16px'>Mod File (.zip):<input onclick='selectFile(&quot;modZip&quot;)' "
                    "type='button' id='modZipSelect' class='fileSelect' value='Select'><input id='modZip' type='file' accept='.zip' "
                    "style='width:1px;visibility:hidden;' onchange='updateFile(this.id, 1, true,%d)'><span style='color:rgba(255, 255, 255, 0.6)!important;"
                    "font-size: 14px;float:right;margin-top: 5px;' id='modZipSize'>0 / %d <span style='color:rgba(255, 255, 255, 0.4)!important;'>MB</span>"
                    "</span></div></div></div>",
               mb, mb);
    sb_append(&sb, "<input id='modNameNew' type='text' maxlength='500' class='mapInput' ");
    if (P.update && P.prefill[0]) {
        sb_append(&sb, "value='");
        esc(&sb, P.prefill);
        sb_append(&sb, "'");
    } else {
        sb_append(&sb, "placeholder='Name'");
    }
    sb_appendf(&sb, " autocomplete='off' autocorrect='off' autocapitalize='off' spellcheck='false'></div><div id='modResHolder' "
                    "style='margin-top:8px;margin-bottom:5px;display:%s'><div id='modUpResp' style='color:rgba(0,0,0,0.3);display: inline-block;'>",
               P.resp[0] ? "block" : "none");
    esc(&sb, P.resp);
    sb_appendf(&sb, "</div></div><div id='pubGame' style='margin-top:10px;width: calc(100%% - 11px);' onclick='uploadMod()'>%s</div>",
               P.update ? "Update" : "Publish");
    return sb_finish(&sb);
}

// a picked file's size line: the window's onFileChange (game.deob.js:220986-220990) and the hub's updateFile
// (hub-10.0.7.js:6796-6800): round(size / unit, 2) / limit and the unit, the hub's span with !important
static void size_line(StrBuf *sb, const double size, const int mb, const int limit, const int hub) {
    sb_appendf(sb, "%g / %d <span style='color:%s'>%s</span>", round(size / (mb ? 1e6 : 1e3) * 100.0) / 100.0, limit,
               hub ? "rgba(255, 255, 255, 0.4)!important;" : "rgba(255,255,255,0.4)", mb ? "MB" : "KB");
}

static const char *base_name(const char *path) {
    const char *b = path ? strrchr(path, '/') : NULL;
    return b ? b + 1 : path ? path : "";
}

// Upload Mod (window 19), v10.0.7's markup (mods_pub gen, game.deob.js:220949-220962); a file already picked shows
// as onFileChange left it (its name in #<id>H's title, its size line, the thumbnail's preview)
static char *publish_form(void) {
    const AccountInfo *acc = active_account();
    if (!acc) return NULL; // signed out: the static 19.html (log in first)
    const int lvl = level_ok(acc), kb = thumb_kb(acc), mb = zip_mb(acc);
    StrBuf sb = {0};
    sb_append(&sb, "<div id='modPubBody' style='padding:10px 5px 5px 5px'><div style='color:rgba(255,255,255,0.6);font-size:15px;margin-bottom:5px'>"
                   "Thumbnail (PNG)");
    if (lvl) {
        sb_append(&sb, " <span style='float:right' id='modThumbSize'>");
        if (P.thumb[0]) size_line(&sb, P.thumb_size, 0, kb, 0);
        else sb_appendf(&sb, "0 / %d <span style='color:rgba(255,255,255,0.4)'>KB</span>", kb);
        sb_append(&sb, "</span>");
    } else {
        sb_append(&sb, " <span style='float:right;color:rgba(255,255,255,0.4);font-size:13px' title='Reach level 10 to attach a thumbnail'>"
                       "Level 10 required</span>");
    }
    sb_append(&sb, "</div><div id='modThumbH' ");
    if (P.thumb[0]) {
        sb_append(&sb, "title='");
        esc(&sb, base_name(P.thumb));
        sb_append(&sb, "' ");
    }
    sb_append(&sb, "style='position:relative;display:flex;align-items:center;gap:10px;padding:10px;border-radius:4px;"
                   "background:rgba(255,255,255,0.12);margin-bottom:15px'><img id='modThumbPrev' src='");
    char *thumb_url = P.thumb[0] ? thumb_data_url(P.thumb) : NULL;
    sb_append(&sb, thumb_url ? thumb_url : "./img/noimg.png");
    free(thumb_url);
    sb_appendf(&sb, "' onerror='this.onerror=null;this.src=\"./img/noimg.png\";' style='width:80px;height:80px;object-fit:cover;border-radius:4px;"
                    "background:rgba(0,0,0,0.3)'><input id='modThumb' type='file' accept='image/png' style='width:1px;visibility:hidden;"
                    "position:absolute' onchange=\"windows[18].onFileChange('modThumb', false, %d)\"%s><a href='javascript:;' class='menuLink' "
                    "style='margin:0%s' %s>%s</a></div>",
               kb, lvl ? "" : " disabled", lvl ? "" : ";pointer-events:none;opacity:0.4",
               lvl ? "onclick='selectFile(&quot;modThumb&quot;)'" : "onclick='return false;'", lvl ? "Select Thumbnail" : "Locked");
    sb_append(&sb, "<div style='color:rgba(255,255,255,0.6);font-size:15px;margin-bottom:5px'>Mod File (.zip) <span style='float:right' id='modZipSize'>");
    if (P.zip[0]) size_line(&sb, P.zip_size, 1, mb, 0);
    else sb_appendf(&sb, "0 / %d <span style='color:rgba(255,255,255,0.4)'>MB</span>", mb);
    sb_append(&sb, "</span></div><div id='modZipH' ");
    if (P.zip[0]) {
        sb_append(&sb, "title='");
        esc(&sb, P.zip_name);
        sb_append(&sb, "' ");
    }
    sb_appendf(&sb, "style='display:flex;align-items:center;gap:10px;padding:10px;border-radius:4px;background:rgba(255,255,255,0.12);"
                    "margin-bottom:15px'><span class='material-icons' style='color:rgba(255,255,255,0.5);font-size:32px'>folder_zip</span>"
                    "<input id='modZip' type='file' accept='.zip' style='width:1px;visibility:hidden;position:absolute' "
                    "onchange=\"windows[18].onFileChange('modZip', true, %d)\"><a href='javascript:;' class='menuLink' style='margin:0' "
                    "onclick='selectFile(&quot;modZip&quot;)'>Select Mod Zip</a></div>",
               mb);
    sb_append(&sb, "<input id='modNameNew' type='text' maxlength='500' class='accountInput' style='margin-top:0;margin-bottom:15px' "
                   "placeholder='Mod Name' autocomplete='off' autocorrect='off' autocapitalize='off' spellcheck='false'>");
    sb_appendf(&sb, "<div id='modUpResp' style='min-height:20px;color:%s;font-size:14px;text-align:center;margin-bottom:10px'>",
               P.resp[0] && !P.resp_err ? "rgba(255,255,255,0.7)" : "#e33b3b");
    esc(&sb, P.resp);
    sb_append(&sb, "</div><a id='modPubSubmit' href='javascript:;' class='menuLink' style='display:block;text-align:center;padding:12px;"
                   "font-size:20px;background:rgba(255,255,255,0.12);border-radius:4px' onclick='windows[18].submit()'>Publish</a></div>");
    return sb_finish(&sb);
}

static unsigned char *read_whole(const char *path, size_t *len);

// FileReader.readAsDataURL(file) of the picked thumbnail (the window's onFileChange, game.deob.js:220973-220980; the
// hub's updateFile, hub-10.0.7.js:6786-6792): the preview shows the picked file through a data: URL (the page
// loads no file: URL), malloc'd; NULL when the file cannot be read
static char *thumb_data_url(const char *path) {
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    static const char head[] = "data:image/png;base64,";
    size_t n = 0;
    unsigned char *d = path && *path ? read_whole(path, &n) : NULL;
    if (!d) return NULL;
    char *out = malloc(sizeof(head) + 4 * ((n + 2) / 3) + 1);
    if (!out) {
        free(d);
        return NULL;
    }
    memcpy(out, head, sizeof(head) - 1);
    size_t o = sizeof(head) - 1;
    for (size_t i = 0; i < n; i += 3) {
        const uint32_t v = (uint32_t) d[i] << 16 | (i + 1 < n ? (uint32_t) d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
        out[o++] = T[v >> 18 & 63];
        out[o++] = T[v >> 12 & 63];
        out[o++] = i + 1 < n ? T[v >> 6 & 63] : '=';
        out[o++] = i + 2 < n ? T[v & 63] : '=';
    }
    out[o] = 0;
    free(d);
    return out;
}

static char *publish_gen(void) {
    if (P.done && !P.inflight) { // the menu's window, not the hub's popup
        P.done = NULL;
        P.update = 0;
        P.prefill[0] = P.thumb_src[0] = 0;
    }
    return publish_form();
}

static void forget_files(void) {
    memset(P.zip, 0, sizeof P.zip);
    memset(P.thumb, 0, sizeof P.thumb);
    P.zip_size = P.thumb_size = 0;
    P.zip_name[0] = P.resp[0] = 0;
    P.resp_err = 0;
}

// a picked file, in place (the page's file inputs do nothing natively): the hub's updateFile (hub-10.0.7.js:6779-6802)
// and the window's onFileChange (game.deob.js:220976-220991). #<id>H's title is the file's name and the thumbnail's
// preview shows it; the hub's #<id>Select reads Change and only its .zip gets a size line (its thumbnail passes no
// size flag), the window writes both
static void picked_in_place(const int thumb, const char *path) {
    const AccountInfo *acc = active_account();
    const int hub = P.done != NULL;
    HtmlElem e;
    if ((e = html_dom_by_id(thumb ? "modThumbH" : "modZipH"))) html_dom_set_attr(e, "title", base_name(path));
    if (hub && (e = html_dom_by_id(thumb ? "modThumbSelect" : "modZipSelect"))) html_dom_set_value(e, "Change");
    if (!thumb || !hub) {
        StrBuf sb = {0};
        size_line(&sb, thumb ? P.thumb_size : P.zip_size, !thumb, thumb ? thumb_kb(acc) : zip_mb(acc), hub);
        char *t = sb_finish(&sb);
        if (t && (e = html_dom_by_id(thumb ? "modThumbSize" : "modZipSize"))) html_dom_set_html(e, t);
        free(t);
    }
    if (thumb && (e = html_dom_by_id("modThumbPrev"))) {
        char *url = thumb_data_url(path);
        if (url) html_dom_set_attr(e, "src", url);
        free(url);
    }
}

char *mods_ui_hub_publish(const char *name, const char *mod_id, const int thumb, void (*done)(void)) {
    if (!P.inflight) forget_files();
    P.done = done;
    P.update = name && *name;
    snprintf(P.prefill, sizeof P.prefill, "%s", P.update ? name : "");
    if (P.update && thumb && mod_id && *mod_id) snprintf(P.thumb_src, sizeof P.thumb_src, "%s/md%s/thumb.png", thumb_host(), mod_id);
    else P.thumb_src[0] = 0;
    return hub_publish_form();
}

static void picked_publish(void *user, const char *path) {
    const int thumb = user != NULL;
    struct stat st;
    if (!path || stat(path, &st) != 0 || st.st_size <= 0) {
        set_resp("That file cannot be read", 1);
        return;
    }
    if (thumb) {
        snprintf(P.thumb, sizeof P.thumb, "%s", path);
        P.thumb_size = (double) st.st_size;
    } else {
        snprintf(P.zip, sizeof P.zip, "%s", path);
        P.zip_size = (double) st.st_size;
        const char *b = strrchr(path, '/');
        snprintf(P.zip_name, sizeof P.zip_name, "%s", b ? b + 1 : path);
    }
    picked_in_place(thumb, path);
}

static void up_end(const char *error) {
    free(P.data);
    P.data = NULL;
    P.inflight = 0;
    P.gen++;
    if (error) set_resp(error, 1);
}

static void up_next(void);

static void up_retry(void *user) {
    if ((unsigned) (uintptr_t) user != P.gen || !P.inflight) return;
    P.sent = P.at; // the chunk again
    up_next();
}

static void up_answer(void *user, const int status, const char *body, const size_t length) {
    (void) length;
    if ((unsigned) (uintptr_t) user != P.gen || !P.inflight) return;
    if (status <= 0 && P.retries < CHUNK_RETRIES) { // no answer at all: the brain may be restarting
        P.retries++;
        html_timer_add(CHUNK_RETRY_MS, up_retry, (void *) (uintptr_t) P.gen);
        return;
    }
    P.retries = 0;
    char err[256];
    if (status != 200) {
        answer_error(body, err, sizeof err, error_text("MODS_UPLOAD_STORAGE_FAIL", UPLOAD_FAILED));
        up_end(err);
        return;
    }
    if (P.part >= 2) { // done answered
        cJSON *j = body ? cJSON_Parse(body) : NULL;
        const double id = cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(j, "data"), "mod_id"));
        cJSON_Delete(j);
        if (!(id > 0)) {
            up_end(error_text("MODS_UPLOAD_STORAGE_FAIL", UPLOAD_FAILED));
            return;
        }
        up_end(NULL);
        forget_files();
        lists_reset();
        if (P.done) { // the hub's popup
            void (*fn)(void) = P.done;
            P.done = NULL;
            P.update = 0;
            P.prefill[0] = P.thumb_src[0] = 0;
            fn();
            return;
        }
        menu_close_window();
        popup(1, "Mod Published!");
        M.tab = TAB_MINE; // the Mod Manager opens on My Mods next
        return;
    }
    if (P.sent >= P.len) {
        free(P.data);
        P.data = NULL;
        P.part++;
    }
    up_next();
}

static unsigned char *read_whole(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    unsigned char *d = NULL;
    if (!fseek(f, 0, SEEK_END)) {
        const long n = ftell(f);
        if (n > 0 && !fseek(f, 0, SEEK_SET) && (d = malloc((size_t) n)) && fread(d, 1, (size_t) n, f) != (size_t) n) {
            free(d);
            d = NULL;
        }
        if (d) *len = (size_t) n;
    }
    fclose(f);
    return d;
}

static void up_next(void) {
    char url[1200];
    const void *user = (const void *) (uintptr_t) P.gen;
    if (P.part == 1 && !P.thumb[0]) P.part = 2;
    if (P.part >= 2) {
        snprintf(url, sizeof url, "%s?fileId=%s&done=1", P.base, P.file_id);
        if (!backend_http_post_bytes(url, "application/octet-stream", "", 0, CHUNK_TIMEOUT_MS, up_answer, (void *) user))
            up_end(error_text("MODS_UPLOAD_STORAGE_FAIL", UPLOAD_FAILED));
        return;
    }
    if (!P.data) {
        P.sent = P.len = 0;
        P.data = read_whole(P.part == 0 ? P.zip : P.thumb, &P.len);
        if (!P.data || (double) P.len != (P.part == 0 ? P.zip_size : P.thumb_size)) {
            up_end(error_text("MODS_UPLOAD_SIZE_MISMATCH", UPLOAD_FAILED));
            return;
        }
    }
    P.at = P.sent;
    P.n = P.len - P.sent < CHUNK ? P.len - P.sent : CHUNK;
    snprintf(url, sizeof url, "%s?fileId=%s&key=%s&offset=%zu", P.base, P.file_id, P.part == 0 ? "mod" : "thumb", P.at);
    P.sent += P.n;
    if (!backend_http_post_bytes(url, "application/octet-stream", P.data + P.at, P.n, CHUNK_TIMEOUT_MS, up_answer, (void *) user))
        up_end(error_text("MODS_UPLOAD_STORAGE_FAIL", UPLOAD_FAILED));
}

static void on_key(void *user, const int status, const char *body, const size_t length) {
    (void) length;
    if ((unsigned) (uintptr_t) user != P.gen || !P.inflight) return;
    char err[256];
    cJSON *j = status == 200 && body ? cJSON_Parse(body) : NULL;
    const cJSON *d = cJSON_GetObjectItemCaseSensitive(j, "data");
    const char *fid = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(d, "fileId"));
    const char *up = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(d, "uploadUrl"));
    if (!fid || !up || strlen(fid) >= sizeof P.file_id) {
        cJSON_Delete(j);
        answer_error(body, err, sizeof err, error_text("MODS_UPLOAD_STORAGE_FAIL", UPLOAD_FAILED));
        up_end(err);
        return;
    }
    snprintf(P.file_id, sizeof P.file_id, "%s", fid);
    if (up[0] == '/') snprintf(P.base, sizeof P.base, "%s%s", backend_url(), up);
    else snprintf(P.base, sizeof P.base, "%s", up);
    cJSON_Delete(j);
    set_resp("Uploading...", 0);
    P.part = 0;
    P.retries = 0;
    up_next();
}

static void publish(void) {
    if (P.inflight) return;
    const AccountInfo *acc = active_account();
    if (!acc) return;
    char name[256] = "";
    HtmlElem in = html_dom_by_id("modNameNew");
    if (in) html_dom_get_value(in, name, sizeof name);
    char *s = name;
    while (*s == ' ') s++;
    for (size_t n = strlen(s); n && s[n - 1] == ' ';) s[--n] = 0;
    // the window's submit (game.deob.js:220993-221012); the hub's uploadMod (hub-10.0.7.js:6755-6773) says Loading...
    // and checks only the .zip's size; kn_brain refuses the rest (a thumbnail's size and level among them)
    if (P.done) set_resp("Loading...", 0);
    if (!*s) { set_resp("Mod name is required", 1); return; }
    if (!P.zip[0]) { set_resp("Select a .zip mod file", 1); return; }
    if (P.zip_size / 1e6 > zip_mb(acc)) {
        char t[64];
        if (P.done) snprintf(t, sizeof t, "Mod.zip size Limit Reached");
        else snprintf(t, sizeof t, "Mod .zip exceeds %d MB limit", zip_mb(acc));
        set_resp(t, 1);
        return;
    }
    if (!P.done && P.thumb[0] && !level_ok(acc)) { set_resp("Thumbnails require level 10+", 1); return; }
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "name", s);
    cJSON *sizes = cJSON_AddObjectToObject(body, "assetSizes");
    cJSON_AddNumberToObject(sizes, "mod", P.zip_size);
    if (P.thumb[0]) cJSON_AddNumberToObject(sizes, "thumb", P.thumb_size);
    char *json = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    char token[400], url[1024];
    backend_url_encode(token, sizeof token, account_session_token());
    snprintf(url, sizeof url, "%s/mods/publish/key?token=%s", backend_url(), token);
    P.gen++;
    P.inflight = 1;
    if (!P.done) set_resp("Requesting upload key...", 0);
    if (!backend_http_request("POST", url, json ? json : "{}", on_key, (void *) (uintptr_t) P.gen))
        up_end(error_text("MODS_UPLOAD_STORAGE_FAIL", UPLOAD_FAILED));
    free(json);
}

// ------------------------------------------------------------------ ?mod=<name>

// the page's ?mod=<name> (deob-10.0.7.js:226181-226192): the mod of exactly that name, loaded as a server mod
// (loadModPack(url, true, true, ...): not kept as the last mod); none found says nothing
static void on_by_name(void *user, const int status, const char *body, const size_t length) {
    (void) user;
    (void) length;
    cJSON *j = status == 200 && body ? cJSON_Parse(body) : NULL;
    const cJSON *d = cJSON_GetObjectItemCaseSensitive(j, "data");
    char id[24];
    jid(d, id, sizeof id);
    const char *name = jstr(d, "mod_name", "name");
    if (d && id[0]) load_user_mod(name, jstr(d, "mod_url", NULL), id, jstr(d, "creatorname", "creator_name"), info_thumb(d), 1);
    cJSON_Delete(j);
}

void mods_ui_query(const char *query) {
    const char *p = query ? strstr(query, "mod=") : NULL;
    if (!p || (p != query && p[-1] != '?' && p[-1] != '&')) return;
    p += 4;
    char raw[200];
    size_t n = strcspn(p, "&#");
    if (!n || n >= sizeof raw) return;
    memcpy(raw, p, n);
    raw[n] = 0;
    char path[260];
    snprintf(path, sizeof path, "/mods/by-name/%s", raw); // still percent-encoded, as the route takes it
    backend_http_get(path, on_by_name, NULL);
}

// favoriteM(1, this, id) (deob-10.0.7.js:228231-228245), signed in: the star's sel class (set = not a favourite)
// flips and its icon follows, FAVORITE_MAPMOD(1, id, wasSel) goes out (kn_brain's fvM, creations.c; the game socket
// for a krunker.io account), the favourite set takes the toggle, and the Favorites tab's list is dropped unless a page
// of it is loading
void mods_ui_favorite(const char *id, HtmlElem star) {
    const long long mid = id ? strtoll(id, NULL, 10) : 0;
    if (!active_account()) return;
    char got[32] = "";
    if (!star || html_dom_get_attr(star, "data-favid", got, sizeof got) < 0 || strcmp(got, id ? id : "") != 0) {
        // `this` did not come through as the star (an inner node or none): the card's star by its id
        char sel[96];
        snprintf(sel, sizeof sel, ".mapActionB[data-fav='1'][data-favid='%lld']", mid);
        star = mid > 0 ? html_dom_query(0, sel) : 0;
    }
    const int was_sel = star ? html_dom_has_class(star, "sel") : fav_known(id) != 1;
    if (star) {
        html_dom_set_class(star, "sel", !was_sel);
        html_dom_set_html(star, was_sel ? "<span class='material-icons' style='font-size:50px;color:#fff;'>star</span>"
                                        : "<span class='material-icons' style='font-size:50px;color:#fff;'>star_border</span>");
    }
    char frame[96];
    snprintf(frame, sizeof frame, "[\"fvM\",1,%lld,%s]", mid, was_sel ? "true" : "false"); // v10's order: kind, id, flag
    if (account_backend() != ACCOUNT_BACKEND_KRUNKER) backend_social_send_text(frame);
    else {
        cJSON *f = cJSON_Parse(frame);
        krunker_net_send_account_frame(f);
        cJSON_Delete(f);
    }
    if (mid > 0 && F.acc) fav_put(mid, was_sel);
    if (!M.data[TAB_FAV].waiting) {
        free(M.data[TAB_FAV].html);
        cJSON_Delete(M.data[TAB_FAV].list);
        memset(&M.data[TAB_FAV], 0, sizeof M.data[TAB_FAV]);
    }
}

// iíïííiì("SUCCESS", null, lines) (deob-10.0.7.js:222910-222916): the green tick over the lines (HTML, joined by <br>)
static void success_popup_html(const char *html) {
    static const char *const pops[] = {"popupContent", "confPremiumPop", "gameCodePop", "tradePop", "twitchPop", "pfpPop", "dropsPop", "confPop"};
    HtmlElem e = html_dom_by_id("popupHolder");
    if (e) html_dom_set_display(e, "block");
    for (size_t i = 0; i < sizeof pops / sizeof pops[0]; i++)
        if ((e = html_dom_by_id(pops[i]))) html_dom_set_display(e, "none");
    HtmlElem content = html_dom_by_id("popupContent");
    if (!content) return;
    html_dom_set_style(content, "width", "300px");
    html_dom_set_display(content, "block");
    StrBuf sb = {0};
    sb_appendf(&sb, "<div style='width:100%%;text-align:center'><div><i class='material-icons' style='font-size:80px;color:#3be354'>done</i></div>"
                    "<div style='font-size:20px;color:rgba(255,255,255,0.5)'>%s</div></div>", html);
    char *h = sb_finish(&sb);
    html_dom_set_html(content, h ? h : "");
    free(h);
}

// windows[3].reset(quiet) (deob-10.0.7.js:218818-218831): the loaded list empties, the pack and its passes, sounds,
// css and logo go, #modVote hides; the Reset Mods link redraws the window and says so in a SUCCESS popup; Ctrl + /
// also forgets the last mod and says "Mods Cleared" in the chat
static void mods_reset(const int quiet) {
    M.nloaded = 0;
    mod_pack_reset();
    creation_vote_set_mod(0, NULL, NULL);
    if (quiet) {
        hud_ui_add_chat_line_on("Mods Cleared", NULL, -1);
        return;
    }
    menu_rerender_if_open(MODS_WINDOW);
    char text[256];
    snprintf(text, sizeof text, "%s<br><small>%s</small>", I18N("mod.reset.0", "Will take effect after respawning."),
             I18N("mod.reset.1", "Map Objects dont get reset"));
    success_popup_html(text);
}

// Ctrl + / (deob-10.0.7.js:209630-209632): setLastMod('') and windows[3].reset(true)
void mods_ui_clear_all(void) {
    html_storage_set("krk_lastMod", "");
    mods_reset(1);
}

// ------------------------------------------------------------------ the mod view (openModDetail, svelte-1v2j2f2)

// window.openModDetail(name) (deob-10.0.7.js:222799-222803) pushes the mod view (:108361-108478): the header (back,
// the extension icon, the name, the share button once loaded), then the hourglass while getModByName runs, its error,
// or the body: the thumbnail with v<version> and the votes, "by <creator>" (the profile), Featured, Use (/?mod=<name>),
// Report, Feature / Delete for staff, Created / Updated and the description
static struct {
    int open, loading, copied;
    unsigned gen;
    char name[160], error[200];
    cJSON *mod;
} V;

static void view_render(void);

static void view_closed(const int accepted, void *user) {
    (void) accepted;
    (void) user;
    V.open = 0;
    V.gen++;
    cJSON_Delete(V.mod);
    V.mod = NULL;
}

// iïìîîíì(date) (deob-10.0.7.js:23916-23923): "D Mon YYYY", "None" without a date
static void view_date(StrBuf *sb, const char *iso) {
    static const char *const mon[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    int y, mo, d;
    if (!iso || !*iso || sscanf(iso, "%d-%d-%d", &y, &mo, &d) != 3 || mo < 1 || mo > 12) {
        sb_append(sb, "None");
        return;
    }
    sb_appendf(sb, "%d %s %d", d, mon[mo - 1], y);
}

static char *view_html(void) {
    StrBuf sb = {0};
    const AccountInfo *acc = active_account();
    sb_append(&sb, "<div class=\"mod-root svelte-1v2j2f2\"><div class=\"mod-header svelte-1v2j2f2\"><button class=\"mod-back svelte-1v2j2f2\" "
                   "type=\"button\" aria-label=\"Back\" onclick=\"knModView('back')\"><i class=\"material-icons svelte-1v2j2f2\">arrow_back</i>"
                   "</button> <i class=\"material-icons mod-header-icon svelte-1v2j2f2\">extension</i> <span class=\"mod-header-title svelte-1v2j2f2\">");
    const char *name = V.mod ? jstr(V.mod, "name", "mod_name") : "";
    esc(&sb, *name ? name : V.name);
    sb_append(&sb, "</span> <div class=\"mod-header-actions svelte-1v2j2f2\">");
    if (V.mod) // the share button (svelte-1524ito): origin + /?mod=<name>, "Copied!" for 1.5 s
        sb_appendf(&sb, "<button class=\"share-btn svelte-1524ito%s\" type=\"button\" title=\"%s\" onclick=\"knModView('share')\">"
                        "<i class=\"material-icons svelte-1524ito\">%s</i> <span class=\"svelte-1524ito\">%s</span></button>",
                   V.copied ? " share-copied" : "", V.copied ? "Copied!" : "Copy link", V.copied ? "check" : "share", V.copied ? "Copied!" : "Share");
    sb_append(&sb, "</div></div> ");
    if (V.loading) {
        sb_append(&sb, "<div class=\"mod-center svelte-1v2j2f2\"><i class=\"material-icons spin svelte-1v2j2f2\">hourglass_empty</i></div>");
    } else if (V.error[0]) {
        sb_append(&sb, "<div class=\"mod-center svelte-1v2j2f2\" style=\"color:#f44336\">");
        esc(&sb, V.error);
        sb_append(&sb, "</div>");
    } else if (V.mod) {
        char id[24], votes[48];
        jid(V.mod, id, sizeof id);
        format_commas(votes, sizeof votes, jnum(V.mod, "votes", "mod_votes"));
        const cJSON *ver = cJSON_GetObjectItemCaseSensitive(V.mod, "version");
        const int featured = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(V.mod, "featured")) || jnum(V.mod, "featured", NULL) != 0;
        sb_appendf(&sb, "<div class=\"mod-body svelte-1v2j2f2\"><div class=\"mod-hero svelte-1v2j2f2\"><img class=\"mod-thumb svelte-1v2j2f2\" "
                        "src=\"%s/md%s/thumb.png\" alt=\"", thumb_host(), id);
        esc(&sb, name);
        sb_append(&sb, "\" onerror=\"this.onerror=null;this.src='./img/noimg.png';\"> <div class=\"mod-hero-overlay svelte-1v2j2f2\">"
                       "<span class=\"mod-version svelte-1v2j2f2\">v");
        if (cJSON_IsString(ver) && ver->valuestring[0]) esc(&sb, ver->valuestring);
        else if (cJSON_IsNumber(ver) && ver->valuedouble != 0) sb_appendf(&sb, "%g", ver->valuedouble);
        else sb_append(&sb, "0.1");
        sb_appendf(&sb, "</span> <span class=\"mod-votes svelte-1v2j2f2\"><i class=\"material-icons svelte-1v2j2f2\">thumb_up</i> %s</span></div></div> "
                        "<div class=\"mod-info-bar svelte-1v2j2f2\"><div class=\"mod-info-left svelte-1v2j2f2\"><button class=\"mod-creator svelte-1v2j2f2\" "
                        "type=\"button\" onclick=\"knModView('creator')\">by ", votes);
        esc(&sb, jstr(V.mod, "creator_name", "creatorname"));
        sb_append(&sb, "</button> ");
        if (featured)
            sb_append(&sb, "<span class=\"mod-featured svelte-1v2j2f2\"><i class=\"material-icons svelte-1v2j2f2\">check_circle</i> Featured</span>");
        sb_append(&sb, "</div> <div class=\"mod-actions svelte-1v2j2f2\"><button class=\"mod-use-btn svelte-1v2j2f2\" type=\"button\" "
                       "onclick=\"knModView('use')\"><i class=\"material-icons svelte-1v2j2f2\">play_arrow</i> Use</button> <button "
                       "class=\"mod-icon-btn svelte-1v2j2f2\" type=\"button\" title=\"Report\" onclick=\"knModView('report')\"><i "
                       "class=\"material-icons svelte-1v2j2f2\">flag</i></button> ");
        if (acc && acc->is_dev)
            sb_appendf(&sb, "<button class=\"mod-icon-btn svelte-1v2j2f2\" type=\"button\" title=\"%s\" onclick=\"knModView('feature')\">"
                            "<i class=\"material-icons svelte-1v2j2f2\" style=\"color:%s\">check_circle</i></button>",
                       featured ? "Unfeature" : "Feature", featured ? "#fbc02d" : "#ccc");
        sb_append(&sb, " ");
        if (acc && acc->is_dev)
            sb_append(&sb, "<button class=\"mod-icon-btn mod-del svelte-1v2j2f2\" type=\"button\" title=\"Delete\" onclick=\"knModView('delete')\">"
                           "<i class=\"material-icons svelte-1v2j2f2\">delete_forever</i></button>");
        sb_append(&sb, "</div></div> <div class=\"mod-content svelte-1v2j2f2\"><div class=\"mod-dates svelte-1v2j2f2\"><span>Created: ");
        view_date(&sb, jstr(V.mod, "created_at", NULL));
        sb_append(&sb, "</span> <span>Updated: ");
        view_date(&sb, jstr(V.mod, "updated_at", NULL));
        sb_append(&sb, "</span></div> <div class=\"mod-desc svelte-1v2j2f2\">");
        const char *desc = jstr(V.mod, "description", "mod_description");
        esc(&sb, *desc ? desc : "No Description");
        sb_append(&sb, "</div></div></div>");
    }
    sb_append(&sb, "</div>");
    return sb_finish(&sb);
}

static void view_render(void) {
    if (V.open) v10_view_set(view_html());
}

static void on_mod_view(void *user, const int status, const char *body, const size_t length) {
    (void) length;
    if ((unsigned) (uintptr_t) user != V.gen || !V.open) return;
    cJSON *j = body ? cJSON_Parse(body) : NULL;
    const cJSON *err = cJSON_GetObjectItemCaseSensitive(j, "error");
    const cJSON *d = cJSON_GetObjectItemCaseSensitive(j, "data");
    V.loading = 0;
    if (err && !cJSON_IsNull(err)) { // throw new Error(error.message || error)
        const char *m = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(err, "message"));
        snprintf(V.error, sizeof V.error, "%s", m && *m ? m : cJSON_IsString(err) && err->valuestring[0] ? err->valuestring : "Failed to load mod");
    } else if (status != 200 || !cJSON_IsObject(d)) {
        snprintf(V.error, sizeof V.error, "Failed to load mod");
    } else {
        cJSON_Delete(V.mod);
        V.mod = cJSON_Duplicate(d, 1);
    }
    cJSON_Delete(j);
    view_render();
}

static void mod_view_open(const char *name) {
    if (!name || !*name) return; // iîIiîïï && push
    if (V.open) v10_view_pop(0);
    memset(V.error, 0, sizeof V.error);
    snprintf(V.name, sizeof V.name, "%s", name);
    V.open = 1;
    V.loading = 1;
    V.copied = 0;
    V.gen++;
    v10_view_push(view_html(), view_render, view_closed, NULL);
    char enc[480], path[520];
    backend_url_encode(enc, sizeof enc, name);
    snprintf(path, sizeof path, "/mods/by-name/%s", enc);
    if (!backend_http_get(path, on_mod_view, (void *) (uintptr_t) V.gen)) {
        V.loading = 0;
        snprintf(V.error, sizeof V.error, "Failed to load mod");
        view_render();
    }
}

static void view_uncopy(void *user) {
    if ((unsigned) (uintptr_t) user != V.gen || !V.copied) return;
    V.copied = 0;
    view_render();
}

static void view_reported(const int accepted, const char *text, void *user) {
    (void) user;
    if (!accepted || !text || !V.mod) return;
    char reason[300];
    snprintf(reason, sizeof reason, "%s", text);
    char *s = reason;
    while (*s == ' ' || *s == '\t' || *s == '\n') s++;
    for (size_t n = strlen(s); n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\n');) s[--n] = 0;
    if (strlen(s) < 10) return; // min 10 chars
    char info[480];
    snprintf(info, sizeof info, "Mod: %s - %s", jstr(V.mod, "name", "mod_name"), s);
    cJSON *f = cJSON_CreateArray(); // players.reportPlayer(creator, 0, info): kn_brain's rpt
    cJSON_AddItemToArray(f, cJSON_CreateString("rpt"));
    cJSON_AddItemToArray(f, cJSON_CreateString(jstr(V.mod, "creator_name", "creatorname")));
    cJSON_AddItemToArray(f, cJSON_CreateNumber(0));
    cJSON_AddItemToArray(f, cJSON_CreateString(info));
    const int sent = backend_social_send(f);
    cJSON_Delete(f);
    ui_alert(sent ? "Report submitted." : "Failed to submit report");
}

static void view_feature_done(void *user, const int status, const cJSON *json) {
    if ((unsigned) (uintptr_t) user != V.gen || !V.mod || status != 200) return;
    const cJSON *f = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(json, "data"), "featured");
    cJSON_DeleteItemFromObjectCaseSensitive(V.mod, "featured");
    cJSON_AddBoolToObject(V.mod, "featured", cJSON_IsTrue(f) || (cJSON_IsNumber(f) && f->valuedouble != 0));
    view_render();
}

static void view_delete_done(void *user, const int status, const cJSON *json) {
    (void) json;
    if ((unsigned) (uintptr_t) user != V.gen || status != 200) return;
    v10_view_pop(0);
}

static void view_delete_confirmed(const int accepted, const char *text, void *user) {
    (void) text;
    if (!accepted || (unsigned) (uintptr_t) user != V.gen || !V.mod) return;
    char id[24], path[120], tok[400];
    jid(V.mod, id, sizeof id);
    v10_url_token(tok, sizeof tok);
    snprintf(path, sizeof path, "/mods/%s?token=%s", id, tok);
    v10_send("DELETE", path, NULL, view_delete_done, (void *) (uintptr_t) V.gen);
}

static void mod_view_action(const char *what) {
    if (!strcmp(what, "back")) {
        v10_view_pop(0);
        return;
    }
    if (!V.mod) return;
    const AccountInfo *acc = active_account();
    if (!strcmp(what, "share")) {
        char origin[256], enc[480], url[800];
        html_page_origin(origin, sizeof origin);
        backend_url_encode(enc, sizeof enc, jstr(V.mod, "name", "mod_name"));
        snprintf(url, sizeof url, "%s/?mod=%s", origin, enc);
        html_clipboard_write(url);
        V.copied = 1;
        view_render();
        html_timer_add(1500, view_uncopy, (void *) (uintptr_t) V.gen);
    } else if (!strcmp(what, "creator")) {
        // the profile view of { playerName: creator_name } (v10_social.c's openPlayerProfile)
        HtmlArg a = {0};
        a.kind = HTML_ARG_STR;
        a.str = jstr(V.mod, "creator_name", "creatorname");
        if (*a.str) v10_social_handle("openPlayerProfile", NULL, &a, 1);
    } else if (!strcmp(what, "use")) { // window.open('/?mod=<name>'): the mod, loaded here
        char q[400], enc[360];
        backend_url_encode(enc, sizeof enc, jstr(V.mod, "name", "mod_name"));
        snprintf(q, sizeof q, "mod=%s", enc);
        mods_ui_query(q);
    } else if (!strcmp(what, "report")) {
        ui_prompt("Report reason (min 10 chars):", view_reported, NULL);
    } else if (!strcmp(what, "feature") && acc && acc->is_dev) {
        char id[24], path[120], tok[400], body[48];
        jid(V.mod, id, sizeof id);
        const int on = !(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(V.mod, "featured")) || jnum(V.mod, "featured", NULL) != 0);
        v10_url_token(tok, sizeof tok);
        snprintf(path, sizeof path, "/mods/%s/feature?token=%s", id, tok);
        snprintf(body, sizeof body, "{\"featured\":%s}", on ? "true" : "false");
        v10_send("POST", path, body, view_feature_done, (void *) (uintptr_t) V.gen);
    } else if (!strcmp(what, "delete") && acc && acc->is_dev) {
        ui_confirm_labeled("Are you sure you want to delete this Mod?", "Delete", "Cancel", view_delete_confirmed, (void *) (uintptr_t) V.gen);
    }
}

// a map's own pack (loadModPack(modURL, true, true, undefined, undefined, creator), deob-10.0.7.js:226475): the Loaded
// tab lists it as "Server Mod" (not the last mod, no vote)
void mods_ui_map_pack(const char *url) {
    creation_vote_set_mod(0, NULL, NULL);
    loaded_add("Server Mod", url, "", "", 0);
    menu_rerender_if_open(MODS_WINDOW);
}

// ------------------------------------------------------------------ pack hooks (mod_pack.h)

// settings.tmp.canLoadMods (deob-10.0.7.js:225868)
int mods_ui_can_load(void) {
    return !ui_setting_is_false("canLoadMods");
}

// textures/logo.png: mainLogo.src = the pack's image while canChangeLogo (deob-10.0.7.js:225561), the page's own
// src back when the pack goes (windows[3].reset, :218827)
static char g_logo_src[512];
void mods_ui_pack_logo(const char *path) {
    const HtmlElem logo = html_dom_by_id("mainLogo");
    if (!path) {
        if (logo && g_logo_src[0]) html_dom_set_attr(logo, "src", g_logo_src);
        g_logo_src[0] = 0;
        return;
    }
    if (!logo || ui_setting_is_false("canChangeLogo")) return;
    if (!g_logo_src[0] && html_dom_get_attr(logo, "src", g_logo_src, sizeof g_logo_src) < 0) g_logo_src[0] = 0;
    char url[512];
    html_dom_asset_url("textures/logo.png", url, sizeof url); // the pack's copy through the asset overlay
    html_dom_set_attr(logo, "src", url);
}

void mods_ui_pack_settings(const char *text, const size_t len) {
    (void) len;
    const int n = ui_settings_mod_text(text);
    if (n) printf("[mod] scripts/settings.txt: %d settings\n", n);
}

// autoLoadLast && lastMod.length && loadModPack(lastMod, true) (deob-10.0.7.js:225960): windows[3].loaded names
// it "Auto Loaded" (:218811)
void mods_ui_autoload_last(void) {
    char url[600];
    if (ui_setting_is_false("autoLoadLast") || !ui_setting_value("autoLoadLast")) return;
    if (html_storage_get("krk_lastMod", url, sizeof url) < 0 || !url[0] || !mods_ui_can_load()) return;
    loaded_add("Auto Loaded", url, "", "", 0); // listed before the pack loads, as windows[3].loaded
    mod_pack_load_user(url);
}

// ------------------------------------------------------------------ the module

char *mods_ui_window_gen(const int id) {
    if (id == MODS_WINDOW) return manager_gen();
    if (id == PUBLISH_WINDOW) return publish_gen();
    return NULL;
}

void mods_ui_window_post(const int id) {
    if (id == MODS_WINDOW) fav_sync(); // windows[3].post: iìíìiíí(1); iíììïïï(1) (deob-10.0.7.js:218775-218778)
    // the Mod Loader's drop area opens the file dialog natively (the page's file input does nothing here)
    HtmlElem drop = id == LOADER_WINDOW ? html_dom_by_id("modDropper") : 0;
    if (drop) html_dom_set_attr(drop, "onclick", "knModPickLocal()");
}

static const char *sarg(const HtmlArg *a, const int argc, const int i) {
    return i < argc && a[i].kind == HTML_ARG_STR && a[i].str ? a[i].str : "";
}

static int iarg(const HtmlArg *a, const int argc, const int i) {
    if (i >= argc) return 0;
    if (a[i].kind == HTML_ARG_NUM || a[i].kind == HTML_ARG_BOOL) return (int) a[i].num;
    return a[i].kind == HTML_ARG_STR && a[i].str ? atoi(a[i].str) : 0;
}

int mods_ui_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    if (!name) return -1;
    if (!strcmp(name, "windows.3.switchTab")) {
        const int t = iarg(args, argc, 0);
        if (t >= 0 && t < TAB_COUNT && t != M.tab) {
            M.tab = t;
            menu_rerender_if_open(MODS_WINDOW);
        }
    } else if (!strcmp(name, "windows.3.loadMore")) {
        // loadMore(el): the link reads Loading... (loadMoreW) while the next page comes (deob-10.0.7.js:218779-218781)
        TabData *d = &M.data[M.tab];
        if (d->used && !d->waiting) {
            if (ev && ev->target) {
                html_dom_set_attr(ev->target, "class", "loadMoreW");
                html_dom_set_html(ev->target, "Loading...");
            }
            list_request(M.tab, d->page + 1);
        }
    } else if (!strcmp(name, "windows.3.searchMods")) {
        search();
    } else if (!strcmp(name, "windows.3.reset")) {
        mods_reset(0);
    } else if (!strcmp(name, "loadUserMod")) { // showWindow(18), "LOADING...", loadModPack (deob-10.0.7.js:225755-225762)
        menu_show_window(LOADER_WINDOW, 0);
        loader_info(I18N("generic.loading", "LOADING..."));
        char id[24];
        if (args && argc > 2 && args[2].kind == HTML_ARG_NUM) snprintf(id, sizeof id, "%.0f", args[2].num);
        else snprintf(id, sizeof id, "%s", sarg(args, argc, 2));
        load_user_mod(sarg(args, argc, 0), sarg(args, argc, 1), id, sarg(args, argc, 3), iarg(args, argc, 4), 0);
    } else if (!strcmp(name, "loadModURL")) {
        char url[512] = "";
        HtmlElem in = html_dom_by_id("modURL");
        if (in) html_dom_get_value(in, url, sizeof url);
        if (url[0]) load_user_mod(NULL, url, "", "", 0, 0);
    } else if (!strcmp(name, "openModDetail")) {
        mod_view_open(sarg(args, argc, 0));
    } else if (!strcmp(name, "knModView")) {
        mod_view_action(sarg(args, argc, 0));
    } else if (!strcmp(name, "loadMod") || !strcmp(name, "knModPickLocal")) {
        ed_pick_file(".zip", picked_local, NULL);
    } else if (!strcmp(name, "selectFile")) { // selectFile("modThumb" | "modZip"): the window's links, the hub's buttons
        const int thumb = !strcmp(sarg(args, argc, 0), "modThumb");
        if (thumb && !P.done && !level_ok(active_account())) return 1; // the window's Locked link
        ed_pick_file(thumb ? "image/png" : ".zip", picked_publish, thumb ? (void *) 1 : NULL);
    } else if (!strcmp(name, "windows.18.onFileChange")) { // the file inputs' onchange: no change event natively
    } else if (!strcmp(name, "windows.18.submit") || !strcmp(name, "uploadMod")) {
        publish();
    } else return -1;
    return 1;
}
