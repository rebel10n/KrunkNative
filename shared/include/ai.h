#pragma once
// Krunker v7.1.4 map AI (src/data/AI.js; v3.1.1 src/data/ai.js before it): the map's AI_SPAWNER
// objects (MapManager.buildAIData / addAI / addPendingAIs, v3.1.1 maps' BOT is the same prefab 55),
// the built-in kinds (AI.js vA767: ghost, Drowned Sailor, Jack O' Lantern, Mirror Image, Lost Soul),
// trigger / KrunkScript / zombies spawns, /summon (server/game.ts spawnSoulSanctumAI) and the
// "aai" / "ai" / "rai" / "ana" sync. shared/src/ai.c simulates them on the authority (server,
// offline game); clients mirror them from AI_ADD / AI_SYNC (+ the v7 packets 160-163) and draw them
// (client/src/ai_render.c, nametags.c). Times are Krunker milliseconds, distances world units.
// Map AIs and map bots (BOT_SPAWNER): shared/include/map_ai.h. doc/v7/map-logic.md §2.8, WP-F.
#include <shared.h>

#define GAME_AI_HAVE_V7_API 1         // game_ai_data_defaults / remove / set_health / play_anim, path_index
#define GAME_AI_TEAMS 1               // GameAI.team (tm, forceTeam): v7 team targeting / damage rules
#define GAME_AI_MAX 64                // AIs listed at once (forced spawns may pass the limit)
#define GAME_AI_LIMIT 40              // v7 config.aiLimit (v3.1.1 20): addAI pending AIs and AI.spawn
#define GAME_AI_MAP_LIMIT GAME_AI_LIMIT
#define GAME_AI_SYNC_INTERVAL 0.2f    // v7 AI.sync: shouldUpdate(updateIndex, tickFrequency, aiSyncT 5): 5 per second
#define GAME_AI_SYNC_RANGE 800.0f     // v7 AI.sync / playAnim: within 800 of the player (v3.1.1 600; spectators: all)
#define GAME_AI_NAME_SIZE 40          // aiName (16 max on maps) / "<player name>'s Ghost"
#define GAME_AI_SRC_SIZE 24
#define GAME_AI_ASSET_SIZE 16         // "aid" with its flag letters ("46544e", "50572q")
#define GAME_AI_ANIM_SIZE 33          // startAnim (<= 32) / clip names
#define GAME_AI_PATH_MAX 32           // pathInput node ids ("phi" comma list)
#define GAME_AI_SENT_MAX 32           // AI.sentTo (player uids)
#define GAME_AI_IGNORE_MAX 8          // ignoreList (ignoreOnStuck) entries
#define GAME_AI_PROJECTILE_KINDS 5    // v3.1.1 projectiles.stored (0 = thrown knife, 1..4 = AI shots)
#define GAME_AI_PATH_TYPES 4          // config.pathTypes: Loop, Do Once, Ping Pong, Random
#define GAME_AI_MAP_BOT_MAX 16        // v7 config.botLimit: BOT_SPAWNER bots per map (addBot)
// effects.js on AIs (AI.giveEffect / tickAI): stack counts per effect index. Effects only work when
// shared.h has the v7 effects (player-viewer WP-B: EffectId, EFFECT_STACK_MAX, g_effects).
#define GAME_AI_EFFECTS 10
#define GAME_AI_EFFECT_STACKS 10

enum { AI_PATH_LOOP, AI_PATH_ONCE, AI_PATH_PING_PONG, AI_PATH_RANDOM };
enum { AI_TARGET_NONE, AI_TARGET_PLAYER, AI_TARGET_AI, AI_TARGET_POINT }; // target: a player, an AI, AI.forceTarget

// The AI init data (AI.js init `dat`): a map AI_SPAWNER's buildAIData + addAI clamps, a built-in
// kind (vA767) or a script / zombies template. game_ai_data_defaults gives AI.init's defaults for
// absent keys; map data takes buildAIData's (speed, turn speed and gravity 1, melee range 10 ...).
typedef struct {
    // ---- identity / render (getData)
    char src[GAME_AI_SRC_SIZE];   // "ghost_0", "model" (asset)
    int asset;                    // aid digits (assetID), 0 none
    char asset_text[GAME_AI_ASSET_SIZE]; // aid as given ("46544e"): letters g/q/f (anim mesh glTF/GLB/FBX),
                                  // e emissive, n normal, s specular, a alphaTest, m metal, r rough, p displ., b bump
    unsigned char has_anim:1;     // hasAnim: g / q / f
    unsigned char no_frames:1;    // v3.1.1 built-in Jack O' Lantern
    unsigned char glow:1;         // emissive #ffffff + "<src>_e" emissiveMap: v3.1.1 servers' ghosts only (v7 clients never glow ghost_0)
    unsigned char no_vis:1;       // noVis ("v"): no mesh
    unsigned char no_fog:1;       // nf
    unsigned char f_trans:1;      // ft
    unsigned char no_smooth:1;    // txs: nearest filtering
    unsigned char rot_y:1;        // rotY (built-in Jack O' Lantern; unused by v7 render)
    float anim_speed;             // animS ("as")
    float sound_range;            // soundR ("sor"), 0 = 1
    char start_anim[GAME_AI_ANIM_SIZE]; // startAnim ("ast"), "" = "Idle"
    float bob_d, bob_s;           // bbD / bbS (map: bbS / 1000)
    int frames;                   // fct (0..100; map AIs without aid: 2)
    float frame_time;             // fs * 100 ms (map AIs without aid: 500)
    float r_anim;                 // spin * config.diskSpin (rAnim), 0 none
    int r_anim_axis;              // spinAxis: 0 y, 1 x
    int side;                     // rs 0..2 (map AIs without aid: 2 double sided)
    float opacity;                // o (1)
    float alpha_test;             // alt 0..1
    int tex_encode;               // ten
    float m_size;                 // ms (0.01..100), 0 = 4 (mSize || 4)
    float m_y_off, m_r_off;       // myo (-100..100) / mro (degrees)
    float hbh, hbw;               // hit box height / width multipliers (0.1..10), 0 = 1
    float tag_scale;              // ains (0..10), 0 = 1
    int lvl;                      // ail (0..999): the nametag level
    // ---- stats
    float health;
    float respawn_t;              // ms
    unsigned char respawn_r:1;    // rr: random respawn time in [100, respawnT || 5000]
    unsigned char can_shoot:1;    // cs
    unsigned char can_hit:1;      // cnh (melee)
    unsigned char xray:1;         // xv: targets through walls (xrayVis)
    unsigned char targets_ai:1;   // tAi (targAI)
    unsigned char targets_players:1; // tPl == null (targPlr)
    unsigned char target_furthest:1; // furthTarg
    unsigned char friendly:1;     // aif
    unsigned char phys_move:1;    // pmv (pMov): velocity / gravity / decel like a player
    unsigned char ignore_on_stuck:1;    // igos
    unsigned char drift_on_stuck:1;     // dos
    unsigned char invincible_on_stuck:1;// ivos
    unsigned char shot_off_h:1;   // shof == null: aim from the shot height (shoOffH)
    unsigned char explode:1;      // exp: shots explode (ex 40, expS 60)
    unsigned char can_destroy:1;  // cdy
    unsigned char start_destroyed:1; // sd (cdy only): not spawned at round start (rsc: spawn chance)
    unsigned char pj_gravity_set:1;  // pjG given (buildAIData: always for map data)
    unsigned char pj_glow:1;      // prgl (pjGL)
    unsigned char pj_trans:1;     // prgt (pjT)
    float speed;                  // spd (0..10), * 0.031 per ms
    float turn_speed;             // tsd (0..30), * 0.002 rad per ms
    float gravity;                // grv (0..1), * 0.1 per ms
    int fire_rate;                // fr (200..10000) ms, 0 = 2000
    float shot_spread;            // aac (aicur 0..10) rad
    float shoot_break;            // shb (0..5000) ms
    float shot_arch_y;            // sha (-90..90) degrees
    float shot_delay;             // shdy (0..5000) ms
    float shot_shake;             // ss (0..1)
    int effect_on_shot;           // se (0..9): projOb.effectInd, -1 none
    int effect_on_hit;            // he (0..9): melee giveEffect, -1 none
    float crit_dmg, crit_hei;     // crd (0..5) / crh (0..10)
    float hit_rate;               // hr (200..10000) ms, 0 = 800
    float melee_range;            // mlr (10..100)
    float melee_radius;           // mlra (0..2 PI) rad, 0 any direction
    float melee_damage;           // mld (1..200), 0 = 10
    float melee_delay;            // mldy (0..5000) ms, 0 = 400
    float melee_shake;            // mls (0..1)
    float chase_distance;         // aic (0..2000), 0 = 10
    int vision;                   // vd (1..1000), 0 = 120
    int score;                    // sp: killScore
    int projectile;               // v3.1.1 pj (0..5); v7 shoots projectiles.stored[1] with projOb
    int behaviour;                // bh (0..20): 1 Weeping, 2 Lord of Souls, 3 enrage, 4 mirror image, 5 Drowned Sailor
    int team;                     // tm
    int path_type;                // pht: AI_PATH_*
    int path[GAME_AI_PATH_MAX];   // phi: PATH_NODE nids
    int path_count;
    float roam_radius;            // rmr (0..500)
    float roam_time;              // rmt (1000..10000) ms
    float spawn_chance;           // rsc (0..100), 0 none
    // projOb (AI.js init): projectiles.stored[1] scaled per AI
    char pj_asset[GAME_AI_ASSET_SIZE]; // pra: projectile model asset ("" = "orb_0")
    float pj_damage;              // pjd (0..1000): dmg = ceil(1 * d) (0 = 1)
    float pj_speed;               // pjs: spd * pjSp
    float pj_gravity;             // prg * 0.0002 (-2..2)
    float pj_scale;               // prsc (0.1..10): hitBox 4 * pjSC, render scale
    float pj_model_scale;         // prms (0.01..10)
    float pj_spin;                // prs (0..20)
    // ---- sounds (user-asset ids, 0 none)
    int idle_sound, death_sound, attack_sound; // idl / dth / atk
    int attack_hit_sound, melee_sound, melee_hit_sound, aggro_sound; // atkh / mel / melh / agr
    // ---- triggers (ct + tas; only the onAI* events: 15 onAIDeath, 20 onAIDamage, 29 onAIHalfHealth,
    // 31 onAIKillPlayer). trig.event holds the v7 event index, -1 none; trig.actions point into the
    // owning GameAIs' data records (never freed per AI).
    int interface_id;             // in (0 none): trigger action 36 spawns pending AIs by interface
    TriggerData trig;
} GameAIData;

typedef struct {
    unsigned char kind;           // AI_TARGET_*
    int id;                       // player uid / AI sid
    unsigned serial;              // AI target: its GameAI.serial (a re-used sid is someone else)
    vec3 point;                   // AI_TARGET_POINT
} GameAITarget;

typedef struct GameAI_t {
    int sid;                      // generateSID(ais): the lowest free id from 1
    int index;                    // built-in kind (AI.js vA767[index]), 0 with data
    char name[GAME_AI_NAME_SIZE];
    unsigned char has_data:1;     // spawned with data (getData sends it)
    unsigned char active:1;
    unsigned char used_special:1;
    unsigned char always_name:1;  // height >= 6: the nametag shows at full health too
    unsigned char shown:1;        // client: mesh.visible (AI.syncAll) / authority: in range of the local player
    unsigned char can_be_seen:1;  // client: Game.canSee(player, ai) == null (gameLoop nameVisRate pass)
    unsigned char sent_non_players:1; // "aai" went to the connections without a player
    unsigned char did_move:1;     // didMov: moved >= 0.1 this update (sync, Move / Idle clip)
    unsigned char on_ground:1;
    unsigned char invincible:1;   // invincibleOnStuck
    unsigned char perm_invincible:1;
    unsigned char was_stuck:1;
    unsigned char removed:1;      // removeFully ran: the slot is dropped after this update
    unsigned serial;              // spawn counter value: tells a re-used sid apart (client meshes)
    GameAIData data;

    vec3 position, start, old;
    float x_dir, y_dir;           // xD (yaw) / yD (pitch)
    vec3 velocity;                // xVel / yVel / zVel (pMov, redirection, drift)
    float max_health, health;
    float speed, turn_speed, gravity;
    int kill_score;
    int team, team_saved;         // team / _team (forceTeam)
    float force_team_timer;
    float respawn_t;
    float respawn_timer;          // rspTimer
    float break_attack, break_move, break_timer;
    float fire_rate, shoot_timer, shot_spread, shot_offset, wait_shoot;
    float special_timer, special_timer2, spawn_minion_timer;
    int spawn_cap;
    float hit_range;              // v3.1.1 name: the melee chase range (meleeRange * 0.7)
    float hit_damage, hit_timer, hit_rate, hit_delay_timer; // hitDMG / hitTimer / hitRate / hitDelayC (< 0 none)
    float bob_d, bob_s, bob_val;  // bobVal: client mesh y += sin(bobVal) * bobD
    float scale;                  // the hit box half width: mSize * hbw
    float height;                 // the hit box half height: mSize * hbh (box y .. y + 2 * height)
    float m_size;                 // mSize (x2 for an asset model on the server, x0.5 on the client)
    int behaviour_type;           // behavType
    float action_freq;
    GameAITarget target;
    int target_uid;               // the player in `target`, -1 none (v3.1.1 startP / chat / tests)
    int old_target_uid;           // oldTargetId
    vec3 roam_target;
    unsigned char has_roam_target;
    float roam_timer;
    int path_index, path_dir;     // pathIndex / pathIndDir
    float stuck_time;
    int stuck_count;
    int force_direction;          // driftOnStuck: 1 / -1, 0 none
    float force_direction_timer;
    int ignore_uid[GAME_AI_IGNORE_MAX]; // ignoreList: player uid -> ms
    float ignore_timer[GAME_AI_IGNORE_MAX];
    int last_hit_uid;             // lastHit.doer (player uid, -1 none) for deathY kills
    float last_hit_timer;
    float vision;
    float chase_distance;
    int frames, frame, last_frame;// v3.1.1 tFrame (texture strip frame)
    float frame_timer;            // frameRT
    float idle_timer;             // client: idlSoundT (ms), idlSoundI 5000 * randFloat(0.8, 1.2)
    unsigned char effects[GAME_AI_EFFECTS];   // giveEffect stacks
    float effect_timers[GAME_AI_EFFECTS][GAME_AI_EFFECT_STACKS]; // attributeTimers (ms)
    unsigned char effect_timer_count[GAME_AI_EFFECTS];
    float zap;                    // attributes.zap: damage every second tick (1000 ms)
    float effect_ticker;          // ticker (500 ms)
    int effect_tick_count;        // tickerC
    int zapped_by;                // zappedBy (player uid, -1 none)
    int sent_to[GAME_AI_SENT_MAX];// AI.sentTo (player uids)
    int sent_count;
    // client mirror
    vec3 from;                    // interpolation start (mesh position at the last "ai")
    float interp_t;               // ms since the last "ai"
    unsigned char interpolate:1;
    unsigned char force_pos:1;
    char clip[GAME_AI_ANIM_SIZE]; // the last "ana" clip to play once ("" none)
    unsigned clip_serial;         // bumped per "ana"
} GameAI;

// An owned data record: a pending map AI (addAI) or a parsed key bag; trigger actions live here.
typedef struct {
    GameAIData data;
    char name[GAME_AI_NAME_SIZE]; // aiName
    vec3 position;
    unsigned char pending:1;      // MapManager.pendingAIs (action 36 spawns from these)
    TriggerActionData *actions;   // owned (data.trig.actions)
} GameAIRecord;

// An AI's shot in flight: who fired it (projectiles.js m.owner = the AI) and its projOb extras.
typedef struct {
    int sid;                      // the shooter (0 none)
    unsigned serial;              // its GameAI.serial
    int team;                     // the shooter's team at the shot
    int effect;                   // projOb.effectInd (se), -1 none
    unsigned char no_objects:1;   // projOb.noObjC (Drowned Sailor bh 5)
    unsigned char used:1;
} GameAIShot;

typedef struct GameAIs_t {
    GameAI ais[GAME_AI_MAX];
    int count;                    // used slots (Krunker AI.ais.length: dead AIs stay listed while they respawn)
    float sync_timer;
    unsigned serial;              // last GameAI.serial handed out
    unsigned char non_players_dirty:1; // send every AI's "aai" to spectators / menu clients next sync
    unsigned char updating:1;     // inside AI.update: removals are deferred
    float force_target_timer;     // AI.forceTargetTimer (trigger action 70)
    vec3 force_target;
    float force_dance_timer;      // AI.forceDanceTimer (spawnable "Dance")
    GameAIRecord *records;        // owned data records (pending map AIs + key bags), freed at reset
    int record_count, record_capacity;
    int map_bot_ids[GAME_AI_MAP_BOT_MAX]; // GameBot.id of the BOT_SPAWNER bots of this round
    int map_bot_count;
    GameAIShot shots[GAME_MAX_PROJECTILES]; // the v7 AI shot in each projectile slot (ai_kind 1, no owner)
} GameAIs;

// ---- packets
// "aai" v3.1.1 (AI.getData): [sid, index, name, x, y, z, dat ? {health, mSize, asset, idl, src} : 0]
#define NET_AI_SYNC_MAX 14            // "ai" entries per AI_SYNC frame (NET_MAX_PAYLOAD)
#pragma pack(push, 1)
typedef struct {
    int32_t sid;
    int32_t index;
    char name[GAME_AI_NAME_SIZE];
    float position[3];
    uint8_t has_data;
    float max_health;
    float m_size;
    int32_t asset;
    int32_t idle_sound;
    char src[GAME_AI_SRC_SIZE];
} NetAIAddPacket;

// "ai" v3.1.1: [sid, x, y, z, xD, yD, floor(health), tFrame] per AI in range. A frame with `first`
// set starts a new sync (syncAll: hideAll first); "ai" 0 is a first frame with no entries.
typedef struct {
    int32_t sid;
    float position[3];
    float x_dir, y_dir;
    int32_t health;
    uint8_t frame;
} NetAISyncEntry;

typedef struct {
    uint8_t first;
    uint8_t count;
    NetAISyncEntry entries[NET_AI_SYNC_MAX];
} NetAISyncPacket;
#pragma pack(pop)
_Static_assert(sizeof(NetAIAddPacket) <= NET_MAX_PAYLOAD, "NetAIAddPacket exceeds NET_MAX_PAYLOAD");
_Static_assert(sizeof(NetAISyncPacket) <= NET_MAX_PAYLOAD, "NetAISyncPacket exceeds NET_MAX_PAYLOAD");

// ---- WP-F: v7.1.4 AI packets (block NET_PACKET_BLOCK_AI 160-169), sent only to peers whose HELLO
// announced NET_PROTOCOL_MINOR_V7; older peers keep getting AI_ADD / AI_SYNC (90 / 91).
enum {
    NET_PACKET_AI_REMOVE = NET_PACKET_BLOCK_AI + 0,  // S->C: NetAIRemovePacket ("rai" sid: removeFully)
    NET_PACKET_AI_SYNC_V7 = NET_PACKET_BLOCK_AI + 1, // S->C: NetAISyncV7Packet ("ai", stride 9)
    NET_PACKET_AI_ADD_V7 = NET_PACKET_BLOCK_AI + 2,  // S->C: NetAIAddV7Packet ("aai" getData v7)
    NET_PACKET_AI_ANIM = NET_PACKET_BLOCK_AI + 3,    // S->C: NetAIAnimPacket ("ana" sid, clip, raw, 0)
    NET_PACKET_AI_SHOT = NET_PACKET_BLOCK_AI + 4,    // S->C: NetAIShotPacket (projectile slot -> shooter: projOb visuals)
};
#define NET_AI_SYNC_V7_MAX 16

#pragma pack(push, 1)
typedef struct {
    int32_t sid;
} NetAIRemovePacket;

// "ai" v7: [sid, x, y, z, xD, yD, floor(health), didMov, team] per AI in range (800).
typedef struct {
    int32_t sid;
    float position[3];
    float x_dir, y_dir;
    int32_t health;
    uint8_t did_move;
    int16_t team;
} NetAISyncV7Entry;

typedef struct {
    uint8_t first;
    uint8_t count;
    NetAISyncV7Entry entries[NET_AI_SYNC_V7_MAX];
} NetAISyncV7Packet;

// "aai" v7 (AI.getData, AI.js:386-417): [sid, index, name, x, y, z, dat ? {...} : 0]
typedef struct {
    int32_t sid;
    int32_t index;
    char name[GAME_AI_NAME_SIZE];
    float position[3];
    uint8_t has_data;
    uint8_t has_anim, no_vis, no_fog, f_trans, no_smooth;
    int16_t team;
    float anim_speed, sound_range;
    float max_health, m_size, m_y_off, m_r_off, hbh;
    char asset[GAME_AI_ASSET_SIZE];
    int32_t idle_sound;
    char src[GAME_AI_SRC_SIZE];
    float tag_scale;
    int16_t lvl;
    uint8_t side;
    float opacity, alpha_test;
    uint8_t tex_encode;
    uint8_t frames;
    float frame_time;
    float r_anim;
    uint8_t r_anim_axis;
    float bob_d, bob_s;
    char start_anim[GAME_AI_ANIM_SIZE];
    // projOb visuals (particles.js: model asset, scale pjSC / pjMS, spin pjS, pjT, pjGL)
    char pj_asset[GAME_AI_ASSET_SIZE];
    float pj_scale, pj_model_scale, pj_spin;
    uint8_t pj_trans, pj_glow;
} NetAIAddV7Packet;

// "ana" sid, clip, raw, isObj = 0: raw 1 = a trigger / script clip name, 0 = config.clipNames index.
typedef struct {
    int32_t sid;
    uint8_t raw;
    char clip[GAME_AI_ANIM_SIZE];
} NetAIAnimPacket;

typedef struct {
    int32_t projectile;           // GameProjectile.id of the NET_PACKET_PROJECTILE just sent
    int32_t sid;                  // the AI that fired it
} NetAIShotPacket;
#pragma pack(pop)
_Static_assert(sizeof(NetAISyncV7Packet) <= NET_MAX_PAYLOAD, "NetAISyncV7Packet exceeds NET_MAX_PAYLOAD");
_Static_assert(sizeof(NetAIAddV7Packet) <= NET_MAX_PAYLOAD, "NetAIAddV7Packet exceeds NET_MAX_PAYLOAD");

// Server per-peer gate (server/src/net.c): 0 when `type` must not go to a peer that announced
// protocol `minor` (v7 AI packets to old peers, the v3.1.1 AI_ADD / AI_SYNC to v7 peers).
int game_ai_packet_for_peer(int type, int minor);

// The spawnable "BOT" templates of the Soul Sanctum map (game.ts SOUL_SANCTUM_AI_TEMPLATES).
typedef struct {
    const char *name;             // aiName
    GameAIData data;
} GameAITemplate;

GameAIs *game_ais(Game*);                        // lazily allocated list (NULL on OOM)
void game_ai_reset(Game*);                       // AI.reset (game.init): drops AIs and data records
void game_ai_free(Game*);                        // game_ai_reset + free the list
void game_ai_spawn_map(Game*);                   // = map_ai_spawn_pending (shared/include/map_ai.h)
void game_ai_data_defaults(GameAIData*);         // AI.init's defaults for an empty `dat`
void game_ai_builtin_data(int index, GameAIData*); // the built-in kind's record (AI.js vA767[index])
// AI.spawn(sid 0 = generateSID, index, name, x, y, z, data NULL = built-in kind): NULL past
// config.aiLimit (40 listed AIs) unless `force` (cSpawn / zombies addAI(..., true)).
GameAI *game_ai_spawn(Game*, int sid, int index, const char *name, vec3 position, const GameAIData *data);
GameAI *game_ai_spawn_ex(Game*, int sid, int index, const char *name, vec3 position, const GameAIData *data, int force);
GameAI *game_ai_find(Game*, int sid);
int game_ai_active_count(const Game*, int team);  // game.activeAIs(team): 0 = any team
void game_ai_tick(Game*, float delta_ms);        // AI.update + the AI.sync cadence (authority)
void game_ai_sync(Game*);                        // AI.sync: "aai" for new viewers, "ai" positions
void game_ai_sync_non_players(Game*);            // a spectator joined: every AI's "aai" to non-players next sync
// AI.changeHealth(amount, source, forceTarget, set): negative = damage. `source` a player or NULL;
// game_ai_change_health_by_ai for AI sources. Returns 1 when it died now.
int game_ai_change_health(Game*, GameAI*, float amount, Player *source, int force_target, int set);
int game_ai_change_health_by_ai(Game*, GameAI*, float amount, GameAI *source);
void game_ai_set_health(Game*, GameAI*, float value);   // KrunkScript health =: changeHealth(v, null, false, true)
void game_ai_kill(Game*, GameAI*, Player *source);      // AI.kill (onAIDeath, mode.onAIDeath, respawn or removal)
void game_ai_remove(Game*, GameAI*);                    // AI.removeFully: "rai" to every viewer, dropped
void game_ai_force_team(Game*, GameAI*, int team, float ms, float chance); // AI.forceTeam
void game_ai_give_effect(Game*, GameAI*, int effect, int count);           // AI.giveEffect
void game_ai_play_anim(Game*, GameAI*, const char *clip, int raw);         // AI.playAnim (server: "ana")
// Hits on an AI by a player's bullet / melee / projectile / explosion (players.js shoot,
// projectiles.js, game.explosion): invincible, changeHealth, pointsPerHit, killScore, "4".
void game_ai_damage(Game*, GameAI*, Player *dealer, float damage, int force_target);
// The same with v7's extras: hit_y (crit band, NAN none), effectInd / effectC (-1 none).
void game_ai_damage_ex(Game*, GameAI*, Player *dealer, float damage, int force_target, float hit_y, int effect, int effect_count);
// Ray / box test against an AI box (x/z +-scale, y .. y + 2 * height), lineInRect semantics.
float game_ai_line_hit(const GameAI*, vec3 origin, vec3 direction);
// Game.canSee(player, x, y, z, 0, 0, true) == null with the hitscan object filter.
int game_ai_player_can_see(const Game*, const Player*, vec3 target);
// Game.canSee(from, x, y, z, pad) == null for a bare point `from` (no height: the ray starts
// cameraHeight below it, along from -> target, as canSee does with a camera position), boxes shrunk
// by `pad` (Krunker Strike's aim assist: the camera, pad .4 within 20 units, else .5).
int game_can_see_point(const Game*, vec3 from, vec3 target, float pad);
// AI.js shot: projectiles.init(x, y, z, yaw, pitch, stored[1], ai, {}, true, projOb).
void game_ai_projectile_config_for(const GameAI*, ProjectileConfig *out);
// Fires an AI's v7 shot (a "pr" with ai_kind 1 + NET_PACKET_AI_SHOT for its projOb visuals).
void game_ai_fire(Game*, GameAI*, vec3 origin, float yaw, float pitch);
// projectile.c hooks for the v7 AI shots (GameAIs.shots, keyed by the projectile slot):
int game_ai_projectile_skips_objects(const Game*, const GameProjectile*); // projOb.noObjC (bh 5)
// projectiles.js AI loop: active, not the shooter, (!ai.team || !owner || ai.team != owner.team).
int game_ai_projectile_can_hit(Game*, const GameProjectile*, const GameAI *target);
// u[0].ai of a shot: player owners -> game_ai_damage_ex; AI shooters -> changeHealth(-dmg, shooter).
void game_ai_projectile_hit_ai(Game*, const GameProjectile*, GameAI *target, Player *owner, float damage);
// u[0].plr of an AI shot: effectInd (se) on the player.
void game_ai_projectile_hit_player(Game*, const GameProjectile*, Player *target);

// Soul Sanctum templates for /summon.
const GameAITemplate *game_ai_templates(int *count);
const GameAITemplate *game_ai_resolve_template(const char *name_or_alias); // normalised name or game.ts alias
size_t game_ai_summon_alias_count(void);     // the aliases game_ai_resolve_template takes (the console completes them)
const char *game_ai_summon_alias(size_t i);  // alias i, NULL past the end
// game.ts spawnSoulSanctumAI: at the player + (sin yaw, cos yaw) * max(48, mSize * 4), y + max(2, mSize / 2).
GameAI *game_ai_summon(Game*, const GameAITemplate*, const Player *origin);

// "pr" for an AI shot: v3.1.1 projectiles.init(..., stored[kind], null, {}, true, "orb_0").
void game_spawn_ai_projectile(Game*, int stored_kind, vec3 origin, float yaw, float pitch);
const ProjectileConfig *game_ai_projectile_config(int stored_kind); // NULL out of range

// Client mirror (non-authority games): AI.cSpawn / AI.syncAll / removeBySid / servAnim.
void game_ai_apply_add(Game*, const NetAIAddPacket*);
void game_ai_apply_sync(Game*, const NetAISyncPacket*);
void game_ai_apply_add_v7(Game*, const NetAIAddV7Packet*);
void game_ai_apply_sync_v7(Game*, const NetAISyncV7Packet*);
void game_ai_apply_remove(Game*, const NetAIRemovePacket*);
void game_ai_apply_anim(Game*, const NetAIAnimPacket*);
void game_ai_client_update(Game*, float delta_ms); // AI.update RENDER branch: interpolation over 1000 / aiSyncT
