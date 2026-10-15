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
//   0 boot                       as the first event: the clock starts as soon as the menu page is set up, map
//                                or not (an official krunker.io join waits for its matchmaker's captcha
//                                before any map exists); a later `play` enters the game once a map is in
//   0 pose x y z yaw pitch       place the local player at rest (like tools/kref/movement-rec.js); offline
//                                the sandbox server's player too (the dev "tp")
//   0 tp x y z yaw pitch         online: the dev "tp" packet to feet x/y/z, then face yaw/pitch
//   100 down W | up W            keys through the key callback: W A S D SPACE SHIFT TAB ESC ENTER DELETE, any
//                                A-Z / 0-9, F1-F12, GRAVE (the dev console's `); CTRL+ / SHIFT+ / ALT+ prefixes
//                                set the modifier bits (down CTRL+C: copy the page selection)
//   100 mdown right | mup right  mouse buttons (left / right), through the mouse button callback
//   100 cursor 640 360           move the cursor (window coordinates) through the cursor callback
//   100 click 640 360            cursor there, then a left press + release (e.g. a KrunkScript DIV)
//   100 clickid specBtn          the same at the centre of a page element's box (a display:none one: its
//                                nearest rendered ancestor, e.g. a switch's <label>)
//   100 clicksel .skinCard[data-index]  the same at the centre of the LAST element matching a selector
//                                (the bottom card of a scrolled list)
//   100 cursorid knOmToolsList 0.98 0.05  the cursor (no click) at that fraction of a page element's box
//                                (default 0.5 0.5): a scroll container's bar, for a wheel or an mdown/mup
//                                drag of its thumb (ui_transitions_e2e.py om-tools)
//   100 look dyaw dpitch         turn the next input's look by these radians (CONTROLS rotation)
//   5000 respawn                 Play again when dead, as soon as the death delay allows (retried
//                                each frame; multi-life tests: server/tests/gunfight.sh)
//   100 wheel 1 | wheel -1       one mouse wheel event (up / down) through the scroll callback
//   100 blur                     the window loses focus (alt-tab)
//   100 nethit 0 | nethit 1      a NET_PACKET_HIT from the server (no target, no damage; 1 = the kill
//                                variant) through the client's event handler: the hit sound and
//                                marker, as when your shot lands (hitmarker tests, alive or dead)
//   100 resize 800 600           the window's size (glfwSetWindowSize; headless: the framebuffer-size
//                                event follows at the frame's end, like a window manager's resize)
//   100 om add 28 | om drop 20   an Object Manager Weapons-tab click without the window: left (add the
//                                extra loadout slot, in hand when it lands; again: take it out) / right
//                                (a pickup at the crosshair)
//   100 om tool 1                a Tools-pane pick (0 Deleter, 1 Duplicator, 2 Rotator, 3 Info, 4 Move,
//                                5 Resize, 6 Properties): sets the Tool Gun's tool and puts the Tool Gun in hand (fire it
//                                with `mdown left`); `om physgun 0` the Physics Gun tile, `om snap 2.5` (the step, 0 off) /
//                                `om release 0|1` the pane's Snap / Release, `om object 10` an Objects tile,
//                                `om npc 1` an NPCs tile (1 the Ghost),
//                                `om entity 14` an Entities tile (spawned at the crosshair floor point),
//                                `om aim 1` turns the look onto handle 1 of the btools selection
//   100 hose                     log the held hose (KrunkNative Piss) as drawn, world points: "[input_script] hose
//                                t=100 tip=x,y,z back=x,y,z hand=x,y,z cam=x,y,z screen_tip=u,v screen_hand=u,v"
//                                (hand: the trigger hand's centre; screen_*: window fraction, y down, -1 behind
//                                the camera; "none" without a hose)
//   100 pour                     log the Nettspend Purple Drank's pour-up as drawn (first person, while reloading it):
//                                "[input_script] pour t=100 strength=0.80 mouth=x,y,z rim=x,y,z screen_mouth=u,v
//                                screen_rim=u,v" (the bottle's mouth, the cup's rim; "none" otherwise)
//   100 laced 8000               Nettspend: start the laced visuals for 8000 ms as the server's KN_EXT_LACED would
//                                (nettspend.h); `laced` with no time logs the effect: "[input_script] laced t=100
//                                intensity=0.83 left=7400 time=0.600"
//   100 flags                    log each mirrored CTF flag (position, carrier, team), its crystal / stand meshes and
//                                its #tScoreM text: "[input_script] flag t=100 i=0 team=1 carrier=-1 pos=..."
//   100 tpstate                  log the KrunkNative third-person camera of the last frame (third_person.h):
//                                "[input_script] tpstate t=100 view=1 model=1 own=1 viewmodel=0 keep=14.9
//                                cam=x,y,z eye=x,y,z wall=0 rot=pitch,yaw dir=pitch,yaw zoom=1.00 aim=0.00"
//                                (own: the local model went into the world pass, viewmodel: the first-person
//                                pass drew, wall: the camera sits in a solid box or solid geometry lies between
//                                the eye and it, rot: the camera's rotation, dir: the player's look, zoom: the
//                                camera's ADS zoom, aim: aim_val)
//   100 cosm @game               log the store items the menu's class preview and our own player wear
//                                (hat, body, face, head, back, primary skin, melee: store indices, -1 none)
//   100 wsel @menu               log what the model build picked on the held slot (weapon, store skin, model /
//                                texture keys or the hose, tint, sight, charm, wrist) for the menu's class
//                                preview (who=preview) and our own player (who=me): "[input_script] wsel t=100
//                                @menu who=me slot=0 tp=0 w=1 skin=2592 model=weapons/weapon_2 ... attach=attach_1 ..."
//                                (tp: the third-person model set, the preview's)
//   100 cam @label               log the camera and our look: "[input_script] cam t=100 @label spawns=1 active=1
//                                rot=pitch,yaw,roll pos=x,y,z eye=x,y,z dir=pitch,yaw zoom=1 recoil=anim_y,anim land=bob_y
//                                lean=x,y,z aim=0 crouch=0 shake=pitch,yaw chase=0 spect=0 tp=0 keep=0" (spawns: our spawns
//                                so far; eye: where the first-person camera sits; chase: the death chase camera has a
//                                killer; keep: the third-person camera's distance). The first 5 finished frames after
//                                each of our own spawns log the same as "@spawn0".."@spawn4" (client/tests/spawn_camera_e2e.py)
//   100 players                  log every player of the session: "[input_script] players t=100 n=2 5:Name@x,y,z
//                                9:Guest@x,y,z*" (* us, - inactive)
//   100 state                    log the held weapon slot, the menu / pointer lock state, position, ammo, ended,
//                                the charge (chargeTime s, fullCharge), the reload and fire timers, the grapple / rope, the GLFW
//                                cursor mode (cursor=normal: the mouse shows, disabled: locked to the game)
//                                and the camera's look (yaw pitch, radians: the pointer-lock tests check that
//                                mouse motion only turns it while the pointer is locked)
//   100 sync                     log our player's server-set movement state (official lobbies, krunker_link.c):
//                                game.move_lock, lockPlayer, frozen, speed boost, noclip, team, health, the mode's
//                                active objective, position and yaw; then a "sync+" line: unlimited ammo,
//                                juggernaut, kranked timer, sharingIsCaring, canThrow and the loadout
//   100 con end                  a developer console command (the rest of the line: con bot_add 5)
//   100 vis @dead endUI chatHolder  log what the player sees of these page elements (html_dom_is_shown):
//                                "[input_script] vis t=100 @dead endUI=1 chatHolder=0" (- = no such id,
//                                @words are labels). Not on the page: :preview (the menu's class
//                                preview is drawn), :spray (the spray wheel is drawn), :objectives (the
//                                objective modes' overlay layer drew: hud_objectives.c), :cursor (the
//                                mouse cursor shows: GLFW cursor mode normal);
//                                UI state transition checks (server/tests/ui_transitions_e2e.py)
//   100 console @label           log what the player sees of the developer console (ui_console_probe):
//                                "[input_script] console t=100 @label open=1 built=1 page=index.html shown=1
//                                sheet=1 last=1 top=5 inside=1 rect=x,y,w,h vp=WxH" (console_visible_e2e.py)
//   100 page editor [query]      this window shows another page in place (app_set_mode: game, hub, editor,
//                                viewer); the clock stops until it is shown and ready (the game page: its
//                                menu set up), then the script goes on there (game-only commands are skipped
//                                on a page)
//   100 scroll @grid knOmGrid    log scroll containers' scrollTop (html_dom_get_scroll), like vis:
//                                "[input_script] scroll t=100 @grid knOmGrid=196.0" (- = no such id or
//                                not a scroll container)
//   100 rect @menu chatHolder uiBase  log page elements' boxes, like vis: "[input_script] rect t=100
//                                @menu chatHolder=86.8,653.2,239.1,55.0,413.0,95.0" = the viewport rect
//                                x,y,w,h (CSS px, getBoundingClientRect) then the layout size
//                                offsetWidth,offsetHeight (- = no such id or not rendered); the two
//                                sizes of a scaled #uiBase give its scale
//   100 call skinSelector 1      dispatch a page handler by name with numeric (or "quoted" string) arguments,
//                                as an inline onclick="skinSelector(1)" would (windows without a menu button;
//                                call setSetting "seek_modes" "1")
//   100 setscroll skinList 4000  scrollTop of a scroll container (html_dom_set_scroll, clamped)
//   100 hit @label 640 360       log the page element at that point (window coordinates): its closest
//                                [data-index] ancestor's data-index and box, "[input_script] hit t=100
//                                @label index=12 rect=x,y,w,h" (index=- when there is none)
//   100 rects @label .skinCard   log every match's data-index and box in window coordinates:
//                                "[input_script] rects t=100 @label n=3 2482@10,20,163,197 -@..." (tests of
//                                the virtual card grids, client/src/ui/ui_vgrid.c)
//   100 nodes @label .skinCard   log the page's element count and how many match the selector
//   100 text @label #contjump    log the first match's text, value and checked state: "[input_script]
//                                text t=100 @label n=1 checked=0 value= text=K" (settings / binds tests)
//   100 grep @label Unknown [id=killFeed]  how many matches' text contains the word (case-insensitive):
//                                "[input_script] grep t=100 @label needle=Unknown n=1 hits=0"
//   100 glyphs @label #menuWindow  log how many characters every match's rendered text draws without a glyph
//                                (tofu, html_dom_missing_glyphs) and the first such code point: "[input_script]
//                                glyphs t=100 @label n=1 missing=0 first=-" (localisation tests: i18n_e2e.py)
//   100 locale @label            log the active language (i18n.h): "[input_script] locale t=100 @label code=ru
//                                gen=3 keys=1009"
//   100 ft @label start | stop   wall-clock frame times between start and stop: one line with the
//                                frame count, avg / p50 / p95 / max ms (perf logs, never asserted here)
//   100 type rifle               type the rest of the line through the char callback (the focused
//                                text field; the menu or the chat must have the keyboard)
//   100 clip @copied             log the system clipboard (GLFW): "[input_script] clip t=100 @copied
//                                text=<C-escaped clipboard>" (selection + Ctrl+C tests)
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
#include <hud_modes.h>  // `flags`
#include <mode_world.h> // `flags`
#include <object_manager.h> // `om object` (OM_TAB_OBJECTS)
#include <third_person.h> // `tpstate`
#include <world.h>        // `tpstate`: world_solid_fraction
#include <bots.h>         // `tpstate`: bot_object_blocks_player
#include <menu_view.h>    // `cam`: menu_view_killer_uid (the death chase)
#include <spectate.h>     // `cam`: spectate_active
#include <nettspend.h>    // `pour`, `laced`
#include <ruleset.h>      // `class`: the class names
#include <GLFW/glfw3.h>
#include <png_write.h>
#include <frame_target.h>
#include <math.h>
#include <stdio.h>
#include <ctype.h>
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
    int boot_mode;    // the first event is `boot`: time runs from the page, before any map
    int play_pending; // boot mode's `play`: enter the game once the map is ready (retried per frame)
    int respawn_pending; // `respawn` ran while dead: Play again as soon as the death delay allows
    int page_wait;    // `page`: 1 until that page is shown and ready, -1 one more frame (its UI ticks), 0 none
    AppMode page_mode;
    char shot[160];   // pending `shot` path (written at frame end)
    char frames[160]; // `frames` prefix: one PNG per frame while frames_left > 0
    int frames_left, frames_index;
    float time;       // ms since play
    float look_yaw, look_pitch;
    float fixed_delta;
    FILE *log;
    const Player *spawn_me; // `cam`: our player as of the last finished frame (NULL / dead: none)
    int spawn_frame, spawns; // frames logged since our last spawn (-1 none pending), spawns seen
    double ft_last;   // `ft`: wall clock of the previous frame (s), 0 = not measuring
    float *ft_ms;     // frame times collected since `ft start`
    int ft_n, ft_cap;
} g_script;

// The page probes: log lines, the whole rest of the line is ids (and @labels).
static int script_probe(const char *cmd) {
    return !strcmp(cmd, "vis") || !strcmp(cmd, "scroll") || !strcmp(cmd, "rect") || !strcmp(cmd, "console");
}

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
        if (!strcmp(e->cmd, "con") || script_probe(e->cmd) || !strcmp(e->cmd, "type") || !strcmp(e->cmd, "om") ||
            !strcmp(e->cmd, "call") || !strcmp(e->cmd, "setscroll") || !strcmp(e->cmd, "hit") || !strcmp(e->cmd, "ft") ||
            !strcmp(e->cmd, "nodes") || !strcmp(e->cmd, "rects") || !strcmp(e->cmd, "clicksel") || !strcmp(e->cmd, "wsel") ||
            !strcmp(e->cmd, "text") || !strcmp(e->cmd, "grep") || !strcmp(e->cmd, "glyphs") || !strcmp(e->cmd, "locale") || !strcmp(e->cmd, "page")) { // the whole rest of the line: `con bot_add 5 difficulty=hard`
            while (*rest == ' ' || *rest == '\t') rest++;
            snprintf(e->arg, sizeof(e->arg), "%s", rest);
            for (size_t len = strlen(e->arg); len && (e->arg[len - 1] == '\n' || e->arg[len - 1] == '\r' || e->arg[len - 1] == ' '); len--) e->arg[len - 1] = 0;
        }
        if (!strcmp(e->cmd, "frames")) sscanf(rest, "%159s %f", e->arg, &e->v[0]); // frames <prefix> <count>
        if (!strcmp(e->cmd, "cursorid")) { // cursorid <id> [fx fy]
            e->v[0] = e->v[1] = 0.5f;
            sscanf(rest, "%1023s %f %f", e->arg, &e->v[0], &e->v[1]);
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

int input_script_fixed_delta(void) {
    script_load();
    return g_script.fixed_delta > 0.0f;
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
    if (!strcmp(name, "GRAVE")) return GLFW_KEY_GRAVE_ACCENT;
    if (!strcmp(name, "ENTER")) return GLFW_KEY_ENTER;
    if (name[0] == 'F' && name[1] >= '1' && name[1] <= '9') { // F1..F12 (F5: the Reconnect bind)
        const int f = atoi(name + 1);
        if (f >= 1 && f <= 12) return GLFW_KEY_F1 + f - 1;
    }
    return GLFW_KEY_UNKNOWN;
}

// window fraction (0..1, y down) of a world point through the client camera, as drawn (vfx.c project_to_screen)
static vec2 script_project(Client *client, const vec3 p) {
    int viewport[4];
    glGetIntegerv(GL_VIEWPORT, viewport);
    Camera *camera = &client->camera;
    camera_update_projection_matrix(camera, viewport[3] > 0 ? (float) viewport[2] / (float) viewport[3] : 1.0f);
    camera_update_world_inverse_matrix(camera);
    const float in[4] = {p.x, p.y, p.z, 1.0f};
    float view[4], clip[4];
    for (int r = 0; r < 4; r++) {
        view[r] = 0.0f;
        for (int c = 0; c < 4; c++) view[r] += camera->world_inverse_matrix[r * 4 + c] * in[c];
    }
    for (int r = 0; r < 4; r++) {
        clip[r] = 0.0f;
        for (int c = 0; c < 4; c++) clip[r] += camera->projection_matrix[r * 4 + c] * view[c];
    }
    if (clip[3] <= 0.0f) return (vec2) {-1.0f, -1.0f}; // behind the camera
    return (vec2) {(clip[0] / clip[3] + 1.0f) * 0.5f, 1.0f - (clip[1] / clip[3] + 1.0f) * 0.5f};
}

// `cam` and the frames after our spawns: the camera as posed this frame and our player's look feel.
static void script_log_cam(const Client *client, const char *label) {
    const Player *me = client->me;
    const Camera *c = &client->camera;
    float shake_pitch, shake_yaw;
    camera_feel_shake_offset(&shake_pitch, &shake_yaw);
    const vec3 eye = me ? (vec3) {me->position.x, me->position.y + me->height - game_constants.camera_height, me->position.z} : c->position;
    fprintf(stderr, "[input_script] cam t=%.0f %s spawns=%d active=%d rot=%.5f,%.5f,%.5f pos=%.3f,%.3f,%.3f eye=%.3f,%.3f,%.3f dir=%.5f,%.5f "
            "zoom=%.4f recoil=%.5f,%.5f land=%.5f lean=%.5f,%.5f,%.5f aim=%.4f crouch=%.4f shake=%.5f,%.5f chase=%d spect=%d tp=%d keep=%.3f\n",
            g_script.time, label, g_script.spawns, me ? me->active : 0, c->rotation.x, c->rotation.y, c->rotation.z, c->position.x, c->position.y,
            c->position.z, eye.x, eye.y, eye.z, me ? me->direction.x : 0.0f, me ? me->direction.y : 0.0f, c->zoom,
            me ? me->recoil_anim_y : 0.0f, me ? me->recoil_anim : 0.0f, me ? me->land_bob_y : 0.0f, me ? me->lean_anim.x : 0.0f,
            me ? me->lean_anim.y : 0.0f, me ? me->lean_anim.z : 0.0f, me ? me->aim_val : 0.0f, me ? me->crouch_val : 0.0f,
            shake_pitch, shake_yaw, menu_view_killer_uid() >= 0, spectate_active(), third_person_view(client),
            third_person_last_frame(client).keep);
}

static void script_run(Client *client, const ScriptEvent *e) {
    if (!strcmp(e->cmd, "down") || !strcmp(e->cmd, "up")) {
        // through the window's key callback (main.c: KrunkScript key hooks, then the binds)
        const char *name = e->arg;
        int mods = 0;
        for (;;) { // CTRL+ / SHIFT+ / ALT+ prefixes
            if (!strncmp(name, "CTRL+", 5)) { mods |= GLFW_MOD_CONTROL; name += 5; }
            else if (!strncmp(name, "SHIFT+", 6)) { mods |= GLFW_MOD_SHIFT; name += 6; }
            else if (!strncmp(name, "ALT+", 4)) { mods |= GLFW_MOD_ALT; name += 4; }
            else break;
        }
        const int key = script_key(name);
        const int action = e->cmd[0] == 'd' ? GLFW_PRESS : GLFW_RELEASE;
        GLFWkeyfun cb = glfwSetKeyCallback(client->window, NULL);
        glfwSetKeyCallback(client->window, cb);
        if (key != GLFW_KEY_UNKNOWN) {
            if (cb) cb(client->window, key, 0, action, mods);
            else ui_controls_key_event(client, key, 0, action, mods);
        }
    } else if (!strcmp(e->cmd, "mdown") || !strcmp(e->cmd, "mup")) {
        const int button = !strcmp(e->arg, "right") ? GLFW_MOUSE_BUTTON_RIGHT : GLFW_MOUSE_BUTTON_LEFT;
        const int action = e->cmd[1] == 'd' ? GLFW_PRESS : GLFW_RELEASE;
        GLFWmousebuttonfun cb = glfwSetMouseButtonCallback(client->window, NULL);
        glfwSetMouseButtonCallback(client->window, cb);
        if (cb) cb(client->window, button, action, 0);
        else ui_controls_mouse_event(client, button, action, 1);
    } else if (!strcmp(e->cmd, "cursor") || !strcmp(e->cmd, "click") || !strcmp(e->cmd, "clickid") || !strcmp(e->cmd, "clicksel") ||
               !strcmp(e->cmd, "cursorid")) {
        // cursor x y / click x y: move the cursor (window coordinates) through the cursor
        // callback; click also presses and releases the left button there. clickid <id>: the
        // centre of that page element's box (CSS px -> window coordinates); cursorid <id> fx fy:
        // that fraction of it, no click.
        double x = e->v[0], y = e->v[1];
        const int by_id = !strcmp(e->cmd, "clickid") || !strcmp(e->cmd, "clicksel") || !strcmp(e->cmd, "cursorid");
        const float fx = e->cmd[1] == 'u' ? e->v[0] : 0.5f, fy = e->cmd[1] == 'u' ? e->v[1] : 0.5f;
        if (by_id) {
            HtmlElem el = 0;
            if (e->cmd[5] == 's') { // clicksel: the last match
                HtmlElem all[256];
                const int n = html_ui_available() ? html_dom_query_all(0, e->arg, all, 256) : 0;
                el = n > 0 ? all[(n < 256 ? n : 256) - 1] : 0;
            } else {
                el = html_ui_available() ? html_dom_by_id(e->arg) : 0;
            }
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
            x = (r.x + r.w * fx) * (vw > 0.0f && ww > 0 ? ww / vw : 1.0f);
            y = (r.y + r.h * fy) * (vh > 0.0f && wh > 0 ? wh / vh : 1.0f);
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
    } else if (!strcmp(e->cmd, "skin")) {
        // skin <index>: the local player found a skin during the match (NET_PACKET_SKIN_FOUND's "skin" path,
        // hud_ui_skin_found). A match's rewards have no offline source, so the end-screen e2e drives this
        // (v10_endscreen_e2e.py): the skin is held for the Collect Rewards button (hud_endscreen_v10.c).
        if (client->me) hud_ui_skin_found(client, client->me->uid, (int) e->v[0]);
        fprintf(stderr, "[input_script] skin t=%.0f %d\n", g_script.time, (int) e->v[0]);
    } else if (!strcmp(e->cmd, "wheel")) {
        // wheel dy: one wheel event through the scroll path (+1 = up, -1 = down)
        GLFWscrollfun cb = glfwSetScrollCallback(client->window, NULL);
        glfwSetScrollCallback(client->window, cb);
        if (cb) cb(client->window, 0.0, e->v[0]);
    } else if (!strcmp(e->cmd, "call")) {
        // call <handler> [n | "text" ...]: the inline handler by name, numeric or double-quoted
        // string arguments (call setSetting "seek_modes" "1")
        char name[64] = "";
        HtmlArg args[8];
        char strs[8][128];
        int argc = 0, used = 0;
        if (sscanf(e->arg, "%63s%n", name, &used) == 1) {
            const char *c = e->arg + used;
            double v;
            int len;
            for (;;) {
                while (*c == ' ' || *c == '\t') c++;
                if (argc >= 8 || !*c) break;
                if (*c == '"') {
                    const char *end = strchr(c + 1, '"');
                    if (!end) break;
                    snprintf(strs[argc], sizeof(strs[argc]), "%.*s", (int) (end - c - 1), c + 1);
                    args[argc] = (HtmlArg) {HTML_ARG_STR, 0, strs[argc]};
                    argc++;
                    c = end + 1;
                } else if (sscanf(c, "%lf%n", &v, &len) == 1) {
                    args[argc++] = (HtmlArg) {HTML_ARG_NUM, v, NULL};
                    c += len;
                } else {
                    break;
                }
            }
            const HtmlEvent ev = {.type = "click", .value = ""};
            if (html_ui_available()) html_ui_dispatch(name, &ev, args, argc);
        }
        fprintf(stderr, "[input_script] call t=%.0f %s argc=%d\n", g_script.time, name, argc);
    } else if (!strcmp(e->cmd, "setscroll")) {
        char id[64] = "";
        float top = 0.0f;
        sscanf(e->arg, "%63s %f", id, &top);
        const HtmlElem el = html_ui_available() ? html_dom_by_id(id) : 0;
        if (el) html_dom_set_scroll(el, 0.0f, top);
        fprintf(stderr, "[input_script] setscroll t=%.0f %s=%.1f%s\n", g_script.time, id, top, el ? "" : " (no such id)");
    } else if (!strcmp(e->cmd, "hit")) {
        // hit @label x y: the [data-index] card under a window point (CSS px through the viewport scale)
        char label[64] = "";
        float x = 0.0f, y = 0.0f, vw = 0.0f, vh = 0.0f;
        int ww = 0, wh = 0;
        sscanf(e->arg, "%63s %f %f", label, &x, &y);
        html_dom_viewport(&vw, &vh, NULL);
        glfwGetWindowSize(client->window, &ww, &wh);
        const float cx = x * (vw > 0.0f && ww > 0 ? vw / (float) ww : 1.0f), cy = y * (vh > 0.0f && wh > 0 ? vh / (float) wh : 1.0f);
        HtmlElem el = html_ui_available() ? html_dom_element_at(cx, cy) : 0;
        el = el ? html_dom_closest(el, "[data-index]") : 0;
        char index[32] = "-";
        HtmlRect r = {0};
        if (el) {
            if (html_dom_get_attr(el, "data-index", index, sizeof(index)) < 0) snprintf(index, sizeof(index), "-");
            html_dom_get_rect(el, &r);
        }
        fprintf(stderr, "[input_script] hit t=%.0f %s index=%s rect=%.0f,%.0f,%.0f,%.0f\n", g_script.time, label, index, r.x, r.y, r.w, r.h);
    } else if (!strcmp(e->cmd, "rects")) {
        char label[64] = "", sel[192] = "";
        sscanf(e->arg, "%63s %191[^\n]", label, sel);
        HtmlElem els[96];
        const int n = html_ui_available() && sel[0] ? html_dom_query_all(0, sel, els, 96) : 0;
        float vw = 0.0f, vh = 0.0f;
        int ww = 0, wh = 0;
        html_dom_viewport(&vw, &vh, NULL);
        glfwGetWindowSize(client->window, &ww, &wh);
        const float kx = vw > 0.0f && ww > 0 ? ww / vw : 1.0f, ky = vh > 0.0f && wh > 0 ? wh / vh : 1.0f;
        char line[8192];
        int len = snprintf(line, sizeof(line), "[input_script] rects t=%.0f %s n=%d", g_script.time, label, n);
        for (int i = 0; i < n && i < 96 && len > 0 && (size_t) len < sizeof(line) - 64; i++) {
            char index[16] = "-";
            HtmlRect r = {0};
            if (html_dom_get_attr(els[i], "data-index", index, sizeof(index)) < 0) snprintf(index, sizeof(index), "-");
            html_dom_get_rect(els[i], &r);
            len += snprintf(line + len, sizeof(line) - (size_t) len, " %s@%.1f,%.1f,%.1f,%.1f", index, r.x * kx, r.y * ky, r.w * kx, r.h * ky);
        }
        fprintf(stderr, "%s\n", line);
    } else if (!strcmp(e->cmd, "nodes")) {
        char label[64] = "", sel[192] = "";
        sscanf(e->arg, "%63s %191[^\n]", label, sel);
        const int all = html_ui_available() ? html_dom_query_all(0, "*", NULL, 0) : 0;
        const int match = html_ui_available() && sel[0] ? html_dom_query_all(0, sel, NULL, 0) : 0;
        fprintf(stderr, "[input_script] nodes t=%.0f %s elements=%d match=%d\n", g_script.time, label, all, match);
    } else if (!strcmp(e->cmd, "text")) {
        // text @label <selector>: the first match's textContent (whitespace runs folded, 160 chars),
        // value and checked state, and how many elements match
        char label[64] = "", sel[192] = "", text[512] = "", value[160] = "", folded[164];
        sscanf(e->arg, "%63s %191[^\n]", label, sel);
        const int n = html_ui_available() && sel[0] ? html_dom_query_all(0, sel, NULL, 0) : 0;
        const HtmlElem el = n > 0 ? html_dom_query(0, sel) : 0;
        if (el) {
            html_dom_get_text(el, text, sizeof(text));
            if (html_dom_get_value(el, value, sizeof(value)) < 0) value[0] = 0;
        }
        size_t k = 0;
        for (const char *s = text; *s && k + 1 < sizeof(folded) - 3; s++) {
            const int space = *s == ' ' || *s == '\n' || *s == '\t' || *s == '\r';
            if (space && (!k || folded[k - 1] == ' ')) continue;
            folded[k++] = space ? ' ' : *s;
        }
        while (k && folded[k - 1] == ' ') k--;
        folded[k] = 0;
        fprintf(stderr, "[input_script] text t=%.0f %s n=%d checked=%d value=%s text=%s\n", g_script.time, label, n,
                el ? html_dom_get_checked(el) : 0, value, folded);
    } else if (!strcmp(e->cmd, "grep")) {
        // grep @label <needle> <selector>: how many matches' textContent contains the needle
        // (case-insensitive; "Unknown" labels on the KN weapons' tiles and feed rows)
        char label[64] = "", needle[64] = "", sel[192] = "";
        sscanf(e->arg, "%63s %63s %191[^\n]", label, needle, sel);
        HtmlElem els[96];
        const int n = html_ui_available() && sel[0] ? html_dom_query_all(0, sel, els, 96) : 0;
        int hits = 0;
        char *text = malloc(65536);
        for (int i = 0; text && i < n && i < 96; i++) {
            html_dom_get_text(els[i], text, 65536);
            for (char *c = text; *c; c++) *c = (char) tolower((unsigned char) *c);
            char low[64];
            size_t k = 0;
            for (; needle[k] && k + 1 < sizeof(low); k++) low[k] = (char) tolower((unsigned char) needle[k]);
            low[k] = 0;
            if (low[0] && strstr(text, low)) hits++;
        }
        free(text);
        fprintf(stderr, "[input_script] grep t=%.0f %s needle=%s n=%d hits=%d\n", g_script.time, label, needle, n, hits);
    } else if (!strcmp(e->cmd, "glyphs")) {
        char label[64] = "", sel[192] = "";
        sscanf(e->arg, "%63s %191[^\n]", label, sel);
        HtmlElem els[64];
        const int n = html_ui_available() && sel[0] ? html_dom_query_all(0, sel, els, 64) : 0;
        int missing = 0;
        uint32_t first = 0;
        for (int i = 0; i < n; i++) {
            uint32_t f = 0;
            const int m = html_dom_missing_glyphs(els[i], &f);
            if (m < 0) { missing = -1; break; }
            if (m && !first) first = f;
            missing += m;
        }
        char cp[16] = "-";
        if (first) snprintf(cp, sizeof(cp), "U+%04X", (unsigned) first);
        fprintf(stderr, "[input_script] glyphs t=%.0f %s n=%d missing=%d first=%s\n", g_script.time, label, n, missing, cp);
    } else if (!strcmp(e->cmd, "locale")) {
        char label[64] = "";
        sscanf(e->arg, "%63s", label);
        fprintf(stderr, "[input_script] locale t=%.0f %s code=%s gen=%u keys=%d\n", g_script.time, label, i18n_locale(),
                i18n_generation(), i18n_table_size(i18n_locale()));
    } else if (!strcmp(e->cmd, "ft")) {
        // ft @label start|stop: wall-clock frame times (input_script_tick samples them)
        char label[64] = "", what[16] = "";
        sscanf(e->arg, "%63s %15s", label, what);
        if (!strcmp(what, "start")) {
            g_script.ft_last = glfwGetTime();
            g_script.ft_n = 0;
        } else if (g_script.ft_last > 0.0) {
            g_script.ft_last = 0.0;
            float sum = 0.0f;
            for (int i = 0; i < g_script.ft_n; i++) sum += g_script.ft_ms[i];
            // insertion sort: a few thousand samples at most
            for (int i = 1; i < g_script.ft_n; i++) {
                const float v = g_script.ft_ms[i];
                int j = i - 1;
                while (j >= 0 && g_script.ft_ms[j] > v) { g_script.ft_ms[j + 1] = g_script.ft_ms[j]; j--; }
                g_script.ft_ms[j + 1] = v;
            }
            const int n = g_script.ft_n;
            fprintf(stderr, "[input_script] ft t=%.0f %s frames=%d avg=%.2f p50=%.2f p95=%.2f max=%.2f ms\n", g_script.time, label, n,
                    n ? sum / n : 0.0f, n ? g_script.ft_ms[n / 2] : 0.0f, n ? g_script.ft_ms[(n * 95) / 100 < n ? (n * 95) / 100 : n - 1] : 0.0f,
                    n ? g_script.ft_ms[n - 1] : 0.0f);
        }
    } else if (!strcmp(e->cmd, "om")) {
        char what[16] = "";
        int id = -1;
        if (sscanf(e->arg, "%15s", what) == 1 && sscanf(strstr(e->arg, what) + strlen(what), "%d", &id) == 1 && id >= 0) {
            if (!strcmp(what, "tool")) object_manager_ui_pick_tool(id);
            else if (!strcmp(what, "physgun")) object_manager_ui_pick_physgun();
            else if (!strcmp(what, "object")) object_manager_ui_pick_entry(OM_TAB_OBJECTS, id);
            else if (!strcmp(what, "npc")) object_manager_ui_pick_entry(OM_TAB_NPCS, id);
            else if (!strcmp(what, "entity")) object_manager_ui_pick_entry(OM_TAB_ENTITIES, id);
            else if (!strcmp(what, "snap")) { // the grid step in units (decimals too: `om snap 0.5`), 0 off
                om_build_set_snap((float) atof(strstr(e->arg, what) + strlen(what)));
                object_manager_ui_snap_changed();
            }
            else if (!strcmp(what, "release")) om_build_set_drop(id);
            else if (!strcmp(what, "aim") && client->me) {
                float pitch, yaw;
                if (om_build_handle_look(client, id, &pitch, &yaw)) {
                    g_script.look_yaw += remainderf(yaw - client->me->direction.y, 2.0f * (float) M_PI);
                    g_script.look_pitch += pitch - client->me->direction.x;
                }
            } else object_manager_ui_pick_weapon(id, !strcmp(what, "drop"));
        }
        fprintf(stderr, "[input_script] om t=%.0f %s %d\n", g_script.time, what, id);
    } else if (!strcmp(e->cmd, "cam")) {
        script_log_cam(client, e->arg[0] ? e->arg : "-");
    } else if (!strcmp(e->cmd, "players")) {
        // players: every player of the session, "uid:name@x,y,z" (+ "*" for us, "-" inactive)
        char line[2048];
        size_t o = 0;
        for (size_t i = 0; i < client->game.player_count && o + 128 < sizeof(line); i++) {
            const Player *p = client->game.players[i];
            if (!p) continue;
            o += (size_t) snprintf(line + o, sizeof(line) - o, " %d:%s@%.2f,%.2f,%.2f%s%s", p->uid, p->name, p->position.x, p->position.y,
                                   p->position.z, p == client->me ? "*" : "", p->active ? "" : "-");
        }
        line[o] = 0;
        fprintf(stderr, "[input_script] players t=%.0f n=%zu%s\n", g_script.time, client->game.player_count, line);
    } else if (!strcmp(e->cmd, "state")) {
        // state: one log line with the held slot and the menu/lock state (scripted checks); cursor =
        // the window's GLFW cursor mode (normal: the player sees the mouse, disabled: the game has it)
        const Player *me = client->me;
        const int cursor_mode = glfwGetInputMode(client->window, GLFW_CURSOR);
        fprintf(stderr, "[input_script] state t=%.0f slot=%d/%d weapon=%s menu=%d locked=%d pos=%.3f,%.3f,%.3f ammo=%d aim=%.2f ended=%d "
                "charge=%.3f full=%d reload=%.2f fire=%.2f grapple=%d rope=%.1f cursor=%s yaw=%.4f pitch=%.4f\n",
                g_script.time, me ? me->loadout_index : -1, me ? (int) me->loadout_size : 0,
                me && me->weapon ? me->weapon->name : "-", html_ui_available() && html_ui_is_menu_open(), client->mouse_state.locked,
                me ? me->position.x : 0.0f, me ? me->position.y : 0.0f, me ? me->position.z : 0.0f,
                me && me->ammo && me->loadout_size ? (int) me->ammo[me->loadout_index] : -1, me ? me->aim_val : 0.0f, client_round_ended(client),
                me ? me->charge_time : 0.0f, me ? me->full_charge : 0, me ? me->reload_timer : 0.0f,
                me && me->reloads && me->loadout_index >= 0 && me->loadout_index < me->loadout_size ? me->reloads[me->loadout_index] : 0.0f,
                me ? me->grapple.active : 0,
                me ? me->grapple.range : 0.0f,
                cursor_mode == GLFW_CURSOR_NORMAL ? "normal" : cursor_mode == GLFW_CURSOR_DISABLED ? "disabled" : "hidden",
                client->camera.rotation.y, client->camera.rotation.x); // the camera's look (mouse-look checks)
    } else if (!strcmp(e->cmd, "sync")) {
        // sync: our player's server-set movement state (official-lobby packets: lm/ulm, lock, frz, spb,
        // noc, tm, wep; client/src/net/krunker_link.c) on one line for scripted checks
        const Player *me = client->me;
        fprintf(stderr, "[input_script] sync t=%.0f move_lock=%d locked=%d frozen=%d boost=%.2f noclip=%d team=%d hp=%.0f/%d objective=%d "
                "pos=%.3f,%.3f,%.3f yaw=%.4f\n", g_script.time, client->game.move_lock, me ? me->mode.locked : -1, me ? me->frozen : -1,
                me ? me->speed_boost : 0.0f, me ? me->noclip : -1, me ? me->team : -1, me ? me->health : 0.0f, me ? me->max_health : 0,
                client->game.mode ? client->game.mode->active_objective : -2, me ? me->position.x : 0.0f, me ? me->position.y : 0.0f,
                me ? me->position.z : 0.0f, me ? me->direction.y : 0.0f);
        // the rest of the line: unl / jg / sic / chrg / rfl and the loadout ("rg")
        char loadout[96] = "-";
        for (int i = 0, n = 0; me && i < me->loadout_size && n < (int) sizeof(loadout) - 12; i++)
            n += snprintf(loadout + n, sizeof(loadout) - (size_t) n, "%s%d", i ? "/" : "", me->loadout[i]);
        fprintf(stderr, "[input_script] sync+ t=%.0f unl=%d jug=%d kranked=%.1f share=%d throw=%d loadout=%s\n", g_script.time,
                me ? me->unlimited_ammo : -1, me ? me->is_juggernaut : -1, me ? me->mode.kranked_timer : 0.0f, me ? me->sharing_is_caring : -1,
                me ? me->can_throw : -1, loadout);
    } else if (!strcmp(e->cmd, "cosm")) {
        // cosm @label: the store items the menu's class preview and our own (spawned) player wear:
        // "[input_script] cosm t=100 @game who=me hat=111 body=-1 ... primary=-1" ("-" = no such player)
        const Player *who[2] = {client->class_preview.player, client->me};
        static const char *const names[2] = {"preview", "me"};
        for (int k = 0; k < 2; k++) {
            const Player *p = who[k];
            if (!p) {
                fprintf(stderr, "[input_script] cosm t=%.0f %s who=%s -\n", g_script.time, e->arg, names[k]);
                continue;
            }
            fprintf(stderr, "[input_script] cosm t=%.0f %s who=%s hat=%d body=%d face=%d head=%d back=%d primary=%d melee=%d\n",
                    g_script.time, e->arg, names[k], (int) p->cosmetics.hat, (int) p->cosmetics.body, (int) p->cosmetics.face,
                    (int) p->cosmetics_v7.head, (int) p->cosmetics_v7.back, (int) p->skins.primary, (int) p->skins.melee);
        }
    } else if (!strcmp(e->cmd, "wsel")) {
        // wsel @label: what player_generate_meshes built on the held slot of the menu's class preview
        // (always slot 0) and of our own player (PlayerArms.selection), one line each ("-" = no model yet)
        const Player *who[2] = {client->class_preview.player, client->me};
        static const char *const names[2] = {"preview", "me"};
        for (int k = 0; k < 2; k++) {
            const PlayerMesh *mesh = who[k] ? who[k]->mesh : NULL;
            const int slot = who[k] ? who[k]->loadout_index : -1;
            const PlayerArms *arms = mesh && mesh->arms && slot >= 0 && slot < who[k]->loadout_size ? mesh->arms[slot] : NULL;
            fprintf(stderr, "[input_script] wsel t=%.0f %s who=%s slot=%d tp=%d %s\n", g_script.time, e->arg, names[k], slot,
                    mesh ? !mesh->render_you || mesh->kn_third_person : -1, arms && arms->selection[0] ? arms->selection : "-");
        }
    } else if (!strcmp(e->cmd, "tpstate")) {
        const Player *me = client->me;
        const ThirdPersonFrame f = third_person_last_frame(client);
        const vec3 cam = client->camera.position;
        const vec3 eye = me ? (vec3) {me->position.x, me->position.y + me->height - game_constants.camera_height, me->position.z} : cam;
        int wall = 0;
        const Map *map = client->game.map;
        for (size_t i = 0; map && i < map->object_count && !wall; i++) {
            const Object *o = map->objects[i];
            if (!bot_object_blocks_player(o) || o->complex_coll) continue;
            const vec3 sz = object_box_size(o);
            wall = cam.x > o->position.x - sz.x * 0.5f && cam.x < o->position.x + sz.x * 0.5f && cam.y > o->position.y &&
                   cam.y < o->position.y + sz.y && cam.z > o->position.z - sz.z * 0.5f && cam.z < o->position.z + sz.z * 0.5f;
        }
        const vec3 seg = {cam.x - eye.x, cam.y - eye.y, cam.z - eye.z};
        if (!wall && (seg.x != 0.0f || seg.y != 0.0f || seg.z != 0.0f)) wall = world_solid_fraction(&client->game, eye, seg) < 1.0f;
        const vec2 dir = me ? (vec2) {me->direction.x, me->direction.y} : (vec2) {0.0f, 0.0f};
        fprintf(stderr, "[input_script] tpstate t=%.0f view=%d model=%d own=%d viewmodel=%d keep=%.2f cam=%.2f,%.2f,%.2f eye=%.2f,%.2f,%.2f wall=%d "
                        "rot=%.4f,%.4f dir=%.4f,%.4f zoom=%.3f aim=%.2f\n",
                g_script.time, f.view, f.model, f.own_model, f.viewmodel, f.keep, cam.x, cam.y, cam.z, eye.x, eye.y, eye.z, wall,
                client->camera.rotation.x, client->camera.rotation.y, dir.x, dir.y, client->camera.zoom, me ? me->aim_val : 0.0f);
    } else if (!strcmp(e->cmd, "hose")) {
        // hose: the held weapon's hose (KrunkNative Piss) as drawn: tip, back end and the trigger hand's
        // centre (world), plus the camera and where the tip / hand land in the window (0..1, y down;
        // piss_hose_e2e.py)
        vec3 tip, back, hand;
        const vec3 cam = client->camera.position;
        if (client->me && player_hose_points(client->me, &tip, &back, &hand)) {
            const vec2 st = script_project(client, tip), sh = script_project(client, hand);
            fprintf(stderr, "[input_script] hose t=%.0f tip=%.4f,%.4f,%.4f back=%.4f,%.4f,%.4f hand=%.4f,%.4f,%.4f cam=%.4f,%.4f,%.4f "
                    "screen_tip=%.4f,%.4f screen_hand=%.4f,%.4f\n",
                    g_script.time, tip.x, tip.y, tip.z, back.x, back.y, back.z, hand.x, hand.y, hand.z, cam.x, cam.y, cam.z, st.x, st.y, sh.x, sh.y);
        } else {
            fprintf(stderr, "[input_script] hose t=%.0f none\n", g_script.time);
        }
    } else if (!strcmp(e->cmd, "pour")) {
        // pour: the Drank's pour-up as drawn (nettspend.h): the bottle's mouth and the cup's rim (world and
        // window fraction) and the pour's strength (nettspend_vfx_e2e.py)
        vec3 mouth, rim;
        const Player *me = client->me;
        if (me && me->weapon && me->reload_timer > 0.0f && player_pour_points(me, &mouth, &rim)) {
            const float reload_speed = client->game.config.reload_speed > 0.0f ? client->game.config.reload_speed : 1.0f;
            const float reload_time = me->weapon->reload_time * reload_speed;
            const float strength = nettspend_pour_strength(reload_time > 0.0f ? 1.0f - me->reload_timer / reload_time : -1.0f);
            const vec2 sm = script_project(client, mouth), sr = script_project(client, rim);
            fprintf(stderr, "[input_script] pour t=%.0f strength=%.2f mouth=%.4f,%.4f,%.4f rim=%.4f,%.4f,%.4f screen_mouth=%.4f,%.4f screen_rim=%.4f,%.4f\n",
                    g_script.time, strength, mouth.x, mouth.y, mouth.z, rim.x, rim.y, rim.z, sm.x, sm.y, sr.x, sr.y);
        } else {
            fprintf(stderr, "[input_script] pour t=%.0f none\n", g_script.time);
        }
    } else if (!strcmp(e->cmd, "laced")) {
        // laced <ms>: the laced visuals start (as the server's KN_EXT_LACED for us); laced: log them
        if (e->v[0] > 0.0f) nettspend_laced_start(e->v[0]);
        else fprintf(stderr, "[input_script] laced t=%.0f intensity=%.3f left=%.0f time=%.3f\n", g_script.time, nettspend_laced_intensity(),
                     nettspend_laced_left_ms(), nettspend_laced_time());
    } else if (!strcmp(e->cmd, "flags")) {
        // flags: one log line per mirrored flag (hud_modes: position, carrier, team) with its
        // crystal / stand meshes as drawn (mode_world) and the HUD text (CTF flag-cycle checks)
        for (int f = 0; f < hud_modes_flag_count(); f++) {
            vec3 p = {0}, crystal = {0}, stand = {0};
            int team = 0, carrier = -1, cv = 0, sv = 0;
            if (!hud_modes_flag(f, &p, &team, &carrier)) continue;
            const int meshes = mode_world_flag_meshes(f, &crystal, &cv, &stand, &sv);
            fprintf(stderr, "[input_script] flag t=%.0f i=%d team=%d carrier=%d pos=%.1f,%.1f,%.1f mesh=%d crystal=%.1f,%.1f,%.1f vis=%d "
                    "stand=%.1f,%.1f,%.1f svis=%d info=%s\n",
                    g_script.time, f, team, carrier, p.x, p.y, p.z, meshes, crystal.x, crystal.y, crystal.z, cv, stand.x, stand.y, stand.z, sv,
                    hud_modes_flag_info(team));
        }
    } else if (!strcmp(e->cmd, "type")) {
        // type <text>: one char callback per code point (ASCII), as typing it
        GLFWcharfun cb = glfwSetCharCallback(client->window, NULL);
        glfwSetCharCallback(client->window, cb);
        for (const char *c = e->arg; cb && *c; c++) cb(client->window, (unsigned char) *c);
    } else if (script_probe(e->cmd) && strcmp(e->cmd, "console")) {
        // vis <id> [<id> ...]: one log line with each element's visibility (1 shown, 0 not, - absent);
        // scroll <id> [<id> ...]: their scrollTop instead; rect <id> [<id> ...]: their boxes
        const int scroll = e->cmd[0] == 's';
        const int rect = e->cmd[0] == 'r';
        char line[2048];
        int n = snprintf(line, sizeof(line), "[input_script] %s t=%.0f", e->cmd, g_script.time);
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
            if (rect) {
                const HtmlElem el = html_ui_available() ? html_dom_by_id(id) : 0;
                HtmlRect r;
                if (el && html_dom_get_rect(el, &r))
                    n += snprintf(line + n, sizeof(line) - (size_t) n, " %s=%.1f,%.1f,%.1f,%.1f,%.1f,%.1f", id, r.x, r.y, r.w, r.h,
                                  html_dom_offset_width(el), html_dom_offset_height(el));
                else n += snprintf(line + n, sizeof(line) - (size_t) n, " %s=-", id);
                continue;
            }
            if (scroll) {
                const HtmlElem el = html_ui_available() ? html_dom_by_id(id) : 0;
                HtmlScroll sc;
                if (el && html_dom_get_scroll(el, &sc)) n += snprintf(line + n, sizeof(line) - (size_t) n, " %s=%.1f", id, sc.top);
                else n += snprintf(line + n, sizeof(line) - (size_t) n, " %s=-", id);
                continue;
            }
            if (!strcmp(id, ":cursor")) {
                n += snprintf(line + n, sizeof(line) - (size_t) n, " %s=%d", id, glfwGetInputMode(client->window, GLFW_CURSOR) == GLFW_CURSOR_NORMAL);
                continue;
            }
            if (!strcmp(id, ":preview") || !strcmp(id, ":spray") || !strcmp(id, ":objectives")) {
                const ClassPreview *cp = &client->class_preview;
                const int shown = id[1] == 's' ? sprays_wheel_shown() : id[1] == 'o' ? hud_objectives_shown()
                    : html_ui_available() && html_ui_is_menu_open() && cp->has_rect && cp->visible && cp->width > 0.0f && cp->height > 0.0f;
                n += snprintf(line + n, sizeof(line) - (size_t) n, " %s=%d", id, shown);
                continue;
            }
            const HtmlElem el = html_ui_available() ? html_dom_by_id(id) : 0;
            n += snprintf(line + n, sizeof(line) - (size_t) n, " %s=%s", id, !el ? "-" : html_dom_is_shown(el) ? "1" : "0");
        }
        fprintf(stderr, "%s\n", line);
    } else if (!strcmp(e->cmd, "clip")) {
        const char *clip = glfwGetClipboardString(client->window);
        char text[512];
        size_t n = 0;
        for (const char *c = clip ? clip : ""; *c && n + 4 < sizeof(text); c++) {
            if (*c == '\n') { text[n++] = '\\'; text[n++] = 'n'; }
            else if (*c == '\\') { text[n++] = '\\'; text[n++] = '\\'; }
            else text[n++] = *c;
        }
        text[n] = 0;
        fprintf(stderr, "[input_script] clip t=%.0f %s text=%s\n", g_script.time, e->arg, text);
    } else if (!strcmp(e->cmd, "console")) {
        char state[320];
        ui_console_probe(state, sizeof(state));
        fprintf(stderr, "[input_script] console t=%.0f %s %s\n", g_script.time, e->arg, state);
    } else if (!strcmp(e->cmd, "conin")) {
        // conin: the developer console's input line and its hint rows (the tab completion: client/tests/class_console_e2e.py)
        char state[1024];
        ui_console_input_probe(state, sizeof(state));
        fprintf(stderr, "[input_script] conin t=%.0f %s %s\n", g_script.time, e->arg, state);
    } else if (!strcmp(e->cmd, "class")) {
        // class @label: the local player's class (index and name), its secondary weapon, health and loadout weapon ids
        // (client/tests/class_console_e2e.py): "[input_script] class t=100 @label index=3 name=Spray N Pray secondary=2
        // health=170 max=170 loadout=1,2"
        const Player *me = client->me;
        const size_t classes = rules_class_count();
        const int index = me ? me->class_index : -1;
        const char *name = index >= 0 && (size_t) index < classes && rules_classes()[index].name ? rules_classes()[index].name : "-";
        char loadout[96] = "";
        for (int i = 0; me && i < me->loadout_size && (size_t) strlen(loadout) < sizeof(loadout) - 12; i++) {
            char id[12];
            snprintf(id, sizeof(id), "%s%d", i ? "," : "", me->loadout[i]);
            strncat(loadout, id, sizeof(loadout) - strlen(loadout) - 1);
        }
        fprintf(stderr, "[input_script] class t=%.0f %s index=%d name=%s secondary=%d health=%.0f max=%d loadout=%s\n", g_script.time, e->arg,
                index, name, me ? me->secondary_index : -1, me ? (double) me->health : -1.0, me ? me->max_health : -1,
                loadout[0] ? loadout : "-");
    } else if (!strcmp(e->cmd, "page")) {
        char mode[16] = "";
        const char *query = "";
        sscanf(e->arg, "%15s", mode);
        if (strlen(e->arg) > strlen(mode)) query = e->arg + strlen(mode) + 1;
        fprintf(stderr, "[input_script] page t=%.0f %s\n", g_script.time, e->arg);
        for (int m = 0; m < APP_MODE_COUNT; m++) {
            if (strcmp(mode, app_mode_name((AppMode) m))) continue;
            app_set_mode(client, (AppMode) m, query);
            g_script.page_wait = 1; // the clock stops until the page is up (page_waiting)
            g_script.page_mode = (AppMode) m;
        }
    } else if (!strcmp(e->cmd, "con")) {
        ui_console_exec(e->arg); // con <word>: a one-word developer console command (`end`)
    } else if (!strcmp(e->cmd, "nethit")) {
        const NetHitPacket hit = {.target_uid = -1, .kill = (uint8_t) (e->v[0] != 0.0f)};
        client_handle_game_event(client, NET_PACKET_HIT, &hit, (uint16_t) sizeof(hit));
    } else if (!strcmp(e->cmd, "killed")) {
        // the KILL packet of a death by the first other active player (a rifle kill, no weapon): the
        // death cam with its killer (client/tests/death_cam_e2e.py); the kill feed and the card follow
        const Player *me = client->me;
        const Player *killer = NULL;
        for (size_t i = 0; me && !killer && i < client->game.player_count; i++) {
            const Player *p = client->game.players[i];
            if (p && p != me && p->active) killer = p;
        }
        if (me && me->active && killer) {
            NetKillPacket kill = {0};
            kill.victim_uid = me->uid;
            kill.killer_uid = killer->uid;
            kill.weapon_index = -1;
            kill.victim_deaths = 1;
            kill.killer_kills = 1;
            kill.respawn_delay = 1.0f;
            client_handle_game_event(client, NET_PACKET_KILL, &kill, (uint16_t) sizeof(kill));
        }
        fprintf(stderr, "[input_script] killed t=%.0f by=%d\n", g_script.time, killer ? killer->uid : -1);
    } else if (!strcmp(e->cmd, "resize")) {
        if (e->v[0] >= 1.0f && e->v[1] >= 1.0f) glfwSetWindowSize(client->window, (int) e->v[0], (int) e->v[1]);
    } else if (!strcmp(e->cmd, "blur")) {
        client_window_focus_lost(client); // alt-tab (the GLFW focus callback)
    } else if (!strcmp(e->cmd, "play") && g_script.boot_mode) {
        g_script.play_pending = 1; // input_script_tick enters once the map is ready
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

// `page`: 1 while the page it asked for is not shown and ready yet (the game page: its menu set up),
// then one more frame so the page's UI ticked once; no events run and the clock stands still.
static int page_waiting(Client *client) {
    if (!g_script.page_wait) return 0;
    if (g_script.page_wait < 0) {
        g_script.page_wait = 0;
        return 1;
    }
    const int ready = g_script.page_mode == APP_MODE_GAME ? app_mode() == APP_MODE_GAME && html_ui_is_ready() && menu_ui()->dom_ready
                                                           : app_mode() == g_script.page_mode && app_mode_page_ready(client);
    if (ready) g_script.page_wait = -1;
    return 1;
}

// `ft start`: one wall-clock frame time per frame (game and page ticks)
static void ft_sample(void) {
    if (g_script.ft_last <= 0.0) return;
    const double now = glfwGetTime();
    if (g_script.ft_n == g_script.ft_cap) {
        const int cap = g_script.ft_cap ? g_script.ft_cap * 2 : 1024;
        float *grown = realloc(g_script.ft_ms, (size_t) cap * sizeof(float));
        if (grown) { g_script.ft_ms = grown; g_script.ft_cap = cap; }
    }
    if (g_script.ft_n < g_script.ft_cap) g_script.ft_ms[g_script.ft_n++] = (float) ((now - g_script.ft_last) * 1000.0);
    g_script.ft_last = now;
}

void input_script_tick(Client *client) {
    script_load();
    if (!g_script.active) return;

    ft_sample();

    if (!g_script.playing && g_script.count && !strcmp(g_script.events[0].cmd, "boot")) {
        if (html_ui_available() && (!html_ui_is_ready() || !menu_ui()->dom_ready)) return;
        g_script.playing = g_script.menu_mode = g_script.boot_mode = 1;
        g_script.time = 0.0f;
        g_script.next = 1;
    }
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

    if (page_waiting(client)) return;

    // A `vis` / `scroll` / `rect` / `state` probe never shares a frame with the input before it: after a stalled frame the due
    // events would otherwise run together and the probe would read the page before the UI
    // reacted to them (the HUD / menu passes run later in the frame), or the look before the
    // frame's mouse movement turned the camera (`state` yaw / pitch).
    int ran = 0;
    while (g_script.next < g_script.count && g_script.events[g_script.next].t <= g_script.time + 1e-3f) {
        const ScriptEvent *e = &g_script.events[g_script.next];
        const char *prev = ran ? g_script.events[g_script.next - 1].cmd : NULL;
        const int probe = script_probe(e->cmd) || !strcmp(e->cmd, "state");
        if (prev && probe && !script_probe(prev) && strcmp(prev, "state")) break;
        script_run(client, e);
        g_script.next++;
        ran = 1;
        if (g_script.page_wait) break; // `page`: the rest waits for that page
    }

    if (g_script.play_pending) {
        if (client->me && client->me->active) {
            g_script.play_pending = 0;
        } else if (client->game.ready && !client->spawn_requested && (!html_ui_is_ready() || menu_ui()->dom_ready)) {
            if (html_ui_available() && html_ui_is_menu_open()) client_set_menu_open(client, 0);
            client_enter_game(client);
        }
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

    // `cam`: the first frames after each of our own spawns (a new Player, or ours active again)
    const Player *live = p && p->active ? p : NULL;
    if (live && live != g_script.spawn_me) {
        g_script.spawns++;
        g_script.spawn_frame = 0;
    }
    g_script.spawn_me = live;
    if (live && g_script.spawn_frame >= 0 && g_script.spawn_frame < 5) {
        char label[16];
        snprintf(label, sizeof(label), "@spawn%d", g_script.spawn_frame++);
        script_log_cam(client, label);
    } else {
        g_script.spawn_frame = -1;
    }

    write_pending_shot(client);

    // The frame time, not the world's: the end-of-round slow motion scales the world delta
    // down to 0 for the whole results screen, which would hold every later event back.
    if (!g_script.page_wait) g_script.time += frame_delta * 1000.0f;
}

void input_script_idle_frame(Client *client, const float frame_delta) {
    script_load();
    if (!g_script.active || !g_script.count || strcmp(g_script.events[0].cmd, "boot") != 0) return;
    input_script_tick(client);
    if (g_script.playing) {
        write_pending_shot(client);
        if (!g_script.page_wait) g_script.time += frame_delta * 1000.0f;
    }
}

// Page windows (app_mode.c): the clock starts once the page is ready; game-only commands
// (play, pose, tp, look, respawn) are skipped.
void input_script_page_tick(Client *client) {
    script_load();
    if (!g_script.active) return;
    ft_sample();
    // Offline, a page window never steps the sandbox (main.c pumps it after the game tick, which
    // the page replaces), so the hub's requests stay unanswered and its lists empty. Scripted
    // runs answer them (delta 0: the frozen game does not advance) so the hub's card lists can
    // be tested (server/tests/ui_transitions_e2e.py hub-items).
    client_offline_pump(client, 0.0f);
    if (!g_script.playing) {
        if (!app_mode_page_ready(client)) return;
        g_script.playing = 1;
        g_script.time = 0.0f;
    }
    if (page_waiting(client)) return;
    int ran = 0; // a probe never shares a frame with the input before it (input_script_tick)
    while (g_script.next < g_script.count && g_script.events[g_script.next].t <= g_script.time + 1e-3f) {
        const ScriptEvent *e = &g_script.events[g_script.next];
        const int probe = script_probe(e->cmd) || !strcmp(e->cmd, "state");
        if (ran && probe && !script_probe(g_script.events[g_script.next - 1].cmd) && strcmp(g_script.events[g_script.next - 1].cmd, "state")) break;
        g_script.next++;
        static const char *const GAME_ONLY[] = {"play", "pose", "tp", "look", "respawn"};
        int skip = 0;
        for (size_t i = 0; i < sizeof(GAME_ONLY) / sizeof(GAME_ONLY[0]); i++) skip |= !strcmp(e->cmd, GAME_ONLY[i]);
        if (!skip) script_run(client, e);
        ran = 1;
        if (g_script.page_wait) break; // `page`: the rest waits for that page
    }
}

void input_script_page_frame_end(Client *client, const float frame_delta) {
    if (!g_script.active || !g_script.playing) return;
    write_pending_shot(client);
    if (!g_script.page_wait) g_script.time += frame_delta * 1000.0f;
}

void input_script_frame_presented(Client *client) {
    if (!g_script.active || !g_script.playing || g_script.frames_left <= 0) return;
    char path[200];
    snprintf(path, sizeof(path), "%s_%04d.png", g_script.frames, g_script.frames_index++);
    write_frame_png(client, path);
    fprintf(stderr, "[input_script] frame %s\n", path); // orders the captures against the log
    g_script.frames_left--;
}
