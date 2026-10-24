#pragma once
// The in-game captcha of Box Office servers (server/src/game_captcha.c, doc/v10/captcha.md): Krunker v10.0.3's
// "cpt" / "cptR" (deob-10.0.3.js:111424-111437), an ALTCHA proof of work the brain issues and verifies.
//
// With the brain's captcha.enforce (its welcome's {"captcha": {"enforce": true}}), every player connection whose
// identity has settled (SERVER_JOIN_READY) is asked once: the server asks the brain for a for=game challenge
// ({"type":"captcha","op":"issue"}), sends NET_EXT_CAPTCHA [challenge JSON] and waits for NET_EXT_CAPTCHA_ANSWER
// [payload], which the brain checks bound to that challenge ({"op":"verify"}). Accepted: an empty NET_EXT_CAPTCHA_ANSWER
// back (v10.0.3's server "cptR"). Refused, no answer within GAME_CAPTCHA_ANSWER_S, or a peer that cannot be asked (no
// EXT): the connection is closed with NET_PACKET_ERROR "CAPTCHA FAILED", the text v10.0.3's client closes with
// (:111430). The game goes on meanwhile (v10.0.3's modal does not stop it either). Without a brain link nothing is asked:
// the offline sandbox, direct servers, a link that dropped (asked again after it comes back, if still enforced).
// When v10.0.3's servers send "cpt" is their own (unknown) rule; Box Office asks each connection once.
#include <server.h>
#include <cJSON.h>
#include <stdint.h>

#define GAME_CAPTCHA_ANSWER_S 45.0f  // the client gives up after 30 s (KN_CAPTCHA_DEADLINE_MS); the brain's round trips
#define GAME_CAPTCHA_FAILED "CAPTCHA FAILED"
#define GAME_CAPTCHA_CODE NET_ERROR_CODE_CAPTCHA // NET_PACKET_ERROR code of the close (shared.h)

void game_captcha_set_enforce(const cJSON *welcome); // BRAIN_EV_LINK_UP: the welcome's captcha.enforce
void game_captcha_link_down(void);                   // pending asks are dropped (asked again once enforced again)
void game_captcha_on_brain_frame(const cJSON *frame); // "captcha-r"
void game_captcha_on_answer(ClientConnection *client, const uint8_t *body, uint16_t length); // NET_EXT_CAPTCHA_ANSWER
void game_captcha_tick(float now);                   // asks the new connections, closes the late ones
int game_captcha_enforced(void);
