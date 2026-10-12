#pragma once
// Krunker v7.1.4 badges (src/data/badges.js): 29 entries over 17 icon ids. The account holds the
// earned indices (player_badge); the selected one is stats.bdg (PlayerCosmeticsV7.badge on the
// wire, -1 none / hidden by the customBadge privacy setting). Shown on nametags, the side and
// Tab leaderboards and the menu account header.
#include <stddef.h>

#define BADGE_COUNT 29

typedef struct {
    const char *name;
    int id;          // icon id: textures/badges/badge_<id>[_<tex>].png
    int tex;         // 0 = none
    const char *desc;
    unsigned char claimable; // claimable: earned from stats (Nuclear, seasons), claimed in the menu
} BadgeDef;

extern const BadgeDef g_badges[BADGE_COUNT];

// The badge of `index` (store order), NULL when out of range.
const BadgeDef *badge_get(int index);
// "textures/badges/badge_<id>[_<tex>].png" (relative to the asset root); "" when out of range.
const char *badge_texture(int index, char *out, size_t size);
