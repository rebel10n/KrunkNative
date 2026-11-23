// chat_filter_test: chat_filter_clean (client/src/ui/chat_filter.c), v10.0.7's profanity Filter.clean
// (deob-10.0.7.js:203240-203290) that Chat Filter / streamer mode apply to chat lines and kill-feed names. The
// expected strings are what the deob's own class returns for the same input (run under node).
//   chat_filter_test
#include "chat_filter.h"

#include <stdio.h>
#include <string.h>

static int failures, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static void expect(const char *in, const char *want) {
    char got[512];
    const int ok = chat_filter_clean(in, got, sizeof(got));
    if (!want) {
        CHECK(!ok, "clean(\"%s\") = \"%s\", want the TypeError (no word character)", in, got);
        return;
    }
    CHECK(ok && !strcmp(got, want), "clean(\"%s\") = %d \"%s\", want \"%s\"", in, ok, got, want);
}

int main(void) {
    expect("hello world", "hello world");
    expect("what the fuck", "what the ****");
    expect("FUCK you, SHIT!", "**** you, ****!");        // the "i" flag; punctuation is its own piece
    expect("a_s_s hole", "*** hole");                    // replaceWord drops "_" before the stars
    expect("assassin class", "assassin class");          // whole pieces only: "ass" inside a word stays
    expect("go to hell", "go to ****");
    expect("god mode", "*** mode");                      // the list's "God"
    expect("b!tch", "b!tch");                            // a word with a non-word character never matches a piece
    expect("fuck-off", "****-off");
    expect("Hi :) shit", "Hi :) ****");
    expect("h\xc3\xa9llo shit", "h\xc3\xa9llo ****");    // a non-ASCII character splits like a JS non-word unit
    expect("sh1t", "****");
    expect("f u c k", "f u c k");
    expect("xxx123 xxx", "xxx123 ***");
    expect("dick's", "****'s");
    // the kill feed cleans the whole name span (deob-10.0.7.js:228087): the markup's own words are clean
    expect("<span style='color:#9eeb56'>&lrm;Bitch&lrm;</span>", "<span style='color:#9eeb56'>&lrm;*****&lrm;</span>");
    // no \b at all: /\b/.exec(text) is null and v10's join throws (the line is dropped)
    expect("!!!", NULL);
    expect("  ", NULL);
    CHECK(chat_filter_is_profane("Shit", 4), "isProfane(Shit)");
    CHECK(!chat_filter_is_profane("Shitake", 7), "isProfane(Shitake)");

    char small[4];
    CHECK(chat_filter_clean("shit happens", small, sizeof(small)) && !strcmp(small, "***"), "truncated: \"%s\"", small);

    if (failures) {
        fprintf(stderr, "chat_filter_test: %d/%d checks failed\n", failures, checks);
        return 1;
    }
    printf("chat_filter_test: %d checks passed\n", checks);
    return 0;
}
