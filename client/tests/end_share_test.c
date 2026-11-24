// #shareTxt has no maxlength (deob-10.0.7.js:227052).
//   end_share_test
#include <stdio.h>
#include <string.h>

void end_share_popup_html(char *out, size_t n, const char *title, const char *placeholder, const char *button);

static int failures, checks;

#define CHECK(cond, ...)                                \
    do {                                                \
        checks++;                                       \
        if (!(cond)) {                                  \
            failures++;                                 \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                        \
            printf("\n");                               \
        }                                               \
    } while (0)

int main(void) {
    char html[1024];
    end_share_popup_html(html, sizeof html, "Share this match", "Add a note", "Post");
    CHECK(strstr(html, "id='shareTxt'"), "the note box is #shareTxt");
    CHECK(!strstr(html, "maxlength"), "the note box has no maxlength");
    CHECK(strstr(html, "placeholder='Add a note'"), "the placeholder is kept");
    CHECK(strstr(html, "onclick='shareMatch()'"), "Share calls shareMatch");
    if (failures) {
        printf("%d/%d failed\n", failures, checks);
        return 1;
    }
    printf("ok %d\n", checks);
    return 0;
}
