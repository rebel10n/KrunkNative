// Match events from the server (KILL/HIT/HEALTH/SCORE/LEADERBOARD/TIMER/END/
// CHAT/PING) applied to the client's game mirror and pushed into the HTML HUD
// through its public API (client/include/ui_logic.h). Krunker v3.1.1 reference:
// the client io handlers in index.js (g1 = "3", gF = "4", gx = "h", gf = "5",
// g5 = "7", fD = "t", fq = "end", gB = "ch", fK = "pi").
//
// Runs on the main thread from client_tick_net(); the transports only queue packets.
#include <player_accounts.h> // PLAYER_INFO / BILLBOARD (doc/backend.md section 8)
#include <ks_client.h> // KrunkScript client host: hooks and script packets
#include <backend_ui.h>      // NET_PACKET_REDIRECT: servers_ui_redirect
#include <online_mode.h>     // NET_PACKET_AUTH_CHALLENGE: online-mode join (net/online_mode.c)
#include <client.h>
#include <hud_modes.h>
#include <html_ui.h>
#include <ui_logic.h>
#include <sound_events.h>
#include <vfx.h>
#include <projectiles_render.h>
#include <ai.h>        // NET_PACKET_AI_ADD / AI_SYNC: the mirrored Game.ai list
#include <ai_render.h> // AI shots ("pr" with an AI stored kind)
#include <camera_feel.h>
#include <spectate.h> // NET_PACKET_SPECTATE -> spectate_begin / spectate_end
#include <menu_view.h> // g1(): CONTROLS.camLookAt(killer) death chase camera
#include <console.h>   // chat lines mirrored into the developer console (ui_console.c)
#include <ui_terminal.h> // NET_PACKET_TERMINAL: openTerminal (ui_terminal.c)
#include <sprays.h>      // NET_PACKET_SPRAY: addSpray (sprays.c)
#include <chat_commands.h> // NET_PACKET_CHEAT: /forcecol on the client's map copy
#include <voice_chat.h>    // NET_PACKET_VOICE: "vc" -> voiceChat (client/src/voice.c)
#include <map_triggers.h>  // map trigger events: "do"/"ro"/"gte"/"bnk", "chp", "s", "scr", "am" popups (client/src/map_triggers.c)
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SERVER_CHAT_COLOR "#fc03ec" // gB type 2 default

void client_net_send(Client *client, const NetPacketType type, const void *payload, const uint16_t length) {
    if (client->offline) {
        client_offline_send(client, type, payload, length); // the sandbox server's connection (net/offline.c)
        return;
    }

    pthread_mutex_lock(&client->net_lock);
    const int socket = client->net_socket;
    pthread_mutex_unlock(&client->net_lock);

    if (socket >= 0) client_net_send_packet(socket, type, payload, length);
}

// fp(chatInput.value) -> "ct". The server enforces the same 3 messages / 1 s burst
// limit by disconnecting; a client that keeps under it never gets kicked.
void client_chat_send(Client *client, const char *text) {
    static NetChatLimiter limiter;
    NetChatSendPacket packet;

    if (!client || !text) return;
    if (!net_chat_sanitize(packet.text, sizeof(packet.text), text, strlen(text))) return;
    if (!client->offline && client->net_socket < 0) return;
    if (!net_chat_allow(&limiter, (float) glfwGetTime())) return;

    client_net_send(client, NET_PACKET_CHAT_SEND, &packet, (uint16_t) (strlen(packet.text) + 1));
}

static Player *events_find_player(Client *client, const int uid) {
    return uid < 0 ? NULL : game_find_player(&client->game, uid);
}

static const Weapon *events_weapon(const Client *client, const int index) {
    const size_t count = client->game.weapon_count ? client->game.weapon_count : sizeof(g_weapons) / sizeof(g_weapons[0]);
    const Weapon *const *weapons = client->game.weapons ? (const Weapon *const *) client->game.weapons : (const Weapon *const *) g_weapons;
    return index >= 0 && index < (int) count ? weapons[index] : NULL;
}

static const char *events_name(const Player *player) {
    return player && player->name[0] ? player->name : "unknown";
}

// players.kill(player) on the client: stop simulating and drop the meshes.
static void events_player_dead(Client *client, Player *player) {
    player->active = 0;
    player->input_queue_size = 0;
    free(player->input_queue);
    player->input_queue = NULL;

    if (player->mesh) {
        scene_remove_player_mesh(player->render_you ? client->fps_scene : client->scene, player->mesh, player->loadout_size);
        player_meshes_fini(player);
    }
}

// g1(): kill feed for everyone, death card + respawn delay for the victim.
static void events_kill(Client *client, const NetKillPacket *packet) {
    Player *victim = events_find_player(client, packet->victim_uid);
    Player *killer = events_find_player(client, packet->killer_uid);
    const Weapon *weapon = events_weapon(client, packet->weapon_index);

    if (victim) {
        victim->deaths = (unsigned int) packet->victim_deaths;
        events_player_dead(client, victim);
    }

    if (killer && killer != victim) killer->kills = (unsigned int) packet->killer_kills;
    if (killer && killer != victim && killer == client->me) sound_events_kill(packet->headshot); // gk: kill sound

    const char *victim_name = events_name(victim);

    if (killer) { // g1: b8 found, even when it is the victim (hC = "": weapon + victim only, e.g. own rocket)
        const int self = killer == victim;
        hud_ui_add_killfeed_ex(client, self ? NULL : events_name(killer), !self && killer == client->me, victim_name,
                               victim && victim == client->me, weapon && weapon->icon ? weapon->icon : NULL, packet->headshot, 0.0);
    } else {
#ifdef HUD_UI_HAVE_KILLFEED_SUICIDE
        hud_ui_add_killfeed_suicide(client, victim_name, victim && victim == client->me);
#endif
    }

    if (!victim || victim != client->me) return;

    client_recon_on_death(client); // no movement input leaves until the respawn (net/recon.c)
    HudKillCard card = {0};
    const int by_other = killer && killer != victim;
    client->camera.zoom = 1.0f; // kill(): toggleAim(player, 0) -> resetAim() -> RENDER.zoom(1); hud_ui_show_death the DOM half
    menu_view_death_cam(client, by_other ? killer : NULL); // CONTROLS.camLookAt(killer.x, y + height / 1.5, z)

    if (by_other) {
        card.killer = events_name(killer);
        card.weapon = packet->fists ? "Fists" : weapon && weapon->name ? weapon->name : "";
        const NetPlayerInfoPacket *info = player_accounts_get(killer->uid);
        card.level = info && info->level > 1 ? info->level : 1; // guests: Krunker shows lvl 1 for fresh players
        card.class_index = packet->killer_class >= 0 ? packet->killer_class : 0;
    }
    card.score = packet->score;

    hud_ui_show_death(client, &card);

    client->spawn_requested = 0;
    client->net_death_time = glfwGetTime();
    client->net_respawn_delay = packet->respawn_delay > 0.0f ? packet->respawn_delay : GAME_DEATH_DELAY;
    client->net_death_menu_pending = 1;
}

// gx(): health of a player; the victim's own packet carries the dealer position.
static void events_health(Client *client, const NetHealthPacket *packet) {
    Player *player = events_find_player(client, packet->uid);
    if (!player) return;

    player->health = packet->health;

    if (player != client->me || !packet->has_source) return;

    client->net_damage_from_valid = 1;
    client->net_damage_from_x = packet->source_x;
    client->net_damage_from_z = packet->source_z;
    client->net_damage_from_time = glfwGetTime();
#ifdef HUD_UI_HAVE_DAMAGE_FROM
    hud_ui_damage_from(client, packet->source_x, packet->source_z);
#endif
}

static void events_apply_board(Client *client, const NetLeaderboardPacket *board) {
    const int count = board->count < GAME_LEADERBOARD_MAX ? board->count : GAME_LEADERBOARD_MAX;

    for (int i = 0; i < count; i++) {
        Player *player = events_find_player(client, board->entries[i].uid);
        if (!player) continue;

        player->score = board->entries[i].score;
        player->kills = (unsigned int) board->entries[i].kills;
        player->deaths = (unsigned int) board->entries[i].deaths;
    }
}

static void events_show_end(Client *client) {
    char text[16];
    const int seconds = client->net_end_seconds > 0 ? client->net_end_seconds : 0;
    snprintf(text, sizeof(text), "%02d:%02d", seconds / 60 % 60, seconds % 60);
    hud_ui_show_end_screen(client, client->net_end_result, text);
}

// endGame(): the round is over. Players stay as they are (v3.1.1 keeps them active and
// the scene plays the endAnim slow motion behind the title, menu_view.c); the next
// round's INIT drops them (client_clear_players).
// endGame only shows the end screen when `(player && player.active) || window.spectating`
// (skip is false from every v3.1.1 server path): a menu client or a dead player keeps its
// menu / death card ("CLICK TO PLAY" still enters: the server spawns it and sends a late
// "end", which then shows the title, the slow motion and the results). Until then only the
// "Next Round in" countdown is known (net_end_seconds; net_end_result stays INT32_MIN).
static void events_end(Client *client, const NetEndPacket *packet) {
    client->net_end_seconds = packet->next_round;
    client->net_round_seconds = 0;
    client->spawn_requested = 0;
    events_apply_board(client, &packet->board);
    // endData.ed "reward": player.reward of every board row (0 = "NONE")
    for (int i = 0; i < packet->board.count && i < GAME_LEADERBOARD_MAX; i++) {
        Player *player = events_find_player(client, packet->board.entries[i].uid);
        if (player) player->reward = packet->rewards[i];
    }
    if (!(client->me && client->me->active) && !spectate_active()) return;

    client->net_end_result = packet->result;
    client->net_death_menu_pending = 0;
    events_show_end(client);
}

static void events_timer(Client *client, const NetTimerPacket *packet) {
    if (packet->end) {
        client->net_end_seconds = packet->seconds;
        if (client->net_end_result != INT32_MIN) events_show_end(client);
        return;
    }

    client->net_round_seconds = packet->seconds;
}

static void events_chat(const NetChatPacket *packet) {
    char name[PLAYER_NAME_SIZE];
    char text[NET_CHAT_TEXT_SIZE];

    memcpy(name, packet->name, sizeof(name));
    memcpy(text, packet->text, sizeof(text));
    name[sizeof(name) - 1] = 0;
    text[sizeof(text) - 1] = 0;

    ui_console_on_chat(packet->kind == NET_CHAT_PLAYER ? (name[0] ? name : "unknown") : NULL, text, packet->kind);

    switch (packet->kind) {
        case NET_CHAT_PLAYER:
            hud_ui_add_chat(name[0] ? name : "unknown", text, NULL);
            break;
        case NET_CHAT_SERVER:
            hud_ui_add_chat(NULL, text, SERVER_CHAT_COLOR);
            break;
        default:
            hud_ui_add_chat(NULL, text, NULL);
            break;
    }
}

// updateNoclip / updateUnlimitedAmmo / updateForceHide ("noc" / "unl" / "fh": the line is
// addChat(null, ..., 2)) and the "borders" / "forcecol" broadcasts of the chat commands.
static void events_cheat(Client *client, const NetCheatPacket *packet) {
    const int on = !!packet->on;
    const char *line = NULL;
    switch (packet->flag) {
        case NET_CHEAT_NOCLIP:
            if (!client->me) return;
            client->me->noclip = (unsigned char) on;
            line = on ? "Noclip - Enabled" : "Noclip - Disabled";
            break;
        case NET_CHEAT_UNLIMITED_AMMO:
            if (!client->me) return;
            client->me->unlimited_ammo = (unsigned char) on;
            if (on) client->me->can_throw = 1;
            line = on ? "Unlimited Ammo - Enabled" : "Unlimited Ammo - Disabled";
            break;
        case NET_CHEAT_FORCE_HIDE:
            if (!client->me) return;
            client->me->force_hide = (unsigned char) on;
            line = on ? "Hide - Enabled" : "Hide - Disabled";
            break;
        case NET_CHEAT_BORDERS:
            client->game.config.disable_borders = (unsigned int) !on; // movement prediction
            break;
        case NET_CHEAT_FORCE_COLLIDE:
            if (client->game.ready) chat_map_force_collide(client->game.map, on);
            break;
        default:
            break;
    }
    if (line) {
        hud_ui_add_chat(NULL, line, SERVER_CHAT_COLOR);
        ui_console_on_cheat(packet->flag, on); // devConsole hookChat: "Noclip - Enabled", "Force Hide - ..."
    }
}

// g7 ("9"): another player's hitscan ray: tracer/impact and (once per shot) the gun sound.
static void events_shot(Client *client, const NetShotPacket *packet) {
    Player *shooter = events_find_player(client, packet->uid);
    if (!shooter || shooter == client->me) return;

    const vec3 hit = {packet->hit[0], packet->hit[1], packet->hit[2]};
    vfx_remote_shot(client, shooter, hit, packet->has_normal, packet->normal_yaw, packet->normal_pitch, packet->trail);
    if (packet->first) sound_events_remote_shot(client, shooter);
}

// "pr": a projectile in flight (rockets, bolts, blaster shots, thrown knives).
static void events_projectile(Client *client, const NetProjectilePacket *packet) {
    if (packet->id < 0 || packet->id >= GAME_MAX_PROJECTILES) return;

    ClientProjectile *projectile = &client->net_projectiles[packet->id];
    projectile->active = 1;
    projectile->owner_uid = packet->owner_uid;
    projectile->weapon_index = packet->weapon_index;
    projectile->position = (vec3) {packet->position[0], packet->position[1], packet->position[2]};
    projectile->velocity = (vec3) {packet->velocity[0], packet->velocity[1], packet->velocity[2]};
    projectile->gravity = packet->gravity;
    projectile->range = packet->range;

    if (packet->ai_kind) { // ai.js shot: physObj "orb_0" with projectiles.stored[kind]
        ai_render_projectile(client, packet);
        return;
    }
    projectiles_render_server_spawn(client, packet); // g9: physObj (rocket box, bolt, blaster shot, thrown knife)
}

// Projectile.update on the client: same flight model as shared/src/projectile.c.
static void events_projectiles_step(Client *client) {
    const double now = glfwGetTime();
    const float dt_ms = client->net_projectiles_time > 0.0 ? (float) (now - client->net_projectiles_time) * 1000.0f : 0.0f;
    client->net_projectiles_time = now;
    if (dt_ms <= 0.0f || dt_ms > 1000.0f) return;

    for (int i = 0; i < GAME_MAX_PROJECTILES; i++) {
        ClientProjectile *projectile = &client->net_projectiles[i];
        if (!projectile->active) continue;

        projectile->velocity.y -= projectile->gravity * dt_ms;
        projectile->position.x += projectile->velocity.x * dt_ms;
        projectile->position.y += projectile->velocity.y * dt_ms;
        projectile->position.z += projectile->velocity.z * dt_ms;

        const float speed = sqrtf(projectile->velocity.x * projectile->velocity.x + projectile->velocity.y * projectile->velocity.y +
                                  projectile->velocity.z * projectile->velocity.z);
        projectile->range -= speed * dt_ms;
        if (projectile->range <= 0.0f) projectile->active = 0; // the server's PROJECTILE_END follows
    }
}

// ---------------------------------------------------------------- maps
// INIT carries the server's map index, which only means something in the server's
// map list. MAP_INFO (sent right before INIT) names the map by hash: the client
// picks its own copy, a map streamed earlier, or downloads the JSON (MAP_REQUEST ->
// MAP_DATA chunks, at most NET_MAP_MAX_SIZE) before it applies the INIT.

#define MAP_WAIT_TIMEOUT 20.0   // seconds before a stalled download falls back to the server index
#define MAP_HELD_MAX 8192       // packets kept while downloading (states beyond this are dropped)

static int map_list_find(const Client *client, const uint32_t hash) {
    for (size_t i = 0; i < client->game.map_count; i++) {
        if (client->game.maps[i] && net_map_hash(client->game.maps[i], NULL, NULL) == hash) return (int) i;
    }
    return -1;
}

static int map_list_add(Client *client, const cJSON *map) {
    const cJSON **list = realloc((void *) client->game.maps, (client->game.map_count + 1) * sizeof(*list));
    if (!list) return -1;

    list[client->game.map_count] = map;
    client->game.maps = list;
    return (int) client->game.map_count++;
}

static void map_download_finish(Client *client, const int map_index) {
    free(client->net_map_data);
    client->net_map_data = NULL;
    client->net_map_received = 0;
    client->net_map_wait = 0;
    client->net_map_ready = 1;
    if (map_index >= 0) client->net_map_init.map_index = map_index;
}

int client_map_prepare_init(Client *client, NetInitPacket *init) {
    if (!client->net_map_info_valid || client->net_map_info.map_index != init->map_index) return 1; // no MAP_INFO: index as-is
    client->net_map_info_valid = 0;

    const NetMapInfoPacket *info = &client->net_map_info;
    const int index = init->map_index;

    if (index >= 0 && (size_t) index < client->game.map_count && net_map_hash(client->game.maps[index], NULL, NULL) == info->hash) return 1;

    const int found = map_list_find(client, info->hash);
    if (found >= 0) {
        init->map_index = found;
        return 1;
    }

    for (size_t i = 0; i < client->net_map_cache_count; i++) {
        if (net_map_hash(client->net_map_cache[i], NULL, NULL) != info->hash) continue;
        const int added = map_list_add(client, client->net_map_cache[i]);
        if (added >= 0) init->map_index = added;
        return 1;
    }

    if (!info->size || info->size > NET_MAP_MAX_SIZE || !(client->net_map_data = malloc(info->size))) {
        fprintf(stderr, "map %s (%u bytes) cannot be downloaded, using map index %d\n", info->name, info->size, index);
        return 1;
    }

    printf("downloading map %s (%u bytes)\n", info->name, info->size);
    client->net_map_received = 0;
    client->net_map_wait = 1;
    client->net_map_wait_since = glfwGetTime();
    client->net_map_init = *init;

    const NetMapRequestPacket request = {.hash = info->hash};
    client_net_send(client, NET_PACKET_MAP_REQUEST, &request, sizeof(request));
    return 0;
}

int client_map_hold(Client *client, const NetPacket *packet) {
    if (!client->net_map_wait) return 0;

    if (glfwGetTime() - client->net_map_wait_since > MAP_WAIT_TIMEOUT) {
        fprintf(stderr, "map download timed out, using map index %d\n", client->net_map_init.map_index);
        map_download_finish(client, -1);
        return 0;
    }

    switch (packet->type) {
        case NET_PACKET_MAP_DATA:
        case NET_PACKET_PING:
        case NET_PACKET_PING_RESULT:
        case NET_PACKET_CHAT:
            return 0; // not tied to the map
        default:
            break;
    }

    if (client->net_map_held_count >= MAP_HELD_MAX && packet->type == NET_PACKET_STATE) return 1;

    NetPacket *held = realloc(client->net_map_held, (client->net_map_held_count + 1) * sizeof(NetPacket));
    if (!held) return 1;

    client->net_map_held = held;
    client->net_map_held[client->net_map_held_count++] = *packet;
    return 1;
}

int client_map_take_ready(Client *client, NetInitPacket *init, NetPacket **held, size_t *held_count) {
    if (!client->net_map_ready) return 0;

    client->net_map_ready = 0;
    *init = client->net_map_init;
    *held = client->net_map_held;
    *held_count = client->net_map_held_count;
    client->net_map_held = NULL;
    client->net_map_held_count = 0;
    return 1;
}

static void events_map_data(Client *client, const void *payload, const uint16_t length) {
    if (!client->net_map_wait || length < NET_MAP_DATA_HEADER_SIZE || length > sizeof(NetMapDataPacket)) return;

    NetMapDataPacket chunk;
    memcpy(&chunk, payload, length);
    const uint32_t size = client->net_map_info.size;

    if (chunk.hash != client->net_map_info.hash || chunk.length != length - NET_MAP_DATA_HEADER_SIZE ||
        chunk.offset > size || chunk.length > size - chunk.offset) return;

    memcpy(client->net_map_data + chunk.offset, chunk.data, chunk.length);
    client->net_map_received += chunk.length;
    if (client->net_map_received < size) return;

    cJSON *map = cJSON_ParseWithLength(client->net_map_data, size);
    if (!map) {
        fprintf(stderr, "downloaded map %s is not valid JSON, using map index %d\n", client->net_map_info.name, client->net_map_init.map_index);
        map_download_finish(client, -1);
        return;
    }

    if (net_map_hash(map, NULL, NULL) != client->net_map_info.hash) fprintf(stderr, "downloaded map %s: hash mismatch (kept)\n", client->net_map_info.name);

    cJSON **cache = realloc(client->net_map_cache, (client->net_map_cache_count + 1) * sizeof(cJSON *));
    if (cache) {
        client->net_map_cache = cache;
        client->net_map_cache[client->net_map_cache_count++] = map;
    }

    printf("downloaded map %s\n", client->net_map_info.name);
    map_download_finish(client, map_list_add(client, map));
}

#define EVENTS_READ(type, var) \
    type var; \
    if (length != sizeof(type)) return 1; \
    memcpy(&var, payload, sizeof(var))

int client_handle_game_packet(Client *client, const NetPacket *packet) {
    return client_handle_game_event(client, packet->type, packet->payload, packet->length);
}

// One server event as its struct, from a received packet above (the offline sandbox
// server's arrive the same way, net/offline.c).
int client_handle_game_event(Client *client, const int type, const void *payload, const uint16_t length) {
    switch (type) {
        case NET_PACKET_CHAT: {
            EVENTS_READ(NetChatPacket, chat);
            events_chat(&chat);
            return 1;
        }
        case NET_PACKET_KILL: {
            EVENTS_READ(NetKillPacket, kill);
            if (client->game.ready) {
                events_kill(client, &kill);
                // KrunkScript onPlayerDeath(id, killerID or ""), index.js:15238-15240
                ks_client_player_killed(client, kill.victim_uid, events_find_player(client, kill.killer_uid) ? kill.killer_uid : -1);
            }
            return 1;
        }
        case NET_PACKET_SCRIPT_MSG:  // KrunkScript (client/src/ks/ks_client_host.c): onNetworkMessage
        case NET_PACKET_SCRIPT_LOG:  // the server script's GAME.log / errors
        case NET_PACKET_SCRIPT_SYNC: // registerSyncValues
            return ks_client_handle_packet(client, type, payload, length);
        case NET_PACKET_SCRIPT_CHAT: { // GAME.CHAT.send / broadcast: a server chat line (its colour is not shown)
            EVENTS_READ(NetScriptChatPacket, script_chat);
            events_chat(&script_chat.chat);
            return 1;
        }
        case NET_PACKET_HIT: {
            EVENTS_READ(NetHitPacket, hit);
            sound_events_hitmarker();     // gF: hit_0
            hud_ui_hit(client, hit.kill); // gF -> aV.showHit(isKill)
#ifdef HUD_UI_HAVE_DAMAGE_NUMBERS
            if (hit.ai) { // doDamage(.., isAI): Game.AI.findBySid -> dmgVal(ai.x, ai.y, ai.z)
                const GameAI *ai = game_ai_find(&client->game, hit.target_uid);
                if (ai && hit.damage) hud_ui_damage_number(client, ai->position.x, ai->position.y, ai->position.z, hit.damage, hit.crit);
                return 1;
            }
            const Player *target = events_find_player(client, hit.target_uid);
            if (target && hit.damage) hud_ui_damage_number(client, target->position.x, target->position.y, target->position.z, hit.damage, hit.crit); // aV.NJ2
#endif
            return 1;
        }
        case NET_PACKET_AI_ADD: { // "aai": AI.cSpawn (the in-process game owns its list already)
            EVENTS_READ(NetAIAddPacket, add);
            if (!game_is_authority(&client->game)) game_ai_apply_add(&client->game, &add);
            return 1;
        }
        case NET_PACKET_AI_SYNC: { // "ai": AI.syncAll
            NetAISyncPacket sync;
            memset(&sync, 0, sizeof(sync));
            if (length < offsetof(NetAISyncPacket, entries) || length > sizeof(sync)) return 1;
            memcpy(&sync, payload, length);
            if (sync.count > NET_AI_SYNC_MAX || length < offsetof(NetAISyncPacket, entries) + sync.count * sizeof(NetAISyncEntry)) return 1;
            if (!game_is_authority(&client->game)) game_ai_apply_sync(&client->game, &sync);
            return 1;
        }
        case NET_PACKET_SHOT: {
            EVENTS_READ(NetShotPacket, shot);
            events_shot(client, &shot);
            return 1;
        }
        case NET_PACKET_PROJECTILE: {
            EVENTS_READ(NetProjectilePacket, projectile);
            events_projectile(client, &projectile);
            return 1;
        }
        case NET_PACKET_PROJECTILE_END: {
            EVENTS_READ(NetProjectileEndPacket, end);
            if (end.id >= 0 && end.id < GAME_MAX_PROJECTILES) client->net_projectiles[end.id].active = 0;
            projectiles_render_server_end(client, &end); // g8: disablePhys / knife linger, impact effect
            ai_render_projectile_end(client, &end);      // an AI orb with that sid
            return 1;
        }
        case NET_PACKET_EXPLOSION: {
            EVENTS_READ(NetExplosionPacket, explosion);
            const vec3 position = {explosion.position[0], explosion.position[1], explosion.position[2]};
            vfx_explosion(client, position, explosion.radius); // gb: explosion effect
            sound_events_explosion(position);
            camera_feel_explosion(&client->camera, position);  // gb: camera shake by distance
            return 1;
        }
        case NET_PACKET_HEALTH: {
            EVENTS_READ(NetHealthPacket, health);
            events_health(client, &health);
            return 1;
        }
        case NET_PACKET_SCORE: {
            EVENTS_READ(NetScorePacket, score);
            if (client->me) client->me->score = score.score;
            hud_popups_score(score.delta); // gf(): the "+N" popup
            return 1;
        }
        case NET_PACKET_KILL_MEDALS: { // "6" -> gk(): the kill's medals into the queue
            EVENTS_READ(NetKillMedalsPacket, medals);
            hud_popups_medals(medals.medals, medals.count < NET_KILL_MEDALS_MAX ? medals.count : NET_KILL_MEDALS_MAX);
            return 1;
        }
        case NET_PACKET_ASSIST: // "10" -> gl()
            hud_popups_assist();
            return 1;
        case NET_PACKET_KILL_STREAK: { // "kst" -> killStreakMessage(sid, streak)
            EVENTS_READ(NetKillStreakPacket, kst);
            hud_ui_kill_streak(client, kst.uid, kst.streak);
            return 1;
        }
        case NET_PACKET_LEADERBOARD: {
            EVENTS_READ(NetLeaderboardPacket, board);
            events_apply_board(client, &board);
            return 1;
        }
        case NET_PACKET_TIMER: {
            EVENTS_READ(NetTimerPacket, timer);
            events_timer(client, &timer);
            return 1;
        }
        case NET_PACKET_END: {
            // the rewards column is optional (an older server sends NET_END_PACKET_V1_SIZE)
            if (length != sizeof(NetEndPacket) && length != NET_END_PACKET_V1_SIZE) return 1;
            NetEndPacket end;
            memset(&end, 0, sizeof(end));
            memcpy(&end, payload, length);
            events_end(client, &end);
            return 1;
        }
        case NET_PACKET_MAP_INFO: {
            EVENTS_READ(NetMapInfoPacket, info);
            info.name[sizeof(info.name) - 1] = 0;
            client->net_map_info = info;
            client->net_map_info_valid = 1;
            return 1;
        }
        case NET_PACKET_MAP_DATA:
            events_map_data(client, payload, length);
            return 1;
        case NET_PACKET_PING: {
            EVENTS_READ(NetPingPacket, ping);
            client_net_send(client, NET_PACKET_PONG, &ping, sizeof(ping)); // fK(): send "po"
            return 1;
        }
        case NET_PACKET_REFILL: // "rfl" -> fY(): b5.refillKnife() (the HUD melee icon follows can_throw)
            if (client->me) client->me->can_throw = 1;
            return 1;
        case NET_PACKET_TERMINAL: {
            EVENTS_READ(NetTerminalPacket, terminal);
            ui_terminal_open(client, terminal.uid); // "ot": openTerminal
            return 1;
        }
        case NET_PACKET_SPRAY: { // "sp": addSpray(sid, sprayIndex, x, y, z, xDr, yDr)
            EVENTS_READ(NetSprayPacket, spray);
            sprays_add(client, &spray);
            return 1;
        }
        case NET_PACKET_CHEAT: {
            EVENTS_READ(NetCheatPacket, cheat);
            events_cheat(client, &cheat);
            return 1;
        }
        case NET_PACKET_TELEPORT_ACK: { // io_tp's "l" ack: our own copy jumps to the new feet position
            EVENTS_READ(NetTeleportPacket, tp);
            if (client->me) net_player_teleport(client->me, tp.x, tp.y, tp.z);
            return 1;
        }
        case NET_PACKET_NOTIFY: { // "notify": the v3.1.1 mod's io events.notify = notify (objects.ts)
            EVENTS_READ(NetNotifyPacket, notify);
            notify.text[sizeof(notify.text) - 1] = 0;
            ui_notify(notify.text);
            return 1;
        }
        case NET_PACKET_AUTH_CHALLENGE: {
            EVENTS_READ(NetAuthChallengePacket, challenge);
            online_mode_on_challenge(client, &challenge);
            return 1;
        }
        case NET_PACKET_REDIRECT: { // server console `send`: move to another server of the network
            EVENTS_READ(NetRedirectPacket, redirect);
            redirect.game_id[sizeof(redirect.game_id) - 1] = 0;
            redirect.host[sizeof(redirect.host) - 1] = 0;
            redirect.reason[sizeof(redirect.reason) - 1] = 0;
            if (redirect.reason[0]) ui_notify(redirect.reason);
            servers_ui_redirect(redirect.game_id, redirect.host, redirect.port, redirect.reason);
            return 1;
        }
        case NET_PACKET_PLAYER_INFO: {
            EVENTS_READ(NetPlayerInfoPacket, info);
            player_accounts_on_info(&info);
            return 1;
        }
        case NET_PACKET_BILLBOARD: {
            EVENTS_READ(NetBillboardPacket, billboard);
            player_accounts_on_billboard(&billboard);
            return 1;
        }
        case NET_PACKET_PING_RESULT: {
            EVENTS_READ(NetPingResultPacket, result);
            client->net_ping_ms = result.ping_ms;
            return 1;
        }
        case NET_PACKET_VOICE: // "vc": window.voiceChat(audio, alias, sid)
            voice_on_packet(client, payload, length);
            return 1;
        case NET_PACKET_SPECTATE: {
            // "start" with spectating = true: our ENT asked to spectate (client/src/spectate.c)
            EVENTS_READ(NetSpectatePacket, spectate);
            if (spectate.spectating && client->game.ready) spectate_begin(client);
            else if (!spectate.spectating) spectate_end(client);
            return 1;
        }
        case NET_PACKET_OBJECT_STATE:
        case NET_PACKET_CHECKPOINT:
        case NET_PACKET_SOUND:
        case NET_PACKET_JUMP_SCARE:
        case NET_PACKET_POSITION:
        case NET_PACKET_POPUP:
            return client_triggers_handle_event(client, type, payload, length); // client/src/map_triggers.c
        default:
            return hud_modes_handle_event(client, type, payload, length); // mode packets (client/src/ui/hud_modes.c)
    }
}

// New session or round (INIT): no clock, no end screen, nobody dead yet.
void client_game_events_reset(Client *client) {
    player_accounts_on_init();
    hud_modes_reset(client);
    hud_popups_on_init(); // "init": bN = 0
#ifdef HUD_UI_HAVE_HIDE_END_SCREEN
    hud_ui_hide_end_screen(client); // no-op unless the end screen is up
#endif
    client->net_round_seconds = -1;
    client->net_end_seconds = -1;
    client->net_end_result = INT32_MIN;
    // "init" -> CONTROLS.reset(): keys = {}, didPressed = {}. A key held through the end
    // screen does not move or jump the next round's first frames; it needs a new press
    // (GLFW_REPEAT never re-presses, ui_controls.c).
    ui_controls_reset_keys();
    client->net_damage_from_valid = 0;
    client->net_death_time = 0.0;
    client->net_death_menu_pending = 0;
    memset(client->net_projectiles, 0, sizeof(client->net_projectiles));
    projectiles_render_reset();
    sprays_clear(client); // "init": RENDER.clearSprays()
    voice_reset_queues(); // initGame: voiceStreamQueues = {}
}

// Test hook (KRUNKNATIVE_TEST_AIM=1, never set in normal runs): point the local
// player at the head of the nearest living player so scripted multi-client runs
// (xdotool holding the fire button) produce deterministic hits and kills.
static void events_test_aim(Client *client) {
    static int enabled = -1;
    if (enabled < 0) {
        const char *value = getenv("KRUNKNATIVE_TEST_AIM");
        enabled = value && value[0] && strcmp(value, "0") != 0;
    }

    Player *me = client->me;
    if (!enabled || !me || !me->active) return;

    const Player *target = NULL;
    float best = 0.0f;

    for (size_t i = 0; i < client->game.player_count; i++) {
        const Player *other = client->game.players[i];
        if (!other || other == me || !other->active) continue;

        const float dx = other->position.x - me->position.x;
        const float dz = other->position.z - me->position.z;
        const float distance = dx * dx + dz * dz;

        if (!target || distance < best) {
            target = other;
            best = distance;
        }
    }

    if (!target) return;

    const float dx = target->position.x - me->position.x;
    const float dz = target->position.z - me->position.z;
    const float eye = me->position.y + me->height - game_constants.camera_height;
    const float head = target->position.y + target->height - 0.6f;

    me->direction.y = atan2f(-dx, -dz);                     // shot dir (sin(yaw + PI), cos(yaw + PI))
    // minus the recoil climb the shot adds (player_shoot: pitch + recoilAnimY * recoilMlt)
    me->direction.x = atan2f(head - eye, sqrtf(dx * dx + dz * dz)) - me->recoil_anim_y * game_constants.recoil_mlt;
}

// Per frame: Krunker reopens the menu config.deathDelay after the death (cE()),
// from where Play respawns.
void client_game_events_tick(Client *client) {
    events_test_aim(client);
    events_projectiles_step(client);
    hud_modes_tick(client); // team scores, flags, game message, lives, team pill
    sprays_tick(client);    // framed / moving spray textures
    client_triggers_tick(client); // jump scare timeout
    if (!client->net_death_menu_pending) return;
    if (glfwGetTime() < client->net_death_time + client->net_respawn_delay) return;

    client->net_death_menu_pending = 0;
    if (html_ui_available() && !html_ui_is_menu_open()) client_set_menu_open(client, 1);
}
