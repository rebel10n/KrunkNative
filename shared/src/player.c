#include <shared.h>
#include <math.h>
#include <pcg_basic.h>
#include <stdio.h>

Player *player_init(Game *game) {
    Player *player = calloc(1, sizeof(Player));
    if (!player) return NULL;

    player->game = game;

    return player;
}

void player_update_height(Player *player) {
    player->height = game_constants.player_height - player->crouch_val * game_constants.crouch_distance;
}

void player_spawn(Player *player) {
    player->active = 1;
    player->speed = 1.0f; // TODO: apply from class index

    player_update_height(player);

    if (player->game->map && player->game->map->spawn_count) {
        const Spawn *spawn = player->game->map->spawns[pcg32_boundedrand(player->game->map->spawn_count)];

        player->position = spawn->position;
        player->direction.x = 0.0f;
        player->direction.y = -(float) M_PI / 2.0f * (1.0f + (float) spawn->direction);
    }
}

void player_proc_input(Player *player, const Input *input, const int recon, const int move_lock) {
    const float delta = CLAMP(input->delta, game_constants.min_delta, game_constants.max_delta);
    const float move_dir = (float) -M_PI / 4.0f * (float) input->move_dir;

    if (player->noclip) player->on_ground = 1;

    player->direction.x = CLAMP(input->x_dir, -(float) M_PI / 2.0f, (float) M_PI / 2.0f);
    player->direction.y = input->y_dir;

    // TODO: WEAPON LEAN & BOB ANIMATIONS

    if (!recon) {
        // TODO: RECOIL ANIMATION
    }

    // TODO: SCOPING

    if (input->crouch && player->crouch_val < 1.0f && !player->on_ladder) {
        // TODO: CROUCHING
        player->crouch_val += game_constants.crouch_speed * delta;
        if (player->crouch_val > 1.0f) player->crouch_val = 1.0f;

        if (player->on_ground) {
            // TODO: WEAPON BOB ANIMATION
        } else {
            player->position.y += game_constants.crouch_speed * delta;
        }
    } else if (!input->crouch && player->crouch_val > 0.0f) {
        player->crouch_val -= game_constants.crouch_speed * delta;
        if (player->crouch_val < 0.0f)  player->crouch_val = 0.0f;

        if (player->on_ground) {
            // TODO: WEAPON BOB ANIMATION
        } else {
            player->position.y -= game_constants.crouch_speed * delta;
        }
    }

    const int contact = player->on_ground || player->on_ladder;

    if (!move_lock) {
        float accel = contact ? (player->terrain_slipping ? game_constants.slipping_speed : game_constants.player_speed) * player->speed : game_constants.air_speed * 1.0f * 1.0f;

        accel *= player->aim_val == 1.0f ? game_constants.aim_slow : 1.0f;
        accel *= player->crouch_val ? game_constants.crouch_slow : 1.0f;
        // TODO: accel *= game mode speed mlt
        // TODO: accel *= weapon speed mlt
        accel *= (player->noclip ? 2.0f : 1.0f) * delta;

        float decel = game_constants.air_decel;

        if (player->on_ladder) decel = game_constants.ladder_decel;
        else if (player->terrain_slipping) decel = game_constants.terrain_slip_decel;
        else if (player->on_terrain) decel = game_constants.terrain_decel;
        else if (player->on_ground) decel = game_constants.ground_decel;

        if (!player->on_ground || !player->crouch_val) {
            player->slide_timer = 0.0f;
        }

        if (player->slide_timer) {
            player->slide_timer -= delta;

            if (player->slide_timer < 0.0f) player->slide_timer = 0.0f;
            else {
                accel *= 0.25f;
                decel = player->on_terrain ? game_constants.terrain_slide_decel : game_constants.slide_decel;

                const float vel = hypotf(player->velocity.x, player->velocity.z);
                const float yaw = normalize_angle(player->direction.y);

                // if (player->slid_cont) {
                //     player->velocity.x = vel * cosf(yaw + (float) M_PI);
                //     player->velocity.z = vel * sinf(yaw + (float) M_PI);
                // } else {
                //     const float vel_dir = atan2f(-player->velocity.x, -player->velocity.z);
                //     const float angle_delta = normalize_angle(yaw - vel_dir) * 0.18f;
                //
                //     player->velocity.x = vel * cosf(vel_dir + (float) M_PI - angle_delta);
                //     player->velocity.z = vel * sinf(vel_dir + (float) M_PI - angle_delta);
                // }
            }
        }

        player->jump_timer -= delta;
        if (player->jump_timer < 0.0f) player->jump_timer = 0.0f;

        if (player->jump_timer <= 0.0f && (player->on_ground || false /* TODO: game map config infinite jump */)) {
            // TODO: try jump
        }

        // TODO: wall jumps
        // TODO: accel *= CLAMP(1.0f - inputs[12], 0.0f, 1.0f); "what?" - joe biden

        if (input->move_dir % 1) {
            // TODO: accel *= strafe_speed;
        }

        if (input->move_dir >= 0) {
            const float yaw_origin = player->direction.y; // TODO: support different cam types?

            player->velocity.x += accel * -sinf(move_dir + yaw_origin);
            player->velocity.z += accel * -cosf(move_dir + yaw_origin);

            if (player->noclip) player->velocity.y += accel * player->direction.x * (input->move_dir > 2 && input->move_dir < 6 ? -1.0f : 1.0f);
        }

        // TODO: air strafes
        // TODO: xVelC, zVelC (apparently for cylinders?)

        if (player->velocity.x) {
            player->position.x += player->velocity.x * delta * 1000.0f; // TODO: multiply by game map config speed_x
            player->velocity.x *= powf(decel, delta * 1000.0f);
        }

        if (player->velocity.y) {
            player->position.y += player->velocity.y * delta * 1000.0f; // TODO: multiply by game map config speed_y

            if (player->noclip) {
                player->velocity.y *= powf(decel, delta * 1000.0f);
            } else if (player->velocity.y > 0.0f) {
                player->velocity.y -= delta * 0.0032f;
            } else if (player->velocity.y < -0.3f) {
                player->velocity.y = -0.3f;
            }
        }

        if (player->velocity.z) {
            player->position.z += player->velocity.z * delta * 1000.0f; // TODO: multiply by game map config speed_z
            player->velocity.z *= powf(decel, delta * 1000.0f);
        }
    }
}
