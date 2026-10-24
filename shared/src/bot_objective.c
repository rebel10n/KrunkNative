// Bots play the mode (shared/include/bot_objective.h): the objective goal of a NextBot's Hunt
// state and the objective-vs-combat weighting of its Attack state. KrunkNative addition: Krunker
// v7.1.4's NextBot (bot/nextbot/states.ts) only roams and fights; the legacy bots.js Bot does
// not even roam. The mode rules themselves are read-only here (shared/src/modes/*): a bot
// scores exactly like a player does, by standing where the mode scores (flag boxes, flag zones,
// objective volumes, dom zones, spawnable pads).
//
// Decisions are made every ~0.5 s (at once when the bot's flag / KR Tag count changes) from the
// authority's mode state. Fairness: bots use what a player's HUD shows (flag pings refreshed
// every flagUpdate, zone ownership, the active objective), plus the server's own knowledge of
// where the remaining enemies are in elimination modes (rate-limited to a 2 s old position).
#include <bot_objective.h>
#include <bot_sense.h>
#include <spawnables.h>
#include <defuse.h>
#include <pcg_basic.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

int mode_player_collides(const Player*, vec3 center, vec3 half); // shared/src/modes/mode_objectives.c (Player.collides, a box)

#define OBJ_PI 3.14159265358979323846f
#define OBJ_THINK_MIN_MS 450.0f
#define OBJ_THINK_MAX_MS 750.0f
#define OBJ_HUNT_THINK_MS 2000.0f    // elimination: how stale the hunted enemy's position may be
#define OBJ_FAIL_MS 4000.0f          // an unreachable goal: roam instead for this long
#define OBJ_ARRIVE_D 4.0f            // a point goal counts as reached within it (feet, 2D)
#define OBJ_TAG_RANGE 600.0f         // tags further away are not worth the walk
#define OBJ_TAG_DENY_RANGE 120.0f    // our own team's KR Tags: deny them only when this close
#define OBJ_DEPOSIT_TAGS 4           // carry this many KR Tags before heading for the objective
#define OBJ_HOLD_FIGHT_RANGE 160.0f  // a holder leaves its spot only for an enemy this close
#define OBJ_CHASE_RANGE 180.0f       // on the way to a goal: an enemy further away is shot at on the move, not chased
#define OBJ_STALL_MS 6000.0f         // a fight that has not ended after this long: back to the objective, shooting on the move
#define OBJ_UNDER_FIRE_MS 3000.0f    // a player that hit the bot this recently is still the fight (not the objective)
#define OBJ_GUARD_OFFSET 26.0f       // the defender's spot: this far from the flag toward the enemy
#define OBJ_SPREAD_MAX 14.0f         // spread spots inside a zone: at most this far off its centre
#define OBJ_KITE_MS 8000.0f          // a neutral-flag carrier picks a new hiding spot this often
#define OBJ_STALEMATE_MS 15000.0f    // CTF: a carrier waiting for our flag this long hunts their carrier itself
#define OBJ_LOOK_SWEEP_MS 1500.0f
#define OBJ_LOOK_SWEEP_RAD 0.7f
#define OBJ_WANDER_MIN_MS 1200.0f    // holding: a new spot inside the zone this often (or once reached)
#define OBJ_WANDER_MAX_MS 2500.0f
#define OBJ_WANDER_REACH 22.0f       // ... within this of the bot (a strafe, not a lap of the zone)
#define OBJ_WANDER_GOAL 10.0f        // ... within this of the goal for a hold without a volume (a flag guard)
#define OBJ_WANDER_INSET 4.0f        // ... this far inside the volume's edges (the body stays in)

// ---------------------------------------------------------------- helpers

static float obj_rand(const float lo, const float hi) { return lo + (hi - lo) * (float) ldexp((double) pcg32_random(), -32); }

static float obj_d2d(const vec3 a, const vec3 b) { return hypotf(a.x - b.x, a.z - b.z); }

static float obj_d3d(const vec3 a, const vec3 b) {
    const float x = a.x - b.x, y = a.y - b.y, z = a.z - b.z;
    return sqrtf(x * x + y * y + z * z);
}

static int obj_teams(const Game *game) { return game->mode && game->mode->config.teams; }

static int obj_alive(const Player *p) { return p && p->active && !p->notarget; }

static int obj_enemy(const Game *game, const Player *self, const Player *o) {
    return o != self && obj_alive(o) && (!obj_teams(game) || !self->team || o->team != self->team);
}

static int obj_mate(const Game *game, const Player *self, const Player *o) {
    return o != self && obj_alive(o) && obj_teams(game) && self->team && o->team == self->team;
}

static int obj_enemy_team(const Game *game, const Player *self) {
    return obj_teams(game) && (self->team == 1 || self->team == 2) ? 3 - self->team : 0;
}

// The centroid of a team's spawn points (0: none); team 0 = every spawn.
static int obj_spawn_centroid(const Game *game, const int team, vec3 *out) {
    const Map *map = game->map;
    vec3 sum = {0.0f, 0.0f, 0.0f};
    int n = 0;
    for (size_t s = 0; map && s < map->spawn_count; s++) {
        const Spawn *spawn = map->spawns[s];
        if (team && spawn->team != team) continue;
        sum.x += spawn->position.x;
        sum.y += spawn->position.y;
        sum.z += spawn->position.z;
        n++;
    }
    if (!n) return 0;
    *out = (vec3) {sum.x / (float) n, sum.y / (float) n, sum.z / (float) n};
    return 1;
}

// Where the enemy comes from: their spawns (teams), else nothing.
static int obj_threat_point(const Game *game, const Player *self, vec3 *out) {
    const int enemy = obj_enemy_team(game, self);
    return enemy && obj_spawn_centroid(game, enemy, out);
}

// This bot among its team's active bots, in list order (humans are not ranked: they play their
// own game; a human carrier still gets escorts).
static void obj_rank(const Game *game, const GameBot *bot, const Player *self, BotObjective *o) {
    int rank = 0, n = 0;
    const GameBots *bots = game->bots;
    for (int i = 0; bots && i < bots->count; i++) {
        const GameBot *b = &bots->list[i];
        const Player *p = b->player;
        if (!p || !p->active || (obj_teams(game) && p->team != self->team)) continue;
        if (b == bot) rank = n;
        n++;
    }
    o->rank = rank;
    o->team_bots = n > 0 ? n : 1;
}

// 1 when no other active bot of the team is closer to `point` (ties: the lower rank claims it).
// Flag carriers have their own job and claim nothing for the others.
static int obj_nearest_bot_is_me(const Game *game, const GameBot *bot, const Player *self, const vec3 point) {
    const GameBots *bots = game->bots;
    const float mine = obj_d3d(self->position, point);
    for (int i = 0; bots && i < bots->count; i++) {
        const GameBot *b = &bots->list[i];
        const Player *p = b->player;
        if (b == bot || !p || !p->active || p->mode.flag || (obj_teams(game) && p->team != self->team)) continue;
        const float d = obj_d3d(p->position, point);
        if (d < mine - 1e-3f || (fabsf(d - mine) <= 1e-3f && b < bot)) return 0;
    }
    return 1;
}

static void obj_set_goal(BotObjective *o, const BotGoalKind kind, const int index, const vec3 goal, const int hold) {
    if (o->kind != kind || o->index != index) {
        o->hold_time = 0.0f;
        o->arrived = 0;
        o->has_wander = 0;
    }
    o->kind = kind;
    o->index = index;
    o->goal = goal;
    o->hold = (unsigned char) hold;
    o->has_zone = 0;
    o->has_look = 0;
}

static void obj_set_zone(BotObjective *o, const vec3 center, const vec3 half) {
    o->zone_center = center;
    o->zone_half = half;
    o->has_zone = 1;
}

static void obj_set_look(BotObjective *o, const vec3 at) {
    o->look = at;
    o->has_look = 1;
}

// A spot inside a volume for this bot: its floor, spread by rank so the team does not stack
// (alt: the centre, after a spread spot proved unreachable).
static vec3 obj_zone_spot(const vec3 center, const vec3 half, const BotObjective *o) {
    vec3 feet = {center.x, center.y - half.y, center.z};
    if (o->team_bots <= 1 || o->alt) return feet;
    const float a = (float) o->rank * 2.0f * OBJ_PI / (float) o->team_bots;
    const float rx = fminf(half.x * 0.5f, OBJ_SPREAD_MAX), rz = fminf(half.z * 0.5f, OBJ_SPREAD_MAX);
    feet.x += cosf(a) * rx;
    feet.z += sinf(a) * rz;
    return feet;
}

// The feet position under a flag: its prefab sits flagScale / 2 + flagOff under the crystal at
// base, a dropped one flagOff over the carrier's feet.
static vec3 obj_flag_feet(const ModeFlag *flag) {
    const int at_base = flag->position.x == flag->origin.x && flag->position.y == flag->origin.y && flag->position.z == flag->origin.z;
    const vec3 p = flag->position;
    return (vec3) {p.x, p.y - (at_base ? MODE_FLAG_SCALE * 0.5f + MODE_FLAG_OFF : MODE_FLAG_OFF), p.z};
}

static vec3 obj_flag_base_feet(const ModeFlag *flag) {
    return (vec3) {flag->origin.x, flag->origin.y - MODE_FLAG_SCALE * 0.5f - MODE_FLAG_OFF, flag->origin.z};
}

// Home and nobody carries it (a carried flag keeps its position: only its ping follows the carrier).
static int obj_flag_at_base(const ModeFlag *flag) {
    return flag->carrier_uid < 0 && flag->position.x == flag->origin.x && flag->position.y == flag->origin.y && flag->position.z == flag->origin.z;
}

// The carrier's HUD ping, as feet.
static vec3 obj_ping_feet(const ModeFlag *flag) { return (vec3) {flag->ping.x, flag->ping.y - MODE_FLAG_HOFF, flag->ping.z}; }

// The capture zone of a flag (mode_tick_flags).
static void obj_flag_zone(const ModeFlag *flag, vec3 *center, vec3 *half) {
    *center = (vec3) {flag->origin.x, flag->origin.y - MODE_FLAG_SCALE * 0.5f - MODE_FLAG_OFF + MODE_FLAG_ZONE_HEIGHT * 0.5f, flag->origin.z};
    *half = (vec3) {MODE_FLAG_ZONE_SIZE * 0.5f, MODE_FLAG_ZONE_HEIGHT * 0.5f, MODE_FLAG_ZONE_SIZE * 0.5f};
}

static const Player *obj_nearest(const Game *game, const Player *self, const int enemies, float *distance) {
    const Player *best = NULL;
    float best_d = 0.0f;
    for (size_t i = 0; i < game->player_count; i++) {
        const Player *o = game->players[i];
        if (!(enemies ? obj_enemy(game, self, o) : obj_mate(game, self, o))) continue;
        const float d = obj_d3d(o->position, self->position);
        if (!best || d < best_d) {
            best = o;
            best_d = d;
        }
    }
    if (distance) *distance = best_d;
    return best;
}

// ---------------------------------------------------------------- per mode

// CTF / Blitz: roles by rank (one defender from two bots on, two from five), the nearest bot
// claims a dropped flag or the enemy carrier, a carrier runs home.
static int think_ctf(Game *game, GameBot *bot, Player *self, BotObjective *o) {
    const GameMode *mode = game->mode;
    const ModeFlag *own = NULL, *enemy = NULL;
    int own_i = -1, enemy_i = -1;
    for (int f = 0; f < mode->flag_count; f++) {
        const ModeFlag *flag = &mode->flags[f];
        if (!flag->active || flag->neutral) continue;
        if (flag->team == self->team) {
            if (!own) { own = flag; own_i = f; }
        } else if (!enemy || obj_d3d(flag->position, self->position) < obj_d3d(enemy->position, self->position)) {
            enemy = flag;
            enemy_i = f;
        }
    }
    if (!own || !enemy) return 0;

    const Player *own_carrier = own->carrier_uid >= 0 ? game_find_player(game, own->carrier_uid) : NULL;

    // carrying: home (Blitz captures whatever our flag does). Our flag away: return it when it lies
    // nearest to us; the enemy holds it: wait by the base while a teammate bot hunts their carrier,
    // and hunt it ourselves when nobody can or the wait drags on (a both-flags-taken stalemate)
    if (self->mode.flag) {
        o->role = BOT_ROLE_ATTACK;
        if (!mode->config.rush && !obj_flag_at_base(own)) {
            if (!own_carrier && obj_nearest_bot_is_me(game, bot, self, own->position)) {
                obj_set_goal(o, BOT_GOAL_FLAG, own_i, obj_flag_feet(own), 0);
                return 1;
            }
            int free_mates = 0;
            for (int i = 0; game->bots && i < game->bots->count; i++) {
                const Player *p = game->bots->list[i].player;
                free_mates += p && p != self && p->active && !p->mode.flag && p->team == self->team;
            }
            if (own_carrier && own_carrier->active && (!free_mates || o->stalemate_ms >= OBJ_STALEMATE_MS)) {
                obj_set_goal(o, BOT_GOAL_CARRIER, own_i, obj_ping_feet(own), 0);
                return 1;
            }
        }
        vec3 center, half;
        obj_flag_zone(own, &center, &half);
        obj_set_goal(o, BOT_GOAL_HOME, own_i, obj_flag_base_feet(own), !obj_flag_at_base(own) && !mode->config.rush);
        obj_set_zone(o, center, half);
        obj_set_look(o, enemy->origin);
        return 1;
    }

    const Player *enemy_carrier = enemy->carrier_uid >= 0 ? game_find_player(game, enemy->carrier_uid) : NULL;
    // defenders by team size (humans count: they attack or defend on their own): none in 1v1 /
    // 2v2, where a guard leaves a lone attacker against a guarded base and nobody ever scores
    int team_size = 0;
    for (size_t i = 0; i < game->player_count; i++) team_size += game->players[i] && game->players[i]->team == self->team;
    const int defenders = MIN(team_size >= 6 ? 2 : team_size >= 3 ? 1 : 0, o->team_bots - 1);
    const int defender = o->rank < defenders;
    o->role = defender ? BOT_ROLE_DEFEND : BOT_ROLE_ATTACK;

    // our flag lies dropped: the nearest bot returns it
    if (!own_carrier && !obj_flag_at_base(own) && obj_nearest_bot_is_me(game, bot, self, own->position)) {
        obj_set_goal(o, BOT_GOAL_FLAG, own_i, obj_flag_feet(own), 0);
        return 1;
    }
    // an enemy carries our flag: the defenders and the bot nearest to its ping intercept
    if (own_carrier && own_carrier->active && (defender || obj_nearest_bot_is_me(game, bot, self, own->ping))) {
        obj_set_goal(o, BOT_GOAL_CARRIER, own_i, obj_ping_feet(own), 0);
        return 1;
    }
    // a teammate carries theirs: the attackers escort it
    if (enemy_carrier && enemy_carrier->active && enemy_carrier->team == self->team && !defender) {
        o->role = BOT_ROLE_SUPPORT;
        obj_set_goal(o, BOT_GOAL_ESCORT, enemy_carrier->uid, enemy_carrier->position, 0);
        return 1;
    }
    if (defender) {
        // a spot between our flag and theirs, facing them
        const vec3 base = obj_flag_base_feet(own);
        const float dx = enemy->origin.x - base.x, dz = enemy->origin.z - base.z;
        const float len = hypotf(dx, dz);
        const float off = OBJ_GUARD_OFFSET * (o->rank == 1 ? 0.5f : 1.0f);
        const vec3 spot = len > 1.0f ? (vec3) {base.x + dx / len * off, base.y, base.z + dz / len * off} : base;
        obj_set_goal(o, BOT_GOAL_GUARD, own_i, spot, 1);
        obj_set_look(o, enemy->origin);
        return 1;
    }
    // attacker: their flag, wherever it lies
    obj_set_goal(o, BOT_GOAL_FLAG, enemy_i, obj_flag_feet(enemy), 0);
    return 1;
}

// Team Defender / Carrier: the neutral flag. Carriers score over time: keep it away from the
// enemy (team: by our spawns; FFA: a far nav point), the rest escort it or chase the enemy carrier.
static int think_oneflag(Game *game, GameBot *bot, Player *self, BotObjective *o) {
    const GameMode *mode = game->mode;
    const ModeFlag *flag = NULL;
    int index = -1;
    for (int f = 0; f < mode->flag_count; f++) {
        if (mode->flags[f].active && mode->flags[f].neutral) { flag = &mode->flags[f]; index = f; break; }
    }
    if (!flag) return 0;

    if (self->mode.flag) {
        o->role = BOT_ROLE_ATTACK;
        float enemy_d = 0.0f;
        const Player *enemy = obj_nearest(game, self, 1, &enemy_d);
        vec3 base;
        if (obj_teams(game) && obj_spawn_centroid(game, self->team, &base)) {
            obj_set_goal(o, BOT_GOAL_KITE, index, base, 1);
        } else {
            // keep the current hiding spot while it is still far from the enemy, else a new one
            const int keep = o->kind == BOT_GOAL_KITE && o->hold_time < OBJ_KITE_MS && (!enemy || obj_d3d(o->goal, enemy->position) > 200.0f);
            if (!keep) {
                vec3 best = self->position;
                float best_score = -1.0f;
                for (int i = 0; i < 6; i++) {
                    vec3 p;
                    if (!bot_nav_random_point_from(game, &self->position, &p)) break;
                    float score = 0.0f;
                    for (size_t k = 0; k < game->player_count; k++) {
                        const Player *e = game->players[k];
                        if (!obj_enemy(game, self, e)) continue;
                        const float d = obj_d3d(p, e->position);
                        if (score == 0.0f || d < score) score = d;
                    }
                    if (score == 0.0f) score = 1.0f;
                    score -= obj_d3d(p, self->position) * 0.3f; // not too far a run with the flag
                    if (score > best_score) { best_score = score; best = p; }
                }
                obj_set_goal(o, BOT_GOAL_KITE, index, best, 1);
            }
        }
        if (enemy) obj_set_look(o, enemy->position);
        return 1;
    }

    const Player *carrier = flag->carrier_uid >= 0 ? game_find_player(game, flag->carrier_uid) : NULL;
    if (carrier && carrier->active && obj_mate(game, self, carrier)) {
        o->role = BOT_ROLE_SUPPORT;
        obj_set_goal(o, BOT_GOAL_ESCORT, carrier->uid, carrier->position, 0);
        return 1;
    }
    o->role = BOT_ROLE_ATTACK;
    if (carrier && carrier->active) {
        obj_set_goal(o, BOT_GOAL_CARRIER, index, obj_ping_feet(flag), 0);
        return 1;
    }
    obj_set_goal(o, BOT_GOAL_FLAG, index, obj_flag_feet(flag), 0);
    return 1;
}

// Hardpoint / King of the Hill: everyone on the active objective (spread over it).
static int think_point(Game *game, GameBot *bot, Player *self, BotObjective *o) {
    (void) bot;
    const ModeObjective *obj = mode_objective_by_uid(game, game->mode->active_objective);
    if (!obj) return 0;
    o->role = BOT_ROLE_ATTACK;
    obj_set_goal(o, BOT_GOAL_HILL, obj->uid, obj_zone_spot(obj->position, obj->half, o), 1);
    obj_set_zone(o, obj->position, obj->half);
    vec3 threat;
    if (obj_threat_point(game, self, &threat)) obj_set_look(o, threat);
    return 1;
}

// Domination: the zones the team does not own, one per bot by rank (all owned: defend them).
static int think_dom(Game *game, GameBot *bot, Player *self, BotObjective *o) {
    (void) bot;
    const GameMode *mode = game->mode;
    if (!mode->dom_zone_count) return 0;
    int open[MODE_MAX_DOM_ZONES], open_n = 0;
    for (int z = 0; z < mode->dom_zone_count; z++) {
        if (mode->dom_zones[z].captured_by != self->team) open[open_n++] = z;
    }
    const int z = open_n ? open[o->rank % open_n] : o->rank % mode->dom_zone_count;
    const ModeDomZone *zone = &mode->dom_zones[z];
    o->role = open_n ? BOT_ROLE_ATTACK : BOT_ROLE_DEFEND;
    obj_set_goal(o, BOT_GOAL_ZONE, z, obj_zone_spot(zone->position, zone->half, o), 1);
    obj_set_zone(o, zone->position, zone->half);
    // face the nearest zone the enemy holds, else their spawns
    const ModeDomZone *threat = NULL;
    for (int k = 0; k < mode->dom_zone_count; k++) {
        const ModeDomZone *other = &mode->dom_zones[k];
        if (k == z || !other->captured_by || other->captured_by == self->team) continue;
        if (!threat || obj_d3d(other->position, zone->position) < obj_d3d(threat->position, zone->position)) threat = other;
    }
    vec3 at;
    if (threat) obj_set_look(o, threat->position);
    else if (obj_threat_point(game, self, &at)) obj_set_look(o, at);
    return 1;
}

// The nearest tag spawnable worth the walk: Kill Confirmed tags (any team), KR Tags (the
// enemy's, ours only to deny when close). -1 none.
static int obj_nearest_tag(const Game *game, const Player *self, const int kr, vec3 *feet) {
    const GameMode *mode = game->mode;
    int best = -1;
    float best_d = OBJ_TAG_RANGE;
    for (int s = 0; s < MODE_MAX_SPAWNABLES; s++) {
        const ModeSpawnable *sp = &mode->spawnables[s];
        if (!sp->active) continue;
        if (kr ? sp->type != SPAWNABLE_KR_TAG && sp->type != SPAWNABLE_DIAMOND_EGG : sp->type != SPAWNABLE_TAG) continue;
        const float d = obj_d3d(sp->position, self->position);
        if (kr && obj_teams(game) && sp->team == self->team && d > OBJ_TAG_DENY_RANGE) continue;
        if (kr && obj_teams(game) && sp->team != self->team && self->mode.dog_tags >= mode->config.tag_hold_limit) continue; // full
        if (d >= best_d) continue;
        best = s;
        best_d = d;
        const SpawnableInfo *info = spawnable_info(sp->type);
        *feet = (vec3) {sp->position.x, sp->position.y - (info ? info->y_off : 0.0f), sp->position.z};
    }
    return best;
}

// Deposit / Deposit FFA: collect KR Tags, deposit a handful (or a full load) on the objective.
static int think_deposit(Game *game, GameBot *bot, Player *self, BotObjective *o) {
    (void) bot;
    const GameMode *mode = game->mode;
    const ModeObjective *obj = mode_objective_by_uid(game, mode->active_objective);
    vec3 tag_feet;
    const int tag = obj_nearest_tag(game, self, 1, &tag_feet);
    const int tags = self->mode.dog_tags;
    const int full = mode->config.tag_hold_limit > 0 && tags >= mode->config.tag_hold_limit;
    o->role = BOT_ROLE_ATTACK;
    if (tags > 0 && obj && (full || tags >= OBJ_DEPOSIT_TAGS || tag < 0 || obj_d3d(tag_feet, self->position) > 200.0f)) {
        obj_set_goal(o, BOT_GOAL_DEPOSIT, obj->uid, obj_zone_spot(obj->position, obj->half, o), 1);
        obj_set_zone(o, obj->position, obj->half);
        vec3 threat;
        if (obj_threat_point(game, self, &threat)) obj_set_look(o, threat);
        return 1;
    }
    if (tag >= 0) {
        obj_set_goal(o, BOT_GOAL_TAG, mode->spawnables[tag].sid, tag_feet, 0);
        return 1;
    }
    return 0;
}

// Kill Confirmed: the nearest tag (confirm theirs, deny ours).
static int think_tags(Game *game, GameBot *bot, Player *self, BotObjective *o) {
    (void) bot;
    vec3 feet;
    const int tag = obj_nearest_tag(game, self, 0, &feet);
    if (tag < 0) return 0;
    o->role = BOT_ROLE_ATTACK;
    obj_set_goal(o, BOT_GOAL_TAG, game->mode->spawnables[tag].sid, feet, 0);
    return 1;
}

// Defuse (shared/src/defuse.c): the bomb is the map's bomb pickup (weapon 17). Attackers (team 2): the carrier
// takes it to the nearest site and plants it (bot_objective_defuse_hold), the nearest bot goes for a bomb lying
// on the floor, the others push onto the sites by rank, and once it is planted they guard it. Defenders (team 1)
// hold the sites by rank, and once it is planted they all go for it and defuse it. A player's HUD shows as much:
// the attackers' marker over the bomb, everyone's beeps at the planted bomb.
static int obj_carries_bomb(const Player *p) {
    for (int i = 0; p->loadout && i < p->loadout_size; i++) {
        if (p->loadout[i] == DEFUSE_BOMB_WEAPON) return 1;
    }
    return 0;
}

// The bomb's pickup (uid, -1 none) and where it lies; *planted: inactive (on a site), else lying when it holds the bomb.
static int obj_bomb(const Game *game, vec3 *feet, int *lying, int *planted) {
    *lying = *planted = 0;
    for (size_t i = 0; game->map && i < game->map->object_count; i++) {
        GamePickup k;
        if (!game->map->objects[i]->pickup || !game_pickup_state(game, (int) i, &k) || k.weapon != DEFUSE_BOMB_WEAPON) continue;
        *lying = k.active;
        *planted = !k.active;
        // a dropped bomb lies at its drop position (y + 1 over the feet), a bomb at home on the map's spot
        *feet = (vec3) {k.position.x, k.position.y - (k.position.y != k.origin.y ? 1.0f : 0.0f), k.position.z};
        return (int) i;
    }
    return -1;
}

// lying 2, planted 4, else 0 (carried, or no bomb), + 8 for a bomb this bot holds: a change re-thinks at once
static int obj_bomb_state(const Game *game, const Player *self) {
    if (!game->mode || !game->mode->config.site) return 0;
    vec3 feet;
    int lying, planted;
    const int uid = obj_bomb(game, &feet, &lying, &planted);
    return (uid < 0 ? 0 : lying ? 2 : planted ? 4 : 0) | (obj_carries_bomb(self) ? 8 : 0);
}

static int think_site(Game *game, GameBot *bot, Player *self, BotObjective *o) {
    const Map *map = game->map;
    const Object *sites[8];
    int n = 0;
    for (size_t i = 0; map && i < map->object_count && n < 8; i++) {
        if (map->objects[i]->bomb_site) sites[n++] = map->objects[i];
    }
    if (!n) return 0;
    vec3 bomb_feet = {0.0f, 0.0f, 0.0f};
    int lying = 0, planted = 0;
    const int bomb = obj_bomb(game, &bomb_feet, &lying, &planted);
    o->last_bomb = obj_bomb_state(game, self);

    // the planted bomb: the defenders defuse it (the first one there holds M1, the others stand by it), the
    // attackers guard it from a few units away
    if (bomb >= 0 && planted) {
        const vec3 center = {bomb_feet.x, bomb_feet.y + 2.0f, bomb_feet.z};
        if (self->team == DEFUSE_DEFENDERS) {
            o->role = BOT_ROLE_DEFEND;
            obj_set_goal(o, BOT_GOAL_BOMB, bomb, bomb_feet, 1);
            obj_set_zone(o, center, (vec3) {6.0f, 4.0f, 3.0f}); // inside the pickup's GameObject (x +- 12, z +- 4)
            return 1;
        }
        o->role = BOT_ROLE_DEFEND;
        obj_set_goal(o, BOT_GOAL_GUARD, bomb, obj_zone_spot(center, (vec3) {OBJ_GUARD_OFFSET, 2.0f, OBJ_GUARD_OFFSET}, o), 1);
        vec3 threat;
        if (obj_threat_point(game, self, &threat)) obj_set_look(o, threat);
        return 1;
    }

    // the carrier: the nearest site
    if (self->team == DEFUSE_ATTACKERS && obj_carries_bomb(self)) {
        const Object *site = sites[0];
        for (int s = 1; s < n; s++) {
            if (obj_d2d(sites[s]->position, self->position) < obj_d2d(site->position, self->position)) site = sites[s];
        }
        const vec3 size = object_box_size(site);
        const vec3 half = {size.x * 0.5f, size.y * 0.5f, size.z * 0.5f};
        const vec3 center = {site->position.x, site->position.y + half.y, site->position.z};
        o->role = BOT_ROLE_ATTACK;
        // the centre of the site, whatever the rank: the plant is the whole job
        obj_set_goal(o, BOT_GOAL_SITE, (int) (site - map->objects[0]), (vec3) {center.x, center.y - half.y, center.z}, 1);
        obj_set_zone(o, center, (vec3) {half.x * 0.7f, half.y, half.z * 0.7f});
        return 1;
    }

    // a bomb lying on the floor: the nearest attacker bot takes it
    if (self->team == DEFUSE_ATTACKERS && bomb >= 0 && lying && obj_nearest_bot_is_me(game, bot, self, bomb_feet)) {
        o->role = BOT_ROLE_ATTACK;
        obj_set_goal(o, BOT_GOAL_BOMB, bomb, bomb_feet, 0);
        return 1;
    }

    const Object *site = sites[o->rank % n];
    const vec3 size = object_box_size(site);
    const vec3 half = {size.x * 0.5f, size.y * 0.5f, size.z * 0.5f};
    const vec3 center = {site->position.x, site->position.y + half.y, site->position.z};
    o->role = self->team == DEFUSE_DEFENDERS ? BOT_ROLE_DEFEND : BOT_ROLE_ATTACK;
    obj_set_goal(o, BOT_GOAL_SITE, (int) (site - map->objects[0]), obj_zone_spot(center, half, o), 1);
    obj_set_zone(o, center, half);
    vec3 threat;
    if (obj_threat_point(game, self, &threat)) obj_set_look(o, threat);
    return 1;
}

// Lives modes (Last Man Standing, Traitor, Prop Hunt, ...): clear the remaining enemies, or
// regroup with a teammate when outnumbered. The enemy position is the server's (2 s stale).
static int think_elimination(Game *game, GameBot *bot, Player *self, BotObjective *o) {
    (void) bot;
    float enemy_d = 0.0f, mate_d = 0.0f;
    const Player *enemy = obj_nearest(game, self, 1, &enemy_d);
    const Player *mate = obj_nearest(game, self, 0, &mate_d);
    if (!enemy) return 0;
    int enemies = 0, mates = 1;
    for (size_t i = 0; i < game->player_count; i++) {
        enemies += obj_enemy(game, self, game->players[i]);
        mates += obj_mate(game, self, game->players[i]);
    }
    o->think_timer = OBJ_HUNT_THINK_MS;
    if (mate && enemies > mates && mate_d > 30.0f) {
        o->role = BOT_ROLE_SUPPORT;
        obj_set_goal(o, BOT_GOAL_REGROUP, mate->uid, mate->position, 0);
        return 1;
    }
    o->role = BOT_ROLE_ATTACK;
    obj_set_goal(o, BOT_GOAL_HUNT, enemy->uid, enemy->position, 0);
    return 1;
}

static int obj_think(Game *game, GameBot *bot, Player *self, BotObjective *o) {
    const GameMode *mode = game->mode;
    const GameModeConfig *cfg = &mode->config;
    obj_rank(game, bot, self, o);
    o->think_timer = obj_rand(OBJ_THINK_MIN_MS, OBJ_THINK_MAX_MS);
    o->last_flag = self->mode.flag;
    o->last_tags = self->mode.dog_tags;
    if (cfg->flags && !cfg->oneflag) return think_ctf(game, bot, self, o);
    if (cfg->oneflag) return think_oneflag(game, bot, self, o);
    if (cfg->dom) return think_dom(game, bot, self, o);
    if (cfg->deposit) return think_deposit(game, bot, self, o);
    if (cfg->objective) return think_point(game, bot, self, o);
    if (cfg->id == MODE_ID_KC) return think_tags(game, bot, self, o);
    if (cfg->site) return think_site(game, bot, self, o);
    if (mode->lives > 0 || cfg->conv_team) return think_elimination(game, bot, self, o); // Infected: chase / regroup too
    return 0;
}

// ---------------------------------------------------------------- public

static int obj_arrived(const Player *self, const BotObjective *o) {
    if (o->has_zone) return mode_player_collides(self, o->zone_center, o->zone_half);
    return obj_d2d(self->position, o->goal) < OBJ_ARRIVE_D && fabsf(self->position.y - o->goal.y) < 6.0f;
}

int bot_objective_update(Game *game, GameBot *bot, Player *self, const float dt) {
    BotObjective *o = &bot->objective;
    if (!game || !game->mode || !game->map || !game->bots || !self) return 0;
    if (o->fail_timer > 0.0f) {
        o->fail_timer -= dt;
        if (o->fail_timer > 0.0f) return 0;
    }
    o->think_timer -= dt;
    const int changed = o->last_flag != self->mode.flag || o->last_tags != self->mode.dog_tags || o->last_bomb != obj_bomb_state(game, self);
    if (o->think_timer <= 0.0f || o->kind == BOT_GOAL_NONE || changed) {
        if (!obj_think(game, bot, self, o)) {
            o->kind = BOT_GOAL_NONE;
            o->role = BOT_ROLE_NONE;
            return 0;
        }
    }
    // the route to it ends short of it (unreachable from here): roam for a while, then try the
    // other spot
    if (bot->path_stranded && obj_d3d(bot->path_goal, o->goal) <= 40.0f) {
        o->fail_timer = OBJ_FAIL_MS;
        o->alt = !o->alt;
        o->kind = BOT_GOAL_NONE;
        return 0;
    }
    o->arrived = (unsigned char) obj_arrived(self, o);
    if (o->arrived) o->hold_time += dt;
    else o->hold_time = 0.0f;
    // Defuse: at the bomb lying on the floor, the attacker takes it (the interact key's "int" 6)
    if (o->kind == BOT_GOAL_BOMB && self->team == DEFUSE_ATTACKERS && game_player_in_pickup(game, self, o->index)) {
        GamePickup k;
        if (game_pickup_state(game, o->index, &k) && k.active && k.weapon == DEFUSE_BOMB_WEAPON) game_player_interact_v7(game, self, GAME_INTERACT_PICKUP, o->index, 0);
    }
    // the point is contested: an enemy standing in the volume we hold is known to us (the HUD's
    // contested state tells a player as much), wherever we are looking. Without it two holders on one
    // zone could sweep their looks away from each other for good and the zone never fell
    if (o->arrived && o->hold && o->has_zone) {
        for (size_t i = 0; i < game->player_count; i++) {
            const Player *p = game->players[i];
            if (p != self && obj_alive(p) && obj_enemy(game, self, p) && mode_player_collides(p, o->zone_center, o->zone_half)) {
                bot_sense_seed(&bot->sense, p, p->position, BOT_STIMULUS_HEARD);
            }
        }
    }
    if (!self->mode.flag) o->stalemate_ms = 0.0f;
    else if ((o->kind == BOT_GOAL_HOME && o->arrived && o->hold) || o->kind == BOT_GOAL_CARRIER) o->stalemate_ms += dt;
    return 1;
}

float bot_objective_hold_yaw(GameBot *bot, const Player *self, const float dt) {
    BotObjective *o = &bot->objective;
    o->look_timer -= dt;
    if (o->look_timer <= 0.0f) {
        o->look_timer = obj_rand(OBJ_LOOK_SWEEP_MS * 0.7f, OBJ_LOOK_SWEEP_MS * 1.3f);
        o->look_offset = o->look_offset > 0.1f ? -OBJ_LOOK_SWEEP_RAD : o->look_offset < -0.1f ? 0.0f : OBJ_LOOK_SWEEP_RAD;
    }
    if (!o->has_look) return bot->yaw + o->look_offset * 0.02f; // nothing to face: drift
    // UTILS.getDrs yaw from self to the look point
    const float p = self->position.x - o->look.x, u = self->position.z - o->look.z;
    return (float) -atan2(-p, u) + o->look_offset;
}

vec3 bot_objective_hold_spot(Game *game, GameBot *bot, const Player *self, const float dt) {
    BotObjective *o = &bot->objective;
    o->wander_timer -= dt;
    const int reached = o->has_wander && obj_d2d(self->position, o->wander) < 3.0f;
    if (o->has_wander && !reached && o->wander_timer > 0.0f) return o->wander;
    // the area to pick in: the volume less an inset (a narrow one: its middle), else around the goal
    float x0, x1, z0, z1;
    if (o->has_zone) {
        const float ix = fminf(OBJ_WANDER_INSET, o->zone_half.x * 0.4f), iz = fminf(OBJ_WANDER_INSET, o->zone_half.z * 0.4f);
        x0 = o->zone_center.x - o->zone_half.x + ix, x1 = o->zone_center.x + o->zone_half.x - ix;
        z0 = o->zone_center.z - o->zone_half.z + iz, z1 = o->zone_center.z + o->zone_half.z - iz;
    } else {
        x0 = o->goal.x - OBJ_WANDER_GOAL, x1 = o->goal.x + OBJ_WANDER_GOAL;
        z0 = o->goal.z - OBJ_WANDER_GOAL, z1 = o->goal.z + OBJ_WANDER_GOAL;
    }
    // near the bot, on its floor, on the walkable grid (a spot in a wall would strand the route)
    const float nx0 = fmaxf(x0, self->position.x - OBJ_WANDER_REACH), nx1 = fminf(x1, self->position.x + OBJ_WANDER_REACH);
    const float nz0 = fmaxf(z0, self->position.z - OBJ_WANDER_REACH), nz1 = fminf(z1, self->position.z + OBJ_WANDER_REACH);
    vec3 pick = o->goal;
    for (int tries = 0; tries < 10; tries++) {
        const int near = tries < 7 && nx0 < nx1 && nz0 < nz1;
        vec3 p = {near ? obj_rand(nx0, nx1) : obj_rand(x0, x1), self->position.y + 2.0f, near ? obj_rand(nz0, nz1) : obj_rand(z0, z1)};
        if (obj_d2d(p, self->position) < 6.0f) continue; // a step that moves it
        float ground;
        if (!bot_nav_ground(game, p, 8.0f, &ground)) continue;
        p.y = ground;
        if (o->has_zone && (p.y > o->zone_center.y + o->zone_half.y || p.y + self->height < o->zone_center.y - o->zone_half.y)) continue;
        if (!bot_nav_on_grid(game, p) || bot_hazard_at(game, p)) continue;
        pick = p;
        break;
    }
    o->wander = pick;
    o->has_wander = 1;
    o->wander_timer = obj_rand(OBJ_WANDER_MIN_MS, OBJ_WANDER_MAX_MS);
    return pick;
}

int bot_objective_over_combat(Game *game, GameBot *bot, Player *self, const Player *target, const float dt, vec3 *goal) {
    BotObjective *o = &bot->objective;
    if (!game || !game->mode || !game->map || !game->bots || !target || o->fail_timer > 0.0f) return 0;
    // a flag picked up / tags collected on the way: the goal changes at once (the carrier runs
    // home); otherwise the decision Hunt made stands for the fight
    o->think_timer -= dt;
    if (o->last_flag != self->mode.flag || o->last_tags != self->mode.dog_tags || o->last_bomb != obj_bomb_state(game, self) ||
        (o->kind != BOT_GOAL_NONE && o->think_timer <= -OBJ_THINK_MAX_MS)) {
        if (!obj_think(game, bot, self, o)) o->kind = BOT_GOAL_NONE;
    }
    if (o->kind == BOT_GOAL_NONE) return 0;
    const float range = obj_d3d(target->position, self->position);
    const int stalled = bot->state_time > OBJ_STALL_MS; // in this Attack (bots.c state_time) without a result
    // under fire from a player in fight range: the fight comes first, stalled or not (the attacker is
    // the threat). A carrier keeps running home / kiting; the attacker out of range is the objective's.
    const float hit_age = bot_sense_hit_age(&bot->sense, target->uid);
    const int under_fire = hit_age >= 0.0f && hit_age < OBJ_UNDER_FIRE_MS;
    if (under_fire && range <= OBJ_CHASE_RANGE && o->kind != BOT_GOAL_HOME && o->kind != BOT_GOAL_CARRIER && o->kind != BOT_GOAL_KITE && o->kind != BOT_GOAL_DEPOSIT) return 0;
    // a threat out of sight that is not shooting at us is only remembered (bot_sense's memory keeps
    // it for the search): the objective comes first, the bot does not leave it to go looking. Without
    // this the longer memory kept CTF attackers searching for enemies they had lost (twice the Attack
    // time of main in bot_objective_test, flags no longer captured)
    const int quiet = !bot->sees_target && !under_fire;
    switch (o->kind) {
        case BOT_GOAL_HOME: // a carrier never stops to fight: it shoots on the run
        case BOT_GOAL_KITE:
            if (!self->mode.flag) return 0;
            *goal = o->goal;
            return 1;
        case BOT_GOAL_DEPOSIT:
            if (self->mode.dog_tags < 3 || range <= OBJ_HOLD_FIGHT_RANGE) return 0;
            *goal = o->goal;
            return 1;
        case BOT_GOAL_BOMB: // Defuse: taking it is on the way (below), defusing / guarding it a hold
            if (!o->hold) {
                if (!quiet && range <= OBJ_CHASE_RANGE && !stalled) return 0;
                *goal = o->goal;
                return 1;
            }
            // fall through
        case BOT_GOAL_HILL: // stay on the point unless the enemy is close
        case BOT_GOAL_ZONE:
        case BOT_GOAL_SITE:
        case BOT_GOAL_GUARD:
            if (!quiet && (obj_arrived(self, o) ? range <= OBJ_HOLD_FIGHT_RANGE : range <= OBJ_CHASE_RANGE && !stalled)) return 0;
            // on it: keep moving inside it while shooting (a strafe), not back to one spot
            *goal = o->hold && obj_arrived(self, o) ? bot_objective_hold_spot(game, bot, self, dt) : o->goal;
            return 1;
        case BOT_GOAL_HUNT: // the enemy is the goal
        case BOT_GOAL_NONE:
            return 0;
        default: // on the way to a flag / carrier / tag: a far enemy is shot at on the move, a close one fought
            if (!quiet && range <= OBJ_CHASE_RANGE && !stalled) return 0;
            *goal = o->goal;
            return 1;
    }
}

int bot_objective_defuse_hold(Game *game, GameBot *bot, Player *self, int *swap) {
    (void) bot;
    *swap = 0;
    if (!game || !game->mode || !game->mode->config.site || !self || !self->active) return 0;
    const int in_hand = self->weapon && self->weapon->bomb;
    if (self->team == DEFUSE_ATTACKERS && obj_carries_bomb(self)) {
        if (defuse_site_at(game, self) >= 0 && self->on_ground) {
            if (in_hand) return 1;
            if (self->swap_timer <= 0.0f) *swap = 4; // swapEquipment: the bomb in hand, then M1
            return 2;
        }
        if (in_hand && self->swap_timer <= 0.0f) *swap = 3; // not on a site: the primary, to fight on the way
        return 0;
    }
    return self->team == DEFUSE_DEFENDERS && defuse_bomb_pickup_at(game, self) >= 0;
}

const char *bot_role_name(const BotRole role) {
    switch (role) {
        case BOT_ROLE_ATTACK: return "attack";
        case BOT_ROLE_DEFEND: return "defend";
        case BOT_ROLE_SUPPORT: return "support";
        default: return "none";
    }
}

const char *bot_goal_name(const BotGoalKind kind) {
    static const char *const k_names[] = {"none", "flag", "home", "carrier", "escort", "guard", "kite", "hill", "zone", "tag", "deposit", "site", "hunt", "regroup",
                                          "bomb"};
    return kind >= 0 && (size_t) kind < sizeof(k_names) / sizeof(k_names[0]) ? k_names[kind] : "none";
}
