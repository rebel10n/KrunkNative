// The end table's extra data and the cells drawn from it (client/include/end_cells.h).
#include <end_cells.h>
#include <mode_columns.h>

#include <stdio.h>
#include <string.h>

static NetEndStatsPacket g_stats;
static NetEndRowPacket g_more[END_CELLS_MORE_MAX];
static NetModeEndEntry g_more_entries[END_CELLS_MORE_MAX];
static int g_more_count;

void end_cells_reset(void) {
    memset(&g_stats, 0, sizeof(g_stats));
    g_more_count = 0;
}

void end_cells_set_stats(const NetEndStatsPacket *stats) {
    if (!stats) return;
    g_stats = *stats;
    if (g_stats.count > NET_END_STATS_MAX) g_stats.count = NET_END_STATS_MAX;
    g_more_count = 0;
}

int end_cells_add_row(const NetEndRowPacket *row) {
    if (!row || g_more_count >= END_CELLS_MORE_MAX || row->index != NET_MODE_END_MAX + g_more_count) return 0;
    g_more[g_more_count] = *row;
    NetModeEndEntry *entry = &g_more_entries[g_more_count];
    memset(entry, 0, sizeof(*entry));
    entry->uid = row->uid;
    for (int s = 0; s < NET_MODE_STAT_COUNT && s < NET_END_ROW_STATS; s++) entry->stats[s] = row->stats[s];
    g_more_count++;
    return 1;
}

int end_cells_more_count(void) {
    return g_more_count;
}

const NetEndRowPacket *end_cells_more_row(const int i) {
    return i >= 0 && i < g_more_count ? &g_more[i] : NULL;
}

const NetModeEndEntry *end_cells_more_entry(const int i) {
    return i >= 0 && i < g_more_count ? &g_more_entries[i] : NULL;
}

const NetEndRowPacket *end_cells_more_find(const int uid) {
    for (int i = 0; i < g_more_count; i++)
        if (g_more[i].uid == uid) return &g_more[i];
    return NULL;
}

int end_cells_dmg(const int uid) {
    for (int i = 0; i < g_stats.count; i++)
        if (g_stats.entries[i].uid == uid) return g_stats.entries[i].dmg;
    const NetEndRowPacket *row = end_cells_more_find(uid);
    return row ? row->dmg : 0;
}

int end_cells_text(const int mode_id, const char *stat, const int uid, const int score, const int time_stat, char *out,
                   const size_t size) {
    if (!stat || !out || !size) return 0;
    if (!strcmp(stat, "dmg")) snprintf(out, size, "%d", end_cells_dmg(uid));
    else if (!strcmp(stat, "candy")) snprintf(out, size, "%d", mode_columns_candy(score));
    else if (!strcmp(stat, "time")) mode_columns_time_text(mode_id, time_stat, out, size);
    else return 0;
    return 1;
}
