#include <skin_style.h>
#include <stdio.h>
#include <string.h>

#define STYLE_PLAYERS 64
#define STYLE_SLOTS 8 // NET_SKIN_STYLES_MAX

static struct {
    int used, uid, count;
    int skin[STYLE_SLOTS], style[STYLE_SLOTS];
} g_styles[STYLE_PLAYERS];
static int g_next;

static int slot_of(const int uid, const int create) {
    for (int i = 0; i < STYLE_PLAYERS; i++)
        if (g_styles[i].used && g_styles[i].uid == uid) return i;
    if (!create) return -1;
    for (int i = 0; i < STYLE_PLAYERS; i++)
        if (!g_styles[i].used) return i;
    const int i = g_next; // a full table drops the oldest entry
    g_next = (g_next + 1) % STYLE_PLAYERS;
    return i;
}

void skin_style_set(const int uid, int count, const int *skins, const int *styles) {
    const int i = slot_of(uid, 1);
    if (count < 0) count = 0;
    if (count > STYLE_SLOTS) count = STYLE_SLOTS;
    memset(&g_styles[i], 0, sizeof(g_styles[i]));
    g_styles[i].used = 1;
    g_styles[i].uid = uid;
    g_styles[i].count = count;
    for (int k = 0; k < count; k++) {
        g_styles[i].skin[k] = skins[k];
        g_styles[i].style[k] = styles[k];
    }
}

int skin_style_of(const int uid, const int skin) {
    const int i = slot_of(uid, 0);
    if (i < 0 || skin < 0) return -1;
    for (int k = 0; k < g_styles[i].count; k++)
        if (g_styles[i].skin[k] == skin) return g_styles[i].style[k];
    return 0;
}

void skin_style_reset(void) {
    memset(g_styles, 0, sizeof(g_styles));
    g_next = 0;
}

void skin_style_texture_key(char *out, const unsigned long n, const char *base, const int style, const int styles) {
    if (style > 0 && style < styles) snprintf(out, n, "%s_s%d", base, style);
    else snprintf(out, n, "%s", base);
}

static void style_preview_variant(char *out, const unsigned long n, const char *base, const int style, const char *style_ext) {
    if (!n) return;
    const char *dot = base ? strrchr(base, '.') : NULL;
    const char *slash = base ? strrchr(base, '/') : NULL;
    if (!dot || (slash && dot < slash)) {
        snprintf(out, n, "%s_s%d.%s", base ? base : "", style, style_ext && *style_ext ? style_ext : "png");
        return;
    }
    snprintf(out, n, "%.*s_s%d.%s", (int) (dot - base), base, style, style_ext && *style_ext ? style_ext : "png");
}

void skin_style_preview_path(char *out, const unsigned long n, const char *base, const int style, const int styles, const char *style_ext) {
    if (style > 0 && style < styles) style_preview_variant(out, n, base, style, style_ext);
    else if (n) snprintf(out, n, "%s", base ? base : "");
}

void skin_style_picker_preview_path(char *out, const unsigned long n, const char *base, const int style, const int styles, const char *style_ext) {
    if (style >= 0 && style < styles) style_preview_variant(out, n, base, style, style_ext);
    else if (n) snprintf(out, n, "%s", base ? base : "");
}
