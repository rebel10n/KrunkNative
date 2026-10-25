#include <cosmetics.h>
#include "viewer/viewer_store.h"
#include <stdio.h>
#include <string.h>

const ViewerSkin *cosmetic_item(const int store_index, const ItemType type) {
    const ViewerSkin *item = viewer_skin(store_index);
    return item && item->type == (int) type ? item : NULL;
}

void cosmetic_asset_keys(const ViewerSkin *item, char *model, const size_t model_n, char *texture, const size_t texture_n) {
    const int dir = item->asset_type ? item->asset_type : item->type;
    const char *prefix = dir >= 0 && dir < g_viewer_type_count ? g_viewer_types[dir] : "";
    snprintf(model, model_n, "%s%d", prefix, item->id > 0 ? item->id : 0);
    if (viewer_skin_tex_truthy(item)) snprintf(texture, texture_n, "%s_%s", model, item->tex);
    else snprintf(texture, texture_n, "%s", model);
    // a custom skin (generateMeshData, deob-10.0.3.js:193943-193944): customModel src "skins/<_id>/model", customTextures
    // texSrc "skins/<_id>/texture", both user assets (assetModel / assetTex, :193975-193976)
    if (item->custom_id && (item->flags & VS_CUSTOM_MODEL)) snprintf(model, model_n, VIEWER_USER_ASSET_KEY "skins/%s/model", item->custom_id);
    if (item->custom_id && (item->flags & VS_CUSTOM_TEXTURES)) snprintf(texture, texture_n, VIEWER_USER_ASSET_KEY "skins/%s/texture", item->custom_id);
    // no texSrc: loadMesh takes src as the texture, and without assetTex from textures/ (getMat, deob:232058, :231924)
    else if (item->custom_id && (item->flags & VS_CUSTOM_MODEL) && !viewer_skin_tex_truthy(item)) snprintf(texture, texture_n, "skins/%s/model", item->custom_id);
}

char *cosmetic_asset_path(const char *dir, const char *key, const char *ext) {
    char rel[320];
    if (!strncmp(key, VIEWER_USER_ASSET_KEY, sizeof(VIEWER_USER_ASSET_KEY) - 1)) snprintf(rel, sizeof(rel), "%s.%s", key, ext);
    else snprintf(rel, sizeof(rel), "%s/%s.%s", dir, key, ext);
    return concat(assets_path(), rel);
}

int cosmetics_attach_index(const int primary_skin, const int attach_pick) {
    const ViewerSkin *skin = cosmetic_item(primary_skin, ITEM_WEAPON);
    return skin && (skin->flags2 & VS2_FORCE_ATTACH) ? 0 : attach_pick;
}

// The slot (field) of each item type, NULL for types not worn on the model.
static int32_t *slot_of(PlayerCosmetics *c, PlayerCosmeticsV7 *v7, const int type) {
    switch (type) {
        case ITEM_HAT: return &c->hat;
        case ITEM_BODY: return &c->body;
        case ITEM_DYE: return &c->dye;
        case ITEM_WAIST: return &c->waist;
        case ITEM_FACE: return &c->face;
        case ITEM_SHOE: return &v7->shoe;
        case ITEM_PET: return &v7->pet;
        case ITEM_WRIST: return &v7->wrist;
        case ITEM_BACK: return &v7->back;
        case ITEM_HEAD: return &v7->head;
        case ITEM_PLAYER_CARD: return &v7->player_card;
        default: return NULL;
    }
}

void cosmetics_route(PlayerCosmetics *c, PlayerCosmeticsV7 *v7) {
    PlayerCosmetics in = *c;
    PlayerCosmeticsV7 in_v7 = *v7;
    PlayerCosmetics out = {.skin_color = c->skin_color, .hair_color = c->hair_color, -1, -1, -1, -1, -1};
    PlayerCosmeticsV7 out_v7 = PLAYER_COSMETICS_V7_NONE;
    out_v7.badge = v7->badge;

    // the item each slot holds, in slot order: right-typed picks first, then moved ones
    int32_t *fields[] = {&in.hat, &in.body, &in.waist, &in.face, &in.dye, &in_v7.head, &in_v7.back, &in_v7.shoe,
                         &in_v7.pet, &in_v7.wrist, &in_v7.player_card};
    const int field_type[] = {ITEM_HAT, ITEM_BODY, ITEM_WAIST, ITEM_FACE, ITEM_DYE, ITEM_HEAD, ITEM_BACK, ITEM_SHOE,
                              ITEM_PET, ITEM_WRIST, ITEM_PLAYER_CARD};
    const int n = (int) (sizeof(fields) / sizeof(fields[0]));
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < n; i++) {
            const ViewerSkin *item = viewer_skin(*fields[i]);
            if (!item || (item->type == field_type[i]) != (pass == 0)) continue;
            int32_t *slot = slot_of(&out, &out_v7, item->type);
            if (slot && *slot < 0) *slot = *fields[i];
        }
    }
    for (int k = 0; k < 2; k++) out_v7.charms[k] = cosmetic_item(v7->charms[k], ITEM_CHARM) ? v7->charms[k] : -1;
    *c = out;
    *v7 = out_v7;
}
