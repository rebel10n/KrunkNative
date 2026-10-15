// The world behind the menu, ported from Krunker v3.1.1 (see client/include/menu_view.h).
// Reference (readable v3.1.1 source, src/index.js + src/libs/controls.js):
//   gameLoop:  endAnimationTimeLeft / config.endAnim delta scaling, the menu camera
//              (`!player && !window.spectating`: menuCameraYaw += deltaMs * 0.0001),
//              CONTROLS.update(deltaMs * Game.config.deltaMlt) every frame
//   g1():      CONTROLS.camLookAt(killer.x, killer.y + killer.height / 1.5, killer.z)
//   endGame(): endAnimationTimeLeft = config.endAnim when (player && player.active) || spectating
//   controls.js camLookAt / update (chase camera), config.js camChaseTrn/Spd/Dst
#include <menu_view.h>
#include <ruleset.h> // config.endAnim (Ruleset.end_anim: Krunker Strike 2.5 s)
#include <spectate.h>
#include <input_script.h> // input_script_adjust_input: `look` (menu_view_look)
#include <ui_logic.h>     // ui_controls_fill_look: the alive mouse sensitivity, reused while dead
#include <bots.h>  // bot_object_blocks_player (death_cam_test)
#include <world.h> // world_solid_fraction
#include <math.h>
#include <stdint.h>

#define MENU_CAM_YAW_RATE 0.0001f // menuCameraYaw += deltaMs * 0.0001
#define CAM_CHASE_TRN 0.0022f     // config.camChaseTrn: turn rate per ms
#define CAM_CHASE_SPD 0.0012f     // config.camChaseSpd: glide rate per ms
#define CAM_CHASE_DST 24.0f       // config.camChaseDst: stop this far from the killer
#define END_ANIM_MS (ruleset_current()->end_anim * 1000.0f) // config.endAnim (v10 5000, Krunker Strike 2500)
#define CAM_DELTA_MAX_MS 33.0f    // config.dltMx = Math.round(1000 / 30): gameLoop's gameTimeDiff cap
// KrunkNative: the death chase stops this far in front of the first solid box / terrain between
// the death eye and its goal (v7.1.4 has no test: a killer within camChaseDst put the goal behind
// the victim, often inside or past a wall, and the player saw the void until the respawn).
#define DEATH_CAM_WALL_GAP 2.0f
// CONTROLS.updateMouseInp: the look sensitivity is (CONTROLS.target ? 0.2 : 1) of the alive one, and
// CONTROLS.target is set while the chase runs (a killer is known).
#define DEATH_CAM_CHASE_LOOK_SCALE 0.2f

static struct {
    float menu_yaw;   // menuCameraYaw (never reset by Krunker, so the spin carries over rounds)
    int dead;         // the death view (killer or not): the rig takes the mouse and holds the death pose
    int chase;        // CONTROLS.target set (a killer is known)
    int killer_uid;   // g1(): killer.isSpectating = true until our next spawn, -1 none
    vec3 position;    // CONTROLS.object.position
    float yaw;        // CONTROLS.object.rotation.y
    float pitch;      // CONTROLS.pitchObj.rotation.x
    float target_yaw, target_pitch;
    vec3 target;      // chase goal camChaseDst short of the killer
    int ended;        // the END state was seen (net_end_seconds >= 0)
    float end_left;   // endAnimationTimeLeft in ms, < 0 = null (no slow motion)
} g_view = {.end_left = -1.0f, .killer_uid = -1};

// UTILS.angleDistance
static float angle_distance(const float from, const float to) {
    return atan2f(sinf(to - from), cosf(from - to));
}

// UTILS.direction2D(z1, x1, z2, x2): the yaw that looks from 1 to 2 (three.js -Z forward).
static float direction_2d(const float z1, const float x1, const float z2, const float x2) {
    return atan2f(x1 - x2, z1 - z2);
}

// UTILS.getXDire: the pitch that looks from a to b (negative = down).
static float x_dire(const vec3 a, const vec3 b) {
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    const float dist = sqrtf(dx * dx + dy * dy + dz * dz);
    if (dist <= 0.0f) return 0.0f;
    return asinf(fabsf(dy) / dist) * (a.y > b.y ? -1.0f : 1.0f);
}

// CONTROLS.camLookAt(x, y, z)
static void chase_look_at(const vec3 at) {
    const float pitch = x_dire(g_view.position, at);
    const float yaw = direction_2d(g_view.position.z, g_view.position.x, at.z, at.x);
    g_view.target_pitch = pitch;
    g_view.target_yaw = yaw;
    g_view.target.x = at.x + CAM_CHASE_DST * sinf(yaw) * cosf(pitch);
    g_view.target.y = at.y - CAM_CHASE_DST * sinf(pitch);
    g_view.target.z = at.z + CAM_CHASE_DST * cosf(yaw) * cosf(pitch);
    g_view.chase = 1;
}

// KrunkNative (not in v7.1.4, see DEATH_CAM_WALL_GAP): pull the chase goal back along the
// segment from the death eye to the first solid object box, map triangle or the terrain
// (world_solid_fraction, shared with the third-person camera). CONTROLS.update only ever moves
// along that segment, so the camera stays in open space between the death position and the goal.
static void chase_clamp_target(const Game *game) {
    const vec3 from = g_view.position;
    const vec3 seg = {g_view.target.x - from.x, g_view.target.y - from.y, g_view.target.z - from.z};
    const float len = sqrtf(seg.x * seg.x + seg.y * seg.y + seg.z * seg.z);
    if (!(len > 0.0f) || !game) return;

    const float t = world_solid_fraction(game, from, seg);
    if (t >= 1.0f) return;

    const float keep = fmaxf(0.0f, t * len - DEATH_CAM_WALL_GAP) / len;
    g_view.target = (vec3) {from.x + seg.x * keep, from.y + seg.y * keep, from.z + seg.z * keep};
}

void menu_view_death_cam(Client *client, const Player *killer) {
    menu_view_reset();
    if (!client) return;

    // The controls rig sits where the first-person camera was (moveCam) with the
    // player's look (object.rotation.y / pitchObj.rotation.x) when the kill lands.
    const Player *me = client->me;
    g_view.dead = 1;
    g_view.position = client->camera.position;
    g_view.yaw = me ? me->direction.y : client->camera.rotation.y;
    g_view.pitch = me ? me->direction.x : client->camera.rotation.x;
    if (!killer) return; // no chase (suicide, fall, kCams off): the mouse still turns the rig

    // g1() also sets killer.interpolate = false: the killer holds still until its next state
    // (client_apply_state starts a new interpolation).
    if (killer != me) ((Player *) killer)->interpolate = 0;
    g_view.killer_uid = killer->uid;
    const vec3 at = {killer->position.x, killer->position.y + killer->height / 1.5f, killer->position.z};
    chase_look_at(at);
    chase_clamp_target(&client->game);
}

void menu_view_reset(void) {
    g_view.dead = 0;
    g_view.chase = 0;
    g_view.killer_uid = -1; // players.js spawn/reset: isSpectating = false
}

int menu_view_dead(void) {
    return g_view.dead;
}

// CONTROLS.updateMouseInp while dead (not spectating, camRot on): the look turns the rig with the
// mouse, sensitivity / aim sensitivity / invertY as alive (ui_controls_fill_look), pitch clamped
// to +-PI/2, yaw wrapped (controls.js xDr). With the chase (a killer known) the look runs at 0.2 of
// that speed (DEATH_CAM_CHASE_LOOK_SCALE). The chase is not switched off by it: CONTROLS.update
// keeps pulling the rig toward the killer every frame, so the look only stays away while it moves.
void menu_view_look(Client *client, const float dx, const float dy) {
    if (!g_view.dead) return;
    const float scale = g_view.chase ? DEATH_CAM_CHASE_LOOK_SCALE : 1.0f;
    Input look = {0};
    look.x_dir = g_view.pitch;
    look.y_dir = g_view.yaw;
    ui_controls_fill_look(client, &look, dx * scale, dy * scale);
    input_script_adjust_input(client, &look); // `look` in the input script (tests)
    g_view.pitch = player_clamp_pitch(look.x_dir);
    g_view.yaw = player_wrap_yaw(look.y_dir);
}

int menu_view_killer_uid(void) {
    return g_view.killer_uid;
}

float menu_view_time_scale(void) {
    return g_view.end_left < 0.0f ? 1.0f : g_view.end_left / END_ANIM_MS;
}

// CONTROLS.update(ms): turn toward the killer, glide to camChaseDst short of it.
static void chase_update(const float ms) {
    g_view.yaw += angle_distance(g_view.yaw, g_view.target_yaw) * ms * CAM_CHASE_TRN;
    g_view.pitch += angle_distance(g_view.pitch, g_view.target_pitch) * ms * CAM_CHASE_TRN;

    const vec3 p = g_view.position, t = g_view.target;
    const float dx = p.x - t.x, dy = p.y - t.y, dz = p.z - t.z;
    const float step = sqrtf(dx * dx + dy * dy + dz * dz) * ms * CAM_CHASE_SPD;
    const float yaw = direction_2d(p.z, p.x, t.z, t.x);
    const float pitch = x_dire(p, t);
    g_view.position.x -= step * sinf(yaw) * cosf(pitch);
    g_view.position.y += step * sinf(pitch);
    g_view.position.z -= step * cosf(yaw) * cosf(pitch);
}

float menu_view_world_delta(Client *client, const float delta) {
    const int playing = (client->me && client->me->active) || spectate_active();
    const int ended = client->net_end_seconds >= 0 && client->net_end_result != INT32_MIN;

    if (ended && !g_view.ended) {
        // endGame(): slow motion only for someone still in the round.
        g_view.end_left = playing ? END_ANIM_MS : -1.0f;
    } else if (!ended) {
        g_view.end_left = -1.0f; // next round's INIT: endAnimationTimeLeft = null
    }
    g_view.ended = ended;

    if (g_view.end_left < 0.0f) return delta;

    // endAnimationTimeLeft counts down in real time, then scales the frame's delta.
    g_view.end_left = fmaxf(0.0f, g_view.end_left - delta * 1000.0f);
    return delta * menu_view_time_scale();
}

void menu_view_camera(Client *client, const float delta) {
    const Player *me = client->me;
    // CONTROLS.update(gameTimeDiff * deltaMlt): gameTimeDiff = min(frame, dltMx), then the endAnim
    // scale. Without the cap one long frame (a hitch while the death card loads, an alt-tab, a
    // throttled browser tab) moved the chase camera by delta * camChaseSpd times its distance:
    // past ~1.7 s it overshoots further each frame and the camera flies off the map.
    const float frame_ms = fminf(delta * 1000.0f, CAM_DELTA_MAX_MS * menu_view_time_scale()); // delta is already endAnim-scaled
    const float ms = frame_ms * (client->game.config.delta_mlt > 0.0f ? client->game.config.delta_mlt : 1.0f);

    if (spectate_active() || (me && me->active)) {
        menu_view_reset(); // CONTROLS.reset() on spawn / spectate start; their cameras pose themselves
        return;
    }

    if (!me) {
        // No player yet (or a new round): CONTROLS.moveCam(map camPos) + rotateCam(menuCameraYaw, 0, 0).
        menu_view_reset();
        const Map *map = client->game.map;
        client->camera.position = map->camera_position;
        if (map->camera_static) {
            // v7.1.4 camPos[3] (hasStaticMenuCamera): the pose of rotateCam(camPos[5], camPos[4],
            // camPos[6]) stays (rotation.y = [5], x = [4], z = [6]); no menu yaw (map-G)
            client->camera.rotation = map->camera_rotation;
        } else {
            g_view.menu_yaw = fmodf(g_view.menu_yaw + frame_ms * MENU_CAM_YAW_RATE, 2.0f * (float) M_PI);
            client->camera.rotation = (vec3) {0.0f, g_view.menu_yaw, 0.0f};
        }
        client->camera.zoom = 1.0f;
        return;
    }

    // Dead: CONTROLS.update runs every frame (menu open or not): the chase toward the killer when
    // there is one, then the rig's pose (the mouse look of menu_view_look turns it between frames).
    if (!g_view.dead) return;
    if (g_view.chase) chase_update(ms);
    client->camera.position = g_view.position;
    client->camera.rotation = (vec3) {g_view.pitch, g_view.yaw, 0.0f};
}
