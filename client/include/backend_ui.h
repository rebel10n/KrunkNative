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
    double elo, elo2, elo4;
    int level, level_prog;
    int premium, featured, is_dev, is_mod, hack, has_2fa;
    int following, followers;
    int mail_unread;
    cJSON *stats;         // acc.stats (object, owned)
    cJSON *data;          // the raw 40-slot data array (owned)
} AccountInfo;

void account_ui_init(Client*);              // listens on the social socket; restores the saved session
const AccountInfo *account_get(void);       // NULL = guest (Krunker `acc == null`)
const char *account_session_token(void);    // saved social session token, "" none
void account_ui_tick(Client*);              // per frame: header bar once the page exists
int  account_ui_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, int argc); // -1 = not ours
// Window markup when the account changes what v3.1.1 shows; NULL = keep the static capture.
char *account_window_gen(int id, const char **header);
void account_window_post(int id);
void account_set_funds(double funds);       // updateFunds(funds): header + windows
void account_set_clan(const char *tag);      // windows[12]: acc.clan (NULL / "" = none)

// ---- clan_ui.c (windows[12], the menu Clans window = showWindow(13))
char *clan_window_gen(void);                // guest: LOGIN REQUIRED; member: the clan page
int  clan_ui_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, int argc); // -1 = not ours
void clan_ui_frame(const cJSON *frame);     // "cln" / "clnL" replies
void clan_ui_reset(void);                   // account changed: drop clanData
int  account_add_skin(int skin_index);      // acc.skins += 1 of skin; returns the owned count
void account_elo_icon_html(char *out, size_t n, double elo, const char *style); // UTILS.eloIconHTML

// ---- store_ui.c (windows[13] prize wheels, prizeWheel / spinResponse)
void store_skin_preview(int skin_index, char *out, size_t n); // UTILS.getPreview(skin) page path
void store_wheels_html(StrBuf *sb);         // Store tab 0 cards
int  store_ui_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, int argc); // -1 = not ours
void store_ui_frame(const cJSON *frame);    // "spin" replies
void store_ui_tick(void);                   // per frame: spin screen geometry + reveal

// ---- servers_ui.c (ClientManager + windows[1])
void servers_ui_init(Client*, int brain_mode); // brain_mode: matchmake at startup instead of the default server
int  servers_ui_brain_mode(void);
void servers_ui_tick(Client*);
int  servers_ui_retry(void);                 // the error screen's "seek a new game" link; 1 = handled
int  servers_ui_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, int argc); // -1 = not ours
char *servers_window_gen(int id, const char **header);
int  servers_ui_host_game(const char *config_json); // /mm/host-game: disabled (hosting = server_bin), 0 unless KRUNKNATIVE_ALLOW_CLIENT_HOST
// Join a lobby with a ticket the brain issued elsewhere (ranked match): native TCP host:port,
// web web_url or /native?target=.
// playMap(name): join a brain lobby on that map, else host it (brain); a published map that
// is not in the catalogue is fetched (/api/map-data) and played offline otherwise. 1 = handled.
int  servers_ui_play_map(const char *name, int catalogue);
void servers_ui_query(const char *query);   // the game page's ?play= / ?game= (acted on in tick)
void servers_ui_redirect(const char *game_id, const char *host, int port, const char *reason); // NET_PACKET_REDIRECT
void servers_ui_join_ticket(const char *host, int port, const char *game_id, const char *ticket, const char *web_url);

// ---- ranked_ui.c (windows[26] + the queue session)
char *ranked_window_gen(void);
int  ranked_ui_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, int argc); // -1 = not ours
const char *ranked_ui_status(void);   // queueStatusMessage, NULL none (play instructions subtitle)
const char *ranked_ui_ready(void);    // queueReadyMessage
void ranked_ui_class_changed(int index);
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
