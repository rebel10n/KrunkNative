// menu_sources_test: whose games the menu's Popular Now shows and whose host its Live Streams come from
// (client/include/menu_sources.h; v10_menu.c, servers_ui.c).
//   cmake-build-debug/menu_sources_test
// Before the fix Popular Now summed Box Office's and krunker.io's custom games whatever the Server Type, so a Box
// Office menu showed krunker.io's "41 playing" counts; and the Live Streams asked only the brain, so with no brain
// (or with a krunker.io account) they never loaded.
#include <menu_sources.h>

#include <stdio.h>

static int g_pass, g_fail;

#define CHECK(c, ...) do { \
    if (c) g_pass++; \
    else { g_fail++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } \
} while (0)

int main(void) {
    // Popular Now: on the Box Office Server Type only; the krunker.io and Both Server Types hide it
    CHECK(menu_popular_on(SEEK_SERVERS_KN), "Box Office servers: Popular Now shows");
    CHECK(!menu_popular_on(SEEK_SERVERS_OFFICIAL), "krunker.io servers: Popular Now is hidden, not filled with krunker.io's games");
    CHECK(!menu_popular_on(SEEK_SERVERS_BOTH), "Both: Popular Now is hidden");

    // Live Streams: the brain's on Box Office while it answers
    CHECK(menu_streams_source(SEEK_SERVERS_KN, 0, 0, 1) == MENU_SRC_KN, "Box Office servers with a brain: the brain's streams");
    CHECK(menu_streams_source(SEEK_SERVERS_BOTH, 0, 0, 1) == MENU_SRC_KN, "Both with a brain: the brain's streams");
    // krunker.io's: the Server Type, an official game (the brain is gated), a signed-in krunker.io account
    CHECK(menu_streams_source(SEEK_SERVERS_OFFICIAL, 0, 0, 1) == MENU_SRC_KIO, "krunker.io servers: krunker.io's streams, even with a brain");
    CHECK(menu_streams_source(SEEK_SERVERS_OFFICIAL, 0, 0, 0) == MENU_SRC_KIO, "krunker.io servers without a brain: krunker.io's streams");
    CHECK(menu_streams_source(SEEK_SERVERS_KN, 1, 0, 1) == MENU_SRC_KIO, "in an official game: krunker.io's streams (the brain is gated)");
    CHECK(menu_streams_source(SEEK_SERVERS_KN, 0, 1, 1) == MENU_SRC_KIO, "krunker.io account signed in: krunker.io's streams, even with a brain");
    CHECK(menu_streams_source(SEEK_SERVERS_KN, 0, 1, 0) == MENU_SRC_KIO, "krunker.io account signed in, no brain: krunker.io's streams");
    CHECK(menu_streams_source(SEEK_SERVERS_BOTH, 0, 0, 0) == MENU_SRC_KIO, "Both without a brain: krunker.io's streams");
    // Box Office without a brain (offline, or still probed): nobody to ask, the overlay stays hidden
    CHECK(menu_streams_source(SEEK_SERVERS_KN, 0, 0, 0) == 0, "Box Office servers without a brain: no host");
    CHECK(menu_streams_source(SEEK_SERVERS_KN, 0, 0, -1) == 0, "Box Office servers while the brain is probed: no host yet");
    printf("menu_sources_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
