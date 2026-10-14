#pragma once
// Krunker Strike director tickets on the game server (doc/v10/strike-director.md).
//
// The Strike director (services/strike/director.c) hands a client {host, port, gameId, ticket};
// the client JOINs with the ticket as NetJoinPacket.client_key. With gameserver.director_secret
// set (the director's strike_director.ticket_secret), this server verifies it
// (services/strike/director_ticket.h: HS256 under HMAC-SHA256(secret, "kn-strike-director-v1")):
// a forged, expired or foreign ticket is refused, never downgraded to a guest. With
// gameserver.director_require_ticket a JOIN without one is refused too.
//
// Private lobbies: a ticket may carry a lobby code (allocate/private). The first valid ticket into
// an empty lobby sets the lobby's code ("" = public); while anyone plays, only tickets with that
// code get in, and the server-info query carries the "private" tag so the director (also one that
// restarted) never hands the lobby out publicly. Without knsvc (the client's sandbox server) the
// gate is compiled out: disabled.
#include <server_config.h>
#include <stddef.h>

typedef enum DirectorGateResult {
    DIRECTOR_GATE_OK = 0,    // a valid ticket for this lobby: admit (as the director's guest)
    DIRECTOR_GATE_PASS,      // no ticket and none needed: the usual join paths decide
    DIRECTOR_GATE_INVALID,   // not a valid director ticket (bad signature / malformed / expired)
    DIRECTOR_GATE_REFUSED,   // not for this server / this lobby, or no ticket where one is needed
} DirectorGateResult;

int director_gate_init(const GsConfig *cfg); // derives the key; 0 = off or ready, -1 = misconfigured
int director_gate_enabled(void);             // a director secret is configured
int director_gate_required(void);            // gameserver.director_require_ticket (and enabled)
// Checks a JOIN's ticket (NULL / "" = none: refused when required or into a private lobby with
// players, else PASS). lobby_empty: nobody else plays (the ticket, or its absence, sets the lobby's
// code). msg gets the refusal text for the client.
DirectorGateResult director_gate_check(const char *ticket, int lobby_empty, char *msg, size_t msg_size);
int director_gate_private(int players);      // 1 = a private lobby with players (server-info "private" tag)
