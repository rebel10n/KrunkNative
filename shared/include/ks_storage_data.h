#pragma once
// GAME.STORAGE data rules (doc/v7/krunkscript.md 2.6, 3.1.12, WP6), shared by the game server
// (server/src/ks_storage.c: request validation and the offline file store) and kn_brain
// (services/brain/db_storage.c: the database store), so both stores behave the same.
//
// v7.1.4 game.storageUpdt (game.js:1168-1223) turns the data object into "k,v,k,v": every key
// and value is ToString'ed and stripped of , ' " and then checked (key <= 20, value <= 100
// UTF-16 units); the key count (<= 30) is checked after the loop. The operation types are the
// trigger "Save Data" options (triggers.js:1186) plus 5 = delete (VER:1680). The stored rows are
// account x map x key -> string value, with a scope (0 private, 1 global: readable by other
// maps of the same creator). The arithmetic of types 1-4 ran inside v7's database (proc 154,
// not in the tree); KN's rules are in ks_storage_apply.
//
// Pure C, no dependencies beyond shared/src/ks/ks_number.c (JS number formatting).
#include <stddef.h>

#define KS_STORAGE_MAX_KEYS 30          // "Too many keys: max 30"
#define KS_STORAGE_KEY_MAX 20           // "Key too long" (UTF-16 units, after stripping)
#define KS_STORAGE_VALUE_MAX 100        // "Value too long"
#define KS_STORAGE_KEY_BYTES (KS_STORAGE_KEY_MAX * 3 + 1)     // UTF-8 bound of 20 units
#define KS_STORAGE_VALUE_BYTES (KS_STORAGE_VALUE_MAX * 3 + 1)
#define KS_STORAGE_TRIGGER_KEY_MAX 12   // Save Data: trigConstTx.toLowerCase().substring(0, 12)
#define KS_STORAGE_SCRIPT_PREFIX "V_"   // script object keys are stored with v7's V_ prefix

enum {
    KS_STORAGE_SET = 0,        // "Set Value" (STORAGE.set)
    KS_STORAGE_SET_HIGHER = 1, // "Set If Higher"
    KS_STORAGE_SET_LOWER = 2,  // "Set If Lower"
    KS_STORAGE_INCREASE = 3,   // "Increase Value" (STORAGE.update)
    KS_STORAGE_DECREASE = 4,   // "Decrease Value"
    KS_STORAGE_DELETE = 5,     // delete the key (VER:1680)
    KS_STORAGE_TYPE_COUNT
};
enum { KS_STORAGE_PRIVATE = 0, KS_STORAGE_GLOBAL = 1 };

typedef struct {
    char key[KS_STORAGE_KEY_BYTES];
    char value[KS_STORAGE_VALUE_BYTES];
} KsStoragePair;

// The "k,v,k,v" of one request, built pair by pair in the data object's order.
typedef struct {
    KsStoragePair pairs[KS_STORAGE_MAX_KEYS];
    int count;          // pairs kept (the first 30)
    int keys;           // keys seen (the count rule looks at all of them)
    const char *error;  // the first key / value error, NULL none
} KsStorageData;

void ks_storage_data_init(KsStorageData*);
// One data[key] (both already ToString'ed, UTF-8): strips , ' " and applies the length rules.
void ks_storage_data_add(KsStorageData*, const char *key, const char *value);
// storageUpdt's verdict on the whole object: the first key / value error, else
// "Too many keys: max 30", else NULL.
const char *ks_storage_data_error(const KsStorageData*);

// parseFloat(text) (NaN when it does not start with a number, NULL = NaN).
double ks_storage_parse_number(const char *text);
// The JS length (UTF-16 code units) of a UTF-8 string.
size_t ks_storage_u16len(const char *utf8);
// A number as JS ToString formats it ("5", "0.5", "1e+21", "NaN"); out >= 32 bytes.
void ks_storage_format_number(double x, char *out);

// One stored value after operation `type` with the request's `value` on the stored `old`
// (NULL = no row). Returns 1 = store *out (NUL-terminated, <= n bytes), 0 = keep the row as
// it is (or keep having none), -1 = delete the row. [KN] rules: set always writes; set if
// higher / lower writes when there is no numeric old value or parseFloat(value) compares
// higher / lower; increase / decrease add parseFloat(value) to parseFloat(old) (0 when absent
// or not a number) and store the JS-formatted sum; a non-numeric value leaves types 1-4
// unchanged; delete removes an existing row.
int ks_storage_apply(int type, const char *old, const char *value, char *out, size_t n);

// The script-visible name of a stored key (v7 addPrefix, then the KS objects' un-prefixed
// view): "V_coins" -> "coins", a trigger's "coins" -> "coins".
const char *ks_storage_script_key(const char *stored);
