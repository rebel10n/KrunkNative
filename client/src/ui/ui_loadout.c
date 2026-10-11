// The Change Loadout window and everything it opens, the K.P.D popup and the
// Changelog: the C port of Krunker v3.1.1's guest-state menu logic for them
// (src/index.js in the v3.1.1 tree):
//   - selected* globals (selectedSkinColor, selectedAttachIndex, ...) with their
//     localStorage keys and `getSavedVal(key) || default` defaults
//   - windows[2].gen/genLoadouts/action        (window 3, loadout presets)
//   - the item pickers 9, 15, 16, 17, 20, 26, 31, 32, 34, 38, 39 (getItemsList and
//     friends) and their select*() handlers, which save and go back to window 3
//   - selectSkinColor / selectStat (updateWindow(3)), skinSelector / selectSkin
//   - shoPolicePop / hidePolicePop / resetPops / clearPops (K.P.D, "Officer Buddy")
//   - windows[34] Changelog (docs/versions.txt) and its toggleLog
// A guest owns no store items, so every picker shows only Krunker's None/Random
// cards, exactly like the real page does for a logged-out player.
#include <ui_logic.h>
#include <audio.h>
#include <backend_ui.h>
#include <online_mode.h> // offline mode: every skin unlocked
#include <render_settings.h>
#include "../viewer/viewer_store.h"
#include <sprays.h> // SPRAYS_BASE_COUNT
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------- Krunker tables

// Weapons: g_weapons (name, preview src, secondary) is Krunker's weaponConfig in id order.
// The attachment slot is UI-only data: weaponConfig `attach != null` / `forceAttach`.
typedef struct {
    const char *name;
    const char *src;   // textures/previews/weapons/<src>.png; NULL = undefined (the knife)
    int attach, force_attach, secondary;
} LoadoutWeapon;

static const unsigned char WEAPON_ATTACH[WEAPON_COUNT] = {[1] = 1, [3] = 1, [6] = 2, [7] = 1, [13] = 1, [14] = 1, [18] = 1}; // 2 = forced

static const LoadoutWeapon *weapon_at(const int id) {
    static LoadoutWeapon w;
    if (id < 0 || id >= WEAPON_COUNT || !g_weapons[id]) return NULL;
    w.name = g_weapons[id]->name ? g_weapons[id]->name : "";
    w.src = g_weapons[id]->src;
    w.secondary = g_weapons[id]->secondary;
    w.attach = WEAPON_ATTACH[id] != 0;
    w.force_attach = WEAPON_ATTACH[id] == 2;
    return &w;
}

// The class's primary weapon id and `secondary` flag (Game.classes, like the class window).
typedef struct {
    int primary;
    int secondary;
} LoadoutClass;

static LoadoutClass class_at(const int index) {
    const Client *client = menu_ui()->client;
    const size_t count = client->game.class_count ? client->game.class_count : sizeof(g_classes) / sizeof(g_classes[0]);
    const ClassConfig *classes = client->game.classes ? client->game.classes : g_classes;
    const ClassConfig *c = index >= 0 && (size_t) index < count ? &classes[index] : &classes[0];
    LoadoutClass out = {c->loadout ? c->loadout[0] : 0, c->secondary};
    return out;
}

// data/classes.js `txts` (selectClass speech lines) by class index; ClassConfig.texts has no count.
static const char *CLASS_TXTS[][5] = {
    [3] = {"Calling in the Big Guns?", "Remember - No Russian.", "Pesky Snipers..."}, // Spray N Pray
    [4] = {"..."},                                                                      // Vince
    [5] = {"I'm onto something"},                                                       // Detective
    [7] = {"..."},                                                                      // Rocketeer
    [9] = {"You sure about this?", "...", "Oh boy", "I don't know about this...", "Not me again..."}, // Runner
    [13] = {NULL},
};
#define CLASS_TXTS_COUNT ((int) (sizeof(CLASS_TXTS) / sizeof(CLASS_TXTS[0])))

// config.skinColors (col as hex, Krunker prints col.toString(16)).
static const char *SKIN_COLORS[] = {"805c4a", "a77860", "d38d6f", "cc997e", "edbfa6"};
#define SKIN_COLOR_COUNT 5

static const char *RETICLES[] = {"Red Dot", "Green Dot", "Pyramid", "Tau", "Ray", "Spread",
                                 "Marine", "Underline", "Precision", "Quantum", "Triad", "Nuclear"};
#define RETICLE_COUNT ((int) (sizeof(RETICLES) / sizeof(RETICLES[0])))

// config.killCardStats names, in order.
static const char *KC_STATS[] = {"SPK", "Kills", "Deaths", "KDR", "KPG", "Games Played", "Wins", "W/L",
                                 "Nukes", "Melees", "Accuracy", "KR", "Challenge Level", "Headshots",
                                 "Solo Ranked", "Duo Ranked", "Quad Ranked", "Losses", "Wallbangs",
                                 "Beatdowns", "Bullseyes"};
#define KC_STAT_COUNT ((int) (sizeof(KC_STATS) / sizeof(KC_STATS[0])))


// ---------------------------------------------------------------- selected* state

// `var selectedX = getSavedVal(key) || default`: one saved integer per key.
typedef struct {
    const char *key;
    int def;
    int value;
} Selected;

enum {
    SEL_SKIN_COL, SEL_HAIR_COL, SEL_CHAT_COL, SEL_DYE, SEL_FACE, SEL_ATTACH, SEL_RETICLE, SEL_MELEE,
    SEL_BACK, SEL_HAT, SEL_WAIST, SEL_SECONDARY, SEL_KC_STAT, SEL_SPRAY, SEL_COUNT
};

static Selected g_sel[SEL_COUNT] = {
    [SEL_SKIN_COL] = {"skinColIndex", -1, -1},  [SEL_HAIR_COL] = {"hairCol", -1, -1},
    [SEL_CHAT_COL] = {"chatCol", -1, -1},       [SEL_DYE] = {"dyeIndex", -1, -1},
    [SEL_FACE] = {"faceIndex", -1, -1},         [SEL_ATTACH] = {"attachIndex", 0, 0},
    [SEL_RETICLE] = {"reticleIndex", 0, 0},     [SEL_MELEE] = {"meleeIndex", -1, -1},
    [SEL_BACK] = {"backIndex", -1, -1},         [SEL_HAT] = {"hatIndex", -1, -1},
    [SEL_WAIST] = {"waistIndex", -1, -1},       [SEL_SECONDARY] = {"secondaryInd", 2, 2},
    [SEL_KC_STAT] = {"kcStatIndex", -1, -1},    [SEL_SPRAY] = {"sprayindex", 0, 0},
};
static int g_sel_loaded;
static int g_skin_weapon = 1; // dC: the weapon the Select Skin window is for

static void sel_load(void) {
    if (g_sel_loaded) return;
    g_sel_loaded = 1;
    for (int i = 0; i < SEL_COUNT; i++) {
        char buf[32];
        g_sel[i].value = g_sel[i].def;
        if (html_storage_get(g_sel[i].key, buf, sizeof(buf)) < 0 || !buf[0]) continue;
        char *end;
        const long v = strtol(buf, &end, 10);
        if (end != buf) g_sel[i].value = (int) v;
    }
}

static int sel(const int which) {
    sel_load();
    return g_sel[which].value;
}

static void sel_save(const int which, const int value) {
    sel_load();
    g_sel[which].value = value;
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", value);
    html_storage_set(g_sel[which].key, buf);
}

int ui_loadout_skin_color(void) {
    const int i = sel(SEL_SKIN_COL);
    return i >= 0 && i < SKIN_COLOR_COUNT ? (int) strtol(SKIN_COLORS[i], NULL, 16) : -1;
}

// The look sent with the spawn request (ENT): skin colour and the cosmetic picks. "Random" (-2) resolves to
// one of the owned items, and a guest owns none, so it is none (-1) like the preview's `dw == -2 ? -1 : dw`.
// hairCol stays the class colour (-1): Krunker only takes a hex string there, which the picker never sets.
void ui_loadout_cosmetics(PlayerCosmetics *out) {
    const int pick[5] = {sel(SEL_HAT), sel(SEL_BACK), sel(SEL_WAIST), sel(SEL_FACE), sel(SEL_DYE)};
    int32_t *slot[5] = {&out->hat, &out->back, &out->waist, &out->face, &out->dye};
    out->skin_color = ui_loadout_skin_color();
    out->hair_color = -1;
    for (int i = 0; i < 5; i++) *slot[i] = pick[i] >= 0 ? pick[i] : -1;
}

static int random_owned_skin(int weapon, int type);
static int weapon_skin(int weapon);

// enterGame's ent[3]/[4]/[14]/[17]/[21]: the same picks, but "Random" (-2) is getRandomOwnedSkin(null, type)
// for every ENT (none for a guest, who owns nothing).
static void ent_cosmetics(PlayerCosmetics *out) {
    ui_loadout_cosmetics(out);
    static const struct { int which, type; } SLOTS[5] = {
        {SEL_HAT, 1}, {SEL_BACK, 2}, {SEL_WAIST, 6}, {SEL_FACE, 7}, {SEL_DYE, 5},
    };
    int32_t *slot[5] = {&out->hat, &out->back, &out->waist, &out->face, &out->dye};
    for (int i = 0; i < 5; i++) {
        if (sel(SLOTS[i].which) == -2) *slot[i] = random_owned_skin(0, SLOTS[i].type);
    }
}

// selectedSkinsByWeapon[weapon] for a loadout slot: -1 none; Random (-2) resolves to an owned skin of
// that weapon (getRandomOwnedSkin(weapon + 1)) for the ENT, and to none for the preview (dJ's hq).
static int32_t slot_weapon_skin(const int weapon, const int resolve_random) {
    if (weapon < 0) return -1;
    const int pick = weapon_skin(weapon);
    if (pick == -2) return resolve_random ? random_owned_skin(weapon + 1, -1) : -1;
    return pick >= 0 ? pick : -1;
}

static void loadout_skins(const int class_index, PlayerSkins *out, const int resolve_random) {
    const int secondary = ui_loadout_secondary(class_index);
    out->primary = slot_weapon_skin(class_at(class_index).primary, resolve_random);
    out->secondary = secondary >= 0 ? slot_weapon_skin(secondary, resolve_random) : -1;
    const int melee = sel(SEL_MELEE);
    out->melee = melee == -2 ? (resolve_random ? random_owned_skin(0, 3) : -1) : melee >= 0 ? melee : -1;
}

void ui_loadout_ent_look(const int class_index, PlayerCosmetics *cosmetics, PlayerSkins *skins) {
    ent_cosmetics(cosmetics);
    loadout_skins(class_index, skins, 1);
}

void ui_loadout_preview_skins(const int class_index, PlayerSkins *out) {
    loadout_skins(class_index, out, 0);
}

// The secondary the player picked (selectedSecondaryIndex, localStorage "secondaryInd",
// default 2 = Pistol) as a Krunker weapon id; -1 when the class has no secondary slot.
int ui_loadout_secondary(const int class_index) {
    if (!class_at(class_index).secondary) return -1;
    const LoadoutWeapon *w = weapon_at(sel(SEL_SECONDARY));
    return w && w->secondary ? sel(SEL_SECONDARY) : 2;
}

// ---------------------------------------------------------------- dJ()

static int g_police_class = -1;   // dI.class while the K.P.D popup shows Officer Buddy
static int g_police_saved = -1;   // the player's own class, restored by hidePolicePop

// dJ(): the class box (name, weapon line, name tag) and the preview. KrunkNative's
// preview follows Client.menu_class_index, re-read every frame.
static void refresh_class_box(void) {
    menu_update_class_box();
    if (g_police_class < 0) return;
    // dI.name: "Officer Buddy <i class='material-icons menuClassPlayerPremium'>beenhere</i>"
    HtmlElem tag = html_dom_by_id("menuClassNameTag");
    if (tag) html_dom_set_html(tag, "Officer Buddy <i class='material-icons menuClassPlayerPremium'>beenhere</i>");
}

// ---------------------------------------------------------------- window 3

static const char *spray_name(int index);

// ---------------------------------------------------------------- Game.store.skins / acc.skins

static const ViewerSkin *store_skin(const int index) { return index >= 0 ? viewer_skin(index) : NULL; }

static const char *rarity_color(const ViewerSkin *s) {
    return s && s->rarity >= 0 && s->rarity < g_viewer_rarity_count ? g_viewer_rarities[s->rarity].color : "#fff";
}

// aY.rarityAnim && store.rarities[r].animate: only Unobtainable (6) animates in v3.1.1
static int rarity_anim(const ViewerSkin *s) {
    return s && s->rarity == 6 && !client_render_settings()->disable_rarity_anim;
}

// skin.creator || "Krunker.io": the viewer table has no creator, so read assets/skins.json once.
static const char *skin_creator(const int index) {
    static cJSON *skins;
    static int loaded;
    if (!loaded) {
        loaded = 1;
        char *path = concat(assets_path(), "skins.json");
        unsigned char *buf = NULL;
        size_t len = 0;
        if (path && ensure_asset_file(path) && !read_file(path, &buf, &len)) skins = cJSON_ParseWithLength((const char *) buf, len);
        free(buf);
        free(path);
    }
    const char *c = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(skins, index), "creator"));
    return c && c[0] ? c : "Krunker.io";
}

typedef struct { int ind, cnt, pos; } OwnedSkin;
static int g_owned_by_rarity_only; // the spray window sorts by rarity alone (stable)

static int owned_rarity(const OwnedSkin *o) { const ViewerSkin *s = store_skin(o->ind); return s ? s->rarity : 0; }
static int owned_season(const OwnedSkin *o) { const ViewerSkin *s = store_skin(o->ind); return s ? (s->seas ? s->seas : 1) : 0; }

// [...acc.skins].sort(): rarity, then season, then index, all descending.
static int owned_cmp(const void *pa, const void *pb) {
    const OwnedSkin *a = pa, *b = pb;
    const int ra = owned_rarity(a), rb = owned_rarity(b);
    if (ra != rb) return rb - ra;
    if (!g_owned_by_rarity_only) {
        const int sa = owned_season(a), sb = owned_season(b);
        if (sa != sb) return sb - sa;
        if (a->ind != b->ind) return b->ind - a->ind;
    }
    return a->pos - b->pos;
}

// Offline mode (online_mode.h): no account, every store skin is unlocked and nothing is saved.
static int all_skins_unlocked(void) { return !online_mode_online(); }

// The loadout may show and wear store skins: signed in (acc.skins) or offline mode (all of them).
static int has_inventory(void) { return account_get() || all_skins_unlocked(); }

// acc.skins ({ind, cnt} entries, account data slot 10), sorted; NULL (count 0) for a guest.
// Offline mode: one of every skin. Caller frees.
static OwnedSkin *owned_skins(int *count, const int by_rarity_only) {
    *count = 0;
    if (all_skins_unlocked() && g_viewer_skin_count > 0) {
        OwnedSkin *all = calloc((size_t) g_viewer_skin_count, sizeof(OwnedSkin));
        if (!all) return NULL;
        for (int i = 0; i < g_viewer_skin_count; i++) {
            const ViewerSkin *s = viewer_skin(i);
            if (!s || !s->name) continue;
            all[*count] = (OwnedSkin) {i, 1, *count};
            (*count)++;
        }
        g_owned_by_rarity_only = by_rarity_only;
        qsort(all, (size_t) *count, sizeof(OwnedSkin), owned_cmp);
        return all;
    }
    const AccountInfo *acc = account_get();
    const cJSON *skins = acc ? cJSON_GetArrayItem(acc->data, 10) : NULL;
    const int n = cJSON_GetArraySize(skins);
    OwnedSkin *out = n > 0 ? calloc((size_t) n, sizeof(OwnedSkin)) : NULL;
    if (!out) return NULL;
    const cJSON *e;
    cJSON_ArrayForEach(e, skins) {
        const cJSON *ind = cJSON_GetObjectItem(e, "ind"), *cnt = cJSON_GetObjectItem(e, "cnt");
        if (!cJSON_IsNumber(ind)) continue;
        out[*count] = (OwnedSkin) {ind->valueint, cJSON_IsNumber(cnt) ? cnt->valueint : 1, *count};
        (*count)++;
    }
    g_owned_by_rarity_only = by_rarity_only;
    qsort(out, (size_t) *count, sizeof(OwnedSkin), owned_cmp);
    return out;
}

// ent[1]: selectedSprayIndex; Random (-2) is UTILS.randInt(0, Game.sprays.length - 1) per ENT.
// Game.sprays are everyone's; a type-4 store spray only while it is owned (offline: all are),
// anything else is the default spray 0 (addSpray's Game.sprays[0] fallback).
int ui_loadout_spray(void) {
    const int pick = sel(SEL_SPRAY);
    if (pick == -2) return rand() % SPRAYS_BASE_COUNT;
    if (pick >= 0 && pick < SPRAYS_BASE_COUNT) return pick;
    const ViewerSkin *skin = store_skin(pick);
    if (!skin || skin->type != 4) return 0;
    if (all_skins_unlocked()) return pick;
    int n = 0, owned = 0;
    OwnedSkin *skins = owned_skins(&n, 0);
    for (int i = 0; i < n && !owned; i++) owned = skins[i].ind == pick;
    free(skins);
    return owned ? pick : 0;
}

// getRandomOwnedSkin(weapon, type): a random owned weapon skin of `weapon` (1-based, type < 0) or item
// of store type `type`; -1 when there is none (a guest owns nothing, offline mode owns everything).
static int random_owned_skin(const int weapon, const int type) {
    int n = 0, matches = 0, pick = -1;
    OwnedSkin *owned = owned_skins(&n, 0);
    for (int i = 0; i < n; i++) {
        const ViewerSkin *s = store_skin(owned[i].ind);
        if (!s || (type < 0 ? s->type != 0 || s->weapon != weapon : s->type != type)) continue;
        if (rand() % ++matches == 0) pick = owned[i].ind; // reservoir sample: uniform over the matches
    }
    free(owned);
    return pick;
}

// One owned item card of getItemsList / Select Skin (weapon: the selectSkin weapon, -1 = fn(ind)).
static void owned_card(StrBuf *out, const OwnedSkin *o, const ViewerSkin *s, const char *fn, const char *img_class, const int with_season, const int weapon) {
    const char *col = rarity_color(s);
    char img[160];
    store_skin_preview(o->ind, img, sizeof(img));
    if (img[0] == '.' && img[1] == '/') memmove(img, img + 2, strlen(img + 2) + 1);
    sb_appendf(out, "<div class=\"skinCard%s%s\" style=\"color:%s;border:5px solid%s\" onmouseenter=\"playTick()\" onclick=\"%s(%d",
               s->rarity == 5 ? "" : " blackShad", rarity_anim(s) ? " rainbow" : "", col, col, fn, o->ind);
    if (weapon >= 0) sb_appendf(out, ",%d", weapon);
    sb_appendf(out, ")\"><div class=\"itemCnt\" data-badge=\"x%d\"></div>", o->cnt);
    sb_append_esc(out, s->name ? s->name : "");
    sb_append(out, "<div class=\"itemOwn\">by ");
    sb_append_esc(out, skin_creator(o->ind));
    sb_append(out, "</div>");
    if (with_season) sb_appendf(out, "<div class=\"itemSea\">Season %d</div>", s->seas ? s->seas : 1);
    sb_appendf(out, "<img loading=\"lazy\" %sclass=\"%s%s\" src=\"%s\"/><div class=\"selctInfoBtn\" onmouseenter=\"playTick()\" onclick=\"openSales(%d)\">?</div></div>",
               weapon >= 0 ? "draggable=\"false\" " : "", img_class, (s->flags & VS_RGB) ? " rgbHue" : "", img, o->ind);
}

// localStorage "skins" parsed once (the class preview asks every frame); writers go through skins_store().
static cJSON *g_skins_cache;
static int g_skins_cached;

static void skins_store(const char *text) {
    html_storage_set("skins", text);
    cJSON_Delete(g_skins_cache);
    g_skins_cache = NULL;
    g_skins_cached = 0;
}

// dz[weapon]: the saved weapon skin (localStorage "skins" {weapon: index}), -1 none.
static int weapon_skin(const int weapon) {
    if (!g_skins_cached) {
        char buf[1024] = "{}";
        html_storage_get("skins", buf, sizeof(buf));
        g_skins_cache = cJSON_Parse(buf);
        g_skins_cached = 1;
    }
    char key[16];
    snprintf(key, sizeof(key), "%d", weapon);
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(g_skins_cache, key);
    return cJSON_IsNumber(v) ? v->valueint : -1;
}

// The loadout row's name: store.skins[i].name, else Random / the default.
static const char *cosmetic_name(const int index, const char *none) {
    const ViewerSkin *s = store_skin(index);
    if (s && s->name) return s->name;
    return index == -2 ? "Random" : none;
}

// The loadout row's colour: the rarity colour of a selected item when signed in.
static const char *cosmetic_color(const int index, const char *grey) {
    const ViewerSkin *s = has_inventory() ? store_skin(index) : NULL;
    return s ? rarity_color(s) : grey;
}

// genLoadouts(): the preset row (#loadoutHolder content).
static void gen_loadouts(StrBuf *out) {
    sb_append(out, "<span class=\"settNameSmall\" style=\"margin:0;margin-bottom:5px;\"><input id=\"loadoutName\" type=\"text\" class=\"formInput\" placeholder=\"Loadout name\" autocomplete=\"off\" autocorrect=\"off\" autocapitalize=\"off\" spellcheck=\"false\" style=\"border:none;background:#eee;padding-bottom:6px;float:left;margin-top:5px;margin-bottom:5px;\"><select id=\"loadoutSelect\" style=\"border-radius:6px;margin-left:10px;border:none;background:#eee;;padding:6px;padding-bottom:6px;margin-top:5px;margin-bottom:5px;\"><option selected=\"\">Select</option>");
    char buf[4096];
    if (html_storage_get("krk_loadouts", buf, sizeof(buf)) >= 0) {
        cJSON *all = cJSON_Parse(buf);
        const cJSON *it;
        cJSON_ArrayForEach(it, all) {
            if (!it->string) continue;
            sb_append(out, "<option>");
            sb_append_esc(out, it->string);
            sb_append(out, "</option>");
        }
        cJSON_Delete(all);
    }
    sb_append(out, "</select><div class=\"loadoutBtn\" onclick=\"windows[2].action(2)\">Delete</div><div class=\"loadoutBtn\" onclick=\"windows[2].action(1)\">Save</div><div class=\"loadoutBtn\" onclick=\"windows[2].action(0)\">Load</div></span>");
}

static void settname_row(StrBuf *out, const char *label, const char *onclick, const char *color, const char *value) {
    sb_appendf(out, "<div class=\"settName\">%s<span onclick=\"%s\" class=\"settText floatR\" style=\"color:%s\">", label, onclick, color);
    sb_append_esc(out, value);
    sb_append(out, "</span></div>");
}

// A cosmetic row: name and rarity colour (rainbowText for an animated rarity) of the selected item.
static void item_row(StrBuf *out, const char *label, const char *onclick, const int index, const char *none, const char *grey) {
    const ViewerSkin *s = has_inventory() ? store_skin(index) : NULL;
    sb_appendf(out, "<div class=\"settName\">%s<span onclick=\"%s\" class=\"settText floatR%s\" style=\"color:%s\">", label, onclick,
               rarity_anim(s) ? " rainbowText" : "", cosmetic_color(index, grey));
    sb_append_esc(out, cosmetic_name(index, none));
    sb_append(out, "</span></div>");
}

static void color_row_premium(StrBuf *out, const char *label) {
    sb_appendf(out, "<div class=\"settName\">%s<i class=\"material-icons premiumSkinCol\" onclick=\"windows[13].premium()\">add_circle_outline</i>", label);
}

char *ui_loadout_gen(void) {
    const MenuUi *ui = menu_ui();
    const int class_index = ui->client->menu_class_index;
    const MenuClass *mc = menu_class_at(class_index);
    const LoadoutClass cls = class_at(class_index);
    const LoadoutClass *lc = &cls;
    const LoadoutWeapon *primary = weapon_at(lc->primary);
    const char *primary_name = primary ? primary->name : "";
    const int has_attach = primary && primary->attach && !primary->force_attach; // hC
    const int has_reticle = primary && primary->attach;                         // hE
    const LoadoutWeapon *sw = weapon_at(sel(SEL_SECONDARY));
    const char *secondary_name = sw ? sw->name : "";
    const char *grey = "rgba(0,0,0,0.3)";

    StrBuf out = {0};
    sb_append(&out, "<div id=\"loadoutHolder\" style=\"text-align:left;margin-bottom:10px\">");
    gen_loadouts(&out);
    sb_append(&out, "</div>");

    sb_append(&out, "<div class=\"settName\">Class<span class=\"settText floatR\" onclick=\"showWindow(6)\">");
    sb_append_esc(&out, mc && mc->name ? mc->name : "");
    // signed in, dz[weapon] names the weapon skin (rarity colour) or "Random"
    const int pskin = has_inventory() ? weapon_skin(lc->primary) : -1;
    const ViewerSkin *ps = store_skin(pskin);
    sb_appendf(&out, "</span></div><div class=\"settName\">Primary<span class=\"settLabel wepLink%s\" style=\"color:%s\" onclick=\"skinSelector(%d)\">",
               rarity_anim(ps) ? " rainbowText" : "", ps ? rarity_color(ps) : "#2196F3", lc->primary);
    sb_append_esc(&out, ps && ps->name ? ps->name : pskin == -2 ? "Random" : primary_name);
    sb_append(&out, "</span></div>");
    if (has_attach) {
        const int a = sel(SEL_ATTACH);
        sb_appendf(&out, "<div class=\"settName\">Attachment<span class=\"settLabel wepLink\" onclick=\"showWindow(31)\" style=\"color:#2196F3\">%s</span></div>",
                   a == 0 ? "Reflex Sight" : "None");
    }
    if (has_reticle) {
        const int r = sel(SEL_RETICLE);
        const char *name = r >= 0 && r < RETICLE_COUNT ? RETICLES[r] : r == -2 ? "Custom" : "None";
        sb_appendf(&out, "<div class=\"settName\">Reticle<span class=\"settLabel wepLink\" onclick=\"showWindow(32)\" style=\"color:%s\">%s</span></div>", grey, name);
    }
    const int sskin = has_inventory() && lc->secondary ? weapon_skin(sel(SEL_SECONDARY)) : -1;
    const ViewerSkin *ss = store_skin(sskin);
    sb_appendf(&out, "<div class=\"settName\">Secondary<span class=\"settLabel%s%s\" %s style=\"color:%s\">",
               lc->secondary ? " wepLink" : "", lc->secondary && rarity_anim(ss) ? " rainbowText" : "", lc->secondary ? "onclick=\"showWindow(20)\"" : "",
               ss ? rarity_color(ss) : lc->secondary ? "#2196F3" : grey);
    sb_append_esc(&out, !lc->secondary ? "None" : ss && ss->name ? ss->name : sskin == -2 ? "Random" : secondary_name);
    sb_append(&out, "</span></div>");
    item_row(&out, "Melee", "showWindow(26)", sel(SEL_MELEE), "Combat Knife", grey);
    item_row(&out, "Face", "showWindow(39)", sel(SEL_FACE), "None", grey);
    item_row(&out, "Hat", "showWindow(16)", sel(SEL_HAT), "None", grey);
    item_row(&out, "Body", "showWindow(17)", sel(SEL_BACK), "None", grey);
    item_row(&out, "Waist", "showWindow(38)", sel(SEL_WAIST), "None", grey);
    item_row(&out, "Dye", "showWindow(34)", sel(SEL_DYE), "None", grey);
    const int spray = sel(SEL_SPRAY);
    const ViewerSkin *spray_skin = spray_name(spray) ? NULL : store_skin(spray);
    if (spray_skin && spray_skin->type == 4) item_row(&out, "Spray", "showWindow(9)", spray, "None", "#2196F3");
    else settname_row(&out, "Spray", "showWindow(9)", "#2196F3", spray_name(spray) ? spray_name(spray) : spray == -2 ? "Random" : "None");

    // Skin Color: the premium "+" then config.skinColors from the last to the first.
    color_row_premium(&out, "Skin Color");
    const int skin = sel(SEL_SKIN_COL);
    for (int i = SKIN_COLOR_COUNT - 1; i >= 0; i--) {
        sb_appendf(&out, "<div class=\"skinColorItem%s\" style=\"background-color:#%s\" onmouseenter=\"playTick()\" onclick=\"selectSkinColor(%d)\"></div>",
                   skin == i ? " activeSkin" : "", SKIN_COLORS[i], i);
    }
    sb_append(&out, "</div>");
    color_row_premium(&out, "Hair Color");
    sb_append(&out, "</div>");
    color_row_premium(&out, "Chat Name Color");
    sb_append(&out, "</div>");

    const int stat = sel(SEL_KC_STAT);
    sb_appendf(&out, "<div class=\"settName\">Killcard Stat<select onchange=\"selectStat(this.value)\" class=\"inputGrey2\" style=\"font-size:16px;padding:4px;margin-bottom: 5px;\"><option value=\"-1\" %s>None</option>",
               stat == -1 ? "selected=\"\"" : "");
    for (int i = 0; i < KC_STAT_COUNT; i++) {
        sb_appendf(&out, "<option value=\"%d\" %s>%s</option>", i, stat == i ? "selected=\"\"" : "", KC_STATS[i]);
    }
    sb_append(&out, "</select></div>");
    return sb_finish(&out);
}

// ---------------------------------------------------------------- pickers

static void skin_card_none_random(StrBuf *out, const char *fn, const char *img_class, const char *img, const int with_season,
                                  const char *none_label, const char *rand_extra) {
    sb_appendf(out, "<div class=\"skinCard blackShad\" style=\"border:5px solid lightgrey\" onmouseenter=\"playTick()\" onclick=\"%s(-1)\">%s<div class=\"itemOwn\">Default</div>%s<img class=\"%s\" src=\"%s\"></div>",
               fn, none_label, with_season ? "<div class=\"itemSea\" style=\"opacity:0;\">Season 1</div>" : "", img_class, img);
    sb_appendf(out, "<div class=\"skinCard blackShad\" style=\"border:5px solid lightgrey\" onmouseenter=\"playTick()\" onclick=\"%s(-2)\">Random<div class=\"itemOwn\" style=\"opacity:0;\">by \?\?\?</div>%s<img class=\"%s skinSil\" src=\"%s\"><div class=\"skinRandom blackShad\"%s>?</div></div>",
               fn, with_season ? "<div class=\"itemSea\" style=\"opacity:0;\">All Seasons</div>" : "", img_class, img, rand_extra);
}

// getItemsList(funcName, type) for a guest: the None and Random cards.
static char *gen_items_list(const char *fn, const int type) {
    const char *img_class = type ? (type == 5 ? "skinImgD" : "skinImgC") : "skinImg";
    char img[64];
    snprintf(img, sizeof(img), "textures/previews/cosmetics/%d_default.png", type);
    StrBuf out = {0};
    sb_append(&out, "<div id=\"skinList\">");
    skin_card_none_random(&out, fn, img_class, img, type != 5, type == 3 ? "Combat Knife" : "None", "");
    int n;
    OwnedSkin *owned = owned_skins(&n, 0); // signed in: acc.skins of this type
    for (int i = 0; i < n; i++) {
        const ViewerSkin *s = store_skin(owned[i].ind);
        if (s && s->type == type) owned_card(&out, &owned[i], s, fn, img_class, type != 5, -1);
    }
    free(owned);
    sb_append(&out, "</div>");
    return sb_finish(&out);
}

// windows[14] Select Skin for weapon dC.
static char *gen_skins(void) {
    const LoadoutWeapon *w = weapon_at(g_skin_weapon);
    char img[64];
    snprintf(img, sizeof(img), "textures/previews/weapons/%s.png", w && w->src ? w->src : "undefined");
    StrBuf out = {0};
    sb_append(&out, "<div id=\"skinList\" style=\"margin-top: 8px;\">");
    sb_appendf(&out, "<div class=\"skinCard blackShad\" style=\"border:5px solid lightgrey\" onmouseenter=\"playTick()\" onclick=\"selectSkin(-1,%d)\">None<div class=\"itemOwn\">Default</div><div class=\"itemSea\" style=\"opacity:0;\">Season 1</div><img draggable=\"false\" class=\"skinImg\" src=\"%s\"></div>", g_skin_weapon, img);
    sb_appendf(&out, "<div class=\"skinCard blackShad\" style=\"border:5px solid lightgrey\" onmouseenter=\"playTick()\" onclick=\"selectSkin(-2,%d)\">Random<div class=\"itemOwn\" style=\"opacity:0;\">by \?\?\?</div><div class=\"itemSea\" style=\"opacity:0;\">All Seasons</div><img draggable=\"false\" class=\"skinImg skinSil\" onmouseenter=\"playTick()\" src=\"%s\"><div class=\"skinRandom blackShad\" style=\"top:68px\">?</div></div>", g_skin_weapon, img);
    int n;
    OwnedSkin *owned = owned_skins(&n, 0); // signed in: owned skins of weapon dC (b7.weapon == dC + 1)
    for (int i = 0; i < n; i++) {
        const ViewerSkin *s = store_skin(owned[i].ind);
        if (s && !s->type && s->weapon == g_skin_weapon + 1) owned_card(&out, &owned[i], s, "selectSkin", "skinImg", 1, s->weapon - 1);
    }
    free(owned);
    sb_append(&out, "</div>");
    return sb_finish(&out);
}

// windows[19] Select Secondary: every `secondary` weapon (none has minRec in v3.1.1).
static char *gen_secondary(void) {
    StrBuf out = {0};
    sb_append(&out, "<div id=\"skinList\">");
    for (int i = 0; i < WEAPON_COUNT; i++) {
        const LoadoutWeapon *w = weapon_at(i);
        if (!w || !w->secondary) continue;
        sb_append(&out, "<div class=\"skinCard blackShad\" style=\"height: 165px;border:5px solid lightgrey\">");
        sb_append_esc(&out, w->name);
        sb_appendf(&out, "<div class=\"itemOwn\">Default</div><img loading=\"lazy\" draggable=\"false\" class=\"secImg\" onmouseenter=\"playTick()\" src=\"textures/previews/weapons/%s.png\" onclick=\"skinSelector(%d, true)\"></div>",
                   w->src ? w->src : "undefined", i);
    }
    sb_append(&out, "</div>");
    return sb_finish(&out);
}

static int class_primary_has_attach(void) {
    const LoadoutWeapon *w = weapon_at(class_at(menu_ui()->client->menu_class_index).primary);
    return w && w->attach;
}

// windows[30] Select Attachment.
static char *gen_attach(void) {
    StrBuf out = {0};
    sb_append(&out, "<div id=\"skinList\"><div class=\"noBtnCard\" style=\"height:150px;border:5px solid lightgrey\" onclick=\"selectAttachment(-1)\">None<div class=\"skinRandom blackShad\" style=\"top: 60px;\">X</div></div>");
    if (class_primary_has_attach()) {
        sb_append(&out, "<div class=\"noBtnCard\" style=\"height:150px;border:5px solid lightgrey\" onclick=\"selectAttachment(0)\">Reflex Sight<div></div><img loading=\"lazy\" draggable=\"false\" class=\"noBtnImgC\" src=\"textures/previews/attach/attach_0.png\"></div>");
    }
    sb_append(&out, "</div>");
    return sb_finish(&out);
}

// windows[31] Select Reticle (no saved custom reticle image).
static char *gen_reticle(void) {
    StrBuf out = {0};
    sb_append(&out, "<div id=\"skinList\" style=\"margin-top:8px\"><div class=\"noBtnCard\" style=\"height:140px;border:5px solid lightgrey\" onclick=\"selectReticle(-1)\" onmouseenter=\"playTick()\">None<div class=\"skinRandom blackShad\" style=\"top: 60px;\" onmouseenter=\"playTick()\">X</div></div>");
    sb_append(&out, "<div class=\"noBtnCard\" style=\"height:140px;border:5px solid lightgrey\">Custom<div></div><img class=\"noBtnImgR skinSil\" style=\"margin-top:5px;\" draggable=\"false\" src=\"textures/reticles/reticle_0.png\" onclick=\"selectReticle(-2)\" onmouseenter=\"playTick()\"><input type=\"url\" class=\"customReticle\" name=\"url\" id=\"slid_customADSDot\" placeholder=\"Reticle Image\" value=\"\" oninput=\"setSetting(&quot;customADSDot&quot;, this.value);updateWindow(null, true);\"></div>");
    if (class_primary_has_attach()) {
        for (int i = 0; i < RETICLE_COUNT; i++) {
            sb_appendf(&out, "<div class=\"noBtnCard\" style=\"height:140px;border:5px solid lightgrey\" onclick=\"selectReticle(%d)\">%s<div class=\"itemOwn\" style=\"margin-top:1px\">Default</div><img loading=\"lazy\" class=\"noBtnImgR\" draggable=\"false\" src=\"textures/reticles/reticle_%d.png\" onmouseenter=\"playTick()\"></div>",
                       i, RETICLES[i], i);
        }
    }
    sb_append(&out, "</div>");
    return sb_finish(&out);
}

// Spray names come from the captured Select Spray window (ui/windows/9.html):
// `onclick="selectSpray(N)" ...>Name<div`.
static const char *spray_name(const int index) {
    static char name[64];
    if (index < 0) return NULL;
    char needle[40];
    snprintf(needle, sizeof(needle), "onclick=\"selectSpray(%d)\"", index);
    const char *hit = strstr(ui_windows_html("9"), needle);
    if (!hit) return NULL;
    const char *gt = strchr(hit, '>');
    if (!gt) return NULL;
    const char *end = strchr(gt + 1, '<');
    if (!end || end - gt - 1 >= (long) sizeof(name)) return NULL;
    memcpy(name, gt + 1, (size_t) (end - gt - 1));
    name[end - gt - 1] = 0;
    return name;
}

// windows[8] Select Spray signed in: the owned type-4 skins (sorted by rarity) go between the
// Random card and the default sprays of the captured window; NULL = keep the capture (guest).
static char *gen_sprays(void) {
    int n;
    OwnedSkin *owned = owned_skins(&n, 1);
    const char *base = ui_windows_html("9");
    const char *at = strstr(base, "<div class=\"noBtnCard\" onclick=\"selectSpray(0)\"");
    StrBuf cards = {0}, style = {0};
    int frames_seen[16], frames_n = 0;
    for (int i = 0; at && i < n; i++) {
        const ViewerSkin *s = store_skin(owned[i].ind);
        if (!s || s->type != 4) continue;
        const char *col = rarity_color(s);
        char img[160], anim[96] = "";
        store_skin_preview(owned[i].ind, img, sizeof(img));
        if (img[0] == '.' && img[1] == '/') memmove(img, img + 2, strlen(img + 2) + 1);
        if (s->frames) { // UTILS.getFrameAnim(b7, "sprayAnm") and its @keyframes
            snprintf(anim, sizeof(anim), "animation: sprayAnm%d %gs steps(%d) infinite;", s->frames, 1 + s->frame_t / 1000.0, s->frames);
            int seen = 0;
            for (int k = 0; k < frames_n; k++) seen |= frames_seen[k] == s->frames;
            if (!seen && frames_n < 16) {
                char kf[48];
                snprintf(kf, sizeof(kf), "@keyframes sprayAnm%d ", s->frames);
                if (!strstr(base, kf)) sb_appendf(&style, "%s{100%%{background-position:-%dpx}}", kf, s->frames * 110);
                frames_seen[frames_n++] = s->frames;
            }
        }
        sb_appendf(&cards, "<div class=\"noBtnCard%s\" style=\"color:%s;border: 5px solid %s\" onclick=\"selectSpray(%d)\">", rarity_anim(s) ? " rainbow" : "", col, col, owned[i].ind);
        sb_append_esc(&cards, s->name ? s->name : "");
        sb_appendf(&cards, "<div class=\"itemOwn\">Spray</div><div draggable=\"false\" class=\"sprayImg\" onmouseenter=\"playTick()\" style=\"background: url(&quot;%s&quot;) -1px;%s\"></div></div>", img, anim);
    }
    free(owned);
    char *c = sb_finish(&cards), *st = sb_finish(&style);
    if (!c || !c[0]) {
        free(c);
        free(st);
        return NULL;
    }
    StrBuf out = {0};
    if (st && st[0]) sb_appendf(&out, "<style>%s</style>", st);
    sb_append_n(&out, base, (size_t) (at - base));
    sb_append(&out, c);
    sb_append(&out, at);
    free(c);
    free(st);
    return sb_finish(&out);
}

// ---------------------------------------------------------------- windows[34] Changelog

// Krunker splits docs/versions.txt with
//   /^\s*== (?=.*?\bUPDATE|UPADTE|INITIAL|TRICKSHOT\b)(.*) ==/gm
// into [prefix, title, body, title, body, ...] and renders every pair but the last.
static int is_word_char(const char c) { return isalnum((unsigned char) c) || c == '_'; }

static int changelog_title(const char *line, const size_t len, const char **title, size_t *title_len) {
    size_t i = 0;
    while (i < len && isspace((unsigned char) line[i])) i++;
    if (len - i < 3 || strncmp(line + i, "== ", 3)) return 0;
    const char *t = line + i + 3;
    const size_t tl = len - i - 3;
    int ok = 0;
    for (size_t k = 0; k + 6 <= tl && !ok; k++) { // .*?\bUPDATE
        if (!strncmp(t + k, "UPDATE", 6) && (k == 0 || !is_word_char(t[k - 1]))) ok = 1;
    }
    if (!ok && tl >= 6 && !strncmp(t, "UPADTE", 6)) ok = 1;
    if (!ok && tl >= 7 && !strncmp(t, "INITIAL", 7)) ok = 1;
    if (!ok && tl >= 9 && !strncmp(t, "TRICKSHOT", 9) && (tl == 9 || !is_word_char(t[9]))) ok = 1;
    if (!ok) return 0;
    // (.*) == : greedy up to the last " ==" on the line
    for (size_t k = tl; k >= 3; k--) {
        if (!strncmp(t + k - 3, " ==", 3)) {
            *title = t;
            *title_len = k - 3;
            return 1;
        }
    }
    return 0;
}

static void changelog_body(StrBuf *out, const char *body, const size_t len) {
    // fixChanges: lines trimmed, empty ones dropped, joined by '</div><div class="changeText">'
    int first = 1;
    const char *p = body, *end = body + len;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t) (end - p));
        const char *le = nl ? nl : end;
        const char *a = p, *b = le;
        while (a < b && isspace((unsigned char) *a)) a++;
        while (b > a && isspace((unsigned char) b[-1])) b--;
        if (b > a) {
            if (!first) sb_append(out, "</div><div class=\"changeText\">");
            sb_append_n(out, a, (size_t) (b - a));
            first = 0;
        }
        p = nl ? nl + 1 : end;
    }
}

static char *gen_changelog(void) {
    // public/docs/versions.txt, shipped next to the window templates so the web build's
    // preloaded /ui/windows package carries it too.
#ifdef __EMSCRIPTEN__
    char *path = concat("/ui/windows/", "versions.txt");
#else
    char *path = concat(assets_path(), "../ui/windows/versions.txt");
#endif
    unsigned char *data = NULL;
    size_t length = 0;
    const int ok = read_file(path, &data, &length) == 0;
    free(path);

    StrBuf out = {0};
    sb_append(&out, "<div>Changelog</div><div id=\"mailList\">");
    if (!ok) {
        sb_append(&out, "Loading...</div>");
        return sb_finish(&out);
    }
    // collect the header matches: (title, title_len, line start, line end)
    typedef struct { const char *title; size_t title_len; const char *start, *end; } Hdr;
    Hdr *hdrs = NULL;
    size_t count = 0, cap = 0;
    const char *text = (const char *) data, *tend = text + length;
    for (const char *p = text; p < tend;) {
        const char *nl = memchr(p, '\n', (size_t) (tend - p));
        const char *le = nl ? nl : tend;
        Hdr h;
        if (changelog_title(p, (size_t) (le - p), &h.title, &h.title_len)) {
            if (count == cap) {
                cap = cap ? cap * 2 : 64;
                Hdr *grown = realloc(hdrs, cap * sizeof(Hdr));
                if (!grown) break;
                hdrs = grown;
            }
            h.start = p;
            h.end = h.title + h.title_len + 3;
            hdrs[count++] = h;
        }
        p = nl ? nl + 1 : tend;
    }
    // `for (hk = 1; hk < logData.length - 2; hk += 2)`: the last pair is skipped
    for (size_t i = 0; i + 1 < count; i++) {
        const char *body = hdrs[i].end;
        // the next match's `^\s*` swallows the blank lines before it
        const char *body_end = hdrs[i + 1].start;
        sb_appendf(&out, "<div class=\"mailObj\" onclick=\"windows[34].toggleLog(this)\"><i class=\"material-icons\" style=\"font-size:34px;float:right;margin-right:5px;\">%s</i><div class=\"mailFrom\">",
                   i ? "arrow_drop_down" : "arrow_drop_up");
        sb_append_n(&out, hdrs[i].title, hdrs[i].title_len);
        sb_appendf(&out, "</div><div %s><div class=\"changeText\">", i ? "style=\"display:none\"" : "");
        changelog_body(&out, body, (size_t) (body_end - body));
        sb_append(&out, "</div></div></div>");
    }
    free(hdrs);
    free(data);
    sb_append(&out, "</div>");
    return sb_finish(&out);
}

void ui_loadout_toggle_log(const HtmlElem obj) {
    if (!obj) return;
    HtmlElem icon = html_dom_first_child(obj);
    HtmlElem from = icon ? html_dom_next(icon) : 0;
    HtmlElem body = from ? html_dom_next(from) : 0;
    if (!icon || !body) return;
    char disp[16] = "";
    const int hidden = html_dom_get_style(body, "display", disp, sizeof(disp)) >= 0 && !strcmp(disp, "none");
    html_dom_set_display(body, hidden ? "block" : "none");
    html_dom_set_text(icon, hidden ? "arrow_drop_up" : "arrow_drop_down");
}

// ---------------------------------------------------------------- window table hook

char *ui_loadout_generate(const int id, const char **header) {
    const char *h = NULL;
    char *html = NULL;
    switch (id) {
        case 3: h = "Change Loadout"; html = ui_loadout_gen(); break;
        case 9: h = "Select Spray"; html = gen_sprays(); break; // NULL = the captured window
        case 15: h = "Select Skin"; html = gen_skins(); break;
        case 16: h = "Select Hat"; html = gen_items_list("selectHat", 1); break;
        case 17: h = "Select Back"; html = gen_items_list("selectBack", 2); break;
        case 20: h = "Select Secondary"; html = gen_secondary(); break;
        case 26: h = "Select Melee"; html = gen_items_list("selectMelee", 3); break;
        case 31: h = "Select Attachment"; html = gen_attach(); break;
        case 32: h = "Select Reticle"; html = gen_reticle(); break;
        case 34: h = "Select Dye"; html = gen_items_list("selectDye", 5); break;
        case 35: h = "Changelog"; html = gen_changelog(); break;
        case 38: h = "Select Waist"; html = gen_items_list("selectWaist", 6); break;
        case 39: h = "Select Face"; html = gen_items_list("selectFace", 7); break;
        default: return NULL;
    }
    if (header) *header = h;
    return html;
}

// ---------------------------------------------------------------- handlers

// selectHat & co.: saveVal, dJ(), showWindow(3)
static void select_and_back(const int which, const int value) {
    sel_save(which, value);
    refresh_class_box();
    menu_show_window(3, 0);
}

void ui_loadout_select(const char *what, const int value) {
    static const struct { const char *name; int which; } MAP[] = {
        {"selectHat", SEL_HAT}, {"selectBack", SEL_BACK}, {"selectWaist", SEL_WAIST},
        {"selectMelee", SEL_MELEE}, {"selectDye", SEL_DYE}, {"selectFace", SEL_FACE},
        {"selectAttachment", SEL_ATTACH}, {"selectReticle", SEL_RETICLE}, {"selectSpray", SEL_SPRAY},
    };
    for (size_t i = 0; i < sizeof(MAP) / sizeof(MAP[0]); i++) {
        if (!strcmp(MAP[i].name, what)) {
            select_and_back(MAP[i].which, value);
            return;
        }
    }
}

// selectSkinColor(i) / selectStat(v): saveVal, dJ(), updateWindow(3).
void ui_loadout_select_skin_color(const int index) {
    sel_save(SEL_SKIN_COL, index);
    refresh_class_box();
    menu_rerender_if_open(3);
}

void ui_loadout_select_stat(const int index) {
    sel_save(SEL_KC_STAT, index);
    refresh_class_box();
    menu_rerender_if_open(3);
}

// skinSelector(weapon, isSecondary): remember dC (and the secondary), open Select Skin.
void ui_loadout_skin_selector(const int weapon, const int secondary) {
    g_skin_weapon = weapon;
    if (secondary) sel_save(SEL_SECONDARY, weapon);
    refresh_class_box();
    menu_show_window(15, 0);
}

// selectSkin(index, weapon): a store skin index or -2 (Random) is remembered, anything else clears it.
void ui_loadout_select_skin(const int index, const int weapon) {
    char buf[1024] = "{}";
    html_storage_get("skins", buf, sizeof(buf));
    cJSON *skins = cJSON_Parse(buf);
    if (!skins || !cJSON_IsObject(skins)) {
        cJSON_Delete(skins);
        skins = cJSON_CreateObject();
    }
    char key[16];
    snprintf(key, sizeof(key), "%d", weapon);
    cJSON_DeleteItemFromObject(skins, key);
    // dz[weapon] = store.skins[index] || index == -2 ? index : undefined
    if (index == -2 || store_skin(index)) cJSON_AddNumberToObject(skins, key, index);
    char *text = cJSON_PrintUnformatted(skins);
    if (text) skins_store(text);
    free(text);
    cJSON_Delete(skins);
    refresh_class_box();
    menu_show_window(3, 0);
}

// windows[2].action(n): 0 load, 1 save, 2 delete the preset named in #loadoutSelect /
// #loadoutName (localStorage krk_loadouts), then genLoadouts().
void ui_loadout_action(const int action) {
    char selected[64] = "Select", name[64] = "";
    HtmlElem sel_el = html_dom_by_id("loadoutSelect"), name_el = html_dom_by_id("loadoutName");
    if (sel_el) html_dom_get_value(sel_el, selected, sizeof(selected));
    if (name_el) html_dom_get_value(name_el, name, sizeof(name));
    const int nothing = !selected[0] || !strcmp(selected, "Select");
    if ((action == 0 || action == 2) && nothing) return;

    char buf[4096] = "{}";
    html_storage_get("krk_loadouts", buf, sizeof(buf));
    cJSON *all = cJSON_Parse(buf);
    if (!all || !cJSON_IsObject(all)) {
        cJSON_Delete(all);
        all = cJSON_CreateObject();
    }
    const char *target = name[0] ? name : nothing ? "Preset1" : selected;

    static const struct { const char *field; int which; } FIELDS[] = {
        {"skinColIndex", SEL_SKIN_COL}, {"hairCol", SEL_HAIR_COL}, {"chatCol", SEL_CHAT_COL},
        {"dyeIndex", SEL_DYE}, {"faceIndex", SEL_FACE}, {"attachIndex", SEL_ATTACH},
        {"reticleIndex", SEL_RETICLE}, {"meleeIndex", SEL_MELEE}, {"backIndex", SEL_BACK},
        {"hatIndex", SEL_HAT}, {"waistIndex", SEL_WAIST}, {"secondaryInd", SEL_SECONDARY},
        {"kcStatIndex", SEL_KC_STAT},
    };
    if (action == 2) {
        cJSON_DeleteItemFromObject(all, selected);
    } else if (action == 1) {
        cJSON *preset = cJSON_CreateObject();
        for (size_t i = 0; i < sizeof(FIELDS) / sizeof(FIELDS[0]); i++) cJSON_AddNumberToObject(preset, FIELDS[i].field, sel(FIELDS[i].which));
        cJSON_AddNumberToObject(preset, "classindex", menu_ui()->client->menu_class_index);
        char skins[1024] = "{}";
        html_storage_get("skins", skins, sizeof(skins));
        cJSON_AddStringToObject(preset, "skins", skins);
        cJSON_DeleteItemFromObject(all, target);
        cJSON_AddItemToObject(all, target, preset);
    } else {
        const cJSON *preset = cJSON_GetObjectItem(all, selected);
        if (cJSON_IsObject(preset)) {
            for (size_t i = 0; i < sizeof(FIELDS) / sizeof(FIELDS[0]); i++) {
                const cJSON *v = cJSON_GetObjectItem(preset, FIELDS[i].field);
                if (cJSON_IsNumber(v)) sel_save(FIELDS[i].which, v->valueint);
            }
            const cJSON *ci = cJSON_GetObjectItem(preset, "classindex");
            if (cJSON_IsNumber(ci)) menu_set_class_index(ci->valueint, 0);
            const cJSON *skins = cJSON_GetObjectItem(preset, "skins");
            if (cJSON_IsString(skins)) skins_store(cJSON_GetStringValue(skins));
            refresh_class_box();
            menu_rerender_if_open(3);
        }
    }
    if (action != 0) {
        char *text = cJSON_PrintUnformatted(all);
        if (text) html_storage_set("krk_loadouts", text);
        free(text);
    }
    cJSON_Delete(all);

    HtmlElem holder = html_dom_by_id("loadoutHolder");
    if (holder) {
        StrBuf sb = {0};
        gen_loadouts(&sb);
        char *html = sb_finish(&sb);
        html_dom_set_html(holder, html);
        free(html);
    }
}

// selectClass(i): the class's own line (txts) or, one time in three, a generic one.
void ui_loadout_class_bubble(const int index) {
    const char *const *txts = index >= 0 && index < CLASS_TXTS_COUNT ? CLASS_TXTS[index] : NULL;
    int n = 0;
    while (txts && n < 5 && txts[n]) n++;
    if (n) {
        menu_show_speech_bubble(txts[rand() % n], 0.0);
    } else if (rand() % 3 == 0) {
        static const char *lines[] = {"Let's Go!", "Alright!", "I'm Ready!"};
        menu_show_speech_bubble(lines[rand() % 3], 0.0);
    }
}

// ---------------------------------------------------------------- K.P.D popup

#define POLICE_CLASS 5            // dI.class: Detective
#define POLICE_BUBBLE_MS 3400.0

static const char *POLICE_IDLE_LINES[] = {
    "I'm here to spin KR and bust hackers...and I'm all out of KR",
    "A fine day for Hacker Hunting",
    "What's a cops favourite food? I DONUT know...",
    "A really good Detective never gets married",
    "I love this song...",
};
static int g_police_timer;

static int popup_visible(const char *id) {
    char d[16] = "";
    HtmlElem el = html_dom_by_id(id);
    return el && html_dom_get_style(el, "display", d, sizeof(d)) >= 0 && !strcmp(d, "block");
}

static void set_disp(const char *id, const char *value) {
    HtmlElem el = html_dom_by_id(id);
    if (el) html_dom_set_display(el, value);
}

static int rand_between(const int lo, const int hi) { return lo + rand() % (hi - lo + 1); }

// showSpeechBubble(html, ms, true): the officer sighs.
static void police_say(const char *line) {
    audio_play(rand() % 2 ? "sigh_1" : "sigh_0", 0.2f, 0, 1.0f);
    menu_show_speech_bubble(line, POLICE_BUBBLE_MS);
}

static void police_idle(void *user) {
    (void) user;
    g_police_timer = 0;
    if (!popup_visible("policePop")) return;
    police_say(POLICE_IDLE_LINES[rand_between(0, 4)]);
    g_police_timer = html_timer_add(rand_between(15000, 30000), police_idle, NULL);
}

int ui_police_active(void) { return g_police_class >= 0; }

void ui_loadout_hide_police_pop(void) {
    if (popup_visible("policePop")) {
        if (g_police_class >= 0) {
            g_police_class = -1;
            if (g_police_saved >= 0) menu_ui()->client->menu_class_index = g_police_saved;
            g_police_saved = -1;
        }
        refresh_class_box(); // dJ() with dI = {}
        audio_stop("ambient_9");
    }
    if (g_police_timer) html_timer_cancel(g_police_timer);
    g_police_timer = 0;
    set_disp("policePop", "none");
    menu_schedule_rect();
}

static void reset_pops(void) {
    static const char *pops[] = {"confPremiumPop", "confPop", "gameCodePop", "tradePop", "twitchPop", "pfpPop", "dropsPop"};
    set_disp("popupHolder", "block");
    set_disp("popupContent", "none");
    for (size_t i = 0; i < sizeof(pops) / sizeof(pops[0]); i++) set_disp(pops[i], "none");
    ui_loadout_hide_police_pop();
}

void ui_loadout_clear_pops(void) {
    ui_dialog_cancel(); // a pending confirm/prompt (ui_dialog.c) counts as "Cancel"
    reset_pops();
    set_disp("popupHolder", "none");
    menu_schedule_rect();
}

// shoPolicePop() for a guest (hj = 1): Officer Buddy in the class preview, the intro
// line, the K.P.D text and the (disabled) application form.
void ui_loadout_show_police_pop(void) {
    reset_pops();
    set_disp("policePop", "block");
    MenuUi *ui = menu_ui();
    g_police_saved = ui->client->menu_class_index;
    g_police_class = POLICE_CLASS;
    ui->client->menu_class_index = POLICE_CLASS; // dI.class, not saved
    refresh_class_box();
    audio_play("ambient_9", 0.14f, 1, 1.0f);
    if (g_police_timer) html_timer_cancel(g_police_timer);
    g_police_timer = html_timer_add(rand_between(15000, 30000), police_idle, NULL);
    police_say("Sign In to Join the Fight against the Hackers!");
    HtmlElem c = html_dom_by_id("policePopC");
    if (c) {
        html_dom_set_html(c,
            "<div style='font-size:30px;margin-bottom:20px'>Krunker Police Department</div>"
            "<div style='color:rgba(0,0,0,0.6);font-size:19px'>The <b>K.P.D</b> allows you to <b>Combat &amp; Report Hackers</b> in an easier way. "
            "<div style='height:15px'></div>If you suspect that there is a Hacker in your Current Lobby you can <b>Call for Aid by pressing the K.P.D Icon on the player list</b>. "
            "<div style='height:15px'></div>Any Active <b>Moderators</b> will then be able to respond to your call from a list of Active Distress calls &amp; Deal with the Hacker accordingly.</div>"
            "<div style='height:15px'></div>"
            "<input class='polApplc' type='text' placeholder='Your Discord ID (USER#0000)'></input>"
            "<input class='polApplc' type='text' placeholder='Why would you be a good Fit?'></input>"
            "<div class='button buttonD' id='activateKDM'>Accepting Applications Soon</div>");
    }
    menu_schedule_rect();
}
