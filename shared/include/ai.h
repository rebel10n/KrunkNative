#pragma once
// Krunker v3.1.1 AI (src/data/ai.js): the map's BOT objects (map.js / objects.js addAI,
// addPendingAIs), the three built-in kinds (Traitor ghosts, Lost Souls, Jack O' Lanterns),
// /summon (server/game.ts spawnSoulSanctumAI) and the "aai" / "ai" sync (AI.sync /
// syncAll / cSpawn). shared/src/ai.c simulates them on the authority (server, offline
// game); clients mirror them from AI_ADD / AI_SYNC and draw them (client/src/ai_render.c,
// nametags.c). Times are Krunker milliseconds, distances world units.
#include <shared.h>

#define GAME_AI_MAX 64                // AIs alive at once (map BOTs + summons + boss minions)
#define GAME_AI_MAP_LIMIT 20          // config.aiLimit: objects.js addAI keeps 20 pending AIs
#define GAME_AI_SYNC_INTERVAL (5.0f / 30.0f) // gameserver.ts AI.sync every AI_SYNC_TICKS (5) server ticks (30 Hz)
#define GAME_AI_SYNC_RANGE 600.0f     // AI.sync: active AIs within 600 of the player (spectators: all)
#define GAME_AI_NAME_SIZE 40          // aiName (16 max on maps) / "<player name>'s Ghost"
#define GAME_AI_SRC_SIZE 24
#define GAME_AI_SENT_MAX 32           // AI.sentTo: players that got this AI's "aai"
#define GAME_AI_PROJECTILE_KINDS 5    // projectiles.stored (0 = thrown knife, 1..4 = AI shots)

// The AI init data (ai.js `z`): a map BOT's addAI options (objects.js limits applied) or a
// built-in kind (ai.js `c`). Only the fields ai.js and the client read.
typedef struct {
    char src[GAME_AI_SRC_SIZE];   // model: "ghost_0", "pumpkin_0" ("model" + asset for map assets)
    int asset;                    // aid (user asset id, 0 none): no frames, no ghost glow
    float health;
    float respawn_t;              // ms
    unsigned char respawn_r:1;    // rr: random respawn time in [100, respawnT || 5000]
    unsigned char can_shoot:1;    // cs
    unsigned char can_hit:1;      // cnh
    unsigned char xray:1;         // xv: targets players through walls
    unsigned char no_frames:1;    // noFram (built-in Jack O' Lantern)
    unsigned char glow:1;         // render: emissive 0xffffff + "<src>_e" emissiveMap
    float speed;                  // spd (0..5), * 0.031 per ms
    float turn_speed;             // tsd (0..30), * 0.002 rad per ms
    float gravity;                // grv (0..1), * 0.1 per ms
    int fire_rate;                // fr (200..10000) ms
    int vision;                   // vd (1..1000)
    int score;                    // sp: killScore
    int projectile;               // pj (0..5): projectiles.stored[pj + 1]
    int behaviour;                // bh (0..20): 1 = Weeping Soul boss, 2 = Lord of Souls boss
    float m_size;                 // ms (0.1..100), 0 = default 4
    float bob_d, bob_s;           // 0 = ai.js defaults 0.2 / 0.004
    int frames;                   // render frames of the built-in mesh conf (0 none)
    int idle_sound, death_sound, attack_sound; // idl / dth / atk: user-asset sound ids ("asset:<id>")
    TriggerData trig;             // Can Trigger (ct): onAIDeath -> game.onTrigger(source, ai) (shared/src/triggers.c)
} GameAIData;

typedef struct GameAI_t {
    int sid;                      // generateSID(ais): the lowest free id from 1
    int index;                    // built-in kind (ai.js c[index]), 0 with map data
    char name[GAME_AI_NAME_SIZE];
    unsigned char has_data:1;     // spawned with map data (getData sends it)
    unsigned char active:1;
    unsigned char used_special:1;
    unsigned char always_name:1;  // scale >= 6: the nametag shows at full health too
    unsigned char shown:1;        // client: mesh.visible (AI.syncAll) / authority: in range of the local player
    unsigned char can_be_seen:1;  // client: Game.canSee(player, ai) == null (gameLoop nameVisRate pass)
    unsigned char sent_non_players:1; // "aai" went to the connections without a player
    unsigned serial;              // spawn counter value: tells a re-used sid apart (client meshes)
    GameAIData data;

    vec3 position, start, old;
    float x_dir, y_dir;           // xD (yaw) / yD (pitch)
    float max_health, health;
    float speed, turn_speed, gravity;
    int kill_score;
    float respawn_t;
    float respawn_timer;          // rspTimer
    float break_attack, break_move, break_timer;
    float fire_rate, shoot_timer, shot_spread, shot_offset;
    float special_timer, special_timer2, spawn_minion_timer;
    int spawn_cap;
    float hit_range, hit_damage, hit_timer, hit_rate;
    float bob_d, bob_s, bob_val;   // bobVal: client mesh y += sin(bobVal) * bobD
    float scale;
    int behaviour_type;           // behavType
    float action_freq;
    int target_uid;               // the player it follows / shoots, -1 = none (startP)
    float vision;
    int frames, frame, last_frame;// frames / tFrame / lastFrame
    float frame_timer;            // frameRT
    float idle_timer;             // client: idlSoundT (ms), idlSoundI 5000 * randFloat(0.8, 1.2)
    int sent_to[GAME_AI_SENT_MAX];// AI.sentTo (player uids)
    int sent_count;
} GameAI;

typedef struct GameAIs_t {
    GameAI ais[GAME_AI_MAX];
    int count;                    // used slots (Krunker AI.ais.length: dead AIs stay listed)
    float sync_timer;
    unsigned serial;              // last GameAI.serial handed out
    unsigned char non_players_dirty:1; // send every AI's "aai" to spectators / menu clients next sync
} GameAIs;

// "aai" (AI.getData): [sid, index, name, x, y, z, dat ? {health, mSize, asset, idl, src} : 0]
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

// "ai": [sid, x, y, z, xD, yD, floor(health), tFrame] per AI in range. A frame with `first`
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

// The spawnable "BOT" templates of the Soul Sanctum map (game.ts SOUL_SANCTUM_AI_TEMPLATES).
typedef struct {
    const char *name;             // aiName
    GameAIData data;
} GameAITemplate;

GameAIs *game_ais(Game*);                        // lazily allocated list (NULL on OOM)
void game_ai_reset(Game*);                       // AI.reset (game.init)
void game_ai_spawn_map(Game*);                   // addPendingAIs: the map's BOT objects (authority)
// AI.spawn(sid 0 = generateSID, index, name, x, y, z, data NULL = built-in kind). Returns the AI.
GameAI *game_ai_spawn(Game*, int sid, int index, const char *name, vec3 position, const GameAIData *data);
GameAI *game_ai_find(Game*, int sid);
void game_ai_tick(Game*, float delta_ms);        // AI.update + the AI.sync cadence (authority)
void game_ai_sync(Game*);                        // AI.sync: "aai" for new viewers, "ai" positions
void game_ai_sync_non_players(Game*);            // a spectator joined: every AI's "aai" to non-players next sync
// AI.changeHealth(amount, source, forceTarget): negative = damage. Returns 1 when it died now.
int game_ai_change_health(Game*, GameAI*, float amount, Player *source, int force_target);
void game_ai_kill(Game*, GameAI*, Player *source);
// Hits on an AI by a player's bullet / melee / projectile / explosion (players.js shoot,
// projectile.js, game.explosion): changeHealth, killScore, "4" to the dealer.
void game_ai_damage(Game*, GameAI*, Player *dealer, float damage, int force_target);
// Ray / box test against an AI box (x/z +-scale, y .. y + 2 * scale), lineInRect semantics.
float game_ai_line_hit(const GameAI*, vec3 origin, vec3 direction);
// Game.canSee(player, x, y, z, 0, 0, true) == null with the hitscan object filter.
int game_ai_player_can_see(const Game*, const Player*, vec3 target);

// Soul Sanctum templates for /summon.
const GameAITemplate *game_ai_templates(int *count);
const GameAITemplate *game_ai_resolve_template(const char *name_or_alias); // normalised name or game.ts alias
// game.ts spawnSoulSanctumAI: at the player + (sin yaw, cos yaw) * max(48, mSize * 4), y + max(2, mSize / 2).
GameAI *game_ai_summon(Game*, const GameAITemplate*, const Player *origin);

// "pr" for an AI shot: projectiles.init(x, y, z, yaw, pitch, stored[kind], null, {}, true, "orb_0").
void game_spawn_ai_projectile(Game*, int stored_kind, vec3 origin, float yaw, float pitch);
const ProjectileConfig *game_ai_projectile_config(int stored_kind); // NULL out of range

// Client mirror (non-authority games): AI.cSpawn / AI.syncAll.
void game_ai_apply_add(Game*, const NetAIAddPacket*);
void game_ai_apply_sync(Game*, const NetAISyncPacket*);
