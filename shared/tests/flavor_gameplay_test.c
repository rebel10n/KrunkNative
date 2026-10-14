// Krunker Strike product-flavor gameplay deltas (doc/v10/krunker-strike.md; shared/include/flavor.h,
// shared/include/flavor_rules.h): the Strike flavor hits harder (damage x1.25) and disables passive
// health regen, while FLAVOR_BASE stays the base game byte-for-byte. This test proves base vs Strike
// differ on both the damage path (player_change_health) and the regen path (game_tick_regen), and
// that under FLAVOR_BASE the flavor helpers and the measured numbers are unchanged no-ops.
// Run from the repository root: ./<build>/flavor_gameplay_test
#include <shared.h>
#include <flavor.h>
#include <flavor_rules.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_failures, g_checks;

#define CHECK(cond, ...) do { g_checks++; \
    if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_failures++; } \
} while (0)

static void noop_send(Game *game, int uid, int type, const void *payload, uint16_t length) {
    (void) game; (void) uid; (void) type; (void) payload; (void) length;
}

// A self-contained map (no cwd dependency): a floor and one spawn, enough for player_spawn.
static const char *const FLAVOR_MAP =
    "{\"name\": \"flavor\", \"spawns\": [[0, 0, 0]], \"objects\": [{\"p\": [0, -10, 0], \"s\": [400, 10, 400]}]}";

static Player *add_player(Game *game, const int uid, const int class_index) {
    Player *player = player_init(game);
    player->uid = uid;
    player->class_index = class_index;
    snprintf(player->name, sizeof(player->name), "p%d", uid);
    game_players_add(game, player);
    player_spawn(player);
    return player;
}

// One dealer-dealt hit of `dmg` to a full-health victim; returns the health lost.
static float deal(Player *dealer, Player *victim, const float dmg) {
    victim->active = 1;
    victim->health = (float) victim->max_health;
    victim->hit_timer = 0.0f;
    const float before = victim->health;
    player_change_health(victim, dealer, dmg, 0);
    return before - victim->health;
}

// Heal from 50 HP by ticking the game for `seconds` of passive regen; returns the final health.
static float regen_from_50(Game *game, Player *player, float *now, const float seconds) {
    player->active = 1;
    player->health = 50.0f;
    player->hit_timer = 0.0f;
    player->chall_mode = 0;
    const int frames = (int) (seconds * 60.0f + 0.5f);
    for (int i = 0; i < frames; i++) game_tick(game, *now += 1.0f / 60.0f, 1.0f / 60.0f);
    return player->health;
}

int main(void) {
    // ---- the table and the helpers (no Game needed) --------------------------------------------
    printf("flavor table + helpers...\n");
    CHECK(flavor_config(FLAVOR_BASE)->damage_mult == 1.0f && flavor_config(FLAVOR_BASE)->regen_mult == 1.0f,
          "FLAVOR_BASE is neutral (damage 1.0, regen 1.0)");
    CHECK(flavor_config(FLAVOR_STRIKE)->damage_mult == 1.25f && flavor_config(FLAVOR_STRIKE)->regen_mult == 0.0f,
          "FLAVOR_STRIKE: damage 1.25, regen 0.0");

    flavor_set(FLAVOR_BASE);
    // Under base the helpers must be exact no-ops (byte-identical base maths): x == scale(x).
    CHECK(flavor_scale_damage(37.3f) == 37.3f && flavor_scale_damage(100.0f) == 100.0f, "base: damage helper is an exact no-op");
    CHECK(flavor_scale_regen(0.021f) == 0.021f && flavor_scale_regen(0.0f) == 0.0f, "base: regen helper is an exact no-op");
    flavor_set(FLAVOR_STRIKE);
    CHECK(flavor_scale_damage(40.0f) == 50.0f, "strike: 40 x 1.25 = 50");
    CHECK(flavor_scale_damage(-10.0f) == -10.0f, "strike: healing (<= 0) is never scaled");
    CHECK(flavor_scale_regen(0.02f) == 0.0f, "strike: regen zeroed");

    // ---- the wired damage + regen paths --------------------------------------------------------
    cJSON *json = cJSON_Parse(FLAVOR_MAP);
    static const cJSON *maps[1];
    maps[0] = json;
    Game game = {0};
    game_configure(&game, NULL, maps, 1, NULL, 0, NULL, 0, NULL, 0);
    game_init(&game, 0, -1, 1);
    game.hooks.send = noop_send;
    game.lag_comp = 0;

    Player *dealer = add_player(&game, 1, 0);
    Player *victim = add_player(&game, 2, 0); // team 0 (FFA): combat_can_damage allows it
    float now = game.now;

    printf("damage path (player_change_health)...\n");
    flavor_set(FLAVOR_BASE);
    const float base_dmg = deal(dealer, victim, 40.0f);
    CHECK(base_dmg == 40.0f, "base: a 40 hit does exactly 40 (byte-identical, got %.3f)", base_dmg);
    const float base_env = deal(NULL, victim, 40.0f);
    CHECK(base_env == 40.0f, "base: environmental 40 (no dealer) does 40 (got %.3f)", base_env);

    flavor_set(FLAVOR_STRIKE);
    const float strike_dmg = deal(dealer, victim, 40.0f);
    CHECK(strike_dmg == 50.0f, "strike: a 40 hit does 50 (x1.25, got %.3f)", strike_dmg);
    CHECK(strike_dmg > base_dmg, "strike deals more than base for the same hit (%.1f > %.1f)", strike_dmg, base_dmg);
    const float strike_env = deal(NULL, victim, 40.0f);
    CHECK(strike_env == 40.0f, "strike: environmental damage (no dealer) is NOT scaled (got %.3f)", strike_env);

    printf("regen path (game_tick_regen)...\n");
    CHECK(game.classes[victim->class_index].regen > 0.0f, "class 0 has passive regen to exercise");
    flavor_set(FLAVOR_BASE);
    const float base_health = regen_from_50(&game, victim, &now, 10.0f);
    CHECK(base_health > 50.0f, "base: passive regen heals above 50 (got %.1f)", base_health);

    flavor_set(FLAVOR_STRIKE);
    const float strike_health = regen_from_50(&game, victim, &now, 10.0f);
    CHECK(strike_health == 50.0f, "strike: no passive regen, stays at 50 (got %.1f)", strike_health);
    CHECK(base_health > strike_health, "base regen heals, strike does not (%.1f > %.1f)", base_health, strike_health);

    flavor_set(FLAVOR_BASE); // leave the process neutral
    free(game.weapons);
    free(game.classes);
    free(game.modes);
    cJSON_Delete(json);

    if (g_failures) {
        printf("flavor_gameplay_test: %d of %d checks failed\n", g_failures, g_checks);
        return 1;
    }
    printf("flavor_gameplay_test: %d checks OK\n", g_checks);
    return 0;
}
