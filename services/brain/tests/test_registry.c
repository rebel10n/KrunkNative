/* Unit test for services/brain/registry.c (v3.1.1 pickBestLobby rules + regions). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "registry.h"

static int g_fail;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); g_fail = 1; } } while (0)

static MmNode *add_node(Registry *r, const char *id, const char *region, int custom) {
    char json[256];
    snprintf(json, sizeof(json),
             "{\"nodeId\":\"%s\",\"region\":\"%s\",\"publicHost\":\"h-%s\",\"publicPort\":21015,\"custom\":%s}", id,
             region, id, custom ? "true" : "false");
    cJSON *hello = cJSON_Parse(json);
    MmNode *n = registry_node_hello(r, id, hello, "brainA", "local", 1000);
    cJSON_Delete(hello);
    return n;
}

static void add_lobby(Registry *r, MmNode *n, const char *lid, int players, int max, int cs, const char *v) {
    char json[512];
    snprintf(json, sizeof(json), "[\"%s\",\"%s\",%d,%d,{\"i\":\"Burg\",\"v\":\"%s\",\"cs\":%s,\"modeName\":\"ffa\"}]",
             lid, n->region, players, max, v, cs ? "true" : "false");
    cJSON *t = cJSON_Parse(json);
    CHECK(registry_lobby_upsert(r, n, t) == 0);
    cJSON_Delete(t);
}

static int outdated_aaaa(const MmLobby *l, void *ud) {
    (void) ud;
    const char *c = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(l->tuple, 4), "commit"));
    return c && !strcmp(c, "aaaaaaaaaaaa");
}

int main(void) {
    Registry *r = registry_new();
    MmPolicy p = {.cross_region = 1, .default_max_players = 8};
    MmPick pick;

    MmPickQuery q0 = {.region = "us-nj", .auto_change = 1, .now_ms = 2000};
    CHECK(registry_pick(r, &p, &q0, &pick) == MM_PICK_NONE);

    MmNode *a = add_node(r, "NY", "us-nj", 0);
    MmNode *b = add_node(r, "NY2", "us-nj", 1);
    MmNode *c = add_node(r, "FRA", "de-fra", 1);
    CHECK(a && b && c);
    CHECK(!registry_valid_node_id("bad:id"));
    add_lobby(r, a, "aaaaa", 3, 8, 0, "1.0");
    add_lobby(r, b, "bbbbb", 5, 8, 0, "1.0");
    add_lobby(r, c, "ccccc", 7, 8, 0, "1.0");
    add_lobby(r, c, "custm", 1, 8, 1, "1.0");

    /* namespacing */
    MmNode *owner = NULL;
    MmLobby *l = registry_lobby(r, "NY:aaaaa", &owner);
    CHECK(l && owner == a && !strcmp(l->local_id, "aaaaa"));
    CHECK(!strcmp(cJSON_GetArrayItem(l->tuple, 0)->valuestring, "NY:aaaaa"));

    /* most populated with room in the region */
    CHECK(registry_pick(r, &p, &q0, &pick) == MM_PICK_OK && pick.lobby == registry_lobby(r, "NY2:bbbbb", NULL));
    /* region de-fra: the public one, never the custom */
    MmPickQuery qf = {.region = "de-fra", .auto_change = 1, .now_ms = 2000};
    CHECK(registry_pick(r, &p, &qf, &pick) == MM_PICK_OK && !strcmp(pick.lobby->game_id, "FRA:ccccc"));
    /* reservations count: fill FRA:ccccc */
    registry_reserve(r, "j1", "FRA:ccccc", 5000);
    CHECK(registry_pick(r, &p, &qf, &pick) == MM_PICK_OK && !strcmp(pick.lobby->game_id, "NY2:bbbbb")); /* cross region */
    p.cross_region = 0;
    CHECK(registry_pick(r, &p, &qf, &pick) == MM_PICK_NONE);
    p.cross_region = 1;
    registry_expire(r, 6000);
    MmPickQuery qf2 = {.region = "de-fra", .auto_change = 1, .now_ms = 6000};
    CHECK(registry_pick(r, &p, &qf2, &pick) == MM_PICK_OK && !strcmp(pick.lobby->game_id, "FRA:ccccc"));

    /* excludeGame (v10 Quick Match): another lobby with room first (cross region included), the
     * excluded one only when nothing else has room */
    MmPickQuery qx = {.region = "us-nj", .auto_change = 1, .now_ms = 6000, .exclude_game = "NY2:bbbbb"};
    CHECK(registry_pick(r, &p, &qx, &pick) == MM_PICK_OK && !strcmp(pick.lobby->game_id, "NY:aaaaa"));
    qx.exclude_game = "NY:aaaaa";
    CHECK(registry_pick(r, &p, &qx, &pick) == MM_PICK_OK && !strcmp(pick.lobby->game_id, "NY2:bbbbb"));
    MmPickQuery qxf = {.region = "de-fra", .auto_change = 1, .now_ms = 6000, .exclude_game = "FRA:ccccc"};
    CHECK(registry_pick(r, &p, &qxf, &pick) == MM_PICK_OK && !strcmp(pick.lobby->game_id, "NY2:bbbbb"));
    p.cross_region = 0;
    CHECK(registry_pick(r, &p, &qxf, &pick) == MM_PICK_OK && !strcmp(pick.lobby->game_id, "FRA:ccccc")); /* the only one */
    p.cross_region = 1;

    /* requested game */
    MmPickQuery qg = {.region = "us-nj", .game_id = "FRA:custm", .auto_change = 0, .now_ms = 2000};
    CHECK(registry_pick(r, &p, &qg, &pick) == MM_PICK_OK && !strcmp(pick.lobby->game_id, "FRA:custm") && !pick.changed);
    MmPickQuery qbad = {.region = "us-nj", .game_id = "NOPE:x", .auto_change = 0, .now_ms = 2000};
    CHECK(registry_pick(r, &p, &qbad, &pick) == MM_PICK_INVALID_GAME);
    qbad.auto_change = 1;
    CHECK(registry_pick(r, &p, &qbad, &pick) == MM_PICK_OK && pick.changed);
    add_lobby(r, a, "fulll", 8, 8, 0, "1.0");
    MmPickQuery qfull = {.region = "us-nj", .game_id = "NY:fulll", .auto_change = 0, .now_ms = 2000};
    CHECK(registry_pick(r, &p, &qfull, &pick) == MM_PICK_GAME_FULL);
    qfull.force = 1;
    CHECK(registry_pick(r, &p, &qfull, &pick) == MM_PICK_OK);

    /* mode caps + soft cap */
    cJSON *caps = cJSON_Parse("{\"FFA\":5}");
    p.mode_caps = caps;
    CHECK(registry_pick(r, &p, &q0, &pick) == MM_PICK_OK && !strcmp(pick.lobby->game_id, "NY:aaaaa"));
    p.mode_caps = NULL;
    cJSON_Delete(caps);
    p.soft_cap = 4;
    CHECK(registry_pick(r, &p, &q0, &pick) == MM_PICK_OK && !strcmp(pick.lobby->game_id, "NY:aaaaa"));
    p.soft_cap = 0;

    /* version filter */
    MmPickQuery qv = {.region = "us-nj", .auto_change = 1, .version = "9.9", .now_ms = 2000};
    CHECK(registry_pick(r, &p, &qv, &pick) == MM_PICK_NO_MATCH);

    /* draining nodes are skipped */
    b->draining = 1;
    CHECK(registry_pick(r, &p, &q0, &pick) == MM_PICK_OK && !strcmp(pick.lobby->game_id, "NY:aaaaa"));
    b->draining = 0;

    /* host target: custom-capable with spare capacity, own brain first */
    MmNode *h = registry_pick_host_node(r, "us-nj", "brainA", 1, 0, 2000);
    CHECK(h == NULL); /* NY2 has a lobby with players and max_lobbies 1; NY is not custom */
    registry_lobby_remove(r, b, "bbbbb");
    h = registry_pick_host_node(r, "us-nj", "brainA", 1, 0, 2000);
    CHECK(h == b);
    add_lobby(r, b, "bbbbb", 0, 8, 0, "1.0");
    CHECK(registry_pick_host_node(r, "us-nj", "brainA", 1, 0, 2000) == b); /* idle lobby */
    registry_reserve(r, "j2", "NY2:bbbbb", 3000);
    CHECK(registry_pick_host_node(r, "us-nj", "brainA", 1, 0, 2000) == NULL); /* seat in flight */
    CHECK(registry_pick_host_node(r, "us-nj", "brainA", 1, 0, 3500) == b);

    /* online_mode + password protected nodes (hello.onlineMode / hello.password) */
    {
        Registry *o = registry_new();
        MmPolicy po = {.cross_region = 1, .default_max_players = 8};
        cJSON *h1 = cJSON_Parse("{\"region\":\"us-nj\",\"publicHost\":\"chitown-1\",\"publicPort\":21015,"
                                "\"custom\":true,\"onlineMode\":1}");
        cJSON *h2 = cJSON_Parse("{\"region\":\"us-nj\",\"publicHost\":\"chitown-2\",\"publicPort\":21016,\"custom\":true}");
        cJSON *h3 = cJSON_Parse("{\"region\":\"us-nj\",\"publicHost\":\"chitown-3\",\"publicPort\":21017,"
                                "\"custom\":true,\"onlineMode\":0,\"password\":true}");
        MmNode *on = registry_node_hello(o, "CHITOWN1", h1, "brainA", "local", 1000);
        MmNode *off = registry_node_hello(o, "CHITOWN2", h2, "brainA", "local", 1000); /* old server_bin: no field */
        MmNode *pw = registry_node_hello(o, "CHITOWN3", h3, "brainA", "local", 1000);
        cJSON_Delete(h1);
        cJSON_Delete(h2);
        cJSON_Delete(h3);
        CHECK(on && on->online_mode && off && !off->online_mode && pw && pw->password);
        add_lobby(o, on, "onl", 6, 8, 0, "1.0");  /* the most populated: an authed player's pick */
        add_lobby(o, off, "off", 2, 8, 0, "1.0");
        add_lobby(o, pw, "pwd", 7, 8, 0, "1.0");  /* fuller still, but password protected */
        MmLobby *lon = registry_lobby(o, "CHITOWN1:onl", NULL);
        CHECK(lon && lon->online_mode && registry_lobby_needs_account(on, lon));
        CHECK(cJSON_IsTrue(cJSON_GetObjectItem(cJSON_GetArrayItem(lon->tuple, 4), "onlineMode"))); /* federated */

        MmPickQuery guest = {.region = "us-nj", .auto_change = 1, .now_ms = 2000};
        MmPickQuery authed = {.region = "us-nj", .auto_change = 1, .authed = 1, .now_ms = 2000};
        /* a guest is never routed to an online_mode node, whatever the population */
        for (int i = 0; i < 4; i++) {
            CHECK(registry_pick(o, &po, &guest, &pick) == MM_PICK_OK && pick.node == off);
            registry_reserve(o, i == 0 ? "g0" : i == 1 ? "g1" : i == 2 ? "g2" : "g3", "CHITOWN2:off", 9000);
        }
        CHECK(registry_pick(o, &po, &guest, &pick) == MM_PICK_OK && pick.node == off); /* 6/8 taken */
        registry_reserve(o, "g4", "CHITOWN2:off", 9000);
        registry_reserve(o, "g5", "CHITOWN2:off", 9000);
        CHECK(registry_pick(o, &po, &guest, &pick) == MM_PICK_NONE); /* off full: not into onl / pwd */
        registry_expire(o, 9500);
        /* an authed player can be: the most populated eligible lobby, never the password one */
        CHECK(registry_pick(o, &po, &authed, &pick) == MM_PICK_OK && pick.node == on && !strcmp(pick.lobby->game_id, "CHITOWN1:onl"));
        /* requested online lobby: the authed player gets it, a guest is moved or refused */
        MmPickQuery gq = {.region = "us-nj", .game_id = "CHITOWN1:onl", .auto_change = 0, .now_ms = 2000};
        CHECK(registry_pick(o, &po, &gq, &pick) == MM_PICK_ACCOUNT_REQUIRED);
        gq.auto_change = 1;
        CHECK(registry_pick(o, &po, &gq, &pick) == MM_PICK_OK && pick.node == off && pick.changed);
        gq.authed = 1;
        gq.auto_change = 0;
        CHECK(registry_pick(o, &po, &gq, &pick) == MM_PICK_OK && pick.node == on && !pick.changed);
        /* the password lobby only by id */
        MmPickQuery pq = {.region = "us-nj", .game_id = "CHITOWN3:pwd", .auto_change = 0, .now_ms = 2000};
        CHECK(registry_pick(o, &po, &pq, &pick) == MM_PICK_OK && pick.node == pw);
        /* pr in the lobby meta (sv_password set later) also protects */
        off->password = 0;
        cJSON *t = cJSON_Parse("[\"off\",\"us-nj\",2,8,{\"i\":\"Burg\",\"v\":\"1.0\",\"cs\":false,\"pr\":true}]");
        registry_lobby_upsert(o, off, t);
        cJSON_Delete(t);
        CHECK(registry_pick(o, &po, &guest, &pick) == MM_PICK_NONE);
        /* a private game (v7 host config "private") too */
        t = cJSON_Parse("[\"off\",\"us-nj\",2,8,{\"i\":\"Burg\",\"v\":\"1.0\",\"cs\":false,\"private\":true}]");
        registry_lobby_upsert(o, off, t);
        cJSON_Delete(t);
        CHECK(registry_pick(o, &po, &guest, &pick) == MM_PICK_NONE);
        add_lobby(o, off, "off", 2, 8, 0, "1.0");
        /* remote copy (federation): the flag comes from the tuple meta only */
        registry_remote_begin(o);
        MmNode *rem = registry_remote_node(o, "CLARA1");
        CHECK(rem != NULL);
        snprintf(rem->region, sizeof(rem->region), "us-nj");
        cJSON *rt = cJSON_Parse("[\"CLARA1:x\",\"us-nj\",7,8,{\"i\":\"Burg\",\"v\":\"1.0\",\"cs\":false,\"onlineMode\":true}]");
        CHECK(registry_lobby_upsert(o, rem, rt) == 0);
        cJSON_Delete(rt);
        registry_remote_end(o);
        CHECK(registry_pick(o, &po, &guest, &pick) == MM_PICK_OK && pick.node == off);
        CHECK(registry_pick(o, &po, &authed, &pick) == MM_PICK_OK && !strcmp(pick.lobby->game_id, "CLARA1:x"));
        /* hosting: never on a password node; online_mode nodes for signed-in hosts only */
        registry_lobby_remove(o, on, "onl");
        registry_lobby_remove(o, off, "off");
        registry_lobby_remove(o, pw, "pwd");
        CHECK(registry_pick_host_node(o, "us-nj", "brainA", 1, 0, 2000) == off);
        registry_node_remove(o, "CHITOWN2");
        CHECK(registry_pick_host_node(o, "us-nj", "brainA", 1, 0, 2000) == NULL);
        CHECK(registry_pick_host_node(o, "us-nj", "brainA", 1, 1, 2000) == on);
        /* an old server_bin reconnecting with the same id drops back to offline */
        cJSON *h1b = cJSON_Parse("{\"region\":\"us-nj\",\"publicHost\":\"chitown-1\",\"publicPort\":21015}");
        CHECK(registry_node_hello(o, "CHITOWN1", h1b, "brainA", "local", 3000) == on && !on->online_mode);
        cJSON_Delete(h1b);
        registry_free(o);
    }

    /* ci-release: protocol majors never cross (meta.proto, ?proto=), outdated lobbies lose ties */
    {
        Registry *v = registry_new();
        MmPolicy pv = {.cross_region = 1, .default_max_players = 8};
        MmNode *n1 = add_node(v, "EDGE1", "CH", 0);
        MmNode *n2 = add_node(v, "EDGE2", "CH", 0);
        MmNode *n3 = add_node(v, "EDGE3", "CH", 0);
        cJSON *t1 = cJSON_Parse("[\"old\",\"CH\",6,8,{\"i\":\"Burg\",\"v\":\"1.0\",\"cs\":false,\"proto\":\"1.3\",\"commit\":\"aaaaaaaaaaaa\"}]");
        cJSON *t2 = cJSON_Parse("[\"new\",\"CH\",2,8,{\"i\":\"Burg\",\"v\":\"1.0\",\"cs\":false,\"proto\":\"1.3\",\"commit\":\"bbbbbbbbbbbb\"}]");
        cJSON *t3 = cJSON_Parse("[\"two\",\"CH\",7,8,{\"i\":\"Burg\",\"v\":\"1.0\",\"cs\":false,\"proto\":\"2.0\"}]");
        CHECK(registry_lobby_upsert(v, n1, t1) == 0 && registry_lobby_upsert(v, n2, t2) == 0 && registry_lobby_upsert(v, n3, t3) == 0);
        cJSON_Delete(t1);
        cJSON_Delete(t2);
        cJSON_Delete(t3);
        CHECK(registry_lobby(v, "EDGE3:two", NULL)->proto_major == 2);
        /* a 1.x client: never the 2.0 lobby although it is the fullest */
        MmPickQuery q1 = {.region = "CH", .auto_change = 1, .now_ms = 2000, .proto_major = 1};
        CHECK(registry_pick(v, &pv, &q1, &pick) == MM_PICK_OK && !strcmp(pick.lobby->game_id, "EDGE1:old"));
        /* outdated EDGE1 (commit aaaa...) loses to the up-to-date one even with fewer players */
        q1.outdated = outdated_aaaa;
        CHECK(registry_pick(v, &pv, &q1, &pick) == MM_PICK_OK && !strcmp(pick.lobby->game_id, "EDGE2:new"));
        /* only outdated lobbies left with room: still matched (preference, not exclusion) */
        registry_lobby_remove(v, n2, "new");
        CHECK(registry_pick(v, &pv, &q1, &pick) == MM_PICK_OK && !strcmp(pick.lobby->game_id, "EDGE1:old"));
        /* asking for the 2.0 lobby by id */
        MmPickQuery qid = {.region = "CH", .game_id = "EDGE3:two", .auto_change = 0, .now_ms = 2000, .proto_major = 1};
        CHECK(registry_pick(v, &pv, &qid, &pick) == MM_PICK_INCOMPATIBLE);
        qid.auto_change = 1;
        CHECK(registry_pick(v, &pv, &qid, &pick) == MM_PICK_OK && pick.changed && !strcmp(pick.lobby->game_id, "EDGE1:old"));
        /* only incompatible lobbies: no match */
        registry_lobby_remove(v, n1, "old");
        CHECK(registry_pick(v, &pv, &q1, &pick) == MM_PICK_NO_MATCH);
        /* a 2.x client, and no client protocol (0 = any) */
        MmPickQuery q2 = {.region = "CH", .auto_change = 1, .now_ms = 2000, .proto_major = 2};
        CHECK(registry_pick(v, &pv, &q2, &pick) == MM_PICK_OK && !strcmp(pick.lobby->game_id, "EDGE3:two"));
        MmPickQuery qany = {.region = "CH", .auto_change = 1, .now_ms = 2000};
        CHECK(registry_pick(v, &pv, &qany, &pick) == MM_PICK_OK);
        registry_free(v);
    }

    /* Krunker v10.0.2 is the only version: quick join (?ruleset=v10 or none) skips a lobby that
     * announces another one (a version-toggle server on v7.1.4); one that announces none is fine;
     * by id any lobby joins */
    {
        Registry *v = registry_new();
        MmPolicy pv = {.cross_region = 1, .default_max_players = 8};
        MmNode *n1 = add_node(v, "RS1", "CH", 0);
        MmNode *n2 = add_node(v, "RS2", "CH", 0);
        cJSON *t1 = cJSON_Parse("[\"seven\",\"CH\",6,8,{\"i\":\"Burg\",\"cs\":false,\"ruleset\":\"v7\"}]");
        cJSON *t2 = cJSON_Parse("[\"ten\",\"CH\",2,8,{\"i\":\"Burg\",\"cs\":false,\"ruleset\":\"v10\"}]");
        CHECK(registry_lobby_upsert(v, n1, t1) == 0 && registry_lobby_upsert(v, n2, t2) == 0);
        cJSON_Delete(t1);
        cJSON_Delete(t2);
        CHECK(!strcmp(registry_lobby(v, "RS1:seven", NULL)->ruleset, "v7") && !strcmp(registry_lobby(v, "RS2:ten", NULL)->ruleset, "v10"));
        MmPickQuery qnone = {.region = "CH", .auto_change = 1, .now_ms = 2000};
        CHECK(registry_pick(v, &pv, &qnone, &pick) == MM_PICK_OK && !strcmp(pick.lobby->game_id, "RS2:ten")); /* no ?ruleset= = v10 */
        MmPickQuery q10 = {.region = "CH", .auto_change = 1, .now_ms = 2000, .ruleset = "v10"};
        CHECK(registry_pick(v, &pv, &q10, &pick) == MM_PICK_OK && !strcmp(pick.lobby->game_id, "RS2:ten"));
        MmPickQuery qid = {.region = "CH", .game_id = "RS1:seven", .auto_change = 0, .now_ms = 2000};
        CHECK(registry_pick(v, &pv, &qid, &pick) == MM_PICK_OK && !strcmp(pick.lobby->game_id, "RS1:seven"));
        registry_lobby_remove(v, n2, "ten");
        CHECK(registry_pick(v, &pv, &q10, &pick) == MM_PICK_NO_MATCH); /* only a v7 lobby left */
        cJSON *t3 = cJSON_Parse("[\"old\",\"CH\",1,8,{\"i\":\"Burg\",\"cs\":false}]");
        CHECK(registry_lobby_upsert(v, n2, t3) == 0);
        cJSON_Delete(t3);
        CHECK(!registry_lobby(v, "RS2:old", NULL)->ruleset[0]);
        CHECK(registry_pick(v, &pv, &q10, &pick) == MM_PICK_OK && !strcmp(pick.lobby->game_id, "RS2:old")); /* none announced: fine */
        registry_free(v);
    }

    /* short game ids (shared/src/server_names.c): ?game=CH2:kz862 is CHITOWN2:kz862 */
    {
        Registry *s = registry_new();
        MmPolicy ps = {.cross_region = 1, .default_max_players = 8};
        MmNode *ch1 = add_node(s, "CHITOWN1", "CH", 0);
        MmNode *ch2 = add_node(s, "CHITOWN2", "CH", 0);
        MmNode *sv1 = add_node(s, "CLARA1", "SV", 0);
        MmNode *ny = add_node(s, "NY", "us-nj", 1);              /* a short community node id */
        MmNode *club = add_node(s, "KRUNKCLUB1", "local", 1);    /* a long one: KRU1 */
        MmNode *bob = add_node(s, "BOBSERVER1", "CH", 1);        /* community node in an edge region: CH1 */
        CHECK(ch1 && ch2 && sv1 && ny && club && bob);
        add_lobby(s, ch1, "aaaaa", 1, 8, 0, "1.0");
        add_lobby(s, ch2, "kz862", 2, 8, 0, "1.0");
        add_lobby(s, sv1, "kz862", 3, 8, 0, "1.0");              /* same lobby id, other node */
        add_lobby(s, ny, "nnnnn", 0, 8, 1, "1.0");
        add_lobby(s, club, "ccccc", 0, 8, 1, "1.0");
        add_lobby(s, bob, "bbbbb", 0, 8, 1, "1.0");
        MmNode *owner = NULL;
        /* canonical ids still resolve (old links) */
        CHECK(registry_lobby_find(s, "CHITOWN2:kz862", &owner) && owner == ch2);
        /* short form: region code + instance number; the lobby id picks between nodes */
        owner = NULL;
        MmLobby *sl = registry_lobby_find(s, "CH2:kz862", &owner);
        CHECK(sl && owner == ch2 && !strcmp(sl->game_id, "CHITOWN2:kz862"));
        CHECK(registry_lobby_find(s, "SV1:kz862", &owner) && owner == sv1);
        CHECK(registry_lobby_find(s, "ch2:kz862", &owner) && owner == ch2); /* typed lowercase */
        CHECK(registry_lobby_find(s, "CH1:aaaaa", &owner) && owner == ch1);
        CHECK(registry_lobby_find(s, "CH1:bbbbb", &owner) && owner == bob);
        CHECK(registry_lobby_find(s, "NY:nnnnn", &owner) && owner == ny);
        CHECK(registry_lobby_find(s, "KRU1:ccccc", &owner) && owner == club);
        /* wrong node, wrong lobby, no lobby part: unknown */
        CHECK(registry_lobby_find(s, "CH1:kz862", NULL) == NULL);
        CHECK(registry_lobby_find(s, "LA2:kz862", NULL) == NULL);
        CHECK(registry_lobby_find(s, "CH2:zzzzz", NULL) == NULL);
        CHECK(registry_lobby_find(s, "CH2:", NULL) == NULL);
        CHECK(registry_lobby_find(s, "kz862", NULL) == NULL);
        /* registry_lobby stays exact (reservations, federation rows) */
        CHECK(registry_lobby(s, "CH2:kz862", NULL) == NULL);
        /* seek-game ?game=<short>: the canonical lobby, not a quick-join replacement */
        MmPickQuery qs = {.region = "SV", .game_id = "CH2:kz862", .auto_change = 0, .now_ms = 2000};
        CHECK(registry_pick(s, &ps, &qs, &pick) == MM_PICK_OK && pick.node == ch2 && !pick.changed &&
              !strcmp(pick.lobby->game_id, "CHITOWN2:kz862"));
        qs.game_id = "CH2:gone1";
        CHECK(registry_pick(s, &ps, &qs, &pick) == MM_PICK_INVALID_GAME);
        /* a lobby whose tuple names another region than its node's hello: both match */
        cJSON *t = cJSON_Parse("[\"rrrrr\",\"LA\",0,8,{\"i\":\"Burg\"}]");
        CHECK(registry_lobby_upsert(s, ch1, t) == 0);
        cJSON_Delete(t);
        CHECK(registry_lobby_find(s, "LA1:rrrrr", &owner) && owner == ch1);
        CHECK(registry_lobby_find(s, "CH1:rrrrr", &owner) && owner == ch1);
        registry_free(s);
    }

    /* mm-fill: populated lobbies fill before empty ones; ties on round time, then node load */
    {
        Registry *f = registry_new();
        MmPolicy pf = {.cross_region = 1, .default_max_players = 8};
        MmNode *e1 = add_node(f, "E1", "eu", 0), *e2 = add_node(f, "E2", "eu", 0), *e3 = add_node(f, "E3", "eu", 0);
        MmNode *us = add_node(f, "US", "us", 0);
        add_lobby(f, e1, "a", 0, 8, 0, "1.0");
        add_lobby(f, e2, "b", 0, 8, 0, "1.0");
        add_lobby(f, e3, "c", 2, 8, 0, "1.0");
        add_lobby(f, us, "d", 6, 8, 0, "1.0");
        MmPickQuery qe = {.region = "eu", .auto_change = 1, .now_ms = 2000};
        /* the populated lobby (2 players, last in order) over the empty ones; never the fuller one
         * of another region while the region has room */
        CHECK(registry_pick(f, &pf, &qe, &pick) == MM_PICK_OK && !strcmp(pick.lobby->game_id, "E3:c"));
        /* a lobby one seat from full is still preferred, a full one is skipped */
        add_lobby(f, e3, "c", 7, 8, 0, "1.0");
        CHECK(registry_pick(f, &pf, &qe, &pick) == MM_PICK_OK && !strcmp(pick.lobby->game_id, "E3:c"));
        add_lobby(f, e3, "c", 8, 8, 0, "1.0");
        CHECK(registry_pick(f, &pf, &qe, &pick) == MM_PICK_OK && !strcmp(pick.lobby->game_id, "E1:a"));
        /* the region full: the fullest lobby with room anywhere (cross_region) */
        add_lobby(f, e1, "a", 8, 8, 0, "1.0");
        add_lobby(f, e2, "b", 8, 8, 0, "1.0");
        CHECK(registry_pick(f, &pf, &qe, &pick) == MM_PICK_OK && !strcmp(pick.lobby->game_id, "US:d"));

        /* equal population: more round time left wins (the end screen last), unknown is not penalised */
        cJSON *t;
        t = cJSON_Parse("[\"a\",\"eu\",3,8,{\"i\":\"Burg\",\"v\":\"1.0\",\"cs\":false,\"timeLeft\":40}]");
        registry_lobby_upsert(f, e1, t);
        cJSON_Delete(t);
        t = cJSON_Parse("[\"b\",\"eu\",3,8,{\"i\":\"Burg\",\"v\":\"1.0\",\"cs\":false,\"timeLeft\":200}]");
        registry_lobby_upsert(f, e2, t);
        cJSON_Delete(t);
        t = cJSON_Parse("[\"c\",\"eu\",3,8,{\"i\":\"Burg\",\"v\":\"1.0\",\"cs\":false,\"timeLeft\":0}]");
        registry_lobby_upsert(f, e3, t);
        cJSON_Delete(t);
        CHECK(registry_lobby(f, "E2:b", NULL)->time_left == 200 && registry_lobby(f, "E1:a", NULL)->time_left == 40);
        CHECK(registry_pick(f, &pf, &qe, &pick) == MM_PICK_OK && !strcmp(pick.lobby->game_id, "E2:b"));
        /* population beats round time: one more player in the lobby about to end */
        t = cJSON_Parse("[\"c\",\"eu\",4,8,{\"i\":\"Burg\",\"v\":\"1.0\",\"cs\":false,\"timeLeft\":0}]");
        registry_lobby_upsert(f, e3, t);
        cJSON_Delete(t);
        CHECK(registry_pick(f, &pf, &qe, &pick) == MM_PICK_OK && !strcmp(pick.lobby->game_id, "E3:c"));
        /* equal population and time: the less loaded node (E1 also runs a busy lobby) */
        add_lobby(f, e1, "a", 3, 8, 0, "1.0");
        add_lobby(f, e2, "b", 3, 8, 0, "1.0");
        add_lobby(f, e3, "c", 8, 8, 0, "1.0");
        add_lobby(f, e1, "a2", 8, 8, 0, "1.0");
        CHECK(registry_lobby(f, "E1:a", NULL)->time_left == -1);
        CHECK(registry_pick(f, &pf, &qe, &pick) == MM_PICK_OK && !strcmp(pick.lobby->game_id, "E2:b"));
        registry_lobby_remove(f, e1, "a2");
        /* all equal: the first in order (stable) */
        CHECK(registry_pick(f, &pf, &qe, &pick) == MM_PICK_OK && !strcmp(pick.lobby->game_id, "E1:a"));

        /* v7.1.4 ServerManager.seek: sk === false (AUTO_SEEK off) is never quick-joined, only by id */
        t = cJSON_Parse("[\"a\",\"eu\",5,8,{\"i\":\"Burg\",\"v\":\"1.0\",\"cs\":false,\"sk\":false}]");
        registry_lobby_upsert(f, e1, t);
        cJSON_Delete(t);
        CHECK(registry_lobby(f, "E1:a", NULL)->no_seek);
        CHECK(registry_pick(f, &pf, &qe, &pick) == MM_PICK_OK && !strcmp(pick.lobby->game_id, "E2:b"));
        MmPickQuery qid = {.region = "eu", .game_id = "E1:a", .auto_change = 0, .now_ms = 2000};
        CHECK(registry_pick(f, &pf, &qid, &pick) == MM_PICK_OK && !strcmp(pick.lobby->game_id, "E1:a"));
        add_lobby(f, e1, "a", 0, 8, 0, "1.0");
        add_lobby(f, e2, "b", 0, 8, 0, "1.0");
        add_lobby(f, e3, "c", 0, 8, 0, "1.0");
        CHECK(!registry_lobby(f, "E1:a", NULL)->no_seek);

        /* concurrent seekers (no report in between): each issued seat counts, so all of them
         * land together until the lobby is full, then the next empty one fills */
        for (int i = 0; i < 8; i++) {
            char jti[16];
            snprintf(jti, sizeof(jti), "c%d", i);
            CHECK(registry_pick(f, &pf, &qe, &pick) == MM_PICK_OK && !strcmp(pick.lobby->game_id, "E1:a"));
            registry_reserve(f, jti, pick.lobby->game_id, 30000);
        }
        CHECK(registry_pick(f, &pf, &qe, &pick) == MM_PICK_OK && !strcmp(pick.lobby->game_id, "E2:b"));
        for (int i = 0; i < 8; i++) {
            char jti[16];
            snprintf(jti, sizeof(jti), "c%d", i);
            registry_release(f, jti);
        }

        /* stale counts: a player is in E3:c (joined by id), the node has not reported it yet */
        registry_reserve(f, "s1", "E3:c", 30000);
        CHECK(registry_pick(f, &pf, &qe, &pick) == MM_PICK_OK && !strcmp(pick.lobby->game_id, "E3:c"));
        /* ticket auth: the node does not count the player yet; the seat stays (shortened) */
        registry_ticket_authed(f, "s1", 2000 + MM_AUTH_GRACE_MS);
        CHECK(registry_reserved(f, "E3:c", 2000) == 1);
        CHECK(registry_reserved(f, "E3:c", 2000 + MM_AUTH_GRACE_MS) == 0); /* refused: freed soon */
        /* player-join: still counted until the node reports the lobby (debounced lobby-up) */
        registry_arrive(f, "s1", NULL, 2100, MM_ARRIVE_GRACE_MS);
        CHECK(registry_reserved(f, "E3:c", 2200) == 1);
        CHECK(registry_pick(f, &pf, &qe, &pick) == MM_PICK_OK && !strcmp(pick.lobby->game_id, "E3:c"));
        /* a second seeker within the same second: lands there too, before any report */
        registry_reserve(f, "s2", "E3:c", 30000);
        CHECK(registry_lobby_taken(f, registry_lobby(f, "E3:c", NULL), 2200) == 2);
        /* a report of another lobby settles nothing */
        add_lobby(f, e1, "a", 0, 8, 0, "1.0");
        CHECK(registry_reserved(f, "E3:c", 2200) == 2);
        /* the report counting s1: s1 settles (no double count), s2 is still in flight */
        add_lobby(f, e3, "c", 1, 8, 0, "1.0");
        CHECK(registry_reserved(f, "E3:c", 2300) == 1);
        CHECK(registry_lobby_taken(f, registry_lobby(f, "E3:c", NULL), 2300) == 2);
        CHECK(registry_pick(f, &pf, &qe, &pick) == MM_PICK_OK && !strcmp(pick.lobby->game_id, "E3:c"));
        /* no report after a player-join: the seat counts for the arrival grace only */
        registry_arrive(f, "s2", NULL, 2400, MM_ARRIVE_GRACE_MS);
        CHECK(registry_reserved(f, "E3:c", 2400 + MM_ARRIVE_GRACE_MS - 1) == 1);
        CHECK(registry_reserved(f, "E3:c", 2400 + MM_ARRIVE_GRACE_MS) == 0);
        /* a later report does not settle an already counted seat twice */
        registry_arrive(f, "s2", NULL, 2500, MM_ARRIVE_GRACE_MS);
        add_lobby(f, e3, "c", 2, 8, 0, "1.0");
        CHECK(registry_reserved(f, "E3:c", 2600) == 0);
        /* federation: settled seats of a node are handed over once (their rows go after the persist) */
        char jtis[4][40];
        CHECK(registry_take_settled(f, "E1", jtis, 4) == 0);
        size_t ns = registry_take_settled(f, "E3", jtis, 4);
        CHECK(ns == 2 && ((!strcmp(jtis[0], "s1") && !strcmp(jtis[1], "s2")) || (!strcmp(jtis[0], "s2") && !strcmp(jtis[1], "s1"))));
        CHECK(registry_take_settled(f, "E3", jtis, 4) == 0);
        /* a ticket another brain issued (its row counts the seat there): recorded, never counted
         * here, released after the report */
        registry_arrive(f, "far", "E2:b", 3000, MM_ARRIVE_GRACE_MS);
        CHECK(registry_reserved(f, "E2:b", 3000) == 0);
        registry_arrive(f, "nogame", NULL, 3000, MM_ARRIVE_GRACE_MS); /* no federation: nothing */
        CHECK(registry_take_settled(f, "E2", jtis, 4) == 0);
        add_lobby(f, e2, "b", 1, 8, 0, "1.0");
        CHECK(registry_take_settled(f, "E2", jtis, 4) == 1 && !strcmp(jtis[0], "far"));
        /* node ids that prefix each other ("E1" / "E1X") stay apart */
        MmNode *e1x = add_node(f, "E1X", "eu", 0);
        add_lobby(f, e1x, "x", 0, 8, 0, "1.0");
        registry_reserve(f, "x1", "E1X:x", 30000);
        registry_arrive(f, "x1", NULL, 3000, MM_ARRIVE_GRACE_MS);
        add_lobby(f, e1x, "x", 1, 8, 0, "1.0");
        CHECK(registry_take_settled(f, "E1", jtis, 4) == 0 && registry_take_settled(f, "E1X", jtis, 4) == 1);
        /* a remote (federated) copy of a lobby never settles this brain's seats */
        registry_reserve(f, "r1", "FAR:z", 30000);
        registry_arrive(f, "r1", NULL, 3000, MM_ARRIVE_GRACE_MS);
        registry_remote_begin(f);
        MmNode *far = registry_remote_node(f, "FAR");
        cJSON *rt = cJSON_Parse("[\"FAR:z\",\"eu\",0,8,{\"i\":\"Burg\",\"v\":\"1.0\",\"cs\":false}]");
        CHECK(far && registry_lobby_upsert(f, far, rt) == 0);
        cJSON_Delete(rt);
        registry_remote_end(f);
        CHECK(registry_reserved(f, "FAR:z", 3000) == 1);
        registry_expire(f, 100000);
        CHECK(f->res_count == 0);
        registry_free(f);
    }

    /* mm-token: v10 seek_modes / seek_maps (dataQuery dsmo / dsma) */
    {
        Registry *m = registry_new();
        MmPolicy pm = {.cross_region = 1, .default_max_players = 8};
        MmNode *a = add_node(m, "MA", "eu", 0), *b = add_node(m, "MB", "eu", 0);
        cJSON *ta = cJSON_Parse("[\"x\",\"eu\",3,8,{\"i\":\"Burg\",\"mode\":0,\"mapIndex\":0}]");
        cJSON *tb = cJSON_Parse("[\"y\",\"eu\",1,8,{\"i\":\"Sandstorm\",\"mode\":1,\"mapIndex\":5}]");
        CHECK(registry_lobby_upsert(m, a, ta) == 0 && registry_lobby_upsert(m, b, tb) == 0);
        cJSON_Delete(ta);
        cJSON_Delete(tb);
        CHECK(registry_lobby(m, "MB:y", NULL)->map_index == 5);
        const int tdm[] = {1}, burg[] = {0}, none[] = {7}, either[] = {9, 0};
        MmPickQuery qm = {.region = "eu", .auto_change = 1, .now_ms = 2000};
        CHECK(registry_pick(m, &pm, &qm, &pick) == MM_PICK_OK && !strcmp(pick.lobby->game_id, "MA:x")); /* fuller */
        qm.modes = tdm, qm.mode_count = 1;
        CHECK(registry_pick(m, &pm, &qm, &pick) == MM_PICK_OK && !strcmp(pick.lobby->game_id, "MB:y"));
        qm.maps = burg, qm.map_count = 1; /* mode 1 and map 0: nothing plays both */
        CHECK(registry_pick(m, &pm, &qm, &pick) == MM_PICK_NO_PREF_MATCH);
        qm.modes = NULL, qm.mode_count = 0;
        CHECK(registry_pick(m, &pm, &qm, &pick) == MM_PICK_OK && !strcmp(pick.lobby->game_id, "MA:x"));
        qm.maps = either, qm.map_count = 2;
        CHECK(registry_pick(m, &pm, &qm, &pick) == MM_PICK_OK && !strcmp(pick.lobby->game_id, "MA:x"));
        qm.maps = none, qm.map_count = 1;
        CHECK(registry_pick(m, &pm, &qm, &pick) == MM_PICK_NO_PREF_MATCH);
        MmPickQuery qid = qm; /* a requested game is never filtered */
        qid.game_id = "MB:y";
        CHECK(registry_pick(m, &pm, &qid, &pick) == MM_PICK_OK && !strcmp(pick.lobby->game_id, "MB:y") && !pick.changed);
        /* every lobby full: no seat at all, whatever the prefs (Servers are at Max Capacity) */
        cJSON *fa = cJSON_Parse("[\"x\",\"eu\",8,8,{\"i\":\"Burg\",\"mode\":0,\"mapIndex\":0}]");
        cJSON *fb = cJSON_Parse("[\"y\",\"eu\",8,8,{\"i\":\"Sandstorm\",\"mode\":1,\"mapIndex\":5}]");
        CHECK(registry_lobby_upsert(m, a, fa) == 0 && registry_lobby_upsert(m, b, fb) == 0);
        cJSON_Delete(fa);
        cJSON_Delete(fb);
        CHECK(registry_pick(m, &pm, &qm, &pick) == MM_PICK_NONE);
        registry_free(m);
    }

    /* remote refresh */
    registry_remote_begin(r);
    MmNode *rm = registry_remote_node(r, "SGP");
    CHECK(rm != NULL);
    snprintf(rm->region, sizeof(rm->region), "sgp");
    CHECK(registry_remote_node(r, "NY") == NULL); /* local stays local */
    registry_remote_end(r);
    CHECK(registry_node(r, "SGP") != NULL);
    registry_remote_begin(r);
    registry_remote_end(r);
    CHECK(registry_node(r, "SGP") == NULL);

    int total = 0;
    cJSON *list = registry_game_list(r, 2000, &total);
    CHECK(cJSON_GetArraySize(list) == 5);
    CHECK(total == 3 + 8 + 7 + 1 + 0);
    cJSON_Delete(list);

    registry_node_remove(r, "NY");
    CHECK(registry_lobby(r, "NY:aaaaa", NULL) == NULL);
    registry_free(r);
    if (!g_fail) printf("brain registry: OK\n");
    return g_fail;
}
