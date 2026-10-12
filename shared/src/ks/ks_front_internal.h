// KrunkScript front end internals (WP1): arena, tokens, shared front state.
// Not part of the frozen API; only ks_lex.c, ks_parse.c and ks_check.c use it.
#ifndef KS_FRONT_INTERNAL_H
#define KS_FRONT_INTERNAL_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "ks.h"
#include "ks_ast.h"

// ---------------------------------------------------------------------------
// Arena: every AST allocation; freed at once by ks_ast_free / on error.
typedef struct KsArenaBlock {
    struct KsArenaBlock *next;
    size_t used, cap;
    // data follows (max_align_t aligned)
} KsArenaBlock;

typedef struct KsArena {
    KsArenaBlock *head;
    int oom;
} KsArena;

KsArena *ks_arena_new(void);
void     ks_arena_free(KsArena *);
void    *ks_arena_alloc(KsArena *, size_t size);          // zeroed; NULL on OOM (sets a->oom)
char    *ks_arena_strndup(KsArena *, const char *s, size_t n);

// ---------------------------------------------------------------------------
// Tokens
typedef enum {
    TK_EOF, TK_NUM, TK_STR, TK_IDENT,
    // keywords (order must match ks_keywords[] in ks_lex.c)
    TK_NUM_T, TK_STR_T, TK_BOOL_T, TK_OBJ_T, TK_ACTION, TK_PUBLIC, TK_RETURN, TK_IF, TK_ELSE,
    TK_FOR, TK_WHILE, TK_BREAK, TK_CONTINUE, TK_TRUE, TK_FALSE, TK_ADDTO, TK_REMOVE,
    TK_LENGTHOF, TK_NOTEMPTY, TK_HASPROP, TK_TONUM, TK_TOSTR, TK_STATIC, TK_DYNAMIC,
    // punctuators
    TK_LBRACE, TK_RBRACE, TK_LPAREN, TK_RPAREN, TK_LBRACKET, TK_RBRACKET,
    TK_SEMI, TK_COMMA, TK_DOT, TK_COLON, TK_QUESTION,
    TK_ASSIGN, TK_ADD_ASSIGN, TK_SUB_ASSIGN, TK_MUL_ASSIGN, TK_DIV_ASSIGN, TK_MOD_ASSIGN,
    TK_POW_ASSIGN, TK_SHL_ASSIGN, TK_SHR_ASSIGN, TK_USHR_ASSIGN, TK_AND_ASSIGN, TK_OR_ASSIGN,
    TK_XOR_ASSIGN,
    TK_INC, TK_DEC, TK_PLUS, TK_MINUS, TK_STAR, TK_SLASH, TK_PERCENT, TK_POW,
    TK_EQ, TK_NE, TK_LT, TK_LE, TK_GT, TK_GE, TK_SHL, TK_SHR, TK_USHR,
    TK_AMP, TK_PIPE, TK_CARET, TK_TILDE, TK_BANG, TK_ANDAND, TK_OROR,
    TK__COUNT
} KsTokKind;

#define TK_FIRST_KEYWORD TK_NUM_T
#define TK_LAST_KEYWORD  TK_DYNAMIC

typedef struct {
    uint8_t kind;
    int line, col;
    uint32_t start, end;   // byte span in the source
    double num;            // TK_NUM
    const char *text;      // TK_IDENT / keywords: name; TK_STR: decoded bytes
    size_t len;            // TK_STR: decoded length
} KsToken;

// ---------------------------------------------------------------------------
// Front state shared by the passes
typedef struct KsFront {
    KsArena *arena;
    const char *src;
    size_t len;
    KsSide side;
    // first error (the front end stops at it)
    int failed;
    KsDiag *err;
    KsDiag err_local;
    // warnings
    KsDiag *warnings;
    int max_warnings;
    int warning_count;
    // parser recursion depth (KS_MAX_PARSE_DEPTH)
    int depth;
    // tokens
    KsToken *toks;
    int ntok;
    int pos;
} KsFront;

// Recursion bounds (8 MiB stacks natively and on the web): parser nesting of
// statements/unary/primary, and expression tree depth in the checker (which also
// bounds long left-deep chains such as a + a + ... + a). Error text:
#define KS_MAX_PARSE_DEPTH 256
#define KS_MAX_EXPR_DEPTH 1024
#define KS_NESTING_MESSAGE "Script is too deeply nested."

void ks_front_error(KsFront *, int line, int col, const char *fmt, ...);
void ks_front_warning(KsFront *, int line, int col, const char *fmt, ...);

// Restricted names (1.2, KCF blacklist), predeclared GAME/UTILS/Math.
int  ks_is_restricted_name(const char *name);
int  ks_is_predeclared_name(const char *name);
const char *ks_token_kind_text(int kind);   // "(" / "num" / ...; NULL for value tokens

size_t ks_map_fullwidth_parens(char *buf, size_t n);   // in place, returns new length

int  ks_lex(KsFront *);                      // fills f->toks; 0 ok, -1 error
KsAstProgram *ks_parse_program(KsFront *);  // NULL on error
int  ks_check_program(KsFront *, KsAstProgram *); // 0 ok, -1 error

#endif
