#include <client.h>
#include <cosmetics.h>
#include <render_settings.h>
#include <math.h>

typedef struct {
    int color;
    float height;
    float scale;
} ColorCubeSegment;

ColorCube *generate_color_cube(const float width, const float height, const float length, const ColorCubeSegment *segments, const size_t segment_count) {
    ColorCube *color_cube = calloc(1, sizeof(ColorCube));
    if (!color_cube) return NULL;

    color_cube->meshes = calloc(segment_count, sizeof(Mesh*));
    color_cube->mesh_count = segment_count;

    if (!color_cube->meshes) {
        free(color_cube);
        return NULL;
    }

    color_cube->transform.scale.x = width;
    color_cube->transform.scale.y = height;
    color_cube->transform.scale.z = length;

    float offset = 0.5f;

    for (size_t i = 0; i < segment_count; i++) {
        const ColorCubeSegment *segment = &segments[i];

        Geometry *geo = create_cube_geo();
        BasicMaterial *material = basic_material_init();

        Mesh *mesh = mesh_init(geo, (Material *) material);

        material->color = hex_to_vec(segments[i].color);

        mesh->transform.scale.x = segment->scale;
        mesh->transform.scale.z = segment->scale;
        mesh->transform.scale.y = segment->height;

        mesh->transform.position.y = offset - segment->height;
        mesh->transform.parent = &color_cube->transform;

        offset -= segment->height;
        color_cube->meshes[i] = mesh;
    }

    return color_cube;
}

PlayerArmMesh *generate_arm(const float x, const float y, const Weapon *weapon, const int is_left, const int shirt_color, const int sleeve_color, const int skin_color, const int third_person, const int left_handed) {
    PlayerArmMesh *arm = calloc(1, sizeof(PlayerArmMesh));
    if (!arm) return NULL;

    const float left_handed_mlt = left_handed ? -1.0f : 1.0f;

    const vec3 hold = {
        (is_left ? weapon->left_hold.x : weapon->right_hold.x) * left_handed_mlt,
        is_left ? weapon->left_hold.y : weapon->right_hold.y,
        (is_left ? weapon->left_hold.z : weapon->right_hold.z) + (third_person ? 0.0f : weapon->hold_distance_offset), // genArm: aD += us && hDstOff
    };

    const float arm_scale = game_constants.arm_scale * (third_person ? 1.0f : 0.68f);
    const float arm_length = MIN(game_constants.upper_arm_length + game_constants.lower_arm_length - 0.01f, sqrtf(
        powf(((weapon->offset.x * left_handed_mlt - hold.x) * (is_left && weapon->akimbo ? -1.0f : 1.0f) - x), 2.0f) +
        powf((weapon->offset.y + hold.y - y), 2.0f) +
        powf((weapon->offset.z - hold.z), 2.0f)
    ));

    float arm_angles[3];
    angles_from_sides(arm_length, game_constants.upper_arm_length, game_constants.lower_arm_length, arm_angles);

    if (third_person) {
        BasicMaterial *upper_material = basic_material_init();
        BasicMaterial *joint_material = basic_material_init();

        upper_material->color = joint_material->color = hex_to_vec(shirt_color);

        arm->upper = mesh_init(create_cube_geo(), (Material *) upper_material);
        arm->joint = mesh_init(create_cube_geo(), (Material *) joint_material);

        arm->upper->transform.scale.x = arm->upper->transform.scale.z = arm_scale;
        arm->upper->transform.scale.y = game_constants.upper_arm_length;

        arm->upper->transform.rotation.x = (float) -M_PI * 0.5f;
        arm->upper->transform.parent = &arm->anchor;

        const float joint_length = sqrtf(2.0f * arm_scale * 0.5f * arm_scale * 0.5f - 2.0f * arm_scale * 0.5f * arm_scale * 0.5f * cosf((float) M_PI - arm_angles[0]));
        const float joint_height = sqrtf(arm_scale * 0.5f * arm_scale * 0.5f - joint_length * 0.5f * joint_length * 0.5f) * 2.0f;

        arm->joint->transform.scale.x = arm_scale;
        arm->joint->transform.scale.y = joint_height;
        arm->joint->transform.scale.z = joint_length;

        arm->joint->transform.rotation.x = (float) M_PI * 0.5f - arm_angles[0] * 0.5f;

        arm->joint->transform.position.y = -joint_height * 0.5f * cosf(arm->joint->transform.rotation.x);
        arm->joint->transform.position.z = -game_constants.upper_arm_length - joint_height * 0.5f * sinf(arm->joint->transform.rotation.x);

        arm->joint->transform.parent = &arm->anchor;
    } else {
        BasicMaterial *material = basic_material_init();
        material->color = hex_to_vec(shirt_color);

        arm->extender = mesh_init(create_cube_geo(), (Material *) material);
        arm->extender->transform.parent = &arm->anchor;

        arm->extender->transform.scale.x = arm->extender->transform.scale.z = arm_scale;
        arm->extender->transform.scale.y = 20.0f;

        arm->extender->transform.rotation.x = (float) -M_PI * 0.5f - arm_angles[0];
        arm->extender->transform.position.z = -game_constants.upper_arm_length;
    }

    const ColorCubeSegment lower_segments[] = {
        {shirt_color, 0.65f, 1.0f},
        {sleeve_color, 0.15f, 1.1f},
        {skin_color, 0.2f, 1.0f},
    };

    arm->lower = generate_color_cube(arm_scale, game_constants.lower_arm_length, arm_scale, lower_segments, sizeof(lower_segments) / sizeof(lower_segments[0]));
    arm->lower->transform.parent = &arm->anchor;

    arm->lower->transform.rotation.x = (float) -M_PI * 0.5f - arm_angles[0];

    arm->lower->transform.position.y = -game_constants.lower_arm_length * 0.5f * cosf(arm->lower->transform.rotation.x);
    arm->lower->transform.position.z = -game_constants.upper_arm_length - game_constants.lower_arm_length * 0.5f * sinf(arm->lower->transform.rotation.x);

    arm->anchor.position.x = x - weapon->offset.x * left_handed_mlt;
    arm->anchor.position.y = y - weapon->offset.y;
    // genArms also pushes the merged first-person arms mesh forward by hDstOff inside the weapon group.
    arm->anchor.position.z = -weapon->offset.z + (third_person ? 0.0f : weapon->hold_distance_offset);
    arm->anchor.scale = (vec3) {1.0f, 1.0f, 1.0f};

    arm->anchor.rotation.x = -arm_angles[1] + atan2f(weapon->offset.y + hold.y - y, -(weapon->offset.z - hold.z));
    arm->anchor.rotation.y = atan2f(x - (weapon->offset.x * left_handed_mlt - hold.x) * (is_left && weapon->akimbo ? -1.0f : 1.0f), -(weapon->offset.z - hold.z));
    arm->anchor.rotation_order = ROTATION_ORDER_EXTRINSIC;
    arm->anchor.parent = &arm->pivot;
    arm->pivot.scale = (vec3) {1.0f, 1.0f, 1.0f};

    return arm;
}

PlayerArms *generate_arms(const Weapon *weapon, const int shirt_color, const int sleeve_color, const int skin_color, const int third_person, const int left_handed) {
    PlayerArms *arms = calloc(1, sizeof(PlayerArms));
    if (!arms) return NULL;

    const float offset = (-game_constants.chest_width + game_constants.arm_scale * 0.5f - game_constants.arm_inset) * (third_person ? 1.0f : weapon->hold_width ? weapon->hold_width : 0.4f);

    arms->left = generate_arm(offset, game_constants.arm_offset, weapon, !left_handed, shirt_color, sleeve_color, skin_color, third_person, left_handed);
    arms->right = generate_arm(-offset, game_constants.arm_offset, weapon,  left_handed, shirt_color, sleeve_color, skin_color, third_person, left_handed);

    if (!arms->left || !arms->right) {
        free(arms->left);
        free(arms->right);
        free(arms);

        return NULL;
    }

    arms->anchor.scale = (vec3) {1.0f, 1.0f, 1.0f};
    arms->left->pivot.parent = &arms->anchor;
    arms->right->pivot.parent = &arms->anchor;

    return arms;
}

ColorCube *generate_head(const int skin_color, const int hair_color) {
    const ColorCubeSegment segments[] = {
        {hair_color, 0.2f, 1.0f},
        {skin_color, 0.8f, 1.0f},
    };

    ColorCube *head = generate_color_cube(
        game_constants.head_scale,
        game_constants.head_scale,
        game_constants.head_scale,
        segments, sizeof(segments) / sizeof(segments[0])
    );

    if (head) head->transform.position.y = game_constants.body_height + game_constants.head_scale * 0.5f;
    return head;
}

ColorCube *generate_body(const int shirt_color, const int pants_color) {
    const ColorCubeSegment segments[] = {
        {shirt_color, 0.8f, 1.0f},
        {pants_color, 0.2f, 1.05f},
    };

    ColorCube *body = generate_color_cube(
        game_constants.chest_width,
        game_constants.body_height,
        game_constants.chest_scale,
        segments, sizeof(segments) / sizeof(segments[0])
    );

    if (body) body->transform.position.y = game_constants.body_height * 0.5f;
    return body;
}

ColorCube *generate_leg(const int is_left, const int pants_color, const int shoe_color) {
    const ColorCubeSegment segments[] = {
        {pants_color, 0.75f, 1.0f},
        {shoe_color, 0.25f, 1.0f}
    };

    ColorCube *leg = generate_color_cube(game_constants.leg_scale, game_constants.leg_height, game_constants.leg_scale, segments, sizeof(segments) / sizeof(segments[0]));

    if (leg) {
        leg->transform.position.x = game_constants.leg_scale * 0.5f * (is_left ? -1.0f : 1.0f);
        leg->transform.position.y = game_constants.leg_height * 0.5f;
    }

    return leg;
}

PlayerCrouchedLeg *generate_crouched_leg(const int is_left, const int pants_color, const int shoe_color) {
    PlayerCrouchedLeg *leg = calloc(1, sizeof(PlayerCrouchedLeg));
    if (!leg) return NULL;

    const float leg_angles[] = {2.0f, 0.5f}; // upper, lower

    BasicMaterial *upper_material = basic_material_init();
    BasicMaterial *joint_material = basic_material_init();

    upper_material->color = joint_material->color = hex_to_vec(pants_color);

    leg->upper = mesh_init(create_cube_geo(), (Material *) upper_material);
    leg->joint = mesh_init(create_cube_geo(), (Material *) joint_material);

    leg->upper->transform.scale.x = leg->upper->transform.scale.z = game_constants.leg_scale;
    leg->upper->transform.scale.y = game_constants.leg_height * 0.5f;

    leg->upper->transform.rotation.x = leg_angles[0];

    leg->upper->transform.position.y = -game_constants.leg_height * 0.5f * cosf(leg->upper->transform.rotation.x);
    leg->upper->transform.position.z = -game_constants.leg_height * 0.5f * sinf(leg->upper->transform.rotation.x);

    leg->upper->transform.parent = &leg->anchor;

    const float joint_length = sqrtf(2.0f * game_constants.leg_scale * 0.5f * game_constants.leg_scale * 0.5f - 2.0f * game_constants.leg_scale * 0.5f * game_constants.leg_scale * 0.5f * cosf((float) M_PI - (leg_angles[0] - leg_angles[1])));
    const float joint_height = sqrtf(game_constants.leg_scale * 0.5f * game_constants.leg_scale * 0.5f - joint_length * 0.5f * joint_length * 0.5f) * 2.0f;

    leg->joint->transform.scale.x = game_constants.leg_scale;
    leg->joint->transform.scale.y = joint_height;
    leg->joint->transform.scale.z = joint_length;

    leg->joint->transform.rotation.x = (leg_angles[0] + leg_angles[1]) * 0.5f;

    leg->joint->transform.position.y = -game_constants.leg_height * 0.5f * cosf(leg->upper->transform.rotation.x) - joint_height * 0.5f * cosf(leg->joint->transform.rotation.x);
    leg->joint->transform.position.z = -game_constants.leg_height * 0.5f * sinf(leg->upper->transform.rotation.x) - joint_height * 0.5f * sinf(leg->joint->transform.rotation.x);

    leg->joint->transform.parent = &leg->anchor;

    const ColorCubeSegment lower_segments[] = {
        {pants_color, 0.5f, 1.0f},
        {shoe_color, 0.5f, 1.0f},
    };

    leg->lower = generate_color_cube(game_constants.leg_scale, game_constants.leg_height * 0.5f, game_constants.leg_scale, lower_segments, sizeof(lower_segments) / sizeof(lower_segments[0]));
    leg->lower->transform.parent = &leg->anchor;

    leg->lower->transform.rotation.x = leg_angles[1];

    leg->lower->transform.position.y = -game_constants.leg_height * 0.5f * cosf(leg->upper->transform.rotation.x) - game_constants.leg_height * 0.25f * cosf(leg->lower->transform.rotation.x);
    leg->lower->transform.position.z = -game_constants.leg_height * 0.5f * sinf(leg->upper->transform.rotation.x) - game_constants.leg_height * 0.25f * sinf(leg->lower->transform.rotation.x);

    leg->anchor.scale = (vec3) {1.0f, 1.0f, 1.0f};

    leg->anchor.position.x = game_constants.leg_scale * 0.5f * (is_left ? -1.0f : 1.0f);
    leg->anchor.position.y = game_constants.leg_height - game_constants.crouch_distance + 0.5f;

    leg->anchor.rotation.y = is_left ? (float) M_PI / 8.0f : (float) -M_PI / 6.0f;

    return leg;
}

// First-person melee mesh hold on the right arm (95.js jLrR6, renderYou): weaponGeos xP/yP/zP and yR/zR.
static const vec3 g_fp_melee_position = {0.9f, -0.95f, 0.72f};
static const vec3 g_fp_melee_rotation = {(float) -M_PI / 3.5f, 0.3f, (float) M_PI * -0.9f};

// armMeshes[1] (right, holds the melee mesh): zR -0.3, xR -0.4, xP 0.4, yP 0.3; armMeshes[0]: xP 0.3, yP -0.5, zR -0.4.
static void player_melee_arms_base_pose(PlayerArms *arms) {
    arms->right->pivot.rotation = (vec3) {-0.4f, 0.0f, -0.3f};
    arms->right->pivot.position = (vec3) {0.4f, 0.3f, 0.0f};
    arms->left->pivot.rotation = (vec3) {0.0f, 0.0f, -0.4f};
    arms->left->pivot.position = (vec3) {0.3f, -0.5f, 0.0f};
}

// Per-frame first-person melee arm pose (95.js updateMsh armMeshes loop): step sway, crouch / jump tilt, the
// knife-throw wind-up while aiming, and the melee mesh hidden once the knife has been thrown.
// The inspect pose (inspectAn, weapon.inspAnim) applies while inspecting and not winding up a throw,
// otherwise the meleeAnim swing / throw / equip tweens (weapon.anim, swapWeapon) offset the right arm
// and the melee mesh. meleeAnim.lArm always moves the left arm.
static void player_update_melee_arms(const Player *player, PlayerArms *arms, const float step_anim, const float step_half_rotate) {
    const float *ma = player->melee_anim;
    player_melee_arms_base_pose(arms);

    // weaponGeos[i] back to its hold every frame (position xP/yP/zP, rotation yR/zR)
    if (arms->weapon_right) {
        arms->weapon_right->transform.position = g_fp_melee_position;
        arms->weapon_right->transform.rotation = g_fp_melee_rotation;
    }

    arms->left->pivot.position.z = step_anim;
    arms->right->pivot.position.z = -step_anim;

    // armMeshes[0] += meleeAnim.lArm reach / punch
    const float l_arm = ma[MELEE_ANIM_L_ARM];
    arms->left->pivot.position.z += l_arm;
    arms->left->pivot.position.y -= l_arm * 0.27f;
    arms->left->pivot.rotation.y += l_arm * 0.1f;
    arms->left->pivot.position.x -= l_arm * 0.3f;

    arms->right->pivot.rotation.z += player->crouch_val * 0.12f - step_half_rotate + player->jump_bob_y * 0.2f;

    if (arms->weapon_right) arms->weapon_right->visible = !!player->can_throw;

    if (player->aim_val > 0.0f && player->can_throw && player->weapon->can_throw) {
        const float wind_up = player->swap_timer > 0.0f ? 0.0f : player->aim_val; // 1 - jKFi4
        arms->right->pivot.rotation.z -= wind_up * 1.3f;
        arms->right->pivot.position.x += wind_up * 1.7f;
        arms->right->pivot.position.z += wind_up * 1.8f;
        arms->right->pivot.position.y += wind_up * 2.9f;
        arms->right->pivot.rotation.y += wind_up * 1.0f;
        arms->right->pivot.rotation.x += wind_up * 1.7f;
    } else if (player->inspecting && player->can_throw) {
        const float *an = player->inspect_anim; // arZ, arX, arY, apZ, apX, apY, wpZ, wpY, wrY
        const float hand = player->left_handed ? -1.0f : 1.0f;
        arms->right->pivot.rotation.z -= an[0];
        arms->right->pivot.rotation.x -= an[1] * hand;
        arms->right->pivot.rotation.y -= an[2];
        arms->right->pivot.position.z += an[3];
        arms->right->pivot.position.x += an[4] * hand;
        arms->right->pivot.position.y += an[5];

        if (arms->weapon_right) {
            arms->weapon_right->transform.position.z += an[6];
            arms->weapon_right->transform.position.y += an[7];
            arms->weapon_right->transform.rotation.y += an[8];
            // rotation.z += wpSp: 0 (4 PI spin only for flipAnim melee skins)
        }
    } else {
        const float r_arm = ma[MELEE_ANIM_R_ARM];
        arms->right->pivot.rotation.z += ma[MELEE_ANIM_ARM_R];
        arms->right->pivot.rotation.y += ma[MELEE_ANIM_ARM_T] - r_arm * 0.25f;
        arms->right->pivot.rotation.x += ma[MELEE_ANIM_ARM_S];
        arms->right->pivot.position.x += ma[MELEE_ANIM_ARM_M] + r_arm * 0.3f;
        arms->right->pivot.position.z += ma[MELEE_ANIM_ARM_E] + r_arm * 1.15f;
        arms->right->pivot.position.y += ma[MELEE_ANIM_ARM_Y] - r_arm * 0.3f;

        if (arms->weapon_right) {
            arms->weapon_right->transform.rotation.z += ma[MELEE_ANIM_WEA_R];
            arms->weapon_right->transform.position.x += ma[MELEE_ANIM_WEA_M];
            arms->weapon_right->transform.rotation.y += ma[MELEE_ANIM_FLIP_W];
        }
    }
}

// loadMesh({src: "attach/" + af.src, transparent: true}, 0, -(yOrg || 0) + (attachYOff || 0), -(attachZOff || 0), PI / 2, af.scale, group)
static Mesh *generate_attachment_mesh(const Weapon *weapon, const WeaponAttachment *attachment, MeshTransform *parent) {
    char model[128];
    char texture[128];
    snprintf(model, sizeof(model), "models/attach/%s.obj", attachment->src);
    snprintf(texture, sizeof(texture), "textures/attach/%s.png", attachment->src);

    char *model_path = concat(assets_path(), model);
    char *texture_path = concat(assets_path(), texture);
    Geometry *geometry = model_path ? load_obj_model(model_path, 0) : NULL;
    const unsigned int texture_id = geometry && texture_path ? load_texture(texture_path) : 0;
    free(model_path);
    free(texture_path);

    if (!geometry) return NULL;

    BasicMaterial *material = basic_material_init();
    material->texture = texture_id;
    material->base.transparent = 1;

    Mesh *mesh = mesh_init(geometry, (Material *) material);
    mesh->transform.position.x = 0.0f;
    mesh->transform.position.y = -weapon->origin.y + weapon->attach_y_offset;
    mesh->transform.position.z = -weapon->attach_z_offset;
    mesh->transform.rotation.y = (float) M_PI * 0.5f;
    mesh->transform.scale.x = mesh->transform.scale.y = mesh->transform.scale.z = attachment->scale ? attachment->scale : 1.0f;
    mesh->transform.parent = parent;

    return mesh;
}

// jLrR6: skinColIndex / hairCol replace the class's skin and hair colours, then a dye overrides any of
// skin / shirt / sleeve / pants / shoes / hair. colors[] is ClassConfig.colors order: skin, shirt, pants, shoes, hair, sleeve.
static void player_apply_cosmetic_colors(const PlayerCosmetics *cosmetics, int colors[6]) {
    if (cosmetics->skin_color >= 0) colors[0] = cosmetics->skin_color;
    if (cosmetics->hair_color >= 0) colors[4] = cosmetics->hair_color;

    const Cosmetic *dye = cosmetic_get(cosmetics->dye, COSMETIC_DYE);
    if (!dye) return; // TODO: dye textures / opacity / glow (a3.tex, a3.opac, a3.glow)

    if (dye->skin_col >= 0) colors[0] = dye->skin_col;
    if (dye->shirt_col >= 0) colors[1] = dye->shirt_col;
    if (dye->sleeve_col >= 0) colors[5] = dye->sleeve_col;
    if (dye->pants_col >= 0) colors[2] = dye->pants_col;
    if (dye->shoe_col >= 0) colors[3] = dye->shoe_col;
    if (dye->hair_col >= 0) colors[4] = dye->hair_col;
}

// loadMesh({src: "<dir>/<prefix><id>", texSrc: tex ? "<dir>/<prefix><id>_<tex>" : null, ...}, 0, 0, 0, PI / 2, scale, holder)
// inside genObj3D(0, y, -sitOffZ) on lowerBody. TODO: glow / emissive textures, rAnim spin, frame animation.
static Mesh *generate_cosmetic_mesh(const Cosmetic *item, const char *dir, const char *prefix, const float y, const float scale, MeshTransform *parent) {
    char model[96];
    char texture[96];
    snprintf(model, sizeof(model), "models/%s/%s%d.obj", dir, prefix, item->id);
    if (item->tex >= 0) snprintf(texture, sizeof(texture), "textures/%s/%s%d_%d.png", dir, prefix, item->id, item->tex);
    else snprintf(texture, sizeof(texture), "textures/%s/%s%d.png", dir, prefix, item->id);

    char *model_path = concat(assets_path(), model);
    char *texture_path = concat(assets_path(), texture);
    Geometry *geometry = model_path ? load_obj_model(model_path, 0) : NULL;
    const unsigned int texture_id = geometry && texture_path ? load_texture(texture_path) : 0;
    free(model_path);
    free(texture_path);

    if (!geometry) return NULL;

    BasicMaterial *material = basic_material_init();
    material->texture = texture_id;
    if (item->color >= 0) material->color = hex_to_vec(item->color);
    material->color.w = item->opacity;
    material->alpha_test = item->alpha_test;
    material->double_sided = item->double_sided;
    material->base.transparent = item->transparent;

    Mesh *mesh = mesh_init(geometry, (Material *) material);
    mesh->transform.position = (vec3) {0.0f, y, -item->sit_off_z};
    mesh->transform.rotation.y = (float) M_PI * 0.5f;
    mesh->transform.scale = (vec3) {scale, scale, scale};
    mesh->transform.parent = parent;
    return mesh;
}

static void player_generate_cosmetic_meshes(PlayerMesh *player_mesh, const PlayerCosmetics *cosmetics, const Cosmetic *hat, const Cosmetic *waist) {
    const float above_legs = game_constants.player_height - game_constants.leg_height; // yz3 - legHeight
    const Cosmetic *back = cosmetic_get(cosmetics->back, COSMETIC_BACK);
    const Cosmetic *face = cosmetic_get(cosmetics->face, COSMETIC_FACE);
    MeshTransform *body = player_mesh->body_anchor; // Krunker's lowerBody

    if (back) {
        player_mesh->cosmetics[PLAYER_COSMETIC_BACK] = generate_cosmetic_mesh(back, "body", "body_",
            (above_legs - game_constants.head_scale) / 2.0f - back->sit_off, back->scl_mlt * 2.1f, body);
    }

    if (waist) {
        player_mesh->cosmetics[PLAYER_COSMETIC_WAIST] = generate_cosmetic_mesh(waist, "waist", "waist_",
            (above_legs - game_constants.head_scale - 2.0f) / 2.0f - waist->sit_off, waist->scl_mlt * 2.1f, body);
    }

    if (hat) {
        player_mesh->cosmetics[PLAYER_COSMETIC_HAT] = generate_cosmetic_mesh(hat, "hats", "hat_", above_legs - hat->sit_off, hat->scl_mlt * 2.1f, body);
    }

    if (face) {
        player_mesh->cosmetics[PLAYER_COSMETIC_FACE] = generate_cosmetic_mesh(face, "faces", "face_", above_legs - face->sit_off, face->scl_mlt, body);
    }
}

void player_generate_meshes(Player *player, const int render_you) {
    if (player->mesh) return;
    player->render_you = render_you;

    int colors[6];
    memcpy(colors, player->game->classes[player->class_index].colors, sizeof(colors));
    player_apply_cosmetic_colors(&player->cosmetics, colors);

    // TODO: mode specific skins

    const int third_person = !render_you || player->game->config.third_person || player->game->map->config.cam_offset.x || player->game->map->config.cam_offset.y || player->game->map->config.cam_offset.z;

    PlayerMesh *player_mesh = calloc(1, sizeof(PlayerMesh));
    if (!player_mesh) return;

    player_mesh->anchor = calloc(1, sizeof(MeshTransform));
    player_mesh->body_anchor = calloc(1, sizeof(MeshTransform));
    player_mesh->upper_body_anchor = calloc(1, sizeof(MeshTransform));

    if (!player_mesh->anchor || !player_mesh->body_anchor || !player_mesh->upper_body_anchor) {
        free(player_mesh->anchor);
        free(player_mesh->body_anchor);
        free(player_mesh->upper_body_anchor);
        return;
    }

    player_mesh->body_anchor->position.y = game_constants.leg_height;
    player_mesh->body_anchor->parent = player_mesh->anchor;
    player_mesh->upper_body_anchor->parent = player_mesh->body_anchor;

    player_mesh->anchor->scale = player_mesh->body_anchor->scale = player_mesh->upper_body_anchor->scale = (vec3) {1.0f, 1.0f, 1.0f};
    player->mesh = player_mesh;

    if (player->game->map->config.model == MODEL_TYPE_SPRITE) {

    } else if (0 /* TODO: prop hunt */) {

    } else {
        // Cosmetic items only exist on third-person models (a4); hat noHead / waist noLegs replace body parts.
        const Cosmetic *hat = third_person ? cosmetic_get(player->cosmetics.hat, COSMETIC_HAT) : NULL;
        const Cosmetic *waist = third_person ? cosmetic_get(player->cosmetics.waist, COSMETIC_WAIST) : NULL;
        const int no_head = hat && hat->no_head;

        if (third_person) {
            player_mesh->body = generate_body(colors[1], colors[2]);
            player_mesh->head = !no_head ? generate_head(colors[0], colors[4]) : NULL;

            player_mesh->body->transform.parent = player_mesh->body_anchor;
            if (player_mesh->head) player_mesh->head->transform.parent = player_mesh->body_anchor;

            player_generate_cosmetic_meshes(player_mesh, &player->cosmetics, hat, waist);
        }

        const int no_legs = waist && waist->no_legs;

        if (third_person && !no_legs) {
            for (int i = 0; i < 2; i++) {
                ColorCube *leg = generate_leg(i, colors[2], colors[3]);

                leg->transform.parent = player_mesh->anchor;
                player_mesh->legs[i] = leg;
            }

            for (int i = 0; i < 2; i++) {
                PlayerCrouchedLeg *leg = generate_crouched_leg(i, colors[2], colors[3]);

                leg->anchor.parent = player_mesh->anchor;
                player_mesh->crouched_legs[i] = leg;
            }
        }

        player_mesh->arms = calloc(player->loadout_size, sizeof(PlayerArms *));

        if (player_mesh->arms) {
            for (int i = 0; i < player->loadout_size; i++) {
                const Weapon *weapon = player->game->weapons[player->loadout[i]];

                PlayerArms *arms = generate_arms(weapon, colors[1], colors[5], colors[0], third_person, player->left_handed);
                if (!arms) continue;

                arms->anchor.parent = player_mesh->upper_body_anchor;
                player_mesh->arms[i] = arms;

                // MELEE MESH
                if (weapon->melee) {
                    const char *model = "models/melee/melee_0.obj";
                    const char *texture = "textures/melee/melee_0.png";

                    char *model_path = concat(assets_path(), model);
                    char *texture_path = concat(assets_path(), texture);

                    Geometry *melee_geo = load_obj_model(model_path, 0);
                    const unsigned int melee_texture = load_texture(texture_path);

                    free(model_path);
                    free(texture_path);

                    if (melee_geo && melee_texture) {
                        BasicMaterial *melee_mat = basic_material_init();
                        Mesh *melee = mesh_init(melee_geo, (Material *) melee_mat);

                        melee_mat->texture = melee_texture;
                        melee->transform.parent = &arms->anchor;

                        // First person (no src + renderYou) builds the two arms as separate meshes with a base
                        // pose and hangs the melee mesh off the right one (95.js jLrR6 armMeshes).
                        if (render_you) {
                            arms->melee_pose = 1;
                            player_melee_arms_base_pose(arms);
                            melee->transform.parent = &arms->right->pivot;
                        }

                        // TODO: skin hold offsets
                        melee->transform.position = render_you ? g_fp_melee_position : (vec3) {1.7f, -0.4f, 1.2f};

                        // TODO: skin rotation
                        melee->transform.rotation = g_fp_melee_rotation;
                        if (!render_you) melee->transform.rotation.y = (float) M_PI * 0.5f;

                        melee->transform.scale.x = melee->transform.scale.y = melee->transform.scale.z = 0.9f; // ae.sclMlt || 0.9

                        arms->weapon_right = melee;
                    }
                }

                if (weapon->src) {
                    for (int j = 0; j < 2; j++) {
                        const size_t model_length = snprintf(NULL, 0, "models/weapons/%s.obj", weapon->src);
                        char *model = malloc(model_length + 1);

                        const size_t texture_length = snprintf(NULL, 0, "textures/weapons/%s.png", weapon->src);
                        char *texture = malloc(texture_length + 1);

                        char *model_path = NULL;
                        char *texture_path = NULL;

                        if (model) {
                            snprintf(model, model_length + 1, "models/weapons/%s.obj", weapon->src);
                            model_path = concat(assets_path(), model);

                            free(model);
                        }

                        if (texture) {
                            snprintf(texture, texture_length + 1, "textures/weapons/%s.png", weapon->src);
                            texture_path = concat(assets_path(), texture);

                            free(texture);
                        }

                        if (!model_path || !texture_path) continue;

                        Geometry *weapon_geo = load_obj_model(model_path, 0);
                        const unsigned int weapon_tex = load_texture(texture_path);

                        free(texture_path);
                        free(model_path);

                        if (!weapon_geo) continue;

                        BasicMaterial *weapon_mat = basic_material_init();
                        Mesh *weapon_mesh = mesh_init(weapon_geo, (Material *) weapon_mat);

                        weapon_mat->texture = weapon_tex;

                        weapon_mesh->transform.position.x = j ? weapon->offset.x * -2.0f : 0.0f;
                        weapon_mesh->transform.position.y = 0.0f; // TODO: weapon skin y offset
                        weapon_mesh->transform.position.z = 0.0f; // TODO: weapon skin z offset

                        // loadMesh(..., ad.rot || PI / 2, ae.mScl || ad.scale): scale falls back to 1, rot is a y angle or an [x, y, z] array.
                        const float weapon_scale = weapon->scale ? weapon->scale : 1.0f;
                        weapon_mesh->transform.scale.x = weapon_mesh->transform.scale.y = weapon_mesh->transform.scale.z = weapon_scale;

                        if (weapon->has_rotation) weapon_mesh->transform.rotation = weapon->rotation;
                        else weapon_mesh->transform.rotation.y = (float) M_PI * 0.5f;
                        weapon_mesh->transform.parent = &arms->anchor;

                        if (j) arms->weapon_left = weapon_mesh;
                        else arms->weapon_right = weapon_mesh;

                        if (!weapon->akimbo) break;
                    }
                }

                // Sight attachment: only on the first-person (U) and preview (V) models, like jLrR6.
                const WeaponAttachment *attachment = weapon_attachment(weapon, DEFAULT_ATTACHMENT_INDEX);
                if (attachment && (render_you || player->uid == -1)) {
                    arms->attachment = generate_attachment_mesh(weapon, attachment, &arms->anchor);
                }

                // TODO: flaps
            }
        }
    }
}

// The arm meshes only (extender / upper / joint / lower), not the weapon.
static void player_arm_parts_set_visible(const PlayerArms *arms, const int visible) {
    if (!arms) return;

    for (int j = 0; j < 2; j++) {
        const PlayerArmMesh *arm = j ? arms->left : arms->right;
        if (!arm) continue;

        if (arm->extender) arm->extender->visible = visible;
        if (arm->upper) arm->upper->visible = visible;
        if (arm->joint) arm->joint->visible = visible;

        for (size_t k = 0; arm->lower && k < arm->lower->mesh_count; k++) {
            arm->lower->meshes[k]->visible = visible;
        }
    }
}

static void player_arms_set_visible(const PlayerArms *arms, const int visible) {
    if (!arms) return;

    if (arms->weapon_right) arms->weapon_right->visible = visible;
    if (arms->weapon_left) arms->weapon_left->visible = visible;
    if (arms->attachment) arms->attachment->visible = visible;

    for (int j = 0; j < 2; j++) {
        const PlayerArmMesh *arm = j ? arms->left : arms->right;
        if (!arm) continue;

        if (arm->extender) arm->extender->visible = visible;
        if (arm->upper) arm->upper->visible = visible;
        if (arm->joint) arm->joint->visible = visible;

        for (size_t k = 0; arm->lower && k < arm->lower->mesh_count; k++) {
            arm->lower->meshes[k]->visible = visible;
        }
    }
}

void player_update_meshes(Player *player, const int is_preview) {
    if (!player->mesh) return;

    const PlayerMesh *player_mesh = player->mesh;

    // Viewmodel settings (v3.1.1 settings.js -> RENDER.aimAnim/weaponBob/weaponLean/
    // weaponRot/weaponOffX..Z/weaponADSOffY; weaponBob defaults to 0.9).
    const ClientRenderSettings *settings = client_render_settings();
    const int animate_aim = settings->aim_anim;
    const float weapon_bobbing_mlt = settings->weapon_bob;
    const float weapon_lean_mlt = settings->weapon_lean;
    const float weapon_rotation = settings->weapon_rot;
    const vec3 weapon_offset_mlt = {settings->weapon_off_x, settings->weapon_off_y, settings->weapon_off_z};

    const int third_person = player->game->config.third_person || player->game->map->config.cam_offset.x || player->game->map->config.cam_offset.y || player->game->map->config.cam_offset.z;
    const float aim_val_mlt = animate_aim && player->is_you ? player->aim_val : 0.0f;
    const float aim_mlt = player->weapon->animate_while_aiming ? 0.0f : aim_val_mlt;
    const float anim_mlt = (1.0f - (1.0f - game_constants.aim_anim_mlt) * aim_val_mlt) * game_constants.anim_mlt * weapon_bobbing_mlt;
    const float anim_mlt_crouch = 1.0f - player->crouch_val * 0.8f;
    const float anim_mlt_lean = player->render_you ? (1.0f - aim_val_mlt) * game_constants.anim_mlt : 0.0f; // a3 = Z * animMult
    // JS: (1 - (1 - Z) * (weapon.recoilYMA || 1)) * animMult; no v3.1.1 weapon defines recoilYMA.
    const float recoil_y_mlt = (1.0f - aim_val_mlt) * game_constants.anim_mlt;
    const float recoil_z_mlt = 1.0f - (player->weapon->recoil_z_mlt ? player->weapon->recoil_z_mlt : 0.5f) * aim_mlt;
    const float anim_rotate_mlt = (1.0f - (player->weapon->z_rotation ? player->weapon->z_rotation : 0.3f) * aim_mlt) * (player->weapon->z_rotation_mlt ? player->weapon->z_rotation_mlt : 1.0f) * weapon_bobbing_mlt;
    const float anim_y_mlt = 1.0f - (player->weapon->jump_y_mlt ? player->weapon->jump_y_mlt : 1.0f) * aim_mlt;
    const float lean_aim_mlt = 1.0f - aim_mlt * 0.45f; // a8 = 1 - (1 - a0) * 0.45
    const float bob_anim_y = player->bob_anim.y * 0.9f * anim_y_mlt * anim_mlt_lean * weapon_bobbing_mlt;
    const float land_bob_y = (player->land_bob_y * (player->weapon->land_bob ? player->weapon->land_bob : 1.0f) * 0.6f) * (1.0f - aim_mlt * 0.75f) * weapon_bobbing_mlt;

    if (player->land_bob_yr != land_bob_y) {
        player->land_bob_yr += (land_bob_y - player->land_bob_yr) * 0.1f;
    }

    const float land_bob_ya = player->land_bob_y * (player->weapon->land_bob ? player->weapon->land_bob : 1.0f) * 0.1f;
    const float bob_crouch_mlt = 1.0f - player->crouch_val * 0.5f;
    const float jump_rotate = player->jump_rotate * bob_crouch_mlt * anim_mlt_lean * weapon_bobbing_mlt;

    if (player->jump_rotate_mlt != jump_rotate) {
        player->jump_rotate_mlt += (jump_rotate - player->jump_rotate_mlt) * 0.08f;
    }

    const float jump_bob_y = player->jump_bob_y * (player->weapon->jump_bob_y_mlt ? player->weapon->jump_bob_y_mlt : 1.0f) * anim_mlt_lean * bob_crouch_mlt * weapon_bobbing_mlt;
    const float recoil_aim_mlt = 1.0f - aim_mlt * 0.75f; // af = 1 - (1 - a0) * 0.75
    const float recoil_mlt = 1.0f - (player->weapon->aim_recoil_mlt ? player->weapon->aim_recoil_mlt : 1.0f) * aim_mlt;
    const float step_mlt = is_preview ? 0.05f : game_constants.step_anim;
    float step_anim = sinf(player->step_val) * step_mlt;
    float step_half = cosf(2.0f * player->step_val) * 0.5f * step_mlt;
    const float step_anim_rotate = -sinf(player->step_chase) * step_mlt;
    const float step_half_rotate = -cosf(2.0f * player->step_chase) * 0.5f * step_mlt;
    const float aim_val = third_person ? 0.0f : aim_val_mlt;
    const float aim_lean = (aim_val <= 0.5f ? aim_val : 0.5f - (aim_val - 0.5f)) * (player->weapon->aim_lean ? player->weapon->aim_lean : 0.5f); // (aimLean || 0.5)
    const float swap_anim = player->weapon->swap_time > 0.0f ? player->swap_timer / player->weapon->swap_time : 0.0f;
    const float left_hand_mlt = player->left_handed ? -1.0f : 1.0f;

    const vec3 weapon_offset = {
        player->weapon->offset.x * (player->render_you ? weapon_offset_mlt.x : 1.0f) * left_hand_mlt,
        player->weapon->offset.y * (player->render_you ? weapon_offset_mlt.y : 1.0f),
        player->weapon->offset.z * (player->render_you ? weapon_offset_mlt.z : 1.0f),
    };

    const float reload_speed = player->game->config.reload_speed > 0.0f ? player->game->config.reload_speed : 1.0f; // (reSpd || 1)
    float reload_anim = player->reload_timer > 0.0f && player->weapon->reload_time > 0.0f ? 1.0f - player->reload_timer / (player->weapon->reload_time * reload_speed) : 0.0f;
    if (reload_anim > 0.5f) reload_anim = 1.0f - reload_anim;

    reload_anim *= player->render_you ? 1.0f : 0.3f;

    const float idle_anim = (1.0f - aim_val_mlt * 0.88f) * 1.75f * weapon_bobbing_mlt;
    const float step_y = player->render_you && !third_person ? fabsf(step_anim_rotate * 0.5f) * anim_mlt_lean : fabsf(step_anim * 3.5f);
    const float step_yaw = player->render_you ? third_person ? -step_anim * 0.5f : 0.0f : -step_anim * 2.0f;

    // Every player's model follows its (server-updated) position; only the viewmodel offsets below are first-person.
    player_mesh->anchor->position = player->position;
    player_mesh->anchor->position.y += step_y;

    if (player->game->map->config.model != MODEL_TYPE_SPRITE) { // TODO: check that is not prop
        player_mesh->anchor->rotation.y = player->direction.y + step_yaw;
    }

    step_half -= step_half * player->crouch_val * game_constants.crouch_anim_mlt;
    step_anim -= step_anim * player->crouch_val * game_constants.crouch_anim_mlt;

    if (!player->render_you) {
        for (int i = 0; i < 4; i++) {
            // waist items with noLegs (and first-person meshes) have no legs to swing
            if (i < 2 ? !player_mesh->legs[i] : !player_mesh->crouched_legs[i - 2]) continue;
            MeshTransform *leg_anchor = i < 2 ? &player_mesh->legs[i]->transform : &player_mesh->crouched_legs[i - 2]->anchor;
            leg_anchor->rotation.x = step_anim * (i == 1 || i == 3 ? 1.0f : -1.0f) * 7.0f + (i > 1 ? -0.6f : 0.0f);
        }
    }

    for (size_t i = 0; i < player->loadout_size; i++) {
        // TODO: update arms
    }

    // TODO: attachment & flap
    // TODO: upper body
    // TODO: weapon meshes

    const float reload_mlt = third_person ? 0.4f : 1.0f;

    player_mesh->upper_body_anchor->rotation.x = bob_anim_y * -0.2f + land_bob_ya + reload_anim * (reload_mlt * -2.8f) +
        player->direction.x * (player->render_you && !third_person ? 1.0f : 0.5f) +
        ((float) -M_PI * 0.25f * swap_anim + player->recoil_anim_y * game_constants.recoil_mlt) +
        (player->weapon->y_rotation ? player->weapon->y_rotation : 0.0f);

    player_mesh->upper_body_anchor->rotation.y = reload_anim * -reload_mlt;
    player_mesh->upper_body_anchor->rotation.z = 0.35f * weapon_rotation;

    player_mesh->upper_body_anchor->position.x = player_mesh->upper_body_anchor->position.z = 0.0f;
    player_mesh->upper_body_anchor->position.y = player->recoil_anim_y * (player->weapon->recoil_y_mlt ? player->weapon->recoil_y_mlt : 0.3f) * recoil_y_mlt + (player->render_you && !third_person ? player->height : game_constants.player_height) - game_constants.camera_height - game_constants.leg_height;

    MeshTransform *arm_anchor = &player_mesh->arms[player->loadout_index]->anchor;

    PlayerArms *held_arms = player_mesh->arms[player->loadout_index];
    if (held_arms && held_arms->melee_pose && player->weapon->melee) {
        player_update_melee_arms(player, held_arms, step_anim, step_half_rotate);
    }

    // aA = attach && attach.aimOffY + (weapon.aimOffY || 0) || 0 (TODO: settings weaponADSOffY scaling)
    const WeaponAttachment *attachment = weapon_attachment(player->weapon, DEFAULT_ATTACHMENT_INDEX);
    float aim_offset_y = attachment ? attachment->aim_offset_y + player->weapon->aim_offset_y : 0.0f;
    // weaponADSOffY: if (renderYou && weaponADSOffY != 1) aA = (aA || 0.1) * weaponADSOffY
    if (player->render_you && settings->weapon_ads_off_y != 1.0f) aim_offset_y = (aim_offset_y ? aim_offset_y : 0.1f) * settings->weapon_ads_off_y;

    // toggleAim: a `scope` weapon (Sniper Rifle) hides its viewmodel once fully aimed so the scope reticle is clear.
    if (player->weapon->scope && player->render_you && !third_person) {
        player_arms_set_visible(player_mesh->arms[player->loadout_index], aim_val_mlt < 1.0f);
    }

    // showHands (index.js updateWeaponShow(0, v)): the first-person arms of the weapon in
    // hand hide while the weapon itself stays (also under the scope rule above).
    if (player->render_you && !third_person && player->loadout_index >= 0 && player->loadout_index < player->loadout_size) {
        const int scoped_out = player->weapon->scope && aim_val_mlt >= 1.0f;
        player_arm_parts_set_visible(player_mesh->arms[player->loadout_index], settings->show_hands && !scoped_out);
    }

    arm_anchor->rotation.x = -cosf(player->idle_anim) * bob_crouch_mlt * 0.01f * idle_anim +
        player->weapon->rotation_offset * anim_mlt_lean + player->weapon->rotation_offset_aim * (1.0f - anim_mlt_lean) -
        player->swap_tween_r * 0.35f * anim_mlt_lean -
        player->land_bob_yr * 0.4f +
        player->recoil_tween[0] * recoil_mlt + // recoilTweenY * ag
        player->lean_anim.y * lean_aim_mlt * (player->weapon->lean_mlt ? player->weapon->lean_mlt : 1.0f) * weapon_lean_mlt +
        step_half_rotate * -0.9f * anim_mlt;

    arm_anchor->rotation.y = player->inspect_x * left_hand_mlt +
        player->jump_rotate_mlt * 0.2f +
        player->lean_anim.x * lean_aim_mlt * (player->weapon->lean_mlt ? player->weapon->lean_mlt : 1.0f) * weapon_lean_mlt +
        (-step_anim_rotate * 0.16f * anim_mlt_lean * anim_mlt_crouch + player->lean_anim.z * 0.2f) * anim_mlt;

    arm_anchor->rotation.z = jump_rotate + aim_lean + player->recoil_tween[1] * recoil_mlt + // recoilTweenR * ag
        player->lean_anim.z * 0.7f * anim_rotate_mlt +
        -player->inspect_x * left_hand_mlt * player->weapon->inspect_rotation +
        player->weapon->crouch_lean * left_hand_mlt * player->crouch_val * anim_mlt_lean * anim_mlt;

    arm_anchor->position.x = player->recoil.x * recoil_aim_mlt +
        player->swap_tween_r * 0.2f -
        player->jump_rotate_mlt * anim_mlt_lean * 1.3f -
        player->inspect_x * left_hand_mlt * player->weapon->inspect_mlt +
        (player->lean_anim.z * 0.35f - player->weapon->crouch_rotation * left_hand_mlt * player->crouch_val * anim_mlt_lean + step_anim * 0.5f * anim_mlt_crouch * anim_mlt_lean) * (1.0f - aim_val_mlt) * anim_mlt + // (...) * Z * a1
        weapon_offset.x - (weapon_offset.x - player->weapon->origin.x * left_hand_mlt) * aim_val;

    arm_anchor->position.y = player->recoil.z * recoil_aim_mlt +
        sinf(player->idle_anim) * 0.02f * idle_anim +
        player->recoil_tween[2] * recoil_mlt + // recoilTweenYM * ag
        jump_bob_y + land_bob_y * 0.5f - bob_anim_y * 1.5f -
        (player->weapon->bomb ? player->interact_progress * 3.0f : 0.0f) +
        (step_half * 0.85f - player->weapon->crouch_drop * player->crouch_val * anim_mlt_lean) * anim_mlt +
        weapon_offset.y - (weapon_offset.y - player->weapon->origin.y + 0.0f /* TODO: weapon skin y origin */ + aim_offset_y) * aim_val;

    arm_anchor->position.z = weapon_offset.z - (weapon_offset.z - player->weapon->origin.z) * aim_val +
        player->recoil_tween[3] + player->bob_anim.z * anim_mlt + player->recoil_anim * player->weapon->recoil_z * recoil_z_mlt;


    if (!player->render_you || third_person) {
        const float crouch_distance = game_constants.crouch_distance * player->crouch_val;
        const float crouch_lean = game_constants.crouch_lean * player->crouch_val;

        player_mesh->body_anchor->rotation.y = player_mesh->body_anchor->rotation.z = 0.0f;
        player_mesh->body_anchor->rotation.x = crouch_lean + player->direction.x * 0.5f;

        player_mesh->upper_body_anchor->rotation.x -= crouch_lean;

        player_mesh->body_anchor->position.x = player_mesh->body_anchor->position.z = 0.0f;
        player_mesh->body_anchor->position.y = game_constants.leg_height - crouch_distance;

        for (int i = 0; i < 2; i++) {
            ColorCube *leg = player_mesh->legs[i];
            if (!leg) continue;

            for (size_t j = 0; j < leg->mesh_count; j++) {
                leg->meshes[j]->visible = !player->crouch_val;
            }
        }

        for (int i = 0; i < 2; i++) {
            PlayerCrouchedLeg *leg = player_mesh->crouched_legs[i];
            if (!leg) continue;

            leg->upper->visible = !!player->crouch_val;
            leg->joint->visible = !!player->crouch_val;

            for (size_t j = 0; j < leg->lower->mesh_count; j++) {
                leg->lower->meshes[j]->visible = !!player->crouch_val;
            }
        }
    }
}

void player_meshes_fini(Player *player) {
    if (!player->mesh) return;

    PlayerMesh *player_mesh = player->mesh;

    if (player_mesh->body) {
        for (size_t i = 0; i < player_mesh->body->mesh_count; i++) {
            mesh_fini(player_mesh->body->meshes[i]);
        }

        free(player_mesh->body);
    }

    if (player_mesh->head) {
        for (size_t i = 0; i < player_mesh->head->mesh_count; i++) {
            mesh_fini(player_mesh->head->meshes[i]);
        }

        free(player_mesh->head);
    }

    if (player_mesh->arms) {
        for (size_t i = 0; i < player->loadout_size; i++) {
            PlayerArms *arms = player_mesh->arms[i];
            if (!arms) continue;

            if (arms->weapon_right) mesh_fini(arms->weapon_right);
            if (arms->weapon_left) mesh_fini(arms->weapon_left);
            if (arms->attachment) mesh_fini(arms->attachment);

            for (int j = 0; j < 2; j++) {
                const PlayerArmMesh *arm = j ? arms->left : arms->right;

                if (arm->extender) mesh_fini(arm->extender);

                if (arm->upper) mesh_fini(arm->upper);
                if (arm->joint) mesh_fini(arm->joint);

                for (size_t k = 0; k < arm->lower->mesh_count; k++) {
                    mesh_fini(arm->lower->meshes[k]);
                }
            }

            free(arms);
        }

        free(player_mesh->arms);
    }

    for (int i = 0; i < 2; i++) {
        ColorCube *leg = player_mesh->legs[i];
        if (!leg) continue;

        for (size_t j = 0; j < leg->mesh_count; j++) {
            mesh_fini(leg->meshes[j]);
        }

        free(leg);
    }

    for (int i = 0; i < 2; i++) {
        PlayerCrouchedLeg *leg = player_mesh->crouched_legs[i];
        if (!leg) continue;

        mesh_fini(leg->upper);
        mesh_fini(leg->joint);

        for (size_t j = 0; j < leg->lower->mesh_count; j++) {
            mesh_fini(leg->lower->meshes[j]);
        }

        free(leg);
    }

    for (int i = 0; i < PLAYER_COSMETIC_SLOTS; i++) {
        if (player_mesh->cosmetics[i]) mesh_fini(player_mesh->cosmetics[i]);
    }

    free(player_mesh->upper_body_anchor);
    free(player_mesh->body_anchor);
    free(player_mesh->anchor);
    free(player_mesh);

    player->mesh = NULL;
}