#pragma once
// Prop Hunt on the client (client/src/prop_hunt_client.c; the mode: shared/include/prop_hunt.h):
// the server's NET_EXT_PROP_UPDATE / NET_EXT_PROP_ROTATION (official lobbies: Krunker's "up" /
// "prpR" through krunker_link.c), the prop wheel (overlay.propWheel*, game.deob.js :159210-159240,
// drawn :160817-160863), the keys (prop: the wheel while held, propRand, propRot, propRotR;
// :221435-221446 / :221490), the Prop team's camera distance (gameLoop :221701) and the HUD
// (#propControlHolder shown and #botRightHider hidden for a prop, :220298).
#include <client.h>

// The local player wears a prop this frame (Prop Hunt, its team is the Prop team, alive).
int prop_hunt_me(const Client*);
// NET_EXT_PROP_UPDATE / NET_EXT_PROP_ROTATION. 1 = handled.
int prop_hunt_on_ext(Client*, uint16_t subtype, const uint8_t *body, uint16_t length);

// The keys (the local player on the Prop team only, as v10 checks).
void prop_hunt_wheel_press(Client*);   // prop: open the wheel
void prop_hunt_wheel_release(Client*); // prop up: pick the wheel's prop when it is not the one worn
void prop_hunt_random(Client*);        // propRand: a random prop when it is not the one worn
void prop_hunt_rotate(Client*, int reset); // propRot / propRotR
// The mouse wheel: an open prop wheel takes it (propWheelChange), 1 = taken; otherwise a prop's camera
// distance moves 5 per notch within [10, 100] (not taken: the weapon scroll goes on as in v10).
int prop_hunt_scroll(Client*, double dy);

// The Prop team's camera: behind the eye at this distance (third_person.c poses it).
float prop_hunt_camera_distance(void);
// Our spawn / a new round: the distance back to 30 (startGame), the wheel closed.
void prop_hunt_reset(void);

// Per frame: the wheel's hold timer, the HUD holders.
void prop_hunt_frame(Client*, float delta_seconds);
// The wheel on the game overlay (after sprays_wheel_render).
void prop_hunt_wheel_render(Client*, float delta_seconds);
int prop_hunt_wheel_opened(void);
int prop_hunt_wheel_index(void);
void prop_hunt_fini(void);

// Packs and sends a pick / rotation request to the server (our EXT; krunker_link.c turns them into
// "prp" / "prpR" for an official lobby). 1 = sent.
int prop_hunt_send_pick(Client*, int prop_id);
int prop_hunt_send_rotate(Client*, int reset);
