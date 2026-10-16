// ui_notify(): KrunkNative's system messages (reloads, server notices, sign-in / matchmaking /
// settings feedback, economy notes). They go to Krunker v10.0.2's notification center
// (v10_notify.c: a text notification, title "Notification", 4000 ms). This file never draws a
// toast: the top-right slide-in toast is the bhop system's and Ctrl TP's (bhop_toast.c, see its
// header), and generic messages never use it.
#include <ui_logic.h>
#include <v10_notify.h>
#include <string.h>

#define NOTIFY_TEXT_MAX 512

// The callers' HTML (escaped text, rarely a tag) as the text a .notification-text shows.
static void html_to_text(const char *html, char *out, const size_t n) {
    static const struct { const char *ent, *ch; } ENT[] = {{"&lt;", "<"}, {"&gt;", ">"}, {"&amp;", "&"}, {"&quot;", "\""}, {"&#39;", "'"}, {"&nbsp;", " "}};
    size_t o = 0;
    for (const char *p = html ? html : ""; *p && o + 1 < n;) {
        if (*p == '<') { // <br> -> a space, other tags dropped
            const char *gt = strchr(p, '>');
            if (!gt) break;
            if (!strncmp(p, "<br", 3) && o && out[o - 1] != ' ') out[o++] = ' ';
            p = gt + 1;
            continue;
        }
        if (*p == '&') {
            size_t k = 0;
            while (k < sizeof(ENT) / sizeof(ENT[0]) && strncmp(p, ENT[k].ent, strlen(ENT[k].ent))) k++;
            if (k < sizeof(ENT) / sizeof(ENT[0])) {
                out[o++] = ENT[k].ch[0];
                p += strlen(ENT[k].ent);
                continue;
            }
        }
        out[o++] = *p++;
    }
    out[o] = 0;
}

void ui_notify(const char *html) {
    char text[NOTIFY_TEXT_MAX];
    html_to_text(html, text, sizeof(text));
    v10_notify_push(I18N("kn.notify.title", "Notification"), text, V10_NOTIFY_DEFAULT_MS);
}

void ui_notify_tick(const double now_ms) {
    v10_notify_tick(now_ms); // the notification center, the announcement banner, the emoji panel
}
