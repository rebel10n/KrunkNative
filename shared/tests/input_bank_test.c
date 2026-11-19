// v10.0.7 inputBank (shared/src/player.c player_input_bank_tick / player_input_bank_spend): the
// authority simulates at most the input time a player could have sent. Honest frames (one per tick,
// at 60 and 144 fps, raw and whole-ms deltas) move exactly as without the bank; a lag switch (frames
// held back, then sent in one burst) moves at most PLAYER_INPUT_BANK_MAX of them; a stalled server tick
// (a "server hiccup") still simulates every frame queued during it; a spawn empties the bank.
// Run from the repository root: ./<build>/input_bank_test
#include <shared.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_failures;

#define CHECK(cond, ...) do { \
    if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_failures++; } \
} while (0)

static void test_send(Game *game, const int uid, const int type, const void *payload, const uint16_t length) {
    (void) game; (void) uid; (void) type; (void) payload; (void) length;
}

static Game g_game;
static Player *g_player;
static int g_seq;
static float g_now;

static void reset_player(void) {
    player_spawn(g_player);
    g_player->position = (vec3) {0.0f, 1000.0f, 0.0f};
    g_player->velocity = (vec3) {0.0f, 0.0f, 0.0f};
    g_player->direction = (vec2) {0.0f, 0.0f};
    g_player->noclip = 1; // flies straight: no ground, no gravity, the distance is the input time
    g_seq = 0;
}

// One frame of walking forward; received > 0 marks a network frame (the bank applies to those only).
static void queue(const float delta, const int networked) {
    Input in;
    memset(&in, 0, sizeof(in));
    in.seq = g_seq++;
    in.delta = delta;
    in.move_dir = 0;
    in.received = networked ? g_game.state_now + 1e-3 : 0.0;
    in.client_time = -1.0;
    CHECK(player_accept_input(g_player, &in), "frame %d accepted", in.seq);
}

static void tick(const float delta) {
    g_now += delta;
    game_tick(&g_game, g_now, delta);
}

static float travelled(const vec3 from) {
    return hypotf(g_player->position.x - from.x, g_player->position.z - from.z);
}

// `seconds` of one frame per tick at `fps`; deltas sent as `whole_ms` (the Krunker wire) or raw.
static float run_honest(const double fps, const double seconds, const int whole_ms, const int networked) {
    reset_player();
    tick(1.0f / 60.0f); // the first refill of the life
    const vec3 from = g_player->position;
    const float dt = (float) (1.0 / fps);
    for (int i = 0; i < (int) (seconds * fps); i++) {
        queue(whole_ms ? roundf(dt * 1000.0f) / 1000.0f : dt, networked);
        tick(dt);
    }
    return travelled(from);
}

static void test_honest(void) {
    const struct { double fps; int whole_ms; } runs[] = {{60.0, 0}, {60.0, 1}, {144.0, 0}, {144.0, 1}, {240.0, 1}, {30.0, 1}};
    for (size_t r = 0; r < sizeof(runs) / sizeof(runs[0]); r++) {
        const float banked = run_honest(runs[r].fps, 5.0, runs[r].whole_ms, 1);
        const float free_run = run_honest(runs[r].fps, 5.0, runs[r].whole_ms, 0);
        CHECK(banked > 1.0f && fabsf(banked - free_run) < 1e-3f, "%.0f fps%s: honest frames move %.4f, unbanked %.4f",
              runs[r].fps, runs[r].whole_ms ? " (whole ms)" : "", banked, free_run);
    }
}

// 1 s of honest play, `hold` s with no frames while the server ticks, then the held frames at once.
static float run_lag_switch(const double hold, const int networked) {
    reset_player();
    const float dt = 1.0f / 60.0f;
    tick(dt);
    for (int i = 0; i < 60; i++) {
        queue(dt, networked);
        tick(dt);
    }
    for (int i = 0; i < (int) (hold * 60.0); i++) tick(dt);
    const vec3 from = g_player->position;
    for (int i = 0; i < (int) (hold * 60.0); i++) queue(dt, networked);
    tick(dt);
    return travelled(from);
}

static void test_lag_switch(void) {
    const float burst = run_lag_switch(2.0, 1);
    const float unbanked = run_lag_switch(2.0, 0); // the whole 2 s at full speed, in one tick
    // the bank holds PLAYER_INPUT_BANK_MAX (0.5 s) + one tick's refill (1.5 / 60 s) of the 2 s
    CHECK(burst < 0.30f * unbanked && burst > 0.22f * unbanked, "the 2 s burst moves ~0.525 s of it: %.3f of %.3f", burst, unbanked);
    const float short_hold = run_lag_switch(0.3, 1);
    const float short_free = run_lag_switch(0.3, 0);
    CHECK(fabsf(short_hold - short_free) < 1e-3f, "a 0.3 s stall loses nothing: %.4f vs %.4f", short_hold, short_free);
}

// The server's own tick stalls 2 s while the client keeps sending: every frame still runs.
static void test_server_hiccup(void) {
    float moved[2];
    for (int networked = 0; networked < 2; networked++) {
        reset_player();
        const float dt = 1.0f / 60.0f;
        tick(dt);
        for (int i = 0; i < 30; i++) {
            queue(dt, networked);
            tick(dt);
        }
        const vec3 from = g_player->position;
        for (int i = 0; i < 120; i++) queue(dt, networked);
        tick(2.0f); // one long tick: the hiccup
        moved[networked] = travelled(from);
    }
    CHECK(moved[1] > 1.0f && fabsf(moved[1] - moved[0]) < 1e-3f, "a stalled server tick simulates every frame: %.4f vs %.4f", moved[1], moved[0]);
}

static void test_spawn_resets(void) {
    reset_player();
    for (int i = 0; i < 30; i++) tick(1.0f / 60.0f);
    CHECK(g_player->input_bank > 0.0 && g_player->input_bank_at > 0.0, "the bank fills while alive (%.3f)", g_player->input_bank);
    player_spawn(g_player);
    CHECK(g_player->input_bank == 0.0 && g_player->input_bank_at == 0.0, "a spawn empties the bank (inputBankAt = 0)");
}

int main(void) {
    cJSON *sandstorm = NULL;
    if (load_json_file("assets/maps/311_sandstorm.json", &sandstorm) && load_json_file("../assets/maps/311_sandstorm.json", &sandstorm)) {
        printf("Failed to load assets/maps/311_sandstorm.json (run from the repository root)\n");
        return 1;
    }
    static const cJSON *maps[1];
    maps[0] = sandstorm;
    game_configure(&g_game, NULL, maps, 1, NULL, 0, NULL, 0, NULL, 0);
    game_init(&g_game, 0, -1, 1); // is_local: the authority
    g_game.hooks.send = test_send;
    if (!g_game.ready) {
        printf("Failed to init the game\n");
        return 1;
    }
    g_player = player_init(&g_game);
    g_player->uid = 1;
    snprintf(g_player->name, sizeof(g_player->name), "p1");
    game_players_add(&g_game, g_player);

    test_honest();
    test_lag_switch();
    test_server_hiccup();
    test_spawn_resets();

    if (g_failures) {
        printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("Test OK\n");
    return 0;
}
