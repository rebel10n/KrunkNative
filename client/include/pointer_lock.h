// The one owner of "should the pointer be locked" (client/src/pointer_lock.c). No GLFW here:
// client/tests/pointer_lock_test.c links the .c alone; menu_ui.c client_pointer_update builds
// the inputs from the UI state, runs the step and applies it (GLFW cursor mode natively, the
// canvas pointer lock on the web), on every state change and once a frame.
//
// v7.1.4 controls.js: CONTROLS.enabled follows the pointer lock (pointerlockchange f2136), the
// mouse only turns the camera while it is enabled (f2137 sums movementX/Y only then;
// updateMouseInp applies the sums), and the menu, the console, the end-screen results
// (CONTROLS.disable() once config.endAnim ran out) and focus loss free it. A round that ends
// while the menu is open closes the menu (hideUI -> toggleMenu(false)) but the lock stays lost:
// the end title then shows a free cursor that turns nothing, until a click on the game view
// (mousedown f2139: !enabled -> toggle(true) -> requestPointerLock) or the results.
//
// Platforms: natively the lock is the GLFW cursor mode and follows `lock` at once, so closing the
// menu (Esc, Click to Play) locks again in the same frame (KrunkNative: the native menu's Escape
// closes it, v7's browser page cannot). On the web the browser grants the canvas lock only on a
// user gesture: the menu waits for it (CLICK TO PLAY) and a lock that is not granted brings the
// menu back (menu_ui.c client_menu_tick). Either way the camera only takes the mouse while the
// lock is really held (pointer_lock_look_allowed).
#ifndef KN_POINTER_LOCK_H
#define KN_POINTER_LOCK_H

typedef struct {
    unsigned char game;          // the game app mode (not the hub / editor / viewer page) with the HTML UI
    unsigned char menu_open;     // #menuHolder (and any window over it)
    unsigned char console_open;  // devConsole.visible (it also opens the menu)
    unsigned char focused;       // the native window has the keyboard focus (always 1 on the web)
    unsigned char end_up;        // the end screen: title animation or results (hud_ui_end_screen_up)
    unsigned char end_results;   // its clickable results (hud_ui_end_screen_interactive)
    unsigned char script_unlock; // KrunkScript INPUTS.unlockMouse (tmpUnlock: controls off too)
    unsigned char script_free;   // KrunkScript INPUTS.freeMouse (forceFreeCam: controls on, no lock)
    unsigned char modal;         // a page modal waits for the player (krunker.io's security challenge): v10
                                 // exits the pointer lock for its modals (CONTROLS.toggle(false)); the lock
                                 // comes back once it closes
} PointerLockInputs;

typedef struct {
    int controls; // CONTROLS.enabled: keys / buttons reach the game (mouse_state.locked)
    int lock;     // the pointer should be locked (GLFW_CURSOR_DISABLED / requestPointerLock)
} PointerLockWant;

typedef struct {
    int was_controls; // `controls` of the previous step
    int end_up;       // end_up of the previous step
    int end_freed;    // the end title runs with the pointer free: until a click or the end screen goes
} PointerLockTracker;

// One step of the state machine (each frame and on every menu / end-screen / focus change).
PointerLockWant pointer_lock_step(PointerLockTracker *t, const PointerLockInputs *in);

// The lock went during the end title without the menu (alt-tab; the browser's Escape on the
// web): v7 shows no menu there (hideUI set UI.active = false), the pointer stays free until a
// click. No-op when no end screen is up.
void pointer_lock_end_free(PointerLockTracker *t);

// A click on the game view while the controls are off: CONTROLS.toggle(true) (the lock again).
void pointer_lock_resume(PointerLockTracker *t);

// The camera may take mouse movement: the controls are on and the pointer is really locked
// (`held`: natively the cursor mode is DISABLED, on the web document.pointerLockElement).
int pointer_lock_look_allowed(const PointerLockWant *want, int held);

#endif
