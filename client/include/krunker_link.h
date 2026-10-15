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
//   init    ... [6] config [7] weps/cls/prks [12] networking   NET_PACKET_GAME_CONFIG + NETWORK_DATA first
//   l       [state array]                    NET_PACKET_RECON + NET_PACKET_PING_RESULT; answered "v" (:220335)
//   f       [reason]                         SYNC_FAILED: the "q" compression state restarts and
//                                            NET_EXT_KN_INPUTS_DROPPED empties the pending inputs (:220333)
//   2       [sid]                            NET_PACKET_LEAVE (:220567)
//   3       [victim, deaths, killer, stats, info, hideFeed]  NET_PACKET_KILL (:220579)
//   4       [sid, dmg, crit, kill, isAI]     NET_PACKET_HIT (:221254)
//   h       [health, sid|null, ...]          NET_PACKET_HEALTH (:221040)
//   t       [formatted, state, rawMs]        NET_PACKET_TIMER: the match clock (:220234, broadcast :191565)
//   start   [formatted, ...]                 seeds the round clock from arg0 (:218939 -> :218948); clears the
//                                            "q" compression state and resync (:218971-218974): the life's first "q" is full
//   ch      [team, name, text, kind, color]  NET_PACKET_CHAT (:221172)
//   pi      []                               answered "po" (:220395)
//   cc      [nonce, unused, ciphertext]      krunker_cheat_answer -> "ccr" (:218169)
//   cpt     [challenge]                      the ALTCHA proof of work (krunker_altcha.h) -> "cptR" [payload]
//                                            (no ahNum step); unsolvable: KRUNKER_LINK_EV_CAPTCHA (:98132)
//   error   [message, code]                  NET_PACKET_ERROR + KRUNKER_LINK_EV_ERROR
// Movement locks and modifiers of our own player (cluster A of the packet audit):
//   lm      [x, y, z, xDire]                 LOCK_MOVE: NET_PACKET_LOCK (MOVE, 1) + TELEPORT_ACK (resetPosition)
//                                            (iîiîìïì :220428 -> game.lockMove :188432)
//   ulm     []                               UNLOCK_MOVE: NET_PACKET_LOCK (MOVE, 0) (game.unlockMove :188442)
//   lock    [locked]                         LOCK_PLAYER: NET_PACKET_LOCK (PLAYER, locked) (:219705)
//   frz noc unl [on]                         NET_PACKET_CHEAT FROZEN / NOCLIP / UNLIMITED_AMMO (:220495 :220488 :220504)
//   spb     [speedBoost, silent]             NET_EXT_PLAYER_SPEED_BOOST (:220498)
//   sic     [on]                             NET_PACKET_SHARING (:220511)
//   jg      [sid, on]                        NET_PACKET_JUGGERNAUT (:220482)
//   busy    [sid, on]                        KrunkerLinkPlayer.busy (setBusy :188630: a nametag icon only)
//   chrg    [sid, krankedTimer ms]           NET_PACKET_MODE_COUNTER CHARGE (:220033)
//   While lm holds, every "q" frame sends moveDir -1 and no jump (:221748); while "lock" holds the
//   live client builds no input at all, so the frames still sent (isn keeps counting) carry none.
// Player-state corrections (cluster B):
//   chgp    [socketId, x, y, z, doRecon, stopMomentum, dir]  NET_PACKET_CHANGE_POSITION; the player is
//                                            found by its socket id (findById), not its sid (:220937)
//   tm      [sid, team]                      NET_PACKET_TEAM (health kept) (:220422)
//   spk     [key, value]                     you[key] = value: the keys this client models (:220514)
//   wep     [weaponId]                       NET_PACKET_OBJECTIVE WEAPON: game.setWeapon = activeObjective (:189457)
//   rg      [sid, changeClass, items]        a class change: NET_PACKET_LOADOUT + NET_PACKET_TEAM (:220027)
//   rfl     []                               NET_PACKET_REFILL (:220520)
//   gsc     [gameState]                      KrunkerLink.game_state (:219143)
//   00      [[id, name, alias]..., keepOpen] NET_EXT_PLAYER_NAME per rename; keepOpen falsy: the client
//                                            closes the socket (KRUNKER_LINK_EV_CLOSE) (:220322)
//   vk      [alias, votes, needed, isNew, voter] | [null]  NET_EXT_VOTE_KICK_STATE (io "vk", :219974)
//   kpd     [code, message, data]            NET_EXT_KPD_RESPONSE (policeRes, :216287; code 0 only)
//   kpdf    []                               NET_EXT_KPD_RESET (iiíìîìi: the officer's verdict open again)
//   start   [.., kpdData {sus, caller}]      NET_EXT_KPD_DATA (socket ids as our uids) for a K.P.D officer
//   a       [error, id, name, data, ...]     ACCOUNT_RESPONSE (iîïïiii :210016): our krunker.io account
//                                            signed in on this socket -> KRUNKER_LINK_EV_ACCOUNT (name,
//                                            level = floor(0.03 * sqrt(data.player_score)))
// Outbound: our NET_PACKET_ENT -> "en", NET_PACKET_INPUT(_BATCH) -> "q" (the delta-compressed
// window of :221748-221830), NET_PACKET_CHAT_SEND / CHAT_V7 -> "ct", the player list's EXT
// NET_EXT_VOTE_KICK -> "v2k" [alias, 1] / [alias] (kickVote :220193, the [2] key :221373) and
// NET_EXT_KPD_CALL -> "kpd" [0, {id: socket id}] (callPolice :216272) / [tab], NET_EXT_KPD_RATE ->
// "rkpd" [0 | 1] (the officer's verdict, iiïíïíì); the rest is dropped.
// Muting is client-side in v10 too (muteUser :217141 only saves mute_chat / mute_voice), and
// reportPlayer opens the profile: neither has a packet.
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
    KRUNKER_LINK_EV_CLOSE,      // the server told us to close the socket ("00" with keepOpen falsy)
    KRUNKER_LINK_EV_ACCOUNT,    // "a" ACCOUNT_RESPONSE: the game server signed our krunker.io account in
                                // (text: its name; account_name / account_level of the link)
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
    char id[64];   // the socket id of its "0" ([0]): "chgp" finds players by it (findById)
    int team;
    int busy;      // "busy" setBusy (a nametag icon; nothing in this client shows it)
} KrunkerLinkPlayer;

typedef struct {
    KrunkerLinkIO io;
    KrunkerSession codec;           // ahNum (krunker_proto.h)
    char socket_id[64];             // io-init
    int my_sid;                     // -1 until our "0"
    int joined;                     // "en" sent
    KrunkerLinkPlayer players[KRUNKER_LINK_MAX_PLAYERS];
    int kpd_case;       // startGame gave a K.P.D case (kpdData): a later one without clears it
    int player_count;
    // "q" window compression (game.deob.js:221765-221830): the last value of every input field
    // (iîîíîîi, kept across windows), the previous window's angle list (iîiìîií) and the resync
    // flag a SYNC_FAILED sets for the next window (iïíîîìï)
    double last_vals[15];
    uint8_t last_set[15];
    double prev_angles[2 + 2 * NET_INPUT_BATCH_MAX];
    int prev_angle_count;
    int resync;
    int unsupported_map;            // init's map index outside this client's catalogue, -1 = none (KRUNKER_LINK_EV_UNSUPPORTED)
    // our player's movement state as the server set it ("lm"/"ulm"/"start" game.moveLock, "lock"
    // window.locked, "noc" noclip): shapes the "q" frames and the "l" RECON flags
    int move_lock, locked, noclip;
    int game_state;                 // "gsc" game.gameState, -1 = not sent
    // init's lobby settings (emit_lobby_config): applyNetworking's rates (snf, tf, sf) and how many
    // config keys the lobby set
    int send_hz, tick_hz, sync_hz, config_keys;
    // the socket carries a krunker.io account's token (`at=`, krunker_account.h): set by the transport
    // after krunker_link_reset; "en" then sends the player's own loadout instead of a guest's
    int account;
    char account_name[64];          // "a" ACCOUNT_RESPONSE: the account's name, "" none yet
    int account_level;              // its level (Account.updateLevel over player_score), 0 = not in it
    // counters (tests, logs, the console's official_sync)
    unsigned frames_in, frames_out, cc_answered, cc_failed, captchas_solved, unknown_in;
    unsigned syncs, sync_failed;    // "l" SYNC_PLAYER applied, "f" SYNC_FAILED received
} KrunkerLink;

void krunker_link_init(KrunkerLink *link, const KrunkerLinkIO *io, int64_t ahkey);
// A new WebSocket: ahNum 0, every per-connection state cleared (the io stays).
void krunker_link_reset(KrunkerLink *link);
// One received binary frame. 0 = malformed (logged and dropped; the session goes on).
int krunker_link_on_frame(KrunkerLink *link, const uint8_t *data, size_t len);
// One of our outbound packets: translated and sent, or dropped. 1 = something was sent.
int krunker_link_send_ours(KrunkerLink *link, NetPacketType type, const void *payload, uint16_t length);
// One of our input frames as the official client's 15 input fields (game.deob.js:221748):
// [isn, dt ms, pitch*1000, yaw*1000, movDirs index, shoot, ads, jump, reload, crouch, scroll,
//  wSwap, 1 - speedLmt, reset, interact]. What the "q" carries and the server's procInputs reads.
void krunker_link_input_fields(const NetInputPacket *in, double v[15]);
// Encode and send one Krunker packet (wire name + args). 1 = sent.
int krunker_link_send(KrunkerLink *link, const char *wire, const KrunkerValue *args, size_t argc);
