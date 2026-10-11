#include <stddef.h>
#include <weapons.h>

// Same order as Krunker v3.1.1's weapon table (27.js / src/data/weapons.js): the index is the weapon id.
const Weapon *g_weapons[WEAPON_COUNT] = {
    &g_awp, &g_ak47, &g_pistol, &g_smg, &g_revolver,
    &g_shotgun, &g_lmg, &g_semi_auto, &g_rpg,
    &g_akimbo, &g_deagle, &g_alien_blaster, &g_knife,
    &g_crossbow, &g_famas, &g_sawed_off, &g_auto_pistol,
    &g_bomb, &g_blaster,
};

const WeaponAttachment g_attachments[ATTACHMENT_COUNT] = {
    {.name = "Reflex Sight", .src = "attach_0", .aim_offset_y = 0.16f, .scale = 1.2f},
};

const WeaponAttachment *weapon_attachment(const Weapon *weapon, const int attach_index) {
    if (!weapon || !weapon->has_attach) return NULL;

    const int index = weapon->force_attach ? 0 : attach_index;
    return index >= 0 && index < ATTACHMENT_COUNT ? &g_attachments[index] : NULL;
}
