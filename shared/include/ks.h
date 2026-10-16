// KrunkScript (Krunker v7.1.4) public C API: the contract between the work
// packages of doc/v7/krunkscript.md (section 6.2, 6.5). Frozen: change only by
// agreement between the front end (WP1) and runtime (WP2) owners.
//
// Shared code: no GL, no network, no new dependencies. The engine reaches
// scripts only through this header (plus ks_api.h for the GAME.* id table);
// scripts reach the engine only through the KsHost vtable.
#ifndef KS_H
#define KS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct KsProgram KsProgram;   // immutable compiled script
typedef struct KsScript  KsScript;    // one running instance (globals + heap)
// KS_SIDE_SHARED: the map's scripts.shared (v10 entity simulation: onEntityInit, simulateEntity).
typedef enum { KS_SIDE_CLIENT, KS_SIDE_SERVER, KS_SIDE_SHARED } KsSide;

// A compile diagnostic (error or warning). `message` is the bare catalogue text
// of spec 1.15 ("Variable \"x\" is not defined."); display form is
// "<line>:<col> <message>" (ks_diag_format). line/col are 1-based, a tab is one
// column, columns count UTF-8 code points. line 0 = not tied to a position.
typedef struct {
    int line, col;
    char message[256];
} KsDiag;

// ---------------------------------------------------------------------------
// Values (WP2): opaque handles owned by a KsScript heap
typedef struct KsValue { uint8_t tag; union { double num; int b; struct KsObj *ref; } as; } KsValue;
enum { KS_UNDEF, KS_NULL, KS_BOOL, KS_NUM, KS_STR, KS_ARR, KS_OBJ, KS_FN, KS_HOST };

// ---------------------------------------------------------------------------
// Host interface (6.5)
typedef KsValue (*KsApiFn)(KsScript *, void *user, const KsValue *args, int argc);

// One GAME.* binding, indexed by KsApiId (ks_api.h). `fn` = call (FN entries) or
// getter (PROP entries); `set` = PROP setter (args[0] = value). A NULL KsHost.api
// or a NULL fn is a gated no-op returning undefined; the VM falls back to the
// shared stdlib (UTILS / UTILS.math / TIME) for those entries when fn is NULL.
typedef struct KsApiImpl { KsApiFn fn; KsApiFn set; } KsApiImpl;

// Host object classes (3.2): players, AIs, scene objects, sounds...
typedef struct KsHostProp {
    const char *name;
    KsValue (*get)(KsScript *, void *native, uint32_t id);
    void (*set)(KsScript *, void *native, uint32_t id, KsValue v);   // NULL = read-only
    KsApiFn method;      // non-NULL = method (called with user = native)
    int is_subobject;    // position/rotation/velocity/scale: cannot be replaced
} KsHostProp;

typedef struct KsHostClass {
    const char *name;
    const KsHostProp *props;
    int prop_count;
} KsHostClass;

typedef struct KsHost {
    KsSide side;
    void (*log)(void *user, int is_error, const char *line);   // console / NET_PACKET_SCRIPT_LOG
    double (*now_ms)(void *user);
    const KsApiImpl *api;   // array indexed by KsApiId (KS_API_COUNT entries) or NULL
} KsHost;

// ---------------------------------------------------------------------------
// Script text helpers (WP1, ks_lex.c)

// Decode a map JSON `scripts.client` / `scripts.server` value (1.1): strict
// base64 (v7 b64Test) is decoded as UTF-8, anything else is taken as raw source.
// Fullwidth parentheses U+FF08/U+FF09 become ASCII ( ). Returns a malloc'd,
// NUL-terminated buffer (caller frees) and its length in *out_len, or NULL on OOM.
char *ks_decode_script(const char *text, size_t len, size_t *out_len);
// 1 when `source` is a compiled-JS script ("use strict"; ... const SM = (m) =>). Loaders
// translate it with ks_import_js (ks_import_js.h); only when that fails do they log
// KS_COMPILED_JS_MESSAGE and run nothing (1.1).
int ks_is_compiled_js(const char *source, size_t len);
#define KS_COMPILED_JS_MESSAGE "KrunkScript: compiled JavaScript scripts are not supported"

// Writes "<line>:<col> <message>" (or just the message when line is 0). Returns
// the length that would have been written (snprintf rules).
int ks_diag_format(const KsDiag *d, char *buf, size_t n);

// ---------------------------------------------------------------------------
// Front end (WP1) + compiler (WP2)

// Lex, parse, type-check (WP1, ks_front) then generate code (WP2). NULL on a
// compile error (*err filled; stops at the first error). Warnings go to
// warnings[0..max_warnings) and *warning_count counts all of them (may exceed
// max_warnings). err/warnings/warning_count may be NULL.
KsProgram *ks_compile(const char *source, size_t len, KsSide side, KsDiag *err, KsDiag *warnings, int max_warnings, int *warning_count);
void       ks_program_free(KsProgram *);

// Front end only (WP1): same diagnostics as ks_compile without building a
// program (editor validate, ksc check, server pre-validation). 0 ok, -1 error.
int        ks_validate(const char *source, size_t len, KsSide side, KsDiag *err, KsDiag *warnings, int max_warnings, int *warning_count);

// Parameter count of hook `name` on `side` (4.1), -1 when it is not a hook there.
int        ks_hook_param_count(KsSide side, const char *name);

// ---------------------------------------------------------------------------
// Instances (WP2)
KsScript  *ks_script_new(const KsProgram *, const KsHost *, void *host_user); // runs global initialisers
void       ks_script_free(KsScript *);
int        ks_script_has_hook(const KsScript *, const char *name);
// Hook call with already-converted arguments; returns 0 ok, -1 runtime error (already logged).
int        ks_script_call_hook(KsScript *, const char *name, const KsValue *args, int argc);
// The same, keeping the hook's return value in *result (undefined when it returns nothing).
int        ks_script_call_hook_value(KsScript *, const char *name, const KsValue *args, int argc, KsValue *result);
int        ks_script_failed(const KsScript *);                    // load error -> never call hooks
void      *ks_script_user(KsScript *);                            // host_user given to ks_script_new
void       ks_script_seed(KsScript *, uint64_t seed);             // UTILS.rand* stream (6.4)

// Call an action value (KS_FN) as a new host entry (API callbacks: STORAGE,
// addAsset onLoad). *ok = 0 on a runtime error (already logged). ok may be NULL.
KsValue    ks_call_value(KsScript *, KsValue fn, const KsValue *args, int argc, int *ok);
// Raise a contained runtime error from a host function ("RUNTIME ERROR: <msg>").
void       ks_runtime_error(KsScript *, const char *msg);
// GC roots held by the host (pending callbacks, cached wrappers).
void       ks_handle_pin(KsScript *, KsValue);
void       ks_handle_unpin(KsScript *, KsValue);

// Values (WP2) — opaque handles owned by a KsScript heap
KsValue ks_num(double), ks_bool(int), ks_undef(void), ks_null(void);
KsValue ks_str(KsScript *, const char *utf8, size_t len);
KsValue ks_obj_new(KsScript *);  KsValue ks_arr_new(KsScript *);
void    ks_obj_set(KsScript *, KsValue obj, const char *key, KsValue v);
KsValue ks_obj_get(KsScript *, KsValue obj, const char *key);
// Own properties of a script object (KS_OBJ) in iteration order (insertion order, 1.10): the
// i-th key (un-prefixed, as scripts see it) and value; 0 past the end or for other values.
size_t  ks_obj_count(KsValue obj);
int     ks_obj_entry(KsScript *, KsValue obj, size_t i, KsValue *key, KsValue *value);
void    ks_arr_push(KsScript *, KsValue arr, KsValue v);
KsValue ks_arr_get(KsScript *, KsValue arr, size_t i);
size_t  ks_arr_len(KsValue arr);
const char *ks_str_utf8(KsValue s, size_t *len);                  // NULL when not a string
// One cached wrapper per (class, native_id): every fetch returns the same wrapper
// (identity equality, script-added properties persist) with `native` updated.
KsValue ks_host_object(KsScript *, const KsHostClass *, void *native, uint32_t native_id);
// Drop that wrapper when the engine object dies: its native becomes NULL and its
// getters/setters/methods become no-ops returning undefined.
void    ks_host_forget(KsScript *, const KsHostClass *, uint32_t native_id);
size_t  ks_to_string(KsScript *, KsValue, char *buf, size_t n);   // ToString (1.12)
double  ks_to_number(KsValue);                                    // ToNumber
double  ks_to_float(KsValue);                                     // F(x) = parseFloat(ToString(x)) || 0: (num) casts, host setters
// (de)serialisation for networking/storage (2.5)
size_t  ks_serialize(KsScript *, KsValue, uint8_t *out, size_t cap);
KsValue ks_deserialize(KsScript *, const uint8_t *in, size_t len, int *ok);
size_t  ks_json_length_prefixed(KsScript *, KsValue);             // 2000-char rule

#ifdef __cplusplus
}
#endif

#endif
