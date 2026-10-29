#pragma once
// Krunker v10.0.4 counter skins (client/src/skin_counter.c): a skin whose attach is type 1 (ViewerSkin.counter_digits)
// carries an LED stat counter on its weapon model (loadCounter, deob-10.0.4.js:204390-204505; the attach on
// generateMeshes, :172723-172737): a dark panel with the stat's 7x7 icon and `digits` 3x5 pixel digits, lit pixels glowing
// in the attach colour. Its count is the copy's stat plus what this match counted (counterLive, :172729-172734 /
// countSkinStat, :175889-175902).
#include <client.h>

struct ViewerSkin;
typedef struct SkinCounter SkinCounter;

// The counter of `skin` on its weapon mesh (`parent`: the weapon mesh's transform, the bundle's weaponMeshes[i]), showing
// `count`; NULL when the skin has no counter. The meshes are added to a scene by skin_counter_scene_add.
SkinCounter *skin_counter_create(const struct ViewerSkin *skin, long count, MeshTransform *parent);
void skin_counter_fini(SkinCounter *counter);
void skin_counter_scene_add(Scene *scene, SkinCounter *counter);
void skin_counter_scene_remove(Scene *scene, SkinCounter *counter);
// The counter follows its weapon's visibility (the per-frame weapon sync, player_mesh.c).
void skin_counter_set_visible(SkinCounter *counter, int visible);
// setCount (deob-10.0.4.js:204493-204496): clamped to 0 .. 10^digits - 1; a change rebuilds the lit pixels.
void skin_counter_set_count(SkinCounter *counter, long count);
long skin_counter_count(const SkinCounter *counter);
// The counted stat (iíìîííî, deob-10.0.4.js:106489-106492): the attach's when it is one of the eight, else "kills".
const char *skin_counter_stat(const SkinCounter *counter);

// countSkinStat on the client (deob-10.0.4.js:175894-175901): the match's count of `stat` with `weapon_id` for player
// `player_id`, kept across mesh rebuilds (counterLive), and the counter on that weapon updated. Unknown pairs are ignored.
void skin_counter_count_stat(SkinCounter *counter, int player_id, int weapon_id, const char *stat, int amount);
// The match's live count already counted for that (player, weapon, stat) (counterLive[weapon].n), 0 none.
long skin_counter_live(int player_id, int weapon_id, const char *stat);
// A new match: every live count starts again (counterLive = {}).
void skin_counter_reset_live(void);
// countSkinStat(player, weapon_id, stat, amount) on the client: the counter of that weapon's slot (weapon_id -1: every
// slot's), when it counts that stat. The kill feed, hits, AI kills and SKIN_STAT call it (net/game_events.c).
void skin_counter_count_player(const Player *player, int weapon_id, const char *stat, int amount);
// Once per frame (vfx_update): the local player's jumps since the last frame (Player.jump_events) on its held counter.
void skin_counter_frame(const Player *me);

// The worn copies' data per player uid and store skin, from NET_EXT_KN_SKIN_COPIES (or the krunker.io account's copy meta):
// the copy's stat each counter shows under this match's count (generateMeshes' IÌiiïíï[stat], deob-10.0.4.js:172727-172728;
// base < 0 removes it) and the copy's seed for the seeded custom shader 15 (iîîíïiï, :24344-24353; 0 none).
void skin_counter_set_base(int uid, int skin, long base);
void skin_counter_set_seed(int uid, int skin, int seed);
long skin_counter_base(int uid, int skin); // 0 when none is known
int skin_counter_seed(int uid, int skin);  // 0 when none is known
void skin_counter_reset_bases(void);
// The jumps of player `uid` since its jump_events were last seen (pure: client/tests/skin_counter_test.c).
int skin_counter_take_jumps(int uid, unsigned short events);

// ---- pure (client/tests/skin_counter_test.c)
// Lit state of digit pixel `pixel` (0-14, row-major 3x5) at position `position` (0 = units) for `count`: the bundle's
// iíïîíií (deob-10.0.4.js:196850-196859) with the digit bitmaps iîîîïíí (:196823).
int skin_counter_digit_lit(long count, int position, int pixel);
// The 7x7 icon of a stat (iîìïííi, deob-10.0.4.js:196824-196833): pixel 0-48 row-major, 1 = lit.
int skin_counter_icon_lit(const char *stat, int pixel);
