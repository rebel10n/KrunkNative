// Krunker v7.1.4 AI: src/data/AI.js (the AI entity f2119 and its manager: spawn, update, sync,
// changeHealth, kill, removeFully, collidesAcc, playAnim), MapManager.buildAIData / addAI /
// addPendingAIs (map AI_SPAWNER objects and their clamps; v3.1.1 maps' BOT is the same prefab),
// game.js findNearestTo / findFurthestFrom / canSee / activeAIs, projectiles.js (AI shots) and
// server/game.ts (Soul Sanctum summon templates). See shared/include/ai.h, doc/v7/map-logic.md §2.8.
//
// Sounds: idl / atk / atkh / mel / melh / agr / dth and the bosses' 3236 are user-asset sounds
// ("asset:<id>" -> assets/user-assets/<id>/sound.mp3). Triggers: trigger_ai_event (WP-C) runs the
// AI's tas for onAIDeath / onAIDamage / onAIHalfHealth / onAIKillPlayer.
// Intentional differences (documented where they happen):
// - Skin rewards (rew / rewm / rewr / rewn) are public-game only in v7 (isPubType): KN has no
//   inventory economy for them, so they are parsed and ignored (map-logic.md §2.8).
// - Players hit AIs at the AI's live position (v7 rewinds AIs by the shooter's ping, posHist).
// - The built-in kinds' own trigger actions (Jack O' Lantern 62, Mirror Image 65) are backend KR
//   actions, no-ops without kn_brain (Decision 4): the kinds carry no trigger.
#include <ai.h>
#include <ai_parts.h> // v10.0.3 AI parts: preciseHits, ai.solid, the playAnim hold
#include <ks_game.h> // v10.0.3 onAIDamage / onAIPartDamage / onAIDeath
#include <net_ext.h>
#include <zombies.h> // map-logic WP-H: insta-kill
#include <map_ai.h>
#include <map_catalog.h>
#include <modes.h>
#include <spawnables.h>
#include <triggers_v7.h>
#include <world.h>
#include <pcg_basic.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AI_PI 3.14159265358979323846
#define AI_IDLE_SOUND_INTERVAL 5000.0f // idlSoundI (ms)
#define AI_BOSS_SOUND 3236             // the bosses' teleport / summon sound (user asset)
#define AI_STUCK_IGNORE_MS 2000.0f     // vLN20002: ignoreList time of a target we got stuck on
#define AI_STUCK_TIME_MS 2000.0f       // vLN20003: stuck time before igos / dos / ivos react
#define AI_LAST_HIT_MS 8000.0f         // lastHit.tim
#define AI_HIT_BREAK 400.0f            // hitBreak
#define AI_GRAVITY 0.00015f            // config.gravity (pMov)
#define AI_GROUND_DECEL 0.99f          // config.groundDecel
#define AI_AIR_DECEL 0.9996f           // config.airDecel
#define AI_DECEL_MIN 0.0001f           // config.decelMin
#define AI_CROUCH_DST 3.0f             // config.crouchDst
#define AI_DISK_SPIN 0.0002f           // config.diskSpin
#define AI_INTERFACE_MAX 10000         // config.interfaceIDMax
#define AI_ENCODING_TYPES 3            // config.encodingTypes.length

static const char *const k_clip_names[] = {"Idle", "Move", "MoveFast", "Jump", "Melee", "Shoot", "Crouch"}; // config.clipNames

// v3.1.1 projectiles.stored[1..4] (projectile.js) for game_spawn_ai_projectile; [0] is the thrown knife.
static const ProjectileConfig k_ai_projectiles[GAME_AI_PROJECTILE_KINDS] = {
    {0},
    {.speed = 0.05f, .gravity = 0.0f, .hit_box = 10.0f, .damage = 60.0f, .range = 500.0f},
    {.speed = 0.25f, .gravity = 0.0f, .damage = 20.0f, .range = 500.0f},
    {.speed = 0.03f, .gravity = 0.0f, .hit_box = 20.0f, .damage = 200.0f, .range = 600.0f},
    {.speed = 0.25f, .gravity = 0.0f, .damage = 10.0f, .range = 500.0f},
};

static const ProjectileConfig *g_shot_override;

const ProjectileConfig *game_ai_projectile_config(const int kind) {
    if (kind == 1 && g_shot_override) return g_shot_override; // game_ai_fire's v7 shot
    return kind > 0 && kind < GAME_AI_PROJECTILE_KINDS ? &k_ai_projectiles[kind] : NULL;
}

// v7 projectiles.stored[1]: every AI shot (AI.js update fires stored[1] with the AI's projOb).
static const ProjectileConfig k_ai_shot = {.speed = 0.1f, .gravity = 0.0f, .hit_box = 4.0f, .damage = 1.0f, .range = 500.0f};

// ---------------------------------------------------------------- helpers

static double ai_round(const double value, const int digits) { // Number.prototype.round
    const double p = pow(10.0, digits);
    return floor(value * p + 0.5) / p; // Math.round: halves round up
}

static float ai_rand_float(const float min, const float max) { // UTILS.randFloat
    return min + (float) ldexp((double) pcg32_random(), -32) * (max - min);
}

static int ai_rand_int(const int min, const int max) { // UTILS.randInt (inclusive)
    if (max <= min) return min;
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

// UTILS.getDrs(target, self, yOff): [yaw, pitch] from the target to self (+ yOff on self's y).
static void ai_get_drs(const vec3 target, const vec3 self, const float y_off, double *yaw, double *pitch) {
    const double p = self.x - target.x, q = self.y + y_off - target.y, u = self.z - target.z;
    *pitch = -atan2(q, sqrt(p * p + u * u));
    *yaw = -atan2(-p, u);
}

// Turn xD / yD towards `goal` by at most `step` (AI.js update: (a - g + 2PI) % 2PI, then a %= 2PI).
static void ai_turn(float *angle, const double goal, const double step) {
    const double n = AI_PI * 2.0;
    const double r = fmod(*angle - goal + n, n);
    const double u = fmin(fmin(fabs(r - n), r), step);
    const double v = r - AI_PI >= 0.0 ? 1.0 : -1.0;
    *angle = (float) fmod(*angle + v * u + n, n);
}

// ---------------------------------------------------------------- map data (buildAIData / addAI)

// JS parseInt(v) || 0 / parseFloat(v) || 0 over a JSON number or numeric string.
static double json_number(const cJSON *item, const int integer) {
    double v = NAN;
    if (cJSON_IsNumber(item)) v = cJSON_GetNumberValue(item);
    else if (cJSON_IsString(item)) v = strtod(cJSON_GetStringValue(item), NULL);
    if (v != v || isinf(v)) return 0.0;
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

static int json_truthy(const cJSON *item) {
    if (!item || cJSON_IsNull(item) || cJSON_IsFalse(item)) return 0;
    if (cJSON_IsNumber(item)) return cJSON_GetNumberValue(item) != 0.0;
    if (cJSON_IsString(item)) return cJSON_GetStringValue(item)[0] != 0;
    return 1;
}

// k == null ? fallback : parseFloat(k) || 0
static float json_float_or(const cJSON *object, const char *key, const float fallback) {
    return json_present(object, key) ? json_float(object, key) : fallback;
}

// The aiName / startAnim / tct check: /[^\w\:\;\(\)\?\,\.\|\[\] \-\+*\$\@\=\^!{}]+/
static int ai_text_char_ok(const unsigned char c) {
    return isalnum(c) || c == '_' || (c && strchr(":;()?,.|[] -+*$@=^!{}", c));
}

static int ai_text_ok(const char *text, const size_t max) {
    if (!text || !text[0] || strlen(text) > max) return 0;
    for (const unsigned char *c = (const unsigned char *) text; *c; c++) {
        if (!ai_text_char_ok(*c)) return 0;
    }
    return 1;
}

// .replace(/[^a-z0-9_]+/gmi, "").substring(0, n)
static void ai_key_text(const char *in, char *out, const size_t n) {
    size_t k = 0;
    for (const unsigned char *c = (const unsigned char *) (in ? in : ""); *c && k < n; c++) {
        if (isalnum(*c) || *c == '_') out[k++] = (char) *c;
    }
    out[k] = 0;
}

// The string of a JSON value as JS `v + ""` would give for the keys we read (strings, numbers).
static void json_text(const cJSON *item, char *out, const size_t size) {
    out[0] = 0;
    if (cJSON_IsString(item)) snprintf(out, size, "%s", cJSON_GetStringValue(item));
    else if (cJSON_IsNumber(item)) {
        const double v = cJSON_GetNumberValue(item);
        if (v == trunc(v) && fabs(v) < 1e15) snprintf(out, size, "%.0f", v);
        else snprintf(out, size, "%g", v);
    }
}

// addAI's per-action sanitize (MapManager.js addAI actions loop): tc, tc2, tct, tcc, tce, ta,
// es / esa, ev, er, ef, tad, cd + cv0..2 (raw; WP-C sanitizes them per condition).
static void ai_parse_action(TriggerActionData *a, const cJSON *raw) {
    memset(a, 0, sizeof(*a));
    a->action = json_int(raw, "ta");
    a->constant = json_int(raw, "tc");
    a->constant2 = json_int(raw, "tc2");
    a->value_type = json_int(raw, "tce");
    a->condition = json_int(raw, "cd");
    for (int i = 0; i < 3; i++) {
        const char key[4] = {'c', 'v', (char) ('0' + i), 0};
        const cJSON *cv = cJSON_GetObjectItem(raw, key);
        a->condition_values[i] = cv && !cJSON_IsNull(cv) ? cv : NULL;
    }
    a->delay = (float) ai_round(ai_limit(json_float(raw, "tad"), 0.0f, TRIGGER_V7_DELAY_MAX), 1);

    const cJSON *tct = cJSON_GetObjectItem(raw, "tct");
    if (cJSON_IsString(tct) && ai_text_ok(cJSON_GetStringValue(tct), TRIGGER_TEXT_MAX)) {
        a->has_text = 1;
        if (a->action == 37 || a->action == 38) ai_key_text(cJSON_GetStringValue(tct), a->text, TRIGGER_V7_KEY_MAX);
        else snprintf(a->text, sizeof(a->text), "%s", cJSON_GetStringValue(tct));
    }
    const cJSON *tcc = cJSON_GetObjectItem(raw, "tcc");
    if (cJSON_IsString(tcc)) ai_key_text(cJSON_GetStringValue(tcc), a->value_key, TRIGGER_V7_KEY_MAX);

    a->asset_sound = (unsigned char) json_present(raw, "esa");
    a->sound = json_truthy(cJSON_GetObjectItem(raw, "esa")) ? json_int(raw, "esa") : json_int(raw, "es"); // parseInt(esa || es) || 0
    if (!a->asset_sound && (a->sound < 0 || a->sound >= TRIGGER_V7_EMITTER_SOUND_COUNT)) a->sound = 0;
    const float ev = json_float(raw, "ev"), er = json_float(raw, "er");
    a->volume = ai_limit(ev ? ev : 1.0f, TRIGGER_EMITTER_VOLUME_MIN, TRIGGER_EMITTER_VOLUME_MAX);
    a->rate = ai_limit(er ? er : 1.0f, TRIGGER_V7_EMITTER_RATE_MIN, TRIGGER_EMITTER_RATE_MAX);
    a->emit_from = (int) ai_limit((float) json_int(raw, "ef"), 0.0f, 2.0f);
}

// map.js AI_SPAWNER: without a non-empty "tas" array the object-level trigger keys become tas[0].
static TriggerActionData *ai_parse_actions(const cJSON *object, int *count) {
    const cJSON *tas = cJSON_GetObjectItem(object, "tas");
    const int has_tas = cJSON_IsArray(tas) && cJSON_GetArraySize(tas) > 0;
    const int n = has_tas ? cJSON_GetArraySize(tas) : 1;
    TriggerActionData *actions = calloc((size_t) n, sizeof(TriggerActionData));
    *count = 0;
    if (!actions) return NULL;
    if (has_tas) {
        const cJSON *entry;
        cJSON_ArrayForEach(entry, tas) {
            if (cJSON_IsObject(entry)) ai_parse_action(&actions[(*count)++], entry); // if (actions[i])
        }
    } else {
        ai_parse_action(&actions[(*count)++], object);
    }
    return actions;
}

void game_ai_data_defaults(GameAIData *d) { // AI.init with an empty dat
    memset(d, 0, sizeof(*d));
    ai_copy(d->src, sizeof(d->src), "ghost_0");
    d->opacity = 1.0f;
    d->effect_on_shot = d->effect_on_hit = -1;
    d->trig.event = -1;
    d->trig.event_index = -1;
    d->trig.action = -1;
    d->trig.volume = d->trig.rate = 1.0f;
    d->targets_players = 0;
    d->roam_time = 1000.0f;
}

static void ai_asset_flags(GameAIData *d) {
    const char *s = d->asset_text;
    d->has_anim = (unsigned char) (strchr(s, 'g') || strchr(s, 'q') || strchr(s, 'f'));
    char digits[GAME_AI_ASSET_SIZE];
    size_t n = 0;
    for (const char *c = s; *c && n + 1 < sizeof(digits); c++) {
        if (isdigit((unsigned char) *c)) digits[n++] = *c;
    }
    digits[n] = 0;
    d->asset = n ? atoi(digits) : 0; // assetID = asset.replace(/[^\d]/g, "")
}

void game_ai_data_set_asset(GameAIData *d, const char *asset) { // dat.asset = (asset || "") + "", its letters and assetID
    if (!d) return;
    ai_copy(d->asset_text, sizeof(d->asset_text), asset ? asset : "");
    ai_asset_flags(d);
}

// MapManager.buildAIData(obj, triggers) + addAI's clamps. `name` gets aiName ("" when invalid).
// actions: the parsed tas (owned by the caller's record), NULL when the object has no trigger.
static void ai_build_data(const cJSON *o, GameAIData *d, char *name, const size_t name_size, TriggerActionData **actions) {
    game_ai_data_defaults(d);
    const int has_aid = json_truthy(cJSON_GetObjectItem(o, "aid"));
    // addAI: no aid -> fct 2, fs 5, rs 2 (the ghost's texture strip, double sided)
    ai_copy(d->src, sizeof(d->src), has_aid ? "model" : "ghost_0");
    if (has_aid) json_text(cJSON_GetObjectItem(o, "aid"), d->asset_text, sizeof(d->asset_text));
    ai_asset_flags(d);

    // projOb
    if (json_truthy(cJSON_GetObjectItem(o, "pra"))) json_text(cJSON_GetObjectItem(o, "pra"), d->pj_asset, sizeof(d->pj_asset));
    d->pj_gravity_set = 1;
    d->pj_gravity = ai_limit(json_float_or(o, "prg", 0.0f) * 0.0002f, -2.0f, 2.0f); // pjG = prg * 0.0002 (AI.js init)
    d->pj_scale = ai_limit(json_float(o, "prsc") ? json_float(o, "prsc") : 1.0f, 0.1f, 10.0f);
    d->pj_model_scale = ai_limit(json_float(o, "prms") ? json_float(o, "prms") : 1.0f, 0.01f, 10.0f);
    d->pj_spin = ai_limit(json_float_or(o, "prs", 1.0f), 0.0f, 20.0f);
    d->pj_speed = (float) (json_int(o, "pjs") ? json_int(o, "pjs") : 1); // parseInt(pjs) || 1 (unclamped)
    d->pj_damage = ai_limit((float) (json_int(o, "pjd") ? json_int(o, "pjd") : 100), 0.0f, 1000.0f);
    d->pj_glow = (unsigned char) json_present(o, "prgl");
    d->pj_trans = (unsigned char) json_present(o, "prgt");

    const cJSON *h = cJSON_GetObjectItem(o, "h");
    d->health = (float) json_number(json_truthy(h) ? h : cJSON_GetObjectItem(o, "hp"), 1); // parseInt(h || hp) || 0
    d->respawn_t = json_float(o, "rt") * 1000.0f;
    d->respawn_r = (unsigned char) json_present(o, "rr");
    d->speed = ai_limit(json_float_or(o, "spd", 1.0f), 0.0f, 10.0f);
    d->turn_speed = ai_limit(json_float_or(o, "tsd", 1.0f), 0.0f, 30.0f);
    d->roam_time = ai_limit((float) (json_int(o, "rmt") ? json_int(o, "rmt") : 1000), 1000.0f, 10000.0f);
    d->roam_radius = ai_limit((float) json_int(o, "rmr"), 0.0f, 500.0f);
    d->gravity = ai_limit(json_float_or(o, "grv", 1.0f), 0.0f, 1.0f);
    d->fire_rate = (int) ai_limit((float) (json_int(o, "fr") ? json_int(o, "fr") : 2000), 200.0f, 10000.0f);
    d->shot_spread = ai_limit(json_float(o, "aac"), 0.0f, 10.0f);
    d->shoot_break = ai_limit((float) (json_present(o, "shb") && json_int(o, "shb") ? json_int(o, "shb") : 400), 0.0f, 5000.0f);
    d->shot_arch_y = ai_limit((float) json_int(o, "sha"), -90.0f, 90.0f);
    d->shot_off_h = (unsigned char) !json_present(o, "shof");
    d->shot_delay = ai_limit((float) json_int(o, "shdy"), 0.0f, 5000.0f);
    d->melee_delay = ai_limit((float) (json_int(o, "mldy") ? json_int(o, "mldy") : 400), 0.0f, 5000.0f);
    d->melee_shake = ai_limit(json_float(o, "mls"), 0.0f, 1.0f);
    d->shot_shake = ai_limit(json_float(o, "ss"), 0.0f, 1.0f);
    d->effect_on_shot = json_present(o, "se") ? (int) ai_limit((float) json_int(o, "se"), 0.0f, 9.0f) : -1;
    d->effect_on_hit = json_present(o, "he") ? (int) ai_limit((float) json_int(o, "he"), 0.0f, 9.0f) : -1;
    d->crit_dmg = ai_limit(json_float(o, "crd"), 0.0f, 5.0f);
    d->crit_hei = ai_limit(json_float(o, "crh"), 0.0f, 10.0f);
    d->hit_rate = ai_limit((float) (json_int(o, "hr") ? json_int(o, "hr") : 800), 200.0f, 10000.0f);
    d->melee_range = ai_limit((float) (json_int(o, "mlr") ? json_int(o, "mlr") : 10), 10.0f, 100.0f);
    d->melee_radius = ai_limit(json_float(o, "mlra"), 0.0f, (float) (AI_PI * 2.0));
    d->chase_distance = ai_limit((float) (json_int(o, "aic") ? json_int(o, "aic") : 10), 0.0f, 2000.0f);
    d->melee_damage = ai_limit((float) (json_int(o, "mld") ? json_int(o, "mld") : 10), 1.0f, 200.0f);
    d->vision = (int) ai_limit((float) (json_int(o, "vd") ? json_int(o, "vd") : 120), 1.0f, 1000.0f);
    d->idle_sound = json_int(o, "idl");
    d->death_sound = json_int(o, "dth");
    d->attack_sound = json_int(o, "atk");
    d->attack_hit_sound = json_int(o, "atkh");
    d->melee_sound = json_int(o, "mel");
    d->melee_hit_sound = json_int(o, "melh");
    d->aggro_sound = json_int(o, "agr");
    d->score = (int) ai_limit((float) json_int(o, "sp"), 0.0f, 10000.0f);
    // rew / rewm / rewr / rewn: public-game skin rewards (no-op natively, see the header)
    d->behaviour = (int) ai_limit((float) json_int(o, "bh"), 0.0f, 20.0f);
    d->ignore_on_stuck = (unsigned char) json_present(o, "igos");
    d->drift_on_stuck = (unsigned char) json_present(o, "dos");
    d->invincible_on_stuck = (unsigned char) json_present(o, "ivos");
    d->can_shoot = (unsigned char) json_present(o, "cs");
    d->can_hit = (unsigned char) json_present(o, "cnh");
    d->xray = (unsigned char) json_present(o, "xv");
    d->targets_ai = (unsigned char) json_present(o, "tAi");
    d->targets_players = (unsigned char) !json_present(o, "tPl");
    d->friendly = (unsigned char) json_present(o, "aif");
    d->explode = (unsigned char) json_present(o, "exp");
    d->no_vis = (unsigned char) json_truthy(cJSON_GetObjectItem(o, "v"));
    const float ms = json_float(o, "ms");
    d->m_size = ms ? (float) ai_round(ai_limit(ms, 0.01f, 100.0f), 2) : 0.0f; // parseFloat (NaN -> 0: mSize || 4)
    d->m_y_off = ai_limit(json_float(o, "myo"), -100.0f, 100.0f);
    d->m_r_off = json_float(o, "mro");
    d->lvl = (int) ai_limit((float) json_int(o, "ail"), 0.0f, 999.0f);
    d->tag_scale = ai_limit(json_float(o, "ains") ? json_float(o, "ains") : 1.0f, 0.0f, 10.0f);
    d->anim_speed = json_float(o, "as");
    d->sound_range = json_float(o, "sor");
    d->hbh = ai_limit(json_float(o, "hbh") ? json_float(o, "hbh") : 1.0f, 0.1f, 10.0f);
    d->hbw = ai_limit(json_float(o, "hbw") ? json_float(o, "hbw") : 1.0f, 0.1f, 10.0f);
    d->path_type = (int) ai_limit((float) json_int(o, "pht"), 0.0f, (float) (GAME_AI_PATH_TYPES - 1));
    d->tex_encode = (int) ai_limit((float) json_int(o, "ten"), 0.0f, (float) (AI_ENCODING_TYPES - 1));
    d->no_fog = (unsigned char) json_present(o, "nf");
    d->side = (int) ai_limit((float) (has_aid ? json_int(o, "rs") : 2), 0.0f, 2.0f);
    d->f_trans = (unsigned char) json_present(o, "ft");
    d->no_smooth = (unsigned char) json_present(o, "txs");
    d->alpha_test = (float) ai_round(ai_limit((float) json_int(o, "alt"), 0.0f, 1.0f), 2); // limitMM(parseInt(alphaTest) || 0, 0, 1)
    const cJSON *op = cJSON_GetObjectItem(o, "o");
    d->opacity = op && !cJSON_IsNull(op) && json_number(op, 0) != 1.0 ? (float) json_number(op, 0) : 1.0f;
    d->frames = (int) ai_limit((float) (has_aid ? json_int(o, "fct") : 2), 0.0f, 100.0f);
    d->frame_time = (float) ai_round(ai_limit(has_aid ? json_float(o, "fs") : 5.0f, 0.0f, 10.0f), 1) * 100.0f;
    d->bob_s = (float) ai_round(ai_limit((float) json_int(o, "bbS"), 0.0f, 10.0f), 2) / 1000.0f;
    d->bob_d = (float) ai_round(ai_limit((float) json_int(o, "bbD"), 0.0f, 10.0f), 2);
    d->phys_move = (unsigned char) json_present(o, "pmv");
    const float spin = ai_limit(json_float(o, "spn"), -20.0f, 20.0f);
    if (spin != 0.0f) {
        d->r_anim = spin * AI_DISK_SPIN;
        d->r_anim_axis = (int) ai_limit((float) json_int(o, "spa"), 0.0f, 1.0f);
    }
    d->team = json_int(o, "tm");
    d->can_destroy = (unsigned char) json_present(o, "cdy");
    d->start_destroyed = (unsigned char) (d->can_destroy && json_present(o, "sd"));
    d->spawn_chance = (float) ai_round(ai_limit(json_float(o, "rsc"), 0.0f, 100.0f), 2);
    d->interface_id = json_int(o, "in") ? (int) ai_limit((float) json_int(o, "in"), 0.0f, (float) AI_INTERFACE_MAX) : 0;

    // startAnim: <= 32 chars of the aiName charset
    const cJSON *ast = cJSON_GetObjectItem(o, "ast");
    if (cJSON_IsString(ast) && ai_text_ok(cJSON_GetStringValue(ast), 32)) ai_copy(d->start_anim, sizeof(d->start_anim), cJSON_GetStringValue(ast));

    // pathInput: "" unless a string of digits and commas
    const cJSON *phi = cJSON_GetObjectItem(o, "phi");
    if (cJSON_IsString(phi) && cJSON_GetStringValue(phi)[0]) {
        const char *s = cJSON_GetStringValue(phi);
        int ok = 1;
        for (const char *c = s; *c && ok; c++) ok = isdigit((unsigned char) *c) || *c == ',';
        // pathInput.split(","): each piece is a pathNodes key ("" never matches a node)
        for (const char *c = s; ok && d->path_count < GAME_AI_PATH_MAX;) {
            const char *end = strchr(c, ',');
            d->path[d->path_count++] = end == c ? -1 : atoi(c);
            if (!end) break;
            c = end + 1;
        }
    }

    // aiName: <= 16 characters of the charset, else ""
    const cJSON *ain = cJSON_GetObjectItem(o, "ain");
    if (name) ai_copy(name, name_size, cJSON_IsString(ain) && ai_text_ok(cJSON_GetStringValue(ain), 16) ? cJSON_GetStringValue(ain) : "");

    // triggers: eventN = canTrigger ? events[te || 15].name : null, onAI* only
    TriggerData *t = &d->trig;
    const int te = json_int(o, "te") ? json_int(o, "te") : 15;
    t->event = json_present(o, "ct") && te >= 0 && te < TRIGGER_V7_EVENT_COUNT && trigger_v7_event_is_ai(te) ? te : -1;
    t->event_index = t->event;
    t->chance = (int) ai_limit((float) json_int(o, "cha"), 0.0f, 100.0f);
    t->constant_event = (int) ai_limit((float) json_int(o, "tce"), 0.0f, 10000.0f);
    t->interface_target = json_int(o, "tin") ? (int) ai_limit((float) json_int(o, "tin"), 0.0f, (float) AI_INTERFACE_MAX) : 0;
    t->volume = t->rate = 1.0f;
    t->action = -1;
    if (actions) {
        *actions = NULL;
        if (t->event >= 0) {
            int count = 0;
            *actions = ai_parse_actions(o, &count);
            t->actions = *actions;
            t->action_count = *actions ? count : 0;
            if (t->action_count) { // the v3.1.1-shaped single-action fields mirror actions[0]
                const TriggerActionData *a = &t->actions[0];
                t->action = a->action;
                t->constant = a->constant;
                if (a->has_text) ai_copy(t->text, sizeof(t->text), a->text);
                t->asset_sound = a->asset_sound;
                t->sound = a->sound;
                t->volume = a->volume;
                t->rate = a->rate;
                t->emit_from = a->emit_from;
            }
        }
    }
}

// ---------------------------------------------------------------- built-in kinds (AI.js vA767)

// vA767[index]. No glow: AI.init (AI.js:328-333) builds a client's mesh from the obj conf
// (emissive #ffffff, glowText) only without `dat`, and a v7 client always has one (AI.js:1158
// cSpawn passes getData's dat), whose emissive comes from an "e" in the asset id alone.
static void ai_builtin(const int index, GameAIData *d) {
    game_ai_data_defaults(d);
    d->targets_players = 0;
    switch (index) {
        case 1: // Drowned Sailor (Raid boss minion, bh 5)
            ai_copy(d->src, sizeof(d->src), "model");
            ai_copy(d->asset_text, sizeof(d->asset_text), "30171e");
            d->no_fog = 1, d->m_y_off = 8.0f, d->side = 2, d->lvl = 30, d->can_shoot = 1, d->targets_players = 1;
            d->path[0] = 99, d->path_count = 1;
            d->vision = 700, d->projectile = 1, d->pj_damage = 20.0f, d->pj_speed = 0.9f, d->pj_scale = 1.6f, d->pj_model_scale = 4.0f;
            ai_copy(d->pj_asset, sizeof(d->pj_asset), "30401e");
            d->health = 1100.0f, d->score = 10, d->speed = 0.9f, d->fire_rate = 3600, d->turn_speed = 20.0f, d->effect_on_shot = 4;
            break;
        case 2: // Jack O' Lantern (Lord of Souls minion)
            ai_copy(d->src, sizeof(d->src), "model");
            ai_copy(d->asset_text, sizeof(d->asset_text), "50838e");
            d->can_hit = 1, d->m_y_off = 6.0f, d->hbh = 0.6f, d->rot_y = 1, d->vision = 700, d->targets_players = 1;
            d->melee_range = 20.0f, d->melee_damage = 20.0f, d->health = 700.0f, d->score = 10, d->speed = 0.6f, d->turn_speed = 4.0f;
            break;
        case 3: // Mirror Image (bh 4)
            ai_copy(d->src, sizeof(d->src), "model");
            ai_copy(d->asset_text, sizeof(d->asset_text), "50578q");
            d->no_fog = 1, d->target_furthest = 1, d->lvl = 50, d->speed = 2.9f, d->turn_speed = 5.5f, d->gravity = 0.4f;
            d->can_hit = 1, d->melee_range = 20.0f, d->melee_delay = 900.0f, d->hit_rate = 900.0f, d->melee_damage = 20.0f;
            d->targets_players = 1, d->vision = 400, d->chase_distance = 50.0f, d->m_size = 1.4f, d->m_r_off = 270.0f;
            d->health = 5000.0f, d->score = 100, d->melee_shake = 0.05f;
            d->aggro_sound = 3206, d->melee_sound = 3236, d->melee_hit_sound = 29672;
            d->anim_speed = 1.8f, d->m_y_off = -0.001f, d->hbh = 2.9f, d->hbw = 1.4f, d->effect_on_shot = 4;
            break;
        case 4: // Lost Soul (Lord of Souls minion)
            d->frames = 2, d->frame_time = 400.0f, d->side = 2, d->can_shoot = 1, d->vision = 500, d->projectile = 1;
            d->pj_damage = 10.0f, d->pj_speed = 0.9f, d->pj_scale = 1.6f, d->pj_model_scale = 1.4f;
            d->health = 300.0f, d->targets_players = 1, d->score = 10, d->speed = 0.8f, d->fire_rate = 2500, d->turn_speed = 20.0f;
            break;
        default: // 0: the ghost (Traitor's "<name>'s Ghost")
            // no frames, as in v7 (vA767[0], AI.js:3-14): its mesh maps the whole ghost_0 strip
            // (two 512px frames; the model's UVs cover one), so both half-frames and the black gutter
            // between them show. The Object Manager's Ghost adds the spawner's strip (game_ai_builtin_data).
            d->side = 2, d->bob_d = 1.0f, d->bob_s = 0.005f;
            break;
    }
    ai_asset_flags(d);
}

void game_ai_builtin_data(const int index, GameAIData *d) {
    ai_builtin(index, d);
}

// ---------------------------------------------------------------- list / records

GameAIs *game_ais(Game *game) {
    if (!game->ai) game->ai = calloc(1, sizeof(GameAIs));
    return game->ai;
}

static void ai_free_records(GameAIs *list) {
    for (int i = 0; i < list->record_count; i++) free(list->records[i].actions);
    free(list->records);
    list->records = NULL;
    list->record_count = list->record_capacity = 0;
}

static GameAIRecord *ai_new_record(GameAIs *list) {
    if (list->record_count >= list->record_capacity) {
        const int capacity = list->record_capacity ? list->record_capacity * 2 : 16;
        GameAIRecord *records = realloc(list->records, (size_t) capacity * sizeof(GameAIRecord));
        if (!records) return NULL;
        // records moved: re-point every trig.actions at its record (AIs copy data by value and
        // keep pointing at the actions block, which is a separate allocation and never moves)
        list->records = records;
        list->record_capacity = capacity;
    }
    GameAIRecord *r = &list->records[list->record_count++];
    memset(r, 0, sizeof(*r));
    return r;
}

void game_ai_reset(Game *game) {
    if (!game->ai) return;
    GameAIs *list = game->ai;
    ai_free_records(list);
    ai_parts_reset(list); // the AIs' parts and solids go; the model cache (AI.rigs) stays like v10.0.3's
    AiPartsState *parts = list->parts;
    const unsigned serial = list->serial;
    int bots[GAME_AI_MAP_BOT_MAX];
    const int bot_count = list->map_bot_count;
    memcpy(bots, list->map_bot_ids, sizeof(bots));
    memset(list, 0, sizeof(*list));
    list->serial = serial; // spawn serials keep growing: client meshes never mix two AIs
    list->parts = parts;
    memcpy(list->map_bot_ids, bots, sizeof(bots)); // map_bots_spawn_pending removes last round's map bots
    list->map_bot_count = bot_count;
}

void game_ai_free(Game *game) {
    if (!game || !game->ai) return;
    ai_free_records(game->ai);
    ai_parts_free(game->ai);
    free(game->ai);
    game->ai = NULL;
}

GameAI *game_ai_find(Game *game, const int sid) {
    if (!game->ai) return NULL;
    for (int i = 0; i < game->ai->count; i++) {
        if (game->ai->ais[i].sid == sid && !game->ai->ais[i].removed) return &game->ai->ais[i];
    }
    return NULL;
}

int game_ai_active_count(const Game *game, const int team) { // game.activeAIs(team)
    int n = 0;
    for (int i = 0; game->ai && i < game->ai->count; i++) {
        const GameAI *ai = &game->ai->ais[i];
        if (ai->active && !ai->removed && (!team || team == ai->team || ai->force_team_timer)) n++;
    }
    return n;
}

static int ai_generate_sid(const GameAIs *list) { // UTILS.generateSID
    for (int sid = 1;; sid++) {
        int used = 0;
        for (int i = 0; i < list->count && !used; i++) used = list->ais[i].sid == sid && !list->ais[i].removed;
        if (!used) return sid;
    }
}

static int ai_listed(const GameAIs *list) { // AI.ais.length
    int n = 0;
    for (int i = 0; i < list->count; i++) n += !list->ais[i].removed;
    return n;
}

// Drop the removed slots (deferred so GameAI pointers held during a tick stay valid).
static void ai_compact(GameAIs *list) {
    int w = 0;
    for (int r = 0; r < list->count; r++) {
        if (list->ais[r].removed) continue;
        if (w != r) list->ais[w] = list->ais[r];
        w++;
    }
    list->count = w;
}

static void ai_set_target_player(GameAI *ai, const Player *player) {
    memset(&ai->target, 0, sizeof(ai->target));
    ai->target_uid = -1;
    if (!player) return;
    ai->target.kind = AI_TARGET_PLAYER;
    ai->target.id = player->uid;
    ai->target_uid = player->uid;
}

static void ai_set_target_ai(GameAI *ai, const GameAI *other) {
    memset(&ai->target, 0, sizeof(ai->target));
    ai->target_uid = -1;
    if (!other) return;
    ai->target.kind = AI_TARGET_AI;
    ai->target.id = other->sid;
    ai->target.serial = other->serial;
}

static void ai_clear_target(GameAI *ai) {
    memset(&ai->target, 0, sizeof(ai->target));
    ai->target_uid = -1;
}

// AI.init(x, y, z, name, index, dat, sid). client: the RENDER branch (mSize x0.5 for assets).
static void ai_init(GameAI *ai, const vec3 position, const char *name, const int index, const GameAIData *dat, const int sid, const int client) {
    memset(ai, 0, sizeof(*ai));
    GameAIData builtin;
    if (!dat) {
        ai_builtin(index, &builtin);
        dat = &builtin;
    }
    const GameAIData *z = dat;

    ai->sid = sid;
    ai->index = index;
    ai->has_data = 1; // v7 getData always sends `dat` (this.dat is the built-in record too)
    ai->data = *z;
    ai->active = 1;
    ai_copy(ai->name, sizeof(ai->name), name);
    ai->bob_d = z->bob_d;
    ai->bob_s = z->bob_s;
    ai->frames = z->frames;
    ai->max_health = z->health;
    ai->health = ai->max_health;
    ai->speed = z->speed * 0.031f;
    ai->turn_speed = z->turn_speed * 0.002f;
    ai->gravity = z->gravity * 0.1f;
    ai->kill_score = z->score;
    ai->respawn_t = z->respawn_t;
    ai->behaviour_type = z->behaviour;
    ai->fire_rate = z->fire_rate ? (float) z->fire_rate : 2000.0f;
    if (ai->fire_rate < 0.0f) ai->fire_rate = 200.0f;
    ai->shoot_timer = ai->fire_rate;
    ai->shot_spread = z->shot_spread;
    ai->shot_offset = ai->behaviour_type == 4 ? 10.0f : 2.0f;
    ai->spawn_cap = 20;
    ai->m_size = (z->m_size ? z->m_size : 4.0f) * (z->asset ? (client ? 0.5f : 2.0f) : 1.0f);
    ai->scale = ai->m_size * (float) ai_round(z->hbw ? z->hbw : 1.0f, 1);
    ai->height = ai->m_size * (float) ai_round(z->hbh ? z->hbh : 1.0f, 1);
    float melee_range = z->melee_range;
    if (melee_range < 10.0f) melee_range = 10.0f;
    ai->data.melee_range = melee_range;
    ai->hit_range = melee_range * 0.7f; // meleeChaseRange
    ai->hit_delay_timer = -1.0f;
    ai->hit_damage = z->melee_damage ? z->melee_damage : 10.0f;
    ai->hit_rate = z->hit_rate ? z->hit_rate : 800.0f;
    if (ai->hit_rate < 0.0f) ai->hit_rate = 200.0f;
    ai->position = (vec3) {(float) ai_round(position.x, 1), (float) ai_round(position.y, 1), (float) ai_round(position.z, 1)};
    ai->start = ai->position;
    ai->old = ai->position;
    ai->on_ground = 1;
    ai_clear_target(ai);
    ai->old_target_uid = -1;
    ai->last_hit_uid = -1;
    ai->zapped_by = -1;
    ai->vision = z->vision ? (float) z->vision : 120.0f;
    ai->chase_distance = z->chase_distance ? z->chase_distance : 10.0f;
    ai->path_index = 0;
    ai->path_dir = 1;
    ai->roam_timer = 0.0f;
    ai->team = ai->team_saved = z->team;
    ai->always_name = ai->height >= 6.0f;
    ai->idle_timer = AI_IDLE_SOUND_INTERVAL * ai_rand_float(0.8f, 1.2f);
    for (int i = 0; i < GAME_AI_IGNORE_MAX; i++) ai->ignore_uid[i] = -1;
}

GameAI *game_ai_spawn_ex(Game *game, int sid, const int index, const char *name, const vec3 position, const GameAIData *data, const int force) {
    GameAIs *list = game_ais(game);
    if (!list) return NULL;
    if (!force && ai_listed(list) >= GAME_AI_LIMIT) return NULL; // AI.spawn: ais.length >= config.aiLimit
    if (list->count >= GAME_AI_MAX) {
        if (list->updating) return NULL;
        ai_compact(list);
        if (list->count >= GAME_AI_MAX) return NULL;
    }
    if (!sid) sid = ai_generate_sid(list);

    GameAI *ai = &list->ais[list->count++];
    ai_init(ai, position, name, index, data, sid, !game_is_authority(game));
    ai->serial = ++list->serial;
    ai_parts_spawned(game, ai); // idleT0 = now (v10.0.3 AI.init, deob-10.0.3.js:204903)
    return ai;
}

GameAI *game_ai_spawn(Game *game, const int sid, const int index, const char *name, const vec3 position, const GameAIData *data) {
    return game_ai_spawn_ex(game, sid, index, name, position, data, 0);
}

// ---------------------------------------------------------------- map AIs (map_ai.h)

static GameAIRecord *ai_record_from_json(GameAIs *list, const cJSON *object, const int pending) {
    GameAIRecord *r = ai_new_record(list);
    if (!r) return NULL;
    ai_build_data(object, &r->data, r->name, sizeof(r->name), &r->actions);
    r->pending = (unsigned char) pending;
    const cJSON *p = cJSON_GetObjectItem(object, "p");
    // map.js: p = limit(p, positionRange).round(2)
    r->position = (vec3) {
        (float) ai_round(ai_limit((float) json_number(cJSON_GetArrayItem(p, 0), 0), -500000.0f, 500000.0f), 2),
        (float) ai_round(ai_limit((float) json_number(cJSON_GetArrayItem(p, 1), 0), -500000.0f, 500000.0f), 2),
        (float) ai_round(ai_limit((float) json_number(cJSON_GetArrayItem(p, 2), 0), -500000.0f, 500000.0f), 2),
    };
    return r;
}

// MapManager.addAI (pendingAIs, at most config.aiLimit) + addPendingAIs at game.init. addAI is
// gated on !RENDER, so v7 only has map AIs on the server; KrunkNative's offline game is an
// authority too and keeps them playable (ai_render.c hides them in the menu, as AI.sync would).
// competitive: game.js `connectedClients <= 2 && isPubType() || isComp` skips the spawn chance roll.
void map_ai_spawn_pending(Game *game) {
    if (!game_is_authority(game) || !game->map) return;
    GameAIs *list = game_ais(game);
    if (!list) return;
    const Map *map = game->map;
    for (size_t i = 0; i < map->ai_spawner_count && i < GAME_AI_LIMIT; i++) ai_record_from_json(list, map->ai_spawners[i], 1);

    for (int i = 0; i < list->record_count; i++) {
        GameAIRecord *r = &list->records[i];
        if (!r->pending) continue;
        const GameAIData *d = &r->data;
        int spawn = !d->can_destroy || !d->start_destroyed;
        if (d->spawn_chance && d->start_destroyed && d->can_destroy && ai_rand_float(0.0f, 100.0f) <= d->spawn_chance) spawn = 1;
        if (spawn) game_ai_spawn(game, 0, 0, r->name, r->position, d); // records may move: re-fetch r next loop
    }
}

void game_ai_spawn_map(Game *game) {
    map_ai_spawn_pending(game);
}

int map_ai_spawn(Game *game, const cJSON *data, const vec3 position, const int team) {
    if (!game_is_authority(game) || !data) return -1;
    GameAIs *list = game_ais(game);
    if (!list) return -1;
    GameAIRecord *r = ai_record_from_json(list, data, 0);
    if (!r) return -1;
    if (team >= 0) r->data.team = team;
    char name[GAME_AI_NAME_SIZE];
    ai_copy(name, sizeof(name), r->name);
    const GameAIData d = r->data;
    GameAI *ai = game_ai_spawn_ex(game, 0, 0, name, position, &d, 1); // zom.js: addAI(..., true) bypasses the limit
    return ai ? ai->sid : -1;
}

// Trigger action 36 "Spawn AI": every pending AI with interface == interfaceT (and not the source's).
int map_ai_spawn_interface(Game *game, const int source_interface, const int interface_target) {
    if (!game_is_authority(game) || !game->ai) return 0;
    int spawned = 0;
    for (int i = 0; i < game->ai->record_count; i++) {
        const GameAIRecord *r = &game->ai->records[i];
        if (!r->pending || r->data.interface_id == source_interface || r->data.interface_id != interface_target) continue;
        char name[GAME_AI_NAME_SIZE];
        ai_copy(name, sizeof(name), r->name);
        const GameAIData d = r->data;
        const vec3 at = r->position;
        if (game_ai_spawn(game, 0, 0, name, at, &d)) spawned++;
    }
    return spawned;
}

// Trigger action 70 "Force AI Target": AI.forceTarget = the trigger's {x, y, z} for 30 s.
void map_ai_force_target_point(Game *game, const vec3 point) {
    if (!game_is_authority(game)) return;
    GameAIs *list = game_ais(game);
    if (!list) return;
    list->force_target_timer = 30000.0f;
    list->force_target = point;
}

void map_ai_force_target(Game *game, const int interface_id, Player *target) { // WP-A stub signature
    (void) interface_id;
    if (target) map_ai_force_target_point(game, target->position);
}

// Trigger action 39 "Play Animation" (AI half): active AIs with interface == tc play `clip` once.
void map_ai_play_anim_interface(Game *game, const int interface_id, const char *clip) {
    for (int i = 0; game->ai && i < game->ai->count; i++) {
        GameAI *ai = &game->ai->ais[i];
        if (ai->active && !ai->removed && ai->data.interface_id == interface_id) game_ai_play_anim(game, ai, clip, 1);
    }
}

// ---------------------------------------------------------------- sight / targets

// game.js canSee: the objects that block (active, not noShoot, not complexColl; transparent too).
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
        // v10's addObjective / addBombSite keep these volumes out of manager.objects, the list canSee walks
        // (as player.c's shot test does). Fixed KN bug (the user: bots "have a party in the zones"): a
        // Hardpoint / KOTH objective or a bomb site (collision forced) hid everyone inside it from the
        // bots, so enemies on one point stood next to each other without a shot
        case PREFAB_OBJECTIVE:
        case PREFAB_BOMB_SITE:
            return 0;
        default:
            return 1;
    }
}

static double js_min(const double a, const double b) { return a != a || b != b ? NAN : a < b ? a : b; }
static double js_max(const double a, const double b) { return a != a || b != b ? NAN : a > b ? a : b; }

// Game.canSee(from, x, y, z) == null: from the eye (from.y + height - cameraHeight) along feet ->
// target; any object box entered before the end blocks (`ah && ah < 1`).
// `pad` (canSee's pad, vPad false): every box shrinks by it on each side, floored at 0.
static int ai_can_see_from_pad(const Game *game, const vec3 feet, const float height, const vec3 target, const double pad) {
    if (!game->map) return 1;
    const double ox = feet.x, oz = feet.z;
    const double oy = feet.y + height - game_constants.camera_height;
    const double ix = 1.0 / (target.x - feet.x), iy = 1.0 / (target.y - feet.y), iz = 1.0 / (target.z - feet.z);

    for (size_t i = 0; i < game->map->object_count; i++) {
        const Object *object = game->map->objects[i];
        if (!ai_object_blocks(object) || object->complex_coll) continue;
        const vec3 size = object_box_size(object);
        const double hx = fmax(0.0, size.x * 0.5 - pad), hz = fmax(0.0, size.z * 0.5 - pad);
        const double cy = object->position.y + size.y * 0.5, hy = fmax(0.0, size.y * 0.5 - pad);
        const double a = (object->position.x - hx - ox) * ix, b = (object->position.x + hx - ox) * ix;
        const double c = (cy - hy - oy) * iy, d = (cy + hy - oy) * iy;
        const double e = (object->position.z - hz - oz) * iz, f = (object->position.z + hz - oz) * iz;
        const double near = js_max(js_max(js_min(a, b), js_min(c, d)), js_min(e, f));
        const double far = js_min(js_min(js_max(a, b), js_max(c, d)), js_max(e, f));
        if (!(far < 0.0) && !(near > far) && near == near && near != 0.0 && near < 1.0) return 0;
    }
#ifdef WORLD_HAVE_TERRAIN_FRACTION // canSee's terrain.raycast (WP-B, shared/include/world.h)
    const vec3 eye = {feet.x, (float) oy, feet.z};
    const vec3 segment = {target.x - feet.x, target.y - feet.y, target.z - feet.z};
    const float t = world_terrain_fraction(game, eye, segment);
    if (t > 0.0f && t < 1.0f) return 0;
#endif
    return 1;
}

static int ai_can_see_from(const Game *game, const vec3 feet, const float height, const vec3 target) {
    return ai_can_see_from_pad(game, feet, height, target, 0.0);
}

int game_ai_player_can_see(const Game *game, const Player *origin, const vec3 target) {
    return ai_can_see_from(game, origin->position, origin->height, target);
}

int game_can_see_point(const Game *game, const vec3 from, const vec3 target, const float pad) {
    return ai_can_see_from_pad(game, from, 0.0f, target, pad);
}

// A target candidate of findNearestTo / findFurthestFrom (players first, then AIs).
typedef struct {
    Player *player;
    GameAI *ai;
    float distance;
} AICandidate;

static int ai_ignored(const GameAI *self, const int uid) {
    for (int i = 0; i < GAME_AI_IGNORE_MAX; i++) {
        if (self->ignore_uid[i] == uid) return 1;
    }
    return 0;
}

// game.findNearestTo / findFurthestFrom(from, x, y, z, team, maxDist, ignoreCanSee, includeAIs,
// includePlayers[, noBots, exclude = ignoreList]).
static AICandidate ai_find_target(Game *game, GameAI *self, const vec3 point, const int furthest) {
    AICandidate best = {0};
    const int team = self->team;
    const int players = self->data.targets_players, ais = self->data.targets_ai;
    const float range = self->vision;
    for (size_t i = 0; players && i < game->player_count; i++) {
        Player *p = game->players[i];
        if (!p || !p->active || p->notarget) continue; // /notarget
        if (!furthest && ai_ignored(self, p->uid)) continue;
        const float distance = ai_d3d(point, p->position);
        if (!(distance <= range)) continue;
        if ((best.player || best.ai) && (furthest ? !(best.distance < distance) : !(best.distance > distance))) continue;
        if (!self->data.xray && !ai_can_see_from(game, p->position, p->height, point)) continue;
        if (team && team == p->team) continue;
        best = (AICandidate) {p, NULL, distance};
    }
    for (int i = 0; ais && game->ai && i < game->ai->count; i++) {
        GameAI *a = &game->ai->ais[i];
        if (!a->active || a->removed || a == self) continue;
        const float distance = ai_d3d(point, a->position);
        if (!(distance <= range)) continue;
        if ((best.player || best.ai) && (furthest ? !(best.distance < distance) : !(best.distance > distance))) continue;
        if (!self->data.xray && !ai_can_see_from(game, a->position, a->height, point)) continue;
        if (team && team == a->team) continue;
        best = (AICandidate) {NULL, a, distance};
    }
    return best;
}

float game_ai_line_hit(const GameAI *ai, const vec3 origin, const vec3 direction) {
    // lineInRect(x - scale, z - scale, y, x + scale, z + scale, y + height * 2)
    const vec3 box = {ai->scale * 2.0f, ai->height * 2.0f, ai->scale * 2.0f};
    return line_in_rect(origin, direction, ai->position, box);
}

// v10.0.3: usesHitTest() ? hitTest(...) (and no box) : lineInRect (deob-10.0.3.js:196017-196030, :196677-196689,
// :206849-206860)
float game_ai_ray_hit(Game *game, GameAI *ai, const vec3 origin, const vec3 direction, GameAIHitPart *part) {
    if (part) memset(part, 0, sizeof(*part));
    if (ai_parts_hit_model(game, ai)) {
        float t = -1.0f, mult = 1.0f;
        char name[AI_PART_NAME_SIZE];
        if (!ai_parts_hit_test(game, ai, origin, direction, &t, name, sizeof(name), &mult)) return -1.0f;
        if (part) {
            part->has_part = 1;
            snprintf(part->name, sizeof(part->name), "%s", name);
            part->mult = mult;
        }
        return t;
    }
    return game_ai_line_hit(ai, origin, direction);
}

// The target's position (and its crouch for the aim), 0 when it is gone.
static int ai_target_position(Game *game, GameAI *ai, vec3 *out, float *crouch, int *is_player) {
    *crouch = 0.0f;
    *is_player = 0;
    switch (ai->target.kind) {
        case AI_TARGET_PLAYER: {
            const Player *p = game_find_player(game, ai->target.id);
            if (!p || p->notarget) return 0; // /notarget drops a current target too
            *out = p->position;
            *crouch = p->crouch_val;
            *is_player = 1;
            return 1;
        }
        case AI_TARGET_AI: {
            const GameAI *a = game_ai_find(game, ai->target.id);
            if (!a || a->serial != ai->target.serial) return 0;
            *out = a->position;
            return 1;
        }
        case AI_TARGET_POINT:
            *out = ai->target.point;
            return 1;
        default:
            return 0;
    }
}

// ---------------------------------------------------------------- sounds

// AI.emitSound(id, vol, x, y, z): game.playSound(id, (vol || 1) * 0.9, 0, round(x || ai.x), ..,
// true, soundR || 1, ...): "s" to everyone with the raw flag -> a 3D user-asset sound.
static void ai_emit_sound(Game *game, const GameAI *ai, const int id, const float volume, const vec3 *at) {
    if (!id || !game_is_authority(game)) return;
    const vec3 p = at ? *at : ai->position;
    const vec3 position = {roundf(p.x ? p.x : ai->position.x), roundf(p.y ? p.y : ai->position.y), roundf(p.z ? p.z : ai->position.z)};
    char name[32];
    snprintf(name, sizeof(name), "asset:%d", id);
    const float rate = ai->data.sound_range ? ai->data.sound_range : 1.0f;
    game_play_sound(game, name, -1, position, (volume ? volume : 1.0f) * 0.9f, rate); // shared/src/triggers.c
}

// ---------------------------------------------------------------- triggers

// game.onTrigger(player, ai) for one of the AI's onAI* events (this[eventN]).
static void ai_trigger(Game *game, GameAI *ai, const int event, Player *player) {
    if (!game_is_authority(game) || ai->data.trig.event != event) return;
#ifdef TRIGGER_HAVE_AI_EVENT
    trigger_ai_event(game, ai, event, player); // WP-C (shared/src/triggers_v7.c)
#else
    if (event == 15) {
        const int saved = ai->data.trig.event;
        ai->data.trig.event = TRIGGER_EVENT_ON_AI_DEATH;
        game_trigger_ai_death(game, player, ai); // v3.1.1-shaped single action until WP-C lands
        ai->data.trig.event = saved;
    }
#endif
}

// ---------------------------------------------------------------- effects (AI.giveEffect / tickAI)

#ifdef EFFECT_STACK_MAX // the v7 effects table (player-viewer WP-B) is in shared.h
#define AI_HAVE_EFFECTS 1
#define AI_EFFECT_ZAPPED EFFECT_ZAPPED

static const EffectConfig *ai_effect_config(const Game *game, const int effect) {
    return game && game->effects ? &game->effects[effect] : &g_effects[effect];
}

static void ai_effect_execute(Game *game, GameAI *ai, const int effect) {
    if (effect != EFFECT_ZAPPED) return; // the only attribute an AI reads (tickAI: attributes.zap)
    const EffectConfig *e = ai_effect_config(game, effect);
    ai->zap = (float) ai->effects[effect] * e->mlt * e->hp;
}

static void ai_push_effect_timer(GameAI *ai, const int effect, const float ms) {
    if (ai->effect_timer_count[effect] < GAME_AI_EFFECT_STACKS) ai->effect_timers[effect][ai->effect_timer_count[effect]++] = ms;
}

void game_ai_give_effect(Game *game, GameAI *ai, const int effect, int count) {
    if (!ai || effect < 0 || effect >= EFFECT_COUNT || effect >= GAME_AI_EFFECTS) return;
    const EffectConfig *e = ai_effect_config(game, effect);
    if (count <= 0) count = 1;
    const int stacks = ai->effects[effect] + count;
    if (stacks > e->max) {
        const int over = stacks - e->max;
        ai->effects[effect] = (unsigned char) e->max;
        if (e->timer > 0.0f) {
            for (int i = 0; i < over && i < GAME_AI_EFFECT_STACKS; i++) {
                if (i < ai->effect_timer_count[effect]) ai->effect_timers[effect][i] = e->timer * 1000.0f;
                else ai_push_effect_timer(ai, effect, e->timer * 1000.0f);
            }
        }
        return; // v7 returns without execute()
    }
    ai->effects[effect] = (unsigned char) stacks;
    if (e->timer > 0.0f) {
        for (int i = 0; i < count; i++) ai_push_effect_timer(ai, effect, e->timer * 1000.0f);
    }
    ai_effect_execute(game, ai, effect);
}

static void ai_remove_effect(Game *game, GameAI *ai, const int effect) { // removeEffect(i, 1)
    if (ai->effects[effect]) ai->effects[effect]--;
    if (ai->effects[effect]) {
        ai_effect_execute(game, ai, effect);
        return;
    }
    ai->effect_timer_count[effect] = 0; // clearEffect
    if (effect == EFFECT_ZAPPED) ai->zap = 0.0f;
}

static float ai_player_siphon(const Player *p) { return p ? p->attributes.siphon : 0.0f; }
static void ai_player_effect(Player *p, const int effect) { player_give_effect(p, effect, 1); }
#else
#define AI_HAVE_EFFECTS 0
#define AI_EFFECT_ZAPPED 8 // effects.js "Zapped"

void game_ai_give_effect(Game *game, GameAI *ai, const int effect, const int count) {
    (void) game, (void) ai, (void) effect, (void) count; // no effects table in this build
}

static void ai_remove_effect(Game *game, GameAI *ai, const int effect) {
    (void) game;
    ai->effects[effect] = 0;
    ai->effect_timer_count[effect] = 0;
}

static float ai_player_siphon(const Player *p) { (void) p; return 0.0f; }
static void ai_player_effect(Player *p, const int effect) { (void) p, (void) effect; }
#endif

// ---------------------------------------------------------------- health

static void ai_send_hit(Game *game, const GameAI *ai, const Player *dealer, const float damage, const int crit) {
    if (!dealer) return;
    const NetHitPacket hit = {
        .target_uid = ai->sid,
        .damage = (int32_t) roundf(damage),
        .crit = (uint8_t) crit,
        .kill = (uint8_t) !ai->active,
        .ai = 1,
    };
    game_send_hit(game, dealer->uid, &hit, damage); // game.showHit(player, ai, dmg, crit, true)
}

void game_ai_remove(Game *game, GameAI *ai) { // AI.removeFully
    if (!ai || ai->removed) return;
    ai->active = 0;
    ai->removed = 1;
    ai->respawn_timer = 0.0f;
    ai_parts_drop_solid(game, ai); // removeFully: solidPose = null; applySolid() (deob-10.0.3.js:206589)
    if (game_is_authority(game)) {
        const NetAIRemovePacket packet = {.sid = ai->sid};
        for (int i = 0; i < ai->sent_count; i++) game_send(game, ai->sent_to[i], NET_PACKET_AI_REMOVE, &packet, sizeof(packet));
        if (ai->sent_non_players) game_send(game, NET_TARGET_NON_PLAYERS, NET_PACKET_AI_REMOVE, &packet, sizeof(packet));
    }
    // the slot is dropped at the next tick (ai_compact): callers may hold GameAI pointers (/kill)
}

// AI.kill(killer, silent)
static void ai_kill(Game *game, GameAI *ai, Player *source, const int silent) {
    if (!ai->active) return;
    ai->active = 0;
    ai->health = 0.0f;
    ai->path_index = 0;
    ai->path_dir = 1;
    ai->respawn_timer = 0.0f;
    if (!silent) {
        if (source) source->mode.ai_kills++; // aiKills (Zombies endStats "zombies", map-logic WP-H)
        ai_emit_sound(game, ai, ai->data.death_sound, 0.0f, NULL);
        ai->respawn_timer = ai->data.respawn_r ? (float) ai_rand_int(100, ai->respawn_t ? (int) ai->respawn_t : 5000) : ai->respawn_t;
        ai_trigger(game, ai, 15, source); // onAIDeath
#ifdef SPAWNABLES_HAVE_AI_HOOKS
        if (game_is_authority(game)) mode_v7_on_ai_death(game, ai, source); // game.mode.onAIDeath (WP-D)
#endif
        // v10.0.3 KrunkScript onAIDeath(sid, name, killerId) after mode.onAIDeath (deob-10.0.3.js:206338-206341)
        if (game_is_authority(game)) ks_game_on_ai_death(game, ai->sid, ai->name, source);
        // reward && isPubType(): skin drops (not ported, see the header)
        if (source && ai_player_siphon(source) > 0.0f) player_change_health(source, NULL, -ai_player_siphon(source), 0); // siphon heals
    }
    if (!ai->respawn_timer) game_ai_remove(game, ai);
}

void game_ai_kill(Game *game, GameAI *ai, Player *source) {
    ai_kill(game, ai, source, 0);
}

static void ai_aggro(Game *game, GameAI *ai, const int is_player, Player *player, GameAI *other) {
    const int same = is_player ? ai->target.kind == AI_TARGET_PLAYER && ai->target.id == player->uid
                               : ai->target.kind == AI_TARGET_AI && ai->target.id == other->sid;
    if (!same) ai_emit_sound(game, ai, ai->data.aggro_sound, 0.0f, NULL);
    if (is_player) {
        if (!ai_ignored(ai, player->uid)) {
            ai_set_target_player(ai, player);
            ai->action_freq = 5000.0f;
        }
    } else {
        ai_set_target_ai(ai, other);
        ai->action_freq = 5000.0f;
    }
}

// AI.changeHealth(amount, source, forceTarget, set) for a player (or no) source.
int game_ai_change_health(Game *game, GameAI *ai, float amount, Player *source, const int force_target, const int set) {
    const int was_active = ai->active;
    char part[AI_PART_NAME_SIZE];
    const int has_part = ai_parts_take_dmg_part(game, ai, part, sizeof(part)); // dmgPart = this.dmgPart; this.dmgPart = null
    if (source && zombies_insta_kill(game) && ai->health) amount = -(ai->health * 2.0f); // WP-H: instaKillTimer (AI.changeHealth)
    if (ai->health) {
        const float before = ai->health;
        if (set) ai->health = amount;
        else ai->health += amount;
        // v10.0.3 KrunkScript (server): onAIDamage(sid, playerId, dmg), then onAIPartDamage(sid, part, playerId, dmg) for a
        // model hit, before the death check (deob-10.0.3.js:206243-206249)
        if (!set && amount < 0.0f && game_is_authority(game)) {
            ks_game_on_ai_damage(game, ai->sid, source, -amount);
            if (has_part) ks_game_on_ai_part_damage(game, ai->sid, part, source, -amount);
        }
        if (ai->health <= 0.0f) {
            ai_kill(game, ai, source, 0);
        } else if (amount > 0.0f) {
            if (ai->health > ai->max_health) ai->health = ai->max_health;
        } else if (source) {
            ai_trigger(game, ai, 20, source); // onAIDamage
            if (before > ai->max_health / 2.0f && ai->health <= ai->max_health / 2.0f) ai_trigger(game, ai, 29, source); // onAIHalfHealth
        }
    }
    if (source) {
        if (amount < 0.0f) {
            source->damage_dealt -= amount;
            ai->last_hit_uid = source->uid;
            ai->last_hit_timer = AI_LAST_HIT_MS;
        }
        // `amount < this.behaviour == 1` is (amount < true) == amount < 1: damage (and 0) aggroes; v10.0.3: not while a
        // script sets the target (!this.scriptTarget, deob-10.0.3.js:206276)
        if (amount < 1.0f && !ai->script_target && (!source->team || source->team != ai->team) && (force_target || ai->target.kind == AI_TARGET_NONE) &&
            ai->data.targets_players) {
            ai_aggro(game, ai, 1, source, NULL);
        }
    }
    return was_active && !ai->active;
}

int game_ai_change_health_by_ai(Game *game, GameAI *ai, const float amount, GameAI *source) {
    if (source == ai) return 0;
    const int was_active = ai->active;
    char part[AI_PART_NAME_SIZE];
    const int has_part = ai_parts_take_dmg_part(game, ai, part, sizeof(part));
    if (ai->health) {
        ai->health += amount;
        if (amount < 0.0f && game_is_authority(game)) { // an AI source is no player: playerId "" (deob-10.0.3.js:206245)
            ks_game_on_ai_damage(game, ai->sid, NULL, -amount);
            if (has_part) ks_game_on_ai_part_damage(game, ai->sid, part, NULL, -amount);
        }
        if (ai->health <= 0.0f) ai_kill(game, ai, NULL, 0);
        else if (amount > 0.0f && ai->health > ai->max_health) ai->health = ai->max_health;
    }
    if (source && amount < 1.0f && !ai->script_target && (!source->team || source->team != ai->team) && ai->target.kind == AI_TARGET_NONE &&
        ai->data.targets_ai) {
        ai_aggro(game, ai, 0, NULL, source);
    }
    return was_active && !ai->active;
}

void game_ai_set_health(Game *game, GameAI *ai, const float value) {
    if (!ai) return;
    if (!game_is_authority(game)) {
        ai->health = value;
        return;
    }
    if (!ai->health) { // changeHealth only acts on a live AI; set it anyway (KrunkScript writes it)
        ai->health = value;
        return;
    }
    game_ai_change_health(game, ai, value, NULL, 0, 1);
}

void game_ai_damage_ex(Game *game, GameAI *ai, Player *dealer, float damage, const int force_target, const float hit_y, const int effect,
                       const int effect_count) {
    game_ai_damage_part(game, ai, dealer, damage, force_target, hit_y, effect, effect_count, NULL);
}

int game_ai_damage_part(Game *game, GameAI *ai, Player *dealer, float damage, const int force_target, const float hit_y, const int effect,
                        const int effect_count, const GameAIHitPart *part) {
    if (!ai || !ai->active || ai->invincible) return 0;
    int crit = 0;
    const int model_hit = part && part->has_part;
    if (model_hit) { // v10.0.3: part ? dmg * part.m (crit when part.m > 1) : the crit band (deob-10.0.3.js:196799-196805)
        damage *= part->mult;
        crit = part->mult > 1.0f;
    } else if (hit_y == hit_y && ai->data.crit_dmg && ai->data.crit_hei && ai->position.y + ai->height * 2.0f - hit_y < ai->data.crit_hei) {
        crit = 1;
        damage *= ai->data.crit_dmg;
    }
    if (effect >= 0 && effect < GAME_AI_EFFECTS && effect_count > 0) {
        game_ai_give_effect(game, ai, effect, effect_count);
        if (effect == AI_EFFECT_ZAPPED && dealer) ai->zapped_by = dealer->uid;
    }
    if (model_hit && !part->mult) { // armourHit: onAIPartDamage(sid, part, playerId, 0) while active, no damage (deob-10.0.3.js:206223)
        if (game_is_authority(game)) ks_game_on_ai_part_damage(game, ai->sid, part->name, dealer, 0.0);
        return 1;
    }
    ai_parts_set_dmg_part(game, ai, model_hit ? part->name : NULL); // dmgPart = part || null
    game_ai_change_health(game, ai, -damage, dealer, force_target, 0);
    if (!dealer) return 0;
#ifdef SPAWNABLES_HAVE_AI_HOOKS
    if (ai->kill_score && game->mode && game->mode->config.points_per_hit) player_score(dealer, (int) ceilf(damage * game->mode->config.points_per_hit), 1);
#endif
    if (!ai->active && ai->kill_score) player_score(dealer, ai->kill_score, 1);
    ai_send_hit(game, ai, dealer, damage, crit);
    return 0;
}

void game_ai_damage(Game *game, GameAI *ai, Player *dealer, const float damage, const int force_target) {
    game_ai_damage_ex(game, ai, dealer, damage, force_target, NAN, -1, 0);
}

// projectiles.js: an AI hit by a projectile of a player (owner) or of another AI (owner_ai_sid).
void game_ai_force_team(Game *game, GameAI *ai, const int team, const float ms, const float chance) {
    (void) game;
    if (chance && (float) ai_round(ai_rand_float(0.0f, 100.0f), 2) > chance) return;
    ai->team_saved = ai->team;
    ai->team = team;
    ai->force_team_timer = ms;
    ai_clear_target(ai);
    ai->action_freq = 0.0f;
}

// ---------------------------------------------------------------- animation

static int ai_clip_index(const char *clip) {
    for (int i = 0; i < (int) (sizeof(k_clip_names) / sizeof(k_clip_names[0])); i++) {
        if (!strcmp(k_clip_names[i], clip)) return i;
    }
    return -1;
}

// AI.playAnim(ai, clip, reps, raw) on the server: "ana" sid, raw ? clip : clipNames index, raw to
// players within 800 (spectators: all). Only hasAnim AIs.
void game_ai_play_anim(Game *game, GameAI *ai, const char *clip, const int raw) {
    game_ai_play_anim_reps(game, ai, clip, 1, raw);
}

void game_ai_play_anim_reps(Game *game, GameAI *ai, const char *clip, const int reps, const int raw) {
    game_ai_play_anim_hold(game, ai, clip, reps, raw, 0);
}

void game_ai_play_anim_hold(Game *game, GameAI *ai, const char *clip, const double reps_in, const int raw, const int hold) {
    if (!ai || !ai->data.has_anim || !clip || !clip[0]) return;
    const int reps = (int) reps_in;
    // v10.0.3 AI.playAnim on the server (deob-10.0.3.js:206455-206465): a raw clip sets clipState {n, t0, reps:
    // max(1, reps || 1), hold} (the pose of clipAt) and heldClip = hold ? clip : null (partsDirty when it changes)
    if (game_is_authority(game) && raw) ai_parts_play_anim(game, ai, clip, reps_in, hold);
    if (!game_is_authority(game)) {
        ai_copy(ai->clip, sizeof(ai->clip), clip);
        ai->clip_reps = reps;
        ai->clip_serial++;
        return;
    }
    if (!raw && ai_clip_index(clip) < 0) return;
    if (game->is_local) { // the in-process client reads the list: play it there, reps times (v10.0.2 playClip(clip, reps))
        ai_copy(ai->clip, sizeof(ai->clip), clip);
        ai->clip_reps = reps;
        ai->clip_serial++;
        return;
    }
    NetAIAnimPacket packet;
    memset(&packet, 0, sizeof(packet));
    packet.sid = ai->sid;
    packet.raw = (uint8_t) !!raw;
    ai_copy(packet.clip, sizeof(packet.clip), clip);
    for (size_t i = 0; i < game->player_count; i++) {
        const Player *p = game->players[i];
        // v10.0.3 (deob-10.0.3.js:206471): spectating || alwaysSync || within 800
        if (!p || !p->active || (!ai->data.always_sync && ai_d3d(p->position, ai->position) > GAME_AI_SYNC_RANGE)) continue;
        game_send(game, p->uid, NET_PACKET_AI_ANIM, &packet, sizeof(packet));
    }
    game_send(game, NET_TARGET_NON_PLAYERS, NET_PACKET_AI_ANIM, &packet, sizeof(packet)); // spectators
}

// ---------------------------------------------------------------- shots

void game_ai_projectile_config_for(const GameAI *ai, ProjectileConfig *out) {
    // Projectile.init(stored[1], projOb): spd * pjSp, grav = pjG ?? stored, hitB * pjSC,
    // dmg = ceil(dmg * (d || 1)), ex 40 / expS 60 when the AI explodes.
    *out = k_ai_shot;
    const GameAIData *d = &ai->data;
    out->speed *= d->pj_speed ? d->pj_speed : 1.0f;
    if (d->pj_gravity_set) out->gravity = d->pj_gravity;
    out->hit_box *= d->pj_scale ? d->pj_scale : 1.0f;
    out->damage = ceilf(out->damage * (d->pj_damage ? d->pj_damage : 1.0f));
    if (d->explode) {
        out->explode = 60.0f;
        out->explode_damage = 40.0f;
    }
}

// projectiles.init(x, y, z, yaw, pitch, stored[1], this, {}, true, projOb): KN's AI shot path is
// game_spawn_ai_projectile (ai_kind 1: old clients draw stored[1]'s orb) with this AI's config;
// the shot is remembered per projectile slot for the hit rules, v7 clients get its visuals.
void game_ai_fire(Game *game, GameAI *ai, const vec3 origin, const float yaw, const float pitch) {
    if (!game_is_authority(game) || !ai) return;
    GameAIs *list = game_ais(game);
    if (!list) return;
    int slot = -1;
    for (int i = 0; i < GAME_MAX_PROJECTILES && slot < 0; i++) {
        if (!game->projectiles[i].active) slot = i; // projectile_spawn takes the first free slot
    }
    if (slot < 0) return;
    ProjectileConfig config;
    game_ai_projectile_config_for(ai, &config);
    g_shot_override = &config;
    game_spawn_ai_projectile(game, 1, origin, yaw, pitch);
    g_shot_override = NULL;
    if (!game->projectiles[slot].active) return;
    list->shots[slot] = (GameAIShot) {
        .sid = ai->sid, .serial = ai->serial, .team = ai->team, .effect = ai->data.effect_on_shot,
        .no_objects = (unsigned char) (ai->behaviour_type == 5), .used = 1,
    };
    if (!game->is_local) {
        const NetAIShotPacket shot = {.projectile = slot, .sid = ai->sid};
        game_send(game, NET_TARGET_ALL, NET_PACKET_AI_SHOT, &shot, sizeof(shot));
    }
}

static const GameAIShot *ai_shot_of(const Game *game, const GameProjectile *projectile) {
    if (!game->ai || !projectile || projectile->owner_uid >= 0 || projectile->ai_kind != 1) return NULL;
    if (projectile->id < 0 || projectile->id >= GAME_MAX_PROJECTILES) return NULL;
    const GameAIShot *shot = &game->ai->shots[projectile->id];
    return shot->used ? shot : NULL;
}

int game_ai_projectile_skips_objects(const Game *game, const GameProjectile *projectile) {
    const GameAIShot *shot = ai_shot_of(game, projectile);
    return shot && shot->no_objects;
}

int game_ai_projectile_can_hit(Game *game, const GameProjectile *projectile, const GameAI *target) {
    if (!target || !target->active || target->removed) return 0;
    const Player *owner = projectile->owner_uid >= 0 ? game_find_player(game, projectile->owner_uid) : NULL;
    if (owner) return !target->team || target->team != owner->team; // a player's shot
    const GameAIShot *shot = ai_shot_of(game, projectile);
    if (!shot) return 0; // v3.1.1 AI shots and ownerless projectiles never hit AIs
    if (target->sid == shot->sid && target->serial == shot->serial) return 0;
    return !target->team || !shot->team || target->team != shot->team;
}

void game_ai_projectile_hit_ai(Game *game, const GameProjectile *projectile, GameAI *target, Player *owner, const float damage) {
    game_ai_projectile_hit_ai_part(game, projectile, target, owner, damage, NULL);
}

void game_ai_projectile_hit_ai_part(Game *game, const GameProjectile *projectile, GameAI *target, Player *owner, float damage,
                                    const GameAIHitPart *part) {
    if (!target || target->invincible) return;
    if (owner) { // effectInd (+ zappedBy) unless a teammate; changeHealth(-dmg, owner, wId == 12)
#ifdef EFFECT_STACK_MAX
        const int effect = projectile->effect >= 0 && (!owner->team || owner->team != target->team) ? projectile->effect : -1;
        game_ai_damage_part(game, target, owner, damage, projectile->thrown_melee, NAN, effect, projectile->effect_count, part);
#else
        game_ai_damage_part(game, target, owner, damage, projectile->thrown_melee, NAN, -1, 0, part);
#endif
        return;
    }
    const GameAIShot *shot = ai_shot_of(game, projectile);
    if (!shot) return;
    if (shot->effect >= 0) game_ai_give_effect(game, target, shot->effect, 1);
    // v10.0.3 (deob-10.0.3.js:206911-206920): part ? dmg * part.m; part.m 0 -> armourHit (no player: playerId "")
    if (part && part->has_part) {
        damage *= part->mult;
        if (!part->mult) {
            if (game_is_authority(game)) ks_game_on_ai_part_damage(game, target->sid, part->name, NULL, 0.0);
            return;
        }
    }
    ai_parts_set_dmg_part(game, target, part && part->has_part ? part->name : NULL);
    GameAI *shooter = game_ai_find(game, shot->sid);
    if (shooter && shooter->serial != shot->serial) shooter = NULL;
    if (shooter) game_ai_change_health_by_ai(game, target, -damage, shooter);
    else game_ai_change_health(game, target, -damage, NULL, 0, 0);
}

void game_ai_projectile_hit_player(Game *game, const GameProjectile *projectile, Player *target) {
    const GameAIShot *shot = ai_shot_of(game, projectile);
    if (!shot || !target || shot->effect < 0) return;
    ai_player_effect(target, shot->effect); // !owner.isPlayer: no team check
}

static void ai_fire(Game *game, GameAI *ai, const vec3 origin, const float yaw, const float pitch) {
    game_ai_fire(game, ai, origin, yaw, pitch);
}

static void ai_orbs(Game *game, GameAI *ai, const vec3 at, const int count) {
    double yaw = 0.0;
    for (int i = 0; i < count; i++) {
        ai_fire(game, ai, at, (float) yaw, 0.0f); // stored[1] with projOb, pitch 0, a ring
        yaw += AI_PI * 2.0 / count;
    }
}

// ---------------------------------------------------------------- update (server)

// The goal of this update: a target, the path node, the roam point or startP.
typedef struct {
    vec3 at;
    int node;      // a PATH_NODE (arrival distance 1, advances the path)
    int target;    // the target (aiChasd / melee chase range)
} AIGoal;

static const Object *ai_path_node(const Game *game, const int nid) { // manager.pathNodes[id]: the last one wins
    const Object *found = NULL;
    for (size_t i = 0; game->map && i < game->map->path_node_count; i++) {
        if (game->map->path_nodes[i].nid == nid && game->map->path_nodes[i].object < game->map->object_count) {
            found = game->map->objects[game->map->path_nodes[i].object];
        }
    }
    return found;
}

static void ai_advance_path(GameAI *ai) {
    const int n = ai->data.path_count;
    switch (ai->data.path_type) {
        case AI_PATH_LOOP:
            if (++ai->path_index >= n) ai->path_index = 0;
            break;
        case AI_PATH_ONCE:
            if (++ai->path_index >= n) ai->path_index = n - 1;
            break;
        case AI_PATH_PING_PONG:
            ai->path_index += ai->path_dir;
            if (ai->path_index >= n) {
                ai->path_index = n - 1;
                ai->path_dir *= -1;
            } else if (ai->path_index <= 0) {
                ai->path_index = 0;
                ai->path_dir *= -1;
            }
            break;
        case AI_PATH_RANDOM:
            ai->path_index = ai_rand_int(0, n - 1);
            break;
        default:
            break;
    }
}

// MapManager.collision for the AI box (collidesAcc: the swept box of old -> new).
static int ai_collides_acc(const GameAI *ai, const float ox, const float oy, const float oz, const float w, const float h, const float l) {
    const vec3 p = ai->position, o = ai->old;
    return fminf(p.x, o.x) - ai->scale < ox + w && fmaxf(p.x, o.x) + ai->scale > ox - w &&
           fminf(p.z, o.z) - ai->scale < oz + l && fmaxf(p.z, o.z) + ai->scale > oz - l &&
           fminf(p.y, o.y) <= oy + h && fmaxf(p.y, o.y) + ai->height * 2.0f >= oy - h;
}

// The objects an AI collides with (AI.js:838): not onEnterObject / onExitObject, flag, trigger or
// noAICol; complexColl objects are not in the grid. KN's pass-through volumes are
// COLLISION_TYPE_NONE: only AI_REDIRECTION matters to AIs. v7 also lets NOPVP / PVP zones block AIs
// (they are plain GameObjects there); fixed: they are player volumes, AIs walk through them.
static int ai_object_collides(const Object *o) {
    if (!o->active || o->no_ai_col || o->trigger || o->flag || o->complex_coll) return 0;
    if (o->redirection) return 1;
    if (o->collision_type == COLLISION_TYPE_NONE) return 0;
    if (o->trig && (o->trig->event_index == 3 || o->trig->event_index == TRIGGER_V7_EVENT_ON_EXIT_OBJECT)) return 0;
    return 1;
}

static void ai_collide_map(Game *game, GameAI *ai, const float q, const float mlt) {
    ai->on_ground = 0;
    if (!game->map) return;
    int ramp_set = 0;
    float ramp_top = 0.0f;
    for (size_t i = 0; i < game->map->object_count; i++) {
        const Object *o = game->map->objects[i];
        if (!ai_object_collides(o)) continue;
        // v7 boxes: centre x / y / z, half width / height / length
        const vec3 size = object_box_size(o);
        const float w = size.x * 0.5f, h = size.y * 0.5f, l = size.z * 0.5f;
        const float x = o->position.x, y = o->position.y + h, z = o->position.z;
        if (!ai_collides_acc(ai, x, y, z, w, h, l)) continue;
        vec3 *p = &ai->position;
        const float s = ai->scale;

        if (o->ramp) {
            if (p->y < y + h) {
                const float dir = (float) AI_PI * 0.5f * (float) o->direction;
                const vec2 at = {p->x + s * cosf(dir), p->z + s * sinf(dir)};
                const float progress = fmaxf(0.0f, fminf(1.0f, progress_on_line(o->ramp->start, o->ramp->end, at)));
                const float top = y - h + (h * 2.0f + 0.01f) * progress;
                if (!ramp_set) {
                    ramp_set = 1;
                    ramp_top = top;
                }
                if (p->y <= top && ramp_top <= top) {
                    ramp_top = top;
                    p->y = top;
                    ai->velocity.y = 0.0f;
                    ai->on_ground = 1;
                }
            }
        } else if (o->redirection) {
            // xVel / zVel along -dir + PI / 2 at cos(boostDr), boostDr = asin(sy / |(sy, sz)|)
            const float dir = (float) AI_PI * 0.5f * (float) o->direction;
            const float hyp = sqrtf(o->scale.y * o->scale.y + o->scale.z * o->scale.z);
            const float boost_dr = hyp > 0.0f ? asinf(o->scale.y / hyp) : 0.0f;
            const float v = ai->speed * mlt * q * 0.05f;
            ai->velocity.x = v * sinf(-dir + (float) AI_PI / 2.0f) * cosf(boost_dr);
            ai->velocity.z = v * cosf(-dir + (float) AI_PI / 2.0f) * cosf(boost_dr);
        } else if (p->y < y + h && y + h - p->y <= 3.0f && ai->old.y < y + h) {
            p->y = y + h;
            ai->velocity.y = 0.0f;
            ai->on_ground = 1;
        } else if (ai->old.y >= y + h) {
            p->y = y + h;
            ai->velocity.y = 0.0f;
            ai->on_ground = 1;
        } else if (ai->old.x - s >= x + w - 0.00001f) {
            p->x = x + w + s;
            ai->velocity.x = 0.0f;
        } else if (ai->old.x + s <= x - w + 0.00001f) {
            p->x = x - w - s;
            ai->velocity.x = 0.0f;
        } else if (ai->old.z - s >= z + l - 0.00001f) {
            p->z = z + l + s;
            ai->velocity.z = 0.0f;
        } else if (ai->old.z + s <= z - l + 0.00001f) {
            p->z = z - l - s;
            ai->velocity.z = 0.0f;
        } else if (p->y < y + h && p->y > y - h && ai->old.y > y - h && ai->old.y < y + h) {
            // embedded (fell into the object's footprint from below its top; the step-up above only
            // lifts up to 3 units, and nothing else matched): the top is the way out, else the AI falls
            // through the floor forever. KN fix for "walking AI falling through the floor" (v10.0.3)
            p->y = y + h;
            ai->velocity.y = 0.0f;
            ai->on_ground = 1;
        }
    }
}

// tickAI: Zapped damage every second 500 ms tick, then the timed effect stacks.
static void ai_tick_effects(Game *game, GameAI *ai, const float q) {
    ai->effect_ticker -= q;
    if (ai->effect_ticker <= 0.0f) {
        ai->effect_ticker = 500.0f;
        ai->effect_tick_count++;
        if (ai->zap > 0.0f && ai->effect_tick_count % 2 == 0) {
            Player *by = ai->zapped_by >= 0 ? game_find_player(game, ai->zapped_by) : NULL;
            const float zap = ai->zap;
            game_ai_change_health(game, ai, -zap, by, 0, 0);
            if (by) ai_send_hit(game, ai, by, zap, 0); // showHit(zappedBy, ai, zap, false, true)
            if (!ai->active) return;
        }
    }
    for (int e = 0; e < GAME_AI_EFFECTS; e++) {
        for (int k = ai->effect_timer_count[e] - 1; k >= 0; k--) {
            ai->effect_timers[e][k] -= q;
            if (ai->effect_timers[e][k] > 0.0f) continue;
            memmove(&ai->effect_timers[e][k], &ai->effect_timers[e][k + 1], (size_t) (ai->effect_timer_count[e] - k - 1) * sizeof(float));
            ai->effect_timer_count[e]--;
            ai_remove_effect(game, ai, e);
        }
    }
}

static void ai_stuck(GameAI *ai, const float q, const int moving) {
    const int chasing_player = ai->target.kind == AI_TARGET_PLAYER && ai->target.id == ai->old_target_uid;
    const int trying = ai->speed && ai->hit_delay_timer < 0.0f && !ai->wait_shoot && !ai->break_move && moving;
    if (ai->data.invincible_on_stuck) {
        if (trying && chasing_player && !ai->did_move) ai->stuck_time += q;
        else if (ai->did_move) {
            ai->stuck_time = 0.0f;
            if (!ai->perm_invincible) ai->invincible = 0;
        }
        if (trying && chasing_player && ai->stuck_time >= AI_STUCK_TIME_MS) {
            ai->stuck_count++;
            ai->stuck_time = 0.0f;
            ai->was_stuck = 1;
            ai->invincible = 1;
            if (ai->stuck_count == 4) ai->perm_invincible = 1; // + broadcast("so", "mystery_1", 2, 1)
        }
    } else if (ai->data.ignore_on_stuck) {
        if (trying && chasing_player && !ai->did_move) ai->stuck_time += q;
        else if (ai->did_move) ai->stuck_time = 0.0f;
        if (trying && chasing_player && ai->stuck_time >= AI_STUCK_TIME_MS) {
            ai->stuck_time = 0.0f;
            ai_clear_target(ai);
            ai->action_freq = 0.0f;
            int slot = -1;
            for (int i = 0; i < GAME_AI_IGNORE_MAX && slot < 0; i++) {
                if (ai->ignore_uid[i] == ai->old_target_uid || ai->ignore_uid[i] < 0) slot = i;
            }
            if (slot < 0) slot = 0;
            ai->ignore_uid[slot] = ai->old_target_uid;
            ai->ignore_timer[slot] = AI_STUCK_IGNORE_MS;
            ai->was_stuck = 1;
        }
    } else if (ai->data.drift_on_stuck) {
        if (!ai->force_direction_timer) {
            if (trying && chasing_player && !ai->did_move) ai->stuck_time += q;
            else if (ai->did_move) ai->stuck_time = 0.0f;
            if (trying && chasing_player && ai->stuck_time >= AI_STUCK_TIME_MS) {
                ai->stuck_time = 0.0f;
                ai->force_direction = ai_rand_int(0, 1) ? 1 : -1;
                ai->force_direction_timer = 2000.0f;
                ai->was_stuck = 1;
            }
        } else {
            ai->force_direction_timer -= q;
            if (ai->force_direction_timer <= 0.0f) {
                ai->force_direction_timer = 0.0f;
                ai->force_direction = 0;
            }
        }
    }
}

// The drift velocity of driftOnStuck (applied by the next pMov step, like v7).
static void ai_drift(GameAI *ai, const float q, const float mlt) {
    if (!ai->data.drift_on_stuck || !ai->force_direction_timer || !ai->force_direction) return;
    const float v = ai->speed * mlt * q * 0.01f;
    ai->velocity.x = (float) ai->force_direction * v * sinf(ai->x_dir - (float) AI_PI / 4.0f) * cosf(ai->y_dir);
    ai->velocity.z = (float) ai->force_direction * v * cosf(ai->x_dir - (float) AI_PI / 4.0f) * cosf(ai->y_dir);
}

// Melee: every player (and AI) in meleeRange of a non-team AI takes hitDMG (AI.js:738-767).
static void ai_melee_hit(Game *game, const int index) {
    GameAI *ai = &game->ai->ais[index];
    for (size_t i = 0; i < game->player_count; i++) {
        Player *p = game->players[i];
        if (!p || !p->active || (ai->team && ai->team == p->team)) continue;
        if (ai_d3d(ai->position, p->position) > ai->data.melee_range) continue;
        if (ai->data.melee_radius) {
            double yaw, pitch;
            ai_get_drs(p->position, ai->position, 0.0f, &yaw, &pitch);
            const double a = atan2(sin(ai->x_dir - yaw), cos(yaw - ai->x_dir)) - AI_PI / 2.0; // getAngleDst(drs[0], xD) - PI / 2
            if (fabs(a) > ai->data.melee_radius) continue;
        }
        if (player_change_health(p, NULL, ai->hit_damage, 0)) {
            player_kill(p, NULL, NULL, 0);
            ai = &game->ai->ais[index];
            ai_trigger(game, ai, 31, p); // onAIKillPlayer
        } else if (ai->data.effect_on_hit >= 0 && ai->data.effect_on_hit < GAME_AI_EFFECTS) {
            ai_player_effect(p, ai->data.effect_on_hit); // players.giveEffect(player, effectOnHit)
        }
        ai = &game->ai->ais[index];
    }
    for (int i = 0; i < game->ai->count; i++) {
        GameAI *other = &game->ai->ais[i];
        ai = &game->ai->ais[index];
        if (other == ai || other->removed || (ai->team && ai->team == other->team)) continue;
        if (ai_d3d(ai->position, other->position) - other->scale * 2.0f <= ai->data.melee_range + ai->scale) {
            game_ai_change_health_by_ai(game, other, -ai->hit_damage, ai);
        }
    }
}

// AI.update(q) on the server (q: ms * deltaMlt). Spawns below may grow game->ai->ais: callers
// re-fetch their AI by index after them.
static void ai_update(Game *game, const int index, const float q) {
    GameAIs *list = game->ai;
    GameAI *ai = &list->ais[index];
    if (ai->removed) return;
    // v10.0.3: `if (this.behavior == 0) return` before the respawn timer (deob-10.0.3.js:205618); only a script's
    // ai.behaviour = 0 sets it (AI.init never does)
    if (ai->has_script_behavior && ai->script_behavior == 0.0f) return;

    if (ai->respawn_timer) {
        ai->respawn_timer -= q;
        if (!(ai->respawn_timer <= 0.0f)) return;
        ai->active = 1;
        ai_clear_target(ai);
        for (int i = 0; i < GAME_AI_IGNORE_MAX; i++) ai->ignore_uid[i] = -1;
        ai->stuck_time = 0.0f;
        ai->old_target_uid = -1;
        ai->position = ai->start;
        ai->health = ai->max_health;
        ai->respawn_timer = 0.0f;
    }
    if (!ai->active) return;

    ai_tick_effects(game, ai, q);
    if (!ai->active) return;
    if (ai->break_move > 0.0f) {
        ai->break_move -= q;
        if (ai->break_move <= 0.0f) ai->break_move = 0.0f;
    }
    ai->old = ai->position;
    ai->old_target_uid = ai->target.kind == AI_TARGET_PLAYER ? ai->target.id : -1;
    ai->was_stuck = 0;
    if (ai->force_team_timer) {
        ai->force_team_timer -= q;
        if (ai->force_team_timer <= 0.0f) {
            ai->force_team_timer = 0.0f;
            ai->team = ai->team_saved;
            ai_clear_target(ai);
            ai->action_freq = 0.0f;
        }
    }
    for (int i = 0; i < GAME_AI_IGNORE_MAX; i++) {
        if (ai->ignore_uid[i] < 0) continue;
        ai->ignore_timer[i] -= q;
        if (ai->ignore_timer[i] <= 0.0f) ai->ignore_uid[i] = -1;
    }
    if (list->force_target_timer) {
        ai->action_freq = list->force_target_timer;
        memset(&ai->target, 0, sizeof(ai->target));
        ai->target.kind = AI_TARGET_POINT;
        ai->target.point = list->force_target;
        ai->target_uid = -1;
        ai->force_direction_timer = 0.0f;
        ai->force_direction = 0;
    }

    float mlt = 1.0f;
    const int has_target = ai->target.kind != AI_TARGET_NONE;
    if (ai->behaviour_type == 1 && has_target) { // Weeping Soul: breaks, orb rings, teleports
        ai->break_timer += q;
        if (ai->break_timer >= 5000.0f) {
            ai->break_timer = (float) -ai_rand_int(0, 5000);
            ai->break_attack = 4000.0f;
        }
        if (ai->health <= ai->max_health / 1.5f) {
            mlt = 1.5f;
            ai->special_timer -= q;
            if (ai->special_timer <= 0.0f) {
                ai->special_timer = (float) ai_rand_int(7000, 12000);
                ai->break_move = 2000.0f;
                ai->break_attack = 3000.0f;
                ai_orbs(game, ai, (vec3) {ai->position.x, ai->position.y + ai->shot_offset, ai->position.z}, 8);
            }
        }
        if (ai->health <= ai->max_health / 2.0f) {
            mlt = 2.0f;
            ai->special_timer2 -= q;
            if (ai->special_timer2 <= 0.0f) {
                ai_emit_sound(game, ai, AI_BOSS_SOUND, 0.0f, NULL);
                ai->special_timer2 = (float) ai_rand_int(12000, 16000);
                ai->special_timer = 0.0f;
                const float angle = ai_rand_float(-(float) AI_PI, (float) AI_PI);
                ai->position.x = ai->start.x + 100.0f * cosf(angle);
                ai->position.z = ai->start.z + 100.0f * sinf(angle);
            }
        }
    }
    if (ai->behaviour_type == 2 && has_target) { // Lord of Souls: Jack O' Lanterns, orb rings, Lost Souls
        if (ai->health <= ai->max_health / 2.0f) {
            if (ai->shoot_timer > 7000.0f) ai->shoot_timer = 7000.0f;
            if (!ai->used_special) {
                ai->used_special = 1;
                double g = 0.0;
                for (int i = 0; i < 6; i++) {
                    ai = &list->ais[index];
                    const float h = ai->scale * 2.0f + 70.0f;
                    const vec3 at = ai->position;
                    const vec3 spawn = {at.x + h * (float) cos(g), at.y + ai->scale * 2.0f + 10.0f, at.z + h * (float) sin(g)};
                    ai_emit_sound(game, ai, AI_BOSS_SOUND, 0.0f, &at);
                    game_ai_spawn(game, 0, 2, "Jack O' Lantern", spawn, NULL);
                    g += AI_PI * 2.0 / 6.0;
                }
                ai = &list->ais[index];
            }
        }
        ai->special_timer -= q;
        if (ai->special_timer <= 0.0f) {
            ai->special_timer = (float) ai_rand_int(5000, 10000);
            ai->break_attack = 1000.0f;
            ai_orbs(game, ai, (vec3) {ai->position.x, ai->position.y + 3.0f + ai->height + ai->shot_offset, ai->position.z}, 8);
        }
        if (ai->spawn_cap > 0) {
            ai->spawn_minion_timer += q;
            if (ai->spawn_minion_timer >= 6000.0f) {
                ai->spawn_cap--;
                ai->spawn_minion_timer = (float) -ai_rand_int(0, 5000);
                const float angle = ai_rand_float(-(float) AI_PI, (float) AI_PI);
                const float h = ai->scale * 2.0f + 100.0f;
                const vec3 spawn = {ai->position.x + h * cosf(angle), ai->position.y + ai->scale * 2.0f + 10.0f, ai->position.z + h * sinf(angle)};
                ai_emit_sound(game, ai, AI_BOSS_SOUND, 0.0f, &spawn);
                game_ai_spawn(game, 0, 4, "Lost Soul", spawn, NULL);
                ai = &list->ais[index];
            }
        }
    }
    if (ai->behaviour_type == 3 && has_target && ai->health <= ai->max_health / 2.0f) { // enrage
        if (ai->shoot_timer > 4000.0f) ai->shoot_timer = 4000.0f;
        mlt = 2.0f;
    }
    if (ai->behaviour_type == 4 && has_target && ai->health <= ai->max_health / 2.0f && !ai->used_special) { // mirror image
        ai->used_special = 1;
        const vec3 at = {ai->start.x, ai->start.y + 0.01f, ai->start.z};
        game_ai_spawn(game, 0, 3, "Mirror Image", at, NULL);
        ai = &list->ais[index];
    }
    if (ai->behaviour_type == 5) { // Drowned Sailor spawner
        ai->special_timer -= q;
        if (ai->special_timer <= 0.0f) {
            ai->special_timer = (float) ai_rand_int(10000, 20000);
            const vec3 at = ai->position;
            ai_emit_sound(game, ai, AI_BOSS_SOUND, 0.0f, &at);
            game_ai_spawn(game, 0, 1, "Drowned Sailor", (vec3) {at.x, at.y + 10.0f, at.z}, NULL);
            ai = &list->ais[index];
        }
    }

    int moving = 0;
    // behaviour 1 (every AI: init sets it; the `behavior == 0` early-out is a typo)
    ai->action_freq -= q;
    if (ai->script_target) { // v10.0.3 (deob-10.0.3.js:205765): a script's target is kept until it leaves or dies
        const Player *t = ai->target.kind == AI_TARGET_PLAYER ? game_find_player(game, ai->target.id) : NULL;
        if (ai->target.kind == AI_TARGET_PLAYER && (!t || !t->active)) ai_clear_target(ai);
        if (ai->target.kind == AI_TARGET_AI) {
            const GameAI *o = game_ai_find(game, ai->target.id);
            if (!o || !o->active || o->serial != ai->target.serial) ai_clear_target(ai);
        }
    } else if (ai->action_freq <= 0.0f) {
        const vec3 eye = {ai->position.x, ai->position.y + ai->height / 2.0f, ai->position.z};
        const AICandidate found = ai_find_target(game, ai, eye, ai->data.target_furthest);
        const int changed = found.player ? !(ai->target.kind == AI_TARGET_PLAYER && ai->target.id == found.player->uid)
                          : found.ai ? !(ai->target.kind == AI_TARGET_AI && ai->target.id == found.ai->sid)
                                     : ai->target.kind != AI_TARGET_NONE;
        if (changed) ai_emit_sound(game, ai, ai->data.aggro_sound, 0.0f, NULL);
        if (found.player) ai_set_target_player(ai, found.player);
        else if (found.ai) ai_set_target_ai(ai, found.ai);
        else ai_clear_target(ai);
        ai->action_freq = ai->target.kind != AI_TARGET_NONE ? (ai->data.target_furthest ? 7000.0f : 2000.0f) : 5000.0f;
    }

    vec3 target_at = {0};
    float target_crouch = 0.0f;
    int target_is_player = 0;
    if (ai->target.kind != AI_TARGET_NONE && !ai_target_position(game, ai, &target_at, &target_crouch, &target_is_player)) {
        ai_clear_target(ai); // a player who left / a removed AI: JS keeps the stale object, KN drops it
    }

    AIGoal goal = {.at = ai->has_roam_target ? ai->roam_target : ai->start};
    if (ai->target.kind != AI_TARGET_NONE) {
        goal.at = target_at;
        goal.target = 1;
    } else if (ai->has_goal) { // v10.0.3 ai.moveTo: target || goal || roamTarget || startP (deob-10.0.3.js:205779)
        goal.at = ai->goal;
    } else {
        const Object *node = ai->data.path_count && ai->path_index < ai->data.path_count ? ai_path_node(game, ai->data.path[ai->path_index]) : NULL;
        if (node) {
            goal.at = node->position;
            goal.node = 1;
        } else if (ai->data.roam_radius && (ai->roam_timer -= q) <= 0.0f) {
            ai->roam_timer = ai->data.roam_time;
            const float angle = ai_rand_float(-(float) AI_PI, (float) AI_PI);
            ai->roam_target = (vec3) {ai->start.x + ai->data.roam_radius * cosf(angle), ai->start.y, ai->start.z + ai->data.roam_radius * sinf(angle)};
            ai->has_roam_target = 1;
        }
    }

    double yaw, pitch;
    ai_get_drs(goal.at, ai->position, 0.0f, &yaw, &pitch);
    if (list->force_dance_timer) {
        yaw = ai_rand_int(0, 360) * AI_PI / 180.0;
        pitch = ai_rand_int(0, 360) * AI_PI / 180.0;
    }
    float turn = ai->hit_delay_timer >= 0.0f ? 0.0f : 1.0f;
    if (ai->wait_shoot) turn = 0.01f;
    if (list->force_dance_timer) turn = 10.0f;
    ai_turn(&ai->x_dir, yaw + AI_PI / 2.0, ai->turn_speed * turn * mlt * q);
    ai_turn(&ai->y_dir, pitch, ai->turn_speed * turn * mlt * q);

    float chase = 10.0f;
    if (goal.target) chase = ai->data.can_hit ? ai->hit_range : ai->chase_distance;
    else if (goal.node) chase = 1.0f;
    const float distance = ai_d3d(ai->position, goal.at);
    const int back_off = goal.target && distance < ai->hit_range * 0.9f;
    if ((distance >= chase || back_off) && ai->speed && ai->hit_delay_timer < 0.0f && !ai->wait_shoot && !ai->break_move) {
        float step = fminf(distance, ai->speed * mlt * q * (back_off ? -0.1f : 1.0f));
        moving = 1;
        if (ai->data.phys_move) {
            step *= 0.01f;
            const float decel = ai->on_ground ? AI_GROUND_DECEL : AI_AIR_DECEL;
            ai->velocity.y -= q * AI_GRAVITY;
            if (ai->on_ground) {
                ai->velocity.x += step * sinf(ai->x_dir + (float) AI_PI / 2.0f) * cosf(ai->y_dir);
                ai->velocity.y += step * sinf(ai->y_dir);
                ai->velocity.z += step * cosf(ai->x_dir + (float) AI_PI / 2.0f) * cosf(ai->y_dir);
            }
            ai->position.x += ai->velocity.x * q;
            ai->velocity.x *= powf(decel, q);
            if (fabsf(ai->velocity.x) <= AI_DECEL_MIN) ai->velocity.x = 0.0f; // cropVal
            ai->position.y += ai->velocity.y * q;
            if (ai->velocity.y > 0.0f) ai->velocity.y -= q * 0.000032f;
            else if (ai->velocity.y <= -0.3f) ai->velocity.y = -0.3f;
            ai->position.z += ai->velocity.z * q;
            ai->velocity.z *= powf(decel, q);
            if (fabsf(ai->velocity.z) <= AI_DECEL_MIN) ai->velocity.z = 0.0f;
        } else {
            ai->position.x += step * sinf(ai->x_dir + (float) AI_PI / 2.0f) * cosf(ai->y_dir);
            ai->position.z += step * cosf(ai->x_dir + (float) AI_PI / 2.0f) * cosf(ai->y_dir);
            ai->position.y += step * sinf(ai->y_dir);
            ai->position.y -= ai->gravity * q; // v7: gravity only while moving
        }
    } else if (goal.node && ai->data.path_count > 1) {
        ai_advance_path(ai);
    }

    if (ai->break_attack > 0.0f) {
        ai->break_attack -= q;
        if (ai->break_attack <= 0.0f) ai->break_attack = 0.0f;
    }

    // melee (AI.js:738): pick a victim, wait melDel (hitDelayC), then hit everyone in range
    if (!ai->break_attack && !ai->wait_shoot && ai->data.can_hit) {
        int strike = 0;
        if (ai->hit_delay_timer < 0.0f) {
            ai->hit_timer -= q;
            if (ai->hit_timer <= 0.0f && ai->target.kind != AI_TARGET_NONE) {
                ai->hit_timer = 200.0f;
                float target_scale = 0.0f;
                if (ai->target.kind == AI_TARGET_AI) {
                    const GameAI *other = game_ai_find(game, ai->target.id);
                    target_scale = other ? other->scale : 0.0f;
                } else if (ai->target.kind == AI_TARGET_PLAYER) {
                    const Player *p = game_find_player(game, ai->target.id);
                    target_scale = p ? p->scale : 0.0f;
                }
                if (ai_d3d(ai->position, target_at) - target_scale <= ai->hit_range) {
                    ai->hit_timer = ai->hit_rate * (float) ai_rand_int(1, 1); // randInt(1, 1.2) is always 1
                    if (ai->behaviour_type == 4 && ai_rand_int(0, ai->health <= ai->max_health / 2.0f ? 1 : 3) == 0) ai->hit_timer = 400.0f;
                    ai->hit_delay_timer = ai->data.melee_delay ? ai->data.melee_delay : 400.0f;
                    ai->break_move = AI_HIT_BREAK;
                    ai_emit_sound(game, ai, ai->data.melee_sound, 0.0f, NULL);
                    game_ai_play_anim(game, ai, "Melee", 0);
                }
            }
        }
        if (ai->hit_delay_timer >= 0.0f) {
            ai->hit_delay_timer -= q;
            if (ai->hit_delay_timer <= 0.0f) {
                ai->hit_delay_timer = -1.0f;
                // meleeShake: game.rumble (client camera shake, not ported)
                ai_emit_sound(game, ai, ai->data.melee_hit_sound, 0.0f, NULL);
                strike = 1;
            }
        }
        if (strike) {
            ai_melee_hit(game, index);
            ai = &list->ais[index];
            if (!ai->active) return;
        }
    }

    // shooting (AI.js:768-816)
    int fire = 0;
    if (!ai->break_attack && ai->hit_delay_timer < 0.0f && ai->data.can_shoot && !ai->wait_shoot) {
        ai->shoot_timer -= q;
        if (ai->target.kind != AI_TARGET_NONE && ai->shoot_timer < 0.0f) {
            if (ai->data.shot_delay) ai->wait_shoot = ai->data.shot_delay;
            else fire = 1;
            ai_emit_sound(game, ai, ai->data.attack_sound, 0.0f, NULL);
            game_ai_play_anim(game, ai, "Shoot", 0);
            ai->break_move = ai->data.shoot_break;
        }
    }
    if (ai->wait_shoot) {
        ai->wait_shoot -= q;
        if (ai->wait_shoot <= 0.0f) {
            ai->wait_shoot = 0.0f;
            fire = 2;
        }
    }
    if (fire) {
        ai->shoot_timer = ai->fire_rate;
        float shot_yaw = ai->x_dir - (float) AI_PI / 2.0f, shot_pitch = 0.0f;
        ai_emit_sound(game, ai, ai->data.attack_hit_sound, 0.0f, NULL);
        // shotShake: game.rumble (client camera shake, not ported)
        if (ai->target.kind != AI_TARGET_NONE && ai_target_position(game, ai, &target_at, &target_crouch, &target_is_player)) {
            double y, p;
            // getDrs(target, ai, shoOffH ? shotOff + height / 2 : crouchDst * crouchVal): v7 rewinds a
            // player target by fetchState(target, ai), KN aims at its live position
            const float off = ai->data.shot_off_h ? ai->shot_offset + ai->height / 2.0f : AI_CROUCH_DST * target_crouch;
            ai_get_drs(target_at, ai->position, off, &y, &p);
            if (fire == 1) shot_yaw = (float) y + ai_rand_float(-ai->shot_spread, ai->shot_spread);
            shot_pitch = (float) p + ai_rand_float(-ai->shot_spread, ai->shot_spread) + ai->data.shot_arch_y * (float) AI_PI / 180.0f;
        }
        const vec3 at = {ai->position.x, ai->position.y + ai->height / 2.0f + ai->shot_offset, ai->position.z};
        ai_fire(game, ai, at, shot_yaw, shot_pitch);
        ai = &list->ais[index];
    }

    // avoidAiC: push away from every other AI in x / z
    for (int i = 0; i < list->count; i++) {
        const GameAI *other = &list->ais[i];
        if (other == ai || other->removed) continue;
        const float dx = other->position.x - ai->position.x, dz = other->position.z - ai->position.z;
        float d = sqrtf(dx * dx + dz * dz);
        if (d < ai->scale + other->scale) {
            d = ai->scale + other->scale - d;
            const float dir = atan2f(ai->position.z - other->position.z, ai->position.x - other->position.x); // getDir
            ai->position.x += d * cosf(dir);
            ai->position.z += d * sinf(dir);
        }
    }

    ai_collide_map(game, ai, q, mlt);
#ifdef SPAWNABLES_HAVE_AI_HOOKS
    spawnable_pickup_ai(game, ai); // aiOnly spawnables (Zombie Portal), WP-D
    ai = &list->ais[index];
#endif

    const int moved_before = ai->did_move;
    ai->did_move = ai_d3d(ai->position, ai->old) >= 0.1f;
    if (ai->did_move != moved_before) ai_parts_moved(game, ai); // movT0 = now (deob-10.0.3.js:206063)
    ai_stuck(ai, q, moving);
    ai_drift(ai, q, mlt);

    if (ai->last_hit_timer) {
        ai->last_hit_timer -= q;
        if (ai->last_hit_timer <= 0.0f) {
            ai->last_hit_timer = 0.0f;
            ai->last_hit_uid = -1;
        }
    }
    if (game->map && ai->position.y <= game->map->death_y) {
        Player *doer = ai->last_hit_uid >= 0 ? game_find_player(game, ai->last_hit_uid) : NULL;
        game_ai_kill(game, ai, doer);
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
        fprintf(stderr, "[ai] %2d sid %d %-16s %s hp %.0f/%.0f pos %.1f %.1f %.1f xD %.2f yD %.2f target %d:%d path %d team %d\n", i, ai->sid,
                ai->name, ai->active ? "on " : "off", ai->health, ai->max_health, ai->position.x, ai->position.y, ai->position.z, ai->x_dir,
                ai->y_dir, ai->target.kind, ai->target.id, ai->path_index, ai->team);
    }
}

void game_ai_tick(Game *game, const float delta_ms) {
    if (!game_is_authority(game) || !game->ai) return;
    GameAIs *list = game->ai;
    ai_compact(list);
    if (list->force_target_timer) {
        list->force_target_timer -= delta_ms;
        if (list->force_target_timer <= 0.0f) list->force_target_timer = 0.0f;
    }
    if (list->force_dance_timer) {
        list->force_dance_timer -= delta_ms;
        if (list->force_dance_timer <= 0.0f) list->force_dance_timer = 0.0f;
    }
    if (!list->count) return;
    ai_parts_tick(game); // v10.0.3: an AI whose solid pose waits for its model applies it now (deob-10.0.3.js:206420)
    list->updating = 1;
    for (int i = list->count - 1; i >= 0; i--) ai_update(game, i, delta_ms); // AI.update walks the list backwards
    list->updating = 0;
    ai_compact(list);
    ai_log(game, delta_ms);

    list->sync_timer += delta_ms / 1000.0f;
    if (list->sync_timer >= GAME_AI_SYNC_INTERVAL) {
        list->sync_timer = 0.0f;
        game_ai_sync(game);
    }
}

// ---------------------------------------------------------------- sync

int game_ai_packet_for_peer(const int type, const int minor) {
    const int v7 = minor >= NET_PROTOCOL_MINOR_V7;
    switch (type) {
        case NET_PACKET_AI_ADD:
        case NET_PACKET_AI_SYNC:
            return !v7;
        case NET_PACKET_AI_REMOVE:
        case NET_PACKET_AI_SYNC_V7:
        case NET_PACKET_AI_ADD_V7:
        case NET_PACKET_AI_ANIM:
        case NET_PACKET_AI_SHOT:
            return v7;
        default:
            return 1;
    }
}

static void ai_add_packet(const GameAI *ai, NetAIAddPacket *out) { // v3.1.1 AI.getData
    memset(out, 0, sizeof(*out));
    out->sid = ai->sid;
    out->index = ai->index;
    ai_copy(out->name, sizeof(out->name), ai->name);
    out->position[0] = (float) ai_round(ai->position.x, 1);
    out->position[1] = (float) ai_round(ai->position.y, 1);
    out->position[2] = (float) ai_round(ai->position.z, 1);
    out->has_data = ai->has_data;
    out->max_health = ai->max_health;
    out->m_size = ai->scale;
    out->asset = ai->data.asset;
    out->idle_sound = ai->data.idle_sound;
    ai_copy(out->src, sizeof(out->src), ai->data.src);
}

static void ai_add_packet_v7(const GameAI *ai, NetAIAddV7Packet *out) { // v7 AI.getData
    memset(out, 0, sizeof(*out));
    const GameAIData *d = &ai->data;
    out->sid = ai->sid;
    out->index = ai->index;
    ai_copy(out->name, sizeof(out->name), ai->name);
    out->position[0] = (float) ai_round(ai->position.x, 1);
    out->position[1] = (float) ai_round(ai->position.y, 1);
    out->position[2] = (float) ai_round(ai->position.z, 1);
    out->has_data = 1;
    out->has_anim = d->has_anim;
    out->anim_speed = d->anim_speed;
    out->sound_range = d->sound_range ? d->sound_range : 1.0f;
    out->team = (int16_t) ai->team;
    out->max_health = ai->max_health;
    out->m_size = ai->m_size;
    out->m_y_off = d->m_y_off;
    out->m_r_off = d->m_r_off;
    out->hbh = (float) ai_round(d->hbh ? d->hbh : 1.0f, 1);
    ai_copy(out->asset, sizeof(out->asset), d->asset_text);
    out->idle_sound = d->idle_sound;
    ai_copy(out->src, sizeof(out->src), d->src);
    out->no_vis = d->no_vis;
    out->tag_scale = d->tag_scale ? d->tag_scale : 1.0f;
    out->lvl = (int16_t) d->lvl;
    out->side = (uint8_t) d->side;
    out->opacity = d->opacity;
    out->tex_encode = (uint8_t) d->tex_encode;
    out->alpha_test = d->alpha_test;
    out->no_smooth = d->no_smooth;
    out->no_fog = d->no_fog;
    out->f_trans = d->f_trans;
    out->frames = (uint8_t) d->frames;
    out->frame_time = d->frame_time;
    out->r_anim = d->r_anim;
    out->r_anim_axis = (uint8_t) d->r_anim_axis;
    out->bob_d = d->bob_d;
    out->bob_s = d->bob_s;
    ai_copy(out->start_anim, sizeof(out->start_anim), d->start_anim);
    ai_copy(out->pj_asset, sizeof(out->pj_asset), d->pj_asset);
    out->pj_scale = d->pj_scale;
    out->pj_model_scale = d->pj_model_scale;
    out->pj_spin = d->pj_spin;
    out->pj_trans = d->pj_trans;
    out->pj_glow = d->pj_glow;
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

static void ai_sync_entry_v7(const GameAI *ai, NetAISyncV7Entry *out) {
    out->sid = ai->sid;
    out->position[0] = (float) ai_round(ai->position.x, 1);
    out->position[1] = (float) ai_round(ai->position.y, 1);
    out->position[2] = (float) ai_round(ai->position.z, 1);
    out->x_dir = (float) ai_round(ai->x_dir, 2);
    out->y_dir = (float) ai_round(ai->y_dir, 2);
    out->health = (int32_t) floorf(ai->health);
    out->did_move = ai->did_move;
    out->team = (int16_t) ai->team;
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

static void ai_send_sync_v7(Game *game, const int target, const NetAISyncV7Entry *entries, const int count) {
    NetAISyncV7Packet packet;
    int sent = 0;
    do {
        memset(&packet, 0, sizeof(packet));
        packet.first = (uint8_t) (sent == 0);
        packet.count = (uint8_t) (count - sent < NET_AI_SYNC_V7_MAX ? count - sent : NET_AI_SYNC_V7_MAX);
        memcpy(packet.entries, entries + sent, packet.count * sizeof(NetAISyncV7Entry));
        game_send(game, target, NET_PACKET_AI_SYNC_V7, &packet,
                  (uint16_t) (offsetof(NetAISyncV7Packet, entries) + packet.count * sizeof(NetAISyncV7Entry)));
        sent += packet.count;
    } while (sent < count);
}

// Both encodings go out; the server's per-peer gate (game_ai_packet_for_peer) keeps one.
static void ai_send_add(Game *game, const int target, const GameAI *ai) {
    NetAIAddPacket add;
    ai_add_packet(ai, &add);
    game_send(game, target, NET_PACKET_AI_ADD, &add, sizeof(add));
    NetAIAddV7Packet add7;
    ai_add_packet_v7(ai, &add7);
    game_send(game, target, NET_PACKET_AI_ADD_V7, &add7, sizeof(add7));
}

// AI.sync: every active player gets "aai" for AIs in range (800) it has not seen and one "ai"
// list; connections without a player (spectators, menu) get every active AI.
void game_ai_sync(Game *game) {
    GameAIs *list = game->ai;
    if (!list || !list->count || game->is_local) return; // the in-process client reads the list directly

    NetAISyncEntry entries[GAME_AI_MAX];
    NetAISyncV7Entry entries7[GAME_AI_MAX];

    // v10.0.3 AI.sync (deob-10.0.3.js:206561-206568): a partsDirty AI sends "aip" to everyone it was sent to
    for (int i = 0; i < list->count; i++) {
        GameAI *ai = &list->ais[i];
        AiParts *parts = ai->removed ? NULL : ai_parts_of(game, ai, 0);
        if (!parts || !parts->dirty) continue;
        parts->dirty = 0;
        for (int k = 0; k < ai->sent_count; k++) ai_parts_send(game, ai, ai->sent_to[k]);
        if (ai->sent_non_players) ai_parts_send(game, ai, NET_TARGET_NON_PLAYERS);
    }

    for (size_t p = 0; p < game->player_count; p++) {
        const Player *player = game->players[p];
        if (!player || !player->active) continue;
        int count = 0;
        for (int i = 0; i < list->count; i++) {
            GameAI *ai = &list->ais[i];
            // alwaysSync (AI.spawn, v10.0.3) skips the range: the AI is synced to every player
            if (!ai->active || ai->removed || (!ai->data.always_sync && ai_d3d(player->position, ai->position) > GAME_AI_SYNC_RANGE)) continue;
            if (ai_sent_to(ai, player->uid)) {
                ai_sync_entry(ai, &entries[count]);
                ai_sync_entry_v7(ai, &entries7[count]);
                count++;
            } else {
                ai_send_add(game, player->uid, ai);
                // getData's pg / hn / hc / sp (v10.0.3 :205151-205152): KN's "aai" has no room, the "aip" follows it
                if (ai_parts_has_state(game, ai)) ai_parts_send(game, ai, player->uid);
            }
        }
        ai_send_sync(game, player->uid, entries, count);
        ai_send_sync_v7(game, player->uid, entries7, count);
    }

    int count = 0;
    for (int i = 0; i < list->count; i++) {
        GameAI *ai = &list->ais[i];
        if (ai->removed) continue;
        if (list->non_players_dirty || !ai->sent_non_players) {
            ai->sent_non_players = 1;
            ai_send_add(game, NET_TARGET_NON_PLAYERS, ai);
            if (ai_parts_has_state(game, ai)) ai_parts_send(game, ai, NET_TARGET_NON_PLAYERS);
        }
        if (ai->active) {
            ai_sync_entry(ai, &entries[count]);
            ai_sync_entry_v7(ai, &entries7[count]);
            count++;
        }
    }
    list->non_players_dirty = 0;
    ai_send_sync(game, NET_TARGET_NON_PLAYERS, entries, count);
    ai_send_sync_v7(game, NET_TARGET_NON_PLAYERS, entries7, count);
}

void game_ai_sync_non_players(Game *game) {
    if (game->ai) game->ai->non_players_dirty = 1;
}

// ---------------------------------------------------------------- client mirror

static void ai_mirror_init(Game *game, const int sid, const int index, const char *name, const vec3 position, const GameAIData *data) {
    GameAI *ai = game_ai_find(game, sid);
    if (ai) { // a known sid is re-initialised (the server resends "aai" when a connection moves)
        const unsigned serial = ai->serial;
        ai_init(ai, position, name, index, data, sid, 1);
        ai->serial = serial;
    } else {
        ai = game_ai_spawn_ex(game, sid, index, name, position, data, 1);
    }
    if (ai) {
        ai->shown = 1; // a fresh mesh is visible until the next syncAll
        ai->force_pos = 1;
        ai->from = ai->position;
    }
}

// v3.1.1 AI.cSpawn(getData)
void game_ai_apply_add(Game *game, const NetAIAddPacket *packet) {
    if (!game_ais(game)) return;
    char name[GAME_AI_NAME_SIZE];
    memcpy(name, packet->name, sizeof(name));
    name[sizeof(name) - 1] = 0;
    const vec3 position = {packet->position[0], packet->position[1], packet->position[2]};

    GameAIData data;
    if (packet->has_data) {
        game_ai_data_defaults(&data); // dat: {health, mSize, asset, idl, src}
        data.health = packet->max_health;
        data.m_size = packet->m_size;
        data.asset = packet->asset;
        if (packet->asset) snprintf(data.asset_text, sizeof(data.asset_text), "%d", packet->asset);
        data.idle_sound = packet->idle_sound;
        memcpy(data.src, packet->src, sizeof(data.src));
        data.src[sizeof(data.src) - 1] = 0;
        data.glow = !data.asset;
        data.frames = data.asset ? 0 : 2;
        data.side = 2;
        data.asset = 0; // v3.1.1 servers send mSize already scaled for the mesh: no x0.5
    } else {
        ai_builtin(packet->index, &data);
    }
    ai_mirror_init(game, packet->sid, packet->index, name, position, &data);
    GameAI *ai = game_ai_find(game, packet->sid);
    if (ai && packet->has_data) {
        ai->data.asset = packet->asset;
        ai->scale = ai->m_size = ai->height = packet->m_size ? packet->m_size : 4.0f;
        ai->always_name = ai->height >= 6.0f;
    }
}

void game_ai_apply_add_v7(Game *game, const NetAIAddV7Packet *p) {
    if (!game_ais(game)) return;
    char name[GAME_AI_NAME_SIZE];
    memcpy(name, p->name, sizeof(name));
    name[sizeof(name) - 1] = 0;
    GameAIData d;
    game_ai_data_defaults(&d);
    memcpy(d.src, p->src, sizeof(d.src));
    d.src[sizeof(d.src) - 1] = 0;
    memcpy(d.asset_text, p->asset, sizeof(d.asset_text));
    d.asset_text[sizeof(d.asset_text) - 1] = 0;
    ai_asset_flags(&d);
    d.has_anim = p->has_anim;
    d.anim_speed = p->anim_speed;
    d.sound_range = p->sound_range;
    d.team = p->team;
    d.health = p->max_health;
    d.m_size = p->m_size; // the server's mSize (x2 for assets): AI.init halves it again on the client
    d.m_y_off = p->m_y_off;
    d.m_r_off = p->m_r_off;
    d.hbh = p->hbh;
    d.idle_sound = p->idle_sound;
    d.no_vis = p->no_vis;
    d.tag_scale = p->tag_scale;
    d.lvl = p->lvl;
    d.side = p->side;
    d.opacity = p->opacity;
    d.tex_encode = p->tex_encode;
    d.alpha_test = p->alpha_test;
    d.no_smooth = p->no_smooth;
    d.no_fog = p->no_fog;
    d.f_trans = p->f_trans;
    d.frames = p->frames;
    d.frame_time = p->frame_time;
    d.r_anim = p->r_anim;
    d.r_anim_axis = p->r_anim_axis;
    d.bob_d = p->bob_d;
    d.bob_s = p->bob_s;
    memcpy(d.start_anim, p->start_anim, sizeof(d.start_anim));
    d.start_anim[sizeof(d.start_anim) - 1] = 0;
    memcpy(d.pj_asset, p->pj_asset, sizeof(d.pj_asset));
    d.pj_asset[sizeof(d.pj_asset) - 1] = 0;
    d.pj_scale = p->pj_scale;
    d.pj_model_scale = p->pj_model_scale;
    d.pj_spin = p->pj_spin;
    d.pj_trans = p->pj_trans;
    d.pj_glow = p->pj_glow;
    ai_mirror_init(game, p->sid, p->index, name, (vec3) {p->position[0], p->position[1], p->position[2]}, &d);
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
        ai->interpolate = 0;
    }
}

// v7 AI.syncAll: hideAll (forcePos for AIs without fresh data), then per entry the target and the
// interpolation start (the mesh position) unless forcePos.
void game_ai_apply_sync_v7(Game *game, const NetAISyncV7Packet *packet) {
    GameAIs *list = game->ai;
    if (!list) return;
    if (packet->first) {
        for (int i = 0; i < list->count; i++) {
            GameAI *ai = &list->ais[i];
            ai->force_pos = !ai->shown; // forcePos = !latestData
            ai->shown = 0;
        }
    }
    for (int i = 0; i < packet->count && i < NET_AI_SYNC_V7_MAX; i++) {
        const NetAISyncV7Entry *e = &packet->entries[i];
        GameAI *ai = game_ai_find(game, e->sid);
        if (!ai) continue;
        const vec3 mesh = ai->interpolate ? ai->from : ai->position; // last drawn position
        vec3 drawn = mesh;
        if (ai->interpolate) { // the mesh is where the interpolation got to
            const float t = fminf(1.6f, ai->interp_t / (1000.0f * GAME_AI_SYNC_INTERVAL));
            drawn = (vec3) {ai->from.x + (ai->position.x - ai->from.x) * t, ai->from.y + (ai->position.y - ai->from.y) * t,
                            ai->from.z + (ai->position.z - ai->from.z) * t};
        }
        ai->shown = 1;
        ai->position = (vec3) {e->position[0], e->position[1] + sinf(ai->bob_val) * ai->bob_d, e->position[2]};
        ai->x_dir = e->x_dir;
        ai->y_dir = e->y_dir;
        if (!ai->force_pos) {
            ai->interp_t = 0.0f;
            ai->from = drawn;
            ai->interpolate = 1;
        } else {
            ai->interpolate = 0;
            ai->from = ai->position;
        }
        ai->force_pos = 0;
        ai->health = (float) e->health;
        ai->did_move = e->did_move;
        ai->team = e->team;
    }
}

void game_ai_apply_remove(Game *game, const NetAIRemovePacket *packet) { // removeBySid
    GameAIs *list = game->ai;
    if (!list) return;
    for (int i = list->count - 1; i >= 0; i--) {
        if (list->ais[i].sid != packet->sid) continue;
        ai_parts_drop_solid(game, &list->ais[i]); // AI.remove: solidPose = null; applySolid() (deob-10.0.3.js:206605)
        list->ais[i].active = 0;
        list->ais[i].removed = 1;
    }
    ai_compact(list);
}

void game_ai_apply_anim(Game *game, const NetAIAnimPacket *packet) { // servAnim(sid, clip, raw)
    GameAI *ai = game_ai_find(game, packet->sid);
    if (!ai) return;
    char clip[GAME_AI_ANIM_SIZE];
    memcpy(clip, packet->clip, sizeof(clip));
    clip[sizeof(clip) - 1] = 0;
    ai_copy(ai->clip, sizeof(ai->clip), clip);
    ai->clip_reps = 1; // "ana" carries no reps: v10.0.2 servAnim plays it once
    ai->clip_serial++;
}

// The interpolated draw position lives in GameAI.from + interp_t; ai_render.c reads it.
void game_ai_client_update(Game *game, const float delta_ms) {
    for (int i = 0; game->ai && i < game->ai->count; i++) {
        GameAI *ai = &game->ai->ais[i];
        if (ai->interpolate) ai->interp_t += delta_ms;
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

// buildSoulSanctumAITemplates: the first BOT per normalised aiName of the Soul Sanctum map, with
// buildAIData's values and no addAI clamps (game.ts builds the data itself). The templates carry
// no trigger: /summon spawns never fire the map's Destroy Interface actions.
const GameAITemplate *game_ai_templates(int *count) {
    if (g_template_count < 0) {
        g_template_count = 0;
        const int index = map_catalog_index_of_key("311_soul_sanctum"); // the v3.1.1 map (catalogue 311_ entry)
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
            ai_build_data(object, &template->data, NULL, 0, NULL);
            template->data.fire_rate = json_int(object, "fr") ? json_int(object, "fr") : template->data.fire_rate; // unclamped
            ai_copy(g_template_names[g_template_count], GAME_AI_NAME_SIZE, cJSON_GetStringValue(ain));
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

// buildSoulSanctumAIAliases: the words summon takes for a template besides its name (alias, template key)
static const char *const k_summon_aliases[][2] = {
    {"weep", "weepingsoul"}, {"weeping", "weepingsoul"}, {"wander", "wanderingsoul"}, {"wandering", "wanderingsoul"},
    {"fiend", "soulfiend"}, {"soulfiend", "soulfiend"}, {"angered", "angeredsoul"}, {"angeredsoul", "angeredsoul"},
    {"gate", "gatekeeper"}, {"gatekeeper", "gatekeeper"}, {"lord", "lordofsouls"}, {"lordofsouls", "lordofsouls"},
};

size_t game_ai_summon_alias_count(void) {
    return sizeof(k_summon_aliases) / sizeof(k_summon_aliases[0]);
}

const char *game_ai_summon_alias(const size_t i) {
    return i < game_ai_summon_alias_count() ? k_summon_aliases[i][0] : NULL;
}

const GameAITemplate *game_ai_resolve_template(const char *name_or_alias) {
    char key[64], other[GAME_AI_NAME_SIZE];
    ai_normalise(name_or_alias ? name_or_alias : "", key, sizeof(key));
    for (size_t i = 0; i < sizeof(k_summon_aliases) / sizeof(k_summon_aliases[0]); i++) {
        if (!strcmp(key, k_summon_aliases[i][0])) {
            ai_copy(key, sizeof(key), k_summon_aliases[i][1]);
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
    const float distance = fmaxf(48.0f, scale * 4.0f);
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
