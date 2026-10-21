// client/src/app_url.c: Krunker page URLs -> page file + query. The hub's Play button opens
// "/?play=<map>" (social_ui.c h_play_map); the game window must receive that query, or Play starts
// the default public game instead of the map.
//   cmake-build-debug/app_url_test
#include <app_url.h>

#include <stdio.h>
#include <string.h>

static int g_fail, g_pass;
#define CHECK(cond, ...) do { if (cond) g_pass++; else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// expect the game (kind 1) with `query`
static void expect_game(const char *url, const char *query) {
    char q[256] = "unset";
    const char *page = "unset";
    const int kind = app_url_split(url, &page, q, sizeof(q));
    CHECK(kind == 1 && page == NULL && !strcmp(q, query), "game %s: kind %d page %s query \"%s\", want \"%s\"", url, kind, page ? page : "(null)", q, query);
}

// expect a page `file` with `query`
static void expect_page(const char *url, const char *file, const char *query) {
    char q[256] = "unset";
    const char *page = NULL;
    const int kind = app_url_split(url, &page, q, sizeof(q));
    CHECK(kind == 2 && page && !strcmp(page, file) && !strcmp(q, query), "page %s: kind %d page %s query \"%s\", want %s \"%s\"", url, kind, page ? page : "(null)", q, file, query);
}

static void expect_other(const char *url) {
    char q[256] = "unset";
    const char *page = "unset";
    const int kind = app_url_split(url, &page, q, sizeof(q));
    CHECK(kind == 0 && page == NULL, "not ours %s: kind %d", url, kind);
}

int main(void) {
    // the hub's Play links keep the map name (spaces are percent-encoded by the hub)
    expect_game("/?play=Sandstorm", "play=Sandstorm");
    expect_game("/?play=My%20Map", "play=My%20Map");
    expect_game("/?play=My+Map&x=1", "play=My+Map&x=1");
    expect_game("https://krunker.io/?play=Burg", "play=Burg");
    expect_game("//krunker.io/?play=Map%26Two", "play=Map%26Two");
    expect_game("/?play=Map#frag", "play=Map");
    expect_game("/?mod=Some%20Mod", "mod=Some%20Mod");
    // the game page itself: "/", "index.html", the fragment and the editor's Test Map parameters
    expect_game("/", "");
    expect_game("", "");
    expect_game("/#top", "");
    expect_game("index.html", "");
    expect_game("index.html?offline=1&testMap=1", "offline=1&testMap=1");
    // the other pages keep their query too (the hub's "?p=map&q=x")
    expect_page("/social.html?p=map&q=Krunker", "social.html", "p=map&q=Krunker");
    expect_page("/social.html?p=map&q=a#frag", "social.html", "p=map&q=a");
    expect_page("./editor.html", "editor.html", "");
    expect_page("https://krunker.io/viewer.html?id=3", "viewer.html", "id=3");
    // not a Krunker page: left to the browser
    expect_other("https://example.com/?play=Burg");
    expect_other("/social.htmlx");
    expect_other("/unknown.html");
    expect_other(NULL);

    printf("app_url_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
