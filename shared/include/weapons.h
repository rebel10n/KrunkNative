#pragma once
#include <common.h>

struct Player_t;

// Krunker projectile flight (v7.1.4 weapons.js pj_* fields, projectiles.js stored[]). Krunker
// units: spd units/ms, grav units/ms^2. Moved here from shared.h so each Weapon carries one.
typedef struct {
    float speed;        // spd (units/ms)
    float gravity;      // grav (units/ms^2)
    float range;        // max flight distance
    float damage;       // direct hit damage
    float explode;      // explosion radius, 0 none
    float explode_damage; // eDmg at the centre
    float hit_box;      // hitB: sphere test radius around the target's middle, 0 = player box
    unsigned char no_impact:1; // nImp: no impact particles
    unsigned char dont_pred:1; // dontPred: no lag compensation (stateConst 0; the Crossbow)
} ProjectileConfig;

// KrunkNative Piss (WEAPON_ID_PISS, shared/include/piss.h; doc/v7/decisions.md "Piss"): the stream
// and puddles of the v3.1.1 reconstruction's grappling-gun branch (0c317bf / e1624e4, the
// weapons.js mustard* / mustardPuddle* / hose* fields). Krunker units: speed units/ms, gravity
// units/ms^2, times in ms like the branch.
typedef struct {
    int color;              // mustardColor 0xRRGGBB (16765745 = #ffd231)
    int particles;          // mustardParticles: client spray particles per shot
    float speed;            // mustardSpeed: stream speed (units/ms)
    float spread;           // mustardSpread: particle yaw jitter (+-, pitch +-0.55x), visual only
    float gravity;          // mustardGravity (units/ms^2)
    float life;             // mustardLife (ms): how long the stream (and a particle, x0.75-1.25) flies
    float size;             // mustardSize: particle size (scale x0.14-0.24, length x0.55-0.95)
    float pelvis_forward;   // hosePelvisForward / mustardPelvisForward: the source in front of the feet
    float pelvis_height;    // hosePelvisHeight / mustardPelvisHeight: ... above them (- crouch x 1.25)
    float puddle_radius;    // mustardPuddleRadius
    float puddle_life;      // mustardPuddleLife (ms)
    float puddle_dps;       // mustardPuddleDPS
    float puddle_tick;      // mustardPuddleTick (ms): damage every tick of dps x tick / 1000
    int puddles_per_player; // KN: live puddles per owner (the oldest goes)
    int puddles_max;        // mustardMaxPuddles: live puddles in the game (the oldest goes)
    float hose_radius;      // hoseRadius (genHose)
    float hose_length;      // hoseLength
    vec3 hose_view;         // first-person centre from the trigger hand's centre, weapon anchor axes, x mirrored left-handed (KN; the branch: hoseViewX, hoseViewY - 1.5, hoseViewZ in the weapon anchor)
    vec3 hose_view_rot;     // first-person rotation x / y / z (hoseViewRotX / Y / Z), y and z mirrored left-handed
    // KrunkNative Purple Drank (shared/include/drank.h; doc/v7/decisions.md "Purple Drank"): 0 on the Piss
    float puddle_slow;      // move accel x of a player standing in the puddle (victims only), 0 = no slow
    float laced_chance;     // chance per reload (the pour-up) that the pourer gets laced, 0 = never
    float laced_ms;         // how long the laced state lasts (ms)
} WeaponStream;

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
    // JS `anim(player, TWEEN, type, charge)` (players.js melee, local player only): tweens
    // player->melee_anim. type 1 = swing with the knife, 2 = fists (knife thrown), 3 = throw release
    // (charge: the throw's d, 0.1..1; v10.js:172423). NULL: no anim.
    void (*melee_anim)(struct Player_t *player, int type, float charge);

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

    // JS `cSpread` as (x, y) pairs. v7 nests two patterns ([[..], [..]]) and pellet i of a shot
    // uses pattern ammo_left % 2, pair i: pairs [(ammo % 2) * shots, +shots). One-pattern
    // weapons repeat it.
    const vec2 *custom_spread;
    unsigned int custom_spread_count;

    // ---- WP-B: Krunker v7.1.4 fields (src/data/weapons.js). "0 = x" marks a v7 default. ----
    const char *sound;          // sound: assets/sound/<sound>.mp3 shot (client/src/sound_events.c)
    unsigned char type;         // weaponTypes.js: 0 primary, 1 secondary, 2 melee, 3 equipment
    int kill_score;             // kill: ["", N] replaces the base 50 kill score, 0 = none
    int min_rec;                // minRec: account level to pick it, 0 = open
    int mag_reserve;            // magReserve: reserve magazines (finiteAmmo modes)
    float leg_mlt;              // lsMlt, 0 = 0.5: legshot multiplier (not on headshots)
    float move_spread;          // movSpread, 0 = 1: spreadPlus gained per unit moved / fallen
    float aim_spread;           // adSpread: share of the hip spread kept while fully aimed
    // Charge system (game setting chrgWeps, players.js / player.js chargeTime)
    float charge_time;          // chrgTime (s), 0 = not chargeable (the knife uses the class throwChrg)
    float charge_mlt;           // chrgMlt, 0 = 0.9: projectile dmgMlt = 1 - chrgMlt * (1 - charge)
    float charge_decay;         // chrgDec: charge seconds lost per second while not charging
    float full_charge_mlt;      // fullChrgMlt: damage x of the full-charge shot (Charge Rifle 50)
    unsigned char over_charge:1;     // ovrChrg: past chrgTime * 1.2 the charge resets (overCharged)
    unsigned char shoot_charge:1;    // shootChrg: charges with the trigger held, not while aiming
    unsigned char rate_charge:1;     // rateChrg: fire interval x max(0.7, (1 - charge) * 1.5)
    unsigned char full_charge_shot:1;// fullChrgSht: normal shots cost no ammo, the full charge fires one round
    unsigned char bypass_charge:1;   // bypassChrg: normal shots keep the charge
    unsigned char no_comp:1;         // noComp: not in competitive lobbies
    unsigned char custom_only:1;     // customOnly: custom games only
    unsigned char no_reload:1;       // noReload
    unsigned char no_swap:1;         // noSwap: can't switch away (Minigun, War Machine)
    unsigned char no_jump:1;         // noJump (Minigun)
    unsigned char remove_on_empty:1; // removeOnEmpty: dropped from the loadout when empty
    unsigned char no_proj_spread:1;  // noProjSpread
    unsigned char build:1;           // build (Build Tool)
    unsigned char no_cross:1;        // noCross: no crosshair
    unsigned char can_upgrade:1;     // canUpgrade (krumMachine)
    unsigned char kn_tool:1;         // KrunkNative Tool Gun (object_manager.h OM_ACTION_TOOL): fires no bullet, spends no ammo
    unsigned char kn_physgun:1;      // KrunkNative Physics Gun (object_manager.h OM_ACTION_EDIT): never fires, the client grabs
    // ---- Krunker v10.0.2 weapon fields (doc/v10/gameplay.md section 5; no v7.1.4 weapon sets them,
    // so they are behaviour-neutral under the v7 ruleset)
    unsigned char no_dmg:1;          // noDmg (Compressor): the bullet ray traces nothing; out of onlyWeapons
    unsigned char phys_pull:1;       // physPull (Compressor): the physPow ray fired with the aim input pulls (sign -1)
    unsigned char non_obj_impulse:1; // nonObjImpulse (weapon-level, v7 had it on modes only): self push, no object push
    unsigned char r_shoot:1;         // rShoot (Compressor): the aim input fires too (procInputs lMouse || rShoot && rMouse)
    unsigned char trail:1;           // trail (Sniper Rifle, AWP Blaster): a bullet trail on every shot, see weapon_shot_trail
    unsigned char trail_charge:1;    // trailChrg (Charge Rifle): the charged trail on a full-charge shot, see weapon_shot_trail
    float gravity_mlt;               // gravMlt, 0 = 1 (Boulder 1.7): player gravity x while held
    int tint;                        // KrunkNative: viewmodel / pickup colour multiplier 0xRRGGBB, 0 = none
    int effect_on_hit;          // effectOnHit + 1 (0 = none): EffectId given on hit, floor(charge * 10) stacks
    int deployable;             // deployable + 1 (0 = none): the spawnable a shot places (Slimer pads)
    float deploy_range;         // deployRang
    float grapple_range;        // grapRang (Grappler)
    ProjectileConfig projectile_config; // pj_*: speed 0 = none (also the bullet-drop flight of hitscan guns)
    const WeaponStream *stream; // KrunkNative Piss: a stream instead of a bullet (shared/src/piss.c), NULL none
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
// v7.1.4 weapons 19-28 (shared/src/weapons/v7_*.c)
extern const Weapon g_build_tool;
extern const Weapon g_grappler;
extern const Weapon g_tehchy9;
extern const Weapon g_noob_tube;
extern const Weapon g_slimer;
extern const Weapon g_zapper;
extern const Weapon g_minigun;
extern const Weapon g_war_machine;
extern const Weapon g_akimbo_pistol;
extern const Weapon g_charge_rifle;
// Krunker v10.0.2 (shared/src/rules/v10/): the rebalanced copies and the two new secondaries
extern const Weapon g_ak47_v10, g_pistol_v10, g_smg_v10, g_lmg_v10, g_semi_auto_v10, g_akimbo_v10,
    g_deagle_v10, g_famas_v10, g_blaster_v10, g_tehchy9_v10;
extern const Weapon g_boulder;    // v10 id 29
extern const Weapon g_compressor; // v10 id 30
// KrunkNative-only weapons after v7's table (WEAPON_ID_TOOL_GUN...)
extern const Weapon g_tool_gun;
extern const Weapon g_piss; // WEAPON_ID_PISS (shared/src/weapons/kn_piss.c)
extern const Weapon g_phys_gun;
extern const Weapon g_drank;        // WEAPON_ID_DRANK (shared/src/weapons/kn_drank.c)
extern const Weapon g_money_spread; // WEAPON_ID_MONEY_SPREAD (shared/src/weapons/kn_money_spread.c)

// Krunker v7.1.4 weapon attachments (src/data/attach.js). `dotSrc` is never read by the client.
typedef struct {
    const char *name;
    const char *src;     // models/attach/<src>.obj + textures/attach/<src>.png
    float aim_offset_y;  // added to the weapon's aimOffY while aiming
    float scale;
    float zoom_mlt;      // zoomMlt, 0 = 1: ADS zoom x (Acog 1.35; FOV only, spread untouched)
    float z_recoil;      // zRec, 0 = the weapon's recoilZM (viewmodel recoil push)
    float z_offset;      // zOff: sight mesh z offset
    float exp_scale;     // expScale (explosion preview scale, cosmetic)
    int min_rec;         // minRec: class rank (stats c<classIndex>) to pick it, 0 = open
} WeaponAttachment;

#define ATTACHMENT_COUNT 2
#define ATTACHMENT_ACOG 1
// The sight y offset of the Acog per weapon id (attach.js yOff {1: 0.01, 14: -0.01}), 0 elsewhere.
float weapon_attachment_y_offset(const WeaponAttachment*, int weapon_id);
// The ADS zoom of `weapon` with `attachment` (v7 players.js: weapon.zoom * zoomMlt).
float weapon_aim_zoom(const Weapon*, const WeaponAttachment*);
// The trail argument of a hitscan ray's NetShotPacket.trail, and the "9" tracer's slot 6 (Krunker v10 shoot(), the
// same value for every ray of the shot): 0 none, 1 weapon.trail, 2 weapon.trailChrg on a full-charge shot (the
// charged trail). full_charge is the shot's fullCharge flag.
unsigned char weapon_shot_trail(const Weapon*, int full_charge);

// v7.1.4 scope overlays (src/data/aimRecticles.js): the saved scopeIndex picks an entry, whose
// `id` names textures/scopes/scope_<id>.png; -1 no overlay, -2 the saved custom image.
typedef struct {
    const char *name;
    int id;
} ScopeOverlay;

#define SCOPE_OVERLAY_COUNT 7
extern const ScopeOverlay g_scope_overlays[SCOPE_OVERLAY_COUNT];
// getSavedVal("attachIndex") || 0: every player uses the Reflex Sight until attachment selection exists.
#define DEFAULT_ATTACHMENT_INDEX 0

extern const WeaponAttachment g_attachments[ATTACHMENT_COUNT];

// The attachment mounted on `weapon` for a player with `attach_index`, or NULL (weapon.attach == null or no such entry).
const WeaponAttachment *weapon_attachment(const Weapon *weapon, int attach_index);

// Indexed exactly like Krunker v7.1.4's weaponConfig (class loadouts and packets use these ids;
// 0-18 keep their v3.1.1 meaning).
#define WEAPON_COUNT 29 // v7.1.4's weaponConfig: what the v7 menus, skins and host config list
#define WEAPON_ID_PISTOL 2
#define WEAPON_ID_KNIFE 12
#define WEAPON_ID_CROSSBOW 13
#define WEAPON_ID_BUILD_TOOL 19
#define WEAPON_ID_GRAPPLER 20
#define WEAPON_ID_SLIMER 23
#define WEAPON_ID_ZAPPER 24
#define WEAPON_ID_MINIGUN 25
#define WEAPON_ID_WAR_MACHINE 26
#define WEAPON_ID_CHARGE_RIFLE 28
#define WEAPON_ID_BOULDER 29    // v10.0.2
#define WEAPON_ID_COMPRESSOR 30 // v10.0.2
#define WEAPON_V10_COUNT 31     // v10.0.2's weaponConfig
// KrunkNative-only ids sit at WEAPON_ID_KN_BASE, above Krunker's own table (v10.0.2 has 31
// weapons: its 29 Boulder and 30 Compressor took the ids KN used before; doc/v7/decisions.md
// "KrunkNative id ranges"). Ids between the last Krunker weapon and the KN base are NULL in the
// table. The KN weapons are in the ruleset's table (Game.weapons), so loadouts, pickups and
// packets take them, but outside WEAPON_COUNT, so the menu lists (loadout, market, host config,
// challenges) never show them (doc/v7/decisions.md "Tool Gun").
#define WEAPON_ID_KN_BASE 64
#define WEAPON_ID_TOOL_GUN (WEAPON_ID_KN_BASE + 0)
#define WEAPON_ID_PISS (WEAPON_ID_KN_BASE + 1) // KrunkNative Piss (doc/v7/decisions.md "Piss")
#define WEAPON_ID_PHYS_GUN (WEAPON_ID_KN_BASE + 2)
#define WEAPON_ID_DRANK (WEAPON_ID_KN_BASE + 3)        // KrunkNative Purple Drank (doc/v7/decisions.md "Purple Drank")
#define WEAPON_ID_MONEY_SPREAD (WEAPON_ID_KN_BASE + 4) // KrunkNative Money Spread (doc/v7/decisions.md "Money Spread")
#define WEAPON_TABLE_COUNT (WEAPON_ID_KN_BASE + 5)
// The ids builds before the ruleset seam gave the KN weapons (29 Tool Gun, 30 Piss, 31 Physics
// Gun, right after v7's table). weapon_id_from_legacy maps a stored or typed id of that era (host
// config "weps" keys, `give <id>`) to today's, for rulesets whose Krunker table ends before 29.
#define WEAPON_ID_LEGACY_KN_BASE 29
#define WEAPON_LEGACY_KN_COUNT 3 // the KN weapons of that era (29 Tool Gun, 30 Piss, 31 Physics Gun); later ones never had a legacy id
int weapon_id_from_legacy(int id, int krunker_weapon_count);

// The weapon tables live in the rulesets (shared/include/ruleset.h; shared/src/rules/<ver>/):
// ruleset_current()->weapons, or Game.weapons with a game in scope.
// v7.1.4 upgradeWeapons.js entry of weapon `id` (krumMachine upgrades), NULL out of range.
const Weapon *weapon_upgraded(int id);