#pragma once
// Krunker v7.1.4 cosmetic items on the player model (players.js generateMeshes, playerMesh.js):
// the store items of each slot, looked up in the client's store table (game.store.skins,
// client/src/viewer/viewer_store_table.c, generated from assets/skins.json), and the look a
// player sends in its ENT. client/src/player_mesh.c builds the meshes.
#include <stddef.h>
#include <stdint.h>
#include <shared.h>

struct ViewerSkin;

// v7 src/data/itemTypes.js (previews.json): store.skins[i].type
typedef enum {
    ITEM_WEAPON = 0, ITEM_HAT = 1, ITEM_BODY = 2, ITEM_MELEE = 3, ITEM_SPRAY = 4, ITEM_DYE = 5, ITEM_WAIST = 6,
    ITEM_FACE = 7, ITEM_SHOE = 8, ITEM_PET = 9, ITEM_COLLECTIBLE = 10, ITEM_WRIST = 11, ITEM_CHARM = 12,
    ITEM_TICKET = 13, ITEM_BACK = 14, ITEM_HEAD = 15, ITEM_PLAYER_CARD = 16, ITEM_TYPE_COUNT
} ItemType;

// store.skins[store_index] when it is an item of `type`, else NULL (-1, out of range, other types).
const struct ViewerSkin *cosmetic_item(int store_index, ItemType type);

// players.js generateMeshData src / texSrc: "<store.types[assetType || type]><id>" and, with a
// truthy tex, "<that>_<tex>" (texture = model otherwise). E.g. "hats/hat_12", "body/body_4_1".
void cosmetic_asset_keys(const struct ViewerSkin *item, char *model, size_t model_n, char *texture, size_t texture_n);

// The look the player model wears: each index moves to the slot of its item's type, so v3.1.1
// placements keep working (a v7 HEAD item in hatIndex, a BACK item in bodyIndex: KrunkNative
// inventories and saved picks from before the split); an index of a type no slot takes is dropped.
// An explicit pick in the right slot wins over a moved one.
void cosmetics_route(PlayerCosmetics *cosmetics, PlayerCosmeticsV7 *v7);

// The sight a loadout mounts (v7.1.4 enterGame ent[12] and updateClassPreview's attachIndex): the
// pick, but 0 (the Reflex Sight) when the primary's skin has forceAttach. A weapon's own forceAttach
// is weapon_attachment()'s. One rule for the ENT, the menu preview and our own viewmodel.
int cosmetics_attach_index(int primary_skin, int attach_pick);

// players.js generateMeshes overrides: game.config.noCosm clears every slot and the skin colour;
// the Juggernaut wears items 4908-4911 (body, head, shoes, waist).
#define COSMETIC_JUGGERNAUT_BODY 4908
#define COSMETIC_JUGGERNAUT_HEAD 4909
#define COSMETIC_JUGGERNAUT_SHOES 4910
#define COSMETIC_JUGGERNAUT_WAIST 4911
