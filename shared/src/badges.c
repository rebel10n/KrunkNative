// Krunker v7.1.4 src/data/badges.js, in index order (the index is what accounts store).
#include <badges.h>
#include <stdio.h>

const BadgeDef g_badges[BADGE_COUNT] = {
    {"Legacy Challenger", 0, 0, "Completed Old Challenge Lvl 30", 0},
    {"Skin Creator I", 1, 0, "50 skins added to the game", 0},
    {"Skin Creator II", 1, 1, "100 skins added to the game", 0},
    {"Skin Creator III", 1, 2, "200 skins added to the game", 0},
    {"Skin Creator IV", 1, 3, "300 skins added to the game", 0},
    {"Deputy", 2, 0, "", 0},
    {"Officer", 2, 1, "", 0},
    {"Sergeant", 2, 2, "", 0},
    {"Lieutenant", 2, 3, "", 0},
    {"Captain", 2, 4, "", 0},
    {"Major", 2, 5, "", 0},
    {"Nuclear I", 3, 0, "Call in 500 Nukes", 1},
    {"Translators", 7, 0, "Contributors of the Translation Project", 0},
    {"Nuclear II", 3, 1, "Call in 750 Nukes", 1},
    {"Nuclear III", 3, 2, "Call in 1000 Nukes", 1},
    {"Nuclear IV", 3, 3, "Call in 1500 Nukes", 1},
    {"Gratitude I", 8, 0, "Contributing to the development of krunker", 0},
    {"Gratitude II", 8, 1, "Contributing to the development of krunker", 0},
    {"Gratitude III", 8, 2, "Contributing to the development of krunker", 0},
    {"Map Maker I", 9, 0, "", 0},
    {"Map Maker II", 9, 1, "", 0},
    {"Map Maker III", 9, 2, "", 0},
    {"Season 6 Founder", 10, 0, "Purchase the Battle Pass in the first week", 1},
    {"S6 Map Master", 12, 0, "Collect all Season 6 map rewards", 1},
    {"S6 Battle Pass Master", 14, 0, "Complete the Battle Pass in Season 6", 1},
    {"S6 Badge Master", 13, 0, "Obtain all Season 6 badges", 1},
    {"Season 7 Founder", 15, 0, "Purchase the Battle Pass in the first week", 1},
    {"S7 Battle Pass Master", 16, 0, "Complete the Battle Pass in Season 7", 1},
    {"S7 Badge Master", 17, 0, "Obtain all Season 7 badges", 1},
};

const BadgeDef *badge_get(const int index) {
    return index >= 0 && index < BADGE_COUNT ? &g_badges[index] : NULL;
}

const char *badge_texture(const int index, char *out, const size_t size) {
    const BadgeDef *b = badge_get(index);
    if (!b) snprintf(out, size, "%s", "");
    else if (b->tex) snprintf(out, size, "textures/badges/badge_%d_%d.png", b->id, b->tex);
    else snprintf(out, size, "textures/badges/badge_%d.png", b->id);
    return out;
}
