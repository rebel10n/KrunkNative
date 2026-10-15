// shared/src/youtube_link.c: the map video ids kn_brain stores and the client links.
#include <youtube_link.h>

#include <stdio.h>
#include <string.h>

static int g_fail;

static void expect_id(const char *link, const char *want) {
    char id[16] = "x";
    const int ok = kn_youtube_id(link, id, sizeof(id));
    const int pass = want ? ok && !strcmp(id, want) : !ok && !id[0];
    if (!pass) {
        printf("FAIL kn_youtube_id(\"%s\") = %d \"%s\", want %s\n", link ? link : "(null)", ok, id, want ? want : "none");
        g_fail++;
    }
}

int main(void) {
    const char *id = "dQw4w9WgXcQ";
    expect_id("dQw4w9WgXcQ", id);
    expect_id("  dQw4w9WgXcQ\n", id);
    expect_id("https://youtu.be/dQw4w9WgXcQ", id);
    expect_id("https://youtu.be/dQw4w9WgXcQ?t=42", id);
    expect_id("http://www.youtube.com/watch?v=dQw4w9WgXcQ", id);
    expect_id("https://youtube.com/watch?v=dQw4w9WgXcQ", id); // the editor's update value (editor_net.c)
    expect_id("youtube.com/watch?feature=share&v=dQw4w9WgXcQ&t=3", id);
    expect_id("https://m.youtube.com/watch?v=dQw4w9WgXcQ", id);
    expect_id("https://music.youtube.com/watch?v=dQw4w9WgXcQ", id);
    expect_id("https://www.YouTube.com/embed/dQw4w9WgXcQ", id);
    expect_id("https://www.youtube-nocookie.com/embed/dQw4w9WgXcQ?autoplay=1", id);
    expect_id("https://youtube.com/shorts/dQw4w9WgXcQ", id);
    expect_id("https://www.youtube.com/live/dQw4w9WgXcQ?si=abc", id);
    expect_id("//www.youtube.com/v/dQw4w9WgXcQ", id);
    expect_id("a-b_c-d_e-f", "a-b_c-d_e-f");
    // not a video
    expect_id(NULL, NULL);
    expect_id("", NULL);
    expect_id("dQw4w9WgXc", NULL);                                 // 10 characters
    expect_id("dQw4w9WgXcQQ", NULL);                               // 12 characters
    expect_id("https://youtu.be/dQw4w9WgXcQQ", NULL);
    expect_id("https://youtube.com/watch?v=dQw4w9WgXc", NULL);
    expect_id("https://evil.example/watch?v=dQw4w9WgXcQ", NULL);
    expect_id("https://youtube.com.evil.example/watch?v=dQw4w9WgXcQ", NULL);
    expect_id("https://evilyoutube.com/watch?v=dQw4w9WgXcQ", NULL);
    expect_id("https://user@youtube.com/watch?v=dQw4w9WgXcQ", NULL);
    expect_id("javascript:alert(1)//dQw4w9WgXcQ", NULL);
    expect_id("https://youtube.com/watch?x=dQw4w9WgXcQ", NULL);
    expect_id("https://youtube.com/channel/UCqH34FGuOae5o-wO2wOa38A", NULL);
    expect_id("dQw4w9W'XcQ", NULL);
    expect_id("<script>xx</", NULL);

    char url[64];
    if (!kn_youtube_watch_url("https://youtu.be/dQw4w9WgXcQ", url, sizeof(url)) || strcmp(url, "https://www.youtube.com/watch?v=dQw4w9WgXcQ") != 0) {
        printf("FAIL kn_youtube_watch_url = \"%s\"\n", url);
        g_fail++;
    }
    if (kn_youtube_watch_url("not a link", url, sizeof(url)) || url[0]) {
        printf("FAIL kn_youtube_watch_url(not a link) = \"%s\"\n", url);
        g_fail++;
    }
    if (kn_youtube_watch_url("dQw4w9WgXcQ", url, 20) || url[0]) {
        printf("FAIL kn_youtube_watch_url into a short buffer = \"%s\"\n", url);
        g_fail++;
    }
    printf("youtube_link_test: %s\n", g_fail ? "FAILED" : "ok");
    return g_fail ? 1 : 0;
}
