#pragma once
// Krunker v7.1.4 AI visuals (client/src/ai_render.c): the AI meshes (AI.js init / update with
// RENDER: genObj3D container + loadMesh / loadAnimMesh of the AI's user asset or "ghost_0", the
// texture strip, offsets, interpolation, Idle / Move / "ana" clips, idle sounds) and the AI shots
// (particles.js physObj "orb_0" with the shooter's projOb: additive, no depth write, spinning).
// The AI list itself is Game.ai (shared/include/ai.h): the in-process offline game simulates it,
// a server's client mirrors it from AI_ADD / AI_SYNC (v3.1.1) or the v7 AI packets.
#include <client.h>
#include <ai.h>

void ai_render_tick(Client*, float delta);   // per frame before the world pass: meshes, bob, frames, clips, orbs, canBSeen
void ai_render_projectile(Client*, const NetProjectilePacket*);         // "pr" with an AI stored kind
void ai_render_projectile_end(Client*, const NetProjectileEndPacket*);  // "pre"
void ai_render_ai_shot(Client*, const NetAIShotPacket*);                // the shooter of the last AI "pr" (projOb visuals)
void ai_render_reset(Client*);               // drop every AI mesh / orb (map unload, shutdown)
