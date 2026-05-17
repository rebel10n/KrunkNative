#pragma once
#include <common.h>

typedef struct {
    const char *name;
    const char *src;
    const char *icon;

    unsigned char secondary:1;
    unsigned char no_spread:1;
    unsigned char akimbo:1;
    unsigned char no_aim:1;
    unsigned char no_auto:1;
    unsigned char no_inspect:1;
    unsigned char can_throw:1;
    unsigned char melee:1;
    unsigned char equipment:1;
    unsigned char bomb:1;
    unsigned char burst:1;
    unsigned char projectile:1;
    unsigned char projectile_disable:1;
    unsigned char animate_while_aiming:1;

    unsigned int burst_count;
    float burst_rate;

    unsigned int ammo;
    unsigned int shots;

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
    float recoil_r;
    float recover;
    float recover_y;
    float recover_f;
    float physical_power;
    float physical_range;
    float zoom;
    float inaccuracy;

    float recoil_y_mlt;
    float recoil_z;
    float recoil_z_mlt;
    float rotation;
    float z_rotation;
    float z_rotation_mlt;
    float z_lean_mlt;
    float y_rotation;
    float rotation_offset;
    float rotation_offset_aim;
    float jump_y_mlt;
    float land_bob;
    float aim_recoil_mlt;
    float lean_mlt;
    float inspect_rotation;
    float inspect_mlt;
    float crouch_lean;
    float crouch_rotation;
    float crouch_drop;
    float scale;

    float hold_width;
    float hold_distance_offset;
    vec3 origin;
    vec3 offset;
    vec3 left_hold;
    vec3 right_hold;

    const vec2 *custom_spread;
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