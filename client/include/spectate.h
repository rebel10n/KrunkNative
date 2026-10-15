#pragma once
// Krunker v3.1.1 spectating (window.Bf5) on the client: client/src/spectate.c (state,
// cameras, controls) and client/src/ui/hud_spectate.c (#spectateUI over html_dom.h).
// Reference: index.js toggleSpect / spectMode / pressButton (Bf5 block) / the game
// loop's Bf5 camera branch, 132.js followCam / bPZ1 / ZZms7 and the wheel handler.
//
// Flow: the menu's Spectate switch (toggleSpect) sets the request; enterGame sends an
// ENT with spectate = 1 and the server answers NET_PACKET_SPECTATE (Krunker "start"
// with spectating = true), which starts spectating. Clearing the switch and entering
// again sends a normal ENT; our own SPAWN ends spectating. The embedded local server
// has no spectators: like v3.1.1's single player it spectates locally without a player.
#include <client.h>

// ---- spectate.c
void spectate_set_requested(int on);     // window.toggleSpect(checked): `co`
int  spectate_requested(void);
int  spectate_active(void);              // window.Bf5
void spectate_begin(Client*);            // fc(.., spectating): drop our player, free cam where the camera is
void spectate_end(Client*);              // left spectating (our SPAWN, INIT, disconnect)
int  spectate_target_uid(void);          // b4.spect.target sid, -1 free cam
// player-viewer WP-E part 2a (v7.1.4)
int  spectate_show_names(void);          // showSpectatorNameTags (specNames toggles it, default on)
int  spectate_focused_uid(void);         // focusedPlayerId, -1 none
// The Player List's focusPlayer(id, elm, auto) / clearFocus(): `quiet_auto` 1 = v10's auto focus
// of the K.P.D suspect ("Auto Focusing Suspect", never a toggle off)
void spectate_focus_player(Client*, int uid, int quiet_auto);
void spectate_clear_focus_player(void);
int  spectate_objective_cam(void);       // following an objective spectate camera
int  spectate_has_objective_cams(const Client*); // mode.objective && Map.spectateCams has some
void spectate_on_player_spawn(Client*, int uid); // io_spawn: a focused player is followed again
void spectate_on_player_leave(int uid);  // io_removePlayer: the focused player left (clearFocus)
int  spectate_first_person(void);        // cu && cv: first-person follow view
const char *spectate_person_label(void); // #specIns text ("First Person" / "Third Person")
void spectate_mode(Client*, int dir);    // window.spectMode(dir): 0 free cam, +1 next, -1 previous player
// Per input frame (menu closed): mouse look, free-cam flight or the follow cameras.
void spectate_tick(Client*, float delta, float mouse_dx, float mouse_dy);
// Around the world scene pass: the followed player's model is hidden in first person (Vp8.visible = false).
void spectate_render_begin(Client*);
void spectate_render_end(Client*);
// Input hooks from ui_controls.c (in game, menu closed). Return 1 when spectating used it.
int  spectate_press_key(Client*, int dom_key_code);  // pressButton: F, H, 0-9, R
int  spectate_mouse_press(Client*, int which);       // canvas mousedown: 1 left = previous, 3 right = next
int  spectate_scroll(Client*, int wheel_up);         // wheel: free-cam speed / follow distance

// ---- main.c
Player *client_find_player(Client*, int uid);

// ---- ui_controls.c: raw key state (CONTROLS.keys[code]) for the hard-coded spectator keys (Q/E)
int  ui_controls_code_down(int dom_key_code);

// ---- hud_spectate.c
void hud_spectate_tick(Client*);         // per frame after client_menu_tick: #spectateUI visibility + contents
void hud_spectate_reset(void);           // fc(): empty player rows (specTeam0/1), fresh state
int  hud_spectate_slot_uid(int index);   // fE[index].player sid for the 1-9 keys, -1 none
void hud_spectate_refresh(void);         // cw(): target name/health box changed
