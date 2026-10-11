#pragma once
// Krunker v3.1.1 chat "/" commands: the io_ct command table of the v3.1.1 offline server
// (jails/kru/src/Krunker_v3.1.1/server/packetHandlers.ts, `cmds`). The original game
// client sends every chat line raw ("ct"); the server parses a leading "/" and answers
// the sender with magenta Server lines ("ch", "Server", text, 2). Unknown commands are
// broadcast as ordinary chat.
//
// Shared by the native server (server/src/net.c, plus its stdin CLI as the console
// sender) and the offline game (client/src/net/local_game.c): each wires a
// ChatCommandHost with its connections. Pure game logic, no transport or GL. The command
// table and its per-command native status are documented in doc/console.md.
#include <shared.h>

#define CHAT_CMD_ADDRESS_SIZE 48
#define CHAT_CMD_IP_BANS_MAX 64
#define CHAT_CMD_CONSOLE (-1000)   // sender id of the server console: every permission, replies go to host->log

// Who may run a command when the host has perms on (v3.1.1 PERMS, --perms): requireDev /
// requirePremiumCheatPerm. With perms off (the default, like the v3.1.1 server) everyone may.
typedef enum {
    CHAT_PERM_ANY = 0,
    CHAT_PERM_DEV = 1,       // requireDev: DEV accounts
    CHAT_PERM_CHEAT = 2,     // requirePremiumCheatPerm: DEV or the premium host of a custom lobby
} ChatCommandPerm;

typedef struct {
    const char *name;        // "kick"
    const char *alias_of;    // "c" for "count", NULL otherwise
    const char *usage;       // "<name|id>"
    const char *desc;        // the v3.1.1 dev console description
    ChatCommandPerm perm;
} ChatCommandInfo;

// One connection as the command layer sees it.
typedef struct {
    int id;                  // connection id == Player.uid
    const char *name;
    const char *address;     // dotted IPv4, "" unknown (the local server)
    int ping_ms;             // -1 unknown ("calculating")
    Player *player;          // NULL: still in the menu
    int spectating;
    int dev;                 // passes requireDev while perms are on
} ChatPeer;

typedef struct ChatCommandHost {
    Game *game;
    void *user;
    int perms;               // v3.1.1 PERMS: 0 = everyone may run every command
    const char *lobby;       // /server line 1 ("Lobby X | region Y | socket Z")
    const char *region;
    float tick_rate;         // Hz

    int  (*peer_count)(void *user);
    int  (*peer_at)(void *user, int index, ChatPeer *out);    // 0 = no such index
    // ws.close(4002 "kicked") / (4003 "banned"): the host may defer it past the current packet.
    void (*disconnect)(void *user, int id, const char *message, const char *code);
    // cycleToMap(map, mode): every client reloads on `map_index` in game->modes[mode_index].
    void (*change_map)(void *user, int map_index, int mode_index);
    // Replies to CHAT_CMD_CONSOLE (and a copy of every broadcast) when set.
    void (*log)(void *user, const char *text);

    // per-server state (pnhBannedAddresses, forceCollideState)
    char ip_bans[CHAT_CMD_IP_BANS_MAX][CHAT_CMD_ADDRESS_SIZE];
    int ip_ban_count;
    const Map *force_collide_map; // map /forcecol is ON for (a map change turns it off)
} ChatCommandHost;

// io_ct after the burst limit: 1 when `text` was a known "/" command (handled, not
// broadcast), 0 when it is ordinary chat (unknown commands included).
int chat_command_handle(ChatCommandHost*, int sender_id, const char *text);
// pnhBannedAddresses.has(address): connections from it are refused (close 4003).
int chat_command_ip_banned(const ChatCommandHost*, const char *address);
// The table (for help, completion and doc/console.md).
const ChatCommandInfo *chat_command_list(int *count);
const ChatCommandInfo *chat_command_find(const char *name);
// /forcecol on a map: on = every non-colliding solid object collides (Object.force_col
// marks them), off = restores them. Returns how many objects changed. The server and
// every client (NET_CHEAT_FORCE_COLLIDE) apply it to their own copy of the map.
int chat_map_force_collide(Map*, int on);
// parseToggleLiteral: "on"/"1"/"true" -> 1, "off"/"0"/"false" -> 0, NULL/other -> !current.
int chat_parse_toggle(const char *arg, int current);
