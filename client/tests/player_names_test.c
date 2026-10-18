// Hide player names (client/src/ui/player_names.c) without a window: the setting off changes nothing; on,
// every other player is Player_<n> (n counts in the order the client first saw them, the same number for the
// whole match, from 1 again after player_names_reset), the local player is Player, an account is Player, and
// no clan tag shows; a name-less kill or line passes through, and the table's overflow shows Player.
//   cmake-build-debug/player_names_test
#include <player_names.h>

#include <stdio.h>
#include <string.h>

// the setting (client_settings_hide_player_names, ui_client_settings.c): the test sets it
static int g_hide;
int client_settings_hide_player_names(void) {
    return g_hide;
}

static int g_fail, g_pass;
#define CHECK(cond, ...) do { if (cond) g_pass++; else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void shown_is(const int uid, const int self, const char *name, const char *want) {
    char out[64];
    const char *got = player_names_shown(uid, self, name, out, sizeof(out));
    CHECK(want ? (got && !strcmp(got, want)) : got == NULL, "shown(uid %d, self %d, '%s') = '%s', want '%s'", uid, self, name ? name : "(null)", got ? got : "(null)", want ? want : "(null)");
}

static void off_tests(void) {
    g_hide = 0;
    player_names_reset();
    shown_is(10, 0, "Bob", "Bob");
    shown_is(10, 1, "Alice", "Alice");
    shown_is(11, 0, NULL, NULL);
    CHECK(!strcmp(player_names_clan("KN"), "KN"), "clan shown when names are not hidden");
    CHECK(!strcmp(player_names_account("Alice"), "Alice"), "account shown when names are not hidden");
    CHECK(player_names_clan(NULL)[0] == 0, "no clan is an empty clan");
}

static void on_tests(void) {
    g_hide = 1;
    player_names_reset();
    player_names_see(10); // SPAWN order: 10, 20, 30
    player_names_see(20);
    player_names_see(30);
    player_names_see(20); // a repeat keeps its number
    shown_is(20, 0, "Bob", "Player_2");
    shown_is(10, 0, "Carol", "Player_1");
    shown_is(30, 0, "Dave", "Player_3");
    shown_is(10, 0, "Carol", "Player_1");     // stable for the match
    shown_is(40, 0, "Erin", "Player_4");      // first seen by a display: the next number
    shown_is(20, 1, "Bob", "Player");          // the local player is Player, whatever its number
    shown_is(-1, 0, "Guest", "Player_5");     // no uid: numbered by name
    shown_is(-1, 0, "Guest", "Player_5");
    shown_is(-1, 0, "Other", "Player_6");
    shown_is(50, 0, "", "");                   // a name-less line stays as it is
    shown_is(-1, 0, NULL, NULL);
    CHECK(player_names_clan("[KN]")[0] == 0, "clan tag removed while names are hidden");
    CHECK(player_names_clan(NULL)[0] == 0, "no clan while hidden");
    CHECK(!strcmp(player_names_account("Alice"), "Player"), "the account reads Player while hidden");
}

static void reset_tests(void) {
    g_hide = 1;
    player_names_reset(); // leaving the game: the match's numbers start again
    player_names_see(30);
    shown_is(30, 0, "Dave", "Player_1");
    shown_is(20, 0, "Bob", "Player_2");
    g_hide = 0;
    shown_is(20, 0, "Bob", "Bob");            // off again: the name back
    g_hide = 1;
    shown_is(20, 0, "Bob", "Player_2");       // numbers kept while the setting toggles
}

static void overflow_tests(void) {
    g_hide = 1;
    player_names_reset();
    for (int uid = 0; uid < 256; uid++) player_names_see(uid);
    shown_is(255, 0, "x", "Player_256");
    shown_is(300, 0, "y", "Player"); // no room left: Player, never a raw name
}

static void word_tests(void) {
    g_hide = 1;
    CHECK(player_names_word_at("Quentin joined", 0, "Quentin") == 7, "a name at the start of a line");
    CHECK(player_names_word_at("Quentins", 0, "Quentin") == 0, "a name inside a longer word is not a match");
    CHECK(player_names_word_at("xQuentin", 1, "Quentin") == 0, "no match after a word character");
    CHECK(player_names_word_at("a Quentin!", 2, "Quentin") == 7, "a name between spaces and punctuation");
    CHECK(player_names_word_at("Bob's", 0, "Bob") == 3, "an apostrophe ends a word");
    CHECK(player_names_word_at("Hi", 0, "") == 0, "an empty name never matches");
    CHECK(player_names_word_at("Hi", 0, NULL) == 0, "a NULL name never matches");
    CHECK(player_names_extras_hidden(0) == 1, "another player's extras are hidden while names are hidden");
    CHECK(player_names_extras_hidden(1) == 0, "the local player's extras stay");
    g_hide = 0;
    CHECK(player_names_extras_hidden(0) == 0, "extras shown with the setting off");
    g_hide = 1;
}

int main(void) {
    off_tests();
    on_tests();
    reset_tests();
    overflow_tests();
    word_tests();
    printf("player_names_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
