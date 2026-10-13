#include <shared.h>
#include <world.h> // map-logic WP-B: tris, zones, score zones, terrain, resets
#include <ai.h>
#include <spawnables.h>
#include <zombies.h> // map-logic WP-H: finiteAmmo, double points, machines, airdrops
#include <game_config.h> // WP-E: config.classes, forceC
#include <ks_game.h> // KrunkScript hook points (onPlayerUpdate / Spawn / Damage / Death)
#include <triggers_v7.h> // WP-C: onPlayerSpawn / Damage / Die / Kill map triggers
#include <player_card.h> // WP-E: the death screen's life accuracy
#include <piss.h> // KrunkNative Piss: the stream and its puddles
#include <math.h>
#include <pcg_basic.h>
#include <stdio.h>
#include <string.h>
#include <ruleset.h>

// Krunker combat constants (v3.1.1 / v7.1.4 src/config.js), used by the spread update and
// the hitscan/damage code below. Head / leg zones: shared/src/effects.c (player_hit_*).
#define COMBAT_HEAD_SCALE 2.0f      // config.headScale: headshot band below the top of the hitbox
#define COMBAT_THROW_CHARGE 0.9f    // v7 config.throwCharge (s): knife-throw charge of a class without throwChrg
#define COMBAT_OVER_CHARGE 1.2f     // v7 config.overCharge: ovrChrg weapons reset at chrgTime * 1.2
#define COMBAT_DENSITY_FLESH 0.2f   // config.materialDens.flesh
#define COMBAT_DENSITY_DEFAULT 0.5f // config.materialDens.default
#define COMBAT_FIST_DAMAGE 35.0f    // melee without the knife (after a throw)
#define COMBAT_SPREAD_MOVE 25.0f    // config.spreadMove
#define COMBAT_SPREAD_FALL 30.0f    // config.spreadFall
#define COMBAT_SPREAD_RECOVER 0.985f// config.spreadRecover (per ms)
#define COMBAT_SPREAD_MLT 240.0f    // config.spreadMlt (recoilAnim term)
#define COMBAT_CROUCH_SPREAD 0.55f  // config.crouchSpread
// v7.1.4 Grappler (players.js tryGrapple, player.js procInputs grapple)
#define GRAPPLE_MIN_RANGE 35.0f     // the rope reels in while range > 35
#define GRAPPLE_REEL_SPEED 0.04f    // units of rope per ms (range -= delta * 0.04)
#define GRAPPLE_RELEASE_SLACK 0.2f  // after the collisions: getD3D - range >= 0.2 lets go (blocked)


#ifdef KRUNKNATIVE_CLIENT
#include <client.h>
#endif

void player_melee(Player*);
void player_shoot(Player*);
static void combat_spawn(Player*, const ClassConfig*);
static void combat_update_charge(Player*, const Input*, float delta);
static void combat_try_grapple(Player*, int fire);
static void player_grapple_pull(Player*, float delta_ms, float decel);
static double grapple_d3d(vec3 a, vec3 b);
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
    player->cosmetics_v7 = PLAYER_COSMETICS_V7_NONE;
    player->show_badges = PLAYER_SHOW_BADGES_ALL;
    player->skins = PLAYER_SKINS_NONE;
    player->last_kill_time = -1e6f; // Krunker lastKill starts far in the past (no multi-kill on the first kill)
    player->last_spray = -1e6f;     // lastSpray 0 against Date.now(): the first spray is never on cooldown
    for (int i = 0; i < PLAYER_DAMAGE_SLOTS; i++) player->damage_received[i].uid = -1;
    player->lag_comp = 1.0f;                        // players.js: this.lagComp = 1
    player->c_send_rate = GAME_LAG_COMP_SEND_RATE;  // this.cSendRate = 10
    player->head_mlt = 1.0f;                        // v7.1.4 headMlt
    player_set_streak_indexes(player, NULL);        // v7.1.4 streakIndex1..3 defaults (Nuke only)
    player->pc_stat_index = -1;                     // WP-E: no player card stat until the server sets one
    effects_reset(player);

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
    // A new life: no input processed yet (gameserver.ts addPlayer / respawn: _lastDecodedSN = -1,
    // inputSN 0 = no "l" until procInputs drained one); the client restarts its counter at 0.
    player->input_seq = -1;
    player->recon_snap = 0;
    player->has_shot_vel_obj = 0;
    player->state_history_count = 0; // players.js init: this.stateHistory.length = 0
    player->state_history_head = 0;
    lag_comp_clock_reset(&player->lag_clock); // a new life restarts its input seq at 0

    player->active = 1;
    // WP-E death screen: player.js reset() of the life counters, spawnTime = Date.now()
    player->life_start = player->game->now;
    player->life_shots = player->life_hits = player->life_kills = player->life_score = 0;
    // player.js init (:434-445): interProg = 0, makingProg = doingInter = false. A hold that
    // was under way when the player died does not carry into the new life.
    player->interact_progress = 0.0f;
    player->making_progress = 0;
    player->doing_interact = 0;

    const size_t class_count = player->game->class_count ? player->game->class_count : rules_class_count();
    const ClassConfig *classes = player->game->classes ? player->game->classes : rules_classes();
    const size_t weapon_count = player->game->weapon_count ? player->game->weapon_count : rules_weapon_count();
    Weapon **weapons = player->game->weapons ? player->game->weapons : (Weapon**) rules_weapons();

    // setClass (v3.1.1 src/data/players.js): mode overrides, then primary + optional secondary + knife.
    // WP-E (v7 player.js:649-664): config.classes first (classes[0] when not listed), and the
    // mode's classes only with forceC.
    player->class_index = game_config_class_for(player->game, player->class_index);
    if (player->game->mode && player->game->config.force_classes) {
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
    // v7.1.4 weapons.js req: customOnly secondaries (Grappler) only in a custom game that is not
    // comp (`game.isCustom && !game.isComp`; KN's hosted games and the offline sandbox are custom,
    // game_config_is_custom; official restrictSecs has no KN equivalent, the isDev bypass is
    // per account) and noComp ones stay out of comp lobbies (`!this.noComp || !game.isComp`).
    const int ranked = player->game->config.comp;
    const int custom = game_config_is_custom(player->game) && !ranked;
    const int secondary_index = picked_secondary >= 0 && picked_secondary < (int) weapon_count && weapons[picked_secondary] && weapons[picked_secondary]->secondary &&
        (!weapons[picked_secondary]->custom_only || custom) && !(ranked && weapons[picked_secondary]->no_comp)
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
    combat_spawn(player, class_config); // WP-B: v7 throwCharge, effects / perks / juggernaut reset
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
        player->ammo[i] = (unsigned int) mode_ammo_limit(player->game, (int) weapons[player->loadout[i]]->ammo); // v7 ammoLimit (aon 0)
    }
    player->loadout_base = loadout_size;
    player_loadout_append_extras(player); // KrunkNative Object Manager: extra weapons after the class loadout

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
    player_end_grapple(player); // Player.reset: grapple = null

    player->air_time = 0.0f;
    player->covered_distance = 0.0f;
    player->aim_time = 0.0f;

    player->swap_timer = 0.0f;
    player->slide_timer = 0.0f;
    player->jump_timer = 0.0f;
    player->reload_timer = 0.0f;

    player_update_height(player);
    player_movement_spawn(player); // WP-A: wall-jump limits and v7 movement state
    player_swap_weapon(player, 0, 1, 0, 0);

    // getSpawnPoint(player.team, player): every mode, offline and server, bots included.
    vec3 spawn_position;
    int spawn_direction;
    if (game_get_spawn_point(player->game, player, &spawn_position, &spawn_direction)) {
        player->position = spawn_position;
        player->direction.x = 0.0f;
        player->direction.y = -(float) M_PI / 2.0f * (1.0f + (float) spawn_direction);
    }

    // shared/src/modes: the mode's loadout (Gun Game tier, Sharp Shooter, Stalker).
    mode_player_spawned(player->game, player);
    // map-logic WP-H (Player.reset): full finiteAmmo reserves, no shared boxes
    player->shared_boxes = 0;
    player->sharing_is_caring = 0;
    zombies_reserves_reset(player);
    if (zombies_finite_ammo(player)) zombies_send_reserves(player);

    // A player that reached a check point respawns there (shared/src/world.c).
    if (game_is_authority(player->game)) game_spawn_at_checkpoint(player->game, player);
    world_player_spawned(player->game, player); // WP-B: setInitPos, noPVP / forcePVP off
    ks_game_on_player_spawn(player->game, player); // KrunkScript: player script state reset, server onPlayerSpawn
    // onPlayerSpawn map triggers: v7.1.4 never dispatches them (a reconstruction gap); KN does, from the
    // authority's spawn (doc/v7/decisions.md "player spawn" trigger event; zombie_high relies on it)
    if (game_is_authority(player->game)) trigger_player_event(player->game, TRIGGER_EVENT_ON_PLAYER_SPAWN, player);
}

void player_queue_input(Player *player, const Input *input) {
    Input *new_input_queue = realloc(player->input_queue, (player->input_queue_size + 1) * sizeof(Input));
    if (!new_input_queue) return;

    player->input_queue = new_input_queue;
    player->input_queue[player->input_queue_size++] = *input;
}

// v3.1.1 server/inputDecode.ts decodeQ for one frame from the network. Rejects non-finite
// fields; clamps delta to [0, max_delta] (sanitizeDelta; procInputs clamps again); drops
// seq <= the last processed or queued seq (_lastDecodedSN), except a counter restart
// (maybeRebaseForSNRestart: far below the floor, or back near zero), which clears the stale
// queue; keeps at most PLAYER_INPUT_QUEUE_MAX frames by dropping the oldest (trimInputQueue).
// v3.1.1 has no speed-hack check and neither does this.
#define PLAYER_INPUT_QUEUE_MAX 240   // MAX_QUEUED_INPUTS
#define PLAYER_INPUT_SEQ_RESTART 100 // SN_RESTART_THRESHOLD

int player_accept_input(Player *player, Input *input) {
    if (!player || !input) return 0;
    if (!isfinite(input->delta) || !isfinite(input->x_dir) || !isfinite(input->y_dir) || !isfinite(input->slowdown)) return 0;
    const float raw_delta = input->delta; // before the clamp: a longer frame breaks the lag comp clock
    input->delta = input->delta < 0.0f ? 0.0f : MIN(input->delta, game_constants.max_delta);
    input->x_dir = player_clamp_pitch(input->x_dir); // a v3.1.1 client never sends more (controls.js)
    input->y_dir = player_wrap_yaw(input->y_dir);
    if (input->move_dir < -1 || input->move_dir > 7) input->move_dir = -1; // no movement key, like an unknown input[4]
    if (input->swap > 4) input->swap = 0;                                  // input[11] codes are 0..4

    int floor = player->input_seq;
    if (player->input_queue_size && player->input_queue[player->input_queue_size - 1].seq > floor) {
        floor = player->input_queue[player->input_queue_size - 1].seq;
    }
    if (floor >= 0 && input->seq <= floor) {
        const int far_below = input->seq < floor - PLAYER_INPUT_SEQ_RESTART;
        const int fresh = input->seq < PLAYER_INPUT_SEQ_RESTART && input->seq < floor;
        if (!far_below && !fresh) return 0; // replay / out of order
        player->input_queue_size = 0;       // restarted counter: the old life's frames are stale
        player->input_seq = input->seq - 1;
    }

    if (player->input_queue_size >= PLAYER_INPUT_QUEUE_MAX) {
        const size_t drop = player->input_queue_size - PLAYER_INPUT_QUEUE_MAX + 1;
        memmove(player->input_queue, player->input_queue + drop, (player->input_queue_size - drop) * sizeof(Input));
        player->input_queue_size -= drop;
    }
    player_queue_input(player, input);
    if (player->input_queue_size && player->input_queue[player->input_queue_size - 1].seq == input->seq) {
        lag_comp_clock_frame(&player->lag_clock, &player->input_queue[player->input_queue_size - 1], raw_delta);
        *input = player->input_queue[player->input_queue_size - 1];
    }
    return 1;
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
    // TWEEN.js measures elapsed time on the ms clock, so a 100 ms tween stepped 50 x 2 ms
    // completes on the 50th frame; summed float seconds can fall a hair short of the
    // duration and finish (and chain the landing's Elastic settle) one frame late.
    const float k = tween->duration > 0.0f && tween->time < tween->duration - MOVEMENT_TIMER_EPSILON ? tween->time / tween->duration : 1.0f;
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

// ---------------------------------------------------------------- v7.1.4 movement
// Krunker v7.1.4 src/data/player.js procInputs (859-2012), players.js setOnGround / resetStep /
// playerStep / jump / tryJump (24-32, 1612-1750) and data/movement.js[0] "Default" (dir / mov /
// vel). v7 values are per ms; KrunkNative keeps delta in seconds and its game_constants scaled
// by 1000, so the code below converts with `dt` (the frame in ms) where v7 mixes per-ms knobs.
// doc/v7/player-viewer.md 1.1.7 lists every change from v3.1.1 this region carries.

#define MOVEMENT_SLIDE_LOCK 0.1f          // canSlideT = 100 ms after a slide starts (resetStep)
#define MOVEMENT_QUAD_SIZE 150.0f         // config.quadSize: the collision grid cell (MapManager)
#define MOVEMENT_PAD_MIN_HALF_HEIGHT 3.0f // addPad: h = max(sy, 3) around the pad's y
#define MOVEMENT_CYLINDER_STEP_EPSILON 0.00001f // cylinder step-up (v3.1.1: only when config.isComp)

static const ClassConfig *movement_class(const Player *player) {
    const Game *game = player->game;
    const size_t count = game->class_count ? game->class_count : CLASS_COUNT;
    const ClassConfig *classes = game->classes ? game->classes : rules_classes();
    if (player->class_index < 0 || (size_t) player->class_index >= count) return NULL;
    return &classes[player->class_index];
}

// this.config: the player's class movement knobs (zeros read as the v7 defaults)
static const ClassMovement *movement_knobs(const Player *player) {
    static const ClassMovement none = {0};
    const ClassConfig *class_config = movement_class(player);
    return class_config ? &class_config->movement : &none;
}

// game.mode movement keys (gameModes/*.js: gravMlt, jumpMlt, groundAccel, frSlid, noSliding,
// noWallJ, wallJ / wallJM / wallJD / wallJV / wallJYV). Multipliers read 1 when unset.
typedef struct {
    float gravity_mlt, jump_mlt, ground_accel;     // gravMlt, jumpMlt, groundAccel (1 = none)
    float wall_jump_decel, wall_jump_vel;          // wallJD, wallJV: 0 = the class value
    float wall_jump_y_vel;                         // wallJYV (1)
    int wall_jump_set, wall_jump, wall_jump_max;   // wallJ / wallJM
    int free_slide, no_sliding, no_wall_jump;      // frSlid, noSliding, noWallJ
} MovementModeKeys;

static MovementModeKeys movement_mode(const Player *player) {
    const Game *game = player->game;
    MovementModeKeys keys = {
        .gravity_mlt = mode_grav_mlt(game),
        .jump_mlt = mode_jump_mlt(game),
        .ground_accel = mode_ground_accel(game),
        .wall_jump_y_vel = 1.0f,
        .free_slide = mode_fr_slid(game),
        .no_sliding = mode_no_sliding(game),
        .no_wall_jump = mode_no_wall_jump(game),
    };
    float wall_jump = 0.0f, wall_jump_max = 0.0f; // modes.h WP-D accessors (chs / imp / clas / zom)
    if (mode_wall_jump(game, &wall_jump, &wall_jump_max, &keys.wall_jump_decel, &keys.wall_jump_vel, &keys.wall_jump_y_vel)) {
        keys.wall_jump_set = 1;
        keys.wall_jump = (int) wall_jump;
        keys.wall_jump_max = (int) wall_jump_max;
    }
    return keys;
}

static int movement_real(const Player *player) {
    return player->game->mode && player->game->mode->config.real_movement;
}

#define MOVEMENT_OR(value, def) ((value) ? (value) : (def))

// The double v7 computes for a value KrunkNative keeps as a float: table values and crouchVal /
// yaw / pitch are decimals with at most 6 digits, so rounding gives JS's exact double back.
// Used where v7's exact double arithmetic decides a tick (the slide's last tick, a move
// direction 90 degrees off the look direction).
static double movement_js(const float value) {
    return round((double) value * 1e6) / 1e6;
}

// slideTimer: the physics counts v7's double ms (slide_ms); slide_timer (s) mirrors it for the
// network, sounds and UI, and a slide_timer written elsewhere (reconciliation) wins.
static double movement_slide_ms(Player *player) {
    if ((float) (player->slide_ms / 1000.0) != player->slide_timer) player->slide_ms = (double) player->slide_timer * 1000.0;
    return player->slide_ms;
}

static void movement_set_slide_ms(Player *player, const double ms) {
    player->slide_ms = ms;
    player->slide_timer = (float) (ms / 1000.0);
}

// this.config.hbS / hbH: the class pads every collision test adds to the player's box
static float movement_pad_w(const Player *player) {
    const ClassConfig *class_config = movement_class(player);
    return class_config ? class_config->hitbox_pad_w : 0.0f;
}

static float movement_pad_h(const Player *player) {
    const ClassConfig *class_config = movement_class(player);
    return class_config ? class_config->hitbox_pad_h : 0.0f;
}

// wallJP: config.wallJP, 0 in noWallJ modes
static float movement_wall_jump_power(const Player *player) {
    return movement_mode(player).no_wall_jump ? 0.0f : player->game->config.wall_jump;
}

// setClass: wallJump = mode.wallJ ?? class wallJ, wallJumpMax = mode.wallJM ?? class wallJM
void player_movement_spawn(Player *player) {
    const ClassConfig *class_config = movement_class(player);
    const MovementModeKeys mode_keys = movement_mode(player);
    const MovementModeKeys *mode = &mode_keys;
    player->wall_jump = mode->wall_jump_set ? mode->wall_jump : class_config && class_config->wall_jump;
    player->wall_jump_max = mode->wall_jump_set ? mode->wall_jump_max : class_config ? class_config->wall_jump_max : 0;
    // Intentional difference: v7 keeps wallJumpC across a death until the next landing, so a life
    // can start with wall jumps already spent; a new life starts with all of them.
    player->wall_jump_count = 0;
    player->can_slide_timer = 0.0f;
    player->on_pad = player->was_on_pad = 0;
    player->was_on_ground = 1;
    player->stop_crouch = player->was_stop_crouch = 0;
    player->height_old = player->height;
}

// A map object's collision box in v7 terms: centre x/z, half width w / length l, bottom and top.
// KrunkNative objects keep the map's bottom-centred p and full size s. v7 sizes a jump pad from
// the map (addPad: w = sx/2, l = sz/2, y = p.y, h = max(sy, 3), so the box spans p.y -+ h; v3.1.1
// used fixed 5 x 1 x 5 half extents) and a cylinder's radius is min(sx, sz) / 2.
typedef struct {
    float x, z, w, l, bottom, top;
} MovementBox;

static void movement_box(const Object *object, MovementBox *box) {
    box->x = object->position.x;
    box->z = object->position.z;
    if (object->jump_pad) {
        const float h = MAX(object->scale.y, MOVEMENT_PAD_MIN_HALF_HEIGHT);
        box->w = object->scale.x * 0.5f;
        box->l = object->scale.z * 0.5f;
        box->bottom = object->position.y - h;
        box->top = object->position.y + h;
    } else {
        const vec3 size = object_box_size(object); // CUSTOM_ASSET / TRIGGER: s * hbw / hbh
        if (object->collision_type == COLLISION_TYPE_CYLINDER) {
            box->w = box->l = MIN(size.x, size.z) * 0.5f;
        } else {
            box->w = size.x * 0.5f;
            box->l = size.z * 0.5f;
        }
        box->bottom = object->position.y;
        box->top = object->position.y + size.y;
    }
}

static float movement_border(const Player *player, const Object *object) {
    return object->is_border && !player->game->config.disable_borders ? game_constants.border_height : 0.0f;
}

// MapManager.getGridArrays(x, y, z): the player only tests the objects registered in its grid
// cell. applyToQuad registers an object in every config.quadSize cell its box reaches, inflated
// by playerScale (config, not the class pad) + 0.1 sideways, playerHeight + 0.1 vertically and
// borderH above a border (disableB or not).
static int movement_in_cell(const int cell[3], const MovementBox *box, const Object *object) {
    const float half_h = (box->top - box->bottom) * 0.5f, centre_y = (box->top + box->bottom) * 0.5f;
    const float ex = box->w + game_constants.player_scale + 0.1f, ez = box->l + game_constants.player_scale + 0.1f;
    const float ey = half_h + game_constants.player_height + 0.1f;
    const float border = object->is_border ? game_constants.border_height : 0.0f;
    return cell[0] >= (int) floorf((box->x - ex) / MOVEMENT_QUAD_SIZE) && cell[0] <= (int) floorf((box->x + ex) / MOVEMENT_QUAD_SIZE) &&
           cell[2] >= (int) floorf((box->z - ez) / MOVEMENT_QUAD_SIZE) && cell[2] <= (int) floorf((box->z + ez) / MOVEMENT_QUAD_SIZE) &&
           cell[1] >= (int) floorf((centre_y - ey) / MOVEMENT_QUAD_SIZE) && cell[1] <= (int) floorf((centre_y + ey + border) / MOVEMENT_QUAD_SIZE);
}

// collidesE(obj, scale + hbS, collPad): an inclusive box test for every kind of object
// (cylinders too), reaching borderH above a border.
static int movement_collides_padded(const Player *player, const Object *object, const MovementBox *box, const float pad) {
    const float s = player->scale + movement_pad_w(player);
    const vec3 p = player->position;
    return p.x - s <= box->x + box->w + pad && p.x + s >= box->x - box->w - pad &&
           p.z - s <= box->z + box->l + pad && p.z + s >= box->z - box->l - pad &&
           p.y <= box->top + movement_border(player, object) && p.y + movement_pad_h(player) + player->height >= box->bottom;
}

// collides(obj): cylinders by distance (no border, no hbH), boxes strictly inside sideways.
static int movement_collides(const Player *player, const Object *object, const MovementBox *box) {
    const float s = player->scale + movement_pad_w(player);
    const vec3 p = player->position;
    if (object->collision_type == COLLISION_TYPE_CYLINDER) {
        if (!(p.y <= box->top && p.y + player->height >= box->bottom)) return 0;
        return hypotf(p.x - box->x, p.z - box->z) <= box->w + s;
    }
    return p.x - s < box->x + box->w && p.x + s > box->x - box->w && p.z - s < box->z + box->l && p.z + s > box->z - box->l &&
           p.y <= box->top + movement_border(player, object) && p.y + movement_pad_h(player) + player->height >= box->bottom;
}

// Player.collides / collidesE for code outside the movement loop (interact prompts, sounds,
// modes): pad > 0 is the padded inclusive test, 0 the solid one.
int player_collides(Player *player, const Object *object, const float pad) {
    if (object->collision_type == COLLISION_TYPE_NONE) return 0;
    MovementBox box;
    movement_box(object, &box);
    return pad > 0.0f ? movement_collides_padded(player, object, &box, pad) : movement_collides(player, object, &box);
}

// players.setOnGround (lastGround is only read by v7 modes KrunkNative does not have yet)
static void movement_set_on_ground(Player *player) {
    player->on_ground = 1;
}

// players.resetStep: a landing (only from the air, and only moving down: v3.1.1 also slid when a
// grounded player stepped down a ramp or ledge) resets the wall-jump count, plays the land feel
// and starts a slide when crouched, then locks new slides for 100 ms. Fall damage is the
// authority's (players.js isServer).
void player_reset_step(Player *player, const int recon) {
    if (player->was_on_ground || !(player->last_position.y > player->position.y)) return;

    const float land_velocity = player->velocity.y;
    player->wall_jump_count = 0;

    if (!recon && player->render_you && land_velocity) player_feel_land(player);
    if (!recon && land_velocity) player->land_events++; // step_N at 0.7 (sound_events.c)

    Game *game = player->game;
    const MovementModeKeys mode_keys = movement_mode(player);
    const MovementModeKeys *mode = &mode_keys;
    const int no_sliding = mode->no_sliding && !player_has_perk(player, PERK_SLIDE) && !player_has_perk(player, PERK_SLIDE_EXPLODE);
    const int slide = !movement_real(player) && player->crouch_val && player->can_slide && player->can_slide_timer <= 0.0f &&
                      game->config.can_slide && !no_sliding;

    if (slide) {
        const ClassConfig *class_config = movement_class(player);
        player->can_slide = 0;
        player->can_slide_timer = MOVEMENT_SLIDE_LOCK;
        // config.slideTime * (slidTime || 1) * (slT || 1) * crouchVal, in v7's double ms
        movement_set_slide_ms(player, movement_js(game_constants.slide_time * 1000.0f) * movement_js(MOVEMENT_OR(game->config.slide_time, 1.0f)) *
                                      movement_js(class_config ? MOVEMENT_OR(class_config->slide_time_mlt, 1.0f) : 1.0f) * movement_js(player->crouch_val));
        // Intentional difference: v7 reads the players manager's onTerrain here (always undefined,
        // so terrain never weakens the slide boost); this reads the player's.
        float slide_mlt = (player->on_terrain ? game_constants.player_terrain_slide_velocity_mlt : game_constants.player_slide_velocity_mlt) *
                          (class_config ? MOVEMENT_OR(class_config->slide_accel_mlt, 1.0f) : 1.0f);
        slide_mlt *= game->config.slide_speed; // game.config.slidSpd || 0
        player->velocity.x *= slide_mlt;
        player->velocity.z *= slide_mlt;
    }

    if (game_is_authority(game) && game->config.fall_damage > 0.0f) {
        float damage = fabsf(land_velocity);
        if (damage > game->config.fall_damage_threshold) {
            damage *= 10.0f * game->config.fall_damage;
            damage = floorf(damage * 10.0f + 0.5f) / 10.0f; // changeHealth: dmg.round(1)
            if (player_change_health(player, NULL, damage, 0)) player_kill(player, NULL, NULL, 0);
        }
    }
}

// The walls a ladder climb or wall jump can use (map.c wall_jumpable = !notWallJ) and the solid
// objects of the v7 collision chain.
void player_do_map_collisions(Player *player, const Input *input, const float move_dir, const int can_jump, const float delta, const int recon) {
    Game *game = player->game;
    const float dt = delta * 1000.0f;
    const float pad_w = movement_pad_w(player), pad_h = movement_pad_h(player);
    const float s = player->scale + pad_w;
    const int has_move = input->move_dir >= 0;
    const ClassMovement *knobs = movement_knobs(player);
    player->was_on_ground = can_jump ? 1 : 0;

    const int cell[3] = {
        (int) floorf(player->position.x / MOVEMENT_QUAD_SIZE),
        (int) floorf(player->position.y / MOVEMENT_QUAD_SIZE),
        (int) floorf(player->position.z / MOVEMENT_QUAD_SIZE),
    };
    int has_ramp_height = 0, side_pushed = 0;
    float max_ramp_height = 0.0f;

    world_player_tris(player);        // WP-B: the tris (cpx) capsule pass before the objects (player.js:1337-1350)
    world_player_colls_begin(player); // WP-B: collidingObjs of onEnterObject / onExitObject

    for (size_t i = 0; i < game->map->object_count; i++) {
        const Object *object = game->map->objects[i];
        if (object->complex_coll) continue; // WP-B: cpx objects collide through their tris only (applyToQuad skips them)
        MovementBox box;
        movement_box(object, &box);
        const float top = box.top, bottom = box.bottom;

        if (!object->active || object->collision_type == COLLISION_TYPE_NONE || !movement_in_cell(cell, &box, object) ||
            !movement_collides_padded(player, object, &box, game_constants.collision_padding)) {
            if (player->in_object == (int) i + 1) player->in_object = 0; // left the onEnter trigger
            continue;
        }
        world_player_colls_touch(player, (int) i); // WP-B: vA541 (saveColls)
        if (object->prefab == PREFAB_PERK_MACHINE) zombies_perk_machine_touch(game, player, (int) i); // WP-H gotFreeP

        const int event = game_object_event(object);

        // (the gate / bank / terminal interact prompts: game_interact_prompt, shared/src/interact.c)

        if (event == TRIGGER_EVENT_ON_COLLISION) {
            // onCollision: game.now - lastColT >= onColT ((constantEvent || 1) s); lastColT starts at 0
            TriggerData *trig = object->trig;
            const float now = game->now * 1000.0f;
            const float cooldown = (float) (trig->constant_event ? trig->constant_event : 1) * 1000.0f;
            if (!trig->last_collision || now - trig->last_collision >= cooldown) {
                trig->last_collision = fmaxf(now, 1e-3f);
                game_on_trigger(game, player, (Object *) object, NULL);
            }
        }

        if (event == TRIGGER_EVENT_ON_INTERACT) {
            // onInteract: hold M1 on the ground for (constantEvent || 1) s (changeInteract type 4,
            // the prompt and #interactProg come from game_interact_prompt / interact_progress)
            if (input->shoot && player->on_ground) {
                const float interact_ms = (float) (object->trig->constant_event ? object->trig->constant_event : 1) * 1000.0f;
                player->doing_interact = 1;
                player->making_progress = 1;
                player->interact_progress += delta * 1000.0f / interact_ms;
                if (player->interact_progress >= 1.0f) {
                    player->interact_progress = 0.0f;
                    game_on_trigger(game, player, (Object *) object, NULL);
                }
            }
            continue; // walk-through: the jumpPad / zone / collision else-chain is skipped
        }

        const int exit_event = object->trig && object->trig->event_index == TRIGGER_V7_EVENT_ON_EXIT_OBJECT;
        if (event == TRIGGER_EVENT_ON_ENTER || exit_event) {
            // v7 onEnterObject / onExitObject (WP-B): collidingObjs; onEnterObject fires the tick
            // the object joins it, onExitObject when it leaves (world_player_colls_end). Nothing
            // else of the object applies (the jumpPad / zone / collision else-chain is skipped)
            if (world_player_colls_enter(player, (int) i) && event == TRIGGER_EVENT_ON_ENTER) game_on_trigger(game, player, (Object *) object, NULL);
            player->in_object = (int) i + 1;
            continue;
        }

        if (object->jump_pad) {
            // v7 jump pads push along d: 0 up, 1 down, 2 +x, 4 -x, 3 +z, 5 -z (the pitch/yaw maths
            // of player.js, evaluated). A horizontal pad only writes the non-zero axis (x pads also
            // zero z: v7 writes bounce * cos(PI/2) there); bm 0 pads do nothing sideways.
            const float bounce = object->bounce * 0.1f / (player->crouch_val == 1.0f && object->crouch ? 2.0f : 1.0f);
            switch (object->direction) {
                case 1: player->velocity.y = -bounce; break;
                case 2: if (bounce) { player->velocity.x = bounce; player->velocity.z = 0.0f; } break;
                case 3: if (bounce) player->velocity.z = bounce; break;
                case 4: if (bounce) { player->velocity.x = -bounce; player->velocity.z = 0.0f; } break;
                case 5: if (bounce) player->velocity.z = -bounce; break;
                default: player->velocity.y = bounce; break;
            }
            player->on_ground = 0;
            // TODO: bounce sound (bounce_0 once per pad entry: !was_on_pad)
            player->on_pad = 1;
        } else if (object->score_zone) {
            world_player_score_zone(player, object); // WP-B: scoreObjs once, score(scoreP), logTime
        } else if (
            !object->objective && !object->bomb_site &&
            // WP-B: premium / verified / partner / KPD / team / KR zones pass the qualifying players
            !world_zone_passable(player, object)
        ) {
            if (object->teleporter) {
                game_check_teleport(game, player, (Object *) object);
                continue;
            }

            if (object->checkpoint) {
                game_set_checkpoint(game, player, (Object *) object);
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

            // A TRIGGER without onEnter / onInteract is a solid block (objects.js addTrigger:
            // Krunker's `trigger` flag in this chain is the CTF flag zone, not this prefab).

            if (object->kill) {
                // TODO: reset flag
                if (!player->god_mode) player_kill(player, NULL, NULL, 0);
                continue;
            }

            if (object->ladder) {
                // below the top, not crouched, not grappling. v7 dropped the bottom clamp:
                // climbing down carries on below the ladder.
                if (!(player->position.y < top) || player->crouch_val != 0.0f || player->grapple.active) continue;

                player->velocity.y = 0.0f;
                player->on_ladder = 1;
                player->on_terrain = 0;

                if (has_move) {
                    const float ladder_dir = (float) M_PI * 0.5f * (float) object->direction;
                    const float dir = (fabsf(normalize_angle(ladder_dir - (move_dir - player->direction.y))) - (float) M_PI * 0.5f) / ((float) M_PI * 0.5f);

                    if (dir > 0.0f) {
                        player->position.y += game_constants.ladder_speed * MOVEMENT_OR(knobs->ladder_accel, 1.0f) * player->weapon->speed_mlt * dir * delta;
                        if (player->position.y >= top) player->position.y = top;
                    }
                }

                continue;
            }

            if (object->no_player_col) continue; // noPCol: walk-through

            // ropeLadder: climbable from any side, relative to the player's own facing; still solid
            if (object->rope_ladder && player->position.y < top && player->crouch_val == 0.0f) {
                player->velocity.y = 0.0f;
                player->on_ladder = 1;
                player->on_terrain = 0;
                if (has_move) {
                    const float facing = atan2f(cosf(player->direction.y), sinf(player->direction.y)); // getDir(sin, cos, 0, 0)
                    const float dir = (fabsf(normalize_angle(facing - (move_dir - player->direction.y))) - (float) M_PI * 0.5f) / ((float) M_PI * 0.5f);
                    if (dir > 0.0f) {
                        player->position.y += game_constants.ladder_speed * MOVEMENT_OR(knobs->ladder_accel, 1.0f) * player->weapon->speed_mlt * dir * delta;
                        if (player->position.y >= top) player->position.y = top;
                    }
                }
            }

            // Wall-jump surfaces: every solid but ramps and cylinders, unless notWallJ (wj)
            // (v3.1.1: only blocks built wallJumpable), higher than climbHeight above the feet.
            if (!object->ramp && object->collision_type != COLLISION_TYPE_CYLINDER && !can_jump && player->position.y < top &&
                top - player->position.y > game_constants.climb_height && object->wall_jumpable) {
                if (player->last_position.x - s >= box.x + box.w - game_constants.collision_padding) {
                    player->on_wall = 1;
                } else if (player->last_position.x + s <= box.x - box.w + game_constants.collision_padding) {
                    player->on_wall = 2;
                } else if (player->last_position.z - s >= box.z + box.l - game_constants.collision_padding) {
                    player->on_wall = 3;
                } else if (player->last_position.z + s <= box.z - box.l + game_constants.collision_padding) {
                    player->on_wall = 4;
                }
            }

            if (!movement_collides(player, object, &box)) continue;

            if (object->ramp) {
                if (!(player->position.y < top)) continue;

                const float ramp_direction = (float) M_PI * 0.5f * (float) object->direction;
                const vec2 player_dir = {player->position.x + s * cosf(ramp_direction), player->position.z + s * sinf(ramp_direction)};

                const float progress = MAX(0.0f, MIN(1.0f, progress_on_line(object->ramp->start, object->ramp->end, player_dir)));
                const float collision_height = bottom + (object->scale.y + 0.01f) * progress;

                if (!has_ramp_height) {
                    has_ramp_height = 1;
                    max_ramp_height = collision_height;
                }

                if ((player->position.y <= collision_height || can_jump) && max_ramp_height <= collision_height) {
                    max_ramp_height = collision_height;

                    if (object->ramp->boost) {
                        player->position.y = collision_height;

                        const float boost_velocity = object->ramp->boost * game_constants.booster_speed * delta;
                        const float boost_direction = asinf(object->scale.y / hypotf(object->scale.y, object->scale.z));

                        player->velocity.x += boost_velocity * sinf(-ramp_direction + (float) M_PI * 0.5f) * cosf(boost_direction);
                        player->velocity.z += boost_velocity * cosf(-ramp_direction + (float) M_PI * 0.5f) * cosf(boost_direction);
                        player->velocity.y += boost_velocity * sinf(boost_direction);
                    } else {
                        player_reset_step(player, recon); // before the snap: lands only when the ramp is below the old y

                        player->position.y = collision_height;
                        player->velocity.y = 0.0f;
                        player->on_wall = 0;
                        movement_set_on_ground(player);
                        player->on_terrain = 0;

                        if (!player->ramp_fix) player->ramp_fix = calloc(1, sizeof(float));

                        if (player->ramp_fix) {
                            player->on_ramp = 1;
                            *player->ramp_fix = bottom + object->scale.y * roundf(progress);
                        }
                    }
                }

                continue;
            }

            if (object->collision_type == COLLISION_TYPE_CYLINDER) {
                int y_collision = 0;

                if (player->last_position.y >= top) {
                    player->position.y = top;
                    player_reset_step(player, recon);
                    if (!player->was_on_pad) player->velocity.y = 0.0f;
                    movement_set_on_ground(player);
                    player->on_terrain = 0;
                    y_collision = 1;
                } else if (player->last_position.y + player->height + pad_h <= bottom) {
                    player->position.y = bottom - player->height;
                    if (!player->was_on_pad) player->velocity.y = 0.0f;
                } else if (player->position.y < top && top - player->position.y <= game_constants.climb_height && player->last_position.y < top && can_jump) {
                    player->position.y += (top - player->position.y) * 0.3f + MOVEMENT_CYLINDER_STEP_EPSILON;
                    movement_set_on_ground(player);
                    player->on_terrain = 0;
                    y_collision = 1;
                } else {
                    const float direction = atan2f(player->position.z - box.z, player->position.x - box.x);

                    player->position.x = box.x + (box.w + s) * cosf(direction);
                    player->position.z = box.z + (box.w + s) * sinf(direction);

                    // a second side push in the same tick (another cylinder) stops the player
                    if (side_pushed) {
                        player->velocity.x = 0.0f;
                        player->velocity.z = 0.0f;
                    }
                    side_pushed = 1;
                }

                if (y_collision) {
                    const float direction = atan2f(player->position.z - box.z, player->position.x - box.x) - object->spin * delta * game_constants.disk_spin;
                    const float distance = hypotf(player->position.x - box.x, player->position.z - box.z);

                    const float old_x = player->position.x;
                    const float old_z = player->position.z;

                    player->position.x = box.x + distance * cosf(direction);
                    player->position.z = box.z + distance * sinf(direction);

                    player->x_vel_cylinder = player->position.x - old_x;
                    player->z_vel_cylinder = player->position.z - old_z;
                }

                continue;
            }

            const float border = movement_border(player, object);
            if (
                !border && player->position.y < top && top - player->position.y <= game_constants.climb_height &&
                player->last_position.y < top && can_jump
            ) {
                player->position.y += dt * 0.03f;
                if (player->position.y > top) player->position.y = top;

                movement_set_on_ground(player);
                player->on_terrain = 0;
            } else if (player->last_position.y >= top + border) {
                // TODO: step src
                player->position.y = top + border;
                player_reset_step(player, recon);
                if (!player->was_on_pad) player->velocity.y = 0.0f;
                movement_set_on_ground(player);
                player->on_terrain = 0;
            } else if (player->last_position.x - s >= box.x + box.w - 0.00001f) {
                player->position.x = box.x + box.w + s;
                player->velocity.x = 0.0f;
            } else if (player->last_position.x + s <= box.x - box.w + 0.00001f) {
                player->position.x = box.x - box.w - s;
                player->velocity.x = 0.0f;
            } else if (player->last_position.z - s >= box.z + box.l - 0.00001f) {
                player->position.z = box.z + box.l + s;
                player->velocity.z = 0.0f;
            } else if (player->last_position.z + s <= box.z - box.l + 0.00001f) {
                player->position.z = box.z - box.l - s;
                player->velocity.z = 0.0f;
            } else if (player->last_position.y + player->height + pad_h <= bottom) {
                // a ceiling: with headClipFix the pre-crouch height keeps an uncrouching player
                // under it, and crouch and jump wait a tick (stopCrouch)
                player->stop_crouch = 1;
                player->position.y = bottom - (player->was_stop_crouch && game->config.head_clip_fix ? player->height_old : player->height) - pad_w;
                if (!player->was_on_pad) player->velocity.y = 0.0f;
            } else if (game->config.head_clip_fix && player->last_position.y + player->height_old + pad_h <= bottom) {
                player->stop_crouch = 1;
                player->position.y = bottom - player->height - pad_w;
                if (!player->was_on_pad) player->velocity.y = 0.0f;
            }
        }
    }

    world_player_colls_end(player);  // WP-B: onExitObject for the objects left this tick
    airdrop_player_collide(player, input, delta); // WP-H: the airdrop claim hold (player.js:1718)
    world_player_pvp_zones(player);  // WP-B: noPVP / forcePVP (COLLISION_TYPE_NONE volumes)
}

// players.jump: blocked by a noJump weapon (Minigun) and for a tick after a headClipFix ceiling
// hit; Jump Boost / Rooted effects and the mode's jumpMlt / jumpVel scale it; ovrJump sets the
// vertical velocity instead of adding to it. jumpCooldown is jpC * 0 in v7 (always 0).
void player_jump(Player *player) {
    if ((player->weapon && player->weapon->no_jump) || player->was_stop_crouch) return;

    Game *game = player->game;
    const MovementModeKeys mode_keys = movement_mode(player);
    const MovementModeKeys *mode = &mode_keys;

    player->did_act = 1;
    player->jump_timer = 0.0f;
    player->did_jump = 1;
    player->did_wall_jump = 1;
    player->on_terrain = 0;

    const float attr_jump = MOVEMENT_OR(player_attr_jump(player), 1.0f);
    const float mode_jump = MOVEMENT_OR(mode->jump_mlt, 1.0f);
    // mode.jumpVel (no v7.1.4 mode sets it) is not among the WP-D mode keys yet
    float jump_vel = game_constants.jump_velocity * game->config.jump_mlt * attr_jump * mode_jump;
    if (movement_real(player)) jump_vel *= 0.92f;
    jump_vel *= player_attr_rooted(player);
    const float jump_push = game_constants.jump_push * game->config.jump_mlt * attr_jump * mode_jump;

    const float up = (jump_vel - jump_vel * (game_constants.crouch_jump * player->crouch_val)) *
                     (player->weapon->jump_mlt ? player->weapon->jump_mlt : player->weapon->speed_mlt) *
                     (player->aim_val != 1.0f ? 1.0f : game_constants.jump_aim_slow);
    if (movement_knobs(player)->override_jump) player->velocity.y = up;
    else player->velocity.y += up;

    const float vel = hypotf(player->velocity.x, player->velocity.z);
    player->velocity.x -= jump_push * vel * sinf(player->direction.y);
    player->velocity.z -= jump_push * vel * cosf(player->direction.y);
}

// players.tryJump: autoJump jumps on every grounded tick the key is held (v7 sends jump while
// held); otherwise the key must be released between jumps.
static void movement_try_jump(Player *player, const int jump, const int recon) {
    const int blocked = (player->weapon && player->weapon->no_jump) || player->was_stop_crouch; // jump() returns early
    if (player->game->config.auto_jump && jump) {
        player_jump(player);
        if (!recon && player->render_you && !blocked) player_feel_jump(player);
        return;
    }
    if (player->did_jump && !jump) player->did_jump = 0;
    if (!player->did_jump && jump) {
        player_jump(player);
        if (!recon && player->render_you && !blocked) player_feel_jump(player);
    }
}

void player_swap_weapon(Player *player, const int index, const int force_swap, const int instant_swap, const int recon) {
    // v7.1.4 swapWeapon: a noSwap weapon in hand (Minigun, War Machine) only gives way to another
    // noSwap one (a loadout change forces the swap). KrunkNative Object Manager: one held in an
    // extra loadout slot does not lock the loadout (v7 only hands them out with a streak).
    const int extra_slot = player->loadout_index >= player_loadout_base(player);
    if (!force_swap && !extra_slot && player->weapon && player->weapon->no_swap && player->loadout && index >= 0 && index < player->loadout_size) {
        const Weapon *target = player->game->weapons[player->loadout[index]];
        if (target && !target->no_swap) return;
    }

    int loadout_changed = force_swap || player->loadout_index != index;
    const int prev_index = player->loadout_index;
    player->loadout_index = index;

    if (!instant_swap && loadout_changed) {
        player->reload_timer = 0.0f;
        player->did_shoot = 0;
        player->burst_count = 0;
        player->charge_time = 0.0f; // v7.1.4: chargeTime = 0, fullCharge = false
        player->full_charge = 0;
    }

    player->weapon = player_slot_weapon(player, player->loadout_index); // v7.1.4 getWeapon: upgraded slots

    if (!player->weapon) {
        player->weapon = player->game->weapons[player->loadout[0]];

        if (player->weapon) {
            player->loadout_index = 0;
            loadout_changed = 1;
        }
    }
    // swapWeapon: !noEndGrapple && the slot changed -> endGrapple (a reconcile's swap keeps it)
    if (!recon && prev_index != player->loadout_index) player_end_grapple(player);

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
    if (player->reload_timer || (int) player->ammo[player->loadout_index] >= mode_ammo_limit(player->game, (int) player->weapon->ammo)) return;
    // v7.1.4 noReload (streak equipment; finiteAmmo reloads it). KrunkNative deviation: in an
    // Object Manager extra slot (v7 has none) it reloads like any weapon. v7 hands these out for
    // one magazine and takes them away empty (removeOnEmpty, tryDeployable's removeWeapon); an
    // extra slot cannot be taken away (player_remove_weapon only edits the base), so without a
    // reload the Slimer / Minigun / War Machine would sit there empty for the rest of the life.
    const int extra_slot = player->loadout_index >= player_loadout_base(player);
    if (player->weapon->no_reload && !zombies_finite_ammo(player) && !extra_slot) return;
    if (!zombies_reload_allowed(player)) return; // WP-H: finiteAmmo with an empty reserve

    // reload * (reSpd || 1) * attributes.reload (Sleight of Hand) * (perk 2 ? 0.5 : 1) * (kranked
    // bonuses.reload) * (mode.reloadMlt) (players.js reload)
    player->reload_timer = player->weapon->reload_time * (player->game->config.reload_speed > 0.0f ? player->game->config.reload_speed : 1.0f) *
                           player_attr_reload(player) * mode_reload_mlt(player->game, player);
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

// v7.1.4 index.js gameLoop, a frame that samples no input frame (netRate): `if ((tmpPlayer.config.movSch
// || 0) == 0 && tmpPlayer.config.trnSp == 1) { yDire = CONTROLS.yDr; xDire = cameraYaw;
// updateMshClient }`. Other schemes and turn speeds keep the look of the last input frame.
int player_look_follows_camera(const Player *player) {
    const ClassMovement *knobs = movement_knobs(player);
    return knobs->scheme == 0 && MOVEMENT_OR(knobs->turn_rate, 1.0f) == 1.0f;
}

// JS Number.prototype.round(3) (2.js): Math.round(v * 1000) / 1000, halves toward +inf.
float player_round3(const float v) {
    // Past 2^23 / 1000 a float has no thousandths left to round, and v * 1000 must not
    // overflow to inf (a huge network yaw used to become inf here, then NaN positions).
    if (!(fabsf(v) < 8388.0f)) return v;
    return floorf(v * 1000.0f + 0.5f) / 1000.0f;
}

// controls.js: xDr = object.rotation.y % Math.PI2 (JS % keeps the sign), so the yaw a client
// sends stays in (-2pi, 2pi) however far it turns. KrunkNative applies it on both sides: an
// unbounded yaw loses float precision, and remote players interpolating between two far-apart
// unwrapped yaws spun on screen ("visual spinbot"). Non-finite (only a broken or malicious
// client) becomes 0 so it can never reach the physics.
float player_wrap_yaw(const float yaw) {
    if (!isfinite(yaw)) return 0.0f;
    if (yaw > -(float) M_PI * 2.0f && yaw < (float) M_PI * 2.0f) return yaw;
    return fmodf(yaw, (float) M_PI * 2.0f);
}

// controls.js: pitchObj.rotation.x = max(-PI_2, min(PI_2, ...)) before yDr is taken.
float player_clamp_pitch(const float pitch) {
    if (!isfinite(pitch)) return 0.0f;
    return CLAMP(pitch, -(float) M_PI / 2.0f, (float) M_PI / 2.0f);
}

// manager.reload(player, Q = +-1) / 95.js pY2(P, Q): the scroll wheel (input[10]) and the
// melee key on melee step the weapon index. Up past ammos.length - (length > 3 ? 3 : 2)
// wraps to 0 (so the melee of a 3+ weapon loadout is never scrolled to); down from slot 0
// goes to min(length - 1, 2), from any other slot straight to 0.
// KrunkNative Object Manager: with extra loadout weapons the wheel steps through every slot and
// wraps at both ends (v7's wrap would never reach most of them).
void player_scroll_weapon(Player *player, const int dir, const int recon) {
    const int length = (int) player->loadout_size;
    if (length <= 1 || !dir) return;

    int index = player->loadout_index;
    if (player_loadout_base(player) < length) {
        index = ((index + (dir > 0 ? 1 : -1)) % length + length) % length;
        player_swap_weapon(player, index, 0, 0, recon);
        return;
    }
    if (dir == 1) {
        index++;
        if (index > length - (length > 3 ? 3 : 2)) index = 0;
    } else if (dir == -1 && index - 1 < 0) {
        index = MIN(length - 1, 2);
    } else {
        index = 0; // Q == -1 from a later slot, or Q outside +-1 (pY2 treats it as 0)
    }

    player_swap_weapon(player, index, 0, 0, recon);
}

void player_proc_input(Player *player, const Input *raw_input, const int recon, const int move_lock) {
    // v3.1.1 sends input[1] as whole ms (Math.round(delta * deltaMlt)) and yaw/pitch as
    // Math.round(v.round(3) * 1000) (index.js b9) and runs procInputs on those values on
    // both sides, so the prediction and the server see the same numbers. KrunkNative's
    // Input carries floats: round them here, on both sides, before anything uses them.
    Input rounded = *raw_input;
    rounded.delta = roundf(raw_input->delta * 1000.0f) / 1000.0f;
    // The client clamps pitch and wraps yaw before rounding (controls.js yDr / xDr); done here
    // again so no input (prediction, replay or a hostile network frame) has an out-of-range look.
    rounded.x_dir = player_round3(player_clamp_pitch(raw_input->x_dir));
    rounded.y_dir = player_round3(player_wrap_yaw(raw_input->y_dir));
    const ClassMovement *knobs = movement_knobs(player);
    const MovementModeKeys mode_keys = movement_mode(player);
    const MovementModeKeys *mode_movement = &mode_keys;
    if (knobs->lock_pitch) rounded.x_dir = 0.0f; // class lckCmYR
    const Input *input = &rounded;
    // KrunkScript onPlayerUpdate + disableDefault masks; disablePlayerBehaviour skips the rest (PLJ:894-948)
    if (player->game->ks && ks_game_on_player_update(player->game, player, &rounded)) return;

    // v7.1.4: Math.max(0.1, Math.min(input[1], config.dltMx)); NaN, 0 and negative deltas run as
    // 0.1 ms (v3.1.1: 0).
    const float delta = input->delta > game_constants.min_delta ? MIN(input->delta, game_constants.max_delta) : game_constants.min_delta;
    // config.movDirs[input[4]] + class movDrP * PI
    const float move_dir = -(float) M_PI / 2.0f + (float) M_PI / 4.0f * (float) input->move_dir + knobs->move_dir_offset * (float) M_PI;

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

    // movement.js[0].dir: below a turn speed of 1 (class trnSp * trnSpd) the look turns at most
    // speed * 0.01 rad per ms
    const ClassConfig *turn_class = movement_class(player);
    const float turn = MOVEMENT_OR(knobs->turn_rate, 1.0f) * (turn_class ? MOVEMENT_OR(turn_class->turn_speed, 1.0f) : 1.0f);
    if (!ks_player_allows(player, KS_PLAYER_DEFAULT_ROTATION)) {
        // KrunkScript defaultRotation = false: the script turns the player (player.js:969)
    } else if (turn >= 1.0f) {
        player->direction.x = CLAMP(input->x_dir, -(float) M_PI / 2.0f, (float) M_PI / 2.0f);
        player->direction.y = input->y_dir;
    } else {
        const float pi2 = (float) M_PI * 2.0f, step = turn * 0.01f * delta * 1000.0f;
        const float d = fmodf(player->direction.y - input->y_dir + pi2, pi2);
        const float move = MIN(MIN(fabsf(d - pi2), d), step);
        player->direction.y = fmodf(player->direction.y + (d - (float) M_PI >= 0.0f ? 1.0f : -1.0f) * move + pi2, pi2);
        if (player->direction.x < input->x_dir) player->direction.x = MIN(player->direction.x + step, input->x_dir);
        else if (player->direction.x > input->x_dir) player->direction.x = MAX(player->direction.x - step, input->x_dir);
    }

    const int feel_loadout_index = player->loadout_index;

    // input[11]: 4 swapEquipment, 2 swapMelee, 1 swapSecondary, 3 primary (pY2 to slot 0),
    // otherwise input[10] scrolls (players.js procInputs).
    if (input->swap == 3) {
        if (player->loadout_size > 1) player_swap_weapon(player, 0, 0, 0, recon);
    } else if (input->swap && player->loadout_size > 1) {
        const int swap_secondary = input->swap == 1;
        const int swap_melee = input->swap == 2;
        const int swap_equipment = input->swap == 4;

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
            // swapSecondary / swapEquipment toggle back to slot 0; swapMelee on melee is
            // reload(player, 1): the next slot (95.js swapMelee).
            if (player->loadout_index != swap_to) player_swap_weapon(player, swap_to, 0, 0, recon);
            else if (swap_melee) player_scroll_weapon(player, 1, recon);
            else player_swap_weapon(player, 0, 0, 0, recon);
        }
    } else if (!input->swap && input->select_slot) {
        // KrunkNative Object Manager: a number key picks a slot (extra loadout weapons)
        const int slot = input->select_slot - 1;
        if (slot < player->loadout_size && slot != player->loadout_index) player_swap_weapon(player, slot, 0, 0, recon);
    } else if (!input->swap && input->scroll) {
        player_scroll_weapon(player, input->scroll, recon);
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

    // v7.1.4 charge system (player.js: right after the aim update, not in replays)
    if (!recon) combat_update_charge(player, input, delta);
    // map-logic WP-B: the Build Tool pick (player.js:1109-1124: aim held with a build weapon)
    player->select_build = 0;
    if (input->scope && player->weapon && player->weapon->build) player_select_build(player, yaw_delta, pitch_delta);

    // v7.1.4: a headClipFix ceiling hit last tick blocks crouching (and jumping) for this one;
    // classes without canCrch never crouch.
    player->was_stop_crouch = player->game->config.head_clip_fix && player->stop_crouch;
    player->stop_crouch = 0;
    const int crouch = input->crouch && !knobs->no_crouch;

    if (ks_player_allows(player, KS_PLAYER_DEFAULT_MOVEMENT) && !player->was_stop_crouch) { // defaultMovement
        if (crouch && player->crouch_val < 1.0f && !player->on_ladder) {
            player->crouch_val = MIN(1.0f, player->crouch_val + game_constants.crouch_speed * delta);
            // v7.1.4 player.js: crouchVal reaching 1 -> if (manager.isServer) game.incStat("crc", this)
            if (player->crouch_val >= 1.0f && !recon) game_inc_stat(player->game, player, "crc");

            if (player->on_ground || player->grapple.active) {
                if (!recon && player->render_you) player->bob_anim.y -= game_constants.crouch_anim * 1.4f * delta;
            } else {
                player->position.y += game_constants.crouch_speed * delta;
            }
        } else if (!crouch && player->crouch_val > 0.0f) {
            player->crouch_val = MAX(0.0f, player->crouch_val - game_constants.crouch_speed * delta);

            if (player->on_ground || player->grapple.active) {
                if (!recon && player->render_you) player->bob_anim.y += game_constants.crouch_anim * delta;
            } else {
                player->position.y -= game_constants.crouch_speed * delta;
            }
        }

        if (player->crouch_val) player->crouch_val = roundf(player->crouch_val * 1000.0f) / 1000.0f; // crouchVal.round(3)
    }

    player->height_old = player->height; // updateHeight: heightOld
    player_update_height(player);

    const int contact = player->on_ground || player->on_ladder;

    if (!move_lock) {
        const float dt = delta * 1000.0f;
        const double dt_ms = round((double) delta * 10000.0) / 10.0; // v7's frame in ms (whole ms, or the 0.1 floor)
        const float ground_accel_mlt = MOVEMENT_OR(mode_movement->ground_accel, 1.0f); // mode.groundAccel: ground and air
        float accel;

        // grA / 100 and arA / 100 per ms (KrunkNative keeps them x1000 per s)
        if (contact) accel = MOVEMENT_OR(knobs->ground_accel, 0.042f) * 10.0f * player->speed * ground_accel_mlt;
        else accel = MOVEMENT_OR(knobs->air_accel, 0.0047f) * 10.0f * (movement_real(player) ? 0.72f : 1.0f) * ground_accel_mlt;

        accel *= player->aim_val == 1.0f ? game_constants.aim_slow : 1.0f;
        accel *= player->game->mode ? MOVEMENT_OR(player->game->mode->config.speed_mlt[player->team], 1.0f) : 1.0f;
        accel *= player->crouch_val ? MOVEMENT_OR(knobs->crouch_speed, game_constants.crouch_slow) : 1.0f;
        accel *= player->weapon->speed_mlt * delta;
        accel *= player->weapon->shoot_charge && input->shoot ? 0.35f : 1.0f; // charging (Charge Rifle)

        float decel;

        if (player->on_ladder) decel = game_constants.ladder_decel;
        else if (player->on_terrain) decel = game_constants.terrain_decel;
        else if (player->on_ground) decel = MOVEMENT_OR(knobs->ground_decel, game_constants.ground_decel);
        else decel = MOVEMENT_OR(knobs->air_decel, game_constants.air_decel);

        if (player->can_slide_timer > 0.0f) {
            player->can_slide_timer -= delta;
            if (player->can_slide_timer <= MOVEMENT_TIMER_EPSILON) player->can_slide_timer = 0.0f;
        }

        if (player->crouch_val <= 0.5f) player->can_slide = 1;

        double slide_ms = movement_slide_ms(player);
        if (!player->on_ground || !player->crouch_val) slide_ms = 0.0;

        if (slide_ms) {
            // TODO (WP-B perks): Drift Fuze (perk 5) explodes every 750 ms while sliding (server)
            slide_ms -= dt_ms;
            if (slide_ms <= 0.0) slide_ms = 0.0;
            movement_set_slide_ms(player, slide_ms);

            if (slide_ms > 0.0) {
                accel *= 0.25f;
                decel = player->on_terrain ? game_constants.terrain_slide_decel : game_constants.slide_decel;

                // v7 slides always follow the look direction (v3.1.1's "Non FPS Sliding" setting
                // slidCont is gone); frSlid slides toward the held movement key, bkSlid backwards
                // on the S keys (movDirs 4..6).
                const float vel = hypotf(player->velocity.x, player->velocity.z);
                float yaw = player->direction.y;
                const int v7_dir = input->move_dir >= 0 ? (input->move_dir + 1) % 8 : -1; // config.movDirs index
                const ClassConfig *class_config = movement_class(player);
                if ((knobs->free_slide || mode_movement->free_slide) && v7_dir >= 0) yaw -= (float) M_PI / 4.0f * (float) (v7_dir - 1);
                else if (class_config && class_config->back_slide && v7_dir >= 4 && v7_dir <= 6) yaw += (float) M_PI;
                const float dir = atan2f(cosf(yaw), sinf(yaw)); // getDir(sin, cos, 0, 0)

                player->velocity.x = vel * cosf(dir + (float) M_PI);
                player->velocity.z = vel * sinf(dir + (float) M_PI);
            }
        }

        movement_set_slide_ms(player, slide_ms);

        player->jump_timer = MAX(0.0f, player->jump_timer - delta);

        if (ks_player_allows(player, KS_PLAYER_DEFAULT_MOVEMENT) && player->jump_timer <= 0.0f && !knobs->disable_jump && (player->on_ground || knobs->infinite_jump)) {
            movement_try_jump(player, input->jump, recon);
        }

        if (!contact) {
            const float wall_power = movement_wall_jump_power(player);
            const float wall_decel = mode_movement->wall_jump_decel ? mode_movement->wall_jump_decel : MOVEMENT_OR(knobs->wall_jump_decel, 0.3f);
            const float wall_mlt = player->velocity.y < 0.0f && player->wall_jump && player->on_wall && wall_power > 0.0f && player->crouch_val ? wall_decel : 1.0f;
            player->velocity.y -= delta * (game_constants.gravity * player->game->config.gravity_mlt * MOVEMENT_OR(mode_movement->gravity_mlt, 1.0f)) * wall_mlt;
        }

        // 95.js: ac *= limitMM(1 - a0[12], 0, 1) (input[12]: the controller's Gradual Speed)
        accel *= CLAMP(1.0f - input->slowdown, 0.0f, 1.0f);
        if (player->noclip) accel *= 2.0f;
        // speedBoost (admin) is not in KrunkNative yet
        accel *= player_attr_speed(player);
        accel *= player_attr_rooted(player);
        accel *= mode_speed_mlt(player->game, player); // Kranked bonuses.speed (player.js:1236, WP-D)
        accel *= player_has_perk(player, PERK_SPEED) ? 1.15f : 1.0f;

        // movement.js[0].mov (defaultMovement): diagonal movDirs get config.strafeSpd unless realMov
        if (ks_player_allows(player, KS_PLAYER_DEFAULT_MOVEMENT) && input->move_dir >= 0) {
            const float yaw_origin = player->direction.y; // TODO: camType 1 without camRot uses the camera offset
            float strafe = 1.0f;
            if (input->move_dir % 2 && !movement_real(player)) strafe = player->game->config.strafe_speed;

            // in double like v7: 90 degrees off the look direction the cosine is a tiny difference
            // of two angles, and float error there drifts the player sideways over a long climb
            const double angle = -M_PI / 2.0 + M_PI / 4.0 * input->move_dir + movement_js(knobs->move_dir_offset) * M_PI - movement_js(yaw_origin);
            player->velocity.x += (float) (accel * strafe * cos(angle));
            player->velocity.z += (float) (accel * strafe * sin(angle));

            // movDir > 0 (movDirs 4..7: back, back-diagonals, left) flies down
            if (player->noclip) player->velocity.y += accel * strafe * (player->direction.x * (input->move_dir >= 3 && input->move_dir <= 6 ? -1.0f : 1.0f));
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

        // Air strafe (config.airStrf, v7 currentSeason >= 7): turning swings the air velocity by
        // the full yaw change while holding right or forward-right (turning right) and left or
        // forward-left (turning left). v3.1.1: x0.73, pure strafe keys only.
        if (!contact && yaw_delta && player->game->config.air_strafe &&
            (((input->move_dir == 2 || input->move_dir == 1) && yaw_delta > 0.0f) || ((input->move_dir == 6 || input->move_dir == 7) && yaw_delta < 0.0f))) {
            const float vel = hypotf(player->velocity.x, player->velocity.z);
            const float dir = atan2f(-player->velocity.z, -player->velocity.x) + yaw_delta;

            player->velocity.x = vel * cosf(dir + (float) M_PI);
            player->velocity.z = vel * sinf(dir + (float) M_PI);
        }

        // movement.js[0].vel
        const float speed_x = MOVEMENT_OR(knobs->speed_x, 1.0f), speed_y = MOVEMENT_OR(knobs->speed_y, 1.0f), speed_z = MOVEMENT_OR(knobs->speed_z, 1.0f);
        if (!ks_player_allows(player, KS_PLAYER_DEFAULT_VELOCITY)) {
            // KrunkScript defaultVelocity = false: no integration or friction (player.js:1264)
        } else if (player->game->config.fixed_movement && !player->noclip) {
            // fixMov: the exact integral of the per-ms decay, x += (v d^t - v) / ln d
            if (player->velocity.x) {
                const float v = player->velocity.x * powf(decel, dt);
                player->position.x += (v - player->velocity.x) / logf(decel) * speed_x;
                player->velocity.x = CROP(v, game_constants.min_decel);
            }
            if (player->velocity.y) {
                const float v = player->velocity.y * powf(0.9993f, dt);
                player->position.y += (v - player->velocity.y) / logf(0.9993f) * speed_y;
                player->velocity.y = v;
            }
            if (player->velocity.z) {
                const float v = player->velocity.z * powf(decel, dt);
                player->position.z += (v - player->velocity.z) / logf(decel) * speed_z;
                player->velocity.z = CROP(v, game_constants.min_decel);
            }
        } else {
            if (player->velocity.x) {
                player->position.x += player->velocity.x * speed_x * dt;
                player->velocity.x *= powf(decel, dt);
                player->velocity.x = CROP(player->velocity.x, game_constants.min_decel);
            }

            if (player->velocity.y) {
                player->position.y += player->velocity.y * speed_y * dt;

                if (player->noclip) {
                    player->velocity.y *= powf(decel, dt);
                } else if (player->velocity.y > 0.0f) {
                    player->velocity.y -= delta * 0.032f;
                } else if (player->velocity.y <= -0.3f) {
                    player->velocity.y = -0.3f;
                }
            }

            if (player->velocity.z) {
                player->position.z += player->velocity.z * speed_z * dt;
                player->velocity.z *= powf(decel, dt);
                player->velocity.z = CROP(player->velocity.z, game_constants.min_decel);
            }
        }

        // player.js:1271-1290: jump lets go, then the rope reels in and holds the player on its sphere
        if (input->jump) player_end_grapple(player);
        if (player->grapple.active) player_grapple_pull(player, dt, decel);

        if (!input->shoot && !recon) player->doing_interact = 0;

        const int can_jump = player->on_ground && !player->did_jump; // wasOnGround

        player->was_on_pad = player->on_pad;
        player->on_ground = player->noclip;
        player->on_ladder = 0;
        player->on_wall = 0;
        player->on_ramp = 0;
        player->on_pad = 0;
        player->making_progress = 0;

        // TODO (pColl): players.playerCollision for classes with player collision
        if (!player->noclip) player_do_map_collisions(player, input, move_dir, can_jump, delta, recon);
        else player->was_on_ground = can_jump ? 1 : 0;
        if (player->noclip) world_player_pvp_zones(player); // WP-B: v7 clears noPVP / forcePVP in noclip too

        // player.js:1756-1762: a collision that kept the player off the rope's sphere lets go
        if (player->grapple.active && grapple_d3d(player->position, player->grapple.point) - player->grapple.range >= GRAPPLE_RELEASE_SLACK) {
            player_end_grapple(player);
        }

        if (!player->did_jump && player->ramp_fix && fabsf(player->position.y - *player->ramp_fix) <= game_constants.climb_height) {
            if (!player->on_ramp) {
                player->position.y = *player->ramp_fix;
                movement_set_on_ground(player);
                player->velocity.y = 0.0f;

                free(player->ramp_fix);
                player->ramp_fix = NULL;
            }
        } else {
            free(player->ramp_fix);
            player->ramp_fix = NULL;
        }

        if (!player->making_progress && !recon) player->interact_progress = 0.0f;
        world_player_terrain(player, recon); // WP-B: terrain ground snap (player.js:1779-1797)

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

        // playerStep: every grounded player that moved at least 0.1 this tick (v3.1.1: the local
        // player holding a movement key; the step sound is sound_events' step_dir_neg flips)
        if (contact && delta_pos >= 0.1f) player_step(player, delta_pos);

        if (player->render_you) {
            if (contact && input->move_dir >= 0) {
                player->lean_anim.z -= delta_pos * game_constants.lean_mlt_z * (player->weapon->z_lean_mlt ? player->weapon->z_lean_mlt : 1.0f) * cosf(move_dir);
            } else {
                player->step_val *= powf(game_constants.step_pull, delta * 1000.0f);
            }
        }

        if (player->step_chase != player->step_val) player->step_chase += (player->step_val - player->step_chase) * 0.15f;

        // Krunker spread: movement/fall spreadPlus x weapon movSpread, crouch and recoil terms,
        // scaled by the hip-fire amount plus adSpread of the aimed share (v7.1.4 player.js;
        // KrunkNative aim_val 1 = aimed, Krunker aimVal 0 = aimed). KN bug fixed: movSpread (Akimbo
        // Uzi 0.01, Akimbo Pistol 0.1) used to be ignored (doc/v7/player-viewer.md 5.2).
        const float move_spread = player->weapon->move_spread ? player->weapon->move_spread : 1.0f;
        player->spread_plus += (delta_pos * COMBAT_SPREAD_MOVE + fabsf(player->last_position.y - player->position.y) * COMBAT_SPREAD_FALL) * move_spread;
        player->spread_plus *= powf(COMBAT_SPREAD_RECOVER, delta * 1000.0f);
        player->spread = player->weapon->no_spread ? 0.0f : MAX(player->weapon->min_spread,
            (player->weapon->spread - player->weapon->spread * COMBAT_CROUCH_SPREAD * player->crouch_val +
             player->recoil_anim * COMBAT_SPREAD_MLT + player->spread_plus) * ((1.0f - player->aim_val) + player->aim_val * player->weapon->aim_spread));

        // Intentional difference: a reconciliation replay (recon) stops here. v3.1.1 runs the
        // reload / timers / shoot code in replays too and relies on the predicted reloads[]
        // timers to keep shots from firing twice (while counting the timers down twice); its
        // physPow knockback comes back through input.velObj below. Here replays never shoot,
        // reload or advance weapon timers: the authority owns ammo and fire, and a replayed
        // shot would duplicate client tracers, projectiles and recoil.
        if (recon) goto weapon_done;

        if (input->reload && !player->game->mode->config.no_reloads) {
            player_reload(player);
        }

        if (player->reload_timer > 0.0f) {
            player->reload_timer = MAX(0.0f, player->reload_timer - delta);

            // TODO: play reload end sound

            if (!player->reload_timer) {
                player->did_shoot = 0;
                if (zombies_finite_ammo(player)) zombies_reload_finish(player); // WP-H: from the reserve
                else player->ammo[player->loadout_index] = (unsigned int) mode_ammo_limit(player->game, (int) player->weapon->ammo);
            }
        }

        player->swap_timer = MAX(0.0f, player->swap_timer - delta);

        for (int i = 0; i < player->loadout_size; i++) {
            player->reloads[i] = MAX(0.0f, player->reloads[i] - delta);
        }

        if (player->weapon && !move_lock && !player->doing_interact) {
            int will_shoot = player->burst_count || !player->weapon->no_auto && input->shoot;

            if (player->did_shoot && !input->shoot) {
                player->did_shoot = 0;
            }

            if (!player->did_shoot && input->shoot) {
                will_shoot = 1;
            }

            // map-logic WP-B: players.build runs whenever the timers allow (player.js:1911-1914),
            // with the fire input as `place`; the Build Tool never takes the shoot path
            if (player->weapon->grapple_range > 0.0f && player->reloads[player->loadout_index] <= 0.0f && player->swap_timer <= 0.0f && player->reload_timer <= 0.0f) {
                combat_try_grapple(player, will_shoot); // player.js:1911: tryGrapple(this, grapRang, fire, input)
            } else if (player->weapon->kn_physgun) {
                // KrunkNative Physics Gun: never fires; the client grabs with the fire button held
                // and sends the poses as Object Manager edits (object_manager.h OM_ACTION_EDIT)
            } else if (player->weapon->build && player->reloads[player->loadout_index] <= 0.0f && player->swap_timer <= 0.0f && player->reload_timer <= 0.0f) {
                player_build(player, will_shoot);
            } else if (will_shoot && player->reloads[player->loadout_index] <= 0.0f && player->swap_timer <= 0.0f && player->reload_timer <= 0.0f) {
                if (player->weapon->build) {
                    // (players.build above)
                } else if (player->weapon->melee) {
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

weapon_done:;
    }

    // procInputs: if (!moveLock && recon && input.velObj) the shot's knockback again (the
    // replay does not shoot, the reconciled velocity predates it).
    if (!move_lock && recon && input->has_vel_obj) {
        player->velocity.x -= input->vel_obj.x;
        player->velocity.z -= input->vel_obj.z;
        if (!player->noclip) player->velocity.y -= input->vel_obj.y;
        player->on_ground = 0;
    }
    // ... and input.grapObj: the grapple this frame's Grappler shot hooked
    if (!move_lock && recon && input->has_grap_obj) player->grapple = input->grap_obj;

    // Wall jump: at most wallJumpMax per airborne period (v3.1.1: unlimited); the class / mode
    // wallJV, wallJYV and config.wallJP shape it.
    if (!move_lock && player->wall_jump && player->wall_jump_count < player->wall_jump_max && !player->on_ground && !player->on_ladder &&
        player->on_wall && movement_wall_jump_power(player) > 0.0f) {
        if (player->did_wall_jump && !input->jump) player->did_wall_jump = 0;

        if (!player->did_wall_jump && input->jump) {
            player->did_wall_jump = 1;
            player->wall_jump_count++;
            if (!recon && player->is_you) player_feel_wall_jump(player);

            const int real = movement_real(player);
            const float power = movement_wall_jump_power(player);
            const float velocity = (mode_movement->wall_jump_vel ? mode_movement->wall_jump_vel : MOVEMENT_OR(knobs->wall_jump_vel, 0.03f)) * (real ? 0.7f : 1.0f);
            player->velocity.y = (real ? 0.9f : 1.0f) * 0.058f * MOVEMENT_OR(mode_movement->wall_jump_y_vel, 1.0f);

            switch (player->on_wall) {
                case 1:
                    player->velocity.x = velocity * power;
                    break;
                case 2:
                    player->velocity.x = -velocity * power;
                    break;
                case 3:
                    player->velocity.z = velocity * power;
                    break;
                case 4:
                    player->velocity.z = -velocity * power;
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

    world_player_reset_input(player, input->reset, recon, delta * 1000.0f); // WP-B: input[13] Parkour resets
}

// playerStep: stepVal swings between +-PI/2 * stpFr (class step frequency) with the distance
// covered (3D, x1.4 on a ladder for the local player); crouched remote players step 1.6x faster.
void player_step(Player *player, float distance) {
    if (!distance) return;
    if (!player->render_you && player->crouch_val) distance *= 1.6f;

    player->step_val += distance * MOVEMENT_STEP_MLT * (player->step_dir_neg ? -1.0f : 1.0f);
    const float limit = (float) M_PI / 2.0f * MOVEMENT_OR(movement_knobs(player)->step_freq, 1.0f);

    if ((player->step_val >= limit && !player->step_dir_neg) || (player->step_val <= -limit && player->step_dir_neg)) {
        player->step_dir_neg ^= 1;
        player->step_events++; // playerStep sound (sound_events.c)
    }
}

// ---------------------------------------------------------------- combat
// Hitscan, damage, kills and score: Krunker v3.1.1 src/data/players.js (shoot,
// melee, changeHealth, kill, score) and src/data/kills.js (reward). Only the
// authority (native server, offline local server, is_local games) applies damage;
// clients predict recoil/spread and learn the outcome from HIT/HEALTH/KILL packets.
// Lag compensation (storeState / fetchState) is below. Kill streaks, effects and perks:
// shared/src/effects.c. AI
// targets: shared/src/ai.c (game_ai_damage); map object hits (destructibles, trigger
// events): shared/src/triggers.c (game_object_hit).

// ---------------------------------------------------------------- lag compensation
// v3.1.1 players.js storeState / fetchState / getStateConst (and projectile.js stateConst).
// The server keeps every player's {time, h, x, y, z} for config.stateHistory (500 ms), one
// entry per update (game_tick_world). A shot (hitscan rays, the melee swing, and every tick
// of a projectile's flight) tests a target's hitbox where it stood at
//     targetTime = game.now - (stateConst + (1000 / target.cSendRate) * config.interpolation)
// (ms): the newest entry at or before targetTime, else the oldest one, with no interpolation
// between entries. (1000 / cSendRate) * interpolation is the client's remote-player delay
// (players.js update moves x1 -> x2 over that time; client_apply_state). Only targets are
// rewound: the shooter's origin, map objects and AI boxes stay current, and only players alive
// now (active) are tested. Head and leg zones (combat_fire_ray) use the target's current y and
// height, like v3.1.1's G.obj.y, not the rewound state.
//
// stateConst, the network part of the rewind (player_get_state_const):
//  - literal (server.cfg lag_comp_anchor 0): v3.1.1 getStateConst = ping * 2 * lagComp,
//    counted back from game.now when the shot is processed. player.ping is the smoothed RTT
//    (packetHandlers.ts io_po), so this goes back two RTTs, about one more than the client saw.
//  - arrival-anchored (lag_comp_anchor 2; intentional difference from v3.1.1): the shot is
//    resolved at the time its input was fired, not at the later tick that processed it.
//    Input.arrival is when the input reached the server on the state_now clock, minus the
//    deltas of the later frames of its batch. The client fired it RTT/2 (uplink) before that,
//    and was showing targets as they were RTT/2 (downlink) plus the interpolation delay earlier.
//    So stateConst = (game.now - arrival) + ping * lagComp (lagComp 1: one RTT), and
//    fetchState still adds the interpolation delay as in v3.1.1. The time an input waited for
//    its tick (or a batch that arrived at once) is measured instead of folded into "ping * 2".
//    A shot whose packet was delayed (jitter, a lag spike) is rewound that much too little.
//  - exact (lag_comp_anchor 1, the default; intentional difference from v3.1.1): the shot's time
//    comes from the client's own input timeline, so a delayed packet costs nothing. No packet
//    change: every frame carries its delta, so client_time(frame) = the running sum of the
//    (clamped) deltas of the connection's timeline (LagCompClock below). Per packet, skew =
//    received - client_time; offset = the least skew of the last LAG_COMP_CLOCK_WINDOW s (the
//    least-delayed packets set it), so offset + client_time(shot frame) is when that frame
//    would have arrived over the fastest path. The client fired it the least uplink delay
//    before that and saw targets the downlink delay + interpolation earlier: the network part
//    is the least uplink + the typical downlink = ping - excess, where excess = the window's
//    median skew - offset (ping is a smoothed RTT, so it holds the typical uplink delay).
//    stateConst = (game.now - (offset + client_time)) + (ping - excess) * lagComp.
//    Guards: the shot time is never after the frame's arrival (causal) nor more than
//    LAG_COMP_CLOCK_MAX_EARLY before it; until a timeline has LAG_COMP_CLOCK_CALIBRATION s of
//    samples, and for the rest of a life whose client clock ran faster or slower than real time
//    (an anomaly: summed deltas growing faster than arrivals, or the offset drifting), the shot
//    falls back to the arrival-anchored estimate. A client cannot gain rewind by speeding up its
//    clock (the shot time stays <= its arrival, and the excess only lowers the ping part); one
//    holding back or slowing down frames gains at most LAG_COMP_CLOCK_MAX_EARLY, like a real lag
//    spike, inside the same 500 ms window v3.1.1 allows a client that delays its pongs.
// The 500 ms stateHistory window bounds the rewind in every mode: the oldest entry is the limit.

#define LAG_COMP_CLOCK_WINDOW 5.0          // s of packet samples behind offset / excess
#define LAG_COMP_CLOCK_CALIBRATION 0.5     // s of samples (and LAG_COMP_CLOCK_MIN_SAMPLES) before a timeline is used
#define LAG_COMP_CLOCK_MIN_SAMPLES 8
#define LAG_COMP_CLOCK_MAX_EARLY 0.25      // s a shot may be credited before its packet's arrival
#define LAG_COMP_CLOCK_LOST_DELTA 0.005    // s a clamped frame may lose before its timeline breaks
#define LAG_COMP_CLOCK_DRIFT_TOLERANCE 0.1 // s the skew may leave the calibration baseline (route changes)...
#define LAG_COMP_CLOCK_DRIFT_RATE 0.005    // ...plus 0.5 % of the timeline's age (clock rate differences)

const char *lag_comp_anchor_name(const int anchor) {
    return anchor == LAG_COMP_ANCHOR_EXACT ? "exact" : anchor == LAG_COMP_ANCHOR_ARRIVAL ? "arrival" : "v3.1.1";
}

int lag_comp_anchor_parse(const char *text) {
    if (!text) return -1;
    if (!strcmp(text, "1") || !strcmp(text, "exact") || !strcmp(text, "on") || !strcmp(text, "true")) return LAG_COMP_ANCHOR_EXACT;
    if (!strcmp(text, "2") || !strcmp(text, "arrival")) return LAG_COMP_ANCHOR_ARRIVAL;
    if (!strcmp(text, "0") || !strcmp(text, "v3") || !strcmp(text, "v3.1.1") || !strcmp(text, "literal") ||
        !strcmp(text, "off") || !strcmp(text, "false")) return LAG_COMP_ANCHOR_V3;
    return -1;
}

void lag_comp_clock_reset(LagCompClock *clock) {
    const unsigned short epoch = clock->epoch;
    memset(clock, 0, sizeof(*clock));
    clock->epoch = (unsigned short) (epoch + 1);
    clock->last_seq = -1;
}

// A frame whose time the client clock cannot account for (seq restart / gap, a frame longer than
// the delta clamp): a new timeline. Its samples start over; the anomaly flag stays for the life.
static void lag_comp_clock_break(LagCompClock *clock) {
    clock->epoch++;
    clock->count = 0;
    clock->head = 0;
    clock->calibrated = 0;
    clock->client_time = 0.0;
}

static int lag_comp_compare_double(const void *a, const void *b) {
    const double x = *(const double *) a, y = *(const double *) b;
    return x < y ? -1 : x > y;
}

void lag_comp_clock_frame(LagCompClock *clock, Input *input, const float raw_delta) {
    input->client_time = -1.0;
    input->clock_epoch = 0;
    if (!(input->received > 0.0)) return; // not from the network (bots, tests, offline)

    const int fresh = clock->last_seq < 0 || input->seq != clock->last_seq + 1 ||
                      !isfinite(raw_delta) || raw_delta > game_constants.max_delta + LAG_COMP_CLOCK_LOST_DELTA;
    if (fresh) lag_comp_clock_break(clock);
    else clock->client_time += input->delta; // the frame is generated `delta` after the previous one
    clock->last_seq = input->seq;
    input->client_time = clock->client_time;
    input->clock_epoch = clock->epoch;

    // one sample per packet: its frames share `received`, the last one (the least skew) counts
    const double skew = input->received - clock->client_time;
    if (clock->count && input->received == clock->sample_received[clock->head]) {
        if (skew < clock->sample_skew[clock->head]) clock->sample_skew[clock->head] = skew;
    } else {
        if (clock->count && input->received < clock->sample_received[clock->head]) return; // non-causal stamp
        clock->head = (clock->head + 1) % LAG_COMP_CLOCK_SAMPLES;
        clock->sample_received[clock->head] = input->received;
        clock->sample_skew[clock->head] = skew;
        if (clock->count < LAG_COMP_CLOCK_SAMPLES) clock->count++;
        if (clock->count == 1) {
            clock->begin = input->received;
            clock->baseline = skew;
        }
    }

    // offset = the least skew of the window, excess = its median skew above that
    static double window[LAG_COMP_CLOCK_SAMPLES];
    int n = 0;
    double offset = 0.0;
    for (int i = 0; i < clock->count; i++) {
        const int at = (clock->head - i + LAG_COMP_CLOCK_SAMPLES) % LAG_COMP_CLOCK_SAMPLES;
        if (clock->sample_received[at] < input->received - LAG_COMP_CLOCK_WINDOW) break;
        window[n] = clock->sample_skew[at];
        offset = n ? fmin(offset, window[n]) : window[n];
        n++;
    }
    qsort(window, (size_t) n, sizeof(window[0]), lag_comp_compare_double);
    clock->offset = offset;
    clock->excess = (n % 2 ? window[n / 2] : (window[n / 2 - 1] + window[n / 2]) * 0.5) - offset;

    const double age = input->received - clock->begin;
    if (!clock->calibrated) {
        clock->baseline = fmin(clock->baseline, skew);
        clock->calibrated = age >= LAG_COMP_CLOCK_CALIBRATION && clock->count >= LAG_COMP_CLOCK_MIN_SAMPLES;
        return;
    }
    // client_time may only grow as fast as real time: a skew below the baseline means the summed
    // deltas outran the arrivals (a sped-up clock); a window median above it (spikes do not move a
    // median), that they fell behind (a slowed-down clock, which would earn rewind)
    const double tolerance = LAG_COMP_CLOCK_DRIFT_TOLERANCE + LAG_COMP_CLOCK_DRIFT_RATE * age;
    if (!clock->anomaly && (skew < clock->baseline - tolerance || clock->offset + clock->excess > clock->baseline + tolerance)) {
        clock->anomaly = 1;
        clock->anomaly_new = 1;
    }
}

double lag_comp_clock_shot_time(const LagCompClock *clock, const Input *input, double *excess) {
    if (!clock->calibrated || clock->anomaly || input->client_time < 0.0 || input->clock_epoch != clock->epoch ||
        !(input->received > 0.0)) return 0.0;
    double sent = clock->offset + input->client_time;
    if (sent > input->received) sent = input->received; // causal: never after it arrived
    if (sent < input->received - LAG_COMP_CLOCK_MAX_EARLY) sent = input->received - LAG_COMP_CLOCK_MAX_EARLY;
    *excess = clock->excess;
    return sent;
}

static const PlayerStoredState *player_state_entry(const Player *player, const int newest_first) {
    const int index = (player->state_history_head - newest_first + PLAYER_STATE_HISTORY_MAX) % PLAYER_STATE_HISTORY_MAX;
    return &player->state_history[index];
}

// v7.1.4 adjustedHeight (players.updateHeight): the height with the head band scaled by headMlt,
// the top of the hit box (ah in stateHistory).
static float combat_adjusted_height(const Player *player) {
    // headMlt: the spawn value, or the Bighead FFA one (mode.head_mlt, WP-D); only one is ever set
    const float head_mlt = (player->head_mlt > 0.0f ? player->head_mlt : 1.0f) * mode_head_mlt(player);
    return player->height - COMBAT_HEAD_SCALE + COMBAT_HEAD_SCALE * head_mlt;
}

// players.js storeState: unshift {time: game.now, h, ah, x, y, z}, then drop every entry at least
// config.stateHistory old (they are the oldest ones: time only grows).
void player_store_state(Player *player) {
    const Game *game = player->game;

    player->state_history_head = (player->state_history_head + 1) % PLAYER_STATE_HISTORY_MAX;
    player->state_history[player->state_history_head] = (PlayerStoredState) {game->state_now, player->position, player->height,
                                                                             combat_adjusted_height(player)};
    if (player->state_history_count < PLAYER_STATE_HISTORY_MAX) player->state_history_count++;

    while (player->state_history_count > 0 &&
           game->state_now - player_state_entry(player, player->state_history_count - 1)->time >= GAME_STATE_HISTORY) {
        player->state_history_count--;
    }
}

// players.js getStateConst: ping * 2 * lagComp (a lagComp <= 0 counts as 1); anchored: the
// time since the running input arrived plus one ping (see above). lagComp is the player's
// Settings > Network slider (ENT lag_comp, ent[11]; the server clamps it to 0.1..1 like
// gameserver.ts applyLagCompFromEnt, so the <= 0 rule only catches a never-set value) times
// the server's lag_comp_scale (KrunkNative; 1 = v3.1.1).
float player_get_state_const(const Player *player) {
    const Game *game = player->game;
    const float scale = game->lag_comp_scale > 0.0f ? game->lag_comp_scale : 1.0f;
    const float lag_comp = (player->lag_comp > 0.0f ? player->lag_comp : 1.0f) * scale; // parity: lc <= 0 -> 1
    const float ping = player->ping > 0.0f ? player->ping : 0.0f;
    if (game->lag_comp_anchor == LAG_COMP_ANCHOR_V3) return ping * 2.0f * lag_comp;

    if (game->lag_comp_anchor == LAG_COMP_ANCHOR_EXACT && player->input_arrival > 0.0) {
        double excess = 0.0;
        const double sent = lag_comp_clock_shot_time(&player->lag_clock, &player->input_running, &excess);
        if (sent > 0.0) {
            const double age = game->state_now > sent ? game->state_now - sent : 0.0;
            const float network = fmaxf(0.0f, ping - (float) (excess * 1000.0));
            return (float) (age * 1000.0) + network * lag_comp;
        }
    }

    const double age = player->input_arrival > 0.0 && game->state_now > player->input_arrival ? game->state_now - player->input_arrival : 0.0;
    return (float) (age * 1000.0) + ping * lag_comp;
}

// players.js fetchState(player, ping). 0: nothing to hit (v3.1.1 `if (G = fetchState(...))`
// with an empty stateHistory, e.g. a player who spawned after this tick's storeState).
int players_fetch_state(const Player *target, const float ping_ms, PlayerStoredState *out) {
    const Game *game = target->game;
    if (!game->lag_comp) { // offline / lag_comp 0: where the target is now
        *out = (PlayerStoredState) {game->state_now, target->position, target->height, combat_adjusted_height(target)};
        return 1;
    }
    if (!target->state_history_count) return 0;

    const float send_rate = target->c_send_rate > 0.0f ? target->c_send_rate : GAME_LAG_COMP_SEND_RATE;
    const double target_time = game->state_now - (ping_ms + 1000.0f / send_rate * game_constants.interpolation) / 1000.0;

    for (int i = 0; i < target->state_history_count; i++) {
        const PlayerStoredState *state = player_state_entry(target, i);
        if (state->time <= target_time) {
            *out = *state;
            return 1;
        }
    }

    *out = *player_state_entry(target, target->state_history_count - 1);
    return 1;
}

typedef struct {
    const Object *object;
    Player *player;
    float t;
    GameAI *ai;         // players.js shoot / melee: game.AI.ais boxes (authority)
    PlayerStoredState state; // a player hit: the rewound box it was traced against (head / leg zones)
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
        object->pickup || object->flag || object->premium || object->verified ||
        object->ladder || object->ramp) return 0; // TRIGGERs: only onEnter ones are noShoot

    switch (object->prefab) {
        case PREFAB_TEAM_ZONE:
        case PREFAB_SPAWN_POINT:
        case PREFAB_CAMERA_POSITION:
        case PREFAB_SPECTATE_CAM:
        // v7.1.4 MapManager keeps these out of manager.objects (addObjective -> objectives,
        // addDominationZone -> domzones, addBombSite -> sites, addEnvironmentZone -> envZones),
        // so shots, sprays, builds and the Grappler pass through them. KN bug fixed: a bomb
        // site zone (Kanji's A / B) stopped hitscan rays and builds like a wall.
        case PREFAB_OBJECTIVE:
        case PREFAB_BOMB_SITE:
        case PREFAB_DOMINATION_ZONE:
        case PREFAB_ENVIRONMENT_ZONE:
            return 0;
        default:
            return 1;
    }
}

// players.spray traces the same map.manager.objects as a shot (shared/src/interact.c).
int game_object_shootable(const Object *object) {
    return combat_object_shootable(object);
}

static int combat_can_damage(const Player *dealer, const Player *victim) {
    if (!world_pvp_allows(dealer, victim)) return 0; // WP-B: v7 noPVP / forcePVP pairs (forcePVP: no team)
    return !dealer->team || world_pvp_ignores_team(dealer, victim) || dealer->team != victim->team || (dealer->game->mode && dealer->game->mode->config.dmg_team) ||
           dealer->game->config.friendly_fire; // v7 game.config.frFire
}

int player_can_damage(const Player *dealer, const Player *victim) { return combat_can_damage(dealer, victim); }

// lineInRect against every shootable object and (authority only) every other living player's
// hitbox: x/z +-(scale + hitboxPadW), y from the feet to adjustedHeight + hitboxPadH (v7.1.4:
// game hitBoxPad 0.6, 0.1 in Ranked, + class hbS / hbH; v3.1.1 padded by mode.hitBoxPad || 1).
static void combat_trace(Player *shooter, const vec3 origin, const vec3 dir, const int include_players, HitList *hits) {
    Game *game = shooter->game;

    for (size_t i = 0; i < game->map->object_count; i++) {
        const Object *object = game->map->objects[i];
        if (!combat_object_shootable(object)) continue;

        const float t = line_in_rect(origin, dir, object->position, object_box_size(object));
        if (t <= 0.0f || t > 1.0f) continue;

        hit_list_push(hits, (Hit) {object, NULL, t});
    }

    if (!include_players) return;

    const float state_const = player_get_state_const(shooter);

    for (size_t i = 0; i < game->player_count; i++) {
        Player *other = game->players[i];
        if (!other || !other->active || other == shooter || !combat_can_damage(shooter, other)) continue;

        // G = fetchState(list[i], player.getStateConst()): the box where the shooter saw it
        PlayerStoredState state;
        if (!players_fetch_state(other, state_const, &state)) continue;

        const float half = other->scale + player_hitbox_pad_w(other);
        const vec3 box_scale = {half * 2.0f, state.adjusted_height + player_hitbox_pad_h(other), half * 2.0f};
        const float t = line_in_rect(origin, dir, state.position, box_scale);
        if (t <= 0.0f || t > 1.0f) continue;

        hit_list_push(hits, (Hit) {NULL, other, t, NULL, state});
    }

    // game.AI.ais: x/z +-scale, y .. y + 2 * height (no hitBoxPad); v7: not the shooter's team AIs
    for (int i = 0; game->ai && i < game->ai->count; i++) {
        GameAI *ai = &game->ai->ais[i];
        if (!ai->active || (shooter->team && shooter->team == ai->team && !(game->mode && game->mode->config.dmg_team) && !game->config.friendly_fire)) continue;
        const float t = game_ai_line_hit(ai, origin, dir);
        if (t <= 0.0f || t > 1.0f) continue;
        hit_list_push(hits, (Hit) {NULL, NULL, t, ai});
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
    const size_t count = game->weapon_count ? game->weapon_count : rules_weapon_count();
    const Weapon *const *weapons = game->weapons ? (const Weapon *const *) game->weapons : (const Weapon *const *) rules_weapons();
    return index >= 0 && index < (int) count ? weapons[index] : NULL;
}

// weapons.js `kill` (["", N]: replaces the base 50, v7 adds the Rocket's 75) and `scope`
// (No / Quick Scope: Sniper Rifle and v7's Charge Rifle).
static int combat_weapon_kill_score(const Weapon *weapon) {
    return weapon ? weapon->kill_score : 0;
}

static int combat_weapon_scope(const Weapon *weapon) {
    return weapon && weapon->scope;
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
        if (info && info->headshot) score += combat_medal(medals, "Headshot", 25); // v7.1.4 kills.js
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

    amount = mode_score_scale(game, player, amount, is_kill); // v7 killMlt / oneflag carrier multipliers (WP-D)
    if (amount > 0 && zombies_double_points(game)) amount *= 2; // WP-H: doublePointsTimer
    const int gain = mode_score_gain(game, amount, is_kill); // noScoreK: kill rewards add nothing
    player->score += gain;
    if (player->active) player->life_score += gain; // WP-E: scoreL (the death screen's "Score")
    if (gain) game_challenge(game, player, "score", gain, -1, -1); // WP-E part 2c: challenges.update(game, "score")
    const int cap = mode_score_cap(game); // v7 noScrLimit: no cap (WP-D)
    if (cap && player->score > cap) {
        player->score = cap;
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
    if (dealer && damage > 0.0f && dealer->dmg_mlt > 0.0f) damage *= dealer->dmg_mlt; // v7.1.4 bots: player.dmgMlt
    if (dealer && dealer->smite && damage > 0.0f) damage = 10000.0f; // /smite: weapon/melee/explosion dmg = 10000

    if (dealer && dealer->team > 0 && dealer->team < GAME_CONFIG_TEAMS) damage *= game->config.team_damage[dealer->team]; // t<team>Dmg (sdm teams 3-5 too)
    damage = floorf(damage * 10.0f + 0.5f) / 10.0f; // v7.1.4 changeHealth: dmg.round(1)

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
    // KrunkScript onPlayerDamage: damage > 0 that leaves the victim alive (PLS:4295-4299)
    if (damage > 0.0f && victim->health > 0.0f) {
        trigger_player_event(game, TRIGGER_EVENT_ON_PLAYER_DAMAGE, victim); // playerTrigger("onPlayerDamage", victim) (WP-C)
        ks_game_on_player_damage(game, victim, dealer, damage);
    }

    return victim->health <= 0.0f;
}

// WP-E part 2c: the stats and challenge events of one kill (v7 players.kill, kills.js reward and
// the melee / hitscan / projectile kill sites): "k" and "kstk" with the killing weapon (none for
// a weaponless nuke kill: v7's stk kills), mk / fk / tmk, hs / ls / wb, then the kill's medals
// (incMedal) and their challenge keys (kills.js vA742). v7 counts hs / ls / wb on hitscan and
// projectile kills only; the kill info carries them for those (a melee headshot medal adds no
// stat there either).
static void combat_kill_progress(Game *game, Player *killer, const PlayerKillInfo *info, const int weapon_index,
                                 const NetKillMedalsPacket *medals) {
    const int wep = info && info->weapon_id >= 0 ? weapon_index : -1;
    game_challenge(game, killer, "k", 1, wep, -1);
    // players.kill: a weapon kill adds the weapon to uniqueWeaponKills -> challenges.update("uwk", length)
    // bit = the weapon id; KrunkNative's (WEAPON_ID_KN_BASE..) fold onto the top bits, above every
    // Krunker table (v10: 31 weapons)
    const int bit = wep >= WEAPON_ID_KN_BASE ? 63 - (wep - WEAPON_ID_KN_BASE) : wep;
    if (wep >= 0 && bit >= 0 && bit < 64 && !(killer->unique_weapon_kills & (1ull << bit))) {
        killer->unique_weapon_kills |= 1ull << bit;
        int unique = 0;
        for (uint64_t m = killer->unique_weapon_kills; m; m &= m - 1) unique++;
        game_challenge(game, killer, "uwk", unique, -1, -1);
    }
    game_challenge(game, killer, "kstk", (int) killer->kill_streak, -1, -1); // nonWep: the kill streak
    if (wep >= 0) game_challenge(game, killer, "kstk", (int) killer->weapon_streak, wep, -1);
    const Weapon *weapon = combat_weapon_at(game, weapon_index);
    const int melee = weapon && weapon->melee;
    if (melee) {
        const char *key = info && info->thrown ? "tmk" : info && info->fists ? "fk" : "mk";
        game_inc_stat(game, killer, key);
        game_challenge(game, killer, key, 1, wep, -1);
    }
    if (info && !melee) {
        static const char *const keys[3] = {"hs", "wb", "ls"};
        const int on[3] = {info->headshot, info->wallbang, info->legshot};
        for (int i = 0; i < 3; i++) {
            if (!on[i]) continue;
            game_inc_stat(game, killer, keys[i]);
            game_challenge(game, killer, keys[i], 1, wep, -1);
        }
    }
    static const char *const medal_keys[][2] = {{"First Blood", "fb"}, {"Revenge", "rk"}, {"Mid Air", "airk"}, {"Turbo Kill", "tk"},
                                                {"No Scope", "ns"},    {"Quick Scope", "qs"}, {"Longshot", "lns"}, {"Close Call", "cc"},
                                                {"Buzzkill", "bzk"},   {"Driftkill", "drk"}};
    for (int m = 0; medals && m < medals->count; m++) {
        const char *name = medals->medals[m].text;
        game_inc_medal(game, killer, name);
        for (size_t k = 0; k < sizeof(medal_keys) / sizeof(*medal_keys); k++)
            if (!strcmp(medal_keys[k][0], name)) game_challenge(game, killer, medal_keys[k][1], 1, wep, -1);
    }
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
    game_challenge(game, player, "d", 1, -1, -1); // WP-E part 2c: challenges.update(game, "d", 1)

    const int weapon_index = killer ? (kill_info && kill_info->weapon_id >= 0 ? kill_info->weapon_id : combat_weapon_index(killer)) : -1;
    int reward = 0;
    int streak_message = 0; // the "kst" streak (killStreak % 5 == 0), 0 = none

    if (killer && killer != player) {
        player->last_killer_uid = killer->uid;
        killer->kills++;
        if (killer->active) killer->life_kills++; // WP-E: killsL

        if (streaks) {
            killer->streak++;
            killer->kill_streak++;
            killer->real_kill_streak++; // v7.1.4 realKillStreak -> updateStreaks (streak loadout)
            killer->death_streak = 0;
            player_update_streaks(killer);
            // v7.1.4 weaponStreak / "wstk": kills in a row with one weapon (not the Nuke's weaponless kills)
            if (kill_info && kill_info->weapon_id >= 0) {
                killer->weapon_streak = killer->streak_weapon == weapon_index ? killer->weapon_streak + 1 : 1;
                killer->streak_weapon = weapon_index;
                const NetWeaponStreakPacket wstk = {.weapon = weapon_index, .count = killer->weapon_streak};
                game_send(game, killer->uid, NET_PACKET_WEAPON_STREAK, &wstk, sizeof(wstk));
            }
            if (killer->kill_streak % 5 == 0) streak_message = (int) killer->kill_streak; // X = killStreak
            if (game->now - killer->last_kill_time >= GAME_FEED_TIMER) killer->streak = 0;
        }

        killer->last_kill_time = game->now;

        NetKillMedalsPacket medals;
        memset(&medals, 0, sizeof(medals));
        if (!skip_rewards && game->config.kill_rewards) {
            reward = combat_kill_reward(game, killer, player, kill_info, combat_weapon_at(game, weapon_index), &medals);
            medals.kills = (int32_t) killer->kills;
            if (medals.count) game_send(game, killer->uid, NET_PACKET_KILL_MEDALS, &medals, sizeof(medals)); // IO.send(Q.id, "6", W, ..)
        }
        combat_kill_progress(game, killer, kill_info, weapon_index, &medals); // WP-E part 2c
        if (!skip_rewards && game->config.kill_rewards) player_score(killer, reward, 1);
        // players.kill: incStat("kjg") for killing a Juggernaut, incStat("jgk") for a kill as one
        if (player->is_juggernaut) game_inc_stat(game, killer, "kjg");
        if (killer->is_juggernaut) game_inc_stat(game, killer, "jgk");
    }

    // Assists: config.assistMin damage within config.assistTime.
    for (size_t i = 0; i < game->player_count; i++) {
        Player *other = game->players[i];
        if (!other || other == killer || other == player || (game->mode && game->mode->config.no_assists) || game->config.no_assists) continue;

        for (int k = 0; k < PLAYER_DAMAGE_SLOTS; k++) {
            const PlayerDamage *damage = &player->damage_received[k];
            if (damage->uid != other->uid || damage->value < GAME_ASSIST_MIN || game->now - damage->time > GAME_ASSIST_TIME) continue;

            other->assists++;
            game_inc_stat(game, other, "ast"); // WP-E part 2c: giveAssist's incStat / challenge / incMedal
            game_challenge(game, other, "ast", 1, -1, -1);
            game_inc_medal(game, other, "Assist");
            player_score(other, GAME_ASSIST_SCORE, 1);
            game_send(game, other->uid, NET_PACKET_ASSIST, NULL, 0); // IO.send(id, "10")
            break;
        }
    }

    for (int k = 0; k < PLAYER_DAMAGE_SLOTS; k++) player->damage_received[k] = (PlayerDamage) {-1, 0.0f, 0.0f};
    player->kill_streak = 0;

    // WP-E: the victim's "3" extras (players.js:4860) right before its KILL: the killing weapon,
    // the killer's class stat and player card stat, and this life's stats
    NetDeathInfoPacket death = {
        .victim_uid = player->uid,
        .killer_uid = killer ? killer->uid : -1,
        .weapon_index = weapon_index,
        .class_stat = killer && killer != player ? killer->class_stat : 0,
        .pc_stat_index = (int8_t) (killer && killer != player && killer->pc_stat_index >= 0 ? killer->pc_stat_index : -1),
        .thrown = (uint8_t) (kill_info && kill_info->thrown),
        .fists = (uint8_t) (kill_info && kill_info->fists),
        .time_alive_ms = (int32_t) fmaxf(0.0f, (game->now - player->life_start) * 1000.0f),
        .shots = player->life_shots,
        .kills = player->life_kills,
        .score = player->life_score,
    };
    if (death.pc_stat_index >= 0) snprintf(death.pc_stat_value, sizeof(death.pc_stat_value), "%s", killer->pc_stat_value);
    player_card_accuracy(player->life_shots, player->life_hits, death.accuracy, sizeof(death.accuracy));
    game_send(game, player->uid, NET_PACKET_DEATH_INFO, &death, sizeof(death));

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

    mode_on_kill(game, player, killer, kill_info && kill_info->weapon_id < 0 ? -1 : weapon_index); // lives, mode.onKill; -1: a weaponless (streak) kill (WP-D: gun.js info.stk)
    // v7.1.4 Siphon: the killer heals attributes.siphon
    if (killer && killer != player && killer->active && killer->attributes.siphon > 0.0f) player_change_health(killer, NULL, -killer->attributes.siphon, 0);
    if (streak_message && killer) { // IO.broadcast("kst", Q.sid, X): killStreakMessage on every client
        const NetKillStreakPacket kst = {.uid = killer->uid, .streak = streak_message};
        game_send(game, NET_TARGET_ALL, NET_PACKET_KILL_STREAK, &kst, sizeof(kst));
    }
    // WP-C: dropWeapon(victim, true), then playerTrigger("onPlayerDie", victim) / ("onPlayerKill", killer)
    game_player_drop_all(game, player);
    trigger_player_event(game, TRIGGER_EVENT_ON_PLAYER_DIE, player);
    if (killer && killer != player) trigger_player_event(game, TRIGGER_EVENT_ON_PLAYER_KILL, killer);
    ks_game_on_player_death(game, player, killer); // KrunkScript onPlayerDeath(id, killerID) (PLS:4889-4891)
}

void player_melee(Player *player) {
    // v7.1.4 melee: `!player.disableMelee && config.canMelee` (KrunkScript; 1 without a script)
    if (!ks_player_allows(player, KS_PLAYER_CAN_MELEE)) return;
    // 95.js: weapon.canThrow && jKFi4 == 0 && canThrow (fully aimed); the readable jail src/data/players.js has aimVal <= 0.1.
    const int is_throw = player->can_throw && player->weapon->can_throw && player->aim_val == 1.0f;
    const ClassConfig *class_config = player->game->classes && player->class_index >= 0 && (size_t) player->class_index < player->game->class_count
        ? &player->game->classes[player->class_index] : NULL;

    // rate * (fiRat || 1) * attributes.fRate * (perk 1 ? 0.66 : 1) * (kranked bonuses.firerate) * class melRate
    player->reloads[player->loadout_index] = player->weapon->rate * (player->game->config.fire_rate > 0.0f ? player->game->config.fire_rate : 1.0f) *
                                             player_attr_fire_rate(player) * mode_fire_rate_mlt(player->game, player) *
                                             (class_config && class_config->melee_rate > 0.0f ? class_config->melee_rate : 1.0f);
    player->did_shoot = 1;
    player->did_act = 1;

    // v7.1.4 knife-throw charge: d = clamp(chargeTime / throwCharge, 0.1, 1) (chrgWeps), then reset
    float throw_d = 1.0f;
    if (is_throw && !player->game->config.no_charge_weapons) {
        const float full = player->throw_charge > 0.0f ? player->throw_charge : COMBAT_THROW_CHARGE;
        throw_d = CLAMP(player->charge_time / full, 0.1f, 1.0f);
    }
    player->charge_time = 0.0f;
    player->full_charge = 0;

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
        // stored[0] with {d, v: max(0.3, d)}: ceil(180 * d) damage, 0.38 * v speed
        const ProjectileShot shot = {.speed_mlt = fmaxf(0.3f, throw_d), .damage_d = throw_d, .effect = -1};
        player->throw_speed_mlt = shot.speed_mlt; // physObj(..., {d, v}): the thrower's own knife (projectiles_render.c)
        game_spawn_projectile_ex(player->game, player, WEAPON_ID_KNIFE, projectile_config_for(player->weapon, 1), origin, player->direction.y,
                                 player->direction.x, 1, &shot);
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

    // Every target in range soaks the full swing unless the weapon pierces (x attributes.dmg, v7.1.4).
    float damage = (player->can_throw ? player->weapon->damage : COMBAT_FIST_DAMAGE) * player_attr_damage(player);

    for (size_t i = 0; i < hits.count && damage > 0.0f; i++) {
        Player *victim = hits.items[i].player;
        if (hits.items[i].ai && !mode_waiting(game)) game_ai_damage(game, hits.items[i].ai, player, player->smite ? 10000.0f : damage, 0); // G.ai: !waitTimers
        // G.obj: destructible health and onMelee / onDamage / onPunch triggers
        if (hits.items[i].object) game_object_hit(game, (Object *) hits.items[i].object, player, player->smite ? 10000.0f : damage, 0, GAME_HIT_MELEE);
        if (hits.items[i].object) zombies_melee_object(game, player, hits.items[i].object); // WP-H: share a ready box

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
        .first = (uint8_t) (ray == 0 ? (player->shot_blast ? 2 : 1) : 0), // 2: playSound(weapon.sound + "_blast")
    };

    if (last && last->object) {
        const Object *object = last->object;
        const float x = packet.hit[0], y = packet.hit[1], z = packet.hit[2];
        const vec3 size = object_box_size(object);

        packet.has_normal = 1;
        if (y >= object->position.y + size.y) packet.normal_pitch = (float) M_PI / 2.0f;
        else if (y <= object->position.y) packet.normal_pitch = -(float) M_PI / 2.0f;
        else if (x <= object->position.x - size.x * 0.5f) packet.normal_yaw = -(float) M_PI / 2.0f;
        else if (x >= object->position.x + size.x * 0.5f) packet.normal_yaw = (float) M_PI / 2.0f;
        else if (z <= object->position.z - size.z * 0.5f) packet.normal_yaw = (float) M_PI;
    }

    game_send(game, NET_TARGET_EXCEPT(player->uid), NET_PACKET_SHOT, &packet, sizeof(packet)); // players + spectators
}

// One ray of player_shoot(): physPow push (i < 0) or bullet damage with falloff,
// headshot/leg multipliers and wallbang through penetrable objects.
static void combat_fire_ray(Player *player, const int ray, const float pitch, const float yaw, const float shot_height, const float charge) {
    Game *game = player->game;
    const Weapon *weapon = player->weapon;
    const int authority = game_is_authority(game);
    const float range = ray < 0 ? weapon->physical_range : weapon->range;
    if (range <= 0.0f || (!authority && ray >= 0)) return;
    if (authority && ray >= 0 && player->active) player->life_shots++; // WP-E: totShotsL (the death screen's "Shots Fired")
    if (authority && ray >= 0) game_inc_stat(game, player, "s");      // WP-E part 2c: incStat("s")

    const vec3 origin = {player->position.x, shot_height, player->position.z};
    const vec3 dir = combat_shot_vector(range, pitch, yaw);
    HitList hits = {0};

    // v7.1.4 knockback: config impulseMlt x mode impulseMlt; noImpulseDist drops the distance term
    const GameModeConfig *mode_config = game->mode ? &game->mode->config : NULL;
    const float impulse_mlt = game->config.impulse_mlt * (mode_config && mode_config->has_impulse_mlt ? mode_config->impulse_mlt : 1.0f);
    const int no_impulse_dist = mode_config && mode_config->no_impulse_dist;

    // nonObjImpulse modes push the physPow ray a fixed 0.04 (x1 with noImpulseDist), hit or not
    if (ray < 0 && mode_config && mode_config->non_obj_impulse) {
        const float impulse = (no_impulse_dist ? 1.0f : 0.04f) * impulse_mlt;
        const vec3 push = {impulse * sinf(yaw + (float) M_PI) * cosf(pitch), impulse * sinf(pitch),
                           impulse * cosf(yaw + (float) M_PI) * cosf(pitch)};
        player->velocity.x -= push.x;
        player->velocity.z -= push.z;
        player->velocity.y -= push.y;
        player->on_ground = 0;
        if (!authority) {
            player->shot_vel_obj = push;
            player->has_shot_vel_obj = 1;
        }
    }

    combat_trace(player, origin, dir, authority && ray >= 0, &hits);
    hit_list_sort(&hits);
    // map-logic WP-B: shoot raycasts the terrain too (players.js:2982-2990; pushed first, so it wins
    // ties): nothing behind it is reached and the tracer ends on it
    const float terrain_t = world_terrain_fraction(game, origin, dir);
    if (terrain_t > 0.0f) {
        size_t kept = 0;
        for (size_t i = 0; i < hits.count; i++)
            if (hits.items[i].t < terrain_t) hits.items[kept++] = hits.items[i];
        hits.count = kept;
        hit_list_push(&hits, (Hit) {NULL, NULL, terrain_t}); // the terrain: no object, player or AI
    }
    if (authority && ray >= 0) { // map-logic WP-H: canShoot spawnables (Skull EE) before the first wall
        float stop_t = 2.0f;
        for (size_t i = 0; i < hits.count && stop_t > 1.0f; i++)
            if (!hits.items[i].player && !hits.items[i].ai) stop_t = hits.items[i].t;
        zombies_shoot_spawnables(game, player, origin, dir, stop_t);
    }

    // v7.1.4: dmg x charge (vLN1167: chrgTime charge, fullChrgMlt on a full-charge shot) x
    // attributes.dmg; pierce subtracts from the charged base without the attribute.
    const float base = weapon->damage * charge;
    float damage = base * player_attr_damage(player);
    int wallbang = 0;
    const Hit *last = NULL; // Krunker G: the last target the bullet reached (tracer end)

    for (size_t i = 0; i < hits.count; i++) {
        const Hit *hit = &hits.items[i];
        last = hit;

        if (ray < 0) {
            // v7.1.4: no object push in nonObjImpulse modes, nor off noPCol / noImpulse ("ip") objects
            const Object *object = hit->object;
            if ((mode_config && mode_config->non_obj_impulse) || (object && (object->no_player_col || object->no_impulse)) || hit->t < 0.0f) break;
            const float impulse = (no_impulse_dist ? 1.0f : 1.0f - hit->t) * weapon->physical_power * impulse_mlt;
            const vec3 push = {impulse * sinf(yaw + (float) M_PI) * cosf(pitch), impulse * sinf(pitch),
                               impulse * cosf(yaw + (float) M_PI) * cosf(pitch)};
            player->velocity.x -= push.x;
            player->velocity.z -= push.z;
            player->velocity.y -= push.y;
            player->on_ground = 0;
            // if (!IO) a1.velObj = {x, y, z}: the client keeps it for reconciliation replays
            if (!authority) {
                player->shot_vel_obj = push;
                player->has_shot_vel_obj = 1;
            }
            break;
        }

        // WP-E: hitShotsL counts a ray whose first hit is a player or an AI (players.js vLN02370 == 0)
        if (i == 0 && (hit->player || hit->ai) && player->active) player->life_hits++;
        if (i == 0 && (hit->player || hit->ai)) game_inc_stat(game, player, "h"); // WP-E part 2c: incStat("h")
        const float drop = fminf(1.0f, 1.0f - (1.0f - hit->t) * range / (range - weapon->drop_start));
        // v7.1.4: falloff clamps at 0 and a 0 bullet stops (v3.1.1 could go negative)
        float dealt = fmaxf(0.0f, damage - weapon->damage_drop * drop);
        if (dealt == 0.0f) break;

        if (hit->player) {
            Player *victim = hit->player;
            const float hit_y = shot_height + range * hit->t * sinf(pitch);
            // v7.1.4 zones: head within headScale * headMlt of adjustedHeight, legs below legHeight
            // minus the crouch drop, x weapon hsMlt (unless noHeadShot) / lsMlt (not on heads).
            // KN bug fixed: noHeadShot was ignored (Auto Pistol, 5.2). v7 bug fixed: v7 measures the
            // zones from the target's live position, not the rewound box the ray hit.
            const int head = player_hit_is_head(victim, hit->state.position.y, hit->state.adjusted_height, hit_y);
            const int leg = player_hit_is_leg(victim, hit->state.position.y, hit_y);
            dealt = player_hit_damage(game, weapon, dealt, head, leg);

            // effectOnHit: floor(charge * 10) stacks (no stock hitscan weapon has one)
            const int effect_count = (int) floorf(charge * 10.0f);
            if (weapon->effect_on_hit > 0 && effect_count > 0) {
                player_give_effect(victim, weapon->effect_on_hit - 1, effect_count);
                if (weapon->effect_on_hit - 1 == EFFECT_ZAPPED) victim->zapped_by = player->uid;
            }

            if (player_change_health(victim, player, dealt, head)) {
                PlayerKillInfo info = {
                    .weapon_id = combat_weapon_index(player),
                    .headshot = (unsigned char) head,
                    .wallbang = (unsigned char) wallbang,
                    .legshot = (unsigned char) (leg && !head), // WP-E part 2c: the "ls" stat
                    .distance = hit->t,
                };
                player_kill(victim, player, &info, 0);
            }

            damage -= weapon->pierce ? base * COMBAT_DENSITY_FLESH * weapon->pierce : damage;
        } else if (hit->ai) {
            // G.ai: changeHealth(-dmg, player), killScore, "4"; the bullet goes on like a wall it pierces
            // v7 players.js: the crit band (crd / crh) and the weapon's effectOnHit (floor(charge * 10) stacks)
            const float hit_y = shot_height + range * hit->t * sinf(pitch);
            const int ai_effect_count = (int) floorf(charge * 10.0f);
            game_ai_damage_ex(game, hit->ai, player, player->smite ? 10000.0f : dealt, 0, hit_y,
                              weapon->effect_on_hit > 0 && ai_effect_count > 0 ? weapon->effect_on_hit - 1 : -1, ai_effect_count);
            damage -= weapon->pierce ? base * COMBAT_DENSITY_DEFAULT * weapon->pierce : damage;
        } else {
            if (!hit->object) break; // WP-B: the terrain stops the bullet
            // G.obj: destructible health ("4", "do", onDestroy) and onShoot / onDamage triggers
            game_object_hit(game, (Object *) hit->object, player, dealt, 0, GAME_HIT_SHOT);
            if (!hit->object->penetrable) break;
            wallbang = 1;
            damage -= weapon->pierce ? base * COMBAT_DENSITY_DEFAULT * weapon->pierce : damage;
        }

        if (damage <= 0.0f) break;
    }

    if (ray >= 0) combat_send_shot(player, ray, pitch, yaw, shot_height, range, last);

    free(hits.items);
}

// v7.1.4 removeOnEmpty: the emptied streak equipment leaves the loadout (the Juggernaut's Minigun
// ends the Juggernaut). v7 bug fixed: v7 only does this in the shooter's own client (renderYou,
// with serverIO only in the sandbox), so on a dedicated server the empty weapon stayed.
static void combat_remove_if_empty(Player *player, const int weapon_id) {
    if (!game_is_authority(player->game) || !player->weapon->remove_on_empty || player->ammo[player->loadout_index] > 0) return;
    if (weapon_id == WEAPON_ID_MINIGUN && player->is_juggernaut) player_set_juggernaut(player, 0);
    else player_remove_weapon(player, weapon_id);
}

// v7.1.4 players.tryDeployable (authority): the first map object along the look ray within
// deployRang gets the weapon's spawnable (medals.js index `deployable`, the Slime Pad) at the hit
// point; the emptied Slimer leaves the loadout. The pad's face orientation (xD / zD) is not
// carried by spawnable_drop yet.
static void combat_try_deploy(Player *player, const int weapon_id) {
    Game *game = player->game;
    if (!game_is_authority(game)) return;
    const Weapon *weapon = player->weapon;
    const float eye = player->position.y + player->height - game_constants.camera_height;
    const vec3 origin = {player->position.x, eye, player->position.z};
    const vec3 dir = combat_shot_vector(weapon->deploy_range, player->direction.x, player->direction.y);
    HitList hits = {0};
    combat_trace(player, origin, dir, 0, &hits);
    hit_list_sort(&hits);
    if (hits.count && weapon->deployable > 0) {
        const float t = hits.items[0].t;
        const vec3 at = {roundf((origin.x + dir.x * t) * 10.0f) / 10.0f, roundf((origin.y + dir.y * t) * 10.0f) / 10.0f,
                         roundf((origin.z + dir.z * t) * 10.0f) / 10.0f};
        spawnable_drop(game, weapon->deployable - 1, player, 0, at);
    }
    free(hits.items);
    if (player->ammo[player->loadout_index] <= 0) player_remove_weapon(player, weapon_id);
}

// ---------------------------------------------------------------- Grappler (v7.1.4)
// players.js tryGrapple / endGrapple, player.js procInputs (the rope, the release) and the recon
// branch (input.grapObj). Both sides run it: the authority's grapple rides in RECON (V3) for the
// owner and NET_PACKET_GRAPPLE for everyone's rope; the owner predicts its own (client).

static double grapple_js_round1(const double v) { return floor(v * 10.0 + 0.5) / 10.0; } // Number.round(1)

static double grapple_d3d(const vec3 a, const vec3 b) { // UTILS.getD3D
    const double dx = (double) a.x - b.x, dy = (double) a.y - b.y, dz = (double) a.z - b.z;
    return sqrt(dx * dx + dy * dy + dz * dz);
}

void player_end_grapple(Player *player) { // manager.endGrapple: grapple = null
    if (player) memset(&player->grapple, 0, sizeof(player->grapple));
}

// The grapple slot of v7's "k" sync (game.js: grapple ? [x, y, z] : 0 per other player):
// NET_TARGET_ALL sends every hook that appeared, moved or let go since the others last heard
// (a dead player has none); a connection uid gets every current hook (a joiner).
void game_sync_grapples(Game *game, const int uid) {
    if (!game || !game_is_authority(game)) return;
    for (size_t i = 0; i < game->player_count; i++) {
        Player *p = game->players[i];
        if (!p) continue;
        const int active = p->active && p->grapple.active;
        const vec3 point = active ? p->grapple.point : (vec3) {0.0f, 0.0f, 0.0f};
        const int changed = active != p->grapple_sent.active || point.x != p->grapple_sent.point.x || point.y != p->grapple_sent.point.y ||
                            point.z != p->grapple_sent.point.z;
        if (uid == NET_TARGET_ALL ? !changed : !active || p->uid == uid) continue;
        const NetGrapplePacket packet = {.uid = p->uid, .active = (uint8_t) active, .point = {point.x, point.y, point.z}};
        game_send(game, uid == NET_TARGET_ALL ? NET_TARGET_EXCEPT(p->uid) : uid, NET_PACKET_GRAPPLE, &packet, sizeof(packet));
        if (uid == NET_TARGET_ALL) {
            p->grapple_sent.active = (unsigned char) active;
            p->grapple_sent.point = point;
        }
    }
}

// tryGrapple(player, grapRang, fire, input): runs whenever the timers allow, acts on `fire`. The
// first active, shootable map object without noGrapple ("gp") along the look ray within grapRang
// takes the hook (the point rounded to 0.1); the rope starts at the hit distance plus the eye
// height (height - cameraHeight) + 0.001. A miss still spends the shot (rate) and the hook.
static void combat_try_grapple(Player *player, const int fire) {
    if (!fire || !player->active || !player->weapon) return;
    Game *game = player->game;
    const Weapon *weapon = player->weapon;
    player->did_shoot = 1;
    player->charge_time = 0.0f;
    player->full_charge = 0;
    player->did_act = 1;
    player_end_grapple(player);
    player->reloads[player->loadout_index] = weapon->rate;
    // game.playSound(weapon.sound): client/src/sound_events.c hears the fire timer jump
    player->recoil_force += weapon->recoil;
#ifdef KRUNKNATIVE_CLIENT
    if (!game->server && player->render_you) { // recoilTween (weapon.recoilAnim) + cancelInspect
        player_feel_recoil(player);
        player_cancel_inspect(player);
    }
#endif
    const double range = weapon->grapple_range;
    const double yaw = player->direction.y, pitch = player->direction.x;
    const double eye = (double) player->position.y + player->height - game_constants.camera_height;
    const vec3 origin = {player->position.x, (float) eye, player->position.z};
    const vec3 dir = combat_shot_vector((float) range, player->direction.x, player->direction.y);
    float best = 2.0f;
    for (size_t i = 0; game->map && i < game->map->object_count; i++) {
        const Object *object = game->map->objects[i];
        if (!combat_object_shootable(object) || object->no_grapple) continue;
        const float t = line_in_rect(origin, dir, object->position, object_box_size(object));
        if (t > 0.0f && t <= 1.0f && t < best) best = t;
    }
    if (best > 1.0f) return;
    const double dist = range * best;
    PlayerGrapple *g = &player->grapple;
    g->active = 1;
    g->point.x = (float) grapple_js_round1(player->position.x + dist * sin(yaw + M_PI) * cos(pitch));
    g->point.y = (float) grapple_js_round1(eye + dist * sin(pitch));
    g->point.z = (float) grapple_js_round1(player->position.z + dist * cos(yaw + M_PI) * cos(pitch));
    g->range = (float) (dist + player->height - game_constants.camera_height + 0.001);
    if (!game_is_authority(game)) { // if (!serverIO) input.grapObj: the replays hook again
        player->shot_grap_obj = *g;
        player->has_shot_grap_obj = 1;
    }
    // PARTICLES.addRope: client/src/vfx.c draws every active grapple
}

// procInputs while grappling: range -= delta * 0.04 while > 35; off the sphere of radius range
// round the hook, the player is put back on it (UTILS.getDrs angles) and the velocity becomes
// that step times pow(decel, delta) per ms, the frame's friction (vLN01569).
static void player_grapple_pull(Player *player, const float delta_ms, const float decel) {
    PlayerGrapple *g = &player->grapple;
    if (g->range > GRAPPLE_MIN_RANGE) g->range -= delta_ms * GRAPPLE_REEL_SPEED;
    const double d = grapple_d3d(player->position, g->point);
    if (!(d > g->range) || delta_ms <= 0.0f) return;
    // getDrs(player, grapple): [yaw, pitch] from the player to the hook
    const double dx = (double) g->point.x - player->position.x, dy = (double) g->point.y - player->position.y, dz = (double) g->point.z - player->position.z;
    const double pitch = -atan2(dy, sqrt(dx * dx + dz * dz));
    const double yaw = -atan2(-dx, dz) + M_PI / 2.0;
    const double r = g->range;
    const double nx = g->point.x + r * sin(yaw + M_PI / 2.0) * cos(pitch);
    const double nz = g->point.z + r * cos(yaw + M_PI / 2.0) * cos(pitch);
    const double ny = g->point.y + r * sin(pitch);
    const double k = pow(decel, delta_ms) / delta_ms;
    player->velocity.x = (float) ((nx - player->last_position.x) * k);
    player->velocity.y = (float) ((ny - player->last_position.y) * k);
    player->velocity.z = (float) ((nz - player->last_position.z) * k);
    player->position = (vec3) {(float) nx, (float) ny, (float) nz};
}

// ---------------------------------------------------------------- Piss (KrunkNative)
// shared/include/piss.h: the stream flies the branch's particle arc from the hose; it stops at the
// first player, AI, map object or terrain, and a landing leaves a puddle on the ground below.

// findPuddleGroundY: the highest surface under (x, z) at most 0.5 above `from_y` (map objects the
// stream can hit, then the terrain), within PISS_GROUND_PROBE. 0: nothing below.
static int stream_ground(Player *player, const float x, const float from_y, const float z, float *ground) {
    const vec3 origin = {x, from_y + 0.5f, z};
    const vec3 down = {0.0f, -(PISS_GROUND_PROBE + 0.5f), 0.0f};
    HitList hits = {0};
    combat_trace(player, origin, down, 0, &hits);
    float t = 2.0f;
    for (size_t i = 0; i < hits.count; i++) t = fminf(t, hits.items[i].t);
    free(hits.items);
    const float terrain_t = world_terrain_fraction(player->game, origin, down);
    if (terrain_t > 0.0f) t = fminf(t, terrain_t);
    if (t > 1.0f) return 0;
    *ground = origin.y + down.y * t;
    return 1;
}

int player_stream_trace(Player *player, const float pitch, const float yaw, const int include_players, PissStream *out) {
    const WeaponStream *stream = player && player->weapon ? player->weapon->stream : NULL;
    if (!stream || !out || !player->game || !player->game->map) return 0;
    memset(out, 0, sizeof(*out));
    out->source = piss_stream_source(player, stream, pitch, yaw);
    out->velocity = (vec3) {sinf(yaw + (float) M_PI) * cosf(pitch) * stream->speed, sinf(pitch) * stream->speed,
                            cosf(yaw + (float) M_PI) * cosf(pitch) * stream->speed};
    out->gravity = stream->gravity;
    out->time = stream->life;
    out->end = piss_stream_at(out->source, out->velocity, out->gravity, stream->life);

    for (float t0 = 0.0f; t0 < stream->life && !out->hit; t0 += PISS_TRACE_STEP_MS) {
        const float t1 = fminf(stream->life, t0 + PISS_TRACE_STEP_MS);
        const vec3 a = piss_stream_at(out->source, out->velocity, out->gravity, t0);
        const vec3 b = piss_stream_at(out->source, out->velocity, out->gravity, t1);
        const vec3 seg = {b.x - a.x, b.y - a.y, b.z - a.z};
        HitList hits = {0};
        combat_trace(player, a, seg, include_players, &hits);
        hit_list_sort(&hits);
        const float terrain_t = world_terrain_fraction(player->game, a, seg);
        const Hit *first = hits.count ? &hits.items[0] : NULL;
        float t = first ? first->t : 2.0f;
        if (terrain_t > 0.0f && terrain_t <= t) { // the terrain wins ties, like shoot
            t = terrain_t;
            first = NULL;
            out->hit = PISS_HIT_TERRAIN;
        } else if (first) {
            out->hit = first->player ? PISS_HIT_PLAYER : first->ai ? PISS_HIT_AI : PISS_HIT_OBJECT;
            out->player = first->player;
            out->ai = first->ai;
            out->object = first->object;
        }
        if (out->hit) {
            out->time = t0 + (t1 - t0) * t;
            out->end = (vec3) {a.x + seg.x * t, a.y + seg.y * t, a.z + seg.z * t};
        }
        free(hits.items);
    }

    // a landing: the puddle on the ground below, a step back from a wall (buildMustardPuddle +
    // findPuddleGroundY, with the landing point the trace found instead of the branch's fixed throw)
    if (out->hit == PISS_HIT_OBJECT || out->hit == PISS_HIT_TERRAIN) {
        float hx = out->velocity.x, hz = out->velocity.z;
        const float h = sqrtf(hx * hx + hz * hz);
        if (h > 1e-6f) {
            hx /= h;
            hz /= h;
        }
        const float x = out->end.x - hx * 0.3f, z = out->end.z - hz * 0.3f;
        float ground;
        if (stream_ground(player, x, out->end.y, z, &ground)) {
            out->puddle = 1;
            out->puddle_at = (vec3) {roundf(x * 10.0f) / 10.0f, roundf((ground + PISS_PUDDLE_LIFT) * 100.0f) / 100.0f, roundf(z * 10.0f) / 10.0f};
        }
    }
    return 1;
}

// fireMustard (authority): the stream's hit takes the weapon's damage (no head / leg zones), a
// landing makes a puddle; everyone else sees the spray from the SHOT (hit = where it stopped).
static void combat_fire_stream(Player *player, const int weapon_id, const float pitch, const float yaw) {
    Game *game = player->game;
    if (!game_is_authority(game)) return;
    const Weapon *weapon = player->weapon;
    PissStream stream;
    if (!player_stream_trace(player, pitch, yaw, 1, &stream)) return;
    if (player->active) player->life_shots++;
    game_inc_stat(game, player, "s");

    const float damage = weapon->damage * player_attr_damage(player);
    if (stream.hit == PISS_HIT_PLAYER && stream.player) {
        Player *victim = stream.player;
        if (player->active) player->life_hits++;
        game_inc_stat(game, player, "h");
        const float dealt = player_hit_damage(game, weapon, damage, 0, 0);
        if (dealt > 0.0f && player_change_health(victim, player, dealt, 0) && victim->active) {
            PlayerKillInfo info = {.weapon_id = weapon_id, .distance = stream.time / fmaxf(1.0f, weapon->stream->life)};
            player_kill(victim, player, &info, 0);
        }
    } else if (stream.hit == PISS_HIT_AI && stream.ai) {
        if (player->active) player->life_hits++;
        game_inc_stat(game, player, "h");
        game_ai_damage_ex(game, stream.ai, player, player->smite ? 10000.0f : damage, 0, stream.end.y, -1, 0);
    } else if (stream.hit == PISS_HIT_OBJECT && stream.object) {
        game_object_hit(game, (Object *) stream.object, player, damage, 0, GAME_HIT_SHOT); // onShoot / destructibles
    }
    if (stream.puddle) piss_puddle_spawn(game, player, weapon_id, stream.puddle_at);

    NetShotPacket packet = {
        .uid = player->uid,
        .weapon_index = weapon_id,
        .hit = {stream.end.x, stream.end.y, stream.end.z},
        .first = 1,
    };
    game_send(game, NET_TARGET_EXCEPT(player->uid), NET_PACKET_SHOT, &packet, sizeof(packet)); // players + spectators
}

void player_shoot(Player *player) {
    // v7.1.4 shoot: `!player.disableShooting && config.canShoot` (KrunkScript; 1 without a script)
    if (!ks_player_allows(player, KS_PLAYER_CAN_SHOOT)) return;
    // TODO: increase shots stat
    Game *game = player->game;
    const Weapon *weapon = player->weapon;
    const int weapon_id = combat_weapon_index(player);
    const int full = player->full_charge; // procInputs: shoot(..., fullCharge)
    player->shot_blast = (unsigned char) !!full;
    const int charging = !game->config.no_charge_weapons;

    // fullChrgSht weapons (Charge Rifle) spend a round only on the full-charge shot
    // (the KrunkNative Tool Gun spends none: each shot is an Object Manager request, see below)
    if (!player->unlimited_ammo && !weapon->kn_tool && (!weapon->full_charge_shot || full) && player->ammo[player->loadout_index] > 0) player->ammo[player->loadout_index]--;

    player->did_act = 1;

    // v7.1.4 charge: vLN1167 = min(1, chargeTime / chrgTime) unless bypassChrg (which keeps
    // charging between normal shots); rateChrg shortens the interval as the charge builds.
    float charge = 1.0f;
    if (!weapon->bypass_charge) {
        if (weapon->charge_time > 0.0f && charging) charge = MIN(1.0f, player->charge_time / weapon->charge_time);
        player->did_shoot = 1;
        player->charge_time = 0.0f;
        player->full_charge = 0;
    }
    float rate_charge = 1.0f;
    if (weapon->rate_charge && weapon->charge_time > 0.0f) {
        rate_charge = 1.0f - MIN(1.0f, player->charge_time / weapon->charge_time);
        rate_charge = MAX(0.7f, rate_charge * 1.5f);
    }
    if (full) { // the full-charge shot: fullChrgMlt damage (Charge Rifle 50 x 15 = 750), no spread
        player->full_charge = 0;
        player->charge_time = 0.0f;
        charge = weapon->full_charge_mlt > 0.0f ? weapon->full_charge_mlt : 1.0f;
    } else {
        player->did_shoot = 1;
    }

    if (player->burst_count) player->burst_count--;
    else player->burst_count = weapon->burst ? weapon->burst_count - 1 : 0;

    // (burst ? burstR : rate) * (fiRat || 1) * attributes.fRate * (perk 1 ? 0.66 : 1) * (kranked
    // bonuses.firerate) * rateChrg
    player->reloads[player->loadout_index] = (player->burst_count && weapon->burst ? weapon->burst_rate : weapon->rate) *
                                             (game->config.fire_rate > 0.0f ? game->config.fire_rate : 1.0f) *
                                             player_attr_fire_rate(player) * mode_fire_rate_mlt(game, player) * rate_charge;

    // TODO: update player ammo (HUD)
    // TODO: play sound

    player->recoil_force += weapon->recoil * (full ? 3.5f : 1.0f);

    // TODO: gamepad rumble?

#ifdef KRUNKNATIVE_CLIENT
    if (!game->server) { // the view's lean: not on a server's player (the in-process sandbox server)
        const float rand_recoil = ((float) pcg32_random() / (float) UINT32_MAX - 0.5f) * 2.0f * (float) M_PI;

        player->recoil.x += weapon->recoil_r * sinf(rand_recoil);
        player->recoil.z += weapon->recoil_r * 0.3f * cosf(rand_recoil);

        if (player->render_you) {
            player_feel_recoil(player);
            player_cancel_inspect(player);
        }
    }
#endif
    // TODO: muzzle flash & casings

    // KrunkNative Tool Gun: the fire timer, the recoil and the client's tracer only; the client
    // sends the shot as OM_ACTION_TOOL (object_manager_ui.c) and the authority applies the tool
    if (weapon->kn_tool) return;

    // KrunkNative Piss: the stream instead of a bullet, straight along the view (buildMustardPacket
    // reads player.pitch / yaw: noSpread, and the recoil kick only moves the viewmodel)
    if (weapon->stream) {
        combat_fire_stream(player, weapon_id, player->direction.x, player->direction.y);
        return;
    }

    // deployRang (Slimer): tryDeployable places the weapon's spawnable (Slime Pad) instead of firing
    if (weapon->deploy_range > 0.0f) {
        combat_try_deploy(player, weapon_id);
        return;
    }

    // bullet drop (bDrop) turns the pj_disable guns into projectiles with their own pj_* flight
    const int is_projectile = weapon->projectile && weapon->projectile_config.speed > 0.0f && (!weapon->projectile_disable || game->config.bullet_drop);
    const float shot_height = player->position.y + player->height - game_constants.camera_height;

    vec2 shot_angles = {};

    if (is_projectile) {
        // config.projSpreads[shotCntP % 12] scales the spread of each projectile (noProjSpread: none).
        static const float proj_spreads[] = {0.1f, -0.8f, 0.3f, 1.0f, 0.1f, -0.4f, -0.1f, 0.3f, 0.5f, -0.45f, 0.2f, -0.2f};
        player->shot_count++;
        const float projectile_spread = weapon->no_proj_spread ? 0.0f : proj_spreads[player->shot_count % (sizeof(proj_spreads) / sizeof(proj_spreads[0]))];
        const float spread = (player->spread + weapon->inaccuracy) * game_constants.spread_adjustment * projectile_spread;

        shot_angles.x = player->direction.x + player->recoil_anim_y * game_constants.recoil_mlt + spread;
        shot_angles.y = player->direction.y + spread;

        // {dmgMlt: 1 - (chrgMlt || 0.9) * (1 - charge), effectInd / effectC: floor(charge * 10)}
        const float charge_mlt = weapon->charge_mlt > 0.0f ? weapon->charge_mlt : 0.9f;
        const ProjectileShot shot = {
            .damage_mlt = 1.0f - charge_mlt * (1.0f - charge),
            .effect = weapon->effect_on_hit > 0 ? weapon->effect_on_hit - 1 : -1,
            .effect_count = (int) floorf(charge * 10.0f),
        };
        const vec3 origin = {player->position.x, shot_height, player->position.z};
        game_spawn_projectile_ex(game, player, weapon_id, &weapon->projectile_config, origin, shot_angles.y, shot_angles.x, 0, &shot);
    }

    if (!is_projectile || weapon->physical_power) {
        // A projectile weapon only casts its physPow ray (weapons.js shots: 0 for the RPG).
        const int rays = is_projectile ? 0 : weapon->shots ? (int) weapon->shots : 1;
        for (int i = weapon->physical_power ? -1 : 0; i < rays; i++) {
            if (weapon->custom_spread && i >= 0) {
                // v7.1.4 cSpread[ammo % 2]: two patterns of `shots` pairs (one-pattern tables repeat)
                const unsigned int shots = weapon->shots ? weapon->shots : 1;
                const unsigned int offset = (player->ammo[player->loadout_index] % 2) * shots + (unsigned int) i;

                const vec2 spread = weapon->custom_spread[offset % weapon->custom_spread_count];
                const float spread_mlt = (float) pcg32_random() / (float) UINT32_MAX * 0.02f + 0.3f;

                // swizzle spread (x is pitch, y is yaw)
                shot_angles.x = player->direction.x + spread.y * spread_mlt;
                shot_angles.y = player->direction.y + spread.x * spread_mlt;
            } else {
                // the full-charge shot has no spread
                const float spread_range = i >= 0 && !full ? (player->spread + weapon->inaccuracy) * game_constants.spread_adjustment : 0.0f;
                const vec2 spread = {
                    ((float) pcg32_random() / (float) UINT32_MAX - 0.5f) * 2.0f * spread_range,
                    ((float) pcg32_random() / (float) UINT32_MAX - 0.5f) * 2.0f * spread_range,
                };

                shot_angles.x = player->direction.x + spread.x;
                shot_angles.y = player->direction.y + spread.y;
            }

            shot_angles.x += player->recoil_anim_y * game_constants.recoil_mlt;

            combat_fire_ray(player, i, shot_angles.x, shot_angles.y, shot_height, charge);
        }
    }

    combat_remove_if_empty(player, weapon_id);
}

// v7.1.4 setClass / Player.reset combat state: throwCharge from the class, no effects, perks,
// charge or Juggernaut, realKillStreak 0; the earned streaks are kept (player.streaks).
static void combat_spawn(Player *player, const ClassConfig *class_config) {
    effects_reset(player);
    player->upgraded_slots = 0;
    player->weapon_streak = 0;
    player->streak_weapon = -1;
    player->is_juggernaut = 0;
    player->real_kill_streak = 0;
    player->throw_charge = class_config->throw_charge > 0.0f ? class_config->throw_charge : COMBAT_THROW_CHARGE;
    player->start_health = player->max_health;
    if (player->head_mlt <= 0.0f) player->head_mlt = 1.0f;
}

// v7.1.4 player.js procInputs charge (game setting chrgWeps): shootChrg weapons charge while
// the trigger is held, the others (Crossbow, Zapper, the knife throw) while aiming in. Past
// chrgTime the charge creeps on at delta * 0.1 / (160 / (chrgTime * (overCharge - 1))) ms per
// ms (chrgTime * 0.125 s per s); ovrChrg weapons reset at chrgTime * 1.2 and stay overCharged
// until un-aimed, the others clamp at chrgTime as fullCharge. chrgDec weapons decay instead of
// resetting when released. mode.forceCharge has no KN mode yet.
static void combat_update_charge(Player *player, const Input *input, const float delta) {
    const Weapon *weapon = player->weapon;
    if (!weapon || player->game->config.no_charge_weapons) return;

    const int charging = weapon->shoot_charge ? input->shoot : player->aim_val > 0.0f; // Krunker aimVal < 1
    const float full = weapon->charge_time > 0.0f ? weapon->charge_time : player->throw_charge > 0.0f ? player->throw_charge : COMBAT_THROW_CHARGE;
    const int chargeable = (weapon->charge_time > 0.0f && player->ammo && player->ammo[player->loadout_index] > 0) ||
                           (player->can_throw && weapon->can_throw);

    if (charging && !player->over_charged && player->swap_timer <= 0.0f && chargeable) {
        player->charge_time += player->charge_time >= full ? delta * 0.1f / (0.16f / (full * (COMBAT_OVER_CHARGE - 1.0f))) : delta;
        if (weapon->over_charge) {
            if (player->charge_time >= full * COMBAT_OVER_CHARGE) {
                player->charge_time = 0.0f;
                player->full_charge = 0;
                player->over_charged = 1;
            }
        } else if (player->charge_time >= full - 1e-5f) { // v7 sums whole ms: 70 frames of 10 ms are 700, not 699.99
            player->full_charge = 1;
            player->charge_time = full;
        }
    } else if (weapon->charge_decay > 0.0f && player->charge_time > 0.0f) {
        player->charge_time -= weapon->charge_decay * delta;
        player->full_charge = 0;
        if (player->charge_time <= 0.0f) {
            player->charge_time = 0.0f;
            if (player->aim_val == 0.0f) player->over_charged = 0;
        }
    } else {
        player->full_charge = 0;
        player->charge_time = 0.0f;
        if (player->aim_val == 0.0f) player->over_charged = 0;
    }
}

// index.js Math.lerpAngle(a, b, t): b + (a - b) * t the short way round, wrapped into [0, 2PI]
// by % (so a negative result stays negative, as in JS).
// Math.lerpAngle(from = a, to = b, t) adds 2pi to the smaller angle when they are more than pi
// apart, which is the shortest arc only while |a - b| < 3pi. Here the difference is always
// taken modulo 2pi, so endpoints from different wraps (or a stale, unwrapped yaw) still turn
// the short way instead of spinning; for the v3.1.1 range (-2pi, 2pi) the result is the same.
float player_lerp_angle(float a, float b, const float t) {
    const float pi2 = (float) M_PI * 2.0f;
    if (!isfinite(a)) a = isfinite(b) ? b : 0.0f;
    if (!isfinite(b)) b = a;
    b = player_wrap_yaw(b);
    const float d = normalize_angle(player_wrap_yaw(a) - b); // shortest signed arc b -> a, [-pi, pi)
    const float v = b + d * t;
    return v >= 0.0f && v <= pi2 ? v : fmodf(v, pi2);
}

// procInputs every queued network frame (the tick, and gameserver.ts io_int before its action).
void player_drain_inputs(Player *player) {
    if (!player->active || !player->input_queue_size) return;

    // Detached first: an input can kill the player (kill objects, authority), and
    // player_kill() drops the queue; the rest of a dead player's inputs are discarded.
    Input *queue = player->input_queue;
    const size_t count = player->input_queue_size;
    player->input_queue = NULL;
    player->input_queue_size = 0;

    for (size_t i = 0; i < count && player->active; i++) {
        player->input_arrival = queue[i].arrival; // anchored lag compensation: this input's shots
        player->input_running = queue[i];
        player_proc_input(player, &queue[i], 0, player->game->move_lock || mode_input_locked(player)); // lockMove / lockPlayer
        player->input_seq = queue[i].seq;
    }
    player->input_arrival = 0.0;
    player->input_running.received = 0.0;

    free(queue);
}

void player_update(Player *player, const float delta) {
    if (!player->active) return;

    player_feel_update(player, delta);
    player_drain_inputs(player);

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

        // yaw = lerpAngle(xDir2, xDir1, min(1, interp)), pitch likewise (players.js update)
        const float t = MIN(1.0f, progress);
        player->direction.y = player_lerp_angle(player->interp_dir_end.y, player->interp_dir_start.y, t);
        player->direction.x = player_lerp_angle(player->interp_dir_end.x, player->interp_dir_start.x, t);
    }
}
