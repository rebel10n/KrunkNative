#pragma once
// Krunker v10.0.2's mobile controls (CONTROLS.isMobile, the #mobileUI touch table, the
// nipplejs joystick, the "mobile" settings; game.deob.js), driven over the DOM-ops API like the
// rest of the C UI logic: client/src/ui/ui_mobile.c.
//
//   - detection: CONTROLS.checkMobile() (user agent, or no requestPointerLock);
//   - #mobileUI: shown while the game owns the input (toggleControlUI/toggleGameUI),
//     buttons placed from mobileControlPositions;
//   - the virtual joystick nipplejs drew in #mobileJoystick (static mode, size 120,
//     black), re-implemented here: same markup/styles, same direction -> move-key rules;
//   - touch look (fakeMouse(2, dx, dy, 0, "mblInput")) with the mobile sensitivities,
//     the gradual joystick speed (gradSpeedMbl -> CONTROLS.speedLmt, ui_controls.c);
//   - pointer-lock-free play ("TAP TO PLAY", locklessChange).
// Natively ui_mobile_active() is 0 unless KRUNKNATIVE_MOBILE=1: GLFW delivers touchscreen input
// as mouse events, and ui_mobile_mouse() feeds the left button to the same touch targets. The
// Krunker Strike flavor is the mobile version on every platform, so it is 1 there too (strike_ui.h).
#include <client.h>

typedef enum { UI_TOUCH_START, UI_TOUCH_MOVE, UI_TOUCH_END, UI_TOUCH_CANCEL } UiTouchPhase;

typedef struct {
    long identifier;        // Touch.identifier
    float client_x, client_y; // CSS px, viewport origin
    float screen_x, screen_y; // CSS px (Krunker's look deltas use screenX/Y)
} UiTouch;

int   ui_mobile_active(void);                 // CONTROLS.isMobile
void  ui_mobile_tick(Client *client);         // per frame: DOM setup, #mobileUI visibility
// Touch events on #mobileUI (changedTouches). Returns 1 = preventDefault.
int   ui_mobile_touch(UiTouchPhase phase, const UiTouch *changed, int count);
// mblInput look delta since the last call, already scaled by the mobile sensitivity
// (sensitivityXMbl/YMbl * 4, aim sensitivities while the right button is held) and
// invertYMbl; 0 when nothing moved. ui_controls_fill_input multiplies by mouseSens.
int   ui_mobile_take_look(int aiming, float *dx, float *dy);
// The mblInput look factors take_look applies (sensitivity*Mbl x 4, aim ones while aiming; invertYMbl).
void  ui_mobile_look_sens(int aiming, float *sx, float *sy, int *invert);
// A touch is held on the controls (the joystick or a look drag): Strike's joystick.active ||
// moveJoystick.active for its aim assist (client/src/strike_assist_client.c).
int   ui_mobile_touch_active(void);
// Native mouse as the one touch (only while #mobileUI is up): the left button's press / drag /
// release, 1 = consumed (the page and the game do not see it). KRUNKNATIVE_MOBILE=1 forces it.
int   ui_mobile_mouse(UiTouchPhase phase, float x, float y);
// The scenario renderer's state.mobile and tests: force the phone layout on or off.
void  ui_mobile_force(int on);
// Krunker Strike's controls (strike_touch.c) look through the same mblInput path: finger travel
// in CSS px, scaled by the mobile sensitivities when ui_mobile_take_look() hands it on.
void  ui_mobile_add_look(Client *client, float dx, float dy);
// The play gesture on a phone (TAP TO PLAY): fullscreen + landscape lock (the Krunker
// apps' behaviour; the v3.1.1 page itself stayed windowed). No-op unless mobile.
void  ui_mobile_play_gesture(void);
// Settings window tab markup: v3.1.1 generates the "mobile" category rows with
// display:block and the "Mobile" header visible when isMobile (hide: !isMobile).
// Returns a malloc'd copy with those rows shown, or NULL when nothing changes.
char *ui_mobile_settings_html(const char *tab_html);
