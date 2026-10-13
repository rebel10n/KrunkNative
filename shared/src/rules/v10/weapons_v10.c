#include <stddef.h>
#include <weapons.h>

// Krunker v10.0.2's weapon table (doc/v10/gameplay.md section 5): ids 0-28 as v7.1.4, 29 Boulder
// and 30 Compressor are new. TODO(v10-gameplay): for now a copy of v7's table (the same Weapon
// definitions); the v10 balance pass and the two new secondaries go in here (v10 copies of the
// changed Weapon entries live in this directory).
const Weapon *const g_weapons_v10[WEAPON_TABLE_COUNT] = {
    &g_awp, &g_ak47, &g_pistol, &g_smg, &g_revolver,
    &g_shotgun, &g_lmg, &g_semi_auto, &g_rpg,
    &g_akimbo, &g_deagle, &g_alien_blaster, &g_knife,
    &g_crossbow, &g_famas, &g_sawed_off, &g_auto_pistol,
    &g_bomb, &g_blaster,
    &g_build_tool, &g_grappler, &g_tehchy9, &g_noob_tube, &g_slimer,
    &g_zapper, &g_minigun, &g_war_machine, &g_akimbo_pistol, &g_charge_rifle,
    // 29 Boulder, 30 Compressor: TODO(v10-gameplay)
    [WEAPON_ID_TOOL_GUN] = &g_tool_gun, // KrunkNative (decisions.md "Tool Gun")
    [WEAPON_ID_PISS] = &g_piss,         // KrunkNative (decisions.md "Piss")
    [WEAPON_ID_PHYS_GUN] = &g_phys_gun, // KrunkNative (object_manager.h OM_ACTION_EDIT)
};
