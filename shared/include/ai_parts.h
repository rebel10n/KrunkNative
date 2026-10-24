#pragma once
// Krunker v10.0.3 AI parts (KrunkScript AI.spawn preciseHits, ai.solid, ai.playAnim hold, ai.bodyDamage /
// setPartDamage / glowPart / setNodeVisible / getPartPosition, onAIPartDamage), shared/src/ai_parts.c.
// Line numbers are deob-10.0.3.js (~/jails/kru/src/Krunker_v10.0.3/deob/game.deob.js):
//   :204037-204465  the GLB reader the server loads an AI model with (iîìïiìî): the rig (nodes with name,
//                   parent, rest TRS, the mesh's bounding-box centre; the animation clips; byName) and the
//                   hit mesh (iíìíííï :204466: triangles grouped by their dominant skin joint, or by the mesh
//                   node, in that node's space, sorted and split into 48-triangle chunks with boxes)
//   :204317         the pose of a clip at a time (every node's model-space matrix, iíîìíïî)
//   :204603-204687  the ray against the posed hit mesh (slab test per group and chunk, Moller-Trumbore)
//   :205154-205450  AI.setPartDamage / glowPart / setNodeVisible / partMult / hitMesh / clipAt / hitTest /
//                   partPosition / setSolid / applySolid
//   :206429-206446  AI manager rigs / hitMeshes / loadModel (user-assets <id>/model.glb, async)
//   :206452-206468  AI.playAnim: the server's clip state (n, t0, reps, hold) and heldClip
//   :206506-206568  "aip" AI_PARTS: syncParts (client) and the partsDirty send in AI.sync
// The model is the asset's model.glb under assets_path() ("user-assets/<id>/model.glb"), asked for through the fetch
// hook (the client's asset_prefetch): until it arrives (or when it is missing) a precise AI is hit on its box, as in v10.0.3
// while loadModel runs. Clients use the same model for ai.solid (the collision tris of the solid pose).
#include <ai.h>

#define AI_PARTS_MAX 32        // partGlow / hiddenNodes (v10.0.3: 32); setPartDamage too (KN cap)
#define AI_PART_NAME_SIZE 49   // part / node names: /^[\w.\-]{1,48}$/ (iiÍíìíï :209277)

typedef struct AiPartsModel AiPartsModel;

enum { AI_PARTS_MODEL_LOADING = 0, AI_PARTS_MODEL_READY = 1, AI_PARTS_MODEL_FAILED = 2 };

typedef struct {
    char name[AI_PART_NAME_SIZE];
    float mult;                // 0..20
} AiPartDamage;

typedef struct {
    char name[AI_PART_NAME_SIZE];
    char color[8];             // "#rgb" / "#rrggbb", "" = the material's own emissive colour
    double intensity;          // 0 < .. <= 1 (+toFixed(2): a double, so the wire carries 0.46, not its float)
} AiPartGlow;

typedef struct {
    char clip[GAME_AI_ANIM_SIZE]; // n: "" = null (the rest pose)
    double t;                  // seconds into the clip
    float x, y, z, d;          // the AI's position and xD at setSolid
    float s, r, yo;            // modelScale, mROff (degrees), mYOff
} AiSolidPose;

// The parts state of one AI (both sides; created on first use, keyed by sid + serial).
typedef struct {
    int sid;
    unsigned serial;
    float body_dmg;            // bodyDmg (1)
    int dmg_count;
    AiPartDamage dmg[AI_PARTS_MAX];
    int glow_count;
    AiPartGlow glow[AI_PARTS_MAX];
    int hidden_count;
    char hidden[AI_PARTS_MAX][AI_PART_NAME_SIZE];
    char held_clip[GAME_AI_ANIM_SIZE]; // heldClip, "" none
    unsigned char dirty:1;     // partsDirty: "aip" to every viewer at the next AI.sync
    unsigned char clip_set:1;  // clipState (server)
    unsigned char clip_hold:1;
    unsigned char clip_ended:1;
    unsigned char solid:1;     // solidPose != null
    unsigned char solid_wait:1;// the model is still loading: applySolid again each tick
    unsigned char dmg_part_set:1;
    unsigned char held_late:1; // client: spawned holding (heldLate: jump to the clip's end)
    char clip[GAME_AI_ANIM_SIZE];
    double clip_t0;            // ms (game time)
    double clip_reps;          // max(1, reps || 1)
    AiSolidPose solid_pose;
    float *solid_tris;         // owned: 9 floats per world-space triangle
    size_t solid_count;
    char dmg_part[AI_PART_NAME_SIZE]; // dmgPart: the part of the hit about to changeHealth
    char held_shown[GAME_AI_ANIM_SIZE]; // client render: heldShown
} AiParts;

// ---- the model (one per asset id, cached on the GameAIs; v10.0.3 AI.rigs / AI.hitMeshes)
// Parses a GLB (iîìïiìî): NULL when it is not a GLB or has no JSON chunk. A GLB without a BIN chunk
// keeps its node rig (names, parents) and no clips / hit triangles.
AiPartsModel *ai_parts_model_parse(const unsigned char *glb, size_t length);
void ai_parts_model_free(AiPartsModel*);
int ai_parts_model_node_count(const AiPartsModel*);
int ai_parts_model_node_find(const AiPartsModel*, const char *name); // byName: the first node so named, -1 none
const char *ai_parts_model_node_name(const AiPartsModel*, int node);
int ai_parts_model_node_parent(const AiPartsModel*, int node);       // -1 for a root
int ai_parts_model_hit_tris(const AiPartsModel*);                    // the hit mesh's triangle count
// iîiîïîì (:204179): the mesh nodes' triangles at the rest pose, model space, 9 floats each, at most 6000 (the cap of
// liveObjects.loadModel, :212173): a live asset's complexCollision (ks_entities.c). NULL / 0 when it has none.
const float *ai_parts_model_rest_tris(const AiPartsModel*, size_t *count);
int ai_parts_model_has_clip(const AiPartsModel*, const char *name);
float ai_parts_model_clip_duration(const AiPartsModel*, const char *name); // seconds, 0 when missing
// The model-space matrices (column-major 4x4, out: node_count * 16) of clip `clip` (NULL / unknown: the
// rest pose) at `t` seconds (iíîìíïî).
void ai_parts_model_pose(const AiPartsModel*, const char *clip, double t, double *out);
// The model of `asset` for this game: asks for it (loadModel) when `load` and it is not cached yet, and
// polls a load still running. *state is AI_PARTS_MODEL_*; returns the model only when ready.
AiPartsModel *ai_parts_model_get(Game*, int asset, int load, int *state);
// Installs a parsed model for an asset (tests); takes ownership. NULL marks the asset failed.
void ai_parts_model_put(Game*, int asset, AiPartsModel*);
// How a model file is asked for before it is read: 1 present, 0 still arriving (asked again later), -1 missing. The
// client sets asset_prefetch (the web fetches in the background, the native client its asset downloads); without a
// hook (server_bin, kn_brain, tools, tests) the file is read at once.
void ai_parts_set_fetch_hook(int (*fetch)(const char *path));

// ---- per AI
AiParts *ai_parts_of(Game*, const GameAI*, int create); // NULL when the AI has none (create: NULL on OOM)
void ai_parts_spawned(Game*, GameAI*);   // AI.init: idleT0 = now, movT0 = 0
void ai_parts_moved(Game*, GameAI*);     // the update: didMov changed -> movT0 = now
// AI.hitMesh: a precise AI's model on the authority (asks for it); NULL = hit its box
AiPartsModel *ai_parts_hit_model(Game*, GameAI*);
// AI.hitTest(x, y, z, dx, dy, dz): the ray origin + t * delta, t in [0, 1], against the posed hit mesh
// (hidden nodes skipped). 1 on a hit: *t (>= 0.0001), the part and its multiplier (partMult: the nearest
// setPartDamage of the part or an ancestor, else bodyDamage; 0 = an armour hit).
int ai_parts_hit_test(Game*, GameAI*, vec3 origin, vec3 delta, float *t, char *part, size_t part_size, float *mult);
// AI.partPosition(name): the world position of the part's mesh centre in the current pose; 0 when the
// asset's rig is not loaded or has no such node.
int ai_parts_part_position(Game*, GameAI*, const char *name, vec3 *out);
// AI.clipAt(now ms): the clip and its time (seconds) the AI is posed with; 0 none.
int ai_parts_clip_at(Game*, GameAI*, double now_ms, char *clip, size_t clip_size, double *t);
// The KrunkScript setters (values already validated by the binding).
void ai_parts_set_part_damage(Game*, GameAI*, const char *part, float mult); // setPartDamage
void ai_parts_set_body_damage(Game*, GameAI*, float mult);                   // bodyDamage =
float ai_parts_body_damage(Game*, const GameAI*);
void ai_parts_glow_part(Game*, GameAI*, const char *part, const char *color, double intensity); // glowPart
void ai_parts_node_visible(Game*, GameAI*, const char *node, int visible);   // setNodeVisible
// dmgPart: set by the hit right before changeHealth, consumed by it (onAIPartDamage).
void ai_parts_set_dmg_part(Game*, GameAI*, const char *part); // NULL clears
int ai_parts_take_dmg_part(Game*, GameAI*, char *out, size_t size);
// AI.setSolid / applySolid: the solid pose (a snapshot of the clip pose and the placement) and its collision
// triangles (at most 6000 of area > 0.01, the largest kept) for player movement (world_player_tris).
int ai_parts_solid(Game*, const GameAI*);
void ai_parts_set_solid(Game*, GameAI*, int solid);
void ai_parts_apply_solid(Game*, GameAI*);
void ai_parts_drop_solid(Game*, GameAI*); // removeFully / remove: solidPose = null; applySolid()
void ai_parts_tick(Game*);                // the manager update: a solidWait AI retries applySolid
// The collision triangles of the solid AIs: entry `i` (9 floats per triangle, world space), NULL past the end
// (an entry may have none: *tri_count 0).
const float *ai_parts_solid_tris(Game*, int i, size_t *tri_count);
// AI.playAnim's server clip state (raw clip names: scripts / triggers) and heldClip (partsDirty on a change).
void ai_parts_play_anim(Game*, GameAI*, const char *clip, double reps, int hold);

// ---- the "aip" record (AI.sync :206561-206568, AI.syncParts :206506-206520):
// [sid, partGlow {name: [color, intensity]}, hiddenNodes [names], heldClip | null, solidPose | null]
// as UTF-8 JSON, malloc'd (NULL on OOM); ai_parts_apply_json applies one on a client mirror.
char *ai_parts_record_json(Game*, const GameAI*);
void ai_parts_apply_json(Game*, const char *json, size_t length);
int ai_parts_has_state(Game*, const GameAI*); // what a late viewer must be told (getData pg / hn / hc / sp)
// The wire: NET_PACKET_EXT subtype AI_PARTS_EXT_SUBTYPE (0x0c00, "aip"; the 0x0c00 block of the net_ext.h
// registry), a NetExtJsonChunk header + a slice of the record, split over consecutive EXTs like the entities'.
// 10.0.3 sends pg / hn / hc / sp inside getData ("aai"); KN's fixed "aai" has no room, so a late viewer gets the
// "aip" right after its "aai" (deliberate). EXT peers only (older peers keep the box hits and no parts).
#define AI_PARTS_EXT_SUBTYPE 0x0c00
// getData's `refl: this.dat.refl || undefined` (deob-10.0.3.js:205147): EXT 0x0c01 NetAIReflPacket right after the "aai"
// of an AI with reflections (the fixed "aai" stays as older peers read it); clients set data.refl (ai_parts_on_refl).
#define AI_REFL_EXT_SUBTYPE 0x0c01
#pragma pack(push, 1)
typedef struct {
    int32_t sid;
    float refl;     // 0 < refl <= 4
} NetAIReflPacket;
#pragma pack(pop)
void ai_parts_on_refl(Game*, const uint8_t *body, uint16_t length);
void ai_parts_send(Game*, const GameAI*, int uid);              // to one connection (uid or NET_TARGET_*)
void ai_parts_on_ext(Game*, const uint8_t *body, uint16_t length); // a client mirror: assemble and apply

// ---- GameAIs lifetime
void ai_parts_reset(GameAIs*); // AI.reset: the parts and solids go, the model cache stays
void ai_parts_free(GameAIs*);
