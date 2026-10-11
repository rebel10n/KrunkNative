#include <shared.h>
#include <math.h>
#include <pcg_basic.h>
#include <stdio.h>
#include <string.h>

// Krunker v3.1.1 combat constants (src/config.js), used by the spread update and
// the hitscan/damage code below.
#define COMBAT_HEAD_SCALE 2.0f      // config.headScale: headshot band below the top of the hitbox
#define COMBAT_LEG_HEIGHT 4.2f      // config.legHeight: leg shots deal half damage
#define COMBAT_DENSITY_FLESH 0.2f   // config.materialDens.flesh
#define COMBAT_DENSITY_DEFAULT 0.5f // config.materialDens.default
#define COMBAT_FIST_DAMAGE 35.0f    // melee without the knife (after a throw)
#define COMBAT_SPREAD_MOVE 25.0f    // config.spreadMove
#define COMBAT_SPREAD_FALL 30.0f    // config.spreadFall
#define COMBAT_SPREAD_RECOVER 0.985f// config.spreadRecover (per ms)
#define COMBAT_SPREAD_MLT 240.0f    // config.spreadMlt (recoilAnim term)
#define COMBAT_CROUCH_SPREAD 0.55f  // config.crouchSpread


#ifdef KRUNKNATIVE_CLIENT
#include <client.h>
#endif

void player_melee(Player*);
void player_shoot(Player*);
void player_step(Player*, float);
void player_cancel_inspect(Player*);

Player *player_init(Game *game) {
    Player *player = calloc(1, sizeof(Player));
    if (!player) return NULL;

    player->game = game;
    player->slid_cont = 1;
    player->scale = game_constants.player_scale;
    player->last_killer_uid = -1;
    player->secondary_index = -1;
    player->cosmetics = PLAYER_COSMETICS_NONE;
    player->last_kill_time = -1e6f; // Krunker lastKill starts far in the past (no multi-kill on the first kill)
    for (int i = 0; i < PLAYER_DAMAGE_SLOTS; i++) player->damage_received[i].uid = -1;

    return player;
}

void player_update_height(Player *player) {
    player->height = game_constants.player_height - player->crouch_val * game_constants.crouch_distance;
}

void player_spawn(Player *player) {
    free(player->loadout);
    free(player->ammo);
    free(player->reloads);
    free(player->input_queue);

    player->loadout = NULL;
    player->ammo = NULL;
    player->reloads = NULL;
    player->input_queue = NULL;
    player->input_queue_size = 0;

    player->active = 1;

    const size_t class_count = player->game->class_count ? player->game->class_count : sizeof(g_classes) / sizeof(g_classes[0]);
    const ClassConfig *classes = player->game->classes ? player->game->classes : g_classes;
    const size_t weapon_count = player->game->weapon_count ? player->game->weapon_count : sizeof(g_weapons) / sizeof(g_weapons[0]);
    Weapon **weapons = player->game->weapons ? player->game->weapons : (Weapon**) g_weapons;

    // setClass (v3.1.1 src/data/players.js): mode overrides, then primary + optional secondary + knife.
    if (player->game->mode) {
        const GameModeConfig *mode_config = &player->game->mode->config;
        if (mode_config->no_weapons) player->class_index = 9;
        if (mode_config->force_class >= 0) player->class_index = mode_config->force_class;
        if (player->team >= 0 && player->team < 3 && mode_config->team_class[player->team] >= 0) player->class_index = mode_config->team_class[player->team];
    }

    if (player->class_index < 0 || player->class_index >= (int) class_count) {
        player->class_index = 0;
    }

    const ClassConfig *class_config = &classes[player->class_index];
    int loadout[3] = {1, WEAPON_ID_KNIFE, WEAPON_ID_KNIFE};
    int loadout_size = 0;
    const int primary = class_config->loadout ? class_config->loadout[0] : 1;
    const int fallback_weapon = weapon_count > 1 ? 1 : 0;
    const int primary_index = primary >= 0 && primary < (int) weapon_count && weapons[primary] ? primary : fallback_weapon;
    // setClass(game, classIndex, secIndex): the picked secondary when it is a secondary weapon, else the Pistol.
    const int picked_secondary = player->secondary_index;
    const int secondary_index = picked_secondary >= 0 && picked_secondary < (int) weapon_count && weapons[picked_secondary] && weapons[picked_secondary]->secondary
        ? picked_secondary : WEAPON_ID_PISTOL < (int) weapon_count ? WEAPON_ID_PISTOL : -1;
    const int melee_index = WEAPON_ID_KNIFE < (int) weapon_count ? WEAPON_ID_KNIFE : primary_index;

    if (primary != primary_index) {
        fprintf(stderr, "player_spawn: class %d (%s) primary weapon %d is not registered, using %d\n", player->class_index, class_config->name, primary, primary_index);
    }

    loadout[loadout_size++] = primary_index;
    if (class_config->secondary && !player->game->config.no_secondaries && secondary_index >= 0 && secondary_index != primary_index) loadout[loadout_size++] = secondary_index;
    if (melee_index != primary_index && loadout_size < (int) (sizeof(loadout) / sizeof(loadout[0]))) loadout[loadout_size++] = melee_index;

    const int mode_health = player->game->mode ? player->game->mode->config.health : 0;
    const float health_mlt = player->game->config.health_mlt > 0.0f ? player->game->config.health_mlt : 1.0f;

    player->speed = class_config->speed ? class_config->speed : 1.0f;
    player->max_health = mode_health > 0 ? mode_health : (int) ((float) class_config->health * health_mlt);
    player->health = (float) player->max_health;
    player->wall_jump = class_config->wall_jump;
    player->loadout_size = loadout_size;

    player->loadout = calloc((size_t) loadout_size, sizeof(int));
    player->ammo = calloc((size_t) loadout_size, sizeof(int));
    player->reloads = calloc((size_t) loadout_size, sizeof(float));

    if (!player->loadout || !player->ammo || !player->reloads) {
        free(player->loadout);
        free(player->ammo);
        free(player->reloads);
        player->loadout = NULL;
        player->ammo = NULL;
        player->reloads = NULL;
        player->loadout_size = 0;
        player->active = 0;
        return;
    }

    for (int i = 0; i < loadout_size; i++) {
        player->loadout[i] = loadout[i];
        const int ammo_limit = player->game->mode ? player->game->mode->config.ammo_limit : 0;
        player->ammo[i] = ammo_limit > 0 && weapons[player->loadout[i]]->ammo ? ammo_limit : (int) weapons[player->loadout[i]]->ammo;
    }

    player->velocity.x = 0.0f;
    player->velocity.y = 0.0f;
    player->velocity.z = 0.0f;

    player->crouch_val = 0.0f;
    player->aim_val = 0.0f;
    player_reset_feel(player); // Player.reset: step/bob/lean/recoil feel from rest

    player->on_ground = 1;
    player->on_wall = 0;
    player->on_terrain = 0;
    player->on_ladder = 0;

    player->can_slide = 1;
    player->can_throw = 1;
    player->in_object = 0;

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

    // shared/src/modes: the mode's loadout (Gun Game tier, Sharp Shooter, Stalker) and,
    // on the authority, the team spawn point (getSpawnPoint(team)).
    mode_player_spawned(player->game, player);
}

void player_queue_input(Player *player, const Input *input) {
    Input *new_input_queue = realloc(player->input_queue, (player->input_queue_size + 1) * sizeof(Input));
    if (!new_input_queue) return;

    player->input_queue = new_input_queue;
    player->input_queue[player->input_queue_size++] = *input;
}

// ---------------------------------------------------------------- movement feel
// Krunker v3.1.1 first-person feel (src/data/players.js playerStep / resetStep / jump,
// the wall jump in procInputs, and the TWEEN.js tweens they start on landBobY,
// jumpBobY and jumpRot). Only the local, non-recon player animates; the tweens run in
// real (unclamped) frame time like TWEEN.update() in index.js gameLoop.

#define MOVEMENT_STEP_MLT 0.25f      // config.stepMlt
// Krunker timers count integer ms in doubles; KrunkNative counts float seconds, whose
// residue after N subtractions (~1e-7 s) must not keep a timer alive for one more tick.
#define MOVEMENT_TIMER_EPSILON 1e-5f

// TWEEN.Easing (public/libs/Tween.min.js)
static float player_ease(const int ease, const float k) {
    switch (ease) {
        case PLAYER_EASE_BACK_OUT: {
            const float t = k - 1.0f;
            return t * t * (2.70158f * t + 1.70158f) + 1.0f;
        }
        case PLAYER_EASE_BACK_IN_OUT: {
            const float n = 2.5949095f;
            float t = k * 2.0f;
            if (t < 1.0f) return t * t * ((1.0f + n) * t - n) * 0.5f;
            t -= 2.0f;
            return 0.5f * (t * t * ((1.0f + n) * t + n) + 2.0f);
        }
        case PLAYER_EASE_CUBIC_OUT: {
            const float t = k - 1.0f;
            return t * t * t + 1.0f;
        }
        case PLAYER_EASE_SINUSOIDAL_OUT:
            return sinf(k * (float) M_PI * 0.5f);
        case PLAYER_EASE_ELASTIC_OUT:
            if (k <= 0.0f) return 0.0f;
            if (k >= 1.0f) return 1.0f;
            return powf(2.0f, -10.0f * k) * sinf(5.0f * (k - 0.1f) * (float) M_PI) + 1.0f;
        default:
            return k;
    }
}

static void player_tween_start(PlayerTween *tween, const float from, const float to, const float duration, const PlayerEase ease) {
    *tween = (PlayerTween) {
        .start = from,
        .end = to,
        .duration = duration,
        .ease = (unsigned char) ease,
        .active = 1,
    };
}

// TWEEN.Tween(o).to({v: [via, end]}): Interpolation.Linear over [start, via, end].
static void player_tween_start_via(PlayerTween *tween, const float from, const float via, const float to, const float duration, const PlayerEase ease) {
    player_tween_start(tween, from, to, duration, ease);
    tween->via = via;
    tween->has_via = 1;
}

static float player_tween_value(const PlayerTween *tween, const float k) {
    const float e = player_ease(tween->ease, k);
    if (!tween->has_via) return tween->start + (tween->end - tween->start) * e;

    const float points[3] = {tween->start, tween->via, tween->end};
    const float f = 2.0f * e;
    if (e < 0.0f) return points[0] + (points[1] - points[0]) * f;
    if (e > 1.0f) return points[2] + (points[1] - points[2]) * (2.0f - f);

    const int i = (int) floorf(f);
    const int j = i + 1 > 2 ? 2 : i + 1;
    return points[i] + (points[j] - points[i]) * (f - (float) i);
}

// Returns 1 when the tween finished during this step.
static int player_tween_advance(PlayerTween *tween, float *value, const float delta) {
    if (!tween->active) return 0;

    tween->time += delta;
    const float k = tween->duration > 0.0f ? MIN(1.0f, tween->time / tween->duration) : 1.0f;
    *value = player_tween_value(tween, k);

    if (k < 1.0f) return 0;
    tween->active = 0;
    return 1;
}

// meleeAnim.anim: TWEEN start() captures the masked channels (even with a delay), update()
// eases them toward the step's targets, and onComplete starts `then` at the completing frame.
static void player_melee_anim_capture(Player *player, const MeleeAnimStep *step) {
    for (int i = 0; i < MELEE_ANIM_CHANNELS; i++) {
        if (step->mask & (1u << i)) player->melee_anim_tween.start[i] = player->melee_anim[i];
    }
}

void player_melee_anim_start(Player *player, const MeleeAnimStep *step, const MeleeAnimStep *then) {
    MeleeAnimTween *tween = &player->melee_anim_tween;
    tween->step = *step;
    tween->has_then = then != NULL;
    if (then) tween->then = *then;
    tween->time = 0.0f;
    tween->phase = 1;
    player_melee_anim_capture(player, step);
}

void player_melee_anim_stop(Player *player) {
    player->melee_anim_tween.phase = 0;
}

void player_melee_anim_reset(Player *player) {
    for (int i = 0; i < MELEE_ANIM_CHANNELS; i++) player->melee_anim[i] = 0.0f;
}

static void player_melee_anim_advance(Player *player, const float delta) {
    MeleeAnimTween *tween = &player->melee_anim_tween;
    if (!tween->phase) return;

    const MeleeAnimStep *step = tween->phase == 1 ? &tween->step : &tween->then;
    tween->time += delta;
    if (tween->time < step->delay) return;

    const float k = step->duration > 0.0f ? MIN(1.0f, (tween->time - step->delay) / step->duration) : 1.0f;
    const float e = player_ease(step->ease, k);
    for (int i = 0; i < MELEE_ANIM_CHANNELS; i++) {
        if (step->mask & (1u << i)) player->melee_anim[i] = tween->start[i] + (step->to[i] - tween->start[i]) * e;
    }
    if (k < 1.0f) return;

    if (tween->phase == 1 && tween->has_then) {
        tween->phase = 2;
        tween->time = 0.0f;
        player_melee_anim_capture(player, &tween->then);
    } else {
        tween->phase = 0;
    }
}

// swapWeapon (local, not recon, another slot): stop meleeAnim.anim and resetMeleeAnim; from
// hip-fire armR = -0.3, and a melee weapon flicks armR / weaR back to 0 over 310 ms Sinusoidal.Out.
// (A flipAnim melee skin also spins weaR from -4 PI: no melee skins natively.)
static void player_feel_melee_equip(Player *player) {
    player_melee_anim_stop(player);
    player_melee_anim_reset(player);
    if (player->aim_val) return;

    player->melee_anim[MELEE_ANIM_ARM_R] = -0.3f;
    if (!player->weapon->melee) return;

    const MeleeAnimStep equip = {
        .mask = MELEE_ANIM_BIT(ARM_R) | MELEE_ANIM_BIT(WEA_R),
        .duration = 0.31f,
        .ease = PLAYER_EASE_SINUSOIDAL_OUT,
    };
    player_melee_anim_start(player, &equip, NULL);
}

// Advanced once per frame before the queued inputs, like TWEEN.update() at the top of
// gameLoop: a tween started by this frame's inputs first moves next frame.
static void player_feel_update(Player *player, const float delta) {
    if (player_tween_advance(&player->land_tween, &player->land_bob_y, delta) && player->land_tween.then_settle) {
        // resetStep onComplete: a fresh tween started at that moment (TWEEN.now())
        player_tween_start(&player->land_tween, player->land_bob_y, 0.0f, 1.1f, PLAYER_EASE_ELASTIC_OUT);
    }

    player_tween_advance(&player->jump_bob_tween, &player->jump_bob_y, delta);
    player_tween_advance(&player->jump_rot_tween, &player->jump_rotate, delta);
    player_tween_advance(&player->swap_tween, &player->swap_tween_r, delta);
    player_melee_anim_advance(player, delta);

    // inspectTween (Exponential.Out, 400 ms) toward weapon.inspAnim.
    if (player->inspect_anim_active) {
        player->inspect_anim_time += delta;
        const float k = MIN(1.0f, player->inspect_anim_time / 0.4f);
        const float e = k >= 1.0f ? 1.0f : 1.0f - powf(2.0f, -10.0f * k);

        for (int i = 0; i < 9; i++) {
            player->inspect_anim[i] = player->inspect_anim_start[i] + (player->weapon->inspect_anim[i] - player->inspect_anim_start[i]) * e;
        }
        if (k >= 1.0f) player->inspect_anim_active = 0;
    }

    // recoilTween: kick (linear) then onComplete -> all four back to 0 (Back.Out), the
    // return starting at the completing frame like TWEEN's onComplete start().
    if (player->recoil_tween_phase) {
        player->recoil_tween_time += delta;
        const float k = player->recoil_tween_duration > 0.0f ? MIN(1.0f, player->recoil_tween_time / player->recoil_tween_duration) : 1.0f;

        if (player->recoil_tween_phase == 1) {
            for (int i = 0; i < 4; i++) {
                if (!(player->recoil_tween_mask & (1 << i))) continue;
                const float target = i == 0 ? player->weapon->recoil_tween_y : i == 1 ? player->weapon->recoil_tween_r : i == 2 ? player->weapon->recoil_tween_ym : player->weapon->recoil_tween_z;
                player->recoil_tween[i] = player->recoil_tween_start[i] + (target - player->recoil_tween_start[i]) * k;
            }
        } else {
            const float e = player_ease(PLAYER_EASE_BACK_OUT, k);
            for (int i = 0; i < 4; i++) player->recoil_tween[i] = player->recoil_tween_start[i] * (1.0f - e);
        }

        if (k >= 1.0f) {
            if (player->recoil_tween_phase == 1) {
                player->recoil_tween_phase = 2;
                player->recoil_tween_time = 0.0f;
                player->recoil_tween_duration = player->recoil_tween_return;
                for (int i = 0; i < 4; i++) player->recoil_tween_start[i] = player->recoil_tween[i];
            } else {
                player->recoil_tween_phase = 0;
            }
        }
    }
}

// shoot (renderYou): restart the viewmodel recoil tween. Duration (recoilAnim.time || rate)
// * fiRat, recoilAnim.aimTime while fully aimed; kick over 15 %, return over 95 %.
static void player_feel_recoil(Player *player) {
    const Weapon *weapon = player->weapon;
    const float fire_rate = player->game->config.fire_rate > 0.0f ? player->game->config.fire_rate : 1.0f;
    float time = (weapon->recoil_anim_time ? weapon->recoil_anim_time : weapon->rate) * fire_rate;
    if (player->aim_val >= 1.0f && weapon->recoil_anim_aim_time) time = weapon->recoil_anim_aim_time;

    for (int i = 0; i < 4; i++) player->recoil_tween_start[i] = player->recoil_tween[i];
    player->recoil_tween_mask = weapon->has_recoil_anim ? weapon->recoil_anim_mask : 0;
    player->recoil_tween_phase = 1;
    player->recoil_tween_time = 0.0f;
    player->recoil_tween_duration = time * 0.15f;
    player->recoil_tween_return = time * 0.95f;
}

// swapWeapon (local HUD path, not recon): a hip-fire swap to another slot wiggles the
// new weapon: swapTweenR = swapWiggle || 0.6 -> 0 over swapTime + 220 ms, Back.InOut.
static void player_feel_swap(Player *player) {
    player->swap_tween_r = player->weapon->swap_wiggle ? player->weapon->swap_wiggle : 0.6f;
    player_tween_start(&player->swap_tween, player->swap_tween_r, 0.0f, player->weapon->swap_time + 0.22f, PLAYER_EASE_BACK_IN_OUT);
}

// Math.random-like source for jumpRot that leaves the shared pcg32 stream alone.
static float player_feel_random(const float min, const float max) {
    static uint32_t state = 0x9e3779b9u;
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return min + (max - min) * (float) (state >> 8) / 16777216.0f;
}

// resetStep (landing): landBobY -> velY * 6 and jumpBobY -> 0 over 100 ms, then
// landBobY -> 0 over 1100 ms Elastic.Out.
static void player_feel_land(Player *player) {
    player_tween_start(&player->land_tween, player->land_bob_y, player->velocity.y * 6.0f, 0.1f, PLAYER_EASE_LINEAR);
    player->land_tween.then_settle = 1;
    player_tween_start(&player->jump_bob_tween, player->jump_bob_y, 0.0f, 0.1f, PLAYER_EASE_LINEAR);
}

// jump: landBobY -> 0 (100 ms), jumpBobY -> 0.22 (500 ms Back.Out), jumpRot -> [0.12..0.15, 0] (900 ms Back.Out)
static void player_feel_jump(Player *player) {
    player_tween_start(&player->land_tween, player->land_bob_y, 0.0f, 0.1f, PLAYER_EASE_LINEAR);
    player_tween_start(&player->jump_bob_tween, player->jump_bob_y, 0.22f, 0.5f, PLAYER_EASE_BACK_OUT);
    player_tween_start_via(&player->jump_rot_tween, player->jump_rotate, player_feel_random(0.12f, 0.15f), 0.0f, 0.9f, PLAYER_EASE_BACK_OUT);
}

// wall jump: jumpBobY -> 0.26 (500 ms Back.Out), jumpRot -> [0.15, 0] (900 ms Back.Out)
static void player_feel_wall_jump(Player *player) {
    player_tween_start(&player->jump_bob_tween, player->jump_bob_y, 0.26f, 0.5f, PLAYER_EASE_BACK_OUT);
    player_tween_start_via(&player->jump_rot_tween, player->jump_rotate, 0.15f, 0.0f, 0.9f, PLAYER_EASE_BACK_OUT);
}

// Player.reset (spawn): the step/bob/lean/recoil feel starts from rest.
void player_reset_feel(Player *player) {
    player->step_val = player->step_chase = 0.0f;
    player->step_dir_neg = 0;
    player->land_bob_y = player->land_bob_yr = player->jump_bob_y = 0.0f;
    player->jump_rotate = player->jump_rotate_mlt = 0.0f;
    player->bob_anim = player->lean_anim = player->recoil = (vec3) {0};
    player->recoil_force = player->recoil_anim = player->recoil_anim_y = 0.0f;
    player->land_tween.active = player->jump_bob_tween.active = player->jump_rot_tween.active = 0;
    player->swap_tween_r = 0.0f;
    player->swap_tween.active = 0;
    player->inspecting = 0;
    player->inspect_x = 0.0f;
    player->inspect_anim_active = 0;
    for (int i = 0; i < 9; i++) player->inspect_anim[i] = 0.0f;
    player->recoil_tween_phase = 0;
    for (int i = 0; i < 4; i++) player->recoil_tween[i] = 0.0f;
}

void player_reset_step(Player *player, const int recon) {
    if (!recon && player->render_you && player->velocity.y) player_feel_land(player);
    if (!recon && player->velocity.y) player->land_events++; // resetStep: step_N at 0.7 (sound_events.c)

    // TODO: landing step sound (sound_events)

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

// game.onTrigger(player, object): server only (io), team-filtered, singleUse / chance
// gated, then triggers.actions[action].execute. Only "Refill Player Knife" is ported.
static void player_on_trigger(Player *player, Object *object) {
    Game *game = player->game;
    if (!game_is_authority(game)) return;
    if (object->team && game->mode && game->mode->config.teams && (int) object->team != player->team) return;
    if (object->trigger_single_use && object->triggered) return;
    if (object->trigger_chance && !((int) pcg32_boundedrand(101) <= object->trigger_chance)) return; // randInt(0, 100) <= chance
    object->triggered = 1;
    switch (object->trigger_action) {
        case TRIGGER_ACTION_REFILL_KNIFE: // h.refillKnife(); g.send(h.id, "rfl")
            player->can_throw = 1;
            game_send(game, player->uid, NET_PACKET_REFILL, NULL, 0);
            break;
        default: // TODO: the other trigger actions
            break;
    }
}

void player_do_map_collisions(Player *player, const Input *input, const float move_dir, const int can_jump, const float delta, const int recon) {
    float *max_ramp_height = NULL;

    // TODO: grid arrays (perf)
    for (size_t i = 0; i < player->game->map->object_count; i++) {
        const Object *object = player->game->map->objects[i];
        const int collides_padded = player_collides(player, object, game_constants.collision_padding);

        if (!object->active || !collides_padded) {
            if (player->in_object == (int) i + 1) player->in_object = 0; // left the onEnter trigger
            continue;
        }

        const int collides = player_collides(player, object, 0.0f);

        // TODO: interactable's
        // TODO: collision trigger (onCollision, onTrigger)
        // TODO: onInteract
        // TODO: onEnter

        if (object->trigger_on_enter) {
            // onEnter: fires when the player steps in (inObject != sid); nothing else of the
            // object applies (the jumpPad / zone / collision else-chain is skipped)
            if (player->in_object != (int) i + 1) player_on_trigger(player, (Object *) object);
            player->in_object = (int) i + 1;
            continue;
        }

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
                    if (player->last_position.y > player->position.y) player_reset_step(player, recon);

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
    if (loadout_changed && !recon) player_cancel_inspect(player); // swapWeapon: cancelInspect
    // TODO: weapon meshes

    const PlayerMesh *player_mesh = player->mesh;

    if (player_mesh) {
        for (size_t i = 0; i < player->loadout_size; i++) {
            const PlayerArms *arms = player_mesh->arms[i];
            const int show = i == player->loadout_index;

            if (arms->weapon_right) arms->weapon_right->visible = show;
            if (arms->weapon_left) arms->weapon_left->visible = show;
            if (arms->attachment) arms->attachment->visible = show;

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

    player->reload_timer = player->weapon->reload_time * (player->game->config.reload_speed > 0.0f ? player->game->config.reload_speed : 1.0f); // (reSpd || 1)
    player->burst_count = 0;

    player_cancel_inspect(player);
}

// cancelInspect: stop inspectTween, inspecting off, inspectX and the inspectAn pose back to 0.
void player_cancel_inspect(Player *player) {
    player->inspecting = 0;
    player->inspect_x = 0.0f;
    player->inspect_anim_active = 0;
    for (int i = 0; i < 9; i++) player->inspect_anim[i] = 0.0f;
}

void player_inspect_weapon(Player *player) {
    if (player->inspecting) return player_cancel_inspect(player);
    if (player->weapon->no_inspect || player->aim_val || player->reload_timer || (player->weapon->melee && !player->can_throw)) return;

    // wInspect: inspectX then swings toward weapon.insX || PI / 2.8 in procInputs, and a
    // weapon.inspAnim (Combat Knife) tweens inspectAn to that pose over 400 ms Exponential.Out.
    // (flipAnim melee skins also spin the knife, wpSp = 4 PI, and play melee_equip: no skins yet.)
    player->inspecting = 1;

    if (player->weapon->has_inspect_anim) {
        for (int i = 0; i < 9; i++) player->inspect_anim_start[i] = player->inspect_anim[i];
        player->inspect_anim_time = 0.0f;
        player->inspect_anim_active = 1;
    }
}

void player_proc_input(Player *player, const Input *input, const int recon, const int move_lock) {
    // const float delta = CLAMP(input->delta, game_constants.min_delta, game_constants.max_delta);
    const float delta = MIN(input->delta, game_constants.max_delta);
    const float move_dir = -(float) M_PI / 2.0f + (float) M_PI / 4.0f * (float) input->move_dir;

    if (player->noclip) player->on_ground = 1;

    // UTILS.angleDistance(new, old) = old - new (players.js a7 and the pitch term)
    const float pitch_delta = normalize_angle(player->direction.x - input->x_dir);
    const float yaw_delta = normalize_angle(player->direction.y - input->y_dir);

    // TODO: spin distance (unused?)

    // Local first-person animation only (render_you is never set on the server).
    if (!recon && player->render_you) {
        player->lean_anim.x -= yaw_delta * game_constants.lean_sensitivity;
        player->lean_anim.x = CLAMP(player->lean_anim.x, -game_constants.lean_max, game_constants.lean_max);

        player->lean_anim.y -= pitch_delta * game_constants.lean_sensitivity;
        player->lean_anim.y = CLAMP(player->lean_anim.y, -game_constants.lean_max, game_constants.lean_max);

        if (player->lean_anim.x) player->lean_anim.x *= powf(game_constants.lean_pull, delta * 1000.0f);
        if (player->lean_anim.y) player->lean_anim.y *= powf(game_constants.lean_pull, delta * 1000.0f);
        if (player->lean_anim.z) player->lean_anim.z *= powf(game_constants.lean_pull_z, delta * 1000.0f);

        if (player->bob_anim.y) player->bob_anim.y *= powf(game_constants.bob_pull_y, delta * 1000.0f);
        if (player->bob_anim.z) player->bob_anim.z *= powf(game_constants.bob_pull_z, delta * 1000.0f);

        if (player->recoil.x) player->recoil.x *= powf(game_constants.lean_pull, delta * 1000.0f);
        if (player->recoil.z) player->recoil.z *= powf(game_constants.lean_pull, delta * 1000.0f);

        if (player->inspecting && player->weapon) {
            const float target = player->weapon->inspect_x ? player->weapon->inspect_x : (float) M_PI / 2.8f;
            if (player->inspect_x < target) player->inspect_x += (target - player->inspect_x) * 0.1f;
        }
    }

    player->direction.x = CLAMP(input->x_dir, -(float) M_PI / 2.0f, (float) M_PI / 2.0f);
    player->direction.y = input->y_dir;

    const int feel_loadout_index = player->loadout_index;

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

    if (!recon && player->is_you && player->loadout_index != feel_loadout_index) {
        if (!player->aim_val) player_feel_swap(player);
        player_feel_melee_equip(player);
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
            player_cancel_inspect(player);
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
            if (!recon && player->render_you) player->bob_anim.y -= game_constants.crouch_anim * 1.4f * delta;
        } else {
            player->position.y += game_constants.crouch_speed * delta;
        }
    } else if (!input->crouch && player->crouch_val > 0.0f) {
        player->crouch_val = MAX(0.0f, player->crouch_val - game_constants.crouch_speed * delta);

        if (player->on_ground) {
            if (!recon && player->render_you) player->bob_anim.y += game_constants.crouch_anim * delta;
        } else {
            player->position.y -= game_constants.crouch_speed * delta;
        }
    }

    if (player->crouch_val) player->crouch_val = roundf(player->crouch_val * 1000.0f) / 1000.0f; // crouchVal.round(3)

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
            player->slide_timer = player->slide_timer - delta > MOVEMENT_TIMER_EPSILON ? player->slide_timer - delta : 0.0f;

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
                if (!recon && player->render_you) player_feel_jump(player);
            }
        }

        if (!contact) {
            const float wall_mlt = player->velocity.y < 0.0f && player->wall_jump && player->on_wall && player->game->config.wall_jump > 0.0f && player->crouch_val ? 0.3f : 1.0f;
            player->velocity.y -= delta * game_constants.gravity * player->game->config.gravity_mlt * wall_mlt;
        }

        // 95.js: ac *= limitMM(1 - a0[12], 0, 1) (input[12]: the controller's Gradual Speed)
        accel *= CLAMP(1.0f - input->slowdown, 0.0f, 1.0f);

        // Diagonal movDirs: config.strafeSpd (1.2) unless the mode uses realMov.
        if (input->move_dir >= 0 && input->move_dir % 2 && !player->game->mode->config.real_movement) {
            accel *= player->game->config.strafe_speed > 0.0f ? player->game->config.strafe_speed : 1.0f;
        }

        if (input->move_dir >= 0) {
            const float yaw_origin = player->direction.y; // TODO: support different cam types?

            player->velocity.x += accel * cosf(move_dir - yaw_origin);
            player->velocity.z += accel * sinf(move_dir - yaw_origin);

            if (player->noclip) player->velocity.y += accel * player->direction.x * (input->move_dir > 2 && input->move_dir < 6 ? -1.0f : 1.0f);
        }

        // Air strafe (config.airStrf): turning while holding right/left (movDirs 3/7)
        // swings the air velocity with the camera.
        if (!contact && yaw_delta && player->game->config.air_strafe && ((input->move_dir == 2 && yaw_delta > 0.0f) || (input->move_dir == 6 && yaw_delta < 0.0f))) {
            const float vel = hypotf(player->velocity.x, player->velocity.z);
            const float dir = atan2f(-player->velocity.z, -player->velocity.x) + yaw_delta * 0.73f;

            player->velocity.x = vel * cosf(dir + (float) M_PI);
            player->velocity.z = vel * sinf(dir + (float) M_PI);
        }

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

        // playerStep (the step sound is TODO: sound_events can follow step_dir_neg flips)
        if (player->is_you && contact && input->move_dir >= 0) player_step(player, delta_pos);

        if (player->render_you) {
            if (contact && input->move_dir >= 0) {
                player->lean_anim.z -= delta_pos * game_constants.lean_mlt_z * (player->weapon->z_lean_mlt ? player->weapon->z_lean_mlt : 1.0f) * cosf(move_dir);
            } else {
                player->step_val *= powf(game_constants.step_pull, delta * 1000.0f);
            }

            if (player->step_chase != player->step_val) player->step_chase += (player->step_val - player->step_chase) * 0.15f;
        }

        // Krunker spread: movement/fall spreadPlus, crouch and recoil terms, scaled by
        // the hip-fire amount (KrunkNative aim_val 1 = aimed, Krunker aimVal 0 = aimed).
        player->spread_plus += delta_pos * COMBAT_SPREAD_MOVE + fabsf(player->last_position.y - player->position.y) * COMBAT_SPREAD_FALL;
        player->spread_plus *= powf(COMBAT_SPREAD_RECOVER, delta * 1000.0f);
        player->spread = player->weapon->no_spread ? 0.0f : MAX(player->weapon->min_spread,
            (player->weapon->spread - player->weapon->spread * COMBAT_CROUCH_SPREAD * player->crouch_val +
             player->recoil_anim * COMBAT_SPREAD_MLT + player->spread_plus) * (1.0f - player->aim_val));

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
            int will_shoot = player->burst_count || !player->weapon->no_auto && input->shoot;

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
            if (!recon && player->is_you) player_feel_wall_jump(player);

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

// playerStep: stepVal swings between +-PI/2 with the distance covered (3D, x1.4 on a
// ladder for the local player); crouched remote players step 1.6x faster.
void player_step(Player *player, float distance) {
    if (!distance) return;
    if (!player->render_you && player->crouch_val) distance *= 1.6f;

    player->step_val += distance * MOVEMENT_STEP_MLT * (player->step_dir_neg ? -1.0f : 1.0f);

    if ((player->step_val >= (float) M_PI / 2.0f && !player->step_dir_neg) || (player->step_val <= -(float) M_PI / 2.0f && player->step_dir_neg)) {
        player->step_dir_neg ^= 1;
        player->step_events++; // playerStep sound (sound_events.c)
    }
}

// ---------------------------------------------------------------- combat
// Hitscan, damage, kills and score: Krunker v3.1.1 src/data/players.js (shoot,
// melee, changeHealth, kill, score) and src/data/kills.js (reward). Only the
// authority (native server, offline local server, is_local games) applies damage;
// clients predict recoil/spread and learn the outcome from HIT/HEALTH/KILL packets.
// Not ported yet: lag compensation (fetchState rewind), projectiles, destructible
// objects, AI targets, medals ("6" packet) and kill streak rewards.

typedef struct {
    const Object *object;
    Player *player;
    float t;
} Hit;

typedef struct {
    Hit *items;
    size_t count;
    size_t capacity;
} HitList;

static void hit_list_push(HitList *list, const Hit hit) {
    if (list->count == list->capacity) {
        const size_t capacity = list->capacity ? list->capacity * 2 : 16;
        Hit *items = realloc(list->items, capacity * sizeof(Hit));
        if (!items) return;

        list->items = items;
        list->capacity = capacity;
    }

    list->items[list->count++] = hit;
}

// UTILS.orderByDst (insertion sort keeps equal distances in push order like V8's stable sort).
static void hit_list_sort(HitList *list) {
    for (size_t i = 1; i < list->count; i++) {
        const Hit hit = list->items[i];
        size_t at = i;

        while (at > 0 && list->items[at - 1].t > hit.t) {
            list->items[at] = list->items[at - 1];
            at--;
        }

        list->items[at] = hit;
    }
}

// Krunker only traces objects that are active and not noShoot. map.c does not
// parse noShoot yet, so the zone/helper prefabs Krunker creates with noShoot
// (objects.js addFlag/addWeapon/addDeathZone/addScoreZone/addPremiumZone/
// addVerifiedZone/addTeamZone/addCheckPoint/addTeleporter/addLadder/addRamp) and
// collision-less objects are skipped here.
static int combat_object_shootable(const Object *object) {
    if (!object->active || object->no_shoot || object->collision_type == COLLISION_TYPE_NONE) return 0;
    if (object->kill || object->score_zone || object->team_zone || object->teleporter || object->checkpoint ||
        object->pickup || object->flag || object->premium || object->verified || object->trigger ||
        object->ladder || object->ramp) return 0;

    switch (object->prefab) {
        case PREFAB_TEAM_ZONE:
        case PREFAB_SPAWN_POINT:
        case PREFAB_CAMERA_POSITION:
        case PREFAB_SPECTATE_CAM:
            return 0;
        default:
            return 1;
    }
}

static int combat_can_damage(const Player *dealer, const Player *victim) {
    return !dealer->team || dealer->team != victim->team || (dealer->game->mode && dealer->game->mode->config.dmg_team);
}

// lineInRect against every shootable object and (authority only) every other
// living player's hitbox: x/z +-(scale + hitBoxPad), y from the feet to height + pad.
static void combat_trace(Player *shooter, const vec3 origin, const vec3 dir, const int include_players, HitList *hits) {
    Game *game = shooter->game;

    for (size_t i = 0; i < game->map->object_count; i++) {
        const Object *object = game->map->objects[i];
        if (!combat_object_shootable(object)) continue;

        const float t = line_in_rect(origin, dir, object->position, object->scale);
        if (t <= 0.0f || t > 1.0f) continue;

        hit_list_push(hits, (Hit) {object, NULL, t});
    }

    if (!include_players) return;

    const float pad = game->mode && game->mode->config.hitbox_pad ? game->mode->config.hitbox_pad : 1.0f;

    for (size_t i = 0; i < game->player_count; i++) {
        Player *other = game->players[i];
        if (!other || !other->active || other == shooter || !combat_can_damage(shooter, other)) continue;

        const float half = other->scale + pad;
        const vec3 box_scale = {half * 2.0f, other->height + pad, half * 2.0f};
        const float t = line_in_rect(origin, dir, other->position, box_scale);
        if (t <= 0.0f || t > 1.0f) continue;

        hit_list_push(hits, (Hit) {NULL, other, t});
    }
}

static vec3 combat_shot_vector(const float range, const float pitch, const float yaw) {
    return (vec3) {
        range * sinf(yaw + (float) M_PI) * cosf(pitch),
        range * sinf(pitch),
        range * cosf(yaw + (float) M_PI) * cosf(pitch),
    };
}

static int combat_weapon_index(const Player *player) {
    if (!player->loadout || player->loadout_index < 0 || player->loadout_index >= player->loadout_size) return -1;
    return player->loadout[player->loadout_index];
}

static const Weapon *combat_weapon_at(const Game *game, const int index) {
    const size_t count = game->weapon_count ? game->weapon_count : sizeof(g_weapons) / sizeof(g_weapons[0]);
    const Weapon *const *weapons = game->weapons ? (const Weapon *const *) game->weapons : (const Weapon *const *) g_weapons;
    return index >= 0 && index < (int) count ? weapons[index] : NULL;
}

// weapons.js fields the native Weapon struct does not carry yet: `kill` (custom
// kill score replacing the base 50) and `scope` (sniper scope: No/Quick Scope).
static int combat_weapon_kill_score(const Weapon *weapon) {
    static const struct { const char *name; int score; } table[] = {
        {"Pistol", 75}, {"Revolver", 50}, {"Desert Eagle", 50}, {"Alien Blaster", 50},
        {"Crossbow", 75}, {"Auto Pistol", 50}, {"Blaster", 75},
    };

    if (!weapon || !weapon->name) return 0;
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (!strcmp(weapon->name, table[i].name)) return table[i].score;
    }
    return 0;
}

static int combat_weapon_scope(const Weapon *weapon) {
    return weapon && weapon->name && !strcmp(weapon->name, "Sniper Rifle");
}

// One kills.js reward pair (tmpList.push(name, score)); returns the score for getScore().
static int combat_medal(NetKillMedalsPacket *medals, const char *name, const int score) {
    if (medals->count < NET_KILL_MEDALS_MAX) {
        NetMedalPacket *m = &medals->medals[medals->count++];
        snprintf(m->text, sizeof(m->text), "%s", name);
        m->score = score;
    }
    return score;
}

// kills.js reward() + getScore(): the medals of one kill (into `medals`) and their summed score.
// KrunkNative aim_val runs 0 (hip) -> 1 (aimed), Krunker's aimVal 1 -> 0.
static int combat_kill_reward(Game *game, Player *killer, Player *victim, const PlayerKillInfo *info, const Weapon *weapon,
                              NetKillMedalsPacket *medals) {
    int score = 0;

    if (game->kill_count <= 1) score += combat_medal(medals, "First Blood", 25);
    if (killer->last_killer_uid >= 0 && killer->last_killer_uid == victim->uid) {
        killer->last_killer_uid = -1;
        score += combat_medal(medals, "Revenge", 25);
    }

    if (weapon && weapon->melee) {
        if (info && info->thrown) score += combat_medal(medals, "Bullseye", 150);
        else if (info && info->fists) score += combat_medal(medals, "Beatdown", 175);
        else score += combat_medal(medals, "Execution", 150);
    } else {
        const int custom = combat_weapon_kill_score(weapon);
        const int scope = combat_weapon_scope(weapon);

        if (custom) score += combat_medal(medals, "", custom);   // weapon.kill = ["", N]
        else if (scope && killer->aim_val == 0.0f) score += combat_medal(medals, "No Scope", 100);
        else if (!scope || (killer->aim_val == 1.0f && killer->aim_time > 0.18f)) score += combat_medal(medals, "", 50);
        else score += combat_medal(medals, "Quick Scope", 75);

        if (info && info->headshot) score += combat_medal(medals, "Headshot", 50);
        if (info && info->wallbang) score += combat_medal(medals, "Wallbang", 25);
        if (!scope && weapon && info && info->distance * weapon->range > 200.0f) score += combat_medal(medals, "Longshot", 25);
    }

    if (killer->health <= (float) killer->max_health * 0.2f && killer->active) score += combat_medal(medals, "Close Call", 20);
    if (killer->death_streak >= 3) score += combat_medal(medals, "Comeback", 25);
    if (victim->kill_streak >= 10) score += combat_medal(medals, "Buzzkill", 25);
    else if (victim->kill_streak >= 5) score += combat_medal(medals, "Buzzkill", 10);
    if (killer->slide_timer > 0.0f) score += combat_medal(medals, "Driftkill", 50);
    if (killer->air_time >= 0.4f) score += combat_medal(medals, "Mid Air", 25);
    if (sqrtf(killer->velocity.x * killer->velocity.x + killer->velocity.y * killer->velocity.y +
              killer->velocity.z * killer->velocity.z) * 1000.0f >= 180.0f) score += combat_medal(medals, "Turbo Kill", 25);

    static const char *const feeds[7] = {"Double Kill", "Triple Kill", "Quad Kill", "Multi Kill", "Mega Kill", "Ultra Kill", "Super Kill"};
    for (int i = 6; i >= 0; i--) {
        if ((int) killer->streak > i) {
            score += combat_medal(medals, feeds[i], (i + 1) * 50);
            break;
        }
    }

    return score;
}

// players.score(player, Q, isKill): public games cap the score at 13000.
void player_score(Player *player, int amount, const int is_kill) {
    Game *game = player->game;
    if (!game_is_authority(game) || !amount) return;

    player->score += mode_score_gain(game, amount, is_kill); // noScoreK: kill rewards add nothing
    if (player->score > GAME_SCORE_CAP) {
        player->score = GAME_SCORE_CAP;
        amount = 0;
    }

    mode_on_score(game, player, amount, is_kill); // objectiveScore + team score (players.teamScore)

    const NetScorePacket packet = {.delta = amount, .score = player->score};
    game_send(game, player->uid, NET_PACKET_SCORE, &packet, sizeof(packet));
    game->leaders_dirty = 1;
}

static void combat_record_damage(Player *victim, const Player *dealer, const float damage, const float now) {
    int slot = -1;

    for (int i = 0; i < PLAYER_DAMAGE_SLOTS; i++) {
        if (victim->damage_received[i].uid == dealer->uid && victim->damage_received[i].value > 0.0f) {
            victim->damage_received[i].value += damage;
            victim->damage_received[i].time = now;
            return;
        }

        if (slot < 0 && victim->damage_received[i].value <= 0.0f) slot = i;
    }

    if (slot < 0) { // full: replace the oldest entry
        slot = 0;
        for (int i = 1; i < PLAYER_DAMAGE_SLOTS; i++) {
            if (victim->damage_received[i].time < victim->damage_received[slot].time) slot = i;
        }
    }

    victim->damage_received[slot] = (PlayerDamage) {dealer->uid, damage, now};
}

static void combat_send_health(Game *game, const Player *victim, const Player *dealer) {
    NetHealthPacket packet = {.uid = victim->uid, .health = ceilf(victim->health)};

    // Everyone else (players, spectators): health bars.
    game_send(game, NET_TARGET_EXCEPT(victim->uid), NET_PACKET_HEALTH, &packet, sizeof(packet));

    if (dealer) {
        packet.has_source = 1;
        packet.source_x = roundf(dealer->position.x);
        packet.source_z = roundf(dealer->position.z);
    }

    game_send(game, victim->uid, NET_PACKET_HEALTH, &packet, sizeof(packet));
}

// players.changeHealth(victim, dealer, dmg, allowTeamDamage, crit): negative dmg
// heals. Returns 1 when the victim is out of health (the caller kills).
int player_change_health(Player *victim, Player *dealer, float damage, const int crit) {
    Game *game = victim->game;
    if (!game_is_authority(game) || !victim->active) return 0;
    if (damage < 0.0f && victim->health == (float) victim->max_health) return 0;
    if (dealer && damage > 0.0f && dealer != victim && !combat_can_damage(dealer, victim)) return 0;
    if (dealer && dealer->smite && damage > 0.0f) damage = 10000.0f; // /smite: weapon/melee/explosion dmg = 10000

    if (dealer && dealer->team == 1) damage *= game->config.team1_damage;
    else if (dealer && dealer->team == 2) damage *= game->config.team2_damage;

    if (dealer && dealer != victim && damage > 0.0f) {
        const NetHitPacket hit = {
            .target_uid = victim->uid,
            .damage = (int32_t) roundf(damage),
            .crit = (uint8_t) !!crit,
            .kill = (uint8_t) (victim->health - damage <= 0.0f),
        };
        game_send(game, dealer->uid, NET_PACKET_HIT, &hit, sizeof(hit));
    }

    if (damage > 0.0f) victim->hit_timer = GAME_REGEN_DELAY;

    victim->health = CLAMP(victim->health - damage, 0.0f, (float) victim->max_health);
    if (victim->god_mode) victim->health = (float) victim->max_health;

    if (dealer && dealer != victim) {
        dealer->damage_dealt += damage;
        combat_record_damage(victim, dealer, damage, game->now);
    }

    combat_send_health(game, victim, dealer);

    return victim->health <= 0.0f;
}

// players.kill(player, killer, info, skipRewards). The victim stops playing on
// every peer; the authority also does the bookkeeping and announces the kill.
void player_kill(Player *player, Player *killer, PlayerKillInfo *kill_info, const int skip_rewards) {
    if (!player->active) return;

    Game *game = player->game;
    if (!game_is_authority(game)) return; // clients wait for the server's KILL packet

    player->active = 0;
    player->deaths++;
    player->last_killer_uid = -1;
    player->death_time = game->now;
    player->input_queue_size = 0;
    free(player->input_queue);
    player->input_queue = NULL;

    const int streaks = !(game->mode && game->mode->config.no_streaks) && !game->config.no_streaks;
    if (streaks) player->death_streak++;

    game->kill_count++;

    const int weapon_index = killer ? (kill_info && kill_info->weapon_id >= 0 ? kill_info->weapon_id : combat_weapon_index(killer)) : -1;
    int reward = 0;

    if (killer && killer != player) {
        player->last_killer_uid = killer->uid;
        killer->kills++;

        if (streaks) {
            killer->streak++;
            killer->kill_streak++;
            killer->death_streak = 0;
            if (game->now - killer->last_kill_time >= GAME_FEED_TIMER) killer->streak = 0;
        }

        killer->last_kill_time = game->now;

        if (!skip_rewards && game->config.kill_rewards) {
            NetKillMedalsPacket medals;
            memset(&medals, 0, sizeof(medals));
            reward = combat_kill_reward(game, killer, player, kill_info, combat_weapon_at(game, weapon_index), &medals);
            medals.kills = (int32_t) killer->kills;
            if (medals.count) game_send(game, killer->uid, NET_PACKET_KILL_MEDALS, &medals, sizeof(medals)); // IO.send(Q.id, "6", W, ..)
            player_score(killer, reward, 1);
        }
    }

    // Assists: config.assistMin damage within config.assistTime.
    for (size_t i = 0; i < game->player_count; i++) {
        Player *other = game->players[i];
        if (!other || other == killer || other == player || (game->mode && game->mode->config.no_assists)) continue;

        for (int k = 0; k < PLAYER_DAMAGE_SLOTS; k++) {
            const PlayerDamage *damage = &player->damage_received[k];
            if (damage->uid != other->uid || damage->value < GAME_ASSIST_MIN || game->now - damage->time > GAME_ASSIST_TIME) continue;

            other->assists++;
            player_score(other, GAME_ASSIST_SCORE, 1);
            game_send(game, other->uid, NET_PACKET_ASSIST, NULL, 0); // IO.send(id, "10")
            break;
        }
    }

    for (int k = 0; k < PLAYER_DAMAGE_SLOTS; k++) player->damage_received[k] = (PlayerDamage) {-1, 0.0f, 0.0f};
    player->kill_streak = 0;

    const NetKillPacket packet = {
        .victim_uid = player->uid,
        .killer_uid = killer ? killer->uid : -1,
        .weapon_index = weapon_index,
        .headshot = (uint8_t) (kill_info && kill_info->headshot),
        .wallbang = (uint8_t) (kill_info && kill_info->wallbang),
        .fists = (uint8_t) (kill_info && kill_info->fists),
        .victim_deaths = (int32_t) player->deaths,
        .killer_kills = killer ? (int32_t) killer->kills : 0,
        .killer_class = killer ? killer->class_index : -1,
        .score = reward,
        .respawn_delay = GAME_DEATH_DELAY,
    };
    game_send(game, NET_TARGET_ALL, NET_PACKET_KILL, &packet, sizeof(packet));
    game->leaders_dirty = 1;

    mode_on_kill(game, player, killer, weapon_index); // lives, mode.onKill (flags, Gun Game, tags, ...)
}

void player_melee(Player *player) {
    // 95.js: weapon.canThrow && jKFi4 == 0 && canThrow (fully aimed); the readable jail src/data/players.js has aimVal <= 0.1.
    const int is_throw = player->can_throw && player->weapon->can_throw && player->aim_val == 1.0f;

    player->reloads[player->loadout_index] = player->weapon->rate * (player->game->config.fire_rate > 0.0f ? player->game->config.fire_rate : 1.0f); // (fiRat || 1)
    player->did_shoot = 1;
    player->did_act = 1;

    // TODO: gamepad rumble?
    // isYou: weapon.anim(player, TWEEN, canThrow ? (throw ? 3 : 1) : 2), cancelInspect (swsh sounds: sound_events.c)
    if (player->is_you) {
        if (player->weapon->melee_anim) player->weapon->melee_anim(player, player->can_throw ? (is_throw ? 3 : 1) : 2);
        player_cancel_inspect(player);
    }

    if (is_throw) {
        // projectiles.init(stored[0]) with wId 12: the knife flies, Bullseye on a kill.
        player->can_throw = !!player->unlimited_ammo;
        player->shot_count++;
        const vec3 origin = {player->position.x, player->position.y + player->height - game_constants.camera_height, player->position.z};
        game_spawn_projectile(player->game, player, WEAPON_ID_KNIFE, projectile_config_for(player->weapon, 1), origin, player->direction.y, player->direction.x, 1);
        return;
    }

    Game *game = player->game;
    if (!game_is_authority(game)) return;

    const vec3 origin = {player->position.x, player->position.y + player->height - game_constants.camera_height, player->position.z};
    const vec3 dir = combat_shot_vector(player->weapon->range, player->direction.x, player->direction.y);
    const int weapon_index = combat_weapon_index(player);
    HitList hits = {0};

    combat_trace(player, origin, dir, 1, &hits);
    hit_list_sort(&hits);

    // Every target in range soaks the full swing unless the weapon pierces.
    float damage = player->can_throw ? player->weapon->damage : COMBAT_FIST_DAMAGE;

    for (size_t i = 0; i < hits.count && damage > 0.0f; i++) {
        Player *victim = hits.items[i].player;

        if (victim && player_change_health(victim, player, mode_melee_damage(game, player, victim, damage), 0)) { // onMelee (Stalker)
            PlayerKillInfo info = {.weapon_id = weapon_index, .fists = !player->can_throw, .distance = hits.items[i].t};
            player_kill(victim, player, &info, 0);
        }

        damage -= player->weapon->pierce ? player->weapon->damage * COMBAT_DENSITY_FLESH * player->weapon->pierce : damage;
    }

    free(hits.items);
}

// Krunker "9" to everyone but the shooter: where the ray ended (12 cm before the
// target) and, for map objects, which face it hit (particle direction).
static void combat_send_shot(Player *player, const int ray, const float pitch, const float yaw, const float shot_height,
                             const float range, const Hit *last) {
    Game *game = player->game;
    const float end = (last ? range * last->t : range) - 0.12f;

    NetShotPacket packet = {
        .uid = player->uid,
        .weapon_index = combat_weapon_index(player),
        .hit = {
            player->position.x + end * sinf(yaw + (float) M_PI) * cosf(pitch),
            shot_height + end * sinf(pitch),
            player->position.z + end * cosf(yaw + (float) M_PI) * cosf(pitch),
        },
        .first = (uint8_t) (ray == 0),
    };

    if (last && last->object) {
        const Object *object = last->object;
        const float x = packet.hit[0], y = packet.hit[1], z = packet.hit[2];

        packet.has_normal = 1;
        if (y >= object->position.y + object->scale.y) packet.normal_pitch = (float) M_PI / 2.0f;
        else if (y <= object->position.y) packet.normal_pitch = -(float) M_PI / 2.0f;
        else if (x <= object->position.x - object->scale.x * 0.5f) packet.normal_yaw = -(float) M_PI / 2.0f;
        else if (x >= object->position.x + object->scale.x * 0.5f) packet.normal_yaw = (float) M_PI / 2.0f;
        else if (z <= object->position.z - object->scale.z * 0.5f) packet.normal_yaw = (float) M_PI;
    }

    game_send(game, NET_TARGET_EXCEPT(player->uid), NET_PACKET_SHOT, &packet, sizeof(packet)); // players + spectators
}

// One ray of player_shoot(): physPow push (i < 0) or bullet damage with falloff,
// headshot/leg multipliers and wallbang through penetrable objects.
static void combat_fire_ray(Player *player, const int ray, const float pitch, const float yaw, const float shot_height) {
    Game *game = player->game;
    const Weapon *weapon = player->weapon;
    const int authority = game_is_authority(game);
    const float range = ray < 0 ? weapon->physical_range : weapon->range;
    if (range <= 0.0f || (!authority && ray >= 0)) return;

    const vec3 origin = {player->position.x, shot_height, player->position.z};
    const vec3 dir = combat_shot_vector(range, pitch, yaw);
    HitList hits = {0};

    combat_trace(player, origin, dir, authority && ray >= 0, &hits);
    hit_list_sort(&hits);

    float damage = weapon->damage;
    int wallbang = 0;
    const Hit *last = NULL; // Krunker G: the last target the bullet reached (tracer end)

    for (size_t i = 0; i < hits.count; i++) {
        const Hit *hit = &hits.items[i];
        last = hit;

        if (ray < 0) {
            const float impulse = (1.0f - hit->t) * weapon->physical_power * game->config.impulse_mlt;
            player->velocity.x -= impulse * sinf(yaw + (float) M_PI) * cosf(pitch);
            player->velocity.z -= impulse * cosf(yaw + (float) M_PI) * cosf(pitch);
            player->velocity.y -= impulse * sinf(pitch);
            player->on_ground = 0;
            break;
        }

        const float drop = fminf(1.0f, 1.0f - (1.0f - hit->t) * range / (range - weapon->drop_start));
        float dealt = damage - weapon->damage_drop * drop;

        if (hit->player) {
            Player *victim = hit->player;
            const float hit_y = shot_height + range * hit->t * sinf(pitch);
            const int head = victim->position.y + victim->height - hit_y < COMBAT_HEAD_SCALE;
            const int leg = victim->position.y + COMBAT_LEG_HEIGHT > hit_y;

            if (head) dealt *= weapon->headshot_mlt ? weapon->headshot_mlt : 1.5f;
            if (leg) dealt *= 0.5f;
            if (!head && game->config.headshots_only) dealt = 0.0f;

            if (player_change_health(victim, player, dealt, head)) {
                PlayerKillInfo info = {
                    .weapon_id = combat_weapon_index(player),
                    .headshot = (unsigned char) head,
                    .wallbang = (unsigned char) wallbang,
                    .distance = hit->t,
                };
                player_kill(victim, player, &info, 0);
            }

            damage -= weapon->pierce ? weapon->damage * COMBAT_DENSITY_FLESH * weapon->pierce : damage;
        } else if (hit->object->penetrable) {
            wallbang = 1;
            damage -= weapon->pierce ? weapon->damage * COMBAT_DENSITY_DEFAULT * weapon->pierce : damage;
        } else {
            break;
        }

        if (damage <= 0.0f) break;
    }

    if (ray >= 0) combat_send_shot(player, ray, pitch, yaw, shot_height, range, last);

    free(hits.items);
}

void player_shoot(Player *player) {
    // TODO: increase shots stat

    if (!player->unlimited_ammo) player->ammo[player->loadout_index]--;

    player->did_shoot = 1;
    player->did_act = 1;

    if (player->burst_count) player->burst_count--;
    else player->burst_count = player->weapon->burst ? player->weapon->burst_count - 1 : 0;

    player->reloads[player->loadout_index] = (player->burst_count && player->weapon->burst ? player->weapon->burst_rate : player->weapon->rate) * (player->game->config.fire_rate > 0.0f ? player->game->config.fire_rate : 1.0f); // (fiRat || 1)

    // TODO: update player ammo (HUD)
    // TODO: play sound

    player->recoil_force += player->weapon->recoil;

    // TODO: gamepad rumble?

#ifdef KRUNKNATIVE_CLIENT
    const float rand_recoil = ((float) pcg32_random() / (float) UINT32_MAX - 0.5f) * 2.0f * (float) M_PI;

    player->recoil.x += player->weapon->recoil_r * sinf(rand_recoil);
    player->recoil.z += player->weapon->recoil_r * 0.3f * cosf(rand_recoil);

    if (player->render_you) {
        player_feel_recoil(player);
        player_cancel_inspect(player);
    }
#endif
    // TODO: muzzle flash & casings

    const int is_projectile = player->weapon->projectile && (!player->weapon->projectile_disable || player->game->config.bullet_drop);
    const float shot_height = player->position.y + player->height - game_constants.camera_height;

    vec2 shot_angles = {};

    if (is_projectile) {
        // config.projSpreads[shotCntP % 12] scales the spread of each projectile.
        static const float proj_spreads[] = {0.1f, -0.8f, 0.3f, 1.0f, 0.1f, -0.4f, -0.1f, 0.3f, 0.5f, -0.45f, 0.2f, -0.2f};
        player->shot_count++;
        const float projectile_spread = proj_spreads[player->shot_count % (sizeof(proj_spreads) / sizeof(proj_spreads[0]))];
        const float spread = (player->spread + player->weapon->inaccuracy) * game_constants.spread_adjustment * projectile_spread;

        shot_angles.x = player->direction.x + player->recoil_anim_y * game_constants.recoil_mlt + spread;
        shot_angles.y = player->direction.y + spread;

        const vec3 origin = {player->position.x, shot_height, player->position.z};
        game_spawn_projectile(player->game, player, combat_weapon_index(player), projectile_config_for(player->weapon, 0),
                              origin, shot_angles.y, shot_angles.x, 0);
    }

    if (!is_projectile || player->weapon->physical_power) {
        // A projectile weapon only casts its physPow ray (weapons.js shots: 0 for the RPG).
        const int rays = is_projectile ? 0 : player->weapon->shots ? (int) player->weapon->shots : 1;
        for (int i = player->weapon->physical_power ? -1 : 0; i < rays; i++) {
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

            combat_fire_ray(player, i, shot_angles.x, shot_angles.y, shot_height);
        }
    }
}

void player_update(Player *player, const float delta) {
    if (!player->active) return;

    player_feel_update(player, delta);

    if (player->input_queue_size) {
        // Detached first: an input can kill the player (kill objects, authority), and
        // player_kill() drops the queue; the rest of a dead player's inputs are discarded.
        Input *queue = player->input_queue;
        const size_t count = player->input_queue_size;
        player->input_queue = NULL;
        player->input_queue_size = 0;

        for (size_t i = 0; i < count && player->active; i++) {
            player_proc_input(player, &queue[i], 0, player->game->move_lock || mode_input_locked(player)); // lockMove / lockPlayer
            player->input_seq = queue[i].seq;
        }

        free(queue);
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
