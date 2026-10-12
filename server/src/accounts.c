// Account-aware game behaviour + the game side of the brain protocol
// (server/include/server_accounts.h, doc/backend.md section 8).
#include <server_accounts.h>
#include <server_log.h>
#include <brain.h>
#include <build_info.h>
#include <central_auth.h>
#include <rcon.h>
#include <auth_join.h>
#include <map_catalog.h>
#include <map_validate.h>
#include <game_config.h> // WP-E: v7 host types, setupGameConfig / applyConfig per round
#include <ranked.h>      // RANKED_TEAM_SIZE
#include <ks_storage.h>    // WP6: GAME.STORAGE / KR reward replies ("acct-r")
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define ACC_SNAPSHOT_INTERVAL 5.0f   // brainClient.ts SNAPSHOT_INTERVAL_MS
#define ACC_PUSH_DEBOUNCE 0.25f      // DEBOUNCE_PUSH_MS
#define ACC_HOST_IDLE_REVERT 60.0f   // an empty hosted lobby goes back to the public rotation
#define ACC_VERSION "1.0.0"

static struct {
    int brain;                 // brain configured (brain_start succeeded)
    char lobby_id[8];          // local lobby id (tuple slot 0), the brain namespaces it "<node>:<id>"
    char match_id[48];         // this round's id for idempotent stats commits
    unsigned int round_counter;
    unsigned int billboard_cycle;
    int round_end_seen;        // an END went out this round
    int round_committed;

    float next_snapshot;
    float push_at;             // 0 = no lobby-up pending
    int draining;
    int drain_down_sent;

    // hosted custom game (brain "host")
    int custom;
    float custom_since;
    int host_account_id;
    char host_name[32];
    int custom_had_players;
    cJSON *pending_host;       // "host" waiting for its published map (/api/map-data)
    uint32_t pending_rid;
    cJSON *hosted_map;         // the fetched published map of the current custom game (owned)
    char published_map[128];   // WP6: config.customMap of the current custom game ("" none)
    int kr_rewards;            // WP6: host option krRewards on that published map

    // bans file
    int ban_accounts[256];
    int ban_account_count;
    char ban_names[64][32];
    int ban_name_count;
    unsigned long ip_ban_hash;
    int bans_dirty;            // account bans changed (/ban, /unban)
    float next_ban_check;

    const cJSON *cli_map;      // positional map.json of the command line (kept for reverts)

    // dedicated server (doc/backend.md 8.7-8.9)
    char server_nonce[AUTH_JOIN_NONCE_LEN + 1]; // per boot, AUTH_CHALLENGE / serverHash
    uint32_t next_central_rid;
} g_acc;

// Ranked lobby state (brain queue, see "ranked" below).
#define ACC_RANKED_MAX 8

typedef struct {
    int account_id;
    char name[32];
    int team;                  // 0 / 1 (brain team index)
    unsigned char joined:1;    // connected and settled at least once
    unsigned char left:1;      // left (or never came) before the end: abandoned
    int kills, deaths, score;  // snapshot when leaving
} RankedSlot;

static struct {
    int active;
    char match_id[64];
    char queue[16];
    int team_size;
    RankedSlot slots[ACC_RANKED_MAX];
    int count;
    float created_at;
    float grace;
    int started;               // everyone in (or the grace ran out with both teams present)
    int end_now;               // needAll: end the round on the next tick
    int ended;                 // END went out: result sent
    float ended_at;
    uint32_t result_rid;
} g_rk;

// ------------------------------------------------------------------ helpers

// true, a non-zero number or "1"/"true" (svc-accounts sends 0/1 flags)
static int acc_truthy(const cJSON *item) {
    if (cJSON_IsBool(item)) return cJSON_IsTrue(item);
    if (cJSON_IsNumber(item)) return item->valuedouble != 0.0;
    if (cJSON_IsString(item)) return !strcmp(item->valuestring, "1") || !strcasecmp(item->valuestring, "true");
    return 0;
}

static const char *acc_str(const cJSON *item, const char *def) {
    return cJSON_IsString(item) ? item->valuestring : def;
}

static ClientConnection *acc_client_by_id(const int id) {
    for (size_t i = 0; i < g_server->client_count; i++) {
        if (g_server->clients[i].id == id) return &g_server->clients[i];
    }
    return NULL;
}

static ClientConnection *acc_client_by_rid(const uint32_t rid) {
    if (!rid) return NULL;
    for (size_t i = 0; i < g_server->client_count; i++) {
        if (g_server->clients[i].acct.auth_rid == rid) return &g_server->clients[i];
    }
    return NULL;
}

static void acc_chat_to(ClientConnection *client, const char *text) {
    NetChatPacket packet;
    net_packet_chat(&packet, -1, NET_CHAT_SERVER, NULL, text);
    net_send_packet(client->fd, NET_PACKET_CHAT, &packet, sizeof(packet));
}

void accounts_notice(ClientConnection *client, const char *text) {
    if (client && text && text[0]) acc_chat_to(client, text);
}

static const char *acc_map_name(void) {
    const Game *game = &g_server->game;
    const int index = game->current_map_index;
    const cJSON *map = index >= 0 && (size_t) index < game->map_count ? game->maps[index] : NULL;
    const cJSON *name = cJSON_GetObjectItem(map, "name");
    return cJSON_IsString(name) ? name->valuestring : "Unknown";
}

static int acc_player_count(void) {
    int count = 0;
    for (size_t i = 0; i < g_server->client_count; i++) {
        if (g_server->clients[i].acct.join_state == SERVER_JOIN_READY) count++;
    }
    return count;
}

static void acc_new_match_id(void) {
    char rnd[20];
    brain_random_hex(rnd, sizeof(rnd), 6);
    snprintf(g_acc.match_id, sizeof(g_acc.match_id), "%s:%s:%u:%s", g_gs_config.node_id, g_acc.lobby_id, ++g_acc.round_counter, rnd);
}

// A map of this server's own list (public rotation, custom dirs, command line map) by name.
static const cJSON *acc_public_map_by_name(const char *name) {
    for (size_t i = 0; i < g_server->game.map_count; i++) {
        const cJSON *map_name = cJSON_GetObjectItem(g_server->game.maps[i], "name");
        if (cJSON_IsString(map_name) && !strcasecmp(map_name->valuestring, name)) return g_server->game.maps[i];
    }
    return NULL;
}

const char *accounts_join_message(void) { return g_gs_config.join_message; }
const char *accounts_leave_message(void) { return g_gs_config.leave_message; }

// ------------------------------------------------------------------ lobby state (brain)

static cJSON *acc_lobby_tuple(void) {
    const Game *game = &g_server->game;
    cJSON *tuple = cJSON_CreateArray();
    cJSON_AddItemToArray(tuple, cJSON_CreateString(g_acc.lobby_id));
    cJSON_AddItemToArray(tuple, cJSON_CreateString(g_gs_config.region));
    cJSON_AddItemToArray(tuple, cJSON_CreateNumber(acc_player_count()));
    cJSON_AddItemToArray(tuple, cJSON_CreateNumber(g_server->config.max_players));

    cJSON *meta = cJSON_CreateObject();
    const char *map = acc_map_name();
    const int mode_id = game_mode_id(game);
    const int catalogue = game->current_map_index >= 0 && (size_t) game->current_map_index < game->map_count
                              ? map_catalog_index_of_json(game->maps[game->current_map_index]) : -1;
    const int custom_map = catalogue < 0 || catalogue >= MAP_CATALOG_OFFICIAL_COUNT;

    if (g_acc.custom) {
        char label[96];
        const GameModeConfig *mode = mode_config(mode_id);
        snprintf(label, sizeof(label), "%s_%s", mode ? mode->alias : "ffa", map);
        cJSON_AddStringToObject(meta, "i", label);
    } else {
        cJSON_AddStringToObject(meta, "i", map);
    }
    cJSON_AddStringToObject(meta, "v", ACC_VERSION);
    // build identity (build_info.h): the brain flags outdated lobbies and matches protocols
    // (services/brain/release_info.c); federated with the tuple like every meta field
    cJSON_AddStringToObject(meta, "proto", kn_build_protocol());
    cJSON_AddStringToObject(meta, "build", kn_build_info()->describe);
    cJSON_AddStringToObject(meta, "commit", kn_build_info()->commit);
    cJSON_AddNumberToObject(meta, "commitTime", (double) kn_build_info()->commit_time);
    cJSON_AddBoolToObject(meta, "cs", g_acc.custom);
    if (custom_map) cJSON_AddStringToObject(meta, "m", map);
    else cJSON_AddNullToObject(meta, "m");
    cJSON_AddBoolToObject(meta, "pr", g_gs_config.password[0] != 0); // password protected (never quick-joined)
    cJSON_AddBoolToObject(meta, "earnKR", g_gs_config.earn_kr && !g_acc.custom);
    cJSON_AddNumberToObject(meta, "mode", mode_id);
    cJSON_AddStringToObject(meta, "modeName", mode_name(mode_id));
    cJSON_AddStringToObject(meta, "map", map);
    cJSON_AddStringToObject(meta, "name", g_gs_config.name);
    cJSON_AddStringToObject(meta, "host", g_gs_config.public_host);
    cJSON_AddNumberToObject(meta, "port", g_gs_config.public_port ? g_gs_config.public_port : g_server->config.listen_port);
    if (g_gs_config.web_url[0]) cJSON_AddStringToObject(meta, "webUrl", g_gs_config.web_url);
    cJSON_AddNumberToObject(meta, "timeLeft", game->end_timer > 0.0f ? 0 : (int) ceilf(game->round_timer > 0.0f ? game->round_timer : 0.0f));
    cJSON_AddBoolToObject(meta, "draining", g_acc.draining);
    // WP-E: a hosted game's config.hostTypes index (comp / zombies / raid) and its privacy, for the
    // custom games list; the brain passes meta through, older brains and clients ignore them
    if (game->config.host_type > 0) cJSON_AddNumberToObject(meta, "hostType", game->config.host_type);
    if (game->config.is_private) cJSON_AddBoolToObject(meta, "private", 1);
    if (g_rk.active) {
        cJSON_AddBoolToObject(meta, "ranked", 1);
        cJSON_AddStringToObject(meta, "matchId", g_rk.match_id);
    }
    cJSON_AddItemToArray(tuple, meta);
    return tuple;
}

static void acc_send_snapshot(void) {
    if (!brain_linked()) return;
    cJSON *frame = cJSON_CreateObject();
    cJSON_AddStringToObject(frame, "type", "snapshot");
    cJSON *lobbies = cJSON_AddArrayToObject(frame, "lobbies");
    if (!(g_acc.draining && g_acc.drain_down_sent)) cJSON_AddItemToArray(lobbies, acc_lobby_tuple());
    brain_post(frame, 0);
    g_acc.push_at = 0.0f;
}

static void acc_push_lobby_now(void) {
    if (!brain_linked()) return;
    cJSON *frame = cJSON_CreateObject();
    cJSON_AddStringToObject(frame, "type", "lobby-up");
    cJSON_AddItemToObject(frame, "lobby", acc_lobby_tuple());
    brain_post(frame, 0);
    g_acc.push_at = 0.0f;
}

// joins / leaves / map changes: one lobby-up 250 ms later (brainClient.ts _queueLobbyPush)
static void acc_queue_push(void) {
    if (g_acc.brain && !g_acc.push_at) g_acc.push_at = g_server->now + ACC_PUSH_DEBOUNCE;
}

static cJSON *acc_hello(void) {
    cJSON *hello = cJSON_CreateObject();
    cJSON_AddStringToObject(hello, "type", "hello");
    cJSON_AddStringToObject(hello, "nodeId", g_gs_config.node_id);
    cJSON_AddStringToObject(hello, "region", g_gs_config.region);
    cJSON_AddStringToObject(hello, "name", g_gs_config.name);
    cJSON_AddStringToObject(hello, "publicHost", g_gs_config.public_host);
    cJSON_AddNumberToObject(hello, "publicPort", g_gs_config.public_port ? g_gs_config.public_port : g_server->config.listen_port);
    if (g_gs_config.web_url[0]) cJSON_AddStringToObject(hello, "webUrl", g_gs_config.web_url);
    cJSON_AddStringToObject(hello, "version", ACC_VERSION);
    const KnBuildInfo *build = kn_build_info();
    cJSON_AddStringToObject(hello, "build", build->describe);
    cJSON_AddStringToObject(hello, "commit", build->commit);
    cJSON_AddNumberToObject(hello, "commitTime", (double) build->commit_time);
    cJSON_AddStringToObject(hello, "buildTime", build->build_time);
    cJSON_AddStringToObject(hello, "protocol", kn_build_protocol());
    cJSON_AddNumberToObject(hello, "maxPlayers", g_server->config.max_players);
    cJSON_AddBoolToObject(hello, "custom", g_gs_config.custom_games);
    cJSON_AddNumberToObject(hello, "maxLobbies", 1);
    // Matchmaking eligibility: the brain sends only signed-in players to online_mode nodes
    // and never quick-joins password protected ones (services/brain/registry.c).
    cJSON_AddNumberToObject(hello, "onlineMode", g_gs_config.online_mode ? 1 : 0);
    cJSON_AddBoolToObject(hello, "password", g_gs_config.password[0] != 0);
    cJSON *modes = cJSON_AddArrayToObject(hello, "modes");
    for (size_t i = 0; i < g_server->game.mode_count; i++) cJSON_AddItemToArray(modes, cJSON_CreateNumber(g_server->game.modes[i]));
    cJSON *maps = cJSON_AddArrayToObject(hello, "maps");
    for (size_t i = 0; i < g_server->game.map_count; i++) {
        const cJSON *name = cJSON_GetObjectItem(g_server->game.maps[i], "name");
        if (cJSON_IsString(name)) cJSON_AddItemToArray(maps, cJSON_CreateString(name->valuestring));
    }
    return hello;
}

// ------------------------------------------------------------------ bans file

static unsigned long acc_ip_ban_hash(void) {
    const ChatCommandHost *host = net_chat_host();
    unsigned long hash = 5381u + (unsigned long) host->ip_ban_count;
    for (int i = 0; i < host->ip_ban_count; i++) {
        for (const char *p = host->ip_bans[i]; *p; p++) hash = hash * 33u + (unsigned char) *p;
    }
    return hash;
}

static void acc_load_bans(void) {
    if (!g_gs_config.bans_file[0]) return;
    cJSON *json = NULL;
    if (load_json_file(g_gs_config.bans_file, &json)) {
        server_log("Bans file %s not found, starting with no bans", g_gs_config.bans_file);
        g_acc.ip_ban_hash = acc_ip_ban_hash();
        return;
    }

    ChatCommandHost *host = net_chat_host();
    const cJSON *entry;
    cJSON_ArrayForEach(entry, cJSON_GetObjectItem(json, "ips")) {
        if (!cJSON_IsString(entry) || chat_command_ip_banned(host, entry->valuestring) || host->ip_ban_count >= CHAT_CMD_IP_BANS_MAX) continue;
        snprintf(host->ip_bans[host->ip_ban_count++], CHAT_CMD_ADDRESS_SIZE, "%s", entry->valuestring);
    }
    cJSON_ArrayForEach(entry, cJSON_GetObjectItem(json, "accounts")) {
        if (cJSON_IsNumber(entry) && g_acc.ban_account_count < 256) g_acc.ban_accounts[g_acc.ban_account_count++] = entry->valueint;
    }
    cJSON_ArrayForEach(entry, cJSON_GetObjectItem(json, "names")) {
        if (cJSON_IsString(entry) && g_acc.ban_name_count < 64) snprintf(g_acc.ban_names[g_acc.ban_name_count++], 32, "%s", entry->valuestring);
    }
    cJSON_Delete(json);
    g_acc.ip_ban_hash = acc_ip_ban_hash();
    server_log("Bans: %d IP(s), %d account(s), %d name(s) from %s", host->ip_ban_count, g_acc.ban_account_count, g_acc.ban_name_count,
               g_gs_config.bans_file);
}

// /banip and /unbanip change the chat host's list: persist it.
static void acc_save_bans_if_changed(void) {
    if (!g_gs_config.bans_file[0]) return;
    const unsigned long hash = acc_ip_ban_hash();
    if (hash == g_acc.ip_ban_hash && !g_acc.bans_dirty) return;
    g_acc.ip_ban_hash = hash;
    g_acc.bans_dirty = 0;

    const ChatCommandHost *host = net_chat_host();
    cJSON *json = cJSON_CreateObject();
    cJSON *ips = cJSON_AddArrayToObject(json, "ips");
    for (int i = 0; i < host->ip_ban_count; i++) cJSON_AddItemToArray(ips, cJSON_CreateString(host->ip_bans[i]));
    cJSON *accounts = cJSON_AddArrayToObject(json, "accounts");
    for (int i = 0; i < g_acc.ban_account_count; i++) cJSON_AddItemToArray(accounts, cJSON_CreateNumber(g_acc.ban_accounts[i]));
    cJSON *names = cJSON_AddArrayToObject(json, "names");
    for (int i = 0; i < g_acc.ban_name_count; i++) cJSON_AddItemToArray(names, cJSON_CreateString(g_acc.ban_names[i]));

    char *text = cJSON_Print(json);
    cJSON_Delete(json);
    if (!text) return;
    char tmp[GS_STR_LARGE + 8];
    snprintf(tmp, sizeof(tmp), "%s.tmp", g_gs_config.bans_file);
    FILE *f = fopen(tmp, "wb");
    if (f && fputs(text, f) >= 0 && fclose(f) == 0) {
        if (rename(tmp, g_gs_config.bans_file)) server_log_error("Cannot write bans file %s", g_gs_config.bans_file);
        else server_log("Saved %d IP ban(s) to %s", host->ip_ban_count, g_gs_config.bans_file);
    } else {
        if (f) fclose(f);
        server_log_error("Cannot write bans file %s", tmp);
    }
    free(text);
}

// /ban /unban /bans on accounts (chat command host callbacks, net.c).
int accounts_ban_account(const int account_id, const char *name) {
    if (account_id <= 0) return 0;
    for (int i = 0; i < g_acc.ban_account_count; i++) {
        if (g_acc.ban_accounts[i] == account_id) return 1;
    }
    if (g_acc.ban_account_count >= 256) return 0;
    g_acc.ban_accounts[g_acc.ban_account_count++] = account_id;
    g_acc.bans_dirty = 1;
    server_log("Banned account %d (%s)%s", account_id, name ? name : "?", g_gs_config.bans_file[0] ? "" : " until restart (no gameserver.bans_file)");
    return 1;
}

int accounts_unban_account(const int account_id) {
    for (int i = 0; i < g_acc.ban_account_count; i++) {
        if (g_acc.ban_accounts[i] != account_id) continue;
        g_acc.ban_accounts[i] = g_acc.ban_accounts[--g_acc.ban_account_count];
        g_acc.bans_dirty = 1;
        return 1;
    }
    return 0;
}

int accounts_list_account_bans(int *out, const int max) {
    for (int i = 0; i < g_acc.ban_account_count && i < max; i++) out[i] = g_acc.ban_accounts[i];
    return g_acc.ban_account_count; // the total (chat_commands.h)
}

static int acc_account_banned(const int account_id, const char *username) {
    for (int i = 0; i < g_acc.ban_account_count; i++) {
        if (account_id > 0 && g_acc.ban_accounts[i] == account_id) return 1;
    }
    for (int i = 0; i < g_acc.ban_name_count; i++) {
        if (username && username[0] && !strcasecmp(g_acc.ban_names[i], username)) return 1;
    }
    return 0;
}

// ------------------------------------------------------------------ public rotation

void accounts_configure_public(Game *game, const cJSON *cli_map) {
    g_acc.cli_map = cli_map;
    size_t map_count = 0;
    const cJSON **maps = cli_map ? map_list_with_custom(cli_map, &map_count) : gs_build_map_list(&g_gs_config, &map_count);
    size_t mode_count = 0;
    int *modes = gs_build_mode_list(&g_gs_config, &mode_count);
    game_configure(game, g_gs_config.has_game ? &g_gs_config.game : NULL, maps, map_count, modes, mode_count, NULL, 0, NULL, 0);
    // WP-E server presets (gameserver.host_type / map_config): the rotation plays as a hosted game
    if (g_gs_config.host_type >= 0 || g_gs_config.map_config) {
        cJSON *request = g_gs_config.game_request[0] ? cJSON_Parse(g_gs_config.game_request) : NULL;
        game_config_host(game, request, g_gs_config.host_type >= 0 ? g_gs_config.host_type : GAME_HOST_CUSTOM);
        cJSON_Delete(request);
    }
}

// ------------------------------------------------------------------ PLAYER_INFO

static void acc_fill_info(const ClientConnection *client, NetPlayerInfoPacket *info, const ClientConnection *to) {
    memset(info, 0, sizeof(*info));
    info->uid = client->player ? client->player->uid : client->id;
    // WP-C: v7.1.4 players.js sync with anonMode: a random level, no clan, account or badges for
    // everyone but the player and dev viewers
    if (client->player && client->player->anon_mode && to != client && !(to && (to->acct.flags & NET_PLAYER_INFO_DEV))) {
        info->level = 1 + rand() % 100; // fakeLevel || UTILS.randInt(1, 100)
        return;
    }
    info->account_id = client->acct.account_id;
    info->level = client->acct.level;
    info->flags = client->acct.flags;
    // WP-F: ENT showBadges (settings verifiedBadge / premiumBadge): the player may hide those badges
    // from others (players.js sync showBadges.verified / .premium)
    if (client->player && to != client) {
        if (!(client->player->show_badges & PLAYER_SHOW_BADGE_VERIFIED)) info->flags &= (uint8_t) ~NET_PLAYER_INFO_VERIFIED;
        if (!(client->player->show_badges & PLAYER_SHOW_BADGE_PREMIUM)) info->flags &= (uint8_t) ~NET_PLAYER_INFO_PREMIUM;
    }
    snprintf(info->clan, sizeof(info->clan), "%s", client->acct.clan);
}

static void acc_broadcast_info(const ClientConnection *client) {
    if (!g_acc.brain || !client->player) return;
    NetPlayerInfoPacket info;
    for (size_t i = 0; i < g_server->client_count; i++) {
        acc_fill_info(client, &info, &g_server->clients[i]);
        net_send_packet(g_server->clients[i].fd, NET_PACKET_PLAYER_INFO, &info, sizeof(info));
    }
}

void accounts_send_infos_to(ClientConnection *target) {
    if (!g_acc.brain) return;
    for (size_t i = 0; i < g_server->client_count; i++) {
        const ClientConnection *client = &g_server->clients[i];
        if (!client->player || client == target) continue;
        NetPlayerInfoPacket info;
        acc_fill_info(client, &info, target);
        net_send_packet(target->fd, NET_PACKET_PLAYER_INFO, &info, sizeof(info));
    }
}

// ------------------------------------------------------------------ billboard

void accounts_send_billboard(ClientConnection *client) {
    const GsBillboard *bb = gs_billboard_for_cycle(&g_gs_config, g_acc.billboard_cycle);
    if (!bb) return; // engine defaults, exactly as before (no packet)
    NetBillboardPacket packet;
    memset(&packet, 0, sizeof(packet));
    snprintf(packet.txt, sizeof(packet.txt), "%s", bb->txt);
    snprintf(packet.txt_col, sizeof(packet.txt_col), "%s", bb->txt_col);
    snprintf(packet.bg_col, sizeof(packet.bg_col), "%s", bb->bg_col);
    net_send_packet(client->fd, NET_PACKET_BILLBOARD, &packet, sizeof(packet));
}

// ------------------------------------------------------------------ stats commits

// game.js endGame: reward = min(premiumT ? 40 : 30, floor(score * rewardMlt / 100)) for
// accounts that are not hack-flagged, none in hosted (custom) games or noReward modes.
static int acc_reward(const ClientConnection *client, const Player *player) {
    const GameModeConfig *mode = mode_config(game_mode_id(&g_server->game));
    if (!g_gs_config.earn_kr || g_acc.custom || !client->acct.account_id || client->acct.hack) return 0;
    if (mode && mode->no_reward) return 0;
    const double mlt = (mode && mode->reward_mlt ? mode->reward_mlt : 1) * (double) g_gs_config.kr_multiplier;
    const int cap = client->acct.premium ? g_gs_config.kr_cap_premium : g_gs_config.kr_cap;
    const int reward = (int) floor((double) player->score * mlt / 100.0);
    return reward < 0 ? 0 : reward > cap ? cap : reward;
}

// GameHooks.reward (game_end_round): the END packets' reward column and the commit's "kr".
int accounts_reward(const Player *player) {
    const ClientConnection *client = player ? acc_client_by_id(player->uid) : NULL;
    return client ? acc_reward(client, player) : 0;
}

// ------------------------------------------------------------------ WP-E part 2c: progression

// GameHooks.progress: game.incStat / challenges.update of an account's player, tallied for the
// stats commit (doc/backend.md 8.4). Guests and hack-flagged accounts keep nothing.
void accounts_progress(const Player *player, const int stat, const char *key, const int value, const int weapon, const int streak) {
    ClientConnection *client = player ? acc_client_by_id(player->uid) : NULL;
    if (!client || client->acct.account_id <= 0 || client->acct.hack || !key || !key[0] || strlen(key) >= sizeof(client->acct.tallies[0].key)) return;
    ServerAccount *a = &client->acct;
    if (stat) {
        const int index = progression_stat_index(key);
        if (index >= 0 && a->stat_counts[index] < UINT32_MAX) a->stat_counts[index]++;
        return;
    }
    const ChallengeKeyKind kind = challenge_key_kind(key);
    if (kind == CHALLENGE_KEY_UNKNOWN || kind == CHALLENGE_KEY_META) return;
    ServerChallengeTally *t = NULL;
    for (int i = 0; i < a->tally_count; i++) {
        ServerChallengeTally *c = &a->tallies[i];
        if (c->weapon == weapon && c->streak == streak && !strcmp(c->key, key)) t = c;
    }
    if (!t) {
        if (a->tally_count >= SERVER_TALLY_MAX) return;
        t = &a->tallies[a->tally_count++];
        memset(t, 0, sizeof(*t));
        snprintf(t->key, sizeof(t->key), "%s", key);
        t->weapon = (int16_t) weapon;
        t->streak = (int16_t) streak;
        t->value = kind == CHALLENGE_KEY_ADD ? 0 : value;
    }
    if (kind == CHALLENGE_KEY_ADD) t->value += value;
    else if (kind == CHALLENGE_KEY_MAX || !strcmp(key, "kstk")) t->value = value > t->value ? value : t->value;
    else t->value = value; // SET
}

static void acc_progress_clear(ServerAccount *a) {
    memset(a->stat_counts, 0, sizeof(a->stat_counts));
    a->tally_count = 0;
}

// The commit's progression fields: "stats" (incStat deltas, transferStats), "prog" (the challenge
// tallies [key, value, weapon, streak], plus game.js endGame's "w" / "gp"), "medals" ({name:
// count}, player.medals) and the match context of checkChallenge (mapIndex, mapId, ranked).
static void acc_add_progress(cJSON *p, const ClientConnection *client, const Player *player, const int final, const int won) {
    const ServerAccount *a = &client->acct;
    cJSON *stats = cJSON_AddObjectToObject(p, "stats");
    for (int i = 0; i < PROGRESSION_STAT_COUNT; i++)
        if (a->stat_counts[i]) cJSON_AddNumberToObject(stats, g_progression_stats[i], a->stat_counts[i]);
    cJSON *prog = cJSON_AddArrayToObject(p, "prog");
    for (int i = 0; i < a->tally_count; i++) {
        const ServerChallengeTally *t = &a->tallies[i];
        cJSON *row = cJSON_CreateArray();
        cJSON_AddItemToArray(row, cJSON_CreateString(t->key));
        cJSON_AddItemToArray(row, cJSON_CreateNumber(t->value));
        cJSON_AddItemToArray(row, cJSON_CreateNumber(t->weapon));
        cJSON_AddItemToArray(row, cJSON_CreateNumber(t->streak));
        cJSON_AddItemToArray(prog, row);
    }
    if (final) { // game.js endGame: challenges.update(game, "w") for the winners, "gp" for everyone
        const char *keys[2] = {"w", "gp"};
        for (int i = won ? 0 : 1; i < 2; i++) {
            cJSON *row = cJSON_CreateArray();
            cJSON_AddItemToArray(row, cJSON_CreateString(keys[i]));
            cJSON_AddItemToArray(row, cJSON_CreateNumber(1));
            cJSON_AddItemToArray(row, cJSON_CreateNumber(-1));
            cJSON_AddItemToArray(row, cJSON_CreateNumber(-1));
            cJSON_AddItemToArray(prog, row);
        }
    }
    cJSON *medals = cJSON_AddObjectToObject(p, "medals");
    for (int i = 0; player && i < PLAYER_MEDAL_SLOTS && i < PROGRESSION_MEDAL_COUNT; i++)
        if (player->medal_counts[i]) cJSON_AddNumberToObject(medals, g_progression_medals[i], player->medal_counts[i]);
    const Game *game = &g_server->game;
    const int catalogue = game->current_map_index >= 0 && (size_t) game->current_map_index < game->map_count
                              ? map_catalog_index_of_json(game->maps[game->current_map_index]) : -1;
    cJSON_AddNumberToObject(p, "mapIndex", catalogue >= 0 && catalogue < MAP_CATALOG_OFFICIAL_COUNT ? catalogue : -1);
    cJSON_AddBoolToObject(p, "ranked", game->config.comp); // checkChallenge: T "ranked" needs game.isComp
}

static cJSON *acc_player_stats(const ClientConnection *client, const int final) {
    const Player *player = client->player;
    const GameModeConfig *mode = mode_config(game_mode_id(&g_server->game));
    // updateAccounts: hosted games and noReward modes only add time played
    const int counts = !g_acc.custom && !(mode && mode->no_reward) && !client->acct.hack;
    const float seconds = client->acct.match_since > 0.0f ? g_server->now - client->acct.match_since : 0.0f;
    const int won = final && client->acct.round_result == 1;

    cJSON *p = cJSON_CreateObject();
    cJSON_AddNumberToObject(p, "accountId", client->acct.account_id);
    cJSON_AddStringToObject(p, "name", client->name);
    cJSON_AddNumberToObject(p, "kills", counts && player ? player->kills : 0);
    cJSON_AddNumberToObject(p, "deaths", counts && player ? player->deaths : 0);
    cJSON_AddNumberToObject(p, "score", counts && player ? player->score : 0);
    cJSON_AddBoolToObject(p, "won", counts && won);
    cJSON_AddNumberToObject(p, "timePlayed", (double) (int64_t) (seconds * 1000.0f)); // ms (Krunker timePlayed)
    cJSON_AddNumberToObject(p, "games", counts && final ? 1 : 0);
    cJSON_AddNumberToObject(p, "kr", final && player ? player->reward : 0); // game_end_round -> accounts_reward
    cJSON_AddNumberToObject(p, "xp", counts && player && player->score > 0 ? player->score : 0);
    cJSON_AddNumberToObject(p, "classIndex", player ? player->class_index : client->class_index);
    if (counts) acc_add_progress(p, client, player, final, won); // WP-E part 2c (v7 updateAccounts / checkChallenges)
    return p;
}

// One commit frame for `clients` (final: round end, else a mid-round leave).
static void acc_commit(ClientConnection **clients, const int count, const int final, const char *match_id) {
    if (!g_acc.brain || !count) return;
    cJSON *frame = cJSON_CreateObject();
    const uint32_t rid = brain_next_rid();
    cJSON_AddStringToObject(frame, "type", "commit");
    cJSON_AddNumberToObject(frame, "rid", rid);
    cJSON_AddStringToObject(frame, "lobbyId", g_acc.lobby_id);
    cJSON_AddStringToObject(frame, "matchId", match_id);
    cJSON_AddBoolToObject(frame, "final", final);
    cJSON_AddStringToObject(frame, "map", acc_map_name());
    cJSON_AddNumberToObject(frame, "mode", game_mode_id(&g_server->game));
    cJSON_AddBoolToObject(frame, "custom", g_acc.custom);
    cJSON *players = cJSON_AddArrayToObject(frame, "players");
    for (int i = 0; i < count; i++) {
        cJSON_AddItemToArray(players, acc_player_stats(clients[i], final));
        acc_progress_clear(&clients[i]->acct);
        clients[i]->acct.committed = 1;
        server_log("[stats] %s %s (account %d): K %u D %u S %d%s", final ? "final" : "partial", clients[i]->name, clients[i]->acct.account_id,
                   clients[i]->player ? clients[i]->player->kills : 0, clients[i]->player ? clients[i]->player->deaths : 0,
                   clients[i]->player ? clients[i]->player->score : 0, final && clients[i]->acct.round_result == 1 ? " won" : "");
    }
    brain_post(frame, 1);
}

static int acc_should_commit(const ClientConnection *client) {
    return client->acct.account_id > 0 && client->acct.match_since > 0.0f && !client->acct.committed;
}

// Round end (first tick after the END packets): every account that played this round.
static void acc_commit_round(void) {
    ClientConnection *list[64];
    int count = 0;
    for (size_t i = 0; i < g_server->client_count && count < 64; i++) {
        ClientConnection *client = &g_server->clients[i];
        if (acc_should_commit(client) && client->player) list[count++] = client;
    }
    acc_commit(list, count, 1, g_acc.match_id);
    g_acc.round_committed = 1;
}

// A leaving player, or players still in a round that is abandoned (map change): partial.
static void acc_commit_partial(ClientConnection *client) {
    if (!acc_should_commit(client)) return;
    char id[64];
    snprintf(id, sizeof(id), "%s#c%d", g_acc.match_id, client->id);
    acc_commit(&client, 1, 0, id);
}

void accounts_on_end_packet(const int uid, const NetEndPacket *end) {
    ClientConnection *client = acc_client_by_id(uid);
    if (client) client->acct.round_result = end->result;
    g_acc.round_end_seen = 1;
}

void accounts_round_ending(void) {
    // a round abandoned before its END (CLI / chat map change): what was played so far
    if (!g_acc.round_committed) {
        for (size_t i = 0; i < g_server->client_count; i++) acc_commit_partial(&g_server->clients[i]);
    }
}

void accounts_round_changed(void) {
    Game *game = &g_server->game;
    const int mode_id = game_mode_id(game);
    game_network_apply(game, g_server->config.tick_rate); // WP-E: the round's networkData (applyNetworking, mode.sendR)

    // per-mode lobby size and round length (config, CLI --round-seconds)
    g_server->config.max_players = (unsigned short) (g_acc.custom && game->config.max_players > 0 ? game->config.max_players
                                                                                                : gs_lobby_size(&g_gs_config, mode_id));
    // game_init already started the clock with the previous round_length: correct it
    const float seconds = g_acc.custom ? 0.0f : gs_round_seconds(&g_gs_config, mode_id);
    const float used = game->round_length;
    game->round_length = seconds;
    if (game->round_timer > 0.0f && seconds > 0.0f) {
        game->round_timer = seconds;
    } else if (game->round_timer > 0.0f && used > 0.0f && game->mode) { // back to the mode's own game time
        const float mode_time = game->mode->config.game_time;
        const float minutes = (float) game->config.game_time;
        game->round_timer = mode_time == MODE_GAME_TIME_CONFIG ? (minutes > 0.0f ? minutes * 60.0f : 240.0f) : mode_time;
    }

    if (g_acc.round_counter) g_acc.billboard_cycle++; // pickBillboard at every map cycle
    acc_new_match_id();
    g_acc.round_end_seen = 0;
    g_acc.round_committed = 0;
    for (size_t i = 0; i < g_server->client_count; i++) {
        ServerAccount *a = &g_server->clients[i].acct;
        a->match_since = 0.0f;
        a->round_result = -1;
        a->committed = 0;
        acc_progress_clear(a); // WP-E part 2c: a new round's tallies
    }
    acc_queue_push();

    if (g_acc.draining && g_acc.round_counter > 1 && !g_acc.drain_down_sent && brain_linked()) {
        cJSON *down = cJSON_CreateObject();
        cJSON_AddStringToObject(down, "type", "lobby-down");
        cJSON_AddStringToObject(down, "lobbyId", g_acc.lobby_id);
        brain_post(down, 0);
        g_acc.drain_down_sent = 1;
        server_log("[brain] drained: round finished, lobby-down sent");
    }
}

// ------------------------------------------------------------------ ranked (brain queue)
// doc/backend.md 6.5. A v7.1.4 ranked match is a comp lobby (game.isComp, config.hostTypes[1]
// "Comp Server Setup": Hardpoint, the comp maps, rankedClasses, fixMov / jumpMlt / slidSpd)
// for the queue's roster. v7's server strings: "Joined Ranked Match. Abandoning a match will
// result in a temporary ban" (ranked.join), "Game in Progress" (ranked.progress) for anyone
// else, "Spectating Disabled in Ranked", "Can't switch while in-game" (the roster's teams are
// the comp lobby's `t`). v7's server code is not in the reconstruction, so the lobby rules are
// v3.1.1's isFromQueue ones (src/data/game.js): minPlayers holds the start until everyone is in
// (needAllTimer, here the brain's startGraceS), a player leaving before the end screen ends the
// match (leaveGame -> endGame(3)) and is an abandoner (checkLeave: rankedDeduction and the
// rankedCooldown queue ban, applied by the brain); one round (comp endTimer "inf").


static RankedSlot *acc_ranked_slot(const int account_id) {
    for (int i = 0; i < g_rk.count; i++) {
        if (account_id > 0 && g_rk.slots[i].account_id == account_id) return &g_rk.slots[i];
    }
    return NULL;
}

static void acc_ranked_setup(const cJSON *config) {
    memset(&g_rk, 0, sizeof(g_rk));
    g_rk.active = 1;
    g_rk.created_at = g_server->now;
    snprintf(g_rk.match_id, sizeof(g_rk.match_id), "%s", acc_str(cJSON_GetObjectItem(config, "matchId"), ""));
    snprintf(g_rk.queue, sizeof(g_rk.queue), "%s", acc_str(cJSON_GetObjectItem(config, "queue"), ""));
    const cJSON *size = cJSON_GetObjectItem(config, "teamSize");
    g_rk.team_size = cJSON_IsNumber(size) && size->valueint > 0 ? size->valueint : 1;
    const cJSON *grace = cJSON_GetObjectItem(config, "startGraceS");
    g_rk.grace = cJSON_IsNumber(grace) && grace->valuedouble > 0.0 ? (float) grace->valuedouble : 50.0f; // needAllTimer 50 s
    const cJSON *team;
    int t = 0;
    cJSON_ArrayForEach(team, cJSON_GetObjectItem(config, "teams")) {
        const cJSON *member;
        cJSON_ArrayForEach(member, team) {
            if (g_rk.count >= ACC_RANKED_MAX) break;
            RankedSlot *slot = &g_rk.slots[g_rk.count];
            const cJSON *id = cJSON_GetObjectItem(member, "accountId");
            if (!cJSON_IsNumber(id) || id->valueint <= 0) continue;
            memset(slot, 0, sizeof(*slot));
            slot->account_id = id->valueint;
            slot->team = t;
            snprintf(slot->name, sizeof(slot->name), "%s", acc_str(cJSON_GetObjectItem(member, "name"), ""));
            g_rk.count++;
        }
        if (++t >= 2) break;
    }
    server_log("[ranked] match %s (%s): %d player(s), grace %.0f s", g_rk.match_id, g_rk.queue, g_rk.count, (double) g_rk.grace);
}

// players.setTeam for queued lobbies: the account's queue team + 1, else the mode picks.
// isPremiumCustomHost: this connection's account hosts the current custom lobby (brain "host").
// v3.1.1 also needs premiumT > 0, but every account has premium while accounts.free_premium
// is on, so the host account alone decides.
const char *accounts_published_map(void) {
    return g_acc.custom && g_acc.published_map[0] ? g_acc.published_map : NULL;
}

int accounts_kr_rewards(void) {
    return accounts_published_map() && g_acc.kr_rewards;
}

int accounts_is_custom_host(const ClientConnection *client) {
    return g_acc.custom && g_acc.host_account_id && client->acct.account_id == g_acc.host_account_id;
}

int accounts_assigned_team(const ClientConnection *client) {
    const RankedSlot *slot = g_rk.active ? acc_ranked_slot(client->acct.account_id) : NULL;
    return slot ? slot->team + 1 : 0;
}

int accounts_ranked_active(void) {
    return g_rk.active;
}

// Only the roster plays a ranked lobby; a roster player who left is out (needAll).
static const char *acc_ranked_refusal(const ClientConnection *client) {
    if (!g_rk.active) return NULL;
    RankedSlot *slot = acc_ranked_slot(client->acct.account_id);
    if (!slot || slot->left || g_rk.ended) return "Game in Progress"; // ranked.progress
    return NULL;
}

static void acc_ranked_snapshot(RankedSlot *slot, const ClientConnection *client) {
    if (!client || !client->player) return;
    slot->kills = (int) client->player->kills;
    slot->deaths = (int) client->player->deaths;
    slot->score = client->player->score;
}

static ClientConnection *acc_client_by_account(const int account_id) {
    for (size_t i = 0; i < g_server->client_count; i++) {
        ClientConnection *client = &g_server->clients[i];
        if (account_id > 0 && client->acct.account_id == account_id && client->acct.join_state == SERVER_JOIN_READY) return client;
    }
    return NULL;
}

static void acc_ranked_on_leave(const ClientConnection *client) {
    RankedSlot *slot = g_rk.active ? acc_ranked_slot(client->acct.account_id) : NULL;
    if (!slot || g_rk.ended || slot->left || client->acct.join_state != SERVER_JOIN_READY) return;
    acc_ranked_snapshot(slot, client);
    slot->left = 1;
    g_rk.end_now = 1; // leaveGame: needAll -> endGame(3)
    server_log("[ranked] %s (account %d) left the match: abandoned, ending the round", client->name, slot->account_id);
}

// The winning team index (0/1), -1 draw: abandoners lose to a team that stayed, else the
// round's END (didWin of a roster player).
static int acc_ranked_winner(void) {
    int abandoned[2] = {0, 0}, present[2] = {0, 0};
    for (int i = 0; i < g_rk.count; i++) {
        const RankedSlot *slot = &g_rk.slots[i];
        if (slot->left || !slot->joined) abandoned[slot->team]++;
        else present[slot->team]++;
    }
    if (abandoned[0] && !abandoned[1] && present[1]) return 1;
    if (abandoned[1] && !abandoned[0] && present[0]) return 0;
    for (int i = 0; i < g_rk.count; i++) {
        const ClientConnection *client = acc_client_by_account(g_rk.slots[i].account_id);
        if (client && client->acct.round_result == 1) return g_rk.slots[i].team;
    }
    return -1;
}

static void acc_ranked_send_result(void) {
    g_rk.ended = 1;
    g_rk.ended_at = g_server->now;
    const int winner = acc_ranked_winner();

    cJSON *frame = cJSON_CreateObject();
    g_rk.result_rid = brain_next_rid();
    cJSON_AddStringToObject(frame, "type", "ranked-result");
    cJSON_AddNumberToObject(frame, "rid", g_rk.result_rid);
    cJSON_AddStringToObject(frame, "matchId", g_rk.match_id);
    cJSON_AddStringToObject(frame, "lobbyId", g_acc.lobby_id);
    cJSON_AddNumberToObject(frame, "winner", winner);
    cJSON *players = cJSON_AddArrayToObject(frame, "players");
    cJSON *abandoned = cJSON_AddArrayToObject(frame, "abandoned");
    for (int i = 0; i < g_rk.count; i++) {
        RankedSlot *slot = &g_rk.slots[i];
        acc_ranked_snapshot(slot, acc_client_by_account(slot->account_id));
        cJSON *p = cJSON_CreateObject();
        cJSON_AddNumberToObject(p, "accountId", slot->account_id);
        cJSON_AddNumberToObject(p, "team", slot->team);
        cJSON_AddNumberToObject(p, "score", slot->score);
        cJSON_AddNumberToObject(p, "kills", slot->kills);
        cJSON_AddNumberToObject(p, "deaths", slot->deaths);
        cJSON_AddItemToArray(players, p);
        if (slot->left || !slot->joined) cJSON_AddItemToArray(abandoned, cJSON_CreateNumber(slot->account_id));
    }
    server_log("[ranked] match %s over: winner team %d, %d abandoned", g_rk.match_id, winner, cJSON_GetArraySize(abandoned));
    brain_post(frame, 1); // reliable: the brain closes the match once
}

static void acc_handle_ranked_reply(const cJSON *frame) {
    const cJSON *rating;
    cJSON_ArrayForEach(rating, cJSON_GetObjectItem(frame, "ratings")) {
        const cJSON *id = cJSON_GetObjectItem(rating, "accountId");
        const cJSON *old_value = cJSON_GetObjectItem(rating, "old");
        const cJSON *new_value = cJSON_GetObjectItem(rating, "new");
        const cJSON *delta = cJSON_GetObjectItem(rating, "delta");
        if (!cJSON_IsNumber(id) || !cJSON_IsNumber(new_value)) continue;
        char text[NET_CHAT_TEXT_SIZE];
        snprintf(text, sizeof(text), "Ranked rating: %.0f -> %.0f (%+.0f)%s",
                 cJSON_IsNumber(old_value) ? old_value->valuedouble : 0.0, new_value->valuedouble,
                 cJSON_IsNumber(delta) ? delta->valuedouble : 0.0, acc_truthy(cJSON_GetObjectItem(rating, "abandoned")) ? " (abandoned)" : "");
        server_log("[ranked] account %d %s", id->valueint, text);
        ClientConnection *client = acc_client_by_account(id->valueint);
        if (client) acc_chat_to(client, text);
    }
    if (!cJSON_IsTrue(cJSON_GetObjectItem(frame, "ok"))) {
        server_log_error("[ranked] result rejected: %s", acc_str(cJSON_GetObjectItem(frame, "err"), "?"));
    }
}

static void acc_revert_custom(const char *why);

static void acc_tick_ranked(void) {
    if (!g_rk.active) return;
    Game *game = &g_server->game;

    if (!g_rk.started && !g_rk.ended) {
        int joined = 0, teams[2] = {0, 0};
        for (int i = 0; i < g_rk.count; i++) {
            const RankedSlot *slot = &g_rk.slots[i];
            if (slot->joined && !slot->left) {
                joined++;
                teams[slot->team]++;
            }
        }
        if (joined == g_rk.count) {
            g_rk.started = 1;
            server_log("[ranked] all %d player(s) in: starting", joined);
        } else if (g_server->now - g_rk.created_at >= g_rk.grace) {
            for (int i = 0; i < g_rk.count; i++) {
                if (!g_rk.slots[i].joined) g_rk.slots[i].left = 1; // never came: abandoned
            }
            if (teams[0] && teams[1]) { // start with whoever is there
                if (game->mode) game->mode->min_players = joined;
                g_rk.started = 1;
                server_log("[ranked] grace over: starting with %d of %d player(s)", joined, g_rk.count);
            } else {
                g_rk.end_now = 1; // needAllTimer ran out: endGame(3)
                server_log("[ranked] grace over with a team missing: ending the match");
            }
        }
    }

    if (g_rk.end_now && !g_rk.ended && game->end_timer <= 0.0f && game->ready) {
        g_rk.end_now = 0;
        game_end_round(game); // END to whoever is left (their didWin), then the result
        if (!g_rk.ended) acc_ranked_send_result();
    }

    if (!g_rk.ended && g_acc.round_end_seen) acc_ranked_send_result();

    // infEndTimer: no next round; the lobby returns to the public rotation instead
    if (g_rk.ended && (game->end_timer <= 1.0f || !acc_player_count())) {
        g_rk.active = 0;
        acc_revert_custom("ranked match over");
    }
}


// ------------------------------------------------------------------ info query / skins

static char g_community[64]; // the coordinator's welcome "community" (brainId fallback)

int accounts_player_count(void) { return acc_player_count(); }

const char *accounts_community_name(void) {
    if (g_gs_config.community[0]) return g_gs_config.community;
    return g_acc.brain ? g_community : "";
}

int accounts_info_tags(char tags[][24], const int max) {
    int n = 0;
    if (n < max && g_rk.active) snprintf(tags[n++], 24, "ranked");
    if (n < max && g_acc.custom) snprintf(tags[n++], 24, "custom");
    if (n < max && g_acc.draining) snprintf(tags[n++], 24, "draining");
    if (n < max && g_acc.brain) snprintf(tags[n++], 24, "community");
    return n;
}

// enforce_skins: -1 auto = on while linked to a coordinator (that is where inventories live)
static int acc_skins_enforced(void) {
    return g_gs_config.enforce_skins > 0 || (g_gs_config.enforce_skins < 0 && g_acc.brain);
}

#define ACC_SKIN_MAX ((int) (sizeof(((ServerAccount *) 0)->skins_owned) * 8))

int accounts_owns_skin(const ClientConnection *client, const int skin) {
    if (skin < 0 || !acc_skins_enforced()) return 1;
    if (!client->acct.account_id || !client->acct.skins_known || skin >= ACC_SKIN_MAX) return 0;
    return (client->acct.skins_owned[skin >> 3] >> (skin & 7)) & 1;
}

int accounts_filter_cosmetics(const ClientConnection *client, PlayerCosmetics *c, PlayerCosmeticsV7 *v7) {
    int32_t *slots[14] = {&c->hat, &c->body, &c->waist, &c->face, &c->dye};
    int count = 5;
    if (v7) {
        int32_t *more[] = {&v7->head, &v7->back, &v7->shoe, &v7->pet, &v7->wrist, &v7->charms[0], &v7->charms[1], &v7->player_card};
        for (size_t i = 0; i < sizeof(more) / sizeof(more[0]); i++) slots[count++] = more[i];
    }
    int stripped = 0;
    for (int i = 0; i < count; i++) {
        if (*slots[i] >= 0 && !accounts_owns_skin(client, *slots[i])) {
            *slots[i] = -1;
            stripped++;
        }
    }
    // v7.1.4 premium slot rule: without premium a player wears a HEAD or a hat, a BACK or a body, not
    // both (the head / back wins, as on the Customize tab, v7 index.js:5169-5209). v7 only enforces
    // it in the menu; KrunkNative also drops the pair here, for accounts (sandbox / guests: no rule).
    if (v7 && client->acct.account_id && !client->acct.premium && !(client->acct.flags & NET_PLAYER_INFO_DEV)) {
        if (v7->head >= 0 && c->hat >= 0) c->hat = -1, stripped++;
        if (v7->back >= 0 && c->body >= 0) c->body = -1, stripped++;
    }
    return stripped;
}

int accounts_filter_skins(const ClientConnection *client, PlayerSkins *s) {
    int32_t *slots[3] = {&s->primary, &s->secondary, &s->melee};
    int stripped = 0;
    for (int i = 0; i < 3; i++) {
        if (*slots[i] >= 0 && !accounts_owns_skin(client, *slots[i])) {
            *slots[i] = -1;
            stripped++;
        }
    }
    return stripped;
}

// account.data[10] (Account.getData skins): [{ind, cnt}, ...] -> bitset
static void acc_load_skins(ClientConnection *client, const cJSON *account) {
    memset(client->acct.skins_owned, 0, sizeof(client->acct.skins_owned));
    const cJSON *data = cJSON_GetObjectItem(account, "data");
    const cJSON *skins = cJSON_IsArray(data) ? cJSON_GetArrayItem(data, 10) : cJSON_GetObjectItem(account, "skins");
    client->acct.skins_known = cJSON_IsArray(skins);
    int count = 0;
    const cJSON *e;
    cJSON_ArrayForEach(e, skins) {
        const cJSON *ind = cJSON_IsObject(e) ? cJSON_GetObjectItem(e, "ind") : e;
        const cJSON *cnt = cJSON_IsObject(e) ? cJSON_GetObjectItem(e, "cnt") : NULL;
        if (!cJSON_IsNumber(ind) || (cnt && cJSON_IsNumber(cnt) && cnt->valueint <= 0)) continue;
        const int skin = ind->valueint;
        if (skin < 0 || skin >= ACC_SKIN_MAX) continue;
        client->acct.skins_owned[skin >> 3] |= (uint8_t) (1u << (skin & 7));
        count++;
    }
    if (client->acct.skins_known) server_log("%s owns %d skin(s) on this network", client->name, count);
}

// ------------------------------------------------------------------ connection lifecycle

int accounts_refuse_connect(const char *address, NetErrorPacket *error) {
    (void) address;
    if (g_acc.draining) {
        snprintf(error->message, sizeof(error->message), "GAME UPDATING"); // gameserver.ts UPDATING_SHUTDOWN_MESSAGE
        snprintf(error->code, sizeof(error->code), "004004");
        return 1;
    }
    return 0;
}

static void acc_reject(ClientConnection *client, const char *message, const char *code);

// A client cannot wear a skin it does not own (the coordinator's inventory): unowned
// cosmetic, weapon and melee slots fall back to none before the spawn, with a chat notice once per ENT.
static void acc_enforce_skins(ClientConnection *client) {
    const int stripped = accounts_filter_cosmetics(client, &client->cosmetics, &client->cosmetics_v7) + accounts_filter_skins(client, &client->skins);
    if (!stripped) return;
    server_log("%s: removed %d unowned cosmetic(s)", client->name, stripped);
    acc_chat_to(client, "You don't own some of the skins you equipped on this server");
}

static void acc_set_ready(ClientConnection *client) {
    const char *ranked_refusal = acc_ranked_refusal(client);
    if (ranked_refusal) { // isFromQueue: the queued accounts only
        acc_reject(client, ranked_refusal, "004001");
        return;
    }
    RankedSlot *slot = g_rk.active ? acc_ranked_slot(client->acct.account_id) : NULL;
    if (slot && !slot->joined) acc_chat_to(client, "Joined Ranked Match. Abandoning a match will result in a temporary ban"); // ranked.join
    if (slot) slot->joined = 1;
    const int was_waiting = client->acct.join_state != SERVER_JOIN_READY;
    client->acct.join_state = SERVER_JOIN_READY;
    net_comp_lobby_sync(client); // comp lobby: the account identity (+ the roster team)
    client->acct.auth_rid = 0;
    if (client->acct.account_id && gs_is_op(&g_gs_config, client->acct.account_id, client->acct.username)) {
        client->op = 1;
        client->acct.flags |= NET_PLAYER_INFO_DEV;
        server_log("%s is an op (config gameserver.ops)", client->name);
    }

    if (g_acc.brain && (client->acct.ticket_jti[0] || client->acct.account_id)) {
        cJSON *join = cJSON_CreateObject();
        cJSON_AddStringToObject(join, "type", "player-join");
        cJSON_AddStringToObject(join, "lobbyId", g_acc.lobby_id);
        if (client->acct.ticket_jti[0]) cJSON_AddStringToObject(join, "clientId", client->acct.ticket_jti);
        cJSON_AddNumberToObject(join, "accountId", client->acct.account_id);
        brain_post(join, 0);
    }
    acc_queue_push();

    if (was_waiting && client->acct.spawn_deferred) {
        client->acct.spawn_deferred = 0;
        acc_enforce_skins(client); // the deferred ENT's cosmetics, now that the inventory is known
        net_resume_ent(client, client->acct.spawn_spectate);
    }
}

void accounts_on_connect(ClientConnection *client) {
    memset(&client->acct, 0, sizeof(client->acct));
    client->acct.badge = -1;
    client->acct.round_result = -1;
    const int gated = g_acc.brain || g_gs_config.online_mode || g_gs_config.password[0];
    client->acct.join_state = gated ? SERVER_JOIN_AWAIT : SERVER_JOIN_READY;
    client->acct.join_deadline = g_server->now + g_gs_config.join_timeout;
    if (g_gs_config.online_mode) {
        // Mojang-style join: the client tells central "joining <serverHash>", then JOINs with
        // client_key "hj1:<client nonce>" (shared/include/auth_join.h)
        NetAuthChallengePacket challenge;
        memset(&challenge, 0, sizeof(challenge));
        snprintf(challenge.server_nonce, sizeof(challenge.server_nonce), "%s", g_acc.server_nonce);
        challenge.online_mode = 1;
        snprintf(challenge.auth_url, sizeof(challenge.auth_url), "%s", g_gs_config.auth_url);
        net_send_packet(client->fd, NET_PACKET_AUTH_CHALLENGE, &challenge, sizeof(challenge));
    }
    if (!g_acc.brain) return;
    acc_queue_push();
}

static void acc_reject(ClientConnection *client, const char *message, const char *code) {
    server_log("Rejecting %s (id %d): %s", client->address, client->id, message);
    client->acct.join_state = SERVER_JOIN_AUTH; // never spawns while the kick is pending
    client->acct.auth_rid = 0;
    net_kick_client(client->id, message, code);
}

static void acc_become_guest(ClientConnection *client, const char *why) {
    if (g_gs_config.online_mode) { // online-mode=true: every player is a verified account
        acc_reject(client, client->acct.central_pending ? "Authentication servers are down" : "Failed to verify username", "004001");
        return;
    }
    if (g_gs_config.password[0] && !client->acct.password_ok) {
        acc_reject(client, "This server needs a password", "004005");
        return;
    }
    if (!g_acc.brain) { // standalone: guests always (allow_guests / require_ticket are coordinator rules)
        if (why) server_log("%s joins as a guest (%s)", client->name, why);
        client->acct.account_id = 0;
        acc_set_ready(client);
        return;
    }
    if (!g_gs_config.allow_guests || g_gs_config.require_ticket) {
        acc_reject(client, g_gs_config.allow_guests ? "Join through the server list" : "Guests are not allowed on this server", "004001");
        return;
    }
    if (why) server_log("%s joins as a guest (%s)", client->name, why);
    client->acct.account_id = 0;
    acc_set_ready(client);
}

static void acc_send_auth(ClientConnection *client, const char *key, const char *value) {
    cJSON *frame = cJSON_CreateObject();
    const uint32_t rid = brain_next_rid();
    cJSON_AddStringToObject(frame, "type", "auth");
    cJSON_AddNumberToObject(frame, "rid", rid);
    cJSON_AddStringToObject(frame, key, value);
    client->acct.auth_rid = rid;
    client->acct.join_state = SERVER_JOIN_AUTH;
    client->acct.join_deadline = g_server->now + 10.0f; // brainClient.ts DB_CALL_TIMEOUT_MS
    brain_post(frame, 0);
}

static void acc_apply_account(ClientConnection *client, const cJSON *account);

// one connection per account: the newer one wins (a reconnect after a network drop)
static void acc_kick_duplicates(ClientConnection *client) {
    for (size_t i = 0; i < g_server->client_count; i++) {
        ClientConnection *other = &g_server->clients[i];
        if (other != client && other->acct.account_id == client->acct.account_id && other->acct.join_state == SERVER_JOIN_READY) {
            net_kick_client(other->id, "Logged in from another location", "004002");
        }
    }
}

static void acc_copy_field(char *dst, const size_t size, const char *src) {
    char clean[NET_CHAT_TEXT_SIZE];
    if (!src || !net_chat_sanitize(clean, sizeof(clean), src, strlen(src))) {
        dst[0] = 0;
        return;
    }
    snprintf(dst, size, "%s", clean);
}

// online_mode JOIN (contract-auth): "hj1:<client nonce>" -> serverHash -> hasJoined, through
// the coordinator when linked (it creates the community account, returns skins/stats),
// else straight to central auth (session-only stats). Seek-game tickets of the coordinator
// are trusted as before. Returns 1 when the JOIN was consumed (or must be ignored).
static int acc_online_join(ClientConnection *client, const NetJoinPacket *join) {
    const size_t prefix = strlen(AUTH_JOIN_KEY_PREFIX);
    if (strncmp(join->client_key, AUTH_JOIN_KEY_PREFIX, prefix)) {
        if (g_acc.brain && join->client_key[0]) return 0; // a coordinator ticket: verified below
        // the client's first JOIN goes out before it saw the challenge: wait for the "hj1:" one
        return 1;
    }
    const char *client_nonce = join->client_key + prefix;
    char username[32];
    acc_copy_field(username, sizeof(username), join->name);
    if (!kn_auth_is_hex(client_nonce, AUTH_JOIN_NONCE_LEN) || !username[0]) {
        acc_reject(client, "Failed to verify username", "004001");
        return 1;
    }
    char hash[AUTH_SERVER_HASH_LEN + 1];
    kn_server_hash(g_acc.server_nonce, "", client_nonce, hash);
    snprintf(client->acct.username, sizeof(client->acct.username), "%s", username);
    client->acct.join_state = SERVER_JOIN_AUTH;
    client->acct.join_deadline = g_server->now + 10.0f;
    client->acct.central_pending = 1;
    if (brain_linked()) {
        cJSON *frame = cJSON_CreateObject();
        const uint32_t rid = brain_next_rid();
        cJSON_AddStringToObject(frame, "type", "auth");
        cJSON_AddNumberToObject(frame, "rid", rid);
        cJSON *hj = cJSON_AddObjectToObject(frame, "hasJoined");
        cJSON_AddStringToObject(hj, "username", username);
        cJSON_AddStringToObject(hj, "serverHash", hash);
        cJSON_AddStringToObject(hj, "ip", client->address);
        client->acct.auth_rid = rid;
        brain_post(frame, 0);
        return 1;
    }
    const uint32_t rid = 0x80000000u | ++g_acc.next_central_rid; // never collides with brain rids
    client->acct.auth_rid = rid;
    if (central_auth_has_joined(g_gs_config.auth_url, rid, username, hash, client->address)) {
        acc_reject(client, "Authentication servers are down", "004001");
    }
    return 1;
}

// central hasJoined finished (standalone online_mode)
static void acc_handle_central(const CentralAuthResult *r) {
    ClientConnection *client = acc_client_by_rid(r->rid);
    if (!client || client->acct.join_state != SERVER_JOIN_AUTH || !client->acct.central_pending) return;
    client->acct.central_pending = 0;
    if (r->status != 1) {
        server_log("%s (%s): %s", client->acct.username, client->address, r->error);
        acc_reject(client, r->status == 0 ? "Failed to verify username" : "Authentication servers are down", "004001");
        return;
    }
    const cJSON *profile = cJSON_GetObjectItem(r->reply, "profile");
    cJSON *account = cJSON_IsObject(profile) ? cJSON_Duplicate(profile, 1) : cJSON_CreateObject();
    cJSON_DeleteItemFromObject(account, "id");
    cJSON_AddNumberToObject(account, "id", cJSON_GetNumberValue(cJSON_GetObjectItem(r->reply, "id")));
    if (!cJSON_GetObjectItem(account, "username"))
        cJSON_AddStringToObject(account, "username", acc_str(cJSON_GetObjectItem(r->reply, "username"), client->acct.username));
    acc_apply_account(client, account);
    const int banned = acc_truthy(cJSON_GetObjectItem(account, "banned"));
    cJSON_Delete(account);
    client->acct.central_verified = 1;
    if (banned || acc_account_banned(client->acct.account_id, client->acct.username)) {
        acc_reject(client, "Banned", "004003");
        return;
    }
    acc_kick_duplicates(client);
    server_log("%s verified by central auth as %s (account %d, session-only stats)", client->address, client->acct.username,
               client->acct.account_id);
    acc_set_ready(client);
}

void accounts_handle_join(ClientConnection *client, const NetPacket *packet) {
    if (client->acct.join_state != SERVER_JOIN_AWAIT) return; // standalone without password: guests, as always
    if (packet->length != sizeof(NetJoinPacket) && packet->length != NET_JOIN_PACKET_V1_SIZE &&
        packet->length != NET_JOIN_PACKET_V2_SIZE) return;

    NetJoinPacket join;
    memset(&join, 0, sizeof(join));
    memcpy(&join, packet->payload, packet->length);
    join.game_id[sizeof(join.game_id) - 1] = 0;
    join.client_key[sizeof(join.client_key) - 1] = 0;
    join.session[sizeof(join.session) - 1] = 0;
    join.name[sizeof(join.name) - 1] = 0;
    join.password[sizeof(join.password) - 1] = 0;

    if (g_gs_config.password[0]) {
        if (strcmp(join.password, g_gs_config.password)) {
            acc_reject(client, join.password[0] ? "Wrong server password" : "This server needs a password", "004005");
            return;
        }
        client->acct.password_ok = 1;
    }

    if (g_gs_config.online_mode && acc_online_join(client, &join)) return;

    if (!g_acc.brain) {
        acc_become_guest(client, NULL); // standalone: a password-checked guest
        return;
    }

    if (join.client_key[0]) {
        char err[96] = "";
        cJSON *claims = brain_verify_ticket(join.client_key, err, sizeof(err));
        const char *nid = cJSON_GetStringValue(cJSON_GetObjectItem(claims, "nid"));
        const char *lid = cJSON_GetStringValue(cJSON_GetObjectItem(claims, "lid"));
        if (!claims) {
            cJSON_Delete(claims);
            acc_reject(client, strstr(err, "exp") ? "Join key expired" : "Invalid join key", "004001");
            return;
        }
        if (!nid || strcmp(nid, g_gs_config.node_id) || !lid || strcmp(lid, g_acc.lobby_id)) {
            cJSON_Delete(claims);
            acc_reject(client, "Join key is for another server", "004001");
            return;
        }
        const char *jti = cJSON_GetStringValue(cJSON_GetObjectItem(claims, "jti"));
        snprintf(client->acct.ticket_jti, sizeof(client->acct.ticket_jti), "%s", jti ? jti : "");
        const cJSON *acc = cJSON_GetObjectItem(claims, "acc");
        const int account_id = cJSON_IsNumber(acc) ? acc->valueint : 0;
        if (account_id > 0) {
            // the ticket is authoritative for who it is; the brain adds level, clan, bans
            client->acct.account_id = account_id;
            acc_copy_field(client->acct.username, sizeof(client->acct.username), cJSON_GetStringValue(cJSON_GetObjectItem(claims, "name")));
            if (client->acct.username[0]) snprintf(client->name, sizeof(client->name), "%s", client->acct.username);
            cJSON_Delete(claims);
            if (brain_linked()) acc_send_auth(client, "ticket", join.client_key);
            else acc_set_ready(client);
            return;
        }
        cJSON_Delete(claims);
        // a guest ticket: a session token may still identify the player
        if (!join.session[0]) {
            acc_become_guest(client, NULL);
            return;
        }
    }

    if (join.session[0]) {
        if (!brain_linked()) {
            acc_become_guest(client, "account service unavailable");
            return;
        }
        acc_send_auth(client, "token", join.session);
        return;
    }

    if (g_gs_config.require_ticket) {
        acc_reject(client, "Join through the server list", "004001");
        return;
    }
    acc_become_guest(client, NULL);
}

static void acc_apply_account(ClientConnection *client, const cJSON *account) {
    const cJSON *id = cJSON_GetObjectItem(account, "id");
    if (cJSON_IsNumber(id)) client->acct.account_id = id->valueint;
    acc_copy_field(client->acct.username, sizeof(client->acct.username), cJSON_GetStringValue(cJSON_GetObjectItem(account, "username")));
    char alias[32];
    acc_copy_field(alias, sizeof(alias), cJSON_GetStringValue(cJSON_GetObjectItem(account, "alias")));
    const char *display = alias[0] ? alias : client->acct.username;
    if (display[0]) snprintf(client->name, sizeof(client->name), "%s", display);

    const cJSON *level = cJSON_GetObjectItem(account, "level");
    client->acct.level = cJSON_IsNumber(level) ? level->valueint : 1;
    acc_copy_field(client->acct.clan, sizeof(client->acct.clan), cJSON_GetStringValue(cJSON_GetObjectItem(account, "clan")));
    const cJSON *funds = cJSON_GetObjectItem(account, "funds");
    client->acct.funds = cJSON_IsNumber(funds) ? funds->valueint : 0;
    client->acct.premium = acc_truthy(cJSON_GetObjectItem(account, "premium"));
    client->acct.hack = acc_truthy(cJSON_GetObjectItem(account, "hack"));
    client->acct.flags = 0;
    if (client->acct.premium) client->acct.flags |= NET_PLAYER_INFO_PREMIUM;
    if (acc_truthy(cJSON_GetObjectItem(account, "featured"))) client->acct.flags |= NET_PLAYER_INFO_VERIFIED;
    if (acc_truthy(cJSON_GetObjectItem(account, "dev"))) client->acct.flags |= NET_PLAYER_INFO_DEV;
    if (acc_truthy(cJSON_GetObjectItem(account, "mod"))) client->acct.flags |= NET_PLAYER_INFO_MOD;
    // WP-F: the custom badge (v7 account.stats.bdg, badges.js index), when the brain supplies one
    const cJSON *stats = cJSON_GetObjectItem(account, "stats");
    const cJSON *bdg = cJSON_IsObject(stats) ? cJSON_GetObjectItem(stats, "bdg") : cJSON_GetObjectItem(account, "badge");
    client->acct.badge = cJSON_IsNumber(bdg) && bdg->valueint >= 0 ? bdg->valueint : -1;
    // WP-E part 2c: only a badge the account owns (kn_brain's account "badges", badges.js indices)
    const cJSON *owned = cJSON_GetObjectItem(account, "badges");
    if (client->acct.badge >= 0 && cJSON_IsArray(owned)) {
        int found = 0;
        const cJSON *b;
        cJSON_ArrayForEach(b, owned) found |= cJSON_IsNumber(b) && b->valueint == client->acct.badge;
        if (!found) client->acct.badge = -1;
    }
    acc_load_skins(client, account);
    // WP-E: the death screen's account values (class levels, config.playerCardStats)
    client->acct.has_stats = 1;
    for (int i = 0; i < SERVER_CLASS_STATS_MAX; i++) {
        char key[8];
        snprintf(key, sizeof(key), "c%d", i);
        const cJSON *c = cJSON_IsObject(stats) ? cJSON_GetObjectItemCaseSensitive(stats, key) : NULL;
        client->acct.class_stats[i] = cJSON_IsNumber(c) && c->valuedouble > 0.0 ? (int) c->valuedouble : 0;
    }
    for (int i = 0; i < PLAYER_CARD_STAT_COUNT; i++) player_card_stat_format(i, account, client->acct.pc_stats[i], sizeof(client->acct.pc_stats[i]));
}

void accounts_player_card(const ClientConnection *client, Player *player) {
    const int account = client->acct.account_id > 0 && client->acct.has_stats;
    const int cls = player->class_index;
    // players.js:4860: account.stats["c" + classIndex] unless anonMode; [pcStatIndex, stat] with an account
    player->class_stat = account && !player->anon_mode && cls >= 0 && cls < SERVER_CLASS_STATS_MAX ? client->acct.class_stats[cls] : 0;
    player->pc_stat_index = account && client->pc_stat_index >= 0 && client->pc_stat_index < PLAYER_CARD_STAT_COUNT ? client->pc_stat_index : -1;
    if (player->pc_stat_index >= 0) snprintf(player->pc_stat_value, sizeof(player->pc_stat_value), "%s", client->acct.pc_stats[player->pc_stat_index]);
    else player->pc_stat_value[0] = 0;
}

static void acc_handle_auth_reply(const cJSON *frame) {
    const cJSON *rid = cJSON_GetObjectItem(frame, "rid");
    ClientConnection *client = cJSON_IsNumber(rid) ? acc_client_by_rid((uint32_t) rid->valuedouble) : NULL;
    if (!client || client->acct.join_state != SERVER_JOIN_AUTH) return;

    const cJSON *account = cJSON_GetObjectItem(frame, "account");
    if (!cJSON_IsTrue(cJSON_GetObjectItem(frame, "ok")) || !cJSON_IsObject(account)) {
        const char *err = cJSON_GetStringValue(cJSON_GetObjectItem(frame, "err"));
        acc_reject(client, err && err[0] ? err : "Invalid session", err && !strncasecmp(err, "Banned", 6) ? "004003" : "004001");
        return;
    }

    acc_apply_account(client, account);

    if (acc_truthy(cJSON_GetObjectItem(account, "banned"))) {
        const char *reason = cJSON_GetStringValue(cJSON_GetObjectItem(account, "ban_reason"));
        char message[96];
        snprintf(message, sizeof(message), "Banned%s%s", reason && reason[0] ? ": " : "", reason ? reason : "");
        acc_reject(client, message, "004003");
        return;
    }
    if (acc_account_banned(client->acct.account_id, client->acct.username)) {
        acc_reject(client, "Banned", "004003");
        return;
    }

    acc_kick_duplicates(client);
    if (client->acct.central_pending) {
        client->acct.central_pending = 0;
        client->acct.central_verified = 1;
    }

    server_log("%s authenticated as account %d (level %d%s%s)", client->address, client->acct.account_id, client->acct.level,
               client->acct.clan[0] ? ", clan " : "", client->acct.clan);
    acc_set_ready(client);
}

int accounts_gate_ent(ClientConnection *client, const int spectate) {
    if (client->acct.join_state == SERVER_JOIN_READY) {
        acc_enforce_skins(client);
        return 1;
    }
    client->acct.spawn_deferred = 1;
    client->acct.spawn_spectate = (unsigned char) (spectate != 0);
    return 0;
}

void accounts_on_spawn(ClientConnection *client, const int first_spawn) {
    if (client->acct.match_since <= 0.0f) client->acct.match_since = g_server->now;
    acc_broadcast_info(client);
    if (!first_spawn) return;
    accounts_send_infos_to(client);
    for (int i = 0; i < g_gs_config.motd_count; i++) {
        char text[NET_CHAT_TEXT_SIZE];
        const char *line = g_gs_config.motd[i];
        const char *dollar = strchr(line, '$');
        if (dollar) snprintf(text, sizeof(text), "%.*s%s%s", (int) (dollar - line), line, client->name, dollar + 1);
        else snprintf(text, sizeof(text), "%s", line);
        acc_chat_to(client, text);
    }
}

static void acc_revert_custom(const char *why);

void accounts_on_disconnect(ClientConnection *client) {
    if (!g_acc.brain) return;
    acc_ranked_on_leave(client);
    acc_commit_partial(client);
    if (client->acct.join_state == SERVER_JOIN_READY && (client->acct.account_id || client->acct.ticket_jti[0])) {
        cJSON *leave = cJSON_CreateObject();
        cJSON_AddStringToObject(leave, "type", "player-leave");
        cJSON_AddStringToObject(leave, "lobbyId", g_acc.lobby_id);
        if (client->acct.ticket_jti[0]) cJSON_AddStringToObject(leave, "clientId", client->acct.ticket_jti);
        cJSON_AddNumberToObject(leave, "accountId", client->acct.account_id);
        brain_post(leave, 0);
    }
    acc_queue_push();
}


// ------------------------------------------------------------------ hosted custom games

static void acc_revert_custom(const char *why) {
    if (!g_acc.custom) return;
    server_log("[brain] custom game over (%s): back to the public rotation", why);
    if (g_rk.active && !g_rk.ended) acc_ranked_send_result(); // everyone left: the match still closes once
    g_rk.active = 0;
    g_acc.custom = 0;
    g_acc.host_account_id = 0;
    g_acc.published_map[0] = 0;
    g_acc.kr_rewards = 0;
    accounts_configure_public(&g_server->game, g_acc.cli_map);
    net_change_map(-1, -1);
}

// brain "host": v3.1.1 custom config (maps, modes, server-config varNs).
static void acc_host_reply_error(const cJSON *frame, const char *err) {
    const cJSON *rid = cJSON_GetObjectItem(frame, "rid");
    cJSON *reply = cJSON_CreateObject();
    cJSON_AddStringToObject(reply, "type", "host-reply");
    if (cJSON_IsNumber(rid)) cJSON_AddNumberToObject(reply, "rid", rid->valuedouble);
    cJSON_AddBoolToObject(reply, "ok", 0);
    cJSON_AddStringToObject(reply, "err", err);
    brain_post(reply, 0);
}

// config.customMap / config.mapName: a map this server has (its own list, then a catalogue
// entry with exactly that name or key), else NULL = a published map to fetch.
static const cJSON *acc_local_custom_map(const char *name) {
    const cJSON *json = acc_public_map_by_name(name);
    if (json) return json;
    for (size_t i = 0; i < map_catalog_count(); i++) {
        const MapCatalogEntry *e = map_catalog_at(i);
        if (e && ((e->name && !strcasecmp(e->name, name)) || (e->key && !strcasecmp(e->key, name)))) return map_catalog_json(i);
    }
    const int renamed = map_catalog_index_of_old_key(name); // an older client's "v3_burg" / "old_burg"
    return renamed >= 0 ? map_catalog_json((size_t) renamed) : NULL;
}

static void acc_host_apply(const cJSON *frame, const cJSON *custom_map);

static void acc_handle_host(const cJSON *frame) {
    const cJSON *config = cJSON_GetObjectItem(frame, "config");
    const char *err = NULL;
    if (!g_gs_config.custom_games) err = "custom games are disabled on this server";
    else if (g_acc.draining) err = "server is draining";
    else if (g_acc.custom || g_acc.pending_host) err = "already hosting a custom game";
    else if (acc_player_count() > 0) err = "lobby is not empty";
    if (err) {
        acc_host_reply_error(frame, err);
        return;
    }

    // v3.1.1 hosting a community map (playMap / Host Game "customMap")
    const char *custom = acc_str(cJSON_GetObjectItem(config, "customMap"), acc_str(cJSON_GetObjectItem(config, "mapName"), NULL));
    if (custom && custom[0]) {
        const cJSON *local = acc_local_custom_map(custom);
        if (!local) {
            g_acc.pending_rid = brain_next_rid();
            if (brain_fetch_map(g_acc.pending_rid, custom)) {
                acc_host_reply_error(frame, "cannot fetch the map");
                return;
            }
            g_acc.pending_host = cJSON_Duplicate(frame, 1);
            return; // acc_handle_map_data finishes it
        }
        acc_host_apply(frame, local);
        return;
    }
    acc_host_apply(frame, NULL);
}

// /api/map-data answer for the pending "host".
static void acc_handle_map_data(cJSON *frame) {
    const cJSON *rid = cJSON_GetObjectItem(frame, "rid");
    if (!g_acc.pending_host || !cJSON_IsNumber(rid) || (uint32_t) rid->valuedouble != g_acc.pending_rid) return;
    cJSON *pending = g_acc.pending_host;
    g_acc.pending_host = NULL;
    cJSON *map = cJSON_IsTrue(cJSON_GetObjectItem(frame, "ok")) ? cJSON_DetachItemFromObject(frame, "map") : NULL;
    if (!map) {
        const char *err = acc_str(cJSON_GetObjectItem(frame, "err"), "map not found");
        server_log_error("[brain] published map for a hosted game: %s", err);
        acc_host_reply_error(pending, err);
        cJSON_Delete(pending);
        return;
    }
    // v7.1.4 game.prepareGame: "(" / ")" -> full-width, then customGameConfig.validateMap (a
    // published map has a creator, so the featured limits apply) and the prefab / texture checks
    map_prepare_custom_json(map);
    const char *invalid = map_validate_error(map, 1);
    if (invalid) {
        server_log_error("[brain] published map for a hosted game rejected: %s", invalid);
        acc_host_reply_error(pending, invalid);
        cJSON_Delete(pending);
        cJSON_Delete(map);
        return;
    }
    map_sanitize_json(map);
    if (acc_player_count() > 0 || g_acc.custom || g_acc.draining) { // the lobby changed meanwhile
        acc_host_reply_error(pending, "lobby is not empty");
        cJSON_Delete(map);
    } else {
        cJSON *previous = g_acc.hosted_map;
        g_acc.hosted_map = map;
        acc_host_apply(pending, map);
        cJSON_Delete(previous); // only referenced by a reverted game (the current one is map)
    }
    cJSON_Delete(pending);
}

static void acc_host_apply(const cJSON *frame, const cJSON *custom_map) {
    const cJSON *rid = cJSON_GetObjectItem(frame, "rid");
    const cJSON *config = cJSON_GetObjectItem(frame, "config");
    cJSON *reply = cJSON_CreateObject();
    cJSON_AddStringToObject(reply, "type", "host-reply");
    if (cJSON_IsNumber(rid)) cJSON_AddNumberToObject(reply, "rid", rid->valuedouble);

    // WP-E: the request is a v7 host config: "override" picks the host type (config.hostTypes);
    // every round then runs setupGameConfig(override, map config + request) -> applyConfig(cfg, 3)
    // (game_apply_map_config). The setup below only decides the forced maps / modes.
    cJSON *request = cJSON_IsObject(config) ? cJSON_Duplicate(config, 1) : cJSON_CreateObject();
    const cJSON *override = cJSON_GetObjectItem(config, "override");
    const int ranked = cJSON_IsTrue(cJSON_GetObjectItem(config, "ranked"));
    // ranked (doc/backend.md 6.5): v7's ranked match is a comp lobby, config.hostTypes[1]
    const int host_index = ranked ? GAME_HOST_COMP
                         : cJSON_IsNumber(override) && override->valueint >= 0 && override->valueint < GAME_HOST_TYPE_COUNT ? override->valueint : GAME_HOST_CUSTOM;

    if (ranked && request) {
        // the roster is the lobby: tmSize = team size - 1 (comp minPlayers / maxPlayers = 2 x
        // (tmSize + 1) + specSlots, so everyone must be in before the countdown, v3.1.1 needAll),
        // no spectator slots (ranked.spectate), the overDat's map / mode replaced by the queue's
        const cJSON *team_size = cJSON_GetObjectItem(config, "teamSize");
        int size = cJSON_IsNumber(team_size) && team_size->valueint > 0 ? team_size->valueint : RANKED_TEAM_SIZE;
        if (size > 4) size = 4; // tmSize options 1v1..4v4
        const char *const replaced[] = {"tmSize", "specSlots", "allowSpect", "override", "maxPlayers", "minPlayers"};
        for (size_t i = 0; i < sizeof(replaced) / sizeof(replaced[0]); i++) cJSON_DeleteItemFromObjectCaseSensitive(request, replaced[i]);
        cJSON_AddNumberToObject(request, "tmSize", size - 1);
        cJSON_AddNumberToObject(request, "specSlots", 0);
        cJSON_AddBoolToObject(request, "allowSpect", 0);
        cJSON_AddNumberToObject(request, "override", GAME_HOST_COMP);
    }
    // Comp (ranked), Zombies and Raid hosts: setupGameConfig's overDat / forceDat / allowedDat
    cJSON *forced = ranked || host_index == GAME_HOST_ZOMBIES || host_index == GAME_HOST_RAID ? game_config_setup(host_index, request, NULL, ranked) : NULL;
    GameConfig game_config = g_default_game_config;
    game_config_read(&game_config, forced ? forced : request, GAME_CONFIG_CLAMP_HOSTED, host_index); // round 1 before its map loads

    // maps: catalogue indices, or names (this server's own maps first: custom dirs / the
    // command line map), else the public rotation
    const cJSON *chosen[GS_MAX_LIST];
    size_t map_count = 0;
    cJSON *map_names = cJSON_Duplicate(cJSON_GetObjectItem(forced ? forced : config, "maps"), 1);
    const cJSON *single_map = cJSON_GetObjectItem(config, "map");
    if (!cJSON_IsArray(map_names) && cJSON_IsString(single_map)) {
        cJSON_Delete(map_names);
        map_names = cJSON_CreateArray();
        cJSON_AddItemToArray(map_names, cJSON_CreateString(single_map->valuestring));
    }
    const cJSON *entry;
    cJSON_ArrayForEach(entry, map_names) {
        if (map_count >= GS_MAX_LIST) break;
        const cJSON *json = NULL;
        if (cJSON_IsString(entry)) json = acc_public_map_by_name(entry->valuestring);
        if (!json) {
            const int index = cJSON_IsNumber(entry) ? entry->valueint : cJSON_IsString(entry) ? map_catalog_find(entry->valuestring) : -1;
            json = index >= 0 ? map_catalog_json((size_t) index) : NULL;
        }
        if (json) chosen[map_count++] = json;
    }
    cJSON_Delete(map_names);
    if (custom_map) {
        chosen[0] = custom_map;
        map_count = 1;
    }
    if (!map_count) {
        for (size_t i = 0; i < g_server->game.map_count && map_count < GS_MAX_LIST; i++) chosen[map_count++] = g_server->game.maps[i];
    }
    const cJSON **maps = map_count ? calloc(map_count, sizeof(cJSON *)) : NULL;
    if (maps) memcpy(maps, chosen, map_count * sizeof(cJSON *));
    else map_count = 0;

    int *modes = NULL;
    size_t mode_count = 0;
    cJSON *mode_list = cJSON_Duplicate(cJSON_GetObjectItem(forced ? forced : config, "modes"), 1);
    const cJSON *single_mode = cJSON_GetObjectItem(config, "mode");
    if (!cJSON_GetArraySize(mode_list) && cJSON_IsNumber(single_mode)) {
        cJSON_Delete(mode_list);
        mode_list = cJSON_CreateArray();
        cJSON_AddItemToArray(mode_list, cJSON_CreateNumber(single_mode->valueint));
    }
    if (cJSON_GetArraySize(mode_list) > 0) {
        modes = calloc((size_t) cJSON_GetArraySize(mode_list), sizeof(int));
        cJSON_ArrayForEach(entry, mode_list) {
            const int id = cJSON_IsNumber(entry) ? entry->valueint : cJSON_IsString(entry) ? mode_find(entry->valuestring) : -1;
            if (modes && mode_config(id) && (mode_hostable(id) || forced)) modes[mode_count++] = id;
        }
        if (!mode_count) {
            free(modes);
            modes = NULL;
        }
    }
    cJSON_Delete(mode_list);
    if (ranked && !mode_count) {
        modes = calloc(1, sizeof(int));
        if (modes) modes[mode_count++] = MODE_ID_POINT; // the comp host's overDat modes [2]: Hardpoint
    }

    g_acc.custom = 1;
    g_acc.custom_since = g_server->now;
    // WP6: a hosted published map is v7's customMap played with defaultSetts: GAME.STORAGE, Save /
    // Load Data and (host option krRewards, customGameConfig.js:438-445) KR rewards use its name.
    // [KN] defaultSetts is not modelled: every hosted published map counts.
    const char *published = custom_map ? acc_str(cJSON_GetObjectItem(config, "customMap"), acc_str(cJSON_GetObjectItem(config, "mapName"), "")) : "";
    snprintf(g_acc.published_map, sizeof(g_acc.published_map), "%s", published);
    g_acc.kr_rewards = g_acc.published_map[0] && cJSON_IsTrue(cJSON_GetObjectItem(config, "krRewards"));
    g_acc.custom_had_players = 0;
    const cJSON *host_id = cJSON_GetObjectItem(frame, "hostAccountId");
    g_acc.host_account_id = cJSON_IsNumber(host_id) ? host_id->valueint : 0;
    snprintf(g_acc.host_name, sizeof(g_acc.host_name), "%s", acc_str(cJSON_GetObjectItem(frame, "hostName"), ""));

    if (ranked) acc_ranked_setup(config);
    game_configure(&g_server->game, &game_config, maps, map_count, modes, mode_count, NULL, 0, NULL, 0);
    game_config_host(&g_server->game, request, host_index); // WP-E: every round's map config + this request
    cJSON_Delete(request);
    cJSON_Delete(forced);
    net_change_map(0, 0);
    server_log("[brain] hosting a custom game for %s (account %d): %zu map(s), %zu mode(s), %d players", g_acc.host_name[0] ? g_acc.host_name : "?",
               g_acc.host_account_id, g_server->game.map_count, g_server->game.mode_count, g_server->config.max_players);

    cJSON_AddBoolToObject(reply, "ok", 1);
    cJSON_AddStringToObject(reply, "lobbyId", g_acc.lobby_id);
    brain_post(reply, 0);
    acc_push_lobby_now();
}

static void acc_tick_custom(void) {
    if (!g_acc.custom) return;
    const int players = acc_player_count();
    if (players) g_acc.custom_had_players = 1;

    if (g_acc.host_account_id && g_acc.custom_had_players) {
        int host_present = 0;
        for (size_t i = 0; i < g_server->client_count; i++) host_present |= g_server->clients[i].acct.account_id == g_acc.host_account_id;
        if (!host_present) {
            net_broadcast_chat(NULL, "Host ended game"); // gameserver.ts HOST_ENDED_MESSAGE
            acc_revert_custom("host left");
            return;
        }
    }
    if (!players && (g_acc.custom_had_players || g_server->now - g_acc.custom_since > ACC_HOST_IDLE_REVERT)) acc_revert_custom("lobby empty");
}

// ------------------------------------------------------------------ brain events

static void acc_handle_commit_reply(const cJSON *frame) {
    const cJSON *result;
    cJSON_ArrayForEach(result, cJSON_GetObjectItem(frame, "results")) {
        const cJSON *id = cJSON_GetObjectItem(result, "accountId");
        if (!cJSON_IsNumber(id)) continue;
        if (!cJSON_IsTrue(cJSON_GetObjectItem(result, "ok"))) {
            const char *err = cJSON_GetStringValue(cJSON_GetObjectItem(result, "err"));
            server_log_error("[stats] commit for account %d failed: %s", id->valueint, err ? err : "?");
            continue;
        }
        for (size_t i = 0; i < g_server->client_count; i++) {
            ClientConnection *client = &g_server->clients[i];
            if (client->acct.account_id != id->valueint) continue;
            const cJSON *level = cJSON_GetObjectItem(result, "level");
            const cJSON *funds = cJSON_GetObjectItem(result, "funds");
            const int old_level = client->acct.level;
            if (cJSON_IsNumber(level)) client->acct.level = level->valueint;
            if (cJSON_IsNumber(funds)) client->acct.funds = funds->valueint;
            if (client->acct.level != old_level) acc_broadcast_info(client);
        }
    }
}

static void acc_handle_drain(const cJSON *frame) {
    const char *reason = cJSON_GetStringValue(cJSON_GetObjectItem(frame, "reason"));
    if (g_acc.draining) return;
    g_acc.draining = 1;
    server_log("[brain] drain requested (%s): finishing the round, refusing new players", reason ? reason : "-");
    net_broadcast_chat(NULL, "Server is restarting after this round");
    acc_push_lobby_now();
}

void accounts_begin_drain(const char *why) {
    if (g_acc.draining) return;
    g_acc.draining = 1;
    server_log("Draining (%s): refusing new players, finishing the round", why ? why : "-");
    net_broadcast_chat(NULL, "Server is shutting down after this round");
    if (brain_linked()) { // the coordinator stops offering this lobby at once
        cJSON *drain = cJSON_CreateObject();
        cJSON_AddStringToObject(drain, "type", "drain");
        cJSON_AddStringToObject(drain, "reason", why ? why : "shutdown");
        brain_post(drain, 0);
    }
    acc_push_lobby_now();
}

int accounts_drained(void) {
    return !acc_player_count() || g_acc.round_end_seen;
}

static void acc_handle_frame(cJSON *frame) {
    const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(frame, "type"));
    if (!type) return;
    if (!strcmp(type, "auth-reply")) acc_handle_auth_reply(frame);
    else if (!strcmp(type, "commit-reply")) acc_handle_commit_reply(frame);
    else if (!strcmp(type, "host")) acc_handle_host(frame);
    else if (!strcmp(type, "drain")) acc_handle_drain(frame);
    else if (!strcmp(type, "rcon")) rcon_handle_brain_frame(frame);
    else if (!strcmp(type, "ranked-result-reply")) acc_handle_ranked_reply(frame);
    else if (!strcmp(type, "map-data")) acc_handle_map_data(frame);
    else if (!strcmp(type, "assign")) {
        // best-effort heads-up; the ticket itself is verified on JOIN
        const char *jti = cJSON_GetStringValue(cJSON_GetObjectItem(frame, "jti"));
        server_log("[brain] assignment %s for lobby %s", jti ? jti : "?", acc_str(cJSON_GetObjectItem(frame, "lobbyId"), "?"));
    } else if (!strcmp(type, "acct-r") && ks_storage_on_brain_frame(frame)) {
        // WP6: GAME.STORAGE / Save / Load Data / KR rewards (server/src/ks_storage.c)
    } else if (!strcmp(type, "acct-r") || !strcmp(type, "host-request-reply")) {
        // no game-socket account ops / host requests on the native protocol yet
    }
}

static void acc_link_down(void) {
    ks_storage_link_down(); // their replies never come
    // pending auths will never be answered: ticket holders keep the ticket identity,
    // session-only joins become guests (or are refused)
    for (size_t i = 0; i < g_server->client_count; i++) {
        ClientConnection *client = &g_server->clients[i];
        if (client->acct.join_state != SERVER_JOIN_AUTH || !client->acct.auth_rid) continue;
        if (client->acct.central_pending) acc_become_guest(client, "coordinator link lost");
        else if (client->acct.account_id > 0) acc_set_ready(client);
        else acc_become_guest(client, "account service unavailable");
    }
}

// ------------------------------------------------------------------ lifecycle

void accounts_init(void) {
    static const char alphabet[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    char rnd[16];
    brain_random_hex(rnd, sizeof(rnd), 5);
    for (int i = 0; i < 5; i++) {
        const unsigned v = (unsigned) strtoul((char[]) {rnd[i * 2], rnd[i * 2 + 1], 0}, NULL, 16);
        g_acc.lobby_id[i] = alphabet[v % (sizeof(alphabet) - 1)];
    }
    g_acc.lobby_id[5] = 0;
    brain_random_hex(g_acc.server_nonce, sizeof(g_acc.server_nonce), AUTH_JOIN_NONCE_LEN / 2);
    if (g_gs_config.online_mode)
        server_log("Online mode: players are verified by %s%s", g_gs_config.brain_url[0] ? "the coordinator + " : "", g_gs_config.auth_url);

    acc_load_bans();
    g_acc.brain = brain_start(&g_gs_config);
    accounts_round_changed(); // first round: lobby size / round length / match id
    if (g_acc.brain) {
        extern void brain_set_hello_and_run(cJSON *hello);
        server_log("[brain] linking to %s as %s:%s (region %s)", g_gs_config.brain_url, g_gs_config.node_id, g_acc.lobby_id, g_gs_config.region);
        brain_set_hello_and_run(acc_hello());
        g_acc.brain = brain_enabled();
    }
}

void accounts_tick(const float now) {
    if (now >= g_acc.next_ban_check) {
        g_acc.next_ban_check = now + 1.0f;
        acc_save_bans_if_changed();
    }
    CentralAuthResult central;
    while (central_auth_poll(&central)) {
        acc_handle_central(&central);
        cJSON_Delete(central.reply);
    }
    if (!g_acc.brain) {
        // standalone online_mode / password: the same JOIN deadline
        for (size_t i = 0; i < g_server->client_count; i++) {
            ClientConnection *client = &g_server->clients[i];
            if (client->acct.join_state == SERVER_JOIN_READY || now < client->acct.join_deadline) continue;
            acc_become_guest(client, "no JOIN");
        }
        return;
    }

    BrainEvent ev;
    while (brain_next_event(&ev)) {
        if (ev.kind == BRAIN_EV_LINK_UP) {
            // community name of this network (coordinator config), brainId as a fallback
            const char *community = cJSON_GetStringValue(cJSON_GetObjectItem(ev.frame, "community"));
            if (!community || !community[0]) community = cJSON_GetStringValue(cJSON_GetObjectItem(ev.frame, "communityName"));
            if (!community || !community[0]) community = cJSON_GetStringValue(cJSON_GetObjectItem(ev.frame, "brainId"));
            snprintf(g_community, sizeof(g_community), "%s", community ? community : "");
            acc_send_snapshot();
        }
        else if (ev.kind == BRAIN_EV_LINK_DOWN) acc_link_down();
        else if (ev.kind == BRAIN_EV_FRAME) acc_handle_frame(ev.frame);
        cJSON_Delete(ev.frame);
    }

    // join timeouts (no JOIN / no auth-reply in time)
    for (size_t i = 0; i < g_server->client_count; i++) {
        ClientConnection *client = &g_server->clients[i];
        if (client->acct.join_state == SERVER_JOIN_READY || now < client->acct.join_deadline) continue;
        if (client->acct.join_state == SERVER_JOIN_AWAIT) acc_become_guest(client, "no JOIN");
        else if (client->acct.central_pending) acc_become_guest(client, "hasJoined timed out");
        else if (client->acct.auth_rid && client->acct.account_id > 0) acc_set_ready(client);
        else if (client->acct.auth_rid) acc_become_guest(client, "account lookup timed out");
    }

    if (g_acc.round_end_seen && !g_acc.round_committed && g_server->game.end_timer > 0.0f) acc_commit_round();
    acc_tick_ranked();
    acc_tick_custom();

    if (g_acc.push_at && now >= g_acc.push_at) acc_push_lobby_now();
    if (now >= g_acc.next_snapshot) {
        g_acc.next_snapshot = now + ACC_SNAPSHOT_INTERVAL;
        acc_send_snapshot();
    }
}

void accounts_shutdown(void) {
    acc_save_bans_if_changed();
    if (!g_acc.brain) return;

    // what everyone played so far, then leave the brain's lists
    for (size_t i = 0; i < g_server->client_count; i++) acc_commit_partial(&g_server->clients[i]);
    if (brain_linked()) {
        cJSON *drain = cJSON_CreateObject();
        cJSON_AddStringToObject(drain, "type", "drain");
        cJSON_AddStringToObject(drain, "reason", "shutdown");
        brain_post(drain, 0);
        cJSON *down = cJSON_CreateObject();
        cJSON_AddStringToObject(down, "type", "lobby-down");
        cJSON_AddStringToObject(down, "lobbyId", g_acc.lobby_id);
        brain_post(down, 0);
    }
}
