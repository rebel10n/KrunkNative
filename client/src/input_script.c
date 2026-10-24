// Scripted input for movement / first-person feel parity tests.
//
//   KRUNKNATIVE_INPUT_SCRIPT=<file>   timed input events (below)
//   KRUNKNATIVE_INPUT_LOG=<file>      one CSV row per frame from `play` on (header in the file)
//   KRUNKNATIVE_FIXED_DELTA=<ms>      every frame advances exactly this much time, so a slow host
//                                     (Xvfb/llvmpipe) replays at a chosen 30/60/144 fps step
//
// Script: one event per line, "<t_ms> <command> [args]", '#' starts a comment (not inside "..."). Times count
// summed frame deltas (the round-end slow motion does not stretch them) from the frame the
// local player became active after `play`.
//   0 play                       close the menu and enter the game (waits for the map)
//   100 waitfor <selector>       hold the script (its clock too) until a node matches the selector, `waitfor !<selector>`
//                                until none does (gives up after 90 s, logged); for steps the clock alone cannot time
//   0 menu                       as the first event instead of play: the clock starts on the menu once
//                                the map is ready (menu clicks before any spawn, e.g. a comp lobby's
//                                team / class picks); a later `play` then enters the game
//   0 boot                       as the first event: the clock starts as soon as the menu page is set up, map
//                                or not (an official krunker.io join waits for its matchmaker's captcha
//                                before any map exists); a later `play` enters the game once a map is in
//   0 pose x y z yaw pitch       place the local player at rest (like tools/kref/movement-rec.js); offline
//                                the sandbox server's player too (the dev "tp")
//   0 tp x y z yaw pitch         online: the dev "tp" packet to feet x/y/z, then face yaw/pitch
//   100 recon feet_y [lag]       test only: a forced server correction (NET_PACKET_RECON snap) to our x/z at feet_y,
//                                acked `lag` input frames back (30): those after it are replayed
//   100 down W | up W            keys through the key callback: W A S D SPACE SHIFT TAB ESC ENTER DELETE, any
//                                A-Z / 0-9, F1-F12, GRAVE (the dev console's `); CTRL+ / SHIFT+ / ALT+ prefixes
//                                set the modifier bits (down CTRL+C: copy the page selection). A down of a
//                                printable key (A-Z, 0-9, SPACE, GRAVE) also sends its char through the char
//                                callback right after, as GLFW does (none with CTRL+ / ALT+; SHIFT+ letters upper case)
//   100 gpad start               a game controller button press + release (gamepad.js names: start,
//                                button_1, ...) through the gamepad event machine, as a pad would
//                                (Gamepad.active, then CONTROLS' listeners); no pad is needed
//   100 gpad shoulder_bottom_right down | up   the button held (down) until released (up): the trigger
//                                holds shoot, as the pad's loop sees it over the frames
//   100 gpad stick left 0 -1    a stick held at that deflection (x y, -1..1) until the next stick line
//                                (gpad stick right 0 0 centres it); the scripted pad is connected at its first line
//   100 mdown right | mup right  mouse buttons (left / right), through the mouse button callback
//   100 touch start 1 640 360    a finger (id 1) down / moved / lifted on the phone controls (ui_mobile.c, the
//   100 touch move 1 700 360     web build's #mobileUI touch listeners): several ids at once are several fingers
//   100 touch end 1             (joystick + look + fire); window px like click. Needs KRUNKNATIVE_MOBILE=1
//   100 touch tap 640 360        a finger down and up at a point through the mouse callbacks (a page tap: the
//                                menu's TAP TO PLAY, or a #mobileUI button, as the mouse touch routes it)
//   100 touch start 1 300 500    under Krunker Strike the same verbs drive the page's touch layer (strike_touch.c):
//                                window px like the phone's, converted to the page's CSS px
//   100 touch 320 180           the Krunker Strike form: a finger (id 1) tapped at that CSS point of the Strike
//                                page (strike_touch.c); the first touch makes the touch controls the input
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
//   100 blur                     the window loses focus (alt-tab): the GLFW focus callback (keyboard focus
//                                gone, pointer free, menu or end title as on a lost lock)
//   100 focus                    the window has the keyboard focus again (the callback with focused = 1)
//   100 nethit 0 | nethit 1      a NET_PACKET_HIT from the server (no target, no damage; 1 = the kill
//                                variant) through the client's event handler: the hit sound and
//                                marker, as when your shot lands (hitmarker tests, alive or dead)
//   100 damage 23.4 0            the server's hit on the first other active player for 23.4 (crit 0 / 1):
//                                NET_EXT_KN_HIT_DAMAGE (when it has a fraction) + NET_PACKET_HIT through the
//                                client's event handler, with that player put 20 units ahead of the camera
//                                first (client copy only) so the damage number shows (dmgType tests)
//   100 resize 800 600           the window's size (glfwSetWindowSize; headless: the framebuffer-size
//                                event follows at the frame's end, like a window manager's resize)
//   100 om add 28 | om drop 20   an Object Manager Weapons-tab click without the window: left (add the
//                                extra loadout slot, in hand when it lands; again: take it out) / right
//                                (a pickup at the crosshair)
//   100 om tool 1                a Tools-pane pick (0 Deleter, 1 Duplicator, 2 Rotator, 3 Info, 4 Move,
//                                5 Resize, 6 Properties): sets the Tool Gun's tool and puts the Tool Gun in hand (fire it
//                                with `mdown left`); `om physgun 0` the Physics Gun tile, `om snap 2.5` (the step, 0 off),
//                                `om object 10` an Objects tile,
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
//   100 wars 42 NUKE ff3b3b 7    a "warsClan" from the server (NET_EXT_WARS_CLAN through the client's EXT handler): clan id,
//                                name, banner colour (hex without its '#', which starts a comment; "s3": paint shader 3; "-":
//                                none), logo version (optional); `wars none` clears it
//                                (the Turf Wars popup and the map's wars_banner banners, wars_banners.c)
//   100 spinw 0 37.5 2482        a "spinW" (NET_EXT_SPIN_WHEEL_RESPONSE, not ours): spinner uid, randNum, the item won
//   100 wheelrar 2 0 28 53 13.5 4.46 1 0.04  test only: store wheel 2 draws by rarity with these odds (the compiled-in data
//                                has only pool wheels among the map wheels 0..2, whose stop throws in v10 before the won
//                                item goes up; the brain's store_spin_data could serve such a wheel)
//   100 wheels @label            log the map prize wheels: "[input_script] wheels t=100 @label n=1 planes=1 banners=3
//                                0:wheel=2,rot=82.47,mlt=1.00,end=0.0/0.0,item=2482,paints=12,tex=1,up=-1@0,0,0" (prize_wheels.c
//                                state, tex: the plane has a painted canvas, up: the won item in front of the hub and its model's
//                                world position, -1 none)
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
//   100 poses                    log each player's pose on its model and the attached script objects per pass:
//                                "[input_script] poses t=100 fps=1 world=3 0*:has=0x20 hold=0.000 l=x,y,z r=x,y,z
//                                merged=x,y,z ..." (fps / world: attachTo objects the viewmodel / world pass draws;
//                                l / r: the held slot's arm pivots' rotations; player_pose_render.c)
//   100 players                  log every player of the session: "[input_script] players t=100 n=2 5:Name@x,y,z
//                                9:Guest@x,y,z*" (* us, - inactive)
//   100 models @label            log every player's model (v10.0.3 class models, player_class_model.h): "[input_script]
//                                models t=100 @label n=2 5:body=model,mdl=2,asset=11441g,clip=Move 9:body=default,..."
//                                (body: default, model, none, fallback, - dead; clip: the GLTF's clip, * while a one-shot
//                                runs; stalker=1: the Stalker look)
//   100 aimlast                  turn the look onto the centre of the newest object (the crate the Object Manager
//                                spawned last), as `tp` sets the look: the crosshair is on it for a grab or a pick
//   100 pg @label                log the Physics Gun's grab (om_build.c): "[input_script] pg t=100 @label shown=x,y,z
//                                sent=x,y,z target=x,y,z grab=1 uid=5 frozen=0 releasing=0 drag=0 sel=-1 aim=5
//                                aimfeet=x,y,z hold=x,y,z edit=1 takes=1 gun=1 caps=15 shoot=1 map=0" (shown: the
//                                carry as swept, sent: the last pose sent, target: the aim's own pose, hold: the
//                                object in the client's copy; aim: the object under the crosshair; edit / takes: the
//                                Physics Gun may edit and take it; gun: in hand; shoot: alive; map: a map object), then
//                                "... @label last=5 spawned=1 at=x,y,z" for the newest object
//   100 omres 7                  an Object Manager answer to a Physics Gun edit (SPAWN_RESULT, action EDIT, status 7 =
//                                Spawn in first) through the client's packet handler, as an edit on its way when we died
//   100 toast @label             the toasts issued so far (ui_notify): "[input_script] toast t=100 @label n=3"
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
//   100 spawn                    the clock stops until our player is active and the menu is closed (the loading after a
//                                Play click), then the script goes on: a later event's time counts from the spawn
//   100 scroll @grid knOmGrid    log scroll containers' scrollTop (html_dom_get_scroll), like vis:
//                                "[input_script] scroll t=100 @grid knOmGrid=196.0" (- = no such id or
//                                not a scroll container)
//   100 style @r ammoHolder:background-size  log inline style values, like vis: "[input_script] style
//                                t=100 @r ammoHolder:background-size=57%_100%" (spaces as _, - = unset or no such id)
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
//   100 attr @label src #victorySub  log the first match's attribute: "[input_script] attr t=100 @label n=1
//                                src=./img/vic.png" ("-" when absent or nothing matches)
//   100 glyphs @label #menuWindow  log how many characters every match's rendered text draws without a glyph
//                                (tofu, html_dom_missing_glyphs) and the first such code point: "[input_script]
//                                glyphs t=100 @label n=1 missing=0 first=-" (localisation tests: i18n_e2e.py)
//   100 locale @label            log the active language (i18n.h): "[input_script] locale t=100 @label code=ru
//                                gen=3 keys=1009"
//   100 ft @label start | stop   wall-clock frame times between start and stop: one line with the
//                                frame count, avg / p50 / p95 / max ms (perf logs, never asserted here)
//   100 builds @label            log the player builds: "[input_script] builds t=100 @label n=12 predicted=1
//                                meshes=12 visible=12 last=x,y,z spawned=2 draws=1 spawned_predicted=0" (n: active map
//                                builds, predicted: the ones this client placed ahead of the server, meshes / visible:
//                                their slots in the build batch, last: the newest one's feet, spawned: Object Manager
//                                objects drawn, draws: the builds' draw calls, spawned_predicted: Object Manager objects
//                                this client placed ahead of the server; map_post.c)
//   100 placewatch @label        from this frame, count frames and wall-clock ms until one more build mesh shows
//                                (with `mdown left` on the Build Tool: input to block visible): "[input_script]
//                                place t=100 @label frames=2 ms=3.1 visible=13" ("timeout" after 5 s)
//   100 type rifle               type the rest of the line through the char callback (the focused
//                                text field; the menu or the chat must have the keyboard)
//   100 clip @copied             log the system clipboard (GLFW): "[input_script] clip t=100 @copied
//                                text=<C-escaped clipboard>" (selection + Ctrl+C tests)
//   1200 shot /tmp/a.png         write the frame (world + first-person layer) after this frame's input
//   1200 frames /tmp/f 120       write this and the next 119 finished frames (page included) as
//                                /tmp/f_0000.png ... (input_script_frame_presented); each logs "[input_script]
//                                frame /tmp/f_0000.png t=1200 open=1 ... rect=x,y,w,h ..." (the `console` probe's
//                                fields: the developer console's state as that frame was painted)
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
#include <player_accounts.h> // `netchat`: the bot PLAYER_INFO the server sends
#include <sprays.h>  // `vis :spray`
#include <hud_modes.h>  // `flags`
#include <mode_world.h> // `flags`
#include <object_manager.h> // `om object` (OM_TAB_OBJECTS)
#include <third_person.h> // `tpstate`
#include <player_class_model.h> // `models`: the class models
#include <world.h>        // `tpstate`: world_solid_fraction
#include <bots.h>         // `tpstate`: bot_object_blocks_player
#include <menu_view.h>    // `cam`: menu_view_killer_uid (the death chase)
#include <spectate.h>     // `cam`: spectate_active
#include <strike_ui.h>    // `touch`: a finger on the Krunker Strike touch layer (strike_touch.c)
#include <html_dom.h>     // html_dom_viewport: the Strike page's CSS px for `touch start|move|end`
#include <hud_map.h>     // `pings`: the ping markers on screen
#include <nettspend.h>    // `pour`, `laced`
#include <ruleset.h>      // `class`: the class names
#include <map_post.h>     // `builds`, `placewatch`
#include <gamepad.h>      // `gpad`
#include <entities.h>       // `script`: entities / runtime blocks
#include <player_pose_render.h> // `poses`: the player poses on the models
#include <ks_client.h>         // `poses`: the attached script objects per pass
#include <ui_mobile.h>  // `touch`: the phone touch controls
#include <ai.h>           // `world`: the AI list
#include <net_ext.h>      // `damage`: NET_EXT_KN_HIT_DAMAGE; `ranked`: NET_EXT_KN_RANKED_MATCH
#include <lobby_ui.h>     // `ranked`: comp_ui_round (the comp menu an INIT opens)
#include <s2c_events.h>   // `wars` / `spinw`: the EXT handler
#include <prize_wheels.h>  // `wheels`
#include <store_spins.h>   // `wheelrar`
#include <prize_wheel_render.h>
#include <wars_banners.h>
#include <GLFW/glfw3.h>
#include <png_write.h>
#include <frame_target.h>
#include <math.h>
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#define SCRIPT_MAX_EVENTS 4096 // a key sweep across a round end is ~1500 lines (console_draw_sweep_e2e.py); the table is static (BSS)

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
    int spawn_wait;   // `spawn`: 1 until our player is active and the menu closed (spawn_waiting)
    char wait_sel[192]; // `waitfor <selector>`: the clock and the events stop until a node matches (clock_held)
    double wait_start;  // glfwGetTime() when the current waitfor began (WAIT_SECONDS_MAX, then the event gives up)
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
    char pw_label[64]; // `placewatch`: the label, "" = not watching
    int pw_base, pw_frames; // visible build meshes when it started, frames since
    double pw_start;  // wall clock when it started (s)
} g_script;

// The page probes: log lines, the whole rest of the line is ids (and @labels).
static int script_probe(const char *cmd) {
    return !strcmp(cmd, "vis") || !strcmp(cmd, "scroll") || !strcmp(cmd, "rect") || !strcmp(cmd, "console") || !strcmp(cmd, "style");
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
        // '#' outside double quotes starts a comment (call setSetting "scopeBorderCol" "#ff0000" keeps its colour)
        for (char *c = line, quoted = 0; *c; c++) {
            if (*c == '"') quoted = (char) !quoted;
            else if (*c == '#' && !quoted) {
                *c = 0;
                break;
            }
        }

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
            !strcmp(e->cmd, "text") || !strcmp(e->cmd, "rows") || !strcmp(e->cmd, "gpad") || !strcmp(e->cmd, "grep") || !strcmp(e->cmd, "attr") || !strcmp(e->cmd, "glyphs") || !strcmp(e->cmd, "locale") || !strcmp(e->cmd, "page") ||
            !strcmp(e->cmd, "waitfor") ||
            !strcmp(e->cmd, "netchat") || !strcmp(e->cmd, "wars") || !strcmp(e->cmd, "wheels") || !strcmp(e->cmd, "wheelrar") || !strcmp(e->cmd, "builds") || !strcmp(e->cmd, "pg") || !strcmp(e->cmd, "toast") || !strcmp(e->cmd, "placewatch") || !strcmp(e->cmd, "touch")) { // the whole rest of the line: `con bot_add 5 difficulty=hard`
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
    if (g_script.count == SCRIPT_MAX_EVENTS) fprintf(stderr, "[input_script] WARNING: more than %d lines, the rest are dropped\n", SCRIPT_MAX_EVENTS);
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
        {"BACKSLASH", GLFW_KEY_BACKSLASH}, // Krunker Strike's alt shoot bind (220)
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

// The code point a US-layout press of `key` types (0: none): letters (Shift: upper case), digits,
// Space. Ctrl / Alt chords type nothing (GLFW drops their chars: `plain` in _glfwInputChar).
static unsigned int script_key_char(const int key, const int mods) {
    if (mods & (GLFW_MOD_CONTROL | GLFW_MOD_ALT)) return 0;
    if (key >= GLFW_KEY_A && key <= GLFW_KEY_Z) return (unsigned int) ((mods & GLFW_MOD_SHIFT) ? 'A' : 'a') + (unsigned int) (key - GLFW_KEY_A);
    if (key >= GLFW_KEY_0 && key <= GLFW_KEY_9 && !(mods & GLFW_MOD_SHIFT)) return '0' + (unsigned int) (key - GLFW_KEY_0);
    if (key == GLFW_KEY_SPACE) return ' ';
    if (key == GLFW_KEY_GRAVE_ACCENT) return (mods & GLFW_MOD_SHIFT) ? '~' : '`';
    return 0;
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

// touch start|move|end <id> [x y] / touch tap x y (see the header): the phone touch routing with an
// explicit finger id (ui_mobile_touch_point), window px scaled to the framebuffer as the mouse path does.
static void script_touch(Client *client, const char *arg) {
    char verb[16] = {0};
    if (sscanf(arg, "%15s", verb) != 1) return;
    if (verb[0] >= '0' && verb[0] <= '9') {
        // touch X Y: the Krunker Strike form (see the header): a finger tapped at that CSS point of the Strike
        // page (id 1); the first touch makes the touch controls the input (strike_touch_input)
        float sx = 0.0f, sy = 0.0f;
        if (client && sscanf(arg, "%f %f", &sx, &sy) == 2 && strike_ui_active()) {
            strike_touch_event(client, 0, 1, sx, sy);
            strike_touch_event(client, 2, 1, sx, sy);
        }
        return;
    }
    const char *rest = arg + strlen(verb);
    float x = 0.0f, y = 0.0f;
    if (!strcmp(verb, "tap")) {
        if (sscanf(rest, "%f %f", &x, &y) != 2) return;
        glfwSetCursorPos(client->window, x, y);
        // the page reads the pointer's position on a tap only while its menu is up; in game a cursor
        // move would be a mouse look (a phone's finger has none), so the move is not delivered there
        if (!html_ui_available() || html_ui_is_menu_open()) {
            GLFWcursorposfun pos = glfwSetCursorPosCallback(client->window, NULL);
            glfwSetCursorPosCallback(client->window, pos);
            if (pos) pos(client->window, x, y);
        }
        GLFWmousebuttonfun cb = glfwSetMouseButtonCallback(client->window, NULL);
        glfwSetMouseButtonCallback(client->window, cb);
        if (cb) {
            cb(client->window, GLFW_MOUSE_BUTTON_LEFT, GLFW_PRESS, 0);
            cb(client->window, GLFW_MOUSE_BUTTON_LEFT, GLFW_RELEASE, 0);
        }
        return;
    }
    UiTouchPhase phase;
    if (!strcmp(verb, "start")) phase = UI_TOUCH_START;
    else if (!strcmp(verb, "move")) phase = UI_TOUCH_MOVE;
    else if (!strcmp(verb, "end")) phase = UI_TOUCH_END;
    else {
        fprintf(stderr, "[input_script] touch %s: unknown verb\n", verb);
        return;
    }
    long id = 0;
    if (sscanf(rest, "%ld %f %f", &id, &x, &y) < 1) return;
    int ww = 0, wh = 0, fw = 0, fh = 0;
    glfwGetWindowSize(client->window, &ww, &wh);
    glfwGetFramebufferSize(client->window, &fw, &fh);
    if (ww > 0 && wh > 0) {
        x *= (float) fw / (float) ww;
        y *= (float) fh / (float) wh;
    }
    if (strike_ui_active()) { // Krunker Strike: the finger on the page's touch layer (strike_touch.c), CSS px
        float vw = 0.0f, vh = 0.0f, dpr = 1.0f;
        html_dom_viewport(&vw, &vh, &dpr);
        if (dpr > 0.0f) {
            x /= dpr;
            y /= dpr;
        }
        strike_touch_event(client, phase == UI_TOUCH_START ? 0 : phase == UI_TOUCH_MOVE ? 1 : 2, id, x, y);
        return;
    }
    const int used = ui_mobile_touch_point(phase, id, x, y);
    fprintf(stderr, "[input_script] touch %s id=%ld at=%.0f,%.0f used=%d\n", verb, id, x, y, used);
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
            // A real key press of a printable key is a key event AND a char event (GLFW's X11 /
            // Wayland / Win32 / Cocoa backends: the key callback first, then the char callback,
            // never with Ctrl or Alt held). Typing tests need both: a field that only sees the
            // keydown (or a game bind that only checks the char) passes a test it fails for real.
            const unsigned int cp = script_key_char(key, mods);
            if (action == GLFW_PRESS && cp) {
                GLFWcharfun ccb = glfwSetCharCallback(client->window, NULL);
                glfwSetCharCallback(client->window, ccb);
                if (ccb) ccb(client->window, cp);
            }
        }
    } else if (!strcmp(e->cmd, "gpad")) {
        if (!gamepad_script_input(e->arg)) fprintf(stderr, "[input_script] gpad %s: unknown button or stick\n", e->arg);
    } else if (!strcmp(e->cmd, "mdown") || !strcmp(e->cmd, "mup")) {
        const int button = !strcmp(e->arg, "right") ? GLFW_MOUSE_BUTTON_RIGHT : !strcmp(e->arg, "middle") ? GLFW_MOUSE_BUTTON_MIDDLE : GLFW_MOUSE_BUTTON_LEFT;
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
    } else if (!strcmp(e->cmd, "touch")) {
        script_touch(client, e->arg);
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
    } else if (!strcmp(e->cmd, "recon") && client->me) {
        // recon feet_y: test-only forced server correction. A NET_PACKET_RECON-shaped snap of our own
        // player to (x, feet_y, z) with zero velocity, acked at our last input frame, through the
        // client's real reconciliation (client_recon_apply: snap, then replay of the pending inputs).
        // The e2e harness uses it to see what a server correction does at a given height.
        NetReconPacket pkt = {0};
        // acked `lag` input frames back (default 30): the frames after it are replayed on top of the snap
        const int lag = e->v[1] > 0 ? (int) e->v[1] : 30;
        pkt.ack_seq = client->me->input_seq - lag;
        pkt.position[0] = client->me->position.x;
        pkt.position[1] = e->v[0];
        pkt.position[2] = client->me->position.z;
        pkt.yaw = client->me->direction.y;
        pkt.flags = NET_RECON_ON_GROUND;
        pkt.crouch_val = client->me->crouch_val;
        pkt.aim_val = client->me->aim_val;
        fprintf(stderr, "[input_script] recon t=%.0f feet y %.3f ack %d\n", g_script.time, e->v[0], pkt.ack_seq);
        client_recon_apply(client, &pkt);
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
    } else if (!strcmp(e->cmd, "ranked")) {
        // ranked <0|1> [<0|1>]: the server's NET_EXT_KN_RANKED_MATCH for this round, through the client's own packet
        // path (game_events.c). A second 1 first makes the game a comp lobby (config.comp, test mode) and opens its menu
        // as an INIT's comp_ui_round does, so the comp box's title shows (v10 g:219431). The in-match gates are
        // checked by client/tests/ranked_match_ui_e2e.py.
        if (e->v[1] != 0) {
            client->game.config.comp = 1;
            client->game.config.test_mode = 1;
            comp_ui_round(client);
        }
        const NetRankedMatchPacket ranked = {.ranked = e->v[0] != 0};
        uint8_t payload[NET_EXT_HEADER_SIZE + sizeof(ranked)];
        const uint16_t n = net_ext_pack(payload, sizeof(payload), NET_EXT_KN_RANKED_MATCH, &ranked, sizeof(ranked));
        if (n) client_handle_game_event(client, NET_PACKET_EXT, payload, n);
        fprintf(stderr, "[input_script] ranked t=%.0f %d comp=%d\n", g_script.time, (int) (e->v[0] != 0), (int) (e->v[1] != 0));
    } else if (!strcmp(e->cmd, "lrmr")) {
        // lrmr <mmr_change> <latest_rank> <rp_change>: the rating reply's packet (accounts.c acc_send_latest_mmr) through the
        // client's packet path (game_events.c -> hud_endscreen_v10_ranked): the end screen's rank card and "+N RP"
        const NetLatestRankedMmrPacket packet = {.mmr_change = (int32_t) e->v[0], .latest_rank = (int32_t) e->v[1],
                                                 .rp_change = (int32_t) e->v[2]};
        uint8_t payload[NET_EXT_HEADER_SIZE + sizeof(packet)];
        const uint16_t n = net_ext_pack(payload, sizeof(payload), NET_EXT_LATEST_RANKED_MMR_CHANGE, &packet, sizeof(packet));
        if (n) client_handle_game_event(client, NET_PACKET_EXT, payload, n);
        fprintf(stderr, "[input_script] lrmr t=%.0f %d %d %d\n", g_script.time, packet.mmr_change, packet.latest_rank, packet.rp_change);
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
    } else if (!strcmp(e->cmd, "waitfor")) {
        snprintf(g_script.wait_sel, sizeof(g_script.wait_sel), "%s", e->arg);
        g_script.wait_start = glfwGetTime();
        fprintf(stderr, "[input_script] waitfor t=%.0f %s\n", g_script.time, e->arg);
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
    } else if (!strcmp(e->cmd, "rows")) {
        // rows @label <selector>: one "[input_script] row" line per match (the first 96, document order), its
        // textContent folded as `text` does: the whole text of each row, so a test can read every chat line exactly
        char label[64] = "", sel[192] = "";
        sscanf(e->arg, "%63s %191[^\n]", label, sel);
        HtmlElem els[96];
        const int n = html_ui_available() && sel[0] ? html_dom_query_all(0, sel, els, 96) : 0;
        fprintf(stderr, "[input_script] rows t=%.0f %s n=%d\n", g_script.time, label, n);
        char *text = malloc(65536);
        for (int i = 0; text && i < n && i < 96; i++) {
            html_dom_get_text(els[i], text, 65536);
            char folded[256];
            size_t k = 0;
            for (const char *s = text; *s && k + 1 < sizeof(folded); s++) {
                const int space = *s == ' ' || *s == '\n' || *s == '\t' || *s == '\r';
                if (space && (!k || folded[k - 1] == ' ')) continue;
                folded[k++] = space ? ' ' : *s;
            }
            while (k && folded[k - 1] == ' ') k--;
            folded[k] = 0;
            fprintf(stderr, "[input_script] row t=%.0f %s text=%s\n", g_script.time, label, folded);
        }
        free(text);
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
    } else if (!strcmp(e->cmd, "attr")) {
        // attr @label <name> <selector>: the first match's attribute ("-" when absent or no match)
        char label[64] = "", name[64] = "", sel[192] = "", value[256] = "-";
        sscanf(e->arg, "%63s %63s %191[^\n]", label, name, sel);
        const int n = html_ui_available() && sel[0] ? html_dom_query_all(0, sel, NULL, 0) : 0;
        const HtmlElem el = n > 0 ? html_dom_query(0, sel) : 0;
        if (!el || html_dom_get_attr(el, name, value, sizeof(value)) < 0) snprintf(value, sizeof(value), "-");
        fprintf(stderr, "[input_script] attr t=%.0f %s n=%d %s=%s\n", g_script.time, label, n, name, value);
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
    } else if (!strcmp(e->cmd, "builds")) {
        int n = 0, predicted = 0, visible = 0, spawned_predicted = 0;
        const Map *map = client->game.map;
        const Object *last = NULL;
        for (size_t i = 0; map && i < map->object_count; i++) {
            const Object *o = map->objects[i];
            spawned_predicted += o->spawned && o->predicted_ms > 0.0f;
            if (!o->dynamic || !o->active || o->destroyed) continue;
            n++;
            predicted += o->predicted_ms > 0.0f;
            last = o;
        }
        const int meshes = map_post_build_meshes(&visible);
        char at[96] = "-";
        if (last) snprintf(at, sizeof(at), "%.2f,%.2f,%.2f", last->position.x, last->position.y, last->position.z);
        fprintf(stderr, "[input_script] builds t=%.0f %s n=%d predicted=%d meshes=%d visible=%d last=%s spawned=%d draws=%d spawned_predicted=%d\n",
                g_script.time, e->arg[0] ? e->arg : "-", n, predicted, meshes, visible, at, map_post_spawned_meshes(), map_post_build_draws(),
                spawned_predicted);
    } else if (!strcmp(e->cmd, "aimlast")) {
        // aimlast: the look turns onto the centre of the newest object (the crate the Object Manager spawned last),
        // as `tp` sets the look: the crosshair is on it for the Physics Gun's grab and pick
        const Player *me = client->me;
        const Map *map = client->game.map;
        if (me && map && map->object_count) {
            const Object *o = map->objects[map->object_count - 1];
            const float eye = me->position.y + me->height - game_constants.camera_height;
            const float dx = o->position.x - me->position.x, dy = o->position.y + o->scale.y * 0.5f - eye, dz = o->position.z - me->position.z;
            client->me->direction = (vec2) {atan2f(dy, sqrtf(dx * dx + dz * dz)), atan2f(-dx, -dz)};
        }
    } else if (!strcmp(e->cmd, "toast")) {
        // toast @label: the toasts issued so far (ui_notify): "[input_script] toast t=100 @label n=3"
        fprintf(stderr, "[input_script] toast t=%.0f %s n=%d\n", g_script.time, e->arg[0] ? e->arg : "-", ui_notify_issued());
    } else if (!strcmp(e->cmd, "omres")) {
        // omres <status>: an Object Manager answer to a Physics Gun edit (a SPAWN_RESULT with action EDIT) through
        // the client's packet handler, as the server sends one to an edit that was on its way when we died
        // (status 7 = Spawn in first): "[input_script] omres t=100 status=7"
        NetSpawnResultPacket res;
        memset(&res, 0, sizeof(res));
        res.action = OM_ACTION_EDIT;
        res.item = OM_EDIT_SOURCE_PHYSGUN;
        res.status = (uint8_t) e->v[0];
        res.caps = (uint8_t) object_manager_ui_caps();
        client_handle_game_event(client, NET_PACKET_SPAWN_RESULT, &res, (uint16_t) sizeof(res));
        fprintf(stderr, "[input_script] omres t=%.0f status=%d\n", g_script.time, (int) res.status);
    } else if (!strcmp(e->cmd, "pg")) {
        // pg @label: the Physics Gun's carry (om_build.c) and the object under the crosshair (the pick the
        // gun takes on fire): "[input_script] pg t=100 @label grab=1 uid=5 frozen=0 releasing=0 drag=0 sel=-1
        // aim=5 aimfeet=x,y,z hold=x,y,z" (uid -1 none; aimfeet the crosshair's object's feet)
        int st[6];
        float shown[3], sent[3], target[3];
        om_build_probe(st, shown, sent, target);
        ObjectManagerAim aim;
        const int hit = client->me && object_manager_pick(&client->game, client->me, client->me->direction.x, client->me->direction.y, &aim);
        const Object *ao = hit && aim.object >= 0 ? client->game.map->objects[aim.object] : NULL;
        const Object *ho = st[1] >= 0 ? client->game.map->objects[st[1]] : NULL;
        char af[80] = "-", hf[80] = "-";
        if (ao) snprintf(af, sizeof(af), "%.2f,%.2f,%.2f", ao->position.x, ao->position.y, ao->position.z);
        if (ho) snprintf(hf, sizeof(hf), "%.2f,%.2f,%.2f", ho->position.x, ho->position.y, ho->position.z);
        const int ai = hit ? aim.object : -1;
        const int dev = (object_manager_ui_caps() & OM_CAP_DEV) != 0;
        fprintf(stderr, "[input_script] pg t=%.0f %s shown=%.2f,%.2f,%.2f sent=%.2f,%.2f,%.2f target=%.2f,%.2f,%.2f grab=%d uid=%d frozen=%d releasing=%d drag=%d sel=%d aim=%d aimfeet=%s hold=%s "
                "edit=%d takes=%d gun=%d caps=%d shoot=%d map=%d\n",
                g_script.time, e->arg[0] ? e->arg : "-", shown[0], shown[1], shown[2], sent[0], sent[1], sent[2], target[0], target[1], target[2], st[0], st[1], st[2], st[3], st[4], st[5], ai, af, hf,
                ai >= 0 && object_manager_editable(&client->game, ai), ai >= 0 && object_manager_physgun_takes(&client->game, ai, dev),
                client->me && client->me->weapon ? client->me->weapon->kn_physgun : -1, object_manager_ui_caps(),
                client->me ? (int) client->me->active : -1, ai >= 0 && object_manager_is_map_object(&client->game, ai));
        if (client->game.map && client->game.map->object_count) {
            const Object *lo = client->game.map->objects[client->game.map->object_count - 1];
            fprintf(stderr, "[input_script] pg t=%.0f %s last=%zu spawned=%d at=%.2f,%.2f,%.2f\n", g_script.time, e->arg[0] ? e->arg : "-",
                    client->game.map->object_count - 1, lo->spawned, lo->position.x, lo->position.y, lo->position.z);
        }
    } else if (!strcmp(e->cmd, "placewatch")) {
        snprintf(g_script.pw_label, sizeof(g_script.pw_label), "%s", e->arg[0] ? e->arg : "-");
        map_post_build_meshes(&g_script.pw_base);
        g_script.pw_frames = 0;
        g_script.pw_start = glfwGetTime();
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
    } else if (!strcmp(e->cmd, "pings")) {
        // pings: the v10 ping markers ("mpos", hud_map.c) on screen now and whether we spectate
        fprintf(stderr, "[input_script] pings t=%.0f shown=%d spect=%d\n", g_script.time, hud_ping_markers_shown(), spectate_active());
    } else if (!strcmp(e->cmd, "jugg")) {
        // jugg: the v10 Juggernaut markers (hud_objectives.c) the overlay drew last frame
        fprintf(stderr, "[input_script] jugg t=%.0f shown=%d\n", g_script.time, hud_objectives_juggernauts());
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
    } else if (!strcmp(e->cmd, "poses")) {
        // poses: each player's pose on its model (player_pose_describe) and the attached script objects per pass
        char line[4096];
        size_t o = 0;
        for (size_t i = 0; i < client->game.player_count && o + 200 < sizeof(line); i++) {
            const Player *p = client->game.players[i];
            if (!p) continue;
            char rec[192];
            player_pose_describe(p, rec, sizeof(rec));
            o += (size_t) snprintf(line + o, sizeof(line) - o, " %d%s:%s", p->uid, p == client->me ? "*" : "", rec);
        }
        line[o] = 0;
        int fps = 0, world = 0;
        ks_scene_attached_layers(&fps, &world);
        fprintf(stderr, "[input_script] poses t=%.0f fps=%d world=%d%s\n", g_script.time, fps, world, line);
    } else if (!strcmp(e->cmd, "models")) {
        // models @label: each player's model, "uid:body=..,mdl=..,asset=..,clip=.." (player_class_model_describe)
        char line[4096];
        size_t o = 0;
        for (size_t i = 0; i < client->game.player_count && o + 240 < sizeof(line); i++) {
            const Player *p = client->game.players[i];
            if (!p) continue;
            char model[224];
            player_class_model_describe(p, model, sizeof(model));
            for (char *c = model; *c; c++)
                if (*c == ' ') *c = ',';
            o += (size_t) snprintf(line + o, sizeof(line) - o, " %d:%s%s", p->uid, model, p == client->me ? "*" : "");
        }
        line[o] = 0;
        fprintf(stderr, "[input_script] models t=%.0f %s n=%zu%s\n", g_script.time, e->arg, client->game.player_count, line);
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
    } else if (!strcmp(e->cmd, "script")) {
        // script [uid]: v10 KrunkScript entities / runtime blocks (entities.c), our feet and, with a uid,
        // that map object's bottom centre (objmv checks)
        char info[384];
        entities_describe(client, info, sizeof(info));
        const Player *me = client->me;
        const Map *map = client->game.ready ? client->game.map : NULL;
        const int uid = (int) e->v[0];
        const Object *o = map && uid > 0 && (size_t) uid < map->object_count ? map->objects[uid] : NULL;
        fprintf(stderr, "[input_script] script t=%.0f %s me=%.2f,%.2f,%.2f ground=%d obj=%.2f,%.2f,%.2f lockshoot=%d\n", g_script.time, info,
                me ? me->position.x : 0.0f, me ? me->position.y : 0.0f, me ? me->position.z : 0.0f, me ? me->on_ground : 0,
                o ? o->position.x : 0.0f, o ? o->position.y : 0.0f, o ? o->position.z : 0.0f, me ? me->lock_shooting : -1);
    } else if (!strcmp(e->cmd, "prop")) {
        // prop <sid>: a player's Prop Hunt state (shared/include/prop_hunt.h, the official "0"[21] / "up" / "prpR"
        // packets): the prop it wears, its switches, the rotation and the saved yaw, its team
        const int sid = (int) e->v[0];
        const Player *p = client->game.ready ? game_find_player(&client->game, sid) : NULL;
        fprintf(stderr, "[input_script] prop t=%.0f sid=%d id=%d switches=%d rot=%d saved=%.2f set=%d team=%d\n", g_script.time, sid,
                p ? p->prop_id : -1, p ? p->prop_switches : -1, p ? p->prop_rot : -1, p ? p->prop_rot_saved : 0.0f,
                p ? p->prop_rot_saved_set : -1, p ? p->team : -1);
    } else if (!strcmp(e->cmd, "world")) {
        // world: the mode objectives / world objects the official world packets drive (krunker_link_world.c):
        // #gameMessage, Domination zones (capturedBy/captureT ms), spawnables (type:sid), destroyed map
        // objects, AIs, airdrops, the zombie wave, lockout (ms), rounds left and the round wins
        Game *game = &client->game;
        const GameMode *mode = game->mode;
        char dom[160] = "", tags[256] = "";
        size_t o = 0;
        for (int z = 0; mode && z < mode->dom_zone_count && o + 24 < sizeof(dom); z++)
            o += (size_t) snprintf(dom + o, sizeof(dom) - o, "%s%d/%.0f", z ? "," : "", mode->dom_zones[z].captured_by,
                                   mode->dom_zones[z].capture_t * 1000.0f);
        o = 0;
        for (int i = 0; i < MODE_MAX_SPAWNABLES && o + 24 < sizeof(tags); i++) {
            int data_c = 0;
            const int type = hud_modes_spawnable_type(i, &data_c);
            vec3 at;
            int team = 0;
            if (type < 0 || !hud_modes_spawnable(i, &at, &team)) continue;
            o += (size_t) snprintf(tags + o, sizeof(tags) - o, "%s%d@%.0f,%.0f", o ? "," : "", type, at.x, at.z);
        }
        int destroyed = 0, drops = 0;
        char low[64] = "";
        for (size_t i = 0, n = 0; game->map && i < game->map->object_count; i++) {
            if (!game->map->objects[i]->destroyed) continue;
            destroyed++;
            if (i < 16 && n + 4 < sizeof(low)) n += (size_t) snprintf(low + n, sizeof(low) - n, "%s%zu", n ? "," : "", i); // the uids below 16
        }
        const int ais = game->ready ? game_ai_active_count(game, 0) : 0;
        for (int i = 0; i < AIRDROP_MAX; i++) drops += game->zombies.drops[i].active;
        fprintf(stderr, "[input_script] world t=%.0f msg=\"%s\" dom=%s tags=%s destroyed=%d:%s ai=%d drops=%d wave=%d lock=%.0f rounds=%d wins=%d/%d\n",
                g_script.time, hud_modes_game_message(), dom[0] ? dom : "-", tags[0] ? tags : "-", destroyed, low[0] ? low : "-", ais, drops, game->zombies.wave,
                mode ? mode->lockout_timer * 1000.0f : -1.0f, mode ? mode->round_count : -1, mode ? mode->team_round_wins[1] : -1,
                mode ? mode->team_round_wins[2] : -1);
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
    } else if (!strcmp(e->cmd, "hands")) {
        // hands: both first-person hands as drawn (nettspend_vfx_e2e.py): the trigger hand's and the off
        // hand's centres (world) and where they land in the window (0..1, y down), plus the Drank's
        // bottle centre, its visibility and where it lands, and whether the off arm is drawn ("none"
        // without arms)
        vec3 trigger, off, bottle;
        int bottle_visible = 0, off_visible = 0;
        if (client->me && player_hands_points(client->me, &trigger, &off, &bottle, &bottle_visible, &off_visible)) {
            const vec2 st = script_project(client, trigger), so = script_project(client, off), sb = script_project(client, bottle);
            fprintf(stderr, "[input_script] hands t=%.0f trigger=%.4f,%.4f,%.4f off=%.4f,%.4f,%.4f screen_trigger=%.4f,%.4f screen_off=%.4f,%.4f "
                    "bottle=%.4f,%.4f,%.4f visible=%d screen_bottle=%.4f,%.4f off_visible=%d\n",
                    g_script.time, trigger.x, trigger.y, trigger.z, off.x, off.y, off.z, st.x, st.y, so.x, so.y, bottle.x, bottle.y, bottle.z,
                    bottle_visible, sb.x, sb.y, off_visible);
        } else {
            fprintf(stderr, "[input_script] hands t=%.0f none\n", g_script.time);
        }
    } else if (!strcmp(e->cmd, "arms")) {
        // arms: the trigger arm's and the off arm's shoulder, elbow and wrist as drawn, world space (player_arm_joints;
        // nettspend_arms_e2e.py checks the elbows' bend)
        for (int off = 0; off < 2; off++) {
            vec3 shoulder, elbow, wrist;
            if (client->me && player_arm_joints(client->me, off, &shoulder, &elbow, &wrist)) {
                fprintf(stderr, "[input_script] arms t=%.0f %s shoulder=%.4f,%.4f,%.4f elbow=%.4f,%.4f,%.4f wrist=%.4f,%.4f,%.4f\n",
                        g_script.time, off ? "off" : "trigger", shoulder.x, shoulder.y, shoulder.z, elbow.x, elbow.y, elbow.z, wrist.x, wrist.y, wrist.z);
            } else {
                fprintf(stderr, "[input_script] arms t=%.0f %s none\n", g_script.time, off ? "off" : "trigger");
            }
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
    } else if (!strcmp(e->cmd, "wars")) {
        // wars <id> <name> <colour> [logo] | wars none: a "warsClan" through the client's EXT handler (s2c_events.c)
        NetWarsClanPacket w;
        memset(&w, 0, sizeof(w));
        int id = 0;
        char name[32] = "", color[24] = "", logo[64] = "";
        if (strcmp(e->arg, "none") != 0 && sscanf(e->arg, "%d %31s %23s %63s", &id, name, color, logo) >= 2) {
            w.has = 1;
            w.clan_id = id;
            snprintf(w.clan_name, sizeof(w.clan_name), "%s", name);
            // '#' starts a script comment: "ff3b3b" stands for "#ff3b3b" ("s2" is paint shader 2, "-" no colour)
            const int hex = color[0] && strspn(color, "0123456789abcdefABCDEF") == strlen(color);
            snprintf(w.banner_color, sizeof(w.banner_color), "%s%s", hex ? "#" : "", strcmp(color, "-") ? color : "");
            snprintf(w.clan_logo, sizeof(w.clan_logo), "%s", logo);
        }
        s2c_events_on_ext(client, NET_EXT_WARS_CLAN, (const uint8_t *) &w, (uint16_t) sizeof(w));
        fprintf(stderr, "[input_script] wars t=%.0f has=%d id=%d name=%s color=%s logo=%s\n", g_script.time, (int) w.has, w.clan_id,
                w.clan_name, w.banner_color, w.clan_logo);
    } else if (!strcmp(e->cmd, "spinw")) {
        // spinw <uid> <rand> <skin>: a "spinW" for another player (no account change) through the EXT handler
        const NetSpinWheelPacket w = {.uid = (int32_t) e->v[0], .rand = e->v[1], .skin = (int32_t) e->v[2]};
        s2c_events_on_ext(client, NET_EXT_SPIN_WHEEL_RESPONSE, (const uint8_t *) &w, (uint16_t) sizeof(w));
        fprintf(stderr, "[input_script] spinw t=%.0f uid=%d rand=%g skin=%d\n", g_script.time, w.uid, (double) w.rand, w.skin);
    } else if (!strcmp(e->cmd, "wheelrar")) {
        // wheelrar <wheel> <odds...>: store wheel <wheel>'s pools become rarity odds (test only, see the top)
        const StoreSpins *spins = store_spins_builtin();
        char args[256];
        snprintf(args, sizeof(args), "%s", e->arg);
        char *p = args;
        const long wheel = strtol(p, &p, 10);
        if (wheel >= 0 && wheel < spins->wheel_count) {
            StoreWheel *w = &spins->wheels[wheel];
            w->pool_count = 0;
            w->slices = 0;
            for (char *end; w->slices < STORE_WHEEL_SLICES; p = end) {
                const double odds = strtod(p, &end);
                if (end == p) break;
                w->weights[w->slices++] = odds;
            }
            fprintf(stderr, "[input_script] wheelrar t=%.0f wheel=%ld slices=%d\n", g_script.time, wheel, w->slices);
        }
    } else if (!strcmp(e->cmd, "wheels")) {
        // wheels [@label]: the map prize wheels' spin state and planes, the Turf Wars banners
        char line[2048];
        int at = snprintf(line, sizeof(line), "[input_script] wheels t=%.0f %s n=%d planes=%d banners=%d", g_script.time, e->arg,
                          prize_wheels_count(client), prize_wheel_render_count(), wars_banners_count());
        for (int i = 0; i < prize_wheels_count(client) && at < (int) sizeof(line) - 160; i++) {
            const PrizeWheel *w = prize_wheels_get(client, i);
            vec3 item_at = {0};
            const int up = prize_wheel_render_item(i, &item_at);
            at += snprintf(line + at, sizeof(line) - (size_t) at, " %d:wheel=%d,rot=%.2f,mlt=%.2f,end=%.1f/%.1f,item=%d,paints=%u,tex=%d,up=%d@%.2f,%.2f,%.2f",
                           i, w->wheel, (double) w->spin_rotation, (double) w->spin_whl_mlt, (double) w->end_anim_val, (double) w->end_anim_target,
                           w->item_id, w->paints, prize_wheel_render_texture(i) != 0, up, (double) item_at.x, (double) item_at.y, (double) item_at.z);
        }
        fprintf(stderr, "%s\n", line);
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
    } else if (!strcmp(e->cmd, "netchat")) {
        // netchat <kind> <text>: a server chat line as the server would send it, through the client's
        // event handler (chat_switch_e2e.py). 0 a human player's line (uid 2, a guest: no PLAYER_INFO);
        // 3 a bot's line (uid 1, after the bot PLAYER_INFO the server sends with its spawn, bots.c
        // bot_send_info); 1 a game line; 2 a server line.
        int kind = 0;
        char text[NET_CHAT_TEXT_SIZE] = "";
        if (sscanf(e->arg, "%d %287[^\n]", &kind, text) >= 1 && kind >= 0 && kind <= 3) {
            NetChatPacket chat;
            if (kind == 3) {
                NetPlayerInfoPacket info;
                memset(&info, 0, sizeof(info));
                info.uid = 1;
                info.flags = NET_PLAYER_INFO_BOT;
                player_accounts_on_info(&info);
                net_packet_chat(&chat, 1, NET_CHAT_PLAYER, "Bot", text);
            } else if (kind == 0) {
                net_packet_chat(&chat, 2, NET_CHAT_PLAYER, "Guest", text);
            } else {
                net_packet_chat(&chat, -1, (NetChatKind) kind, NULL, text);
            }
            client_handle_game_event(client, NET_PACKET_CHAT, &chat, (uint16_t) sizeof(chat));
        }
    } else if (script_probe(e->cmd) && strcmp(e->cmd, "console")) {
        // vis <id> [<id> ...]: one log line with each element's visibility (1 shown, 0 not, - absent);
        // scroll <id> [<id> ...]: their scrollTop instead; rect <id> [<id> ...]: their boxes
        const int style = !strcmp(e->cmd, "style");
        const int scroll = !style && e->cmd[0] == 's';
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
            if (style) { // id:property -> the inline value, spaces as '_' ("57%_100%"), - unset
                char *colon = strchr(id, ':');
                if (colon) *colon = 0;
                const HtmlElem el = colon && html_ui_available() ? html_dom_by_id(id) : 0;
                char value[128];
                if (!el || html_dom_get_style(el, colon + 1, value, sizeof(value)) < 0 || !value[0]) snprintf(value, sizeof(value), "-");
                for (char *v = value; *v; v++) if (*v == ' ') *v = '_';
                n += snprintf(line + n, sizeof(line) - (size_t) n, " %s:%s=%s", id, colon ? colon + 1 : "", value);
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
    } else if (!strcmp(e->cmd, "spawn")) {
        fprintf(stderr, "[input_script] spawn t=%.0f\n", g_script.time);
        g_script.spawn_wait = 1; // the clock stops until we spawn and the menu is closed (spawn_waiting)
    } else if (!strcmp(e->cmd, "con")) {
        ui_console_exec(e->arg); // con <word>: a one-word developer console command (`end`)
    } else if (!strcmp(e->cmd, "nethit")) {
        const NetHitPacket hit = {.target_uid = -1, .kill = (uint8_t) (e->v[0] != 0.0f)};
        client_handle_game_event(client, NET_PACKET_HIT, &hit, (uint16_t) sizeof(hit));
    } else if (!strcmp(e->cmd, "damage")) {
        Player *target = NULL;
        for (size_t i = 0; client->me && !target && i < client->game.player_count; i++) {
            Player *p = client->game.players[i];
            if (p && p != client->me && p->active) target = p;
        }
        if (target) {
            const float *m = client->camera.world_inverse_matrix; // row 2: the camera's back axis
            target->position = (vec3) {client->camera.position.x - m[8] * 20.0f, client->camera.position.y - 7.0f - m[9] * 20.0f,
                                       client->camera.position.z - m[10] * 20.0f};
            const NetHitPacket hit = {.target_uid = target->uid, .damage = (int32_t) lroundf(e->v[0]), .crit = (uint8_t) (e->v[1] != 0.0f)};
            const int32_t tenths = (int32_t) lroundf(e->v[0] * 10.0f);
            if (tenths % 10) {
                const NetHitDamagePacket exact = {.target_uid = hit.target_uid, .damage_tenths = tenths};
                uint8_t ext[NET_EXT_HEADER_SIZE + sizeof(exact)];
                const uint16_t n = net_ext_pack(ext, sizeof(ext), NET_EXT_KN_HIT_DAMAGE, &exact, sizeof(exact));
                if (n) client_handle_game_event(client, NET_PACKET_EXT, ext, n);
            }
            client_handle_game_event(client, NET_PACKET_HIT, &hit, (uint16_t) sizeof(hit));
        }
        fprintf(stderr, "[input_script] damage t=%.0f value=%.1f target=%d\n", g_script.time, e->v[0], target ? target->uid : -1);
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
        client_window_focus_changed(client, 0); // alt-tab: the GLFW focus callback (main.c)
    } else if (!strcmp(e->cmd, "focus")) {
        client_window_focus_changed(client, 1); // back in the window
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

// `spawn`: the clock stands still until our player is active and the menu is closed (the loading after
// a Play click can take seconds, so a fixed time would race it); then the script goes on.
static int spawn_waiting(Client *client) {
    if (!g_script.spawn_wait) return 0;
    if (!client->me || !client->me->active || (html_ui_available() && html_ui_is_menu_open())) return 1;
    g_script.spawn_wait = 0;
    return 0;
}

// the clock stands still while `page` or `spawn` holds the script
static int clock_held(void) { return g_script.page_wait || g_script.spawn_wait || g_script.wait_sel[0]; }

// `page`: 1 while the page it asked for is not shown and ready yet (the game page: its menu set up),
// then one more frame so the page's UI ticked once; no events run and the clock stands still.
// `waitfor <selector>` holds the script until a node matches, `waitfor !<selector>` until none does; a wait that
// outlives WAIT_SECONDS_MAX (wall clock: a loaded host runs few frames) is logged and dropped, so a failed wait shows
// in the log, not as a hang
#define WAIT_SECONDS_MAX 90.0

static int page_waiting(Client *client) {
    if (spawn_waiting(client)) return 1;
    if (g_script.wait_sel[0]) {
        const char *sel = g_script.wait_sel;
        const int none = sel[0] == '!'; // `waitfor !<selector>`: until no node matches
        if (none) sel++;
        const int n = html_ui_available() ? html_dom_query_all(0, sel, NULL, 0) : 0;
        if (none ? n == 0 : n > 0) {
            fprintf(stderr, "[input_script] waitfor done %s after %.1f s\n", g_script.wait_sel, glfwGetTime() - g_script.wait_start);
            g_script.wait_sel[0] = 0;
            return 0;
        }
        if (glfwGetTime() - g_script.wait_start >= WAIT_SECONDS_MAX) {
            fprintf(stderr, "[input_script] waitfor timeout %s\n", g_script.wait_sel);
            g_script.wait_sel[0] = 0;
            return 0;
        }
        return 1;
    }
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
        if (clock_held()) break; // `page` / `spawn` / `waitfor`: the rest waits for that page, our spawn or the waitfor
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

    if (g_script.pw_label[0]) { // `placewatch`: this frame's build meshes are in the scene (map_post_frame ran)
        int visible = 0;
        map_post_build_meshes(&visible);
        const double ms = (glfwGetTime() - g_script.pw_start) * 1000.0;
        if (visible > g_script.pw_base || ms > 5000.0) {
            if (visible > g_script.pw_base)
                fprintf(stderr, "[input_script] place t=%.0f %s frames=%d ms=%.1f visible=%d\n", g_script.time, g_script.pw_label, g_script.pw_frames, ms, visible);
            else fprintf(stderr, "[input_script] place t=%.0f %s timeout frames=%d\n", g_script.time, g_script.pw_label, g_script.pw_frames);
            g_script.pw_label[0] = 0;
        } else {
            g_script.pw_frames++;
        }
    }

    write_pending_shot(client);

    // The frame time, not the world's: the end-of-round slow motion scales the world delta
    // down to 0 for the whole results screen, which would hold every later event back.
    if (!clock_held()) g_script.time += frame_delta * 1000.0f;
}

void input_script_idle_frame(Client *client, const float frame_delta) {
    script_load();
    if (!g_script.active || !g_script.count || strcmp(g_script.events[0].cmd, "boot") != 0) return;
    input_script_tick(client);
    if (g_script.playing) {
        write_pending_shot(client);
        if (!clock_held()) g_script.time += frame_delta * 1000.0f;
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
        static const char *const GAME_ONLY[] = {"play", "pose", "tp", "look", "respawn", "recon"};
        int skip = 0;
        for (size_t i = 0; i < sizeof(GAME_ONLY) / sizeof(GAME_ONLY[0]); i++) skip |= !strcmp(e->cmd, GAME_ONLY[i]);
        if (!skip) script_run(client, e);
        ran = 1;
        if (clock_held()) break; // `page` / `spawn` / `waitfor`: the rest waits for that page, our spawn or the waitfor
    }
}

void input_script_page_frame_end(Client *client, const float frame_delta) {
    if (!g_script.active || !g_script.playing) return;
    write_pending_shot(client);
    if (!clock_held()) g_script.time += frame_delta * 1000.0f;
}

void input_script_frame_presented(Client *client) {
    if (!g_script.active || !g_script.playing || g_script.frames_left <= 0) return;
    char path[200];
    snprintf(path, sizeof(path), "%s_%04d.png", g_script.frames, g_script.frames_index++);
    write_frame_png(client, path);
    // orders the captures against the log; the developer console's state as this frame was painted
    // (ui_console_probe), so a per-frame test knows which frames must show it (console_draw_sweep_e2e.py)
    char con[320];
    ui_console_probe(con, sizeof(con));
    fprintf(stderr, "[input_script] frame %s t=%.0f %s\n", path, g_script.time, con);
    g_script.frames_left--;
}
