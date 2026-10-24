// Lookups over the generated store tables (viewer_store_table.c).
#include "viewer_store.h"
#include <stdio.h>
#include <string.h>

// The viewer's dev config folders edit store.skins entries in place (viewer.js f453 / f454): a
// skin being edited is a mutable copy every lookup returns instead of the table row.
#define VIEWER_MAX_EDITS 32
static struct {
    int count;
    int skin_index[VIEWER_MAX_EDITS];
    ViewerSkin skin[VIEWER_MAX_EDITS];
    int dye_count;
    ViewerDye dye[VIEWER_MAX_EDITS];
} g_edits;

// v10.0.3 "sdc" SKIN_DEFINITION_CHANGED (deob-10.0.3.js:261708-261714): the server replaced store.skins[skinId] at
// run time (client/src/viewer/viewer_skin_define.c parses it); the definition stands in for the table row from then
// on, under the dev edits. An index past the table is a new item: viewer_skin finds it, the lists built from the
// table (g_viewer_skin_count, the option lists) do not.
#define VIEWER_MAX_DEFINES 256
static struct {
    int count;
    int skin_index[VIEWER_MAX_DEFINES];
    ViewerSkin skin[VIEWER_MAX_DEFINES];
} g_defines;

static const ViewerSkin *table_skin(const int index) {
    for (int i = 0; i < g_defines.count; i++) {
        if (g_defines.skin_index[i] == index) return &g_defines.skin[i];
    }
    return index >= 0 && index < g_viewer_skin_count ? &g_viewer_skins[index] : NULL;
}

const ViewerSkin *viewer_skin(const int index) {
    for (int i = 0; i < g_edits.count; i++) {
        if (g_edits.skin_index[i] == index) return &g_edits.skin[i];
    }
    return table_skin(index);
}

ViewerSkin *viewer_skin_define_slot(const int index) {
    if (index < 0 || index > VIEWER_SKIN_INDEX_MAX) return NULL;
    for (int i = 0; i < g_defines.count; i++) {
        if (g_defines.skin_index[i] == index) return &g_defines.skin[i];
    }
    if (g_defines.count >= VIEWER_MAX_DEFINES) return NULL;
    g_defines.skin_index[g_defines.count] = index;
    memset(&g_defines.skin[g_defines.count], 0, sizeof(g_defines.skin[0]));
    return &g_defines.skin[g_defines.count++];
}

ViewerSkin *viewer_skin_edit(const int index) {
    for (int i = 0; i < g_edits.count; i++) {
        if (g_edits.skin_index[i] == index) return &g_edits.skin[i];
    }
    const ViewerSkin *row = table_skin(index);
    if (!row || g_edits.count >= VIEWER_MAX_EDITS) return NULL;
    g_edits.skin_index[g_edits.count] = index;
    g_edits.skin[g_edits.count] = *row;
    return &g_edits.skin[g_edits.count++];
}

static const ViewerDye *table_dye(const int index);

const ViewerDye *viewer_dye(const int index) {
    for (int i = 0; i < g_edits.dye_count; i++) {
        if (g_edits.dye[i].index == index) return &g_edits.dye[i];
    }
    return table_dye(index);
}

ViewerDye *viewer_dye_edit(const int index) {
    for (int i = 0; i < g_edits.dye_count; i++) {
        if (g_edits.dye[i].index == index) return &g_edits.dye[i];
    }
    const ViewerSkin *row = table_skin(index);
    if (!row || row->type != 5 || g_edits.dye_count >= VIEWER_MAX_EDITS) return NULL;
    ViewerDye *dye = &g_edits.dye[g_edits.dye_count++];
    const ViewerDye *base = table_dye(index);
    if (base) *dye = *base;
    else *dye = (ViewerDye) {.index = index};
    return dye;
}

void viewer_skin_edits_clear(void) {
    memset(&g_edits, 0, sizeof(g_edits));
}

static const ViewerDye *table_dye(const int index) {
    int lo = 0, hi = g_viewer_dye_count;
    while (lo < hi) {
        const int mid = lo + (hi - lo) / 2;
        if (g_viewer_dyes[mid].index < index) lo = mid + 1;
        else hi = mid;
    }
    return lo < g_viewer_dye_count && g_viewer_dyes[lo].index == index ? &g_viewer_dyes[lo] : NULL;
}

const ViewerCharmMount *viewer_charm_mount(const int weapon_id) {
    return weapon_id >= 0 && weapon_id < VIEWER_CHARM_WEAPONS && g_viewer_charm_mounts[weapon_id].set ? &g_viewer_charm_mounts[weapon_id] : NULL;
}

// The altSkins of v10.0.3's two weapon tables that swap the model (both keep v7.1.4's, src/data/weapons.js and
// upgradeWeapons.js). weapons (deob-10.0.3.js:112381-115372): 5 Shotgun "Nova Pump" (:112928-112949), 7 Semi Auto
// "Nova Semi" (:113147-113164), 9 Akimbo Uzi "Nova Uzi" (:113362-113371) and 11 Alien Blaster "Nova Alien"
// (:113548-113568). upgradeWeapons (deob:199770-202673): 5, 7 and 9 the same remodels plus {projection: true, video: 2,
// outline: 0, outlineS: 1.05} (:200322-200353, :200559-200584, :200784-200801) and 11 Cupids Blaster {mid: 4, seas: 6,
// weapon: 12, rarity: 0} (:201026-201030). Every other upgraded altSkin is only that video and outline; no other weapon
// has one. The video and the outline are not kept here: skin_video_alt_skin and zombies_render.c draw them.
#define ALT_SKIN(nm, wp, md, md_t, ss, fl) .name = nm, .type = 0, .weapon = wp, .id = -1, .rarity = 0, .seas = ss, .pat = -1, \
                                           .mid = md, .mid_t = md_t, .flags = fl, .emiss = -1, .color = -1
static const ViewerAltSkin k_alt_nova_pump = {
    .skin = {ALT_SKIN("Nova Pump", 6, 0, 38, 2, VS_BLOCKED), .y_off = -0.5f, .z_off = 0.28f, .m_scl = 1.0f, .scl = 0.9f},
    .charm_m = {-0.37f, -0.2f, 1.18f},
};
static const ViewerAltSkin k_alt_nova_semi = {
    .skin = {ALT_SKIN("Nova Semi", 8, 0, 0, 3, VS_BLOCKED)},
    .charm_m = {-0.29999999999999993f, -0.35000000000000014f, 1.13f},
};
static const ViewerAltSkin k_alt_nova_uzi = {.skin = {ALT_SKIN("Nova Uzi", 10, 0, 0, 3, VS_BLOCKED)}};
static const ViewerAltSkin k_alt_nova_alien = {
    .skin = {ALT_SKIN("Nova Alien", 12, 5, 0, 7, VS_BLOCKED), .y_off = -0.2f, .m_scl = 1.5f},
    .charm_m = {-0.51f, -0.14f, -1.2500000000000013f},
    .no_snd = 1,
};
static const ViewerAltSkin k_alt_cupids_blaster = {.skin = {ALT_SKIN(NULL, 12, 4, -1, 6, 0)}}; // no name, not blocked
#undef ALT_SKIN

const ViewerAltSkin *viewer_weapon_alt_skin(const int weapon_id, const int upgraded) {
    switch (weapon_id) {
        case 5: return &k_alt_nova_pump;
        case 7: return &k_alt_nova_semi;
        case 9: return &k_alt_nova_uzi;
        case 11: return upgraded ? &k_alt_cupids_blaster : &k_alt_nova_alien;
        default: return NULL;
    }
}

const ViewerOptionList *viewer_option_list(const ViewerListKind kind, const int key, const int season) {
    for (int i = 0; i < g_viewer_list_count; i++) {
        const ViewerOptionList *list = &g_viewer_lists[i];
        if (list->kind == kind && list->key == key && list->season == season) return list;
    }
    return NULL;
}

void viewer_option_label(const ViewerOptionList *list, const int i, char *buf, const size_t n) {
    const ViewerSkin *skin = list && i >= 0 && i < list->count ? viewer_skin(list->pairs[i * 2]) : NULL;
    if (!skin) {
        snprintf(buf, n, "Default");
        return;
    }
    const char *rarity = (skin->flags & VS_FREE) ? "Free" : skin->rarity >= 0 && skin->rarity < g_viewer_rarity_count ? g_viewer_rarities[skin->rarity].name : "";
    snprintf(buf, n, "%s [%s]", skin->name, rarity);
}

int viewer_option_value(const ViewerOptionList *list, const int i) {
    return list && i >= 0 && i < list->count ? list->pairs[i * 2 + 1] : -1;
}

const ViewerPreview *viewer_preview(const ViewerSkin *skin) {
    static const ViewerPreview none;
    if (!skin) return &none;
    if (skin->mid >= 0)
        for (int i = 0; i < g_viewer_preview_remodel_count; i++)
            if (g_viewer_preview_remodels[i].weapon == skin->weapon && g_viewer_preview_remodels[i].mid == skin->mid)
                return &g_viewer_preview_remodels[i].preview;
    return skin->weapon >= 0 && skin->weapon < VIEWER_PREVIEW_SLOTS ? &g_viewer_previews[skin->weapon] : &none;
}

int viewer_skin_tex_truthy(const ViewerSkin *skin) {
    return skin->tex && skin->tex[0] && !((skin->flags & VS_TEX_NUM) && !strcmp(skin->tex, "0"));
}

void viewer_melee_keys(const ViewerSkin *skin, char *model, const size_t model_n, char *texture, const size_t texture_n) {
    const int id = skin && skin->id > 0 ? skin->id : 0;
    snprintf(model, model_n, "melee/melee_%d", id);
    if (skin && (skin->flags & VS_PAT) && skin->tex) snprintf(texture, texture_n, "%s", skin->tex);
    else if (skin && viewer_skin_tex_truthy(skin)) snprintf(texture, texture_n, "melee/melee_%d_%s", id, skin->tex);
    else snprintf(texture, texture_n, "%s", model);
}

// v10 getPreview(skin, store.types) (v10.js:24328) without assetsUrl(): the asset-relative path
//   textures/<types[type]><id>.<ext>                                     sprays (4), player cards (16), emoji (17), paints (18)
//   textures/previews/cosmetics/<assetType || type>_<id>[_c<pat> | _<tex>].<ext>   other cosmetics
//   textures/previews/melee/melee_<id>[_c<pat> | _<tex>].<ext>           melee (3)
//   textures/previews/weapons/weapon_<weapon>_<m<mid>[_<midT>] | c<pat> | <tex> | <id>>.<ext>
// with ext = prevExt || texExt || "png". (v3.1.1 kept 3D sprays under previews/cosmetics; v7 has none.)
void viewer_skin_preview_path(const ViewerSkin *skin, char *path, const size_t n) {
    const int type = skin->type;
    const char *types = type >= 0 && type < g_viewer_type_count ? g_viewer_types[type] : "";
    const char *ext = skin->prev_ext ? skin->prev_ext : skin->tex_ext ? skin->tex_ext : "png";

    if (type == 4 || type == 16 || type == 17 || type == 18) {
        if (skin->id_name) snprintf(path, n, "textures/%s%s.%s", types, skin->id_name, ext);
        else snprintf(path, n, "textures/%s%d.%s", types, skin->id, ext);
    } else if (type && type != 3) {
        const int dir = skin->asset_type ? skin->asset_type : type;
        if (skin->pat >= 0) snprintf(path, n, "textures/previews/cosmetics/%d_%d_c%d.%s", dir, skin->id, skin->pat, ext);
        else if (viewer_skin_tex_truthy(skin)) snprintf(path, n, "textures/previews/cosmetics/%d_%d_%s.%s", dir, skin->id, skin->tex, ext);
        else snprintf(path, n, "textures/previews/cosmetics/%d_%d.%s", dir, skin->id, ext);
    } else if (type == 3) {
        if (skin->pat >= 0) snprintf(path, n, "textures/previews/%s%d_c%d.%s", types, skin->id, skin->pat, ext);
        else if (skin->tex) snprintf(path, n, "textures/previews/%s%d_%s.%s", types, skin->id, skin->tex, ext);
        else snprintf(path, n, "textures/previews/%s%d.%s", types, skin->id, ext);
    } else if (skin->mid >= 0) {
        if (skin->mid_t >= 0) snprintf(path, n, "textures/previews/%s%d_m%d_%d.%s", types, skin->weapon, skin->mid, skin->mid_t, ext);
        else snprintf(path, n, "textures/previews/%s%d_m%d.%s", types, skin->weapon, skin->mid, ext);
    } else if (skin->pat >= 0) {
        snprintf(path, n, "textures/previews/%s%d_c%d.%s", types, skin->weapon, skin->pat, ext);
    } else if (viewer_skin_tex_truthy(skin)) {
        snprintf(path, n, "textures/previews/%s%d_%s.%s", types, skin->weapon, skin->tex, ext);
    } else {
        snprintf(path, n, "textures/previews/%s%d_%d.%s", types, skin->weapon, skin->id, ext);
    }
}

// utils.js getPreviewExport(): the last path component of getPreview().
void viewer_preview_export_name(const ViewerSkin *skin, char *buf, const size_t n) {
    char path[256];
    viewer_skin_preview_path(skin, path, sizeof(path));

    const char *slash = strrchr(path, '/');
    snprintf(buf, n, "%s", slash ? slash + 1 : path);
}
