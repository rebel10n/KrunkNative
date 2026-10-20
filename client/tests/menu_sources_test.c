// menu_sources_test: whose games the menu's Popular Now counts and whose host its Live Streams come from
// (client/include/menu_sources.h; v10_menu.c, servers_ui.c).
//   cmake-build-debug/menu_sources_test
// Before the fix Popular Now summed Box Office's and krunker.io's custom games whatever the Server Type, so a Box
// Office menu showed krunker.io's "41 playing" counts, and the Live Streams asked the brain for every Server Type
// (krunker.io's host only while an official game was the session).
#include <menu_sources.h>

#include <stdio.h>

static int g_pass, g_fail;

#define CHECK(c, ...) do { \
    if (c) g_pass++; \
    else { g_fail++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } \
} while (0)

int main(void) {
    // Popular Now: Box Office servers count Box Office's lobbies only, never krunker.io's
    CHECK(menu_popular_sources(SEEK_SERVERS_KN, 0) == MENU_SRC_KN, "Box Office servers: Box Office's games only");
    CHECK(!(menu_popular_sources(SEEK_SERVERS_KN, 0) & MENU_SRC_KIO), "Box Office servers: krunker.io's games are not counted");
    // krunker.io servers: krunker.io's custom games only (v10's own fetchMatches)
    CHECK(menu_popular_sources(SEEK_SERVERS_OFFICIAL, 0) == MENU_SRC_KIO, "krunker.io servers: krunker.io's games only");
    // Both: the two lists summed
    CHECK(menu_popular_sources(SEEK_SERVERS_BOTH, 0) == (MENU_SRC_KN | MENU_SRC_KIO), "Both: both lists");
    // an official krunker.io game is the session: the brain is gated, so krunker.io's are the only ones that answer
    CHECK(menu_popular_sources(SEEK_SERVERS_KN, 1) == MENU_SRC_KIO, "in an official game: krunker.io's games (the brain is gated)");
    CHECK(menu_popular_sources(SEEK_SERVERS_BOTH, 1) == MENU_SRC_KIO, "in an official game with Both: krunker.io's games only");

    // Live Streams: Box Office asks the brain's /streams/top; krunker.io asks api.krunker.io, v10's host
    CHECK(menu_streams_source(SEEK_SERVERS_KN, 0, 1) == MENU_SRC_KN, "Box Office servers with a brain: the brain's streams");
    CHECK(menu_streams_source(SEEK_SERVERS_OFFICIAL, 0, 1) == MENU_SRC_KIO, "krunker.io servers: krunker.io's streams, even with a brain");
    CHECK(menu_streams_source(SEEK_SERVERS_OFFICIAL, 0, 0) == MENU_SRC_KIO, "krunker.io servers without a brain: krunker.io's streams");
    // an official game is the session: krunker.io's whatever the Server Type (the brain is gated)
    CHECK(menu_streams_source(SEEK_SERVERS_KN, 1, 1) == MENU_SRC_KIO, "in an official game: krunker.io's streams");
    // Box Office without a brain: nobody to ask (hidden, v10's error state); Both falls back to krunker.io
    CHECK(menu_streams_source(SEEK_SERVERS_KN, 0, 0) == 0, "Box Office servers without a brain: no host");
    CHECK(menu_streams_source(SEEK_SERVERS_KN, 0, -1) == 0, "Box Office servers while the brain is probed: no host yet");
    CHECK(menu_streams_source(SEEK_SERVERS_BOTH, 0, 1) == MENU_SRC_KN, "Both with a brain: the brain's streams");
    CHECK(menu_streams_source(SEEK_SERVERS_BOTH, 0, 0) == MENU_SRC_KIO, "Both without a brain: krunker.io's streams");
    printf("menu_sources_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
