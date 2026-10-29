// Krunker v10.0.4 counter skins: countSkinStat on the authority (GameHooks.skin_stat, game_count_skin_stat): the jump,
// shot, hit, kill and trackStat (candy) sites call it with the stat and weapon the bundle counts (deob-10.0.4.js:174219-174221,
// :174925-174928, :175903-175908, :176019-176023, :184308-184311), and a client game (no authority) never does.
#include <shared.h>
#include <stdio.h>
#include <string.h>

void player_jump(Player *player); // shared/src/player.c (no header: the movement code calls it)

static int g_failures, g_checks;
#define CHECK(cond, ...) do { \
    g_checks++; \
    if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_failures++; } \
} while (0)

typedef struct {
    int uid, weapon, amount;
    char stat[16];
} Counted;
static Counted g_counted[64];
static int g_count;

static void skin_stat(Game *game, Player *player, const int weapon, const char *stat, const int amount) {
    (void) game;
    if (g_count >= 64) return;
    g_counted[g_count].uid = player->uid;
    g_counted[g_count].weapon = weapon;
    g_counted[g_count].amount = amount;
    snprintf(g_counted[g_count].stat, sizeof(g_counted[0].stat), "%s", stat);
    g_count++;
}

// the times `stat` was counted for `uid` (weapon -2 = any), their amounts summed
static int counted(const int uid, const char *stat, const int weapon) {
    int n = 0;
    for (int i = 0; i < g_count; i++)
        if (g_counted[i].uid == uid && !strcmp(g_counted[i].stat, stat) && (weapon == -2 || g_counted[i].weapon == weapon)) n += g_counted[i].amount;
    return n;
}

static void test_send(Game *game, const int uid, const int type, const void *payload, const uint16_t length) {
    (void) game, (void) uid, (void) type, (void) payload, (void) length;
}

static const char *k_flat_map = "{\"name\":\"Flat\",\"spawns\":[[0,0,0,0,0,0],[30,0,0,0,0,0]],\"objects\":[{\"p\":[0,-10,0],\"s\":[2000,10,2000]}]}";

static void game_on(Game *game, const cJSON *map, const int authority) {
    static const cJSON *maps[1];
    maps[0] = map;
    memset(game, 0, sizeof(*game));
    game_configure(game, NULL, maps, 1, NULL, 0, NULL, 0, NULL, 0);
    game->hooks.send = test_send;
    game->hooks.skin_stat = skin_stat;
    game_init(game, 0, 0, (unsigned char) authority);
}

static Player *add_player(Game *game, const int uid, const vec3 at) {
    Player *player = player_init(game);
    player->uid = uid;
    player->has_account = 1;
    snprintf(player->name, sizeof(player->name), "P%d", uid);
    game_players_add(game, player);
    player_spawn(player);
    player->position = at;
    player->last_position = at;
    return player;
}

static int held(const Player *p) { return p->loadout && p->loadout_index >= 0 && p->loadout_index < p->loadout_size ? p->loadout[p->loadout_index] : -1; }

int main(void) {
    cJSON *map = cJSON_Parse(k_flat_map);
    static Game game;

    printf("Running the authority's counted stats...\n");
    game_on(&game, map, 1);
    Player *a = add_player(&game, 1, (vec3) {0.0f, 0.0f, 0.0f});
    Player *b = add_player(&game, 2, (vec3) {30.0f, 0.0f, 0.0f});
    const int w = held(a);
    CHECK(w >= 0 && a->weapon, "the shooter holds a weapon (%d)", w);

    g_count = 0;
    player_jump(a);
    CHECK(counted(1, "jumps", w) == 1, "a jump counts \"jumps\" on the held weapon (%d)", counted(1, "jumps", -2));

    g_count = 0;
    player_change_health(b, a, 10.0f, 0);
    CHECK(counted(1, "hits", w) == 1 && counted(1, "headshots", -2) == 0, "a body hit counts \"hits\" only");
    player_change_health(b, a, 10.0f, 1);
    CHECK(counted(1, "hits", w) == 2 && counted(1, "headshots", w) == 1, "a crit counts \"hits\" and \"headshots\"");
    CHECK(counted(2, "hits", -2) == 0, "the victim counts nothing");

    g_count = 0;
    PlayerKillInfo info = {.weapon_id = w, .headshot = 1};
    player_kill(b, a, &info, 1);
    CHECK(counted(1, "kills", w) == 1 && counted(1, "hskills", w) == 1, "a headshot kill counts \"kills\" and \"hskills\" (%d %d)",
          counted(1, "kills", -2), counted(1, "hskills", -2));

    player_spawn(b);
    g_count = 0;
    PlayerKillInfo streak = {.weapon_id = w, .streak = 2};
    player_kill(b, a, &streak, 1);
    CHECK(counted(1, "kills", -2) == 0, "a streak kill counts nothing (countSkinKill: info.stk == null)");

    g_count = 0;
    game_count_skin_stat(&game, a, -1, "candy", 5);
    CHECK(counted(1, "candy", -1) == 5, "trackStat's candy: every counter of the stat (weapon -1), the amount");
    game_count_skin_stat(&game, a, w, "candy", 0);
    CHECK(counted(1, "candy", w) == 1, "an amount below 1 counts 1 (iíIïíïi || 1)");
    game_count_skin_stat(&game, a, w, "dances", 3);
    CHECK(counted(1, "dances", -2) == 0, "an unknown stat counts nothing");

    CHECK(skin_stat_index("kills") == 0 && skin_stat_index("candy") == 7 && skin_stat_index("x") < 0, "SKIN_STATS order");
    CHECK(skin_stat_server_only("candy") && !skin_stat_server_only("kills"), "candy is the server-only stat");

    printf("Running a client game (no authority)...\n");
    static Game client;
    game_on(&client, map, 0);
    Player *c = add_player(&client, 3, (vec3) {0.0f, 0.0f, 0.0f});
    g_count = 0;
    player_jump(c);
    game_count_skin_stat(&client, c, held(c), "kills", 1);
    CHECK(g_count == 0, "nothing is counted without the authority (%d)", g_count);

    cJSON_Delete(map);
    printf("skin_stat_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
