// client/src/ui/join_errors.c without a window: every failed-join code the matchmaker (KN's brain and
// krunker.io's) can answer, and where its text shows. The texts are checked against Krunker v10.0.2's
// own English (ui/i18n/en.json matchmaker.* / error.connectedElsewhere, flattened the way the native
// error screen shows them): the same case must show v10's words, not the raw code.
//   cmake-build-debug/join_errors_test
#include <join_errors.h>
#include <i18n.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail, g_pass;
#define CHECK(cond, ...) do { if (cond) g_pass++; else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// ---- test doubles: the English defaults (the page's en.json is not loaded here)
const char *i18n_tr(const char *key, const char *english) {
    (void) key;
    return english;
}

const char *i18n_trf(char *out, size_t n, const char *key, const char *english, int argc, const char *const *argv) {
    (void) key;
    size_t o = 0;
    for (const char *p = english; *p && o + 1 < n; ) {
        if (argc > 0 && !strncmp(p, "{0}", 3)) {
            for (const char *a = argv[0]; *a && o + 1 < n; a++) out[o++] = *a;
            p += 3;
        } else {
            out[o++] = *p++;
        }
    }
    out[o] = 0;
    return out;
}

static void text_of(const char *code, char *out, size_t n) {
    join_error_text(code, out, n);
}

// One row: the code the matchmaker answers (NULL: nothing answered) and the text the player must see.
typedef struct {
    const char *code;
    const char *expect;
    int v10; // v10.0.2 has this case's text (the expectation is its English)
} Row;

static const Row ROWS[] = {
    // v10.0.2 texts (en.json, matchmaker.* and error.connectedElsewhere, with <br><small> flattened)
    {"GameFull", "Game is full.", 1},
    {"InvalidGameId", "Game not found.", 1},
    {"NoServersMatchQuery", "GAME UPDATING", 1}, // v10: the Game Updating screen (menu_ui.c keys on the text)
    {"updating", "GAME UPDATING", 1},
    {"NoAvailableServers", "Servers are at Max Capacity", 1},
    {"no_lobbies", "Servers are at Max Capacity", 1},
    {"NoMatchingServers", "No Games Found. Try changing Quickplay options", 1},
    {"CaptchaRequired", "Security check failed. Complete the security check and try again", 1},
    {"RecaptchaFailed", "Security check failed. Refresh the page and try again", 1},
    {"TooManyAccountSessions", "Too many active sessions. Your account has too many games in progress. Close them and try again", 1},
    {"GameIssue", "Unknown Join Issue", 1},
    {"AccountAlreadyPlaying", "Connected Elsewhere. Your account started a game on another device or tab, so this session was closed", 1},
    {"Failed", "Failed", 1},
    {"InvalidClientValidationToken", "Socket Error", 1}, // v10's ERRORS table words
    {"InvalidClientIP", "Socket Mismatch", 1},
    {"ExpiredClientValidationToken", "Validation Error", 1},
    {"InvalidClientValidationTokenData", "Data Error", 1},
    // KN's own: v10 has no text for these cases (its matchmaker shows the raw word or nothing)
    {"AccountRequired", "This game requires an account.", 0},
    {"TooManyRequests", "Too many requests, try again in a minute", 0},
    {"InvalidKey", "The matchmaker refused this join (InvalidKey).", 0},
    {"TooManyIPs", "The matchmaker refused this join (TooManyIPs).", 0},
    {"SomeNewWord", "The matchmaker refused this join (SomeNewWord).", 0},
    {NULL, "Could not reach the matchmaker", 0}, // nothing answered: DNS, refused, timeout, the proxy
};

int main(void) {
    char out[256];
    for (size_t i = 0; i < sizeof(ROWS) / sizeof(ROWS[0]); i++) {
        const Row *r = &ROWS[i];
        const char *label = r->code ? r->code : "(no answer)";
        text_of(r->code, out, sizeof(out));
        CHECK(!strcmp(out, r->expect), "%s: shows \"%s\", want \"%s\"", label, out, r->expect);
        CHECK(out[0] != 0, "%s: empty text", label);
        CHECK(!strchr(out, '<') && !strchr(out, '>'), "%s: markup left in \"%s\"", label, out);
        if (r->v10) printf("  v10  %-32s \"%s\"\n", label, out);
        else printf("  KN   %-32s \"%s\"\n", label, out);
    }

    // a failed answer with no word is v10's "Unknown Join Issue", never "Game is full." (the old fallback)
    text_of("GameIssue", out, sizeof(out));
    CHECK(strcmp(out, "Game is full.") != 0, "an answer without a word must not read as a full game");

    // a long v10 text fits its buffer whole (the popup and the screen copy it)
    char small[16];
    join_error_text("TooManyAccountSessions", small, sizeof(small));
    CHECK(strlen(small) < sizeof(small) && small[0] == 'T', "a small buffer is written NUL-terminated (\"%s\")", small);

    // where it shows: the Lost Connection screen when the join never started, v10's error popup in a game
    CHECK(join_error_place(0) == JOIN_ERROR_SCREEN, "not in a game: the Lost Connection screen");
    CHECK(join_error_place(1) == JOIN_ERROR_SCREEN, "in a game: v10's error screen too (a failed seek leaves the game behind it)");

    printf("join_errors_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
