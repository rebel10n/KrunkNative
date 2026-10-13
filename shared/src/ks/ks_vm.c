// KrunkScript bytecode interpreter: script instances, host entries (hooks, callbacks),
// call frames, the loop budget / instruction budget / call depth limits and the runtime
// errors of doc/v7/krunkscript.md 1.13-1.14 with their exact texts.
//
// Host entry = ks_script_new (global initialisers), ks_script_call_hook, ks_call_value
// from outside the VM. Each entry resets LC and the instruction budget; a runtime error
// aborts only that entry and is logged as "RUNTIME ERROR: <msg>" (globals keep what was
// written before the error).
#include "ks_rt.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) {
        fprintf(stderr, "KrunkScript: out of memory\n");
        abort();
    }
    return q;
}

void ks_raise_fatal(KsScript *s, const char *msg)
{
    if (s->err) return;
    ks_raise(s, "%s", msg);
    s->err_fatal = 1;
}

void ks_runtime_error(KsScript *s, const char *msg)
{
    if (s) ks_raise(s, "%s", msg ? msg : "Error");
}

const char *ks_api_path(int api_id)
{
    const KsApiEntry *e = ks_api_entry(api_id);
    return e && e->path ? e->path : "?";
}

// ---------------------------------------------------------------------------
// Stack

static inline void stack_reserve(KsScript *s, uint32_t extra)
{
    if (s->sp + extra > s->stack_cap) {
        uint32_t cap = s->stack_cap ? s->stack_cap * 2 : 256;
        while (cap < s->sp + extra) cap *= 2;
        s->stack = xrealloc(s->stack, cap * sizeof(KsValue));
        s->stack_cap = cap;
    }
}

static inline void push(KsScript *s, KsValue v)
{
    if (s->sp == s->stack_cap) stack_reserve(s, 1);
    s->stack[s->sp++] = v;
}

// ---------------------------------------------------------------------------
// JS operators

static KsStr *chr_str(KsScript *s, uint16_t unit)
{
    if (unit < 128) {
        if (!s->chr[unit]) {
            char c = (char)unit;
            s->chr[unit] = ks_str_new_raw(s, &c, 1, 1, 1);
        }
        return s->chr[unit];
    }
    char buf[4];
    size_t n;
    if (unit < 0x800) {
        buf[0] = (char)(0xC0 | (unit >> 6));
        buf[1] = (char)(0x80 | (unit & 0x3F));
        n = 2;
    } else {
        buf[0] = (char)(0xE0 | (unit >> 12));
        buf[1] = (char)(0x80 | ((unit >> 6) & 0x3F));
        buf[2] = (char)(0x80 | (unit & 0x3F));
        n = 3;
    }
    return ks_str_new_raw(s, buf, n, 1, 0);
}

// ToPrimitive (default hint) for objects; primitives unchanged.
static KsValue to_primitive(KsScript *s, KsValue v)
{
    if (v.tag >= KS_ARR) return ks_str_value(ks_to_str(s, v));
    return v;
}

static KsValue js_add(KsScript *s, KsValue a, KsValue b)
{
    if (a.tag == KS_NUM && b.tag == KS_NUM) return ks_num(a.as.num + b.as.num);
    if (a.tag == KS_STR && b.tag == KS_STR) return ks_str_value(ks_str_concat(s, KS_AS_STR(a), KS_AS_STR(b)));
    KsValue pa = to_primitive(s, a), pb = to_primitive(s, b);
    if (pa.tag == KS_STR || pb.tag == KS_STR)
        return ks_str_value(ks_str_concat(s, ks_to_str(s, pa), ks_to_str(s, pb)));
    return ks_num(ks_to_number(pa) + ks_to_number(pb));
}

static inline double num_of(KsValue v) { return v.tag == KS_NUM ? v.as.num : ks_to_number(v); }

static double js_pow(double x, double y)
{
    if (isnan(y)) return NAN;
    if (y == 0) return 1;
    if ((x == 1 || x == -1) && isinf(y)) return NAN;
    return pow(x, y);
}

// Abstract relational comparison; returns 1 true, 0 false (undefined -> false).
static int js_less(KsScript *s, KsValue a, KsValue b, int or_equal, int swap)
{
    KsValue pa = to_primitive(s, a), pb = to_primitive(s, b);
    if (swap) { KsValue t = pa; pa = pb; pb = t; }
    if (pa.tag == KS_STR && pb.tag == KS_STR) {
        int c = ks_str_compare(KS_AS_STR(pa), KS_AS_STR(pb));
        return or_equal ? c <= 0 : c < 0;
    }
    double x = ks_to_number(pa), y = ks_to_number(pb);
    if (isnan(x) || isnan(y)) return 0;
    return or_equal ? x <= y : x < y;
}

// N(x) = parseInt(x) || 0
static double index_n(KsScript *s, KsValue i)
{
    if (i.tag == KS_NUM) {
        double d = i.as.num;
        if (d >= 0 && d < 2147483648.0 && d == floor(d)) return d;
        if (d <= 0 && d > -2147483648.0 && d == ceil(d) && d != 0) return d; // negative integers
        if (d == 0) return 0;
    }
    KsStr *str = ks_to_str(s, i);
    double v = ks_parse_int(str->data, str->len);
    return (isnan(v) || v == 0) ? 0 : v;
}

// BC(a, i): a.hasOwnProperty(N(i)) ? N(i) : error. Returns 0 on error.
static int bounds_check(KsScript *s, KsValue a, KsValue i, int line, int col, double *out)
{
    if (a.tag == KS_UNDEF || a.tag == KS_NULL) {
        ks_raise(s, "Cannot read properties of %s (reading 'hasOwnProperty')", a.tag == KS_NULL ? "null" : "undefined");
        return 0;
    }
    double n = index_n(s, i);
    double len = a.tag == KS_ARR ? KS_AS_ARR(a)->len : a.tag == KS_STR ? KS_AS_STR(a)->u16len : 0;
    if (n >= 0 && n < len) {
        *out = n;
        return 1;
    }
    ks_raise_at(s, line, col, "Index out of range.");
    return 0;
}

// A(x, f): scalar conversion, element-wise and in place for arrays.
static KsValue cast_value(KsScript *s, KsValue v, int base, int depth)
{
    if (v.tag == KS_ARR) {
        if (depth > 1000) {
            ks_raise_fatal(s, "Maximum call stack size exceeded");
            return v;
        }
        KsArray *a = KS_AS_ARR(v);
        // Shared sub-arrays are visited once per reference (like JS): charge the instruction
        // budget so a DAG of arrays cannot take exponential time.
        s->budget -= a->len;
        if (s->budget < 0) {
            ks_raise_fatal(s, "Execution timed out.");
            return v;
        }
        for (uint32_t i = 0; i < a->len && !s->err; i++) a->items[i] = cast_value(s, a->items[i], base, depth + 1);
        return v;
    }
    switch (base) {
    case KS_T_NUM: {
        double d;
        if (v.tag == KS_NUM) d = v.as.num;
        else if (v.tag == KS_STR) d = ks_parse_float(KS_AS_STR(v)->data, KS_AS_STR(v)->len);
        else if (v.tag == KS_BOOL || v.tag == KS_UNDEF || v.tag == KS_NULL || v.tag >= KS_OBJ) d = NAN;
        else d = NAN;
        return ks_num((isnan(d) || d == 0) ? 0 : d);
    }
    case KS_T_STR:
        return ks_str_value(ks_to_str(s, v));
    case KS_T_BOOL:
        return ks_bool(ks_truthy(v));
    case KS_T_OBJ:
        if (v.tag == KS_OBJ || v.tag == KS_HOST || v.tag == KS_NULL) return v;
        return ks_ref_value(KS_OBJ, ks_object_new(s));
    }
    return v;
}

static KsValue entries_of(KsScript *s, KsValue x)
{
    KsArray *out = ks_array_new(s, 0);
    KsValue outv = ks_ref_value(KS_ARR, out);
    // Keep `out` reachable while entries are built: nothing collects here (no safe point).
#define ADD_ENTRY(keyv, valv)                                       \
    do {                                                            \
        KsObject *e = ks_object_new(s);                             \
        ks_object_put(s, e, s->s_key, (keyv));                      \
        ks_object_put(s, e, s->s_value, (valv));                    \
        ks_array_push(s, out, ks_ref_value(KS_OBJ, e));             \
    } while (0)
    char num[32];
    switch (x.tag) {
    case KS_STR: {
        KsStr *str = KS_AS_STR(x);
        for (uint32_t i = 0; i < str->u16len && !s->err; i++) {
            uint16_t u;
            ks_str_unit_at(str, i, &u);
            size_t n = ks_num_format(i, num);
            ADD_ENTRY(ks_str_value(ks_str_new_raw(s, num, n, (uint32_t)n, 1)), ks_str_value(chr_str(s, u)));
        }
        break;
    }
    case KS_ARR: {
        KsArray *a = KS_AS_ARR(x);
        for (uint32_t i = 0; i < a->len && !s->err; i++) {
            size_t n = ks_num_format(i, num);
            ADD_ENTRY(ks_str_value(ks_str_new_raw(s, num, n, (uint32_t)n, 1)), a->items[i]);
        }
        break;
    }
    case KS_OBJ: {
        KsObject *o = KS_AS_OBJ(x);
        for (uint32_t i = 0; i < o->count && !s->err; i++) ADD_ENTRY(ks_str_value(o->props[i].key), o->props[i].val);
        break;
    }
    case KS_HOST: {
        KsHostObj *h = KS_AS_HOST(x);
        for (int i = 0; i < h->cls->prop_count && !s->err; i++) {
            KsValue v;
            ks_get_prop(s, x, h->prop_keys[i], &v);
            ADD_ENTRY(ks_str_value(h->prop_keys[i]), v);
        }
        if (h->expando)
            for (uint32_t i = 0; i < h->expando->count && !s->err; i++)
                ADD_ENTRY(ks_str_value(h->expando->props[i].key), h->expando->props[i].val);
        break;
    }
    default: break;
    }
#undef ADD_ENTRY
    return outv;
}

static int not_empty(KsValue x)
{
    switch (x.tag) {
    case KS_STR: return KS_AS_STR(x)->len > 0;
    case KS_ARR: return KS_AS_ARR(x)->len > 0;
    case KS_OBJ: return KS_AS_OBJ(x)->count > 0;
    case KS_HOST: return KS_AS_HOST(x)->cls->prop_count > 0 || (KS_AS_HOST(x)->expando && KS_AS_HOST(x)->expando->count > 0);
    default: return 0;
    }
}

// ---------------------------------------------------------------------------
// Native calls

typedef struct ArgCopy {
    KsValue local[16];
    KsValue *v;
} ArgCopy;

// Copies args out of the VM stack: a native may re-enter the VM and grow the stack.
static const KsValue *copy_args(ArgCopy *c, const KsValue *args, int argc)
{
    c->v = argc <= 16 ? c->local : xrealloc(NULL, (size_t)argc * sizeof(KsValue));
    if (argc) memcpy(c->v, args, (size_t)argc * sizeof(KsValue));
    return c->v;
}
static void free_args(ArgCopy *c) { if (c->v != c->local) free(c->v); }

static void internalize_error(KsScript *s, const char *path)
{
    if (s->err && !s->err_fatal) snprintf(s->err_msg, sizeof s->err_msg, "Internal Error \"%s\"", path);
}

static KsValue call_api(KsScript *s, int id, const KsValue *args, int argc)
{
    KsApiFn fn = NULL;
    if (id >= 0 && id < KS_API_COUNT) {
        if (s->host && s->host->api) fn = s->host->api[id].fn;
        if (!fn) fn = ks_stdlib_fn(id);
    }
    if (!fn) return ks_undef();
    ArgCopy c;
    const KsValue *a = copy_args(&c, args, argc);
    s->native_depth++;
    KsValue r = fn(s, s->user, a, argc);
    s->native_depth--;
    free_args(&c);
    internalize_error(s, ks_api_path(id));
    return r;
}

static void set_api(KsScript *s, int id, KsValue v)
{
    KsApiFn fn = NULL;
    if (id >= 0 && id < KS_API_COUNT && s->host && s->host->api) fn = s->host->api[id].set;
    if (!fn) return;
    s->native_depth++;
    fn(s, s->user, &v, 1);
    s->native_depth--;
    internalize_error(s, ks_api_path(id));
}

static KsValue call_method(KsScript *s, KsHostObj *self, const KsHostProp *p, const KsValue *args, int argc)
{
    if (!self->native) return ks_undef();
    ArgCopy c;
    const KsValue *a = copy_args(&c, args, argc);
    s->native_depth++;
    KsValue r = p->method(s, self->native, a, argc);
    s->native_depth--;
    free_args(&c);
    if (s->err && !s->err_fatal) snprintf(s->err_msg, sizeof s->err_msg, "Internal Error \"%s.%s\"", self->cls->name, p->name);
    return r;
}

// ---------------------------------------------------------------------------
// Interpreter

static inline uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static inline uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline int32_t rdi32(const uint8_t *p) { int32_t v; memcpy(&v, p, 4); return v; }

// Pushes a frame for `action` whose `argc` arguments are on the stack top; `entry_sp` is
// the stack height to restore on return. Returns the entry ip or UINT32_MAX on error.
static uint32_t enter_action(KsScript *s, uint32_t action, int argc, uint32_t entry_sp, uint32_t ret_ip)
{
    const KsActionInfo *a = &s->prog->actions[action];
    if (s->depth >= KS_MAX_DEPTH || s->sp + a->locals > KS_MAX_STACK) {
        ks_raise_fatal(s, "Maximum call stack size exceeded");
        return UINT32_MAX;
    }
    while (argc < a->params) { push(s, ks_undef()); argc++; }
    if (argc > a->params) s->sp -= (uint32_t)(argc - a->params);
    stack_reserve(s, (uint32_t)(a->locals - a->params) + 8);
    uint32_t base = s->sp - a->params;
    for (int i = a->params; i < a->locals; i++) s->stack[s->sp++] = ks_undef();
    if (s->depth == s->frames_cap) {
        s->frames_cap = s->frames_cap ? s->frames_cap * 2 : 64;
        s->frames = xrealloc(s->frames, s->frames_cap * sizeof(KsFrame));
    }
    KsFrame *f = &s->frames[s->depth++];
    f->action = action;
    f->ret_ip = ret_ip;
    f->base = base;
    f->entry_sp = entry_sp;
    return a->code;
}

// Calls a KS_FN value whose args sit on the stack top above the callee slot at entry_sp.
// Script actions push a frame (returns its ip); natives complete immediately (returns
// UINT32_MAX - 1 with the result pushed). UINT32_MAX = error.
#define KS_IP_DONE (UINT32_MAX - 1)
static uint32_t call_fn_value(KsScript *s, KsFn *f, int argc, uint32_t entry_sp, uint32_t ret_ip)
{
    if (f->kind == KS_FN_ACTION) return enter_action(s, (uint32_t)f->index, argc, entry_sp, ret_ip);
    KsValue r;
    const KsValue *args = s->stack + s->sp - argc;
    if (f->kind == KS_FN_API) r = call_api(s, f->index, args, argc);
    else r = call_method(s, f->self, f->method, args, argc);
    if (s->err) return UINT32_MAX;
    s->sp = entry_sp;
    push(s, r);
    return KS_IP_DONE;
}

#define POP() (s->stack[--s->sp])
#define TOP(n) (s->stack[s->sp - 1 - (n)])
#define LOCAL(i) (s->stack[base + (i)])
#define CHECK()           \
    do {                  \
        if (s->err) goto error; \
    } while (0)

static int vm_run(KsScript *s, uint32_t ip, uint32_t stop_depth)
{
    const uint8_t *code = s->prog->code;
    uint32_t base = s->frames[s->depth - 1].base;
    char numbuf[32];
    (void)numbuf;

    for (;;) {
        if (--s->budget < 0) {
            ks_raise_fatal(s, "Execution timed out.");
            goto error;
        }
        uint8_t op = code[ip++];
        switch (op) {
        case KS_OPC_UNDEF: push(s, ks_undef()); break;
        case KS_OPC_NULL: push(s, ks_null()); break;
        case KS_OPC_TRUE: push(s, ks_bool(1)); break;
        case KS_OPC_FALSE: push(s, ks_bool(0)); break;
        case KS_OPC_NUM: push(s, ks_num(s->prog->nums[rd32(code + ip)])); ip += 4; break;
        case KS_OPC_STR: push(s, ks_str_value(s->kstr[rd32(code + ip)])); ip += 4; break;
        case KS_OPC_POP: s->sp--; break;
        case KS_OPC_DUP: push(s, TOP(0)); break;
        case KS_OPC_DUP2: {
            KsValue a = TOP(1), b = TOP(0);
            push(s, a);
            push(s, b);
            break;
        }
        case KS_OPC_GET_LOCAL: push(s, LOCAL(rd16(code + ip))); ip += 2; break;
        case KS_OPC_SET_LOCAL: LOCAL(rd16(code + ip)) = POP(); ip += 2; break;
        case KS_OPC_GET_GLOBAL: push(s, s->globals[rd32(code + ip)]); ip += 4; break;
        case KS_OPC_SET_GLOBAL: s->globals[rd32(code + ip)] = POP(); ip += 4; break;

        case KS_OPC_GET_MEMBER: {
            KsStr *name = s->kstr[rd32(code + ip)];
            ip += 4;
            KsValue o = TOP(0), v = ks_undef();
            if (o.tag == KS_OBJ) {
                KsValue *slot = ks_object_find(KS_AS_OBJ(o), name);
                if (slot) v = *slot;
            } else {
                ks_get_prop(s, o, name, &v);
                CHECK();
            }
            TOP(0) = v;
            break;
        }
        case KS_OPC_SET_MEMBER: {
            KsStr *name = s->kstr[rd32(code + ip)];
            ip += 4;
            KsValue v = POP(), o = POP();
            ks_set_prop(s, o, name, v);
            CHECK();
            break;
        }
        case KS_OPC_GET_INDEX: {
            int line = (int)rd32(code + ip), col = (int)rd32(code + ip + 4);
            ip += 8;
            KsValue i = POP(), a = TOP(0);
            if (a.tag == KS_ARR && i.tag == KS_NUM) { // fast path
                double d = i.as.num;
                KsArray *arr = KS_AS_ARR(a);
                if (d >= 0 && d < arr->len && d == (double)(uint32_t)d) {
                    TOP(0) = arr->items[(uint32_t)d];
                    break;
                }
            }
            double n;
            if (!bounds_check(s, a, i, line, col, &n)) goto error;
            if (a.tag == KS_ARR) {
                TOP(0) = KS_AS_ARR(a)->items[(uint32_t)n];
            } else {
                uint16_t u = 0;
                ks_str_unit_at(KS_AS_STR(a), (uint32_t)n, &u);
                TOP(0) = ks_str_value(chr_str(s, u));
            }
            break;
        }
        case KS_OPC_CHECK_INDEX: {
            int line = (int)rd32(code + ip), col = (int)rd32(code + ip + 4);
            ip += 8;
            double n;
            if (!bounds_check(s, TOP(1), TOP(0), line, col, &n)) goto error;
            TOP(0) = ks_num(n);
            break;
        }
        case KS_OPC_SET_INDEX: {
            KsValue v = POP(), i = POP(), a = POP();
            if (a.tag == KS_ARR) {
                KS_AS_ARR(a)->items[(uint32_t)i.as.num] = v;
            } else if (a.tag == KS_STR) {
                char buf[96];
                snprintf(buf, sizeof buf, "%s", KS_AS_STR(a)->data);
                ks_raise(s, "Cannot assign to read only property '%u' of string '%s'", (unsigned)i.as.num, buf);
                goto error;
            }
            break;
        }
        case KS_OPC_GET_API: {
            int id = rd16(code + ip);
            ip += 2;
            KsValue r = call_api(s, id, NULL, 0);
            CHECK();
            push(s, r);
            break;
        }
        case KS_OPC_SET_API: {
            int id = rd16(code + ip);
            ip += 2;
            set_api(s, id, POP());
            CHECK();
            break;
        }

        case KS_OPC_CALL: {
            uint32_t action = rd32(code + ip);
            int argc = code[ip + 4];
            ip += 5;
            ks_gc_maybe(s);
            CHECK();
            uint32_t nip = enter_action(s, action, argc, s->sp - (uint32_t)argc, ip);
            if (nip == UINT32_MAX) goto error;
            ip = nip;
            base = s->frames[s->depth - 1].base;
            break;
        }
        case KS_OPC_CALL_VALUE: {
            int argc = code[ip];
            uint32_t text = rd32(code + ip + 1);
            ip += 5;
            uint32_t callee = s->sp - (uint32_t)argc - 1;
            KsValue f = s->stack[callee];
            if (f.tag != KS_FN) {
                ks_raise(s, "%s is not a function", s->kstr[text]->data);
                goto error;
            }
            ks_gc_maybe(s);
            CHECK();
            uint32_t nip = call_fn_value(s, KS_AS_FN(f), argc, callee, ip);
            if (nip == UINT32_MAX) goto error;
            if (nip != KS_IP_DONE) {
                ip = nip;
                base = s->frames[s->depth - 1].base;
            }
            break;
        }
        case KS_OPC_CALL_API: {
            int id = rd16(code + ip);
            int argc = code[ip + 2];
            ip += 3;
            KsValue r = call_api(s, id, s->stack + s->sp - argc, argc);
            CHECK();
            s->sp -= (uint32_t)argc;
            push(s, r);
            break;
        }
        case KS_OPC_CALL_METHOD: {
            KsStr *name = s->kstr[rd32(code + ip)];
            int argc = code[ip + 4];
            int line = (int)rd32(code + ip + 5), col = (int)rd32(code + ip + 9);
            ip += 13;
            uint32_t callee = s->sp - (uint32_t)argc - 1;
            KsValue recv = s->stack[callee], fv = ks_undef();
            if (recv.tag == KS_HOST) {
                KsHostObj *h = KS_AS_HOST(recv);
                const KsHostProp *p = ks_host_find_prop(h, name);
                if (p && p->method) {
                    KsValue r = call_method(s, h, p, s->stack + s->sp - argc, argc);
                    CHECK();
                    s->sp = callee;
                    push(s, r);
                    break;
                }
            }
            if (recv.tag == KS_OBJ) {
                KsValue *slot = ks_object_find(KS_AS_OBJ(recv), name);
                if (slot) fv = *slot;
            } else {
                ks_get_prop(s, recv, name, &fv);
                CHECK();
            }
            if (fv.tag != KS_FN) {
                char msg[200];
                snprintf(msg, sizeof msg, "Property \"%s\" is not an an \"action\".", name->data);
                ks_raise_at(s, line, col, msg);
                goto error;
            }
            s->stack[callee] = fv;
            ks_gc_maybe(s);
            CHECK();
            uint32_t nip = call_fn_value(s, KS_AS_FN(fv), argc, callee, ip);
            if (nip == UINT32_MAX) goto error;
            if (nip != KS_IP_DONE) {
                ip = nip;
                base = s->frames[s->depth - 1].base;
            }
            break;
        }
        case KS_OPC_ACTION_REF:
            push(s, ks_ref_value(KS_FN, ks_fn_action(s, rd32(code + ip))));
            ip += 4;
            break;
        case KS_OPC_API_REF:
            push(s, ks_ref_value(KS_FN, ks_fn_api(s, rd16(code + ip))));
            ip += 2;
            break;

        case KS_OPC_JMP: ip = (uint32_t)((int32_t)(ip + 4) + rdi32(code + ip)); break;
        case KS_OPC_JF: {
            KsValue c = POP();
            int t = c.tag == KS_BOOL ? c.as.b : ks_truthy(c);
            ip = t ? ip + 4 : (uint32_t)((int32_t)(ip + 4) + rdi32(code + ip));
            break;
        }
        case KS_OPC_JF_KEEP:
            if (!ks_truthy(TOP(0))) ip = (uint32_t)((int32_t)(ip + 4) + rdi32(code + ip));
            else { s->sp--; ip += 4; }
            break;
        case KS_OPC_JT_KEEP:
            if (ks_truthy(TOP(0))) ip = (uint32_t)((int32_t)(ip + 4) + rdi32(code + ip));
            else { s->sp--; ip += 4; }
            break;
        case KS_OPC_LOOP:
            if (s->loop_count++ > KS_MAX_LOOP) {
                ks_raise_at(s, (int)rd32(code + ip), (int)rd32(code + ip + 4), "Loop timed out.");
                goto error;
            }
            ip += 8;
            if (s->bytes >= s->gc_threshold) {
                ks_gc_maybe(s);
                CHECK();
            }
            break;

        case KS_OPC_ADD: {
            KsValue b = POP();
            TOP(0) = js_add(s, TOP(0), b);
            CHECK();
            break;
        }
#define NUM_BINOP(OPC, EXPR)                         \
        case OPC: {                                  \
            double y = num_of(POP()), x = num_of(TOP(0)); \
            TOP(0) = ks_num(EXPR);                   \
            break;                                   \
        }
        NUM_BINOP(KS_OPC_SUB, x - y)
        NUM_BINOP(KS_OPC_MUL, x * y)
        NUM_BINOP(KS_OPC_DIV, x / y)
        NUM_BINOP(KS_OPC_MOD, ks_js_mod(x, y))
        NUM_BINOP(KS_OPC_POW, js_pow(x, y))
        NUM_BINOP(KS_OPC_SHL, (double)(int32_t)(ks_to_uint32(x) << (ks_to_uint32(y) & 31)))
        NUM_BINOP(KS_OPC_SHR, (double)(ks_to_int32(x) >> (ks_to_uint32(y) & 31)))
        NUM_BINOP(KS_OPC_USHR, (double)(ks_to_uint32(x) >> (ks_to_uint32(y) & 31)))
        NUM_BINOP(KS_OPC_BAND, (double)(ks_to_int32(x) & ks_to_int32(y)))
        NUM_BINOP(KS_OPC_BOR, (double)(ks_to_int32(x) | ks_to_int32(y)))
        NUM_BINOP(KS_OPC_BXOR, (double)(ks_to_int32(x) ^ ks_to_int32(y)))
#undef NUM_BINOP
        case KS_OPC_EQ: {
            KsValue b = POP();
            TOP(0) = ks_bool(ks_strict_equal(TOP(0), b));
            break;
        }
        case KS_OPC_NE: {
            KsValue b = POP();
            TOP(0) = ks_bool(!ks_strict_equal(TOP(0), b));
            break;
        }
        case KS_OPC_LT:
        case KS_OPC_LE:
        case KS_OPC_GT:
        case KS_OPC_GE: {
            KsValue b = POP(), a = TOP(0);
            int r;
            if (a.tag == KS_NUM && b.tag == KS_NUM) {
                double x = a.as.num, y = b.as.num;
                r = op == KS_OPC_LT ? x < y : op == KS_OPC_LE ? x <= y : op == KS_OPC_GT ? x > y : x >= y;
            } else if (op == KS_OPC_LT) r = js_less(s, a, b, 0, 0);
            else if (op == KS_OPC_LE) r = js_less(s, a, b, 1, 0);
            else if (op == KS_OPC_GT) r = js_less(s, a, b, 0, 1);
            else r = js_less(s, a, b, 1, 1);
            TOP(0) = ks_bool(r);
            break;
        }
        case KS_OPC_NEG: TOP(0) = ks_num(-num_of(TOP(0))); break;
        case KS_OPC_POS: TOP(0) = ks_num(num_of(TOP(0))); break;
        case KS_OPC_BNOT: TOP(0) = ks_num((double)~ks_to_int32(num_of(TOP(0)))); break;
        case KS_OPC_NOT: TOP(0) = ks_bool(!ks_truthy(TOP(0))); break;
        case KS_OPC_INC: TOP(0) = ks_num(num_of(TOP(0)) + 1); break;
        case KS_OPC_DEC: TOP(0) = ks_num(num_of(TOP(0)) - 1); break;
        case KS_OPC_CAST:
            TOP(0) = cast_value(s, TOP(0), code[ip++], 0);
            CHECK();
            break;
        case KS_OPC_TOSTR:
            TOP(0) = ks_str_value(ks_to_str(s, TOP(0)));
            CHECK();
            break;
        case KS_OPC_LENGTHOF: {
            KsValue v = TOP(0);
            double n = 0;
            if (v.tag == KS_UNDEF || v.tag == KS_NULL) {
                ks_raise(s, "Cannot read properties of %s (reading 'length')", v.tag == KS_NULL ? "null" : "undefined");
                goto error;
            }
            if (v.tag == KS_STR) n = KS_AS_STR(v)->u16len;
            else if (v.tag == KS_ARR) n = KS_AS_ARR(v)->len;
            else if (v.tag == KS_FN && KS_AS_FN(v)->kind == KS_FN_ACTION) n = s->prog->actions[KS_AS_FN(v)->index].params;
            TOP(0) = ks_num(n);
            break;
        }
        case KS_OPC_NOTEMPTY: TOP(0) = ks_bool(not_empty(TOP(0))); break;
        case KS_OPC_HASPROP: {
            KsStr *name = s->kstr[rd32(code + ip)];
            ip += 4;
            int has;
            if (!ks_has_own(s, TOP(0), name, &has)) goto error;
            TOP(0) = ks_bool(has);
            break;
        }
        case KS_OPC_ADDTO: {
            uint32_t text = rd32(code + ip);
            ip += 4;
            KsValue v = POP(), a = POP();
            if (a.tag == KS_ARR) {
                ks_array_push(s, KS_AS_ARR(a), v);
                CHECK();
            } else if (a.tag == KS_UNDEF || a.tag == KS_NULL) {
                ks_raise(s, "Cannot read properties of %s (reading 'push')", a.tag == KS_NULL ? "null" : "undefined");
                goto error;
            } else {
                ks_raise(s, "%s.push is not a function", s->kstr[text]->data);
                goto error;
            }
            break;
        }
        case KS_OPC_REMOVE_INDEX: {
            KsValue i = POP(), a = POP();
            if (a.tag == KS_ARR) {
                KsArray *arr = KS_AS_ARR(a);
                double d = num_of(i);
                d = isnan(d) ? 0 : trunc(d);
                double len = arr->len;
                double start = d < 0 ? fmax(len + d, 0) : fmin(d, len);
                if (start < len) ks_array_splice1(s, arr, (uint32_t)start);
            }
            break;
        }
        case KS_OPC_REMOVE_MEMBER: {
            KsStr *name = s->kstr[rd32(code + ip)];
            ip += 4;
            KsValue o = POP();
            if (o.tag == KS_UNDEF || o.tag == KS_NULL) {
                ks_raise(s, "Cannot convert undefined or null to object");
                goto error;
            }
            if (o.tag == KS_OBJ) ks_object_delete(s, KS_AS_OBJ(o), name);
            else if (o.tag == KS_HOST && KS_AS_HOST(o)->expando && !ks_host_find_prop(KS_AS_HOST(o), name))
                ks_object_delete(s, KS_AS_HOST(o)->expando, name);
            break;
        }
        case KS_OPC_NEW_ARRAY: {
            uint32_t n = rd32(code + ip);
            ip += 4;
            if (n > KS_MAX_ARR_LEN) {
                ks_raise(s, "Invalid array length");
                goto error;
            }
            KsArray *a = ks_array_new(s, n);
            if (n) memcpy(a->items, s->stack + s->sp - n, n * sizeof(KsValue));
            a->len = n;
            s->sp -= n;
            push(s, ks_ref_value(KS_ARR, a));
            break;
        }
        case KS_OPC_NEW_OBJECT: push(s, ks_ref_value(KS_OBJ, ks_object_new(s))); break;
        case KS_OPC_INIT_PROP: {
            KsStr *name = s->kstr[rd32(code + ip)];
            ip += 4;
            KsValue v = POP();
            ks_object_put(s, KS_AS_OBJ(TOP(0)), name, v);
            CHECK();
            break;
        }
        case KS_OPC_ENTRIES:
            TOP(0) = entries_of(s, TOP(0));
            CHECK();
            break;
        case KS_OPC_FOREACH: {
            uint16_t arr_slot = rd16(code + ip), idx_slot = rd16(code + ip + 2), var_slot = rd16(code + ip + 4);
            int32_t exit_rel = rdi32(code + ip + 6);
            ip += 10;
            KsArray *arr = KS_AS_ARR(LOCAL(arr_slot));
            double i = LOCAL(idx_slot).as.num;
            if (i < arr->len) {
                LOCAL(var_slot) = arr->items[(uint32_t)i];
                LOCAL(idx_slot) = ks_num(i + 1);
            } else {
                ip = (uint32_t)((int32_t)ip + exit_rel);
            }
            break;
        }
        case KS_OPC_RETURN:
        case KS_OPC_RETURN_UNDEF: {
            KsValue r = op == KS_OPC_RETURN ? POP() : ks_undef();
            KsFrame *f = &s->frames[--s->depth];
            s->sp = f->entry_sp;
            push(s, r);
            ip = f->ret_ip;
            if (s->depth == stop_depth) return 0;
            base = s->frames[s->depth - 1].base;
            break;
        }
        default:
            ks_raise_fatal(s, "Invalid bytecode.");
            goto error;
        }
    }
error:
    return -1;
}

// ---------------------------------------------------------------------------
// Host entries

static void log_error(KsScript *s)
{
    char line[400];
    snprintf(line, sizeof line, "RUNTIME ERROR: %s", s->err_msg);
    if (s->host && s->host->log) s->host->log(s->user, 1, line);
}

// Runs `action` (or the initialiser when action == UINT32_MAX) with args. Returns 0 ok.
static int vm_entry(KsScript *s, uint32_t action, const KsValue *args, int argc, KsValue *result)
{
    int outer = s->running == 0;
    if (outer) {
        s->loop_count = 0;
        s->budget = KS_MAX_INSTR;
        s->err = 0;
        s->err_fatal = 0;
    }
    if (s->err) return -1;
    uint32_t sp0 = s->sp, depth0 = s->depth;
    ArgCopy c;
    const KsValue *a = copy_args(&c, args, argc);
    stack_reserve(s, (uint32_t)argc + 1);
    s->stack[s->sp++] = ks_undef(); // callee slot
    for (int i = 0; i < argc; i++) s->stack[s->sp++] = a[i];
    free_args(&c);
    uint32_t ip;
    if (action == UINT32_MAX) {
        // the initialiser frame: no parameters, init_locals hidden slots
        if (s->depth == s->frames_cap) {
            s->frames_cap = s->frames_cap ? s->frames_cap * 2 : 64;
            s->frames = xrealloc(s->frames, s->frames_cap * sizeof(KsFrame));
        }
        s->sp = sp0 + 1;
        stack_reserve(s, s->prog->init_locals + 8u);
        KsFrame *f = &s->frames[s->depth++];
        f->action = UINT32_MAX;
        f->ret_ip = 0;
        f->base = s->sp;
        f->entry_sp = sp0;
        for (int i = 0; i < s->prog->init_locals; i++) s->stack[s->sp++] = ks_undef();
        ip = s->prog->init_code;
    } else {
        ip = enter_action(s, action, argc, sp0, 0);
    }
    int rc = -1;
    if (ip != UINT32_MAX) {
        if (outer) ks_gc_maybe(s);
        if (!s->err) {
            s->running++;
            rc = vm_run(s, ip, depth0);
            s->running--;
        }
    }
    if (rc == 0 && result) *result = s->stack[s->sp - 1];
    s->sp = sp0;
    s->depth = depth0;
    if (rc != 0 && outer) {
        if (s->oom) s->failed = 1;
        log_error(s);
        s->err = 0;
        s->err_fatal = 0;
    }
    return rc;
}

static int find_public(const KsScript *s, const char *name)
{
    const KsProgram *p = s->prog;
    for (uint32_t i = 0; i < p->action_count; i++)
        if (p->actions[i].is_public && strcmp(p->actions[i].name, name) == 0) return (int)i;
    return -1;
}

KsScript *ks_script_new(const KsProgram *prog, const KsHost *host, void *host_user)
{
    if (!prog) return NULL;
    KsScript *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->prog = prog;
    s->host = host;
    s->user = host_user;
    s->gc_threshold = KS_GC_MIN_THRESHOLD;
    ks_script_seed(s, 0x853c49e6748fea9bull);
    s->s_empty = ks_str_intern(s, "", 0);
    s->s_key = ks_str_intern(s, "key", 3);
    s->s_value = ks_str_intern(s, "value", 5);
    s->kstr = xrealloc(NULL, (prog->str_count ? prog->str_count : 1) * sizeof(KsStr *));
    for (uint32_t i = 0; i < prog->str_count; i++) s->kstr[i] = ks_str_intern(s, prog->strs[i].data, prog->strs[i].len);
    s->action_fns = calloc(prog->action_count ? prog->action_count : 1, sizeof(KsFn *));
    s->globals = calloc(prog->global_count ? prog->global_count : 1, sizeof(KsValue));
    if (!s->action_fns || !s->globals) abort();
    stack_reserve(s, 256);
    if (vm_entry(s, UINT32_MAX, NULL, 0, NULL) != 0) s->failed = 1;
    return s;
}

void ks_script_free(KsScript *s)
{
    if (!s) return;
    ks_heap_free_all(s);
    free(s->kstr);
    free(s->action_fns);
    free(s->globals);
    free(s->stack);
    free(s->frames);
    free(s);
}

int ks_script_has_hook(const KsScript *s, const char *name)
{
    return s && name && find_public(s, name) >= 0;
}

int ks_script_call_hook(KsScript *s, const char *name, const KsValue *args, int argc)
{
    if (!s || s->failed || !name) return -1;
    int a = find_public(s, name);
    if (a < 0) return 0;
    return vm_entry(s, (uint32_t)a, args, argc < 0 ? 0 : argc, NULL);
}

int ks_script_call_hook_value(KsScript *s, const char *name, const KsValue *args, int argc, KsValue *result)
{
    if (result) *result = ks_undef();
    if (!s || s->failed || !name) return -1;
    int a = find_public(s, name);
    if (a < 0) return 0;
    return vm_entry(s, (uint32_t)a, args, argc < 0 ? 0 : argc, result);
}

int ks_script_failed(const KsScript *s) { return !s || s->failed; }
void *ks_script_user(KsScript *s) { return s ? s->user : NULL; }

void ks_script_seed(KsScript *s, uint64_t seed)
{
    // PCG32 (pcg_basic srandom: initstate = seed, initseq = 54)
    s->rng_state = 0;
    s->rng_inc = (54u << 1u) | 1u;
    s->rng_state = s->rng_state * 6364136223846793005ull + s->rng_inc;
    s->rng_state += seed;
    s->rng_state = s->rng_state * 6364136223846793005ull + s->rng_inc;
}

static uint32_t pcg32(KsScript *s)
{
    uint64_t old = s->rng_state;
    s->rng_state = old * 6364136223846793005ull + s->rng_inc;
    uint32_t xorshifted = (uint32_t)(((old >> 18u) ^ old) >> 27u);
    uint32_t rot = (uint32_t)(old >> 59u);
    return (xorshifted >> rot) | (xorshifted << ((-rot) & 31));
}

double ks_rand01(KsScript *s)
{
    // 53 random bits like Math.random's double in [0, 1)
    uint64_t hi = pcg32(s) >> 5, lo = pcg32(s) >> 6;
    return ((double)hi * 67108864.0 + (double)lo) / 9007199254740992.0;
}

KsValue ks_call_value(KsScript *s, KsValue fn, const KsValue *args, int argc, int *ok)
{
    if (ok) *ok = 0;
    if (!s || s->failed || fn.tag != KS_FN) return ks_undef();
    KsFn *f = KS_AS_FN(fn);
    KsValue r = ks_undef();
    if (f->kind == KS_FN_ACTION) {
        if (vm_entry(s, (uint32_t)f->index, args, argc, &r) != 0) return ks_undef();
    } else {
        int outer = s->running == 0;
        if (outer) {
            s->err = 0;
            s->err_fatal = 0;
        }
        r = f->kind == KS_FN_API ? call_api(s, f->index, args, argc) : call_method(s, f->self, f->method, args, argc);
        if (s->err) {
            if (outer) {
                log_error(s);
                s->err = 0;
            }
            return ks_undef();
        }
    }
    if (ok) *ok = 1;
    return r;
}
