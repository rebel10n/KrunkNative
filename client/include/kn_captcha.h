#pragma once
// Box Office's captcha (doc/v10/captcha.md): the brain's ALTCHA proof of work (services/brain/captcha.h) in front of
// a password sign-in, a registration and a hosted game, solved by this client itself on every platform (native and
// web run the same C: krunker_altcha.c's solver, in slices of the frame loop). Nothing is shown for it: like
// v10.0.3's invisible ("interaction-only") Turnstile before loginUsername (deob-10.0.3.js:130320-130369, 130762), the
// sign-in's "Please wait..." and the host's "Creating Game..." stay up until the answer.
//
//   1. GET <brain>/captcha/challenge?for=<purpose> -> {algorithm, challenge, maxnumber, salt, signature}
//   2. the number is searched, KN_CAPTCHA_SLICE_MS of each frame (kn_captcha_tick)
//   3. done(user, payload): the payload (base64 JSON: the <altcha-widget>'s value, altcha.1.0.6.min.js B()) goes in
//      the request's captcha field: the social "a" frame's 4th element (sendAccountRequest's captcha slot), the
//      hub's login frame's, /mm/host-game's ?captcha=.
//
// No challenge to send (a brain from before the route answers 404, an error, no answer, an unsolvable or refused
// challenge, KN_CAPTCHA_DEADLINE_MS): done(user, NULL) and the request goes without one, as v10.0.3 sends
// loginUsername without "b" when Turnstile gave no token (deob-10.0.3.js:130763: `token ?? undefined`). The brain
// decides what that gets (captcha.enforce: off until the clients that solve are out, doc/backend.md 7.10).
#include <cJSON.h>

#define KN_CAPTCHA_SLICE_MS 6.0         // solving time per frame, shared by every captcha in flight
#define KN_CAPTCHA_DEADLINE_MS 30000.0  // a challenge not solved by then is given up (sent without)
#define KN_CAPTCHA_SLOTS 4

typedef void (*KnCaptchaDone)(void *user, const char *payload);

// Starts one for purpose ("login", "register", "hosting"). Its id (> 0); 0 when none could be started (no brain URL,
// every slot busy): done is then never called. Otherwise done is called exactly once, on the main thread (from
// backend_poll or kn_captcha_tick, never from inside this call), unless kn_captcha_cancel(id) comes first.
unsigned kn_captcha_request(const char *purpose, KnCaptchaDone done, void *user);
void kn_captcha_cancel(unsigned id); // done is not called; an unknown or finished id is ignored
void kn_captcha_tick(void);          // once per frame (backend_frame): solves, and gives up past the deadline
int kn_captcha_pending(void);        // captchas in flight

// A social-socket frame whose element `slot` is the captcha: takes ownership of frame, sets frame[slot] to the
// payload (JSON null without one) once the captcha is done and calls send(frame), then wipes the frame's strings
// (the password it carries) and frees it. When no captcha can be started it is sent at once, with null. Its id
// (kn_captcha_cancel drops the frame unsent), or 0 when it was sent at once.
typedef void (*KnCaptchaSendFrame)(const cJSON *frame);
unsigned kn_captcha_send_frame(const char *purpose, cJSON *frame, int slot, KnCaptchaSendFrame send);
