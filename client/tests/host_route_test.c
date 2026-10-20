// host_route_test: which backend Start Game hosts on (client/include/host_route.h, host_flow_ui.c).
//   cmake-build-debug/host_route_test
// Before the fix the chooser's Start Game hosted on krunker.io only while an official game was the session, so a
// Server Type of krunker.io with no open connection created the game on Box Office's brain instead.
#include <host_route.h>

#include <stdio.h>

static int g_pass, g_fail;

#define CHECK(c, ...) do { \
    if (c) g_pass++; \
    else { g_fail++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } \
} while (0)

int main(void) {
    // chooser (no tab), Server Type krunker.io, no official game open: krunker.io hosts (not Box Office's brain)
    CHECK(host_route_krunker(-1, 1, 0) == 1, "Server Type krunker.io with no open game hosts on krunker.io");
    // chooser, Server Type Box Office, no official game: Box Office's brain hosts
    CHECK(host_route_krunker(-1, 0, 0) == 0, "Server Type Box Office hosts on the brain");
    // chooser, Server Type Box Office, in an official game: unchanged (krunker.io hosts from its own game)
    CHECK(host_route_krunker(-1, 0, 1) == 1, "an official game the player is in still hosts on krunker.io");
    // a Servers tab's choice wins over the Server Type
    CHECK(host_route_krunker(0, 1, 0) == 0, "the Box Office tab hosts on the brain whatever the Server Type");
    CHECK(host_route_krunker(1, 0, 1) == 1, "the krunker.io tab hosts on krunker.io whatever the Server Type");
    CHECK(host_route_krunker(1, 0, 0) == 1, "the krunker.io tab hosts on krunker.io with no open game");
    printf("host_route_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
