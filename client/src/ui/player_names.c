// Hide player names: the display boundary for names, clan tags and profile pictures (player_names.h).
#include <player_names.h>

// client_settings_hide_player_names (client_settings.h, ui_client_settings.c): the one thing this module reads.
// The weak default (names shown) serves the builds that link no settings UI (the net and protocol tests).
__attribute__((weak)) int client_settings_hide_player_names(void) {
    return 0;
}

#include <stdio.h>
#include <string.h>

#define PN_MAX 256        // players numbered in one match (a match has at most a few dozen)
#define PN_NAME_MAX 64    // the name a uid-less player is numbered by

typedef struct {
    int uid;              // -1: numbered by name
    char name[PN_NAME_MAX];
    int number;
} PnEntry;

static PnEntry g_pn[PN_MAX];
static int g_pn_count;
static int g_pn_last;     // the last number handed out (1, 2, ...)

int player_names_hidden(void) {
    return client_settings_hide_player_names();
}

// The player's number, handed out on first sight. 0 when the table is full (shown as "Player").
static int pn_number(const int uid, const char *name) {
    for (int i = 0; i < g_pn_count; i++) {
        const PnEntry *e = &g_pn[i];
        if (uid >= 0 ? e->uid == uid : (e->uid < 0 && !strcmp(e->name, name ? name : ""))) return e->number;
    }
    if (g_pn_count >= PN_MAX) return 0;
    PnEntry *e = &g_pn[g_pn_count++];
    e->uid = uid >= 0 ? uid : -1;
    snprintf(e->name, sizeof(e->name), "%s", name ? name : "");
    e->number = ++g_pn_last;
    return e->number;
}

void player_names_see(const int uid) {
    if (uid >= 0) pn_number(uid, NULL);
}

void player_names_reset(void) {
    g_pn_count = 0;
    g_pn_last = 0;
}

const char *player_names_shown(const int uid, const int self, const char *name, char *out, const size_t size) {
    if (!player_names_hidden() || !name || !name[0]) return name; // no name, no one to hide (a kill without a killer)
    if (self || !out || !size) return "Player";
    const int n = pn_number(uid, name);
    if (n <= 0) return "Player";
    snprintf(out, size, "Player_%d", n);
    return out;
}

const char *player_names_account(const char *name) {
    return player_names_hidden() ? "Player" : name;
}

const char *player_names_clan(const char *clan) {
    return player_names_hidden() || !clan ? "" : clan;
}
