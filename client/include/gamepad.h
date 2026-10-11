#pragma once
// Game controller support: the port of Krunker v3.1.1's gamepad module
// (src/libs/gamepad.js = Krunker.Unpacked-v3.1.1/133.js) and the controller half of
// CONTROLS (src/libs/controls.js = 132.js: the button -> bind mapping, the stick
// handlers, fakeMouse, toggle) plus the shot/melee rumble of 95.js.
//
// Platform layer: natively the GLFW joystick/gamepad API (glfwGetGamepadState with the
// SDL_GameControllerDB mappings GLFW ships, reshaped into the browser's "standard"
// Gamepad layout; unmapped joysticks pass their raw buttons/axes through like the
// browser's non-standard mapping); on the web the browser Gamepad API through
// emscripten/html5.h (navigator.getGamepads(), exactly what v3.1.1 reads).
// Implementation: client/src/gamepad.c. Key/mouse effects go through ui_controls.c.
#include <client.h>

void gamepad_init(Client*);              // hotplug listeners (+ pads already plugged in natively)
// Once per main-loop iteration, after the game frame. `rendered`: client_tick ran this
// iteration (the frame-cap skipped it otherwise). Runs the gamepad loop at the cadence
// of the browser's requestAnimationFrame (web: every call; native: display refresh).
void gamepad_frame(Client*, int rendered);
void gamepad_frame_begin(void);          // before a rendered frame's client_tick: updateMouseInp's active reset
int  gamepad_active(void);               // Gamepad.active (PRESS START, rumble gate)
void gamepad_set_active(int active);     // CONTROLS clears it on mouse input
int  gamepad_connected(void);            // Gamepad._connected
