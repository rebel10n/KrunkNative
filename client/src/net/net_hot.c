// The web hot loader's early seek (client/include/net_hot.h, doc/web-hot-loader.md). window.__knHot
// is written by platform/web/hot-loader.js (inlined in <head> of the game page); this only reads it.
#include <net_hot.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>

EM_JS(int, kn_hot_status, (void), {
    var H = typeof window !== 'undefined' ? window.__knHot : null;
    if (!H || H.used) return -1;
    return H.status < 0 ? -2 : (H.status | 0);
});

EM_JS(double, kn_hot_age, (void), {
    var H = typeof window !== 'undefined' ? window.__knHot : null;
    return H ? performance.now() - H.t0 : 0;
});

EM_JS(char *, kn_hot_game, (void), {
    var H = typeof window !== 'undefined' ? window.__knHot : null;
    return stringToNewUTF8(H && H.game ? H.game : "");
});

EM_JS(char *, kn_hot_take, (void), {
    var H = typeof window !== 'undefined' ? window.__knHot : null;
    if (!H || H.used || H.status !== 200) return 0;
    H.used = true;
    H.times.taken = Math.round(performance.now() - H.t0);
    console.log('[hot] the client takes the hot seek (answer at ' + H.times.answer + ' ms, taken at ' + H.times.taken + ' ms)');
    return stringToNewUTF8(H.body);
});

EM_JS(void, kn_hot_drop, (const char *why), {
    var H = typeof window !== 'undefined' ? window.__knHot : null;
    if (!H || H.used) return;
    H.used = true;
    console.log('[hot] the hot seek is not used: ' + UTF8ToString(why));
});

int net_hot_seek_status(void) { return kn_hot_status(); }
double net_hot_age_ms(void) { return kn_hot_age(); }

void net_hot_seek_game(char *out, const int size) {
    char *g = kn_hot_game();
    snprintf(out, (size_t) size, "%s", g ? g : "");
    free(g);
}

char *net_hot_seek_take(void) { return kn_hot_take(); }
void net_hot_seek_drop(const char *why) { kn_hot_drop(why ? why : ""); }

#else

int net_hot_seek_status(void) { return -1; }
double net_hot_age_ms(void) { return 0.0; }
void net_hot_seek_game(char *out, const int size) {
    if (out && size > 0) out[0] = 0;
}
char *net_hot_seek_take(void) { return NULL; }
void net_hot_seek_drop(const char *why) { (void) why; }

#endif
