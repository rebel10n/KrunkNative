// KrunkScript checker (doc/v7/krunkscript.md 1.5-1.9, 1.15, 4.1): scopes and
// slots, static types, GAME.* path resolution, hook signatures, and the front
// end entry points (ks_front, ks_validate).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ks_api.h"
#include "ks_front_internal.h"

// ---------------------------------------------------------------------------
// Types

static const KsType T_NUM = {KS_T_NUM, 0}, T_STR = {KS_T_STR, 0}, T_BOOL = {KS_T_BOOL, 0},
                    T_OBJ = {KS_T_OBJ, 0}, T_ANY = {KS_T_ANY, 0}, T_VOID = {KS_T_VOID, 0},
                    T_ACTION = {KS_T_ACTION, 0};

int ks_type_eq(KsType a, KsType b) { return a.base == b.base && a.dims == b.dims; }

const char *ks_type_name(KsType t, char *buf, size_t n) {
    static const char *const names[] = {"num", "str", "bool", "obj", "any", "void", "action"};
    size_t o = (size_t) snprintf(buf, n, "%s", t.base < 7 ? names[t.base] : "?");
    for (int i = 0; i < t.dims && o + 2 < n; i++) {
        buf[o++] = '[';
        buf[o++] = ']';
        buf[o] = 0;
    }
    return buf;
}

static int is_any(KsType t) { return t.base == KS_T_ANY && t.dims == 0; }
static int is_num(KsType t) { return t.base == KS_T_NUM && t.dims == 0; }
static int is_str(KsType t) { return t.base == KS_T_STR && t.dims == 0; }
static int is_bool(KsType t) { return t.base == KS_T_BOOL && t.dims == 0; }
static int is_obj(KsType t) { return t.base == KS_T_OBJ && t.dims == 0; }
static int is_array(KsType t) { return t.dims > 0; }

// ---------------------------------------------------------------------------
// Hooks (4.1)

typedef struct { const char *name; int params; } Hook;

static const Hook client_hooks[] = {
    {"start", 0}, {"update", 1}, {"render", 1}, {"onPlayerSpawn", 1}, {"onPlayerDeath", 2},
    {"onPlayerUpdate", 3}, {"onKeyPress", 2}, {"onKeyUp", 2}, {"onKeyHeld", 2},
    {"onControllerPress", 2}, {"onControllerUp", 2}, {"onControllerHeld", 2},
    {"onMouseClick", 3}, {"onMouseUp", 3}, {"onMouseScroll", 1}, {"onDIVClicked", 1},
    {"onNetworkMessage", 2},
};

static const Hook server_hooks[] = {
    {"start", 0}, {"update", 1}, {"onPlayerSpawn", 1}, {"onPlayerDeath", 2},
    {"onPlayerDamage", 3}, {"onPlayerUpdate", 3}, {"onCustomTrigger", 3},
    {"onNetworkMessage", 3}, {"onChatMessage", 2}, {"onPlayerLeave", 1}, {"onGameEnd", 0},
    {"onAdFinished", 2}, {"onServerClosed", 0},
    {"shouldTrigger", 3}, // v10.0.2: trigger condition 12 "Custom Action" (playerId, triggerSid, param) -> bool
};

int ks_hook_param_count(KsSide side, const char *name) {
    const Hook *h = side == KS_SIDE_SERVER ? server_hooks : client_hooks;
    size_t n = side == KS_SIDE_SERVER ? sizeof server_hooks / sizeof *h : sizeof client_hooks / sizeof *h;
    for (size_t i = 0; i < n; i++)
        if (strcmp(h[i].name, name) == 0) return h[i].params;
    return -1;
}

// ---------------------------------------------------------------------------
// Checker state

typedef struct {
    const char *name;
    KsType type;
    int slot;
    int depth;
} Local;

typedef struct {
    KsFront *f;
    KsAstProgram *prog;
    Local *locals;
    int nlocals, cap;
    int depth;
    int visible_globals;   // globals visible to the code being checked
    KsAstAction *action;   // NULL while checking global initialisers
    int loop_depth;
    int next_slot;
    int expr_depth;
} Ck;

#define FAILED (c->f->failed)
#define ERR(node, ...) ks_front_error(c->f, (node)->line, (node)->col, __VA_ARGS__)

static int find_global(Ck *c, const char *name, int limit) {
    for (int i = limit - 1; i >= 0; i--)
        if (strcmp(c->prog->globals[i]->u.var.name, name) == 0) return i;
    return -1;
}

static Local *find_local(Ck *c, const char *name) {
    for (int i = c->nlocals - 1; i >= 0; i--)
        if (strcmp(c->locals[i].name, name) == 0) return &c->locals[i];
    return NULL;
}

static void push_scope(Ck *c) { c->depth++; }
static void pop_scope(Ck *c) {
    while (c->nlocals > 0 && c->locals[c->nlocals - 1].depth == c->depth) c->nlocals--;
    c->depth--;
}

// Declare a local in the current scope; returns its slot or -1 (error set).
static int declare_local(Ck *c, const char *name, KsType type, int line, int col) {
    for (int i = c->nlocals - 1; i >= 0 && c->locals[i].depth == c->depth; i--) {
        if (strcmp(c->locals[i].name, name) == 0) {
            ks_front_error(c->f, line, col, "Variable \"%s\" is already defined.", name);
            return -1;
        }
    }
    if (c->nlocals == c->cap) {
        int cap = c->cap ? c->cap * 2 : 32;
        Local *l = realloc(c->locals, (size_t) cap * sizeof *l);
        if (!l) {
            ks_front_error(c->f, line, col, "Out of memory.");
            return -1;
        }
        c->locals = l;
        c->cap = cap;
    }
    Local *l = &c->locals[c->nlocals++];
    l->name = name;
    l->type = type;
    l->slot = c->next_slot++;
    l->depth = c->depth;
    return l->slot;
}

static KsNode *new_node(Ck *c, int kind, const KsNode *at) {
    KsNode *n = ks_arena_alloc(c->f->arena, sizeof *n);
    if (!n) {
        ks_front_error(c->f, at->line, at->col, "Out of memory.");
        return NULL;
    }
    n->kind = (uint8_t) kind;
    n->line = at->line;
    n->col = at->col;
    n->start = at->start;
    n->end = at->end;
    return n;
}

// ---------------------------------------------------------------------------
// GAME.* paths (rule 9, 10)

#define GP_NONE (-3)
#define GP_ROOT (-2)

static void api_path_text(int parent, const char *name, char *buf, size_t n) {
    if (parent < 0) snprintf(buf, n, "GAME.%s", name);
    else snprintf(buf, n, "GAME.%s.%s", ks_api_entry(parent)->path, name);
}

// Resolve an IDENT/MEMBER chain rooted at GAME, UTILS or Math through namespaces.
// Returns an api id, GP_ROOT for a bare GAME, or GP_NONE (not a GAME path, or an
// error was raised for an unknown member).
static int game_path(Ck *c, KsNode *n) {
    if (n->parens) return GP_NONE;
    if (n->kind == KS_E_IDENT) {
        const char *s = n->u.ident.name;
        if (strcmp(s, "GAME") == 0) return GP_ROOT;
        if (strcmp(s, "UTILS") == 0) return KS_API_UTILS;
        if (strcmp(s, "Math") == 0) return KS_API_UTILS_MATH;
        return GP_NONE;
    }
    if (n->kind != KS_E_MEMBER) return GP_NONE;
    int p = game_path(c, n->u.member.obj);
    if (p == GP_NONE) return GP_NONE;
    if (p >= 0 && ks_api_entry(p)->kind != KS_API_KIND_NS) return GP_NONE;
    int parent = p == GP_ROOT ? -1 : p;
    int id = ks_api_child(parent, n->u.member.name);
    if (id < 0) {
        char path[160];
        api_path_text(parent, n->u.member.name, path, sizeof path);
        ks_front_error(c->f, n->u.member.name_line, n->u.member.name_col, "Unknown member \"%s\".", path);
        return GP_NONE;
    }
    return id;
}

// Rewrite *pn into a GAMEREF for api `id`; type: FN = action, PROP = its type.
static int make_gameref(Ck *c, KsNode **pn, int id) {
    KsNode *g = new_node(c, KS_E_GAMEREF, *pn);
    if (!g) return 0;
    g->u.gameref.api_id = id;
    const KsApiEntry *e = ks_api_entry(id);
    g->type = e->kind == KS_API_KIND_FN ? T_ACTION : e->ret;
    *pn = g;
    return 1;
}

static void not_a_value(Ck *c, const KsNode *at, int id) {
    if (id == GP_ROOT) ERR(at, "\"GAME\" is not a value.");
    else ERR(at, "\"GAME.%s\" is not a value.", ks_api_entry(id)->path);
}

// ---------------------------------------------------------------------------
// Expressions

static KsType check_expr(Ck *c, KsNode **pn);

static int find_action(const KsAstProgram *p, const char *name) {
    for (int i = 0; i < p->action_count; i++)
        if (strcmp(p->actions[i].name, name) == 0) return i;
    return -1;
}

int ks_ast_find_action(const KsAstProgram *p, const char *name) { return find_action(p, name); }

// A value-producing expression: void (an action without a return type) is an error.
static KsType check_value(Ck *c, KsNode **pn) {
    KsType t = check_expr(c, pn);
    if (FAILED) return T_ANY;
    if (t.base == KS_T_VOID) {
        KsNode *n = *pn;
        const char *name = "?";
        if (n->kind == KS_E_CALL && n->u.call.call_kind == KS_CALL_ACTION)
            name = c->prog->actions[n->u.call.action_index].name;
        ERR(n, "Action \"%s\" does not return a value.", name);
        return T_ANY;
    }
    return t;
}

// Make the checked expression *pn (type t) fit a slot of type `to` (rule 1).
static int coerce(Ck *c, KsNode **pn, KsType to) {
    KsNode *n = *pn;
    KsType t = n->type;
    if (ks_type_eq(t, to) || is_any(to)) return 1;
    if (is_any(t)) {
        if (to.base == KS_T_ACTION) return 1;   // no cast to action: checked when called
        KsNode *cast = new_node(c, KS_E_CAST, n);
        if (!cast) return 0;
        cast->u.cast.to = to;
        cast->u.cast.expr = n;
        cast->u.cast.implicit = 1;
        cast->type = to;
        *pn = cast;
        char tb[32];
        ks_front_warning(c->f, n->line, n->col, "Implicit cast of dynamic value to %s.", ks_type_name(to, tb, sizeof tb));
        return 1;
    }
    char a[32], b[32];
    ERR(n, "Cannot assign %s to %s.", ks_type_name(t, a, sizeof a), ks_type_name(to, b, sizeof b));
    return 0;
}

static const char *binop_text(int op) {
    static const char *const t[] = {"+", "-", "*", "/", "%", "**", "<<", ">>", ">>>", "&", "|", "^",
                                    "==", "!=", "<", "<=", ">", ">=", "&&", "||"};
    return op <= KS_OP_OR ? t[op] : "?";
}

// Type of `l op r` for the arithmetic/comparison operators; error at `at`.
static KsType binary_type(Ck *c, const KsNode *at, int op, KsType l, KsType r) {
    char a[32], b[32];
    switch (op) {
    case KS_OP_ADD:
        if (is_any(l) || is_any(r)) return T_ANY;
        if (is_num(l) && is_num(r)) return T_NUM;
        if (is_str(l) && is_str(r)) return T_STR;
        ERR(at, "Cannot add %s and %s; use (str) or toStr.", ks_type_name(l, a, sizeof a), ks_type_name(r, b, sizeof b));
        return T_ANY;
    case KS_OP_EQ:
    case KS_OP_NE:
        if (is_any(l) || is_any(r) || ks_type_eq(l, r)) return T_BOOL;
        ERR(at, "Cannot compare %s and %s.", ks_type_name(l, a, sizeof a), ks_type_name(r, b, sizeof b));
        return T_BOOL;
    case KS_OP_LT:
    case KS_OP_LE:
    case KS_OP_GT:
    case KS_OP_GE:
        if (is_any(l) || is_any(r) || (is_num(l) && is_num(r)) || (is_str(l) && is_str(r))) return T_BOOL;
        ERR(at, "Cannot compare %s and %s.", ks_type_name(l, a, sizeof a), ks_type_name(r, b, sizeof b));
        return T_BOOL;
    default:
        if ((is_num(l) || is_any(l)) && (is_num(r) || is_any(r))) return T_NUM;
        ERR(at, "Operator \"%s\" needs num operands.", binop_text(op));
        return T_NUM;
    }
}

// obj.name read: obj must be obj or any (rule 4).
static int check_member_base(Ck *c, KsNode *n) {
    KsType t = check_value(c, &n->u.member.obj);
    if (FAILED) return 0;
    if (!is_obj(t) && !is_any(t)) {
        char a[32];
        ERR(n->u.member.obj, "Cannot read property of %s.", ks_type_name(t, a, sizeof a));
        return 0;
    }
    return 1;
}

static KsType check_call(Ck *c, KsNode *n) {
    KsNode *callee = n->u.call.callee;
    KsNodeList *args = &n->u.call.args;
    KsType result = T_ANY;

    if (callee->kind == KS_E_IDENT && !callee->parens) {
        const char *name = callee->u.ident.name;
        Local *l = find_local(c, name);
        int g = l ? -1 : find_global(c, name, c->visible_globals);
        if (l || g >= 0) {
            check_expr(c, &n->u.call.callee);
            if (FAILED) return T_ANY;
            KsType t = n->u.call.callee->type;
            if (t.base != KS_T_ACTION && !is_any(t)) {
                char a[32];
                ERR(callee, "Cannot call %s.", ks_type_name(t, a, sizeof a));
                return T_ANY;
            }
            n->u.call.call_kind = KS_CALL_ACTION_VALUE;
        } else if (ks_is_predeclared_name(name)) {
            not_a_value(c, callee, game_path(c, callee));
            return T_ANY;
        } else {
            int ai = find_action(c->prog, name);
            if (ai < 0) {
                ERR(callee, "Action \"%s\" is not defined.", name);
                return T_ANY;
            }
            KsAstAction *a = &c->prog->actions[ai];
            if (args->count != a->param_count) {
                ERR(callee, "Action \"%s\" expects %d argument%s.", name, a->param_count, a->param_count == 1 ? "" : "s");
                return T_ANY;
            }
            for (int i = 0; i < args->count; i++) {
                check_value(c, &args->items[i]);
                if (FAILED || !coerce(c, &args->items[i], a->params[i].type)) return T_ANY;
            }
            n->u.call.call_kind = KS_CALL_ACTION;
            n->u.call.action_index = ai;
            callee->type = T_ACTION;
            return a->ret_type;
        }
    } else if (callee->kind == KS_E_MEMBER && !callee->parens) {
        int id = game_path(c, callee);
        if (FAILED) return T_ANY;
        if (id >= 0 || id == GP_ROOT) {
            const KsApiEntry *e = id >= 0 ? ks_api_entry(id) : NULL;
            if (!e || e->kind == KS_API_KIND_NS) {
                not_a_value(c, callee, id);
                return T_ANY;
            }
            if (!make_gameref(c, &n->u.call.callee, id)) return T_ANY;
            if (e->kind == KS_API_KIND_FN) {
                n->u.call.call_kind = KS_CALL_GAME_API;
                n->u.call.api_id = id;
                result = e->ret;
            } else {
                if (!is_any(e->ret)) {
                    char a[32];
                    ERR(callee, "Cannot call %s.", ks_type_name(e->ret, a, sizeof a));
                    return T_ANY;
                }
                n->u.call.call_kind = KS_CALL_ACTION_VALUE;
            }
        } else {
            if (!check_member_base(c, callee)) return T_ANY;
            callee->type = T_ANY;
            n->u.call.call_kind = KS_CALL_METHOD;
        }
    } else {
        KsType t = check_value(c, &n->u.call.callee);
        if (FAILED) return T_ANY;
        if (t.base != KS_T_ACTION && !is_any(t)) {
            char a[32];
            ERR(callee, "Cannot call %s.", ks_type_name(t, a, sizeof a));
            return T_ANY;
        }
        n->u.call.call_kind = KS_CALL_ACTION_VALUE;
    }
    // untyped arguments (rule 6): any value
    for (int i = 0; i < args->count; i++) {
        check_value(c, &args->items[i]);
        if (FAILED) return T_ANY;
    }
    return result;
}

static KsType check_expr_inner(Ck *c, KsNode **pn) {
    KsNode *n = *pn;
    char a[32], b[32];
    switch (n->kind) {
    case KS_E_NUM: return T_NUM;
    case KS_E_STR: return T_STR;
    case KS_E_BOOL: return T_BOOL;
    case KS_E_GAMEREF: return n->type;
    case KS_E_ACTIONREF: return T_ACTION;

    case KS_E_IDENT: {
        const char *name = n->u.ident.name;
        Local *l = find_local(c, name);
        if (l) {
            n->u.ident.scope = KS_SCOPE_LOCAL;
            n->u.ident.slot = l->slot;
            return l->type;
        }
        int g = find_global(c, name, c->visible_globals);
        if (g >= 0) {
            n->u.ident.scope = KS_SCOPE_GLOBAL;
            n->u.ident.slot = g;
            return c->prog->globals[g]->u.var.type;
        }
        if (ks_is_predeclared_name(name)) {
            not_a_value(c, n, game_path(c, n));
            return T_ANY;
        }
        int ai = find_action(c->prog, name);
        if (ai >= 0) {
            KsNode *r = new_node(c, KS_E_ACTIONREF, n);
            if (!r) return T_ANY;
            r->u.actionref.action_index = ai;
            r->parens = n->parens;
            *pn = r;
            return T_ACTION;
        }
        ERR(n, "Variable \"%s\" is not defined.", name);
        return T_ANY;
    }

    case KS_E_MEMBER: {
        int id = game_path(c, n);
        if (FAILED) return T_ANY;
        if (id == GP_ROOT || (id >= 0 && ks_api_entry(id)->kind == KS_API_KIND_NS)) {
            not_a_value(c, n, id);
            return T_ANY;
        }
        if (id >= 0) {
            int parens = n->parens;
            if (!make_gameref(c, pn, id)) return T_ANY;
            (*pn)->parens = (uint8_t) parens;
            return (*pn)->type;
        }
        if (!check_member_base(c, n)) return T_ANY;
        return T_ANY;
    }

    case KS_E_INDEX: {
        KsType t = check_value(c, &n->u.index.obj);
        if (FAILED) return T_ANY;
        KsType it = check_value(c, &n->u.index.index);
        if (FAILED) return T_ANY;
        KsType r;
        if (is_array(t)) r = (KsType) {t.base, (uint8_t) (t.dims - 1)};
        else if (is_str(t)) r = T_STR;
        else if (is_any(t)) r = T_ANY;
        else {
            ERR(n->u.index.obj, "Cannot index %s.", ks_type_name(t, a, sizeof a));
            return T_ANY;
        }
        if (!is_num(it) && !is_any(it)) {
            ERR(n->u.index.index, "Array index must be a number.");
            return T_ANY;
        }
        return r;
    }

    case KS_E_CALL:
        return check_call(c, n);

    case KS_E_UNARY: {
        KsType t = check_value(c, &n->u.unary.expr);
        if (FAILED) return T_ANY;
        if (n->op == '!') return T_BOOL;
        if (!is_num(t) && !is_any(t)) {
            ERR(n, "Operator \"%c\" needs num operands.", n->op);
            return T_ANY;
        }
        return T_NUM;
    }

    case KS_E_BINARY: {
        KsType l = check_value(c, &n->u.binary.l);
        if (FAILED) return T_ANY;
        KsType r = check_value(c, &n->u.binary.r);
        if (FAILED) return T_ANY;
        return binary_type(c, n, n->op, l, r);
    }

    case KS_E_LOGICAL: {
        KsType l = check_value(c, &n->u.binary.l);
        if (FAILED) return T_ANY;
        KsType r = check_value(c, &n->u.binary.r);
        if (FAILED) return T_ANY;
        return is_bool(l) && is_bool(r) ? T_BOOL : T_ANY;
    }

    case KS_E_TERNARY: {
        check_value(c, &n->u.ternary.cond);
        if (FAILED) return T_ANY;
        KsType t = check_value(c, &n->u.ternary.then);
        if (FAILED) return T_ANY;
        KsType e = check_value(c, &n->u.ternary.els);
        if (FAILED) return T_ANY;
        return ks_type_eq(t, e) ? t : T_ANY;
    }

    case KS_E_CAST:
        check_value(c, &n->u.cast.expr);
        return n->u.cast.to;

    case KS_E_KEYWORD:
        check_value(c, &n->u.keyword.expr);
        switch (n->op) {
        case KS_KW_LENGTHOF: return T_NUM;
        case KS_KW_NOTEMPTY: return T_BOOL;
        case KS_KW_TONUM: return T_NUM;
        default: return T_STR;
        }

    case KS_E_HASPROP: {
        // reuse the member checks on a temporary MEMBER node
        KsNode tmp = *n;
        tmp.kind = KS_E_MEMBER;
        tmp.parens = 0;
        int id = game_path(c, &tmp);
        if (FAILED) return T_BOOL;
        if (id >= 0 || id == GP_ROOT) {
            ERR(n, "\"hasProp\" needs an object property.");
            return T_BOOL;
        }
        if (!check_member_base(c, &tmp)) return T_BOOL;
        n->u.member.obj = tmp.u.member.obj;
        return T_BOOL;
    }

    case KS_E_ARRAY: {
        KsType elem = n->u.array.elem_type;
        for (int i = 0; i < n->u.array.items.count; i++) {
            check_value(c, &n->u.array.items.items[i]);
            if (FAILED || !coerce(c, &n->u.array.items.items[i], elem)) return T_ANY;
        }
        return (KsType) {elem.base, (uint8_t) (elem.dims + 1)};
    }

    case KS_E_OBJECT:
        for (int i = 0; i < n->u.object.values.count; i++) {
            check_value(c, &n->u.object.values.items[i]);
            if (FAILED) return T_OBJ;
        }
        return T_OBJ;

    default:
        (void) b;
        ERR(n, "Unexpected expression.");
        return T_ANY;
    }
}

static KsType check_expr(Ck *c, KsNode **pn) {
    if (++c->expr_depth > KS_MAX_EXPR_DEPTH) {
        ERR(*pn, KS_NESTING_MESSAGE);
        c->expr_depth--;
        return T_ANY;
    }
    KsType t = check_expr_inner(c, pn);
    c->expr_depth--;
    if (!FAILED) (*pn)->type = t;
    return t;
}

// ---------------------------------------------------------------------------
// Statements

static void check_stmt(Ck *c, KsNode *n);

static void check_body(Ck *c, KsNode *n) {
    push_scope(c);
    check_stmt(c, n);
    pop_scope(c);
}

static void check_vardecl(Ck *c, KsNode *n, int global_index) {
    if (n->u.var.init) {
        check_value(c, &n->u.var.init);
        if (FAILED || !coerce(c, &n->u.var.init, n->u.var.type)) return;
    }
    if (global_index >= 0) {
        if (find_global(c, n->u.var.name, global_index) >= 0 || find_action(c->prog, n->u.var.name) >= 0) {
            ERR(n, "Variable \"%s\" is already defined.", n->u.var.name);
            return;
        }
        n->u.var.scope = KS_SCOPE_GLOBAL;
        n->u.var.slot = global_index;
        return;
    }
    n->u.var.scope = KS_SCOPE_LOCAL;
    n->u.var.slot = declare_local(c, n->u.var.name, n->u.var.type, n->line, n->col);
}

// Static type of an assignment target (rule 7 "++ -- op="), resolving it in place.
static int check_target(Ck *c, KsNode *stmt, KsType *out) {
    KsNode **pt = &stmt->u.assign.target;
    KsNode *t = *pt;
    KsType lt;
    if (t->kind == KS_E_IDENT) {
        const char *name = t->u.ident.name;
        if (!find_local(c, name) && find_global(c, name, c->visible_globals) < 0) {
            if (ks_is_predeclared_name(name)) {
                ERR(t, "Cannot assign to \"%s\".", name);
                return 0;
            }
            ERR(t, "Variable \"%s\" is not defined.", name);
            return 0;
        }
        lt = check_expr(c, pt);
    } else if (t->kind == KS_E_MEMBER) {
        int id = game_path(c, t);
        if (FAILED) return 0;
        if (id >= 0 && ks_api_entry(id)->kind == KS_API_KIND_PROP) {
            lt = check_expr(c, pt);
        } else if (id >= 0 || id == GP_ROOT) {
            ERR(t, "Cannot assign to \"GAME%s%s\".", id >= 0 ? "." : "", id >= 0 ? ks_api_entry(id)->path : "");
            return 0;
        } else {
            lt = check_expr(c, pt);
        }
    } else {
        lt = check_expr(c, pt);
        if (!FAILED && is_str(t->u.index.obj->type)) {
            ERR(t, "Cannot assign to a character of a str.");
            return 0;
        }
    }
    if (FAILED) return 0;
    if (stmt->u.assign.has_cast) {
        if (!is_any(lt) && !ks_type_eq(lt, stmt->u.assign.cast)) {
            char a[32], b[32];
            ERR(t, "Cannot assign %s to %s.", ks_type_name(stmt->u.assign.cast, a, sizeof a), ks_type_name(lt, b, sizeof b));
            return 0;
        }
        lt = stmt->u.assign.cast;
    }
    *out = lt;
    return 1;
}

static void check_assign(Ck *c, KsNode *n) {
    KsType tt;
    if (!check_target(c, n, &tt)) return;
    if (n->kind == KS_S_INCDEC) {
        const char *op = n->op == KS_OP_INC ? "++" : "--";
        if (is_any(tt)) ERR(n, "Cannot apply \"%s\" to a dynamic value; add a cast.", op);
        else if (!is_num(tt)) ERR(n, "Operator \"%s\" needs num operands.", op);
        return;
    }
    KsType vt = check_value(c, &n->u.assign.value);
    if (FAILED) return;
    if (n->op == KS_OP_ASSIGN) {
        coerce(c, &n->u.assign.value, tt);
        return;
    }
    char opname[8];
    snprintf(opname, sizeof opname, "%s=", binop_text(n->op));
    if (is_any(tt)) {
        ERR(n, "Cannot apply \"%s\" to a dynamic value; add a cast.", opname);
        return;
    }
    char a[32], b[32];
    if (n->op == KS_OP_ADD) {
        if ((is_num(tt) || is_str(tt)) && (ks_type_eq(tt, vt) || is_any(vt))) return;
        if (is_num(tt) || is_str(tt))
            ERR(n->u.assign.value, "Cannot add %s and %s; use (str) or toStr.", ks_type_name(tt, a, sizeof a), ks_type_name(vt, b, sizeof b));
        else
            ERR(n, "Operator \"%s\" needs num operands.", opname);
        return;
    }
    if (!is_num(tt) || (!is_num(vt) && !is_any(vt))) ERR(n, "Operator \"%s\" needs num operands.", opname);
}

static void check_stmt(Ck *c, KsNode *n) {
    if (FAILED) return;
    switch (n->kind) {
    case KS_S_EMPTY:
    case KS_S_BREAK:
    case KS_S_CONTINUE:
        if (n->kind != KS_S_EMPTY && c->loop_depth == 0)
            ERR(n, "\"%s\" outside of a loop.", n->kind == KS_S_BREAK ? "break" : "continue");
        return;
    case KS_S_BLOCK:
        push_scope(c);
        for (int i = 0; i < n->u.block.count && !FAILED; i++) check_stmt(c, n->u.block.items[i]);
        pop_scope(c);
        return;
    case KS_S_VARDECL:
        check_vardecl(c, n, -1);
        return;
    case KS_S_IF:
        check_value(c, &n->u.if_.cond);
        if (FAILED) return;
        check_body(c, n->u.if_.then);
        if (n->u.if_.els) check_body(c, n->u.if_.els);
        return;
    case KS_S_WHILE:
        check_value(c, &n->u.loop.cond);
        if (FAILED) return;
        c->loop_depth++;
        check_body(c, n->u.loop.body);
        c->loop_depth--;
        return;
    case KS_S_FOR:
        push_scope(c);
        if (n->u.loop.init) check_stmt(c, n->u.loop.init);
        if (!FAILED && n->u.loop.cond) check_value(c, &n->u.loop.cond);
        if (!FAILED && n->u.loop.step) check_stmt(c, n->u.loop.step);
        if (!FAILED) {
            c->loop_depth++;
            check_body(c, n->u.loop.body);
            c->loop_depth--;
        }
        pop_scope(c);
        return;
    case KS_S_FOREACH: {
        check_value(c, &n->u.foreach.iterable);
        if (FAILED) return;
        if (!is_obj(n->u.foreach.var_type)) {
            char a[32];
            ERR(n, "Cannot assign obj to %s.", ks_type_name(n->u.foreach.var_type, a, sizeof a));
            return;
        }
        push_scope(c);
        n->u.foreach.slot = declare_local(c, n->u.foreach.var_name, n->u.foreach.var_type, n->line, n->col);
        if (!FAILED) {
            c->loop_depth++;
            check_body(c, n->u.foreach.body);
            c->loop_depth--;
        }
        pop_scope(c);
        return;
    }
    case KS_S_RETURN: {
        KsType rt = c->action ? c->action->ret_type : T_VOID;
        if (rt.base == KS_T_VOID) {
            if (n->u.ret.expr) ERR(n->u.ret.expr, "Unexpected return value.");
            return;
        }
        if (!n->u.ret.expr) {
            ERR(n, "Missing return value.");
            return;
        }
        check_value(c, &n->u.ret.expr);
        if (!FAILED) coerce(c, &n->u.ret.expr, rt);
        return;
    }
    case KS_S_ADDTO: {
        KsType t = check_value(c, &n->u.addto.target);
        if (FAILED) return;
        if (!is_array(t)) {
            ERR(n->u.addto.target, "\"addTo\" needs an array.");
            return;
        }
        check_value(c, &n->u.addto.value);
        if (!FAILED) coerce(c, &n->u.addto.value, (KsType) {t.base, (uint8_t) (t.dims - 1)});
        return;
    }
    case KS_S_REMOVE: {
        KsNode *t = n->u.remove.target;
        if (t->kind == KS_E_MEMBER) {
            int id = game_path(c, t);
            if (FAILED) return;
            if (id >= 0 || id == GP_ROOT) {
                ERR(t, "\"remove\" needs an array element or object property.");
                return;
            }
            if (check_member_base(c, t)) t->type = T_ANY;
            return;
        }
        check_expr(c, &n->u.remove.target);
        if (FAILED) return;
        KsType ot = t->u.index.obj->type;
        if (!is_array(ot) && !is_any(ot)) ERR(t, "\"remove\" needs an array element or object property.");
        return;
    }
    case KS_S_ASSIGN:
    case KS_S_INCDEC:
        check_assign(c, n);
        return;
    case KS_S_EXPR:
        check_expr(c, &n->u.expr.expr);
        return;
    default:
        ERR(n, "Unexpected statement.");
        return;
    }
}

// ---------------------------------------------------------------------------
// Program

int ks_check_program(KsFront *f, KsAstProgram *prog) {
    Ck ck = {.f = f, .prog = prog};
    Ck *c = &ck;

    // actions: names, hooks (rule 12)
    for (int i = 0; i < prog->action_count && !FAILED; i++) {
        KsAstAction *a = &prog->actions[i];
        if (find_action(prog, a->name) != i) {
            ks_front_error(f, a->line, a->col, "Action \"%s\" is already defined.", a->name);
            break;
        }
        a->hook_params = -1;
        if (a->is_public) {
            int hp = ks_hook_param_count(prog->side, a->name);
            if (hp >= 0 && a->param_count > hp) {
                ks_front_error(f, a->line, a->col, "Hook \"%s\" takes at most %d parameter%s.", a->name, hp, hp == 1 ? "" : "s");
                break;
            }
            a->hook_params = hp;
        }
    }

    // globals in order: each initialiser sees only the globals declared before it
    for (int i = 0; i < prog->global_count && !FAILED; i++) {
        c->visible_globals = i;
        check_vardecl(c, prog->globals[i], i);
    }

    // action bodies see every global (module-level let, actions run after load)
    c->visible_globals = prog->global_count;
    for (int i = 0; i < prog->action_count && !FAILED; i++) {
        KsAstAction *a = &prog->actions[i];
        c->action = a;
        c->next_slot = 0;
        c->nlocals = 0;
        c->depth = 0;
        c->loop_depth = 0;
        push_scope(c);
        for (int p = 0; p < a->param_count && !FAILED; p++)
            declare_local(c, a->params[p].name, a->params[p].type, a->params[p].line, a->params[p].col);
        if (!FAILED) check_stmt(c, a->body);
        pop_scope(c);
        a->local_count = c->next_slot;
    }
    free(c->locals);
    return FAILED ? -1 : 0;
}

// ---------------------------------------------------------------------------
// Entry points

KsAstProgram *ks_front(const char *source, size_t len, KsSide side, KsDiag *err,
                       KsDiag *warnings, int max_warnings, int *warning_count) {
    KsFront f;
    memset(&f, 0, sizeof f);
    f.side = side;
    f.err = err;
    f.warnings = warnings;
    f.max_warnings = warnings ? max_warnings : 0;
    if (err) memset(err, 0, sizeof *err);
    if (warning_count) *warning_count = 0;

    if (len > KS_MAX_SOURCE_BYTES) {
        ks_front_error(&f, 0, 0, "Script too large.");
        return NULL;
    }
    f.arena = ks_arena_new();
    char *copy = f.arena ? ks_arena_alloc(f.arena, len + 1) : NULL;
    if (!copy) {
        ks_arena_free(f.arena);
        ks_front_error(&f, 0, 0, "Out of memory.");
        return NULL;
    }
    if (len) memcpy(copy, source, len);
    len = ks_map_fullwidth_parens(copy, len);
    copy[len] = 0;
    f.src = copy;
    f.len = len;

    KsAstProgram *prog = NULL;
    if (ks_lex(&f) == 0) {
        prog = ks_parse_program(&f);
        if (prog && ks_check_program(&f, prog) != 0) prog = NULL;
    }
    free(f.toks);
    if (f.arena->oom && !f.failed) ks_front_error(&f, 0, 0, "Out of memory.");
    if (warning_count) *warning_count = f.warning_count;
    if (!prog || f.failed) {
        ks_arena_free(f.arena);
        return NULL;
    }
    return prog;
}

void ks_ast_free(KsAstProgram *prog) {
    if (prog) ks_arena_free(prog->arena);
}

int ks_validate(const char *source, size_t len, KsSide side, KsDiag *err, KsDiag *warnings,
                int max_warnings, int *warning_count) {
    KsAstProgram *p = ks_front(source, len, side, err, warnings, max_warnings, warning_count);
    if (!p) return -1;
    ks_ast_free(p);
    return 0;
}
