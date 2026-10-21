#pragma once
#include <stddef.h>

// Splits a Krunker page URL ("/", "/?play=<map>", "https://krunker.io/social.html?p=map&q=x",
// "./editor.html", "index.html?testMap=1") into the page file and its query, without any client
// state (app_mode.c maps the page to its mode; client/tests/app_url_test.c checks the split).
// Returns 0 when the URL is not one of ours, 1 for the game (`*page` stays NULL), 2 for a page
// (`*page` is "social.html", "editor.html" or "viewer.html"). `query` gets the parameters
// without the '?' and without the '#fragment' ("" when none); `n` is its size.
int app_url_split(const char *url, const char **page, char *query, size_t n);
