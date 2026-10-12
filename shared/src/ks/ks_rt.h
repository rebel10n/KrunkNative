// KrunkScript runtime internals (WP2): heap objects, the KsScript instance, the compiled
// program layout and the helpers shared by ks_value.c, ks_number.c, ks_serial.c,
// ks_stdlib.c, ks_compile.c and ks_vm.c. Not part of the public ks.h contract.
//
// Strings are stored as WTF-8 (UTF-8 that may contain lone surrogates as 3-byte
// sequences) so every JS UTF-16 string round-trips; lengths, indices and comparisons
// visible to scripts use UTF-16 code units (doc/v7/krunkscript.md 1.12).
#ifndef KS_RT_H
#define KS_RT_H

#include "ks.h"
#include "ks_ast.h"
#include "ks_api.h"

#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Limits (doc/v7/krunkscript.md 1.14)

#define KS_MAX_LOOP 1000000          // LC++ > 1e6 -> "Loop timed out."
#define KS_MAX_INSTR 50000000L       // per host entry -> "Execution timed out."
#define KS_MAX_DEPTH 2000            // "Maximum call stack size exceeded"
#define KS_MAX_STR_UNITS 1048576u    // "Invalid string length"
#define KS_MAX_ARR_LEN 1000000u      // "Invalid array length"
#define KS_MAX_PROPS 65536u          // "Too many properties."
#define KS_HEAP_LIMIT ((size_t)64 << 20) // "Out of memory." (script disabled)
#define KS_GC_MIN_THRESHOLD ((size_t)1 << 20)
#define KS_MAX_STACK (1u << 20)          // VM value slots -> "Maximum call stack size exceeded"
#define KS_MAX_OUTPUT ((size_t)4 << 20)  // GAME.log / serialisation output bytes

// ---------------------------------------------------------------------------
// Heap objects. Every KsValue with tag KS_STR/ARR/OBJ/FN/HOST points at one of these.

enum { KS_GC_STR = 1, KS_GC_ARR, KS_GC_OBJ, KS_GC_FN, KS_GC_HOST };

typedef struct KsObj {
    struct KsObj *next;
    uint8_t type;
    uint8_t marked;
} KsObj;

typedef struct KsStr {
    KsObj h;
    uint32_t len;     // WTF-8 bytes
    uint32_t u16len;  // UTF-16 code units
    uint32_t hash;    // 0 = not computed yet
    uint8_t ascii;
    uint8_t interned;
    uint32_t cache_unit, cache_byte; // last UTF-16 index -> byte offset (sequential s[i])
    char data[];      // len bytes + NUL
} KsStr;

typedef struct KsProp {
    KsStr *key; // interned
    KsValue val;
} KsProp;

typedef struct KsObject {
    KsObj h;
    KsProp *props;      // insertion order, dense (removal compacts)
    uint32_t count, cap;
    int32_t *index;     // open addressing -> props index; NULL while count is small
    uint32_t index_cap;
} KsObject;

typedef struct KsArray {
    KsObj h;
    KsValue *items;
    uint32_t len, cap;
} KsArray;

enum { KS_FN_ACTION, KS_FN_API, KS_FN_METHOD };

struct KsHostObj;
typedef struct KsFn {
    KsObj h;
    uint8_t kind;
    int32_t index;                 // action index / api id
    const KsHostProp *method;      // KS_FN_METHOD
    struct KsHostObj *self;        // KS_FN_METHOD receiver
} KsFn;

typedef struct KsHostObj {
    KsObj h;
    const KsHostClass *cls;
    void *native;
    uint32_t native_id;
    KsObject *expando;             // script-added properties (lazily created)
    KsStr **prop_keys;             // interned names of cls->props (owned by the class cache)
} KsHostObj;

#define KS_AS_STR(v) ((KsStr *)(v).as.ref)
#define KS_AS_ARR(v) ((KsArray *)(v).as.ref)
#define KS_AS_OBJ(v) ((KsObject *)(v).as.ref)
#define KS_AS_FN(v) ((KsFn *)(v).as.ref)
#define KS_AS_HOST(v) ((KsHostObj *)(v).as.ref)

static inline KsValue ks_ref_value(uint8_t tag, void *p)
{
    KsValue v;
    v.tag = tag;
    v.as.ref = (struct KsObj *)p;
    return v;
}
static inline int ks_is_ref(KsValue v) { return v.tag >= KS_STR; }

// ---------------------------------------------------------------------------
// Bytecode. Stack machine; operands follow the opcode little-endian:
// u8 (B), u16 (H), u32 (W), i32 (J, relative to the end of the instruction).
// "l c" = source line/col (two u32) of the construct for positioned errors.

#define KS_OPCODES(X) \
    X(UNDEF)        /*            -> undefined */ \
    X(NULL)         /*            -> null */ \
    X(TRUE)         \
    X(FALSE)        \
    X(NUM)          /* W const    -> num */ \
    X(STR)          /* W const    -> str */ \
    X(POP)          \
    X(DUP)          /* a -> a a */ \
    X(DUP2)         /* a b -> a b a b */ \
    X(GET_LOCAL)    /* H slot */ \
    X(SET_LOCAL)    /* H slot     v -> */ \
    X(GET_GLOBAL)   /* W slot */ \
    X(SET_GLOBAL)   /* W slot     v -> */ \
    X(GET_MEMBER)   /* W name     o -> v */ \
    X(SET_MEMBER)   /* W name     o v -> */ \
    X(GET_INDEX)    /* W l W c    a i -> v      (BC: bounds check) */ \
    X(CHECK_INDEX)  /* W l W c    a i -> a n    (BC before the RHS of a write) */ \
    X(SET_INDEX)    /*            a n v -> */ \
    X(GET_API)      /* H id       -> v          (PROP getter) */ \
    X(SET_API)      /* H id       v ->          (PROP setter) */ \
    X(CALL)         /* W action B argc   args -> r */ \
    X(CALL_VALUE)   /* B argc W text     f args -> r */ \
    X(CALL_API)     /* H id B argc       args -> r */ \
    X(CALL_METHOD)  /* W name B argc W l W c   o args -> r  (FC check) */ \
    X(ACTION_REF)   /* W action   -> fn */ \
    X(API_REF)      /* H id       -> fn */ \
    X(JMP)          /* J */ \
    X(JF)           /* J          c -> */ \
    X(JF_KEEP)      /* J          c -> c (jump) | -> (fall through) */ \
    X(JT_KEEP)      /* J */ \
    X(LOOP)         /* W l W c    loop budget (LC) check */ \
    X(ADD) X(SUB) X(MUL) X(DIV) X(MOD) X(POW) \
    X(SHL) X(SHR) X(USHR) X(BAND) X(BOR) X(BXOR) \
    X(EQ) X(NE) X(LT) X(LE) X(GT) X(GE) \
    X(NEG) X(POS) X(BNOT) X(NOT) \
    X(INC) X(DEC)   /* ToNumber(v) +/- 1 */ \
    X(CAST)         /* B base     A(x, F|S|B|O) in place for arrays */ \
    X(TOSTR)        /* S(x) */ \
    X(LENGTHOF)     \
    X(NOTEMPTY)     \
    X(HASPROP)      /* W name */ \
    X(ADDTO)        /* W text     a v -> */ \
    X(REMOVE_INDEX) /*            a i -> */ \
    X(REMOVE_MEMBER)/* W name     o -> */ \
    X(NEW_ARRAY)    /* W n        v1..vn -> a */ \
    X(NEW_OBJECT)   /*            -> o */ \
    X(INIT_PROP)    /* W name     o v -> o */ \
    X(ENTRIES)      /*            x -> [{key, value}...] (OE) */ \
    X(FOREACH)      /* H arr H idx H var J exit */ \
    X(RETURN)       /*            v -> */ \
    X(RETURN_UNDEF)

enum {
#define KS_OP_ENUM(name) KS_OPC_##name,
    KS_OPCODES(KS_OP_ENUM)
#undef KS_OP_ENUM
    KS_OPC__COUNT
};

// ---------------------------------------------------------------------------
// Compiled program (ks_compile.c builds it, ks_vm.c runs it; immutable, shareable)

typedef struct KsConstStr {
    char *data;
    uint32_t len;
} KsConstStr;

typedef struct KsActionInfo {
    char *name;
    uint32_t code;          // entry offset in KsProgram.code
    uint16_t params;
    uint16_t locals;        // including params and hidden temporaries
    uint8_t is_public;
} KsActionInfo;

struct KsProgram {
    KsSide side;
    uint8_t *code;
    uint32_t code_len, code_cap;
    double *nums;
    uint32_t num_count, num_cap;
    KsConstStr *strs;
    uint32_t str_count, str_cap;
    KsActionInfo *actions;
    uint32_t action_count;
    uint32_t global_count;
    uint32_t init_code;     // global initialiser pseudo-action
    uint16_t init_locals;
};

// ks_compile.c: AST (checked by the front end) -> bytecode. NULL + *err on failure.
KsProgram *ks_codegen(const KsAstProgram *ast, KsSide side, KsDiag *err);

// ---------------------------------------------------------------------------
// Script instance

typedef struct KsFrame {
    uint32_t action;   // index into actions, or UINT32_MAX for the initialiser
    uint32_t ret_ip;   // ip to resume in the caller
    uint32_t base;     // stack index of local 0
    uint32_t entry_sp; // stack height to restore on return (callee slot)
} KsFrame;

typedef struct KsHostCacheEntry {
    const KsHostClass *cls;
    uint32_t native_id;
    KsHostObj *obj;
} KsHostCacheEntry;

typedef struct KsClassKeys {
    const KsHostClass *cls;
    KsStr **keys;
} KsClassKeys;

struct KsScript {
    const KsProgram *prog;
    const KsHost *host;
    void *user;

    // heap
    KsObj *objects;
    size_t bytes, gc_threshold;
    uint32_t gc_count;
    KsObj **gray;
    size_t gray_len, gray_cap;
    int oom;

    // interned strings (weak; rebuilt after each sweep)
    KsStr **intern;
    uint32_t intern_cap, intern_count;

    // roots
    KsValue *globals;
    KsStr **kstr;           // program string constants (interned)
    KsFn **action_fns;      // cached action values (identity: f == f)
    KsValue *pins;
    uint32_t pin_count, pin_cap;
    KsHostCacheEntry *hcache;
    uint32_t hcache_count, hcache_cap;
    KsClassKeys *class_keys;
    uint32_t class_key_count;

    // VM
    KsValue *stack;
    uint32_t sp, stack_cap;
    KsFrame *frames;
    uint32_t depth, frames_cap;
    int running;            // nesting level of vm entries
    int native_depth;       // > 0 while a C function runs (no GC)
    uint32_t loop_count;    // LC
    long budget;            // instructions left in this host entry

    // error state
    int err;                // pending runtime error
    int err_fatal;          // timeout / stack / memory: never rewritten to "Internal Error"
    char err_msg[320];      // text after "RUNTIME ERROR: "
    int failed;             // load error or out of memory: never run again

    // misc
    uint64_t rng_state, rng_inc;
    KsStr *s_empty, *s_key, *s_value;
    KsStr *chr[128];        // one-character ASCII strings (s[i] results)
};

// ---------------------------------------------------------------------------
// ks_value.c — heap, strings, objects, arrays, host objects, GC

void *ks_heap_alloc(KsScript *s, size_t size, uint8_t type);
void ks_heap_account(KsScript *s, long delta);
void ks_gc_collect(KsScript *s);
void ks_gc_maybe(KsScript *s);
void ks_heap_free_all(KsScript *s);

KsStr *ks_str_new(KsScript *s, const char *utf8, size_t len); // sanitises invalid UTF-8
KsStr *ks_str_new_raw(KsScript *s, const char *wtf8, size_t len, uint32_t u16len, int ascii);
KsStr *ks_str_intern(KsScript *s, const char *utf8, size_t len);
KsStr *ks_str_intern_str(KsScript *s, KsStr *str);
KsStr *ks_str_concat(KsScript *s, KsStr *a, KsStr *b);
KsStr *ks_str_substring(KsScript *s, KsStr *str, uint32_t from, uint32_t to); // UTF-16 units
int ks_str_unit_at(const KsStr *str, uint32_t index, uint16_t *unit);
int ks_str_equal(const KsStr *a, const KsStr *b);
int ks_str_compare(const KsStr *a, const KsStr *b);                              // UTF-16 order
long ks_str_index_of(const KsStr *hay, const KsStr *needle, uint32_t from_byte); // byte offset or -1
uint32_t ks_str_u16_offset(const KsStr *str, uint32_t byte_off);
uint32_t ks_str_hash(KsStr *str);

KsObject *ks_object_new(KsScript *s);
KsValue *ks_object_find(KsObject *o, const KsStr *key);           // key must be interned
int ks_object_put(KsScript *s, KsObject *o, KsStr *key, KsValue v); // 0 on property limit
int ks_object_delete(KsScript *s, KsObject *o, const KsStr *key);

KsArray *ks_array_new(KsScript *s, uint32_t cap);
int ks_array_push(KsScript *s, KsArray *a, KsValue v);             // 0 on length limit
void ks_array_splice1(KsScript *s, KsArray *a, uint32_t at);

KsFn *ks_fn_action(KsScript *s, uint32_t action);
KsFn *ks_fn_api(KsScript *s, int32_t api_id);
KsFn *ks_fn_method(KsScript *s, KsHostObj *self, const KsHostProp *method);

// Host objects
KsStr **ks_class_keys(KsScript *s, const KsHostClass *cls);
const KsHostProp *ks_host_find_prop(KsHostObj *h, const KsStr *key);

// Generic property access used by the VM, serialisation and the host API.
// Returns 0 and raises the JS TypeError on undefined/null receivers.
int ks_get_prop(KsScript *s, KsValue recv, KsStr *key, KsValue *out);
int ks_set_prop(KsScript *s, KsValue recv, KsStr *key, KsValue v);
int ks_has_own(KsScript *s, KsValue recv, KsStr *key, int *out);

// Conversions (1.12)
KsStr *ks_to_str(KsScript *s, KsValue v);     // ToString
int ks_truthy(KsValue v);                     // ToBoolean
int ks_strict_equal(KsValue a, KsValue b);    // ===
double ks_value_to_number(KsScript *s, KsValue v); // ToNumber (arrays via ToString)

// Runtime errors (text after "RUNTIME ERROR: "); the first error wins.
void ks_raise(KsScript *s, const char *fmt, ...);
void ks_raise_at(KsScript *s, int line, int col, const char *msg);

static inline KsValue ks_str_value(KsStr *str) { return ks_ref_value(KS_STR, str); }

// ---------------------------------------------------------------------------
// ks_number.c

size_t ks_num_format(double x, char *out); // out >= 32 bytes; NUL-terminated
double ks_parse_float(const char *s, size_t len);
double ks_parse_int(const char *s, size_t len);
double ks_string_to_number(const char *s, size_t len);
int32_t ks_to_int32(double x);
uint32_t ks_to_uint32(double x);
double ks_js_round(double x);
double ks_js_mod(double a, double b);
int ks_is_js_space(uint32_t cp);
size_t ks_skip_js_space(const char *s, size_t len, size_t i);

// ---------------------------------------------------------------------------
// ks_serial.c

// GAME.log text of one argument (logStringify(sanitize(x))); 0 when v7 would throw
// (cyclic structure, action value) -> Internal Error "log".
int ks_log_format(KsScript *s, KsValue v, char **out, size_t *out_len);

// ---------------------------------------------------------------------------
// ks_stdlib.c

// Shared implementation of a GAME.* entry (log, UTILS.*, UTILS.math.*, TIME.now,
// TIME.getReadable), used when the host leaves api[id].fn NULL. NULL if none.
KsApiFn ks_stdlib_fn(int api_id);
double ks_rand01(KsScript *s);

// ---------------------------------------------------------------------------
// ks_vm.c

// Raises a fatal error (instruction budget, call depth, heap): never converted to
// `Internal Error "<path>"` by the API wrapper.
void ks_raise_fatal(KsScript *s, const char *msg);
// Dotted path for `Internal Error "<path>"` of an API id.
const char *ks_api_path(int api_id);

#endif
