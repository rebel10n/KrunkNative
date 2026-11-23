// The Black Market table and store.checkReq (client/src/ui/economy_client.c, economy_black_market_table.c generated
// from assets/skins.json): the v10.0.7 items and each requirement kind (deob-10.0.7.js:103566-103580).
#include <economy_client.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

static int failures = 0, checks = 0;
#define CHECK(cond, ...)                                \
    do {                                                \
        checks++;                                       \
        if (!(cond)) {                                  \
            failures++;                                 \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                        \
            printf("\n");                               \
        }                                               \
    } while (0)

static double now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double) tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

static const int *g_owned;
static int g_owned_n;
static int owns(const int skin, void *user) {
    (void) user;
    for (int i = 0; i < g_owned_n; i++)
        if (g_owned[i] == skin) return 1;
    return 0;
}

int main(void) {
    int n = 0;
    const EconBlackMarketItem *items = econ_black_market(&n);
    CHECK(n == 38, "v10.0.7's store has 38 blackM items (%d)", n);
    int listed = 0;
    for (int i = 0; i < n; i++) listed += !items[i].hide && !items[i].free_;
    CHECK(listed == 36, "the tab lists the 36 that are neither hide nor free (%d)", listed);

    const EconBlackMarketItem *puppet = econ_black_market_find(1641), *cape = econ_black_market_find(1483),
                              *nuke = econ_black_market_find(1642), *trader = econ_black_market_find(2996),
                              *kpd = econ_black_market_find(2998), *played = econ_black_market_find(3852),
                              *blade = econ_black_market_find(4531), *tamer = econ_black_market_find(7618);
    CHECK(puppet && puppet->hide && puppet->req == ECON_REQ_NEVER, "Puppet Master: hidden, req disabled");
    CHECK(cape && cape->req == ECON_REQ_ACC_AGE && cape->amount == 31536000000.0 && cape->funds == 100, "1 Year Cape: accAge 1 y, 100 KR");
    CHECK(nuke && nuke->req == ECON_REQ_STATS && !strcmp(nuke->var, "n") && nuke->amount == 1000, "Nuke Tamer: stats n >= 1000 (\"1000\")");
    CHECK(trader && trader->req == ECON_REQ_ACC_VAR && !strcmp(trader->var, "trades") && trader->amount == 5000, "Master Trader: accVar trades");
    CHECK(blade && blade->req == ECON_REQ_RANK && blade->amount == 4700 && !strcmp(blade->req_t, "Reach Kracked rank"), "Emperor's Blade: rank 4700");
    CHECK(tamer && tamer->req == ECON_REQ_OWN_SKINS && tamer->skin_count == 6 && tamer->skins[0] == 7722, "Ranked Tamer: own 6 charms");
    CHECK(!econ_black_market_find(1), "a store item that is not blackM");

    cJSON *stats = cJSON_Parse("{\"n\": 999}");
    EconReqAccount a = {.create_ms = now_ms() - 400.0 * 86400000.0, .funds = 10, .trades = 4999, .job_rating_pos = 150,
                        .time_played = 2592000000.0, .peak_mmr = 4699, .stats = stats, .owns = owns};
    CHECK(econ_black_market_req_met(cape, &a), "accAge: 400 days >= 1 year");
    a.create_ms = now_ms() - 300.0 * 86400000.0;
    CHECK(!econ_black_market_req_met(cape, &a), "accAge: 300 days < 1 year");
    a.create_ms = 0;
    CHECK(!econ_black_market_req_met(cape, &a), "accAge: an unknown creation date is unmet");
    CHECK(!econ_black_market_req_met(nuke, &a), "stats: 999 < 1000");
    cJSON_SetNumberValue(cJSON_GetObjectItem(stats, "n"), 1000);
    CHECK(econ_black_market_req_met(nuke, &a), "stats: 1000 >= 1000");
    CHECK(!econ_black_market_req_met(trader, &a), "accVar trades: 4999 < 5000");
    a.trades = 5000;
    CHECK(econ_black_market_req_met(trader, &a), "accVar trades: 5000");
    CHECK(econ_black_market_req_met(kpd, &a), "accVar jobRatingPos: 150 >= 150");
    CHECK(econ_black_market_req_met(played, &a), "accVar timePlayed: 720 h");
    CHECK(!econ_black_market_req_met(blade, &a), "rank: peak 4699 < 4700");
    a.peak_mmr = 4700;
    CHECK(econ_black_market_req_met(blade, &a), "rank: peak 4700");
    static const int some[] = {7722, 7723, 7724, 7725, 7726}, all[] = {7722, 7723, 7724, 7725, 7726, 7727};
    g_owned = some, g_owned_n = 5;
    CHECK(!econ_black_market_req_met(tamer, &a), "ownSkins: 5 of 6");
    g_owned = all, g_owned_n = 6;
    CHECK(econ_black_market_req_met(tamer, &a), "ownSkins: all 6");
    CHECK(!econ_black_market_req_met(puppet, &a), "disabled: never met");
    CHECK(!econ_black_market_req_met(cape, NULL), "no account: a req is unmet");
    cJSON_Delete(stats);

    printf("%d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
