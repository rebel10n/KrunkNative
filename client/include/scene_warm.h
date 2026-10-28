#pragma once
// v10.0.4's warmScene (deob-10.0.4.js:204997-205054; changelog "Reduced stutter when a match starts"): after a match's
// map loads, every scene child is compiled (renderer.compileAsync with frustum culling off) before the match is shown,
// so nothing compiles or uploads on the frames after the loading card goes. The card waits for it (iîíïîìí hides
// loadingBg only once !warming, :223008-223016; warmScene().then(iîíïîìí) at match init, :223151).
// KN's GL renderer compiles a program / state / texture on its first draw, so a warm frame draws the whole world
// with frustum culling off (scene_set_frustum_culling): the warm lasts until the scene is stable for 3 frames
// (the bundle waits for its pipeline cache to stop growing for 3 frames, :205038-205046) or 4 s (0xfa0) have passed.
// KRUNKNATIVE_SCENE_WARM=0 turns it off.
#include <client.h>

// client_load_map: the map's meshes are in the scene. now_ms: any monotonic clock in ms.
void scene_warm_begin(const Scene *scene, double now_ms);
// Once per frame before the world render: 1 = draw the world with frustum culling off this frame.
int scene_warm_frame(const Scene *scene, double now_ms);
// 1 = warming: the loading card stays up (v10_loading's hide waits for it).
int scene_warm_active(void);
