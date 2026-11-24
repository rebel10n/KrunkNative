// clan_discord_link_ok: windows[12].updateDis (deob-10.0.7.js:220483), the i flag included.
//   clan_discord_test
#include <clan_discord.h>

#include <stdio.h>

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
    CHECK(clan_discord_link_ok("https://discord.gg/ab"), "discord.gg");
    CHECK(clan_discord_link_ok("https://Discord.GG/Ab-12"), "a capitalised host (the i flag)");
    CHECK(clan_discord_link_ok("https://DISCORD.COM/invite/ok99"), "discord.com/invite");
    CHECK(clan_discord_link_ok("https://DiscordApp.com/invite/zz"), "discordapp.com");
    CHECK(clan_discord_link_ok("discord.gg/invite/code2"), "the invite segment is optional");
    CHECK(!clan_discord_link_ok("https://discord.gg/a"), "one character is too short");
    CHECK(!clan_discord_link_ok("not a link"), "a bare word is not a link");
    CHECK(!clan_discord_link_ok(""), "empty");
    CHECK(!clan_discord_link_ok(0), "null");
    if (failures) {
        printf("%d/%d failed\n", failures, checks);
        return 1;
    }
    printf("ok %d\n", checks);
    return 0;
}
