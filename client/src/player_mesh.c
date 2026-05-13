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

    arm->anchor.position.x = x;
    arm->anchor.position.y = y;
    arm->anchor.scale.x = arm->anchor.scale.y = arm->anchor.scale.z = 1.0f;

    if (third_person) {
        BasicMaterial *material = basic_material_init();
        material->color = hex_to_vec(shirt_color);

        arm->upper = mesh_init(create_cube_geo(), (Material *) material);
        arm->joint = mesh_init(create_cube_geo(), (Material *) material);

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

    // TODO: anchor position?

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

void generate_crouched_leg() {

}

void player_generate_meshes(Player *player, const int render_you) {
    int colors[6];
    memcpy(colors, player->game->classes[player->class_index].colors, sizeof(colors));

    // TODO: dye colors
    // TODO: mode specific skins

    const int third_person = !render_you || player->game->config.third_person || player->game->map->config.cam_offset.x || player->game->map->config.cam_offset.y || player->game->map->config.cam_offset.z;

    PlayerMesh *player_mesh = calloc(1, sizeof(PlayerMesh));
    if (!player_mesh) return;

    player_mesh->anchor = calloc(1, sizeof(MeshTransform));
    player_mesh->body_anchor = calloc(1, sizeof(MeshTransform));

    if (!player_mesh->anchor || !player_mesh->body_anchor) {
        free(player_mesh->anchor);
        free(player_mesh->body_anchor);
        return;
    }

    player_mesh->body_anchor->position.y = game_constants.leg_height;
    player_mesh->body_anchor->parent = player_mesh->anchor;

    player_mesh->anchor->scale = player_mesh->body_anchor->scale = (vec3) {1.0f, 1.0f, 1.0f};
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
        }

        player_mesh->arms = calloc(player->loadout_size, sizeof(PlayerArms *));

        if (player_mesh->arms) {
            for (int i = 0; i < player->loadout_size; i++) {
                const Weapon *weapon = player->game->weapons[player->loadout[i]];

                // ARM GENERATION
                PlayerArms *arms = generate_arms(weapon, colors[1], colors[5], colors[0], third_person, 0);

                if (arms) {
                    arms->anchor.position.y = player->height - game_constants.camera_height - game_constants.leg_height;
                    arms->anchor.parent = player_mesh->body_anchor;
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