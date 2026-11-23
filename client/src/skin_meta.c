#include <skin_meta.h>
#include <stdio.h>
#include <string.h>

// The worn copies' metadata of the match's players (NET_EXT_KN_SKIN_META, client/src/net/game_events.c).
#define META_PLAYERS 64
#define META_SLOTS 12 // NET_SKIN_META_MAX

static struct {
    int used, uid, count;
    int skin[META_SLOTS];
    SkinCopyMeta m[META_SLOTS];
} g_meta[META_PLAYERS];
static int g_next;

static int slot_of(const int uid, const int create) {
    for (int i = 0; i < META_PLAYERS; i++)
        if (g_meta[i].used && g_meta[i].uid == uid) return i;
    if (!create) return -1;
    for (int i = 0; i < META_PLAYERS; i++)
        if (!g_meta[i].used) return i;
    const int i = g_next; // a full table drops the oldest entry
    g_next = (g_next + 1) % META_PLAYERS;
    return i;
}

void skin_meta_set(const int uid, int count, const int *skins, const float *wears, const int *serials, const char *const *names) {
    const int i = slot_of(uid, 1);
    if (count < 0) count = 0;
    if (count > META_SLOTS) count = META_SLOTS;
    memset(&g_meta[i], 0, sizeof(g_meta[i]));
    g_meta[i].used = 1;
    g_meta[i].uid = uid;
    for (int k = 0; k < count; k++) {
        if (!skins || skins[k] < 0) continue;
        SkinCopyMeta *m = &g_meta[i].m[g_meta[i].count];
        m->wear = wears && wears[k] >= 0.0f && wears[k] < 1.0f ? wears[k] : -1.0f;
        m->serial = serials && serials[k] > 0 ? serials[k] : 0;
        snprintf(m->name, sizeof(m->name), "%s", names && names[k] ? names[k] : "");
        if (m->wear < 0.0f && !m->serial && !m->name[0]) continue; // a plain unit: nothing to keep
        g_meta[i].skin[g_meta[i].count++] = skins[k];
    }
}

int skin_meta_of(const int uid, const int skin, SkinCopyMeta *out) {
    if (out) {
        memset(out, 0, sizeof(*out));
        out->wear = -1.0f;
    }
    const int i = slot_of(uid, 0);
    if (i < 0 || skin < 0) return 0;
    for (int k = 0; k < g_meta[i].count; k++) {
        if (g_meta[i].skin[k] != skin) continue;
        if (out) *out = g_meta[i].m[k];
        return 1;
    }
    return 0;
}

float skin_meta_wear(const int uid, const int skin) {
    SkinCopyMeta m;
    return skin_meta_of(uid, skin, &m) ? m.wear : -1.0f;
}

void skin_meta_reset(void) {
    memset(g_meta, 0, sizeof(g_meta));
    g_next = 0;
}
