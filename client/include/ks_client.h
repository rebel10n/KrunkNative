#pragma once
// KrunkScript client host (doc/v7/krunkscript.md WP4; client/src/ks/ks_client_host.c and the
// GAME.* namespaces next to it). The map's scripts.client runs in the client's own Game
// (shared/include/ks_game.h): reloaded on every INIT (map load and each round restart, v7
// Game.mainScript = null + prepareGame, 2.2), start() at the end of the round setup, update()
// from the game frame, render() inside the overlay canvas pass (ks_overlay.h), and the event
// hooks (keys, mouse, controller, DIV clicks, spawn/death, network) from the client's input and
// packet handlers. A map without a client script runs nothing and costs nothing.
#include <client.h>

// ---- lifecycle (main.c)
void ks_client_init(Client*);           // once, after the window, scenes and the HTML page exist
// client_apply_init: the round's map is loaded and the players dropped. Unloads the previous
// instance (script DIVs, scene objects, sounds, camera/input state), empties #customGUIHolder
// (overlay.resetAll), loads the map's client script (global initialisers) and calls start().
void ks_client_round(Client*, int server_minor); // server_minor: INIT's protocol minor (0 = other major)
void ks_client_unload(Client*);          // leaving the game / map (also detected per frame)
// After game_tick_world: update(delta * deltaMlt) (delta = the endAnim-scaled world delta,
// raw = the frame delta), then script scene objects (sprites, texture moves, models, lights).
void ks_client_frame(Client*, float world_delta_seconds, float frame_delta_seconds);
// After the HUD wrote the page this frame, before it is composited: GAME.UI.hideDefault /
// hideCrosshair keep the default HUD hidden.
void ks_client_ui_late(Client*);
// After the engine posed client->camera for this frame: GAME.CAMERA (detached rig, fov).
void ks_client_camera(Client*);

// ---- input (main.c GLFW callbacks; KRUNKNATIVE_INPUT_SCRIPT drives the same callbacks)
// keydown / keyup / auto-repeat: onKeyPress / onKeyUp / onKeyHeld while the game owns the
// controls (Escape always), INPUTS.keyDown state.
void ks_client_key(Client*, int key, int scancode, int action, int mods);
// onMouseClick / onMouseUp on the game view, re-lock after INPUTS.unlockMouse, script DIVs
// under a free cursor (the HTML page takes those clicks). 1 = consumed (main.c stops).
int  ks_client_mouse_button(Client*, int button, int action);
void ks_client_cursor(Client*, double x, double y); // window coordinates (INPUTS.mousePos / mouseMovement)
void ks_client_scroll(Client*, double dy);          // onMouseScroll
// gamepad.c: onControllerPress / Held / Up (type 0 press, 1 hold, 2 release) for a mapped
// button name (krunkscript.md 4.4); code = the value of the bind the button presses, or 0.
void ks_client_controller(int type, const char *button, int code);
// INPUTS.unlockMouse / freeMouse: the cursor is free in game (no pointer lock, the page takes
// clicks on script DIVs, the lock-loss check must not open the menu).
int  ks_client_pointer_free(void);
// INPUTS.unlockMouse (until lockMouse or a click on the game view): the controls are off.
int  ks_client_input_unlocked(void);

// ---- events
void ks_client_player_spawned(Client*, const Player*);              // onPlayerSpawn (any player)
void ks_client_player_killed(Client*, int victim_uid, int killer_uid); // onPlayerDeath; killer -1 = none
// NET_PACKET_SCRIPT_MSG / SCRIPT_LOG / SCRIPT_SYNC (200-202). 1 = handled.
int  ks_client_handle_packet(Client*, int type, const void *payload, uint16_t length);
void ks_client_div_clicked(const char *id); // onDIVClicked for map GUI elements (#cusDIV<i>)

// ---- state other client code reads
int  ks_client_3d_disabled(void);        // GAME.DEFAULT.disable3D: no world / first-person pass
int  ks_client_hide_crosshair(void);     // GAME.UI.hideCrosshair
int  ks_client_players_hidden(void);     // GAME.PLAYERS.disableMeshes
// GAME.INPUTS.disableDefault (game.dontSendInputs): no input frame is predicted or sent.
int  ks_client_inputs_disabled(void);
// GAME.DEFAULT.disablePrediction (game.noPred): inputs are sent but neither predicted nor
// replayed by the reconciliation.
int  ks_client_no_prediction(void);
