/*
 * services/strike/director_ticket.h - the Krunker Strike director's join ticket
 * (doc/v10/strike-director.md): what POST /v1/director/allocate hands a client and the Strike
 * game server checks on JOIN (NetJoinPacket.client_key).
 *
 * The same construction as the matchmaker validation token (services/brain/mm_token.c): a compact
 * JWS (knsvc kn_signed_token, HS256) under a key derived from a shared secret,
 * HMAC-SHA256(secret, "kn-strike-director-v1") hex, so a director ticket is never a valid brain
 * join ticket or mm token and vice versa. The director reads strike_director.ticket_secret, the
 * game server gameserver.director_secret: the same value.
 *
 * Claims: {"typ":"sdt","gid":<gameId>,"port":<game port>,"jti":<16 random bytes hex>,
 *          "prv":<private lobby code, absent for public>,"iat","exp"}.
 * Pure (knsvc crypto + cJSON, no sockets): linked by kn_strike_director and server_bin.
 */
#ifndef KN_STRIKE_DIRECTOR_TICKET_H
#define KN_STRIKE_DIRECTOR_TICKET_H

#include <stddef.h>
#include <stdint.h>

#define DIRECTOR_TICKET_TYP "sdt"
#define DIRECTOR_TICKET_KDF_LABEL "kn-strike-director-v1"
#define DIRECTOR_TICKET_MAX 351 /* NetJoinPacket.client_key (char[352]) minus the NUL */
#define DIRECTOR_CODE_MAX 16

typedef struct DirectorTicket {
    char gid[64];                  /* the director's game id ("<region>:<sid>") */
    int port;                      /* the game server's public port the ticket is for */
    char jti[40];
    char code[DIRECTOR_CODE_MAX];  /* private lobby code ("" = public) */
    int64_t exp;                   /* unix seconds */
} DirectorTicket;

/* key (65 bytes, hex) = HMAC-SHA256(secret, label). 0 ok, -1 blank secret / crypto failure. */
int director_ticket_key(const char *secret, char key[65]);

/* A signed ticket (malloc'd, NULL on failure or when it would not fit DIRECTOR_TICKET_MAX).
 * code NULL/"" = a public lobby. jti_out (>= 33 bytes, optional) gets the ticket id. */
char *director_ticket_issue(const char *key, const char *gid, int port, const char *code, int ttl_s,
                            char *jti_out, size_t jti_cap);

/* Checks signature, expiry (now_s 0 = the wall clock) and typ. 0 = ok (out filled), -1 = refused
 * (err: "expired", "signature", "not a director ticket", ...). */
int director_ticket_verify(const char *key, const char *ticket, int64_t now_s, DirectorTicket *out, char *err,
                           size_t errlen);

#endif
