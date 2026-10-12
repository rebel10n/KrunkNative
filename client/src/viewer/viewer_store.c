// Lookups over the generated store tables (viewer_store_table.c).
#include "viewer_store.h"
#include <stdio.h>
#include <string.h>

const ViewerSkin *viewer_skin(const int index) {
    return index >= 0 && index < g_viewer_skin_count ? &g_viewer_skins[index] : NULL;
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

const ViewerPreview *viewer_preview(const int weapon) {
    static const ViewerPreview none;
    return weapon >= 0 && weapon < VIEWER_PREVIEW_SLOTS ? &g_viewer_previews[weapon] : &none;
}

int viewer_skin_tex_truthy(const ViewerSkin *skin) {
    return skin->tex && skin->tex[0] && !((skin->flags & VS_TEX_NUM) && !strcmp(skin->tex, "0"));
}

// v7 utils.js getPreview(skin, store) without assetsUrl(): the asset-relative path
//   textures/<types[type]><id>.<ext>                                     sprays (4), player cards (16)
//   textures/previews/cosmetics/<assetType || type>_<id>[_<tex>].<ext>   other cosmetics
//   textures/previews/melee/melee_<id>[_c<pat> | _<tex>].<ext>           melee (3)
//   textures/previews/weapons/weapon_<weapon>_<m<mid>[_<midT>] | c<pat> | <tex> | <id>>.<ext>
// with ext = texExt || "png". (v3.1.1 kept 3D sprays under previews/cosmetics; v7 has none.)
void viewer_skin_preview_path(const ViewerSkin *skin, char *path, const size_t n) {
    const int type = skin->type;
    const char *types = type >= 0 && type < g_viewer_type_count ? g_viewer_types[type] : "";
    const char *ext = skin->tex_ext ? skin->tex_ext : "png";

    if (type == 4 || type == 16) {
        snprintf(path, n, "textures/%s%d.%s", types, skin->id, ext);
    } else if (type && type != 3) {
        const int dir = skin->asset_type ? skin->asset_type : type;
        if (viewer_skin_tex_truthy(skin)) snprintf(path, n, "textures/previews/cosmetics/%d_%d_%s.%s", dir, skin->id, skin->tex, ext);
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
