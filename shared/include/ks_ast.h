// KrunkScript syntax tree (doc/v7/krunkscript.md 6.3): produced and checked by
// the front end (WP1: ks_lex.c, ks_parse.c, ks_check.c), consumed by the code
// generator (WP2). Frozen: change only by agreement between WP1 and WP2.
//
// Ownership: every node, string and list lives in the program's arena and is
// released by ks_ast_free(). After ks_front() succeeds every expression node
// carries its resolved static type and every name its resolved slot/index.
#ifndef KS_AST_H
#define KS_AST_H

#include <stddef.h>
#include <stdint.h>

#include "ks.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Types (1.4). `any` and `void` are checker-internal. `dims` > 0 = array of
// `base` with that many [] (base is never ANY/VOID/ACTION for arrays).
typedef enum {
    KS_T_NUM, KS_T_STR, KS_T_BOOL, KS_T_OBJ,
    KS_T_ANY, KS_T_VOID, KS_T_ACTION
} KsTypeBase;

typedef struct KsType { uint8_t base; uint8_t dims; } KsType;

// Writes "num", "str[][]", "action", "any", "void" (returns buf).
const char *ks_type_name(KsType t, char *buf, size_t n);
int         ks_type_eq(KsType a, KsType b);

// ---------------------------------------------------------------------------
// Nodes
typedef enum {
    // expressions
    KS_E_NUM,        // u.num
    KS_E_STR,        // u.str (escape-processed bytes)
    KS_E_BOOL,       // u.boolean
    KS_E_IDENT,      // u.ident: a variable (global or local)
    KS_E_MEMBER,     // u.member: obj.name on a script/host value
    KS_E_INDEX,      // u.index: obj[index] (bounds-checked, line/col = the "[")
    KS_E_CALL,       // u.call
    KS_E_GAMEREF,    // u.gameref: resolved GAME.* path (NS used only as call/member base)
    KS_E_UNARY,      // u.unary, op = '!' '-' '+' '~'
    KS_E_BINARY,     // u.binary, op = KsBinOp (line/col = the operator)
    KS_E_LOGICAL,    // u.binary, op = KS_OP_AND / KS_OP_OR (line/col = the operator)
    KS_E_TERNARY,    // u.ternary (line/col = the "?")
    KS_E_CAST,       // u.cast; u.cast.implicit = inserted by the checker for any -> T
    KS_E_KEYWORD,    // u.keyword, op = KsKeywordOp
    KS_E_HASPROP,    // u.member: hasProp obj.name
    KS_E_ARRAY,      // u.array
    KS_E_OBJECT,     // u.object
    KS_E_ACTIONREF,  // u.actionref: a top-level action used as a value

    // statements
    KS_S_VARDECL,    // u.var
    KS_S_IF,         // u.if_
    KS_S_WHILE,      // u.loop (cond, body; line/col = the "while" keyword)
    KS_S_FOR,        // u.loop (init?, cond?, step?, body; line/col = the "for" keyword)
    KS_S_FOREACH,    // u.foreach (line/col = the "for" keyword)
    KS_S_RETURN,     // u.ret
    KS_S_BREAK,
    KS_S_CONTINUE,
    KS_S_ADDTO,      // u.addto
    KS_S_REMOVE,     // u.remove (target is KS_E_INDEX or KS_E_MEMBER)
    KS_S_ASSIGN,     // u.assign, op = KsAssignOp
    KS_S_INCDEC,     // u.assign (value NULL), op = KS_OP_INC / KS_OP_DEC, u.assign.prefix
    KS_S_EXPR,       // u.expr (always a KS_E_CALL)
    KS_S_BLOCK,      // u.block
    KS_S_EMPTY       // ";"
} KsNodeKind;

typedef enum {
    KS_OP_ADD, KS_OP_SUB, KS_OP_MUL, KS_OP_DIV, KS_OP_MOD, KS_OP_POW,
    KS_OP_SHL, KS_OP_SHR, KS_OP_USHR, KS_OP_BAND, KS_OP_BOR, KS_OP_BXOR,
    KS_OP_EQ, KS_OP_NE, KS_OP_LT, KS_OP_LE, KS_OP_GT, KS_OP_GE,
    KS_OP_AND, KS_OP_OR,
    // assignment (KS_S_ASSIGN): plain "=" or compound op = the matching binary op
    KS_OP_ASSIGN,
    // KS_S_INCDEC
    KS_OP_INC, KS_OP_DEC
} KsBinOp;

typedef enum { KS_KW_LENGTHOF, KS_KW_NOTEMPTY, KS_KW_TONUM, KS_KW_TOSTR } KsKeywordOp;

typedef enum {
    KS_CALL_ACTION,        // direct call of a top-level action: u.call.action_index
    KS_CALL_ACTION_VALUE,  // callee is an expression of type action/any (variable, element...)
    KS_CALL_GAME_API,      // callee is a KS_E_GAMEREF of kind FN: u.call.api_id
    KS_CALL_METHOD         // callee is a KS_E_MEMBER: FC check on obj.name (line/col = the name)
} KsCallKind;

enum { KS_SCOPE_GLOBAL, KS_SCOPE_LOCAL };
enum { KS_MOD_NONE, KS_MOD_STATIC, KS_MOD_DYNAMIC };

typedef struct KsNode KsNode;
typedef struct { KsNode **items; int count; } KsNodeList;

struct KsNode {
    uint8_t kind;      // KsNodeKind
    uint8_t op;        // KsBinOp / KsKeywordOp / unary char, per kind
    uint8_t parens;    // expression was written inside ( ) (parser detail)
    int line, col;     // first token (except where noted above)
    uint32_t start, end; // byte span in KsAstProgram.source (end exclusive)
    KsType type;       // expressions: static type after checking
    union {
        double num;
        struct { const char *s; size_t len; } str;   // UTF-8; lone surrogates as 3-byte WTF-8
        int boolean;
        struct { const char *name; int scope; int slot; } ident;
        struct { KsNode *obj; const char *name; int name_line, name_col; } member;
        struct { KsNode *obj; KsNode *index; } index;
        struct {
            KsNode *callee;
            KsNodeList args;
            uint8_t call_kind;      // KsCallKind
            int action_index;       // KS_CALL_ACTION
            int api_id;             // KS_CALL_GAME_API (KsApiId)
            const char *callee_text; // source text of the callee ("<expr> is not a function")
        } call;
        struct { int api_id; } gameref;  // KsApiId; kind from ks_api_entry()
        struct { KsNode *expr; } unary;
        struct { KsNode *l, *r; } binary;
        struct { KsNode *cond, *then, *els; } ternary;
        struct { KsType to; KsNode *expr; int implicit; } cast;
        struct { KsNode *expr; } keyword;
        struct { KsType elem_type; KsNodeList items; } array;
        struct { const char **keys; KsNodeList values; } object;   // keys un-prefixed
        struct { int action_index; } actionref;

        struct { const char *name; KsType type; KsNode *init; int scope, slot; uint8_t modifier; } var;
        struct { KsNode *cond, *then, *els; } if_;   // els may be another KS_S_IF
        struct { KsNode *init, *cond, *step, *body; } loop;
        struct { const char *var_name; KsType var_type; int slot; KsNode *iterable, *body; } foreach;
        struct { KsNode *expr; } ret;
        struct { KsNode *target, *value; } addto;   // target may be a KS_E_CAST
        struct { KsNode *target; } remove;
        struct {
            KsNode *target;     // KS_E_IDENT, KS_E_MEMBER, KS_E_INDEX or KS_E_GAMEREF (PROP)
            KsNode *value;      // NULL for KS_S_INCDEC
            int has_cast;       // "(T) target op= value": static type of a dynamic l-value
            KsType cast;
            int prefix;         // KS_S_INCDEC: ++x (1) or x++ (0)
        } assign;
        struct { KsNode *expr; } expr;
        KsNodeList block;
    } u;
};

typedef struct {
    const char *name;
    KsType type;          // KS_T_ACTION for action parameters
    uint8_t modifier;
    int line, col;
} KsParam;

typedef struct {
    const char *name;
    int is_public;
    KsType ret_type;      // KS_T_VOID when the action has no return type
    KsParam *params;
    int param_count;      // params use local slots 0..param_count-1
    KsNode *body;         // KS_S_BLOCK
    int local_count;      // total local slots (params included); slots are never reused
    int hook_params;      // public hook on this side: its parameter count, else -1
    int line, col;
} KsAstAction;

typedef struct KsAstProgram {
    KsSide side;
    const char *source;   // copy of the (decoded) source, for spans
    size_t source_len;
    KsNode **globals;     // KS_S_VARDECL in source order; global slot = index
    int global_count;
    KsAstAction *actions; // source order; action index = position
    int action_count;
    struct KsArena *arena;
} KsAstProgram;

// Lex + parse + check. NULL on error (*err filled). Warnings as in ks_compile.
KsAstProgram *ks_front(const char *source, size_t len, KsSide side, KsDiag *err,
                       KsDiag *warnings, int max_warnings, int *warning_count);
void          ks_ast_free(KsAstProgram *);
// Index of a top-level action by name, -1 if none.
int           ks_ast_find_action(const KsAstProgram *, const char *name);

// Hard source limit (1.15 "Script too large.").
#define KS_MAX_SOURCE_BYTES (1024u * 1024u)

#ifdef __cplusplus
}
#endif

#endif
