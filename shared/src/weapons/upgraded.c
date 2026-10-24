#include <string.h>
#include <weapons.h>

// Krunker v7.1.4 src/data/upgradeWeapons.js: a second 29-entry table indexed like weapons.js
// (`upgraded: true`), used for a loadout slot a krumMachine upgraded (player.upgrades[slot]).
// Each entry is its base weapon plus the gameplay fields below (checked against v10.0.3's table,
// shared/tests/data/v10/upgradeWeapons.json, by weapons_v10_test). Upgrades drop minRec and
// canUpgrade (and the Alien Blaster's noComp, the Grappler's customOnly). Not ported: the cosmetic altSkin / muzID / charm
// changes, magReserve (finiteAmmo modes), and the Cupids Blaster's funcOnHit.

static Weapon g_upgraded[WEAPON_COUNT];
static int g_upgraded_ready;

static const vec2 k_charge_rifle_spread[] = { // cSpread [[-0.27, 0.02, 0.03, -0.31, 0.3, -0.01], [-0.17, -0.22, -0.3, 0.31, 0.32, 0.28]]
    {-0.27f, 0.02f}, {0.03f, -0.31f}, {0.3f, -0.01f},
    {-0.17f, -0.22f}, {-0.3f, 0.31f}, {0.32f, 0.28f},
};

static void burst(Weapon *w, const unsigned int count, const float rate) {
    w->burst = 1;
    w->burst_count = count;
    w->burst_rate = rate;
}

// The base entries: v10.0.3's upgradeWeapons table (deob-10.0.3.js:199770-202673) is v7.1.4's unchanged, independent
// of the v10 balance pass (its Zombie Assaulter keeps dmgDrop 5 and the Pistol swapTime 260), so it
// builds on the v7.1.4 definitions (shared/src/weapons/), not on the ruleset's rebalanced copies.
static const Weapon *const k_base[WEAPON_COUNT] = {
    &g_awp, &g_ak47, &g_pistol, &g_smg, &g_revolver,
    &g_shotgun, &g_lmg, &g_semi_auto, &g_rpg,
    &g_akimbo, &g_deagle, &g_alien_blaster, &g_knife,
    &g_crossbow, &g_famas, &g_sawed_off, &g_auto_pistol,
    &g_bomb, &g_blaster,
    &g_build_tool, &g_grappler, &g_tehchy9, &g_noob_tube, &g_slimer,
    &g_zapper, &g_minigun, &g_war_machine, &g_akimbo_pistol, &g_charge_rifle,
};

static void build(void) {
    const Weapon *const *base = k_base;
    for (int i = 0; i < WEAPON_COUNT; i++) {
        g_upgraded[i] = *base[i];
        Weapon *w = &g_upgraded[i];
        w->sound = "weapon_upgraded"; // every upgraded weapon shares one shot sound
        w->min_rec = 0;
        w->can_upgrade = 0;
    }
    Weapon *w;
    g_upgraded[11].no_comp = 0;     // the Alien Blaster's noComp goes; the other noComp / customOnly stay
    g_upgraded[20].custom_only = 0; // Grappler
    w = &g_upgraded[0]; w->name = "AWP Blaster"; w->ammo = 12; w->rate = 0.28f; w->reload_time = 1.2f; w->min_spread = 20.0f; burst(w, 2, 0.1f);
    w = &g_upgraded[1]; w->name = "Zombie Assaulter"; w->ammo = 35; w->damage = 28.0f; w->rate = 0.09f;
    w = &g_upgraded[2]; w->ammo = 16; w->rate = 0.1f;
    w = &g_upgraded[3]; w->name = "UMPire"; w->ammo = 44; w->damage = 24.0f; w->rate = 0.08f;
    w = &g_upgraded[4]; w->ammo = 12; burst(w, 3, 0.09f);
    w = &g_upgraded[5]; w->ammo = 6; burst(w, 3, 0.06f);
    w = &g_upgraded[6]; w->name = "NORU"; w->damage = 22.0f; w->rate = 0.1f;
    w = &g_upgraded[7]; w->ammo = 12; w->rate = 0.1f;
    w = &g_upgraded[8]; w->name = "Dream Wrecker"; w->ammo = 9; w->speed_mlt = 0.95f; w->no_auto = 0;
    w->projectile_config.damage = 60.0f; w->projectile_config.explode = 45.0f; w->projectile_config.explode_damage = 45.0f;
    w = &g_upgraded[9]; w->ammo = 30; w->damage = 18.0f;
    w = &g_upgraded[10]; w->ammo = 3; w->damage = 80.0f; w->rate = 0.35f;
    w = &g_upgraded[11]; w->name = "Cupids Blaster";
    w = &g_upgraded[13]; w->ammo = 3; // explosive bolts
    w->projectile_config = (ProjectileConfig) {.speed = 0.75f, .gravity = 0.00008f, .range = 1500.0f, .damage = 100.0f, .explode = 100.0f,
                                               .explode_damage = 100.0f, .no_impact = 1};
    w = &g_upgraded[14]; w->ammo = 36; w->burst_count = 4;
    w = &g_upgraded[15]; w->ammo = 3; burst(w, 3, 0.1f);
    w = &g_upgraded[16]; w->ammo = 18; w->rate = 0.07f;
    w = &g_upgraded[18]; w->headshot_mlt = 1.5f; w->rate = 0.11f; w->reload_time = 1.1f;
    w = &g_upgraded[21]; w->ammo = 18; w->burst_rate = 0.08f;
    w = &g_upgraded[22]; w->ammo = 2; w->projectile_config.damage = 50.0f; w->projectile_config.explode = 45.0f; w->projectile_config.explode_damage = 40.0f;
    w = &g_upgraded[24]; w->ammo = 10; w->damage = 12.0f;
    w = &g_upgraded[27]; w->ammo = 15; w->rate = 0.09f;
    w = &g_upgraded[28]; w->shots = 3; w->custom_spread = k_charge_rifle_spread; w->custom_spread_count = 6;
    g_upgraded_ready = 1;
}

const Weapon *weapon_upgraded(const int id) {
    if (id < 0 || id >= WEAPON_COUNT) return NULL;
    if (!g_upgraded_ready) build(); // idempotent: a racing second build writes the same values
    return &g_upgraded[id];
}
