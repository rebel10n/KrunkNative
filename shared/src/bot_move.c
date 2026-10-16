// Bot player locomotion (shared/include/bot_move.h): slide hopping and the jump / crouch / slide
// gating of the input frames shared/src/bots.c builds.
//
// Krunker v10.0.2 BotManager.slideHop (v10:174693, the `btsh` / bhop key): a bot presses jump on
// every grounded frame once its slide has <= 50 ms left and crouch while falling with canSlide.
// That hops into walls, ceilings and off ledges, and waits the slide out on the ground.
// KrunkNative (intentional differences):
// - The hop fires on the first grounded frame that can jump: players.tryJump wants the key up
//   between jumps, so the landing frame releases it and the next one presses it (1 - 2 frames on
//   the ground). The landing's slide boost (resetStep: velocity x playerSlideVelocityMlt) is
//   carried into the air under air friction (0.9996 / ms) instead of slide (0.999) or ground
//   (0.99) friction, and air acceleration adds to it: a hopping bot outruns a walking one.
// - The crouch is released while rising (canSlide comes back under crouchVal 0.5, player.js) and
//   held while falling, so every landing starts a slide when the run past it is clear.
// - Before a hop the jump is simulated (jump velocity, jumpPush along the look, rising / falling
//   gravity, air friction, the held key's air acceleration) and the player's box is swept along
//   the arc against the map's solids: a ceiling or a wall in the arc, no ground or a hazard under
//   the landing, a wall in the stop / slide run past it (the slide follows the look), a route
//   corner or end before the landing refuse it, and the bot walks. In the air the rest of the
//   flight is checked again every frame (the AI turns its aim and keys while airborne): the air
//   strafe or the AI's key is kept only when it leaves the flight clear, else the key is let go. The route segment ahead (bot_path.h) must be a WALK
//   link without a ledge / low ceiling / hazard flag with BOT_PATH_HOP_HEADROOM of headroom.
// - Jumps the AI asks for (ledges, low obstacles, dodges, randomJump) are refused under a ceiling
//   the jump would hit, and on a ladder.
// - Skill gaps (BotHopSkill, per bot from its personality seed, correlated with difficulty):
//   some bots never hop, "learning" ones mistime it (wait on the ground and lose speed, forget the
//   crouch so the landing does not slide, hesitate, skip landings), clean ones hop on the next
//   frame after every clear landing. Every check above applies to all three.
// - slideHop is off by default for every bot (bot_config_defaults, NextBots included; the
//   bot_config / bot_add key turns it on). Off, a bot never hops, bhops or air strafes: no
//   grounded hop, no air strafe, no hop-flight key steering, no slide crouch. The single jumps the
//   AI asks for (ledges, low obstacles, dodges, randomJump) and the route needs are still made,
//   under the same ceiling and ladder checks.
#include <bot_move.h>
#include <bots.h>
#include <bot_path.h>
#include <pcg_basic.h>
#include <math.h>
#include <string.h>

#define BM_PI 3.14159265358979323846f
#define BM_SAMPLES 20            // boxes swept along the arc
#define BM_BOX_MAX 256           // solids gathered around one arc (more: the map is scanned per sample)
#define BM_STEP_TOL 3.2f         // climbHeight 3: a step the feet take in stride is no obstacle
#define BM_HEAD_MARGIN 0.6f      // above the standing height
#define BM_SIDE_MARGIN 0.4f      // around scale + hitboxPadW
#define BM_LAND_DROP 8.0f        // ground this far under the landing feet still catches the bot
#define BM_RISE_DRAG 0.032f      // player.js: velocity.y -= delta * 0.032 while rising (per s)
#define BM_MIN_SPEED 0.012f      // units / ms: slower is standing still (walking ~0.04)
#define BM_RETRY_MS 150.0f       // a refused hop: walk this long before asking again
#define BM_BUMP_HOLD_MS 500.0f   // a ceiling hit (any cause): no hops for this long
#define BM_CORNER_COS 0.906f     // cos 25 deg: a route segment this well aligned continues the run
#define BM_SLIDE_MS 360.0f       // the slide after a landing travels about speed * this
#define BM_STOP_MS 110.0f        // an upright landing stops within speed * this (ground friction 0.99 / ms)
#define BM_FRAME_MS (1000.0f / 60.0f)
// learning hoppers (BOT_HOP_LEARNING), per landing
#define BM_LEARN_WAIT_MIN 50.0f  // ms on the ground before the hop (3 - 13 frames; the crouch and
#define BM_LEARN_WAIT_MAX 220.0f // the slide run out past ~140 ms and ground friction eats the speed)
#define BM_LEARN_SKIP 0.15f      // chance to let a landing go by (walk on for a moment)
#define BM_LEARN_HESITATE 0.07f  // chance to stand still for a moment
#define BM_LEARN_NO_SLIDE 0.35f  // chance to forget the crouch for the landing (no slide hop)

static float bm_rand(void) { return (float) ldexp((double) pcg32_random(), -32); }

static float bm_wrap(const float a) { return atan2f(sinf(a), cosf(a)); }

static vec3 bm_forward(const float yaw) { return (vec3) {-sinf(yaw), 0.0f, -cosf(yaw)}; }

static float bm_d2d(const vec3 a, const vec3 b) { return hypotf(a.x - b.x, a.z - b.z); }

void bot_move_reset(BotMove *m) {
    const unsigned char skill = m->skill; // the bot's, not this life's (bot_move_apply refreshes it)
    memset(m, 0, sizeof(*m));
    m->skill = skill;
}

const char *bot_move_reason_name(const int reason) {
    switch (reason) {
        case BOT_MOVE_OK: return "ok";
        case BOT_MOVE_NO_PATH_KIND: return "path link";
        case BOT_MOVE_NO_PATH_FLAG: return "path flag";
        case BOT_MOVE_NO_PATH_HEADROOM: return "path headroom";
        case BOT_MOVE_NO_CEILING: return "ceiling";
        case BOT_MOVE_NO_WALL: return "wall";
        case BOT_MOVE_NO_GROUND: return "no ground";
        case BOT_MOVE_NO_HAZARD: return "hazard";
        case BOT_MOVE_NO_CORNER: return "corner";
        case BOT_MOVE_NO_SPEED: return "standing";
        case BOT_MOVE_NO_CHANCE: return "cadence";
        default: return "?";
    }
}

// ---------------------------------------------------------------- solids

typedef struct {
    float lo[3], hi[3];
    unsigned char ramp;
} BmBox;

typedef struct {
    BmBox boxes[BM_BOX_MAX];
    int count;                  // -1: too many, scan the map per test
    const Map *map;
} BmSolids;

static void bm_object_box(const Object *o, BmBox *b) {
    const vec3 size = object_box_size(o);
    b->lo[0] = o->position.x - size.x * 0.5f;
    b->hi[0] = o->position.x + size.x * 0.5f;
    b->lo[1] = o->position.y;
    b->hi[1] = o->position.y + size.y;
    b->lo[2] = o->position.z - size.z * 0.5f;
    b->hi[2] = o->position.z + size.z * 0.5f;
    b->ramp = (unsigned char) (o->ramp != 0);
}

static int bm_box_overlaps(const BmBox *b, const float lo[3], const float hi[3]) {
    return !(hi[0] < b->lo[0] || lo[0] > b->hi[0] || hi[1] < b->lo[1] || lo[1] > b->hi[1] || hi[2] < b->lo[2] || lo[2] > b->hi[2]);
}

// The solids touching the world box lo..hi.
static void bm_collect(const Game *game, const float lo[3], const float hi[3], BmSolids *out) {
    out->count = 0;
    out->map = game->map;
    if (!game->map) return;
    for (size_t i = 0; i < game->map->object_count; i++) {
        const Object *o = game->map->objects[i];
        if (!bot_object_blocks_player(o)) continue;
        BmBox b;
        bm_object_box(o, &b);
        if (!bm_box_overlaps(&b, lo, hi)) continue;
        if (out->count >= BM_BOX_MAX) {
            out->count = -1;
            return;
        }
        out->boxes[out->count++] = b;
    }
}

// The first solid (ramps: include ramps) overlapping lo..hi, NULL for none; *scratch holds a
// scanned object's box when the gathered list overflowed.
static const BmBox *bm_hit(const BmSolids *s, const float lo[3], const float hi[3], const int ramps, BmBox *scratch) {
    if (s->count >= 0) {
        for (int i = 0; i < s->count; i++) {
            if ((ramps || !s->boxes[i].ramp) && bm_box_overlaps(&s->boxes[i], lo, hi)) return &s->boxes[i];
        }
        return NULL;
    }
    if (!s->map) return NULL;
    for (size_t i = 0; i < s->map->object_count; i++) {
        const Object *o = s->map->objects[i];
        if (!bot_object_blocks_player(o) || (!ramps && o->ramp)) continue;
        bm_object_box(o, scratch);
        if (bm_box_overlaps(scratch, lo, hi)) return scratch;
    }
    return NULL;
}

static float bm_half_width(const Player *self) { return self->scale + player_hitbox_pad_w(self) + BM_SIDE_MARGIN; }

// ---------------------------------------------------------------- the hop

// No solid of `solids` in the body's run from `from` along `dir` for `len` units.
static int bm_run_clear(const BmSolids *solids, const vec3 from, const vec3 dir, const float len, const float s, const float height) {
    const float lo[3] = {MIN(from.x, from.x + dir.x * len) - s, from.y + BM_STEP_TOL, MIN(from.z, from.z + dir.z * len) - s};
    const float hi[3] = {MAX(from.x, from.x + dir.x * len) + s, from.y + height, MAX(from.z, from.z + dir.z * len) + s};
    BmBox scratch;
    return bm_hit(solids, lo, hi, 0, &scratch) == NULL;
}

// The flight of a body at `feet` with velocity `vel` over a floor at `floor_y` (the take-off
// floor), holding a move key whose world direction is `key` (zero: no key) the whole way: the
// boxes it sweeps are tested against the map's solids, then the ground under the end of the fall,
// the hazards under the landing and the run past it (the ground friction stop along the landing
// velocity; the slide, which follows the look, along it and along `look`).
static int bm_flight(const Game *game, const Player *self, const vec3 feet, const vec3 vel, const vec3 key, const float floor_y, const vec3 look, const float run_limit,
                     BotMoveHop *out) {
    memset(out, 0, sizeof(*out));
    const GameConstants *k = &game_constants;
    const Weapon *w = self->weapon;
    const float weapon_speed = w && w->speed_mlt > 0.0f ? w->speed_mlt : 1.0f;
    const float weapon_grav = w && w->gravity_mlt > 0.0f ? w->gravity_mlt : 1.0f;
    const float grav = k->gravity * (game->config.gravity_mlt > 0.0f ? game->config.gravity_mlt : 1.0f) * weapon_grav;
    const float g_up = (grav + BM_RISE_DRAG) * 0.001f, g_dn = grav * 0.001f; // per ms^2
    // the vertical arc: rise to the apex, fall back to the floor
    const float h0 = feet.y - floor_y, vy = vel.y;
    const float t_up = vy > 0.0f ? vy / g_up : 0.0f;
    const float apex_h = MAX(0.0f, h0 + (vy > 0.0f ? 0.5f * vy * t_up : 0.0f));
    const float vf = MIN(vy, 0.0f);
    const float t_dn = (vf + sqrtf(vf * vf + 2.0f * g_dn * apex_h)) / g_dn;
    const float total = MAX(t_up + t_dn, BM_FRAME_MS);
    const float air_decel = k->air_decel, air_accel = k->air_speed * weapon_speed * 0.001f; // per ms
    out->apex = MAX(0.0f, apex_h - h0);
    out->air_ms = total;

    // the horizontal path, sampled (air friction, the key's acceleration)
    vec3 pos[BM_SAMPLES + 1];
    float px = feet.x, pz = feet.z, vx = vel.x, vz = vel.z;
    const float step = total / BM_SAMPLES;
    const float dec = powf(air_decel, step);
    pos[0] = feet;
    float lo_x = px, hi_x = px, lo_z = pz, hi_z = pz;
    for (int i = 1; i <= BM_SAMPLES; i++) {
        px += vx * step;
        pz += vz * step;
        vx = vx * dec + key.x * air_accel * step;
        vz = vz * dec + key.z * air_accel * step;
        const float t = step * (float) i;
        const float y = t < t_up ? h0 + vy * t - 0.5f * g_up * t * t : apex_h + vf * (t - t_up) - 0.5f * g_dn * (t - t_up) * (t - t_up);
        pos[i] = (vec3) {px, floor_y + MAX(y, 0.0f), pz};
        lo_x = MIN(lo_x, px), hi_x = MAX(hi_x, px), lo_z = MIN(lo_z, pz), hi_z = MAX(hi_z, pz);
    }
    pos[BM_SAMPLES].y = floor_y;
    const vec3 land = pos[BM_SAMPLES];
    // the run past the landing: the slide follows the look, the stop the landing velocity
    const float v = hypotf(vx, vz);
    const vec3 vdir = v > 1e-4f ? (vec3) {vx / v, 0.0f, vz / v} : look;
    const float slide = v * BM_SLIDE_MS * 1.3f;
    for (int i = 0; i < 2; i++) {
        const vec3 d = i ? look : vdir;
        lo_x = MIN(lo_x, land.x + d.x * slide), hi_x = MAX(hi_x, land.x + d.x * slide);
        lo_z = MIN(lo_z, land.z + d.z * slide), hi_z = MAX(hi_z, land.z + d.z * slide);
    }
    const float s = bm_half_width(self);
    const float height = k->player_height + BM_HEAD_MARGIN;
    const float lo[3] = {lo_x - s - 1.0f, floor_y - BM_LAND_DROP - 1.0f, lo_z - s - 1.0f};
    const float hi[3] = {hi_x + s + 1.0f, floor_y + apex_h + height + 1.0f, hi_z + s + 1.0f};
    BmSolids solids; // one gather for the arc and the run past it
    bm_collect(game, lo, hi, &solids);
    BmBox scratch;

    // the apex (between two samples) first, then the samples
    for (int i = -1; i <= BM_SAMPLES; i++) {
        vec3 f;
        float t;
        if (i < 0) {
            if (t_up <= 0.0f) continue;
            const int a = MIN((int) (t_up / step), BM_SAMPLES - 1);
            const float u = CLAMP(t_up / step - (float) a, 0.0f, 1.0f);
            f = (vec3) {pos[a].x + (pos[a + 1].x - pos[a].x) * u, floor_y + apex_h, pos[a].z + (pos[a + 1].z - pos[a].z) * u};
            t = t_up;
        } else {
            f = pos[i];
            t = step * (float) i;
        }
        const float blo[3] = {f.x - s, f.y + BM_STEP_TOL, f.z - s}, bhi[3] = {f.x + s, f.y + height, f.z + s};
        const BmBox *hit = bm_hit(&solids, blo, bhi, 0, &scratch);
        if (hit) {
            out->reason = hit->lo[1] > MAX(feet.y, floor_y) + height * 0.5f ? BOT_MOVE_NO_CEILING : BOT_MOVE_NO_WALL; // above the body: a ceiling
            out->travel = bm_d2d(f, feet);
            return 0;
        }
        if (i < 0) continue;
        // the last part of the fall must have ground under it (not a ledge, not the void): a solid
        // between the floor less BM_LAND_DROP and the sample's feet
        if (i && t >= t_up + 0.6f * t_dn && !self->on_terrain) {
            const float glo[3] = {f.x - s * 0.5f, floor_y - BM_LAND_DROP, f.z - s * 0.5f}, ghi[3] = {f.x + s * 0.5f, f.y + 1.0f, f.z + s * 0.5f};
            if (!bm_hit(&solids, glo, ghi, 1, &scratch)) {
                out->travel = bm_d2d(f, feet);
                out->reason = BOT_MOVE_NO_GROUND;
                return 0;
            }
        }
    }
    out->travel = bm_d2d(land, feet);
    out->land_y = floor_y;
    if (bot_hazard_at(game, land)) {
        out->reason = BOT_MOVE_NO_HAZARD;
        return 0;
    }
    if (run_limit >= 0.0f && out->travel + s > run_limit) {
        out->reason = BOT_MOVE_NO_CORNER;
        return 0;
    }
    // past the landing: the ground friction stop (speed / -ln 0.99 ~ speed x 100 ms) must be free
    // of walls, and the slide (speed x BM_SLIDE_MS, along the velocity and the look) too for a
    // crouched landing, else land upright
    if (!bm_run_clear(&solids, land, vdir, v * BM_STOP_MS, s, height)) {
        out->reason = BOT_MOVE_NO_WALL;
        return 0;
    }
    out->slide_clear = bm_run_clear(&solids, land, vdir, slide, s, height) && bm_run_clear(&solids, land, look, slide, s, height);
    out->reason = BOT_MOVE_OK;
    return 1;
}

int bot_move_plan(const Game *game, const Player *self, const float look_yaw, const float key_yaw, const int crouched, const float run_limit, BotMoveHop *out) {
    const GameConstants *k = &game_constants;
    const Weapon *w = self->weapon;
    const float jump_mlt = game->config.jump_mlt > 0.0f ? game->config.jump_mlt : 1.0f;
    const float weapon_jump = w ? (w->jump_mlt > 0.0f ? w->jump_mlt : (w->speed_mlt > 0.0f ? w->speed_mlt : 1.0f)) : 1.0f;
    // crouchJump takes 25% x crouchVal off the jump; the crouch is let go on the jump frame, so
    // plan with a little less crouch than now (a higher arc: the safe side for ceilings)
    const float crouch = crouched ? MAX(0.0f, self->crouch_val - 0.25f) : 0.0f;
    const float up = k->jump_velocity * jump_mlt * weapon_jump * (1.0f - k->crouch_jump * crouch);
    // players.jump: jumpPush adds a tenth of the speed along the LOOK, not the velocity
    const vec3 look = bm_forward(look_yaw);
    const float speed = hypotf(self->velocity.x, self->velocity.z);
    const float push = k->jump_push * jump_mlt * speed;
    // a hop out of a slide: the slide has turned the velocity to the look (player.c: v7 slides always follow
    // the look), so the speed leaves along it. Skytemple: a bot that landed sliding on a drop hopped, and the
    // flight planned along its old velocity found the floor the hop really flew past (50 units off the ledge).
    const int sliding = self->on_ground && self->slide_timer > 0.0f;
    const vec3 vel = sliding ? (vec3) {look.x * (speed + push), up, look.z * (speed + push)}
                             : (vec3) {self->velocity.x + look.x * push, up, self->velocity.z + look.z * push};
    const vec3 key = isnan(key_yaw) ? (vec3) {0.0f, 0.0f, 0.0f} : bm_forward(key_yaw);
    return bm_flight(game, self, self->position, vel, key, self->position.y, look, run_limit, out);
}

// Room for a standing jump of `apex` over the head at the feet and a little ahead.
static int bm_headroom(const Game *game, const Player *self, const vec3 dir, const float apex) {
    const float s = bm_half_width(self);
    const vec3 f = self->position;
    const float ahead = 6.0f;
    const float lo[3] = {MIN(f.x, f.x + dir.x * ahead) - s, f.y + game_constants.player_height, MIN(f.z, f.z + dir.z * ahead) - s};
    const float hi[3] = {MAX(f.x, f.x + dir.x * ahead) + s, f.y + game_constants.player_height + apex + BM_HEAD_MARGIN, MAX(f.z, f.z + dir.z * ahead) + s};
    BmSolids solids;
    bm_collect(game, lo, hi, &solids);
    BmBox scratch;
    return bm_hit(&solids, lo, hi, 0, &scratch) == NULL;
}

// ---------------------------------------------------------------- the route ahead

static int bm_segment_ok(const BotPathSegment *s) {
    if (s->kind != BOT_PATH_WALK) return BOT_MOVE_NO_PATH_KIND;
    if (s->flags & (BOT_PATH_FLAG_LEDGE | BOT_PATH_FLAG_LOW_CEILING | BOT_PATH_FLAG_HAZARD)) return BOT_MOVE_NO_PATH_FLAG;
    if (s->headroom < BOT_PATH_HOP_HEADROOM) return BOT_MOVE_NO_PATH_HEADROOM;
    return BOT_MOVE_OK;
}

// The straight run along `dir` the route allows before it turns, ends or stops being hoppable:
// *run (-1: the move is off the route, only the world sweep applies). The segment being walked
// must itself be hoppable (the reason otherwise).
static int bm_path_run(const BotPathSegment *segs, const int n, const vec3 dir, float *run) {
    *run = -1.0f;
    const int first = bm_segment_ok(&segs[0]);
    if (first != BOT_MOVE_OK) return first;
    float total = 0.0f;
    for (int i = 0; i < n; i++) {
        const BotPathSegment *s = &segs[i];
        const float dx = s->to.x - s->from.x, dz = s->to.z - s->from.z;
        const float len = hypotf(dx, dz);
        if (len < 0.5f) continue;
        const float along = (dx * dir.x + dz * dir.z) / len;
        if (along < BM_CORNER_COS) {
            if (i == 0) return BOT_MOVE_OK; // off the route (a dodge, a side step): the sweep decides
            break;
        }
        if (i && bm_segment_ok(s) != BOT_MOVE_OK) break;
        total += len * along;
        if (s->flags & BOT_PATH_FLAG_LAST) break;
    }
    *run = total;
    return BOT_MOVE_OK;
}

// ---------------------------------------------------------------- per tick

uint32_t bot_move_seed(const char *name, const int uid) {
    uint32_t h = 2166136261u; // FNV-1a over the name, then the uid (bot_chat_personality_of)
    for (const unsigned char *p = (const unsigned char *) (name ? name : ""); *p; p++) h = (h ^ *p) * 16777619u;
    return (h ^ (uint32_t) uid) * 16777619u;
}

int bot_move_skill_roll(const uint32_t seed, const float difficulty) {
    // the hop talent: a uniform [0, 1) from the seed (murmur3 fmix32 with a salt, so it does not
    // follow the chat personality, which takes the seed's own bits); higher is better
    uint32_t h = seed ^ 0x9e3779b9u;
    h ^= h >> 16;
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    h *= 0xc2b2ae35u;
    h ^= h >> 16;
    const float talent = (float) (h >> 8) / 16777216.0f;
    float none, learning; // shares under which a bot never hops / is learning
    if (difficulty >= BOT_DIFFICULTY_MAX) none = 0.0f, learning = 0.0f;
    else if (difficulty >= BOT_DIFFICULTY_HARD) none = 0.0f, learning = 0.15f;
    else if (difficulty >= BOT_DIFFICULTY_NORMAL) none = 0.30f, learning = 0.35f;
    else none = 0.75f, learning = 0.25f;
    if (talent < none) return BOT_HOP_NONE;
    if (talent < none + learning) return BOT_HOP_LEARNING;
    return BOT_HOP_CLEAN;
}

int bot_move_skill(const GameBot *bot) { return bot_move_skill_roll(bot_move_seed(bot->name, bot->uid), bot->config.difficulty); }

const char *bot_move_skill_name(const int skill) {
    switch (skill) {
        case BOT_HOP_NONE: return "none";
        case BOT_HOP_LEARNING: return "learning";
        case BOT_HOP_CLEAN: return "clean";
        default: return "?";
    }
}

int bot_move_hops(const GameBot *bot) { return bot->config.slide_hop && bot_move_skill(bot) != BOT_HOP_NONE; }

static void bm_refuse(BotMove *m, const int reason) {
    m->reason = reason;
    m->vetoes++;
    if (reason >= 0 && reason <= BOT_MOVE_NO_CHANCE) m->refused[reason]++;
    m->retry_ms = BM_RETRY_MS;
}

void bot_move_apply(Game *game, GameBot *bot, Player *self, const float dt, const float look_yaw, int *move, unsigned char *jump, unsigned char *crouch, unsigned char *jump_late) {
    BotMove *m = &bot->move;
    *jump_late = 0;
    m->skill = (unsigned char) bot_move_skill(bot);
    if (!self->active || self->noclip) return;
    const int grounded = self->on_ground || self->on_ladder;

    // timers, landings, head bumps
    if (grounded) {
        if (m->air_ms > 0.0f) {
            m->landings++;
            m->rolled = 0;
            m->hopping = 0;
        }
        m->ground_ms += dt;
        m->air_ms = 0.0f;
    } else {
        m->air_ms += dt;
        m->ground_ms = 0.0f;
    }
    if (m->retry_ms > 0.0f) m->retry_ms -= dt;
    if (self->stop_crouch) {
        m->bumps++;
        if (m->retry_ms < BM_BUMP_HOLD_MS) m->retry_ms = BM_BUMP_HOLD_MS;
    }
    const float yaw_delta = bm_wrap(m->prev_yaw - look_yaw); // procInputs: direction.y - input.y_dir
    m->prev_yaw = look_yaw;
    m->strafing = 0;

    const int travelling = *move >= 0;
    const float move_yaw = travelling ? look_yaw - (float) *move * (BM_PI / 4.0f) : look_yaw;
    const vec3 dir = bm_forward(move_yaw);
    const float vx = self->velocity.x, vz = self->velocity.z;
    const float speed = hypotf(vx, vz);
    const float key_yaw = travelling ? move_yaw : NAN;
    // slideHop (config, off by default): no grounded hop, bhop, air strafe or slide crouch below.
    // Only the single jumps of section 1 are made.
    const int slide = bot->config.slide_hop != 0;

    // 1. a jump the AI asked for: never into a ceiling, never off a ladder
    if (*jump) {
        if (!self->on_ground || self->on_ladder || self->was_stop_crouch) *jump = 0;
        else {
            BotMoveHop h;
            bot_move_plan(game, self, look_yaw, key_yaw, *crouch, -1.0f, &h);
            if (h.reason == BOT_MOVE_NO_CEILING || !bm_headroom(game, self, dir, h.apex)) {
                *jump = 0;
                m->ai_jumps_refused++;
                m->reason = BOT_MOVE_NO_CEILING;
            } else {
                m->hopping = 1;
                m->guard = 0; // it may aim at a ledge above the floor: the AI steers the flight
                m->hop_travel = h.travel;
                m->hop_apex = h.apex;
                m->slide_land = (unsigned char) (slide && h.reason == BOT_MOVE_OK && h.slide_clear);
            }
        }
        if (*jump) return;
    }

    const int hops = slide && m->skill != BOT_HOP_NONE && travelling && !*crouch && !self->on_ladder;

    if (!grounded) {
        // air strafe (config.airStrf): a right / left key while the look turns the same way swings
        // the velocity by the turn; take it when the body must turn that way toward the move
        int strafe = -1;
        if (hops && game->config.air_strafe && speed > 0.02f && (*move == 0 || *move == 1 || *move == 7)) {
            const float vdir = atan2f(-vz, -vx), wdir = atan2f(-dir.z, -dir.x);
            const float need = bm_wrap(wdir - vdir);
            if (fabsf(need) > 0.05f && yaw_delta * need > 0.0f && fabsf(yaw_delta) <= fabsf(need) * 1.5f) strafe = need > 0.0f ? 2 : 6; // MOVE_RIGHT / MOVE_LEFT
        }
        if (slide && m->hopping && m->guard) {
            // our hop: the look (aim) and the AI's key change in the air, and air acceleration /
            // the strafe bend the flight. Keep the first of strafe, the AI's key, no key whose
            // rest of the flight (from here, held to the landing) is clear; the strafe is judged
            // as if it had swung the velocity all the way to the move direction.
            const vec3 look = bm_forward(look_yaw);
            const int want = strafe >= 0 ? strafe : *move;
            const int cands[3] = {strafe, *move, -1};
            int chosen = -1, strafed = 0, slide_ok = 0;
            for (int c = 0; c < 3; c++) {
                const int code = cands[c];
                if (c < 2 && code < 0) continue;
                vec3 vel = self->velocity;
                if (c == 0) vel.x = dir.x * speed, vel.z = dir.z * speed;
                const vec3 key = code >= 0 ? bm_forward(look_yaw - (float) code * (BM_PI / 4.0f)) : (vec3) {0.0f, 0.0f, 0.0f};
                BotMoveHop h;
                if (bm_flight(game, self, self->position, vel, key, m->hop_floor, look, -1.0f, &h)) {
                    chosen = code;
                    strafed = c == 0;
                    slide_ok = h.slide_clear;
                    break;
                }
            }
            // (nothing clear at all, pushed off course: no key, no slide)
            if (chosen != want) m->air_holds++;
            *move = chosen;
            m->strafing = (unsigned char) strafed;
            if (!slide_ok) m->slide_land = 0;
        } else if (strafe >= 0) {
            *move = strafe;
            m->strafing = 1;
        }
        // let the crouch go while rising (canSlide comes back), hold it while falling into a slide
        // when the run past the landing is clear
        if (!*crouch) *crouch = (unsigned char) (self->velocity.y <= 0.0f && self->can_slide && m->slide_land && hops);
        return;
    }

    if (!hops) return;
    *crouch = 0; // upright between hops: the landing's slide runs on while crouchVal drops
    // this landing's timing: a clean hopper goes at once, a learning one waits, skips, hesitates
    if (!m->rolled) {
        m->rolled = 1;
        m->wait_ms = 0.0f;
        if (m->skill == BOT_HOP_LEARNING) {
            m->wait_ms = BM_LEARN_WAIT_MIN + bm_rand() * (BM_LEARN_WAIT_MAX - BM_LEARN_WAIT_MIN);
            if (bm_rand() < BM_LEARN_HESITATE) {
                m->hesitate_ms = 150.0f + bm_rand() * 200.0f;
                m->hesitations++;
            } else if (bm_rand() < BM_LEARN_SKIP) {
                m->reason = BOT_MOVE_NO_CHANCE;
                m->retry_ms = 400.0f + bm_rand() * 600.0f;
            }
        }
    }
    if (m->hesitate_ms > 0.0f) {
        m->hesitate_ms -= dt;
        *move = -1; // MOVE_NONE: stand for a moment
        return;
    }
    if (m->retry_ms > 0.0f || self->was_stop_crouch || self->jump_timer > 0.0f) return;
    if (m->ground_ms < m->wait_ms) return;
    if (speed < BM_MIN_SPEED) {
        m->reason = BOT_MOVE_NO_SPEED;
        return;
    }

    // the route ahead
    float run = -1.0f;
    BotPathSegment segs[8];
    const int n = bot_path_current(bot, segs, 8);
    if (n) {
        const int r = bm_path_run(segs, n, dir, &run);
        if (r != BOT_MOVE_OK) {
            bm_refuse(m, r);
            return;
        }
    } else {
        vec3 goal;
        if (bot_path_goal(bot, &goal)) run = bm_d2d(goal, self->position); // straight at it: land before it
    }
    BotMoveHop h;
    if (!bot_move_plan(game, self, look_yaw, key_yaw, 1, run, &h)) {
        bm_refuse(m, h.reason);
        return;
    }

    // the key: tryJump wants it released since the last jump. A tick of two or more frames
    // releases it on the first and presses it on the second; a one-frame tick waits a frame.
    if (self->did_jump) {
        if (dt < BM_FRAME_MS * 1.5f) return;
        *jump_late = 1;
    }
    *jump = 1;
    m->hopping = 1;
    m->guard = 1;
    m->hop_floor = self->position.y;
    m->hops++;
    m->hop_travel = h.travel;
    m->hop_apex = h.apex;
    m->slide_land = (unsigned char) (h.slide_clear && (run < 0.0f || run >= h.travel + speed * BM_SLIDE_MS));
    if (m->slide_land && m->skill == BOT_HOP_LEARNING && bm_rand() < BM_LEARN_NO_SLIDE) {
        m->slide_land = 0; // forgets to crouch for the landing: no slide, no boost
        m->slides_forgotten++;
    }
    m->reason = BOT_MOVE_OK;
}
