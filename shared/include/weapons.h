#pragma once
#include <common.h>

struct Player_t;

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
    unsigned char scope:1;        // JS `scope`: sniper reticle overlay + hide viewmodel while fully aimed
    unsigned char no_headshot:1;  // JS `noHeadShot`
    unsigned char has_rotation:1; // JS `rot` present: use `rotation` (x, y, z) for the weapon mesh instead of {0, PI/2, 0}
    unsigned char has_attach:1;   // JS `attach` present: takes the player's sight attachment (g_attachments)
    unsigned char force_attach:1; // JS `forceAttach`: always uses attachment 0

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
    vec3 rotation;
    float z_rotation;
    float z_rotation_mlt;
    float z_lean_mlt;
    float y_rotation;
    float rotation_offset;
    float rotation_offset_aim;
    float aim_offset_y;   // JS `aimOffY`: extra Y offset applied to the aim origin
    float jump_y_mlt;     // JS `jYMlt`: bob Y multiplier while aiming
    float jump_bob_y_mlt; // JS `jumpYM`: jump bob Y multiplier
    float land_bob;
    float aim_recoil_mlt;
    float lean_mlt;
    float inspect_rotation;
    float inspect_mlt;
    float inspect_x;      // JS `insX`: inspect swing target (0 = PI / 2.8)
    float swap_wiggle;    // JS `swapWiggle`: swap wiggle amplitude (0 = 0.6)
    float aim_lean;       // JS `aimLean`: viewmodel roll while aiming in / out (0 = 0.5)
    // JS `inspAnim` (players.js wInspect): first-person melee inspect pose the arm/weapon tween
    // toward over 400 ms (Exponential.Out). Order: arZ, arX, arY, apZ, apX, apY, wpZ, wpY, wrY.
    unsigned char has_inspect_anim:1;
    float inspect_anim[9];
    // JS `anim(player, TWEEN, type)` (players.js melee, local player only): tweens player->melee_anim.
    // type 1 = swing with the knife, 2 = fists (knife thrown), 3 = throw release. NULL: no anim.
    void (*melee_anim)(struct Player_t *player, int type);

    // JS `recoilAnim` (players.js shoot): the viewmodel kick tweens to these targets over
    // time * 0.15 (linear), then back to 0 over time * 0.95 (Back.Out). time 0 = `rate`;
    // aim_time replaces time while fully aimed. has_recoil_anim = 0: no kick targets.
    unsigned char has_recoil_anim:1;
    unsigned char recoil_anim_mask; // bit 0 Y, 1 R, 2 YM, 3 Z: targets present (others keep their value)
    float recoil_anim_time;         // seconds
    float recoil_anim_aim_time;     // seconds
    float recoil_tween_y;           // recoilTweenY: weapon pitch kick
    float recoil_tween_r;           // recoilTweenR: weapon roll kick
    float recoil_tween_ym;          // recoilTweenYM: weapon rise
    float recoil_tween_z;           // recoilTweenZ: weapon push back
    float crouch_lean;
    float crouch_rotation;
    float crouch_drop;
    float attach_y_offset; // JS `attachYOff`
    float attach_z_offset; // JS `attachZOff`
    float scale;

    float hold_width;
    float hold_distance_offset;
    vec3 origin;
    vec3 offset;
    vec3 left_hold;
    vec3 right_hold;

    // JS `cSpread` as (x, y) pairs; pellet i of a shot uses pair ammo_left * shots + i.
    const vec2 *custom_spread;
    unsigned int custom_spread_count;
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
extern const Weapon g_crossbow;
extern const Weapon g_famas;
extern const Weapon g_sawed_off;
extern const Weapon g_auto_pistol;
extern const Weapon g_bomb;
extern const Weapon g_blaster;

// Krunker v3.1.1 weapon attachments (src/data/attach.js / 86.js). `dotSrc` is never read by the client.
typedef struct {
    const char *name;
    const char *src;     // models/attach/<src>.obj + textures/attach/<src>.png
    float aim_offset_y;  // added to the weapon's aimOffY while aiming
    float scale;
} WeaponAttachment;

#define ATTACHMENT_COUNT 1
// getSavedVal("attachIndex") || 0: every player uses the Reflex Sight until attachment selection exists.
#define DEFAULT_ATTACHMENT_INDEX 0

extern const WeaponAttachment g_attachments[ATTACHMENT_COUNT];

// The attachment mounted on `weapon` for a player with `attach_index`, or NULL (weapon.attach == null or no such entry).
const WeaponAttachment *weapon_attachment(const Weapon *weapon, int attach_index);

// Indexed exactly like Krunker v3.1.1's weaponConfig (class loadouts and packets use these ids).
#define WEAPON_COUNT 19
#define WEAPON_ID_PISTOL 2
#define WEAPON_ID_KNIFE 12

extern const Weapon *g_weapons[WEAPON_COUNT];