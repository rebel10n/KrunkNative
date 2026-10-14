#include <stddef.h>
#include <weapons.h>

// Krunker v10.0.2's weapon table (doc/v10/gameplay.md section 5; v10.js weaponConfig v10:98982):
// ids 0-28 keep v7.1.4's order and meaning, with the ten rebalanced entries replaced by their v10
// copies (weapons_rebalance_v10.c), and 29 Boulder / 30 Compressor are new (weapons_new_v10.c).
// Every other entry is the v7 definition itself: the extractor diff (~/.cache/kn-v10/ext/
// diff-weapons.txt) finds no other changed field, and shared/tests/weapons_v10_test.c checks the
// whole table against shared/tests/data/v10/weapons.json.
const Weapon *const g_weapons_v10[WEAPON_TABLE_COUNT] = {
    &g_awp, &g_ak47_v10, &g_pistol_v10, &g_smg_v10, &g_revolver,
    &g_shotgun, &g_lmg_v10, &g_semi_auto_v10, &g_rpg,
    &g_akimbo_v10, &g_deagle_v10, &g_alien_blaster, &g_knife,
    &g_crossbow, &g_famas_v10, &g_sawed_off, &g_auto_pistol,
    &g_bomb, &g_blaster_v10,
    &g_build_tool, &g_grappler, &g_tehchy9_v10, &g_noob_tube, &g_slimer,
    &g_zapper, &g_minigun, &g_war_machine, &g_akimbo_pistol, &g_charge_rifle,
    [WEAPON_ID_BOULDER] = &g_boulder,
    [WEAPON_ID_COMPRESSOR] = &g_compressor,
    [WEAPON_ID_TOOL_GUN] = &g_tool_gun, // KrunkNative (decisions.md "Tool Gun")
    [WEAPON_ID_PISS] = &g_piss,         // KrunkNative (decisions.md "Piss")
    [WEAPON_ID_PHYS_GUN] = &g_phys_gun, // KrunkNative (object_manager.h OM_ACTION_EDIT)
    [WEAPON_ID_DRANK] = &g_drank,               // KrunkNative (decisions.md "Purple Drank")
    [WEAPON_ID_MONEY_SPREAD] = &g_money_spread, // KrunkNative (decisions.md "Money Spread")
};
