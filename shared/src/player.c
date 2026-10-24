#include <shared.h>
#include <prop_hunt.h> // Prop Hunt: the disguise a spawn resets / gives
#include <player_anim.h> // v10 PLAYER_ANIM: the Jump / Melee clips of an animated class model
#include <net_ext.h>     // NET_PLAYER_CLIP_*
#include <stalker.h> // v10 Stalker: noMovT / isHidden
#include <world.h> // map-logic WP-B: tris, zones, score zones, terrain, resets
#include <ai.h>
#include <bot_chat.h>
#include <spawnables.h>
#include <zombies.h> // map-logic WP-H: finiteAmmo, double points, machines, airdrops
#include <game_config.h> // WP-E: config.classes, forceC
#include <ks_game.h> // KrunkScript hook points (onPlayerUpdate / Spawn / Damage / Death)
#include <triggers_v7.h> // WP-C: onPlayerSpawn / Damage / Die / Kill map triggers
#include <flavor_rules.h> // product-flavor gates (Krunker Strike: its six classes)
#include <player_card.h> // WP-E: the death screen's life accuracy
#include <piss.h> // KrunkNative Piss: the stream and its puddles
#include <math.h>
#include <pcg_basic.h>
#include <drank.h> // KrunkNative Drank: puddle slow, the laced pour-up
#include <stdio.h>
#include <string.h>
#include <ruleset.h>
#include <hitboxes.h> // v10.0.2 posed hit boxes

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
    player->skin_paints = PLAYER_SKIN_PAINTS_NONE;
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
    float drop = player->crouch_val * game_constants.crouch_distance;
    // v10 updateHeight (deob-10.0.3.js:193560-193566): on a rendering client the local player's pose bodyY
    // moves the eye with the posed body (an emote's lowering); render_you is never set on an authority, which
    // like v10's server keeps the plain height (the hit boxes do not follow the pose)
    if (player->render_you && (player->pose.has & PLAYER_POSE_BODY_Y)) drop -= player->pose.body_y;
    player->height = game_constants.player_height - drop;
}

// setClass: the class loadout of `player` from its class_index and secondary_index: the primary, the picked
// secondary (when the class has a slot for one) and the knife, health and speed, the ammo and the extra weapons.
// A spawn runs it, and so does player_switch_class mid-life. 0 when out of memory (the loadout is then NULL).
static int player_build_loadout(Player *player) {
    const size_t class_count = player->game->class_count ? player->game->class_count : rules_class_count();
    const ClassConfig *classes = player->game->classes ? player->game->classes : rules_classes();
    const size_t weapon_count = player->game->weapon_count ? player->game->weapon_count : rules_weapon_count();
    Weapon **weapons = player->game->weapons ? player->game->weapons : (Weapon**) rules_weapons();

    // setClass (v3.1.1 src/data/players.js): mode overrides, then primary + optional secondary + knife.
    // WP-E (v7 player.js:649-664): config.classes first (classes[0] when not listed), and the
    // mode's classes only with forceC.
    player->class_index = game_config_class_for(player->game, player->class_index);
    // Krunker Strike: a pick Strike lacks plays its starter class; a mode's forced class still
    // applies below (flavor_rules.h; base: the pick itself)
    player->class_index = flavor_class_pick(player->class_index);
    if (player->game->mode && player->game->config.force_classes) {
        const GameModeConfig *mode_config = &player->game->mode->config;
        if (mode_config->no_weapons) player->class_index = 9;
        if (mode_config->force_class >= 0) player->class_index = mode_config->force_class;
        if (player->team >= 0 && player->team < 3 && mode_config->team_class[player->team] >= 0) player->class_index = mode_config->team_class[player->team];
    }

    if (player->class_index < 0 || player->class_index >= (int) class_count) {
        player->class_index = 0;
    }
    // KrunkNative's own classes (the Nettspend) are out of an official krunker.io game only (game_class_allowed)
    if (!game_class_allowed(player->game, player->class_index)) player->class_index = 0;

    const ClassConfig *class_config = &classes[player->class_index];
    int loadout[3] = {1, WEAPON_ID_KNIFE, WEAPON_ID_KNIFE};
    int loadout_size = 0;
    const int primary = class_config->loadout ? class_config->loadout[0] : 1;
    const int fallback_weapon = weapon_count > 1 ? 1 : 0;
    const int primary_index = primary >= 0 && primary < (int) weapon_count && weapons[primary] ? primary : fallback_weapon;
    // setClass(game, classIndex, secIndex): the picked secondary when it is a secondary weapon, else the Pistol.
    // KrunkNative: a class with kn_secondary (the Nettspend's Money Spread) always carries that one.
    const int fixed_secondary = class_config->kn_secondary > 0 ? class_config->kn_secondary - 1 : -1;
    const int picked_secondary = fixed_secondary >= 0 && fixed_secondary < (int) weapon_count && weapons[fixed_secondary] ? fixed_secondary : player->secondary_index;
    // v10 updateSecIndex (v10.js:169364) with weapons.js req (game_weapon_req): a secondary the req refuses is the
    // Pistol. req's first clause lets a dev through (account.isDev: player->dev, the server's net_client_is_dev at
    // the spawn, as the picker's acc->is_dev); otherwise customOnly secondaries (the Grappler) only in a custom
    // game that is not comp, noComp ones out of comp lobbies. A class-bound secondary (the Nettspend's Money
    // Spread) is the class's, not a pick: no gate, so the class plays the same in every lobby (doc/v7/decisions.md
    // "Nettspend class"). A client's copy of a player (not the authority) takes the SPAWN's secondary as is, like
    // setClass pushes the secIndex the server sent: the server settled it (it knows who is a dev, the copy not).
    const int bound = fixed_secondary >= 0 && picked_secondary == fixed_secondary;
    const int settled = !game_is_authority(player->game);
    const int secondary_index = picked_secondary >= 0 && picked_secondary < (int) weapon_count && weapons[picked_secondary] && weapons[picked_secondary]->secondary &&
        (bound || settled || game_weapon_req(player->game, weapons[picked_secondary], player->dev))
        ? picked_secondary : WEAPON_ID_PISTOL < (int) weapon_count ? WEAPON_ID_PISTOL : -1;
    // updateSecIndex settles the player's secIndex (the pick, or 2 when refused): the SPAWN carries that one, so
    // every copy of this player builds the loadout the authority did. The connection keeps the pick itself.
    if (class_config->secondary && !bound && secondary_index >= 0) player->secondary_index = secondary_index;
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
        return 0;
    }

    for (int i = 0; i < loadout_size; i++) {
        player->loadout[i] = loadout[i];
        player->ammo[i] = (unsigned int) mode_ammo_limit(player->game, (int) weapons[player->loadout[i]]->ammo); // v7 ammoLimit (aon 0)
    }
    player->loadout_base = loadout_size;
    player_loadout_append_extras(player); // KrunkNative Object Manager: extra weapons after the class loadout

    return 1;
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

    if (!player_build_loadout(player)) {
        player->active = 0;
        return;
    }

    player->velocity.x = 0.0f;
    player->velocity.y = 0.0f;
    player->velocity.z = 0.0f;

    player->crouch_val = 0.0f;
    player->aim_val = 0.0f;
    player_reset_feel(player); // Player.reset: step/bob/lean/recoil feel from rest

    player->on_ground = 1;
    player->noclip_prev = player->noclip; // a spawn is not a flip off
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
    // v10.0.2 Player.reset (v10:169129-169130, 169259-169260)
    player->stuck_to_wall = 0;
    player->stuck_to_wall_dir = 0;
    player->last_stalker_leap = 0.0f;
    player->stalker_coyote_time = 0.0f;
    player->no_move_time = 0.0f; // init: noMovT = 0, isHidden = false (deob-10.0.3.js:190674, :190729)
    player->is_hidden = 0;

    player->air_time = 0.0f;
    player->covered_distance = 0.0f;
    player->aim_time = 0.0f;
    player->aim_dir = 0; // Player.init aimDir = 0 (deob-10.0.3.js:190608)

    player->swap_timer = 0.0f;
    player->slide_timer = 0.0f;
    player->jump_timer = 0.0f;
    player->reload_timer = 0.0f;
    player->laced_timer = player->laced_duration = 0.0f; // KrunkNative Drank: a new life is sober

    player_update_height(player);
    game_prop_spawned(player->game, player); // Prop Hunt: player.init's prop reset; a Prop-team spawn wears a random prop
    player_movement_spawn(player); // WP-A: wall-jump limits and v7 movement state
    player_swap_weapon(player, 0, 1, 0, 0);

    // getSpawnPoint(player.team, player): every mode, offline and server, bots included.
    // player.init: yDire = 0 (pitch level); the ENT: xDire = -((spawn.dir || 0) + 1) * PI / 2.
    // A map without spawn points (KrunkNative only: v7.1.4 reads map.spawns[0]) keeps the
    // position but still looks level along dir 0, never along the last life's look.
    vec3 spawn_position;
    int spawn_direction = 0;
    const int has_spawn = game_get_spawn_point(player->game, player, &spawn_position, &spawn_direction);
    if (has_spawn) player->position = spawn_position;
    player->direction.x = 0.0f;
    player->direction.y = -(float) M_PI / 2.0f * (1.0f + (float) spawn_direction);

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

// KrunkNative class switch (the console's class, offline sandbox): the class and its secondary take effect
// now, as a respawn gives them (the loadout, full health, the class's speed, wall jumps and throw charge).
// Where the player stands and looks, and its input stream, stay as they are: no spawn point, no input reset.
// 0 when out of memory (the player is then inactive, as a failed spawn leaves it).
int player_switch_class(Player *player, const int class_index, const int secondary_index) {
    free(player->loadout);
    free(player->ammo);
    free(player->reloads);
    player->loadout = NULL;
    player->ammo = NULL;
    player->reloads = NULL;
    player->loadout_size = 0;
    player->class_index = class_index;
    player->secondary_index = secondary_index;
    if (!player_build_loadout(player)) {
        player->active = 0;
        return 0;
    }
    player_movement_spawn(player); // the class's wall-jump limits and movement state (setClass)
    player_swap_weapon(player, 0, 1, 0, 0); // the held weapon is the new primary, as a spawn leaves it
    return 1;
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

// v10.0.2 player.isStalker (v10:168997: mode.teamOptions[team - 1] == "Stalker") in a mode with the v10
// Stalker rework (GameModeConfig.stalker_v10, only set by the v10 mode table): leap, wall stick,
// coyote time, wall jumps off notWallJ walls, and only the stalker slides.
static int movement_stalker(const Player *player) {
    const Game *game = player->game;
    if (!game || !game->mode || !game->mode->config.stalker_v10 || player->team < 1 || player->team > 2) return 0;
    const char *option = game->mode->config.team_options[player->team - 1];
    return option && !strcmp(option, "Stalker");
}

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
    // resetMovVals (deob-10.0.3.js:195029-195040, on spawn :191059 and on a class change :190817): the
    // movement scheme's sendVals back to 0 (tmpVel {0, 0, 0}); the cmd and overlay copies go with them
    memset(&player->scheme, 0, sizeof(player->scheme));
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
    int no_sliding = mode->no_sliding && !player_has_perk(player, PERK_SLIDE) && !player_has_perk(player, PERK_SLIDE_EXPLODE);
    // v10 mode.canSlide overrides it (v10:171971): Stalker `canSlide: p => p.isStalker`
    if (game->mode && game->mode->config.stalker_v10) no_sliding = !movement_stalker(player);
    const int slide = !movement_real(player) && player->crouch_val && player->can_slide && player->can_slide_timer <= 0.0f &&
                      game->config.can_slide && !no_sliding;

    if (slide) {
        const ClassConfig *class_config = movement_class(player);
        player->can_slide = 0;
        player->can_slide_timer = MOVEMENT_SLIDE_LOCK;
        // config.slideTime * (slidTime || 1) * (slT || 1) * crouchVal, in v7's double ms
        movement_set_slide_ms(player, movement_js(game_constants.slide_time * 1000.0f) * movement_js(MOVEMENT_OR(game->config.slide_time, 1.0f)) *
                                      movement_js(class_config ? MOVEMENT_OR(class_config->slide_time_mlt, 1.0f) : 1.0f) * movement_js(player->crouch_val));
        // (this.onTerrain ? 0.4 : 1.2) * slA (v10:171975): `this` is the players manager, whose
        // onTerrain is always undefined, so every slide gets 1.2, on terrain too. Mirrored on purpose:
        // official servers run it, and a 0.4 here desynced terrain slides against them.
        float slide_mlt = game_constants.player_slide_velocity_mlt * (class_config ? MOVEMENT_OR(class_config->slide_accel_mlt, 1.0f) : 1.0f);
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

// rampFix ("Solid Ramp Sides", v10.0.3 Player.blockRampSide): a walker that comes at a ramp from
// beside, below its surface, is moved out to the side it came from (x / z faces of the ramp's box,
// padded by collision_padding), and its x / z velocity is cleared. Returns 0 when no side applies.
static int movement_block_ramp_side(Player *player, const MovementBox *box, const float s) {
    const float pad = game_constants.collision_padding;
    if (player->last_position.x - s >= box->x + box->w - pad) {
        player->position.x = box->x + box->w + s;
        player->velocity.x = 0.0f;
    } else if (player->last_position.x + s <= box->x - box->w + pad) {
        player->position.x = box->x - box->w - s;
        player->velocity.x = 0.0f;
    } else if (player->last_position.z - s >= box->z + box->l - pad) {
        player->position.z = box->z + box->l + s;
        player->velocity.z = 0.0f;
    } else if (player->last_position.z + s <= box->z - box->l + pad) {
        player->position.z = box->z - box->l - s;
        player->velocity.z = 0.0f;
    } else {
        return 0;
    }
    return 1;
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
                top - player->position.y > game_constants.climb_height && (object->wall_jumpable || movement_stalker(player))) { // v10: the stalker any wall (v10:169909)
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

                // rampFix (v10.0.3 blockRampSide): a rise of more than 3 above the walker, from below the
                // ramp's foot, is a side that blocks it. The map flag or the game config's rampFix turns it on.
                if ((game->map->ramp_fix || game->config.ramp_fix) && collision_height - player->last_position.y > 3.0f &&
                    player->last_position.y - bottom < 3.0f && movement_block_ramp_side(player, &box, s)) {
                    continue;
                }

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
    player->did_stick_to_wall = 1; // v10 jump() (v10:172016): a jump does not also stick to a wall
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
    player_play_clip(player, NET_PLAYER_CLIP_JUMP); // `if (!recon) playClip(player, "Jump", 1)` (deob-10.0.3.js:195271; authority only)
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

    // KrunkNative Drank: the pour-up may lace the pourer (authority-rolled, shared/include/drank.h)
    if (player->weapon->stream && player->weapon->stream->laced_chance > 0.0f) drank_pour(player);
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

// ---------------------------------------------------------------- v10.0.3 movement schemes
// Krunker v10.0.3 game.movement (deob-10.0.3.js:18553-19199), picked by the class movSch
// (`game.movement[this.config.movSch || 0]`, deob-10.0.3.js:191275). procInputs calls the scheme's
// dir (the look, deob-10.0.3.js:191385-191391, under defaultRotation), mov (the acceleration,
// :191716-191722, under defaultMovement and not stuck to a wall) and vel (the integration,
// :191741-191747, under defaultVelocity); a scheme without one runs Default's (movement[0]). The
// scheme keys: airDecel (SRM 1: the air decel), noThrow (SRM: no aiming, :191450) and noWallJ (SRM:
// no wall jumps, :192436). Everything runs in player_proc_input on both sides, so the prediction
// and the server agree; SRM's and Vehicle's sendVals reach the owner with each reconciliation
// (NET_EXT_PLAYER_MOVE_VALS, shared/include/net_ext.h). doc/v10/movement-schemes.md.
//
// movDir is config.movDirs[input[4]] + movDrP * PI (deob-10.0.3.js:191302-191305), undefined
// without a move key or while KrunkScript disables it. SRM's keys and dashes and Vehicle's
// throttle and steering read the raw input array (input[4] / [6] / [7]), which KrunkScript's
// disabledInputs never touch: a script that disables movDir still drives a vehicle.

// SRM's config (deob-10.0.3.js:18685-18726)
#define SRM_FRICTION 0.006
#define SRM_BASE_SPEED 0.07
#define SRM_GROUND_ACCEL 0.014
#define SRM_GROUND_DECEL 0.01             // groundDeacceleration: the friction's minimum speed
#define SRM_AIR_ACCEL 0.00085
#define SRM_AIR_DECEL 0.00085             // airDeacceleration: accelerating against the velocity
#define SRM_SIDE_STRAFE_ACCEL 0.05
#define SRM_SIDE_STRAFE_SPEED 0.01
#define SRM_MIN_RAMP_SLIDE_VELOCITY 0.125
#define SRM_ON_WALL_MAX_MS 116.0          // onWallMaxFrames
#define SRM_FRICTION_DELAY_MS 33.0        // mov: onGroundFrames <= 33 lands without friction or ground move
#define SRM_WISH_DASH_MS 300.0f           // mov: input[6] keeps the dash wanted for 300 ms

typedef struct {
    double acceleration, cooldown, speed, height;
} SrmDash;
static const SrmDash k_srm_dash = {1000.0, 1000.0, 0.09999500000000001, 0.035};
static const SrmDash k_srm_walldash = {1000.0, 1150.0, 0.052500000000000005, 0.07};
// wallNormal[onWall - 1] (onWall 1: +x side of the wall, 2: -x, 3: +z, 4: -z)
static const double k_srm_wall_normal[4][3] = {{1.0, 0.0, 0.0}, {-1.0, 0.0, 0.0}, {0.0, 0.0, 1.0}, {0.0, 0.0, -1.0}};

MovementScheme player_movement_scheme(const Player *player) {
    const int scheme = movement_knobs(player)->scheme;
    return scheme > 0 && scheme < MOVEMENT_SCHEME_COUNT ? (MovementScheme) scheme : MOVEMENT_SCHEME_DEFAULT;
}

// (trnSp || 1) * (trnSpd || 1): Default.dir's and Vehicle.dir's turn speed
static float movement_turn_speed(const Player *player) {
    const ClassConfig *class_config = movement_class(player);
    return MOVEMENT_OR(movement_knobs(player)->turn_rate, 1.0f) * (class_config ? MOVEMENT_OR(class_config->turn_speed, 1.0f) : 1.0f);
}

// Default.dir at a turn speed of 1 or more copies the input look; Vehicle and MMORPG turn the body
// themselves, and a slower turn lags it. The camera then keeps the mouse look (v10's camera is
// always CONTROLS; the player model is xDire / yDire).
int player_direction_is_look(const Player *player) {
    const MovementScheme scheme = player_movement_scheme(player);
    return scheme != MOVEMENT_SCHEME_VEHICLE && scheme != MOVEMENT_SCHEME_MMORPG && movement_turn_speed(player) >= 1.0f;
}

// KrunkNative's Input.move_dir k (0 forward, clockwise, -1 none) as v10's input[4] index i:
// config.movDirs[i] = -PI + (i + 1) * PI / 4 (deob-10.0.3.js:19251-19253), so i = (k + 1) % 8.
static int movement_v10_dir(const int move_dir) {
    return move_dir >= 0 && move_dir <= 7 ? (move_dir + 1) % 8 : -1;
}

// movDir: config.movDirs[i] + (movDrP || 0) * PI in v10's double
static double movement_dir_angle(const Player *player, const int v10_dir) {
    return -M_PI + (double) (v10_dir + 1) * M_PI / 4.0 + movement_js(movement_knobs(player)->move_dir_offset) * M_PI;
}

// The air decel a scheme sets (`IÍîiíîî.airDecel || this.config.airDec || config.airDecel`, :191631), 0 = none
static float movement_scheme_air_decel(const Player *player) {
    return player_movement_scheme(player) == MOVEMENT_SCHEME_SRM ? 1.0f : 0.0f;
}

static int movement_scheme_no_throw(const Player *player) {
    return player_movement_scheme(player) == MOVEMENT_SCHEME_SRM;
}

static int movement_scheme_no_wall_jump(const Player *player) {
    return player_movement_scheme(player) == MOVEMENT_SCHEME_SRM;
}

// Default.dir (deob-10.0.3.js:18555-18588): at a turn speed (trnSp || 1) * (trnSpd || 1) of 1 or more
// xDire / yDire take the input look; below it they turn toward it at speed * 0.01 rad per ms.
static void scheme_default_dir(Player *player, const Input *input, const float delta) {
    const float turn = movement_turn_speed(player);
    if (turn >= 1.0f) {
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
}

// Vehicle.dir (deob-10.0.3.js:19107-19141): on the ground the W keys (input[4] 0..2) build forwardV by
// vhpF per 60 fps frame up to vhmP, the S keys (4..6) reverseV by vhrF up to vhmR, both bleed off
// without their keys. Above vhtrsh forwardV (or in reverse) the D keys (2..4) and A keys (0, 6, 7)
// steer angVel by (trnSp || 1) [* vhtrsv * 10 * (forwardV + reverseV)] * (trnSpd || 1) * 0.004 a
// frame, mirrored in reverse. The look (input[2] / [3]) does not turn a vehicle.
static void scheme_vehicle_dir(Player *player, const int v10_dir, const double dt_ms) {
    if (!player->on_ground) return;
    const ClassOverrides *vh = class_overrides(movement_class(player));
    MovementSchemeState *s = &player->scheme;
    const double frames = dt_ms / (1000.0 / 60.0);
    const int forward = v10_dir == 0 || v10_dir == 1 || v10_dir == 2;
    const int reverse = v10_dir == 4 || v10_dir == 5 || v10_dir == 6;
    double forward_v = s->forward_v, reverse_v = s->reverse_v;
    forward_v += (forward ? 1.0 : -1.0) * movement_js(vh->vehicle_power_force) * frames;
    reverse_v += (reverse ? 1.0 : -1.0) * movement_js(vh->vehicle_reverse_force) * frames;
    forward_v = MAX(0.0, MIN(movement_js(vh->vehicle_max_power), forward_v));
    reverse_v = MAX(0.0, MIN(movement_js(vh->vehicle_max_reverse), reverse_v));
    s->forward_v = (float) forward_v;
    s->reverse_v = (float) reverse_v;

    const ClassConfig *class_config = movement_class(player);
    double turn = movement_js(MOVEMENT_OR(movement_knobs(player)->turn_rate, 1.0f));
    if (vh->vehicle_turn_conv) turn *= movement_js(vh->vehicle_turn_conv) * 10.0 * (forward_v + reverse_v);
    turn *= class_config ? movement_js(MOVEMENT_OR(class_config->turn_speed, 1.0f)) : 1.0;
    turn *= 0.004;
    const double sign = reverse ? -1.0 : 1.0;
    if (forward_v >= movement_js(vh->vehicle_turn_threshold) || reverse) {
        double ang_vel = s->ang_vel;
        if (v10_dir == 2 || v10_dir == 3 || v10_dir == 4) ang_vel -= sign * frames * turn;
        if (v10_dir == 0 || v10_dir == 6 || v10_dir == 7) ang_vel += sign * frames * turn;
        s->ang_vel = (float) ang_vel;
    }
}

// MMORPG.dir (deob-10.0.3.js:19178-19189): with a move key the body (xDire) turns toward the
// direction it moves in (mouseX - movDir - PI / 2) at 0.012 rad per ms; the pitch (yDire) is never
// set, and without a key the body keeps its heading while the camera turns freely.
static void scheme_mmorpg_dir(Player *player, const Input *input, const int v10_dir, const double dt_ms) {
    if (v10_dir < 0) return; // movDir != null
    const double pi2 = M_PI * 2.0, step = dt_ms * 0.012;
    const double target = movement_js(input->y_dir) - movement_dir_angle(player, v10_dir) - M_PI / 2.0;
    const double d = fmod(movement_js(player->direction.y) - target + pi2, pi2);
    const double move = MIN(MIN(fabs(d - pi2), d), step);
    player->direction.y = (float) fmod(movement_js(player->direction.y) + (d - M_PI >= 0.0 ? 1.0 : -1.0) * move + pi2, pi2);
}

// movement[movSch].dir || movement[0].dir (deob-10.0.3.js:191385-191391). `raw_dir` is input[4]
// as sent (Vehicle reads it unmasked), `masked_dir` the movDir after KrunkScript (MMORPG's).
static void movement_scheme_dir(Player *player, const Input *input, const int raw_dir, const int masked_dir, const float delta) {
    const double dt_ms = round((double) delta * 10000.0) / 10.0;
    switch (player_movement_scheme(player)) {
        case MOVEMENT_SCHEME_VEHICLE: scheme_vehicle_dir(player, movement_v10_dir(raw_dir), dt_ms); break;
        case MOVEMENT_SCHEME_MMORPG: scheme_mmorpg_dir(player, input, movement_v10_dir(masked_dir), dt_ms); break;
        default: scheme_default_dir(player, input, delta); break;
    }
}

// Default.mov (deob-10.0.3.js:18589-18605): the keys relative to the look (xDire). A "3rd Person"
// class camera (camType 1) without mouse rotation (camRot) moves relative to its fixed offset
// instead: -getDir(camOffX, camOffZ, 0, 0) % PI. Diagonals (movDir off 0, +-PI/2 and PI) get
// config.strafeSpd unless the mode is realMov; noclip flies up or down with the pitch.
static void scheme_default_mov(Player *player, const float accel_in, const int v10_dir) {
    if (v10_dir < 0) return; // movDir !== undefined (the strafe factor before it changes nothing then)
    double origin = movement_js(player->direction.y);
    const ClassOverrides *cam = class_overrides(movement_class(player));
    if (cam->cam_type == 1 && !cam->cam_rotation) origin = fmod(-atan2(movement_js(cam->cam_offset[2]), movement_js(cam->cam_offset[0])), M_PI);
    const double move_dir = movement_dir_angle(player, v10_dir);
    float accel = accel_in;
    if (move_dir != M_PI && move_dir != 0.0 && move_dir != M_PI / 2.0 && move_dir != -M_PI / 2.0 && !movement_real(player)) accel *= player->game->config.strafe_speed;
    // in double like v10: 90 degrees off the look direction the cosine is a tiny difference of two
    // angles, and float error there drifts the player sideways over a long climb
    player->velocity.x += (float) (accel * cos(move_dir - origin));
    player->velocity.z += (float) (accel * sin(move_dir - origin));
    if (player->noclip) player->velocity.y += accel * (player->direction.x * (move_dir > 0.0 ? -1.0f : 1.0f));
}

// Dir Only.mov (deob-10.0.3.js:18673-18679): the keys move along the world axes, the look ignored
// (W -z, D +x; no strafeSpd).
static void scheme_dir_only_mov(Player *player, const float accel, const int v10_dir) {
    if (v10_dir < 0) return;
    const double move_dir = movement_dir_angle(player, v10_dir);
    player->velocity.x += (float) (accel * cos(move_dir));
    player->velocity.z += (float) (accel * sin(move_dir));
}

// MMORPG.mov (deob-10.0.3.js:19190-19197): the keys relative to the input look (the camera), not to
// the body: angle = -((mouseX - movDir - PI / 2) % 2PI) - PI / 2. No strafeSpd.
static void scheme_mmorpg_mov(Player *player, const Input *input, const float accel, const int v10_dir) {
    if (v10_dir < 0) return;
    double angle = fmod(movement_js(input->y_dir) - movement_dir_angle(player, v10_dir) - M_PI / 2.0, M_PI * 2.0);
    angle = -angle - M_PI / 2.0;
    player->velocity.x += (float) (accel * cos(angle));
    player->velocity.z += (float) (accel * sin(angle));
}

// ---- SRM (deob-10.0.3.js:18680-19021): Quake-style movement. On the ground friction and a
// baseSpeed acceleration (no friction for the first 33 ms after a landing, so a held jump keeps a
// bhop's speed), in the air the classic strafe acceleration (side keys alone: sideStrafeSpeed /
// sideStrafeAcceleration), the jump key auto-hops, the aim key dashes (on the ground) or wall-dashes
// (off a wall in the air, the velocity reflected off it), and a ramp hit fast enough bounces.

static double srm_len_xz(const vec3 v) {
    return sqrt((double) v.x * v.x + (double) v.z * v.z);
}

// UpdateVelocity: tmpVel = velocity
static void srm_update_velocity(Player *player) {
    player->scheme.tmp_vel = player->velocity;
}

// AutoBhop: players.jump(player); wishJump = justJumped = true
static void srm_auto_bhop(Player *player, int *just_jumped) {
    player_jump(player);
    *just_jumped = 1;
}

// iíïiïíí(ramp) (deob-10.0.3.js:28436-28444): the ramp's slope in degrees (dir 0 / PI along x, else z)
static double srm_ramp_angle(const Object *ramp) {
    const vec3 size = ramp->scale;
    const double run = ramp->direction == 0 || ramp->direction == 2 ? size.x : size.z;
    return atan((double) size.y / run) * 57.29577951308232;
}

// a := (x, y, z) of tmpVel along nDir[ramp.dir]: "x" for dir 0 / PI, "z" for PI / 2 / 1.5 PI
static double srm_ramp_axis_speed(const Player *player, const Object *ramp) {
    return ramp->direction == 0 || ramp->direction == 2 ? player->scheme.tmp_vel.x : player->scheme.tmp_vel.z;
}

// SetAccelerationValues (deob-10.0.3.js:18984-19020, isYou): the strafe overlay's angles. Only the HUD
// reads them (client/src/vfx.c). v10 also zeroes tmpVel.y there; the next UpdateVelocity always
// overwrites it before anything reads it, so that is left out.
static void srm_overlay(Player *player, const double cmd[3], const double wishdir[3], const double wishspeed, const double accel, const double dt_ms) {
    MovementSchemeState *s = &player->scheme;
    if (!(player->air_time > 0.0f && (s->cmd_forward || s->cmd_right))) {
        s->overlay_valid = 0;
        return;
    }
    const double v[3] = {s->tmp_vel.x, 0.0, s->tmp_vel.z};
    const double w[3] = {wishdir[0], 0.0, wishdir[2]};
    const double lc = sqrt(cmd[0] * cmd[0] + cmd[2] * cmd[2]);
    const double lv = sqrt(v[0] * v[0] + v[2] * v[2]);
    const double lw = sqrt(w[0] * w[0] + w[2] * w[2]);
#define SRM_SIGN_CROSS(a, b) ((a)[0] * (b)[2] - (a)[2] * (b)[0] < 0.0 ? -1.0 : (a)[0] * (b)[2] - (a)[2] * (b)[0] > 0.0 ? 1.0 : 0.0)
    const double base = acos((cmd[0] * w[0] + cmd[2] * w[2]) / (lc * lw)) * SRM_SIGN_CROSS(w, cmd);
    const double side = SRM_SIGN_CROSS(w, v);
    const double optimal = side * acos(wishspeed * (1.0 - accel * dt_ms) / (lv * lw)) - base;
    double minimum = wishspeed / (2.0 - lw * lw) * lw / lv;
    minimum = side * acos(minimum < 1.0 ? minimum : 1.0) - base;
    const double maximum = side * acos(accel * -0.5 * dt_ms * wishspeed * lw / lv) - base;
    double current = SRM_SIGN_CROSS(cmd, v) * acos((v[0] * cmd[0] + v[2] * cmd[2]) / (lv * lc));
#undef SRM_SIGN_CROSS
    const double pi2 = M_PI * 2.0;
    current += trunc((minimum - current) / pi2) * pi2;
    current += trunc((maximum - current) / pi2) * pi2;
    s->overlay[0] = (float) current;
    s->overlay[1] = (float) optimal;
    s->overlay[2] = (float) minimum;
    s->overlay[3] = (float) maximum;
    s->overlay_valid = 1;
}

// Accelerate (deob-10.0.3.js:18923-18940): crouching halves the wanted speed; add up to it along wishdir
static void srm_accelerate(Player *player, const double wishdir[3], double wishspeed, const double accel, const double dt_ms) {
    const MovementSchemeState *s = &player->scheme;
    wishspeed *= 1.0 - movement_js(player->crouch_val) / 2.0;
    const double add = wishspeed - (s->tmp_vel.x * wishdir[0] + s->tmp_vel.y * wishdir[1] + s->tmp_vel.z * wishdir[2]);
    double accel_speed = accel * dt_ms * wishspeed;
    if (player->is_you) {
        const double cmd[3] = {s->cmd_right, 0.0, s->cmd_forward};
        srm_overlay(player, cmd, wishdir, wishspeed, accel, dt_ms);
    }
    if (!(add <= 0.0)) {
        if (accel_speed > add) accel_speed = add;
        player->velocity.x += (float) (accel_speed * wishdir[0]);
        player->velocity.z += (float) (accel_speed * wishdir[2]);
    }
}

// Dash (deob-10.0.3.js:18839-18881): "dash" pushes along the move keys (the last tick's cmd; along the
// look without keys), "walldash" reflects tmpVel off the wall (x 0.7 from a ramp) and pushes along it,
// or straight off the wall when standing still against it without keys. The speed is at least the
// current one; velocity.y becomes the dash's height (a ramp walldash adds the ramp speed).
static void srm_dash(Player *player, const int wall, const int v10_dir, const Object *ramp, const int from_ramp, const double dt_ms, int *just_jumped) {
    MovementSchemeState *s = &player->scheme;
    const SrmDash *dash = wall ? &k_srm_walldash : &k_srm_dash;
    const double *normal = k_srm_wall_normal[player->on_wall >= 1 && player->on_wall <= 4 ? player->on_wall - 1 : 0];
    int still = 0;
    if (wall) {
        // reflect: v - n * 2 (v . n)
        const double dot = s->tmp_vel.x * normal[0] + s->tmp_vel.z * normal[2];
        const double rx = s->tmp_vel.x - normal[0] * dot * 2.0, rz = s->tmp_vel.z - normal[2] * dot * 2.0;
        still = fabsf(s->tmp_vel.x) <= 0.0075f && fabsf(s->tmp_vel.z) <= 0.0075f && v10_dir < 0;
        s->tmp_vel.x = (float) (rx * (from_ramp ? 0.7 : 1.0));
        s->tmp_vel.z = (float) (rz * (from_ramp ? 0.7 : 1.0));
    }
    s->wish_dash = 0.0f;
    if (wall) s->walldash_timer = (float) dash->cooldown;
    else s->dash_timer = (float) dash->cooldown;

    double dir[3];
    if (!wall) {
        if (v10_dir < 0 || (s->cmd_right == 0 && s->cmd_forward == 0)) {
            dir[0] = -sin(movement_js(player->direction.y)), dir[1] = 0.0, dir[2] = -cos(movement_js(player->direction.y));
        } else {
            const double angle = movement_dir_angle(player, v10_dir) - movement_js(player->direction.y);
            dir[0] = cos(angle), dir[1] = 0.0, dir[2] = sin(angle);
        }
    } else if (still) {
        dir[0] = normal[0], dir[1] = 0.0, dir[2] = normal[2];
    } else {
        dir[0] = s->tmp_vel.x, dir[1] = 0.0, dir[2] = s->tmp_vel.z;
    }
    const double len = sqrt(dir[0] * dir[0] + dir[2] * dir[2]);
    const double inv = 1.0 / (len ? len : 1.0); // normalize: a zero vector stays zero
    dir[0] *= inv, dir[2] *= inv;

    const double speed = MAX(dash->speed + dt_ms / (wall ? 25000.0 : 1500.0), srm_len_xz(s->tmp_vel));
    if (ramp || from_ramp) srm_auto_bhop(player, just_jumped);
    player->velocity.x = 0.0f;
    if (from_ramp && ramp) {
        // Intentional difference: v10 scales the ramp speed by iíïiïíí(player) / 45, the slope of the
        // *player*, which has no dir: undefined, so velocity.y became NaN (a falsy NaN then froze the
        // vertical integration). The ramp's slope is the evident intent and keeps the numbers finite.
        player->velocity.y = (float) (dash->height + fabs(srm_ramp_axis_speed(player, ramp)) * (srm_ramp_angle(ramp) / 45.0) * 0.8);
    } else {
        player->velocity.y = (float) dash->height;
    }
    player->velocity.z = 0.0f;
    srm_update_velocity(player);
    srm_accelerate(player, dir, speed, dash->acceleration, dt_ms);
}

// RampBounce (deob-10.0.3.js:18959-18978): landing on a ramp turns the fall into speed down the
// slope (tmpVel.y * min(1, slope / 45) * 0.75 along the ramp's dir); a held jump hops off it.
static void srm_ramp_bounce(Player *player, const Object *ramp, const double angle, const int jump, int *just_jumped) {
    const double bounce = player->scheme.tmp_vel.y * MIN(1.0, angle / 45.0) * 0.75;
    if (jump) srm_auto_bhop(player, just_jumped);
    switch (ramp->direction) {
        case 0: player->velocity.x += (float) bounce; break;
        case 1: player->velocity.z += (float) bounce; break;
        case 2: player->velocity.x -= (float) bounce; break;
        case 3: player->velocity.z -= (float) bounce; break;
        default: break;
    }
    srm_update_velocity(player);
}

// collidingRamp: map.manager.getGridArrays(x, y, z).find(o => o.ramp && player.collides(o)), the first
// ramp of the player's grid cell (map order) the player's box touches
static const Object *srm_colliding_ramp(const Player *player) {
    const Map *map = player->game->map;
    const int cell[3] = {
        (int) floorf(player->position.x / MOVEMENT_QUAD_SIZE),
        (int) floorf(player->position.y / MOVEMENT_QUAD_SIZE),
        (int) floorf(player->position.z / MOVEMENT_QUAD_SIZE),
    };
    for (size_t i = 0; map && i < map->object_count; i++) {
        const Object *object = map->objects[i];
        if (!object->ramp || object->complex_coll || object->collision_type == COLLISION_TYPE_NONE) continue;
        MovementBox box;
        movement_box(object, &box);
        if (movement_in_cell(cell, &box, object) && movement_collides(player, object, &box)) return object;
    }
    return NULL;
}

// SRM.mov (deob-10.0.3.js:18727-18838)
static void scheme_srm_mov(Player *player, const Input *raw_input, const int v10_dir, const double dt_ms) {
    MovementSchemeState *s = &player->scheme;
    const int raw_dir = movement_v10_dir(raw_input->move_dir); // SetMovementDir: input[4] unmasked
    int just_jumped = 0;

    if (player->on_ground) s->on_ground_ms += (float) dt_ms;
    else s->on_ground_ms = 0.0f;
    if (player->on_wall) s->on_wall_ms += (float) dt_ms;
    else s->on_wall_ms = 0.0f;

    const Object *ramp = player->on_ground ? srm_colliding_ramp(player) : NULL;
    if (s->on_wall_ms <= SRM_ON_WALL_MAX_MS) {
        // tmpVel.x || velocity.x: a wall contact (collision zeroed the velocity) keeps the speed it came with
        if (s->tmp_vel.x) player->velocity.x = s->tmp_vel.x;
        if (s->tmp_vel.z) player->velocity.z = s->tmp_vel.z;
    }
    const double angle = ramp ? srm_ramp_angle(ramp) : 0.0;
    const double min_slide = (double) (int32_t) (200.0 - angle * SRM_MIN_RAMP_SLIDE_VELOCITY * 1.25 * 25.0) / 1000.0; // (200 - a * ...) | 0
    const double flat_speed = srm_len_xz(s->tmp_vel);
    if (ramp && s->tmp_vel.y < 0.0f && player->on_ground && (min_slide * -0.5 / s->tmp_vel.y <= 0.1 || flat_speed < min_slide)) {
        srm_ramp_bounce(player, ramp, angle, raw_input->jump, &just_jumped);
    } else if (raw_input->jump && (!ramp || flat_speed < min_slide)) {
        // wishJump is cleared at the top of every tick, so only airTime, velocity.y and wishDash gate it
        if (player->air_time == 0.0f && player->velocity.y == 0.0f && !s->wish_dash) srm_auto_bhop(player, &just_jumped);
    } else if (player->on_ground && ramp && flat_speed >= min_slide) {
        if (s->wish_dash && s->walldash_timer <= 0.0f && player->on_wall) srm_dash(player, 1, v10_dir, ramp, 1, dt_ms, &just_jumped);
        srm_update_velocity(player);
    }

    if (raw_input->scope) s->wish_dash = SRM_WISH_DASH_MS;
    if (s->wish_dash > 0.0f) s->wish_dash = MAX(0.0f, s->wish_dash - (float) dt_ms);
    if (s->dash_timer > 0.0f) s->dash_timer = MAX(0.0f, s->dash_timer - (float) dt_ms);
    if (s->walldash_timer > 0.0f) s->walldash_timer = MAX(0.0f, s->walldash_timer - (float) dt_ms);

    if (s->dash_timer <= 0.0f && s->wish_dash && player->on_ground) {
        srm_dash(player, 0, v10_dir, ramp, 0, dt_ms, &just_jumped);
    } else if (s->walldash_timer <= 0.0f && s->wish_dash && !player->on_ground && player->on_wall) {
        srm_dash(player, 1, v10_dir, ramp, 0, dt_ms, &just_jumped);
    }
    if (just_jumped) return;

    // SetMovementDir (deob-10.0.3.js:18887-18902): input[4] 0..2 forward (-1), 4..6 back (+1); 2..4
    // right (+1), 6, 7, 0 left (-1)
    s->cmd_forward = (signed char) (raw_dir >= 0 && raw_dir <= 2 ? -1 : raw_dir >= 4 && raw_dir <= 6 ? 1 : 0);
    s->cmd_right = (signed char) (raw_dir >= 2 && raw_dir <= 4 ? 1 : raw_dir >= 6 || raw_dir == 0 ? -1 : 0);
    srm_update_velocity(player);

    // wishdir: (rightmove, 0, forwardmove) turned by xDire about y; nothing without movDir or keys.
    // v10 builds the matrix from the quaternion of Euler (0, xDire, 0) (iíîìïïi, deob-10.0.3.js:28407-28434):
    // m0 = m10 = 1 - 2 sin^2(a / 2), m8 = -m2 = 2 sin(a / 2) cos(a / 2). Kept in that form: a diagonal's
    // |wishdir|^2 is 2 only to rounding, and the strafe overlay's 2 - |wishdir|^2 turns on those bits.
    double wishdir[3] = {0.0, 0.0, 0.0};
    if (v10_dir >= 0 && (s->cmd_right || s->cmd_forward)) {
        const double half = movement_js(player->direction.y) / 2.0;
        const double sy = sin(half), y2 = sy + sy, yy = sy * y2, wy = cos(half) * y2;
        wishdir[0] = (1.0 - yy) * s->cmd_right + wy * s->cmd_forward;
        wishdir[2] = -wy * s->cmd_right + (1.0 - yy) * s->cmd_forward;
    }
    const double friction_on = s->on_ground_ms <= SRM_FRICTION_DELAY_MS ? 0.0 : 1.0;
    if (player->on_ground) {
        // ApplyFriction (deob-10.0.3.js:18903-18922)
        const double speed = srm_len_xz(s->tmp_vel);
        const double drop = (speed < SRM_GROUND_DECEL ? SRM_GROUND_DECEL : speed) * SRM_FRICTION * dt_ms * friction_on;
        double keep = MAX(0.0, speed - drop);
        if (speed > 0.0) keep /= speed;
        player->velocity.x = (float) (player->velocity.x * keep);
        player->velocity.z = (float) (player->velocity.z * keep);
        if (friction_on == 0.0) return;
        // GroundMove (deob-10.0.3.js:18941-18946): normalized wishdir at baseSpeed + delta / 1000
        const double len = sqrt(wishdir[0] * wishdir[0] + wishdir[2] * wishdir[2]);
        const double inv = 1.0 / (len ? len : 1.0);
        wishdir[0] *= inv, wishdir[2] *= inv;
        srm_accelerate(player, wishdir, sqrt(wishdir[0] * wishdir[0] + wishdir[2] * wishdir[2]) * (SRM_BASE_SPEED + dt_ms / 1000.0), SRM_GROUND_ACCEL, dt_ms);
    } else {
        // AirMove (deob-10.0.3.js:18947-18958): the raw wishdir (a diagonal is sqrt 2 long)
        double wishspeed = sqrt(wishdir[0] * wishdir[0] + wishdir[2] * wishdir[2]) * SRM_BASE_SPEED;
        double accel = s->tmp_vel.x * wishdir[0] + s->tmp_vel.y * wishdir[1] + s->tmp_vel.z * wishdir[2] < 0.0 ? SRM_AIR_DECEL : SRM_AIR_ACCEL;
        if (s->cmd_forward == 0 && s->cmd_right != 0) {
            if (wishspeed > SRM_SIDE_STRAFE_SPEED) wishspeed = SRM_SIDE_STRAFE_SPEED;
            accel = SRM_SIDE_STRAFE_ACCEL;
        }
        srm_accelerate(player, wishdir, wishspeed, accel, dt_ms);
    }
    srm_update_velocity(player);
}

// ---- Vehicle.mov (deob-10.0.3.js:19142-19167): forwardV - reverseV pushes along the heading (accel
// * 120 per unit), x / z drift with vhdr per 60 fps frame (the exact integral, x speedX / speedZ),
// and angVel turns the heading with vhaDr. The asset model's roll (meshRef, angVel * vhtL) is the
// client's (a class "Model" asset; KrunkNative does not draw class assets yet).
static void scheme_vehicle_mov(Player *player, const float accel, const double dt_ms) {
    const ClassOverrides *vh = class_overrides(movement_class(player));
    const ClassMovement *knobs = movement_knobs(player);
    MovementSchemeState *s = &player->scheme;
    const double frames = dt_ms / (1000.0 / 60.0);
    const double drag = movement_js(vh->vehicle_drag), angular_drag = movement_js(vh->vehicle_angular_drag);
    const double push = (double) accel * 120.0 * ((double) s->forward_v - s->reverse_v);
    const double yaw = movement_js(player->direction.y);
    const double vx = player->velocity.x - sin(yaw) * push;
    const double vz = player->velocity.z - cos(yaw) * push;
    const double nvx = vx * pow(drag, frames), nvz = vz * pow(drag, frames);
    player->position.x += (float) ((nvx - vx) / log(drag) * movement_js(MOVEMENT_OR(knobs->speed_x, 1.0f)));
    player->position.z += (float) ((nvz - vz) / log(drag) * movement_js(MOVEMENT_OR(knobs->speed_z, 1.0f)));
    player->velocity.x = (float) nvx;
    player->velocity.z = (float) nvz;
    const double ang = (double) s->ang_vel * pow(angular_drag, frames);
    player->direction.y = (float) fmod(yaw + (ang - s->ang_vel) / log(angular_drag), M_PI * 2.0);
    s->ang_vel = (float) ang;
}

// movement[movSch].mov || movement[0].mov (deob-10.0.3.js:191716-191722)
static void movement_scheme_mov(Player *player, const Input *raw_input, const Input *input, const float accel, const double dt_ms) {
    const int v10_dir = movement_v10_dir(input->move_dir); // movDir after KrunkScript's mask
    switch (player_movement_scheme(player)) {
        case MOVEMENT_SCHEME_FOLLOW_MOUSE: {
            // Follow Mouse.mov (deob-10.0.3.js:18651-18656): always forward along the look, keys unused
            const double angle = -movement_js(player->direction.y) - M_PI / 2.0;
            player->velocity.x += (float) (accel * cos(angle));
            player->velocity.z += (float) (accel * sin(angle));
            break;
        }
        case MOVEMENT_SCHEME_MOVE_RIGHT:
            // Move Right.mov (deob-10.0.3.js:18658-18662): velocity (0.01, -, 0) every tick
            player->velocity.x = 0.01f;
            player->velocity.z = 0.0f;
            break;
        case MOVEMENT_SCHEME_FLY: {
            // Fly.mov (deob-10.0.3.js:18664-18671): always along the look, the pitch lifting it
            const double yaw = movement_js(player->direction.y) + M_PI, pitch = movement_js(player->direction.x);
            player->velocity.x += (float) (accel * sin(yaw) * cos(pitch));
            player->velocity.z += (float) (accel * cos(yaw) * cos(pitch));
            player->velocity.y += (float) (accel * sin(pitch));
            break;
        }
        case MOVEMENT_SCHEME_DIR_ONLY: scheme_dir_only_mov(player, accel, v10_dir); break;
        case MOVEMENT_SCHEME_SRM: scheme_srm_mov(player, raw_input, v10_dir, dt_ms); break;
        case MOVEMENT_SCHEME_VEHICLE: scheme_vehicle_mov(player, accel, dt_ms); break;
        case MOVEMENT_SCHEME_MMORPG: scheme_mmorpg_mov(player, input, accel, v10_dir); break;
        default: scheme_default_mov(player, accel, v10_dir); break;
    }
}

// Vehicle.vel (deob-10.0.3.js:19168-19175): only y moves here (Vehicle.mov moved x / z), with the
// 0.9993 per ms decay integrated exactly; gravity still pulls it in procInputs.
static void scheme_vehicle_vel(Player *player, const double dt_ms) {
    if (!player->velocity.y) return;
    const double decay = 0.9993;
    const double v = player->velocity.y * pow(decay, dt_ms);
    player->position.y += (float) ((v - player->velocity.y) / log(decay) * movement_js(MOVEMENT_OR(movement_knobs(player)->speed_y, 1.0f)));
    player->velocity.y = (float) v;
}

// SYNC_PLAYER's sendVals (deob-10.0.3.js:222398-222403): the scheme's state after the acked input, in
// sendVals order; the owner writes them back before its replay (:258289-258295). 0 = the scheme has none.
int player_movement_send_vals(const Player *player, float vals[PLAYER_MOVE_VALS_MAX]) {
    const MovementSchemeState *s = &player->scheme;
    switch (player_movement_scheme(player)) {
        case MOVEMENT_SCHEME_SRM: // onGroundFrames, onWallFrames, wishDash, dashTimer, walldashTimer, tmpVel
            vals[0] = s->on_ground_ms, vals[1] = s->on_wall_ms, vals[2] = s->wish_dash, vals[3] = s->dash_timer;
            vals[4] = s->walldash_timer, vals[5] = s->tmp_vel.x, vals[6] = s->tmp_vel.y, vals[7] = s->tmp_vel.z;
            return 8;
        case MOVEMENT_SCHEME_VEHICLE: // forwardV, reverseV, angVel
            vals[0] = s->forward_v, vals[1] = s->reverse_v, vals[2] = s->ang_vel;
            return 3;
        default:
            return 0;
    }
}

void player_movement_apply_vals(Player *player, const int scheme, const float vals[PLAYER_MOVE_VALS_MAX], const int count) {
    if (scheme != (int) player_movement_scheme(player)) return; // a class change raced the sync
    for (int i = 0; i < count; i++) if (!isfinite(vals[i])) return;
    MovementSchemeState *s = &player->scheme;
    if (scheme == MOVEMENT_SCHEME_SRM && count == 8) {
        s->on_ground_ms = vals[0], s->on_wall_ms = vals[1], s->wish_dash = vals[2], s->dash_timer = vals[3];
        s->walldash_timer = vals[4], s->tmp_vel = (vec3) {vals[5], vals[6], vals[7]};
    } else if (scheme == MOVEMENT_SCHEME_VEHICLE && count == 3) {
        s->forward_v = vals[0], s->reverse_v = vals[1], s->ang_vel = vals[2];
    }
}

// v10.0.3 gameLoop, a frame that samples no input frame (netRate): `if ((tmpPlayer.config.movSch || 0)
// == 0 && tmpPlayer.config.trnSp == 1) { yDire = CONTROLS.yDr; xDire = cameraYaw; updateMsh }`
// (deob-10.0.3.js:260853-260857). Other schemes and turn speeds keep the look of the last input frame.
int player_look_follows_camera(const Player *player) {
    return player_movement_scheme(player) == MOVEMENT_SCHEME_DEFAULT && MOVEMENT_OR(movement_knobs(player)->turn_rate, 1.0f) == 1.0f;
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

    // v10: Math.max(1 * config.deltaMlt, Math.min(input[1], dltMx)); NaN, 0 and negative deltas run as
    // deltaMlt ms (v10:169582; v7.1.4: 0.1 ms, v3.1.1: 0). input[1] already carries deltaMlt (main.c).
    // Krunker Strike keeps v7's unscaled 0.1 ms floor (Ruleset.delta_floor_scaled, strike_rules.h).
    const Ruleset *rules = game_ruleset(player->game);
    const GameConstants *k = game_constants_of(player->game);
    const float delta_mlt = player->game && player->game->config.delta_mlt > 0.0f ? player->game->config.delta_mlt : 1.0f;
    const float min_delta = k->min_delta * (rules->delta_floor_scaled ? delta_mlt : 1.0f);
    const float delta = input->delta > min_delta ? MIN(input->delta, k->max_delta) : min_delta;
    // config.movDirs[input[4]] + class movDrP * PI
    const float move_dir = -(float) M_PI / 2.0f + (float) M_PI / 4.0f * (float) input->move_dir + knobs->move_dir_offset * (float) M_PI;

    if (player->noclip) player->on_ground = 1;
    // A flip off leaves that forced onGround behind: no collision test has run since, so the jump
    // gate below must not read it as ground (KrunkNative; v10 jumped on that stale ground).
    const int left_noclip = player->noclip_prev && !player->noclip;
    player->noclip_prev = player->noclip;

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

    // the movement scheme's dir, or Default's (deob-10.0.3.js:191385-191391); KrunkScript
    // defaultRotation = false: the script turns the player
    if (ks_player_allows(player, KS_PLAYER_DEFAULT_ROTATION)) movement_scheme_dir(player, input, raw_input->move_dir, input->move_dir, delta);

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
        // v10.0.3: also !movement[movSch].noThrow (SRM: the aim key dashes instead, deob-10.0.3.js:191450)
        const int can_scope = player->reload_timer <= 0.0f && player->swap_timer <= 0.0f && (!player->weapon->melee || (player->can_throw && player->game->config.throwable_melees)) &&
                              !movement_scheme_no_throw(player);

        if (input->scope && player->aim_val < 1.0f && can_scope) {
            player_cancel_inspect(player);
            player->aim_dir = 0; // aimDir = 0 (deob-10.0.3.js:191453)
            player->aim_val += 1.0f / player->weapon->aim_speed * delta;
            player->aim_val = MIN(1.0f, player->aim_val);

            if (player->aim_val == 1.0f && !recon) {
                // TODO: toggle aim (HUD)
            }
        } else if (!can_scope || !input->scope && player->aim_val > 0.0f) {
            player->aim_dir = 1; // aimDir = 1 (deob-10.0.3.js:191465)
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

    // v10.0.2 Stalker leap (v10:169664-169685): the aim input (input[6]) while grounded, stuck to a
    // wall or within the coyote time, 3 s apart: 0.15 along the look direction. Not in replays: the
    // client keeps it as the input's velObj like a shot's knockback.
    if (!recon && movement_stalker(player) && input->scope &&
        (player->on_ground || player->stuck_to_wall || player->stalker_coyote_time > 0.0f) &&
        player->game->now - player->last_stalker_leap >= 3.0f) {
        player->last_stalker_leap = player->game->now;
        player->did_jump = 1;
        player->did_wall_jump = 1;
        player->did_stick_to_wall = 1;
        player->stuck_to_wall = 0;
        player->stalker_coyote_time = 0.0f;
        const float power = 0.15f, yaw = player->direction.y, pitch = player->direction.x;
        const vec3 leap = {power * sinf(yaw + (float) M_PI) * cosf(pitch), power * sinf(pitch), power * cosf(yaw + (float) M_PI) * cosf(pitch)};
        player->velocity.x += leap.x;
        player->velocity.z += leap.z;
        player->velocity.y += leap.y;
        player->on_ground = 0;
        if (!game_is_authority(player->game)) {
            player->shot_vel_obj = (vec3) {-leap.x, -leap.y, -leap.z};
            player->has_shot_vel_obj = 1;
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
        // `mode.groundAccel ?? 1` on the ground and in the air (deob-10.0.3.js:191624; Chaos Snipers / Impulsed 2)
        const float ground_accel_mlt = MOVEMENT_OR(mode_movement->ground_accel, 1.0f);
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
        else decel = MOVEMENT_OR(movement_scheme_air_decel(player), MOVEMENT_OR(knobs->air_decel, game_constants.air_decel)); // scheme airDecel first (:191631)

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
                const int v7_dir = movement_v10_dir(raw_input->move_dir); // input[4] as sent (deob-10.0.3.js:191663-191665)
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
            // KrunkNative: noclip is flight and never jumps. Its forced onGround made every held or
            // toggled jump a pop (auto bhop toggles the key each frame, and tryJump re-arms on each
            // release), so with the flag on the impulses stacked, one per tick with autoJump. v10
            // pops once per press here; this is a deliberate difference. A release still re-arms
            // did_jump, as tryJump does.
            if (!player->noclip) {
                if (!left_noclip) movement_try_jump(player, input->jump, recon);
            } else if (!input->jump) {
                player->did_jump = 0;
            }
        }

        if (!contact && !player->stuck_to_wall) { // v10: no gravity while stuck to a wall (v10:169754)
            const float wall_power = movement_wall_jump_power(player);
            const float wall_decel = mode_movement->wall_jump_decel ? mode_movement->wall_jump_decel : MOVEMENT_OR(knobs->wall_jump_decel, 0.3f);
            const float wall_mlt = player->velocity.y < 0.0f && player->wall_jump && player->on_wall && wall_power > 0.0f && player->crouch_val ? wall_decel : 1.0f;
            // config.gravMlt x (weapon.gravMlt || 1) x (mode.gravMlt ?? 1) (deob-10.0.3.js:191697; Boulder 1.7,
            // Chaos Snipers 0.92, Impulsed 1.05)
            const float weapon_grav = player->weapon && player->weapon->gravity_mlt > 0.0f ? player->weapon->gravity_mlt : 1.0f;
            player->velocity.y -= delta * (game_constants.gravity * player->game->config.gravity_mlt * weapon_grav * MOVEMENT_OR(mode_movement->gravity_mlt, 1.0f)) * wall_mlt;
        }
        if (player->stuck_to_wall) player->velocity = (vec3) {0.0f, 0.0f, 0.0f}; // v10:169755

        // 95.js: ac *= limitMM(1 - a0[12], 0, 1) (input[12]: the controller's Gradual Speed)
        accel *= CLAMP(1.0f - input->slowdown, 0.0f, 1.0f);
        if (player->noclip) accel *= 2.0f;
        if (player->speed_boost) accel *= player->speed_boost; // speedBoost && (ac *= speedBoost): official lobbies' "spb"
        accel *= player_attr_speed(player);
        accel *= player_attr_rooted(player);
        accel *= drank_puddle_slow(player->game, player); // KrunkNative Drank puddles (shared/include/drank.h)
        accel *= mode_speed_mlt(player->game, player); // Kranked bonuses.speed (player.js:1236, WP-D)
        accel *= player_has_perk(player, PERK_SPEED) ? 1.15f : 1.0f;

        // the movement scheme's mov, or Default's (deob-10.0.3.js:191716-191722): defaultMovement, not
        // while stuck to a wall
        if (ks_player_allows(player, KS_PLAYER_DEFAULT_MOVEMENT) && !player->stuck_to_wall) movement_scheme_mov(player, raw_input, input, accel, dt_ms);

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
            (((raw_input->move_dir == 2 || raw_input->move_dir == 1) && yaw_delta > 0.0f) || ((raw_input->move_dir == 6 || raw_input->move_dir == 7) && yaw_delta < 0.0f))) { // input[4] (deob-10.0.3.js:191736)
            const float vel = hypotf(player->velocity.x, player->velocity.z);
            const float dir = atan2f(-player->velocity.z, -player->velocity.x) + yaw_delta;

            player->velocity.x = vel * cosf(dir + (float) M_PI);
            player->velocity.z = vel * sinf(dir + (float) M_PI);
        }

        // the movement scheme's vel, or Default's (deob-10.0.3.js:191741-191747; Default.vel :18606-18648)
        const float speed_x = MOVEMENT_OR(knobs->speed_x, 1.0f), speed_y = MOVEMENT_OR(knobs->speed_y, 1.0f), speed_z = MOVEMENT_OR(knobs->speed_z, 1.0f);
        if (!ks_player_allows(player, KS_PLAYER_DEFAULT_VELOCITY)) {
            // KrunkScript defaultVelocity = false: no integration or friction (player.js:1264)
        } else if (player_movement_scheme(player) == MOVEMENT_SCHEME_VEHICLE) {
            scheme_vehicle_vel(player, dt_ms);
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

        // v10 noMovT / isHidden, outside replays (deob-10.0.3.js:191766-191779; shared/src/stalker.c)
        if (!recon) stalker_update_hidden(player, delta);

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
        // v10 stalkerCoyoteTime (v10:170078): 150 ms to leap after leaving the ground or a wall
        if (movement_stalker(player)) {
            if (player->on_ground || player->stuck_to_wall) player->stalker_coyote_time = 0.15f;
            else if (player->stalker_coyote_time > 0.0f) player->stalker_coyote_time = MAX(0.0f, player->stalker_coyote_time - delta);
        }
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
            // v10.0.2 rShoot (Compressor): the aim input fires too (v10:170122 `lMouse || weapon.rShoot && rMouse`)
            const int fire = input->shoot || (player->weapon->r_shoot && input->scope);
            int will_shoot = player->burst_count || !player->weapon->no_auto && fire;

            if (player->did_shoot && !fire) {
                player->did_shoot = 0;
            }

            if (!player->did_shoot && fire) {
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
                        player->shot_aim = input->scope; // v10 shoot(..., rMouse) (v10:170139)
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

    // v10.0.2 Stalker wall stick (v10:170153-170154): the jump key against a wall in the air sticks
    // the stalker there (no gravity, no movement); the next jump press lets go with 0.03 off the wall.
    if (!move_lock && movement_stalker(player)) {
        if (player->on_wall && !player->on_ground) {
            if (player->did_stick_to_wall && !input->jump) player->did_stick_to_wall = 0;
            if (!player->did_stick_to_wall && input->jump && !player->stuck_to_wall) {
                player->did_stick_to_wall = 1;
                player->stuck_to_wall = 1;
                player->stuck_to_wall_dir = (unsigned char) player->on_wall;
            }
        }
        if (player->stuck_to_wall && input->jump && !player->did_stick_to_wall) {
            player->did_stick_to_wall = 1;
            player->stuck_to_wall = 0;
            if (player->stuck_to_wall_dir == 1) player->velocity.x = 0.03f;
            else if (player->stuck_to_wall_dir == 2) player->velocity.x = -0.03f;
            else if (player->stuck_to_wall_dir == 3) player->velocity.z = 0.03f;
            else if (player->stuck_to_wall_dir == 4) player->velocity.z = -0.03f;
            player->stuck_to_wall_dir = 0;
        }
    }

    // Wall jump: at most wallJumpMax per airborne period (v3.1.1: unlimited); the class / mode
    // wallJV, wallJYV and config.wallJP shape it. v10: not while stuck to a wall (v10:170155).
    // v10.0.3: not under a noWallJ movement scheme (SRM, deob-10.0.3.js:192436)
    if (!move_lock && !movement_scheme_no_wall_jump(player) && player->wall_jump && player->wall_jump_count < player->wall_jump_max && !player->on_ground && !player->on_ladder &&
        player->on_wall && !player->stuck_to_wall && movement_wall_jump_power(player) > 0.0f) {
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
static float combat_adjusted_height(const Player *player);

void player_hit_zone_base(const Player *target, const PlayerStoredState *state, float *base_y, float *adjusted_height) {
    if (game_ruleset(target->game)->hit_zones_live || !state) { // v7 / Strike: d.obj.y + d.obj.adjustedHeight
        *base_y = target->position.y;
        *adjusted_height = combat_adjusted_height(target);
        return;
    }
    *base_y = state->position.y;
    *adjusted_height = state->adjusted_height;
}

static float combat_adjusted_height(const Player *player) {
    // headMlt: the spawn value, or the Bighead FFA one (mode.head_mlt, WP-D); only one is ever set
    const float head_mlt = (player->head_mlt > 0.0f ? player->head_mlt : 1.0f) * mode_head_mlt(player);
    return player->height - COMBAT_HEAD_SCALE + COMBAT_HEAD_SCALE * head_mlt;
}

// The v10.0.2 pose fields of a live player (players.storeState, v10:170708-170722): the hit boxes
// (shared/src/hitboxes.c) need them, so every state carries them. v7 ignores them.
static void combat_store_pose(PlayerStoredState *out, const Player *player) {
    const int index = player->loadout_index;
    out->step_val = player->step_val;
    out->step_chase = player->step_chase;
    out->x_dir = player->direction.y; // xD: the yaw (KrunkNative direction.y)
    out->y_dir = player->direction.x; // yD: the pitch (direction.x)
    out->crouch_val = roundf(player->crouch_val); // v10 rounds it (v10:170718)
    out->weapon_index = player->loadout && index >= 0 && index < player->loadout_size ? player->loadout[index] : -2;
}

// players.js storeState: unshift {time: game.now, h, ah, x, y, z}, then drop every entry at least
// config.stateHistory old (they are the oldest ones: time only grows).
void player_store_state(Player *player) {
    const Game *game = player->game;

    player->state_history_head = (player->state_history_head + 1) % PLAYER_STATE_HISTORY_MAX;
    player->state_history[player->state_history_head] = (PlayerStoredState) {game->state_now, player->position, player->height,
                                                                             combat_adjusted_height(player)};
    combat_store_pose(&player->state_history[player->state_history_head], player);
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
        combat_store_pose(out, target);
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
    int zone;           // v10 hit boxes: 1 + HitKind of the box hit; 0 = the v7 zones by height
    GameAIHitPart part; // v10.0.3 preciseHits: the AI model part hit (has_part 0 = its box)
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
    const int posed = hitboxes_active(game); // v10 (not Prop Hunt): posed boxes instead of the padded AABB

    for (size_t i = 0; i < game->player_count; i++) {
        Player *other = game->players[i];
        if (!other || !other->active || other == shooter || !combat_can_damage(shooter, other)) continue;

        // G = fetchState(list[i], player.getStateConst()): the box where the shooter saw it
        PlayerStoredState state;
        if (!players_fetch_state(other, state_const, &state)) continue;

        if (posed) { // v10.0.2: the posed hit boxes (shared/src/hitboxes.c), the zone from the box hit
            float t;
            int kind;
            if (!hitboxes_trace_player(other, &state, origin, dir, 1, &t, &kind)) continue;
            hit_list_push(hits, (Hit) {NULL, other, t, NULL, state, 1 + kind});
            continue;
        }

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
        // v10.0.3: usesHitTest() ? hitTest (the posed model, no box) : the box (deob-10.0.3.js:196017, :196677)
        GameAIHitPart part;
        const float t = game_ai_ray_hit(game, ai, origin, dir, &part);
        if (t <= 0.0f || t > 1.0f) continue;
        Hit hit = {NULL, NULL, t, ai};
        hit.part = part;
        hit_list_push(hits, hit);
    }
}

// The face of an axis-aligned box (lo / hi corners) nearest to `p`, as its outward normal.
static vec3 ray_box_normal(const vec3 p, const vec3 lo, const vec3 hi) {
    const float dist[6] = {fabsf(p.x - lo.x), fabsf(p.x - hi.x), fabsf(p.y - lo.y), fabsf(p.y - hi.y), fabsf(p.z - lo.z), fabsf(p.z - hi.z)};
    int face = 0;
    for (int i = 1; i < 6; i++)
        if (dist[i] < dist[face]) face = i;
    vec3 n = {0.0f, 0.0f, 0.0f};
    const float sign = face & 1 ? 1.0f : -1.0f;
    if (face < 2) n.x = sign;
    else if (face < 4) n.y = sign;
    else n.z = sign;
    return n;
}

int game_raycast(Game *game, const vec3 origin, vec3 dir, const float range, const Player *exclude, const int flags, GameRayHit *out) {
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    out->object_index = -1;
    if (!game || !game->map || !(range > 0.0f)) return 0;
    const float len = sqrtf(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    if (!(len > 0.0f)) return 0;
    dir = (vec3) {dir.x / len, dir.y / len, dir.z / len};
    const vec3 seg = {dir.x * range, dir.y * range, dir.z * range}; // line_in_rect fractions along the whole ray

    float best = 2.0f; // fraction of the ray; > 1 = none
    GameRayKind kind = GAME_RAY_NONE;
    int object_index = -1;
    const Object *object_hit = NULL;
    const Player *player_hit = NULL;

    for (size_t i = 0; i < game->map->object_count; i++) {
        const Object *object = game->map->objects[i];
        if (!game_object_shootable(object)) continue;
        if (object->dyn && !(flags & GAME_RAY_DYNAMIC)) continue;
        const float t = line_in_rect(origin, seg, object->position, object_box_size(object));
        if (t <= 0.0f || t > 1.0f || t >= best) continue;
        best = t;
        kind = GAME_RAY_OBJECT;
        object_index = (int) i;
        object_hit = object;
        player_hit = NULL;
    }

    if (flags & GAME_RAY_PLAYERS) {
        for (size_t i = 0; i < game->player_count; i++) {
            const Player *other = game->players[i];
            if (!other || other == exclude || !other->active || !(other->health > 0.0f)) continue;
            const float half = other->scale > 0.0f ? other->scale : 2.45f; // v10 player.scale || 2.45
            const float height = other->height > 0.0f ? other->height : 11.0f; // v10 player.height || 11
            const vec3 size = {half * 2.0f, height, half * 2.0f};
            const float t = line_in_rect(origin, seg, other->position, size);
            if (t <= 0.0f || t > 1.0f || t >= best) continue;
            best = t;
            kind = GAME_RAY_PLAYER;
            object_index = -1;
            object_hit = NULL;
            player_hit = other;
        }
    }

    float terrain_dist = 0.0f;
    if ((flags & GAME_RAY_TERRAIN) && terrain_raycast(game, origin, dir, range, &terrain_dist) && terrain_dist < best * range) {
        best = terrain_dist / range;
        kind = GAME_RAY_TERRAIN_HIT;
        object_index = -1;
        object_hit = NULL;
        player_hit = NULL;
    }

    if (kind == GAME_RAY_NONE) return 0;
    out->kind = kind;
    out->dist = best * range;
    out->point = (vec3) {origin.x + dir.x * out->dist, origin.y + dir.y * out->dist, origin.z + dir.z * out->dist};
    out->object_index = object_index;
    out->object = object_hit;
    out->player = player_hit;
    if (kind == GAME_RAY_TERRAIN_HIT) {
        out->normal = (vec3) {0.0f, 1.0f, 0.0f};
    } else if (kind == GAME_RAY_OBJECT) {
        const vec3 size = object_box_size(object_hit);
        const vec3 lo = {object_hit->position.x - size.x * 0.5f, object_hit->position.y, object_hit->position.z - size.z * 0.5f};
        const vec3 hi = {object_hit->position.x + size.x * 0.5f, object_hit->position.y + size.y, object_hit->position.z + size.z * 0.5f};
        out->normal = ray_box_normal(out->point, lo, hi);
    } else {
        const float half = player_hit->scale > 0.0f ? player_hit->scale : 2.45f;
        const float height = player_hit->height > 0.0f ? player_hit->height : 11.0f;
        const vec3 p = player_hit->position;
        const vec3 lo = {p.x - half, p.y, p.z - half}, hi = {p.x + half, p.y + height, p.z + half};
        out->normal = ray_box_normal(out->point, lo, hi);
    }
    return 1;
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

    // a weaponOrder mode (Gun Game), not a streak kill: Humiliation (melee) / Promotion, medals without a score
    // (deob-10.0.3.js:192525-192532)
    if (game->mode && game->mode->config.weapon_order_size && !(info && info->streak > 0))
        combat_medal(medals, weapon && weapon->melee ? "Humiliation" : "Promotion", 0);
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
        // `!scope || aimVal == 0 && aimTime > 180 || aimDir == 1` (deob-10.0.3.js:192570): letting go of the aim is no Quick Scope
        else if (!scope || (killer->aim_val == 1.0f && killer->aim_time > 0.18f) || killer->aim_dir == 1) score += combat_medal(medals, "", 50);
        else score += combat_medal(medals, "Quick Scope", 75);

        if (info && info->headshot) score += combat_medal(medals, "Headshot", 50);
        if (info && info->wallbang) score += combat_medal(medals, "Wallbang", 25);
        // dst * range > 200, scoped weapons too (deob-10.0.3.js:192586)
        if (weapon && info && info->distance * weapon->range > 200.0f) score += combat_medal(medals, "Longshot", 25);
    }
    if (victim->mode.flag) score += combat_medal(medals, "Snatched", 50); // the victim carried a flag (deob-10.0.3.js:192591-192594)

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
    if (mode_double_xp(game)) amount *= 2; // v10.0.3 isDoubleXP (aon / md in a pub-type game, deob-10.0.3.js:197855)
    if (amount > 0 && zombies_double_points(game)) amount *= 2; // WP-H: doublePointsTimer
    const int gain = mode_score_gain(game, amount, is_kill); // noScoreK: kill rewards add nothing
    player->score += gain;
    if (player->active) player->life_score += gain; // WP-E: scoreL (the death screen's "Score")
    const int cap = mode_score_cap(game, player); // pub-type games, not a dev, not noScrLimit (deob-10.0.3.js:197869)
    if (cap && player->score > cap) {
        player->score = cap;
        amount = 0;
    }

    mode_on_score(game, player, amount, is_kill); // objectiveScore + team score (players.teamScore)
    // WP-E part 2c: challenges.update(game, "score", amt) with the amount after the cap (a capped score adds 0;
    // a noScoreK kill still adds its amount: deob-10.0.3.js:197903-197905)
    if (amount) game_challenge(game, player, "score", amount, -1, -1);

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
        game_send_hit(game, dealer->uid, &hit, damage);
    }

    if (damage > 0.0f) victim->hit_timer = GAME_REGEN_DELAY;
    // v10.0.3 changeHealth: runDmgTaken += the damage, capped at the health left, unless in god mode
    // (deob-10.0.3.js:198005-198007; Raid's "rnodmg")
    if (damage > 0.0f && !victim->god_mode) mode_run_damage(game, victim, damage > victim->health ? victim->health : damage);

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

    // v10.0.3 players.kill (deob-10.0.3.js:198485-198496): with botsAsAI (Raid) a bot's kill has no killer and a
    // bot's death no kill-feed row; noKillFeed (Traitor) hides every row ("3" hideFeed)
    const int bots_as_ai = game->mode && game->mode->config.bots_as_ai;
    if (killer && killer->is_bot && bots_as_ai) killer = NULL;
    const int hide_feed = (player->is_bot && bots_as_ai) || (game->mode && game->mode->config.no_kill_feed);
    // deaths, deathsL and the "d" challenge only once the match runs: `!waitTimers || !waitTimers[0] ||
    // waitTimers[0].contTime` (deob-10.0.3.js:198511)
    const int counts_death = !mode_waiting(game);

    player->active = 0;
    if (counts_death) player->deaths++;
    player->last_killer_uid = -1;
    player->death_time = game->now;
    player->input_queue_size = 0;
    free(player->input_queue);
    player->input_queue = NULL;

    const int streaks = !(game->mode && game->mode->config.no_streaks) && !game->config.no_streaks;
    if (streaks) player->death_streak++;

    game->kill_count++;
    if (counts_death) game_challenge(game, player, "d", 1, -1, -1); // WP-E part 2c: challenges.update(game, "d", 1)
    if (game->hooks.kill) game->hooks.kill(game, player, killer); // v10 territorialWars.onPlayerDeath
    bot_chat_on_kill(game, player, killer); // KrunkNative bot chat: the odd kill / death comment (shared/src/bot_chat.c)

    const int weapon_index = killer ? (kill_info && kill_info->weapon_id >= 0 ? kill_info->weapon_id : combat_weapon_index(killer)) : -1;
    int reward = 0;
    int streak_message = 0; // the "kst" streak (killStreak % 5 == 0), 0 = none

    if (killer && killer != player) {
        player->last_killer_uid = killer->uid;
        killer->kills++;
        if (killer->active) killer->life_kills++; // WP-E: killsL

        if (streaks && !rules_ranked_no_streaks(game)) { // v10: no kill streaks in ranked (v10:174076)
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
        // info.stk: NET_KILL_NUKE for a Nuke kill (every patch 2 peer), the streak bits for any streak's
        // (the kill feed's img/streaks/<stk>.png: the Bombardment's bombs, stk 3)
        .fists = (uint8_t) ((kill_info && kill_info->fists ? NET_KILL_FISTS : 0) | (kill_info && kill_info->streak == STREAK_NUKE + 1 ? NET_KILL_NUKE : 0) |
                            (kill_info && kill_info->streak > 0 && kill_info->streak < 16 ? kill_info->streak << NET_KILL_STREAK_SHIFT : 0) |
                            (hide_feed ? NET_KILL_HIDE_FEED : 0)),
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
    player->fire_count++; // players.js melee: input.syncTime = true
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
    player_play_clip(player, NET_PLAYER_CLIP_MELEE); // playClip(player, "Melee", 1) (deob-10.0.3.js:195909; authority only)

    // TODO: gamepad rumble?
    // isYou: weapon.anim(player, TWEEN, canThrow ? (throw ? 3 : 1) : 2, d), cancelInspect (v10.js:172423;
    // swsh sounds: sound_events.c)
    if (player->is_you) {
        if (player->weapon->melee_anim) player->weapon->melee_anim(player, player->can_throw ? (is_throw ? 3 : 1) : 2, throw_d);
        player_cancel_inspect(player);
    }

    if (is_throw) {
        // projectiles.init(stored[0]) with wId 12: the knife flies, Bullseye on a kill.
        player->can_throw = !!player->unlimited_ammo;
        player->last_throw_time = player->game->now; // lastThrowTime (deob-10.0.3.js:195939): the Refill streak's wait
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
        // G.ai: !waitTimers; v10.0.3 part ? dmg * part.m, part.m 0 = armourHit (deob-10.0.3.js:196062-196079)
        if (hits.items[i].ai && !mode_waiting(game))
            game_ai_damage_part(game, hits.items[i].ai, player, player->smite ? 10000.0f : damage, 0, NAN, -1, 0, &hits.items[i].part);
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
        // the "9" trail of this shot (every ray carries it): 2 for a full-charge Charge Rifle, 1 for the Sniper Rifle, else 0
        .trail = weapon_shot_trail(player->weapon, player->shot_blast),
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
    // v10.0.2 noDmg (Compressor): the rays trace nothing and the shot is no "s" stat (v10:172690, 172853)
    const int no_damage = weapon->no_dmg;
    if (authority && ray >= 0 && player->active) { // WP-E: totShotsL (the death screen's "Shots Fired")
        player->life_shots++;
        player->match_shots++;
    }
    if (authority && ray >= 0 && !no_damage) game_inc_stat(game, player, "s"); // WP-E part 2c: incStat("s")

    const vec3 origin = {player->position.x, shot_height, player->position.z};
    const vec3 dir = combat_shot_vector(range, pitch, yaw);
    HitList hits = {0};

    // knockback: config.impulseMlt x (mode.impulseMlt != null ? mode.impulseMlt : 1) (deob-10.0.3.js:196585 the shooter,
    // :196732 a hit; Impulsed 3, Zombies / Classic FFA 0); noImpulseDist drops the distance term
    const GameModeConfig *mode_config = game->mode ? &game->mode->config : NULL;
    const float impulse_mlt = game->config.impulse_mlt * (mode_config && mode_config->has_impulse_mlt ? mode_config->impulse_mlt : 1.0f);
    const int no_impulse_dist = mode_config && mode_config->no_impulse_dist;

    // v10.0.2 (v10:172807-172840): a nonObjImpulse mode, a weapon's nonObjImpulse, or its physPull
    // with the aim input held push the shooter by the weapon's physPow (x1 with noImpulseDist), hit
    // or not; physPull + aim pulls (x -1). (v7.1.4: nonObjImpulse modes only, a fixed 0.04.)
    // Compressor: left click pushes the shooter back, right click pulls it forward; nobody else moves.
    const int pull = weapon->phys_pull && player->shot_aim;
    const int non_obj = (mode_config && mode_config->non_obj_impulse) || weapon->non_obj_impulse || pull;
    if (ray < 0 && non_obj) {
        const float power = weapon->physical_power;
        const float impulse = (no_impulse_dist ? 1.0f : power) * impulse_mlt * (pull ? -1.0f : 1.0f);
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

    if (!no_damage) combat_trace(player, origin, dir, authority && ray >= 0, &hits);
    hit_list_sort(&hits);
    // map-logic WP-B: shoot raycasts the terrain too (players.js:2982-2990; pushed first, so it wins
    // ties): nothing behind it is reached and the tracer ends on it
    const float terrain_t = no_damage ? 0.0f : world_terrain_fraction(game, origin, dir);
    if (terrain_t > 0.0f) {
        size_t kept = 0;
        for (size_t i = 0; i < hits.count; i++)
            if (hits.items[i].t < terrain_t) hits.items[kept++] = hits.items[i];
        hits.count = kept;
        hit_list_push(&hits, (Hit) {NULL, NULL, terrain_t}); // the terrain: no object, player or AI
    }
    if (authority && ray >= 0 && !no_damage) { // map-logic WP-H: canShoot spawnables (Skull EE) before the first wall
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
            if (non_obj || (object && (object->no_player_col || object->no_impulse)) || hit->t < 0.0f) break;
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
        if (i == 0 && (hit->player || hit->ai) && player->active) {
            player->life_hits++;
            player->match_hits++;
        }
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
            // v10.0.2: the box's hitType instead (headshot / legshot; arms are torsoshot, v10:172979)
            // Krunker Strike (Ruleset.hit_zones_live): v7's zones on the live target, not the rewound box
            float zone_y, zone_ah;
            player_hit_zone_base(victim, &hit->state, &zone_y, &zone_ah);
            const int head = hit->zone ? hit->zone - 1 == HIT_KIND_HEAD : player_hit_is_head(victim, zone_y, zone_ah, hit_y);
            const int leg = hit->zone ? hit->zone - 1 == HIT_KIND_LEG : player_hit_is_leg(victim, zone_y, hit_y);
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
            // v10.0.3: a model part scales the damage (part.m), an armour part (m 0) stops the bullet (deob-10.0.3.js:196797-196817)
            if (game_ai_damage_part(game, hit->ai, player, player->smite ? 10000.0f : dealt, 0, hit_y,
                                    weapon->effect_on_hit > 0 && ai_effect_count > 0 ? weapon->effect_on_hit - 1 : -1, ai_effect_count, &hit->part))
                break;
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
    if (player->active) {
        player->life_shots++;
        player->match_shots++;
    }
    game_inc_stat(game, player, "s");

    const float damage = weapon->damage * player_attr_damage(player);
    if (stream.hit == PISS_HIT_PLAYER && stream.player) {
        Player *victim = stream.player;
        if (player->active) {
            player->life_hits++;
            player->match_hits++;
        }
        game_inc_stat(game, player, "h");
        const float dealt = player_hit_damage(game, weapon, damage, 0, 0);
        if (dealt > 0.0f && player_change_health(victim, player, dealt, 0) && victim->active) {
            PlayerKillInfo info = {.weapon_id = weapon_id, .distance = stream.time / fmaxf(1.0f, weapon->stream->life)};
            player_kill(victim, player, &info, 0);
        }
    } else if (stream.hit == PISS_HIT_AI && stream.ai) {
        if (player->active) {
            player->life_hits++;
            player->match_hits++;
        }
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
    player->fire_count++; // players.js shoot: input.syncTime = true (the official "q" syncTimes)
    mode_run_shot(player->game, player); // v10.0.3 shoot: runShots++ (deob-10.0.3.js:196342; Raid's "rnogun")
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
        player->shot_look = shot_angles;

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

            if (i >= 0) player->shot_look = shot_angles; // the bullet's own look (not the physPow knock ray)
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

        // yaw = lerpAngle(xDir2, xDir1, min(1, interp)), pitch likewise (players.js update)
        const float t = MIN(1.0f, progress);

        if (player->step_carried) {
            // v10.js:169517: stepVal = s1 + (s2 - s1) * t between the two buffered states (the server's
            // walk cycle, never re-derived here); playerStep(sound only) when it crosses zero (posStep)
            player->step_val = player->interp_step_start + (player->interp_step_end - player->interp_step_start) * t;
            if (!player->pos_step && player->step_val > 0.0f) {
                player->pos_step = 1;
                player->step_events++;
            } else if (player->pos_step && player->step_val < 0.0f) {
                player->pos_step = 0;
                player->step_events++;
            }
        } else if (player->on_ground) {
            // a server whose STATE carries no step value (before 1.4.5): the cycle from the distance covered
            player_step(player, hypotf(player->last_position.x - player->position.x, player->last_position.z - player->position.z));
        }
        player->direction.y = player_lerp_angle(player->interp_dir_end.y, player->interp_dir_start.y, t);
        player->direction.x = player_lerp_angle(player->interp_dir_end.x, player->interp_dir_start.x, t);
    }
}
