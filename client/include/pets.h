#pragma once
// Krunker v7.1.4 pets (src/data/pets.js): a client-side follower per player that wears a pet item
// (store type 9, petIndex). No server state: every client moves its own copy toward the owner's
// petHolder (genObj3D(3, player.height, 0) on the player's objInstances: 3 units to its right at
// player height), at spd (0.12 per ms by default) slowed down linearly within 40 units, bobbing
// sin(bobY) * 0.2 (bobY += 0.002 * dt) and turning toward its travel by at most 0.01 rad per ms.
// The model is "pets/pet_<id>" (OBJ, or the hasAnim GLTF), scale sclMlt || 0.8, turned PI / 2
// (0 for a preview pet), with the item's material effects (skin_fx.c).
#include <client.h>

struct ViewerSkin;
struct GltfInstance;

typedef struct PlayerPet {
    MeshTransform root;            // this.mesh: the follower in world (scene) space
    Mesh *mesh;                    // the OBJ model, NULL for a GLTF pet
    struct GltfInstance *model;    // the hasAnim model, NULL for an OBJ pet
    vec3 position;                 // this.x / y / z
    float speed;                   // spd || 0.12 (per ms)
    float bob_y;                   // bobY
    int preview;                   // Game.pets.add(player, true): menu / viewer preview pet
    Scene *scene;                  // the scene it was added to (scene_add_player_mesh), NULL = none
} PlayerPet;

// The world scene (Client.scene): where the local player's pet goes, since its own model is in the
// first-person scene (scene_add_player_mesh of a PlayerMesh with render_you). NULL: the same scene.
extern Scene *g_pet_world_scene;

#define PET_DEFAULT_SPEED 0.12f
#define PET_DEFAULT_SCALE 0.8f
#define PET_BOB_SPEED 0.002f
#define PET_BOB_HEIGHT 0.2f
#define PET_SLOW_DISTANCE 40.0f
#define PET_TURN_SPEED 0.01f
#define PET_HOLDER_X 3.0f          // petHolder genObj3D(3, player.height, 0)

// Pet.init: a pet of `item` (type 9) starting at `target`; NULL when the model is missing.
PlayerPet *pet_create(const struct ViewerSkin *item, vec3 target, int preview);
// Pet.update(delta): follow `target` (the petHolder's world position) for dt_ms.
void pet_update(PlayerPet *pet, vec3 target, float dt_ms);
void pet_set_visible(PlayerPet *pet, int visible);
// scene_add_player_mesh / scene_remove_player_mesh: add the pet to `scene` (or g_pet_world_scene
// for a first-person model) once, and remove it from wherever it was added.
void scene_add_pet(Scene *scene, PlayerPet *pet, int render_you);
void scene_remove_pet(PlayerPet *pet);
void pet_free(PlayerPet *pet);
