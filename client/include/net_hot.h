// The web hot loader's early seek (platform/web/hot-loader.js, doc/web-hot-loader.md): the page's
// first script seeks through kn_brain's /mm/hot-seek before the client has downloaded, and opens the
// WebRTC channel to the answer's server (net_rtc.c adopts that). servers_ui.c's startup seek takes
// the answer here instead of seeking itself. Native builds send the same seek from C as soon as the
// command line is parsed (net_hot_seek_start, from backend_hot_start), before the window, GL and the
// menu page are set up.
#ifndef KN_NET_HOT_H
#define KN_NET_HOT_H

// Native: sends GET <path> (/mm/hot-seek?...) for the lobby game ("" = any); 0 when not sent. The web: a no-op
// (the page's hot loader sent it).
int net_hot_seek_start(const char *path, const char *game);
// -1: no hot seek (native: none sent; the web: a page the loader skipped, or already taken / dropped); 0: its answer
// is not back yet; else the answer's HTTP status (-2 = the request failed: no answer)
int net_hot_seek_status(void);
// ms since the hot seek was sent (the web: since the page's hot loader started; 0 without one)
double net_hot_age_ms(void);
// The lobby the page asked for (its ?game=; "" = any), into out
void net_hot_seek_game(char *out, int size);
// The answer's body (malloc'd JSON), and the hot seek is marked taken: NULL when there is none
char *net_hot_seek_take(void);
// The hot seek's answer will not be used (logged with why): the client seeks itself
void net_hot_seek_drop(const char *why);

#endif
