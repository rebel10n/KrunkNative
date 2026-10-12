#pragma once
// Krunker v3.1.1 map triggers (src/data/triggers.js events / actions, map.js addTrigger and
// addAI trigger fields, objects.js addTrigger / addHelpers limits, game.js onTrigger).
//
// Dependency-free on purpose: the map editor (client/src/editor) builds its Event / Action
// selects from these tables like editor.js does from triggers.js (v.events / v.actions):
//   - Event: a TRIGGER lists every event without "onAI"; a BOT lists only the "onAI" ones
//     (and has a "Can Trigger" switch, map key "ct").
//   - Event Value: "tce", slider 0..10000 (TRIGGER_CONSTANT_EVENT_MAX).
//   - Action: "ta", every action; Value: "tct" text field when the action's kind is
//     TRIGGER_VALUE_TEXT, else "tc" slider -10000..10000; Action Chance (%): "cha" 0..100.
//     A TRIGGER_VALUE_SOUND action plays the trigger's sound fields (es/esa, ev, er, ef).
//   - Interface: ID "in" 0..1000 (canInterface prefabs), Target "tin" 0..1000 (TRIGGER).
// The simulation lives in shared/src/triggers.c (server and offline game only).

// triggers.events (index = map key "te")
typedef enum {
    TRIGGER_EVENT_NONE = -1,           // no / unknown event (te >= 16 on newer maps): never fires
    TRIGGER_EVENT_ON_SHOOT = 0,        // "onShoot": hit by a bullet
    TRIGGER_EVENT_ON_MELEE = 1,        // "onMelee": hit by a melee swing
    TRIGGER_EVENT_ON_DAMAGE = 2,       // "onDamage": shot, swung at, hit by a projectile or interface damage
    TRIGGER_EVENT_ON_ENTER = 3,        // "onEnter": a player steps in (walk-through, not shootable)
    TRIGGER_EVENT_ON_DESTROY = 4,      // "onDestroy": its health ran out / an action destroyed it
    TRIGGER_EVENT_ON_RESPAWN = 5,      // "onRespawn": a destroyed trigger came back
    TRIGGER_EVENT_ON_TIMER = 6,        // "onTimer": every Event Value seconds while active
    TRIGGER_EVENT_ON_PROJECTILE = 7,   // "onProjectile": hit by a projectile
    TRIGGER_EVENT_ON_THROWING_MELEE = 8, // "onThrowingMelee": hit by a thrown knife
    TRIGGER_EVENT_ON_PUNCH = 9,        // "onPunch": hit by fists (knife thrown)
    TRIGGER_EVENT_ON_INTERACT = 10,    // "onInteract": hold M1 on the ground inside it for Event Value seconds
    TRIGGER_EVENT_ON_COLLISION = 11,   // "onCollision": touched, at most once per Event Value seconds
    TRIGGER_EVENT_ON_STORAGE_EQUALS = 12,  // "onStorageEquals": its storage became == Event Value
    TRIGGER_EVENT_ON_STORAGE_GREATER = 13, // "onStorageGreater": storage > Event Value
    TRIGGER_EVENT_ON_STORAGE_LESSER = 14,  // "onStorageLesser": storage < Event Value
    TRIGGER_EVENT_ON_AI_DEATH = 15,    // "onAIDeath": a BOT with Can Trigger died
    TRIGGER_EVENT_COUNT
} TriggerEvent;

// triggers.actions (index = map key "ta")
typedef enum {
    TRIGGER_ACTION_GIVE_SCORE = 0,             // "Give Player Score" (Value || 1)
    TRIGGER_ACTION_KILL_PLAYER = 1,            // "Kill Player"
    TRIGGER_ACTION_RESPAWN_PLAYER = 2,         // "Respawn Player": to a spawn point
    TRIGGER_ACTION_CHANGE_HEALTH = 3,          // "Change Player Health": +Value heals, -Value hurts
    TRIGGER_ACTION_DESTROY_INTERFACE = 4,      // "Destroy Interface": destructibles / gates of the Target
    TRIGGER_ACTION_TOGGLE_GATE = 5,            // "Toggle Interface Gate"
    TRIGGER_ACTION_CHANGE_INTERFACE_HEALTH = 6,// "Change Interface Health" (+Value heals)
    TRIGGER_ACTION_TELEPORT_TO_INTERFACE = 7,  // "Teleport To Interface"
    TRIGGER_ACTION_CHANGE_DEPOSIT = 8,         // "Change Interface Deposit Box Amount"
    TRIGGER_ACTION_KILL_OPPOSING_TEAM = 9,     // "Kill Opposing Team"
    TRIGGER_ACTION_RESPAWN_OPPOSING_TEAM = 10, // "Respawn Opposing Team"
    TRIGGER_ACTION_RESPAWN_DESTRUCTIBLE = 11,  // "Respawn Destructible Interface"
    TRIGGER_ACTION_TOGGLE_DESTRUCTIBLE = 12,   // "Toggle Destructible Interface"
    TRIGGER_ACTION_SET_SCORE = 13,             // "Set Player Score"
    TRIGGER_ACTION_BROADCAST_SOUND = 14,       // "Broadcast Sound" (the trigger's es/ev/er/ef)
    TRIGGER_ACTION_CLEAR_LOADOUT = 15,         // "Clear Player Loadout": knife only
    TRIGGER_ACTION_BROADCAST_MESSAGE = 16,     // "Broadcast Message": chat line to everyone
    TRIGGER_ACTION_SEND_MESSAGE = 17,          // "Send Message": chat line to the player
    TRIGGER_ACTION_SWAP_TEAM = 18,             // "Swap Player Team"
    TRIGGER_ACTION_SET_TEAM = 19,              // "Set Player Team" (Value clamped to 1..2)
    TRIGGER_ACTION_ROUND_TIME = 20,            // "Round Time": +Value ms on the round clock
    TRIGGER_ACTION_RESET_INTERFACE = 21,       // "Reset Interface": restart onTimer triggers
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
    TRIGGER_ACTION_COUNT
} TriggerAction;

typedef enum {
    TRIGGER_VALUE_NUMBER, // "tc" slider
    TRIGGER_VALUE_TEXT,   // type: "Text": "tct" text field
    TRIGGER_VALUE_SOUND,  // custom: true (Broadcast Sound): the trigger's sound fields
} TriggerValueKind;

typedef struct {
    const char *name;      // triggers.actions[i].name
    unsigned char kind;    // TriggerValueKind
} TriggerActionInfo;

extern const char *const g_trigger_events[TRIGGER_EVENT_COUNT];        // triggers.events
extern const TriggerActionInfo g_trigger_actions[TRIGGER_ACTION_COUNT];

// config.js limits
#define TRIGGER_LIMIT 20                // config.triggerLimit: actions one trigger chain may run
#define TRIGGER_TEXT_MAX 70             // addTrigger: longer "tct" texts are dropped
#define TRIGGER_CONSTANT_MIN (-10000)   // editor "Value" slider
#define TRIGGER_CONSTANT_MAX 10000
#define TRIGGER_CONSTANT_EVENT_MAX 10000 // addTrigger: limitMM(constantEvent, 0, 10000)
#define TRIGGER_INTERFACE_MAX 2000      // config.interfaceIDMax (v7.1.4; v3.1.1 1000)
#define TRIGGER_EMITTER_VOLUME_MIN 0.05f
#define TRIGGER_EMITTER_VOLUME_MAX 2.0f
#define TRIGGER_EMITTER_RATE_MIN 0.5f
#define TRIGGER_EMITTER_RATE_MAX 4.0f
#define TRIGGER_EMITTER_SOUND_COUNT 51
#define TRIGGER_NETWORK_SOUND_COUNT 51

extern const char *const g_trigger_emitter_sounds[TRIGGER_EMITTER_SOUND_COUNT]; // config.emitterSounds ("es")
extern const char *const g_trigger_network_sounds[TRIGGER_NETWORK_SOUND_COUNT]; // config.networkSounds (BOT "es")

// "tct" check of addTrigger / addAI: <= 70 characters from [\w:;()?,.|[] -+*$@=^!{}].
int trigger_text_valid(const char *text);

// ---- WP-A: Krunker v7.1.4 trigger data model (doc/v7/map-logic.md §3.2-§3.4). The parse lives in
// shared/src/map.c; the runtime (events 0-31, conditions 0-11, actions 0-71) is WP-C's.
#define TRIGGER_V7_EVENT_COUNT 32       // triggers.events
#define TRIGGER_V7_CONDITION_COUNT 12   // triggers.conditions
#define TRIGGER_V7_ACTION_COUNT 72      // triggers.actions
#define TRIGGER_V7_ACTION_LIMIT 5       // config.triggerActionLimit (editor save / validation; the runtime has no cap)
#define TRIGGER_V7_CHAIN_LIMIT 30       // config.triggerLimit (v3.1.1 20)
#define TRIGGER_V7_CUSTOM_VALUE_LIMIT 30 // config.custValLimit
#define TRIGGER_V7_KEY_MAX 10           // "tcc" / input condition values: [a-z0-9_]{0,10}
#define TRIGGER_V7_DELAY_MAX 60.0f      // "tad" seconds (rounded to 0.1)
#define TRIGGER_V7_COST_MAX 10000       // "tac"
#define TRIGGER_V7_EMITTER_RATE_MIN 0.1f // config.emitterRateMin (v3.1.1 0.5)
#define TRIGGER_V7_EMITTER_SOUND_COUNT 73
#define TRIGGER_V7_EVENT_ON_INTERACT_KEY 16
#define TRIGGER_V7_EVENT_ON_EXIT_OBJECT 23

// v7 "onAI*" events (15 onAIDeath, 20 onAIDamage, 29 onAIHalfHealth, 31 onAIKillPlayer): AI_SPAWNER
// only; a TRIGGER drops them (MapManager.addTrigger: eventN.includes("onAI") -> null).
static inline int trigger_v7_event_is_ai(const int event) {
    return event == 15 || event == 20 || event == 29 || event == 31;
}

// One entry of "tas" (MapManager.addTrigger action sanitize, MapManager.js:1938-2041).
typedef struct {
    int action;                    // ta: parseInt || 0 (an index into the 72 v7 actions; validated by WP-C)
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
    int event;               // te (TriggerEvent)
    int action;              // ta (TriggerAction; out of range = no action)
    int chance;              // cha 0..100 (0 = always)
    int constant;            // tc
    int constant_event;      // tce 0..10000: seconds (timer / interact / collision) or the storage threshold
    int interface_target;    // tin, 0 none
    int sound;               // es: g_trigger_emitter_sounds index (BOT: network sound), esa: asset id
    float volume, rate;      // ev / er (limited to the emitter ranges)
    int emit_from;           // ef: non-zero = play at the player
    unsigned char asset_sound:1;   // esa (user asset sounds are not available natively)
    unsigned char network_sound:1; // BOT: "sound" indexes config.networkSounds
    unsigned char single_use:1;    // su
    unsigned char self_destruct:1; // sfd (singleUse && canDestroy): Destroy Self after the action
    unsigned char triggered:1;     // fired at least once this round
    char text[TRIGGER_TEXT_MAX + 1]; // tct ("" none)
    float timer;             // triggerTimer (ms): onTimer countdown
    float last_collision;    // lastColT (game time, ms): onCollision cooldown
    int storage;             // onStorage*: the interface storage

    // ---- WP-A: v7 fields. The single-action fields above mirror actions[0] so the v3.1.1-shaped
    // runtime keeps working until WP-C runs the action list.
    int event_index;         // te as v7 reads it: 0..31, -1 = out of range or an onAI event (never fires)
    int action_cost;         // tac 0..10000 (0 unless te == onInteractKey or tam is set)
    int action_method;       // tam: 0 cost KR, 1 cost score, 2 require score, 3 require kills, 4 account, 5 verified,
                             // 6 premium, 7 partner, 8 level
    unsigned char single_use_player:1; // sup: once per player id per round (uses[])
    int action_count;
    TriggerActionData *actions; // tas (any length; validation caps it at TRIGGER_V7_ACTION_LIMIT); owned
} TriggerData;
