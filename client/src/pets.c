// Krunker v7.1.4 pets (client/include/pets.h, v7 src/data/pets.js).
#include <pets.h>
#include <cosmetics.h>
#include <gltf.h>
#include <skin_fx.h>
#include "viewer/viewer_store.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

PlayerPet *pet_create(const ViewerSkin *item, const vec3 target, const int preview) {
    if (!item || item->type != ITEM_PET) return NULL;
    PlayerPet *pet = calloc(1, sizeof(PlayerPet));
    if (!pet) return NULL;
    pet->position = target;
    pet->speed = item->spd > 0.0f ? item->spd : PET_DEFAULT_SPEED;
    pet->preview = preview;
    pet->root.scale = (vec3) {1.0f, 1.0f, 1.0f};
    pet->root.position = target;

    // loadMesh / loadAnimMesh({src: "pets/pet_<id>", texSrc tex ? "pets/pet_<id>_<tex>"}, 0, 0, 0,
    // preview ? 0 : PI / 2, sclMlt || 0.8, this.mesh) with the item's material options
    char model[96], texture[128];
    cosmetic_asset_keys(item, model, sizeof(model), texture, sizeof(texture));
    const float scale = item->scl_mlt > 0.0f ? item->scl_mlt : PET_DEFAULT_SCALE;
    const vec3 rotation = {0.0f, preview ? 0.0f : (float) M_PI * 0.5f, 0.0f};

    if (item->flags & VS_HAS_ANIM) {
        char rel[160];
        snprintf(rel, sizeof(rel), "models/%s.%s", model, item->file_ext ? item->file_ext : "gltf");
        char *path = concat(assets_path(), rel);
        pet->model = path ? gltf_instance_create(gltf_model_load(path), item->anim_s > 0.0f ? item->anim_s : 1.0f) : NULL;
        free(path);
        if (pet->model) {
            pet->model->root.rotation = rotation;
            pet->model->root.scale = (vec3) {scale, scale, scale};
            pet->model->root.parent = &pet->root;
            skin_fx_apply_gltf(pet->model, item, model);
        }
    }

    if (!pet->model) {
        char rel[160];
        snprintf(rel, sizeof(rel), "models/%s.obj", model);
        char *path = concat(assets_path(), rel);
        Geometry *geometry = path ? load_obj_model(path, 0) : NULL;
        free(path);
        if (!geometry) {
            free(pet);
            return NULL;
        }
        snprintf(rel, sizeof(rel), "textures/%s.png", texture);
        path = concat(assets_path(), rel);
        const unsigned int texture_id = path ? load_texture(path) : 0;
        free(path);

        BasicMaterial *material = basic_material_init();
        if (!material) {
            free(pet);
            return NULL;
        }
        material->texture = texture_id;
        pet->mesh = mesh_init(geometry, (Material *) material);
        pet->mesh->transform.rotation = rotation;
        pet->mesh->transform.scale = (vec3) {scale, scale, scale};
        pet->mesh->transform.parent = &pet->root;
        // pets.js: glowText = glow, emissive (glow || sameGlow) ? emiss || #fff (no noEmis)
        const int glow = (item->flags & (VS_GLOW | VS_SAME_GLOW)) != 0;
        skin_fx_apply(pet->mesh, item, viewer_skin_tex_truthy(item) ? texture : NULL, model, (item->flags & VS_GLOW) != 0, glow);
    }
    return pet;
}

void pet_update(PlayerPet *pet, const vec3 target, const float dt_ms) {
    if (!pet || dt_ms <= 0.0f) return;
    // UTILS.getDrs(target, pet): [yaw, pitch] of the pet seen from the target
    const float dx = pet->position.x - target.x, dy = pet->position.y - target.y, dz = pet->position.z - target.z;
    const float yaw = -atan2f(-dx, dz);
    const float pitch = -atan2f(dy, sqrtf(dx * dx + dz * dz));
    const float distance = sqrtf(dx * dx + dy * dy + dz * dz);
    const float slow = distance < PET_SLOW_DISTANCE ? distance / PET_SLOW_DISTANCE : 1.0f;
    const float step = fminf(distance, pet->speed * dt_ms) * slow;
    // x += d * sin(yaw + PI) * cos(pitch), z += d * cos(yaw + PI) * cos(pitch), y += d * sin(pitch)
    pet->position.x += step * sinf(yaw + (float) M_PI) * cosf(pitch);
    pet->position.z += step * cosf(yaw + (float) M_PI) * cosf(pitch);
    pet->position.y += step * sinf(pitch);

    pet->bob_y += PET_BOB_SPEED * dt_ms;
    pet->root.position = (vec3) {pet->position.x, pet->position.y + sinf(pet->bob_y) * PET_BOB_HEIGHT, pet->position.z};

    // turn toward the travel direction by at most 0.01 * slow rad per ms
    const float two_pi = 2.0f * (float) M_PI;
    const float diff = fmodf(pet->root.rotation.y - yaw + two_pi, two_pi);
    const float turn = fminf(fminf(fabsf(diff - two_pi), diff), slow * PET_TURN_SPEED * dt_ms);
    const float sign = diff - (float) M_PI >= 0.0f ? 1.0f : -1.0f;
    pet->root.rotation.y = fmodf(pet->root.rotation.y + sign * turn + two_pi, two_pi);
}

void pet_set_visible(PlayerPet *pet, const int visible) {
    if (!pet) return;
    if (pet->mesh) pet->mesh->visible = visible;
    if (pet->model) gltf_instance_set_visible(pet->model, visible);
}

Scene *g_pet_world_scene;

void scene_add_pet(Scene *scene, PlayerPet *pet, const int render_you) {
    if (!pet || pet->scene) return;
    Scene *target = render_you && g_pet_world_scene ? g_pet_world_scene : scene;
    if (!target) return;
    if (pet->mesh) scene_add_mesh(target, pet->mesh);
    scene_add_gltf(target, pet->model);
    pet->scene = target;
}

void scene_remove_pet(PlayerPet *pet) {
    if (!pet || !pet->scene) return;
    if (pet->mesh) scene_remove_mesh(pet->scene, pet->mesh);
    scene_remove_gltf(pet->scene, pet->model);
    pet->scene = NULL;
}

void pet_free(PlayerPet *pet) {
    if (!pet) return;
    scene_remove_pet(pet);
    if (pet->mesh) {
        skin_fx_forget(pet->mesh);
        mesh_fini(pet->mesh);
    }
    if (pet->model) {
        skin_fx_forget_gltf(pet->model);
        gltf_instance_fini(pet->model);
    }
    free(pet);
}
