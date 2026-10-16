// forfeit_test: the ranked team forfeit vote (shared/src/forfeit.c; Krunker v10.0.2 voteToForfeit,
// updateForfeitVote, resolveForfeit, voteForfeitTimer 60 s, voteForfeitDisconnectGrace 30 s). The
// gates (ranked, a team, IN_PROGRESS), the team-only "vf" packets, the chat lines, a unanimous
// vote ending the round for the other team (mode_pick_winner), one start per player per game,
// the 60 s expiry and a leaver's 30 s grace. Run from the repository root.
#include <shared.h>
#include <forfeit.h>
#include <modes.h>
#include <net_ext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures, g_checks;
#define CHECK(cond, ...) do { g_checks++; \
    if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_failures++; } \
} while (0)

#define MAX_SENT 512
typedef struct {
    int uid;
    int type;
    uint16_t length;
    uint8_t payload[NET_MAX_PAYLOAD];
} Sent;
static Sent g_sent[MAX_SENT];
static int g_sent_count;

static void test_send(Game *game, const int uid, const int type, const void *payload, const uint16_t length) {
    (void) game;
    if (g_sent_count >= MAX_SENT) return;
    Sent *s = &g_sent[g_sent_count++];
    s->uid = uid;
    s->type = type;
    s->length = length;
    memcpy(s->payload, payload, length < sizeof(s->payload) ? length : sizeof(s->payload));
}

static const cJSON *g_test_maps[1];

static void open_game(Game *game) {
    memset(game, 0, sizeof(*game));
    int *modes = calloc(1, sizeof(int));
    modes[0] = MODE_ID_TDM;
    game_configure(game, NULL, g_test_maps, 1, modes, 1, NULL, 0, NULL, 0);
    game->hooks.send = test_send;
    game_init(game, 0, 0, 1);
    game->is_ranked = 1;
    game->now = 0.0f;
    g_sent_count = 0;
}

static void close_game(Game *game) {
    if (game->mode) mode_fini(game->mode);
    game->mode = NULL;
    free(game->weapons);
    free(game->classes);
    free(game->modes);
}

static Player *add_player(Game *game, const int uid, const int team) {
    Player *player = player_init(game);
    player->uid = uid;
    snprintf(player->name, sizeof(player->name), "p%d", uid);
    game_players_add(game, player); // mode_player_added picks a team: the test's team goes on after it
    player->team = team;
    return player;
}

static void remove_player(Game *game, const int uid) {
    for (size_t i = 0; i < game->player_count; i++) {
        if (game->players[i] && game->players[i]->uid == uid) {
            game->players[i] = game->players[--game->player_count];
            return;
        }
    }
}

// the last chat line sent to uid ("" none)
static const char *chat_to(const int uid) {
    static char text[NET_CHAT_TEXT_SIZE];
    text[0] = 0;
    for (int i = 0; i < g_sent_count; i++) {
        if (g_sent[i].uid != uid || g_sent[i].type != NET_PACKET_CHAT) continue;
        NetChatPacket packet;
        memcpy(&packet, g_sent[i].payload, sizeof(packet));
        snprintf(text, sizeof(text), "%s", packet.text);
    }
    return text;
}

// "vf" packets (subtype 0x0902) that reached uid: the count with the given active flag
static int vf_count(const int uid, const int active) {
    int n = 0;
    for (int i = 0; i < g_sent_count; i++) {
        if (g_sent[i].type != NET_PACKET_EXT || g_sent[i].uid != uid) continue;
        if (g_sent[i].length < NET_EXT_HEADER_SIZE + sizeof(NetVoteForfeitPacket)) continue;
        uint16_t sub = (uint16_t) (g_sent[i].payload[0] | (g_sent[i].payload[1] << 8));
        if (sub != NET_EXT_VOTE_FORFEIT) continue;
        const NetVoteForfeitPacket *p = (const NetVoteForfeitPacket *) (g_sent[i].payload + NET_EXT_HEADER_SIZE);
        if (p->active == active) n++;
    }
    return n;
}

static void test_forfeit(void) {
    printf("ranked forfeit vote...\n");
    static Game game;
    open_game(&game);
    Player *p1 = add_player(&game, 1, 1), *p2 = add_player(&game, 2, 1);
    Player *p3 = add_player(&game, 3, 2), *p4 = add_player(&game, 4, 2);
    Player *spec = add_player(&game, 5, 0);
    (void) p1; (void) p2; (void) p3; (void) p4;

    // gates: a spectator, a warmup, and an unranked game
    forfeit_command(&game, spec);
    CHECK(strstr(chat_to(5), "Must play") != NULL, "a spectator is refused (\"%s\")", chat_to(5));
    game_set_state(&game, GAME_STATE_WARMUP);
    forfeit_command(&game, p1);
    CHECK(strstr(chat_to(1), "Wait till match starts") != NULL && !game.forfeit.active, "warmup: wait (\"%s\")", chat_to(1));
    game_set_state(&game, GAME_STATE_IN_PROGRESS);
    game.is_ranked = 0;
    forfeit_command(&game, p1);
    CHECK(strstr(chat_to(1), "ranked") != NULL && !game.forfeit.active, "unranked: refused");
    game.is_ranked = 1;

    // a vote opens for team 1 (req 2: the initiator and its team mate); only team 1 hears it
    g_sent_count = 0;
    forfeit_command(&game, p1);
    CHECK(game.forfeit.active && game.forfeit.team == 1 && game.forfeit.voter_count == 1 && game.forfeit.req == 2,
          "start: active, team 1, 1 of 2");
    CHECK(vf_count(1, 1) == 1 && vf_count(2, 1) == 1, "vf opened to both team 1 players");
    CHECK(vf_count(3, 1) == 0 && vf_count(4, 1) == 0 && vf_count(5, 1) == 0, "vf is team only");
    forfeit_command(&game, p1);
    CHECK(strstr(chat_to(1), "already voted") != NULL, "a second vote from the same player is refused");
    forfeit_command(&game, p3);
    CHECK(strstr(chat_to(3), "other team") != NULL && game.forfeit.voter_count == 1, "the other team cannot vote now");
    forfeit_tick(&game, 1.0f);
    CHECK(game.forfeit.active, "one of two: still running");

    // the second team 1 vote is unanimous: the team forfeits, team 2 wins the round
    forfeit_command(&game, p2);
    CHECK(!game.forfeit.active && game.forfeit.forced_team == 2, "unanimous: the vote passes, team 2 forced");
    CHECK(game.end_timer > 0.0f, "the round is over (end screen running)");
    CHECK(mode_pick_winner(&game).team == 2, "mode_pick_winner gives the round to team 2");
    CHECK(strstr(chat_to(1), "Forfeiting Game") != NULL && strstr(chat_to(3), "Other Team Forfeited") != NULL,
          "team 1 hears \"Forfeiting Game\", team 2 \"Other Team Forfeited\"");
    CHECK(vf_count(1, 0) >= 1 && vf_count(2, 0) >= 1, "vf null to team 1 when it ends");

    // one start per player per game: p1 started one already this game (the round is over: back in play for the test)
    game_set_state(&game, GAME_STATE_IN_PROGRESS);
    forfeit_command(&game, p1);
    CHECK(strstr(chat_to(1), "one vote per game") != NULL, "a second start by p1 is refused");

    // a new round: no vote, no starts. p3 starts team 2; it expires after 60 s
    game.end_timer = 0.0f;
    forfeit_round_start(&game);
    game_set_state(&game, GAME_STATE_IN_PROGRESS);
    CHECK(game.forfeit.forced_team == 0 && game.forfeit.start_count == 0, "round start clears the forfeit state");
    forfeit_command(&game, p3);
    CHECK(game.forfeit.active && game.forfeit.team == 2 && game.forfeit.timer == FORFEIT_TIMER_S, "p3 opens team 2's vote");
    forfeit_tick(&game, 59.0f);
    CHECK(game.forfeit.active, "59 s: still open");
    forfeit_tick(&game, 2.0f);
    CHECK(!game.forfeit.active, "61 s: expired");
    CHECK(vf_count(-1, 0) >= 1, "expiry: \"vf\" null to everyone (NET_TARGET_ALL)");
    forfeit_command(&game, p3);
    CHECK(strstr(chat_to(3), "one vote per game") != NULL && !game.forfeit.active, "after expiry p3 cannot start again");

    // a leaver's grace: p4 starts team 2 (req 2 with p3); p3 leaves; it counts for 30 s, then not
    forfeit_round_start(&game);
    game_set_state(&game, GAME_STATE_IN_PROGRESS);
    game.now = 100.0f;
    forfeit_command(&game, p4);
    CHECK(game.forfeit.active && game.forfeit.req == 2, "p4 opens team 2 (req 2)");
    forfeit_player_left(&game, p3);
    remove_player(&game, 3);
    game.now = 110.0f;
    forfeit_tick(&game, 1.0f);
    CHECK(game.forfeit.active && game.forfeit.req == 2 && game.forfeit.voter_count == 1,
          "p3 gone 10 s: still eligible (req 2), one vote of two");
    game.now = 131.0f;
    forfeit_tick(&game, 1.0f);
    CHECK(!game.forfeit.active && game.forfeit.forced_team == 1, "p3 gone 31 s: req 1, p4 alone passes (team 1 forced)");

    close_game(&game);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    cJSON *sandstorm = NULL;
    if (load_json_file("assets/maps/311_sandstorm.json", &sandstorm) && load_json_file("../assets/maps/311_sandstorm.json", &sandstorm)) {
        printf("Failed to load assets/maps/311_sandstorm.json (run from the repository root)\n");
        return 1;
    }
    g_test_maps[0] = sandstorm;

    test_forfeit();

    if (g_failures) {
        printf("forfeit_test: %d of %d checks failed\n", g_failures, g_checks);
        return 1;
    }
    printf("forfeit_test: %d checks OK\n", g_checks);
    return 0;
}
