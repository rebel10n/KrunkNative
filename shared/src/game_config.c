// STUB (WP-A): shared/include/game_config.h bodies until WP-E implements the v7 game config and
// the shared next-match flow (doc/v7/map-logic.md §7 WP-E). WP-E replaces this file.
#include <game_config.h>

int game_apply_map_config(Game *game, const Map *map) {
    (void) game;
    (void) map;
    return 0;
}

void game_next_match(Game *game) { (void) game; }

int game_vote_match(Game *game, Player *player, const int mode, const int map_index) {
    (void) game;
    (void) player;
    (void) mode;
    (void) map_index;
    return 0;
}
