#pragma once
// Krunker v7.1.4 data/triggers.js tables (doc/v7/map-logic.md §3.5-§3.7): event names, condition
// value definitions and action UI flags. GENERATED data in shared/src/trigger_v7_data.c
// (tools/maps/gen_prefab_data.py). Indices are the map JSON values: "te", "cd", "ta".
// Dependency-free (the map validation, the editor and kn_editor_mcp use it); the runtime is WP-C's.
#include <stddef.h>
#include <triggers.h>

typedef enum {
    TRIGGER_V7_VAL_NUMBER, // number: parseFloat || def, limitMM(min, max)
    TRIGGER_V7_VAL_INPUT,  // text: [a-z0-9_] cut to len
    TRIGGER_V7_VAL_SELECT, // option index: limitMM(parseInt || def, 0, option_count - 1)
    TRIGGER_V7_VAL_BOOL,   // !!value
} TriggerV7ValKind;

typedef struct {
    const char *name;     // "Key", "Type", "Val"
    unsigned char kind;   // TriggerV7ValKind
    float def;            // numeric default ("" for inputs)
    int len;              // input length (10)
    int option_count;     // select options
    float min, max, step; // number range
} TriggerV7ConditionVal;

typedef struct {
    const char *name;     // "Player Value"
    int val_count;        // cv0..cv2 in use
    TriggerV7ConditionVal vals[3];
} TriggerV7Condition;

enum {
    TRIGGER_V7_ACT_CUST = 1 << 0,        // hasCust: the value can come from a value type (tce / tcc)
    TRIGGER_V7_ACT_SOUND = 1 << 1,       // hasSound: es / esa / ev / er / ef
    TRIGGER_V7_ACT_EXTRA = 1 << 2,       // hasExtra: a second constant tc2
    TRIGGER_V7_ACT_DEV_ONLY = 1 << 3,    // devOnly (backend actions)
    TRIGGER_V7_ACT_BYPASS_TYPE = 1 << 4, // bypassType
    TRIGGER_V7_ACT_TEXT = 1 << 5,        // type "Text": the value is tct
    TRIGGER_V7_ACT_BOTH = 1 << 6,        // type "Both": tct and tc
};

typedef struct {
    const char *name;          // "Give Player Score"
    unsigned flags;            // TRIGGER_V7_ACT_*
    const char *value_label;   // actionValue ("Node ID"), NULL = "Value"
    const char *value_label2;  // actionValue2 (tc2), NULL none
    const char *text_label;    // actionValueTxt ("Key"), NULL none
    int option_count;          // actionOptions: tc is a select index
    int option_count2;         // actionOptions2: tc2 is a select index
} TriggerV7Action;

extern const char *const g_trigger_v7_events[TRIGGER_V7_EVENT_COUNT];
// The v10.0.2 rows follow the v7 ones (condition 12, actions 72-74); maps may use them all.
extern const TriggerV7Condition g_trigger_v7_conditions[TRIGGER_V10_CONDITION_COUNT];
extern const TriggerV7Action g_trigger_v7_actions[TRIGGER_V10_ACTION_COUNT];
