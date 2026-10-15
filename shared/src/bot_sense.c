// Bot perception and combat (shared/include/bot_sense.h): Krunker v7.1.4 bot/nextbot/vision.ts
// (KrunkerVision), combat.ts (aim parts, fire modes) and the aim-focus fields of
// krunkerConfig.ts BOT_PROFILE_PRESETS, on game.canSee (shared/src/ai.c).
//
// Intentional differences from v7.1.4 (also in doc/console.md):
// - Peripheral vision: a target outside the 90 degree FOV but inside the peripheral band is seen
//   when close (periph_range) and recognized at half speed; v7's FOV check is all or nothing.
// - Hearing: v7 seeds known threats from the manager's hearing / weapon-fire propagation (not in
//   the port); here a scan seeds gunfire (the player's shot serial changed) within hear_shots and
//   running footsteps within hear_steps, with a position error that grows with the distance.
// - Memory: a lost target is chased to its predicted position (last seen velocity for up to
//   0.8 s), then searched for in a fan of spots along the way it went until memory_ms (10 s at
//   max, less on lower tiers) pass without a stimulus; v7 paths to the raw last known position.
// - Aim: v7's aim focus re-rolls a random offset every aimFocusInterval, shrinking with the
//   lock-on time; here the error is a smoothed random walk that decays with the lock time
//   (and resets after a gap), grows with the target's angular speed, leads moving targets
//   (projectile flight time; the slew lag for hitscan) and fires in bursts that rest while the
//   recoil climb settles. Difficulty tiers set every knob (bot_sense_profile), accuracy scales
//   the error.
#include <bots.h>
#include <ai.h>
#include <pcg_basic.h>
#include <math.h>
#include <string.h>

#define BS_PI 3.14159265358979323846f
#define BS_DEG(d) ((d) * BS_PI / 180.0f)

// ---------------------------------------------------------------- helpers

static float bs_rand(void) { return ldexpf((float) pcg32_random(), -32); }
static float bs_rand_range(const float lo, const float hi) { return lo + (hi - lo) * bs_rand(); }

static float bs_d3d(const vec3 a, const vec3 b) {
    const float x = a.x - b.x, y = a.y - b.y, z = a.z - b.z;
    return sqrtf(x * x + y * y + z * z);
}

static float bs_d2d(const vec3 a, const vec3 b) { return hypotf(a.x - b.x, a.z - b.z); }

static float bs_angle_delta(const float from, const float to) { return atan2f(sinf(to - from), cosf(to - from)); }

// UTILS.getDrs(target, self, yOff): [yaw, pitch] from self to target
static void bs_drs(const vec3 target, const vec3 self, float *yaw, float *pitch) {
    const double p = self.x - target.x, q = self.y - target.y, u = self.z - target.z;
    *pitch = (float) -atan2(q, sqrt(p * p + u * u));
    *yaw = (float) -atan2(-p, u);
}

static vec3 bs_eye(const Player *p) {
    return (vec3) {p->position.x, p->position.y + p->height - game_constants.camera_height, p->position.z};
}

// Is `point` inside the cone of half-angle `half` around the look yaw (feet to feet, 2D)?
static int bs_in_cone(const Player *self, const float yaw, const vec3 point, const float half) {
    if (half >= BS_PI) return 1;
    const float dx = point.x - self->position.x, dz = point.z - self->position.z;
    if (hypotf(dx, dz) < 1e-3f) return 1;
    const float want = -atan2f(dx, -dz); // bs_drs yaw of point from self
    return fabsf(bs_angle_delta(yaw, want)) <= half;
}

// The player's shot serial: hitscan rays fired this life + projectiles thrown (player.c).
static unsigned int bs_shot_serial(const Player *p) { return (unsigned int) p->life_shots + p->shot_count; }

static int bs_alive(const Player *p) { return p && p->active && p->health > 0.0f && !p->notarget; }

static int bs_enemy(const Player *self, const Player *other, const BotSenseRules *rules) {
    if (!bs_alive(other) || other == self) return 0;
    if (!rules->target_bots && other->is_bot) return 0;
    if (!rules->target_team && self->team && self->team == other->team) return 0;
    return 1;
}

static BotThreat *bs_slot(BotSense *sense, const int uid) {
    for (int i = 0; i < BOT_SENSE_THREATS; i++) {
        if (sense->threats[i].uid == uid) return &sense->threats[i];
    }
    return NULL;
}

// A free slot, else the oldest known threat's (a fresher stimulus replaces it).
static BotThreat *bs_new_slot(BotSense *sense) {
    BotThreat *oldest = NULL;
    for (int i = 0; i < BOT_SENSE_THREATS; i++) {
        BotThreat *t = &sense->threats[i];
        if (t->uid < 0) return t;
        if (!oldest || t->known_at < oldest->known_at) oldest = t;
    }
    return oldest;
}

static void bs_threat_init(BotThreat *t, const int uid, const float now) {
    memset(t, 0, sizeof(*t));
    t->uid = uid;
    t->known_at = now;
    t->acquired_at = now;
    t->visible_at = t->visible_since = t->aware_at = t->reacquired_at = -1.0f;
    t->sample_at = -1.0f;
    t->hit_at = -1.0f;
}

// ---------------------------------------------------------------- profiles

void bot_sense_reset(BotSense *sense) {
    memset(sense, 0, sizeof(*sense));
    for (int i = 0; i < BOT_SENSE_THREATS; i++) sense->threats[i].uid = -1;
    sense->search_uid = -1;
    sense->aim_uid = -1;
}

BotSenseProfile bot_sense_profile(const float difficulty, const float accuracy) {
    BotSenseProfile p;
    // BOT_PROFILE_PRESETS: skill / reactionTime + attackDelay / aimFocusInitial / aimFocusDecay /
    // aimFocusInterval, and the KrunkNative senses per tier
    if (difficulty >= BOT_DIFFICULTY_MAX) {
        p = (BotSenseProfile) {BS_DEG(60), BS_DEG(100), 420.0f, 2600.0f, 260.0f, BOT_SENSE_RECOGNIZE_MS, 50.0f, BOT_SENSE_FORGET_MS,
                               0.016f, 0.004f, 6.0f, 400.0f, 1.0f, 0.035f, 0, 0.0f, 1.0f, 0.02f};
    } else if (difficulty >= BOT_DIFFICULTY_HARD) {
        p = (BotSenseProfile) {BS_DEG(55), BS_DEG(90), 300.0f, 1800.0f, 200.0f, BOT_SENSE_RECOGNIZE_MS / 0.75f, 300.0f, 9000.0f,
                               0.065f, 0.022f, 4.0f, 500.0f, 0.85f, 0.021f, 8, 180.0f, 0.85f, 0.035f};
    } else if (difficulty >= BOT_DIFFICULTY_NORMAL) {
        p = (BotSenseProfile) {BS_DEG(50), BS_DEG(85), 220.0f, 1300.0f, 160.0f, BOT_SENSE_RECOGNIZE_MS / 0.5f, 550.0f, 7000.0f,
                               0.1f, 0.04f, 3.0f, 600.0f, 0.6f, 0.016f, 5, 250.0f, 0.6f, 0.045f};
    } else {
        p = (BotSenseProfile) {BS_DEG(45), BS_DEG(75), 160.0f, 900.0f, 120.0f, BOT_SENSE_RECOGNIZE_MS / 0.2f, 900.0f, 5000.0f,
                               0.14f, 0.065f, 2.0f, 800.0f, 0.3f, 0.012f, 3, 350.0f, 0.35f, 0.055f};
    }
    // config accuracy (0..100, the bot_difficulty presets 25 / 50 / 85 / 100): 100 halves the
    // error, 50 keeps it, 0 doubles it
    const float acc = CLAMP(accuracy, 0.0f, 100.0f) / 100.0f;
    const float mlt = 2.0f - acc * 1.5f;
    p.aim_error *= mlt;
    p.aim_error_locked *= mlt;
    return p;
}

// ---------------------------------------------------------------- line of sight

vec3 bot_sense_aim_part(const Player *t, const BotAimPart part) {
    const float height = t->height > 1.0f ? t->height : 1.0f;
    switch (part) {
        case BOT_AIM_GUT: return (vec3) {t->position.x, t->position.y + height * 0.48f, t->position.z};
        case BOT_AIM_CHEST: return (vec3) {t->position.x, t->position.y + height * 0.6f, t->position.z};
        case BOT_AIM_HEAD: return (vec3) {t->position.x, t->position.y + height - 1.4f, t->position.z};
        case BOT_AIM_FEET: return (vec3) {t->position.x, t->position.y + height * 0.18f, t->position.z};
        default: return t->position;
    }
}

int bot_sense_los_point(const Game *game, const Player *self, const vec3 point) {
    return game_ai_player_can_see(game, self, point); // game.canSee from the eye
}

int bot_sense_los(const Game *game, const Player *self, const Player *other) {
    static const BotAimPart k_order[] = {BOT_AIM_GUT, BOT_AIM_CHEST, BOT_AIM_HEAD, BOT_AIM_FEET};
    for (size_t i = 0; i < sizeof(k_order) / sizeof(k_order[0]); i++) {
        if (bot_sense_los_point(game, self, bot_sense_aim_part(other, k_order[i]))) return (int) k_order[i];
    }
    return -1;
}

// ---------------------------------------------------------------- the scan

void bot_sense_seed(BotSense *sense, const Player *other, const vec3 position, const BotStimulus stimulus) {
    if (!bs_alive(other)) return;
    BotThreat *t = bs_slot(sense, other->uid);
    if (!t) {
        t = bs_new_slot(sense);
        bs_threat_init(t, other->uid, sense->time);
    }
    t->known = position;
    t->known_at = sense->time;
    t->known_seen = 0;
    t->visible = 0;
    t->visible_since = -1.0f;
    t->stimulus = (unsigned char) stimulus;
    t->shots = bs_shot_serial(other);
    if (stimulus == BOT_STIMULUS_HIT) {
        t->given_up = 0;
        t->hit_at = sense->time;
    }
    // heard / hit stimuli are actionable at once (no recognize gate)
    if (t->aware_at < 0.0f) t->aware_at = sense->time;
    t->velocity = (vec3) {0.0f, 0.0f, 0.0f};
}

void bot_sense_give_up(BotSense *sense, const int uid) {
    BotThreat *t = bs_slot(sense, uid);
    if (t && !t->visible) t->given_up = 1;
}

int bot_sense_clear_shot(const Game *game, const Player *self, const float yaw, const float pitch, const float distance, const float spread, const float floor_y) {
    // the centre ray first, then the edges and corners of the spread square (x is pitch, y is yaw, each
    // drawn independently in player.c): a shot that may land anywhere in it must not meet a wall
    const vec3 eye = bs_eye(self);
    const int rings = spread > 0.0f ? 9 : 1;
    static const signed char k_dy[9] = {0, 1, -1, 0, 0, 1, 1, -1, -1}, k_dp[9] = {0, 0, 0, 1, -1, 1, -1, 1, -1};
    for (int k = 0; k < rings; k++) {
        const float y = yaw + (float) k_dy[k] * spread, pt = pitch + (float) k_dp[k] * spread;
        const float cp = cosf(pt);
        vec3 far = {eye.x - sinf(y) * cp * distance, eye.y + sinf(pt) * distance, eye.z - cosf(y) * cp * distance};
        if (far.y < floor_y) far.y = floor_y;
        if (!bot_sense_los_point(game, self, far)) return 0;
    }
    return 1;
}

// player.c's recoil step (combat update, before the shot of the tick): recoilAnim / recoilAnimY grow
// by the recoil force, then recover. *anim / *anim_y: the values the shot of a tick `dt` ms long sees.
static void bs_recoil_step(const Player *self, const float dt, float *anim, float *anim_y) {
    const Weapon *w = self->weapon;
    float a = self->recoil_anim, ay = self->recoil_anim_y;
    if (self->recoil_force) {
        a += self->recoil_force * dt / 1000.0f;
        ay += self->recoil_force * (!w->recoil_y ? 1.0f : w->recoil_y) * (1.0f - self->crouch_val * 0.3f) * dt / 1000.0f;
    }
    a *= powf(w->recover, dt);
    ay *= powf(w->recover_y ? w->recover_y : w->recover, dt);
    *anim = a;
    *anim_y = ay;
}

#define BS_SPREAD_MLT 240.0f // config.spreadMlt (player.c COMBAT_SPREAD_MLT: the recoilAnim term of the spread)

float bot_sense_shot_spread(const Player *self, const float dt) {
    const Weapon *w = self->weapon;
    if (!w || w->melee || w->no_spread) return 0.0f;
    if (w->custom_spread && w->custom_spread_count > 0) { // cSpread pattern * (0.3 .. 0.32)
        float widest = 0.0f;
        for (unsigned int i = 0; i < w->custom_spread_count; i++) {
            widest = MAX(widest, MAX(fabsf(w->custom_spread[i].x), fabsf(w->custom_spread[i].y)));
        }
        return widest * 0.32f;
    }
    // hitscan rays: +-range per axis; projectiles: range * projSpreads (|factor| <= 1). The recoil
    // the tick adds widens the spread before the shot (scaled like the rest by the hip / aim share)
    float anim, anim_y;
    bs_recoil_step(self, dt, &anim, &anim_y);
    const float aim_share = (1.0f - self->aim_val) + self->aim_val * w->aim_spread;
    const float grow = MAX(0.0f, anim - self->recoil_anim) * BS_SPREAD_MLT * aim_share;
    return (self->spread + grow + w->inaccuracy) * game_constants.spread_adjustment;
}

float bot_sense_shot_climb(const Player *self, const float dt) {
    if (!self->weapon) return 0.0f;
    float anim, anim_y;
    bs_recoil_step(self, dt, &anim, &anim_y);
    return anim_y * game_constants.recoil_mlt;
}

// Hearing: a position error that grows with the distance (easy ears are vaguer).
static vec3 bs_heard_at(const Player *other, const float distance, const float range) {
    const float err = 6.0f + 40.0f * (distance / (range > 1.0f ? range : 1.0f));
    const float a = bs_rand_range(-BS_PI, BS_PI);
    return (vec3) {other->position.x + cosf(a) * err, other->position.y, other->position.z + sinf(a) * err};
}

static void bs_scan(Game *game, BotSense *sense, const Player *self, const float yaw, const BotSenseProfile *profile, const BotSenseRules *rules) {
    const float now = sense->time;
    for (size_t i = 0; i < game->player_count; i++) {
        const Player *o = game->players[i];
        if (!o || o == self) continue;
        BotThreat *prev = bs_slot(sense, o->uid);
        if (!bs_enemy(self, o, rules)) {
            if (prev) prev->uid = -1; // dead / switched team: obsolete (CKnownEntity::IsObsolete)
            continue;
        }
        const float distance = bs_d3d(o->position, self->position);
        const unsigned int serial = bs_shot_serial(o);

        // sight: the FOV cone, or the peripheral band when close
        int visible = 0, peripheral = 0, part = -1;
        if (distance <= rules->max_distance) {
            if (bs_in_cone(self, yaw, o->position, profile->fov_half)) visible = 1;
            else if (distance <= profile->periph_range && bs_in_cone(self, yaw, o->position, profile->periph_half)) visible = peripheral = 1;
            if (visible) {
                part = bot_sense_los(game, self, o);
                visible = part >= 0;
            }
        }

        // hearing: gunfire in earshot, running footsteps nearby (not while in sight anyway)
        int heard = 0, fired = 0;
        if (!visible) {
            fired = o->did_shoot || (prev && prev->shots != serial);
            const float speed = hypotf(o->velocity.x, o->velocity.z);
            const int running = o->on_ground && o->crouch_val < 0.5f && speed > 0.25f * game_constants.player_speed;
            if (fired && distance <= profile->hear_shots) heard = 1;
            else if (running && distance <= profile->hear_steps) heard = 1;
        }

        if (!visible && !heard && !prev) continue;
        BotThreat *t = prev;
        if (!t) {
            t = bs_new_slot(sense);
            bs_threat_init(t, o->uid, now);
        }
        t->shots = serial;

        if (visible) {
            // a velocity sample from the scans (units/s)
            if (t->sample_at >= 0.0f && now > t->sample_at + 1.0f) {
                const float s = (now - t->sample_at) / 1000.0f;
                vec3 v = {(o->position.x - t->sample_pos.x) / s, (o->position.y - t->sample_pos.y) / s, (o->position.z - t->sample_pos.z) / s};
                if (now - t->sample_at > BOT_SENSE_SCAN_MS * 3.0f) v = (vec3) {0.0f, 0.0f, 0.0f}; // too old a sample
                t->velocity = v;
            }
            t->sample_pos = o->position;
            t->sample_at = now;
            if (!t->visible) {
                t->visible_since = now;
                if (t->visible_at >= 0.0f) t->reacquired_at = now;
            }
            t->visible = 1;
            t->given_up = 0;
            t->peripheral = (unsigned char) peripheral;
            t->part = (unsigned char) part;
            t->known = o->position;
            t->known_at = t->visible_at = now;
            t->known_seen = 0;
            t->stimulus = BOT_STIMULUS_SEEN;
            // the recognize gate: a fresh sighting takes recognize_ms (twice that peripherally)
            if (t->aware_at < 0.0f) {
                const float need = profile->recognize_ms * (peripheral ? 2.0f : 1.0f);
                if (now - t->visible_since >= need) t->aware_at = now;
            }
        } else {
            if (t->visible) t->visible = 0;
            t->visible_since = -1.0f;
            t->sample_at = -1.0f;
            if (heard) {
                const float range = fired ? profile->hear_shots : profile->hear_steps;
                t->known = bs_heard_at(o, distance, range);
                t->known_at = now;
                t->known_seen = 0;
                t->stimulus = BOT_STIMULUS_HEARD;
                if (t->aware_at < 0.0f) t->aware_at = now;
            } else if (!t->known_seen && bs_in_cone(self, yaw, t->known, profile->fov_half)) {
                // the last known spot is in view and empty: it moved on
                const vec3 spot = {t->known.x, t->known.y + o->height * 0.5f, t->known.z};
                if (bot_sense_los_point(game, self, spot)) t->known_seen = 1;
            }
        }
    }

    // forget: gone, or no stimulus for memory_ms (easy bots never recognized it: half that)
    for (int i = 0; i < BOT_SENSE_THREATS; i++) {
        BotThreat *t = &sense->threats[i];
        if (t->uid < 0) continue;
        const Player *p = game_find_player(game, t->uid);
        const float age = now - t->known_at;
        const float keep = t->aware_at >= 0.0f ? profile->memory_ms : profile->memory_ms * 0.5f;
        if (!bs_alive(p) || age > keep) {
            t->uid = -1;
            if (sense->search_uid == t->uid) sense->search_uid = -1;
        }
    }
}

// Being hurt: whoever hit us last is known at its real position (the hit direction tells).
static void bs_hit_stimulus(Game *game, BotSense *sense, const Player *self, const BotSenseRules *rules) {
    const PlayerDamage *latest = NULL;
    for (int k = 0; k < PLAYER_DAMAGE_SLOTS; k++) {
        const PlayerDamage *d = &self->damage_received[k];
        if (d->uid >= 0 && (!latest || d->time > latest->time)) latest = d;
    }
    const Player *attacker = latest ? game_find_player(game, latest->uid) : NULL;
    if (!attacker || !bs_enemy(self, attacker, rules)) return;
    BotThreat *t = bs_slot(sense, attacker->uid);
    if (t && t->visible) { // in sight already: the sight keeps its position, the hit is still recorded
        t->hit_at = sense->time; // (bot_sense_hit_age: a shooter in sight is under-fire too, the "forgor" report)
        t->given_up = 0;
        return;
    }
    bot_sense_seed(sense, attacker, attacker->position, BOT_STIMULUS_HIT);
}

void bot_sense_update(Game *game, BotSense *sense, const Player *self, const float yaw, const BotSenseProfile *profile, const BotSenseRules *rules, const int hurt, const float dt) {
    sense->time += dt;
    sense->scan_timer -= dt;
    if (sense->reaction_left > 0.0f) sense->reaction_left -= dt;
    if (sense->burst_rest > 0.0f) sense->burst_rest -= dt;
    if (hurt) bs_hit_stimulus(game, sense, self, rules);
    if (sense->scan_timer > 0.0f && !hurt) return;
    sense->scan_timer = BOT_SENSE_SCAN_MS;
    bs_scan(game, sense, self, yaw, profile, rules);
}

const BotThreat *bot_sense_threat(const BotSense *sense, const int uid) {
    for (int i = 0; i < BOT_SENSE_THREATS; i++) {
        if (sense->threats[i].uid == uid && uid >= 0) return &sense->threats[i];
    }
    return NULL;
}

float bot_sense_hit_age(const BotSense *sense, const int uid) {
    const BotThreat *t = bot_sense_threat(sense, uid);
    return t && t->hit_at >= 0.0f ? sense->time - t->hit_at : -1.0f;
}

int bot_sense_visible(const BotSense *sense, const int uid) {
    const BotThreat *t = bot_sense_threat(sense, uid);
    return t && t->visible;
}

static int bs_aware(const BotSense *sense, const BotThreat *t, const float recognize_ms) {
    return t->aware_at >= 0.0f || sense->time - t->acquired_at >= recognize_ms;
}

// ThreatScore: distance to the last known position + age * 0.05 - the visible bonus
static float bs_score(const BotSense *sense, const Player *self, const BotThreat *t, const float max_dist) {
    const float dist = bs_d3d(self->position, t->known);
    if (dist > max_dist) return INFINITY;
    const float age = sense->time - t->known_at;
    return dist + (age > 0.0f ? age : 0.0f) * 0.05f - (t->visible ? BOT_SENSE_VISIBLE_BONUS : 0.0f);
}

Player *bot_sense_select(Game *game, const BotSense *sense, const Player *self, const BotSenseRules *rules, const int current_uid, const int ignore_uid) {
    Player *best = NULL;
    float best_score = rules->max_distance;
    const BotThreat *current = current_uid >= 0 && current_uid != ignore_uid ? bot_sense_threat(sense, current_uid) : NULL;
    if (current) {
        Player *p = game_find_player(game, current->uid);
        if (bs_enemy(self, p, rules)) {
            const float score = bs_score(sense, self, current, rules->max_distance);
            if (isfinite(score)) {
                best = p;
                best_score = score + BOT_SENSE_SWITCH_MARGIN;
            }
        }
    }
    for (int i = 0; i < BOT_SENSE_THREATS; i++) {
        const BotThreat *t = &sense->threats[i];
        if (t->uid < 0 || t->uid == current_uid || t->uid == ignore_uid) continue;
        if (t->aware_at < 0.0f || t->given_up) continue;
        Player *p = game_find_player(game, t->uid);
        if (!bs_enemy(self, p, rules)) continue;
        const float score = bs_score(sense, self, t, rules->max_distance);
        if (score < best_score) {
            best_score = score;
            best = p;
        }
    }
    return best;
}

int bot_sense_predict(const BotSense *sense, const int uid, vec3 *out) {
    const BotThreat *t = bot_sense_threat(sense, uid);
    if (!t) return 0;
    const float age = sense->time - t->known_at;
    const float s = (age < BOT_SENSE_PREDICT_MS ? age : BOT_SENSE_PREDICT_MS) / 1000.0f;
    *out = (vec3) {t->known.x + t->velocity.x * s, t->known.y, t->known.z + t->velocity.z * s};
    return 1;
}

// ---------------------------------------------------------------- the search

int bot_sense_search(Game *game, BotSense *sense, const int uid, const Player *self, const int arrived, const float dt, vec3 *out) {
    BotThreat *t = bs_slot(sense, uid);
    if (!t) {
        sense->search_uid = -1;
        sense->search_active = 0;
        return 0;
    }
    if (sense->search_uid != uid) { // a new hunt
        sense->search_uid = uid;
        sense->search_active = 0;
        sense->search_step = 0;
        sense->search_timer = 0.0f;
    }
    vec3 predicted;
    bot_sense_predict(sense, uid, &predicted);
    if (!sense->search_active) {
        // head for where it probably is; once there (or the spot is in view and empty), search
        const int there = arrived || bs_d2d(self->position, predicted) < BOT_SENSE_SEARCH_REACHED;
        if (!there && !t->known_seen) {
            *out = predicted;
            return 1;
        }
        sense->search_active = 1;
        sense->search_timer = 0.0f;
    }
    sense->search_timer -= dt;
    const int reached = arrived || bs_d2d(self->position, sense->search_point) < BOT_SENSE_SEARCH_REACHED;
    if (sense->search_timer <= 0.0f || reached || sense->search_step == 0) {
        // a fan of spots along the way it was going (any way when it stood still), further out
        // each step, and a new look at the spot it was last known at every fourth step
        sense->search_step++;
        sense->search_timer = BOT_SENSE_SEARCH_STEP_MS;
        const float speed = hypotf(t->velocity.x, t->velocity.z);
        float heading = speed > 5.0f ? atan2f(t->velocity.z, t->velocity.x) : bs_rand_range(-BS_PI, BS_PI);
        if (sense->search_step % 4 == 0) {
            sense->search_point = predicted;
        } else {
            heading += bs_rand_range(-1.0f, 1.0f) * BS_DEG(70);
            const float reach = 60.0f + 50.0f * (float) ((sense->search_step - 1) % 4);
            vec3 spot = {predicted.x + cosf(heading) * reach, predicted.y, predicted.z + sinf(heading) * reach};
            vec3 walkable;
            // snap onto the nav grid's area around it when there is one
            if (bot_nav_random_point_from(game, &spot, &walkable) && bs_d2d(walkable, spot) < 90.0f) spot = walkable;
            sense->search_point = spot;
        }
    }
    *out = sense->search_point;
    return 1;
}

// ---------------------------------------------------------------- aim

float bot_sense_aim_error(const BotSenseProfile *profile, const float lock_ms) {
    const float t = (lock_ms > 0.0f ? lock_ms : 0.0f) / 1000.0f;
    return (profile->aim_error - profile->aim_error_locked) * expf(-profile->aim_decay * t) + profile->aim_error_locked;
}

vec3 bot_sense_lead(const BotSenseProfile *profile, const vec3 eye, const vec3 point, const vec3 velocity, const Weapon *w) {
    const float distance = bs_d3d(eye, point);
    float seconds;
    if (w && w->projectile_config.speed > 0.0f) seconds = distance / w->projectile_config.speed / 1000.0f; // spd units/ms
    else seconds = 0.06f; // hitscan: the slew lag of the look
    seconds *= profile->lead;
    return (vec3) {point.x + velocity.x * seconds, point.y + velocity.y * seconds * 0.5f, point.z + velocity.z * seconds};
}

void bot_sense_engage(BotSense *sense, const BotSenseProfile *profile, const Player *self, const int uid, const float extra_ms) {
    sense->shots_serial = bs_shot_serial(self);
    if (sense->aim_uid != uid) {
        sense->aim_uid = uid;
        sense->lock_ms = 0.0f;
        sense->unseen_ms = 0.0f;
        sense->aim_valid = 0;
        // the fresh lock's error, inside the profile's band like every later one (bot_sense_aim): a
        // square of +-aim_error per axis put its corners 1.41 x aim_error off
        const float a = bs_rand_range(-BS_PI, BS_PI), r = bs_rand_range(0.4f, 1.0f) * profile->aim_error;
        sense->err_yaw = sense->err_goal_yaw = cosf(a) * r;
        sense->err_pitch = sense->err_goal_pitch = sinf(a) * r * 0.6f;
        sense->err_timer = 0.0f;
    }
    sense->reaction_left = profile->reaction_ms + extra_ms;
    sense->burst_count = 0;
    sense->burst_rest = 0.0f;
    sense->trigger = 0;
}

void bot_sense_aim(BotSense *sense, const BotSenseProfile *profile, const Player *self, const Player *target, const int visible, const vec3 fallback,
                   const float dt, float *want_yaw, float *want_pitch) {
    const vec3 eye = bs_eye(self);
    const BotThreat *t = bot_sense_threat(sense, target->uid);
    if (sense->aim_uid != target->uid) bot_sense_engage(sense, profile, self, target->uid, 0.0f);

    // lock state: the error decays while the target stays in sight, a gap over 300 ms resets it
    if (visible) {
        sense->lock_ms += dt;
        sense->unseen_ms = 0.0f;
    } else {
        sense->unseen_ms += dt;
        if (sense->unseen_ms > 300.0f) sense->lock_ms = 0.0f;
    }

    vec3 point;
    if (visible) {
        const BotAimPart part = t && t->part < BOT_AIM_PARTS ? (BotAimPart) t->part : BOT_AIM_CHEST;
        // the part in sight; a well locked-on marksman walks the aim up to the head
        const BotAimPart prefer = profile->aim_error_locked < 0.015f && sense->lock_ms > 400.0f ? BOT_AIM_HEAD : BOT_AIM_CHEST;
        point = bot_sense_aim_part(target, part == BOT_AIM_GUT || part == BOT_AIM_CHEST ? prefer : part);
        const vec3 velocity = t ? t->velocity : (vec3) {0.0f, 0.0f, 0.0f};
        point = bot_sense_lead(profile, eye, point, velocity, self->weapon);
    } else {
        point = (vec3) {fallback.x, fallback.y + target->height * 0.6f, fallback.z};
    }
    float yaw, pitch;
    bs_drs(point, eye, &yaw, &pitch);

    // the target's angular speed across the view makes it harder to track
    float angular = 0.0f;
    if (sense->aim_valid) angular = fabsf(bs_angle_delta(sense->aim_yaw, yaw)) / (dt > 1.0f ? dt : 1.0f) * 1000.0f; // rad/s
    sense->aim_yaw = yaw;
    sense->aim_pitch = pitch;
    sense->aim_valid = 1;

    // the tracking error: a new goal every aim_interval (sooner while the lock is fresh), the
    // error eased toward it; its magnitude follows the aim-focus curve
    sense->err_timer -= dt;
    if (sense->err_timer <= 0.0f) {
        const float fresh = sense->lock_ms < 500.0f ? 0.5f : 1.0f;
        sense->err_timer = profile->aim_interval_ms * fresh;
        float mag = bot_sense_aim_error(profile, sense->lock_ms);
        mag *= 1.0f + (angular > 3.0f ? 3.0f : angular) * 0.35f * (1.0f - profile->lead);
        const float a = bs_rand_range(-BS_PI, BS_PI), r = bs_rand_range(0.4f, 1.0f) * mag;
        sense->err_goal_yaw = cosf(a) * r;
        sense->err_goal_pitch = sinf(a) * r * 0.6f;
    }
    const float ease = 1.0f - expf(-dt / 120.0f);
    sense->err_yaw += (sense->err_goal_yaw - sense->err_yaw) * ease;
    sense->err_pitch += (sense->err_goal_pitch - sense->err_pitch) * ease;
    yaw += sense->err_yaw;
    pitch += sense->err_pitch;

    // pull down against the recoil climb (shots leave at pitch + recoilAnimY * recoilMlt)
    pitch -= self->recoil_anim_y * game_constants.recoil_mlt * profile->recoil_pull;
    *want_yaw = yaw;
    *want_pitch = CLAMP(pitch, -BS_PI * 0.5f, BS_PI * 0.5f);
}

// ---------------------------------------------------------------- fire control

int bot_sense_fire(BotSense *sense, const BotSenseProfile *profile, const Player *self, const int aimed, const int visible, const int in_range, const float dt) {
    (void) dt;
    const Weapon *w = self->weapon;
    const unsigned int serial = bs_shot_serial(self);
    if (serial != sense->shots_serial) { // a shot went off since the last decision
        sense->burst_count += (int) (serial - sense->shots_serial);
        sense->shots_serial = serial;
    }
    if (!w || !aimed || !visible || !in_range || sense->reaction_left > 0.0f) {
        sense->trigger = 0;
        if (!visible || !w) sense->burst_count = 0;
        return 0;
    }
    if (w->melee) {
        sense->trigger = 1;
        return 1;
    }
    // an uncompensated climb pauses the trigger (the recoil-aware rest)
    const float climb = self->recoil_anim_y * game_constants.recoil_mlt * (1.0f - profile->recoil_pull);
    if (climb > profile->recoil_settle) {
        sense->trigger = 0;
        return 0;
    }
    if (w->no_auto || w->burst) { // semi-automatic: the rate paces it (the frames alternate)
        sense->trigger = 1;
        return 1;
    }
    if (sense->burst_rest > 0.0f) {
        sense->trigger = 0;
        return 0;
    }
    if (profile->burst_shots > 0 && sense->burst_count >= profile->burst_shots) {
        sense->burst_count = 0;
        sense->burst_rest = profile->burst_rest_ms * bs_rand_range(0.7f, 1.3f);
        sense->trigger = 0;
        return 0;
    }
    sense->trigger = 1;
    return 1;
}
