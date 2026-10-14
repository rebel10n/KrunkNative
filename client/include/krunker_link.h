#pragma once
// The official-Krunker session's translation layer (client/src/net/krunker_link.c): Krunker's
// packets <-> this client's NetPackets. No sockets, no globals: krunker_net.c (the transport)
// feeds it received frames and sends what it encodes; client/tests/krunker_link_test.c drives it
// directly. Packet layouts are read from the live build's game.deob.js (line numbers below).
//
// Inbound (S->C), each turned into the NetPacket(s) our client already handles:
//   io-init [socketId]                       remember the socket id (players.add compares ids with it)
//   load    [...]                            answered with "load" (INIT_LOAD, :222180)
//   init    [mapIndex, modeIndex, ...]       NET_PACKET_INIT (map catalogue index 0..31 = Krunker's
//                                            official mapIndex; modeIndex = the Krunker mode id) (:219289)
//   frs     [...]                            ENTER_GAME acknowledgement: logged (the spawn is "0")
//   0       [flat list, stride 53]           NET_PACKET_SPAWN per player (:220266, players.add :171902)
//   k       [flat list, stride 13, rate, t]  NET_PACKET_STATE per other player (:220529)
//   l       [state array]                    NET_PACKET_RECON + NET_PACKET_PING_RESULT; answered "v" (:220335)
//   f       [reason]                         SYNC_FAILED: the "q" compression state restarts (:220333)
//   2       [sid]                            NET_PACKET_LEAVE (:220567)
//   3       [victim, deaths, killer, stats, info, hideFeed]  NET_PACKET_KILL (:220579)
//   4       [sid, dmg, crit, kill, isAI]     NET_PACKET_HIT (:221254)
//   h       [health, sid|null, ...]          NET_PACKET_HEALTH (:221040)
//   ch      [team, name, text, kind, color]  NET_PACKET_CHAT (:221172)
//   pi      []                               answered "po" (:220395)
//   cc      [nonce, unused, ciphertext]      krunker_cheat_answer -> "ccr" (:218169)
//   cpt     [challenge]                      the ALTCHA proof of work (krunker_altcha.h) -> "cptR" [payload]
//                                            (no ahNum step); unsolvable: KRUNKER_LINK_EV_CAPTCHA (:98132)
//   error   [message, code]                  NET_PACKET_ERROR + KRUNKER_LINK_EV_ERROR
// Outbound: our NET_PACKET_ENT -> "en", NET_PACKET_INPUT(_BATCH) -> "q" (the delta-compressed
// window of :221748-221830), NET_PACKET_CHAT_SEND / CHAT_V7 -> "ct"; the rest is dropped.
#include <krunker_cheat.h>
#include <krunker_proto.h>
#include <shared.h>
#include <stddef.h>
#include <stdint.h>

#define KRUNKER_LINK_MAX_PLAYERS 64

typedef enum {
    KRUNKER_LINK_EV_NOTE = 0,   // informational (logged)
    KRUNKER_LINK_EV_CAPTCHA,    // the server asked for a Turnstile captcha ("cpt")
    KRUNKER_LINK_EV_ERROR,      // the server's "error" (it closes right after)
    KRUNKER_LINK_EV_JOINED,     // our own player was added (the "0" with our socket id)
    KRUNKER_LINK_EV_UNSUPPORTED,// something this client cannot show (an unknown map index, ...)
} KrunkerLinkEvent;

typedef struct {
    // a translated packet for the client's inbound queue
    void (*emit)(void *user, const NetPacket *packet);
    // an encoded frame to write to the WebSocket (in order: ahNum already applied)
    void (*send)(void *user, const uint8_t *frame, size_t len);
    // KRUNKER_LINK_EV_* with a sentence (may be NULL)
    void (*event)(void *user, KrunkerLinkEvent kind, const char *text);
    // optional: answers a "cc" instead of krunker_cheat_answer (same contract: 0 = answered). The
    // native transport releases its lock around the evaluation (QuickJS: up to ~250 ms).
    int (*cheat)(void *user, const char *nonce, const char *ciphertext_b64, KrunkerCcr *out);
    // optional: takes a "cpt" ALTCHA proof of work instead of krunker_altcha_solve: 0 = solved now
    // (*payload_b64 malloc'd, the link sends "cptR"), 1 = the transport answers later itself
    // (krunker_link_send "cptR"), -1 = refused / unsolvable (KRUNKER_LINK_EV_CAPTCHA)
    int (*altcha)(void *user, const char *algorithm, const char *challenge, const char *salt, const char *signature, long max_number,
                  char **payload_b64);
    void *user;
} KrunkerLinkIO;

typedef struct {
    int sid;
    float health;
    int max_health;
} KrunkerLinkPlayer;

typedef struct {
    KrunkerLinkIO io;
    KrunkerSession codec;           // ahNum (krunker_proto.h)
    char socket_id[64];             // io-init
    int my_sid;                     // -1 until our "0"
    int joined;                     // "en" sent
    KrunkerLinkPlayer players[KRUNKER_LINK_MAX_PLAYERS];
    int player_count;
    // "q" window compression (game.deob.js:221765-221830): the last value of every input field
    // (iîîíîîi, kept across windows), the previous window's angle list (iîiìîií) and the resync
    // flag a SYNC_FAILED sets for the next window (iïíîîìï)
    double last_vals[15];
    uint8_t last_set[15];
    double prev_angles[2 + 2 * NET_INPUT_BATCH_MAX];
    int prev_angle_count;
    int resync;
    // counters (tests, logs)
    unsigned frames_in, frames_out, cc_answered, cc_failed, captchas_solved, unknown_in;
} KrunkerLink;

void krunker_link_init(KrunkerLink *link, const KrunkerLinkIO *io, int64_t ahkey);
// A new WebSocket: ahNum 0, every per-connection state cleared (the io stays).
void krunker_link_reset(KrunkerLink *link);
// One received binary frame. 0 = malformed (logged and dropped; the session goes on).
int krunker_link_on_frame(KrunkerLink *link, const uint8_t *data, size_t len);
// One of our outbound packets: translated and sent, or dropped. 1 = something was sent.
int krunker_link_send_ours(KrunkerLink *link, NetPacketType type, const void *payload, uint16_t length);
// Encode and send one Krunker packet (wire name + args). 1 = sent.
int krunker_link_send(KrunkerLink *link, const char *wire, const KrunkerValue *args, size_t argc);
