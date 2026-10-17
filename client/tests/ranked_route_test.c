// ranked_backend_route (client/include/ranked_route.h): which ranked backend the Ranked popup uses for the
// connected server and each sign-in state. The connected server decides; the other account is never used.
// ranked_match_url / _host: the
// game socket of a ranked match (v10's _33929 URL, at= only) and the host isInOtherRankedMatch compares.
#include <ranked_route.h>
#include <stdio.h>
#include <string.h>

static int failures;

static void expect_ok(const char *name, const int ok) {
    if (!ok) {
        printf("FAIL %s\n", name);
        failures++;
    } else {
        printf("ok   %s\n", name);
    }
}

static void expect_str(const char *got, const char *want, const char *name) {
    if (strcmp(got, want)) {
        printf("FAIL %s: got \"%s\", want \"%s\"\n", name, got, want);
        failures++;
    } else {
        printf("ok   %s\n", name);
    }
}

static void expect_match(const char *connection, const int production, const int secure, const char *token,
                         const char *want_url, const char *want_log, const char *name) {
    char url[256] = "", log[256] = "";
    if (!ranked_match_url(connection, production, secure, token, url, sizeof(url), log, sizeof(log))) {
        printf("FAIL %s: no URL\n", name);
        failures++;
        return;
    }
    expect_str(url, want_url, name);
    expect_str(log, want_log, "   (the log's URL, without the token)");
}

static void expect(const int official, const int krunker, const int kn, const RankedBackend want, const char *name) {
    const RankedBackend got = ranked_backend_route(official, krunker, kn);
    if (got != want) {
        printf("FAIL %s: got %d, want %d\n", name, (int) got, (int) want);
        failures++;
    } else {
        printf("ok   %s\n", name);
    }
}

int main(void) {
    expect(1, 1, 1, RANKED_BACKEND_KRUNKER, "official, both signed in -> krunker.io");
    expect(1, 1, 0, RANKED_BACKEND_KRUNKER, "official, krunker.io only -> krunker.io");
    expect(1, 0, 1, RANKED_BACKEND_NONE, "official, KrunkNative only -> none (never KrunkNative's on krunker.io)");
    expect(1, 0, 0, RANKED_BACKEND_NONE, "official, neither signed in -> none");
    expect(0, 1, 1, RANKED_BACKEND_KN, "KrunkNative, both signed in -> KrunkNative (never krunker.io's)");
    expect(0, 0, 1, RANKED_BACKEND_KN, "KrunkNative, KrunkNative only -> KrunkNative");
    expect(0, 1, 0, RANKED_BACKEND_NONE, "KrunkNative, krunker.io only -> none (never krunker.io's here)");
    expect(0, 0, 0, RANKED_BACKEND_NONE, "KrunkNative, neither signed in -> none");

    // v10's socket URL: //host:port/ws? with at= as the only parameter (no gameId / clientKey)
    expect_match("127.0.0.1:9", 0, 0, "tok", "ws://127.0.0.1:9/ws?at=tok", "ws://127.0.0.1:9/ws?",
                 "mirror, plain: ws with the port and at= only");
    expect_match("eu-1.krunker.io:3001", 0, 1, "tok", "wss://eu-1.krunker.io:3001/ws?at=tok", "wss://eu-1.krunker.io:3001/ws?",
                 "mirror, https API: wss with the port");
    expect_match("na-7.krunker.io", 0, 0, "tok", "ws://na-7.krunker.io:3000/ws?at=tok", "ws://na-7.krunker.io:3000/ws?",
                 "no port in the connection: 3000 (v10:97583)");
    expect_match("na-7.krunker.io:443", 1, 1, "tok", "wss://na-7.krunker.io/ws?at=tok", "wss://na-7.krunker.io/ws?",
                 "krunker.io's own API: wss with no port (v10's //host on krunker.io)");
    expect_match("127.0.0.1:9", 0, 0, "", "ws://127.0.0.1:9/ws?", "ws://127.0.0.1:9/ws?",
                 "a guest: no at= at all");
    expect_match("127.0.0.1:9", 0, 0, NULL, "ws://127.0.0.1:9/ws?", "ws://127.0.0.1:9/ws?", "a NULL token is a guest");
    {
        char url[16], log[16];
        expect_ok("ranked_match_url: a result that does not fit fails",
                  ranked_match_url("127.0.0.1:9", 0, 0, "tok", url, sizeof(url), log, sizeof(log)) == 0);
    }
    {
        char host[32];
        expect_ok("ranked_match_host: host:port gives host", ranked_match_host("a.krunker.io:3000", host, sizeof(host)) == 1 &&
                  !strcmp(host, "a.krunker.io"));
        expect_ok("ranked_match_host: no colon gives the whole text", ranked_match_host("a.krunker.io", host, sizeof(host)) == 1 &&
                  !strcmp(host, "a.krunker.io"));
        expect_ok("ranked_match_host: empty gives none", ranked_match_host("", host, sizeof(host)) == 0);
        expect_ok("ranked_match_host: a NULL connection gives none", ranked_match_host(NULL, host, sizeof(host)) == 0);
    }
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("ranked_route_test: all ok\n");
    return 0;
}
