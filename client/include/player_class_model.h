#pragma once
// Krunker v10.0.3 class models (client/src/player_class_model.c, doc/v10/class-models.md): a class's "Player Asset"
// (classConfig mdl / aID / mdlFP / animS / mdlS / mdPS / mdPY / mdlDr / mdlYO, deob-10.0.3.js:20255-20334) drawn by
// players.generateMeshes in place of the default body (:194228-194266), the clips players.playClip plays on it
// (:193806-193820: Crouch / Move / Idle every frame :191184-191187, Jump :195272, Melee :195909, Shoot :196418 and the
// server's "c" PLAYER_ANIM :257769) and the asset "uai" UPDATE_ASSET_ID swaps in (players.updateAssetID :197964).
//
// PlayerMesh.class_model holds it; player_mesh.c builds it with the rest of the model, scene.c adds it instead of the
// body, arms and weapons (which are still built: the anchors other code reads), and player_update_meshes poses it.
//
// Model builds: every player_generate_meshes is v10's generateMeshes (players.add on a spawn, regenMeshes on a swap, a
// loadout, a prop change) unless player_class_model_keep() said otherwise first: KrunkNative rebuilds a model for
// reasons v10 has none for (a downloaded file, the Stalker look, its own third person), and such a rebuild keeps the
// asset the model shows and its script handle (getAsset). A generateMeshes takes player.assetTxt when one waits (a
// "uai" or player.assetID: Player.asset_txt) and clears it, else the class's aID (:194230-194265).
#include <client.h>
#include <gltf.h>

typedef struct PlayerClassModel {
    int mdl;                      // what it draws: 0 nothing, 1 Sprite, 2 Model
    int class_mdl;                // the class's config.mdl at the build (0 Default, 1 Sprite, 2 Model)
    char asset[32];               // the asset text it was built from (assetTxt || config.aID, "" for mdl 0)
    unsigned char from_swap;      // that text was a "uai" / player.assetID swap (assetTxt), not the class's aID
    // KrunkNative: the default body draws instead, for an FBX ("f") model: v10 draws it with three's FBXLoader,
    // KrunkNative has no FBX loader (doc/v10/class-models.md)
    unsigned char default_body;
    unsigned char has_anims;      // player.hasAnims (config.aID holds g / q / f, deob-10.0.3.js:194877)
    unsigned char world;          // in the world scene for the local first-person model (render_you, scene_add_class_model)
    unsigned char has_last;       // last_position is set
    unsigned serial;              // this model (meshRef) for getAsset's handle; a keep-rebuild carries it over
    char pending[600];            // a file still downloading at the build ("" none): rebuilt (kept) once it lands
    // RENDER.loadMesh / loadAnimMesh's mesh (:194251-194263): (0, y, 0) on objInstances (the anchor), rotation y mdlDr,
    // scale mdlS * mdPS; loadSprite's sprite sits at (0, 5.5, 0) with scale 11 (:234089-234099)
    MeshTransform holder;
    Mesh *mesh;                   // mdl 1: the sprite; mdl 2: the OBJ model (re-centred under the holder)
    GltfInstance *gltf;           // mdl 2: the GLTF / GLB model (loadAnimMesh); its root is meshRef.children[0]
    vec3 last_position;           // oldX / oldY / oldZ of players.update's Move / Idle pick
    char clip[24];                // the clip the GLTF shows: the last playClip that took ("" none; the `models` probe)
} PlayerClassModel;

// generateMeshes' branch for this player. render_you: the local first-person model; third_person: the model is seen
// from outside (thirdPerson, a camera offset or another player's: IÍïíiií, deob-10.0.3.js:194099). NULL: the default
// body (config.mdl 0 with the model shown). Otherwise a model that stands in for the body, which may draw nothing: v10
// builds nothing at all for our first-person model when the class has mdlFP off (:194228), nothing while the asset
// loads, and nothing for one that never loads (a missing or broken file). A generateMeshes through the model branch
// takes (clears) a waiting Player.asset_txt.
PlayerClassModel *player_class_model_create(Player*, int render_you, int third_person);
void player_class_model_free(PlayerClassModel*);
// It stands in for the default body (scene_add_player_mesh draws it instead)
int player_class_model_replaces_body(const PlayerClassModel*);
// The scene lists (scene_add_player_mesh / scene_remove_player_mesh). render_you: the local first-person model's goes
// to the world scene (g_pet_world_scene), as v10 draws it on layer 0 with the world, not with the viewmodel.
void scene_add_class_model(Scene*, PlayerClassModel*, int render_you);
void scene_remove_class_model(Scene*, PlayerClassModel*, int render_you);
// Per frame (player_update_meshes): the anchor without the step bob / sway (deob-10.0.3.js:193662-193681), the
// Crouch / Move / Idle clip, the Vehicle scheme's lean (:19145-19152) and the GLTF's animation.
void player_class_model_update(Player*, PlayerClassModel*, MeshTransform *anchor, float step_yaw);
// The model must be rebuilt (main.c client_refresh_weapon_skins through player_weapon_skin_ready): 1 a generateMeshes
// (a waiting assetTxt of an active player, the class's Player Type or aID changed since the build), 2 a rebuild that
// keeps the model's asset (its file came down, or failed, since the build); 0 none.
int player_class_model_stale(const Player*, const PlayerClassModel*);
// The next build of this player is KrunkNative's own rebuild, not v10's generateMeshes: it keeps the asset the model
// shows (a swap stays) and its getAsset handle, and leaves a waiting assetTxt waiting. Call it right before
// client_drop_player_mesh / player_meshes_fini of such a rebuild.
void player_class_model_keep(const Player*);
// A SPAWN for a player alive already (main.c keeps its meshes): v10's players.add builds the model again, so 1 when that
// build would change it (it shows a swap, which reverts to the class's aID, or an assetTxt waits)
int player_class_model_spawn_rebuild(const Player*);
// The drawn GLTF class model of the player (meshRef with playClip), NULL when none
GltfInstance *player_class_model_gltf(const Player*);
// The drawn class model of player `uid` built as `serial` (getAsset's handle, ks_scene.c), NULL once it is gone
PlayerClassModel *player_class_model_find(const Game*, int uid, unsigned serial);
// players.playClip's client branch (deob-10.0.3.js:193817-193819): an active player with hasAnims whose model is drawn
// plays `clip` `reps` times (meshRef.playClip(name, reps): 0 loops, < 0 is -1's Infinity)
void player_class_model_play_clip(Player*, const char *clip, float reps);
// KrunkScript's player.playAnim(clip, reps) / stopAnim() (clip NULL) on the drawn GLTF, with no hasAnims test
// (deob-10.0.3.js:208232-208244)
void player_class_model_script_clip(const Player*, const char *clip, int reps);
// One line about the player's model for the input script's `models` probe: "body=model mdl=2 asset=11441g clip=Idle"
// (body: default, model, none (nothing drawn), fallback (the default body for an FBX model), - (no mesh: dead);
// " swap=1" for a model built from assetTxt, " stalker=1" for a model built with the Stalker look)
void player_class_model_describe(const Player*, char *out, size_t n);
// players.updateAssetID on the client (deob-10.0.3.js:197964-197975): `if (config.mdl && assetTxt.length)` the asset
// waits in Player.asset_txt for the next generateMeshes; 1 when it applies (the caller rebuilds an active player's
// model at once: regenMeshes)
int player_class_model_set_asset(Player*, const char *asset_txt);
// INIT: keep requests of a session that ended are dropped
void player_class_model_reset(void);
// ks_scene.c's getAsset handles of a model being freed lose it (ks_scene_class_model_freed)
void ks_scene_class_model_freed(const PlayerClassModel*);
