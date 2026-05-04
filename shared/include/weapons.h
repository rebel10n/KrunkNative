#pragma once

typedef struct {
    const char *name;
    const char *src;
    const char *icon;

    unsigned char secondary:1;
    unsigned char no_spread:1;
    unsigned char akimbo:1;
    unsigned char no_aim:1;
    unsigned char no_auto:1;
    unsigned char melee:1;
    unsigned char equipment:1;

    unsigned int ammo;

    float swap_time;
    float reload_time;
    float aim_speed;
    float speed_mlt;
    float jump_mlt;
    float damage;
    float headshot_mlt;
    float pierce;
    float range;
    float drop_start;
    float damage_drop;
    float rate;
    float spread;
    float min_spread;
    float recoil;
    float recoil_y;
    float recover;
    float recover_y;
    float recover_f;
    float zoom;
} Weapon;

extern const Weapon g_awp;
extern const Weapon g_ak47;
extern const Weapon g_pistol;
extern const Weapon g_smg;
extern const Weapon g_revolver;
extern const Weapon g_shotgun;
extern const Weapon g_lmg;
extern const Weapon g_semi_auto;
extern const Weapon g_rpg;
extern const Weapon g_akimbo;
extern const Weapon g_deagle;
extern const Weapon g_alien_blaster;
extern const Weapon g_knife;

extern const Weapon *g_weapons[13];