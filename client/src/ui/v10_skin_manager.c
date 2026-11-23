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
// The editor's preview pane (:124686-124704, :127463-127540): the viewer page in an iframe (#knSmPreview) whose
// query is the edited skin's game skin as JSON (iíïíîïì, 300 ms after a change), drawn natively by viewer_embed.h; a
// player card / emoji shows its texture or thumbnail instead. Capture Thumbnail (:124566-124570, :124764-124790) takes
// that showcase as a 256 px PNG (viewer_embed_capture), uploads it as the thumbnail, sets itemConfig.customThumbnail and
// saves the skin. The Appearance tab's Model, Textures (diffuse / emissive, Transparency, Pulse), Sounds (shoot / swap
// / inspect, Loop while inspecting), Dye Colors and Shader Studio sections (:125300-127400) upload through
// uploadAsset (:124434-124459): a file picked in the platform dialog (ed_pick_file, the hidden <input type=file>'s
// place) is checked as v10's drop zone checks it (svelte-145grbc: type, size), then goes to the brain as the gapi
// key + chunks of Box Office's asset uploads (POST /skins/:id/upload, then uploadUrl, services/brain/asset_upload.c);
// once stored it is also the local copy at once (asset_fetch_client_put), as a browser would load it again. Test
// (testSkinSandbox :204469-204590) closes the window, enters the sandbox game and, 50 ms on, defines the skin
// (skinManager.setSkin: viewer_skin_define) and puts it on the local player's slot of its type (a weapon skin: the
// class whose primary it is), then rebuilds the player's meshes.
//
// The Appearance tab's Paintable (paintTargets, the paint mask texture_p.png), Paint (a paint's colour or its node graph,
// previewed on paintPreviewBase), Wear (four layers packed into texture_w.png), Kounter (itemConfig.attach) and Charm
// (charmM{X,Y,Z}Off / charmMScl, noCharm) sections and the preview's Wear slider / paint pick are v10's
// (:126048-126978, :127496-127530). Shader Studio's node canvas is sgEditor (shader_graph_ui.c); its graph compiles to
// the preview's and the game's material (skin_fx.c, mod_shader_graph.c). An OGG sound plays as MP3 and WAV do
// (miniaudio with stb_vorbis, client/src/miniaudio.c).
// Not ported: the preview's Kounter / charm tools (Snap to surface, the Move / Rotate / Scale gizmo: the showcase
// iframe's attachEdit / charmEdit messages); the fields set the placement. A paint's node graph is saved, but the
// paint preview draws only a solid colour (the viewer's &paint=).
// A krunker.io account's skins are krunker.io's: v10_gapi sends no write there (read-only), so its uploads fail as its
// saves do.
#include <v10_skin_manager.h>
#include <v10_social.h>
#include <v10_ui.h>
#include <backend.h> // the upload chunks (backend_http_post_bytes)
#include <backend_ui.h>
#include <html_ui.h>
#include <ruleset.h>
#include <ui_logic.h>
#include <weapons.h>
#include "../viewer/viewer_store.h" // the catalogue's base skins and the rarity colours
#include <asset_fetch.h>   // asset_fetch_client_put: an uploaded file is the local copy at once
#include <client.h>
#include <png_write.h>     // the captured thumbnail
#include <user_asset_path.h> // user_asset_rel_is_kn: a Box Office skin's files are the brain's
#include <viewer_embed.h>  // the preview iframe's showcase and its capture
#include <shader_graph_ui.h> // Shader Studio's node canvas (sgEditor)
#include <stb_image.h>     // the wear layers (the canvas drawImage of iÏiìíiì)
#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MSG 320
#define SM_PAGE_SIZE 10       // the table's pageSize default (:125010)
#define CACHE_MS 30000.0      // the provider's cache lifetime
#define SEARCH_DEBOUNCE_MS 300.0
#define PREVIEW_DEBOUNCE_MS 300.0 // the preview JSON's setTimeout (:124687)
#define CAPTURE_SIZE 256          // capturePreview's size (:124567)
#define TEST_DELAY_MS 50.0        // testSkinSandbox's setTimeout (:204473)
#define TEST_WAIT_MS 10000.0      // KrunkNative: how long Test waits for the local player / its new class
#define CHUNK (512u * 1024u)      // kn_brain's request bodies are 1 MiB at most (mods_ui.c's uploads)
#define CHUNK_TIMEOUT_MS 60000
#define SLOT_ERRORS 16

// the platform file dialog (client/src/editor/editor_tools.c; editor_internal.h pulls in GL)
typedef void (*EdFileFn)(void *user, const char *path);
void ed_pick_file(const char *accept, EdFileFn fn, void *user);

enum { TAB_GENERAL, TAB_APPEARANCE, TAB_ADVANCED };
enum { R_LIST, R_SAVE, R_DELETE, R_DUPLICATE, R_THUMB };
enum { T_PLAYER_CARD = 16, T_EMOJI = 17, T_PAINT = 18 };

typedef struct {
    char slot[24];
    char upload_error[200], drop_error[200], file_name[128];
} SlotState;

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
    // the preview pane (iÍìïïíî): the game skin's JSON for the iframe's query, set 300 ms after a change
    char *preview;
    double preview_due;
    unsigned asset_rev; // the skin files' ?v= (userAssetsUrl's Date.now(), :24298-24300): bumped by each upload
    int capturing, captured; // iÍiíiìî, iÍìîiiï
    // the Appearance tab: textureSettings.hasGlow, the model picked this session (iÏíïííi: its extension), the
    // sections' upload errors and the drop zones' own state (a picked file's name, a refused file's message)
    int has_glow;
    char model_ext[8];
    SlotState slots[SLOT_ERRORS];
    // the upload running (one at a time): the gapi key, then the chunks to uploadUrl, then done
    struct {
        int active;
        unsigned gen;
        char slot[24], type[16], category[16], file_name[128];
        int64_t skin;
        unsigned char *data;
        size_t len, sent;
        int done_sent;
        char file_id[96], base[1200];
    } up;
    // Shader Studio (iÍíìiìî open, iIïïiìì legacy, iIiiíîì graph, iÎiiiîí applyToAll, iIíiîiî parts), per skin
    struct {
        int open, legacy, apply_all, preview;
        cJSON *graph, *parts;
        int64_t skin;
        int loaded;
        SgEditor *canvas; // the node canvas editor (kind "surface", onChange iÌiïíîi :127145)
        int render_due;   // the canvas changed the exposed params: the window again on the next tick
    } sh;
    // the Appearance tab's Paintable, Paint, Wear, Kounter and Charm sections and the preview's controls, per skin
    struct {
        int64_t skin;                // the skin they were set up for (another one: they start over)
        int ready;
        int wear_on;                 // iÎíïíiî: the Wear toggle
        unsigned char *layer[4];     // iÎïïíii: the picked layers (the files), NULL none
        size_t layer_len[4];
        int generating;              // iÏiîìïï
        char wear_error[200];        // iIïîïiï
        char targets[256];           // iIíìííi: the mesh names' text
        int attach_mode, charm_mode; // iIíîììî / iÍîïïíí: PLACE_TOOLS' index, 0 View
        SgEditor *paint_canvas;      // a paint's node canvas (kind "paint")
        int paint_base;              // paintPreviewBase (iÍìîîíi): the item the paint is previewed on
        int wear_open, wear_touched; // iIììììì, iÍïííîï: the preview's Wear slider shown / asked for
        double wear_level;           // iÎïîìïî
        int paint_pick;              // iÍìïíìî: the preview's paint (a catalogue index), -1 none
    } ed;
} SM;

// Test's pending swap (outlives the window, which Test closes): the skin, its game skin, when it is due
static struct {
    int64_t id;
    cJSON *skin;
    double due, until;
    int class_asked;
} g_test;

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

// o[k] = v (v owned; NULL deletes)
static void put(cJSON *o, const char *k, cJSON *v) {
    cJSON_DeleteItemFromObjectCaseSensitive(o, k);
    if (v) cJSON_AddItemToObject(o, k, v);
}

static int cfg_true(const cJSON *cfg, const char *k) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(cfg, k);
    return v && !cJSON_IsFalse(v) && !cJSON_IsNull(v) && !(cJSON_IsNumber(v) && v->valuedouble == 0) &&
           !(cJSON_IsString(v) && !v->valuestring[0]);
}

void v10_skin_file_url(const char *folder, const char *file, const unsigned rev, char *out, const size_t n) {
    char rel[160];
    snprintf(rel, sizeof(rel), "skins/%s/%s", folder ? folder : "", file ? file : "");
    if (user_asset_rel_is_kn(rel)) snprintf(out, n, "%s/user-assets/%s?v=%u", backend_url(), rel, rev);
    else snprintf(out, n, "https://user-assets.krunker.io/%s?v=%u", rel, rev);
}

static void skin_url(const char *file, char *out, const size_t n) {
    char folder[32];
    snprintf(folder, sizeof(folder), "%lld", (long long) editing_id());
    v10_skin_file_url(folder, file, SM.asset_rev, out, n);
}

// the Appearance tab's per-slot state ("model", "texture:diffuse", "sound:inspect", ...)
static SlotState *slot_state(const char *slot, const int make) {
    for (int i = 0; i < SLOT_ERRORS; i++)
        if (SM.slots[i].slot[0] && !strcmp(SM.slots[i].slot, slot)) return &SM.slots[i];
    if (!make) return NULL;
    for (int i = 0; i < SLOT_ERRORS; i++)
        if (!SM.slots[i].slot[0]) {
            memset(&SM.slots[i], 0, sizeof(SM.slots[i]));
            snprintf(SM.slots[i].slot, sizeof(SM.slots[i].slot), "%s", slot);
            return &SM.slots[i];
        }
    return NULL;
}

static const char *upload_error(const char *slot) {
    const SlotState *st = slot_state(slot, 0);
    return st ? st->upload_error : "";
}

static void set_upload_error(const char *slot, const char *msg) {
    SlotState *st = slot_state(slot, 1);
    if (st) snprintf(st->upload_error, sizeof(st->upload_error), "%s", msg ? msg : "");
}

static int uploading(const char *slot) { return SM.up.active && !strcmp(SM.up.slot, slot); }

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

// iììïïíî (:120558-120581): a custom skin's attributes from its base skin, AUTO_KEYS of skins.json
static void add_auto(cJSON *attrs, const cJSON *skin) {
    const cJSON *cfg = cJSON_GetObjectItemCaseSensitive(skin, "itemConfig");
    const int base = base_skin((int) jnum(skin, "type"), (int) jnum(skin, "weapon"), cfg, attrs);
    const cJSON *json = base >= 0 ? ui_loadout_skin_json(base) : NULL;
    for (size_t i = 0; json && i < sizeof(AUTO_KEYS) / sizeof(AUTO_KEYS[0]); i++) {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(json, AUTO_KEYS[i]);
        if (v) put(attrs, AUTO_KEYS[i], cJSON_Duplicate(v, 1));
    }
}

// the Advanced tab's Automatic Attributes: the base skin's AUTO_KEYS (skins.json), as the box prints them
static char *auto_attributes(void) {
    cJSON *attrs = cJSON_CreateObject();
    add_auto(attrs, SM.editing);
    char *text = stringify(attrs);
    cJSON_Delete(attrs);
    return text;
}

// iíïíîïì (:120593-120606): the game's skin of a custom skin: the base skin's attributes, {_id: skinId, name, type,
// weapon, rarity}, the itemConfig over them, then a template's (iîííiíì :120583-120591). rarity_meta (the rarity's
// row) is left out: the game reads the rarity itself here, and the preview drops it as v10's does.
static cJSON *game_skin(const cJSON *skin) {
    cJSON *g = cJSON_CreateObject();
    add_auto(g, skin);
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(skin, "skinId");
    put(g, "_id", cJSON_IsNumber(id) ? cJSON_CreateNumber(id->valuedouble) : cJSON_CreateNull());
    put(g, "name", cJSON_CreateString(jstr(skin, "name")));
    put(g, "type", cJSON_CreateNumber(jnum(skin, "type")));
    put(g, "weapon", cJSON_CreateNumber(jnum(skin, "weapon")));
    put(g, "rarity", cJSON_CreateNumber(jnum(skin, "rarity")));
    const cJSON *cfg = cJSON_GetObjectItemCaseSensitive(skin, "itemConfig"), *c;
    cJSON_ArrayForEach(c, cfg) if (c->string) put(g, c->string, cJSON_Duplicate(c, 1));
    const cJSON *tpl = cJSON_GetObjectItemCaseSensitive(g, "template");
    const cJSON *row = cJSON_IsNumber(tpl) && tpl->valuedouble >= 1 ? ui_loadout_skin_json((int) tpl->valuedouble) : NULL;
    for (size_t i = 0; row && i < sizeof(AUTO_KEYS) / sizeof(AUTO_KEYS[0]); i++) {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(row, AUTO_KEYS[i]);
        if (v) put(g, AUTO_KEYS[i], cJSON_Duplicate(v, 1));
    }
    return g;
}

// testSkinSandbox's customShaderInfo of an itemConfig.customShader graph (:204476-204508): "*" when it applies to
// the whole model (or names no part), else one entry per part; each {shaderDef, shaderArgs: {params}}
static cJSON *shader_info(const cJSON *shader) {
    const cJSON *graph = cJSON_GetObjectItemCaseSensitive(shader, "graph");
    if (strcmp(jstr(shader, "type"), "graph") || !cJSON_IsObject(graph)) return NULL;
    cJSON *info = cJSON_CreateObject();
    const cJSON *parts = cJSON_GetObjectItemCaseSensitive(shader, "parts");
    const int all = !cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(shader, "applyToAll")) || !cJSON_GetArraySize(parts);
    const cJSON *part = all ? NULL : parts->child;
    do {
        cJSON *e = cJSON_CreateObject();
        cJSON *def = cJSON_AddObjectToObject(e, "shaderDef");
        cJSON_AddStringToObject(def, "type", "graph");
        cJSON_AddItemToObject(def, "graph", cJSON_Duplicate(graph, 1));
        const cJSON *params = part ? cJSON_GetObjectItemCaseSensitive(part, "params") : NULL;
        if (cJSON_GetArraySize(params)) cJSON_AddItemToObject(cJSON_AddObjectToObject(e, "shaderArgs"), "params", cJSON_Duplicate(params, 1));
        cJSON_AddItemToObject(info, part ? part->string : "*", e);
        part = part ? part->next : NULL;
    } while (part);
    return info;
}

// encodeURIComponent
static void uri_component(StrBuf *sb, const char *s) {
    for (const unsigned char *p = (const unsigned char *) s; *p; p++) {
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || strchr("-_.!~*'()", *p))
            sb_appendf(sb, "%c", *p);
        else sb_appendf(sb, "%%%02X", *p);
    }
}

// the preview's showcase query value (:124688-124701): the game skin without name, rarity, rarity_meta and
// customShader, as encodeURIComponent(JSON.stringify(...)); with Shader Studio's Refresh Preview, the shader as
// customShaderInfo (the iframe's shaderStudioUpdate, :124651-124656). v10 leaves out attach and the charm offsets
// (iïiîïîï :120757) and posts them as attachEdit / charmEdit (:124719-124732); KrunkNative's iframe takes no
// messages, so they stay in the query (the showcase draws the Kounter, viewer_scene.c item_counter).
static char *preview_query(void) {
    cJSON *g = game_skin(SM.editing);
    static const char *const strip[] = {"name", "rarity", "rarity_meta", "customShader"};
    for (size_t i = 0; i < sizeof(strip) / sizeof(strip[0]); i++) cJSON_DeleteItemFromObjectCaseSensitive(g, strip[i]);
    if (SM.sh.preview) {
        cJSON *info = shader_info(cJSON_GetObjectItemCaseSensitive(editing_config(), "customShader"));
        if (info) put(g, "customShaderInfo", info);
    }
    char *json = cJSON_PrintUnformatted(g);
    cJSON_Delete(g);
    StrBuf sb = {0};
    uri_component(&sb, json ? json : "{}");
    free(json);
    return sb_finish(&sb);
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
    int n = snprintf(path, sizeof(path), "/skins?page=%d&page_size=%d", SM.page, SM_PAGE_SIZE);
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

static int num_pages(void) { return SM.total > 0 ? (SM.total + SM_PAGE_SIZE - 1) / SM_PAGE_SIZE : 0; }

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
    case R_THUMB: // Capture's updateSkin, then invalidateCache and Captured
        SM.capturing = 0;
        if (ok) {
            invalidate();
            SM.captured = 1;
            fprintf(stderr, "[skins] captured %lld\n", (long long) editing_id());
        } else fail_text("Failed to upload thumbnail: ", status, error);
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
        char folder[32], url[600];
        snprintf(folder, sizeof(folder), "%lld", (long long) jnum(skin, "skinId"));
        v10_skin_file_url(folder, "thumbnail.png", SM.asset_rev, url, sizeof(url));
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


// ------------------------------------------------------------------ the Appearance tab's sections

typedef struct {
    const char *type, *category; // uploadAsset's {type, category}
    const char *accept;          // the drop zone's acceptedTypes, joined
    long long max;               // its maxSize
} DropKind;

static const DropKind DROP_MODEL = {"model", "", ".obj,.gltf,.glb", 0x3200000};       // :125747-125749
static const DropKind DROP_TEXTURE = {"texture", "", ".png,.jpg,.jpeg", 0xa00000};    // :125944-125946
static const DropKind DROP_SOUND = {"sound", "", ".mp3,.wav,.ogg", 0x200000};         // :127022-127024
static const DropKind DROP_WEAR = {"wear", "", ".png,.jpg,.jpeg", 0xa00000};          // a wear layer, :126545-126547

// "Max: 10.0MB" / "500KB" (:11999)
static void size_text(char *out, const size_t n, const long long max) {
    if (max < 0x100000) snprintf(out, n, "%lldKB", (long long) llround((double) max / 1024.0));
    else snprintf(out, n, "%.1fMB", (double) max / 1024.0 / 1024.0);
}

// iììiíïí (:11910-12030, svelte-145grbc): the drop zone. Its click is the hidden input's (the platform dialog here);
// a picked file shows its name (file-success-state), a refused one its message (file-error-state)
static void file_drop(StrBuf *sb, const DropKind *k, const char *category, const char *slot, const char *label) {
    const SlotState *st = slot_state(slot, 0);
    const int has = st && st->file_name[0], err = st && st->drop_error[0] && !has;
    sb_appendf(sb, "<div class=\"file-drop-target svelte-145grbc%s%s%s\" role=\"button\" tabindex=\"0\" aria-label=\"", has ? " has-file" : "",
               err ? " error" : "", uploading(slot) ? " disabled" : "");
    sb_text(sb, label);
    sb_appendf(sb, "\" id=\"knSmDrop-%s\" onclick=\"knSkins('pick','%s','%s')\"><input type=\"file\" style=\"display: none;\" class=\"svelte-145grbc\" "
                   "accept=\"%s\"> <div class=\"file-drop-content svelte-145grbc\">", slot, k->type, category, k->accept);
    if (has) {
        sb_append(sb, "<div class=\"file-success-state svelte-145grbc\"><i class=\"material-icons success-icon svelte-145grbc\">check_circle</i> "
                      "<div class=\"file-name svelte-145grbc\">");
        sb_text(sb, st->file_name);
        sb_appendf(sb, "</div> <button class=\"remove-file-btn svelte-145grbc\" aria-label=\"Remove file\" onclick=\"event.stopPropagation();"
                       "knSkins('unset','%s','%s')\"><i class=\"material-icons svelte-145grbc\">close</i></button></div>", k->type, category);
    } else if (err) {
        sb_append(sb, "<div class=\"file-error-state svelte-145grbc\"><i class=\"material-icons error-icon svelte-145grbc\">error</i> "
                      "<div class=\"error-message svelte-145grbc\">");
        sb_text(sb, st->drop_error);
        sb_append(sb, "</div> <div class=\"retry-hint svelte-145grbc\">Click to try again</div></div>");
    } else {
        char types[64], max[24];
        snprintf(types, sizeof(types), "%s", k->accept);
        size_text(max, sizeof(max), k->max);
        sb_append(sb, "<div class=\"file-empty-state svelte-145grbc\"><i class=\"material-icons add-icon svelte-145grbc\">add</i> <div class=\"drop-label svelte-145grbc\">");
        sb_text(sb, label);
        sb_append(sb, "</div> <div class=\"accepted-types svelte-145grbc\">");
        for (const char *p = types; *p; p++) sb_append(sb, *p == ',' ? ", " : (char[2]) {*p, 0});
        sb_appendf(sb, "</div> <div class=\"size-limit svelte-145grbc\">Max: %s</div></div>", max);
    }
    sb_append(sb, "</div></div>");
}

static void upload_error_markup(StrBuf *sb, const char *hash, const char *slot) {
    const char *e = upload_error(slot);
    if (!e[0]) return;
    sb_appendf(sb, "<div class=\"upload-error svelte-%s\" id=\"knSmError-%s\"><i class=\"material-icons svelte-%s\">error</i> ", hash, slot, hash);
    sb_text(sb, e);
    sb_append(sb, "</div>");
}

static void model_note(StrBuf *sb, const char *ext) {
    if (!strcmp(ext, "gltf") || !strcmp(ext, "glb"))
        sb_append(sb, " <div class=\"info-note svelte-gygf75\">\xf0\x9f\x93\x9d GLTF/GLB models include textures - texture section will be hidden</div>");
    else if (!strcmp(ext, "obj")) sb_append(sb, " <div class=\"info-note svelte-gygf75\">\xf0\x9f\x93\x9d OBJ models require separate texture files</div>");
}

// the Model section (iïîîîìì, svelte-gygf75, :125700-125805): the uploaded model (Format, Status, Replace / Remove,
// both onModelRemoved) or the drop zone
static void model_section(StrBuf *sb) {
    const cJSON *cfg = editing_config();
    const char *ext = jstr(cfg, "fileExt"); // modelType (iÌíiiïí)
    sb_append(sb, "<div class=\"model-section section svelte-gygf75\" id=\"knSmModel\"><h3 class=\"section-title svelte-gygf75\"><i class=\"material-icons svelte-gygf75\">"
                  "view_in_ar</i> Model</h3> <div class=\"section-content svelte-gygf75\"><p class=\"section-description svelte-gygf75\">Upload a 3D model for your skin. "
                  "Supported formats: OBJ, GLTF, GLB</p> ");
    upload_error_markup(sb, "gygf75", "model");
    if (cfg_true(cfg, "customModel")) { // existingCustomModel
        char up[8] = "";
        for (size_t i = 0; ext[i] && i + 1 < sizeof(up); i++) up[i] = (char) (ext[i] >= 'a' && ext[i] <= 'z' ? ext[i] - 32 : ext[i]);
        sb_append(sb, " <div class=\"existing-model svelte-gygf75\"><div class=\"model-info file-info svelte-gygf75\"><div class=\"info-item svelte-gygf75\">"
                      "<span class=\"info-label svelte-gygf75\">Format:</span> <span class=\"info-value svelte-gygf75\">");
        sb_text(sb, up[0] ? up : "Unknown");
        sb_append(sb, "</span></div> <div class=\"info-item svelte-gygf75\"><span class=\"info-label svelte-gygf75\">Status:</span> <span class=\"info-value svelte-gygf75\">"
                      "Uploaded</span></div> ");
        model_note(sb, ext);
        sb_append(sb, "</div> <div class=\"existing-model-actions svelte-gygf75\"><button type=\"button\" class=\"btn-replace svelte-gygf75\" id=\"knSmModelReplace\" "
                      "onclick=\"knSkins('unset','model','')\"><i class=\"material-icons svelte-gygf75\">refresh</i> Replace</button> <button type=\"button\" "
                      "class=\"btn-remove svelte-gygf75\" id=\"knSmModelRemove\" onclick=\"knSkins('unset','model','')\"><i class=\"material-icons svelte-gygf75\">"
                      "delete</i> Remove</button></div></div>");
    } else {
        sb_append(sb, " ");
        file_drop(sb, &DROP_MODEL, "", "model", uploading("model") ? "Uploading..." : "Drop 3D model here");
        if (ext[0]) {
            char up[8] = "";
            for (size_t i = 0; ext[i] && i + 1 < sizeof(up); i++) up[i] = (char) (ext[i] >= 'a' && ext[i] <= 'z' ? ext[i] - 32 : ext[i]);
            sb_appendf(sb, " <div class=\"model-info file-info svelte-gygf75\"><div class=\"info-item svelte-gygf75\"><span class=\"info-label svelte-gygf75\">Format:</span> "
                           "<span class=\"info-value svelte-gygf75\">%s</span></div> <div class=\"info-item svelte-gygf75\"><span class=\"info-label svelte-gygf75\">Max size:</span> "
                           "<span class=\"info-value svelte-gygf75\">50MB</span></div> ", up);
            model_note(sb, ext);
            sb_append(sb, "</div>");
        }
    }
    sb_append(sb, "</div></div>");
}

// the Textures section (iïìììïí, svelte-4e4ust, :125806-126046): diffuse (a paint's Icon) and emissive (not for a
// player card), Texture Settings (Transparency, and Pulse with a glow), each texture uploaded or its drop zone
static void textures_section(StrBuf *sb, const int type) {
    const cJSON *cfg = editing_config();
    const cJSON *existing = cJSON_GetObjectItemCaseSensitive(cfg, "customTextures");
    const int icon_only = type == T_PAINT, hide_emissive = type == T_PLAYER_CARD;
    sb_appendf(sb, "<div class=\"textures-section section svelte-4e4ust\" id=\"knSmTextures\"><h3 class=\"section-title svelte-4e4ust\"><i class=\"material-icons svelte-4e4ust\">"
                   "%s</i> %s</h3> <div class=\"section-content svelte-4e4ust\"><p class=\"section-description svelte-4e4ust\">%s</p> ",
               icon_only ? "image" : "texture", icon_only ? "Paint Icon" : "Textures",
               icon_only ? "Upload the icon shown for this paint in the shop and inventory."
                         : "Upload texture maps to customize the appearance of your skin. Diffuse texture is required.");
    if (!icon_only) {
        sb_appendf(sb, "<div class=\"texture-settings svelte-4e4ust\"><h4 class=\"svelte-4e4ust\">Texture Settings</h4> <div class=\"settings-grid svelte-4e4ust\">"
                       "<label class=\"toggle-switch svelte-4e4ust\"><span class=\"svelte-4e4ust\">Transparency</span> <input type=\"checkbox\" class=\"svelte-4e4ust\" "
                       "id=\"knSmTrans\" onchange=\"knSkins('trans',this.checked)\"%s> <span class=\"slider svelte-4e4ust\"></span></label> ",
                   cfg_true(cfg, "trans") ? " checked" : "");
        if (cfg_true(cfg, "glow") && !hide_emissive) {
            const double p = jnum(cfg, "pulsT");
            const char *sel = p == 0.0015 ? "slow" : p == 0.003 ? "fast" : "off";
            sb_appendf(sb, "<div class=\"form-group form-row svelte-4e4ust\"><label for=\"pulse-setting\" class=\"svelte-4e4ust\">Pulse</label> <select id=\"pulse-setting\" "
                           "class=\"form-select svelte-4e4ust\" onchange=\"knSkins('pulse',this.value)\"><option class=\"svelte-4e4ust\" value=\"off\"%s>Off</option>"
                           "<option class=\"svelte-4e4ust\" value=\"slow\"%s>Slow</option><option class=\"svelte-4e4ust\" value=\"fast\"%s>Fast</option></select></div>",
                       !strcmp(sel, "off") ? " selected" : "", !strcmp(sel, "slow") ? " selected" : "", !strcmp(sel, "fast") ? " selected" : "");
        }
        sb_append(sb, "</div></div>");
    }
    sb_append(sb, " <div class=\"texture-uploads svelte-4e4ust\">");
    for (int i = 0; i < (icon_only || hide_emissive ? 1 : 2); i++) {
        const char *cat = i ? "emissive" : "diffuse";
        const char *label = i ? "Emissive/Glow" : icon_only ? "Icon" : "Diffuse/Albedo";
        const char *desc = i ? "Glowing/emissive areas (suffix: _e)" : icon_only ? "The image shown for this paint in the shop and inventory."
                                                                                 : "Main color/diffuse texture (Required)";
        char slot[24];
        snprintf(slot, sizeof(slot), "texture:%s", cat);
        sb_appendf(sb, "<div class=\"texture-upload-item svelte-4e4ust%s\" id=\"knSmTex-%s\"><div class=\"texture-header svelte-4e4ust\"><h5 class=\"svelte-4e4ust\">%s</h5> ",
                   i ? "" : " required", cat, label);
        if (!i) sb_append(sb, "<span class=\"required-badge svelte-4e4ust\">Required</span>");
        if (uploading(slot)) sb_append(sb, " <span class=\"uploading-badge svelte-4e4ust\"><i class=\"material-icons spinning svelte-4e4ust\">sync</i> Uploading...</span>");
        sb_appendf(sb, "</div> <p class=\"texture-description svelte-4e4ust\">%s</p> ", desc);
        upload_error_markup(sb, "4e4ust", slot);
        if (i) // settingKey hasGlow: the texture's toggle
            sb_appendf(sb, " <label class=\"texture-toggle svelte-4e4ust\"><input type=\"checkbox\" class=\"svelte-4e4ust\" id=\"knSmGlow\" "
                           "onchange=\"knSkins('glow',this.checked)\"%s> <span class=\"svelte-4e4ust\">Enable Emissive/Glow</span></label>",
                       SM.has_glow ? " checked" : "");
        if (cfg_true(existing, cat)) {
            char url[600];
            skin_url(i ? "texture_e.png" : "texture.png", url, sizeof(url));
            sb_appendf(sb, " <div class=\"existing-texture svelte-4e4ust\"><div class=\"texture-preview svelte-4e4ust\"><img class=\"texture-image svelte-4e4ust\" src=\"%s\" alt=\"%s texture\">"
                           "</div> <div class=\"existing-texture-actions svelte-4e4ust\"><button type=\"button\" class=\"btn-replace svelte-4e4ust\" "
                           "onclick=\"knSkins('unset','texture','%s')\"><i class=\"material-icons svelte-4e4ust\">refresh</i> Replace</button> <button type=\"button\" "
                           "class=\"btn-remove svelte-4e4ust\" id=\"knSmTexRemove-%s\" onclick=\"knSkins('unset','texture','%s')\"><i class=\"material-icons svelte-4e4ust\">"
                           "delete</i> Remove</button></div></div>", url, label, cat, cat, cat);
        } else if (!i || SM.has_glow) {
            sb_append(sb, " <div class=\"file-drop-container svelte-4e4ust\">");
            file_drop(sb, &DROP_TEXTURE, cat, slot, uploading(slot) ? "Uploading..." : "Drop texture here");
            sb_append(sb, "</div>");
        }
        sb_append(sb, "</div>");
    }
    sb_append(sb, "</div></div></div>");
}

// the Sounds section (iííìiíï, svelte-awmvtb, :126980-127098): a weapon's shoot / swap / inspect sounds, a melee's
// swap / inspect; other types get the disabled section
static void sounds_section(StrBuf *sb, const int type) {
    static const struct {
        const char *kind, *title, *desc, *flag;
    } items[] = {{"shoot", "Shoot Sound", "Plays every time the weapon fires", "customSound"},
                 {"swap", "Swap Sound", "Plays when the item is equipped (swapped to)", "customSwapSound"},
                 {"inspect", "Inspect Sound", "Plays when the item is inspected", "customInspectSound"}};
    if (type != 0 && type != 3) {
        sb_append(sb, "<div class=\"sound-section-disabled section svelte-awmvtb\" id=\"knSmSounds\"><h3 class=\"section-title svelte-awmvtb\"><i class=\"material-icons svelte-awmvtb\">"
                      "volume_up</i> Sounds</h3> <div class=\"section-content svelte-awmvtb\"><p class=\"section-description disabled svelte-awmvtb\">Custom sounds are only "
                      "available for weapons and melee</p></div></div>");
        return;
    }
    const cJSON *cfg = editing_config();
    sb_append(sb, "<div class=\"sound-section section svelte-awmvtb\" id=\"knSmSounds\"><h3 class=\"section-title svelte-awmvtb\"><i class=\"material-icons svelte-awmvtb\">volume_up</i> "
                  "Sounds</h3> <div class=\"section-content svelte-awmvtb\"><p class=\"section-description svelte-awmvtb\">Upload custom sounds for this item. Supported formats: "
                  "MP3, WAV, OGG</p> <div class=\"sound-items svelte-awmvtb\">");
    for (int i = type == 3 ? 1 : 0; i < 3; i++) {
        char slot[24];
        snprintf(slot, sizeof(slot), "sound:%s", items[i].kind);
        sb_appendf(sb, "<div class=\"sound-item svelte-awmvtb\" id=\"knSmSound-%s\"><div class=\"sound-header svelte-awmvtb\"><h5 class=\"svelte-awmvtb\">%s</h5></div> "
                       "<p class=\"sound-description svelte-awmvtb\">%s</p> ", items[i].kind, items[i].title, items[i].desc);
        upload_error_markup(sb, "awmvtb", slot);
        if (cfg_true(cfg, items[i].flag)) {
            sb_appendf(sb, " <div class=\"existing-sound svelte-awmvtb\"><div class=\"sound-info svelte-awmvtb\"><div class=\"info-item svelte-awmvtb\"><span class=\"icon svelte-awmvtb\">"
                           "\xe2\x9c\x85</span> <span class=\"svelte-awmvtb\">Custom sound uploaded</span></div></div> <div class=\"existing-sound-actions svelte-awmvtb\">"
                           "<button type=\"button\" class=\"btn-replace svelte-awmvtb\" onclick=\"knSkins('unset','sound','%s')\"><i class=\"material-icons svelte-awmvtb\">refresh</i> "
                           "Replace</button> <button type=\"button\" class=\"btn-remove svelte-awmvtb\" id=\"knSmSoundRemove-%s\" onclick=\"knSkins('unset','sound','%s')\">"
                           "<i class=\"material-icons svelte-awmvtb\">delete</i> Remove</button></div></div>", items[i].kind, items[i].kind, items[i].kind);
        } else {
            sb_append(sb, " <div class=\"file-drop-container svelte-awmvtb\">");
            file_drop(sb, &DROP_SOUND, items[i].kind, slot, uploading(slot) ? "Uploading..." : "Drop sound file here");
            sb_append(sb, "</div>");
        }
        if (i == 2)
            sb_appendf(sb, " <label class=\"loop-toggle svelte-awmvtb\"><input type=\"checkbox\" id=\"knSmInspLoop\" onchange=\"knSkins('loop',this.checked)\"%s> Loop while "
                           "inspecting</label>", cfg_true(cfg, "inspSLoop") ? " checked" : "");
        sb_append(sb, "</div>");
    }
    sb_append(sb, "</div> <div class=\"sound-info svelte-awmvtb\"><div class=\"info-item svelte-awmvtb\"><span class=\"icon svelte-awmvtb\">\xf0\x9f\x92\xa1</span> "
                  "<span class=\"svelte-awmvtb\">Keep each file under 2MB for best performance</span></div> <div class=\"info-item svelte-awmvtb\"><span class=\"icon svelte-awmvtb\">"
                  "\xf0\x9f\x8e\xb5</span> <span class=\"svelte-awmvtb\">Recommended: 44.1kHz, 16-bit, mono or stereo</span></div></div></div></div>");
}

static const char *const DYE_KEYS[] = {"skinCol", "shirtCol", "sleeveCol", "pantsCol", "waistCol", "shoeCol", "hairCol"}; // iÏiììîî
static const char *const DYE_LABELS[] = {"Skin", "Shirt", "Sleeves", "Pants", "Waist", "Shoes", "Hair"};

// the Dye Colors section (iïíìíîí, :125660-125700): each colour (iïiííìí: the itemConfig number as #rrggbb, else
// #ffffff) with its remove button when set, and the Opacity slider (opac, 1 when absent)
static void dye_section(StrBuf *sb) {
    const cJSON *cfg = editing_config();
    sb_append(sb, "<div class=\"section dye-colors-section svelte-wfs5t6\" id=\"knSmDye\"><h3 class=\"section-title svelte-wfs5t6\"><i class=\"material-icons svelte-wfs5t6\">"
                  "palette</i> Dye Colors</h3> <div class=\"section-content svelte-wfs5t6\"><p class=\"section-description svelte-wfs5t6\">Configure the colors and opacity "
                  "for this dye. Colors are applied to different parts of the player model.</p> <div class=\"dye-colors-grid svelte-wfs5t6\">");
    for (int i = 0; i < 7; i++) {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(cfg, DYE_KEYS[i]);
        const int set = cJSON_IsNumber(v);
        char hex[8] = "#ffffff";
        if (set) snprintf(hex, sizeof(hex), "#%06x", (unsigned) ((long long) v->valuedouble & 0xffffff));
        sb_appendf(sb, "<div class=\"dye-color-item svelte-wfs5t6\"><label class=\"dye-color-label svelte-wfs5t6\" for=\"dye-%s\">%s:</label> <div class=\"dye-color-input-wrapper "
                       "svelte-wfs5t6\"><input type=\"color\" class=\"dye-color-input svelte-wfs5t6\" id=\"dye-%s\" value=\"%s\" oninput=\"knSkins('dye','%s',this.value)\" "
                       "onchange=\"knSkins('dye','%s',this.value)\"> ", DYE_KEYS[i], DYE_LABELS[i], DYE_KEYS[i], hex, DYE_KEYS[i], DYE_KEYS[i]);
        if (set)
            sb_appendf(sb, "<button type=\"button\" class=\"dye-color-remove svelte-wfs5t6\" title=\"Remove color\" id=\"knSmDyeRemove-%s\" onclick=\"knSkins('undye','%s')\">"
                           "<i class=\"material-icons svelte-wfs5t6\">close</i></button>", DYE_KEYS[i], DYE_KEYS[i]);
        sb_append(sb, "</div></div>");
    }
    const cJSON *op = cJSON_GetObjectItemCaseSensitive(cfg, "opac");
    const double opac = cJSON_IsNumber(op) ? op->valuedouble : 1.0;
    sb_appendf(sb, "</div> <div class=\"dye-opacity-control svelte-wfs5t6\"><label class=\"dye-opacity-label svelte-wfs5t6\" for=\"dye-opacity\">Opacity:</label> <div class=\"dye-opacity-wrapper "
                   "svelte-wfs5t6\"><input type=\"range\" id=\"dye-opacity\" min=\"0\" max=\"1\" step=\"0.01\" class=\"dye-opacity-slider svelte-wfs5t6\" value=\"%g\" "
                   "oninput=\"knSkins('opac',this.value)\"> <span class=\"dye-opacity-value svelte-wfs5t6\" id=\"knSmOpacValue\">%.2f</span></div></div></div></div>",
               opac, opac);
}

// ---- Shader Studio (iììîiìï, svelte-1x46pbz, :127099-127410)

// iìíïïìí("surface") (:122361-122420): a baseColor node into the output, alpha 1
static cJSON *default_graph(void) {
    return cJSON_Parse("{\"v\":1,\"kind\":\"surface\",\"nodes\":[{\"id\":\"base1\",\"type\":\"baseColor\",\"x\":120,\"y\":90},{\"id\":\"out\",\"type\":\"output\","
                       "\"x\":420,\"y\":80,\"inputs\":{\"color\":{\"node\":\"base1\"},\"alpha\":{\"value\":1}}}],\"output\":\"out\"}");
}

// iìïìïïï (:122528): a graph document
static int graph_ok(const cJSON *g) {
    return cJSON_IsObject(g) && jnum(g, "v") == 1 && cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(g, "nodes")) &&
           cJSON_IsString(cJSON_GetObjectItemCaseSensitive(g, "output"));
}

// iiïìïiï (:122495-122511): a param's default of its type
static cJSON *param_default(const char *type, const cJSON *v) {
    if (!strcmp(type, "color")) {
        const char *h = cJSON_IsString(v) ? v->valuestring : "";
        int ok = h[0] == '#' && strlen(h) == 7;
        for (int i = 1; ok && i < 7; i++) ok = strchr("0123456789abcdefABCDEF", h[i]) != NULL;
        return cJSON_CreateString(ok ? h : "#ffffff");
    }
    const int n = !strcmp(type, "vec2") ? 2 : !strcmp(type, "vec3") ? 3 : !strcmp(type, "vec4") ? 4 : 1;
    if (n == 1) return cJSON_CreateNumber(cJSON_IsNumber(v) && isfinite(v->valuedouble) ? v->valuedouble : 0);
    cJSON *a = cJSON_CreateArray();
    const cJSON *first = cJSON_IsArray(v) ? v->child : v;
    for (int i = 0; i < n; i++) {
        const cJSON *e = cJSON_IsArray(v) ? cJSON_GetArrayItem(v, i) : NULL;
        cJSON_AddItemToArray(a, cJSON_CreateNumber(cJSON_IsNumber(e) ? e->valuedouble : cJSON_IsNumber(first) ? first->valuedouble : 0));
    }
    return a;
}

// iïíìíìï (:122457-122494): the graph's "param" nodes, by name (the first of a name)
static cJSON *graph_params(void) {
    cJSON *out = cJSON_CreateArray();
    const cJSON *node;
    cJSON_ArrayForEach(node, cJSON_GetObjectItemCaseSensitive(SM.sh.graph, "nodes")) {
        if (strcmp(jstr(node, "type"), "param")) continue;
        const cJSON *p = cJSON_GetObjectItemCaseSensitive(node, "params");
        char name[64];
        trimmed(name, sizeof(name), jstr(p, "name"));
        int dup = !name[0];
        const cJSON *e;
        cJSON_ArrayForEach(e, out) dup |= !strcmp(jstr(e, "name"), name);
        if (dup) continue;
        const char *type = jstr(p, "type")[0] ? jstr(p, "type") : "float";
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "nodeId", jstr(node, "id"));
        cJSON_AddStringToObject(o, "name", name);
        cJSON_AddStringToObject(o, "type", type);
        cJSON_AddItemToObject(o, "default", param_default(type, cJSON_GetObjectItemCaseSensitive(p, "default")));
        cJSON_AddItemToArray(out, o);
    }
    return out;
}

// iÍìïíïî: a value as the chips print it
static void param_text(StrBuf *sb, const char *type, const cJSON *v) {
    if (!strcmp(type, "color")) sb_text(sb, cJSON_IsString(v) ? v->valuestring : "");
    else if (cJSON_IsArray(v)) {
        const cJSON *e;
        int first = 1;
        cJSON_ArrayForEach(e, v) {
            sb_appendf(sb, "%s%g", first ? "" : ", ", round(e->valuedouble * 1000.0) / 1000.0);
            first = 0;
        }
    } else sb_appendf(sb, "%g", round((cJSON_IsNumber(v) ? v->valuedouble : 0) * 1000.0) / 1000.0);
}

static void shader_write(void);

// the canvas's onChange (iÌiïíîi, :127145-127170): itemConfig.customShader takes the graph (onSkinChange, the legacy
// notice goes) and the preview shows it (onShaderStudioUpdate); the window again when the exposed params changed
static void on_canvas_change(void *user, const cJSON *graph) {
    (void) user;
    if (!SM.editing || !SM.sh.loaded || !graph) return;
    cJSON *before = graph_params();
    cJSON_Delete(SM.sh.graph);
    SM.sh.graph = cJSON_Duplicate(graph, 1);
    cJSON *after = graph_params();
    if (!cJSON_Compare(before, after, 1) || SM.sh.legacy) SM.sh.render_due = 1;
    cJSON_Delete(before);
    cJSON_Delete(after);
    shader_write();
    SM.sh.legacy = 0;
    SM.sh.preview = 1;
}

static SgEditor *canvas(void) {
    if (!SM.sh.canvas) SM.sh.canvas = sg_editor_new("knSmSg", "knSkins('sg'", "surface", on_canvas_change, NULL);
    return SM.sh.canvas;
}

// the studio's state of the edited skin (the effect at :127106-127124): its graph customShader, else the default
static void shader_load(void) {
    const int64_t id = editing_id();
    if (SM.sh.loaded && SM.sh.skin == id) return;
    cJSON_Delete(SM.sh.graph);
    cJSON_Delete(SM.sh.parts);
    const cJSON *cs = cJSON_GetObjectItemCaseSensitive(editing_config(), "customShader");
    const cJSON *g = cJSON_GetObjectItemCaseSensitive(cs, "graph");
    const int graph = !strcmp(jstr(cs, "type"), "graph") && graph_ok(g);
    SM.sh.legacy = cs && !graph;
    SM.sh.graph = graph ? cJSON_Duplicate(g, 1) : default_graph();
    SM.sh.apply_all = !graph || !cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(cs, "applyToAll"));
    SM.sh.parts = cJSON_CreateArray();
    const cJSON *p;
    if (graph) cJSON_ArrayForEach(p, cJSON_GetObjectItemCaseSensitive(cs, "parts")) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "nodeName", p->string ? p->string : "");
        const cJSON *params = cJSON_GetObjectItemCaseSensitive(p, "params");
        cJSON_AddItemToObject(e, "params", cJSON_IsObject(params) ? cJSON_Duplicate(params, 1) : cJSON_CreateObject());
        cJSON_AddItemToArray(SM.sh.parts, e);
    }
    if (cs) SM.sh.open = 1;
    SM.sh.skin = id;
    SM.sh.loaded = 1;
    SM.sh.render_due = 0;
    sg_editor_set_graph(canvas(), SM.sh.graph); // a new document: the canvas's history and view start over
}

static void shader_section(StrBuf *sb) {
    shader_load();
    sb_appendf(sb, "<div class=\"section shader-studio-section svelte-1x46pbz\" id=\"knSmShader\"><h3 class=\"section-title svelte-1x46pbz\" style=\"cursor: pointer\" "
                   "id=\"knSmShaderToggle\" onclick=\"knSkins('shader','toggle')\"><i class=\"material-icons svelte-1x46pbz\">account_tree</i> %s <i class=\"material-icons "
                   "expand-icon svelte-1x46pbz\">%s</i></h3>", i18n_tr("shaderGraph.studio.title", "Shader Studio"), SM.sh.open ? "expand_less" : "expand_more");
    if (!SM.sh.open) {
        sb_append(sb, " <!----></div>");
        return;
    }
    sb_append(sb, " <div class=\"section-content svelte-1x46pbz\"><p class=\"section-description svelte-1x46pbz\">");
    sb_text(sb, i18n_tr("shaderGraph.studio.desc", "Build the item's material as a node graph with live preview."));
    sb_append(sb, "</p> ");
    if (SM.sh.legacy) {
        sb_append(sb, "<div class=\"legacy-notice svelte-1x46pbz\"><i class=\"material-icons svelte-1x46pbz\">warning</i> ");
        sb_text(sb, i18n_tr("shaderGraph.legacyNotice", "This item used a GLSL shader, which is no longer supported."));
        sb_append(sb, "</div>");
    }
    // the node canvas (iïiìííí, kind "surface"): its pending change first, then the studio's graph as its prop
    sg_editor_flush(canvas());
    sg_editor_set_graph(canvas(), SM.sh.graph);
    sb_append(sb, " ");
    sg_editor_html(canvas(), sb);
    sb_append(sb, " ");
    cJSON *params = graph_params();
    sb_append(sb, " <div class=\"params-section svelte-1x46pbz\"><div class=\"editor-label svelte-1x46pbz\">");
    sb_text(sb, i18n_tr("shaderGraph.params.title", "Exposed params"));
    sb_append(sb, "</div> ");
    if (cJSON_GetArraySize(params)) {
        sb_append(sb, "<div class=\"params-list svelte-1x46pbz\">");
        const cJSON *p;
        cJSON_ArrayForEach(p, params) {
            sb_append(sb, "<div class=\"param-chip svelte-1x46pbz\" title=\"");
            sb_text(sb, jstr(p, "nodeId"));
            sb_append(sb, "\"><span class=\"param-name svelte-1x46pbz\">");
            sb_text(sb, jstr(p, "name"));
            sb_append(sb, "</span> <span class=\"param-type svelte-1x46pbz\">");
            sb_text(sb, jstr(p, "type"));
            sb_append(sb, "</span> <span class=\"param-default svelte-1x46pbz\">= ");
            param_text(sb, jstr(p, "type"), cJSON_GetObjectItemCaseSensitive(p, "default"));
            sb_append(sb, "</span></div>");
        }
        sb_append(sb, "</div>");
    } else {
        sb_append(sb, "<p class=\"parts-description svelte-1x46pbz\">");
        sb_text(sb, i18n_tr("shaderGraph.params.none", "Add a \xe2\x80\x9cparam\xe2\x80\x9d node to the graph to expose a value that parts can override."));
        sb_append(sb, "</p>");
    }
    sb_appendf(sb, "</div> <div class=\"apply-all-row svelte-1x46pbz\"><label class=\"apply-all-label svelte-1x46pbz\"><input type=\"checkbox\" class=\"apply-all-checkbox "
                   "svelte-1x46pbz\" id=\"knSmApplyAll\" onchange=\"knSkins('shader','applyAll',this.checked)\"%s> %s</label> <span class=\"apply-all-hint svelte-1x46pbz\">%s</span></div> ",
               SM.sh.apply_all ? " checked" : "", i18n_tr("shaderGraph.applyAll", "Apply to all parts"),
               SM.sh.apply_all ? i18n_tr("shaderGraph.applyAll.on", "Shader applies to the entire model")
                               : i18n_tr("shaderGraph.applyAll.off", "Shader only applies to the parts listed below"));
    if (!SM.sh.apply_all) {
        sb_appendf(sb, "<div class=\"parts-section svelte-1x46pbz\"><div class=\"parts-header svelte-1x46pbz\"><div class=\"editor-label svelte-1x46pbz\">%s</div> "
                       "<button class=\"add-uniform-btn svelte-1x46pbz\" id=\"knSmAddPart\" onclick=\"knSkins('shader','addPart')\"><i class=\"material-icons svelte-1x46pbz\">add</i> "
                       "%s</button></div> <p class=\"parts-description svelte-1x46pbz\">", i18n_tr("shaderGraph.parts.title", "Parts"),
                   i18n_tr("shaderGraph.parts.add", "Add Part"));
        sb_text(sb, i18n_tr("shaderGraph.parts.desc", "Only the named mesh nodes below get the shader."));
        sb_append(sb, "</p> ");
        int i = 0;
        const cJSON *part;
        cJSON_ArrayForEach(part, SM.sh.parts) {
            sb_append(sb, "<div class=\"part-block svelte-1x46pbz\"><div class=\"part-header svelte-1x46pbz\"><input type=\"text\" class=\"part-name-input svelte-1x46pbz\" placeholder=\"");
            sb_text(sb, i18n_tr("shaderGraph.parts.nodeName", "Node name (e.g. Blob_low)"));
            sb_appendf(sb, "\" id=\"knSmPart%d\" value=\"", i);
            sb_text(sb, jstr(part, "nodeName"));
            sb_appendf(sb, "\" onchange=\"knSkins('shader','partName',%d,this.value)\"> <button class=\"copy-defaults-btn svelte-1x46pbz\" title=\"%s\" "
                           "onclick=\"knSkins('shader','copyDefaults',%d)\"%s><i class=\"material-icons svelte-1x46pbz\">content_copy</i></button> <button "
                           "class=\"remove-uniform-btn svelte-1x46pbz\" onclick=\"knSkins('shader','removePart',%d)\"><i class=\"material-icons svelte-1x46pbz\">close</i></button></div> ",
                       i, i18n_tr("shaderGraph.parts.copyDefaults", "Override every param with the graph defaults"), i,
                       cJSON_GetArraySize(params) ? "" : " disabled", i);
            const cJSON *pv = cJSON_GetObjectItemCaseSensitive(part, "params");
            if (!cJSON_GetArraySize(params)) {
                sb_append(sb, "<p class=\"parts-description svelte-1x46pbz\">");
                sb_text(sb, i18n_tr("shaderGraph.params.none", "Add a \xe2\x80\x9cparam\xe2\x80\x9d node to the graph to expose a value that parts can override."));
                sb_append(sb, "</p>");
            }
            const cJSON *p;
            cJSON_ArrayForEach(p, params) {
                const char *name = jstr(p, "name"), *type = jstr(p, "type");
                const cJSON *v = cJSON_GetObjectItemCaseSensitive(pv, name);
                sb_appendf(sb, "<div class=\"uniform-row svelte-1x46pbz\"><label class=\"override-toggle svelte-1x46pbz\" title=\"%s\"><input type=\"checkbox\" "
                               "class=\"uniform-checkbox svelte-1x46pbz\" onchange=\"knSkins('shader','override',%d,'", i18n_tr("shaderGraph.parts.override", "Override for this part"), i);
                sb_text(sb, name);
                sb_appendf(sb, "',this.checked)\"%s></label> <span class=\"uniform-name svelte-1x46pbz\">", v ? " checked" : "");
                sb_text(sb, name);
                sb_append(sb, "</span> <span class=\"param-type svelte-1x46pbz\">");
                sb_text(sb, type);
                sb_append(sb, "</span> <div class=\"uniform-value svelte-1x46pbz\">");
                if (v) { // KrunkNative: one text field for the value (v10's sg value editor: a slider / colour / vector)
                    StrBuf vb = {0};
                    param_text(&vb, type, v);
                    char *vt = sb_finish(&vb);
                    sb_appendf(sb, "<input type=\"%s\" class=\"sgInput\" value=\"", !strcmp(type, "color") ? "color" : "text");
                    sb_text(sb, vt ? vt : "");
                    sb_appendf(sb, "\" onchange=\"knSkins('shader','param',%d,'", i);
                    sb_text(sb, name);
                    sb_append(sb, "',this.value)\">");
                    free(vt);
                } else {
                    sb_appendf(sb, "<span class=\"param-default svelte-1x46pbz\">%s: ", i18n_tr("shaderGraph.parts.useDefault", "default"));
                    param_text(sb, type, cJSON_GetObjectItemCaseSensitive(p, "default"));
                    sb_append(sb, "</span>");
                }
                sb_append(sb, "</div></div>");
            }
            sb_append(sb, "</div>");
            i++;
        }
        sb_append(sb, "</div>");
    }
    cJSON_Delete(params);
    sb_appendf(sb, " <div class=\"shader-actions svelte-1x46pbz\"><button class=\"apply-btn svelte-1x46pbz\" id=\"knSmShaderApply\" onclick=\"knSkins('shader','refresh')\">"
                   "<i class=\"material-icons svelte-1x46pbz\">play_arrow</i> %s</button> <button class=\"clear-btn svelte-1x46pbz\" id=\"knSmShaderClear\" "
                   "onclick=\"knSkins('shader','clearPreview')\"><i class=\"material-icons svelte-1x46pbz\">visibility_off</i> %s</button> <button class=\"clear-btn danger "
                   "svelte-1x46pbz\" id=\"knSmShaderRemove\" onclick=\"knSkins('shader','remove')\"><i class=\"material-icons svelte-1x46pbz\">delete</i> %s</button></div></div></div>",
               i18n_tr("shaderGraph.refreshPreview", "Refresh Preview"), i18n_tr("shaderGraph.clearPreview", "Clear Preview"),
               i18n_tr("shaderGraph.removeShader", "Remove Shader"));
}

static void changed(void);
static void unset_asset(const char *type, const char *category);
static void up_start(const char *slot, const char *type, const char *category, const char *file_name, unsigned char *data, size_t len);
static int64_t arg_i64(const HtmlArg *args, int argc, int i);
static const char *arg_s(const HtmlArg *args, int argc, int i);

// ------------------------------------------------------------------ Paintable, Paint, Wear, Kounter and Charm

#define WEAR_SIZE 1024        // the packed wear map's size (iIïìíiì, :126414)
#define PAINT_BASE_WEAPON 7805 // the Paint section's Weapon / Cosmetic quick picks (iïíìíìì / iìîììií, :124210-124211)
#define PAINT_BASE_COSMETIC 7798

// the wear layers (iÌíïììì, :126415-126431): each picked file's red channel is packed into one channel
static const struct {
    const char *key, *label, *desc;
} WEAR_LAYERS[4] = {{"rust", "Rust", "Rust patches and corrosion (packed into red channel)"},
                    {"dirt", "Dirt", "Dirt and grime buildup (packed into green channel)"},
                    {"scratch", "Scratches", "Scratches and gouges (packed into blue channel)"},
                    {"wear", "General Wear", "Overall degradation (packed into alpha channel)"}};

// the Kounter's / charm's preview tools (iIiìíìì :126670-126690, iÍiïîìî :126851-126867)
static const struct {
    const char *mode, *icon, *label;
} PLACE_TOOLS[5] = {{"off", "3d_rotation", "View"},       {"snap", "ads_click", "Snap to surface"}, {"translate", "open_with", "Move"},
                    {"rotate", "rotate_90_degrees_ccw", "Rotate"}, {"scale", "open_in_full", "Scale"}};

// the Kounter's stats (iiîïîìî :106682) and their labels (iIïíîiì :126692-126701)
static const char *const KOUNTER_STATS[8] = {"kills", "hskills", "aikills", "headshots", "hits", "shots", "jumps", "candy"};
static const char *const KOUNTER_LABELS[8] = {"Kills", "Headshot kills", "AI kills", "Headshots", "Shots hit", "Shots fired", "Jumps", "Candy (map script)"};
static const char *const CHARM_KEYS[4] = {"charmMXOff", "charmMYOff", "charmMZOff", "charmMScl"}; // iïiîïîï (:120757)

// a JS number as String(n) gives it (enough for the fields' values)
static void js_number(char *out, const size_t n, const double v) {
    if (v == (double) (long long) v && fabs(v) < 1e15) snprintf(out, n, "%lld", (long long) v);
    else snprintf(out, n, "%.15g", v);
}

static double js_float(const float v) {
    char t[32];
    snprintf(t, sizeof(t), "%.7g", (double) v);
    return strtod(t, NULL);
}

static int tool_index(const char *mode) {
    for (int i = 0; i < 5; i++)
        if (!strcmp(mode, PLACE_TOOLS[i].mode)) return i;
    return 0;
}

static void wear_layers_clear(void) {
    for (int i = 0; i < 4; i++) {
        free(SM.ed.layer[i]);
        SM.ed.layer[i] = NULL;
        SM.ed.layer_len[i] = 0;
        char slot[24];
        snprintf(slot, sizeof(slot), "wear:%s", WEAR_LAYERS[i].key);
        SlotState *st = slot_state(slot, 0);
        if (st) st->file_name[0] = st->drop_error[0] = 0;
    }
}

static SgEditor *paint_canvas(void);

// each section's own state, started over for another skin (the effects keyed on editingSkin.skinId: :126097-126102,
// :126254-126259, :126495-126497, and the preview's :124876-124878)
static void editors_load(void) {
    if (!SM.editing) return;
    const int64_t id = editing_id();
    if (SM.ed.ready && SM.ed.skin == id) return;
    const cJSON *cfg = editing_config();
    SM.ed.ready = 1;
    SM.ed.skin = id;
    wear_layers_clear();
    SM.ed.wear_on = cfg_true(cJSON_GetObjectItemCaseSensitive(cfg, "customTextures"), "wear");
    SM.ed.generating = 0;
    SM.ed.wear_error[0] = 0;
    SM.ed.targets[0] = 0; // the mesh names: paintTargets.join(", ") when it is a list
    const cJSON *targets = cJSON_GetObjectItemCaseSensitive(cfg, "paintTargets");
    if (cJSON_IsArray(targets)) {
        size_t at = 0;
        const cJSON *t;
        cJSON_ArrayForEach(t, targets) {
            char part[64];
            if (cJSON_IsString(t)) snprintf(part, sizeof(part), "%s", t->valuestring);
            else if (cJSON_IsNumber(t)) js_number(part, sizeof(part), t->valuedouble);
            else snprintf(part, sizeof(part), "%s", cJSON_IsNull(t) ? "null" : cJSON_IsTrue(t) ? "true" : "false");
            const int w = snprintf(SM.ed.targets + at, sizeof(SM.ed.targets) - at, "%s%s", at ? ", " : "", part);
            if (w < 0 || (size_t) w >= sizeof(SM.ed.targets) - at) break;
            at += (size_t) w;
        }
    }
    SM.ed.attach_mode = SM.ed.charm_mode = 0;
    SM.ed.wear_open = SM.ed.wear_touched = 0;
    SM.ed.wear_level = 0.0;
    SM.ed.paint_pick = -1;
    if (!SM.ed.paint_base) SM.ed.paint_base = PAINT_BASE_WEAPON;
    // the paint's canvas: paintShaderGraph when it is a graph (iìïìïïï :122528). A Solid Colour paint has none: v10
    // mounts the sgEditor only in Animated Shader mode (:126258), so its default graph (an onChange that drops
    // paintColor) waits for paintMode "shader"
    const cJSON *pg = cJSON_GetObjectItemCaseSensitive(cfg, "paintShaderGraph");
    if (graph_ok(pg)) sg_editor_set_graph(paint_canvas(), pg);
    else if (SM.ed.paint_canvas) {
        sg_editor_free(SM.ed.paint_canvas);
        SM.ed.paint_canvas = NULL;
    }
}

// ---- Paintable (iìïïiiî, svelte-16sjsp2, :126048-126213): paintTargets true / a list of mesh names, the paint mask

// iÌiíììì: the text split at ",", trimmed, without the empty names
static cJSON *mesh_list(const char *text) {
    cJSON *a = cJSON_CreateArray();
    const char *p = text;
    while (p && *p) {
        const char *e = strchr(p, ',');
        const size_t n = e ? (size_t) (e - p) : strlen(p);
        char part[256];
        snprintf(part, sizeof(part), "%.*s", (int) (n < sizeof(part) - 1 ? n : sizeof(part) - 1), p);
        char t[256];
        trimmed(t, sizeof(t), part);
        if (t[0]) cJSON_AddItemToArray(a, cJSON_CreateString(t));
        p = e ? e + 1 : NULL;
    }
    return a;
}

static void paintable_section(StrBuf *sb) {
    const cJSON *cfg = editing_config();
    const cJSON *targets = cJSON_GetObjectItemCaseSensitive(cfg, "paintTargets");
    const int on = cfg_true(cfg, "paintTargets"), specific = cJSON_IsArray(targets);
    sb_appendf(sb, "<div class=\"section paintable-section svelte-16sjsp2\" id=\"knSmPaintable\"><h3 class=\"section-title svelte-16sjsp2\"><i class=\"material-icons svelte-16sjsp2\">"
                   "format_paint</i> Paintable</h3> <div class=\"section-content svelte-16sjsp2\"><p class=\"section-description svelte-16sjsp2\">Allow players to apply a "
                   "Paint to this item to recolor it. Optionally restrict which parts get painted with a\n        mask or by naming meshes.</p> <div class=\"paintable-settings "
                   "svelte-16sjsp2\"><label class=\"toggle-switch svelte-16sjsp2\"><span class=\"svelte-16sjsp2\">Paintable</span> <input type=\"checkbox\" class=\"svelte-16sjsp2\" "
                   "id=\"knSmPaintableOn\" onchange=\"knSkins('paintable',this.checked)\"%s> <span class=\"slider svelte-16sjsp2\"></span></label></div> ",
               on ? " checked" : "");
    if (on) {
        sb_appendf(sb, "<div class=\"form-group form-row svelte-16sjsp2\"><label for=\"paint-targets-mode\" class=\"svelte-16sjsp2\">Paint area</label> <select "
                       "id=\"paint-targets-mode\" class=\"form-select svelte-16sjsp2\" onchange=\"knSkins('paintArea',this.value)\"><option class=\"svelte-16sjsp2\" value=\"all\"%s>"
                       "All meshes</option><option class=\"svelte-16sjsp2\" value=\"specific\"%s>Specific meshes</option></select></div> ",
                   specific ? "" : " selected", specific ? " selected" : "");
        if (specific) {
            sb_append(sb, "<div class=\"form-group svelte-16sjsp2\"><label for=\"paint-targets-meshes\" class=\"svelte-16sjsp2\">Mesh names</label> <input id=\"knSmPaintMeshes\" "
                          "type=\"text\" class=\"form-input svelte-16sjsp2\" placeholder=\"e.g. head, body, barrel\" oninput=\"knSkins('input','paintMeshes')\" value=\"");
            sb_text(sb, SM.ed.targets);
            sb_append(sb, "\"> <p class=\"field-hint svelte-16sjsp2\">Comma-separated mesh/material names. Only these meshes will accept paint.</p></div>");
        }
        sb_append(sb, " <div class=\"texture-upload-item svelte-16sjsp2\" id=\"knSmPaintMask\"><div class=\"texture-header svelte-16sjsp2\"><h5 class=\"svelte-16sjsp2\">Paint Mask</h5> "
                      "<span class=\"optional-badge svelte-16sjsp2\">Optional</span>");
        if (uploading("texture:mask"))
            sb_append(sb, " <span class=\"uploading-badge svelte-16sjsp2\"><i class=\"material-icons spinning svelte-16sjsp2\">sync</i> Uploading...</span>");
        sb_append(sb, "</div> <p class=\"texture-description svelte-16sjsp2\">Grayscale texture whose red channel limits the paintable region (suffix: _p). White = "
                      "paintable, black =\n            untouched.</p> ");
        upload_error_markup(sb, "16sjsp2", "texture:mask");
        if (cfg_true(cJSON_GetObjectItemCaseSensitive(cfg, "customTextures"), "mask")) {
            char url[600];
            skin_url("texture_p.png", url, sizeof(url));
            sb_appendf(sb, " <div class=\"existing-texture svelte-16sjsp2\"><div class=\"texture-preview svelte-16sjsp2\"><img alt=\"Paint mask texture\" class=\"texture-image "
                           "svelte-16sjsp2\" src=\"%s\"></div> <div class=\"existing-texture-actions svelte-16sjsp2\"><button type=\"button\" class=\"btn-remove svelte-16sjsp2\" "
                           "id=\"knSmPaintMaskRemove\" onclick=\"knSkins('unset','texture','mask')\"><i class=\"material-icons svelte-16sjsp2\">delete</i> Remove</button></div></div>",
                       url);
        } else {
            sb_append(sb, " <div class=\"file-drop-container svelte-16sjsp2\">");
            file_drop(sb, &DROP_TEXTURE, "mask", "texture:mask", uploading("texture:mask") ? "Uploading..." : "Drop paint mask here");
            sb_append(sb, "</div>");
        }
        sb_append(sb, "</div>");
    }
    sb_append(sb, "</div></div>");
}

// ---- Paint (iiììiìî, svelte-1qb7o10, :126215-126400): a paint's solid colour or its node graph shader

static void on_paint_canvas_change(void *user, const cJSON *graph) { // iÍííiïi: paintShaderGraph, no paintColor
    (void) user;
    if (!SM.editing || !graph) return;
    cJSON *cfg = editing_config();
    put(cfg, "paintShaderGraph", cJSON_Duplicate(graph, 1));
    put(cfg, "paintColor", NULL);
    put(cfg, "paintShaderCode", NULL); // iÍìiìíï: the GLSL of old goes with any change
    put(cfg, "paintShaderUniforms", NULL);
    changed();
}

static SgEditor *paint_canvas(void) {
    if (!SM.ed.paint_canvas) SM.ed.paint_canvas = sg_editor_new("knSmPg", "knSkins('pg'", "paint", on_paint_canvas_change, NULL);
    return SM.ed.paint_canvas;
}

static int paint_shader_mode(void) { return graph_ok(cJSON_GetObjectItemCaseSensitive(editing_config(), "paintShaderGraph")); } // iÍïìïïî

static void paint_section(StrBuf *sb) {
    const cJSON *cfg = editing_config();
    const int shader = paint_shader_mode();
    const char *color = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(cfg, "paintColor"));
    sb_append(sb, "<div class=\"section paint-section svelte-1qb7o10\" id=\"knSmPaint\"><h3 class=\"section-title svelte-1qb7o10\"><i class=\"material-icons svelte-1qb7o10\">"
                  "colorize</i> Paint</h3> <div class=\"section-content svelte-1qb7o10\"><p class=\"section-description svelte-1qb7o10\">A Paint is a consumable players apply "
                  "to paintable items to recolor them. Choose a solid colour or author an\n        animated shader, then preview it on a real item.</p> ");
    if (cJSON_IsString(cJSON_GetObjectItemCaseSensitive(cfg, "paintShaderCode"))) {
        sb_append(sb, "<div class=\"legacy-notice svelte-1qb7o10\"><i class=\"material-icons svelte-1qb7o10\">warning</i> ");
        sb_text(sb, i18n_tr("shaderGraph.paint.legacyNotice", "This Paint used a GLSL shader, which is no longer supported. Pick Animated Shader to author it as a node "
                                                              "graph; the old code is dropped on save."));
        sb_append(sb, "</div>");
    }
    sb_appendf(sb, " <div class=\"paint-mode-row svelte-1qb7o10\"><button type=\"button\" class=\"paint-mode-btn svelte-1qb7o10%s\" id=\"knSmPaintModeColor\" "
                   "onclick=\"knSkins('paintMode','color')\">Solid Colour</button> <button type=\"button\" class=\"paint-mode-btn svelte-1qb7o10%s\" id=\"knSmPaintModeShader\" "
                   "onclick=\"knSkins('paintMode','shader')\">Animated Shader</button></div> ",
               shader ? "" : " active", shader ? " active" : "");
    if (!shader) {
        sb_append(sb, "<div class=\"paint-color-row svelte-1qb7o10\"><label class=\"paint-color-label svelte-1qb7o10\" for=\"paint-color\">Paint color:</label> <div "
                      "class=\"paint-color-input-wrapper svelte-1qb7o10\"><input type=\"color\" id=\"knSmPaintColor\" class=\"paint-color-input svelte-1qb7o10\" value=\"");
        sb_text(sb, color && color[0] ? color : "#ffffff");
        sb_append(sb, "\" oninput=\"knSkins('paintColor',this.value)\">");
        if (color && color[0]) {
            sb_append(sb, " <span class=\"paint-color-value svelte-1qb7o10\" id=\"knSmPaintColorValue\">");
            sb_text(sb, color);
            sb_append(sb, "</span> <button type=\"button\" class=\"paint-color-remove svelte-1qb7o10\" title=\"Clear color\" id=\"knSmPaintColorClear\" "
                          "onclick=\"knSkins('paintColor','')\"><i class=\"material-icons svelte-1qb7o10\">close</i></button>");
        }
        sb_append(sb, "</div></div>");
    } else {
        sb_append(sb, "<p class=\"shader-hint svelte-1qb7o10\">");
        sb_text(sb, i18n_tr("shaderGraph.paint.shaderHint", "The graph's output is the paint colour; the game keeps texture detail, mask and intensity. The shader "
                                                          "id is assigned when this Paint is reviewed for release."));
        sb_append(sb, "</p> ");
        sg_editor_flush(paint_canvas());
        sg_editor_set_graph(paint_canvas(), cJSON_GetObjectItemCaseSensitive(cfg, "paintShaderGraph"));
        sg_editor_html(paint_canvas(), sb);
    }
    sb_appendf(sb, " <div class=\"paint-preview-base svelte-1qb7o10\"><label class=\"field-label svelte-1qb7o10\" for=\"paint-preview-base\">Preview on item:</label> <div "
                   "class=\"preview-base-controls svelte-1qb7o10\"><input id=\"knSmPaintBase\" type=\"number\" class=\"preview-base-input svelte-1qb7o10\" value=\"%d\" "
                   "onchange=\"knSkins('paintBase',this.value)\"> <button type=\"button\" class=\"preview-base-quick svelte-1qb7o10\" id=\"knSmPaintBaseWeapon\" "
                   "onclick=\"knSkins('paintBase','%d')\">Weapon</button> <button type=\"button\" class=\"preview-base-quick svelte-1qb7o10\" id=\"knSmPaintBaseCosmetic\" "
                   "onclick=\"knSkins('paintBase','%d')\">Cosmetic</button></div> <span class=\"preview-base-note svelte-1qb7o10\">The paint is applied to this item in the "
                   "3D preview.</span></div></div></div>",
               SM.ed.paint_base, PAINT_BASE_WEAPON, PAINT_BASE_COSMETIC);
}

// ---- Wear (iíîîíii, svelte-e65miw, :126401-126616): four layers packed into texture_w.png, uploaded as the wear map

static void wear_section(StrBuf *sb) {
    const cJSON *cfg = editing_config();
    const int existing = cfg_true(cJSON_GetObjectItemCaseSensitive(cfg, "customTextures"), "wear");
    const int up = uploading("texture:wear");
    sb_appendf(sb, "<div class=\"section wear-section svelte-e65miw\" id=\"knSmWear\"><h3 class=\"section-title svelte-e65miw\"><i class=\"material-icons svelte-e65miw\">"
                   "healing</i> Wear</h3> <div class=\"section-content svelte-e65miw\"><p class=\"section-description svelte-e65miw\">Give this skin a wear map so unboxed "
                   "instances show rust, dirt, and scratches that scale with their wear level.\n        Upload the individual effect layers and they are packed into a single "
                   "wear texture.</p> <div class=\"wear-settings svelte-e65miw\"><label class=\"toggle-switch svelte-e65miw\"><span class=\"svelte-e65miw\">Wear</span> <input "
                   "type=\"checkbox\" class=\"svelte-e65miw\" id=\"knSmWearOn\" onchange=\"knSkins('wearOn',this.checked)\"%s> <span class=\"slider svelte-e65miw\"></span>"
                   "</label></div> ",
               SM.ed.wear_on ? " checked" : "");
    if (SM.ed.wear_on) {
        const char *err = upload_error("texture:wear");
        if (!err[0]) err = SM.ed.wear_error;
        if (err[0]) {
            sb_append(sb, "<div class=\"upload-error svelte-e65miw\" id=\"knSmWearError\"><i class=\"material-icons svelte-e65miw\">error</i> ");
            sb_text(sb, err);
            sb_append(sb, "</div>");
        }
        sb_append(sb, " ");
        if (existing) {
            char url[600];
            skin_url("texture_w.png", url, sizeof(url));
            sb_appendf(sb, "<div class=\"existing-texture svelte-e65miw\" id=\"knSmWearExisting\"><div class=\"texture-preview svelte-e65miw\"><img alt=\"Packed wear map\" "
                           "class=\"texture-image svelte-e65miw\" src=\"%s\"></div> <p class=\"texture-description svelte-e65miw\">Packed wear map uploaded. Upload new layers "
                           "below to replace it.</p></div>",
                       url);
        }
        sb_append(sb, " <div class=\"channel-uploads svelte-e65miw\">");
        int count = 0;
        for (int i = 0; i < 4; i++) {
            char slot[24], label[64];
            snprintf(slot, sizeof(slot), "wear:%s", WEAR_LAYERS[i].key);
            snprintf(label, sizeof(label), "Drop %s layer here", WEAR_LAYERS[i].label);
            for (char *p = label + 5; *p && strcmp(p, " layer here"); p++) *p = (char) tolower((unsigned char) *p);
            if (SM.ed.layer[i]) count++;
            sb_appendf(sb, "<div class=\"texture-upload-item svelte-e65miw\" id=\"knSmWear-%s\"><div class=\"texture-header svelte-e65miw\"><h5 class=\"svelte-e65miw\">%s</h5> ",
                       WEAR_LAYERS[i].key, WEAR_LAYERS[i].label);
            if (SM.ed.layer[i]) sb_append(sb, "<span class=\"ready-badge svelte-e65miw\">Ready</span>");
            sb_appendf(sb, "</div> <p class=\"texture-description svelte-e65miw\">%s</p> <div class=\"file-drop-container svelte-e65miw\">", WEAR_LAYERS[i].desc);
            const SlotState *st = slot_state(slot, 0);
            file_drop(sb, &DROP_WEAR, WEAR_LAYERS[i].key, slot, SM.ed.layer[i] && st && st->file_name[0] ? st->file_name : label);
            sb_append(sb, "</div></div>");
        }
        sb_appendf(sb, "</div> <div class=\"wear-actions svelte-e65miw\"><button type=\"button\" class=\"btn-generate svelte-e65miw\" id=\"knSmWearGenerate\" "
                       "onclick=\"knSkins('wearGenerate')\"");
        disabled_attr(sb, SM.ed.generating || up || !count);
        if (SM.ed.generating || up)
            sb_appendf(sb, "><i class=\"material-icons spinning svelte-e65miw\">sync</i> %s</button></div>", SM.ed.generating ? "Generating..." : "Uploading...");
        else sb_appendf(sb, "><i class=\"material-icons svelte-e65miw\">auto_fix_high</i> Generate &amp; Upload Wear Map (%d/4 layers)</button></div>", count);
    }
    sb_append(sb, "</div></div>");
}

// the layer's pixels drawn into the 1024 px canvas (drawImage, smoothed) and its red channel read back (iÏiìíiì)
static int wear_layer_red(const unsigned char *file, const size_t len, unsigned char *out) {
    int w = 0, h = 0, n = 0;
    unsigned char *px = stbi_load_from_memory(file, (int) len, &w, &h, &n, 4);
    if (!px || w <= 0 || h <= 0) {
        if (px) stbi_image_free(px);
        return 0;
    }
    for (int y = 0; y < WEAR_SIZE; y++) {
        const double fy = ((double) y + 0.5) * h / WEAR_SIZE - 0.5;
        const int y0 = fy < 0 ? 0 : (int) fy, y1 = y0 + 1 < h ? y0 + 1 : h - 1;
        const double ty = fy < 0 ? 0 : fy - y0;
        for (int x = 0; x < WEAR_SIZE; x++) {
            const double fx = ((double) x + 0.5) * w / WEAR_SIZE - 0.5;
            const int x0 = fx < 0 ? 0 : (int) fx, x1 = x0 + 1 < w ? x0 + 1 : w - 1;
            const double tx = fx < 0 ? 0 : fx - x0;
            // premultiplied, as the canvas keeps it; read back unpremultiplied (getImageData)
            double r = 0, a = 0;
            const int xs[2] = {x0, x1 < x0 ? x0 : x1}, ys[2] = {y0, y1 < y0 ? y0 : y1};
            const double wx[2] = {1 - tx, tx}, wy[2] = {1 - ty, ty};
            for (int j = 0; j < 2; j++)
                for (int i = 0; i < 2; i++) {
                    const unsigned char *p = px + ((size_t) ys[j] * (size_t) w + (size_t) xs[i]) * 4;
                    const double k = wx[i] * wy[j] * p[3] / 255.0;
                    r += k * p[0];
                    a += k;
                }
            const double v = a > 0 ? r / a : 0;
            out[(size_t) y * WEAR_SIZE + (size_t) x] = (unsigned char) (v < 0 ? 0 : v > 255 ? 255 : lround(v));
        }
    }
    stbi_image_free(px);
    return 1;
}

// Generate & Upload (iïííîîi, :120676-120705): each layer's red into its channel (the others 0), PNG, then
// onWearGenerated (:126583-126610): a saved skin's texture "wear" (texture_w.png), customTextures.wear on success
static void wear_generate(void) {
    int count = 0;
    for (int i = 0; i < 4; i++) count += SM.ed.layer[i] != NULL;
    if (!count || SM.ed.generating || uploading("texture:wear")) return;
    SM.ed.generating = 1;
    SM.ed.wear_error[0] = 0;
    unsigned char *rgba = calloc((size_t) WEAR_SIZE * WEAR_SIZE, 4);
    unsigned char *red = malloc((size_t) WEAR_SIZE * WEAR_SIZE);
    if (!rgba || !red) {
        snprintf(SM.ed.wear_error, sizeof(SM.ed.wear_error), "Failed to generate wear map");
        free(rgba);
        free(red);
        SM.ed.generating = 0;
        return;
    }
    for (int i = 0; i < 4 && !SM.ed.wear_error[0]; i++) {
        if (!SM.ed.layer[i]) continue;
        if (!wear_layer_red(SM.ed.layer[i], SM.ed.layer_len[i], red)) {
            char slot[24];
            snprintf(slot, sizeof(slot), "wear:%s", WEAR_LAYERS[i].key);
            const SlotState *st = slot_state(slot, 0);
            snprintf(SM.ed.wear_error, sizeof(SM.ed.wear_error), "Failed to decode %s", st && st->file_name[0] ? st->file_name : WEAR_LAYERS[i].key);
            break;
        }
        for (size_t p = 0; p < (size_t) WEAR_SIZE * WEAR_SIZE; p++) rgba[4 * p + (size_t) i] = red[p];
    }
    free(red);
    size_t len = 0;
    unsigned char *png = SM.ed.wear_error[0] ? NULL : png_encode_rgba(rgba, WEAR_SIZE, WEAR_SIZE, &len);
    free(rgba);
    SM.ed.generating = 0;
    if (SM.ed.wear_error[0]) return;
    if (!png) {
        snprintf(SM.ed.wear_error, sizeof(SM.ed.wear_error), "PNG encode failed");
        return;
    }
    fprintf(stderr, "[skins] wear map packed: %d layer(s), %zu bytes\n", count, len);
    if (!editing_id()) {
        snprintf(SM.ed.wear_error, sizeof(SM.ed.wear_error), "Please save the skin first before uploading a wear map");
        free(png);
        return;
    }
    if (SM.up.active) {
        snprintf(SM.ed.wear_error, sizeof(SM.ed.wear_error), "Another upload is still running");
        free(png);
        return;
    }
    set_upload_error("texture:wear", "");
    up_start("texture:wear", "texture", "wear", "texture_w.png", png, len);
}

// the Wear toggle (iÎïìiïí): off drops the layers, and an uploaded map (onWearRemoved: its texture removed)
static void wear_toggle(const int on) {
    SM.ed.wear_on = on;
    if (on) return;
    wear_layers_clear();
    SM.ed.wear_error[0] = 0;
    if (cfg_true(cJSON_GetObjectItemCaseSensitive(editing_config(), "customTextures"), "wear")) {
        set_upload_error("texture:wear", "");
        unset_asset("texture", "wear");
    }
}

// ---- Kounter (iïíìiïí, svelte-1rov8jm, :126656-126848): itemConfig.attach, a stat counter on the weapon

static const cJSON *attach_of(void) {
    const cJSON *a = cJSON_GetObjectItemCaseSensitive(editing_config(), "attach");
    return cJSON_IsObject(a) ? a : NULL;
}

// iíììíîï (:106684): the attach's stat when it is one of the eight, else "kills"
static const char *attach_stat(const cJSON *attach) {
    const char *s = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(attach, "stat"));
    for (int i = 0; s && i < 8; i++)
        if (!strcmp(s, KOUNTER_STATS[i])) return s;
    return "kills";
}

static void place_tools(StrBuf *sb, const char *hash, const char *op, const int mode, const int rotate) {
    sb_appendf(sb, "<div class=\"tools svelte-%s\">", hash);
    for (int i = 0; i < 5; i++) {
        if (i == 3 && !rotate) continue; // the charm has no Rotate
        sb_appendf(sb, "<button class=\"tool-btn svelte-%s%s\" title=\"%s\" id=\"knSm%sTool-%s\" onclick=\"knSkins('%s','%s')\"><span class=\"material-icons svelte-%s\">%s</span> %s</button>",
                   hash, mode == i ? " active" : "", PLACE_TOOLS[i].label, op, PLACE_TOOLS[i].mode, op, PLACE_TOOLS[i].mode, hash, PLACE_TOOLS[i].icon, PLACE_TOOLS[i].label);
    }
    sb_append(sb, "</div> ");
}

static void kounter_section(StrBuf *sb) {
    const cJSON *a = attach_of();
    sb_append(sb, "<div class=\"section svelte-1rov8jm\" id=\"knSmKounter\"><h3 class=\"section-title svelte-1rov8jm\"><span class=\"material-icons svelte-1rov8jm\">pin</span> "
                  "Kounter</h3> <div class=\"section-content\"><p class=\"section-description svelte-1rov8jm\">A Kounter is mounted on the weapon and tracks one stat for this "
                  "exact item. Kounter skins are never stackable.</p> ");
    if (!a) {
        sb_append(sb, "<button class=\"attach-btn svelte-1rov8jm\" id=\"knSmKounterAdd\" onclick=\"knSkins('attachAdd')\"><span class=\"material-icons svelte-1rov8jm\">add</span> "
                      "Add Kounter</button></div></div>");
        return;
    }
    place_tools(sb, "1rov8jm", "attachMode", SM.ed.attach_mode, 1);
    sb_appendf(sb, "<p class=\"hint svelte-1rov8jm\">%s</p> <div class=\"fields svelte-1rov8jm\">",
               SM.ed.attach_mode == 1 ? "Click the weapon in the preview to mount the Kounter on that surface."
               : !SM.ed.attach_mode   ? "The preview spins freely. Pick a tool to hold it still and place the Kounter."
                                      : "Drag the gizmo in the preview. Drag anywhere else to look around.");
    static const char *const axes[3] = {"X", "Y", "Z"}, *const keys[3] = {"x", "y", "z"};
    char v[40];
    for (int i = 0; i < 3; i++) { // attach[x] ?? 0
        const cJSON *n = cJSON_GetObjectItemCaseSensitive(a, keys[i]);
        js_number(v, sizeof(v), cJSON_IsNumber(n) ? n->valuedouble : 0);
        sb_appendf(sb, "<label class=\"svelte-1rov8jm\"><span>%s</span> <input type=\"number\" step=\"0.01\" class=\"svelte-1rov8jm\" id=\"knSmKounter%s\" value=\"%s\" "
                       "onchange=\"knSkins('attachSet','%s',this.value)\"></label>",
                   axes[i], axes[i], v, keys[i]);
    }
    const cJSON *rot = cJSON_GetObjectItemCaseSensitive(a, "rot");
    for (int i = 0; i < 3; i++) { // Math.round(180 * (rot[i] || 0) / PI)
        const cJSON *n = cJSON_GetArrayItem(rot, i);
        const double deg = floor(180.0 * (cJSON_IsNumber(n) ? n->valuedouble : 0) / M_PI + 0.5);
        js_number(v, sizeof(v), deg);
        sb_appendf(sb, "<label class=\"svelte-1rov8jm\"><span>Rot %s (deg)</span> <input type=\"number\" step=\"1\" class=\"svelte-1rov8jm\" id=\"knSmKounterRot%s\" value=\"%s\" "
                       "onchange=\"knSkins('attachRot',%d,this.value)\"></label>",
                   axes[i], axes[i], v, i);
    }
    const cJSON *scl = cJSON_GetObjectItemCaseSensitive(a, "scl"), *digits = cJSON_GetObjectItemCaseSensitive(a, "digits");
    js_number(v, sizeof(v), cJSON_IsNumber(scl) ? scl->valuedouble : 1);
    sb_appendf(sb, " <label class=\"svelte-1rov8jm\"><span>Scale</span> <input type=\"number\" step=\"0.01\" min=\"0.01\" class=\"svelte-1rov8jm\" id=\"knSmKounterScale\" value=\"%s\" "
                   "onchange=\"knSkins('attachSet','scl',this.value)\"></label>",
               v);
    js_number(v, sizeof(v), cJSON_IsNumber(digits) ? digits->valuedouble : 5);
    sb_appendf(sb, " <label class=\"svelte-1rov8jm\"><span>Digits</span> <input type=\"number\" step=\"1\" min=\"1\" class=\"svelte-1rov8jm\" max=\"10\" id=\"knSmKounterDigits\" "
                   "value=\"%s\" onchange=\"knSkins('attachSet','digits',this.value)\"></label> <label class=\"svelte-1rov8jm\"><span>Tracks</span> <select class=\"svelte-1rov8jm\" "
                   "id=\"knSmKounterStat\" onchange=\"knSkins('attachStat',this.value)\">",
               v);
    const char *stat = attach_stat(a);
    for (int i = 0; i < 8; i++)
        sb_appendf(sb, "<option value=\"%s\"%s>%s</option>", KOUNTER_STATS[i], !strcmp(stat, KOUNTER_STATS[i]) ? " selected" : "", KOUNTER_LABELS[i]);
    const char *color = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(a, "color"));
    sb_append(sb, "</select></label> <label class=\"svelte-1rov8jm\"><span>Glow</span> <input type=\"color\" class=\"svelte-1rov8jm\" id=\"knSmKounterGlow\" value=\"");
    sb_text(sb, color && color[0] ? color : "#7fe3ff");
    sb_append(sb, "\" onchange=\"knSkins('attachColor',this.value)\"></label></div> <button class=\"attach-btn danger svelte-1rov8jm\" id=\"knSmKounterRemove\" "
                  "onclick=\"knSkins('attachRemove')\"><span class=\"material-icons svelte-1rov8jm\">delete</span> Remove Kounter</button></div></div>");
}

// iIîìïïì: itemConfig.attach set (NULL deletes it)
static void attach_write(cJSON *attach) {
    put(editing_config(), "attach", attach);
    changed();
}

// ---- Charm (iìîïîïi, svelte-sr5bk2, :126848-126978): charmM{X,Y,Z}Off / charmMScl, noCharm

// iïïíîíï (:120758): the weapon's charm mount (weapons[weapon - 1].charm), NULL when it takes none
static const ViewerCharmMount *weapon_charm(void) {
    const int w = (int) jnum(SM.editing, "weapon");
    return w > 0 ? viewer_charm_mount(w - 1) : NULL;
}

// iììïïíí (:120762-120774): the skin's own placement when it sets any of the four keys, else none
static int charm_placement(double out[4]) {
    const ViewerCharmMount *m = weapon_charm();
    const cJSON *cfg = editing_config();
    int any = 0;
    for (int i = 0; i < 4; i++) {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(cfg, CHARM_KEYS[i]);
        any |= v && !cJSON_IsNull(v);
    }
    if (!m || !any) return 0;
    for (int i = 0; i < 3; i++) { // skin || weapon || 0
        const double v = jnum(cfg, CHARM_KEYS[i]);
        out[i] = v != 0 ? v : js_float(m->mount[i]);
    }
    const double s = jnum(cfg, "charmMScl");
    out[3] = s != 0 ? s : 1;
    return 1;
}

// iìiîìíì (:120779-120792): the four keys go; a placement sets them (never exactly 0: ±0.001), the scale at least 0.05
static void charm_write(const double *p) {
    cJSON *cfg = editing_config();
    for (int i = 0; i < 4; i++) put(cfg, CHARM_KEYS[i], NULL);
    if (p) {
        for (int i = 0; i < 3; i++) put(cfg, CHARM_KEYS[i], cJSON_CreateNumber(fabs(p[i]) < 0.001 ? (p[i] < 0 ? -0.001 : 0.001) : p[i]));
        put(cfg, "charmMScl", cJSON_CreateNumber(p[3] != 0 ? (p[3] > 0.05 ? p[3] : 0.05) : 1));
    }
    changed();
}

static void charm_section(StrBuf *sb) {
    const int hidden = cfg_true(editing_config(), "noCharm");
    sb_appendf(sb, "<div class=\"section svelte-sr5bk2\" id=\"knSmCharm\"><h3 class=\"section-title svelte-sr5bk2\"><span class=\"material-icons svelte-sr5bk2\">key</span> Charm</h3> "
                   "<div class=\"section-content\"><p class=\"section-description svelte-sr5bk2\">Where a player's charm hangs on this skin. Without a custom placement the "
                   "charm uses the weapon's default spot.</p> <label class=\"hide-toggle svelte-sr5bk2\"><input type=\"checkbox\" id=\"knSmNoCharm\" "
                   "onchange=\"knSkins('noCharm',this.checked)\"%s> Hide charms on this skin</label> ",
               hidden ? " checked" : "");
    double p[4];
    if (!hidden && !charm_placement(p)) {
        sb_append(sb, "<button class=\"attach-btn svelte-sr5bk2\" id=\"knSmCharmCustomize\" onclick=\"knSkins('charmCustomize')\"><span class=\"material-icons svelte-sr5bk2\">tune"
                      "</span> Customize charm placement</button>");
    } else if (!hidden) {
        place_tools(sb, "sr5bk2", "charmMode", SM.ed.charm_mode, 0);
        sb_appendf(sb, "<p class=\"hint svelte-sr5bk2\">%s</p> <div class=\"fields svelte-sr5bk2\">",
                   SM.ed.charm_mode == 1 ? "Click the weapon in the preview to hang the charm from that spot."
                   : !SM.ed.charm_mode   ? "The preview spins freely. Pick a tool to hold it still and place the charm."
                                         : "Drag the gizmo in the preview. Drag anywhere else to look around.");
        static const char *const axes[3] = {"X", "Y", "Z"}, *const keys[3] = {"x", "y", "z"};
        char v[40];
        for (int i = 0; i < 3; i++) {
            js_number(v, sizeof(v), p[i]);
            sb_appendf(sb, "<label class=\"svelte-sr5bk2\"><span>%s</span> <input type=\"number\" step=\"0.01\" class=\"svelte-sr5bk2\" id=\"knSmCharm%s\" value=\"%s\" "
                           "onchange=\"knSkins('charmSet','%s',this.value)\"></label>",
                       axes[i], axes[i], v, keys[i]);
        }
        js_number(v, sizeof(v), p[3]);
        sb_appendf(sb, " <label class=\"svelte-sr5bk2\"><span>Scale</span> <input type=\"number\" step=\"0.05\" min=\"0.05\" class=\"svelte-sr5bk2\" id=\"knSmCharmScale\" value=\"%s\" "
                       "onchange=\"knSkins('charmSet','scl',this.value)\"></label></div> <button class=\"attach-btn danger svelte-sr5bk2\" id=\"knSmCharmReset\" "
                       "onclick=\"knSkins('charmReset')\"><span class=\"material-icons svelte-sr5bk2\">restart_alt</span> Reset to weapon default</button>",
                   v);
    }
    sb_append(sb, "</div></div>");
}

// ---- the preview's controls (:127496-127530): Wear (its slider) and a paint, with the query they add (iIìííiî)

static int preview_wear_control(void) { // iÌíïïîï: customTextures.wear or wearable
    const cJSON *cfg = editing_config();
    return cfg_true(cJSON_GetObjectItemCaseSensitive(cfg, "customTextures"), "wear") || cfg_true(cfg, "wearable");
}

static int preview_paint_control(void) { return cfg_true(editing_config(), "paintTargets"); } // iÎiîîîï

// iÏíiííì: the catalogue's paints (a paintColor; the store's shader paints carry no paintShader in this catalogue)
static int is_preview_paint(const ViewerSkin *s) { return s && s->type == T_PAINT && s->paint_color && s->paint_color[0]; }

// the query after the showcase (iIìííiî :124880-124889): the Kounter's stat at 1284, the wear the slider set
// (KrunkNative: as &wear=<level>, the iframe's updateWear message folded into its query), the paint picked
static void preview_extra(StrBuf *sb) {
    sb_appendf(sb, "&%s=1284", attach_stat(attach_of()));
    if (SM.ed.wear_touched) {
        char v[24];
        js_number(v, sizeof(v), SM.ed.wear_level);
        sb_appendf(sb, "&wear=%s", v);
    }
    const ViewerSkin *p = SM.ed.paint_pick >= 0 ? viewer_skin(SM.ed.paint_pick) : NULL;
    if (is_preview_paint(p)) sb_appendf(sb, "&paint=%s", p->paint_color[0] == '#' ? p->paint_color + 1 : p->paint_color);
}

static void preview_controls(StrBuf *sb) {
    const int wear = preview_wear_control(), paint = preview_paint_control();
    if (!wear && !paint) return;
    sb_append(sb, " <div class=\"preview-controls svelte-103lu6x\">");
    if (wear) {
        sb_appendf(sb, "<button class=\"preview-control-btn svelte-103lu6x%s\" id=\"knSmPvWear\" onclick=\"knSkins('pvWear')\"><i class=\"material-icons svelte-103lu6x\">healing</i> "
                       "Wear</button> ",
                   SM.ed.wear_open ? " active" : "");
        if (SM.ed.wear_open) {
            char v[24];
            js_number(v, sizeof(v), SM.ed.wear_level);
            sb_appendf(sb, "<input type=\"range\" min=\"0\" max=\"1\" step=\"0.01\" class=\"wear-slider svelte-103lu6x\" id=\"knSmPvWearLevel\" value=\"%s\" "
                           "oninput=\"knSkins('pvWearLevel',this.value)\"> <span class=\"wear-value svelte-103lu6x\" id=\"knSmPvWearValue\">%.2f</span>",
                       v, SM.ed.wear_level);
        }
    }
    sb_append(sb, " ");
    if (paint) {
        sb_appendf(sb, "<select class=\"paint-select svelte-103lu6x\" id=\"knSmPvPaint\" onchange=\"knSkins('pvPaint',this.value)\"><option class=\"svelte-103lu6x\" value=\"\"%s>"
                       "No paint</option>",
                   SM.ed.paint_pick < 0 ? " selected" : "");
        for (int i = 0; i < g_viewer_skin_count; i++) {
            const ViewerSkin *s = viewer_skin(i);
            if (!is_preview_paint(s) || !s->name) continue;
            sb_appendf(sb, "<option class=\"svelte-103lu6x\" value=\"%d\"%s>", i, i == SM.ed.paint_pick ? " selected" : "");
            sb_text(sb, s->name);
            sb_append(sb, "</option>");
        }
        sb_append(sb, "</select>");
    }
    sb_append(sb, "</div>");
}

// the editors' actions; 1 = handled (the window renders again), 2 = handled, no new markup
static int editors_op(const char *op, const HtmlArg *args, const int argc) {
    cJSON *cfg = editing_config();
    if (!strcmp(op, "paintable")) { // iÏiïiïi
        put(cfg, "paintTargets", arg_i64(args, argc, 1) ? cJSON_CreateTrue() : NULL);
    } else if (!strcmp(op, "paintArea")) { // iÌïïìiì: "all" is true, "specific" the mesh names
        put(cfg, "paintTargets", !strcmp(arg_s(args, argc, 1), "all") ? cJSON_CreateTrue() : mesh_list(SM.ed.targets));
    } else if (!strcmp(op, "paintMode")) { // iÍïïìïì: Animated Shader keeps a graph (the default paint one), Solid Colour drops it
        const int shader = !strcmp(arg_s(args, argc, 1), "shader");
        if (shader == paint_shader_mode()) return 2;
        if (shader) {
            put(cfg, "paintColor", NULL);
            sg_editor_flush(paint_canvas());
            const cJSON *g = sg_editor_graph(paint_canvas());
            if (!graph_ok(g)) {
                sg_editor_set_graph(paint_canvas(), NULL);
                g = sg_editor_graph(paint_canvas());
            }
            put(cfg, "paintShaderGraph", g ? cJSON_Duplicate(g, 1) : NULL);
        } else put(cfg, "paintShaderGraph", NULL);
        put(cfg, "paintShaderCode", NULL);
        put(cfg, "paintShaderUniforms", NULL);
    } else if (!strcmp(op, "paintColor")) { // iÌïïïiì: the colour, "" deletes it
        const char *c = arg_s(args, argc, 1);
        const int had = cJSON_IsString(cJSON_GetObjectItemCaseSensitive(cfg, "paintColor"));
        put(cfg, "paintColor", c[0] ? cJSON_CreateString(c) : NULL);
        put(cfg, "paintShaderCode", NULL);
        put(cfg, "paintShaderUniforms", NULL);
        changed();
        if (had && c[0]) { // the picker keeps its drag: only the shown value follows
            const HtmlElem out = html_dom_by_id("knSmPaintColorValue");
            if (out) html_dom_set_html(out, c);
            return 2;
        }
        return 1;
    } else if (!strcmp(op, "paintBase")) { // iîiììiì: parseInt || the one it was
        const char *t = arg_s(args, argc, 1);
        const int n = t[0] ? atoi(t) : (int) arg_i64(args, argc, 1);
        if (n) SM.ed.paint_base = n;
        SM.preview_due = now_ms() + PREVIEW_DEBOUNCE_MS;
        return 1;
    } else if (!strcmp(op, "wearOn")) {
        wear_toggle(arg_i64(args, argc, 1) != 0);
        return 1;
    } else if (!strcmp(op, "wearGenerate")) {
        wear_generate();
        return 1;
    } else if (!strcmp(op, "attachAdd")) { // iïïííïï: the default Kounter (iIîìîií :126662-126671)
        cJSON *a = cJSON_CreateObject();
        cJSON_AddNumberToObject(a, "type", 1);
        cJSON_AddNumberToObject(a, "x", -0.4);
        cJSON_AddNumberToObject(a, "y", 0.1);
        cJSON_AddNumberToObject(a, "z", 0.5);
        const double rot[3] = {0, -1.571, 0};
        cJSON_AddItemToObject(a, "rot", cJSON_CreateDoubleArray(rot, 3));
        cJSON_AddNumberToObject(a, "scl", 0.36);
        cJSON_AddStringToObject(a, "color", "#7fe3ff");
        cJSON_AddNumberToObject(a, "digits", 5);
        attach_write(a);
        return 1;
    } else if (!strcmp(op, "attachRemove")) { // iìîiîíí: no attach, the View tool
        attach_write(NULL);
        SM.ed.attach_mode = 0;
        return 1;
    } else if (!strcmp(op, "attachMode") || !strcmp(op, "charmMode")) { // the tools; one of the two at a time (:124633-124638)
        const int m = tool_index(arg_s(args, argc, 1));
        if (!strcmp(op, "attachMode")) SM.ed.attach_mode = m, SM.ed.charm_mode = m ? 0 : SM.ed.charm_mode;
        else SM.ed.charm_mode = m, SM.ed.attach_mode = m ? 0 : SM.ed.attach_mode;
        return 1;
    } else if (!strcmp(op, "attachSet")) { // iÎììíîî: parseFloat; digits rounded into 1..10
        const cJSON *a = attach_of();
        const char *key = arg_s(args, argc, 1), *t = arg_s(args, argc, 2);
        char *end = NULL;
        double v = strtod(t, &end);
        if (!a || end == t || (strcmp(key, "x") && strcmp(key, "y") && strcmp(key, "z") && strcmp(key, "scl") && strcmp(key, "digits"))) return 2;
        if (!strcmp(key, "digits")) v = fmin(10, fmax(1, floor(v + 0.5)));
        cJSON *n = cJSON_Duplicate(a, 1);
        put(n, key, cJSON_CreateNumber(v));
        attach_write(n);
        return 1;
    } else if (!strcmp(op, "attachRot")) { // degrees to radians, to 0.001
        const cJSON *a = attach_of();
        const int i = (int) arg_i64(args, argc, 1);
        const char *t = arg_s(args, argc, 2);
        char *end = NULL;
        const double v = strtod(t, &end);
        if (!a || end == t || i < 0 || i > 2) return 2;
        double rot[3] = {0, 0, 0};
        const cJSON *r = cJSON_GetObjectItemCaseSensitive(a, "rot");
        for (int k = 0; k < 3; k++) {
            const cJSON *it = cJSON_GetArrayItem(r, k);
            rot[k] = cJSON_IsNumber(it) ? it->valuedouble : 0;
        }
        rot[i] = floor(v * M_PI / 180.0 * 1000.0 + 0.5) / 1000.0;
        cJSON *n = cJSON_Duplicate(a, 1);
        put(n, "rot", cJSON_CreateDoubleArray(rot, 3));
        attach_write(n);
        return 1;
    } else if (!strcmp(op, "attachStat") || !strcmp(op, "attachColor")) { // iìîîìïî / iíïiiiì
        const cJSON *a = attach_of();
        if (!a) return 2;
        cJSON *n = cJSON_Duplicate(a, 1);
        put(n, !strcmp(op, "attachStat") ? "stat" : "color", cJSON_CreateString(arg_s(args, argc, 1)));
        attach_write(n);
        return 1;
    } else if (!strcmp(op, "noCharm")) { // iÍìîíìì: hidden charms drop the tool
        const int on = arg_i64(args, argc, 1) != 0;
        put(cfg, "noCharm", on ? cJSON_CreateTrue() : NULL);
        if (on) SM.ed.charm_mode = 0;
    } else if (!strcmp(op, "charmCustomize")) { // iìíîìíi: the weapon's spot, scale 1
        const ViewerCharmMount *m = weapon_charm();
        if (!m) return 2;
        const double p[4] = {js_float(m->mount[0]), js_float(m->mount[1]), js_float(m->mount[2]), 1};
        charm_write(p);
        return 1;
    } else if (!strcmp(op, "charmReset")) { // iîíìiïi
        charm_write(NULL);
        SM.ed.charm_mode = 0;
        return 1;
    } else if (!strcmp(op, "charmSet")) { // iÍíîíîï: parseFloat into the placement
        double p[4];
        const char *key = arg_s(args, argc, 1), *t = arg_s(args, argc, 2);
        char *end = NULL;
        const double v = strtod(t, &end);
        if (end == t || !charm_placement(p)) return 2;
        const int k = !strcmp(key, "x") ? 0 : !strcmp(key, "y") ? 1 : !strcmp(key, "z") ? 2 : !strcmp(key, "scl") ? 3 : -1;
        if (k < 0) return 2;
        p[k] = v;
        charm_write(p);
        return 1;
    } else if (!strcmp(op, "pvWear")) { // iìïiîíï: the slider; the first time the query asks for the wear
        SM.ed.wear_open = !SM.ed.wear_open;
        if (SM.ed.wear_open && !SM.ed.wear_touched) {
            SM.ed.wear_touched = 1;
            SM.preview_due = now_ms() + PREVIEW_DEBOUNCE_MS;
        }
        return 1;
    } else if (!strcmp(op, "pvWearLevel")) { // updateWear (iÍìíîìì)
        const double v = atof(arg_s(args, argc, 1));
        SM.ed.wear_level = v < 0 ? 0 : v > 1 ? 1 : v;
        SM.preview_due = now_ms() + PREVIEW_DEBOUNCE_MS;
        const HtmlElem out = html_dom_by_id("knSmPvWearValue");
        if (out) {
            char t[16];
            snprintf(t, sizeof(t), "%.2f", SM.ed.wear_level);
            html_dom_set_html(out, t);
        }
        return 2; // the slider keeps its drag
    } else if (!strcmp(op, "pvPaint")) {
        const char *t = arg_s(args, argc, 1);
        SM.ed.paint_pick = t[0] ? atoi(t) : -1;
        SM.preview_due = now_ms() + PREVIEW_DEBOUNCE_MS;
        return 1;
    } else return 0;
    changed();
    return 1;
}

// iïìiiìì (:124527, :125610): Same attributes as (not for a paint), Dye Colors (a dye), Model (not for a spray, card,
// emoji, face, dye or paint), Textures (not with a GLTF / GLB model or for a dye), Paintable (not for those six types),
// Paint (a paint), Wear and Kounter (a weapon or melee), Charm (a weapon whose weapon takes a charm), Sounds (not for
// those six types), Shader Studio (not for a paint)
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
    const char *ext = jstr(editing_config(), "fileExt");
    const int no_model = type == 4 || type == T_PLAYER_CARD || type == T_EMOJI || type == 7 || type == 5 || type == T_PAINT; // iÌiìiìï
    if (type == 5) dye_section(sb);
    if (!no_model) model_section(sb);
    if (!(!strcmp(ext, "gltf") || !strcmp(ext, "glb") || type == 5)) textures_section(sb, type); // iÎîììïï hides it
    editors_load();
    if (!no_model) paintable_section(sb); // iIíïììi
    if (type == T_PAINT) paint_section(sb);
    if (type == 0 || type == 3) wear_section(sb), kounter_section(sb);
    if (type == 0 && weapon_charm()) charm_section(sb);
    if (!no_model) sounds_section(sb, type);
    if (type != T_PAINT) shader_section(sb);
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

// the iframe's src: a paint on its preview item (paintPreviewBase, :127470; its colour as the viewer's &paint=, the
// paintPreview message's place), else the edited skin's JSON and the controls' query (preview_extra)
static char *preview_src(void) {
    StrBuf sb = {0};
    if ((int) jnum(SM.editing, "type") == T_PAINT) {
        sb_appendf(&sb, "/viewer.html?nobg&anim&showcase=%d", SM.ed.paint_base ? SM.ed.paint_base : PAINT_BASE_WEAPON);
        const char *c = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(editing_config(), "paintColor"));
        if (c && c[0] && !paint_shader_mode()) sb_appendf(&sb, "&paint=%s", c[0] == '#' ? c + 1 : c);
    } else {
        sb_appendf(&sb, "/viewer.html?nobg&showcase=%s", SM.preview ? SM.preview : "");
        preview_extra(&sb);
    }
    char *src = sb_finish(&sb);
    static char *shown; // the e2e's probe: each new src once (the JSON's long tail left out)
    if (src && (!shown || strcmp(shown, src))) {
        free(shown);
        shown = strdup(src);
        const char *q = strstr(src, "showcase=");
        const char *tail = q ? strchr(q, '&') : NULL;
        if (q && (tail ? tail - q : (long) strlen(q)) <= 24) q = NULL, tail = NULL; // an index: the whole src
        fprintf(stderr, "[skins] preview %.*s%s\n", (int) (q ? q - src + 9 : (long) strlen(src)), src, tail ? tail : "");
    }
    return src;
}

// the preview pane (iiìíiiì, :127463-127540) once its JSON is set: a paint's animated preview, a player card's /
// emoji's texture (or thumbnail), else the 3D preview with its wear / paint controls
static void preview_markup(StrBuf *sb) {
    if (!SM.preview) {
        sb_append(sb, "<!---->");
        return;
    }
    const int type = (int) jnum(SM.editing, "type");
    sb_append(sb, "<div class=\"preview-3d svelte-103lu6x\">");
    if (type == T_PLAYER_CARD || type == T_EMOJI) {
        char url[600];
        if (cfg_true(cJSON_GetObjectItemCaseSensitive(editing_config(), "customTextures"), "diffuse")) skin_url("texture.png", url, sizeof(url));
        else if (cfg_true(editing_config(), "customThumbnail")) skin_url("thumbnail.png", url, sizeof(url));
        else url[0] = 0;
        if (url[0]) sb_appendf(sb, "<img alt=\"Thumbnail\" class=\"preview-frame svelte-103lu6x\" style=\"object-fit: contain; border: none\" id=\"knSmPreviewImg\" src=\"%s\">", url);
    } else {
        char *src = preview_src();
        sb_appendf(sb, "<iframe class=\"preview-frame svelte-103lu6x\" style=\"border: none\" title=\"%s\" id=\"knSmPreview\" src=\"",
                   type == T_PAINT ? "Paint Preview" : "3D Preview");
        sb_text(sb, src ? src : "");
        sb_append(sb, "\"></iframe>");
        free(src);
        if (type != T_PAINT) preview_controls(sb);
    }
    sb_append(sb, "</div>");
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
    sb_append(sb, "</div> ");
    preview_markup(sb);
    sb_append(sb, "</div></div>");
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
            sb_appendf(sb, "<button class=\"io-action-bar-button svelte-103lu6x%s\" id=\"knSmCapture\" onclick=\"knSkins('capture')\" title=\"%s\"",
                       SM.capturing ? " is-loading" : "", editing_id() ? "Position the preview, then capture it as the item thumbnail" : "Save the skin first");
            disabled_attr(sb, SM.busy || SM.capturing || !editing_id());
            sb_appendf(sb, "><i class=\"material-icons svelte-103lu6x\">%s</i> %s</button>", SM.capturing ? "sync" : SM.captured ? "check" : "photo_camera",
                       SM.capturing ? "Capturing..." : SM.captured ? "Captured" : "Capture Thumbnail");
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
    free(SM.preview);
    free(SM.up.data);
    cJSON_Delete(SM.sh.graph);
    cJSON_Delete(SM.sh.parts);
    sg_editor_free(SM.sh.canvas);
    sg_editor_free(SM.ed.paint_canvas);
    for (int i = 0; i < 4; i++) free(SM.ed.layer[i]);
    const unsigned gen = SM.gen + 1, rev = SM.asset_rev;
    memset(&SM, 0, sizeof(SM));
    SM.gen = gen;
    SM.asset_rev = rev; // the files' ?v= stays new: a reopened window must not show the cached old file
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

static void changed(void) { // onSkinChange (iÍíìîïí): the skin is unsaved, and the preview follows 300 ms on
    SM.saved = 0;
    SM.preview_due = now_ms() + PREVIEW_DEBOUNCE_MS;
}

// the editor opens on a skin: its preview, its glow toggle (hasGlow: an emissive texture, never for a player card),
// Shader Studio's state, the sections' errors (each section component is new)
static void editor_opened(void) {
    free(SM.preview);
    SM.preview = NULL;
    SM.preview_due = now_ms() + PREVIEW_DEBOUNCE_MS;
    SM.captured = SM.capturing = 0;
    SM.has_glow = cfg_true(cJSON_GetObjectItemCaseSensitive(editing_config(), "customTextures"), "emissive") && (int) jnum(SM.editing, "type") != T_PLAYER_CARD;
    SM.model_ext[0] = 0;
    memset(SM.slots, 0, sizeof(SM.slots));
    SM.sh.loaded = 0;
    SM.sh.open = SM.sh.preview = 0;
    SM.ed.ready = 0;
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
    } else if (!strcmp(what, "paintMeshes")) { // iÎiiiîï: the text kept, paintTargets its names
        snprintf(SM.ed.targets, sizeof(SM.ed.targets), "%s", v);
        put(editing_config(), "paintTargets", mesh_list(v));
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
    editor_opened();
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
    editor_opened();
    do_save();
}


// ------------------------------------------------------------------ uploads (uploadAsset, :124434-124459)

static unsigned char *read_whole(const char *path, size_t *len) {
    *len = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    unsigned char *d = NULL;
    if (!fseek(f, 0, SEEK_END)) {
        const long n = ftell(f);
        if (n >= 0 && !fseek(f, 0, SEEK_SET) && (d = malloc((size_t) n + 1)) && fread(d, 1, (size_t) n, f) != (size_t) n) {
            free(d);
            d = NULL;
        }
        if (d) *len = (size_t) n;
    }
    fclose(f);
    return d;
}

static const char *base_name(const char *path) {
    const char *a = strrchr(path, '/'), *b = strrchr(path, '\\');
    const char *s = a > b ? a : b;
    return s ? s + 1 : path;
}

// '.' + name.split('.').pop().toLowerCase()
static void name_ext(const char *name, char *out, const size_t n) {
    const char *dot = strrchr(name, '.');
    const char *e = dot ? dot + 1 : name;
    size_t i = 0;
    out[i++] = '.';
    for (; *e && i + 1 < n; e++) out[i++] = (char) (*e >= 'A' && *e <= 'Z' ? *e + 32 : *e);
    out[i] = 0;
}

static void apply_config(void);

// one upload's end: its error on its section (the message, else "Upload failed"), then applyConfig (iÍïîîíî), which
// every section's upload ends with
static void up_end(const char *err) {
    if (!SM.up.active) return;
    if (err && !strcmp(SM.up.type, "thumbnail")) {
        SM.capturing = 0;
        snprintf(SM.error, sizeof(SM.error), "Failed to upload thumbnail: %s", err);
    } else if (err) set_upload_error(SM.up.slot, err[0] ? err : "Upload failed");
    const int thumb = !strcmp(SM.up.type, "thumbnail");
    free(SM.up.data);
    const unsigned gen = SM.up.gen;
    memset(&SM.up, 0, sizeof(SM.up));
    SM.up.gen = gen;
    if (!thumb) apply_config();
    render();
}

// the stored file: the local copy (asset_fetch_client_put, as the browser's next load gets it), the files' ?v=, then
// the section's success (:125367-125378, :125420-125435, :125486, :124778-124786)
static void up_stored(const char *asset_path) {
    char want[48];
    snprintf(want, sizeof(want), "skins/%lld/", (long long) SM.up.skin);
    if (!strncmp(asset_path, want, strlen(want)) && !strstr(asset_path, "..")) {
        if (!strcmp(SM.up.type, "model")) { // the brain dropped the model of another format
            static const char *const exts[] = {"obj", "gltf", "glb", "fbx"};
            for (size_t i = 0; i < sizeof(exts) / sizeof(exts[0]); i++) {
                char rel[96];
                snprintf(rel, sizeof(rel), "%smodel.%s", want, exts[i]);
                if (strcmp(rel, asset_path)) asset_fetch_client_put(rel, NULL, 0);
            }
        }
        asset_fetch_client_put(asset_path, SM.up.data, SM.up.len);
    }
    SM.asset_rev++;
    fprintf(stderr, "[skins] uploaded %s %s %s (%zu bytes)\n", SM.up.type, SM.up.category[0] ? SM.up.category : "-", asset_path, SM.up.len);
    cJSON *cfg = SM.editing && editing_id() == SM.up.skin ? editing_config() : NULL;
    if (!cfg) {
        up_end(NULL);
        return;
    }
    if (!strcmp(SM.up.type, "thumbnail")) { // itemConfig.customThumbnail, updateSkin
        put(cfg, "customThumbnail", cJSON_CreateTrue());
        char path[48];
        snprintf(path, sizeof(path), "/skins/%lld", (long long) SM.up.skin);
        send_req("PUT", path, cJSON_Duplicate(SM.editing, 1), R_THUMB);
    } else if (!strcmp(SM.up.type, "model")) {
        put(cfg, "customModel", cJSON_CreateTrue());
        changed();
    } else if (!strcmp(SM.up.type, "texture")) {
        if (!((int) jnum(SM.editing, "type") == T_PLAYER_CARD && !strcmp(SM.up.category, "emissive"))) {
            cJSON *tex = cJSON_GetObjectItemCaseSensitive(cfg, "customTextures");
            if (!cJSON_IsObject(tex)) {
                put(cfg, "customTextures", cJSON_CreateObject());
                tex = cJSON_GetObjectItemCaseSensitive(cfg, "customTextures");
            }
            put(tex, SM.up.category, cJSON_CreateTrue());
            if (!strcmp(SM.up.category, "emissive")) put(cfg, "glow", cJSON_CreateTrue());
            if (!strcmp(SM.up.category, "wear")) wear_layers_clear(); // iïííîîi: the layers once the map is up
            changed();
        }
    } else if (!strcmp(SM.up.type, "sound")) {
        const char *flag = !strcmp(SM.up.category, "swap") ? "customSwapSound" : !strcmp(SM.up.category, "inspect") ? "customInspectSound" : "customSound";
        put(cfg, flag, cJSON_CreateTrue());
        changed();
    }
    up_end(NULL);
}

// the brain's refusal of a chunk / done: its {error} text
static void body_error(const char *body, char *out, const size_t n) {
    cJSON *j = body ? cJSON_Parse(body) : NULL;
    const char *e = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j, "error"));
    if (!e) e = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j, "message"));
    snprintf(out, n, "%s", e ? e : "Upload failed");
    cJSON_Delete(j);
}

static void up_next(void);

static void on_up_chunk(void *user, const int status, const char *body, const size_t length) {
    (void) length;
    if ((unsigned) (uintptr_t) user != SM.up.gen || !SM.up.active) return;
    if (status != 200) {
        char err[200];
        body_error(body, err, sizeof(err));
        up_end(err);
        return;
    }
    if (SM.up.done_sent) { // {data: {asset_path}}
        cJSON *j = body ? cJSON_Parse(body) : NULL;
        const char *path = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(j, "data"), "asset_path"));
        char rel[160];
        snprintf(rel, sizeof(rel), "%s", path ? path : "");
        cJSON_Delete(j);
        if (!rel[0]) up_end("Upload failed");
        else up_stored(rel);
        return;
    }
    up_next();
}

static void up_next(void) {
    char url[1400];
    void *user = (void *) (uintptr_t) SM.up.gen;
    if (SM.up.sent >= SM.up.len) {
        snprintf(url, sizeof(url), "%s?fileId=%s&done=1", SM.up.base, SM.up.file_id);
        SM.up.done_sent = 1;
        if (!backend_http_post_bytes(url, "application/octet-stream", "", 0, CHUNK_TIMEOUT_MS, on_up_chunk, user)) up_end("Upload failed");
        return;
    }
    const size_t at = SM.up.sent, n = SM.up.len - at < CHUNK ? SM.up.len - at : CHUNK;
    snprintf(url, sizeof(url), "%s?fileId=%s&key=file&offset=%zu", SM.up.base, SM.up.file_id, at);
    SM.up.sent += n;
    if (!backend_http_post_bytes(url, "application/octet-stream", SM.up.data + at, n, CHUNK_TIMEOUT_MS, on_up_chunk, user)) up_end("Upload failed");
}

static void on_up_key(void *user, const int status, const cJSON *data, const cJSON *error) {
    if ((unsigned) (uintptr_t) user != SM.up.gen || !SM.up.active) return;
    const char *fid = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(data, "fileId"));
    const char *url = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(data, "uploadUrl"));
    if (status < 200 || status >= 300 || error || !fid || !url || strlen(fid) >= sizeof(SM.up.file_id)) {
        up_end(v10_gapi_error_text(error, "Upload failed"));
        return;
    }
    snprintf(SM.up.file_id, sizeof(SM.up.file_id), "%s", fid);
    if (url[0] == '/') snprintf(SM.up.base, sizeof(SM.up.base), "%s%s", backend_url(), url);
    else snprintf(SM.up.base, sizeof(SM.up.base), "%s", url);
    up_next();
}

// uploadAsset(skinId, file, type, category): the gapi key, the chunks, done (one upload at a time; data owned)
static void up_start(const char *slot, const char *type, const char *category, const char *file_name, unsigned char *data, const size_t len) {
    free(SM.up.data);
    const unsigned gen = SM.up.gen + 1;
    memset(&SM.up, 0, sizeof(SM.up));
    SM.up.gen = gen;
    SM.up.active = 1;
    SM.up.skin = editing_id();
    SM.up.data = data;
    SM.up.len = len;
    snprintf(SM.up.slot, sizeof(SM.up.slot), "%s", slot);
    snprintf(SM.up.type, sizeof(SM.up.type), "%s", type);
    snprintf(SM.up.category, sizeof(SM.up.category), "%s", category);
    snprintf(SM.up.file_name, sizeof(SM.up.file_name), "%s", file_name);
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "asset_type", type);
    if (category[0]) cJSON_AddStringToObject(body, "category", category);
    cJSON_AddStringToObject(body, "fileName", file_name);
    cJSON_AddNumberToObject(body, "size", (double) len);
    char path[64];
    snprintf(path, sizeof(path), "/skins/%lld/upload", (long long) SM.up.skin);
    const int sent = v10_gapi("POST", path, body, on_up_key, (void *) (uintptr_t) gen);
    cJSON_Delete(body);
    if (!sent) up_end("Failed to fetch");
}

// iÍïîîíî (:125342-125356): the picked model's extension as fileExt (hasAnim when the model was a GLTF), glow with
// the glow toggle and an emissive texture
static void apply_config(void) {
    if (!SM.editing) return;
    cJSON *cfg = editing_config();
    if (SM.model_ext[0]) {
        const int was_gltf = !strcmp(jstr(cfg, "fileExt"), "gltf");
        put(cfg, "fileExt", cJSON_CreateString(SM.model_ext));
        if (was_gltf) put(cfg, "hasAnim", cJSON_CreateTrue());
    }
    if (SM.has_glow && cfg_true(cJSON_GetObjectItemCaseSensitive(cfg, "customTextures"), "emissive")) put(cfg, "glow", cJSON_CreateTrue());
    changed();
}

static const DropKind *drop_kind(const char *type) {
    return !strcmp(type, "model")     ? &DROP_MODEL
           : !strcmp(type, "texture") ? &DROP_TEXTURE
           : !strcmp(type, "sound")   ? &DROP_SOUND
           : !strcmp(type, "wear")    ? &DROP_WEAR
                                      : NULL;
}

static void slot_name(const char *type, const char *category, char *out, const size_t n) {
    if (!strcmp(type, "model")) snprintf(out, n, "model");
    else snprintf(out, n, "%s:%s", type, category[0] ? category : !strcmp(type, "texture") ? "diffuse" : "shoot");
}

// a file for a section: the drop zone's check (iììiíïí's validateFile, :11960-11975), then the section's
// onFileSelected (the model's / a texture's / a sound's, :125357-125386, :125405-125455, :125476-125495)
static void file_chosen(const char *type, const char *category, const char *path) {
    const DropKind *k = drop_kind(type);
    if (!k || !SM.editing || !path || !path[0]) return;
    char slot[24], ext[16];
    slot_name(type, category, slot, sizeof(slot));
    const char *cat = strchr(slot, ':') ? strchr(slot, ':') + 1 : "";
    SlotState *st = slot_state(slot, 1);
    if (!st) return;
    size_t len = 0;
    unsigned char *data = read_whole(path, &len);
    const char *name = base_name(path);
    name_ext(name, ext, sizeof(ext));
    st->file_name[0] = st->drop_error[0] = 0;
    if (!data) snprintf(st->drop_error, sizeof(st->drop_error), "The file could not be read"); // KrunkNative: the path is the dialog's
    else if (k->max && (long long) len > k->max) {
        char max[24];
        size_text(max, sizeof(max), k->max);
        snprintf(st->drop_error, sizeof(st->drop_error), "File size exceeds %s limit", max);
    } else {
        char list[64];
        snprintf(list, sizeof(list), ",%s,", k->accept);
        char needle[24];
        snprintf(needle, sizeof(needle), ",%s,", ext);
        if (!strstr(list, needle)) {
            StrBuf sb = {0};
            sb_append(&sb, "File type not supported. Accepted types: ");
            for (const char *p = k->accept; *p; p++) sb_append(&sb, *p == ',' ? ", " : (char[2]) {*p, 0});
            char *t = sb_finish(&sb);
            snprintf(st->drop_error, sizeof(st->drop_error), "%s", t ? t : "");
            free(t);
        }
    }
    if (st->drop_error[0]) {
        free(data);
        return;
    }
    snprintf(st->file_name, sizeof(st->file_name), "%s", name);
    st->upload_error[0] = 0;
    if (!strcmp(type, "wear")) { // a wear layer: kept for Generate (iÎïïíii), nothing uploads yet
        int i = 0;
        while (i < 4 && strcmp(cat, WEAR_LAYERS[i].key)) i++;
        if (i == 4) {
            st->file_name[0] = 0;
            free(data);
            return;
        }
        free(SM.ed.layer[i]);
        SM.ed.layer[i] = data;
        SM.ed.layer_len[i] = len;
        SM.ed.wear_error[0] = 0;
        return;
    }
    const char *wait = "";
    if (!strcmp(type, "model")) {
        snprintf(SM.model_ext, sizeof(SM.model_ext), "%s", ext + 1);
        wait = "Please save the skin first before uploading model";
    } else if (!strcmp(type, "texture")) {
        if (!strcmp(cat, "emissive") && !SM.has_glow) SM.has_glow = 1; // the drop zone's file turns its setting on
        wait = "Please save the skin first before uploading textures";
    } else wait = "Please save the skin first before uploading sound";
    if (!editing_id()) {
        set_upload_error(slot, wait);
        free(data);
        apply_config();
        return;
    }
    if (SM.up.active) { // KrunkNative: one upload at a time (v10 runs each section's at once)
        set_upload_error(slot, "Another upload is still running");
        free(data);
        return;
    }
    up_start(slot, type, cat, name, data, len);
}

// the hidden <input type=file>'s dialog: the platform's (ed_pick_file); its answer goes to the section asked
static struct {
    char type[16], category[16];
    unsigned gen;
} g_pick;

static void on_picked(void *user, const char *path) {
    (void) user;
    if (g_pick.gen != SM.gen || !SM.depth) return;
    file_chosen(g_pick.type, g_pick.category, path);
    render();
}

// a section's Remove (and the drop zone's remove-file button): the model (onModelRemoved :125388), a texture
// (iIíiïïï :125456, iIiîïïi), a sound (iÎííiïí :125496)
static void unset_asset(const char *type, const char *category) {
    char slot[24];
    slot_name(type, category, slot, sizeof(slot));
    SlotState *st = slot_state(slot, 0);
    if (st) st->file_name[0] = st->drop_error[0] = st->upload_error[0] = 0;
    cJSON *cfg = editing_config();
    const char *cat = strchr(slot, ':') ? strchr(slot, ':') + 1 : "";
    if (!strcmp(type, "model")) {
        SM.model_ext[0] = 0;
        put(cfg, "customModel", cJSON_CreateFalse());
        apply_config();
    } else if (!strcmp(type, "texture")) {
        cJSON *tex = cJSON_GetObjectItemCaseSensitive(cfg, "customTextures");
        if (cJSON_IsObject(tex)) {
            cJSON_DeleteItemFromObjectCaseSensitive(tex, cat);
            if (!tex->child) cJSON_DeleteItemFromObjectCaseSensitive(cfg, "customTextures");
        }
        if (!strcmp(cat, "emissive")) {
            cJSON_DeleteItemFromObjectCaseSensitive(cfg, "glow");
            cJSON_DeleteItemFromObjectCaseSensitive(cfg, "pulsT");
        }
        apply_config();
    } else if (!strcmp(type, "sound")) {
        put(cfg, !strcmp(cat, "swap") ? "customSwapSound" : !strcmp(cat, "inspect") ? "customInspectSound" : "customSound", cJSON_CreateFalse());
        apply_config();
    } else if (!strcmp(type, "wear")) { // a layer's remove (the drop zone's onfileRemoved)
        for (int i = 0; i < 4; i++)
            if (!strcmp(cat, WEAR_LAYERS[i].key)) {
                free(SM.ed.layer[i]);
                SM.ed.layer[i] = NULL;
                SM.ed.layer_len[i] = 0;
            }
    }
}

// ------------------------------------------------------------------ Shader Studio's edits

static cJSON *part_at(const int i) { return cJSON_GetArrayItem(SM.sh.parts, i); }

static cJSON *part_params(cJSON *part) {
    cJSON *p = cJSON_GetObjectItemCaseSensitive(part, "params");
    if (!cJSON_IsObject(p)) {
        put(part, "params", cJSON_CreateObject());
        p = cJSON_GetObjectItemCaseSensitive(part, "params");
    }
    return p;
}

static const cJSON *param_named(const cJSON *params, const char *name) {
    const cJSON *p;
    cJSON_ArrayForEach(p, params) if (!strcmp(jstr(p, "name"), name)) return p;
    return NULL;
}

// a value editor's text as the param's type ("#rrggbb", a number, "x, y, z")
static cJSON *param_parse(const char *type, const char *text) {
    if (!strcmp(type, "color")) {
        cJSON *v = cJSON_CreateString(text);
        cJSON *d = param_default("color", v);
        cJSON_Delete(v);
        return d;
    }
    cJSON *a = cJSON_CreateArray();
    const char *p = text;
    while (*p) {
        char *end = NULL;
        const double d = strtod(p, &end);
        if (end == p) {
            p++;
            continue;
        }
        cJSON_AddItemToArray(a, cJSON_CreateNumber(d));
        p = end;
    }
    cJSON *out = param_default(type, cJSON_GetArraySize(a) == 1 && !strcmp(type, "float") ? a->child : a);
    cJSON_Delete(a);
    return out;
}

// onChange (:127180-127200): itemConfig.customShader {type: "graph", graph, applyToAll, parts: {nodeName: {params}}}
static void shader_write(void) {
    cJSON *cs = cJSON_CreateObject();
    cJSON_AddStringToObject(cs, "type", "graph");
    cJSON_AddItemToObject(cs, "graph", cJSON_Duplicate(SM.sh.graph, 1));
    cJSON_AddBoolToObject(cs, "applyToAll", SM.sh.apply_all);
    if (!SM.sh.apply_all) {
        cJSON *parts = cJSON_AddObjectToObject(cs, "parts");
        const cJSON *part;
        cJSON_ArrayForEach(part, SM.sh.parts) {
            char name[64];
            trimmed(name, sizeof(name), jstr(part, "nodeName"));
            if (!name[0] || cJSON_GetObjectItemCaseSensitive(parts, name)) continue;
            cJSON *e = cJSON_AddObjectToObject(parts, name);
            const cJSON *params = cJSON_GetObjectItemCaseSensitive(part, "params");
            cJSON_AddItemToObject(e, "params", cJSON_IsObject(params) ? cJSON_Duplicate(params, 1) : cJSON_CreateObject());
        }
    }
    put(editing_config(), "customShader", cs);
    changed();
}

static void shader_op(const char *op, const HtmlArg *args, const int argc);

// ------------------------------------------------------------------ Capture Thumbnail (:124566-124570, :124764-124790)

static void capture(void) {
    if (!editing_id()) {
        snprintf(SM.error, sizeof(SM.error), "Please save the skin first before capturing a thumbnail");
        return;
    }
    if (SM.capturing || SM.up.active) return;
    SM.capturing = 1;
    SM.captured = 0;
    unsigned char *rgba = viewer_embed_capture(CAPTURE_SIZE);
    size_t n = 0;
    unsigned char *png = rgba ? png_encode_rgba(rgba, CAPTURE_SIZE, CAPTURE_SIZE, &n) : NULL;
    free(rgba);
    if (!png) { // the iframe's capture never answers (no showcase shown): Capturing... stays in v10; here it ends
        SM.capturing = 0;
        snprintf(SM.error, sizeof(SM.error), "Failed to upload thumbnail: %s", "the preview is not shown");
        return;
    }
    up_start("thumbnail", "thumbnail", "", "thumbnail.png", png, n);
}

// ------------------------------------------------------------------ Test (testSkinSandbox, :204469-204610)

// the class whose primary is weapon index w (setClass's search), or the first with a secondary; 0 when none
static int test_class(const int secondary, const int w) {
    const ClassConfig *classes = rules_classes();
    const size_t count = rules_class_count();
    for (size_t i = 0; classes && i < count; i++) {
        if (secondary ? classes[i].secondary : classes[i].loadout && classes[i].loadout[0] == w) return (int) i;
    }
    return 0;
}

static void test_apply(Client *client, Player *me) {
    const int type = (int) jnum(g_test.skin, "type");
    const int id = (int) g_test.id;
    client_drop_player_mesh(client, me);
    const char *slot = "-";
    switch (type) {
    case 0: {
        const int weapon = (int) jnum(g_test.skin, "weapon");
        if (weapon < 1) break;
        const Weapon *w = rules_weapon(weapon - 1);
        const int secondary = w && w->secondary;
        const int cls = test_class(secondary, weapon - 1);
        if (me->class_index != cls || (secondary && me->secondary_index != weapon - 1))
            player_switch_class(me, cls, secondary ? weapon - 1 : me->secondary_index);
        if (secondary) me->skins.secondary = id, slot = "secondary";
        else me->skins.primary = id, slot = "primary";
        break;
    }
    case 1: me->cosmetics.hat = id, slot = "hat"; break;
    case 15: me->cosmetics_v7.head = id, slot = "head"; break;
    case 7: me->cosmetics.face = id, slot = "face"; break;
    case 2: me->cosmetics.body = id, slot = "body"; break;
    case 14: me->cosmetics_v7.back = id, slot = "back"; break;
    case 6: me->cosmetics.waist = id, slot = "waist"; break;
    case 8: me->cosmetics_v7.shoe = id, slot = "shoe"; break;
    case 11: me->cosmetics_v7.wrist = id, slot = "wrist"; break;
    case 5: me->cosmetics.dye = id, slot = "dye"; break;
    case 3: me->skins.melee = id, slot = "melee"; break;
    case 9: me->cosmetics_v7.pet = id, slot = "pet"; break;
    case 12:
        if (me->cosmetics_v7.charms[1] != -1) me->cosmetics_v7.charms[1] = id;
        me->cosmetics_v7.charms[0] = id, slot = "charm";
        break;
    default: break;
    }
    client_add_player_mesh(client, me, me->render_you); // players.regenMeshes
    fprintf(stderr, "[skins] test %d type=%d slot=%s class=%d\n", id, type, slot, me->class_index);
}

// each frame: 50 ms after Test, setSkin; then the sandbox player's slot once it is in the game (KrunkNative waits
// up to TEST_WAIT_MS for it: v10's tmpPlayer is there at once)
static void test_tick(void) {
    if (!g_test.skin || now_ms() < g_test.due) return;
    if (!g_test.class_asked) { // skinManager.setSkin(id, skin)
        g_test.class_asked = 1;
        viewer_skin_define((int) g_test.id, g_test.skin);
        g_test.until = now_ms() + TEST_WAIT_MS;
    }
    MenuUi *ui = menu_ui();
    Client *client = ui ? ui->client : NULL;
    Player *me = client ? client->me : NULL;
    if (me && me->active && me->mesh) test_apply(client, me);
    else if (now_ms() < g_test.until) return;
    else fprintf(stderr, "[skins] test %lld: no player\n", (long long) g_test.id);
    cJSON_Delete(g_test.skin);
    memset(&g_test, 0, sizeof(g_test));
}

// the preview JSON 300 ms after a change: a new iframe src (viewer_embed follows it), or the pane's first render
static void preview_tick(void) {
    if (SM.preview_due <= 0 || now_ms() < SM.preview_due) return;
    SM.preview_due = 0;
    if (!SM.editing) return;
    char *q = preview_query();
    if (!q) return;
    const int first = !SM.preview;
    free(SM.preview);
    SM.preview = q;
    const HtmlElem frame = first ? 0 : html_dom_by_id("knSmPreview");
    const int type = (int) jnum(SM.editing, "type");
    if (frame && type != T_PLAYER_CARD && type != T_EMOJI) {
        char *src = preview_src();
        if (src) html_dom_set_attr(frame, "src", src);
        free(src);
    } else render();
}

// an inline handler's argument: a literal, or this.value / this.checked (events.c fills .str / .num)
static int arg_is_str(const HtmlArg *args, const int argc, const int i) {
    return i < argc && (args[i].kind == HTML_ARG_STR || args[i].kind == HTML_ARG_THIS_VALUE);
}

static int64_t arg_i64(const HtmlArg *args, const int argc, const int i) {
    if (i >= argc) return 0;
    if (args[i].kind == HTML_ARG_NUM || args[i].kind == HTML_ARG_BOOL || args[i].kind == HTML_ARG_THIS_CHECKED) return (int64_t) args[i].num;
    if (arg_is_str(args, argc, i) && args[i].str) return strtoll(args[i].str, NULL, 10);
    return 0;
}

static const char *arg_s(const HtmlArg *args, const int argc, const int i) {
    return arg_is_str(args, argc, i) && args[i].str ? args[i].str : "";
}

static void shader_op(const char *op, const HtmlArg *args, const int argc) {
    shader_load();
    const int i = (int) arg_i64(args, argc, 2);
    if (!strcmp(op, "toggle")) SM.sh.open = !SM.sh.open;
    else if (!strcmp(op, "applyAll")) SM.sh.apply_all = arg_i64(args, argc, 2) != 0;
    else if (!strcmp(op, "addPart")) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "nodeName", "");
        cJSON_AddObjectToObject(e, "params");
        cJSON_AddItemToArray(SM.sh.parts, e);
    } else if (!strcmp(op, "removePart")) {
        if (i >= 0 && i < cJSON_GetArraySize(SM.sh.parts)) cJSON_DeleteItemFromArray(SM.sh.parts, i);
    } else if (!strcmp(op, "partName")) {
        cJSON *part = part_at(i);
        if (part) put(part, "nodeName", cJSON_CreateString(arg_s(args, argc, 3)));
        return; // the field keeps its caret: no new markup
    } else if (!strcmp(op, "copyDefaults") || !strcmp(op, "override") || !strcmp(op, "param")) {
        cJSON *part = part_at(i);
        if (!part) return;
        cJSON *params = graph_params();
        cJSON *pv = part_params(part);
        if (!strcmp(op, "copyDefaults")) {
            const cJSON *p;
            cJSON_ArrayForEach(p, params) put(pv, jstr(p, "name"), cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(p, "default"), 1));
        } else {
            const char *name = arg_s(args, argc, 3);
            const cJSON *p = param_named(params, name);
            if (p && !strcmp(op, "override"))
                put(pv, name, arg_i64(args, argc, 4) ? cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(p, "default"), 1) : NULL);
            else if (p) put(pv, name, param_parse(jstr(p, "type"), arg_s(args, argc, 4)));
        }
        cJSON_Delete(params);
    } else if (!strcmp(op, "refresh")) { // Refresh Preview: the skin's customShader, the preview with it
        shader_write();
        SM.sh.preview = 1;
    } else if (!strcmp(op, "clearPreview")) {
        SM.sh.preview = 0;
        SM.preview_due = now_ms() + PREVIEW_DEBOUNCE_MS;
    } else if (!strcmp(op, "remove")) { // Remove Shader
        cJSON_DeleteItemFromObjectCaseSensitive(editing_config(), "customShader");
        cJSON_Delete(SM.sh.graph);
        cJSON_Delete(SM.sh.parts);
        SM.sh.graph = default_graph();
        SM.sh.parts = cJSON_CreateArray();
        SM.sh.apply_all = 1;
        SM.sh.legacy = 0;
        SM.sh.preview = 0;
        changed();
    }
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
    if (!strcmp(op, "sg")) { // Shader Studio's node canvas: its own partial updates
        if (!SM.editing) return 1;
        shader_load();
        return sg_editor_handle(canvas(), ev, args + 1, argc - 1);
    }
    if (!strcmp(op, "pg")) { // the Paint section's node canvas (kind "paint")
        if (!SM.editing) return 1;
        editors_load();
        return sg_editor_handle(paint_canvas(), ev, args + 1, argc - 1);
    }
    // any other action (Save, Remove Shader, a tab) sees the canvas's last edit (its 100 ms onChange now)
    if (SM.sh.canvas) sg_editor_flush(SM.sh.canvas);
    if (SM.ed.paint_canvas) sg_editor_flush(SM.ed.paint_canvas);
    if (SM.editing) {
        editors_load();
        const int r = editors_op(op, args, argc);
        if (r == 2) return 1;
        if (r == 1) {
            render();
            return 1;
        }
    }
    if (!strcmp(op, "close")) { // iïìiïîï: the view stack's pop
        if (v10_view_depth() == SM.depth) v10_view_pop(0);
        return 1;
    }
    if (!strcmp(op, "set")) {
        char value[32];
        const int64_t n = arg_i64(args, argc, 2);
        if (arg_is_str(args, argc, 2)) snprintf(value, sizeof(value), "%s", arg_s(args, argc, 2));
        else snprintf(value, sizeof(value), "%lld", (long long) n);
        on_set(arg_s(args, argc, 1), value);
    } else if (!strcmp(op, "tab")) { // iÏîïîïí
        const char *t = arg_s(args, argc, 1);
        SM.tab = !strcmp(t, "appearance") ? TAB_APPEARANCE : !strcmp(t, "advanced") ? TAB_ADVANCED : TAB_GENERAL;
    } else if (!strcmp(op, "back")) { // iìiiíîi
        cJSON_Delete(SM.editing);
        SM.editing = NULL;
        free(SM.preview);
        SM.preview = NULL;
        SM.preview_due = 0;
        SM.saved = 1;
        SM.error[0] = 0;
    } else if (!strcmp(op, "save")) {
        if (SM.saved) return 1;
        do_save();
    } else if (!strcmp(op, "capture")) { // iììííií: the showcase's 256 px capture as the thumbnail
        if (SM.busy) return 1;
        capture();
    } else if (!strcmp(op, "pick") || !strcmp(op, "file")) { // a drop zone's click / a file for it (scripted: its path)
        if (!SM.editing || SM.up.active) return 1;
        const char *type = arg_s(args, argc, 1);
        const DropKind *k = drop_kind(type);
        if (!k) return 1;
        if (!strcmp(op, "file")) file_chosen(type, arg_s(args, argc, 2), arg_s(args, argc, 3));
        else {
            snprintf(g_pick.type, sizeof(g_pick.type), "%s", type);
            snprintf(g_pick.category, sizeof(g_pick.category), "%s", arg_s(args, argc, 2));
            g_pick.gen = SM.gen;
            ed_pick_file(k->accept, on_picked, NULL);
            return 1; // on_picked renders (at once natively, later on the web)
        }
    } else if (!strcmp(op, "unset")) {
        if (!SM.editing) return 1;
        unset_asset(arg_s(args, argc, 1), arg_s(args, argc, 2));
    } else if (!strcmp(op, "glow")) { // textureSettings.hasGlow: off removes the emissive texture
        if (!SM.editing) return 1;
        SM.has_glow = arg_i64(args, argc, 1) != 0;
        if (!SM.has_glow) unset_asset("texture", "emissive");
    } else if (!strcmp(op, "trans") || !strcmp(op, "loop")) { // Transparency / Loop while inspecting
        if (!SM.editing) return 1;
        put(editing_config(), !strcmp(op, "trans") ? "trans" : "inspSLoop", arg_i64(args, argc, 1) ? cJSON_CreateTrue() : NULL);
        changed();
    } else if (!strcmp(op, "pulse")) { // Pulse: pulsT 0.0015 / 0.003, Off deletes it
        if (!SM.editing) return 1;
        const char *v = arg_s(args, argc, 1);
        put(editing_config(), "pulsT", !strcmp(v, "slow") ? cJSON_CreateNumber(0.0015) : !strcmp(v, "fast") ? cJSON_CreateNumber(0.003) : NULL);
        changed();
    } else if (!strcmp(op, "dye") || !strcmp(op, "undye")) { // a dye colour as parseInt(hex, 16); its remove button
        if (!SM.editing) return 1;
        const char *key = arg_s(args, argc, 1);
        int known = 0;
        for (int i = 0; i < 7; i++) known |= !strcmp(key, DYE_KEYS[i]);
        if (!known) return 1;
        cJSON *cfg = editing_config();
        const int had = cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(cfg, key));
        if (!strcmp(op, "dye")) {
            const char *hex = arg_s(args, argc, 2);
            if (hex[0] == '#') hex++;
            char *end = NULL;
            const long v = strtol(hex, &end, 16);
            if (end == hex) return 1;
            put(cfg, key, cJSON_CreateNumber((double) v));
        } else put(cfg, key, NULL);
        changed();
        if (had && !strcmp(op, "dye")) return 1; // the picker stays as it is: no new markup
    } else if (!strcmp(op, "opac")) { // Opacity: 1 deletes opac
        if (!SM.editing) return 1;
        const double v = atof(arg_s(args, argc, 1));
        put(editing_config(), "opac", v >= 1.0 ? NULL : cJSON_CreateNumber(v < 0 ? 0 : v));
        changed();
        const HtmlElem out = html_dom_by_id("knSmOpacValue");
        if (out) {
            char t[16];
            snprintf(t, sizeof(t), "%.2f", v < 0 ? 0 : v > 1 ? 1 : v);
            html_dom_set_html(out, t);
        }
        return 1; // the slider keeps its drag
    } else if (!strcmp(op, "shader")) {
        if (!SM.editing) return 1;
        const char *sub = arg_s(args, argc, 1);
        shader_op(sub, args, argc);
        if (!strcmp(sub, "partName")) return 1;
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
        const cJSON *row = row_by_id(a1);
        if (row) { // the row's game skin, with its shader as customShaderInfo (:204476-204508), 50 ms on
            cJSON_Delete(g_test.skin);
            memset(&g_test, 0, sizeof(g_test));
            g_test.id = a1;
            g_test.skin = game_skin(row);
            cJSON *info = shader_info(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(row, "itemConfig"), "customShader"));
            if (info) put(g_test.skin, "customShaderInfo", info);
            g_test.due = now_ms() + TEST_DELAY_MS;
        }
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
    test_tick();
    if (SM.depth) preview_tick();
    if (SM.depth && SM.editing && SM.ed.paint_canvas) sg_editor_tick(SM.ed.paint_canvas);
    if (SM.depth && SM.editing && SM.sh.canvas) {
        sg_editor_tick(SM.sh.canvas);
        if (SM.sh.render_due) {
            SM.sh.render_due = 0;
            render();
        }
    }
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
