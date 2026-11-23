#pragma once
// Krunker v10.0.7 item copies on the players of a match: the metadata m of each worn copy (getPlayerAddData's skin
// entries {i, id, m}, read by iïîìîîï, deob-10.0.7.js:24335-24337). Box Office's copies (kn_brain skin_copies: 0352
// paint, 0398 wear and serial, 0401 name tag) keep:
//   wear    m.wear, 0 <= wear < 1. generateMeshData draws it with the item's "<type><id>_wear" map at level m.wear
//           (deob-10.0.7.js:173646-173651; the melee "melee/melee_<id>_wear" :174102-174107; a weapon skin's
//           "<weapon path>_wear" :174184-174189). The tiers (Flawless / Tested / Trashed / Wrecked) are :16478-16494.
//   serial  m.serial, the copy's mint number ("#n"; no model drawing reads it).
//   name    m.name, the copy's name tag: the HUD's weapon slot shows "<name>" in .weapNametag instead of the skin name
//           (deob-10.0.7.js:176440), the menu's .skinNametag / tooltips show it in quotes.
// Every player's worn copies come from the server (NET_EXT_KN_SKIN_META, sent with each SKINS of a player and after a
// mid-match inventory refresh: the owner's first copy of each worn store index, the plain-index rule of the paint and
// the style). Your own account outside a match (the menu's preview) reads the account's owned list instead.
//
// For skin_fx / player_mesh: skin_meta_wear(player->uid, store_index) is the level to draw the wear map at, -1 none.

typedef struct {
    float wear;    // m.wear, -1 = none
    int serial;    // m.serial, 0 = none
    char name[24]; // m.name, "" = none (UTF-8, NUL-terminated)
} SkinCopyMeta;

// 1 when player `uid` wears store item `skin` (a weapon skin, melee or cosmetic index) as a copy with metadata, and
// *out (may be NULL) gets it; 0 otherwise (no packet for that player, or that item is a plain unit), *out cleared.
int skin_meta_of(int uid, int skin, SkinCopyMeta *out);
// m.wear of that worn copy (0 <= wear < 1), or -1 when it has none (also -1 for an unknown player).
float skin_meta_wear(int uid, int skin);
// NET_EXT_KN_SKIN_META: player `uid`'s worn copies, replaced as a whole (count 0 clears them). skins / wears / serials
// / names are count entries; names[k] may be NULL.
void skin_meta_set(int uid, int count, const int *skins, const float *wears, const int *serials, const char *const *names);
void skin_meta_reset(void); // a new connection: every player's metadata is gone
