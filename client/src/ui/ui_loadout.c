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
//   (windows[34] Changelog / What's New: client/src/ui/v10_news.c)
// Items come from KrunkNative's store table (client/src/viewer/viewer_store.h, the v3.1.1
// skins until the v7 table lands): a guest owns none, a kn_brain account its acc.skins and
// offline mode every skin. v7's new slots (head, back, shoes, pet, wrist, charm, player card)
// have no items in that table yet, so their pickers show Krunker's None / Random cards.
#include <ui_logic.h>
#include <ui_vgrid.h> // the pickers' card lists are virtual grids
#include <ui_tooltip.h> // createTip / createStreakTip / createSpinHistoryTip / cancelTip
#include <audio.h>
#include <backend_ui.h>
#include <online_mode.h> // offline mode: every skin unlocked
#include <app_mode.h> // app_route_url_target (openSales)
#include <render_settings.h>
#include <skin_paint.h> // the owned card's paint badge (Krunker v10.0.3's skinPaintBadge)
#include <paint_picker.h> // the owned card's paint button (paint_picker.c)
#include "../viewer/viewer_store.h"
#include <sprays.h> // SPRAYS_BASE_COUNT, social_v7.h (store spray indices)
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ruleset.h>
#include <cosmetics.h> // cosmetics_route, cosmetics_attach_index (ui_loadout_look)
#include <krunker_net.h> // the K.P.D's account in an official krunker.io game (police_identity)
#include <game_config.h> // game_class_allowed: KrunkNative's own classes are out of official krunker.io games
#include <krunker_account.h> // krunker_account_level: the level an official krunker.io game checks (game_class_level_ok)
#include <strike_econ.h> // Krunker Strike: the look is Strike's loadout (strike_econ_apply_look)

// ---------------------------------------------------------------- Krunker tables

// Weapons: rules_weapons() (name, preview src, secondary) is Krunker's weaponConfig in id order.
// The rest is UI-only weaponConfig data of v7.1.4 (src/data/weapons.js): `attach != null`
// (v7 has no forceAttach), `scope`, `charm` and the secondaries' `minRec` unlock level.
typedef struct {
    int id;
    const char *name;
    const char *src;   // textures/previews/weapons/<src>.png; NULL = undefined (the knife)
    int attach, scope, charm, secondary, min_rec;
} LoadoutWeapon;

// KrunkNative Nettspend (doc/v7/decisions.md "Nettspend class"): the two KN weapons a class loadout
// holds (Purple Drank, Money Spread). Every other KN weapon (Tool Gun, Piss, Physics Gun) stays out
// of the loadout menus; these two need the Loadout tab's tiles, previews and the class preview.
static int kn_loadout_weapon(const int id) {
    return id == WEAPON_ID_DRANK || id == WEAPON_ID_MONEY_SPREAD;
}

// The secondary a class always carries (ClassConfig.kn_secondary: the Nettspend's Money Spread, no
// Select Secondary for it), -1 = the player's pick (Krunker's classes). Mirrors player_spawn.
static int class_bound_secondary(const int class_index) {
    const Client *client = menu_ui()->client;
    const size_t count = client->game.class_count ? client->game.class_count : rules_class_count();
    const ClassConfig *classes = client->game.classes ? client->game.classes : rules_classes();
    const int bound = class_index >= 0 && (size_t) class_index < count ? classes[class_index].kn_secondary - 1 : -1;
    return bound >= 0 && rules_weapon(bound) ? bound : -1;
}

#define V7_WEAPON_COUNT 29
static const unsigned char WEAPON_ATTACH[V7_WEAPON_COUNT] = {[1] = 1, [3] = 1, [6] = 1, [7] = 1, [13] = 1, [14] = 1, [18] = 1};
static const unsigned char WEAPON_SCOPE[V7_WEAPON_COUNT] = {[0] = 1, [28] = 1};
static const unsigned char WEAPON_NO_CHARM[V7_WEAPON_COUNT] = {[12] = 1, [17] = 1, [19] = 1, [23] = 1, [25] = 1, [26] = 1};
static const unsigned char WEAPON_MIN_REC[V7_WEAPON_COUNT] = {[10] = 15, [11] = 50, [15] = 20, [16] = 30, [21] = 40, [22] = 25, [24] = 35, [27] = 10};

static const LoadoutWeapon *weapon_at(const int id) {
    static LoadoutWeapon w;
    if (id < 0 || (id >= rules_krunker_weapon_count() && !kn_loadout_weapon(id)) || !rules_weapon(id)) return NULL;
    w.id = id;
    w.name = rules_weapon(id)->name ? rules_weapon(id)->name : "";
    w.src = rules_weapon(id)->src;
    w.secondary = rules_weapon(id)->secondary;
    w.attach = id < V7_WEAPON_COUNT && WEAPON_ATTACH[id];
    w.scope = id < V7_WEAPON_COUNT && WEAPON_SCOPE[id];
    w.charm = kn_loadout_weapon(id) ? 0 : id >= V7_WEAPON_COUNT || !WEAPON_NO_CHARM[id]; // KN weapons: no charm
    w.min_rec = id < V7_WEAPON_COUNT ? WEAPON_MIN_REC[id] : rules_weapon(id)->min_rec; // v10: the Compressor 45
    return &w;
}

// The class's primary weapon id and `secondary` flag (Game.classes, like the class window).
typedef struct {
    int primary;
    int secondary;
} LoadoutClass;

// v10 classes req (_34270): a dev, any class in a custom (non-official) game, else minRec <=
// the account's level; a guest has level 0. KrunkNative's offline sandbox counts as v10's
// isSandbox (every class), as before.
static int class_min_rec(const int index) {
    const Client *client = menu_ui()->client;
    const size_t count = client->game.class_count ? client->game.class_count : rules_class_count();
    const ClassConfig *classes = client->game.classes ? client->game.classes : rules_classes();
    return index >= 0 && (size_t) index < count ? classes[index].min_rec : 0;
}

// The class's level req (game_class_level_ok, v10 _34270): only an official krunker.io game has one, and
// the level is the krunker.io account's (the official servers see that account, krunker_account.h; a
// guest is 0). Box Office lobbies, the offline sandbox and offline mode open every class. A dev stays
// exempt on the KrunkNative account's flag, as before.
int ui_loadout_class_open(const Client *client, const int index) {
    const AccountInfo *acc = active_account();
    return game_class_level_ok(&client->game, index, krunker_account_level(), acc && acc->is_dev);
}

static int class_unlocked(const int index) { return ui_loadout_class_open(menu_ui()->client, index); }

static LoadoutClass class_at(const int index) {
    const Client *client = menu_ui()->client;
    const size_t count = client->game.class_count ? client->game.class_count : rules_class_count();
    const ClassConfig *classes = client->game.classes ? client->game.classes : rules_classes();
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

// config.skinColors (col as hex, Krunker prints col.toString(16)): Bronze .. Pale, Infected, and
// v10.0.2's Yellowish and Darker Color (v10 _7524, every one req: account).
static const char *SKIN_COLORS[] = {"805c4a", "a77860", "d38d6f", "cc997e", "edbfa6", "819481", "e3a857", "260701"};
#define SKIN_COLOR_COUNT 8

// data/reticles.js (id == index)
static const char *RETICLES[] = {"Red Dot", "Green Dot", "Pyramid", "Tau", "Ray", "Spread",
                                 "Marine", "Underline", "Precision", "Quantum", "Triad", "Nuclear"};
#define RETICLE_COUNT ((int) (sizeof(RETICLES) / sizeof(RETICLES[0])))

// data/aimRecticles.js (scope overlays): name, id (textures/scopes/scope_<id>.png)
static const struct { const char *name; int id; } SCOPES[] = {
    {"Default", 0}, {"Pro", 1}, {"OG", 2}, {"Waspy", 4}, {"MoonJuice", 5}, {"Swixie", 6}, {"Sorable", 7},
};
#define SCOPE_COUNT ((int) (sizeof(SCOPES) / sizeof(SCOPES[0])))

// data/attach.js (Game.attach, id == index, already in minRec order): name and minRec. v10 gives
// the Acog Scope `minRec: 20` with a req on the CLASS level (stats.c<class>), devs exempt
// (v10.js:178106 "Acog Scope"); the Reflex Sight has none.
static const struct { const char *name; int min_rec; } ATTACHMENTS[] = {{"Reflex Sight", 0}, {"Acog Scope", 20}};
#define ATTACH_COUNT 2

// data/streaks.js (Game.streaks, index order): name, slot, kills, devOnly (v10.0.3, deob-10.0.3.js:203338-203520:
// the Refill is a dev's only, the Bombardment everyone's; shared/src/effects.c g_streaks)
static const struct { const char *name; int slot, kills, dev; } STREAKS[] = {
    {"Nuke", 2, 25, 0}, {"Airdrop", 1, 12, 0}, {"Refill", 0, 5, 1}, {"Bombardment", 2, 20, 0},
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
    [SEL_STREAK3] = {"streakIndex3", 0, 0}, // v10.js:210268 _70281(2, streakIndex3, 0): a new loadout has the Nuke
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

static int random_owned_skin(int weapon, int type);
static int weapon_skin(int weapon);
static int has_inventory(void);

// A store item pick as worn: -1 none (also v7's explicit None, -3); Random (-2) is getRandomOwnedSkin
// (`weapon` 1-based for a weapon skin, else store `type`) when resolve_random (the ENT), none for the
// preview (`== -2 ? -1`). Nothing without an inventory, as the tabs show it.
static int32_t worn(const int pick, const int resolve_random, const int weapon, const int type) {
    if (!has_inventory()) return -1;
    if (pick == -2) return resolve_random ? random_owned_skin(weapon, type) : -1;
    return pick >= 0 ? pick : -1;
}

// skin colour and hat / body / waist / face / dye (enterGame ent[3]/[4]/[14]/[17]/[21], dJ's
// hatIndex & co). hairCol stays the class colour (-1): Krunker only takes a hex string there, which the
// picker never sets. PlayerCosmetics.body is the type-2 body item (v7 Body, bodyIndex).
static void look_cosmetics(PlayerCosmetics *out, const int resolve_random) {
    static const struct { int which, type; } SLOTS[5] = {
        {SEL_HAT, 1}, {SEL_BODY, 2}, {SEL_WAIST, 6}, {SEL_FACE, 7}, {SEL_DYE, 5},
    };
    int32_t *slot[5] = {&out->hat, &out->body, &out->waist, &out->face, &out->dye};
    out->skin_color = ui_loadout_skin_color();
    out->hair_color = -1;
    for (int i = 0; i < 5; i++) *slot[i] = worn(sel(SLOTS[i].which), resolve_random, 0, SLOTS[i].type);
}

// WP-F: the v7.1.4 slots of enterGame's "en" (ent[15] shoeIndex, [21] petIndex, [26] wristIndex,
// [30] selectedLoadoutCharms, [31] backIndex, [32] headIndex, [33] playerCardIndex); the secondary's
// charm only for a class with a secondary slot. The badge is the server's (-1 here).
static void look_cosmetics_v7(const int class_index, PlayerCosmeticsV7 *out, const int resolve_random) {
    static const struct { int which, type; } SLOTS[6] = {
        {SEL_HEAD, 15}, {SEL_BACK, 14}, {SEL_SHOE, 8}, {SEL_PET, 9}, {SEL_WRIST, 11}, {SEL_CARD, 16},
    };
    *out = PLAYER_COSMETICS_V7_NONE;
    int32_t *slot[6] = {&out->head, &out->back, &out->shoe, &out->pet, &out->wrist, &out->player_card};
    for (int i = 0; i < 6; i++) *slot[i] = worn(sel(SLOTS[i].which), resolve_random, 0, SLOTS[i].type);
    for (int k = 0; k < 2; k++) out->charms[k] = k == 1 && !class_at(class_index).secondary ? -1 : worn(charm_at(k), resolve_random, 0, 12);
}

// selectedSkinsByWeapon[weapon] of a loadout slot (ent[2]) and meleeIndex (ent[9]).
static void look_skins(const int class_index, PlayerSkins *out, const int resolve_random) {
    const int primary = class_at(class_index).primary, secondary = ui_loadout_secondary(class_index);
    out->primary = primary >= 0 ? worn(weapon_skin(primary), resolve_random, primary + 1, -1) : -1;
    out->secondary = secondary >= 0 ? worn(weapon_skin(secondary), resolve_random, secondary + 1, -1) : -1;
    out->melee = worn(sel(SEL_MELEE), resolve_random, 0, 3);
}

void ui_loadout_look(const int class_index, const int for_ent, LoadoutLook *out) {
    look_cosmetics(&out->cosmetics, for_ent);
    look_cosmetics_v7(class_index, &out->cosmetics_v7, for_ent);
    cosmetics_route(&out->cosmetics, &out->cosmetics_v7); // v3.1.1 placements to their v7 slots
    look_skins(class_index, &out->skins, for_ent);
    ui_loadout_skin_paints(&out->skins, &out->skin_paints); // the account's own painted copies of those skins
    out->secondary_index = ui_loadout_secondary(class_index);
    // forceAttach ? 0 : parseInt(attachIndex): the primary weapon's (also weapon_attachment()'s) or its skin's
    const int primary = class_at(class_index).primary;
    // Krunker Strike: the store items come from the account's Strike loadout only (strike_econ.c),
    // never the Box Office picks; the routing below still applies
    if (strike_econ_apply_look(primary, out->secondary_index, &out->skins, &out->cosmetics, &out->cosmetics_v7))
        cosmetics_route(&out->cosmetics, &out->cosmetics_v7);
    const Weapon *pw = rules_weapon(primary);
    out->attach_index = pw && pw->force_attach ? 0 : cosmetics_attach_index(out->skins.primary, sel(SEL_ATTACH));
}

// ---------------------------------------------------------------- the account's own paint

// v10.0.3's plain-index rule (deob-10.0.3.js:252090-252095): a loadout slot that is a plain store index wears the
// first owned copy of that index with metadata; its m (an object, or a JSON string: the normaliser deob:128883-128894)
// has `paint` "#rrggbb" or `paintShader` n. The owned list is account data slot 10 ({ind, cnt: 1, id, m, st: 0} copies).
int ui_loadout_skin_paint(const int index, WornSkinPaint *out) {
    *out = WORN_SKIN_PAINT_NONE;
    const AccountInfo *acc = active_account();
    const cJSON *owned = acc && acc->data ? cJSON_GetArrayItem(acc->data, 10) : NULL;
    if (index < 0 || !owned) return 0;
    const cJSON *e;
    cJSON_ArrayForEach(e, owned) {
        const cJSON *ind = cJSON_GetObjectItemCaseSensitive(e, "ind");
        if (!cJSON_IsNumber(ind) || ind->valueint != index) continue;
        const cJSON *m = cJSON_GetObjectItemCaseSensitive(e, "m");
        cJSON *parsed = NULL;
        if (cJSON_IsString(m) && m->valuestring) m = parsed = cJSON_Parse(m->valuestring);
        if (!cJSON_IsObject(m)) {
            cJSON_Delete(parsed);
            continue;
        }
        const cJSON *paint = cJSON_GetObjectItemCaseSensitive(m, "paint");
        const cJSON *shader = cJSON_GetObjectItemCaseSensitive(m, "paintShader");
        int found = 0;
        if (cJSON_IsString(paint) && paint->valuestring && paint->valuestring[0] == '#' && strlen(paint->valuestring) == 7) {
            char *end;
            const long rgb = strtol(paint->valuestring + 1, &end, 16);
            if (*end == '\0') {
                out->color = (int) rgb;
                found = 1;
            }
        } else if (cJSON_IsNumber(shader)) {
            out->shader = shader->valueint;
            found = 1;
        }
        cJSON_Delete(parsed);
        if (found) return 1;
    }
    return 0;
}

void ui_loadout_skin_paints(const PlayerSkins *skins, PlayerSkinPaints *out) {
    *out = PLAYER_SKIN_PAINTS_NONE;
    if (!skins) return;
    ui_loadout_skin_paint(skins->primary, &out->primary);
    ui_loadout_skin_paint(skins->secondary, &out->secondary);
    ui_loadout_skin_paint(skins->melee, &out->melee);
}

// The item view's viewer URL tail for an index the account has a painted copy of (v10.0.3's instance → viewer URL,
// deob-10.0.3.js:123160-123173: "&paint=<rrggbb>" or "&paintShader=<n>"); "" otherwise.
void ui_loadout_paint_query(const int index, char *out, const size_t n) {
    WornSkinPaint paint;
    out[0] = '\0';
    if (!ui_loadout_skin_paint(index, &paint)) return;
    if (paint.color >= 0) snprintf(out, n, "&paint=%06x", paint.color & 0xFFFFFF);
    else if (paint.shader >= 0) snprintf(out, n, "&paintShader=%d", paint.shader);
}

int ui_loadout_weapon_skin_shown(const int weapon) {
    const int pick = weapon >= 0 && has_inventory() ? weapon_skin(weapon) : -1;
    return pick >= 0 && viewer_skin(pick) ? pick : -1;
}

static int secondary_req_ok(const Client *client, const Weapon *weapon, const AccountInfo *acc);

// The secondary the player picked (selectedSecondaryIndex, localStorage "secondaryInd",
// default 2 = Pistol) as a Krunker weapon id; -1 when the class has no secondary slot.
int ui_loadout_secondary(const int class_index) {
    if (!class_at(class_index).secondary) return -1;
    const int bound = class_bound_secondary(class_index); // KrunkNative Nettspend: Money Spread
    if (bound >= 0) return bound;
    const int id = sel(SEL_SECONDARY);
    const LoadoutWeapon *w = weapon_at(id);
    if (!w || !w->secondary) return 2;
    // v10 updateSecIndex (v10.js:169364): a secondary its req refuses is the Pistol (2), as the spawn keeps it; its
    // level tail too (deob-10.0.3.js:112377, the server's spawn checks it: game_weapon_level_ok), online and offline.
    const AccountInfo *acc = active_account();
    if (!secondary_req_ok(menu_ui()->client, rules_weapon(id), acc)) return 2;
    return id;
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

// store.skins[index] as assets/skins.json has it (the fields the viewer table leaves out:
// creator, blackM, limT, limited, itemShop, noSpin), read once.
static const cJSON *skin_json(const int index) {
    static cJSON *skins;
    static const cJSON **items; // skins.json entries by index (cJSON_GetArrayItem walks the list)
    static int loaded, count;
    if (!loaded) {
        loaded = 1;
        char *path = concat(assets_path(), "skins.json");
        unsigned char *buf = NULL;
        size_t len = 0;
        if (path && ensure_asset_file(path) && !read_file(path, &buf, &len)) skins = cJSON_ParseWithLength((const char *) buf, len);
        free(buf);
        free(path);
        const int n = cJSON_GetArraySize(skins);
        items = n > 0 ? calloc((size_t) n, sizeof(*items)) : NULL;
        const cJSON *e;
        if (items) cJSON_ArrayForEach(e, skins) items[count++] = e;
    }
    return index >= 0 && index < count ? items[index] : NULL;
}

const cJSON *ui_loadout_skin_json(const int index) { return skin_json(index); } // ui_tooltip.c / v10_notify.c: lore, emoji text

// skin.creator || "Krunker.io"
static const char *skin_creator(const int index) {
    const char *c = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(skin_json(index), "creator"));
    return c && c[0] ? c : "Krunker.io";
}

// meta: the account entry's m (metadata: paint colour / paint shader, client/src/ui/skin_paint.c); NULL when none
typedef struct { int ind, cnt, pos; const cJSON *meta; int copy_id; } OwnedSkin; // copy_id: the entry's id (a copy), 0 for a stack
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

// v10's secondary req (deob-10.0.3.js:112377): a dev, or customOnly allowed and the level/premium tail
// skipped in a custom game. Its offline sandbox is not a custom game, so Customs Only and the level
// locks apply there exactly as in a public game; the client's offline flag stands for is_sandbox, which
// it never receives (same idiom as hud_votekick.c). game_weapon_level_ok is the server-side twin.
static int secondary_req_ok(const Client *client, const Weapon *weapon, const AccountInfo *acc) {
    if (!weapon) return 0;
    if (acc && acc->is_dev) return 1;
    if (game_config_is_custom(&client->game) && !client->game.is_sandbox && !client->offline) return 1;
    if (weapon->custom_only) return 0;
    if (weapon->no_comp && client->game.config.comp) return 0;
    return (acc ? acc->level : 0) >= weapon->min_rec;
}

// The loadout may show and wear store skins: signed in (acc.skins) or offline mode (all of them).
static int has_inventory(void) { return active_account() || all_skins_unlocked(); }

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
    const AccountInfo *acc = active_account();
    const cJSON *skins = acc ? cJSON_GetArrayItem(acc->data, 10) : NULL;
    const int n = cJSON_GetArraySize(skins);
    OwnedSkin *out = n > 0 ? calloc((size_t) n, sizeof(OwnedSkin)) : NULL;
    if (!out) return NULL;
    const cJSON *e;
    cJSON_ArrayForEach(e, skins) {
        const cJSON *ind = cJSON_GetObjectItem(e, "ind"), *cnt = cJSON_GetObjectItem(e, "cnt"), *id = cJSON_GetObjectItem(e, "id");
        if (!cJSON_IsNumber(ind)) continue;
        out[*count] = (OwnedSkin) {ind->valueint, cJSON_IsNumber(cnt) ? cnt->valueint : 1, *count, cJSON_GetObjectItemCaseSensitive(e, "m"),
                                   cJSON_IsNumber(id) ? id->valueint : 0};
        (*count)++;
    }
    g_owned_by_rarity_only = by_rarity_only;
    qsort(out, (size_t) *count, sizeof(OwnedSkin), owned_cmp);
    return out;
}

// store.skins items with `free` (and not blocked) of type SPRAY: v10's list of free sprays (v10
// getSkins(account, true) adds them to everyone's own, guests included).
static int free_spray(const ViewerSkin *s) {
    return s && s->type == 4 && (s->flags & VS_FREE) && !(s->flags & VS_BLOCKED);
}

// A free spray outside KrunkNative's own 0..52 picks (v3.1.1's Game.sprays = store 2482 + i), which
// Select Spray lists by name (BASE_SPRAYS) instead.
static int free_store_spray(const int index) {
    return (index < SPRAY_DEFAULT_INDEX || index >= SPRAY_DEFAULT_INDEX + SPRAYS_BASE_COUNT) && free_spray(store_skin(index));
}

// getRandomOwnedSkin(null, 4) (v10:10178643): one of the account's sprays, the free ones included
// (offline mode: every spray); -1 for a guest (players.spray then sprays 2482).
static int random_owned_spray(void) {
    if (!active_account() && !all_skins_unlocked()) return -1;
    int n = 0, matches = 0, pick = -1;
    OwnedSkin *owned = owned_skins(&n, 0);
    for (int i = 0; i < n; i++) {
        const ViewerSkin *s = store_skin(owned[i].ind);
        if (s && s->type == 4 && !free_spray(s) && rand() % ++matches == 0) pick = owned[i].ind; // reservoir sample
    }
    free(owned);
    for (int i = 0; i < g_viewer_skin_count; i++) {
        if (free_spray(store_skin(i)) && rand() % ++matches == 0) pick = i;
    }
    return pick;
}

// ent[1]: selectedSprayIndex, as v10's ENTER_GAME sends it: the saved store index (-2 Random:
// random_owned_spray). The client does not check ownership: the server's players.spray keeps a
// free spray or one canUseSkin allows (the account owns it), anything else sprays 2482
// (shared/src/social_v7.c game_spray_pick). KrunkNative's own picks 0..52 are the free store items
// 2482 + i.
int ui_loadout_spray(void) {
    const int strike = strike_econ_spray(); // Krunker Strike: the equipped Strike spray
    if (strike >= 0) return strike;
    const int pick = sel(SEL_SPRAY);
    if (pick == -2) return random_owned_spray();
    if (pick >= 0 && pick < SPRAYS_BASE_COUNT) return store_spray_from_legacy(pick);
    return pick;
}

// updateSprayWheel: the account's SPRAY items other than the equipped one (acc.skins order, then
// the free sprays: getSkins(account, true); v7 puts the favourites first, KrunkNative keeps no
// favourites), at most SPRAY_WHEEL_MAX, then the equipped spray (sprayIndex <= 0 -> 2482).
// Returns the count written to `out` (<= max).
int ui_loadout_spray_wheel(int *out, const int max) {
    if (!out || max <= 0) return 0;
    const int pick = sel(SEL_SPRAY);
    const int equipped = pick >= 0 ? store_spray_from_legacy(pick) : SPRAY_DEFAULT_INDEX;
    int count = 0, n = 0;
    OwnedSkin *skins = owned_skins(&n, 0);
    for (int i = 0; i < n && count < SPRAY_WHEEL_MAX && count < max - 1; i++) {
        const ViewerSkin *skin = store_skin(skins[i].ind);
        if (skin && skin->type == 4 && !free_spray(skin) && skins[i].ind != equipped) out[count++] = skins[i].ind;
    }
    free(skins);
    for (int i = 0; i < g_viewer_skin_count && count < SPRAY_WHEEL_MAX && count < max - 1; i++) {
        if (free_spray(store_skin(i)) && i != equipped) out[count++] = i;
    }
    const ViewerSkin *eq = store_skin(equipped);
    out[count++] = eq && eq->type == 4 ? equipped : SPRAY_DEFAULT_INDEX;
    return count;
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

// The loadout wears this store index: a cosmetic slot or a weapon's saved skin (the wallet's "Keep equipped items",
// deob-10.0.4.js:147054-147072, :147296). KrunkNative's slots hold plain indices: a painted copy is worn as the first
// owned copy of its index (ui_loadout_skin_paint).
int ui_loadout_wears(const int index) {
    if (index < 0) return 0;
    static const int slots[] = {SEL_DYE, SEL_FACE, SEL_SHOE, SEL_MELEE, SEL_BODY, SEL_BACK, SEL_HAT, SEL_HEAD, SEL_CARD,
                                SEL_WAIST, SEL_PET, SEL_WRIST, SEL_SPRAY};
    for (size_t i = 0; i < sizeof(slots) / sizeof(slots[0]); i++)
        if (sel(slots[i]) == index) return 1;
    weapon_skin(0); // the "skins" cache
    const cJSON *v;
    cJSON_ArrayForEach(v, g_skins_cache) if (cJSON_IsNumber(v) && v->valueint == index) return 1;
    return 0;
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

// A weapon's default preview, v7's UTILS.assetsUrl("/textures/previews/weapons/" + src + ".png"):
// from the content tree, which has every weapon (ui/textures only the v7.1.4 slice, 1..19).
static void weapon_preview(const LoadoutWeapon *w, char *out, const size_t n) {
    char path[96];
    if (w && kn_loadout_weapon(w->id)) { // KrunkNative Nettspend: page-relative previews (ui/img/kn)
        snprintf(out, n, "img/kn/preview_%s.png", w->id == WEAPON_ID_DRANK ? "drank" : "money_spread");
        return;
    }
    snprintf(path, sizeof(path), "textures/previews/weapons/%s.png", w && w->src ? w->src : "undefined");
    asset(path, out, n);
}

// assetsUrl("/textures/previews/cosmetics/<type>_default.png")
static void cosmetic_default(const int type, char *out, const size_t n) {
    char path[64];
    snprintf(path, sizeof(path), "textures/previews/cosmetics/%d_default.png", type);
    asset(path, out, n);
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
    sb_appendf(out, "<div class=\"custSubContainer\" onclick=\"charmSelector(%d)\">\n\t\t\t\t<div class=\"custItNm\" style=\"font-size: 12px;\">%s</div>\n\t\t\t\t", slot,
               I18N("windows.loadout.charm", "Charm"));
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

// tab 0: the class cards (Game.config.classes). Only an official krunker.io game locks a class by level
// (ui_loadout_class_open); Box Office lobbies, the sandbox and offline mode open every class.
static void gen_tab_class(StrBuf *out) {
    const MenuUi *ui = menu_ui();
    const AccountInfo *acc = active_account();
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
        // KrunkNative's own class (the Nettspend) in an official krunker.io game: locked like a level lock
        // (game_class_allowed, the rule player_spawn and client_enter_game follow)
        if (!game_class_allowed(&ui->client->game, c->index)) {
            sb_appendf(out, "<div class=\"classCard\" onmouseenter=\"playTick()\" >\n\t\t\t\t\t\t<div class=\"classImgH\">\n\t\t\t\t\t\t\t<img loading=\"lazy\" style=\"image-rendering:pixelated\" class=\"classImgC\" src=\"%s\">\n\t\t\t\t\t\t\t<div class='classHP'>%d<span style='color: #868484;font-size: 12px;'> HP</span></div>\n\t\t\t\t\t\t</div>\n\t\t\t\t\t\t<div class=\"className\">",
                       icon, c->hp);
            sb_append_esc(out, c->name ? c->name : "");
            sb_append(out, "</div>\n\t\t\t\t\t\t<div class=\"classWeap\">");
            sb_append_esc(out, c->weapon ? c->weapon : "");
            sb_appendf(out, "</div>\n\t\t\t\t\t\t<div class='classXPBar'>\n\t\t\t\t\t\t\t<div class='clsXPBarC' style='width:calc(%d%% - 8px)'></div>\n\t\t\t\t\t\t\t<div class='clsXPBarV'>%d / %d</div>\n\t\t\t\t\t\t</div>\n\t\t\t\t\t\t<div class='classLvl'>Lvl %d</div>\n\t\t\t\t\t\t<div class='lockedClass'>\n\t\t\t\t\t\t\t<span class='material-icons' style='font-size: 70px;color:#fff;'>\n\t\t\t\t\t\t\tlock\n\t\t\t\t\t\t\t</span>\n\t\t\t\t\t\t\t<span class='lockedClassText'>Not on official servers</span>\n\t\t\t\t\t\t</div>\n\t\t\t\t\t</div>",
                       pct, (int) (xp - lo), (int) (hi - lo), level);
            continue;
        }
        const int open = class_unlocked(c->index);
        if (!open) { // v10: no onclick, the req line instead of the weapon, the lock over the card
            sb_appendf(out, "<div class=\"classCard\" onmouseenter=\"playTick()\" >\n\t\t\t\t\t\t<div class=\"classImgH\">\n\t\t\t\t\t\t\t<img loading=\"lazy\" style=\"image-rendering:pixelated\" class=\"classImgC\" src=\"%s\">\n\t\t\t\t\t\t\t<div class='classHP'>%d<span style='color: #868484;font-size: 12px;'> HP</span></div>\n\t\t\t\t\t\t</div>\n\t\t\t\t\t\t<div class=\"className\">",
                       icon, c->hp);
            sb_append_esc(out, c->name ? c->name : "");
            sb_appendf(out, "</div>\n\t\t\t\t\t\t<div class=\"classWeap\">Req lvl %d</div>\n\t\t\t\t\t\t<div class='classXPBar'>\n\t\t\t\t\t\t\t<div class='clsXPBarC' style='width:calc(%d%% - 8px)'></div>\n\t\t\t\t\t\t\t<div class='clsXPBarV'>%d / %d</div>\n\t\t\t\t\t\t</div>\n\t\t\t\t\t\t<div class='classLvl'>Lvl %d</div>\n\t\t\t\t\t\t<div class='lockedClass'>\n\t\t\t\t\t\t\t<span class='material-icons' style='font-size: 70px;color:#fff;'>\n\t\t\t\t\t\t\tlock\n\t\t\t\t\t\t\t</span>\n\t\t\t\t\t\t\t<span class='lockedClassText'>Register to unlock</span>\n\t\t\t\t\t\t</div>\n\t\t\t\t\t</div>",
                       class_min_rec(c->index), pct, (int) (xp - lo), (int) (hi - lo), level);
            continue;
        }
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
    weapon_preview(pw, prim.img, sizeof(prim.img));
    const int bound = lc.secondary ? class_bound_secondary(class_index) : -1; // KrunkNative Nettspend
    const int sec_id = bound >= 0 ? bound : sel(SEL_SECONDARY);
    const LoadoutWeapon *sw = weapon_at(sec_id) ? (sw_copy = *weapon_at(sec_id), &sw_copy) : NULL;
    snprintf(sec.name, sizeof(sec.name), "%s", lc.secondary && sw ? sw->name : "None");
    weapon_preview(sw, sec.img, sizeof(sec.img));
    const int melee = sel(SEL_MELEE);
    snprintf(mel.name, sizeof(mel.name), "%s", melee == -2 ? "Random" : "Combat Knife");
    cosmetic_default(3, mel.img, sizeof(mel.img));
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
    sb_appendf(out, "<div class='custItNm'>%s</div>", I18N("windows.loadout.primary", "Primary"));
    cust_label_icon(out, &prim, ";right: unset;left: 8px;", "custIcon");
    const int prim_charm = pw && pw->charm;
    if (prim_charm) charm_box(out, 0);
    if (has_attach) {
        const int a = sel(SEL_ATTACH);
        sb_appendf(out, "<div class=\"custSubContainer contCanSelect%s\" onclick=\"viewAttachments()\">\n\t\t\t\t<div class=\"custItNm\" style=\"font-size: 12px;\">%s</div>\n\t\t\t\t", prim_charm ? " bottom" : "",
                   I18N("windows.loadout.primary.sight", "Sight"));
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
    // secondary (a class-bound one has no Select Secondary: the tile is not a button)
    if (lc.secondary && bound < 0) sb_append(out, "<div class='custContainer contCanSelect' onclick='showWindow(20)' style='width: 47.5%;height: 163px;' onmouseenter='playTick()'>");
    else sb_append(out, "<div class='custContainer' style='width: 47.5%;height: 163px;' onmouseenter='playTick()'>");
    sb_appendf(out, "<div class='custItNm'>%s</div>", I18N("windows.loadout.secondary", "Secondary"));
    cust_label_icon(out, &sec, ";right: unset;left: 8px;", "custIcon");
    if (lc.secondary && sw && sw->charm) charm_box(out, 1);
    sb_append(out, "</div> ");
    // melee
    sb_appendf(out, "<div class='custContainer contCanSelect' onclick='showWindow(26)' style='width: 30.7%%;height: 163px;' onmouseenter='playTick()'><div class='custItNm'>%s</div>",
               I18N("windows.loadout.melee", "Melee Skin"));
    cust_label_icon(out, &mel, "", "custIcon");
    sb_append(out, "</div> ");
    // reticle (attachment weapons) or scope (scoped weapons)
    char rname[64], rimg[512] = "";
    if (has_attach) {
        const int r = sel(SEL_RETICLE);
        if (r >= 0 && r < RETICLE_COUNT) {
            snprintf(rname, sizeof(rname), "%s", RETICLES[r]);
            char path[64];
            snprintf(path, sizeof(path), "textures/reticles/reticle_%d.png", r);
            asset(path, rimg, sizeof(rimg));
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
    sb_appendf(out, "<div class='custItNm'>%s</div><span class='settLabel blackShad' style='color:%s'>",
               has_attach ? I18N("windows.loadout.primary.reticle", "Reticle") : I18N("windows.loadout.primary.scope", "Scope"),
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
    sb_appendf(out, "<div class='custContainer contCanSelect' onclick='showWindow(45)' onmouseenter='playTick()' style='width: 30.7%%;height: 163px;'><div class='custItNm'>%s</div>",
               I18N("windows.loadout.streaks", "Streaks"));
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

// i18n.t("windows.loadout." + slot key): the slot's name
static const char *slot_label(const CustSlot *cs) {
    char key[48];
    snprintf(key, sizeof(key), "windows.loadout.%s", cs->key);
    return i18n_tr(key, cs->label);
}

// Non-premium accounts may not wear hat + head or body + back together (v7 decision 9; the
// offline sandbox player is exempt like v7's sandbox, a dev account too).
static int premium_pair_blocked(const int i) {
    const AccountInfo *acc = active_account();
    if (CUST_SLOTS[i].pair < 0 || !acc || acc->premium || acc->is_dev || all_skins_unlocked()) return 0;
    return slot_used(sel(CUST_SLOTS[CUST_SLOTS[i].pair].which)) && slot_used(sel(CUST_SLOTS[i].which));
}

static void gen_tab_customize(StrBuf *out) {
    int frames_seen[16], frames_n = 0;
    for (int i = 0; i < CUST_SLOT_COUNT; i++) {
        const CustSlot *cs = &CUST_SLOTS[i];
        int pick = sel(cs->which);
        if (i > CUST_SLOTS[i].pair && premium_pair_blocked(i)) {
            sb_appendf(out, "<div class='custContainer' onclick='windows[13].premium()' style='width: 22.5%%;height: 174px;'><div class='custItNm blackShad'>%s</div>", slot_label(cs));
            sb_appendf(out, "<div class=\"custPremReq blackShad\">\n\t\t\t\t\t\t<div style=\"color: var(--yellow);font-size: 15px;\">Premium Required to use Both</div>\n\t\t\t\t\t\tor\n\t\t\t\t\t\t<div style=\"color: var(--red);font-size: 15px;\">Remove %s</div>\n\t\t\t\t\t</div></div> ",
                       slot_label(&CUST_SLOTS[cs->pair]));
            continue;
        }
        SlotView v = {.color = "#efefef"};
        snprintf(v.name, sizeof(v.name), "%s", pick == -2 ? "Random" : "None");
        if (cs->type < 0) asset("textures/sprays/0.png", v.img, sizeof(v.img));
        else cosmetic_default(cs->type, v.img, sizeof(v.img));
        const char *img_class = cs->type == 16 ? "custIconCard" : "custIcon";
        int spray_item = 0;
        const char *spray_base = cs->type < 0 ? spray_name(pick) : NULL;
        const ViewerSkin *s = cs->type < 0 && spray_base ? NULL
                            : (has_inventory() || (cs->type < 0 && free_spray(store_skin(pick))) ? store_skin(pick) : NULL);
        if (spray_base) { // KrunkNative's default sprays (Game.sprays of v3.1.1): free items
            snprintf(v.name, sizeof(v.name), "%s", spray_base);
            char path[64];
            snprintf(path, sizeof(path), "textures/sprays/%d.png", BASE_SPRAYS[pick].id);
            asset(path, v.img, sizeof(v.img));
            spray_item = 1;
        } else if (s && (cs->type < 0 ? s->type == 4 : s->type == cs->type)) {
            slot_item(&v, pick);
            v.glow = NULL;
            spray_item = s->type == 4;
        }
        sb_appendf(out, "<div class='custContainer contCanSelect' onclick='showWindow(%d)' onmouseenter='playTick()' style='width: 22.25%%;height: 174px;'><div class='custItNm blackShad'>%s</div><div class='custClear' onclick='windows[%d].clear();'><span class='material-icons'>close</span></div>",
                   cs->window, slot_label(cs), cs->window - 1);
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
    color_row_premium(out, I18N("windows.loadout.color.skin", "Skin Color"));
    const int skin = sel(SEL_SKIN_COL);
    for (int i = SKIN_COLOR_COUNT - 1; i >= 0; i--) {
        sb_appendf(out, "<div class='skinColorItem%s' style='background-color:#%s' onmouseenter='playTick()' onclick='selectSkinColor(%d)'></div>",
                   skin == i ? " activeSkin" : "", SKIN_COLORS[i], i);
    }
    sb_append(out, "</div>");
    color_row_premium(out, I18N("windows.loadout.color.hair", "Hair Color"));
    sb_append(out, "</div>");
    color_row_premium(out, I18N("windows.loadout.color.chat", "Chat Name Color"));
    sb_append(out, "</div>");
    const int stat = sel(SEL_PC_STAT);
    sb_appendf(out, "<div class='settName' style='border-bottom:unset;'>%s<select onchange='selectStat(this.value)' class='inputGrey2' style='font-size:16px;padding:4px;margin-bottom: 5px;'><option value='-1' %s>None</option>",
               I18N("windows.loadout.stat", "Playercard Stat"), stat == -1 ? "selected" : "");
    for (int i = 0; i < PC_STAT_COUNT; i++) sb_appendf(out, "<option value='%d' %s>%s</option>", i, stat == i ? "selected" : "", PC_STATS[i]);
    sb_append(out, "</select></div>");
}

// ---- the pieces the v10 page's Change Loadout shell reuses (v10_customize.c)
void ui_loadout_tab_html(StrBuf *out, const int tab) {
    switch (tab) {
        case 0: gen_tab_class(out); break;
        case 1: gen_tab_loadout(out, menu_ui()->client->menu_class_index); break;
        case 2: gen_tab_customize(out); break;
        default: gen_tab_other(out); break;
    }
}

int ui_loadout_tab(void) { return g_tab; }
void ui_loadout_set_tab(const int tab) { g_tab = tab >= 0 && tab <= 3 ? tab : 1; }

int ui_loadout_saved(const char *key) {
    for (int i = 0; i < SEL_COUNT; i++)
        if (!strcmp(g_sel[i].key, key)) return sel(i);
    return -1;
}

int ui_loadout_skin_color_count(void) { return SKIN_COLOR_COUNT; }
const char *ui_loadout_skin_color_hex(const int i) { return i >= 0 && i < SKIN_COLOR_COUNT ? SKIN_COLORS[i] : ""; }
int ui_loadout_pc_stat_count(void) { return PC_STAT_COUNT; }
const char *ui_loadout_pc_stat_name(const int i) { return i >= 0 && i < PC_STAT_COUNT ? PC_STATS[i] : ""; }
void ui_loadout_refresh_preview(void) { refresh_class_box(); }

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

// ---- the open picker's card list: Select Skin (windows[14]), f1828 (hat, body, melee, ...)
// and Select Spray. v7.1.4 writes every owned card into #skinList at once (a staff account owns
// all 6448 store items; a weapon or type has up to ~550); here #skinList is a virtual grid
// (ui_vgrid.h) over g_pick.view, the searched entries: the same cards in the same places, only
// the rows near the scroll position exist in the DOM.
typedef enum { PICK_OWNED, PICK_NONE, PICK_RANDOM, PICK_FREE_SPRAY } PickEntryKind;
typedef struct { PickEntryKind kind; int ind, cnt; const cJSON *meta; int copy_id; } PickEntry; // ind: store index (owned) / free spray; meta: OwnedSkin.meta; copy_id: OwnedSkin.copy_id
typedef enum { PICKER_ITEMS, PICKER_SKIN, PICKER_SPRAY } PickerKind;
static struct {
    PickerKind kind;
    char fn[32];            // select function (f1828's funcName, selectSkin, selectSpray)
    const char *img_class;  // skinImg / skinImgC / skinImgD / skinImgP
    char default_img[256];  // None / Random preview
    int type, with_season;
    PickEntry *all;
    int n_all;
    int *view, n_view;      // positions -> all[]: the cards searchItems keeps
    VGrid grid;
} g_pick;

static void pick_add(const PickEntryKind kind, const int ind, const int cnt) {
    PickEntry *grown = realloc(g_pick.all, (size_t) (g_pick.n_all + 1) * sizeof(*grown));
    if (!grown) return;
    g_pick.all = grown;
    g_pick.all[g_pick.n_all++] = (PickEntry) {kind, ind, cnt};
}

// An owned entry with its metadata (owned_paint_badge reads it).
static void pick_add_owned(const OwnedSkin *o) {
    const int before = g_pick.n_all;
    pick_add(PICK_OWNED, o->ind, o->cnt);
    if (g_pick.n_all > before) {
        g_pick.all[before].meta = o->meta;
        g_pick.all[before].copy_id = o->copy_id;
    }
}

static void pick_card(StrBuf *out, int pos, void *user);

// New picker: entries are added with pick_add(), then pick_list() writes #skinList.
static void pick_begin(const PickerKind kind, const char *fn, const char *img_class, const int type) {
    free(g_pick.all);
    free(g_pick.view);
    g_pick.all = NULL;
    g_pick.view = NULL;
    g_pick.n_all = g_pick.n_view = 0;
    g_pick.kind = kind;
    snprintf(g_pick.fn, sizeof(g_pick.fn), "%s", fn);
    g_pick.img_class = img_class;
    g_pick.type = type;
    g_pick.with_season = 1;
}

static void pick_view_all(void) {
    free(g_pick.view);
    g_pick.view = g_pick.n_all ? malloc((size_t) g_pick.n_all * sizeof(int)) : NULL;
    g_pick.n_view = 0;
    for (int i = 0; g_pick.view && i < g_pick.n_all; i++) g_pick.view[g_pick.n_view++] = i;
}

static void pick_list(StrBuf *out) {
    pick_view_all();
    vgrid_begin(&g_pick.grid, "skinList", g_pick.n_view, pick_card, NULL);
    vgrid_html(&g_pick.grid, out);
}

// The paint badge of an owned card (Krunker v10.0.3's skinPaintBadge, client/src/ui/skin_paint.c): the store
// paint of the item's colour (type 18, same colour) gives its name and thumbnail. A shader paint has no store
// colour here, so it takes its shader's store paint name (skin_paint.c) with the format_paint icon.
static void owned_paint_badge(StrBuf *out, const OwnedSkin *o) {
    if (!skin_paint_shows(o->meta)) return;
    const char *colour = skin_paint_color(o->meta);
    char name[128], thumb[256], html[1024];
    SkinPaint store = {NULL, NULL};
    for (int i = 0; colour && i < g_viewer_skin_count; i++) {
        const ViewerSkin *p = store_skin(i);
        if (!p || p->type != 18 || !skin_paint_same_color(p->paint_color, colour)) continue;
        if (p->name) {
            snprintf(name, sizeof(name), "%s", p->name);
            store.name = name;
        }
        skin_thumb(i, thumb, sizeof(thumb));
        store.thumb = thumb[0] ? thumb : NULL;
        break;
    }
    skin_paint_badge(html, sizeof(html), o->meta, &store);
    sb_append(out, html);
}

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
    sb_appendf(out, "<img loading='lazy' %sclass='%s%s' src='%s' />",
               weapon >= 0 ? "draggable='false' " : "", img_class, (s->flags & VS_RGB) ? " rgbHue" : "", img);
    owned_paint_badge(out, o); // v10.0.3: after the image, before the info button
    // v10.0.3's paint-icon (deob:147316, shown when paintTargets and a skinId: deob:150226): the picker of this unit
    if (paint_picker_can_open(o->ind))
        sb_appendf(out, "<div class='selctPaintBtn' onmouseenter='playTick()' onclick='paintOpen(%d,%d);event.stopPropagation()'><i class='material-icons'>format_paint</i></div>",
                   o->ind, o->copy_id ? o->copy_id : o->ind);
    sb_appendf(out, "<div class='selctInfoBtn' onmouseenter='playTick()'onclick='viewItem(%d)'>?</div><div class='selctFavBtn material-icons' onmouseenter='playTick()' data-col='white' data-icon='star_border' onclick='favItem(this, %d)'></div></div>",
               o->ind, o->ind);
}

// One store spray card of Select Spray (v10:10518334: a free one has a light grey border, no
// count and "Free Spray").
static void spray_card(StrBuf *out, const PickEntry *e, const ViewerSkin *s) {
    const char *col = rarity_color(s);
    const int is_free = free_spray(s);
    char img[256], anim[96] = "", style[96], count[48] = "";
    skin_thumb(e->ind, img, sizeof(img));
    if (s->frames) snprintf(anim, sizeof(anim), "animation: sprayAnm%d %gs steps(%d) infinite;", s->frames, 1 + s->frame_t / 1000.0, s->frames);
    if (is_free) snprintf(style, sizeof(style), "border: 5px solid lightgrey");
    else snprintf(style, sizeof(style), "color:%s;border: 5px solid %s", col, col);
    if (!is_free) snprintf(count, sizeof(count), "<div class='sprayCnt'>x%d</div>", e->cnt);
    sb_appendf(out, "<div class='noBtnCard%s%s' style='height:158px;%s'onclick='selectSpray(%d);cancelTip()' data-index='%d' onmouseover='createTip(this)' onmouseout='cancelTip()'>%s",
               s->rarity == 5 ? "" : " blackShad", !is_free && rarity_anim(s) ? " rainbow" : "", style, e->ind, e->ind, count);
    sb_append_esc(out, s->name ? s->name : "");
    sb_appendf(out, "<div class='itemOwn'>%s</div><div draggable='false' class='sprayImg' onmouseenter='playTick()' style='background: url(&quot;%s&quot;) -1px;%s'></div></div>",
               is_free ? I18N("windows.spray.default", "Free Spray") : I18N("windows.spray.spin", "Spray"), img, anim);
}

static void pick_card(StrBuf *out, const int pos, void *user) {
    (void) user;
    if (pos < 0 || pos >= g_pick.n_view) return;
    const PickEntry *e = &g_pick.all[g_pick.view[pos]];
    const char *fn = g_pick.fn, *img_class = g_pick.img_class, *img = g_pick.default_img;
    const int type = g_pick.type;
    if (g_pick.kind == PICKER_SPRAY) {
        if (e->kind == PICK_RANDOM) {
            { char sp[256]; asset("textures/sprays/0.png", sp, sizeof(sp)); sb_appendf(out, "<div class='noBtnCard blackShad' onclick='selectSpray(-2)' style='height:158px;border: 5px solid lightgrey'>Random<div class='itemOwn' style='opacity:0;''>by \?\?\?</div><img draggable='false' style='margin-top:10px;width:110px;' class='noBtnImgC skinSil' src='%s' onmouseenter='playTick()'><div class='skinRandom blackShad' style='top: 80px;'>?</div></div>", sp); }
        } else if (e->kind == PICK_FREE_SPRAY) {
            const int i = e->ind, frames = BASE_SPRAYS[i].id == 88 ? 4 : 0; // Verb: 4 frames x 250 ms
            char spray_path[64], spray_url[256];
            snprintf(spray_path, sizeof(spray_path), "textures/sprays/%d.png", BASE_SPRAYS[i].id);
            asset(spray_path, spray_url, sizeof(spray_url));
            sb_appendf(out, "<div class='noBtnCard blackShad' style='height:158px;border: 5px solid lightgrey'onclick='selectSpray(%d);cancelTip()' data-index='%d' onmouseover='createTip(this)' onmouseout='cancelTip()'>%s<div class='itemOwn'>%s</div><div draggable='false' class='sprayImg' onmouseenter='playTick()' style='background: url(&quot;%s&quot;) -1px;%s'></div>",
                       i, i, BASE_SPRAYS[i].name, I18N("windows.spray.default", "Free Spray"), spray_url, frames ? "animation: sprayAnm4 1.25s steps(4) infinite;" : "");
            // the store's free spray items (2482 + i): their item / favourite buttons as on every card
            sb_appendf(out, "<div class='selctInfoBtn' onmouseenter='playTick()' onclick='viewItem(%d)'>?</div><div class='selctFavBtn material-icons' onmouseenter='playTick()' data-col='white' data-icon='star_border' onclick='favItem(this, %d)'></div></div>",
                       SPRAY_DEFAULT_INDEX + i, SPRAY_DEFAULT_INDEX + i);
        } else {
            const ViewerSkin *s = store_skin(e->ind);
            if (s) spray_card(out, e, s);
        }
        return;
    }
    if (g_pick.kind == PICKER_SKIN) {
        if (e->kind == PICK_NONE)
            sb_appendf(out, "<div class='skinCard blackShad' style='border:5px solid lightgrey' onmouseenter='playTick()' onclick='selectSkin(-3,%d)'>None<div class='itemOwn'>%s</div><div class='itemSea' style='opacity:0;'>Season 1</div><img draggable='false' class='skinImg' src='%s'></div>", g_skin_weapon,
                       I18N("generic.default", "Default"), img);
        else if (e->kind == PICK_RANDOM)
            sb_appendf(out, "<div class='skinCard blackShad' style='border:5px solid lightgrey' onmouseenter='playTick()' onclick='selectSkin(-2,%d)'>Random<div class='itemOwn' style='opacity:0;'>by \?\?\?</div><div class='itemSea' style='opacity:0;'>All Seasons</div><img draggable='false' class='skinImg skinSil' onmouseenter='playTick()' src='%s'><div class='skinRandom blackShad' style='top:68px'>?</div></div>", g_skin_weapon, img);
        else {
            const ViewerSkin *s = store_skin(e->ind);
            const OwnedSkin o = {e->ind, e->cnt, 0, e->meta, e->copy_id};
            if (s) owned_card(out, &o, s, "selectSkin", "skinImg", 1, s->weapon - 1);
        }
        return;
    }
    if (e->kind == PICK_NONE)
        sb_appendf(out, "<div class='skinCard blackShad' style='border:5px solid lightgrey' onmouseenter='playTick()' onclick='%s(-3)'>%s<div class='itemOwn'>%s</div>%s<img class='%s' src='%s'></div>",
                   fn, type == 3 ? "Combat Knife" : "None", I18N("generic.default", "Default"), type == 5 ? "" : "<div class='itemSea' style='opacity:0;'>Season 1</div>", img_class, img);
    else if (e->kind == PICK_RANDOM)
        sb_appendf(out, "<div class='skinCard blackShad' style='border:5px solid lightgrey' onmouseenter='playTick()' onclick='%s(-2)'>Random<div class='itemOwn' style='opacity:0;'>by \?\?\?</div>%s<img class='%s skinSil' src='%s'><div class='skinRandom blackShad'>?</div></div>",
                   fn, type == 5 ? "" : "<div class='itemSea' style='opacity:0;'>All Seasons</div>", img_class, img);
    else {
        const ViewerSkin *s = store_skin(e->ind);
        const OwnedSkin o = {e->ind, e->cnt, 0, e->meta, e->copy_id};
        if (s) owned_card(out, &o, s, fn, img_class, type != 5, -1);
    }
}

// f1828(funcName, type): None (-3) + Random (-2) + the owned items of that store type.
static char *gen_items_list(const char *fn, const int type) {
    const char *img_class = type ? (type == 16 ? "skinImgP" : type == 5 ? "skinImgD" : "skinImgC") : "skinImg";
    pick_begin(PICKER_ITEMS, fn, img_class, type);
    cosmetic_default(type, g_pick.default_img, sizeof(g_pick.default_img));
    pick_add(PICK_NONE, -3, 0);
    pick_add(PICK_RANDOM, -2, 0);
    int n;
    OwnedSkin *owned = owned_skins(&n, 0);
    for (int i = 0; i < n; i++) {
        const ViewerSkin *s = store_skin(owned[i].ind);
        if (s && s->type == type) pick_add_owned(&owned[i]);
    }
    free(owned);
    StrBuf out = {0};
    sb_append(&out, ITEM_SEARCH_HEAD("2", "") "<div id='skinList' style='overflow-y: scroll;height: calc(100% - 95px);'>");
    pick_list(&out);
    sb_append(&out, "</div>");
    return sb_finish(&out);
}

// windows[14] Select Skin for weapon activeSkinSel (back to Select Secondary for secondaries).
static char *gen_skins(void) {
    const LoadoutWeapon *w = weapon_at(g_skin_weapon);
    pick_begin(PICKER_SKIN, "selectSkin", "skinImg", 0);
    weapon_preview(w, g_pick.default_img, sizeof(g_pick.default_img));
    pick_add(PICK_NONE, -3, 0);
    pick_add(PICK_RANDOM, -2, 0);
    int n;
    OwnedSkin *owned = owned_skins(&n, 0);
    for (int i = 0; i < n; i++) {
        const ViewerSkin *s = store_skin(owned[i].ind);
        if (s && !s->type && s->weapon == g_skin_weapon + 1) pick_add_owned(&owned[i]);
    }
    free(owned);
    StrBuf out = {0};
    sb_appendf(&out, "<div id='itemSearchH'><div class='custBack' onclick='playSelect(0.1);goBackWindow(%d)' onmouseenter='playTick();'><span class='material-icons custBackArr'>arrow_back</span></div><input id='itemSearch' type='text' placeholder='Search Item' onkeyup='searchItems(this)'><div class='winClose' onclick='playSelect(0.1);closWind()' onmouseenter='playTick();'><span class='material-icons winCloseArr'>close</span></div></div>",
               w && w->secondary ? 19 : 2);
    sb_append(&out, "<div id='skinList' style='margin-top: 8px;overflow-y: scroll;height: calc(100% - 95px);'>");
    pick_list(&out);
    sb_append(&out, "</div>");
    return sb_finish(&out);
}

// windows[19] Select Secondary: every `secondary` weapon by minRec; locked ones need that
// level in a public game and in v10's offline sandbox, open in a custom game (secondary_req_ok).
static char *gen_secondary(void) {
    const AccountInfo *acc = active_account();
    const Client *client = menu_ui()->client;
    StrBuf out = {0};
    sb_append(&out, "<div class='winBackH'><div class='custBack' onclick='playSelect(0.1);goBackWindow(44)' onmouseenter='playTick();'><span class='material-icons custBackArr'>arrow_back</span></div><div class='winSelH'>Select Secondary</div><div class='winClose' onclick='playSelect(0.1);closWind()' onmouseenter='playTick();'><span class='material-icons winCloseArr'>close</span></div></div>");
    sb_append(&out, "<div id='skinList'>");
    for (int rec = 0; rec <= 255; rec++) { // stable sort by minRec
        for (int i = 0; i < rules_krunker_weapon_count(); i++) {
            const LoadoutWeapon *w = weapon_at(i);
            if (!w || !w->secondary || w->min_rec != rec) continue;
            // v10's card (v10.js:214015-214020): a secondary whose req refuses it has no click (customOnly: a custom
            // game that is not comp, or a dev; noComp: not in comp; minRec: the level, not in a custom game,
            // game_weapon_level_ok, deob-10.0.3.js:112377). all_skins_unlocked() is about skins, not this level
            // requirement: the offline sandbox shows the locks like a public game.
            const int unlocked = secondary_req_ok(client, rules_weapon(i), acc);
            const int skin = has_inventory() ? weapon_skin(i) : -1;
            char img[256];
            if (skin >= 0 && store_skin(skin)) skin_thumb(skin, img, sizeof(img));
            else weapon_preview(w, img, sizeof(img));
            sb_append(&out, "<div class='skinCard blackShad' style='height: 165px;border:5px solid lightgrey'");
            if (unlocked) sb_appendf(&out, " onclick='skinSelector(%d, true)'", i);
            sb_append(&out, ">");
            sb_append_esc(&out, w->name);
            if (w->min_rec) {
                char lvl[16], own[64];
                snprintf(lvl, sizeof(lvl), "%d", w->min_rec);
                if (unlocked) snprintf(own, sizeof(own), "%s", I18N("windows.secondary.unlocked", "Unlocked"));
                else I18N_F(own, "windows.secondary.req", "Req lvl {0}", lvl);
                sb_appendf(&out, "<div class='itemOwn'>%s</div>", own);
            } else {
                sb_appendf(&out, "<div class='itemOwn'>%s</div>", I18N("generic.default", "Default"));
            }
            sb_appendf(&out, "<img loading='lazy' draggable='false' class='secImg' onmouseenter='playTick()' src='%s' />", img);
            if (!unlocked) sb_append(&out, "<div class='lockedCard'><span class='material-icons' style='font-size: 70px;color:#fff;'>lock</span></div>");
            sb_append(&out, "</div>");
        }
    }
    sb_append(&out, "</div>");
    return sb_finish(&out);
}

static int class_primary_has_attach(void) {
    const LoadoutWeapon *w = weapon_at(class_at(menu_ui()->client->menu_class_index).primary);
    return w && w->attach;
}

// v10 Acog Scope req (v10.js:178111): a dev, or the CLASS level max(1, floor(rankVar * sqrt(stats.c<class>)) + 1)
// >= minRec. The window also opens it in a custom game and in the sandbox (isCustom / isSandbox, v10.js:214308):
// KrunkNative's offline sandbox and offline mode count as both, like the secondary lock.
static int attach_unlocked(const int index, const int class_index) {
    const Client *client = menu_ui()->client;
    const AccountInfo *acc = active_account();
    if (index < 0 || index >= ATTACH_COUNT || !ATTACHMENTS[index].min_rec) return 1;
    if (!online_mode_online() || client->offline || game_config_is_custom(&client->game)) return 1;
    if (acc && acc->is_dev) return 1;
    char key[16];
    snprintf(key, sizeof(key), "c%d", class_index);
    const cJSON *xp_v = acc && acc->stats ? cJSON_GetObjectItem(acc->stats, key) : NULL;
    const double xp = cJSON_IsNumber(xp_v) ? xp_v->valuedouble : 0.0;
    const int level = (int) floor(RANK_VAR * sqrt(xp)) + 1;
    return (level > 1 ? level : 1) >= ATTACHMENTS[index].min_rec;
}

// windows[30] Select Attachment (v10.js:214299): None, then Game.attach by minRec; a locked one has
// no onclick, "Req lvl 20" and the lock card, an open one with a minRec says "Unlocked".
static char *gen_attach(void) {
    const int cls = menu_ui()->client->menu_class_index;
    StrBuf out = {0};
    sb_appendf(&out, "<div id='skinList'><div class='noBtnCard blackShad' style='height:150px;border:5px solid lightgrey' onclick='selectAttachment(-1)'>%s<div class='skinRandom blackShad' style='top: 60px;'>X</div></div>",
               I18N("generic.none", "None"));
    if (class_primary_has_attach()) {
        for (int i = 0; i < ATTACH_COUNT; i++) {
            const int open = attach_unlocked(i, cls);
            char img[256], path[64], own[64];
            snprintf(path, sizeof(path), "textures/previews/attach/attach_%d.png", i);
            asset(path, img, sizeof(img));
            if (!ATTACHMENTS[i].min_rec) {
                snprintf(own, sizeof(own), "%s", I18N("generic.default", "Default"));
            } else if (open) {
                snprintf(own, sizeof(own), "%s", I18N("windows.secondary.unlocked", "Unlocked"));
            } else {
                char lvl[16];
                snprintf(lvl, sizeof(lvl), "%d", ATTACHMENTS[i].min_rec);
                I18N_F(own, "windows.secondary.req", "Req lvl {0}", lvl);
            }
            sb_append(&out, "<div class='noBtnCard blackShad' style='height:150px;border:5px solid lightgrey'");
            if (open) sb_appendf(&out, " onclick='selectAttachment(%d)'", i);
            sb_appendf(&out, ">%s<div class='itemOwn'>%s</div><img loading='lazy' draggable='false' class='noBtnImgA' src='%s' />%s</div>",
                       ATTACHMENTS[i].name, own, img,
                       open ? "" : "<div class='lockedCard'><span class='material-icons' style='font-size: 70px;color:#fff;'>lock</span></div>");
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
    const char *items = strstr(tpl, "<div class=\"wcpItems\">"); // the v10 page's capture is wrapped in the shell
    if (items) tpl = items + strlen("<div class=\"wcpItems\">");
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
            char path[64], url[256];
            snprintf(path, sizeof(path), "textures/reticles/reticle_%d.png", i);
            asset(path, url, sizeof(url));
            sb_appendf(&out, "<div class='noBtnCard blackShad' style='height:140px;border:5px solid lightgrey' onclick='selectReticle(%d)'>%s<div class='itemOwn' style='margin-top:1px'>%s</div><img loading='lazy' class='noBtnImgR' draggable='false' src='%s' onmouseenter='playTick()' /></div>",
                       i, RETICLES[i], I18N("generic.default", "Default"), url);
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
            sb_appendf(&out, "<div class='noBtnCard blackShad' style='height:140px;border:5px solid lightgrey' onclick='selectScope(%d)'>%s<div class='itemOwn' style='margin-top:1px'>%s</div><img loading='lazy' class='noBtnImgR' draggable='false' src='%s' onmouseenter='playTick()' /></div>",
                       i, SCOPES[i].name, I18N("generic.default", "Default"), img);
        }
    }
    sb_append(&out, "</div>");
    return sb_finish(&out);
}

// windows[44] Select Streaks: the streak list (by slot, then kills) and the three slots.
static char *gen_streaks(void) {
    const AccountInfo *acc = active_account();
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
// default sprays, then v10's other free sprays by store index; then the owned store sprays by
// rarity), with their frame animations.
static char *gen_sprays(void) {
    StrBuf style = {0};
    int frames_seen[16], frames_n = 0;
    pick_begin(PICKER_SPRAY, "selectSpray", "", 4);
    pick_add(PICK_RANDOM, -2, 0);
    for (int i = 0; i < SPRAYS_BASE_COUNT; i++) {
        if (BASE_SPRAYS[i].id == 88 && frames_n < 16) { // Verb: 4 frames x 250 ms
            sb_appendf(&style, "@keyframes sprayAnm%d {100%%{background-position:-%dpx}}", 4, 4 * 110);
            frames_seen[frames_n++] = 4;
        }
        pick_add(PICK_FREE_SPRAY, i, 0);
    }
    int n;
    OwnedSkin *owned = owned_skins(&n, 1);
    // v10's other free sprays (Poisn, Chwoubaka, ...), then the owned ones; a free spray an
    // account owns (offline mode: all) is listed once, with the free ones
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < (pass ? n : g_viewer_skin_count); i++) {
            const int ind = pass ? owned[i].ind : i;
            const ViewerSkin *s = store_skin(ind);
            if (!s || s->type != 4 || (pass ? free_spray(s) : !free_store_spray(ind))) continue;
            if (s->frames) { // UTILS.getFrameAnim(b7, "sprayAnm") and its @keyframes
                int seen = 0;
                for (int k = 0; k < frames_n; k++) seen |= frames_seen[k] == s->frames;
                if (!seen && frames_n < 16) {
                    sb_appendf(&style, "@keyframes sprayAnm%d {100%%{background-position:-%dpx}}", s->frames, s->frames * 110);
                    frames_seen[frames_n++] = s->frames;
                }
            }
            pick_add(PICK_OWNED, ind, pass ? owned[i].cnt : 1);
        }
    }
    free(owned);
    char *st = sb_finish(&style);
    StrBuf out = {0};
    if (st[0]) sb_appendf(&out, "<style>%s</style>", st);
    free(st);
    sb_append(&out, ITEM_SEARCH_HEAD("2", ", &quot;noBtnCard&quot;") "<div id='skinList' style='overflow-y: scroll;height: calc(100% - 95px);'>");
    pick_list(&out);
    sb_append(&out, "</div>");
    return sb_finish(&out);
}

// The owned list changed (a paint applied): every chooser that lists owned cards shows it again (the badges).
void ui_loadout_owned_changed(void) {
    static const int ids[] = {15, 16, 17, 26, 34, 38, 39, 40, 43, 44, 46, 50, 51, 52};
    for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) menu_rerender_if_open(ids[i]);
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
        case 35: return v10_news_changelog_gen(); // v10_news.c
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
    if (r >= 0 && r < RETICLE_COUNT) {
        char path[64];
        snprintf(path, sizeof(path), "textures/reticles/reticle_%d.png", r);
        asset(path, src, sizeof(src));
    } else if (r == -2) html_storage_get("savedReticle", src, sizeof(src));
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

// selectClass's windows[2].switchTab(1, true): updateWindow(3) re-renders the window only when it
// is the one showing (the comp menu's class picker selects with no window open)
void ui_loadout_select_tab(const int tab) {
    g_tab = tab >= 0 && tab <= 3 ? tab : 1;
    if (menu_ui()->active_window == 3) menu_show_window(3, 1);
}

// The handlers that stop the event (f1822(window.event) in v7: charmSelector,
// viewAttachments, windows[n].clear, remove*) sit inside a .custContainer whose own onclick
// opens a picker: they act at once and stop the event (html_ui_stop_propagation), so the
// container's handler never runs.
typedef enum { DEFER_SHOW, DEFER_CLEAR } DeferKind;
static void view_item(int index, int mode);
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

// UTILS.compareString(a, b): either contains the other, case-insensitively.
static int compare_string(const char *a, const char *b) {
    if (!a || !b) return 0;
    char la[256], lb[256];
    snprintf(la, sizeof(la), "%s", a);
    snprintf(lb, sizeof(lb), "%s", b);
    for (char *c = la; *c; c++) *c = (char) tolower((unsigned char) *c);
    for (char *c = lb; *c; c++) *c = (char) tolower((unsigned char) *c);
    return strstr(la, lb) != NULL || strstr(lb, la) != NULL;
}

// v7.1.4 searchItems' test of one card (data-index = store index).
static int search_match(const PickEntry *e, const char *q) {
    if (e->kind == PICK_FREE_SPRAY) return compare_string(q, BASE_SPRAYS[e->ind].name); // KrunkNative's free sprays: by name
    const ViewerSkin *s = e->kind == PICK_OWNED ? store_skin(e->ind) : NULL;
    if (!q[0] || !s) return 1;
    const cJSON *j = skin_json(e->ind);
    if (compare_string(q, cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j, "keyW")))) return 1;
    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "drop")) && compare_string(q, "drops")) return 1;
    if (compare_string(q, cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j, "limT")))) return 1;
    const LoadoutWeapon *w = s->weapon ? weapon_at(s->weapon - 1) : NULL;
    if (w && compare_string(q, w->name)) return 1;
    if (compare_string(q, s->name)) return 1;
    const cJSON *c;
    cJSON_ArrayForEach(c, cJSON_GetObjectItemCaseSensitive(j, "creators")) if (compare_string(q, cJSON_GetStringValue(c))) return 1;
    return 0;
}

// searchItems(input, cardClass): v7.1.4 hides the #skinList cards past the first two that do not
// match (keyword, "drops", limited tag, weapon, name, creators); here the grid shows the rest.
static void search_items(const HtmlEvent *ev) {
    char q[128] = "";
    HtmlElem input = html_dom_by_id("itemSearch");
    if (input) html_dom_get_value(input, q, sizeof(q));
    else if (ev && ev->value) snprintf(q, sizeof(q), "%s", ev->value);
    if (!g_pick.view) return;
    g_pick.n_view = 0;
    for (int i = 0; i < g_pick.n_all; i++) {
        if (i < 2 || search_match(&g_pick.all[i], q)) g_pick.view[g_pick.n_view++] = i;
    }
    vgrid_reset(&g_pick.grid, g_pick.n_view);
}

// The open picker's grid follows the scroll position (client_menu_tick).
void ui_loadout_tick(void) {
    v10_customize_tick(); // the v10 page's preview column follows a drag
    if (g_pick.view) vgrid_tick(&g_pick.grid);
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
    if (!strcmp(name, "viewItem")) {
        view_item(harg_int(args, argc, 0, -1), harg_int(args, argc, 1, 0));
        return 1;
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
    if (!strcmp(name, "searchItems")) { search_items(ev); return 1; }
    // windows[n].clear(): the Customize tab's custClear "x" (f1822 + select*(-1, true, true))
    if (!strncmp(name, "windows.", 8)) {
        const char *dot = strchr(name + 8, '.');
        if (dot && !strcmp(dot, ".clear")) {
            defer(DEFER_CLEAR, atoi(name + 8));
            return 1;
        }
    }
    // item tooltips (ui_tooltip.c); favourites and the item view are store features KrunkNative leaves out
    if (!strcmp(name, "createTip") || !strcmp(name, "cancelTip") || !strcmp(name, "createStreakTip") || !strcmp(name, "createSpinHistoryTip")) return ui_tooltip_handle(name, ev, args, argc); // ui_tooltip.c
    if (!strcmp(name, "favItem")) return 1;
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

void ui_loadout_reset_pops(void) { reset_pops(); }

// clearPops()
void ui_loadout_clear_pops(void) {
    ui_dialog_cancel(); // a pending confirm/prompt (ui_dialog.c) counts as "Cancel"
    reset_pops();
    HtmlElem holder = html_dom_by_id("popupHolder");
    if (holder) html_dom_set_style(holder, "z-index", "2147483647");
    set_disp("popupHolder", "none");
    menu_schedule_rect();
}

// store.tags: the limT badge colours (null ones fall back to #41c300)
static const char *store_tag_color(const char *tag) {
    static const struct { const char *name, *color; } tags[] = {
        {"Debug", "#000000"}, {"Twitch", "#6441a5"}, {"Charity", "#6441a5"}, {"Raid", "#eb347a"}, {"Canteen", "#fe4002"},
        {"Burg", "#ADE4F9"}, {"Sandstorm", "#ADE4F9"}, {"Littletown", "#ADE4F9"}, {"Site", "#ADE4F9"}, {"Industry", "#ADE4F9"},
        {"Subzero", "#ADE4F9"}, {"Freight", "#ADE4F9"}, {"Undergrowth", "#ADE4F9"}, {"Lostworld", "#ADE4F9"}, {"Kanji", "#ADE4F9"},
        {"Citadel", "#ADE4F9"}, {"Oasis", "#ADE4F9"}, {"Evacuation", "#ADE4F9"}, {"Craftable", "#cc51f8"}, {"NFT", "#f3b8ff"},
        {"Ranked", "#ff5500"}, {"Halloween", "#ff5500"},
        {"FRVR", "linear-gradient(132deg, rgba(0,189,255,1) 0%, rgba(155,0,255,1) 25%, rgba(255,0,41,1) 50%, rgba(255,150,3,1) 75%, rgba(255,199,0,1) 100%)"},
    };
    for (size_t i = 0; tag && i < sizeof(tags) / sizeof(tags[0]); i++) {
        if (!strcmp(tags[i].name, tag)) return tags[i].color;
    }
    return "#41c300";
}

// account.getSkinCount(index): the cnt of acc.skins' entry (0 for a guest / offline)
static int account_skin_count(const int index) {
    const AccountInfo *acc = active_account();
    const cJSON *skins = acc ? cJSON_GetArrayItem(acc->data, 10) : NULL;
    const cJSON *e;
    cJSON_ArrayForEach(e, skins) {
        const cJSON *ind = cJSON_GetObjectItem(e, "ind"), *cnt = cJSON_GetObjectItem(e, "cnt");
        if (cJSON_IsNumber(ind) && ind->valueint == index) return cJSON_IsNumber(cnt) ? cnt->valueint : 1;
    }
    return 0;
}

static int json_truthy(const cJSON *item) {
    return item && !cJSON_IsFalse(item) && !cJSON_IsNull(item) && !(cJSON_IsNumber(item) && item->valuedouble == 0.0) &&
           !(cJSON_IsString(item) && !item->valuestring[0]);
}

// viewItem(index, mode) (v7 index.js): the item popup over the menu; its 3D showcase is the
// viewer page's in an iframe (viewer.html?nobg&showcase=<index>, drawn natively by
// viewer_embed.h); a mode or an unknown item opens the sales page instead.
static void view_item(const int index, const int mode) {
    html_ui_stop_propagation(); // f1822(window.event)
    const ViewerSkin *skin = store_skin(index);
    if (!skin || mode) {
        char url[96];
        snprintf(url, sizeof(url), "/social.html?p=itemsales&i=%d", index);
        if (!app_route_url_target(url, 1)) ui_open_external(url); // openSales
        return;
    }
    const int count = account_skin_count(index);
    reset_pops();
    const HtmlElem pop = html_dom_by_id("itemViewPop");
    if (!pop) return;
    const int animate = skin->rarity == 6; // store.rarities[r].animate (Unobtainable)
    const char *color = skin->rarity >= 0 && skin->rarity < g_viewer_rarity_count ? g_viewer_rarities[skin->rarity].color : NULL;
    char border[96];
    snprintf(border, sizeof(border), "5px solid %s", color ? color : "lightgrey");
    html_dom_set_display(pop, "block");
    html_dom_set_style(pop, "border", border);
    html_dom_set_class(pop, "rainbow", animate);

    const cJSON *json = skin_json(index);
    const char *lim_t = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json, "limT"));
    StrBuf sb = {0};
    sb_appendf(&sb, "<div class='itemViewName%s%s' style='color:%s;'>", skin->rarity == 5 ? "" : " blackShad",
               rarity_anim(skin) ? " rainbow" : "", color ? color : "#fff"); // 5: CONTRABAND
    sb_append_esc(&sb, skin->name ? skin->name : "");
    sb_append(&sb, "</div>");
    const int vaulted = (skin->seas ? skin->seas : 1) != g_viewer_current_season ||
                        (json_truthy(cJSON_GetObjectItemCaseSensitive(json, "limited")) && json_truthy(cJSON_GetObjectItemCaseSensitive(json, "noSpin")) &&
                         !json_truthy(cJSON_GetObjectItemCaseSensitive(json, "itemShop")));
    if (json_truthy(cJSON_GetObjectItemCaseSensitive(json, "blackM"))) {
        sb_append(&sb, "<div class='itemViewSea' style='background:#00ffff'>Black Market</div>");
    } else if (lim_t && lim_t[0]) {
        sb_appendf(&sb, "<div class='itemViewSea' style='background:%s'>", store_tag_color(lim_t));
        sb_append_esc(&sb, lim_t);
        sb_append(&sb, "</div>");
    } else if (vaulted) {
        sb_append(&sb, "<div class='itemViewSea'>Vaulted</div>");
    } else if (json_truthy(cJSON_GetObjectItemCaseSensitive(json, "limited")) || (skin->flags & VS_BLOCKED)) {
        sb_append(&sb, "<div class='itemViewSea' style='background:#faa332;'>Limited</div>");
    }
    if (skin->type == 16) { // PLAYER_CARD: its thumbnail (UTILS.getPreview)
        char path[256], url[320];
        viewer_skin_preview_path(skin, path, sizeof(path));
        html_dom_asset_url(path, url, sizeof(url));
        sb_appendf(&sb, "<div class='viewItemCard' style='background-image:url(%s)'></div>", url);
    } else {
        char paint[32]; // the owned copy's paint on the showcase (v10's instance → viewer URL, deob:123160-123173)
        ui_loadout_paint_query(index, paint, sizeof(paint));
        sb_appendf(&sb, "<iframe style='width: 100%%;height: 450px;border:none;margin-top:10px;' src='/viewer.html?nobg&showcase=%d%s'></iframe>", index, paint);
    }
    sb_appendf(&sb, "<div class='itemViewCnt'>x%d <span style='color:#ffffff8f;'>Owned</span></div><div id='itemInfoBtn' onclick='openSales(%d)'>More Information</div>", count, index);
    char *html = sb_finish(&sb);
    html_dom_set_html(pop, html);
    free(html);
    menu_schedule_rect(); // the class preview's hole follows the popup backdrop
}

// v10 _7291: the officers' tabs; switchKPDTab(i) asks the server for list i + 1 (policeRes code i + 1)
// for the first three. Demos and Reviewers are v10 web API lists (kpd.listDemos / kpd.reviewers)
// that KrunkNative has no backend for: their tab says so in #kpdCalls (v10's request-failed path).
static const char *const KPD_TABS[] = {"Active Calls", "Ban Logs", "Stats", "Demos", "Reviewers"};
#define KPD_TAB_COUNT ((int) (sizeof(KPD_TABS) / sizeof(KPD_TABS[0])))
static int g_kpd_tab; // _71731
static int g_kpd_demo_off;      // _71738: the Demos page's offset (v10 kpdDemoPage steps 30 suspects)
static char g_kpd_demo_q[24];   // _71739: the Demos search (a player's name, "" none)
#define KPD_DEMO_PAGE 30
#define KPD_APPLY_REPORTS 20 // v10: an application from 20 positive reports (and a rating above 0.7)

// v10's _67362 for the K.P.D: the account the game server knows. In an official krunker.io game that is
// the krunker.io account its server signed in on the socket ("a", krunker_net_account), or a guest: our
// own account means nothing to krunker.io's K.P.D, whose lists only its officers get (an officer view
// for ours would ask that server for lists it never sends: "Loading..." for good). Elsewhere it is ours.
typedef struct {
    int signed_in;
    int officer;                    // v10 isMod || isDeputy
    int deputy;                     // isDeputy (classN "Deputy")
    int job_rating, job_rating_pos; // jobRating / jobRatingPos
} PoliceIdentity;

static PoliceIdentity police_identity(void) {
    PoliceIdentity id = {0};
    if (krunker_net_active()) {
        KrunkerNetAccount k;
        if (krunker_net_account(&k)) {
            id.signed_in = 1;
            id.officer = k.role == 2 || k.role == 3; // updatePerms: isMod = role 2, isDeputy = role 3
            id.deputy = k.role == 3;
            id.job_rating = k.job_rating;
            id.job_rating_pos = k.job_rating_pos;
        }
        return id;
    }
    const AccountInfo *acc = active_account();
    if (!acc) return id;
    id.signed_in = 1;
    // KrunkNative's officers are the moderator and DEV accounts (server/src/votekick.c kpd_is_officer)
    id.officer = acc->is_mod || acc->is_dev;
    id.job_rating = acc->job_rating;
    id.job_rating_pos = acc->job_rating_pos;
    return id;
}

int ui_police_officer(void) { return police_identity().officer; }

const char *ui_police_class_name(void) {
    const PoliceIdentity id = police_identity(); // v10 classN: isDeputy ? Deputy : isMod ? Marshal : Officer
    return id.deputy ? "Deputy" : id.officer ? "Marshal" : "Officer";
}

int ui_loadout_kpd_tab(void) { return g_kpd_tab; }

void ui_loadout_switch_kpd_tab(const int tab) {
    g_kpd_tab = tab >= 0 && tab < KPD_TAB_COUNT ? tab : 0;
    ui_loadout_show_police_pop();
}

// showPolicePop() (v10 :215977): Officer Buddy in the class preview (classN "Officer", "Marshal" for an
// officer), the speech line for a guest (1), a player (0: "You look Strong...") or an officer (3:
// "Welcome Back Officer..."), and the K.P.D text. An officer gets v10's officer view: the Mod Rules
// line, the tab bar (switchKPDTab) and #kpdCalls, filled by policeRes (hud_votekick.c) for the tab.
// A player sees its Helpful Reports bar (jobRatingPos / jobRating) and the application line (20
// positive reports needed). Who is an officer, a player or a guest is police_identity's: in an official
// krunker.io game the account that game's server signed in (a guest there sees v10's guest view).
void ui_loadout_show_police_pop(void) {
    reset_pops();
    HtmlElem holder = html_dom_by_id("popupHolder");
    if (holder) html_dom_set_style(holder, "z-index", "20");
    set_disp("policePop", "block");
    MenuUi *ui = menu_ui();
    g_police_saved = ui->client->menu_class_index; // reset_pops put the player's own class back first
    g_police_class = POLICE_CLASS;
    ui->client->menu_class_index = POLICE_CLASS; // dI.class, not saved
    refresh_class_box();
    audio_play("ambient_9", 0.14f, 1, 1.0f);
    if (g_police_timer) html_timer_cancel(g_police_timer);
    g_police_timer = html_timer_add(rand_between(15000, 30000), police_idle, NULL);
    const PoliceIdentity id = police_identity();
    const PoliceIdentity *acc = id.signed_in ? &id : NULL;
    const int officer = id.officer;
    police_say(officer ? "Welcome Back Officer. Let's see who needs our help today..."
               : acc   ? "You look Strong. Why don't you join us?"
                       : "Sign In to Join the Fight against the Hackers!");
    HtmlElem c = html_dom_by_id("policePopC");
    if (c) {
        StrBuf sb = {0};
        sb_append(&sb, "<div style='font-size:30px;margin-bottom:12px;color:#858585'>Krunker Police Department</div>");
        if (officer) sb_appendf(&sb, "<div class='kpdRank'>%s</div>", id.deputy ? "Deputy" : "Marshal"); // v10 role 3 / 2
        if (officer) {
            sb_append(&sb, "<div style='margin-top:0px;color:rgba(0,0,0,0.6)'>When responding to a <b>Distress Call</b>, please remember to "
                           "follow the <a href='./docs/kpd.txt' class='menuLink' style='font-size:15px'>Mod Rules</a></div>"
                           "<div class='menuTabsNew' style='margin-top:10px;table-layout: fixed;'>");
            for (int i = 0; i < KPD_TAB_COUNT; i++)
                sb_appendf(&sb, "<div class='menuTabNew%s' onclick='switchKPDTab(%d)'>%s</div>", g_kpd_tab == i ? " tabANew" : "", i, KPD_TABS[i]);
            sb_append(&sb, "</div><div id='kpdCalls'>Loading...</div>");
        } else {
            // v10: the guide (the Player List key, click here, the K.P.D icon), the Helpful Reports bar
            // (jobRatingPos / jobRating, coloured by (pos || 1) / (all || 1)) and the application line
            char key[32];
            const double ratio = acc ? (acc->job_rating_pos ? acc->job_rating_pos : 1) / (double) (acc->job_rating ? acc->job_rating : 1) : 0.0;
            const char *col = ratio <= 0.5 ? "#fc3232" : ratio <= 0.7 ? "#f7d31e" : "#63de26";
            ui_controls_key_name(ui_controls_bind(UI_CONTROL_PLAYER_LIST), key, sizeof(key));
            sb_append(&sb, "<div style='color:white;font-size: 23px;'>If you suspect that there is a cheater in your current lobby you can call a member of K.P.D to investigate.<div style='height:15px'></div>Press <span style='color:#fbc02d'>[");
            sb_append_esc(&sb, key);
            sb_append(&sb, "]</span> or <a href='#' onclick='showWindow(23);'>click here</a> to open the Player List.<div style='height:30px'></div>"
                           "On the player list, click the K.P.D icon next to the player's name. A member of K.P.D will join your game and take action if needed."
                           "<div style='height:15px'></div><img class='kpdGuideImg' src='./img/guide/kpd_new.png'/><div style='height:15px'></div>"
                           "Any positive reports will result in punishment for the cheater, and you will be granted KR as a reward.</div><div style='height:30px'></div>");
            if (acc)
                sb_appendf(&sb, "<div id='trustProg'><div id='trustProgHD'>%d/%d Helpful Reports</div><div id='trustPrgHD' style='width:%g%%;background-color:%s'></div></div>",
                           acc->job_rating_pos, acc->job_rating, 100.0 * ratio, col);
            sb_append(&sb, "<div style='margin-top:20px;margin-bottom:20px;font-size:25px;color:white;'>");
            if (!acc) sb_append(&sb, "Sign-in to apply for K.P.D");
            else if (acc->job_rating_pos >= KPD_APPLY_REPORTS && ratio > 0.7)
                sb_append(&sb, "You may qualify to become a K.P.D officer. Submit a application <a href='https://forms.gle/1xy2LznPkYn9gh8Q7' target='_blank' style='color:#fff'>here</a>");
            else sb_append(&sb, "When you reach 20 positive reports, you can apply to become part of K.P.D and gain rewards!");
            sb_append(&sb, "</div>");
        }
        char *html = sb_finish(&sb);
        html_dom_set_html(c, html);
        free(html);
    }
    if (officer) { // the tab's list: "kpd" tab + 1 (Active Calls 1, Ban Logs 2, Stats 3, Demos 4, Reviewers 5); the answer fills #kpdCalls
        if (g_kpd_tab < 3) hud_votekick_kpd_request(ui->client, g_kpd_tab + 1);
        else if (g_kpd_tab == 3) hud_votekick_kpd_list(ui->client, 4, g_kpd_demo_off, g_kpd_demo_q); // listDemos({off, q})
        else hud_votekick_kpd_list(ui->client, 5, 0, ""); // reviewers()
    }
    menu_schedule_rect();
}

// genericPopup(html, cls)
static void kpd_generic_popup(const char *html, const char *cls) {
    ui_loadout_clear_pops();
    set_disp("popupHolder", "block");
    HtmlElem pop = html_dom_by_id("genericPop");
    if (!pop) return;
    html_dom_set_attr(pop, "class", cls);
    html_dom_set_attr(pop, "style", "");
    html_dom_set_html(pop, html);
    html_dom_set_display(pop, "block");
}

// v10 _69540(null, text): the error response popup (responsePop, a red clear icon)
void ui_loadout_response_popup(const char *text) {
    StrBuf sb = {0};
    sb_append(&sb, "<div style='width:100%;text-align:center'><div><i class='material-icons' style='font-size:80px;color:#e33b3b'>clear</i></div>"
                   "<div style='font-size:20px;color:rgba(255,255,255,0.5)'>");
    sb_append_esc(&sb, text && text[0] ? text : "Request Failed");
    sb_append(&sb, "</div></div>");
    char *html = sb_finish(&sb);
    kpd_generic_popup(html, "responsePop");
    free(html);
}

void ui_loadout_kpd_demo_query(int *off, const char **q) {
    if (off) *off = g_kpd_demo_off;
    if (q) *q = g_kpd_demo_q;
}

// kpdDemoSearch(clear) (v10 :216202): the search box's text trimmed (none when cleared), page 1 again
void ui_loadout_kpd_demo_search(const int clear) {
    char text[64] = "";
    const HtmlElem input = html_dom_by_id("kpdDemoQ");
    if (!clear && input) html_dom_get_value(input, text, sizeof(text));
    const char *start = text;
    while (*start == ' ' || *start == '\t') start++;
    size_t len = strlen(start);
    while (len && (start[len - 1] == ' ' || start[len - 1] == '\t')) len--;
    if (len >= sizeof(g_kpd_demo_q)) len = sizeof(g_kpd_demo_q) - 1;
    memcpy(g_kpd_demo_q, start, clear ? 0 : len);
    g_kpd_demo_q[clear ? 0 : len] = 0;
    g_kpd_demo_off = 0;
    ui_loadout_show_police_pop();
}

// kpdDemoPage(delta) (v10 :216208): 30 suspects back or on, never below the first page
void ui_loadout_kpd_demo_page(const int delta) {
    const int off = g_kpd_demo_off + KPD_DEMO_PAGE * delta;
    g_kpd_demo_off = off > 0 ? off : 0;
    ui_loadout_show_police_pop();
}
