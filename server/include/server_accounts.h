#pragma once
// Account-aware game behaviour on top of the brain link (doc/backend.md section 8):
// join tickets / session tokens -> account identity, PLAYER_INFO (level, clan) for HUDs,
// bans, MOTD, billboards, per-mode lobby size / round length, lobby state pushed to the
// brain, match stats + KR rewards committed at round end and on disconnect, hosted
// custom games and draining. Every function runs with g_server->lock held.
#include <server.h>
#include <server_config.h>

// Configure Game from the config (or the CLI map, rotation and mode list). Used at startup
// and when a hosted custom lobby reverts to the public rotation.
void accounts_configure_public(Game*, const cJSON *cli_map);
void accounts_init(void);                 // after the first game_init: lobby id, bans, brain thread
void accounts_tick(float now);            // brain events, join timeouts, round end, lobby pushes
void accounts_shutdown(void);             // drain, commit everyone, flush, stop the brain thread

// Connection lifecycle (net.c)
int  accounts_refuse_connect(const char *address, NetErrorPacket *error); // 1 = refuse (bans file, draining)
void accounts_on_connect(ClientConnection*);
void accounts_handle_join(ClientConnection*, const NetPacket*);
int  accounts_gate_ent(ClientConnection*, int spectate);   // 0 = deferred until the identity is settled
void accounts_on_spawn(ClientConnection*, int first_spawn);
void accounts_send_infos_to(ClientConnection*);           // every player's PLAYER_INFO to a joiner
void accounts_on_disconnect(ClientConnection*);
void accounts_on_end_packet(int uid, const NetEndPacket*);
void accounts_send_billboard(ClientConnection*);          // before INIT

// Round changes (net_change_map_internal): before the players are freed / after game_init.
void accounts_round_ending(void);
void accounts_round_changed(void);

// Account bans (bans_file "accounts"): /ban, /unban, /bans. 1 = done.
int accounts_ban_account(int account_id, const char *name);
int accounts_unban_account(int account_id);
int accounts_list_account_bans(int *out, int max);

// GameHooks.reward: KR of this player's round (v3.1.1 endGame rules + gameserver.kr_*).
int accounts_reward(const Player*);

// Ranked lobbies (brain queue): the account's team (1 / 2) from the roster, 0 = mode picks.
int accounts_assigned_team(const ClientConnection*);
// The account hosting the current custom lobby (requirePremiumCheatPerm's custom host).
int accounts_is_custom_host(const ClientConnection*);
// WP6: the published map (config.customMap) the current custom lobby hosts, NULL none:
// GAME.STORAGE and Save / Load Data need one online (v7 customMap + defaultSetts).
const char *accounts_published_map(void);
// WP6: v7 game.krRewards: the host option krRewards on a hosted published map.
int accounts_kr_rewards(void);

// Templates of config.serverMessages[0/1] ("" = off).
const char *accounts_join_message(void);
const char *accounts_leave_message(void);

// Graceful shutdown: refuse joins, tell the coordinator; drained = nobody left or the round ended.
void accounts_begin_drain(const char *why);
int accounts_drained(void);

// Server-info query / finder (server/src/info_query.c)
int accounts_player_count(void);              // players whose identity is settled
const char *accounts_community_name(void);    // gameserver.community, else the coordinator's name, "" standalone
int accounts_info_tags(char tags[][24], int max); // ranked / custom / draining / linked

// Skins (doc/backend.md 8.7): the coordinator's inventory of this account.
// 1 = the player may wear store.skins index `skin` (always 1 when not enforced, guests: none).
int accounts_owns_skin(const ClientConnection*, int skin);
// Clears cosmetics the player does not own (ENT). Returns how many were stripped.
int accounts_filter_cosmetics(const ClientConnection*, PlayerCosmetics*);
// Same for the weapon skins and the melee skin (ENT ent[2] / ent[9]).
int accounts_filter_skins(const ClientConnection*, PlayerSkins*);

// net.c exports used here
void net_kick_client(int id, const char *message, const char *code); // deferred close (pending kicks)
void net_resume_ent(ClientConnection*, int spectate);              // replay a deferred ENT
