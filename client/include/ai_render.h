#pragma once
// Krunker v3.1.1 AI visuals (client/src/ai_render.c): the AI meshes (ai.js init / update with
// RENDER: genObj3D container + loadMesh(model, 0, scale, 0, 0, scale), bobbing, xD yaw, the
// 2-frame "hit" texture) and the AI shots (particles.js physObj with src "orb_0": additive,
// no depth write, spinning). The AI list itself is Game.ai (shared/include/ai.h): the
// in-process offline game simulates it, a server's client mirrors it from AI_ADD / AI_SYNC.
#include <client.h>

void ai_render_tick(Client*, float delta);   // per frame before the world pass: meshes, bob, frames, orbs, canBSeen
void ai_render_projectile(Client*, const NetProjectilePacket*);         // "pr" with an AI stored kind
void ai_render_projectile_end(Client*, const NetProjectileEndPacket*);  // "pre"
void ai_render_reset(Client*);               // drop every AI mesh / orb (map unload, shutdown)
