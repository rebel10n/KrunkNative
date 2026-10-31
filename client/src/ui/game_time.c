// The game time total and its text (client/include/game_time.h). No GL, DOM or storage calls: hud_ui.c
// reads and writes the stored value through html_storage (native files, web localStorage).
#include <game_time.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void game_time_load(GameTime *t, const char *text) {
    memset(t, 0, sizeof(*t));
    char *end = NULL;
    const double ms = text && *text ? strtod(text, &end) : 0.0;
    t->saved_ms = ms > 0.0 && end != text && isfinite(ms) ? floor(ms) : 0.0;
}

int game_time_step(GameTime *t, const int session, const int counting, const double now_ms) {
    const int was = t->in_session;
    double dt = now_ms - t->last_ms;
    if (!(dt > 0.0) || !was) dt = 0.0; // no time counted across a gap between games, or before the first step
    t->last_ms = now_ms;
    t->in_session = session != 0;

    if (!session) // the game just ended: keep its play
        return was && t->pending_ms > 0.0;
    if (was && counting) t->pending_ms += dt;
    return t->pending_ms >= GAME_TIME_FLUSH_MS;
}

double game_time_ms(const GameTime *t) { return t->saved_ms + t->pending_ms; }

int game_time_store(GameTime *t, char *out, const size_t n) {
    if (!n) return 0;
    const double total = floor(game_time_ms(t));
    t->saved_ms = total;
    t->pending_ms = 0.0;
    const int len = snprintf(out, n, "%.0f", total);
    return len > 0 && (size_t) len < n ? len : 0;
}

// out += fmt-formatted part, never past n
static void append_part(char *out, const size_t n, const char *fmt, const long long v) {
    const size_t len = strlen(out);
    if (len + 1 < n) snprintf(out + len, n - len, fmt, v);
}

void game_time_clock(char *out, const size_t n, const double ms) {
    if (!n) return;
    const long long s = (long long) floor((ms > 0.0 ? ms : 0.0) / 1000.0);
    snprintf(out, n, "%02lld:%02lld:%02lld", s / 3600, s % 3600 / 60, s % 60);
}

double game_time_account_step(GameTimeAccount *a, const int id, const double played_ms, const double counted_ms) {
    const double played = played_ms > 0.0 ? played_ms : 0.0;
    if (a->id != id || a->base_ms != played) { // another account, or the server added a match: count from it
        a->id = id;
        a->base_ms = played;
        a->extra_ms = 0.0;
    }
    if (counted_ms > 0.0) a->extra_ms += counted_ms;
    return a->base_ms + a->extra_ms;
}

void game_time_readable(char *out, const size_t n, const double ms) {
    if (!n) return;
    out[0] = 0;
    long long s = (long long) floor((ms > 0.0 ? ms : 0.0) / 1000.0);
    long long m = s / 60;
    s %= 60;
    long long h = m / 60;
    m %= 60;
    long long d = h / 24;
    h %= 24;
    const long long y = d / 365;
    d %= 365;

    if (y) append_part(out, n, "%lldy ", y);
    if (d) append_part(out, n, "%lldd ", d);
    if (h) append_part(out, n, "%lldh ", h);
    append_part(out, n, "%lldm ", m);
    append_part(out, n, "%llds", s);
}
