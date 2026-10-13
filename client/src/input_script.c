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
//   0 menu                       as the first event instead of play: the clock starts on the menu once
//                                the map is ready (menu clicks before any spawn, e.g. a comp lobby's
//                                team / class picks); a later `play` then enters the game
//   0 pose x y z yaw pitch       place the local player at rest (like tools/kref/movement-rec.js); offline
//                                the sandbox server's player too (the dev "tp")
//   0 tp x y z yaw pitch         online: the dev "tp" packet to feet x/y/z, then face yaw/pitch
//   100 down W | up W            keys through the key callback: W A S D SPACE SHIFT TAB ESC ENTER DELETE, any
//                                A-Z / 0-9, F1-F12
//   100 mdown right | mup right  mouse buttons (left / right), through the mouse button callback
//   100 cursor 640 360           move the cursor (window coordinates) through the cursor callback
//   100 click 640 360            cursor there, then a left press + release (e.g. a KrunkScript DIV)
//   100 clickid specBtn          the same at the centre of a page element's box (a display:none one: its
//                                nearest rendered ancestor, e.g. a switch's <label>)
//   100 look dyaw dpitch         turn the next input's look by these radians (CONTROLS rotation)
//   5000 respawn                 Play again when dead, as soon as the death delay allows (retried
//                                each frame; multi-life tests: server/tests/gunfight.sh)
//   100 wheel 1 | wheel -1       one mouse wheel event (up / down) through the scroll callback
//   100 blur                     the window loses focus (alt-tab)
//   100 state                    log the held weapon slot, the menu / pointer lock state, position, ammo, ended
//   100 con end                  a developer console command (the rest of the line: con bot_add 5)
//   100 vis @dead endUI chatHolder  log what the player sees of these page elements (html_dom_is_shown):
//                                "[input_script] vis t=100 @dead endUI=1 chatHolder=0" (- = no such id,
//                                @words are labels). Not on the page: :preview (the menu's class
//                                preview is drawn), :spray (the spray wheel is drawn);
//                                UI state transition checks (server/tests/ui_transitions_e2e.py)
//   1200 shot /tmp/a.png         write the frame (world + first-person layer) after this frame's input
//   1200 frames /tmp/f 120       write this and the next 119 finished frames (page included) as
//                                /tmp/f_0000.png ... (input_script_frame_presented)
//   3000 quit                    close the window
// Events run at the start of the first frame whose time is >= t, before the input is
// sampled, so a key held from t=0 to t=1500 is down for the inputs of [0, 1500).
#include <gl.h>
#include <input_script.h>
#include <app_mode.h>
#include <camera_feel.h>
#include <ui_logic.h>
#include <html_ui.h>
#include <console.h> // `con`: ui_console_exec
#include <sprays.h>  // `vis :spray`
#include <GLFW/glfw3.h>
#include <png_write.h>
#include <frame_target.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCRIPT_MAX_EVENTS 512

typedef struct {
    float t;          // ms
    char cmd[16];
    char arg[1024];
    float v[5];
} ScriptEvent;

static struct {
    int loaded, active;
    ScriptEvent events[SCRIPT_MAX_EVENTS];
    int count, next;
    int playing;      // `play` ran and the player is active: time runs (dead or alive)
    int menu_mode;    // the first event is `menu`: time runs from the menu, `play` just enters the game
    int respawn_pending; // `respawn` ran while dead: Play again as soon as the death delay allows
    char shot[160];   // pending `shot` path (written at frame end)
    char frames[160]; // `frames` prefix: one PNG per frame while frames_left > 0
    int frames_left, frames_index;
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

    char line[1200];
    while (fgets(line, sizeof(line), file) && g_script.count < SCRIPT_MAX_EVENTS) {
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;

        ScriptEvent *e = &g_script.events[g_script.count];
        memset(e, 0, sizeof(*e));
        char a[32] = {0}, b[32] = {0};
        const int n = sscanf(line, "%f %15s %1023s", &e->t, e->cmd, e->arg);
        strncpy(a, e->arg, sizeof(a) - 1);
        if (n < 2) continue;

        // numeric args follow the command; key/button names are kept as text
        const char *rest = strstr(line, e->cmd) + strlen(e->cmd);
        // (down / up always name a key: "down 4" is the 4 key, not a number)
        const int key_cmd = !strcmp(e->cmd, "down") || !strcmp(e->cmd, "up");
        if (n < 3 || (!key_cmd && ((a[0] >= '0' && a[0] <= '9') || a[0] == '-' || a[0] == '.'))) {
            e->arg[0] = 0;
            sscanf(rest, "%f %f %f %f %f", &e->v[0], &e->v[1], &e->v[2], &e->v[3], &e->v[4]);
        }
        (void) b;
        if (!strcmp(e->cmd, "con") || !strcmp(e->cmd, "vis")) { // the whole rest of the line: `con bot_add 5 difficulty=hard`
            while (*rest == ' ' || *rest == '\t') rest++;
            snprintf(e->arg, sizeof(e->arg), "%s", rest);
            for (size_t len = strlen(e->arg); len && (e->arg[len - 1] == '\n' || e->arg[len - 1] == '\r' || e->arg[len - 1] == ' '); len--) e->arg[len - 1] = 0;
        }
        if (!strcmp(e->cmd, "frames")) sscanf(rest, "%159s %f", e->arg, &e->v[0]); // frames <prefix> <count>
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
                "fov,wp_x,wp_y,wp_z,wr_x,wr_y,wr_z,ubp_y,ubr_x,obj_y,can_slide,idle_anim,arm_yaw_off,arm_pitch_off\n");
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
        {"F", GLFW_KEY_F}, {"G", GLFW_KEY_G}, {"T", GLFW_KEY_T}, {"TAB", GLFW_KEY_TAB}, {"DELETE", GLFW_KEY_DELETE},
    };
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        if (!strcmp(name, keys[i].name)) return keys[i].key;
    }
    // any other letter / digit, ESC and ENTER (KrunkScript key hooks)
    if (name[0] >= 'A' && name[0] <= 'Z' && !name[1]) return GLFW_KEY_A + (name[0] - 'A');
    if (name[0] >= '0' && name[0] <= '9' && !name[1]) return GLFW_KEY_0 + (name[0] - '0');
    if (!strcmp(name, "ESC")) return GLFW_KEY_ESCAPE;
    if (!strcmp(name, "ENTER")) return GLFW_KEY_ENTER;
    if (name[0] == 'F' && name[1] >= '1' && name[1] <= '9') { // F1..F12 (F5: the Reconnect bind)
        const int f = atoi(name + 1);
        if (f >= 1 && f <= 12) return GLFW_KEY_F1 + f - 1;
    }
    return GLFW_KEY_UNKNOWN;
}

static void script_run(Client *client, const ScriptEvent *e) {
    if (!strcmp(e->cmd, "down") || !strcmp(e->cmd, "up")) {
        // through the window's key callback (main.c: KrunkScript key hooks, then the binds)
        const int key = script_key(e->arg);
        const int action = e->cmd[0] == 'd' ? GLFW_PRESS : GLFW_RELEASE;
        GLFWkeyfun cb = glfwSetKeyCallback(client->window, NULL);
        glfwSetKeyCallback(client->window, cb);
        if (key != GLFW_KEY_UNKNOWN) {
            if (cb) cb(client->window, key, 0, action, 0);
            else ui_controls_key_event(client, key, 0, action, 0);
        }
    } else if (!strcmp(e->cmd, "mdown") || !strcmp(e->cmd, "mup")) {
        const int button = !strcmp(e->arg, "right") ? GLFW_MOUSE_BUTTON_RIGHT : GLFW_MOUSE_BUTTON_LEFT;
        const int action = e->cmd[1] == 'd' ? GLFW_PRESS : GLFW_RELEASE;
        GLFWmousebuttonfun cb = glfwSetMouseButtonCallback(client->window, NULL);
        glfwSetMouseButtonCallback(client->window, cb);
        if (cb) cb(client->window, button, action, 0);
        else ui_controls_mouse_event(client, button, action, 1);
    } else if (!strcmp(e->cmd, "cursor") || !strcmp(e->cmd, "click") || !strcmp(e->cmd, "clickid")) {
        // cursor x y / click x y: move the cursor (window coordinates) through the cursor
        // callback; click also presses and releases the left button there. clickid <id>: the
        // centre of that page element's box (CSS px -> window coordinates).
        double x = e->v[0], y = e->v[1];
        if (!strcmp(e->cmd, "clickid")) {
            HtmlElem el = html_ui_available() ? html_dom_by_id(e->arg) : 0;
            HtmlRect r = {0};
            while (el && !html_dom_get_rect(el, &r)) el = html_dom_parent(el); // a hidden checkbox: its <label>
            if (!el) {
                fprintf(stderr, "[input_script] clickid %s: not shown\n", e->arg);
                return;
            }
            float vw = 0.0f, vh = 0.0f;
            int ww = 0, wh = 0;
            html_dom_viewport(&vw, &vh, NULL);
            glfwGetWindowSize(client->window, &ww, &wh);
            x = (r.x + r.w * 0.5f) * (vw > 0.0f && ww > 0 ? ww / vw : 1.0f);
            y = (r.y + r.h * 0.5f) * (vh > 0.0f && wh > 0 ? wh / vh : 1.0f);
        }
        glfwSetCursorPos(client->window, x, y);
        GLFWcursorposfun pos = glfwSetCursorPosCallback(client->window, NULL);
        glfwSetCursorPosCallback(client->window, pos);
        if (pos) pos(client->window, x, y);
        if (e->cmd[1] == 'l') {
            GLFWmousebuttonfun cb = glfwSetMouseButtonCallback(client->window, NULL);
            glfwSetMouseButtonCallback(client->window, cb);
            if (cb) {
                cb(client->window, GLFW_MOUSE_BUTTON_LEFT, GLFW_PRESS, 0);
                cb(client->window, GLFW_MOUSE_BUTTON_LEFT, GLFW_RELEASE, 0);
            }
        }
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
        if (client->offline) { // the sandbox server's player follows (the dev "tp": we host it)
            const NetTeleportPacket tp = {e->v[0], e->v[1] + me->height, e->v[2]};
            client_net_send(client, NET_PACKET_TELEPORT, &tp, sizeof(tp));
        }
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
    } else if (!strcmp(e->cmd, "vis")) {
        // vis <id> [<id> ...]: one log line with each element's visibility (1 shown, 0 not, - absent)
        char line[2048];
        int n = snprintf(line, sizeof(line), "[input_script] vis t=%.0f", g_script.time);
        for (const char *c = e->arg; *c && n > 0 && (size_t) n < sizeof(line);) {
            while (*c == ' ' || *c == '\t') c++;
            char id[64];
            size_t len = 0;
            while (c[len] && c[len] != ' ' && c[len] != '\t') len++;
            if (!len) break;
            snprintf(id, sizeof(id), "%.*s", (int) (len < sizeof(id) ? len : sizeof(id) - 1), c);
            c += len;
            if (id[0] == '@') { // a label for the log reader
                n += snprintf(line + n, sizeof(line) - (size_t) n, " %s", id);
                continue;
            }
            if (!strcmp(id, ":preview") || !strcmp(id, ":spray")) {
                const ClassPreview *cp = &client->class_preview;
                const int shown = id[1] == 's' ? sprays_wheel_shown()
                    : html_ui_available() && html_ui_is_menu_open() && cp->has_rect && cp->visible && cp->width > 0.0f && cp->height > 0.0f;
                n += snprintf(line + n, sizeof(line) - (size_t) n, " %s=%d", id, shown);
                continue;
            }
            const HtmlElem el = html_ui_available() ? html_dom_by_id(id) : 0;
            n += snprintf(line + n, sizeof(line) - (size_t) n, " %s=%s", id, !el ? "-" : html_dom_is_shown(el) ? "1" : "0");
        }
        fprintf(stderr, "%s\n", line);
    } else if (!strcmp(e->cmd, "con")) {
        ui_console_exec(e->arg); // con <word>: a one-word developer console command (`end`)
    } else if (!strcmp(e->cmd, "blur")) {
        client_window_focus_lost(client); // alt-tab (the GLFW focus callback)
    } else if (!strcmp(e->cmd, "play") && g_script.menu_mode) {
        if (html_ui_available() && html_ui_is_menu_open()) client_set_menu_open(client, 0);
        client_enter_game(client);
    } else if (!strcmp(e->cmd, "shot")) {
        strncpy(g_script.shot, e->arg, sizeof(g_script.shot) - 1);
    } else if (!strcmp(e->cmd, "frames")) {
        // frames <prefix> <count>: write this and the next count-1 frames as <prefix>_NNNN.png
        snprintf(g_script.frames, sizeof(g_script.frames), "%s", e->arg);
        g_script.frames_left = e->v[0] >= 1.0f ? (int) e->v[0] : 1;
    } else if (!strcmp(e->cmd, "quit")) {
        if (g_script.log) fflush(g_script.log);
        glfwSetWindowShouldClose(client->window, 1);
    }
}

void input_script_tick(Client *client) {
    script_load();
    if (!g_script.active) return;

    if (!g_script.playing && g_script.count && !strcmp(g_script.events[0].cmd, "menu")) {
        if (!client->game.ready || (html_ui_is_ready() && !menu_ui()->dom_ready)) return;
        g_script.playing = g_script.menu_mode = 1;
        g_script.time = 0.0f;
        g_script.next = 1;
    }
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

    // A `vis` probe never shares a frame with the input before it: after a stalled frame the due
    // events would otherwise run together and the probe would read the page before the UI
    // reacted to them (the HUD / menu passes run later in the frame).
    int ran = 0;
    while (g_script.next < g_script.count && g_script.events[g_script.next].t <= g_script.time + 1e-3f) {
        const ScriptEvent *e = &g_script.events[g_script.next];
        if (ran && !strcmp(e->cmd, "vis") && strcmp(g_script.events[g_script.next - 1].cmd, "vis") != 0) break;
        script_run(client, e);
        g_script.next++;
        ran = 1;
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

// Reads the window's picture (frame_target_framebuffer(), bottom row first) and writes it top row first, opaque.
static void write_frame_png(Client *client, const char *path) {
    int width, height;
    glfwGetFramebufferSize(client->window, &width, &height);
    unsigned char *pixels = malloc((size_t) width * height * 4);
    unsigned char *flipped = malloc((size_t) width * height * 4);

    if (pixels && flipped) {
        glBindFramebuffer(GL_FRAMEBUFFER, frame_target_framebuffer()); // the window's picture (frame_target.h)
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

        const size_t row = (size_t) width * 4;
        for (int y = 0; y < height; y++) memcpy(flipped + (size_t) y * row, pixels + (size_t) (height - 1 - y) * row, row);
        for (size_t i = 3; i < row * (size_t) height; i += 4) flipped[i] = 255;
        png_write_rgba(path, flipped, width, height);
    }

    free(pixels);
    free(flipped);
}

static void write_pending_shot(Client *client) {
    if (!g_script.shot[0]) return;
    write_frame_png(client, g_script.shot);
    g_script.shot[0] = 0;
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
        // The viewmodel's look against the camera's: the arms turn with objInstances (anchor yaw)
        // and pitch with the upper body (direction.x), the camera adds shake, recoil and the
        // landing bob (camera_feel_apply). 0 = locked to the camera, as Krunker draws them.
        float shake_pitch, shake_yaw;
        camera_feel_shake_offset(&shake_pitch, &shake_yaw);
        const float yaw_off = an ? an->rotation.y - (c->rotation.y - shake_yaw) : 0.0f;
        const float arm_yaw_off = atan2f(sinf(yaw_off), cosf(yaw_off));
        const float arm_pitch_off = p->direction.x - (c->rotation.x - shake_pitch - p->recoil_anim_y * game_constants.recoil_mlt - p->land_bob_y * 0.1f);
        fprintf(g_script.log, "%.3f,%.3f,%.5f,%.5f,%.5f,%.6f,%.6f,%.6f,%d,%d,%.4f,%.4f,%.4f,%.4f,%.5f,%.5f,"
            "%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.4f,%.4f,%.4f,%.5f,%.5f,%.4f,%.5f,%.5f,%.5f,%u,%u,"
            "%.4f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%d,%.5f,%.6f,%.6f\n",
            g_script.time, delta * 1000.0f, p->position.x, p->position.y, p->position.z,
            p->velocity.x, p->velocity.y, p->velocity.z, p->on_ground, p->on_ladder, p->crouch_val, p->aim_val, p->height,
            p->slide_timer, p->step_val, p->step_chase, p->land_bob_y, p->land_bob_yr, p->jump_bob_y, p->jump_rotate,
            p->jump_rotate_mlt, p->bob_anim.y, p->bob_anim.z, p->lean_anim.x, p->lean_anim.y, p->lean_anim.z, p->recoil_anim_y,
            c->position.x, c->position.y, c->position.z, c->rotation.x, c->rotation.y, c->zoom,
            p->recoil_tween[0], p->recoil_tween[3], p->inspect_x, (unsigned) p->step_events, (unsigned) p->land_events,
            c->fov, vm ? vm->position.x : 0.0f, vm ? vm->position.y : 0.0f, vm ? vm->position.z : 0.0f,
            vm ? vm->rotation.x : 0.0f, vm ? vm->rotation.y : 0.0f, vm ? vm->rotation.z : 0.0f,
            ub ? ub->position.y : 0.0f, ub ? ub->rotation.x : 0.0f, an ? an->position.y : 0.0f, p->can_slide, p->idle_anim, arm_yaw_off, arm_pitch_off);
    }

    write_pending_shot(client);

    // The frame time, not the world's: the end-of-round slow motion scales the world delta
    // down to 0 for the whole results screen, which would hold every later event back.
    g_script.time += frame_delta * 1000.0f;
}

// Page windows (app_mode.c): the clock starts once the page is ready; game-only commands
// (play, pose, tp, look, respawn) are skipped.
void input_script_page_tick(Client *client) {
    script_load();
    if (!g_script.active) return;
    if (!g_script.playing) {
        if (!app_mode_page_ready(client)) return;
        g_script.playing = 1;
        g_script.time = 0.0f;
    }
    while (g_script.next < g_script.count && g_script.events[g_script.next].t <= g_script.time + 1e-3f) {
        const ScriptEvent *e = &g_script.events[g_script.next++];
        static const char *const GAME_ONLY[] = {"play", "pose", "tp", "look", "respawn"};
        int skip = 0;
        for (size_t i = 0; i < sizeof(GAME_ONLY) / sizeof(GAME_ONLY[0]); i++) skip |= !strcmp(e->cmd, GAME_ONLY[i]);
        if (!skip) script_run(client, e);
    }
}

void input_script_page_frame_end(Client *client, const float frame_delta) {
    if (!g_script.active || !g_script.playing) return;
    write_pending_shot(client);
    g_script.time += frame_delta * 1000.0f;
}

void input_script_frame_presented(Client *client) {
    if (!g_script.active || !g_script.playing || g_script.frames_left <= 0) return;
    char path[200];
    snprintf(path, sizeof(path), "%s_%04d.png", g_script.frames, g_script.frames_index++);
    write_frame_png(client, path);
    fprintf(stderr, "[input_script] frame %s\n", path); // orders the captures against the log
    g_script.frames_left--;
}
