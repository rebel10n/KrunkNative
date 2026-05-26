#include <shared.h>
#include <math.h>
#include <pcg_basic.h>
#include <stdio.h>

#ifdef KRUNKNATIVE_CLIENT
#include <client.h>
#endif

void player_melee(Player*);
void player_shoot(Player*);

Player *player_init(Game *game) {
    Player *player = calloc(1, sizeof(Player));
    if (!player) return NULL;

    player->game = game;
    player->slid_cont = 1;
    player->scale = game_constants.player_scale;

    return player;
}

void player_update_height(Player *player) {
    player->height = game_constants.player_height - player->crouch_val * game_constants.crouch_distance;
}

void player_spawn(Player *player) {
    player->active = 1;

    player->class_index = 0;
    player->speed = g_classes[0].speed;
    player->health = (float) g_classes[0].health;
    player->max_health = g_classes[0].health;

    player->loadout_size = 3;

    player->loadout = calloc(3, sizeof(int));
    player->ammo = calloc(3, sizeof(int));
    player->reloads = calloc(3, sizeof(float));

    player->loadout[0] = 1;
    player->loadout[1] = 2;
    player->loadout[2] = 12;

    player->ammo[0] = player->game->weapons[player->loadout[0]]->ammo;
    player->ammo[1] = player->game->weapons[player->loadout[1]]->ammo;
    player->ammo[2] = player->game->weapons[player->loadout[2]]->ammo;

    player->velocity.x = 0.0f;
    player->velocity.y = 0.0f;
    player->velocity.z = 0.0f;

    player->crouch_val = 0.0f;
    player->aim_val = 0.0f;

    player->on_ground = 1;
    player->on_wall = 0;
    player->on_terrain = 0;
    player->on_ladder = 0;

    player->can_slide = 1;
    player->can_throw = 1;

    player->did_jump = 0;
    player->did_wall_jump = 0;
    player->did_act = 0;

    player->air_time = 0.0f;
    player->covered_distance = 0.0f;
    player->aim_time = 0.0f;

    player->swap_timer = 0.0f;
    player->slide_timer = 0.0f;
    player->jump_timer = 0.0f;
    player->reload_timer = 0.0f;

    player_update_height(player);
    player_swap_weapon(player, 0, 1, 0, 0);

    if (player->game->map && player->game->map->spawn_count) {
        const Spawn *spawn = player->game->map->spawns[pcg32_boundedrand(player->game->map->spawn_count)];

        player->position = spawn->position;
        player->direction.x = 0.0f;
        player->direction.y = -(float) M_PI / 2.0f * (1.0f + (float) spawn->direction);
    }
}

void player_kill(Player *player, const Player *killer, PlayerKillInfo *kill_info, const int skip_rewards) {
    if (!player->active) return;

    player->active = 0;
}

void player_queue_input(Player *player, const Input *input) {
    Input *new_input_queue = realloc(player->input_queue, (player->input_queue_size + 1) * sizeof(Input));
    if (!new_input_queue) return;

    player->input_queue = new_input_queue;
    player->input_queue[player->input_queue_size++] = *input;
}

void player_reset_step(Player *player, const int recon) {
    // TODO: animate

    if (!player->game->config.can_slide || player->game->mode->config.real_movement || !player->crouch_val || !player->can_slide) return;

    player->can_slide = 0;

    player->slide_timer =
        game_constants.slide_time *
        player->game->config.slide_time *
        player->game->map->config.slide_time *
        player->crouch_val
    ;

    const float slide_mlt = (
        player->on_terrain ?
        game_constants.player_terrain_slide_velocity_mlt :
        game_constants.player_slide_velocity_mlt
    ) * player->game->map->config.slide_accel;

    player->velocity.x *= slide_mlt;
    player->velocity.z *= slide_mlt;
}

int player_collides(Player *player, const Object *object, const float pad) {
    switch (object->collision_type) {
        case COLLISION_TYPE_NONE:
            return 0;
        case COLLISION_TYPE_BOX: {
            const int in_x = player->position.x + player->scale > object->position.x - (object->scale.x * 0.5f + pad) && player->position.x - player->scale < object->position.x + (object->scale.x * 0.5f + pad);
            const int in_z = player->position.z + player->scale > object->position.z - (object->scale.z * 0.5f + pad) && player->position.z - player->scale < object->position.z + (object->scale.z * 0.5f + pad);
            const int in_y = player->position.y + player->height >= object->position.y && player->position.y <= object->position.y + object->scale.y + (player->game->config.disable_borders || !object->is_border ? 0.0f : game_constants.border_height);

            return in_x && in_y && in_z;
        }
        case COLLISION_TYPE_CYLINDER: {
            const int in_y = player->position.y + player->height > object->position.y && player->position.y < object->position.y + object->scale.y + (player->game->config.disable_borders || !object->is_border ? 0.0f : game_constants.border_height);
            return in_y && hypotf(player->position.x - object->position.x, player->position.z - object->position.z) <= object->scale.x * 0.5f + player->scale + pad;
        }
    }

    return 0;
}

void player_do_map_collisions(Player *player, const Input *input, const float move_dir, const int can_jump, const float delta, const int recon) {
    float *max_ramp_height = NULL;

    // TODO: grid arrays (perf)
    for (size_t i = 0; i < player->game->map->object_count; i++) {
        const Object *object = player->game->map->objects[i];
        const int collides_padded = player_collides(player, object, game_constants.collision_padding);

        if (!object->active || !collides_padded) {
            // TODO: in object
            continue;
        }

        const int collides = player_collides(player, object, 0.0f);

        // TODO: interactable's
        // TODO: collision trigger (onCollision, onTrigger)
        // TODO: onInteract
        // TODO: onEnter

        if (object->jump_pad) {
            const float bounce_vel = object->bounce * 0.1f * (player->crouch_val == 1.0f && object->crouch ? 0.5f : 1.0f);

            player->velocity.y = bounce_vel;
            player->on_ground = 0;

            // TODO: bounce sound
        } else if (object->score_zone) {
            // TODO: score
        } else if (
            !object->objective && !object->bomb_site &&
            (!object->premium || !1) && // TODO: check premium
            (!object->verified || !1) && // TODO: check verified
            (!object->team_zone || player->game->mode->config.teams && object->team != player->team)
        ) {
            if (object->teleporter) {
                // TODO: check teleport
                continue;
            }

            if (object->checkpoint) {
                // TODO: set checkpoint
                continue;
            }

            if (object->pickup) {
                // TODO: pickups
                continue;
            }

            if (object->flag) {
                // TODO: flags
                continue;
            }

            if (object->trigger) {
                // TODO: triggers
                continue;
            }

            if (object->kill && !player->god_mode) {
                // TODO: reset flag
                player_kill(player, NULL, NULL, 0);
                continue;
            }

            if (object->ladder) {
                if (player->position.y >= object->position.y + object->scale.y || player->crouch_val) continue;

                player->velocity.y = 0.0f;
                player->on_ladder = 1;
                player->on_terrain = 0;

                // TODO: step src

                if (input->move_dir >= 0) {
                    const float ladder_dir = (float) M_PI * 0.5f * (float) object->direction;
                    const float dir = (fabsf(normalize_angle(ladder_dir - (move_dir - player->direction.y))) - (float) M_PI * 0.5f) / ((float) M_PI * 0.5f);

                    if (dir > 0.0f) {
                        player->position.y += game_constants.ladder_speed * player->game->map->config.ladder_accel * player->weapon->speed_mlt * dir * delta;
                        player->position.y = CLAMP(player->position.y, object->position.y, object->position.y + object->scale.y);
                    }
                }

                continue;
            }

            if (!object->ramp && object->collision_type != COLLISION_TYPE_CYLINDER && !can_jump && object->position.y + object->scale.y - player->position.y > game_constants.climb_height && object->wall_jumpable) {
                if (player->last_position.x - player->scale >= object->position.x + object->scale.x * 0.5f - game_constants.collision_padding) {
                    player->on_wall = 1;
                } else if (player->last_position.x + player->scale <= object->position.x - object->scale.x * 0.5f + game_constants.collision_padding) {
                    player->on_wall = 2;
                } else if (player->last_position.z - player->scale >= object->position.z + object->scale.z * 0.5f - game_constants.collision_padding) {
                    player->on_wall = 3;
                } else if (player->last_position.z + player->scale <= object->position.z - object->scale.z * 0.5f + game_constants.collision_padding) {
                    player->on_wall = 4;
                }
            }

            if (!collides) continue;

            if (object->ramp) {
                if (player->position.y >= object->position.y + object->scale.y) continue;

                const float ramp_direction = (float) M_PI * 0.5f * (float) object->direction;
                const vec2 player_dir = {player->position.x + player->scale * cosf(ramp_direction), player->position.z + player->scale * sinf(ramp_direction)};

                const float progress = MAX(0.0f, MIN(1.0f, progress_on_line(object->ramp->start, object->ramp->end, player_dir)));
                const float collision_height = object->position.y + (object->scale.y + 0.01f) * progress;

                if (!max_ramp_height) {
                    max_ramp_height = calloc(1, sizeof(float));
                    if (max_ramp_height) *max_ramp_height = collision_height;
                }

                if ((player->position.y <= collision_height || can_jump) && (!max_ramp_height || *max_ramp_height <= collision_height)) {
                    if (max_ramp_height) *max_ramp_height = collision_height;

                    if (object->ramp->boost) {
                        player->position.y = collision_height;

                        const float boost_velocity = object->ramp->boost * game_constants.booster_speed * delta;
                        const float boost_direction = asinf(object->scale.y / hypotf(object->scale.y, object->scale.z));

                        player->velocity.x += boost_velocity * sinf(-ramp_direction + (float) M_PI * 0.5f) * cosf(boost_direction);
                        player->velocity.z += boost_velocity * cosf(-ramp_direction + (float) M_PI * 0.5f) * cosf(boost_direction);
                        player->velocity.y += boost_velocity * sinf(boost_direction);
                    } else {
                        if (player->last_position.y > player->position.y) player_reset_step(player, recon);

                        player->position.y = collision_height;
                        player->velocity.y = 0.0f;
                        player->on_wall = 0;
                        player->on_ground = 1;
                        player->on_terrain = 0;

                        if (!player->ramp_fix) player->ramp_fix = calloc(1, sizeof(float));

                        if (player->ramp_fix) {
                            player->on_ramp = 1;
                            *player->ramp_fix = object->position.y + object->scale.y * roundf(progress);
                        }
                    }
                }

                continue;
            }

            if (object->collision_type == COLLISION_TYPE_CYLINDER) {
                int y_collision = 1;

                if (player->last_position.y >= object->position.y + object->scale.y) {
                    if (player->last_position.y > player->position.y) ; // TODO: reset step

                    player->position.y = object->position.y + object->scale.y;
                    player->velocity.y = 0.0f;
                    player->on_ground = 1;
                    player->on_terrain = 0;
                } else if (player->last_position.y + player->height <= object->position.y) {
                    player->position.y = object->position.y - player->height;
                    player->velocity.y = 0.0f;
                } else if (player->position.y < object->position.y + object->scale.y && object->position.y + object->scale.y - player->position.y <= game_constants.climb_height && player->last_position.y < object->position.y + object->scale.y && can_jump) {
                    player->position.y += (object->position.y + object->scale.y - player->position.y) * 0.3f;
                    player->on_ground = 1;
                    player->on_terrain = 0;
                } else {
                    const float direction = atan2f(player->position.z - object->position.z, player->position.x - object->position.x);

                    player->position.x = object->position.x + (object->scale.x * 0.5f + player->scale) * cosf(direction);
                    player->position.z = object->position.z + (object->scale.x * 0.5f + player->scale) * sinf(direction);

                    y_collision = 0;
                }

                if (y_collision) {
                    const float direction = atan2f(player->position.z - object->position.z, player->position.x - object->position.x) - object->spin * delta * game_constants.disk_spin;
                    const float distance = hypotf(player->position.x - object->position.x, player->position.z - object->position.z);

                    const float old_x = player->position.x;
                    const float old_z = player->position.z;

                    player->position.x = object->position.x + distance * cosf(direction);
                    player->position.z = object->position.z + distance * sinf(direction);

                    player->x_vel_cylinder = player->position.x - old_x;
                    player->z_vel_cylinder = player->position.z - old_z;
                }

                continue;
            }

            if (
                (!object->is_border || player->game->config.disable_borders) &&
                player->position.y < object->position.y + object->scale.y &&
                object->position.y + object->scale.y - player->position.y <= game_constants.climb_height &&
                player->last_position.y < object->position.y + object->scale.y && can_jump
            ) {
                player->position.y += delta * 30.0f;
                player->position.y = MIN(player->position.y, object->position.y + object->scale.y);

                player->on_ground = 1;
                player->on_terrain = 0;
            } else if (player->last_position.y >= object->position.y + object->scale.y + (!object->is_border || player->game->config.disable_borders ? 0.0f : game_constants.border_height)) {
                // TODO: step src

                if (player->last_position.y > player->position.y) player_reset_step(player, recon);

                player->position.y = object->position.y + object->scale.y + (!object->is_border || player->game->config.disable_borders ? 0.0f : game_constants.border_height);
                player->velocity.y = 0.0f;
                player->on_ground = 1;
                player->on_terrain = 0;
            } else if (player->last_position.x - player->scale >= object->position.x + object->scale.x * 0.5f - 0.00001f) {
                player->position.x = object->position.x + object->scale.x * 0.5f + player->scale;
                player->velocity.x = 0.0f;
            } else if (player->last_position.x + player->scale <= object->position.x - object->scale.x * 0.5f + 0.00001f) {
                player->position.x = object->position.x - object->scale.x * 0.5f - player->scale;
                player->velocity.x = 0.0f;
            } else if (player->last_position.z - player->scale >= object->position.z + object->scale.z * 0.5f - 0.00001f) {
                player->position.z = object->position.z + object->scale.z * 0.5f + player->scale;
                player->velocity.z = 0.0f;
            } else if (player->last_position.z + player->scale <= object->position.z - object->scale.z * 0.5f + 0.00001f) {
                player->position.z = object->position.z - object->scale.z * 0.5f - player->scale;
                player->velocity.z = 0.0f;
            } else if (player->last_position.y + player->height <= object->position.y) {
                player->position.y = object->position.y - player->height;
                player->velocity.y = 0.0f;
            }
        }
    }

    free(max_ramp_height);
}

void player_jump(Player *player) {
    player->did_act = 1;
    player->jump_timer = (player->terrain_slipping ? game_constants.player_slipping_jump_cooldown : 0.0f) * player->game->map->config.jump_cooldown;
    player->did_jump = 1;
    player->did_wall_jump = 1;
    player->on_terrain = 0;

    const float jump_push = game_constants.jump_push * player->game->config.jump_mlt;
    const float jump_vel = game_constants.jump_velocity * player->game->config.jump_mlt * (player->game->mode->config.real_movement ? 0.92f : 1.0f);
    const float vel = hypotf(player->velocity.x, player->velocity.z);

    player->velocity.y += jump_vel * (1 - game_constants.crouch_jump * player->crouch_val) * (player->weapon->jump_mlt ? player->weapon->jump_mlt : player->weapon->speed_mlt) * (player->aim_val != 1.0f ? 1.0f : game_constants.jump_aim_slow);

    player->velocity.x -= vel * jump_push * sinf(player->direction.y);
    player->velocity.z -= vel * jump_push * cosf(player->direction.y);
}

void player_swap_weapon(Player *player, const int index, const int force_swap, const int instant_swap, const int recon) {
    int loadout_changed = force_swap || player->loadout_index != index;
    player->loadout_index = index;

    if (!instant_swap && loadout_changed) {
        player->reload_timer = 0.0f;
        player->did_shoot = 0;
        player->burst_count = 0;
    }

    player->weapon = player->game->weapons[player->loadout[player->loadout_index]];

    if (!player->weapon) {
        player->weapon = player->game->weapons[player->loadout[0]];

        if (player->weapon) {
            player->loadout_index = 0;
            loadout_changed = 1;
        }
    }

#ifdef KRUNKNATIVE_CLIENT
    // TODO: update inspect, weapon meshes

    const PlayerMesh *player_mesh = player->mesh;

    if (player_mesh) {
        for (size_t i = 0; i < player->loadout_size; i++) {
            const PlayerArms *arms = player_mesh->arms[i];
            const int show = i == player->loadout_index;

            if (arms->weapon_right) arms->weapon_right->visible = show;
            if (arms->weapon_left) arms->weapon_left->visible = show;

            for (int j = 0; j < 2; j++) {
                const PlayerArmMesh *arm = j ? arms->left : arms->right;

                if (arm->extender) arm->extender->visible = show;

                if (arm->upper) arm->upper->visible = show;
                if (arm->joint) arm->joint->visible = show;

                for (size_t k = 0; k < arm->lower->mesh_count; k++) {
                    arm->lower->meshes[k]->visible = show;
                }
            }
        }
    }
#endif

    if (!instant_swap && player->weapon) {
        if (!recon && loadout_changed) player->swap_timer = player->weapon->swap_time;

        // TODO: animations & HUD
    }
}

void player_reload(Player *player) {
    if (player->reload_timer || player->ammo[player->loadout_index] >= player->weapon->ammo) return;

    player->reload_timer = player->weapon->reload_time * player->game->config.reload_speed;
    player->burst_count = 0;

    // TODO: cancel inspect
}

void player_cancel_inspect(Player *player) {
    player->inspecting = 0;
    // TODO
}

void player_inspect_weapon(Player *player) {
    if (player->inspecting) return player_cancel_inspect(player);
    if (player->weapon->no_inspect || player->aim_val || player->reload_timer || (player->weapon->melee && !player->can_throw)) return;

    player->inspecting = 1;

    if (player->weapon->melee) {
        // TODO: melee inspect
    } else {
        // TODO
    }
}

void player_proc_input(Player *player, const Input *input, const int recon, const int move_lock) {
    // const float delta = CLAMP(input->delta, game_constants.min_delta, game_constants.max_delta);
    const float delta = MIN(input->delta, game_constants.max_delta);
    const float move_dir = -(float) M_PI / 2.0f + (float) M_PI / 4.0f * (float) input->move_dir;

    if (player->noclip) player->on_ground = 1;

    const float pitch_delta = normalize_angle(input->x_dir - player->direction.x);
    const float yaw_delta = normalize_angle(input->y_dir - player->direction.y);

    // TODO: spin distance (unused?)

#ifdef KRUNKNATIVE_CLIENT
    if (!recon && player->render_you) {
        player->lean_anim.x -= yaw_delta * game_constants.lean_sensitivity;
        player->lean_anim.x = CLAMP(player->lean_anim.x, -game_constants.lean_max, game_constants.lean_max);

        player->lean_anim.y -= pitch_delta * game_constants.lean_sensitivity;
        player->lean_anim.y = CLAMP(player->lean_anim.x, -game_constants.lean_max, game_constants.lean_max);

        if (player->lean_anim.x) player->lean_anim.x *= powf(game_constants.lean_pull, delta * 1000.0f);
        if (player->lean_anim.y) player->lean_anim.y *= powf(game_constants.lean_pull, delta * 1000.0f);
        if (player->lean_anim.z) player->lean_anim.z *= powf(game_constants.lean_pull_z, delta * 1000.0f);

        if (player->bob_anim.y) player->bob_anim.y *= powf(game_constants.bob_pull_y, delta * 1000.0f);
        if (player->bob_anim.z) player->bob_anim.z *= powf(game_constants.bob_pull_z, delta * 1000.0f);

        if (player->recoil.x) player->recoil.x *= powf(game_constants.lean_pull, delta * 1000.0f);
        if (player->recoil.z) player->recoil.z *= powf(game_constants.lean_pull, delta * 1000.0f);

        // TODO: inspect x
    }
#endif

    player->direction.x = CLAMP(input->x_dir, -(float) M_PI / 2.0f, (float) M_PI / 2.0f);
    player->direction.y = input->y_dir;

    if (input->swap && player->loadout_size > 1) {
        const int swap_secondary = input->swap == 1;
        const int swap_melee = input->swap == 2;
        const int swap_equipment = input->swap == 3;

        int swap_to = -1;

        for (int i = 0; i < player->loadout_size; i++) {
            const int weapon = player->loadout[i];

            if (swap_secondary && player->game->weapons[weapon]->secondary) {
                swap_to = i;
                break;
            }

            if (swap_melee && player->game->weapons[weapon]->melee) {
                swap_to = i;
                break;
            }

            if (swap_equipment && player->game->weapons[weapon]->equipment) {
                swap_to = i;
                break;
            }
        }

        if (swap_to >= 0) {
            if (player->loadout_index == swap_to) swap_to = 0;
            player_swap_weapon(player, swap_to, 0, 0, recon);
        }
    }

    if (!recon) {
        if (player->recoil_force) {
            player->recoil_anim += player->recoil_force * delta;
            player->recoil_anim_y += player->recoil_force * (!player->weapon->recoil_y ? 1.0f : player->weapon->recoil_y) * (1.0f - player->crouch_val * 0.3f) * delta;
            player->recoil_force *= powf(player->weapon->recover_f, delta * 1000.0f);
        }

        if (player->recoil_anim) player->recoil_anim *= powf(player->weapon->recover, delta * 1000.0f);
        if (player->recoil_anim_y) player->recoil_anim_y *= powf(player->weapon->recover_y ? player->weapon->recover_y : player->weapon->recover, delta * 1000.0f);
    }

    player->last_position = player->position;

    if (player->weapon->no_aim && player->aim_val > 0.0f) {
        // TODO: toggle aim (HUD)
        player->aim_val = 0.0f;
    } else if (player->weapon->zoom && (!player->weapon->no_aim || player->swap_timer > 0.0f)) {
        const int can_scope = player->reload_timer <= 0.0f && player->swap_timer <= 0.0f && (!player->weapon->melee || (player->can_throw && player->game->config.throwable_melees));

        if (input->scope && player->aim_val < 1.0f && can_scope) {
            // TODO: cancel inspect animation
            player->aim_val += 1.0f / player->weapon->aim_speed * delta;
            player->aim_val = MIN(1.0f, player->aim_val);

            if (player->aim_val == 1.0f && !recon) {
                // TODO: toggle aim (HUD)
            }
        } else if (!can_scope || !input->scope && player->aim_val > 0.0f) {
            player->aim_val -= 1.0f / player->weapon->aim_speed * delta;
            player->aim_val = MAX(0.0f, player->aim_val);

            if (!player->aim_val && !recon) {
                // TODO: toggle aim (HUD)
            }
        }

        // TODO: update aim (HUD)

        if (player->aim_val == 1.0f) {
            player->aim_time += delta;
        } else {
            player->aim_time = 0.0f;
        }
    }

    if (input->crouch && player->crouch_val < 1.0f && !player->on_ladder) {
        player->crouch_val = MIN(1.0f, player->crouch_val + game_constants.crouch_speed * delta);

        if (player->on_ground) {
#ifdef KRUNKNATIVE_CLIENT
            if (!recon) player->bob_anim.y -= game_constants.crouch_anim * 1.4f * delta;
#endif
        } else {
            player->position.y += game_constants.crouch_speed * delta;
        }
    } else if (!input->crouch && player->crouch_val > 0.0f) {
        player->crouch_val = MAX(0.0f, player->crouch_val - game_constants.crouch_speed * delta);

        if (player->on_ground) {
#ifdef KRUNKNATIVE_CLIENT
            if (!recon) player->bob_anim.y += game_constants.crouch_anim * delta;
#endif
        } else {
            player->position.y -= game_constants.crouch_speed * delta;
        }
    }

    player_update_height(player);

    const int contact = player->on_ground || player->on_ladder;

    if (!move_lock) {
        float accel;

        if (contact) accel = (player->terrain_slipping ? game_constants.slipping_speed : game_constants.player_speed) * player->speed;
        else accel = game_constants.air_speed * player->game->map->config.air_accel * (player->game->mode->config.real_movement ? 0.72f : 1.0f);

        accel *= player->aim_val == 1.0f ? game_constants.aim_slow : 1.0f;
        accel *= player->crouch_val ? game_constants.crouch_slow : 1.0f;
        accel *= player->game->mode->config.speed_mlt[player->team];
        accel *= player->weapon->speed_mlt;
        accel *= (player->noclip ? 2.0f : 1.0f) * delta;

        float decel;

        if (player->on_ladder) decel = game_constants.ladder_decel;
        else if (player->terrain_slipping) decel = game_constants.terrain_slip_decel;
        else if (player->on_terrain) decel = game_constants.terrain_decel;
        else if (player->on_ground) decel = game_constants.ground_decel;
        else decel = game_constants.air_decel;

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
                    const float angle_delta = normalize_angle(vel_dir - dir) * 0.18f;

                    player->velocity.x = vel * cosf(vel_dir + (float) M_PI - angle_delta);
                    player->velocity.z = vel * sinf(vel_dir + (float) M_PI - angle_delta);
                }
            }
        }

        player->jump_timer = MAX(0.0f, player->jump_timer - delta);

        if (player->jump_timer <= 0.0f && (player->on_ground || player->game->map->config.infinite_jump)) {
            if (player->did_jump && !input->jump) player->did_jump = 0;
            if (input->jump && (!player->did_jump || player->game->config.auto_jump)) {
                player_jump(player);
            }
        }

        if (!contact) {
            const float wall_mlt = player->velocity.y < 0.0f && player->wall_jump && player->on_wall && player->game->config.wall_jump > 0.0f && player->crouch_val ? 0.3f : 1.0f;
            player->velocity.y -= delta * game_constants.gravity * player->game->config.gravity_mlt * wall_mlt;
        }

        // TODO: accel *= CLAMP(1.0f - inputs[12], 0.0f, 1.0f); "what?" - joe biden

        if (input->move_dir > 0 && input->move_dir % 2) {
            // TODO: accel *= strafe_speed;
        }

        if (input->move_dir >= 0) {
            const float yaw_origin = player->direction.y; // TODO: support different cam types?

            player->velocity.x += accel * cosf(move_dir - yaw_origin);
            player->velocity.z += accel * sinf(move_dir - yaw_origin);

            if (player->noclip) player->velocity.y += accel * player->direction.x * (input->move_dir > 2 && input->move_dir < 6 ? -1.0f : 1.0f);
        }

        // TODO: air strafes

        if (!contact) {
            if (player->x_vel_cylinder) {
                player->velocity.x += player->x_vel_cylinder * 0.07f;
                player->x_vel_cylinder = 0.0f;
            }

            if (player->z_vel_cylinder) {
                player->velocity.z += player->z_vel_cylinder * 0.07f;
                player->z_vel_cylinder = 0.0f;
            }
        }

        if (player->velocity.x) {
            player->position.x += player->velocity.x * player->game->map->config.speed.x * delta * 1000.0f;
            player->velocity.x *= powf(decel, delta * 1000.0f);
            player->velocity.x = CROP(player->velocity.x, game_constants.min_decel);
        }

        if (player->velocity.y) {
            player->position.y += player->velocity.y * player->game->map->config.speed.y * delta * 1000.0f;

            if (player->noclip) {
                player->velocity.y *= powf(decel, delta * 1000.0f);
            } else if (player->velocity.y > 0.0f) {
                player->velocity.y -= delta * 0.032f;
            } else if (player->velocity.y < -0.3f) {
                player->velocity.y = -0.3f;
            }
        }

        if (player->velocity.z) {
            player->position.z += player->velocity.z * player->game->map->config.speed.z * delta * 1000.0f;
            player->velocity.z *= powf(decel, delta * 1000.0f);
            player->velocity.z = CROP(player->velocity.z, game_constants.min_decel);
        }

        const int can_jump = player->on_ground && !player->did_jump;

        player->on_ground = player->noclip;
        player->on_ladder = 0;
        player->on_wall = 0;
        player->on_ramp = 0;

        if (!player->noclip) player_do_map_collisions(player, input, move_dir, can_jump, delta, recon);

        if (!player->did_jump && player->ramp_fix && fabsf(player->position.y - *player->ramp_fix) <= game_constants.climb_height) {
            if (!player->on_ramp) {
                player->position.y = *player->ramp_fix;
                player->on_ground = 1;
                player->velocity.y = 0.0f;

                free(player->ramp_fix);
                player->ramp_fix = NULL;
            }
        } else {
            free(player->ramp_fix);
            player->ramp_fix = NULL;
        }

        // TODO: inter prog
        // TODO: terrain ray cast

        player->air_time = player->on_ground ? 0 : player->air_time + delta;
    }

    const float delta_pos2D = hypotf(player->position.x - player->last_position.x, player->position.z - player->last_position.z);
    player->covered_distance += delta_pos2D;

    if (!recon && 1 /* TODO: check that teamOptions != prop */ && player->game->map->config.model != MODEL_TYPE_SPRITE) {
        const float delta_pos = sqrtf(
            powf(player->position.x - player->last_position.x, 2.0f) +
            powf(player->position.y - player->last_position.y, 2.0f) +
            powf(player->position.z - player->last_position.z, 2.0f)
        ) * (player->render_you && player->on_ladder ? 1.4f : 1.0f);

        if (player->render_you) {
            player->bob_anim.z += delta_pos2D * game_constants.bob_mlt_z;
            player->bob_anim.y -= (player->last_position.y - player->position.y) * game_constants.bob_mlt_y;
        }

        // TODO: play step sound

        if (player->render_you) {
            if (contact && input->move_dir >= 0) {
                player->lean_anim.z -= delta_pos * game_constants.lean_mlt_z * (player->weapon->z_lean_mlt ? player->weapon->z_lean_mlt : 1.0f) * cosf(move_dir);
            } else {
                player->step_val *= powf(game_constants.step_pull, delta * 1000.0f);
            }

            if (player->step_chase != player->step_val) player->step_chase += (player->step_val - player->step_chase) * 0.15f;
        }

        // TODO: update spread

        if (input->reload && !player->game->mode->config.no_reloads) {
            player_reload(player);
        }

        if (player->reload_timer > 0.0f) {
            player->reload_timer = MAX(0.0f, player->reload_timer - delta);

            // TODO: play reload end sound

            if (!player->reload_timer) {
                player->did_shoot = 0;
                player->ammo[player->loadout_index] = player->weapon->ammo;
            }
        }

        player->swap_timer = MAX(0.0f, player->swap_timer - delta);

        for (int i = 0; i < player->loadout_size; i++) {
            player->reloads[i] = MAX(0.0f, player->reloads[i] - delta);
        }

        if (player->weapon && !move_lock && !0 /* TODO: doing interact */) {
            int will_shoot = player->weapon->burst_count || !player->weapon->no_auto && input->shoot;

            if (player->did_shoot && !input->shoot) {
                player->did_shoot = 0;
            }

            if (!player->did_shoot && input->shoot) {
                will_shoot = 1;
            }

            if (will_shoot && player->reloads[player->loadout_index] <= 0.0f && player->swap_timer <= 0.0f && player->reload_timer <= 0.0f) {
                if (player->weapon->melee) {
                    player_melee(player);
                } else {
                    if (player->ammo[player->loadout_index] > 0) {
                        player_shoot(player);
                    } else if (!player->game->mode->config.no_reloads) {
                        player_reload(player);
                    }
                }
            }
        }

        // TODO: shooting
    }

    // TODO: vel obj

    if (!move_lock && player->wall_jump && !player->on_ground && !player->on_ladder && player->on_wall && player->game->config.wall_jump > 0.0f) {
        if (player->did_wall_jump && !input->jump) player->did_wall_jump = 0;

        if (!player->did_wall_jump && input->jump) {
            player->did_wall_jump = 1;
            // TODO: animate

            const float velocity = (player->game->mode->config.real_movement ? 0.7f : 1.0f) * 0.03f;
            player->velocity.y = (player->game->mode->config.real_movement ? 0.9f : 1.0f) * 0.058f;

            switch (player->on_wall) {
                case 1:
                    player->velocity.x = velocity * player->game->config.wall_jump;
                    break;
                case 2:
                    player->velocity.x = -velocity * player->game->config.wall_jump;
                    break;
                case 3:
                    player->velocity.z = velocity * player->game->config.wall_jump;
                    break;
                case 4:
                    player->velocity.z = -velocity * player->game->config.wall_jump;
                    break;
                default:
                    break;
            }

            player->on_wall = 0;
        }
    }

    // TODO: trading?
    // TODO: player collisions
    // TODO: update interact
}

void player_step(Player *player, const float distance) {

}

void player_melee(Player *player) {
    const int is_throw = player->can_throw && player->weapon->can_throw && player->aim_val == 1.0f;

    player->reloads[player->loadout_index] = player->weapon->rate * player->game->config.fire_rate;
    player->did_shoot = 1;
    player->did_act = 1;

    // TODO: gamepad rumble?
    // TODO: animate
    // TODO: play sound
    // TODO: cancel inspect

    const float shot_height = player->position.y + player->height - game_constants.camera_height;
    vec2 shot_dir = {};

    if (is_throw) {
        player->can_throw = !!player->unlimited_ammo;
        // TODO: init projectile
    } else {
        // TODO: hitscan
    }
}

typedef struct {
    const Object *object;
    float t;
} Hit;

void player_shoot(Player *player) {
    // TODO: increase shots stat

    if (!player->unlimited_ammo) player->ammo[player->loadout_index]--;

    player->did_shoot = 1;
    player->did_act = 1;

    if (player->burst_count) player->burst_count--;
    else player->burst_count = player->weapon->burst ? player->weapon->burst_count - 1 : 0;

    player->reloads[player->loadout_index] = (player->burst_count && player->weapon->burst ? player->weapon->burst_rate : player->weapon->rate) * player->game->config.fire_rate;

    // TODO: update player ammo (HUD)
    // TODO: play sound

    player->recoil_force += player->weapon->recoil;

    // TODO: gamepad rumble?

#ifdef KRUNKNATIVE_CLIENT
    const float rand_recoil = ((float) pcg32_random() / (float) UINT32_MAX - 0.5f) * 2.0f * (float) M_PI;

    player->recoil.x += player->weapon->recoil_r * sinf(rand_recoil);
    player->recoil.z += player->weapon->recoil_r * 0.3f * cosf(rand_recoil);

    // TODO: recoil tween
#endif

    // TODO: cancel inspect
    // TODO: muzzle flash & casings

    const int is_projectile = player->weapon->projectile && (!player->weapon->projectile_disable || player->game->config.bullet_drop);
    const float shot_height = player->position.y + player->height - game_constants.camera_height;

    vec2 shot_angles = {};

    if (is_projectile) {
        const float projectile_spread = 0.0f; // TODO
        const float spread = (player->spread + player->weapon->inaccuracy) * game_constants.spread_adjustment * projectile_spread;

        shot_angles.x = player->direction.x + player->recoil_anim_y * game_constants.recoil_mlt + spread;
        shot_angles.y = player->direction.y + spread;

        // TODO: init projectile
    }

    if (!is_projectile || player->weapon->physical_power) {
        for (int i = player->weapon->physical_power ? -1 : 0; i < (player->weapon->shots ? player->weapon->shots : 1); i++) {
            if (player->weapon->custom_spread && i >= 0) {
                const unsigned int offset = player->ammo[player->loadout_index] * player->weapon->shots;

                const vec2 spread = player->weapon->custom_spread[offset + i];
                const float spread_mlt = (float) pcg32_random() / (float) UINT32_MAX * 0.02f + 0.3f;

                // swizzle spread (x is pitch, y is yaw)
                shot_angles.x = player->direction.x + spread.y * spread_mlt;
                shot_angles.y = player->direction.y + spread.x * spread_mlt;
            } else {
                const float spread_range = i >= 0 ? (player->spread + player->weapon->inaccuracy) * game_constants.spread_adjustment : 0.0f;
                const vec2 spread = {
                    ((float) pcg32_random() / (float) UINT32_MAX - 0.5f) * 2.0f * spread_range,
                    ((float) pcg32_random() / (float) UINT32_MAX - 0.5f) * 2.0f * spread_range,
                };

                shot_angles.x = player->direction.x + spread.x;
                shot_angles.y = player->direction.y + spread.y;
            }

            shot_angles.x += player->recoil_anim_y * game_constants.recoil_mlt;

            const float range = i < 0 ? player->weapon->physical_range : player->weapon->range;

            const vec3 shot_origin = {player->position.x, shot_height, player->position.z};
            const vec3 shot_dir = {
                range * sinf(shot_angles.y + (float) M_PI) * cosf(shot_angles.x),
                range * sinf(shot_angles.x),
                range * cosf(shot_angles.y + (float) M_PI) * cosf(shot_angles.x),
            };

            size_t hit_count = 0;
            Hit *hits = NULL;

            for (size_t j = 0; j < player->game->map->object_count; j++) {
                const Object *object = player->game->map->objects[j];
                const float t = line_in_rect(shot_origin, shot_dir, object->position, object->scale);

                if (t < 0.0f || t > 1.0f) continue;

                if (!hit_count) {
                    hits = calloc(1, sizeof(Hit));
                    if (hits) hit_count = 1;
                } else {
                    Hit *new_hits = realloc(hits, (hit_count + 1) * sizeof(Hit));
                    if (new_hits) {
                        hits = new_hits;
                        hit_count++;
                    } else {
                        free(hits);
                        hits = NULL;
                    }
                }

                if (!hits) continue;

#ifdef KRUNKNATIVE_CLIENT
                if (object->mesh) ((Mesh *) object->mesh)->material->wireframe = 1;
#endif

                hits[hit_count - 1] = (Hit) {object, t};
            }

            free(hits);
        }
    }
}

void player_update(Player *player, const float delta) {
    if (!player->active) return;

    if (player->input_queue_size) {
        for (size_t i = 0; i < player->input_queue_size; i++) {
            player_proc_input(player, &player->input_queue[i], 0, player->game->move_lock);
            player->input_seq = player->input_queue[i].seq;
        }

        free(player->input_queue);

        player->input_queue_size = 0;
        player->input_queue = NULL;
    }

    player->idle_anim += game_constants.idle_anim_speed * delta;

    if (player->hp_chase > player->health / (float) player->max_health) {
        player->hp_chase = MAX(0.0f, player->hp_chase - delta * 0.2f);
    } else {
        player->hp_chase = player->health / (float) player->max_health;
    }

    if (player->interpolate) {
        player->dt += delta;

        const float progress = MIN(1.6f, player->dt * player->send_rate / game_constants.interpolation) / player->game->config.delta_mlt;

        player->last_position = player->position;

        player->position.x = player->interp_pos_start.x + (player->interp_pos_end.x - player->interp_pos_start.x) * progress;
        player->position.y = player->interp_pos_start.y + (player->interp_pos_end.y - player->interp_pos_start.y) * progress;
        player->position.z = player->interp_pos_start.z + (player->interp_pos_end.z - player->interp_pos_start.z) * progress;

        if (player->on_ground) {
            player_step(player, hypotf(player->last_position.x - player->position.x, player->last_position.z - player->position.z));
        }

        // TODO: linear interpolate direction change
    }
}
