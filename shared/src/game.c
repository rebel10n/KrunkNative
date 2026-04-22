#include <shared.h>

void game_players_add(Game *game, Player *player, const int is_you) {
    if (!game->players) {
        game->player_count = is_you ? 1 : 2;
        game->players = calloc(game->player_count, sizeof(Player));

        if (!game->players) {
            game->player_count = 0;
            return;
        }

        game->players[is_you ? 0 : 1] = player;
        return;
    }

    if (is_you) game->players[0] = player;
    else {
        Player **new_players = realloc(game->players, (game->player_count + 1) * sizeof(Player *));
        if (!new_players) return;

        new_players[game->player_count++] = player;
        game->players = new_players;
    }
}