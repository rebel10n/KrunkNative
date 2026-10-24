// Krunker v10.0.3 player poses and game.applyFreezeOthers on the authority (shared/include/player_pose.h).
// Line numbers: deob-10.0.3.js (Krunker_v10.0.3/deob/game.deob.js).
#include <player_pose.h>
#include <shared.h>
#include <net_ext.h>
#include "triggers_internal.h" // trigger_send_change_position ("chgp")
#include <math.h>
#include <string.h>

void player_pose_arm_target(const PlayerPose *pose, const int side, float out[3]) {
    // applyPose (:193375): pose[armL / armR] || (hold ? [-0.25, 0, 0] : holdL && left ? tp [-1.2, 0, -0.6] : [0, 0, 0])
    static const float hold[3] = {-0.25f, 0.0f, 0.0f};     // IIïiîîi (:193330)
    static const float hold_l[3] = {-1.2f, 0.0f, -0.6f};   // IIìiïïï.tp (:193331-193334)
    const float *src = NULL;
    if (side == 0 && (pose->has & PLAYER_POSE_ARM_L)) src = pose->arm_l;
    else if (side == 1 && (pose->has & PLAYER_POSE_ARM_R)) src = pose->arm_r;
    else if (pose->hold) src = hold;
    else if (pose->hold_l && side == 0) src = hold_l;
    for (int i = 0; i < 3; i++) out[i] = src ? src[i] : 0.0f;
}

int player_pose_splits_arms(const PlayerPose *pose) {
    // IÎíïíîí (:193337-193339): !!pose && (!!armL || !!armR || !!holdL)
    return pose && pose->has && ((pose->has & (PLAYER_POSE_ARM_L | PLAYER_POSE_ARM_R)) || pose->hold_l);
}

int player_pose_hides_weapon(const PlayerPose *pose) {
    // isWeaponModelHidden (:195350): !!poseData && (poseData.weapon === 0 || !!poseData.hold)
    return pose && pose->has && (((pose->has & PLAYER_POSE_WEAPON) && !pose->weapon) || pose->hold);
}

static void pose_to_packet(const Player *player, NetPlayerPosePacket *packet) {
    memset(packet, 0, sizeof(*packet));
    packet->uid = player->uid;
    const PlayerPose *pose = &player->pose;
    if (!pose->has) return; // pose || 0
    packet->has = pose->has & PLAYER_POSE_KEYS;
    packet->legs = pose->legs;
    packet->weapon = pose->weapon;
    packet->bob = pose->bob;
    packet->face_ride = pose->face_ride;
    packet->hold = pose->hold;
    packet->hold_l = pose->hold_l;
    packet->leg_x = pose->leg_x;
    packet->body_y = pose->body_y;
    packet->torso_x = pose->torso_x;
    memcpy(packet->arm_l, pose->arm_l, sizeof(packet->arm_l));
    memcpy(packet->arm_r, pose->arm_r, sizeof(packet->arm_r));
}

uint16_t player_pose_pack(const Player *player, uint8_t *out, const uint16_t size) {
    if (!player || !out) return 0;
    NetPlayerPosePacket packet;
    pose_to_packet(player, &packet);
    return net_ext_pack(out, size, NET_EXT_PLAYER_POSE, &packet, sizeof(packet));
}

int game_set_player_pose(Game *game, Player *player, const PlayerPose *pose) {
    // setPlayerPose (:221035-221042): if (!isServer || !player) return false; poseData = sanitizePose(pose);
    // broadcast(PLAYER_POSE, sid, poseData || 0). The script's setter sanitized the object already
    // (ks_pose.c); a direct caller's values are kept to the same keys.
    if (!game || !player || !game_is_authority(game)) return 0;
    if (pose && (pose->has & PLAYER_POSE_KEYS)) {
        player->pose = *pose;
        player->pose.has &= PLAYER_POSE_KEYS;
    } else {
        memset(&player->pose, 0, sizeof(player->pose));
    }
    uint8_t buf[NET_EXT_HEADER_SIZE + sizeof(NetPlayerPosePacket)];
    const uint16_t n = player_pose_pack(player, buf, sizeof(buf));
    if (n) game_send(game, NET_TARGET_ALL, NET_PACKET_EXT, buf, n);
    return 1;
}

void game_clear_player_poses(Game *game) {
    // endGame (:221901-221903): if (player.poseData) this.setPlayerPose(player, null), per player
    if (!game || !game_is_authority(game)) return;
    for (size_t i = 0; i < game->player_count; i++) {
        Player *player = game->players[i];
        if (player && player->pose.has) game_set_player_pose(game, player, NULL);
    }
}

int game_apply_freeze_others(Game *game, const int freeze, const int except_uid) {
    // applyFreezeOthers (:221547-221573). v10 writes the script-visible flags (defaultMovement /
    // defaultVelocity = !freeze, disableShooting / disableMelee = freeze); KrunkNative keeps the freeze
    // apart in Player.frozen (the same flag "frz" sets on the client), which ks_player_allows reads for
    // exactly those four (ks_game.c). Deliberate difference: an unfreeze here gives back whatever the
    // script had set before, where v10 turns the four defaults on again.
    // KN has no spectating players (a spectator is a connection without a player), so the
    // `spectating` skips of v10 have nothing to skip.
    if (!game || !game_is_authority(game)) return 0;
    game->freeze_others = (unsigned char) !!freeze;
    int changed = 0;
    for (size_t i = 0; i < game->player_count; i++) {
        Player *player = game->players[i];
        if (!player || (except_uid >= 0 && player->uid == except_uid)) continue;
        if (!freeze && !player->frozen) continue;
        player->frozen = (unsigned char) !!freeze;
        if (freeze && player->active) { // changePosition(p, x, y, z, true, true, undefined, true)
            game_change_position(game, player, player->position, 1, 1, 0, 0);
            trigger_send_change_position(game, player, 1, 1, 0, 0);
        }
        const NetCheatPacket packet = {.flag = NET_CHEAT_FROZEN, .on = (uint8_t) !!freeze}; // SET_FROZEN "frz"
        game_send(game, player->uid, NET_PACKET_CHEAT, &packet, sizeof(packet));
        changed++;
    }
    return changed;
}
