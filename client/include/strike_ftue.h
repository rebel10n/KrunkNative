#pragma once
// Krunker Strike's first-time user experience (FRVR ftue_flows; client/src/ui/strike_ftue.c).
//
//   first run   FRVR hasAlreadyPlayedBefore: the first start of an account on a touch device goes
//               to quickConnect (the FTUE runs in that game); a returning account goes to the menu
//   storage     per account: "played" and each flow's last step ("done" when the flow ended),
//               in the client's own knFtue entry (html_storage: $XDG_CONFIG_HOME/krunknative/knFtue.json)
//   overlay     FRVR's CAe (.ftue-overlay): the input blocker, the highlight of one element (its four
//               blockers and the hand), the middle image, the dark blur, the bottom text and the tutor
//               speech bubble. The page markup is built here; ui/strike/css/kn-strike-overrides.css
//               and the page's own CSS style it.
#include <client.h>
#include <html_dom.h>
#include <cJSON.h>

// The boot gate (strike_ui_tick, once the page has booted): 1 the first time this account starts
// the game (the flag is stored at once, as FRVR's hasAlreadyPlayedBefore does), else 0.
int  strike_ftue_first_run(void);

// The flow's progress for this account: a step id while it runs, "done" once it ended.
int  strike_ftue_completed(const char *flow);      // 1 when the flow ended ("done")
void strike_ftue_step_done(const char *flow, const char *step);
void strike_ftue_end(const char *flow);

// The overlay (FRVR CAe). Texts are HTML (FRVR's g.trust); NULL or "" clears the element.
void strike_ftue_bind(HtmlElem overlay);            // strike_ui.c: the page's .ftue-overlay root
void strike_ftue_highlight(const char *selector, int interactable); // a box around the element
void strike_ftue_unhighlight(void);
void strike_ftue_blur(int on);                       // .ftue-overlay__dark-blur
void strike_ftue_block_input(int on);                // .ftue-overlay__input-blocker
void strike_ftue_bottom_text(const char *html);      // .ftue-overlay__bottom-text
void strike_ftue_tutor(const char *html, int left, int check); // the tutor bubble (left or right side)
void strike_ftue_image(const char *src, int flipped); // .ftue-overlay__middle-image
void strike_ftue_clear(void);                        // every element off (a step's end)
void strike_ftue_tick(void);                         // per frame: the highlight follows its element

// --render-scenario: state.strike.ftue (strike_ui.c strike_ui_scenario)
//   {"highlight": "<selector>" | {"selector", "interactable"}, "blur": true, "blocker": true,
//    "bottomText": "<html>", "tutor": {"text", "side": "left"|"right", "check"}, "image": {"src", "flipped"}}
void strike_ftue_scenario(const cJSON *ftue);

// ---------------------------------------------------------------- the controls tutorial
// FRVR controls_ftue (the CONTROLS_FTUE view nB, bundle ~110380-110550; its step list in ftue_flows).
// The step engine and its sandbox are strike_ftue_controls.c's. The steps that need FRVR's FTUE bots
// (enemies_near*, enemies_far*: FTUE_BOTS / FTUE_RESPAWN) are not run; ui/strike/README.md lists them.
typedef enum {
    STRIKE_FTUE_EV_MOVE_START, // R: the move stick went down (touch)
    STRIKE_FTUE_EV_MOVE_END,   // V: the move stick was let go (touch)
    STRIKE_FTUE_EV_LOOK_START, // H: the look (camera) stick went down (touch)
    STRIKE_FTUE_EV_LOOK_END,   // j: the look stick was let go (touch)
    STRIKE_FTUE_EV_JUMP,       // the jump control went down (touch, keyboard or pad)
    STRIKE_FTUE_EV_RELOAD,     // the reload control went down (touch or keyboard)
} StrikeFtueEvent;

// The first run (strike_ftue_first_run() == 1, before the quick connect): when the flow has not ended, the
// sandbox map (FRVR's bot_ftue_v2) becomes the game's start map. The flow then starts once that game is ready.
void strike_ftue_controls_first_run(Client *client);
void strike_ftue_controls_tick(Client *client);   // per frame: starts the flow, runs the step timers
void strike_ftue_controls_event(StrikeFtueEvent ev);   // a touch control: ends the step it advances
void strike_ftue_controls_control_down(int ui_control); // UI_CONTROL_JUMP / UI_CONTROL_RELOAD pressed (touch or keys)
void strike_ftue_controls_tap(void);              // the .frvrMenuBlur was tapped (FRVR: Tx unless disableTapToPass)
void strike_ftue_controls_go(void);               // "Let's Go!" on complete_go (FRVR: Tx, then backToOrigin)
int  strike_ftue_controls_running(void);
// Desktop (no phone: strike_touch_layer() 0): there is no tap blur, the mouse reaches the game. A press is a tap
// (it passes a tap step, not a hideBlur / disableTapToPass one), the keys WASD advance move_controls and
// move_interaction, the mouse look camera_controls and camera_interaction (looking = the look moved this frame).
void strike_ftue_controls_desktop_press(void);
void strike_ftue_controls_desktop_frame(int looking);
// 1 while the step takes the pointer on the page (its tap blur on a phone, or the "Let's Go!" button): such presses
// are the page's clicks, not the game's (strike_ui_pointer_to_page)
int  strike_ftue_controls_pointer_to_page(void);

// The controls view (FRVR nB): the step's parameters, set by strike_ftue_controls.c. The strings are copied.
typedef struct {
    const char *bottom;       // bottomText (HTML), NULL none
    const char *tutor;        // the tutor's bubble text (HTML), NULL none
    int tutor_left;           // the tutor on the left side (else right)
    int tutor_check;          // showCheck
    const char *middle;       // middleImage src, NULL none
    int middle_flipped;
    int blur;                 // .frvrMenuBlur: a tap passes the step
    int ignore_events;        // enableControls: touches reach the game
    const char *hand_side;    // handAnimation side "left" / "right", NULL none
    const char *hand_anim;    // "vertical" / "horizontal" / "tap"
    int ripple;               // rippleEffect
    int arrows;               // camera_controls: the look arrows
    int reload_highlight;     // reload: the highlight and its hand
    int complete_button;      // complete_go: "Let's Go!" and its hand
} StrikeFtueControlsView;
void strike_ftue_controls_view(const StrikeFtueControlsView *v);
void strike_ftue_controls_view_clear(void);
