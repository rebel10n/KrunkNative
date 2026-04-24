#include <shared.h>
#include <math.h>
#include <pcg_basic.h>
#include <stdio.h>

Player *player_init(Game *game) {
    Player *player = calloc(1, sizeof(Player));
    if (!player) return NULL;

    player->game = game;
    player->scale = game_constants.player_scale;

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

void player_reset_step(Player *player, const int recon) {
    // TODO: animate

    if (!1 /* TODO: game config can slide */ || 0 /* TODO: game mode real mov */ || !player->crouch_val || !player->can_slide) return;

    player->can_slide = 0;

    player->slide_timer =
        game_constants.slide_time *
        1.0f /* TODO: game config slide time */ *
        1.0f /* TODO: game map config slide time */ *
        player->crouch_val
    ;

    const float slide_mlt = (
        player->on_terrain ?
        game_constants.player_terrain_slide_velocity_mlt :
        game_constants.player_slide_velocity_mlt
    ) * 1.0f; // TODO: game map config slide accel

    player->velocity.x *= slide_mlt;
    player->velocity.z *= slide_mlt;
}

void player_do_map_collisions(Player *player, const int can_jump, const float delta, const int recon) {
    for (size_t i = 0; i < player->game->map->object_count; i++) {
        const Object *object = player->game->map->objects[i];

        const int collides =
            (object->ramp || object->collision_type == COLLISION_TYPE_BOX) &&
            player->position.x + player->scale > object->position.x - object->scale.x * 0.5f &&
            player->position.x - player->scale < object->position.x + object->scale.x * 0.5f &&
            player->position.y + player->height >= object->position.y &&
            player->position.y <= object->position.y + object->scale.y &&
            player->position.z + player->scale > object->position.z - object->scale.z * 0.5f &&
            player->position.z - player->scale < object->position.z + object->scale.z * 0.5f ||

            object->collision_type == COLLISION_TYPE_CYLINDER &&
            player->position.y <= object->position.x + object->scale.y &&
            player->position.y + player->height >= object->position.y
        ;

        if (!collides) continue;

        if (object->collision_type == COLLISION_TYPE_CYLINDER) {
            const float closest_x =
                object->position.x > player->position.x + player->scale ? player->position.x + player->scale :
                object->position.x < player->position.x - player->scale ? player->position.x - player->scale :
                object->position.x
            ;

            const float closest_z =
                object->position.z > player->position.z + player->scale ? player->position.z + player->scale :
                object->position.z < player->position.z - player->scale ? player->position.z - player->scale :
                object->position.x
            ;

            const float r = hypotf(closest_x - object->position.x, closest_z - object->position.z);
            const float coll_r = MIN(object->scale.x, object->scale.z) * 0.5f;

            if (r > coll_r) continue;

            // TODO
            continue;
        }

        if (object->ramp) {
            const float ramp_direction = -(float) M_PI / 2.0f * (float) object->ramp->direction;
            const vec2 player_xz = {player->position.x + player->scale * cosf(ramp_direction), player->position.z + player->scale * sinf(ramp_direction)};

            const float progress = CLAMP(progress_on_line(object->ramp->start, object->ramp->end, player_xz), 0.0f, 1.0f);
            const float collision_height = object->position.y + (object->scale.y + 0.01f) * progress;

            if (player->position.y <= collision_height || can_jump) {
                // TODO: ramp boost

                if (player->last_position.y > player->position.y) player_reset_step(player, recon);

                player->position.y = collision_height;
                player->velocity.y = 0.0f;
                player->on_wall = 0;
                player->on_ground = 1;
                player->on_terrain = 0;
            }

            continue;
        }

        if (
            player->position.y <= object->position.y + object->scale.y &&
            player->last_position.y < object->position.y + object->scale.y &&
            object->position.y + object->scale.y - player->position.y <= game_constants.climb_height &&
            can_jump
        ) {
            player->position.y += delta * 30.0f;

            if (player->position.y > object->position.y + object->scale.y) {
                player->position.y = object->position.y + object->scale.y;
            }

            player->on_ground = 1;
            player->on_terrain = 0;
        } else if (player->last_position.y >= object->position.y + object->scale.y) {
            if (player->last_position.y > player->position.y) player_reset_step(player, recon);

            player->position.y = object->position.y + object->scale.y;
            player->velocity.y = 0.0f;

            player->on_ground = 1;
            player->on_terrain = 0;
        } else if (player->last_position.y + player->height <= object->position.y) {
            player->position.y = object->position.y - player->height;
            player->velocity.y = 0.0f;
        } else if (player->last_position.x - player->scale >= object->position.x + object->scale.x * 0.5f) {
            player->position.x = object->position.x + object->scale.x * 0.5f + player->scale;
            player->velocity.x = 0.0f;
        } else if (player->last_position.x + player->scale <= object->position.x - object->scale.x * 0.5f) {
            player->position.x = object->position.x - object->scale.x * 0.5f - player->scale;
            player->velocity.x = 0.0f;
        } else if (player->last_position.z - player->scale >= object->position.z + object->scale.z * 0.5f) {
            player->position.z = object->position.z + object->scale.z * 0.5f + player->scale;
            player->velocity.z = 0.0f;
        } else if (player->last_position.z + player->scale <= object->position.z - object->scale.z * 0.5f) {
            player->position.z = object->position.z - object->scale.z * 0.5f - player->scale;
            player->velocity.z = 0.0f;
        }
    }
}

void player_jump(Player *player) {
    player->did_act = 1;
    player->jump_timer = (player->terrain_slipping ? game_constants.player_slipping_jump_cooldown : 0.0f) * 1.0f; // TODO game map config jump cooldown
    player->did_jump = 1;
    player->did_wall_jump = 1;
    player->on_terrain = 0;

    const float jump_push = game_constants.jump_push * 1.0f; // TODO game config jumpMlt
    const float jump_vel = game_constants.jump_velocity * 1.0f; // TODO game config jumpMlt, if game mode realMov jump_vel *= 0.92
    const float vel = hypotf(player->velocity.x, player->velocity.z);

    player->velocity.y += jump_vel * (1 - game_constants.crouch_jump * player->crouch_val) * 1.0f /* TODO: player weapon jumpMlt || player weapon speedMlt */ * (player->aim_val ? 1.0f : game_constants.jump_aim_slow);

    player->velocity.x -= vel * jump_push * sinf(player->direction.y);
    player->velocity.z -= vel * jump_push * cosf(player->direction.y);
}

void player_proc_input(Player *player, const Input *input, const int recon, const int move_lock) {
    const float delta = CLAMP(input->delta, game_constants.min_delta, game_constants.max_delta);
    const float move_dir = -(float) M_PI / 2.0f + (float) M_PI / 4.0f * (float) input->move_dir;

    if (player->noclip) player->on_ground = 1;

    player->direction.x = CLAMP(input->x_dir, -(float) M_PI / 2.0f, (float) M_PI / 2.0f);
    player->direction.y = input->y_dir;

    // TODO: WEAPON LEAN & BOB ANIMATIONS

    if (!recon) {
        // TODO: RECOIL ANIMATION
    }

    player->last_position = player->position;

    // TODO: SCOPING

    if (input->crouch && player->crouch_val < 1.0f && !player->on_ladder) {
        player->crouch_val = MIN(1.0f, player->crouch_val + game_constants.crouch_speed * delta);

        if (player->on_ground) {
            // TODO: WEAPON BOB ANIMATION
        } else {
            player->position.y += game_constants.crouch_speed * delta;
        }
    } else if (!input->crouch && player->crouch_val > 0.0f) {
        player->crouch_val = MAX(0.0f, player->crouch_val - game_constants.crouch_speed * delta);

        if (player->on_ground) {
            // TODO: WEAPON BOB ANIMATION
        } else {
            player->position.y -= game_constants.crouch_speed * delta;
        }
    }

    player_update_height(player);

    const int contact = player->on_ground || player->on_ladder;

    if (!move_lock) {
        float accel = contact ? (player->terrain_slipping ? game_constants.slipping_speed : game_constants.player_speed) * player->speed : game_constants.air_speed;

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

        if (player->crouch_val <= 0.5) player->can_slide = 1;

        if (!player->on_ground || !player->crouch_val) {
            player->slide_timer = 0.0f;
        }

        if (player->slide_timer) {
            player->slide_timer = MAX(0.0f, player->slide_timer - delta);

            if (player->slide_timer > 0.0f) {
                accel *= 0.25f;
                decel = player->on_terrain ? game_constants.terrain_slide_decel : game_constants.slide_decel;

                const float vel = hypotf(player->velocity.x, player->velocity.z);
                const float dir = (float) M_PI / 2.0f - player->direction.y;

                if (player->slid_cont) {
                    player->velocity.x = vel * cosf(dir + (float) M_PI);
                    player->velocity.z = vel * sinf(dir + (float) M_PI);
                } else {
                    const float vel_dir = atan2f(-player->velocity.z, -player->velocity.x);
                    const float angle_delta = normalize_angle(dir - vel_dir) * 0.18f;

                    player->velocity.x = vel * cosf(dir + (float) M_PI - angle_delta);
                    player->velocity.z = vel * sinf(dir + (float) M_PI - angle_delta);
                }
            }
        }

        player->jump_timer = MAX(0.0f, player->jump_timer - delta);

        if (player->jump_timer <= 0.0f && (player->on_ground || 0 /* TODO: game map config infinite jump */)) {
            if (player->did_jump && !input->jump) player->did_jump = 0;
            if (input->jump && (!player->did_jump || 0 /* TODO: game map config auto jump */)) {
                player_jump(player);
            }
        }

        if (!contact) {
            const float wall_mlt = 1.0f; // TODO: wall jumps
            player->velocity.y -= delta * game_constants.gravity * 1.0f * wall_mlt; // TODO: game config gravity mlt
        }

        // TODO: accel *= CLAMP(1.0f - inputs[12], 0.0f, 1.0f); "what?" - joe biden

        if (input->move_dir % 1) {
            // TODO: accel *= strafe_speed;
        }

        if (input->move_dir >= 0) {
            const float yaw_origin = player->direction.y; // TODO: support different cam types?

            player->velocity.x += accel * cosf(move_dir - yaw_origin);
            player->velocity.z += accel * sinf(move_dir - yaw_origin);

            if (player->noclip) player->velocity.y += accel * player->direction.x * (input->move_dir > 2 && input->move_dir < 6 ? -1.0f : 1.0f);
        }

        // TODO: air strafes
        // TODO: xVelC, zVelC (apparently for cylinders?)

        if (player->velocity.x) {
            player->position.x += player->velocity.x * delta * 1000.0f; // TODO: multiply by game map config speed_x
            player->velocity.x *= powf(decel, delta * 1000.0f);
            player->velocity.x = CROP(player->velocity.x, game_constants.min_decel);
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
            player->velocity.z = CROP(player->velocity.z, game_constants.min_decel);
        }

        const int can_jump = player->on_ground && !player->did_jump;

        player->on_ground = player->noclip;
        player->on_ladder = 0;
        player->on_wall = 0;

        if (!player->noclip) player_do_map_collisions(player, can_jump, delta, recon);
    }
}
