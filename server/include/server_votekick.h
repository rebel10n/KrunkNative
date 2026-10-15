#pragma once
// The player list's actions on the server: the vote kick of Krunker
// v10.0.2 (config voteKickReq 4, voteKickTimer 35 s, voteKickBan 30 min, voteKickMaxLvl 20) and
// the K.P.D call's lobby-side rules (kpdRepLvl: level 5 to call, kpdCallTime: one call per
// suspect per 10 s, kpdAutoTime: a suspect reported again within 60 s is "already reported").
// State lives in game.vote_kick (shared.h VoteKick) and this module's ban / cooldown lists;
// server/src/net.c dispatches the NET_PACKET_EXT subtypes here.
#include <server.h>
#include <net_ext.h>
#include <stdint.h>

// v10.0.2 config constants (game.deob.js:20095-20098, 19747-19749)
#define VOTE_KICK_REQ 4
#define VOTE_KICK_TIMER_S 35.0f
#define VOTE_KICK_BAN_S 1800.0f
#define VOTE_KICK_MAX_LVL 20
#define VOTE_KICK_COOLDOWN_S 35.0f // KrunkNative: no new vote until a timer's length after the last one ended
#define KPD_REP_LVL 5              // the K.P.D button shows from level 5 (v10:214300 account.level >= 5)
#define KPD_CALL_TIME_S 10.0f      // config.kpdCallTime
#define KPD_AUTO_TIME_S 60.0f      // config.kpdAutoTime
#define KPD_WATCH_TIME_S 10.0f     // config.kpdWatchTime: an officer watches this long before the verdict counts
#define KPD_CASE_TIME_S 600.0f     // KrunkNative: an officer joining later than this after the call gets no case

void votekick_init(Game *game);   // hooks.vote_kick_expired, clears the lists (a new server / sandbox)
void votekick_handle(ClientConnection *client, const uint8_t *body, uint16_t length); // NET_EXT_VOTE_KICK
void votekick_on_leave(const ClientConnection *client); // the target left: the vote is void; a voter left: recounted
// 1 when this identity (an account, else its address) is still vote-kick banned: message says
// for how long. Checked when a join settles (accounts.c acc_set_ready).
int votekick_banned(int account_id, const char *address, char *message, size_t size);
void votekick_send_state(const ClientConnection *to); // the running vote to a (re)joining 1.4 peer

// NET_EXT_KPD_CALL: the rules (level, the target, rate limits), then accounts_kpd_request
void votekick_handle_kpd(ClientConnection *client, const uint8_t *body, uint16_t length);
// K.P.D officers (moderator / DEV accounts, v10 isMod / isDeputy): one who joins a lobby with an open
// call gets the case (NET_EXT_KPD_DATA, v10 startGame kpdData); its verdict (NET_EXT_KPD_RATE, "rkpd")
// goes to kn_brain (kpd_rate: the call's status / rating). votekick_kpd_recorded: the call's id.
void votekick_officer_joined(const ClientConnection *client);
void votekick_handle_kpd_rate(ClientConnection *client, const uint8_t *body, uint16_t length);
void votekick_kpd_recorded(int suspect_id, int64_t call_id);

void votekick_kpd_reply(const ClientConnection *to, uint8_t code, uint8_t status, const char *message, const char *name,
                        const char *json); // NET_EXT_KPD_RESPONSE (json: the list tail for codes 1..3, NULL none)
