// Scripted input for movement / first-person feel parity tests.
//
//   KRUNKNATIVE_INPUT_SCRIPT=<file>   timed input events (below)
//   KRUNKNATIVE_INPUT_LOG=<file>      one CSV row per frame from `play` on (header in the file)
//   KRUNKNATIVE_FIXED_DELTA=<ms>      every frame advances exactly this much time, so a slow host
//                                     (Xvfb/llvmpipe) replays at a chosen 30/60/144 fps step
//
// Script: one event per line, "<t_ms> <command> [args]", '#' starts a comment. Times count
// summed frame deltas (the round-end slow motion does not stretch them) from the frame the
// local player became active after `play`.
//   0 play                       close the menu and enter the game (waits for the map)
//   0 pose x y z yaw pitch       place the local player at rest (like tools/kref/movement-rec.js)
//   0 tp x y z yaw pitch         online: the dev "tp" packet to feet x/y/z, then face yaw/pitch
//   100 down W | up W            keys through the normal key path: W A S D SPACE SHIFT R E Q X 1 2 3 C N F G T TAB
//   100 mdown right | mup right  mouse buttons (left / right)
//   100 look dyaw dpitch         turn the next input's look by these radians (CONTROLS rotation)
//   5000 respawn                 Play again when dead, as soon as the death delay allows (retried
//                                each frame; multi-life tests: server/tests/gunfight.sh)
//   100 wheel 1 | wheel -1       one mouse wheel event (up / down) through the scroll callback
//   100 blur                     the window loses focus (alt-tab)
//   100 state                    log the held weapon slot, the menu / pointer lock state, position, ammo, ended
//   100 con end                  a developer console command (the rest of the line: con bot_add 5)
//   1200 shot /tmp/a.png         write the frame (world + first-person layer) after this frame's input
//   3000 quit                    close the window
// Events run at the start of the first frame whose time is >= t, before the input is
// sampled, so a key held from t=0 to t=1500 is down for the inputs of [0, 1500).
#include <gl.h>
#include <input_script.h>
#include <camera_feel.h>
#include <ui_logic.h>
#include <html_ui.h>
#include <console.h> // `con`: ui_console_exec
#include <GLFW/glfw3.h>
#include <png_write.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCRIPT_MAX_EVENTS 512

typedef struct {
    float t;          // ms
    char cmd[16];
    char arg[160];
    float v[5];
} ScriptEvent;

static struct {
    int loaded, active;
    ScriptEvent events[SCRIPT_MAX_EVENTS];
    int count, next;
    int playing;      // `play` ran and the player is active: time runs (dead or alive)
    int respawn_pending; // `respawn` ran while dead: Play again as soon as the death delay allows
    char shot[160];   // pending `shot` path (written at frame end)
    float time;       // ms since play
    float look_yaw, look_pitch;
    float fixed_delta;
    FILE *log;
} g_script;

static void script_load(void) {
    if (g_script.loaded) return;
    g_script.loaded = 1;

    const char *fixed = getenv("KRUNKNATIVE_FIXED_DELTA");
    if (fixed) g_script.fixed_delta = (float) atof(fixed) / 1000.0f;

    const char *path = getenv("KRUNKNATIVE_INPUT_SCRIPT");
    if (!path) return;

    FILE *file = fopen(path, "r");
    if (!file) {
        fprintf(stderr, "[input_script] cannot open %s\n", path);
        return;
    }

    char line[256];
    while (fgets(line, sizeof(line), file) && g_script.count < SCRIPT_MAX_EVENTS) {
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;

        ScriptEvent *e = &g_script.events[g_script.count];
        memset(e, 0, sizeof(*e));
        char a[32] = {0}, b[32] = {0};
        const int n = sscanf(line, "%f %15s %159s", &e->t, e->cmd, e->arg);
        strncpy(a, e->arg, sizeof(a) - 1);
        if (n < 2) continue;

        // numeric args follow the command; key/button names are kept as text
        const char *rest = strstr(line, e->cmd) + strlen(e->cmd);
        if (n < 3 || (a[0] >= '0' && a[0] <= '9') || a[0] == '-' || a[0] == '.') {
            e->arg[0] = 0;
            sscanf(rest, "%f %f %f %f %f", &e->v[0], &e->v[1], &e->v[2], &e->v[3], &e->v[4]);
        }
        (void) b;
        if (!strcmp(e->cmd, "con")) { // the whole rest of the line: `con bot_add 5 difficulty=hard`
            while (*rest == ' ' || *rest == '\t') rest++;
            snprintf(e->arg, sizeof(e->arg), "%s", rest);
            for (size_t len = strlen(e->arg); len && (e->arg[len - 1] == '\n' || e->arg[len - 1] == '\r' || e->arg[len - 1] == ' '); len--) e->arg[len - 1] = 0;
        }
        g_script.count++;
    }
    fclose(file);

    const char *log = getenv("KRUNKNATIVE_INPUT_LOG");
    if (log) {
        g_script.log = fopen(log, "w");
        if (g_script.log) {
            fprintf(g_script.log, "t,delta,x,y,z,vx,vy,vz,on_ground,on_ladder,crouch,aim,height,slide,step_val,step_chase,"
                "land_bob_y,land_bob_yr,jump_bob_y,jump_rotate,jump_rotate_mlt,bob_y,bob_z,lean_x,lean_y,lean_z,recoil_anim_y,"
                "cam_x,cam_y,cam_z,cam_pitch,cam_yaw,zoom,recoil_tween_y,recoil_tween_z,inspect_x,step_events,land_events,"
                "fov,wp_x,wp_y,wp_z,wr_x,wr_y,wr_z,ubp_y,ubr_x,obj_y,can_slide,idle_anim\n");
        }
    }

    g_script.active = 1;
    fprintf(stderr, "[input_script] %d events from %s%s\n", g_script.count, path, g_script.fixed_delta > 0.0f ? " (fixed delta)" : "");
}

float input_script_delta(const float delta) {
    script_load();
    return g_script.fixed_delta > 0.0f ? g_script.fixed_delta : delta;
}

static int script_key(const char *name) {
    static const struct { const char *name; int key; } keys[] = {
        {"W", GLFW_KEY_W}, {"A", GLFW_KEY_A}, {"S", GLFW_KEY_S}, {"D", GLFW_KEY_D},
        {"SPACE", GLFW_KEY_SPACE}, {"SHIFT", GLFW_KEY_LEFT_SHIFT}, {"R", GLFW_KEY_R}, {"E", GLFW_KEY_E},
        {"Q", GLFW_KEY_Q}, {"C", GLFW_KEY_C}, {"X", GLFW_KEY_X}, {"1", GLFW_KEY_1}, {"2", GLFW_KEY_2}, {"3", GLFW_KEY_3}, {"N", GLFW_KEY_N},
        {"F", GLFW_KEY_F}, {"G", GLFW_KEY_G}, {"T", GLFW_KEY_T}, {"TAB", GLFW_KEY_TAB},
    };
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        if (!strcmp(name, keys[i].name)) return keys[i].key;
    }
    return GLFW_KEY_UNKNOWN;
}

static void script_run(Client *client, const ScriptEvent *e) {
    if (!strcmp(e->cmd, "down") || !strcmp(e->cmd, "up")) {
        const int key = script_key(e->arg);
        if (key != GLFW_KEY_UNKNOWN) ui_controls_key_event(client, key, 0, e->cmd[0] == 'd' ? GLFW_PRESS : GLFW_RELEASE, 0);
    } else if (!strcmp(e->cmd, "mdown") || !strcmp(e->cmd, "mup")) {
        const int button = !strcmp(e->arg, "right") ? GLFW_MOUSE_BUTTON_RIGHT : GLFW_MOUSE_BUTTON_LEFT;
        ui_controls_mouse_event(client, button, e->cmd[1] == 'd' ? GLFW_PRESS : GLFW_RELEASE, 1);
    } else if (!strcmp(e->cmd, "look")) {
        g_script.look_yaw += e->v[0];
        g_script.look_pitch += e->v[1];
    } else if (!strcmp(e->cmd, "pose") && client->me) {
        Player *me = client->me;
        me->position = (vec3) {e->v[0], e->v[1], e->v[2]};
        me->last_position = me->position;
        me->velocity = (vec3) {0};
        me->direction = (vec2) {e->v[4], e->v[3]};
        me->on_ground = 1;
    } else if (!strcmp(e->cmd, "tp") && client->me) {
        // tp x y z yaw pitch: the dev "tp" (feet at x/y/z, like the mod's Ctrl TP) and a look
        // direction, so online runs (server_bin) can place both players (tools/kref/players-native.sh).
        const NetTeleportPacket tp = {e->v[0], e->v[1] + client->me->height, e->v[2]};
        client_net_send(client, NET_PACKET_TELEPORT, &tp, sizeof(tp));
        client->me->direction = (vec2) {e->v[4], e->v[3]};
    } else if (!strcmp(e->cmd, "respawn")) {
        // dead (death card / menu back): Play again, like `play`. The ENT waits for
        // config.deathDelay, so the request stays pending (retried every frame in
        // input_script_tick) until our SPAWN arrives; a no-op while alive.
        g_script.respawn_pending = !client->me || !client->me->active;
    } else if (!strcmp(e->cmd, "wheel")) {
        // wheel dy: one wheel event through the scroll path (+1 = up, -1 = down)
        GLFWscrollfun cb = glfwSetScrollCallback(client->window, NULL);
        glfwSetScrollCallback(client->window, cb);
        if (cb) cb(client->window, 0.0, e->v[0]);
    } else if (!strcmp(e->cmd, "state")) {
        // state: one log line with the held slot and the menu/lock state (scripted checks)
        const Player *me = client->me;
        fprintf(stderr, "[input_script] state t=%.0f slot=%d/%d weapon=%s menu=%d locked=%d pos=%.3f,%.3f,%.3f ammo=%d aim=%.2f ended=%d\n",
                g_script.time, me ? me->loadout_index : -1, me ? (int) me->loadout_size : 0,
                me && me->weapon ? me->weapon->name : "-", html_ui_available() && html_ui_is_menu_open(), client->mouse_state.locked,
                me ? me->position.x : 0.0f, me ? me->position.y : 0.0f, me ? me->position.z : 0.0f,
                me && me->ammo && me->loadout_size ? (int) me->ammo[me->loadout_index] : -1, me ? me->aim_val : 0.0f, client_round_ended(client));
    } else if (!strcmp(e->cmd, "con")) {
        ui_console_exec(e->arg); // con <word>: a one-word developer console command (`end`)
    } else if (!strcmp(e->cmd, "blur")) {
        client_window_focus_lost(client); // alt-tab (the GLFW focus callback)
    } else if (!strcmp(e->cmd, "shot")) {
        strncpy(g_script.shot, e->arg, sizeof(g_script.shot) - 1);
    } else if (!strcmp(e->cmd, "quit")) {
        if (g_script.log) fflush(g_script.log);
        glfwSetWindowShouldClose(client->window, 1);
    }
}

void input_script_tick(Client *client) {
    script_load();
    if (!g_script.active) return;

    if (!g_script.playing) {
        // `play` (first event, any time): enter the game once the map is ready and the menu
        // has set up its page (menu_dom_init loads the saved classindex/name: entering
        // before it spawned every scripted run as class 0).
        if (!client->game.ready) return;
        if (html_ui_is_ready() && !menu_ui()->dom_ready) return;
        if (!client->me || !client->me->active) {
            if (html_ui_available() && html_ui_is_menu_open()) client_set_menu_open(client, 0);
            client_enter_game(client);
            return;
        }
        g_script.playing = 1;
        g_script.time = 0.0f;
        while (g_script.next < g_script.count && !strcmp(g_script.events[g_script.next].cmd, "play")) g_script.next++;
    }

    while (g_script.next < g_script.count && g_script.events[g_script.next].t <= g_script.time + 1e-3f) {
        script_run(client, &g_script.events[g_script.next++]);
    }

    if (g_script.respawn_pending) {
        if (client->me && client->me->active) {
            g_script.respawn_pending = 0; // our SPAWN arrived
        } else if (!client->spawn_requested) {
            if (html_ui_available() && html_ui_is_menu_open()) client_set_menu_open(client, 0);
            client_enter_game(client); // no-op until config.deathDelay passed
        }
    }
}

void input_script_adjust_input(Client *client, Input *input) {
    (void) client;
    if (!g_script.active || (!g_script.look_yaw && !g_script.look_pitch)) return;
    input->y_dir += g_script.look_yaw;
    input->x_dir += g_script.look_pitch;
    g_script.look_yaw = g_script.look_pitch = 0.0f;
}

void input_script_frame_end(Client *client, const float delta, const float frame_delta) {
    if (!g_script.active || !g_script.playing) return;

    const Player *p = client->me;
    if (g_script.log && p) {
        const Camera *c = &client->camera;
        // viewmodel (weaponMeshes[weaponIndex]), upperBody and objInstances transforms
        const PlayerMesh *pm = p->mesh;
        const MeshTransform *vm = pm && p->loadout_index >= 0 && pm->arms[p->loadout_index] ? &pm->arms[p->loadout_index]->anchor : NULL;
        const MeshTransform *ub = pm ? pm->upper_body_anchor : NULL;
        const MeshTransform *an = pm ? pm->anchor : NULL;
        fprintf(g_script.log, "%.3f,%.3f,%.5f,%.5f,%.5f,%.6f,%.6f,%.6f,%d,%d,%.4f,%.4f,%.4f,%.4f,%.5f,%.5f,"
            "%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.4f,%.4f,%.4f,%.5f,%.5f,%.4f,%.5f,%.5f,%.5f,%u,%u,"
            "%.4f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%d,%.5f\n",
            g_script.time, delta * 1000.0f, p->position.x, p->position.y, p->position.z,
            p->velocity.x, p->velocity.y, p->velocity.z, p->on_ground, p->on_ladder, p->crouch_val, p->aim_val, p->height,
            p->slide_timer, p->step_val, p->step_chase, p->land_bob_y, p->land_bob_yr, p->jump_bob_y, p->jump_rotate,
            p->jump_rotate_mlt, p->bob_anim.y, p->bob_anim.z, p->lean_anim.x, p->lean_anim.y, p->lean_anim.z, p->recoil_anim_y,
            c->position.x, c->position.y, c->position.z, c->rotation.x, c->rotation.y, c->zoom,
            p->recoil_tween[0], p->recoil_tween[3], p->inspect_x, (unsigned) p->step_events, (unsigned) p->land_events,
            c->fov, vm ? vm->position.x : 0.0f, vm ? vm->position.y : 0.0f, vm ? vm->position.z : 0.0f,
            vm ? vm->rotation.x : 0.0f, vm ? vm->rotation.y : 0.0f, vm ? vm->rotation.z : 0.0f,
            ub ? ub->position.y : 0.0f, ub ? ub->rotation.x : 0.0f, an ? an->position.y : 0.0f, p->can_slide, p->idle_anim);
    }

    if (g_script.shot[0]) {
        int width, height;
        glfwGetFramebufferSize(client->window, &width, &height);
        unsigned char *pixels = malloc((size_t) width * height * 4);
        unsigned char *flipped = malloc((size_t) width * height * 4);

        if (pixels && flipped) {
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

            const size_t row = (size_t) width * 4;
            for (int y = 0; y < height; y++) memcpy(flipped + (size_t) y * row, pixels + (size_t) (height - 1 - y) * row, row);
            for (size_t i = 3; i < row * (size_t) height; i += 4) flipped[i] = 255;
            png_write_rgba(g_script.shot, flipped, width, height);
        }

        free(pixels);
        free(flipped);
        g_script.shot[0] = 0;
    }

    // The frame time, not the world's: the end-of-round slow motion scales the world delta
    // down to 0 for the whole results screen, which would hold every later event back.
    g_script.time += frame_delta * 1000.0f;
}
