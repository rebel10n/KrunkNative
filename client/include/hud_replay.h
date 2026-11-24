#pragma once
// The K.P.D replay viewer (client/src/ui/hud_replay.c, doc/v10/replays.md 2.5): Krunker v10.0.7's
// watchDemo / openDemoLink / the #replayControls bar and its resolve area (deob-10.0.7.js:229590-229845,
// :230167) over the replay player (demo_play.h). The page's #replayControls markup keeps Krunker's ids;
// the bar's CSS (ui/css/main.css) and text follow v10.0.7.
#include <client.h>

// watchDemo(tok, ct): the demo's download from the brain (GET /kpd/demos/<tok>/download, the account's
// session), then playback; "Demo Download Failed" / "Invalid Demo File" popups otherwise.
void hud_replay_watch(Client*, const char *tok, const char *ct); // Client NULL: the bound one
// openDemoLink(tok, ct) (a ?demo=<tok>&sus=<ct> link, client_bin --demo-link): the case popup
// (GET /kpd/demos/<tok>?ct=), whose Watch button is watchDemo.
void hud_replay_open_link(Client*, const char *tok, const char *ct);
// Per frame after hud_spectate_tick: the bar's visibility and readouts, onRoundStart (the menu goes
// down, the camera follows the suspect) and the 1.5 s re-follow (:229810-229812).
void hud_replay_tick(Client*);
// The page's listeners on #replayControls (installed once the page is up; idempotent).
void hud_replay_bind(Client*);
int  hud_replay_hitboxes(void); // the hitbox toggle (replayHitBtn)
// --render-scenario (ui_scenario.c): state.replayBar {time, duration, speed, playing, suspect, segments: [[a, b]],
// tok, ct, hitboxes, case: {opn, yes, f_no, myv, rsl, ban}, resolve: "status" | "question" | "error"} shows the bar
// with those values (no demo plays); state.replayCase (demoInfo's data) the openDemoLink popup.
struct cJSON;
void hud_replay_scenario(Client*, const struct cJSON *bar, const struct cJSON *case_popup);

// ---- client/src/replay_hitbox.c (GL): the hitbox outlines after the world pass
void replay_hitbox_render(Client*);
void replay_hitbox_fini(void);
