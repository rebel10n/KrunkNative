#include <stddef.h>
#include <weapons.h>

// Krunker v7.1.4's weapon table, in src/data/weapons.js order: the index is the weapon id.
// Ids 0-18 keep their v3.1.1 meaning; 19-28 are new in v7; 29-63 are empty; KrunkNative's own
// weapons sit at WEAPON_ID_KN_BASE (weapons.h).
const Weapon *const g_weapons_v7[WEAPON_TABLE_COUNT] = {
    &g_awp, &g_ak47, &g_pistol, &g_smg, &g_revolver,
    &g_shotgun, &g_lmg, &g_semi_auto, &g_rpg,
    &g_akimbo, &g_deagle, &g_alien_blaster, &g_knife,
    &g_crossbow, &g_famas, &g_sawed_off, &g_auto_pistol,
    &g_bomb, &g_blaster,
    &g_build_tool, &g_grappler, &g_tehchy9, &g_noob_tube, &g_slimer,
    &g_zapper, &g_minigun, &g_war_machine, &g_akimbo_pistol, &g_charge_rifle,
    [WEAPON_ID_TOOL_GUN] = &g_tool_gun, // KrunkNative (decisions.md "Tool Gun")
    [WEAPON_ID_PISS] = &g_piss,         // KrunkNative (decisions.md "Piss")
    [WEAPON_ID_PHYS_GUN] = &g_phys_gun, // KrunkNative (object_manager.h OM_ACTION_EDIT)
};
