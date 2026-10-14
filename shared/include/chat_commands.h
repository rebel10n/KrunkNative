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

// Who may run a command (v3.1.1 PERMS, --perms): requireDev / requirePremiumCheatPerm.
// KrunkNative (security, doc/v7/command-auth.md): unlike v3.1.1, perms off alone opens nothing;
// only perms off + cheats_open (gameserver.cheats "all", --open-cheats, the offline sandbox)
// lets everyone run every command but the CHAT_PERM_HOST_DEV ones.
typedef enum {
    CHAT_PERM_ANY = 0,
    CHAT_PERM_DEV = 1,       // requireDev: DEV accounts
    CHAT_PERM_CHEAT = 2,     // requirePremiumCheatPerm: DEV or the premium host of a custom lobby
    CHAT_PERM_DEV_CHEAT = 3, // requireDev, and a cheat (unlimited ammo, score, summon...): see cheats_open
    CHAT_PERM_HOST_DEV = 4,  // Krunker v7.1.4 requireHostOrDev (bot_*): DEV peers / the custom host only, perms on or off
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
    int premium_host;        // isPremiumCustomHost: the account that hosts this custom lobby (CHAT_PERM_CHEAT)
    int account_id;          // player.account.id, 0 = guest (no account ban possible)
    int ranked;              // a ranked match is on (ObjectManagerPerms.ranked: give / drop)
} ChatPeer;

typedef struct ChatCommandHost {
    Game *game;
    void *user;
    int perms;               // v3.1.1 PERMS: 1 = DEV commands for DEV peers even with cheats_open
    // KrunkNative (deliberate difference): with perms off, v3.1.1 lets everyone use every command
    // (/kick /ban /ip /god /noclip ...). 0 (server_bin default, gameserver.cheats "auth") keeps
    // moderation for DEV peers and the cheats for DEV peers / the custom host even then; 1 ("all")
    // with perms off = v3.1.1's open server.
    int cheats_open;
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
    // pnhBannedAccountIds (optional; NULL = every player is a guest, v3.1.1's guest replies):
    int  (*ban_account)(void *user, int account_id, const char *name); // add (persisted), 1 = ok
    int  (*unban_account)(void *user, int account_id);                 // 1 = it was banned
    int  (*list_account_bans)(void *user, int *out, int max);          // ids into out, returns the total
    // KrunkNative give / drop changed connection `id`'s Object Manager extra weapons (optional):
    // the server keeps them on the connection, as after a SPAWN_REQ.
    void (*om_extras_changed)(void *user, int id);

    // per-server state (pnhBannedAddresses, forceCollideState)
    char ip_bans[CHAT_CMD_IP_BANS_MAX][CHAT_CMD_ADDRESS_SIZE];
    int ip_ban_count;
    const Map *force_collide_map; // map /forcecol is ON for (a map change turns it off)
} ChatCommandHost;

// io_ct after the burst limit: 1 when `text` was a known "/" command (handled, not
// broadcast), 0 when it is ordinary chat (unknown commands included).
int chat_command_handle(ChatCommandHost*, int sender_id, const char *text);
// io_prmA(index): the premiumKeys (and KrunkNative's N key, index 0): 0 noclip, 1 god,
// 2 kill self, 3 smite, gated like /noclip (requirePremiumCheatPerm). The noclip toggle
// reaches the client as NET_CHEAT_NOCLIP; denials as ["notify", ...].
void chat_command_premium_action(ChatCommandHost*, int sender_id, int index);
// The command gate (requireDev / requirePremiumCheatPerm) for the server's own packets
// (io_tp: CHAT_PERM_DEV_CHEAT, io_cycle: CHAT_PERM_DEV): 1 = allowed, else the sender got
// the notify. label names the action in the denial log.
int chat_command_allowed(ChatCommandHost*, int sender_id, ChatCommandPerm, const char *label);
// KrunkNative third-person camera (doc/v7/decisions.md "Third person"): may this sender use it?
// Allowed in the offline sandbox, for DEV peers, the custom game's host, everyone in a custom game
// whose host set knTPCam, and on a server whose owner opened the cheats (perms off, cheats "all");
// refused in ranked / comp (devs excepted) and on public servers. 0 = refused, *reason (if given)
// is the notify text.
int chat_third_person_allowed(ChatCommandHost*, int sender_id, const char **reason);
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
