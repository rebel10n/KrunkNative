// Krunker v3.1.1 spectating (window.Bf5): state, cameras and controls. The HUD half
// (#spectateUI) is client/src/ui/hud_spectate.c; see client/include/spectate.h for the
// flow. Reference (unpacked v3.1.1 client):
//   index.js  toggleSpect / spectMode / pressButton's Bf5 block / the game loop's Bf5
//             branch (first-person follow: cm.crouch / cm.aim easing, zoom, Vp8 hidden)
//   132.js    spect {speed 1, followDis = minD 10, maxD 160}, followCam, bPZ1, ZZms7
//             (free cam), the wheel handler and updateMouseInp (look, pitch clamp)
// KrunkNative has no objective spectate cams (SPECTATE_CAM prefabs): H and 0 do nothing,
// exactly like v3.1.1 in modes without objectives.
#include <spectate.h>
#include <ui_logic.h>
#include <html_ui.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define SPECT_MIN_DIST 10.0f      // config.specMinD
#define SPECT_MAX_DIST 160.0f     // config.specMaxD (mode.specMaxD overrides it, none in KrunkNative)
#define SPECT_MIN_SPEED 1.0f
#define SPECT_MAX_SPEED 10.0f
#define SPECT_FLY_SPEED 0.04f     // ZZms7: units per ms at speed 1 ...
#define SPECT_FLY_FAST 0.08f      // ... and with the crouch key held
#define SPECT_FOLLOW_LERP 0.9f    // followCam: object.position.lerp(target, 0.9) per frame
#define SPECT_CROUCH_RATE 0.0085f // cm.crouch easing per ms
#define SPECT_AIM_RATE 0.009f     // cm.aim easing per ms
#define DOM_KEY_Q 81              // ZZms7 keys[81]: down
#define DOM_KEY_E 69              // ZZms7 keys[69]: up

static struct {
    int requested;       // co
    int active;          // window.Bf5
    int target_uid;      // b4.spect.target (players only), -1 none
    int person;          // cu: 1 first person (default), 0 third person
    int toggled;         // R was pressed at least once (#specIns keeps its markup text until then)
    float speed;         // spect.speed
    float follow_dist;   // spect.followDis
    vec3 position;       // b4.object.position (free cam / follow pivot)
    float yaw;           // b4.object.rotation.y
    float pitch;         // b4.hRI0.rotation.x
    float crouch;        // cm.crouch
    float aim;           // cm.aim, Krunker convention (1 = not aimed); < 0 unset
    Player *hidden;      // player whose meshes spectate_render_begin took out of the scene
} g_spect = {
    .target_uid = -1,
    .person = 1,
    .speed = 1.0f,
    .follow_dist = SPECT_MIN_DIST,
    .aim = -1.0f,
};

void spectate_set_requested(const int on) {
    g_spect.requested = !!on;
    menu_log("toggleSpect(%s)", on ? "true" : "false");
}

int spectate_requested(void) { return g_spect.requested; }
int spectate_active(void) { return g_spect.active; }
int spectate_target_uid(void) { return g_spect.active ? g_spect.target_uid : -1; }
int spectate_first_person(void) { return g_spect.person; }

const char *spectate_person_label(void) {
    // #specIns starts as "First Person" in the markup; the R toggle writes cu ? "Third Person" : "First Person".
    return g_spect.toggled && g_spect.person ? "Third Person" : "First Person";
}

// "fpSp": tell the server who we follow (informational).
static void spectate_send_target(Client *client, const int uid) {
    const NetSpectateTargetPacket packet = {.uid = uid};
    client_net_send(client, NET_PACKET_SPECTATE_TARGET, &packet, sizeof(packet));
}

// Spectators have no player: forget ours (the server removed it from the match).
static void spectate_drop_me(Client *client) {
    Player *me = client->me;
    if (!me) return;

    for (size_t i = 0; i < client->game.player_count; i++) {
        if (client->game.players[i] != me) continue;
        memmove(&client->game.players[i], &client->game.players[i + 1], (client->game.player_count - i - 1) * sizeof(Player *));
        client->game.player_count--;
        break;
    }

    if (me->mesh) {
        scene_remove_player_mesh(me->render_you ? client->fps_scene : client->scene, me->mesh, me->loadout_size);
        player_meshes_fini(me);
    }

    free(me->loadout);
    free(me->ammo);
    free(me->reloads);
    free(me->input_queue);
    free(me);
    client->me = NULL;
}

void spectate_begin(Client *client) {
    spectate_render_end(client);
    // From the menu / a new round the free cam starts at the map's camera; a player who
    // switches to spectating keeps the view it had (b4.object stays where it was).
    const int had_player = client->me != NULL;
    spectate_drop_me(client);

    // fc(..., spectating): b4.reset() (no target, rotation 0), UI.toggleGameUI(true).
    g_spect.active = 1;
    g_spect.target_uid = -1;
    g_spect.position = had_player || !client->game.map ? client->camera.position : client->game.map->camera_position;
    g_spect.yaw = 0.0f;
    g_spect.pitch = 0.0f;
    g_spect.crouch = 0.0f;
    g_spect.aim = -1.0f; // spect.speed / followDis carry over (b4.reset() keeps them)

    client->in_game = 1;
    client->spawn_requested = 0;
    client->net_death_time = 0.0;
    client->net_death_menu_pending = 0;
    client->camera.zoom = 1.0f;
    hud_ui_hide_death(client); // toggleGameUI(true): the death card goes away
    hud_spectate_reset();
    printf("[spectate] spectating\n");
}

void spectate_end(Client *client) {
    if (!g_spect.active) return;

    spectate_render_end(client);
    g_spect.active = 0;
    g_spect.target_uid = -1;
    client->camera.zoom = 1.0f;
    printf("[spectate] stopped spectating\n");
}

// 132.js bPZ1: the free cam continues from where the camera is (third-person orbit included).
static void spectate_free_from_camera(const Client *client) {
    g_spect.position = client->camera.position;
}

void spectate_mode(Client *client, const int dir) {
    if (!g_spect.active) return;

    if (dir) {
        // Cycle aW.players.list from the current target (index 0 without one) to the next active player.
        const int count = (int) client->game.player_count;
        int index = 0;
        int pick = -1;

        for (int i = 0; i < count; i++) {
            const Player *player = client->game.players[i];
            if (player && g_spect.target_uid >= 0 && player->uid == g_spect.target_uid) index = i;
        }

        for (int i = 0; i < count; i++) {
            index += dir;
            if (index >= count) index = 0;
            else if (index < 0) index = count - 1;

            const Player *player = client->game.players[index];
            if (player && player->active && player != client->me) {
                pick = player->uid;
                break;
            }
        }

        if (pick != g_spect.target_uid) {
            g_spect.target_uid = pick;
            spectate_send_target(client, pick);
        }
    } else {
        g_spect.target_uid = -1;
        g_spect.crouch = 0.0f; // cm = {}
        g_spect.aim = -1.0f;
        spectate_free_from_camera(client);
        spectate_send_target(client, -1);
    }

    client->camera.zoom = 1.0f; // aY.zoom(1); the aim recticle opacity follows the target (hud_spectate.c)
    hud_spectate_refresh();
}

static void spectate_toggle_person(Client *client) {
    g_spect.person = !g_spect.person;
    g_spect.toggled = 1;
    spectate_free_from_camera(client); // bPZ1
    hud_spectate_refresh();
}

int spectate_press_key(Client *client, const int code) {
    if (!g_spect.active) return 0;

    if (code == 70) { // F: free cam
        spectate_mode(client, 0);
        return 1;
    }

    if (code == 72 || code == 48) return 1; // H / 0: objective cams, none in KrunkNative's modes

    if (code > 48 && code < 58) { // 1-9: the player in that #specTeam slot
        const int uid = hud_spectate_slot_uid(code - 49);
        const Player *player = uid >= 0 ? client_find_player(client, uid) : NULL;

        if (player && player->active) {
            g_spect.target_uid = uid;
            spectate_send_target(client, uid);
            hud_spectate_refresh();
        }
        return 1;
    }

    if (code == 82) { // R: first / third person
        spectate_toggle_person(client);
        return 1;
    }

    return 0;
}

int spectate_mouse_press(Client *client, const int which) {
    if (!g_spect.active) return 0;
    if (which == 1) spectate_mode(client, -1);
    else if (which == 3) spectate_mode(client, 1);
    else return 0;
    return 1;
}

int spectate_scroll(Client *client, const int wheel_up) {
    (void) client;
    if (!g_spect.active) return 0;

    const float step = wheel_up ? 1.0f : -1.0f;

    if (g_spect.target_uid >= 0) {
        g_spect.follow_dist = CLAMP(g_spect.follow_dist - step * 3.0f, SPECT_MIN_DIST, SPECT_MAX_DIST);
    } else {
        g_spect.speed = CLAMP(g_spect.speed + step, SPECT_MIN_SPEED, SPECT_MAX_SPEED);
    }

    return 1;
}

// camera local -> world for the yaw/pitch rig (three.js object.rotation.y > hRI0.rotation.x).
static vec3 spectate_rotate(const float x, const float y, const float z) {
    const float cp = cosf(g_spect.pitch), sp = sinf(g_spect.pitch);
    const float cy = cosf(g_spect.yaw), sy = sinf(g_spect.yaw);
    const float ry = y * cp - z * sp;
    const float rz = y * sp + z * cp;
    return (vec3) {x * cy + rz * sy, ry, -x * sy + rz * cy};
}

static float spectate_ease(const float value, const float target, const float step) {
    if (value < target) return value + step >= target ? target : value + step;
    if (value > target) return value - step <= target ? target : value - step;
    return value;
}

// ZZms7: fly along the camera axes (move binds, Q down / E up), crouch key = double speed.
static void spectate_fly(const float ms) {
    float x = 0.0f, y = 0.0f, z = 0.0f;
    if (ui_controls_down(UI_CONTROL_FORWARD)) z -= 1.0f;
    if (ui_controls_down(UI_CONTROL_BACKWARD)) z += 1.0f;
    if (ui_controls_down(UI_CONTROL_LEFT)) x -= 1.0f;
    if (ui_controls_down(UI_CONTROL_RIGHT)) x += 1.0f;
    if (ui_controls_code_down(DOM_KEY_Q)) y -= 1.0f;
    if (ui_controls_code_down(DOM_KEY_E)) y += 1.0f;

    const float speed = (ui_controls_down(UI_CONTROL_CROUCH) ? SPECT_FLY_FAST : SPECT_FLY_SPEED) * g_spect.speed * ms;
    const vec3 move = spectate_rotate(x, y, z);
    g_spect.position.x += move.x * speed;
    g_spect.position.y += move.y * speed;
    g_spect.position.z += move.z * speed;
}

// The followed player's eye (first person): y + height - cameraHeight - cm.crouch * crouchDst,
// view = its look direction, zoom = its weapon's zoom eased by cm.aim.
static void spectate_first_person_pose(Client *client, const Player *target, const float ms) {
    g_spect.crouch = spectate_ease(g_spect.crouch, target->crouch_val, ms * SPECT_CROUCH_RATE);

    float zoom = 1.0f;
    const Weapon *weapon = target->weapon;
    if (weapon) {
        if (g_spect.aim < 0.0f) g_spect.aim = 1.0f;
        g_spect.aim = spectate_ease(g_spect.aim, 1.0f - target->aim_val, ms * SPECT_AIM_RATE);
        if (weapon->zoom) zoom = 1.0f + (weapon->zoom - 1.0f) * (1.0f - g_spect.aim);
    }

    g_spect.position.x = target->position.x;
    g_spect.position.y = target->position.y + game_constants.player_height - game_constants.camera_height - g_spect.crouch * game_constants.crouch_distance;
    g_spect.position.z = target->position.z;
    g_spect.yaw = target->direction.y;
    g_spect.pitch = target->direction.x;

    client->camera.position = g_spect.position;
    client->camera.zoom = zoom;
}

// followCam: the pivot eases to the top of the player's head, the camera sits followDis
// behind it along the view direction (the mouse orbits it).
static void spectate_follow_pose(Client *client, const Player *target) {
    const vec3 goal = {target->position.x, target->position.y + game_constants.player_height, target->position.z};
    g_spect.position.x += (goal.x - g_spect.position.x) * SPECT_FOLLOW_LERP;
    g_spect.position.y += (goal.y - g_spect.position.y) * SPECT_FOLLOW_LERP;
    g_spect.position.z += (goal.z - g_spect.position.z) * SPECT_FOLLOW_LERP;

    const vec3 offset = spectate_rotate(0.0f, 0.0f, g_spect.follow_dist);
    client->camera.position.x = g_spect.position.x + offset.x;
    client->camera.position.y = g_spect.position.y + offset.y;
    client->camera.position.z = g_spect.position.z + offset.z;
}

void spectate_tick(Client *client, const float delta, const float mouse_dx, const float mouse_dy) {
    if (!g_spect.active || !client->game.ready) return;

    const float ms = MIN(delta, 0.1f) * 1000.0f;
    const Player *target = g_spect.target_uid >= 0 ? client_find_player(client, g_spect.target_uid) : NULL;

    if (g_spect.target_uid >= 0 && (!target || !target->active)) {
        spectate_mode(client, 0); // left the game / died: back to the free cam
        target = NULL;
    }

    const int first_person = target && g_spect.person;

    if (!first_person) {
        // updateMouseInp (window.noMouseInp is set in first person): sensitivity, aim
        // sensitivity while the right button is held, invertY; pitch clamped to +-PI/2.
        Input look = {0};
        look.x_dir = g_spect.pitch;
        look.y_dir = g_spect.yaw;
        ui_controls_fill_input(client, &look, mouse_dx, mouse_dy);
        g_spect.pitch = CLAMP(look.x_dir, -(float) M_PI / 2.0f, (float) M_PI / 2.0f);
        g_spect.yaw = player_wrap_yaw(look.y_dir); // controls.js xDr % Math.PI2 (float precision)
    }

    if (first_person) {
        spectate_first_person_pose(client, target, ms);
    } else if (target) {
        spectate_follow_pose(client, target);
    } else {
        spectate_fly(ms);
        client->camera.position = g_spect.position;
    }

    client->camera.rotation.x = g_spect.pitch;
    client->camera.rotation.y = g_spect.yaw;
    client->camera.rotation.z = 0.0f;
}

void spectate_render_begin(Client *client) {
    spectate_render_end(client);
    if (!g_spect.active || !g_spect.person || g_spect.target_uid < 0) return;

    Player *target = client_find_player(client, g_spect.target_uid);
    if (!target || !target->active || !target->mesh || target->render_you) return;

    scene_remove_player_mesh(client->scene, target->mesh, target->loadout_size);
    g_spect.hidden = target;
}

void spectate_render_end(Client *client) {
    Player *hidden = g_spect.hidden;
    g_spect.hidden = NULL;
    if (!hidden) return;

    // Only while it is still the same live player with the same meshes.
    for (size_t i = 0; i < client->game.player_count; i++) {
        if (client->game.players[i] == hidden && hidden->mesh) {
            scene_add_player_mesh(client->scene, hidden->mesh, hidden->loadout_size);
            return;
        }
    }
}
