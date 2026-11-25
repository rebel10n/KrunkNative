#pragma once
// The end table's data that NET_PACKET_MODE_END has no room for, and the cells drawn from it (client/src/ui/end_cells.c).
// Our own server sends NET_EXT_KN_END_STATS (every team's score key, each row's dmg) and then NET_EXT_KN_END_ROW for each
// row past MODE_END's 16 (shared/src/modes/mode_net.c mode_send_end). No DOM or Client: hud_modes.c draws the table.
#include <net_ext.h>
#include <shared.h>
#include <stddef.h>

#define END_CELLS_MORE_MAX (NET_END_ROWS_MAX - NET_MODE_END_MAX)

void end_cells_reset(void);
// A round's NET_EXT_KN_END_STATS. It comes before that round's rows, so it also drops the previous round's rows.
void end_cells_set_stats(const NetEndStatsPacket *stats);
// A NET_EXT_KN_END_ROW: kept when it is the next row (16, 17, ...), so the rows stay in the end order. 1 when kept.
int end_cells_add_row(const NetEndRowPacket *row);
// The rows past 16 kept so far, and row `i` of them (the end table's row 16 + i), NULL past the end.
int end_cells_more_count(void);
const NetEndRowPacket *end_cells_more_row(int i);
// Row `i` past 16 as MODE_END's entry (uid and NetModeStat stats), NULL past the end.
const NetModeEndEntry *end_cells_more_entry(int i);
// The row of `uid` past 16, NULL when it is in the first 16 or not in the table.
const NetEndRowPacket *end_cells_more_find(int uid);
// endForm.dmg, Math.round(dmgDealt || 0) (deob-10.0.7.js:18343-18345): END_STATS' cell, else the extra row's, else 0
// (an older server sends neither).
int end_cells_dmg(int uid);
// The cells formatted here, the same way as the server and the bridge (mode_columns.h): "dmg", "candy" from `score`
// (statForm.candy, :106114-106118) and "time" from MODE_END's time stat (statForm.time: Race "MM:SS.t", Raid "MM:SS",
// "DNF" without one). 1 when `stat` is one of them (out holds the text), 0 otherwise (out is untouched).
int end_cells_text(int mode_id, const char *stat, int uid, int score, int time_stat, char *out, size_t size);
