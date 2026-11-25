#pragma once
// The server core (kn_server_core): connection table, packet dispatch, broadcast, the game
// tick step and the accounts gate. server_bin links it with its network frontend (TCP / UDP
// sockets, rcon, brain link, finder, CLI: server_cli.h); the client links it as the offline
// sandbox server (server/src/sandbox.c, server_sandbox.h) over an in-memory transport. No GL,
// sockets or threads of its own: the frontend owns the I/O and the threads.
#include <shared.h>
#include <social.h> // social hub requests (SocialAssembly)
#include <voice.h> // voice chat relay (io_vc)
#include <chat_commands.h>
#include <player_card.h> // WP-E: PLAYER_CARD_STAT_COUNT (ServerAccount)
#include <progression.h> // WP-E part 2c: PROGRESSION_STAT_COUNT (ServerAccount)
#include <net_ext.h> // NetSkinCopyIdsPacket (ClientConnection: the copies an ENT's slots name)
#include <pthread.h>

#ifdef WIN32
#include <winsock2.h>
#else
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <fcntl.h>
#endif

typedef struct {
    unsigned short tick_rate;
    unsigned short listen_port;
    unsigned short max_players;
    float idle_timeout;
    float idle_menu_timeout;
    float idle_move_threshold;
    int perms;                     // --perms / PERMS=1: DEV-only chat commands need an op'd or loopback client
    int cheats_open;               // --open-cheats / gameserver.cheats "all": cheats for everyone while perms are off
} ServerConfig;

#define SERVER_PLAYER_NAME_MAX 32

// Backend identity + match bookkeeping of one connection (server/src/accounts.c,
// doc/backend.md section 8). account_id 0 = guest. Standalone servers never leave
// SERVER_JOIN_READY.
typedef enum {
    SERVER_JOIN_READY = 0,     // identity settled (guest or account): ENT spawns
    SERVER_JOIN_AWAIT,         // brain-linked: waiting for NET_PACKET_JOIN (join_timeout)
    SERVER_JOIN_AUTH,          // JOIN received, waiting for the brain's auth-reply
} ServerJoinState;

#define SERVER_CLASS_STATS_MAX 32 // account.stats c0..c31 (v7.1.4 has 16 classes)
#define SERVER_TALLY_MAX 128      // challenge tallies of one round (key x weapon x streak x item)
#define SERVER_SKIN_PAINT_MAX 256 // painted store indices of one account (one paint per index; more are not sent)
#define SERVER_SKIN_STYLE_MAX 32  // styled store indices of one account (v10.0.7: two items have styles)
#define SERVER_SKIN_META_MAX 256  // store indices of one account with a worn copy's wear / serial / name tag
#define SERVER_SKIN_META_NAME 24  // NET_SKIN_META_NAME
#define SERVER_CHALLENGES_MAX 64   // active challenges of one account the game server follows mid-match (chal_told bits)

// one painted copy of the account, by store index (a shader paint has color -1, a colour paint shader -1)
typedef struct {
    int32_t skin;
    WornSkinPaint paint;
} ServerSkinPaint;

// One owned copy of the account ({ind, cnt: 1, id, m, st: 0} in the owned list): what a loadout slot naming it
// ("<copyId>#<index>", NetSkinCopyIdsPacket) wears. paint, style, seed and wear come from its m (seed: skin_copy_seed of
// m.seed or the id, deob-10.0.7.js:24346-24352); style 0 and wear < 0 are none.
#define SERVER_SKIN_COPY_MAX 512
typedef struct {
    int32_t id;
    int32_t skin;
    WornSkinPaint paint;
    int32_t style;
    int32_t seed;
    float wear;
    int32_t serial;                    // m.serial, 0 none
    char name[SERVER_SKIN_META_NAME];  // m.name (a Name Tag), "" none
    // m[stat] of a counter skin's copy, -1 none (skin_counters_load): a slot naming this copy counts from it, not from the
    // index's first counted copy (v10.0.7 counts from the slot's own m, deob-10.0.7.js:173031, m from :174120)
    int64_t counter;
    // what this match counted on this copy (a slot naming it), taken with the copy's id: v10.0.7 keys a match's skinStats
    // by the copy (deob-10.0.7.js:176205) and adds each to that copy (:191914-191936)
    int64_t pending;
} ServerSkinCopy;

// v10.0.4 counter skins (server/src/skin_counters_server.c): a counter skin the account owns, its copy's stat (m[stat],
// the counter's base) and what this match counted on it (skinStats, deob-10.0.4.js:175891-175893; saved with the commit)
#define SERVER_SKIN_COUNTER_MAX 32
typedef struct {
    int32_t skin;    // store.skins index
    int32_t stat;    // index in SKIN_STATS (shared.h)
    int64_t base;    // the copy's stat when the account was read
    int64_t pending; // counted since the last commit, on every copy of the index
    int32_t base_copy; // the owned entry `base` is from (a copy's id, 0 a stack): a plain-index slot's counts go there
} ServerSkinCounter;

// WP-E part 2c: one challenges.update tally (shared/include/progression.h)
typedef struct {
    char key[24];              // v7 keys, or a map's own ("Give Challenge Progress" triggers)
    int16_t weapon;            // -1 none
    int16_t streak;            // -1 none
    int32_t item;              // -1 none: the spray ("spry") / found skin ("find")
    int value;                 // ADD keys: the sum; MAX / SET keys: the highest / last value
} ServerChallengeTally;

typedef struct {
    int account_id;            // 0 guest
    char username[32];
    int level;                 // 0 guest
    char clan[8];
    int flag;                  // stats.flg: the Country Flag index (client account_flags.h), -1 none (the Strike kill card)
    int ranked_rank;           // account ranked_rank (v10 setData, deob-10.0.7.js:115141): the running season's tier, 0 Unranked
    // the account's titles (kn_brain's account "titles": player_titles ids, v10's account.titles): the worn titleIndex is
    // sent only when it is one of them (NET_EXT_KN_PLAYER_TITLE)
#define SERVER_TITLE_MAX 512
    uint8_t titles_owned[SERVER_TITLE_MAX / 8];
    unsigned char flags;       // NET_PLAYER_INFO_*
    unsigned char hack:1;      // account.hack: no rewards / stats (game.js)
    unsigned char premium:1;
    unsigned char chat_banned:1; // account.chat_banned (kn_brain feature_bans 'chat'): no chat lines, the forfeit vote still works
    int funds;
    char guest_id[33];         // NET_EXT_KN_GUEST_ID: the menu's guest id (32 lowercase hex), "" none; its unclaimed rewards

    unsigned char join_state;  // ServerJoinState
    unsigned char spawn_deferred:1;    // an ENT arrived before the identity was settled
    unsigned char spawn_spectate:1;
    float join_deadline;       // g_server->now
    uint32_t auth_rid;
    char ticket_jti[40];       // seek-game ticket id (player-join releases its reservation)
    unsigned char resumed:1;   // the JOIN resumed an expired ticket this lobby admitted before (acc_resume_allowed)

    float match_since;         // g_server->now of the first spawn in this round, 0 = not playing
    int round_result;          // END result of this round: 1 won, 0 lost, -1 draw / none
    // v10.0.7 endGame's skin drops of this round, decided with the END (accounts_skin_reward) and granted with its stats
    // commit: the map's mapReward skin and the event's reward skin, 0 = none
    int end_map_skin, end_event_skin;
    unsigned char committed:1; // this round's stats were already sent

    // dedicated server (doc/backend.md 8.7-8.9)
    unsigned char central_pending:1;   // online_mode: waiting for central hasJoined
    unsigned char central_verified:1;  // online_mode: identity confirmed by central auth
    unsigned char auth_session:1;      // the pending brain auth is a session token (a refusal = guest)
    unsigned char skins_known:1;       // skins_owned holds the coordinator's inventory
    unsigned char password_ok:1;       // a JOIN carried the right server password
    uint8_t skins_owned[1024];         // bitset of owned store.skins indices (8192: v10.0.2 has 8069), enforced on ENT
    // getSkins' other store items (deob-10.0.7.js:217781-217789, accounts_load_owner): the account's Twitch (tw_twitchname,
    // item.streamers) and v10's isSkinMaker (players.player_skinmaker); the name is `username`, the officer flag MOD
    char twitch[32];
    unsigned char skin_maker:1;
    // the paint of the account's painted copies, one per store index (the first copy of that index in the owned
    // list: the official plain-index rule, doc/v10/paint-copy.md section 6). Sent with the owner's SKINS (V2).
    uint16_t skin_paint_count;
    ServerSkinPaint skin_paints[SERVER_SKIN_PAINT_MAX];
    // the account's counter skins (accounts_load_skins), counted by the game's skin_stat hook
    uint16_t skin_counter_count;
    ServerSkinCounter skin_counters[SERVER_SKIN_COUNTER_MAX];
    // the seed of the account's copies, one per store index (the first copy of that index with an id or a meta seed:
    // skin_copy_seed), for the seeded custom shader 15 (deob-10.0.4.js:24344-24353). Sent in NET_EXT_KN_SKIN_COPIES.
    uint16_t skin_seed_count;
    struct {
        int32_t skin, seed;
    } skin_seeds[SERVER_SKIN_PAINT_MAX];
    // v10.0.7 item styles: the style of the account's styled copies, one per store index (the first copy of that index
    // with m.style > 0, the plain-index rule as the paint). Sent before the owner's SPAWN (NET_EXT_KN_SKIN_STYLES).
    uint16_t skin_style_count;
    struct {
        int32_t skin, style;
    } skin_styles[SERVER_SKIN_STYLE_MAX];
    // every owned copy with an id (accounts_load_skins), for the slots that name one (accounts_worn_copy)
    uint16_t skin_copy_count;
    ServerSkinCopy skin_copies[SERVER_SKIN_COPY_MAX];
    // v10.0.7 item copies: the wear, mint serial and name tag of the account's copies, one per store index (the first
    // copy of that index with any of them, the plain-index rule as the paint). Sent with the owner's SKINS
    // (NET_EXT_KN_SKIN_META, accounts_skin_meta_send).
    uint16_t skin_meta_count;
    struct {
        int32_t skin, serial;
        float wear; // -1 none
        char name[SERVER_SKIN_META_NAME];
    } skin_metas[SERVER_SKIN_META_MAX];
    int badge;                        // badgeIndex (v7 account stats.bdg, badges.js), -1 none
    // player-viewer WP-E part 2a: the death screen's account values (players.js:4860), read from the
    // brain account at auth: stats["c" + classIndex] and every config.playerCardStats value
    unsigned char has_stats:1;
    int class_stats[SERVER_CLASS_STATS_MAX];
    char pc_stats[PLAYER_CARD_STAT_COUNT][16];
    // player-viewer WP-E part 2c: this round's game.incStat counts (progression.h stat keys) and
    // challenges.update tallies per (key, weapon, streak), committed with the stats and then cleared
    uint32_t stat_counts[PROGRESSION_STAT_COUNT];
    ServerChallengeTally tallies[SERVER_TALLY_MAX];
    int tally_count;
    // v10.0.7's mid-match CHALLENGE_COMPLETED (deob-10.0.7.js:180731-180732): the account's activeChallenges as kn_brain
    // read them at the join and at each round start (cJSON array, owned, NULL none), each one's progVal after the last
    // event (chal_pv, PV at the read) and the ones already reported this round (chal_told bit i = challenge i)
    cJSON *challenges;
    uint32_t chal_rid;                 // the pending "challenges" read, 0 none
    uint64_t chal_told;
    double chal_pv[SERVER_CHALLENGES_MAX];
    // KrunkNative live sign-in (NET_EXT_KN_ACCOUNT_AUTH, accounts.c): one verification in flight,
    // a few per minute, nothing changes before it verifies
    uint32_t live_rid;                 // the pending brain / central request, 0 none
    float live_deadline;               // g_server->now: answered UNAVAILABLE after this
    uint8_t live_seq;                  // the AUTH's seq, echoed by the answer
    uint8_t live_kind;                 // NET_ACCOUNT_AUTH_* of the pending one
    char live_username[32];            // hasJoined: the claimed name (the reply decides who it is)
    float live_window_at;              // start of the current rate-limit window
    int live_attempts;                 // verifications started in that window
} ServerAccount;

struct ServerTransport;

typedef struct {
    int fd;
    struct sockaddr_in addr;
    const struct ServerTransport *transport; // the frontend that accepted it (set before net_add_client)
    NetPacketBuffer read_buffer;
    int id;
    int player_number;
    int class_index;
    int secondary_index;           // ENT secondary pick (weapon id, -1 default)
    int team_pick;                 // NET_EXT_KN_SWAP_TEAM before the first spawn (the menu's #teamSelector): its team, 0 none
    PlayerCosmetics cosmetics;     // ENT skin colour / cosmetic items (none for older ENTs)
    int slid_cont;                 // ENT slidCont (gameserver.ts ent[15]: player.slidCont), 1 for older ENTs
    int spray_index;               // ENT sprayIndex (gameserver.ts ent[1]: player.sprayIndex), 0 for older ENTs
    PlayerSkins skins;             // ENT weapon skins + meleeIndex (ent[2], ent[9]), owned ones only once enforced
    float lag_comp;                // ENT lagComp (ent[11] slider) clamped to 0.1..1, 1 for older ENTs
    int attach_index;              // WP-C ENT V8 attachIndex (ent[12]), DEFAULT_ATTACHMENT_INDEX for older ENTs
    unsigned char chall_mode:1;    // WP-C ENT V8 challengeMode (ent[7])
    unsigned char anon_mode:1;     // WP-C ENT V8 anonMode (ent[19])
    PlayerCosmeticsV7 cosmetics_v7; // WP-F ENT V9: the v7.1.4 cosmetic slots (none for older ENTs; badge unused)
    unsigned char show_badges;     // WP-F ENT V9 showBadges (PLAYER_SHOW_BADGE_*), all for older ENTs
    int streak_indexes[3];         // WP-B: STREAK_SLOTS (v7 ENT streakIndexes), applied at spawn
    unsigned char streak_indexes_set:1; // 0: the default pick (Nuke only)
    int pc_stat_index;             // WP-E: NET_PACKET_PC_STAT (v7 ENT pcStatIndex), -1 none
    // v10.0.7 "<copyId>#<index>" slots: NET_EXT_KN_SKIN_COPY_IDS before the ENT (ent_copies, zero for an older client), worn
    // from the spawn on like the skins (worn_copies, accounts_worn_copy)
    NetSkinCopyIdsPacket ent_copies;
    NetSkinCopyIdsPacket worn_copies;
    // v10.0.7 ENTER_GAME [34] titleIndex: NET_EXT_KN_LOOK before the ENT (ent_title, -1 for an older client), worn from
    // the spawn on (title_index) and sent as ADD_PLAYERS' [52] when the account owns it (NET_EXT_KN_PLAYER_TITLE)
    int ent_title;
    int title_index;
    char name[SERVER_PLAYER_NAME_MAX];
    char address[CHAT_CMD_ADDRESS_SIZE]; // dotted peer address (chat /ip, /banip)
    unsigned char op:1;            // CLI `op`: passes the DEV checks of chat commands while perms are on
    unsigned char op_account:1;    // op only for its account (config gameserver.ops): goes with the account
    // On the server's own machine: a loopback TCP peer that is not net-bridge (or a bridged
    // browser whose forwarded address is loopback too). Such a player hosts the server: DEV.
    unsigned char local:1;
    unsigned char first_packet_seen:1; // NET_PACKET_FORWARDED counts only as the first packet
    unsigned char peer_minor;      // HELLO minor of a NET_PROTOCOL_MAJOR peer (0 before / other majors): per-peer packet gates
    unsigned char peer_patch;      // WP-C: its HELLO patch (0 for a 2-byte HELLO): net_peer_has_wpc(peer_minor, peer_patch)
    unsigned char peer_flavor;     // its HELLO flavor nibble (GameFlavor; FLAVOR_BASE for a base or absent nibble)
    unsigned char join_spawn_due;  // the joiner's dead SPAWN (net_send_join_spawn) goes out with its HELLO: its size follows the peer's version

    float connected_at;
    float spawned_at;
    float last_significant_move_at; // the idle clock: connect, (re)spawn, map change, any activity (net_note_activity)
    vec3 idle_position;
    float idle_look[2];             // the look (x_dir, y_dir) at the last activity: turning away from it is activity
    int idle_shots;                 // the player's shots (life_shots + shot_count) at the last idle check
    float idle_damage_dealt;        // its damage_dealt at the last idle check
    float idle_health;              // its health at the last idle check: a drop is damage taken
    unsigned char has_idle_position:1;
    unsigned char has_idle_look:1;
    unsigned char joined:1;        // spawned in the current round (respawned by the next round)
    unsigned char announced:1;     // "<name> joined the game" was broadcast for this connection
    unsigned char voice_player:1;  // v7 findSocketPlayer for "vc": set by every ENT (spectators too), kept
                                   // across deaths, round ends and map changes
    unsigned char recorder:1;      // the K.P.D demo recorder's hidden spectator (server/src/demo_rec.c): never
                                   // counted, listed, announced, pinged or kicked

    NetChatLimiter chat_limiter;   // io_ct burst limit
    uint32_t ping_seq;             // seq of the outstanding PING, 0 none
    double ping_sent_at;           // net_time() of that PING
    double next_ping_at;           // net_time() of the next PING
    int ping_ms;                   // smoothed RTT, -1 unknown
    double next_recon_at;          // net_time() of the next periodic RECON ("l", NET_RECON_SEND_HZ)
    SocialAssembly social;         // social hub request being reassembled (shared/include/social.h)
    SocialAssembly voice;          // voice clip being reassembled (shared/include/voice.h, io_vc)
    SocialAssembly voice_v7;       // WP-E: a NET_PACKET_VOICE_V7 clip being reassembled (v7 "vc" with channel)

    Player *player;
    ServerAccount acct;            // backend account + match stats (server/src/accounts.c)
    // WP-C: v7 player.custVals are never reset, but KN frees the players at every map change: the
    // connection keeps them across rounds (net_free_players / net_spawn_client)
    TriggerValueSet custom_values;
    // KrunkNative Object Manager: the extra loadout weapons, kept across map changes like custVals
    int om_extras[PLAYER_EXTRA_WEAPONS_MAX];
    int om_extra_count;
    // KrunkNative: the player state that outlives the Player (PlayerKeep: cheats set from the menu,
    // a menu `tp`, the last life's view), applied at each spawn (net_spawn_client)
    PlayerKeep keep;
    // Projectile slots (GameProjectile.id) whose PROJECTILE the live-world filter held back while
    // this connection was dead or in the menu; resent from the game state when it spawns
    // (net.c net_resync_live_world)
    uint64_t missed_projectiles[(GAME_MAX_PROJECTILES + 63) / 64]; // bit (slot & 63) of word slot / 64
} ClientConnection;

typedef struct {
    volatile int should_quit;

    size_t client_count;
    ClientConnection *clients;
    int next_client_id;
    unsigned int used_player_numbers;
    float now;
    double tick_wall;              // CLOCK_MONOTONIC seconds when the running/last game_tick started
    pthread_mutex_t lock;

    ServerConfig config;
    Game game;
} Server;

extern Server *g_server;

// A connection's byte transport: TCP (server/src/net_tcp.c), UDP (net_udp.c) or the
// in-process sandbox (sandbox.c). Every hook runs with g_server->lock held unless noted.
typedef struct ServerTransport {
    const char *name;
    // Read what arrived for the connection and hand each whole packet to net_handle_packet().
    // Returns 0 once the connection was removed (net_handle_packet returned 0, or the peer
    // is gone and the transport called net_remove_client), else 1.
    int (*poll)(ClientConnection *client);
    // Send one tick's whole frames ([type, length hi, length lo, payload]...) to fd.
    void (*send_frames)(int fd, const uint8_t *frames, size_t length);
    void (*close)(int fd);
    // server_time() when the data carrying the latest PONG / INPUT of fd arrived, 0 unknown.
    double (*pong_received_at)(int fd);
    double (*input_received_at)(int fd);
    // Send whatever is queued for every peer now (end of tick), NULL when sends are immediate.
    void (*flush)(void);
} ServerTransport;

// Transports whose flush runs at the end of every tick (net_broadcast_states).
void server_transport_register(const ServerTransport *transport);
void server_transport_unregister(const ServerTransport *transport);
void server_transport_flush_all(void);

// The server's wall clock (seconds): CLOCK_MONOTONIC, or the sandbox's own clock
// (server_set_clock) so an in-process server follows the client's frame time.
double server_time(void);
void server_set_clock(double (*clock)(void)); // NULL = CLOCK_MONOTONIC

// Why the server stops, for the players' disconnect message (doc/backend.md 8.5): console
// `quit <reason>` / `drain <reason>` set it; otherwise gameserver.shutdown_reason_file (a
// deploy writes "update" there before restarting the units) is read once at the first stop
// request. "update" -> ERROR "GAME UPDATING" (v7.1.4 UPDATING_SHUTDOWN_MESSAGE) code 004004,
// "restart" -> "Server restarting", other text -> "Server shutting down: <text>", none ->
// "Server shutting down"; all but "update" use code 004006 (NET_ERROR_CODE_SHUTDOWN).
void server_set_shutdown_reason(const char *reason);
const char *server_shutdown_reason(void); // "" = none (reads the reason file the first time)
// The disconnect every connected player gets on shutdown (and its drain chat notice).
void server_shutdown_message(char *message, size_t message_size, char *code, size_t code_size);
const char *server_shutdown_notice(void);

// Krunker's game hooks (GameHooks.send / next_round / respawn / reward / alloc_uid) on
// g_server->game. Caller holds g_server->lock.
void net_install_game_hooks(void);
// Every connection's pending packets through net_handle_packet, then the kicks the chat
// commands queued. Caller holds g_server->lock (server_bin's net thread, the sandbox step).
void net_poll_clients(void);
int net_handle_packet(ClientConnection*, const NetPacket*); // 0 = the client was removed
void net_add_client(ClientConnection*); // takes a copy; caller holds g_server->lock
// The K.P.D demo recorder (server/src/demo_rec.c): a hidden spectator connection on `transport` / fd (no
// player, never counted, listed, pinged or kicked), sent its join burst at once. Returns its id, -1 when the
// game is not ready. net_send_demo_keyframe sends it the join burst again (MAP_INFO, INIT and what follows,
// the spectate ack, every player, the round state): the recorder captures that as a keyframe.
int net_add_recorder(const struct ServerTransport *transport, int fd);
void net_send_demo_keyframe(int recorder_id);

// One server tick (main.c's loop body, the sandbox step): g_server->now / tick_wall, bot
// slots, game_tick, accounts_tick, idle kicks, the per-tick broadcast. Caller holds the lock.
void server_core_tick(float now, float delta, double tick_wall);

void net_broadcast_states();
void net_check_idle_clients(float);
void net_remove_client(ClientConnection*); // disconnect + free; caller holds g_server->lock

// Switch every client to another map/mode (the current players respawn on it).
// Caller holds g_server->lock.
#define SERVER_HAVE_NET_CHANGE_MAP 1
void net_change_map(int map_index, int mode_index);

// Server chat line to every client (CLI `say`): a NET_CHAT_SERVER message.
// Caller holds g_server->lock.
#define SERVER_HAVE_NET_CHAT 1
void net_broadcast_chat(const char *from, const char *msg);
// A server line about player `uid` (its real name `real`; "$" in `template` = the name, each recipient's view of it):
// the form each peer reads (NET_EXT_KN_SERVER_LINE or the composed NET_CHAT_SERVER line). net.c
void net_broadcast_player_line(int uid, const char *real, const char *template);
// A server line about player `uid` (name `name`, "$" = name) to one connection in the form it reads (net_ext.h
// NET_EXT_KN_SERVER_LINE, or the composed line): the MOTD's line about the player who joined.
void net_server_line_to(ClientConnection *to, int uid, const char *name, const char *template);
// the brain's "ranked-notice" line: the queued player's name apart from the template ("$" = name): NET_EXT_KN_RANKED_NOTICE to an
// EXT peer, the composed NET_CHAT_RANKED line to an older one
void net_broadcast_ranked_notice(const char *name, const char *template);

// v10's game announcement banner (NET_EXT_GAME_ANNOUNCEMENT, shared/include/net_ext.h): the CLI's
// `announce` sets the server's message ("" clears it) and sends it to every client; each client
// whose join settles gets the current one (accounts.c). Caller holds g_server->lock.
#define SERVER_HAVE_NET_ANNOUNCE 1
void net_set_game_announcement(const char *msg);
const char *net_game_announcement(void);
void net_send_game_announcement(ClientConnection *client);
void net_set_game_ranked_queue(const char *label); // `announce ranked-queue <label>` (NULL / "" clears)

// Krunker v3.1.1 chat "/" commands (shared/src/chat_commands.c) wired to the connections;
// the CLI runs them as CHAT_CMD_CONSOLE. Caller holds g_server->lock.
#define SERVER_HAVE_CHAT_COMMANDS 1
ChatCommandHost *net_chat_host(void);

void on_client_connect(ClientConnection*);
void on_client_disconnect(ClientConnection*);
void on_client_event(ClientConnection*);
