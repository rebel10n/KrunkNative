#pragma once
// Krunker v10.0.3 class models (client/src/player_class_model.c, doc/v10/class-models.md): a class's "Player Asset"
// (classConfig mdl / aID / mdlFP / animS / mdlS / mdPS / mdPY / mdlDr / mdlYO, deob-10.0.3.js:20255-20334) drawn by
// players.generateMeshes in place of the default body (:194228-194266), the clips players.playClip plays on it
// (:193806-193820: Crouch / Move / Idle every frame :191184-191187, Jump :195272, Melee :195909, Shoot :196418 and the
// server's "c" PLAYER_ANIM :257769) and the asset "uai" UPDATE_ASSET_ID swaps in (players.updateAssetID :197964).
//
// PlayerMesh.class_model holds it; player_mesh.c builds it with the rest of the model, scene.c adds it instead of the
// body, arms and weapons (which are still built: the anchors other code reads), and player_update_meshes poses it.
#include <client.h>
#include <gltf.h>

typedef struct PlayerClassModel {
    int mdl;                      // what it draws: 0 nothing, 1 Sprite, 2 Model
    int class_mdl;                // the class's config.mdl at the build (0 Default, 1 Sprite, 2 Model)
    char asset[32];              // the asset text it was built from (player.assetTxt || config.aID, "" for mdl 0)
    // KrunkNative: the default body draws instead (an FBX, a model file missing or broken: v10 would draw nothing)
    unsigned char default_body;
    unsigned char has_anims;      // player.hasAnims (config.aID holds g / q / f, deob-10.0.3.js:194877)
    unsigned char world;          // in the world scene for the local first-person model (render_you, scene_add_class_model)
    unsigned char has_last;       // last_position is set
    char pending[600];            // the model file that was still downloading at the build ("" none): rebuilt once it lands
    // RENDER.loadMesh / loadAnimMesh's mesh (:194251-194263): (0, y, 0) on objInstances (the anchor), rotation y mdlDr,
    // scale mdlS * mdPS; loadSprite's sprite sits at (0, 5.5, 0) with scale 11 (:234089-234099)
    MeshTransform holder;
    Mesh *mesh;                   // mdl 1: the sprite; mdl 2: the OBJ model (re-centred under the holder)
    GltfInstance *gltf;           // mdl 2: the GLTF / GLB model (loadAnimMesh)
    vec3 last_position;           // oldX / oldY / oldZ of players.update's Move / Idle pick
    char clip[24];                // the clip the GLTF shows: the last playClip that took ("" none; the `models` probe)
} PlayerClassModel;

// generateMeshes' branch for this player. render_you: the local first-person model; third_person: the model is seen
// from outside (thirdPerson, a camera offset or another player's: IÍïíiií, deob-10.0.3.js:194099). NULL: the default
// body (config.mdl 0 with the model shown). Otherwise a model that stands in for the body, which may draw nothing: v10
// builds nothing at all for our first-person model when the class has mdlFP off (:194228), and nothing while the
// asset loads. default_body set: KrunkNative draws the default body after all (an asset this build cannot draw).
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
// The model no longer matches the player: its asset was swapped (uai, player.assetID), the class's model changed, or
// its file came down (or failed) since the build. main.c rebuilds it (player_weapon_skin_ready).
int player_class_model_stale(const Player*, const PlayerClassModel*);
// The drawn GLTF class model of the player (meshRef with playClip), NULL when none
GltfInstance *player_class_model_gltf(const Player*);
// players.playClip's client branch (deob-10.0.3.js:193817-193819): an active player with hasAnims whose model is drawn
// plays `clip` `reps` times (meshRef.playClip(name, reps): 0 loops, < 0 is -1's Infinity)
void player_class_model_play_clip(Player*, const char *clip, float reps);
// KrunkScript's player.playAnim(clip, reps) / stopAnim() (clip NULL) on the drawn GLTF, with no hasAnims test
// (deob-10.0.3.js:208232-208244)
void player_class_model_script_clip(const Player*, const char *clip, int reps);
// One line about the player's model for the input script's `models` probe: "body=model mdl=2 asset=11441g clip=Idle"
// (body: default, model, none (nothing drawn), fallback (the default body for an asset this build cannot draw), - (no
// mesh: dead); " stalker=1" for a model built with the Stalker look)
void player_class_model_describe(const Player*, char *out, size_t n);
// players.updateAssetID on the client (deob-10.0.3.js:197964-197975): `if (config.mdl && assetTxt.length)` keep the
// asset for the player's model; 1 when it applies (the caller rebuilds an active player's model: regenMeshes)
int player_class_model_set_asset(Player*, const char *asset_txt);
// "uai" for a player this client does not know yet: KrunkNative's server runs KrunkScript's onPlayerSpawn before the
// SPAWN goes out, so a script's `player.assetID = ...` there reaches a new player's client first. It is kept for that
// uid and taken by its first model build (player_class_model_create). player_class_model_reset (INIT) drops them.
void player_class_model_hold_asset(int uid, const char *asset_txt);
void player_class_model_reset(void);
