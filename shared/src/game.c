#include <object_manager.h>
#include <piss.h> // KrunkNative Piss puddles
#include <drank.h> // KrunkNative Drank: laced timers
#include <forfeit.h>  // the team forfeit vote (v10 voteToForfeit)
#include <net_ext.h> // game_send_hit: NET_EXT_KN_HIT_DAMAGE
#include <shared.h>
#include <ruleset.h>
#include <ai.h>
#include <map_ai.h>
#include <bots.h>
#include <bot_chat.h>
#include <world.h>
#include <triggers_v7.h>
#include <ks_game.h> // KrunkScript hook points (onGameEnd, TIME.freeze)
#include <spawnables.h>
#include <game_config.h> // WP-E: v7 custom game config, map config, match votes
#include <zombies.h> // map-logic WP-H: machines, power-ups, airdrops
#include <comp_lobby.h> // v7 comp lobby (lobbyPlayers)
#include <account.h>     // WP-E part 2c: incMedal (accounts and the sandbox)
#include <progression.h> // WP-E part 2c: overlay.medalsList
#include <flavor_rules.h> // product-flavor defaults (Krunker Strike: its lobby config)
#include <map_catalog.h>  // map_catalog_rotation: Krunker Strike's weighted map pool
#include <pcg_basic.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#ifdef KRUNKNATIVE_CLIENT
#include <client.h>
#endif

const GameConfig g_default_game_config = {
    .max_players = 8,
    .min_players = 0,
    .game_time = 4,
    .warmup_time = 0.0f,
    .auto_respawn = 0,
    .lives = 0,
    .score_limit = 0,
    .gravity_mlt = 1.0f,
    .jump_mlt = 1.0f,
    .delta_mlt = 1.0f,
    .slide_time = 1.0f,
    .impulse_mlt = 1.0f,
    .wall_jump = 1.0f,
    .strafe_speed = 1.2f,
    .slide_speed = 1.0f,
    .can_slide = 1,
    .air_strafe = 0,
    .auto_jump = 0,
    .bullet_drop = 0,
    .health_regen = 1,
    .kill_rewards = 1,
    .headshots_only = 0,
    .no_secondaries = 0,
    .no_streaks = 0,
    .disable_borders = 0,
    .throwable_melees = 1,
    .select_team = 0,
    .allow_spectate = 1,
    .third_person = 0,
    .hide_nametags = 0,
    .team1_name = "Alpha", // v7 config.teamNames (v3 "Team 1" / "Team 2")
    .team2_name = "Bravo",
    .team1_damage = 1.0f,
    .team2_damage = 1.0f,
    .health_mlt = 1.0f,
    .fire_rate = 1.0f,
    .reload_speed = 1.0f,
    .hitbox_pad = 0.6f, // hitBoxPad def (hitbox_pad_set 0: Ranked keeps defC 0.1, effects.c)
    .hurtbox_scale = 1.0f, // v10.0.2 hurtboxScale def (v10:16931)
    // WP-E: the other v7.1.4 serverConfig defaults (shared/src/game_config.c holds the full table)
    .game_rounds = 1,
    .intermission_time = 30.0f,
    .objective_time = 1.0f,
    .force_classes = 1,
    .log_time = 1,
    .kill_cams = 1,
    .allow_anonymous = 1,
    .om_weapons = 1, // KrunkNative knOmWeps
    .tp_cam = 0,     // KrunkNative knTPCam: the host must allow third person
    .spec_slots = 2,
    .team_size = 3,
    .minimaps = 1,
    .team_names = {"", "Alpha", "Bravo", "Charlie", "Delta", "Echo"},
    .team_damage = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f},
};

void game_clear_players(Game *game) {
    if (game->player_count) {
        free(game->players);
        game->player_count = 0;
    }
}

void game_configure(
    Game *game, const GameConfig *config,
    const cJSON **maps, const size_t map_count,
    int *modes, const size_t mode_count,
    Weapon **weapons, const size_t weapon_count,
    ClassConfig *classes, size_t class_count
) {
    // the ruleset (ruleset.h, Krunker v10.0.2) supplies the default tables; the game keeps its pointer
    const Ruleset *rules = ruleset_current();
    game->rules = rules;
    if (config) game->config = *config;
    else game->config = *flavor_default_game_config(); // base: g_default_game_config
    game_config_sync_teams(&game->config); // WP-E: team1/2_name point at this copy's team_names
    game_config_free(game);                // WP-E: a new configuration drops the host request / overrides
    comp_lobby_free(game);                 // and a new game object's lobbyPlayers start empty

    if (game->map_count) {
        free(game->maps);
        game->map_count = 0;
    }

    if (map_count) {
        game->maps = maps;
        game->map_count = map_count;
    } else if (flavor_current()->rules) {
        // Krunker Strike: FRVR's map pool, each map listed StrikeMap.weight times (mapPool's
        // mapWeight; game_init then picks uniformly), never the v7 rotation (map_catalog.c)
        load_default_maps();
        const size_t total = map_catalog_rotation(NULL, 0);
        int *pool = total ? malloc(total * sizeof(int)) : NULL;
        const size_t n = pool ? map_catalog_rotation(pool, total) : 0;
        game->maps = n ? calloc(n, sizeof(cJSON *)) : NULL;
        game->map_count = 0;
        for (size_t i = 0; game->maps && i < n; i++) {
            if (pool[i] >= 0 && pool[i] < MAP_CATALOG_OFFICIAL_COUNT && g_maps[pool[i]]) game->maps[game->map_count++] = g_maps[pool[i]];
        }
        free(pool);
        if (!game->map_count) {
            free(game->maps);
            game->maps = NULL;
        }
    } else {
        load_default_maps();

        game->map_count = sizeof(g_rotation_maps) / sizeof(g_rotation_maps[0]);
        game->maps = calloc(game->map_count, sizeof(cJSON*));

        if (game->maps) {
            for (size_t i = 0; i < game->map_count; i++) {
                game->maps[i] = g_maps[g_rotation_maps[i]];
            }
        } else {
            game->map_count = 0;
        }
    }

    if (game->mode_count) {
        free(game->modes);
        game->mode_count = 0;
    }

    if (mode_count) {
        game->modes = modes;
        game->mode_count = mode_count;
    } else {
        // Free for All only unless the host picks modes (server --mode / CLI `mode`,
        // Host Game); g_rotation_modes is Krunker's public rotation for `--mode rotation`.
        game->mode_count = 1;
        game->modes = calloc(game->mode_count, sizeof(int));

        if (game->modes) game->modes[0] = MODE_ID_FFA;
        else game->mode_count = 0;
    }

    if (game->weapon_count) {
        free(game->weapons);
        game->weapon_count = 0;
    }

    if (weapon_count) {
        game->weapons = weapons;
        game->weapon_count = weapon_count;
    } else {
        game->weapon_count = rules->weapon_count;
        game->weapons = calloc(game->weapon_count, sizeof(Weapon*));

        if (game->weapons) memcpy(game->weapons, rules->weapons, game->weapon_count * sizeof(Weapon*));
        else game->weapon_count = 0;
    }

    if (game->class_count) {
        free(game->classes);
        game->class_count = 0;
    }

    if (class_count) {
        game->classes = classes;
        game->class_count = class_count;
    } else {
        game->class_count = rules->class_count;
        game->classes = calloc(game->class_count, sizeof(ClassConfig));

        if (game->classes) memcpy(game->classes, rules->classes, game->class_count * sizeof(ClassConfig));
        else game->class_count = 0;
    }
}

void game_init(Game *game, int map_index, int mode_index, const unsigned char is_local) {
    game->ready = 0;
    game->is_local = is_local;

    game_clear_players(game);

    if (!game->map_count || !game->mode_count) return;

    // Neither given (a public server's first round, or no end-screen picks): a playable random
    // pair of the rotation (v7.1.4 server startup). A hosted game keeps v7 init's independent
    // config.maps / config.modes picks below (game.js:3428-3429).
    if (map_index < 0 && mode_index < 0 && !game_config_is_custom(game)) game_pick_rotation_pair(game, &map_index, &mode_index);

    const int map_to_load = map_index >= 0 && map_index < game->map_count ? map_index : (int) pcg32_boundedrand(game->map_count);
    const int map_changed = !game->map || game->map->raw_data != game->maps[map_to_load];

    // current_mode_index (and mode_index) index game->modes; game->modes holds mode ids.
    const int mode_to_load = mode_index >= 0 && mode_index < (int) game->mode_count ? mode_index : (int) pcg32_boundedrand((uint32_t) game->mode_count);

    game->current_map_index = map_to_load;
    game->current_mode_index = mode_to_load;

    object_manager_restore_map(game); // KrunkNative: the map's own Objects back from the Object Manager's edits
    if (map_changed) {
        Map *new_map = map_init_ex(game->maps[map_to_load], !game->server); // a server's map has no meshes

        if (game->map) map_fini(game->map);
        game->map = new_map;
    } else {
        map_reset(game->map);
    }

    GameMode *new_mode = mode_init(game->modes[mode_to_load]);

    if (game->mode) mode_fini(game->mode);
    game->mode = new_mode;

    if (game->vote_kick) {
        free(game->vote_kick);
        game->vote_kick = NULL;
    }

    game_ai_reset(game); // game.init: AI.reset() (clients drop their mirrored AIs too)
    game_bots_on_init(game); // v7.1.4 bots: the host freed their players; they rejoin next tick (shared/src/bots.c)
    bot_chat_on_init(game);  // their pending chat lines are dropped

    if (!game->mode || !game->map) return;

    // WP-E: a hosted game / the sandbox plays each map with its embedded "config" (and the host's
    // settings); the round clock below reads the result.
    if (game_is_authority(game)) game_apply_map_config(game, game->map);

    game->ready = 1;
    game_start_round(game);
    map_ai_spawn_pending(game); // map.manager.addPendingAIs(AI): the map's AI_SPAWNERs (authority, WP-F)
    map_bots_spawn_pending(game); // addPendingBots(bots): the map's BOT_SPAWNERs (authority, WP-F)
}

// The index of mode `mode_id` in game->modes, appending it when the list lacks it (a
// client following a server's INIT, which carries the Krunker mode id). -1: unknown id.
int game_mode_index_for(Game *game, const int mode_id) {
    if (!mode_config(mode_id)) return -1;

    for (size_t i = 0; i < game->mode_count; i++) {
        if (game->modes && game->modes[i] == mode_id) return (int) i;
    }

    int *modes = realloc(game->modes, (game->mode_count + 1) * sizeof(int));
    if (!modes) return -1;

    game->modes = modes;
    game->modes[game->mode_count] = mode_id;
    return (int) game->mode_count++;
}

// The Krunker mode id of the current round (game->modes[current_mode_index]), -1 none.
int game_mode_id(const Game *game) {
    if (game->mode) return game->mode->config.id;
    const int index = game->current_mode_index;
    return index >= 0 && (size_t) index < game->mode_count && game->modes ? game->modes[index] : -1;
}

int game_is_authority(const Game *game) {
    return game->is_local || game->server;
}

int game_can_spawn(const Game *game, const Player *player) {
    if (!game->ready || game->end_timer > 0.0f) return 0;
    if (player && player->active) return 0;
    if (player && player->deaths && game->now - player->death_time < GAME_DEATH_DELAY - 0.25f) return 0;
    return mode_can_spawn(game, player); // lives modes: out of lives once the match started
}

void game_player_changing(const Game *game, Player *player, const int done) {
    if (game && game->hooks.player_changing) game->hooks.player_changing(game, player, done);
}

void game_send(Game *game, const int uid, const int type, const void *payload, const uint16_t length) {
    if (game->hooks.send) game->hooks.send(game, uid, type, payload, length);
}

void game_send_hit(Game *game, const int uid, const NetHitPacket *hit, const float damage) {
    const int32_t tenths = (int32_t) lroundf(damage * 10.0f);
    if (hit->damage && tenths % 10) { // the fraction the whole-number HIT loses (dmgType "Float", net_ext.h)
        const NetHitDamagePacket body = {.target_uid = hit->target_uid, .damage_tenths = tenths, .ai = hit->ai};
        uint8_t buffer[NET_EXT_HEADER_SIZE + sizeof(body)];
        const uint16_t n = net_ext_pack(buffer, sizeof(buffer), NET_EXT_KN_HIT_DAMAGE, &body, sizeof(body));
        if (n) game_send(game, uid, NET_PACKET_EXT, buffer, n);
    }
    game_send(game, uid, NET_PACKET_HIT, hit, sizeof(*hit));
}

// ------------------------------------------------------------------ WP-E part 2c: progression

// game.isPubType(): !hasHost() || mimicPubs. The offline sandbox is v7's host-less sandbox.
int game_is_pub_type(const Game *game) {
    return game && (game->is_sandbox || !game_config_is_custom(game) || game->config.mimic_pubs);
}

// game.incStat(stat, player): mode.skipStats is not ported (no v7 mode lists one KrunkNative runs).
void game_inc_stat(Game *game, const Player *player, const char *key) {
    if (!game || !player || player->is_bot || !game->hooks.progress || !game_is_authority(game) || !game_is_pub_type(game)) return;
    game->hooks.progress(game, player, 1, key, 1, -1, -1, -1);
}

// challenges.update(game, key, value, {wep, stk}, player) / updateMulti
void game_challenge(Game *game, const Player *player, const char *key, const int value, const int weapon, const int streak) {
    if (!game || !player || player->is_bot || !game->hooks.progress || !game_is_authority(game) || !game_is_pub_type(game)) return;
    game->hooks.progress(game, player, 0, key, value, weapon, streak, -1);
}

void game_give_skin(Game *game, const Player *player, const int skin, const char *reason) {
    if (!game || !player || player->is_bot || skin <= 0 || !game->hooks.give_skin || !game_is_authority(game) || !game_is_pub_type(game)) return;
    game->hooks.give_skin(game, player, skin, reason ? reason : "raid");
}

void game_challenge_item(Game *game, const Player *player, const char *key, const int value, const int item) {
    if (!game || !player || player->is_bot || !game->hooks.progress || !game_is_authority(game) || !game_is_pub_type(game)) return;
    game->hooks.progress(game, player, 0, key, value, -1, -1, item);
}

// game.incMedal(medal, player): player && (game.isSandbox || player.account) && isPubType()
void game_inc_medal(Game *game, Player *player, const char *name) {
    if (!game || !player || player->is_bot || !game_is_authority(game) || !game_is_pub_type(game)) return;
    AccountInfo account; // player.account: Player.has_account (the server's spawn) or the account hook
    if (!game->is_sandbox && !player->has_account && !(game_account_info(game, player, &account) && account.has_account)) return;
    const int index = progression_medal_index(name);
    if (index >= 0 && index < PLAYER_MEDAL_SLOTS && player->medal_counts[index] < UINT16_MAX) player->medal_counts[index]++;
}

// endData.mdls of one player: its medals in list order, right before its END.
static void game_send_end_medals(Game *game, const Player *player) {
    if (player->is_bot) return;
    NetEndMedalsPacket packet;
    memset(&packet, 0, sizeof(packet));
    for (int i = 0; i < PLAYER_MEDAL_SLOTS; i++) {
        if (!player->medal_counts[i]) continue;
        packet.medals[packet.count].index = (uint8_t) i;
        packet.medals[packet.count].count = player->medal_counts[i];
        packet.count++;
    }
    game_send(game, player->uid, NET_PACKET_END_MEDALS, &packet, sizeof(packet));
}

Player *game_find_player(Game *game, const int uid) {
    for (size_t i = 0; i < game->player_count; i++) {
        if (game->players[i] && game->players[i]->uid == uid) return game->players[i];
    }

    return NULL;
}

// players.syncLeaders: the mode's leader order (leaderSort / killSort / orderByScore, stable).
static size_t game_sorted_players(Game *game, Player **out, const size_t max) {
    Player *all[256]; // every player is ranked, then the top `max` rows are kept
    size_t count = 0;

    const int bots_as_ai = game->mode && game->mode->config.bots_as_ai; // !isBot || !mode.botsAsAI
    for (size_t i = 0; i < game->player_count && count < sizeof(all) / sizeof(all[0]); i++) {
        if (game->players[i] && !(bots_as_ai && game->players[i]->is_bot)) all[count++] = game->players[i];
    }
    mode_sort_leaders(game, all, count);
    if (count > max) count = max;
    memcpy(out, all, count * sizeof(*out));
    return count;
}

static void game_fill_board_from(const Player *const *sorted, const size_t count, NetLeaderboardPacket *board) {
    memset(board, 0, sizeof(*board));
    board->count = (uint8_t) count;

    for (size_t i = 0; i < count; i++) {
        board->entries[i].uid = sorted[i]->uid;
        board->entries[i].score = sorted[i]->score;
        board->entries[i].kills = (int32_t) sorted[i]->kills;
        board->entries[i].deaths = (int32_t) sorted[i]->deaths;
    }
}

static void game_fill_board(Game *game, NetLeaderboardPacket *board) {
    Player *sorted[GAME_LEADERBOARD_MAX];
    const size_t count = game_sorted_players(game, sorted, GAME_LEADERBOARD_MAX);

    memset(board, 0, sizeof(*board));
    board->count = (uint8_t) count;

    for (size_t i = 0; i < count; i++) {
        board->entries[i].uid = sorted[i]->uid;
        board->entries[i].score = sorted[i]->score;
        board->entries[i].kills = (int32_t) sorted[i]->kills;
        board->entries[i].deaths = (int32_t) sorted[i]->deaths;
    }
}

// players.syncLeaders(true, uid): the top 16 [sid, score] rows (KrunkNative adds kills/deaths).
void game_sync_leaders(Game *game, const int uid) {
    NetLeaderboardPacket board;
    game_fill_board(game, &board);
    game_send(game, uid, NET_PACKET_LEADERBOARD, &board, sizeof(board));

    // WP-E: the v7 "7" columns (mode.leaderStats, default ["score"]) in the same order
    NetLeaderStatsPacket stats;
    memset(&stats, 0, sizeof(stats));
    static const char *const score_only[] = {"score", NULL};
    const char *const *keys = game->mode && game->mode->config.leader_stats ? game->mode->config.leader_stats : score_only;
    while (keys[stats.stat_count] && stats.stat_count < NET_LEADER_STATS_MAX) stats.stat_count++;
    stats.count = board.count;
    for (int i = 0; i < board.count; i++) {
        const Player *player = game_find_player(game, board.entries[i].uid);
        stats.uid[i] = board.entries[i].uid;
        for (int k = 0; k < stats.stat_count; k++) stats.values[i][k] = mode_leader_stat_value(player, keys[k]);
    }
    game_send(game, uid, NET_PACKET_LEADER_STATS, &stats, sizeof(stats));

    if (uid == NET_TARGET_ALL) game->leaders_dirty = 0;
}

// game.init -> gameTimer = config.gameTime * 60000 (a non-positive clock falls back to 4:00).
void game_start_round(Game *game) {
    const float minutes = (float) game->config.game_time;

    game->round_timer = game->round_length > 0.0f ? game->round_length : minutes > 0.0f ? minutes * 60.0f : 240.0f;
    game->round_seconds_sent = -1;
    game->round_endless = 0;
    game->end_timer = 0.0f;
    game->kill_count = 0;
    game->nuke_timer = 0.0f;
    game->nuke_player = NULL;
    game->leaders_dirty = 1;
    memset(game->projectiles, 0, sizeof(game->projectiles));

    // finishInit: resetAll / resetPickups / triggerQueue / globalVals (WP-C), then the mode's clock
    // (gameTime), waitTimers, conditions, objectives, gameStart, then multiObjectTrigger("onGameStart")
    game_set_state(game, GAME_STATE_INITIALIZING); // v10 setGameState(INITIALIZING) at startGame
    forfeit_round_start(game);                     // v10 startGame: no forfeit vote, no starts
    trigger_round_reset(game);
    zombies_round_start(game); // WP-H finishInit: mystery boxes / krum machines, power-ups, airdrops
    if (game->mode) mode_round_start(game);
    world_round_start(game); // WP-B: resetZone, movedObjs, saveColls (shared/src/world.c)
    piss_round_start(game);  // KrunkNative Piss: no puddles
    // v10 startGame: a warmup (config.warmupTime) is WARMUP, else the wait timers make IDLE, else
    // the round is IN_PROGRESS (mode_round.c sets that when the timers drain)
    if (game->config.warmup_time > 0.0f) game_set_state(game, GAME_STATE_WARMUP);
    else if (mode_match_starting(game)) game_set_state(game, GAME_STATE_IDLE);
    else game_set_state(game, GAME_STATE_IN_PROGRESS);
    trigger_multi_event(game, TRIGGER_EVENT_ON_GAME_START, NULL);
}

static void game_send_timer(Game *game, const int uid, const int seconds, const int end) {
    const NetTimerPacket packet = {.seconds = seconds, .end = (uint8_t) end};
    game_send(game, uid, NET_PACKET_TIMER, &packet, sizeof(packet));
}

// endGame(): didWin = (team && team == winner) || player == winner; "end" carries didWin
// when there is a winner, else -1 (draw).
static void game_send_end(Game *game, Player *player, const ModeWinner winner, const NetLeaderboardPacket *board, const int32_t *rewards) {
    NetEndPacket packet;
    memset(&packet, 0, sizeof(packet));

    const int did_win = (player->team && player->team == winner.team) || player == winner.player;
    packet.result = (int8_t) (winner.team || winner.player ? did_win : -1);
    packet.next_round = (int32_t) ceilf(game->end_timer);
    packet.winner_uid = winner.player ? winner.player->uid : -1;
    packet.board = *board;
    memcpy(packet.rewards, rewards, sizeof(packet.rewards));

    game_send(game, player->uid, NET_PACKET_END, &packet, sizeof(packet));
}

// players.list.sort(mode.endSort || orderByScore), then the mode's winner.
static size_t game_end_order(Game *game, Player **sorted, ModeWinner *winner) {
    size_t count = 0;
    for (size_t i = 0; i < game->player_count && count < GAME_LEADERBOARD_MAX; i++) {
        if (game->players[i]) sorted[count++] = game->players[i];
    }
    if (game->mode) mode_sort_players(game, sorted, count);
    else count = game_sorted_players(game, sorted, GAME_LEADERBOARD_MAX);

    *winner = (ModeWinner) {0, NULL};
    if (game->mode) *winner = mode_pick_winner(game);
    else if (count) winner->player = sorted[0];
    return count;
}

// io_ent during the vote window (v3.1.1 server/packetHandlers.ts): a player who enters
// while the round is over still spawns (first spawn or respawn, no deathDelay check: the
// client's menu enforced it), and then gets `ctx.send("end", false, -1, endData, ...)`.
int game_end_late_spawn(const Game *game, const Player *player) {
    return game->ready && game->end_timer > 0.0f && !(player && player->active);
}

// That late "end" (didWin -1) for one uid: the endData rows as they stand.
void game_send_end_late(Game *game, const int uid) {
    if (!game->ready || game->end_timer <= 0.0f) return;

    Player *sorted[GAME_LEADERBOARD_MAX];
    ModeWinner winner;
    const size_t count = game_end_order(game, sorted, &winner);

    NetEndPacket packet;
    memset(&packet, 0, sizeof(packet));
    packet.result = -1;
    packet.next_round = (int32_t) ceilf(game->end_timer);
    packet.winner_uid = winner.player ? winner.player->uid : -1;
    game_fill_board_from((const Player *const *) sorted, count, &packet.board);
    for (size_t i = 0; i < count; i++) packet.rewards[i] = sorted[i]->reward;
    game_send(game, uid, NET_PACKET_END, &packet, sizeof(packet));
    game_send_match_votes(game, uid); // WP-E: the vote cards of this end screen
}

// game.endGame(3): the round clock ran out. Winner = best score, per-player "end"
// packets, end screen timer (config.endTimer), then players.clear().
void game_end_round(Game *game) {
    if (game->end_timer > 0.0f) return;

    const float end_timer = game_ruleset(game)->end_timer; // config.endTimer (Krunker Strike: 30 s)
    game->end_timer = end_timer > 0.0f ? end_timer : GAME_END_TIMER;
    game_set_state(game, GAME_STATE_POST_MATCH); // v10 endGame: setGameState(POST_MATCH)
    game->round_timer = 0.0f;
    game->round_seconds_sent = -1;
    game->nuke_timer = 0.0f;
    game->nuke_player = NULL;

    Player *sorted[GAME_LEADERBOARD_MAX];
    ModeWinner winner;
    size_t count = game_end_order(game, sorted, &winner);
    if (game->mode) { // v7 mode.onPreEnd / onEnd (Zombies wave score, Raid times), then the end order again (WP-D)
        mode_on_game_end(game);
        count = game_end_order(game, sorted, &winner);
    }

    // endGame(): player.reward for every player (hooks.reward: accounts live in the server). The
    // reward hook folds in additionalReward (claimed KR Packages), then it resets (game.js:191331).
    for (size_t i = 0; i < game->player_count; i++) {
        Player *player = game->players[i];
        if (!player) continue;
        player->reward = game->hooks.reward ? game->hooks.reward(game, player) : 0;
        player->additional_reward = 0;
    }

    NetLeaderboardPacket board;
    game_fill_board_from((const Player *const *) sorted, count, &board);
    int32_t rewards[GAME_LEADERBOARD_MAX] = {0};
    for (size_t i = 0; i < count; i++) rewards[i] = sorted[i]->reward;

    // Spectators / menu clients see the end screen without a result (the v3.1.1 server's
    // vote-window "end" with didWin -1).
    NetEndPacket watcher;
    memset(&watcher, 0, sizeof(watcher));
    watcher.result = -1;
    watcher.next_round = (int32_t) ceilf(game->end_timer);
    watcher.winner_uid = winner.player ? winner.player->uid : -1;
    watcher.board = board;
    memcpy(watcher.rewards, rewards, sizeof(rewards));
    game_send(game, NET_TARGET_NON_PLAYERS, NET_PACKET_END, &watcher, sizeof(watcher));

    for (size_t i = 0; i < game->player_count; i++) {
        Player *player = game->players[i];
        if (!player) continue;
        game_send_end_medals(game, player); // WP-E part 2c: endData.mdls
        game_send_end(game, player, winner, &board, rewards);
    }

    if (game->mode) mode_send_end(game, winner, sorted, count);
    bot_chat_on_match_end(game, &winner, sorted, count); // KrunkNative bot chat: one or two bots say gg (shared/src/bot_chat.c)
    ks_game_on_game_end(game); // KrunkScript onGameEnd(), after mode.onEnd (GJS:4665-4667)
    if (game_is_authority(game)) game_match_list_build(game); // WP-E: endData.mts + MATCH_VOTE_STATE

    memset(game->projectiles, 0, sizeof(game->projectiles));

    // players.clear(): nobody plays on the end screen (an in-process client keeps its
    // players for the endAnim, Game.end_keep_players; game_tick skips their regen/kills).
    for (size_t i = 0; i < game->player_count; i++) {
        Player *player = game->players[i];
        if (!player) continue;

        if (!game->end_keep_players) player->active = 0;
        player->input_queue_size = 0;
        free(player->input_queue);
        player->input_queue = NULL;
    }
}

void game_set_round_time(Game *game, const float seconds) {
    if (!game->ready || game->end_timer > 0.0f) return;

    game->round_timer = seconds > 0.0f ? seconds : 0.001f; // <= 0: the round ends on the next tick
    game->round_seconds_sent = -1;
}

// /endless (v3.1.1 dev cmd_endless, endTimer = "inf"). Intentional difference: the
// reference zeroes gameTimer (every client gets "t" 00:00) and turning it off restores a
// full gameTime round; KrunkNative pauses the clock instead. On: round_timer is kept and
// every client gets the frozen value; off: the countdown resumes from exactly there.
// Toggling never touches scores or restarts the round.
void game_set_endless(Game *game, const int on) {
    if (!game->ready || game->end_timer > 0.0f) return;
    if (game->round_endless == !!on) return;

    game->round_endless = (unsigned char) !!on;
    game->round_seconds_sent = -1; // off: game_tick_round resends the clock on the next tick

    const int clocked = !(game->mode && game->mode->config.timed) && game->round_timer > 0.0f;
    if (on && clocked) {
        game->round_seconds_sent = (int) game->round_timer;
        game_send_timer(game, NET_TARGET_ALL, game->round_seconds_sent, 0); // the frozen clock
    }
}

// What a client that connects mid-round needs: the clock, or the end screen.
void game_send_round_state(Game *game, const int uid) {
    if (!game->ready || !game_is_authority(game)) return;

    forfeit_send_to(game, uid); // a forfeit vote of the joiner's team in progress

    object_manager_send_state(game, uid); // KrunkNative: spawned objects first (their pickups sync below)
    piss_send_state(game, uid);           // KrunkNative Piss: the live puddles
    game_send_object_state(game, uid); // init dest / gates / banks (shared/src/triggers.c)
    world_send_state(game, uid);        // init zone / mov (shared/src/world.c, WP-B)
    zombies_send_state(game, uid);      // init pwup / mybx / krm, the wave, airdrops (WP-H)
    game_sync_grapples(game, uid);      // the others' Grappler ropes ("k" grapple slots)

    if (game->end_timer > 0.0f) {
        game_send_timer(game, uid, (int) game->end_timer, 1);
        game_send_match_votes(game, uid); // WP-E
        return;
    }

    if (game->mode && game->mode->config.timed) game_send_timer(game, uid, (int) game->mode->round_clock, 0);
    else if (game->round_timer > 0.0f) game_send_timer(game, uid, (int) game->round_timer, 0);
    if (game->mode) mode_send_state(game, uid);
}

void game_player_left(Game *game, Player *player) {
    game_bots_mimic_player_left(game, player); // bot_mimic: the controller leaving releases control
    forfeit_player_left(game, player); // the team forfeit vote keeps a leaver eligible for its grace
    mode_player_left(game, player); // players.remove: dropFlag
    piss_player_left(game, player);  // KrunkNative Piss: its puddles go
    game_player_drop_all(game, player); // players.remove: dropWeapon(player, true) (WP-C)
    trigger_player_left(game, player);  // its looping sounds stop, queued actions lose it (WP-C)
    if (game->nuke_player == player) {
        game->nuke_player = NULL;
        game->nuke_timer = 0.0f;
    }

    for (size_t i = 0; i < game->player_count; i++) {
        Player *other = game->players[i];
        if (!other || other == player) continue;

        for (int k = 0; k < PLAYER_DAMAGE_SLOTS; k++) {
            if (other->damage_received[k].uid == player->uid) other->damage_received[k].uid = -1;
        }

        if (other->last_killer_uid == player->uid) other->last_killer_uid = -1;
    }

    game->leaders_dirty = 1;
}

// game.update timers: the end screen countdown, else the round clock ("t" packets on
// every whole-second change, UTILS.getTime floors). Returns 1 when the end screen ran
// out and the hook must rotate the map.
static int game_tick_round(Game *game, const float delta) {
    if (game->end_timer > 0.0f) {
        game->end_timer -= delta;

        if (game->end_timer <= 0.0f) {
            game->end_timer = 0.0f;
            game_set_state(game, GAME_STATE_ENDED); // v10 endTimer <= 0: setGameState(ENDED), then the next game
            return 1;
        }

        const int seconds = (int) game->end_timer;
        if (seconds != game->round_seconds_sent) {
            game->round_seconds_sent = seconds;
            game_send_timer(game, NET_TARGET_ALL, seconds, 1);
        }

        return 0;
    }

    // the mode's waitTimers and conditions (106.js update); HOLD stops the clock
    const int state = game->mode ? mode_tick_round(game, delta) : MODE_ROUND_RUN;
    if (state == MODE_ROUND_END) {
        if (game->mode && mode_end_game(game)) game->round_seconds_sent = -1; // v7 rounds / overtime go on (WP-D)
        else game_end_round(game);
        return 0;
    }
    if (state == MODE_ROUND_HOLD) return 0;

    if (game->mode && game->mode->config.timed) { // Race: the clock counts up
        const int seconds = (int) game->mode->round_clock;
        if (seconds != game->round_seconds_sent) {
            game->round_seconds_sent = seconds;
            game_send_timer(game, NET_TARGET_ALL, seconds, 0);
        }
        return 0;
    }

    if (game->round_timer <= 0.0f && game->mode && game->mode->config.game_time == 0.0f) return 0; // gameTime 0: no clock
    if (game->round_endless) return 0; // /endless: endTimer "inf" skips the gameTimer countdown
    if (ks_game_round_frozen(game)) return 0; // KrunkScript GAME.TIME.freeze(): gameTimer 0 stops the countdown

    game->round_timer -= delta;

    if (game->round_timer <= 0.0f) {
        if (game->mode && mode_end_game(game)) game->round_seconds_sent = -1; // v7 rounds / overtime go on (WP-D)
        else game_end_round(game);
        return 0;
    }

    const int seconds = (int) game->round_timer;
    if (seconds != game->round_seconds_sent) {
        game->round_seconds_sent = seconds;
        game_send_timer(game, NET_TARGET_ALL, seconds, 0);
    }

    return 0;
}

// players.tickPlayer: every 0.5 s a player that was not hit for regenDelay heals
// maxHealth * regen (class regen, 0 when the config/mode disables regen).
static void game_tick_regen(Game *game, Player *player, const float delta) {
    if (player->hit_timer > 0.0f) player->hit_timer -= delta;

    player->regen_ticker -= delta;
    if (player->regen_ticker <= 0.0f) {
        player->regen_ticker = GAME_REGEN_TICK;
        player->regen_tick_count++; // v7.1.4 tickerC
        // v7.1.4: no regen while a Juggernaut, nor in challenge mode (WP-C: ENT challMode)
        const int regen_on = player->hit_timer <= 0.0f && !player->chall_mode && !player->is_juggernaut && game->config.health_regen &&
                             !(game->mode && game->mode->config.no_regen) && !player->mode.no_regen &&
                             game->classes && player->class_index >= 0 && player->class_index < (int) game->class_count;
        const float regen = regen_on ? game->classes[player->class_index].regen : 0.0f;
        if (regen > 0.0f) player_change_health(player, NULL, -(float) player->max_health * regen, 0);
        player_effects_regen_tick(player); // v7.1.4 Poisoned every 8th tick, Zapped every 2nd (shared/src/effects.c)
    }

    if (player->active) player_tick_effects(player, delta); // attributeTimers
}

void game_tick(Game *game, const float now, const float delta) {
    game_tick_world(game, now, delta, delta);
}

void game_tick_world(Game *game, const float now, const float delta, const float world_delta) {
    if (!game->ready) return;

    game->now = now;
    game->state_now += delta; // game.now of the stateHistory (lag compensation, player.c)

    const int authority = game_is_authority(game);
    int next_round = 0;
    if (!authority) world_builds_tick(game, delta); // KrunkNative: predicted builds the server never confirmed give way (build.c)

    if (authority) {
        next_round = game_tick_round(game, delta);

        if (!game->end_timer && game->nuke_timer && game->nuke_player) {
            game->nuke_timer = MAX(0.0f, game->nuke_timer - delta);

            if (!game->nuke_timer) {
                int nuke_reward = 0;
                const NetNukePacket detonated = {.state = 1};
                game_send(game, NET_TARGET_ALL, NET_PACKET_NUKE, &detonated, sizeof(detonated)); // v7.1.4 "n", 1

                for (size_t i = 0; i < game->player_count; i++) {
                    Player *player = game->players[i];
                    if (!player->active || player == game->nuke_player || (player->team && game->nuke_player->team == player->team) || player->god_mode) continue;

                    PlayerKillInfo kill_info = {
                        .streak = STREAK_NUKE + 1, // {stk: 0}: the kill feed's streak icon
                        .weapon_id = -1,
                    };

                    nuke_reward += 50;
                    player_kill(player, game->nuke_player, &kill_info, 1);
                }

                if (nuke_reward) {
                    // v7.1.4: "6" [Nuke medal, reward] and the "4" hit marker, then the score
                    NetKillMedalsPacket medals;
                    memset(&medals, 0, sizeof(medals));
                    medals.count = 1;
                    snprintf(medals.medals[0].text, sizeof(medals.medals[0].text), "Nuke");
                    medals.medals[0].score = nuke_reward;
                    medals.kills = (int32_t) game->nuke_player->kills;
                    game_send(game, game->nuke_player->uid, NET_PACKET_KILL_MEDALS, &medals, sizeof(medals));
                    const NetHitPacket hit = {.target_uid = -1}; // send("4"): the hit marker and hit_0, no damage number
                    game_send(game, game->nuke_player->uid, NET_PACKET_HIT, &hit, sizeof(hit));
                    player_score(game->nuke_player, nuke_reward, 1);
                }
                // v7 streaks.js: findTriggers("onNukeEnd", nukePlayer) in custom games; KN fires it on
                // every map (official maps have no nuke triggers)
                trigger_multi_event(game, TRIGGER_EVENT_ON_NUKE_END, game->nuke_player);
                // nukePlayer.account && isPubType() && randInt(1, 140) == 140: giveSkin(4040, "streak")
                if (game->nuke_player && game->hooks.give_skin && pcg32_boundedrand(140) == 139) game_give_skin(game, game->nuke_player, 4040, "streak");
            }
        }

        if (game->vote_kick) {
            game->vote_kick->timer = MAX(0.0f, game->vote_kick->timer - delta);

            if (!game->vote_kick->timer) {
                // v7/v10 game.update: voteToKick timed out: everyone's "vk" null (votekick.c)
                if (game->hooks.vote_kick_expired) game->hooks.vote_kick_expired(game);
                free(game->vote_kick);
                game->vote_kick = NULL;
            }
        }

        // v10 game.update: voteToForfeit's timer, the pass check (forfeit.c)
        forfeit_tick(game, delta);
    }

    // v7.1.4 game.update order (doc/v7/map-logic.md §2.0): 1 timers, 2 streaks, 3 vote-kick,
    // 4 bomb, 5 trigger timerObjs, 6 triggerQueue, 7 objective time, 8 power-ups, 9 bots, 10 players,
    // 11 player sounds, 12 AI, 14 airdrop, 15 teleporters, 16 spinners, 17 destructables, 18 flags,
    // 19 spawnables, 20 dom zones, 21-22 machines, 23 mode.update, 24 zone, 25 projectiles,
    // 26 KrunkScript update, 27 interact progress, 28 team scores, 29 leaders. The WP-A stubs
    // (world.h, triggers_v7.h, spawnables.h) sit at their v7 positions; the owning packages fill
    // them and move the existing calls (timerObjs / destructibles in game_tick_map_objects) as needed.
    if (authority && !game->end_timer) trigger_queue_update(game, delta); // #6 (WP-C)

    // v7.1.4 game.update: bots.update(delta * deltaMlt) before players.update (their input frames)
    if (authority && !game->end_timer) game_bots_tick(game, world_delta);
    if (authority) bot_chat_tick(game, delta); // bot chat lines go out on the end screen too (shared/src/bot_chat.c)

    for (size_t i = 0; i < game->player_count; i++) {
        Player *player = game->players[i];

        player_update(player, world_delta * game->config.delta_mlt);
        if (authority && player->om_held) object_manager_hold_tick(game, player); // KrunkNative: a dev's Physics Gun holds it

#ifdef KRUNKNATIVE_CLIENT
        // players.js update(): p.update(game, delta) then updateMsh(p), so the meshes
        // (first-person viewmodel included) show this frame's input like the camera does.
        player_update_meshes(player, 0);
#endif

        if (!authority || !player->active || game->end_timer > 0.0f) continue; // end screen: kept players only animate

        game_tick_regen(game, player, delta);
        if (!player->active) continue; // Poisoned / Zapped can kill in tickPlayer
        player_store_state(player); // players.js update: tickPlayer, storeState, then the deathY check

        // v7.1.4 players.js tickPlayer: y <= deathY kills unless godMode (noclip players too; v3.1.1
        // had no godMode exception). map-logic WP-B.
        if (!player->god_mode && player->position.y <= game->map->death_y) {
            // the CTF flag goes home in mode_on_kill (mode_in_kill_zone)
            player_kill(player, NULL, NULL, 0);
        }
    }

    if (authority && !game->end_timer) game_ai_tick(game, world_delta * 1000.0f * game->config.delta_mlt); // AI.update(delta * deltaMlt) + AI.sync
    if (authority && !game->end_timer) game_tick_projectiles(game, delta);
    zombies_tick(game, delta); // WP-H #8 power-ups, #14 airdrops, #21-22 machines (clients: timers, falls)
    game_tick_map_objects(game, delta * 1000.0f); // teleporter cooldowns; authority: onTimer triggers, destructible respawns
    if (authority && !game->end_timer) spawnables_update(game, delta); // #19 (WP-D)
    if (authority && !game->end_timer) piss_tick(game, delta); // KrunkNative Piss: puddle lifetimes and damage
    if (!authority) piss_client_tick(game, delta);
    drank_tick(game, delta); // KrunkNative Drank: laced timers (both sides)
    if (authority && !game->end_timer && game->mode) mode_tick(game, delta); // objectives, flags, tags, conversions, forceSpawn
    if (!authority && !game->end_timer && game->mode) mode_dom_client_tick(game, delta); // v7 dom captureT interpolation (WP-D)
    if (authority && !game->end_timer) zone_update(game, delta); // #24 (WP-B)
    world_tick(game, delta); // WP-B: ownTime clocks (players.tickPlayer), the client zone tween
    if (authority && game->leaders_dirty) game_sync_leaders(game, NET_TARGET_ALL);

    // Last: the hook re-inits the game (frees the players iterated above).
    if (next_round && game->hooks.next_round) game->hooks.next_round(game);
}

void game_players_add(Game *game, Player *player) {
    if (!game->player_count) {
        game->players = calloc(1, sizeof(Player *));
        if (!game->players) return;
    } else {
        Player **new_players = realloc(game->players, (game->player_count + 1) * sizeof(Player *));
        if (!new_players) return;

        game->players = new_players;
    }

    game->players[game->player_count++] = player;
    game->leaders_dirty = 1;

    mode_player_added(game, player); // players.add: setTeam + lives (authority only)
    if (!player->is_bot) game_bots_human_joined(game, player); // fill quota: a bot makes room at once
}

int game_pick_next_map(const Game *game) {
    if (!game->map_count) return 0;
    return (int) pcg32_boundedrand((uint32_t) game->map_count);
}
