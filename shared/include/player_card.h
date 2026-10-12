#pragma once
// Krunker v7.1.4 player card data shared by the server (the "3" death stats it sends) and the
// client (the death screen, Change Loadout): config.playerCardStats (src/config.js:2883-3050)
// and the class level of the death screen (f1975: config.rankVar).
#include <stddef.h>
#include <cJSON.h>

#define PLAYER_CARD_STAT_COUNT 28
#define PLAYER_CARD_RANK_VAR 0.03 // config.rankVar

// config.playerCardStats[index].name ("SPK", "Kills", ...), NULL out of range.
const char *player_card_stat_name(int index);

// config.playerCardStats[index].stat(player) from a brain account object (the fields v7's
// account has: kills, deaths, games, wins, funds, score, invVal and the "stats" object), printed
// like JavaScript prints the value (toFixed(2) ratios, "<n>%" accuracy, plain integers).
// Returns 0 (out[0] = 0) for an index out of range or no account.
int player_card_stat_format(int index, const cJSON *account, char *out, size_t n);

// f1975: "lvl N" of a class from account.stats["c" + classIndex]:
// max(1, floor(rankVar * sqrt(stat)) + 1).
int player_card_class_level(double class_stat);

// (shots ? min(shots, hits) / shots * 100 : 0).round(1) + "%": the "Accuracy" of the stat
// above and of the death screen's life stats (players.js:4860).
void player_card_accuracy(double shots, double hits, char *out, size_t n);
