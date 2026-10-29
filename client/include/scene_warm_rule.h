#pragma once
// The warm's stop rule, GL- and scene-free (client/src/scene_warm.c; client/tests/scene_warm_test.c checks it).
// scene_warm.h wraps it for a Scene.
#include <stddef.h>

#define SCENE_WARM_MAX_MS 4000.0 // 0xfa0: the model wait, the compile wait and the race all give up after 4 s
#define SCENE_WARM_STABLE 3      // frames without a new mesh (iíÎîíìí >= 3, deob-10.0.4.js:205042)

// A map load: mesh_count meshes are in the world scene. now_ms: any monotonic clock in ms.
void scene_warm_begin_count(size_t mesh_count, double now_ms);
// Once per frame before the world render, with the scene's mesh count now: 1 = draw the world with frustum culling
// off this frame. The warm ends once SCENE_WARM_STABLE frames in a row added no mesh, or after SCENE_WARM_MAX_MS.
int scene_warm_frame_count(size_t mesh_count, double now_ms);
// 1 = warming: the loading card stays up (v10_loading's hide waits for it).
int scene_warm_active(void);
// 1 = the warm is on (KRUNKNATIVE_SCENE_WARM is not "0").
int scene_warm_enabled(void);
