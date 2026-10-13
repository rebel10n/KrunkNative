#include <stddef.h>
#include <weapons.h>

// The weapon tables are the rulesets' (shared/src/rules/<ver>/weapons_<ver>.c, ruleset.h).

int weapon_id_from_legacy(const int id, const int krunker_weapon_count) {
    // ids 29-31 were the KN weapons until the ruleset seam; a ruleset whose Krunker table reaches
    // 29 (v10: Boulder, Compressor) means its own weapon by them
    if (id >= WEAPON_ID_LEGACY_KN_BASE && id < WEAPON_ID_LEGACY_KN_BASE + (WEAPON_TABLE_COUNT - WEAPON_ID_KN_BASE) &&
        id >= krunker_weapon_count) return WEAPON_ID_KN_BASE + (id - WEAPON_ID_LEGACY_KN_BASE);
    return id;
}

// v7.1.4 src/data/attach.js. The Acog's `noVertM` has no reader; its `req` is the per-class rank
// gate (minRec 20 on stats c<classIndex>), left to the loadout UI.
const WeaponAttachment g_attachments[ATTACHMENT_COUNT] = {
    {.name = "Reflex Sight", .src = "attach_0", .aim_offset_y = 0.16f, .scale = 1.2f, .exp_scale = 12.0f},
    {.name = "Acog Scope", .src = "attach_1", .aim_offset_y = 0.22f, .scale = 1.1f, .zoom_mlt = 1.35f, .z_recoil = 0.9f,
     .z_offset = 0.35f, .exp_scale = 8.0f, .min_rec = 20},
};

const WeaponAttachment *weapon_attachment(const Weapon *weapon, const int attach_index) {
    if (!weapon || !weapon->has_attach) return NULL;

    const int index = weapon->force_attach ? 0 : attach_index;
    return index >= 0 && index < ATTACHMENT_COUNT ? &g_attachments[index] : NULL;
}

const ScopeOverlay g_scope_overlays[SCOPE_OVERLAY_COUNT] = {
    {"Default", 0}, {"Pro", 1}, {"OG", 2}, {"Waspy", 4}, {"MoonJuice", 5}, {"Swixie", 6}, {"Sorable", 7},
};

// attach.js Acog yOff: {1: 0.01, 14: -0.01} (Assault Rifle, Famas)
float weapon_attachment_y_offset(const WeaponAttachment *attachment, const int weapon_id) {
    if (attachment != &g_attachments[ATTACHMENT_ACOG]) return 0.0f;
    return weapon_id == 1 ? 0.01f : weapon_id == 14 ? -0.01f : 0.0f;
}

// players.js updateAim: RENDER.zoom(1 + (weapon.zoom * (attach.zoomMlt || 1) - 1) * aim * adsFov)
float weapon_aim_zoom(const Weapon *weapon, const WeaponAttachment *attachment) {
    if (!weapon) return 1.0f;
    const float mlt = attachment && attachment->zoom_mlt > 0.0f ? attachment->zoom_mlt : 1.0f;
    return weapon->zoom * mlt;
}
