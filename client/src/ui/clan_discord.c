#include <clan_discord.h>

static int ci_prefix(const char *s, const char *lit, int n) {
    for (int i = 0; i < n; i++) {
        unsigned char a = (unsigned char) s[i], b = (unsigned char) lit[i];
        if (!a) return 0;
        if (a >= 'A' && a <= 'Z') a = (unsigned char) (a + 32);
        if (b >= 'A' && b <= 'Z') b = (unsigned char) (b + 32);
        if (a != b) return 0;
    }
    return 1;
}

int clan_discord_link_ok(const char *value) {
    if (!value) return 0;
    for (const char *p = value; *p; p++) {
        const char *q;
        int len = 0;
        if (!ci_prefix(p, "discord", 7)) continue;
        q = p + 7;
        if (ci_prefix(q, ".com/", 5)) q += 5;
        else if (ci_prefix(q, "app.com/", 8)) q += 8;
        else if (ci_prefix(q, ".gg/", 4)) q += 4;
        else continue;
        if (ci_prefix(q, "invite/", 7)) q += 7;
        while (len < 32 && (q[len] == '-' || (q[len] >= '0' && q[len] <= '9') ||
                            ((unsigned char) q[len] | 32) >= 'a' && ((unsigned char) q[len] | 32) <= 'z'))
            len++;
        if (len >= 2) return 1;
    }
    return 0;
}
