// Singleplayer / offline play (client_bin --offline, web ?offline=1, Host Game without a
// server, console `offline` / `map`).
//
// client->game is the one authoritative Game (game.is_local): the main thread simulates it
// in client_tick_game() exactly as it simulates any client game, and the server-side rules
// (spawn rules, damage / kills / score / streaks, the round clock / end screen / map and mode
// rotation, modes, chat commands, projectiles) run in shared/ as they do on server_bin.
//
//   server -> client: everything the shared game would send to a connection leaves through
//     GameHooks.send and goes straight into the client's event handlers
//     (client_handle_game_event, net/game_events.c) as the struct itself.
//   client -> server: client_net_send() hands each message to local_game_receive() as a
//     function call; the local player's input is queued into the game directly
//     (client_tick_game), so it is never sent, acknowledged or reconciled.
//
// No thread, packet queue, second map load, ping simulation or input acks. Krunker v3.1.1
// runs its single player the same way (index.js aW.singlePlayer: the client's own game,
// b3.send dropped, the spawn / leaderboard / kill handlers called directly).
#include <client.h>
#include <chat_commands.h>
#include <bots.h>
#include <social.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------- server -> client

// GameHooks.send: one connection (ours), so every event for our uid, for everyone, for
// everyone but another uid, or (without a player in the game) for non-players reaches it.
static void local_game_send(Game *game, const int uid, const int type, const void *payload, const uint16_t length) {
    Client *client = game->hooks.user;
    const int me = client->offline_uid;
    const int deliver = uid == NET_TARGET_ALL || uid == me ||
        (uid == NET_TARGET_NON_PLAYERS && !game_find_player(game, me)) ||
        (uid <= NET_TARGET_EXCEPT(0) && NET_TARGET_EXCEPT_UID(uid) != me);
    if (!deliver) return;

    client_net_log_packet("<=", (NetPacketType) type, payload, length); // KRUNKNATIVE_PACKET_LOG=1
    client_handle_game_event(client, type, payload, length);
}

// GameHooks.player_changing: a mode swaps a loadout / outfit in place. This game is also the
// render state, so the meshes built for the old loadout go first and come back after.
static void local_game_player_changing(const Game *game, Player *player, const int done) {
    static Player *rebuilding; // set between the two calls (never nested)
    Client *client = game->hooks.user;

    if (!done) {
        rebuilding = player->mesh ? player : NULL;
        if (rebuilding) client_drop_player_mesh(client, player);
        return;
    }

    if (rebuilding != player) return;
    rebuilding = NULL;
    if (player->active) client_add_player_mesh(client, player, player == client->me);
}

// GameHooks.bot_spawning / bot_removing: v7.1.4 bot players (shared/src/bots.c) live in this
// game, which is also the render state; their SPAWN / LEAVE only reach remote clients, so
// their meshes are built and dropped here, like any other player's (no AI render path).
static void local_game_bot_spawning(Game *game, Player *player, const int done) {
    Client *client = game->hooks.user;
    if (!done) client_drop_player_mesh(client, player);
    else if (player->active) client_player_spawned(client, player, 0);
}

static void local_game_bot_removing(Game *game, Player *player) {
    client_drop_player_mesh(game->hooks.user, player);
}

// ---------------------------------------------------------------- session

static Player *local_game_player(Client *client) {
    return game_find_player(&client->game, client->offline_uid);
}

// The rounds' mode list: just `mode_id` (Host Game / KRUNKNATIVE_MODE), else Free for All.
static void local_game_set_mode(Game *game, const int mode_id) {
    int *modes = calloc(1, sizeof(int));
    if (!modes) return;

    modes[0] = mode_config(mode_id) ? mode_id : MODE_ID_FFA;
    free(game->modes);
    game->modes = modes;
    game->mode_count = 1;
}

// What a server's INIT does on the client: drop the round, (re)load the map, fresh clock.
static void local_game_load(Client *client, const int map_index, const int mode_index) {
    if (client->map_loaded) client_unload_map(client);
    client_clear_players(client);
    client_game_events_reset(client);

    game_init(&client->game, map_index, mode_index, 1); // is_local: this game is the authority
    client_load_map(client);
    client->map_loaded = client->game.ready;
    client->offline_joined = 0;
}

// player.sprayIndex from the last ENT (applyEntData ent[1]): kept across respawns and map changes.
static int g_local_spray_index;
// player.skins / meleeIndex from the last ENT (ent[2], ent[9]): offline every skin is owned.
static PlayerSkins g_local_skins = {-1, -1, -1};

// io_ent -> players.add / spawn on the authority, then what SPAWN does on the client.
static void local_game_spawn(Client *client, const int class_index, const int secondary_index, const PlayerCosmetics cosmetics, const int slid_cont) {
    Game *game = &client->game;
    Player *player = local_game_player(client);
    const int first_spawn = !player;

    if (!player) {
        player = player_init(game);
        if (!player) return;

        player->uid = client->offline_uid;
        player->is_you = 1;
        snprintf(player->name, sizeof(player->name), "Guest_%d", player->uid + 1);
        game_players_add(game, player); // players.add: team / lives (mode)
    }

    client_drop_player_mesh(client, player); // respawn: fresh loadout, timers and meshes
    player->class_index = class_index;
    player->secondary_index = secondary_index;
    player->cosmetics = cosmetics;
    player->slid_cont = slid_cont; // gameserver.ts: player.slidCont = ent[15]
    player->spray_index = g_local_spray_index; // applyEntData: player.sprayIndex = ent[1]
    player->skins = g_local_skins;             // applyEntData: player.skins = ent[2], meleeIndex = ent[9]
    player_spawn(player); // setClass, spawn point, the mode's loadout / outfit
    client->offline_joined = 1;
    client_player_spawned(client, player, 1);

    if (first_spawn) {
        // io_ent first spawn: leaderboard + round clock (+ mode state) for the joiner
        game_sync_leaders(game, player->uid);
        game_send_round_state(game, player->uid);
    }
}

// Reload on another map / mode (rotation, ` cycle, Host Game, chat /map). The local player
// comes back when it is alive (cycle) or took part in the round (`respawn_joined`).
static void local_game_change(Client *client, const int map_index, const int mode_index, const int respawn_joined) {
    if (!client->game.map_count) return;

    const Player *player = local_game_player(client);
    const int respawn = player && (player->active || (respawn_joined && client->offline_joined));
    const int class_index = player ? player->class_index : 0;
    const int secondary_index = player ? player->secondary_index : -1;
    const PlayerCosmetics cosmetics = player ? player->cosmetics : PLAYER_COSMETICS_NONE;
    const int slid_cont = player ? player->slid_cont : 1;

    local_game_load(client, map_index, mode_index);
    if (respawn && client->game.ready) local_game_spawn(client, class_index, secondary_index, cosmetics, slid_cont);
}

// GameHooks.next_round: the end screen ran out (pickWinningMap without votes).
static void local_game_next_round(Game *game) {
    local_game_change(game->hooks.user, game_pick_next_map(game), game_pick_next_mode(game), 1);
}

// GameHooks.respawn (forceSpawn): the local player comes back with its last class.
static void local_game_respawn(Game *game, const int uid) {
    Client *client = game->hooks.user;
    const Player *player = local_game_player(client);
    if (!player || player->uid != uid || player->active || game->end_timer > 0.0f) return;
    local_game_spawn(client, player->class_index, player->secondary_index, player->cosmetics, player->slid_cont);
}

void local_game_host(Client *client, const int map_index, const int mode_id) {
    Game *game = &client->game;
    if (!client->offline || !game->map_count || !mode_config(mode_id)) return;

    local_game_set_mode(game, mode_id); // config.modes = [id]: later rounds keep it
    const int map = map_index >= 0 && (size_t) map_index < game->map_count ? map_index : game->current_map_index;
    local_game_change(client, map, 0, 1);
}

// ---------------------------------------------------------------- client -> server

// ENT: the server's spawn rules (shared with server_bin). Spectating never gets here:
// client_enter_game() spectates locally offline, like v3.1.1's single player.
static void local_game_ent(Client *client, const void *payload, const uint16_t length) {
    NetEntPacket ent = {.secondary_index = -1, .cosmetics = PLAYER_COSMETICS_NONE, .slid_cont = 1};
    memcpy(&ent, payload, length < sizeof(ent) ? length : sizeof(ent)); // the client's own struct, maybe without its tail
    if (ent.spectate) return;
    g_local_spray_index = length >= NET_ENT_PACKET_V5_SIZE ? ent.spray_index : 0;
    g_local_skins = length >= NET_ENT_PACKET_V6_SIZE ? ent.skins : PLAYER_SKINS_NONE;
    const int late = game_end_late_spawn(&client->game, local_game_player(client)); // io_ent in the vote window
    if (!late && !game_can_spawn(&client->game, local_game_player(client))) return;

    local_game_spawn(client, ent.class_index, ent.secondary_index, ent.cosmetics, ent.slid_cont ? 1 : 0);
    if (late) game_send_end_late(&client->game, client->offline_uid); // then "end" with didWin -1
}

// Krunker v3.1.1 "/" commands (shared/src/chat_commands.c) for the one local player, who
// hosts the game: every command is allowed and only it can be the target.
static int local_chat_peer_count(void *user) {
    (void) user;
    return 1;
}

static int local_chat_peer_at(void *user, const int index, ChatPeer *out) {
    Client *client = user;
    if (index != 0) return 0;

    Player *player = local_game_player(client);
    *out = (ChatPeer) {
        .id = client->offline_uid,
        .name = player ? player->name : "Guest_1",
        .address = "",
        .ping_ms = 0, // no network
        .player = player,
        .dev = 1,
    };
    return 1;
}

static void local_chat_change_map(void *user, const int map_index, const int mode_index) {
    local_game_change(user, map_index, mode_index, 0);
}

static ChatCommandHost *local_chat_host(Client *client) {
    static ChatCommandHost host;
    host.game = &client->game;
    host.user = client;
    host.lobby = "offline";
    host.region = "local";
    host.tick_rate = 0.0f; // no server ticks: the game runs every frame
    host.peer_count = local_chat_peer_count;
    host.peer_at = local_chat_peer_at;
    host.change_map = local_chat_change_map;
    return &host;
}

// io_ct for the single local player: a "/" command, else the line comes straight back as "ch".
static void local_game_chat(Client *client, const void *payload, const uint16_t length) {
    char text[NET_CHAT_TEXT_SIZE];
    if (!net_chat_sanitize(text, sizeof(text), payload, length)) return;
    if (chat_command_handle(local_chat_host(client), client->offline_uid, text)) return;

    const Player *player = local_game_player(client);
    NetChatPacket chat;
    net_packet_chat(&chat, client->offline_uid, NET_CHAT_PLAYER, player ? player->name : "Guest_1", text);
    game_send(&client->game, NET_TARGET_ALL, NET_PACKET_CHAT, &chat, sizeof(chat));
}

// Social hub replies keep their chunked frames: the hub page (client/src/ui/social_ui.c)
// pulls NET_PACKET_SOCIAL_REPLY out of client->net_packets itself while the game is frozen.
static int local_game_social_reply(void *user, const NetPacketType type, const void *payload, const uint16_t length) {
    NetPacket packet = {.type = (uint8_t) type, .length = length};
    if (length > NET_MAX_PAYLOAD) return 0;
    memcpy(packet.payload, payload, length);
    client_net_queue_packet(user, &packet);
    return 1;
}

// Social hub requests (shared/include/social.h), answered like server_bin from our map list.
static void local_game_social(Client *client, const void *payload, const uint16_t length) {
    static SocialAssembly assembly; // one client
    char *request = NULL;
    uint32_t id = 0;
    if (social_assembly_push(&assembly, payload, length, &request, &id) != 1) return;

    SocialMapInfo maps[64];
    const SocialHost host = {.maps = maps, .map_count = social_maps_from_game(&client->game, maps, 64)};
    char *reply = social_service_handle(&host, request);
    free(request);
    if (!reply) return;

    social_send_message(NET_PACKET_SOCIAL_REPLY, id, reply, strlen(reply), local_game_social_reply, client);
    free(reply);
}

// What server_bin does with each client message (server/src/net.c net_handle_packet), as
// direct calls. Add new client -> server messages here (relays answer through game_send).
void local_game_receive(Client *client, const NetPacketType type, const void *payload, const uint16_t length) {
    if (!client->offline) return;
    client_net_log_packet("=>", type, payload, length);

    switch (type) {
        case NET_PACKET_ENT:
            if (payload && length) local_game_ent(client, payload, length);
            break;
        case NET_PACKET_CYCLE: // the ` debug key: next map, same mode
            if (!length && client->game.map_count) {
                local_game_change(client, (client->game.current_map_index + 1) % (int) client->game.map_count, client->game.current_mode_index, 0);
            }
            break;
        case NET_PACKET_CHAT_SEND:
            if (payload && length && length <= sizeof(NetChatSendPacket)) local_game_chat(client, payload, length);
            break;
        case NET_PACKET_SOCIAL_REQUEST:
            if (payload && length) local_game_social(client, payload, length);
            break;
        case NET_PACKET_VOICE_SEND: // io_vc relays to every peer but the speaker: alone, nobody hears it
            break;
        case NET_PACKET_INTERACT: { // io_int (console agent: map terminals)
            NetInteractPacket interact = {0};
            if (!payload || !length || length > sizeof(interact)) break;
            memcpy(&interact, payload, length);
            game_player_interact(&client->game, local_game_player(client), interact.action, interact.secondary);
            break;
        }
        case NET_PACKET_PREMIUM_ACTION: // "prmA" (premiumKeys, the N key): the local player hosts, so it may
            if (payload && length == 1) chat_command_premium_action(local_chat_host(client), client->offline_uid, *(const uint8_t *) payload);
            break;
        case NET_PACKET_TELEPORT: { // "tp" (the mod's Ctrl TP): io_tp; the local player is DEV (perms off)
            NetTeleportPacket tp;
            Player *player = local_game_player(client);
            if (!payload || length != sizeof(tp) || !player) break;
            memcpy(&tp, payload, sizeof(tp));
            if (!isfinite(tp.x) || !isfinite(tp.y) || !isfinite(tp.z)) {
                printf("[gameserver] tp invalid coords: %g %g %g\n", tp.x, tp.y, tp.z);
                break;
            }
            const NetTeleportPacket ack = {tp.x, tp.y - NET_TELEPORT_EYE_OFFSET, tp.z};
            net_player_teleport(player, ack.x, ack.y, ack.z);
            local_game_send(&client->game, client->offline_uid, NET_PACKET_TELEPORT_ACK, &ack, sizeof(ack));
            printf("[gameserver] tp %d -> %g %g %g\n", client->offline_uid, ack.x, ack.y, ack.z);
            break;
        }
        case NET_PACKET_CLEAR_LOG: // "clearLog" (the mod's F1): io_clearLog clears the server's terminal = ours
#ifndef __EMSCRIPTEN__ // on the web the in-page server's log is the browser console the client's F1 clears
            if (!length) {
                fputs("\x1B[2J\x1B[3J\x1B[H", stdout);
                fflush(stdout);
            }
#endif
            break;
        case NET_PACKET_TERMINAL_MSG: // "tmc": no handler on the v3.1.1 server (io_default logs it)
            if (payload && length > sizeof(int32_t)) {
                NetTerminalMsgPacket msg = {0};
                memcpy(&msg, payload, length < sizeof(msg) ? length : sizeof(msg));
                msg.text[sizeof(msg.text) - 1] = 0;
                printf("tmc %d,\"%s\"\n", (int) msg.uid, msg.text);
            }
            break;
        default:
            // INPUT is queued straight into the game; PONG / MAP_REQUEST / SPECTATE_TARGET /
            // HELLO have nothing to answer in-process.
            break;
    }
}

// ---------------------------------------------------------------- start / stop

static int local_game_begin(Client *client, const int map_index) {
    Game *game = &client->game;

    // The offline game's mode: client->net_mode_index (a Krunker mode id, Host Game) or
    // KRUNKNATIVE_MODE (alias / name / id) for scripted runs; Free for All by default.
    const char *mode_env = getenv("KRUNKNATIVE_MODE");
    local_game_set_mode(game, mode_env && mode_env[0] ? mode_find(mode_env) : client->net_mode_index);

    game->hooks = (GameHooks) {
        .user = client,
        .send = local_game_send,
        .next_round = local_game_next_round,
        .respawn = local_game_respawn,
        .player_changing = local_game_player_changing,
        .bot_spawning = local_game_bot_spawning,
        .bot_removing = local_game_bot_removing,
    };
    game->end_keep_players = 1; // v3.1.1's client keeps playing the endAnim with its players

    client->offline = 1;
    client->offline_uid = 0;
    client->offline_joined = 0;
    client->net_ping_ms = 0; // nothing to ping (Krunker's #pingText default)

    local_game_load(client, map_index, -1);
    if (!game->ready) {
        fprintf(stderr, "offline: could not load a map\n");
        local_game_stop(client);
        return 0;
    }

    printf("offline: playing map %d, mode %d in-process\n", game->current_map_index, game_mode_id(game));
    return 1;
}

int local_game_start(Client *client) {
    // Host Game's pick (offline_start_map, one load), else the startup map (the first of
    // the client's map list, map_list_with_custom), else a random rotation map, like server_bin.
    int map_index = client->startup_map ? 0 : -1;
    const cJSON *pick = client->offline_start_map;
    client->offline_start_map = NULL;
    for (size_t i = 0; pick && i < client->game.map_count; i++) {
        if (client->game.maps[i] == pick) map_index = (int) i;
    }
    return local_game_begin(client, map_index);
}

// The editor's Test Map: one map slot of the client's list holds our copy of the last
// tested map (replaced by the next test, freed once the game no longer uses it).
int local_game_host_custom(Client *client, const cJSON *map) {
    static int test_slot = -1;
    Game *game = &client->game;
    if (!cJSON_IsObject(map)) return 0;

    cJSON *copy = cJSON_Duplicate(map, 1);
    if (!copy) return 0;

    cJSON *old = NULL;
    if (test_slot >= 0 && (size_t) test_slot < game->map_count) {
        old = (cJSON *) game->maps[test_slot];
        game->maps[test_slot] = copy;
    } else {
        const cJSON **list = realloc((void *) game->maps, (game->map_count + 1) * sizeof(*list));
        if (!list) {
            cJSON_Delete(copy);
            return 0;
        }
        list[game->map_count] = copy;
        game->maps = list;
        test_slot = (int) game->map_count++;
    }

    int ok;
    if (client->offline) {
        const int mode_id = game->ready && game_mode_id(game) >= 0 ? game_mode_id(game) : MODE_ID_FFA;
        local_game_host(client, test_slot, mode_id);
        ok = game->ready && game->current_map_index == test_slot;
    } else {
        // leave a remote session first, like the console's `offline`
        client_net_disconnect(client, "Offline", "000000");
        client->net_should_quit = 1;
        ok = local_game_begin(client, test_slot);
    }

    if (old && (!game->map || game->map->raw_data != old)) cJSON_Delete(old); // the Map built from it is gone
    return ok;
}

void local_game_stop(Client *client) {
    if (!client->offline) return;

    client->offline = 0;
    client->net_ping_ms = -1;
    game_bots_free(&client->game); // the offline lobby's bots end with it (their players go with the next INIT)
    client->game.hooks = (GameHooks) {0};
    client->game.is_local = 0; // a plain mirror until the next INIT
    client->game.end_keep_players = 0;
}
