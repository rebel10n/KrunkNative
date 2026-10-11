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
