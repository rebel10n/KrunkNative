#pragma once
// Nettspend (KrunkNative-only class; doc/v7/decisions.md "Nettspend"): the client side of the
// Purple Drank (primary) and Money Spread (secondary) weapons and of the "laced" event.
// client/src/nettspend_vfx.c. Everything here is render state: the shared tables (weapon
// ids WEAPON_ID_DRANK / WEAPON_ID_MONEY_SPREAD in weapons.h, the stats in kn_drank.c /
// kn_money_spread.c, the class, NET_EXT_KN_LACED in net_ext.h, the laced timer in drank.h) are
// shared/'s.
//
//   Purple Drank  - a lean double cup (procedural, like the Piss's hose) in the trigger hand; the
//                   stream and puddles are the Piss machinery in the stream's colour (vfx.c reads
//                   WeaponStream.color / GamePuddle.color, nothing to do here). Reload = "pour up":
//                   the cup lifts and tilts, a bottle comes in over the rim and pours (droplets,
//                   vfx.c) until the reload ends (first person only, like every reload anim).
//   Money Spread  - a cash stack in the trigger hand; each shot bursts a fan of fluttering bills
//                   (vfx.c), the pellets themselves are hitscan tracers like a shotgun's.
//   Laced         - NET_EXT_KN_LACED {uid, duration_ms} from the server (net_ext.h NetLacedPacket;
//                   the shared Player.laced_timer follows it through drank_apply_laced): for the
//                   local player a screen-distortion post-effect (assets/shaders/laced.frag,
//                   render_target.c) fades in, holds for the duration and fades out. Visual only (v1).
#include <client.h>
#include <net_ext.h>
#include <stddef.h>
#include <stdint.h>
#include <weapons.h>

typedef enum {
    NETTSPEND_WEAPON_NONE = 0,
    NETTSPEND_WEAPON_DRANK,
    NETTSPEND_WEAPON_MONEY_SPREAD,
} NettspendWeapon;

// Which Nettspend weapon `weapon_id` is (NETTSPEND_WEAPON_NONE for every other id).
NettspendWeapon nettspend_weapon_kind(int weapon_id);

// ---- KrunkNative-only icon art ----
// Next to Krunker's in the page tree (tools/nettspend/icons.py draws them), so the usual URLs find
// them: ui/textures/weapons/icon_kn_drank.png / icon_kn_money.png (Weapon.icon: HUD rack, kill feed,
// Object Manager, comp), ui/img/death-ui/<the same stems>.png (the death card) and
// ui/textures/classes/icon_16.png (the Nettspend's ClassConfig.icon_index).

// ---- the weapon meshes (player_mesh.c, prefab.c pickups) ----
// The cup / cash stack local frame follows the Piss's hose: the long axis is z, the "tip" (the cup's
// rim centre, where the stream leaves and the pour lands; the top of the cash stack) is
// (0, 0, -NETTSPEND_*_LENGTH / 2), so player_hose_points works unchanged with arms->hose set to it.
#define NETTSPEND_CUP_LENGTH 1.0f     // the double cup's height (z extent; the fist is ~0.8 thick)
#define NETTSPEND_CUP_RADIUS 0.34f    // its rim radius (the base is narrower)
#define NETTSPEND_FOAM_COLOR 0xf6f5f1 // the styrofoam: matte off-white
#define NETTSPEND_FOAM_GLOW 0.22f     // the cup's emissive lift (white in the shade too)
#define NETTSPEND_CASH_GLOW 0.35f     // the bills' green emissive lift (green in the shade too)
#define NETTSPEND_TP_SCALE 1.8f      // third person / the class preview: the cup and the cash at this size (the body's fist is far)
#define NETTSPEND_BOTTLE_LENGTH 1.3f  // the pour-up bottle's height (mouth at -length / 2)
// Where the off hand holds the bottle: this far (of its half length) from its centre toward the mouth;
// negative = toward the bottom. Low on the body, tipped over the cup from the off hand's side: the
// whole bottle shows between the fist and the rim, while the forearm leaves the fist the other way
// (held round the label or the neck, the fist hid the bottle; poured from the far side, the arm
// crossed in front of the cup).
#define NETTSPEND_BOTTLE_GRIP (-0.6f)
#define NETTSPEND_CASH_LENGTH 0.9f    // the Money Spread's height (z extent: the brick in the fist, the fan of bills above)
#define NETTSPEND_CASH_FAN 5          // bills in the fan
#define NETTSPEND_DRANK_COLOR 0x7b2cbf // the lean (#7b2cbf): the cup's liquid; the stream / puddles use WeaponStream.color
#define NETTSPEND_CASH_COLOR 0x2f9e5b  // a bill's face
#define NETTSPEND_CASH_BAND_COLOR 0xf1e6b8 // the stack's paper band / a bill's edge
// Third person: the trigger hand reaches straight out in front of its shoulder (the shoulder stays on the torso at
// x = +1.9, generate_arms). The pistol's hold aims the arm about 14 degrees in from the shoulder; the Drank's hand is
// held NETTSPEND_TP_HAND_REACH units in front (the arm is 5.4 long, so the elbow is nearly straight), at the pistol's
// height and NETTSPEND_TP_HAND_X across (the same aim). The arm solver reads the hand's forward distance as
// hold.z - offset.z and its x as -hold.x, so the hold is offset.z + reach and -x.
#define NETTSPEND_TP_HAND_REACH 5.0f
#define NETTSPEND_TP_HAND_X 0.65f
// Third person: the off hand hangs straight down from its shoulder, this long (the arm is 5.4 long, so the elbow
// is slightly bent, a relaxed arm), its hand at mid-thigh (player_mesh.c player_generate_meshes)
#define NETTSPEND_TP_OFF_HAND_REACH 5.3f
// The off hand outside the pour-up (player_mesh.c player_off_hand_away): its arm hangs down from the
// shoulder, this far out to its own side and forward per unit down. Zero: the third-person rest already hangs
// straight down (NETTSPEND_TP_OFF_HAND_REACH), so the arm stays straight and continuous from the shoulder.
#define NETTSPEND_OFF_HAND_AWAY_OUT 0.0f
#define NETTSPEND_OFF_HAND_AWAY_FWD 0.0f
// The lean cup (a white styrofoam double cup, purple lean), the pour-up bottle, the fanned cash. The
// caller owns the mesh (mesh_fini); the geometries are cached like the hose's and never freed.
Mesh *nettspend_cup_mesh(void);
Mesh *nettspend_bottle_mesh(void);
Mesh *nettspend_cash_mesh(void);

// The pour-up (the local player's reload of the Drank, first and third person): poses `cup`
// (arms->hose) and `bottle` (arms->weapon_left, hidden outside the reload) from the trigger hand's
// centre `hand` and the off hand's centre `off_hand` (the arms anchor frame, the rest pose, x mirrored
// for left-handed), the cup's and bottle's scale (third person: NETTSPEND_TP_SCALE) and the reload
// progress 0..1 (0 = just started). Both hands take part: the bottle starts and ends in the off hand,
// which carries it (held low on its body) over the cup and tips it. `cup_hand` / `bottle_hand` (may be NULL) get where the
// trigger hand holds the cup and the off hand the bottle (the anchor frame; the rest centres outside
// the reload): player_mesh.c reaches the arms there.
// `progress` < 0: not reloading (the rest pose, bottle hidden). Returns the pour's strength 0..1
// (the droplets' rate, vfx.c).
float nettspend_pose_pour(Mesh *cup, Mesh *bottle, vec3 hand, vec3 off_hand, float mirror, float scale, float progress,
                          vec3 *cup_hand, vec3 *bottle_hand);
// The pour's strength 0..1 at reload progress 0..1 (the bottle is tipped over the cup; vfx.c's
// droplet rate), 0 outside the pour beat or when not reloading (progress < 0)
float nettspend_pour_strength(float progress);
// The cup's rest pose in the trigger hand (first person), from the hand's centre
void nettspend_pose_cup_rest(Mesh *cup, vec3 hand, float mirror);
// The cash stack's pose in the trigger hand (first person)
void nettspend_pose_cash(Mesh *cash, vec3 hand, float mirror);

// ---- laced (the post-effect) ----
#define NETTSPEND_LACED_FADE_IN_MS 600.0f   // the distortion's ramp up
#define NETTSPEND_LACED_FADE_OUT_MS 1500.0f // ... and down at the end
#define NETTSPEND_LACED_MAX_MS 60000.0f     // a duration above this is clamped (a bad packet)
// The server says `uid` got laced for `duration_ms` (a NET_EXT_KN_LACED body, NetLacedPacket): the
// local player's visuals start (or keep the longer remainder); others' doses only log for now.
void nettspend_on_laced(Client *client, const uint8_t *body, uint16_t body_length);
// Start the local player's laced visuals now (the input script's `laced <ms>`, tests)
void nettspend_laced_start(float duration_ms);
// Advance the effect's clock (vfx_update: once per rendered game frame, ms)
void nettspend_laced_tick(float dt_ms);
void nettspend_laced_reset(void); // map change / INIT: no effect
// The post-effect's strength 0..1 (0: off, render_target.c skips the laced pass) and clock (s)
float nettspend_laced_intensity(void);
float nettspend_laced_time(void);
float nettspend_laced_left_ms(void); // ms until the visuals end (0 when off)
