// Krunker v3.1.1 game modes (shared/src/modes) with simulated players on Sandstorm:
// the mode table, team assignment, scoring, objectives (Hardpoint / KOTH zones, CTF
// flags, Kill Confirmed tags), waitTimer picks (Hide & Seek, Infected, Boss, Stalker,
// Simon, Traitor), lives, Gun Game / One in the Chamber / Sharp Shooter loadouts and
// every mode's end-of-round rule (conditions + winConditions), through the shared game
// hooks (no sockets). Run from the repository root: ./<build>/modes_test
#include <shared.h>
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
static const cJSON *g_test_maps[1];

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

static void start_mode(Game *game, const int mode_id) {
    free_players(game);
    int *modes = calloc(1, sizeof(int));
    modes[0] = mode_id;
    const cJSON **maps = calloc(1, sizeof(cJSON *)); // game_configure owns (frees) the lists
    maps[0] = g_test_maps[0];
    game_configure(game, NULL, maps, 1, modes, 1, NULL, 0, NULL, 0);
    game->hooks.send = test_send;
    game->hooks.respawn = test_respawn;
    g_sent_count = 0;
    memset(g_respawns, 0, sizeof(g_respawns));
    game_init(game, 0, 0, 1);
    game->now = 0.0f;
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
        "Race", "Last Man Standing", "Simon Says", "Gun Game", "Prop Hunt", "Boss Hunt", "Ranked", "Ranked",
        "Stalker", "King of the Hill", "One in the Chamber", "Trade", "Kill Confirmed", "Defuse", "Sharp Shooter",
        "Traitor", "Raid",
    };

    int count = 0;
    const GameModeConfig *modes = mode_list(&count);
    CHECK(count == 24 && mode_count() == 24, "v3.1.1 has 24 modes (%d)", count);
    for (int i = 0; i < count; i++) {
        CHECK(modes[i].id == i && !strcmp(modes[i].name, names[i]), "mode %d is %s (%s)", i, names[i], modes[i].name);
    }

    CHECK(mode_find("tdm") == MODE_ID_TDM && mode_find("Capture the Flag") == MODE_ID_CTF && mode_find("19") == MODE_ID_KC, "mode_find");
    CHECK(mode_find("ranked") == MODE_ID_RANKED_POINT && mode_find("nope") == -1, "mode_find ranked / unknown");
    CHECK(g_rotation_modes[0] == 0 && g_rotation_modes[4] == 19, "rotationModes [0, 1, 2, 3, 19]");
    CHECK(!mode_hostable(MODE_ID_RAID) && !mode_hostable(MODE_ID_RANKED) && mode_hostable(MODE_ID_GUN), "preventCustomGames");

    const GameModeConfig *stalk = mode_config(MODE_ID_STALK);
    CHECK(stalk->has_fog_dist && stalk->fog_dist == 250.0f && stalk->has_fog_col && stalk->fog_col == 1842204 &&
          stalk->has_sky_col && stalk->sky_col == 1842204 && stalk->has_light_col && stalk->light_col == 855309 &&
          stalk->has_amb_col && stalk->amb_col == 1513239, "Stalker lighting overrides");
    const GameModeConfig *ffa = mode_config(MODE_ID_FFA);
    CHECK(!ffa->has_fog_dist && !ffa->has_fog_col && !ffa->has_sky_col && !ffa->has_light_col && !ffa->has_amb_col, "FFA keeps the map lighting");
    CHECK(mode_config(MODE_ID_INFECT)->speed_mlt[2] == 1.15f && mode_config(MODE_ID_HIDE)->speed_mlt[1] == 0.8f, "spdMlt");
    CHECK(mode_config(MODE_ID_GUN)->weapon_order_size == 17 && mode_config(MODE_ID_GUN)->weapon_order[16] == WEAPON_ID_KNIFE, "Gun Game order");
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

static void test_point(Game *game, const int mode_id) {
    printf("Running %s test...\n", mode_name(mode_id));
    start_mode(game, mode_id);
    GameMode *mode = game->mode;
    CHECK(mode->objective_count == 4, "Sandstorm has 4 objectives (%d)", mode->objective_count);
    CHECK(mode->active_objective == 0 && mode->wait_timer_count == 1 && mode->wait_timers[0].cont_time, "gameStart: objective 0 + rotation timer");

    Player *a = add_player(game, 1, 0);
    Player *b = add_player(game, 2, 0);
    const ModeObjective *objective = &mode->objectives[0];
    place(a, objective->position.x, objective->position.y - objective->half.y, objective->position.z);
    place(b, 1000.0f, 1000.0f, 1000.0f);

    tick(game, 1.5f, 0.25f);
    const int points = mode_id == MODE_ID_KING ? 20 : 10;
    CHECK(a->score == points && b->score == 0, "objScore %d per 1.5 s in the zone (%d / %d)", points, a->score, b->score);
    CHECK(a->mode.objective_score == points, "objectiveScore %d", a->mode.objective_score);
    if (mode_id == MODE_ID_POINT) CHECK(mode_team_score(game, a->team) == points, "Hardpoint: zone points feed the team (%d)", mode_team_score(game, a->team));

    const int before = a->score;
    const int team_before = mode_team_score(game, a->team);
    kill(b, a, 1);
    if (mode_id == MODE_ID_KING) CHECK(a->score == before, "KOTH noScoreK: kills give no score (%d)", a->score);
    else CHECK(a->score > before && mode_team_score(game, a->team) == team_before, "Hardpoint: kill score is not team score");

    tick(game, 60.0f, 0.5f);
    CHECK(mode->active_objective == 1, "next objective after 60 s (%d)", mode->active_objective);
    CHECK(last_sent(NET_PACKET_OBJECTIVE, NET_TARGET_ALL) != NULL, "obj broadcast");

    if (mode_id == MODE_ID_POINT) {
        mode->team_scores[1] = 5;
        mode->team_scores[2] = 30;
        game->round_timer = 0.1f;
        tick(game, 0.5f, 0.25f);
        CHECK(ended(game) && end_result_for(a->team == 2 ? 1 : 2) == 1, "the higher team score wins");
    } else {
        game->round_timer = 0.1f;
        tick(game, 0.5f, 0.25f);
        CHECK(ended(game) && end_result_for(1) == 1, "KOTH: the best score wins");
    }
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

    // tagLimit 65 ends the round, more confirms win
    a->mode.confirms = 65;
    tick(game, 0.1f, 0.1f);
    CHECK(ended(game) && end_result_for(a->uid) == 1 && end_result_for(b->uid) == 0, "tag limit: team 2 wins");
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
    Player *a = add_player(game, 1, 0);
    Player *b = add_player(game, 2, 0);
    CHECK(a->class_index == 9 && a->loadout[0] == 2 && a->loadout_size == 2, "starts on the Pistol + knife (%d)", a->loadout[0]);

    tick(game, 1.0f, 0.5f);
    kill(b, a, 2);
    CHECK(a->mode.weapon_tier == 1 && a->loadout[0] == 16, "kill: next weapon (tier %d weapon %d)", a->mode.weapon_tier, a->loadout[0]);
    CHECK(last_sent(NET_PACKET_LOADOUT, NET_TARGET_ALL) != NULL, "inat broadcast");
    CHECK(a->score > 0 && a->assists == 0, "kill rewards still score (noReward is the KR reward) (%d)", a->score);

    player_spawn(b);
    CHECK(b->loadout[0] == 2 && b->mode.weapon_tier == 0, "b respawns on tier 0");
    player_spawn(a);
    CHECK(a->loadout[0] == 16, "keepLoadout: a respawns with its tier weapon");

    // a melee kill sets the victim back
    a->mode.weapon_tier = 3;
    mode_set_player_loadout(a, &(int) {4}, 1, 0);
    kill(a, b, WEAPON_ID_KNIFE);
    CHECK(a->mode.weapon_tier == 2 && a->mode.setbacks == 1 && a->loadout[0] == 10, "setback (tier %d)", a->mode.weapon_tier);
    CHECK(b->mode.melees == 1, "melee kill counted");

    // reaching the end of the order ends the round
    player_spawn(a);
    player_spawn(b);
    a->mode.weapon_tier = 16;
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

    static Game game;
    test_table();
    test_ffa(&game);
    test_tdm(&game);
    test_point(&game, MODE_ID_POINT);
    test_point(&game, MODE_ID_KING);
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

    if (g_failures) {
        printf("modes_test: %d of %d check(s) failed\n", g_failures, g_checks);
        return 1;
    }

    printf("Test OK (%d checks)\n", g_checks);
    return 0;
}
