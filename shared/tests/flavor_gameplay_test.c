// Krunker Strike product-flavor gameplay (doc/v10/krunker-strike.md; shared/include/flavor.h,
// flavor_rules.h, strike_rules.h): under FLAVOR_STRIKE the game plays FRVR's Strike tables (class
// health / speed / regen / movement, weapon stats, mode limits, lobby defaults, the class gate)
// through the same engine paths (game_configure, player_spawn, player_change_health,
// game_tick_regen), while FLAVOR_BASE stays the base game byte-for-byte. Self-contained (no cwd
// dependency); strike_rules_test checks the tables themselves against strike_data.json.
// Run: ./<build>/flavor_gameplay_test
#include <shared.h>
#include <flavor.h>
#include <flavor_rules.h>
#include <game_config.h>
#include <ruleset.h>
#include <strike_rules.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_failures, g_checks;

#define CHECK(cond, ...) do { g_checks++; \
    if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_failures++; } \
} while (0)

static int near(const double a, const double b) { return fabs(a - b) <= 1e-5 * fmax(1.0, fabs(b)); }

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

// Heal from 50 HP by ticking the game for `seconds` of passive regen; returns the HP gained.
static float regen_from_50(Game *game, Player *player, float *now, const float seconds) {
    player->active = 1;
    player->health = 50.0f;
    player->hit_timer = 0.0f;
    player->chall_mode = 0;
    const int frames = (int) (seconds * 60.0f + 0.5f);
    for (int i = 0; i < frames; i++) game_tick(game, *now += 1.0f / 60.0f, 1.0f / 60.0f);
    return player->health - 50.0f;
}

// What one flavor plays: a game configured under it, three players (class 0, 3, 4) spawned.
typedef struct {
    int max_health[3], class_index[3];
    float damage, regen_gain, ground_accel, speed;
    int game_time, no_streaks, select_team, max_players;
    float strafe;
    int ffa_limit, gun_order, map_count;
    unsigned int sniper_ammo;
    float sniper_damage;
} Played;

static Played play(const GameFlavor flavor) {
    flavor_set(flavor);
    Played out = {0};
    cJSON *json = cJSON_Parse(FLAVOR_MAP);
    const cJSON **maps = calloc(1, sizeof(cJSON *)); // game_configure owns (frees) the list
    maps[0] = json;
    Game game = {0};
    game_configure(&game, NULL, maps, 1, NULL, 0, NULL, 0, NULL, 0);
    game_init(&game, 0, -1, 1);
    game.hooks.send = noop_send;
    game.lag_comp = 0;

    Player *p0 = add_player(&game, 1, 0); // Triggerman
    Player *p3 = add_player(&game, 2, 3); // Spray N Pray: not a Strike class
    Player *p4 = add_player(&game, 3, 4); // Vince / Strike's Shotgunner
    const Player *ps[3] = {p0, p3, p4};
    for (int i = 0; i < 3; i++) {
        out.max_health[i] = ps[i]->max_health;
        out.class_index[i] = ps[i]->class_index;
    }
    out.damage = deal(p0, p4, 40.0f);
    float now = game.now;
    out.regen_gain = regen_from_50(&game, p0, &now, 10.0f);
    out.ground_accel = game.classes[0].movement.ground_accel;
    out.speed = game.classes[0].speed;
    out.game_time = game.config.game_time;
    out.no_streaks = game.config.no_streaks;
    out.select_team = game.config.select_team;
    out.max_players = game.config.max_players;
    out.strafe = game.config.strafe_speed;
    out.ffa_limit = mode_config(MODE_ID_FFA)->score_limit;
    out.gun_order = mode_config(MODE_ID_GUN)->weapon_order_size;
    out.sniper_ammo = game.weapons[0]->ammo;
    out.sniper_damage = game.weapons[0]->damage;

    free(game.weapons);
    free(game.classes);
    free(game.modes);
    free(game.maps);
    cJSON_Delete(json);
    return out;
}

int main(void) {
    printf("flavor table...\n");
    CHECK(flavor_config(FLAVOR_BASE)->rules == NULL, "FLAVOR_BASE has no rules (Krunker v10.0.2's own)");
    CHECK(flavor_config(FLAVOR_STRIKE)->rules == strike_rules(), "FLAVOR_STRIKE plays strike_rules()");

    printf("ruleset per flavor...\n");
    flavor_set(FLAVOR_BASE);
    CHECK(ruleset_current() == ruleset_v10(), "base: ruleset_current() is v10's");
    CHECK(rules_weapon(WEAPON_ID_BOULDER) != NULL && rules_krunker_weapon_count() == WEAPON_V10_COUNT, "base: the Boulder, 31 weapons");
    CHECK(flavor_class_pick(3) == 3 && flavor_class_allowed(3) && flavor_mode_allowed(MODE_ID_TRADE), "base: every class and mode");
    CHECK(flavor_default_game_config() == &g_default_game_config && flavor_max_players() == 0, "base: Krunker's lobby defaults");
    const int *rot = NULL;
    CHECK(flavor_rotation_modes(FLAVOR_BASE, &rot) == sizeof(g_rotation_modes) / sizeof(g_rotation_modes[0]) && rot == g_rotation_modes,
          "base: Krunker's public mode rotation");
    float def = 0.0f;
    CHECK(!flavor_config_default("gameTime", &def), "base: no option default override");

    flavor_set(FLAVOR_STRIKE);
    CHECK(ruleset_current() == strike_ruleset() && ruleset_current() != ruleset_v10(), "strike: ruleset_current() is Strike's");
    CHECK(!strcmp(ruleset_current()->key, "v10"), "strike: same ruleset key (the wire stays v10's)");
    CHECK(rules_weapon(WEAPON_ID_BOULDER) == NULL && rules_weapon(WEAPON_ID_COMPRESSOR) == NULL && rules_krunker_weapon_count() == 29,
          "strike: 29 weapons, no Boulder / Compressor");
    CHECK(rules_weapon(WEAPON_ID_TOOL_GUN) != NULL, "strike: KrunkNative's custom-only tools stay");
    CHECK(!strcmp(rules_classes()[4].name, "Shotgunner") && rules_classes()[3].hide && !rules_classes()[0].hide, "strike: Shotgunner, Spray N Pray hidden");
    CHECK(flavor_class_pick(3) == 0 && flavor_class_pick(7) == 7 && !flavor_class_allowed(6) && flavor_class_allowed(5), "strike: the class gate");
    CHECK(!flavor_mode_allowed(MODE_ID_TRADE) && !flavor_mode_allowed(MODE_ID_IMP) && flavor_mode_allowed(MODE_ID_ZOM), "strike: no Trade / Impulsed");
    CHECK(!mode_hostable(MODE_ID_TRADE), "strike: Trade is not hostable");
    CHECK(flavor_rotation_modes(FLAVOR_STRIKE, &rot) == 6 && rot[0] == MODE_ID_FFA && rot[5] == MODE_ID_DOM, "strike: the six offered modes");
    CHECK(flavor_config_default("gameTime", &def) && def == 3.0f, "strike: gameTime default 3");
    CHECK(flavor_max_players() == 10, "strike: lobby cap 10");

    printf("gameplay per flavor (game_configure, player_spawn, damage, regen)...\n");
    const Played base = play(FLAVOR_BASE);
    const Played strike = play(FLAVOR_STRIKE);
    const Played base_again = play(FLAVOR_BASE);

    CHECK(base.max_health[0] == 100 && base.max_health[1] == 170 && base.max_health[2] == 90, "base: Triggerman 100, Spray N Pray 170, Vince 90 (got %d %d %d)",
          base.max_health[0], base.max_health[1], base.max_health[2]);
    CHECK(base.class_index[1] == 3, "base: Spray N Pray is played");
    CHECK(strike.max_health[0] == 180 && strike.max_health[2] == 160, "strike: Triggerman 180, Shotgunner 160 (got %d %d)", strike.max_health[0], strike.max_health[2]);
    CHECK(strike.class_index[1] == 0 && strike.max_health[1] == 180, "strike: a Spray N Pray pick plays Triggerman (class %d)", strike.class_index[1]);

    CHECK(base.damage == 40.0f && strike.damage == 40.0f, "a 40 hit does 40 under both (no invented multiplier; got %.2f / %.2f)", base.damage, strike.damage);
    // passive regen: max health x class regen per regen tick; Strike 180 x 0.15 against 100 x 0.1
    CHECK(base.regen_gain > 0.0f && strike.regen_gain > base.regen_gain, "strike regenerates (and faster): %.2f > %.2f", strike.regen_gain, base.regen_gain);

    CHECK(base.ground_accel == 0.0f && near(strike.ground_accel, 0.048), "grA: base 0 (= 0.042), strike 0.048 (got %g)", (double) strike.ground_accel);
    CHECK(near(base.speed, 1.05) && near(strike.speed, 1.0), "Triggerman speed 1.05 / 1.0");
    CHECK(base.game_time == 4 && strike.game_time == 3, "match minutes 4 / 3");
    CHECK(near(base.strafe, 1.2) && near(strike.strafe, 1.0), "strafe speed 1.2 / 1.0");
    CHECK(!base.no_streaks && strike.no_streaks && !base.select_team && strike.select_team, "streaks / team select");
    CHECK(base.max_players == 8 && strike.max_players == 10, "lobby size 8 / 10");
    CHECK(base.ffa_limit == 0 && strike.ffa_limit == 1500, "FFA score limit none / 1500");
    CHECK(base.gun_order == 21 && strike.gun_order == 20, "Gun Game order 21 / 20 (no Charge Rifle)");
    CHECK(base.sniper_damage == 100.0f && strike.sniper_damage == 165.0f, "Sniper damage 100 / 165");
    CHECK(base.sniper_ammo == strike.sniper_ammo, "Sniper ammo unchanged");

    CHECK(!memcmp(&base, &base_again, sizeof(base)), "base after a Strike run plays exactly as before");

    flavor_set(FLAVOR_BASE); // leave the process neutral
    if (g_failures) {
        printf("flavor_gameplay_test: %d of %d checks failed\n", g_failures, g_checks);
        return 1;
    }
    printf("flavor_gameplay_test: %d checks OK\n", g_checks);
    return 0;
}
