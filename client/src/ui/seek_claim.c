// seek_claim.h: the matchmaking actions and the lobby they claimed. Plain counters: no window, no network.
#include <seek_claim.h>

unsigned seek_claim_start(SeekClaims *c) {
    if (!++c->started) c->started = 1; // wrapped: 0 stays "none"
    return c->started;
}

int seek_claim_overtaken(const SeekClaims *c, const unsigned action) { return action != 0 && action <= c->claimed; }

int seek_claim_error_shown(const SeekClaims *c, const unsigned action, const int in_lobby) {
    return !in_lobby && action != 0 && action == c->started && !seek_claim_overtaken(c, action);
}

unsigned seek_claim_enter(SeekClaims *c, const unsigned action) {
    const unsigned mine = action ? action : seek_claim_start(c);
    if (mine > c->claimed) c->claimed = mine;
    return mine;
}
