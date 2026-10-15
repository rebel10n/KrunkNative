// Prediction sync against an official Krunker server, deterministic and without sockets.
//   cmake-build-debug/krunker_sync_test
//
// The "official server" here is a faithful double-precision reimplementation of the live bundle's (game.deob.js, Krunker 10.0.2r1)
// procInputs (players.Uiblms, game.deob.js:169596-170201; identical to 10.0.2's Wjm9aj after
// identifier renaming) for what a flat floor exercises: look, aim (aimVal), crouch, ground / air
// acceleration, strafeSpd, decel, slide, jump, gravity, the velocity step and the floor box
// collision with resetStep's slide start. It decodes our "q" windows the way the server reads
// them (the official client's encoder, :221755-221830) and answers with "l" SYNC_PLAYER frames
// at the live bundle's layout (KRUNKER_SYNC_*).
//
// The client is ours: shared physics predicting every input frame (player_queue_input +
// game_tick, as main.c), the v10 "q" window (recon.c), krunker_link translating both ways, and
// net_reconcile on every RECON. Measured: the correction each "l" applies (0 = the client runs the
// server's physics on the server's inputs). The lobby's settings reach the client through init
// (krunker_link emit_lobby_config -> GAME_CONFIG / NETWORK_DATA), as on a live server.
//
// Negative controls prove the measurement sees each bug this test was written for: the movement
// key sent unmapped (our move_dir as Krunker's movDirs index: every key 45 degrees off), aimVal
// not inverted (Krunker 1 = hip, ours 1 = aimed), and the lobby config never applied.
#include <krunker_link.h>
#include <game_config.h>
#include <net_ext.h>
#include <cJSON.h>
#include <math.h>
#include <shared.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures, g_checks;
#define CHECK(cond, ...) do { g_checks++; if (!(cond)) { g_failures++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define SYNC_TOLERANCE 0.02f // a correction above this is a snap the player would see (units)

// krunker_link's cheat-check fallback (the sync test never sends "cc")
int krunker_cheat_answer(const char *nonce, const char *ciphertext_b64, KrunkerCcr *out) {
    (void) nonce;
    (void) ciphertext_b64;
    memset(out, 0, sizeof(*out));
    return -1;
}
void krunker_ccr_free(KrunkerCcr *out) {
    free(out->nonce);
    free(out->payload_b64);
    out->nonce = out->payload_b64 = NULL;
}

// ---------------------------------------------------------------- the official server (reference)

static double js_round3(const double v) { return floor(v * 1000.0 + 0.5) / 1000.0; } // Number.prototype.round(3)
static double crop(const double v, const double min) { return v <= min && v >= -min ? 0.0 : v; } // :23980
static double clampd(const double v, const double lo, const double hi) { return v < lo ? lo : v > hi ? hi : v; }

typedef struct {
    // player state (the fields "l" carries, plus what procInputs keeps between frames)
    double x, y, z, ox, oy, oz, vx, vy, vz, x_dire, y_dire;
    double aim, crouch, height, slide, can_slide_t, jump_cd;
    int on_ground, was_on_ground, did_jump, did_jump_w, can_slide, on_terrain, wall_jump_c, input_sn;
    // lobby config (applyConfig) and Triggerman / Assault Rifle (classConfig defaults :101976, AR)
    double grav_mlt, jump_mlt, strafe_spd, slid_spd, delta_mlt;
    double speed, gr_a, ar_a, crch_spd, gr_dec, air_dec, sl_a, sl_t, spd_mlt, aim_spd;
    // "q" decoder state (the server's view of the official client's compression)
    double last_vals[15];
    double prev_angles[2 + 2 * NET_INPUT_BATCH_MAX];
    int prev_angle_count;
    // negative control: read input[4] as our raw move_dir (the link before the movDirs fix)
    int control_raw_move_dir;
    int dead; // between our death and our respawn: the server ignores our "q" frames (io_q)
    int last_isn, isn_gaps; // the isn stream as decoded: a frame that is not the last + 1 (-1: none yet)
} Server;

static void server_defaults(Server *s) {
    memset(s, 0, sizeof(*s));
    s->grav_mlt = s->jump_mlt = s->slid_spd = s->delta_mlt = 1.0;
    s->strafe_spd = 1.2;                       // strafeSpd def
    s->speed = 1.05;                           // Triggerman speed
    s->gr_a = 0.042, s->ar_a = 0.0047, s->crch_spd = 0.3, s->gr_dec = 0.99, s->air_dec = 0.9996; // class option defs
    s->sl_a = s->sl_t = 1.0;
    s->spd_mlt = 0.95, s->aim_spd = 130.0;     // Assault Rifle spdMlt, aimSpd (ms)
    s->aim = 1.0;                              // hip
    s->last_isn = -1;
    s->height = 11.0;
    s->on_ground = 1;
    s->last_vals[4] = -1;
}

// players.add / player.init for a new life at the spawn (flat floor, spawn at y 0): the physics
// state from rest (onGround true, no velocity, inputSN 0). The "q" decoder keeps the last decoded
// frame (last_vals: the keys and flags the server still holds). That is the assumption under test:
// a window the server dropped while we were dead is never decoded, so the server's held inputs can
// be stale when the new life starts. The official client answers that at its "start" (see below).
static void server_respawn(Server *s) {
    s->x = s->y = s->z = s->ox = s->oy = s->oz = 0.0;
    s->vx = s->vy = s->vz = 0.0;
    s->on_ground = 1, s->was_on_ground = 0, s->did_jump = s->did_jump_w = 0, s->on_terrain = 0;
    s->can_slide = 1, s->can_slide_t = 0, s->slide = 0, s->jump_cd = 0, s->wall_jump_c = 0;
    s->aim = 1.0, s->crouch = 0.0, s->height = 11.0;
    s->input_sn = 0;
    s->dead = 0;
}

// players.jump (:172028) with config.autoJump off (tryJump :172066)
static void server_jump(Server *s) {
    s->did_jump = s->did_jump_w = 1;
    s->on_terrain = 0;
    const double up = 0.072 * s->jump_mlt, push = 0.1 * s->jump_mlt;
    s->vy += (up - up * (0.25 * s->crouch)) * s->spd_mlt * (s->aim ? 1.0 : 0.85);
    const double v = sqrt(s->vx * s->vx + s->vz * s->vz);
    s->vx -= push * v * sin(s->x_dire);
    s->vz -= push * v * cos(s->x_dire);
}

// players.resetStep (:171977): a landing starts a slide when crouched
static void server_reset_step(Server *s) {
    if (s->was_on_ground || s->oy <= s->y) return;
    s->wall_jump_c = 0;
    if (s->crouch && s->can_slide && s->can_slide_t <= 0) { // config.canSlide, no realMov / noSliding
        s->can_slide = 0;
        s->can_slide_t = 100;
        s->slide = 350.0 * 1.0 * s->sl_t * s->crouch;       // * (slidTime || 1)
        const double m = 1.2 * s->sl_a * s->slid_spd;       // (manager.onTerrain undefined ? 1.2) * slA * slidSpd
        s->vx *= m;
        s->vz *= m;
    }
}

// One input frame through Uiblms (no move lock, flat floor with its top at y 0).
static void server_proc(Server *s, const double in[15]) {
    const double base = 1.0 * s->delta_mlt;
    double d = fmax(base, fmin(in[1], 33.0)); // Math.round(1000 / 30); deltaDiv 1
    if (!(d > 0) || d < base) d = base;
    s->input_sn = (int) in[0];
    const double pitch = in[2] / 1000.0, yaw = in[3] / 1000.0;
    const int ads = in[6] != 0, jump = in[7] != 0, crouch = in[9] != 0;
    int idx = (int) in[4];
    if (s->control_raw_move_dir && idx >= 0) idx = (idx + 7) % 8; // what the server saw before the fix
    const int moving = idx >= 0 && idx < 8;
    const double move_dir = -M_PI + (idx + 1) * M_PI / 4.0; // movDirs (:16415)

    // movement[0].dir (trnSp 1): the look, round(3), pitch clamped
    s->x_dire = js_round3(yaw);
    s->y_dire = clampd(js_round3(pitch), -M_PI / 2.0, M_PI / 2.0);
    s->ox = s->x, s->oy = s->y, s->oz = s->z;

    // aim (weapon.zoom, canThrow / reload / swap clear)
    if (ads && s->aim > 0) {
        s->aim -= 1.0 / (s->aim_spd / d);
        if (getenv("SYNC_DEBUG") && s->aim > 0 && s->aim < 1e-6) printf("    server aim residue %.3g at isn %d\n", s->aim, (int) in[0]);
        if (getenv("SYNC_DEBUG") && s->aim < 0 && s->aim > -1e-6) printf("    server aim undershoot %.3g at isn %d\n", s->aim, (int) in[0]);
        if (s->aim <= 0) s->aim = 0;
    } else if (!ads && s->aim < 1) {
        s->aim += 1.0 / (s->aim_spd / d);
        if (s->aim >= 1) s->aim = 1;
    }

    // crouch (canCrch, no headClipFix stop), updateHeight
    if (crouch && s->crouch < 1) {
        s->crouch += 0.007 * d;
        if (s->crouch >= 1) s->crouch = 1;
        if (!s->on_ground) s->y += 0.007 * d;
    } else if (!crouch && s->crouch > 0) {
        s->crouch -= 0.007 * d;
        if (s->crouch <= 0) s->crouch = 0;
        if (!s->on_ground) s->y -= 0.007 * d;
    }
    if (s->crouch) s->crouch = js_round3(s->crouch);
    s->height = 11.0 - 3.0 * s->crouch;

    const int contact = s->on_ground;
    double accel = contact ? s->gr_a / 100.0 * s->speed : s->ar_a / 100.0;
    accel *= s->aim ? 1.0 : 0.55;
    accel *= s->crouch ? s->crch_spd : 1.0;
    accel *= s->spd_mlt * d;
    double decel = s->on_terrain ? 0.99 : s->on_ground ? s->gr_dec : s->air_dec;
    if (s->can_slide_t > 0) {
        s->can_slide_t -= d;
        if (s->can_slide_t <= 0) s->can_slide_t = 0;
    }
    if (s->crouch <= 0.5) s->can_slide = 1;
    if (!(s->on_ground && s->crouch)) s->slide = 0;
    if (s->slide) {
        s->slide -= d;
        if (s->slide <= 0) s->slide = 0;
        else {
            accel *= 0.25;
            decel = s->on_terrain ? 0.9996 : 0.999;
            const double v = sqrt(s->vx * s->vx + s->vz * s->vz);
            const double dir = atan2(cos(s->x_dire), sin(s->x_dire)); // getDir(sin, cos, 0, 0)
            s->vx = v * cos(dir + M_PI);
            s->vz = v * sin(dir + M_PI);
        }
    }
    s->jump_cd = fmax(s->jump_cd - d, 0);
    if (s->jump_cd <= 0 && s->on_ground) {
        if (s->did_jump && !jump) s->did_jump = 0;
        if (!s->did_jump && jump) server_jump(s);
    }
    if (!contact) s->vy -= d * (0.00015 * s->grav_mlt);
    accel *= clampd(1.0 - in[12], 0, 1);
    if (moving) { // movement[0].mov
        double a = accel;
        if (move_dir != M_PI && move_dir != 0 && move_dir != M_PI / 2 && move_dir != -M_PI / 2) a *= s->strafe_spd;
        s->vx += a * cos(move_dir - s->x_dire);
        s->vz += a * sin(move_dir - s->x_dire);
    }
    // movement[0].vel
    if (s->vx) { s->x += s->vx * d; s->vx *= pow(decel, d); s->vx = crop(s->vx, 0.0001); }
    if (s->vy) {
        s->y += s->vy * d;
        if (s->vy > 0) s->vy -= 0.000032 * d;
        else if (s->vy <= -0.3) s->vy = -0.3;
    }
    if (s->vz) { s->z += s->vz * d; s->vz *= pow(decel, d); s->vz = crop(s->vz, 0.0001); }

    // collisions: the floor box (top 0, 10 deep)
    s->was_on_ground = s->on_ground && !s->did_jump;
    s->on_ground = 0;
    if (s->y <= 0 && s->y + s->height >= -10) {
        if (s->y < 0 && -s->y <= 3 && s->oy < 0 && s->was_on_ground) {
            s->y += 0.03 * d;
            if (s->y > 0) s->y = 0;
            s->on_ground = 1;
            s->on_terrain = 0;
        } else if (s->oy >= 0) {
            s->y = 0;
            server_reset_step(s);
            s->vy = 0;
            s->on_ground = 1;
            s->on_terrain = 0;
        }
    }
}

// The server's reading of one "q" window ([resync, firstIsn, dts, dtLen, angles?, changes?]):
// frame j's 15 fields, as the official client built them (the inverse of :221755-221830).
static int server_decode_q(Server *s, const KrunkerFrame *f, double frames[][15], const int max) {
    const KrunkerValue *dts = krunker_frame_arg(f, 2);
    const int dt_len = (int) krunker_num(krunker_frame_arg(f, 3), 0);
    double dt[NET_INPUT_BATCH_MAX];
    int n = 0;
    if (krunker_is(dts, KRUNKER_STR) && dt_len > 0) {
        for (uint32_t o = 0; o + (uint32_t) dt_len <= dts->len && n < max; o += (uint32_t) dt_len) {
            char part[32];
            snprintf(part, sizeof(part), "%.*s", dt_len, krunker_str(dts) + o);
            dt[n++] = atof(part);
        }
    } else if (krunker_is(dts, KRUNKER_ARRAY)) {
        for (uint32_t i = 0; i < dts->len && n < max; i++) dt[n++] = krunker_num(&dts->u.items[i], 0);
    }
    if (krunker_num(krunker_frame_arg(f, 0), 0)) { // resync: the client restarted its deltas
        for (int k = 4; k < 15; k++) s->last_vals[k] = 0;
        s->last_vals[4] = -1;
        s->prev_angle_count = 0;
    }
    const KrunkerValue *angles = krunker_frame_arg(f, 4);
    if (krunker_is(angles, KRUNKER_ARRAY)) {
        s->prev_angle_count = (int) angles->len;
        for (uint32_t i = 0; i < angles->len && i < 2 + 2 * NET_INPUT_BATCH_MAX; i++) s->prev_angles[i] = krunker_num(&angles->u.items[i], 0);
    } // absent or 0: the same list as the window before
    const KrunkerValue *changes = krunker_frame_arg(f, 5);
    const int first = (int) krunker_num(krunker_frame_arg(f, 1), 0);
    double pitch = 0, yaw = 0;
    for (int j = 0; j < n; j++) {
        double *v = frames[j];
        v[0] = first + j;
        v[1] = dt[j];
        if (j == 0) {
            pitch = s->prev_angle_count >= 2 ? s->prev_angles[0] : 0;
            yaw = s->prev_angle_count >= 2 ? s->prev_angles[1] : 0;
        } else if (s->prev_angle_count > 2 * j + 1) {
            pitch += s->prev_angles[2 * j];
            yaw += s->prev_angles[2 * j + 1];
        }
        v[2] = pitch;
        v[3] = yaw;
        for (int k = 4; k < 15; k++) {
            char key[16];
            snprintf(key, sizeof(key), "%d-%d", j, k);
            const KrunkerValue *c = krunker_is(changes, KRUNKER_MAP) ? krunker_get(changes, key) : NULL;
            if (c) s->last_vals[k] = krunker_num(c, 0);
            v[k] = s->last_vals[k];
        }
    }
    return n;
}

// "l" SYNC_PLAYER at the live bundle's layout (KRUNKER_SYNC_*). aim_ours: the negative control that
// sends aimVal in our convention instead of Krunker's.
static void server_sync_frame(const Server *s, const int aim_ours, KrunkerBuf *out) {
    KrunkerValue l[KRUNKER_SYNC_SEND_VALS];
    for (int i = 0; i < KRUNKER_SYNC_SEND_VALS; i++) l[i] = KRUNKER_V_NUM(0);
    l[KRUNKER_SYNC_INPUT_SN] = KRUNKER_V_NUM(s->input_sn);
    l[KRUNKER_SYNC_PING] = KRUNKER_V_NUM(80);
    l[KRUNKER_SYNC_X] = KRUNKER_V_NUM(s->x);
    l[KRUNKER_SYNC_Y] = KRUNKER_V_NUM(s->y);
    l[KRUNKER_SYNC_Z] = KRUNKER_V_NUM(s->z);
    l[KRUNKER_SYNC_VEL_X] = KRUNKER_V_NUM(s->vx);
    l[KRUNKER_SYNC_VEL_Y] = KRUNKER_V_NUM(s->vy);
    l[KRUNKER_SYNC_VEL_Z] = KRUNKER_V_NUM(s->vz);
    l[KRUNKER_SYNC_X_DIRE] = KRUNKER_V_NUM(s->x_dire);
    l[KRUNKER_SYNC_ON_GROUND] = KRUNKER_V_BOOL(s->on_ground);
    l[KRUNKER_SYNC_DID_JUMP] = KRUNKER_V_BOOL(s->did_jump);
    l[KRUNKER_SYNC_DID_JUMP_W] = KRUNKER_V_BOOL(s->did_jump_w);
    l[KRUNKER_SYNC_WALL_JUMP_C] = KRUNKER_V_NUM(s->wall_jump_c);
    l[KRUNKER_SYNC_ON_WALL] = KRUNKER_V_NUM(0);
    l[KRUNKER_SYNC_ON_LADDER] = KRUNKER_V_BOOL(0);
    l[KRUNKER_SYNC_AIM_VAL] = KRUNKER_V_NUM(aim_ours ? 1.0 - s->aim : s->aim);
    l[KRUNKER_SYNC_CROUCH_VAL] = KRUNKER_V_NUM(s->crouch);
    l[KRUNKER_SYNC_WEAPON] = KRUNKER_V_NUM(0);
    l[KRUNKER_SYNC_SLIDE_TIMER] = KRUNKER_V_NUM(s->slide);
    l[KRUNKER_SYNC_CAN_SLIDE] = KRUNKER_V_BOOL(s->can_slide);
    l[KRUNKER_SYNC_ON_TERRAIN] = KRUNKER_V_BOOL(s->on_terrain);
    l[KRUNKER_SYNC_GRAPPLE] = KRUNKER_V_NIL;
    l[KRUNKER_SYNC_CAN_SLIDE_T] = KRUNKER_V_NUM(s->can_slide_t);
    l[KRUNKER_SYNC_PROTECTED] = KRUNKER_V_NIL;
    const KrunkerValue arg = KRUNKER_V_ARRAY(l, KRUNKER_SYNC_SEND_VALS);
    krunker_proto_encode_ah(0, "l", &arg, 1, out);
}

// ---------------------------------------------------------------- the wire (ordered, with latency)

#define WIRE_MAX 2048
typedef struct {
    double at[WIRE_MAX];
    uint8_t *data[WIRE_MAX];
    size_t len[WIRE_MAX];
    int head, count;
} Wire;

static void wire_push(Wire *w, const double at, const uint8_t *data, const size_t len) {
    if (w->count >= WIRE_MAX) return;
    const int i = (w->head + w->count++) % WIRE_MAX;
    w->at[i] = at;
    w->data[i] = malloc(len);
    memcpy(w->data[i], data, len);
    w->len[i] = len;
}

static int wire_pop(Wire *w, const double now, uint8_t **data, size_t *len) {
    if (!w->count || w->at[w->head] > now) return 0;
    *data = w->data[w->head];
    *len = w->len[w->head];
    w->head = (w->head + 1) % WIRE_MAX;
    w->count--;
    return 1;
}

// ---------------------------------------------------------------- the client (ours)

typedef struct {
    Game game;
    Player *me;
    KrunkerLink link;
    Server server;
    Wire up, down;
    double now, latency, server_next_tick, next_sync;
    Input pending[NET_RECON_PENDING_MAX + 64];
    size_t pending_count;
    NetInputPacket batch[NET_INPUT_BATCH_MAX];
    int batch_count;
    float batch_time, input_accum;
    int next_seq;
    unsigned frame_rng;
    int control_aim_ours, control_no_config;
    int control_no_pause;    // negative control: input frames built through a lock (the client before the pause)
    unsigned q_while_locked; // "q" windows sent while the link knew of a lock
    // measurements
    unsigned recons, over; // over: corrections above SYNC_TOLERANCE
    float max_correction, sum_correction;
} Sim;

static Sim *g_sim; // the link's io callbacks

static void io_send(void *user, const uint8_t *frame, const size_t len) {
    (void) user;
    wire_push(&g_sim->up, g_sim->now + g_sim->latency, frame, len);
}

static void sim_apply_spawn(Sim *sim, const NetSpawnPacket *s);

static void io_emit(void *user, const NetPacket *packet) {
    (void) user;
    Sim *sim = g_sim;
    switch (packet->type) {
        case NET_PACKET_SPAWN: {
            NetSpawnPacket spawn;
            memset(&spawn, 0, sizeof(spawn));
            memcpy(&spawn, packet->payload, packet->length < sizeof(spawn) ? packet->length : sizeof(spawn));
            sim_apply_spawn(sim, &spawn);
            break;
        }
        case NET_PACKET_GAME_CONFIG:
            if (!sim->control_no_config) game_config_receive(&sim->game, packet->payload, packet->length);
            break;
        case NET_PACKET_NETWORK_DATA: {
            NetNetworkDataPacket rates;
            memcpy(&rates, packet->payload, sizeof(rates));
            game_network_receive(&sim->game, &rates);
            break;
        }
        case NET_PACKET_RECON: {
            if (!sim->me || !sim->me->active) break; // client_recon_apply: a dead player takes no "l"
            NetReconPacket recon;
            memset(&recon, 0, sizeof(recon));
            memcpy(&recon, packet->payload, packet->length < sizeof(recon) ? packet->length : sizeof(recon));
            const vec3 before = sim->me->position;
            if (getenv("SYNC_DEBUG"))
                printf("    recon ack %d: client aim %.4f crouch %.3f pos %.4f %.4f %.4f vel %.6f %.6f | server-at-ack pos %.4f %.4f %.4f aim(ours) %.4f pending %zu\n",
                       recon.ack_seq, sim->me->aim_val, sim->me->crouch_val, before.x, before.y, before.z, sim->me->velocity.x, sim->me->velocity.z,
                       recon.position[0], recon.position[1], recon.position[2], recon.aim_val, sim->pending_count);
            sim->pending_count = net_reconcile(sim->me, &recon, sim->pending, sim->pending_count, 0);
            const vec3 after = sim->me->position;
            const float c = sqrtf((after.x - before.x) * (after.x - before.x) + (after.y - before.y) * (after.y - before.y) +
                                  (after.z - before.z) * (after.z - before.z));
            if (getenv("SYNC_DEBUG")) printf("    -> correction %.5f\n", (double) c);
            if (c > sim->max_correction) sim->max_correction = c;
            sim->sum_correction += c;
            if (c > SYNC_TOLERANCE) sim->over++;
            sim->recons++;
            break;
        }
        case NET_PACKET_EXT: {
            uint16_t subtype, body_length;
            const uint8_t *body;
            if (net_ext_unpack(packet->payload, packet->length, &subtype, &body, &body_length) && subtype == NET_EXT_KN_INPUTS_DROPPED)
                sim->pending_count = 0;
            break;
        }
        default: break;
    }
}

static const char *k_flat_map = "{\"name\":\"syncflat\",\"spawns\":[[0,0,0]],\"objects\":[{\"p\":[0,-10,0],\"s\":[8000,10,8000]}]}";

// init with the lobby's settings (arg 6 config, arg 12 networking) through the link
static void sim_init_frame(Sim *sim, const double grav, const double jump, const double strafe) {
    const KrunkerValue cfg_kv[] = {KRUNKER_V_STR("gravMlt"), KRUNKER_V_NUM(grav), KRUNKER_V_STR("jumpMlt"), KRUNKER_V_NUM(jump),
                                   KRUNKER_V_STR("strafeSpd"), KRUNKER_V_NUM(strafe)};
    const KrunkerValue net_kv[] = {KRUNKER_V_STR("tf"), KRUNKER_V_NUM(30), KRUNKER_V_STR("sf"), KRUNKER_V_NUM(1)};
    KrunkerValue args[13];
    for (int i = 0; i < 13; i++) args[i] = KRUNKER_V_NIL;
    args[0] = KRUNKER_V_NUM(0);
    args[1] = KRUNKER_V_NUM(0);
    args[6] = KRUNKER_V_MAP(cfg_kv, 3);
    args[12] = KRUNKER_V_MAP(net_kv, 2);
    KrunkerBuf buf = {0};
    krunker_proto_encode_ah(0, "init", args, 13, &buf);
    krunker_link_on_frame(&sim->link, buf.data, buf.len);
    krunker_buf_free(&buf);
}

static int sim_init(Sim *sim, const double grav, const double jump, const double strafe, const int controls) {
    memset(sim, 0, sizeof(*sim));
    g_sim = sim;
    sim->control_aim_ours = (controls & 2) != 0;
    sim->control_no_config = (controls & 4) != 0; // the client never applies the lobby config
    const cJSON *map = cJSON_Parse(k_flat_map);
    if (!map) return 0;
    game_configure(&sim->game, NULL, &map, 1, NULL, 0, NULL, 0, NULL, 0);
    game_init(&sim->game, 0, 0, 0);
    sim->me = player_init(&sim->game);
    sim->me->uid = 1;
    sim->me->class_index = 0; // Triggerman
    game_players_add(&sim->game, sim->me);
    player_spawn(sim->me);
    sim->me->is_you = 1;
    sim->me->position = sim->me->last_position = (vec3) {0.0f, 0.0f, 0.0f};
    sim->me->velocity = (vec3) {0.0f, 0.0f, 0.0f};
    sim->me->direction = (vec2) {0.0f, 0.0f};
    sim->me->swap_timer = 0.0f;

    const KrunkerLinkIO io = {.emit = io_emit, .send = io_send, .user = NULL};
    krunker_link_init(&sim->link, &io, 3);
    sim_init_frame(sim, grav, jump, strafe);

    server_defaults(&sim->server);
    sim->server.control_raw_move_dir = controls & 1;
    sim->server.grav_mlt = grav;
    sim->server.jump_mlt = jump;
    sim->server.strafe_spd = strafe;
    sim->server.can_slide = sim->me->can_slide;
    sim->latency = 0.045;
    sim->frame_rng = 5;
    sim->next_sync = 1.0;
    return 1;
}

static void client_flush(Sim *sim) {
    if (!sim->batch_count) return;
    if (sim->link.locked) sim->q_while_locked++;
    uint8_t payload[NET_MAX_PAYLOAD];
    payload[0] = (uint8_t) sim->batch_count;
    memcpy(payload + 1, sim->batch, (size_t) sim->batch_count * sizeof(NetInputPacket));
    krunker_link_send_ours(&sim->link, NET_PACKET_INPUT_BATCH, payload, (uint16_t) (1 + sim->batch_count * sizeof(NetInputPacket)));
    sim->batch_count = 0;
}

static void server_tick(Sim *sim) {
    uint8_t *data;
    size_t len;
    while (wire_pop(&sim->up, sim->now, &data, &len)) {
        KrunkerFrame f;
        // io_q: a dead player's "q" is dropped before it is decoded (the decoder state is not updated)
        if (!sim->server.dead && krunker_proto_decode(data, len, 0, &f) == KRUNKER_PROTO_OK) {
            if (f.name_len == 1 && f.name[0] == 'q') {
                double frames[NET_INPUT_BATCH_MAX][15];
                const int n = server_decode_q(&sim->server, &f, frames, NET_INPUT_BATCH_MAX);
                for (int j = 0; j < n; j++) {
                    if (sim->server.last_isn >= 0 && (int) frames[j][0] != sim->server.last_isn + 1) sim->server.isn_gaps++;
                    sim->server.last_isn = (int) frames[j][0];
                    server_proc(&sim->server, frames[j]);
                }
            }
            krunker_frame_free(&f);
        }
        free(data);
    }
    if (sim->now >= sim->next_sync) { // sf 1: one "l" a second, to a living player
        sim->next_sync += 1.0;
        if (!sim->server.dead) {
            KrunkerBuf buf = {0};
            server_sync_frame(&sim->server, sim->control_aim_ours, &buf);
            wire_push(&sim->down, sim->now + sim->latency, buf.data, buf.len);
            krunker_buf_free(&buf);
        }
    }
}

// One client frame (fps +-30%): the server ticks and deliveries inside it, then main.c's input
// frame (netRate 144 Hz sampling, prediction, the "q" window of 1 / tf).
static void sim_step(Sim *sim, Input input, const float fps) {
    sim->frame_rng = sim->frame_rng * 1664525u + 1013904223u;
    const float r = (float) ((sim->frame_rng >> 8) & 0xffff) / 65535.0f;
    const float delta = (1.0f / fps) * (0.7f + 0.6f * r);
    sim->now += delta;
    while (sim->server_next_tick <= sim->now) {
        server_tick(sim);
        sim->server_next_tick += 1.0 / 30.0;
    }
    uint8_t *data;
    size_t len;
    while (wire_pop(&sim->down, sim->now, &data, &len)) {
        krunker_link_on_frame(&sim->link, data, len);
        free(data);
    }
    if (!sim->me || !sim->me->active) { // dead or not spawned yet: no input frame leaves (main.c)
        game_tick(&sim->game, (float) sim->now, delta);
        return;
    }
    // recon.c client_recon_sample: v10 builds no input frame while "lock" holds (krunker_link_inputs_paused)
    if (!sim->control_no_pause && krunker_link_inputs_paused(&sim->link)) {
        game_tick(&sim->game, (float) sim->now, delta);
        return;
    }
    sim->batch_time += delta;
    const float input_delta = net_input_rate_take(&sim->input_accum, delta, 144);
    if (input_delta < 0.0f) {
        game_tick(&sim->game, (float) sim->now, delta);
        return;
    }
    input.delta = input_delta;
    input.seq = sim->next_seq++;
    player_queue_input(sim->me, &input);
    if (sim->pending_count < sizeof(sim->pending) / sizeof(sim->pending[0])) sim->pending[sim->pending_count++] = input;
    if (sim->batch_count >= NET_INPUT_BATCH_MAX) client_flush(sim);
    net_packet_from_input(&sim->batch[sim->batch_count++], &input);
    const int hz = game_network_data(&sim->game)->tick_hz;
    if (sim->batch_time >= 1.0f / (float) (hz > 0 ? hz : 30)) {
        sim->batch_time = 0.0f;
        client_flush(sim);
    }
    game_tick(&sim->game, (float) sim->now, input_delta);
}

// ---------------------------------------------------------------- scripts

typedef Input (*Script)(double t);

static Input base_input(const double t) {
    Input in = {0};
    in.y_dir = (float) fmod(0.4 * t, 2.0 * M_PI); // turning slowly
    in.x_dir = 0.15f * (float) sin(t);
    in.move_dir = -1;
    return in;
}

static Input script_walk(const double t) { // every key in turn: straights and diagonals
    Input in = base_input(t);
    static const int dirs[] = {0, 1, 2, 3, 4, 5, 6, 7, 0, 2, 6};
    in.move_dir = dirs[(int) (t / 0.7) % (int) (sizeof(dirs) / sizeof(dirs[0]))];
    return in;
}

static Input script_hold(const double t) { // one key held the whole time (W), the look turning
    Input in = base_input(t);
    in.move_dir = 0;
    return in;
}

static Input script_ads(const double t) { // aiming down sights on and off while walking
    Input in = script_walk(t);
    in.scope = fmod(t, 2.0) < 1.3;
    return in;
}

static Input script_jump(const double t) { // forward with jumps (held for a few frames)
    Input in = base_input(t);
    in.move_dir = t < 4.0 ? 0 : 7;
    in.jump = fmod(t, 0.9) < 0.06;
    return in;
}

static Input script_slide(const double t) { // run, jump, crouch before landing: slides
    Input in = base_input(t);
    in.move_dir = 0;
    const double p = fmod(t, 2.0);
    in.jump = p > 0.6 && p < 0.65;
    in.crouch = p > 0.75 && p < 1.5;
    return in;
}

typedef struct {
    float max, mean;
    unsigned recons, over;
} Result;

static Result run(Script script, const double seconds, const double grav, const double jump, const double strafe, const int controls) {
    Sim *sim = calloc(1, sizeof(Sim));
    Result res = {0};
    if (!sim || !sim_init(sim, grav, jump, strafe, controls)) {
        CHECK(0, "sim init");
        free(sim);
        return res;
    }
    while (sim->now < seconds) sim_step(sim, script(sim->now), 144.0f);
    res.max = sim->max_correction;
    res.recons = sim->recons;
    res.over = sim->over;
    res.mean = sim->recons ? sim->sum_correction / (float) sim->recons : 0.0f;
    free(sim);
    return res;
}

// ---------------------------------------------------------------- the spawn path (no direct setup)

static const char *k_socket = "sock1";

// The spawn as the live server sends it: "0" ADD_PLAYERS with our own entry (the socket id io-init
// gave, players.add :220269 / :171902), then "start" (startGame :218939). The link makes the SPAWN.
static void sim_spawn_packets(Sim *sim, const double yaw) {
    KrunkerValue add[KRUNKER_ADD_STRIDE];
    for (int i = 0; i < KRUNKER_ADD_STRIDE; i++) add[i] = KRUNKER_V_NIL;
    add[KRUNKER_ADD_ID] = KRUNKER_V_STR(k_socket);
    add[KRUNKER_ADD_SID] = KRUNKER_V_NUM(1);
    add[KRUNKER_ADD_X] = KRUNKER_V_NUM(0);
    add[KRUNKER_ADD_Y] = KRUNKER_V_NUM(0);
    add[KRUNKER_ADD_Z] = KRUNKER_V_NUM(0);
    add[KRUNKER_ADD_NAME] = KRUNKER_V_STR("sync");
    add[KRUNKER_ADD_CLASS_INDEX] = KRUNKER_V_NUM(0);
    add[KRUNKER_ADD_MAX_HEALTH] = KRUNKER_V_NUM(100);
    add[KRUNKER_ADD_TEAM] = KRUNKER_V_NUM(0);
    add[KRUNKER_ADD_X_DIRE] = KRUNKER_V_NUM(yaw);
    KrunkerValue list = KRUNKER_V_ARRAY(add, KRUNKER_ADD_STRIDE);
    KrunkerBuf buf = {0};
    krunker_proto_encode_ah(0, "0", &list, 1, &buf);
    krunker_link_on_frame(&sim->link, buf.data, buf.len);
    krunker_buf_free(&buf);
    KrunkerValue st[7];
    for (int i = 0; i < 7; i++) st[i] = KRUNKER_V_NIL;
    st[0] = KRUNKER_V_STR("3:00"); // the clock; moveLock (4) and kpdData (6) absent
    krunker_proto_encode_ah(0, "start", st, 7, &buf);
    krunker_link_on_frame(&sim->link, buf.data, buf.len);
    krunker_buf_free(&buf);
}

// The client's SPAWN (main.c client_apply_spawn: a dead player is spawned from rest) and the
// recon reset of our own spawn (recon.c client_recon_reset: CONTROLS.reset()).
static void sim_apply_spawn(Sim *sim, const NetSpawnPacket *s) {
    if (!s->is_you || !s->active) return;
    if (!sim->me) {
        sim->me = player_init(&sim->game);
        if (!sim->me) return;
        sim->me->uid = s->uid;
        sim->me->class_index = 0; // Triggerman
        game_players_add(&sim->game, sim->me);
    }
    sim->me->is_you = 1;
    player_spawn(sim->me);
    sim->me->team = s->team;
    sim->me->position = sim->me->last_position = (vec3) {s->position[0], s->position[1], s->position[2]};
    sim->me->direction = (vec2) {s->direction[0], s->direction[1]};
    sim->me->swap_timer = 0.0f;
    sim->input_accum = 0.0f;
    sim->pending_count = 0;
    sim->batch_count = 0;
    sim->batch_time = 0.0f;
    sim->next_seq = 0;
}

// A new session: io-init (the socket id), then the spawn above. Nothing is set up by the test.
static int sim_init_spawned(Sim *sim, const double grav, const double jump, const double strafe) {
    memset(sim, 0, sizeof(*sim));
    g_sim = sim;
    const cJSON *map = cJSON_Parse(k_flat_map);
    if (!map) return 0;
    game_configure(&sim->game, NULL, &map, 1, NULL, 0, NULL, 0, NULL, 0);
    game_init(&sim->game, 0, 0, 0);
    const KrunkerLinkIO io = {.emit = io_emit, .send = io_send, .user = NULL};
    krunker_link_init(&sim->link, &io, 3);
    sim_init_frame(sim, grav, jump, strafe);
    KrunkerValue sid = KRUNKER_V_STR(k_socket);
    KrunkerBuf buf = {0};
    krunker_proto_encode_ah(0, "io-init", &sid, 1, &buf);
    krunker_link_on_frame(&sim->link, buf.data, buf.len);
    krunker_buf_free(&buf);
    server_defaults(&sim->server);
    sim->server.grav_mlt = grav;
    sim->server.jump_mlt = jump;
    sim->server.strafe_spd = strafe;
    sim->server.can_slide = 1;
    sim->latency = 0.045;
    sim->frame_rng = 5;
    sim->next_sync = 1.0;
    sim_spawn_packets(sim, 0.0);
    return sim->me != NULL;
}

// A death and a respawn. The server kills us at kill_at and drops our "q" frames from then on (io_q);
// we learn of the kill one latency later, so the windows we send in between are lost to the server.
// The respawn: the server's new life at respawn_at, our ADD_PLAYERS / start one latency after that.
static Result run_spawned(const Script script, const double seconds, const double kill_at, const double respawn_at, const double grav,
                          const double jump, const double strafe) {
    Sim *sim = calloc(1, sizeof(Sim));
    Result res = {0};
    if (!sim || !sim_init_spawned(sim, grav, jump, strafe)) {
        CHECK(0, "spawned sim init");
        free(sim);
        return res;
    }
    int killed = 0, learned = 0, respawned = 0, spawned = 0;
    while (sim->now < seconds) {
        if (kill_at > 0 && !killed && sim->now >= kill_at) {
            killed = 1;
            sim->server.dead = 1;
        }
        if (killed && !learned && sim->now >= kill_at + sim->latency) { // the client's events_player_dead
            learned = 1;
            sim->me->active = 0;
            sim->batch_count = 0; // client_recon_on_death
            sim->batch_time = 0.0f;
        }
        if (killed && !respawned && sim->now >= respawn_at) {
            respawned = 1;
            server_respawn(&sim->server);
        }
        if (respawned && !spawned && sim->now >= respawn_at + sim->latency) {
            spawned = 1;
            sim_spawn_packets(sim, 0.0);
        }
        sim_step(sim, script(sim->now), 144.0f);
    }
    res.max = sim->max_correction;
    res.recons = sim->recons;
    res.over = sim->over;
    res.mean = sim->recons ? sim->sum_correction / (float) sim->recons : 0.0f;
    free(sim);
    return res;
}

// "lock" [true] at lock_at and [false] at unlock_at, each reaching us one latency later, W held throughout.
// v10 builds no input frame in between: nothing is sent, no isn is used up, nothing is predicted, and the
// server (no inputs) does not move us either. Measured: the "q" windows sent while locked, the isn gaps the
// server saw, how far we moved while locked, the corrections.
typedef struct {
    Result r;
    unsigned q_while_locked;
    int isn_gaps;
    float moved_while_locked;
} LockResult;

static void server_send_lock(Sim *sim, const int on) {
    const KrunkerValue arg = KRUNKER_V_BOOL(on);
    KrunkerBuf buf = {0};
    krunker_proto_encode_ah(0, "lock", &arg, 1, &buf);
    wire_push(&sim->down, sim->now + sim->latency, buf.data, buf.len);
    krunker_buf_free(&buf);
}

static LockResult run_locked(const Script script, const double seconds, const double lock_at, const double unlock_at, const int no_pause) {
    Sim *sim = calloc(1, sizeof(Sim));
    LockResult res = {0};
    if (!sim || !sim_init_spawned(sim, 1, 1, 1.2)) {
        CHECK(0, "locked sim init");
        free(sim);
        return res;
    }
    sim->control_no_pause = no_pause;
    int sent_lock = 0, sent_unlock = 0, was_locked = 0;
    vec3 at_lock = {0};
    while (sim->now < seconds) {
        if (!sent_lock && sim->now >= lock_at) sent_lock = 1, server_send_lock(sim, 1);
        if (!sent_unlock && sim->now >= unlock_at) sent_unlock = 1, server_send_lock(sim, 0);
        sim_step(sim, script(sim->now), 144.0f);
        if (sim->link.locked && !was_locked) at_lock = sim->me->position;
        if (sim->link.locked) {
            const vec3 p = sim->me->position;
            const float d = sqrtf((p.x - at_lock.x) * (p.x - at_lock.x) + (p.z - at_lock.z) * (p.z - at_lock.z));
            if (d > res.moved_while_locked) res.moved_while_locked = d;
        }
        was_locked = sim->link.locked;
    }
    res.r.max = sim->max_correction;
    res.r.recons = sim->recons;
    res.r.over = sim->over;
    res.r.mean = sim->recons ? sim->sum_correction / (float) sim->recons : 0.0f;
    res.q_while_locked = sim->q_while_locked;
    res.isn_gaps = sim->server.isn_gaps;
    free(sim);
    return res;
}

static Input script_release(const double t) { // W held; jump pressed at 1 s, released at 1.2 s
    Input in = base_input(t);
    in.move_dir = 0;
    in.jump = t >= 1.0 && t < 1.2;
    return in;
}

int main(void) {
    // Our tables against the live bundle's (the reference's constants): Triggerman, the AR
    {
        Sim *sim = calloc(1, sizeof(Sim));
        if (sim && sim_init(sim, 1, 1, 1.2, 0)) {
            CHECK(fabsf(sim->me->speed - 1.05f) < 1e-6f && fabsf(sim->me->weapon->speed_mlt - 0.95f) < 1e-6f &&
                  fabsf(sim->me->weapon->aim_speed - 0.13f) < 1e-6f,
                  "Triggerman speed 1.05 / AR spdMlt 0.95 / aimSpd 130 ms (ours %.3f %.3f %.3f)", sim->me->speed, sim->me->weapon->speed_mlt,
                  sim->me->weapon->aim_speed);
            CHECK(sim->game.config.gravity_mlt == 1.0f && sim->link.tick_hz == 30 && game_network_data(&sim->game)->tick_hz == 30,
                  "init's settings reached the client game");
        }
        free(sim);
    }

    static const struct {
        const char *name;
        Script script;
        double grav, jump, strafe;
    } cases[] = {
        {"walk (every key)", script_walk, 1, 1, 1.2},
        {"walk one direction (W held)", script_hold, 1, 1, 1.2},
        {"walk + ADS", script_ads, 1, 1, 1.2},
        {"jumps", script_jump, 1, 1, 1.2},
        {"slides", script_slide, 1, 1, 1.2},
        {"lobby config (gravMlt 0.7, jumpMlt 1.3, strafeSpd 1.6)", script_jump, 0.7, 1.3, 1.6},
        {"lobby config, every key", script_walk, 0.7, 1.3, 1.6},
    };
    // Our client is float and the server's model is double, so a discontinuity can differ by one frame:
    // at an ADS endpoint Krunker's aimVal can keep a residue of ~1e-17 above 0 (still truthy, so the
    // full ground accel applies: `aimVal ? 1 : 0.55`) while our float aim has already clamped to 0,
    // or the reverse. That frame's accel differs by 0.45x and the replay carries it: one snap of up to
    // ~0.2 units per run is expected (the "walk + ADS" run shows it). Two or more snaps above
    // SYNC_TOLERANCE, or any snap above 0.25, is a desync.
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const Result r = run(cases[i].script, 9.0, cases[i].grav, cases[i].jump, cases[i].strafe, 0);
        printf("%-58s %u syncs, correction max %.5f mean %.5f, over %.2f: %u\n", cases[i].name, r.recons, (double) r.max, (double) r.mean,
               (double) SYNC_TOLERANCE, r.over);
        CHECK(r.recons >= 7, "%s: syncs %u", cases[i].name, r.recons);
        CHECK(r.over <= 1 && r.max < 0.25f, "%s: %u corrections above %.2f, max %.5f", cases[i].name, r.over, (double) SYNC_TOLERANCE,
              (double) r.max);
    }

    // the spawn through the link (ADD_PLAYERS -> SPAWN -> start): player.init's state from rest
    {
        Sim *sim = calloc(1, sizeof(Sim));
        if (sim && sim_init_spawned(sim, 1, 1, 1.2)) {
            CHECK(sim->me && sim->me->active && sim->me->is_you && sim->me->on_ground && sim->me->velocity.y == 0.0f && sim->me->velocity.x == 0.0f &&
                      sim->me->velocity.z == 0.0f && sim->me->air_time == 0.0f && sim->me->position.y == 0.0f && sim->next_seq == 0,
                  "spawn: onGround, no velocity, air time 0, inputSN 0 at the spawn");
        }
        free(sim);
    }
    {
        const Result r = run_spawned(script_hold, 9.0, 0.0, 0.0, 1, 1, 1.2);
        printf("%-58s %u syncs, correction max %.5f mean %.5f, over %.2f: %u\n", "spawn (ADD_PLAYERS path), walk one direction", r.recons,
               (double) r.max, (double) r.mean, (double) SYNC_TOLERANCE, r.over);
        CHECK(r.recons >= 7 && r.over <= 1 && r.max < 0.25f, "spawn walk: syncs %u, %u corrections above %.2f, max %.5f", r.recons, r.over,
              (double) SYNC_TOLERANCE, (double) r.max);
    }
    // a death while a key changes in the 45 ms gap before the client learns of it (the release of
    // jump, the window is dropped by the server), then the respawn holding W: the server kept jump
    // held and jumps on spawn while the client thinks it stands. The official client's start resends
    // every field (startGame clears its delta memory); without that the two stay apart.
    {
        const Result r = run_spawned(script_release, 4.0, 1.19, 1.5, 1, 1, 1.2);
        printf("%-58s %u syncs, correction max %.5f mean %.5f, over %.2f: %u\n", "death in the gap, respawn walking W", r.recons, (double) r.max,
               (double) r.mean, (double) SYNC_TOLERANCE, r.over);
        CHECK(r.recons >= 3 && r.over <= 1 && r.max < 0.25f, "respawn: syncs %u, %u corrections above %.2f, max %.5f", r.recons, r.over,
              (double) SYNC_TOLERANCE, (double) r.max);
    }

    // "lock" while walking: no "q" while it holds, the isn stream has no gap, we stand still and the
    // server agrees when it ends
    {
        const LockResult l = run_locked(script_hold, 6.0, 2.0, 3.6, 0);
        printf("%-58s %u syncs, correction max %.5f, over %.2f: %u, q while locked %u, isn gaps %d, moved %.4f\n", "lock while walking (W held)",
               l.r.recons, (double) l.r.max, (double) SYNC_TOLERANCE, l.r.over, l.q_while_locked, l.isn_gaps, (double) l.moved_while_locked);
        CHECK(l.q_while_locked == 0, "lock: %u \"q\" windows sent while locked (v10 builds no input frame)", l.q_while_locked);
        CHECK(l.isn_gaps == 0, "lock: the server saw %d isn gaps", l.isn_gaps);
        CHECK(l.moved_while_locked < 1e-4f, "lock: we moved %.4f while locked", (double) l.moved_while_locked);
        CHECK(l.r.recons >= 5 && l.r.over <= 1 && l.r.max < 0.25f, "lock: syncs %u, %u corrections above %.2f, max %.5f", l.r.recons, l.r.over,
              (double) SYNC_TOLERANCE, (double) l.r.max);
        const LockResult c = run_locked(script_hold, 6.0, 2.0, 3.6, 1);
        printf("%-58s q while locked %u\n", "control: input frames built through the lock", c.q_while_locked);
        CHECK(c.q_while_locked > 10, "the control must send \"q\" while locked (%u)", c.q_while_locked);
    }

    // negative controls: each bug must show as corrections
    const Result raw = run(script_walk, 9.0, 1, 1, 1.2, 1);
    printf("%-58s correction max %.5f\n", "control: move_dir unmapped (45 degrees off)", (double) raw.max);
    CHECK(raw.max > 1.0f, "unmapped move_dir must desync (max %.5f)", (double) raw.max);
    const Result aim = run(script_ads, 9.0, 1, 1, 1.2, 2);
    printf("%-58s correction max %.5f\n", "control: aimVal not inverted", (double) aim.max);
    CHECK(aim.max > 0.1f, "aimVal in our convention must desync (max %.5f)", (double) aim.max);
    const Result cfg = run(script_jump, 9.0, 0.7, 1.3, 1.6, 4);
    printf("%-58s correction max %.5f\n", "control: lobby config not applied", (double) cfg.max);
    CHECK(cfg.max > 0.1f, "an unapplied lobby config must desync (max %.5f)", (double) cfg.max);

    printf("%d/%d passed\n", g_checks - g_failures, g_checks);
    return g_failures ? 1 : 0;
}
