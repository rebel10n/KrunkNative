#include <skin_style.h>
#include "viewer/viewer_store.h"
#include <stdio.h>
#include <string.h>

#define STYLE_PLAYERS 64
#define STYLE_SLOTS 8 // NET_SKIN_STYLES_MAX

static struct {
    int used, uid, count;
    int skin[STYLE_SLOTS], style[STYLE_SLOTS];
    double wear[STYLE_SLOTS]; // the worn copy's m.wear (NET_EXT_KN_SKIN_STYLES V2), -1 none
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

void skin_style_set(const int uid, const int count, const int *skins, const int *styles) { skin_style_set_wears(uid, count, skins, styles, NULL); }

void skin_style_set_wears(const int uid, int count, const int *skins, const int *styles, const double *wears) {
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
        g_styles[i].wear[k] = wears && wears[k] >= 0.0 ? wears[k] : -1.0;
    }
}

double skin_style_wear_of(const int uid, const int skin) {
    const int i = slot_of(uid, 0);
    if (i < 0 || skin < 0) return -1.0;
    for (int k = 0; k < g_styles[i].count; k++)
        if (g_styles[i].skin[k] == skin) return g_styles[i].wear[k];
    return -1.0;
}

int skin_style_known(const int uid) { return slot_of(uid, 0) >= 0; }

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

void skin_style_preview(const ViewerSkin *item, const int style, char *out, const unsigned long n) {
    if (!out || !n) return;
    if (!item) {
        out[0] = 0;
        return;
    }
    const int dir = item->asset_type ? item->asset_type : item->type;
    const char *ext = item->style_ext && *item->style_ext ? item->style_ext : "png";
    if (item->pat >= 0) snprintf(out, n, "textures/previews/cosmetics/%d_%d_c%d_s%d.%s", dir, item->id, item->pat, style, ext);
    else if (viewer_skin_tex_truthy(item)) snprintf(out, n, "textures/previews/cosmetics/%d_%d_%s_s%d.%s", dir, item->id, item->tex, style, ext);
    else snprintf(out, n, "textures/previews/cosmetics/%d_%d_s%d.%s", dir, item->id, style, ext);
}
