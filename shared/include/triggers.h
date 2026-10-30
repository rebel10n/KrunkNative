#pragma once
// Krunker v7.1.4 map triggers (src/data/triggers.js events / conditions / actions, map.js and
// MapManager.addTrigger / addAI trigger fields, game.js onTrigger). v3.1.1 maps load the way v7
// loads them (map.js:538-557 wraps their single action into tas[0]); the indices of v3.1.1's 16
// events and 32 actions are unchanged in v7 (doc/v7/map-logic.md §3).
//
// Dependency-free on purpose: the map editor (client/src/editor) and kn_editor_mcp read these
// tables and the generated ones in trigger_v7_data.h (UI flags, condition value definitions).
//   - Event: a TRIGGER lists every event without "onAI"; an AI_SPAWNER lists only the "onAI"
//     ones (and has a "Can Trigger" switch, map key "ct").
//   - Event Value: "tce", slider 0..10000 (TRIGGER_CONSTANT_EVENT_MAX).
//   - Actions: "tas" (up to 5 in the editor), each "ta" with "tc" / "tc2" / "tct", a condition
//     "cd" + "cv0".."cv2", a delay "tad" and a value source "tce" + "tcc".
//   - Interface: ID "in" 0..2000 (canInterface prefabs), Target "tin" 0..2000 (TRIGGER).
// The runtime lives in shared/src/triggers*.c (authority only: the server and the offline
// sandbox server); shared/include/triggers_v7.h is its API.

#include <stddef.h>

// triggers.events (index = map key "te")
typedef enum {
    TRIGGER_EVENT_NONE = -1,           // no / unknown event (te out of 0..31, or an onAI event on a TRIGGER): never fires
    TRIGGER_EVENT_ON_SHOOT = 0,        // "onShoot": hit by a bullet
    TRIGGER_EVENT_ON_MELEE = 1,        // "onMelee": hit by a melee swing
    TRIGGER_EVENT_ON_DAMAGE = 2,       // "onDamage": shot, swung at, hit by a projectile or interface damage
    TRIGGER_EVENT_ON_ENTER = 3,        // "onEnterObject" (v3.1.1 "onEnter"): a player steps in (walk-through, not shootable)
    TRIGGER_EVENT_ON_DESTROY = 4,      // "onDestroy": its health ran out / an action destroyed it
    TRIGGER_EVENT_ON_RESPAWN = 5,      // "onRespawn": a destroyed trigger came back
    TRIGGER_EVENT_ON_TIMER = 6,        // "onTimer": every (Event Value || 1) seconds while active
    TRIGGER_EVENT_ON_PROJECTILE = 7,   // "onProjectile": hit by a projectile
    TRIGGER_EVENT_ON_THROWING_MELEE = 8, // "onThrowingMelee": hit by a thrown knife
    TRIGGER_EVENT_ON_PUNCH = 9,        // "onPunch": hit by fists (knife thrown)
    TRIGGER_EVENT_ON_INTERACT = 10,    // "onHoldMouse" (v3.1.1 "onInteract"): hold M1 on the ground inside it for Event Value seconds
    TRIGGER_EVENT_ON_COLLISION = 11,   // "onCollision": touched, at most once per Event Value seconds
    TRIGGER_EVENT_ON_STORAGE_EQUALS = 12,  // "onStorageEquals": its storage became == Event Value
    TRIGGER_EVENT_ON_STORAGE_GREATER = 13, // "onStorageGreater": storage > Event Value
    TRIGGER_EVENT_ON_STORAGE_LESSER = 14,  // "onStorageLesser": storage < Event Value
    TRIGGER_EVENT_ON_AI_DEATH = 15,    // "onAIDeath": an AI with Can Trigger died
    TRIGGER_EVENT_ON_INTERACT_KEY = 16,    // "onInteractKey": the interact key in range 7 ("int" 7, uid)
    TRIGGER_EVENT_ON_PLAYER_SPAWN = 17,    // "onPlayerSpawn" (KN dispatches it, doc/v7/decisions.md)
    TRIGGER_EVENT_ON_PLAYER_KILL = 18,     // "onPlayerKill": the killer
    TRIGGER_EVENT_ON_PLAYER_DIE = 19,      // "onPlayerDie": the victim
    TRIGGER_EVENT_ON_AI_DAMAGE = 20,       // "onAIDamage" (AI only)
    TRIGGER_EVENT_ON_NUKE_START = 21,      // "onNukeStart"
    TRIGGER_EVENT_ON_NUKE_END = 22,        // "onNukeEnd"
    TRIGGER_EVENT_ON_EXIT_OBJECT = 23,     // "onExitObject": a player left it
    TRIGGER_EVENT_ON_CUSTOM_VAL_EQUALS = 24, // "onCustomValEquals": an interface trigger value became == Event Value
    TRIGGER_EVENT_ON_CUSTOM_VAL_LESSER = 25, // "onCustomValLesser"
    TRIGGER_EVENT_ON_GAME_START = 26,      // "onGameStart": after mode.gameStart
    TRIGGER_EVENT_ON_OBJECTIVE_SWITCH = 27, // "onObjectiveSwitch"
    TRIGGER_EVENT_ON_PLAYER_DAMAGE = 28,   // "onPlayerDamage": the victim (damage > 0, still alive)
    TRIGGER_EVENT_ON_AI_HALF_HEALTH = 29,  // "onAIHalfHealth" (AI only)
    TRIGGER_EVENT_ON_CUSTOM_VAL_GREATER = 30, // "onCustomValGreater"
    TRIGGER_EVENT_ON_AI_KILL_PLAYER = 31,  // "onAIKillPlayer" (AI only)
    TRIGGER_EVENT_COUNT
} TriggerEvent;
#define TRIGGER_EVENT_ON_ENTER_OBJECT TRIGGER_EVENT_ON_ENTER
#define TRIGGER_EVENT_ON_HOLD_MOUSE TRIGGER_EVENT_ON_INTERACT

// triggers.actions (index = "ta")
typedef enum {
    TRIGGER_ACTION_GIVE_SCORE = 0,             // "Give Player Score" (value || 1)
    TRIGGER_ACTION_KILL_PLAYER = 1,            // "Kill Player"
    TRIGGER_ACTION_RESPAWN_PLAYER = 2,         // "Respawn Player": to its check point, else a spawn point
    TRIGGER_ACTION_CHANGE_HEALTH = 3,          // "Change Player Health": +value heals, -value hurts
    TRIGGER_ACTION_DESTROY_INTERFACE = 4,      // "Destroy Interface": destructibles / gates of the Target
    TRIGGER_ACTION_TOGGLE_GATE = 5,            // "Toggle Interface Gate"
    TRIGGER_ACTION_CHANGE_INTERFACE_HEALTH = 6,// "Change Interface Health" (+value heals)
    TRIGGER_ACTION_TELEPORT_TO_INTERFACE = 7,  // "Teleport Player To Interface"
    TRIGGER_ACTION_CHANGE_DEPOSIT = 8,         // "Change Interface Deposit Box Amount"
    TRIGGER_ACTION_KILL_OPPOSING_TEAM = 9,     // "Kill Opposing Team"
    TRIGGER_ACTION_RESPAWN_OPPOSING_TEAM = 10, // "Respawn Opposing Team"
    TRIGGER_ACTION_RESPAWN_DESTRUCTIBLE = 11,  // "Respawn Destructible Interface"
    TRIGGER_ACTION_TOGGLE_DESTRUCTIBLE = 12,   // "Toggle Destructible Interface"
    TRIGGER_ACTION_SET_SCORE = 13,             // "Set Player Score"
    TRIGGER_ACTION_BROADCAST_SOUND = 14,       // "Broadcast Sound" (the action's es/ev/er/ef)
    TRIGGER_ACTION_CLEAR_LOADOUT = 15,         // "Clear Player Loadout": knife only
    TRIGGER_ACTION_BROADCAST_MESSAGE = 16,     // "Broadcast Message": chat line to everyone
    TRIGGER_ACTION_SEND_MESSAGE = 17,          // "Send Message": chat line to the player
    TRIGGER_ACTION_SWAP_TEAM = 18,             // "Swap Player Team"
    TRIGGER_ACTION_SET_TEAM = 19,              // "Set Player Team" (value clamped to 1..teamLimit 5)
    TRIGGER_ACTION_ROUND_TIME = 20,            // "Round Time": +value ms on the round clock
    TRIGGER_ACTION_RESET_INTERFACE = 21,       // "Reset Interface"
    TRIGGER_ACTION_END_ROUND = 22,             // "End Round"
    TRIGGER_ACTION_REFILL_KNIFE = 23,          // "Refill Player Knife"
    TRIGGER_ACTION_DESTROY_SELF = 24,          // "Destroy Self"
    TRIGGER_ACTION_INCREASE_STORAGE = 25,      // "Increase Interface Storage"
    TRIGGER_ACTION_DECREASE_STORAGE = 26,      // "Decrease Interface Storage"
    TRIGGER_ACTION_SET_STORAGE = 27,           // "Set Interface Storage"
    TRIGGER_ACTION_RESET_STORAGE = 28,         // "Reset Interface Storage"
    TRIGGER_ACTION_BROADCAST_POPUP = 29,       // "Broadcast Popup": medal popup for everyone
    TRIGGER_ACTION_SEND_POPUP = 30,            // "Send Popup": medal popup for the player
    TRIGGER_ACTION_JUMP_SCARE = 31,            // "Jump Scare"
    TRIGGER_ACTION_REWARD_KR = 32,             // "Reward KR" (backend)
    TRIGGER_ACTION_CLEAR_CHECKPOINTS = 33,     // "Clear Player Checkpoints"
    TRIGGER_ACTION_SEND_SOUND = 34,            // "Send Sound"
    TRIGGER_ACTION_EXPLOSION = 35,             // "Explosion"
    TRIGGER_ACTION_SPAWN_AI = 36,              // "Spawn AI"
    TRIGGER_ACTION_SET_PLAYER_VALUE = 37,      // "Set Player Value"
    TRIGGER_ACTION_CHANGE_PLAYER_VALUE = 38,   // "Inc/Dec Player Value"
    TRIGGER_ACTION_PLAY_ANIMATION = 39,        // "Play Animation"
    TRIGGER_ACTION_TELEPORT_TO_NODE = 40,      // "Teleport Player to Path Node"
    TRIGGER_ACTION_INTERFACE_TO_NODE = 41,     // "Teleport Destructible Interface to Path Node"
    TRIGGER_ACTION_RESET_INTERFACE_POSITION = 42, // "Reset Destructible Interface Position"
    TRIGGER_ACTION_SET_INTERFACE_VALUE = 43,   // "Set Interface Trigger Value"
    TRIGGER_ACTION_CHANGE_INTERFACE_VALUE = 44,// "Inc/Dec Interface Trigger Value"
    TRIGGER_ACTION_SET_TRIGGER_VALUE = 45,     // "Set Trigger Value (Self)"
    TRIGGER_ACTION_CHANGE_TRIGGER_VALUE = 46,  // "Inc/Dec Trigger Value (Self)"
    TRIGGER_ACTION_CHANGE_CLASS = 47,          // "Change Class"
    TRIGGER_ACTION_SET_DEPOSIT = 48,           // "Set Interface Deposit Box Amount"
    TRIGGER_ACTION_SAVE_DATA = 49,             // "Save Data" (backend)
    TRIGGER_ACTION_CHANGE_PRIMARY = 50,        // "Change Primary Weapon"
    TRIGGER_ACTION_CHANGE_SECONDARY = 51,      // "Change Secondary Weapon"
    TRIGGER_ACTION_SET_GLOBAL_VALUE = 52,      // "Set Global Value"
    TRIGGER_ACTION_CHANGE_GLOBAL_VALUE = 53,   // "Change Global Value"
    TRIGGER_ACTION_STOP_SOUND = 54,            // "Stop Sound"
    TRIGGER_ACTION_GUI_ALL = 55,               // "Show/Hide GUI Element (All)"
    TRIGGER_ACTION_GUI_PLAYER = 56,            // "Show/Hide GUI Element (Player)"
    TRIGGER_ACTION_LOAD_DATA = 57,             // "Load Data" (backend)
    TRIGGER_ACTION_CUSTOM = 58,                // "Custom Action": KrunkScript onCustomTrigger
    TRIGGER_ACTION_REWARD_JUNK = 59,           // "Reward Junk" (backend, devOnly)
    TRIGGER_ACTION_REWARD_SKIN = 60,           // "Reward Skin" (backend, devOnly)
    TRIGGER_ACTION_CHANGE_AMBIENT = 61,        // "Change Ambient Sound"
    TRIGGER_ACTION_CHALLENGE = 62,             // "Give Challenge Progress" (backend, devOnly)
    TRIGGER_ACTION_PLAY_ANIMATION_SELF = 63,   // "Play Animation (Self)"
    TRIGGER_ACTION_GIVE_EFFECT = 64,           // "Give Effect"
    TRIGGER_ACTION_CHALLENGE_ALL = 65,         // "Give Challenge Progress (All)" (backend, devOnly)
    TRIGGER_ACTION_RANDOM_EFFECT = 66,         // "Give Random Effect"
    TRIGGER_ACTION_VIDEO_AD = 67,              // "Play Video Ad": FORCE_VIDEO_AD, the placeholder ad (doc/v10/ads.md)
    TRIGGER_ACTION_DROP_POWERUP = 68,          // "Drop Powerup" (devOnly, zombies)
    TRIGGER_ACTION_SPAWN_BOT = 69,             // "Spawn Bot at Node" (devOnly)
    TRIGGER_ACTION_FORCE_AI_TARGET = 70,       // "Force AI Target"
    TRIGGER_ACTION_MODE_SPECIFIC = 71,         // "Mode Specific"
    // v10.0.2 (doc/v10/map-format.md §5)
    TRIGGER_ACTION_SPAWN_ENTITY = 72,          // "Spawn Entity": tct "archetype[|tag]" at the trigger
    TRIGGER_ACTION_DESPAWN_ENTITY = 73,        // "Despawn Entity": tct tag
    TRIGGER_ACTION_SET_ENTITY_VALUE = 74,      // "Set Entity Value": tct "tag|key", value getActionValue(tc)
    TRIGGER_ACTION_COUNT
} TriggerAction;

typedef enum {
    TRIGGER_VALUE_NUMBER, // "tc" slider / select
    TRIGGER_VALUE_TEXT,   // type "Text" / "Both": "tct" text field (Both also "tc")
    TRIGGER_VALUE_SOUND,  // hasSound: the action's sound fields
} TriggerValueKind;

typedef struct {
    const char *name;      // triggers.actions[i].name
    unsigned char kind;    // TriggerValueKind
} TriggerActionInfo;

extern const char *const g_trigger_events[TRIGGER_EVENT_COUNT];        // triggers.events
extern const TriggerActionInfo g_trigger_actions[TRIGGER_ACTION_COUNT];

// config limits: Krunker v10.0.3's (config deob-10.0.3.js:22367-23014; v7.1.4's values unless noted)
#define TRIGGER_LIMIT 30                // config.triggerLimit (:22958): onTrigger calls one trigger chain may run (v3.1.1 20)
#define TRIGGER_TEXT_MAX 70             // addTrigger: longer "tct" texts are dropped
#define TRIGGER_CONSTANT_MIN (-10000)   // editor "Value" slider
#define TRIGGER_CONSTANT_MAX 10000
#define TRIGGER_CONSTANT_EVENT_MAX 10000 // addTrigger: limitMM(constantEvent, 0, 10000)
#define TRIGGER_INTERFACE_MAX 2000      // config.interfaceIDMax (:22628, :20135; v3.1.1 1000)
#define TRIGGER_TEAM_LIMIT 5            // config.teamLimit (:22893; Set Player Team)
#define TRIGGER_INTERACT_KEY_RADIUS 7.0f // addTrigger: onInteractKey tRadius
#define TRIGGER_EMITTER_VOLUME_MIN 0.05f // config.emitterVolMin (:22534, :21276)
#define TRIGGER_EMITTER_VOLUME_MAX 2.0f  // config.emitterVolMax (:22533)
#define TRIGGER_EMITTER_RATE_MIN 0.1f   // config.emitterRateMin (:22529, :21277; v3.1.1 0.5)
#define TRIGGER_EMITTER_RATE_MAX 4.0f   // config.emitterRateMax (:22528)
// config.emitterSounds (:21278, :22532): v10.0.3 appends scare_1 (74; v7.1.4 73; index 16 is scare_0, v3.1.1 jumpS_0)
#define TRIGGER_EMITTER_SOUND_COUNT 74
#define TRIGGER_SOUND_SCAPE_COUNT 14    // config.soundScapes (Change Ambient Sound options)

// config.emitterSounds ("es" of SOUND_EMITTER, TRIGGER and AI_SPAWNER actions); config.networkSounds,
// the list sent as an index, is shared/include/network_sounds.h
extern const char *const g_trigger_emitter_sounds[TRIGGER_EMITTER_SOUND_COUNT];
extern const int g_trigger_sound_scapes[TRIGGER_SOUND_SCAPE_COUNT];             // config.soundScapes values, key order

// The clip an "es" / "esa" sound reference plays, as client/src/audio.c names clips: "asset:<id>"
// for a user asset (esa), else config.emitterSounds[es], emitterSounds[0] for an index the list
// lacks (`assetSnd ? snd : emitterSounds[snd] ? emitterSounds[snd] : emitterSounds[0]`: addEmitter
// deob-10.0.3.js:186574, addTrigger :186330, addAI :187510).
#define TRIGGER_SOUND_NAME_SIZE 24 // "asset:" + any int and every emitterSounds name fit (<= NET_SOUND_NAME_SIZE_V1)
void trigger_sound_name(int sound, int asset, char *out, size_t size);

// A SOUND_EMITTER object: map.js (deob-10.0.3.js:189196-189204) -> MapManager.addEmitter
// (:186572-186580) -> SOUND.play3DL(snd, x, y, z, vol, rate, dist, roll, assetSnd, distM).
typedef struct {
    char sound[TRIGGER_SOUND_NAME_SIZE]; // trigger_sound_name of parseInt(esa || es) || 0
    unsigned char asset;  // esa != null
    float volume;         // ev: parseFloat || 1, emitterVolMin..emitterVolMax (0.05..2)
    float rate;           // er: parseFloat || 1, emitterRateMin..emitterRateMax (0.1..4)
    float distance;       // ed: parseInt || 0, 0..5000 (play3DL refDistance, 0 = 25)
    int distance_model;   // edm: parseInt || 0, 0..2 (Howler inverse / linear / exponential, :224689)
    float rolloff;        // ero: parseFloat || 0, 0..1 (rolloffFactor, 0 = 1)
} TriggerEmitter;
struct cJSON;
// 0 when the object is not a map object (no fields read); every field is defaulted as Krunker does.
int trigger_emitter_parse(const struct cJSON *object, TriggerEmitter *out);

// "tct" check of addTrigger / addAI: <= 70 characters from [\w:;()?,.|[] -+*$@=^!{}].
int trigger_text_valid(const char *text);

// ---- WP-A: Krunker v7.1.4 trigger data model (doc/v7/map-logic.md §3.2-§3.4). The parse lives in
// shared/src/map.c; the runtime (events 0-31, conditions 0-11, actions 0-71) is WP-C's.
#define TRIGGER_V7_EVENT_COUNT 32       // triggers.events
#define TRIGGER_V7_CONDITION_COUNT 12   // triggers.conditions
#define TRIGGER_V7_ACTION_COUNT 72      // triggers.actions
#define TRIGGER_V7_ACTION_LIMIT 5       // config.triggerActionLimit (editor save / validation; the runtime has no cap)
// v10.0.2 (doc/v10/map-format.md §5): + condition 12 "Custom Action", actions 72-74, and
// triggerActionLimit 8 (Ruleset.trigger_action_limit). The tables hold the v10 rows.
#define TRIGGER_V10_CONDITION_COUNT 13
#define TRIGGER_V10_ACTION_COUNT 75
#define TRIGGER_V10_ACTION_LIMIT 8      // config.triggerActionLimit (deob-10.0.3.js:22957)
#define TRIGGER_V10_CONDITION_CUSTOM 12 // mainScript.shouldTrigger(playerId | "NONE", triggerSid, param)
#define TRIGGER_V7_CHAIN_LIMIT 30       // config.triggerLimit (deob-10.0.3.js:22958; v3.1.1 20)
#define TRIGGER_V7_CUSTOM_VALUE_LIMIT 30 // config.custValLimit (deob-10.0.3.js:22500, :20827)
#define TRIGGER_V7_KEY_MAX 10           // "tcc" / input condition values: [a-z0-9_]{0,10}
#define TRIGGER_V7_DELAY_MAX 60.0f      // "tad" seconds (rounded to 0.1)
#define TRIGGER_V7_COST_MAX 10000       // "tac"
#define TRIGGER_V7_EMITTER_RATE_MIN 0.1f // config.emitterRateMin (v3.1.1 0.5)
#define TRIGGER_V7_EMITTER_SOUND_COUNT TRIGGER_EMITTER_SOUND_COUNT // the "es" index range: v10.0.3's 74
#define TRIGGER_V7_EVENT_ON_INTERACT_KEY 16
#define TRIGGER_V7_EVENT_ON_EXIT_OBJECT 23
#define TRIGGER_V7_USES_MAX 64          // KN: per-player single-use list of one trigger (sup); a full list refuses newcomers

// v7 "onAI*" events (15 onAIDeath, 20 onAIDamage, 29 onAIHalfHealth, 31 onAIKillPlayer): AI_SPAWNER
// only; a TRIGGER drops them (MapManager.addTrigger: eventN.includes("onAI") -> null).
static inline int trigger_v7_event_is_ai(const int event) {
    return event == 15 || event == 20 || event == 29 || event == 31;
}

// One entry of "tas" (MapManager.addTrigger action sanitize, MapManager.js:1938-2041).
typedef struct {
    int action;                    // ta: parseInt || 0 (an index into the 72 v7 / 75 v10 actions; validated by WP-C)
    int constant;                  // tc: parseInt || 0
    int constant2;                 // tc2: parseInt || 0
    unsigned char has_text:1;      // tct passed the <= 70 chars charset check
    unsigned char asset_sound:1;   // esa != null
    char text[TRIGGER_TEXT_MAX + 1]; // tct; for ta 37 / 38 reduced to [a-z0-9_]{0,10}
    char value_key[TRIGGER_V7_KEY_MAX + 1]; // tcc: [a-z0-9_]{0,10}, "" none
    int value_type;                // tce: config.inputTypes 0 input / 1 trigger / 2 player / 3 global value
    float delay;                   // tad: 0..60 s, rounded to 0.1
    int condition;                 // cd: parseInt || 0 (WP-C maps an unknown index to 0)
    const void *condition_values[3]; // cv0..cv2: the raw cJSON values (NULL absent); WP-C sanitizes them
                                     // per triggers.conditions[cd].vals
    int sound;                     // es (config.emitterSounds index, invalid -> 0) or the esa asset id
    float volume;                  // ev: 0.05..2 (1)
    float rate;                    // er: 0.1..4 (1)
    int emit_from;                 // ef: 0 at the trigger, 1 at the player, 2 global (2D)
} TriggerActionData;

// A trigger's settings and runtime state: objects.js GameObject trigger* fields of a TRIGGER
// (Object.trig) or ai.js of a BOT with Can Trigger (GameAIData.trig).
typedef struct {
    int event;               // te: the v7 TriggerEvent 0..31 (TRIGGER_EVENT_NONE never fires; == event_index)
    int action;              // ta (TriggerAction; out of range = no action)
    int chance;              // cha 0..100 (0 = always)
    int constant;            // tc
    int constant_event;      // tce 0..10000: seconds (timer / interact / collision) or the storage threshold
    int interface_target;    // tin, 0 none
    int sound;               // es: g_trigger_emitter_sounds index (a BOT's too, as v7.1.4+ read it), esa: asset id
    float volume, rate;      // ev / er (limited to the emitter ranges)
    int emit_from;           // ef: non-zero = play at the player
    unsigned char asset_sound:1;   // esa (user asset sounds are not available natively)
    unsigned char single_use:1;    // su
    unsigned char self_destruct:1; // sfd (singleUse && canDestroy): Destroy Self after the action
    unsigned char triggered:1;     // fired at least once this round
    char text[TRIGGER_TEXT_MAX + 1]; // tct ("" none)
    float timer;             // triggerTimer (ms): onTimer countdown
    float last_collision;    // lastColT (game time, ms): onCollision cooldown
    int storage;             // onStorage*: the interface storage

    // ---- WP-A: v7 fields. The single-action fields above mirror actions[0] (WP-C's runtime runs
    // the action list; the mirror is only read when there is no list).
    int event_index;         // te as v7 reads it: 0..31, -1 = out of range or an onAI event (never fires)
    int action_cost;         // tac 0..10000 (0 unless te == onInteractKey or tam is set)
    int action_method;       // tam: 0 cost KR, 1 cost score, 2 require score, 3 require kills, 4 account, 5 verified,
                             // 6 premium, 7 partner, 8 level
    unsigned char single_use_player:1; // sup: once per player id per round (uses[])
    int action_count;
    TriggerActionData *actions; // tas (any length; validation caps it at the ruleset's trigger_action_limit); owned

    // ---- WP-C: v7 runtime state (shared/src/triggers_v7.c). A TriggerData without actions
    // (actions NULL: hand-built in tests, v3-shaped AI data) runs the mirror fields above as its
    // tas[0], the same wrap map.js:538-557 applies to v3 maps.
    int use_count;                       // uses.length (singleUseP): player uids that fired it this round
    int uses[TRIGGER_V7_USES_MAX];       // (in-struct: AI data is copied by value per spawn)
} TriggerData;

// custVals / globalVals (config.custValLimit): a key set grows only while it holds <= 30 keys
// (`Object.keys(custVals).length > custValLimit` refuses the 32nd), so 31 slots. Keys are the
// raw texts of the actions ("tct": <= 70 chars of the trigger charset; [a-z0-9_]{0,10} for
// actions 37 / 38 and the conditions).
#define TRIGGER_VALUE_SLOTS (TRIGGER_V7_CUSTOM_VALUE_LIMIT + 1)
#define TRIGGER_VALUE_KEY_SIZE (TRIGGER_TEXT_MAX + 1)
// A value is a number or, as v10 keeps it, a string: Load Data's stored value that is not a number
// (`isNaN(parseFloat(v)) ? v : parseFloat(v)`, deob-10.0.3.js:183563) and an official "cv" / "gv" string. The
// conditions and getActionValue read `value`, the text's JS Number() (NaN unless blank or numeric: a string against
// a number compares as that); the HUD shows the text; "Increase" appends to it (JS `+=` on a string). Sizes: any
// stored value (KS_STORAGE_VALUE_BYTES) whole.
#define TRIGGER_VALUE_TEXT_SIZE 301
typedef struct {
    char key[TRIGGER_VALUE_KEY_SIZE];
    double value;
    unsigned char is_text;               // the value is `text` (UTF-8)
    char text[TRIGGER_VALUE_TEXT_SIZE];
} TriggerValue;

typedef struct {
    int count;
    TriggerValue values[TRIGGER_VALUE_SLOTS];
} TriggerValueSet;

// The value of `key`, NULL when the set has none.
const TriggerValue *trigger_values_find(const TriggerValueSet*, const char *key);
// The slot of `key`, created with 0 when missing and the set holds <= 30 keys; NULL when full.
TriggerValue *trigger_values_slot(TriggerValueSet*, const char *key);
// A slot's value: a number (the text goes), a string (value = its JS Number()), or `+= n` as JS adds (a string
// gets String(n) appended).
void trigger_value_set_number(TriggerValue*, double value);
void trigger_value_set_text(TriggerValue*, const char *text);
void trigger_value_add(TriggerValue*, double n);
// JS Number(text): blank -> 0, a whole numeric literal (decimal, exponent, 0x / 0o / 0b, Infinity) -> it, else NaN.
double trigger_js_number(const char *text);
