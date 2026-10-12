// The Change Loadout window and everything it opens, the K.P.D popup and the
// Changelog: the C port of Krunker v7.1.4's menu logic for them (src/index.js):
//   - the selected* globals (skinColIndex, hatIndex, bodyIndex, ..., streakIndex1-3, charms)
//     with their localStorage keys and `getSavedVal(key) || default` defaults
//   - windows[2] (showWindow(3)): #selectorContainer tabs Class / Loadout / Customize /
//     Other (switchTab), genLoadouts / action (loadout presets)
//   - the item pickers (f1828 getItemsList and the windows built on it: 16 hat, 17 body,
//     26 melee, 34 dye, 38 waist, 39 face, 40 shoes, 43 pet, 44 wrist, 46 charm, 50 back,
//     51 head, 52 player card; 15 weapon skin, 20 secondary, 9 spray, 31 attachment,
//     32 reticle, 41 scope, 45 streaks) and their select*() handlers, which save and go back
//     to window 3 (custClear: windows[n].clear())
//   - shoPolicePop / hidePolicePop / resetPops / clearPops (K.P.D, "Officer Buddy")
//   - windows[34] Changelog (docs/versions.txt: "Whats New" / every update) and its toggleLog
// Items come from KrunkNative's store table (client/src/viewer/viewer_store.h, the v3.1.1
// skins until the v7 table lands): a guest owns none, a kn_brain account its acc.skins and
// offline mode every skin. v7's new slots (head, back, shoes, pet, wrist, charm, player card)
// have no items in that table yet, so their pickers show Krunker's None / Random cards.
#include <ui_logic.h>
#include <audio.h>
#include <backend_ui.h>
#include <online_mode.h> // offline mode: every skin unlocked
#include <render_settings.h>
#include "../viewer/viewer_store.h"
#include <sprays.h> // SPRAYS_BASE_COUNT
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------- Krunker tables

// Weapons: g_weapons (name, preview src, secondary) is Krunker's weaponConfig in id order.
// The rest is UI-only weaponConfig data of v7.1.4 (src/data/weapons.js): `attach != null`
// (v7 has no forceAttach), `scope`, `charm` and the secondaries' `minRec` unlock level.
typedef struct {
    const char *name;
    const char *src;   // textures/previews/weapons/<src>.png; NULL = undefined (the knife)
    int attach, scope, charm, secondary, min_rec;
} LoadoutWeapon;

#define V7_WEAPON_COUNT 29
static const unsigned char WEAPON_ATTACH[V7_WEAPON_COUNT] = {[1] = 1, [3] = 1, [6] = 1, [7] = 1, [13] = 1, [14] = 1, [18] = 1};
static const unsigned char WEAPON_SCOPE[V7_WEAPON_COUNT] = {[0] = 1, [28] = 1};
static const unsigned char WEAPON_NO_CHARM[V7_WEAPON_COUNT] = {[12] = 1, [17] = 1, [19] = 1, [23] = 1, [25] = 1, [26] = 1};
static const unsigned char WEAPON_MIN_REC[V7_WEAPON_COUNT] = {[10] = 15, [11] = 50, [15] = 20, [16] = 30, [21] = 40, [22] = 25, [24] = 35, [27] = 10};

static const LoadoutWeapon *weapon_at(const int id) {
    static LoadoutWeapon w;
    if (id < 0 || id >= WEAPON_COUNT || !g_weapons[id]) return NULL;
    w.name = g_weapons[id]->name ? g_weapons[id]->name : "";
    w.src = g_weapons[id]->src;
    w.secondary = g_weapons[id]->secondary;
    w.attach = id < V7_WEAPON_COUNT && WEAPON_ATTACH[id];
    w.scope = id < V7_WEAPON_COUNT && WEAPON_SCOPE[id];
    w.charm = id >= V7_WEAPON_COUNT || !WEAPON_NO_CHARM[id];
    w.min_rec = id < V7_WEAPON_COUNT ? WEAPON_MIN_REC[id] : 0;
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

// config.skinColors (col as hex, Krunker prints col.toString(16)): Bronze .. Pale, Infected.
static const char *SKIN_COLORS[] = {"805c4a", "a77860", "d38d6f", "cc997e", "edbfa6", "819481"};
#define SKIN_COLOR_COUNT 6

// data/reticles.js (id == index)
static const char *RETICLES[] = {"Red Dot", "Green Dot", "Pyramid", "Tau", "Ray", "Spread",
                                 "Marine", "Underline", "Precision", "Quantum", "Triad", "Nuclear"};
#define RETICLE_COUNT ((int) (sizeof(RETICLES) / sizeof(RETICLES[0])))

// data/aimRecticles.js (scope overlays): name, id (textures/scopes/scope_<id>.png)
static const struct { const char *name; int id; } SCOPES[] = {
    {"Default", 0}, {"Pro", 1}, {"OG", 2}, {"Waspy", 4}, {"MoonJuice", 5}, {"Swixie", 6}, {"Sorable", 7},
};
#define SCOPE_COUNT ((int) (sizeof(SCOPES) / sizeof(SCOPES[0])))

// data/attach.js (Game.attach, id == index)
static const char *ATTACHMENTS[] = {"Reflex Sight", "Acog Scope"};
#define ATTACH_COUNT 2

// data/streaks.js (Game.streaks, index order): name, slot, kills, devOnly
static const struct { const char *name; int slot, kills, dev; } STREAKS[] = {
    {"Nuke", 2, 25, 0}, {"Airdrop", 1, 12, 0}, {"Refill", 0, 5, 1}, {"Bombardment", 2, 20, 1},
    {"Slimer", 0, 7, 0}, {"Juggernaut", 2, 30, 0}, {"War Machine", 1, 15, 0},
};
#define STREAK_COUNT ((int) (sizeof(STREAKS) / sizeof(STREAKS[0])))

// config.playerCardStats names, in order.
static const char *PC_STATS[] = {"SPK", "Kills", "Deaths", "KDR", "KPG", "Games Played", "Wins", "W/L",
                                 "Nukes", "Melees", "Accuracy", "KR", "Headshots", "Losses", "Wallbangs",
                                 "Beatdowns", "Bullseyes", "Sprays Placed", "Assists", "Inventory Value",
                                 "Crouches", "Shots", "Hits", "Misses", "Legshots", "Slimers", "Juggernauts",
                                 "War Machines"};
#define PC_STAT_COUNT ((int) (sizeof(PC_STATS) / sizeof(PC_STATS[0])))

#define RANK_VAR 0.03 // config.rankVar (class levels)

// ---------------------------------------------------------------- selected* state

// `var selectedX = getSavedVal(key) || default`: one saved integer per key.
typedef struct {
    const char *key;
    int def;
    int value;
} Selected;

enum {
    SEL_SKIN_COL, SEL_HAIR_COL, SEL_CHAT_COL, SEL_DYE, SEL_FACE, SEL_SHOE, SEL_ATTACH, SEL_RETICLE, SEL_SCOPE,
    SEL_MELEE, SEL_BODY, SEL_BACK, SEL_HAT, SEL_HEAD, SEL_CARD, SEL_WAIST, SEL_PET, SEL_WRIST, SEL_SECONDARY,
    SEL_PC_STAT, SEL_SPRAY, SEL_STREAK1, SEL_STREAK2, SEL_STREAK3, SEL_COUNT
};

static Selected g_sel[SEL_COUNT] = {
    [SEL_SKIN_COL] = {"skinColIndex", -1, -1}, [SEL_HAIR_COL] = {"hairCol", -1, -1},
    [SEL_CHAT_COL] = {"chatCol", -1, -1},      [SEL_DYE] = {"dyeIndex", -1, -1},
    [SEL_FACE] = {"faceIndex", -1, -1},        [SEL_SHOE] = {"shoeIndex", -1, -1},
    [SEL_ATTACH] = {"attachIndex", 0, 0},      [SEL_RETICLE] = {"reticleIndex", 0, 0},
    [SEL_SCOPE] = {"scopeIndex", 0, 0},        [SEL_MELEE] = {"meleeIndex", -1, -1},
    [SEL_BODY] = {"bodyIndex", -1, -1},        [SEL_BACK] = {"backIndex", -1, -1},
    [SEL_HAT] = {"hatIndex", -1, -1},          [SEL_HEAD] = {"headIndex", -1, -1},
    [SEL_CARD] = {"playerCardIndex", -1, -1},  [SEL_WAIST] = {"waistIndex", -1, -1},
    [SEL_PET] = {"petIndex", -1, -1},          [SEL_WRIST] = {"wristIndex", -1, -1},
    [SEL_SECONDARY] = {"secondaryInd", 2, 2},  [SEL_PC_STAT] = {"pcStatIndex", -1, -1},
    [SEL_SPRAY] = {"sprayIndex", 0, 0},
    [SEL_STREAK1] = {"streakIndex1", -1, -1},  [SEL_STREAK2] = {"streakIndex2", -1, -1},
    [SEL_STREAK3] = {"streakIndex3", 0, 0},
};
static int g_sel_loaded;
static int g_skin_weapon = 1; // activeSkinSel: the weapon the Select Skin window is for

static void sel_save(int which, int value);

// f1795(slot, index, default): a streak index stays only when it exists and is for that slot.
static int streak_valid(const int slot, const int index) {
    return index >= 0 && index < STREAK_COUNT && STREAKS[index].slot == slot ? index : -1;
}

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
    for (int k = 0; k < 3; k++) g_sel[SEL_STREAK1 + k].value = streak_valid(k, g_sel[SEL_STREAK1 + k].value);
    // KrunkNative v3.1.1 -> v7.1.4 migration: v3's "Back" slot (backIndex) held body items
    // (store type 2), which v7 calls Body (bodyIndex); v7's Back is the new type 14. The
    // Killcard stat (kcStatIndex) became the Playercard stat (pcStatIndex). Moved once.
    char buf[32];
    if (html_storage_get("knV7LoadoutMigrated", buf, sizeof(buf)) < 0) {
        html_storage_set("knV7LoadoutMigrated", "1");
        if (html_storage_get("bodyIndex", buf, sizeof(buf)) < 0 && html_storage_get("backIndex", buf, sizeof(buf)) >= 0 && buf[0]) {
            sel_save(SEL_BODY, (int) strtol(buf, NULL, 10));
            sel_save(SEL_BACK, -1);
        }
        if (html_storage_get("pcStatIndex", buf, sizeof(buf)) < 0 && html_storage_get("kcStatIndex", buf, sizeof(buf)) >= 0 && buf[0])
            sel_save(SEL_PC_STAT, (int) strtol(buf, NULL, 10));
        // v3 saved the spray as "sprayindex", v7 as "sprayIndex"
        if (html_storage_get("sprayIndex", buf, sizeof(buf)) < 0 && html_storage_get("sprayindex", buf, sizeof(buf)) >= 0 && buf[0])
            sel_save(SEL_SPRAY, (int) strtol(buf, NULL, 10));
    }
}

static int sel(const int which) {
    sel_load();
    return g_sel[which].value;
}

static void sel_save(const int which, const int value) {
    g_sel_loaded = 1;
    g_sel[which].value = value;
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", value);
    html_storage_set(g_sel[which].key, buf);
}

// selectedCharms (localStorage "charms", [primary, secondary]): -1 none.
static int charm_at(const int slot) {
    char buf[256] = "[]";
    html_storage_get("charms", buf, sizeof(buf));
    cJSON *a = cJSON_Parse(buf);
    const cJSON *v = cJSON_GetArrayItem(a, slot);
    const int r = cJSON_IsNumber(v) ? v->valueint : -1;
    cJSON_Delete(a);
    return r;
}

static void charm_save(const int slot, const int value) {
    char buf[256] = "[]";
    html_storage_get("charms", buf, sizeof(buf));
    cJSON *a = cJSON_Parse(buf);
    if (!cJSON_IsArray(a)) {
        cJSON_Delete(a);
        a = cJSON_CreateArray();
    }
    while (cJSON_GetArraySize(a) <= slot) cJSON_AddItemToArray(a, cJSON_CreateNull());
    cJSON_ReplaceItemInArray(a, slot, value >= 0 || value == -2 ? cJSON_CreateNumber(value) : cJSON_CreateNull());
    char *text = cJSON_PrintUnformatted(a);
    if (text) html_storage_set("charms", text);
    free(text);
    cJSON_Delete(a);
}

int ui_loadout_skin_color(void) {
    const int i = sel(SEL_SKIN_COL);
    return i >= 0 && i < SKIN_COLOR_COUNT ? (int) strtol(SKIN_COLORS[i], NULL, 16) : -1;
}

// The look sent with the spawn request (ENT): skin colour and the cosmetic picks. "Random" (-2) resolves to
// one of the owned items, and a guest owns none, so it is none (-1) like the preview's `dw == -2 ? -1 : dw`;
// v7's explicit None (-3) is none too. hairCol stays the class colour (-1): Krunker only takes a hex string
// there, which the picker never sets. PlayerCosmetics.back is the type-2 body item (v7 Body, bodyIndex).
void ui_loadout_cosmetics(PlayerCosmetics *out) {
    const int pick[5] = {sel(SEL_HAT), sel(SEL_BODY), sel(SEL_WAIST), sel(SEL_FACE), sel(SEL_DYE)};
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
        {SEL_HAT, 1}, {SEL_BODY, 2}, {SEL_WAIST, 6}, {SEL_FACE, 7}, {SEL_DYE, 5},
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

// streakIndex1..3 (the streak loadout, one per slot): -1 none.
int ui_loadout_streak(const int slot) {
    return slot >= 0 && slot < 3 ? sel(SEL_STREAK1 + slot) : -1;
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

// KrunkNative's default sprays: v3.1.1's Game.sprays (src/data/sprays.js, the indices sprays.c
// resolves: index -> textures/sprays/<id>.png). v7.1.4 sells them as free store sprays
// ("Free Spray"); the v7 store table (6448 skins) replaces this with WP-F's import.
static const struct { const char *name; int id; } BASE_SPRAYS[SPRAYS_BASE_COUNT] = {
    {"Krunker", 0}, {"Target", 1}, {"GG", 2}, {"Pumpkin", 3}, {"Spooky", 4}, {"Ded", 5}, {"Sadface", 6}, {"Laugh Cry", 7},
    {"Sid", 8}, {"Vince", 9}, {"Kiki", 10}, {"Ladder", 11}, {"Dummy", 12}, {"8 Ball", 13}, {"xD", 14}, {"MMOK", 15},
    {"Madman", 16}, {"Terminal", 17}, {"YODO", 18}, {"Crayon", 19}, {"Myth", 20}, {"SpikyJohn", 21}, {"LevelGaming", 22},
    {"PolarAce", 23}, {"FrostyWolf", 24}, {"Waspy", 25}, {"Kpal", 26}, {"Rain Song", 27}, {"nV", 28}, {"BV Hype", 29},
    {"Kaarson", 30}, {"Deal With It", 31}, {"Rice", 32}, {"Lore", 33}, {"Desu Sid", 34}, {"Anomaly", 35}, {"FaZe", 36},
    {"Nudah", 37}, {"Misfits", 38}, {"Bill $aber", 54}, {"Fields Food v1", 55}, {"Fields Food v2", 56}, {"Phil", 57},
    {"VoKUS", 58}, {"StremZ", 59}, {"Zed", 63}, {"Northern Forces", 64}, {"Sorable", 65}, {"Syctis", 66},
    {"Moon Juice", 79}, {"caL", 83}, {"Quacky", 87}, {"Verb", 88},
};
static const char *spray_name(const int index) {
    return index >= 0 && index < SPRAYS_BASE_COUNT ? BASE_SPRAYS[index].name : NULL;
}


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

// genLoadouts(true): the preset row (#loadoutHolder content).
static void gen_loadouts(StrBuf *out) {
    sb_append(out, "<span class='settNameSmall' style='margin:0;margin-bottom:5px;border-bottom: unset;'><input id='loadoutName' type='text' class='formInput' placeholder='Loadout name' autocomplete='off' autocorrect='off' autocapitalize='off' spellcheck='false' style='border:none;background:#eee;padding-bottom:6px;float:left;margin-top:5px;margin-bottom:5px;'><select id='loadoutSelect'><option selected>Select</option>");
    char buf[8192];
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
    sb_append(out, "</select><div class='loadoutBtn' onclick='windows[2].action(2)'>Delete</div><div class='loadoutBtn' onclick='windows[2].action(1)'>Save</div><div class='loadoutBtn' onclick='windows[2].action(0)'>Load</div></span>");
}

// A store item's thumbnail (UTILS.getPreview / skin.thumbnail).
static void skin_thumb(const int index, char *out, const size_t n) {
    store_skin_preview(index, out, n);
    if (out[0] == '.' && out[1] == '/') memmove(out, out + 2, strlen(out + 2) + 1);
}

// UTILS.assetsUrl(path): files ui/ carries are page-relative, the rest come from the asset tree.
static void asset(const char *path, char *out, const size_t n) {
    html_dom_asset_url(path, out, n);
}

static int g_tab = 1;          // windows[2].tabIndex
static int g_charm_slot;       // vLN01715: the loadout slot charmSelector opened Select Charm for

// The selected item of one slot as windows[2].gen shows it.
typedef struct {
    char name[96];
    const char *color;   // label colour
    const char *glow;    // rarity colour for the drop-shadow, NULL = none
    int animate;         // rarity animates (rainbowText)
    char img[256];
} SlotView;

// A store item picked for a slot, if the player may show it (has_inventory()).
static int slot_item(SlotView *v, const int index) {
    const ViewerSkin *s = has_inventory() ? store_skin(index) : NULL;
    if (!s) return 0;
    snprintf(v->name, sizeof(v->name), "%s", s->name ? s->name : "");
    v->color = rarity_color(s);
    v->glow = rarity_color(s);
    v->animate = rarity_anim(s);
    skin_thumb(index, v->img, sizeof(v->img));
    return 1;
}

// <span class='settLabel ...'>name</span> + the icon / Random / None of a .custContainer
static void cust_label_icon(StrBuf *out, const SlotView *v, const char *label_style, const char *img_class) {
    sb_appendf(out, "<span class='settLabel %s%s' style='color:%s%s'>", !strcmp(v->color, "#292929") ? "greyShad" : "blackShad",
               v->animate ? " rainbowText" : "", v->color, label_style);
    sb_append_esc(out, v->name);
    sb_append(out, "</span>");
    if (!strcmp(v->name, "Random")) {
        sb_appendf(out, "<img draggable='false' class='%s custSil' src='%s'><div class='selRandom blackShad'>?</div>", img_class, v->img);
    } else if (!strcmp(v->name, "None")) {
        sb_append(out, "<div class='selRandom blackShad'>X</div>");
    } else {
        sb_appendf(out, "<img class='%s' src='%s'", img_class, v->img);
        if (v->glow) sb_appendf(out, " style='filter:drop-shadow(0 0 0.2rem %s88)'", v->glow);
        sb_append(out, ">");
    }
}

// A charm sub-box (custSubContainer onclick charmSelector(slot)).
static void charm_box(StrBuf *out, const int slot) {
    const int pick = charm_at(slot);
    const ViewerSkin *s = has_inventory() ? store_skin(pick) : NULL;
    char img[256];
    asset("textures/previews/cosmetics/12_default.png", img, sizeof(img));
    sb_appendf(out, "<div class=\"custSubContainer\" onclick=\"charmSelector(%d)\">\n\t\t\t\t<div class=\"custItNm\" style=\"font-size: 12px;\">Charm</div>\n\t\t\t\t", slot);
    if (s) {
        skin_thumb(pick, img, sizeof(img));
        sb_appendf(out, "<img class=\"custIcon\" src=\"%s\"  style=\"margin-top:0;filter:drop-shadow(0 0 0.2rem %s88)\">", img, rarity_color(s));
    } else if (pick == -2) {
        sb_appendf(out, "<img draggable='false' class='custIcon custSil' src='%s'>\n\t\t\t\t\t<div class='selRandom blackShad' style=\"font-size: 20px;\">?</div>", img);
    } else {
        sb_append(out, "<div class=\"selRandom blackShad\" style=\"font-size: 20px;\">X</div>");
    }
    sb_append(out, "\n\t\t\t</div>");
}

// tab 0: the class cards (Game.config.classes). Classes that need an account stay unlocked:
// the offline player counts as having one (doc/v7/decisions.md) and v7's sandbox does the same.
static void gen_tab_class(StrBuf *out) {
    const MenuUi *ui = menu_ui();
    const AccountInfo *acc = account_get();
    sb_append(out, "<div class='skinList' style='text-align: center;'>");
    for (int i = 0; i < ui->class_count; i++) {
        const MenuClass *c = &ui->classes[i];
        char key[16];
        snprintf(key, sizeof(key), "c%d", c->index);
        const cJSON *xp_v = acc && acc->stats ? cJSON_GetObjectItem(acc->stats, key) : NULL;
        const double xp = cJSON_IsNumber(xp_v) ? xp_v->valuedouble : 0.0;
        const double lv = RANK_VAR * sqrt(xp);
        int level = (int) floor(lv);
        const int pct = (int) lround((lv - level) * 100.0);
        const double lo = level > 1 ? pow(level / RANK_VAR, 2) : 0.0;
        const double hi = pow((level + 1) / RANK_VAR, 2);
        level = level + 1 > 1 ? level + 1 : 1;
        char icon[256];
        snprintf(icon, sizeof(icon), "%s", c->icon);
        sb_appendf(out, "<div class=\"classCard\" onmouseenter=\"playTick()\" onclick=\"selectClass(%d)\">\n\t\t\t\t\t\t<div class=\"classImgH\">\n\t\t\t\t\t\t\t<img loading=\"lazy\" style=\"image-rendering:pixelated\" class=\"classImgC\" src=\"%s\">\n\t\t\t\t\t\t\t<div class='classHP'>%d<span style='color: #868484;font-size: 12px;'> HP</span></div>\n\t\t\t\t\t\t</div>\n\t\t\t\t\t\t<div class=\"className\">",
                   c->index, icon, c->hp);
        sb_append_esc(out, c->name ? c->name : "");
        sb_append(out, "</div>\n\t\t\t\t\t\t<div class=\"classWeap\">");
        sb_append_esc(out, c->weapon ? c->weapon : "");
        sb_appendf(out, "</div>\n\t\t\t\t\t\t<div class='classXPBar'>\n\t\t\t\t\t\t\t<div class='clsXPBarC' style='width:calc(%d%% - 8px)'></div>\n\t\t\t\t\t\t\t<div class='clsXPBarV'>%d / %d</div>\n\t\t\t\t\t\t</div>\n\t\t\t\t\t\t<div class='classLvl'>Lvl %d</div>\n\t\t\t\t\t\t\n\t\t\t\t\t</div>",
                   pct, (int) (xp - lo), (int) (hi - lo), level);
    }
    sb_append(out, "</div>");
}

// tab 1: presets, primary (+ charm, sight), secondary (+ charm), melee, reticle / scope, streaks.
static void gen_tab_loadout(StrBuf *out, const int class_index) {
    const LoadoutClass lc = class_at(class_index);
    LoadoutWeapon pw_copy, sw_copy; // weapon_at returns one static slot
    const LoadoutWeapon *pw = weapon_at(lc.primary) ? (pw_copy = *weapon_at(lc.primary), &pw_copy) : NULL;
    const int knife = lc.primary == 12;
    SlotView prim = {.color = "#efefef"}, sec = {.color = lc.secondary ? "#2196F3" : "#efefef"}, mel = {.color = "#efefef"};
    snprintf(prim.name, sizeof(prim.name), "%s", knife ? "None" : pw ? pw->name : "");
    snprintf(prim.img, sizeof(prim.img), "textures/previews/weapons/%s.png", pw && pw->src ? pw->src : "undefined");
    const int sec_id = sel(SEL_SECONDARY);
    const LoadoutWeapon *sw = weapon_at(sec_id) ? (sw_copy = *weapon_at(sec_id), &sw_copy) : NULL;
    snprintf(sec.name, sizeof(sec.name), "%s", lc.secondary && sw ? sw->name : "None");
    snprintf(sec.img, sizeof(sec.img), "textures/previews/weapons/%s.png", sw && sw->src ? sw->src : "undefined");
    const int melee = sel(SEL_MELEE);
    snprintf(mel.name, sizeof(mel.name), "%s", melee == -2 ? "Random" : "Combat Knife");
    snprintf(mel.img, sizeof(mel.img), "textures/previews/cosmetics/3_default.png");
    if (has_inventory()) { // `if (account)`: the selected skins
        const int ps = weapon_skin(lc.primary);
        if (!knife && !slot_item(&prim, ps) && ps == -2) {
            snprintf(prim.name, sizeof(prim.name), "Random");
            prim.color = "#2196F3";
        }
        const int ss = lc.secondary ? weapon_skin(sec_id) : -1;
        if (lc.secondary && !slot_item(&sec, ss) && ss == -2) snprintf(sec.name, sizeof(sec.name), "Random");
        if (melee >= 0) slot_item(&mel, melee);
    }
    const int has_attach = pw && pw->attach;
    const int has_scope = pw && pw->scope;

    sb_append(out, "<div id='loadoutHolder' style='text-align:left;margin-bottom:5px;margin-top: -8px;'>");
    gen_loadouts(out);
    sb_append(out, "</div><div style='margin-top: 10px;'>");
    // primary
    if (knife) sb_append(out, "<div class='custContainer' style='width: 47.5%;height: 163px;' onmouseenter='playTick()'>");
    else sb_appendf(out, "<div class='custContainer contCanSelect' onclick='skinSelector(%d)' style='width: 47.5%%;height: 163px;' onmouseenter='playTick()'>", lc.primary);
    sb_append(out, "<div class='custItNm'>Primary</div>");
    cust_label_icon(out, &prim, ";right: unset;left: 8px;", "custIcon");
    const int prim_charm = pw && pw->charm;
    if (prim_charm) charm_box(out, 0);
    if (has_attach) {
        const int a = sel(SEL_ATTACH);
        sb_appendf(out, "<div class=\"custSubContainer contCanSelect%s\" onclick=\"viewAttachments()\">\n\t\t\t\t<div class=\"custItNm\" style=\"font-size: 12px;\">Sight</div>\n\t\t\t\t", prim_charm ? " bottom" : "");
        if (a >= 0 && a < ATTACH_COUNT) {
            char img[256], path[64];
            snprintf(path, sizeof(path), "textures/previews/attach/attach_%d.png", a);
            asset(path, img, sizeof(img));
            sb_appendf(out, "<img class=\"custIcon\" src=\"%s\">", img);
        } else {
            sb_append(out, "<div class=\"selRandom blackShad\" style=\"font-size: 20px;\">X</div>");
        }
        sb_append(out, "\n\t\t\t</div>");
    }
    sb_append(out, "</div> ");
    // secondary
    if (lc.secondary) sb_append(out, "<div class='custContainer contCanSelect' onclick='showWindow(20)' style='width: 47.5%;height: 163px;' onmouseenter='playTick()'>");
    else sb_append(out, "<div class='custContainer' style='width: 47.5%;height: 163px;' onmouseenter='playTick()'>");
    sb_append(out, "<div class='custItNm'>Secondary</div>");
    cust_label_icon(out, &sec, ";right: unset;left: 8px;", "custIcon");
    if (lc.secondary && sw && sw->charm) charm_box(out, 1);
    sb_append(out, "</div> ");
    // melee
    sb_append(out, "<div class='custContainer contCanSelect' onclick='showWindow(26)' style='width: 30.7%;height: 163px;' onmouseenter='playTick()'><div class='custItNm'>Melee Skin</div>");
    cust_label_icon(out, &mel, "", "custIcon");
    sb_append(out, "</div> ");
    // reticle (attachment weapons) or scope (scoped weapons)
    char rname[64], rimg[512] = "";
    if (has_attach) {
        const int r = sel(SEL_RETICLE);
        if (r >= 0 && r < RETICLE_COUNT) {
            snprintf(rname, sizeof(rname), "%s", RETICLES[r]);
            snprintf(rimg, sizeof(rimg), "textures/reticles/reticle_%d.png", r);
        } else if (r == -2) {
            snprintf(rname, sizeof(rname), "Custom");
            html_storage_get("savedReticle", rimg, sizeof(rimg));
        } else {
            snprintf(rname, sizeof(rname), "None");
        }
    } else {
        const int sc = sel(SEL_SCOPE);
        if (has_scope && sc >= 0 && sc < SCOPE_COUNT) {
            char path[64];
            snprintf(rname, sizeof(rname), "%s", SCOPES[sc].name);
            snprintf(path, sizeof(path), "textures/scopes/scope_%d.png", SCOPES[sc].id);
            asset(path, rimg, sizeof(rimg));
        } else if (sc == -2) {
            snprintf(rname, sizeof(rname), "Custom");
            html_storage_get("savedScope", rimg, sizeof(rimg));
        } else {
            snprintf(rname, sizeof(rname), "None");
        }
    }
    const int pickable = has_attach || has_scope;
    if (pickable) sb_appendf(out, "<div class='custContainer contCanSelect' onclick='showWindow(%d)' onmouseenter='playTick()' style='width: 30.7%%;height: 163px;'>", has_attach ? 32 : 41);
    else sb_append(out, "<div class='custContainer' onmouseenter='playTick()' style='width: 30.7%;height: 163px;'>");
    sb_appendf(out, "<div class='custItNm'>%s</div><span class='settLabel blackShad' style='color:%s'>", has_attach ? "Reticle" : "Scope",
               pickable && strcmp(rname, "None") ? "#2196F3" : "#efefef");
    sb_append_esc(out, rname);
    sb_append(out, "</span>");
    if (pickable && strcmp(rname, "None") && rimg[0]) {
        sb_append(out, "<img class='custIconScope' src='");
        sb_append_esc(out, rimg);
        sb_append(out, "'>");
    } else {
        sb_append(out, "<div class='selRandom blackShad'>X</div>");
    }
    sb_append(out, "</div> ");
    // streaks
    sb_append(out, "<div class='custContainer contCanSelect' onclick='showWindow(45)' onmouseenter='playTick()' style='width: 30.7%;height: 163px;'><div class='custItNm'>Streaks</div>");
    StrBuf icons = {0};
    for (int k = 0; k < 3; k++) {
        const int st = sel(SEL_STREAK1 + k);
        if (st >= 0 && st < STREAK_COUNT) sb_appendf(&icons, "<img class='custStreakIcon' src='img/streaks/%d.png'>", st);
    }
    char *ic = sb_finish(&icons);
    if (ic[0]) sb_appendf(out, "<div class='custStreakIconH'>%s</div>", ic);
    else sb_append(out, "<div class='selRandom blackShad'>X</div>");
    free(ic);
    sb_append(out, "</div></div>");
}

// The Customize tab's slots: [label i18n, store type, selection, picker window, premium pair]
typedef struct {
    const char *key, *label;
    int type, which, window, pair; // pair: index of the slot it may not be combined with (-1 none)
} CustSlot;

static const CustSlot CUST_SLOTS[] = {
    {"hat", "Hat", 1, SEL_HAT, 16, 1},       {"head", "Head", 15, SEL_HEAD, 51, 0},
    {"face", "Face", 7, SEL_FACE, 39, -1},   {"body", "Body", 2, SEL_BODY, 17, 4},
    {"back", "Back", 14, SEL_BACK, 50, 3},   {"dye", "Dye", 5, SEL_DYE, 34, -1},
    {"waist", "Waist", 6, SEL_WAIST, 38, -1}, {"shoe", "Shoes", 8, SEL_SHOE, 40, -1},
    {"pet", "Pet", 9, SEL_PET, 43, -1},      {"wrist", "Wrist", 11, SEL_WRIST, 44, -1},
    {"spray", "Spray", -1, SEL_SPRAY, 9, -1}, {"playerCard", "Player Card", 16, SEL_CARD, 52, -1},
};
#define CUST_SLOT_COUNT ((int) (sizeof(CUST_SLOTS) / sizeof(CUST_SLOTS[0])))

static int slot_used(const int index) { return index >= 0 || index == -2; }

// Non-premium accounts may not wear hat + head or body + back together (v7 decision 9; the
// offline sandbox player is exempt like v7's sandbox, a dev account too).
static int premium_pair_blocked(const int i) {
    const AccountInfo *acc = account_get();
    if (CUST_SLOTS[i].pair < 0 || !acc || acc->premium || acc->is_dev || all_skins_unlocked()) return 0;
    return slot_used(sel(CUST_SLOTS[CUST_SLOTS[i].pair].which)) && slot_used(sel(CUST_SLOTS[i].which));
}

static void gen_tab_customize(StrBuf *out) {
    int frames_seen[16], frames_n = 0;
    for (int i = 0; i < CUST_SLOT_COUNT; i++) {
        const CustSlot *cs = &CUST_SLOTS[i];
        int pick = sel(cs->which);
        if (i > CUST_SLOTS[i].pair && premium_pair_blocked(i)) {
            sb_appendf(out, "<div class='custContainer' onclick='windows[13].premium()' style='width: 22.5%%;height: 174px;'><div class='custItNm blackShad'>%s</div>", cs->label);
            sb_appendf(out, "<div class=\"custPremReq blackShad\">\n\t\t\t\t\t\t<div style=\"color: var(--yellow);font-size: 15px;\">Premium Required to use Both</div>\n\t\t\t\t\t\tor\n\t\t\t\t\t\t<div style=\"color: var(--red);font-size: 15px;\">Remove %s</div>\n\t\t\t\t\t</div></div> ",
                       CUST_SLOTS[cs->pair].label);
            continue;
        }
        SlotView v = {.color = "#efefef"};
        snprintf(v.name, sizeof(v.name), "%s", pick == -2 ? "Random" : "None");
        char path[64];
        if (cs->type < 0) snprintf(path, sizeof(path), "textures/sprays/0.png");
        else snprintf(path, sizeof(path), "textures/previews/cosmetics/%d_default.png", cs->type);
        snprintf(v.img, sizeof(v.img), "%s", path);
        const char *img_class = cs->type == 16 ? "custIconCard" : "custIcon";
        int spray_item = 0;
        const char *spray_base = cs->type < 0 ? spray_name(pick) : NULL;
        const ViewerSkin *s = cs->type < 0 && spray_base ? NULL : (has_inventory() ? store_skin(pick) : NULL);
        if (spray_base) { // KrunkNative's default sprays (Game.sprays of v3.1.1): free items
            snprintf(v.name, sizeof(v.name), "%s", spray_base);
            snprintf(v.img, sizeof(v.img), "textures/sprays/%d.png", BASE_SPRAYS[pick].id);
            spray_item = 1;
        } else if (s && (cs->type < 0 ? s->type == 4 : s->type == cs->type)) {
            slot_item(&v, pick);
            v.glow = NULL;
            spray_item = s->type == 4;
        }
        sb_appendf(out, "<div class='custContainer contCanSelect' onclick='showWindow(%d)' onmouseenter='playTick()' style='width: 22.25%%;height: 174px;'><div class='custItNm blackShad'>%s</div><div class='custClear' onclick='windows[%d].clear();'><span class='material-icons'>close</span></div>",
                   cs->window, cs->label, cs->window - 1);
        sb_appendf(out, "<span class='custLabel %s%s' style='color:%s'>", s && s->rarity == 5 ? "greyShad" : "blackShad", v.animate ? " rainbowText" : "", v.color);
        sb_append_esc(out, v.name);
        sb_append(out, "</span>");
        if (!strcmp(v.name, "Random")) {
            sb_appendf(out, "<img draggable='false' class='%s custSil' style='margin-top: 0px;' src='%s'><div class='selRandom blackShad'>?</div>", img_class, v.img);
        } else if (!strcmp(v.name, "None")) {
            sb_append(out, "<div class='selRandom blackShad'>X</div>");
        } else if (spray_item) {
            char anim[96] = "";
            if (s && s->frames) {
                snprintf(anim, sizeof(anim), "animation: custIconAnm%d %gs steps(%d) infinite;", s->frames, 1 + s->frame_t / 1000.0, s->frames);
                int seen = 0;
                for (int k = 0; k < frames_n; k++) seen |= frames_seen[k] == s->frames;
                if (!seen && frames_n < 16) frames_seen[frames_n++] = s->frames;
            }
            sb_appendf(out, "<div draggable='false' class='custIconSpray' style='background: url(&quot;%s&quot;) -1px;%s'></div>", v.img, anim);
        } else {
            sb_appendf(out, "<img class='%s' style='margin-top: 0px;' src='%s'>", img_class, v.img);
        }
        sb_append(out, "</div> ");
    }
    sb_append(out, "<div style='margin-bottom: 10px'></div>");
    if (frames_n) {
        sb_append(out, "<style>");
        for (int k = 0; k < frames_n; k++) sb_appendf(out, "@keyframes custIconAnm%d {100%%{background-position:-%dpx}}", frames_seen[k], frames_seen[k] * 144);
        sb_append(out, "</style>");
    }
}

static void color_row_premium(StrBuf *out, const char *label) {
    sb_appendf(out, "<div class='settName' style='border-bottom:unset;'>%s<i class='material-icons premiumSkinCol' onclick='windows[13].premium()'>add_circle_outline</i>", label);
}

static void gen_tab_other(StrBuf *out) {
    color_row_premium(out, "Skin Color");
    const int skin = sel(SEL_SKIN_COL);
    for (int i = SKIN_COLOR_COUNT - 1; i >= 0; i--) {
        sb_appendf(out, "<div class='skinColorItem%s' style='background-color:#%s' onmouseenter='playTick()' onclick='selectSkinColor(%d)'></div>",
                   skin == i ? " activeSkin" : "", SKIN_COLORS[i], i);
    }
    sb_append(out, "</div>");
    color_row_premium(out, "Hair Color");
    sb_append(out, "</div>");
    color_row_premium(out, "Chat Name Color");
    sb_append(out, "</div>");
    const int stat = sel(SEL_PC_STAT);
    sb_appendf(out, "<div class='settName' style='border-bottom:unset;'>Playercard Stat<select onchange='selectStat(this.value)' class='inputGrey2' style='font-size:16px;padding:4px;margin-bottom: 5px;'><option value='-1' %s>None</option>",
               stat == -1 ? "selected" : "");
    for (int i = 0; i < PC_STAT_COUNT; i++) sb_appendf(out, "<option value='%d' %s>%s</option>", i, stat == i ? "selected" : "", PC_STATS[i]);
    sb_append(out, "</select></div>");
}

// windows[2].gen()
char *ui_loadout_gen(void) {
    const int class_index = menu_ui()->client->menu_class_index;
    static const struct { const char *label, *img, *style, *icon_class; } TABS[4] = {
        {"Class", "img/customize/class.png", "", " pixelated"},
        {"Loadout", "img/customize/loadout.png", "", " crisp-edges"},
        {"Customize", "img/customize/outfit.png", " style='font-size: 17px'", ""},
        {"Other", "img/customize/other.png", "", ""}, // v7 writes other.png + ".png", which its asset host still serves
    };
    StrBuf out = {0};
    sb_append(&out, "<div id='selectorContainer'>");
    for (int t = 0; t < 4; t++) {
        sb_appendf(&out, "<div class='selectorItem%s'%s onclick='windows[2].switchTab(%d)' onmouseenter='playTick()'><div class='selectorLabel'>%s</div><img loading='lazy' class='selectorIcon%s' src='%s'></div>",
                   g_tab == t ? " selectedItem" : "", TABS[t].style, t, TABS[t].label, TABS[t].icon_class, TABS[t].img);
    }
    sb_append(&out, "</div>");
    sb_appendf(&out, "<div id='customizeContainer'%s>", g_tab == 2 ? " style='margin-bottom: -10px;text-align: center;'" : "");
    switch (g_tab) {
        case 0: gen_tab_class(&out); break;
        case 1: gen_tab_loadout(&out, class_index); break;
        case 2: gen_tab_customize(&out); break;
        default: gen_tab_other(&out); break;
    }
    sb_append(&out, "</div>");
    return sb_finish(&out);
}

// ---------------------------------------------------------------- pickers

#define ITEM_SEARCH_HEAD(back, cls) \
    "<div id='itemSearchH'><div class='custBack' onclick='playSelect(0.1);goBackWindow(" back ")' onmouseenter='playTick();'><span class='material-icons custBackArr'>arrow_back</span></div><input id='itemSearch' type='text' placeholder='Search Item' onkeyup='searchItems(this" cls ")'><div class='winClose' onclick='playSelect(0.1);closWind()' onmouseenter='playTick();'><span class='material-icons winCloseArr'>close</span></div></div>"

// One owned item card of f1828 / Select Skin (weapon: the selectSkin weapon, -1 = fn(ind)).
static void owned_card(StrBuf *out, const OwnedSkin *o, const ViewerSkin *s, const char *fn, const char *img_class, const int with_season, const int weapon) {
    const char *col = rarity_color(s);
    char img[256];
    skin_thumb(o->ind, img, sizeof(img));
    sb_appendf(out, "<div class='skinCard%s%s' style='color:%s;border:5px solid%s' onmouseenter='playTick()'onclick='%s(%d",
               s->rarity == 5 ? "" : " blackShad", rarity_anim(s) ? " rainbow" : "", col, col, fn, o->ind);
    if (weapon >= 0) sb_appendf(out, ",%d", weapon);
    sb_appendf(out, ");cancelTip()' data-index='%d' onmouseover='createTip(this)' onmouseout='cancelTip()'><div class='itemCnt' data-badge='x%d'></div>", o->ind, o->cnt);
    sb_append_esc(out, s->name ? s->name : "");
    sb_append(out, "<div class='itemOwn' style='z-index: 3;position: relative;'>by ");
    sb_append_esc(out, skin_creator(o->ind));
    sb_append(out, "</div>");
    if (with_season) sb_appendf(out, "<div class='itemSea'>Season %d</div>", s->seas ? s->seas : 1);
    sb_appendf(out, "<img loading='lazy' %sclass='%s%s' src='%s' /><div class='selctInfoBtn' onmouseenter='playTick()'onclick='viewItem(%d)'>?</div><div class='selctFavBtn material-icons' onmouseenter='playTick()' data-col='white' data-icon='star_border' onclick='favItem(this, %d)'></div></div>",
               weapon >= 0 ? "draggable='false' " : "", img_class, (s->flags & VS_RGB) ? " rgbHue" : "", img, o->ind, o->ind);
}

// f1828(funcName, type): None (-3) + Random (-2) + the owned items of that store type.
static char *gen_items_list(const char *fn, const int type) {
    const char *img_class = type ? (type == 16 ? "skinImgP" : type == 5 ? "skinImgD" : "skinImgC") : "skinImg";
    char img[64];
    snprintf(img, sizeof(img), "textures/previews/cosmetics/%d_default.png", type);
    StrBuf out = {0};
    sb_append(&out, ITEM_SEARCH_HEAD("2", "") "<div id='skinList' style='overflow-y: scroll;height: calc(100% - 95px);'>");
    sb_appendf(&out, "<div class='skinCard blackShad' style='border:5px solid lightgrey' onmouseenter='playTick()' onclick='%s(-3)'>%s<div class='itemOwn'>Default</div>%s<img class='%s' src='%s'></div>",
               fn, type == 3 ? "Combat Knife" : "None", type == 5 ? "" : "<div class='itemSea' style='opacity:0;'>Season 1</div>", img_class, img);
    sb_appendf(&out, "<div class='skinCard blackShad' style='border:5px solid lightgrey' onmouseenter='playTick()' onclick='%s(-2)'>Random<div class='itemOwn' style='opacity:0;'>by \?\?\?</div>%s<img class='%s skinSil' src='%s'><div class='skinRandom blackShad'>?</div></div>",
               fn, type == 5 ? "" : "<div class='itemSea' style='opacity:0;'>All Seasons</div>", img_class, img);
    int n;
    OwnedSkin *owned = owned_skins(&n, 0);
    for (int i = 0; i < n; i++) {
        const ViewerSkin *s = store_skin(owned[i].ind);
        if (s && s->type == type) owned_card(&out, &owned[i], s, fn, img_class, type != 5, -1);
    }
    free(owned);
    sb_append(&out, "</div>");
    return sb_finish(&out);
}

// windows[14] Select Skin for weapon activeSkinSel (back to Select Secondary for secondaries).
static char *gen_skins(void) {
    const LoadoutWeapon *w = weapon_at(g_skin_weapon);
    char img[64];
    snprintf(img, sizeof(img), "textures/previews/weapons/%s.png", w && w->src ? w->src : "undefined");
    StrBuf out = {0};
    sb_appendf(&out, "<div id='itemSearchH'><div class='custBack' onclick='playSelect(0.1);goBackWindow(%d)' onmouseenter='playTick();'><span class='material-icons custBackArr'>arrow_back</span></div><input id='itemSearch' type='text' placeholder='Search Item' onkeyup='searchItems(this)'><div class='winClose' onclick='playSelect(0.1);closWind()' onmouseenter='playTick();'><span class='material-icons winCloseArr'>close</span></div></div>",
               w && w->secondary ? 19 : 2);
    sb_append(&out, "<div id='skinList' style='margin-top: 8px;overflow-y: scroll;height: calc(100% - 95px);'>");
    sb_appendf(&out, "<div class='skinCard blackShad' style='border:5px solid lightgrey' onmouseenter='playTick()' onclick='selectSkin(-3,%d)'>None<div class='itemOwn'>Default</div><div class='itemSea' style='opacity:0;'>Season 1</div><img draggable='false' class='skinImg' src='%s'></div>", g_skin_weapon, img);
    sb_appendf(&out, "<div class='skinCard blackShad' style='border:5px solid lightgrey' onmouseenter='playTick()' onclick='selectSkin(-2,%d)'>Random<div class='itemOwn' style='opacity:0;'>by \?\?\?</div><div class='itemSea' style='opacity:0;'>All Seasons</div><img draggable='false' class='skinImg skinSil' onmouseenter='playTick()' src='%s'><div class='skinRandom blackShad' style='top:68px'>?</div></div>", g_skin_weapon, img);
    int n;
    OwnedSkin *owned = owned_skins(&n, 0);
    for (int i = 0; i < n; i++) {
        const ViewerSkin *s = store_skin(owned[i].ind);
        if (s && !s->type && s->weapon == g_skin_weapon + 1) owned_card(&out, &owned[i], s, "selectSkin", "skinImg", 1, s->weapon - 1);
    }
    free(owned);
    sb_append(&out, "</div>");
    return sb_finish(&out);
}

// windows[19] Select Secondary: every `secondary` weapon by minRec; locked ones need that
// level (unlocked offline and in v7's sandbox).
static char *gen_secondary(void) {
    const AccountInfo *acc = account_get();
    StrBuf out = {0};
    sb_append(&out, "<div class='winBackH'><div class='custBack' onclick='playSelect(0.1);goBackWindow(44)' onmouseenter='playTick();'><span class='material-icons custBackArr'>arrow_back</span></div><div class='winSelH'>Select Secondary</div><div class='winClose' onclick='playSelect(0.1);closWind()' onmouseenter='playTick();'><span class='material-icons winCloseArr'>close</span></div></div>");
    sb_append(&out, "<div id='skinList'>");
    for (int rec = 0; rec <= 255; rec++) { // stable sort by minRec
        for (int i = 0; i < WEAPON_COUNT; i++) {
            const LoadoutWeapon *w = weapon_at(i);
            if (!w || !w->secondary || w->min_rec != rec) continue;
            const int unlocked = !w->min_rec || all_skins_unlocked() || (acc && acc->level >= w->min_rec);
            const int skin = has_inventory() ? weapon_skin(i) : -1;
            char img[256];
            if (skin >= 0 && store_skin(skin)) skin_thumb(skin, img, sizeof(img));
            else snprintf(img, sizeof(img), "textures/previews/weapons/%s.png", w->src ? w->src : "undefined");
            sb_append(&out, "<div class='skinCard blackShad' style='height: 165px;border:5px solid lightgrey'");
            if (unlocked) sb_appendf(&out, " onclick='skinSelector(%d, true)'", i);
            sb_append(&out, ">");
            sb_append_esc(&out, w->name);
            if (w->min_rec) sb_appendf(&out, "<div class='itemOwn'>%s%d</div>", unlocked ? "Unlocked" : "Req lvl ", unlocked ? 0 : w->min_rec);
            else sb_append(&out, "<div class='itemOwn'>Default</div>");
            sb_appendf(&out, "<img loading='lazy' draggable='false' class='secImg' onmouseenter='playTick()' src='%s' />", img);
            if (!unlocked) sb_append(&out, "<div class='lockedCard'><span class='material-icons' style='font-size: 70px;color:#fff;'>lock</span></div>");
            sb_append(&out, "</div>");
        }
    }
    sb_append(&out, "</div>");
    char *html = sb_finish(&out);
    // "Unlocked" carries no level: drop the %d placeholder written above
    StrBuf fix = {0};
    sb_append_replace_all(&fix, html, "Unlocked0", "Unlocked");
    free(html);
    return sb_finish(&fix);
}

static int class_primary_has_attach(void) {
    const LoadoutWeapon *w = weapon_at(class_at(menu_ui()->client->menu_class_index).primary);
    return w && w->attach;
}

// windows[30] Select Attachment: Reflex Sight and Acog Scope (no minRec in v7.1.4).
static char *gen_attach(void) {
    StrBuf out = {0};
    sb_append(&out, "<div id='skinList'><div class='noBtnCard blackShad' style='height:150px;border:5px solid lightgrey' onclick='selectAttachment(-1)'>None<div class='skinRandom blackShad' style='top: 60px;'>X</div></div>");
    if (class_primary_has_attach()) {
        for (int i = 0; i < ATTACH_COUNT; i++) {
            char img[256], path[64];
            snprintf(path, sizeof(path), "textures/previews/attach/attach_%d.png", i);
            asset(path, img, sizeof(img));
            sb_appendf(&out, "<div class='noBtnCard blackShad' style='height:150px;border:5px solid lightgrey' onclick='selectAttachment(%d)'>%s<div class='itemOwn'>Default</div><img loading='lazy' draggable='false' class='noBtnImgA' src='%s' /></div>",
                       i, ATTACHMENTS[i], img);
        }
    }
    sb_append(&out, "</div>");
    return sb_finish(&out);
}

// The settings rows above a picker's #skinList in its captured template (f1797 rows of
// reticleWidth / reticleHeight, the scope border rows): their values are applied after
// rendering (ui_window_post -> ui_settings_apply_values).
static void template_head(StrBuf *out, const char *key) {
    const char *tpl = ui_windows_html(key);
    const char *list = strstr(tpl, "<div id=\"skinList\"");
    if (list) sb_append_n(out, tpl, (size_t) (list - tpl));
}

// Custom images (customReticles / customScopes: [[name, url], ...] in localStorage).
static void custom_images(StrBuf *out, const char *storage_key, const char *select_fn, const char *remove_fn) {
    char buf[4096] = "[]";
    html_storage_get(storage_key, buf, sizeof(buf));
    cJSON *list = cJSON_Parse(buf);
    int i = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, list) {
        const char *name = cJSON_GetStringValue(cJSON_GetArrayItem(it, 0));
        const char *url = cJSON_GetStringValue(cJSON_GetArrayItem(it, 1));
        sb_appendf(out, "<div class='noBtnCard blackShad' style='height:140px;border:5px solid #fc5a4e' onclick='%s(-2, &quot;", select_fn);
        sb_append_esc(out, url ? url : "");
        sb_append(out, "&quot;)'>");
        sb_append_esc(out, name && name[0] ? name : "Custom");
        sb_append(out, "<div class='itemOwn' style='margin-top:1px'>Custom</div><img loading='lazy' class='noBtnImgR' draggable='false' src='");
        sb_append_esc(out, url ? url : "");
        sb_appendf(out, "' onmouseenter='playTick()' /><div class='selctDelBtn' onmouseenter='playTick()'onclick='%s(%d)'>X</div></div>", remove_fn, i++);
    }
    cJSON_Delete(list);
}

// windows[31] Select Reticle
static char *gen_reticle(void) {
    StrBuf out = {0};
    template_head(&out, "32");
    sb_append(&out, "<div id='skinList' style='margin-top:18px;'><div class='noBtnCard blackShad' style='height:140px;border:5px solid lightgrey' onclick='selectReticle(-1)' onmouseenter='playTick()'>None<div class='skinRandom blackShad' style='top: 60px;' onmouseenter='playTick()'>X</div></div>");
    if (class_primary_has_attach()) {
        sb_append(&out, "<div class='noBtnCard blackShad' style='height:140px;border:5px solid lightgrey'><div style='color: #fff;margin-top: 33px;' onclick='window.windows[31].newPop()' onmouseenter='playTick()'>ADD<br>CUSTOM</div></div>");
        custom_images(&out, "krk_custRets", "selectReticle", "removeReticle");
        for (int i = 0; i < RETICLE_COUNT; i++) {
            sb_appendf(&out, "<div class='noBtnCard blackShad' style='height:140px;border:5px solid lightgrey' onclick='selectReticle(%d)'>%s<div class='itemOwn' style='margin-top:1px'>Default</div><img loading='lazy' class='noBtnImgR' draggable='false' src='textures/reticles/reticle_%d.png' onmouseenter='playTick()' /></div>",
                       i, RETICLES[i], i);
        }
    }
    sb_append(&out, "</div>");
    return sb_finish(&out);
}

// windows[40] Select Scope (aimRecticles: the scope overlay image)
static char *gen_scope(void) {
    const LoadoutWeapon *w = weapon_at(class_at(menu_ui()->client->menu_class_index).primary);
    StrBuf out = {0};
    template_head(&out, "41");
    sb_append(&out, "<div id='skinList' style='margin-top:18px'><div class='noBtnCard blackShad' style='height:140px;border:5px solid lightgrey' onclick='selectScope(-1)' onmouseenter='playTick()'>None<div class='skinRandom blackShad' style='top: 60px;' onmouseenter='playTick()'>X</div></div>");
    if (w && w->scope) {
        sb_append(&out, "<div class='noBtnCard' style='height:140px;border:5px solid lightgrey'><div style='color: #fff;margin-top: 33px;' onclick='window.windows[40].newPop()' onmouseenter='playTick()'>ADD<br>CUSTOM</div></div>");
        custom_images(&out, "krk_custScps", "selectScope", "removeScope");
        for (int i = 0; i < SCOPE_COUNT; i++) {
            char img[256], path[64];
            snprintf(path, sizeof(path), "textures/scopes/scope_%d.png", SCOPES[i].id);
            asset(path, img, sizeof(img));
            sb_appendf(&out, "<div class='noBtnCard blackShad' style='height:140px;border:5px solid lightgrey' onclick='selectScope(%d)'>%s<div class='itemOwn' style='margin-top:1px'>Default</div><img loading='lazy' class='noBtnImgR' draggable='false' src='%s' onmouseenter='playTick()' /></div>",
                       i, SCOPES[i].name, img);
        }
    }
    sb_append(&out, "</div>");
    return sb_finish(&out);
}

// windows[44] Select Streaks: the streak list (by slot, then kills) and the three slots.
static char *gen_streaks(void) {
    const AccountInfo *acc = account_get();
    const int dev = acc && acc->is_dev;
    StrBuf out = {0};
    sb_append(&out, "<div class='winBackH'><div class='custBack' onclick='playSelect(0.1);goBackWindow(44)' onmouseenter='playTick();'><span class='material-icons custBackArr'>arrow_back</span></div><div class='winSelH'>Select Streaks</div><div class='winClose' onclick='playSelect(0.1);closWind()' onmouseenter='playTick();'><span class='material-icons winCloseArr'>close</span></div></div>");
    sb_append(&out, "<div id='custStreakListH'>");
    int order[STREAK_COUNT], n = 0;
    for (int slot = 0; slot < 3; slot++) {
        for (int kills = 0; kills <= 100; kills++) {
            for (int i = 0; i < STREAK_COUNT; i++) {
                if (STREAKS[i].slot == slot && STREAKS[i].kills == kills && (!STREAKS[i].dev || dev)) order[n++] = i;
            }
        }
    }
    for (int k = 0; k < n; k++) {
        const int i = order[k], slot = STREAKS[i].slot;
        const int free_slot = sel(SEL_STREAK1 + slot) == -1;
        sb_append(&out, "<div class='streakCard blackShad' style='height: 165px;border:5px solid lightgrey'");
        if (free_slot) sb_appendf(&out, " onclick='selectStreak(%d, %d);cancelTip()'", i, slot);
        sb_appendf(&out, " data-index='%d' onmouseover='createStreakTip(this)' onmouseout='cancelTip()' id='streakSel%d_%d'>%s<div class='itemOwn'>Slot %d | %d kills</div><img loading='lazy' draggable='false' class='streakImg' onmouseenter='playTick()' src='img/streaks/%d.png' />",
                   i, slot, i, STREAKS[i].name, slot + 1, STREAKS[i].kills, i);
        if (!free_slot) sb_append(&out, "<div class='lockedCard'><span class='material-icons' style='font-size: 70px;color:#fff'>lock</span></div>");
        sb_append(&out, "</div>");
    }
    sb_append(&out, "</div><div id='custStreakSelH'>");
    for (int slot = 0; slot < 3; slot++) {
        const int i = sel(SEL_STREAK1 + slot);
        const int has = i >= 0 && i < STREAK_COUNT;
        sb_appendf(&out, "<div class='streakCard blackShad' style='height: 165px;border:5px solid lightgrey' onclick='selectStreak(-1, %d)'><div class='streakActionOvrl'><div class='streakActionHol'><div class='streakActionB'><span class='material-icons' style='font-size:70px;color:#fff;'>close</span></div></div></div>%s<div class='itemOwn'>Slot %d</div>",
                   slot, has ? STREAKS[i].name : "None", slot + 1);
        if (has) sb_appendf(&out, "<img loading='lazy' draggable='false' class='streakImg' onmouseenter='playTick()' src='img/streaks/%d.png'/>", i);
        else sb_append(&out, "<div class='skinRandom blackShad' style='top:68px;left: 57px;'>?</div>");
        sb_append(&out, "</div>");
    }
    sb_append(&out, "</div>");
    return sb_finish(&out);
}

// windows[8] Select Spray: Random, then every spray the player has (free ones first: the
// default sprays; then the owned store sprays by rarity), with their frame animations.
static char *gen_sprays(void) {
    StrBuf cards = {0}, style = {0};
    int frames_seen[16], frames_n = 0;
    sb_append(&cards, ITEM_SEARCH_HEAD("2", ", &quot;noBtnCard&quot;") "<div id='skinList' style='overflow-y: scroll;height: calc(100% - 95px);'>");
    sb_append(&cards, "<div class='noBtnCard blackShad' onclick='selectSpray(-2)' style='height:158px;border: 5px solid lightgrey'>Random<div class='itemOwn' style='opacity:0;''>by \?\?\?</div><img draggable='false' style='margin-top:10px;width:110px;' class='noBtnImgC skinSil' src='textures/sprays/0.png' onmouseenter='playTick()'><div class='skinRandom blackShad' style='top: 80px;'>?</div></div>");
    for (int i = 0; i < SPRAYS_BASE_COUNT; i++) {
        const int frames = BASE_SPRAYS[i].id == 88 ? 4 : 0; // Verb: 4 frames x 250 ms
        if (frames && frames_n < 16) {
            sb_appendf(&style, "@keyframes sprayAnm%d {100%%{background-position:-%dpx}}", frames, frames * 110);
            frames_seen[frames_n++] = frames;
        }
        sb_appendf(&cards, "<div class='noBtnCard blackShad' style='height:158px;border: 5px solid lightgrey'onclick='selectSpray(%d);cancelTip()' data-index='%d' onmouseover='createTip(this)' onmouseout='cancelTip()'>%s<div class='itemOwn'>Free Spray</div><div draggable='false' class='sprayImg' onmouseenter='playTick()' style='background: url(&quot;textures/sprays/%d.png&quot;) -1px;%s'></div></div>",
                   i, i, BASE_SPRAYS[i].name, BASE_SPRAYS[i].id, frames ? "animation: sprayAnm4 1.25s steps(4) infinite;" : "");
    }
    int n;
    OwnedSkin *owned = owned_skins(&n, 1);
    for (int i = 0; i < n; i++) {
        const ViewerSkin *s = store_skin(owned[i].ind);
        if (!s || s->type != 4) continue;
        const char *col = rarity_color(s);
        char img[256], anim[96] = "";
        skin_thumb(owned[i].ind, img, sizeof(img));
        if (s->frames) { // UTILS.getFrameAnim(b7, "sprayAnm") and its @keyframes
            snprintf(anim, sizeof(anim), "animation: sprayAnm%d %gs steps(%d) infinite;", s->frames, 1 + s->frame_t / 1000.0, s->frames);
            int seen = 0;
            for (int k = 0; k < frames_n; k++) seen |= frames_seen[k] == s->frames;
            if (!seen && frames_n < 16) {
                sb_appendf(&style, "@keyframes sprayAnm%d {100%%{background-position:-%dpx}}", s->frames, s->frames * 110);
                frames_seen[frames_n++] = s->frames;
            }
        }
        sb_appendf(&cards, "<div class='noBtnCard%s%s' style='height:158px;color:%s;border: 5px solid %s'onclick='selectSpray(%d);cancelTip()' data-index='%d' onmouseover='createTip(this)' onmouseout='cancelTip()'><div class='sprayCnt'>x%d</div>",
                   s->rarity == 5 ? "" : " blackShad", rarity_anim(s) ? " rainbow" : "", col, col, owned[i].ind, owned[i].ind, owned[i].cnt);
        sb_append_esc(&cards, s->name ? s->name : "");
        sb_appendf(&cards, "<div class='itemOwn'>Spray</div><div draggable='false' class='sprayImg' onmouseenter='playTick()' style='background: url(&quot;%s&quot;) -1px;%s'></div></div>", img, anim);
    }
    free(owned);
    sb_append(&cards, "</div>");
    char *c = sb_finish(&cards), *st = sb_finish(&style);
    StrBuf out = {0};
    if (st[0]) sb_appendf(&out, "<style>%s</style>", st);
    sb_append(&out, c);
    free(c);
    free(st);
    return sb_finish(&out);
}

// ---------------------------------------------------------------- windows[34] Changelog

// Krunker splits docs/versions.txt with
//   /^\s*== (?=.*?\bUPDATE|UPADTE|INITIAL|TRICKSHOT\b)(.*) ==/gm
// into [prefix, title, body, title, body, ...]; every pair but the last is an update whose
// version is the title's first /[\d\.]+/, grouped by its first digit (the tabs v1.x .. v7.x).
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
    for (size_t k = tl; k >= 3; k--) { // (.*) == : greedy up to the last " ==" on the line
        if (!strncmp(t + k - 3, " ==", 3)) {
            *title = t;
            *title_len = k - 3;
            return 1;
        }
    }
    return 0;
}

typedef struct {
    char version[24];        // "7.1.4"
    const char *body;        // the update's text up to the next title
    size_t body_len;
} LogEntry;

#define GAME_VERSION "7.1.4" // config.gameVersion
static char *g_log_text;
static LogEntry *g_log;
static int g_log_count;
static int g_log_tab = -1;   // windows[34].tabIndex: the major version shown

static void changelog_load(void) {
    if (g_log_text) return;
#ifdef __EMSCRIPTEN__
    char *path = concat("/ui/windows/", "versions.txt");
#else
    const char *dir = getenv("KRUNKNATIVE_UI_DIR");
    char *path = dir && *dir ? concat(dir, "/windows/versions.txt") : concat(assets_path(), "../ui/windows/versions.txt");
#endif
    unsigned char *data = NULL;
    size_t length = 0;
    const int ok = read_file(path, &data, &length) == 0;
    free(path);
    if (!ok) return;
    g_log_text = (char *) data;
    typedef struct { const char *title; size_t title_len; const char *start, *end; } Hdr;
    Hdr *hdrs = NULL;
    size_t count = 0, cap = 0;
    const char *text = g_log_text, *tend = text + length;
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
    g_log = calloc(count ? count : 1, sizeof(LogEntry));
    for (size_t i = 0; g_log && i + 1 < count; i++) {
        const char *v = hdrs[i].title, *ve = v + hdrs[i].title_len;
        while (v < ve && !(isdigit((unsigned char) *v) || *v == '.')) v++;
        size_t k = 0;
        LogEntry *e = &g_log[g_log_count];
        while (v < ve && (isdigit((unsigned char) *v) || *v == '.') && k + 1 < sizeof(e->version)) e->version[k++] = *v++;
        e->version[k] = 0;
        if (!k) continue;
        e->body = hdrs[i].end;
        e->body_len = (size_t) (hdrs[i + 1].start - hdrs[i].end);
        if (isdigit((unsigned char) e->version[0]) && e->version[0] - '0' > g_log_tab) g_log_tab = e->version[0] - '0';
        g_log_count++;
    }
    free(hdrs);
}

// One trimmed line of an update body: [start, end)
static int next_line(const char **p, const char *end, const char **a, const char **b) {
    while (*p < end) {
        const char *nl = memchr(*p, '\n', (size_t) (end - *p));
        const char *le = nl ? nl : end;
        *a = *p;
        *b = le;
        *p = nl ? nl + 1 : end;
        while (*a < *b && isspace((unsigned char) **a)) (*a)++;
        while (*b > *a && isspace((unsigned char) (*b)[-1])) (*b)--;
        if (*b > *a) return 1;
    }
    return 0;
}

static char *gen_changelog(void) {
    changelog_load();
    StrBuf out = {0};
    if (!g_log_text) {
        sb_append(&out, "Loading...");
        return sb_finish(&out);
    }
    char seen[32] = "";
    html_storage_get("lastViewedUpdate", seen, sizeof(seen));
    if (strcmp(seen, GAME_VERSION)) { // "Whats New in v7.1.4 ?"
        sb_append(&out, "<div class='logVersionHed' style='background:url(img/backgrounds/whatsnew.png);'>Whats New in v" GAME_VERSION " ?</div>");
        int open = 0;
        for (int i = 0; i < g_log_count; i++) {
            if (strcmp(g_log[i].version, GAME_VERSION)) continue;
            const char *p = g_log[i].body, *end = p + g_log[i].body_len, *a, *b;
            while (next_line(&p, end, &a, &b)) {
                if (*a == '*') {
                    if (open) sb_append(&out, "</div>");
                    open = 1;
                    sb_append(&out, "<div class='mapListCat'>");
                    sb_append_n(&out, a + 1, (size_t) (b - a - 1));
                    sb_append(&out, "</div><div class='setBodH' style='padding-top:20px;padding-bottom:20px;'>");
                } else {
                    const char *t = a + 1;
                    if (t < b && *t == ' ') t++;
                    if (*a == '@') {
                        sb_append(&out, "<img class='logImg' src='");
                        sb_append_n(&out, t, (size_t) (b - t));
                        sb_append(&out, "'><div></div>");
                    } else {
                        sb_append(&out, "- <span style='color:rgba(255,255,255,0.6);'>");
                        sb_append_n(&out, t, (size_t) (b - t));
                        sb_append(&out, "</span><div></div>");
                    }
                }
            }
            break;
        }
        sb_append(&out, "</div><div class='viewAllUpdates' onclick='windows[34].viewAll()'>View All Updates</div>");
        return sb_finish(&out);
    }
    // every update of the selected major version, newest first; the latest one opened
    sb_append(&out, "<div style='margin-bottom:15px'><div style='height:20px;'></div><div class='bigMenTabs'>");
    for (int major = 9; major >= 0; major--) {
        int any = 0;
        for (int i = 0; i < g_log_count && !any; i++) any = g_log[i].version[0] - '0' == major;
        if (any) sb_appendf(&out, "<div class='bigMenTab%s' onclick='windows[34].switchTab(%d)'>v%d.x</div>", g_log_tab == major ? " bigMenTabA" : "", major, major);
    }
    sb_append(&out, "</div></div><div id='mailList'>");
    const char *latest = NULL;
    for (int i = 0; i < g_log_count && !latest; i++) if (g_log[i].version[0] - '0' == g_log_tab) latest = g_log[i].version;
    for (int i = 0; i < g_log_count; i++) {
        if (g_log[i].version[0] - '0' != g_log_tab) continue;
        const int is_latest = latest && !strcmp(latest, g_log[i].version) && &g_log[i].version[0] == latest;
        sb_appendf(&out, "<div class='mailObj' onclick='windows[34].toggleLog(this)'><i class='material-icons' style='vertical-align:middle;margin-top:-10px;font-size:50px;float:right;margin-right:10px;color:#fff'>%s</i><div class='mailFrom' style='color: #fbc02d;'>%s %s</div><div style='margin-top:10px;%s'>",
                   is_latest ? "arrow_drop_up" : "arrow_drop_down", is_latest ? "LATEST UPDATE" : "UPDATE", g_log[i].version, is_latest ? "" : "display:none;");
        // fixChanges: "*" and "@" lines dropped, "- " removed, one <li> per line
        sb_append(&out, "<li class='changeText' style='color: rgba(255, 255, 255, 0.5);'>");
        const char *p = g_log[i].body, *end = p + g_log[i].body_len, *a, *b;
        int first = 1;
        while (next_line(&p, end, &a, &b)) {
            if (*a == '*' || *a == '@') continue;
            if (!first) sb_append(&out, "</li><li class=\"changeText\" style=\"color: rgba(255, 255, 255, 0.5);\">");
            first = 0;
            const char *dash = b - a >= 2 ? strstr(a, "- ") : NULL;
            if (dash && dash < b) {
                sb_append_n(&out, a, (size_t) (dash - a));
                sb_append_n(&out, dash + 2, (size_t) (b - dash - 2));
            } else {
                sb_append_n(&out, a, (size_t) (b - a));
            }
        }
        sb_append(&out, "</li></div></div>");
    }
    sb_append(&out, "</div>");
    return sb_finish(&out);
}

// windows[34].toggleLog(obj): children[2] shown / hidden, children[0] the arrow.
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
    (void) header; // the captured windows.json header is used
    switch (id) {
        case 3: return ui_loadout_gen();
        case 9: return gen_sprays();
        case 15: return gen_skins();
        case 16: return gen_items_list("selectHat", 1);
        case 17: return gen_items_list("selectBody", 2);
        case 20: return gen_secondary();
        case 26: return gen_items_list("selectMelee", 3);
        case 31: return gen_attach();
        case 32: return gen_reticle();
        case 34: return gen_items_list("selectDye", 5);
        case 35: return gen_changelog();
        case 38: return gen_items_list("selectWaist", 6);
        case 39: return gen_items_list("selectFace", 7);
        case 40: return gen_items_list("selectShoe", 8);
        case 41: return gen_scope();
        case 43: return gen_items_list("selectPet", 9);
        case 44: return gen_items_list("selectWrist", 11);
        case 45: return gen_streaks();
        case 46: return gen_items_list("selectCharm", 12);
        case 50: return gen_items_list("selectBack", 14);
        case 51: return gen_items_list("selectHead", 15);
        case 52: return gen_items_list("selectPlayerCard", 16);
        default: return NULL;
    }
}

// ---------------------------------------------------------------- handlers

// updateAimDot(reticleIndex, savedReticle): #aimDot shows the picked reticle (or the custom one).
static void update_aim_dot(void) {
    HtmlElem dot = html_dom_by_id("aimDot");
    if (!dot) return;
    const int r = sel(SEL_RETICLE);
    char src[512] = "";
    if (r >= 0 && r < RETICLE_COUNT) snprintf(src, sizeof(src), "textures/reticles/reticle_%d.png", r);
    else if (r == -2) html_storage_get("savedReticle", src, sizeof(src));
    if (src[0]) html_dom_set_attr(dot, "src", src);
}

static const struct { const char *name; int which; } SELECT_MAP[] = {
    {"selectHat", SEL_HAT}, {"selectHead", SEL_HEAD}, {"selectBody", SEL_BODY}, {"selectBack", SEL_BACK},
    {"selectWaist", SEL_WAIST}, {"selectMelee", SEL_MELEE}, {"selectDye", SEL_DYE}, {"selectFace", SEL_FACE},
    {"selectShoe", SEL_SHOE}, {"selectPet", SEL_PET}, {"selectWrist", SEL_WRIST}, {"selectPlayerCard", SEL_CARD},
    {"selectSpray", SEL_SPRAY}, {"selectAttachment", SEL_ATTACH}, {"selectReticle", SEL_RETICLE}, {"selectScope", SEL_SCOPE},
};

// selectHat(i, noShow, update) & co.: saveVal, updateClassPreview, showWindow(3) unless noShow,
// updateWindow(3) with update.
static int select_item(const char *what, const int value, const int no_show, const int update) {
    for (size_t i = 0; i < sizeof(SELECT_MAP) / sizeof(SELECT_MAP[0]); i++) {
        if (strcmp(SELECT_MAP[i].name, what)) continue;
        sel_save(SELECT_MAP[i].which, value);
        if (SELECT_MAP[i].which == SEL_RETICLE) update_aim_dot();
        refresh_class_box();
        if (!no_show) menu_show_window(3, 0);
        if (update) menu_rerender_if_open(3);
        return 1;
    }
    return 0;
}

void ui_loadout_select(const char *what, const int value) {
    select_item(what, value, 0, 0);
}

// selectSkinColor(i) / selectStat(v): saveVal, updateClassPreview, updateWindow(3).
void ui_loadout_select_skin_color(const int index) {
    sel_save(SEL_SKIN_COL, index);
    refresh_class_box();
    menu_rerender_if_open(3);
}

void ui_loadout_select_stat(const int index) {
    sel_save(SEL_PC_STAT, index);
    refresh_class_box();
    menu_rerender_if_open(3);
}

// skinSelector(weapon, isSecondary): activeSkinSel (and the secondary), open Select Skin.
void ui_loadout_skin_selector(const int weapon, const int secondary) {
    g_skin_weapon = weapon;
    if (secondary) sel_save(SEL_SECONDARY, weapon);
    refresh_class_box();
    menu_show_window(15, 0);
}

// selectSkin(index, weapon): a store skin index or -2 (Random) is remembered, anything else
// (v7's None is -3) clears it.
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
    // skinsLoadout[weapon] = store.skins[index] || index == -2 ? index : undefined
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
static const struct { const char *field; int which; } PRESET_FIELDS[] = {
    {"skinColIndex", SEL_SKIN_COL}, {"hairCol", SEL_HAIR_COL}, {"chatCol", SEL_CHAT_COL},
    {"dyeIndex", SEL_DYE}, {"faceIndex", SEL_FACE}, {"shoeIndex", SEL_SHOE}, {"attachIndex", SEL_ATTACH},
    {"reticleIndex", SEL_RETICLE}, {"scopeIndex", SEL_SCOPE}, {"meleeIndex", SEL_MELEE}, {"bodyIndex", SEL_BODY},
    {"backIndex", SEL_BACK}, {"petIndex", SEL_PET}, {"wristIndex", SEL_WRIST}, {"hatIndex", SEL_HAT},
    {"headIndex", SEL_HEAD}, {"playerCardIndex", SEL_CARD}, {"waistIndex", SEL_WAIST},
    {"secondaryInd", SEL_SECONDARY}, {"pcStatIndex", SEL_PC_STAT}, {"streakIndex1", SEL_STREAK1},
    {"streakIndex2", SEL_STREAK2}, {"streakIndex3", SEL_STREAK3},
};

// .replace(/[^a-zA-Z0-9 _-]+/gim, "")
static void preset_name(char *s) {
    char *w = s;
    for (const char *r = s; *r; r++) if (isalnum((unsigned char) *r) || *r == ' ' || *r == '_' || *r == '-') *w++ = *r;
    *w = 0;
}

void ui_loadout_action(const int action) {
    char selected[64] = "Select", name[64] = "";
    HtmlElem sel_el = html_dom_by_id("loadoutSelect"), name_el = html_dom_by_id("loadoutName");
    if (sel_el) html_dom_get_value(sel_el, selected, sizeof(selected));
    if (name_el) html_dom_get_value(name_el, name, sizeof(name));
    preset_name(selected);
    const int nothing = !selected[0] || !strcmp(selected, "Select");
    if ((action == 0 || action == 2) && nothing) return;

    char buf[8192] = "{}";
    html_storage_get("krk_loadouts", buf, sizeof(buf));
    cJSON *all = cJSON_Parse(buf);
    if (!all || !cJSON_IsObject(all)) {
        cJSON_Delete(all);
        all = cJSON_CreateObject();
    }
    const char *target = name[0] ? name : nothing ? "Preset1" : selected;
    if (action == 2) {
        cJSON_DeleteItemFromObject(all, selected);
    } else if (action == 1) {
        cJSON *preset = cJSON_CreateObject();
        for (size_t i = 0; i < sizeof(PRESET_FIELDS) / sizeof(PRESET_FIELDS[0]); i++) cJSON_AddNumberToObject(preset, PRESET_FIELDS[i].field, sel(PRESET_FIELDS[i].which));
        cJSON_AddNumberToObject(preset, "classindex", menu_ui()->client->menu_class_index);
        char skins[1024] = "{}", charms[256] = "[]", saved[512] = "";
        html_storage_get("skins", skins, sizeof(skins));
        cJSON_AddStringToObject(preset, "skins", skins);
        html_storage_get("charms", charms, sizeof(charms));
        cJSON_AddStringToObject(preset, "charms", charms);
        if (html_storage_get("savedReticle", saved, sizeof(saved)) >= 0) cJSON_AddStringToObject(preset, "savedReticle", saved);
        saved[0] = 0;
        if (html_storage_get("savedScope", saved, sizeof(saved)) >= 0) cJSON_AddStringToObject(preset, "savedScope", saved);
        cJSON_DeleteItemFromObject(all, target);
        cJSON_AddItemToObject(all, target, preset);
    } else {
        const cJSON *preset = cJSON_GetObjectItem(all, selected);
        if (cJSON_IsObject(preset)) {
            for (size_t i = 0; i < sizeof(PRESET_FIELDS) / sizeof(PRESET_FIELDS[0]); i++) {
                const cJSON *v = cJSON_GetObjectItem(preset, PRESET_FIELDS[i].field);
                if (cJSON_IsNumber(v)) sel_save(PRESET_FIELDS[i].which, v->valueint);
            }
            for (int k = 0; k < 3; k++) sel_save(SEL_STREAK1 + k, streak_valid(k, sel(SEL_STREAK1 + k)));
            // v3.1.1 presets: kcStatIndex / backIndex (the body slot then)
            const cJSON *kc = cJSON_GetObjectItem(preset, "kcStatIndex");
            if (cJSON_IsNumber(kc) && !cJSON_GetObjectItem(preset, "pcStatIndex")) sel_save(SEL_PC_STAT, kc->valueint);
            const cJSON *ci = cJSON_GetObjectItem(preset, "classindex");
            if (cJSON_IsNumber(ci)) menu_set_class_index(ci->valueint, 0);
            const cJSON *skins = cJSON_GetObjectItem(preset, "skins");
            if (cJSON_IsString(skins)) skins_store(cJSON_GetStringValue(skins));
            const cJSON *charms = cJSON_GetObjectItem(preset, "charms");
            if (cJSON_IsString(charms)) html_storage_set("charms", cJSON_GetStringValue(charms));
            const cJSON *ret = cJSON_GetObjectItem(preset, "savedReticle");
            if (cJSON_IsString(ret)) html_storage_set("savedReticle", cJSON_GetStringValue(ret));
            const cJSON *scp = cJSON_GetObjectItem(preset, "savedScope");
            if (cJSON_IsString(scp)) html_storage_set("savedScope", cJSON_GetStringValue(scp));
            update_aim_dot();
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

// windows[2].switchTab(tab, force, sub)
void ui_loadout_switch_tab(const int tab) {
    g_tab = tab >= 0 && tab <= 3 ? tab : 1;
    menu_show_window(3, 1);
}

// The handlers that stop the event (f1822(window.event) in v7: charmSelector,
// viewAttachments, windows[n].clear, remove*) sit inside a .custContainer whose own onclick
// opens a picker: they act at once and stop the event (html_ui_stop_propagation), so the
// container's handler never runs.
typedef enum { DEFER_SHOW, DEFER_CLEAR } DeferKind;
static struct { DeferKind kind; int a, b; } g_defer;

static void deferred(void *user) {
    (void) user;
    if (g_defer.kind == DEFER_SHOW) {
        menu_show_window(g_defer.a, 1);
    } else {
        // windows[n].clear(): select*(-1, true, true)
        const int win = g_defer.a + 1;
        for (int i = 0; i < CUST_SLOT_COUNT; i++) {
            if (CUST_SLOTS[i].window != win) continue;
            sel_save(CUST_SLOTS[i].which, -1);
            refresh_class_box();
        }
        if (win == 46) charm_save(g_charm_slot, -1);
        menu_show_window(3, 1);
    }
}

static void defer(const DeferKind kind, const int a) {
    g_defer.kind = kind;
    g_defer.a = a;
    html_ui_stop_propagation();
    deferred(NULL);
}

// selectStreak(index, slot): one streak per slot (-1 empties it); re-renders the window.
static void select_streak(int index, const int slot) {
    if (slot < 0 || slot > 2) return;
    if (index < 0 || index >= STREAK_COUNT || STREAKS[index].slot != slot) index = -1;
    sel_save(SEL_STREAK1 + slot, index);
    refresh_class_box();
    menu_rerender_if_open(45);
}

// selectCharm(index): selectedCharms[slot] = item || -2 ? index : -1 (one charm per item unless
// owned twice: v7's "Charm already in use" popup becomes a plain alert).
static void select_charm(const int index) {
    const int value = store_skin(index) || index == -2 ? index : -1;
    if (value >= 0 && charm_at(g_charm_slot ? 0 : 1) == value) {
        int n, count = 0;
        OwnedSkin *owned = owned_skins(&n, 0);
        for (int i = 0; i < n; i++) if (owned[i].ind == value) count = owned[i].cnt;
        free(owned);
        if (count < 2 && !all_skins_unlocked()) {
            ui_alert("Charm already in use");
            return;
        }
    }
    charm_save(g_charm_slot, value);
    menu_show_window(3, 0);
}

// windows[31] / windows[40].newPop(): the "New Image" popup (#genericPop), addNew() reads it.
static int g_new_image_window;
static void new_image_pop(const int window) {
    g_new_image_window = window;
    HtmlElem pop = html_dom_by_id("genericPop"), holder = html_dom_by_id("popupHolder");
    if (!pop || !holder) return;
    char html[1024];
    snprintf(html, sizeof(html), "<div class='pubHed'>New Image</div><div style='font-size:20px;color:#fff'><div style='margin-top: 3px;font-size: 22px;color: rgba(255, 255, 255, 0.7);'>Name:<input type='text' id='newCustomName' class='inputGrey2' placeholder='Name'></div><div style='margin-top: 3px;font-size: 22px;color: rgba(255, 255, 255, 0.7);'>Image:<input type='url' id='newCustomImg' class='inputGrey2' placeholder='Image URL'></div><div class='mapFndB' onclick='window.windows[%d].addNew()' onmouseover='playTick(0.1)' style='background-color:#fc5a4e;width: 380px;margin-top: 10px;'>Add</div></div>",
             window);
    ui_loadout_clear_pops();
    html_dom_set_display(holder, "block");
    html_dom_set_attr(pop, "class", "imageNewPop"); // genericPopup(html, "imageNewPop")
    html_dom_set_attr(pop, "style", "");
    html_dom_set_html(pop, html);
    html_dom_set_display(pop, "block");
}

// sanitizeCustomImageUrl: http(s) URLs only, no quotes / angle brackets.
static int custom_url_ok(const char *url) {
    if (strncmp(url, "https://", 8) && strncmp(url, "http://", 7)) return 0;
    return !strpbrk(url, "\"'<>` ");
}

static void new_image_add(const int window) {
    char name[64] = "", url[512] = "";
    HtmlElem n = html_dom_by_id("newCustomName"), u = html_dom_by_id("newCustomImg");
    if (n) html_dom_get_value(n, name, sizeof(name));
    if (u) html_dom_get_value(u, url, sizeof(url));
    preset_name(name);
    if (!name[0] || !url[0]) { ui_alert(!url[0] && !name[0] ? "Something Went Wrong" : "Invalid Name"); return; }
    if (!custom_url_ok(url)) { ui_alert("Invalid Image"); return; }
    const char *key = window == 40 ? "krk_custScps" : "krk_custRets";
    char buf[4096] = "[]";
    html_storage_get(key, buf, sizeof(buf));
    cJSON *list = cJSON_Parse(buf);
    if (!cJSON_IsArray(list)) { cJSON_Delete(list); list = cJSON_CreateArray(); }
    cJSON *pair = cJSON_CreateArray();
    cJSON_AddItemToArray(pair, cJSON_CreateString(name));
    cJSON_AddItemToArray(pair, cJSON_CreateString(url));
    cJSON_AddItemToArray(list, pair);
    char *text = cJSON_PrintUnformatted(list);
    if (text) html_storage_set(key, text);
    free(text);
    cJSON_Delete(list);
    ui_loadout_clear_pops();
    menu_rerender_if_open(window + 1);
}

static void remove_custom_image(const char *key, const int index, const int window) {
    char buf[4096] = "[]";
    html_storage_get(key, buf, sizeof(buf));
    cJSON *list = cJSON_Parse(buf);
    if (cJSON_IsArray(list) && index >= 0 && index < cJSON_GetArraySize(list)) {
        cJSON_DeleteItemFromArray(list, index);
        char *text = cJSON_PrintUnformatted(list);
        if (text) html_storage_set(key, text);
        free(text);
    }
    cJSON_Delete(list);
    defer(DEFER_SHOW, window); // removeReticle stops the event: the card's selectReticle must not win
}

// selectReticle(-2, url) / selectScope(-2, url): the custom image is remembered too.
static void select_custom(const int which, const char *storage, const int value, const char *url) {
    sel_save(which, value);
    html_storage_set(storage, value == -2 && url && custom_url_ok(url) ? url : "");
    if (which == SEL_RETICLE) update_aim_dot();
    refresh_class_box();
    menu_show_window(3, 0);
}

static int harg_int(const HtmlArg *args, const int argc, const int i, const int fallback) {
    if (i >= argc) return fallback;
    if (args[i].kind == HTML_ARG_NUM || args[i].kind == HTML_ARG_BOOL) return (int) args[i].num;
    if (args[i].kind == HTML_ARG_STR && args[i].str) {
        char *end;
        const long v = strtol(args[i].str, &end, 10);
        return end != args[i].str ? (int) v : fallback;
    }
    return fallback;
}

static const char *harg_str(const HtmlArg *args, const int argc, const int i) {
    return i < argc && args[i].kind == HTML_ARG_STR && args[i].str ? args[i].str : "";
}

// searchItems(input, cardClass): hide the cards of #skinList whose text does not contain the query.
static void search_items(const HtmlEvent *ev, const char *card_class) {
    char q[128] = "";
    HtmlElem input = html_dom_by_id("itemSearch");
    if (input) html_dom_get_value(input, q, sizeof(q));
    else if (ev && ev->value) snprintf(q, sizeof(q), "%s", ev->value);
    for (char *c = q; *c; c++) *c = (char) tolower((unsigned char) *c);
    char sel_q[64];
    snprintf(sel_q, sizeof(sel_q), "#skinList .%s", card_class && card_class[0] ? card_class : "skinCard");
    HtmlElem cards[2048];
    const int n = html_dom_query_all(0, sel_q, cards, 2048);
    for (int i = 0; i < n; i++) {
        char text[256] = "";
        html_dom_get_text(cards[i], text, sizeof(text));
        for (char *c = text; *c; c++) *c = (char) tolower((unsigned char) *c);
        html_dom_set_display(cards[i], !q[0] || strstr(text, q) ? "" : "none");
    }
}

// Inline handlers this module owns (ui_handlers.c h_fallback); -1 = not ours.
int ui_loadout_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    if (!name) return -1;
    if (!strcmp(name, "windows.2.switchTab")) {
        ui_loadout_switch_tab(harg_int(args, argc, 0, 1));
        return 1;
    }
    if (!strcmp(name, "goBackWindow")) { // windows[n].goBack(): 2 -> Change Loadout, 19 -> Select Secondary, 44 -> Change Loadout
        const int w = harg_int(args, argc, 0, 2);
        menu_show_window(w == 19 ? 20 : 3, 0);
        return 1;
    }
    if (!strcmp(name, "selectReticle") && argc > 1) {
        select_custom(SEL_RETICLE, "savedReticle", harg_int(args, argc, 0, 0), harg_str(args, argc, 1));
        return 1;
    }
    if (!strcmp(name, "selectScope")) {
        select_custom(SEL_SCOPE, "savedScope", harg_int(args, argc, 0, 0), harg_str(args, argc, 1));
        return 1;
    }
    if (!strncmp(name, "select", 6)) {
        const int value = harg_int(args, argc, 0, -1);
        if (select_item(name, value, harg_int(args, argc, 1, 0), harg_int(args, argc, 2, 0))) return 1;
        if (!strcmp(name, "selectStreak")) { select_streak(value, harg_int(args, argc, 1, 0)); return 1; }
        if (!strcmp(name, "selectCharm")) { select_charm(value); return 1; }
        if (!strcmp(name, "selectHairColor") || !strcmp(name, "selectChatColor")) { // premium hex colours
            html_storage_set(!strcmp(name, "selectHairColor") ? "hairCol" : "chatCol", harg_str(args, argc, 0));
            refresh_class_box();
            menu_rerender_if_open(3);
            return 1;
        }
        return -1;
    }
    if (!strcmp(name, "charmSelector")) { // f1822 + vLN01715 = slot; showWindow(46)
        g_charm_slot = harg_int(args, argc, 0, 0) ? 1 : 0;
        defer(DEFER_SHOW, 46);
        return 1;
    }
    if (!strcmp(name, "viewAttachments")) { defer(DEFER_SHOW, 31); return 1; }
    if (!strcmp(name, "removeReticle")) { remove_custom_image("krk_custRets", harg_int(args, argc, 0, -1), 32); return 1; }
    if (!strcmp(name, "removeScope")) { remove_custom_image("krk_custScps", harg_int(args, argc, 0, -1), 41); return 1; }
    if (!strcmp(name, "windows.31.newPop") || !strcmp(name, "windows.40.newPop")) { new_image_pop(name[8] == '3' ? 31 : 40); return 1; }
    if (!strcmp(name, "windows.31.addNew") || !strcmp(name, "windows.40.addNew")) { new_image_add(name[8] == '3' ? 31 : 40); return 1; }
    if (!strcmp(name, "searchItems")) { search_items(ev, harg_str(args, argc, 1)); return 1; }
    if (!strcmp(name, "windows.34.switchTab")) {
        g_log_tab = harg_int(args, argc, 0, g_log_tab);
        menu_rerender_if_open(35);
        return 1;
    }
    if (!strcmp(name, "windows.34.viewAll")) { // lastViewedUpdate = config.gameVersion; showWindow(35, true)
        html_storage_set("lastViewedUpdate", GAME_VERSION);
        menu_show_window(35, 1);
        return 1;
    }
    // windows[n].clear(): the Customize tab's custClear "x" (f1822 + select*(-1, true, true))
    if (!strncmp(name, "windows.", 8)) {
        const char *dot = strchr(name + 8, '.');
        if (dot && !strcmp(dot, ".clear")) {
            defer(DEFER_CLEAR, atoi(name + 8));
            return 1;
        }
    }
    // item tooltips, favourites and the item view are store features KrunkNative leaves out
    if (!strcmp(name, "createTip") || !strcmp(name, "cancelTip") || !strcmp(name, "createStreakTip") || !strcmp(name, "favItem")) return 1;
    return -1;
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

// resetPops() (v7.1.4)
static void reset_pops(void) {
    static const char *pops[] = {"itemViewPop", "bundlePop", "premiumPop", "spinHistoryPop", "genericPop"};
    HtmlElem holder = html_dom_by_id("popupHolder");
    if (holder) html_dom_set_style(holder, "z-index", "2147483645");
    set_disp("popupHolder", "block");
    set_disp("popupContent", "none");
    HtmlElem content = html_dom_by_id("popupContent");
    if (content) html_dom_set_style(content, "background-color", "#353535");
    for (size_t i = 0; i < sizeof(pops) / sizeof(pops[0]); i++) set_disp(pops[i], "none");
    HtmlElem generic = html_dom_by_id("genericPop");
    if (generic) {
        html_dom_set_attr(generic, "class", "");
        html_dom_set_html(generic, "");
    }
    ui_loadout_hide_police_pop();
}

// clearPops()
void ui_loadout_clear_pops(void) {
    ui_dialog_cancel(); // a pending confirm/prompt (ui_dialog.c) counts as "Cancel"
    reset_pops();
    HtmlElem holder = html_dom_by_id("popupHolder");
    if (holder) html_dom_set_style(holder, "z-index", "2147483647");
    set_disp("popupHolder", "none");
    menu_schedule_rect();
}

// shoPolicePop() (v7.1.4): Officer Buddy in the class preview (classN "Officer"), the speech
// line for a guest (1) or an account (0: "You look Strong..."), and the K.P.D text; an account
// sees its Helpful Reports bar (jobRatingPos / jobRating, none tracked: 0/0) and the
// application line (20 positive reports needed).
void ui_loadout_show_police_pop(void) {
    reset_pops();
    HtmlElem holder = html_dom_by_id("popupHolder");
    if (holder) html_dom_set_style(holder, "z-index", "20");
    set_disp("policePop", "block");
    MenuUi *ui = menu_ui();
    g_police_saved = ui->client->menu_class_index;
    g_police_class = POLICE_CLASS;
    ui->client->menu_class_index = POLICE_CLASS; // dI.class, not saved
    refresh_class_box();
    audio_play("ambient_9", 0.14f, 1, 1.0f);
    if (g_police_timer) html_timer_cancel(g_police_timer);
    g_police_timer = html_timer_add(rand_between(15000, 30000), police_idle, NULL);
    const AccountInfo *acc = account_get();
    police_say(acc ? "You look Strong. Why don't you join us?" : "Sign In to Join the Fight against the Hackers!");
    HtmlElem c = html_dom_by_id("policePopC");
    if (c) {
        StrBuf sb = {0};
        sb_append(&sb, "<div style='font-size:30px;margin-bottom:12px;color:#858585'>Krunker Police Department</div>"
                       "<div style='color:rgba(0,0,0,0.6);font-size: 23px;'>The <b>K.P.D</b> allows you to <b>Combat &amp; Report Hackers</b> in an easier way. <div style='height:15px'></div>If you suspect that there is a Hacker in your Current Lobby you can <b>Call for Aid by pressing the K.P.D Icon on the player list</b>. <div style='height:15px'></div>Any Active <b>Moderators</b> will then be able to respond to your call from a list of Active Distress calls &amp; Deal with the Hacker accordingly.</div><div style='height:15px'></div>");
        if (acc) sb_append(&sb, "<div id='trustProg'><div id='trustProgHD'>0/0 Helpful Reports</div><div id='trustPrgHD' style='width:100%;background-color:#63de26'></div></div>");
        sb_appendf(&sb, "<div style='margin-top:20px;margin-bottom:20px;font-size:25px;color: #858585;'>%s</div>", acc ? "20 positive reports are required to apply." : "Sign-in to apply for K.P.D");
        char *html = sb_finish(&sb);
        html_dom_set_html(c, html);
        free(html);
    }
    menu_schedule_rect();
}
