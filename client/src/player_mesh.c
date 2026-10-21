#include <client.h>
#include <cosmetics.h>
#include <gltf.h>
#include <nettspend.h> // the Drank's cup / pour-up and the Money Spread's cash stack
#include <render_settings.h>
#include <ui_logic.h> // ui_setting_weapon_float: vmSetts Per Weapon
#include <skin_fx.h>
#include <third_person.h> // the local third-person model
#include <prop_hunt.h> // Prop Hunt: the prop model
#include <social_v7.h> // game_player_is_prop
#include <pets.h>
#include <ruleset.h> // game_constants_of: the flavor's constants (Krunker Strike crouch lean)
#include <vfx.h>
#include "viewer/viewer_store.h" // game.store.skins: weapon / melee skins
#include <math.h>
#include <stdio.h>

// geos.js getColor(colr): THREE.Color(colr) with its HSL saturation raised 1.5 times (clamped). Every
// genColorCube / genCube segment colours its vertices with it (v3.1.1 did the same), so the player's
// body parts are more saturated than their config colours.
vec4 krunker_get_color(const int hex) {
    const float r = (float) ((hex >> 16) & 255) / 255.0f, g = (float) ((hex >> 8) & 255) / 255.0f, b = (float) (hex & 255) / 255.0f;
    const float max = fmaxf(r, fmaxf(g, b)), min = fminf(r, fminf(g, b));
    const float l = (min + max) * 0.5f;
    if (max == min) return (vec4) {r, g, b, 1.0f};
    const float delta = max - min;
    float s = l <= 0.5f ? delta / (max + min) : delta / (2.0f - max - min);
    float h;
    if (max == r) h = (g - b) / delta + (g < b ? 6.0f : 0.0f);
    else if (max == g) h = (b - r) / delta + 2.0f;
    else h = (r - g) / delta + 4.0f;
    h /= 6.0f;
    s = fminf(1.0f, s * 1.5f);
    // setHSL
    const float q = l <= 0.5f ? l * (1.0f + s) : l + s - l * s;
    const float p = 2.0f * l - q;
    float out[3];
    for (int i = 0; i < 3; i++) {
        float t = h + (1.0f - (float) i) / 3.0f;
        if (t < 0.0f) t += 1.0f;
        if (t > 1.0f) t -= 1.0f;
        out[i] = t < 1.0f / 6.0f ? p + (q - p) * 6.0f * t : t < 0.5f ? q : t < 2.0f / 3.0f ? p + (q - p) * 6.0f * (2.0f / 3.0f - t) : p;
    }
    return (vec4) {out[0], out[1], out[2], 1.0f};
}

ColorCube *generate_color_cube(const float width, const float height, const float length, const ColorCubeSegment *segments, const size_t segment_count) {
    return generate_color_cube_at(width, height, length, segments, segment_count, 0.5f);
}

ColorCube *generate_color_cube_at(const float width, const float height, const float length, const ColorCubeSegment *segments, const size_t segment_count, const float top) {
    ColorCube *color_cube = calloc(1, sizeof(ColorCube));
    if (!color_cube) return NULL;

    color_cube->meshes = calloc(segment_count ? segment_count : 1, sizeof(Mesh*));
    color_cube->mesh_count = 0;

    if (!color_cube->meshes) {
        free(color_cube);
        return NULL;
    }

    color_cube->transform.scale.x = width;
    color_cube->transform.scale.y = height;
    color_cube->transform.scale.z = length;

    float offset = top;

    for (size_t i = 0; i < segment_count; i++) {
        const ColorCubeSegment *segment = &segments[i];
        if (segment->height <= 0.0f) continue; // a skipped segment (noHair / noBody / legLen)

        Geometry *geo = create_cube_geo();
        BasicMaterial *material = basic_material_init();

        Mesh *mesh = mesh_init(geo, (Material *) material);

        material->color = krunker_get_color(segments[i].color);

        mesh->transform.scale.x = segment->scale;
        mesh->transform.scale.z = segment->scale;
        mesh->transform.scale.y = segment->height;

        mesh->transform.position.y = offset - segment->height;
        mesh->transform.parent = &color_cube->transform;

        offset -= segment->height;
        color_cube->meshes[color_cube->mesh_count++] = mesh;
    }

    return color_cube;
}

void color_cube_fini(ColorCube *cube) {
    if (!cube) return;
    for (size_t i = 0; i < cube->mesh_count; i++) {
        skin_fx_forget(cube->meshes[i]);
        mesh_fini(cube->meshes[i]);
    }
    free(cube->meshes);
    free(cube);
}

PlayerArmMesh *generate_arm(const float x, const float y, const Weapon *weapon, const int is_left, const int shirt_color, const int sleeve_color, const int skin_color, const int third_person, const int left_handed, const int no_left_sleeve) {
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

        upper_material->color = joint_material->color = krunker_get_color(shirt_color);

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
        material->color = krunker_get_color(shirt_color);

        arm->extender = mesh_init(create_cube_geo(), (Material *) material);
        arm->extender->transform.parent = &arm->anchor;

        arm->extender->transform.scale.x = arm->extender->transform.scale.z = arm_scale;
        arm->extender->transform.scale.y = 20.0f;

        arm->extender->transform.rotation.x = (float) -M_PI * 0.5f - arm_angles[0];
        arm->extender->transform.position.z = -game_constants.upper_arm_length;
    }

    // genArm: a noLeftSleeve wrist (VS2_NO_LEFT_SLEEVE, the wrist item's flag) takes the left sleeve away: that
    // band is the shirt colour at scale 1 instead of the sleeve colour at 1.1. v10 also sets noAmb on the
    // forearm (its vertex ambient shading, which this renderer's colour cubes do not draw)
    const int bare_left_sleeve = is_left && no_left_sleeve;
    const ColorCubeSegment lower_segments[] = {
        {shirt_color, 0.65f, 1.0f},
        {bare_left_sleeve ? shirt_color : sleeve_color, 0.15f, bare_left_sleeve ? 1.0f : 1.1f},
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

PlayerArms *generate_arms(const Weapon *weapon, const int shirt_color, const int sleeve_color, const int skin_color, const int third_person, const int left_handed, const int no_left_sleeve) {
    PlayerArms *arms = calloc(1, sizeof(PlayerArms));
    if (!arms) return NULL;

    const float offset = (-game_constants.chest_width + game_constants.arm_scale * 0.5f - game_constants.arm_inset) * (third_person ? 1.0f : weapon->hold_width ? weapon->hold_width : 0.4f);

    arms->left = generate_arm(offset, game_constants.arm_offset, weapon, !left_handed, shirt_color, sleeve_color, skin_color, third_person, left_handed, no_left_sleeve);
    arms->right = generate_arm(-offset, game_constants.arm_offset, weapon,  left_handed, shirt_color, sleeve_color, skin_color, third_person, left_handed, no_left_sleeve);

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

// playerMesh.js genHead: [hairCol 0.2 (skipped with noHair)], [skinCol 0.8]; nothing with noHead.
ColorCube *generate_head(const int skin_color, const int hair_color, const int no_hair) {
    const ColorCubeSegment segments[] = {
        {hair_color, no_hair ? 0.0f : 0.2f, 1.0f},
        {skin_color, 0.8f, 1.0f},
    };

    ColorCube *head = generate_color_cube_at(
        game_constants.head_scale,
        game_constants.head_scale,
        game_constants.head_scale,
        segments, sizeof(segments) / sizeof(segments[0]), no_hair ? 0.3f : 0.5f
    );

    if (head) head->transform.position.y = game_constants.body_height + game_constants.head_scale * 0.5f;
    return head;
}

// playerMesh.js genBody: [shirtCol 0.8 (skipped with a body item's noBody)], [waistCol 0.2 x1.05]
ColorCube *generate_body(const int shirt_color, const int waist_color, const int no_body) {
    const ColorCubeSegment segments[] = {
        {shirt_color, no_body ? 0.0f : 0.8f, 1.0f},
        {waist_color, 0.2f, 1.05f},
    };

    ColorCube *body = generate_color_cube_at(
        game_constants.chest_width,
        game_constants.body_height,
        game_constants.chest_scale,
        segments, sizeof(segments) / sizeof(segments[0]), no_body ? -0.3f : 0.5f
    );

    if (body) body->transform.position.y = game_constants.body_height * 0.5f;
    return body;
}

// playerMesh.js genLeg (standing): genCube(.., [[pants, legLen || 0.75], [shoes, 0.25 (skipped
// with legLen: the shoe item is the foot)]], -0.5) at (+-legScale / 2, legHeight): the cube hangs
// from the hip, which is the pivot of the walk swing.
ColorCube *generate_leg(const int is_left, const int pants_color, const int shoe_color, const float leg_len) {
    const ColorCubeSegment segments[] = {
        {pants_color, leg_len > 0.0f ? leg_len : 0.75f, 1.0f},
        {shoe_color, leg_len > 0.0f ? 0.0f : 0.25f, 1.0f}
    };

    ColorCube *leg = generate_color_cube_at(game_constants.leg_scale, game_constants.leg_height, game_constants.leg_scale, segments, sizeof(segments) / sizeof(segments[0]), 0.0f);

    if (leg) {
        leg->transform.position.x = game_constants.leg_scale * 0.5f * (is_left ? -1.0f : 1.0f);
        leg->transform.position.y = game_constants.leg_height;
    }

    return leg;
}

PlayerCrouchedLeg *generate_crouched_leg(const int is_left, const int pants_color, const int shoe_color) {
    PlayerCrouchedLeg *leg = calloc(1, sizeof(PlayerCrouchedLeg));
    if (!leg) return NULL;

    const float leg_angles[] = {2.0f, 0.5f}; // upper, lower

    BasicMaterial *upper_material = basic_material_init();
    BasicMaterial *joint_material = basic_material_init();

    upper_material->color = joint_material->color = krunker_get_color(pants_color);

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

    // weaponGeos[i] back to its hold every frame (position xP/yP/zP, rotation yR/zR; the skin's sxOff & co)
    if (arms->weapon_right) {
        arms->weapon_right->transform.position = arms->melee_position;
        arms->weapon_right->transform.rotation = arms->melee_rotation;
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
        const float wind_up = player->is_you && player->swap_timer > 0.0f ? 0.0f : player->aim_val; // 1 - jKFi4 (swapTime: isYou only)
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

// ---------------------------------------------------------------- Prop Hunt

int prop_hunt_player(const Player *player) {
    const Game *game = player ? player->game : NULL;
    return game && game->mode && game->mode->config.prop && game_player_is_prop(game, player) && prop_is_valid(player->prop_id);
}

// loadMesh({isMdl, src: name.toLowerCase() + "_0", emissive: ACIDBARREL}, 0, scale + CPY, 0, 0, scale, objInstances)
static Mesh *player_prop_mesh(const int prop_id) {
    char src[64], rel[96];
    prop_model_src(prop_id, src, sizeof(src));
    snprintf(rel, sizeof(rel), "models/%s.obj", src);
    char *path = concat(assets_path(), rel);
    if (path) ensure_asset_file(path); // fetched on demand (web / a fresh asset tree)
    Geometry *geometry = path ? load_obj_model(path, 0) : NULL;
    free(path);
    if (!geometry) return NULL;
    BasicMaterial *material = basic_material_init();
    if (!material) return NULL;
    snprintf(rel, sizeof(rel), "textures/%s.png", src);
    path = concat(assets_path(), rel);
    if (path) ensure_asset_file(path);
    material->texture = path ? load_texture(path) : 0;
    free(path);
    if (!strcmp(src, "acidbarrel_0")) material->emissive = (vec4) {1.0f, 1.0f, 1.0f, 1.0f}; // emissive #ffffff
    Mesh *mesh = mesh_init(geometry, (Material *) material);
    if (!mesh) return NULL;
    const float scale = prop_model_scale(prop_id);
    mesh->transform.position = (vec3) {0.0f, prop_model_y(prop_id), 0.0f};
    mesh->transform.scale = (vec3) {scale, scale, scale};
    return mesh;
}

// ---------------------------------------------------------------- store skins on weapons

static Geometry *skin_model(const char *key) {
    char rel[192];
    snprintf(rel, sizeof(rel), "models/%s.obj", key);
    char *path = concat(assets_path(), rel);
    Geometry *geometry = path ? load_obj_model(path, 0) : NULL;
    free(path);
    return geometry;
}

static unsigned int skin_texture(const char *key) {
    char rel[256];
    snprintf(rel, sizeof(rel), "textures/%s.png", key);
    char *path = concat(assets_path(), rel);
    const unsigned int texture = path ? load_texture(path) : 0;
    free(path);
    return texture;
}

static int skin_texture_exists(const char *key) {
    char rel[256];
    snprintf(rel, sizeof(rel), "textures/%s.png", key);
    char *path = concat(assets_path(), rel);
    if (!path) return 0;
    ensure_asset_file(path);
    FILE *file = fopen(path, "rb");
    if (file) fclose(file);
    free(path);
    return file != NULL;
}

// generateMeshes `ae`: game.store.skins[weapon.melee ? meleeIndex : player.skins[slot]], {} (NULL) when
// there is none or a weapon skin is for another weapon (ae.weapon - 1 != loadout[slot]).
static const ViewerSkin *player_slot_skin(const Player *player, const int slot, const Weapon *weapon) {
    if (weapon->melee) {
        const ViewerSkin *skin = viewer_skin(player->skins.melee);
        return skin && skin->type == 3 ? skin : NULL;
    }
    const ViewerSkin *skin = viewer_skin(slot == 0 ? player->skins.primary : slot == 1 ? player->skins.secondary : -1);
    return skin && skin->type == 0 && skin->weapon - 1 == player->loadout[slot] ? skin : NULL;
}

// KrunkNative Piss: the centre of the trigger hand (the arm generate_arms gives the right hold:
// arms->right, arms->left when left-handed), i.e. of its lower arm's last (skin) segment, in the
// arms anchor's frame. Static there (the per-arm pivots only move for melee and the Drank's
// pour-up), so the first-person hose is placed from it once.
static vec3 player_arm_hand(const PlayerArms *arms, const PlayerArmMesh *arm) {
    if (!arm || !arm->lower || !arm->lower->mesh_count) return (vec3) {0.0f, 0.0f, 0.0f};
    const Mesh *skin = arm->lower->meshes[arm->lower->mesh_count - 1];
    return transform_point_to(&skin->transform, (vec3) {0.0f, 0.5f, 0.0f}, &arms->anchor); // the unit cube spans y 0..1
}

static vec3 player_trigger_hand(const PlayerArms *arms, const int left_handed) {
    return player_arm_hand(arms, left_handed ? arms->left : arms->right);
}

// The other hand (the Drank's bottle comes from it: the pour-up uses both)
static vec3 player_off_hand(const PlayerArms *arms, const int left_handed) {
    return player_arm_hand(arms, left_handed ? arms->right : arms->left);
}

// Rodrigues: m = I + sn K + (1 - cs) K^2, the rotation by the angle with sine sn and cosine cs about the unit axis k
static void player_mat3_axis_rotation(float m[3][3], const float kx, const float ky, const float kz, const float sn, const float cs) {
    const float t = 1.0f - cs;
    m[0][0] = cs + kx * kx * t, m[0][1] = kx * ky * t - kz * sn, m[0][2] = kx * kz * t + ky * sn;
    m[1][0] = ky * kx * t + kz * sn, m[1][1] = cs + ky * ky * t, m[1][2] = ky * kz * t - kx * sn;
    m[2][0] = kz * kx * t - ky * sn, m[2][1] = kz * ky * t + kx * sn, m[2][2] = cs + kz * kz * t;
}

// The elbow at rest (the top of the forearm, where the upper arm ends) in the arms anchor frame, the pivot at identity
static vec3 player_arm_elbow(const PlayerArms *arms, const PlayerArmMesh *arm) {
    if (!arms || !arm || !arm->lower || !arm->lower->mesh_count) return (vec3) {0.0f, 0.0f, 0.0f};
    return transform_point_to(&arm->lower->meshes[0]->transform, (vec3) {0.0f, 1.0f, 0.0f}, &arms->anchor); // the shirt segment's top
}

// Turns `arm` (its pivot, identity at rest outside melee) about its shoulder (arm->anchor's origin)
// so the hand centre at `rest` (the arms anchor frame, the pivot at identity) lands on `target`: the
// rotation takes the shoulder-to-hand direction onto the shoulder-to-target one, and the arm slides
// along it for the difference in reach, so the shoulder stays put (on the body in third person; in
// first person the long sleeve keeps passing below the camera). Nettspend's pour-up: the hands
// follow the cup and the bottle. `pole` (may be NULL; the arms frame, with the rest `elbow`): the arm
// then rolls about the shoulder-to-target line, the hand staying on it, until its elbow lies on the
// pole's side of that line (NETTSPEND_OFF_ELBOW_POLE_*: the off arm's elbow bends down and back).
static void player_arm_reach(PlayerArmMesh *arm, const vec3 rest, const vec3 target, const vec3 elbow, const vec3 *pole) {
    if (!arm) return;
    const vec3 sh = arm->anchor.position;
    const float ax = rest.x - sh.x, ay = rest.y - sh.y, az = rest.z - sh.z;
    const float bx = target.x - sh.x, by = target.y - sh.y, bz = target.z - sh.z;
    const float la = sqrtf(ax * ax + ay * ay + az * az), lb = sqrtf(bx * bx + by * by + bz * bz);
    float m[3][3] = {{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}};
    if (la > 1e-4f && lb > 1e-4f) {
        float kx = ay * bz - az * by, ky = az * bx - ax * bz, kz = ax * by - ay * bx;
        const float sn = sqrtf(kx * kx + ky * ky + kz * kz) / (la * lb), cs = (ax * bx + ay * by + az * bz) / (la * lb);
        if (sn > 1e-5f) {
            const float kl = sqrtf(kx * kx + ky * ky + kz * kz);
            kx /= kl, ky /= kl, kz /= kl;
            player_mat3_axis_rotation(m, kx, ky, kz, sn, cs);
        }
    }
    if (pole && la > 1e-4f && lb > 1e-4f) {
        // the roll about the chord k (m takes the rest direction onto k, so the axis keeps the hand: R a = la k)
        const float kx = bx / lb, ky = by / lb, kz = bz / lb;
        const float ex = elbow.x - sh.x, ey = elbow.y - sh.y, ez = elbow.z - sh.z;
        const float fx = m[0][0] * ex + m[0][1] * ey + m[0][2] * ez, fy = m[1][0] * ex + m[1][1] * ey + m[1][2] * ez,
                    fz = m[2][0] * ex + m[2][1] * ey + m[2][2] * ez;                     // the elbow after m
        const float fk = fx * kx + fy * ky + fz * kz;
        const float px = fx - fk * kx, py = fy - fk * ky, pz = fz - fk * kz;             // its bend across the chord
        const float qk = pole->x * kx + pole->y * ky + pole->z * kz;
        const float qx = pole->x - qk * kx, qy = pole->y - qk * ky, qz = pole->z - qk * kz; // the pole's side of the chord
        const float pl = sqrtf(px * px + py * py + pz * pz), ql = sqrtf(qx * qx + qy * qy + qz * qz);
        if (pl > 1e-5f && ql > 1e-5f) {
            const float sn = (kx * (py * qz - pz * qy) + ky * (pz * qx - px * qz) + kz * (px * qy - py * qx)) / (pl * ql); // k . (p x q)
            const float cs = (px * qx + py * qy + pz * qz) / (pl * ql);                                                   // p . q
            float roll[3][3], turned[3][3];
            player_mat3_axis_rotation(roll, kx, ky, kz, sn, cs);
            for (int i = 0; i < 3; i++)
                for (int j = 0; j < 3; j++) turned[i][j] = roll[i][0] * m[0][j] + roll[i][1] * m[1][j] + roll[i][2] * m[2][j];
            for (int i = 0; i < 3; i++)
                for (int j = 0; j < 3; j++) m[i][j] = turned[i][j];
        }
    }
    // the pivot's rotation (intrinsic: Rx * Ry * Rz, three.js "XYZ"), from the matrix
    const float y = asinf(fminf(fmaxf(m[0][2], -1.0f), 1.0f));
    const float x = fabsf(m[0][2]) < 0.9999999f ? atan2f(-m[1][2], m[2][2]) : atan2f(m[2][1], m[1][1]);
    const float z = fabsf(m[0][2]) < 0.9999999f ? atan2f(-m[0][1], m[0][0]) : 0.0f;
    arm->pivot.rotation_order = ROTATION_ORDER_INTRINSIC;
    arm->pivot.rotation = (vec3) {x, y, z};
    // p' = R p + P: the shoulder stays (P = sh - R sh) and the hand reaches target (+ the slide)
    const float rax = m[0][0] * ax + m[0][1] * ay + m[0][2] * az, ray = m[1][0] * ax + m[1][1] * ay + m[1][2] * az,
                raz = m[2][0] * ax + m[2][1] * ay + m[2][2] * az;
    const float rsx = m[0][0] * sh.x + m[0][1] * sh.y + m[0][2] * sh.z, rsy = m[1][0] * sh.x + m[1][1] * sh.y + m[1][2] * sh.z,
                rsz = m[2][0] * sh.x + m[2][1] * sh.y + m[2][2] * sh.z;
    arm->pivot.position = (vec3) {sh.x - rsx + bx - rax, sh.y - rsy + by - ray, sh.z - rsz + bz - raz};
}

// Nettspend Drank: where the off hand is while it does not pour (the arms anchor frame): the arm turned
// down about its shoulder (arm->anchor's origin) to hang at the side, its reach kept, a little out to
// its own side and forward (NETTSPEND_OFF_HAND_AWAY_*). `rest` is the hand's centre at rest. First person only:
// third person hangs at its rest (NETTSPEND_OFF_HAND_FWD).
static vec3 player_off_hand_away(const PlayerArmMesh *arm, const vec3 rest) {
    if (!arm) return rest;
    const vec3 sh = arm->anchor.position;
    const float dx = rest.x - sh.x, dy = rest.y - sh.y, dz = rest.z - sh.z, reach = sqrtf(dx * dx + dy * dy + dz * dz);
    const float side = sh.x < 0.0f ? -1.0f : 1.0f;
    float ax = side * NETTSPEND_OFF_HAND_AWAY_OUT, ay = -1.0f, az = -NETTSPEND_OFF_HAND_AWAY_FWD;
    const float l = sqrtf(ax * ax + ay * ay + az * az);
    ax /= l, ay /= l, az /= l;
    return (vec3) {sh.x + ax * reach, sh.y + ay * reach, sh.z + az * reach};
}

static void player_arm_mesh_set_visible(const PlayerArmMesh *arm, const int visible) {
    if (!arm) return;
    if (arm->extender) arm->extender->visible = visible;
    if (arm->upper) arm->upper->visible = visible;
    if (arm->joint) arm->joint->visible = visible;
    for (size_t k = 0; arm->lower && k < arm->lower->mesh_count; k++) arm->lower->meshes[k]->visible = visible;
}

int player_hose_points(const Player *player, vec3 *tip, vec3 *back, vec3 *hand) {
    const PlayerMesh *mesh = player ? player->mesh : NULL;
    if (!mesh || !mesh->arms || player->loadout_index < 0 || player->loadout_index >= player->loadout_size) return 0;
    const PlayerArms *arms = mesh->arms[player->loadout_index];
    if (!arms || !arms->hose) return 0;
    const float half = arms->hose_length * 0.5f;
    // hoseTip: genObj3D(0, 0, -hoseLength / 2) inside the hose mesh
    if (tip) *tip = transform_point_to(&arms->hose->transform, (vec3) {0.0f, 0.0f, -half}, NULL);
    if (back) *back = transform_point_to(&arms->hose->transform, (vec3) {0.0f, 0.0f, half}, NULL);
    if (hand) *hand = transform_point_to(&arms->anchor, player_trigger_hand(arms, player->left_handed), NULL);
    return 1;
}

// Nettspend (nettspend.h): the pour-up's points while the local player reloads the Drank (first or
// third person): the bottle's mouth and the cup's rim centre (world); 0 otherwise (vfx.c droplets).
int player_pour_points(const Player *player, vec3 *mouth, vec3 *rim) {
    const PlayerMesh *mesh = player ? player->mesh : NULL;
    if (!mesh || !mesh->arms || player->loadout_index < 0 || player->loadout_index >= player->loadout_size) return 0;
    if (nettspend_weapon_kind(player->loadout[player->loadout_index]) != NETTSPEND_WEAPON_DRANK) return 0;
    const PlayerArms *arms = mesh->arms[player->loadout_index];
    if (!arms || !arms->hose || !arms->weapon_left || !arms->weapon_left->visible) return 0;
    if (mouth) *mouth = transform_point_to(&arms->weapon_left->transform, (vec3) {0.0f, 0.0f, -NETTSPEND_BOTTLE_LENGTH * 0.5f}, NULL);
    if (rim) *rim = transform_point_to(&arms->hose->transform, (vec3) {0.0f, 0.0f, -arms->hose_length * 0.5f}, NULL);
    return 1;
}

int player_hands_points(const Player *player, vec3 *trigger, vec3 *off, vec3 *bottle, int *bottle_visible, int *off_visible) {
    const PlayerMesh *mesh = player ? player->mesh : NULL;
    if (!mesh || !mesh->arms || player->loadout_index < 0 || player->loadout_index >= player->loadout_size) return 0;
    const PlayerArms *arms = mesh->arms[player->loadout_index];
    if (!arms) return 0;
    if (trigger) *trigger = transform_point_to(&arms->anchor, player_trigger_hand(arms, player->left_handed), NULL);
    if (off) *off = transform_point_to(&arms->anchor, player_off_hand(arms, player->left_handed), NULL);
    // the bottle's grip (where the off hand holds it: NETTSPEND_BOTTLE_GRIP of its half length from the centre toward the mouth)
    if (bottle) *bottle = arms->weapon_left ? transform_point_to(&arms->weapon_left->transform, (vec3) {0.0f, 0.0f, -NETTSPEND_BOTTLE_LENGTH * 0.5f * NETTSPEND_BOTTLE_GRIP}, NULL)
                                            : (vec3) {0.0f, 0.0f, 0.0f};
    if (bottle_visible) *bottle_visible = arms->weapon_left ? arms->weapon_left->visible : 0;
    if (off_visible) { // the off arm's skin (its hand) as drawn
        const PlayerArmMesh *arm = player->left_handed ? arms->right : arms->left;
        *off_visible = arm && arm->lower && arm->lower->mesh_count ? arm->lower->meshes[arm->lower->mesh_count - 1]->visible : 0;
    }
    return 1;
}

// The joints of the trigger arm (off = 0) or the off arm (off = 1) as drawn, in world space: the shoulder (the arm's
// anchor), the elbow (the top of the forearm, where the upper arm ends) and the wrist (the hand's centre). The
// nettspend_arms_e2e.py bend check reads them (the verb `arms` in input_script.c). 0 without arms.
int player_arm_joints(const Player *player, const int off, vec3 *shoulder, vec3 *elbow, vec3 *wrist) {
    const PlayerMesh *mesh = player ? player->mesh : NULL;
    if (!mesh || !mesh->arms || player->loadout_index < 0 || player->loadout_index >= player->loadout_size) return 0;
    const PlayerArms *arms = mesh->arms[player->loadout_index];
    if (!arms) return 0;
    const PlayerArmMesh *arm = off ? (player->left_handed ? arms->right : arms->left) : (player->left_handed ? arms->left : arms->right);
    if (!arm || !arm->lower || !arm->lower->mesh_count) return 0;
    if (shoulder) *shoulder = transform_point_to(&arm->anchor, (vec3) {0.0f, 0.0f, 0.0f}, NULL);
    if (elbow) *elbow = transform_point_to(&arm->lower->meshes[0]->transform, (vec3) {0.0f, 1.0f, 0.0f}, NULL); // the shirt segment's top
    if (wrist) *wrist = transform_point_to(&arms->anchor, player_arm_hand(arms, arm), NULL);
    return 1;
}

Mesh *player_hose_mesh(const WeaponStream *stream, const int color) {
    if (!stream || stream->hose_radius <= 0.0f || stream->hose_length <= 0.0f) return NULL;
    Geometry *geometry = create_hose_geo(stream->hose_radius, stream->hose_length, 18); // hoseSides ?? 18
    BasicMaterial *material = geometry ? basic_material_init() : NULL;
    if (!material) return NULL;
    material->color = krunker_get_color(color);
    Mesh *mesh = mesh_init(geometry, (Material *) material);
    if (!mesh) {
        material_fini((Material *) material);
        return NULL;
    }
    mesh->transform.scale = (vec3) {1.0f, 1.0f, 1.0f};
    return mesh;
}

// getMat for store items: client/src/skin_fx.c (skin_fx_apply / skin_fx_apply_gltf / skin_fx_apply_dye)

// mesh_fini / gltf_instance_fini, after dropping the skin animations registered on them
static void fx_mesh_fini(Mesh *mesh) {
    if (!mesh) return;
    skin_fx_forget(mesh);
    mesh_fini(mesh);
}

static void fx_gltf_fini(GltfInstance *model) {
    if (!model) return;
    skin_fx_forget_gltf(model);
    gltf_instance_fini(model);
}

// The weapon loadMesh src / texSrc: "weapons/<src>[_<mid>]", then UTILS.getReModel (a remodel), ae.tex,
// "weapons/skins/<src>_<id>", else the model's own texture.
static void weapon_skin_keys(const Weapon *weapon, const ViewerSkin *skin, char *model, const size_t model_n, char *texture, const size_t texture_n) {
    if (skin && skin->mid >= 0) snprintf(model, model_n, "weapons/%s_%d", weapon->src, skin->mid);
    else snprintf(model, model_n, "weapons/%s", weapon->src);

    if (!skin) snprintf(texture, texture_n, "%s", model);
    else if (skin->mid >= 0) snprintf(texture, texture_n, "weapons/weapon_%d_%d", skin->weapon, skin->mid_t > 0 ? skin->mid_t : skin->mid);
    else if (viewer_skin_tex_truthy(skin)) snprintf(texture, texture_n, "%s", skin->tex);
    else if (skin->id >= 0) snprintf(texture, texture_n, "weapons/skins/%s_%d", weapon->src, skin->id);
    else snprintf(texture, texture_n, "%s", model);
}

// v7 hasAnim weapon / melee skins: RENDER.loadAnimMesh(src "<model>.<fileExt || gltf>") with the
// same position / rotation / scale arguments as loadMesh (players.js:805, :1140), the model's own
// GLTF materials. The mesh the pose code drives (weapon_right / weapon_left) becomes a carrier with
// an empty geometry and the GLTF root hangs off it, so every pose and visibility write keeps working.
// TODO (v7 render.js f1117): env maps (`env`, the "_env" texture) on the GLTF materials.
static Mesh *gltf_skin_carrier(const ViewerSkin *skin, const char *model_key, GltfInstance **out) {
    *out = NULL;
    if (!skin || !(skin->flags & VS_HAS_ANIM)) return NULL;
    char rel[192];
    snprintf(rel, sizeof(rel), "models/%s.%s", model_key, skin->file_ext ? skin->file_ext : "gltf");
    char *path = concat(assets_path(), rel);
    GltfInstance *instance = path ? gltf_instance_create(gltf_model_load(path), skin->anim_s) : NULL;
    free(path);
    if (!instance) return NULL;
    BasicMaterial *material = basic_material_init();
    Mesh *carrier = material ? mesh_init(gltf_empty_geometry(), (Material *) material) : NULL;
    if (!carrier) {
        if (material) material_fini((Material *) material);
        gltf_instance_fini(instance);
        return NULL;
    }
    instance->root.parent = &carrier->transform;
    skin_fx_apply_gltf(instance, skin, model_key);
    *out = instance;
    return carrier;
}

// hasAnim skin whose GLTF model loads (cached by gltf_model_load)
static int gltf_skin_available(const ViewerSkin *skin, const char *model_key) {
    if (!skin || !(skin->flags & VS_HAS_ANIM)) return 0;
    char rel[192];
    snprintf(rel, sizeof(rel), "models/%s.%s", model_key, skin->file_ext ? skin->file_ext : "gltf");
    char *path = concat(assets_path(), rel);
    const int ok = path && gltf_model_load(path) != NULL;
    free(path);
    return ok;
}

// The GLTF remodels follow their carrier's visibility and advance their clips.
static void player_weapon_models_update(const PlayerMesh *player_mesh, const int loadout_size) {
    if (!player_mesh->arms) return;
    const double now = gltf_clock();
    for (int i = 0; i < loadout_size; i++) {
        const PlayerArms *arms = player_mesh->arms[i];
        if (!arms) continue;
        for (int k = 0; k < 2; k++) {
            GltfInstance *model = arms->weapon_models[k];
            const Mesh *carrier = k ? arms->weapon_left : arms->weapon_right;
            if (!model || !carrier) continue;
            gltf_instance_set_visible(model, carrier->visible);
            gltf_instance_update(model, now);
        }
        // charm and wrist follow the weapon / arm visibility
        const Mesh *shown = arms->weapon_right ? arms->weapon_right : arms->left && arms->left->lower && arms->left->lower->mesh_count ? arms->left->lower->meshes[0] : NULL;
        const int visible = shown ? shown->visible : 0;
        if (arms->charm_mount) arms->charm_mount->visible = visible;
        if (arms->charm) arms->charm->visible = visible;
        const int arm_visible = arms->left && arms->left->lower && arms->left->lower->mesh_count ? arms->left->lower->meshes[0]->visible : visible;
        if (arms->wrist) arms->wrist->visible = arm_visible;
        if (arms->wrist_model) {
            gltf_instance_set_visible(arms->wrist_model, arm_visible);
            gltf_instance_update(arms->wrist_model, now);
        }
    }
}

// The melee loadMesh: src "melee/melee_<id || 0>", texSrc pat ? tex : tex ? "melee/melee_<id>_<tex>" : the model's.
static void melee_skin_keys(const ViewerSkin *skin, char *model, const size_t model_n, char *texture, const size_t texture_n) {
    const int id = skin && skin->id > 0 ? skin->id : 0;
    snprintf(model, model_n, "melee/melee_%d", id);
    if (skin && (skin->flags & VS_PAT) && skin->tex) snprintf(texture, texture_n, "%s", skin->tex);
    else if (skin && viewer_skin_tex_truthy(skin)) snprintf(texture, texture_n, "melee/melee_%d_%s", id, skin->tex);
    else snprintf(texture, texture_n, "%s", model);
}

// ---------------------------------------------------------------- cosmetics (v7.1.4 players.js generateMeshes)

// The items the model wears (players.js generateMeshes): the routed slots (cosmetics_route), with
// game.config.noCosm, the Prop team's petIndex and the Juggernaut outfit applied.
typedef struct {
    const ViewerSkin *hat, *head, *face, *body, *back, *waist, *shoe, *wrist, *pet;
    const ViewerDye *dye;
    const ViewerSkin *dye_item;
    int skin_color, hair_color; // ENT skinColIndex colour / hairCol, -1 = the class's
} PlayerLook;

static void player_look(const Player *player, PlayerLook *look) {
    PlayerCosmetics c = player->cosmetics;
    PlayerCosmeticsV7 v7 = player->cosmetics_v7;
    cosmetics_route(&c, &v7);
    if (player->game && player->game->config.no_cosmetics) { // noCosm "Hide Cosmetics": every slot and skinColIndex
        const int hair = c.hair_color;
        c = PLAYER_COSMETICS_NONE;
        c.hair_color = hair;
        v7 = PLAYER_COSMETICS_V7_NONE;
    }
    if (player->is_juggernaut) { // players.js:774-779
        c.body = COSMETIC_JUGGERNAUT_BODY;
        c.waist = COSMETIC_JUGGERNAUT_WAIST;
        v7.head = COSMETIC_JUGGERNAUT_HEAD;
        v7.shoe = COSMETIC_JUGGERNAUT_SHOES;
    }
    memset(look, 0, sizeof(*look));
    look->hat = cosmetic_item(c.hat, ITEM_HAT);
    look->head = cosmetic_item(v7.head, ITEM_HEAD);
    look->face = cosmetic_item(c.face, ITEM_FACE);
    look->body = cosmetic_item(c.body, ITEM_BODY);
    look->back = cosmetic_item(v7.back, ITEM_BACK);
    look->waist = cosmetic_item(c.waist, ITEM_WAIST);
    look->shoe = cosmetic_item(v7.shoe, ITEM_SHOE);
    look->wrist = cosmetic_item(v7.wrist, ITEM_WRIST);
    look->pet = cosmetic_item(v7.pet, ITEM_PET);
    look->dye_item = cosmetic_item(c.dye, ITEM_DYE);
    look->dye = look->dye_item ? viewer_dye(c.dye) : NULL;
    look->skin_color = c.skin_color;
    look->hair_color = c.hair_color;
}

// UTILS.getRandom of a dye colour list: picked per player (uid) so every client and every
// regeneration agree (v7 rolls Math.random per model).
static int dye_pick(const ViewerDyeColor *part, const int uid, const int salt) {
    if (part->count <= 0) return -1;
    const unsigned h = (unsigned) uid * 2654435761u + (unsigned) salt * 40503u;
    return part->cols[(h >> 16) % (unsigned) part->count];
}

// players.js generateMeshes colours. colors[] is ClassConfig.colors order: skin, shirt, pants,
// shoes, hair, sleeve; *waist_color the waist band (config.colors[6] || pants, dye waistCol).
static void player_look_colors(const PlayerLook *look, const int uid, int colors[6], int *waist_color) {
    if (look->skin_color >= 0) colors[0] = look->skin_color;
    if (look->hair_color >= 0) colors[4] = look->hair_color;
    *waist_color = colors[2];
    const ViewerDye *dye = look->dye;
    if (!dye) return;

    if (look->dye_item->flags2 & VS2_USE_SKIN) { // useSkin: everything takes the skin colour
        colors[1] = colors[2] = colors[3] = colors[5] = *waist_color = colors[0];
        return;
    }
    // dye fields: skinCol, shirtCol, sleeveCol, pantsCol, waistCol (|| pants), shoeCol, hairCol
    static const int target[VIEWER_DYE_PARTS] = {0, 1, 5, 2, -1, 3, 4};
    for (int i = 0; i < VIEWER_DYE_PARTS; i++) {
        const int col = dye_pick(&dye->parts[i], uid, i);
        if (col < 0 || target[i] < 0) continue;
        colors[target[i]] = col;
    }
    const int waist = dye_pick(&dye->parts[VIEWER_DYE_WAIST], uid, VIEWER_DYE_WAIST);
    *waist_color = waist >= 0 ? waist : colors[2];
}

// RENDER.loadMesh(generateMeshData(item), 0, y, 0, rot, scale, holder): the item's model
// ("<store.types[assetType || type]><id>.obj", textures/<...>[_<tex>].png) with its material options
// (skin_material: colour, opacity, side, alphaTest, wire, tileS, frames, the glow).
// TODO (WP-F part 2): pulsT / frames / movT / rAnim / rgb animation, env maps, shaderNum, video.
static Mesh *generate_item_mesh(const ViewerSkin *item, const vec3 at, const vec3 rotation, const float scale, MeshTransform *holder) {
    char model[128], texture[160];
    cosmetic_asset_keys(item, model, sizeof(model), texture, sizeof(texture));
    Geometry *geometry = skin_model(model);
    if (!geometry) return NULL;
    unsigned int texture_id = skin_texture(texture);
    if (!texture_id && strcmp(texture, model) != 0) texture_id = skin_texture(model);

    BasicMaterial *material = basic_material_init();
    material->texture = texture_id;
    Mesh *mesh = mesh_init(geometry, (Material *) material);
    mesh->transform.position = at;
    mesh->transform.rotation = rotation;
    mesh->transform.scale = (vec3) {scale, scale, scale};
    mesh->transform.parent = holder;

    // generateMeshData: glowText = glow, emissive = (glow || sameGlow) && !noEmis
    const int glow = (item->flags & (VS_GLOW | VS_SAME_GLOW)) != 0;
    skin_fx_apply(mesh, item, viewer_skin_tex_truthy(item) ? texture : NULL, model, (item->flags & VS_GLOW) != 0, glow && !(item->flags & VS_NO_EMIS));
    return mesh;
}

// RENDER.loadAnimMesh for hasAnim items: "<model>.<fileExt || gltf>" with the GLTF's own materials,
// every clip playing (timeScale animS). TODO (WP-F part 2): colour / opacity / env on its materials.
static struct GltfInstance *generate_item_model(const ViewerSkin *item, const vec3 at, const vec3 rotation, const float scale, MeshTransform *holder) {
    if (!(item->flags & VS_HAS_ANIM)) return NULL;
    char model[128], texture[160];
    cosmetic_asset_keys(item, model, sizeof(model), texture, sizeof(texture));
    char rel[192];
    snprintf(rel, sizeof(rel), "models/%s.%s", model, item->file_ext ? item->file_ext : "gltf");
    char *path = concat(assets_path(), rel);
    GltfInstance *instance = path ? gltf_instance_create(gltf_model_load(path), item->anim_s > 0.0f ? item->anim_s : 1.0f) : NULL;
    free(path);
    if (!instance) return NULL;
    instance->root.position = at;
    instance->root.rotation = rotation;
    instance->root.scale = (vec3) {scale, scale, scale};
    instance->root.parent = holder;
    skin_fx_apply_gltf(instance, item, model);
    return instance;
}

// One slot: holder = genObj3D(x, y, -sitOffZ) on `parent`, the item inside it at (0, item_y, 0),
// rotation PI / 2 about y, `scale`. hasAnim items whose GLTF loads use it, the others the OBJ.
static void player_generate_cosmetic(PlayerMesh *player_mesh, const int slot, const ViewerSkin *item, MeshTransform *parent,
                                     const float x, const float y, const float item_y, const float scale) {
    MeshTransform *holder = &player_mesh->cosmetic_holders[slot];
    holder->position = (vec3) {x, y, -item->sit_off_z};
    holder->scale = (vec3) {1.0f, 1.0f, 1.0f};
    holder->parent = parent;
    const vec3 rotation = {0.0f, (float) M_PI * 0.5f, 0.0f};
    const vec3 at = {0.0f, item_y, 0.0f};
    player_mesh->cosmetic_models[slot] = generate_item_model(item, at, rotation, scale, holder);
    if (!player_mesh->cosmetic_models[slot]) player_mesh->cosmetics[slot] = generate_item_mesh(item, at, rotation, scale, holder);
}

static float item_scl(const ViewerSkin *item) { return item->scl_mlt != 0.0f ? item->scl_mlt : 1.0f; } // sclMlt || 1

// generateMeshes: body / back / waist / hat / head / face items on lowerBody (body_anchor), the shoes
// on meshHoldObj (anchor) per standing leg. random_leg: randInt(0, 1) for a randomLeg shoe.
static void player_generate_cosmetic_meshes(PlayerMesh *player_mesh, const PlayerLook *look, const int random_leg) {
    const float above_legs = game_constants.player_height - game_constants.leg_height; // playerHeight - legHeight
    MeshTransform *lower = player_mesh->body_anchor;
    const struct { int slot; const ViewerSkin *item; float y; float scale; } items[] = {
        {PLAYER_COSMETIC_BODY, look->body, (above_legs - game_constants.head_scale) / 2.0f, 2.1f},
        {PLAYER_COSMETIC_BACK, look->back, (above_legs - game_constants.head_scale) / 2.0f, 2.1f},
        {PLAYER_COSMETIC_WAIST, look->waist, (above_legs - game_constants.head_scale - 2.0f) / 2.0f, 2.1f},
        {PLAYER_COSMETIC_HAT, look->hat, above_legs, 2.1f},
        {PLAYER_COSMETIC_HEAD, look->head, above_legs, 2.1f},
        {PLAYER_COSMETIC_FACE, look->face, above_legs, 1.0f},
    };
    for (size_t i = 0; i < sizeof(items) / sizeof(items[0]); i++) {
        const ViewerSkin *item = items[i].item;
        if (!item) continue;
        player_generate_cosmetic(player_mesh, items[i].slot, item, lower, 0.0f, items[i].y - item->sit_off, 0.0f, item_scl(item) * items[i].scale);
    }

    // shoes: for i < 2 (standing legs) genObj3D(legScale / 2 * (i == 1 ? -1 : 1), legHeight - sitOff - 0.01,
    // -sitOffZ), the shoe at y -legHeight, scale sclMlt; hidden on the other leg with randomLeg
    if (look->shoe && player_mesh->legs[0]) {
        for (int i = 0; i < 2; i++) {
            const float x = game_constants.leg_scale * 0.5f * (i == 1 ? -1.0f : 1.0f);
            player_generate_cosmetic(player_mesh, PLAYER_COSMETIC_SHOE_R + i, look->shoe, player_mesh->anchor, x,
                                     game_constants.leg_height - look->shoe->sit_off - 0.01f, -game_constants.leg_height, item_scl(look->shoe));
            player_mesh->shoe_hidden[i] = (look->shoe->flags2 & VS2_RANDOM_LEG) && random_leg != i;
        }
    }
}

// Euler angles (three.js 'XYZ', the MeshTransform default: R = Rx * Ry * Rz) of the rotation whose
// columns are the axes x, y, z.
static vec3 euler_xyz_from_axes(const vec3 x, const vec3 y, const vec3 z) {
    const float sy = fmaxf(-1.0f, fminf(1.0f, z.x));
    if (fabsf(z.x) < 0.9999999f) return (vec3) {atan2f(-z.y, z.z), asinf(sy), atan2f(-y.x, x.x)};
    return (vec3) {atan2f(y.z, y.y), asinf(sy), 0.0f};
}

// The wrist item (v10 genArms' wrist frame, genWristFrame, and the wristMeshes loop in its player
// build): on the left arm of every player's model (own, preview and remote), for every arm pose. The frame
// hangs off the arm's group (v10 armMeshes[i], the arm's pivot here, which the first-person melee pose
// moves), and is built there: genWristFrame's up axis is the group's, not the arm placement's, so the item
// stays upright in the melee pose too. It sits at wristCuff * lowerArmLength (0.225 * 2.7) below the forearm's centre along
// the forearm: its +Z is the forearm's down axis, its +Y is up made orthogonal to it. The frame's scale is
// wristScale * the arm's width (genArm's 1.2, times 0.68 in the first person), `unit` is that width / 1.2,
// and the item sits in the frame at -sitOff * unit, rotated (sxRot, -PI / 2 + syRot, szRot), scaled by
static void player_generate_wrist(PlayerArms *arms, const ViewerSkin *wrist, const int third_person) {
    PlayerArmMesh *arm = arms->left;
    if (!arm || !arm->lower) return;
    const float unit = third_person ? 1.0f : 0.68f;
    const float arm_width = game_constants.arm_scale * unit;

    // genWristFrame: the wrist point (0, -wristCuff * lowerArmLength, 0) of the forearm and the forearm's down
    // axis, both in the arm group's space (the anchor's rotation included). The forearm is read without its
    // cube scale: that is the geometry's size, and v10's forearm mesh is unscaled.
    MeshTransform forearm = arm->lower->transform;
    forearm.scale = (vec3) {1.0f, 1.0f, 1.0f};
    MeshTransform *group = &arm->pivot;
    const float cuff = -game_constants.wrist_cuff * game_constants.lower_arm_length;
    const vec3 origin = transform_point_to(&forearm, (vec3) {0.0f, 0.0f, 0.0f}, group);
    const vec3 wrist_point = transform_point_to(&forearm, (vec3) {0.0f, cuff, 0.0f}, group);
    const vec3 down = transform_point_to(&forearm, (vec3) {0.0f, -1.0f, 0.0f}, group);
    const vec3 z = {down.x - origin.x, down.y - origin.y, down.z - origin.z};

    // Z = the forearm's down axis, Y = world up made orthogonal to Z (v10 (0, 1, 0) in the group), X = Y x Z
    vec3 y = {-z.y * z.x, 1.0f - z.y * z.y, -z.y * z.z}; // (0, 1, 0) minus its part along Z
    const float y_len = sqrtf(y.x * y.x + y.y * y.y + y.z * y.z);
    y = y_len > 1e-6f ? (vec3) {y.x / y_len, y.y / y_len, y.z / y_len} : (vec3) {0.0f, 0.0f, 1.0f};
    const vec3 x = {y.y * z.z - y.z * z.y, y.z * z.x - y.x * z.z, y.x * z.y - y.y * z.x};

    MeshTransform *holder = &arms->wrist_holder;
    holder->parent = group;
    holder->scale = (vec3) {1.0f, 1.0f, 1.0f};
    holder->rotation_order = ROTATION_ORDER_INTRINSIC;
    holder->position = wrist_point;
    holder->rotation = euler_xyz_from_axes(x, y, z);

    // the item inside the frame (players.js:1127-1130 / v10 loadMesh(item, -sitOffX, -sitOff, -sitOffZ) * unit)
    const vec3 at = {-wrist->sit_off_x * unit, -wrist->sit_off * unit, -wrist->sit_off_z * unit};
    const vec3 rotation = {wrist->sx_rot, (float) -M_PI * 0.5f + wrist->sy_rot, wrist->sz_rot};
    const float scale = game_constants.wrist_scale * arm_width * item_scl(wrist);
    arms->wrist_model = generate_item_model(wrist, at, rotation, scale, holder);
    if (!arms->wrist_model) arms->wrist = generate_item_mesh(wrist, at, rotation, scale, holder);
}

// The weapon charm (players.js:1262-1291): charms/charm_mount_0 at the weapon's charmM{X,Y,Z}Off
// (scale charmMScl), and the charm at charm{X,Y,Z}Off + 0.17 y inside it, rotation -PI, scale
// charmScl, with the charm item's glow / frames / alphaTest. Not on skins with noCharm.
static void player_generate_charm(PlayerArms *arms, const int weapon_id, const ViewerSkin *weapon_skin, const ViewerSkin *charm) {
    const ViewerCharmMount *mount = viewer_charm_mount(weapon_id);
    if (!mount || !charm || (weapon_skin && (weapon_skin->flags2 & VS2_NO_CHARM))) return;
    Geometry *mount_geo = skin_model("charms/charm_mount_0");
    if (!mount_geo) return;
    BasicMaterial *mount_mat = basic_material_init();
    mount_mat->texture = skin_texture("charms/charm_mount_0");
    mount_mat->base.transparent = 1;
    arms->charm_mount = mesh_init(mount_geo, (Material *) mount_mat);
    arms->charm_mount->transform.position = (vec3) {mount->mount[0], mount->mount[1], mount->mount[2]};
    arms->charm_mount->transform.rotation.y = (float) M_PI * 0.5f;
    arms->charm_mount->transform.scale = (vec3) {mount->mount_scl, mount->mount_scl, mount->mount_scl};
    arms->charm_mount->transform.parent = &arms->anchor;

    const vec3 rotation = {0.0f, (float) -M_PI, 0.0f};
    arms->charm = generate_item_mesh(charm, (vec3) {0.0f, 0.0f, 0.0f}, rotation, mount->scl, &arms->charm_mount->transform);
    if (arms->charm) {
        arms->charm->transform.position = (vec3) {mount->offset[0], mount->offset[1] + 0.17f, mount->offset[2]};
        ((BasicMaterial *) arms->charm->material)->base.transparent = 1;
    }
}

static void dye_color_cube(ColorCube *cube, const ViewerSkin *dye) {
    for (size_t k = 0; cube && k < cube->mesh_count; k++) skin_fx_apply_dye(cube->meshes[k], dye);
}

static void player_dye_meshes(PlayerMesh *player_mesh, const size_t loadout_size, const ViewerSkin *dye) {
    if (!(dye->flags & VS_RGB) && dye->color < 0 && dye->opac <= 0.0f && !(dye->tex && !(dye->flags & VS_TEX_NUM)) && !(dye->flags2 & VS2_PROJECTION)) return;
    dye_color_cube(player_mesh->head, dye);
    dye_color_cube(player_mesh->body, dye);
    for (int i = 0; i < 2; i++) {
        dye_color_cube(player_mesh->legs[i], dye);
        PlayerCrouchedLeg *leg = player_mesh->crouched_legs[i];
        if (!leg) continue;
        skin_fx_apply_dye(leg->upper, dye);
        skin_fx_apply_dye(leg->joint, dye);
        dye_color_cube(leg->lower, dye);
    }
    for (size_t w = 0; player_mesh->arms && w < loadout_size; w++) {
        const PlayerArms *arms = player_mesh->arms[w];
        if (!arms) continue;
        for (int j = 0; j < 2; j++) {
            const PlayerArmMesh *arm = j ? arms->left : arms->right;
            if (!arm) continue;
            skin_fx_apply_dye(arm->extender, dye);
            skin_fx_apply_dye(arm->upper, dye);
            skin_fx_apply_dye(arm->joint, dye);
            dye_color_cube(arm->lower, dye);
        }
    }
}

// updateHeadSize(player, headMlt): the head cube scales by headMlt about lowerBody's origin and
// drops 0.48 per mode headMlt step (0.1); the face / hat / head holders scale with it and sit on
// the scaled head (face sitOff / headMlt).
static void player_apply_head_mlt(PlayerMesh *player_mesh, const PlayerLook *look, const float mlt) {
    if (player_mesh->head_mlt == mlt) return;
    player_mesh->head_mlt = mlt;
    const float hs = game_constants.head_scale;
    if (player_mesh->head) {
        const float steps = roundf((mlt - 1.0f) / 0.1f);
        player_mesh->head->transform.scale = (vec3) {hs * mlt, hs * mlt, hs * mlt};
        player_mesh->head->transform.position.y = (game_constants.body_height + hs * 0.5f) * mlt + steps * -0.48f;
    }
    const float top = game_constants.player_height - game_constants.leg_height + (hs * mlt - hs) / 2.0f;
    const struct { int slot; const ViewerSkin *item; float sit; } heads[] = {
        {PLAYER_COSMETIC_FACE, look->face, look->face ? look->face->sit_off / mlt : 0.0f},
        {PLAYER_COSMETIC_HAT, look->hat, look->hat ? look->hat->sit_off : 0.0f},
        {PLAYER_COSMETIC_HEAD, look->head, look->head ? look->head->sit_off : 0.0f},
    };
    for (size_t i = 0; i < sizeof(heads) / sizeof(heads[0]); i++) {
        if (!heads[i].item) continue;
        MeshTransform *holder = &player_mesh->cosmetic_holders[heads[i].slot];
        holder->scale = (vec3) {mlt, mlt, mlt};
        holder->position.y = top - heads[i].sit;
    }
}

// The pet follows its holder (pets.update) and the item particles spawn (players.js itemEffects:
// every 500 ms a body / back / dye / shoes item with `particle` emits PARTICLES.effect at the player:
// shoes at its feet, the others around its middle), on the frame clock (gltf_clock)
static void player_update_pet_and_effects(const Player *player, PlayerMesh *player_mesh) {
    const double now = gltf_clock();
    float dt_ms = player_mesh->pet_clock < 0.0 ? 0.0f : (float) ((now - player_mesh->pet_clock) * 1000.0);
    player_mesh->pet_clock = now;
    if (dt_ms > 100.0f) dt_ms = 100.0f; // a stalled frame / menu: no teleport

    if (player_mesh->pet) {
        // petHolder.getWorldPosition: the anchor turned by its yaw
        const MeshTransform *anchor = player_mesh->anchor;
        const float yaw = anchor->rotation.y;
        const vec3 local = player_mesh->pet_holder.position;
        const vec3 target = {anchor->position.x + local.x * cosf(yaw) + local.z * sinf(yaw), anchor->position.y + local.y,
                             anchor->position.z - local.x * sinf(yaw) + local.z * cosf(yaw)};
        pet_update(player_mesh->pet, target, dt_ms);
        pet_set_visible(player_mesh->pet, player->active || player->uid == -1);
    }

    if (player->uid == -1 || !player->active || dt_ms <= 0.0f) return; // previews: no world particles
    player_mesh->effect_timer -= dt_ms;
    if (player_mesh->effect_timer > 0.0f) return;
    player_mesh->effect_timer = 500.0f;
    PlayerLook look;
    player_look(player, &look);
    const ViewerSkin *items[4] = {look.body, look.back, look.dye_item, look.shoe};
    for (int i = 0; i < 4; i++) {
        if (!items[i] || !(items[i]->flags2 & VS2_PARTICLE)) continue;
        const float spread_y = i == 2 ? 4.0f : 2.0f; // dye: randInt(-4, 4) on y
        const vec3 at = {player->position.x + (float) (rand() % 5 - 2),
                         i == 3 ? player->position.y + 0.1f : player->position.y + player->height / 2.0f + (float) (rand() % (int) (spread_y * 2 + 1)) - spread_y,
                         player->position.z + (float) (rand() % 5 - 2)};
        vfx_item_effect(at, items[i]->particle);
    }
}

// headMlt: the spawn value times Bighead FFA's mode headMlt (only one is ever above 1)
static float player_head_mlt(const Player *player) {
    const float own = player->head_mlt > 0.0f ? player->head_mlt : 1.0f;
    const float mlt = own * mode_head_mlt(player);
    return mlt > 1.0f ? mlt : 1.0f;
}

void player_generate_meshes(Player *player, const int render_you) {
    if (player->mesh) return;
    player->render_you = render_you;
    // leftHand (Game > View Model, v10.js:171160): every model this client builds takes RENDER.leftHand,
    // its upper body mirrored (v10.js:171752; here the arms' offsets flip, generate_arm) and the muzzles with
    // it (vfx.c). Like v10 it holds from the next model build (a respawn), not live.
    player->left_handed = (unsigned char) !!client_render_settings()->left_hand;

    PlayerLook look;
    player_look(player, &look);
    int colors[6], waist_color;
    memcpy(colors, player->game->classes[player->class_index].colors, sizeof(colors));
    player_look_colors(&look, player->uid, colors, &waist_color);

    // TODO: mode specific skins

    // KrunkNative: the local player's model for the third-person camera (third_person.h) is the
    // same third-person model as v7's game.config.thirdPerson builds for renderYou
    const int kn_third_person = render_you && (third_person_model() || prop_hunt_player(player)); // a prop: its own model (Prop Hunt)
    const int third_person = !render_you || kn_third_person || player->game->config.third_person || player->game->map->config.cam_offset.x || player->game->map->config.cam_offset.y || player->game->map->config.cam_offset.z;

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
    player_mesh->render_you = render_you;
    player_mesh->kn_third_person = kn_third_person;
    player_mesh->pet_clock = -1.0;

    // generateMeshes: petHolder = genObj3D(3, player.height, 0) on objInstances; regenMeshes / players.add:
    // game.pets.add(player) (a preview pet for the menu / viewer model, uid -1)
    const float holder_height = player->height > 0.0f ? player->height : game_constants.player_height;
    player_mesh->pet_holder.position = (vec3) {PET_HOLDER_X, holder_height, 0.0f};
    player_mesh->pet_holder.scale = (vec3) {1.0f, 1.0f, 1.0f};
    player_mesh->pet_holder.parent = player_mesh->anchor;
    if (look.pet && player->game->map->config.model != MODEL_TYPE_SPRITE) {
        const float yaw = player->direction.y;
        const vec3 target = {player->position.x + PET_HOLDER_X * cosf(yaw), player->position.y + holder_height, player->position.z - PET_HOLDER_X * sinf(yaw)};
        player_mesh->pet = pet_create(look.pet, target, player->uid == -1);
    }

    if (player->game->map->config.model == MODEL_TYPE_SPRITE) {

    } else {
        // Prop Hunt (players.js :171319): the prop model stands in for the body. The body and arms are still built
        // (the loadout and the arm anchors read them) but scene_add_player_mesh keeps them out of the scene.
        if (prop_hunt_player(player) && third_person) {
            player_mesh->prop = player_prop_mesh(player->prop_id);
            if (player_mesh->prop) player_mesh->prop->transform.parent = player_mesh->anchor;
        }
        // Cosmetic items only exist on third-person models (v12992); a hat / head noHead, noHair, a body
        // noBody and a waist noLegs change the body parts.
        const int no_head = third_person && ((look.hat && (look.hat->flags & VS_NO_HEAD)) || (look.head && (look.head->flags & VS_NO_HEAD)));
        const int no_hair = third_person && ((look.hat && (look.hat->flags2 & VS2_NO_HAIR)) || (look.head && (look.head->flags2 & VS2_NO_HAIR)));
        const int no_body = third_person && look.body && (look.body->flags2 & VS2_NO_BODY);
        const int no_legs = third_person && look.waist && (look.waist->flags & VS_NO_LEGS);
        // UTILS.randInt(0, 1) for a randomLeg shoe, per player so it is stable across regenerations
        const int random_leg = (int) (((unsigned) player->uid * 2246822519u) >> 31);

        if (third_person) {
            player_mesh->body = generate_body(colors[1], waist_color, no_body);
            player_mesh->head = !no_head ? generate_head(colors[0], colors[4], no_hair) : NULL;

            player_mesh->body->transform.parent = player_mesh->body_anchor;
            if (player_mesh->head) player_mesh->head->transform.parent = player_mesh->body_anchor;
        }

        if (third_person && !no_legs) {
            for (int i = 0; i < 2; i++) {
                // legLen: the shoe's pants share, only on the random leg of a randomLeg shoe
                const int shoe_leg = look.shoe && (!(look.shoe->flags2 & VS2_RANDOM_LEG) || random_leg == i);
                ColorCube *leg = generate_leg(i, colors[2], colors[3], shoe_leg ? look.shoe->leg_len : 0.0f);

                leg->transform.parent = player_mesh->anchor;
                player_mesh->legs[i] = leg;
            }

            for (int i = 0; i < 2; i++) {
                PlayerCrouchedLeg *leg = generate_crouched_leg(i, colors[2], colors[3]);

                leg->anchor.parent = player_mesh->anchor;
                player_mesh->crouched_legs[i] = leg;
            }
        }

        if (third_person) {
            player_generate_cosmetic_meshes(player_mesh, &look, random_leg);
            player_apply_head_mlt(player_mesh, &look, player_head_mlt(player));
        }

        player_mesh->arms = calloc(player->loadout_size, sizeof(PlayerArms *));

        if (player_mesh->arms) {
            for (int i = 0; i < player->loadout_size; i++) {
                const Weapon *weapon = player->game->weapons[player->loadout[i]];
                // Nettspend Purple Drank in third person: the trigger hand takes the pistol's grip (its offset and
                // hold height; the reach is set below), not the table's: the Drank's grip is the Piss hose's, low at
                // the belly, so the cup sat at the belt line. The hand is held out in front of the shoulder
                // (NETTSPEND_TP_HAND_REACH, the arm nearly straight), not out to the side: the arm points forward
                // like a pistol's hold. The free (left) hand keeps the Drank's own hold. First person keeps the
                // table's grip.
                Weapon drank_grip;
                const Weapon *arms_weapon = weapon;
                if (third_person && nettspend_weapon_kind(player->loadout[i]) == NETTSPEND_WEAPON_DRANK) {
                    const Weapon *pistol = player->game->weapons[WEAPON_ID_PISTOL];
                    drank_grip = *weapon;
                    drank_grip.offset = pistol->offset;
                    // generate_arm moves each shoulder by -offset.x: the pistol's 1.2 pushed the trigger shoulder into the
                    // torso and the off one out, so the shoulders stay symmetric (x 0); z keeps the pistol's, y the pistol's
                    // less the shoulder rise below
                    drank_grip.offset.x = 0.0f;
                    drank_grip.offset.y -= NETTSPEND_TP_SHOULDER_RISE; // the shoulders up; the hands keep their holds (generate_arm: a hand sits at -hold)
                    drank_grip.right_hold = pistol->right_hold;
                    drank_grip.right_hold.x = -NETTSPEND_TP_HAND_X;
                    drank_grip.right_hold.z = drank_grip.offset.z + NETTSPEND_TP_HAND_REACH;
                    // the off hand's rest is the pelvis joint (the Drank's pelvis height above the feet, hosePelvisHeight 4.55):
                    // the arms anchor sits at the held weapon's offset.y under the upper body, whose origin is player_height -
                    // camera_height above the feet, so the pelvis is pelvis_height - (player_height - camera_height) in the upper
                    // body's frame, and the hand's arm-frame y (the arm solver's hold.y) is that less weapon->offset.y. The hold
                    // is the shoulder's own x, its y NETTSPEND_OFF_HAND_RISE above the pelvis and its z NETTSPEND_OFF_HAND_FWD ahead
                    // of the shoulder: the rest is the hand at about hip height plus a little, past the hip line (the stream's
                    // pelvis_forward ahead of the feet)
                    const float pelvis_height = weapon->stream ? weapon->stream->pelvis_height : game_constants.leg_height;
                    const float left_shoulder_x = -game_constants.chest_width + game_constants.arm_scale * 0.5f - game_constants.arm_inset;
                    drank_grip.left_hold.x = -left_shoulder_x;
                    drank_grip.left_hold.y = pelvis_height + NETTSPEND_OFF_HAND_RISE - (game_constants.player_height - game_constants.camera_height) - weapon->offset.y;
                    drank_grip.left_hold.z = drank_grip.offset.z + NETTSPEND_OFF_HAND_FWD;
                    arms_weapon = &drank_grip;
                }

                // the wrist item's noLeftSleeve (genArms' options: the arms are built with the wrist's flag)
                const int no_left_sleeve = look.wrist && (look.wrist->flags2 & VS2_NO_LEFT_SLEEVE);
                PlayerArms *arms = generate_arms(arms_weapon, colors[1], colors[5], colors[0], third_person, player->left_handed, no_left_sleeve);
                if (!arms) continue;

                arms->anchor.parent = player_mesh->upper_body_anchor;
                player_mesh->arms[i] = arms;
                int sel_skin = -1;                        // arms->selection: the store skin worn,
                char sel_model[128] = "", sel_tex[192] = ""; // the model / texture keys built

                // MELEE MESH (the meleeIndex store item, else melee_0)
                if (weapon->melee) {
                    const ViewerSkin *skin = player_slot_skin(player, i, weapon);
                    char model[64], texture[192];
                    melee_skin_keys(skin, model, sizeof(model), texture, sizeof(texture));

                    GltfInstance *melee_model = NULL;
                    Mesh *melee_carrier = gltf_skin_carrier(skin, model, &melee_model); // hasAnim: GLTF remodel
                    Geometry *melee_geo = melee_carrier ? NULL : skin_model(model);
                    unsigned int melee_texture = melee_carrier ? 0 : skin_texture(texture);
                    if (!melee_carrier && !melee_texture && strcmp(texture, model) != 0) melee_texture = skin_texture(model);
                    if (!melee_carrier && !melee_geo && skin) { // a skin model this build lacks: the default knife
                        skin = NULL;
                        melee_skin_keys(NULL, model, sizeof(model), texture, sizeof(texture));
                        melee_geo = skin_model(model);
                        melee_texture = skin_texture(texture);
                    }

                    if (melee_carrier || (melee_geo && melee_texture)) {
                        sel_skin = skin ? player->skins.melee : -1;
                        snprintf(sel_model, sizeof(sel_model), "%s", model);
                        snprintf(sel_tex, sizeof(sel_tex), "%s", melee_carrier ? "gltf" : texture);
                        Mesh *melee = melee_carrier;
                        if (!melee) {
                            BasicMaterial *melee_mat = basic_material_init();
                            melee = mesh_init(melee_geo, (Material *) melee_mat);
                            melee_mat->texture = melee_texture;
                        }
                        arms->weapon_models[0] = melee_model;
                        if (skin && !melee_carrier) {
                            // melee loadMesh: glowText = glow || sameGlow, emissive (glow || sameGlow), seeThrough transparent
                            const int glow = (skin->flags & (VS_GLOW | VS_SAME_GLOW)) != 0;
                            skin_fx_apply(melee, skin, texture, model, glow, glow);
                        }
                        melee->transform.parent = &arms->anchor;

                        // First person (no src + renderYou) builds the two arms as separate meshes with a base
                        // pose and hangs the melee mesh off the right one (95.js jLrR6 armMeshes).
                        if (render_you) {
                            arms->melee_pose = 1;
                            player_melee_arms_base_pose(arms);
                            melee->transform.parent = &arms->right->pivot;
                        }

                        // first person: + the skin's sxOff / syOff / szOff, flip (y + PI) and sxRot (z)
                        arms->melee_position = g_fp_melee_position;
                        arms->melee_rotation = g_fp_melee_rotation;
                        if (skin) {
                            arms->melee_position.x += skin->sx_off;
                            arms->melee_position.y += skin->sy_off;
                            arms->melee_position.z += skin->sz_off;
                            if (skin->flags & VS_FLIP) arms->melee_rotation.y += (float) M_PI;
                            arms->melee_rotation.z += skin->sx_rot;
                        }
                        melee->transform.position = render_you ? arms->melee_position : (vec3) {1.7f, -0.4f, 1.2f};

                        melee->transform.rotation = render_you ? arms->melee_rotation : g_fp_melee_rotation;
                        if (!render_you) melee->transform.rotation.y = (float) M_PI * 0.5f;

                        const float melee_scale = skin && skin->scl_mlt ? skin->scl_mlt : 0.9f; // ae.sclMlt || 0.9
                        melee->transform.scale.x = melee->transform.scale.y = melee->transform.scale.z = melee_scale;

                        arms->weapon_right = melee;
                    }
                }

                // Nettspend (nettspend.h): the Drank's cup and the Money Spread's cash stack are procedural
                // whatever the table's src / stream say (below, next to the hose)
                const NettspendWeapon kn_kind = nettspend_weapon_kind(player->loadout[i]);
                if (weapon->src && !kn_kind) {
                    // player.skins[slot]: remodel / texture, yOff / zOff, mScl and the glow (ai)
                    const ViewerSkin *skin = player_slot_skin(player, i, weapon);
                    char model[128], texture[192];
                    weapon_skin_keys(weapon, skin, model, sizeof(model), texture, sizeof(texture));
                    // hasAnim: a GLTF remodel, one instance per hand below (empty_geometry marks it)
                    Geometry *skin_geo = gltf_skin_available(skin, model) ? gltf_empty_geometry() : skin_model(model);
                    if (!skin_geo && skin) { // a remodel this build lacks: the plain weapon
                        skin = NULL;
                        weapon_skin_keys(weapon, NULL, model, sizeof(model), texture, sizeof(texture));
                        skin_geo = skin_model(model);
                    }
                    unsigned int skin_tex = skin_geo && skin_geo != gltf_empty_geometry() ? skin_texture(texture) : 0;
                    if (!skin_tex && skin_geo && skin_geo != gltf_empty_geometry() && strcmp(texture, model) != 0) skin_tex = skin_texture(model);
                    const int glow = skin && (skin->flags & (VS_GLOW | VS_SAME_GLOW));
                    if (skin_geo) {
                        sel_skin = skin ? (i == 0 ? player->skins.primary : player->skins.secondary) : -1;
                        snprintf(sel_model, sizeof(sel_model), "%s", model);
                        snprintf(sel_tex, sizeof(sel_tex), "%s", skin_geo == gltf_empty_geometry() ? "gltf" : skin_tex ? texture : "-");
                    }

                    for (int j = 0; j < 2; j++) {
                        Geometry *weapon_geo = skin_geo;
                        const unsigned int weapon_tex = skin_tex;

                        if (!weapon_geo) continue;

                        GltfInstance *weapon_model = NULL;
                        Mesh *weapon_mesh = NULL;
                        if (weapon_geo == gltf_empty_geometry()) {
                            weapon_mesh = gltf_skin_carrier(skin, model, &weapon_model);
                            if (!weapon_mesh) continue;
                        } else {
                            BasicMaterial *weapon_mat = basic_material_init();
                            weapon_mesh = mesh_init(weapon_geo, (Material *) weapon_mat);
                            weapon_mat->texture = weapon_tex;
                            if (weapon->tint) { // KrunkNative weapons (the Tool Gun): a colour over the reused model
                                weapon_mat->color = (vec4) {(float) ((weapon->tint >> 16) & 255) / 255.0f, (float) ((weapon->tint >> 8) & 255) / 255.0f,
                                                            (float) (weapon->tint & 255) / 255.0f, 1.0f};
                            }
                            if (skin) skin_fx_apply(weapon_mesh, skin, texture, model, glow, glow && !(skin->flags & VS_NO_EMIS));
                        }
                        arms->weapon_models[j] = weapon_model;

                        weapon_mesh->transform.position.x = j ? weapon->offset.x * -2.0f : 0.0f;
                        weapon_mesh->transform.position.y = skin ? skin->y_off : 0.0f; // ae.yOff || 0
                        weapon_mesh->transform.position.z = skin ? skin->z_off : 0.0f; // ae.zOff || 0

                        // loadMesh(..., ad.rot || PI / 2, ae.mScl || ad.scale): scale falls back to 1, rot is a y angle or an [x, y, z] array.
                        const float weapon_scale = skin && skin->m_scl ? skin->m_scl : weapon->scale ? weapon->scale : 1.0f;
                        weapon_mesh->transform.scale.x = weapon_mesh->transform.scale.y = weapon_mesh->transform.scale.z = weapon_scale;

                        if (weapon->has_rotation) weapon_mesh->transform.rotation = weapon->rotation;
                        else weapon_mesh->transform.rotation.y = (float) M_PI * 0.5f;
                        weapon_mesh->transform.parent = &arms->anchor;

                        if (j) arms->weapon_left = weapon_mesh;
                        else arms->weapon_right = weapon_mesh;

                        if (!weapon->akimbo) break;
                    }
                }

                // KrunkNative Piss (e1624e4 hoseViewModel): the model-less weapon's hose in the skin colour
                // (genHose color: skinCol). First person in the weapon anchor, held in the trigger hand
                // (KN: hose_view from the hand, see player_trigger_hand); third person on the lower body at
                // the pelvis (hosePelvisHeight - legHeight up, -(hosePelvisForward - hoseLength / 2)
                // forward). Shown and hidden with the slot like any weapon mesh.
                if (kn_kind) {
                    // Nettspend (nettspend.h): the Purple Drank's white styrofoam double cup or the Money Spread's
                    // fan of cash, procedural, in the trigger hand on every model (first person, third person,
                    // the menu's class preview), never the table's stand-ins (the hose, a model). It takes the
                    // hose's place (arms->hose) so the stream leaves from the rim / the bills from the fan's top
                    // (player_hose_points). Both hands take part in the Drank's pour-up: the bottle is the off hand's
                    // (arms->weapon_left, hidden outside a reload, nettspend_pose_pour), in third person too.
                    const int drank = kn_kind == NETTSPEND_WEAPON_DRANK;
                    Mesh *held = drank ? nettspend_cup_mesh() : nettspend_cash_mesh();
                    if (held) {
                        const float mirror = player->left_handed ? -1.0f : 1.0f;
                        const vec3 hand = player_trigger_hand(arms, player->left_handed);
                        snprintf(sel_model, sizeof(sel_model), "%s", drank ? "cup" : "cash");
                        snprintf(sel_tex, sizeof(sel_tex), "#%06x", (unsigned) (drank ? NETTSPEND_FOAM_COLOR : NETTSPEND_CASH_COLOR));
                        held->transform.parent = &arms->anchor;
                        if (third_person) { // the body's hand is seen from afar: a bigger cup / fan (nettspend_pose_cup_rest keeps its base on the fist)
                            const float s = NETTSPEND_TP_SCALE;
                            held->transform.scale = (vec3) {s, s, s};
                        }
                        if (drank) nettspend_pose_cup_rest(held, hand, mirror);
                        else {
                            nettspend_pose_cash(held, hand, mirror);
                            if (third_person) held->transform.position.y += (NETTSPEND_TP_SCALE - 1.0f) * NETTSPEND_CASH_LENGTH * 0.5f - 0.2f;
                        }
                        if (drank) {
                            Mesh *bottle = nettspend_bottle_mesh();
                            if (bottle) {
                                bottle->transform.parent = &arms->anchor;
                                bottle->visible = 0;
                                arms->weapon_left = bottle;
                            }
                        }
                        arms->weapon_right = held;
                        arms->hose = held;
                        arms->hose_length = drank ? NETTSPEND_CUP_LENGTH : NETTSPEND_CASH_LENGTH;
                    }
                } else if (weapon->stream && !weapon->src) {
                    const WeaponStream *stream = weapon->stream;
                    Mesh *hose = player_hose_mesh(stream, colors[0]);
                    if (hose) {
                        snprintf(sel_model, sizeof(sel_model), "hose");
                        snprintf(sel_tex, sizeof(sel_tex), "#%06x", (unsigned) colors[0] & 0xffffffu);
                        if (third_person) {
                            hose->transform.parent = player_mesh->body_anchor;
                            hose->transform.position = (vec3) {0.0f, stream->pelvis_height - game_constants.leg_height,
                                                               -(stream->pelvis_forward - stream->hose_length * 0.5f)};
                        } else {
                            const float mirror = player->left_handed ? -1.0f : 1.0f;
                            const vec3 hand = player_trigger_hand(arms, player->left_handed);
                            hose->transform.parent = &arms->anchor;
                            hose->transform.position = (vec3) {hand.x + stream->hose_view.x * mirror, hand.y + stream->hose_view.y,
                                                               hand.z + stream->hose_view.z};
                            // rotateMesh(hoseMesh, hoseViewRotY, hoseViewRotX, hoseViewRotZ), mirrored left-handed
                            hose->transform.rotation = (vec3) {stream->hose_view_rot.x, stream->hose_view_rot.y * mirror,
                                                               stream->hose_view_rot.z * mirror};
                        }
                        arms->weapon_right = hose;
                        arms->hose = hose;
                        arms->hose_length = stream->hose_length;
                    }
                }

                // v7.1.4 charm (charms[i], the primary's / secondary's) and the own model's wrist item
                if (!weapon->melee && i < 2) {
                    const ViewerSkin *charm = player->game->config.no_cosmetics ? NULL : cosmetic_item(player->cosmetics_v7.charms[i], ITEM_CHARM);
                    player_generate_charm(arms, player->loadout[i], player_slot_skin(player, i, weapon), charm);
                }
                // v10 generateMeshes builds it on every visible player's left arm (no isYou test: wristMeshes
                // on each client-side model), so other players wear their wrist item too
                if (look.wrist) player_generate_wrist(arms, look.wrist, third_person);

                // Sight attachment: only on the first-person (U) and preview (V) models, like jLrR6.
                // v7.1.4: the player's pick (Reflex / Acog), the Acog's per-weapon yOff and its zOff.
                const WeaponAttachment *attachment = weapon_attachment(weapon, player_attach_index(player));
                if (attachment && (render_you || player->uid == -1)) {
                    arms->attachment = generate_attachment_mesh(weapon, attachment, &arms->anchor);
                    if (arms->attachment) {
                        arms->attachment->transform.position.y -= weapon_attachment_y_offset(attachment, player->loadout[i]);
                        arms->attachment->transform.position.z -= attachment->z_offset;
                    }
                }

                const int charm_index = !weapon->melee && i < 2 && arms->charm ? player->cosmetics_v7.charms[i] : -1;
                snprintf(arms->selection, sizeof(arms->selection), "w=%d skin=%d model=%s tex=%s tint=%06x attach=%s charm=%d wrist_id=%d",
                         player->loadout[i], sel_skin, sel_model[0] ? sel_model : "-", sel_tex[0] ? sel_tex : "-", (unsigned) weapon->tint,
                         arms->attachment && attachment ? attachment->src : "-", charm_index, (arms->wrist || arms->wrist_model) && look.wrist ? look.wrist->id : -1);

                // TODO: flaps
            }
        }

        // the dye's material (tex, colour, opacity, glow, rgb) on every body part (genHead / genBody /
        // genLeg / genArms getMat)
        if (look.dye_item) player_dye_meshes(player_mesh, player->loadout_size, look.dye_item);
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

    PlayerMesh *player_mesh = player->mesh;

    // v7 animated cosmetics: RENDER's mixers advance with the frame clock
    for (int i = 0; i < PLAYER_COSMETIC_SLOTS; i++) gltf_instance_update(player_mesh->cosmetic_models[i], gltf_clock());

    player_update_pet_and_effects(player, player_mesh);

    // headMlt changes (Bighead FFA kills): updateHeadSize
    const float head_mlt = player_head_mlt(player);
    if (head_mlt != player_mesh->head_mlt) {
        PlayerLook look;
        player_look(player, &look);
        player_apply_head_mlt(player_mesh, &look, head_mlt);
    }

    // Viewmodel settings (v3.1.1 settings.js -> RENDER.aimAnim/weaponBob/weaponLean/
    // weaponRot/weaponOffX..Z/weaponADSOffY; weaponBob defaults to 0.9).
    // vmSetts "Per Weapon": the local player's held weapon reads its own clones (RENDER.getPlayerWeaponId,
    // v10.js:199009: p.currentWeaponId + 1 for the local renderer's settings, 0 = All)
    const ClientRenderSettings *settings = client_render_settings();
    const int vm_weapon = player->render_you && player->loadout && player->loadout_index >= 0 && player->loadout_index < player->loadout_size
                              ? player->loadout[player->loadout_index] : -1;
#define VM_SETTING(key, value) ui_setting_weapon_float(key, vm_weapon, (float) (value))
    const int animate_aim = VM_SETTING("aimAnim", settings->aim_anim) != 0.0f;
    const float weapon_bobbing_mlt = VM_SETTING("weaponBob", settings->weapon_bob);
    const float weapon_lean_mlt = VM_SETTING("weaponLean", settings->weapon_lean);
    const float weapon_rotation = VM_SETTING("weaponRot", settings->weapon_rot);
    const vec3 weapon_offset_mlt = {VM_SETTING("weaponOffX", settings->weapon_off_x), VM_SETTING("weaponOffY", settings->weapon_off_y),
                                    VM_SETTING("weaponOffZ", settings->weapon_off_z)};
    const float weap_swap_y = VM_SETTING("weapSwapY", settings->weap_swap_y);
    const float weap_reload_y = VM_SETTING("weapReloadY", settings->weap_reload_y);
    const float weapon_ads_off_y = VM_SETTING("weaponADSOffY", settings->weapon_ads_off_y);
#undef VM_SETTING

    const int third_person = player_mesh->kn_third_person || player->game->config.third_person || player->game->map->config.cam_offset.x || player->game->map->config.cam_offset.y || player->game->map->config.cam_offset.z;
    const float aim_val_mlt = animate_aim && player->is_you ? player->aim_val : 0.0f;
    const float aim_mlt = player->weapon->animate_while_aiming ? 0.0f : aim_val_mlt;
    const float anim_mlt = (1.0f - (1.0f - game_constants.aim_anim_mlt) * aim_val_mlt) * game_constants.anim_mlt * weapon_bobbing_mlt;
    const float anim_mlt_crouch = 1.0f - player->crouch_val * 0.8f;
    const float anim_mlt_lean = player->render_you ? (1.0f - aim_val_mlt) * game_constants.anim_mlt : 0.0f; // a3 = Z * animMult
    // JS: (1 - (1 - Z) * (weapon.recoilYMA || 1)) * animMult; no v3.1.1 weapon defines recoilYMA.
    const float recoil_y_mlt = (1.0f - aim_val_mlt) * game_constants.anim_mlt;
    // v7.1.4: the sight's zRec (Acog 0.9) replaces the weapon's recoilZM
    const WeaponAttachment *recoil_attachment = weapon_attachment(player->weapon, player_attach_index(player));
    const float recoil_z_base = recoil_attachment && recoil_attachment->z_recoil ? recoil_attachment->z_recoil :
                                player->weapon->recoil_z_mlt ? player->weapon->recoil_z_mlt : 0.5f;
    const float recoil_z_mlt = 1.0f - recoil_z_base * aim_mlt;
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
    // ao = swapTime / weapon.swapTime. players.js reload() only sets swapTime for isYou on a
    // rendering client, so other players never show the swap dip (their swap_timer is set by
    // player_spawn but only ticks down in procInputs, which they never run here).
    // * RENDER.weapSwapY (v10.js:170917): a negative weapSwapY swaps the weapon in from above
    const float swap_anim = player->is_you && player->weapon->swap_time > 0.0f ? player->swap_timer / player->weapon->swap_time * weap_swap_y : 0.0f;
    const float left_hand_mlt = player->left_handed ? -1.0f : 1.0f;

    const vec3 weapon_offset = {
        player->weapon->offset.x * (player->render_you ? weapon_offset_mlt.x : 1.0f) * left_hand_mlt,
        player->weapon->offset.y * (player->render_you ? weapon_offset_mlt.y : 1.0f),
        player->weapon->offset.z * (player->render_you ? weapon_offset_mlt.z : 1.0f),
    };

    const float reload_speed = player->game->config.reload_speed > 0.0f ? player->game->config.reload_speed : 1.0f; // (reSpd || 1)
    float reload_anim = player->reload_timer > 0.0f && player->weapon->reload_time > 0.0f ? 1.0f - player->reload_timer / (player->weapon->reload_time * reload_speed) : 0.0f;
    if (reload_anim > 0.5f) reload_anim = 1.0f - reload_anim;
    reload_anim *= weap_reload_y; // * RENDER.weapReloadY * weapon.reloY (v10.js:170919; every stock reloY is 1)

    reload_anim *= player->render_you ? 1.0f : 0.3f;
    // Nettspend Purple Drank (nettspend.h): the pour-up is the first-person reload animation, so the
    // upper body keeps still instead of Krunker's dip (which would swing the cup out of view)
    if (player->render_you && !third_person && player->loadout_index >= 0 && player->loadout_index < player->loadout_size &&
        nettspend_weapon_kind(player->loadout[player->loadout_index]) == NETTSPEND_WEAPON_DRANK) reload_anim = 0.0f;

    const float idle_anim = (1.0f - aim_val_mlt * 0.88f) * 1.75f * weapon_bobbing_mlt;
    // renderYou && !thirdPerson ? |stepChase term * 0.5| * a0 : |sin(stepVal) * 6| (v10.js:170922, v7.1.4
    // players.js:381; v3.1.1 bobbed other models by 3.5)
    const float step_y = player->render_you && !third_person ? fabsf(step_anim_rotate * 0.5f) * anim_mlt_lean : fabsf(step_anim * 6.0f);
    const float step_yaw = player->render_you ? third_person ? -step_anim * 0.5f : 0.0f : -step_anim * 2.0f;

    // Every player's model follows its (server-updated) position; only the viewmodel offsets below are first-person.
    player_mesh->anchor->position = player->position;
    player_mesh->anchor->position.y += step_y;

    if (player_mesh->prop) {
        // Prop Hunt: no step bob (players.js :170101), the model turns with the look only while propRot
        // is on, else it keeps propRotSaved (:170946)
        player_mesh->anchor->position = player->position;
        player_mesh->anchor->rotation.y = prop_model_yaw(player);
    } else if (player->game->map->config.model != MODEL_TYPE_SPRITE) {
        player_mesh->anchor->rotation.y = player->direction.y + step_yaw;
    }

    step_half -= step_half * player->crouch_val * game_constants.crouch_anim_mlt;
    step_anim -= step_anim * player->crouch_val * game_constants.crouch_anim_mlt;

    if (!player->render_you || third_person) { // v7 swings every legMeshes entry (renderYou + thirdPerson has legs)
        for (int i = 0; i < 4; i++) {
            // waist items with noLegs (and first-person meshes) have no legs to swing
            if (i < 2 ? !player_mesh->legs[i] : !player_mesh->crouched_legs[i - 2]) continue;
            MeshTransform *leg_anchor = i < 2 ? &player_mesh->legs[i]->transform : &player_mesh->crouched_legs[i - 2]->anchor;
            // v7.1.4 swings the legs x9 (v3.1.1: x7); the shoes follow their leg
            leg_anchor->rotation.x = step_anim * (i == 1 || i == 3 ? 1.0f : -1.0f) * 9.0f + (i > 1 ? -0.6f : 0.0f);
            if (i < 2) player_mesh->cosmetic_holders[PLAYER_COSMETIC_SHOE_R + i].rotation.x = leg_anchor->rotation.x;
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
    const WeaponAttachment *attachment = weapon_attachment(player->weapon, player_attach_index(player));
    float aim_offset_y = attachment ? attachment->aim_offset_y + player->weapon->aim_offset_y : 0.0f;
    // weaponADSOffY: if (renderYou && weaponADSOffY != 1) aA = (aA || 0.1) * weaponADSOffY
    if (player->render_you && weapon_ads_off_y != 1.0f) aim_offset_y = (aim_offset_y ? aim_offset_y : 0.1f) * weapon_ads_off_y;

    // toggleAim: a `scope` weapon (Sniper Rifle) hides its viewmodel once fully aimed so the scope reticle is clear.
    // v10 toggleAim hides weaponMeshes[loadoutIndex] whatever the camera (v10.js 172054): KrunkNative's third
    // person hides the held weapon of the local model too (the host's thirdPerson keeps v7's rule).
    if (player->weapon->scope && player->render_you && (!third_person || player_mesh->kn_third_person)) {
        player_arms_set_visible(player_mesh->arms[player->loadout_index], aim_val_mlt < 1.0f);
    }

    // showHands (index.js updateWeaponShow(0, v)): the first-person arms of the weapon in
    // hand hide while the weapon itself stays (also under the scope rule above).
    if (player->render_you && !third_person && player->loadout_index >= 0 && player->loadout_index < player->loadout_size) {
        const int scoped_out = player->weapon->scope && aim_val_mlt >= 1.0f;
        player_arm_parts_set_visible(player_mesh->arms[player->loadout_index], settings->show_hands && !scoped_out);
    }

    // 1.5 * -stepHalfRotate * (renderYou && !thirdPerson ? 0 : 1): other players' (and a third-person
    // model's) held weapon and arms swing with their steps (v10.js:170997, v7.1.4 players.js:499)
    arm_anchor->rotation.x = (player->render_you && !third_person ? 0.0f : step_half_rotate * -1.5f) +
        -cosf(player->idle_anim) * bob_crouch_mlt * 0.01f * idle_anim +
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
        // v10 0; Krunker Strike keeps v7's -PI * 0.1 (the ruleset's constants, strike_rules.h)
        const float crouch_lean = game_constants_of(player->game)->crouch_lean * player->crouch_val;

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
            // updateHeight: legHeight - crouchDst + 0.5 + (crouchDst - crouchDst * crouchVal)
            leg->anchor.position.y = game_constants.leg_height - game_constants.crouch_distance + 0.5f + (game_constants.crouch_distance - crouch_distance);
        }

        // updateHeight: a shoe shows with its standing leg (randomLeg: only on its leg)
        for (int i = 0; i < 2; i++) {
            const int visible = !player_mesh->shoe_hidden[i] && !player->crouch_val;
            Mesh *shoe = player_mesh->cosmetics[PLAYER_COSMETIC_SHOE_R + i];
            if (shoe) shoe->visible = visible;
            if (player_mesh->cosmetic_models[PLAYER_COSMETIC_SHOE_R + i]) gltf_instance_set_visible(player_mesh->cosmetic_models[PLAYER_COSMETIC_SHOE_R + i], visible);
        }
    }

    // Nettspend Purple Drank (nettspend.h): one arm holds the cup out, the other only comes up for the
    // refill (user, 2026-10-15: "hold out the cup with one arm (the right one) and only pull out the left
    // arm in order to refill it"). Outside the pour-up the off arm is away: third person at its rest (the hand at
    // about hip height, forward of the hip); first person player_off_hand_away (turned down about its shoulder, the hand at
    // the side). First person also hides it, out of view like a one-handed viewmodel's. The pour-up of the local player's reload, first and third person: the
    // off hand brings the bottle up from there, over the cup, tips it and takes it back down, while the
    // trigger hand holds the cup up, both arms reaching where the cup and the bottle are
    // (player_arm_reach). Only the local player's reload_timer runs on the client (procInputs), like the
    // reload dip above: the others' pour stays hidden, their off hand at the side.
    if (held_arms && held_arms->hose && !held_arms->melee_pose && nettspend_weapon_kind(player->loadout[player->loadout_index]) == NETTSPEND_WEAPON_DRANK) {
        PlayerArmMesh *trigger_arm = player->left_handed ? held_arms->left : held_arms->right;
        PlayerArmMesh *off_arm = player->left_handed ? held_arms->right : held_arms->left;
        for (int i = 0; i < 2; i++) {
            PlayerArmMesh *arm = i ? off_arm : trigger_arm;
            if (!arm) continue;
            arm->pivot.position = arm->pivot.rotation = (vec3) {0.0f, 0.0f, 0.0f};
        }
        const vec3 trigger_rest = player_trigger_hand(held_arms, player->left_handed), off_rest = player_off_hand(held_arms, player->left_handed);
        // the off hand outside the pour-up: third person hangs at its rest (forward of the hip, the bend generated for it);
        // first person keeps its away pose (player_off_hand_away)
        const vec3 off_away = third_person ? off_rest : player_off_hand_away(off_arm, off_rest);
        const float reload_time = player->weapon->reload_time * reload_speed;
        const float progress = player->render_you && player->reload_timer > 0.0f && reload_time > 0.0f ? 1.0f - player->reload_timer / reload_time : -1.0f;
        const int pouring = progress >= 0.0f && progress <= 1.0f;
        vec3 cup_hand, bottle_hand;
        nettspend_pose_pour(held_arms->hose, held_arms->weapon_left, trigger_rest, off_away, left_hand_mlt, held_arms->hose->transform.scale.x, progress,
                            &cup_hand, &bottle_hand);
        if (pouring) player_arm_reach(trigger_arm, trigger_rest, cup_hand, player_arm_elbow(held_arms, trigger_arm), NULL);
        else if (trigger_arm && trigger_arm->upper) {
            // third person, not pouring: the arm keeps its generated rest (the pistol's hold with the hand reaching
            // NETTSPEND_TP_HAND_REACH out in front, player_generate_meshes), so it points forward like a pistol held
            // out; the cup sits in the hand
            nettspend_pose_cup_rest(held_arms->hose, trigger_rest, left_hand_mlt);
        }
        // away, or carrying the bottle: the elbow bends down and back (NETTSPEND_OFF_ELBOW_POLE_*)
        const vec3 off_pole = {0.0f, NETTSPEND_OFF_ELBOW_POLE_Y, NETTSPEND_OFF_ELBOW_POLE_Z};
        player_arm_reach(off_arm, off_rest, bottle_hand, player_arm_elbow(held_arms, off_arm), &off_pole);
        // first person: the off arm shows only for the refill (showHands above already decided the arms)
        if (player->render_you && !third_person && !pouring) player_arm_mesh_set_visible(off_arm, 0);
    }

    // the charm sways with the weapon (players.js:501-504)
    PlayerArms *charm_arms = player->loadout_index >= 0 && player->loadout_index < player->loadout_size ? player_mesh->arms[player->loadout_index] : NULL;
    if (charm_arms && charm_arms->charm) {
        const float sway_x = -player->lean_anim.x * 3.8f + player->crouch_val * 0.2f * anim_mlt_lean - bob_anim_y * 3.5f + player->land_bob_yr * 5.0f - step_anim * 2.0f;
        charm_arms->charm->transform.rotation.y = (float) -M_PI - arm_anchor->rotation.y * 0.3f;
        charm_arms->charm->transform.rotation.x = fmaxf(sway_x, -0.3f);
        charm_arms->charm->transform.rotation.z = player->direction.x * 0.2f - player->recoil_tween[0] * 1.5f - player->recoil_anim * 4.0f;
    }

    player_weapon_models_update(player_mesh, player->loadout_size);
}

void player_meshes_fini(Player *player) {
    if (!player->mesh) return;

    PlayerMesh *player_mesh = player->mesh;

    if (player_mesh->body) {
        for (size_t i = 0; i < player_mesh->body->mesh_count; i++) {
            fx_mesh_fini(player_mesh->body->meshes[i]);
        }

        free(player_mesh->body);
    }

    if (player_mesh->head) {
        for (size_t i = 0; i < player_mesh->head->mesh_count; i++) {
            fx_mesh_fini(player_mesh->head->meshes[i]);
        }

        free(player_mesh->head);
    }

    if (player_mesh->arms) {
        for (size_t i = 0; i < player->loadout_size; i++) {
            PlayerArms *arms = player_mesh->arms[i];
            if (!arms) continue;

            if (arms->weapon_right) fx_mesh_fini(arms->weapon_right);
            if (arms->weapon_left) fx_mesh_fini(arms->weapon_left);
            for (int k = 0; k < 2; k++) fx_gltf_fini(arms->weapon_models[k]);
            if (arms->charm) fx_mesh_fini(arms->charm);
            if (arms->charm_mount) fx_mesh_fini(arms->charm_mount);
            if (arms->wrist) fx_mesh_fini(arms->wrist);
            fx_gltf_fini(arms->wrist_model);
            if (arms->attachment) fx_mesh_fini(arms->attachment);

            for (int j = 0; j < 2; j++) {
                const PlayerArmMesh *arm = j ? arms->left : arms->right;

                if (arm->extender) fx_mesh_fini(arm->extender);

                if (arm->upper) fx_mesh_fini(arm->upper);
                if (arm->joint) fx_mesh_fini(arm->joint);

                for (size_t k = 0; k < arm->lower->mesh_count; k++) {
                    fx_mesh_fini(arm->lower->meshes[k]);
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
            fx_mesh_fini(leg->meshes[j]);
        }

        free(leg);
    }

    for (int i = 0; i < 2; i++) {
        PlayerCrouchedLeg *leg = player_mesh->crouched_legs[i];
        if (!leg) continue;

        fx_mesh_fini(leg->upper);
        fx_mesh_fini(leg->joint);

        for (size_t j = 0; j < leg->lower->mesh_count; j++) {
            fx_mesh_fini(leg->lower->meshes[j]);
        }

        free(leg);
    }

    for (int i = 0; i < PLAYER_COSMETIC_SLOTS; i++) {
        fx_mesh_fini(player_mesh->cosmetics[i]);
        fx_gltf_fini(player_mesh->cosmetic_models[i]);
    }

    if (player_mesh->prop) fx_mesh_fini(player_mesh->prop);
    pet_free(player_mesh->pet);
    free(player_mesh->upper_body_anchor);
    free(player_mesh->body_anchor);
    free(player_mesh->anchor);
    free(player_mesh);

    player->mesh = NULL;
}
// ---------------------------------------------------------------- debug log
// KRUNKNATIVE_PLAYER_LOG=<file>: one JSON line per frame per other player with its state and
// the world matrices (row-major) of the model nodes, the same nodes tools/kref/players-ref.js
// records from the real client: root (objInstances), lower (lowerBody), upper (upperBody),
// wm (weaponMeshes[i]: the held arms anchor), the held arm part meshes (unit cubes, y 0..1)
// and the weapon mesh. tools/kref/players-compare.py compares the two.
static void player_log_matrix(FILE *file, const char *key, MeshTransform *node) {
    Mesh probe = {0};
    probe.transform.scale = (vec3) {1.0f, 1.0f, 1.0f};
    probe.transform.parent = node;
    mesh_update_transform_matrix(&probe);
    fprintf(file, "\"%s\":[", key);
    for (int i = 0; i < 16; i++) fprintf(file, "%s%.5f", i ? "," : "", probe.transform_matrix[i]);
    fputs("]", file);
}

static void player_log_mesh(FILE *file, Mesh *mesh, int *first) {
    if (!mesh) return;
    mesh_update_transform_matrix(mesh);
    fprintf(file, "%s{\"vis\":%d,\"m\":[", *first ? "" : ",", mesh->visible);
    for (int i = 0; i < 16; i++) fprintf(file, "%s%.5f", i ? "," : "", mesh->transform_matrix[i]);
    fputs("]}", file);
    *first = 0;
}

void player_mesh_debug_log(Player *player, const double now) {
    static FILE *file;
    static int checked;
    if (!checked) {
        checked = 1;
        const char *path = getenv("KRUNKNATIVE_PLAYER_LOG");
        if (path && *path) file = fopen(path, "w");
    }
    if (!file || !player->mesh || player->render_you || !player->active) return;

    PlayerMesh *pm = player->mesh;
    fprintf(file, "{\"t\":%.2f,\"uid\":%d,\"x\":%.4f,\"y\":%.4f,\"z\":%.4f,\"yaw\":%.4f,\"pitch\":%.4f,\"cr\":%.4f,\"step\":%.4f,\"chase\":%.4f,\"g\":%d,\"wi\":%d,\"aim\":%.3f,\"interp\":%d,",
            now * 1000.0, player->uid, player->position.x, player->position.y, player->position.z, player->direction.y, player->direction.x,
            player->crouch_val, player->step_val, player->step_chase, player->on_ground, player->loadout_index, player->aim_val, player->interpolate);
    player_log_matrix(file, "root", pm->anchor);
    fputc(',', file);
    player_log_matrix(file, "lower", pm->body_anchor);
    fputc(',', file);
    player_log_matrix(file, "upper", pm->upper_body_anchor);

    PlayerArms *arms = player->loadout_index >= 0 && player->loadout_index < player->loadout_size ? pm->arms[player->loadout_index] : NULL;
    if (arms) {
        fputc(',', file);
        player_log_matrix(file, "wm", &arms->anchor);
        fputs(",\"arms\":[", file);
        int first = 1;
        for (int j = 0; j < 2; j++) {
            PlayerArmMesh *arm = j ? arms->left : arms->right;
            if (!arm) continue;
            player_log_mesh(file, arm->extender, &first);
            player_log_mesh(file, arm->upper, &first);
            player_log_mesh(file, arm->joint, &first);
            for (size_t k = 0; arm->lower && k < arm->lower->mesh_count; k++) player_log_mesh(file, arm->lower->meshes[k], &first);
        }
        fputs("],\"weapon\":[", file);
        first = 1;
        player_log_mesh(file, arms->weapon_right, &first);
        player_log_mesh(file, arms->weapon_left, &first);
        fputc(']', file);
    }
    fputs(",\"legs\":[", file);
    for (int i = 0; i < 4; i++) {
        const MeshTransform *leg = i < 2 ? (pm->legs[i] ? &pm->legs[i]->transform : NULL) : (pm->crouched_legs[i - 2] ? &pm->crouched_legs[i - 2]->anchor : NULL);
        fprintf(file, "%s%.4f", i ? "," : "", leg ? leg->rotation.x : 0.0f);
    }
    fputs("]}\n", file);
    fflush(file);
}
