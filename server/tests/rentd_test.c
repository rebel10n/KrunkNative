// kn_rentd's pure part (server/include/rentd.h): the agent API's answer, the map argument, the instance's
// server.cfg, the decision table and the backoff (doc/v10/rented-servers.md).
#include <rentd.h>

#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c, what)                                                                                                     \
    do {                                                                                                                   \
        if (!(c)) {                                                                                                        \
            fails++;                                                                                                       \
            printf("FAIL %s\n", what);                                                                                     \
        } else {                                                                                                           \
            printf("ok   %s\n", what);                                                                                     \
        }                                                                                                                  \
    } while (0)

int main(void) {
    RentdSlot s[4];
    const char *answer = "{\"now\":1,\"slots\":[{\"id\":1,\"port\":21024,\"name\":\"Ashburn_1\",\"region\":0,\"public_host\":\"x\","
                         "\"generation\":3,\"run\":true,\"map\":\"Arena\",\"creator\":\"alice\",\"key\":\"\",\"game_id\":7},"
                         "{\"id\":2,\"port\":21025,\"name\":\"Ashburn_2\",\"generation\":0,\"run\":false,\"map\":\"\"},"
                         "{\"id\":0,\"port\":1},{\"id\":4,\"port\":70000}]}";
    const int n = rentd_parse_slots(answer, s, 4);
    CHECK(n == 2, "two valid slots (a zero id and a bad port are dropped)");
    CHECK(s[0].id == 1 && s[0].port == 21024 && s[0].generation == 3 && s[0].run && !strcmp(s[0].creator, "alice"),
          "the first slot's fields");
    CHECK(!s[1].run, "the second slot is not rented");
    CHECK(rentd_parse_slots("[]", s, 4) == -1 && rentd_parse_slots("nope", s, 4) == -1 && rentd_parse_slots(NULL, s, 4) == -1,
          "anything but {slots: [...]} is refused");

    char map[256];
    RentdSlot g = {0};
    snprintf(g.map, sizeof g.map, "Arena");
    snprintf(g.creator, sizeof g.creator, "alice");
    CHECK(rentd_map_arg(&g, map, sizeof map) && !strcmp(map, "published:alice/Arena"), "a published map: published:<creator>/<name>");
    snprintf(g.key, sizeof g.key, "kanji");
    CHECK(rentd_map_arg(&g, map, sizeof map) && !strcmp(map, "kanji"), "a catalogue key wins");
    g.key[0] = 0;
    g.creator[0] = 0;
    CHECK(rentd_map_arg(&g, map, sizeof map) && !strcmp(map, "Arena"), "a name alone");
    snprintf(g.map, sizeof g.map, "A\"rena");
    CHECK(!rentd_map_arg(&g, map, sizeof map), "a quote is refused");
    snprintf(g.map, sizeof g.map, "a,b");
    CHECK(!rentd_map_arg(&g, map, sizeof map), "a list separator is refused");
    snprintf(g.map, sizeof g.map, "Arena");
    snprintf(g.creator, sizeof g.creator, "../x");
    CHECK(!rentd_map_arg(&g, map, sizeof map), "a creator with a slash is refused");
    g.map[0] = 0;
    g.creator[0] = 0;
    CHECK(!rentd_map_arg(&g, map, sizeof map), "no game, no map");

    RentdShared sh = {0};
    snprintf(sh.region, sizeof sh.region, "VA");
    snprintf(sh.listen_host, sizeof sh.listen_host, "172.16.5.94");
    snprintf(sh.public_host, sizeof sh.public_host, "ashburn-dedi.example");
    snprintf(sh.coordinator_url, sizeof sh.coordinator_url, "wss://c.example/mm/internal/ws");
    sh.max_players = 8;
    char cfg[8192];
    CHECK(rentd_cfg_text(&s[0], &sh, "/var/lib/kn", cfg, sizeof cfg) > 0, "the cfg is written");
    CHECK(strstr(cfg, "hostname        \"Ashburn_1\"\n") && strstr(cfg, "node_id         \"RENT1\"\n") && strstr(cfg, "port            21024\n")
              && strstr(cfg, "maps            \"published:alice/Arena\"\n") && strstr(cfg, "dedicated       1\n")
              && strstr(cfg, "public_host     \"x\"\n") && strstr(cfg, "region          \"VA\"\n"),
          "the cfg: hostname, node id, port, the game, dedicated, the slot's public host, the region");
    CHECK(rentd_cfg_text(&s[1], &sh, "/var/lib/kn", cfg, sizeof cfg) == -1, "a slot without a game has no cfg");
    s[0].public_host[0] = 0;
    CHECK(rentd_cfg_text(&s[0], &sh, "/var/lib/kn", cfg, sizeof cfg) > 0 && strstr(cfg, "public_host     \"ashburn-dedi.example\"\n"),
          "no public host on the slot: the host's");
    CHECK(rentd_cfg_text(&s[0], &sh, "/var/lib/kn", cfg, 64) == -1, "a cfg that does not fit is refused");

    RentdSlot want = s[0];
    CHECK(rentd_decide(&want, 0, 0, 10, 0) == RENTD_START, "rented, nothing runs: start");
    CHECK(rentd_decide(&want, 0, 0, 10, 20) == RENTD_KEEP, "rented, in the backoff: wait");
    CHECK(rentd_decide(&want, 1, 3, 10, 0) == RENTD_KEEP, "rented, the generation runs: keep");
    CHECK(rentd_decide(&want, 1, 2, 10, 0) == RENTD_RESTART, "rented, an older generation runs: restart");
    want.run = 0;
    CHECK(rentd_decide(&want, 1, 3, 10, 0) == RENTD_STOP, "no longer rented: stop");
    CHECK(rentd_decide(&want, 0, 3, 10, 0) == RENTD_KEEP, "not rented, nothing runs: nothing");
    CHECK(rentd_decide(NULL, 1, 3, 10, 0) == RENTD_STOP, "the slot left the host: stop");

    CHECK(rentd_backoff(1) == 1 && rentd_backoff(2) == 2 && rentd_backoff(3) == 4 && rentd_backoff(7) == 60 && rentd_backoff(50) == 60,
          "the backoff: 1, 2, 4 .. 60 s");
    printf("rentd_test: %s\n", fails ? "FAILED" : "OK");
    return fails ? 1 : 0;
}
