// KrunkScript code generator: checked AST (ks_ast.h, WP1) -> stack bytecode (ks_rt.h).
// Also defines ks_compile(), the public front end + codegen entry point.
//
// Lowering follows the v7 compiler output (doc/v7/krunkscript.md 1.16):
//   a[i]           GET_INDEX l c          (BC: N(i) own-index check, read and write)
//   o.f(args)      CALL_METHOD f argc l c (FC: callable check)
//   loops          LOOP l c before each iteration's body (LC++ > 1e6)
//   (T) e          CAST base              (A(x, F|S|B|O): element-wise, in place)
//   toNum / toStr  CAST num / TOSTR
//   for (obj e : x) ENTRIES + FOREACH over two hidden locals
#include "ks_rt.h"
#include "ks_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Patch {
    uint32_t *at;
    int count, cap;
} Patch;

typedef struct Loop {
    Patch breaks, continues;
    struct Loop *outer;
} Loop;

typedef struct Gen {
    KsProgram *p;
    const KsAstProgram *ast;
    KsDiag *err;
    int failed;
    Loop *loop;
    int locals;       // slots in use by the current action (AST locals + hidden)
    // string constant dedupe (open addressing over p->strs indices)
    int32_t *str_index;
    uint32_t str_index_cap;
} Gen;

static void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) abort();
    return q;
}

static void gen_fail(Gen *g, int line, int col, const char *msg)
{
    if (g->failed) return;
    g->failed = 1;
    if (g->err) {
        g->err->line = line;
        g->err->col = col;
        snprintf(g->err->message, sizeof g->err->message, "%s", msg);
    }
}

// ---------------------------------------------------------------------------
// Emission

static void emit_bytes(Gen *g, const void *b, size_t n)
{
    KsProgram *p = g->p;
    if (p->code_len + n > p->code_cap) {
        uint32_t cap = p->code_cap ? p->code_cap * 2 : 1024;
        while (cap < p->code_len + n) cap *= 2;
        p->code = xrealloc(p->code, cap);
        p->code_cap = cap;
    }
    memcpy(p->code + p->code_len, b, n);
    p->code_len += (uint32_t)n;
}

static void emit_op(Gen *g, int op) { uint8_t b = (uint8_t)op; emit_bytes(g, &b, 1); }
static void emit_u8(Gen *g, uint32_t v) { uint8_t b = (uint8_t)v; emit_bytes(g, &b, 1); }
static void emit_u16(Gen *g, uint32_t v) { uint16_t h = (uint16_t)v; emit_bytes(g, &h, 2); }
static void emit_u32(Gen *g, uint32_t v) { emit_bytes(g, &v, 4); }
static void emit_pos(Gen *g, int line, int col) { emit_u32(g, (uint32_t)line); emit_u32(g, (uint32_t)col); }

// Emits a jump with a placeholder; returns the offset of the i32 operand.
static uint32_t emit_jump(Gen *g, int op)
{
    emit_op(g, op);
    uint32_t at = g->p->code_len;
    emit_u32(g, 0);
    return at;
}

static void patch_to(Gen *g, uint32_t at, uint32_t target)
{
    int32_t rel = (int32_t)target - (int32_t)(at + 4);
    memcpy(g->p->code + at, &rel, 4);
}

static void patch_here(Gen *g, uint32_t at) { patch_to(g, at, g->p->code_len); }

static void emit_jump_to(Gen *g, int op, uint32_t target)
{
    uint32_t at = emit_jump(g, op);
    patch_to(g, at, target);
}

static void patch_add(Patch *pl, uint32_t at)
{
    if (pl->count == pl->cap) {
        pl->cap = pl->cap ? pl->cap * 2 : 8;
        pl->at = xrealloc(pl->at, (size_t)pl->cap * sizeof(uint32_t));
    }
    pl->at[pl->count++] = at;
}

static void patch_all(Gen *g, Patch *pl, uint32_t target)
{
    for (int i = 0; i < pl->count; i++) patch_to(g, pl->at[i], target);
    free(pl->at);
    pl->at = NULL;
    pl->count = pl->cap = 0;
}

// ---------------------------------------------------------------------------
// Constants

static uint32_t const_num(Gen *g, double d)
{
    KsProgram *p = g->p;
    for (uint32_t i = 0; i < p->num_count && i < 64; i++)
        if (memcmp(&p->nums[i], &d, sizeof d) == 0) return i;
    if (p->num_count == p->num_cap) {
        p->num_cap = p->num_cap ? p->num_cap * 2 : 32;
        p->nums = xrealloc(p->nums, p->num_cap * sizeof(double));
    }
    p->nums[p->num_count] = d;
    return p->num_count++;
}

static uint32_t str_hash(const char *s, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ (uint8_t)s[i]) * 16777619u;
    return h;
}

static uint32_t const_str(Gen *g, const char *s, size_t n)
{
    KsProgram *p = g->p;
    if ((p->str_count + 1) * 2 > g->str_index_cap) {
        uint32_t cap = g->str_index_cap ? g->str_index_cap * 2 : 256;
        free(g->str_index);
        g->str_index = xrealloc(NULL, cap * sizeof(int32_t));
        memset(g->str_index, 0xFF, cap * sizeof(int32_t));
        g->str_index_cap = cap;
        for (uint32_t i = 0; i < p->str_count; i++) {
            uint32_t h = str_hash(p->strs[i].data, p->strs[i].len) & (cap - 1);
            while (g->str_index[h] >= 0) h = (h + 1) & (cap - 1);
            g->str_index[h] = (int32_t)i;
        }
    }
    uint32_t mask = g->str_index_cap - 1;
    uint32_t h = str_hash(s, n) & mask;
    for (; g->str_index[h] >= 0; h = (h + 1) & mask) {
        KsConstStr *c = &p->strs[g->str_index[h]];
        if (c->len == n && memcmp(c->data, s, n) == 0) return (uint32_t)g->str_index[h];
    }
    if (p->str_count == p->str_cap) {
        p->str_cap = p->str_cap ? p->str_cap * 2 : 64;
        p->strs = xrealloc(p->strs, p->str_cap * sizeof(KsConstStr));
    }
    KsConstStr *c = &p->strs[p->str_count];
    c->data = xrealloc(NULL, n + 1);
    if (n) memcpy(c->data, s, n);
    c->data[n] = 0;
    c->len = (uint32_t)n;
    g->str_index[h] = (int32_t)p->str_count;
    return p->str_count++;
}

static uint32_t const_cstr(Gen *g, const char *s) { return const_str(g, s, s ? strlen(s) : 0); }

// Source text of a node (for "<expr> is not a function" / ".push is not a function").
static uint32_t const_span(Gen *g, const KsNode *n)
{
    const KsAstProgram *a = g->ast;
    if (!a->source || n->end <= n->start || n->end > a->source_len) return const_cstr(g, "expression");
    size_t len = n->end - n->start;
    if (len > 200) len = 200;
    return const_str(g, a->source + n->start, len);
}

// ---------------------------------------------------------------------------
// Expressions

static void gen_expr(Gen *g, const KsNode *n);
static void gen_stmt(Gen *g, const KsNode *n);

static void gen_default(Gen *g, KsType t)
{
    if (t.dims > 0) {
        emit_op(g, KS_OPC_NEW_ARRAY);
        emit_u32(g, 0);
        return;
    }
    switch (t.base) {
    case KS_T_NUM: emit_op(g, KS_OPC_NUM); emit_u32(g, const_num(g, 0)); break;
    case KS_T_STR: emit_op(g, KS_OPC_STR); emit_u32(g, const_str(g, "", 0)); break;
    case KS_T_BOOL: emit_op(g, KS_OPC_FALSE); break;
    case KS_T_OBJ: emit_op(g, KS_OPC_NEW_OBJECT); break;
    default: emit_op(g, KS_OPC_UNDEF); break;
    }
}

static int binop_opcode(int op)
{
    switch (op) {
    case KS_OP_ADD: return KS_OPC_ADD;
    case KS_OP_SUB: return KS_OPC_SUB;
    case KS_OP_MUL: return KS_OPC_MUL;
    case KS_OP_DIV: return KS_OPC_DIV;
    case KS_OP_MOD: return KS_OPC_MOD;
    case KS_OP_POW: return KS_OPC_POW;
    case KS_OP_SHL: return KS_OPC_SHL;
    case KS_OP_SHR: return KS_OPC_SHR;
    case KS_OP_USHR: return KS_OPC_USHR;
    case KS_OP_BAND: return KS_OPC_BAND;
    case KS_OP_BOR: return KS_OPC_BOR;
    case KS_OP_BXOR: return KS_OPC_BXOR;
    case KS_OP_EQ: return KS_OPC_EQ;
    case KS_OP_NE: return KS_OPC_NE;
    case KS_OP_LT: return KS_OPC_LT;
    case KS_OP_LE: return KS_OPC_LE;
    case KS_OP_GT: return KS_OPC_GT;
    case KS_OP_GE: return KS_OPC_GE;
    default: return -1;
    }
}

static void gen_args(Gen *g, const KsNodeList *args)
{
    if (args->count > 255) {
        gen_fail(g, 0, 0, "Too many arguments.");
        return;
    }
    for (int i = 0; i < args->count; i++) gen_expr(g, args->items[i]);
}

static void gen_get_var(Gen *g, int scope, int slot)
{
    if (scope == KS_SCOPE_GLOBAL) { emit_op(g, KS_OPC_GET_GLOBAL); emit_u32(g, (uint32_t)slot); }
    else { emit_op(g, KS_OPC_GET_LOCAL); emit_u16(g, (uint32_t)slot); }
}

static void gen_set_var(Gen *g, int scope, int slot)
{
    if (scope == KS_SCOPE_GLOBAL) { emit_op(g, KS_OPC_SET_GLOBAL); emit_u32(g, (uint32_t)slot); }
    else { emit_op(g, KS_OPC_SET_LOCAL); emit_u16(g, (uint32_t)slot); }
}

static void gen_call(Gen *g, const KsNode *n)
{
    const KsNode *callee = n->u.call.callee;
    switch (n->u.call.call_kind) {
    case KS_CALL_ACTION:
        gen_args(g, &n->u.call.args);
        emit_op(g, KS_OPC_CALL);
        emit_u32(g, (uint32_t)n->u.call.action_index);
        emit_u8(g, (uint32_t)n->u.call.args.count);
        break;
    case KS_CALL_GAME_API:
        gen_args(g, &n->u.call.args);
        emit_op(g, KS_OPC_CALL_API);
        emit_u16(g, (uint32_t)n->u.call.api_id);
        emit_u8(g, (uint32_t)n->u.call.args.count);
        break;
    case KS_CALL_METHOD:
        gen_expr(g, callee->u.member.obj);
        gen_args(g, &n->u.call.args);
        emit_op(g, KS_OPC_CALL_METHOD);
        emit_u32(g, const_cstr(g, callee->u.member.name));
        emit_u8(g, (uint32_t)n->u.call.args.count);
        emit_pos(g, callee->u.member.name_line, callee->u.member.name_col);
        break;
    default: { // KS_CALL_ACTION_VALUE
        gen_expr(g, callee);
        gen_args(g, &n->u.call.args);
        emit_op(g, KS_OPC_CALL_VALUE);
        emit_u8(g, (uint32_t)n->u.call.args.count);
        const char *text = n->u.call.callee_text;
        emit_u32(g, text ? const_cstr(g, text) : const_span(g, callee));
        break;
    }
    }
}

static void gen_cast(Gen *g, KsType to)
{
    switch (to.base) {
    case KS_T_NUM: case KS_T_STR: case KS_T_BOOL: case KS_T_OBJ:
        emit_op(g, KS_OPC_CAST);
        emit_u8(g, to.base);
        break;
    default: break; // any/action: no conversion
    }
}

static void gen_expr(Gen *g, const KsNode *n)
{
    if (g->failed || !n) return;
    switch (n->kind) {
    case KS_E_NUM:
        emit_op(g, KS_OPC_NUM);
        emit_u32(g, const_num(g, n->u.num));
        break;
    case KS_E_STR:
        emit_op(g, KS_OPC_STR);
        emit_u32(g, const_str(g, n->u.str.s, n->u.str.len));
        break;
    case KS_E_BOOL:
        emit_op(g, n->u.boolean ? KS_OPC_TRUE : KS_OPC_FALSE);
        break;
    case KS_E_IDENT:
        gen_get_var(g, n->u.ident.scope, n->u.ident.slot);
        break;
    case KS_E_MEMBER:
        gen_expr(g, n->u.member.obj);
        emit_op(g, KS_OPC_GET_MEMBER);
        emit_u32(g, const_cstr(g, n->u.member.name));
        break;
    case KS_E_INDEX:
        gen_expr(g, n->u.index.obj);
        gen_expr(g, n->u.index.index);
        emit_op(g, KS_OPC_GET_INDEX);
        emit_pos(g, n->line, n->col);
        break;
    case KS_E_CALL:
        gen_call(g, n);
        break;
    case KS_E_GAMEREF: {
        const KsApiEntry *e = ks_api_entry(n->u.gameref.api_id);
        if (e && e->kind == KS_API_KIND_PROP) {
            emit_op(g, KS_OPC_GET_API);
            emit_u16(g, (uint32_t)n->u.gameref.api_id);
        } else if (e && e->kind == KS_API_KIND_FN) {
            emit_op(g, KS_OPC_API_REF);
            emit_u16(g, (uint32_t)n->u.gameref.api_id);
        } else {
            emit_op(g, KS_OPC_UNDEF); // a bare namespace is never a value
        }
        break;
    }
    case KS_E_UNARY:
        gen_expr(g, n->u.unary.expr);
        switch (n->op) {
        case '!': emit_op(g, KS_OPC_NOT); break;
        case '-': emit_op(g, KS_OPC_NEG); break;
        case '+': emit_op(g, KS_OPC_POS); break;
        case '~': emit_op(g, KS_OPC_BNOT); break;
        default: gen_fail(g, n->line, n->col, "Unsupported unary operator."); break;
        }
        break;
    case KS_E_BINARY: {
        gen_expr(g, n->u.binary.l);
        gen_expr(g, n->u.binary.r);
        int op = binop_opcode(n->op);
        if (op < 0) gen_fail(g, n->line, n->col, "Unsupported operator.");
        else emit_op(g, op);
        break;
    }
    case KS_E_LOGICAL: {
        gen_expr(g, n->u.binary.l);
        uint32_t j = emit_jump(g, n->op == KS_OP_AND ? KS_OPC_JF_KEEP : KS_OPC_JT_KEEP);
        gen_expr(g, n->u.binary.r);
        patch_here(g, j);
        break;
    }
    case KS_E_TERNARY: {
        gen_expr(g, n->u.ternary.cond);
        uint32_t jf = emit_jump(g, KS_OPC_JF);
        gen_expr(g, n->u.ternary.then);
        uint32_t jend = emit_jump(g, KS_OPC_JMP);
        patch_here(g, jf);
        gen_expr(g, n->u.ternary.els);
        patch_here(g, jend);
        break;
    }
    case KS_E_CAST:
        gen_expr(g, n->u.cast.expr);
        gen_cast(g, n->u.cast.to);
        break;
    case KS_E_KEYWORD:
        gen_expr(g, n->u.keyword.expr);
        switch (n->op) {
        case KS_KW_LENGTHOF: emit_op(g, KS_OPC_LENGTHOF); break;
        case KS_KW_NOTEMPTY: emit_op(g, KS_OPC_NOTEMPTY); break;
        case KS_KW_TONUM: emit_op(g, KS_OPC_CAST); emit_u8(g, KS_T_NUM); break;
        case KS_KW_TOSTR: emit_op(g, KS_OPC_TOSTR); break;
        default: gen_fail(g, n->line, n->col, "Unsupported keyword operator."); break;
        }
        break;
    case KS_E_HASPROP:
        gen_expr(g, n->u.member.obj);
        emit_op(g, KS_OPC_HASPROP);
        emit_u32(g, const_cstr(g, n->u.member.name));
        break;
    case KS_E_ARRAY:
        for (int i = 0; i < n->u.array.items.count; i++) gen_expr(g, n->u.array.items.items[i]);
        emit_op(g, KS_OPC_NEW_ARRAY);
        emit_u32(g, (uint32_t)n->u.array.items.count);
        break;
    case KS_E_OBJECT:
        emit_op(g, KS_OPC_NEW_OBJECT);
        for (int i = 0; i < n->u.object.values.count; i++) {
            gen_expr(g, n->u.object.values.items[i]);
            emit_op(g, KS_OPC_INIT_PROP);
            emit_u32(g, const_cstr(g, n->u.object.keys[i]));
        }
        break;
    case KS_E_ACTIONREF:
        emit_op(g, KS_OPC_ACTION_REF);
        emit_u32(g, (uint32_t)n->u.actionref.action_index);
        break;
    default:
        gen_fail(g, n->line, n->col, "Unsupported expression.");
        break;
    }
}

// ---------------------------------------------------------------------------
// Statements

static int new_hidden_local(Gen *g)
{
    if (g->locals >= 65535) {
        gen_fail(g, 0, 0, "Script too large.");
        return 0;
    }
    return g->locals++;
}

// target op= value / target++ (value == NULL: inc/dec with op KS_OP_INC/DEC)
static void gen_assign(Gen *g, const KsNode *n)
{
    const KsNode *t = n->u.assign.target;
    const KsNode *value = n->u.assign.value;
    int op = n->op;
    int compound = op != KS_OP_ASSIGN;

    // Pushes the operation applied to the current value (already on the stack).
#define APPLY_OP()                                                       \
    do {                                                                 \
        if (op == KS_OP_INC) emit_op(g, KS_OPC_INC);                     \
        else if (op == KS_OP_DEC) emit_op(g, KS_OPC_DEC);                \
        else {                                                           \
            gen_expr(g, value);                                          \
            int bop = binop_opcode(op);                                  \
            if (bop < 0) gen_fail(g, n->line, n->col, "Bad assignment."); \
            else emit_op(g, bop);                                        \
        }                                                                \
    } while (0)

    switch (t->kind) {
    case KS_E_IDENT:
        if (compound) {
            gen_get_var(g, t->u.ident.scope, t->u.ident.slot);
            APPLY_OP();
        } else {
            gen_expr(g, value);
        }
        gen_set_var(g, t->u.ident.scope, t->u.ident.slot);
        break;
    case KS_E_MEMBER: {
        uint32_t name = const_cstr(g, t->u.member.name);
        gen_expr(g, t->u.member.obj);
        if (compound) {
            emit_op(g, KS_OPC_DUP);
            emit_op(g, KS_OPC_GET_MEMBER);
            emit_u32(g, name);
            APPLY_OP();
        } else {
            gen_expr(g, value);
        }
        emit_op(g, KS_OPC_SET_MEMBER);
        emit_u32(g, name);
        break;
    }
    case KS_E_INDEX:
        gen_expr(g, t->u.index.obj);
        gen_expr(g, t->u.index.index);
        emit_op(g, KS_OPC_CHECK_INDEX);
        emit_pos(g, t->line, t->col);
        if (compound) {
            emit_op(g, KS_OPC_DUP2);
            emit_op(g, KS_OPC_GET_INDEX);
            emit_pos(g, t->line, t->col);
            APPLY_OP();
        } else {
            gen_expr(g, value);
        }
        emit_op(g, KS_OPC_SET_INDEX);
        break;
    case KS_E_GAMEREF:
        if (compound) {
            emit_op(g, KS_OPC_GET_API);
            emit_u16(g, (uint32_t)t->u.gameref.api_id);
            APPLY_OP();
        } else {
            gen_expr(g, value);
        }
        emit_op(g, KS_OPC_SET_API);
        emit_u16(g, (uint32_t)t->u.gameref.api_id);
        break;
    case KS_E_CAST: // defensive: "(T) target" folded into the target by the front end
        gen_fail(g, t->line, t->col, "Invalid assignment target.");
        break;
    default:
        gen_fail(g, t->line, t->col, "Invalid assignment target.");
        break;
    }
#undef APPLY_OP
}

static void gen_loop_body(Gen *g, Loop *l, const KsNode *body)
{
    l->outer = g->loop;
    g->loop = l;
    gen_stmt(g, body);
    g->loop = l->outer;
}

static void gen_stmt(Gen *g, const KsNode *n)
{
    if (g->failed || !n) return;
    switch (n->kind) {
    case KS_S_VARDECL:
        if (n->u.var.init) gen_expr(g, n->u.var.init);
        else gen_default(g, n->u.var.type);
        gen_set_var(g, n->u.var.scope, n->u.var.slot);
        break;
    case KS_S_IF: {
        gen_expr(g, n->u.if_.cond);
        uint32_t jf = emit_jump(g, KS_OPC_JF);
        gen_stmt(g, n->u.if_.then);
        if (n->u.if_.els) {
            uint32_t jend = emit_jump(g, KS_OPC_JMP);
            patch_here(g, jf);
            gen_stmt(g, n->u.if_.els);
            patch_here(g, jend);
        } else {
            patch_here(g, jf);
        }
        break;
    }
    case KS_S_WHILE: {
        Loop l = {0};
        uint32_t top = g->p->code_len;
        gen_expr(g, n->u.loop.cond);
        uint32_t jexit = emit_jump(g, KS_OPC_JF);
        emit_op(g, KS_OPC_LOOP);
        emit_pos(g, n->line, n->col);
        gen_loop_body(g, &l, n->u.loop.body);
        emit_jump_to(g, KS_OPC_JMP, top);
        patch_here(g, jexit);
        patch_all(g, &l.breaks, g->p->code_len);
        patch_all(g, &l.continues, top);
        break;
    }
    case KS_S_FOR: {
        Loop l = {0};
        if (n->u.loop.init) gen_stmt(g, n->u.loop.init);
        uint32_t top = g->p->code_len;
        uint32_t jexit = 0;
        int has_cond = n->u.loop.cond != NULL;
        if (has_cond) {
            gen_expr(g, n->u.loop.cond);
            jexit = emit_jump(g, KS_OPC_JF);
        }
        emit_op(g, KS_OPC_LOOP);
        emit_pos(g, n->line, n->col);
        gen_loop_body(g, &l, n->u.loop.body);
        uint32_t step = g->p->code_len;
        if (n->u.loop.step) gen_stmt(g, n->u.loop.step);
        emit_jump_to(g, KS_OPC_JMP, top);
        if (has_cond) patch_here(g, jexit);
        patch_all(g, &l.breaks, g->p->code_len);
        patch_all(g, &l.continues, step);
        break;
    }
    case KS_S_FOREACH: {
        Loop l = {0};
        int arr = new_hidden_local(g), idx = new_hidden_local(g);
        gen_expr(g, n->u.foreach.iterable);
        emit_op(g, KS_OPC_ENTRIES);
        emit_op(g, KS_OPC_SET_LOCAL);
        emit_u16(g, (uint32_t)arr);
        emit_op(g, KS_OPC_NUM);
        emit_u32(g, const_num(g, 0));
        emit_op(g, KS_OPC_SET_LOCAL);
        emit_u16(g, (uint32_t)idx);
        uint32_t top = g->p->code_len;
        emit_op(g, KS_OPC_FOREACH);
        emit_u16(g, (uint32_t)arr);
        emit_u16(g, (uint32_t)idx);
        emit_u16(g, (uint32_t)n->u.foreach.slot);
        uint32_t jexit = g->p->code_len;
        emit_u32(g, 0);
        emit_op(g, KS_OPC_LOOP);
        emit_pos(g, n->line, n->col);
        gen_loop_body(g, &l, n->u.foreach.body);
        emit_jump_to(g, KS_OPC_JMP, top);
        patch_here(g, jexit);
        patch_all(g, &l.breaks, g->p->code_len);
        patch_all(g, &l.continues, top);
        break;
    }
    case KS_S_RETURN:
        if (n->u.ret.expr) {
            gen_expr(g, n->u.ret.expr);
            emit_op(g, KS_OPC_RETURN);
        } else {
            emit_op(g, KS_OPC_RETURN_UNDEF);
        }
        break;
    case KS_S_BREAK:
    case KS_S_CONTINUE:
        if (!g->loop) {
            gen_fail(g, n->line, n->col, n->kind == KS_S_BREAK ? "\"break\" outside of a loop." : "\"continue\" outside of a loop.");
            break;
        }
        patch_add(n->kind == KS_S_BREAK ? &g->loop->breaks : &g->loop->continues, emit_jump(g, KS_OPC_JMP));
        break;
    case KS_S_ADDTO:
        gen_expr(g, n->u.addto.target);
        gen_expr(g, n->u.addto.value);
        emit_op(g, KS_OPC_ADDTO);
        // "<expr>.push is not a function": v7 lowered an explicit cast to A(x, f), which V8
        // prints as "A(...)".
        if (n->u.addto.target->kind == KS_E_CAST && !n->u.addto.target->u.cast.implicit)
            emit_u32(g, const_cstr(g, "A(...)"));
        else if (n->u.addto.target->kind == KS_E_CAST)
            emit_u32(g, const_span(g, n->u.addto.target->u.cast.expr));
        else
            emit_u32(g, const_span(g, n->u.addto.target));
        break;
    case KS_S_REMOVE: {
        const KsNode *t = n->u.remove.target;
        if (t->kind == KS_E_INDEX) {
            gen_expr(g, t->u.index.obj);
            gen_expr(g, t->u.index.index);
            emit_op(g, KS_OPC_REMOVE_INDEX);
        } else if (t->kind == KS_E_MEMBER) {
            gen_expr(g, t->u.member.obj);
            emit_op(g, KS_OPC_REMOVE_MEMBER);
            emit_u32(g, const_cstr(g, t->u.member.name));
        } else {
            gen_fail(g, n->line, n->col, "\"remove\" needs an array element or object property.");
        }
        break;
    }
    case KS_S_ASSIGN:
    case KS_S_INCDEC:
        gen_assign(g, n);
        break;
    case KS_S_EXPR:
        gen_expr(g, n->u.expr.expr);
        emit_op(g, KS_OPC_POP);
        break;
    case KS_S_BLOCK:
        for (int i = 0; i < n->u.block.count; i++) gen_stmt(g, n->u.block.items[i]);
        break;
    case KS_S_EMPTY:
        break;
    default:
        gen_fail(g, n->line, n->col, "Unsupported statement.");
        break;
    }
}

// ---------------------------------------------------------------------------
// Program

static char *dup_str(const char *s)
{
    size_t n = s ? strlen(s) : 0;
    char *d = xrealloc(NULL, n + 1);
    if (n) memcpy(d, s, n);
    d[n] = 0;
    return d;
}

void ks_program_free(KsProgram *p)
{
    if (!p) return;
    free(p->code);
    free(p->nums);
    for (uint32_t i = 0; i < p->str_count; i++) free(p->strs[i].data);
    free(p->strs);
    for (uint32_t i = 0; i < p->action_count; i++) free(p->actions[i].name);
    free(p->actions);
    free(p);
}

KsProgram *ks_codegen(const KsAstProgram *ast, KsSide side, KsDiag *err)
{
    KsProgram *p = xrealloc(NULL, sizeof *p);
    memset(p, 0, sizeof *p);
    p->side = side;
    Gen g = {0};
    g.p = p;
    g.ast = ast;
    g.err = err;

    p->global_count = (uint32_t)ast->global_count;
    p->action_count = (uint32_t)ast->action_count;
    p->actions = xrealloc(NULL, (size_t)(ast->action_count ? ast->action_count : 1) * sizeof(KsActionInfo));
    memset(p->actions, 0, (size_t)(ast->action_count ? ast->action_count : 1) * sizeof(KsActionInfo));

    // Global initialisers: a pseudo-action run once by ks_script_new.
    p->init_code = p->code_len;
    g.locals = 0;
    for (int i = 0; i < ast->global_count && !g.failed; i++) gen_stmt(&g, ast->globals[i]);
    emit_op(&g, KS_OPC_RETURN_UNDEF);
    p->init_locals = (uint16_t)g.locals;

    for (int i = 0; i < ast->action_count && !g.failed; i++) {
        const KsAstAction *a = &ast->actions[i];
        KsActionInfo *info = &p->actions[i];
        info->name = dup_str(a->name);
        info->params = (uint16_t)a->param_count;
        info->is_public = (uint8_t)(a->is_public != 0);
        info->code = p->code_len;
        if (a->local_count > 65000) {
            gen_fail(&g, a->line, a->col, "Script too large.");
            break;
        }
        g.locals = a->local_count;
        g.loop = NULL;
        gen_stmt(&g, a->body);
        // Falling off the end: typed actions return their type's default (1.5 rule 11).
        if (a->ret_type.base == KS_T_VOID && a->ret_type.dims == 0) {
            emit_op(&g, KS_OPC_RETURN_UNDEF);
        } else {
            gen_default(&g, a->ret_type);
            emit_op(&g, KS_OPC_RETURN);
        }
        info->locals = (uint16_t)g.locals;
    }
    free(g.str_index);
    if (!g.failed && p->code_len > 0x7FFFFFFF) gen_fail(&g, 0, 0, "Script too large.");
    if (g.failed) {
        for (uint32_t i = 0; i < p->action_count; i++)
            if (!p->actions[i].name) p->actions[i].name = dup_str("");
        ks_program_free(p);
        return NULL;
    }
    return p;
}

KsProgram *ks_compile(const char *source, size_t len, KsSide side, KsDiag *err, KsDiag *warnings,
                      int max_warnings, int *warning_count)
{
    KsAstProgram *ast = ks_front(source, len, side, err, warnings, max_warnings, warning_count);
    if (!ast) return NULL;
    KsProgram *p = ks_codegen(ast, side, err);
    ks_ast_free(ast);
    return p;
}
