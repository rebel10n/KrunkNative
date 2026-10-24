// Krunker v10.0.3 config badges (deob-10.0.3.js:115767-116206), in index order (the index is what
// accounts store): 0-28 are v7.1.4's src/data/badges.js, 29-67 the badges v10 added after them.
// claimable is v10's flag; the rules are badge_rule_met (shared/src/progression.c).
#include <badges.h>
#include <stdio.h>

const BadgeDef g_badges[BADGE_COUNT] = {
    {"Legacy Challenger", 0, 0, "Completed Old Challenge Lvl 30", 0},
    {"Skin Creator I", 1, 0, "50 skins added to the game", 1},
    {"Skin Creator II", 1, 1, "100 skins added to the game", 1},
    {"Skin Creator III", 1, 2, "200 skins added to the game", 1},
    {"Skin Creator IV", 1, 3, "300 skins added to the game", 1},
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
    // v10.0.3 additions (deob-10.0.3.js:116012-116205)
    {"VIP", 18, 0, "", 0},
    {"Wanna be Bug Catcher", 19, 0, "Successfully Report a Major Bug/Glitch/Exploit.", 0},
    {"Just a Bug Catcher", 19, 1, "Successfully Report 10 Major Bug/Glitch/Exploit.", 0},
    {"\"The\" Bug Catcher", 19, 2, "Successfully Report 30 Major Bug/Glitch/Exploit.", 0},
    {"Advanced Bug Catcher", 19, 3, "Successfully Report 50 Major Bug/Glitch/Exploit.", 0},
    {"Pro Bug Catcher", 19, 4, "Successfully Report 100 Major Bug/Glitch/Exploit.", 0},
    {"Master Bug Catcher", 19, 5, "True Team Player Bug Catcher! 500 Wow!", 0},
    {"Wanna be 3D Modeler", 20, 0, "Successfully create a Custom 3D Model.", 0},
    {"Just a 3D Modeler", 20, 1, "Successfully create 10 Custom 3D Model.", 0},
    {"\"The\" 3D Modeler", 20, 2, "Successfully create 50 Custom 3D Model.", 0},
    {"Advanced 3D Modeler", 20, 3, "Successfully create 100 Custom 3D Model.", 0},
    {"Pro 3D Modeler", 20, 4, "Successfully create 200 Custom 3D Model.", 0},
    {"Master 3D Modeler", 20, 5, "True Custom Skin 3D Modeler! 500! WoW", 0},
    {"Wanna be Texturer", 21, 0, "Successfully Texture a Skin.", 0},
    {"Just a Texturer", 21, 1, "Successfully Texture 50 Skins.", 0},
    {"\"The\" Texturer", 21, 2, "Successfully Texture 100 Skins.", 0},
    {"Advanced Texturer", 21, 3, "Successfully Texture 300 Skins.", 0},
    {"Pro Texturer", 21, 4, "Successfully Texture 500 Skins.", 0},
    {"Master Texturer", 21, 5, "True Custom Skin 3D Modeler! 1000 Skins! Wow!", 0},
    {"FRVR Tournament Participant", 22, 0, "Participate in a FRVR Tournament.", 0},
    {"FRVR Tournament Winner", 22, 1, "1st Place on FRVR Tournament.", 0},
    {"FRVR Tournament 2nd Place", 22, 2, "2nd Place on FRVR Tournament.", 0},
    {"FRVR Tournament 3rd Place", 22, 3, "3rd Place on FRVR Tournament.", 0},
    {"Map Making Event Winner", 23, 0, "1st Place on Map Making Event.", 0},
    {"Skin Making Event Winner", 23, 1, "1st Place on Skin Making Event.", 0},
    {"Spray Making Event Winner", 23, 2, "1st Place on Spray Making Event.", 0},
    {"Map Making Event Runner-Up", 24, 0, "Runner-Up on Map Making Event.", 0},
    {"Skin Making Event Runner-Up", 24, 1, "Runner-Up on Skin Making Event.", 0},
    {"Spray Making Event Runner-Up", 24, 2, "Runner-Up on Spray Making Event.", 0},
    {"Tournament Caster", 25, 0, "Officially be a Tournament Caster", 0},
    {"Season 9 Contributor", 26, 0, "Thank you for helping Season 9 and 9.1 with content.", 0},
    {"KrunkCup Badge", 27, 0, "From the World KrunkCup 2026 Bundle", 1},
    {"S10 Battle Pass", 28, 0, "Get to level 60 on the Season 10 Battle Pass.", 1},
    {"S10 Supreme Battle Pass", 29, 0, "Get to level 70 on the Season 10 Battle Pass.", 1},
    {"S10 Master Battle Pass", 30, 0, "Complete the Season 10 Battle Pass.", 1},
    {"Community Event Winner", 31, 0, "For the Winner of a community event.", 0},
    {"Bastion Raid Tester", 32, 0, "For those who tested and helped on Bastion Raid.", 0},
    {"Bastion Raid Creator", 33, 0, "Guess who did it?!", 0},
    {"Season 10 Contributor", 34, 0, "For everyone that helped Season 10 in any shape or form! Thank you!", 0},
};

const BadgeDef *badge_get(const int index) {
    return index >= 0 && index < BADGE_COUNT ? &g_badges[index] : NULL;
}

const char *badge_texture(const int index, char *out, const size_t size) {
    const BadgeDef *b = badge_get(index);
    // v10: textures/badges/badge_<id>[_<tex>].png on the asset host (deob-10.0.3.js:116208); the icons from
    // BADGE_PAGE_ICON_MIN on ship in the page tree (ui/img/badges), not in assets/textures
    const char *dir = b && b->id >= BADGE_PAGE_ICON_MIN ? "img/badges" : "textures/badges";
    if (!b) snprintf(out, size, "%s", "");
    else if (b->tex) snprintf(out, size, "%s/badge_%d_%d.png", dir, b->id, b->tex);
    else snprintf(out, size, "%s/badge_%d.png", dir, b->id);
    return out;
}
