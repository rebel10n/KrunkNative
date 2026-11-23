// Krunker v10.0.7's Skin Manager on the v10 page: the sandbox menu's "Skin Manager" item (menuBtnSkinManager,
// deob-10.0.7.js:149446, shown by $skinManagerEnabled in connectToSandbox :229392; v10_menu.c sandbox_apply) calls
// window.showSkinManager (:204459), which pushes component iïiìíií (:124611-127559) on the page's one view stack
// (v10_view_push, v10_competitive.c) inside a fullscreen popup (iîïííií, :12094: popup-container svelte-1wz7ymp).
//
// The window lists the account's custom skins (a svelte-12kndpz table of 10 a page, newest first, searchable by
// name or id with a 300 ms debounce, Test / Edit / Duplicate / Delete per row) and edits one (General, Appearance,
// Advanced tabs; Back, Save, Capture Thumbnail). Its data is the skins service (:124350-124511) over the gapi tunnel
// of the social socket (v10_gapi.c -> services/brain/skins.c), or krunker.io's own gapi for a krunker.io account
// (v10_gapi.c kio_gapi). The provider (:124230-124345) caches a page for 30 s and is refreshed (invalidateCache) by
// every write; a list that fails shows the rows it had (none), as v10's table swallows it.
//
// The markup is v10's templates with the same classes (ui/css/svelte.css styles svelte-103lu6x, -12kndpz, -iy81sy,
// -wfs5t6 and -3rvofx); Svelte's __click / __input / __change are inline knSkins(op, ...) handlers here, and the
// elements carry knSm* ids (KrunkNative-only, for the client e2e). The strings are v10's (the component has no i18n).
//
// Not ported, each because Box Office lacks what it needs: the 3D preview iframe (the showcase page: the preview
// pane, Capture Thumbnail, the wear / paint / attach / charm editors), the asset uploads (models, textures, sounds,
// thumbnails: POST /skins/:id/upload is refused, skins.c), the Appearance tab's upload, dye colour and shader
// sections, and the Test button's skin swap on the sandbox player (it closes the window and enters the sandbox game,
// as testSkinSandbox :204469 does before it calls skinManager.setSkin).
#include <v10_skin_manager.h>
#include <v10_social.h>
#include <v10_ui.h>
#include <backend.h>
#include <backend_ui.h>
#include <html_ui.h>
#include <ruleset.h>
#include <ui_logic.h>
#include <weapons.h>
#include "../viewer/viewer_store.h" // the catalogue's base skins and the rarity colours
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MSG 320
#define PAGE_SIZE 10          // the table's pageSize default (:125010)
#define CACHE_MS 30000.0      // the provider's cache lifetime
#define SEARCH_DEBOUNCE_MS 300.0

enum { TAB_GENERAL, TAB_APPEARANCE, TAB_ADVANCED };
enum { R_LIST, R_SAVE, R_DELETE, R_DUPLICATE };
enum { T_PLAYER_CARD = 16, T_EMOJI = 17, T_PAINT = 18 };

static struct {
    int depth;               // the window's view-stack depth, 0 = closed
    unsigned gen;            // bumped on close: late answers are dropped
    // the table (iîîïïìî) and its provider (iiìîîìï)
    cJSON *rows;             // the page's skins
    int page, total, loading;
    char key[160];           // the cached page's "page|search"
    double fetched_at;       // when it came (0 = none / invalidated)
    unsigned list_seq;       // the newest list request
    char search[128];        // the search field
    char filter[128];        // the provider's search (the field, trimmed, after the debounce)
    double search_due;       // when the field's value becomes the filter (0 = nothing pending)
    // the window
    int busy;                // iÍiíiîï: a save / delete / duplicate is running
    char error[MSG];
    cJSON *editing;          // editingSkin, NULL = the list
    int saved;               // iÎïìíiì
    int tab;
    // the delete prompt
    int confirm_depth;
    int64_t confirm_id;
} SM;

typedef struct {
    int kind;
    unsigned gen, seq;
} Req;

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec * 1000.0 + (double) ts.tv_nsec / 1e6;
}

static double jnum(const cJSON *o, const char *k) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? v->valuedouble : 0;
}

static const char *jstr(const cJSON *o, const char *k) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) && v->valuestring ? v->valuestring : "";
}

static void sb_text(StrBuf *sb, const char *text) { sb_append_esc(sb, text ? text : ""); }

static void trimmed(char *out, const size_t n, const char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    snprintf(out, n, "%s", s);
    size_t len = strlen(out);
    while (len > 0 && (out[len - 1] == ' ' || out[len - 1] == '\t' || out[len - 1] == '\n' || out[len - 1] == '\r')) out[--len] = 0;
}

static int64_t editing_id(void) {
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(SM.editing, "skinId");
    return cJSON_IsNumber(id) ? (int64_t) id->valuedouble : 0;
}

static cJSON *editing_config(void) {
    cJSON *cfg = cJSON_GetObjectItemCaseSensitive(SM.editing, "itemConfig");
    if (!cJSON_IsObject(cfg)) {
        cJSON_DeleteItemFromObjectCaseSensitive(SM.editing, "itemConfig");
        cfg = cJSON_AddObjectToObject(SM.editing, "itemConfig");
    }
    return cfg;
}

static void set_number(cJSON *o, const char *k, const double v) {
    cJSON *it = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsNumber(it)) cJSON_SetNumberValue(it, v);
    else {
        cJSON_DeleteItemFromObjectCaseSensitive(o, k);
        cJSON_AddNumberToObject(o, k, v);
    }
}

// ------------------------------------------------------------------ the item tables

// iiiïiií + iìíiiií (:24595-24598): a cosmetic's social.market.* label, a weapon skin's weapon name
static const char *type_label(const int type, const int weapon) {
    static const char *const keys[] = {NULL, "social.market.hat", "social.market.body", "social.market.melee", "social.market.spray",
                                       "social.market.dye", "social.market.waist", "social.market.face", "social.market.shoe",
                                       "social.market.pet", "social.market.collectible", "social.market.wrist", "social.market.charm",
                                       "social.market.ticket", "social.market.back", "social.market.head", "social.market.playerCard",
                                       "social.market.emoji", "social.market.paint"};
    static const char *const english[] = {NULL, "Hat Item", "Body Item", "Melee Item", "Spray Item", "Dye Item", "Waist Item",
                                          "Face Item", "Shoe Item", "Pet Item", "Collectible Item", "Wrist Item", "Charm Item",
                                          "Spin Ticket Item", "Back Item", "Head Item", "Player Card", "Emoji", "Paint"};
    if (type > 0 && type < (int) (sizeof(keys) / sizeof(keys[0]))) return i18n_tr(keys[type], english[type]);
    const Weapon *w = weapon >= 1 ? rules_weapon(weapon - 1) : NULL;
    return w && w->name ? w->name : "";
}

static const char *const TYPE_NAMES[] = {"Weapon", "Hat", "Body", "Melee", "Spray", "Dye", "Waist", "Face", "Shoe", "Pet",
                                         "Collectible", "Wrist", "Charm", "Ticket", "Back", "Head", "Player Card", "Emoji", "Paint"};
#define TYPE_COUNT ((int) (sizeof(TYPE_NAMES) / sizeof(TYPE_NAMES[0])))

// the General tab's weapon list: every weapon but these 1-based ids (iÎíîîíî, :125107)
static int weapon_listed(const int id) { return id != 13 && id != 18 && id != 20 && id != 26 && id != 27; }

static const char *rarity_color(const int rarity) {
    return rarity >= 0 && rarity < g_viewer_rarity_count && g_viewer_rarities[rarity].color ? g_viewer_rarities[rarity].color : "#B0B0B0";
}

// iììïïíî (:120485-120570): the base skin whose attributes a custom skin copies, itemConfig.copyId or the type's
// default; -1 = none (the attributes are then {id: 1} or a tex, which the Automatic Attributes box shows)
static int base_skin(const int type, const int weapon, const cJSON *cfg, cJSON *attrs) {
    const cJSON *copy = cJSON_GetObjectItemCaseSensitive(cfg, "copyId");
    int id = cJSON_IsNumber(copy) ? (int) copy->valuedouble : 0;
    const int custom_model = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(cfg, "customModel")) ||
                             cJSON_IsString(cJSON_GetObjectItemCaseSensitive(cfg, "customModel"));
    const int custom_tex = cJSON_GetObjectItemCaseSensitive(cfg, "customTextures") != NULL;
    if (!id) {
        static const short weapon_base[32] = {[1] = 7418, [2] = 7401, [3] = 7411, [4] = 7417, [5] = 7412, [6] = 7416, [7] = 7410,
                                              [8] = 7415, [9] = 7413, [11] = 7408, [12] = 7402, [14] = 7407, [15] = 7409,
                                              [16] = 7414, [17] = 7403, [19] = 7404, [22] = 7419, [29] = 7405, [31] = 7406};
        if (type == 7) id = 1830;
        else if (type == 5) {
        } else if (type == 0 && weapon >= 0 && weapon < 32 && weapon_base[weapon]) id = weapon_base[weapon];
        else if (type == 0 && (weapon == 21 || weapon == 24 || weapon == 30)) {
            if (!custom_tex && attrs) {
                char tex[32];
                snprintf(tex, sizeof(tex), "weapons/weapon_%d", weapon);
                cJSON_AddStringToObject(attrs, "tex", tex);
            }
        } else if (!custom_model && attrs) cJSON_AddNumberToObject(attrs, "id", 1);
    }
    return id > 0 && id < g_viewer_skin_count ? id : -1;
}

// iììíiìì (:120484): the attributes a custom skin takes from its base skin
static const char *const AUTO_KEYS[] = {
    "alpha", "animInd", "animS", "attachAimY", "attachBypass", "attachScale", "attachYOff", "attachZOff", "cSwing", "charmMScl",
    "charmMXOff", "charmMYOff", "charmMZOff", "charmScl", "charmXOff", "charmYOff", "charmZOff", "classIndex", "color", "emiss", "env",
    "expScl", "explode", "fileExt", "flipAnim", "forceAttach", "frameT", "frames", "glow", "glowInt", "hairCol", "hasAnim", "id",
    "inspS", "inspSLoop", "jumpSnd", "keyW", "killSnd", "leftHoldX", "leftHoldY", "leftHoldZ", "legLen", "lowR", "mScl", "mid", "midT",
    "minGlo", "movD", "movT", "noAttach", "noBody", "noCharm", "noComp", "noEmis", "noHair", "noHead", "noLeftSleeve", "noLegs", "noMuz",
    "noRightHand", "noScope", "noShine", "noSleeve", "noSnd", "oFlip", "opac", "opacity", "osxOff", "osxRot", "osyOff", "oszOff",
    "pantsCol", "particle", "pat", "prevRot", "priceMlt", "projection", "pulsT", "qsKR", "rAnim", "rAnimA", "rAnimM", "rAnimT",
    "randomLeg", "reloadSnd", "replaceHair", "rgb", "rgbS", "rightHoldX", "rightHoldY", "rightHoldZ", "rot", "rotOffA", "sameGlow",
    "scl", "sclH", "sclMlt", "sclW", "shaderNum", "shirtCol", "shoeCol", "side", "sitMlt", "sitOff", "sitOffZ", "skinCol", "sleeveCol",
    "snd", "spnScl", "spnSpd", "sprayI", "spraySnd", "statc", "stepSnd", "sxOff", "sxRot", "syOff", "syRot", "szOff", "szRot",
    "template", "tex", "texEncode", "tileS", "trans", "useSkin", "video", "waistCol", "watch", "wire", "xOff", "xOrg", "xRot", "yOff",
    "yOrg", "yRot", "zOff", "zOrg"};

// JSON.stringify(v, null, 2): two-space indents, "{}" / "[]" when empty (cJSON_Print uses tabs and "{\n}")
static void json_pretty(StrBuf *sb, const cJSON *v, const int depth) {
    if ((cJSON_IsObject(v) || cJSON_IsArray(v)) && v->child) {
        const int obj = cJSON_IsObject(v);
        sb_append(sb, obj ? "{\n" : "[\n");
        for (const cJSON *c = v->child; c; c = c->next) {
            for (int i = 0; i <= depth; i++) sb_append(sb, "  ");
            if (obj) {
                cJSON *key = cJSON_CreateString(c->string ? c->string : "");
                char *k = cJSON_PrintUnformatted(key);
                sb_append(sb, k ? k : "\"\"");
                sb_append(sb, ": ");
                free(k);
                cJSON_Delete(key);
            }
            json_pretty(sb, c, depth + 1);
            sb_append(sb, c->next ? ",\n" : "\n");
        }
        for (int i = 0; i < depth; i++) sb_append(sb, "  ");
        sb_append(sb, obj ? "}" : "]");
        return;
    }
    if (cJSON_IsObject(v) || cJSON_IsArray(v)) {
        sb_append(sb, cJSON_IsObject(v) ? "{}" : "[]");
        return;
    }
    char *leaf = cJSON_PrintUnformatted(v);
    sb_append(sb, leaf ? leaf : "null");
    free(leaf);
}

static char *stringify(const cJSON *v) {
    StrBuf sb = {0};
    json_pretty(&sb, v, 0);
    return sb_finish(&sb);
}

// the Advanced tab's Automatic Attributes: the base skin's AUTO_KEYS (skins.json), as the box prints them
static char *auto_attributes(void) {
    cJSON *attrs = cJSON_CreateObject();
    const int base = base_skin((int) jnum(SM.editing, "type"), (int) jnum(SM.editing, "weapon"), editing_config(), attrs);
    const cJSON *json = base >= 0 ? ui_loadout_skin_json(base) : NULL;
    for (size_t i = 0; json && i < sizeof(AUTO_KEYS) / sizeof(AUTO_KEYS[0]); i++) {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(json, AUTO_KEYS[i]);
        if (v) cJSON_AddItemToObject(attrs, AUTO_KEYS[i], cJSON_Duplicate(v, 1));
    }
    char *text = stringify(attrs);
    cJSON_Delete(attrs);
    return text;
}

// ------------------------------------------------------------------ requests

static void on_answer(void *user, int status, const cJSON *data, const cJSON *error);
static void render(void);

static void send_req(const char *method, const char *path, cJSON *body, const int kind) {
    Req *r = calloc(1, sizeof(*r));
    if (!r) {
        cJSON_Delete(body);
        return;
    }
    r->kind = kind;
    r->gen = SM.gen;
    if (kind == R_LIST) r->seq = ++SM.list_seq;
    const int sent = v10_gapi(method, path, body, on_answer, r);
    cJSON_Delete(body);
    if (!sent) on_answer(r, 0, NULL, NULL); // no brain: the failure path (frees r)
}

// the table's fetchData + count (one list call answers both): the cached page while it is fresh
static void load_page(void) {
    char key[sizeof(SM.key)];
    snprintf(key, sizeof(key), "%d|%s", SM.page, SM.filter);
    if (SM.fetched_at > 0 && !strcmp(key, SM.key) && now_ms() - SM.fetched_at < CACHE_MS) return;
    snprintf(SM.key, sizeof(SM.key), "%s", key);
    SM.fetched_at = 0;
    SM.loading = 1;
    char path[400];
    int n = snprintf(path, sizeof(path), "/skins?page=%d&page_size=%d", SM.page, PAGE_SIZE);
    if (SM.filter[0] && n > 0 && (size_t) n < sizeof(path)) {
        char enc[320];
        backend_path_segment(enc, sizeof(enc), SM.filter);
        snprintf(path + n, sizeof(path) - (size_t) n, "&search=%s", enc);
    }
    send_req("GET", path, NULL, R_LIST);
}

// invalidateCache: every write refreshes the table (onRefresh)
static void invalidate(void) {
    SM.fetched_at = 0;
    SM.key[0] = 0;
    load_page();
}

static int num_pages(void) { return SM.total > 0 ? (SM.total + PAGE_SIZE - 1) / PAGE_SIZE : 0; }

static void fail_text(const char *prefix, const int status, const cJSON *error) {
    snprintf(SM.error, sizeof(SM.error), "%s%s", prefix, v10_gapi_error_text(error, status ? "Request failed" : "Failed to fetch"));
}

static void on_answer(void *user, const int status, const cJSON *data, const cJSON *error) {
    Req *r = user;
    const int ok = status >= 200 && status < 300 && !error;
    if (r->gen != SM.gen || !SM.depth) {
        free(r);
        return;
    }
    switch (r->kind) {
    case R_LIST:
        if (r->seq != SM.list_seq) break; // a newer page / filter was asked for
        SM.loading = 0;
        if (ok) {
            cJSON_Delete(SM.rows);
            const cJSON *skins = cJSON_GetObjectItemCaseSensitive(data, "skins");
            SM.rows = cJSON_IsArray(skins) ? cJSON_Duplicate(skins, 1) : cJSON_CreateArray();
            SM.total = (int) jnum(data, "total");
            SM.fetched_at = now_ms();
        } // else performFetch's catch: the cached rows (none once invalidated); the table swallows the error
        else if (!SM.rows) SM.rows = cJSON_CreateArray();
        break;
    case R_SAVE:
        SM.busy = 0;
        if (ok) {
            const cJSON *skin = cJSON_GetObjectItemCaseSensitive(data, "skin");
            if (SM.editing && !editing_id() && cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(skin, "skinId")))
                set_number(SM.editing, "skinId", jnum(skin, "skinId")); // the new skin's id: Save now updates it
            SM.saved = 1;
            invalidate();
        } else fail_text("Failed to save skin: ", status, error);
        break;
    case R_DELETE:
        SM.busy = 0;
        if (ok) invalidate();
        else fail_text("Failed to delete skin: ", status, error);
        break;
    case R_DUPLICATE:
        SM.busy = 0;
        if (ok) invalidate();
        else fail_text("Failed to duplicate skin: ", status, error);
        break;
    default: break;
    }
    free(r);
    render();
}

// ------------------------------------------------------------------ markup

static void disabled_attr(StrBuf *sb, const int off) {
    if (off) sb_append(sb, " disabled");
}

// the item card (iîíìîïi, market-card svelte-r5taai) of a custom skin: its customThumbnail, else the type's preview
// (iíïíîïì's thumbnail, :120603-120605: textures/previews/weapons/<src>.png or cosmetics/<type>_default.png)
static void skin_card(StrBuf *sb, const cJSON *skin, const int size, const int info) {
    const int type = (int) jnum(skin, "type"), weapon = (int) jnum(skin, "weapon"), rarity = (int) jnum(skin, "rarity");
    const char *color = rarity_color(rarity);
    char path[160], url[512];
    const Weapon *w = type == 0 && weapon >= 1 ? rules_weapon(weapon - 1) : NULL;
    if (type == 0) snprintf(path, sizeof(path), "textures/previews/weapons/%s.png", w && w->src ? w->src : "weapon_1");
    else snprintf(path, sizeof(path), "textures/previews/cosmetics/%d_default.png", type);
    html_dom_asset_url(path, url, sizeof(url));
    sb_appendf(sb, "<div class=\"market-card svelte-r5taai%s\" role=\"button\" tabindex=\"0\" style=\"border: 2px solid %s; width: %dpx; height: %dpx;\">",
               size <= 80 ? " tiny" : "", color, size, size);
    sb_appendf(sb, "<img draggable=\"false\" loading=\"lazy\" class=\"itemImg marketImg svelte-r5taai\" src=\"%s\" alt=\"", url);
    sb_text(sb, jstr(skin, "name"));
    sb_append(sb, "\"> <!---->");
    if (info) {
        sb_appendf(sb, " <div class=\"itemInfo svelte-r5taai\"><!----> <div class=\"itemName svelte-r5taai\" style=\"color: %s;\">", color);
        sb_text(sb, jstr(skin, "name")[0] ? jstr(skin, "name") : "Unnamed Item");
        sb_append(sb, "</div> <!----> <div class=\"itemType svelte-r5taai\">");
        sb_text(sb, type_label(type, weapon));
        sb_append(sb, "</div> <!----></div>");
    }
    sb_append(sb, " <!----></div>");
}

static void thumbnail(StrBuf *sb, const cJSON *skin) {
    const cJSON *cfg = cJSON_GetObjectItemCaseSensitive(skin, "itemConfig");
    const cJSON *custom = cJSON_GetObjectItemCaseSensitive(cfg, "customThumbnail");
    if (custom && !cJSON_IsFalse(custom) && !cJSON_IsNull(custom)) {
        char path[96], url[512];
        snprintf(path, sizeof(path), "skins/%lld/thumbnail.png", (long long) jnum(skin, "skinId"));
        html_dom_asset_url(path, url, sizeof(url));
        sb_appendf(sb, "<img class=\"item-thumbnail svelte-103lu6x\" width=\"56\" height=\"56\" src=\"%s\" alt=\"", url);
        sb_text(sb, jstr(skin, "name"));
        sb_append(sb, "\">");
    } else skin_card(sb, skin, 56, 0);
}

static void table_markup(StrBuf *sb) {
    sb_append(sb, "<div class=\"table-container svelte-12kndpz\"><div class=\"table-wrapper svelte-12kndpz\"><table class=\"table svelte-12kndpz\">"
                  "<thead class=\"svelte-12kndpz\"><tr><th class=\"item-column svelte-103lu6x\">Item</th> <th class=\"name-column svelte-103lu6x\">Name</th> "
                  "<th class=\"actions-column svelte-103lu6x\">Actions</th></tr></thead><tbody class=\"svelte-12kndpz\" id=\"knSmRows\">");
    const int nrows = cJSON_GetArraySize(SM.rows);
    if (SM.loading)
        sb_append(sb, "<tr class=\"svelte-12kndpz\"><td colspan=\"100%\" class=\"loading svelte-12kndpz\"><div class=\"loading-spinner svelte-12kndpz\">Loading...</div></td></tr>");
    else if (nrows == 0)
        sb_append(sb, "<tr class=\"svelte-12kndpz\"><td colspan=\"100%\" class=\"empty svelte-12kndpz\" id=\"knSmEmpty\">No data available</td></tr>");
    else {
        const int off = SM.busy || SM.editing;
        const cJSON *skin;
        cJSON_ArrayForEach(skin, SM.rows) {
            const long long id = (long long) jnum(skin, "skinId");
            sb_appendf(sb, "<tr class=\"svelte-12kndpz\" id=\"knSmRow%lld\"><td class=\"item-column svelte-103lu6x\">", id);
            thumbnail(sb, skin);
            sb_append(sb, "</td> <td class=\"name-column svelte-103lu6x\"><div class=\"item-name svelte-103lu6x\">");
            sb_text(sb, jstr(skin, "name"));
            sb_appendf(sb, " #%lld</div> <div class=\"item-type svelte-103lu6x\">", id);
            sb_text(sb, type_label((int) jnum(skin, "type"), (int) jnum(skin, "weapon")));
            sb_appendf(sb, "</div></td> <td class=\"actions-column svelte-103lu6x\"><div class=\"action-buttons svelte-103lu6x\">"
                           "<button class=\"action-btn test-btn svelte-103lu6x\" id=\"knSmTest%lld\" onclick=\"knSkins('test',%lld)\"", id, id);
            disabled_attr(sb, SM.busy);
            sb_appendf(sb, ">Test</button> <button class=\"action-btn edit-btn svelte-103lu6x\" id=\"knSmEdit%lld\" onclick=\"knSkins('edit',%lld)\"", id, id);
            disabled_attr(sb, off);
            sb_appendf(sb, "><i class=\"material-icons svelte-103lu6x\">edit</i></button> <button class=\"action-btn duplicate-btn svelte-103lu6x\" "
                           "id=\"knSmDuplicate%lld\" onclick=\"knSkins('duplicate',%lld)\"", id, id);
            disabled_attr(sb, off);
            sb_appendf(sb, "><i class=\"material-icons svelte-103lu6x\">content_copy</i></button> <button class=\"action-btn delete-btn svelte-103lu6x\" "
                           "id=\"knSmDelete%lld\" onclick=\"knSkins('delete',%lld)\"", id, id);
            disabled_attr(sb, off);
            sb_append(sb, "><i class=\"material-icons svelte-103lu6x\">delete</i></button></div></td></tr>");
        }
    }
    sb_append(sb, "</tbody></table></div> ");
    const int pages = num_pages();
    if (pages > 1) {
        sb_append(sb, "<div class=\"pagination svelte-12kndpz\"><button class=\"pagination-btn svelte-12kndpz\" id=\"knSmPrev\" onclick=\"knSkins('page',-1)\"");
        disabled_attr(sb, SM.page == 1);
        sb_appendf(sb, ">Previous</button> <div class=\"pagination-info svelte-12kndpz\" id=\"knSmPageInfo\">Page %d of %d (%d items)</div> "
                       "<button class=\"pagination-btn svelte-12kndpz\" id=\"knSmNext\" onclick=\"knSkins('page',1)\"", SM.page, pages, SM.total);
        disabled_attr(sb, SM.page == pages);
        sb_append(sb, ">Next</button></div>");
    }
    sb_append(sb, "</div>");
}

static void general_markup(StrBuf *sb) {
    const int type = (int) jnum(SM.editing, "type"), weapon = (int) jnum(SM.editing, "weapon"), rarity = (int) jnum(SM.editing, "rarity");
    sb_append(sb, "<div class=\"general-tab svelte-iy81sy\"><div class=\"general-content svelte-iy81sy\"><div class=\"form-group form-row svelte-iy81sy\">"
                  "<label for=\"skin-id\" class=\"svelte-iy81sy\">ID:</label> ");
    if (editing_id()) sb_appendf(sb, "<span id=\"skin-id\" class=\"svelte-iy81sy\">%lld</span>", (long long) editing_id());
    else sb_append(sb, "<span id=\"skin-id\" class=\"svelte-iy81sy\">No ID</span>");
    sb_append(sb, "</div> <div class=\"form-group form-row svelte-iy81sy\"><label for=\"skin-name\" class=\"svelte-iy81sy\">Name:</label> "
                  "<input id=\"skin-name\" type=\"text\" class=\"form-input svelte-iy81sy\" value=\"");
    sb_text(sb, jstr(SM.editing, "name"));
    sb_append(sb, "\" oninput=\"knSkins('input','name')\"></div> <div class=\"form-group form-row svelte-iy81sy\"><label for=\"skin-type\" class=\"svelte-iy81sy\">Type:</label> "
                  "<select id=\"skin-type\" class=\"form-select svelte-iy81sy\" onchange=\"knSkins('set','type',this.value)\">");
    for (int i = 0; i < TYPE_COUNT; i++)
        sb_appendf(sb, "<option class=\"svelte-iy81sy\" value=\"%d\"%s>%s</option>", i, i == type ? " selected" : "", TYPE_NAMES[i]);
    sb_append(sb, "</select></div> ");
    if (type == 0) {
        sb_append(sb, "<div class=\"form-group form-row svelte-iy81sy\"><label for=\"skin-weapon\" class=\"svelte-iy81sy\">Weapon:</label> "
                      "<select id=\"skin-weapon\" class=\"form-select svelte-iy81sy\" onchange=\"knSkins('set','weapon',this.value)\">");
        const int count = rules_krunker_weapon_count();
        for (int i = 0; i < count; i++) {
            const Weapon *w = rules_weapon(i);
            if (!w || !weapon_listed(i + 1)) continue;
            sb_appendf(sb, "<option class=\"svelte-iy81sy\" value=\"%d\"%s>", i + 1, i + 1 == weapon ? " selected" : "");
            sb_text(sb, w->name);
            sb_append(sb, "</option>");
        }
        sb_append(sb, "</select></div>");
    }
    sb_append(sb, " <div class=\"form-group form-row svelte-iy81sy\"><label for=\"skin-rarity\" class=\"svelte-iy81sy\">Rarity:</label> "
                  "<select id=\"skin-rarity\" class=\"form-select svelte-iy81sy\" onchange=\"knSkins('set','rarity',this.value)\">");
    for (int i = 0; i < g_viewer_rarity_count; i++)
        sb_appendf(sb, "<option class=\"svelte-iy81sy\" value=\"%d\" style=\"color: %s\"%s>%s</option>", i, rarity_color(i), i == rarity ? " selected" : "",
                   g_viewer_rarities[i].name);
    sb_append(sb, "</select></div> <div class=\"preview-section svelte-iy81sy\"><div class=\"preview-item svelte-iy81sy\">");
    skin_card(sb, SM.editing, 136, 1);
    sb_append(sb, "</div></div></div></div>");
}

// the Appearance tab's "Same attributes as" (every section but this one needs an upload or the 3D preview)
static void appearance_markup(StrBuf *sb) {
    const int type = (int) jnum(SM.editing, "type"), weapon = (int) jnum(SM.editing, "weapon");
    sb_append(sb, "<div class=\"appearance-tab svelte-wfs5t6\">");
    if (type != T_PAINT) {
        const cJSON *copy = cJSON_GetObjectItemCaseSensitive(editing_config(), "copyId");
        const int sel = cJSON_IsNumber(copy) ? (int) copy->valuedouble : -1;
        sb_append(sb, "<div class=\"section same-attributes-section svelte-wfs5t6\"><h3 class=\"section-title svelte-wfs5t6\"><i class=\"material-icons svelte-wfs5t6\">"
                      "content_copy</i> Same attributes as</h3> <div class=\"section-content svelte-wfs5t6\"><p class=\"section-description svelte-wfs5t6\">Select a "
                      "base skin to copy attributes from. The selected skin's properties will be used as defaults for this\n        skin.</p> "
                      "<div class=\"copy-id-selector svelte-wfs5t6\"><label for=\"copy-id-select\" class=\"svelte-wfs5t6\">Base Skin:</label> "
                      "<select id=\"copy-id-select\" class=\"form-select svelte-wfs5t6\" onchange=\"knSkins('set','copyId',this.value)\">");
        sb_appendf(sb, "<option class=\"svelte-wfs5t6\" value=\"default\"%s>Default</option>", sel < 0 ? " selected" : "");
        for (int i = 0; i < g_viewer_skin_count; i++) { // iÍiíííî: the catalogue's skins of the same type (a weapon skin: the same weapon)
            const ViewerSkin *s = viewer_skin(i);
            if (!s || !s->name) continue;
            if (type == 0 ? !(weapon && s->weapon == weapon && s->type == 0) : s->type != type) continue;
            sb_appendf(sb, "<option class=\"svelte-wfs5t6\" value=\"%d\"%s>", i, i == sel ? " selected" : "");
            sb_text(sb, s->name);
            sb_appendf(sb, " (#%d)</option>", i);
        }
        sb_append(sb, "</select></div> ");
        const ViewerSkin *base = sel >= 0 ? viewer_skin(sel) : NULL;
        if (base && base->name) {
            sb_append(sb, "<div class=\"current-selection svelte-wfs5t6\"><span class=\"selection-label svelte-wfs5t6\">Current selection:</span> "
                          "<span class=\"selection-value svelte-wfs5t6\">");
            sb_text(sb, base->name);
            sb_appendf(sb, " (#%d)</span></div>", sel);
        }
        sb_append(sb, "</div></div>");
    }
    sb_append(sb, "</div>");
}

static void advanced_markup(StrBuf *sb) {
    char *cfg = stringify(editing_config());
    char *autos = auto_attributes();
    sb_append(sb, "<div class=\"advanced-tab svelte-3rvofx\"><div class=\"advanced-section svelte-3rvofx\"><h3 class=\"section-title svelte-3rvofx\">"
                  "<i class=\"material-icons svelte-3rvofx\">code</i> Item Configuration (JSON)</h3> <textarea id=\"skin-config\" class=\"form-textarea svelte-3rvofx\" "
                  "rows=\"8\" placeholder=\"{&#10;  &quot;glow&quot;: true,&#10;  &quot;rgb&quot;: true,&#10;  &quot;animated&quot;: true,&#10;  &quot;trans&quot;: true&#10;}\" "
                  "oninput=\"knSkins('input','config')\">");
    sb_text(sb, cfg ? cfg : "{}");
    sb_append(sb, "</textarea> <div class=\"automatic-attributes svelte-3rvofx\"><div class=\"automatic-attributes-header svelte-3rvofx\"><div class=\"automatic-attributes-title svelte-3rvofx\">"
                  "<i class=\"material-icons svelte-3rvofx\">info</i> <h4 class=\"svelte-3rvofx\">Automatic Attributes</h4></div> <span class=\"info-text svelte-3rvofx\">These attributes "
                  "are automatically copied from the default/selected base skin. They can be overridden above.</span></div> <div class=\"automatic-attributes-content svelte-3rvofx\">"
                  "<pre class=\"svelte-3rvofx\" id=\"knSmAutoAttrs\">");
    sb_text(sb, autos ? autos : "{}");
    sb_append(sb, "</pre></div></div></div></div>");
    free(cfg);
    free(autos);
}

static void editor_markup(StrBuf *sb) {
    static const char *const tabs[] = {"general", "appearance", "advanced"};
    static const char *const icons[] = {"settings", "palette", "code"};
    static const char *const labels[] = {"General", "Appearance", "Advanced"};
    sb_append(sb, "<div class=\"skin-editor svelte-103lu6x\"><div class=\"tab-navigation svelte-103lu6x\">");
    for (int i = 0; i < 3; i++)
        sb_appendf(sb, "%s<button class=\"tab-button svelte-103lu6x%s\" id=\"knSmTab%d\" onclick=\"knSkins('tab','%s')\"><i class=\"material-icons svelte-103lu6x\">%s</i> %s</button>",
                   i ? " " : "", SM.tab == i ? " active" : "", i, tabs[i], icons[i], labels[i]);
    sb_append(sb, "</div> <div class=\"tab-content-container svelte-103lu6x\"><div class=\"tab-content svelte-103lu6x\">");
    if (SM.tab == TAB_GENERAL) general_markup(sb);
    else if (SM.tab == TAB_APPEARANCE) appearance_markup(sb);
    else advanced_markup(sb);
    sb_append(sb, "</div> <!----></div></div>");
}

static void save_button(StrBuf *sb) {
    sb_append(sb, "<button class=\"io-action-bar-button svelte-103lu6x");
    if (SM.busy) sb_append(sb, " is-loading");
    sb_append(sb, "\" id=\"knSmSave\" onclick=\"knSkins('save')\"");
    disabled_attr(sb, SM.saved || SM.busy);
    sb_appendf(sb, "><i class=\"material-icons svelte-103lu6x\">%s</i> %s</button>", SM.busy ? "sync" : "save", SM.busy ? "Saving..." : "Save");
}

static void window_markup(StrBuf *sb) {
    sb_append(sb, "<div class=\"popup-container svelte-1wz7ymp\"><div class=\"popup svelte-1wz7ymp fullscreen\" style=\"\">"
                  "<div class=\"io-popup-container skin-manager-window svelte-103lu6x\" id=\"knSmWindow\"><div class=\"io-popup-header io-popup-header--slim svelte-103lu6x\">"
                  "<i class=\"material-icons skin-icon svelte-103lu6x\">palette</i> Skin Manager <button class=\"close-btn svelte-103lu6x\" title=\"Close\" aria-label=\"Close\" "
                  "id=\"knSmClose\" onclick=\"knSkins('close')\"><i class=\"material-icons svelte-103lu6x\">close</i></button></div> <div class=\"io-action-bar svelte-103lu6x\">");
    if (SM.editing) {
        sb_append(sb, "<button class=\"io-action-bar-button svelte-103lu6x\" id=\"knSmBack\" onclick=\"knSkins('back')\"");
        disabled_attr(sb, SM.busy);
        sb_append(sb, "><i class=\"material-icons svelte-103lu6x\">arrow_back</i> Back</button> ");
        save_button(sb);
        sb_append(sb, " ");
        const int type = (int) jnum(SM.editing, "type");
        if (type != T_PLAYER_CARD && type != T_EMOJI) {
            sb_appendf(sb, "<button class=\"io-action-bar-button svelte-103lu6x\" id=\"knSmCapture\" onclick=\"knSkins('capture')\" title=\"%s\"",
                       editing_id() ? "Position the preview, then capture it as the item thumbnail" : "Save the skin first");
            disabled_attr(sb, SM.busy || !editing_id());
            sb_append(sb, "><i class=\"material-icons svelte-103lu6x\">photo_camera</i> Capture Thumbnail</button>");
        }
    } else {
        sb_append(sb, "<div class=\"search-container svelte-103lu6x\"><input type=\"text\" placeholder=\"Search by name or ID...\" class=\"search-input svelte-103lu6x\" "
                      "id=\"knSmSearch\" value=\"");
        sb_text(sb, SM.search);
        sb_append(sb, "\" oninput=\"knSkins('input','search')\"");
        disabled_attr(sb, SM.busy);
        sb_append(sb, "> ");
        if (SM.search[0]) {
            sb_append(sb, "<button class=\"search-clear svelte-103lu6x\" id=\"knSmSearchClear\" onclick=\"knSkins('clear')\"");
            disabled_attr(sb, SM.busy);
            sb_append(sb, "><i class=\"material-icons svelte-103lu6x\">close</i></button>");
        }
        sb_append(sb, "</div> <button class=\"io-action-bar-button svelte-103lu6x\" id=\"knSmAdd\" onclick=\"knSkins('add')\"");
        disabled_attr(sb, SM.busy);
        sb_append(sb, "><i class=\"material-icons svelte-103lu6x\">add</i> Add new skin</button>");
    }
    sb_append(sb, "</div> ");
    if (SM.error[0]) {
        sb_append(sb, "<div class=\"error-message svelte-103lu6x\" id=\"knSmError\"><i class=\"material-icons svelte-103lu6x\">error</i> ");
        sb_text(sb, SM.error);
        sb_append(sb, " <button class=\"svelte-103lu6x\" id=\"knSmErrorClose\" onclick=\"knSkins('dismiss')\">\xc3\x97</button></div>");
    }
    sb_append(sb, " <div class=\"io-popup-content svelte-103lu6x\"><div class=\"skin-manager-table svelte-103lu6x\">");
    if (SM.editing) editor_markup(sb);
    else table_markup(sb);
    sb_append(sb, "</div> <!----></div></div></div></div>");
}

// re-rendering replaces the markup: keep the focused field and its caret
static void render(void) {
    if (!SM.depth || v10_view_depth() != SM.depth) return;
    StrBuf sb = {0};
    window_markup(&sb);
    char focus[48] = "";
    int c0 = 0, c1 = 0, caret = 0;
    const HtmlElem a = html_dom_active_element();
    if (a && html_dom_get_attr(a, "id", focus, sizeof(focus)) >= 0 && (!strncmp(focus, "knSm", 4) || !strncmp(focus, "skin-", 5)))
        caret = html_dom_text_selection(a, &c0, &c1);
    else focus[0] = 0;
    v10_view_set(sb_finish(&sb));
    if (focus[0]) {
        const HtmlElem e = html_dom_by_id(focus);
        if (e) {
            html_dom_focus(e);
            if (caret) html_dom_text_set_selection(e, c0, c1);
        }
    }
}

// ------------------------------------------------------------------ the view stack

static void reset(void) {
    cJSON_Delete(SM.rows);
    cJSON_Delete(SM.editing);
    const unsigned gen = SM.gen + 1;
    memset(&SM, 0, sizeof(SM));
    SM.gen = gen;
}

static void window_closed(const int accepted, void *user) {
    (void) accepted;
    (void) user;
    reset();
}

static void open_window(void) { // window.showSkinManager
    if (SM.depth) {
        while (v10_view_depth() > SM.depth) v10_view_pop(0);
        render();
        return;
    }
    reset();
    v10_view_push(strdup(""), render, window_closed, NULL);
    SM.depth = v10_view_depth();
    SM.page = 1;
    SM.saved = 1;
    load_page();
    render();
}

static void confirm_closed(const int accepted, void *user) { // iÌíîïîï after iìiîíïî's answer
    (void) user;
    const int64_t id = SM.confirm_id;
    SM.confirm_depth = 0;
    SM.confirm_id = 0;
    if (accepted && SM.depth && id > 0) {
        char path[48];
        snprintf(path, sizeof(path), "/skins/%lld", (long long) id);
        SM.busy = 1;
        SM.error[0] = 0;
        send_req("DELETE", path, NULL, R_DELETE);
    }
    render();
}

// iìiîíïî (:104114): the io-popup confirm with v10's "Delete Skin" texts
static void push_confirm(const int64_t id) {
    StrBuf sb = {0};
    sb_append(&sb, "<div class=\"popup-container svelte-1wz7ymp\"><div class=\"popup svelte-1wz7ymp\" style=\"width: 800px;\"><div class=\"io-popup-container\">"
                   "<div class=\"io-popup-header\">Delete Skin</div> <div class=\"io-popup-content\"><div class=\"io-card\"><p>Are you sure you want to delete this skin?</p> "
                   "<form><div class=\"io-input-container\"><div class=\"io-button-group\"><button type=\"button\" class=\"io-button\" id=\"knSmConfirmCancel\" "
                   "onclick=\"knViewPop(0)\">Cancel</button> <button type=\"button\" class=\"io-button io-button--accept\" id=\"knSmConfirm\" onclick=\"knViewPop(1)\">"
                   "Delete</button></div></div></form></div></div></div></div></div>");
    SM.confirm_id = id;
    v10_view_push(sb_finish(&sb), NULL, confirm_closed, NULL);
    SM.confirm_depth = v10_view_depth();
}

static const cJSON *row_by_id(const int64_t id) {
    const cJSON *skin;
    cJSON_ArrayForEach(skin, SM.rows) if ((int64_t) jnum(skin, "skinId") == id) return skin;
    return NULL;
}

// ------------------------------------------------------------------ input

static void set_disabled(const char *id, const int off) {
    const HtmlElem e = html_dom_by_id(id);
    if (!e) return;
    if (off) html_dom_set_attr(e, "disabled", "");
    else html_dom_remove_attr(e, "disabled");
}

static void changed(void) { // onSkinChange (iÍíìîïí): the skin is unsaved
    SM.saved = 0;
}

// value: the inline handler's third argument when it has one (a scripted call), else this.value
static void on_input(const char *what, const HtmlEvent *ev, const char *value) {
    const char *v = value && value[0] ? value : ev && ev->value ? ev->value : "";
    if (!strcmp(what, "search")) {
        snprintf(SM.search, sizeof(SM.search), "%s", v);
        SM.search_due = now_ms() + SEARCH_DEBOUNCE_MS;
        const HtmlElem clear = html_dom_by_id("knSmSearchClear");
        if (!clear != !SM.search[0]) render(); // the clear button comes or goes
        return;
    }
    if (!SM.editing) return;
    if (!strcmp(what, "name")) {
        cJSON_DeleteItemFromObjectCaseSensitive(SM.editing, "name");
        cJSON_AddStringToObject(SM.editing, "name", v);
        changed();
        set_disabled("knSmSave", SM.busy);
    } else if (!strcmp(what, "config")) { // iìîîííi: a text that parses replaces itemConfig; else nothing changes
        cJSON *cfg = cJSON_Parse(v);
        if (!cfg) return;
        cJSON_ReplaceItemInObjectCaseSensitive(SM.editing, "itemConfig", cfg);
        changed();
        set_disabled("knSmSave", SM.busy);
        const HtmlElem pre = html_dom_by_id("knSmAutoAttrs");
        char *autos = pre ? auto_attributes() : NULL;
        if (autos) {
            StrBuf sb = {0};
            sb_text(&sb, autos);
            char *html = sb_finish(&sb);
            html_dom_set_html(pre, html);
            free(html);
            free(autos);
        }
    }
}

// a General / Appearance select: type and weapon drop itemConfig.copyId (iIíìíîí, :125108-125121)
static void on_set(const char *what, const char *value) {
    if (!SM.editing) return;
    if (!strcmp(what, "copyId")) {
        cJSON *cfg = editing_config();
        cJSON_DeleteItemFromObjectCaseSensitive(cfg, "copyId");
        if (value[0] && strcmp(value, "default")) {
            char *end = NULL;
            const long n = strtol(value, &end, 10);
            if (end != value) cJSON_AddNumberToObject(cfg, "copyId", (double) n);
        }
    } else if (!strcmp(what, "type") || !strcmp(what, "weapon") || !strcmp(what, "rarity")) {
        set_number(SM.editing, what, (double) atoi(value));
        if (strcmp(what, "rarity")) cJSON_DeleteItemFromObjectCaseSensitive(editing_config(), "copyId");
    } else return;
    changed();
}

static void do_save(void) { // iÏíìíïi
    if (SM.busy || !SM.editing) return;
    SM.busy = 1;
    SM.error[0] = 0;
    const int64_t id = editing_id();
    if (id) {
        char path[48];
        snprintf(path, sizeof(path), "/skins/%lld", (long long) id);
        send_req("PUT", path, cJSON_Duplicate(SM.editing, 1), R_SAVE);
    } else send_req("POST", "/skins", cJSON_Duplicate(SM.editing, 1), R_SAVE);
}

static void do_edit(const int64_t id) { // iÍiíiíi: a copy of the row with a deep-copied itemConfig
    const cJSON *row = row_by_id(id);
    if (!row) return;
    cJSON *e = cJSON_CreateObject();
    cJSON_AddNumberToObject(e, "skinId", jnum(row, "skinId"));
    cJSON_AddStringToObject(e, "name", jstr(row, "name"));
    cJSON_AddNumberToObject(e, "type", jnum(row, "type"));
    cJSON_AddNumberToObject(e, "weapon", jnum(row, "weapon"));
    cJSON_AddNumberToObject(e, "rarity", jnum(row, "rarity"));
    const cJSON *cfg = cJSON_GetObjectItemCaseSensitive(row, "itemConfig");
    cJSON_AddItemToObject(e, "itemConfig", cJSON_IsObject(cfg) ? cJSON_Duplicate(cfg, 1) : cJSON_CreateObject());
    cJSON_Delete(SM.editing);
    SM.editing = e;
    SM.saved = 1;
}

static void do_add(void) { // iìiîiìí: a new skin, then a save (the editor opens on it once it has an id)
    cJSON *e = cJSON_CreateObject();
    cJSON_AddNullToObject(e, "skinId");
    cJSON_AddStringToObject(e, "name", "New Skin");
    cJSON_AddNumberToObject(e, "type", 0);
    cJSON_AddNumberToObject(e, "weapon", 1);
    cJSON_AddNumberToObject(e, "rarity", 0);
    cJSON_AddObjectToObject(e, "itemConfig");
    cJSON_Delete(SM.editing);
    SM.editing = e;
    SM.tab = TAB_GENERAL;
    do_save();
}

static int64_t arg_i64(const HtmlArg *args, const int argc, const int i) {
    if (i >= argc) return 0;
    if (args[i].kind == HTML_ARG_NUM || args[i].kind == HTML_ARG_BOOL) return (int64_t) args[i].num;
    if (args[i].kind == HTML_ARG_STR && args[i].str) return strtoll(args[i].str, NULL, 10);
    return 0;
}

static const char *arg_s(const HtmlArg *args, const int argc, const int i) {
    return i < argc && args[i].kind == HTML_ARG_STR && args[i].str ? args[i].str : "";
}

int v10_skin_manager_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    if (!name) return -1;
    if (!strcmp(name, "showSkinManager")) {
        open_window();
        return 1;
    }
    if (strcmp(name, "knSkins")) return -1;
    if (!SM.depth) return 1;
    const char *op = arg_s(args, argc, 0);
    const int64_t a1 = arg_i64(args, argc, 1);
    if (!strcmp(op, "input")) {
        on_input(arg_s(args, argc, 1), ev, arg_s(args, argc, 2));
        return 1;
    }
    if (!strcmp(op, "close")) { // iïìiïîï: the view stack's pop
        if (v10_view_depth() == SM.depth) v10_view_pop(0);
        return 1;
    }
    if (!strcmp(op, "set")) {
        char value[32];
        const int64_t n = arg_i64(args, argc, 2);
        if (argc > 2 && args[2].kind == HTML_ARG_STR) snprintf(value, sizeof(value), "%s", arg_s(args, argc, 2));
        else snprintf(value, sizeof(value), "%lld", (long long) n);
        on_set(arg_s(args, argc, 1), value);
    } else if (!strcmp(op, "tab")) { // iÏîïîïí
        const char *t = arg_s(args, argc, 1);
        SM.tab = !strcmp(t, "appearance") ? TAB_APPEARANCE : !strcmp(t, "advanced") ? TAB_ADVANCED : TAB_GENERAL;
    } else if (!strcmp(op, "back")) { // iìiiíîi
        cJSON_Delete(SM.editing);
        SM.editing = NULL;
        SM.saved = 1;
        SM.error[0] = 0;
    } else if (!strcmp(op, "save")) {
        if (SM.saved) return 1;
        do_save();
    } else if (!strcmp(op, "capture")) { // iììííií: the showcase iframe captures; Box Office has none
        if (!editing_id()) snprintf(SM.error, sizeof(SM.error), "Please save the skin first before capturing a thumbnail");
        else snprintf(SM.error, sizeof(SM.error), "Box Office has no 3D preview to capture a thumbnail from");
    } else if (!strcmp(op, "clear")) { // iiïììîi: the field and (at once) the filter
        SM.search[0] = SM.filter[0] = 0;
        SM.search_due = 0;
        SM.key[0] = 0;
        load_page();
    } else if (!strcmp(op, "dismiss")) { // iìîiîíï
        SM.error[0] = 0;
    } else if (!strcmp(op, "add")) {
        if (SM.busy) return 1;
        do_add();
    } else if (!strcmp(op, "edit")) {
        if (SM.busy || SM.editing) return 1;
        do_edit(a1);
    } else if (!strcmp(op, "duplicate")) { // iIìííîï
        if (SM.busy || SM.editing || a1 <= 0) return 1;
        char path[64];
        snprintf(path, sizeof(path), "/skins/%lld/duplicate", (long long) a1);
        SM.busy = 1;
        SM.error[0] = 0;
        send_req("POST", path, NULL, R_DUPLICATE);
    } else if (!strcmp(op, "delete")) {
        if (SM.busy || SM.editing || a1 <= 0) return 1;
        push_confirm(a1);
        return 1;
    } else if (!strcmp(op, "test")) { // testSkinSandbox (:204469): close the view, the windows, enter the game
        if (SM.busy) return 1;
        v10_view_pop(0);
        menu_toggle_window(0);
        menu_play();
        return 1;
    } else if (!strcmp(op, "page")) { // the table's Previous / Next
        const int pages = num_pages();
        const int next = SM.page + (a1 < 0 ? -1 : 1);
        if (next < 1 || next > pages) return 1;
        SM.page = next;
        load_page();
    }
    render();
    return 1;
}

int v10_skin_manager_key(const int key) {
    if (!html_key_is_escape(key) || !SM.depth) return 0;
    const int top = v10_view_depth();
    if (top == SM.confirm_depth || top == SM.depth) {
        v10_view_pop(0);
        return 1;
    }
    return 0;
}

void v10_skin_manager_tick(void) {
    if (!SM.depth || SM.search_due <= 0 || now_ms() < SM.search_due) return;
    SM.search_due = 0;
    char t[sizeof(SM.filter)];
    trimmed(t, sizeof(t), SM.search);
    if (!strcmp(t, SM.filter)) return;
    snprintf(SM.filter, sizeof(SM.filter), "%s", t); // setFilters: a new search refreshes the table
    SM.key[0] = 0;
    load_page();
    render();
}

int v10_skin_manager_open(void) { return SM.depth > 0; }
