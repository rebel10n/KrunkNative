// KrunkNative third-person camera (client/include/third_person.h): the console / bind state, the
// server grant, the local model swap and the camera pose.
#include <third_person.h>
#include <class_camera.h>     // the game's / class's own camera offsets
#include <prop_hunt_client.h> // Prop Hunt: the Prop team's camera
#include <console.h>
#include <spectate.h>
#include <player_class_model.h> // player_class_model_keep
#include <ui_logic.h>
#include <math.h>
#include <stdio.h>

static struct {
    int wanted;     // the player asked for third person (thirdperson / the bind) and did not go back
    int granted;    // the server's NET_CHEAT_THIRD_PERSON 1 for this round
    int reask;      // INIT cleared the grant while wanted: ask again once in game
    int cancelled;  // firstperson while a request was unanswered: its grant is dropped
    int built;      // the mode the local player's mesh was built for (-1 none yet)
    const Player *built_for;
    float keep;     // the camera distance shown (eases out after a wall pulled it in)
    int own_model, viewmodel; // the last frame's passes (third_person_note_frame)
} g_tp = {.built = -1};

static void tp_send(Client *client, const int on) {
    client_chat_send(client, on ? "/thirdperson on" : "/thirdperson off"); // offline: the sandbox server
}

void third_person_request(Client *client) {
    if (!client) return;
    g_tp.wanted = 1;
    g_tp.cancelled = 0;
    if (g_tp.granted) {
        ui_console_printf(NULL, "Third Person - Enabled");
        return;
    }
    if (!client->offline && !client->net_connected) {
        ui_console_printf("#ff6a6a", "thirdperson: not connected");
        return;
    }
    tp_send(client, 1);
}

void third_person_first_person(Client *client) {
    const int granted = g_tp.granted;
    g_tp.cancelled = g_tp.wanted && !granted; // a request in flight
    g_tp.wanted = g_tp.reask = g_tp.granted = 0;
    // the server answers "Third Person - Disabled" (its log follows); nothing granted: say it here
    if (granted && client) tp_send(client, 0);
    else ui_console_printf(NULL, "Third Person - Disabled");
}

void third_person_toggle(Client *client) {
    if (g_tp.wanted || g_tp.granted) third_person_first_person(client);
    else third_person_request(client);
}

void third_person_on_grant(Client *client, const int on) {
    (void) client;
    if (on && g_tp.cancelled) { // the answer to a request firstperson already took back
        g_tp.cancelled = 0;
        return;
    }
    // a grant also comes from "/thirdperson" typed in the chat: the camera follows the server
    g_tp.granted = g_tp.wanted = on;
    g_tp.cancelled = 0;
    if (!on) g_tp.reask = 0; // refused (or taken): no re-ask next round
}

void third_person_on_init(Client *client) {
    (void) client;
    g_tp.reask = g_tp.wanted;
    g_tp.granted = 0;
    prop_hunt_reset(); // startGame: the Prop team's camera distance back to 30
}

// The mode the local player's meshes are built for: granted. Aiming down sights keeps it (v10
// toggleAim / updateAim only zoom; the camera stays at thrdPX/PY/PZ), so only a grant changes it,
// and third_person_frame rebuilds the meshes before the frame's input, camera and passes.
int third_person_model(void) {
    return g_tp.granted;
}

// v10's own camera offsets (the game's thirdPerson setting, class camOffX/Y/Z, a camType other than
// FPS) are the class camera's (class_camera.h); this module stays out of the way there.
static int tp_game_offset(const Client *client) {
    const ClassCamera c = class_camera_current(client);
    return class_camera_third_person(&c);
}

// Prop Hunt: the Prop team's camera is always behind the eye (gameLoop :221701), at the distance the
// mouse wheel sets (prop_hunt_client.h), whatever the grant
static int tp_wanted(const Client *client) {
    return third_person_model() || prop_hunt_me(client);
}

int third_person_view(const Client *client) {
    const Player *me = client ? client->me : NULL;
    return tp_wanted(client) && me && me->active && me->mesh && !tp_game_offset(client) && !spectate_active();
}

void third_person_frame(Client *client) {
    Player *me = client->me;
    if (g_tp.reask && me && me->active && (client->offline || client->net_connected)) {
        g_tp.reask = 0;
        tp_send(client, 1);
    }
    if (!me || !me->active || !me->mesh) {
        g_tp.built = -1;
        g_tp.built_for = NULL;
        return;
    }
    const int mode = tp_wanted(client);
    if (g_tp.built_for == me && g_tp.built == mode) return;
    if (g_tp.built_for == me && g_tp.built != -1) {
        // the model changes: rebuild the local player's meshes (the same path as a loadout change); KrunkNative's own
        // rebuild, so a v10.0.3 class model keeps its asset (player_class_model.h)
        player_class_model_keep(me);
        client_drop_player_mesh(client, me);
        client_add_player_mesh(client, me, 1);
        g_tp.keep = 0.0f;
    }
    g_tp.built_for = me;
    g_tp.built = mode;
}

int third_person_draw_model(const Client *client) {
    return third_person_view(client) && g_tp.keep >= TP_MODEL_MIN_DIST;
}

static vec3 tp_eye(const Player *me) {
    return (vec3) {me->position.x, me->position.y + me->height - game_constants.camera_height, me->position.z};
}

static vec3 tp_offset(const Client *client) {
    // a prop: camera.position.set(0, 0, iîiïìïi) in the look frame (:221701)
    if (prop_hunt_me(client)) return (vec3) {0.0f, 0.0f, prop_hunt_camera_distance()};
    return (vec3) {TP_OFFSET_X, TP_OFFSET_Y, TP_OFFSET_Z};
}

void third_person_apply_camera(Client *client, const vec2 view, const float delta) {
    if (!third_person_view(client)) {
        g_tp.keep = 0.0f;
        return;
    }
    const Player *me = client->me;
    const vec3 eye = tp_eye(me);
    // camera_feel_apply's recoil / landing bob / shake ride on top of the view look
    const float pitch = player_clamp_pitch(view.x) + (client->camera.rotation.x - me->direction.x);
    const float yaw = view.y + (client->camera.rotation.y - me->direction.y);

    vec3 spot;
    const float keep = third_person_camera_spot(&client->game, eye, pitch, yaw, tp_offset(client), &spot);
    // pulled in at once (never inside a wall), eased back out once the wall is gone
    if (keep < g_tp.keep || g_tp.keep <= 0.0f) g_tp.keep = keep;
    else g_tp.keep += (keep - g_tp.keep) * fminf(1.0f, TP_EASE_OUT * fminf(delta, 0.1f) * 1000.0f);
    const float k = keep > 0.0f ? g_tp.keep / keep : 0.0f;

    client->camera.position = (vec3) {eye.x + (spot.x - eye.x) * k, eye.y + (spot.y - eye.y) * k, eye.z + (spot.z - eye.z) * k};
    client->camera.rotation.x = pitch;
    client->camera.rotation.y = yaw;
    // camera.zoom stays camera_feel_apply's ADS zoom: v10 renderer.zoom (updateAim, toggleAim's
    // scope zoom) narrows the world camera whatever its offset (v10.js 172050-172063, 199263)
}

// Our spawn: the camera distance starts over (pulled in at once, no ease out from the last life's).
void third_person_on_spawn(void) {
    g_tp.keep = 0.0f;
}

void third_person_note_frame(const int own_model, const int viewmodel) {
    g_tp.own_model = own_model;
    g_tp.viewmodel = viewmodel;
}

ThirdPersonFrame third_person_last_frame(const Client *client) {
    const int view = third_person_view(client);
    return (ThirdPersonFrame) {view, third_person_model(), g_tp.own_model, g_tp.viewmodel, view ? g_tp.keep : 0.0f};
}
