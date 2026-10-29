#pragma once
// v10.0.4's warmScene (deob-10.0.4.js:204997-205054; changelog "Reduced stutter when a match starts"): after a match's
// map loads, every scene child is compiled (renderer.compileAsync with frustum culling off) before the match is shown,
// so nothing compiles or uploads on the frames after the loading card goes. The card waits for it (iîíïîìí hides
// loadingBg only once !warming, :223008-223016; warmScene().then(iîíïîìí) at match init, :223151).
// KN's GL renderer compiles a program / state / texture on its first draw, so a warm frame draws the whole world
// with frustum culling off (scene_set_frustum_culling): the warm lasts until the scene is stable for 3 frames
// (the bundle waits for its pipeline cache to stop growing for 3 frames, :205038-205046) or 4 s (0xfa0) have passed.
// Before that the bundle waits for the models still loading (:205001-205009): scene_warm_preload loads the map's AI
// models, which KN otherwise builds when the AIs first sync (all in one frame when a map's AIs spawn together).
// KRUNKNATIVE_SCENE_WARM=0 turns both off. The stop rule itself: scene_warm_rule.h.
#include <client.h>
#include <scene_warm_rule.h>

// client_load_map: the map's meshes are in the scene. now_ms: any monotonic clock in ms.
void scene_warm_begin(const Scene *scene, double now_ms);
// Once per frame before the world render: 1 = draw the world with frustum culling off this frame.
int scene_warm_frame(const Scene *scene, double now_ms);
// client_load_map, before scene_warm_begin: the map's AI models into the loaders' caches, so their first build is
// not a parse + upload on a play frame (scene_warm_preload.c).
void scene_warm_preload(const Map *map);
