// WP-B combat maths against Krunker v7.1.4 (src/data/players.js, player.js, projectiles.js,
// effects.js, streaks.js; doc/v7/player-viewer.md 1.2, 1.6): hitbox padding 0.6 / 0.1 + class
// hbS / hbH, head / leg zones (headMlt, crouch, per-weapon lsMlt, noHeadShot), the charge system
// (Crossbow 20 hip / 200 full / overcharge, knife throw ceil(180 d), Charge Rifle 750), status
// effects, perks, kill streaks (slots, earning, Nuke, Slimer, Juggernaut) and Siphon.
// Run from the repository root: ./<build>/combat_test
#include <shared.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

void player_shoot(Player*);  // shared/src/player.c
void player_melee(Player*);
void player_reload(Player*);

static int g_failures, g_checks;

#define CHECK(cond, ...) do { g_checks++; \
    if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_failures++; } \
} while (0)

#define DT 0.01f // input delta: whole ms, so procInputs' rounding keeps it exact

static int g_last_hit_damage = -1;
static int g_watch_uid = -1; // record HITs on this target only (pierce 1 rays go on into others)
static int g_last_explosion_radius = -1;

static void test_send(Game *game, const int uid, const int type, const void *payload, const uint16_t length) {
    (void) game; (void) uid;
    if (type == NET_PACKET_EXPLOSION && length == sizeof(NetExplosionPacket)) g_last_explosion_radius = (int) ((const NetExplosionPacket *) payload)->radius;
    if (type == NET_PACKET_HIT && length == sizeof(NetHitPacket) && ((const NetHitPacket *) payload)->target_uid == g_watch_uid) {
        g_last_hit_damage = ((const NetHitPacket *) payload)->damage;
    }
}

static Player *add_player(Game *game, const int uid, const int class_index) {
    Player *player = player_init(game);
    player->uid = uid;
    player->class_index = class_index;
    snprintf(player->name, sizeof(player->name), "p%d", uid);
    game_players_add(game, player);
    player_spawn(player);
    return player;
}

static void place(Player *player, const float x, const float z) {
    player->position = (vec3) {x, 1000.0f, z};
    player->velocity = (vec3) {0.0f, 0.0f, 0.0f};
    player->direction = (vec2) {0.0f, 0.0f};
    player->spread = 0.0f;
    player->spread_plus = 0.0f;
    player->recoil_anim_y = 0.0f;
    player->recoil_force = 0.0f;
    player->recoil_anim = 0.0f;
    player->crouch_val = 0.0f;
    player_update_height(player);
}

static void revive(Player *player) {
    player->active = 1;
    player->health = (float) player->max_health;
}

// Point the shooter's eye straight down -z at height `y` on the target's line.
static void aim_at(Player *shooter, const float x, const float y, const float z) {
    const float eye = shooter->position.y + shooter->height - game_constants.camera_height;
    const float dx = x - shooter->position.x, dz = z - shooter->position.z;
    shooter->direction.y = atan2f(dx, dz) - (float) M_PI;
    shooter->direction.x = atanf((y - eye) / hypotf(dx, dz));
}

static void equip(Player *player, const int weapon_id) {
    for (int i = 0; i < player->loadout_size; i++) {
        if (player->loadout[i] == weapon_id) {
            player_swap_weapon(player, i, 1, 1, 0);
            player->swap_timer = 0.0f;
            player->reloads[i] = 0.0f;
            return;
        }
    }
}

static float hitscan_damage(Game *game, Player *shooter, Player *target, const float y_offset, const float x) {
    revive(target);
    target->position.x = x;
    player_store_state(target);
    aim_at(shooter, 0.0f, target->position.y + y_offset, target->position.z);
    shooter->ammo[shooter->loadout_index] = shooter->weapon->ammo;
    shooter->spread = 0.0f;
    shooter->recoil_anim_y = 0.0f;
    shooter->reloads[shooter->loadout_index] = 0.0f;
    const float before = target->health;
    player_shoot(shooter);
    (void) game;
    return before - target->health;
}

static void test_hitbox(Game *game, Player *shooter, Player *target) {
    printf("hitbox padding (v7 hitBoxPad 0.6, Ranked 0.1, class hbS / hbH)...\n");
    CHECK(fabsf(player_hitbox_pad_w(target) - 0.6f) < 1e-6f && fabsf(player_hitbox_pad_h(target) - 0.6f) < 1e-6f, "default pad 0.6");
    game->config.hitbox_pad_set = 1;
    game->config.hitbox_pad = 0.1f;
    CHECK(fabsf(player_hitbox_pad_w(target) - 0.1f) < 1e-6f, "a 0.1 game setting");
    game->config.hitbox_pad_set = 0;

    place(shooter, 0.0f, 0.0f);
    place(target, 0.0f, -50.0f);
    equip(shooter, 0); // Sniper: one ray, 100 dmg, hsMlt 1.5
    const float half = target->scale + 0.6f;
    CHECK(hitscan_damage(game, shooter, target, 6.0f, half - 0.05f) > 0.0f, "a ray 0.05 inside scale + 0.6 hits");
    CHECK(hitscan_damage(game, shooter, target, 6.0f, half + 0.05f) == 0.0f, "a ray 0.05 outside scale + 0.6 misses (v3.1.1 pad 1 hit it)");
    CHECK(hitscan_damage(game, shooter, target, 6.0f, target->scale + 0.95f) == 0.0f, "the old v3 pad edge misses");

    // the box top: adjustedHeight + hitboxPadH (11 + 0.6); the rising ray enters the front face
    // (scale + 0.6 nearer) about 0.13 lower than the aim point
    CHECK(hitscan_damage(game, shooter, target, 11.6f, 0.0f) > 0.0f, "inside the top pad");
    CHECK(hitscan_damage(game, shooter, target, 11.85f, 0.0f) == 0.0f, "above the top pad");
}

static void test_zones(Game *game, Player *shooter, Player *target) {
    printf("head / leg zones and multipliers...\n");
    target->head_mlt = 1.0f;
    target->crouch_val = 0.0f;
    CHECK(player_hit_is_head(target, 0.0f, 11.0f, 9.1f) && !player_hit_is_head(target, 0.0f, 11.0f, 8.9f), "head band 2 below the top");
    target->head_mlt = 2.0f; // Big Head: headScale * headMlt, adjustedHeight 13
    CHECK(player_hit_is_head(target, 0.0f, 13.0f, 9.1f) && !player_hit_is_head(target, 0.0f, 13.0f, 8.9f), "headMlt 2: a 4 band under 13");
    target->head_mlt = 1.0f;
    CHECK(player_hit_is_leg(target, 0.0f, 4.1f) && !player_hit_is_leg(target, 0.0f, 4.3f), "legs below 4.2");
    target->crouch_val = 1.0f;
    CHECK(player_hit_is_leg(target, 0.0f, 1.1f) && !player_hit_is_leg(target, 0.0f, 1.3f), "crouched legs below 4.2 - 3");
    target->crouch_val = 0.0f;

    const Weapon *auto_pistol = g_weapons[16], *semi = g_weapons[7], *sniper = g_weapons[0];
    CHECK(fabsf(player_hit_damage(game, auto_pistol, 16.0f, 1, 0) - 17.6f) < 1e-3f, "Auto Pistol head 16 x 1.1 = 17.6 (KN no-headshot bug fixed; v3 noHeadShot gone)");
    CHECK(fabsf(player_hit_damage(game, semi, 35.0f, 0, 1) - 19.0015f) < 1e-3f, "Semi Auto leg 35 x 0.5429");
    CHECK(fabsf(player_hit_damage(game, semi, 35.0f, 1, 0) - 50.9985f) < 1e-3f, "Semi Auto head 35 x 1.4571");
    CHECK(fabsf(player_hit_damage(game, sniper, 100.0f, 1, 1) - 150.0f) < 1e-3f, "no leg multiplier on a headshot");
    game->config.headshots_only = 1;
    CHECK(player_hit_damage(game, sniper, 100.0f, 0, 0) == 0.0f && player_hit_damage(game, sniper, 100.0f, 1, 0) == 150.0f, "headshotOnly");
    game->config.headshots_only = 0;

    // end to end with the Sniper at 50 units: head 1.5x the body shot, legs 0.5x
    place(shooter, 0.0f, 0.0f);
    place(target, 0.0f, -50.0f);
    equip(shooter, 0);
    const float body = hitscan_damage(game, shooter, target, 6.0f, 0.0f);
    const float head = hitscan_damage(game, shooter, target, 10.0f, 0.0f);
    const float leg = hitscan_damage(game, shooter, target, 2.0f, 0.0f);
    // (changeHealth rounds every hit to 0.1, v7.1.4)
    CHECK(body > 0.0f && fabsf(head - fminf(body * 1.5f, (float) target->max_health)) < 0.1f, "Sniper head %.2f = 1.5 x body %.2f", head, body);
    CHECK(fabsf(leg - body * 0.5f) < 0.1f, "Sniper leg %.2f = 0.5 x body", leg);
    CHECK(fabsf(body * 10.0f - roundf(body * 10.0f)) < 1e-3f, "damage rounded to 0.1 (%.3f)", body);

    // Strength x10: attributes.dmg 2
    player_give_effect(shooter, EFFECT_STRENGTH, 10);
    // v7: attributes.dmg scales the base before the falloff: (2 * 100 - drop) * 0.5 = leg + 50
    const float strong = hitscan_damage(game, shooter, target, 2.0f, 0.0f);
    CHECK(fabsf(strong - (leg + 50.0f)) < 0.1f, "Strength x10 doubles the base (%.2f vs leg %.2f + 50)", strong, leg);
    player_remove_effect(shooter, EFFECT_STRENGTH, 0);
}

static GameProjectile *last_projectile(Game *game, const int weapon_index) {
    GameProjectile *found = NULL;
    for (int i = 0; i < GAME_MAX_PROJECTILES; i++) {
        if (game->projectiles[i].active && game->projectiles[i].weapon_index == weapon_index) found = &game->projectiles[i];
    }
    return found;
}

static void clear_projectiles(Game *game) {
    for (int i = 0; i < GAME_MAX_PROJECTILES; i++) game->projectiles[i].active = 0;
}

// procInputs frames holding scope (and shoot when asked), no movement.
static void frames(Player *player, const int count, const int scope, const int shoot) {
    static int seq;
    for (int i = 0; i < count; i++) {
        Input input = {.seq = ++seq, .delta = DT, .move_dir = -1, .scope = (unsigned char) scope, .shoot = (unsigned char) shoot,
                       .x_dir = player->direction.x, .y_dir = player->direction.y};
        player_proc_input(player, &input, 0, 0);
        player->position.y = 1000.0f; // hold still in the air
        player->velocity = (vec3) {0.0f, 0.0f, 0.0f};
    }
}

static float crossbow_shot(Game *game, Player *bowman, const int aim_frames) {
    clear_projectiles(game);
    equip(bowman, WEAPON_ID_CROSSBOW);
    bowman->aim_val = 0.0f;
    bowman->charge_time = 0.0f;
    bowman->over_charged = 0;
    bowman->ammo[bowman->loadout_index] = 1;
    frames(bowman, aim_frames, 1, 0);
    player_shoot(bowman);
    const GameProjectile *bolt = last_projectile(game, WEAPON_ID_CROSSBOW);
    return bolt ? bolt->config.damage : -1.0f;
}

static void test_charge(Game *game) {
    printf("charge system (chrgWeps)...\n");
    Player *bowman = add_player(game, 20, 11);
    place(bowman, 100.0f, 100.0f);

    const float hip = crossbow_shot(game, bowman, 0);
    CHECK(fabsf(hip - 20.0f) < 0.01f, "Crossbow hip shot 200 x 0.1 = 20 (got %.2f)", hip);
    const float half = crossbow_shot(game, bowman, 65); // 0.65 s: 1 - 0.9 * 0.5
    CHECK(fabsf(half - 110.0f) < 0.5f, "Crossbow half charge 110 (got %.2f)", half);
    const float full = crossbow_shot(game, bowman, 130);
    CHECK(fabsf(full - 200.0f) < 0.5f, "Crossbow full 1.3 s charge 200 (got %.2f)", full);
    // past 1.3 s the charge creeps at 1.3 * 0.125 /s to 1.56: overcharged about 1.6 s later
    bowman->aim_val = 0.0f;
    bowman->charge_time = 0.0f;
    bowman->over_charged = 0;
    bowman->ammo[bowman->loadout_index] = 1; // only a loaded weapon charges
    frames(bowman, 130 + 150, 1, 0);
    CHECK(!bowman->over_charged && bowman->charge_time > 1.5f, "not overcharged after 1.5 s more (%.3f)", bowman->charge_time);
    frames(bowman, 20, 1, 0);
    CHECK(bowman->over_charged && bowman->charge_time == 0.0f, "overcharged after 1.7 s more");
    player_shoot(bowman);
    const GameProjectile *bolt = last_projectile(game, WEAPON_ID_CROSSBOW);
    CHECK(bolt && fabsf(bolt->config.damage - 20.0f) < 0.01f, "an overcharged bolt is a hip shot");
    frames(bowman, 20, 0, 0); // un-aim
    CHECK(!bowman->over_charged, "un-aiming clears overCharged");

    game->config.no_charge_weapons = 1; // chrgWeps off: v3.1.1 always-200 bolts
    CHECK(fabsf(crossbow_shot(game, bowman, 0) - 200.0f) < 0.01f, "chrgWeps off: a hip bolt does 200");
    game->config.no_charge_weapons = 0;

    // Knife throw: Runner throwChrg 0.6 s, others 0.8
    Player *runner = add_player(game, 21, 9);
    place(runner, 120.0f, 100.0f);
    CHECK(fabsf(runner->throw_charge - 0.6f) < 1e-6f && fabsf(bowman->throw_charge - 0.8f) < 1e-6f, "class throwChrg");
    equip(runner, WEAPON_ID_KNIFE);
    clear_projectiles(game);
    frames(runner, 60, 1, 0);
    CHECK(runner->aim_val == 1.0f, "knife fully aimed");
    player_melee(runner);
    const GameProjectile *knife = last_projectile(game, WEAPON_ID_KNIFE);
    CHECK(knife && fabsf(knife->config.damage - 180.0f) < 0.01f && fabsf(knife->config.speed - 0.38f) < 1e-6f,
          "a 0.6 s Runner throw: 180 damage at full speed (%.2f, %.3f)", knife ? knife->config.damage : -1.0f, knife ? knife->config.speed : -1.0f);

    runner->can_throw = 1;
    runner->aim_val = 0.0f;
    runner->charge_time = 0.0f;
    runner->reloads[runner->loadout_index] = 0.0f;
    clear_projectiles(game);
    frames(runner, 12, 1, 0); // 0.12 s: d = 0.2
    const float d = CLAMP(runner->charge_time / 0.6f, 0.1f, 1.0f);
    player_melee(runner);
    knife = last_projectile(game, WEAPON_ID_KNIFE);
    CHECK(knife && fabsf(knife->config.damage - ceilf(180.0f * d)) < 0.01f && fabsf(knife->config.speed - 0.38f * fmaxf(0.3f, d)) < 1e-5f,
          "a quick throw: ceil(180 d) = %.0f, speed 0.38 max(0.3, d)", ceilf(180.0f * d));

    // Charge Rifle (Infiltrator): normal shots cost nothing, the full charge does 15 x 50
    Player *infiltrator = add_player(game, 22, CLASS_ID_INFILTRATOR);
    Player *target = add_player(game, 23, 0);
    place(infiltrator, 0.0f, 300.0f);
    place(target, 0.0f, 250.0f);
    player_store_state(target);
    CHECK(infiltrator->weapon == g_weapons[WEAPON_ID_CHARGE_RIFLE], "Infiltrator holds the Charge Rifle");
    aim_at(infiltrator, 0.0f, target->position.y + 6.0f, target->position.z);
    infiltrator->charge_time = 0.0f;
    infiltrator->swap_timer = 0.0f; // the spawn equip
    frames(infiltrator, 30, 0, 1); // trigger held 0.3 s: shots fire, the charge builds
    CHECK(infiltrator->ammo[infiltrator->loadout_index] == 1, "fullChrgSht: normal shots keep the round");
    CHECK(infiltrator->charge_time > 0.2f, "bypassChrg: shots keep the charge (%.3f)", infiltrator->charge_time);
    revive(target);
    g_watch_uid = target->uid;
    for (int i = 0; i < 60 && infiltrator->ammo[infiltrator->loadout_index] == 1; i++) {
        target->health = (float) target->max_health;
        g_last_hit_damage = -1;
        frames(infiltrator, 1, 0, 1);
    }
    CHECK(infiltrator->ammo[infiltrator->loadout_index] == 0, "the full-charge shot spends the round");
    CHECK(g_last_hit_damage > 700 && g_last_hit_damage <= 750, "full-charge hit ~750 (15 x 50 minus falloff): %d", g_last_hit_damage);
}

static void test_effects(Game *game) {
    printf("status effects and perks...\n");
    Player *p = add_player(game, 30, 0);
    CHECK(player_attr_speed(p) == 1.0f && player_attr_jump(p) == 1.0f && player_attr_rooted(p) == 1.0f && !player_has_perk(p, PERK_SPEED), "defaults");
    player_give_effect(p, EFFECT_BOOSTED, 3);
    CHECK(fabsf(player_attr_speed(p) - 1.3f) < 1e-6f, "Boosted x3 = 1.3");
    player_give_effect(p, EFFECT_BOOSTED, 10);
    CHECK(p->effects[EFFECT_BOOSTED] == 5 && fabsf(player_attr_speed(p) - 1.5f) < 1e-6f, "Boosted caps at 5");
    player_give_effect(p, EFFECT_ROOTED, 1);
    CHECK(fabsf(player_attr_rooted(p) - 0.75f) < 1e-6f, "Rooted 0.75");
    player_tick_effects(p, 24.9f);
    CHECK(p->effects[EFFECT_ROOTED] == 1, "Rooted still on at 24.9 s");
    player_tick_effects(p, 0.2f);
    CHECK(p->effects[EFFECT_ROOTED] == 0 && player_attr_rooted(p) == 1.0f, "Rooted wears off at 25 s");
    CHECK(p->effects[EFFECT_BOOSTED] == 5, "untimed effects stay");
    player_give_effect(p, EFFECT_SLEIGHT_OF_HAND, 2);
    player_give_perk(p, PERK_FAST_RELOAD);
    CHECK(fabsf(player_attr_reload(p) - 0.4f) < 1e-6f, "reload 0.8 x perk 0.5");
    player_give_effect(p, EFFECT_RAPID_FIRE, 4);
    player_give_perk(p, PERK_RAPID_FIRE);
    CHECK(fabsf(player_attr_fire_rate(p) - 0.8f * 0.66f) < 1e-6f, "fire rate 0.8 x 0.66");
    player_give_perk(p, PERK_HEALTH);
    CHECK(p->max_health == 250 && p->health == 250.0f && p->perk_count == 3, "perk 0: 250 HP");
    player_give_perk(p, PERK_HEALTH);
    CHECK(p->perk_count == 3, "a perk once");

    // Zapped: 2 HP per 2 stacks... zap = stacks * 0.5 * 2 every 2nd tick, credited to zappedBy
    Player *zapper = add_player(game, 31, 0);
    p->health = 100.0f;
    p->max_health = 100;
    player_give_effect(p, EFFECT_ZAPPED, 6);
    p->zapped_by = zapper->uid;
    CHECK(fabsf(p->attributes.zap - 6.0f) < 1e-6f, "Zapped x6: 6 HP a tick");
    p->regen_tick_count = 1;
    player_effects_regen_tick(p);
    CHECK(p->health == 100.0f, "zap on even ticks only");
    p->regen_tick_count = 2;
    player_effects_regen_tick(p);
    CHECK(p->health == 94.0f, "zap tick 6 HP (%.1f)", p->health);
    player_give_effect(p, EFFECT_POISONED, 1);
    p->regen_tick_count = 8;
    player_effects_regen_tick(p);
    CHECK(p->health == 94.0f - 10.0f - 6.0f, "poison 10 on the 8th tick (+ zap)");

    // effects reset at spawn
    player_spawn(p);
    CHECK(p->effects[EFFECT_BOOSTED] == 0 && player_attr_speed(p) == 1.0f && p->perk_count == 0 && p->max_health == 100, "spawn resets effects and perks");

    // Siphon: the killer heals 10 per stack
    Player *victim = add_player(game, 32, 0);
    player_give_effect(zapper, EFFECT_SIPHON, 2);
    zapper->health = 50.0f;
    player_kill(victim, zapper, NULL, 1);
    CHECK(zapper->health == 70.0f, "Siphon x2 heals 20 on a kill (%.1f)", zapper->health);
}

static void kill_n(Game *game, Player *killer, const int n) {
    static int uid = 100;
    for (int i = 0; i < n; i++) {
        Player *victim = add_player(game, uid++, 0);
        player_kill(victim, killer, NULL, 1);
    }
}

static void test_streaks(Game *game) {
    printf("kill streaks...\n");
    Player *p = add_player(game, 40, 0);
    CHECK(p->streak_indexes[0] == -1 && p->streak_indexes[1] == -1 && p->streak_indexes[2] == STREAK_NUKE, "default pick: Nuke only");
    const int bad[STREAK_SLOTS] = {STREAK_NUKE, STREAK_SLIMER, STREAK_JUGGERNAUT};
    player_set_streak_indexes(p, bad);
    CHECK(p->streak_indexes[0] == -1 && p->streak_indexes[1] == -1 && p->streak_indexes[2] == STREAK_JUGGERNAUT, "picks only count in their own slot");

    // the default pick: 25 kills earn a Nuke only with an account (offline sandbox counts)
    player_set_streak_indexes(p, NULL);
    p->has_account = 0;
    kill_n(game, p, 25);
    CHECK(!p->streaks_owned[STREAK_NUKE] && p->real_kill_streak == 0, "a guest earns no Nuke; the counter resets at 25");
    p->has_account = 1;
    kill_n(game, p, 24);
    CHECK(!p->streaks_owned[STREAK_NUKE] && p->real_kill_streak == 24, "24 kills: nothing yet");
    kill_n(game, p, 1);
    CHECK(p->streaks_owned[STREAK_NUKE] == 1 && p->real_kill_streak == 0, "25: a Nuke");
    player_check_streak(p, 2);
    CHECK(!p->streaks_owned[STREAK_NUKE] && fabsf(game->nuke_timer - 10.0f) < 1e-6f && game->nuke_player == p, "the streak key calls in the Nuke (10 s)");
    CHECK(!game_start_nuke(game, p), "one Nuke at a time");
    game->nuke_timer = 0.0f;
    game->nuke_player = NULL;

    // a 7 / 15 / 30 loadout
    const int picks[STREAK_SLOTS] = {STREAK_SLIMER, STREAK_WAR_MACHINE, STREAK_JUGGERNAUT};
    player_set_streak_indexes(p, picks);
    player_spawn(p);
    CHECK(p->real_kill_streak == 0, "realKillStreak resets at spawn");
    kill_n(game, p, 7);
    CHECK(p->streaks_owned[STREAK_SLIMER] == 1, "7: Slimer");
    kill_n(game, p, 8);
    CHECK(p->streaks_owned[STREAK_WAR_MACHINE] == 1 && p->real_kill_streak == 15, "15: War Machine");

    player_check_streak(p, 0);
    CHECK(p->loadout[0] == WEAPON_ID_SLIMER && p->weapon == g_weapons[WEAPON_ID_SLIMER] && player_has_equipment(p), "Slimer in hand");
    player_check_streak(p, 1);
    CHECK(p->streaks_owned[STREAK_WAR_MACHINE] == 1, "no War Machine while holding equipment");
    while (p->ammo[p->loadout_index] > 0) { // deploy both pads
        p->reloads[p->loadout_index] = 0.0f;
        player_shoot(p);
    }
    CHECK(!player_has_equipment(p) && p->loadout[0] == 1, "the emptied Slimer leaves the loadout");
    player_check_streak(p, 1);
    CHECK(p->loadout[0] == WEAPON_ID_WAR_MACHINE && p->weapon->no_swap, "War Machine in hand");
    const Weapon *held = p->weapon;
    player_swap_weapon(p, 1, 0, 0, 0);
    CHECK(p->weapon == held, "noSwap: can't switch away");

    kill_n(game, p, 15);
    CHECK(p->streaks_owned[STREAK_JUGGERNAUT] == 1 && p->real_kill_streak == 0, "30: Juggernaut, counter reset");
    player_remove_weapon(p, WEAPON_ID_WAR_MACHINE);
    const int class_health = p->max_health;
    player_check_streak(p, 2);
    CHECK(p->is_juggernaut && p->max_health == 2000 && p->health == 2000.0f && p->loadout[0] == WEAPON_ID_MINIGUN, "Juggernaut: 2000 HP + Minigun");
    p->ammo[p->loadout_index] = 1;
    p->reloads[p->loadout_index] = 0.0f;
    player_shoot(p);
    CHECK(!p->is_juggernaut && p->max_health == class_health && !player_has_equipment(p), "an empty Minigun ends the Juggernaut");

    game->config.no_streaks = 1;
    const unsigned int before = p->real_kill_streak;
    kill_n(game, p, 7);
    CHECK(p->real_kill_streak == before, "noStreaks: no counting");
    game->config.no_streaks = 0;

    CHECK(g_weapons[8]->kill_score == 75, "Rocket kill score 75 (v7)");

    // "wstk": kills in a row per weapon; another weapon restarts at 1, the Nuke's kills don't count
    player_spawn(p);
    for (int i = 0; i < 3; i++) {
        Player *victim = add_player(game, 900 + i, 0);
        PlayerKillInfo info = {.weapon_id = i < 2 ? 1 : 2};
        player_kill(victim, p, &info, 1);
    }
    CHECK(p->streak_weapon == 2 && p->weapon_streak == 1, "weapon streak restarts on a new weapon");
    Player *nuked = add_player(game, 910, 0);
    PlayerKillInfo nuke_info = {.weapon_id = -1};
    player_kill(nuked, p, &nuke_info, 1);
    CHECK(p->streak_weapon == 2 && p->weapon_streak == 1, "Nuke kills are not weapon kills");
}

static void test_upgrades(Game *game) {
    printf("upgradeWeapons (krumMachine slots)...\n");
    Player *p = add_player(game, 50, 0); // Triggerman: AR, Pistol, Knife
    CHECK(p->weapon == g_weapons[1] && p->weapon->ammo == 28, "base AR");
    player_upgrade_weapon(p, 0);
    CHECK(p->weapon == weapon_upgraded(1) && !strcmp(p->weapon->name, "Zombie Assaulter") && p->ammo[0] == 35 &&
          p->weapon->damage == 28.0f && fabsf(p->weapon->rate - 0.09f) < 1e-6f, "slot 0 upgraded: Zombie Assaulter, 35 rounds");
    player_swap_weapon(p, 1, 0, 1, 0);
    CHECK(p->weapon == g_weapons[2], "the Pistol slot stays base");
    player_swap_weapon(p, 0, 0, 1, 0);
    CHECK(p->weapon == weapon_upgraded(1), "back to the upgraded slot");
    player_give_weapon(p, WEAPON_ID_SLIMER); // the loadout shifts: the upgrade follows the AR
    CHECK(p->loadout[1] == 1 && p->upgraded_slots == 2 && player_slot_weapon(p, 1) == weapon_upgraded(1), "the upgrade moves with its weapon");
    player_remove_weapon(p, WEAPON_ID_SLIMER);
    CHECK(p->loadout[0] == 1 && p->upgraded_slots == 1, "and back");
    CHECK(weapon_upgraded(13)->projectile_config.explode == 100.0f && !weapon_upgraded(13)->projectile_config.dont_pred, "explosive upgraded bolts");
    player_spawn(p);
    CHECK(!p->upgraded_slots && p->weapon == g_weapons[1], "spawn drops the upgrades");
}

// map-D's v7 mode keys in the weapon / combat code: reloadMlt, kranked bonuses.reload /
// firerate, ammoLimit, headMlt (Bighead FFA), disabledStreaks, explMlt / explSMlt.
static void test_mode_multipliers(Game *game) {
    printf("mode multipliers...\n");
    GameMode *saved = game->mode;

    game->mode = mode_init(MODE_ID_IMP); // Impulsed: reloadMlt 0.5, explMlt 5, explSMlt 2, disabledStreaks [4, 5, 6]
    Player *p = add_player(game, 60, 0); // Triggerman AR
    p->ammo[p->loadout_index] = 0;
    player_reload(p);
    CHECK(fabsf(p->reload_timer - p->weapon->reload_time * 0.5f) < 1e-5f, "Impulsed reloadMlt 0.5 (%f)", p->reload_timer);
    p->reload_timer = 0.0f;

    Player *victim = add_player(game, 61, 0);
    place(p, 0.0f, 0.0f);
    place(victim, 0.0f, 15.0f);
    victim->health = 100.0f;
    g_last_explosion_radius = -1;
    game_explosion(game, (vec3) {0.0f, 1000.0f, 0.0f}, 10.0f, 20.0f, p, 8);
    CHECK(g_last_explosion_radius == 20, "explSMlt 2: the \"ex\" radius 20 (%d)", g_last_explosion_radius);
    CHECK(fabsf(victim->health - (100.0f - 20.0f * 0.25f * 5.0f)) < 1e-4f, "explMlt 5 on others (%.2f)", victim->health);
    p->health = 100.0f;
    game_explosion(game, p->position, 10.0f, 20.0f, p, 8);
    CHECK(fabsf(p->health - (100.0f - 20.0f * 0.2f)) < 1e-4f, "explMlt not on the source (%.2f)", p->health);

    const int picks[STREAK_SLOTS] = {STREAK_SLIMER, -1, STREAK_NUKE};
    player_set_streak_indexes(p, picks);
    p->has_account = 1;
    kill_n(game, p, 7);
    CHECK(!p->streaks_owned[STREAK_SLIMER], "disabledStreaks: no Slimer in Impulsed");
    mode_fini(game->mode);

    game->mode = mode_init(MODE_ID_KRANK); // bonuses reload 0.35 / firerate 0.85 while kranked
    player_spawn(p);
    p->mode.is_kranked = 1;
    p->ammo[p->loadout_index] = 0;
    player_reload(p);
    CHECK(fabsf(p->reload_timer - p->weapon->reload_time * 0.35f) < 1e-5f, "kranked reload x0.35");
    p->reload_timer = 0.0f;
    p->ammo[p->loadout_index] = p->weapon->ammo;
    p->reloads[p->loadout_index] = 0.0f;
    player_shoot(p);
    CHECK(fabsf(p->reloads[p->loadout_index] - p->weapon->rate * 0.85f) < 1e-5f, "kranked fire rate x0.85 (%f)", p->reloads[p->loadout_index]);
    p->mode.is_kranked = 0;
    mode_fini(game->mode);

    game->mode = mode_init(MODE_ID_AON); // ammoLimit 0
    player_spawn(p);
    CHECK(p->ammo[0] == 0, "All or Nothing: ammoLimit 0 at spawn (%u)", p->ammo[0]);
    mode_fini(game->mode);

    game->mode = saved;
    // Bighead FFA: player.headMlt widens the head band (headScale 2 x headMlt from the top)
    victim->mode.head_mlt = 0.0f;
    CHECK(!player_hit_is_head(victim, 0.0f, 10.0f, 7.5f), "2.5 below the top: body");
    victim->mode.head_mlt = 1.5f;
    CHECK(player_hit_is_head(victim, 0.0f, 10.0f, 7.5f), "headMlt 1.5: head");
    victim->mode.head_mlt = 0.0f;
}

// WP-C: ENT V8 challMode (v7.1.4 players.js tickPlayer: no regen while challMode)
static void test_chall_mode(Game *game) {
    Player *p = add_player(game, 9, 0);
    float now = 0.0f;
    p->health = 50.0f;
    p->hit_timer = 0.0f;
    p->chall_mode = 1;
    for (int i = 0; i < 120; i++) game_tick(game, now += 1.0f / 60.0f, 1.0f / 60.0f);
    CHECK(p->health == 50.0f, "challMode: no regen (%.1f)", (double) p->health);
    p->chall_mode = 0;
    for (int i = 0; i < 120; i++) game_tick(game, now += 1.0f / 60.0f, 1.0f / 60.0f);
    CHECK(p->health > 50.0f, "without challMode the class regen heals (%.1f)", (double) p->health);
}

int main(void) {
    cJSON *sandstorm = NULL;
    if (load_json_file("assets/maps/311_sandstorm.json", &sandstorm) && load_json_file("../assets/maps/311_sandstorm.json", &sandstorm)) {
        printf("Failed to load assets/maps/311_sandstorm.json (run from the repository root)\n");
        return 1;
    }
    static const cJSON *maps[1];
    maps[0] = sandstorm;

    Game game = {0};
    game_configure(&game, NULL, maps, 1, NULL, 0, NULL, 0, NULL, 0);
    game_init(&game, 0, -1, 1);
    game.hooks.send = test_send;
    game.lag_comp = 0;

    Player *shooter = add_player(&game, 1, 1); // Hunter: Sniper
    Player *target = add_player(&game, 2, 3);  // Spray N Pray: 180 HP holds a close Sniper headshot (~161)
    test_hitbox(&game, shooter, target);
    test_zones(&game, shooter, target);
    test_charge(&game);
    test_effects(&game);
    test_streaks(&game);
    test_upgrades(&game);
    test_mode_multipliers(&game);
    test_chall_mode(&game);

    if (g_failures) {
        printf("combat_test: %d of %d checks failed\n", g_failures, g_checks);
        return 1;
    }
    printf("combat_test: %d checks OK\n", g_checks);
    return 0;
}
