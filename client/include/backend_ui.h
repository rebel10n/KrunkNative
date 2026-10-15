#pragma once
// Menu side of the brain backend (client/include/backend.h): the v3.1.1 account
// (index.js sendAccountRequest / accountResponse / updateAccount / cK, windows[4])
// and the matchmaker client (ClientManager.js + windows[1] Servers, quickJoinRegion,
// switchServer, setGameInstanceId, Host Game). Everything is inert when
// backend_enabled() is 0: the guest nickname, the offline game and direct connects
// keep their pre-backend behaviour.
//
//   client/src/ui/account_ui.c   account state, social-socket auth, header bar,
//                                Account window (5) and the LOGIN REQUIRED windows
//   client/src/ui/servers_ui.c   ping-list / game-list / seek-game / host-game, the
//                                Servers window (2), region label, server switching
#include <client.h>
#include <html_dom.h>
#include <cJSON.h>
#include <ui_logic.h> // StrBuf

// ---- account_ui.c (Krunker `acc`)
typedef struct {
    int id;
    char name[32];        // account username
    char alias[32];       // premium display name, else name
    char clan[16];        // "" = none
    double kills, deaths, wins, games, funds, score, time_played;
    double elo;           // player_elo: the current season's ranked rating
    int level;
    double level_prog;    // v7.1.4 getLevelProg(): percent, linear in score (WP-E part 2c)
    int premium, featured, is_dev, is_mod, hack, has_2fa;
    int creator;          // v10 creatorBadge: kn_brain's account info "creator_badge" (players.creator_badge)
    int following, followers;
    int job_rating, job_rating_pos; // K.P.D Helpful Reports: rated calls / tagged ones (data 38 / 39)
    int mail_unread;
    cJSON *stats;         // acc.stats (object, owned)
    cJSON *data;          // the raw 40-slot data array (owned)
    cJSON *badges;        // acc.badges: earned badges.js indices (array, owned; kn_brain info "badges")
} AccountInfo;

void account_ui_init(Client*);              // listens on the social socket; restores the saved session
const AccountInfo *account_get(void);       // NULL = guest (Krunker `acc == null`)
const char *account_session_token(void);    // saved social session token, "" none
int  account_ui_logout(void);               // logoutAcc (menu Logout, console `logout`): 1 logged out, 0 guest
// The alt manager's switch: sign out of the current account, then sign in with a saved session.
// 0 started, -1 no account server in this build.
int  account_ui_use_session(const char *username, const char *token);
void account_ui_tick(Client*);              // per frame: header bar once the page exists
void account_send_frame(const char *label);  // one no-argument social frame ("ttl": the account's titles)
int  account_ui_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, int argc); // -1 = not ours
int  account_ui_reset_active(void);           // KrunkNative forgot password: a reset step is up in window 5
int  account_ui_reset_enter(HtmlElem target); // KrunkNative forgot password: Enter in a reset field submits its step (1 = handled)
int  account_ui_twofa_handler(const char *name); // 1 for the 2FA popup's handlers (sub2FA, onInput2FA, onKeyDown2FA): every page has #twoFAPop
// Window markup when the account changes what v3.1.1 shows; NULL = keep the static capture.
char *account_window_gen(int id, const char **header);
void account_window_post(int id);
double account_reward_ms(void);             // daily free KR: ms left, 0 claimable, -1 no account / level too low
void account_set_funds(double funds);       // updateFunds(funds): header + windows
void account_set_clan(const char *tag);      // windows[12]: acc.clan (NULL / "" = none)
// WP-E part 2c: the account's selected custom badge (stats.bdg) when it owns it and the
// customBadge privacy setting shows it, else -1 (menu header, v7 updateClassPreview).
int account_custom_badge(void);
void account_ui_scenario(const cJSON *account); // --render-scenario: state.account stands in for a login
void account_ui_scenario_post(void);             // after the scenario's window: account.c2fa (the 2FA popup)
int account_owns_badge(int badge);
void account_popup(int kind, const char *error, const char *text); // popupResponse: 0 error, 1 success, 2 loading
void account_readable_time(char *out, size_t n, double ms);         // UTILS.getReadableTime
// account.activeChallenges (kn_brain "chgL"; NULL = not loaded / guest), its request, and
// challenges.updateNames' N
const cJSON *account_challenges(void);
void account_challenges_request(void);
void account_challenge_name(const cJSON *challenge, char *out, size_t n);

// ---- battlepass_ui.c (v7.1.4 windows[5], showWindow(6): the Battle Pass; the end screen's #endBPLayout)
char *battlepass_window_gen(int id, const char **header); // NULL: not window 6 / guest (the captured Sign-in card)
void battlepass_window_post(int id);
int  battlepass_ui_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, int argc); // -1 = not ours
int  battlepass_ui_frame(const char *label, const cJSON *frame); // bpdl, bppd, bpclmr, purbp, purbpl, bpEnd; 1 = handled
void battlepass_ui_reset(void);             // logout: windows[5].data, account.BP, bpData
void battlepass_ui_challenges_changed(void); // the challenge list changed (tab 1 re-renders)
int  battlepass_ui_tier(void);              // account.BP.tier (0 without)
void battlepass_ui_end_clear(void);         // a new end screen: no bpData until kn_brain sends it
void battlepass_ui_scenario(const cJSON *state); // --render-scenario: state.account.battlepass
int  battlepass_ui_end_html(StrBuf *sb);    // tabs.performance's #endBPLayout; 0 = none (no account / bpData)

const struct BattlePass *battlepass_ui_pass(void); // windows[5].data parsed (NULL none)
int64_t battlepass_ui_bpp(void);            // account.BP.bpp (0 without)
void battlepass_ui_request(void);           // v10: "bpdl" once an account is signed in (no-op when asked)

// ---- v10_rewards.c (Krunker v10.0.2: signup rewards, windows[32], the sidebar's Battle Pass item)
void v10_show_signup_rewards(void);         // window.showSignupRewards
void v10_rewards_end_screen(void);          // endScreen.init: #matchEndSignupIncentives (guests)
int  v10_rewards_frame(const char *label, const cJSON *frame); // "prr"; 1 = handled
char *v10_rewards_window_gen(int id);       // window 33 on the v10 page; NULL otherwise
int  v10_rewards_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, int argc); // -1 = not ours
void v10_rewards_dom_init(void);            // page ready: resets the sidebar item / window state
void v10_rewards_tick(void);                // per frame: the sidebar item

// ---- clan_ui.c (windows[12], the menu Clans window = showWindow(13))
char *clan_window_gen(void);                // guest: LOGIN REQUIRED; member: the clan page
int  clan_ui_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, int argc); // -1 = not ours
// v10 Turf Wars popup (turf_ui.c: openTurfWars, genericPopup(_51011, 'defaultModal'))
void turf_ui_scenario(const cJSON *state); // --render-scenario: state.turf
int  turf_ui_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, int argc); // -1 = not ours
void clan_ui_frame(const cJSON *frame);     // "cln" / "clnL" replies
// v10 Market & Trading (v10_market.c): window.openExchange(tab, itemIndex) -> the exchangeModal popup
void v10_market_open(int tab, int item);
int  v10_market_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, int argc); // -1 = not ours
void v10_market_menu_setup(void);           // the v10 page's #menuBtnMarket item + the social listener
void v10_market_scenario(const cJSON *state); // --render-scenario: state.market
int  v10_market_owns_frame(const char *label); // 1 = the open exchange answers this reply (account_ui.c skips it)
void clan_ui_reset(void);                   // account changed: drop clanData
void clan_ui_scenario(const cJSON *state);  // --render-scenario: state.clan {data, tab, motd, loot, recruit}
int  account_add_skin(int skin_index);      // acc.skins += 1 of skin; returns the owned count
void account_remove_skin(int skin_index);   // acc.removeSkin: -1 of skin (a redeemed spin ticket)
void account_set_last_reward(double ms);    // acc.lastReward (the Free KR wheel's clock)

// ---- store_ui.c (windows[13] prize wheels, prizeWheel / spinResponse)
void store_skin_preview(int skin_index, char *out, size_t n); // UTILS.getPreview(skin): its asset-host URL (html_dom_asset_url)
char *store_window_gen(void);               // windows[13].gen(): the v7.1.4 Store (guest and signed in), malloc'd; v10 page: the Svelte store
void store_ui_window_post(int id);          // v10 page: #menuWindow.eterno on the Eterno Shop tab
int  store_ui_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, int argc); // -1 = not ours
void store_ui_frame(const cJSON *frame);    // "spin" / "spinKR" / "spinH" replies
void store_ui_free_kr_wheel(void);          // claimReward(): the Free KR wheel
void store_ui_account_changed(void);        // a login / logout: the spin history is the old account's
void store_ui_tick(void);                   // per frame: spin screen geometry + reveal
void store_ui_scenario_wheel(int index, int multi); // --render-scenario state.spin(+spinMulti): prizeWheel(index, null, multi) with noAnim
void store_ui_scenario_result(const cJSON *result); // state.spinResult {rand, item}: the spin's reply, the spin skipped

// ---- servers_ui.c (ClientManager + windows[1])
void servers_ui_init(Client*, int brain_mode); // brain_mode: matchmake at startup instead of the default server
void servers_ui_set_director_mode(int on);    // Krunker Strike: the director matchmakes at startup (strike_director.h)
int  servers_ui_brain_mode(void);            // matchmaking on and the session is a brain lobby (not pinned)
int  servers_ui_pinned(void);               // 1 = pinned to a direct server (console connect, Direct Connect)
void servers_ui_connect_direct(const char *host, int port); // console `connect`: a pinned direct session
void servers_ui_disconnect(void);           // console `disconnect`: stay disconnected, no automatic seek
int  servers_ui_reconnect(void);            // console `reconnect`: the last server (lobby: sought again); 1 = started
int  servers_ui_reconnect_offline(void);    // console `reconnect`: 1 = `disconnect` left the offline game, start it again
// F5's restart fallback (game_reload.c): what the next process joins to come back to this session,
// 2 = the matchmade lobby id (also while it is being joined), 1 = "host:port" (the caller checks the
// connection), 0 when there is no server to come back to.
int  servers_ui_session_target(char *out, size_t n);
void servers_ui_tick(Client*);
int  servers_ui_retry(void);                 // the error screen's "seek a new game" link; 1 = handled
int  servers_ui_quick_match(void);           // v10 Quick Match: = servers_ui_find_new_game
int  servers_ui_find_new_game(void);         // F4 / Quick Match per Settings > Quickplay (seek_prefs.h); 0 = nothing to seek
// A lost session (every NET_STATUS_FAILED, main thread): a server shutdown ERROR (004004 / 004006),
// a refused join key (004001) or a session the transport lost without a server ERROR (dropped: not
// a kick / ban) starts a rejoin through the matchmaker with a fresh ticket (direct servers: the
// transport retries). error may be rewritten (a second refusal, quick drops). 1 = rejoining.
// had_session: the session had been joined (a drain refuses new connections with 004004 too).
// A live session's server stopping for an update (004004) restarts the client instead when
// Settings > Client > Auto-restart is on (game_reload.h): the new process rejoins the lobby.
int  servers_ui_connection_lost(char *error, size_t error_size, const char *code, int dropped, int had_session);
// The transport gave up (the retry budget is spent, client_net_give_up): no rejoin any more.
void servers_ui_gave_up(void);
int  servers_ui_rejoining(void);             // 1 = a rejoin is due / seeking, or the client restarts (the error screen says so)
const char *servers_ui_rejoin_text(void);    // "Reconnecting..." / "Rejoining a game..." / "Restarting..." for that line
// A restart's resume (game_reload.c): hold the startup seek, then rejoin the lobby the old
// process was in with the restart rejoin's backoff (REJOIN_RESTART; the screen says Game Updating).
void servers_ui_hold_startup_seek(int hold);
void servers_ui_resume_rejoin(const char *game_id);
// The resume joined its session itself (offline game, direct server, official game): no startup seek.
void servers_ui_skip_startup_seek(void);
// Reload (F5's relaunch / the console's `reload`): the lobby the player was in, sought by its game id
// (never the Quickplay launch seek, never excludeGame); only when it is gone or full, one plain seek
// elsewhere, and a notice says so. _resume: the new process's startup seek; _rejoin: in place now,
// 0 = not in a matchmade lobby.
void servers_ui_reload_resume(const char *game_id);
int  servers_ui_reload_rejoin(void);
int  servers_ui_join_link(const char *text);  // Join field / pasted link or "NODE:lobby" id (v3.1.1 f9); 1 = joining, 0 = refused (a notice says why)
// Connect window / console connect / Join field given a game link or lobby id (CH2:kz862) or a krunker.io
// game link: 1 = joining (the brain's seek, or the official krunker.io join), 0 = not a game link (err
// untouched: read it as host[:port]), -1 = a game that cannot be joined here (err = the text to show).
int  servers_ui_connect_game(const char *text, char *err, size_t n);
// The address of a direct connect: "local" / "default", or host[:port] with host an IPv4 address,
// localhost or a dotted name and port 1..65535 (default 21015). 1 = valid (host, port written); 0 = err says why.
int  servers_ui_parse_address(const char *text, char *host, size_t host_n, int *port, char *err, size_t err_n);
int  servers_ui_join_game(const char *game_id); // v10 switchServer(id) (a friend's game): seek it; 1 = joining, 0 = no matchmaker
int  servers_ui_join_password(const char *password); // v10 submitGameCode: the same game again with the server password
void servers_ui_invite_link(char *out, size_t n); // copyInviteLink: <origin>/?game=<id>, the id unescaped, short (CH2:kz862)
int  servers_ui_account_required(void);     // 1 = the last seek answered AccountRequired to a guest (error screen: Log in)
char *servers_ui_region_options(const char *selected); // region <option>s (2-letter codes, place name as title); malloc'd
int  servers_ui_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, int argc); // -1 = not ours
char *servers_window_gen(int id, const char **header); // windows 2 (Servers), 8 (Host), 10 (Custom Games)
void servers_ui_set_games(const cJSON *list);    // fixed /mm/game-list answer (scenarios, KRUNKNATIVE_SERVER_LIST); NULL = the brain's
// v10 window.fetchMatches: {map: players} of the custom games in the game list (v10 Popular Now);
// fn runs once (at once for a fixed list or without a brain), the object is only valid during the call
typedef void (*ServersMatchesFn)(void *user, const cJSON *matches);
void servers_ui_fetch_matches(ServersMatchesFn fn, void *user);
// KrunkNative addition: the menu's Connect button (next to Invite / Join) and its window
// showWindow id of KrunkNative's Connect window: above every Krunker page's windows[] (it was 53,
// past v7's 52, which is v10's second Change Loadout entry; ui_logic.h KN_WINDOW_BASE)
#define SERVERS_WINDOW_CONNECT (KN_WINDOW_BASE + 2)
void servers_ui_menu_setup(void);                // inject #menuBtnConnect into #subLogoButtons
char *servers_connect_window_gen(void);          // the Connect window (host[:port], console `connect` semantics)
// Build versions (ci-release; brain fields: doc/backend.md "Build version"). entry = a game-list
// tuple or a finder /servers entry. The _html helpers return malloc'd markup (caller frees).
void  servers_ui_note_list_response(const cJSON *response); // finder / game-list answer: download URL
int   servers_ui_entry_outdated(const cJSON *entry);
int   servers_ui_entry_incompatible(const cJSON *entry);
char *servers_ui_entry_badge_html(const cJSON *entry);      // "" or an Outdated / Incompatible badge
char *servers_ui_download_button_html(const cJSON *list_response); // "Download server" -> downloadServer()
void  servers_ui_open_download(void);                       // downloadServer(): the release page / OS asset
int  servers_ui_host_game(const char *config_json); // /mm/host-game: disabled (hosting = server_bin), 0 unless KRUNKNATIVE_ALLOW_CLIENT_HOST
// Join a lobby with a ticket the brain issued elsewhere (ranked match): native TCP host:port,
// web web_url or /native?target=.
// playMap(name): join a brain lobby on that map, else host it (brain); a published map that
// is not in the catalogue is fetched (/api/map-data) and played offline otherwise. 1 = handled.
int  servers_ui_play_map(const char *name, int catalogue);
void servers_ui_query(const char *query);   // the game page's ?play= / ?game= (acted on in tick)
void servers_ui_redirect(const char *game_id, const char *host, int port, const char *reason); // NET_PACKET_REDIRECT
void servers_ui_join_ticket(const char *host, int port, const char *game_id, const char *ticket, const char *web_url);

// ---- ranked_ui.c (v10's rankedMenuModal popup, openRankedMenu, + the queue session)
char *ranked_ui_window_gen(void);           // windows[26].gen(): '' (v10 only has the popup)
int  ranked_ui_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, int argc); // -1 = not ours
const char *ranked_ui_status(void);   // queueStatusMessage, NULL none (play instructions subtitle)
const char *ranked_ui_ready(void);    // queueReadyMessage
void ranked_ui_class_changed(int index);
int  ranked_ui_in_ranked(void);       // queued or in a ranked match (v7 config.isComp: custom.host.ranked)
void ranked_ui_left_match(void);      // the ranked match's lobby was left
// --render-scenario: state.ranked {open, tab, profile, board, history, shop, ...} stands in for the brain
void ranked_ui_scenario(const cJSON *state);
// Custom Games window (windows[9]) rows from GET /api/maps?index=<tab>&pos=<n>&accountId=:
// -1 = no brain (use the local catalogue), 0 = loading (requested; the window re-renders),
// 1 = *rows (borrowed cJSON array of map rows). _more: another page may follow (Load More).
int  servers_games_tab_rows(int tab, const cJSON **rows);
int  servers_games_tab_more(int tab);

// ---- backend_startup.c (main.c glue)
// Strips --brain <url> and --connect <host[:port]> from argv (before the other parsers).
void backend_parse_args(int *argc, char **argv);
// Startup decision after Client is set up: applies --connect, initialises the brain
// client, account and matchmaker. Returns 1 when the brain matchmaker connects the
// game (main.c must not start the default transport), 0 for the classic paths
// (offline game, --connect, KRUNKNATIVE_SERVER_PORT, web ?ws=, no brain, scenarios).
int backend_startup(Client*, int offline, int scenario);
// Per frame (client_tick): HTTP / social socket delivery, header bar, matchmaker.
void backend_frame(Client*);
