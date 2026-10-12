// Krunker v3.1.1 AI: src/data/ai.js (the AI entity and its manager: spawn, update, sync,
// changeHealth, kill, collides), objects.js addAI / addPendingAIs (map "BOT" objects and
// their limits), map.js addAI options, players.js findNearestTo, game.js canSee,
// server/game.ts (Soul Sanctum summon templates, spawnSoulSanctumAI) and
// server/gameserver.ts (AI.sync every 5 server ticks). See shared/include/ai.h.
//
// Sounds: idl / atk / dth and the bosses' 3236 are user-asset sounds ("asset:<id>" ->
// assets/user-assets/<id>/sound.mp3; the v3.1.1 offline build ships 3206 and 3, the others
// are silent there too). onAIDeath runs game_trigger_ai_death (shared/src/triggers.c).
// Not ported: AI skin rewards (rew: no inventory natively) and user-asset AI models (aid).
#include <ai.h>
#include <map_catalog.h>
#include <modes.h>
#include <pcg_basic.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AI_PI 3.14159265358979323846
#define AI_HIT_DAMAGE 25.0f   // hitDMG
#define AI_HIT_RATE 800.0f    // hitRate (ms)
#define AI_IDLE_SOUND_INTERVAL 5000.0f // idlSoundI (ms)
#define AI_BOSS_SOUND 3236    // the bosses' teleport / summon sound (user asset)

// ai.js `c`: the built-in kinds (0 Traitor ghost, 1 Lost Soul, 2 Jack O' Lantern).
static const GameAIData k_builtin[] = {
    {.src = "ghost_0", .glow = 1, .bob_d = 1.0f, .bob_s = 0.005f, .m_size = 4.0f},
    {.src = "ghost_0", .glow = 1, .frames = 2, .can_shoot = 1, .vision = 500, .projectile = 1, .health = 300.0f,
     .score = 10, .speed = 0.8f, .fire_rate = 2500, .turn_speed = 20.0f, .m_size = 4.0f},
    {.src = "pumpkin_0", .glow = 1, .no_frames = 1, .can_hit = 1, .vision = 500, .health = 700.0f, .score = 10,
     .speed = 0.6f, .turn_speed = 4.0f, .m_size = 4.0f},
};
#define AI_BUILTIN_COUNT ((int) (sizeof(k_builtin) / sizeof(k_builtin[0])))

// projectiles.stored[1..4] (projectile.js); [0] is the thrown knife (projectile.c).
static const ProjectileConfig k_ai_projectiles[GAME_AI_PROJECTILE_KINDS] = {
    {0},
    {.speed = 0.05f, .gravity = 0.0f, .hit_box = 10.0f, .damage = 60.0f, .range = 500.0f},
    {.speed = 0.25f, .gravity = 0.0f, .damage = 20.0f, .range = 500.0f},
    {.speed = 0.03f, .gravity = 0.0f, .hit_box = 20.0f, .damage = 200.0f, .range = 600.0f},
    {.speed = 0.25f, .gravity = 0.0f, .damage = 10.0f, .range = 500.0f},
};

const ProjectileConfig *game_ai_projectile_config(const int kind) {
    return kind > 0 && kind < GAME_AI_PROJECTILE_KINDS ? &k_ai_projectiles[kind] : NULL;
}

// ---------------------------------------------------------------- helpers

static double ai_round(const double value, const int digits) { // Number.prototype.round
    const double p = pow(10.0, digits);
    return floor(value * p + 0.5) / p; // Math.round: halves round up
}

static float ai_rand_float(const float min, const float max) { // UTILS.randFloat
    return min + (float) ldexp((double) pcg32_random(), -32) * (max - min);
}

static int ai_rand_int(const int min, const int max) { // UTILS.randInt (inclusive)
    return min + (int) pcg32_boundedrand((uint32_t) (max - min + 1));
}

static float ai_d3d(const vec3 a, const vec3 b) { // UTILS.getD3D
    const float x = a.x - b.x, y = a.y - b.y, z = a.z - b.z;
    return sqrtf(x * x + y * y + z * z);
}

static float ai_limit(const float value, const float min, const float max) { // UTILS.limitMM
    return value < min ? min : value > max ? max : value;
}

static void ai_copy(char *out, const size_t size, const char *in) {
    snprintf(out, size, "%s", in ? in : "");
}

// ---------------------------------------------------------------- map data

// JS parseInt(v) || 0 / parseFloat(v) || 0 over a JSON number or numeric string.
static double json_number(const cJSON *item, const int integer) {
    double v = NAN;
    if (cJSON_IsNumber(item)) v = cJSON_GetNumberValue(item);
    else if (cJSON_IsString(item)) v = strtod(cJSON_GetStringValue(item), NULL);
    else if (cJSON_IsBool(item)) v = NAN;
    if (v != v) return 0.0;
    return integer ? trunc(v) : v;
}

static int json_int(const cJSON *object, const char *key) {
    return (int) json_number(cJSON_GetObjectItem(object, key), 1);
}

static float json_float(const cJSON *object, const char *key) {
    return (float) json_number(cJSON_GetObjectItem(object, key), 0);
}

static int json_present(const cJSON *object, const char *key) { // OBJECT.k != null
    const cJSON *item = cJSON_GetObjectItem(object, key);
    return item && !cJSON_IsNull(item);
}

// objects.js addAI aiName check: /[^\w\:\;\(\)\?\,\.\|\[\] \-\+*\$\@\=\^!{}]+/
static int ai_name_char_ok(const unsigned char c) {
    return isalnum(c) || c == '_' || (c && strchr(":;()?,.|[] -+*$@=^!{}", c));
}

// map.js addAI trigger options + objects.js addAI / ai.js init: eventN = ct ? events[te || 15]
// : null and only "onAI" events survive (index 15); action, chance, constant, constantEvent,
// constantTxt (the addTrigger text check), the sound fields (es through config.networkSounds,
// esa an asset id; ev / er unclamped), interfaceT; singleUse is never passed, interface is 0.
static void ai_parse_trigger(const cJSON *object, TriggerData *trig) {
    memset(trig, 0, sizeof(*trig));
    const int te = json_int(object, "te");
    trig->event = json_present(object, "ct") && (te ? te : TRIGGER_EVENT_ON_AI_DEATH) == TRIGGER_EVENT_ON_AI_DEATH ? TRIGGER_EVENT_ON_AI_DEATH : TRIGGER_EVENT_NONE;
    trig->action = json_int(object, "ta");
    trig->chance = (int) ai_limit((float) json_int(object, "cha"), 0.0f, 100.0f);
    trig->constant = json_int(object, "tc");
    trig->constant_event = (int) ai_limit((float) json_int(object, "tce"), 0.0f, (float) TRIGGER_CONSTANT_EVENT_MAX);
    trig->interface_target = json_int(object, "tin");
    const cJSON *tct = cJSON_GetObjectItem(object, "tct");
    if (cJSON_IsString(tct) && trigger_text_valid(cJSON_GetStringValue(tct))) snprintf(trig->text, sizeof(trig->text), "%s", cJSON_GetStringValue(tct));
    trig->asset_sound = (unsigned char) json_present(object, "esa");
    trig->sound = json_int(object, trig->asset_sound ? "esa" : "es"); // parseInt(esa || es)
    if (trig->asset_sound && !trig->sound) trig->sound = json_int(object, "es");
    trig->network_sound = 1;
    const float volume = json_float(object, "ev"), rate = json_float(object, "er");
    trig->volume = volume ? volume : 1.0f;
    trig->rate = rate ? rate : 1.0f;
    trig->emit_from = json_int(object, "ef");
}

// map.js addAI options (game.ts buildAIDataFromBotObject is the same) and, with `limits`,
// objects.js addAI's clamps (map BOTs only: /summon passes the raw template data).
static void ai_parse_bot(const cJSON *object, GameAIData *out, char *name, const size_t name_size, const int limits) {
    memset(out, 0, sizeof(*out));
    const int asset = json_int(object, "aid");
    out->asset = asset;
    ai_copy(out->src, sizeof(out->src), asset ? "model" : "ghost_0");
    out->glow = 1;
    const cJSON *h = cJSON_GetObjectItem(object, "h");
    out->health = (float) json_number(h && json_number(h, 1) ? h : cJSON_GetObjectItem(object, "hp"), 1); // h || hp
    out->respawn_t = json_float(object, "rt") * 1000.0f;
    out->respawn_r = (unsigned char) json_present(object, "rr");
    out->speed = json_float(object, "spd");
    out->turn_speed = json_float(object, "tsd");
    out->gravity = json_float(object, "grv");
    out->fire_rate = json_int(object, "fr");
    out->vision = json_int(object, "vd");
    out->idle_sound = json_int(object, "idl");
    out->death_sound = json_int(object, "dth");
    out->attack_sound = json_int(object, "atk");
    out->score = json_int(object, "sp");
    out->projectile = json_int(object, "pj");
    out->behaviour = json_int(object, "bh");
    out->can_shoot = (unsigned char) json_present(object, "cs");
    out->can_hit = (unsigned char) json_present(object, "cnh");
    out->xray = (unsigned char) json_present(object, "xv");
    out->m_size = json_float(object, "ms"); // parseFloat: NaN (no "ms") -> 0 -> mSize || 4
    ai_parse_trigger(object, &out->trig);

    const cJSON *ain = cJSON_GetObjectItem(object, "ain");
    const char *ai_name = cJSON_IsString(ain) ? cJSON_GetStringValue(ain) : "";
    if (!limits) {
        ai_copy(name, name_size, ai_name);
        return;
    }

    if (out->m_size) out->m_size = ai_limit(out->m_size, 0.1f, 100.0f);
    out->speed = ai_limit(out->speed, 0.0f, 5.0f);
    out->turn_speed = ai_limit(out->turn_speed, 0.0f, 30.0f);
    out->gravity = ai_limit(out->gravity, 0.0f, 1.0f);
    out->fire_rate = (int) ai_limit((float) out->fire_rate, 200.0f, 10000.0f);
    out->vision = (int) ai_limit((float) out->vision, 1.0f, 1000.0f);
    out->score = (int) ai_limit((float) out->score, 0.0f, 10000.0f);
    out->projectile = (int) ai_limit((float) out->projectile, 0.0f, 5.0f);
    out->behaviour = (int) ai_limit((float) out->behaviour, 0.0f, 20.0f);

    int ok = ai_name[0] && strlen(ai_name) <= 16;
    for (const unsigned char *c = (const unsigned char *) ai_name; ok && *c; c++) ok = ai_name_char_ok(*c);
    ai_copy(name, name_size, ok ? ai_name : "Bot");
}

static int ai_is_bot_object(const cJSON *object) {
    const cJSON *i = cJSON_GetObjectItem(object, "i");
    if (cJSON_IsNumber(i)) return (int) cJSON_GetNumberValue(i) == PREFAB_BOT;
    return cJSON_IsString(i) && !strcmp(cJSON_GetStringValue(i), "BOT");
}

// ---------------------------------------------------------------- list

GameAIs *game_ais(Game *game) {
    if (!game->ai) game->ai = calloc(1, sizeof(GameAIs));
    return game->ai;
}

void game_ai_reset(Game *game) {
    if (!game->ai) return;
    const unsigned serial = game->ai->serial;
    memset(game->ai, 0, sizeof(*game->ai));
    game->ai->serial = serial; // spawn serials keep growing: client meshes never mix two AIs
}

GameAI *game_ai_find(Game *game, const int sid) {
    if (!game->ai) return NULL;
    for (int i = 0; i < game->ai->count; i++) {
        if (game->ai->ais[i].sid == sid) return &game->ai->ais[i];
    }
    return NULL;
}

static int ai_generate_sid(const GameAIs *list) { // UTILS.generateSID
    for (int sid = 1;; sid++) {
        int used = 0;
        for (int i = 0; i < list->count && !used; i++) used = list->ais[i].sid == sid;
        if (!used) return sid;
    }
}

// AI.init(x, y, z, name, index, dat, sid)
static void ai_init(GameAI *ai, const vec3 position, const char *name, const int index, const GameAIData *dat, const int sid) {
    memset(ai, 0, sizeof(*ai));
    const GameAIData *z = dat ? dat : &k_builtin[index >= 0 && index < AI_BUILTIN_COUNT ? index : 0];

    ai->sid = sid;
    ai->index = index;
    ai->has_data = (unsigned char) !!dat;
    ai->data = *z;
    ai->active = 1;
    ai_copy(ai->name, sizeof(ai->name), name);
    ai->max_health = z->health;
    ai->health = ai->max_health;
    ai->speed = z->speed * 0.031f;
    ai->turn_speed = z->turn_speed * 0.002f;
    ai->gravity = z->gravity * 0.1f;
    ai->kill_score = z->score;
    ai->respawn_t = z->respawn_t;
    ai->fire_rate = z->fire_rate ? (float) z->fire_rate : 2000.0f;
    if (ai->fire_rate < 0.0f) ai->fire_rate = 200.0f;
    ai->shoot_timer = ai->fire_rate;
    ai->shot_spread = 0.02f;
    ai->shot_offset = 2.0f;
    ai->spawn_cap = 20;
    ai->hit_range = z->can_shoot ? 40.0f : 10.0f;
    ai->hit_damage = AI_HIT_DAMAGE;
    ai->hit_rate = AI_HIT_RATE;
    ai->bob_d = z->bob_d ? z->bob_d : 0.2f;
    ai->bob_s = z->bob_s ? z->bob_s : 0.004f;
    ai->scale = z->m_size ? z->m_size : 4.0f;
    ai->position = (vec3) {(float) ai_round(position.x, 1), (float) ai_round(position.y, 1), (float) ai_round(position.z, 1)};
    ai->start = ai->position;
    ai->behaviour_type = z->behaviour;
    ai->target_uid = -1;
    ai->vision = z->vision ? (float) z->vision : 120.0f;
    ai->always_name = ai->scale >= 6.0f;
    ai->frames = z->asset ? 0 : 2;
    if (z->no_frames) ai->frames = 0;
    ai->idle_timer = AI_IDLE_SOUND_INTERVAL * ai_rand_float(0.8f, 1.2f);
}

GameAI *game_ai_spawn(Game *game, int sid, const int index, const char *name, const vec3 position, const GameAIData *data) {
    GameAIs *list = game_ais(game);
    if (!list || list->count >= GAME_AI_MAX) return NULL;
    if (!sid) sid = ai_generate_sid(list);

    GameAI *ai = &list->ais[list->count++];
    ai_init(ai, position, name, index, data, sid);
    ai->serial = ++list->serial;
    return ai;
}

// objects.js addAI (pendingAIs, at most config.aiLimit) + addPendingAIs at game.init.
// addAI is gated on !RENDER, so v3.1.1 only has map AIs on the server: its single-player
// client (playCustomMap) never spawns them. Intentional difference: KrunkNative's offline
// game is an authority too and keeps the map's BOTs playable; ai_render.c hides them until
// the local player has spawned, as AI.sync would (nothing reaches a menu connection).
void game_ai_spawn_map(Game *game) {
    if (!game_is_authority(game) || !game->map || !game->map->raw_data) return;

    const cJSON *objects = cJSON_GetObjectItem(game->map->raw_data, "objects");
    int pending = 0;

    for (int i = 0; i < cJSON_GetArraySize(objects) && pending < GAME_AI_MAP_LIMIT; i++) {
        const cJSON *object = cJSON_GetArrayItem(objects, i);
        const cJSON *p = cJSON_GetObjectItem(object, "p");
        if (!ai_is_bot_object(object) || !cJSON_IsArray(p) || cJSON_GetArraySize(p) < 3) continue;

        GameAIData data;
        char name[GAME_AI_NAME_SIZE];
        ai_parse_bot(object, &data, name, sizeof(name), 1);
        // map.js: p rounded to 2 decimals (UTILS.limit to positionRange is a no-op on real maps)
        const vec3 position = {
            (float) ai_round(json_number(cJSON_GetArrayItem(p, 0), 0), 2),
            (float) ai_round(json_number(cJSON_GetArrayItem(p, 1), 0), 2),
            (float) ai_round(json_number(cJSON_GetArrayItem(p, 2), 0), 2),
        };
        game_ai_spawn(game, 0, 0, name, position, &data);
        pending++;
    }
}

// ---------------------------------------------------------------- sight / targets

// game.js canSee: the objects that block (active, not noShoot; seeThroughTransparent).
static int ai_object_blocks(const Object *object) {
    if (!object->active || object->no_shoot || object->collision_type == COLLISION_TYPE_NONE) return 0;
    if (object->kill || object->score_zone || object->team_zone || object->teleporter || object->checkpoint ||
        object->pickup || object->flag || object->premium || object->verified ||
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

static double js_min(const double a, const double b) { return a != a || b != b ? NAN : a < b ? a : b; }
static double js_max(const double a, const double b) { return a != a || b != b ? NAN : a > b ? a : b; }

// Game.canSee(player, x, y, z, 0, 0, true) == null: from the eye (y + height - cameraHeight)
// along feet -> target; any object box entered before the end blocks (`ah && ah < 1`).
int game_ai_player_can_see(const Game *game, const Player *origin, const vec3 target) {
    if (!game->map) return 1;
    const double ox = origin->position.x, oz = origin->position.z;
    const double oy = origin->position.y + origin->height - game_constants.camera_height;
    const double ix = 1.0 / (target.x - origin->position.x), iy = 1.0 / (target.y - origin->position.y),
                 iz = 1.0 / (target.z - origin->position.z);

    for (size_t i = 0; i < game->map->object_count; i++) {
        const Object *object = game->map->objects[i];
        if (!ai_object_blocks(object)) continue;
        const double hx = object->scale.x * 0.5, hz = object->scale.z * 0.5;
        const double a = (object->position.x - hx - ox) * ix, b = (object->position.x + hx - ox) * ix;
        const double c = (object->position.y - oy) * iy, d = (object->position.y + object->scale.y - oy) * iy;
        const double e = (object->position.z - hz - oz) * iz, f = (object->position.z + hz - oz) * iz;
        const double near = js_max(js_max(js_min(a, b), js_min(c, d)), js_min(e, f));
        const double far = js_min(js_min(js_max(a, b), js_max(c, d)), js_max(e, f));
        if (!(far < 0.0) && !(near > far) && near == near && near != 0.0 && near < 1.0) return 0;
    }
    return 1;
}

// players.findNearestTo(x, y, z, range, xray)
static Player *ai_find_nearest(const Game *game, const vec3 point, const float range, const int xray) {
    Player *best = NULL;
    float best_distance = 0.0f;
    for (size_t i = 0; i < game->player_count; i++) {
        Player *player = game->players[i];
        if (!player || !player->active) continue;
        const float distance = ai_d3d(point, player->position);
        if (distance <= range && (!best || best_distance > distance) && (xray || game_ai_player_can_see(game, player, point))) {
            best = player;
            best_distance = distance;
        }
    }
    return best;
}

float game_ai_line_hit(const GameAI *ai, const vec3 origin, const vec3 direction) {
    const vec3 box = {ai->scale * 2.0f, ai->scale * 2.0f, ai->scale * 2.0f};
    return line_in_rect(origin, direction, ai->position, box);
}

// UTILS.getDrs(target, ai): [yaw, pitch] from the target to the AI.
static void ai_get_drs(const vec3 target, const vec3 self, double *yaw, double *pitch) {
    const double p = self.x - target.x, q = self.y - target.y, u = self.z - target.z;
    *pitch = -atan2(q, sqrt(p * p + u * u));
    *yaw = -atan2(-p, u);
}

// ---------------------------------------------------------------- sounds

// AI.emitSound(id, vol, x, y, z): game.playSound(id, (vol || 1) * 0.9, 0, round(x || ai.x), ..,
// true, 1, false, true, true): "s" to everyone with the raw flag -> the client's
// Game.playerSound -> SOUND.play3D(id, x, y, z, vol, 1, .., asset): a user-asset sound.
static void ai_emit_sound(Game *game, const GameAI *ai, const int id, const float volume, const vec3 *at) {
    if (!id) return;
    const vec3 p = at ? *at : ai->position;
    const vec3 position = {p.x ? p.x : ai->position.x, p.y ? p.y : ai->position.y, p.z ? p.z : ai->position.z}; // p || this.x
    char name[32];
    snprintf(name, sizeof(name), "asset:%d", id);
    game_play_sound(game, name, -1, position, (volume ? volume : 1.0f) * 0.9f, 1.0f); // shared/src/triggers.c
}

// ---------------------------------------------------------------- health

void game_ai_kill(Game *game, GameAI *ai, Player *source) {
    if (!ai->active) return;
    ai->active = 0;
    ai->health = 0.0f;
    ai_emit_sound(game, ai, ai->data.death_sound, 0.0f, NULL); // deathSound && emitSound(dth)
    ai->respawn_timer = ai->data.respawn_r ? (float) ai_rand_int(100, ai->respawn_t ? (int) ai->respawn_t : 5000) : ai->respawn_t;
    game_trigger_ai_death(game, source, ai); // this.onAIDeath && f.onTrigger(m, this) (checks trig.event)
    // reward && !isCustom: getItemByChance skins for every active player (no inventory natively)
}

int game_ai_change_health(Game *game, GameAI *ai, const float amount, Player *source, const int force_target) {
    const int was_active = ai->active;
    if (ai->health) {
        ai->health += amount;
        if (ai->health <= 0.0f) game_ai_kill(game, ai, source);
    }
    if (source) source->damage_dealt -= amount;
    ai->frame_timer = 1000.0f;
    ai->frame = 1;
    if (source && (force_target || ai->target_uid < 0)) { // behaviour is always 1
        ai->target_uid = source->uid;
        ai->action_freq = 5000.0f;
    }
    return was_active && !ai->active;
}

void game_ai_damage(Game *game, GameAI *ai, Player *dealer, const float damage, const int force_target) {
    game_ai_change_health(game, ai, -damage, dealer, force_target);
    if (!dealer) return;
    if (!ai->active && ai->kill_score) player_score(dealer, ai->kill_score, 1);

    const NetHitPacket hit = {
        .target_uid = ai->sid,
        .damage = (int32_t) roundf(damage),
        .crit = 0,
        .kill = (uint8_t) !ai->active,
        .ai = 1,
    };
    game_send(game, dealer->uid, NET_PACKET_HIT, &hit, sizeof(hit)); // IO.send(id, "4", sid, dmg, 0, !active, 1)
}

// ---------------------------------------------------------------- update

static void ai_orbs(Game *game, const vec3 at, const int count) {
    double yaw = 0.0;
    for (int i = 0; i < count; i++) {
        game_spawn_ai_projectile(game, 1, at, (float) yaw, 0.0f); // stored[1], pitch 0, a ring
        yaw += AI_PI * 2.0 / count;
    }
}

// Turn xD / yD towards `target` at turnSpd * mlt * delta (ai.js update).
static void ai_turn(float *angle, const double goal, const double step) {
    const double n = AI_PI * 2.0;
    const double r = fmod(*angle - goal + n, n);
    const double u = fmin(fmin(fabs(r - n), r), step);
    const double v = r - AI_PI >= 0.0 ? 1.0 : -1.0;
    *angle = (float) fmod(*angle + v * u + n, n); // JS keeps adding 2 PI; the angle is the same
}

static void ai_collide_map(Game *game, GameAI *ai) {
    if (!game->map) return;
    for (size_t i = 0; i < game->map->object_count; i++) {
        const Object *o = game->map->objects[i];
        if (!o->active || o->collision_type == COLLISION_TYPE_NONE) continue;
        // Krunker boxes: centre x/y/z, half width / height / length
        const float w = o->scale.x * 0.5f, h = o->scale.y * 0.5f, l = o->scale.z * 0.5f;
        const float x = o->position.x, y = o->position.y + h, z = o->position.z;
        vec3 *p = &ai->position;
        const float s = ai->scale;
        if (!(p->x - s < x + w && p->x + s > x - w && p->z - s < z + l && p->z + s > z - l && p->y <= y + h && p->y + s >= y - h)) continue;

        if ((p->y < y + h && y + h - p->y <= 3.0f && ai->old.y < y + h) || ai->old.y >= y + h) p->y = y + h;
        else if (ai->old.x - s >= x + w - 0.00001f) p->x = x + w + s;
        else if (ai->old.x + s <= x - w + 0.00001f) p->x = x - w - s;
        else if (ai->old.z - s >= z + l - 0.00001f) p->z = z + l + s;
        else if (ai->old.z + s <= z - l + 0.00001f) p->z = z - l - s;
    }
}

static void ai_boss_weeping(Game *game, GameAI *ai, const float q, float *mlt) {
    ai->break_timer += q;
    if (ai->break_timer >= 5000.0f) {
        ai->break_timer = (float) -ai_rand_int(0, 5000);
        ai->break_attack = 4000.0f;
    }
    if (ai->health <= ai->max_health / 1.5f) {
        *mlt = 1.5f;
        ai->special_timer -= q;
        if (ai->special_timer <= 0.0f) {
            ai->special_timer = (float) ai_rand_int(7000, 12000);
            ai->break_move = 2000.0f;
            ai->break_attack = 3000.0f;
            ai_orbs(game, (vec3) {ai->position.x, ai->position.y + ai->shot_offset, ai->position.z}, 8);
        }
    }
    if (ai->health <= ai->max_health / 2.0f) {
        *mlt = 2.0f;
        ai->special_timer2 -= q;
        if (ai->special_timer2 <= 0.0f) {
            ai_emit_sound(game, ai, AI_BOSS_SOUND, 0.0f, NULL); // emitSound(3236)
            ai->special_timer2 = (float) ai_rand_int(12000, 16000);
            ai->special_timer = 0.0f;
            const float angle = ai_rand_float(-(float) AI_PI, (float) AI_PI);
            ai->position.x = ai->start.x + cosf(angle) * 100.0f;
            ai->position.z = ai->start.z + sinf(angle) * 100.0f;
        }
    }
}

// The spawns below may move game->ai->ais: callers re-fetch their AI by index.
static void ai_boss_lord(Game *game, const int index, const float q) {
    GameAI *ai = &game->ai->ais[index];
    if (ai->health <= ai->max_health / 2.0f) {
        if (ai->shoot_timer > 7000.0f) ai->shoot_timer = 7000.0f;
        if (!ai->used_special) {
            ai->used_special = 1;
            double g = 0.0;
            const vec3 at = ai->position;
            const float scale = ai->scale;
            for (int i = 0; i < 6; i++) {
                const float h = scale * 2.0f + 70.0f;
                const vec3 spawn = {at.x + h * (float) cos(g), at.y + scale * 2.0f + 10.0f, at.z + h * (float) sin(g)};
                ai_emit_sound(game, ai, AI_BOSS_SOUND, 0.0f, &at); // emitSound(3236, null, x, y, z)
                game_ai_spawn(game, 0, 2, "Jack O' Lantern", spawn, NULL);
                ai = &game->ai->ais[index];
                g += AI_PI * 2.0 / 6.0;
            }
            ai = &game->ai->ais[index];
        }
    }
    ai->special_timer -= q;
    if (ai->special_timer <= 0.0f) {
        ai->special_timer = (float) ai_rand_int(5000, 10000);
        ai->break_attack = 1000.0f;
        ai_orbs(game, (vec3) {ai->position.x, ai->position.y + 3.0f + ai->scale + ai->shot_offset, ai->position.z}, 8);
    }
    if (ai->spawn_cap > 0) {
        ai->spawn_minion_timer += q;
        if (ai->spawn_minion_timer >= 6000.0f) {
            ai->spawn_cap--;
            ai->spawn_minion_timer = (float) -ai_rand_int(0, 6000);
            const float angle = ai_rand_float(-(float) AI_PI, (float) AI_PI);
            const float h = ai->scale * 2.0f + 100.0f;
            const vec3 spawn = {ai->position.x + h * cosf(angle), ai->position.y + ai->scale * 2.0f + 10.0f, ai->position.z + h * sinf(angle)};
            ai_emit_sound(game, ai, AI_BOSS_SOUND, 0.0f, &spawn); // emitSound(3236, null, I, J, K)
            game_ai_spawn(game, 0, 1, "Lost Soul", spawn, NULL);
        }
    }
}

// AI.update(q) on the server (q: ms * deltaMlt).
static void ai_update(Game *game, const int index, const float q) {
    GameAI *ai = &game->ai->ais[index];

    if (ai->respawn_timer) {
        ai->respawn_timer -= q;
        if (!(ai->respawn_timer <= 0.0f)) return;
        ai->active = 1;
        ai->target_uid = -1;
        ai->position = ai->start;
        ai->health = ai->max_health;
        ai->respawn_timer = 0.0f;
    }
    if (!ai->active) return;

    if (ai->break_move > 0.0f) {
        ai->break_move -= q;
        if (ai->break_move <= 0.0f) ai->break_move = 0.0f;
    }
    if (ai->frame_timer) {
        ai->frame_timer -= q;
        if (ai->frame_timer <= 0.0f) {
            ai->frame_timer = 0.0f;
            ai->frame = 0;
        }
    }
    ai->old = ai->position;
    ai->position.y -= ai->gravity * q;

    float mlt = 1.0f;
    if (ai->behaviour_type == 1 && ai->target_uid >= 0) ai_boss_weeping(game, ai, q, &mlt);
    if (ai->behaviour_type == 2 && ai->target_uid >= 0) {
        ai_boss_lord(game, index, q);
        ai = &game->ai->ais[index];
    }

    // behaviour 1 (every AI: init sets it, and the `behavior == 0` early-out is a typo)
    ai->action_freq -= q;
    if (ai->action_freq <= 0.0f) {
        const Player *target = ai_find_nearest(game, (vec3) {ai->position.x, ai->position.y + ai->scale / 2.0f, ai->position.z}, ai->vision, ai->data.xray);
        ai->target_uid = target ? target->uid : -1;
        ai->action_freq = target ? 2000.0f : 5000.0f;
    }

    const Player *target = ai->target_uid >= 0 ? game_find_player(game, ai->target_uid) : NULL;
    if (ai->target_uid >= 0 && !target) ai->target_uid = -1; // left the game
    const vec3 goal = target ? target->position : ai->start;

    double yaw, pitch;
    ai_get_drs(goal, ai->position, &yaw, &pitch);
    ai_turn(&ai->x_dir, yaw + AI_PI / 2.0, ai->turn_speed * mlt * q);
    ai_turn(&ai->y_dir, pitch, ai->turn_speed * mlt * q);

    if (ai->speed && !ai->break_move && ai_d3d(ai->position, goal) >= ai->hit_range) {
        const float w = ai->speed * mlt * q;
        ai->position.x += w * sinf(ai->x_dir + (float) AI_PI / 2.0f) * cosf(ai->y_dir);
        ai->position.z += w * cosf(ai->x_dir + (float) AI_PI / 2.0f) * cosf(ai->y_dir);
        ai->position.y += w * sinf(ai->y_dir);
    }

    if (ai->break_attack > 0.0f) {
        ai->break_attack -= q;
        if (ai->break_attack <= 0.0f) ai->break_attack = 0.0f;
    }

    if (!ai->break_attack && ai->data.can_hit) {
        ai->hit_timer -= q;
        if (ai->hit_timer <= 0.0f) {
            ai->hit_timer = ai->hit_rate;
            Player *victim = ai_find_nearest(game, ai->position, ai->hit_range, 1);
            if (victim) {
                ai->break_move = 900.0f;
                if (player_change_health(victim, NULL, ai->hit_damage, 0)) {
                    PlayerKillInfo info = {.weapon_id = -1};
                    player_kill(victim, NULL, &info, 0);
                }
            }
        }
    }

    if (!ai->break_attack && ai->data.can_shoot) {
        ai->shoot_timer -= q;
        if (target && ai->shoot_timer < 0.0f) {
            ai->shoot_timer = ai->fire_rate;
            ai->frame_timer = 1000.0f;
            ai->frame = 1;
            double shot_yaw, shot_pitch;
            ai_get_drs(target->position, ai->position, &shot_yaw, &shot_pitch);
            shot_yaw += ai_rand_float(-ai->shot_spread, ai->shot_spread);
            shot_pitch += ai_rand_float(-ai->shot_spread, ai->shot_spread);
            const int kind = ai->data.projectile + 1;
            if (game_ai_projectile_config(kind)) {
                ai_emit_sound(game, ai, ai->data.attack_sound, 0.0f, NULL); // emitSound(atkSound)
                const vec3 at = {ai->position.x, ai->position.y + ai->scale / 2.0f + ai->shot_offset, ai->position.z};
                game_spawn_ai_projectile(game, kind, at, (float) shot_yaw, (float) shot_pitch);
            }
        }
    }

    ai_collide_map(game, ai);
}

// AI manager update: every AI, then active moving AIs push the later ones apart.
static void ai_update_all(Game *game, const float q) {
    GameAIs *list = game->ai;
    for (int i = 0; i < list->count; i++) ai_update(game, i, q);

    for (int i = 0; i < list->count; i++) {
        GameAI *a = &list->ais[i];
        if (!a->active || !a->speed) continue;
        for (int j = i + 1; j < list->count; j++) {
            const GameAI *b = &list->ais[j];
            if (!b->active) continue;
            const float distance = ai_d3d(a->position, b->position);
            const float min = a->scale + b->scale;
            if (distance < min) {
                const float direction = atan2f(a->position.z - b->position.z, a->position.x - b->position.x); // direction2D
                a->position.x = b->position.x + min * cosf(direction);
                a->position.z = b->position.z + min * sinf(direction);
            }
        }
    }
}

// KRUNKNATIVE_AI_LOG=1: one line per AI every second (stderr)
static void ai_log(const Game *game, const float delta_ms) {
    static int enabled = -1;
    static float timer;
    if (enabled < 0) {
        const char *value = getenv("KRUNKNATIVE_AI_LOG");
        enabled = value && *value && *value != '0';
    }
    if (!enabled || (timer -= delta_ms) > 0.0f) return;
    timer = 1000.0f;
    for (int i = 0; i < game->ai->count; i++) {
        const GameAI *ai = &game->ai->ais[i];
        fprintf(stderr, "[ai] %2d sid %d %-16s %s hp %.0f/%.0f pos %.1f %.1f %.1f xD %.2f yD %.2f target %d\n", i, ai->sid, ai->name,
                ai->active ? "on " : "off", ai->health, ai->max_health, ai->position.x, ai->position.y, ai->position.z, ai->x_dir,
                ai->y_dir, ai->target_uid);
    }
}

void game_ai_tick(Game *game, const float delta_ms) {
    if (!game_is_authority(game) || !game->ai || !game->ai->count) return;
    ai_update_all(game, delta_ms);
    ai_log(game, delta_ms);

    game->ai->sync_timer += delta_ms / 1000.0f;
    if (game->ai->sync_timer >= GAME_AI_SYNC_INTERVAL) {
        game->ai->sync_timer = 0.0f;
        game_ai_sync(game);
    }
}

// ---------------------------------------------------------------- sync

static void ai_add_packet(const GameAI *ai, NetAIAddPacket *out) { // AI.getData
    memset(out, 0, sizeof(*out));
    out->sid = ai->sid;
    out->index = ai->index;
    ai_copy(out->name, sizeof(out->name), ai->name);
    out->position[0] = (float) ai_round(ai->position.x, 1);
    out->position[1] = (float) ai_round(ai->position.y, 1);
    out->position[2] = (float) ai_round(ai->position.z, 1);
    out->has_data = ai->has_data;
    if (!ai->has_data) return;
    out->max_health = ai->max_health;
    out->m_size = ai->scale;
    out->asset = ai->data.asset;
    out->idle_sound = ai->data.idle_sound;
    ai_copy(out->src, sizeof(out->src), ai->data.src);
}

static void ai_sync_entry(const GameAI *ai, NetAISyncEntry *out) {
    out->sid = ai->sid;
    out->position[0] = (float) ai_round(ai->position.x, 1);
    out->position[1] = (float) ai_round(ai->position.y, 1);
    out->position[2] = (float) ai_round(ai->position.z, 1);
    out->x_dir = (float) ai_round(ai->x_dir, 2);
    out->y_dir = (float) ai_round(ai->y_dir, 2);
    out->health = (int32_t) floorf(ai->health);
    out->frame = (uint8_t) ai->frame;
}

static int ai_sent_to(GameAI *ai, const int uid) {
    for (int i = 0; i < ai->sent_count; i++) {
        if (ai->sent_to[i] == uid) return 1;
    }
    if (ai->sent_count < GAME_AI_SENT_MAX) ai->sent_to[ai->sent_count++] = uid;
    return 0;
}

static void ai_send_sync(Game *game, const int target, const NetAISyncEntry *entries, const int count) {
    NetAISyncPacket packet;
    int sent = 0;
    do {
        memset(&packet, 0, sizeof(packet));
        packet.first = (uint8_t) (sent == 0);
        packet.count = (uint8_t) (count - sent < NET_AI_SYNC_MAX ? count - sent : NET_AI_SYNC_MAX);
        memcpy(packet.entries, entries + sent, packet.count * sizeof(NetAISyncEntry));
        game_send(game, target, NET_PACKET_AI_SYNC, &packet, (uint16_t) (offsetof(NetAISyncPacket, entries) + packet.count * sizeof(NetAISyncEntry)));
        sent += packet.count;
    } while (sent < count);
}

// AI.sync: every active player gets "aai" for AIs in range it has not seen and one "ai"
// list; connections without a player (spectators, menu) get every active AI.
void game_ai_sync(Game *game) {
    GameAIs *list = game->ai;
    if (!list || !list->count || game->is_local) return; // the in-process client reads the list directly

    NetAISyncEntry entries[GAME_AI_MAX];
    NetAIAddPacket add;

    for (size_t p = 0; p < game->player_count; p++) {
        const Player *player = game->players[p];
        if (!player || !player->active) continue;
        int count = 0;
        for (int i = 0; i < list->count; i++) {
            GameAI *ai = &list->ais[i];
            if (!ai->active || ai_d3d(player->position, ai->position) > GAME_AI_SYNC_RANGE) continue;
            if (ai_sent_to(ai, player->uid)) {
                ai_sync_entry(ai, &entries[count++]);
            } else {
                ai_add_packet(ai, &add);
                game_send(game, player->uid, NET_PACKET_AI_ADD, &add, sizeof(add));
            }
        }
        ai_send_sync(game, player->uid, entries, count);
    }

    int count = 0;
    for (int i = 0; i < list->count; i++) {
        GameAI *ai = &list->ais[i];
        if (list->non_players_dirty || !ai->sent_non_players) {
            ai->sent_non_players = 1;
            ai_add_packet(ai, &add);
            game_send(game, NET_TARGET_NON_PLAYERS, NET_PACKET_AI_ADD, &add, sizeof(add));
        }
        if (ai->active) ai_sync_entry(ai, &entries[count++]);
    }
    list->non_players_dirty = 0;
    ai_send_sync(game, NET_TARGET_NON_PLAYERS, entries, count);
}

void game_ai_sync_non_players(Game *game) {
    if (game->ai) game->ai->non_players_dirty = 1;
}

// ---------------------------------------------------------------- client mirror

static void ai_builtin_render(const int index, GameAIData *out) {
    *out = k_builtin[index >= 0 && index < AI_BUILTIN_COUNT ? index : 0];
}

// AI.cSpawn(getData): the client's init. A known sid is re-initialised (the server resends
// "aai" when a connection moves between spectating and playing).
void game_ai_apply_add(Game *game, const NetAIAddPacket *packet) {
    GameAIs *list = game_ais(game);
    if (!list) return;

    char name[GAME_AI_NAME_SIZE];
    memcpy(name, packet->name, sizeof(name));
    name[sizeof(name) - 1] = 0;
    const vec3 position = {packet->position[0], packet->position[1], packet->position[2]};

    GameAIData data;
    if (packet->has_data) {
        // dat: {health, mSize, asset, idl, src}; the mesh is "ghost_0" unless it is an asset
        memset(&data, 0, sizeof(data));
        data.health = packet->max_health;
        data.m_size = packet->m_size;
        data.asset = packet->asset;
        data.idle_sound = packet->idle_sound;
        memcpy(data.src, packet->src, sizeof(data.src));
        data.src[sizeof(data.src) - 1] = 0;
        data.glow = !data.asset;
    } else {
        ai_builtin_render(packet->index, &data);
    }

    GameAI *ai = game_ai_find(game, packet->sid);
    if (ai) {
        const unsigned serial = ai->serial;
        ai_init(ai, position, name, packet->index, packet->has_data ? &data : NULL, packet->sid);
        ai->serial = serial;
    } else {
        ai = game_ai_spawn(game, packet->sid, packet->index, name, position, packet->has_data ? &data : NULL);
    }
    if (ai) ai->shown = 1; // a fresh mesh is visible until the next syncAll
}

void game_ai_apply_sync(Game *game, const NetAISyncPacket *packet) {
    GameAIs *list = game->ai;
    if (!list) return;
    if (packet->first) {
        for (int i = 0; i < list->count; i++) list->ais[i].shown = 0; // hideAll
    }
    for (int i = 0; i < packet->count && i < NET_AI_SYNC_MAX; i++) {
        const NetAISyncEntry *entry = &packet->entries[i];
        GameAI *ai = game_ai_find(game, entry->sid);
        if (!ai) continue;
        ai->shown = 1;
        ai->position = (vec3) {entry->position[0], entry->position[1], entry->position[2]};
        ai->x_dir = entry->x_dir;
        ai->y_dir = entry->y_dir;
        ai->health = (float) entry->health;
        ai->frame = entry->frame;
    }
}

// ---------------------------------------------------------------- summons (server/game.ts)

static GameAITemplate g_templates[16];
static char g_template_names[16][GAME_AI_NAME_SIZE];
static int g_template_count = -1;

static void ai_normalise(const char *in, char *out, const size_t size) { // normaliseSpawnableAIKey
    size_t n = 0;
    for (const unsigned char *c = (const unsigned char *) in; *c && n + 1 < size; c++) {
        if (isalnum(*c)) out[n++] = (char) tolower(*c);
    }
    out[n] = 0;
}

static int ai_template_compare(const void *a, const void *b) { // localeCompare (ASCII names)
    return strcasecmp(((const GameAITemplate *) a)->name, ((const GameAITemplate *) b)->name);
}

// buildSoulSanctumAITemplates: the first BOT per normalised aiName of the Soul Sanctum map.
const GameAITemplate *game_ai_templates(int *count) {
    if (g_template_count < 0) {
        g_template_count = 0;
        const int index = map_catalog_index_of_key("soul_sanctum");
        const cJSON *map = index >= 0 ? map_catalog_json((size_t) index) : NULL;
        const cJSON *objects = map ? cJSON_GetObjectItem(map, "objects") : NULL;
        for (int i = 0; i < cJSON_GetArraySize(objects) && g_template_count < 16; i++) {
            const cJSON *object = cJSON_GetArrayItem(objects, i);
            const cJSON *ain = cJSON_GetObjectItem(object, "ain");
            if (!cJSON_IsString(ain) || !cJSON_GetStringValue(ain)[0]) continue;

            char key[GAME_AI_NAME_SIZE], other[GAME_AI_NAME_SIZE];
            ai_normalise(cJSON_GetStringValue(ain), key, sizeof(key));
            int seen = 0;
            for (int t = 0; t < g_template_count && !seen; t++) {
                ai_normalise(g_templates[t].name, other, sizeof(other));
                seen = !strcmp(key, other);
            }
            if (seen) continue;

            GameAITemplate *template = &g_templates[g_template_count];
            ai_parse_bot(object, &template->data, g_template_names[g_template_count], GAME_AI_NAME_SIZE, 0);
            template->name = g_template_names[g_template_count];
            g_template_count++;
        }
        // listSoulSanctumAIs sorts the names; sort the table and re-point the names
        qsort(g_templates, (size_t) g_template_count, sizeof(GameAITemplate), ai_template_compare);
        static char sorted[16][GAME_AI_NAME_SIZE];
        for (int t = 0; t < g_template_count; t++) {
            ai_copy(sorted[t], GAME_AI_NAME_SIZE, g_templates[t].name);
            g_templates[t].name = sorted[t];
        }
    }
    if (count) *count = g_template_count;
    return g_templates;
}

const GameAITemplate *game_ai_resolve_template(const char *name_or_alias) {
    static const char *const aliases[][2] = { // buildSoulSanctumAIAliases
        {"weep", "weepingsoul"}, {"weeping", "weepingsoul"}, {"wander", "wanderingsoul"}, {"wandering", "wanderingsoul"},
        {"fiend", "soulfiend"}, {"soulfiend", "soulfiend"}, {"angered", "angeredsoul"}, {"angeredsoul", "angeredsoul"},
        {"gate", "gatekeeper"}, {"gatekeeper", "gatekeeper"}, {"lord", "lordofsouls"}, {"lordofsouls", "lordofsouls"},
    };
    char key[64], other[GAME_AI_NAME_SIZE];
    ai_normalise(name_or_alias ? name_or_alias : "", key, sizeof(key));
    for (size_t i = 0; i < sizeof(aliases) / sizeof(aliases[0]); i++) {
        if (!strcmp(key, aliases[i][0])) {
            ai_copy(key, sizeof(key), aliases[i][1]);
            break;
        }
    }
    int count = 0;
    const GameAITemplate *templates = game_ai_templates(&count);
    for (int i = 0; i < count; i++) {
        ai_normalise(templates[i].name, other, sizeof(other));
        if (!strcmp(key, other)) return &templates[i];
    }
    return NULL;
}

// spawnSoulSanctumAI(name, {origin: player x/y/z/yaw, distance: max(48, mSize * 4)})
GameAI *game_ai_summon(Game *game, const GameAITemplate *template, const Player *origin) {
    if (!template || !origin || !game_is_authority(game)) return NULL;
    const float scale = template->data.m_size ? template->data.m_size : 4.0f;
    const float distance = fmaxf(48.0f, (template->data.m_size ? template->data.m_size : 4.0f) * 4.0f);
    const float yaw = origin->direction.y;
    const vec3 position = {
        origin->position.x + sinf(yaw) * distance,
        origin->position.y + fmaxf(2.0f, scale / 2.0f),
        origin->position.z + cosf(yaw) * distance,
    };
    GameAI *ai = game_ai_spawn(game, 0, 0, template->name, position, &template->data);
    if (ai) game_ai_sync(game); // AI.sync() right away
    return ai;
}
