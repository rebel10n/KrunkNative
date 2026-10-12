// KrunkScript recursive-descent parser (doc/v7/krunkscript.md 1.3): tokens ->
// KsAstProgram. Stops at the first error (no recovery). Names are resolved and
// types attached later by ks_check.c.
#include <stdio.h>
#include <string.h>

#include "ks_front_internal.h"

#define FAIL_IF(c) do { if (c) return NULL; } while (0)
#define TRY(x) do { if (!(x) || f->failed) return NULL; } while (0)

// ---------------------------------------------------------------------------
// Token helpers

static KsToken *cur(KsFront *f) { return &f->toks[f->pos]; }
static KsToken *peek(KsFront *f, int k) {
    int i = f->pos + k;
    return &f->toks[i < f->ntok ? i : f->ntok - 1];
}
static int at(KsFront *f, int kind) { return cur(f)->kind == kind; }
static KsToken *next(KsFront *f) {
    KsToken *t = cur(f);
    if (t->kind != TK_EOF) f->pos++;
    return t;
}
static int accept(KsFront *f, int kind) {
    if (!at(f, kind)) return 0;
    next(f);
    return 1;
}
static int is_base_type(int kind) { return kind >= TK_NUM_T && kind <= TK_OBJ_T; }
static int is_keyword(int kind) { return kind >= TK_FIRST_KEYWORD && kind <= TK_LAST_KEYWORD; }

static void tok_text(KsFront *f, const KsToken *t, char *buf, size_t n) {
    const char *kt = ks_token_kind_text(t->kind);
    if (kt) {
        snprintf(buf, n, "%s", kt);
        return;
    }
    size_t len = t->end - t->start;
    if (len > 40) len = 40;
    if (len >= n) len = n - 1;
    memcpy(buf, f->src + t->start, len);
    buf[len] = 0;
}

static void unexpected(KsFront *f, const KsToken *t) {
    if (t->kind == TK_EOF) {
        ks_front_error(f, t->line, t->col, "Unexpected end of script.");
        return;
    }
    char buf[64];
    tok_text(f, t, buf, sizeof buf);
    ks_front_error(f, t->line, t->col, "Unexpected \"%s\".", buf);
}

static int expect(KsFront *f, int kind) {
    if (accept(f, kind)) return 1;
    KsToken *t = cur(f);
    if (t->kind == TK_EOF) {
        unexpected(f, t);
        return 0;
    }
    char buf[64];
    tok_text(f, t, buf, sizeof buf);
    ks_front_error(f, t->line, t->col, "Expected \"%s\" but found \"%s\".", ks_token_kind_text(kind), buf);
    return 0;
}

static int check_name(KsFront *f, const KsToken *t, int is_decl) {
    if (ks_is_restricted_name(t->text) || (is_decl && ks_is_predeclared_name(t->text))) {
        ks_front_error(f, t->line, t->col, "Restricted name \"%s\".", t->text);
        return 0;
    }
    return 1;
}

// A declared name (variable, parameter, action): an identifier, not restricted.
static const KsToken *expect_decl_name(KsFront *f) {
    KsToken *t = cur(f);
    if (t->kind != TK_IDENT) {
        unexpected(f, t);
        return NULL;
    }
    if (!check_name(f, t, 1)) return NULL;
    return next(f);
}

// A property name after "." or an object key: identifiers and keywords.
static const KsToken *expect_prop_name(KsFront *f) {
    KsToken *t = cur(f);
    if (t->kind != TK_IDENT && !is_keyword(t->kind)) {
        unexpected(f, t);
        return NULL;
    }
    if (!check_name(f, t, 0)) return NULL;
    return next(f);
}

// ---------------------------------------------------------------------------
// Nodes and lists

static KsNode *node_at(KsFront *f, int kind, const KsToken *t) {
    KsNode *n = ks_arena_alloc(f->arena, sizeof *n);
    if (!n) {
        ks_front_error(f, t->line, t->col, "Out of memory.");
        return NULL;
    }
    n->kind = (uint8_t) kind;
    n->line = t->line;
    n->col = t->col;
    n->start = t->start;
    n->end = t->end;
    n->type.base = KS_T_ANY;
    return n;
}

static KsNode *finish(KsFront *f, KsNode *n) {
    if (n && f->pos > 0) n->end = f->toks[f->pos - 1].end;
    return n;
}

typedef struct { KsNode **items; int count, cap; } Vec;

static int vec_push(KsFront *f, Vec *v, KsNode *n) {
    if (v->count == v->cap) {
        int cap = v->cap ? v->cap * 2 : 8;
        KsNode **items = ks_arena_alloc(f->arena, (size_t) cap * sizeof *items);
        if (!items) {
            ks_front_error(f, 0, 0, "Out of memory.");
            return 0;
        }
        if (v->count) memcpy(items, v->items, (size_t) v->count * sizeof *items);
        v->items = items;
        v->cap = cap;
    }
    v->items[v->count++] = n;
    return 1;
}

static KsNodeList vec_list(Vec v) {
    KsNodeList l = {v.items, v.count};
    return l;
}

// ---------------------------------------------------------------------------
// Types

// baseType { "[" "]" }
static int parse_type(KsFront *f, KsType *t) {
    KsToken *b = cur(f);
    if (!is_base_type(b->kind)) {
        unexpected(f, b);
        return 0;
    }
    next(f);
    t->base = (uint8_t) (b->kind == TK_NUM_T ? KS_T_NUM : b->kind == TK_STR_T ? KS_T_STR
                       : b->kind == TK_BOOL_T ? KS_T_BOOL : KS_T_OBJ);
    t->dims = 0;
    while (at(f, TK_LBRACKET) && peek(f, 1)->kind == TK_RBRACKET) {
        if (t->dims == 255) {
            unexpected(f, cur(f));
            return 0;
        }
        next(f);
        next(f);
        t->dims++;
    }
    return 1;
}

// ---------------------------------------------------------------------------
// Expressions

static KsNode *parse_expr(KsFront *f);
static KsNode *parse_unary(KsFront *f);

static KsNode *parse_array_lit(KsFront *f) {
    KsToken *bt = cur(f);
    KsNode *n = node_at(f, KS_E_ARRAY, bt);
    FAIL_IF(!n);
    KsType elem;
    elem.base = (uint8_t) (bt->kind == TK_NUM_T ? KS_T_NUM : bt->kind == TK_STR_T ? KS_T_STR
                         : bt->kind == TK_BOOL_T ? KS_T_BOOL : KS_T_OBJ);
    elem.dims = 0;
    next(f);
    Vec items = {0};
    for (;;) {
        if (!at(f, TK_LBRACKET)) {
            expect(f, TK_LBRACKET);
            return NULL;
        }
        if (peek(f, 1)->kind == TK_RBRACKET) {
            next(f);
            next(f);
            if (at(f, TK_LBRACKET)) {
                if (elem.dims == 254) {
                    unexpected(f, cur(f));
                    return NULL;
                }
                elem.dims++;
                continue;
            }
            break;   // empty element list
        }
        next(f);   // "["
        for (;;) {
            KsNode *e;
            TRY(e = parse_expr(f));
            if (!vec_push(f, &items, e)) return NULL;
            if (accept(f, TK_COMMA)) {
                if (at(f, TK_RBRACKET)) break;
                continue;
            }
            break;
        }
        if (!expect(f, TK_RBRACKET)) return NULL;
        break;
    }
    n->u.array.elem_type = elem;
    n->u.array.items = vec_list(items);
    return finish(f, n);
}

static KsNode *parse_object_lit(KsFront *f) {
    KsNode *n = node_at(f, KS_E_OBJECT, cur(f));
    FAIL_IF(!n);
    next(f);   // "{"
    Vec values = {0};
    int kcap = 0;
    const char **keys = NULL;
    while (!at(f, TK_RBRACE)) {
        const char *key;
        KsToken *kt = cur(f);
        if (kt->kind == TK_STR) {
            if (ks_is_restricted_name(kt->text)) {
                ks_front_error(f, kt->line, kt->col, "Restricted name \"%s\".", kt->text);
                return NULL;
            }
            key = kt->text;
            next(f);
        } else {
            const KsToken *t = expect_prop_name(f);
            if (!t) return NULL;
            key = t->text;
        }
        if (!expect(f, TK_COLON)) return NULL;
        KsNode *v;
        TRY(v = parse_expr(f));
        if (values.count == kcap) {
            int cap = kcap ? kcap * 2 : 8;
            const char **k2 = ks_arena_alloc(f->arena, (size_t) cap * sizeof *k2);
            if (!k2) {
                ks_front_error(f, kt->line, kt->col, "Out of memory.");
                return NULL;
            }
            if (values.count) memcpy(k2, keys, (size_t) values.count * sizeof *k2);
            keys = k2;
            kcap = cap;
        }
        keys[values.count] = key;
        if (!vec_push(f, &values, v)) return NULL;
        if (!accept(f, TK_COMMA)) break;
    }
    if (!expect(f, TK_RBRACE)) return NULL;
    n->u.object.keys = keys;
    n->u.object.values = vec_list(values);
    return finish(f, n);
}

static KsNode *parse_primary(KsFront *f) {
    KsToken *t = cur(f);
    KsNode *n;
    switch (t->kind) {
    case TK_NUM:
        TRY(n = node_at(f, KS_E_NUM, t));
        n->u.num = t->num;
        next(f);
        return n;
    case TK_STR:
        TRY(n = node_at(f, KS_E_STR, t));
        n->u.str.s = t->text;
        n->u.str.len = t->len;
        next(f);
        return n;
    case TK_TRUE:
    case TK_FALSE:
        TRY(n = node_at(f, KS_E_BOOL, t));
        n->u.boolean = t->kind == TK_TRUE;
        next(f);
        return n;
    case TK_IDENT:
        if (!check_name(f, t, 0)) return NULL;
        TRY(n = node_at(f, KS_E_IDENT, t));
        n->u.ident.name = t->text;
        n->u.ident.slot = -1;
        next(f);
        return n;
    case TK_LPAREN: {
        next(f);
        TRY(n = parse_expr(f));
        if (!expect(f, TK_RPAREN)) return NULL;
        n->parens = 1;
        return n;
    }
    case TK_LBRACE:
        return parse_object_lit(f);
    default:
        if (is_base_type(t->kind)) return parse_array_lit(f);
        unexpected(f, t);
        return NULL;
    }
}

static KsNode *parse_postfix(KsFront *f) {
    KsNode *p;
    TRY(p = parse_primary(f));
    for (;;) {
        KsToken *t = cur(f);
        if (t->kind == TK_LPAREN) {
            KsNode *n;
            TRY(n = node_at(f, KS_E_CALL, t));
            n->line = p->line;
            n->col = p->col;
            n->start = p->start;
            n->u.call.callee = p;
            n->u.call.action_index = -1;
            n->u.call.api_id = -1;
            next(f);
            Vec args = {0};
            while (!at(f, TK_RPAREN)) {
                KsNode *a;
                TRY(a = parse_expr(f));
                if (!vec_push(f, &args, a)) return NULL;
                if (!accept(f, TK_COMMA)) break;
            }
            if (!expect(f, TK_RPAREN)) return NULL;
            n->u.call.args = vec_list(args);
            n->u.call.callee_text = ks_arena_strndup(f->arena, f->src + p->start, p->end - p->start);
            if (!n->u.call.callee_text) {
                ks_front_error(f, t->line, t->col, "Out of memory.");
                return NULL;
            }
            p = finish(f, n);
        } else if (t->kind == TK_DOT) {
            next(f);
            const KsToken *name = expect_prop_name(f);
            if (!name) return NULL;
            KsNode *n;
            TRY(n = node_at(f, KS_E_MEMBER, t));
            n->line = p->line;
            n->col = p->col;
            n->start = p->start;
            n->u.member.obj = p;
            n->u.member.name = name->text;
            n->u.member.name_line = name->line;
            n->u.member.name_col = name->col;
            p = finish(f, n);
        } else if (t->kind == TK_LBRACKET) {
            KsNode *n;
            TRY(n = node_at(f, KS_E_INDEX, t));
            n->start = p->start;
            next(f);
            KsNode *idx;
            TRY(idx = parse_expr(f));
            if (!expect(f, TK_RBRACKET)) return NULL;
            n->u.index.obj = p;
            n->u.index.index = idx;
            p = finish(f, n);
        } else {
            return p;
        }
    }
}

static int keyword_op(int kind) {
    switch (kind) {
    case TK_LENGTHOF: return KS_KW_LENGTHOF;
    case TK_NOTEMPTY: return KS_KW_NOTEMPTY;
    case TK_TONUM: return KS_KW_TONUM;
    case TK_TOSTR: return KS_KW_TOSTR;
    default: return -1;
    }
}

static KsNode *parse_unary_inner(KsFront *f);

static KsNode *parse_unary(KsFront *f) {
    if (++f->depth > KS_MAX_PARSE_DEPTH) {
        ks_front_error(f, cur(f)->line, cur(f)->col, KS_NESTING_MESSAGE);
        return NULL;
    }
    KsNode *n = parse_unary_inner(f);
    f->depth--;
    return n;
}

static KsNode *parse_unary_inner(KsFront *f) {
    KsToken *t = cur(f);
    KsNode *n;
    switch (t->kind) {
    case TK_BANG:
    case TK_MINUS:
    case TK_PLUS:
    case TK_TILDE: {
        TRY(n = node_at(f, KS_E_UNARY, t));
        n->op = (uint8_t) (t->kind == TK_BANG ? '!' : t->kind == TK_MINUS ? '-' : t->kind == TK_PLUS ? '+' : '~');
        next(f);
        TRY(n->u.unary.expr = parse_unary(f));
        return finish(f, n);
    }
    case TK_LENGTHOF:
    case TK_NOTEMPTY:
    case TK_TONUM:
    case TK_TOSTR:
        TRY(n = node_at(f, KS_E_KEYWORD, t));
        n->op = (uint8_t) keyword_op(t->kind);
        next(f);
        TRY(n->u.keyword.expr = parse_unary(f));
        return finish(f, n);
    case TK_HASPROP: {
        next(f);
        KsNode *p;
        TRY(p = parse_postfix(f));
        if (p->kind != KS_E_MEMBER || p->parens) {
            ks_front_error(f, t->line, t->col, "\"hasProp\" needs an object property.");
            return NULL;
        }
        TRY(n = node_at(f, KS_E_HASPROP, t));
        n->u.member = p->u.member;
        return finish(f, n);
    }
    case TK_LPAREN:
        if (is_base_type(peek(f, 1)->kind)) {
            TRY(n = node_at(f, KS_E_CAST, t));
            next(f);
            if (!parse_type(f, &n->u.cast.to)) return NULL;
            if (!expect(f, TK_RPAREN)) return NULL;
            TRY(n->u.cast.expr = parse_unary(f));
            return finish(f, n);
        }
        return parse_postfix(f);
    default:
        return parse_postfix(f);
    }
}

// power = unary [ "**" power ]
static KsNode *parse_power(KsFront *f) {
    KsNode *l;
    TRY(l = parse_unary(f));
    if (!at(f, TK_POW)) return l;
    if (l->kind == KS_E_UNARY && !l->parens) {
        ks_front_error(f, l->line, l->col, "Wrap the unary operand of \"**\" in parentheses.");
        return NULL;
    }
    KsToken *op = next(f);
    KsNode *n;
    TRY(n = node_at(f, KS_E_BINARY, op));
    n->op = KS_OP_POW;
    n->start = l->start;
    n->u.binary.l = l;
    TRY(n->u.binary.r = parse_power(f));
    return finish(f, n);
}

typedef KsNode *(*ParseFn)(KsFront *);
typedef struct { uint8_t tok, op; } OpMap;

static KsNode *parse_binary_level(KsFront *f, ParseFn sub, const OpMap *ops, int nops, int node_kind) {
    KsNode *l;
    TRY(l = sub(f));
    for (;;) {
        int found = -1;
        for (int i = 0; i < nops; i++)
            if (at(f, ops[i].tok)) { found = i; break; }
        if (found < 0) return l;
        KsToken *op = next(f);
        KsNode *n;
        TRY(n = node_at(f, node_kind, op));
        n->op = ops[found].op;
        n->start = l->start;
        n->u.binary.l = l;
        TRY(n->u.binary.r = sub(f));
        l = finish(f, n);
    }
}

static const OpMap ops_mul[] = {{TK_STAR, KS_OP_MUL}, {TK_SLASH, KS_OP_DIV}, {TK_PERCENT, KS_OP_MOD}};
static const OpMap ops_add[] = {{TK_PLUS, KS_OP_ADD}, {TK_MINUS, KS_OP_SUB}};
static const OpMap ops_shift[] = {{TK_SHL, KS_OP_SHL}, {TK_SHR, KS_OP_SHR}, {TK_USHR, KS_OP_USHR}};
static const OpMap ops_rel[] = {{TK_LT, KS_OP_LT}, {TK_LE, KS_OP_LE}, {TK_GT, KS_OP_GT}, {TK_GE, KS_OP_GE}};
static const OpMap ops_eq[] = {{TK_EQ, KS_OP_EQ}, {TK_NE, KS_OP_NE}};
static const OpMap ops_band[] = {{TK_AMP, KS_OP_BAND}};
static const OpMap ops_bxor[] = {{TK_CARET, KS_OP_BXOR}};
static const OpMap ops_bor[] = {{TK_PIPE, KS_OP_BOR}};
static const OpMap ops_and[] = {{TK_ANDAND, KS_OP_AND}};
static const OpMap ops_or[] = {{TK_OROR, KS_OP_OR}};

#define LEVEL(name, sub, ops, kind) \
    static KsNode *name(KsFront *f) { return parse_binary_level(f, sub, ops, (int) (sizeof ops / sizeof ops[0]), kind); }
LEVEL(parse_mul, parse_power, ops_mul, KS_E_BINARY)
LEVEL(parse_add, parse_mul, ops_add, KS_E_BINARY)
LEVEL(parse_shift, parse_add, ops_shift, KS_E_BINARY)
LEVEL(parse_rel, parse_shift, ops_rel, KS_E_BINARY)
LEVEL(parse_eq, parse_rel, ops_eq, KS_E_BINARY)
LEVEL(parse_band, parse_eq, ops_band, KS_E_BINARY)
LEVEL(parse_bxor, parse_band, ops_bxor, KS_E_BINARY)
LEVEL(parse_bor, parse_bxor, ops_bor, KS_E_BINARY)
LEVEL(parse_and, parse_bor, ops_and, KS_E_LOGICAL)
LEVEL(parse_or, parse_and, ops_or, KS_E_LOGICAL)

static KsNode *parse_expr(KsFront *f) {
    KsNode *c;
    TRY(c = parse_or(f));
    if (!at(f, TK_QUESTION)) return c;
    KsNode *n;
    TRY(n = node_at(f, KS_E_TERNARY, cur(f)));
    next(f);
    n->start = c->start;
    n->u.ternary.cond = c;
    TRY(n->u.ternary.then = parse_expr(f));
    if (!expect(f, TK_COLON)) return NULL;
    TRY(n->u.ternary.els = parse_expr(f));
    return finish(f, n);
}

// ---------------------------------------------------------------------------
// Statements

static KsNode *parse_statement(KsFront *f);
static KsNode *parse_block(KsFront *f);

// varDecl = [ "static" | "dynamic" ] type Ident [ "=" expr ]
static KsNode *parse_vardecl(KsFront *f) {
    KsNode *n;
    TRY(n = node_at(f, KS_S_VARDECL, cur(f)));
    if (accept(f, TK_STATIC)) n->u.var.modifier = KS_MOD_STATIC;
    else if (accept(f, TK_DYNAMIC)) n->u.var.modifier = KS_MOD_DYNAMIC;
    if (!parse_type(f, &n->u.var.type)) return NULL;
    const KsToken *name = expect_decl_name(f);
    if (!name) return NULL;
    n->u.var.name = name->text;
    n->u.var.slot = -1;
    if (accept(f, TK_ASSIGN)) TRY(n->u.var.init = parse_expr(f));
    return finish(f, n);
}

static int assign_op(int kind) {
    switch (kind) {
    case TK_ASSIGN: return KS_OP_ASSIGN;
    case TK_ADD_ASSIGN: return KS_OP_ADD;
    case TK_SUB_ASSIGN: return KS_OP_SUB;
    case TK_MUL_ASSIGN: return KS_OP_MUL;
    case TK_DIV_ASSIGN: return KS_OP_DIV;
    case TK_MOD_ASSIGN: return KS_OP_MOD;
    case TK_POW_ASSIGN: return KS_OP_POW;
    case TK_SHL_ASSIGN: return KS_OP_SHL;
    case TK_SHR_ASSIGN: return KS_OP_SHR;
    case TK_USHR_ASSIGN: return KS_OP_USHR;
    case TK_AND_ASSIGN: return KS_OP_BAND;
    case TK_OR_ASSIGN: return KS_OP_BOR;
    case TK_XOR_ASSIGN: return KS_OP_BXOR;
    default: return -1;
    }
}

// assignTarget = [ cast ] postfix  (postfix ends in Ident, .Ident or [expr])
static KsNode *parse_assign_target(KsFront *f, KsNode *stmt) {
    if (at(f, TK_LPAREN) && is_base_type(peek(f, 1)->kind)) {
        next(f);
        if (!parse_type(f, &stmt->u.assign.cast)) return NULL;
        if (!expect(f, TK_RPAREN)) return NULL;
        stmt->u.assign.has_cast = 1;
    }
    return parse_postfix(f);
}

static int valid_target(const KsNode *t) {
    return !t->parens && (t->kind == KS_E_IDENT || t->kind == KS_E_MEMBER || t->kind == KS_E_INDEX);
}

// simpleStmt (assignment, ++/--, or a call)
static KsNode *parse_simple(KsFront *f) {
    KsToken *first = cur(f);
    KsNode *n;
    TRY(n = node_at(f, KS_S_ASSIGN, first));
    if (first->kind == TK_INC || first->kind == TK_DEC) {
        next(f);
        n->kind = KS_S_INCDEC;
        n->op = first->kind == TK_INC ? KS_OP_INC : KS_OP_DEC;
        n->u.assign.prefix = 1;
        KsNode *t;
        TRY(t = parse_assign_target(f, n));
        if (!valid_target(t)) {
            ks_front_error(f, t->line, t->col, "Invalid assignment target.");
            return NULL;
        }
        n->u.assign.target = t;
        return finish(f, n);
    }
    KsNode *t;
    TRY(t = parse_assign_target(f, n));
    KsToken *opt = cur(f);
    int op = assign_op(opt->kind);
    if (op >= 0 || opt->kind == TK_INC || opt->kind == TK_DEC) {
        if (!valid_target(t)) {
            ks_front_error(f, t->line, t->col, "Invalid assignment target.");
            return NULL;
        }
        next(f);
        n->u.assign.target = t;
        if (op >= 0) {
            n->op = (uint8_t) op;
            TRY(n->u.assign.value = parse_expr(f));
        } else {
            n->kind = KS_S_INCDEC;
            n->op = opt->kind == TK_INC ? KS_OP_INC : KS_OP_DEC;
        }
        return finish(f, n);
    }
    if (n->u.assign.has_cast || t->kind != KS_E_CALL || t->parens) {
        if (opt->kind == TK_EOF) {
            unexpected(f, opt);
        } else {
            char buf[64];
            tok_text(f, opt, buf, sizeof buf);
            ks_front_error(f, opt->line, opt->col, "Expected \"=\" but found \"%s\".", buf);
        }
        return NULL;
    }
    n->kind = KS_S_EXPR;
    memset(&n->u, 0, sizeof n->u);
    n->u.expr.expr = t;
    return finish(f, n);
}

static KsNode *parse_body(KsFront *f) {
    if (at(f, TK_LBRACE)) return parse_block(f);
    return parse_statement(f);
}

static KsNode *parse_for(KsFront *f) {
    KsToken *kw = next(f);
    if (!expect(f, TK_LPAREN)) return NULL;
    // for (T v : e)
    if (is_base_type(cur(f)->kind)) {
        int save = f->pos;
        KsType t;
        if (!parse_type(f, &t)) return NULL;
        int is_each = at(f, TK_IDENT) && peek(f, 1)->kind == TK_COLON;
        f->pos = save;
        if (is_each) {
            KsNode *n;
            TRY(n = node_at(f, KS_S_FOREACH, kw));
            parse_type(f, &n->u.foreach.var_type);
            const KsToken *name = expect_decl_name(f);
            if (!name) return NULL;
            n->u.foreach.var_name = name->text;
            n->u.foreach.slot = -1;
            next(f);   // ":"
            TRY(n->u.foreach.iterable = parse_expr(f));
            if (!expect(f, TK_RPAREN)) return NULL;
            TRY(n->u.foreach.body = parse_body(f));
            return finish(f, n);
        }
    }
    KsNode *n;
    TRY(n = node_at(f, KS_S_FOR, kw));
    if (!at(f, TK_SEMI)) {
        int k = cur(f)->kind;
        if (k == TK_STATIC || k == TK_DYNAMIC || is_base_type(k)) TRY(n->u.loop.init = parse_vardecl(f));
        else TRY(n->u.loop.init = parse_simple(f));
    }
    if (!expect(f, TK_SEMI)) return NULL;
    if (!at(f, TK_SEMI)) TRY(n->u.loop.cond = parse_expr(f));
    if (!expect(f, TK_SEMI)) return NULL;
    if (!at(f, TK_RPAREN)) TRY(n->u.loop.step = parse_simple(f));
    if (!expect(f, TK_RPAREN)) return NULL;
    TRY(n->u.loop.body = parse_body(f));
    return finish(f, n);
}

static KsNode *parse_statement_inner(KsFront *f);

static KsNode *parse_statement(KsFront *f) {
    if (++f->depth > KS_MAX_PARSE_DEPTH) {
        ks_front_error(f, cur(f)->line, cur(f)->col, KS_NESTING_MESSAGE);
        return NULL;
    }
    KsNode *n = parse_statement_inner(f);
    f->depth--;
    return n;
}

static KsNode *parse_statement_inner(KsFront *f) {
    KsToken *t = cur(f);
    KsNode *n;
    switch (t->kind) {
    case TK_LBRACE:
        return parse_block(f);
    case TK_SEMI:
        TRY(n = node_at(f, KS_S_EMPTY, t));
        next(f);
        return n;
    case TK_STATIC:
    case TK_DYNAMIC:
    case TK_NUM_T:
    case TK_STR_T:
    case TK_BOOL_T:
    case TK_OBJ_T:
        TRY(n = parse_vardecl(f));
        if (!expect(f, TK_SEMI)) return NULL;
        return finish(f, n);
    case TK_IF: {
        TRY(n = node_at(f, KS_S_IF, t));
        next(f);
        if (!expect(f, TK_LPAREN)) return NULL;
        TRY(n->u.if_.cond = parse_expr(f));
        if (!expect(f, TK_RPAREN)) return NULL;
        TRY(n->u.if_.then = parse_body(f));
        if (accept(f, TK_ELSE)) TRY(n->u.if_.els = parse_body(f));
        return finish(f, n);
    }
    case TK_WHILE:
        TRY(n = node_at(f, KS_S_WHILE, t));
        next(f);
        if (!expect(f, TK_LPAREN)) return NULL;
        TRY(n->u.loop.cond = parse_expr(f));
        if (!expect(f, TK_RPAREN)) return NULL;
        TRY(n->u.loop.body = parse_body(f));
        return finish(f, n);
    case TK_FOR:
        return parse_for(f);
    case TK_RETURN:
        TRY(n = node_at(f, KS_S_RETURN, t));
        next(f);
        if (!at(f, TK_SEMI)) TRY(n->u.ret.expr = parse_expr(f));
        if (!expect(f, TK_SEMI)) return NULL;
        return finish(f, n);
    case TK_BREAK:
    case TK_CONTINUE:
        TRY(n = node_at(f, t->kind == TK_BREAK ? KS_S_BREAK : KS_S_CONTINUE, t));
        next(f);
        if (!expect(f, TK_SEMI)) return NULL;
        return finish(f, n);
    case TK_ADDTO:
        TRY(n = node_at(f, KS_S_ADDTO, t));
        next(f);
        TRY(n->u.addto.target = parse_unary(f));
        TRY(n->u.addto.value = parse_expr(f));
        if (!expect(f, TK_SEMI)) return NULL;
        return finish(f, n);
    case TK_REMOVE: {
        TRY(n = node_at(f, KS_S_REMOVE, t));
        next(f);
        KsNode *target;
        TRY(target = parse_postfix(f));
        if ((target->kind != KS_E_INDEX && target->kind != KS_E_MEMBER) || target->parens) {
            ks_front_error(f, target->line, target->col, "\"remove\" needs an array element or object property.");
            return NULL;
        }
        n->u.remove.target = target;
        if (!expect(f, TK_SEMI)) return NULL;
        return finish(f, n);
    }
    case TK_ACTION:
    case TK_PUBLIC:
    case TK_ELSE:
    case TK_EOF:
        unexpected(f, t);
        return NULL;
    default:
        TRY(n = parse_simple(f));
        if (!expect(f, TK_SEMI)) return NULL;
        return finish(f, n);
    }
}

static KsNode *parse_block(KsFront *f) {
    KsNode *n;
    TRY(n = node_at(f, KS_S_BLOCK, cur(f)));
    if (!expect(f, TK_LBRACE)) return NULL;
    Vec stmts = {0};
    while (!at(f, TK_RBRACE)) {
        if (at(f, TK_EOF)) {
            unexpected(f, cur(f));
            return NULL;
        }
        KsNode *s;
        TRY(s = parse_statement(f));
        if (!vec_push(f, &stmts, s)) return NULL;
    }
    next(f);
    n->u.block = vec_list(stmts);
    return finish(f, n);
}

// ---------------------------------------------------------------------------
// Top level

// [ "static" | "dynamic" ] ( type | [ type ] "action" [ "(" [ type { "," type } ] ")" ] ) Ident
static int parse_param(KsFront *f, KsParam *p) {
    KsToken *first = cur(f);
    p->line = first->line;
    p->col = first->col;
    if (accept(f, TK_STATIC)) p->modifier = KS_MOD_STATIC;
    else if (accept(f, TK_DYNAMIC)) p->modifier = KS_MOD_DYNAMIC;
    if (is_base_type(cur(f)->kind)) {
        if (!parse_type(f, &p->type)) return 0;
    } else if (!at(f, TK_ACTION)) {
        unexpected(f, cur(f));
        return 0;
    }
    if (accept(f, TK_ACTION)) {
        p->type.base = KS_T_ACTION;
        p->type.dims = 0;
        if (accept(f, TK_LPAREN)) {
            while (!at(f, TK_RPAREN)) {
                KsType ignored;
                if (!parse_type(f, &ignored)) return 0;
                if (!accept(f, TK_COMMA)) break;
            }
            if (!expect(f, TK_RPAREN)) return 0;
        }
    }
    const KsToken *name = expect_decl_name(f);
    if (!name) return 0;
    p->name = name->text;
    p->line = name->line;
    p->col = name->col;
    return 1;
}

static int parse_action(KsFront *f, KsAstAction *a) {
    KsToken *first = cur(f);
    a->line = first->line;
    a->col = first->col;
    a->hook_params = -1;
    a->is_public = accept(f, TK_PUBLIC);
    a->ret_type.base = KS_T_VOID;
    a->ret_type.dims = 0;
    if (is_base_type(cur(f)->kind) && !parse_type(f, &a->ret_type)) return 0;
    if (!expect(f, TK_ACTION)) return 0;
    const KsToken *name = expect_decl_name(f);
    if (!name) return 0;
    a->name = name->text;
    if (!expect(f, TK_LPAREN)) return 0;
    int cap = 0;
    while (!at(f, TK_RPAREN)) {
        if (a->param_count == cap) {
            cap = cap ? cap * 2 : 4;
            KsParam *ps = ks_arena_alloc(f->arena, (size_t) cap * sizeof *ps);
            if (!ps) {
                ks_front_error(f, first->line, first->col, "Out of memory.");
                return 0;
            }
            if (a->param_count) memcpy(ps, a->params, (size_t) a->param_count * sizeof *ps);
            a->params = ps;
        }
        if (!parse_param(f, &a->params[a->param_count])) return 0;
        a->param_count++;
        if (!accept(f, TK_COMMA)) break;
    }
    if (!expect(f, TK_RPAREN)) return 0;
    a->body = parse_block(f);
    return a->body != NULL && !f->failed;
}

// Does the token stream at the cursor start a "[type] action" declaration?
static int starts_action(KsFront *f) {
    if (at(f, TK_PUBLIC) || at(f, TK_ACTION)) return 1;
    if (!is_base_type(cur(f)->kind)) return 0;
    int i = 1;
    while (peek(f, i)->kind == TK_LBRACKET && peek(f, i + 1)->kind == TK_RBRACKET) i += 2;
    return peek(f, i)->kind == TK_ACTION;
}

KsAstProgram *ks_parse_program(KsFront *f) {
    KsAstProgram *prog = ks_arena_alloc(f->arena, sizeof *prog);
    if (!prog) {
        ks_front_error(f, 0, 0, "Out of memory.");
        return NULL;
    }
    prog->side = f->side;
    prog->source = f->src;
    prog->source_len = f->len;
    prog->arena = f->arena;
    Vec globals = {0};
    int acap = 0;
    while (!at(f, TK_EOF) && !f->failed) {
        if (accept(f, TK_SEMI)) continue;
        if (starts_action(f)) {
            if (prog->action_count == acap) {
                acap = acap ? acap * 2 : 16;
                KsAstAction *as = ks_arena_alloc(f->arena, (size_t) acap * sizeof *as);
                if (!as) {
                    ks_front_error(f, 0, 0, "Out of memory.");
                    return NULL;
                }
                if (prog->action_count) memcpy(as, prog->actions, (size_t) prog->action_count * sizeof *as);
                prog->actions = as;
            }
            if (!parse_action(f, &prog->actions[prog->action_count])) return NULL;
            prog->action_count++;
            continue;
        }
        int k = cur(f)->kind;
        if (k == TK_STATIC || k == TK_DYNAMIC || is_base_type(k)) {
            KsNode *d;
            TRY(d = parse_vardecl(f));
            if (!expect(f, TK_SEMI)) return NULL;
            finish(f, d);
            d->u.var.scope = KS_SCOPE_GLOBAL;
            if (!vec_push(f, &globals, d)) return NULL;
            continue;
        }
        KsToken *t = cur(f);
        ks_front_error(f, t->line, t->col, "Statements are only allowed inside actions.");
        return NULL;
    }
    if (f->failed) return NULL;
    prog->globals = globals.items;
    prog->global_count = globals.count;
    return prog;
}
