// The end table's extra cells and rows (client/src/ui/end_cells.c) without a window: NET_EXT_KN_END_STATS' dmg, the rows
// past MODE_END's 16 (NET_EXT_KN_END_ROW), and the dmg, candy and time cells as v10.0.7's statForm and endForm print them.
//   cmake-build-debug/end_cells_test
#include <end_cells.h>
#include <mode_columns.h>

#include <stdio.h>
#include <string.h>

static int g_fail, g_pass;
#define CHECK(cond, ...) do { if (cond) g_pass++; else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const char *cell(const int mode_id, const char *stat, const int uid, const int score, const int time_stat) {
    static char out[64];
    snprintf(out, sizeof(out), "(none)");
    if (!end_cells_text(mode_id, stat, uid, score, time_stat, out, sizeof(out))) return "(none)";
    return out;
}

static NetEndRowPacket row_at(const int index, const int uid, const int dmg) {
    NetEndRowPacket row;
    memset(&row, 0, sizeof(row));
    row.index = index;
    row.uid = uid;
    row.dmg = dmg;
    row.stats[NET_MODE_STAT_OBJECTIVE] = (int16_t) (uid * 2);
    return row;
}

int main(void) {
    end_cells_reset();

    // endForm.dmg: Math.round(dmgDealt || 0) (deob-10.0.7.js:18343-18345); an older server sends no END_STATS: 0
    CHECK(!strcmp(cell(MODE_ID_POINT, "dmg", 3, 0, 0), "0"), "dmg without END_STATS: %s", cell(MODE_ID_POINT, "dmg", 3, 0, 0));
    NetEndStatsPacket stats;
    memset(&stats, 0, sizeof(stats));
    stats.count = 2;
    stats.entries[0] = (NetEndStatsEntry){3, 1234};
    stats.entries[1] = (NetEndStatsEntry){7, 88};
    end_cells_set_stats(&stats);
    CHECK(!strcmp(cell(MODE_ID_POINT, "dmg", 3, 0, 0), "1234") && !strcmp(cell(MODE_ID_RAID, "dmg", 7, 0, 0), "88") &&
              !strcmp(cell(MODE_ID_POINT, "dmg", 9, 0, 0), "0"),
          "dmg from END_STATS by uid, 0 for a uid it lacks");

    // Candy Heist's statForm.candy: Math.floor(score / 10) (:106114-106118)
    CHECK(!strcmp(cell(MODE_ID_CANDY, "candy", 3, 125, 0), "12") && !strcmp(cell(MODE_ID_CANDY, "candy", 3, -15, 0), "-2") &&
              !strcmp(cell(MODE_ID_CANDY, "candy", 3, -10, 0), "-1") && !strcmp(cell(MODE_ID_CANDY, "candy", 3, 9, 0), "0"),
          "candy floors (125 -> 12, -15 -> -2, -10 -> -1, 9 -> 0)");

    // statForm.time: timer || "DNF" (:18319-18321). Race's timer is getTime with showMS (:104868); Raid's onEnd gives
    // getTime(ms) with no fraction (:105253). MODE_END's stat is the tenths + 1.
    CHECK(!strcmp(cell(MODE_ID_RACE, "time", 3, 0, 623 + 1), "01:02.3"), "Race: %s", cell(MODE_ID_RACE, "time", 3, 0, 624));
    CHECK(!strcmp(cell(MODE_ID_RAID, "time", 3, 0, 623 + 1), "01:02"), "Raid: MM:SS, no tenths (%s)", cell(MODE_ID_RAID, "time", 3, 0, 624));
    CHECK(!strcmp(cell(MODE_ID_RACE, "time", 3, 0, 0), "DNF") && !strcmp(cell(MODE_ID_RAID, "time", 3, 0, 0), "DNF"), "no time: DNF");
    CHECK(!strcmp(cell(MODE_ID_FFA, "score", 3, 5, 0), "(none)"), "a column end_cells does not format is left to hud_modes.c");

    // the rows past 16: kept in the end order only, their dmg read when END_STATS has none for the uid
    CHECK(end_cells_more_count() == 0 && !end_cells_more_row(0), "no rows past 16 yet");
    NetEndRowPacket r = row_at(17, 40, 5);
    CHECK(!end_cells_add_row(&r) && end_cells_more_count() == 0, "row 17 before row 16 is dropped");
    for (int i = 0; i < 24; i++) {
        r = row_at(16 + i, 100 + i, 500 + i);
        CHECK(end_cells_add_row(&r), "row %d kept", 16 + i);
    }
    CHECK(end_cells_more_count() == 24 && end_cells_more_row(23)->uid == 123 && end_cells_more_entry(23)->uid == 123 &&
              end_cells_more_entry(23)->stats[NET_MODE_STAT_OBJECTIVE] == 246 && end_cells_more_find(110)->index == 26 &&
              !end_cells_more_find(3),
          "a 40-player table: rows 16..39 in order, MODE_END entries for them");
    CHECK(!strcmp(cell(MODE_ID_POINT, "dmg", 123, 0, 0), "523") && !strcmp(cell(MODE_ID_POINT, "dmg", 3, 0, 0), "1234"),
          "dmg of row 39 from its END_ROW, of row 0 from END_STATS");
    for (int i = 40; i < NET_END_ROWS_MAX + 4; i++) {
        r = row_at(i, 100 + i, 0);
        end_cells_add_row(&r);
    }
    CHECK(end_cells_more_count() == END_CELLS_MORE_MAX, "at most NET_END_ROWS_MAX rows (%d past 16)", end_cells_more_count());

    // the next round's END_STATS drops the last round's rows
    end_cells_set_stats(&stats);
    CHECK(end_cells_more_count() == 0 && !strcmp(cell(MODE_ID_POINT, "dmg", 123, 0, 0), "0"), "a new END_STATS clears the rows");
    end_cells_reset();
    CHECK(!strcmp(cell(MODE_ID_POINT, "dmg", 3, 0, 0), "0"), "reset clears END_STATS");

    printf("end_cells_test: %d checks, %d failed\n", g_pass + g_fail, g_fail);
    return g_fail ? 1 : 0;
}
