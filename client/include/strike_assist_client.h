#pragma once
// Krunker Strike's mobile aim assist and auto-fire in the client (client/src/strike_assist_client.c;
// the GL-free core is strike_assist.h). No-ops unless a phone (ui_mobile_active) plays a flavor with
// Strike's rules.
#include <client.h>

// The gameLoop pass, once per input frame: the shoot key auto-fire leaves (0 / 1), or -1.
int strike_assist_client_tick(Client *client);
// updateMouseInp's share: the pull to add to the touch look sums (px, before the mobile sensitivity)
// and fw() for every look input. Returns 0 (zeros, mlt 1) when inactive.
int strike_assist_client_look(float *look_x, float *look_y, float *mlt);
