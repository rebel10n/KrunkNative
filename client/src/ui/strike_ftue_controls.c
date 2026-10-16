// Krunker Strike's controls tutorial (strike_ftue.h): FRVR's controls_ftue flow, run in the first run's sandbox.
// Sources: FRVR Krunker Strike 1.37.2, read as text only (never executed):
//   ftue_flows.controls_ftue   tools/strike/strike_data.json, /ui/entries/1.27.0/default_features (the step
//                              ids, tutor texts, hands and timers; the 1.37.2 code uses the same ids)
//   nB (CONTROLS_FTUE)         assets/index-Cz2uYXJQ.deob.js ~110380-110550: loadStep, the tap blur, the view
//   engine (Qce)               ~96230-96420: start, nextStep(flow, id) advances only from step id, forceStep,
//                              end (a finished flow is stored as "done"), the nextStepTimer
//   touch hooks                ~146230-146305: R (move start) -> move_controls, V (move end) -> move_interaction,
//                              H (look start) -> camera_controls, j (look end) -> camera_interaction, the jump
//                              and reload binds -> jump and reload
// The steps that need FRVR's FTUE bots (enemies_near*, enemies_far*: FTUE_BOTS / FTUE_RESPAWN, which the
// engine does not have) are left out; ui/strike/README.md lists them with the reason.
#include <client.h>
#include <html_dom.h>
#include <html_ui.h>
#include <map_catalog.h>
#include <strike_ftue.h>
#include <ui_logic.h>
#include <stdio.h>
#include <string.h>

#define CONTROLS_FLOW "controls_ftue"  // the storage key (strike_ftue_step_done / strike_ftue_end)
#define SANDBOX_MAP "bot_ftue_v2"      // FRVR's ftue.fileName "Mini Industry Tutorial" (the Strike copy)
#define TAP_TEXT "Tap anywhere to continue"

// One step of ftue_flows.controls_ftue: the loadStep parameters that the view and the engine read.
typedef struct {
    const char *id;
    const char *bottom;      // bottomText (HTML), NULL none
    const char *tutor;       // the tutor bubble (HTML), NULL none
    int tutor_left;          // side "left" (else "right")
    int tutor_check;         // showCheck
    const char *middle;      // middleImage src, NULL none
    int middle_flipped;
    int enable;              // enableControls: the touches reach the game
    int hide_blur;           // hideBlur: no tap blur
    int no_tap;              // disableTapToPass: the blur does not pass the step
    int timer_ms;            // nextStepTimer: the step ends by itself
    const char *hand_side;   // handAnimation side, NULL none
    const char *hand_anim;   // handAnimation animation
    int ripple;              // rippleEffect
    int delayed_hand_ms;     // delayedParameters (welcome): the hand and the ripple come after this long
    int arrows;              // camera_controls: the look arrows
    int reload_hl;           // reload: the highlight and its hand
    int complete;            // complete_go: "Let's Go!" and its hand
    int advance;             // the StrikeFtueEvent that ends the step, -1 none (a tap or the timer)
} CtlStep;

// The live steps, in FRVR's order. Not run (FRVR's FTUE bots): enemies_near, enemies_near_controls,
// enemies_near_interaction, enemies_near_end, enemies_far, enemies_far_controls, enemies_far_interaction,
// enemies_far_end. "reload" is run without them (its step needs only the reload control).
static const CtlStep STEPS[] = {
    {.id = "welcome", .bottom = TAP_TEXT, .tutor = "Welcome to Krunker!", .tutor_left = 1,
     .hand_side = "right", .hand_anim = "tap", .delayed_hand_ms = 6000, .advance = -1},
    {.id = "basics", .bottom = TAP_TEXT, .tutor = "Let's go through the basics to play", .tutor_left = 1, .advance = -1},
    {.id = "hold", .bottom = TAP_TEXT, .tutor = "First, hold your phone like this", .tutor_left = 1,
     .middle = "./img/ftue/controls/hands_hold.svg", .advance = -1},
    {.id = "move", .bottom = TAP_TEXT, .tutor = "Use the left side of your screen to <span style=\"color: #15C5D0\">WALK</span>",
     .tutor_left = 0, .middle = "./img/ftue/controls/hands_walk.svg", .advance = -1},
    {.id = "move_controls", .enable = 1, .hide_blur = 1, .hand_side = "left", .hand_anim = "vertical",
     .advance = STRIKE_FTUE_EV_MOVE_START},
    {.id = "move_interaction", .enable = 1, .hide_blur = 1, .advance = STRIKE_FTUE_EV_MOVE_END},
    {.id = "move_end", .no_tap = 1, .timer_ms = 3000, .tutor = "Well done!", .tutor_left = 1, .advance = -1},
    {.id = "camera", .bottom = TAP_TEXT, .tutor = "Use the right side of your screen to look around", .tutor_left = 1,
     .middle = "./img/ftue/controls/hands_walk.svg", .middle_flipped = 1, .advance = -1},
    {.id = "camera_controls", .enable = 1, .hide_blur = 1, .hand_side = "right", .hand_anim = "horizontal", .arrows = 1,
     .advance = STRIKE_FTUE_EV_LOOK_START},
    {.id = "camera_interaction", .enable = 1, .hide_blur = 1, .advance = STRIKE_FTUE_EV_LOOK_END},
    {.id = "camera_end", .no_tap = 1, .timer_ms = 3000, .tutor = "You're getting the hang of it!", .tutor_left = 1, .advance = -1},
    {.id = "jump", .enable = 1, .hide_blur = 1, .hand_side = "right", .hand_anim = "tap", .ripple = 1,
     .tutor = "If you tap anywhere, you jump. Give it a try!", .tutor_left = 1, .advance = STRIKE_FTUE_EV_JUMP},
    {.id = "jump_end", .hide_blur = 1, .no_tap = 1, .timer_ms = 3000, .tutor = "Parkour!", .tutor_left = 1, .advance = -1},
    {.id = "reload", .enable = 1, .hide_blur = 1, .reload_hl = 1,
     .tutor = "If you're running out of bullets, tap here to reload", .tutor_left = 1, .advance = STRIKE_FTUE_EV_RELOAD},
    {.id = "complete", .bottom = TAP_TEXT, .tutor = "This is it! Now you know all the basics.", .tutor_left = 1, .advance = -1},
    {.id = "complete_go", .tutor = "You are now ready for real battles!", .tutor_left = 1, .tutor_check = 1, .complete = 1,
     .advance = -1},
};
#define STEP_COUNT (sizeof(STEPS) / sizeof(STEPS[0]))

static struct {
    int pending;      // the first run: the flow starts once its game is ready
    int running;
    int index;        // the step in STEPS (-1 before the first)
    int timer;        // nextStepTimer's html timer, 0 none
    int delayed;      // delayedParameters' html timer, 0 none
    int delayed_done; // the welcome step's hand and ripple are on
} G;

static void cancel_timers(void) {
    if (G.timer) html_timer_cancel(G.timer);
    if (G.delayed) html_timer_cancel(G.delayed);
    G.timer = G.delayed = 0;
}

static void finish(void) {
    cancel_timers();
    G.running = 0;
    strike_ftue_end(CONTROLS_FLOW);
    strike_ftue_controls_view_clear();
    printf("[strike] ftue: controls done\n");
}

// The view of the current step (FRVR's nB render): the hand and ripple of a delayed step wait for their timer.
static void show(void) {
    const CtlStep *s = &STEPS[G.index];
    const int late = s->delayed_hand_ms > 0 && !G.delayed_done;
    const StrikeFtueControlsView v = {
        .bottom = s->bottom,
        .tutor = s->tutor,
        .tutor_left = s->tutor_left,
        .tutor_check = s->tutor_check,
        .middle = s->middle,
        .middle_flipped = s->middle_flipped,
        .blur = !s->hide_blur,
        .ignore_events = s->enable,
        .hand_side = late ? NULL : s->hand_side,
        .hand_anim = late ? NULL : s->hand_anim,
        .ripple = late ? 0 : s->ripple,
        .arrows = s->arrows,
        .reload_highlight = s->reload_hl,
        .complete_button = s->complete,
    };
    strike_ftue_controls_view(&v);
}

static void on_timer(void *user);
static void on_delayed(void *user);

static void load(const int index) {
    cancel_timers();
    G.index = index;
    G.delayed_done = 0;
    const CtlStep *s = &STEPS[index];
    strike_ftue_step_done(CONTROLS_FLOW, s->id);
    printf("[strike] ftue: controls step %s\n", s->id);
    show();
    if (s->timer_ms) G.timer = html_timer_add(s->timer_ms, on_timer, NULL);
    if (s->delayed_hand_ms) G.delayed = html_timer_add(s->delayed_hand_ms, on_delayed, NULL);
}

static void next(void) {
    if (!G.running) return;
    if (G.index + 1 >= (int) STEP_COUNT) {
        finish();
        return;
    }
    load(G.index + 1);
}

// html_timer_add callbacks: the ids are cleared first (a fired timer is not cancelled again)
static void on_timer(void *user) {
    (void) user;
    G.timer = 0;
    next();
}

static void on_delayed(void *user) {
    (void) user;
    G.delayed = 0;
    G.delayed_done = 1;
    if (G.running) show();
}

static void begin(void) {
    if (G.running || strike_ftue_completed(CONTROLS_FLOW)) return; // FRVR's start(): a finished flow does not run again
    G.running = 1;
    G.index = -1;
    next();
}

// The first run (before its quick connect): the sandbox becomes the game's start map (local_game_pick_map)
// unless the flow has ended. The flow starts once that game is ready (strike_ftue_controls_tick).
void strike_ftue_controls_first_run(Client *client) {
    if (strike_ftue_completed(CONTROLS_FLOW)) return;
    const int map = map_catalog_index_of_key(SANDBOX_MAP);
    const int slot = client && map >= 0 ? local_game_pick_map(client, map) : -1;
    printf("[strike] ftue: controls sandbox %s (slot %d)\n", SANDBOX_MAP, slot);
    G.pending = 1;
}

void strike_ftue_controls_tick(Client *client) {
    if (G.running && html_ui_is_menu_open()) { // the game was left: the flow waits (its step is stored)
        cancel_timers();
        G.running = 0;
        strike_ftue_controls_view_clear();
        return;
    }
    if (G.pending && client && client->game.ready && !html_ui_is_menu_open()) {
        G.pending = 0;
        begin();
    }
}

// The touch controls and the keys report here; only the step's own event ends it (nextStep(flow, id)).
void strike_ftue_controls_event(const StrikeFtueEvent ev) {
    if (!G.running) return;
    if (STEPS[G.index].advance == (int) ev) next();
}

void strike_ftue_controls_control_down(const int ui_control) {
    if (ui_control == UI_CONTROL_JUMP) strike_ftue_controls_event(STRIKE_FTUE_EV_JUMP);
    else if (ui_control == UI_CONTROL_RELOAD) strike_ftue_controls_event(STRIKE_FTUE_EV_RELOAD);
}

// The blur is there unless hideBlur; a tap passes the step unless disableTapToPass (FRVR's onclick)
void strike_ftue_controls_tap(void) {
    if (!G.running) return;
    const CtlStep *s = &STEPS[G.index];
    if (!s->hide_blur && !s->no_tap) next();
}

// "Let's Go!" (complete_go): FRVR's Tx, then backToOrigin (the caller leaves the game)
void strike_ftue_controls_go(void) {
    if (G.running && STEPS[G.index].complete) next();
}

int strike_ftue_controls_running(void) { return G.running; }

int strike_ftue_controls_pointer_to_page(void) {
    if (!G.running) return 0;
    const CtlStep *s = &STEPS[G.index];
    return !s->hide_blur || s->complete; // the tap blur (hideBlur off) and "Let's Go!" are DOM targets in FRVR
}
