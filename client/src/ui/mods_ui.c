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
//   - Upload Mod (:220943-221050): signed out, the window asks to log in (the static 19.html); signed in, a thumbnail
//     (level 10, 250 / 500 KB premium; KB = 1e3 bytes), the .zip (25 / 50 MB premium; MB = 1e6), the name, Publish:
//     api.mods.publishKey({name, assetSizes}) then publishUpload(uploadUrl, fileId, zip, thumb) -> {data: {mod_id}};
//     on success the window closes, a success popup shows, and the Mod Manager forgets its lists and opens on My Mods.
//   - ?mod=<name> on the page loads that mod by name (:226181-226190; here GET /mods/by-name/<name>).
//
// Here (kn_brain, services/brain/asset_upload.c): publishKey is POST /mods/publish/key?token=, the files go to
// uploadUrl (/mods/publish) as chunks (?fileId=&key=&offset=, then &done=1) like the editor's assets, because the brain
// takes request bodies of 1 MiB; a chunk whose request failed outright (a brain restart: its tickets survive it) is
// sent again a few times before the upload gives up. Errors come back as MODS_* codes shown through errors.mods.* (i18n).
//
// Native differences: the page's <input type=file> does nothing natively, so the two Select links and the Mod
// Loader's drop area open the platform file dialog (ed_pick_file) through KrunkNative handlers (knModPick,
// knModPickLocal). The window markup is KrunkNative's own, on the page's classes.
#include <creation_vote.h>
#include <backend.h>
#include <backend_ui.h>
#include <html_dom.h>
#include <html_ui.h>
#include <i18n.h>
#include <mod_pack.h>
#include <ui_logic.h>

#include <cJSON.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

// the platform file dialog (client/src/editor/editor_tools.c; editor_internal.h pulls in GL)
typedef void (*EdFileFn)(void *user, const char *path);
void ed_pick_file(const char *accept, EdFileFn fn, void *user);

#define MODS_WINDOW 4
#define LOADER_WINDOW 18
#define PUBLISH_WINDOW 19
#define USER_ASSETS "https://user-assets.krunker.io"
#define CHUNK (512u * 1024u)        // kn_brain's request bodies are 1 MiB at most
#define CHUNK_TIMEOUT_MS 60000
#define CHUNK_RETRIES 20            // a request that failed outright (status 0): a restarting brain
#define CHUNK_RETRY_MS 1500
#define LOADED_MAX 16

enum { TAB_HOME, TAB_POPULAR, TAB_NEW, TAB_FAV, TAB_MINE, TAB_LOADED, TAB_SEARCH, TAB_COUNT };

static const char *const k_tab_names[TAB_COUNT] = {"Home", "Popular", "New", "Favorites", "My Mods", "Loaded", "Search"};

typedef struct {
    char *html;   // the cards so far
    int used, waiting, page, more;
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

// ------------------------------------------------------------------ the player's loaded mods

static void loaded_add(const char *name, const char *url, const char *id, const char *creator, const int thumb) {
    for (int i = 0; i < M.nloaded; i++)
        if (!strcmp(M.loaded[i].name, name)) return;
    if (M.nloaded == LOADED_MAX) {
        memmove(&M.loaded[0], &M.loaded[1], sizeof(M.loaded[0]) * (LOADED_MAX - 1));
        M.nloaded--;
    }
    LoadedMod *m = &M.loaded[M.nloaded++];
    snprintf(m->name, sizeof m->name, "%s", name && *name ? name : "Mod");
    snprintf(m->url, sizeof m->url, "%s", url ? url : "");
    snprintf(m->id, sizeof m->id, "%s", id ? id : "");
    snprintf(m->creator, sizeof m->creator, "%s", creator ? creator : "");
    m->thumb = thumb;
}

// loadUserMod / loadModURL: the pack becomes the player's mod (mod_pack.h), kept over map changes
static int load_user_mod(const char *name, const char *url, const char *id, const char *creator, const int thumb) {
    char full[600];
    mod_url_of(url, id, full, sizeof full);
    popup(2, I18N("generic.loading", "Loading..."));
    if (!mod_pack_load_user(full)) {
        popup(0, "This mod could not be loaded");
        return 0;
    }
    loaded_add(name, full, id, creator, thumb);
    creation_vote_set_mod(id ? atoi(id) : 0, name, creator); // #modVote: the loaded mod's vote (:225872-225880)
    popup(1, "Mod loaded");
    menu_rerender_if_open(MODS_WINDOW);
    return 1;
}

static void picked_local(void *user, const char *path) {
    (void) user;
    const size_t n = path ? strlen(path) : 0;
    if (n < 4 || strcasecmp(path + n - 4, ".zip")) {
        popup(0, I18N("mod.invalid", "Invalid mod file"));
        return;
    }
    if (!mod_pack_load_file(path)) {
        popup(0, "This mod could not be loaded");
        return;
    }
    const char *base = strrchr(path, '/');
    loaded_add(base ? base + 1 : path, path, "", "", 0);
    creation_vote_set_mod(0, NULL, NULL); // a local zip has no brain id: no vote
    popup(1, "Mod loaded");
}

// ------------------------------------------------------------------ the Mod Manager (window 4)

static void list_request(int tab, int page);

static void card(StrBuf *sb, const cJSON *r) {
    char id[24], url[600], votes[32];
    jid(r, id, sizeof id);
    const char *name = jstr(r, "mna", "mod_name"), *creator = jstr(r, "ctn", "creatorname");
    const int thumb = info_thumb(r);
    mod_url_of(jstr(r, "url", "mod_url"), id, url, sizeof url);
    snprintf(votes, sizeof votes, "%.0f", jnum(r, "vte", "mod_votes"));
    const int fav = jnum(r, "fav", NULL) != 0 || M.tab == TAB_FAV;
    sb_append(sb, "<div style='display:inline-block;'><div class='mapListItem'><div class='mapActionOvrl'><div class='mapActionHol'>"
                  "<div class='mapActionB' title='Load' onclick='loadUserMod(&quot;");
    arg(sb, name);
    sb_append(sb, "&quot;,&quot;");
    arg(sb, url);
    sb_appendf(sb, "&quot;,&quot;%s&quot;,&quot;", id);
    arg(sb, creator);
    sb_appendf(sb, "&quot;,%d)'><span class='material-icons' style='font-size:70px;color:#fff;'>play_arrow</span></div></div></div>", thumb);
    sb_appendf(sb, "<span title='Favorite' class='material-icons creationAction%s' onclick='favoriteM(1, this, %s);'>grade</span>", fav ? " sel" : "", id);
    sb_append(sb, "<div class='mapListThumbHolder'><img draggable='false' class='mapListThumb' loading='lazy' src='");
    if (thumb) sb_appendf(sb, USER_ASSETS "/md%s/thumb.png", id);
    else {
        const char *img = jstr(r, "mod_image", NULL);
        esc(sb, *img ? img : "./img/noimg.png");
    }
    sb_append(sb, "' onerror='this.onerror=null;this.src=\"./img/noimg.png\";'/></div><div class='mapListCrdI'>");
    esc(sb, name);
    sb_append(sb, "<span style='float:right;color:rgba(255,255,255,0.6);margin-right:6px;'>");
    esc(sb, votes);
    sb_append(sb, " <span class='material-icons' style='font-size:16px;vertical-align:middle'>thumb_up</span></span></div></div></div>");
}

static void on_list(void *user, const int status, const char *body, const size_t length) {
    (void) length;
    const unsigned gen = (unsigned) ((uintptr_t) user >> 8);
    const int tab = (int) ((uintptr_t) user & 0xff);
    if (gen != M.gen || tab < 0 || tab >= TAB_COUNT) return;
    TabData *d = &M.data[tab];
    d->waiting = 0;
    cJSON *j = status == 200 && body ? cJSON_Parse(body) : NULL;
    const cJSON *rows = cJSON_GetObjectItemCaseSensitive(j, "data");
    StrBuf sb = {0};
    if (d->html) sb_append(&sb, d->html);
    const int n = cJSON_GetArraySize(rows);
    const cJSON *r;
    cJSON_ArrayForEach(r, rows) card(&sb, r);
    if (!n && !d->html) sb_append(&sb, I18N("windows.mods.none", "No Mods found"));
    d->more = n > 0 && tab != TAB_HOME && tab != TAB_FAV;
    free(d->html);
    d->html = sb_finish(&sb);
    cJSON_Delete(j);
    if (M.tab == tab) menu_rerender_if_open(MODS_WINDOW);
}

static void list_request(const int tab, const int page) {
    const AccountInfo *acc = active_account();
    char path[160];
    snprintf(path, sizeof path, "/webhooks/general/mods/list?index=%d&pos=%d&accountId=%d", tab, page, acc ? acc->id : 0);
    M.data[tab].waiting = 1;
    M.data[tab].used = 1;
    if (!backend_http_get(path, on_list, (void *) (uintptr_t) (M.gen << 8 | (unsigned) tab))) {
        M.data[tab].waiting = 0;
        free(M.data[tab].html);
        StrBuf none = {0};
        sb_append(&none, I18N("windows.mods.none", "No Mods found"));
        M.data[tab].html = sb_finish(&none);
    }
}

static void on_search(void *user, const int status, const char *body, const size_t length) {
    (void) user;
    (void) length;
    HtmlElem out = html_dom_by_id("searchResultsMO");
    if (!out) return;
    cJSON *j = status == 200 && body ? cJSON_Parse(body) : NULL;
    const cJSON *rows = cJSON_GetObjectItemCaseSensitive(j, "data");
    StrBuf sb = {0};
    const cJSON *r;
    cJSON_ArrayForEach(r, rows) card(&sb, r);
    if (!cJSON_GetArraySize(rows)) sb_append(&sb, I18N("windows.mods.none", "No Mods found"));
    char *html = sb_finish(&sb);
    html_dom_set_html(out, html);
    free(html);
    cJSON_Delete(j);
}

static void search(void) {
    char val[128] = "", enc[400], path[480];
    HtmlElem in = html_dom_by_id("mdSrch");
    if (in) html_dom_get_value(in, val, sizeof val);
    backend_url_encode(enc, sizeof enc, val);
    snprintf(path, sizeof path, "/webhooks/general/mods/search?val=%s", enc);
    HtmlElem out = html_dom_by_id("searchResultsMO");
    if (out) html_dom_set_html(out, I18N("generic.loading", "Loading..."));
    backend_http_get(path, on_search, NULL);
}

static char *manager_gen(void) {
    StrBuf sb = {0};
    sb_append(&sb, "<a href='javascript:;' onclick='showWindow(18)' class='menuLink'>Load Mod</a> | "
                   "<a href='javascript:;' onclick='showWindow(19)' class='menuLink'>Upload Mod</a>"
                   "<div style='margin-top:30px;margin-bottom:10px'><div class='bigMenTabs'>");
    for (int i = 0; i < TAB_COUNT; i++) {
        sb_appendf(&sb, "<div class='bigMenTab%s' onmouseenter='playTick(0.1);' onclick='playSelect();windows[3].switchTab(%d)'>",
                   M.tab == i ? " bigMenTabA" : "", i);
        if (i == TAB_HOME) sb_append(&sb, "<span class='material-icons-outlined' style='color:#fbc02d;font-size:42px;vertical-align:bottom'>home</span>");
        else if (i == TAB_SEARCH) sb_append(&sb, "<span class='material-icons' style='color:#ffffff;font-size:42px;vertical-align:bottom'>search</span>");
        else sb_append(&sb, k_tab_names[i]);
        sb_append(&sb, "</div>");
    }
    sb_append(&sb, "</div></div><div style='color:rgba(255,255,255,0.3)' id='modList'>");
    if (M.tab == TAB_SEARCH) {
        sb_append(&sb, "<input type='text' id='mdSrch' class='smlInput' placeholder='Mod name' onkeydown='if(window.event.keyCode==13)windows[3].searchMods()' />"
                       "<div class='searchBtn' onclick='windows[3].searchMods()'><span class='material-icons' style='color:#fff;font-size:45px;'>search</span></div>"
                       "<div style='color:rgba(0,0,0,0.5);margin-top:10px' id='searchResultsMO'></div>");
    } else if (M.tab == TAB_LOADED) {
        for (int i = 0; i < M.nloaded; i++) {
            const LoadedMod *m = &M.loaded[i];
            sb_append(&sb, "<div class='mapListItem'><div style='height:200px;width:100%;overflow:hidden;'><img class='mapListThumb' src='");
            if (m->thumb && m->id[0]) sb_appendf(&sb, USER_ASSETS "/md%s/thumb.png", m->id);
            else sb_append(&sb, "./img/noimg.png");
            sb_append(&sb, "' /></div><div style='margin-top:1px'><a href='javascript:;'>");
            esc(&sb, m->name);
            sb_append(&sb, "</a></div></div>");
        }
        if (M.nloaded) sb_append(&sb, "<div class='loadMoreD' onclick='windows[3].reset()'>Reset Mods</div>");
        else sb_append(&sb, I18N("windows.mods.none", "No Mods found"));
    } else {
        TabData *d = &M.data[M.tab];
        if (!d->used) list_request(M.tab, 0);
        if (d->html) sb_append(&sb, d->html);
        if (d->waiting) sb_append(&sb, "<div class='loadMoreW'>Loading...</div>");
        else if (d->more) sb_append(&sb, "<div class='loadMoreD' onclick='windows[3].loadMore(this)'>Load More</div>");
    }
    sb_append(&sb, "</div>");
    return sb_finish(&sb);
}

static void lists_reset(void) {
    for (int i = 0; i < TAB_COUNT; i++) free(M.data[i].html);
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
    HtmlElem el = html_dom_by_id("modUpResp");
    if (!el) return;
    html_dom_set_style(el, "color", err ? "#e33b3b" : "rgba(255,255,255,0.7)");
    html_dom_set_text(el, P.resp);
}

static char *publish_form(void) {
    const AccountInfo *acc = active_account();
    if (!acc) return NULL; // signed out: the static 19.html (log in first)
    const int lvl = level_ok(acc);
    StrBuf sb = {0};
    sb_append(&sb, "<div id='modPubBody' style='padding:10px 5px 5px 5px'>"
                   "<div style='color:rgba(255,255,255,0.6);font-size:15px;margin-bottom:5px'>Thumbnail (.png)<span style='float:right' id='modThumbSize'>");
    if (lvl) sb_appendf(&sb, "%.2f / %d KB", P.thumb_size / 1e3, thumb_kb(acc));
    else sb_append(&sb, "from level 10");
    sb_append(&sb, "</span></div><div id='modThumbH' style='display:flex;align-items:center;gap:10px;padding:10px;border-radius:4px;"
                   "background:rgba(255,255,255,0.12);margin-bottom:15px'><img id='modThumbPrev' src='");
    esc(&sb, P.thumb_src[0] ? P.thumb_src : "./img/noimg.png");
    sb_append(&sb, "' onerror='this.onerror=null;this.src=\"./img/noimg.png\";' style='width:80px;height:80px;object-fit:cover;border-radius:4px'>");
    if (lvl) sb_append(&sb, "<a href='javascript:;' class='menuLink' style='margin:0' onclick='knModPick(&quot;thumb&quot;)'>Choose image</a>");
    else sb_append(&sb, "<span style='color:rgba(255,255,255,0.4)'>Locked</span>");
    sb_appendf(&sb, "</div><div style='color:rgba(255,255,255,0.6);font-size:15px;margin-bottom:5px'>Mod pack (.zip)<span style='float:right' "
                    "id='modZipSize'>%.2f / %d MB</span></div><div id='modZipH' style='display:flex;align-items:center;gap:10px;padding:10px;"
                    "border-radius:4px;background:rgba(255,255,255,0.12);margin-bottom:15px'><span class='material-icons' "
                    "style='color:rgba(255,255,255,0.5);font-size:32px'>folder_zip</span><a href='javascript:;' class='menuLink' style='margin:0' "
                    "onclick='knModPick(&quot;zip&quot;)'>Choose .zip</a><span id='modZipName' style='color:rgba(255,255,255,0.5)'>",
               P.zip_size / 1e6, zip_mb(acc));
    esc(&sb, P.zip_name);
    sb_append(&sb, "</span></div><input id='modNameNew' type='text' maxlength='128' class='accountInput' style='margin-top:0;margin-bottom:15px' placeholder='");
    esc(&sb, I18N("windows.publish.mod.name", "Mod Name"));
    sb_append(&sb, "' value='");
    esc(&sb, P.prefill);
    sb_appendf(&sb, "' autocomplete='off'><div id='modUpResp' style='min-height:20px;color:%s;font-size:14px;text-align:center;margin-bottom:10px'>",
               P.resp_err ? "#e33b3b" : "rgba(255,255,255,0.7)");
    esc(&sb, P.resp);
    sb_append(&sb, "</div><a id='modPubSubmit' href='javascript:;' class='menuLink' style='display:block;text-align:center;padding:12px;"
                   "font-size:20px;background:rgba(255,255,255,0.12);border-radius:4px' onclick='knModPublish()'>");
    sb_append(&sb, P.update ? "Update" : "Publish");
    sb_append(&sb, "</a></div>");
    return sb_finish(&sb);
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

// the hub's popup cannot be re-rendered under the player: the picked file's size and name go in place
static void picked_in_place(const int thumb) {
    const AccountInfo *acc = active_account();
    char t[96];
    HtmlElem e;
    if (thumb) {
        snprintf(t, sizeof t, "%.2f / %d KB", P.thumb_size / 1e3, thumb_kb(acc));
        if ((e = html_dom_by_id("modThumbSize"))) html_dom_set_text(e, t);
    } else {
        snprintf(t, sizeof t, "%.2f / %d MB", P.zip_size / 1e6, zip_mb(acc));
        if ((e = html_dom_by_id("modZipSize"))) html_dom_set_text(e, t);
        if ((e = html_dom_by_id("modZipName"))) html_dom_set_text(e, P.zip_name);
    }
}

char *mods_ui_hub_publish(const char *name, const char *mod_id, const int thumb, void (*done)(void)) {
    if (!P.inflight) forget_files();
    P.done = done;
    P.update = name && *name;
    snprintf(P.prefill, sizeof P.prefill, "%s", P.update ? name : "");
    if (P.update && thumb && mod_id && *mod_id) snprintf(P.thumb_src, sizeof P.thumb_src, USER_ASSETS "/md%s/thumb.png", mod_id);
    else P.thumb_src[0] = 0;
    return publish_form();
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
    if (P.done) picked_in_place(thumb);
    char name[256] = "";
    HtmlElem in = html_dom_by_id("modNameNew");
    if (in) html_dom_get_value(in, name, sizeof name);
    if (!P.done) menu_rerender_if_open(PUBLISH_WINDOW);
    in = html_dom_by_id("modNameNew");
    if (in && name[0]) html_dom_set_value(in, name);
    HtmlElem prev = thumb ? html_dom_by_id("modThumbPrev") : 0;
    if (prev) {
        char url[1100];
        snprintf(url, sizeof url, "file://%s", path);
        html_dom_set_attr(prev, "src", url);
    }
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
        answer_error(body, err, sizeof err, error_text("MODS_UPLOAD_STORAGE_FAIL", "The mod upload failed"));
        up_end(err);
        return;
    }
    if (P.part >= 2) { // done answered
        cJSON *j = body ? cJSON_Parse(body) : NULL;
        const double id = cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(j, "data"), "mod_id"));
        cJSON_Delete(j);
        if (!(id > 0)) {
            up_end(error_text("MODS_UPLOAD_STORAGE_FAIL", "The mod upload failed"));
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
        popup(1, "Your mod is published");
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
            up_end(error_text("MODS_UPLOAD_STORAGE_FAIL", "The mod upload failed"));
        return;
    }
    if (!P.data) {
        P.sent = P.len = 0;
        P.data = read_whole(P.part == 0 ? P.zip : P.thumb, &P.len);
        if (!P.data || (double) P.len != (P.part == 0 ? P.zip_size : P.thumb_size)) {
            up_end(error_text("MODS_UPLOAD_SIZE_MISMATCH", "The file changed since it was chosen"));
            return;
        }
    }
    P.at = P.sent;
    P.n = P.len - P.sent < CHUNK ? P.len - P.sent : CHUNK;
    snprintf(url, sizeof url, "%s?fileId=%s&key=%s&offset=%zu", P.base, P.file_id, P.part == 0 ? "mod" : "thumb", P.at);
    P.sent += P.n;
    if (!backend_http_post_bytes(url, "application/octet-stream", P.data + P.at, P.n, CHUNK_TIMEOUT_MS, up_answer, (void *) user))
        up_end(error_text("MODS_UPLOAD_STORAGE_FAIL", "The mod upload failed"));
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
        answer_error(body, err, sizeof err, error_text("MODS_UPLOAD_STORAGE_FAIL", "The mod upload failed"));
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
    if (!*s) { set_resp("Give the mod a name", 1); return; }
    if (!P.zip[0]) { set_resp("Choose the mod's .zip first", 1); return; }
    if (P.zip_size / 1e6 > zip_mb(acc)) { set_resp(error_text("MODS_PUBLISH_SIZE_LIMIT", "The .zip is too large"), 1); return; }
    if (P.thumb[0] && !level_ok(acc)) { set_resp(error_text("MODS_PUBLISH_THUMB_LEVEL", "A thumbnail needs level 10"), 1); return; }
    if (P.thumb[0] && P.thumb_size / 1e3 > thumb_kb(acc)) { set_resp(error_text("MODS_PUBLISH_SIZE_LIMIT", "The image is too large"), 1); return; }
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
    set_resp("Asking the server for an upload...", 0);
    if (!backend_http_request("POST", url, json ? json : "{}", on_key, (void *) (uintptr_t) P.gen))
        up_end(error_text("MODS_UPLOAD_STORAGE_FAIL", "The mod upload failed"));
    free(json);
}

// ------------------------------------------------------------------ ?mod=<name>

static void on_by_name(void *user, const int status, const char *body, const size_t length) {
    (void) user;
    (void) length;
    cJSON *j = status == 200 && body ? cJSON_Parse(body) : NULL;
    const cJSON *d = cJSON_GetObjectItemCaseSensitive(j, "data");
    char id[24];
    jid(d, id, sizeof id);
    const char *name = jstr(d, "mod_name", "name");
    if (d && id[0]) load_user_mod(name, jstr(d, "mod_url", NULL), id, jstr(d, "creator_name", "creatorname"), info_thumb(d));
    else popup(0, "No mod of that name");
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

// favoriteM(1, this, id): the star's sel class flips, kn_brain's fvM (kind 1, creations.c) records it, and the
// Favorites tab is asked again next time
void mods_ui_favorite(const char *id, const HtmlElem star) {
    const long long mid = id ? strtoll(id, NULL, 10) : 0;
    if (mid <= 0 || !active_account()) return;
    const int now = star ? !html_dom_has_class(star, "sel") : 1;
    if (star) html_dom_set_class(star, "sel", now);
    char frame[96];
    snprintf(frame, sizeof frame, "[\"fvM\",%lld,1,%d]", mid, now);
    if (account_backend() != ACCOUNT_BACKEND_KRUNKER) backend_social_send_text(frame);
    free(M.data[TAB_FAV].html);
    memset(&M.data[TAB_FAV], 0, sizeof M.data[TAB_FAV]);
}

// ------------------------------------------------------------------ the module

char *mods_ui_window_gen(const int id) {
    if (id == MODS_WINDOW) return manager_gen();
    if (id == PUBLISH_WINDOW) return publish_gen();
    return NULL;
}

void mods_ui_window_post(const int id) {
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
    (void) ev;
    if (!name) return -1;
    if (!strcmp(name, "windows.3.switchTab")) {
        const int t = iarg(args, argc, 0);
        if (t >= 0 && t < TAB_COUNT && t != M.tab) {
            M.tab = t;
            menu_rerender_if_open(MODS_WINDOW);
        }
    } else if (!strcmp(name, "windows.3.loadMore")) {
        TabData *d = &M.data[M.tab];
        if (!d->waiting && d->more) {
            list_request(M.tab, ++d->page);
            menu_rerender_if_open(MODS_WINDOW);
        }
    } else if (!strcmp(name, "windows.3.searchMods")) {
        search();
    } else if (!strcmp(name, "windows.3.reset")) {
        M.nloaded = 0;
        mod_pack_reset();
        popup(1, "Mods cleared");
        menu_rerender_if_open(MODS_WINDOW);
    } else if (!strcmp(name, "loadUserMod")) {
        menu_show_window(LOADER_WINDOW, 0);
        char id[24];
        if (args && argc > 2 && args[2].kind == HTML_ARG_NUM) snprintf(id, sizeof id, "%.0f", args[2].num);
        else snprintf(id, sizeof id, "%s", sarg(args, argc, 2));
        load_user_mod(sarg(args, argc, 0), sarg(args, argc, 1), id, sarg(args, argc, 3), iarg(args, argc, 4));
    } else if (!strcmp(name, "loadModURL")) {
        char url[512] = "";
        HtmlElem in = html_dom_by_id("modURL");
        if (in) html_dom_get_value(in, url, sizeof url);
        if (url[0]) load_user_mod("", url, "", "", 0);
    } else if (!strcmp(name, "loadMod") || !strcmp(name, "knModPickLocal")) {
        ed_pick_file(".zip", picked_local, NULL);
    } else if (!strcmp(name, "knModPick")) {
        const int thumb = !strcmp(sarg(args, argc, 0), "thumb");
        if (thumb && !level_ok(active_account())) return 1;
        ed_pick_file(thumb ? "image/png" : ".zip", picked_publish, thumb ? (void *) 1 : NULL);
    } else if (!strcmp(name, "knModPublish")) {
        publish();
    } else return -1;
    return 1;
}
