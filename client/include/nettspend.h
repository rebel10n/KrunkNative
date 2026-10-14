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
// Krunker's icons are imported content (assets/textures/..., gitignored); KN-only art is tracked in
// the page tree instead: a weapon icon stem starting "icon_kn_" (the Drank's icon_kn_drank, the
// Money Spread's icon_kn_money) is ui/img/weapons/<stem>.png (the HUD rack and the kill feed; the
// death card's ui/img/death-ui/<stem>.png sits with Krunker's), a class icon_index >= CLASS_COUNT
// (Nettspend: 16) is ui/img/classes/icon_<n>.png. Both write the page URL into `out` and return
// it, or return NULL for Krunker's own (the caller's usual textures/... path). tools/nettspend/icons.py
// draws them.
#define NETTSPEND_ICON_PREFIX "icon_kn_"
const char *nettspend_weapon_icon_src(const char *icon, char *out, size_t size);
const char *nettspend_class_icon_src(int icon_index, char *out, size_t size);

// ---- the weapon meshes (player_mesh.c, prefab.c pickups) ----
// The cup / cash stack local frame follows the Piss's hose: the long axis is z, the "tip" (the cup's
// rim centre, where the stream leaves and the pour lands; the top of the cash stack) is
// (0, 0, -NETTSPEND_*_LENGTH / 2), so player_hose_points works unchanged with arms->hose set to it.
#define NETTSPEND_CUP_LENGTH 1.0f     // the double cup's height (z extent; the fist is ~0.8 thick)
#define NETTSPEND_CUP_RADIUS 0.34f    // its rim radius (the base is narrower)
#define NETTSPEND_BOTTLE_LENGTH 1.3f  // the pour-up bottle's height (mouth at -length / 2)
#define NETTSPEND_CASH_LENGTH 0.5f    // the cash stack's height (z extent, bills stacked along z)
#define NETTSPEND_DRANK_COLOR 0x7b2cbf // the lean (#7b2cbf): the cup's liquid; the stream / puddles use WeaponStream.color
#define NETTSPEND_CASH_COLOR 0x2f9e5b  // a bill's face
#define NETTSPEND_CASH_BAND_COLOR 0xf1e6b8 // the stack's paper band / a bill's edge
// The lean cup (double styrofoam cup, purple liquid), the pour-up bottle, the cash stack. The
// caller owns the mesh (mesh_fini); the geometries are cached like the hose's and never freed.
Mesh *nettspend_cup_mesh(void);
Mesh *nettspend_bottle_mesh(void);
Mesh *nettspend_cash_mesh(void);

// The pour-up (first person, the local player's reload of the Drank): poses `cup` (arms->hose) and
// `bottle` (arms->weapon_left, hidden outside the reload) from the trigger hand's centre `hand` (the
// arms anchor frame, x mirrored for left-handed) and the reload progress 0..1 (0 = just started).
// `progress` < 0: not reloading (the rest pose, bottle hidden). Returns the pour's strength 0..1
// (the droplets' rate, vfx.c).
float nettspend_pose_pour(Mesh *cup, Mesh *bottle, vec3 hand, float mirror, float progress);
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
