#pragma once
// Projectile visuals (client/src/projectiles_render.c): Krunker v3.1.1's physObj
// projectiles (Krunker.Unpacked-v3.1.1/88.js `D`) for rockets, crossbow bolts,
// blaster shots and thrown knives.
//
//   index.js g9 ("pr")  a server projectile: physObj(x, y, z, yaw, pitch, range, ..., sid, null, meleeIndex)
//   index.js g8 ("pre") its end: disablePhys(); a lingering knife sticks at the end point,
//                       an impact also spawns the bullet-impact effect
//   95.js shoot()       the owner predicts its own projectile (physObj with dst 3000, owid
//                       shotCntP) unless the projectile is `dontPred` (crossbow)
//
// The native server sends PROJECTILE to every client including the owner, so the owner's
// predicted visual is linked to the server id when its PROJECTILE arrives (oldest unlinked
// prediction of the same weapon) instead of spawning a second one.
//
// Box projectiles are drawn by vfx.c with the tracer boxes (projectiles_render_boxes);
// thrown knives are textured melee meshes drawn through a private Scene. The Bombardment bombs
// (stored[2], "bmb" / "pr" with bomb = 1) are the streak_3 model in the same Scene, spun about Y.
#include <client.h>

typedef struct {
    vec3 position;
    vec3 forward;   // lookAt direction (local +z)
    float roll;     // accumulated rotateX
    vec3 size;      // scale.x, scale.y, length
    vec4 color;
    int lit;        // 0: MeshBasicMaterial (no light, no fog)
} ProjectileBox;

void projectiles_render_server_spawn(Client*, const NetProjectilePacket*); // PROJECTILE ("pr")
void projectiles_render_server_end(Client*, const NetProjectileEndPacket*); // PROJECTILE_END ("pre")
void projectiles_render_local_shot(Client*, const Player *me, int thrown);  // owner prediction (vfx.c)

void projectiles_render_update(Client*, float dt_ms);
void projectiles_render_draw_meshes(Client*, Camera*);          // thrown knives, world pass
int projectiles_render_boxes(ProjectileBox *out, int max);      // box projectiles for vfx.c
void projectiles_render_reset(void);                            // map change / new round
void projectiles_render_fini(void);                             // GL-owning meshes; context current
