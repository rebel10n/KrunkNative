#pragma once
// Combat visual effects (client/src/vfx.c): Krunker v3.1.1's particle manager
// (Krunker.Unpacked-v3.1.1/88.js), the first-person muzzle flash and casings
// (95.js shoot), the screen-space muzzle flash and damage overlay drawn on the
// 2D overlay canvas (91.js), the RPG explosion manager (88.js ExplosionManager)
// and map PARTICLES areas (93.js -> 88.js area()).
//
// Everything is client-side render state: sprites, boxes and debris live in
// fixed pools here and are drawn with assets/shaders/vfx.{vert,frag}; nothing is
// stored in shared game state.
//
// Render order (client_tick_game in main.c):
//   scene_render(world) -> vfx_update + vfx_render_world
//   depth clear -> scene_render(fps)  -> vfx_render_fps   (muzzle flash sprite)
//   overlay_render                    -> vfx_render_screen (screen flash, blood)
//   html_ui_render (page, HUD and the #aimRecticle scope on top)
#include <client.h>

void vfx_update(Client*, float delta);   // diff player state (local prediction), spawn, simulate
void vfx_render_world(Client*);          // world layer, after the world scene
void vfx_render_fps(Client*, Camera*);   // first-person layer, after the fps scene (its fpsFOV camera)
void vfx_render_screen(Client*);         // 2D overlay layer (Krunker's #game-overlay canvas)
void vfx_reset(void);                    // drop every live effect (map change)
void vfx_fini(void);                     // GL objects; call with the context current

// Event hooks for server-authoritative effects. vfx_update() predicts the local
// player's own hitscan shots (and hands its projectiles to projectiles_render.c).
//
// Krunker packet "9" (index.js g7): a remote player's hitscan shot. `hit` is the
// impact point; `has_normal` is 0 when the shot hit a player or nothing (no
// impact particles); normal_yaw/normal_pitch are the face angles Krunker sends
// (aM/aN: yaw 0 = +z, PI/2 = +x, pitch PI/2 = up). `trail` = weapon.trail.
void vfx_remote_shot(Client*, const Player *shooter, vec3 hit, int has_normal, float normal_yaw, float normal_pitch, int trail);
// KrunkNative Piss: another player's stream shot (NET_PACKET_SHOT of a stream weapon): the spray
// from its hose along its view, ending about where the stream stopped (`hit`).
void vfx_remote_stream_shot(Client*, const Player *shooter, const WeaponStream *stream, vec3 hit);
// Nettspend Money Spread (nettspend.h): another player's throw (NET_PACKET_SHOT of the Money
// Spread, on top of vfx_remote_shot's pellet tracer): the burst of bills from its hands, once per throw
// (the caller sends it with the throw's first ray only: one NET_PACKET_SHOT per bill).
void vfx_remote_money_spread(Client*, const Player *shooter);
// 88.js effect(x, y, z, yaw, pitch, 0): impact puffs plus the bullet hole (index.js g8
// spawns it at a projectile's end with yaw = pitch = 0).
void vfx_impact(Client*, vec3 position, float yaw, float pitch);
// v7.1.4 players.js itemEffects -> PARTICLES.effect(x, y, z, 0, 0, particle): a store item's
// particle effect (5 feathers, 6 embers, 7 stars) at `position`; other ids do nothing.
void vfx_item_effect(vec3 position, int effect);
// Krunker packet "ex" (index.js gb): explosion at `position` with range `radius`
// (projectile.explode, e.g. 35 for the RPG).
void vfx_explosion(Client*, vec3 position, float radius);
// v10.0.2 ExplosionManager.explodeAt(x, y, z, scale, small, large): debris only (no sound / shake).
void vfx_explode_at(Client*, vec3 position, float scale, int small, int large);
// v10.0.3 addTrail(physObj, 1) for a projectile physObj drawn with trails (the Bombardment's bombs):
// from `start` along `velocity` (units/ms), growing at its speed up to `dst` / 2; Bullet Trails setting.
void vfx_physobj_trail(vec3 start, vec3 velocity, float dst);
