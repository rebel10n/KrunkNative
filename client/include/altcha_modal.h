#pragma once
// v10.0.7's Security Challenge (client/src/ui/altcha_modal.c): the game socket's `showCaptcha(challenge, cb,
// hideOnSolve)` (deob-10.0.7.js:98914-98968). A dimmed #captcha-holder on <body> with a white card: "Security
// Challenge", "Please complete the security challenge to continue." and the ALTCHA 1.0.6 widget
// (<altcha-widget challengejson>), which waits for the player's "I'm not a robot" click, then searches the proof of
// work ("Verifying...") and shows "Verified" with its hidden input altcha; 200 ms later (the bundle's poll) the
// callback gets the input's value. A search that fails leaves the widget's error, "Verification failed. Try again
// later.", and a click starts it again: the callback never hears of a failure (v10's cb(value, null) only).
//
// Shown for the in-game "cpt" (Box Office: kn_captcha_game.c; official games: krunker_net.c), which stays up after
// the answer until the server's "cptR" or "error" (altcha_modal_hide), and for a guest's hosting challenge
// (hide_on_solve: the card goes with the answer). The solving is kn_captcha_solve's (krunker_altcha.c, slices of the
// frame loop on every platform). While the card is up the pointer is the page's (krunker_captcha_needs_player).
#include <stddef.h>

typedef void (*AltchaModalDone)(void *user, const char *payload);
// The session the card belongs to is still there (polled while it is up); 0 removes it, as v10's socket reset
// does (deob-10.0.7.js:98984-98985). NULL: never removed that way.
typedef int (*AltchaModalAlive)(void *user);

// 1 = the page can show the card (the game page is up); 0 = it cannot (no HTML engine, another page): the caller
// solves the challenge itself.
int altcha_modal_available(void);
// showCaptcha: challenge_json is the ALTCHA challenge {algorithm, challenge, salt, signature, maxnumber?}. A card
// already up is kept (v10 builds it only while captchaHolder is null) and this call is dropped: 1 is returned, done
// never runs for it. 0 = altcha_modal_available() is 0 (nothing shown, done never runs).
int altcha_modal_show(const char *challenge_json, int hide_on_solve, AltchaModalDone done, AltchaModalAlive alive, void *user);
// _hideCaptcha (the server's "cptR", its "error", the socket's reset): the card goes; a search in flight stops.
void altcha_modal_hide(void);
int altcha_modal_open(void);

// The widget's markup in a state (tests and the module): 0 unverified, 1 verifying, 2 verified (payload = the hidden
// input's value), 3 error. challenge_json goes in the challengejson attribute. Returns malloc'd HTML.
enum { ALTCHA_UNVERIFIED = 0, ALTCHA_VERIFYING, ALTCHA_VERIFIED, ALTCHA_ERROR };
char *altcha_modal_widget_html(int state, const char *challenge_json, const char *payload);
