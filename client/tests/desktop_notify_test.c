// The desktop notifications switch and the centre items that reach the desktop (client/src/ui/
// desktop_notify.c), without a window or a notifier: the sink stands in for the OS, so no process
// is started and nothing is shown.
//   - the switch: default on, absent or "true" is on, "false" is off, and a stored value is read back;
//   - an item posts once while the switch is on, and nothing while it is off;
//   - the caption of the toggle for each state (off, on with a notifier, on without one).
//   cmake-build-debug/desktop_notify_test
#include <desktop_notify.h>

#include <stdio.h>
#include <string.h>

static int g_fail, g_pass;
#define CHECK(cond, ...) do { if (cond) g_pass++; else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int g_sent;
static char g_title[128], g_text[512];
static void sink(const char *title, const char *text) {
    g_sent++;
    snprintf(g_title, sizeof(g_title), "%s", title);
    snprintf(g_text, sizeof(g_text), "%s", text);
}

static void switch_tests(void) {
    CHECK(desktop_notify_enabled() == 1, "the switch is on by default");
    CHECK(desktop_notify_parse(NULL) == 1, "an absent desktopNotify is on");
    CHECK(desktop_notify_parse("") == 1, "an empty desktopNotify is on");
    CHECK(desktop_notify_parse("true") == 1, "\"true\" is on");
    CHECK(desktop_notify_parse("false") == 0, "\"false\" is off");

    desktop_notify_set_enabled(0);
    CHECK(desktop_notify_enabled() == 0, "set off reads back off");
    desktop_notify_set_enabled(1);
    CHECK(desktop_notify_enabled() == 1, "set on reads back on");
}

static void post_tests(void) {
    desktop_notify_set_sink(sink);

    g_sent = 0;
    desktop_notify_set_enabled(1);
    desktop_notify_item("Friend Request", "bob followed you. Follow them back to become friends.");
    CHECK(g_sent == 1, "a friend request posts once (got %d)", g_sent);
    CHECK(!strcmp(g_title, "Friend Request") && !strncmp(g_text, "bob followed you", 16), "the item's title and text reach the sink");

    desktop_notify_set_enabled(0);
    desktop_notify_item("Message from alice", "hi");
    desktop_notify_item("Wild Offer!", "LUCKY YOU! Nice offer - grab it!");
    desktop_notify_item("Survey", "Question?");
    CHECK(g_sent == 1, "nothing posts while the switch is off (got %d)", g_sent);

    desktop_notify_set_enabled(1);
    desktop_notify_item("Message from alice", "hi");
    CHECK(g_sent == 2 && !strcmp(g_title, "Message from alice"), "a message posts again once the switch is back on (got %d)", g_sent);

    desktop_notify_item(NULL, NULL);
    CHECK(g_sent == 3 && g_title[0] == 0 && g_text[0] == 0, "NULL title and text post as empty text (got %d)", g_sent);

    desktop_notify_set_sink(NULL);
}

static void caption_tests(void) {
    char out[256];
    desktop_notify_caption(0, 1, out, sizeof(out));
    CHECK(!strcmp(out, "Desktop notifications are off."), "off caption: '%s'", out);
    desktop_notify_caption(0, 0, out, sizeof(out));
    CHECK(!strcmp(out, "Desktop notifications are off."), "off caption ignores the notifier: '%s'", out);
    desktop_notify_caption(1, 1, out, sizeof(out));
    CHECK(strstr(out, "also show on your desktop") != NULL, "on caption with a notifier: '%s'", out);
    desktop_notify_caption(1, 0, out, sizeof(out));
    CHECK(strstr(out, "No desktop notifier") != NULL && strstr(out, "notify-send") != NULL, "on caption without one: '%s'", out);
    desktop_notify_caption(1, 1, out, 8);
    CHECK(strlen(out) < 8 && out[0] == 'F', "a short buffer is truncated, not overrun: '%s'", out);
}

int main(void) {
    switch_tests();
    post_tests();
    caption_tests();
    printf("desktop_notify_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
