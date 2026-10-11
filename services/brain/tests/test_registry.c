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
    MmNode *h = registry_pick_host_node(r, "us-nj", "brainA", 1, 2000);
    CHECK(h == NULL); /* NY2 has a lobby with players and max_lobbies 1; NY is not custom */
    registry_lobby_remove(r, b, "bbbbb");
    h = registry_pick_host_node(r, "us-nj", "brainA", 1, 2000);
    CHECK(h == b);
    add_lobby(r, b, "bbbbb", 0, 8, 0, "1.0");
    CHECK(registry_pick_host_node(r, "us-nj", "brainA", 1, 2000) == b); /* idle lobby */
    registry_reserve(r, "j2", "NY2:bbbbb", 3000);
    CHECK(registry_pick_host_node(r, "us-nj", "brainA", 1, 2000) == NULL); /* seat in flight */
    CHECK(registry_pick_host_node(r, "us-nj", "brainA", 1, 3500) == b);

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
