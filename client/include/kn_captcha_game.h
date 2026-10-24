#pragma once
// The in-game captcha of Box Office servers, on the client (doc/v10/captcha.md): v10.0.3's "cpt" / "cptR"
// (deob-10.0.3.js:111424-111437) over KrunkNative's protocol (NET_EXT_CAPTCHA / NET_EXT_CAPTCHA_ANSWER, net_ext.h;
// the server's side: server/include/server_captcha.h). Official krunker.io games answer their own "cpt" in
// krunker_link.c / krunker_net.c.
//
// NET_EXT_CAPTCHA [challenge JSON]: solved by kn_captcha_solve (slices of the frame loop, the same on native and web);
// the payload goes back as NET_EXT_CAPTCHA_ANSWER on the same session. Nothing is shown (v10.0.3 shows a "Security
// Challenge" modal around the ALTCHA widget; Box Office solves it the widget's way without one). The server's empty
// NET_EXT_CAPTCHA_ANSWER: accepted (v10.0.3's _hideCaptcha). A challenge this client cannot solve: v10.0.3's failure
// branch, the error "CAPTCHA FAILED" and the socket closed (:111428-111431), shown on the error screen.
#include <client.h>
#include <stdint.h>

#define KN_CAPTCHA_GAME_FAILED "CAPTCHA FAILED"

void kn_captcha_game_packet(Client *client, uint16_t subtype, const uint8_t *body, uint16_t length);
