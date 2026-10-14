#pragma once
// Game server configuration: the "gameserver" section of config/config.json
// (documented key by key in config/config.example.json and doc/backend.md).
//
// Precedence, lowest to highest: built-in defaults < config file < environment
// (KN_GAMESERVER_<KEY>, plus the v3.1.1 MM_* / PUBLIC_* / PERMS aliases) < CLI flags.
// Without a config file every value is the default and server_bin behaves exactly
// like the standalone server it was before the backend existed.
#include <shared.h>
#include <modes.h>

#define GS_CONFIG_DEFAULT_PATH "config/config.json"
#define GS_MAX_LIST 64          // maps, modes, ops, billboards, MOTD lines, custom map dirs
#define GS_STR_SMALL 64
#define GS_STR_LARGE 256

// v3.1.1 billboards.json entry / game.updateBillboards(): txt, txtCol, bgCol, plus the
// KrunkNative billboard_image (an asset path, NET_PACKET_BILLBOARD_IMAGE). An entry has text,
// an image or both; it overrides every BILLBOARD of every map (doc/v7/decisions.md).
typedef struct {
    char txt[64];
    char txt_col[8];
    char bg_col[8];
    char image[NET_BILLBOARD_IMAGE_MAX];
} GsBillboard;

typedef struct {
    // ---- identity / network (brain registration)
    char region[32];            // brain region key, the 2-letter code ("SV", "CH"; MM_NODE_REGION)
    char node_id[16];           // lobby id prefix ("NY" -> "NY:ab12c"; MM_NODE_ID)
    char name[GS_STR_SMALL];    // human-readable server name (server browser)
    char listen_host[GS_STR_SMALL];
    unsigned short listen_port;
    char public_host[128];      // what clients connect to (PUBLIC_DOMAIN)
    unsigned short public_port; // 0 = listen_port (PUBLIC_PORT)
    char brain_url[GS_STR_LARGE];   // ws://host:port/path of kn_brain's internal socket, "" = standalone
    char brain_secret[GS_STR_SMALL * 2]; // bearer secret (never logged)
    char join_secret[GS_STR_SMALL * 2];  // HMAC key of the brain's join tickets ("" = brain_secret)
    char web_url[GS_STR_LARGE];          // ws(s) URL web clients use (net-bridge /native), "" none
    char brain_http_url[GS_STR_LARGE];   // brain HTTP base for /api/map-data ("" = from brain_url)
    unsigned short tick_rate;

    // ---- lobby
    int max_players;                     // lobby size (all modes)
    int mode_max_players[MODE_COUNT];    // per mode override, 0 = max_players
    int team_size;                       // team modes: lobby size = 2 * team_size (0 = off)
    int mode_team_size[MODE_COUNT];
    char maps[GS_MAX_LIST][GS_STR_SMALL];  // rotation: catalogue names / keys / indices ("" none = Krunker rotation)
    int map_count;
    int modes[GS_MAX_LIST];              // rotation mode ids (0 = FFA only)
    int mode_count;
    char custom_map_dirs[8][GS_STR_LARGE];
    int custom_map_dir_count;
    float round_seconds;                 // all modes, 0 = the mode's game time
    float mode_round_seconds[MODE_COUNT];
    GameConfig game;                     // "game": v7.1.4 serverConfig keys (maxPlayers, gameTime, lives, gravMlt, ...)
    int has_game;
    // WP-E server presets: host_type plays the rotation as that config.hostTypes entry (0 custom,
    // 1 comp, 2 rent, 3 zombies, 4 raid; -1 a public game) and map_config applies each map's
    // embedded "config" under the "game" keys (both make it a hosted game: setupGameConfig +
    // applyConfig per round, random vote picks). game_request is the "game" object as given.
    int host_type;
    int map_config;
    char game_request[4096];
    int custom_games;                    // accept hosted custom games from the brain

    // ---- messages
    char motd[8][NET_CHAT_TEXT_SIZE];    // server chat lines to every joiner
    int motd_count;
    char join_message[NET_CHAT_TEXT_SIZE];  // "$ joined the game" ($ = name), "" = off
    char leave_message[NET_CHAT_TEXT_SIZE];
    GsBillboard billboards[GS_MAX_LIST]; // rotated per map cycle (v3.1.1 pickBillboard)
    int billboard_count;

    // ---- moderation
    float idle_timeout;                  // seconds alive without any activity, 0 = never
    float idle_menu_timeout;             // seconds dead / in the menu without any activity, 0 = never
    float idle_move_threshold;
    int perms;                           // v3.1.1 PERMS: DEV chat commands only for ops / loopback
    int cheats_open;                     // gameserver.cheats "all": v3.1.1 perms-off cheats for everyone (default "auth")
    char ops[GS_MAX_LIST][GS_STR_SMALL]; // account ids or names that are DEV (op) on join
    int op_count;
    char bans_file[GS_STR_LARGE];        // persisted IP / name / account bans, "" = memory only
    int allow_guests;                    // with a brain: players without an account may join
    int require_ticket;                  // with a brain: every join needs a ticket or session token
    float join_timeout;                  // seconds a brain-linked server waits for JOIN

    // ---- rewards (v3.1.1 endGame: min(premium ? 40 : 30, floor(score * rewardMlt / 100)))
    int earn_kr;
    int kr_cap;
    int kr_cap_premium;
    float kr_multiplier;                 // on top of the mode's rewardMlt

    // ---- dedicated server (server.cfg, doc/backend.md 8.7)
    char password[16];                   // sv_password: JOIN.password must match ("" = open server)
    char rcon_password[GS_STR_SMALL];    // remote console password ("" = local rcon off; coordinator rcon still works)
    char rcon_host[GS_STR_SMALL];        // local rcon bind address (default 127.0.0.1)
    unsigned short rcon_port;            // local rcon TCP port, 0 = off
    unsigned short query_port;           // UDP server-info query port, 0 = the game port
    int online_mode;                     // 1: accounts verified by the central auth service (hasJoined); guests still welcome
    char auth_url[GS_STR_LARGE];         // central auth base (https://coordinator.krunk.io)
    char finder_url[GS_STR_LARGE];       // server finder base, "" = not listed (opt-in heartbeat)
    char community[GS_STR_SMALL];        // community / network name shown in server lists
    char tags[GS_STR_SMALL * 2];         // extra comma-separated info-query tags
    int enforce_skins;                   // -1 auto (on with a coordinator), 0 off, 1 on
    float shutdown_grace;                // seconds a SIGTERM/SIGINT drain waits for players (0 = immediate)
    char shutdown_reason_file[GS_STR_LARGE]; // read at the first stop request: "update" = deploy (server_shutdown_reason)
    int lag_comp;                        // 1: hits rewind targets (v3.1.1 storeState / fetchState), 0 = current positions
    int lag_comp_anchor;                 // LAG_COMP_ANCHOR_*: 1 exact (client input timeline, default), 2 arrival + ping, 0 v3.1.1 ping * 2
    float lag_comp_scale;                // 0.1..1 (default 1) times each player's lagComp (ENT ent[11] slider)
    // Krunker v7.1.4 bot players (shared/src/bots.c; KrunkNative addition: v7 only has the dev
    // console). bots: keep N bots in the lobby; bot_quota: fill the lobby up to N players with
    // bots that leave as humans join (wins over bots). Both capped at config.botLimit (16) and
    // the lobby size. bot_difficulty: easy | normal | hard | max | a number (botDifficultyConfig).
    // bot_quota keeps an empty lobby empty: no humans in the game = no bots, after
    // bot_quota_grace seconds (default 10) so a quick rejoin keeps the match.
    int bots;
    int bot_quota;
    float bot_quota_grace;
    char bot_difficulty[16];
    char log_file[GS_STR_LARGE];         // append every log line here too ("" = stdout/stderr only)
    char cfg_path[GS_STR_LARGE];         // server.cfg actually loaded ("" none)

    // ---- bookkeeping
    char path[GS_STR_LARGE];             // file actually loaded ("" none)
} GsConfig;

extern GsConfig g_gs_config;

void gs_config_defaults(GsConfig*);
// Reads `path` (NULL: $KN_CONFIG, else config/config.json). A missing file is not an
// error when the path was not given explicitly. Returns 0 on success, else fills err.
int gs_config_load(GsConfig*, const char *path, int explicit_path, char *err, size_t err_size);
// server.cfg (CS:S style "key value" lines, layered over config.json's "gameserver"):
// set before gs_config_load(). path NULL = ./server.cfg, else ./cfg/server.cfg when present.
void gs_config_set_cfg_path(const char *path);
// Parses server.cfg text into a "gameserver"-shaped object (aliases mapped). Returns NULL
// and fills err on a syntax error. Exposed for tests.
cJSON *gs_cfg_parse(const char *text, char *err, size_t err_size);
// Applies KN_GAMESERVER_* and the v3.1.1 aliases. Returns the number of overrides.
int gs_config_apply_env(GsConfig*);
// --gameserver.<key>=<value> from the command line: recorded before gs_config_load().
int gs_config_set_cli(const char *key, const char *value);
// "gameserver" object -> config (also used for hot reload / tests). Returns 0 or fills err.
int gs_config_from_json(GsConfig*, const cJSON *section, char *err, size_t err_size);
// gameserver.game / server.cfg game.<varN>: the v7.1.4 serverConfig keys present, clamped (WP-E).
void gs_game_config_from_json(GameConfig*, const cJSON*);

// Lobby size / round length for a mode id (team sizes and per-mode overrides applied).
int gs_lobby_size(const GsConfig*, int mode_id);
float gs_round_seconds(const GsConfig*, int mode_id);

// The rotation's map list for game_configure (caller owns the array, not the JSON);
// NULL + count 0 = Krunker's default rotation.
const cJSON **gs_build_map_list(const GsConfig*, size_t *count);
int *gs_build_mode_list(const GsConfig*, size_t *count);

// Billboard for the n-th map cycle (NULL: none configured = engine defaults).
const GsBillboard *gs_billboard_for_cycle(const GsConfig*, unsigned int cycle);

int gs_is_op(const GsConfig*, int account_id, const char *name);
void gs_config_log_summary(const GsConfig*);
