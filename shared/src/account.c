// WP-A: GameHooks.account_info front (shared/include/account.h).
#include <account.h>
#include <shared.h>
#include <string.h>

int game_account_info(const Game *game, const Player *player, AccountInfo *out) {
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    if (!game || !player || !game->hooks.account_info) return 0;
    return game->hooks.account_info(game, player, out) ? 1 : 0;
}
