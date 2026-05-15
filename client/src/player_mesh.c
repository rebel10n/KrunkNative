#include <client.h>
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
        is_left ? weapon->left_hold.z : weapon->right_hold.z,
    };

    const float arm_scale = game_constants.arm_scale * (third_person ? 1.0f : 0.68f);
    const float arm_length = MIN(game_constants.upper_arm_length + game_constants.lower_arm_length - 0.01f, sqrtf(
        powf((weapon->offset.x * left_handed_mlt - hold.x - x), 2.0f) +
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
    arm->anchor.position.z = -weapon->offset.z;
    arm->anchor.scale = (vec3) {1.0f, 1.0f, 1.0f};

    arm->anchor.rotation.x = -arm_angles[1] + atan2f(weapon->offset.y + hold.y - y, -(weapon->offset.z - hold.z));
    arm->anchor.rotation.y = atan2f(x - (weapon->offset.x * left_handed_mlt - hold.x), -(weapon->offset.z - hold.z));
    arm->anchor.rotation_order = ROTATION_ORDER_EXTRINSIC;

    return arm;
}

PlayerArms *generate_arms(const Weapon *weapon, const int shirt_color, const int sleeve_color, const int skin_color, const int third_person, const int left_handed) {
    PlayerArms *arms = calloc(1, sizeof(PlayerArms));
    if (!arms) return NULL;

    const float offset = (game_constants.chest_width - game_constants.arm_scale * 0.5f + game_constants.arm_inset) * (third_person ? 1.0f : 0.4f);

    arms->right = generate_arm(offset, game_constants.arm_offset, weapon,  left_handed, shirt_color, sleeve_color, skin_color, third_person, left_handed);
    arms->left = generate_arm(-offset, game_constants.arm_offset, weapon, !left_handed, shirt_color, sleeve_color, skin_color, third_person, left_handed);

    if (!arms->left || !arms->right) {
        free(arms->left);
        free(arms->right);
        free(arms);

        return NULL;
    }

    arms->anchor.position.z += third_person ? 0.0f : weapon->hold_distance_offset;

    arms->anchor.scale = (vec3) {1.0f, 1.0f, 1.0f};
    arms->left->anchor.parent = &arms->anchor;
    arms->right->anchor.parent = &arms->anchor;

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

void player_generate_meshes(Player *player, const int render_you) {
    if (player->mesh) return;
    player->render_you = render_you;

    int colors[6];
    memcpy(colors, player->game->classes[player->class_index].colors, sizeof(colors));

    // TODO: dye colors
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
        int no_head = 0;

        if (third_person) {
            player_mesh->body = generate_body(colors[1], colors[2]);
            player_mesh->head = !no_head ? generate_head(colors[0], colors[4]) : NULL;

            player_mesh->body->transform.parent = player_mesh->body_anchor;
            player_mesh->head->transform.parent = player_mesh->body_anchor;
        }

        int no_legs = 0;

        // TODO: back skin
        // TODO: waist skin
        // TODO: hat skin
        // TODO: face skin

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

                // ARM GENERATION
                PlayerArms *arms = generate_arms(weapon, colors[1], colors[5], colors[0], third_person, player->left_handed);

                if (arms) {
                    arms->anchor.parent = player_mesh->upper_body_anchor;
                    player_mesh->arms[i] = arms;
                }

                // MELEE MESH
                if (weapon->melee) {
                    // TODO
                }

                // WEAPON MESH
                if (weapon->src) {
                    for (int j = 0; j < 2; j++) {
                        // TODO

                        if (!weapon->akimbo) break;
                    }
                }
            }
        }
    }
}

void player_update_meshes(Player *player, const int is_preview) {
    if (!player->mesh) return;

    const PlayerMesh *player_mesh = player->mesh;

    // TODO: move to client settings?
    const int animate_aim = 1;
    const float weapon_bobbing_mlt = 1.0f;
    const float weapon_lean_mlt = 1.0f;
    const float weapon_rotate_mlt = 1.0f;
    const vec3 weapon_offset_mlt = {1.0f, 1.0f, 1.0f};

    const int third_person = player->game->config.third_person || player->game->map->config.cam_offset.x || player->game->map->config.cam_offset.y || player->game->map->config.cam_offset.z;
    const float aim_val_mlt = animate_aim && player->is_you ? player->aim_val : 0.0f;
    const float aim_mlt = player->weapon->animate_while_aiming ? 0.0f : aim_val_mlt;
    const float anim_mlt = (1.0f - (1.0f - game_constants.aim_anim_mlt) * aim_val_mlt) * game_constants.anim_mlt * weapon_bobbing_mlt;
    const float anim_mlt_crouch = 1.0f - player->crouch_val * 0.8f;
    const float anim_mlt_lean = player->render_you ? 1.0f - aim_val_mlt * game_constants.anim_mlt : 0.0f;
    const float recoil_y_mlt = 1.0f - (player->weapon->recoil_y_mlt ? player->weapon->recoil_y_mlt : 1.0f) * game_constants.anim_mlt;
    const float recoil_z_mlt = 1.0f - (player->weapon->recoil_z_mlt ? player->weapon->recoil_z_mlt : 0.5f) * aim_mlt;
    const float anim_rotate_mlt = (1.0f - (player->weapon->z_rotation ? player->weapon->z_rotation : 0.3f) * aim_mlt) * (player->weapon->z_rotation_mlt ? player->weapon->z_rotation_mlt : 1.0f) * weapon_bobbing_mlt;
    const float anim_y_mlt = 1.0f - (player->weapon->jump_y_mlt ? player->weapon->jump_y_mlt : 1.0f) * aim_mlt;
    const float lean_aim_mlt = 1.0f * aim_mlt * 0.45f;
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

    const float jump_bob_y = player->jump_bob_y * (player->weapon->jump_y_mlt ? player->weapon->jump_y_mlt : 1.0f) * anim_mlt_lean * bob_crouch_mlt * weapon_bobbing_mlt;
    const float recoil_aim_mlt = 1.0f - aim_mlt * 0.89f;
    const float recoil_mlt = 1.0f - (player->weapon->aim_recoil_mlt ? player->weapon->aim_recoil_mlt : 1.0f) * aim_mlt;
    const float step_mlt = is_preview ? 0.05f : game_constants.step_anim;
    float step_anim = sinf(player->step_val) * step_mlt;
    float step_half = cosf(2.0f * player->step_val) * 0.5f * step_mlt;
    const float step_anim_rotate = -sinf(player->step_chase) * step_mlt;
    const float step_half_rotate = -cosf(2.0f * player->step_chase) * 0.5f * step_mlt;
    const float aim_val = third_person ? 0.0f : aim_val_mlt;
    const float aim_lean = (aim_val <= 0.5f ? aim_val : 0.5f - (aim_val - 0.5f)) * 0.5f;
    const float swap_anim = player->swap_timer / player->weapon->swap_time;
    const float left_hand_mlt = player->left_handed ? -1.0f : 1.0f;

    const vec3 weapon_offset = {
        player->weapon->offset.x * (player->render_you ? weapon_offset_mlt.x : 1.0f) * left_hand_mlt,
        player->weapon->offset.y * (player->render_you ? weapon_offset_mlt.y : 1.0f),
        player->weapon->offset.z * (player->render_you ? weapon_offset_mlt.z : 1.0f),
    };

    float reload_anim = player->reload_timer > 0.0f ? 1.0f - player->reload_timer / (player->weapon->reload_time * player->game->config.reload_speed) : 0.0f;
    if (reload_anim > 0.5f) reload_anim = 1.0f - reload_anim;

    reload_anim *= player->render_you ? 1.0f : 0.3f;

    const float idle_anim = (1.0f - aim_val_mlt * 0.88f) * 1.75f * weapon_bobbing_mlt;
    const float step_y = player->render_you && !third_person ? fabsf(step_anim_rotate * 0.5f) * anim_mlt_lean : fabsf(step_anim * 3.5f);
    const float step_yaw = player->render_you ? third_person ? -step_anim * 0.5f : 0.0f : -step_anim * 2.0f;

    if (player->is_you || player->uid == -1) {
        player_mesh->anchor->position = player->position;
        player_mesh->anchor->position.y += step_y;
    }

    if (player->game->map->config.model != MODEL_TYPE_SPRITE) { // TODO: check that is not prop
        player_mesh->anchor->rotation.y = player->direction.y + step_yaw;
    }

    step_half -= step_half * player->crouch_val * game_constants.crouch_anim_mlt;
    step_anim -= step_anim * player->crouch_val * game_constants.crouch_anim_mlt;

    if (!player->render_you) {
        for (int i = 0; i < 4; i++) {
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
    player_mesh->upper_body_anchor->rotation.z = 0.35f * weapon_rotate_mlt;

    player_mesh->upper_body_anchor->position.x = player_mesh->upper_body_anchor->position.z = 0.0f;
    player_mesh->upper_body_anchor->position.y = player->recoil_anim_y * (player->weapon->recoil_y_mlt ? player->weapon->recoil_y_mlt : 0.3f) * recoil_y_mlt + player->height - game_constants.camera_height - game_constants.leg_height;

    MeshTransform *arm_anchor = &player_mesh->arms[player->loadout_index]->anchor;

    arm_anchor->rotation.x = -cosf(player->idle_anim) * bob_crouch_mlt * 0.01f * idle_anim +
        player->weapon->rotation_offset * anim_mlt_lean + player->weapon->rotation_offset_aim * (1.0f - anim_mlt_lean) -
        0.0f /* TODO: player->swap_tween_r */ * 0.35f * anim_mlt_lean -
        player->land_bob_yr * 0.4f +
        0.0f /* TODO: player->recoil_tween_y */ * recoil_mlt +
        player->lean_anim.y * lean_aim_mlt * (player->weapon->lean_mlt ? player->weapon->lean_mlt : 1.0f) * weapon_lean_mlt +
        step_half_rotate * -0.9f * anim_mlt;

    arm_anchor->rotation.y = 0.0f /* TODO: player->inspect.x */ * left_hand_mlt +
        player->jump_rotate_mlt * 0.2f +
        player->lean_anim.x * lean_aim_mlt * (player->weapon->lean_mlt ? player->weapon->lean_mlt : 1.0f) * weapon_lean_mlt +
        (-step_anim_rotate * 0.16f * anim_mlt_lean * anim_mlt_crouch + player->lean_anim.z * 0.2f) * anim_mlt;

    arm_anchor->rotation.z = jump_rotate + aim_lean + 0.0f /* TODO: player->swap_tween_r */ * recoil_mlt +
        player->lean_anim.z * 0.7f * anim_rotate_mlt +
        -0.0f /* TODO: player->inspect.x */ * left_hand_mlt * player->weapon->inspect_rotation +
        player->weapon->crouch_lean * left_hand_mlt * player->crouch_val * anim_mlt_lean * anim_mlt;

    arm_anchor->position.x = player->recoil.x * recoil_aim_mlt +
        0.0f /* TODO: player->swap_tween_r */ * 0.2f -
        player->jump_rotate_mlt * anim_mlt_lean * 1.3f -
        0.0f /* TODO: player->inspect.x */ * left_hand_mlt * player->weapon->inspect_mlt +
        (player->lean_anim.z * 0.35f - player->weapon->crouch_rotation * left_hand_mlt * player->crouch_val * anim_mlt_lean + step_anim * 0.5f * anim_mlt_crouch * anim_mlt_lean) * aim_val_mlt * anim_mlt +
        weapon_offset.x - (weapon_offset.x - player->weapon->origin.x * left_hand_mlt) * aim_val;

    arm_anchor->position.y = player->recoil.z * recoil_aim_mlt +
        sinf(player->idle_anim) * 0.02f * idle_anim +
        0.0f /* TODO: player->recoil_tween_ym */ * recoil_mlt +
        jump_bob_y + land_bob_y * 0.5f - bob_anim_y * 1.5f -
        (player->weapon->bomb ? player->interact_progress * 3.0f : 0.0f) +
        (step_half * 0.85f - player->weapon->crouch_drop * player->crouch_val * anim_mlt_lean) * anim_mlt +
        weapon_offset.y - (weapon_offset.y - player->weapon->origin.y + 0.0f /* TODO: weapon skin y origin */ + 0.0f /* TODO: aim offset y */) * aim_val;

    arm_anchor->position.z = weapon_offset.z - (weapon_offset.z - player->weapon->origin.z) * aim_val +
        0.0f /* TODO: player->recoil_tween_z */ + player->bob_anim.z * anim_mlt + player->recoil_anim * player->weapon->recoil_z * recoil_z_mlt;
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

    free(player_mesh->upper_body_anchor);
    free(player_mesh->body_anchor);
    free(player_mesh->anchor);
    free(player_mesh);

    player->mesh = NULL;
}