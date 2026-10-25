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
// WP-E: the death screen's account values on the player (players.js:4860): class_stat of its
// class (0 for guests and anonMode) and its pcStatIndex value (-1 for guests / none picked).
void accounts_player_card(const ClientConnection*, Player*);
void accounts_send_infos_to(ClientConnection*);           // every player's PLAYER_INFO to a joiner
void accounts_on_disconnect(ClientConnection*);
// KrunkNative live sign-in (NET_EXT_KN_ACCOUNT_AUTH body, shared/include/net_ext.h): sign in, switch
// accounts or sign out on the live connection, verified as a JOIN is; answered with
// NET_EXT_KN_ACCOUNT_RIGHTS. accounts_push_rights: the connection's rights now (STATE), e.g. after
// a console op / deop.
void accounts_handle_live_auth(ClientConnection*, const uint8_t *body, uint16_t length);
void accounts_handle_joined_rank(ClientConnection*); // NET_EXT_JOINED_RANK "joinedRank" (ranked match joined)
// NET_EXT_KN_GUEST_ID (C -> S, shared/include/net_ext.h): a guest's id for its unclaimed rewards (kept on a guest connection only).
void accounts_handle_guest_id(ClientConnection*, const uint8_t *body, uint16_t length);
void accounts_push_rights(ClientConnection*);
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
// GameHooks.progress (WP-E part 2c): game.incStat (stat 1) / challenges.update (stat 0) of an
// account's player, tallied for the next stats commit.
void accounts_progress(const Player*, int stat, const char *key, int value, int weapon, int streak, int item);
// season-system: GameHooks.give_skin (v7 game.giveSkin -> sendUserSkin through kn_brain)
void accounts_give_skin(const Player*, int skin, const char *reason);
// Junk wallet (doc/v10/junk.md): GameHooks.junk_credit -> kn_brain's junk_credit (hundredths); the
// finder's "You found" line follows the brain's answer.
void accounts_junk_credit(const Player*, int64_t hundredths, const char *reason);
void accounts_on_kill(const Player *victim, const Player *killer); // GameHooks.kill: v10 Turf Wars clan kills

// Ranked lobbies (brain queue): the account's team (1 / 2) from the roster, 0 = mode picks.
int accounts_assigned_team(const ClientConnection*);
int accounts_clan_war_allowed(const ClientConnection*); // knClanWar: only the two clans play, others spectate
const char *accounts_clan_war_label(void);              // "Clan War: [A] vs [B]", NULL when none
// 1 while this lobby plays a ranked match (its roster's teams are fixed: no "cd" team picks).
int accounts_ranked_active(void);
// v10 lobbyPlayerId stand-in: the connection's roster slot in the ranked match, 1-based (0: not on the roster)
int accounts_ranked_lobby_id(const ClientConnection *client);
// A one-line server notice in the chat of `client` (v7's ranked.* messages, refusals).
void accounts_notice(ClientConnection*, const char *text);
// The account hosting the current custom lobby (requirePremiumCheatPerm's custom host).
int accounts_is_custom_host(const ClientConnection*);
// v10.0.3 ADMIN_ACTIONS 8 (Transfer Host): the custom host hands the game to `target` (a signed-in or guest player
// here); 1 when it hosts now, 0 when refused (not the host, no such player, the transfer limit reached).
int accounts_transfer_host_to(ClientConnection *host, ClientConnection *target);
// Tests only: `host` hosts a custom game here, as the brain's "host" frame makes it (server/tests/votekick_test.c),
// and one custom-game tick (the host leaving: transfer or end), which accounts_tick runs only with a brain link.
void accounts_host_for_test(ClientConnection *host, int transfer_on_leave);
void accounts_tick_custom_for_test(void);
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
// The same for the connection playing `player` (GameHooks.can_use_skin: players.spray); 0 for bots.
int accounts_can_use_skin(const Player*, int skin);
// Clears cosmetics the player does not own (ENT). Returns how many were stripped.
int accounts_filter_cosmetics(const ClientConnection*, PlayerCosmetics*, PlayerCosmeticsV7*); // unowned slots -> -1 (+ the premium hat/head, body/back rule)
// Same for the weapon skins and the melee skin (ENT ent[2] / ent[9]).
int accounts_filter_skins(const ClientConnection*, PlayerSkins*);
// The account's owned list (data slot 10, cJSON) -> the skin bitset and the painted copies (their paint).
void accounts_load_skins(ClientConnection*, const cJSON *account);
// The paint of each worn skin, from the owner's painted copies (NULL owner: none). Sent with SKINS V2.
void accounts_skin_paints(const ClientConnection *owner, const PlayerSkins *skins, PlayerSkinPaints *out);

// net.c exports used here
void net_kick_client(int id, const char *message, const char *code); // deferred close (pending kicks)
// The player list's K.P.D call (server_votekick.h): records it in kn_brain (op kpd_call) and answers
// the caller; tab 1..3 asks for an officer's list (kpd_calls / kpd_log). target NULL for the lists.
void accounts_kpd_request(ClientConnection *client, int tab, const ClientConnection *target);
// An officer's verdict on call `call_id` (op kpd_rate; 0 / no coordinator: logged only). A refusal
// ("Case already closed") is told to the officer in chat.
void accounts_kpd_rate(ClientConnection *officer, int64_t call_id, int tagged, const char *suspect);
// A K.P.D application (NET_EXT_KPD_APPLY, op kpd_apply): the answer is policeRes code 6.
void accounts_kpd_apply(ClientConnection *client, const char *discord, const char *reason);
// The K.P.D window's Demos (tab 4, op kpd_demos: a page of 30 suspect groups from `off`, the name `q`)
// and Reviewers (tab 5, op kpd_reviewers) lists (NET_EXT_KPD_LIST): the answer is code 4 / 5.
void accounts_kpd_list(ClientConnection *client, int tab, int off, const char *q);
// /kpd <name> on|off and /kpd apps (chat_commands.c, DEV): ops kpd_officer / kpd_apps; the answer goes
// to `requester` (a connection id, or CHAT_CMD_CONSOLE: the log). A grant reaches a connection of that
// account here at once. 0 when nothing could be sent (told already).
int accounts_kpd_officer(int requester, const char *name, int on);
int accounts_kpd_apps(int requester);
void net_resume_ent(ClientConnection*, int spectate);              // replay a deferred ENT
void net_comp_lobby_sync(ClientConnection*);                        // v7 syncLobbyP(player, id, 0) once the identity is known
int net_client_is_dev(const ClientConnection*);                    // DEV checks: op, the server's machine, a DEV account
void net_client_guest_name(ClientConnection*);                      // Guest_N again (live sign-out)
unsigned int net_revoke_lost_cheats(ClientConnection*, int lost);    // lost DEV / host: what they allowed, off
