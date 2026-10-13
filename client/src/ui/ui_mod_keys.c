// The rest of the v3.1.1 offline build's client mod keys
// (/home/user/jails/kru/src/Krunker_v3.1.1/mod/mods.ts), ported 1:1:
//   - the document keydown switch: F1 io.send("clearLog") + console.clear(), F4
//     location.href = "/", F6 window.open("editor.html"),
//     T toggles Ctrl TP (localStorage "ctrlTp", notify "Ctrl TP - Enabled/Disabled");
//     F1/F4/T are ignored while an INPUT/TEXTAREA has focus, F6 never is. Its F5
//     location.reload() is the Reconnect bind now (ui_controls.c reconnect_key: rebindable,
//     the console's `reconnect` natively, still location.reload() on the web);
//   - Ctrl TP: while it is enabled and ControlLeft is held, the canvas' left mousedown is
//     taken from the game (no shot) and teleports: a ray from the camera centre (far 6000)
//     against what the scene draws, minus the local player, then io.send("tp", x,
//     y + player.height, z); the matching mouseup is swallowed too.
// ui_mods.c (the shared mods.ts keydown, console agent) calls ui_mod_keys_keydown after
// its bind matching; ui_controls_mouse_event calls ui_mod_keys_ctrl_tp_mouse.
#include <ui_logic.h>
#include <app_mode.h>
#include <html_ui.h>
#include <GLFW/glfw3.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#define CTRL_TP_KEY "ctrlTp"
#define CTRL_TP_FAR 6000.0f // tpRaycaster.far

static struct {
    int loaded;
    int ctrl_tp_enabled; // window.ctrlTpEnabled
    int tp_press;        // the left mousedown was taken: its mouseup is swallowed too
} g_keys;

static void keys_load(void) {
    if (g_keys.loaded) return;
    g_keys.loaded = 1;
    char value[8] = "";
    g_keys.ctrl_tp_enabled = html_storage_get(CTRL_TP_KEY, value, sizeof(value)) >= 0 && !strcmp(value, "1");
}

void ui_mod_keys_reload(void) {
    g_keys.loaded = 0;
    keys_load();
}

static void toggle_ctrl_tp(void) {
    g_keys.ctrl_tp_enabled = !g_keys.ctrl_tp_enabled;
    html_storage_set(CTRL_TP_KEY, g_keys.ctrl_tp_enabled ? "1" : "0"); // String(+window.ctrlTpEnabled)
    const char *msg = g_keys.ctrl_tp_enabled ? "Ctrl TP - Enabled" : "Ctrl TP - Disabled";
    ui_notify(msg);
    printf("%s\n", msg);
}

int ui_mod_keys_keydown(Client *client, const int key, const int input) {
    keys_load();
    switch (key) {
        // F1 (mods.ts: clearLog + console.clear()) is the Game Settings bind now (ui_controls.c)
        case GLFW_KEY_F4:
            if (input) return 0;
            app_reload("/"); // location.href = "/"
            return 0;
        case GLFW_KEY_F6:
            printf("Creating editor window\n");
            app_route_url_target("editor.html", 1); // window.open("editor.html")
            return 0;
        case GLFW_KEY_T:
            if (input) return 0;
            toggle_ctrl_tp();
            return 0; // no preventDefault: T still reaches the game (primKey)
        default:
            return 0;
    }
}

// THREE.Raycaster.setFromCamera({0, 0}, camera) + intersectObject(scene) without the local
// player: the nearest drawn map object or other player's box along the view axis.
static int ctrl_tp_ray(const Client *client, vec3 *hit) {
    const Camera *camera = &client->camera;
    const float *m = camera->world_inverse_matrix; // row-major world -> view: row 2 is the view z axis
    const vec3 origin = camera->position;
    const vec3 dir = {-m[8] * CTRL_TP_FAR, -m[9] * CTRL_TP_FAR, -m[10] * CTRL_TP_FAR};
    const Game *game = &client->game;
    float best = 2.0f;
    for (size_t i = 0; game->map && i < game->map->object_count; i++) {
        const Object *object = game->map->objects[i];
        if (!object || !object->active || !object->mesh) continue; // visible meshes only
        const float t = line_in_rect(origin, dir, object->position, object->scale);
        if (t > 0.0f && t <= 1.0f && t < best) best = t;
    }
    for (size_t i = 0; i < game->player_count; i++) {
        const Player *other = game->players[i];
        if (!other || !other->active || other == client->me) continue;
        const vec3 box = {other->scale * 2.0f, other->height, other->scale * 2.0f};
        const float t = line_in_rect(origin, dir, other->position, box);
        if (t > 0.0f && t <= 1.0f && t < best) best = t;
    }
    if (best > 1.0f) return 0;
    *hit = (vec3) {origin.x + dir.x * best, origin.y + dir.y * best, origin.z + dir.z * best};
    return 1;
}

int ui_mod_keys_ctrl_tp_mouse(Client *client, const int button, const int action) {
    keys_load();
    if (!client || button != GLFW_MOUSE_BUTTON_LEFT) return 0;
    if (action != GLFW_PRESS) { // mouseup: swallowed after a Ctrl TP mousedown only
        const int taken = g_keys.tp_press;
        g_keys.tp_press = 0;
        return taken;
    }
    if (!g_keys.ctrl_tp_enabled || ui_text_input_focused()) return 0; // window.ctrlTp is never set from a field
    if (!client->window || glfwGetKey(client->window, GLFW_KEY_LEFT_CONTROL) != GLFW_PRESS) return 0; // window.ctrlTp
    g_keys.tp_press = 1;
    const Player *me = client->me;
    vec3 hit;
    if (me && me->active && ctrl_tp_ray(client, &hit)) {
        const NetTeleportPacket tp = {hit.x, hit.y + (me->height ? me->height : 11.0f), hit.z};
        client_net_send(client, NET_PACKET_TELEPORT, &tp, sizeof(tp)); // io.send("tp", x, y + height, z)
    }
    return 1;
}
