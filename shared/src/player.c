#include <shared.h>
#include <math.h>
#include <pcg_basic.h>

Player *player_init(Game *game) {
    Player *player = calloc(1, sizeof(Player));
    if (!player) return NULL;

    player->game = game;

    return player;
}

void player_update_height(Player *player) {
    player->height = game_constants.player_height;
}

void player_spawn(Player *player) {
    player->active = 1;
    player_update_height(player);

    if (player->game->map && player->game->map->spawn_count) {
        const Spawn *spawn = player->game->map->spawns[pcg32_boundedrand(player->game->map->spawn_count)];

        player->position = spawn->position;
        player->direction.x = 0.0f;
        player->direction.y = -(float) M_PI / 2.0f * (1.0f + (float) spawn->direction);
    }
}

void player_proc_input(Player *player, const Input *input) {
    const float delta = CLAMP(input->delta, game_constants.min_delta, game_constants.max_delta);
}
