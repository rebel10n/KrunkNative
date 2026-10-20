// seek_claim_test: which matchmaking answer may put the player in a lobby (client/src/ui/seek_claim.c).
//   cmake-build-debug/seek_claim_test
// The scripts are the orderings the brain lane gives: answers come back in the order their requests were
// sent, and a seek a ping list (or a region race) sends goes out after the answer that is in front of it.
#include <seek_claim.h>

#include <stdio.h>

static int g_pass, g_fail;

#define CHECK(c, ...) do { \
    if (c) g_pass++; \
    else { g_fail++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } \
} while (0)

// A community map's Start Game while the launch seek's ping list is out: the ping list's seek goes out
// behind the host request, so its answers (a lobby, a full lobby, a failure) come after the host joined.
static void test_community_join_then_late_seek(void) {
    SeekClaims c = {0};
    const unsigned launch = seek_claim_start(&c); // Auto Seek on Launch: its ping list is in flight
    const unsigned host = seek_claim_start(&c);   // Start Game (the community map)
    CHECK(!seek_claim_overtaken(&c, host), "host: its answer is not overtaken");
    CHECK(seek_claim_enter(&c, host) == host, "host: the host answer claims its lobby");
    CHECK(seek_claim_overtaken(&c, launch), "late launch seek (a lobby answer): dropped, the player stays in the host's lobby");
    CHECK(seek_claim_overtaken(&c, launch), "late launch seek (a failure): dropped, no error on the lobby");
    CHECK(seek_claim_overtaken(&c, host), "a duplicate of the host answer: dropped, not joined twice");
    const unsigned later = seek_claim_start(&c); // a seek started after the join is a new action
    CHECK(!seek_claim_overtaken(&c, later), "a seek started after the join is not overtaken");
}

// Two user actions: the later one answers first and claims the lobby; the earlier one is then overtaken.
static void test_later_action_first(void) {
    SeekClaims c = {0};
    const unsigned quick = seek_claim_start(&c); // Quick Match
    const unsigned join = seek_claim_start(&c);  // a lobby picked by hand
    seek_claim_enter(&c, join);
    CHECK(seek_claim_overtaken(&c, quick), "Quick Match answered after the join: dropped");
}

// Two user actions answered in order: each lobby is entered (the newest one wins, nothing is dropped).
static void test_earlier_action_first(void) {
    SeekClaims c = {0};
    const unsigned first = seek_claim_start(&c);
    const unsigned second = seek_claim_start(&c);
    CHECK(!seek_claim_overtaken(&c, first), "first answer: not overtaken");
    seek_claim_enter(&c, first);
    CHECK(!seek_claim_overtaken(&c, second), "second answer after the first lobby: not overtaken (the latest click wins)");
    seek_claim_enter(&c, second);
    CHECK(seek_claim_overtaken(&c, first), "the first action is over once the second has its lobby");
}

// A server's redirect (or a ticket) enters a lobby without a request: it claims a new action, so a seek in
// flight is overtaken by it.
static void test_redirect_claims(void) {
    SeekClaims c = {0};
    const unsigned seek = seek_claim_start(&c);
    const unsigned claimed = seek_claim_enter(&c, 0);
    CHECK(claimed > seek, "a redirect claims a new action, after the seek in flight");
    CHECK(seek_claim_overtaken(&c, seek), "the seek in flight is overtaken by the redirect");
    CHECK(!seek_claim_overtaken(&c, 0), "action 0 (no request) is never overtaken");
}

// "Could not reach the matchmaker" is shown only for the player's current action, outside a lobby.
static void test_error_shown(void) {
    SeekClaims c = {0};
    const unsigned launch = seek_claim_start(&c);
    CHECK(seek_claim_error_shown(&c, launch, 0), "the current action's failure outside a lobby is shown");
    CHECK(!seek_claim_error_shown(&c, launch, 1), "a failure while the player is in a lobby is not shown");
    const unsigned host = seek_claim_start(&c);
    CHECK(!seek_claim_error_shown(&c, launch, 0), "an older action's failure is not shown once a newer one exists");
    CHECK(seek_claim_error_shown(&c, host, 0), "the newest action's failure is shown");
    seek_claim_enter(&c, host);
    CHECK(!seek_claim_error_shown(&c, host, 0), "an action that entered a lobby does not show a failure");
    CHECK(!seek_claim_error_shown(&c, 0, 0), "action 0 never shows a failure");
}

int main(void) {
    test_community_join_then_late_seek();
    test_later_action_first();
    test_earlier_action_first();
    test_redirect_claims();
    test_error_shown();
    printf("seek_claim_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
