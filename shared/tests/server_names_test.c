// Game ids and their short ?game= link form (shared/src/server_names.c): the short node ids of
// the edges and of community nodes, link <-> canonical matching, and the Join window / Connect
// / pasted-link parser (both id forms, %3A, '+', other parameters). Prints "Test OK".
#include <server_names.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail = 1; } } while (0)

static int short_is(const char *node, const char *region, const char *want) {
    char out[KN_SHORT_NODE_MAX];
    kn_node_short_id(node, region, out, sizeof(out));
    if (strcmp(out, want)) printf("  short(%s, %s) = %s, want %s\n", node, region ? region : "NULL", out, want);
    return !strcmp(out, want);
}

static int link_is(const char *id, const char *region, const char *want) {
    char out[160];
    kn_game_link_id(id, region, out, sizeof(out));
    if (strcmp(out, want)) printf("  link(%s, %s) = %s, want %s\n", id, region ? region : "NULL", out, want);
    return !strcmp(out, want);
}

static int parse_is(const char *text, const char *want) {
    char id[160];
    const int ok = kn_game_link_parse(text, id, sizeof(id));
    if (ok != (want != NULL) || (want && strcmp(id, want))) printf("  parse(%s) = %d '%s', want %s\n", text, ok, id, want ? want : "none");
    return ok == (want != NULL) && (!want || !strcmp(id, want));
}

int main(void) {
    // the edges (deploy/ansible: node_id = <EDGE><n>, region = the 2-letter code)
    CHECK(short_is("CHITOWN2", "CH", "CH2"));
    CHECK(short_is("CLARA1", "SV", "SV1"));
    CHECK(short_is("ANGELES12", "LA", "LA12"));
    CHECK(short_is("ASHBURN3", "VA", "VA3"));
    CHECK(short_is("chitown2", "ch", "CH2"));
    CHECK(short_is("CHITOWN", "CH", "CH"));
    // already short: kept (the doc's NY, server.cfg's default LOC, test nodes)
    CHECK(short_is("NY", "us-nj", "NY"));
    CHECK(short_is("NY2", "VA", "NY2"));
    CHECK(short_is("LOC", "local", "LOC"));
    CHECK(short_is("EDGE3", "us-nj", "EDGE3"));
    CHECK(short_is("CH2", "CH", "CH2"));
    // long community ids: the first 3 letters / digits + the instance number
    CHECK(short_is("KRUNKCLUB1", "local", "KRU1"));
    CHECK(short_is("my-server12", "eu-west", "MYS12"));
    CHECK(short_is("COMMUNITY", NULL, "COM"));
    CHECK(short_is("12345", "CH", "12345"));
    CHECK(short_is("", "CH", ""));

    // game ids -> link form; a "host:port" style id keeps its node part when already short
    CHECK(link_is("CHITOWN2:kz862", "CH", "CH2:kz862"));
    CHECK(link_is("CLARA1:abcde", "SV", "SV1:abcde"));
    CHECK(link_is("NY:aaaaa", "us-nj", "NY:aaaaa"));
    CHECK(link_is("CHITOWN2:kz862", NULL, "CHI2:kz862"));
    CHECK(link_is("CH2:kz862", "CH", "CH2:kz862")); // idempotent
    CHECK(link_is("CH2:kz862", NULL, "CH2:kz862"));
    CHECK(link_is("kz862", "CH", "kz862"));
    CHECK(link_is("", "CH", ""));

    // both forms name the canonical lobby
    CHECK(kn_game_id_matches("CHITOWN2:kz862", "CHITOWN2:kz862", "CH"));
    CHECK(kn_game_id_matches("CH2:kz862", "CHITOWN2:kz862", "CH"));
    CHECK(kn_game_id_matches("ch2:kz862", "CHITOWN2:kz862", "CH"));
    CHECK(!kn_game_id_matches("CH2:KZ862", "CHITOWN2:kz862", "CH")); // lobby ids are case-sensitive
    CHECK(!kn_game_id_matches("CH1:kz862", "CHITOWN2:kz862", "CH"));
    CHECK(!kn_game_id_matches("CH2:kz863", "CHITOWN2:kz862", "CH"));
    CHECK(!kn_game_id_matches("SV2:kz862", "CHITOWN2:kz862", "CH"));
    CHECK(!kn_game_id_matches("CH2:kz862", "CHITOWN2:kz862", NULL)); // region unknown: CHI2
    CHECK(kn_game_id_matches("KRU1:ccccc", "KRUNKCLUB1:ccccc", "local"));
    CHECK(!kn_game_id_matches("", "CHITOWN2:kz862", "CH"));
    CHECK(!kn_game_id_matches("CH2", "CHITOWN2:kz862", "CH"));

    // round trip: link form of a canonical id resolves back to it
    const char *ids[][2] = {{"CHITOWN2:kz862", "CH"}, {"ASHBURN1:a1b2c", "VA"}, {"NY:aaaaa", "us-nj"}, {"KRUNKCLUB1:x", "local"}, {"LOC:q", "local"}};
    for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        char link[160];
        kn_game_link_id(ids[i][0], ids[i][1], link, sizeof(link));
        CHECK(kn_game_id_matches(link, ids[i][0], ids[i][1]));
    }

    // Join window / Connect / pasted text (v3.1.1 f9)
    CHECK(parse_is("https://krunk.io/?game=CH2:kz862", "CH2:kz862"));
    CHECK(parse_is("https://krunk.io/?game=CHITOWN2:kz862", "CHITOWN2:kz862")); // old long links
    CHECK(parse_is("https://krunk.io/?game=CH2%3Akz862", "CH2:kz862"));
    CHECK(parse_is("http://127.0.0.1:8080/?brain=x&game=CH2:kz862&ws=y", "CH2:kz862"));
    CHECK(parse_is("krunk.io/?game=CH2:kz862#top", "CH2:kz862"));
    CHECK(parse_is("  www.krunk.io/?game=SV1:abcde \r\n", "SV1:abcde"));
    CHECK(parse_is("localhost/?game=LOC:abcde", "LOC:abcde"));
    CHECK(parse_is("https://krunk.io/?mygame=CH2:kz862", NULL));
    CHECK(parse_is("https://krunk.io/?play=Burg", NULL));
    CHECK(parse_is("CH2:kz862", "CH2:kz862"));
    CHECK(parse_is("CHITOWN2:kz862", "CHITOWN2:kz862"));
    CHECK(parse_is(" NY:aaaaa ", "NY:aaaaa"));
    CHECK(parse_is("ch2:kz862", NULL));          // a bare id names its node in capitals (not host:port)
    CHECK(parse_is("chitown-2.krunk.io:21015", NULL));
    CHECK(parse_is("127.0.0.1:21015", NULL));
    CHECK(parse_is("2CH:kz862", NULL));
    CHECK(parse_is("CH2:", NULL));
    CHECK(parse_is("CH2:kz-862", NULL));
    CHECK(parse_is("", NULL));
    CHECK(parse_is(NULL, NULL));
    // a bare query: ?game= or game= (no site)
    CHECK(parse_is("?game=CH2:kz862", "CH2:kz862"));
    CHECK(parse_is("game=CH2%3Akz862", "CH2:kz862"));
    CHECK(parse_is("?game=", NULL));
    CHECK(parse_is("?mygame=CH2:kz862", NULL));
    // krunker.io's own links: the official site (kn_game_link_is_krunker)
    CHECK(kn_game_link_is_krunker("https://krunker.io/?game=ab12:xyz"));
    CHECK(kn_game_link_is_krunker("  https://www.Krunker.io/?game=ab12:xyz  "));
    CHECK(kn_game_link_is_krunker("krunker.io/?game=ab12:xyz"));
    CHECK(kn_game_link_is_krunker("http://eu.krunker.io:8080/?game=ab12%3Axyz"));
    CHECK(!kn_game_link_is_krunker("https://krunker.io.evil.com/?game=ab12:xyz"));
    CHECK(!kn_game_link_is_krunker("https://notkrunker.io/?game=ab12:xyz"));
    CHECK(!kn_game_link_is_krunker("https://krunk.io/?game=CH2:kz862"));
    CHECK(!kn_game_link_is_krunker("CH2:kz862"));
    CHECK(!kn_game_link_is_krunker("?game=ab12:xyz"));
    CHECK(!kn_game_link_is_krunker("krunker.io"));
    CHECK(!kn_game_link_is_krunker(NULL));

    if (g_fail) return 1;
    printf("Test OK\n");
    return 0;
}
