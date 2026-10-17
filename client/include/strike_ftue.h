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
