// Krunker v7.1.4 game modes (shared/src/modes, doc/v7/map-logic.md §4.1) with simulated players
// on Sandstorm and the v7 fixtures (shared/tests/maps/v7_dom.json, v7_objectives.json): the mode
// table, team assignment, scoring, objectives (Hardpoint / KOTH zones by uid, CTF / Blitz /
// neutral flags, Kill Confirmed tags, Deposit KR Tags, Domination zones), waitTimer picks (Hide &
// Seek, Infected, Boss, Stalker, Simon, Traitor), lives, loadouts, the modifier modes, rounds /
// best of / overtime and every mode's end-of-round rule (conditions + winConditions), through
// the shared game hooks (no sockets). Run from the repository root: ./<build>/modes_test
#include <shared.h>
#include <spawnables.h>
#include <player_card.h>
#include <progression.h>
#include <prop_hunt.h>
#include <social_v7.h> // game_player_is_prop
#include <math.h>
#include <stdio.h>
#include <string.h>

#define MAX_SENT 4096

typedef struct {
    int uid;
    int type;
    uint16_t length;
    uint8_t payload[NET_MAX_PAYLOAD];
} SentPacket;

static SentPacket g_sent[MAX_SENT];
static int g_sent_count;
static int g_failures;
static int g_checks;
static int g_respawns[64];
static const cJSON *g_test_maps[3]; // sandstorm, v7_dom, v7_objectives

static void test_send(Game *game, const int uid, const int type, const void *payload, const uint16_t length) {
    (void) game;
    if (g_sent_count >= MAX_SENT) return;
    SentPacket *packet = &g_sent[g_sent_count++];
    packet->uid = uid;
    packet->type = type;
    packet->length = length;
    memcpy(packet->payload, payload, length);
}

static void test_respawn(Game *game, const int uid) {
    if (uid >= 0 && uid < 64) g_respawns[uid]++;
    Player *player = game_find_player(game, uid);
    if (player) player_spawn(player);
}

static const SentPacket *last_sent(const int type, const int uid) {
    for (int i = g_sent_count - 1; i >= 0; i--) {
        if (g_sent[i].type == type && (uid == -2 || g_sent[i].uid == uid)) return &g_sent[i];
    }
    return NULL;
}

static int count_sent(const int type) {
    int count = 0;
    for (int i = 0; i < g_sent_count; i++) count += g_sent[i].type == type;
    return count;
}

#define CHECK(cond, ...) do { \
    g_checks++; \
    if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_failures++; } \
} while (0)

// ------------------------------------------------------------------ fixtures

static void free_players(Game *game) {
    for (size_t i = 0; i < game->player_count; i++) {
        Player *player = game->players[i];
        free(player->loadout);
        free(player->ammo);
        free(player->reloads);
        free(player->input_queue);
        free(player);
    }
    free(game->players);
    game->players = NULL;
    game->player_count = 0;
}

static void start_mode_on(Game *game, const int mode_id, const int map) {
    free_players(game);
    int *modes = calloc(1, sizeof(int));
    modes[0] = mode_id;
    const cJSON **maps = calloc(1, sizeof(cJSON *)); // game_configure owns (frees) the lists
    maps[0] = g_test_maps[map];
    game_configure(game, NULL, maps, 1, modes, 1, NULL, 0, NULL, 0);
    game->hooks.send = test_send;
    game->hooks.respawn = test_respawn;
    g_sent_count = 0;
    memset(g_respawns, 0, sizeof(g_respawns));
    game_init(game, 0, 0, 1);
    game->now = 0.0f;
}

static void start_mode(Game *game, const int mode_id) {
    start_mode_on(game, mode_id, 0);
}

static int weapon_exists(const Game *game, const int id) {
    return id >= 0 && (size_t) id < game->weapon_count && game->weapons[id];
}

static const SentPacket *count_type(const int type, int *count) {
    const SentPacket *last = NULL;
    *count = 0;
    for (int i = 0; i < g_sent_count; i++) {
        if (g_sent[i].type == type) {
            (*count)++;
            last = &g_sent[i];
        }
    }
    return last;
}

static Player *add_player(Game *game, const int uid, const int class_index) {
    Player *player = player_init(game);
    player->uid = uid;
    player->class_index = class_index;
    snprintf(player->name, sizeof(player->name), "P%d", uid);
    game_players_add(game, player);
    player_spawn(player);
    return player;
}

// Standing still somewhere: `y` at the feet.
static void place(Player *player, const float x, const float y, const float z) {
    player->position = (vec3) {x, y, z};
    player->last_position = player->position;
    player->velocity = (vec3) {0.0f, 0.0f, 0.0f};
}

static void tick(Game *game, const float seconds, const float step) {
    for (float t = 0.0f; t < seconds - 1e-4f; t += step) game_tick(game, game->now + step, step);
}

static void kill(Player *victim, Player *killer, const int weapon_id) {
    PlayerKillInfo info = {.weapon_id = weapon_id};
    player_kill(victim, killer, killer ? &info : NULL, 0);
}

static int ended(const Game *game) {
    return game->end_timer > 0.0f;
}

static NetModeEndPacket last_end(void) {
    NetModeEndPacket packet;
    memset(&packet, 0, sizeof(packet));
    const SentPacket *sent = last_sent(NET_PACKET_MODE_END, -2);
    if (sent) memcpy(&packet, sent->payload, sizeof(packet));
    return packet;
}

static int end_result_for(const int uid) {
    const SentPacket *sent = last_sent(NET_PACKET_END, uid);
    if (!sent) return -99;
    NetEndPacket packet;
    memcpy(&packet, sent->payload, sizeof(packet));
    return packet.result;
}

static int team_count(const Game *game, const int team) {
    int count = 0;
    for (size_t i = 0; i < game->player_count; i++) count += game->players[i]->team == team;
    return count;
}

static Player *first_on_team(const Game *game, const int team) {
    for (size_t i = 0; i < game->player_count; i++) {
        if (game->players[i]->team == team) return game->players[i];
    }
    return NULL;
}

// ------------------------------------------------------------------ table

static void test_table(void) {
    printf("Running mode table test...\n");
    static const char *const names[MODE_COUNT] = {
        "Free for All", "Team Deathmatch", "Hardpoint", "Capture the Flag", "Parkour", "Hide & Seek", "Infected",
        "Race", "Last Man Standing", "Simon Says", "Gun Game", "Prop Hunt", "Boss Hunt", "Classic FFA", "Deposit",
        "Stalker", "King of the Hill", "One in the Chamber", "Trade", "Kill Confirmed", "Defuse", "Sharp Shooter",
        "Traitor", "Raid", "Blitz", "Domination", "Squad Deathmatch", "Kranked FFA", "Team Defender", "Deposit FFA",
        "Carrier", "All or Nothing", "Mag Dump", "Chaos Snipers", "Bighead FFA", "Zombies", "Impulsed",
    };
    static const char *const keys[MODE_V7_TABLE_SIZE] = {
        "ffa", "tdm", "point", "ctf", "bhop", "hide", "infect", "race", "lms", "simon", "gun", "prop", "boss", "clas",
        "depo", "stalk", "king", "oinc", "trade", "kc", "def", "shrp", "trai", "raid", "blitz", "dom", "sdm", "krank",
        "tdf", "depoffa", "car", "aon", "md", "chs", "bhffa", "zom", "imp",
    };

    int count = 0;
    const GameModeConfig *modes = mode_list(&count);
    CHECK(count == 37 && mode_count() == 37 && MODE_V7_TABLE_SIZE == 37, "v7.1.4 has 37 modes (%d)", count);
    for (int i = 0; i < count; i++) {
        CHECK(modes[i].id == i && modes[i].name && !strcmp(modes[i].name, names[i]), "mode %d is %s (%s)", i, names[i], modes[i].name);
        if (i < MODE_V7_TABLE_SIZE) CHECK(modes[i].key && !strcmp(modes[i].key, keys[i]), "mode %d key %s", i, keys[i]);
    }

    CHECK(mode_find("tdm") == MODE_ID_TDM && mode_find("Capture the Flag") == MODE_ID_CTF && mode_find("19") == MODE_ID_KC, "mode_find");
    CHECK(mode_find("ranked") == -1 && mode_find("nope") == -1 && mode_find("dom") == 25, "mode_find: no ranked mode / unknown / dom");
    CHECK(MODE_ID_CLAS == 13 && MODE_ID_DEPO == 14 && MODE_ID_RESERVED_RANKED_37 == 37 && MODE_ID_RESERVED_RANKED_38 == 38 &&
          !mode_config(MODE_ID_RESERVED_RANKED_37) && !mode_config(MODE_ID_RESERVED_RANKED_38) && !mode_hostable(37) && !mode_hostable(38),
          "v7 ids 13 / 14; 37 / 38 reserved for ranked, no mode");
    CHECK(g_rotation_modes[0] == 0 && g_rotation_modes[1] == 2 && g_rotation_modes[2] == 3 && g_rotation_modes[3] == 19 &&
          g_rotation_modes[4] == 25, "rotationModes [0, 2, 3, 19, 25]");
    CHECK(mode_hostable(MODE_ID_RAID) && mode_hostable(MODE_ID_GUN) &&
          !mode_hostable(MODE_ID_CAR) && !mode_hostable(MODE_ID_AON) && !mode_hostable(MODE_ID_MD) && !mode_hostable(MODE_ID_ZOM) &&
          !mode_hostable(MODE_ID_IMP) && mode_hostable(MODE_ID_DOM), "preventCustomGames (raid hostable again)");
    // v10 zom custUI: switchLeaderboard(true, true), the kill feed in the chat; every other mode leaves it to the setting
    int forced_old = 0;
    for (int id = 0; id < MODE_COUNT; id++) forced_old += mode_config(id) && mode_config(id)->force_old_scoreboard;
    CHECK(mode_config(MODE_ID_ZOM)->force_old_scoreboard && forced_old == 1, "forceOldScoreboard: Zombies only (%d modes)", forced_old);

    const GameModeConfig *stalk = mode_config(MODE_ID_STALK);
    CHECK(stalk->has_fog_dist && stalk->fog_dist == 250.0f && stalk->has_fog_col && stalk->fog_col == 1842204 &&
          stalk->has_sky_col && stalk->sky_col == 1842204 && stalk->has_light_col && stalk->light_col == 855309 &&
          stalk->has_amb_col && stalk->amb_col == 1513239, "Stalker lighting overrides");
    const GameModeConfig *ffa = mode_config(MODE_ID_FFA);
    CHECK(!ffa->has_fog_dist && !ffa->has_fog_col && !ffa->has_sky_col && !ffa->has_light_col && !ffa->has_amb_col, "FFA keeps the map lighting");
    CHECK(mode_config(MODE_ID_INFECT)->speed_mlt[2] == 1.15f && mode_config(MODE_ID_HIDE)->speed_mlt[1] == 0.8f, "spdMlt");
    // v10.0.2 weaponOrder (v10:105622): v7.1.4's plus the Charge Rifle (28) after the Sniper
    static const int order[] = {2, 27, 16, 10, 4, 11, 21, 5, 3, 9, 1, 14, 6, 0, 28, 18, 7, 8, 13, 22, 12};
    const GameModeConfig *gun = mode_config(MODE_ID_GUN);
    CHECK(gun->weapon_order_size == 21 && !memcmp(gun->weapon_order, order, sizeof(order)), "Gun Game v10 order");
    CHECK(!gun->no_reward && !gun->no_streaks && mode_streak_disabled(NULL, 4) == 0, "gun: rewards and streaks on in v7");
    CHECK(mode_config(MODE_ID_KING)->kill_mlt == 0.15f && mode_config(MODE_ID_KING)->obj_score == 10 && !mode_config(MODE_ID_KING)->no_score_k, "king keys");
    CHECK(mode_config(MODE_ID_DOM)->capture_time == 10.0f && mode_config(MODE_ID_DOM)->lockout_mlt == 1.5f && mode_config(MODE_ID_DOM)->per_player_mlt == 0.2f, "dom keys");
    CHECK(mode_config(MODE_ID_SDM)->multi_team == 4 && mode_config(MODE_ID_DEPO)->tag_hold_limit == 20 && mode_config(MODE_ID_DEPOFFA)->obj_score == 75, "sdm / depo keys");
    CHECK(mode_config(MODE_ID_CHS)->grav_mlt == 0.92f && mode_config(MODE_ID_IMP)->expl_mlt == 5.0f && mode_config(MODE_ID_AON)->has_ammo_limit &&
          mode_config(MODE_ID_AON)->ammo_limit == 0 && mode_config(MODE_ID_CLAS)->no_melee, "modifier keys");
}

// ------------------------------------------------------------------ FFA / TDM

static void test_ffa(Game *game) {
    printf("Running FFA test...\n");
    start_mode(game, MODE_ID_FFA);
    Player *a = add_player(game, 1, 0);
    Player *b = add_player(game, 2, 0);
    CHECK(a->team == 0 && b->team == 0, "FFA has no teams");

    kill(b, a, 1);
    CHECK(a->score > 0 && mode_team_score(game, 0) == 0, "kill score %d, no team score", a->score);

    game->round_timer = 0.5f;
    tick(game, 1.0f, 0.5f);
    CHECK(ended(game), "time out ends the round");
    CHECK(end_result_for(1) == 1 && end_result_for(2) == 0, "FFA: the best score wins (%d / %d)", end_result_for(1), end_result_for(2));
}

// NET_EXT_KN_SWAP_TEAM (mode_swap_team, the Tap-to-Play switch): between lives only (a living player keeps
// its team), a team mode with selTeam and not a comp lobby, teams 1 and 2; the change is broadcast as "tm".
static void test_swap_team(Game *game) {
    printf("Running Switch Teams test...\n");
    start_mode(game, MODE_ID_TDM);
    game->config.select_team = 1;
    Player *a = add_player(game, 1, 0);
    Player *b = add_player(game, 2, 0);
    const int other = a->team == 1 ? 2 : 1;
    const int b_team = b->team;
    CHECK(a->team == 1 || a->team == 2, "a TDM player has a team (%d)", a->team);
    CHECK(!mode_swap_team(game, a, other) && a->team != other, "a living player keeps its team");
    a->active = 0; // between lives: the Tap-to-Play screen
    g_sent_count = 0;
    CHECK(!mode_swap_team(game, a, 0) && !mode_swap_team(game, a, 3), "only teams 1 and 2");
    CHECK(!mode_swap_team(game, a, a->team), "the same team: nothing changes");
    CHECK(mode_swap_team(game, a, other) == 1 && a->team == other, "between lives the team flips");
    int sent = 0;
    const SentPacket *tm = count_type(NET_PACKET_TEAM, &sent);
    CHECK(sent >= 1 && tm, "the change is broadcast as NET_PACKET_TEAM (%d)", sent);
    if (tm) {
        NetTeamPacket team;
        memcpy(&team, tm->payload, sizeof(team));
        CHECK(team.uid == a->uid && team.team == other, "NET_PACKET_TEAM: uid %d team %d", team.uid, team.team);
    }
    CHECK(b->team == b_team, "the other player keeps its team");
    game->config.comp = 1;
    CHECK(!mode_swap_team(game, a, a->team == 1 ? 2 : 1), "a comp lobby's roster picks the teams");
    game->config.comp = 0;
    game->config.select_team = 0;
    CHECK(!mode_swap_team(game, a, a->team == 1 ? 2 : 1), "selTeam off: no switch");
    start_mode(game, MODE_ID_FFA);
    game->config.select_team = 1;
    Player *f = add_player(game, 1, 0);
    f->active = 0;
    CHECK(!mode_swap_team(game, f, 1) && f->team == 0, "FFA has no teams to switch to");
}

static void test_tdm(Game *game) {
    printf("Running TDM test...\n");
    start_mode(game, MODE_ID_TDM);
    Player *a = add_player(game, 1, 0);
    Player *b = add_player(game, 2, 0);
    Player *c = add_player(game, 3, 0);
    CHECK(a->team == 2 && b->team == 1 && c->team == 2, "setTeam balances (%d %d %d)", a->team, b->team, c->team);

    kill(b, a, 1);
    const int reward = a->score;
    CHECK(reward > 0 && mode_team_score(game, 2) == reward && mode_team_score(game, 1) == 0, "team score = kill score (%d / %d)", mode_team_score(game, 2), reward);
    CHECK(last_sent(NET_PACKET_TEAM_SCORE, NET_TARGET_ALL) != NULL, "ts broadcast");

    PlayerKillInfo info = {.weapon_id = 1};
    CHECK(!player_change_health(c, a, 500.0f, 0), "no friendly fire (dmgTeam off)");
    (void) info;

    game->round_timer = 0.1f;
    tick(game, 0.5f, 0.25f);
    CHECK(ended(game), "TDM round over");
    CHECK(end_result_for(1) == 1 && end_result_for(3) == 1 && end_result_for(2) == 0, "team 2 wins (%d %d %d)", end_result_for(1), end_result_for(3), end_result_for(2));
    const NetModeEndPacket end = last_end();
    CHECK(end.winner_team == 2 && end.team_scores[1] == reward, "MODE_END winner %d scores %d/%d", end.winner_team, end.team_scores[0], end.team_scores[1]);

    // draw: no score at all
    start_mode(game, MODE_ID_TDM);
    add_player(game, 1, 0);
    add_player(game, 2, 0);
    game->round_timer = 0.1f;
    tick(game, 0.5f, 0.25f);
    CHECK(end_result_for(1) == -1 && end_result_for(2) == -1, "0 - 0 is a draw");
}

// ------------------------------------------------------------------ objectives

static void test_point(Game *game) {
    printf("Running Hardpoint test...\n");
    start_mode(game, MODE_ID_POINT);
    GameMode *mode = game->mode;
    CHECK(mode->objective_count == 4, "Sandstorm has 4 objectives (%d)", mode->objective_count);
    CHECK(mode->objectives[0].uid == 0 && mode->objectives[3].uid == 3, "auto uids 0..3 (%d %d)", mode->objectives[0].uid, mode->objectives[3].uid);
    CHECK(mode->active_objective == 0 && mode->wait_timer_count == 1 && mode->wait_timers[0].cont_time, "gameStart: objective uid 0 + rotation timer");
    CHECK(fabsf(mode->objective_time - 60.0f) < 0.01f, "objtvTime 1 min (%.1f)", mode->objective_time);

    Player *a = add_player(game, 1, 0);
    Player *b = add_player(game, 2, 0);
    const ModeObjective *objective = &mode->objectives[0];
    place(a, objective->position.x, objective->position.y - objective->half.y, objective->position.z);
    place(b, 1000.0f, 1000.0f, 1000.0f);

    tick(game, 1.5f, 0.25f);
    CHECK(a->score == 10 && b->score == 0, "objScore 10 per 1.5 s in the zone (%d / %d)", a->score, b->score);
    CHECK(a->mode.objective_score == 10, "objectiveScore %d", a->mode.objective_score);
    CHECK(mode_team_score(game, a->team) == 10, "Hardpoint: zone points feed the team (%d)", mode_team_score(game, a->team));

    // two on the zone: both score (v7 point.js), not only a lone occupant
    place(b, objective->position.x + 2.0f, objective->position.y - objective->half.y, objective->position.z);
    tick(game, 1.5f, 0.25f);
    CHECK(a->score == 20 && b->score == 10, "everyone on the hardpoint scores (%d / %d)", a->score, b->score);

    const int before = a->score;
    const int team_before = mode_team_score(game, a->team);
    kill(b, a, 1);
    CHECK(a->score > before && mode_team_score(game, a->team) == team_before, "Hardpoint: kill score is not team score");

    tick(game, 60.0f, 0.5f);
    CHECK(mode->active_objective == 1, "next objective (uid + 1) after 60 s (%d)", mode->active_objective);
    const SentPacket *sent = last_sent(NET_PACKET_OBJECTIVE_V7, NET_TARGET_ALL);
    CHECK(sent != NULL, "obj v7 broadcast");
    if (sent) {
        NetObjectiveV7Packet obj;
        memcpy(&obj, sent->payload, sizeof(obj));
        CHECK(obj.uid == 1 && !obj.initial && obj.time_ms > 59000, "obj uid %d initial %d ms %d", obj.uid, obj.initial, obj.time_ms);
    }

    mode->team_scores[1] = 5;
    mode->team_scores[2] = 30;
    game->round_timer = 0.1f;
    tick(game, 0.5f, 0.25f);
    CHECK(ended(game) && end_result_for(a->team == 2 ? 1 : 2) == 1, "the higher team score wins");
}

// king.js (v7): a random hill, +10 every 1.5 s only for a lone occupant, "uobj" occupancy, kill
// score x (killMlt 0.15 + player.killMlt), Defender (+50, killMlt += 0.05) / Attacker (+25)
static void test_king(Game *game) {
    printf("Running King of the Hill test...\n");
    start_mode(game, MODE_ID_KING);
    GameMode *mode = game->mode;
    CHECK(mode->active_objective >= 0 && mode->active_objective < 4 && mode->wait_timer_count == 1, "gameStart: a random hill (%d)", mode->active_objective);

    Player *a = add_player(game, 1, 0);
    Player *b = add_player(game, 2, 0);
    CHECK(a->team == 0 && b->team == 0, "KOTH is FFA");
    const ModeObjective *hill = mode_objective_by_uid(game, mode->active_objective);
    place(a, hill->position.x, hill->position.y - hill->half.y, hill->position.z);
    place(b, 1000.0f, 1000.0f, 1000.0f);
    tick(game, 1.5f, 0.25f);
    CHECK(a->score == 10 && hill->on_zone == 1, "a lone king scores 10 (%d, on %d)", a->score, hill->on_zone);
    CHECK(last_sent(NET_PACKET_OBJECTIVE_OCCUPANCY, NET_TARGET_ALL) != NULL, "uobj");

    place(b, hill->position.x + 2.0f, hill->position.y - hill->half.y, hill->position.z);
    tick(game, 1.5f, 0.25f);
    CHECK(a->score == 10 && b->score == 0 && hill->on_zone == 2, "contested hill: nobody scores (%d / %d)", a->score, b->score);

    // a kill from the hill: Defender +50, killMlt += hillMlt; the kill reward x (0.15 + killMlt)
    int score = a->score;
    kill(b, a, 1);
    const int reward = a->score - score - 50;
    CHECK(a->mode.kill_mlt > 0.049f && a->mode.kill_mlt < 0.051f, "Defender raises killMlt (%.2f)", a->mode.kill_mlt);
    CHECK(reward > 0 && reward < 20, "kill reward x 0.15 (%d)", reward);

    // a kill of the king from outside: Attacker +25
    player_spawn(b);
    place(b, 1000.0f, 1000.0f, 1000.0f);
    tick(game, 0.1f, 0.1f);
    score = b->score;
    place(a, hill->position.x, hill->position.y - hill->half.y, hill->position.z);
    tick(game, 0.1f, 0.1f);
    kill(a, b, 1);
    CHECK(b->score - score >= 25, "Attacker +25 (%d)", b->score - score);

    game->round_timer = 0.1f;
    tick(game, 0.5f, 0.25f);
    CHECK(ended(game), "KOTH: the clock ends it");
}

static void test_ctf(Game *game) {
    printf("Running CTF test...\n");
    start_mode(game, MODE_ID_CTF);
    GameMode *mode = game->mode;
    CHECK(mode->flag_count == 2 && mode->flags[0].team == 1 && mode->flags[1].team == 2, "flags: team 1 (tm 0) and team 2 (tm \"1\")");
    CHECK(fabsf(mode->flags[0].origin.y - (33.0f + 12.0f)) < 0.01f, "the crystal floats flagScale / 2 + flagOff up");
    // each flag's uid is its map object's id (the index NET_OBJECT packets use), and the flag is in play
    for (int f = 0; f < mode->flag_count; f++) {
        const int id = mode->flags[f].uid;
        CHECK(id >= 0 && (size_t) id < game->map->object_count && game->map->objects[id] && game->map->objects[id]->raw &&
              game->map->objects[id]->prefab == PREFAB_FLAG && mode->flags[f].active, "flag %d is map object %d, in play", f, id);
    }

    Player *red = add_player(game, 1, 0);   // team 2
    Player *blue = add_player(game, 2, 0);  // team 1
    CHECK(red->team == 2 && blue->team == 1, "teams %d / %d", red->team, blue->team);

    const ModeFlag *blue_flag = &mode->flags[0];
    const ModeFlag *red_flag = &mode->flags[1];
    place(blue, 1000.0f, 1000.0f, 1000.0f);

    // red takes the blue flag
    place(red, blue_flag->origin.x, blue_flag->origin.y - 6.0f, blue_flag->origin.z);
    tick(game, 0.1f, 0.1f);
    CHECK(blue_flag->carrier_uid == red->uid && red->mode.flag == 1, "pickup (carrier %d)", blue_flag->carrier_uid);
    CHECK(red->score == 20 && !strcmp(mode->flag_info[1], red->name), "pickup +20, flag info = carrier (%s)", mode->flag_info[1]);

    // ... and carries it home: capture
    place(red, red_flag->origin.x, red_flag->origin.y - 12.0f, red_flag->origin.z);
    tick(game, 0.1f, 0.1f);
    CHECK(red->mode.caps == 1 && mode_team_score(game, 2) == 1 && red->score == 170, "capture: caps %d team %d score %d", red->mode.caps, mode_team_score(game, 2), red->score);
    CHECK(blue_flag->carrier_uid < 0 && blue_flag->position.x == blue_flag->origin.x, "the captured flag goes home");

    // the carrier dies: the flag drops where it fell, a defender returns it
    place(red, blue_flag->origin.x, blue_flag->origin.y - 6.0f, blue_flag->origin.z);
    tick(game, 0.1f, 0.1f);
    place(red, 0.0f, 100.0f, 0.0f);
    kill(red, blue, 1);
    CHECK(blue_flag->carrier_uid < 0 && fabsf(blue_flag->position.y - 106.0f) < 0.01f && !strcmp(mode->flag_info[1], "AWAY"), "drop (y %.1f, %s)", blue_flag->position.y, mode->flag_info[1]);
    place(blue, 0.0f, 100.0f, 0.0f);
    const int blue_score = blue->score;
    tick(game, 0.1f, 0.1f);
    CHECK(blue_flag->position.x == blue_flag->origin.x && blue->score == blue_score + 50 && !strcmp(mode->flag_info[1], "AT BASE"), "return +50");
    CHECK(mode_team_score(game, 2) == 1, "player score does not feed the team score (noScoreC)");

    game->round_timer = 0.1f;
    tick(game, 0.5f, 0.25f);
    CHECK(end_result_for(red->uid) == 1 && end_result_for(blue->uid) == 0, "more captures win");
}

void mode_send_dom_update(Game *game, const int *zones, const int count, const int uid); // mode_net.c
// Domination zones: each zone is its map object (the id NET_OBJECT packets use), and the DOM_UPDATE to a joiner
// carries that id (protocol 1.4.4).
static void test_dom_zone_objects(Game *game) {
    printf("Running domination zone object id test...\n");
    start_mode_on(game, MODE_ID_DOM, 1);
    GameMode *mode = game->mode;
    int ok = mode->dom_zone_count > 0;
    for (int z = 0; z < mode->dom_zone_count; z++) {
        const int id = mode->dom_zones[z].object_id;
        ok = ok && id >= 0 && (size_t) id < game->map->object_count && game->map->objects[id] && game->map->objects[id]->prefab == PREFAB_DOMINATION_ZONE;
    }
    CHECK(ok, "each domination zone is its map object (%d zones)", mode->dom_zone_count);
    int zones[MODE_MAX_DOM_ZONES];
    for (int z = 0; z < mode->dom_zone_count && z < MODE_MAX_DOM_ZONES; z++) zones[z] = z;
    g_sent_count = 0;
    mode_send_dom_update(game, zones, mode->dom_zone_count, 1);
    const SentPacket *sent = last_sent(NET_PACKET_DOM_UPDATE, 1);
    CHECK(sent && sent->length == sizeof(NetDomUpdatePacket), "DOM_UPDATE to the joiner");
    if (sent) {
        NetDomUpdatePacket update;
        memcpy(&update, sent->payload, sizeof(update));
        int same = update.count == mode->dom_zone_count;
        for (int z = 0; z < update.count && z < mode->dom_zone_count; z++) same = same && update.zones[z].object_id == mode->dom_zones[z].object_id;
        CHECK(same, "the DOM_UPDATE carries each zone's map object id (%d zones)", update.count);
    }
}

static void test_kc(Game *game) {
    printf("Running Kill Confirmed test...\n");
    start_mode(game, MODE_ID_KC);
    Player *a = add_player(game, 1, 0); // team 2
    Player *b = add_player(game, 2, 0); // team 1
    Player *c = add_player(game, 3, 0); // team 2
    place(a, 0.0f, 500.0f, 0.0f);
    place(b, 0.0f, 500.0f, 50.0f);
    place(c, 200.0f, 500.0f, 200.0f);

    kill(b, a, 1);
    CHECK(mode_team_score(game, 2) == 0, "kills alone do not score (noScoreC)");
    CHECK(game->mode->spawnables[0].active && game->mode->spawnables[0].team == 1, "the victim drops a tag of its team");

    place(a, 0.0f, 500.0f, 50.0f);
    tick(game, 0.1f, 0.1f);
    CHECK(a->mode.confirms == 1 && mode_team_score(game, 2) == 1 && !game->mode->spawnables[0].active, "confirm +1 team");

    kill(c, a, 1); // hmm: teammates cannot normally kill, the tag is still dropped
    player_spawn(b);
    place(b, 200.0f, 500.0f, 200.0f);
    tick(game, 0.1f, 0.1f);
    CHECK(b->mode.denies == 0 && b->mode.confirms == 1, "b (team 1) confirms c's tag (team 2)");

    // a tag expires after 8 s
    player_spawn(c);
    place(c, 300.0f, 500.0f, 300.0f);
    kill(c, b, 1);
    tick(game, 8.5f, 0.5f);
    int active = 0;
    for (int i = 0; i < MODE_MAX_SPAWNABLES; i++) active += game->mode->spawnables[i].active;
    CHECK(active == 0, "tags vanish after 8 s");

    // deny: picking up your own team's tag
    player_spawn(c);
    place(c, 400.0f, 500.0f, 400.0f);
    kill(c, b, 1);
    place(a, 400.0f, 500.0f, 400.0f);
    tick(game, 0.1f, 0.1f);
    CHECK(a->mode.denies == 1, "deny (%d)", a->mode.denies);

    // tagLimit 65 (team score) ends the round, the higher team score wins
    game->mode->team_scores[2] = 65;
    tick(game, 0.1f, 0.1f);
    CHECK(ended(game) && end_result_for(a->uid) == 1 && end_result_for(b->uid) == 0, "tag limit: team 2 wins");

    // a tie: denies, then kills, then score (team 2's "score" sums kills: v7.1.4 quirk)
    start_mode(game, MODE_ID_KC);
    a = add_player(game, 1, 0); // team 2
    b = add_player(game, 2, 0); // team 1
    b->mode.denies = 1;
    CHECK(mode_pick_winner(game).team == 1, "tie: more denies win");
    b->mode.denies = 0;
    a->kills = 2;
    CHECK(mode_pick_winner(game).team == 2, "tie: more kills win");
    a->kills = 0;
    b->score = 100;
    CHECK(mode_pick_winner(game).team == 1, "tie: team 1 score");
    b->score = 0;
    a->score = 500;
    CHECK(mode_pick_winner(game).team == 2, "tie: team 2 'score' is its kills (0 vs 0): team 2");
}

// ------------------------------------------------------------------ picks / conversions

static void test_hide(Game *game) {
    printf("Running Hide & Seek test...\n");
    start_mode(game, MODE_ID_HIDE);
    Player *a = add_player(game, 1, 0);
    CHECK(a->team == 1 && a->class_index == 9, "Hiders start on team 1 with no weapons (class %d)", a->class_index);

    tick(game, 12.0f, 1.0f);
    CHECK(game->mode->wait_timer_count == 2 && game->mode->wait_timers[0].time > 9.0f, "waits for minPlayers 2");
    const SentPacket *message = last_sent(NET_PACKET_GAME_MESSAGE, NET_TARGET_ALL);
    CHECK(message && !strcmp((const char *) message->payload, NET_GAME_MESSAGE_WAITING), "gmsg wt");

    Player *b = add_player(game, 2, 0);
    Player *c = add_player(game, 3, 0);
    place(a, -300.0f, 500.0f, 0.0f);
    place(b, 0.0f, 500.0f, 300.0f);
    place(c, 300.0f, 500.0f, 0.0f);
    tick(game, 10.5f, 0.5f);
    Player *seeker = first_on_team(game, 2);
    CHECK(seeker && team_count(game, 2) == 1 && seeker->mode.locked, "a locked seeker is picked");
    message = last_sent(NET_PACKET_GAME_MESSAGE, NET_TARGET_ALL);
    CHECK(message && !strncmp((const char *) message->payload, "releasing seeker in ", 20), "next gmsg: %s", message ? (const char *) message->payload : "-");

    tick(game, 44.5f, 0.5f);
    CHECK(!seeker->mode.locked && game->mode->wait_timer_count == 0, "released after 45 s");

    // touching converts (and kills: killConv)
    Player *hider = seeker == b ? c : b;
    place(seeker, 0.0f, 500.0f, 0.0f);
    place(hider, 3.0f, 500.0f, 0.0f);
    place(seeker == a ? c : a, 500.0f, 500.0f, 500.0f);
    const float clock = game->round_timer;
    tick(game, 0.1f, 0.1f);
    CHECK(hider->team == 2 && !hider->active && seeker->mode.convs == 1 && seeker->score == 100, "found: team %d convs %d score %d", hider->team, seeker->mode.convs, seeker->score);
    CHECK(game->round_timer > clock + 9.0f, "gameTimer += 10 s");

    // the last hider is found: seekers win
    Player *last = first_on_team(game, 1);
    place(last, 0.0f, 500.0f, 2.0f);
    tick(game, 0.2f, 0.1f);
    CHECK(ended(game) && end_result_for(seeker->uid) == 1 && end_result_for(last->uid) == 1, "no hiders left: seekers win");
}

static void test_infect(Game *game) {
    printf("Running Infected test...\n");
    start_mode(game, MODE_ID_INFECT);
    Player *a = add_player(game, 1, 0);
    Player *b = add_player(game, 2, 0);
    Player *c = add_player(game, 3, 0);
    CHECK(a->team == 1 && b->team == 1 && c->team == 1, "everyone starts a Survivor");
    place(a, 0.0f, 500.0f, 0.0f);
    place(b, 300.0f, 500.0f, 0.0f);
    place(c, 600.0f, 500.0f, 0.0f);

    tick(game, 20.5f, 0.5f);
    Player *zombie = first_on_team(game, 2);
    CHECK(zombie && team_count(game, 2) == 1 && !zombie->active, "infection: one player, killed");
    CHECK(zombie && zombie->cosmetics.skin_color == MODE_INFECTED_SKIN && zombie->cosmetics_v7.head == MODE_INFECTED_HAT, "Infected outfit (v7 headIndex)");

    player_spawn(zombie);
    CHECK(zombie->class_index == 9 && zombie->weapon && zombie->weapon->melee, "Infected respawn as class 9 with the knife");
    CHECK(game->mode->config.speed_mlt[zombie->team] == 1.15f, "Infected run 15%% faster");

    Player *survivor = zombie == a ? b : a;
    place(zombie, survivor->position.x + 2.0f, survivor->position.y, survivor->position.z);
    tick(game, 0.1f, 0.1f);
    CHECK(survivor->team == 2 && zombie->mode.convs == 1, "touching infects");

    // late joiners are Infected (convTeam)
    Player *d = add_player(game, 4, 0);
    CHECK(d->team == 2, "late joiner infected (%d)", d->team);

    Player *last = first_on_team(game, 1);
    kill(last, NULL, 0);
    tick(game, 0.1f, 0.1f);
    CHECK(ended(game) && end_result_for(zombie->uid) == 1 && end_result_for(last->uid) == 0, "no active survivors: Infected win");
}

static void test_lms(Game *game) {
    printf("Running Last Man Standing test...\n");
    start_mode(game, MODE_ID_LMS);
    CHECK(fabsf(game->round_timer - 300.0f) < 0.01f, "gameTime 5 min");
    Player *a = add_player(game, 1, 0);
    Player *b = add_player(game, 2, 0);
    Player *c = add_player(game, 3, 0);
    CHECK(a->mode.lives == 1 && game->mode->wait_timer_count == 1, "joined during the wait: 1 life");

    kill(b, a, 1);
    CHECK(b->mode.lives == 1 && mode_can_spawn(game, b), "deaths before the start are free");
    player_spawn(b);

    tick(game, 12.5f, 0.5f);
    CHECK(game->mode->wait_timer_count == 0, "match started");
    Player *late = add_player(game, 4, 0);
    CHECK(late->mode.lives == 0, "late joiners get no lives");

    Player *t1 = first_on_team(game, 1);
    Player *t2 = first_on_team(game, 2);
    for (size_t i = 0; i < game->player_count; i++) {
        Player *player = game->players[i];
        if (player->team == 1 && player->mode.lives) kill(player, t2, 1);
    }
    CHECK(t1->mode.lives == 0 && !mode_can_spawn(game, t1), "out of lives");
    tick(game, 0.1f, 0.1f);
    CHECK(ended(game) && end_result_for(t2->uid) == 1 && end_result_for(t1->uid) == 0, "the side with lives wins");
    (void) c;
}

static void test_gun(Game *game) {
    printf("Running Gun Game test...\n");
    start_mode(game, MODE_ID_GUN);
    const GameModeConfig *config = &game->mode->config;
    Player *a = add_player(game, 1, 0);
    Player *b = add_player(game, 2, 0);
    CHECK(a->class_index == 9 && a->loadout[0] == 2 && a->loadout_size == 2, "starts on the Pistol + knife (%d)", a->loadout[0]);

    tick(game, 1.0f, 0.5f);
    kill(b, a, 2);
    CHECK(a->mode.weapon_tier == 1, "kill: next tier (%d)", a->mode.weapon_tier);
    if (weapon_exists(game, config->weapon_order[1])) CHECK(a->loadout[0] == config->weapon_order[1], "tier 1 weapon %d", a->loadout[0]);
    CHECK(last_sent(NET_PACKET_LOADOUT, NET_TARGET_ALL) != NULL, "inat broadcast");
    CHECK(a->score > 0 && a->assists == 0, "kill rewards still score (%d)", a->score);

    // a streak kill (no weapon) does not promote (gun.js info.stk)
    player_spawn(b);
    kill(b, a, -1);
    CHECK(a->mode.weapon_tier == 1, "streak kill: no promotion (%d)", a->mode.weapon_tier);

    player_spawn(b);
    CHECK(b->loadout[0] == 2 && b->mode.weapon_tier == 0, "b respawns on tier 0");
    player_spawn(a);
    CHECK(a->mode.weapon_tier == 1, "keepLoadout: a keeps its tier");

    // a melee kill sets the victim back
    a->mode.weapon_tier = 3;
    mode_set_player_loadout(a, &(int) {config->weapon_order[3]}, 1, 0);
    kill(a, b, WEAPON_ID_KNIFE);
    CHECK(a->mode.weapon_tier == 2 && a->mode.setbacks == 1, "setback (tier %d)", a->mode.weapon_tier);
    if (weapon_exists(game, config->weapon_order[2])) CHECK(a->loadout[0] == config->weapon_order[2], "tier 2 weapon (%d)", a->loadout[0]);
    CHECK(b->mode.melees == 1, "melee kill counted");

    // reaching the end of the order ends the round
    player_spawn(a);
    player_spawn(b);
    a->mode.weapon_tier = config->weapon_order_size - 1;
    mode_set_player_loadout(a, &(int) {12}, 1, 0);
    kill(b, a, 12);
    tick(game, 0.1f, 0.1f);
    CHECK(ended(game) && end_result_for(a->uid) == 1 && end_result_for(b->uid) == 0, "last weapon kill wins (tier %d)", a->mode.weapon_tier);
}

static void test_boss(Game *game) {
    printf("Running Boss Hunt test...\n");
    start_mode(game, MODE_ID_BOSS);
    Player *a = add_player(game, 1, 0);
    Player *b = add_player(game, 2, 0);
    add_player(game, 3, 0);
    CHECK(a->team == 1 && b->team == 1, "Heros on team 1");
    tick(game, 15.5f, 0.5f);
    Player *boss = first_on_team(game, 2);
    CHECK(boss && boss->max_health == 10000 && boss->health == 10000.0f && boss->mode.no_regen, "a 10000 hp boss");
    Player *hero = first_on_team(game, 1);
    kill(boss, hero, 1);
    tick(game, 0.1f, 0.1f);
    CHECK(ended(game) && end_result_for(hero->uid) == 1 && end_result_for(boss->uid) == 0, "boss down: Heros win");
}

static void test_stalker(Game *game) {
    printf("Running Stalker test...\n");
    start_mode(game, MODE_ID_STALK);
    add_player(game, 1, 0);
    add_player(game, 2, 0);
    add_player(game, 3, 0);
    tick(game, 15.5f, 0.5f);
    Player *stalker = first_on_team(game, 2);
    // v10.0.2 pickStalker (v10:188459): floor(200 + 50 x (active players - 1)) health (v7.1.4: 400)
    CHECK(stalker && stalker->max_health == 300 && stalker->loadout[0] == WEAPON_ID_KNIFE && stalker->loadout_size == 1, "a 300 hp (3 players) knife-only Stalker");
    CHECK(stalker && stalker->cosmetics.skin_color == 0 && stalker->cosmetics.hat == -1, "Stalker outfit");
    CHECK(game->mode->config.speed_mlt[1] == 0.65f, "the Living walk slowly");

    Player *living = first_on_team(game, 1);
    // v10.0.2 onMelee: a factor, 0.6 for the stalker's stab (which heals it 10: changeHealth -10),
    // 0.8 for anyone else (v7.1.4: the stab took the victim's whole health)
    stalker->health = stalker->max_health - 50.0f;
    const float stalker_health = stalker->health;
    const float damage = mode_melee_damage(game, stalker, living, 50.0f);
    CHECK(fabsf(damage - 30.0f) < 1e-3f && fabsf(stalker->health - (stalker_health + 10.0f)) < 1e-3f,
          "onMelee: the stab deals 0.6x (%.1f) and heals the stalker 10 (%.0f -> %.0f)", damage, stalker_health, stalker->health);
    CHECK(fabsf(mode_melee_damage(game, living, stalker, 50.0f) - 40.0f) < 1e-3f, "onMelee: the Living's knife deals 0.8x");

    kill(stalker, living, 1);
    tick(game, 0.1f, 0.1f);
    CHECK(ended(game) && end_result_for(living->uid) == 1, "the Stalker died (1 life): the Living win");
}

static void test_simon_traitor(Game *game) {
    printf("Running Simon Says / Traitor test...\n");
    start_mode(game, MODE_ID_SIMON);
    add_player(game, 1, 0);
    add_player(game, 2, 0);
    tick(game, 11.0f, 0.5f);
    CHECK(!first_on_team(game, 2), "Simon needs 3 players");
    add_player(game, 3, 0);
    tick(game, 10.5f, 0.5f);
    Player *simon = first_on_team(game, 2);
    CHECK(simon && simon->max_health == 5000, "Simon picked with 5000 hp");

    start_mode(game, MODE_ID_TRAI);
    for (int i = 1; i <= 6; i++) add_player(game, i, 0);
    CHECK(team_count(game, 1) == 6, "everyone starts Innocent");
    tick(game, 15.5f, 0.5f);
    CHECK(team_count(game, 2) == 2, "2 traitors from 6 players (%d)", team_count(game, 2));
    Player *traitor = first_on_team(game, 2);
    Player *innocent = first_on_team(game, 1);
    Player *other_innocent = NULL;
    for (size_t i = 0; i < game->player_count; i++) {
        if (game->players[i]->team == 1 && game->players[i] != innocent) other_innocent = game->players[i];
    }
    const float before = other_innocent->health;
    player_change_health(other_innocent, innocent, 10.0f, 0);
    CHECK(other_innocent->health == before - 10.0f, "dmgTeam: teammates hurt each other");
    other_innocent->health = before;
    const int score = traitor->score;
    kill(innocent, traitor, 1);
    CHECK(traitor->score == score, "Traitor noScoreK");
    for (size_t i = 0; i < game->player_count; i++) {
        Player *player = game->players[i];
        if (player->team == 2 && player->active) kill(player, innocent, 1);
    }
    tick(game, 0.1f, 0.1f);
    CHECK(ended(game) && end_result_for(innocent->uid) == 1 && end_result_for(traitor->uid) == 0, "traitors out: Innocent win");
}

static void test_oinc(Game *game) {
    printf("Running One in the Chamber test...\n");
    start_mode(game, MODE_ID_OINC);
    Player *a = add_player(game, 1, 0);
    Player *b = add_player(game, 2, 0);
    CHECK(a->class_index == 10 && a->max_health == 10 && a->ammo[0] == 1, "Deagler, 10 hp, 1 bullet");
    CHECK(a->mode.lives == 3 && game->move_lock, "3 lives, movement locked until the start");
    tick(game, 10.5f, 0.5f);
    CHECK(!game->move_lock, "unlockMove after 10 s");

    kill(b, a, 10);
    CHECK(a->ammo[0] == 2 && b->mode.lives == 2, "kill refills a bullet, the victim loses a life");
    tick(game, 3.0f + GAME_DEATH_DELAY + 0.5f, 0.5f);
    CHECK(g_respawns[2] == 1 && b->active, "forceSpawn 3 s");

    kill(b, a, 10);
    tick(game, 6.5f, 0.5f);
    kill(b, a, 10);
    tick(game, 0.1f, 0.1f);
    CHECK(b->mode.lives == 0 && ended(game) && end_result_for(a->uid) == 1, "the last player with lives wins");
}

static void test_shrp(Game *game) {
    printf("Running Sharp Shooter test...\n");
    start_mode(game, MODE_ID_SHRP);
    Player *a = add_player(game, 1, 0);
    const int weapon = game->mode->active_objective;
    CHECK(a->class_index == 9 && a->loadout[0] == weapon && !game->weapons[weapon]->melee, "everyone holds the objective weapon (%d)", weapon);
    int changed = 0;
    for (int i = 0; i < 8 && !changed; i++) {
        tick(game, 20.0f, 0.5f);
        changed = game->mode->active_objective != weapon;
    }
    CHECK(changed && a->loadout[0] == game->mode->active_objective, "a new weapon every 20 s");
}

static void test_misc(Game *game) {
    printf("Running Parkour / Trade / Race / Prop / Def / Raid / Ranked test...\n");
    start_mode(game, MODE_ID_BHOP);
    Player *a = add_player(game, 1, 0);
    Player *b = add_player(game, 2, 0);
    CHECK(a->team == 1 && b->team == 1 && !player_change_health(b, a, 50.0f, 0), "Parkour: friendly");

    start_mode(game, MODE_ID_TRADE);
    a = add_player(game, 1, 0);
    tick(game, 300.0f, 5.0f);
    CHECK(!ended(game), "Trade has no clock (gameTime 0)");

    start_mode(game, MODE_ID_RACE);
    CHECK(game->move_lock && game->mode->config.timed, "Race: locked start, the clock counts up");
    a = add_player(game, 1, 0);
    tick(game, 15.5f, 0.5f);
    CHECK(!game->move_lock, "race starting");
    tick(game, 2.0f, 0.5f);
    CHECK(a->mode.finished && game->mode->wait_timer_count == 1, "no score zones: target 0, finished at once");
    tick(game, 10.5f, 0.5f);
    CHECK(ended(game), "race ends 10 s after the first finisher");

    start_mode(game, MODE_ID_PROP);
    a = add_player(game, 1, 0);
    b = add_player(game, 2, 0);
    CHECK(a->team != b->team, "Prop Hunt: teamSort balances");
    tick(game, 10.5f, 0.5f);
    Player *seeker = first_on_team(game, 2);
    CHECK(seeker && seeker->mode.locked, "seekers locked after 10 s");
    tick(game, 30.5f, 0.5f);
    CHECK(seeker && !seeker->mode.locked, "released 30 s later");
    Player *prop = first_on_team(game, 1);
    kill(prop, seeker, 1);
    tick(game, 0.1f, 0.1f);
    CHECK(ended(game) && end_result_for(seeker->uid) == 1, "props found: seekers win");

    // Prop Hunt's mechanics (prop_hunt.h): a Prop-team spawn wears a random prop with no switches made; a pick applies
    // a valid prop other than the one worn and costs a switch (3 per life); the rotation toggles (off keeps the look)
    // and a reset clears the saved yaw; a Seeker's shot on a prop object costs it 10 health
    start_mode(game, MODE_ID_PROP);
    add_player(game, 1, 0);
    add_player(game, 2, 0);
    Player *pp = first_on_team(game, 1); // the Prop team (team_options[0])
    Player *sk = first_on_team(game, 2); // the Seekers
    CHECK(pp && sk && game_player_is_prop(game, pp) && !game_player_is_prop(game, sk), "Prop: team 1 is the Prop team, team 2 the Seekers");
    CHECK(prop_is_valid(pp->prop_id) && pp->prop_switches == 0, "Prop: a Prop-team spawn wears a valid prop with no switches");
    CHECK(sk->prop_id == PROP_DEFAULT_ID, "Prop: a Seeker wears the default prop");
    const int want = pp->prop_id == 16 ? 20 : 16;
    CHECK(game_prop_pick(game, pp, want) == 1 && pp->prop_id == want && pp->prop_switches == 1, "Prop: a pick applies and costs a switch");
    CHECK(!game_prop_pick(game, pp, want) && pp->prop_switches == 1, "Prop: the prop worn is no pick");
    CHECK(!game_prop_pick(game, sk, 16) && sk->prop_id == PROP_DEFAULT_ID, "Prop: a Seeker picks nothing");
    const int ids[] = {2, 8, 28, 36}; // none is 16 or 20: the first two picks succeed, the third hits the 3-switch limit
    int taken = 0;
    for (int i = 0; i < 4; i++) taken += game_prop_pick(game, pp, ids[i]);
    CHECK(taken == 2 && pp->prop_switches == 3 && pp->prop_id == 8, "Prop: picks stop at the switch limit (%d taken, %d switches)", taken, pp->prop_switches);
    pp->direction.y = 0.5f;
    CHECK(game_prop_rotate(game, pp, 0) == 1 && pp->prop_rot == 1, "Prop: the rotation turns on");
    pp->direction.y = 1.25f;
    game_prop_rotate(game, pp, 0);
    CHECK(!pp->prop_rot && pp->prop_rot_saved_set && pp->prop_rot_saved == 1.25f, "Prop: the rotation off keeps the look");
    game_prop_rotate(game, pp, 1);
    CHECK(!pp->prop_rot && !pp->prop_rot_saved_set && pp->prop_rot_saved == 0.0f, "Prop: the reset clears the saved yaw");
    Object shot;
    memset(&shot, 0, sizeof(shot));
    shot.prefab = 16; // a CONE: a prop object of the map
    const float seeker_health = sk->health;
    game_prop_seeker_hit(game, sk, &shot);
    CHECK(sk->health == seeker_health - 10.0f, "Prop: a Seeker's shot on a prop costs it 10 health");
    const float prop_health = pp->health;
    game_prop_seeker_hit(game, pp, &shot);
    CHECK(pp->health == prop_health, "Prop: a prop's hit on a prop object costs nothing");

    start_mode(game, MODE_ID_DEF);
    a = add_player(game, 1, 0);
    b = add_player(game, 2, 0);
    CHECK(game->mode->config.real_movement && game->mode->config.hitbox_pad == 0.5f && game->move_lock, "Defuse: realMov, hitBoxPad, locked");
    tick(game, 10.5f, 0.5f);
    CHECK(!game->move_lock, "match starting -> unlockMove");
    Player *defender = first_on_team(game, 1);
    Player *attacker = first_on_team(game, 2);
    kill(attacker, defender, 1);
    tick(game, 0.1f, 0.1f);
    CHECK(ended(game) && end_result_for(defender->uid) == 1, "attackers out: defenders win");

    start_mode(game, MODE_ID_RAID);
    a = add_player(game, 1, 0);
    CHECK(fabsf(game->round_timer - 1500.0f) < 0.01f && a->class_index == 9 && a->mode.lives == 1, "Raid config (v10 25 min)");
}


// ------------------------------------------------------------------ v7.1.4 modes

// MapManager.addObjective uids (obi - 1, else the v7 auto uid), randObjective, neutral flags
static void test_objectives_v7(Game *game) {
    printf("Running v7 objective / flag table test...\n");
    start_mode_on(game, MODE_ID_DEPO, 2);
    GameMode *mode = game->mode;
    CHECK(mode->objective_count == 3 && mode->objectives[0].uid == 1 && mode->objectives[1].uid == 2 && mode->objectives[2].uid == 0,
          "uids obi 2 -> 1, auto -> 2, obi 1 -> 0 (%d %d %d)", mode->objectives[0].uid, mode->objectives[1].uid, mode->objectives[2].uid);
    CHECK(mode_objective_by_uid(game, mode->active_objective) != NULL, "randObjective picks an existing uid (%d)", mode->active_objective);
    CHECK(mode->flag_count == 3 && mode->flags[0].neutral && mode->flags[0].team == -1 && mode->flags[1].team == 1 && mode->flags[2].team == 2, "flags: tm -1 neutral, 0 -> 1, 1 -> 2");

    // the next objective of point is uid + 1, else uid 0
    start_mode_on(game, MODE_ID_POINT, 2);
    mode = game->mode;
    CHECK(mode->active_objective == 0, "point starts on getNextObjective(-1 + 1) = uid 0 (%d)", mode->active_objective);
    tick(game, 60.5f, 0.5f);
    CHECK(mode->active_objective == 1, "then uid 1 (%d)", mode->active_objective);
    tick(game, 60.0f, 0.5f);
    CHECK(mode->active_objective == 2, "then uid 2 (%d)", mode->active_objective);
    tick(game, 60.0f, 0.5f);
    CHECK(mode->active_objective == 0, "no uid 3: back to 0 (%d)", mode->active_objective);

    // CTF ignores the neutral flag, tdf uses only it
    start_mode_on(game, MODE_ID_CTF, 2);
    CHECK(!game->mode->flags[0].active && game->mode->flags[1].active && game->mode->flags[2].active, "CTF: neutral flag out of play");
    start_mode_on(game, MODE_ID_TDF, 2);
    CHECK(game->mode->flags[0].active && !game->mode->flags[1].active && !game->mode->flags[2].active, "tdf: only the neutral flag");
}

// CTF v7: a dropped flag goes home after flagIdle 45 s; Blitz (rush) captures while your own flag
// is carried from home and respawns the capper; Team Defender (oneflag) carriers score.
static void test_flags_v7(Game *game) {
    printf("Running v7 flag test (idle, Blitz, Team Defender)...\n");
    start_mode_on(game, MODE_ID_CTF, 2);
    GameMode *mode = game->mode;
    Player *red = add_player(game, 1, 0);  // team 2
    Player *blue = add_player(game, 2, 0); // team 1
    ModeFlag *blue_flag = &mode->flags[1];
    place(blue, 900.0f, 0.0f, 900.0f);
    place(red, blue_flag->origin.x, blue_flag->origin.y - 6.0f, blue_flag->origin.z);
    tick(game, 0.1f, 0.1f);
    CHECK(blue_flag->carrier_uid == red->uid, "red carries the blue flag");
    place(red, 0.0f, 0.0f, 300.0f);
    kill(red, blue, 1);
    CHECK(blue_flag->carrier_uid < 0 && blue_flag->idle_timer > 44.0f, "dropped with a 45 s idle timer");
    tick(game, 44.0f, 0.5f);
    CHECK(blue_flag->position.x != blue_flag->origin.x, "still out after 44 s");
    tick(game, 1.5f, 0.5f);
    CHECK(blue_flag->position.x == blue_flag->origin.x && blue_flag->position.z == blue_flag->origin.z && !strcmp(mode->flag_info[1], "AT BASE"), "home after 45 s");

    start_mode_on(game, MODE_ID_BLITZ, 2);
    mode = game->mode;
    red = add_player(game, 1, 0);
    blue = add_player(game, 2, 0);
    ModeFlag *red_flag = &mode->flags[2];
    blue_flag = &mode->flags[1];
    place(blue, red_flag->origin.x, red_flag->origin.y - 6.0f, red_flag->origin.z); // blue takes the red flag
    place(red, blue_flag->origin.x, blue_flag->origin.y - 6.0f, blue_flag->origin.z); // red takes the blue flag
    tick(game, 0.1f, 0.1f);
    CHECK(red_flag->carrier_uid == blue->uid && blue_flag->carrier_uid == red->uid, "both flags taken");
    place(red, red_flag->origin.x, red_flag->origin.y - 12.0f, red_flag->origin.z); // home, own flag carried away
    g_sent_count = 0;
    tick(game, 0.1f, 0.1f);
    CHECK(red->mode.caps == 1 && mode_team_score(game, 2) == 1, "Blitz rush: capture while your flag is away (%d)", red->mode.caps);
    CHECK(count_sent(NET_PACKET_ACTION) == 0, "Blitz: no action feed");
    CHECK(fabsf(red->position.x - red_flag->origin.x) > 20.0f || fabsf(red->position.z - red_flag->origin.z) > 20.0f, "the capper is sent to a spawn");

    start_mode_on(game, MODE_ID_TDF, 2);
    mode = game->mode;
    Player *a = add_player(game, 1, 0);
    Player *b = add_player(game, 2, 0);
    Player *c = add_player(game, 3, 0);
    ModeFlag *flag = &mode->flags[0];
    place(a, flag->origin.x, flag->origin.y - 6.0f, flag->origin.z);
    place(b, 900.0f, 0.0f, 900.0f);
    place(c, -900.0f, 0.0f, -900.0f);
    tick(game, 0.1f, 0.1f);
    CHECK(flag->carrier_uid == a->uid && a->score == 21 && mode->flag_team == a->team, "anyone picks up the neutral flag (+20, then +1 at once: objectiveTimer starts at 0) (carrier %d score %d team %d/%d)", flag->carrier_uid, a->score, mode->flag_team, a->team);
    const int score = a->score;
    tick(game, 1.5f, 0.25f);
    CHECK(a->score == score + 1, "carrier +1 per 1.5 s in a small public lobby (%d)", a->score - score);
    // kill score multipliers: carrier 1.25, its team 1, others 0.5
    CHECK(mode_score_scale(game, a, 100, 1) == 125 && mode_score_scale(game, b->team == a->team ? b : c, 100, 1) == (b->team == a->team ? 100 : 100), "carrier x1.25");
    Player *enemy = b->team != a->team ? b : c;
    CHECK(mode_score_scale(game, enemy, 100, 1) == 50 && mode_score_scale(game, enemy, 100, 0) == 100, "non-carrier team x0.5 (kills only)");
    place(a, 0.0f, 0.0f, 300.0f);
    kill(a, enemy, 1);
    CHECK(flag->carrier_uid < 0 && mode->flag_team == 0, "the carrier's death drops it");
}

// game.js updateDomZones / dom.js (map-logic.md §4.1a)
static void test_dom(Game *game) {
    printf("Running Domination test...\n");
    start_mode_on(game, MODE_ID_DOM, 1);
    GameMode *mode = game->mode;
    CHECK(mode->dom_zone_count == 3 && mode->dom_zones[1].position.x == 0.0f && fabsf(mode->dom_zones[1].half.y - 10.0f) < 0.01f, "3 zones");

    Player *a = add_player(game, 1, 0); // team 2
    Player *b = add_player(game, 2, 0); // team 1
    Player *c = add_player(game, 3, 0); // team 2
    CHECK(a->team == 2 && b->team == 1 && c->team == 2, "teams");
    place(a, -100.0f, 0.0f, 0.0f);
    place(b, 400.0f, 0.0f, 400.0f);
    place(c, 400.0f, 0.0f, -400.0f);
    ModeDomZone *zone = &mode->dom_zones[0];

    // one player: 10 s to capture (mlt 1)
    tick(game, 9.5f, 0.25f);
    CHECK(!zone->captured_by && zone->capture_t > 9.0f && zone->last_team_on == 2 && zone->mlt == 1.0f, "capturing (%.2f)", zone->capture_t);
    CHECK(last_sent(NET_PACKET_DOM_UPDATE, NET_TARGET_ALL) != NULL, "uct");
    tick(game, 0.75f, 0.25f);
    CHECK(zone->captured_by == 2 && a->mode.caps == 1 && a->score >= 200, "captured: Secured +200 (%d)", a->score);
    CHECK(last_sent(NET_PACKET_DOM_CAPTURE, NET_TARGET_ALL) != NULL, "cct");

    // owned zones score +10 every 1.5 s
    const int team_score = mode_team_score(game, 2);
    tick(game, 1.5f, 0.25f);
    CHECK(mode_team_score(game, 2) == team_score + 10, "owner +10 per 1.5 s (%d)", mode_team_score(game, 2) - team_score);

    // two players: mlt 1 + 0.2; contested: frozen; empty: reset
    ModeDomZone *mid = &mode->dom_zones[1];
    place(a, 0.0f, 0.0f, 0.0f);
    place(c, 2.0f, 0.0f, 0.0f);
    tick(game, 1.0f, 0.25f);
    CHECK(fabsf(mid->mlt - 1.2f) < 1e-4f && fabsf(mid->capture_t - 1.2f) < 0.01f, "perPlayerMlt (mlt %.2f t %.2f)", mid->mlt, mid->capture_t);
    place(b, -2.0f, 0.0f, 0.0f);
    const float frozen = mid->capture_t;
    tick(game, 1.0f, 0.25f);
    CHECK(mid->capture_t == frozen && mid->teams_on_count == 2, "contested: frozen");
    place(a, 400.0f, 0.0f, 400.0f);
    place(c, 400.0f, 0.0f, -400.0f);
    place(b, 900.0f, 0.0f, 900.0f);
    tick(game, 0.5f, 0.25f);
    CHECK(mid->capture_t == 0.0f && !mid->last_team_on, "empty: reset");

    // lockout: team 2 holds every zone -> team 1 captures 1.5x faster
    mode->dom_zones[1].captured_by = 2;
    mode->dom_zones[2].captured_by = 2;
    CHECK(mode_has_all_zones(game) == 2, "hasAllZones");
    place(b, 0.0f, 0.0f, 0.0f);
    tick(game, 1.0f, 0.25f);
    CHECK(fabsf(mid->mlt - 1.5f) < 1e-4f, "lockoutMlt (%.2f)", mid->mlt);

    // Defender: a kill from your own zone
    place(a, -100.0f, 0.0f, 0.0f);
    tick(game, 0.25f, 0.25f);
    const int before = a->score;
    kill(b, a, 1);
    CHECK(a->score - before >= 25 && a->mode.on_zone == 0, "Defender +25 (%d)", a->score - before);
}

// depo.js / depoffa.js + medals.js KR Tag
static void test_deposit(Game *game) {
    printf("Running Deposit test...\n");
    start_mode(game, MODE_ID_DEPO);
    GameMode *mode = game->mode;
    Player *a = add_player(game, 1, 0); // team 2
    Player *b = add_player(game, 2, 0); // team 1
    place(a, 0.0f, 600.0f, 0.0f);
    place(b, 0.0f, 600.0f, 40.0f);

    // a kill drops 1 tag (+ the victim's); clusters of 3 with jitter above that
    b->mode.dog_tags = 6;
    kill(b, a, 1);
    int active = 0, total = 0;
    for (int i = 0; i < MODE_MAX_SPAWNABLES; i++) {
        if (!mode->spawnables[i].active) continue;
        active++;
        total += mode->spawnables[i].data_c;
        CHECK(mode->spawnables[i].type == SPAWNABLE_KR_TAG && mode->spawnables[i].team == b->team, "KR Tag of the victim's team");
    }
    CHECK(active == 3 && total == 7 && b->mode.dog_tags == 0, "7 tags in 3 clusters (%d / %d)", active, total);

    // picking them up: +10 each stack, krt; the hold limit leaves the rest
    a->mode.dog_tags = 18;
    for (int i = 0; i < MODE_MAX_SPAWNABLES; i++) {
        if (!mode->spawnables[i].active || mode->spawnables[i].data_c != 3) continue;
        place(a, mode->spawnables[i].position.x, mode->spawnables[i].position.y - 3.0f, mode->spawnables[i].position.z);
        tick(game, 0.1f, 0.1f);
        CHECK(a->mode.dog_tags == 20 && mode->spawnables[i].active && mode->spawnables[i].data_c == 1, "hold limit 20: 1 tag stays (%d, c %d)",
              a->mode.dog_tags, mode->spawnables[i].data_c);
        break;
    }
    CHECK(last_sent(NET_PACKET_MODE_COUNTER, a->uid) != NULL, "krt");

    // deposit one per 500 ms on the objective: +50, team +1
    const ModeObjective *objective = mode_objective_by_uid(game, mode->active_objective);
    place(a, objective->position.x, objective->position.y - objective->half.y, objective->position.z);
    const int score = a->score;
    tick(game, 1.0f, 0.25f);
    CHECK(a->mode.deposits == 2 && a->mode.dog_tags == 18 && mode_team_score(game, a->team) == 2 && a->score == score + 100,
          "deposits %d tags %d team %d score +%d", a->mode.deposits, a->mode.dog_tags, mode_team_score(game, a->team), a->score - score);

    // denies: your own team's tags
    player_spawn(b);
    place(b, 300.0f, 600.0f, 300.0f);
    Player *c = add_player(game, 3, 0); // team 2
    place(c, 300.0f, 600.0f, 340.0f);
    kill(c, b, 1);
    place(a, 300.0f, 600.0f, 340.0f);
    tick(game, 0.1f, 0.1f);
    CHECK(a->mode.denies == 1, "teammate's tag: deny (%d)", a->mode.denies);

    // tagLimit on the team score
    mode->team_scores[2] = 65;
    tick(game, 0.1f, 0.1f);
    CHECK(ended(game) && end_result_for(a->uid) == 1, "65 deposits win");
    const NetModeEndPacket end = last_end();
    CHECK(end.count && end.entries[0].stats[NET_MODE_STAT_DEPOSITS] >= 0, "deposits end stat");

    // Deposit FFA: 750 ms, +75, no team score; the first to 65 deposits wins
    start_mode(game, MODE_ID_DEPOFFA);
    mode = game->mode;
    a = add_player(game, 1, 0);
    CHECK(a->team == 0, "FFA");
    a->mode.dog_tags = 5;
    objective = mode_objective_by_uid(game, mode->active_objective);
    place(a, objective->position.x, objective->position.y - objective->half.y, objective->position.z);
    tick(game, 0.75f, 0.25f);
    CHECK(a->mode.deposits == 1 && a->score == 75, "depoffa: +75 (%d / %d)", a->mode.deposits, a->score);
    a->mode.deposits = 65;
    tick(game, 0.1f, 0.1f);
    CHECK(ended(game), "65 deposits end Deposit FFA");
}

// sdm: 4 teams, smallest team first
static void test_sdm(Game *game) {
    printf("Running Squad Deathmatch test...\n");
    start_mode_on(game, MODE_ID_SDM, 1);
    int counts[MODE_TEAMS] = {0};
    for (int i = 1; i <= 8; i++) counts[add_player(game, i, 0)->team]++;
    CHECK(counts[1] == 2 && counts[2] == 2 && counts[3] == 2 && counts[4] == 2 && !counts[5], "8 players: 2 per team (%d %d %d %d)", counts[1], counts[2], counts[3], counts[4]);
    CHECK(mode_team_count(game) == 4, "4 teams");
    Player *a = game->players[0];
    Player *b = NULL;
    for (size_t i = 0; i < game->player_count; i++) {
        if (game->players[i]->team == 3) b = game->players[i];
    }
    kill(b, a, 1);
    CHECK(mode_team_score(game, a->team) == a->score && a->score > 0, "team score");
    game->mode->team_scores[3] = a->score + 50;
    game->round_timer = 0.1f;
    tick(game, 0.5f, 0.25f);
    CHECK(ended(game) && end_result_for(b->uid) == 1 && end_result_for(a->uid) == 0, "team 3 wins");
}

// krank / bhffa / aon / md / chs / imp / clas
static void test_modifiers(Game *game) {
    printf("Running Kranked / Bighead / All or Nothing / Mag Dump / Chaos / Impulsed / Classic test...\n");
    start_mode(game, MODE_ID_KRANK);
    Player *a = add_player(game, 1, 0);
    Player *b = add_player(game, 2, 0);
    place(a, 0.0f, 600.0f, 0.0f);
    place(b, 200.0f, 600.0f, 0.0f);
    kill(b, a, 1);
    CHECK(a->mode.is_kranked && fabsf(a->mode.kranked_timer - 15.0f) < 0.01f, "a kill krankes the killer");
    CHECK(fabsf(mode_speed_mlt(game, a) - 1.25f) < 1e-4f && fabsf(mode_reload_mlt(game, a) - 0.35f) < 1e-4f && fabsf(mode_fire_rate_mlt(game, a) - 0.85f) < 1e-4f, "bonuses");
    player_spawn(b);
    const int score = a->score;
    kill(b, a, 1);
    CHECK(a->score - score >= 35, "Recharge +35 (%d)", a->score - score);
    tick(game, 15.5f, 0.5f);
    CHECK(!a->active, "the timer runs out: boom");

    start_mode(game, MODE_ID_BHFFA);
    a = add_player(game, 1, 0);
    b = add_player(game, 2, 0);
    for (int i = 0; i < 3; i++) {
        kill(b, a, 1);
        player_spawn(b);
    }
    CHECK(fabsf(a->mode.head_mlt - 1.3f) < 1e-4f && fabsf(mode_head_mlt(a) - 1.3f) < 1e-4f, "headMlt 1 + 0.1 x kills (%.2f)", a->mode.head_mlt);
    a->kills = 40;
    kill(b, a, 1);
    CHECK(a->mode.head_mlt == 3.0f, "capped at 3");

    start_mode(game, MODE_ID_AON);
    a = add_player(game, 1, 0);
    b = add_player(game, 2, 0);
    CHECK(a->class_index == 9 && a->loadout[0] == 16 && a->ammo[0] == 0 && !a->can_throw, "aon: weapon 16, no ammo, no knife throw (%d %u)", a->loadout[0], a->ammo[0]);
    place(b, 0.0f, 600.0f, 0.0f);
    kill(b, a, 16);
    int packs = 0;
    for (int i = 0; i < MODE_MAX_SPAWNABLES; i++) packs += game->mode->spawnables[i].active && game->mode->spawnables[i].type == SPAWNABLE_AMMO_PACK;
    CHECK(packs == 1, "the victim drops an Ammo Pack");

    start_mode(game, MODE_ID_MD);
    a = add_player(game, 1, 0);
    b = add_player(game, 2, 0);
    place(b, 0.0f, 600.0f, 0.0f);
    kill(b, a, 2);
    const ModeSpawnable *box = NULL;
    for (int i = 0; i < MODE_MAX_SPAWNABLES; i++) {
        if (game->mode->spawnables[i].active && game->mode->spawnables[i].type == SPAWNABLE_WEAPON_BOX) box = &game->mode->spawnables[i];
    }
    CHECK(box && box->data_w >= 0, "md: a Weapon Box with a weapon");
    if (box && weapon_exists(game, box->data_w)) {
        const int weapon = box->data_w;
        place(a, box->position.x, box->position.y - 1.0f, box->position.z);
        tick(game, 0.1f, 0.1f);
        CHECK(a->loadout[0] == weapon && a->mode.pickups == 1, "picked up: weapon %d", a->loadout[0]);
    }

    start_mode(game, MODE_ID_CHS);
    a = add_player(game, 1, 0);
    CHECK(a->class_index == 0 && a->loadout[0] == 0 && a->max_health == 10 && a->ammo[0] == 1, "chs: sniper, 10 hp, 1 bullet");
    CHECK(mode_grav_mlt(game) == 0.92f && mode_jump_mlt(game) == 1.5f && mode_ground_accel(game) == 2.0f && mode_reload_mlt(game, a) == 0.5f, "chs physics");
    float wall_jump = 0.0f, d = 0.0f, v = 0.0f, yv = 0.0f, max = 0.0f;
    CHECK(mode_wall_jump(game, &wall_jump, &max, &d, &v, &yv) && max == 100.0f && yv == 2.0f, "chs wallJ");

    start_mode(game, MODE_ID_IMP);
    a = add_player(game, 1, 0);
    CHECK(a->loadout_size >= 2 && a->loadout[0] == 5 && (!weapon_exists(game, 22) || a->loadout[1] == 22), "imp: weapons 5 + 22");
    CHECK(mode_explosion_damage_mlt(game, a, NULL) == 5.0f && mode_explosion_damage_mlt(game, a, a) == 1.0f && mode_explosion_size_mlt(game) == 2.0f, "explMlt others only");

    start_mode(game, MODE_ID_CLAS);
    a = add_player(game, 1, 0);
    int knife = 0;
    for (int i = 0; i < a->loadout_size; i++) knife |= a->loadout[i] == WEAPON_ID_KNIFE;
    CHECK(!knife && !a->can_throw && mode_no_sliding(game) && mode_no_wall_jump(game) && game->mode->config.real_movement, "clas: no melee, no slide / wall jump");
}

// endGame outside best of (v10.0.3 deob 221603-221605): the round's winner (getWinTeam(true) over the
// round's team scores) gets a round win in teamRoundWins too. The match's winner is getWinTeam() over
// those round wins, not over the scores that carry on from round to round (no reset outside best of).
static void test_round_wins_outside_best_of(Game *game) {
    printf("Running round wins outside best of test...\n");
    start_mode(game, MODE_ID_TDM);
    GameMode *mode = game->mode;
    game->config.intermission_time = 10.0f;
    mode->game_rounds = 2;
    mode->round_count = 1;
    mode->best_of = 0;
    mode->need_winner = 1;
    add_player(game, 1, 0); // team 2
    add_player(game, 2, 0); // team 1

    // round 1: team 2 wins it (10 to 0): a round win is counted, round 2 is the last round
    mode->team_scores[2] = 10;
    game->round_timer = 0.1f;
    tick(game, 0.5f, 0.25f);
    CHECK(!ended(game) && mode->round_count == 0 && mode->has_round_wins && mode->team_round_wins[2] == 1 && mode->team_round_wins[1] == 0,
          "round 1 outside best of: team 2 has a round win (wins %d / %d, left %d, has %d)", mode->team_round_wins[1], mode->team_round_wins[2],
          mode->round_count, mode->has_round_wins);
    const SentPacket *sent = last_sent(NET_PACKET_ROUNDS, NET_TARGET_ALL);
    CHECK(sent != NULL, "rds after round 1");
    if (sent) {
        NetRoundsPacket rounds;
        memcpy(&rounds, sent->payload, sizeof(rounds));
        CHECK(rounds.has_round_wins && rounds.win_team == 2 && rounds.team_round_wins[2] == 1, "rds carries the round win");
    }
    tick(game, 10.5f, 0.5f); // intermission over

    // round 2: team 1 wins it on the carried-over scores (30 against 10). One round win each is a tie
    // with noDraws: 60 s of overtime, not a team 1 match win by the scores.
    mode->team_scores[1] = 30;
    game->round_timer = 0.1f;
    tick(game, 0.5f, 0.25f);
    CHECK(mode->team_round_wins[1] == 1 && mode->team_round_wins[2] == 1, "round 2 outside best of: 1-1 in round wins (%d / %d)",
          mode->team_round_wins[1], mode->team_round_wins[2]);
    CHECK(!ended(game) && game->round_timer > 59.0f && last_sent(NET_PACKET_TEXT_POPUP, NET_TARGET_ALL) != NULL,
          "1-1 with noDraws: overtime, no match winner yet");
}

// endGame rounds: best of, intermission (flipped spawns, locked movement), overtime
static void test_rounds(Game *game) {
    printf("Running rounds / best of / overtime test...\n");
    start_mode(game, MODE_ID_TDM);
    GameMode *mode = game->mode;
    game->config.intermission_time = 10.0f; // WP-E: intermTmr (the serverConfig default is 30 s)
    mode->game_rounds = 3;
    mode->round_count = 2;
    mode->best_of = 1;
    Player *a = add_player(game, 1, 0); // team 2
    Player *b = add_player(game, 2, 0); // team 1

    mode->team_scores[2] = 10;
    game->round_timer = 0.1f;
    tick(game, 0.5f, 0.25f);
    CHECK(!ended(game) && mode->team_round_wins[2] == 1 && mode->round_count == 1 && mode->flipped_spawns, "round 1 to team 2 (wins %d left %d)",
          mode->team_round_wins[2], mode->round_count);
    CHECK(mode->team_scores[2] == 0 && game->move_lock && mode->wait_timer_count && mode->wait_timers[0].dont_wait, "scores reset, intermission");
    const SentPacket *sent = last_sent(NET_PACKET_ROUNDS, NET_TARGET_ALL);
    CHECK(sent != NULL, "rds");
    if (sent) {
        NetRoundsPacket rounds;
        memcpy(&rounds, sent->payload, sizeof(rounds));
        CHECK(rounds.rounds_left == 1 && rounds.win_team == 2 && rounds.team_round_wins[2] == 1, "rds payload");
    }
    tick(game, 10.5f, 0.5f);
    CHECK(!game->move_lock && !mode->wait_timer_count && a->active && b->active, "intermission over, everyone respawned");

    // a tie with noDraws: 60 s overtime
    mode->need_winner = 1;
    game->round_timer = 0.1f;
    tick(game, 0.5f, 0.25f);
    CHECK(!ended(game) && mode->in_overtime && game->round_timer > 59.0f && last_sent(NET_PACKET_TEXT_POPUP, NET_TARGET_ALL) != NULL, "overtime");

    // team 2 wins round 2: 2 round wins of 3 win the match
    mode->team_scores[2] = 5;
    game->round_timer = 0.1f;
    tick(game, 0.5f, 0.25f);
    CHECK(ended(game) && end_result_for(a->uid) == 1 && end_result_for(b->uid) == 0, "best of 3: team 2 wins the match");

    // flipped spawns: team 1 spawns on team 2's spawn points
    start_mode_on(game, MODE_ID_TDM, 1);
    game->mode->flipped_spawns = 1;
    Player *c = add_player(game, 1, 0); // team 2
    // team 1's spawns or the neutral one (-150, -150): ties between them are broken at random, so
    // the check is "never one of team 2's own spawns" (x 200, or (-300, 300) spread 20)
    const int team2_spawn = fabsf(c->position.x - 200.0f) < 0.01f || (fabsf(c->position.x + 300.0f) < 28.0f && fabsf(c->position.z - 300.0f) < 28.0f);
    CHECK(c->team == 2 && !team2_spawn, "flipped: team 2 at a team 1 (or neutral) spawn (%.0f, %.0f)", c->position.x, c->position.z);
}

// v7 getSpawnPoint: teamOnly and spread
static void test_spawn_v7(Game *game) {
    printf("Running v7 spawn point test...\n");
    start_mode_on(game, MODE_ID_FFA, 1);
    int team_only = 0, spread = 0;
    for (int i = 0; i < 40; i++) {
        Player *p = add_player(game, 100 + i, 0);
        team_only += fabsf(p->position.x - 150.0f) < 0.01f && fabsf(p->position.z - 150.0f) < 0.01f;
        const float dx = p->position.x + 300.0f, dz = p->position.z - 300.0f;
        if (sqrtf(dx * dx + dz * dz) < 25.0f) spread += sqrtf(dx * dx + dz * dz) > 0.5f;
        p->active = 0;
    }
    CHECK(team_only == 0, "FFA never takes a teamOnly spawn (%d)", team_only);
    (void) spread;

    // team 2: its plain spawn (200, 0), the teamless one, or the spread one (-300, 300, spread 20): within 27 - 10
    start_mode_on(game, MODE_ID_TDM, 1);
    int spread_hits = 0, bad = 0;
    for (int i = 0; i < 40; i++) {
        Player *p = add_player(game, 200 + i, 0);
        if (p->team != 2) {
            p->active = 0;
            continue;
        }
        const float sx = p->position.x + 300.0f, sz = p->position.z - 300.0f;
        const float d = sqrtf(sx * sx + sz * sz);
        if (d <= 17.71f) spread_hits += d > 0.5f;
        else if (!(fabsf(p->position.x - 200.0f) < 0.01f && fabsf(p->position.z) < 0.01f) &&
                 !(fabsf(p->position.x + 150.0f) < 0.01f && fabsf(p->position.z + 150.0f) < 0.01f)) bad++; // or the teamless one
        p->active = 0;
    }
    CHECK(!bad && spread_hits > 0, "team 2: own spawns only, the spread one moves the point (%d spread, %d bad)", spread_hits, bad);
}

// zom shell + raid v7
static void test_zombies_raid(Game *game) {
    printf("Running Zombies shell / Raid v7 test...\n");
    start_mode(game, MODE_ID_ZOM);
    Player *a = add_player(game, 1, 0);
    Player *b = add_player(game, 2, 0);
    CHECK(a->team == 1 && b->team == 1 && a->score == 150 && a->class_index == 9 && a->loadout[0] == 2, "zom: friendly, 150 starter score, pistol");
    tick(game, 12.5f, 0.5f); // lives modes: "match starts in " 12 s
    a->score = 1000;
    kill(b, NULL, 0);
    CHECK(a->score == 900, "a death costs everyone 10%% (%d)", a->score);
    kill(a, NULL, 0);
    tick(game, 0.1f, 0.1f);
    CHECK(ended(game) && end_result_for(a->uid) == 0 && a->score == 100, "everyone down: winCondition 2, score = wave x 100");

    start_mode(game, MODE_ID_RAID);
    a = add_player(game, 1, 0);
    CHECK(mode_hostable(MODE_ID_RAID) && fabsf(game->round_timer - 1500.0f) < 0.01f, "raid hostable, 25 min (v10 gameTime 1500000)");
    // raid.js onEnd: with time left, every active player without a time gets the elapsed clock;
    // v7 has no boss-death end timer any more (v3.1.1's Lord of Souls rule)
    game->round_timer = 123.5f;
    mode_on_game_end(game);
    CHECK(a->mode.has_timer && fabsf(a->mode.timer - (1500.0f - 123.5f)) < 1e-3f, "onEnd: time = elapsed clock (%.2f)", a->mode.timer);
    a->mode.has_timer = 0;
    game->round_timer = 1500.0f;
    tick(game, 20.5f, 0.5f);
    tick(game, 10.0f, 0.5f);
    kill(a, NULL, 0);
    tick(game, 0.1f, 0.1f);
    CHECK(ended(game) && end_result_for(a->uid) == 0, "everyone out: team 2 (the raid) wins");
}

// the WP-D packets (140-149) and the joiner sync
static void test_net_v7(Game *game) {
    printf("Running v7 mode packet test...\n");
    start_mode_on(game, MODE_ID_DOM, 1);
    add_player(game, 1, 0);
    g_sent_count = 0;
    game_send_round_state(game, 1);
    int count = 0;
    const SentPacket *sync = count_type(NET_PACKET_MODE_SYNC_V7, &count);
    CHECK(sync && sync->length == sizeof(NetModeSyncV7Packet), "MODE_SYNC_V7 to the joiner");
    const SentPacket *doms = count_type(NET_PACKET_DOM_UPDATE, &count);
    CHECK(doms && doms->uid == 1, "sync.doms to the joiner");
    if (doms) {
        NetDomUpdatePacket update;
        memcpy(&update, doms->payload, sizeof(update));
        CHECK(update.count == 3, "every zone (%d)", update.count);
    }
    CHECK(sizeof(NetDomUpdatePacket) <= NET_MAX_PAYLOAD && sizeof(NetModeSyncV7Packet) <= NET_MAX_PAYLOAD && sizeof(NetSpawnableV7Packet) <= NET_MAX_PAYLOAD &&
          sizeof(NetRoundsPacket) <= NET_MAX_PAYLOAD && sizeof(NetModeStatePacket) <= NET_MAX_PAYLOAD, "v7 packets fit a frame");
    CHECK(NET_PACKET_OBJECTIVE_V7 == 140 && NET_PACKET_MODE_SYNC_V7 == 149, "block 140-149");

    start_mode(game, MODE_ID_KC);
    Player *a = add_player(game, 1, 0);
    Player *b = add_player(game, 2, 0);
    g_sent_count = 0;
    kill(b, a, 1);
    const SentPacket *crsp = last_sent(NET_PACKET_SPAWNABLE_V7, NET_TARGET_ALL);
    CHECK(crsp != NULL, "crsp");
    if (crsp) {
        NetSpawnableV7Packet packet;
        memcpy(&packet, crsp->payload, sizeof(packet));
        CHECK(packet.type == SPAWNABLE_TAG && packet.active && packet.team == b->team, "crsp type 0, the victim's team");
    }
}

static void test_net(Game *game) {
    printf("Running mode packet test...\n");
    start_mode(game, MODE_ID_CTF);
    add_player(game, 1, 0);
    g_sent_count = 0;
    game_send_round_state(game, 1);
    const SentPacket *sent = last_sent(NET_PACKET_MODE_STATE, 1);
    CHECK(sent && sent->length == sizeof(NetModeStatePacket), "MODE_STATE to the joiner");
    if (sent) {
        NetModeStatePacket state;
        memcpy(&state, sent->payload, sizeof(state));
        CHECK(state.mode_id == MODE_ID_CTF && state.flag_count == 2 && !strcmp(state.flag_info[0], "AT BASE") && state.lives == -1, "snapshot");
        // the snapshot's flags carry the uid and the active bit of the mode's flags (protocol 1.4.3)
        int flags_match = 1;
        for (int f = 0; f < state.flag_count; f++) {
            flags_match = flags_match && state.flags[f].uid == game->mode->flags[f].uid && state.flags[f].active == game->mode->flags[f].active;
        }
        CHECK(flags_match && state.flags[0].uid >= 0, "snapshot flags carry the uid and active bit (uid %d)", state.flags[0].uid);
    }
    CHECK(sizeof(NetModeStatePacket) <= NET_MAX_PAYLOAD && sizeof(NetModeEndPacket) <= NET_MAX_PAYLOAD, "mode packets fit a frame (%zu / %zu)",
          sizeof(NetModeStatePacket), sizeof(NetModeEndPacket));
    CHECK(game_mode_index_for(game, MODE_ID_INFECT) >= 0 && game->modes[game_mode_index_for(game, MODE_ID_INFECT)] == MODE_ID_INFECT, "INIT mode id -> index");
    CHECK(count_sent(NET_PACKET_MODE_STATE) >= 1, "sent");
}

// WP-E: players.syncLeaders "7": mode.leaderStats columns (endForm applied) in the leader order.
static NetLeaderStatsPacket leader_stats_sent(int *found) {
    NetLeaderStatsPacket stats;
    memset(&stats, 0, sizeof(stats));
    const SentPacket *sent = last_sent(NET_PACKET_LEADER_STATS, NET_TARGET_ALL);
    *found = sent && sent->length == sizeof(stats);
    if (*found) memcpy(&stats, sent->payload, sizeof(stats));
    return stats;
}

static void test_leader_stats(Game *game) {
    printf("Running leader stats test...\n");
    CHECK(NET_PACKET_LEADER_STATS == NET_PACKET_BLOCK_SOCIAL && NET_PACKET_BLOCK_SANDBOX == 250, "WP-E block 240-249");
    CHECK(NET_PACKET_BLOCK_END == 255, "KrunkNative Object Manager block 250-254");
    CHECK(sizeof(NetLeaderStatsPacket) <= NET_MAX_PAYLOAD, "LEADER_STATS fits a frame (%zu)", sizeof(NetLeaderStatsPacket));

    start_mode(game, MODE_ID_FFA); // ["score", "kills", "deaths"], orderByScore
    Player *a = add_player(game, 1, 0);
    Player *b = add_player(game, 2, 0);
    a->score = 100, a->kills = 1, a->deaths = 4;
    b->score = 300, b->kills = 3, b->deaths = 2;
    g_sent_count = 0;
    game_sync_leaders(game, NET_TARGET_ALL);
    int found;
    NetLeaderStatsPacket stats = leader_stats_sent(&found);
    CHECK(found && stats.count == 2 && stats.stat_count == 3, "FFA: 2 rows x 3 columns (%d x %d)", stats.count, stats.stat_count);
    CHECK(stats.uid[0] == 2 && stats.uid[1] == 1, "by score (%d, %d)", stats.uid[0], stats.uid[1]);
    CHECK(stats.values[0][0] == 300 && stats.values[0][1] == 3 && stats.values[0][2] == 2, "score / kills / deaths");

    start_mode(game, MODE_ID_ZOM); // ["points", "zombies", "downs", "dmg"]
    a = add_player(game, 1, 0);
    a->score = 450, a->deaths = 2, a->damage_dealt = 99.6f;
    a->mode.ai_kills = 7;
    g_sent_count = 0;
    game_sync_leaders(game, NET_TARGET_ALL);
    stats = leader_stats_sent(&found);
    CHECK(found && stats.count == 1 && stats.stat_count == 4, "Zombies: 4 columns (%d)", stats.stat_count);
    CHECK(stats.values[0][0] == 450 && stats.values[0][1] == 7 && stats.values[0][2] == 2 && stats.values[0][3] == 100,
          "points = score, zombies = aiKills, downs = deaths, dmg = round(dmgDealt) (%d %d %d %d)",
          stats.values[0][0], stats.values[0][1], stats.values[0][2], stats.values[0][3]);

    start_mode(game, MODE_ID_RAID); // botsAsAI: bots stay off the board
    a = add_player(game, 1, 0);
    b = add_player(game, 2, 0);
    b->is_bot = 1;
    b->score = 999;
    g_sent_count = 0;
    game_sync_leaders(game, NET_TARGET_ALL);
    stats = leader_stats_sent(&found);
    CHECK(found && stats.count == 1 && stats.uid[0] == 1 && stats.stat_count == 1, "Raid: no bots, [\"score\"] (%d rows)", stats.count);
}

// WP-E part 2a: the victim's "3" extras (NET_PACKET_DEATH_INFO) ahead of its KILL, and the
// config.playerCardStats / class level formulas.
static void test_death_info(Game *game) {
    printf("Running death info test...\n");
    CHECK(NET_PACKET_DEATH_INFO == NET_PACKET_BLOCK_SOCIAL + 1 && NET_PACKET_PC_STAT == NET_PACKET_BLOCK_SOCIAL + 2, "WP-E part 2a ids 241 / 242");
    CHECK(sizeof(NetDeathInfoPacket) <= NET_MAX_PAYLOAD, "DEATH_INFO fits a frame");

    start_mode(game, MODE_ID_FFA);
    game->now = 2.0f;
    Player *killer = add_player(game, 1, 0);
    Player *victim = add_player(game, 2, 0);
    CHECK(victim->life_start == 2.0f && !victim->life_shots && !victim->life_hits && !victim->life_kills && !victim->life_score,
          "spawn resets the life counters");
    CHECK(killer->pc_stat_index == -1 && !killer->class_stat, "no account values by default");
    player_score(victim, 40, 0);
    CHECK(victim->life_score == 40, "scoreL follows the score (%d)", victim->life_score);
    victim->life_shots = 8;
    victim->life_hits = 3;
    killer->class_stat = 2500;
    killer->pc_stat_index = 3;
    snprintf(killer->pc_stat_value, sizeof(killer->pc_stat_value), "1.52");
    game->now = 7.25f;
    g_sent_count = 0;
    PlayerKillInfo info = {.weapon_id = 1};
    player_kill(victim, killer, &info, 1);
    int death_at = -1, kill_at = -1;
    for (int i = 0; i < g_sent_count; i++) {
        if (g_sent[i].type == NET_PACKET_DEATH_INFO && death_at < 0) death_at = i;
        if (g_sent[i].type == NET_PACKET_KILL && kill_at < 0) kill_at = i;
    }
    CHECK(death_at >= 0 && kill_at > death_at && g_sent[death_at].uid == 2, "DEATH_INFO to the victim, before the KILL");
    NetDeathInfoPacket death;
    memset(&death, 0, sizeof(death));
    if (death_at >= 0 && g_sent[death_at].length == sizeof(death)) memcpy(&death, g_sent[death_at].payload, sizeof(death));
    CHECK(death.victim_uid == 2 && death.killer_uid == 1 && death.weapon_index == 1, "victim / killer / weapon");
    CHECK(death.class_stat == 2500 && death.pc_stat_index == 3 && !strcmp(death.pc_stat_value, "1.52"), "the killer's class and card stat");
    CHECK(death.time_alive_ms == 5250 && death.shots == 8 && !strcmp(death.accuracy, "37.5%") && death.score == 40,
          "life stats (%d ms, %d shots, %s, score %d)", death.time_alive_ms, death.shots, death.accuracy, death.score);
    CHECK(killer->life_kills == 1, "killsL counts the kill");

    // config.playerCardStats and f1975's class level
    cJSON *account = cJSON_Parse("{\"kills\":300,\"deaths\":120,\"games\":40,\"wins\":25,\"score\":45000,\"funds\":12,"
                                 "\"stats\":{\"s\":1000,\"h\":381,\"hs\":77,\"c2\":10000}}");
    char v[16];
    CHECK(player_card_stat_format(0, account, v, sizeof(v)) && !strcmp(v, "150.00"), "SPK %s", v);
    CHECK(player_card_stat_format(3, account, v, sizeof(v)) && !strcmp(v, "2.50"), "KDR %s", v);
    CHECK(player_card_stat_format(7, account, v, sizeof(v)) && !strcmp(v, "1.67"), "W/L %s", v);
    CHECK(player_card_stat_format(10, account, v, sizeof(v)) && !strcmp(v, "38.1%"), "Accuracy %s", v);
    CHECK(player_card_stat_format(12, account, v, sizeof(v)) && !strcmp(v, "77"), "Headshots %s", v);
    CHECK(player_card_stat_format(13, account, v, sizeof(v)) && !strcmp(v, "15"), "Losses %s", v);
    CHECK(player_card_stat_format(23, account, v, sizeof(v)) && !strcmp(v, "619"), "Misses %s", v);
    CHECK(!player_card_stat_format(PLAYER_CARD_STAT_COUNT, account, v, sizeof(v)) && !player_card_stat_format(0, NULL, v, sizeof(v)), "no stat");
    CHECK(!strcmp(player_card_stat_name(27), "War Machines"), "28 stats");
    cJSON_Delete(account);
    CHECK(player_card_class_level(0) == 1 && player_card_class_level(10000) == 4 && player_card_class_level(2500) == 2, "class levels");
    player_card_accuracy(0, 0, v, sizeof(v));
    CHECK(!strcmp(v, "0%"), "no shots: 0%%");
}

// WP-E part 2c: v7 stat / challenge events (GameHooks.progress), incMedal counts and the end
// screen's NET_PACKET_END_MEDALS, the level progress curve and challenges.finalize.
#define PROG_EVENTS_MAX 64
static struct {
    int uid, stat, value, weapon, streak;
    char key[8];
} g_prog[PROG_EVENTS_MAX];
static int g_prog_count;

static void test_progress_hook(Game *game, const Player *player, const int stat, const char *key, const int value, const int weapon, const int streak,
                               const int item) {
    (void) game;
    (void) item;
    if (g_prog_count >= PROG_EVENTS_MAX) return;
    g_prog[g_prog_count].uid = player->uid;
    g_prog[g_prog_count].stat = stat;
    g_prog[g_prog_count].value = value;
    g_prog[g_prog_count].weapon = weapon;
    g_prog[g_prog_count].streak = streak;
    snprintf(g_prog[g_prog_count].key, sizeof(g_prog[0].key), "%s", key);
    g_prog_count++;
}

static int prog_has(const int uid, const int stat, const char *key, const int value, const int weapon) {
    for (int i = 0; i < g_prog_count; i++)
        if (g_prog[i].uid == uid && g_prog[i].stat == stat && !strcmp(g_prog[i].key, key) && g_prog[i].value == value && g_prog[i].weapon == weapon) return 1;
    return 0;
}

static void test_progression(Game *game) {
    printf("Running progression test...\n");
    CHECK(NET_PACKET_END_MEDALS == NET_PACKET_BLOCK_SOCIAL + 3 && sizeof(NetEndMedalsPacket) <= NET_MAX_PAYLOAD, "END_MEDALS: id 243, fits a frame");
    CHECK(PLAYER_MEDAL_SLOTS == PROGRESSION_MEDAL_COUNT && progression_medal_index("Headshot") == 9 && progression_medal_index("Cycled") == 55 &&
          progression_medal_index("Nuke") == -1, "overlay.medalsList");

    start_mode(game, MODE_ID_FFA);
    game->hooks.progress = test_progress_hook;
    g_prog_count = 0;
    Player *killer = add_player(game, 1, 0);
    Player *victim = add_player(game, 2, 0);
    killer->has_account = 1; // the server's player.account
    PlayerKillInfo info = {.weapon_id = 1, .headshot = 1};
    player_kill(victim, killer, &info, 0);
    CHECK(prog_has(2, 0, "d", 1, -1), "challenges.update(\"d\") for the victim");
    CHECK(prog_has(1, 0, "k", 1, 1) && prog_has(1, 0, "kstk", 1, -1) && prog_has(1, 0, "kstk", 1, 1), "\"k\" and \"kstk\" (kill / weapon streak)");
    CHECK(prog_has(1, 1, "hs", 1, -1) && prog_has(1, 0, "hs", 1, 1), "headshot kill: incStat + challenge");
    CHECK(prog_has(1, 0, "fb", 1, 1), "First Blood: \"fb\"");
    int score_events = 0;
    for (int i = 0; i < g_prog_count; i++) score_events += g_prog[i].uid == 1 && !strcmp(g_prog[i].key, "score") && g_prog[i].value == killer->score;
    CHECK(score_events == 1, "challenges.update(\"score\") with the kill's score (%d)", killer->score);
    CHECK(killer->medal_counts[progression_medal_index("First Blood")] == 1 && killer->medal_counts[progression_medal_index("Headshot")] == 1,
          "incMedal: First Blood + Headshot");
    game_inc_medal(game, victim, "Assist");
    CHECK(!victim->medal_counts[progression_medal_index("Assist")], "no medals without an account");
    game_inc_medal(game, killer, "Not A Medal");
    game->hooks.progress = NULL;

    g_sent_count = 0;
    game_end_round(game);
    int medals_at = -1, end_at = -1;
    for (int i = 0; i < g_sent_count; i++) {
        if (g_sent[i].type == NET_PACKET_END_MEDALS && g_sent[i].uid == 1 && medals_at < 0) medals_at = i;
        if (g_sent[i].type == NET_PACKET_END && g_sent[i].uid == 1 && end_at < 0) end_at = i;
    }
    CHECK(medals_at >= 0 && end_at > medals_at, "END_MEDALS before the player's END");
    NetEndMedalsPacket medals;
    memset(&medals, 0, sizeof(medals));
    if (medals_at >= 0 && g_sent[medals_at].length == sizeof(medals)) memcpy(&medals, g_sent[medals_at].payload, sizeof(medals));
    CHECK(medals.count == 2 && medals.medals[0].index == 2 && medals.medals[0].count == 1 && medals.medals[1].index == 9,
          "endData.mdls in list order (%d entries)", medals.count);

    // getLevelProg (v7): linear in score between the level bounds
    CHECK(progression_level(0, 0.03) == 1 && progression_level(10000, 0.03) == 3, "levels");
    const double prog = progression_level_prog(7222, 0.03);
    CHECK(prog > 49.9 && prog < 50.0 && progression_level_prog(10000, 0.03) == 0.0 && progression_level_prog(2222, 0.03) == 50.0,
          "level progress %.3f", prog);

    // challenges.finalize
    cJSON *active = cJSON_Parse("["
        "{\"CI\":1,\"T\":\"map\",\"DAT\":{\"ky\":\"k\",\"val\":10,\"mp\":0},\"REW\":{\"kr\":5,\"sk\":[7,8]},\"PV\":4,\"PT\":0},"
        "{\"CI\":2,\"T\":\"weapon\",\"DAT\":{\"ky\":\"k\",\"val\":50,\"wep\":1},\"REW\":{\"kr\":5},\"PV\":0,\"PT\":0},"
        "{\"CI\":3,\"T\":\"map\",\"DAT\":{\"ky\":\"k\",\"val\":10,\"mp\":3},\"REW\":{},\"PV\":0,\"PT\":0},"
        "{\"CI\":4,\"T\":\"daily\",\"DAT\":{\"ky\":\"kstk\",\"val\":5},\"REW\":{\"kr\":1},\"PV\":0,\"PT\":0},"
        "{\"CI\":5,\"T\":\"daily\",\"DAT\":{\"ky\":\"kstk\",\"val\":3,\"wep\":1},\"REW\":{},\"PV\":0,\"PT\":0},"
        "{\"CI\":6,\"T\":\"nightmare\",\"DAT\":{\"ky\":\"score\",\"val\":1000,\"sm\":1},\"REW\":{},\"PV\":0,\"PT\":0},"
        "{\"CI\":7,\"T\":\"map\",\"DAT\":{\"ky\":\"chall\",\"val\":1},\"REW\":{\"kr\":2},\"PV\":0,\"PT\":0},"
        "{\"CI\":8,\"T\":\"daily\",\"DAT\":{\"ky\":\"uwk\",\"val\":9},\"REW\":{},\"PV\":0,\"PT\":0},"
        "{\"CI\":9,\"T\":\"ranked\",\"DAT\":{\"ky\":\"w\",\"val\":1},\"REW\":{},\"PV\":0,\"PT\":0}]");
    cJSON *events = cJSON_Parse("[[\"k\",3,-1,-1],[\"k\",4,1,-1],[\"kstk\",6,-1,-1],[\"kstk\",2,1,-1],[\"score\",400,-1,-1],[\"uwk\",3,-1,-1],[\"w\",1,-1,-1]]");
    cJSON *results = cJSON_CreateArray(), *completed = cJSON_CreateArray(), *reward = cJSON_CreateObject();
    const ChallengeMatch match = {.map_index = 0, .map_id = -1, .mode = 0, .class_index = 0};
    challenges_finalize(active, events, &match, results, completed, reward);
    char *text = cJSON_PrintUnformatted(results);
    // 1: 4 + 7 = 11 >= 10 done; 2: 4 with the weapon; 3 other map; 4: nonWep streak 6 >= 5 done; 5: weapon
    // streak 2 < 3 (a SET/MAX key below its value is not saved); 6: single match 400 < 1000 (reset);
    // 7: one completed map challenge; 8: MAX below its value; 9: not a ranked game
    CHECK(text && !strcmp(text, "[1,1,11,0,2,4,1,4,6,1,7,1]"), "chg triples %s", text ? text : "");
    free(text);
    CHECK(cJSON_GetArraySize(completed) == 3 && cJSON_GetNumberValue(cJSON_GetObjectItem(reward, "kr")) == 8 &&
          cJSON_GetArraySize(cJSON_GetObjectItem(reward, "skins")) == 2, "completed 1, 4, 7; reward 8 KR + 2 skins");
    CHECK(cJSON_GetNumberValue(cJSON_GetObjectItem(cJSON_GetArrayItem(active, 0), "PT")) == 1, "PT set in place");
    // a player who left mid-match keeps only DAT.nem challenges
    cJSON *results2 = cJSON_CreateArray();
    const ChallengeMatch quit = {.map_index = 0, .map_id = -1, .quit = 1};
    CHECK(challenges_finalize(active, events, &quit, results2, NULL, NULL) == 0 && cJSON_GetArraySize(results2) == 0, "quit: nothing");
    char name[96];
    cJSON *daily = cJSON_Parse("{\"T\":\"daily\",\"N\":\"\",\"DAT\":{\"ky\":\"hs\",\"val\":5,\"wep\":0}}");
    challenge_name(daily, NULL, name, sizeof(name));
    CHECK(!strcmp(name, "Get 5 headshots "), "updateNames without weapon names: \"%s\"", name);
    cJSON *stats = cJSON_Parse("{\"n\":760}");
    CHECK(badge_requirement_met(11, stats) && badge_requirement_met(13, stats) && !badge_requirement_met(14, stats) &&
          !badge_requirement_met(22, stats), "Nuclear badge rules");
    cJSON_Delete(stats);
    cJSON_Delete(daily);
    cJSON_Delete(results2);
    cJSON_Delete(active);
    cJSON_Delete(events);
    cJSON_Delete(results);
    cJSON_Delete(completed);
    cJSON_Delete(reward);
}

// v7.1.4 challenge contexts the 2c port skipped: "uwk" (uniqueWeaponKills), incStat "kjg" / "jgk"
static void test_progression_contexts(Game *game) {
    printf("Running progression contexts test...\n");
    start_mode(game, MODE_ID_FFA);
    game->hooks.progress = test_progress_hook;
    g_prog_count = 0;
    Player *killer = add_player(game, 1, 0);
    Player *victim = add_player(game, 2, 0);
    killer->has_account = 1;
    PlayerKillInfo info = {.weapon_id = 1};
    player_kill(victim, killer, &info, 0);
    CHECK(prog_has(1, 0, "uwk", 1, -1), "uniqueWeaponKills: \"uwk\" 1");
    Player *jugg = add_player(game, 3, 0);
    jugg->is_juggernaut = 1;
    PlayerKillInfo info2 = {.weapon_id = 3};
    player_kill(jugg, killer, &info2, 0);
    CHECK(prog_has(1, 0, "uwk", 2, -1), "a second weapon: \"uwk\" 2");
    CHECK(prog_has(1, 1, "kjg", 1, -1) && !prog_has(1, 1, "jgk", 1, -1), "incStat(\"kjg\") for killing a Juggernaut");
    int uwk_events = 0;
    Player *third = add_player(game, 4, 0);
    PlayerKillInfo info3 = {.weapon_id = 3};
    player_kill(third, killer, &info3, 0);
    for (int i = 0; i < g_prog_count; i++) uwk_events += g_prog[i].uid == 1 && !strcmp(g_prog[i].key, "uwk");
    CHECK(uwk_events == 2, "the same weapon again: no \"uwk\" (%d events)", uwk_events);
    killer->is_juggernaut = 1;
    Player *fourth = add_player(game, 5, 0);
    PlayerKillInfo info4 = {.weapon_id = 3};
    player_kill(fourth, killer, &info4, 0);
    CHECK(prog_has(1, 1, "jgk", 1, -1), "incStat(\"jgk\") for a kill as a Juggernaut");
    killer->is_juggernaut = 0;
    game->hooks.progress = NULL;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    cJSON *sandstorm = NULL;
    if (load_json_file("assets/maps/311_sandstorm.json", &sandstorm) && load_json_file("../assets/maps/311_sandstorm.json", &sandstorm)) {
        printf("Failed to load assets/maps/311_sandstorm.json (run from the repository root)\n");
        return 1;
    }
    g_test_maps[0] = sandstorm;
    cJSON *dom = NULL, *objectives = NULL;
    if (load_json_file("shared/tests/maps/v7_dom.json", &dom) || load_json_file("shared/tests/maps/v7_objectives.json", &objectives)) {
        printf("Failed to load shared/tests/maps/v7_*.json (run from the repository root)\n");
        return 1;
    }
    g_test_maps[1] = dom;
    g_test_maps[2] = objectives;

    static Game game;
    test_table();
    test_ffa(&game);
    test_tdm(&game);
    test_swap_team(&game);
    test_point(&game);
    test_king(&game);
    test_ctf(&game);
    test_dom_zone_objects(&game);
    test_kc(&game);
    test_hide(&game);
    test_infect(&game);
    test_lms(&game);
    test_gun(&game);
    test_boss(&game);
    test_stalker(&game);
    test_simon_traitor(&game);
    test_oinc(&game);
    test_shrp(&game);
    test_misc(&game);
    test_net(&game);
    test_objectives_v7(&game);
    test_flags_v7(&game);
    test_dom(&game);
    test_deposit(&game);
    test_sdm(&game);
    test_modifiers(&game);
    test_rounds(&game);
    test_round_wins_outside_best_of(&game);
    test_spawn_v7(&game);
    test_zombies_raid(&game);
    test_net_v7(&game);
    test_leader_stats(&game);
    test_death_info(&game);
    test_progression(&game);
    test_progression_contexts(&game);

    if (g_failures) {
        printf("modes_test: %d of %d check(s) failed\n", g_failures, g_checks);
        return 1;
    }

    printf("Test OK (%d checks)\n", g_checks);
    return 0;
}
