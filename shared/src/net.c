#include <shared.h>
#include <flavor.h> // the process product flavor: carried in the HELLO rulesets byte (shared.h)
#include <knet.h> // virtual (UDP) descriptors
#include <math.h>
#include <net_ext.h> // NetServerLinePacket (the static asserts after net_format_server_line)
#include <stddef.h>
#include <stdio.h>
#include <string.h>

// Optional trailing fields: older peers send the prefix, the readers below fill the rest with defaults.
_Static_assert(offsetof(NetEntPacket, cosmetics) == NET_ENT_PACKET_V2_SIZE, "ENT cosmetics must follow secondary_index");
_Static_assert(offsetof(NetSpawnPacket, cosmetics) == NET_SPAWN_PACKET_V1_SIZE, "SPAWN cosmetics must follow secondary_index");
_Static_assert(NET_ENT_PACKET_V4_SIZE == NET_ENT_PACKET_V3_SIZE + 2 && NET_ENT_PACKET_V5_SIZE == NET_ENT_PACKET_V4_SIZE + 4, "ENT spectate, slid_cont then spray_index must follow the cosmetics");
_Static_assert(NET_ENT_PACKET_V6_SIZE == NET_ENT_PACKET_V5_SIZE + sizeof(PlayerSkins) && sizeof(PlayerSkins) == 12, "ENT weapon skins must follow spray_index");
_Static_assert(NET_ENT_PACKET_V7_SIZE == NET_ENT_PACKET_V6_SIZE + sizeof(float) && NET_ENT_PACKET_V7_SIZE == 58, "ENT lag_comp must follow the skins");
_Static_assert(NET_ENT_PACKET_V8_SIZE == NET_ENT_PACKET_V7_SIZE + 6, "ENT attach_index, chall_mode, anon_mode must be the last bytes (WP-C)");
_Static_assert(NET_SPAWN_PACKET_V3_SIZE == NET_SPAWN_PACKET_V2_SIZE + 8, "SPAWN attach_index, head_mlt must be the last bytes (WP-C)");
_Static_assert(sizeof(PlayerCosmeticsV7) == 36, "PlayerCosmeticsV7 is nine int32 (wire)");
_Static_assert(NET_ENT_PACKET_V9_SIZE == NET_ENT_PACKET_V8_SIZE + 37, "ENT cosmetics_v7, show_badges must follow anon_mode (WP-F)");
_Static_assert(NET_SPAWN_PACKET_V4_SIZE == NET_SPAWN_PACKET_V3_SIZE + 36, "SPAWN cosmetics_v7 must follow head_mlt (WP-F)");
_Static_assert(NET_RECON_PACKET_V1_SIZE == 58 && NET_RECON_PACKET_V2_SIZE == 72, "RECON v7 locks must be the last bytes (WP-C; net-bridge reads 58)");
_Static_assert(NET_RECON_PACKET_V3_SIZE == NET_RECON_PACKET_V2_SIZE + 17, "RECON grapple must follow the v7 locks");
_Static_assert(NET_INPUT_PACKET_V1_SIZE == 22 && NET_INPUT_PACKET_V2_SIZE == 23 && sizeof(NetInputPacket) == 24, "INPUT slowdown then scroll must be the last bytes");

#ifdef WIN32
#include <winsock2.h>
#define NET_SEND(fd, data, len) send((fd), (const char*) (data), (int) (len), 0)
#define NET_ERRNO WSAGetLastError()
#define NET_WOULD_BLOCK(err) ((err) == WSAEWOULDBLOCK)
#else
#include <sys/socket.h>
#include <errno.h>
// A peer that closed its socket must not kill the process with SIGPIPE: Linux
// takes MSG_NOSIGNAL per call, macOS/BSD set SO_NOSIGPIPE on the socket instead.
#ifdef MSG_NOSIGNAL
#define NET_SEND(fd, data, len) send((fd), (data), (len), MSG_NOSIGNAL)
#else
#define NET_SEND(fd, data, len) net_send_nosigpipe((fd), (data), (len))
static ssize_t net_send_nosigpipe(const int fd, const void *data, const size_t length) {
#ifdef SO_NOSIGPIPE
    const int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
    return send(fd, data, length, 0);
}
#endif
#define NET_ERRNO errno
#define NET_WOULD_BLOCK(err) ((err) == EAGAIN || (err) == EWOULDBLOCK)
#endif

int net_send_packet(const int fd, const NetPacketType type, const void *payload, const uint16_t length) {
    if (length > NET_MAX_PAYLOAD) return 0;
    if (fd >= NET_LOOPBACK_FD_BASE && net_loopback_send()) return net_loopback_send()(fd, (uint8_t) type, payload, length); // in-process sandbox
    if (fd >= NET_VIRTUAL_FD_BASE) { // a UDP (knet) peer: server/src/net_udp.c
        const NetVirtualSendFn send_fn = net_virtual_send();
        return send_fn ? send_fn(fd, (uint8_t) type, payload, length) : 0;
    }

    uint8_t frame[NET_PACKET_HEADER_SIZE + NET_MAX_PAYLOAD];
    frame[0] = (uint8_t) type;
    frame[1] = (uint8_t) (length >> 8);
    frame[2] = (uint8_t) (length & 0xff);

    if (payload && length) memcpy(frame + NET_PACKET_HEADER_SIZE, payload, length);

    size_t sent = 0;
    const size_t total = NET_PACKET_HEADER_SIZE + length;

    while (sent < total) {
        const int wrote = NET_SEND(fd, frame + sent, total - sent);

        if (wrote <= 0) {
            if (wrote < 0 && NET_WOULD_BLOCK(NET_ERRNO)) continue;
            return 0;
        }

        sent += (size_t) wrote;
    }

    return 1;
}

int net_buffer_push(NetPacketBuffer *buffer, const void *data, const size_t length) {
    if (!length) return 1;
    if (length > sizeof(buffer->data) - buffer->length) return 0;

    memcpy(buffer->data + buffer->length, data, length);
    buffer->length += length;

    return 1;
}

int net_buffer_next(NetPacketBuffer *buffer, NetPacket *packet) {
    if (buffer->length < NET_PACKET_HEADER_SIZE) return 0;

    const uint16_t length = (uint16_t) (((uint16_t) buffer->data[1] << 8) | buffer->data[2]);
    if (length > NET_MAX_PAYLOAD) return -1;

    const size_t frame_length = NET_PACKET_HEADER_SIZE + length;
    if (buffer->length < frame_length) return 0;

    packet->type = buffer->data[0];
    packet->length = length;
    if (length) memcpy(packet->payload, buffer->data + NET_PACKET_HEADER_SIZE, length);

    buffer->length -= frame_length;
    if (buffer->length) memmove(buffer->data, buffer->data + frame_length, buffer->length);

    return 1;
}

void net_packet_from_input(NetInputPacket *packet, const Input *input) {
    packet->seq = input->seq;
    packet->move_dir = input->move_dir;
    packet->delta = input->delta;
    packet->x_dir = input->x_dir;
    packet->y_dir = input->y_dir;
    packet->flags = 0;
    packet->swap = input->swap;
    const float slowdown = roundf(input->slowdown * 10.0f);
    packet->slowdown = (uint8_t) (slowdown <= 0.0f ? 0 : slowdown >= 10.0f ? 10 : slowdown);
    packet->scroll = (int8_t) (input->scroll > 0 ? 1 : input->scroll < 0 ? -1 : 0);
    // KrunkNative Object Manager: a slot pick rides in the scroll byte as slot + 2 (2..127). Clients
    // only send it to servers that granted extra weapons; older servers read any > 0 as a scroll.
    if (input->select_slot && input->select_slot <= 126) packet->scroll = (int8_t) (input->select_slot + 1);

    if (input->shoot) packet->flags |= NET_INPUT_SHOOT;
    if (input->scope) packet->flags |= NET_INPUT_SCOPE;
    if (input->jump) packet->flags |= NET_INPUT_JUMP;
    if (input->crouch) packet->flags |= NET_INPUT_CROUCH;
    if (input->reload) packet->flags |= NET_INPUT_RELOAD;
    if (input->reset == 1) packet->flags |= NET_INPUT_RESET_START; // WP-B input[13]
    if (input->reset == 2) packet->flags |= NET_INPUT_RESET_LAST;
    if (input->interact_held) packet->flags |= NET_INPUT_INTERACT; // WP-B input[14]
}

void net_packet_to_input(Input *input, const NetInputPacket *packet) {
    memset(input, 0, sizeof(*input));

    input->seq = packet->seq;
    input->move_dir = packet->move_dir;
    input->delta = packet->delta;
    input->x_dir = packet->x_dir;
    input->y_dir = packet->y_dir;
    input->swap = packet->swap;
    input->slowdown = (packet->slowdown > 10 ? 10 : packet->slowdown) / 10.0f;
    input->scroll = (signed char) (packet->scroll > 0 ? 1 : packet->scroll < 0 ? -1 : 0);
    if (packet->scroll >= 2) { // KrunkNative Object Manager slot pick (net_packet_from_input)
        input->select_slot = (unsigned char) (packet->scroll - 1);
        input->scroll = 0;
    }
    input->shoot = !!(packet->flags & NET_INPUT_SHOOT);
    input->scope = !!(packet->flags & NET_INPUT_SCOPE);
    input->jump = !!(packet->flags & NET_INPUT_JUMP);
    input->crouch = !!(packet->flags & NET_INPUT_CROUCH);
    input->reload = !!(packet->flags & NET_INPUT_RELOAD);
    input->reset = packet->flags & NET_INPUT_RESET_LAST ? 2 : packet->flags & NET_INPUT_RESET_START ? 1 : 0; // WP-B input[13]
    input->interact_held = !!(packet->flags & NET_INPUT_INTERACT);                                        // WP-B input[14]
}

void net_packet_from_player(NetStatePacket *packet, const Player *player) {
    packet->uid = player->uid;
    packet->ack_seq = player->input_seq;
    packet->active = player->active;
    packet->loadout_index = player->loadout_index;
    packet->position[0] = player->position.x;
    packet->position[1] = player->position.y;
    packet->position[2] = player->position.z;
    packet->velocity[0] = player->velocity.x;
    packet->velocity[1] = player->velocity.y;
    packet->velocity[2] = player->velocity.z;
    // Sent wrapped (controls.js xDr range): a receiver interpolates between consecutive states.
    packet->direction[0] = player_clamp_pitch(player->direction.x);
    packet->direction[1] = player_wrap_yaw(player->direction.y);
    packet->health = player->health;
    packet->aim_val = player->aim_val;
    packet->crouch_val = player->crouch_val;
}

// packetHandlers.ts io_tp: the fields its buildLPacket ack overrides (position, zero
// velocities, onGround, no didJump/didJumpW/onWall/onLadder, crouchVal 0, slideTimer 0,
// canSlide, no onTerrain, xVelC/zVelC 0); everything else is kept.
void net_player_teleport(Player *player, const float x, const float y, const float z) {
    player->recon_snap = 1; // the authority follows with a NET_RECON_SEQ_ALL "l" (io_tp lastSNBySocketId)
    player->position = (vec3) {x, y, z};
    player->last_position = player->position;
    player->velocity = (vec3) {0.0f, 0.0f, 0.0f};
    player->on_ground = 1;
    player->did_jump = 0;
    player->did_wall_jump = 0;
    player->on_wall = 0;
    player->on_ladder = 0;
    player->crouch_val = 0.0f;
    player->height = game_constants.player_height; // crouchVal 0
    player->slide_timer = 0.0f;
    player->can_slide = 1;
    player->on_terrain = 0;
    player->x_vel_cylinder = 0.0f;
    player->z_vel_cylinder = 0.0f;
}

void net_packet_apply_player(Player *player, const NetStatePacket *packet) {
    player->active = packet->active;
    player->input_seq = packet->ack_seq;
    player->position.x = packet->position[0];
    player->position.y = packet->position[1];
    player->position.z = packet->position[2];
    player->velocity.x = packet->velocity[0];
    player->velocity.y = packet->velocity[1];
    player->velocity.z = packet->velocity[2];
    player->direction.x = player_clamp_pitch(packet->direction[0]);
    player->direction.y = player_wrap_yaw(packet->direction[1]);
    player->health = packet->health;
    player->aim_val = packet->aim_val;
    player->crouch_val = packet->crouch_val;

    if (packet->loadout_index >= 0 && packet->loadout_index < player->loadout_size && packet->loadout_index != player->loadout_index) {
        player_swap_weapon(player, packet->loadout_index, 0, 1, 1);
    }
}

// packetHandlers.ts buildLPacket: `player` right after the tick that ran input `ack_seq`.
void net_packet_from_recon(NetReconPacket *packet, const Player *player, const int32_t ack_seq) {
    memset(packet, 0, sizeof(*packet));
    packet->ack_seq = ack_seq;
    packet->position[0] = player->position.x;
    packet->position[1] = player->position.y;
    packet->position[2] = player->position.z;
    packet->velocity[0] = player->velocity.x;
    packet->velocity[1] = player->velocity.y;
    packet->velocity[2] = player->velocity.z;
    packet->yaw = player_wrap_yaw(player->direction.y);
    packet->aim_val = player->aim_val;
    packet->crouch_val = player->crouch_val;
    packet->loadout_index = player->loadout_index;
    packet->slide_timer = player->slide_timer;
    packet->x_vel_cylinder = player->x_vel_cylinder;
    packet->z_vel_cylinder = player->z_vel_cylinder;
    packet->on_wall = (uint8_t) player->on_wall;
    if (player->on_ground) packet->flags |= NET_RECON_ON_GROUND;
    if (player->did_jump) packet->flags |= NET_RECON_DID_JUMP;
    if (player->did_wall_jump) packet->flags |= NET_RECON_DID_JUMP_W;
    if (player->on_ladder) packet->flags |= NET_RECON_ON_LADDER;
    if (player->can_slide) packet->flags |= NET_RECON_CAN_SLIDE;
    if (player->on_terrain) packet->flags |= NET_RECON_ON_TERRAIN;
    if (player->noclip) packet->flags |= NET_RECON_NOCLIP;
    packet->wall_jump_count = (uint16_t) (player->wall_jump_count <= 0 ? 0 : player->wall_jump_count >= UINT16_MAX ? UINT16_MAX : player->wall_jump_count);
    packet->can_slide_timer = player->can_slide_timer;
    // slideTimer as the physics counts it (player.c movement_slide_ms: slide_ms unless slide_timer moved)
    packet->slide_ms = (float) (player->slide_ms / 1000.0) == player->slide_timer ? player->slide_ms : (double) player->slide_timer * 1000.0;
    // [23] player.grapple || 0
    packet->grapple_active = (uint8_t) !!player->grapple.active;
    if (player->grapple.active) {
        packet->grapple[0] = player->grapple.point.x;
        packet->grapple[1] = player->grapple.point.y;
        packet->grapple[2] = player->grapple.point.z;
        packet->grapple[3] = player->grapple.range;
    }
}

// index.js reconcilePlayer(l, true) up to the replay: every field of the "l" packet is
// written onto our own player; loadout_index < 0 keeps the slot (weaponIndex null).
void net_packet_apply_recon(Player *player, const NetReconPacket *packet) {
    if (!isfinite(packet->position[0]) || !isfinite(packet->position[1]) || !isfinite(packet->position[2])) return;
    player->position = (vec3) {packet->position[0], packet->position[1], packet->position[2]};
    player->velocity = (vec3) {packet->velocity[0], packet->velocity[1], packet->velocity[2]};
    player->direction.y = player_wrap_yaw(packet->yaw);
    player->aim_val = packet->aim_val;
    player->crouch_val = packet->crouch_val;
    player_update_height(player);
    player->slide_timer = packet->slide_timer;
    // v7.1.4 io_syncPlayer [12] wallJumpC, [24] canSlideT; the exact slide ms when it is this
    // slide_timer (a V1 RECON read through net_packet_read_recon carries slide_timer * 1000)
    player->wall_jump_count = packet->wall_jump_count;
    player->can_slide_timer = isfinite(packet->can_slide_timer) ? packet->can_slide_timer : 0.0f;
    if (isfinite(packet->slide_ms) && (float) (packet->slide_ms / 1000.0) == packet->slide_timer) player->slide_ms = packet->slide_ms;
    player->x_vel_cylinder = packet->x_vel_cylinder;
    player->z_vel_cylinder = packet->z_vel_cylinder;
    player->on_wall = packet->on_wall & 7;
    player->on_ground = !!(packet->flags & NET_RECON_ON_GROUND);
    player->did_jump = !!(packet->flags & NET_RECON_DID_JUMP);
    player->did_wall_jump = !!(packet->flags & NET_RECON_DID_JUMP_W);
    player->on_ladder = !!(packet->flags & NET_RECON_ON_LADDER);
    player->can_slide = !!(packet->flags & NET_RECON_CAN_SLIDE);
    player->on_terrain = !!(packet->flags & NET_RECON_ON_TERRAIN);
    player->noclip = !!(packet->flags & NET_RECON_NOCLIP);
    // [23] tmpPlayer.grapple = grapple || null (a RECON without it keeps the predicted one)
    if (packet->grapple_active != NET_RECON_GRAPPLE_KEEP) {
        player->grapple.active = packet->grapple_active == 1;
        player->grapple.point = (vec3) {packet->grapple[0], packet->grapple[1], packet->grapple[2]};
        player->grapple.range = packet->grapple[3];
        if (!player->grapple.active) memset(&player->grapple, 0, sizeof(player->grapple));
    }
    // players.reload(player, 0, false, weaponIndex, undefined, true): swapWeapon's noEndGrapple
    if (packet->loadout_index >= 0 && packet->loadout_index < player->loadout_size && packet->loadout_index != player->loadout_index) {
        player_swap_weapon(player, packet->loadout_index, 0, 0, 1);
    }
}

// index.js reconcilePlayer(l, true): snap to the server state, then drop the acked inputs
// (seq <= ack_seq, or all of them once NET_RECON_PENDING_MAX are pending) and replay the rest
// through procInputs(input, Game, recon = true). No smoothing and no error threshold: v3.1.1
// snaps every time and relies on the replay landing where the prediction already was.
// `pending` is seq-ascending and compacted in place; returns the new count.
size_t net_reconcile(Player *player, const NetReconPacket *packet, Input *pending, size_t count, const int move_lock) {
    net_packet_apply_recon(player, packet);

    const int drop_all = count >= NET_RECON_PENDING_MAX;
    size_t kept = 0;
    for (size_t i = 0; i < count; i++) {
        if (drop_all || pending[i].seq <= packet->ack_seq) continue;
        pending[kept++] = pending[i];
    }
    for (size_t i = 0; i < kept && player->active; i++) player_proc_input(player, &pending[i], 1, move_lock);
    return kept;
}

// After the client's tick ran its newest input: that input's physPow knockback becomes its
// velObj (players.js shoot: if (!IO) a1.velObj = {...}) for later reconciliation replays.
// The same for tryGrapple's input.grapObj (if (!serverIO) a1.grapObj = {x, y, z, r}).
void net_recon_take_vel_obj(Player *player, Input *pending, const size_t count) {
    if (!player->has_shot_vel_obj && !player->has_shot_grap_obj) return;
    const int vel = player->has_shot_vel_obj, grap = player->has_shot_grap_obj;
    player->has_shot_vel_obj = 0;
    player->has_shot_grap_obj = 0;
    for (size_t i = count; i-- > 0;) {
        if (pending[i].seq != player->input_seq) continue;
        if (vel) {
            pending[i].vel_obj = player->shot_vel_obj;
            pending[i].has_vel_obj = 1;
        }
        if (grap) {
            pending[i].grap_obj = player->shot_grap_obj;
            pending[i].has_grap_obj = 1;
        }
        return;
    }
}

// gameserver.ts "l" schedule for one playing connection: a server-driven snap (recon_snap,
// a noclip flip) goes out at once, otherwise every 1 / NET_RECON_SEND_HZ s once procInputs
// drained an input this life. 1 = send now (and the schedule / snap state is advanced).
int net_recon_due(Player *player, const double now, double *next_at) {
    if (!player || !player->active) return 0;
    const int snap = player->recon_snap || player->noclip != player->recon_noclip;
    if (!snap && (now < *next_at || player->input_seq < 0)) return 0;
    *next_at = now + 1.0 / NET_RECON_SEND_HZ;
    player->recon_snap = 0;
    player->recon_noclip = player->noclip;
    return 1;
}

int net_packet_read_input_batch(NetInputBatchPacket *batch, const NetPacket *packet) {
    if (packet->length < 1) return 0;
    const uint8_t count = packet->payload[0];
    if (!count || count > NET_INPUT_BATCH_MAX || packet->length != 1 + count * sizeof(NetInputPacket)) return 0;
    batch->count = count;
    memcpy(batch->frames, packet->payload + 1, count * sizeof(NetInputPacket));
    return 1;
}

// Krunker v3.1.1 chat input: the page caps #chatInput at 70 characters and the
// server drops blank lines. Copies at most NET_CHAT_MAX_CHARS code points of `in`
// (stopping at a NUL), replaces control characters with spaces, drops invalid
// UTF-8 bytes and trims the ends. Returns 1 when something is left.
int net_chat_sanitize(char *out, const size_t size, const char *in, const size_t length) {
    if (!out || !size) return 0;
    out[0] = 0;
    if (!in) return 0;

    size_t written = 0;
    int chars = 0;
    size_t i = 0;

    while (i < length && in[i] && chars < NET_CHAT_MAX_CHARS) {
        const unsigned char c = (unsigned char) in[i];
        size_t seq = c < 0x80 ? 1 : (c & 0xe0) == 0xc0 ? 2 : (c & 0xf0) == 0xe0 ? 3 : (c & 0xf8) == 0xf0 ? 4 : 0;

        if (!seq || i + seq > length) { i++; continue; }

        int valid = 1;
        for (size_t k = 1; k < seq; k++) {
            if (((unsigned char) in[i + k] & 0xc0) != 0x80) valid = 0;
        }
        if (!valid) { i++; continue; }
        if (written + seq + 1 > size) break;

        if (seq == 1) out[written++] = (char) (c < 0x20 || c == 0x7f ? ' ' : c);
        else for (size_t k = 0; k < seq; k++) out[written++] = in[i + k];

        i += seq;
        chars++;
    }

    out[written] = 0;

    size_t start = 0;
    while (out[start] == ' ') start++;
    while (written > start && out[written - 1] == ' ') out[--written] = 0;
    if (start) memmove(out, out + start, written - start + 1);

    return out[0] != 0;
}

// io_ct burst limit: a window opens on the first message and allows
// NET_CHAT_BURST_MAX messages within NET_CHAT_BURST_WINDOW seconds.
int net_chat_allow(NetChatLimiter *limiter, const float now) {
    if (!limiter->count || now - limiter->window_start >= NET_CHAT_BURST_WINDOW) {
        limiter->window_start = now;
        limiter->count = 1;
    } else {
        limiter->count++;
    }

    return limiter->count <= NET_CHAT_BURST_MAX;
}

void net_packet_chat(NetChatPacket *packet, const int uid, const NetChatKind kind, const char *name, const char *text) {
    memset(packet, 0, sizeof(*packet));
    packet->uid = uid;
    packet->kind = (uint8_t) kind;
    if (name) strncpy(packet->name, name, sizeof(packet->name) - 1);
    if (text) strncpy(packet->text, text, sizeof(packet->text) - 1);
}

// config.serverMessages[type] with "$" replaced by the player's name (broadcastServerMessage in the v3.1.1 server).
void net_format_server_line(char *out, const size_t size, const char *template, const char *name) {
    if (!out || !size) return;
    if (!template) template = "";
    const char *dollar = strchr(template, '$');
    if (dollar) snprintf(out, size, "%.*s%s%s", (int) (dollar - template), template, name && name[0] ? name : "unknown", dollar + 1);
    else snprintf(out, size, "%s", template);
}

// NET_EXT_KN_SERVER_NAMES composed: a single pass over the template, so a name is copied in and never scanned.
void net_format_server_names(char *out, const size_t size, const char *template, const char *const *names, const int count) {
    if (!out || !size) return;
    size_t len = 0;
    int next = 0;
    for (const char *p = template ? template : ""; *p && len + 1 < size; p++) {
        if (*p == '$' && next < count) {
            const char *name = names && names[next] && names[next][0] ? names[next] : "unknown";
            next++;
            for (const char *s = name; *s && len + 1 < size; s++) out[len++] = *s;
        } else {
            out[len++] = *p;
        }
    }
    out[len] = 0;
}
// net_ext.h spells NetServerLinePacket's sizes out (it does not include shared.h): keep them the shared constants'
_Static_assert(sizeof(((NetServerLinePacket *) 0)->name) == PLAYER_NAME_SIZE, "NetServerLinePacket.name is PLAYER_NAME_SIZE");
_Static_assert(sizeof(((NetServerLinePacket *) 0)->text) == NET_CHAT_TEXT_SIZE, "NetServerLinePacket.text is NET_CHAT_TEXT_SIZE");
_Static_assert(sizeof(((NetServerNameEntry *) 0)->name) == PLAYER_NAME_SIZE, "NetServerNameEntry.name is PLAYER_NAME_SIZE");
_Static_assert(sizeof(((NetServerNamesPacket *) 0)->text) == NET_SERVER_NAMES_TEXT, "NetServerNamesPacket.text is NET_SERVER_NAMES_TEXT");

// io_po: one RTT sample folded into the published ping (EWMA, alpha 0.35).
int net_ping_update(const int previous_ms, const float rtt_seconds) {
    int sample = (int) (rtt_seconds * 1000.0f + 0.5f);
    if (sample < 0) sample = 0;
    if (sample > NET_PING_MAX_MS) sample = NET_PING_MAX_MS;
    if (previous_ms < 0) return sample;

    const float smoothed = (float) previous_ms + (float) (sample - previous_ms) * NET_PING_EWMA_ALPHA;
    return (int) (smoothed + (smoothed >= 0.0f ? 0.5f : -0.5f));
}

static uint32_t net_fnv1a(const char *data, const size_t length) {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < length; i++) {
        hash ^= (uint8_t) data[i];
        hash *= 16777619u;
    }
    return hash ? hash : 1u;
}

uint32_t net_map_hash(const cJSON *map, char **json_out, size_t *size_out) {
    // Maps are immutable once loaded: remember the hash and size per object.
    static struct { const cJSON *map; uint32_t hash; size_t size; } cache[64];
    static size_t cache_next;

    if (json_out) *json_out = NULL;
    if (size_out) *size_out = 0;
    if (!map) return 0;

    if (!json_out) {
        for (size_t i = 0; i < sizeof(cache) / sizeof(cache[0]); i++) {
            if (cache[i].map != map) continue;
            if (size_out) *size_out = cache[i].size;
            return cache[i].hash;
        }
    }

    // KrunkScript 2.1 (GJS:625-628): the server script never leaves the server. Clients hash
    // and receive the map without scripts.server (both sides strip it, so hashes agree).
    char *json;
    const cJSON *scripts = cJSON_GetObjectItemCaseSensitive(map, "scripts");
    if (cJSON_IsObject(scripts) && cJSON_GetObjectItemCaseSensitive(scripts, "server")) {
        cJSON *copy = cJSON_Duplicate(map, 1);
        cJSON_DeleteItemFromObjectCaseSensitive(cJSON_GetObjectItemCaseSensitive(copy, "scripts"), "server");
        json = copy ? cJSON_PrintUnformatted(copy) : NULL;
        cJSON_Delete(copy);
    } else {
        json = cJSON_PrintUnformatted(map);
    }
    if (!json) return 0;

    const size_t size = strlen(json);
    const uint32_t hash = net_fnv1a(json, size);

    cache[cache_next].map = map;
    cache[cache_next].hash = hash;
    cache[cache_next].size = size;
    cache_next = (cache_next + 1) % (sizeof(cache) / sizeof(cache[0]));

    if (size_out) *size_out = size;
    if (json_out) *json_out = json;
    else cJSON_free(json);

    return hash;
}

int net_peer_has_wpc(const int minor, const int patch) {
    return minor > NET_PROTOCOL_MINOR_V7 || (minor == NET_PROTOCOL_MINOR_V7 && patch >= 1);
}

int net_packet_read_hello(NetHelloPacket *hello, const NetPacket *packet) {
    if (packet->length < offsetof(NetHelloPacket, patch)) return 0;
    memset(hello, 0, sizeof(*hello));
    memcpy(hello, packet->payload, packet->length < sizeof(*hello) ? packet->length : sizeof(*hello));
    return 1;
}

int net_packet_read_init(NetInitPacket *init, const NetPacket *packet) {
    if (packet->length < NET_INIT_PACKET_V1_SIZE) return 0; // the ruleset byte is optional
    memset(init, 0, sizeof(*init));
    memcpy(init, packet->payload, packet->length < sizeof(*init) ? packet->length : sizeof(*init));
    return 1;
}

NetHelloPacket net_hello_make(void) {
    // The low nibble is the ruleset mask (v10.0.2); the high nibble is this process's product flavor
    // (shared.h). FLAVOR_BASE is a zero nibble, so a base build's HELLO is byte-identical (0x04).
    return (NetHelloPacket) {NET_PROTOCOL_MAJOR, NET_PROTOCOL_MINOR, NET_PROTOCOL_PATCH,
                             NET_WIRE_RULESET_BYTE(NET_HELLO_RULESETS, flavor_get())};
}

int net_packet_read_recon(NetReconPacket *recon, const NetPacket *packet, const Player *keep) {
    if (packet->length < NET_RECON_PACKET_V1_SIZE || (packet->length > NET_RECON_PACKET_V1_SIZE && packet->length < NET_RECON_PACKET_V2_SIZE)) return 0;
    memset(recon, 0, sizeof(*recon));
    memcpy(recon, packet->payload, packet->length < sizeof(*recon) ? packet->length : sizeof(*recon));
    if (packet->length < NET_RECON_PACKET_V2_SIZE) { // a 1.3.0 server / net-bridge: these stay as predicted
        recon->wall_jump_count = (uint16_t) (keep && keep->wall_jump_count > 0 ? MIN(keep->wall_jump_count, UINT16_MAX) : 0);
        recon->can_slide_timer = keep ? keep->can_slide_timer : 0.0f;
        recon->slide_ms = (double) recon->slide_timer * 1000.0;
    }
    if (packet->length < NET_RECON_PACKET_V3_SIZE) { // no grapple sent: the predicted one stays
        recon->grapple_active = NET_RECON_GRAPPLE_KEEP;
        memset(recon->grapple, 0, sizeof(recon->grapple));
    } else if (recon->grapple_active > 1 || !isfinite(recon->grapple[0]) || !isfinite(recon->grapple[1]) || !isfinite(recon->grapple[2]) ||
               !isfinite(recon->grapple[3])) {
        recon->grapple_active = 0;
    }
    return 1;
}

int net_rate_hz(const int index) {
    static const int rates[NET_RATE_COUNT] = {30, 60, 120, 144, 240, 360};
    return rates[index >= 0 && index < NET_RATE_COUNT ? index : NET_RATE_DEFAULT_INDEX];
}

// index.js gameLoop: inputFrameDelta += gameTimeDiff; shouldSampleInput =
// roundToClos(inputFrameDelta, 1000 / netRate) >= 1000 / netRate (p * Math.round(x / p) >= p, so
// x / p >= 0.5); the frame then takes Math.round(inputFrameDelta * deltaMlt) and inputFrameDelta = 0.
float net_input_rate_take(float *accum, const float frame_delta, const int rate_hz) {
    if (isfinite(frame_delta) && frame_delta > 0.0f) *accum += frame_delta;
    const double period_ms = 1000.0 / (double) (rate_hz > 0 ? rate_hz : net_rate_hz(NET_RATE_DEFAULT_INDEX));
    const double accum_ms = (double) *accum * 1000.0;
    if (period_ms * floor(accum_ms / period_ms + 0.5) < period_ms) return -1.0f;
    const float delta = *accum;
    *accum = 0.0f;
    return delta;
}

int net_packet_read_input(NetInputPacket *input, const NetPacket *packet) {
    if (packet->length != sizeof(NetInputPacket) && packet->length != NET_INPUT_PACKET_V2_SIZE &&
        packet->length != NET_INPUT_PACKET_V1_SIZE) return 0;

    input->slowdown = 0;
    input->scroll = 0;
    memcpy(input, packet->payload, packet->length);
    return 1;
}

int net_packet_read_ent(NetEntPacket *ent, const NetPacket *packet) {
    // 4 / 8 bytes, or cosmetics followed by the optional spectate and slid_cont bytes (36..38),
    // or with spray_index (42), with the weapon skins (54), with lag_comp (58), with
    // attachIndex / challMode / anonMode (64, WP-C), or all of it with the v7 cosmetic slots and
    // showBadges (101, WP-F). Longer ENTs (a newer peer) read the V9 prefix; a V8-sized one (a 1.3.1
    // peer before WP-F) has no v7 slots.
    if (packet->length != NET_ENT_PACKET_V2_SIZE && packet->length != NET_ENT_PACKET_MIN_SIZE &&
        packet->length != NET_ENT_PACKET_V5_SIZE && packet->length != NET_ENT_PACKET_V6_SIZE &&
        packet->length != NET_ENT_PACKET_V7_SIZE && packet->length < NET_ENT_PACKET_V8_SIZE &&
        (packet->length < NET_ENT_PACKET_V3_SIZE || packet->length > NET_ENT_PACKET_V4_SIZE)) return 0;

    ent->secondary_index = -1;
    ent->cosmetics = PLAYER_COSMETICS_NONE;
    ent->spectate = 0;
    ent->slid_cont = 1; // absent: settings.js slidCont default (true)
    ent->spray_index = 0; // absent: selectedSprayIndex default (0, "Krunker")
    ent->skins = PLAYER_SKINS_NONE; // absent: no weapon / melee skins
    ent->lag_comp = 1.0f; // absent: settings.js lagCompensation default (1)
    ent->attach_index = DEFAULT_ATTACHMENT_INDEX; // absent: parseEnterData integer(data[12], 0)
    ent->chall_mode = 0;
    ent->anon_mode = 0;
    ent->cosmetics_v7 = PLAYER_COSMETICS_V7_NONE;
    ent->show_badges = PLAYER_SHOW_BADGES_ALL;
    // a 1.3.1 ENT cut inside the WP-F tail (V8 < length < V9) carries none of it
    const size_t length = packet->length > NET_ENT_PACKET_V8_SIZE && packet->length < NET_ENT_PACKET_V9_SIZE ? NET_ENT_PACKET_V8_SIZE : packet->length;
    memcpy(ent, packet->payload, length < sizeof(*ent) ? length : sizeof(*ent));
    ent->show_badges &= PLAYER_SHOW_BADGES_ALL;
    ent->spectate = ent->spectate ? 1 : 0;
    ent->slid_cont = ent->slid_cont ? 1 : 0;
    ent->chall_mode = ent->chall_mode ? 1 : 0;
    ent->anon_mode = ent->anon_mode ? 1 : 0;
    return 1;
}

uint16_t net_ent_size_for(const int major, const int minor, const int patch) {
    // 1.3.1 readers prefix-read longer ENTs, so the WP-F tail (V9) goes to every 1.3.1+ server
    if (major > NET_PROTOCOL_MAJOR || (major == NET_PROTOCOL_MAJOR && net_peer_has_wpc(minor, patch))) return (uint16_t) NET_ENT_PACKET_V9_SIZE;
    if (major == NET_PROTOCOL_MAJOR && minor >= 2) return (uint16_t) NET_ENT_PACKET_V7_SIZE;
    if (major == NET_PROTOCOL_MAJOR && minor == 1) return (uint16_t) NET_ENT_PACKET_V6_SIZE;
    return (uint16_t) NET_ENT_PACKET_V4_SIZE;
}

int net_packet_read_spawn(NetSpawnPacket *spawn, const NetPacket *packet) {
    // 78 (no cosmetics), V2 (cosmetics), V3 (+ attachIndex / headMlt, WP-C), V4 (+ the v7 cosmetic
    // slots, WP-F) or longer (V4 prefix). V3 < length < V4 (cut inside the WP-F tail) reads as V3.
    if (packet->length != NET_SPAWN_PACKET_V1_SIZE && packet->length != NET_SPAWN_PACKET_V2_SIZE &&
        packet->length < NET_SPAWN_PACKET_V3_SIZE) return 0;

    spawn->cosmetics = PLAYER_COSMETICS_NONE;
    spawn->attach_index = DEFAULT_ATTACHMENT_INDEX;
    spawn->head_mlt = 1.0f;
    spawn->cosmetics_v7 = PLAYER_COSMETICS_V7_NONE;
    const size_t length = packet->length > NET_SPAWN_PACKET_V3_SIZE && packet->length < NET_SPAWN_PACKET_V4_SIZE ? NET_SPAWN_PACKET_V3_SIZE : packet->length;
    memcpy(spawn, packet->payload, length < sizeof(*spawn) ? length : sizeof(*spawn));
    if (!isfinite(spawn->head_mlt) || spawn->head_mlt <= 0.0f) spawn->head_mlt = 1.0f;
    return 1;
}

#ifndef KRUNKNATIVE_SERVER

void player_send_event() { /* SHIM */ }
void game_broadcast() { /* SHIM */ }

#endif

// server.cfg billboard_image / NET_PACKET_BILLBOARD_IMAGE (shared/include/shared.h)
int billboard_image_path_ok(const char *path) {
    if (!path || !path[0] || path[0] == '/') return 0;
    const size_t len = strlen(path);
    if (len >= NET_BILLBOARD_IMAGE_MAX || strstr(path, "..") || strstr(path, "//")) return 0;
    for (const char *p = path; *p; p++) {
        const char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' || c == '/')) return 0;
    }
    static const char *const extensions[] = {".png", ".jpg", ".jpeg"};
    for (size_t i = 0; i < sizeof(extensions) / sizeof(extensions[0]); i++) {
        const size_t n = strlen(extensions[i]);
        if (len <= n) continue;
        size_t k = 0;
        while (k < n && (path[len - n + k] | 0x20) == extensions[i][k]) k++; // ASCII case-insensitive
        if (k == n) return 1;
    }
    return 0;
}
