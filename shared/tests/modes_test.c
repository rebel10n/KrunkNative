// Krunker v7.1.4 game modes (shared/src/modes, doc/v7/map-logic.md §4.1) with simulated players
// on Sandstorm and the v7 fixtures (shared/tests/maps/v7_dom.json, v7_objectives.json): the mode
// table, team assignment, scoring, objectives (Hardpoint / KOTH zones by uid, CTF / Blitz /
// neutral flags, Kill Confirmed tags, Deposit KR Tags, Domination zones), waitTimer picks (Hide &
// Seek, Infected, Boss, Stalker, Simon, Traitor), lives, loadouts, the modifier modes, rounds /
// best of / overtime and every mode's end-of-round rule (conditions + winConditions), through
// the shared game hooks (no sockets). Run from the repository root: ./<build>/modes_test
#include <shared.h>
#include <spawnables.h>
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
        "Ranked", "Ranked",
    };
    static const char *const keys[MODE_V7_TABLE_SIZE] = {
        "ffa", "tdm", "point", "ctf", "bhop", "hide", "infect", "race", "lms", "simon", "gun", "prop", "boss", "clas",
        "depo", "stalk", "king", "oinc", "trade", "kc", "def", "shrp", "trai", "raid", "blitz", "dom", "sdm", "krank",
        "tdf", "depoffa", "car", "aon", "md", "chs", "bhffa", "zom", "imp",
    };

    int count = 0;
    const GameModeConfig *modes = mode_list(&count);
    CHECK(count == 39 && mode_count() == 39 && MODE_V7_TABLE_SIZE == 37, "v7.1.4 has 37 modes + KN Ranked 37 / 38 (%d)", count);
    for (int i = 0; i < count; i++) {
        CHECK(modes[i].id == i && modes[i].name && !strcmp(modes[i].name, names[i]), "mode %d is %s (%s)", i, names[i], modes[i].name);
        if (i < MODE_V7_TABLE_SIZE) CHECK(modes[i].key && !strcmp(modes[i].key, keys[i]), "mode %d key %s", i, keys[i]);
    }

    CHECK(mode_find("tdm") == MODE_ID_TDM && mode_find("Capture the Flag") == MODE_ID_CTF && mode_find("19") == MODE_ID_KC, "mode_find");
    CHECK(mode_find("ranked") == MODE_ID_RANKED_POINT && mode_find("nope") == -1 && mode_find("dom") == 25, "mode_find ranked / unknown / dom");
    CHECK(MODE_ID_CLAS == 13 && MODE_ID_DEPO == 14 && MODE_ID_RANKED_POINT == 37 && MODE_ID_RANKED == 38, "v7 ids 13 / 14, Ranked 37 / 38");
    CHECK(g_rotation_modes[0] == 0 && g_rotation_modes[1] == 2 && g_rotation_modes[2] == 3 && g_rotation_modes[3] == 19 &&
          g_rotation_modes[4] == 25, "rotationModes [0, 2, 3, 19, 25]");
    CHECK(mode_hostable(MODE_ID_RAID) && !mode_hostable(MODE_ID_RANKED) && !mode_hostable(MODE_ID_RANKED_POINT) && mode_hostable(MODE_ID_GUN) &&
          !mode_hostable(MODE_ID_CAR) && !mode_hostable(MODE_ID_AON) && !mode_hostable(MODE_ID_MD) && !mode_hostable(MODE_ID_ZOM) &&
          !mode_hostable(MODE_ID_IMP) && mode_hostable(MODE_ID_DOM), "preventCustomGames (raid hostable again)");
    CHECK(mode_config(MODE_ID_RANKED)->is_ranked && mode_config(MODE_ID_RANKED_POINT)->is_ranked && !mode_config(MODE_ID_DEPO)->is_ranked, "Ranked flags");

    const GameModeConfig *stalk = mode_config(MODE_ID_STALK);
    CHECK(stalk->has_fog_dist && stalk->fog_dist == 250.0f && stalk->has_fog_col && stalk->fog_col == 1842204 &&
          stalk->has_sky_col && stalk->sky_col == 1842204 && stalk->has_light_col && stalk->light_col == 855309 &&
          stalk->has_amb_col && stalk->amb_col == 1513239, "Stalker lighting overrides");
    const GameModeConfig *ffa = mode_config(MODE_ID_FFA);
    CHECK(!ffa->has_fog_dist && !ffa->has_fog_col && !ffa->has_sky_col && !ffa->has_light_col && !ffa->has_amb_col, "FFA keeps the map lighting");
    CHECK(mode_config(MODE_ID_INFECT)->speed_mlt[2] == 1.15f && mode_config(MODE_ID_HIDE)->speed_mlt[1] == 0.8f, "spdMlt");
    static const int order[] = {2, 27, 16, 10, 4, 11, 21, 5, 3, 9, 1, 14, 6, 0, 18, 7, 8, 13, 22, 12};
    const GameModeConfig *gun = mode_config(MODE_ID_GUN);
    CHECK(gun->weapon_order_size == 20 && !memcmp(gun->weapon_order, order, sizeof(order)), "Gun Game v7 order");
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
    CHECK(zombie && zombie->cosmetics.skin_color == MODE_INFECTED_SKIN && zombie->cosmetics.hat == MODE_INFECTED_HAT, "Infected outfit");

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
    CHECK(stalker && stalker->max_health == 400 && stalker->loadout[0] == WEAPON_ID_KNIFE && stalker->loadout_size == 1, "a 400 hp knife-only Stalker");
    CHECK(stalker && stalker->cosmetics.skin_color == 0 && stalker->cosmetics.hat == -1, "Stalker outfit");
    CHECK(game->mode->config.speed_mlt[1] == 0.65f, "the Living walk slowly");

    Player *living = first_on_team(game, 1);
    const float damage = mode_melee_damage(game, stalker, living, 50.0f);
    CHECK(damage == living->health, "onMelee: one stab kills (%.0f)", damage);

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
    CHECK(fabsf(game->round_timer - 360.0f) < 0.01f && a->class_index == 9 && a->mode.lives == 1, "Raid config");

    start_mode(game, MODE_ID_RANKED);
    a = add_player(game, 1, 0);
    b = add_player(game, 2, 0);
    tick(game, 10.5f, 0.5f);
    a->kills = 10;
    tick(game, 0.1f, 0.1f);
    CHECK(ended(game) && end_result_for(a->uid) == 1, "Ranked: first to 10 kills");
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

// endGame rounds: best of, intermission (flipped spawns, locked movement), overtime
static void test_rounds(Game *game) {
    printf("Running rounds / best of / overtime test...\n");
    start_mode(game, MODE_ID_TDM);
    GameMode *mode = game->mode;
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
    const int team1_spawn = fabsf(c->position.x + 200.0f) < 0.01f || (fabsf(c->position.x - 150.0f) < 0.01f && fabsf(c->position.z - 150.0f) < 0.01f);
    CHECK(c->team == 2 && team1_spawn, "flipped: team 2 at a team 1 spawn (%.0f, %.0f)", c->position.x, c->position.z);
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
    CHECK(mode_hostable(MODE_ID_RAID) && fabsf(game->round_timer - 360.0f) < 0.01f, "raid hostable, 6 min");
    // raid.js onEnd: with time left, every active player without a time gets the elapsed clock;
    // v7 has no boss-death end timer any more (v3.1.1's Lord of Souls rule)
    game->round_timer = 123.5f;
    mode_on_game_end(game);
    CHECK(a->mode.has_timer && fabsf(a->mode.timer - (360.0f - 123.5f)) < 1e-3f, "onEnd: time = elapsed clock (%.2f)", a->mode.timer);
    a->mode.has_timer = 0;
    game->round_timer = 360.0f;
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
    }
    CHECK(sizeof(NetModeStatePacket) <= NET_MAX_PAYLOAD && sizeof(NetModeEndPacket) <= NET_MAX_PAYLOAD, "mode packets fit a frame (%zu / %zu)",
          sizeof(NetModeStatePacket), sizeof(NetModeEndPacket));
    CHECK(game_mode_index_for(game, MODE_ID_INFECT) >= 0 && game->modes[game_mode_index_for(game, MODE_ID_INFECT)] == MODE_ID_INFECT, "INIT mode id -> index");
    CHECK(count_sent(NET_PACKET_MODE_STATE) >= 1, "sent");
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    cJSON *sandstorm = NULL;
    if (load_json_file("assets/maps/sandstorm.json", &sandstorm) && load_json_file("../assets/maps/sandstorm.json", &sandstorm)) {
        printf("Failed to load assets/maps/sandstorm.json (run from the repository root)\n");
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
    test_point(&game);
    test_king(&game);
    test_ctf(&game);
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
    test_spawn_v7(&game);
    test_zombies_raid(&game);
    test_net_v7(&game);

    if (g_failures) {
        printf("modes_test: %d of %d check(s) failed\n", g_failures, g_checks);
        return 1;
    }

    printf("Test OK (%d checks)\n", g_checks);
    return 0;
}
