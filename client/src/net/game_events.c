// Match events from the server (KILL/HIT/HEALTH/SCORE/LEADERBOARD/TIMER/END/
// CHAT/PING) applied to the client's game mirror and pushed into the HTML HUD
// through its public API (client/include/ui_logic.h). Krunker v3.1.1 reference:
// the client io handlers in index.js (g1 = "3", gF = "4", gx = "h", gf = "5",
// g5 = "7", fD = "t", fq = "end", gB = "ch", fK = "pi").
//
// Runs on the main thread from client_tick_net(); the transports only queue packets.
#include <player_accounts.h> // PLAYER_INFO (doc/backend.md section 8)
#include <billboard_ad.h>     // BILLBOARD / BILLBOARD_IMAGE: the server's ad on every map billboard
#include <ks_client.h> // KrunkScript client host: hooks and script packets
#include <backend_ui.h>      // NET_PACKET_REDIRECT: servers_ui_redirect
#include <online_mode.h>     // NET_PACKET_AUTH_CHALLENGE: online-mode join (net/online_mode.c)
#include <v10_notify.h> // NET_EXT_GAME_ANNOUNCEMENT: the game announcement banner
#include <nettspend_audio.h> // NET_EXT_KN_LACED: the Purple Drank's laced spell, its ambience (nettspend_audio.c)
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
#include <hud_map.h>  // WP-E part 2a: death markers, minimap blips
#include <menu_view.h> // g1(): CONTROLS.camLookAt(killer) death chase camera
#include <console.h>   // chat lines mirrored into the developer console (ui_console.c)
#include <sprays.h>      // NET_PACKET_SPRAY: addSpray (sprays.c)
#include <chat_commands.h> // NET_PACKET_CHEAT: /forcecol on the client's map copy
#include <third_person.h>
#include <voice_chat.h>    // NET_PACKET_VOICE: "vc" -> voiceChat (client/src/voice.c)
#include <v10_party.h>     // official lobbies' password prompt ("code" / "codeT" / "codeP")
#include <hud_social.h>    // WP-E part 2b: NET_PACKET_CHAT_V7 / VOICE_V7 / SPRAY_V7 (social_v7.h)
#include <lobby_ui.h>      // WP-E: GAME_CONFIG / NETWORK_DATA / MATCH_VOTE_STATE (client/src/net/lobby_events.c)
#include <world_events.h>  // map-logic WP-B: NET_PACKET_ZONE / BUILD / LOG_TIME / OBJECT_ANIM
#include <map_triggers.h>  // map trigger events: "do"/"ro"/"gte"/"bnk", "chp", "s", "scr", "am" popups (client/src/map_triggers.c)
#include <player_card.h>   // WP-E part 2a: death screen class levels / player card stats
#include <cosmetics.h>     // player card items (cosmetic_item)
#include "../viewer/viewer_store.h" // store skins / rarities (the killer's weapon skin)
#include <game_config.h>   // game_config_is_custom (the report prompt)
#include <net_ext.h> // NET_PACKET_EXT (254)
#include <render_settings.h> // endMessage, dmgType
#include <entities.h> // v10 KrunkScript entities / runtime blocks (official lobbies)
#include <nettspend.h> // Nettspend: the laced post-effect, the Money Spread's bills
#include <drank.h>     // KrunkNative Drank: drank_apply_laced (the shared laced timer)
#include <nav_overlay.h> // NET_EXT_KN_BOT_PATH: the spectated bot's route (show_navmeshes overlay)
#include <object_manager.h> // KrunkNative Object Manager: NET_PACKET_SPAWN_RESULT / LOADOUT_EXTRA / SPAWNED_OBJ
#include <strike_ui.h>       // Krunker Strike: its own in-game UI gets the same events (strike_hud.c)
#include <i18n.h>     // NET_EXT_KN_OFFICIAL_CHAT_I18N: an official server's "chi" key, translated here
#include <map_post.h> // NET_EXT_KN_OFFICIAL_LEADER_BOARD: a map LEADERBOARD's rows
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ruleset.h>

#define SERVER_CHAT_COLOR "#fc03ec" // gB type 2 default
#define RANKED_CHAT_COLOR "#5ce05a" // the ranked UI's green: a ranked queue's notice (NET_CHAT_RANKED)

void client_net_send(Client *client, const NetPacketType type, const void *payload, const uint16_t length) {
    client_net_tally_egress(length); // WP-E: #egressPacketCount
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
    object_manager_ui_chat_sent(packet.text); // KrunkNative: `/give <weapon>` equips it once given

    if (social_server_v7(client)) { // v7 f1909: "ct", channel (#chatSwitch), msg
        NetChatV7SendPacket v7 = {.channel = (uint8_t) (hud_ui_chat_channel() == CHAT_CHANNEL_TEAM ? CHAT_CHANNEL_TEAM : CHAT_CHANNEL_GLOBAL)};
        memcpy(v7.text, packet.text, sizeof(v7.text));
        client_net_send(client, NET_PACKET_CHAT_V7, &v7, (uint16_t) (1 + strlen(v7.text) + 1));
        return;
    }
    client_net_send(client, NET_PACKET_CHAT_SEND, &packet, (uint16_t) (strlen(packet.text) + 1));
}

// NET_EXT_KN_HIT_DAMAGE: the exact damage of the NET_PACKET_HIT right behind it (net_ext.h)
static NetHitDamagePacket g_hit_damage;
static int g_hit_damage_valid;

// The damage number's value: the exact one when the server sent it for this hit, else the HIT's.
static double events_hit_damage(const NetHitPacket *hit) {
    // the HIT's whole number is Math.round (our servers) or Math.ceil (official lobbies) of the exact one
    const int match = g_hit_damage_valid && g_hit_damage.target_uid == hit->target_uid && g_hit_damage.ai == hit->ai &&
                      fabs(g_hit_damage.damage_tenths / 10.0 - hit->damage) < 1.0;
    g_hit_damage_valid = 0;
    return match ? g_hit_damage.damage_tenths / 10.0 : (double) hit->damage;
}

static Player *events_find_player(Client *client, const int uid) {
    return uid < 0 ? NULL : game_find_player(&client->game, uid);
}

static const Weapon *events_weapon(const Client *client, const int index) {
    const size_t count = client->game.weapon_count ? client->game.weapon_count : rules_weapon_count();
    const Weapon *const *weapons = client->game.weapons ? (const Weapon *const *) client->game.weapons : (const Weapon *const *) rules_weapons();
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

// WP-E part 2a: the victim's "3" extras (NET_PACKET_DEATH_INFO), sent right before its KILL.
static NetDeathInfoPacket g_death_info;
static int g_death_info_valid;

// store.skins[index].thumbnail of a player card (Game.store.skins[playerCardIndex] ? .thumbnail : null).
static const char *events_card_url(const int index, char *out, const size_t n) {
    out[0] = 0;
    if (index < 0 || !cosmetic_item(index, ITEM_PLAYER_CARD)) return NULL;
    store_skin_preview(index, out, n);
    return out[0] ? out : NULL;
}

// f1975: the killer's weapon [name] = its skin's name in the rarity colour (the skin on the
// loadout slot of the killing weapon, the melee skin on a melee kill, "Fists" on a fist kill).
static const char *events_weapon_label(const Player *killer, const Weapon *weapon, const int weapon_index, const int fists,
                                       const char **color) {
    *color = NULL;
    if (!weapon) return fists ? "Fists" : "";
    if (weapon->melee && fists) return "Fists";
    const ViewerSkin *skin = NULL;
    if (weapon->melee) {
        skin = killer->skins.melee >= 0 ? viewer_skin(killer->skins.melee) : NULL;
    } else {
        int slot = -1;
        for (int i = 0; i < killer->loadout_size && slot < 0; i++)
            if (killer->loadout && killer->loadout[i] == weapon_index) slot = i;
        const int index = slot == 0 ? killer->skins.primary : slot == 1 ? killer->skins.secondary : -1;
        skin = index >= 0 ? viewer_skin(index) : NULL;
        if (skin && (skin->type != 0 || skin->weapon - 1 != weapon_index)) skin = NULL; // skin.weapon - 1 != loadout[slot]
    }
    if (!skin || !skin->name) return weapon->name ? weapon->name : "";
    if (skin->rarity >= 0 && skin->rarity < g_viewer_rarity_count) *color = g_viewer_rarities[skin->rarity].color;
    return skin->name;
}

// v7 ENT pcStatIndex (getSavedVal("pcStatIndex"), Change Loadout's player card stat), right before
// every ENT to v7 servers: the stat the server shows on our victims' death screens.
void client_send_pc_stat(Client *client) {
    if (!client->offline && client->net_server_minor < NET_PROTOCOL_MINOR_V7) return;
    char buf[16];
    long index = -1;
    if (html_storage_get("pcStatIndex", buf, sizeof(buf)) >= 0 && buf[0]) index = strtol(buf, NULL, 10);
    const NetPcStatPacket packet = {.index = (int8_t) (index >= 0 && index < PLAYER_CARD_STAT_COUNT ? index : -1)};
    client_net_send(client, NET_PACKET_PC_STAT, &packet, sizeof(packet));
}

// g1(): kill feed for everyone, death card + respawn delay for the victim.
static void events_kill(Client *client, const NetKillPacket *packet) {
    Player *victim = events_find_player(client, packet->victim_uid);
    Player *killer = events_find_player(client, packet->killer_uid);
    const Weapon *weapon = events_weapon(client, packet->weapon_index);

    // g1() runs entirely inside `if (victim)` (game.deob.js:220586): a "3" for a sid we have not
    // synced yet - a join race, the KILL beating the victim's "0"/ADD_PLAYERS - is dropped whole,
    // with no kill-feed row, death card, marker or FX. Without this guard the feed printed the
    // literal events_name() fallback, e.g. "unknown committed neck rope". v10 never shows that.
    if (!victim) return;

    // io_killPlayer: overlay.addDeathMarker for a teammate, ourselves or anyone while spectating
    const Player *me = client->me;
    if (victim == me || spectate_active() || (me && me->team && me->team == victim->team))
        hud_death_marker_add(victim->position, victim == me);
    victim->deaths = (unsigned int) packet->victim_deaths;
    events_player_dead(client, victim);
    // v10.0.2 g1: someone else died -> Settings > Game > Customization "Particle Effect on Kill"
    // (killFx, default on): ExplosionManager.explodeAt(x, y + 8.25, z, 0.8, 12, 0)
    if (victim != me && !ui_setting_is_false("killFx"))
        vfx_explode_at(client, (vec3) {victim->position.x, victim->position.y + 8.25f, victim->position.z}, 0.8f, 12, 0);

    if (killer && killer != victim) killer->kills = (unsigned int) packet->killer_kills;
    if (killer && killer != victim && killer == client->me) sound_events_kill(packet->headshot, killer, packet->weapon_index); // gk: kill sound (getKillSnd)

    const char *victim_name = events_name(victim);

    if (killer) { // g1: b8 found, even when it is the victim (hC = "": weapon + victim only, e.g. own rocket)
        const int self = killer == victim;
        // info.stk: the streak's icon (the Nuke on our servers; any streak from an official one)
        const int stk = NET_KILL_STREAK(packet->fists) >= 0 ? NET_KILL_STREAK(packet->fists) : packet->fists & NET_KILL_NUKE ? STREAK_NUKE : -1;
        // weaponConfig[wId != null ? wId : killer.loadout[killer.loadoutIndex]] (the feed row, :221227)
        const Weapon *feed_weapon = weapon ? weapon : killer->weapon;
        const int feed_flags = (packet->wallbang ? HUD_FEED_WALLBANG : 0) | (packet->fists & NET_KILL_FISTS ? HUD_FEED_FISTS : 0) |
                               (packet->fists & NET_KILL_THROWN ? HUD_FEED_THROWN : 0) | (packet->fists & NET_KILL_HIDE_FEED ? HUD_FEED_HIDDEN : 0);
        hud_ui_add_killfeed_flags(client, self ? NULL : events_name(killer), !self && killer == client->me, victim_name,
                                  victim && victim == client->me, feed_weapon && feed_weapon->icon ? feed_weapon->icon : NULL, packet->headshot,
                                  stk, feed_flags);
        strike_hud_killfeed(client, self ? NULL : events_name(killer), !self && killer == client->me, victim_name,
                            victim && victim == client->me, weapon, packet->headshot);
    } else {
#ifdef HUD_UI_HAVE_KILLFEED_SUICIDE
        if (!(packet->fists & NET_KILL_HIDE_FEED)) hud_ui_add_killfeed_suicide(client, victim_name, victim && victim == client->me);
#endif
        strike_hud_killfeed(client, NULL, 0, victim_name, victim && victim == client->me, NULL, 0);
    }

    if (victim != client->me) return;

    client_recon_on_death(client); // no movement input leaves until the respawn (net/recon.c)
    HudKillCard card = {0};
    const int by_other = killer && killer != victim;
    const NetDeathInfoPacket *death = g_death_info_valid && g_death_info.victim_uid == packet->victim_uid ? &g_death_info : NULL;
    g_death_info_valid = 0;
    client->camera.zoom = 1.0f; // kill(): toggleAim(player, 0) -> resetAim() -> RENDER.zoom(1); hud_ui_show_death the DOM half
    // CONTROLS.camLookAt(killer.x, y + height / 1.5, z) only with the game config's Kill Cams (kCams, default on)
    menu_view_death_cam(client, by_other && client->game.config.kill_cams ? killer : NULL);

    char killer_card[256], my_card[256], pc_value[sizeof(g_death_info.pc_stat_value) + 1];
    if (by_other) {
        const int weapon_index = death && death->weapon_index >= 0 ? death->weapon_index : packet->weapon_index;
        const Weapon *killer_weapon = death && death->weapon_index >= 0 ? events_weapon(client, death->weapon_index) : weapon;
        card.killer = events_name(killer);
        card.weapon = events_weapon_label(killer, killer_weapon, weapon_index, (packet->fists & NET_KILL_FISTS) || (death && death->fists), &card.weapon_color);
        card.class_index = packet->killer_class >= 0 ? packet->killer_class : 0;
        if (death) {
            card.level = player_card_class_level(death->class_stat); // f1975: the killer's class level from "3" [1]
            card.thrown = killer_weapon && killer_weapon->melee && death->thrown;
            if (death->pc_stat_index >= 0 && player_card_stat_name(death->pc_stat_index)) {
                card.pc_stat_name = player_card_stat_name(death->pc_stat_index);
                snprintf(pc_value, sizeof(pc_value), "%.*s", (int) sizeof(death->pc_stat_value), death->pc_stat_value);
                card.pc_stat_value = pc_value;
            }
        } else { // an older server: the account level stands in (guests show lvl 1)
            const NetPlayerInfoPacket *info = player_accounts_get(killer->uid);
            card.level = info && info->level > 1 ? info->level : 1;
        }
        card.killer_card_url = events_card_url(killer->cosmetics_v7.player_card, killer_card, sizeof(killer_card));
        card.my_card_url = events_card_url(victim->cosmetics_v7.player_card, my_card, sizeof(my_card));
        // (otherPlayer.level <= 10 || !level) && account && !Game.isCustom: "[key] to Report"
        const NetPlayerInfoPacket *info = player_accounts_get(killer->uid);
        card.report = active_account() && !game_config_is_custom(&client->game) && (!info || info->level <= 10);
        // our "lvl": account.stats["c" + classIndex] (guests: lvl 1)
        const AccountInfo *acc = active_account();
        char key[8];
        snprintf(key, sizeof(key), "c%d", victim->class_index);
        const cJSON *cls = acc && acc->stats ? cJSON_GetObjectItemCaseSensitive(acc->stats, key) : NULL;
        card.my_level = player_card_class_level(cJSON_IsNumber(cls) ? cls->valuedouble : 0.0);
    }
    if (death) { // "3" [3]: [timeAlive, shots, accuracy, kills, score]
        card.stats = 1;
        card.time_alive_ms = death->time_alive_ms;
        card.shots = death->shots;
        static char accuracy[sizeof(g_death_info.accuracy) + 1];
        snprintf(accuracy, sizeof(accuracy), "%.*s", (int) sizeof(death->accuracy), death->accuracy);
        card.accuracy = accuracy;
        card.kills = death->kills;
        card.life_score = death->score;
    }
    card.score = packet->score;

    hud_ui_show_death(client, &card);
    strike_hud_death(client, &card, killer && killer != victim ? killer : NULL); // the Strike kill card

    client->spawn_requested = 0;
    client->net_death_time = client_death_clock(client);
    client->net_respawn_delay = packet->respawn_delay > 0.0f ? packet->respawn_delay : GAME_DEATH_DELAY;
    client->net_death_menu_pending = 1;
}

// gx(): health of a player; the victim's own packet carries the dealer position.
static void events_health(Client *client, const NetHealthPacket *packet) {
    Player *player = events_find_player(client, packet->uid);
    if (!player) return;

    const float before = player->health;
    player->health = packet->health;

    // v10 "h" (:221062): the hit direction / shake only when the health went down
    if (player != client->me || !packet->has_source || packet->health >= before) return;

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
    strike_hud_end(client, client->net_end_result); // the Strike victory title and end screen
}

// endGame(): the round is over. Players stay as they are (v3.1.1 keeps them active and
// the scene plays the endAnim slow motion behind the title, menu_view.c); the next
// round's INIT drops them (client_clear_players).
// endGame only shows the end screen when `(player && player.active) || window.spectating`
// (skip is false from every v3.1.1 server path): a menu client or a dead player keeps its
// menu / death card ("CLICK TO PLAY" still enters: the server spawns it and sends a late
// "end", which then shows the title, the slow motion and the results). Until then only the
// "Next Round in" countdown is known (net_end_seconds; net_end_result stays INT32_MIN).
// endMessage (v10.js:219680, the end-game handler): the chat line is sent once per match (the flag
// clears on startGame) unless this client is a K.P.D officer on a case (kpdData), whoever won and
// whether or not we were playing.
static int g_end_message_sent, g_kpd_case;
static int g_end_show_now; // NET_EXT_KN_END_TYPE 1 (official "end" type 1) for the END that follows it

static void events_end_message(Client *client) {
    const char *message = client_render_settings()->end_message;
    if (!message[0] || g_kpd_case || g_end_message_sent) return;
    g_end_message_sent = 1;
    client_chat_send(client, message);
}

static void events_end(Client *client, const NetEndPacket *packet) {
    events_end_message(client);
    const int show_now = g_end_show_now;
    g_end_show_now = 0;
    client->net_end_seconds = packet->next_round;
    client->net_round_seconds = 0;
    client->spawn_requested = 0;
    events_apply_board(client, &packet->board);
    // endData.ed "reward": player.reward of every board row (0 = "NONE")
    // legacyData.rank: the local player's place in the board's order (the END order)
    client->net_end_rank = 0;
    for (int i = 0; i < packet->board.count && i < GAME_LEADERBOARD_MAX; i++) {
        Player *player = events_find_player(client, packet->board.entries[i].uid);
        if (player) player->reward = packet->rewards[i];
        if (client->me && !client->net_end_rank && packet->board.entries[i].uid == client->me->uid) client->net_end_rank = i + 1;
    }
    const int playing = (client->me && client->me->active) || spectate_active();
    // v10's end handler (v10.js:219715): a playing or spectating player gets the title, then the
    // results; one who is not in the game gets them only from an "end" of type 1 (an official
    // lobby's to a player who joins it while its round is over), at once and without the title
    if (!playing && !show_now) return;

    client->net_end_result = packet->result;
    client->net_death_menu_pending = 0;
    events_show_end(client);
    if (!playing) hud_ui_end_results_now(client); // showEndScreen + the results, no vicTitle
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

    if (packet->kind == NET_CHAT_PLAYER && packet->uid >= 0) { // mute_chat by the sender's account (v7 "ch" carries it)
        const NetPlayerInfoPacket *info = player_accounts_get(packet->uid);
        if (info && social_chat_muted(info->account_id)) return;
    }
    ui_console_on_chat(packet->kind == NET_CHAT_PLAYER ? (name[0] ? name : "unknown") : NULL, text, packet->kind);

    switch (packet->kind) {
        case NET_CHAT_PLAYER: // a bot's line (the server's bot chat) is never hidden by Show Player Messages
            hud_ui_add_player_chat(name[0] ? name : "unknown", text, !player_accounts_is_bot(packet->uid));
            break;
        case NET_CHAT_SERVER:
            hud_ui_add_chat(NULL, text, SERVER_CHAT_COLOR);
            break;
        case NET_CHAT_RANKED: // the brain's "queued for ranked" line (ranked_ui.c's green)
            hud_ui_add_chat(NULL, text, RANKED_CHAT_COLOR);
            break;
        case NET_CHAT_FOUND_JUNK: // io_foundJunk
            hud_ui_found_junk(text);
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
        // KrunkNative: from the menu (no player yet) the line still shows; the server sends the flag
        // again after the spawn (net_send_kept_cheats)
        case NET_CHEAT_NOCLIP:
            if (client->me) client->me->noclip = (unsigned char) on;
            line = on ? "Noclip - Enabled" : "Noclip - Disabled";
            break;
        case NET_CHEAT_UNLIMITED_AMMO:
            if (client->me) client->me->unlimited_ammo = (unsigned char) on;
            if (on && client->me) client->me->can_throw = 1;
            line = on ? "Unlimited Ammo - Enabled" : "Unlimited Ammo - Disabled";
            break;
        case NET_CHEAT_FORCE_HIDE:
            if (client->me) client->me->force_hide = (unsigned char) on;
            line = on ? "Hide - Enabled" : "Hide - Disabled";
            break;
        case NET_CHEAT_HELD: // KrunkNative: a dev's Physics Gun carries us (mode_input_locked: our input moves nothing)
            if (client->me) client->me->om_held = (unsigned char) !!on;
            break;
        case NET_CHEAT_FROZEN: // official "frz" (game.deob.js:220495): no line, prediction stops (ks_player_allows)
            if (client->me) client->me->frozen = (unsigned char) on;
            break;
        case NET_CHEAT_BORDERS:
            client->game.config.disable_borders = (unsigned int) !on; // movement prediction
            break;
        case NET_CHEAT_FORCE_COLLIDE:
            if (client->game.ready) chat_map_force_collide(client->game.map, on);
            break;
        case NET_CHEAT_THIRD_PERSON: // KrunkNative `thirdperson` granted / taken (third_person.c)
            third_person_on_grant(client, on);
            break;
        default:
            break;
    }
    if (line) {
        hud_ui_add_chat(NULL, line, SERVER_CHAT_COLOR);
        ui_console_on_cheat(packet->flag, on); // devConsole hookChat: "Noclip - Enabled", "Force Hide - ..."
    }
}

// Official-lobby player state (krunker_link.c, net_ext.h): "spb" speedBoost (+ its "SpeedBoost - x"
// line unless silent, iìîïììî game.deob.js:220498) and an "00" rename.
static void events_player_ext(Client *client, const uint16_t subtype, const uint8_t *body, const uint16_t length) {
    if (subtype == NET_EXT_PLAYER_SPEED_BOOST && length == sizeof(NetSpeedBoostPacket)) {
        NetSpeedBoostPacket boost;
        memcpy(&boost, body, sizeof(boost));
        if (!isfinite(boost.boost) || boost.boost < 0.0f) boost.boost = 0.0f;
        if (client->me) client->me->speed_boost = boost.boost;
        if (!boost.silent) {
            char line[64];
            snprintf(line, sizeof(line), "SpeedBoost - %g", (double) boost.boost);
            hud_ui_add_chat(NULL, line, SERVER_CHAT_COLOR);
        }
    } else if (subtype == NET_EXT_PLAYER_NAME && length == sizeof(NetPlayerNamePacket)) {
        NetPlayerNamePacket rename;
        memcpy(&rename, body, sizeof(rename));
        rename.name[sizeof(rename.name) - 1] = 0;
        Player *player = client_find_player(client, rename.uid);
        if (player && rename.name[0]) {
            snprintf(player->name, sizeof(player->name), "%s", rename.name);
            client->game.leaders_dirty = 1;
        }
    }
}

// g7 ("9"): another player's hitscan ray: tracer/impact and (once per shot) the gun sound.
static void events_shot(Client *client, const NetShotPacket *packet) {
    Player *shooter = events_find_player(client, packet->uid);
    if (!shooter || shooter == client->me) return;

    const vec3 hit = {packet->hit[0], packet->hit[1], packet->hit[2]};
    const int w = packet->weapon_index;
    const Weapon *weapon = w >= 0 && (size_t) w < client->game.weapon_count && client->game.weapons ? client->game.weapons[w] : NULL;
    if (weapon && weapon->stream) vfx_remote_stream_shot(client, shooter, weapon->stream, hit); // KrunkNative Piss: the spray, no tracer
    else {
        // Nettspend: the Money Spread sends one SHOT per bill (combat_send_shot runs per ray); the burst is the throw's,
        // so it comes with the first ray only (`first` 1 / 2), and a bill leaves no dust puff (no has_normal)
        const int money = nettspend_weapon_kind(w) == NETTSPEND_WEAPON_MONEY_SPREAD;
        if (money && packet->first) vfx_remote_money_spread(client, shooter);
        vfx_remote_shot(client, shooter, hit, !money && packet->has_normal, packet->normal_yaw, packet->normal_pitch, packet->trail);
    }
    if (packet->first) sound_events_remote_shot(client, shooter, packet->first == 2);
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

int client_map_list_add(Client *client, const cJSON *map) {
    for (size_t i = 0; i < client->game.map_count; i++) {
        if (client->game.maps[i] == map) return (int) i; // already listed (an official game's next round)
    }
    const cJSON **list = realloc((void *) client->game.maps, (client->game.map_count + 1) * sizeof(*list));
    if (!list) return -1;

    list[client->game.map_count] = map;
    client->game.maps = list;
    return (int) client->game.map_count++;
}

void client_set_custom_game_id(Client *client, const cJSON *map, const char *id) {
    if (!map || !id || !id[0]) return;
    for (size_t i = 0; i < client->custom_game_count; i++) {
        if (client->custom_games[i].map != map) continue;
        snprintf(client->custom_games[i].id, sizeof(client->custom_games[i].id), "%s", id);
        return;
    }
    if (client->custom_game_count == client->custom_game_cap) {
        const size_t cap = client->custom_game_cap ? client->custom_game_cap * 2 : 8;
        void *grown = realloc(client->custom_games, cap * sizeof(*client->custom_games));
        if (!grown) return;
        client->custom_games = grown;
        client->custom_game_cap = cap;
    }
    client->custom_games[client->custom_game_count].map = map;
    snprintf(client->custom_games[client->custom_game_count].id, sizeof(client->custom_games[client->custom_game_count].id), "%s", id);
    client->custom_game_count++;
}

const char *client_custom_game_id_of(const Client *client, const int map_index) {
    if (map_index < 0 || (size_t) map_index >= client->game.map_count || !client->game.maps) return "";
    const cJSON *map = client->game.maps[map_index];
    for (size_t i = 0; i < client->custom_game_count; i++)
        if (client->custom_games[i].map == map) return client->custom_games[i].id;
    return "";
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
        const int added = client_map_list_add(client, client->net_map_cache[i]);
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
        case NET_PACKET_CHAT_V7:
        case NET_PACKET_VOICE:
        case NET_PACKET_VOICE_V7:
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
    map_download_finish(client, client_map_list_add(client, map));
}

// The NUL-terminated strings of an official-HUD body (net_ext.h): up to `max` of them, NULL when the
// body ends first.
static int official_strings(const uint8_t *p, size_t left, const char **out, const int max) {
    int n = 0;
    while (n < max && left) {
        const uint8_t *end = memchr(p, 0, left);
        if (!end) break;
        out[n++] = (const char *) p;
        left -= (size_t) (end - p) + 1;
        p = end + 1;
    }
    return n;
}

// Official "rds" Switching Sides (game.deob.js:219244-219262, NET_EXT_KN_OFFICIAL_ROUND_SWITCH): every
// player inactive and its model gone, our aim off, the death card hidden (uiBase.onDeathScrn, game UI
// off), the camera gliding to the overview (camTarget(0, 220, 100, -PI / 2 * 0.7, 0)), and the menu
// after 3 s unless our next "0" respawned us (the death-menu timer, cleared by our SPAWN).
static void events_round_switch(Client *client) {
    for (size_t i = 0; i < client->game.player_count; i++) {
        Player *p = client->game.players[i];
        if (p && p->active) events_player_dead(client, p);
    }
    client->camera.zoom = 1.0f; // toggleAim(you, 0)
    if (client->me) client->me->aim_val = 0.0f;
    client_recon_on_death(client);
    hud_ui_round_switch(client);
    menu_view_overview(client);
    client->spawn_requested = 0;
    client->net_death_time = glfwGetTime();
    client->net_respawn_delay = 3.0f; // setTimeout(.., 3000)
    client->net_death_menu_pending = 1;
}

// One official addChat line (iíïíííï, game.deob.js:221172): a player line (kind 0: Show Player Messages, the
// mute list, its name in nameColor) or a game / server line (a server line in its colour, default #fc03ec),
// on its chat tab (0 global, 1 team; every tab outside team modes)
static void events_official_line(const int kind, const int channel, const int32_t account, const char *name, const char *text,
                                 const char *color, const char *name_color) {
    if (!text) return;
    if (kind == NET_CHAT_PLAYER && account && social_chat_muted(account)) return; // mutedChatAccountIds
    const char *who = name && name[0] ? name : "unknown";
    ui_console_on_chat(kind == NET_CHAT_PLAYER ? who : NULL, text, kind);
    if (kind == NET_CHAT_PLAYER) hud_ui_add_chat_channel(who, text, name_color && name_color[0] ? name_color : NULL, channel, 1);
    else hud_ui_add_chat_line_on(text, kind == NET_CHAT_SERVER ? (color && color[0] ? color : SERVER_CHAT_COLOR) : NULL, channel);
}

// An official krunker.io server's HUD packets that our own NetPackets cannot carry (krunker_link.c
// makes them, net_ext.h NET_EXT_KN_OFFICIAL_*).
// An official server refused our "en" ("fe" FAILED_TO_ENDER, v10 iíìiîîì :208203): loading ends, the
// menu comes back (showMenu: the player may click to play again) and the message is a red server
// line (addChat(-1, null, msg, 2, "#ed4242")).
static void events_enter_failed(Client *client, const char *text, const uint16_t length) {
    const int alive = client->me && client->me->active;
    if (!alive) { // the enter never happened: nothing of ours is in the game
        client->in_game = 0;
        client->spawn_requested = 0;
        client->menu_play_pending = 0;
    }
    if (html_ui_available() && !html_ui_is_menu_open()) client_set_menu_open(client, 1);
    char line[NET_CHAT_TEXT_SIZE];
    snprintf(line, sizeof(line), "%.*s", length ? (int) strnlen(text, length) : 0, text);
    if (line[0]) {
        ui_console_on_chat(NULL, line, NET_CHAT_SERVER);
        hud_ui_add_chat(NULL, line, "#ed4242");
    }
}

static void events_official_hud(Client *client, const uint16_t subtype, const uint8_t *body, const uint16_t length) {
    if (subtype == NET_EXT_KN_OFFICIAL_AMMO && length == sizeof(NetOfficialAmmoPacket)) {
        // "uamo" -> you.ammos / reserveAmmos per loadout slot, players.updatePlayerAmmo (the HUD reads them)
        NetOfficialAmmoPacket a;
        memcpy(&a, body, sizeof(a));
        Player *me = client->me;
        if (!me || !me->ammo) return;
        for (int i = 0; i < a.count && i < NET_OFFICIAL_AMMO_SLOTS && i < me->loadout_size; i++) {
            if (a.ammo[i] >= 0) me->ammo[i] = (unsigned int) a.ammo[i];
        }
        // finiteAmmo reserves: KN keys them by weapon id (shared.h reserve_weapons)
        for (int i = 0; i < a.reserve_count && i < NET_OFFICIAL_AMMO_SLOTS && i < me->loadout_size && me->loadout; i++) {
            if (a.reserve[i] < 0) continue;
            int k = 0;
            while (k < me->reserve_count && me->reserve_weapons[k] != me->loadout[i]) k++;
            if (k == me->reserve_count) {
                if (k >= PLAYER_RESERVE_MAX) continue;
                me->reserve_count++;
                me->reserve_weapons[k] = me->loadout[i];
            }
            me->reserve_ammo[k] = a.reserve[i];
        }
    } else if (subtype == NET_EXT_KN_OFFICIAL_CHAT_I18N && length >= sizeof(NetOfficialChatI18nHeader)) {
        // "chi" -> addChat(team, name, i18n.t(key, ...args), kind, color)
        NetOfficialChatI18nHeader h;
        memcpy(&h, body, sizeof(h));
        h.color[sizeof(h.color) - 1] = 0;
        h.name_color[sizeof(h.name_color) - 1] = 0;
        const char *str[2 + 16];
        const int n = official_strings(body + sizeof(h), length - sizeof(h), str, 2 + (h.argc < 16 ? h.argc : 16));
        if (n < 2) return;
        char text[NET_CHAT_TEXT_SIZE];
        i18n_format(text, sizeof(text), str[1], n - 2, str + 2);
        events_official_line(h.kind, h.channel, 0, str[0], text, h.color, h.name_color);
    } else if (subtype == NET_EXT_KN_OFFICIAL_MEDAL && length == sizeof(NetOfficialMedalPacket)) {
        // "am" -> addMedal([name, score, sound, count]): the score as given ("12 KR")
        NetOfficialMedalPacket m;
        memcpy(&m, body, sizeof(m));
        m.text[sizeof(m.text) - 1] = 0;
        m.score[sizeof(m.score) - 1] = 0;
        hud_popups_medal_scored(m.text, m.score, m.sound, m.count);
    } else if (subtype == NET_EXT_KN_OFFICIAL_END_ROW && length == sizeof(NetOfficialEndRowPacket)) {
        NetOfficialEndRowPacket row; // an "end" row: the end table prints it even when its player left
        memcpy(&row, body, sizeof(row));
        row.name[sizeof(row.name) - 1] = 0;
        hud_modes_end_row(&row);
    } else if (subtype == NET_EXT_KN_OFFICIAL_LEADER_BOARD && length >= sizeof(NetOfficialLeaderBoardHeader)) {
        // "ulb" -> map.manager.updatePendingLeader(index, rows): the canvas of that LEADERBOARD object
        NetOfficialLeaderBoardHeader h;
        memcpy(&h, body, sizeof(h));
        const char *cells[256];
        const int n = official_strings(body + sizeof(h), length - sizeof(h), cells, h.cells);
        if (client->game.ready && n >= 2) map_post_leaderboard_rows(client->scene, client->game.map, h.index, cells, n & ~1);
    }
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
            const double damage = events_hit_damage(&hit);
#ifdef HUD_UI_HAVE_DAMAGE_NUMBERS
            if (hit.ai) { // doDamage(.., isAI): Game.AI.findBySid -> dmgVal(ai.x, ai.y, ai.z)
                const GameAI *ai = game_ai_find(&client->game, hit.target_uid);
                if (ai && hit.damage) hud_ui_damage_number(client, ai->position.x, ai->position.y, ai->position.z, damage, hit.crit);
                return 1;
            }
            const Player *target = events_find_player(client, hit.target_uid);
            if (target && hit.damage) hud_ui_damage_number(client, target->position.x, target->position.y, target->position.z, damage, hit.crit); // aV.NJ2
#else
            (void) damage;
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
        // WP-F: the v7.1.4 AI packets (servers send them to clients that announced NET_PROTOCOL_MINOR_V7)
        case NET_PACKET_AI_ADD_V7: { // "aai" v7: AI.cSpawn(getData)
            EVENTS_READ(NetAIAddV7Packet, add);
            if (!game_is_authority(&client->game)) game_ai_apply_add_v7(&client->game, &add);
            return 1;
        }
        case NET_PACKET_AI_SYNC_V7: { // "ai" v7, stride 9: AI.syncAll
            NetAISyncV7Packet sync;
            memset(&sync, 0, sizeof(sync));
            if (length < offsetof(NetAISyncV7Packet, entries) || length > sizeof(sync)) return 1;
            memcpy(&sync, payload, length);
            if (sync.count > NET_AI_SYNC_V7_MAX || length < offsetof(NetAISyncV7Packet, entries) + sync.count * sizeof(NetAISyncV7Entry)) return 1;
            if (!game_is_authority(&client->game)) game_ai_apply_sync_v7(&client->game, &sync);
            return 1;
        }
        case NET_PACKET_AI_REMOVE: { // "rai": AI.removeBySid
            EVENTS_READ(NetAIRemovePacket, remove);
            if (!game_is_authority(&client->game)) game_ai_apply_remove(&client->game, &remove);
            return 1;
        }
        case NET_PACKET_AI_ANIM: { // "ana" sid, clip, raw: AI.servAnim
            EVENTS_READ(NetAIAnimPacket, anim);
            if (!game_is_authority(&client->game)) game_ai_apply_anim(&client->game, &anim);
            return 1;
        }
        case NET_PACKET_AI_SHOT: { // the AI that fired the last "pr": its projOb visuals
            EVENTS_READ(NetAIShotPacket, shot);
            ai_render_ai_shot(client, &shot);
            return 1;
        }
        case NET_PACKET_SHOT: {
            EVENTS_READ(NetShotPacket, shot);
            hud_minimap_on_shot(client, shot.uid); // f1988: the shooter blips on the minimap
            events_shot(client, &shot);
            return 1;
        }
        case NET_PACKET_PROJECTILE: {
            EVENTS_READ(NetProjectilePacket, projectile);
            hud_minimap_on_shot(client, projectile.owner_uid); // f1988 (io_addProjectile's sid)
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
            if (client->me && medals.kills > 0) client->me->kills = (unsigned int) medals.kills; // gk(): killsVal = totalKills
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
        case NET_PACKET_DEATH_INFO: { // WP-E part 2a: the victim's "3" extras, ahead of its KILL
            EVENTS_READ(NetDeathInfoPacket, death);
            g_death_info = death;
            g_death_info_valid = 1;
            return 1;
        }
        case NET_PACKET_SKIN_FOUND: { // season-system: v7 "skin" (io_skinReward) / "skinF" (io_skinFoundMessage)
            EVENTS_READ(NetSkinFoundPacket, found);
            hud_ui_skin_found(client, found.uid, found.skin);
            return 1;
        }
        case NET_PACKET_END_MEDALS: { // WP-E part 2c: endData.mdls, right before this player's END
            EVENTS_READ(NetEndMedalsPacket, medals);
            hud_ui_set_end_medals(&medals);
            return 1;
        }
        case NET_PACKET_LEADER_STATS: { // WP-E: v7 "7" columns (mode.leaderStats) in the server's board order
            EVENTS_READ(NetLeaderStatsPacket, stats);
            hud_ui_set_leader_stats(&stats);
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
        case NET_PACKET_SPRAY: { // "sp": addSpray(sid, sprayIndex, x, y, z, xDr, yDr)
            EVENTS_READ(NetSprayPacket, spray);
            sprays_add(client, &spray);
            return 1;
        }
        case NET_PACKET_SPRAY_V7: { // v7 "sp": addSpray("<sid>_<n>", sprayIndex, x, y, z, rotY, rotX, rotZ, dyeId)
            EVENTS_READ(NetSprayV7Packet, spray);
            sprays_add_v7(client, &spray);
            return 1;
        }
        case NET_PACKET_SOCIAL_V7: // the server takes the v7 chat / voice / spray / taunt packets
            if (length >= 1) social_on_server_caps(((const uint8_t *) payload)[0]);
            return 1;
        case NET_PACKET_CHAT_V7: { // v7 "ch": io_addChat(channel, ..., accountId)
            EVENTS_READ(NetChatV7Packet, chat);
            social_on_chat_v7(client, &chat);
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
            billboard_ad_on_packet(&billboard); // applied by the next INIT (billboard_ad.h)
            return 1;
        }
        case NET_PACKET_BILLBOARD_IMAGE: {
            EVENTS_READ(NetBillboardImagePacket, billboard_image);
            billboard_ad_on_image_packet(&billboard_image);
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
        case NET_PACKET_VOICE_V7: // v7 "vc": io_voiceChat(audio, name, sid, channel, volume, isFinal, accountId)
            voice_on_packet_v7(client, payload, length);
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
        case NET_PACKET_CUSTOM_VALUE:      // WP-C v7 trigger packets (120-128)
        case NET_PACKET_GLOBAL_VALUE:
        case NET_PACKET_GUI_TOGGLE:
        case NET_PACKET_AMBIENT_CHANGE:
        case NET_PACKET_STOP_SOUND:
        case NET_PACKET_PLAYER_SOUND:
        case NET_PACKET_CONFIRM_INTERACT:
        case NET_PACKET_CHANGE_POSITION:
        case NET_PACKET_PICKUP:
            return client_triggers_handle_event(client, type, payload, length); // client/src/map_triggers.c
        case NET_PACKET_EFFECT:
        case NET_PACKET_STREAK:
        case NET_PACKET_NUKE:
        case NET_PACKET_JUGGERNAUT:
        case NET_PACKET_PERKS:
        case NET_PACKET_UPGRADES:
        case NET_PACKET_WEAPON_STREAK:
        case NET_PACKET_GRAPPLE:
            return combat_events_handle(client, type, payload, length); // WP-B (client/src/net/combat_events.c)
        case NET_PACKET_BLOCK_ZOMBIES + 0: // map-logic WP-H (client/src/net/zombie_events.c): mybx / krm, smybx,
        case NET_PACKET_BLOCK_ZOMBIES + 1: // pwup, upz, uamo reserves, sic
        case NET_PACKET_BLOCK_ZOMBIES + 2:
        case NET_PACKET_BLOCK_ZOMBIES + 3:
        case NET_PACKET_BLOCK_ZOMBIES + 4:
        case NET_PACKET_BLOCK_ZOMBIES + 5:
        case NET_PACKET_BLOCK_ZOMBIES + 6: // zombies end stats (aiKills / rounds)
        case NET_PACKET_BLOCK_AIRDROP + 0: // ad, rad, krp
        case NET_PACKET_BLOCK_AIRDROP + 1:
        case NET_PACKET_BLOCK_AIRDROP + 2:
            return zombie_events_handle(client, type, payload, length);
        case NET_PACKET_GAME_CONFIG:
        case NET_PACKET_NETWORK_DATA:
        case NET_PACKET_MATCH_VOTE_STATE:
        case NET_PACKET_COMP_LOBBY:
            return lobby_events_handle(client, type, payload, length); // WP-E
        case NET_PACKET_SPAWN_RESULT: // KrunkNative Object Manager (client/src/ui/object_manager_ui.c)
        case NET_PACKET_LOADOUT_EXTRA:
        case NET_PACKET_SPAWNED_OBJ:
            return object_manager_events_handle(client, type, payload, length);
        case NET_PACKET_ZONE:
        case NET_PACKET_BUILD:
        case NET_PACKET_LOG_TIME:
        case NET_PACKET_OBJECT_ANIM:
            return client_world_handle_event(client, type, payload, length); // map-logic WP-B (client/src/world_events.c)
        case NET_PACKET_EXT: { // v10+ envelope (shared/include/net_ext.h)
            uint16_t subtype, body_length;
            const uint8_t *body;
            if (!net_ext_unpack(payload, length, &subtype, &body, &body_length)) return 1;
            // entities, pose, locks ... land with their features; unknown subtypes are ignored, never an error
            if (subtype == NET_EXT_KN_ACCOUNT_RIGHTS) online_mode_on_rights(client, body, body_length); // live sign-in (online_mode.c)
            else if (subtype == NET_EXT_KN_INPUTS_DROPPED) client_recon_drop_pending(client); // official "f" (krunker_link.c)
            else if (subtype == NET_EXT_KN_END_TYPE && body_length >= 1) g_end_show_now = body[0] == 1; // official "end" type
            else if (subtype == NET_EXT_GAME_ANNOUNCEMENT) v10_announce_on_packet(body, body_length); // the banner (v10_notify.c)
            else if (subtype == NET_EXT_KN_HIT_DAMAGE && body_length >= sizeof(NetHitDamagePacket)) { // dmgType "Float"
                memcpy(&g_hit_damage, body, sizeof(g_hit_damage));
                g_hit_damage_valid = 1;
            } else if (subtype == NET_EXT_KN_LACED) { // Nettspend: a bad pour (nettspend.h)
                drank_apply_laced(&client->game, body, body_length); // the shared timer (Player.laced_timer, drank.h)
                nettspend_on_laced(client, body, body_length);       // the local player's post-effect
                if (client->me) nettspend_audio_on_laced(body, body_length, client->me->uid); // our own laced ambience
            } else if (subtype == NET_EXT_VOTE_KICK_STATE && body_length >= sizeof(NetVoteKickStatePacket)) { // "vk" (hud_votekick.c)
                NetVoteKickStatePacket state;
                memcpy(&state, body, sizeof(state));
                hud_votekick_on_state(client, &state);
            } else if (subtype == NET_EXT_KPD_DATA && body_length >= sizeof(NetKpdDataPacket)) { // startGame kpdData (officers)
                NetKpdDataPacket data;
                memcpy(&data, body, sizeof(data));
                g_kpd_case = data.sus >= 0; // sus -1: no case
                hud_votekick_on_kpd_data(client, &data);
            } else if (subtype == NET_EXT_KPD_RESET) { // "kpdf": the verdict is open again
                hud_votekick_on_kpd_reset(client);
            } else if (subtype == NET_EXT_KPD_RESPONSE && body_length >= sizeof(NetKpdResponsePacket)) { // "kpd" policeRes
                NetKpdResponsePacket res;
                memcpy(&res, body, sizeof(res));
                hud_votekick_on_kpd(client, &res, (const char *) body + sizeof(res), body_length - sizeof(res));
            }
            else if (subtype == NET_EXT_PLAYER_SPEED_BOOST || subtype == NET_EXT_PLAYER_NAME) events_player_ext(client, subtype, body, body_length);
            else if (subtype == NET_EXT_KN_BOT_PATH) nav_overlay_bot_path(client, body, body_length); // the spectated bot's route (nav_overlay.c)
            else if (subtype == NET_EXT_MARK_POSITION && body_length >= sizeof(NetMarkPositionPacket)) { // "mpos": addPingIcon (hud_map.c)
                NetMarkPositionPacket ping;
                memcpy(&ping, body, sizeof(ping));
                ping.key[sizeof(ping.key) - 1] = 0;
                hud_ping_marker_add(ping.key, (vec3) {ping.x, ping.y, ping.z});
            }
            else if (subtype == NET_EXT_KN_OFFICIAL_ENTER_FAILED) events_enter_failed(client, (const char *) body, body_length); // "fe"
            else if (subtype >= NET_EXT_KN_OFFICIAL_AMMO && subtype <= NET_EXT_KN_OFFICIAL_END_ROW) events_official_hud(client, subtype, body, body_length);
            else if (subtype == NET_EXT_KN_OFFICIAL_ROUND_SWITCH) events_round_switch(client);
            else if (subtype == NET_EXT_KN_OFFICIAL_TEXT_POPUP && body_length > sizeof(NetOfficialTextPopupHeader)) { // textPopup
                NetOfficialTextPopupHeader h;
                memcpy(&h, body, sizeof(h));
                const char *str[2] = {"", ""};
                if (official_strings(body + sizeof(h), body_length - sizeof(h), str, 2) == 2)
                    hud_popups_text_popup(str[0], str[1], h.silent, h.sub_size);
            }
            else if (subtype == NET_EXT_KN_OFFICIAL_COUNTDOWN && body_length == sizeof(NetOfficialCountdownPacket)) { // gmsg countdown
                NetOfficialCountdownPacket c;
                memcpy(&c, body, sizeof(c));
                hud_popups_countdown(c.n);
            }
            else if (subtype == NET_EXT_VOTE_FORFEIT_STATE && body_length >= sizeof(NetVoteForfeitStatePacket)) { // "vf" (hud_votekick.c)
                NetVoteForfeitStatePacket state;
                memcpy(&state, body, sizeof(state));
                hud_votekick_on_forfeit(client, &state);
            }
            else if (subtype == NET_EXT_KN_OFFICIAL_CLOCK_STATE && body_length == 1) hud_ui_set_end_clock_state(body[0]); // "t" state
            else if (subtype == NET_EXT_KN_OFFICIAL_CHAT && body_length > sizeof(NetOfficialChatHeader)) { // "ch"
                NetOfficialChatHeader h;
                memcpy(&h, body, sizeof(h));
                h.color[sizeof(h.color) - 1] = 0;
                h.name_color[sizeof(h.name_color) - 1] = 0;
                const char *str[2] = {"", ""};
                if (official_strings(body + sizeof(h), body_length - sizeof(h), str, 2) == 2)
                    events_official_line(h.kind, h.channel, h.account_id, str[0], str[1], h.color, h.name_color);
            }
            else if (subtype == NET_EXT_KN_OFFICIAL_CODE_PROMPT) v10_party_password_prompt_official(); // "code": gameCode()
            else if (subtype == NET_EXT_KN_OFFICIAL_CODE_TIMER && body_length == sizeof(int32_t)) { // "codeT"
                int32_t seconds;
                memcpy(&seconds, body, sizeof(seconds));
                v10_party_code_timer(seconds);
            }
            else if (subtype == NET_EXT_KN_OFFICIAL_CODE_PASSED) v10_party_code_passed(); // "codeP": clearPops
            else if (subtype == NET_EXT_VOTE_FORFEIT && body_length >= sizeof(NetVoteForfeitPacket)) { // KrunkNative's team vote (server/src/forfeit.c)
                NetVoteForfeitPacket vf;
                memcpy(&vf, body, sizeof(vf));
                NetVoteForfeitStatePacket state; // the same box as the official lobbies'; the voter's line is the server's chat
                memset(&state, 0, sizeof(state));
                state.active = vf.active;
                state.opened = vf.opened;
                state.update_only = (uint8_t) (vf.opened ? 0 : 1);
                state.votes = vf.votes;
                state.needed = vf.needed;
                memcpy(state.initiator, vf.initiator, sizeof(state.initiator));
                state.initiator[sizeof(state.initiator) - 1] = 0;
                hud_votekick_on_forfeit(client, &state);
            }
            else if (subtype == NET_EXT_KN_OFFICIAL_SHAKE && body_length == sizeof(NetOfficialShakePacket)) { // "shk"
                NetOfficialShakePacket shake;
                memcpy(&shake, body, sizeof(shake));
                if (isfinite(shake.amount)) camera_feel_shake(shake.amount);
            }
            else entities_on_ext(client, subtype, body, body_length); // v10 entities, runtime blocks, objmv, plk (entities.c)
            return 1;
        }
        default:
            return hud_modes_handle_event(client, type, payload, length); // mode packets (client/src/ui/hud_modes.c)
    }
}

static int g_game_joined; // an INIT was applied since the last client_leave_game

// New session or round (INIT): no clock, no end screen, nobody dead yet.
void client_game_events_reset(Client *client) {
    client_world_reset(); // WP-B: no speed-run time yet
    player_accounts_on_init();
    billboard_ad_on_init(); // the server's ad for this map, before client_load_map builds it
    hud_modes_reset(client);
    hud_votekick_reset(); // a new round: no vote kick running, the K.P.D phone down
    hud_popups_on_init(); // "init": bN = 0
    hud_v7modes_on_init(); // ui-v7: #krTagHolder emptied, the speed-run list rebuilt
    hud_map_on_init(client); // part 2a: overlay.miniMap.reset + overlay.resetAll (no death markers)
    third_person_on_init(client); // the third-person grant is per round: asked again once in game
    g_death_info_valid = 0;
#ifdef HUD_UI_HAVE_HIDE_END_SCREEN
    hud_ui_hide_end_screen(client); // no-op unless the end screen is up
    strike_hud_end_hide(client);
#endif
    client->net_round_seconds = -1;
    client->net_end_seconds = -1;
    client->net_end_result = INT32_MIN;
    client->net_end_rank = 0;
    g_end_message_sent = 0; // startGame: the next match end sends endMessage again
    g_kpd_case = 0;
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
    g_game_joined = 1;
}

// Leaving a game: a disconnect or a lost session, a server switch (matchmaker, server browser,
// `connect`, a reconnect), KrunkNative <-> official krunker.io, offline <-> online, a reload.
// v10 leaves a game by loading a new page (enterGame's links, backToOrigin, the reload: all
// location.href, v10:218690 / v10:221895), so nothing of the old game survives it. Here the page
// stays: what that game put on it goes now, not when the next game's INIT comes (or never, for a
// Lost Connection screen): the chat and kill feed, the death card, the end screen and its vote,
// the players (the scoreboard, the leaderboard, the player list), spectating, the vote kick, the
// round clock, the game popups and the announcement banner. Main thread; a no-op until a game's
// INIT was applied since the last leave.
void client_leave_game(Client *client) {
    if (!client || !g_game_joined) return;
    g_game_joined = 0;
    if (client->game.player_count || client->me) client_clear_players(client); // spectating ends with them
    client_game_events_reset(client); // the round clock, the end screen, the vote kick, the mode HUD
    g_game_joined = 0;                // (client_game_events_reset marks a game joined)
    lobby_vote_reset();               // the end screen's vote cards
    social_reset_server();
    object_manager_ui_reset();
    hud_ui_leave_game(client);        // chat, kill feed, death card, damage indicators, leader stats
    hud_spectate_reset();
    strike_hud_reset();
    v10_announce_set(NULL);
    client->in_game = 0;
    client->spawn_requested = 0;
    menu_rerender_if_open(23); // the Player List has nobody left
    printf("[client] left the game: its chat, feed, players and end screen are gone\n");
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

// The clock of the death delay (net_death_time, the menu after deathDelay, the respawn gate in
// client_enter_game): the HUD's frame clock (hud_ui_now_ms), in seconds. In play that is GLFW's
// clock, as before; --render-scenario's virtual clock and KRUNKNATIVE_FIXED_DELTA's summed frame
// deltas otherwise, so a scenario's death card -> menu step does not depend on the machine's load.
double client_death_clock(const Client *client) {
    return hud_ui_now_ms(client) / 1000.0;
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
    if (client_death_clock(client) < client->net_death_time + client->net_respawn_delay) return;

    client->net_death_menu_pending = 0;
    // Krunker Strike: Cg() after xe.deathDelay, straight back into the game (strike_hud.c)
    if (strike_ui_active()) {
        strike_hud_respawn(client);
        return;
    }
    // f1771 runs UI.toggleMenu(true) also when the menu is up already (ESC before the death or
    // during it): toggleMenuHider(true) drops .onDeathScrn, so the chat leaves the death
    // screen's place (main.css #uiBase.onDeathScrn #chatHolder) for the menu's.
    menu_auto_respawn_begin(client); // v10: forceSpawn && autoRespawn(forceSpawn + 1), before the menu (menu_ui.c)
    if (html_ui_available()) client_set_menu_open(client, 1);
}
