#pragma once
// Krunker v10.0.3 badges (config badges, deob-10.0.3.js:115767-116206): 68 entries over 35 icon ids.
// Indices 0-28 are v7.1.4's src/data/badges.js in its order (only the Skin Creator I-IV gained
// `claimable` in v10); 29-67 are the ones v10 added after it (VIP .. Season 10 Contributor). The
// account holds the earned indices (player_badge); the selected one is stats.bdg
// (PlayerCosmeticsV7.badge on the wire, -1 none / hidden by the customBadge privacy setting). Shown
// on nametags, the side and Tab leaderboards, the menu account header and the profile popup.
#include <stddef.h>

#define BADGE_COUNT 68

// The first icon id that is not part of the asset tree (assets/textures/badges holds v7.1.4's
// badge_0 .. badge_17): v10's later icons are page images under ui/img/badges (fetched from the asset
// host, ui/README.md). v10 loads every badge from textures/badges/ on the asset host (deob-10.0.3.js:116208);
// KrunkNative ships the newer ones with the page instead.
#define BADGE_PAGE_ICON_MIN 18

typedef struct {
    const char *name;
    int id;          // icon id: badge_<id>[_<tex>].png
    int tex;         // 0 = none
    const char *desc;
    unsigned char claimable; // claimable: earned from the account's facts (badge_rule_met), claimed in the menu
} BadgeDef;

extern const BadgeDef g_badges[BADGE_COUNT];

// The badge of `index` (store order), NULL when out of range.
const BadgeDef *badge_get(int index);
// The icon of `index`: "textures/badges/badge_<id>[_<tex>].png" (relative to the asset root) for the
// v7.1.4 icons, "img/badges/badge_<id>[_<tex>].png" (the page's own tree) from BADGE_PAGE_ICON_MIN on;
// "" when out of range.
const char *badge_texture(int index, char *out, size_t size);
