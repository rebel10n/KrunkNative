/* Unit test: auth join records + rate limiters (auth_state.c), serverHash
 * (shared/src/auth_join.c) and the finder table (finder_table.c): heartbeat, polling,
 * verification by nonce, expiry, filters. No sockets, explicit clocks. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "auth_join.h"
#include "auth_state.h"
#include "finder_table.h"
#include "knsvc/crypto.h"
#include "knsvc/json.h"

static int g_fail;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); g_fail = 1; } } while (0)

static void test_sha_and_hash(void) {
    /* FIPS 180-2 "abc" vector against knsvc's OpenSSL SHA-256 too */
    uint8_t a[32], b[32];
    kn_sha256_bytes("abc", 3, a);
    kn_sha256("abc", 3, b);
    CHECK(!memcmp(a, b, 32));
    char hex[65];
    kn_hex_encode(a, 32, hex, sizeof(hex));
    CHECK(!strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    /* long input crossing blocks */
    char big[1000];
    memset(big, 'x', sizeof(big));
    kn_sha256_bytes(big, sizeof(big), a);
    kn_sha256(big, sizeof(big), b);
    CHECK(!memcmp(a, b, 32));

    char h1[65], h2[65], h3[65], h4[65];
    kn_server_hash("aa", "", "bb", h1);
    kn_server_hash("aa", "", "bb", h2);
    kn_server_hash("aa", "", "bc", h3);
    kn_server_hash("a", "a", "bb", h4); /* separators: ("a","a") != ("aa","") */
    CHECK(!strcmp(h1, h2));
    CHECK(strcmp(h1, h3) != 0);
    CHECK(strcmp(h1, h4) != 0);
    CHECK(strlen(h1) == 64 && kn_auth_is_hex(h1, 64));
    /* the documented construction */
    const char msg[] = "kn-join-v1\0aa\0\0bb";
    kn_sha256(msg, sizeof(msg) - 1, b);
    kn_hex_encode(b, 32, hex, sizeof(hex));
    CHECK(!strcmp(hex, h1));
    CHECK(!kn_auth_is_hex("ABC", 64));
    CHECK(!kn_auth_is_hex("", 64));
}

static void test_joins(void) {
    AuthJoins *j = auth_joins_new();
    cJSON *p = cJSON_Parse("{\"id\":7,\"username\":\"Bob\"}");
    CHECK(auth_joins_put(j, "ab12", 7, "Bob", "1.2.3.4", p, 1000) == 0);
    CHECK(auth_joins_get(j, "ab12", "bob", NULL, 0, 1500) != NULL);           /* case-insensitive */
    CHECK(kn_json_int(auth_joins_get(j, "ab12", "BOB", NULL, 0, 1500), "id", 0) == 7);
    CHECK(auth_joins_get(j, "ab12", "alice", NULL, 0, 1500) == NULL);         /* wrong user */
    CHECK(auth_joins_get(j, "ab13", "bob", NULL, 0, 1500) == NULL);           /* other server */
    CHECK(auth_joins_get(j, "ab12", "bob", "9.9.9.9", 1, 1500) == NULL);      /* ip check */
    CHECK(auth_joins_get(j, "ab12", "bob", "1.2.3.4", 1, 1500) != NULL);
    CHECK(auth_joins_get(j, "ab12", "bob", "9.9.9.9", 0, 1500) != NULL);      /* ip check off */
    CHECK(auth_joins_get(j, "ab12", "bob", NULL, 0, 1000 + AUTH_JOIN_TTL_MS) == NULL); /* expired */
    CHECK(auth_joins_count(j) == 0);                                           /* removed on lookup */
    CHECK(auth_joins_put(j, "cd", 8, "Al", NULL, p, 0) == 0);
    CHECK(auth_joins_put(j, "cd", 9, "Cy", NULL, p, 10) == 0);                 /* replaces */
    CHECK(auth_joins_count(j) == 1);
    CHECK(auth_joins_get(j, "cd", "al", NULL, 0, 20) == NULL);
    CHECK(auth_joins_get(j, "cd", "cy", NULL, 0, 20) != NULL);
    CHECK(auth_joins_purge(j, 10 + AUTH_JOIN_TTL_MS) == 0);
    CHECK(auth_joins_put(j, "", 1, "x", NULL, p, 0) != 0);
    cJSON_Delete(p);
    auth_joins_free(j);
}

static void test_limiter(void) {
    AuthLimiter *l = auth_limiter_new(3, 1000);
    CHECK(auth_limiter_hit(l, "a", 0) && auth_limiter_hit(l, "a", 1) && auth_limiter_hit(l, "a", 2));
    CHECK(!auth_limiter_hit(l, "a", 3));
    CHECK(auth_limiter_hit(l, "b", 3));          /* per key */
    CHECK(auth_limiter_hit(l, "a", 1000));       /* new window */
    auth_limiter_free(l);
    AuthLimiter *u = auth_limiter_new(0, 1000);
    for (int i = 0; i < 100; i++) CHECK(auth_limiter_hit(u, "x", 0));
    auth_limiter_free(u);
}

static cJSON *reply_for(const char *nonce, const char *name, int players, int max, int online) {
    char json[512];
    snprintf(json, sizeof(json),
             "{\"v\":1,\"game\":\"krunknative\",\"name\":\"%s\",\"map\":\"Sandstorm\",\"mode\":\"ffa\",\"modeId\":0,"
             "\"players\":%d,\"max\":%d,\"tags\":[\"ranked\"],\"community\":\"X\",\"online_mode\":%s,"
             "\"password\":false,\"region\":\"eu\",\"version\":\"1\",\"port\":21015,\"nonce\":\"%s\"}",
             name, players, max, online ? "true" : "false", nonce);
    /* through the wire format */
    uint8_t buf[700];
    memcpy(buf, "\xff\xff\xff\xffKNinfo", 10);
    buf[10] = 0;
    memcpy(buf + 11, json, strlen(json));
    return finder_parse_reply(buf, 11 + strlen(json));
}

static void test_finder(void) {
    uint8_t q[64];
    size_t ql = finder_build_query("00ff", q, sizeof(q));
    CHECK(ql == 15 && !memcmp(q, "\xff\xff\xff\xffKNINFO\0" "00ff", 15));
    CHECK(finder_parse_reply((const uint8_t *) "\xff\xff\xff\xffKNinfo\0[]", 13) == NULL); /* not an object */
    CHECK(finder_parse_reply((const uint8_t *) "\xff\xff\xff\xffKNINFO\0{}", 13) == NULL); /* request tag */

    FinderConfig c;
    finder_config_defaults(&c);
    c.max_per_ip = 2;
    FinderTable *t = finder_table_new(&c);
    int created = 0;
    const char *err = NULL;
    FinderEntry *e = finder_heartbeat(t, "10.0.0.1", 21015, 0, "Self Name", "C", 1, "10.0.0.1", 0, &created, &err);
    CHECK(e && created && e->query_port == 21015);
    CHECK(!finder_heartbeat(t, "bad host!", 1, 1, "", "", 0, "1", 0, &created, &err) && err);
    CHECK(!finder_heartbeat(t, "h", 0, 1, "", "", 0, "1", 0, &created, &err) && err);

    FinderFilter all = {.online_mode = -1};
    cJSON *list = finder_list(t, &all, 0);
    CHECK(cJSON_GetArraySize(list) == 0); /* unverified: never listed */
    cJSON_Delete(list);

    FinderEntry *due[8];
    CHECK(finder_due_polls(t, 0, due, 8) == 1);
    finder_poll_stamp(t, e, "n1", 0);
    CHECK(finder_due_polls(t, 1, due, 8) == 0);                        /* outstanding */
    CHECK(!finder_poll_reply(t, e, reply_for("wrong", "S", 1, 8, 1), 5, 100)); /* nonce mismatch */
    CHECK(finder_poll_reply(t, e, reply_for("n1", "Real Name", 3, 8, 1), 20, 100));
    CHECK(e->ping_ms == 20 && e->failures == 0);
    list = finder_list(t, &all, 30);
    CHECK(cJSON_GetArraySize(list) == 1);
    const cJSON *s = cJSON_GetArrayItem(list, 0);
    CHECK(!strcmp(kn_json_str(s, "name", ""), "Real Name")); /* from the query, not the heartbeat */
    CHECK(kn_json_int(s, "players", 0) == 3 && kn_json_bool(s, "online_mode", 0));
    cJSON_Delete(list);

    FinderFilter f = {.online_mode = 0};
    list = finder_list(t, &f, 30);
    CHECK(cJSON_GetArraySize(list) == 0);
    cJSON_Delete(list);
    FinderFilter f2 = {.online_mode = -1, .map = "sandstorm", .tag = "RANKED", .mode = "0", .space = 1, .q = "real"};
    list = finder_list(t, &f2, 30);
    CHECK(cJSON_GetArraySize(list) == 1);
    cJSON_Delete(list);
    FinderFilter f3 = {.online_mode = -1, .map = "kanji"};
    list = finder_list(t, &f3, 30);
    CHECK(cJSON_GetArraySize(list) == 0);
    cJSON_Delete(list);

    /* per-IP cap */
    CHECK(finder_heartbeat(t, "10.0.0.1", 21016, 0, "", "", 0, "10.0.0.1", 0, &created, &err) && created);
    CHECK(!finder_heartbeat(t, "10.0.0.1", 21017, 0, "", "", 0, "10.0.0.1", 0, &created, &err));
    CHECK(finder_remove(t, "10.0.0.1", 21016, "10.0.0.1") == 1);
    CHECK(finder_remove(t, "10.0.0.1", 21015, "6.6.6.6") == 0); /* not the owner */

    /* failed polls -> dropped after max_failures */
    uint64_t now = 20 + (uint64_t) c.poll_interval_ms;
    for (int i = 0; i < c.max_failures; i++) {
        CHECK(finder_table_count(t) == 1);
        CHECK(finder_due_polls(t, now, due, 8) == 1);
        finder_poll_stamp(t, e, "nx", now);
        e->last_heartbeat_ms = now; /* keep heartbeating */
        now += (uint64_t) c.poll_timeout_ms;
        finder_sweep(t, now, NULL);
        now = e && finder_table_count(t) ? e->next_poll_ms : now;
    }
    CHECK(finder_table_count(t) == 0);

    /* heartbeat expiry */
    e = finder_heartbeat(t, "10.0.0.2", 1, 1, "", "", 0, "10.0.0.2", 0, &created, &err);
    CHECK(e);
    finder_sweep(t, (uint64_t) c.expire_ms - 1, NULL);
    CHECK(finder_table_count(t) == 1);
    finder_sweep(t, (uint64_t) c.expire_ms, NULL);
    CHECK(finder_table_count(t) == 0);

    /* stale verified data stops being listed */
    e = finder_heartbeat(t, "10.0.0.3", 1, 1, "", "", 0, "10.0.0.3", 0, &created, &err);
    finder_poll_stamp(t, e, "n3", 0);
    CHECK(finder_poll_reply(t, e, reply_for("n3", "S", 0, 8, 0), 1, 1));
    list = finder_list(t, &all, (uint64_t) c.poll_interval_ms * 3);
    CHECK(cJSON_GetArraySize(list) == 0);
    cJSON_Delete(list);
    finder_table_free(t);
}

/* A KNINFO reply carrying a product flavor (flavor.h): flavor_key NULL = the base game (the field
 * is absent, exactly like a base server). */
static cJSON *reply_flavor(const char *nonce, const char *name, const char *flavor_key) {
    char json[512], flav[64] = "";
    if (flavor_key && *flavor_key) snprintf(flav, sizeof(flav), ",\"flavor\":\"%s\"", flavor_key);
    snprintf(json, sizeof(json),
             "{\"v\":1,\"game\":\"krunknative\",\"name\":\"%s\",\"map\":\"Sandstorm\",\"mode\":\"ffa\",\"modeId\":0,"
             "\"players\":1,\"max\":8,\"tags\":[],\"community\":\"X\",\"online_mode\":false,"
             "\"password\":false,\"region\":\"eu\",\"version\":\"1\",\"port\":21015%s,\"nonce\":\"%s\"}",
             name, flav, nonce);
    uint8_t buf[700];
    memcpy(buf, "\xff\xff\xff\xffKNinfo", 10);
    buf[10] = 0;
    memcpy(buf + 11, json, strlen(json));
    return finder_parse_reply(buf, 11 + strlen(json));
}

/* Strike lobbies list in their own flavor pool: a base listing never shows a Strike server and is
 * byte-identical to before Krunker Strike; a Strike listing shows only Strike servers. */
static void test_finder_flavor(void) {
    FinderTable *t = finder_table_new(NULL);
    int created = 0;
    const char *err = NULL;
    FinderEntry *base = finder_heartbeat(t, "10.1.0.1", 21015, 0, "Base Srv", "", 0, "10.1.0.1", 0, &created, &err);
    FinderEntry *strike = finder_heartbeat(t, "10.1.0.2", 21016, 0, "Strike Srv", "", 0, "10.1.0.2", 0, &created, &err);
    CHECK(base && strike);
    finder_poll_stamp(t, base, "nb", 0);
    finder_poll_stamp(t, strike, "ns", 0);
    CHECK(finder_poll_reply(t, base, reply_flavor("nb", "Base Srv", NULL), 1, 1));
    CHECK(finder_poll_reply(t, strike, reply_flavor("ns", "Strike Srv", "strike"), 1, 1));

    /* base view (no flavor, or "base"): only the base server, and no "flavor" key on it */
    const char *base_views[] = {NULL, "", "base"};
    for (size_t i = 0; i < 3; i++) {
        FinderFilter f = {.online_mode = -1, .flavor = base_views[i]};
        cJSON *list = finder_list(t, &f, 2);
        CHECK(cJSON_GetArraySize(list) == 1);
        const cJSON *s = cJSON_GetArrayItem(list, 0);
        CHECK(s && !strcmp(kn_json_str(s, "name", ""), "Base Srv"));
        CHECK(cJSON_GetObjectItemCaseSensitive(s, "flavor") == NULL); /* base listing unchanged */
        cJSON_Delete(list);
    }

    /* strike view: only the Strike server, carrying flavor:"strike" */
    FinderFilter fs = {.online_mode = -1, .flavor = "strike"};
    cJSON *list = finder_list(t, &fs, 2);
    CHECK(cJSON_GetArraySize(list) == 1);
    const cJSON *s = cJSON_GetArrayItem(list, 0);
    CHECK(s && !strcmp(kn_json_str(s, "name", ""), "Strike Srv"));
    CHECK(!strcmp(kn_json_str(s, "flavor", ""), "strike"));
    cJSON_Delete(list);

    /* "all": both */
    FinderFilter fa = {.online_mode = -1, .flavor = "all"};
    list = finder_list(t, &fa, 2);
    CHECK(cJSON_GetArraySize(list) == 2);
    cJSON_Delete(list);
    finder_table_free(t);
}

/* exclude (a seek's current and recent servers): never listed, whole host:port entries only */
static void test_finder_exclude(void) {
    FinderTable *t = finder_table_new(NULL);
    int created = 0;
    const char *err = NULL;
    FinderEntry *a = finder_heartbeat(t, "10.2.0.1", 21015, 0, "A", "", 0, "10.2.0.1", 0, &created, &err);
    FinderEntry *b = finder_heartbeat(t, "10.2.0.2", 21015, 0, "B", "", 0, "10.2.0.2", 0, &created, &err);
    FinderEntry *c = finder_heartbeat(t, "10.2.0.3", 21015, 0, "C", "", 0, "10.2.0.3", 0, &created, &err);
    CHECK(a && b && c);
    finder_poll_stamp(t, a, "na", 0);
    finder_poll_stamp(t, b, "nb", 0);
    finder_poll_stamp(t, c, "nc", 0);
    CHECK(finder_poll_reply(t, a, reply_flavor("na", "A", "strike"), 1, 1));
    CHECK(finder_poll_reply(t, b, reply_flavor("nb", "B", "strike"), 1, 1));
    CHECK(finder_poll_reply(t, c, reply_flavor("nc", "C", NULL), 1, 1));

    FinderFilter f = {.online_mode = -1, .flavor = "strike", .exclude = "10.2.0.1:21015"};
    cJSON *list = finder_list(t, &f, 2);
    CHECK(cJSON_GetArraySize(list) == 1 && !strcmp(kn_json_str(cJSON_GetArrayItem(list, 0), "name", ""), "B"));
    cJSON_Delete(list);
    /* every Strike server excluded: none is listed (a seek is never sent back to a server it leaves) */
    f.exclude = "10.2.0.2:21015,10.2.0.1:21015";
    list = finder_list(t, &f, 2);
    CHECK(cJSON_GetArraySize(list) == 0);
    cJSON_Delete(list);
    /* partial entries match nothing */
    f.exclude = "10.2.0.1:2101,0.2.0.2:21015,";
    list = finder_list(t, &f, 2);
    CHECK(cJSON_GetArraySize(list) == 2);
    cJSON_Delete(list);
    f.exclude = "";
    list = finder_list(t, &f, 2);
    CHECK(cJSON_GetArraySize(list) == 2);
    cJSON_Delete(list);
    finder_table_free(t);
}

int main(void) {
    test_sha_and_hash();
    test_joins();
    test_limiter();
    test_finder();
    test_finder_flavor();
    test_finder_exclude();
    if (g_fail) return 1;
    printf("auth/finder tests OK\n");
    return 0;
}
