#pragma once
// The in-game captcha of Box Office servers, on the client (doc/v10/captcha.md): v10.0.7's "cpt" / "cptR"
// (deob-10.0.7.js:98801-98810) over KrunkNative's protocol (NET_EXT_CAPTCHA / NET_EXT_CAPTCHA_ANSWER, net_ext.h;
// the server's side: server/include/server_captcha.h). Official krunker.io games answer their own "cpt" in
// krunker_link.c / krunker_net.c.
//
// NET_EXT_CAPTCHA [challenge JSON]: v10's Security Challenge card (altcha_modal.h), answered by the player; the value
// goes back as NET_EXT_CAPTCHA_ANSWER on the same session, and the card stays until the server's empty
// NET_EXT_CAPTCHA_ANSWER (accepted: _hideCaptcha), its error or the session's end. A challenge the widget cannot
// verify shows the widget's error (v10.0.7 never closes the socket itself; the server ends the session).
// Without a page to show the card it is solved here (kn_captcha_solve) and one that cannot be is v10.0.3's failure
// branch: the error "CAPTCHA FAILED" and the socket closed (deob-10.0.3.js:111428-111431).
#include <client.h>
#include <stdint.h>

#define KN_CAPTCHA_GAME_FAILED "CAPTCHA FAILED"

void kn_captcha_game_packet(Client *client, uint16_t subtype, const uint8_t *body, uint16_t length);
